#ifndef SSA_H
#define SSA_H

#include "ast.h"
#include "suspend_analysis.h"
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

/* Typed SSA IR for Kiln's native release backend.
 *
 * Built directly from the AST (not bytecode) for every function proven by
 * suspend_analyze() to never suspend — suspending functions never get an SSA
 * form at all; they keep today's per-instruction transpile path forever (see
 * suspend_analysis.h for why).
 *
 * Construction follows Braun, Buchwald, Hack, Leissa, Mallon, Zwinkau,
 * "Simple and Efficient Construction of Static Single Assignment Form"
 * (CC'13): SSA is built directly during a single forward walk of the
 * AST/CFG, using "incomplete phi" placeholders in not-yet-sealed blocks
 * (loop headers, whose back-edge predecessor isn't known until the loop body
 * has been walked) that get resolved once the block is sealed (all
 * predecessors known).
 *
 * This phase is purely additive: it produces an SSA module for diagnostic
 * dumping (--dump-ssa) only. Nothing here changes what Kiln emits yet — type
 * inference (ssa_infer_types, next), escape analysis, and the native codegen
 * backend that actually consumes this IR are later phases. */

typedef enum {
    SSA_INT,
    SSA_FLOAT,
    SSA_BOOL,
    SSA_STRING,
    SSA_STRUCT,   /* struct_type_name meaningful only when this and proven monomorphic */
    SSA_UNKNOWN,  /* must stay dynamic/boxed — the safe, final "can't be typed" bottom state */
    /* Internal bookkeeping for ssa_infer_types' fixpoint only ("haven't seen
     * enough evidence yet, still optimistically anything") — every value is
     * reset to this at the start of inference and it is NEVER present in any
     * value's `.type` once ssa_infer_types returns (a defensive final sweep
     * converts any leftover PENDING — genuinely unreachable/dead values — to
     * UNKNOWN). Needed to correctly type loop-carried accumulators and
     * (mutually) recursive return types: a naive "only grow from UNKNOWN"
     * pass can never bootstrap a value whose only path to a concrete type
     * routes back through itself (e.g. `total = total + i` in a loop, or
     * naive recursive fibonacci) — this third, optimistic-top state is what
     * makes that possible, safely, via the standard technique used by
     * SCCP-style fixpoint algorithms. */
    SSA_TYPE_PENDING,
} SsaType;

typedef enum {
    /* Constants */
    SSA_OP_CONST_INT,
    SSA_OP_CONST_FLOAT,
    SSA_OP_CONST_STRING,
    SSA_OP_CONST_BOOL,
    SSA_OP_CONST_NIL,

    /* Function parameters and phi nodes */
    SSA_OP_PARAM,
    SSA_OP_PHI,

    /* Arithmetic / bitwise (mirrors BC_ADD..BC_SHR) */
    SSA_OP_ADD, SSA_OP_SUB, SSA_OP_MUL, SSA_OP_DIV, SSA_OP_MOD,
    SSA_OP_BIT_AND, SSA_OP_BIT_OR, SSA_OP_BIT_XOR, SSA_OP_SHL, SSA_OP_SHR,

    /* Comparison / logical */
    SSA_OP_CMP_EQ, SSA_OP_CMP_NE, SSA_OP_CMP_LT, SSA_OP_CMP_GT, SSA_OP_CMP_LE, SSA_OP_CMP_GE,
    SSA_OP_AND, SSA_OP_OR, SSA_OP_NIL_COALESCE,

    /* Unary */
    SSA_OP_NEG, SSA_OP_NOT, SSA_OP_BIT_NOT,

    /* Allocation sites — escape analysis (Phase B) targets */
    SSA_OP_ALLOC_STRUCT, SSA_OP_ALLOC_ARRAY, SSA_OP_ALLOC_TUPLE, SSA_OP_ALLOC_CLOSURE,

    /* Field / index access */
    SSA_OP_FIELD_GET, SSA_OP_FIELD_SET,
    SSA_OP_INDEX_GET, SSA_OP_INDEX_SET,

    /* Calls */
    SSA_OP_CALL_DIRECT,   /* statically resolved to one known, analyzed function */
    SSA_OP_CALL_DYNAMIC,  /* callee not a simple resolvable identifier */
    SSA_OP_DISPATCH,      /* runtime method dispatch (BC_DISPATCH-shaped) */

    /* Reads/writes this phase doesn't model precisely (globals, upvalues,
     * anything from a not-yet-implemented construct like try/match/comptime
     * payload extraction). Always typed SSA_UNKNOWN by the type-inference
     * pass — this is the deliberate, always-safe escape hatch: an
     * SSA_OP_UNKNOWN_VALUE can never be wrongly specialized because nothing
     * ever gives it a concrete type. */
    SSA_OP_UNKNOWN_VALUE,
    SSA_OP_UNKNOWN_EFFECT, /* an opaque side effect (write to something not modeled) */

    /* Control flow terminators (exactly one per block, must be last instr) */
    SSA_OP_JUMP,           /* operands: none; 1 successor */
    SSA_OP_BRANCH,         /* operands[0] = condition; 2 successors: true, false */
    SSA_OP_RETURN,         /* operands = return values (0, 1, or N) */
} SsaOpKind;

