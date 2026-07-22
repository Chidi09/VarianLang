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
| **Language** | JS/TS across separate browser and server runtimes | **Varian on the server** with HTML and directive-selected browser actions |
| **Client bundle** | Webpack/Vite bundles a component runtime | Static pages emit no optional action code; interactive pages emit only detected Lumen actions |
| **Build pipeline** | `npm run build` plus a JS bundler | `vn dev` or `vn build main.vn` composes Lumen routes with the Zenith entry |
| **Data loading** | `getServerSideProps` / `loader` / server actions | **Remix-style `load(req)` and `action(req)`** — auto-hydrated states on GET and WebSocket loops |
| **API + pages** | Separate `app/api/` and `app/` directories | Same `main.vn`, same `ZenithApp` instance |
| **Background jobs** | External workers (Bull, Sidekiq) | Durable local SQLite <code>vn_jobs</code> queue with bounded retries; use an external broker for distributed workers |
| **Swagger docs** | Manual setup or `next-swagger-doc` plugin | Automatic — `app.enable_docs("/docs")` |
| **Security middleware** | Manual — helmet, cors, csurf, express-rate-limit | `cors()`, `rate_limit()`, `csrf()` from `shield.vn` — built in |
| **Deploy** | Node.js runtime + `node_modules` required | **Single native binary** — no runtime, no deps |
| **Scaffold** | `npx create-next-app` → prompts, installs, builds | `vn new myapp` — instant, one command, no downloads |

### BFF by default

Aurora is a **Backend for Frontend (BFF)** by nature. Because Varian is a single language that compiles to a native binary, the backend API and frontend UI live in the same process, share the same types, and run seamlessly without serializing data schemas.

Concretely, every Lumen page component can declare a `load(req)` and `action(req)` function:

```varian
// pages/shop.lumen
// Inside your <script> tag:

fn load(req) {
    let q = query(req, "q", "")
    let rows = sqlite.query(conn, "SELECT * FROM products WHERE name LIKE ?", ["%" + q + "%"])
    return { products: rows, query: q }
}

fn action(req) {
    let item_id = req.json.id
    sqlite.query(conn, "INSERT INTO cart_items (product_id) VALUES (?)", [item_id])
    return redirect("/cart")
}
```

This structure:
- Runs `load(req)` **on the initial page GET** — SSR with hydrated data.
- Ships data state transparently to the browser to bootstrap `/live` WebSocket connections.
- Automatically handles POST requests using the component's `action(req)` block.
- Integrates folder-level hierarchies including `layout.lumen` nesting and nearest `loading.lumen` fallback states.

```sh
vn new myapp          # Scaffold a full Aurora project
cd myapp
vn dev                # http://localhost:8090 — live reload
vn build main.vn --release    # Compile the composed app to a native binary
```

Aurora is **not a separate framework**. It is a project structure convention that the
toolchain (`vn new`, `vn dev`, `vn build`) recognises. A `constellation.toml` with
`kind = "aurora"` tells Kiln to:

- Compile `.lumen` pages via the Lumen build pass
- Embed `public/` assets into the output binary
- Generate `aurora_mount_pages(app)` and compose it with `main.vn`
- Produce a single runnable artifact (`.vnb` or native binary)

---

## Reference storefront

The `aurora-chat/` directory at the repository root is a larger **demonstration application** that
exercises nearly every capability the Varian stack exposes — SQLite, auth, sessions,
background jobs, email, API routes, Lumen SSR pages, Swagger docs, rate limiting, and
more.

### Quick start

```bash
cd aurora-chat
../vn run build_pages.vn    # Compile .lumen pages → .gen/pages.vn
../vn run main.vn           # Start integrated API + page server
# Open http://localhost:8080
```

### Architecture

```
aurora-chat/
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

### Route guards

An optional `pages/+guard.vn` protects the complete page tree; nested guards such as
`pages/account/+guard.vn` or `pages/teams/[team]/+guard.vn` refine a subtree. Each file
defines `fn guard(req)` and returns `null` to continue or a normal Zenith response to stop.
Aurora compiles guards into the composed application, so authorization executes in the
same process with the same request context and no per-request source evaluation.
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

Aurora composes **Lumen + Zenith** in one Varian process behind a single manifest convention
(`kind = "aurora"`). Applications can use the built-in database, validation, authentication,
queue, mail, storage, logging, and security modules without a package-manager dependency tree.

| Capability | Next.js + Express | Aurora |
|---|---|---|
| **What you install** | `npx create-next-app` → 300 MB `node_modules` | `vn new myapp` → **zero downloads** |
| **Language** | JS/TS (client) + JS/TS (server) + SQL (DB) | **Varian everywhere** |
| **Client framework** | React client runtime where interactive | Static export can use **zero JS**; live pages use a core capped below **4.1 KB uncompressed** plus selected actions |
| **Server framework** | Express / Fastify + 16+ packages | **Zenith** — built in |
| **Router** | React Router + Express Router | **One radix trie** — client + server |
| **Auth** | Authentication package + session store | **Built-in** — `zenith_auth.jwt()`, `zenith_auth.session_store()`, `zenith_auth.session()` and password helpers |
| **ORM** | Prisma / Drizzle / Knex | **Built-in** — comptime `select().where().build()`, zero runtime cost |
| **Background jobs** | External queue service or process | **Built-in** — durable SQLite named jobs, bounded retries, worker pools, and `cron()` |
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

Administrative queue controls are not exposed automatically. After installing the
application's authentication and authorization middleware, opt in with
`app.enable_job_dashboard("/operations/jobs")`. The dashboard uses the database selected
by `queue_configure(...)` and is entirely server-rendered, so it adds no browser runtime.
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
