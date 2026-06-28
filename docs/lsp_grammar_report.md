# LSP & Grammar Capabilities Report

## 1. LSP Capabilities

### Currently Implemented (src/lsp.c)

| Feature | LSP Method | Status | Details |
|---|---|---|---|
| **Text Sync** | `textDocument/didOpen`, `didChange`, `didSave` | ✅ Full | Stores docs in-memory (max 32). Full-content sync (kind=1). |
| **Diagnostics** | `textDocument/publishDiagnostics` | ✅ Full | Lint via `lint_buffer()` + Lumen cross-check of `<template>` bindings against `<script>` handlers. Severity: 1 (syntax) / 2 (warning). |
| **Hover** | `textDocument/hover` | ✅ Full | AST node-at-cursor lookup; native docs lookup (`native_docs.h`); user decl signature + docstring extraction; markdown responses. |
| **Completion** | `textDocument/completion` | ✅ Partial | Static keyword list (~60 entries). No context-aware completions. Trigger chars: `.`, `:`. |
| **Go to Definition** | `textDocument/definition` | ✅ Full | `find_decl()` searches top-level declarations by name. Skips prelude-internal definitions. |
| **Formatting** | `textDocument/formatting` | ✅ Full | Delegates to `fmt_format_source()`. Handles Lumen by extracting `<script>` region. |
| **Document Symbols** | `textDocument/documentSymbol` | ✅ Full | Top-level declarations only: fn, struct, schema, enum, actor, test. LSP SymbolKind mapping. |
| **Semantic Tokens** | `textDocument/semanticTokens/full` | ✅ Full | Lexer-based 2-pass approach. 11 token types: keyword, type, function, variable, parameter, property, number, string, operator, decorator, comment. Contextual classification of identifiers (function calls, decorators, properties, types from uppercase, variables otherwise). |

### NOT Implemented

| Feature | LSP Method | Missing? |
|---|---|---|
| **Find References** | `textDocument/references` | ❌ |
| **Rename** | `textDocument/rename` | ❌ |
| **Signature Help** | `textDocument/signatureHelp` | ❌ |
| **Code Actions** | `textDocument/codeAction` | ❌ |
| **Code Lens** | `textDocument/codeLens` | ❌ |
| **Folding Range** | `textDocument/foldingRange` | ❌ |
| **Selection Range** | `textDocument/selectionRange` | ❌ |
| **Workspace Symbols** | `workspace/symbol` | ❌ |
| **Inlay Hints** | `textDocument/inlayHint` | ❌ |
| **Diagnostic Related Info** | — | Related locations, code, and data not included. |
| **Incremental Sync** | — | Only full-document sync. |
| **Context-aware Completions** | — | Only a fixed keyword list. |
| **Multi-file / Module Analysis** | — | No module graph, no cross-file resolution. |
| **Type Info in Hover** | — | Only signatures + docstrings, not inferred types. |

---

## 2. Grammar Capabilities

### Tree-sitter Grammar (`editors/tree-sitter-varian/grammar.js`)

**Supported constructs by category:**

| Category | Constructs |
|---|---|
| **Comments** | `//` line comments, `/* */` block comments |
| **Top-level Declarations** | `use`, `fn` (with generics, async, pub, decorators), `struct` (with generics, decorators), `enum` (with generics, variant data), `actor`, `trait`, `impl`, `type` alias, `test`, `@ffi` declarations |
| **Statements** | `let`, `const`, `mut`, assignment (`=`, `+=`, `-=`, `*=`, `/=`), `return`, `break`, `continue`, `if/else`, `while`, `for/in`, `loop`, `match/case`, `try/catch`, `throw`, `assert`, `comptime`, blocks `{}` |
| **Expressions** | identifiers, numbers (hex, binary, octal, float, int), strings (double, single, interpolated), byte slices, regex, booleans, null, arrays, structs, enums, lambdas `\|x\| expr`, anonymous `fn`, unary (`!`, `not`, `-`, `~`), `await`, propagate `?`, optional chain `?.`, nil coalescing `??`, binary (all standard ops), channel send `<-`, range `..`, calls, member `.`, index `[]` |
| **Types** | primitives (`bool`, `int`, `float`, `string`, `byte`, `void`, `ptr`, `c_int`, `c_double`, `c_float`, `c_char`), type identifiers (uppercase), generics `<T>`, arrays `[T; n]`, function types `fn() -> T`, tuples `(T, U)` |
| **Other** | decorators `@name(...)`, field decorators, named arguments `name: value` |

