# Zenith Ultra-Performance Implementation Plan

To close the remaining performance gap with Go (~87k req/sec) and eventually Rust (~113k req/sec), Zenith must optimize its underlying VM and I/O primitives. The framework architecture has been optimized (epoll, threading, native routing, task pooling), but the execution environment itself still carries dynamic language overhead.

Here is the blueprint for the next phase of optimizations, meant to eliminate allocation overhead, parsing inefficiencies, and interpretation costs.

## Phase 1: SIMD HTTP Parsing & Scatter/Gather I/O
*Goal: Eliminate CPU cycles spent on string manipulation during request parsing and response building.*

1. **`picohttpparser` Integration (`src/lib_http.c`)**
   - Replace the naive `strstr` and `strchr` parsing loops with `picohttpparser`, an industry-standard, SIMD-accelerated C parser used by H2O and other high-performance servers.
   - It parses headers 4-5x faster by reading bytes in parallel using CPU vector instructions.
2. **`writev` Response Serialization (`src/lib_http.c`)**
   - Currently, Zenith builds the HTTP response by copying strings into a fixed buffer using `snprintf`.
   - Update `send_http_response` to use `writev` (scatter/gather I/O). Instead of concatenating the status line, headers, and body into one buffer, `writev` sends them directly from their individual memory addresses in a single syscall, saving a memory allocation and a memory copy.

## Phase 2: Per-Request Arena Allocator (Zero-GC Handlers)
*Goal: Eliminate `malloc`/`free` and Garbage Collector pauses during request handling.*

Even with `Task` pooling, the execution of Varian code inside a handler allocates objects (Strings, Structs, Arrays) on the global VM heap, eventually triggering the GC.

1. **Task-Local Arena (`src/vm.h`, `src/vm.c`)**
   - Attach a fixed-size memory arena (e.g., a 64KB bump allocator) to each `Task` struct.
2. **Arena Allocation Routing**
   - When the VM detects that it is executing inside an HTTP handler task, it routes all new object allocations (`allocate_string`, `new_struct`, etc.) to the task's arena instead of the global heap.
3. **Instant Reset**
   - When the HTTP response is sent and the `Task` is recycled into the free-list, instantly reclaim all memory by simply resetting the arena's offset pointer to `0`. The GC completely ignores these objects, eliminating tracing and sweeping overhead.

## Phase 3: JIT Compilation (Bypassing the Bytecode Loop)
*Goal: Remove the VM interpreter switch-dispatch overhead.*

The interpreter loop (`BC_DISPATCH` in `vm.c`) forces the CPU to constantly branch, preventing instruction pipelining. Hot handlers should be compiled directly to machine code.

1. **Tracing JIT / Hot-Path Compiler**
   - Implement a lightweight Just-In-Time compiler (using something like `asmjit` or a simple custom backend for x86_64).
   - When an endpoint is hit a certain number of times (e.g., 10,000 times), the VM analyzes the bytecode for that closure.
   - It emits native machine code for the handler, skipping `BC_DISPATCH` entirely. Property accesses become direct memory offsets, and function calls become native `CALL` instructions.
2. **AOT Alternative (If JIT is too complex)**
   - Create a transpilation step that converts Varian `.vn` code into equivalent C code before deployment, compiling the entire web server into a static, standalone binary.

## Phase 4: Zero-Copy Asynchronous I/O (`io_uring`)
*Goal: Eliminate syscall overhead completely.*

While `epoll` is fast and $O(1)$, it still requires synchronous `recv()` and `send()` syscalls that block the thread briefly and require context switches.

1. **`io_uring` Backend (`src/lib_http.c`)**
   - On Linux systems, swap the `epoll` event loop for `io_uring`.
   - Zenith will submit read and write operations to a shared memory ring buffer with the kernel.
   - The kernel processes the networking asynchronously, and Zenith simply reads completion events from the ring buffer without invoking any syscalls. This is the gold standard for networking performance today.
