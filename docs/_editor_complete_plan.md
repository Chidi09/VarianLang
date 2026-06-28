# Plan: Complete Varian editor tooling (grammar + Lumen + theme + LSP) — one pass

Two repos, both owned by Chidi09:
- **Main repo**: `C:/Users/X1/CHIDIS WORKSPACE/VarianLang` (origin: github.com/Chidi09/VarianLang)
- **Grammar repo**: github.com/Chidi09/tree-sitter-varian (clone it fresh when needed)

`gh` is authenticated as **Chidi09**. At the START of every shell, run:
```bash
export PATH="$HOME/scoop/shims:$PATH"
```
so `gh`, `node`, `npm`, `tree-sitter` resolve.

## Hard guardrails (violating these is failure)
1. **NEVER add `schema` to the compiler lexer** (`src/lexer.c` keyword table). `schema` is a common identifier (`let schema = ...`) used ~18+ times — reserving it breaks the build/test suite. See `docs/SCHEMA_SITUATION.md`. `schema` highlighting must be **contextual in the grammar only** (`schema Name { ... }`), never a global keyword.
2. **NEVER run `make`** in the main repo (it wipes `build/*.o` via its clean step and breaks the working state). To build C, compile individual objects with `gcc -c` and link with the exact recipe in Phase 4.
3. Do NOT modify `vn_modules/`, `tests/`, or the warm-VM test runner. This task is editor tooling + LSP only.
4. Open a **PR** at the end — do NOT merge it. The orchestrator reviews.

## Branch
```bash
cd "/c/Users/X1/CHIDIS WORKSPACE/VarianLang"
git fetch origin
git checkout -b feat/editor-lsp-complete origin/main
```

## Phase 0 — Toolchain
```bash
node --version || scoop install nodejs
npm --version
tree-sitter --version || npm install -g tree-sitter-cli
```
Confirm all three print versions before continuing.

## Phase 1 — Grammar repo: contextual `schema`, `dispatch_call`, `self`, Lumen injection (CRITICAL)
Clone and work in the grammar repo:
```bash
cd /c/Users/X1/AppData/Local/Temp && rm -rf tsv && git clone https://github.com/Chidi09/tree-sitter-varian tsv && cd tsv
```
Edit `grammar.js`:
- Add a **contextual** `schema_definition` rule: `schema <identifier> { <fields> }` (a declaration). The literal `schema` token must ONLY be consumed in this declaration production — a bare `schema` elsewhere must still parse as an identifier (so `let schema = http.create_struct(...)` works). Use tree-sitter precedence/`token`/`field` as needed; do NOT add `schema` to the global keyword/identifier-exclusion set.
- Add a `dispatch_call` node (method call `obj.method(args)`) and `self` so the existing queries (`highlights.scm` references `(dispatch_call ...)`, `"self"`, `"schema"`) resolve to real nodes.
- Add HTML-template support for Lumen so `.lumen` `<template>…</template>` is parsed as HTML with embedded Varian in `<script>`/`{{ }}` (an injection or an embedded-HTML rule — your call; keep `.vn` grammar unchanged in behavior).

Validate the grammar against REAL files (must parse with NO ERROR nodes where `schema` is an identifier):
```bash
tree-sitter generate
# these MUST NOT error on `let schema = ...`:
tree-sitter parse "/c/Users/X1/CHIDIS WORKSPACE/VarianLang/vn_modules/lumen.vn" | grep -i error | head
tree-sitter parse "/c/Users/X1/CHIDIS WORKSPACE/VarianLang/tests/lumen_form_test.vn" | grep -i error | head
```
Commit (including regenerated `src/parser.c`, `src/grammar.json`, `src/node-types.json`) and push to `main` of tree-sitter-varian; capture the new commit SHA:
```bash
git add -A && git commit -m "grammar: contextual schema, dispatch_call, self, Lumen HTML injection"
git push origin main
NEWSHA=$(git rev-parse HEAD); echo "NEWSHA=$NEWSHA"
```

## Phase 2 — Fix the Zed extension packaging (CRITICAL — this is why install fails today)
In the main repo under `editors/zed-varian/`:
- The Zed install error is: *"grammar directory grammars/varian already exists, but is not a git clone"*. Zed clones the grammar itself — so the committed `grammars/varian` (a dangling gitlink) and `grammars/varian.wasm` (a build artifact) MUST NOT be in the repo. Remove them from git: `git rm -r --cached editors/zed-varian/grammars/varian editors/zed-varian/grammars/varian.wasm` (and delete the empty on-disk dir), and add `editors/zed-varian/grammars/` to `.gitignore`.
- Same for `editors/tree-sitter-varian` — it's a dangling gitlink with no `.gitmodules`. Either remove it from the repo (`git rm --cached editors/tree-sitter-varian`) or convert it to a proper submodule with a committed `.gitmodules`. Removing is simplest since the grammar lives in its own repo.
- Update `editors/zed-varian/extension.toml`: set `[grammars.varian] rev = "<NEWSHA from Phase 1>"`.
- Fix the other logged errors: `extension.toml` for the companion (`editors/zed-varian-lsp/extension.toml`) fails with *"invalid type: map, expected a string at line 10 column 21"* — fix that malformed TOML. And `"icon theme not found: Varian Dark"` — make the icon-theme name in `icon_themes/varian.json` match what's referenced, or remove the dangling reference.
- Ensure `languages/Varian/highlights.scm` and `languages/Lumen/highlights.scm` reference ONLY node types that now exist in the Phase-1 grammar (no "Invalid node type" — verify each `(node ...)` and `"anon"` against `src/node-types.json`).