typedef struct SsaValue SsaValue;
typedef struct SsaInstr SsaInstr;
typedef struct SsaBlock SsaBlock;
typedef struct SsaFunction SsaFunction;
typedef struct SsaModule SsaModule;

struct SsaValue {
    int id;
    SsaType type;                 /* set by ssa_infer_types (Phase A.3); SSA_UNKNOWN until then */
    const char *struct_type_name; /* only meaningful when type == SSA_STRUCT */
    AstNode *origin;              /* source node this value corresponds to, for diagnostics */
    SsaInstr *def;                /* the instruction that produces this value; NULL once trivial */
    /* Union-find-style forwarding: when a phi is found trivial (all operands
     * are the same value or itself), it's redirected here instead of doing a
     * full replace-all-uses-with rewrite. Every reader must resolve through
     * ssa_resolve() before treating a value's identity/def as final. */
    SsaValue *replaced_by;
};

struct SsaInstr {
    SsaOpKind kind;
    SsaBlock *block;
    SsaValue *result;     /* NULL for pure-effect ops (FIELD_SET/INDEX_SET/JUMP/BRANCH/RETURN) */
    SsaValue **operands;
    int operand_count;
    int operand_capacity;

    /* Op-specific payload (only the fields relevant to `kind` are populated) */
    int64_t int_const;
    double float_const;
    const char *string_const;
    bool bool_const;
    const char *field_name;        /* FIELD_GET/FIELD_SET */
    const char *struct_type_name;  /* ALLOC_STRUCT */
    AstNode *callee_fn_node;       /* CALL_DIRECT: the resolved NODE_FN_DECL */
    const char *callee_name;       /* CALL_DIRECT/CALL_DYNAMIC/DISPATCH: for dumping */
    const char *dispatch_method;   /* DISPATCH */
    SsaBlock **targets;            /* JUMP: [dest]; BRANCH: [true_dest, false_dest] */

    /* Escape analysis (Phase B) writes these back onto ALLOC_* instructions */
    bool proven_non_escaping;
    int scope_size_hint;

    SsaInstr *next; /* intrusive list within block, in program order */
};

struct SsaBlock {
    int id;
    SsaInstr *first, *last;

    SsaBlock **preds; int pred_count, pred_capacity;
    SsaBlock **succs; int succ_count, succ_capacity;

    bool sealed; /* all predecessors known — incomplete phis can be finalized */
    bool filled; /* all instructions for this block have been emitted */

    /* Braun-algorithm bookkeeping, both indexed by local variable slot
     * (the same slot numbering compiler_add_local/compiler_find_local use). */
    SsaValue **current_def;     /* size == owning SsaFunction->slot_capacity */
    SsaInstr **incomplete_phis; /* same size; NULL entry == no pending phi for that slot */
};

struct SsaFunction {
    AstNode *fn_node; /* NULL for the synthetic top-level init, if ever built */
    const char *name;

    SsaBlock **blocks; int block_count, block_capacity;
    SsaValue **all_values; int value_count, value_capacity;

