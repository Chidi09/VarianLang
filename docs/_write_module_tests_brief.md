# Brief: Write dedicated tests for untested VarianLang modules

Work in `C:/Users/X1/CHIDIS WORKSPACE/VarianLang`. Create new test files under `tests/`.
A prebuilt `vn.exe` already exists — USE IT to verify. Do NOT run `make`, do NOT relink,
do NOT delete or modify anything in `build/`, do NOT touch `src/`, `deploy/`, or existing
`vn_modules/*.vn`. You only CREATE new `tests/<module>_test.vn` files.

## Modules that need tests

No coverage today — write a dedicated `tests/<m>_test.vn` for each:
`ai`, `auth`, `auth_mfa`, `auth_oauth`, `auth_rbac`, `db`, `mail`, `observe`, `shield`,
`storage`, `validate`.

Thin coverage — write a fuller dedicated `tests/<m>_test.vn` for each:
`config`, `i18n`, `pagination`, `migration`.

## How to discover each module's real API (do NOT invent functions)

For each module, read `vn_modules/<m>.vn` and find:
- top-level `fn <name>(...)` definitions (the public functions), and
- any namespace export like `let <m> = http.create_struct([...names...], [...lambdas...])`
  (call these as `<m>.method(...)`).
Test ONLY functions/methods that actually exist. Grep before you write.

## CRITICAL: tests must NOT hang and must NOT need external services

Many modules talk to networks/DBs/redis/SMTP/sockets. On Windows those calls BLOCK
forever (no connect timeout) and would make the test hang — which gets the file
quarantined. So:
- Test only PURE / constructible / formatting / validation logic that needs no I/O.
- For any function that requires a live backend (db.connect, mail send, storage upload,
  oauth exchange, redis-backed rate limit, etc.), DO NOT call it. Instead assert it
  exists: `assert_ne(function_name, null)` (this is the established pattern — see the
  `migration` smoke test that just does `assert_ne(new_migrator, null)`).
- Never open a server, socket, DB connection, or make an HTTP request in a test.

If you are unsure whether a function does I/O, assume it might and use the
`assert_ne(fn, null)` existence check rather than calling it.

## Test file format (copy this style)

See `tests/regex_test.vn` and `tests/assertions_test.vn` for working examples. Pattern:
```
test "descriptive name" {
    let x = some_pure_function(args)
    assert_eq(x, expected)
}
```
Helpers available: `assert_eq(a,b)`, `assert_ne(a,b)`, `assert_throws(|| { ... })`.

## VarianLang syntax rules (violating these = parse error)

- NO C-style ternary `a ? b : c`; NO if-as-expression. Use a temp var + `if {}`.
- NO keyword as identifier: never name a var/param `type`, `fn`, `match`, `schema`.
- Strings are SINGLE-LINE only. Build long strings by concatenation.
- NO runtime type introspection (`type_of`, `VAL_INT`, etc. do NOT exist).
- `for x in arr {}` and `for i in 0..n {}` both work. `.push()` returns a NEW array.
- String length is NOT `.len()` — check how existing modules/tests get string length
  before using one; for arrays `.len()` is fine.

## Verify EACH file (this is mandatory)

After writing each `tests/<m>_test.vn`, run it with a timeout and confirm it passes
WITHOUT hanging:
```bash
cd "/c/Users/X1/CHIDIS WORKSPACE/VarianLang"
M=/c/Users/X1/scoop/apps/msys2/2026-06-11/msys64
export PATH=".:$M/mingw64/bin:$M/usr/bin:$PATH"
TD="/c/Users/X1/AppData/Local/Temp/vnbuild"; export TMP="$TD" TEMP="$TD"; unset TMPDIR
timeout -k 3 20 ./vn.exe test tests/<m>_test.vn --timeout 6
```
- If it HANGS (timeout kills it, exit 124): you triggered I/O. Remove the offending
  call and replace with an `assert_ne(fn, null)` existence check. Re-run until it passes
  fast.
- If it reports a parse/compile error: you used unsupported syntax or a non-existent
  function. Fix it.
- A file is DONE only when it prints all `✅ PASS` and exits 0 quickly.
- Between runs, if a previous run hung, kill leftovers: `taskkill //IM vn.exe //F`.

## Report
List each test file you created, how many tests each has, and confirm each passes
fast (no hang). Note any module where you could only do existence-checks because
everything needs I/O.
