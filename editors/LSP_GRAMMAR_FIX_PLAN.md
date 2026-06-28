# Varian LSP & Grammar — Fix Plan

Diagnosis date: 2026-06-27. Target editor: **Zed**.

## Root causes of "no colour" in Zed

1. **(PRIMARY) Zed had no highlight query.** Zed loads syntax queries from the
   extension's `languages/<Name>/highlights.scm`, **not** from the grammar repo's
   `queries/`. `editors/zed-varian/languages/Varian/` and `Lumen/` contained only
   `config.toml`. This regressed when the extension moved from `editors/zed/`
   (which had a `highlights.scm`, now deleted) to `editors/zed-varian/`.
   → **FIXED**: added `editors/zed-varian/languages/Varian/highlights.scm`.

2. **The grammar-repo `queries/highlights.scm` is broken** — it references nodes,
   fields, and tokens that do not exist in the compiled grammar. In tree-sitter a
   single unknown reference rejects the WHOLE query (zero highlighting). Offenders:
   - `(escape_sequence)` — node does not exist
   - `"?:"` — token does not exist (grammar has `"?."`)
   - `"async"`, `"await"`, `"as"` — not in the grammar
   - `"use"`, `"null"` as bare strings — not anonymous tokens (`null` is a node)
   - `enum_literal name:` — the field is `type:`, not `name:`
   - `(true)`/`(false)` are NOT nodes — they are tokens inside `(boolean)`

3. **`use "..."` imports don't parse.** `use_statement` is defined in `grammar.js`
   but is not referenced by `_declaration` or `statement`, so it is pruned. Any
   file that begins with `use` (most real apps) produces an ERROR node at line 1
   that cascades and breaks highlighting for the rest of the file.

4. **No `async`/`await` in the grammar** even though the AST/LSP support `async fn`.

## Status

- [x] **Most important:** corrected `highlights.scm` for Zed (Varian).
- [ ] Propagate corrected query to: `languages/Lumen/highlights.scm`,
      `editors/tree-sitter-varian/queries/highlights.scm`,
      `editors/zed-varian/grammars/varian/queries/highlights.scm`.
- [ ] Verify every reference against `node-types.json` (mechanical — Haiku agent).

## Remaining work (needs a toolchain — `tree-sitter` CLI / Node, currently NOT installed)

### A. Grammar fixes (`editors/tree-sitter-varian/grammar.js`) — SOURCE EDITS DONE ✅
1. [x] Added `use_statement` to the `_declaration` choice (top-level, matching
       parser.c:2539 which parses `use` in the program loop).
2. [x] Fixed `use_statement` to `seq("use", $.string, optional(";"))` — real code
       has NO trailing semicolon (e.g. `examples/main.vn:1`), the old rule required one.
3. [x] Added `optional("async")` to `function_definition` and `anonymous_function`
       (lexer reserves `async`, lib/AST expect `async fn`).
4. [x] Added `await_expression: prec.right(11, seq("await", $._expression))` and
       threaded it into `_expression` (matches parser.c:1977 — `await` is a unary prefix).
5. [x] Synced the second copy `editors/zed-varian/grammars/varian/grammar.js` to be
       byte-identical (it was an older snapshot missing the `field(...)` wrappers).
6. [x] **Regenerated** with `tree-sitter generate` (tree-sitter 0.26.9, Node v20.19.6
       via nvm; gcc 16.1.0 via scoop at
       `C:\Users\X1\scoop\apps\mingw\16.1.0-rt_v14-rev1\bin`). `use_statement`,
       `await_expression`, and the `async` token are now in `src/node-types.json`.
       No conflict on `await_expression` — conflicts array untouched.
   - Validated by parsing a snippet: `use "..."` (no semicolon), `async fn`,
     anonymous `async fn`, and `await call(...)` all parse with ZERO error nodes.
7. [ ] Decide on `as` (cast) — add a rule if the language supports it, else leave out.

