# TASK: per-connection state for data_table_component (.vn)

Edit ONLY `vn_modules/lumen.vn`. FOUR edits. Modify nothing else, no other file, run nothing.

## Why
Today `data_table_component(opts)` builds a singleton `core` (via `data_table`) whose holders are
SHARED across all connections — so clicking sort on one client changes it for everyone. Fix: keep
per-connection view state (sort/dir/page) in the component `state` (plain JSON, which is what the
live loop serializes + round-trips), and query at render time through the Pillar-5 cache. Reactivity
without per-connection closures: register the table in a global watched-set; the reactive tick wakes
all live loops when a watched table changes, and each re-renders from its own state.

## Context (ground truth)
- Component model: `lumen_component(state_fn, render_fn, handler_names, handler_fns)`. `state_fn()`
  → plain JSON state (per connection). Handlers `|s, v| { ...; return s }`; member read/write on
  state works (e.g. `s.dt_page = s.dt_page + 1`, like examples/lumen_counter.vn). `render_fn(s)`
  returns `lumen_render(html, {})`.
- Query builder: `db.select(table).fields(arr).where(f,op).order_by(f,dir).paginate(page,per).build()`
  → `{sql,...}`. `cached_query(conn, sql, params, tables)` runs it through the shared cache.
- `_lumen_ensure_reactive()` starts the 30ms reactive tick. `_tpl_bind(struct,k,v)` add/updates.
- `_sanitize.escape_html(str)`. Index `while` loops, `_validate.get_field`, `arr.push` idioms.

## EDIT 1 — global watched-tables set
Find this line (a Pillar-5 global):
```
let _lumen_cache_stats = [http.create_struct(["hits", "misses"], [0, 0])]
```
Insert IMMEDIATELY AFTER it:
```
let _lumen_watched_tables = [http.create_struct([], [])]
```

## EDIT 2 — _lumen_watch_table helper
Find the END of `fn lumen_perf_stats()` — these exact lines:
```
    return http.create_struct(["cache_hits", "cache_misses", "cache_entries"], [h, m, entries])
}
```
Insert IMMEDIATELY AFTER (a new function):
```

// Register a table so the reactive tick wakes ALL live loops when it changes
// (per-connection render-query model: each loop re-renders from its own state).
fn _lumen_watch_table(t) {
    _lumen_ensure_reactive()
    _lumen_watched_tables[0] = _tpl_bind(_lumen_watched_tables[0], t, true)
    return null
}
```

## EDIT 3 — wake on watched-table change in the reactive tick
In `fn _lumen_reactive_tick()`, find this exact block:
```
    if any { let _ = _lumen_async_updates <- true }
    return null
}
```
Replace it with:
```
    let wi = 0
    while wi < tables.len() {
        if _validate.get_field(_lumen_watched_tables[0], tables[wi]) != null { any = true }
        wi = wi + 1
    }
    if any { let _ = _lumen_async_updates <- true }
    return null
}
```

