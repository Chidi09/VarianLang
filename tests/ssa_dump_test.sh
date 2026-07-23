#!/usr/bin/env bash
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d /tmp/varian-ssa-dump.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/main.vn" <<'EOF'
fn fib(n) {
    if n < 2 {
        return n
    }
    return fib(n - 1) + fib(n - 2)
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

fn call_through_param(f, a) {
    return f(a)
}

print(fib(5))
print(sum_to(5))
EOF

output=$($ROOT/vn compile "$WORK/main.vn" "$WORK/main.c" --dump-ssa 2>&1)

# No block should ever be left unsealed once construction finishes.
if echo "$output" | grep -q "UNSEALED"; then
    echo "FAIL: an SSA block was left unsealed:"
    echo "$output" | grep -B2 "UNSEALED"
    exit 1
fi

# fib and sum_to are both leaf/non-suspending -> SSA is built for them.
echo "$output" | grep -q "^function fib {" || { echo "FAIL: no SSA built for fib"; exit 1; }
echo "$output" | grep -q "^function sum_to {" || { echo "FAIL: no SSA built for sum_to"; exit 1; }

# call_through_param calls through a parameter (unresolved callee) -> suspend
# analysis conservatively excludes it from SSA entirely. This is the same
# soundness boundary suspend_analysis_test.sh checks, verified again here at
# the SSA-construction layer (Phase A.2 must respect Phase A.1's gate).
if echo "$output" | grep -q "^function call_through_param {"; then
    echo "FAIL: SSA was built for a function suspend_analyze flagged SUSPENDS"
    exit 1
fi

# fib recurses through a statically-resolved direct call.
echo "$output" | grep -q "call.direct fib" || { echo "FAIL: fib's recursive call wasn't resolved to call.direct"; exit 1; }

# sum_to's accumulator must produce a phi node at the loop header (the
# actual point of the Braun-algorithm construction).
echo "$output" | grep -q "= phi " || { echo "FAIL: no phi node found for the loop accumulator"; exit 1; }

echo "=== SSA dump tests passed ==="
