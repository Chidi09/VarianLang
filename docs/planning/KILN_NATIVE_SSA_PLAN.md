# Kiln Native SSA Pipeline

Typed SSA, escape analysis, and an unboxed native backend for Varian's release
(AOT) compiler. Release path only — the interpreter and its bytecode compiler
are untouched.

## Why

Before this work, `vn build --release` was not a native compiler. `src/aot.c`
transliterated bytecode 1:1 into C: every arithmetic op, struct creation and
field access still went through the same boxed `Value` (tagged union) helpers
the interpreter uses — `aot_op_add` is byte-for-byte the interpreter's
`L_BC_ADD`. The only genuinely native things it did were C `goto`s for jumps
and direct `frame->slots[i]` indexing.

There was no type inference anywhere in the codebase (Varian parses type
annotations but only uses them for FFI marshaling and one shallow lint), and no
static escape analysis. `ZENITH_SPEED_PHASE2_PLAN.md` had already flagged that
devirtualizing AOT dispatch "needs type inference AOT doesn't have."

## Governing safety property

Every pass here is built so that **a bug can only cost an optimization, never
produce a wrong answer or corrupt memory.** Concretely:

- Type inference has no "concrete by default" rule. Every concrete type requires
  a specific rule to fire at the fixpoint; failure settles at `UNKNOWN`.
- Escape analysis starts everything escaping-capable and only proves the
  negative; anything unrecognised stays heap-allocated.
- Native codegen has an explicit whitelist. Any construct it does not recognise
  makes the whole function ineligible and it keeps the existing boxed body.
- Every natively-compiled function emits a permanent runtime type guard per
  parameter and falls back to the boxed body on mismatch.

## Stages

