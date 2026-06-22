# LumenJS — Complete Catalog & Zero-Bloat Bundling Spec

**For the implementing agent. Read the whole thing before touching `vn_modules/lumen.vn`.**

## What "LumenJS" is

**LumenJS** is the official, curated client runtime that ships *with* Lumen — a deliberately
**minuscule** amount of JavaScript that Lumen owns end-to-end. It is **not** "user JS" and it
is **not** a framework you write against. The user never writes JS; they write Varian. LumenJS
is the thin browser-side layer Lumen emits on their behalf — a transport plus a curated set of
browser-API shims — and the user only ever receives the slivers of it their page actually uses.
Think of it as the `--lumen-*` design tokens of the JS world: a small fixed vocabulary Lumen
maintains, tree-shaken to nothing-you-don't-use. Brand it `LumenJS` in code comments, the
`<script>` banner, and docs.

## 0. The one rule everything else derives from

**Varian owns all logic. The browser is a terminal.** LumenJS exists for exactly one
reason: to do the handful of things a server round-trip physically *cannot* or *should not*
do (browser-only APIs, or sub-16ms direct manipulation). Everything with business meaning —
cart math, totals, prices, validation results, auth, data fetching, sorting/filtering/paging
of server data — runs in Varian on the server, full stop. If a feature can be expressed as
`on="handler"` → a Varian `fn handler(s, v)`, it MUST be, and it ships **zero** hand-written JS.

LumenJS is a dumb transport plus a small, opt-in set of browser-API shims. None of its
modules contain app logic.

**Precondition:** this whole design assumes the server-driven `/live` path works. It currently
heap-corrupts on a child-component event (`free(): invalid next size (fast)` on add-to-cart).
That C-core bug must be fixed first — see `_lumen_live_loop` / `{{#each}}`-of-components
child-state handling. A modular client is pointless if the server path it talks to crashes.

## 1. Today's reality (what you're refactoring)

`_lumen_client_js()` in `vn_modules/lumen.vn` (~line 1984) returns **one** inlined IIFE string,
always injected in full by `_lumen_shell()`. It does:

- WebSocket connect + flat 1s reconnect (`setTimeout(c,1000)`).
- Event delegation over a fixed list `['click','input','change','submit','keydown','keyup','blur','focus']`, keyed by `data-lumen-<event>` attributes, sending `{t:'event',h:handler,v:value}`.
- Receives `{t:'html'}` (full) or `{t:'p'}` (prefix/suffix patch), morphs via `reconcile()`.
- Renders a runtime-error overlay on `{t:'error'}`.

Problems to fix: (a) it's monolithic — every page ships the entire thing; (b) only those 8
events, no directives for browser-only needs; (c) no per-page tree-shaking; (d) no hook
escape hatch; (e) reconnect has no backoff and drops events sent while disconnected.

## 2. Target architecture: core + opt-in modules + per-page tree-shaking

Split `_lumen_client_js()` into:

1. **`_lumen_client_core()`** — always included. The transport. ~2.5KB gz ceiling.
2. **`_LUMEN_MODULES`** — a map `name -> js_string`. Each module is self-contained, registers
   itself against its `data-lumen-*` attribute(s), and is **only emitted if the page uses it**.
   Per-module ≤ ~0.5KB gz.
3. **`_lumen_hooks`** — the escape hatch (§5). Only emitted if a `data-lumen-hook` is present.

### Bundling (the "only what they ask for" mechanism)

Lumen already statically rewrites `@event` → `data-lumen-event` at compile time
(`lumen.vn:49` and `:325`) and walks every template. Hang the directive scan off that pass:

- **`_lumen_scan_directives(template_src) -> [names]`** — run **once at compile time** per
  component. Detect which directive attributes (`data-lumen-copy`, `data-lumen-transition`, …),
  event modifiers (`.debounce`, `.confirm`, …), and `data-lumen-hook="X"` appear. Store the
  resulting **manifest** on the compiled component.
- A page's manifest = **union** of its own manifest and every child component it composes
  (composition is known statically; `{{#each Foo}}` still resolves to the single component
  type `Foo`, so its manifest is known). Compute the union at page-build time.
- **`_lumen_shell(inner, ws_path, manifest)`** assembles: `core + concat(_LUMEN_MODULES[n] for
  n in manifest) + hooks_if_any`. Dedup names. A page that only uses `on="click"` ships
  **core and nothing else**.
- **`lumen_client_include([...])`** — explicit override for HTML injected at runtime (raw_html,
  server-built strings) that the static scanner can't see. Adds the named modules to the
  manifest manually. This is the only manual knob; inference is the default so nobody forgets.

**Hard test of success:** a page with `data-lumen-copy` and nothing else ships `core + clipboard`
and zero other modules; a page with no directives ships `core` only; both work.

## 3. Core (always shipped) — harden while refactoring

Keep current behavior, plus:

- **Reconnect with backoff + event queue:** exponential backoff (250ms → 5s cap), and buffer
  events fired while `readyState !== 1`, flush on reconnect. A click during a blip must not vanish.
- **Loading/pending state:** while an event is in-flight, set `data-lumen-pending` on the
  triggering element (and a `lumen-pending` class on `[data-lumen-root]`) so CSS can show
  spinners/disable buttons with no JS authored by the user. Clear on next server frame.
- **Morph hardening:** already preserves focused input `value`; also preserve selection range
  and scroll position of the focused element across morphs.
