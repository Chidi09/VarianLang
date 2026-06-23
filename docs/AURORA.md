<p align="center">
  <img src="assets/aurora-logo.png" alt="Aurora" width="150" />
</p>

# Aurora — the full-stack framework

Aurora is Varian's **full-stack framework** — the layer that binds **Zenith** (HTTP server)
and **Lumen** (frontend) into a single, unified platform. It is Varian's answer to Next.js,
Nuxt, and Rails.

Think of it like Next.js: Next.js bundles React (frontend) together with its own Node server
(backend) under one convention. Aurora does the same — it bundles Lumen and Zenith under a
single project structure, manifest convention (`kind = "aurora"`), and build pipeline.

```
    Aurora (full-stack framework)
      ├── Lumen  ── frontend framework  (.lumen components, Lumen JS)
      └── Zenith ── HTTP server          (routes, middleware, API)
```

When you run `vn new myapp`, you scaffold an **Aurora project** — a full-stack app with
`pages/` (Lumen frontend), `main.vn` (Zenith backend), and `lib/` (shared modules). When
you run `vn dev`, the dev server banner reads **"Aurora — fullstack Varian platform"**.

### Why Aurora over Next.js / Nuxt

| Concern | Next.js / Nuxt | Aurora (Zenith + Lumen) |
|---|---|---|
| **Language** | JS everywhere, but client-side + server-side runtimes differ | **Varian everywhere** — same language, same runtime, same binary |
| **Client bundle** | Webpack/Vite bundles React/Vue SPA → hundreds of KB JS | **Zero client JS** by default — ~2 KB inline **Lumen JS** script |
| **Build pipeline** | `npm run build` → bundler, code-split, tree-shake, optimize | `vn run` — no bundler, no build step for the frontend |
| **Data loading** | `getServerSideProps` / `loader` / server actions | `lumen_mount_data()` — data provider runs on GET + WS reconnect |
| **API + pages** | Separate `app/api/` and `app/` directories | Same `main.vn`, same `ZenithApp` instance |
| **Background jobs** | External workers (Bull, Sidekiq) | `WorkerPool.spawn()` + `cron()` — built in |
| **Swagger docs** | Manual setup or `next-swagger-doc` plugin | Automatic — `app.enable_docs("/docs")` |
| **Security middleware** | Manual — helmet, cors, csurf, express-rate-limit | `cors()`, `rate_limit()`, `csrf()` from `shield.vn` — built in |
| **Deploy** | Node.js runtime + `node_modules` required | **Single native binary** — no runtime, no deps |
| **Scaffold** | `npx create-next-app` → prompts, installs, builds | `vn new myapp` — instant, one command, no downloads |

### BFF by default

Aurora is a **Backend for Frontend (BFF)** by nature, not by configuration. Because Varian
is a single language that compiles to a single binary, the backend API and the frontend UI
live in the same process, share the same types, and are authored in the same language.

Concretely, every Lumen page can declare a **data provider** via `lumen_mount_data()`:

```varian
// aurora/lib/pages.vn — the provider runs server-side, shapes DB data for the component
lumen_mount_data(app, "/shop", shop_comp, |req| {
    let q = query(req, "q", "")
    let rows = sqlite.query(conn, "SELECT * FROM products WHERE name LIKE ? ORDER BY id", ["%" + q + "%"])
    let items = []
    for i in 0..rows.len() {
        let p = rows[i]
        items = items.push(http.create_struct(
            ["id","name","price","stock","image"],
            [p.id, p.name, format_price(p.price_cents), p.stock, p.image]))
    }
    return http.create_struct(["hasProducts", "products", "query"], [items.len() > 0, items, q])
})
```

This provider:
- Runs **on the initial page GET** — SSR with real data
- Runs **on every WebSocket reconnect** — so live interactivity stays data-backed
- Receives the **full request** — access to cookies, session, query params, path params
- Returns **exactly the shape the component needs** — no over-fetching, no under-fetching

There is no separate BFF service to deploy, no network hop between frontend and data, and
no duplicated types across a language boundary. The JSON API routes (`/api/products`,
`/api/cart`, etc.) that the Lumen pages call are also BFF endpoints — they speak the
frontend's language because they're written by the same developer in the same file.

```sh
vn new myapp          # Scaffold a full Aurora project
cd myapp
vn dev                # http://localhost:8090 — live reload
vn build --release    # Compile to a native binary
```

Aurora is **not a separate framework**. It is a project structure convention that the
toolchain (`vn new`, `vn dev`, `vn build`) recognises. A `constellation.toml` with
`kind = "aurora"` tells Kiln to:

