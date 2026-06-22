# Lumen DX plan — five units (for a lightweight implementer)

## How to use this document

You are implementing five **independent** units. Do them **one at a time, in the
order given**. After each unit, run that unit's **Verify** block and confirm it
passes before starting the next. Do not start a later unit if an earlier one's
Verify fails — stop and report the exact output instead.

**Global rules — read once, obey for every unit:**

1. **Do not redesign.** Implement exactly what each unit says. The signatures,
   names, and file locations are chosen deliberately. If something looks
   redundant, it isn't — leave it.
2. **Almost everything here is pure Varian** edited in `vn_modules/lumen.vn`.
   Only Unit 5 (optional) touches C. If a unit says "no C changes," do not edit
   any `.c`/`.h` file for it.
3. **String building must use the builder pattern**, never `out = out + x` in a
   loop over input length. That is: `let parts = []` … `parts.append(chunk)` …
   `return parts.join("")`. `append` and `join` are native O(1)/O(n) methods that
   already exist. Using `out = out + x` in a hot loop reintroduces an O(n²) bug
   that previously OOM-killed the machine. (Small fixed concatenations like
   `"<div>" + x + "</div>"` are fine.)
4. **Never write a `for`/`while` loop whose body can `continue` without the loop
   variable advancing first.** (A compiler bug here was just fixed; don't rely on
   edge behavior — always advance the index before any `continue`.)
5. **Varian strings interpolate `{expr}`.** Inside a normal `"..."` string,
   `{` and `}` are special. When you must emit a literal brace into generated
   code or test data, escape it as `\{` / `\}`. This is why test files that
   contain `{ ... }` in a string can mis-parse — keep test strings brace-free or
   escape them.
6. After each unit, run the **full suite**: `./vn test tests/` must stay at
   **all-pass** (currently 82 passed, 0 failed). A new failure means you broke
   something — fix or revert before continuing.
7. Memory safety when testing renders: a runaway loop can exhaust RAM. Run any
   ad-hoc test under a memory cap and timeout:
   `( ulimit -v 9000000; timeout 20 ./vn /tmp/yourtest.vn )`. The `vn` binary
   needs ~6 GB of *virtual* space just to start, so never set `ulimit -v` below
   ~8000000.

**Key files and helpers you will touch or call (all in `vn_modules/lumen.vn`):**

- `lumen_compile_source(name, lumen_src)` — turns one `.lumen` file's text into a
  generated Varian `_lumen_init_component_<name>()` + registration string.
- `_lumen_build_dir(pages_dir, out_path, port)` — scans a pages directory,
  compiles every `.lumen`, emits one runnable program. **Currently flat** (no
  subdirectories). Unit 2 makes it recursive.
- `_lumen_pascal(base)` — `"book-core"` → `"BookCore"`.
- `_lumen_route(base)` — `"index"` → `"/"`, else `"/" + base`.
- `_lumen_parse_attrs(attr_str)` — parses `key="value"` pairs from a component
  tag's attribute string into a struct. Unit 1 extends it.
- `_lumen_process_composition(html, state)` — expands `<ChildComponent ...>` tags.
- `_lumen_live_loop(ws, component)` — the per-connection event loop; re-renders
  the component after every event. Units 3–4 rely on this re-render.
- `lumen_component(state_fn, render_fn, handler_names, handler_fns)` — builds a
  component struct.
- `http.create_struct(keys_array, values_array)` — builds a struct at runtime
  (use this, NOT `Struct { ... }` literals, to avoid prelude parse-order issues).
- `_validate.get_field(struct, key)` — reads a struct field by string name,
  returns `null` if absent. `_validate.get_keys(struct)` — array of field names.
- Validation lib (in `vn_modules/validate.vn`, available as globals `validate`
  and `sanitize`): `validate.str()`, `validate.num()`, `validate.object(fields)`;
  each returns a validator with `.parse(v)` → a struct with `.success` (bool) and
  either `.data` or `.error`.

