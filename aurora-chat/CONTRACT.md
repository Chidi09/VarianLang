# Aurora Chat Shop — Build Contract (SOURCE OF TRUTH)

Second Aurora example: an **AI shopping assistant** where users *text* a chatbot to
browse and buy goods, on top of a full e-commerce store. The chatbot brain is **real
Google Gemini** (`gemini-2.5-flash`) via the `ai.vn` stdlib module.

Three delegates build against THIS contract. Do not invent names; use exactly what is
written here. If a name/signature is not in this file, mirror the existing `aurora/`
app (`/root/dev/VarianLang/aurora/`) which is the canonical reference.

---

## 0. HARD RULES (all delegates)
1. **Do not invent APIs.** Every stdlib call must exist in `vn_modules/*.vn` or `src/*.c`.
   When unsure, `grep` the repo or copy the pattern from `aurora/`. Hallucinated APIs =
   rejected.
2. Varian syntax facts: `>=`/`<`/`>` return **int 0/1, not bool** — return real bools with
   explicit `if/return`. Struct literals `Name { f: v }` WORK. Dot access on real structs
   (`row.name`) WORKS. `_validate.get_field(obj, "k")` is only for dynamically-decoded
   JSON/maps. Arrays are immutable-style: `a = a.push(x)` (push returns a new array).
   `for i in 0..n { }` ranges. No `++`.
3. Prices are stored as **integer cents** (`price_cents`). Use `format_price(cents)`.
4. All HTML that contains user/DB text MUST go through `_sanitize.escape_html(...)`.
5. Code must compile AND run: `cd aurora-chat && vn run main.vn` (after build step).
6. Only edit files inside `aurora-chat/`. Never touch `vn_modules/` or `src/`.

---

## 1. Directory layout
```
aurora-chat/
  main.vn                 # entry (Claude scaffolds; logic delegate wires)
  build_pages.vn          # compiles .lumen -> .gen/pages.vn (Claude scaffolds)
  vn.json                 # (Claude)
  .env                    # GEMINI_API_KEY (gitignored; Claude)
  lib/
    app_config.vn         # cfg() (Claude scaffolds)
    app_db.vn             # db_init + schema (Claude scaffolds schema)
    app_chat.vn           # CHATBOT BRAIN            -> DEEPSEEK
    app_products.vn       # product search/list      -> DEEPSEEK
    app_cart.vn           # cart                     -> DEEPSEEK
    app_orders.vn         # orders/checkout/csv/mail -> DEEPSEEK
    app_auth.vn           # auth/crypto/storage      -> DEEPSEEK
    app_middleware.vn     # shield/ratelimit/observer-> DEEPSEEK
    app_misc.vn           # fetch/seo/migration/event/validate -> DEEPSEEK
    pages.vn              # mount_pages() route->page wiring -> DEEPSEEK
  pages/                  # .lumen files            -> GEMINI 3.1 PRO
    index.lumen  chat.lumen  catalog.lumen  orders.lumen
    cart.lumen  checkout.lumen  account.lumen  login.lumen
    components/ Nav.lumen Footer.lumen ProductCard.lumen ChatBubble.lumen Price.lumen
  db/
    schema.sql            # reference DDL (Claude)
    seed.vn              # 1000+ product seeder     -> GEMINI 3.5 FLASH
```

---

## 2. DATABASE SCHEMA (FROZEN — every delegate uses these exact table/column names)
SQLite via `sqlite.connect(path)` / `sqlite.query(conn, sql)`. `sqlite.query` returns an
array of row structs; access columns by dot (`row.name`, `row.price_cents`).

```sql
products(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT NOT NULL,
  slug TEXT NOT NULL UNIQUE,
  price_cents INTEGER NOT NULL,
  stock INTEGER NOT NULL DEFAULT 0,
  image TEXT NOT NULL DEFAULT '',
  description TEXT NOT NULL DEFAULT '',
  category TEXT NOT NULL DEFAULT 'general',   -- e.g. electronics, home, apparel, books, toys
  featured INTEGER NOT NULL DEFAULT 0,
  created TEXT NOT NULL DEFAULT (datetime('now'))
)
users(id, email UNIQUE, pass_hash, name, avatar TEXT DEFAULT '', created)
orders(id, user_id, session_key TEXT, total_cents, status TEXT DEFAULT 'pending', created)
order_items(id, order_id, product_id, qty, price_cents)
carts(id, session_key TEXT, product_id INTEGER, qty INTEGER, UNIQUE(session_key, product_id))
chat_sessions(id, session_key TEXT UNIQUE, user_id INTEGER, created)
chat_messages(id, session_key TEXT, role TEXT, content TEXT, created)   -- role: user|assistant
```
`order.status` is one of: `pending | paid | shipped | delivered | cancelled`.

---

