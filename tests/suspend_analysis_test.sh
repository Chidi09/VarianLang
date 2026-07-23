#!/usr/bin/env bash
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d /tmp/varian-suspend-analysis.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/main.vn" <<'EOF'
fn leaf_add(a, b) {
    return a + b
}

fn calls_leaf(a, b) {
    return leaf_add(a, b)
}

fn uses_await() {
    let x = await leaf_add(1, 2)
    return x
}

fn calls_suspending() {
    return uses_await()
}

fn calls_through_param(f) {
    return f()
}

let result = leaf_add(1, 2)
print(result)
EOF

output=$($ROOT/vn compile "$WORK/main.vn" "$WORK/main.c" --dump-suspend 2>&1)

check_status() {
    local name="$1" expected="$2"
    echo "$output" | grep -qE "^  ${name} -> ${expected}\$" || {
        echo "FAIL: expected '$name -> $expected' in suspend dump; got:"
        echo "$output" | grep -E "^  ${name} ->" || echo "  (no entry for $name at all)"
        exit 1
    }
}

# Leaf function with no suspending operations anywhere in its call graph.
check_status "leaf_add" "LEAF"

# Calls a statically-resolved, already-proven-leaf function -> still LEAF.
check_status "calls_leaf" "LEAF"

# Directly contains an `await` -> SUSPENDS.
check_status "uses_await" "SUSPENDS"

# Transitively calls a suspending function -> SUSPENDS (fixpoint propagation).
check_status "calls_suspending" "SUSPENDS"

# Calls through a parameter (unresolved callee) -> conservatively SUSPENDS,
# even though nothing here actually awaits anything. This is the critical
# soundness case: an unresolved call must never be assumed safe.
check_status "calls_through_param" "SUSPENDS"

echo "=== Suspend analysis tests passed ==="
