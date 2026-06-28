# Brief: Fix remaining VarianLang test failures + add module tests

You are working in the VarianLang repo (a bytecode-compiled language; `.vn` programs run via `vn run file.vn`). vn.exe is already built for Windows. Your job has TWO parts:

1. **Fix the remaining failing tests** (list in section C).
2. **Write new dedicated test files** for modules that currently lack real coverage (section D).

You will NOT rebuild the compiler. All `vn_modules/*.vn` are concatenated into a ~13k-line PRELUDE prepended to every program at runtime, so editing a `.vn` module takes effect immediately — no rebuild needed.

## A. How to run tests (exact commands — Git Bash / msys2)

```bash
cd "/c/Users/X1/CHIDIS WORKSPACE/VarianLang"
M=/c/Users/X1/scoop/apps/msys2/2026-06-11/msys64
export PATH=".:$M/mingw64/bin:$M/usr/bin:$PATH"
export TMP="C:\\Users\\X1\\AppData\\Local\\Temp" TEMP="C:\\Users\\X1\\AppData\\Local\\Temp"
unset TMPDIR
# single file:
./vn.exe run tests/SOMETHING_test.vn
# whole suite (ALWAYS cap with timeout so a hang can't block you):
timeout -k 5 180 ./vn.exe test tests/ --timeout 8
```

A test file uses `test "name" { ... assert_eq(a,b) ... }`. Helpers: `assert_eq`, `assert_ne`, `assert_throws`. See `tests/module_smoke_test.vn` for the canonical style.

## B. VarianLang syntax rules you MUST respect (DO NOT violate — these cause parse errors)

- **NO C-style ternary** `cond ? a : b`. NO if-as-expression. Use a temp var + `if {}`.
- **NO keywords as identifiers**: `fn`, `type`, `match`, `schema` are reserved-ish in context; never name a param/var `type` or `fn` (rename to `og_type`, `callback`, etc.).
- **Strings are single-line only** (lexer stops at newline). No multi-line string literals; build long strings by concatenation across statements.
- **NO runtime type introspection**: there is NO `type_of`, `typeof`, `VAL_INT`, `VAL_FUNCTION`, `VAL_CLOSURE`. Those identifiers DO NOT EXIST. To branch on "is it callable", attempt the call inside `try { ... } catch e { ... }`. To branch on "is it a number", compare values (e.g. `if x == 0`, `if x == true`).
- Struct namespace pattern: `let ns = http.create_struct(["a","b"], [ |x|{...}, |y|{...} ])` then call `ns.a(...)`.
- Field access on a non-struct throws "Cannot access field on non-struct value" — never write `someInt.type`.
- `for x in arr { }` and `for i in 0..n { }` both work. `.push()` returns a NEW array (`a = a.push(x)`).
- String length is NOT `.len()` — check existing modules for the right accessor before using one.

## C. Failing tests to FIX

<!-- FILLED IN AFTER SUITE RUN -->
(TO BE COMPLETED — see the captured failures the orchestrator will paste here.)

Known so far:
- `vn_modules/feature.vn` lines ~47 and ~58 reference `VAL_INT` / `VAL_FUNCTION` / `VAL_CLOSURE` and `def.type` (a value's `.type`). These are invalid (see section B). Rewrite:
  - `enabled(self,name)`: after the `def == true` / `def == false` checks, an integer flag should be truthy when non-zero. Replace `if def.type == VAL_INT { return def != 0 }` with logic that does NOT use `.type` or `VAL_*` — e.g. treat any remaining non-null, non-false, non-zero def as enabled: `if def == 0 { return false }` then `return def != null`.
  - `enabled_for(self,name,context)`: replace the `if def.type == VAL_FUNCTION or def.type == VAL_CLOSURE` guard with a `try { return def(context) == true } catch e { return self.enabled(name) }` (attempt the call; if it's not callable, fall back).
  - Keep behavior identical for the existing smoke tests (booleans + override).
- `'children' undefined` failures in `aurora_pending_features_test.vn` are EXPECTED pending features (nested layouts/slots/presence) — do NOT try to implement them; leave those as known-pending. Only fix them if a fix is trivial and the orchestrator confirms.

## D. Modules needing NEW dedicated test files (`tests/<module>_test.vn`)

NO coverage today — write real tests:
- `ai`, `auth`, `auth_mfa`, `auth_oauth`, `auth_rbac`, `db`, `mail`, `observe`, `shield`, `storage`, `validate`

Thin smoke coverage only (1–3 cases) — write fuller dedicated tests:
- `config`, `i18n`, `pagination`, `migration`

Rules for new tests:
- Read the module's public functions first (`vn_modules/<m>.vn`) and test the real, callable API — do NOT invent functions. Grep the module for `fn ` and the namespace `create_struct` export.
- Tests must run WITHOUT network, DB, or external services. For modules that need a DB/HTTP (db, mail, storage, auth_oauth), test only the pure/constructible parts (struct construction, token formatting, validation logic) and assert functions exist with `assert_ne(fn_name, null)` where a live backend is required (mirror the existing `migration module — construction` smoke test at `tests/module_smoke_test.vn:221`).
- Every new test file MUST pass: `./vn.exe run tests/<m>_test.vn` exits 0 with all `✅ PASS`.

## E. After you finish

- Run the full suite (section A) and confirm no NEW failures vs the baseline.
- Print a summary: which files you edited, which test files you created, and the final pass/fail counts.
- Do NOT touch `deploy/` — the orchestrator handles mirroring.