> **Toolchain note for future runs:** the tools live under nvm/scoop and are NOT on
> the Bash tool's PATH (use PowerShell). To build the C test parser, prepend gcc:
> `$env:Path = "C:\Users\X1\scoop\apps\mingw\16.1.0-rt_v14-rev1\bin;" + $env:Path; $env:CC = "gcc"`.

### A.1 Pre-existing corpus failures (NOT caused by the edits — separate cleanup)
`tree-sitter test` shows 5 failures, all stale corpus / grammar-feature gaps:
- `variables.txt`: grammar `let_declaration` allows only ONE name; `let y, z = 20` fails.
- `enums.txt`: corpus expects `enum_literal` via `.`; grammar uses `::`.
- `structs.txt`: corpus expects impl methods as `function_definition`; grammar inlines them.
- `interpolation.txt`: a `{}`-less string parses as `string`, corpus expects `interpolated_string`.
- `actors.txt`: corpus predates the current actor-field representation.
Fix by updating the corpus `.txt` files (or the grammar) — out of scope for this task.

> ⚠️ **Do NOT add `"async"`, `"await"`, `(use_statement)`, or `(await_expression)` to
> `highlights.scm` until step A.6 (regenerate) is done.** The current compiled grammar
> lacks those nodes; referencing them now would re-break the whole query. After regen,
> add: `"async"` → `@keyword`, `"await"` → `@keyword.control`, and optionally a `use`
> path string capture.

### B. Rebuild & ship the grammar to Zed
1. Rebuild `editors/zed-varian/grammars/varian.wasm`
   (`tree-sitter build --wasm` or Zed's grammar build).
2. **De-duplicate grammars.** There are two copies
   (`editors/tree-sitter-varian/` and `editors/zed-varian/grammars/varian/`).
   Make one canonical; ideally a git submodule.
3. [x] **DONE** — committed the regenerated grammar to `tree-sitter-varian` and
   pushed to `origin/main` (commit `b293120fe207e5f3e4aa5227f4319033c1a507b2`).
   Bumped `editors/zed-varian/extension.toml` `[grammars.varian] rev` to that SHA
   (verified it matches `origin/main`). Zed will now fetch the updated grammar.
   Note: the wasm is `.gitignore`d in the grammar repo (Zed builds it from source).

### C. Optional Zed query files (nice-to-have, big UX win)
- `languages/Varian/outline.scm` — symbol outline / breadcrumbs.
  - [x] DONE: outline.scm + brackets.scm added to languages/Varian and languages/Lumen (validated against the grammar).
- `languages/Varian/brackets.scm`, `indents.scm`, `injections.scm`
  (e.g. inject `regex` into `regex_literal`, HTML into Lumen templates).

### D. LSP enhancements (`src/lsp.c`) — in priority order
1. **`semanticTokensProvider`** — biggest resilience win; colours code even if
   tree-sitter is unavailable. Reuse the existing AST walker (`find_node_at`).
2. **Context-aware completion** — `handle_completion` currently ignores the
   document (`(void)json; (void)uri;`, lsp.c:1250) and returns a static keyword
   list with a fragile index-based kind hack. Parse the doc, offer user symbols,
   module members, and imports.
3. `textDocument/references` and `rename`.
4. `signatureHelp` (trigger `(` and `,`).
5. Hierarchical `documentSymbol` (methods under impl/actor, struct fields).
6. Scope-aware hover/definition (locals, params, struct fields, methods, cross-file)
   — today `find_decl` only resolves top-level declarations.

## How to test after each change
1. In Zed: `zed: install dev extension` → pick `editors/zed-varian`.
2. Open an example `.vn` file → expect colour.
3. `zed: open log` to see any "error parsing highlights query …" messages.
4. Validate queries offline once the CLI exists:
   `tree-sitter query editors/.../highlights.scm <file.vn>`.
