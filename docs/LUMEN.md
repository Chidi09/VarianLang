<p align="center">
  <img src="assets/lumen-logo.png" alt="Lumen" width="150" />
</p>

# Lumen — server-driven live components

Lumen is the Varian frontend framework — Varian's answer to Next.js / Nuxt. Components
render to HTML **on the server** (on top of Zenith WebSocket routes). A tiny (~5 KB)
client runtime forwards events over a WebSocket; the server re-renders and morphs the new
HTML into the live DOM. No page reload, no client state, no Varian in the browser, and —
because the server is the only place state lives and renders — **no hydration mismatch**.

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
