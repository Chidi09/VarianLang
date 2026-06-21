# Per-package namespacing — `use "pkg" as ns`  (Constellation C4)

**Status: IMPLEMENTED.** This is the design and implementation for true per-package symbol
isolation — the one remaining gap in Constellation. Today `use "pkg"` loads a
package's top-level symbols into the **flat global namespace**; collisions are
*detected* (a package redefining an in-scope name is a hard error) but not
*avoided*. This plan makes them avoidable: a package's symbols live behind its
alias, so two packages can each define `Button` without conflict.

## Goal & non-goals

- **Goal:** `use "lumen-ui" as ui` then `ui.Button(...)`, with `Button`
  resolving to that package's symbol and never entering the global namespace.
- **Goal:** zero new VM opcode or value type if avoidable — reuse what exists.
- **Goal:** fully backward compatible. `use "pkg"` (no `as`) and `use "file.vn"`
  keep today's flat-global behavior (with the collision check already shipped).
- **Non-goal (v1):** namespacing **types**. `struct`/`enum` definitions are
  resolved at *parse time* into a global type registry; making `ui.Card` usable
  as a type annotation needs a scope-aware type resolver — a larger, separate
  effort. v1 namespaces **functions and values** (the common case); a package's
  exported types stay global and remain collision-checked. Documented honestly,
  not hidden.

## Grounding: what the runtime actually gives us

- **Globals are a flat array** (`vm->global_names[]`/`vm->globals[]`, linear
  lookup, `define_global`/`get_global` in `src/vm.c`). A top-level `fn foo`
  compiles to `define_global("foo", <fn>)`. This is exactly why packages collide
  today.
- **Closures already capture siblings.** A function nested in another function
  resolves names to the enclosing function's locals/upvalues, not globals.
- **Structs + `BC_MEMBER` already do `value.field`.** A struct whose fields are
  functions gives `obj.method` lookup for free.
- **`use` already parses a package in isolation** (`sub_parser` in
  `src/parser.c`) and hands back its statement list.

These four facts make one design fall out cleanly.

## Chosen mechanism — a module is a struct built by an init function

`use "lumen-ui" as ui` is compiled as if the author had written:

```varian
let ui = __module_init_lumen_ui()

fn __module_init_lumen_ui() {
    // —— the package's own top-level declarations, verbatim, but now NESTED ——
    fn _render(node) { ... }          // _-prefixed → module-private
    fn Button(props) { ... _render(props) ... }   // calls sibling: resolves locally
    fn Card(props) { ... }
    let version = "1.2.0"

    // —— auto-generated export struct: every non-_ top-level name ——
    return { Button: Button, Card: Card, version: version }
}
```

Then `ui.Button(props)` is ordinary `BC_MEMBER` + call. Why this works with no
new machinery:

- The package's functions become **nested functions** inside the init function,
  so a call from `Button` to `_render` resolves to a **local/upvalue**, not a
  global — true isolation, reusing the closure system.
- The package's public names are collected into a **struct** bound to the single
  global `ui`. Nothing else of the package touches the global namespace.
- `ui.Button` is the **member access** that already exists.

**Exports = top-level names not starting with `_`.** The `_` prefix becomes the
privacy rule: `_render` is visible to siblings (it's a local in the init fn) but
is *not* placed in the export struct, so `ui._render` doesn't exist. This matches
the stdlib's existing `_lumen_*` "internal" convention exactly — no new concept
for users to learn.

## Closure model: deferral is gated to module initializers

Mutual recursion among hoisted siblings needs the closure's captured cell to be
filled with the sibling's *final* value (the sibling isn't assigned yet when the
closure is created). That is a **deferred close**: the captured slot is snapshotted
at the init function's return. But applying that to *every* closure would break
ordinary value-capture semantics (a loop variable would read its final value,
not the per-iteration one) and would force an upvalue scan on every function
return.

So deferral is **gated to module-init functions only** (`ObjFunction.is_module_init`,
serialized in `.vnb`, baked into AOT codegen):

- In a module initializer, a captured local keeps an open slot and is snapshotted
  at the init frame's return (`close_upvalues` runs only there).
- In every other function, a captured local is closed **at creation** (by value).
  Loops capture per-iteration (`0,1,2`, not `3,3,3`), currying works, and
  `close_upvalues` never runs — no per-return overhead.

`close_upvalues` also matches the returning frame by its slot base
(`captured_owner`), so a frame can never corrupt the upvalues of an unrelated
closure. (Verified across `run` / `.vnb` / `--release` in
`tests/closure_capture_test.sh`/`tests/namespacing_test.sh`.)

## The one real compiler change: hoisting module locals

The catch: inside `__module_init_*`, the package's functions are locals declared
in source order, but they reference each other **mutually** (and out of order).
Top-level globals get away with this today because calls resolve by name at
runtime; locals are slot-based and declared in order, so a forward reference
(`Button` defined before `_render` but calling it — or two mutually-recursive
functions) would fail to resolve.

