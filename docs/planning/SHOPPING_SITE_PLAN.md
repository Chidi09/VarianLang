# Aurora — a full-stack shopping site that exercises all of Zenith + Lumen

## Read this first (instructions for whoever implements this plan)

This plan is executed by a fast/cheaper model and reviewed afterward by a more
capable one, so **precision now matters more than speed**. Follow it literally.

- The goal is a real, runnable storefront — *Aurora* — that uses **almost every
  capability** Zenith (backend) and Lumen (frontend) expose. It doubles as a
  living demo and an integration test of the whole stack.
- **Writing style is a hard requirement, not a preference.** Pages are written
  in the Lumen **component vocabulary** (`<Page>`, `<Section>`, `<Stack>`,
  `<Grid>`, `<Card>`, `<Heading>`, `<Text>`, `<Button>`, `<Badge>`, `<Feature>`,
  …) plus small **custom components**, and `<style scoped>` for the rare bit of
  page-specific CSS. **Do not write pages as raw `<div>` soup with inline
  `style="…"`.** A reviewer will reject any page that reads like hand-written
  HTML instead of Lumen. The one exception is *inside* a leaf custom component
  where a primitive isn't available — keep it minimal and put it in `<style
  scoped>`, not inline.
- **Phase −1 (the caveats) comes first of all.** Before any feature work, attempt
  the root fixes in "Phase −1 — Known caveats / gaps to attempt first" below.
  These are real limitations spotted while writing this plan; fixing them makes
  the rest of Aurora honest rather than worked-around. Per "no half measures,"
  **attempt each at the root**; if one is genuinely too large for this pass,
  implement the documented fallback, leave a clear `// CAVEAT:` note, and flag it
  for the human — do not silently skip it.
- **Phase 0 is mandatory and comes right after Phase −1.** Several Lumen helpers
  (`lumen_store`, `lumen_resource`, `lumen_form`, `lumen_broadcast_store`,
  dynamic product routes) are present in `vn_modules/lumen.vn` but only lightly
  exercised. Before building on any of them, prove each one works with a tiny
  smoke test (Phase 0). If one is broken, **fix it at the root** (architecturally
  correct, in `vn_modules/*.vn` or the C core as appropriate) — do **not** route
  around it with a workaround. Report what you fixed.
- **After each phase, run that phase's verification before moving on.** If
  something fails, stop and report the exact error rather than guessing.
- File/line references were correct when written; the codebase moves fast, so
  re-`grep` the cited symbol before relying on a line number.
- **Memory caps are mandatory in every test run:** prefix run commands with
  `ulimit -v 9000000` (a prior uncapped run OOM-crashed the machine). This is
  incompatible with ASan builds — never combine the two.
