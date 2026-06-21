#!/usr/bin/env bash
# Integration test: `use "<pkg>"` loads a vendored Constellation package across
# all three execution backends (source / bundle / native), and a missing
# package fails loudly with an actionable message.
set -u
VN="$(cd "$(dirname "$0")/.." && pwd)/vn"
WORK="/tmp/varian_use_test"
rm -rf "$WORK"; mkdir -p "$WORK/vn_modules/greeter"
cd "$WORK"
export ASAN_OPTIONS=detect_leaks=0
ulimit -c 0 2>/dev/null || true

cat > vn_modules/greeter/greeter.vn <<'EOF'
fn greeter_hello(name) { return "Hello " + name }
EOF
cat > main.vn <<'EOF'
use "greeter"
print(greeter_hello("World"))
EOF

fail() { echo "FAIL: $1"; exit 1; }

# 1. source
out=$("$VN" run main.vn 2>&1) || fail "source run errored: $out"
echo "$out" | grep -q "Hello World" || fail "source: wrong output: $out"

# 2. bundle
"$VN" build main.vn >/dev/null 2>&1 || fail "bundle build failed"
out=$("$VN" run app.vnb 2>&1) || fail "bundle run errored: $out"
echo "$out" | grep -q "Hello World" || fail "bundle: wrong output: $out"

# 3. native
"$VN" build main.vn --release app >/dev/null 2>&1 || fail "native build failed"
out=$(./app 2>&1) || fail "native run errored: $out"
echo "$out" | grep -q "Hello World" || fail "native: wrong output: $out"

# 4. missing package => clear error mentioning `vn add`
printf 'use "nope"\n' > bad.vn
out=$("$VN" run bad.vn 2>&1) && fail "missing package should have failed"
echo "$out" | grep -q "vn add nope" || fail "missing package error not actionable: $out"

# 5. dedup: using the same package twice must not double-define
cat > dup.vn <<'EOF'
use "greeter"
use "greeter"
print(greeter_hello("Twice"))
EOF
out=$("$VN" run dup.vn 2>&1) || fail "dedup run errored: $out"
echo "$out" | grep -q "Hello Twice" || fail "dedup: wrong output: $out"

# 6. stdlib is NOT shadowed by a project-local vn_modules (the critical fix):
#    new_app (a Zenith stdlib fn) must still resolve here.
printf 'let app = new_app()\nprint("stdlib ok")\n' > stdlib.vn
out=$("$VN" run stdlib.vn 2>&1) || fail "stdlib shadowed by local vn_modules: $out"
echo "$out" | grep -q "stdlib ok" || fail "stdlib not available: $out"

# 7. a package that redefines an existing symbol is a loud error, not a silent shadow
mkdir -p vn_modules/hijacker
printf 'fn new_app() { return "X" }\n' > vn_modules/hijacker/h.vn
printf 'use "hijacker"\n' > hij.vn
out=$("$VN" run hij.vn 2>&1) && fail "symbol collision should have failed"
echo "$out" | grep -q "already exists in scope" || fail "collision not reported: $out"

echo "=== ALL use-package TESTS PASSED ==="
