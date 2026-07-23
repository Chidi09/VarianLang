#!/usr/bin/env bash
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d /tmp/varian-ssa-types.XXXXXX)
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

struct Point {
    x: int,
    y: float,
}

fn get_x(p: Point) {
    return p.x
}

print(fib(5))
print(sum_to(5))
EOF

output=$($ROOT/vn compile "$WORK/main.vn" "$WORK/main.c" --dump-ssa 2>&1)

# The 3-level PENDING/concrete/UNKNOWN lattice must never leak PENDING into
# a final dump — that would mean a value was never finalized.
if echo "$output" | grep -q "PENDING"; then
    echo "FAIL: a PENDING type leaked past ssa_infer_types:"
    echo "$output" | grep "PENDING"
    exit 1
fi

# The flagship case: naive recursive fibonacci with zero annotations must
# fully resolve to int end-to-end (parameter, recursive call results, and
# the final return) — this requires the fixpoint to correctly bootstrap a
# self-referential (recursive) return type, not just straight-line code.
echo "$output" | grep -qE '%0:int = param' || { echo "FAIL: fib's param not inferred int"; exit 1; }
echo "$output" | grep -qE '= call\.direct fib %[0-9]+:int$' || { echo "FAIL: fib's own recursive call result not inferred int"; exit 1; }
echo "$output" | grep -qE '^    return %[0-9]+:int$' || { echo "FAIL: no int-typed return found at all"; exit 1; }

# The other flagship case: a loop-carried accumulator (the actual hot-path
# target for this whole milestone) must also resolve to int — this requires
# the same self-referential bootstrapping, but via a phi cycle within one
# function rather than cross-function recursion.
echo "$output" | grep -qE '= phi %[0-9]+:int, %[0-9]+:int$' || {
    echo "FAIL: no fully-int-typed phi found (loop accumulator didn't resolve)"
    exit 1
}

# Adversarial safety check: a field read through a proven struct-typed
# receiver must still stay unknown, because plain `struct` declarations
# discard field-type annotations during parsing (parser.c's
# parse_struct_decl) — there is currently no way to prove a field's type,
# and trusting the struct name alone would be exactly the "trust the decl
# blindly" shortcut this design explicitly rejects.
echo "$output" | grep -q "field.get .x" || { echo "FAIL: expected a field.get instruction for get_x"; exit 1; }
# Only check the RESULT type (left of '='); the receiver operand (right of
# the op name) is legitimately struct(Point) and must not be confused with it.
if echo "$output" | grep -E '^    %[0-9]+:[a-z]+.* = field\.get \.x' | grep -qvE ':unknown = field\.get'; then
    echo "FAIL: field.get's result resolved to a concrete type despite struct field types being unavailable:"
    echo "$output" | grep "field.get .x"
    exit 1
fi

echo "=== SSA type inference tests passed ==="
