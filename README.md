# Varian 🚀

A blazing fast, concurrent, systems-level programming language built from scratch in C with a custom bytecode VM. Varian combines the speed of C, the concurrency model of Go (Actors & Channels), and the ecosystem of Python into a single, cohesive developer experience.

## The Cheat Code Architecture
Varian doesn't force you to rewrite the world. It comes built-in with a zero-overhead **C FFI** and a fully integrated **Python Bridge**. You can import and use any Python package (like NumPy or PyTorch) seamlessly as if it were written in Varian.

```swift
fn array(args) {
    return python.run("numpy", "array", [args])
}
```

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

Varian includes **Zenith**, a hyper-optimized, non-blocking HTTP web framework built directly into the language runtime. We engineered Zenith to compete directly with the fastest Rust and Go servers, bypassing the traditional bottlenecks of dynamic languages.

#### 🦁 Zero-Overhead Architecture
Zenith fundamentally reimagines how a high-level language handles network traffic:
- **`io_uring` & `writev`**: Bypasses traditional blocking syscalls with Linux's ultra-fast asynchronous I/O and scatter/gather response serialization.
- **Per-Request Arena Allocators**: Memory for each request is allocated in a single contiguous chunk. When the request ends, the memory is instantly reclaimed by resetting a pointer—resulting in **Zero Garbage Collection overhead** during the request lifecycle.
- **AOT Transpilation**: Zenith server code can be natively transpiled to C. This bypasses the bytecode interpreter entirely, allowing the web framework to execute at raw machine speeds while retaining Varian's high-level ergonomics.
- **Shared-Nothing Multi-Threading**: Using `SO_REUSEPORT`, Zenith boots independent VM instances across multiple cores. No Global Interpreter Lock (GIL), no lock contention—just pure linear scaling.

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
- **Zero-Allocation CORS & CSRF:** Fully compliant, automatically injected headers with no string-allocation overhead.
- **Token Bucket Rate Limiting:** Millisecond-precision rate limiters powered by the native scheduler.

#### ⚡ Zero-Cost Comptime ORM
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
- **Radix Trie Routing**: Routes are stored in a highly optimized segment trie. Lookup costs one trie descent per path segment rather than an O(N) scan.
- **Socket-Free Testing**: Run full route, middleware, and handler pipelines synchronously without ever opening a real HTTP port. Perfect for blisteringly fast unit tests.
```swift
// No network stack involved!
let resp = app.handle(fake_req("GET", "/api/users/2"))
```

#### 🔌 Real-Time & Streaming
Zenith supports real-time communication out of the box with zero external dependencies.
- **WebSockets**: Easily upgrade connections and send/receive masked payloads natively.
- **Server-Sent Events (SSE)**: Simple API for streaming fast updates to clients.
```swift
app.get("/ws", |req| {
    let ws = upgrade_websocket(req)
    ws.write("Connected!")
    return Response { _keep_open: true }
}, "WebSocket upgrade")
```

#### 🔒 Static Serving & TLS
- **Static Mounts**: Serve static directories with built-in path-traversal protection and automatic MIME type resolution.
- **Native HTTPS**: Pass your SSL certificates directly to the cluster to handle HTTPS internally.
```swift
app.serve_static("/assets", "public/assets")
app.listen_tls_cluster(443, "cert.pem", "key.pem", 8)
```

### 🛠️ The High-Fidelity Toolchain
Varian isn't just a compiler; it's a complete ecosystem.
- **`vn fmt`**: Opinionated, zero-config code formatter built into the compiler.
- **`vn test`**: Integrated test runner with `test "desc" {}` blocks and native assertions.
- **`vn pkg`**: Package manager with `vn wrap python:<pkg>` to automatically generate native Varian SDK wrappers for foreign libraries using introspection.

### 🎯 Native Metadata Decorators
Fastest-in-class native memoization and retry mechanics evaluated natively by the VM in C.
```swift
@cache
@retry(3)
fn fetch_data() { ... }
```

### 🛡️ Language Core
- **First-Class Types:** `bool`, `int`, `float`, `string`, structs, enums, arrays, tuples.
- **Pattern Matching:** Exhaustive `match` statements with destructuring.
- **Error Handling:** Clean `?` propagation and typed `try/catch`.
- **Comptime:** Execute Varian code during compilation for macros and static generation.

## CLI Usage

```sh
./vn run app.vn         # Execute a file
./vn fmt .              # Format all code
./vn test .             # Run the test suite
./vn lint .             # Static analysis: correctness, security, performance
./vn wrap python:math   # Generate a native Varian wrapper for Python's math module
```

## Documentation

- [`docs/LANGUAGE.md`](docs/LANGUAGE.md) — core language reference (types, functions/
  closures, structs, generics, enums, traits, error handling, decorators, comptime, FFI)
- [`docs/CONCURRENCY.md`](docs/CONCURRENCY.md) — tasks, channels, actors
- [`docs/STDLIB.md`](docs/STDLIB.md) — native modules (`math`, `string`, `sqlite`,
  `http`, `auth`, `validate`, `json_encode`/`decode`, the Python bridge, FFI)
- [`docs/ZENITH.md`](docs/ZENITH.md) — the Zenith web framework and the comptime ORM
- [`docs/TOOLING.md`](docs/TOOLING.md) — the `vn` CLI in full

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
