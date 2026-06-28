# Schema Keyword Situation Report

## Verdict

`schema` is a **declaration keyword in the parser/AST/VM/linter/formatter/LSP** but is **NOT a lexer keyword** — the keyword `"schema"` was removed from `src/lexer.c`'s keyword table because it broke ~18 identifier uses across `vn_modules/`, `tests/`, and `examples/`. The parser path (`parse_schema_decl`) is **dead code** today since the lexer never produces `TOKEN_SCHEMA`. Editor grammars (tree-sitter, VSCode, Zed) have **zero awareness** of `schema` — none highlight it as a keyword. This means the grammar effort can safely add `schema` highlighting without touching the compiler, but must use **contextual highlighting** (keyword only when followed by `Identifier {`) to avoid breaking real code that uses `schema` as a variable/parameter name.

---

## 1. Compiler Lexer

**`include/lexer.h:69`** — The `TOKEN_SCHEMA` enum value still exists:

```c
TOKEN_STRUCT, TOKEN_SCHEMA, TOKEN_ENUM, TOKEN_ACTOR,
```

**`src/lexer.c:75`** — The token name string table still has a `TOKEN_SCHEMA` case.

**`src/lexer.c:129-133`** — The keyword `"schema"` is **intentionally absent** from the keyword table, with this comment:

```c
/* NOTE: `schema` is intentionally NOT a keyword. It is a very common
 * identifier (e.g. `lumen_form(schema)`, `lumen_search(req, schema)`) across
 * the stdlib and user code. Reserving it (added then reverted) broke those
 * uses and the test suite. The TOKEN_SCHEMA enum + parser branch remain for
 * a future, non-colliding schema-declaration syntax. */
```

The lexer never produces `TOKEN_SCHEMA` — `"schema"` input falls through to `TOKEN_IDENTIFIER`.

---

## 2. Parser + AST

**`include/ast.h:99`** — `NODE_SCHEMA_DECL` enum value exists.

**`include/ast.h:344-357`** — Full `schema_decl` union member with `field_types`, decorators, etc.

**`src/ast.c:446-511`** — Full `ast_schema_decl()` constructor implementation.

**`src/parser.c:1017-1170`** — Complete `parse_schema_decl()` function (310 lines).

**`src/parser.c:1814-1816`** — Dispatch call:

```c
if (match(parser, TOKEN_SCHEMA)) {
    return parse_schema_decl(parser);
}
```

This code path is **unreachable** (dead code) because `TOKEN_SCHEMA` is never produced by the lexer.

---

## 3. Compiler/VM

**`src/vm.c:2463-2559`** — Full `case NODE_SCHEMA_DECL:` handler (~100 lines) that:
1. Emits `BC_REGISTER_VALIDATIONS` for decorators
2. Emits a `__schema_metadata_Name` global struct with field types and decorators
3. Emits `BC_STRUCT` for the schema type

This is also **dead code** (unreachable) since `NODE_SCHEMA_DECL` AST nodes are never created.

**`src/lint.c:690-714`** — Linter handles `NODE_SCHEMA_DECL` (duplicate fields, unknown decorators). Dead code.

**`src/lsp.c:754-756, 815-816, 1075-1076, 1439, 1522-1523`** — LSP handles `NODE_SCHEMA_DECL` (hover info, go-to-definition, completions). Dead code.

**`src/fmt.c:285`** — Formatter keyword table includes `{"schema", true}` (treated as a declaration keyword for formatting purposes).

**`src/fmt.c:304`** — `"schema"` is in the `fmt_is_opening_brace_keyword` list.

---

## 4. Identifier-Collision Scan

### vn_modules/*.vn — 12 uses of `schema` as a normal identifier (would break)

