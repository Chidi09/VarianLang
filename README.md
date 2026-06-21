<p align="center">
  <img src="docs/assets/varian-logo.png" alt="Varian — Built for Performance" width="560" />
</p>

<h1 align="center">Varian</h1>

A blazing fast, concurrent, systems-level programming language built from scratch in C with a custom bytecode VM. Varian combines the speed of C, the concurrency model of Go (Actors & Channels), and the ecosystem of Python into a single, cohesive developer experience.

---

## Why Varian?

*   **Python Ecosystem**: Direct access to PyTorch, NumPy, and Pandas without boilerplates or wrapper code.
*   **Go-Style Concurrency**: Scale cleanly using cooperatively scheduled green-thread tasks, actors, and channels.
*   **C-Level Extensibility**: Low-latency execution via native C modules and a near-zero-overhead FFI.
*   **Arena-Optimized Execution**: High-throughput request handling with task-local allocation arenas.
*   **Optional AOT Compilation**: Transpile your Varian modules into standalone, optimized C executables.
*   **All-in-One Toolchain**: Integrated formatter (`vn fmt`), test runner (`vn test`), and linter (`vn lint`).

---

## The Cheat Code Architecture
Varian doesn't force you to rewrite the world. It comes built-in with a near-zero-overhead **C FFI** and a fully integrated **Python Bridge**. You can import and use any Python package (like NumPy or PyTorch) seamlessly as if it were written in Varian.

```swift
fn array(args) {
    return python.run("numpy", "array", [args])
}
```

---

## Features

### ⚡ Thread-Safe Concurrency (Actors & Channels)
Say goodbye to mutex locks and race conditions. Varian uses a cooperative spin-yield scheduler and the Actor model to run thousands of lightweight tasks concurrently.

```swift
actor Counter {
    count: int = 0,
    fn increment(self) { self.count = self.count + 1 }
    fn get(self) -> int { return self.count }
}

let c = Counter.spawn()
c.increment()
print(c.get())
```

### The Zenith Web Framework

<p align="center">
  <img src="docs/zenith_logo.png" alt="Zenith Logo" width="300" />
</p>

Varian includes **Zenith**, a hyper-optimized, non-blocking HTTP web framework built directly into the language runtime. We engineered Zenith to deliver high-performance throughput, bypassing the traditional bottlenecks of dynamic languages.

#### 🦁 Per-Request Arena Architecture
Zenith fundamentally reimagines how a high-level language handles network traffic:
*   **`io_uring` & `writev`**: Bypasses traditional blocking syscalls with Linux's ultra-fast asynchronous I/O and scatter/gather response serialization.
*   **Per-Request Arena Allocators**: Memory for each request is allocated in a single contiguous chunk. When the request ends, the memory is instantly reclaimed by resetting a pointer—resulting in **Zero Garbage Collection overhead** during the request lifecycle.
*   **AOT Transpilation**: Zenith server code can be natively transpiled to C. This bypasses the bytecode interpreter entirely, allowing the web framework to execute at raw machine speeds while retaining Varian's high-level ergonomics.
*   **Shared-Nothing Multi-Threading**: Using `SO_REUSEPORT`, Zenith boots independent VM instances across multiple cores. No Global Interpreter Lock (GIL), no lock contention—just pure linear scaling.

#### 📊 Performance & Benchmarks

To establish transparency, we measure the throughput of a plain text response (`"Hello, World!"`) using `wrk` with 100 concurrent connections across 4 threads. Varian’s custom computed-goto interpreter and per-request task arenas deliver extreme performance for an interpreted VM, positioning it exceptionally close to compiled servers.

| Web Server / Runtime | Throughput (Req/Sec) | Relative Performance |
| :--- | :--- | :--- |
| **Rust (Actix-web, 8 Workers)** | ~120,000 req/sec | 3.5x |
| **Go (Gin, 8 Workers)** | ~70,000 req/sec | 2.0x |
| **Varian (Zenith Cluster, 8 Workers)** | **~34,250 req/sec** | **1.0x** |
| **Varian (Zenith Single Worker)** | **~7,060 req/sec** | **0.2x** |