## 3. CONFIG (provided by Claude in `lib/app_config.vn`)
`cfg()` returns a struct with these fields (read with dot access):
```
c.db_path        "aurora-chat.db"
c.port           8099
c.gemini_key     <from env GEMINI_API_KEY>     // may be "" if unset
c.gemini_model   "gemini-2.5-flash"
c.secret_key     <from env or default dev key> // for crypto-signed session cookies
c.mail_host  c.mail_port  c.mail_user  c.mail_pass
c.has_ai()       returns bool: true if gemini_key != ""
```

---

## 4. AI CHATBOT BRAIN — `ai.vn` usage (DEEPSEEK, in app_chat.vn)
`ai.vn` exposes FREE globals (not an `ai.` module): `ai_client(opts)`, `ai_conversation(sys)`.
Create the client ONCE:
```
let client = ai_client(http.create_struct(
    ["provider", "api_key", "model"],
    ["gemini", c.gemini_key, c.gemini_model]))
```
Build a conversation and call chat (returns a response struct; `resp.message.content` is text):
```
let conv = ai_conversation(SYSTEM_PROMPT)
conv.add("user", user_text)
let resp = client.chat(conv, null)        // or client.chat_with_fallback(conv, null)
let text = resp.message.content
```
**Intent design:** Prompt Gemini to reply as a shopping assistant AND to emit a single
machine-readable action line the server can parse, e.g. ask it to end with
`\n<<ACTION:{"tool":"search","query":"..."}>>` (tools: `search`, `add_to_cart`,
`view_cart`, `checkout`, `track_order`, `none`). Parse that JSON with `json_decode`,
execute against the DB, and feed results back. If `c.has_ai()` is false OR the API errors,
fall back to a deterministic keyword parser (regex/`string.contains`) so the demo still
runs — print a console note, never crash. Persist every turn to `chat_messages`.

`chat_reply(conn, c, client, session_key, user_text)` -> struct
  `{ reply: <string>, products: <array of product rows>, action: <string> }`

---

## 5. BACKEND FUNCTION CONTRACT (DEEPSEEK must expose exactly these)
Each `register_*` takes the app and mounts routes. `app` = `new_app()` (see aurora/main.vn).
HTTP helpers `json_res(data, status)` / `err_res(msg, status)` / `format_price(cents)` are in app_db.vn.

```
app_products.vn:
  search_products(conn, query, category, page, per_page) -> { rows, total }
  get_product(conn, id_or_slug) -> row | null
  register_product_api(app, conn, c)        // GET /api/products?q=&page=, GET /api/products/:id

app_cart.vn:
  cart_session_key(req, c) -> string        // read/sign cookie via _crypto
  cart_add(conn, session_key, product_id, qty)
  cart_items(conn, c, session_key) -> { items, total_cents }
  register_cart_api(app, conn, c)           // POST /api/cart, GET /api/cart

app_chat.vn:
  make_ai_client(c) -> client
  chat_reply(conn, c, client, session_key, user_text) -> { reply, products, action }
  register_chat_api(app, conn, c)           // POST /api/chat  AND  ws route /ws/chat (ws module)

app_orders.vn:
  create_order(conn, c, session_key, user_id, bus) -> order_row   // enqueue(queue)+mail+event+observer
  track_order(conn, order_id) -> row | null
  export_orders_csv(conn) -> string         // csv module
  register_order_api(app, conn, c, bus)      // POST /api/checkout, GET /api/orders/:id, GET /admin/orders.csv

app_auth.vn:
  register_auth(app, conn, c)               // auth module sessions + crypto pw hash + storage avatar upload

app_middleware.vn:
  install_middleware(app, c)                // shield cors+csrf, ratelimit on /api/chat, observer metrics, sanitize

app_misc.vn:
  shipping_quote(dest)                      // fetch module -> external/echo API
  page_seo(title, desc)                     // seo module meta
  run_migrations(conn)                      // migration module
  app_bus()                                 // event module bus (returned, passed to orders)
  validate_checkout(payload) -> { ok, errors }   // validate module

pages.vn:
  mount_pages(app, conn, c)                 // wire each route to its compiled .lumen page state
```

### Module usage matrix (ALL must be exercised — reviewer checks each is imported & called)
| module    | used in            | for                                  |
|-----------|--------------------|--------------------------------------|
| auth      | app_auth           | login sessions                       |
| crypto    | app_auth, app_cart | pw hash, signed session cookie       |
| csv       | app_orders         | `GET /admin/orders.csv` export       |
| event     | app_misc, orders   | `order.created` bus + subscriber     |
| fetch     | app_misc           | external shipping quote              |
| mail      | app_orders         | order confirmation email             |
| migration | app_misc/app_db    | versioned migrator                   |
| observer  | app_middleware     | request count/latency metrics        |
| queue     | app_orders         | async order-fulfillment job          |
| ratelimit | app_middleware     | per-IP cap on `/api/chat`            |
| sanitize  | chat, tables       | escape all user/DB text into HTML    |
| seo       | app_misc, pages    | `<title>`/meta tags                  |
| shield    | app_middleware     | CORS + CSRF                          |
| storage   | app_auth           | user avatar upload                   |
| ws        | app_chat           | live chat socket `/ws/chat`          |
| validate  | app_misc           | checkout payload validation          |

