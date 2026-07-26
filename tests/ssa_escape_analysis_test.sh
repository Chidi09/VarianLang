#!/usr/bin/env bash
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d /tmp/varian-ssa-escape.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/main.vn" <<'EOF'
struct Point {
    x: int,
    y: int,
}

let global_point = 0

fn stores_to_global(a, b) {
    global_point = Point { x: a, y: b }
    return 1
}

fn local_only(a, b) {
    let p = Point { x: a, y: b }
    return p.x + p.y
}

fn does_nothing(p) {
    return 1
}

fn passes_to_call(a, b) {
    let p = Point { x: a, y: b }
    return does_nothing(p)
}

fn returns_struct(a, b) {
    let p = Point { x: a, y: b }
    return p
}

fn captures_in_closure(a, b) {
    let p = Point { x: a, y: b }
    fn inner() {
        return p
    }
    return inner()
}

fn closure_no_capture(a, b) {
    let p = Point { x: a, y: b }
    fn helper(n) {
        return n + 1
    }
    return helper(a) + p.x
}

print(local_only(1, 2))
EOF

output=$($ROOT/vn compile "$WORK/main.vn" "$WORK/main.c" --dump-ssa 2>&1)

check_alloc() {
    local fn="$1" expected="$2"
    local body
    body=$(echo "$output" | awk "/^function ${fn} \{/,/^}/")
    echo "$body" | grep -q "alloc.struct Point" || { echo "FAIL: no alloc.struct found in $fn"; exit 1; }
    echo "$body" | grep "alloc.struct Point" | grep -q "\[${expected}" || {
        echo "FAIL: $fn expected [$expected, got:"
        echo "$body" | grep "alloc.struct Point"
        exit 1
    }
}

# Rule 1: stored into a global -> escapes (HEAP).
check_alloc "stores_to_global" "HEAP"

# Only field-read locally, never escapes -> proven safe to stack-allocate.
check_alloc "local_only" "STACK"

# Rule 4 (deliberately maximally conservative): passed to another analyzed,
# non-suspending function that does nothing with it -> STILL escapes. This
# confirms the "any call escapes all its args, unconditionally" policy is
# actually enforced, not silently optimized past.
check_alloc "passes_to_call" "HEAP"

# Rule 3: returned -> escapes, no interprocedural "caller keeps it local"
# analysis attempted.
check_alloc "returns_struct" "HEAP"

# Captured by a nested closure -> must escape (closures capture by value at
# creation, but the closure itself can outlive this function's frame).
check_alloc "captures_in_closure" "HEAP"

# A sibling nested closure that does NOT reference the local at all must not
# force it to escape merely by existing in the same function.
check_alloc "closure_no_capture" "STACK"

echo "=== SSA escape analysis tests passed ==="