- Commit after each phase passes (the human owns final commits; stage and
  describe, don't push).

---

## Phase −1 — Known caveats / gaps to attempt first (root fixes)

These were spotted while writing this plan. They are ordered by how much they
hurt a "truly full-stack" production app. Attempt each at the root. For each:
do the fix, add/extend a test, run `ulimit -v 9000000; vn test tests/`, and
record the outcome (FIXED / FALLBACK / DEFERRED + why) in
`docs/planning/SHOPPING_SITE_CAVEATS.md`.

### C1 — Outbound HTTP client is blocking (hurts the BFF fan-out story)

**What:** `http.get` / `http.post` (`src/lib_http.c`, the `curl_easy_perform`
calls at ~lines 71 and 115) are **synchronous**. Zenith's scheduler is
cooperative single-threaded green threads, so one slow downstream call blocks the
*entire worker* (and every connection it serves) until it returns or hits
libcurl's 30s timeout. A BFF whose whole job is to fan out to downstream services
will stall under load.

**Why it matters:** the user explicitly framed Varian as full-stack and asked
about BFF support — aggregating downstream APIs is the core BFF use case, and this
is the one thing that makes it fall over at scale.

**Attempt (root):** make outbound HTTP non-blocking under the scheduler. The
correct design is the libcurl **multi** interface (`curl_multi_*`) driven by the
same event loop the server uses, so a green thread that issues `http.get` yields
instead of blocking, and resumes when the transfer completes. Mirror how other
yielding I/O is implemented in the VM/scheduler (grep the task scheduler in
`src/vm.c` for how socket reads yield, and follow that pattern).

**Fallback (if multi-curl is too large this pass):** keep the blocking client but
(a) add a `timeout_ms` argument so callers can bound it tightly, and (b) document
+ demonstrate the **queue worker-pool** pattern in Aurora (`queue` pool offloads
the downstream call off the request's worker). Leave a `// CAVEAT: blocking —
wrap in queue pool` note at the client and flag for the human.

**Verify:** a test that fires N concurrent handlers each doing an `http.get`
against a deliberately slow endpoint shows the server still serving *other*
routes meanwhile (root fix) — or, for the fallback, shows the pool bounding
concurrency without freezing the whole server.

### C2 — WebSocket/SSE upgrades are not TLS-aware (Lumen reactivity breaks behind HTTPS)

**What:** `vn_modules/zenith.vn` documents (in `listen_tls`, the "Known gap"
comment ~line 549) that `upgrade_websocket` / `sse_handshake` write directly to
the raw socket via `http.write_socket()`, which is **not TLS-aware** — `wss://`
is unsupported, only plain `https://` request/response. Meanwhile the Lumen
client picks the scheme from the page protocol
(`location.protocol==='https:'?'wss://':'ws://'`, `vn_modules/lumen.vn` ~1873).

**Why it matters:** Lumen's *entire* reactivity layer runs over that WebSocket
(`lumen_mount` → `/<path>/live`). Deploy Aurora with `listen_tls` /
`listen_tls_cluster` and every page loads but **nothing is reactive** — the
browser tries `wss://`, the server can't upgrade it. That silently breaks the
headline feature in exactly the configuration real production uses.

**Attempt (root):** route the WebSocket/SSE upgrade handshake and subsequent
frame read/writes through the same TLS layer `listen_tls` uses for normal
request/response, instead of the raw `http.write_socket`/`http.read_socket`.
Find where TLS wraps the socket in `listen_tls` and make `upgrade_websocket` /
the live loop use that wrapped channel. This likely needs a TLS-aware
`ws_send`/`ws_read` path in the C socket layer (`src/lib_http.c`) plus the
zenith.vn upgrade code to use it.

**Fallback (if full wss is too large):** document the supported production
topology — terminate TLS at a reverse proxy (nginx/Caddy) that speaks `wss://`
to the browser and plain `ws://`/`http://` to Zenith — and add that to Aurora's
README as the deploy story. Leave a `// CAVEAT` at `listen_tls` pointing here.

**Verify:** a `wss://` client (or `curl --insecure` upgrade over TLS) against a
`listen_tls` server completes the upgrade and receives a live frame (root fix);
or the README documents the proxy topology and a proxied setup is shown working
(fallback).

### C3 — Single-brace `{ }` interpolation collides with escaping (template hazard)

**What:** tracked as the pending "single-brace `{ }` interpolation collision with
escaping" bug (see `docs/PROBLEMS.md` / task #36). Authoring templates that
contain literal braces (inline CSS `style="…{…}"`, JS objects in a `<client>`
block, etc.) can mis-interpolate or require fragile `\{`/`\}` escaping.

**Why it matters:** Aurora pages and components will contain scoped CSS and
possibly a `<client>` island — both brace-heavy. If this bites, page authors
fight the escaper instead of writing Lumen.

**Attempt (root):** resolve the collision in the renderer/escaper
(`vn_modules/lumen.vn` interpolation path + `native_lumen_escape_str`) so literal
braces in template text pass through untouched and only `{{ }}`/`{ }` *expression*
sites interpolate. Add a regression test covering CSS-with-braces and a JS object
literal in template text.

**Fallback:** if the disambiguation is too invasive now, document the exact
escaping rule authors must follow and add a `vn lint` warning when an unescaped
lone `{` appears in template text. Note it as a known sharp edge.

**Verify:** a component whose `<style scoped>` contains `@media{...}` and nested
rules renders byte-correct, and `{{ price }}` still interpolates.

### C4 — File-based routing is flat: no recursive subdirectory / dynamic page routes

**What:** recursive subdirectory page routing is pending (task #37). The page
collector globs a flat `pages/*.lumen`; there's no file-based `pages/product/[id].lumen`
dynamic segment.

**Why it matters:** product detail pages are the spine of a shop. Without this,
detail pages must be hand-registered Zenith routes (the pattern this plan already
uses) rather than dropping in a file — fine, but it's a seam the user will hit
immediately when extending Aurora.

**Attempt (root):** extend `_lumen_collect_pages` / the build to (a) recurse one
level into subdirectories mapping `pages/a/b.lumen` → `/a/b`, and (b) support a
`[param].lumen` (or `:param.lumen`) convention that registers a dynamic route and
exposes the param to the component's `state()`. Keep it minimal: one level of
recursion + single dynamic segment covers the shop.

**Fallback:** keep the hand-registered `GET /product/:id` pattern documented in
the Architecture section; leave product detail as the demonstration that the
manual seam works, and note file-based dynamic routing as a follow-up.

**Verify:** `pages/product/[id].lumen` resolves at `/product/7` with `id==7`
visible in `state()` (root fix); or the manual `:id` route renders the product
(fallback).

### C5 — The default scaffold page is raw HTML, not the component vocabulary

**What:** `vn lumen new` writes a `pages/index.lumen` built from raw
`<main>/<section>` + inline `style="…"` (see `lumen_new` in `src/main.c` ~line
641). It renders fine, but it teaches the *opposite* of the idiom this plan
requires.

**Why it matters:** the starter is the first thing every Lumen user copies.
Shipping the old style there undercuts the whole "write Lumen, not HTML" message.

**Attempt (root):** rewrite the scaffold's embedded `index.lumen` (the string
literal in `lumen_new`) using the component vocabulary — `<Page>`, `<Section>`,
`<Stack>`, `<Heading>`, `<Text>`, `<Button>` — keeping the reactive `@click` logo
as the one bespoke SVG, and `<style scoped>` for any polish. Re-run `vn lumen new`
into a temp dir and confirm it still renders + the logo still pulses.

**Fallback:** none needed — this is self-contained and low-risk; just do it.

**Verification (Phase −1, overall):** `SHOPPING_SITE_CAVEATS.md` records each of
C1–C5 as FIXED / FALLBACK / DEFERRED with a one-line reason; `ulimit -v 9000000;
vn test tests/` is green; any C-core change also builds clean with
`make clean && make -j8`.

---

## What "uses almost every ability" means — the capability matrix

Every row below must be demonstrably exercised by Aurora. The "Where" column is
the intended home; adjust only if Phase 0 shows a feature works differently.

### Zenith (backend) — `vn_modules/zenith.vn` + support modules

| Capability | API | Where in Aurora |
|---|---|---|
| App + radix routing | `new_app()`, `app.get/post/put/delete/patch` | `server.vn` |
| Dynamic path params | `:id` segments via `radix_insert` / `_convert_path` | `GET /product/:id`, `POST /api/cart/:id` |
| Middleware chain | `app.add_middleware(fn)` | logging, session, shield |
| Static assets | `app.serve_static(prefix, dir)` | `/public` → favicons, product images |
| Signed sessions | `session_set/get/clear`, `session_sign` | cart + auth identity in a signed cookie |
| Cookies | `set_cookie`, `cookie`, `clear_cookie` | theme pref, session id |
| Query / form parsing | `query`, `query_params`, `form`, `form_params` | shop filters, checkout POST |
| JSON API | `json_response(value, status)` | `/api/*` cart + catalog JSON |
| Redirects | `redirect`, `redirect_with` | post-login, post-checkout |
| OpenAPI + Swagger | `app.enable_docs("/docs")`, `app.register_schema`, route `meta` | `/docs` for the `/api` surface |
| WebSocket | `upgrade_websocket` (used by `lumen_mount`) | Lumen live pages |
| Error handler | `app.on_error(fn)` | branded 500 page |
| Cluster / TLS | `listen_cluster`, `listen_tls(_cluster)` | documented prod entry (don't need TLS certs in dev) |
| **shield** | `cors`, `rate_limit`, `csrf` | CORS on `/api`, rate-limit on login + checkout, CSRF on POST forms |
| **auth** | `session_middleware`, `jwt_middleware` | login/session; JWT guard on `/api` admin |
| **db** | `db.select(...).where(...).paginate(...)`, sqlite conn | product catalog + orders |
| **validate** | `validate.str().is_email().min().max()`, `.int()` | server-side checkout/register validation |
| **storage** | `new_storage(dir)`, `put/get/list` | product images / generated invoices |
| **mail** | `send_smtp` or `send_resend` | order-confirmation email |
| **queue** | `cron(interval, fn)`, worker `pool` | abandoned-cart sweep (cron) + async email pool |
| **observe** | (inspect `vn_modules/observe.vn`) | request metrics / structured logs |

### Lumen (frontend) — `vn_modules/lumen.vn`

| Capability | API | Where in Aurora |
|---|---|---|
| File-based pages | `pages/*.lumen` compiled by `lumen_compile_source` | all top-level pages |
| Component vocabulary | `<Page> <Section> <Stack> <Row> <Grid> <Card> <Heading> <Text> <Eyebrow> <Button> <Badge> <Feature> <Divider> <Spacer>` | every page |
| Custom components + imports | `import X from "./components/X.lumen"` | `Nav`, `ProductCard`, `CartBadge`, `Price`, `Footer`, `QtyStepper` |
| Scoped CSS | `<style scoped>` | per-component polish only |
| `{{ }}` / `{{! }}` / `{{#if}}` / `{{#each .. as ..}}` | template syntax | listings, conditionals |
| Server-driven reactivity | `@click/@input/@submit="handler"` → re-render → DOM morph | add-to-cart, qty ±, filters, theme toggle |
| Store (Zustand-style) | `lumen_store(initial)` → `get/set/all` | per-connection cart mirror |
| Broadcast store / pub-sub | `lumen_broadcast_store`, `lumen_publish`, `lumen_subscribe` | **live stock counts** shared across all visitors |
| Resource (React-Query-style) | `lumen_resource(fetcher)`, `lumen_async_resource` | product + order data, with loading state |
| Forms (Zod-style) | `lumen_form(schemas)` + `validate` | checkout + register, inline field errors |
| SEO metadata | `lumen_meta(title, desc, og)` | per page (OG/Twitter tags) |
| Live mount on custom app | `lumen_mount(app, path, component)` | the integration seam (see below) |
| Theming | built-in `--lumen-*` tokens, `.dark` | light/dark toggle |

If after Phase 0 a capability genuinely cannot be made to work within scope,
note it explicitly in the final report with the reason — do not silently drop it.

---

## Architecture — one hand-written `server.vn`, Lumen mounted onto it

The naive path (`vn lumen build pages app.vn`) auto-generates a server but makes
it hard to add a custom API, dynamic routes, and middleware. To exercise
*everything*, Aurora is a **hand-written `server.vn`** that:

1. `new_app()`, then installs middleware (`observe` logging → `shield.cors` →
   `shield.rate_limit` → `session_middleware` → `shield.csrf` for POSTs).
2. Registers the **JSON API** (`/api/products`, `/api/cart`, …) with
   `json_response` + OpenAPI `meta`, guarded by `cors`/`rate_limit`.
3. Registers **dynamic SSR routes** (`GET /product/:id`) that fetch from `db`
   and render a Lumen component with the product as state (pattern below).
4. **Mounts each Lumen page** with `lumen_mount(app, "/", IndexComponent)` etc.
   `lumen_mount` wires both `GET <path>` (full shell) and `GET <path>/live`
   (the WebSocket the reactivity runs over). This is the seam where backend and
   frontend meet — exercise it directly rather than via the file-based builder.
5. `app.serve_static("/public", "public")`, `app.enable_docs("/docs")`,
   `app.on_error(error_page)`.
6. Production entry documented as `app.listen_cluster(8080, 8)`; dev uses
   `app.listen(8080)`.

Page components come from compiling the `.lumen` files. Compile them in a build
step (reuse `lumen_compile_file` / the logic `vn lumen build` already uses) and
`use`/include the generated `_lumen_init_component_<Name>()` registrations, then
fetch each with `_lumen_get_component("Index")` to hand to `lumen_mount`.

> **Dynamic product page pattern** (no dependency on the pending recursive-route
> feature #37): register `GET /product/:id` by hand; in the handler read the id
> from the matched params, `db.select("products").where("id","=")…`, build the
> component state from the row, then
> `_lumen_shell((c.render_fn)(state), "/product/" + id + "/live")`. Mirror the
> same id into the live route so reactivity keeps working on detail pages.

Project layout:

```
aurora/
  server.vn                 # Zenith app: middleware, API, dynamic routes, mounts, cron
  build.vn                  # compiles pages/*.lumen → components, then runs server.vn logic
  db/
    schema.sql              # products, variants, orders, order_items, users
    seed.sql                # ~12 demo products with stock counts
  pages/
    index.lumen             # storefront: hero + featured Grid of ProductCard
    shop.lumen              # catalog: filters (@input), Grid, pagination
    cart.lumen             # cart review: QtyStepper, totals, live
    checkout.lumen          # lumen_form + validate, inline errors, places order
    login.lumen             # auth (rate-limited, CSRF)
    account.lumen           # order history (session-gated), lumen_resource
    components/
      Nav.lumen             # uses <Row>/<Button>; CartBadge live count; theme toggle
      Footer.lumen
      ProductCard.lumen     # <Card> + <Price> + add-to-cart @click + live stock Badge
      Price.lumen           # money formatting (one place)
      CartBadge.lumen       # subscribes to per-conn cart store
      QtyStepper.lumen      # +/- buttons, @click handlers
  public/                   # favicons (from scaffold) + product images
  vn.json                   # Constellation manifest (deps, scripts)
```

---

## Phase 0 — Capability smoke tests (do first, completely)

For each item, write a tiny `.vn` under `aurora/_smoke/` that exercises ONLY that
feature and asserts the result. Run with `ulimit -v 9000000; vn _smoke/<f>.vn`.
Record PASS/FAIL in `aurora/_smoke/RESULTS.md`. **Fix any FAIL at the root before
Phase 1.**

1. `store.vn` — `lumen_store({count:0})`; `set("count",2)`; assert `get("count")==2`.
2. `broadcast.vn` — `lumen_broadcast_store("stock", {n:5})`; `set("n",4)`; a second
   `lumen_subscribe("stock")` reads `4`.
3. `resource.vn` — `lumen_resource(| | { return [1,2,3] })`; assert
   `state().loading==false` and `state().data.len()==3`; after a throwing fetcher,
   `state().error != null`.
4. `form.vn` — `lumen_form({email: validate.str().is_email()})`; `.validate({email:"x"})`
   → `ok==false`, `errors.email != null`; valid email → `ok==true`.
5. `db.vn` — open sqlite, create + seed a tiny table, `db.select("t").where("id","=")`
   bound run returns the row; `.paginate(1,5)` works.
6. `dynamic_route.vn` — build a 1-route app with `GET /p/:id`, call `app.handle`
   with a fake req, assert the param is readable.
7. `session.vn` — `session_set` then `session_get` round-trips a struct through the
   signed cookie; tampering fails verification.
8. `shield.vn` — `cors(...)`, `rate_limit(2, 1000)` blocks the 3rd call,
   `csrf()` rejects a POST with no/bad token.
9. `lumen_mount.vn` — mount a trivial component on an app; `app.handle` on the page
   route returns a 200 whose body contains `data-lumen-root` and the component's text.

**Verification (Phase 0):** every smoke test PASS; `RESULTS.md` committed. Any
root fix to `vn_modules/*.vn` or C core also passes the existing suite:
`ulimit -v 9000000; vn test tests/`.

---

## Phase 1 — Data + backend skeleton

1. `db/schema.sql` + `db/seed.sql`: `products(id,name,slug,price_cents,stock,image,description,featured)`,
   `users(id,email,pass_hash)`, `orders(id,user_id,total_cents,status,created)`,
   `order_items(order_id,product_id,qty,price_cents)`. Seed ~12 products.
2. `server.vn`: `new_app()`; open sqlite; install middleware in this order:
   observe-logging → `cors` → `rate_limit` → `session_middleware` → (csrf applied
   per-POST). Add `app.on_error(error_page)` and `app.serve_static`.
3. JSON API with OpenAPI `meta` + `app.register_schema`:
   - `GET /api/products` (supports `?q=&page=`) → `json_response(list)`
   - `GET /api/products/:id` → one product or 404 JSON
   - `GET /api/cart` → current cart from session
   - `POST /api/cart/:id` (qty) / `DELETE /api/cart/:id` → mutate cart in session,
     return updated cart JSON
   - `app.enable_docs("/docs")`.
4. `queue`: `cron(60000, abandoned_cart_sweep)` (logs/email carts idle > N min);
   an email worker `pool` that `send_smtp`/`send_resend` order confirmations off
   the request path.

**Verification (Phase 1):** `ulimit -v 9000000; vn server.vn &` then `curl`:
`/api/products` returns seeded JSON; `/api/products/1` returns one; POST then GET
`/api/cart` reflects the change; `/docs` serves Swagger; bad route → branded 500
via `on_error`; rate-limit returns 429 after the threshold. Kill the server.

---

## Phase 2 — Lumen pages in the component vocabulary

Build the custom components first (`Price`, `ProductCard`, `QtyStepper`,
`CartBadge`, `Nav`, `Footer`), then the pages. **Every page** opens with `<Page>`
and lays out with `<Section>/<Container>/<Stack>/<Grid>/<Row>`; product tiles are
`<ProductCard>`; CTAs are `<Button>`; tags are `<Badge>`; headings/copy are
`<Heading>/<Text>/<Eyebrow>`. Page-specific CSS goes in `<style scoped>`.

1. `components/ProductCard.lumen` — `<Card>` with image, `<Heading>`, `<Price>`,
   a live stock `<Badge>` (subscribes to the `"stock"` broadcast topic), and an
   add-to-cart `<Button @click="add">`. Props arrive as attributes; the slot is
   children.
2. `components/QtyStepper.lumen` — `<Row>` with `−`/`+` `<Button>`s
   (`@click="dec"`/`@click="inc"`) around the quantity.
3. `components/CartBadge.lumen` — reads the per-connection cart `lumen_store`,
   shows item count; updates live as items are added anywhere on the page.
4. `components/Nav.lumen` — `<Row>` brand + links + `CartBadge` + a dark-mode
   toggle (`@click` flips `.dark`, persists via cookie).
5. Pages:
   - `index.lumen` — `lumen_meta(...)`; hero `<Section>`; `<Grid>` of featured
     `ProductCard`s from a `lumen_resource` over `db`.
   - `shop.lumen` — search/filter via `@input="filter"`; `<Grid>` results;
     pagination using `db.select(...).paginate(page, per_page)`.
   - `cart.lumen` — `<Stack>` of line items with `QtyStepper`; totals from the
     session cart; live recalculation on qty change; "Checkout" `<Button>`.
   - `checkout.lumen` — `lumen_form` + `validate` (name/email/address/card-stub);
     `@submit="place"`; inline `{{ errors.email }}`; on success create an order
     (db), enqueue confirmation email, `redirect` to a thank-you state.
   - `login.lumen` — `lumen_form`; rate-limited; sets signed session on success.
   - `account.lumen` — session-gated; `lumen_resource` over `orders` for history.

**Verification (Phase 2):** `ulimit -v 9000000; vn build.vn &`; for each route
`curl` the page and assert (a) it returns 200, (b) body contains `data-lumen-root`
and the expected component text, (c) it does **not** contain telltale raw-HTML
smells the vocabulary would have replaced (a reviewer spot-checks the source for
`<div ... style="` outside scoped components). Confirm the page `<head>` carries
the `--lumen-*` tokens and the `lumen_meta` OG tags.

---

## Phase 3 — Reactivity, live state, and the cross-client feature

1. **Cart**: session cookie is the source of truth; a per-connection
   `lumen_store` mirrors it for instant re-render. Add/qty/remove handlers mutate
   both; `CartBadge` and `cart.lumen` reflect changes live (DOM morph, no client
   JS).
2. **Live stock**: a top-level `lumen_broadcast_store("stock", ...)`. When any
   visitor's add-to-cart decrements stock, `lumen_publish` wakes every live loop
   so all open `ProductCard`s update their stock `<Badge>` in real time. This is
   the headline "full-stack reactive" demo — verify with two browser tabs (or two
   WebSocket clients in a script).
3. **Theme**: `@click` toggle adds/removes `.dark` and persists via cookie; the
   built-in tokens do the rest.

**Verification (Phase 3):** scripted WebSocket test against `/<page>/live`: send an
add-to-cart event, assert the server pushes an HTML/patch frame and that a *second*
connected client also receives a stock-badge update (proves `lumen_publish`
cross-connection wake). Cart total updates on qty change. Theme persists across a
reload (cookie present).

---

## Phase 4 — Polish, docs, and the end-to-end check

1. `vn.json` (Constellation): declare deps + a `scripts` entry to build & run.
   Verify `vn build` (Kiln) produces a `.vnb`, and `vn build --release` a native
   binary, of the whole app (per `project_lumen_frontend` memory).
2. `aurora/README.md`: how to seed, run (`vn build.vn`), the routes, and which
   capability each page demonstrates (mirror the matrix above).
3. End-to-end smoke: home → shop → add to cart → cart → checkout → order created
   → confirmation email enqueued → `account` shows the order.

**Verification (Phase 4):** the end-to-end path passes; `vn build` and
`vn build --release` both succeed; `ulimit -v 9000000; vn test tests/` still green;
no ASan run combined with `ulimit`. Report final capability-matrix coverage
(checked/with-notes) and anything that needed a root fix in Phase 0.

---

## Appendix A — The JavaScript / TypeScript question (design note for the human)

You asked for "a clean way of using JS in Lumen" beyond dropping files in
`public/`, with a preference for **TypeScript** for safety, and "not crazy
amounts of JS." Here's the honest state and the recommended direction. **This is
a separate, small follow-up task — not part of building Aurora** (Aurora should
need essentially zero hand-written client JS, which is the point).

**Why Aurora needs almost no JS.** Lumen is *server-driven*: `@click`/`@input`/
`@submit` are compiled to `data-lumen-*` attributes; a tiny (~2 KB) built-in
runtime forwards the event over a WebSocket, the server re-renders, and the
client morphs the DOM (`_lumen_client_js` in `lumen.vn`). Add-to-cart, quantity
steppers, filters, theme toggles, live stock — all of that is plain server state.
No hydration, no bundle. So the *default answer to "how do I add interactivity"*
is "write a Varian handler," not "write JS."

**The escape hatch that exists today.** For genuinely client-only behavior
(a canvas, a map, an animation that shouldn't round-trip), `lumen_compile_source`
already supports a `<client>` block that is emitted verbatim as a `<script>`
(`lumen.vn:766`). That's the raw-JS island. It works but is untyped.

**Recommended addition (clean + typed, opt-in, no "crazy JS"):**

1. Support `<client lang="ts">`. At compile time, if the block declares TS, pipe
   it through **esbuild** (single static binary, ~25 ms, no `node_modules`) with
   `--loader=ts --format=iife --minify`. Emit the resulting JS into the existing
   `<script>` slot. If `esbuild` isn't on `PATH`, fail the build with a clear,
   actionable message (don't silently ship untranspiled TS).
   - Implementation point: in `lumen_compile_source`, where `<client>` is sliced
     (around `lumen.vn:766-772`), branch on the `lang` attribute and shell out via
     the existing process/exec native (the same mechanism Kiln uses to run build
     scripts — see `project`/Kiln) before concatenation.
2. esbuild *strips* types but doesn't *check* them. For real safety, add an
   optional `tsc --noEmit` pass wired into `vn dev` / Kiln that type-checks
   `<client>` blocks and `public/*.ts`, surfaced as a lint step — fast, advisory,
   and skippable when the toolchain is absent.
3. For shared client utilities (not page-bound), allow `public/*.ts` compiled to
   `public/*.js` on build by the same esbuild pass, so the thing in `public/` is
   the *output*, not hand-written JS.

This keeps the philosophy intact: **TS is opt-in for the rare client island,
type-checked, and produced by a build step** — while the 95% case stays
server-driven with zero client code. Keep the esbuild/tsc dependency *optional*:
a project with no `<client lang="ts">` blocks must build with neither tool
installed.

Suggested follow-up tasks (separate from Aurora): "Lumen: `<client lang=ts>` via
esbuild" and "Lumen: optional `tsc --noEmit` lint in `vn dev`/Kiln."
