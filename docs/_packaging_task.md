# Task: fix the Zed extension packaging (main repo)

Work in `C:/Users/X1/CHIDIS WORKSPACE/VarianLang` on the current branch
(`feat/editor-lsp-complete`). Scope is ONLY `editors/` packaging + `.gitignore`.

## Do NOT
- Do NOT touch `src/`, `vn_modules/`, `tests/`, or run `make`/build anything.
- Do NOT change the `[grammars.varian] rev` in `editors/zed-varian/extension.toml` (handled separately).
- Do NOT touch the `tree-sitter-varian` GitHub repo.

## Fixes (this is why `zed: install dev extension` fails today)
1. **Stop committing Zed's grammar build artifacts** — Zed clones/builds the grammar itself,
   so the repo must not contain them:
   - `git rm -r --cached editors/zed-varian/grammars/varian` (a dangling gitlink)
   - `git rm --cached editors/zed-varian/grammars/varian.wasm`
   - remove the empty on-disk dir: `rm -rf editors/zed-varian/grammars/varian`
   - add a line `editors/zed-varian/grammars/` to `.gitignore`
2. **Remove the dangling `tree-sitter-varian` gitlink** (no `.gitmodules`, breaks checkout):
   - `git rm --cached editors/tree-sitter-varian` ; `rm -rf editors/tree-sitter-varian`
3. **Fix the malformed companion manifest** `editors/zed-varian-lsp/extension.toml` — it fails to
   parse with: *"invalid type: map, expected a string at line 10 column 21"*. Open it, find the
   line ~10 where a value is a TOML table/map but should be a string (or vice-versa), and fix it
   to valid TOML. (Compare against `editors/zed-varian/extension.toml` for the correct shapes of
   `[grammars.*]` / `[language_servers.*]`.)
4. **Fix the icon theme** — Zed logged *"icon theme not found: Varian Dark"*. In
   `editors/zed-varian/extension.toml` and `editors/zed-varian/icon_themes/varian.json`, make the
   declared icon-theme name(s) match what's referenced (or remove the dangling reference so it
   doesn't error).

## Finish
```bash
git add -A
git commit -m "fix(zed): stop committing grammar build artifacts, remove dangling gitlinks, fix lsp manifest + icon theme"
```
Then print: exactly which files you removed-from-tracking / edited, and the fix you made to the
`zed-varian-lsp/extension.toml` line 10. Do not push.
