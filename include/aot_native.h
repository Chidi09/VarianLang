#ifndef AOT_NATIVE_H
#define AOT_NATIVE_H

#include "ssa.h"
#include "vm.h"
#include <stdio.h>
#include <stdbool.h>

/* Kiln's native (unboxed) release backend — Phase C.
 *
 * Consumes the typed, escape-analyzed SSA built by ssa.c/ssa_escape.c and
 * emits, for a deliberately narrow subset of functions, real C functions that
 * take and return raw `int64_t`/`double`/`bool` instead of boxed `Value`s on
 * the task stack. Everything outside that subset keeps today's
 * per-bytecode-instruction transpile in aot.c, byte-for-byte unchanged.
 *
 * ─── Why a second calling convention was necessary ───
 *
 * Varian's ordinary calling convention is a *trampoline*, not a C call:
 * BC_CALL pushes a new CallFrame and RETURNS to task_run, which then runs the
 * callee and re-enters the caller's AOT body from the top, dispatching through
 * its `switch (pc)` resumption table (see aot.c's emitted bodies and
 * task_run's `frame->function->aot_func(vm, t); continue;` loop in vm.c).
 * Re-entry from the top means every raw C local is gone.
 *
 * That is the same hazard suspend_analysis.h describes for await/channel ops,
 * but it applies to ORDINARY CALLS TOO. So a body holding live state in raw C
 * locals cannot contain a trampolined call at all — which is why devirtualizing
 * CALL_DIRECT is not optional polish here, it is the prerequisite for any
 * natively-compiled function to call anything.
 *
 * ─── The native ABI ───
 *
 * Each eligible function is emitted as a plain C function
 *
 *     static <ret> varian_nat_fn_N(VM *vm, <typed params...>);
 *
 * with the SSA body inline. Arguments and the result travel in C registers;
 * no CallFrame is pushed, no task stack is touched. CALL_DIRECT between two
 * eligible functions becomes a direct C call, so recursion and helper calls
 * work at native speed.
 *
 * Three things the trampoline used to provide have to be replaced explicitly:
 *
 *   - GC rooting. Not needed, and not merely assumed: the eligible subset is
 *     restricted to INT/FLOAT/BOOL values, so nothing a native body holds is
 *     heap-allocated or reachable by the collector in the first place.
 *   - Stack-overflow detection. The interpreter's `frame_count >=
 *     TASK_FRAMES_MAX` check cannot see C-level recursion, so every native
 *     function takes a hidden `int __depth` parameter, seeded from
 *     t->frame_count at each boxed->native boundary (so native code shares the
 *     existing budget rather than getting a second one), incremented at each
 *     call and checked on entry. A by-value parameter rather than a counter on
 *     the VM deliberately: it lives in a register, needs no unwind bookkeeping
 *     (nothing to decrement along each error path), and introduces no global
 *     mutable state on the hottest path in the compiler.
 *   - Error propagation. A raw scalar return has no room for an error signal,
 *     so a failing native function calls runtime_error (setting vm->had_error,
 *     exactly as the boxed helpers do) and returns 0; every call site re-checks
 *     vm->had_error immediately and unwinds. That mirrors what task_run's
 *     `while (!vm->had_error ...)` loop does for the boxed path.
 *
 *     This check is the measured cost of the ABI: it is a load through the VM
 *     pointer that the callee may have written, so it cannot be cached in a
 *     register across a call. On recursive fib(35) — close to a worst case,
 *     since the body is trivial and the check dominates — removing it takes
 *     0.14s to 0.05s. It is not removable in general (continuing past an error
 *     with garbage values can turn a clean error into a hang), but it IS
 *     avoidable for calls that provably cannot fail: a callee can only fail via
 *     div/mod/shift-by-zero, its depth guard, or a call to something that can
 *     fail, and the depth guard itself is unnecessary for any function outside
 *     a call cycle (an acyclic call graph has compile-time-bounded depth).
 *     Propagating a "can fail" bit and eliding both is the obvious next
 *     optimization; not implemented in v1.
 *
 * ─── The v1 subset ───
 *
 * A function is eligible only if ALL of the following hold (each verified in
 * the eligibility pass, never inferred):
 *   - suspend_analyze proved it never suspends (ObjFunction.suspends_maybe);
 *   - every live SSA value is typed INT, FLOAT or BOOL — no UNKNOWN, STRING,
 *     STRUCT, array, tuple or closure anywhere;
 *   - every instruction is in the arithmetic/compare/control-flow whitelist,
 *     plus CALL_DIRECT to another eligible function with exactly matching
 *     argument types — no dynamic calls, no dispatch, no allocations, no
 *     field/index access, no global or upvalue traffic, no opaque effects;
 *   - every return yields the same scalar type;
 *   - control flow is well-formed for direct C lowering (every reachable block
 *     terminated, no duplicate predecessor edges, phi arity matching pred
 *     arity, PARAMs confined to the entry block with in-range indices).
 *
 * Because "callee must also be eligible" is mutually recursive, eligibility is
 * a monotonic fixpoint: all candidates start eligible and are demoted until no
 * eligible function calls an ineligible one. Demotion only ever removes, so it
 * terminates, and the answer is checked at the fixpoint rather than assumed
 * mid-flight.
 *
 * Argument types must match the callee's parameter types EXACTLY — no
 * int->float widening at a call boundary. Widening would be a real divergence:
 * the boxed path's entry guard would reject a VAL_INT argument to a float
 * parameter and run the boxed body (integer semantics), while a widening
 * native call would compute float semantics.
 *
 * ─── The guards ───
 *
 * At the boxed->native boundary, a declared parameter type is only ever a
 * BELIEF: parser.c gives every unannotated parameter a synthetic default of
 * `int`, and a dynamic-language caller can pass anything regardless. So the
 * wrapper `varian_aot_fn_N` emits a runtime tag check per typed parameter and
 * falls through to `varian_aot_fn_N_boxed` — the unmodified existing transpile
 * — on any mismatch. These guards are permanent, never elided once "confidence
 * is established": they are what makes a wrong inference a slow path instead of
 * a wrong answer. Native-to-native calls need no such guard, because the
 * argument's type is proven by the caller's own SSA typing rather than
 * declared.
 *
 * ─── VARIAN_SSA_SHADOW_MODE ───
 *
 * Built with -DVARIAN_SSA_SHADOW_MODE, every wrapper runs the boxed reference
 * body first on a snapshot of task state, restores it, runs native for real,
 * and abort()s on divergence in the returned Value. Because a boxed body
 * containing a call returns to the scheduler mid-way rather than completing,
 * the harness drives the trampoline to completion itself, and sets
 * VM.ssa_shadow_ref for the duration so nested wrappers go straight to their
 * boxed bodies — that keeps the reference run genuinely all-boxed and keeps its
 * cost linear instead of exponential in call depth. Verification builds only. */

