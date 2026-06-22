# Shopping Site Caveats — Phase −1 Outcomes

Each caveat is recorded with outcome: FIXED / FALLBACK / DEFERRED + reason.

---

## C1 — Blocking HTTP client

**Outcome:** FALLBACK
**Reason:** Full `curl_multi_*` integration is a significant C-core change. Implemented fallback:
- Added optional `timeout_ms` argument to `http.get(url [, timeout_ms])` and `http.post(url, headers, body [, timeout_ms])` in `src/lib_http.c` (default 30s)
- Left `// CAVEAT:` comments at both function declarations documenting the blocking nature and recommending queue worker-pool offload for production
- See `src/lib_http.c:57` and `src/lib_http.c:82`

---

## C2 — WebSocket/SSE not TLS-aware

**Outcome:** FALLBACK
**Reason:** TLS-aware WebSocket/SSE requires C-core changes to route socket writes through the TLS layer (would need TLS-aware `ws_send`/`ws_read` paths in both C socket layer and zenith.vn). Implemented fallback:
- Added `// CAVEAT:` comments in `zenith.vn` at `upgrade_websocket` (line 178), `sse_handshake` (line 203), and `listen_tls` (line 553) documenting that `wss://` is unsupported
- Documented the production workaround: terminate TLS at a reverse proxy (nginx/Caddy) that speaks plain `ws://` to Zenith
- Lumen reactivity (`lumen_mount`) silently breaks behind `listen_tls` — the browser tries `wss://` from the page protocol, but the server can't upgrade it

---

## C3 — Single-brace interpolation collision

**Outcome:** FIXED
**Reason:** Fixed `_lumen_rewrite_prop_braces` in `vn_modules/lumen.vn` to use proper brace-depth matching instead of naive `index_of("}")`. Nested braces like `data={ {key: val} }` now correctly find the outer `}`. Also handles quoted strings (`"`) inside prop values by tracking string state and not counting braces inside them. The `.lumen` file path's escape-then-decode round-trip (`_lumen_escape_string` → Varian string parser → rewrite → render) already preserved single braces correctly; the fix was in the rewrite step.

**Remaining constraint (documented):** Prop values using `{expr}` shorthand must not contain unescaped `"` inside the expression (use `key="string"` for string values). Complex expressions with nested quotes should use `attr="{{ expr }}"` (double-brace syntax) directly instead of the `attr={expr}` shorthand.

---

## C4 — No dynamic page routes

**Outcome:** FIXED
**Reason:** Extended `_lumen_collect_pages` to detect `[param].lumen` filenames, convert to `:param` route syntax, generate valid component names. Added optional `param` argument to `lumen_mount` — when non-empty, `req.params[param]` is merged into the component's initial state. Updated `_lumen_build_dir` to pass param to `lumen_mount`.

---

## C5 — Default scaffold is raw HTML

**Outcome:** FIXED
**Reason:** Rewrote the embedded `pages/index.lumen` template in `src/main.c` (`lumen_new`) to use the component vocabulary (`<Page>`, `<Section>`, `<Container>`, `<Stack>`, `<Heading>`, `<Text>`, `<Button>`). Increased the page buffer from 1100 to 4096 bytes. The `<svg>` logo with `@click="pulse"` is preserved as the one bespoke element.

