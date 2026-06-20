# Tooling — the `vn` CLI

```
vn                  Start interactive REPL
vn run <file>        Execute a Varian script
vn <file>            Same as `vn run <file>` (back-compat shorthand)
vn fmt <file>        Format a Varian script in-place
vn test [dir]         Run tests in a directory (default: .)
vn lint [path] [--only category] [--format json]   Lint a file or directory
vn add <pkg>          Record a dependency in varian.pkg
vn wrap <target>      Generate a Varian wrapper for a foreign library
vn --help             Show usage
```

Debug environment variables: `VN_DEBUG_AST=1` prints the parsed AST before compilation;
`VN_DEBUG_BYTECODE=1` prints the compiled bytecode disassembly.

## Module loading: there is no `import`

`vn run <file>` (and the bare `vn <file>` form) automatically reads every `.vn` file in
`vn_modules/` (if that directory exists in the current working directory) and
concatenates them as a prelude *before* your file's source, then compiles the combined
source as one program. There is no `import`/`use` keyword in the lexer at all — Zenith,
the comptime ORM, and `queue.vn`'s helpers are simply always in scope as long as
`vn_modules/` is present next to where you run `vn` from.

## `vn run` / bare file

```
vn run examples/zenith_app_test.vn
vn examples/zenith_app_test.vn      // identical
```

## `vn fmt`

Formats a file in place. Implemented as a separate comment-preserving re-lexer
(`src/fmt.c`) — distinct from the parser used by `lint`/`test`/`run`, since formatting
needs to round-trip whitespace/comments that the AST throws away.

## `vn test`

```
vn test tests/
```

Walks a directory for `.vn` files and runs each as a test file. Inside a test file, use
`test "description" { ... }` blocks (see `examples/` and `tests/` for real examples) with
the assertion globals `assert_eq(a, b)`, `assert_ne(a, b)`, `assert_throws(fn)` — all
ordinary globals, not a module. On failure, the actual error message (not just the test
description) is printed indented under the `❌ FAIL` line, so you don't have to re-run
under a debugger to see why something failed.

Mocking native module functions inside tests:

```varian
let saved = mock.intercept("sqlite", "query", |conn, sql, params| { return [] })
// ... test code that calls sqlite.query(...) and gets [] back ...
mock.restore("sqlite", "query", saved)
```

`mock.intercept(type_name, method_name, fake_fn)` swaps in `fake_fn` for any
(type, method) dispatch pair — the same dispatch table every native module
(`sqlite`, `redis`, `string`, etc.) is built on — and returns whatever was previously
bound there, so `mock.restore(type_name, method_name, saved)` can put it back exactly.

## `vn lint`

```
vn lint examples/
vn lint src/foo.vn --only security
vn lint . --format json
```

AST-walking static analysis (not the `fmt.c` tokenizer — a real parse tree walk, the same
pipeline `vn run`/`vn test` use). Output is one line per finding:
`path:line: [category] message`. Categories:

- **correctness**: unreachable code (any statement after `return`/`break`/`continue`/
  `throw(...)` in the same block), unused `let` bindings, shadowed bindings (a `let` name
  that shadows one already declared in an enclosing scope).
- **security**: hardcoded secrets (a `let`/field named like `secret`/`key`/`token`/
  `password`/`api_key` assigned a string literal longer than ~12 chars), string-
  concatenated SQL passed directly to `sqlite.query`/`postgres.query` (the anti-pattern
  the comptime ORM in `docs/ZENITH.md` exists to replace).
- **performance**: N+1 query pattern (a `sqlite`/`postgres`/`redis` call found inside a
  `for`/`while` body), SQL containing `SELECT` passed as a query argument with no
  `LIMIT` clause.

`--only <category>` filters to one category; `--format json` emits machine-readable
output instead of the plain-text lines above.

## `vn add` / `vn wrap` — package scaffolding

```
vn add some-package
```

Appends `some-package = "latest"` to `varian.pkg` (creating it with a `[deps]` header if
it doesn't exist yet) and creates `vn_modules/` if missing. This is bookkeeping only —
there is no registry, fetch, or version resolution implemented; it just records intent.

```
vn wrap python:requests
```

The only currently-supported wrap target is `python:<module>`: it introspects the named
Python module (spawning Python to list its public functions) and writes
`vn_modules/<module>.vn`, a generated wrapper whose functions call through to
`python.run(module, fn, args)` (see `docs/STDLIB.md`) so the rest of your Varian code can
call it like any other module without spelling out `python.run` every time.