## EDIT 4 — rewrite data_table_component + add _render_dt_live
Find this EXACT function:
```
fn data_table_component(opts) {
    let core = data_table(opts)
    let state_fn = | | { return {} }
    let render_fn = |s| { return _data_table_html(core) }
    let handler_names = ["dt_sort", "dt_next", "dt_prev", "dt_reload"]
    let handler_fns = [
        |s, v| { let _ = (core.set_sort)(v); return s },
        |s, v| { let _ = (core.next_page)(); return s },
        |s, v| { let _ = (core.prev_page)(); return s },
        |s, v| { let _ = (core.reload)(); return s }
    ]
    return lumen_component(state_fn, render_fn, handler_names, handler_fns)
}
```
Replace it with (per-connection version + a render helper):
```
// Per-connection live table. View state (sort/dir/page) lives in the component
// state (plain JSON, so it round-trips per connection); rows are queried at
// render time through the shared cache. The table is registered as watched so a
// DB write wakes every live loop, which re-renders from its own state.
fn data_table_component(opts) {
    let conn = _validate.get_field(opts, "conn")
    let table = _validate.get_field(opts, "table")
    let columns = _validate.get_field(opts, "columns")
    if columns == null { columns = [] }
    let page_size = _validate.get_field(opts, "page_size")
    if page_size == null { page_size = 25 }
    let init_sort = _validate.get_field(opts, "order_by")
    let init_dir = _validate.get_field(opts, "order_dir")
    if init_dir == null { init_dir = "asc" }
    let filters = _validate.get_field(opts, "filters")
    if filters == null { filters = [] }
    let cfg = http.create_struct(
        ["conn", "table", "columns", "page_size", "filters"],
        [conn, table, columns, page_size, filters]
    )
    _lumen_watch_table(table)

    let state_fn = | | {
        return http.create_struct(["dt_sort", "dt_dir", "dt_page"], [init_sort, init_dir, 1])
    }
    let render_fn = |s| { return _render_dt_live(cfg, s) }
    let handler_names = ["dt_sort", "dt_next", "dt_prev", "dt_reload"]
    let handler_fns = [
        |s, v| {
            if s.dt_sort == v {
                if s.dt_dir == "asc" { s.dt_dir = "desc" } else { s.dt_dir = "asc" }
            } else {
                s.dt_sort = v
                s.dt_dir = "asc"
            }
            s.dt_page = 1
            return s
        },
        |s, v| { s.dt_page = s.dt_page + 1; return s },
        |s, v| { if s.dt_page > 1 { s.dt_page = s.dt_page - 1 } return s },
        |s, v| { return s }
    ]
    return lumen_component(state_fn, render_fn, handler_names, handler_fns)
}

// Render the per-connection table from cfg (captured config) + s (per-conn state).
fn _render_dt_live(cfg, s) {
    let conn = _validate.get_field(cfg, "conn")
    let table = _validate.get_field(cfg, "table")
    let columns = _validate.get_field(cfg, "columns")
    let page_size = _validate.get_field(cfg, "page_size")
    let filters = _validate.get_field(cfg, "filters")
    let sort = _validate.get_field(s, "dt_sort")
    let dir = _validate.get_field(s, "dt_dir")
    if dir == null { dir = "asc" }
    let page = _validate.get_field(s, "dt_page")
    if page == null { page = 1 }

    let col_names = []
    let ci = 0
    while ci < columns.len() {
        let cn = _validate.get_field(columns[ci], "name")
        if cn != null { col_names = col_names.push(cn) }
        ci = ci + 1
    }
    if col_names.len() == 0 { col_names = ["*"] }

    let q = db.select(table).fields(col_names)
    let params = []
    let fi = 0
    while fi < filters.len() {
        let f = filters[fi]
        q = q.where(_validate.get_field(f, "field"), _validate.get_field(f, "op"))
        params = params.push(_validate.get_field(f, "value"))
        fi = fi + 1
    }
    if sort != null { q = q.order_by(sort, dir) }
    q = q.paginate(page, page_size)
    let compiled = q.build()

    let rows = []
    let err = null
    try { rows = cached_query(conn, compiled.sql, params, [table]) } catch e { err = "" + e }

    let ncols = columns.len()
    let head = "<thead><tr>"
    let hi = 0
    while hi < ncols {
        let col = columns[hi]
        let cname = _validate.get_field(col, "name")
        let clabel = _validate.get_field(col, "label")
        if clabel == null { clabel = cname }
        let sortable = _validate.get_field(col, "sortable")
        if sortable {
            let arrow = ""
            if sort == cname {
                if dir == "desc" { arrow = " ▼" } else { arrow = " ▲" }
            }
            head = head + "<th class=\"lmn-th\"><button class=\"lmn-th-btn\" @click=\"dt_sort\" value=\"" + cname + "\">" + _sanitize.escape_html("" + clabel) + arrow + "</button></th>"
        } else {
            head = head + "<th class=\"lmn-th\">" + _sanitize.escape_html("" + clabel) + "</th>"
        }
        hi = hi + 1
    }
    head = head + "</tr></thead>"

    let body = "<tbody>"
    if err != null {
        body = body + "<tr><td class=\"lmn-td lmn-td-msg\" colspan=\"" + ncols + "\">" + _sanitize.escape_html(err) + "</td></tr>"
    } else {
        if rows.len() == 0 {
            body = body + "<tr><td class=\"lmn-td lmn-td-msg\" colspan=\"" + ncols + "\">No rows.</td></tr>"
        } else {
            let ri = 0
            while ri < rows.len() {
                let row = rows[ri]
                body = body + "<tr>"
                let cj = 0
                while cj < ncols {
                    let cn2 = _validate.get_field(columns[cj], "name")
                    let cell = _validate.get_field(row, cn2)
                    let txt = ""
                    if cell != null { txt = _sanitize.escape_html("" + cell) }
                    body = body + "<td class=\"lmn-td\">" + txt + "</td>"
                    cj = cj + 1
                }
                body = body + "</tr>"
                ri = ri + 1
            }
        }
    }
    body = body + "</tbody>"

    let pager = "<div class=\"lmn-pager\"><button class=\"lmn-pager-btn\" @click=\"dt_prev\">Prev</button><span class=\"lmn-pager-info\">Page " + page + "</span><button class=\"lmn-pager-btn\" @click=\"dt_next\">Next</button></div>"
    let html = "<div class=\"lmn-table-wrap\"><table class=\"lmn-table\">" + head + body + "</table>" + pager + "</div>"
    return lumen_render(html, {})
}
```

## RULES
- Exactly these four edits. Do NOT remove or modify `data_table` / `_data_table_html` (the headless
  core stays for manual use). Change nothing else; reformat nothing.
- Preserve all `\{ \} \"` escaping.
- Confirm in your final message each edit's placement and that `data_table`/`_data_table_html` are
  untouched.
