# Brief: Fix warm-VM cross-file global/impl visibility bug

Work in the MAIN repo at `C:/Users/X1/CHIDIS WORKSPACE/VarianLang`. A new "warm VM" test runner was added to `src/test_runner.c`: it compiles the ~13k-line `vn_modules` prelude ONCE into a persistent VM (708 globals), then for each `*_test.vn` file it compiles ONLY that file (`run_test_file_warm`) and runs it in the same VM, restoring globals/dispatch to a post-prelude snapshot between files (`warm_vm_restore`).

## The bug (deterministic repro)

A test file's OWN top-level globals and `impl` methods are not visible to that file's `test "..."` blocks once the VM is warm.

```bash
cd "/c/Users/X1/CHIDIS WORKSPACE/VarianLang"
M=/c/Users/X1/scoop/apps/msys2/2026-06-11/msys64
export PATH=".:$M/mingw64/bin:$M/usr/bin:$PATH"
TD="/c/Users/X1/AppData/Local/Temp/vnbuild"; export TMP="$TD" TEMP="$TD"; unset TMPDIR
mkdir -p _subset && cp tests/arena_escape_test.vn tests/assertions_test.vn tests/closures_test.vn tests/closure_capture_test.vn _subset/
./vn.exe test _subset --timeout 5
rm -rf _subset
```
Observed: `closures_test` and `closure_capture_test` FAIL with `No method 'make_adder' for type 'Box'` and `Undefined variable '_loop' / '_curry' / '_counter'`. BUT each of those files PASSES when run alone (`./vn.exe test tests/closures_test.vn`). So the failure only appears when the file is NOT the first file in a warm run.

## Diagnosis (very likely cause)

In the ORIGINAL cold runner, each file was compiled as prelude+file CONCATENATED into one program, so the file's top-level `let`/`impl` were compiled with full knowledge of all prelude globals and assigned correct global slots. In the warm runner, each file is compiled ALONE via `compiler_init(&compiler, arena, &chunk, program)` — the compiler does NOT know the 708 prelude globals already in the VM. So the file-compiled-alone mis-assigns global slot indices (and/or resolves its own top-level names as something other than the globals the VM expects), and the dispatch/global lookups at runtime don't line up with the warm VM's existing 708-global table.

Look at how the compiler assigns and resolves globals in `src/vm.c` (search for `global_count`, `global_names`, `DEFINE_GLOBAL`, `GET_GLOBAL`, `resolve_global`, `compiler_init`, and how top-level `let` and `impl` are compiled). Determine whether globals are addressed by NAME or by pre-assigned INDEX. The fix must make a file-compiled-alone agree with the warm VM's existing global table.

## What to implement

Fix the warm runner so a per-file compile is consistent with the warm VM's existing globals. Acceptable approaches (pick the cleanest that matches how the compiler actually works — VERIFY in vm.c first, do not guess):
- **Preferred:** seed the per-file `Compiler` with the prelude's already-defined global names (so it assigns the same indices / treats them as known globals), e.g. by adding a minimal compiler API that pre-populates its global-name table from `warm_vm->global_names`/`global_count` before compiling the file. Then file-defined globals get fresh indices ABOVE the prelude's, matching the VM.
- If globals are name-addressed and the real issue is index base, offset the file's global indices by the prelude global count.
- Whatever you do, `impl` method registration (dispatch table) for a struct defined in the test file must be visible to that file's tests.

After your fix, the repro above must show `closures_test` and `closure_capture_test` PASSING, and they must still pass when run alone. No previously-passing file may regress.

## STRICT scope
- Edit ONLY `src/test_runner.c` and, if necessary, `src/vm.c` (+ its header for a new compiler helper). 
- DO NOT touch `src/lexer.c`, `src/parser.c`, `src/lsp.c`, `src/main.c`, any `vn_modules/*.vn`, any `tests/*.vn`, or `deploy/`.
- `src/vm.c` already has unrelated local modifications (array for-in loops) — do NOT revert or alter those; only add the minimal global-seeding logic.

## Build (you likely CANNOT fully link on Windows — that's fine)
Compile your changed objects to catch errors:
```bash
cd "/c/Users/X1/CHIDIS WORKSPACE/VarianLang"
M=/c/Users/X1/scoop/apps/msys2/2026-06-11/msys64
export PATH="$M/mingw64/bin:$M/usr/bin:$PATH"
TD="/c/Users/X1/AppData/Local/Temp/vnbuild"; mkdir -p "$TD"; export TMP="$TD" TEMP="$TD" TMPDIR="$TD"
gcc -Wall -Wextra -std=gnu11 -g -Iinclude -D_POSIX_C_SOURCE=200809L -IC:/deps/include -D_WIN32_WINNT=0x0600 -DUSE_LOCAL_TRE_H -Ideps/tre-0.9.0/local_includes -IC:/deps -IC:/deps/hiredis/include -IC:/deps/libffi/include -DVN_NO_POSTGRES -DVN_NO_SMTP -c src/test_runner.c -o build/test_runner.o
gcc -Wall -Wextra -std=gnu11 -g -Iinclude -D_POSIX_C_SOURCE=200809L -IC:/deps/include -D_WIN32_WINNT=0x0600 -DUSE_LOCAL_TRE_H -Ideps/tre-0.9.0/local_includes -IC:/deps -IC:/deps/hiredis/include -IC:/deps/libffi/include -DVN_NO_POSTGRES -DVN_NO_SMTP -c src/vm.c -o build/vm.o
```
The final link is done by the orchestrator (it has the manual mingw-OpenSSL link procedure). Just make sure your objects COMPILE with no errors.

## Report
- Exactly what you changed and why (file:line), how globals are addressed in vm.c, and confirmation your objects compiled.
