#!/usr/bin/env bash
# Integration test for namespaced imports `use "pkg" as alias`
set -u
VN="$(cd "$(dirname "$0")/.." && pwd)/vn"
WORK="/tmp/varian_namespacing_test"
rm -rf "$WORK"
mkdir -p "$WORK/vn_modules/pkg_a"
mkdir -p "$WORK/vn_modules/pkg_b"
cd "$WORK"
export ASAN_OPTIONS=detect_leaks=0
ulimit -c 0 2>/dev/null || true

# Define package A (defines Button)
cat > vn_modules/pkg_a/pkg_a.vn <<'EOF'
fn _render_impl(label) {
    return "[A] " + label
}
fn Button(label) {
    return _render_impl(label)
}
EOF

# Define package B (defines Button too, and has mutually-recursive functions to test hoisting)
cat > vn_modules/pkg_b/pkg_b.vn <<'EOF'
fn Button(label) {
    return "[B] " + label + " (is_even=" + is_even_str(5) + ")"
}

fn is_even_str(n) {
    if n == 0 { return "true" }
    return is_odd_str(n - 1)
}

fn is_odd_str(n) {
    if n == 0 { return "false" }
    return is_even_str(n - 1)
}
EOF

# Main program using both packages via alias
cat > main.vn <<'EOF'
use "pkg_a" as ui_a
use "pkg_b" as ui_b

print(ui_a.Button("Hello"))
print(ui_b.Button("World"))
EOF

# Helper to verify output
fail() { echo "FAIL: $1"; exit 1; }

# 1. Test source run
out=$("$VN" run main.vn 2>&1) || fail "source run failed: $out"
echo "$out" | grep -q "\[A\] Hello" || fail "source: pkg_a Button wrong output: $out"
echo "$out" | grep -q "\[B\] World (is_even=false)" || fail "source: pkg_b Button wrong output: $out"

# 2. Test private rendering fails (should throw a runtime error)
cat > main_private.vn <<'EOF'
use "pkg_a" as ui_a
print(ui_a._render_impl)
EOF
out=$("$VN" run main_private.vn 2>&1)
echo "$out" | grep -q "Struct has no field" || fail "private rendering error not clear: $out"

# 3. Test bundle build and run
"$VN" build main.vn >/dev/null 2>&1 || fail "bundle build failed"
out=$("$VN" run app.vnb 2>&1) || fail "bundle run failed: $out"
echo "$out" | grep -q "\[A\] Hello" || fail "bundle: pkg_a Button wrong output: $out"
echo "$out" | grep -q "\[B\] World (is_even=false)" || fail "bundle: pkg_b Button wrong output: $out"

# 4. Test native compile and run
"$VN" build main.vn --release app >/dev/null 2>&1 || fail "native build failed"
out=$(./app 2>&1) || fail "native run failed: $out"
echo "$out" | grep -q "\[A\] Hello" || fail "native: pkg_a Button wrong output: $out"
echo "$out" | grep -q "\[B\] World (is_even=false)" || fail "native: pkg_b Button wrong output: $out"

# 5. Test dedup with namespacing
cat > main_dedup.vn <<'EOF'
use "pkg_a" as a1
use "pkg_a" as a2
print(a1.Button("X"))
print(a2.Button("Y"))
EOF
out=$("$VN" run main_dedup.vn 2>&1) || fail "dedup source run failed: $out"
echo "$out" | grep -q "\[A\] X" || fail "dedup: a1 Button wrong output: $out"
echo "$out" | grep -q "\[A\] Y" || fail "dedup: a2 Button wrong output: $out"

echo "=== ALL namespacing TESTS PASSED ==="
exit 0