*Benchmarks executed on Linux (AMD Ryzen 8-Core/16-Thread Virtualized Host).*

#### 🛠️ Expressive & Ergonomic
Despite its native-level performance, Zenith feels as intuitive as frameworks like Express or FastAPI.

```swift
let app = new_app()

// Middleware
app.use(|req| {
    print("Incoming request: " + req.method + " " + req.path)
    return null
})

// Path Parameters & JSON
app.get("/users/:id", |req| {
    let user_id = req.params["id"]
    return Response { 
        status: 200, 
        body: json_encode({ "user": user_id, "status": "active" }), 
        content_type: "application/json" 
    }
}, "Get User")

// Cluster mode automatically binds across cores
app.listen(3000)
```

#### 🛡️ Enterprise-Ready Security
Zenith ships with high-performance native security modules built directly in C:
*   **Zero-Allocation CORS & CSRF:** Fully compliant, automatically injected headers with no string-allocation overhead.
*   **Token Bucket Rate Limiting:** Millisecond-precision rate limiters powered by the native scheduler.

#### ⚡ Compile-Time SQL ORM
Zenith includes a query builder designed for Varian's `comptime` engine. The structure of your SQL queries (tables, selected fields, operator shapes) is resolved at compile-time and baked directly into the program as static strings. You get the safety and ergonomics of an ORM with the raw runtime performance of hand-written SQL.

```swift
let compiled = comptime {
    select("users")
        .fields(["id", "name", "email"])
        .where("id", "=")
        .limit(10)
        .build()
}
// At runtime, compiled.sql is just a static string: 
// "SELECT id, name, email FROM users WHERE id = ? LIMIT 10"

let conn = sqlite.connect("database.db")
let bound = bind(compiled, [user_id])
let rows = run_sqlite(bound, conn)
```

#### 📚 Instant OpenAPI Docs
Forget maintaining separate API documentation. Zenith auto-generates a Swagger UI and OpenAPI JSON specs from your routes and runtime structs in a single line.
```swift
app.enable_docs("/docs")
```

#### 🔀 Advanced Radix Routing & Testing
*   **Radix Trie Routing**: Routes are stored in a highly optimized segment trie. Lookup costs one trie descent per path segment rather than an O(N) scan.
*   **Socket-Free Testing**: Run full route, middleware, and handler pipelines synchronously without ever opening a real HTTP port. Perfect for blisteringly fast unit tests.
```swift
// No network stack involved!
let resp = app.handle(fake_req("GET", "/api/users/2"))
```

#### 🔌 Real-Time & Streaming
Zenith supports real-time communication out of the box with zero external dependencies.
*   **WebSockets**: Easily upgrade connections and send/receive masked payloads natively.
*   **Server-Sent Events (SSE)**: Simple API for streaming fast updates to clients.
```swift
app.get("/ws", |req| {
    let ws = upgrade_websocket(req)
    ws.write("Connected!")
    return Response { _keep_open: true }
}, "WebSocket upgrade")
```

#### 🔒 Static Serving & TLS
*   **Static Mounts**: Serve static directories with built-in path-traversal protection and automatic MIME type resolution.
*   **Native HTTPS**: Pass your SSL certificates directly to the cluster to handle HTTPS internally.
```swift
app.serve_static("/assets", "public/assets")
app.listen_tls_cluster(443, "cert.pem", "key.pem", 8)
```

---

<p align="center">
  <img src="docs/assets/lumen-logo.png" alt="Lumen Logo" width="170" />
</p>

<h2 align="center">Lumen — the Varian frontend framework</h2>

<p align="center"><i>Server-driven. Live by default. Zero config, zero <code>node_modules</code>, zero hydration mismatch.</i></p>

If Zenith is Varian's answer to Express/FastAPI, **Lumen** is its answer to **Next.js and Nuxt** — a full-stack UI framework that ships *inside the `vn` binary*. There is no `npm install`, no `tsconfig`, no bundler to configure, and no separate client/server codebase to keep in sync. You write `.lumen` components, run `vn dev`, and you have a live, reactive app.

> **Lumen : Varian :: JSX : TypeScript.** A `.lumen` file is markup + bindings; the logic underneath is plain Varian (`.vn`).

