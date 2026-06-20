# Lumen — server-driven live components

Lumen components render to HTML **on the server** (on top of Zenith WebSocket routes).
A tiny client runtime forwards events over a WebSocket; the server re-renders and morphs
the new HTML into the live DOM. No page reload, no client state, no Varian in the browser.

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