/* Which functions get native bodies, and what each one's C signature is.
 * Produced once per output file; indices line up with aot.c's `funcs[]`. */
typedef struct {
    int fn_count;
    SsaFunction **ssa;     /* [fn_count]; NULL where not natively compiled */
    SsaType *return_type;  /* [fn_count]; valid where ssa[i] != NULL */
    int count;             /* how many entries are non-NULL */
} AotNativePlan;

/* Runs the eligibility fixpoint over the whole module. Conservative in every
 * branch: any construct not explicitly recognised makes a function ineligible,
 * so the worst a bug here can do is lose a specialization. */
AotNativePlan *aot_native_plan(SsaModule *mod, ObjFunction **funcs, int fn_count);
void aot_native_plan_free(AotNativePlan *plan);

/* Emits shared static helpers plus every native function's prototype (needed
 * up front for mutual recursion). Call once, after the boxed helpers. */
void aot_native_emit_prelude(FILE *out, AotNativePlan *plan, ObjFunction **funcs);

/* Emits `static <ret> varian_nat_fn_<i>(...)` and the guarded boxed-boundary
 * wrapper `void varian_aot_fn_<i>(VM*, Task*)`. The caller must already have
 * emitted the boxed body as `static void varian_aot_fn_<i>_boxed(VM*, Task*)`. */
void aot_native_emit_function(FILE *out, AotNativePlan *plan, ObjFunction **funcs, int index);

#endif