**Missing / gaps:**

| Gap | Impact |
|---|---|
| **No `schema` declaration rule** | AST cannot represent schema types (Lumen uses these extensively). Falls through to error/unknown nodes. |
| **No `dispatch_call` rule** | Dispatch calls (`object.method()`) are parsed as `call_expression(member_expression(...))`, losing dispatch semantics. |
| **No Lumen template syntax** | The HTML `<template>`/`<script>` structure is invisible to tree-sitter. Only the `<script>` body is parsed. Highlighting the HTML parts relies on the LSP's `blank_lumen_html()` hack for diagnostics. |
| **No `self` keyword in grammar** | `self` is treated as a regular identifier rather than a keyword. |
| **Comment grammar is basic** | No doc-comment distinction (`///` vs `//`). |

---

## 3. Color / Theme Analysis

### Current State: Varian and Lumen Share Identical Highlighting

**VSCode (`editors/vscode/`):**
- `package.json` line 68-77: Both `varian` and `lumen` languages reference the **same** TextMate grammar (`source.varian` scope). Both produce identical scope names → identical colors.
- No per-language theming exists.

**Zed (`editors/zed-varian/`):**
- `languages/Varian/highlights.scm` and `languages/Lumen/highlights.scm` are **byte-for-byte identical**.
- Both languages share the **same** theme from `themes/varian.json`.
- No mechanism to assign different syntax colors per language.

### Current Zed Theme Color Palette ("Varian Dark")

| Highlight Token | Color | Hex |
|---|---|---|
| keyword, keyword.control | Red | `#ff7b72` |
| type, type.builtin | Purple | `#d2a8ff` |
| function, function.call, function.method, function.builtin | Blue | `#2b8fe0` |
| string, string.regex | Light Blue | `#a5d6ff` |
| number | Green | `#3fb950` |
| comment, comment.doc | Gray italic | `#7d8590` |
| variable | Foreground white | `#e6edf3` |
| variable.parameter | Light red | `#ffa198` |
| property | Blue | `#79c0ff` |
| constant, constant.builtin, boolean | Red | `#ff7b72` |
| attribute | Purple | `#bc8cff` |
| operator | Red | `#ff7b72` |
| namespace | Blue | `#58a6ff` |
| constructor | Teal | `#00b8c4` |
| punctuation, punctuation.bracket | Gray | `#7d8590` |
| enum | Purple | `#d2a8ff` |
| string.special | Teal | `#00b8c4` |
| variable.special | Teal | `#00b8c4` |

The dominant accent color is **teal/cyan** (`#00b8c4`), used for accents, active line numbers, links, constructors, etc.

### Observations for Distinct Varian Identity

- **Teal/cyan** (`#00b8c4`) is the accent color — similar to Elixir's philosophy, this could be pushed more.
- The palette is somewhat GitHub-dark inspired (predictable, safe).
- Varian's current identity is largely neutral with teal accents. There's room for a bolder identity (e.g. Rust = orange, Go = blue/teal, Varian could own a unique palette).

---

## 4. Implementation Plan

### Phase 1: LSP Upgrades

#### 1a. Add Missing Features (High Priority)
1. **`textDocument/references`** — Walk the AST to find all identifier references. Use `find_decl` as the base and search all identifier nodes. Return locations.
2. **`textDocument/rename`** — Similar to references but with `WorkspaceEdit` response. Rename across the current document (workspace rename needs module graph).
3. **`textDocument/signatureHelp`** — When cursor is inside a call expression, find the callee declaration and format its parameter list with active parameter highlighting.
4. **`textDocument/foldingRange`** — Use AST block/struct/enum/function ranges. Simple to implement.
5. **`textDocument/codeAction`** — Start with quick-fix for "Handler not defined" in Lumen. Add fix for missing semicolons, unused variables, etc.

