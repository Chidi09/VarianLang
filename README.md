<p align="center">
  <img src="docs/assets/varian-logo.png" alt="Varian" width="280" />
</p>

<h1 align="center">Varian</h1>

<p align="center">
  A compiled, concurrent, systems-level programming language with a custom bytecode VM —
  batteries included, one binary, zero <code>node_modules</code>.
</p>

<p align="center">
  <a href="#quick-start">Quick start</a> ·
  <a href="#lumen--the-frontend-framework">Lumen</a> ·
  <a href="#aurora--the-fullstack-framework">Aurora</a> ·
  <a href="#zenith--the-web-framework">Zenith</a> ·
  <a href="#standard-library">Stdlib</a> ·
  <a href="#cli">CLI</a> ·
  <a href="#documentation">Docs</a>
</p>

---

## Why Varian

Most languages make you assemble a stack: a web framework, a template engine, a build tool,
a package manager, a formatter, a linter, a test runner, an ORM, an LSP server. Each is a
separate install, separate version, separate supply-chain surface.

Varian is **one binary** that is all of those things. The language runtime, the web
framework (Zenith), the frontend framework (Lumen), the build tool (Kiln), the package
manager (Constellation), the LSP server, the formatter, the linter, and the test runner
live in a single native executable. There is nothing to `npm install`, no `pip install`,
no `cargo install` — just `vn` and you're building web apps.

- **Go-style concurrency** — cooperative green-thread tasks, actors, channels
- **Batteries-included stdlib** — SQLite, Postgres, Redis, JWT auth, SMTP, validation,
  HTML templating, regex, sanitization, signed sessions, background queues, OpenAPI docs
- **Server-driven frontend** — Lumen renders HTML on the server, patches the DOM over a
  WebSocket; no client-side framework, no hydration mismatches, no `node_modules`
- **AOT compilation** — ship your app as a native binary via `vn build --release`

---

## Quick start

```sh
# Build the compiler
make

# Run a script
./vn run examples/hello.vn

# Start a new Aurora (fullstack) project
./vn new myapp
cd myapp
./vn dev              # http://localhost:8090 — live reload

# Scaffold a Lumen-only frontend
./vn lumen new myapp
cd myapp
./vn dev              # http://localhost:8090 — live reload

# Run tests
./vn test tests/

# Format code
./vn fmt .

# Start the LSP (for VS Code, Neovim, Zed, etc.)
./vn lsp
```

---

## Lumen — the frontend framework

<p align="center">
  <img src="docs/assets/lumen-logo.png" alt="Lumen Logo" width="170" />
</p>

<p align="center"><i>Server-driven. Live by default. Zero config, zero <code>node_modules</code>, zero hydration mismatch.</i></p>

Lumen is a full-stack UI framework that ships *inside the `vn` binary* — Varian's answer to
Next.js and Nuxt. You write `.lumen` components, run `vn dev`, and you have a live,
reactive app.

> **Lumen : Varian :: JSX : TypeScript.** A `.lumen` file is markup + bindings; the logic
> underneath is plain Varian (`.vn`).

### The core idea: server-driven live

In Lumen, **the server owns all state and does all rendering.** The browser runs a tiny
runtime whose only jobs are: forward DOM events over a WebSocket, and patch the DOM with
whatever HTML the server sends back. State lives in plain Varian on the server — no
`useState`, no `useEffect` dependency arrays, no stale closures, no `useMemo` ceremony.

```
┌────────── Browser ──────────┐         ┌────────────── Server (Varian) ──────────────┐
│  click ─▶ data-lumen-click  │ ──WS──▶ │  run handler ▶ new state ▶ re-render HTML    │
│  morph DOM ◀── DOM patch ── │ ◀──WS── │  diff vs last HTML ▶ send minimal splice     │
└─────────────────────────────┘         └──────────────────────────────────────────────┘
```

Because the server renders the *real* HTML on every change, **SSR and SPA-grade
interactivity are the same mechanism** — there is nothing to "hydrate," so there is no
mismatch to debug.

### Anatomy of a `.lumen` component

```html
<template>
  <main style="display:grid;place-items:center;min-height:100vh">
    <svg @click="pulse" viewBox="0 0 48 48" width="150">
      <rect x="3" y="3" width="42" height="42" rx="12" fill="#1b2233"/>
      <path d="M26 7 L15 27 h7 L19 41 L33 21 h-8 L29 7 Z"
            fill="{{ color }}" style="transition:fill .35s ease"/>
    </svg>
    <p>pulse <b>{{ count }}</b></p>
  </main>
</template>

<script>
fn state() { return { count: 0, color: "#f5b829" } }
fn pulse(s, v) {
  let n = s.get("count") + 1
  return s.set("count", n).set("color", ["#f5b829", "#ff6b6b", "#4dd4ac"][n % 3])
}
</script>
```

