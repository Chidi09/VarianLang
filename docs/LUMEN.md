<p align="center">
  <img src="assets/lumen-logo.png" alt="Lumen" width="150" />
</p>

# Lumen — server-driven live components

Lumen is Varian's server-driven **frontend** framework — the `React` to Aurora's `Next.js`.
Components render to HTML **on the server** (on top of Zenith WebSocket routes). **Lumen JS**
— a tiny (~2 KB) client runtime — forwards events over a WebSocket; the server re-renders
and morphs the new HTML into the live DOM. No page reload, no client state, no Varian in
the browser, and — because the server is the only place state lives and renders — **no
hydration mismatch**.

You can use Lumen standalone (`vn lumen new myapp` → a Lumen-only frontend served by any
backend) or as the frontend half of **Aurora** (`vn new myapp` → full-stack with Zenith +
Lumen + batteries).

### Why Lumen over React / Vue / Svelte

| Concern | React / Vue / Svelte | Lumen |
|---|---|---|
| **Rendering model** | Client VDOM + hydration — 50–400 KB framework | Server-driven HTML over WebSocket — **~2 KB inline Lumen JS** |
| **Hydration mismatch** | Common bug | **Impossible** — server owns all state and rendering |
| **UI components** | None built-in (need MUI, Chakra, Shadcn) | **28 built-in** — `<Page>`, `<Grid>`, `<Card>`, `<Hero>`, etc. |
| **State management** | External (Zustand, Pinia, stores) | **Built-in** `lumen_store()` |
| **Async data** | External (React Query, TanStack Query) | **Built-in** `lumen_resource()`, `lumen_async_resource()` |
| **Form validation** | External (Zod, VeeValidate, yup) | **Built-in** `lumen_form()` — Zod-style |
| **Pub-sub / broadcast** | External library or manual WebSocket | **Built-in** `lumen_publish()`, `lumen_subscribe()`, `lumen_broadcast_store()` |
| **CSS scoping** | Compiler plugin or CSS modules | **Built-in** `data-lumen-css` attribute rewrite |
| **Routing** | External (React Router, Vue Router, svelte-spa-router) | **File-based routing** — `pages/index.lumen` → `/` |
| **SSG** | Framework-specific plugin | **Built-in** `lumen_build_static_dir()` |
| **Scaffold** | `create-react-app`, `npm create vue`, etc. | **One command** — `vn lumen new myapp` |

This page has two halves:

1. **[The developer guide](#the-developer-guide-lumen--vn-dev)** — `.lumen` files, `vn dev`,
   the dev console, file routing, and the batteries every project ships with. Start here.
2. **[The runtime reference](#public-api)** — the underlying `lumen_component` / `lumen_mount`
   API the compiler targets. Read this if you're embedding Lumen by hand or hacking on it.

---

## The developer guide (Lumen + `vn dev`)

### Quick start

```sh
vn lumen new myapp     # scaffold pages/ + public/ (favicons, manifest) + a starter component
cd myapp
vn dev                 # serve ./pages with live reload + the dev console (default :8090)
```

Open `http://localhost:8090/` and click the logo — its colour is reactive server state.

### The `.lumen` file format

A component is up to three blocks in one file:

```html
<template>            <!-- markup with {{ }} bindings and @event hooks -->
  <button @click="pulse">{{ count }}</button>
</template>

<script>              <!-- plain Varian: a state() fn + handler fns + helpers -->
fn state() { return { count: 0 } }
fn pulse(s, v) { return s.set("count", s.get("count") + 1) }
</script>

<client>              <!-- OPTIONAL: verbatim browser JS for a client-only island -->
/* runs once on first paint; left untouched by the live DOM morph */
</client>
```

How it compiles (`lumen_compile_source` in `vn_modules/lumen.vn`):

- `<template>` is escaped and embedded as the component's render string.
- Every `fn` in `<script>` **except `state`** is collected as a handler (so helper
  functions like `_hue` are in scope for your handlers too — they just never fire as
  events). `state()` supplies the initial state.
- The result is a generated `_lumen_init_component_<Name>()` that calls
  `lumen_component(...)` and registers it. The language has no `eval`, so `vn dev` does
  this in a build pass that emits one runnable `.vn`, then serves it.

### Reactivity model

State is **plain Varian data on the server**. There is no `useState`, no signals, no
effect dependency arrays, no virtual DOM in the browser. The whole loop is:

```
click ─▶ data-lumen-click ──WS──▶ run handler ▶ new state ▶ re-render ▶ diff ──WS──▶ morph
```

A handler takes `(state, value)` and returns the **new** state. State helpers are immutable
and arena-safe — `s.set(k, v)` returns a *new* struct, and they chain:

```varian
fn pulse(s, v) {
  let n = s.get("count") + 1
  return s.set("count", n).set("color", _hue(n))   // immutable, chainable
}
```

The scaffolded starter wires `{{ color }}` into an SVG `fill`, so each click recomputes a
colour server-side and Lumen morphs **only the changed attribute** into the DOM — a live
demonstration of the model with zero client code.

### File-based routing

`vn dev <dir>` scans `<dir>` for `.lumen` files and maps each to a route (`_lumen_route`):

| File | Route |
| --- | --- |
| `pages/index.lumen` | `/` |
| `pages/about.lumen` | `/about` |
| `pages/user-card.lumen` | `/user-card` (component name PascalCased to `UserCard`) |

### The interactive dev console

`vn dev` prints a Nuxt/Next-style console and then watches for changes:

```text
   LUMEN   v0.1.0   the Varian frontend framework

  ➜  Local     http://localhost:8090/
  ➜  Pages     2 in pages/

     ● /                  index.lumen
     ● /about             about.lumen

  ✔ ready in 142 ms  · watching pages/ — edit a page to hot-reload
```

- **Live reload.** Save any `.lumen` file → rebuild + restart; the browser's runtime
  auto-reconnects and re-renders. A build that fails mid-edit keeps the last good server
  up and prints `✖ build error — keeping the last good page up`.
- **Colour** is emitted only to a TTY and is suppressed by `NO_COLOR`.
- Implemented in `src/main.c` (`lumen_dev`, `lumen_print_banner`); the framework's own
  `http.serve` startup line is silenced via the `LUMEN_QUIET` env var so the console is
  the single source of truth.

### Batteries included (no extra deps)

Every page Lumen serves gets, with zero setup, the same defaults `create-next-app` /
`nuxi init` / `create-vite` give you — injected by `_lumen_head()`:

- **Favicon set + `manifest.json`.** `vn lumen new` copies a full icon set
  (`favicon.ico`, `favicon-16/32`, `apple-touch-icon`, `android-chrome-192/512`) and a
  themed web-app manifest into `public/`. The generated app serves `public/` via
  `app.serve_static("/", "public")` — consulted only *after* routing, so page routes
  always win and a missing asset just 404s. The bundled source lives in
  `vn_modules/lumen_assets/` so an installed binary finds it too.
- **Responsive meta** — `<meta viewport>`, `theme-color`, `lang="en"`.
- **The Degular typeface**, loaded via Adobe Fonts, wired through a CSS variable with a
  full system-font fallback so a page looks right even before the webfont arrives.
- **Escape-by-default rendering** and the branded **error overlay** (below).

### Error overlay

A runtime error inside a handler is caught in `_lumen_live_loop` and pushed to the browser
as a Lumen-branded overlay (navy/amber, inline-SVG logo) carrying the friendly
`errors.explain()` what/fix text. It auto-clears on the next successful render. Dismissable;
dev-only by nature (it only appears when a live handler throws).

### DOM patch protocol (M4)

After the first full render per connection, the server sends a **minimal splice patch**
instead of full HTML: it computes the longest common ASCII prefix/suffix between the last
HTML and the new HTML and sends only the changed middle as `{"t":"p","s":p,"e":q,"d":mid}`.
Prefix/suffix are kept ASCII so server byte offsets equal the client's UTF-16 string
indices (Unicode in the changed middle is sent verbatim and stays safe). The client
reconstructs `prev[0:s] + d + prev[len-e:]` and morphs. Invisible to authors.

### Composition & slots

- **Child components.** `<UserCard id="u1" name="Ada"/>` renders a child with props and its
  own server-side event scope — events are namespaced `id:handler` so two instances of the
  same component never collide.
- **Slots.** `<Card> inner markup </Card>` projects into the card's `{{! children }}`;
  interactive components nested inside a slot keep their own scope.

### Client islands (M8)

For a genuinely client-only widget (chart, canvas, map), add a `<client>` block of real
browser JS. It's embedded as a `<script>` that runs once on first paint and is left
untouched by the morph (`cloneNode`/`innerHTML` never re-run scripts). This is the *honest*
island — real client code where you ask for it, the rest still server-driven. Lumen
deliberately does **not** compile Varian to a browser bundle; that's exactly what
reintroduces hydration-mismatch bugs.

### Lumen UI (Component Registry)

Lumen ships with an official, Shadcn-inspired component registry called **Lumen UI**. Instead of an external black-box package, Lumen UI provides beautifully designed, accessible components that are copied directly into your codebase (in `pages/components/`) so you fully own and customize the code.

Add components via the CLI:
```sh
vn lumen add button
vn lumen add card
```

**Features:**
- **Zero Client JS:** Components like `Accordion`, `Dialog`, and `Select` are driven entirely by server-side Varian state and scoped CSS.
- **Theme-Aware:** Lumen is "Light Mode First" but ships with native Dark Mode. Global CSS variables (`var(--lumen-bg)`, `var(--lumen-primary)`, etc.) adapt automatically when the `dark` class is toggled on the `<html>` element. Use `vn lumen add theme-toggle` for a pre-built switcher.
- **Inline SVG Icons:** Icons are pure, inline Lucide SVG vectors baked directly into the `.lumen` files. They inherit `currentColor`, cause zero layout shift, and require no external CDN scripts.
- **Batteries Included:** Available components: `button`, `card`, `input`, `dialog`, `badge`, `accordion`, `alert`, `progress`, `select`, `checkbox`, `switch`, `separator`, and `theme-toggle`.


### CLI reference

| Command | Effect |
| --- | --- |
| `vn lumen new <name>` | Scaffold `pages/` + `public/` (favicons + manifest) + a starter component |
| `vn dev [dir] [port]` | Serve a pages dir with live reload + the dev console (default `./pages :8090`) |
| `vn lumen build <dir> <out.vn> [port]` | Compile a pages dir into one runnable Varian app |

---

## Public API

### `lumen_component(state_fn, render_fn, handler_names, handler_fns)`

Create a component struct from four pieces:

| Argument | Description |
| --- | --- |
| `state_fn` | `\| \| { return <initial state> }` — zero-arg closure returning the initial state |
| `render_fn` | `\|state\| { return lumen_render(template, state) }` — renders state to HTML |
| `handler_names` | Array of handler name strings, e.g. `["inc", "dec"]` |
| `handler_fns` | Array of handler closures `\|state, value\| { ...; return state }` |

Handler names and functions are parallel arrays (same index = same handler). Handler
closures receive the current state and the event value (the element's `.value`, or `null`)
and must return the **new** state (mutation works but the returned value is what's used).

Returns a struct (built with `http.create_struct` so there's no parse-order dependency
on zenith.vn's struct declarations).

### `lumen_mount(app, path, component)`

Mount a live component on a Zenith app at `path`. Registers two routes:

- **`GET <path>`** — renders the component's initial HTML wrapped in the live shell
  (`data-lumen-root` container + embedded client script) and returns `text/html`.
- **`GET <path>/live`** — WebSocket upgrade endpoint. Upgrades to RFC 6455, then enters
  the per-connection event loop until the socket closes.

## Wire Protocol

Both directions are JSON text frames over a single WebSocket.

**Client → Server** (an event happened):
```json
{"t":"event","h":"<handlerName>","v":"<value-or-null>"}
```

- `h` = the `data-lumen-<event>` attribute value (originally `@click="handlerName"`).
- `v` = the element's `.value` for input-ish elements, else `null`.

**Server → Client** (re-rendered HTML):
```json
{"t":"html","html":"<full component HTML>"}
```

The client morphs (not replaces) the new HTML onto the live DOM, preserving focus and
input values in untouched fields.

## Per-Connection State

Each WebSocket connection gets its own independent state held in a local variable in the
`_lumen_live_loop` function. State lives exactly as long as the socket. Shared/multi-user
state via actors is a future milestone.

## Security Rules

1. **Handler names from the wire are resolved only against the registered set.**
   An unknown name is silently dropped — never evaluated.
2. **Re-rendering goes through `lumen_render`, which HTML-escapes `{{ }}` by default.**
   Never bypass it.

## Scheduler Model

The Varian VM is a single-threaded cooperative round-robin scheduler.
`ws.read()` is **non-blocking**: it returns `""` (empty string) when no data is available
(EAGAIN), not `null`. The per-connection live loop calls `task.yield()` when there's no
message, allowing the scheduler to service other connections/tasks on the next tick.

## Example

```varian
let counter = lumen_component(
    | | { return { count: 0 } },
    |s| { return lumen_render("<div><p>Count: {{ count }}</p>" +
                              "<button @click=\"inc\">+</button>" +
                              "<button @click=\"dec\">-</button></div>", s) },
    ["inc", "dec"],
    [ |s, v| { s.count = s.count + 1; return s },
      |s, v| { s.count = s.count - 1; return s } ]
)

let app = new_app()
lumen_mount(app, "/", counter)
app.listen(8090)
```

Run it: `./vn run examples/lumen_counter.vn`, open `http://localhost:8090`.

---

## What Lumen ships at once

A typical React or Vue project needs a scaffold tool, a router, a state library, a
data-fetching lib, a form validator, a component library, a CSS scoping solution, an SSG
plugin, and an SEO plugin — 9+ separate packages, each with its own version, changelog,
and CVE surface.

Lumen ships all of this in one runtime, zero `npm install`:

| Capability | React / Vue / Svelte | Lumen |
|---|---|---|
| UI components | None (MUI, Chakra, Shadcn) | **28** — `<Page>`, `<Grid>`, `<Card>`, `<Hero>`, `<Button>`, etc. |
| Server-driven DOM engine | None (VDOM + hydration) | **Built-in** — splice-patch protocol, ~2 KB Lumen JS inline |
| File-based routing | React Router, Vue Router | **Built-in** — `pages/index.lumen` → `/` |
| Dynamic route params | External lib feature | **Built-in** — `[id].lumen` → `/:id` |
| Scoped CSS | CSS modules, styled-components | **Built-in** — `data-lumen-css` attribute rewrite |
| Reactive store | Zustand, Pinia | **Built-in** — `lumen_store()`, `lumen_broadcast_store()` |
| Async data fetching | React Query, TanStack Query | **Built-in** — `lumen_resource()`, `lumen_async_resource()` |
| Pub-sub / broadcast | Manual WebSocket | **Built-in** — `lumen_publish()`, `lumen_subscribe()` |
| Form validation | Zod, VeeValidate, yup | **Built-in** — `lumen_form()` Zod-style |
| SSG | next export, manual | **Built-in** — `lumen_build_static_dir()` |
| SEO metadata | next/head, react-helmet | **Built-in** — `lumen_meta()` |
| Client islands | None / manual | **Built-in** — `<client>` blocks |
| Inline SVG icons | CDN or bundler | **Built-in** — Lucide icons, zero CDN |
| Dark mode | Manual CSS | **Built-in** — CSS variable tokens |
| Live reload | HMR plugin | **Built-in** — `vn dev` |
| Error overlay | Custom setup | **Built-in** — branded in-browser overlay |
| Test helpers | Testing Library, Enzyme | **Built-in** — `lumen_test_render()`, `lumen_test_event()` |
| Favicon + manifest | Manual setup | **Built-in** — scaffolded by `vn lumen new` |
| **Total packages needed** | **9+** (router, state, fetch, forms, UI, CSS, SSG, SEO, icons) | **0** (one binary) |

### Engineering patterns Lumen proves

- **Stateless execution by default** — Every handler receives state and returns new state
  (`vn_modules/lumen.vn:1497-1513`). The server owns all rendering; there is no client-side
  `useState`. Each WebSocket connection holds its own `state` variable in a local
  `_lumen_live_loop` scope — no global mutable state.
- **Guard clauses** — Every handler starts with null checks before business logic
  (`aurora/lib/api_cart.vn:5-7: if session == null { return [] }`).
- **Event-driven decoupling** — `lumen_publish`/`lumen_subscribe`
  (`vn_modules/lumen.vn:1326-1345`) uses a channel-backed notification bus
  (`_lumen_async_updates`, line 191) to decouple producers from all live-loop consumers.
- **Cooperative concurrency** — The live loop calls `task.yield()` when idle
  (`lumen.vn:1790`); the single-threaded scheduler (`src/vm.c:3262-3317`) round-robins
  between connections with zero race conditions.
- **Parse, don't validate** — `lumen_form()` parses raw form input into `{ok, values, errors}`
  at the boundary; inside the system the type guarantees validity.