Acceptance for Phases 1–2: a fresh `zed: install dev extension` on `editors/zed-varian` succeeds (grammar compiles, no query errors), `.vn` and `.lumen` highlight, and `schema` colors only in `schema Name { }` (not in `let schema = ...`). You can't drive Zed's UI headlessly, so instead PROVE it by: grammar `tree-sitter parse` clean (Phase 1) + every highlights.scm node verified present in `node-types.json`.

## Phase 3 — VSCode parity + Teal & Amber theme
- Ensure `.vn` → `varian.tmLanguage.json`, `.lumen` → `lumen.tmLanguage.json` (HTML + embedded Varian), both registered in `editors/vscode/package.json`.
- Confirm the "Teal & Amber" theme exists for both editors (Zed `themes/varian.json`, and a VSCode theme if not already) with the agreed palette: amber keywords, teal types/namespaces, gold functions, muted green-teal strings; `.lumen` template tokens (HTML tags, `@click`/`:bind` directives, `{{ }}`) visually distinct from Varian script.

## Phase 4 — LSP features in `src/lsp.c` (additive; do AFTER 1–3 are solid)
Implement, reusing one def-use/scope pass: **Find References**, **Rename**, **Signature Help**, **context-aware Completions** (best-effort: methods on known namespace structs / literals — Varian is dynamically typed, so heuristic), **Code Actions** (quick fixes for existing diagnostics), **Folding Ranges**. Register each capability in the server's initialize result and implement the handler.

Build to verify it COMPILES (do NOT run `make`):
```bash
cd "/c/Users/X1/CHIDIS WORKSPACE/VarianLang"
M=/c/Users/X1/scoop/apps/msys2/2026-06-11/msys64
export PATH="$M/mingw64/bin:$M/usr/bin:$PATH"
TD="/c/Users/X1/AppData/Local/Temp/vnbuild"; mkdir -p "$TD"; export TMP="$TD" TEMP="$TD" TMPDIR="$TD"
CF="-Wall -Wextra -std=gnu11 -g -Iinclude -D_POSIX_C_SOURCE=200809L -IC:/deps/include -D_WIN32_WINNT=0x0600 -DUSE_LOCAL_TRE_H -Ideps/tre-0.9.0/local_includes -IC:/deps -IC:/deps/hiredis/include -IC:/deps/libffi/include -DVN_NO_POSTGRES -DVN_NO_SMTP"
gcc $CF -c src/lsp.c -o build/lsp.o     # must compile with no errors
```
Then link (only if all objects present; otherwise leave linking to the orchestrator):
```bash
unset TMPDIR
OBJS=$(ls build/*.o | grep -vE 'main_win32.o|lib_postgres.o|lib_smtp.o' | tr '\n' ' ')
gcc $OBJS -o vn.exe -L"$M/mingw64/lib" -LC:/deps/lib deps/libtre.a -lm -lcurl -lcrypto -lssl -lpthread -Wl,-Bstatic -lsqlite3 -lhiredis -lffi -Wl,-Bdynamic -lws2_32 -lwinmm -lcrypt32 -Wl,--stack,268435456
```
If linking fails on OpenSSL, STOP and report — do NOT invent flags; the orchestrator finishes the link.

## Phase 5 — PR
```bash
cd "/c/Users/X1/CHIDIS WORKSPACE/VarianLang"
git add -A && git commit -m "feat(editor+lsp): working grammar (contextual schema, Lumen), themes, and LSP references/rename/signature-help/completions/code-actions/folding"
git push -u origin feat/editor-lsp-complete
gh pr create -R Chidi09/VarianLang --base main --head feat/editor-lsp-complete --title "Editor + LSP: working grammar, Lumen, Teal & Amber, and full LSP features" --body "Completes the editor tooling. Phases 1-3 make the Zed/VSCode extension actually install + highlight (grammar published to tree-sitter-varian @ <NEWSHA>, packaging fixed). Phase 4 adds LSP references/rename/signature-help/completions/code-actions/folding. See docs/_editor_complete_plan.md."
```

## Report at the end
- The tree-sitter-varian NEWSHA you pushed.
- `tree-sitter parse` results on the two real files (proving `schema` stays an identifier).
- Which packaging files you removed/fixed.
- Whether `build/lsp.o` compiled and whether the final link succeeded.
- The PR URL.
- Anything you could NOT finish (be honest — partial is fine, say what's left).
