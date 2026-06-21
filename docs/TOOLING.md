# Tooling — the `vn` CLI

```
vn                  Start interactive REPL
vn run <file>        Execute a Varian script
vn <file>            Same as `vn run <file>` (back-compat shorthand)
vn fmt <file>        Format a Varian script in-place
vn test [dir] [--filter <substr>] [--timeout <secs>]   Run tests in a directory (default: .)
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

`--filter <substr>` runs only tests whose description contains `<substr>`. `--timeout
<secs>` (fractional seconds allowed) caps each individual test; a test exceeding it is
killed at the next loop back-edge, marked `⏱️ TIMEOUT`, and counted as a failure without
hanging the rest of the run. The summary line reports `X passed, Y failed (Z timed out),
W skipped`.

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

## Package Management & Registry (`vn add`, `vn remove`, `vn install`, `vn update`, `vn search`)

For detailed information on Varian's package registry architecture, transitive resolution, capabilities, and version ranges, see **[Constellation Documentation](file:///root/dev/VarianLang/docs/CONSTELLATION.md)**.

* **Add a package dependency**:
  ```
  vn add <pkg_name>
  ```
  Appends `<pkg_name> = "latest"` to `constellation.toml` (initializing a new manifest if missing).

* **Remove a dependency**:
  ```
  vn remove <pkg_name>
  ```
  Removes the package from the manifest, prunes its directory and any unused transitive dependencies under `vn_modules/`, and updates the lockfile.

* **Install dependencies**:
  ```
  vn install [--frozen]
  ```
  Fetches direct Git and index-resolved dependencies recursively, extracts them to `vn_modules/`, and writes the `constellation.lock` file. The `--frozen` flag prevents index updates and fails if the lockfile is out of sync.

* **Update dependencies**:
  ```
  vn update
  ```
  Forces fresh queries to Git/registry index to re-resolve matching versions, computes updated SHA-256 integrity hashes, and rewrites the lockfile.

* **Search registry**:
  ```
  vn search <query>
  ```
  Queries the registry index for packages matching `<query>` (or `"*"` for all) and lists their registered versions.

* **Wrapping foreign libraries**:
  ```
  vn wrap python:requests
  ```
  Introspects a Python module and writes `vn_modules/<module>.vn` wrapper functions calling `python.run(module, fn, args)` (see **[Standard Library Docs](file:///root/dev/VarianLang/docs/STDLIB.md)**).

## Build Orchestration (Kiln — `vn build`)

Varian applications are assembled and compiled using Kiln. For detailed information on bytecode container formats, native AOT compile harnesses, and compilation caching, see **[Kiln Documentation](file:///root/dev/VarianLang/docs/KILN.md)**.

* **Build a portable bytecode bundle**:
  ```
  vn build
  ```
  Assembles all source files and compiles them into a fast, portable `app.vnb` bytecode container (executable directly via `vn app.vnb`).
  
* **Build a native standalone binary**:
  ```
  vn build --release
  ```
  Compiles Varian code directly to optimized C via AOT, compiles it with the host compiler, and links it against `libvarian.a` to produce a standalone executable binary.

## Building from source

```
make            # default: -g debug build, fast to rebuild — use while developing
make release    # -O2 hardened build to ship/benchmark (see docs/SECURITY.md)
make asan       # AddressSanitizer + UBSan build, for finding memory bugs
make clean
```

`make release` enables `_FORTIFY_SOURCE=2`, stack-protector/-clash protection, full
RELRO + immediate binding, a non-executable stack, and a position-independent executable
(ASLR). `make asan` builds with `-fsanitize=address,undefined`; run the suite under it
with `ASAN_OPTIONS=detect_leaks=0 ./vn test tests/` to catch overflows and UB. Both do a
`clean` first so object files aren't mixed between flag sets.