---

## Unit 1 — JSX-style prop binding: `<Child prop={ expr }>`  (issue #4)

### Problem (what the user hit)

In a `.lumen` template, text interpolation is **double-brace**: `{{ title }}`.
But when passing a value into a child component as an attribute, authors
naturally write JSX/Svelte style `<CodeEditor title={ title }>` (single brace).
Today that single-brace form is **not** interpolated — it ends up as the literal
text `{ title }` — so the prop silently doesn't bind. The user worked around it
by writing `title="{{ title }}"`. We will make `prop={ expr }` work as a clean
alias for `prop="{{ expr }}"`.

### Why this exact approach

Text interpolation stays `{{ }}` (Handlebars-style, unchanged — do NOT touch the
`{{ }}` engine). We only add single-brace support **in component-tag attribute
position**, by rewriting `prop={ expr }` to `prop="{{ expr }}"` **before** the
existing pipeline runs. Because `lumen_render` already does its `{{ }}` pass
*before* `_lumen_process_composition`, a rewritten `prop="{{ expr }}"` is resolved
against the parent context exactly like the working workaround. So this is a
pure front-end textual rewrite — no new evaluation logic.

### Steps (pure Varian, `vn_modules/lumen.vn`)

1. Add a new helper function `_lumen_rewrite_prop_braces(html)` near
   `_lumen_parse_attrs`. It scans `html` and, **only inside the opening tag of a
   component** (a `<` immediately followed by an uppercase A–Z letter, up to the
   matching `>`), replaces every attribute of the form `key={ ...expr... }` with
   `key="{{ ...expr... }}"`. Outside component tags, and inside normal lowercase
   HTML tags, change nothing.

   Implementation requirements:
   - Build the output with the builder pattern (`let out = []` … `out.join("")`).
   - Walk with an index `i`. Find the next `<` with `html.index_of("<", i)`.
     Emit everything before it. If the char after `<` is uppercase A–Z
     (`code_at` in 65..90), you are in a component tag: find its end `>` with
     `html.index_of(">", tagStart)`, take that tag substring, run the
     attribute-rewrite on just that substring, emit it, and set `i` to after `>`.
     Otherwise emit `<` and advance `i` by 1.
   - The attribute-rewrite on a single tag substring: scan for `={`; when found,
     the attribute value is everything from after `{` up to the next `}`.
     Replace the `{ ... }` (including the surrounding braces) with
     `"{{ ... }}"` (add the double-quotes and double-braces). Keep scanning after
     the inserted close-quote. Trim leading/trailing spaces inside the braces
     with `_sanitize.trim` before re-wrapping.
   - **Guard against the equals/quote case:** only rewrite when the character
     right after `=` is `{`. If it is `"` or `'`, leave it (that's a normal
     quoted attribute). Always advance your scan index past each handled
     position so you cannot loop forever.

2. Call the new helper **first** inside `lumen_render`, before the `{{ }}` region
   pass. Find this function:

   ```
   fn lumen_render(template, ctx) {
       let res = _lumen_render_region(template, 0, template.len(), ctx, null)
       ...
   }
   ```

   Change the first line of the body to rewrite the template first:

   ```
   fn lumen_render(template, ctx) {
       let tpl2 = _lumen_rewrite_prop_braces(template)
       let res = _lumen_render_region(tpl2, 0, tpl2.len(), ctx, null)
       let raw_html = _lumen_bind_events(_validate.get_field(res, "out"))
       return _lumen_process_composition(raw_html, ctx)
   }
   ```

   (Only the first two lines change: introduce `tpl2`, and pass `tpl2` to
   `_lumen_render_region`. Leave the rest of the function exactly as is.)

3. Do **not** modify `_lumen_escape_string`, the `{{ }}` engine, or
   `_lumen_parse_attrs`. This unit is additive.

### Verify (Unit 1)

