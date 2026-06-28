# Brief: True per-file isolation for the warm-VM test runner (re-run prelude)

Work in the MAIN repo at `C:/Users/X1/CHIDIS WORKSPACE/VarianLang`, editing ONLY `src/test_runner.c`.

## Context

`src/test_runner.c` has a warm-VM test runner: `warm_vm_init()` compiles the ~13k-line `vn_modules` prelude ONCE into `prelude_chunk` and runs it in a persistent `VM warm_vm` (708 globals), then `run_test_file_warm()` compiles+runs each `*_test.vn` file in that same VM. Between files, `warm_vm_restore()` restores a SHALLOW snapshot (`GlobalSnapshot`/`DispatchSnapshot`) of global names+values and the dispatch table.

## The problem

The shallow snapshot restores global *pointers*, but it cannot undo IN-PLACE MUTATION of the heap objects those globals point to. Tests that mutate shared prelude singletons (e.g. lumen's global route/pubsub registries) corrupt state for later files. Symptom: `lumen_m3/m4/m7/live/roundtrip` tests PASS when run alone but FAIL in the full suite with `Struct has no field 'path'`, `Struct has no field 'handle'`, `Array has no method 'topic'`, etc.

Verify the repro (these must end up PASSING after your fix, and must still pass alone):
```bash
cd "/c/Users/X1/CHIDIS WORKSPACE/VarianLang"
M=/c/Users/X1/scoop/apps/msys2/2026-06-11/msys64
export PATH=".:$M/mingw64/bin:$M/usr/bin:$PATH"
TD="/c/Users/X1/AppData/Local/Temp/vnbuild"; export TMP="$TD" TEMP="$TD"; unset TMPDIR
./vn.exe test tests/ --timeout 6      # orchestrator will rebuild first; today: 91 passed, 8 failed
```

## The fix: reset globals + RE-RUN the compiled prelude per file

Replace the shallow snapshot/restore strategy with true re-initialization. The prelude is COMPILED ONCE (keep that — it's the expensive part), but its bytecode is EXECUTED fresh before each test file so all global singletons are recreated clean.

Required behavior, per test file (before compiling/running the file):
1. Reset the warm VM's global state to EMPTY: `global_count = 0` (you may leave the name/value arrays as-is since count gates them), clear the dispatch table (`memset(dispatch_occupied,0,...)`, and reset `dispatch_type_names/method_names/functions` consistently with how `warm_vm_init` populated them), reset the dispatch PIC cache (`dispatch_pic_keys`/`dispatch_pic_idxs` — see how the existing `warm_vm_restore` does it), and reset `validation_registry.count = 0`.
2. Reap dead tasks and set `task_count = 0` / `free_tasks = NULL` (the existing code at the top of `run_test_file_warm` already does this — keep it).
3. RE-RUN the already-compiled prelude bytecode in the warm VM so all 708 globals + dispatch entries are redefined fresh. You need to point the VM at the prelude's compiled function and run its top-level (no tests). Look at how `warm_vm_init` first ran it (`vm_init(warm_vm,&compiler); vm_run(warm_vm,false);`) and at how `vm_run` builds the init task from the VM's main function / compiler in `src/vm.c` — preserve whatever pointer you need (e.g. the prelude `ObjFunction*` / the `Compiler`) in `warm_vm_init` so you can re-run it each file. Re-running must redefine globals (DEFINE_GLOBAL overwrites by name) — confirm in vm.c that re-running is idempotent and safe (the prelude is pure definitions; it does NOT open sockets/threads at eval, which is why the single warm load doesn't hang).
4. Then compile + run the test file as it does today.

You can DELETE `warm_vm_restore` and the `GlobalSnapshot`/`DispatchSnapshot` types/snapshots if they become unused, OR keep `warm_vm_init`'s structure and just add a `warm_vm_reset_and_reload_prelude()` helper. Your call — keep it clean and contained to `src/test_runner.c`.

## MUST NOT change
- Do NOT touch the end-of-`test_run_dir` block that prints the Summary and calls `_Exit(...)` — that is a deliberate fix for an exit-time native-thread hang. Keep printing Summary then `_Exit` exactly as-is.
- Do NOT touch any file other than `src/test_runner.c`. Do NOT touch `vn_modules/`, `tests/`, `quarantined_tests/`, or `deploy/`.
- Keep the prelude COMPILED ONCE (do not recompile per file — only re-EXECUTE).

## Performance target
Today the suite is ~5.5s (one prelude compile+run + 30 files). Re-running the prelude EXECUTION per file will add time but must stay well under a cold full recompile. Aim for under ~25s for the 30-file suite. If re-running the prelude is unexpectedly expensive (e.g. >0.5s each), STOP and report the measured per-file prelude-run cost instead of shipping a slow runner.

## Build (you likely cannot fully link on Windows — orchestrator links)
```bash
cd "/c/Users/X1/CHIDIS WORKSPACE/VarianLang"
M=/c/Users/X1/scoop/apps/msys2/2026-06-11/msys64
export PATH="$M/mingw64/bin:$M/usr/bin:$PATH"
TD="/c/Users/X1/AppData/Local/Temp/vnbuild"; mkdir -p "$TD"; export TMP="$TD" TEMP="$TD" TMPDIR="$TD"
gcc -Wall -Wextra -std=gnu11 -g -Iinclude -D_POSIX_C_SOURCE=200809L -IC:/deps/include -D_WIN32_WINNT=0x0600 -DUSE_LOCAL_TRE_H -Ideps/tre-0.9.0/local_includes -IC:/deps -IC:/deps/hiredis/include -IC:/deps/libffi/include -DVN_NO_POSTGRES -DVN_NO_SMTP -c src/test_runner.c -o build/test_runner.o
```
Make sure it compiles with no errors. Report: exactly what you changed (file:line), how you re-run the prelude (which VM pointer/API), and the per-file prelude-run cost if you can measure it.
