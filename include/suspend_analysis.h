#ifndef SUSPEND_ANALYSIS_H
#define SUSPEND_ANALYSIS_H

#include "ast.h"
#include <stdbool.h>

/* One entry per function declaration found anywhere in the program (top-level
 * or nested/local). Keyed by AST node identity, not name, since nested
 * function declarations can shadow each other. */
typedef struct {
    AstNode *fn_node;
    const char *name; /* for diagnostics only; lookups are by fn_node identity */
    bool suspends;    /* monotonic: starts false (optimistic), only ever flips true */
} SuspendFunc;

struct SuspendAnalysis {
    SuspendFunc *functions;
    int count;
    int capacity;
    bool changed; /* fixpoint bookkeeping, valid only during suspend_analyze() */
};
typedef struct SuspendAnalysis SuspendAnalysis; /* tag must match vm.h's forward declaration */

/* Conservatively determines, for every function declaration (top-level or
 * nested) in `program`, whether executing it might ever reach a suspending
 * operation: an `await`, a channel send/receive, or a call that can't be
 * statically resolved to a single already-analyzed non-suspending function.
 *
 * This gates Kiln's native/typed codegen: only functions proven here to never
 * suspend are eligible for it, because the scheduler's resumption contract
 * (task_run re-entering an AOT function's resumption switch at a recorded
 * bytecode offset) requires all live state at a suspension point to already
 * be sitting in boxed Values on the task stack. A function that might
 * suspend keeps today's per-instruction transpile path, unchanged.
 *
 * The analysis is deliberately over-approximating: any call it cannot
 * statically resolve to one known function (dynamic dispatch, a call through
 * a variable, an unresolved name) is conservatively treated as suspending.
 * That can only cause a function to be excluded from native codegen that
 * could have safely used it — it can never mark a truly-suspending function
 * as safe. */
SuspendAnalysis *suspend_analyze(AstNode *program);
void suspend_analysis_free(SuspendAnalysis *sa);

/* Looks up whether a specific fn_decl node was found to (maybe) suspend.
 * Returns true (fail-safe) if the node isn't present in the analysis. */
bool suspend_analysis_get(SuspendAnalysis *sa, AstNode *fn_node);

#endif
