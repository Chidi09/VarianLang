# TASK: Research LSP and Grammar Capabilities

We need to upgrade the Varian LSP and Grammar, but first we need a detailed summary of their current capabilities and a plan for improving the syntax highlighting colors.

## Objectives
1. **Analyze LSP Capabilities**: Read `src/lsp.c`. What features are currently implemented and functioning? (e.g., completions, hover, go to definition, diagnostics, formatting). Document exactly what the LSP can and cannot do currently.
2. **Analyze Grammar Capabilities**: Look into `editors/tree-sitter-varian/` (like `grammar.js`), and the editor extensions (like `editors/vscode/` or `editors/zed-varian/`). What language constructs are currently supported by the tree-sitter grammar? What is missing?
3. **Analyze Grammar Colors and Themes**:
   - Currently, `.lumen` (SFC) files and `.vn` (Varian source) files seem to be bundled with the same syntax highlighting colors.
   - We want to separate `.lumen` file highlighting from `.vn` file highlighting so they can be styled differently.
   - We want to add more diversity and a distinct aesthetic to Varian's syntax highlighting (e.g., how Rust makes heavy use of orange, we want Varian to have a unique, rich color identity).
4. **Deliverable**: Write a comprehensive report to `docs/lsp_grammar_report.md` that contains:
   - The detailed summary of current LSP capabilities.
   - The detailed summary of current Grammar capabilities.
   - A step-by-step implementation plan for upgrading the LSP/Grammar and implementing the distinct, diversified coloring scheme (separating Lumen and Varian, and giving Varian a unique color profile).

**CRITICAL RULE**: Do not spawn subtasks or parallel agents for this research. Do it sequentially in a single pass to avoid hanging.

Do NOT implement the changes yet. Just write the research report and the implementation plan.
