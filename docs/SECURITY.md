# Security

This document describes Varian/Zenith's security posture: what it defends against, what
it does **not**, and how to build and write apps so they are hard to attack.

## Threat models

| Threat | Status |
| --- | --- |
| **Remote attackers** (untrusted HTTP traffic) | Defended — this is the primary design target. |
| **App-level** (your app's own users: XSS, CSRF, SQLi, auth bypass) | Defended by `shield`/`auth`/`sanitize`/`validate` + parameterized DB drivers, if you use them. |
| **Secrets & data at rest** | Supported — JWT signing, password hashing, secure cookie defaults, TLS. |
| **Untrusted `.vn` code** (running programs you did not write) | **NOT defended. See "Sandboxing" below.** |

## Sandboxing — running untrusted `.vn` is not safe

Varian is **not** a sandbox. A `.vn` program is fully trusted and can take over the host
by design:

- **FFI** (`@ffi("lib.so", "sym")`) `dlopen`/`dlsym`s arbitrary native libraries.
- **`python.run(...)`** executes arbitrary Python.
- **`io`** has unrestricted filesystem access; **`http.get`** enables outbound requests
  (SSRF); unbounded loops can exhaust CPU/memory.

Do not run `.vn` files from untrusted sources, and do not build a multi-tenant
"run user-submitted code" service on the current runtime. A restricted execution mode
(FFI/Python/IO disabled + CPU/memory/wall-clock quotas) would be required first and does
not exist yet.

## Memory safety of the interpreter

The VM is written in C, so a missing bounds check is the highest-severity (RCE) risk. The
hot paths have been audited:

- The HTTP request parser (picohttpparser front end) clamps method/path/query/header/body
  lengths before every `memcpy`.
- All identifier/name copying uses `malloc(strlen+1)` + `strcpy` (correctly sized); there
  are no raw `sprintf`/`strcat`/`gets` calls in the tree.
- The whole test + example suite runs clean under `make asan` (ASan + UBSan), with zero
  sanitizer reports.

When changing C code, re-run `make asan && ASAN_OPTIONS=detect_leaks=0 ./vn test tests/`
before committing. Prefer `snprintf`/explicit length clamps over fixed-buffer `memcpy`.

## Hardened build

Ship the `make release` binary, never the `-g` debug build. It enables:

- `_FORTIFY_SOURCE=2` — libc buffer-overflow checks (needs `-O`, hence release-only).
- `-fstack-protector-strong` + `-fstack-clash-protection` — stack canaries / clash guards.
- `-Wl,-z,relro,-z,now` — full RELRO, immediate binding (GOT/PLT not writable at runtime).
- `-Wl,-z,noexecstack` — non-executable stack.
- `-fPIE -pie` — position-independent executable for ASLR.

Verify with `readelf -d vn | grep BIND_NOW`, `readelf -l vn | grep GNU_STACK`, and
`file vn` (should report `pie executable`).

## Built-in application defenses

- **`auth`** — `sign_jwt`/`verify_jwt`, `hash_password`/`verify_password`,
  `generate_token`, and `constant_time_eq` (use it for any secret comparison to avoid
  timing leaks). See `docs/STDLIB.md`.
- **`shield`** (in `vn_modules/`) — `cors(...)`, `csrf()`, and `rate_limit(...)` /
  `rate_limit_redis(...)` middleware.
- **`sanitize`** — `escape_html`/`strip_html` to neutralize XSS before rendering. The
  Zenith template engine (`<%= %>`) HTML-escapes by default; `<%- %>` is the explicit
  raw/unescaped form — only use it on trusted content.
- **Secure cookies by default** — Zenith's `set_cookie`/sessions emit
  `Path=/; SameSite=Lax; HttpOnly`. Sessions are JWT-signed and reject tampered/wrong-secret
  tokens (return `null`).
- **Parameterized queries** — `sqlite.query`/`postgres.query` use bound parameters
  (`sqlite3_bind_*` / `PQexecParams`). Pass values as args, never string-concatenate them
  into SQL. `vn lint --only security` flags concatenated SQL and hardcoded secrets.
- **SMTP header-injection guard** — `smtp.send` rejects CR/LF in `from`/`to`/`subject`.

## Reporting

This is a young runtime; audits are ongoing. Treat the sandboxing limitation above as the
single most important caveat when deploying.
