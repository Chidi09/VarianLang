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

JS_FIXTURE="$TMP_DIR/javascript_asset.lumen"
cat > "$JS_FIXTURE" <<'EOF'
<template>
  <main><script src="/analytics.js"></script></main>
</template>
EOF
JS_LINT_OUTPUT=$($VN lint "$JS_FIXTURE" --format json 2>&1) || {
    printf '%s\n' "$JS_LINT_OUTPUT"
    echo "JavaScript assets must be valid in Lumen"
    exit 1
}
if printf '%s\n' "$JS_LINT_OUTPUT" | grep -q 'strictly TypeScript'; then
    echo "stale TypeScript-only lint contract remains"
    exit 1
fi

echo "Lumen tooling tests: client islands preserved, linted, and formatting idempotent"
