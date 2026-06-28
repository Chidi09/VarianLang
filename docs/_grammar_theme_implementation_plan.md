# Implementation Plan: Grammars and "Teal and Amber" Theme

We are executing Phase 2 and 3 of our grammar/theme upgrade, focusing on giving Varian a distinct aesthetic and supporting new language features contextually.

## CRITICAL RULE: The Contextual `schema` Rule
As detailed in `docs/SCHEMA_SITUATION.md`, the `schema` keyword MUST ONLY be treated as a keyword in the context of a declaration (e.g., `schema User { ... }`). In ALL other contexts (like `let schema = ...` or `schema.validate()`), it must remain a normal identifier.
- **DO NOT** add `schema` as a global keyword token in `tree-sitter-varian/grammar.js`. Instead, parse it contextually (e.g. using `alias('schema', $.keyword)` only inside a new `schema_definition` rule).
- **DO NOT** touch the C compiler (`src/lexer.c`, etc.). This is purely an editor grammar update.

## Tasks

### 1. Tree-Sitter Grammar (`editors/tree-sitter-varian/grammar.js`)
- Add a `schema_definition` rule (contextual `schema` keyword).
- Add a `dispatch_call` rule to distinguish method calls (`obj.method()`) from regular calls.
- Add `self` as a reserved keyword.

### 2. Separate Varian and Lumen Highlighting
- **Zed**: Update `editors/zed-varian/languages/Lumen/highlights.scm` to include specific captures for HTML tags, template directives (`@click`, `:bind`), and interpolations (`{expr}`). Keep `Varian/highlights.scm` focused on pure Varian.
- **VSCode**: Create `editors/vscode/syntaxes/lumen.tmLanguage.json` (wrapping Varian but adding HTML/template rules). Wire it up in `package.json`. Add the contextual `schema` regex to both grammars.

### 3. Implement the "Teal and Amber" Aesthetic
- We want to move away from generic red/blue themes to a distinct Varian identity.
- Update Zed's `themes/varian.json` (and create `lumen.json` if needed).
- **Keywords**: Amber/Orange (e.g., `#e6a020`).
- **Types/Namespaces**: Teal/Cyan (e.g., `#00b8c4`).
- **Functions/Methods**: Gold/Warm Amber (e.g., `#d4a043`).
- **Strings**: Green-teal (e.g., `#7ec8a0`).
- **Lumen**: Ensure HTML tags/directives have distinct colors from the embedded Varian script.

Execute these updates cleanly. Verify that `vn_modules/lumen.vn` and `tests/lumen_form_test.vn` are not broken by the `schema` highlighting changes.

## SAFETY WARNING
DO NOT run `make` or touch the `build/` directory under any circumstances. This grammar/theme work relies purely on JSON/SCM/JS data files and does not require compiling the `vn.exe` binary. Running `make` might wipe existing build state, which is unacceptable. Just edit the grammar files directly.
