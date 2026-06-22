# Aurora — punch-list (post-review fixes)

A review of the first build found that **Phase −1 (caveat fixes C1–C5) is done and
good**, and the tests/smoke-tests genuinely pass — but the **Aurora app itself is a
skeleton** that doesn't actually demonstrate the integrated, reactive full-stack
shop the plan specified. Do **not** re-run the whole plan. Work this list top to
bottom. After each Part, run its verification (memory-capped:
`ulimit -v 9000000; ...`, never with ASan) and commit.

## What the review found (the gaps you're fixing)

- There are **two separate servers** — hand-written `server.vn` (JSON API) and the
  auto-generated `_gen_app.vn` (pages) — **both binding `:8080`**. They can't run
  together. There is no single integrated process.
- **Pages have zero reactivity** — no `@click`/`@submit` anywhere in `pages/`, and
  `lumen_store` / `lumen_resource` / `lumen_form` / `lumen_broadcast_store` are used
  in **0 page files** (they have passing unit tests, but the app never uses them).
- `server.vn` references **none** of: `mail`, `auth`/`session_middleware`/`jwt`, the
  `db` query builder, `storage`, `observe`, `csrf` (commented out), `validate`. The
  `cron` body is a `print` placeholder; the email pool is spawned but never given a
  job.
- **No dynamic SSR product page** (`GET /product/:id`). C4 added the *mechanism*
  (`lumen_mount_dynamic`), but the app never uses it.
- `checkout.lumen` doesn't validate or place an order; `login.lumen` has no auth
  backend; **`account.lumen` is missing**.

Net: "tests pass" is true; "exercises almost every ability, integrated and
reactive" is not. This list closes that.

---

## Global rule — NO HARDCODING (applies to every Part)

Data and configuration must flow from their real source, never from values baked
into the code. A reviewer will reject inline data, magic numbers, and committed
secrets. Concretely:

- **Config & secrets come from the environment**, centralized in `lib/config.vn`
  (`env.get("PORT", ...)`, `env.get("SESSION_SECRET", ...)`, `DB_PATH`, SMTP host/
  user/pass, `RESEND_API_KEY`, per_page, rate-limit numbers, cron interval). Nothing
  reads `env` outside `config.vn`. **Do not commit a real secret or a usable default
  secret** — if `SESSION_SECRET` is unset, generate a random one at startup and log a
  warning (not `"aurora-dev-secret-change-in-prod"`). No API keys, passwords, or hosts
  as string literals in any file.
