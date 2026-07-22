#!/usr/bin/env bash
set -e

VN="./vn"
if [ ! -f "$VN" ]; then
    echo "Executable $VN not found. Build first."
    exit 1
fi

echo "=== Running Semantic Analysis Test Suite ==="
PASSED=0
FAILED=0

run_pass_test() {
    local file="$1"
    echo -n "Testing $file (expect PASS)... "
    if $VN check "$file" > /dev/null 2>&1; then
        echo "OK"
        PASSED=$((PASSED + 1))
    else
        echo "FAILED"
        echo "Command output:"
        $VN check "$file" || true
        FAILED=$((FAILED + 1))
    fi
}

run_fail_test() {
    local file="$1"
    local expected_code="$2"
    echo -n "Testing $file (expect FAIL with [$expected_code])... "
    set +e
    output=$($VN check "$file" 2>&1)
    status=$?
    set -e
    if [ "$status" -ne 0 ] && echo "$output" | grep -q "\[$expected_code\]"; then
        echo "OK"
        PASSED=$((PASSED + 1))
    else
        echo "FAILED"
        echo "Expected nonzero exit with diagnostic [$expected_code] (exit=$status):"
        echo "$output"
        FAILED=$((FAILED + 1))
    fi
}

# Positive tests
run_pass_test "tests/fixtures/semantic/pass_shadowing.vn"
run_pass_test "tests/fixtures/semantic/pass_closures.vn"
run_pass_test "tests/fixtures/semantic/pass_forward_calls.vn"
run_pass_test "tests/fixtures/semantic/pass_loops.vn"
run_pass_test "tests/fixtures/semantic/pass_builtins.vn"
run_pass_test "tests/fixtures/semantic/pass_structs_enums.vn"
run_pass_test "examples/hello.vn"
run_pass_test "examples/generics.vn"
run_pass_test "examples/enums.vn"
run_pass_test "examples/union_type_test.vn"
run_pass_test "examples/named_args_test.vn"
run_pass_test "examples/traits.vn"

# Negative tests
run_fail_test "tests/fixtures/semantic/fail_dup_decl.vn" "duplicate-declaration"
run_fail_test "tests/fixtures/semantic/fail_dup_param.vn" "duplicate-param"
run_fail_test "tests/fixtures/semantic/fail_undef_id.vn" "undefined-identifier"
run_fail_test "tests/fixtures/semantic/fail_assign_const.vn" "assign-const"
run_fail_test "tests/fixtures/semantic/fail_break_outside.vn" "break-outside-loop"
run_fail_test "tests/fixtures/semantic/fail_continue_outside.vn" "continue-outside-loop"
run_fail_test "tests/fixtures/semantic/fail_return_outside.vn" "return-outside-fn"
run_fail_test "tests/fixtures/semantic/fail_arity_mismatch.vn" "arity-mismatch"
run_fail_test "tests/fixtures/semantic/fail_type_mismatch.vn" "type-mismatch"

# Multiple errors in single run test
echo -n "Testing tests/fixtures/semantic/fail_multiple_errors.vn (expect multiple errors in single run)... "
set +e
multi_output=$($VN check "tests/fixtures/semantic/fail_multiple_errors.vn" 2>&1)
multi_status=$?
set -e
if [ "$multi_status" -ne 0 ] && \
   echo "$multi_output" | grep -q "\[duplicate-declaration\]" && \
   echo "$multi_output" | grep -q "\[break-outside-loop\]" && \
   echo "$multi_output" | grep -q "\[return-outside-fn\]"; then
    echo "OK"
    PASSED=$((PASSED + 1))
else
    echo "FAILED"
    echo "Multiple error output did not contain all expected errors:"
    echo "$multi_output"
    FAILED=$((FAILED + 1))
fi

echo "==========================================="
echo "Semantic Tests: $PASSED passed, $FAILED failed"
if [ "$FAILED" -ne 0 ]; then
    exit 1
fi
