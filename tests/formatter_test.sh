#!/usr/bin/env bash
set -euo pipefail

VN="./vn"
FIXTURE="tests/fixtures/lumen/format_client_blocks.lumen"
TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT
WORK="$TMP_DIR/format_client_blocks.lumen"
ONCE="$TMP_DIR/once.lumen"

cp "$FIXTURE" "$WORK"
"$VN" fmt "$WORK" >/dev/null
cp "$WORK" "$ONCE"
"$VN" fmt "$WORK" >/dev/null
cmp "$ONCE" "$WORK"

test "$(grep -c '<client' "$WORK")" -eq 2
grep -q '<client when="visible" target="#chart">' "$WORK"
grep -q '<client when="idle">window.mountLater();</client>' "$WORK"
grep -q 'document.querySelector("#chart")' "$WORK"
"$VN" fmt --check "$WORK"

set +e
LINT_OUTPUT=$("$VN" lint "$FIXTURE" --format json 2>&1)
LINT_STATUS=$?
set -e
test "$LINT_STATUS" -ne 0
printf '%s\n' "$LINT_OUTPUT" | grep -q 'Client JS block only uses fetch/toggle'

echo "Lumen tooling tests: client islands preserved, linted, and formatting idempotent"
