# Brief: Investigate the `schema` keyword situation and write a handoff doc

You are doing READ-ONLY research in the VarianLang repo (`C:/Users/X1/CHIDIS WORKSPACE/VarianLang`).
Do NOT change any code. Produce ONE output file: `docs/SCHEMA_SITUATION.md` (a clear report).
Do NOT run `make` or touch `build/`, `src/`, `deploy/`.

## Background (context you need)

Earlier, a `schema` KEYWORD was added to the compiler lexer; it broke things because `schema`
is also used as a normal identifier in many modules/tests, so it was REVERTED (removed from
the lexer keyword table). Meanwhile there may still be `schema`-declaration support in the
AST/parser/VM. A separate effort wants to add `schema` support to the EDITOR GRAMMARS
(tree-sitter / VSCode / Zed). We need to know precisely what is safe to (re-)add where.

## Questions to answer — with FILE:LINE evidence for each

1. **Compiler lexer** (`src/lexer.c`): Is `schema` currently a reserved keyword/token? Quote the
   keyword table area. Is there a `TOKEN_SCHEMA` enum value in `include/lexer.h` / `src/lexer.c`?
   Is it currently produced by the lexer or removed/commented out?

2. **Parser + AST** (`src/parser.c`, `include/*.h`, AST node types): Is there a `schema`
   declaration AST node (e.g. `NODE_SCHEMA`, `schema_decl`)? Is there parser code that builds it?
   Crucially: is that parser path REACHABLE without the `schema` lexer keyword (i.e. is it dead
   code now), or does it require `TOKEN_SCHEMA`?

3. **Compiler/VM** (`src/vm.c`): Is there schema handling (e.g. `schema_decl`,
   `__schema_metadata`, field types/decorators)? Quote it. Does anything at runtime depend on it?

4. **Identifier-collision scan** — THE KEY DATA: Count and list every place `schema` is used as
   a NORMAL IDENTIFIER (variable name, struct field, function/param name, map key, method) in
   `vn_modules/*.vn` and `tests/*.vn`. These are what BREAK if `schema` becomes a reserved word
   again. Give the count and a representative sample with file:line. Distinguish these from any
   actual `schema NAME { ... }` declaration usage (report how many of THOSE exist — likely zero).

5. **Editor grammars**: How do the grammars under `editors/` (tree-sitter-varian, VSCode
   textmate grammar, Zed) currently treat `schema`? Is it highlighted as a keyword anywhere? Do
   `.vn` and `.lumen` share the same grammar/theme files? List the relevant files.

## Then write `docs/SCHEMA_SITUATION.md` with these sections

- **Verdict (one paragraph):** current state of `schema` in compiler vs grammar.
- **What is SAFE to do** (for the editor/grammar effort): e.g. can `schema` be highlighted in
  tree-sitter/textmate WITHOUT reserving it in the compiler lexer? (It can — grammars are
  independent of the compiler.) Recommend CONTEXTUAL highlighting (only treat `schema` as a
  declaration keyword when it is followed by an identifier and `{`, NOT when used as a bare
  identifier) so normal identifier uses keep working.
- **What is NOT safe / will break:** re-adding `schema` as a reserved keyword in
  `src/lexer.c` — list exactly what breaks (the N identifier uses from question 4).
- **Concrete recommendation for the grammar effort (Gemini-facing):** a short, copy-pasteable
  set of do/don't bullets so the grammar work can proceed without breaking the compiler or the
  test suite.

Keep it factual and evidence-backed (file:line). This doc is a handoff so others don't have to
re-investigate.