| File | Line | Usage |
|------|------|-------|
| `vn_modules/lumen.vn` | 2250 | `let schema = http.create_struct(...)` — variable name |
| `vn_modules/lumen.vn` | 2251 | `lumen_form(schema)` — variable reference |
| `vn_modules/lumen.vn` | 2267 | `_validate.get_field(schema, name)` — variable reference |
| `vn_modules/lumen.vn` | 2407 | `let schema = http.create_struct(...)` — variable name |
| `vn_modules/lumen.vn` | 2408 | `lumen_form(schema)` — variable reference |
| `vn_modules/lumen.vn` | 2626 | `fn lumen_search(req, schema)` — parameter name |
| `vn_modules/lumen.vn` | 2630 | `_validate.get_keys(schema)` — variable reference |
| `vn_modules/lumen.vn` | 2636 | `_validate.get_field(schema, key)` — variable reference |
| `vn_modules/zenith.vn` | 875 | `fn _json_content(schema)` — parameter name |
| `vn_modules/zenith.vn` | 878 | `[http.create_struct(["schema"], [schema])]` — variable reference |
| `vn_modules/zenith.vn` | 882 | `fn _response_with_schema(desc, schema)` — parameter name |
| `vn_modules/zenith.vn` | 885 | `_json_content(schema)` — variable reference |

### tests/*.vn — 2 uses of `schema` as a normal identifier (would break)

| File | Line | Usage |
|------|------|-------|
| `tests/lumen_form_test.vn` | 2 | `let schema = http.create_struct(...)` — variable name |
| `tests/lumen_form_test.vn` | 3 | `lumen_form(schema)` — variable reference |

### examples/*.vn — 4 uses of `schema` as a normal identifier (would break)

| File | Line | Usage |
|------|------|-------|
| `examples/validate_test.vn` | 3 | `let schema = validate.object(...)` — variable name |
| `examples/validate_test.vn` | 17 | `schema.parse(valid_input)` — variable reference |
| `examples/validate_test.vn` | 30 | `schema.parse(invalid_input1)` — variable reference |
| `examples/validate_test.vn` | 39 | `schema.parse(invalid_input2)` — variable reference |

**Total: 18 identifier uses** across the codebase that would break if `schema` were re-added as a lexer keyword.

### Non-breaking occurrences

- `vn_modules/seo.vn` lines 131, 135, 154, 169, 175, 194, 213, 220, 231, 251, 258, 264, 279, 290: `"http://schema.org"` / `"https://schema.org"` — inside **string literals**, not identifiers. Would NOT break.
- `vn_modules/zenith.vn:954`: `["schema"]` — inside a **string literal**. Would NOT break.
- `vn_modules/migration.vn:1`: comment only. Would NOT break.

### Actual `schema` declaration usage in .vn files

**Zero.** No `.vn` file uses `schema MyType { ... }` as a declaration. The entire parser/VM/AST schema-decl code path is dead.

---

## 5. Editor Grammars

### tree-sitter-varian (`editors/tree-sitter-varian/`)

- **`grammar.js`** — No `schema` grammar rule. The `_declaration` rule lists `struct`, `enum`, `actor`, `trait`, `impl`, `type`, `test`, `ffi` — but not `schema`. Keyword lists do not include `"schema"`.
- **`queries/highlights.scm:103`** — Only `register_schema` is matched as `@function.builtin` (a Zenith framework method). No standalone `schema` keyword highlighting.
- **`tree-sitter.json:9`** — File-types: `["vn", "vhtml", "lumen"]` — all three share this grammar.

### VSCode Extension (`editors/vscode/`)

- **`syntaxes/varian.tmLanguage.json:66-68`** — Keyword patterns list `struct|enum|actor|impl|trait|type|use|pub|mut|test|comptime` etc. — **no `schema`**.
- **`package.json:42,58,67-77`** — `.vn` registered to `"varian"` language, `.lumen` registered to `"lumen"` language. Both use the **same** grammar file (`./syntaxes/varian.tmLanguage.json`) with the **same** scope name (`source.varian`).

### Zed Extension (`editors/zed-varian/`)

- **`languages/Varian/highlights.scm`**, **`languages/Lumen/highlights.scm`** — Byte-identical copies of `tree-sitter-varian/queries/highlights.scm`. Only `register_schema` appears.
- **`languages/Varian/config.toml:2`** — `grammar = "varian"`, `path_suffixes = ["vn", "vhtml"]`.
- **`languages/Lumen/config.toml:2`** — `grammar = "varian"`, `path_suffixes = ["lumen"]`. Shares the same grammar.
- **`grammars/varian/grammar.js`** — Byte-identical to `tree-sitter-varian/grammar.js`. No schema support.
- **`grammars/varian/queries/highlights.scm`** — Shorter 97-line version. **No `schema` mention at all** (not even `register_schema`).

