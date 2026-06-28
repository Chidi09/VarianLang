# VarianLang — Session Handoff (for Claude, continuing on the Linux VPS)

> This is a Claude-to-Claude continuity doc. It captures everything needed to resume the
> work without the user re-explaining. The user's local `~/.claude` memory is Windows-only
> and is NOT on the VPS, so this doc (committed to the repo) is the source of truth.

## 0. What VarianLang is
A bytecode-compiled language. C interpreter (`vn`), AST→bytecode compiler in `src/vm.c`.
Standard library = `vn_modules/*.vn`, concatenated into a ~13k-line **prelude** prepended to
every program at runtime. Sub-projects:
- **Lumen** — web framework (`vn_modules/lumen.vn`); `.lumen` files = HTML `<template>` + Varian `<script>`.
- **Kiln** — build tool (`docs/KILN.md`): reads `constellation.toml`/`.lock`, bundles single-file artifacts.
- **Constellation** — package manager + registry (`docs/CONSTELLATION.md`).
- **Aurora** — reactive data layer (`docs/AURORA.md`); "pillars" are in-progress features.

## 1. Repos (all owned by GitHub user **Chidi09 / chidiisking7@gmail.com**)
- `github.com/Chidi09/VarianLang` — main repo (origin).
- `github.com/Chidi09/tree-sitter-varian` — the editor grammar (referenced by the Zed extension as `[grammars.varian]`).
- `github.com/Chidi09/constellation-index` — the package registry index (created this session; branch-protected, requires reviewed PR).

⚠️ **GitHub identity gotcha**: on the Windows box `gh` was logged into a WRONG account (`Nneji123`). On the VPS, verify `gh auth status` shows **Chidi09** before any push/PR.

## 2. Branch state (as of this handoff)
- `main` — has: Windows port + **warm-VM test runner** + 216 module tests + lumen fixes + dispatch fix + Constellation docs + PR#1 (editor grammar/theme) merged. **315 tests pass / 6 fail.**
- `feat/editor-lsp-complete` — branched off main. Has commit `029243a` (Zed packaging fix: removed committed grammar build-artifacts + dangling gitlinks; gitignored `editors/zed-varian/grammars/`). **This is the active editor branch.** The grammar `rev` bump (see §4) goes here, then PR → main.
- `feature/editor-grammars-and-themes` — already merged via PR#1 (editor grammar/theme + Teal & Amber).

## 3. What got DONE this session
- **Windows**: full CLI build + run (was LSP-only). On Linux this is moot — native build is simpler.
- **Warm-VM test runner** (`src/test_runner.c`): compile prelude ONCE, run all test files in one VM (~5s vs unrunnable). Key fixes: reap dead tasks per file; re-run prelude per file for isolation; `_Exit` after Summary to dodge an exit-time native-thread hang; **dispatch fallback in `src/vm.c` (`L_BC_MEMBER`)** so prelude struct `impl` methods resolve (added `ObjBoundMethod` in `include/vm.h`).
- **216 module tests**: `tests/<m>_test.vn` for ai, auth(+mfa/oauth/rbac), db, mail, observe, shield, storage, validate, config, i18n, migration, pagination. (No I/O — backend-needing fns use `assert_ne(fn, null)`.)
- **6 native-hang tests quarantined** → `quarantined_tests/` (live servers / redis connect w/o timeout — block the single-process runner). On Linux these MIGHT behave differently; revisit.
- **Constellation registry stood up**: `Chidi09/constellation-index` (empty `index.json`, README, CONTRIBUTING, PR template, JSON-validation CI, branch protection: PR + 1 approval). Publish flow = contributor runs `vn publish`, opens PR adding their entry, maintainer reviews+merges.

## 4. OUTSTANDING TODO (priority order)
1. **Finish the Zed grammar** (launch-critical). A Sonnet subagent was pushing an updated
   `tree-sitter-varian` grammar (contextual `schema_definition` + `dispatch_call` + `self` + Lumen
   HTML injection, regenerated with `tree-sitter-cli 0.22.6`). **CHECK**: did it push? Get the new SHA:
   `gh api repos/Chidi09/tree-sitter-varian/commits/main --jq .sha`. If it's NOT the old
   `f33440b2de87f1b6897e95351db32222a308f542`, then on `feat/editor-lsp-complete` set
   `editors/zed-varian/extension.toml` → `[grammars.varian] rev = "<NEWSHA>"`, commit, and the Zed
   extension should install (grammar builds fresh, queries resolve, Teal & Amber applies).
   If the subagent did NOT finish: redo the grammar yourself on the VPS (Linux has node+tree-sitter
   easily) per `docs/_grammar_task_brief.md`. **HARD RULE: never add `schema` to the compiler lexer
   `src/lexer.c` — it breaks ~18+ `let schema = ...` identifier uses. See `docs/SCHEMA_SITUATION.md`.**
