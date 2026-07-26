#!/usr/bin/env bash
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d /tmp/varian-aot-reachability.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/main.vn" <<'EOF'
let challenge = oauth_generate_code_challenge("verifier")
assert(challenge.len() > 0)
EOF

output=$($ROOT/vn compile "$WORK/main.vn" "$WORK/main.c" 2>&1)
case "$output" in
    *"Removed "*" unreachable prelude functions."*) ;;
    *) echo "FAIL: release compiler did not report prelude reachability: $output"; exit 1 ;;
esac

grep -q '"oauth_generate_code_challenge"' "$WORK/main.c" || {
    echo "FAIL: directly reachable prelude function was removed"; exit 1;
}
grep -q '"_base64url_encode"' "$WORK/main.c" || {
    echo "FAIL: transitively reachable prelude function was removed"; exit 1;
}
if grep -q '"oauth_login_or_register"' "$WORK/main.c"; then
    echo "FAIL: unrelated prelude function survived release reachability"; exit 1
fi

echo "=== AOT reachability tests passed ==="
