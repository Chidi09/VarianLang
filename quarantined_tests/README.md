# Quarantined tests

These test files block in **native C code** (server accept loops, or redis/socket
connections without a timeout) and cannot be interrupted by the cooperative
`--timeout` flag, so they stall the single-process warm-VM test runner.

They are pre-existing native-blocking issues on Windows, NOT warm-VM regressions.
Restore them to `tests/` once the underlying native blocking is fixed
(e.g. connect timeouts, or a test-mode guard that skips spawning live servers).

- lumen_triple_brace_test.vn — spawns a live server
- lumen_vocab_test.vn        — spawns a live server
- lumenjs_test.vn            — spawns a live server
- web_test.vn                — spawns a live server
- zenith_onerror_test.vn     — spawns a live server
- module_smoke_test.vn       — first test (cache/redis) blocks on connect