### Summary: `.vn` and `.lumen` share grammars

| File | Shared? |
|------|---------|
| `tree-sitter-varian/grammar.js` | YES — `tree-sitter.json` file-types: `["vn", "vhtml", "lumen"]` |
| `vscode/syntaxes/varian.tmLanguage.json` | YES — both `varian` and `lumen` languages reference it |
| `zed-varian/grammars/varian/grammar.js` | YES — identical copy, used by both Varian and Lumen |
| Highlights/queries `.scm` files | YES — byte-identical across Varian and Lumen |

---

## What is SAFE to do

1. **Add `schema` as a keyword/highlight in tree-sitter grammar, VSCode TextMate grammar, and Zed highlights.scm** — editor grammars are independent of the compiler. The compiler lexer doesn't need to change.

2. **Use CONTEXTUAL highlighting**: highlight `schema` as a keyword **only when** it appears at declaration position: `schema Identifier { ... }`. In tree-sitter, this means adding a grammar rule like:
   ```js
   schema_definition: $ => seq('schema', $.identifier, $.type_annotation, $.struct_body)
   ```
   And only highlighting it when matched as a `schema_definition` node (not as a bare identifier).

3. **The `TOKEN_SCHEMA` enum, `parse_schema_decl`, `NODE_SCHEMA_DECL`, and the VM handler can remain as dead code** for a future non-colliding syntax (e.g. `schema Name { ... }` without conflicting with existing identifier uses).

4. **Grammars can be tested independently** — add `schema` keyword highlighting in the grammars and run tree-sitter tests without touching the compiler.

---

## What will BREAK (NOT safe)

1. **Re-adding `"schema"` to `src/lexer.c`'s keyword table** — this will break **18 identifier uses** in `vn_modules/`, `tests/`, and `examples/` (listed in section 4 above). The test suite will fail because `let schema = ...` would become `let TOKEN_SCHEMA = ...` which is a parse error.

2. **Making `schema` a soft keyword/reserved word in the lexer** — even a soft keyword that prevents using `schema` at declaration start position contextually would be tricky because the parser currently matches `TOKEN_SCHEMA` on line 1814 of `parser.c`. If the lexer ever produces `TOKEN_SCHEMA`, the parser will consume it and try to parse a schema declaration, which will fail on normal identifier usage.

3. **Any approach that changes `check_keyword` in `lexer.c` to return `TOKEN_SCHEMA` for the string `"schema"`** — same immediate breakage.

---

## Concrete Recommendation for Grammar Effort

### DO

- **DO** add `schema` as a keyword in tree-sitter grammar, but only as part of a `schema_definition` production rule (contextual, not a bare keyword token). Follow the pattern `'schema' field: $ => token('schema')` used for other declaration keywords.
- **DO** add `schema` to the VSCode TextMate `keyword.declaration.varian` pattern group.
- **DO** add `"schema"` to the Zed highlights.scm keyword list.
- **DO** test the grammar on these files to confirm contextual highlighting works:
  - `vn_modules/lumen.vn` (lines ~2250, ~2407, ~2626) — `schema` used as variable name
  - `tests/lumen_form_test.vn` (lines 2-3) — `schema` used as variable name
  - `examples/validate_test.vn` (lines 3, 17, 30, 39) — `schema` used as variable name
- **DO** keep `.vn` and `.lumen` grammars in sync — they share the same grammar file(s).

### DON'T

- **DON'T** add `"schema"` to `src/lexer.c`'s keyword table — that breaks 18 identifier uses and the test suite.
- **DON'T** add a bare `"schema"` keyword token to the tree-sitter grammar — this would cause parse errors on every file that uses `schema` as a variable name.
- **DON'T** rely on the compiler's existing `TOKEN_SCHEMA` / `parse_schema_decl` for the grammar effort — those are dead code and unreachable.
- **DON'T** remove the `TOKEN_SCHEMA` enum or `parse_schema_decl` — they are reserved for a future non-colliding schema syntax.
