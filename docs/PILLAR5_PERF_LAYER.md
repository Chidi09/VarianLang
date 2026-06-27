# Pillar 5 — Perf layer (shared reactive query cache)

Goal: the perf win that lets the reactive data layer scale past React/TanStack — a server-side,
**shared, reactive query cache** (Convex-style). Identical queries across ALL connections share one
execution; a DB write auto-invalidates only the affected entries (per-table versioning driven by the
same change-detection the reactive tick already uses). Opt-in primitive — does NOT rewrite the
tested Pillar-1 reactive path.

## Design (lumen.vn, additive)
Globals (after `_lumen_reactive_started`):
- `_lumen_table_version` — holder of a struct `table -> int` (bumped on each change).
- `_lumen_qcache` — holder of a struct `key -> {result, sig, tables}`.
- `_lumen_cache_stats` — holder of `{hits, misses}`.

Functions:
- `_lumen_bump_versions(tables)` — increments the version of each changed table.
- `_lumen_version_sig(tables)` — a `"t:v;"` signature string of those tables' current versions.
- `cached_query(conn, sql, params, tables)` — key = `sql||json_encode(params)`. If a cached entry
  exists AND its `sig` equals the current version sig → HIT (return cached). Else run
  `(conn.query)(sql, params)`, store `{result, sig, tables}`, MISS. Caller passes the table name(s)
  the query reads (explicit — no SQL parsing dependency).
- `lumen_cache_invalidate(tables)` — manual bump (for Postgres / external writes that don't trip the
  sqlite update_hook).
- `lumen_perf_stats()` → `{cache_hits, cache_misses, cache_entries}`.

Tick hook (one line inside `_lumen_reactive_tick`, after the `tables.len()==0` guard):
`_lumen_bump_versions(tables)` — so a sqlite write bumps versions, staling matching cache entries.
Because the tick bumps versions BEFORE running refetches, the first refetch in a tick repopulates
the cache (MISS) and the rest reuse it (HIT) → one query per change across N connections.

## Status
Chunk 1 = the above. Example: `examples/lumen_perf_cache.vn` (an expensive aggregate served via
cached_query — repeated GETs HIT; a write bumps the version so the next GET re-runs once; stats
printed). Untested at runtime (Windows LSP-only / `.vn` interpreted) — needs Linux/WSL. Later:
TTL/LRU eviction (cache currently keeps entries until staled), routing live_query/data_table through
the cache for automatic N-connection sharing, payload compression, fine-grained (row-level) read sets.
