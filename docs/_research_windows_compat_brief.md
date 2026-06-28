# TASK: Research Windows Compatibility Gaps

Please research what is needed to make Varian fully compatible with Windows. Currently, it is only "half-compatible" (Windows `vn.exe` is LSP-only, and `.vn` execution is limited or mostly review-only).

## Objectives
1. Analyze the current Windows build setup and C source files (look at `Makefile`, `src/main.c`, `src/main_win32.c`, and any networking/threading OS layers like in `src/lib_http.c` or others).
2. Check how an MSYS2 environment is currently required/used and what gaps prevent native or smooth execution on Windows.
3. Identify exactly what C code changes, polyfills, or `#ifdef _WIN32` blocks are missing to achieve full runtime execution parity with Linux/WSL.
4. Output a detailed but concise research report to `docs/windows_compat_report.md`. The report should include:
   - A summary of what currently blocks full Windows execution.
   - A clean, step-by-step plan on how to fix these gaps.
   - Do NOT implement the fixes yet, just produce the report.
