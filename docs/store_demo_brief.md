# TASK: Build a demo "store" Aurora app that exercises the new data layer

Create ONE new file `examples/store_app.vn`. Do NOT edit any module. `.vn` is interpreted; the app
is run with `./vn run examples/store_app.vn` (on Linux/WSL/codespace — it does not run on Windows).
Use ONLY the APIs documented below (they exist in vn_modules/lumen.vn and db.vn). Keep it one file.

## What to build
A tiny store admin: a live products table + an "add product" form, on one SQLite DB. When you add a
product (or change stock from another tab), the table updates itself live. Each browser gets its own
sort/page (per-connection state). Routes:
- `/`        → the add-product FORM (data_form_component)
- `/products`→ the live products TABLE (data_table_component)

## APIs you MUST use (exact signatures — do not invent others)
- `let conn = db.connect("sqlite", "store.db")` → wrapper with `(conn.query)(sql, params)` and
  `(conn.execute)(sql, params)` (params is an array; use `?` placeholders).
- `data_table_component({ conn, table, columns, page_size?, order_by?, order_dir?, filters? })`
  - `columns` = array of `{ name, label, sortable }`.
  - returns a live, per-connection, sortable, paginated table component.
- `data_form_component({ submit_label?, fields, on_submit })`
  - `fields` = array of `{ name, label, type?, placeholder?, hint?, validator? }`
    (type: "text"|"email"|"password"|"number"|"textarea"; validator from the global `validate`).
  - `on_submit` = `|values| { ... }` — values is a struct keyed by field name (e.g. `values.name`).
- `validate.str()` / `validate.num()` with chained `.min(n)` / `.max(n)` / `.is_email()` /
  `.optional()`. Example: `validate.str().min(2)`.
- `let app = new_app()`, `lumen_mount(app, "/path", component)`, `app.listen(PORT)`.

## Requirements
1. Open `conn`, then create the table if missing:
   `CREATE TABLE IF NOT EXISTS products (id INTEGER PRIMARY KEY, name TEXT, category TEXT, price INTEGER, stock INTEGER)`
2. SEED THOROUGHLY but only once: check `(conn.query)("SELECT COUNT(*) AS n FROM products", [])` and
   if `[0].n == 0`, insert ~25 realistic products across a few categories (e.g. "Coffee", "Tea",
   "Bakery", "Merch") with varied price (integer cents or whole dollars) and stock. Use a loop or
   explicit inserts; use parameterized `(conn.execute)(..., [name, category, price, stock])`.
3. Build the products table component with sortable columns id, name, category, price, stock and
   `page_size: 10`, `order_by: "id"`, `order_dir: "asc"`.
4. Build the add-product form with fields: name (text, `validate.str().min(2)`), category (text,
   `validate.str().min(2)`), price (number, `validate.num().min(0)`), stock (number,
   `validate.num().min(0)`). `on_submit` inserts the row via `(conn.execute)("INSERT INTO products
   (name, category, price, stock) VALUES (?, ?, ?, ?)", [values.name, values.category, values.price,
   values.stock])` and returns true. (Because the table watches `products`, it auto-updates.)
5. Mount form at `/`, table at `/products`, and `app.listen(8096)`.
6. Top of file: a comment block explaining what it demonstrates and the run/open instructions.

## Rules
- One file only: `examples/store_app.vn`. Invent no new APIs; use exactly those above.
- Mirror the style of `examples/lumen_data_form.vn` and `examples/lumen_data_table.vn` (read them
  for the exact shape; do not modify them).
- In your final message, confirm the file path and that you used only the documented APIs.