Create `/tmp/u1.vn` (note: the literal braces below are inside the Varian source,
so build the template string with `from_codes` or read it from a file to avoid
Varian eating the braces — easiest is to write the template to a file first).
Simplest robust check — add a test file `tests/lumen_prop_brace_test.vn`:

```
test "single-brace prop binds like {{ }}" {
    // Build the template via a child component so composition runs.
    lumen_register_component("PBChild", lumen_component(
        | | { return {} },
        |s| { return "<b>" + _ui_prop(s, "label", "none") + "</b>" },
        [], []
    ))
    // Parent template uses single-brace prop:  <PBChild label={ who } />
    let tpl = "<PBChild label=" + "{ who }" + " />"
    let html = lumen_render(tpl, http.create_struct(["who"], ["Varian"]))
    assert_eq(html.contains("Varian"), true)
    assert_eq(html.contains("none"), false)
}
```

Run: `./vn test tests/lumen_prop_brace_test.vn` → must pass. Then
`./vn test tests/` → still all-pass.

---

## Unit 2 — Recursive subdirectory page routing  (issue #5)

### Problem

`pages/about.lumen` becomes route `/about`, but `pages/book/chapter-01.lumen`
is ignored — `_lumen_build_dir` only reads the top level. We want
`pages/book/chapter-01.lumen` → route `/book/chapter-01`, recursively, any depth.

### Grounding (true facts you can rely on)

- `io.list_dir(path)` returns an **array of names** (files and subdirectories,
  no `.`/`..`), or `null` if `path` is not a readable directory.
- **Directory test without any C change:** `io.list_dir(childPath)` returns
  `null` when `childPath` is a regular file (opendir fails), and a (possibly
  empty) array when it is a directory. So:
  `if io.list_dir(p) != null { /* p is a directory */ }`.
- A name ends with `.lumen` ⇒ it is a Lumen page/component file.

### Steps (pure Varian, `vn_modules/lumen.vn`, no C changes)