### The core idea: server-driven live

Most React/Next apps run your component logic *twice* — once on the server (SSR) and again in the browser (hydration) — which is the entire source of the dreaded *"hydration mismatch"* class of bugs. Lumen does not do this.

In Lumen, **the server owns all state and does all rendering.** The browser runs a tiny (~5 KB) runtime whose only jobs are: forward DOM events over a WebSocket, and patch the DOM with whatever HTML the server sends back. State lives in plain Varian on the server — no `useState`, no `useEffect` dependency arrays, no stale closures, no `useMemo` ceremony.

```
┌────────── Browser ──────────┐         ┌────────────── Server (Varian) ──────────────┐
│  click ─▶ data-lumen-click  │ ──WS──▶ │  run handler ▶ new state ▶ re-render HTML    │
│  morph DOM ◀── DOM patch ── │ ◀──WS── │  diff vs last HTML ▶ send minimal splice     │
└─────────────────────────────┘         └──────────────────────────────────────────────┘
```

Because the server renders the *real* HTML on every change, **SSR and SPA-grade interactivity are the same mechanism** — there is nothing to "hydrate," so there is no mismatch to debug.

### Anatomy of a `.lumen` component

A component is a `<template>` (markup), a `<script>` (Varian logic), and an optional `<client>` (browser-only JS). This is the starter `vn lumen new` scaffolds — a Lumen logo whose colour is reactive server state:

```html
<template>
  <main style="display:grid;place-items:center;min-height:100vh">
    <!-- @click compiles to a live event hook; {{ color }} is reactive state -->
    <svg @click="pulse" viewBox="0 0 48 48" width="150">
      <rect x="3" y="3" width="42" height="42" rx="12" fill="#1b2233"/>
      <path d="M26 7 L15 27 h7 L19 41 L33 21 h-8 L29 7 Z"
            fill="{{ color }}" style="transition:fill .35s ease"/>
    </svg>
    <p>pulse <b>{{ count }}</b></p>
  </main>
</template>

<script>
fn _hue(n) {
  let palette = ["#f5b829", "#ff6b6b", "#4dd4ac", "#5b9cff", "#c77dff", "#ff9f43"]
  return palette[n % 6]
}
fn state() { return { count: 0, color: "#f5b829" } }
fn pulse(s, v) {
  let n = s.get("count") + 1
  return s.set("count", n).set("color", _hue(n))   // immutable, arena-safe
}
</script>
```

Click the logo → the server runs `pulse` → computes a new colour → re-renders → Lumen morphs **only the changed `fill` attribute** into the live DOM. That round-trip *is* the reactivity model. No client state, no virtual DOM in the browser, no rerender-the-world.

### Markup syntax

| Syntax | Meaning |
| --- | --- |
| `{{ expr }}` | Interpolate, **HTML-escaped by default** (XSS-safe) |
| `{{! expr }}` | Interpolate **raw** / unescaped (trusted content only) |
| `{{#if expr}} … {{else}} … {{/if}}` | Conditionals |
| `{{#each items as item}} … {{/each}}` | Loops |
| `@click="handler"` | Live event hook → `data-lumen-click` (also `@input`, `@change`, `@submit`, `@keydown`, …) |
| `<UserCard id="u1" name="Ada" />` | Child component, with props |
| `<Card> … </Card>` + `{{! children }}` | Slots — project inner markup into a component |

Escaping is **opt-out, not opt-in** — you can't forget to escape, because `{{ }}` already does. That single default removes the most common XSS footgun in the React/template world.

### The interactive dev console

`vn dev` is the headline experience — the same "it just works" loop as `next dev` / `nuxi dev` / `vite`, but it's one native binary with nothing to install:

```text
   LUMEN   v0.1.0   the Varian frontend framework

  ➜  Local     http://localhost:8090/
  ➜  Pages     2 in pages/

     ● /                  index.lumen
     ● /about             about.lumen

  ✔ ready in 142 ms  · watching pages/ — edit a page to hot-reload
```