- **Product/catalog/order data comes from the database**, never inline. Pages and
  components render whatever the `db` query returns — there must be **no hardcoded
  product arrays, names, prices, or stock counts in `.lumen` files or `lib/*.vn`
  route handlers**. The only place product literals are allowed is `db/seed.sql`
  (that's data-as-data, and seeding must be idempotent / skip when already seeded).
- **Prices/totals are computed**, not written as literals. Money is stored as integer
  cents in the DB; formatting lives in one place (the `Price` component / one helper),
  not repeated per page.
- **No magic numbers.** `per_page`, rate-limit count/window, cron interval, worker-pool
  size, JWT expiry, etc. are named values in `config.vn`, referenced by name.
- **No duplicated literals.** Route prefixes, table names, cookie names, and the
  product image/asset base each have a single definition that everything else
  references. If a string appears in two files, it belongs in `config.vn` (or a
  shared constant).
- **No placeholder/stub data standing in for a real path.** If something can't read
  its real source yet, it's not done — say so in the final report rather than faking
  a value (e.g. the cron sweep must act on real cart rows, not `print` a placeholder).

When in doubt: if changing a value would normally be a config/ops change rather than
a code change, it must not be a literal in the code.

---

## Part A — Break `server.vn` apart (it's too cluttered) and make it the single entry

`server.vn` is one ~200-line file doing DB init, seed, middleware, every route, the
queue, and `listen()`. It will only grow as the rest of this list lands, so split it
first. Varian supports local includes: **`use "lib/foo.vn"` resolves relative to the
run directory** (confirmed — `examples/main.vn` does `use "examples/math.vn"`). Run
the app from `aurora/`.

**Decomposition rule:** each `lib/*.vn` file defines **functions only** (no top-level
side effects), so include order never matters. `main.vn` `use`s them, then calls them
in a deliberate order. New layout:

```
aurora/
  main.vn                 # entry: use lib/*, build app, wire, listen  (~40 lines)
  lib/
    config.vn             # fn cfg() -> { db_path, port, session_secret, ... }
    db.vn                 # fn db_init(path) -> conn   (connect + CREATE TABLE + seed-if-empty)
    middleware.vn         # fn install_middleware(app)  (observe log → cors → rate_limit → session)
    api_products.vn       # fn register_product_api(app, conn)        (GET /api/products[/:id], uses db.select builder)
    api_cart.vn           # fn register_cart_api(app, conn)  + get_cart/save_cart helpers
    api_orders.vn         # fn register_order_api(app, conn)          (POST /api/checkout: validate → order → email job)
    auth.vn               # fn register_auth(app, conn)               (POST /login, /logout, session_middleware)
    pages.vn              # fn mount_pages(app)                       (compile + lumen_mount / lumen_mount_dynamic)
    jobs.vn               # fn start_jobs(app, conn) -> email_pool    (email worker pool + cron sweep)
  build_pages.vn          # compiles pages/*.lumen -> .gen/pages.vn (components only, no server)
```

`main.vn` reads, in order:

```
use "lib/config.vn"
use "lib/db.vn"
use "lib/middleware.vn"
use "lib/api_products.vn"
use "lib/api_cart.vn"
use "lib/api_orders.vn"
use "lib/auth.vn"
use "lib/jobs.vn"
use "lib/pages.vn"
use ".gen/pages.vn"          // generated Lumen component registrations (Part B)

let c = cfg()
let conn = db_init(c.db_path)
let app = new_app()
install_middleware(app)
register_product_api(app, conn)
register_cart_api(app, conn)
register_order_api(app, conn)
register_auth(app, conn)
let email_pool = start_jobs(app, conn)
mount_pages(app)             // mounts the compiled page components onto THIS app
app.enable_docs("/docs")
app.serve_static("/public", "public")
app.on_error(error_page)
app.listen(c.port)
```

Move the existing logic into the matching `lib/` file wrapped in its function; do not
change behavior in Part A beyond the split. **Delete `server.vn` once `main.vn`
reproduces it.** Keep helper structs/`Response` construction via `http.create_struct`
(no parse-order dependency).

**Verify (A):** `ulimit -v 9000000; vn run main.vn &`; the same `/api/products`,
`/api/products/:id`, `/api/cart` curls from before still return identical JSON; `/docs`
still serves; `ulimit -v 9000000; vn test tests/` still 105/105. Kill it.

---

## Part B — One integrated app: mount the Lumen pages onto `main.vn`'s `app`

Today pages are served by the throwaway `_gen_app.vn`, which builds *its own* app and
`http.serve`s. Instead, compile the pages to **component registrations only** and mount
them onto the app `main.vn` already owns.

1. `build_pages.vn`: compile every `pages/*.lumen` (and its imported
   `components/*.lumen` dependencies) into `.gen/pages.vn` containing **only** the
   `_lumen_init_component_*()` definitions + `lumen_register_component(...)` calls —
   **no `new_app`, no `http.serve`, no `lumen_mount`, no `serve_static`**. Reuse the
   existing Lumen compile path that `_lumen_build_dir` calls internally (per-file
   `lumen_compile_file` in a loop is the clean way to get components without the
   server tail); if the only available builder appends a server, strip that tail in
   `build_pages.vn` after generating. Verify `.gen/pages.vn` has zero `http.serve` /
   `new_app` lines.
2. `lib/pages.vn` → `fn mount_pages(app)`: for each page,
   `lumen_mount(app, "/", _lumen_get_component("Index"))`,
   `lumen_mount(app, "/shop", _lumen_get_component("Shop"))`, etc. Dynamic product
   page goes here too (Part C).
3. **Delete `_gen_app.vn` and `build.vn`** (its job is now `build_pages.vn`).
4. `vn.json` scripts: `dev` = compile pages then run main →
   `"../vn run build_pages.vn && ../vn run main.vn"` (or a tiny `run.vn` that does
   both). Keep `seed` working.

**Verify (B):** one process — `ulimit -v 9000000; vn run build_pages.vn && vn run
main.vn &` — serves **both** the pages (`curl /` and `/shop` contain `data-lumen-root`
+ the component text) **and** the API (`/api/products` JSON) on the same port. Only one
server binds the port. Kill it.

---

## Part C — Dynamic SSR product page (`GET /product/:id`)

Add `pages/product/[id].lumen` (or, if file-based dynamic routing from C4 is wired,
confirm it auto-registers; otherwise mount by hand in `mount_pages`). The component's
`state()` receives the `:id`, loads the product via the **db query builder**
(`db.select("products").where("id","=")…`), and renders it in the component vocabulary
(`<Page>/<Section>/<Stack>`, `<Price>`, an add-to-cart `<Button @click="add">`, live
stock `<Badge>`). Mount with `lumen_mount_dynamic(app, "/product/:id", comp, "id")`.

**Verify (C):** `curl /product/1` returns a 200 HTML page containing that product's
name and price; a bad id renders a friendly not-found, not a crash.

---

## Part D — Make the pages actually reactive (the headline feature)

No page currently has a single `@click`. Wire real server-driven reactivity:

1. **Cart store:** per-connection `lumen_store` mirrors the session cart;
   `ProductCard`'s add-to-cart `<Button @click="add">` and `QtyStepper`'s `@click`
   `inc`/`dec` mutate it; `CartBadge` and `cart.lumen` re-render live (DOM morph, no
   client JS). Session cookie remains the source of truth across navigations.
2. **Live stock across clients:** a top-level `lumen_broadcast_store("stock", …)`;
   add-to-cart decrements it via `lumen_publish`, so every open `ProductCard`'s stock
   `<Badge>` updates in real time. This is the headline "full-stack reactive" demo.
3. **Catalog data:** `index.lumen` / `shop.lumen` load products through
   `lumen_resource` (loading state in template), with `shop.lumen` filtering via
   `@input="filter"` and paginating through the db builder.
4. **Theme toggle** in `Nav`: `@click` flips `.dark`, persists via cookie.

**Verify (D):** scripted WebSocket test against a page's `/live` socket — send an
add-to-cart event, assert the server pushes an HTML/patch frame; a **second** connected
client also receives the stock-badge update (proves cross-connection `lumen_publish`).
Cart total updates on qty change.

---

## Part E — Wire the Zenith capabilities `server.vn` skipped

Each lands in its `lib/` module from Part A:

- **auth** (`lib/auth.vn`): `POST /login` validates credentials, sets a signed session;
  `session_middleware` (or the signed-cookie session already in use) gates
  `account`; `jwt_middleware` guards an admin-only `/api` route. Rate-limit `/login`.
- **mail** (`lib/jobs.vn` + `lib/api_orders.vn`): on successful checkout, **submit** an
  order-confirmation job to the email `pool` that calls `send_smtp` (or `send_resend`).
  The pool must actually receive jobs (today it's spawned but idle).
- **validate** (`lib/api_orders.vn` + `checkout.lumen`): server-side
  `validate.str().is_email()…` on the checkout POST, plus `lumen_form` inline field
  errors on the page.
- **db query builder** (`api_products.vn`): replace raw `sqlite.query("SELECT … LIKE …")`
  string-building with `db.select("products").where(...).paginate(page, per_page)`.
- **real cron** (`lib/jobs.vn`): the `cron(60000, …)` sweep does something real
  (e.g. logs/clears carts idle past a threshold), not a bare `print`.
- **csrf** (`lib/middleware.vn`): actually apply `csrf()` to form POSTs
  (login/checkout), not a comment. Keep JSON `/api` on CORS + content-type.
- **observe** (`lib/middleware.vn`): use the `observe` module for the request log
  instead of the hand-rolled `time.now_ms()`+`print`.
- **storage** (`lib/api_orders.vn`): write a generated invoice/receipt for each order
  via `new_storage(...).put(...)` and expose it (download link or `/api`).

**Verify (E):** `curl` shows: login sets a session and `/account` is gated; a checkout
POST with a bad email returns validation errors and creates no order; a good checkout
creates an order row **and** the email pool logs a sent confirmation; `/docs` lists the
new routes; csrf rejects a tokenless form POST.

---

## Part F — Add `account.lumen` (missing from the build)

Session-gated order history page in the component vocabulary, loading the current
user's orders via `lumen_resource` over the `orders` table. Redirect to `/login` when
no session.

**Verify (F):** logged-out `curl /account` redirects to `/login`; after login it lists
that user's orders.

---

## Final verification (whole list)

- One process serves API + all pages + dynamic product routes on a single port.
- End-to-end: home → shop → add to cart (live badge updates) → cart (qty changes
  recompute) → checkout (validates, creates order, enqueues email) → account shows the
  order.
- `ulimit -v 9000000; vn test tests/` green; Aurora smoke tests green.
- `vn build pages` (Kiln `.vnb`) and `vn build pages --release` (native) both succeed
  on the restructured app.
- Update `aurora/README.md` to the new `main.vn` + `lib/` layout and map each page to
  the capability it demonstrates.
- Record anything that genuinely couldn't be done (with the reason) — do not silently
  drop a matrix item.