| Stage | What | Gate | State |
|---|---|---|---|
| A.1 | Suspend analysis — which functions can never reach `await`/channel/actor dispatch | Conservative on unresolved callees; nested chains propagate | Done |
| A.2 | Typed SSA IR built from the AST (Braun et al. CC'13) | No unsealed blocks across the 142-file corpus | Done |
| A.3 | Type inference over a 3-level lattice | Recursive fib and loop accumulators resolve to `int`; no leaked `PENDING` | Done |
| B | Intraprocedural escape analysis | Global/return/call/closure-capture all force HEAP | Done |
| C | Native unboxed codegen, native-to-native ABI, shadow-mode verification | `tests/ssa_differential_test.sh`: differential + negative control + depth guard | Done (scalar subset) |
| D | Benchmark vs. boxed AOT, Go and Rust | Measured; see below | Done |

### A.1 — Suspend analysis

`src/suspend_analysis.c`. Monotonic fixpoint over the AST, same shape as
`release_prune_unused_functions`. A function is marked `SUSPENDS` on any
`await`, channel send/receive, actor call, call to a known suspending function,
or **any call that cannot be statically resolved to one known function**. FFI
calls do not suspend (synchronous C, cannot re-enter the scheduler).

Result is cached on `ObjFunction.suspends_maybe`, defaulting to `true` — an
unanalyzed function is always assumed to suspend.

This exists because `task_run` can yield a task at any bytecode offset and
resume by re-entering the AOT body's `switch (pc)` table. That only works
because all live state at a suspension point is boxed `Value`s on `t->stack[]`.

### A.2 — Typed SSA IR

`include/ssa.h`, `src/ssa.c`. Built directly from the AST (richer than bytecode:
literal types, field names before shape-hashing, source correlation) for
non-suspending functions only.

Construction follows Braun/Buchwald/Hack et al., "Simple and Efficient
Construction of SSA Form" (CC'13) — incomplete-phi placeholders resolved at
block sealing, no dominance-frontier pass. Trivial phis are eliminated by
union-find-style forwarding (`SsaValue.replaced_by`) rather than a
replace-all-uses-with rewrite.

Two real bugs were found and fixed here:

- **Phi placement.** Lazily-created phis were tail-appended, landing *after* a
  block's already-emitted terminator, breaking the "last instruction is a
  terminator" invariant. Fixed with `ssa_prepend_phi`.
- **`&&`/`||` were lowered eagerly.** Varian short-circuits (Lua/Python style —
  the result is whichever operand survives, not necessarily a bool), and vm.c
  carries an explicit comment that eager evaluation crashes idioms like
  `x == null or x.len() == 0`. Replaced with real branch/merge CFG
  construction. `??` is eager and was correct as-is.

### A.3 — Type inference

3-level lattice: `SSA_TYPE_PENDING` (optimistic top) > concrete > `SSA_UNKNOWN`
(absorbing bottom). Values only ever move downward.

The first implementation used a naive 2-level lattice (start at `UNKNOWN`, grow
only from already-resolved operands) and **could not type the milestone's own
flagship cases** — recursive fibonacci's return type or a `while`-loop
accumulator. Both are self-referential value cycles, and `UNKNOWN` is itself a
self-consistent fixpoint for such a cycle. The optimistic-top state is what
bootstraps them; this is the standard SCCP-style technique.

Promotion rules mirror vm.c's `BINARY_OP_NUM` / `L_BC_ADD` exactly rather than a
reinvented notion of "numeric". Notable quirks encoded faithfully:

- Ordering comparisons (`<`, `>`, `<=`, `>=`) yield **int/float, not bool** —
  they reuse `BINARY_OP_NUM` and push `val_int`/`val_float`. Only `==`/`!=`
  return a real bool.
- `+` auto-stringifies if either operand is a proven string.
- `%` reads both operands as raw `int64` with no type check (a pre-existing
  latent vm.c bug), so `INT` is only sound when both operands are proven `INT`.

Also discovered: plain `struct` declarations parse and **discard** field type
annotations (`parse_struct_decl` calls `parse_type()` and throws the result
away); only `schema` retains them, and `schema` is deliberately not lexed as a
keyword. So field-get typing can only ever resolve through a schema — an AST
limitation, not an inference shortcut.

### B — Escape analysis

`src/ssa_escape.c`. Whole-function monotonic fixpoint tracked **per value**, not
per allocation site, which is what makes the transitive rules fall out for free.
An allocation escapes if it is stored to a global/upvalue, stored into an
already-escaping container, returned, or passed as an operand to **any** call —
direct, dynamic, or dispatch, unconditionally.

That last rule is the most conservative in the pass by design: misclassifying an
escaping value as safe is a stack-use-after-scope memory bug, strictly worse
than a mistyped arithmetic result.

One safety gap was caught by self-review before the phase was declared done: SSA
construction gave each nested closure a fully independent scope, so a struct
captured by a closure had no link back to the enclosing function's SSA values
and would have been wrongly classified non-escaping. Fixed by `ssa_scan_captures`,
which walks a nested closure's whole body and emits an `UNKNOWN_EFFECT` marker
per captured identifier.

### C — Native codegen

`include/aot_native.h`, `src/aot_native.c`.

**Varian's ordinary calling convention is a *trampoline*, not a C call.**
`BC_CALL` pushes a `CallFrame` and returns to `task_run`, which runs the callee
and then re-enters the caller's AOT body from the top via its `switch (pc)`
table. Re-entry from the top means every raw C local is gone. This is the same
hazard A.1 handles for `await`, but it applies to **ordinary calls too** — so
devirtualizing `CALL_DIRECT` is not optional polish, it is the prerequisite for
a natively-compiled function to call anything at all.

Each eligible function is therefore emitted as a plain C function

```c
static <ret> varian_nat_fn_N(VM *vm, int __depth, <typed params...>);
```

with the SSA body inline. Arguments and results travel in registers; no
`CallFrame`, no task stack. `CALL_DIRECT` between two eligible functions is a
direct C call, so recursion and helper calls run at native speed.

Three things the trampoline used to provide are replaced explicitly:

- **GC rooting** — not needed, and not merely assumed: the subset only admits
  INT/FLOAT/BOOL, so nothing a native body holds is heap-allocated at all.
- **Stack-overflow detection** — `frame_count >= TASK_FRAMES_MAX` cannot see
  C-level recursion, so `__depth` is passed by value, seeded from
  `t->frame_count` at the boundary (native code shares the existing budget
  rather than getting a second one) and checked on entry. A parameter rather
  than a VM field: it needs no unwind bookkeeping along error paths and adds no
  global mutable state to the hot path.
- **Error propagation** — a raw scalar return has no error channel, so a failing
  native function calls `runtime_error` and returns 0, and every call site
  re-checks `vm->had_error` immediately.

Eligibility (all verified structurally):

- proven non-suspending;
- every live SSA value typed `INT`/`FLOAT`/`BOOL` — so nothing it computes is
  heap-allocated or GC-visible, which is why GC rooting is a non-issue rather
  than an assumption;
- every instruction in the arithmetic/compare/control-flow whitelist — no calls,
  allocations, field/index access, global or upvalue traffic;
- well-formed CFG: every reachable block terminated, no duplicate predecessor
  edges, phi arity matching pred arity, `PARAM`s confined to the entry block
  with in-range indices.

Because "my callee must also be eligible" is mutually recursive — that is the
whole point, recursion is the feature — eligibility is a monotonic demotion
fixpoint: all candidates start eligible and are demoted until nothing eligible
calls anything ineligible. Demotion only removes, so it terminates, and the
surviving set is verified at the fixpoint rather than assumed mid-flight.

Argument types must match the callee's parameters **exactly** — no int->float
widening at a call boundary. Widening would be a real divergence: the boxed
path's entry guard rejects a `VAL_INT` argument to a float parameter and runs
the boxed body (integer semantics), while a widening native call would compute
float semantics.

Each eligible function emits **two bodies**: the native one under the ordinary
`varian_aot_fn_N` name, and the unmodified existing transpile as
`varian_aot_fn_N_boxed`. The native entry guards each parameter's runtime tag
and falls through to `_boxed` on mismatch. **These guards are permanent.** A
declared annotation is only a belief — `parser.c` gives every *unannotated*
parameter a synthetic default of `int`, and a dynamic-language caller can pass
anything regardless.

Phis are materialized as parallel copies on predecessor edges, staged through
temporaries so a swap across a back edge cannot clobber itself.

### Shadow mode

`vn build --release --ssa-shadow` compiles the generated C with
`-DVARIAN_SSA_SHADOW_MODE`. Every native body then runs the boxed reference body
first over a snapshot of task state, restores it, runs natively for real, and
`abort()`s on any divergence in the returned `Value`. Double execution is sound
because the eligible subset is pure scalar arithmetic with no side effects
beyond the task stack.

Argument slots are saved and restored **by value**, not just the stack pointer:
a function that assigns to its own parameter (`n = n - 1` in a loop) mutates
`frame->slots[]` in place, and the reference run would otherwise hand the native
run already-consumed arguments. This was caught by the harness firing on
`countdown` — a harness bug, not a codegen bug, but exactly the kind the harness
exists to surface.

A boxed body containing a call does *not* run to completion — it pushes a frame
and returns to the scheduler — so the harness drives the trampoline itself until
the frame it started from has returned, holding `VM.ssa_shadow_ref` for the
duration so nested wrappers go straight to their boxed bodies. That keeps the
reference run genuinely all-boxed and its cost linear rather than exponential in
call depth.

`tests/ssa_differential_test.sh` is the gate. It builds one fixture plain and
under shadow mode, requires both to match expected output, requires that
something was actually specialized (so the gate cannot pass vacuously), and adds
two more checks:

- a **negative control** — it corrupts one native body in the generated C and
  requires shadow mode to abort. Without it, a harness that never fires looks
  identical to one that always agrees.
- a **depth-guard check** — unbounded native recursion must report `Stack
  overflow` rather than SIGSEGV, and the error trace must be a couple of frames
  deep, proving the native guard fired rather than the fixture quietly falling
  back to the boxed trampoline. The fixture needs an unreachable base case to be
  natively compiled at all (without one its return type is purely
  self-referential and settles at `UNKNOWN`), and a non-tail `+ 1` so gcc cannot
  turn the recursion into a loop and hide what is being tested. Both were found
  by the check failing.

## D — Measured results

All numbers are best-of-7 on this VPS, `--release`.

### Against the boxed AOT path

Baseline is the *same generated C* with each native entry point patched to
delegate to its boxed body — apples-to-apples, not interpreter-vs-compiled.

| Kernel | Boxed AOT | Native AOT | Speedup |
|---|---|---|---|
| `total = total + i * 3 - (i / 2)` (vectorizable) | 2.28 s | 0.027 s | ~85x |
| `h = h * 31 + i; h = h ^ (h >> 7)` (serial dependency) | 2.27 s | 0.053 s | ~43x |
| recursive `fib(30)` | 0.38 s | 0.02 s | ~19x |

The first row is inflated: unboxing lets gcc auto-vectorize, so it measures both
the removal of boxing *and* newly-unlocked ordinary C optimization. The second
row is the honest per-op figure — ~114 ns/iter boxed vs ~2.6 ns/iter native.

### Against Go and Rust

Same algorithms, hand-written idiomatically in each language. Go 1.22.2 (`go
build`), Rust 1.96.0 (`rustc -O`, with `black_box` on inputs so nothing is
const-folded). All three produce identical output on both benchmarks, which is
the check that they are actually doing the same work.

| Benchmark | Varian | Go | Rust |
|---|---|---|---|
| `fib(35)` — call-heavy | **0.12 s** | 0.08 s | 0.05 s |
| 200M-iteration hash loop — arithmetic-heavy | **0.39 s** | 0.37 s | 0.42 s |

On straight-line arithmetic Varian is level with Go and marginally ahead of Rust
(the Rust version uses `wrapping_*` ops and `black_box`, which cost it a little;
this is not evidence that Varian is "faster than Rust" in general).

On call-heavy code Varian is ~1.5x Go and ~2.4x Rust, and the reason is
identified rather than guessed: the `vm->had_error` check after every native
call. It is a load through the VM pointer that the callee may have written, so
it cannot be cached in a register across the call. Removing it takes fib(35)
from 0.12 s to 0.05 s — i.e. it accounts for essentially the entire gap, and
without it Varian would be at parity with Rust here. `fib` is close to a worst
case for this, since the body is trivial and the check dominates; real code does
more work per call.

That check is not removable in general (continuing past an error with garbage
values can turn a clean error into a hang), but it *is* avoidable for calls that
provably cannot fail — see the deferred list.

### Caveats

These are microbenchmarks of exactly the subset this backend targets, on a
box noisier than the machine used for published numbers; treat them as deltas
here, not replacements for published figures. **Do not extrapolate to web
serving**: prior work found request handling is dominated by dispatch and I/O,
not arithmetic, and the earlier AOT-dispatch inline-cache attempt showed no
measurable benefit there.

Coverage on the 142-file test/example corpus: 138 files have at least one
natively-compiled function. The 4 that compile nothing are deliberate error
fixtures that fail semantic analysis first.

## Deferred to v2

- **Eliding the per-call `vm->had_error` check**, which the cross-language
  benchmark identifies as essentially the whole remaining gap against Go and
  Rust on call-heavy code. A callee can only fail via div/mod/shift-by-zero, its
  depth guard, or a call to something that can fail; and the depth guard itself
  is unnecessary for any function outside a call cycle, since an acyclic call
  graph has compile-time-bounded depth. Propagating a "can fail" bit and eliding
  both checks is the clear next optimization.
- **Stack allocation of proven-non-escaping structs.** Phase B already computes
  `proven_non_escaping` and `scope_size_hint` (sizing mirrors `new_struct`'s
  exact formula so native-stack and heap layouts agree); the codegen half is not
  written. Deliberately sequenced after the scalar subset proved the
  guard/shadow framework, since a mistake here is a memory-safety bug rather
  than a wrong number.
- Interprocedural "caller keeps it local" escape analysis.
- FFI-signature-informed type inference.
- Devirtualizing `CALL_DYNAMIC`/`DISPATCH` — explicitly *not* attempted; the
  prior inline-cache experiment showed no measurable benefit.

## Files

| File | Status |
|---|---|
| `include/suspend_analysis.h`, `src/suspend_analysis.c` | new — A.1 |
| `include/ssa.h`, `src/ssa.c` | new — A.2/A.3 |
| `src/ssa_escape.c` | new — B |
| `include/aot_native.h`, `src/aot_native.c` | new — C |
| `include/vm.h` | `ObjFunction.suspends_maybe`, `ObjFunction.source_node` (additive fields only) |
| `src/vm.c` | populates those two fields; `compiler_add_local`/`compiler_find_local` un-`static`'d |
| `src/aot.c` | wires the four passes into `aot_compile`; per-function native-vs-boxed branch |
| `src/main.c` | `--dump-suspend`, `--dump-ssa`, `--ssa-shadow` flags |
| `tests/suspend_analysis_test.sh`, `tests/ssa_dump_test.sh`, `tests/ssa_type_inference_test.sh`, `tests/ssa_escape_analysis_test.sh`, `tests/ssa_differential_test.sh` | new |

`src/vm.c` changes are confined to populating two additive `ObjFunction` fields
and removing `static` from two accessors — the bytecode compiler's and
interpreter's behavior is unchanged.
