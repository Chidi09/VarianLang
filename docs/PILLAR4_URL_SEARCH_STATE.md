# Pillar 4 — Typed URL / search state

Goal: TanStack-Router-style typed search params, but server-driven. Declare a search schema
(type + default + optional `validate` validator per key); the URL query is parsed, coerced, and
validated into a typed struct on the initial render; component state changes are reflected back
into the address bar (so views are shareable / bookmarkable / back-button-able); and back/forward
restores state. Pairs with Pillars 1–3 (sort/filter/page/search live in the URL and drive the
live queries).

## Ground truth (from a DeepSeek read of lumen.vn + zenith.vn)
- `req.query` = raw query string on the zenith Request (zenith.vn:647); the page/load path gives a
  component `req` with `.path/.query/.params`. zenith has `query_params(req)`/`query(req,k,d)` but
  `lumen.vn` does NOT call `zenith.*` — so we parse the query ourselves in lumen.vn.
- `json_decode(str)` is the safe string→number/bool coercer (used at lumen.vn:2414).
- The shipped client `_lumen_client_core()` has NO history/pushState/popstate — we add it.
- `morph(a,h)` is the single choke point after every server update; the component root `a` is where
  we read a `[data-lumen-url]` marker.

## Design
Server (lumen.vn, append-only new fns):
- `_lumen_parse_query(raw)` → struct (char-scan split on `&`/`=`, `_lumen_qs_decode` on values).
- `_lumen_qs_decode(v)` / `_lumen_qs_encode(v)` → minimal percent/`+` codec for `space & = % #`.
- `_lumen_coerce(rawv, type)` → `json_decode` for int/number/bool (try/catch → null), raw for string.
- `lumen_search(req, schema)` → typed struct. schema = name→`{type, default, validator?}`. Missing/
  invalid → default. Reusable for popstate too (pass `{query: location.search}` as a fake req).
- `lumen_search_to_qs(values)` → `"?k=v&..."` (skips null/empty, encodes values).
- `_lumen_url_marker(values)` → hidden `<div data-lumen-url="...">` (attr escaped) a component renders
  so the client syncs the address bar.

Client (lumen.vn `_lumen_client_core`, hand-written):
- In `morph()`: after reconcile, read `a.querySelector('[data-lumen-url]')` → `history.replaceState`
  to `pathname + marker` when it differs (replaceState, not push — no history spam while typing).
- `popstate` listener → send `{t:'event', h:'lumen_nav', v:location.search}` (components without a
  `lumen_nav` handler ignore it).

## Status
Chunk 1 = the above. Example: `examples/lumen_search_url.vn` (a search box + sort whose state lives
in `?q=&sort=`; load seeds state from the URL, the marker writes it back, `lumen_nav` restores it on
back/forward). Untested at runtime (Windows LSP-only / `.vn` interpreted) — needs Linux/WSL.
v1 codec handles `space & = % #` only (documented); full percent-decode + pushState history +
data_table auto-URL-binding are later polish.
