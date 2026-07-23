#!/usr/bin/env bash
# Phase C gate: every function Kiln compiles natively (typed, unboxed C locals)
# must produce bit-identical results to its boxed reference body.
#
# Two independent checks, because either alone would be weak:
#   1. Differential: build the fixture twice — once ordinarily, once with
#      --ssa-shadow (which makes every native body ALSO run the boxed body and
#      abort on divergence) — and require identical, expected output. This is
#      the real correctness gate.
#   2. Negative control: an injected bug in a native body must make the shadow
#      build abort. Without this, check 1 passing proves nothing — a shadow
#      harness that never fires looks exactly like one that always agrees.
#
# Everything lives in ONE fixture program deliberately: each `vn build` has to
# run a C compiler over ~180k lines of generated code, so a fixture-per-case
# layout would cost minutes of wall clock for no extra coverage.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d /tmp/varian-ssa-diff.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

# Cases concentrate on the boundaries where a typed/unboxed body is most likely
# to drift from the boxed one: int/float promotion, ordering comparisons (which
# in Varian yield int/float, NOT bool), truthiness of numeric conditions, loop
# accumulators carried through phis, int overflow wraparound, and the entry
# guard that catches a violated parameter annotation.
cat > "$WORK/main.vn" <<'EOF'
fn mix(a, b) {
    return a * 2 + b
}
fn cmp_int(a, b) {
    return a < b
}
fn neg_and_bits(a) {
    return (-a) ^ 255
}
fn promote(a: float, b: int) {
    return a + b
}
fn fcmp(a: float, b: float) {
    return a <= b
}
fn fdiv(a: float, b: float) {
    return a / b
}
fn sum_to(n) {
    let total = 0
    let i = 0
    while i < n {
        total = total + i
        i = i + 1
    }
    return total
}
fn countdown(n) {
    let acc = 1
    while n > 1 {
        acc = acc * n
        n = n - 1
    }
    return acc
}
fn wrap(a, b) {
    return a * b
}
fn big_shift(a, n) {
    return a << n
}
fn takes_int(a: int) {
    return a + 1
}
// Native-to-native ABI: recursion and cross-function calls are real C calls
// with raw scalar arguments, so these exist only because that ABI does.
fn fib(n) {
    if n < 2 {
        return n
    }
    return fib(n - 1) + fib(n - 2)
}
fn square(x) {
    return x * x
}
fn hypot_sq(a, b) {
    return square(a) + square(b)
}
// Mutual recursion, to prove the eligibility fixpoint converges rather than
// demoting one arm of a cycle.
fn is_even(n) {
    if n == 0 {
        return 1
    }
    return is_odd(n - 1)
}
fn is_odd(n) {
    if n == 0 {
        return 0
    }
    return is_even(n - 1)
}

print(mix(21, 3))
print(cmp_int(2, 9))
print(cmp_int(9, 2))
print(neg_and_bits(4))
print(promote(0.5, 2))
print(fcmp(1.5, 1.5))
print(fdiv(7.0, 2.0))
print(sum_to(100))
print(countdown(10))
print(sum_to(0))
print(wrap(4611686018427387904, 2))
print(big_shift(1, 62))
// Guard path: the annotation is a belief, not a contract — a caller that
// violates it must fall back to the boxed body and still answer correctly,
// never unbox a mismatched tag.
print(takes_int(41))
print(takes_int(1.5))
print(takes_int("x"))
print(fib(20))
print(hypot_sq(3, 4))
print(is_even(10))
print(is_odd(10))
EOF

cat > "$WORK/expected.txt" <<'EOF'
45
1
0
-253
2.5
1
3.5
4950
3628800
0
-9223372036854775808
4611686018427387904
42
2.5
x1
6765
25
1
0
EOF
EXPECTED=$(cat "$WORK/expected.txt")

build_and_run() {
    local tag="$1"
    shift
    local dir="$WORK/$tag"
    mkdir -p "$dir"
    cp "$WORK/main.vn" "$dir/main.vn"
    ( cd "$dir" && "$ROOT/vn" build main.vn --release "$@" prog ) > "$dir/build.log" 2>&1 \
        || { cat "$dir/build.log" >&2; fail "$tag build failed"; }
    "$dir/prog" > "$dir/out.txt" 2>"$dir/err.txt" \
        || { cat "$dir/err.txt" >&2; fail "$tag run exited non-zero"; }
    cat "$dir/out.txt"
}

plain=$(build_and_run plain)
[ "$plain" = "$EXPECTED" ] || {
    diff <(echo "$EXPECTED") <(echo "$plain") || true
    fail "native release output does not match expected"
}
echo "  ok: native release output matches expected"

