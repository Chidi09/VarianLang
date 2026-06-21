# Lumen — the Varian frontend framework

**Lumen : Varian :: JSX : TypeScript.** `.lumen` files are components (markup +
bindings); the logic language underneath is Varian (`.vn`). Part of the ecosystem:
Varian (language) · Zenith (backend) · **Lumen (frontend)** · Kiln (build) · Constellation
(registry).

## Charter — be as capable as TypeScript/React, fix what they got wrong

The goal isn't "another framework." It's TS-level power with TS's papercuts removed —
the same bar Zenith set against Express/Fastify. Every Lumen decision is measured against
a specific TS/React shortcoming:

| TS / React pain | Lumen's answer |
| --- | --- |
| Config & build hell (tsconfig, webpack/vite/babel, dozens of deps) | Zero-config. `vn dev` / Kiln build, one binary, nothing to wire. |
| Cryptic, wall-of-text type errors | Friendly errors: file, line, caret, plain-English + a fix hint — reuse the hardened `errors` module. Dev gets a browser error overlay. |
| `any` / unsound escapes | Varian structural typing + `@validate` decorators on data. |
| Hydration mismatches, SSR complexity (Next.js) | **Server-driven live**: the server owns state and renders; there is no client/server divergence to mismatch. |
| `node_modules` + supply-chain risk | Batteries-included, ships in the binary. No npm, no lockfile. |
| Reactivity footguns (useEffect deps, stale closures, useMemo/useCallback) | State lives on the server in plain Varian. No dependency arrays, no stale closures, no memo ceremony. |
| Boilerplate (useState, forwardRef, prop drilling) | Plain Varian state + props; a handler is just a function. |
| XSS-by-omission (forget to escape) | `{{ }}` escapes **by default**; raw is the explicit `{{! }}`. |
| CSRF wiring | Auto-injected into every `.lumen` form (planned). |
| Fragmented commands (npm/npx/tsc/eslint/jest/vite) | One `vn` CLI, consistent verbs, same DX as the backend. |

Guiding rule: **clearer commands, clearer errors, better usability** — if a thing is
confusing in React/TS, Lumen makes it obvious or makes it disappear.

## Architecture — server-driven live (decided)

Components render to HTML **on the server** (on top of Zenith). Event bindings (`@click`,
`@input`, …) compile to `data-lumen-*` hook attributes. A tiny (~5KB) client runtime
forwards those events over a WebSocket; the server re-renders the component, diffs against
the previous HTML, and patches the DOM. State and logic stay in Varian on the server — no
Varian-in-browser, no hydration mismatch class of bugs, and it reuses Zenith + the
actor/task concurrency model directly. (LiveView/Hotwire lineage.)

## Markup syntax

```
{{ expr }}                          interpolate, HTML-escaped (safe by default)
{{! expr }}                         interpolate, raw / unescaped (trusted only)
{{#if expr}} .. {{else}} .. {{/if}}
{{#each items as item}} .. {{/each}}
@click="handler"                    -> data-lumen-click="handler"  (live hook)
```

Note: Varian strings treat `{` as interpolation, so authoring templates inside `.vn`
source uses brace escapes (`\{`, `\}` — now supported by the lexer). Real components live
in `.lumen` files read from disk, where `{{ }}` is written plainly.

## Milestones

- **M1 — server renderer (DONE).** `vn_modules/lumen.vn`: `lumen_render(tpl, ctx)` and
  `lumen_render_response(tpl, ctx)` covering `{{ }}` / `{{! }}` escaping, `{{#if}}`/`{{else}}`,
  `{{#each ... as ...}}`, and `@event` → `data-lumen-*`. Tested in `tests/lumen_test.vn`.
  Lexer gained explicit `\{` / `\}` string escapes (consistent across plain + interpolated
  strings) so braces can be written in Varian source at all.
