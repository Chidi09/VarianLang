#ifndef SEMANTIC_H
#define SEMANTIC_H

#include "ast.h"
#include <stdbool.h>

/* Category/code constants for semantic diagnostics */
#define SEM_CODE_DUP_DECL         "duplicate-declaration"
#define SEM_CODE_DUP_PARAM        "duplicate-param"
#define SEM_CODE_UNDEF_ID         "undefined-identifier"
#define SEM_CODE_ASSIGN_CONST     "assign-const"
#define SEM_CODE_BREAK_OUTSIDE    "break-outside-loop"
#define SEM_CODE_CONTINUE_OUTSIDE "continue-outside-loop"
#define SEM_CODE_RETURN_OUTSIDE   "return-outside-fn"
#define SEM_CODE_ARITY_MISMATCH   "arity-mismatch"
#define SEM_CODE_TYPE_MISMATCH    "type-mismatch"

typedef struct {
    char *filename;
    int line;
    int column;
    int offset;
    char code[64];
    char message[256];
} SemanticDiagnostic;

typedef struct {
    SemanticDiagnostic *diagnostics;
    int count;
    int capacity;
    bool had_error;
} SemanticResult;

/* Run semantic analysis on an AST program node.
 * Returns a SemanticResult containing all gathered diagnostics.
 * The caller is responsible for freeing the result with semantic_result_free. */
SemanticResult *semantic_analyze(AstNode *program);

/* Free a SemanticResult structure and all allocated diagnostics. */
void semantic_result_free(SemanticResult *result);

#endif /* SEMANTIC_H */
