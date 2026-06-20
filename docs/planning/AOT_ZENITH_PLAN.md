# Zenith AOT Transpilation & Benchmark Plan

Now that Phase 3 (AOT Transpilation) is successfully implemented and the core VM/FFI bugs are fixed, the next step is to apply the AOT compiler to the actual Zenith HTTP server and measure the performance gains.

## 1. Objective
Transpile the Zenith server code from Varian bytecode to native C, build a standalone optimized binary, and benchmark it to see how close we get to Go/Rust performance (our goal is to narrow the current ~2.2x gap even further).

## 2. Steps to Execute

### Step A: Locate the Benchmark Script
Identify the server script you want to benchmark. There are multiple candidates:
* `/root/dev/VarianLang/server.vn` (Standard server)
* The clustered or single-threaded benchmark artifacts previously created (e.g., `bench_single.vn` or `bench_cluster.vn`).

*Recommendation: Start with the single-worker server to measure pure per-request processing speed without multi-process clustering overhead.*

### Step B: Generate the AOT C Code
Run the `compile` command to transpile the chosen `.vn` file into C.
```bash
cd /root/dev/VarianLang
./vn compile server.vn build/zenith_aot.c
```

### Step C: Compile the Standalone Binary
Compile the generated C file and link it with the Varian VM object files. The `VARIAN_AOT_STANDALONE` flag is critical here, as it tells `main.c` to boot directly into the AOT entry point instead of the REPL/interpreter.

```bash
# 1. Compile main.c for standalone AOT mode
gcc -Wall -Wextra -std=gnu11 -O3 -Iinclude -I/usr/include/x86_64-linux-gnu -I/usr/include/postgresql -D_POSIX_C_SOURCE=200809L -DVARIAN_AOT_STANDALONE -c src/main.c -o build/main_aot.o

# 2. Compile the transpiled zenith code (with -O3 optimizations!)
gcc -Wall -Wextra -std=gnu11 -O3 -Iinclude -I/usr/include/x86_64-linux-gnu -I/usr/include/postgresql -D_POSIX_C_SOURCE=200809L -c build/zenith_aot.c -o build/zenith_aot.o

# 3. Link everything into the final native executable
gcc -Wall -Wextra -std=gnu11 -O3 -Iinclude -I/usr/include/x86_64-linux-gnu -I/usr/include/postgresql -D_POSIX_C_SOURCE=200809L build/main_aot.o build/zenith_aot.o build/aot.o build/arena.o build/ast.o build/ffi.o build/fmt.o build/json.o build/lexer.o build/lib_auth.o build/lib_http.o build/lib_io.o build/lib_math.o build/lib_mock.o build/lib_postgres.o build/lib_python.o build/lib_redis.o build/lib_sanitize.o build/lib_smtp.o build/lib_sqlite.o build/lib_string.o build/lib_task.o build/lib_time.o build/lib_validate.o build/lint.o build/parser.o build/picohttpparser.o build/pkg_manager.o build/test_runner.o build/vm.o -o build/zenith_standalone -lm -lffi -ldl -lcurl -lpq -lcrypto -lssl -lsqlite3 -lhiredis -lpthread -luring
```

### Step D: Benchmark the AOT Binary
Launch the standalone server in the background and run the standard `wrk` load test against it.

```bash
# Start the AOT server
nohup ./build/zenith_standalone > /tmp/zenith_aot.log 2>&1 &
sleep 2

# Verify it's running
curl -s -m 2 http://localhost:18002/plaintext; echo

# Run wrk benchmark
wrk -t4 -c100 -d10s http://localhost:18002/plaintext
wrk -t4 -c100 -d10s http://localhost:18002/json

# Cleanup
killall zenith_standalone
```

### Step E: Compare Results
Compare the `Req/Sec` and `Latency` metrics of the AOT binary against:
1. The standard interpreted Zenith server (which should be noticeably slower).
2. The Go and Rust baseline benchmarks previously recorded.

## 3. Potential Gotchas to Watch Out For
* **FFI Mapping**: We recently fixed the `FFI_PTR`, `FFI_FLOAT`, and `FFI_CHAR` types. If the server makes heavy use of Postgres or Redis FFI extensions, ensure no new FFI type warnings surface.
* **GCC Optimizations**: Ensure you compile the final AOT object (`zenith_aot.o`) and link with `-O3`. This allows the C compiler to inline all the transpiled stack/variable manipulations. The previous interpreter tests didn't benefit from this because the bytecode loop was dynamic.