- **Event modifiers (parsed from the attribute value or a sibling attr), all core-tiny:**
  `.prevent` `.stop` `.once` `.debounce(ms)` `.throttle(ms)` `.confirm("msg")` `.key(Enter)`.
  These keep keystroke floods, double-submits, and delete-confirmations off the server without
  any module. Example: `on="search" data-lumen-mod="debounce:300"`.

## 4. The complete module catalog ("every JS we'd ever need")

Each entry: **attribute** · what it does · **why it's JS** (must pass the §0 filter) · whether it
emits a server event. Tiers: **PURE** = browser API Varian cannot reach; **LATENCY** = server-capable
but a round-trip would feel wrong, opt-in; **NEVER** = do not write JS, listed so you don't drift.

### PURE — browser-only, no server equivalent

| Module | Attribute | Does | Emits server event? |
|---|---|---|---|
| `clipboard` | `data-lumen-copy="text or selector"` | copy/cut/paste to system clipboard | no |
| `focus` | `data-lumen-focus` / `data-lumen-trap` | autofocus on mount, focus-trap a modal/menu, restore focus on close | no |
| `scroll` | `data-lumen-scroll="into-view\|lock\|restore"` | scroll element into view, body-scroll-lock for modals, restore position | no |
| `transition` | `data-lumen-transition="name"` | enter/leave/FLIP move animations applied during DOM morph | no |
| `upload` | `data-lumen-upload` | file picker, drag-drop zone, client preview, chunked upload + progress | progress → server |
| `sortable` | `data-lumen-sortable="group"` | drag-reorder list (sub-16ms direct manipulation) | final order → server |
| `key` | `data-lumen-key="Escape:close,Enter:submit"` | global/element keymaps | mapped → server |
| `persist` | `data-lumen-persist="key"` | localStorage/sessionStorage (theme, dismissed banners, draft text) | no (optional sync) |
| `time` | `data-lumen-time="relative\|countdown"` | auto-tick "3m ago" / countdowns without per-second round-trips | no |
| `inview` | `data-lumen-inview="handler"` | IntersectionObserver: infinite-scroll sentinel, lazy data, viewport analytics | enter → server |
| `media` | `data-lumen-media="lazy\|lightbox\|autoplay"` | lazy `<img>`, lightbox, play/pause on visibility | no |
| `window` | `data-lumen-window="online\|theme\|resize"` | online/offline class, prefers-color-scheme, resize-driven classes | optional → server |
| `nav` | `data-lumen-nav` | pushState live navigation (URL changes without full reload during live nav) | path → server |
| `anchor` | `data-lumen-anchor` | tooltip/popover positioning, pointer-follow | no |
| `toast` | `data-lumen-toast` | display server-pushed ephemeral flash/toast messages, auto-dismiss | no |
| `confirm` | (core modifier `.confirm`) | native confirm() before sending a destructive event | gated → server |

### LATENCY — server-capable; client only to dodge a round-trip (opt-in, document the trade)

| Module | Attribute | Does | Note |
|---|---|---|---|
| `toggle` | `data-lumen-toggle="target"` | local show/hide, tabs, accordion, dropdown open/close | Prefer server `on=` unless the flicker of a round-trip is unacceptable. Pure presentation only — never gate data or auth on it. |

### NEVER — these stay in Varian; do not write JS for them

Cart quantities/totals, prices, currency math, tax/shipping, form **validation results**,
auth/session, permission checks, data fetching, sorting/filtering/paginating server data,
search results, any business rule. If you find yourself writing JS for one of these, stop —
it belongs in a Varian `fn` behind `on="..."`.

## 5. Escape hatch: hooks (the ONLY place users author JS)

For the rare thing not covered, a LiveView-style hook:

```
<div data-lumen-hook="Chart" data-points="...">...</div>
```
```js
// registered by the app, bundled ONLY if some template references data-lumen-hook="Chart"
lumen_hook("Chart", {
  mounted()  { /* el, this.pushEvent(name, payload) available */ },
  updated()  {},
  destroyed(){}
});
```

`pushEvent` rides the same WebSocket. Hooks are still tree-shaken: a hook's JS is emitted only
when its name appears in a rendered template. Keep this the documented "if you must" — most
apps should never define one.

## 6. Implementation checklist (in order)

1. Extract current `_lumen_client_js()` body into `_lumen_client_core()`; verify byte-identical
   behavior first (no modules yet), all 105 tests still pass.
2. Add core hardening from §3 (backoff+queue, pending state, modifiers, morph selection/scroll).
3. Introduce `_LUMEN_MODULES` map and move nothing into it yet; wire `_lumen_shell` to accept a
   manifest and concatenate `core + selected modules`.
4. Implement `_lumen_scan_directives()` in the compile pass; store manifest per component; union
   up the composition tree to the page.
5. Implement modules in priority order: `clipboard, focus, scroll, transition, toast, key,
   confirm(core), upload, inview, persist, time, media, sortable, window, nav, anchor, toggle`.
6. Implement `lumen_client_include()` override and the `hook` system (§5).
7. Convert Aurora's add-to-cart/login/logout/qty back to `on="..."` Varian handlers and **delete
   the `<client>` JS blocks** — they were a workaround for the `/live` crash, not a real need.

## 7. Budgets & acceptance

- Core ≤ ~2.5KB gz. Each module ≤ ~0.5KB gz. Typical page (core + 2–3 modules) ≤ ~4KB gz.
- A page never ships a module it doesn't statically reference (assert this in a test that diffs
  the emitted `<script>` for representative pages).
- No module contains app/business logic (§0). Reviewer rejects any that does.
- Every "NEVER" interaction in Aurora is a Varian `fn`, zero JS.
