#!/usr/bin/env bash
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d /tmp/varian-mail-builder.XXXXXX)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/email_templates"

cat > "$WORK/email_templates/welcome.html" <<'EOF'
<h1>Hello {{name}}</h1><p>{{message}}</p>
EOF

cat > "$WORK/main.vn" <<'EOF'
let msg = email("user@example.com", "Welcome")
let chained = msg.from("team@example.com").cc("audit@example.com").text("plain")
assert(chained.from_addr == "team@example.com")
assert(chained.cc == "audit@example.com")
assert(chained.text_body == "plain")
chained.template("welcome", http.create_struct(
    ["name", "message"],
    ["<Admin>", "A&B"]
))
assert(chained.html_body.index_of("&lt;Admin&gt;") >= 0)
assert(chained.html_body.index_of("A&amp;B") >= 0)
assert_throws(| | { chained.reply_to("safe@example.com\nBcc: attacker@example.com") })
assert_throws(| | { chained.attach("file.txt\r\nX-Evil: yes", "text/plain", "body") })
assert_throws(| | { chained.template("../secrets", null) })
print("mail builder ok")
EOF

output=$(cd "$WORK" && "$ROOT/vn" run main.vn 2>&1)
case "$output" in
    *"mail builder ok"*) ;;
    *) echo "FAIL: mail builder integration failed: $output"; exit 1 ;;
esac

(cd "$WORK" && "$ROOT/vn" build main.vn >/dev/null)
mkdir "$WORK/detached"
bundle_output=$(cd "$WORK/detached" && "$ROOT/vn" run ../app.vnb 2>&1)
case "$bundle_output" in
    *"mail builder ok"*) ;;
    *) echo "FAIL: bundled email template was not self-contained: $bundle_output"; exit 1 ;;
esac

echo "=== Mail builder integration tests passed ==="