* **File-based routing.** Drop `pages/index.lumen` → served at `/`; `pages/about.lumen` → `/about`. (The same convention Next/Nuxt popularized.)
* **Live reload.** Save a file and the server rebuilds, restarts, and the browser auto-reconnects and re-renders — no plugin, no `nodemon`.
* **Branded error overlay.** A runtime error in a handler is caught and shown as a Lumen-branded in-browser overlay (with the friendly `errors.explain()` what/fix text), then auto-clears on the next good render.
* **Batteries included, like the JS starters.** Every page ships, with zero setup, exactly what `create-next-app` / `nuxi init` / `create-vite` give you out of the box:
  * a full **favicon set** + `manifest.json` (`vn lumen new` scaffolds them into `public/`, served automatically),
  * a responsive `<meta viewport>`, `theme-color`, and `lang`,
  * the **Degular** typeface wired up with a system-font fallback,
  * escape-by-default rendering and a friendly error overlay.

### Composition, slots & client islands

* **Components compose.** `<UserCard id="u1" name="Ada"/>` renders a child with its own props and its own server-side event scope (events are namespaced as `id:handler` so two instances never collide).
* **Slots.** `<Card> inner markup </Card>` projects into the card's `{{! children }}`; interactive components *inside* a slot keep their own scope.
* **Honest client islands.** Need a chart, canvas, or map that must run in the browser? Add a `<client>` block of real browser JS. It runs once on first paint and survives the DOM morph. This is the *honest* island — real client code exactly where you ask for it, the rest still server-driven. Lumen deliberately does **not** compile Varian to a browser bundle: that's what reintroduces hydration-mismatch bugs, and avoiding that whole bug class is the point.

### CLI

```sh
vn lumen new myapp     # scaffold pages/ + public/ (favicons, manifest) + a starter component
vn dev                 # serve ./pages with live reload + the dev console (default :8090)
vn dev pages 3000      # custom dir + port
vn lumen build pages app.vn 8090   # compile pages/ into one runnable Varian app
```

### What Lumen fixes about React / Next / TypeScript

| React / Next / TS pain | Lumen's answer |
| --- | --- |
| Config & build hell (tsconfig, webpack/vite/babel, dozens of deps) | **Zero-config.** One binary, `vn dev`, nothing to wire. |
| Hydration mismatches, SSR/CSR divergence | **Server-driven live** — the server owns state and renders; there is no client/server divergence to mismatch. |
| `node_modules` + supply-chain risk | **Batteries-included**, ships in the binary. No npm, no lockfile. |
| Reactivity footguns (effect deps, stale closures, memo ceremony) | State is plain Varian on the server. No dependency arrays, no stale closures. |
| XSS-by-omission (forgot to escape) | `{{ }}` **escapes by default**; raw is the explicit `{{! }}`. |
| Cryptic wall-of-text errors | Friendly errors with file/line/caret + a fix hint, and a branded browser overlay in dev. |
| Fragmented commands (npm/npx/tsc/eslint/jest/vite) | One `vn` CLI with consistent verbs — same DX as the backend. |

See [`docs/LUMEN.md`](docs/LUMEN.md) and [`docs/planning/LUMEN_PLAN.md`](docs/planning/LUMEN_PLAN.md) for the full reference and roadmap.

---

### 🛠️ The High-Fidelity Toolchain
Varian isn't just a compiler; it's a complete ecosystem.
*   **`vn fmt`**: Opinionated, zero-config code formatter built into the compiler.
*   **`vn test`**: Integrated test runner with `test "desc" {}` blocks and native assertions.
*   **`vn pkg`**: Package manager with `vn wrap python:<pkg>` to automatically generate native Varian SDK wrappers for foreign libraries using introspection.

### 🎯 Native Metadata Decorators
Fastest-in-class native memoization and retry mechanics evaluated natively by the VM in C.
```swift
@cache
@retry(3)
fn fetch_data() { ... }
```

### 🛡️ Language Core
*   **First-Class Types:** `bool`, `int`, `float`, `string`, structs, enums, arrays, tuples.
*   **Pattern Matching:** Exhaustive `match` statements with destructuring.
*   **Error Handling:** Clean `?` propagation and typed `try/catch`.
*   **Comptime:** Execute Varian code during compilation for macros and static generation.