1. Add a recursive walker `_lumen_collect_pages(dir, route_prefix, comp_prefix)`
   that returns an array of structs, one per `.lumen` page found at this level and
   below, each built with `http.create_struct`:

   - fields: `["path", "route", "comp"]`
     - `path`  = full file path, e.g. `"website/pages/book/chapter-01.lumen"`
     - `route` = URL route, e.g. `"/book/chapter-01"`
     - `comp`  = unique component name, e.g. `"BookChapter01"`
   - For each `name` in `io.list_dir(dir)`:
     - Skip the literal subdirectory named `"components"` (those are shared
       components, compiled separately by the existing code — do not route them).
     - Let `child = dir + "/" + name`.
     - If `io.list_dir(child) != null` ⇒ it's a directory: **recurse** with
       `route_prefix2 = route_prefix + "/" + name` and
       `comp_prefix2 = comp_prefix + _lumen_pascal(name)`, and append all results.
     - Else if `name` ends with `".lumen"`:
       - `base = name` without the trailing `".lumen"` (use
         `name.substring(0, name.len() - 6)`).
       - `route`: if `route_prefix == "" and base == "index"` ⇒ `"/"`.
         Else if `base == "index"` ⇒ `route_prefix` (the directory's own route,
         e.g. `pages/book/index.lumen` → `/book`).
         Else ⇒ `route_prefix + "/" + base`.
       - `comp = comp_prefix + _lumen_pascal(base)` (prefix makes names unique
         across folders so two `index.lumen` in different dirs don't collide).
       - Append `http.create_struct(["path","route","comp"], [child, route, comp])`.
   - Build the result array with the builder pattern (`results.append(...)`).
   - **Always advance** through the `for`/index loop; never `continue` before the
     loop variable moves (see Global rule 4).

2. Rewrite the **page loop** inside `_lumen_build_dir` to use the walker. Find the
   block that currently does `io.list_dir(pages_dir)` then loops over top-level
   files building `mounts` and `combined`. Replace the page-collection portion
   (NOT the `pages/components` portion, and NOT the import-resolution worklist —
   leave those exactly as they are) with:

   ```
   let pages = _lumen_collect_pages(pages_dir, "", "")
   for pi in 0..pages.len() {
       let p = pages[pi]
       let src = io.read_text(p.path)
       if src != null {
           if _lumen_list_has(compiled, p.comp) == false {
               combined = combined + lumen_compile_source(p.comp, src) + "\n"
               compiled = compiled.push(p.comp)
               // queue this file for the import worklist (same as today)
               work_paths = work_paths.push(p.path)
               let pslash = p.path.last_index_of("/")
               let pdir = pages_dir
               if pslash >= 0 { pdir = p.path.substring(0, pslash) }
               work_bases = work_bases.push(pdir)
           }
           mounts = mounts + "lumen_mount(app, \"" + p.route + "\", _lumen_get_component(\"" + p.comp + "\"))\n"
           count = count + 1
       }
   }
   ```

   Keep the existing `compiled` / `work_paths` / `work_bases` / `combined` /
   `mounts` / `count` variables — this block just feeds them from the recursive
   walk instead of the flat top-level loop. Everything after (the `if count == 0`
   throw, the import worklist `while`, the statics, the `program` assembly) stays
   unchanged.

3. Do not remove the existing `pages/components` compilation block — shared
   components still live there and are compiled before the page loop.

### Verify (Unit 2)

```
mkdir -p /tmp/rt/pages/book /tmp/rt/public
printf '<template><div>idx</div></template>\n<script>\nfn state() { return {} }\n</script>\n' > /tmp/rt/pages/index.lumen
printf '<template><div>ch1</div></template>\n<script>\nfn state() { return {} }\n</script>\n' > /tmp/rt/pages/book/chapter-01.lumen
printf 'let n = _lumen_build_dir("/tmp/rt/pages", "/tmp/rt/out.vn", "8099")\nprint("pages=" + n)\n' > /tmp/rt/build.vn
( ulimit -v 9000000; timeout 20 ./vn /tmp/rt/build.vn )
grep 'lumen_mount' /tmp/rt/out.vn
```

Expect: `pages=2`, and the generated file contains both
`lumen_mount(app, "/", _lumen_get_component("Index"))` and
`lumen_mount(app, "/book/chapter-01", _lumen_get_component("BookChapter01"))`.
Then `./vn test tests/` → still all-pass.

---

## Unit 3 — `lumen.store`: shared reactive state (Zustand-style)

### What it is / why it's simple here

Lumen is server-rendered LiveView: `_lumen_live_loop` **re-renders the component
after every event**. So "reactivity" is already automatic — we do NOT need
subscriptions or a diff system. A store is just a **shared mutable holder** that
components read and write; because the loop re-renders after each handler, reads
pick up the new value on the next render for free.

Scope rule (document it, don't enforce it): a store created **inside `state()`**
is per-connection; a store created at **module top level** is app-global (shared
across all connections — good for things like an online-count or a chat room).

### Steps (pure Varian, `vn_modules/lumen.vn`, no C changes)

1. Add a constructor `lumen_store(initial)` where `initial` is a struct of initial
   values. Back it with a 1-element array holding the current struct (so it is a
   mutable reference even though structs are values). Return a struct (built with
   `http.create_struct`) exposing closures:

   - `get(key)` → current value for `key` (via `_validate.get_field`), or `null`.
   - `set(key, value)` → updates the holder's struct (use `_tpl_bind`, which
     returns a new struct with the field set, and store it back into the holder
     array slot 0), returns nothing meaningful.
   - `all()` → the whole current struct (for spreading into a render context).

   Sketch (follow it closely):

   ```
   fn lumen_store(initial) {
       let holder = [initial]   // holder[0] is the live struct
       let get = |key| { return _validate.get_field(holder[0], key) }
       let set = |key, value| { holder[0] = _tpl_bind(holder[0], key, value); return null }
       let all = | | { return holder[0] }
       return http.create_struct(["get", "set", "all"], [get, set, all])
   }
   ```

   - `holder[0] = ...` uses array index-assignment, which mutates in place (this
     is supported and is the whole point — the closures share one holder).
   - `_tpl_bind(struct, key, value)` is the existing helper that returns the
     struct with `key` set; confirm it exists in this file before using (it is
     used throughout `_lumen_process_composition`).

2. Expose it on a `lumen` namespace struct for ergonomics, IF such a struct
   already exists in the file; otherwise just leave `lumen_store` as a global
   function (the dispatch model registers globals fine). Do not invent a module
   system.

3. No changes to the live loop are required: handlers already return new state and
   the loop re-renders. A store mutated in a handler is reflected on the next
   render automatically.

### Verify (Unit 3)

Add `tests/lumen_store_test.vn`:

```
test "store get/set shares one value" {
    let s = lumen_store(http.create_struct(["count"], [0]))
    assert_eq((s.get)("count"), 0)
    (s.set)("count", 5)
    assert_eq((s.get)("count"), 5)
}
test "two readers see the same store" {
    let s = lumen_store(http.create_struct(["n"], [1]))
    let r1 = s.get
    let r2 = s.get
    (s.set)("n", 9)
    assert_eq(r1("n"), 9)
    assert_eq(r2("n"), 9)
}
```

`./vn test tests/lumen_store_test.vn` must pass; then `./vn test tests/` all-pass.

---

## Unit 4 — `lumen.resource`: async-shaped data (React-Query-style), **v1 = synchronous**

### Honest scope — read this before coding

The valuable end state is a resource that fetches **off** the render thread and
pushes a re-render when it resolves. Implementing true off-thread async safely
requires changes to `_lumen_live_loop` (spawn a task, poll a channel, re-render on
arrival) and is **out of scope for this unit** because it is easy to get wrong.

**This unit implements only the v1 synchronous resource**: the `{ loading, error,
data, refetch }` shape with a fetcher that runs on demand (in the handler/render
path). It gives authors the exact ergonomic surface and is correct; it just does
not yet move work off-thread. Document that limitation in a comment. Do **not**
attempt the async/spawn version here — leave a clearly-marked
`// FUTURE (separate milestone): off-thread fetch + live re-render` note and stop.

### Steps (pure Varian, `vn_modules/lumen.vn`, no C changes)

1. Add `lumen_resource(fetcher)` where `fetcher` is a zero-arg closure returning
   the data (or throwing on error). It returns a struct (via `http.create_struct`)
   with fields:

   - `loading` (bool), `error` (string or null), `data` (the value or null),
   - `refetch` — a closure that runs `fetcher()` inside a `try/catch`, updating a
     holder, and returns the new state struct.

   Back the mutable fields with a holder array (same pattern as Unit 3). Sketch:

   ```
   fn lumen_resource(fetcher) {
       // holder[0] = current {loading, error, data} struct
       let holder = [http.create_struct(["loading","error","data"], [true, null, null])]
       let apply = |st| { holder[0] = st; return st }
       let run = | | {
           let st = null
           try {
               let d = fetcher()
               st = http.create_struct(["loading","error","data"], [false, null, d])
           } catch e {
               st = http.create_struct(["loading","error","data"], [false, "" + e, null])
           }
           return (apply)(st)
       }
       // Run once eagerly so first render has data (v1 synchronous).
       (run)()
       let snapshot = | | { return holder[0] }
       return http.create_struct(
           ["state", "refetch"],
           [snapshot, run]
       )
   }
   ```

   - Note the `try/catch` here is exactly how you surface fetch failures into
     `error`. Do not let the fetcher throw out of the resource.
   - `"" + e` stringifies the thrown error message (uncaught-throw printing was
     just fixed, but here you catch it and format it yourself).

2. Usage pattern to put in a doc comment above the function (authors call
   `r.refetch()` in a handler, and read `r.state()` in render):

   ```
   // let users = lumen_resource(| | { return db.query("select ...") })
   // render:  if users.state().loading { ... } else { each users.state().data ... }
   // handler: users.refetch()   // re-runs the fetcher; loop re-renders after
   ```

### Verify (Unit 4)

Add `tests/lumen_resource_test.vn`:

```
test "resource resolves data synchronously" {
    let r = lumen_resource(| | { return 42 })
    assert_eq((r.state)().loading, false)
    assert_eq((r.state)().data, 42)
    assert_eq((r.state)().error, null)
}
test "resource captures fetch error" {
    let r = lumen_resource(| | { throw("boom") })
    assert_eq((r.state)().data, null)
    assert_eq((r.state)().error.contains("boom"), true)
}
```

`./vn test tests/lumen_resource_test.vn` must pass; then `./vn test tests/`
all-pass.

---

## Unit 5 — Lumen forms + validation (Zod-style), wiring the existing validator

### What it is

`vn_modules/validate.vn` already provides `validate.str()/.num()/.object()` with
`.parse(v)` returning `{ success, data | error }`. This unit adds a thin Lumen
helper so a component can declare a schema, validate a submitted struct, and get
per-field errors to show in the template. **No C changes. Reuse `validate.vn`;
do not reimplement validation.**

### Steps (pure Varian, `vn_modules/lumen.vn`, no C changes)

1. Add `lumen_form(field_schemas)` where `field_schemas` is a struct mapping
   field name → a validator (e.g. `validate.str().min(3)`). It returns a struct
   (via `http.create_struct`) with:

   - `validate(values)` → a struct `{ ok, values, errors }` where:
     - iterate the schema's keys with `_validate.get_keys(field_schemas)`;
     - for each `key`, read `validator = _validate.get_field(field_schemas, key)`
       and `val = _validate.get_field(values, key)`;
     - `res = validator.parse(val)`; if `res.success` is false, record
       `errors[key] = res.error`; else record the cleaned `res.data` into a
       `clean` struct;
     - `ok` is true iff no errors were recorded.
   - Build `errors` and `clean` structs by collecting parallel key/value arrays
     and calling `http.create_struct(keys, vals)` once at the end (do not build
     structs incrementally in a way that depends on mutation you haven't
     verified). Use the array builder pattern for the key/value arrays.
   - **Loop carefully:** advance the index every iteration; no `continue` before
     advancing.

   Return shape:

   ```
   // return http.create_struct(["ok","values","errors"], [ok, cleanStruct, errorsStruct])
   ```

2. Document the intended template usage in a comment:

   ```
   // schema: let f = lumen_form(http.create_struct(["email"], [validate.str().is_email()]))
   // handler on submit:  let r = (f.validate)(form_values)
   //                     if r.ok { ...save r.values... } else { state.errors = r.errors }
   // template:  {{ errors.email }}   // shows the message when present
   ```

### Verify (Unit 5)

Add `tests/lumen_form_test.vn`:

```
test "form validates good and bad input" {
    let schema = http.create_struct(["name"], [validate.str().min(3)])
    let f = lumen_form(schema)
    let good = (f.validate)(http.create_struct(["name"], ["Ada"]))
    assert_eq(good.ok, true)
    let bad = (f.validate)(http.create_struct(["name"], ["x"]))
    assert_eq(bad.ok, false)
    assert_eq(_validate.get_field(bad.errors, "name") != null, true)
}
```

`./vn test tests/lumen_form_test.vn` must pass; then `./vn test tests/` all-pass.

---

## Unit 6 — Unify Lumen `import` with `use`/Constellation package resolution

### Why this exists

Right now there are two import systems that don't meet:

- **`use "<pkg>"`** is the *language* construct (C parser, `src/parser.c`,
  `parser_resolve_use`). It loads a Constellation package's **Varian source**
  (`.vn`) from `./vn_modules/<pkg>/` (project) or `$VARIAN_HOME/vn_modules/<pkg>/`
  (global). Used for backend/library code.
- **Lumen `import`** (added in `_lumen_build_dir`) currently resolves only
  relative `./x.lumen` files and the built-in `"lumen/ui"`. A **bare package
  name** — `import { Button } from "lumen-ui"` — falls through the if/else chain
  and is **silently ignored** (a real bug).

This unit makes a bare-name Lumen `import` resolve from the **same package roots
`use` uses**, so one Constellation package can ship both Varian code (reached via
`use "<pkg>"`) and `.lumen` UI components (reached via
`import { Comp } from "<pkg>"`). It also turns the silent-ignore into a loud
error. **No C changes** — the Lumen build is Varian and replicates the same root
search in Varian.

### Package layout assumption (state it in a comment, then rely on it)

A Constellation UI package `<pkg>` is the directory `vn_modules/<pkg>/`. Its
components are **PascalCase `.lumen` files at the package root**, e.g.
`vn_modules/lumen-ui/Button.lumen`, `vn_modules/lumen-ui/Card.lumen`. So
`import { Button, Card } from "lumen-ui"` resolves to
`vn_modules/lumen-ui/Button.lumen` and `…/Card.lumen`. (A package's `.vn` code is
the `use "<pkg>"` path and is out of scope for Lumen `import`.)

### Steps (pure Varian, `vn_modules/lumen.vn`, no C changes)

1. Add a helper `_lumen_resolve_pkg_dir(pkg)` near `_lumen_join_path`. It returns
   the package directory string, or `null` if the package isn't installed. Search
   the **same roots `use` uses, in the same order**:

   ```
   fn _lumen_resolve_pkg_dir(pkg) {
       let local = "vn_modules/" + pkg
       if io.list_dir(local) != null { return local }
       let home = env.get("VARIAN_HOME")
       if home != null {
           let g = home + "/vn_modules/" + pkg
           if io.list_dir(g) != null { return g }
       }
       return null
   }
   ```

   (`io.list_dir(dir) != null` is the directory-exists test — it returns `null`
   for a missing dir or a regular file, and an array for a real directory.)

2. In `_lumen_build_dir`, the import-resolution `for k in 0..imps.len()` loop
   currently has exactly these branches:

   ```
   if path == "lumen/ui" or path == "lumen" { ... built-in validation ... }
   else if path.ends_with(".lumen") { ... relative file ... }
   ```

   Add a **new `else if` branch for bare package names**, then a **final `else`
   that throws** (this is the silent-ignore fix). Insert between the `.lumen`
   branch and the end of the chain:

   ```
   } else if path.contains("/") == false and path.contains(".") == false {
       // Bare Constellation package name -> resolve its PascalCase .lumen
       // components from the same roots `use` uses.
       let pkgdir = _lumen_resolve_pkg_dir(path)
       if pkgdir == null {
           throw("Lumen: package \"" + path + "\" is not installed (run `vn add " + path + "`) (imported in " + fpath + ")")
       }
       for n in 0..names.len() {
           let cname = names[n]
           if _lumen_list_has(compiled, cname) == false {
               let cpath = pkgdir + "/" + cname + ".lumen"
               let csrc = io.read_text(cpath)
               if csrc == null {
                   throw("Lumen: package \"" + path + "\" has no component \"" + cname + "\" (looked for " + cpath + ", imported in " + fpath + ")")
               }
               combined = combined + lumen_compile_source(cname, csrc) + "\n"
               compiled = compiled.push(cname)
               work_paths = work_paths.push(cpath)
               work_bases = work_bases.push(pkgdir)
           }
       }
   } else {
       throw("Lumen: unresolved import \"" + path + "\" (in " + fpath + "). Use a relative \"./x.lumen\" path, a bare package name, or \"lumen/ui\".")
   }
   ```

   - Keep the existing two branches above unchanged; you are only **adding** the
     `else if` (bare name) and the closing `else` (loud error).
   - The bare-name test `path.contains("/") == false and path.contains(".") == false`
     deliberately excludes `lumen/ui` (has `/`, already handled) and `./x.lumen`
     (has `.`, already handled). Confirm `.contains` exists as a string method in
     this file (it's used elsewhere); if not, use `path.index_of("/") < 0 and path.index_of(".") < 0`.
   - Component names from a package register by their plain name (`Button`), so a
     package `Button` overrides the built-in `Button` via last-wins registration —
     that is expected and fine (a UI package is meant to replace the primitive).

3. Do not touch `src/parser.c` or `parser_resolve_use`. The two systems stay
   separate code paths but now share the **same package roots and the same
   Constellation packages** — that is the unification. Add a short comment at the
   new branch noting it mirrors `parser_resolve_use`'s root order.

### Verify (Unit 6)

```
mkdir -p /tmp/u6/pages /tmp/u6/vn_modules/demokit /tmp/u6/public
printf '<template><b>kit-btn</b></template>\n<script>\nfn state() { return {} }\n</script>\n' > /tmp/u6/vn_modules/demokit/Button.lumen
printf '<template><Button /></template>\n<script>\nimport { Button } from "demokit"\nfn state() { return {} }\n</script>\n' > /tmp/u6/pages/index.lumen
printf 'let n = _lumen_build_dir("/tmp/u6/pages", "/tmp/u6/out.vn", "8099")\nprint("pages=" + n)\n' > /tmp/u6/build.vn
cd /tmp/u6 && ( ulimit -v 9000000; timeout 20 /root/dev/VarianLang/vn /tmp/u6/build.vn ); cd -
grep -E 'init_component_Button|register_component\("Button"\)' /tmp/u6/out.vn
```

Expect: `pages=1`, and the generated file contains
`fn _lumen_init_component_Button()` (the package component got compiled). Note the
build must run with `/tmp/u6` as the working directory so `vn_modules/demokit`
resolves project-locally (that's how `use` resolves too).

Negative test — a missing package must error loudly, not silently:

```
printf '<template><X /></template>\n<script>\nimport { X } from "nopkg"\nfn state() { return {} }\n</script>\n' > /tmp/u6/pages/bad.lumen
cd /tmp/u6 && ( ulimit -v 9000000; timeout 20 /root/dev/VarianLang/vn /tmp/u6/build.vn 2>&1 | head -2 ); cd -
```

Expect a clear `Lumen: package "nopkg" is not installed (run `vn add nopkg`)` line
(via the new uncaught-throw printing). Then remove `/tmp/u6/pages/bad.lumen` and
re-run the positive test; finally `./vn test tests/` → all-pass.

---

## Final acceptance (after all five units)

1. `./vn test tests/` → all-pass (was 82; will be ~86 with the new unit tests).
2. Rebuild the real website end to end and confirm it still compiles fast and
   does not balloon memory:
   ```
   ( ulimit -v 9000000; /usr/bin/time -v ./vn /tmp/wbuild.vn 2>&1 | grep -E "website pages|Maximum resident|Elapsed" )
   ```
   (where `/tmp/wbuild.vn` calls
   `_lumen_build_dir("website/pages", "/tmp/website_out.vn", "8080")`).
   Expect: it prints the page count, finishes in well under 2 s, and Maximum
   resident stays in the low hundreds of MB — NOT multiple GB.
3. Report, per unit: the files changed, the new test file, and its pass/fail.
   If any unit could not be completed as written, stop and report exactly what
   broke rather than improvising a different design.

## Explicitly out of scope (do NOT attempt)

- Off-thread/async resource fetching with live re-render (Unit 4 is synchronous
  only; the async version is a separate, later milestone).
- Cross-connection store broadcast / pub-sub (Unit 3 is a shared holder only).
- Any new VM opcode, value type, or C-level feature.
- Touching the `{{ }}` interpolation engine, `_lumen_escape_string`, the closure
  model, or the loop/`continue` codegen.
