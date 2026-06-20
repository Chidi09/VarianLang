#ifndef LINT_H
#define LINT_H

#include "ast.h"
#include <stdbool.h>

typedef struct {
    const char *only_category;
    const char *format;
    bool had_error;
    int line_offset; /* lines contributed by a prepended vn_modules prelude */
    /* Optional diagnostic sink. When non-NULL, findings are delivered here
     * (line is already prelude-adjusted) instead of being printed to stdout --
     * this is how the LSP server (src/lsp.c) reuses the whole lint engine. */
    void (*sink)(void *ud, int line, int column, const char *category, const char *msg);
    void *sink_ud;
} LintContext;

int run_lint(const char *path, const char *only_category, const char *format);

/* Lint an in-memory buffer (the LSP's open-document text) rather than a file on
 * disk. Prepends the vn_modules prelude like run_lint does so Zenith literals
 * parse, reports the first syntax error (if any) and then all lint findings
 * through ctx->sink. `path` is used only for the vn_modules-membership check and
 * for diagnostic filenames. Returns 0 on success, 1 on syntax error. */
int lint_buffer(const char *user_source, const char *path, LintContext *ctx);

/* Get the cached vn_modules prelude (concatenated standard library). Returns
 * NULL if no vn_modules directory exists. *out_line_count is set to the number
 * of lines in the prelude. */
const char *lint_get_vn_prelude(int *out_line_count);

#endif
