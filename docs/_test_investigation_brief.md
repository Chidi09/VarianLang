# Brief: Investigate VarianLang test speed + mocking, implement safe speedup

You are in the VarianLang repo (a bytecode-compiled language; C interpreter `vn.exe`, programs in `.vn`). Investigate the test infrastructure and mocking, write a findings report, and implement ONE safe optimization. Be thorough but do not make risky changes — anything invasive goes in the PLAN section of the report, not implemented.

## Ground truth (already established — trust this, verify only if needed)

- Test runner: `src/test_runner.c`. Entry `test_run_dir()` (line ~241) loops over every `*_test.vn` in `tests/` and calls `run_test_file()` (line ~165).
- **Slowness root cause:** `run_test_file()` → `test_runner_read_file_with_modules()` (line ~93) calls `read_directory_sources("vn_modules")` (line ~97) **once per test file**. The ~13k-line prelude (all `vn_modules/*.vn` concatenated) is therefore re-read from disk, re-lexed, re-parsed, and re-compiled to bytecode for EVERY test file (37 files). That dominates runtime.
- Each test file currently gets a FRESH `VM` (vm_init at ~200, vm_free at ~224), so there is no VM-level cross-file state today.
- VarianLang globals are LATE-BOUND BY NAME (GET_GLOBAL by string, resolved at runtime). This means a "compile prelude once, run many test files in one warm VM" design is feasible in principle.
- Mocking: native, in `src/lib_mock.c` — `mock.intercept(type, method, fake_fn)` and `mock.restore(type, method, old)`. It swaps entries in the module dispatch table. See also `src/vm.c` ~3413 and ~3450 (comments about dispatch table safety across intercept/restore). Test: `tests/mock_test.vn`.
- There is a separate KNOWN BUG (do NOT fix here, just confirm/characterize): when running the full suite, it HANGS after `aurora_pending_features_test.vn` (file 3) — the next file never completes. Hypothesis: a test in aurora_pending ("durable queue backend", "presence tracking pubsub") spawns a detached OS thread (reactive tick / queue worker / pubsub / websocket) that survives `vm_free` and stalls the next file's run. Confirm whether any native code spawns threads not joined on VM teardown (grep `pthread_create`, `CreateThread`, `_beginthread`, reactive tick in src/).

## Tasks

### 1. Investigate & write findings → `docs/TEST_INFRA_FINDINGS.md`
Cover:
- **Speed:** confirm the per-file prelude recompile; estimate the win from each option below.
- **Mocking:** how `lib_mock.c` works, what it can/can't mock, any correctness gaps (e.g., does restore always work, thread safety, does it leak), and whether `tests/mock_test.vn` is the only coverage.
- **Hang:** identify the exact native thread-spawning code path (file:line) reached by aurora_pending tests and whether it's cleaned up on `vm_free`. State the minimal fix (do not implement).

In the report, give a ranked PLAN for speed, including at least:
  (a) **Safe win — prelude string caching:** read+concat `vn_modules` ONCE in `test_run_dir`, pass the cached prelude string into `run_test_file` so each file only re-lexes/compiles (still avoids 36 disk re-reads + concatenations). Note: this does NOT avoid recompiling the prelude per file.
  (b) **Big win — warm-VM / compile-prelude-once:** compile the prelude to a Chunk and run it ONCE in a single persistent VM, then for each test file compile ONLY the file and execute in that same VM (globals resolve by late binding). Discuss the isolation risk (test-defined globals colliding, mock state leaking, the hang) and how to mitigate (e.g., snapshot/restore globals table between files, or reset mock dispatch entries).
  (c) Any quick parser/compiler wins specific to the prelude.

### 2. Implement ONLY option (a) — the safe prelude string cache
- Modify `src/test_runner.c` so the prelude (`read_directory_sources("vn_modules")`) is read exactly once for the whole `test` run and reused for every file. Keep behavior identical otherwise (each file still gets a fresh VM and is still prelude+file concatenated, lexed, compiled, run).
- Do NOT implement (b) — just document it.
- Keep the existing per-file fresh-VM semantics.

### 3. Build & verify
Build on Windows (msys2). Exact commands:
```bash
cd "/c/Users/X1/CHIDIS WORKSPACE/VarianLang"
M=/c/Users/X1/scoop/apps/msys2/2026-06-11/msys64
export PATH="$M/mingw64/bin:$M/usr/bin:$PATH"
export TMP="C:\\Users\\X1\\AppData\\Local\\Temp" TEMP="C:\\Users\\X1\\AppData\\Local\\Temp"
unset TMPDIR
make USE_POSTGRES=0        # if the full link fails on Windows, report the exact error; do NOT hand-hack the link
```
If `make` can't fully link on Windows, STOP and report — the orchestrator has a known manual link procedure. Do NOT invent linker flags.

After building, time a single test file to show the per-file cost is unchanged and the suite no longer re-reads disk 37×:
```bash
export PATH=".:$M/mingw64/bin:$M/usr/bin:$PATH"
time ./vn.exe run tests/assertions_test.vn
```
Run the suite with a hard cap so the known hang can't block you:
```bash
timeout -k 5 120 ./vn.exe test tests/ --timeout 8
```
(It will still hang at aurora_pending — that's the known bug, not your regression. Confirm files BEFORE aurora_pending still pass.)

## Output / rules
- Print: exactly which lines you changed in `src/test_runner.c`, the findings doc path, and your timing numbers.
- Do NOT touch `deploy/`, do NOT change `vn_modules/`, do NOT implement the warm-VM option, do NOT attempt to fix the hang.
- If a build step fails, report the exact error and stop — don't guess linker flags.