shadow=$(build_and_run shadow --ssa-shadow)
[ "$shadow" = "$EXPECTED" ] || {
    diff <(echo "$EXPECTED") <(echo "$shadow") || true
    fail "shadow-mode output does not match expected"
}
echo "  ok: shadow mode agrees with the boxed reference body on every call"

# Guard against a vacuous pass: if nothing was specialized, the two builds above
# would trivially agree and prove nothing about the native backend.
grep -q "Natively compiled" "$WORK/plain/build.log" \
    || fail "nothing was compiled natively — the gate is vacuous"

# Negative control: corrupt one native body in the generated C and require
# shadow mode to catch it. Patching the generated C (rather than rebuilding vn
# with a broken emitter) injects exactly the class of divergence the harness
# exists to detect, without disturbing the build tree.
NEG="$WORK/shadow"
python3 - "$NEG/prog.c" <<'PY'
import re, sys
path = sys.argv[1]
src = open(path).read()
start = src.index("Native body for 'sum_to'")
end = src.index("\n}\n", start)
body = src[start:end]
# Flip the accumulator update inside sum_to's native body only.
patched, n = re.subn(r"= \(int64_t\)\(v(\d+) \+ v(\d+)\);", r"= (int64_t)(v\1 - v\2);",
                     body, count=1)
if n != 1:
    sys.exit("negative control: found no add to corrupt inside sum_to")
open(path, "w").write(src[:start] + patched + src[end:])
PY

( cd "$NEG" && cc -DVARIAN_SSA_SHADOW_MODE -O2 -I"$ROOT/include" prog.c -o corrupt \
    "$ROOT/libvarian.a" -lm -lffi -ldl -lcurl -lpq -lcrypto -lssl -lsqlite3 \
    -lhiredis -lpthread -luring -lz ) >/dev/null 2>&1 \
    || fail "negative control: corrupted C did not compile"

if ( cd "$NEG" && ./corrupt ) >"$NEG/neg_out.txt" 2>"$NEG/neg_err.txt"; then
    fail "negative control PASSED — shadow mode did not detect an injected bug"
fi
grep -q "SSA SHADOW DIVERGENCE" "$NEG/neg_err.txt" \
    || { cat "$NEG/neg_err.txt" >&2; fail "negative control aborted, but not via the shadow check"; }
echo "  ok: negative control — shadow mode detects an injected divergence"

# Depth guard: native-to-native calls push no CallFrame, so the interpreter's
# `frame_count >= TASK_FRAMES_MAX` check cannot see them and VM.native_depth is
# the only thing standing between unbounded recursion and a C stack overflow.
#
# Two details this fixture depends on, both learned the hard way:
#   - The `if n < 0` base case is unreachable at runtime but load-bearing at
#     compile time: without it every return is self-referential, the return type
#     never gets concrete evidence, and the function is not natively compiled at
#     all — the check would silently test nothing.
#   - The `+ 1` keeps the recursion non-tail so gcc cannot turn it into a loop
#     and hide the very thing being tested.
# A Varian runtime error exits 0 (the interpreter behaves the same way), so the
# assertion is on the reported error and on not dying by signal, not on status.
DEEP="$WORK/deep"
mkdir -p "$DEEP"
cat > "$DEEP/main.vn" <<'EOF'
fn runaway(n) {
    if n < 0 {
        return 0
    }
    return runaway(n + 1) + 1
}
print(runaway(1))
EOF
( cd "$DEEP" && "$ROOT/vn" build main.vn --release prog ) >"$DEEP/build.log" 2>&1 \
    || { cat "$DEEP/build.log" >&2; fail "depth-guard build failed"; }
grep -q "Native body for 'runaway'" "$DEEP/prog.c" \
    || fail "runaway was not natively compiled — the depth-guard check is vacuous"

set +e
( cd "$DEEP" && ./prog ) >"$DEEP/out.txt" 2>"$DEEP/err.txt"
deep_rc=$?
set -e
[ "$deep_rc" -lt 128 ] \
    || fail "runaway recursion died on signal $((deep_rc - 128)) (SIGSEGV = 11) instead of erroring cleanly"
grep -q "Stack overflow" "$DEEP/err.txt" \
    || { cat "$DEEP/err.txt" >&2; fail "runaway recursion did not report a Stack overflow"; }
# Native recursion pushes no CallFrame, so the error trace must be a couple of
# frames deep, not the ~64 the boxed trampoline would unwind. This is what
# distinguishes "the native depth guard fired" from "it quietly fell back to
# the boxed path and TASK_FRAMES_MAX caught it instead".
frames=$(grep -c "in script" "$DEEP/err.txt")
[ "$frames" -lt 10 ] \
    || fail "error trace has $frames frames — recursion went through the boxed trampoline, not the native ABI"
echo "  ok: native recursion depth guard reports Stack overflow instead of crashing"

echo "=== SSA differential tests passed ==="