- Compile `.lumen` pages via the Lumen build pass
- Embed `public/` assets into the output binary
- Wire the Zenith app with middleware, API routes, and page mounts
- Produce a single runnable artifact (`.vnb` or native binary)

---

## Reference storefront

The `aurora/` directory at the repository root is a **demonstration storefront** that
exercises nearly every capability the Varian stack exposes — SQLite, auth, sessions,
background jobs, email, API routes, Lumen SSR pages, Swagger docs, rate limiting, and
more.

### Quick start

```bash
cd aurora
../vn run build_pages.vn    # Compile .lumen pages → .gen/pages.vn
../vn run main.vn           # Start integrated API + page server
# Open http://localhost:8080
```

### Architecture

```
aurora/
  main.vn                  # Entry: use lib/*, build app, wire, listen
  build_pages.vn           # Compile .lumen pages → .gen/pages.vn
  lib/
    config.vn              # Centralized env-based configuration
    db.vn                  # DB init, schema, seed, helpers
    middleware.vn          # Log, CORS, rate-limit middleware
    api_products.vn        # /api/products[/:id] routes
    api_cart.vn            # /api/cart routes with session-backed cart
    api_orders.vn          # /api/checkout — order creation
    auth.vn                # /api/register, /api/login, /api/logout
    jobs.vn                # Email worker pool, cron sweep
    pages.vn               # Mount compiled Lumen pages + SSR product route
  pages/
    index.lumen            # Homepage with featured products grid
    shop.lumen             # Catalog grid
    cart.lumen             # Cart with line items
    checkout.lumen         # Checkout form
    login.lumen            # Sign-in form
    account.lumen          # Session-gated order history
    product/
      detail.lumen         # SSR product detail (hand-registered route)
    components/
      Nav.lumen, Footer.lumen, ProductCard.lumen, Price.lumen,
      QtyStepper.lumen, CartBadge.lumen
```

### Routes

| Route | Capability | Type |
|---|---|---|
| `/` | Index — `<Page>`/`<Section>`/`<Grid>` vocabulary | Lumen page |
| `/shop` | Catalog — `<Grid>` of `<ProductCard>` | Lumen page |
| `/cart` | Cart — `{{#each}}` with session data | Lumen page |
| `/checkout` | Checkout — form layout with `<Card>` sections | Lumen page |
| `/login` | Login — auth session integration | Lumen page |
| `/account` | Order history — session-gated | Lumen page |
| `/product/:id` | SSR product detail — DB query → render | SSR Lumen page |
| `/api/products` | Product listing with pagination | JSON API |
| `/api/products/:id` | Single product | JSON API |
| `/api/cart` | Session-backed cart read | JSON API |
| `/api/cart/:id` | Add/remove cart items | JSON API |
| `/api/checkout` | Validate, create order, enqueue email | JSON API |
| `/api/register` | User registration | JSON API |
| `/api/login` | Login | JSON API |
| `/api/logout` | Logout | JSON API |
| `/docs` | Swagger UI for the JSON API | Docs |

### Configuration (all via environment variables)

| Variable | Default | Description |
|---|---|---|
| `PORT` | `8080` | Server port |
| `SESSION_SECRET` | random | Cookie signing key |
| `DB_PATH` | `aurora.db` | SQLite database path |
| `PER_PAGE` | `12` | Products per page |
| `RATE_LIMIT_MAX` / `RATE_LIMIT_WINDOW_MS` | `100` / `60000` | Rate limiting |
| `CRON_INTERVAL_MS` | `60000` | Cron sweep interval |
| `EMAIL_POOL_SIZE` | `4` | Worker pool size |
| `SMTP_HOST` / `SMTP_PORT` / `SMTP_USER` / `SMTP_PASS` | — | SMTP settings |
| `RESEND_API_KEY` | — | Resend API key alternative |

---

## How Aurora relates to Lumen and Zenith

| | Lumen | Zenith | Aurora |
|---|---|---|---|
| **What** | Frontend framework — `.lumen` components, server-driven DOM | HTTP web framework — router, middleware, WebSocket | Full-stack framework binding Lumen + Zenith together |
| **Scaffold** | `vn lumen new myapp` — `pages/` + `public/` only | N/A (imported via `new_app()`) | `vn new myapp` — `main.vn`, `pages/`, `lib/`, `public/` |
| **Like** | React / Vue | Express / FastAPI | **Next.js / Nuxt** |
| **Output** | Compiled `.lumen` → `.vn` pages | Part of a Varian binary | Single binary (`.vnb` or native) with embedded assets |
| **Use case** | A reactive frontend served by any backend | HTTP server in any Varian program | A complete production web application |

---

---

## What Aurora ships at once

