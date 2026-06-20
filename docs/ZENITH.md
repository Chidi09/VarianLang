# Zenith — the web framework

Zenith (`vn_modules/zenith.vn`) and the comptime ORM (`vn_modules/db.vn`) are not native
modules — they're plain Varian source. Every file under `vn_modules/` is automatically
concatenated as a prelude in front of whatever file you run (see `docs/TOOLING.md`), so
`ZenithApp`, `new_app()`, etc. are just always in scope, with no `import` needed.

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

## Things intentionally *not* built as native C modules ("Untouchables")

Cloud SDKs (S3/R2/GCS, etc.) are not getting dedicated native modules — the existing
`python.run(module, fn, args)` bridge (see `docs/STDLIB.md`) already lets you call
`boto3` or any other Python package directly, which is far less maintenance than
hand-rolling AWS auth/signing in C for a feature most programs won't use.
