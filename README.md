# Varian 🚀

A blazing fast, concurrent, systems-level programming language built from scratch in C with a custom bytecode VM. Varian combines the speed of C, the concurrency model of Go (Actors & Channels), and the ecosystem of Python into a single, cohesive developer experience.

## The Cheat Code Architecture
Varian doesn't force you to rewrite the world. It comes built-in with a zero-overhead **C FFI** and a fully integrated **Python Bridge**. You can import and use any Python package (like NumPy or PyTorch) seamlessly as if it were written in Varian.

```varian
fn array(args) {
    return python.run("numpy", "array", [args])
}
```

## Features

### ⚡ Thread-Safe Concurrency (Actors & Channels)
Say goodbye to mutex locks and race conditions. Varian uses a cooperative spin-yield scheduler and the Actor model to run thousands of lightweight tasks concurrently.

```varian
actor Counter {
    count: int = 0,
    fn increment(self) { self.count = self.count + 1 }
    fn get(self) -> int { return self.count }
}

let c = Counter.spawn()
c.increment()
print(c.get())
```

### 🌐 The Zenith Web Framework

<p align="center">
  <img src="docs/zenith_logo.png" alt="Zenith Logo" width="300" />
</p>

Varian includes `Zenith`, a blazing-fast, non-blocking HTTP web framework powered by native POSIX sockets, `io_uring` for zero-copy I/O, per-request zero-GC arena allocators, and AOT (Ahead-of-Time) compilation. It is deeply integrated into Varian's task scheduler.

```varian
let app = new_app()

app.get("/", |req| {
    return Response { status: 200, body: "Hello from Zenith!", content_type: "text/plain" }
}, "Root endpoint")

app.listen(3000)
```

### 🛠️ The High-Fidelity Toolchain
Varian isn't just a compiler; it's a complete ecosystem.
- **`vn fmt`**: Opinionated, zero-config code formatter built into the compiler.
- **`vn test`**: Integrated test runner with `test "desc" {}` blocks and native assertions.
- **`vn pkg`**: Package manager with `vn wrap python:<pkg>` to automatically generate native Varian SDK wrappers for foreign libraries using introspection.

### 🎯 Native Metadata Decorators
Fastest-in-class native memoization and retry mechanics evaluated natively by the VM in C.
```varian
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