### API

| Function | What it does |
|---|---|
| `lumen_mount(app, path, comp)` | Mount a live component at a route on a Zenith app |
| `lumen_mount_data(app, path, comp, provider)` | Same, with a request-scoped data provider for initial state |
| `lumen_mount_dynamic(app, path, comp, param)` | Same, with a URL path param injected into state |
| `lumen_store(initial)` | Zustand-style store: `get(key)`, `set(key, val)`, `all()` |
| `lumen_resource(fetcher)` | React-Query-style resource: `state()`, `refetch()` |
| `lumen_async_resource(fetcher)` | Same, but refetch spawns a background task and re-renders on completion |
| `lumen_publish(topic, key, val)` | Broadcast a value to all connections on a topic |
| `lumen_subscribe(topic)` | Returns a `{ get(key) }` reader for a topic |
| `lumen_form(schemas)` | Zod-style form validator: `validate(values)` → `{ ok, values, errors }` |
| `lumen_register_component(name, comp)` | Register a reusable component by name |
| `lumen_meta(title, desc, og)` | Declare per-page SEO metadata |
| `lumen_render(tpl, ctx)` | Render a Lumen template string to HTML |
| `lumen_build_dir(dir, out, port)` | Compile a `pages/` directory into a runnable app (file-based routing) |
| `lumen_build_static_dir(dir, out, base)` | SSG: render all pages to static HTML + sitemap.xml |

### Built-in UI components (~27)

`Page`, `Container`, `Section`, `Stack`, `Row`, `Grid`, `Card`, `Heading`, `Text`,
`Eyebrow`, `Button`, `Badge`, `Feature`, `Divider`, `Spacer`, `Hero`, `Nav`, `Footer`,
`Split`, `Field`, `Stat`, `Tag`, `Avatar`, `Alert`, `Empty`, `Skeleton`, `Icon`

### Test helpers

`lumen_test_render`, `lumen_test_event`, `lumen_test_contains`, `lumen_test_attr`,
`lumen_test_count`, `lumen_test_render_response`

### Dev console

```text
   LUMEN   v0.1.0   the Varian frontend framework

  ➜  Local     http://localhost:8090/
  ➜  Pages     2 in pages/

     ● /                  index.lumen
     ● /about             about.lumen

  ✔ ready in 142 ms  · watching pages/ — edit a page to hot-reload
```

- **File-based routing.** Drop `pages/index.lumen` → served at `/`.
- **Live reload.** Save a file → server rebuilds → browser auto-reconnects.
- **Error overlay.** Runtime errors show a branded in-browser overlay with file/line/caret
  and a fix hint.
- **Batteries included.** Favicons, manifest, responsive viewport, Degular typeface —
  all scaffolded by `vn lumen new`.

### CLI

```sh
vn lumen new myapp              # Scaffold pages/ + public/ + starter component
vn dev                          # Serve ./pages with live reload (default :8090)
vn dev pages 3000               # Custom dir + port
vn lumen build pages app.vn 8090   # Compile pages/ into one runnable app
```

---

## Aurora — the fullstack framework

<p align="center">
  <img src="docs/assets/aurora-logo.png" alt="Aurora Logo" width="150" />
</p>

Aurora combines Zenith (server) and Lumen (frontend) into one unified project
structure — Varian's answer to Next.js / Nuxt / Rails. A `constellation.toml` with
`kind = "aurora"` tells the toolchain to bundle both frameworks.

```sh
vn new myapp          # Scaffold a full Aurora project
cd myapp
vn dev                # http://localhost:8090 — live reload
vn build --release    # Compile to a native binary
```

---

## Zenith — the web framework

<p align="center">
  <img src="docs/assets/zenith-logo.png" alt="Zenith Logo" width="200" />
</p>

Zenith is Varian's built-in HTTP web framework — a non-blocking,
`io_uring`-powered server that lives inside the `vn` binary.

```swift
let app = new_app()

app.get("/users/:id", |req| {
    let user_id = req.params["id"]
    return json_response({ "user": user_id, "status": "active" }, 200)
})

app.listen(3000)
```

**Features:**