#### 1b. Improve Existing Features (Medium Priority)
1. **Context-aware completions** — Instead of a static keyword list, use the current AST context to suggest relevant completions (e.g. suggest method names after `.`, suggest variable names in expressions).
2. **Type info in hover** — Use the type system (when available) to show inferred types alongside signatures.
3. **Incremental document sync** — Support `TextDocumentSyncKind.Incremental` (kind=2) to avoid re-sending the full document on every keystroke.
4. **Semantic tokens details** — Add `tokenModifiers` (e.g. `declaration`, `readonly`, `static`, `async`). Currently all token modifiers are 0.
5. **Multi-line token support** — The semantic token pass currently skips tokens with newlines. Implement line-splitting for multi-line tokens.

#### 1c. Cross-file / Workspace Features (Lower Priority)
1. **Module resolution** — Parse `use` statements and build a module graph. Enable cross-file go-to-definition and workspace symbols.
2. **Workspace diagnostics** — Re-lint all files when dependencies change.
3. **`workspace/symbol`** — Aggregate document symbols across all open files.

### Phase 2: Grammar Upgrades

#### 2a. Tree-sitter Grammar Improvements
1. **Add `schema_definition` rule** — Parse `schema Name { field: Type, ... }` declarations properly.
2. **Add `dispatch_call` node** — Distinguish dispatch calls (`obj.method()`) from regular calls for better semantic analysis.
3. **Add doc-comment syntax** — Optionally support `///` and `//!` as separate comment types for documentation generation.
4. **Add `self` keyword token** — Treat `self` as a keyword for better highlighting.
5. **Lumen template support** — Create a separate Lumen grammar that wraps the Varian grammar and adds `<template>`, `<style>`, and HTML-like tag parsing with interpolated expressions `{expr}`.

#### 2b. VSCode TextMate Grammar Improvements
1. **Create separate Lumen TextMate grammar** (`syntaxes/lumen.tmLanguage.json`) — Extend the Varian grammar with patterns for `<template>`, `</template>`, `<style>`, `</style>`, HTML tags, and Lumen-specific directives like `@click`, `@submit`, `:bind`, etc.
2. **Add interpreter string highlighting** — Highlight `{expr}` inside Lumen templates.
3. **Add more precise scope names** — Use more granular scopes for better theme integration.

### Phase 3: Distinct Coloring Scheme for Varian vs Lumen

#### 3a. Separate Highlights for Varian and Lumen (Zed)
1. **Modify `languages/Lumen/highlights.scm`** — Add Lumen-specific captures:
   - HTML tags → `@tag`
   - Template directives (`@click`, `@submit`, `:bind`, `:if`, `:for`) → `@keyword` or `@attribute`
   - Interpolation delimiters `{}` → `@punctuation.special` or `@embedded`
   - CSS/style content → `@string` (or defer to embedded CSS grammar)
2. **Keep `languages/Varian/highlights.scm`** as-is (pure Varian syntax).

#### 3b. Choose a Distinct Color Identity for Varian
**Proposed Varian color philosophy:** "Teal and Amber" — a warm/cool contrast.

| Token | Current Color | Proposed Varian Color | Hex |
|---|---|---|---|
| keyword | Red | **Amber/Orange** | `#e6a020` |
| type | Purple | **Teal/Cyan** | `#00b8c4` |
| function | Blue | **Gold/Amber** | `#d4a043` |
| string | Light Blue | **Green-teal** | `#7ec8a0` |
| number | Green | **Amber** | `#c9a02e` |
| variable | Foreground | Foreground (unchanged) | `#e6edf3` |
| constant/bool | Red | **Amber** | `#e6a020` |
| operator | Red | **Teal** | `#00b8c4` |
| attribute | Purple | **Purple** (unchanged) | `#bc8cff` |
| namespace | Blue | **Teal** | `#00b8c4` |
| property | Blue | **Light teal** | `#6cc6d0` |
| parameter | Light red | **Light amber** | `#e8b84a` |
| comment | Gray italic | Gray italic (unchanged) | `#7d8590` |