Aurora is **Lumen + Zenith + every built-in module** behind a single manifest convention
(`kind = "aurora"`). Where Next.js needs Next + React + React Router + your own API server
+ Bull + Prisma + Zod + Winston + your own auth + your own email + your own rate limiter,
Aurora ships a single binary that is all of those things.

| Capability | Next.js + Express | Aurora |
|---|---|---|
| **What you install** | `npx create-next-app` → 300 MB `node_modules` | `vn new myapp` → **zero downloads** |
| **Language** | JS/TS (client) + JS/TS (server) + SQL (DB) | **Varian everywhere** |
| **Client framework** | React (400 KB gzipped) | **~2 KB Lumen JS** inline |
| **Server framework** | Express / Fastify + 16+ packages | **Zenith** — built in |
| **Router** | React Router + Express Router | **One radix trie** — client + server |
| **Auth** | `next-auth` / `jsonwebtoken` + session store | **Built-in** — `auth.jwt()`, `auth.session_store()`, `auth.sha1_base64()` |
| **ORM** | Prisma / Drizzle / Knex | **Built-in** — comptime `select().where().build()`, zero runtime cost |
| **Background jobs** | Bull / Sidekiq + Redis | **Built-in** — `WorkerPool.spawn()`, `cron()` |
| **Email** | Nodemailer / Resend SDK | **Built-in** — `send_smtp()`, `send_resend()` |
| **File storage** | multer / boto3 SDK | **Built-in** — `Storage.put()/.get()/.delete()` |
| **Structured logging** | Winston / Pino | **Built-in** — JSON `Logger.info_with()` |
| **Prometheus metrics** | prom-client | **Built-in** — `metrics_handler()` |
| **Input validation** | Zod / Joi | **Built-in** — `validate.str().is_email().parse()` |
| **Rate limiting** | express-rate-limit | **Built-in** — in-memory + Redis |
| **CSRF** | csurf / manual | **Built-in** — `csrf()` double-submit cookie |
| **CORS** | cors package | **Built-in** — `cors()` |
| **Swagger docs** | swagger-jsdoc + swagger-ui | **Built-in** — `app.enable_docs("/docs")` |
| **Python bridge** | Subprocess / n/a | **Built-in** — `python.run()` for S3/R2/GCS SDKs |
| **Deploy** | Node.js runtime + `node_modules` | **Single native binary** — `vn build --release` |
| **Total packages** | **30+** (React + Next + Express + Prisma + Zod + Bull + Winston + cors + helmet + csurf + express-rate-limit + jsonwebtoken + nodemailer + multer + swagger-jsdoc + prom-client + …) | **1 binary** |

### Engineering patterns Aurora proves

- **Vertical slice architecture** — Every `aurora/lib/*.vn` is an end-to-end feature:
  `api_products.vn` owns the route, the handler, the DB query, and the response for
  products. `auth.vn` owns register/login/logout. Adding a feature means adding one file
  (`aurora/lib/api_cart.vn:1-70`), not touching controllers, services, repositories across
  five directories.
- **BFF by default** — `lumen_mount_data()` (`lumen.vn:1564-1584`) runs a provider callback
  on both the initial page GET and every WebSocket reconnect, shaping DB data exactly for
  each component. The JSON API routes (`/api/products`, `/api/cart`) are BFF endpoints
  written in the same language, same file as the pages that consume them. No separate BFF
  service to deploy, no network hop, no type duplication across a language boundary.
- **Stateless execution** — All handler functions receive state and return new state
  (immutable update pattern). Sessions are stateless JWT cookies, not server-side stores.
- **Structured configuration as feature flags** — `aurora/lib/config.vn` centralizes all
  env-driven toggles (`PORT`, `RATE_LIMIT_MAX`, `CRON_INTERVAL_MS`, `EMAIL_POOL_SIZE`,
  `SMTP_HOST`, `RESEND_API_KEY`, etc.). Deploy once, toggle without redeploying.
- **Guard clauses everywhere** — Every API handler starts with `if x == null { return err_res(...) }`
  before any business logic — fail fast, keep the happy path flat
  (`aurora/lib/api_products.vn:29`, `aurora/lib/api_cart.vn:34`, `aurora/lib/api_orders.vn:8`).
- **Parse, don't validate** — `db.vn`'s `compile_select` parses query shapes at compile
  time via `comptime { }`. The SQL string is baked into the binary; `bind()` enforces the
  exact parameter count before execution. No invalid SQL ever reaches the driver.

---

## See also

- [Zenith — the web framework](ZENITH.md)
- [Lumen — the frontend framework](LUMEN.md)
- [Kiln — the build tool](KILN.md)
