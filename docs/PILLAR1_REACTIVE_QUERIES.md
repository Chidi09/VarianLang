# Pillar 1 — Reactive Live Queries (coarse, table-level) — IMPLEMENTATION SPEC

Goal: a `live_query` that auto-re-runs and re-renders when the DB tables it read change —
no manual invalidation. Coarse (table-level) first; structured to go fine-grained later.

Grounded in existing code:
- `src/lib_sqlite.c` — `sqlite.connect/query/close`; `conn` is a `sqlite3*` cast to int64.
- `vn_modules/db.vn` — `db.connect()` wrapper with `.query(sql,params)` / `.execute(sql,params)`.
- `vn_modules/lumen.vn` — `lumen_resource(fetcher)` (caches result, `refetch()` re-runs);
  `_lumen_async_updates = task.channel(64)` is the GLOBAL "wake all live loops to re-render"
  signal (`_lumen_async_updates <- true`); `_lumen_live_loop` polls it; VM scheduler is
  single-threaded + cooperative (NO locks needed). Pub-sub already wakes loops this way.

## Design (research-informed)
Convex-style read-set capture, but at **table granularity** (coarse). We REUSE the existing
wake+re-render+morph path; we only add: (1) DB change detection, (2) per-query table
dependencies, (3) a tick that drains changes → refetches dependents → wakes loops.

Deviations from the generic blueprint, with rationale:
- **`sqlite3_update_hook`, not the session/preupdate extension.** update_hook is always
  compiled in (no `SQLITE_ENABLE_*` flag, which our static libsqlite3 may lack) and gives the
  table name — all coarse tracking needs. Session/preupdate (byte diffs) deferred to fine-grained.
- **Reuse the existing WebSocket live loop + current morph**, NOT SSE/idiomorph. Transport
  (SSE) and morph-algorithm swaps are Pillar-5 / stateless-mode concerns; out of scope here.
- **Coarse table-level** invalidation (accept over-fetch) → upgrade to predicate/read-set later.
- **30ms debounce** via the tick interval to coalesce bulk writes (research §10).

## Contract A — C (`src/lib_sqlite.c`)
Add a process-global changed-table buffer + an update hook + a drain native.
1. A small global set of changed table names (dynamic `char**` + count, delimited or deduped).
   Single-threaded VM → plain globals are fine (add a `// TODO mutex if threaded` note).
2. In `lib_sqlite_connect`, after a successful `sqlite3_open`, call
   `sqlite3_update_hook(db, _sqlite_change_cb, NULL)`.
3. `_sqlite_change_cb(void*, int op, const char* dbname, const char* table, sqlite3_int64 rowid)`
   → append `table` to the global set (dedupe; ignore internal `sqlite_*` tables).
4. New native `sqlite.drain_changes()` → returns a `VAL_ARRAY` of changed table-name strings
   accumulated since the last call, then clears the buffer. Register via `vm_register_dispatch`.
   (Follow the existing `is_dispatch` arg pattern; root the array on the task stack like
   `lib_sqlite_query` does — see its GC-rooting comment.)

Keep `connect/query/close` behavior otherwise unchanged. Must compile clean with the project's
msys2 build (`make USE_POSTGRES=0`, `-pipe`).

## Contract B — `.vn` (`vn_modules/lumen.vn`, and db read-set hook in `vn_modules/db.vn`)
1. **Read-set capture** (table granularity, Convex-style):
   - Global `let _lumen_readset = [null]` (holder; `null` element = "not capturing").
   - In `db.vn`'s `.query` and `.execute` closures: if `_lumen_readset[0] != null`, append the
     tables from the SQL to it: `_lumen_readset[0] = _lumen_readset[0] + _lumen_sql_tables(sql)`.
     (Import/define `_lumen_sql_tables` so db.vn can call it, OR put the capture in a tiny shared
     helper. Simplest: define `_lumen_sql_tables` in lumen.vn and have db.vn reference it; if
     cross-module refs are awkward, duplicate the tiny parser in db.vn.)
2. **`_lumen_sql_tables(sql)`** — coarse SQL table extractor: lowercase a copy; collect the
   identifier following each `from`, `join`, `update`, `into` keyword; strip quotes/backticks;
   dedupe; return an array of table names. Good enough for coarse tracking; document that
   subqueries/CTEs are best-effort.
3. **Dependency registry** (globals in lumen.vn):
   - `_lumen_query_deps` : struct mapping `table -> [refetch_fn, ...]` (use http.create_struct /
     _tpl_bind helpers already used by pub-sub).
   - Register each live query's `refetch` under every table in its read-set.
4. **`live_query(fetcher)`** — same surface as `lumen_resource` (`{state, refetch}`):
   - `_lumen_readset[0] = []`  ; build the resource via the existing `lumen_resource(fetcher)`
     so the fetcher runs once (db.query populates the read-set) ; `let tables = _lumen_readset[0]`
     ; `_lumen_readset[0] = null`.
   - For each table in `tables`, append `res.refetch` to `_lumen_query_deps[table]`.
   - Return the resource (`{state, refetch}`) unchanged — templates use `res.state().data`.
5. **Reactive tick** — `_lumen_reactive_tick()`:
   - `let tables = sqlite.drain_changes()` ; if empty, return.
   - Collect the union of refetch fns for those tables (DEDUPE so a query touching 2 changed
     tables refetches once) ; call each refetch (re-runs its query, updates the resource holder).
   - Then `let _ = _lumen_async_updates <- true` to wake live loops → they re-render with fresh
     data and the existing morph pushes the diff.
   - Drive it from the live loop / scheduler on a ~30ms cadence (coalesces bulk writes). Wire it
     wherever `_lumen_async_updates` is already polled so it runs each tick before re-render.

## Don't break
- Existing `lumen_resource` / `lumen_async_resource` / pub-sub stay as-is.
- `db.query`/`db.execute` semantics unchanged when `_lumen_readset[0] == null` (the normal case).
- Coalescing must not drop updates (drain fully each tick).

## Delegation chunks (each individually verifiable)
- **Chunk 1 (C):** Contract A in lib_sqlite.c. Verify = compiles clean (I build via msys2).
- **Chunk 2 (.vn):** `_lumen_sql_tables` + read-set capture in db.vn + registry + `live_query`.
  Verify = review (interpreted; can't run on Windows).
- **Chunk 3 (.vn):** `_lumen_reactive_tick` + wire into the live loop + dedupe + 30ms cadence.
- Mirror any `vn_modules/*` change to `deploy/vn_modules/*`.
- Full runtime test deferred to Linux/WSL (untestable on Windows — LSP-only binary).

## Later (not now): fine-grained read-set (predicate/range), session-extension byte diffs,
SSE/stateless transport, idiomorph, server-windowed `<Table>` (Pillar 2).