| Feature | Implementation |
|---|---|
| **Routing** | Radix (segment) trie — O(depth) lookup. All HTTP methods: GET/POST/PUT/DELETE/PATCH/OPTIONS/HEAD |
| **Middleware** | Closure chain with `next(req)` — CORS, CSRF, auth, logging, etc. |
| **Static serving** | `serve_static(prefix, dir)` with path-traversal protection, MIME resolution |
| **WebSocket** | Full RFC 6455 — masking, opcodes, close frames |
| **SSE** | `sse_handshake(req)` → `SseSender.send(data)` |
| **OpenAPI** | `enable_docs(endpoint)` → `/openapi.json` + Swagger UI with schema registration |
| **Sessions** | Stateless JWT-signed cookies — `session_get`, `session_set`, `session_clear` |
| **Templating** | `render(tpl, ctx)` — `<%= %>` escaped, `<%- %>` raw, `<% if %>`, `<% for %>` |
| **Request helpers** | `query(req, key, default)`, `form(req, key, default)`, `cookie(req, name, default)` |
| **Response helpers** | `json_response`, `redirect`, `redirect_with`, `set_cookie`, `clear_cookie`, `with_header` |
| **Listen modes** | `listen(port)`, `listen_cluster(port, workers)`, `listen_tls(...)`, `listen_tls_cluster(...)` |
| **Error handling** | `on_error(handler)` — custom error responses |

**Caveat (from source):** WebSocket/SSE upgrades write directly to the raw socket FD
and are not TLS-aware — `wss://` is unsupported. For production HTTPS, terminate TLS at
a reverse proxy (nginx/Caddy) proxying `ws://` to Zenith.

**Security middleware** (`shield.vn` — auto-loaded):

| Middleware | What it protects |
|---|---|
| `cors(origins, methods, headers)` | Cross-origin requests + OPTIONS preflight |
| `csrf()` | Double-submit cookie pattern, constant-time compare |
| `rate_limit(max_reqs, window_ms)` | In-memory token bucket (single-worker) |
| `rate_limit_redis(conn, max_reqs, window_s)` | Redis-backed fixed-window (cluster-safe) |

---

## Standard Library — Varian modules (auto-loaded)

All `vn_modules/*.vn` files are automatically concatenated as a prelude — no imports
needed. Every function and struct below is in scope the moment you run `vn`.

| Module | What it provides |
|---|---|
| `zenith.vn` | Web framework — `new_app()`, routing, middleware, static serving, WebSocket, SSE, OpenAPI, templating, sessions, request/response helpers |
| `lumen.vn` | Frontend framework — `lumen_mount`, `lumen_store`, `lumen_resource`, `lumen_form`, file-based routing, SSG, 27 UI components, test DSL |
| `shield.vn` | Security middleware — `cors()`, `csrf()`, `rate_limit()`, `rate_limit_redis()` |
| `auth.vn` | Auth middleware — `zenith_auth.jwt(secret)`, `zenith_auth.session_store()`, `zenith_auth.session(store, cookie)` + password helpers |
| `db.vn` | Compile-time SQL query builder — `select()`, `bind()`, `run_sqlite()`, `run_postgres()`, `test_transaction()` |
| `queue.vn` | Background jobs — `WorkerPool.spawn(n)`, `.submit(fn)`, `.stop()` — also `cron(interval_ms, handler)` |
| `mail.vn` | Email — `send_smtp(host, port, ...)`, `send_resend(api_key, ...)` |
| `storage.vn` | Local file blob store — `new_storage(dir)`, `.put(key, bytes)`, `.get(key)`, `.delete(key)` |
| `observe.vn` | Structured logging (`Logger.info`/`.warn`/`.error`/`.info_with`) + Prometheus metrics handler |
| `validate.vn` | Declarative validation — `validate.str().is_email().parse(v)`, `validate.object({...}).parse(v)` |

## Standard Library — C native modules

| Module | Functions |
|---|---|
| `http` | `get`, `post`, `serve`, `serve_tls`, `create_struct`, `write_socket`, `read_socket`, `close_socket`, `test_request` |
| `auth` | `hash_sha256`, `sign_jwt`, `verify_jwt`, `hash_password`, `verify_password`, `generate_token`, `constant_time_eq`, `sha1_base64` |
| `sqlite` | `connect`, `query`, `close` |
| `postgres` | `connect`, `query`, `close` |
| `redis` | `connect`, `cmd`, `close` |
| `smtp` | `send` |
| `math` | `sin`, `cos`, `sqrt`, `abs`, `floor`, `ceil`, `bit_and`, `bit_or`, `bit_xor` |
| `regex` | `test`, `match`, `groups`, `find_all`, `replace` |
| `io` | `read_text`, `write_text`, `read_bytes`, `write_bytes`, `exists`, `mkdir`, `delete`, `list_dir` |
| `task` | `spawn`, `sleep`, `yield`, `id`, `channel`, `close`, `try_receive` |
| `time` | `now_ms`, `now_iso8601` |
| `env` | `get`, `require`, `load` |
| `errors` | `explain`, `kind`, `is`, `make` |
| `_sanitize` | `escape_html`, `strip_html`, `trim` |
| `_validate` | `is_email`, `is_url`, `is_alphanumeric`, `min_len`, `max_len`, `get_field`, `get_keys` |
| `struct` | `set`, `get`, `has`, `keys` |
| `mock` | `intercept`, `restore` |
| `python` | `run` |
| Top-level | `print`, `json_encode`, `json_decode`, `throw`, `assert_eq`, `assert_ne`, `assert_throws` |

