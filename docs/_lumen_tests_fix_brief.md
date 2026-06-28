# Brief: Fix the 6 remaining lumen test failures

Work in `C:/Users/X1/CHIDIS WORKSPACE/VarianLang`. A prebuilt `vn.exe` exists — USE IT to
verify. Do NOT run `make`, do NOT touch `build/`, `src/`, or `deploy/`. You only edit
test files under `tests/` (and, only if a real module bug is confirmed, `vn_modules/lumen.vn`).

## Current state

Full suite: 315 passed, 6 failed. The 6 failures are all in lumen live/round-trip tests:
1. `tests/lumen_m3_test.vn` — "lumen live round trip with child component event routing" → `Struct has no field 'path'`
2. `tests/lumen_m4_test.vn` — "live loop sends full html on first event, patch on second" → `Struct has no field 'path'`
3. `tests/lumen_m6_test.vn` — "a throwing handler is caught and sent as a friendly error message" → `Struct has no field 'path'`
4. `tests/lumen_m6_test.vn` — "the live shell embeds the branded overlay + logo, no tagline" → `Array has no method 'topic'`
5. `tests/lumen_roundtrip_test.vn` — "lumen live round trip mock" → `Struct has no field 'path'`
6. `tests/lumen_router_test.vn` — "build_dir compiles a pages directory into one runnable app" → `Lumen: pages directory not found: /tmp/lumen_router_test_pages`

## Diagnosis

Failures 1–5: the tests build a MOCK websocket with `http.create_struct(["read","write"], [...])`,
but the current `_lumen_live_loop` (and the live/pubsub code) in `vn_modules/lumen.vn` reads
MORE fields from the ws — e.g. `ws.path` (see `vn_modules/lumen.vn` around line 3475:
`let topic = ws.path`). The mock ws lacks `path`, so it errors. The mocks are STALE relative
to the current lumen live API.

Failure 4 (`Array has no method 'topic'`): something the live shell path treats as a struct
with `.topic` is actually an Array in the test's setup — likely the same stale-mock root, or
a connection-list shape mismatch. Investigate in `vn_modules/lumen.vn` (search `topic`,
`conn.topic`, the pubsub/connection registry) what shape it expects.

Failure 6: the router test uses a `/tmp/...` path that doesn't exist on Windows. Make the
test create its temp pages dir reliably (e.g. use the same dir the test already writes to;
ensure the directory is actually created before `build_dir` runs), or point it at a path that
works cross-platform.

## What to do

1. Read `vn_modules/lumen.vn` to find EVERY field `_lumen_live_loop` (and the functions it
   calls for the live round trip) reads off the `ws`/connection object (grep `ws.` and the
   live-loop body). Build the complete required field set.
2. Update the mock `ws` `http.create_struct([...], [...])` in `lumen_m3`, `lumen_m4`,
   `lumen_m6`, and `lumen_roundtrip` tests so the mock provides ALL fields the live loop
   reads (e.g. add `"path"` with a sensible value like `"/live"`, plus any others). Keep the
   existing `read`/`write` behavior. Do NOT change the assertions' intent.
3. For the `Array has no method 'topic'` case, determine the correct shape and fix the test's
   setup so the value is the struct the code expects (or, ONLY if it's a genuine lumen bug,
   fix `vn_modules/lumen.vn` minimally and explain why).
4. Fix `lumen_router_test.vn` so its pages directory exists on Windows before `build_dir`.

Prefer fixing the TESTS (stale mocks) over changing `lumen.vn`. Only touch `lumen.vn` if you
prove a real module bug; if so, keep it minimal and report it. If any failure turns out to be
a genuinely unimplemented feature, STOP on that one and report it as pending (do not fake it).

## VarianLang syntax reminders
- Build structs/maps with `http.create_struct([keys], [vals])` — NOT JS `{ key: val }`
  literals at value positions you control (those parse as blocks). (Note: existing lumen
  tests use `{ votes: 0 }` inside component lambdas and those currently work — don't churn
  them; only fix the mock `ws` create_struct calls and what's needed.)
- No ternary, no keyword-as-identifier, single-line strings.

## Verify each fixed file (mandatory, must not hang)
```bash
cd "/c/Users/X1/CHIDIS WORKSPACE/VarianLang"
M=/c/Users/X1/scoop/apps/msys2/2026-06-11/msys64
export PATH=".:$M/mingw64/bin:$M/usr/bin:$PATH"
TD="/c/Users/X1/AppData/Local/Temp/vnbuild"; export TMP="$TD" TEMP="$TD"; unset TMPDIR
timeout -k 3 20 ./vn.exe test tests/lumen_m3_test.vn --timeout 6
# ...repeat for lumen_m4, lumen_m6, lumen_roundtrip, lumen_router
taskkill //IM vn.exe //F >/dev/null 2>&1   # between runs if needed
```
Each must end all `✅ PASS`, exit 0, no hang. Then report exactly what you changed per file
and the final per-file pass counts.
