# Brief: Fix warm-VM dispatch of prelude struct `impl` methods

Work in `C:/Users/X1/CHIDIS WORKSPACE/VarianLang`. A prebuilt `vn.exe` exists.
Do NOT run `make`. Do NOT delete or touch `build/` except by compiling individual
objects with `gcc -c` to check your code compiles. The orchestrator does the final link.
Do NOT touch `deploy/`, `tests/`, `quarantined_tests/`, or `vn_modules/`.

## The bug (proven repro)

The warm-VM test runner (`src/test_runner.c`) compiles the `vn_modules` prelude ONCE,
then compiles each `*_test.vn` file SEPARATELY and runs it in the same VM. Problem:
calling an `impl` method on a struct **defined in the prelude** fails in the warm runner,
even though it works in the normal (cold) `vn run` path.

Proof (run these — first works, second fails):
```bash
cd "/c/Users/X1/CHIDIS WORKSPACE/VarianLang"
M=/c/Users/X1/scoop/apps/msys2/2026-06-11/msys64
export PATH=".:$M/mingw64/bin:$M/usr/bin:$PATH"
TD="/c/Users/X1/AppData/Local/Temp/vnbuild"; export TMP="$TD" TEMP="$TD"; unset TMPDIR

# COLD — WORKS, prints "3 items":
printf 'let i = new_i18n("en")\nprint(i.plural(3, "item", "items"))\n' > "$TD/_c.vn"
./vn.exe run "$TD/_c.vn"

# WARM — FAILS with: Struct has no field 'plural'
printf 'test "x" { let i = new_i18n("en"); assert_eq(i.plural(3,"item","items"), "3 items") }\n' > "$TD/_w_test.vn"
mkdir -p "$TD/wd" && cp "$TD/_w_test.vn" "$TD/wd/_w_test.vn"
./vn.exe test "$TD/wd" --timeout 5
```
`new_i18n` returns an `I18n` struct (see `vn_modules/i18n.vn`); `plural` is defined in its
`impl I18n { fn plural(self, count, singular, plural) { ... } }` block. Field access
(`i.locale`) works in warm mode; only `impl` METHOD calls on prelude structs fail.

## Diagnosis to confirm in `src/vm.c`

The compiler almost certainly decides at COMPILE time whether `obj.method(...)` is a
struct field access vs an `impl` method dispatch, based on the struct types / impl methods
it has seen. In the warm runner each test file is compiled by a SEPARATE `Compiler` that
never saw the prelude's `impl` blocks, so `i.plural(...)` compiles as a field access →
runtime "Struct has no field 'plural'". The cold path compiles prelude+file together, so
the compiler knows `plural` is a method.

Read `src/vm.c` to confirm: find how `impl` blocks register methods (the dispatch table:
`dispatch_type_names`/`dispatch_method_names`/`dispatch_functions`, `vm_register_dispatch`,
`BC_DISPATCH`), how a struct type knows its methods, and how `obj.member(...)` is compiled
(field-get + call vs a dispatch/method opcode) and executed (the runtime that produces the
"Struct has no field" error).

## Fix — choose the cleanest, confirm against vm.c

Preferred if feasible: **runtime fallback.** In the field-access runtime path that raises
"Struct has no field 'X'", before erroring, look up the dispatch table by the struct's
type name + "X"; if a method exists, produce the callable/bound method (so the following
call runs with the struct as `self`). This needs no compiler coupling and fixes ALL
prelude-defined impl methods in the warm runner at once.

Alternative: **compiler seeding.** Make the per-file compiler aware of the prelude's known
struct types + impl method names so it emits the dispatch path for `obj.method()`. If you
choose this, capture the registry from the prelude compile in `warm_vm_init` and feed it
into the per-file `compiler_init` in `run_test_file_warm`.

Keep the change minimal and contained to `src/vm.c` (+ its header) and/or
`src/test_runner.c`. Do NOT change the Summary/`_Exit` block at the end of `test_run_dir`.
Do NOT alter the existing array-for-in code in vm.c. Do NOT regress the cold `vn run` path.

## Verify (compile only — orchestrator links)
```bash
M=/c/Users/X1/scoop/apps/msys2/2026-06-11/msys64
export PATH="$M/mingw64/bin:$M/usr/bin:$PATH"
TD="/c/Users/X1/AppData/Local/Temp/vnbuild"; mkdir -p "$TD"; export TMP="$TD" TEMP="$TD" TMPDIR="$TD"
CF="-Wall -Wextra -std=gnu11 -g -Iinclude -D_POSIX_C_SOURCE=200809L -IC:/deps/include -D_WIN32_WINNT=0x0600 -DUSE_LOCAL_TRE_H -Ideps/tre-0.9.0/local_includes -IC:/deps -IC:/deps/hiredis/include -IC:/deps/libffi/include -DVN_NO_POSTGRES -DVN_NO_SMTP"
gcc $CF -c src/vm.c -o build/vm.o
gcc $CF -c src/test_runner.c -o build/test_runner.o   # only if you changed it
```
Confirm zero compile errors. The orchestrator will link and then verify that the WARM repro
above prints success, that `tests/i18n_test.vn` passes, and that the lumen
`Struct has no field`/`Array has no method` failures are reduced.

## Report
- The exact mechanism you found in vm.c (file:line) for `obj.method()` compile + runtime.
- Which fix you chose and why, with file:line of the change.
- Confirmation your objects compile.