- **M2 — the live layer (DONE).** `vn_modules/lumen.vn`: `lumen_component()`,
  `lumen_mount(app, path, component)`, `_lumen_live_loop` (non-blocking WebSocket
  event loop with `task.yield()`), `_lumen_shell`, and `_lumen_client_js()` (~1KB
  embedded JS with event delegation + DOM morph + reconnection). Wire protocol:
  `{"t":"event","h":"<name>","v":"<value>"}` up, `{"t":"html","html":"<HTML>"}` down.
  Scheduler finding: `ws.read()` is non-blocking; the cooperative single-threaded
  VM requires `task.yield()` in the live loop. Tested in `tests/lumen_live_test.vn`
  (dispatch + route registration, no browser needed). Example in
  `examples/lumen_counter.vn`.
- **M3 — `.lumen` single-file components (DONE).** A `.lumen` file = `<template>` markup + a
  Varian `<script>` logic block (`state` + handlers). `lumen_compile_source`/`lumen_compile_file`
  emit a backing `.vn` that calls `lumen_register_component(name, ...)`; `<UserCard id=".." prop=".." />`
  composition with per-child state, prop binding, and `id:handler` namespaced event routing
  through the live loop. Required new string primitives `ends_with` / `last_index_of` and a
  start-offset on `index_of` (registered in the parser builtin-method gate). Tested in
  `tests/lumen_m3_test.vn`; real example `examples/lumen/UserCard.lumen`.

### Roadmap from here (M4+) — assessed against "powerful but simple"

Ordered by value-to-complexity. Each must pass the charter test: does it remove a TS/React
papercut *without* adding a new concept users have to learn?

- **M4 — DOM patch protocol (perf, invisible to users).** Today the server sends the full
  re-rendered HTML (`{"t":"html",...}`) and the client morphs it. Keep the morph as the
  fallback, but add a server-side diff that emits `{"t":"patch","ops":[...]}` (text/attr/child
  ops keyed by a stable path) so only the changed bytes cross the wire. Biggest perf upgrade,
  and it changes **nothing** in the authoring model — purely under the hood. **Recommended next.**
- **M5 — reactive state helpers (DX).** Normal field updates already read cleanly
  (`s.count = s.count + 1; return s`). The rough edge is *adding/dynamic* keys (today
  `_tpl_bind`). Add an ergonomic state API — `s.set("k", v)` returning the new state, and a
  read sugar — so handlers never touch `_tpl_bind`. No signals/effects (those reintroduce the
  reactivity footguns the charter rejects); state stays plain server-side data.
- **M6 — slots / children.** `<Card> <h1>Hi</h1> </Card>`: capture inner markup as a `children`
  region a component renders with `{{! children }}`. High value, moderate parser work; composes
  with M3's prop binding.
- **M7 — DX to beat TS.** Friendly `.lumen` errors via the `errors` module + dev browser error
  overlay; `vn fmt`/`vn lint` understand `.lumen` (unescaped-input, N+1-in-loop); component
  snapshot testing; auto CSRF token in `.lumen` forms; HMR.
- **M8 — client islands (opt-in).** `<Chart client />` runs a component in the browser while the
  rest stays server-driven. Powerful but the highest-complexity item (needs a real client
  payload) — deliberately last, and strictly opt-in so the default model stays server-driven.
- **M9 — native perf port.** Move the hot render/diff path from `vn_modules/lumen.vn` into C
  (the Varian-first-then-native path Zenith took); cache static template segments.

CSS, SSR/SPA: **not separate milestones — already covered by the model.** Server-driven-live
*is* SSR by construction (server renders real HTML on every change; SPA-grade interactivity
comes from the WS morph — no separate hydration). CSS today is plain `<style>`/`class=` in
`.lumen` templates; a scoped-CSS sugar (auto-prefix a component's selectors) can ride along
with M6/M7 if wanted, but global CSS already works with zero new concepts.

## Open decisions (revisit when reached)

- Component state model for the live layer (per-connection actor vs. server session).
- `.lumen` `<script>` compilation: embed Varian directly, or compile to a backing `.vn`.
- Whether `lumen.*` becomes a native module namespace (vs. today's bare `lumen_render`).
