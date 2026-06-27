# Pillar 2 — Server-driven, live, windowed `<Table>`

STATUS: Chunks 1 & 2 DONE (uncommitted→committed this round). Live, sortable, OFFSET-paginated
data table landed: `db.vn` gained `QueryBuilder.order_by` + ORDER BY in `compile_select`;
`lumen.vn` gained `data_table(opts)` (headless core), `_data_table_html(core)` (renderer), and
`data_table_component(opts)` (mountable live component). Example: `examples/lumen_data_table.vn`.
Untested at runtime (Windows is LSP-only / `.vn` interpreted — needs Linux/WSL). Chunk 3 (keyset
+ windowed virtual scroll) deferred.


Goal: a built-in data table that does **server-side sort / filter / paginate + windowed
virtual scroll**, and is **live** (auto-updates on DB change) by building on Pillar 1's
`live_query`. Beats TanStack Table (headless logic) + TanStack Virtual (windowing) because the
**DB does the work over the whole dataset**, not a client-side copy. Research basis: §6 of the
Gemini brief (keyset pagination, server-side windowing, headless logic).

## Build on what already exists (read these FIRST, post-compaction)
- `vn_modules/db.vn` — already has a query **builder**: `select(table)` → `QueryBuilder` with
  `.use_dialect/.fields/.where/.limit/.offset/.paginate(page,per_page)/.cursor_paginate(field,
  has_cursor,l)/.find_by_key/.build()` and `compile_select(...)`, plus `bind()`, `run_sqlite()`,
  `run_postgres()`. REUSE these to build SQL safely.
- `vn_modules/db.vn` — `db.connect()` wrapper `.query/.execute` + the Pillar-1 capture hooks.
- `vn_modules/lumen.vn` — `live_query(fetcher)` (Pillar 1); the `_ui_render_*` component family
  (~lines 2425–2860) and `_ui_component(render_fn)` / `lumen_register_component(name, comp)`;
  how `@for` collections render and how the live loop morphs list updates.

## Design
1. **Keyset (cursor) pagination**, NOT OFFSET (research §6): `ORDER BY <key> DESC, id DESC` +
   `WHERE (key,id) < (cursor)`. Stable, index-seek, O(1) regardless of depth.
2. **Headless core** `data_table(opts)` returning `{ rows(), meta(), set_sort, set_filter,
   next_page, prev_page, set_window }`:
   - opts: `table`, `columns` (name/label/sortable), `page_size`, initial `order_by`/`filters`.
   - Internally builds SQL via `db.select(table)...build()` from current state, wraps the fetch
     in `live_query(...)` so rows auto-update on DB change, and exposes the current rows + meta
     (`has_more`, `cursor`).
   - State (sort col/dir, filters, cursor/page, window) held per component; handlers re-query.
3. **`<Table>` component** (register via `_ui_component`) consuming the headless core for the
   common case: renders `<thead>` with sortable headers (clicking a header fires `set_sort`),
   `<tbody>` rows, and pager / "load more". Headless core stays available for custom markup.
4. **Windowed virtual scroll** (the hard part — can be a later sub-step): client sends viewport
   intersection / scroll position; server computes the row window via keyset; morph only the
   visible rows (absolutely positioned, translateY). Prior art: LiveView/Hotwire infinite scroll.
   v1 can ship "load more"/pagination first, add true windowing after.
5. **Sort/filter state in the URL** ties into Pillar 4 (typed search params) — defer the URL
   coupling; v1 keeps state in the component.

## Chunked delegation (the drill: flash writes, I review; C-free so all reviewable)
Refined after reading the code (decisions): v1 uses **OFFSET pagination** via the builder's
existing `.paginate(page,per_page)` + a NEW `.order_by(field,dir)` (compile_select has no ORDER
BY yet). Keyset + windowing deferred to the last chunk (plan §4 allows "load more first").
The fetcher MUST run through the **connection wrapper** `conn.query(sql,params)` (NOT
`db.run_sqlite`) so Pillar-1's read-set capture sees the table and `live_query` registers the dep.
- **Chunk 1** — (a) `db.vn`: add `QueryBuilder.order_by(field,dir)` + ORDER BY emission in
  `compile_select` (shape-only, comptime-safe, after WHERE / before LIMIT). (b) `lumen.vn`:
  `data_table(opts)` headless core — state holders (sort/dir/page/filters), fetcher builds SQL via
  `db.select(table).fields(..).where(..).order_by(..).paginate(..).build()` then `conn.query`,
  wrapped in `live_query`; returns `{rows, meta, columns, set_sort, set_filter, next_page,
  prev_page, reload}`. Mirror BOTH to deploy.
- **Chunk 2** — DONE-DESIGN: NOT a registered `<Table>` (template props are strings + `conn` is a
  runtime object, so the core can't pass through a tag prop). Instead a **factory**
  `data_table_component(opts)` that closes over `data_table(opts)` and returns a
  `lumen_component(state_fn, render_fn, handler_names, handler_fns)` — built + `lumen_mount`ed in
  code exactly like `examples/lumen_counter.vn`. `render_fn` calls `_data_table_html(core)` which
  builds sortable `<thead>` (header = `<button @click="dt_sort" value="COL">` — client sends
  `el.value`, so the col name reaches the `|s,v|` handler), `<tbody>` rows (cells escaped via
  `_sanitize.escape_html`), and a Prev/Next pager (`@click="dt_prev"`/`dt_next"`), all run through
  `lumen_render(..)` so `@click` binds. Handlers call `core.set_sort/next_page/prev_page/reload`.
  CSS (`.lmn-table*`, `.lmn-th`, `.lmn-pager`) added to `_lumen_design_css()`.
- **Chunk 3 (later)** — keyset cursor pagination + windowed virtual scroll (viewport→window→morph).

## Limits / notes
- Coarse-live inherited from Pillar 1 (table-level). SQLite-first.
- Untestable on Windows (interpreted + LSP-only binary) — review-only; real test on Linux/WSL.
- Keep the headless core usable without `<Table>` (TanStack's headless appeal).
