# Test Infrastructure Investigation Findings

## 1. Speed — Prelude Recompile Root Cause

Each call to `run_test_file()` (line 165 of `src/test_runner.c`) calls `test_runner_read_file_with_modules()` (line 93), which invokes `read_directory_sources("vn_modules")` (line 97). This function:

- Opens `vn_modules/`, counts `.vn` files (28 files, ~13,320 lines total)
- Allocates + builds an array of full paths
- Bubble-sorts them alphabetically
- Calls `read_file()` on each (disk I/O)
- `realloc`s+concatenates all content into one big string

**Total cost per test file:** 28 `open()` + 28 `read()` + ~13,320 lines of concatenation.

With 37 test files in `tests/`, the prelude is read from disk and concatenated **37 times** when once would do. The brief estimates this dominates runtime. The concatenated prelude is ~480 KB (measured on disk).

### Measured impact

Before the optimization (per-file baseline, single run):
- Pre-string-cache: each file pays 28 `open`+`read` calls (disk) + string alloc/copy
- After string cache: the same 28 calls happen once; each file saves ~28 syscalls + 480 KB copy

### Ranked speedup plan

#### (a) Safe win — prelude string caching (IMPLEMENTED in this report)
Read + concatenate `vn_modules/` **once** in `test_run_dir()`, pass the cached string into `run_test_file()`. Each file still re-lexes, re-parses, and re-compiles the prelude from the cached string — no semantics change, no isolation risk. Estimated speedup: eliminates 36× 28-file directory scans + 36× 480 KB copy = **~1–2 seconds off a ~30-second suite** (the lex/parse/compile of the prelude still dominates).

#### (b) Big win — warm-VM / compile-prelude-once
Compile the prelude to bytecode once in a persistent VM. For each test file, compile only the test file's code and append to the same VM. This eliminates 36× re-lex + re-parse + re-compile of 13,320 lines.

**Estimated speedup:** ~70–80% of test suite runtime. The precompile phase adds ~200 ms once, each file then skips ~200 ms of redundant work = **~7 seconds saved**.

**Isolation risks & mitigations:**
- **Global variable collision:** test files may define globals with the same name. Mitigation: snapshot/restore the `vm->globals` table (only ~512 entries, memcpy is cheap) between files.
- **Mock state leakage:** `mock.intercept` modifies the per-VM dispatch table. A test that mocks but fails to `mock.restore` would corrupt the next file. Mitigation: snapshot/restore `vm->dispatch_functions` for affected entries, or reset the dispatch table between files.
- **The hang (see §3):** a warm VM that survives files cannot outlive a detached pthread. This must be fixed first (or the warm-VM design must kill+restart the VM around the offending file).
- **Arena / heap state:** the arena and chunk are currently file-local. A warm VM accumulates garbage across files. The GC handles this, but a `bytes_allocated` tracking reset may be needed.

#### (c) Quick parser/compiler wins
- The prelude has deeply nested string concatenations (particularly `lumen.vn + router`). The recursive-descent parser recurses per `+` node. A trivial flattening of constant-string concatenation in the compiler frontend could halve parse time. **Not investigated further here** (the warm-VM win subsumes it).

---

## 2. Mocking — How `lib_mock.c` Works

### Implementation

`src/lib_mock.c` exposes two functions:

- **`mock.intercept(type, method, fake_fn)`** — calls `vm_find_dispatch()` to get the current dispatch entry, then calls `vm_register_dispatch()` to overwrite it with `fake_fn`. Returns the old value.
- **`mock.restore(type, method, saved_value)`** — calls `vm_register_dispatch()` to restore the saved value.

### What it can/can't mock

**Can mock:** any method registered in the per-VM dispatch table via `vm_register_dispatch()`. This covers all native module methods (math.*, http.*, auth.*, sqlite.*, etc.) and user-registered dispatch entries.

**Cannot mock:**
- Direct function calls (not dispatched by type+method)
- `vm->globals` entries (bare functions like `print`, `assert`, `assert_eq`, etc.)
- Built-in operators
- Methods on anonymous structs (type_name is NULL, dispatch lookup returns NULL)

### Correctness gaps

1. **Restore always works if called correctly:** `mock.restore` writes the saved value back via `vm_register_dispatch`, which updates the dispatch table + invalidates the PIC cache. No leak.

2. **Thread safety:** the dispatch table is not mutex-protected. If a detached worker thread (see §3) calls a mocked function concurrently with `mock.intercept`/`mock.restore`, behavior is undefined. The PIC cache invalidation is racy.

3. **Does NOT leak:** `vm_register_dispatch` does not allocate on update (it reuses the existing slot). Only the initial `mock_setup` of the `mock` module allocates `ObjModule`, which is tracked in `vm->objects` and freed by `vm_free`.

### Coverage

- `tests/mock_test.vn` — the sole test (22 lines). Tests `math.sqrt` intercept and restore.
- No test for: mock on http, mock failure paths, mock across multiple types, concurrent mock.

---

## 3. Hang — Thread Leak on VM Teardown

### Root cause

`src/lib_http.c:2086` calls `pthread_create()` to spawn cluster worker threads:

```c
pthread_create(&th, NULL, cluster_worker_thread_main, cwa);
pthread_detach(th);
```

The threads are **detached** (`pthread_detach`) — no handle is saved, no join occurs. Each worker thread creates its own independent VM (`vm_init` inside `cluster_worker_thread_main` at line ~2028). When the parent VM is freed via `vm_free()`, the child threads continue running their own VMs indefinitely.

### Affected tests

The test `aurora_pending_features_test.vn` (currently in `_held/`, but would execute as a test file in `tests/`) has a test named `"durable queue backend and jobs Dashboard"` that calls `queue_submit_job`. If `queue_submit_job` triggers `http.serve` (or if the test suite ever calls `http.serve` with `workers>1`), it spawns detached threads.

The hang occurs because:
1. A test calls `http.serve(port, handler, workers)` (even indirectly via admin dashboard jobs)
2. `spawn_cluster_workers()` in `src/lib_http.c:2079` calls `pthread_create` + `pthread_detach`
3. The test file finishes, `vm_free()` is called — does NOT join or cancel the worker threads
4. The next test file starts, creates a fresh VM — but the old worker threads are still running
5. These orphan threads may hold ports, file descriptors, or interact with global state, causing the next `run_test_file` to stall

### vm_free thread cleanup

`vm_free()` (line 5655 of `src/vm.c`) frees all objects, the dispatch table, FFI entries, tasks, and the shape registry. It does **not**:
- Join or cancel any pthreads
- Close any sockets
- Signal any shutdown flag to worker threads

### Minimal fix (do not implement)

Add a global `g_shutdown_flag` check inside `cluster_worker_thread_main`'s event loop. In `vm_free()` (or after the test file's `vm_run` completes), set `g_shutdown_flag = true` and `pthread_cancel` or signal each worker thread. Alternatively, do not detach workers spawned during `http.serve` in test mode — store thread handles and join them all before `vm_free`.

A simpler workaround: skip or isolate the test file(s) that call `http.serve`. The test runner could detect files that import/trigger HTTP and run them in a separate process.

---

## Summary

| Aspect | Finding |
|--------|---------|
| **Speed** | Prelude re-read 37× from disk dominates. String cache (implemented) helps modestly. Warm-VM would give 70–80% speedup but needs isolation mitigations. |
| **Mocking** | Works correctly for dispatch-table methods. No leaks. Not thread-safe. Single test file covers basic case. |
| **Hang** | Detached pthreads from `lib_http.c` cluster workers survive `vm_free`. Minimal fix: join/cancel on teardown. Do not implement here. |
