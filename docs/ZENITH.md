# Zenith — the web framework

Zenith (`vn_modules/zenith.vn`) and the comptime ORM (`vn_modules/db.vn`) are not native
modules — they're plain Varian source. Every file under `vn_modules/` is automatically
concatenated as a prelude in front of whatever file you run (see `docs/TOOLING.md`), so
`ZenithApp`, `new_app()`, etc. are just always in scope, with no `import` needed.

## Why Zenith — conveniences over Fastify, Express, FastAPI, and friends

Zenith's pitch is not "fastest raw throughput" (Go and Rust win there — see the honest
numbers in `docs/planning/`). It's **everything a real web app needs, built in, secure by
default, in one binary, with zero dependency tree.** Concretely:

- **Batteries included — nothing to `npm install`.** SQLite, Postgres and Redis drivers,
  JWT auth + password hashing, request validation, HTML sanitization, an HTML template
  engine, signed sessions, cookies, regex, SMTP, a background queue, and OpenAPI
  generation all ship in the box. A comparable Fastify/Express app pulls in dozens of
  plugins (`@fastify/cookie`, `@fastify/cors`, `@fastify/jwt`, `@fastify/rate-limit`,
  `fastify-helmet`, a validation lib, an ORM, a template engine, a mailer…), each its own
  version, changelog, and supply-chain risk. Zenith has **no `node_modules`** and no
  third-party runtime dependencies to audit.

- **No imports, no boilerplate wiring.** Everything in `vn_modules/` is automatically in
  scope. You write `new_app()` and `app.get(...)` — no `require`/`import`, no plugin
  registration order to get right, no `app.use(...)` ceremony.

- **Secure by default, not by opt-in.** Templates HTML-escape (`<%= %>`) unless you
  explicitly ask for raw output; session cookies are `HttpOnly; SameSite=Lax` and
  JWT-signed (a forged secret yields `null`); DB drivers use bound parameters; secret
  comparisons can use `auth.constant_time_eq`; `smtp.send` blocks header injection. In
  Express you reach the same posture only after correctly wiring `helmet`, `csurf`,
  `express-session` + a store, and remembering to escape output yourself.

- **A compile-time ORM with no runtime cost.** `db.vn`'s query builder runs inside
  `comptime`, so SQL strings and parameter counts are produced and checked *at compile
  time* — `bind()` throws before anything reaches the driver if the param count is wrong.
  Fastify/Express bolt on Prisma/Knex/TypeORM, which build queries at runtime and add
  startup + per-query overhead.

- **Validation as declarative decorators.** `@is_email`, `@min_len(n)`, `@is_uuid` etc.
  go directly on struct fields (see `docs/LANGUAGE.md`), instead of hand-writing JSON
  Schema objects or Zod/Joi pipelines.

- **The toolchain is the language, not five more packages.** `vn test` (with mocking,
  `--filter`, `--timeout`), `vn lint` (including security rules that flag concatenated SQL
  and hardcoded secrets), and `vn fmt` are all built into the `vn` binary. No
  jest + supertest + eslint + prettier + their config files and peer-dependency conflicts.

- **OpenAPI for free.** The `summary` you pass to each route feeds a generated OpenAPI
  document — no decorator soup or separate spec file to keep in sync.

- **True multi-core out of the box.** Zenith clusters across threads with independent
  per-thread VMs, plus SIMD request parsing (picohttpparser), `writev` single-syscall
  responses, and a per-request struct arena that skips the GC entirely. Node is
  single-threaded per process — you run the `cluster` module or PM2 to use your cores, and
  still pay V8 GC on every request object.

- **Deploys as one self-contained binary.** `make release` produces a single hardened
  executable (`docs/SECURITY.md`). No runtime to install on the box, no lockfile to
  reconcile, no "works on my Node version" drift.

- **An escape hatch for the long tail.** Anything not built in (S3, niche SDKs) is one
  `python.run(module, fn, args)` call away via the Python bridge, so "batteries included"
  never becomes "stuck when you need something exotic."

Where Zenith does **not** lead: raw single-endpoint throughput trails Go/Rust, the
ecosystem is young, and it is **not** a sandbox for untrusted code (`docs/SECURITY.md`).
The trade is deliberate: dramatically less glue code and supply-chain surface, for a young
runtime you're betting on.

## Building an app

