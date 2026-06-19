#ifndef LINT_H
#define LINT_H

#include "ast.h"
#include <stdbool.h>

typedef struct {
    const char *only_category;
    const char *format;
    bool had_error;
} LintContext;

int run_lint(const char *path, const char *only_category, const char *format);

#endif