    int next_value_id;
    int next_block_id;
    int slot_capacity; /* current width of every block's current_def/incomplete_phis arrays */
};

struct SsaModule {
    SsaFunction **functions;
    int count, capacity;
    AstNode *program; /* retained so ssa_infer_types can look up schema field types */
};

/* Builds SSA for every function (top-level or nested) that suspend_analyze()
 * proved will never suspend. Functions that might suspend are skipped
 * entirely — no SsaFunction is created for them. */
SsaModule *ssa_build(AstNode *program, SuspendAnalysis *sa);
void ssa_module_free(SsaModule *mod);

/* LLVM-IR-flavored textual dump, backing `--dump-ssa`. */
void ssa_dump(SsaModule *mod, FILE *out);

/* Safe-by-construction type inference over the whole module (cross-function,
 * so mutually-recursive CALL_DIRECT chains converge correctly — not just
 * per-function), using a 3-level lattice: SSA_TYPE_PENDING (optimistic top —
 * "no evidence yet, could still become anything") > a concrete type >
 * SSA_UNKNOWN (bottom — "proven cannot be statically typed"). Every value is
 * reset to PENDING at the start of a run and can only ever move DOWNWARD:
 * PENDING -> a specific concrete type (adopted once at least one operand/
 * return-site proves it and nothing yet contradicts it) -> UNKNOWN (if a
 * later-resolving operand turns out to disagree, or a rule can't type it at
 * all). A value is never moved back up, and once at UNKNOWN it's absorbing
 * (final). This is the standard technique (the same shape as SCCP) needed to
 * correctly type loop-carried accumulators and (mutually) recursive return
 * types — a naive "only grow from UNKNOWN, require already-resolved
 * operands" pass (this module's original v1 attempt) can never bootstrap a
 * value whose only path to a concrete type routes back through itself (e.g.
 * `total = total + i` in a loop, or naive recursive fibonacci: verified by
 * hand-tracing both through the fixpoint before landing on this design).
 *
 * The actual safety property: a value only ever settles on a concrete type
 * T when, AT THE FIXPOINT (no more changes possible anywhere), its defining
 * rule — applied to its operands' own FINAL settled types — computes
 * exactly T; if it computed anything else, that would itself be a further
 * change, contradicting "fixpoint reached." So every concrete answer is a
 * genuine, internally-consistent solution derived from real runtime
 * semantics (arithmetic/comparison promotion mirrors vm.c's BINARY_OP_NUM /
 * L_BC_ADD exactly, not a reinvented notion of "numeric"), never a froze-in
 * mid-flight guess. A bug in a rule can still only cause a value to end at
 * UNKNOWN when it could have been specialized (a missed optimization) — it
 * cannot cause a wrong concrete answer, because reaching one requires that
 * rule to hold at the fixpoint, not just transiently. See src/ssa.c for the
 * full rule set. Safe to call repeatedly; idempotent once settled. */
void ssa_infer_types(SsaModule *mod);

/* Intraprocedural escape analysis (src/ssa_escape.c), run after
 * ssa_infer_types. For every SSA_OP_ALLOC_STRUCT/ARRAY/TUPLE/CLOSURE in every
 * function in the module, sets `proven_non_escaping` and (when true)
 * `scope_size_hint` on that instruction. An allocation is proven
 * non-escaping only if it (and anything it's merged with via a phi) is never:
 * stored into a global/upvalue, stored into the field/element of an
 * allocation that itself escapes (transitive), returned, or passed as an
 * operand to ANY call (direct, dynamic, or dispatch — deliberately
 * including the call's own object-typed args, unconditionally, even for
 * calls to other analyzed functions: this is the single most conservative
 * rule in the pass, because misclassifying an escaping value as safe here is
 * a stack-use-after-scope memory bug, strictly worse than a mistyped
 * arithmetic result). Everything not proven safe defaults to escaping
 * (heap-allocated, GC-tracked, exactly as today) — the only thing this
 * pass can do wrong is miss a stack-allocation opportunity, never allow an
 * unsafe one. See src/ssa_escape.c for the full argument. */
void ssa_escape_analyze(SsaModule *mod);

#endif
