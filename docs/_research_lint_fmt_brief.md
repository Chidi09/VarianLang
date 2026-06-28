# TASK: Research Gaps in Linter and Formatter

The current linter (`src/lint.c`) and formatter (`src/fmt.c`) are behind the new implementations and language features recently added to the standard library modules (like `vn_modules/zenith.vn`, `vn_modules/lumen.vn`, etc.). 

## Objectives
1. Analyze the new patterns, syntax constructs, and features used in `vn_modules/zenith.vn`, `vn_modules/lumen.vn`, and other recent modules.
2. Compare these against the capabilities and rules in `src/lint.c` and `src/fmt.c`. 
3. Identify specific gaps: What new syntax does the formatter mangle or fail to format? What valid new patterns does the linter flag as errors (or what new rules should the linter enforce for the new features)?
4. Output a detailed but concise research report to `docs/lint_fmt_gaps_report.md`. The report should include:
   - A summary of the gaps in `fmt.c` and `lint.c`.
   - A clean, step-by-step implementation plan to update both C files to support the latest Varian codebase.
   - Do NOT implement the fixes yet, just produce the report.
