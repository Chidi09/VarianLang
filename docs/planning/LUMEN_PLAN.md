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
- **M3 — `.lumen` single-file components.** A `.lumen` file = `<template>` markup + a Varian
  `<script>` logic block (state + handlers). Loader/compiler that registers a component by
  name and wires its handlers to the live layer. `<UserCard ... />` composition.
- **M4 — DX to beat TS.** Friendly `.lumen` errors via the `errors` module + dev browser
  overlay; `vn fmt`/`vn lint` understand `.lumen` (unescaped-input + N+1-in-loop warnings);
  snapshot testing of component output; auto CSRF token in forms; HMR ("hot reload without
  page refresh").
- **M5 — perf.** Push the hot render/diff path from `vn_modules/lumen.vn` into C (the same
  Varian-first-then-native path Zenith took), and cache static template segments.

## Open decisions (revisit when reached)

- Component state model for the live layer (per-connection actor vs. server session).
- `.lumen` `<script>` compilation: embed Varian directly, or compile to a backing `.vn`.
- Whether `lumen.*` becomes a native module namespace (vs. today's bare `lumen_render`).