So the module-init body needs a **two-pass / hoisting** compile:

1. **Pass 1 — declare:** walk the package's top-level statements and pre-declare
   every `fn`/`let` name as a local slot in the init function *before* compiling
   any body.
2. **Pass 2 — define:** compile each declaration's body/initializer, assigning
   into its pre-declared slot. Now every sibling reference (forward or backward)
   resolves to an existing local slot.

This hoisting pass is the bulk of the implementation and the only genuinely new
compiler logic. It's well-trodden (it's how JS/Python module scopes and
letrec-style binding work). Everything else is AST synthesis.

## Implementation outline

1. **Lexer:** add `as` as a contextual keyword after `use "..."` (or reuse an
   existing `as` token if present — check before adding). Low risk.
2. **Parser (`use` handler, `src/parser.c`):**
   - Parse optional `as <ident>`.
   - Without `as`: today's path (splice statements into the parent program +
     collision check). Unchanged.
   - With `as`: build the AST for `fn __module_init_<mangled>() { <package
     stmts> ; return <export struct literal> }` plus `let <alias> =
     __module_init_<mangled>()`, where the export struct's fields are the
     non-`_` top-level names. Append those two nodes to the parent program
     instead of splicing the package's raw statements.
   - The export-name list is gathered by scanning the package AST's top-level
     `fn`/`let`/`struct` declarations (the parser already tracks these).
3. **Compiler (`src/compiler`/`vm.c` codegen):** implement the hoist-then-define
   pass for a function flagged as a module initializer (a bit on the fn-decl
   node, or a dedicated synthesized node type). This is the core work.
4. **Dedup & idempotency:** reuse the existing `use` dedup set so `use "x" as a`
   and `use "x" as b` build the module once and bind both aliases (or define the
   policy: each alias gets its own instance — simpler, and each is cheap).
5. **Types (v1 limitation):** a package's `struct`/`enum` still registers in the
   global type registry and is collision-checked; it is *not* reached via the
   alias. Emit a clear note if a package exports types under `as`.

## Why not the alternatives

- **Name mangling** (compile `ui.Button` → a global `ui$Button`): still needs
  full scope resolution to rewrite internal references, *and* it special-cases
  member syntax into a global lookup — more semantic surface, same hard part
  (scoping), worse ergonomics. Rejected.
- **A first-class `Namespace` value + `BC_GET_NAMESPACED` opcode + per-module
  symbol table:** the "textbook" answer, but it adds a value type, opcodes, and
  GC integration for no benefit over the struct approach, which the language
  already supports natively. Rejected as over-engineering — the same call made
  for Lumen's M9 and the registry's signing milestone.

## Syntax surface (final)

```varian
use "lumen-ui"                 // flat global (stdlib-style); collision-checked
use "lumen-ui" as ui           // namespaced: ui.Button(...), ui.Card(...)
use "lib/local.vn"             // direct-file include (flat); unchanged
// future sugar (not v1):
// use "lumen-ui" { Button, Card }   // selective import into current scope
```

Hyphenated package names (`lumen-ui`) keep the **string** target; the alias is a
normal identifier (`ui`), sidestepping the "hyphen isn't an identifier" problem
that made `use "..."` a string in the first place.

## Milestones

- **N1 — `as` parse + module synthesis.** Parse `use "x" as a`; synthesize the
  init-fn + export-struct + binding AST. Flat `use` untouched. (No isolation yet
  if hoisting isn't done — gate N1 behind N2.)
- **N2 — module-local hoisting (the core).** Two-pass compile of the init fn so
  mutually-referencing package functions resolve as locals. After N2, isolation
  is real and `tests/` can assert two packages with a same-named `Button`
  coexist.
- **N3 — `_` privacy + export filtering.** Exclude `_`-prefixed names from the
  export struct; verify siblings still see them.
- **N4 — ergonomics & docs.** `use "x" { a, b }` selective sugar (optional),
  update `CONSTELLATION.md`, and a `tests/namespacing_test.sh` covering: two
  packages, same symbol name, both via aliases; private `_` helper not on the
  alias; alias member call across `run`/`.vnb`/`--release`.

## Open questions (decide when implementing)

- **Per-alias instance vs shared module value.** Building once and sharing is
  cheaper but means top-level package side effects run once; per-alias is
  simpler to reason about. Lean shared, keyed by package identity.
- **Re-exporting / transitive `use` inside a namespaced package.** Does a
  package's own `use` leak into its consumer's alias? Default: no — a package's
  imports are its own; only its declared exports appear on the alias.
- **Type namespacing.** When v2 tackles types, does `ui.Card` become valid in
  type position? That needs the type resolver to consult alias scopes — design
  it then, don't pre-build.
- **Interaction with AOT/`.vnb`.** None expected — this is all front-end (parse +
  compile to existing opcodes); the artifact backends are unaffected, the same
  way `use` already compiles into bundles and native binaries today.
```
