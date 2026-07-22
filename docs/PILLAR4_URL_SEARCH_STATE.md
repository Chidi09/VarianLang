# Pillar 4 — Typed URL / search state

Goal: TanStack-Router-style typed search params, but server-driven. Declare a search schema
(type + default + optional `validate` validator per key); the URL query is parsed, coerced, and
validated into a typed struct on the initial render; component state changes are reflected back
into the address bar (so views are shareable / bookmarkable / back-button-able); and back/forward
restores state. Pairs with Pillars 1–3 (sort/filter/page/search live in the URL and drive the
live queries).

## Runtime contract
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
- `_lumen_qs_decode(v)` / `_lumen_qs_encode(v)` implement UTF-8 percent encoding for URL
  components, decode `+` as space, and preserve malformed escape sequences literally.
- `_lumen_coerce(rawv, type)` strictly distinguishes `int`, `number`, `bool`, and `string`.
  For example, `2.5` is not accepted as an integer and `1` is not accepted as a boolean.
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
Shipped and runtime-tested. `tests/lumen_search_test.vn` covers UTF-8/reserved-character
round trips, strict coercion, validator refinement, deterministic serialization, safe HTML
markers, and invalid schema contracts. `examples/lumen_search_url.vn` demonstrates a search
box and sort order whose state lives in `?q=&sort=`; load seeds state from the URL, the marker
writes it back, and `lumen_nav` restores it on back/forward. Deliberate `pushState` navigation
and automatic data-table binding remain separate opt-in concerns.