2. **Constellation default URL flip**: `src/pkg_manager.c:747` still defaults to the non-existent
   `varian-lang/constellation-index`. Change to
   `https://raw.githubusercontent.com/Chidi09/constellation-index/main/index.json`, rebuild, and add a
   `CONSTELLATION_INDEX_URL` note to `docs/CONSTELLATION.md`. (Until then it works via that env var.)
3. **LSP features** (`src/lsp.c`, additive, not launch-blocking): references, rename, signature help,
   context-aware completions (best-effort; dynamic lang), code actions, folding. Build a def-use/scope
   pass once and reuse. See `docs/_editor_complete_plan.md` Phase 4.
4. **6 failing lumen tests** (`lumen_m3/m4/m7/roundtrip`): genuine in-progress live-loop/snapshot
   milestone bugs (patch-on-2nd-event, child-component event routing, snapshot) — NOT warm-VM, NOT
   crashes. Same category as `aurora_pending_features`. Fix needs `_lumen_live_loop` domain work.

## 5. Build & test (try the NATIVE Linux path first — should be clean)
```bash
make USE_POSTGRES=0     # on Linux this should just work; no mingw/OpenSSL/winpty hacks needed
# run the fast warm-VM suite:
./vn test tests/ --timeout 6     # expect ~315 passed / 6 failed (the lumen live ones)
```
(If `make` needs deps: sqlite3, hiredis, libffi, libcurl, openssl, libtre/regex. On Windows these were
vendored under `C:/deps` + `deps/libtre.a`; on Linux use the distro packages.)

## 6. Delegation playbook (what works / what doesn't)
- **opencode / DeepSeek** (`~/.opencode/bin/opencode.exe run -m opencode-go/deepseek-v4-flash --agent build "..." < /dev/null`):
  great for bulk coding, BUT **hangs on `npm install` / `scoop install` / interactive steps**. Pre-install
  toolchains yourself; give it small, install-free tasks. Always review its diffs (it has hallucinated APIs,
  faked "identical" copies, committed dangling submodule gitlinks).
- **Claude subagents** (Agent tool, model `haiku`/`sonnet`): more reliable than DeepSeek, use tools properly,
  don't stall on installs. Use Sonnet for tricky work (grammars), Haiku for mechanical.
- **agy** (Antigravity CLI = Gemini): on **Windows** it needs a TTY → had to wrap in `winpty` (and it can't
  run backgrounded — no console). On **Linux + `ht`** this is clean: `ht` (andyk/ht) is a headless terminal
  that gives a real PTY, so `ht -- agy -p "..." --dangerously-skip-permissions` should drive Gemini reliably
  IF a Linux `agy` exists. `ht` is `cargo install`-able or has Linux release binaries. Model list:
  Gemini 3.5 Flash (Low/Med/High), Gemini 3.1 Pro (Low/High), Claude Sonnet/Opus 4.6, GPT-OSS 120B.
  (`--model "Gemini 3.5 Flash (Low)"` via CLI didn't take on Windows — the accepted token may differ from the
  display name; set the default model in the Antigravity app, or find the real id.)

## 7. In-repo references
- `docs/SCHEMA_SITUATION.md` — why `schema` must stay grammar-only (DO NOT re-add to lexer).
- `docs/CONSTELLATION.md`, `docs/KILN.md`, `docs/AURORA.md` — design docs.
- `docs/_grammar_task_brief.md` — exact grammar steps.
- `docs/_editor_complete_plan.md` — full editor+LSP plan (phased).
- `docs/TEST_INFRA_FINDINGS.md` — test runner + mocking analysis.
- `docs/_packaging_task.md` — Zed packaging fixes (mostly applied in `029243a`).
- Scratch briefs are the `docs/_*.md` files — safe to ignore/delete.

## 8. First moves when you resume on the VPS
1. `gh auth status` → confirm **Chidi09**.
2. `git fetch --all`; checkout `feat/editor-lsp-complete`.
3. Check the grammar SHA (TODO #1) and do the `extension.toml` rev bump if the new grammar is pushed.
4. `make USE_POSTGRES=0 && ./vn test tests/ --timeout 6` to confirm the baseline (≈315/6) builds clean on Linux.
5. Then proceed down §4 TODOs.

## 9. ⚠️ Skills & memory do NOT transfer to this VPS
The Windows box had an `opencode-delegate` skill and Claude memory under `~/.claude/` — those
are machine-local and are NOT on this VPS. So a fresh Claude session here won't auto-know them;
THIS doc is the carried-over context. Delegation options on the VPS:
- **Claude subagents** (the Agent tool, model `haiku`/`sonnet`) — work NATIVELY here, no setup.
  This is the most reliable delegate (DeepSeek/opencode kept hanging on installs). Prefer it.
- **opencode / DeepSeek** — must be installed + authed here first (provider `opencode-go`), then:
  `opencode run -m opencode-go/deepseek-v4-flash --agent build "<self-contained task>" < /dev/null`.
  Pre-install any toolchain yourself; give it small, install-free tasks; review every diff.
- **agy / Gemini** — needs a Linux Antigravity build + sign-in here; then drive cleanly with
  `ht` (Linux-native): `ht -- agy -p "<task>" --dangerously-skip-permissions`.