---

## 6. PAGES (GEMINI 3.1 PRO) — `.lumen` files
`.lumen` format = `<template>...</template>`, `<style scoped>...</style>`, `<script> fn state() { return { ... } } </script>`.
Templating: `{{ expr }}`, escaped `{{! expr }}`, `{{#if cond}}...{{else}}...{{/if}}`,
`{{#each items as x}}...{{/each}}`. Components: `<Nav />`, `<ProductCard id=".." name=".." price=".." />`.
COPY the structure/conventions from `aurora/pages/shop.lumen` and `aurora/pages/components/*`.

Pages to build:
- `index.lumen`     — hero, "Text us to shop" CTA, featured products grid.
- `chat.lumen`      — **the AI chat UI**: scrollable message list (user right / assistant left
  bubbles via `<ChatBubble />`), inline product cards in assistant replies with Add-to-cart,
  text input + send. Client `<script>` opens a WebSocket to `/ws/chat` (fallback: POST `/api/chat`)
  and appends bubbles. Keep JS minimal & vanilla.
- `catalog.lumen`   — full product browser. MUST embed the live **virtual-scroll** table
  (`data_table_virtual_component`, see §7) over `products` (1000+ rows) — the headline
  "as good as TanStack" feature. Columns: name, category, price_cents (label "Price"), stock.
- `orders.lumen`    — admin view. MUST embed the live **paginated/sortable** table
  (`data_table_component`, §7) over `orders`. Columns: id, session_key, total_cents (label
  "Total"), status, created. page_size 25.
- `cart.lumen` `checkout.lumen` `account.lumen` `login.lumen` — standard store pages, mirror aurora.
- `components/`: `Nav.lumen`, `Footer.lumen`, `ProductCard.lumen`, `ChatBubble.lumen`, `Price.lumen`.

Design: modern, clean, dark-mode-friendly using the `--lumen-*` CSS vars seen in
aurora/pages/shop.lumen (`--lumen-fg`, `--lumen-surface`, `--lumen-border`, `--lumen-primary`,
`--lumen-muted-fg`). Responsive grids. No external JS frameworks.

---

## 7. LIVE TABLE COMPONENTS (verified signatures — UI delegate uses, logic delegate mounts)
These are Lumen stdlib components (in `vn_modules/lumen.vn`). A `.lumen` page embeds a live
component by having its server-side handler call the component; the canonical wiring is in
`aurora/` — mirror it. Signatures (opts is a map literal):

```
data_table_component({ conn, table, columns, page_size, order_by, order_dir, filters })
  // paginated + sortable. columns = [ { name, label, sortable } , ... ]

data_table_virtual_component({ conn, table, columns, order_by, order_dir,
                               row_height, viewport_height, window_count, filters })
  // windowed virtual scroll for huge tables.
```
`columns` entries: `{ name: "price_cents", label: "Price", sortable: true }`.
The component renders its own `<table>`, sort buttons, and pager — the page just provides a
mount point. Mirror how `aurora/` pages embed these (grep aurora for `data_table`).

---

## 8. SEED (GEMINI 3.5 FLASH) — `db/seed.vn`
A runnable Varian script: `cd aurora-chat && vn run db/seed.vn`. It must:
- Connect via `sqlite.connect("aurora-chat.db")`, ensure schema (or call `db_init`).
- Insert **1000+ products** spread across ≥5 categories (electronics, home, apparel, books,
  toys), realistic names, varied `price_cents` (e.g. 199..499999), random `stock`, a real
  Unsplash `image` URL, a one-line `description`, ~10% `featured=1`, unique `slug`.
- Generate programmatically (loop over base names × variants/adjectives) — do NOT paste 1000
  literal rows. Batch inserts for speed (multi-row `INSERT ... VALUES (...),(...)` in chunks).
- Also seed ~50 fake `orders` + `order_items` (varied statuses) so the admin paginated table
  has multiple pages.
- Print a summary: total products + orders inserted.
Use ONLY `sqlite.query`. Escape single quotes in generated text (double them) so SQL is valid.

---

## 9. INTEGRATION / DEFINITION OF DONE (Claude verifies)
- `vn run db/seed.vn` inserts 1000+ products with no SQL error.
- `vn run build_pages.vn && vn run main.vn` boots on port 8099.
- `/catalog` virtual table scrolls 1000+ rows; `/admin/orders` paginates & sorts.
- `/chat` returns a Gemini-generated reply and can add a product to the cart by text.
- Every module in the §5 matrix is imported and actually called.
- `./vn test tests/` baseline unchanged (315/6).