```varian
struct User { id: string, name: string }
let users = [User { id: "1", name: "Alice" }, User { id: "2", name: "Bob" }]

let app = new_app()

app.add_middleware(|req, next| {
    print("saw:", req.method, req.path)
    return next(req)
})

app.get("/", |req| {
    return Response { status: 200, body: "Welcome to Zenith!", content_type: "text/plain" }
}, "Root endpoint")

app.get("/api/users/:id", |req| {
    for i in 0..users.len() {
        if users[i].id == req.params.id {
            return Response { status: 200, body: json_encode(users[i]), content_type: "application/json" }
        }
    }
    return Response { status: 404, body: "User not found", content_type: "application/json" }
}, "Get user by ID")

app.listen(8080)
```

`new_app()` returns a real, independent `ZenithApp` struct instance — you can build
several apps with separate route tables and middleware stacks in the same program (no
shared module-level globals). `app.get/post/put/delete(path, handler, summary)`
registers a route; `path` segments starting with `:` (e.g. `:id`) become named params
available as `req.params.id` inside the handler. `summary` is a free-text string used
only for the generated OpenAPI doc.

## Routing & Performance

Routes are stored in a radix/segment trie (`RadixNode` in `zenith.vn`) keyed on
`METHOD + path`, so a lookup costs one trie descent per path segment rather than scanning
every registered route. Param segments (`:id`) are matched as a wildcard child node at
each level.

With the combined implementation of the **per-request struct arena** and **computed-goto bytecode dispatch**, Zenith single-process throughput achieves **10.5k req/sec** on a plaintext benchmark handler, representing a ~40% throughput increase over the base GC-heap allocation model.

## Middleware

```varian
app.add_middleware(|req, next| {
    let start = time_like_thing()
    let resp = next(req)
    print("took", "...")
    return resp
})
```

Each middleware is `|req, next| { ... return next(req) }` — a real closure capturing
`next`, which itself is a closure over the remaining middleware chain and the eventual
route handler. There is no global "current app" or "current next" slot; the whole chain
is built and passed explicitly through `_run_middleware`, which is why multiple `app`
instances don't interfere with each other.

`vn_modules/shield.vn` ships ready-made security middleware: `cors(origins, methods,
headers)`, `csrf()`, and `rate_limit(max_reqs, window_ms)` /
`rate_limit_redis(conn, max_reqs, window_seconds)`. Add them with `app.add_middleware(...)`.

## Global Error Handler

You can define a global error handler callback on the application using `app.on_error(|err, req| { ... })`. Any uncaught error during middleware execution or inside route handlers will be caught and passed to the callback. This lets you construct custom error pages, format JSON API error responses, and log exceptions systematically.

```varian
app.on_error(|err, req| {
    // err is a structured error containing kind, message, and hint.
    return Response {
        status: 500,
        body: json_encode({
            error: "An unexpected error occurred",
            kind: err.kind,
            message: err.message
        }),
        content_type: "application/json"
    }
})
```

If no custom error handler is registered, Zenith falls back to a default clean JSON 500 response (`{"error": "Internal Server Error", "kind": "..."}`) to prevent leaking framework/VM internals.

## Request & response helpers

Reading from the request (all take sensible defaults and URL-decode for you):

```varian
query(req, "page", "1")     // ?page=2 -> "2"   (single query param)
query_params(req)           // whole query string as a struct
form(req, "email", "")      // application/x-www-form-urlencoded body field
form_params(req)            // whole form body as a struct
cookie(req, "sid", "")      // a single request cookie
cookies(req)                // all cookies as a struct
```

Building responses:

```varian
redirect("/login")                  // 302 with Location
redirect_with("/login", 301)        // explicit status
json_response(value, 200)           // JSON body + content_type, any struct/array/primitive
with_header(resp, "X-Trace", id)    // returns a copy of resp with an extra header
set_cookie(resp, "sid", v, null)    // adds Set-Cookie: ...; Path=/; SameSite=Lax; HttpOnly
clear_cookie(resp, "sid")           // expires the cookie
```

Signed sessions (JWT-backed, tamper-evident — a wrong/forged secret yields `null`):

```varian
let resp = session_set(resp, data_struct, secret, null)   // sets a signed `session` cookie
let data = session_get(req, secret)                       // struct, or null if absent/forged
let resp = session_clear(resp)                            // logs the user out
```

## Templates

`render(template, ctx)` renders an ERB-style template string against a context struct, and
`render_response(template, ctx)` wraps the result in a `text/html` `Response`. Delimiters:

- `<%= expr %>` — interpolate, **HTML-escaped** (safe by default).
- `<%- expr %>` — interpolate **raw/unescaped** (only for trusted content).
- `<% if cond %>...<% else %>...<% endif %>` — conditionals.
- `<% for x in items %>...<% endfor %>` — loops.