The **teal** (`#00b8c4`) stays as the accent color, but **amber/orange** replaces red for keywords, creating a warmer, more distinctive look that sets Varian apart from the typical red-keyword languages.

#### 3c. Separate Theme Entries for Lumen (Zed)
1. **Create `themes/lumen.json`** — A variant of the Varian theme with:
   - Different syntax colors for Lumen-specific constructs
   - Possibly a lighter/different background tint for Lumen files
   - HTML/template-specific token colors

2. **Register Lumen theme in `extension.toml`** — Add `themes = ["themes/varian.json", "themes/lumen.json"]`.

#### 3d. VSCode: Separate TextMate Grammars for Varian vs Lumen
1. **Create `syntaxes/varian.tmLanguage.json`** — Varian-only syntax (as current).
2. **Create `syntaxes/lumen.tmLanguage.json`** — Lumen syntax that includes the Varian grammar but adds HTML template patterns.
3. **Update `package.json`** — Wire `lumen` language to `syntaxes/lumen.tmLanguage.json` and `varian` to `syntaxes/varian.tmLanguage.json`.

#### 3e. VSCode: Add Per-Language Theming Support
1. In VSCode, textual scope colors are driven by the theme. Users will need to install a Varian-specific theme or configure their settings:
   ```json
   "editor.tokenColorCustomizations": {
     "[Your Theme]": {
       "textMateRules": [
         { "scope": "keyword.varian", "settings": { "foreground": "#e6a020" } },
         { "scope": "keyword.lumen", "settings": { "foreground": "#c77d2f" } }
       ]
     }
   }
   ```
2. Ship a built-in VSCode theme extension that registers a "Varian Dark" / "Varian Light" color theme with distinct Varian and Lumen coloring.

### Phase 4: Summary of Deliverables

| Deliverable | Files to Create/Modify |
|---|---|
| References LSP feature | `src/lsp.c` (add `handle_references`) |
| Rename LSP feature | `src/lsp.c` (add `handle_rename`) |
| Signature Help LSP feature | `src/lsp.c` (add `handle_signature_help`) |
| Folding Range LSP feature | `src/lsp.c` (add `handle_folding_range`) |
| Context-aware completions | `src/lsp.c` (rewrite `handle_completion`) |
| Schema declaration rule | `editors/tree-sitter-varian/grammar.js` |
| Dispatch call node | `editors/tree-sitter-varian/grammar.js` |
| Doc comment syntax | `editors/tree-sitter-varian/grammar.js` |
| Lumen tree-sitter grammar | `editors/tree-sitter-varian/lumen-grammar.js` (new) |
| Lumen highlights.scm | `editors/zed-varian/languages/Lumen/highlights.scm` (modify) |
| Varian highlights.scm | `editors/zed-varian/languages/Varian/highlights.scm` (verify unchanged) |
| Lumen theme | `editors/zed-varian/themes/lumen.json` (new) |
| VSCode Lumen TextMate grammar | `editors/vscode/syntaxes/lumen.tmLanguage.json` (new) |
| VSCode package.json update | `editors/vscode/package.json` (wire separate grammars) |
| VSCode color theme extension | `editors/vscode/themes/varian-color-theme.json` (new) |

### Priority Order

1. **Phase 1a** (Missing LSP features — References, Rename, SignatureHelp) — Highest user impact.
2. **Phase 2a** (Schema rule, dispatch call) — Needed before accurate semantic analysis.
3. **Phase 3** (Separate Varian/Lumen highlighting) — User-visible improvement, distinct identity.
4. **Phase 1b** (Improve existing LSP features) — Polish.
5. **Phase 2b** (VSCode TextMate grammar improvements) — Together with Phase 3d/3e.
6. **Phase 1c** (Cross-file/workspace features) — Longest lead time, lowest urgency.
