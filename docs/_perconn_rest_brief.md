# TASK: per-connection state for data_form_component and data_table_virtual_component

Edit ONLY `vn_modules/lumen.vn`. Modify nothing else, no other file.

## Background
We previously rewrote `data_table_component` to use per-connection state (instead of a shared singleton `core`). Look at `data_table_component` and `_render_dt_live` in `vn_modules/lumen.vn` to see the exact pattern:
- `state_fn` returns plain JSON-serializable state struct.
- Configuration (like `conn`, `table`, `columns`) is captured in a `cfg` struct.
- Handlers mutate the state `s` and return it.
- `_render_dt_live(cfg, s)` combines the `cfg` and per-connection state `s` to fetch data and render the HTML.
- It uses `cached_query` for data fetching and calls `_lumen_watch_table` so the reactive tick wakes it up.

## What to do
Apply this exact same per-connection state pattern to the remaining built-in components:
1. `data_form_component` and its associated render logic (currently `_data_form_html`).
2. `data_table_virtual_component` and its associated render logic (currently `_data_table_virtual_html`).

### Details for data_form_component:
- Currently it builds a singleton `core = data_form(opts)`.
- Rewrite it so `state_fn` returns the form's state (`values`, `errors`, `status`, `message`).
- Capture `fields`, `names`, `submit_label`, `on_submit`, and the `lumen_form(schema)` validator in a `cfg` struct.
- Handlers (`df_submit`, `df_reset`, `df_set_...`) should mutate `s` directly and handle the `on_submit` logic, just like `data_form` used to do, but operating on the per-connection `s`.
- Create a `_render_df_live(cfg, s)` helper to render the HTML using `cfg` and `s`. Keep the original `data_form` and `_data_form_html` intact for manual headless use.

### Details for data_table_virtual_component:
- Currently it builds a singleton `core = data_table_virtual(opts)`.
- Rewrite it so `state_fn` returns its state (`first`, `count`, `sort`, `dir`, `filters`).
- Capture `conn`, `table`, `columns`, `row_height`, `viewport_height` in a `cfg` struct.
- Handlers mutate `s`.
- Create a `_render_dt_virtual_live(cfg, s)` helper. It should execute the two queries (`COUNT(*)` and the `SELECT` rows) using `cached_query(conn, sql, params, [table])` just like `_render_dt_live` does.
- Call `_lumen_watch_table(table)` in `data_table_virtual_component`.
- Keep the original `data_table_virtual` and `_data_table_virtual_html` intact.

## Rules
- Preserve the exact HTML structure and escaping (`_sanitize.escape_html`).
- Ensure no closures are put into the state returned by `state_fn`.
- Do not modify `data_form` or `data_table_virtual` headless implementations.