```varian
render("Hi <%= name %>", ctx)                     // escapes name
render("<% for x in xs %>[<%= x %>]<% endfor %>", ctx)
```

(ERB-style `<% %>` is used rather than `{{ }}` because `{...}` is string interpolation at
lex time in Varian — see `docs/LANGUAGE.md`.)

## Testing without a socket

```varian
fn fake_req(method, path) {
    return http.create_struct(["method", "path", "body", "json"], [method, path, "", null])
}

print(app.handle(fake_req("GET", "/api/users/2")).body)
```

`app.handle(req)` runs routing + middleware + handler synchronously and returns a
`Response`, with no real HTTP socket involved — this is the basis for testing route
behavior directly (see `examples/zenith_app_test.vn`). `app.listen(port)` is the
production entry point: it wraps `app.handle` in `http.serve(port, |req| {
return self.handle(req) })`.

## OpenAPI docs

```varian
app.enable_docs("/docs")
```

Adds two routes: `GET /openapi.json` (the spec, built from real structs/arrays and
serialized once via `json_encode` — not hand-concatenated JSON strings) and `GET /docs`
(a Swagger UI page pointed at `/openapi.json`). The spec only reflects routes registered
*before* `enable_docs()` is called that have a non-empty `summary`.

## The comptime ORM (`vn_modules/db.vn`)

The core idea: a query's *shape* (table, selected fields, which fields appear in `WHERE`
and with what operator, limit/offset, SQL dialect) is almost always static in your code —
only the bound *values* are runtime data. So shape-building is split out into pure
functions of shape alone, which means they're safe to run inside `comptime { ... }`
(see `docs/LANGUAGE.md`) and the SQL text gets baked into the program once, at
compile-position evaluation time, instead of being rebuilt on every call.

```varian
let compiled = comptime {
    select("users")
        .fields(["id", "name", "email"])
        .where("id", "=")
        .where("status", "=")
        .limit(10)
        .offset(0)
        .build()
}
// compiled.sql == "SELECT id, name, email FROM users WHERE id = ? AND status = ? LIMIT 10 OFFSET 0"
// compiled.param_count == 2

let conn = sqlite.connect(":memory:")
let bound = bind(compiled, [1, "active"])   // throws if the param count doesn't match
let rows = run_sqlite(bound, conn)
```

- `select(table)` returns a `QueryBuilder`. Fluent methods: `.fields(arr)`,
  `.where(field, op)` (note: **no value** — just the field name and comparison operator,
  e.g. `"="`/`">"`; the value is supplied later via `bind`), `.limit(n)`, `.offset(n)`,
  `.use_dialect("postgres")` (changes placeholder style from `?` to `$1, $2, ...`),
  `.paginate(page, per_page)`, `.cursor_paginate(field, has_cursor, limit)`,
  `.find_by_key(field)` (shorthand for `.where(field, "=").limit(1)`), `.build()` →
  `CompiledQuery { sql, param_count, dialect }`.
- `bind(compiled, params)` attaches runtime values, checking `params.len() ==
  compiled.param_count` and throwing before anything reaches the driver if not.
- `run_sqlite(bound, conn)` / `run_postgres(bound, conn)` execute it.

You don't have to use `comptime` — `select(...)...build()` works identically as an
ordinary runtime call if the shape genuinely depends on a runtime value (e.g. an
admin-configurable field list). `comptime` is purely an optimization for the common case
where it doesn't.

## Lumen — server-driven live components

[Lumen](planning/LUMEN_PLAN.md) is the frontend framework built on top of Zenith. A Lumen
component renders HTML on the **server** (via Zenith routes), and a tiny embedded JS
runtime forwards browser events over a **WebSocket** so the server re-renders and morphs
the DOM — no page reload, no client-side state, no Varian in the browser.

```
lumen_component(state_fn, render_fn, handler_names, handler_fns)
lumen_mount(app, path, component)
```

- `GET <path>` serves the initial HTML (rendered + shell with client script).
- `GET <path>/live` upgrades to a WebSocket.
- Events flow as JSON: `{"t":"event","h":"<handler>","v":"<value>"}` up,
  `{"t":"html","html":"<HTML>"}` down.

See `examples/lumen_counter.vn` and `tests/lumen_live_test.vn`.

## Things intentionally *not* built as native C modules ("Untouchables")

Cloud SDKs (S3/R2/GCS, etc.) are not getting dedicated native modules — the existing
`python.run(module, fn, args)` bridge (see `docs/STDLIB.md`) already lets you call
`boto3` or any other Python package directly, which is far less maintenance than
hand-rolling AWS auth/signing in C for a feature most programs won't use.