---

## Core Architecture Deep Dives

### 1. Memory Model (GC + Arenas)
Varian utilizes a hybrid allocation system designed to optimize throughput:
*   **Mark-and-Sweep Garbage Collector**: Manages long-lived heap objects (global configs, persistent caches, and actor contexts).
*   **Task-Local Bump Arenas**: Tasks (such as HTTP handlers) allocate structs inside a task-local `64KB` block. This memory is bulk-reclaimed by resetting a pointer instantly when the task finishes, bypassing the GC completely.
*   **`escape_promote` Write Barrier**: If an arena-allocated struct escapes the task scope (e.g. stored in a global variable, channel, or actor mailbox), the runtime triggers an `escape_promote` deep-copy onto the garbage-collected heap.

### 2. Python Bridge Mechanics & Performance
*   **Execution**: Invokes an embedded Python interpreter instance within the Varian process.
*   **Latency**: Passing arguments and retrieving values through the bridge type coercion layer takes `<5 microseconds`.
*   **Data Sharing**: For large scientific datasets (e.g. NumPy arrays), developers use the direct C FFI layer (`@ffi`) to share pointers and map raw buffer addresses, avoiding copy overhead.

### 3. AOT Compilation Output
Varian's AOT compiler compiles high-level bytecode instructions directly into inline C helper functions, removing the interpreter overhead:

**Varian Source:**
```varian
fn compute(x) {
    return x * 2 + 10
}
```

**Transpiled C Output:**
```c
Value compute(Value x) {
    Value temp1 = val_multiply(x, val_int(2));
    Value temp2 = val_add(temp1, val_int(10));
    return temp2;
}
```

---

## CLI Usage

```sh
./vn run app.vn         # Execute a file
./vn fmt .              # Format all code
./vn test .             # Run the test suite
./vn lint .             # Static analysis: correctness, security, performance
./vn wrap python:math   # Generate a native Varian wrapper for Python's math module
```

## Documentation

*   [`docs/LANGUAGE.md`](docs/LANGUAGE.md) — core language reference (types, functions/closures, structs, generics, enums, traits, error handling, decorators, comptime, FFI)
*   [`docs/CONCURRENCY.md`](docs/CONCURRENCY.md) — tasks, channels, actors
*   [`docs/STDLIB.md`](docs/STDLIB.md) — native modules (`math`, `string`, `regex`, `sqlite`, `http`, `auth`, `validate`, `json_encode`/`decode`, the Python bridge, FFI)
*   [`docs/ZENITH.md`](docs/ZENITH.md) — the Zenith web framework and the comptime ORM
*   [`docs/LUMEN.md`](docs/LUMEN.md) — the Lumen frontend framework (`.lumen` components, server-driven live, `vn dev`)
*   [`docs/TOOLING.md`](docs/TOOLING.md) — the `vn` CLI in full
*   [`docs/SECURITY.md`](docs/SECURITY.md) — threat model, hardened build, app-level defenses, and the sandboxing caveat
*   [`docs/planning/`](docs/planning/) — internal roadmap / design notes

## Project Status
We are currently progressing through the **Varian Architecture Roadmap**:
- [x] **Phase 1:** Core Compiler & Runtime Engine
- [x] **Phase 2:** Advanced Type System & Memory Semantics
- [x] **Phase 3:** The Bridge System (FFI & Python)
- [x] **Phase 4:** Concurrency & Distributed Architecture (Actors/Tasks/Channels)
- [x] **Phase 5:** The High-Fidelity Toolchain (`vn fmt`, `vn test`, `vn pkg`)
- [x] **Phase 6:** AOT Compilation & Zenith Ultra Performance (`io_uring`, zero-copy, zero-GC handlers)
- [ ] **Phase 7:** Production Readiness & Observability (`vn shield`, CI/CD)

## Architecture

Source → Lexer → Parser → AST → Compiler → Bytecode → VM

All frontend allocations use a fast arena allocator. The bytecode VM is a register-style stack machine with heap-allocated objects and a deeply integrated cooperative green-thread scheduler.

## Building

```sh
make
./vn examples/hello.vn
```