---

## Kiln — the build tool

<p align="center">
  <img src="docs/assets/kiln-logo.png" alt="Kiln" width="120" />
</p>

Kiln (`vn build`) compiles Varian applications into single-file artifacts. It
auto-detects project structure (pages/ → Lumen mode), embeds `public/` assets directly
into the output, and can produce a native binary via `--release`.

```sh
vn build                    # Bundle into a .vnb
vn build --release          # Compile to a native C binary
```

See [`docs/KILN.md`](docs/KILN.md).

---

## Constellation — the package manager

<p align="center">
  <img src="docs/assets/constellation-logo.png" alt="Constellation" width="120" />
</p>

Constellation (`vn add`/`vn remove`/`vn search`/`vn install`/`vn publish`) is Varian's
built-in package management. It uses a hybrid CDN index + git vendoring model —
no central registry to maintain, no npm-style supply-chain attack surface.

```sh
vn add my-lib          # Record a dependency
vn install             # Fetch and vendor all deps
vn publish             # Publish a package
```

See [`docs/CONSTELLATION.md`](docs/CONSTELLATION.md).

---

## CLI

```
vn                      Start interactive REPL
vn run <file>           Execute a Varian script
vn fmt <file>           Format a Varian script in-place
vn test [dir]           Run *_test.vn tests (default: .)
vn lint [path]          Lint a file or directory
vn add <pkg>            Add a package dependency
vn remove <pkg>         Remove a package
vn install [--frozen]   Install dependencies
vn update               Update dependencies
vn search <q>           Search the registry
vn publish              Publish a package
vn build <file>         Build a .vnb or --release native binary
vn lsp                  Start the LSP server
vn new <name>           Scaffold an Aurora project
vn dev [dir] [port]     Serve with live reload (default ./pages :8090)
vn lumen new <name>     Scaffold a Lumen-only frontend
vn lumen add <comp>     Copy a Lumen UI component
vn lumen build <dir> <out>   Compile pages/ into one runnable app
```

---

## Architecture

Source → Lexer → Parser → AST → Compiler → Bytecode → VM

All frontend allocations use a fast arena allocator. The bytecode VM is a register-style
stack machine with heap-allocated objects and a deeply integrated cooperative green-thread
scheduler (tasks, actors, channels). Long-lived objects are managed by a mark-and-sweep
GC; short-lived request allocations use task-local bump arenas.

---

## Documentation

| Doc | What it covers |
| --- | --- |
| [`docs/LANGUAGE.md`](docs/LANGUAGE.md) | Core language — types, functions, structs, generics, enums, traits, error handling, decorators, comptime, FFI |
| [`docs/CONCURRENCY.md`](docs/CONCURRENCY.md) | Tasks, channels, actors |
| [`docs/STDLIB.md`](docs/STDLIB.md) | Native modules — math, string, regex, SQLite, Postgres, Redis, HTTP, auth, validate, SMTP, JSON, Python bridge, FFI |
| [`docs/ZENITH.md`](docs/ZENITH.md) | The Zenith web framework |
| [`docs/LUMEN.md`](docs/LUMEN.md) | The Lumen frontend framework — components, events, slots, islands |
| [`docs/KILN.md`](docs/KILN.md) | Kiln build tool — bundling, AOT, asset embedding |
| [`docs/CONSTELLATION.md`](docs/CONSTELLATION.md) | Constellation package manager — registry, vendoring, publishing |
| [`docs/TOOLING.md`](docs/TOOLING.md) | `vn` CLI reference — all commands, environment variables, module loading |
| [`docs/SECURITY.md`](docs/SECURITY.md) | Threat model, hardened build, app-level defenses |
| [`docs/DEPLOYMENT.md`](docs/DEPLOYMENT.md) | Releasing, editor extensions, hosting the website |

---

## Building

```sh
make
./vn run examples/hello.vn
```

Requires: `build-essential`, `libcurl4-openssl-dev`, `libssl-dev`, `libsqlite3-dev`,
`libpq-dev`, `libhiredis-dev`, `libffi-dev`, `liburing-dev`.

---

## Editor support

- **VS Code** — `editors/vscode/` is a complete extension (syntax highlighting + LSP
  client + file icons for `.vn`/`.lumen`). `npx vsce package` produces a `.vsix`.
- **Zed** — LSP works via `editors/zed/`; highlighting needs a `tree-sitter-varian` grammar.
- **Neovim / any LSP client** — point at `vn lsp`.
