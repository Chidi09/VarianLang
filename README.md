# Varian 🚀

A blazing fast, concurrent, systems-level programming language built from scratch in C with a custom bytecode VM. Varian combines the speed of C, the concurrency model of Go (Actors & Channels), and the ecosystem of Python into a single, cohesive developer experience.

## The Cheat Code Architecture
Varian doesn't force you to rewrite the world. It comes built-in with a zero-overhead **C FFI** and a fully integrated **Python Bridge**. You can import and use any Python package (like NumPy or PyTorch) seamlessly as if it were written in Varian.

```varian
import python

fn array(args: any) -> any {
    return python.run("numpy", "array", [args])
}
```

## Features

### ⚡ Thread-Safe Concurrency (Actors & Channels)
Say goodbye to mutex locks and race conditions. Varian uses a cooperative spin-yield scheduler and the Actor model to run thousands of lightweight tasks concurrently.

```varian
actor Counter {
    count: int = 0
    fn increment() { count += 1 }
    fn get() -> int { return count }
}

c = Counter.spawn()
task.spawn(fn() { c.increment() })
```

### 🌐 The Zenith Web Framework
Varian includes `Zenith`, a blazing-fast, non-blocking HTTP web framework powered by native POSIX sockets deeply integrated into Varian's task scheduler.

```varian
import zenith

app = zenith.ZenithApp.spawn()

app.get("/", fn(req) {
    return "Hello from Zenith!"
})

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
./vn wrap python:math   # Generate a native Varian wrapper for Python's math module
```

## Project Status
We are currently progressing through the **Varian Architecture Roadmap**:
- [x] **Phase 1:** Core Compiler & Runtime Engine
- [x] **Phase 2:** Advanced Type System & Memory Semantics
- [x] **Phase 3:** The Bridge System (FFI & Python)
- [x] **Phase 4:** Concurrency & Distributed Architecture (Actors/Tasks/Channels)
- [x] **Phase 5:** The High-Fidelity Toolchain (`vn fmt`, `vn test`, `vn pkg`)
- [ ] **Phase 6:** Production Readiness & Observability (`vn shield`, CI/CD)

## Architecture

Source → Lexer → Parser → AST → Compiler → Bytecode → VM

All frontend allocations use a fast arena allocator. The bytecode VM is a register-style stack machine with heap-allocated objects and a deeply integrated cooperative green-thread scheduler.

## Building

```sh
make
./vn examples/hello.vn
```
