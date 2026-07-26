#include "ssa.h"
#include "vm.h"
#include <stdlib.h>
#include <string.h>

/* ─── Growable arrays ─── */

#define GROW(arr, count, cap, type)                                          \
    do {                                                                     \
        if ((count) >= (cap)) {                                              \
            (cap) = (cap) ? (cap) * 2 : 8;                                   \
            (arr) = realloc((arr), (size_t)(cap) * sizeof(type));            \
        }                                                                    \
    } while (0)

/* ─── Function-name resolution table (whole program, built once) ───
 * Parallel to suspend_analysis's own collection, but independent: SSA
 * construction needs to know, for a NODE_CALL by name, both the callee's
 * AstNode (to record on SSA_OP_CALL_DIRECT) and whether it's itself eligible
 * for SSA (suspend_analysis_get == false). Ambiguous same-name matches
 * (shadowing) are resolved conservatively: if more than one function shares
 * a name, or any match is suspending, treat the call as unresolved
 * (CALL_DYNAMIC) rather than direct — an imprecise but always-safe fallback,
 * matching the project's established "over-approximate, never unsound"
 * philosophy (see release_prune_unused_functions in aot.c). */
typedef struct {
    AstNode *fn_node;
    const char *name;
} SsaFnTableEntry;

typedef struct {
    SsaFnTableEntry *entries;
    int count, capacity;
} SsaFnTable;

static void ssa_fntable_add(SsaFnTable *t, AstNode *node, const char *name) {
    GROW(t->entries, t->count, t->capacity, SsaFnTableEntry);
    t->entries[t->count].fn_node = node;
    t->entries[t->count].name = name;
    t->count++;
}

static void ssa_fntable_collect(AstNode *node, SsaFnTable *t) {
    if (!node) return;
    switch (node->kind) {
        case NODE_PROGRAM:
            for (int i = 0; i < node->program.stmt_count; i++)
                ssa_fntable_collect(node->program.stmts[i], t);
            break;
        case NODE_BLOCK:
            for (int i = 0; i < node->block.stmt_count; i++)
                ssa_fntable_collect(node->block.stmts[i], t);
            break;
        case NODE_IF:
            ssa_fntable_collect(node->if_stmt.then_branch, t);
            ssa_fntable_collect(node->if_stmt.else_branch, t);
            break;
        case NODE_WHILE: ssa_fntable_collect(node->while_stmt.body, t); break;
        case NODE_FOR: ssa_fntable_collect(node->for_stmt.body, t); break;
        case NODE_LOOP: ssa_fntable_collect(node->loop_stmt.body, t); break;
        case NODE_TRY:
            ssa_fntable_collect(node->try_stmt.try_body, t);
            ssa_fntable_collect(node->try_stmt.catch_body, t);
            break;
        case NODE_MATCH:
            for (int i = 0; i < node->match_stmt.arm_count; i++)
                ssa_fntable_collect(node->match_stmt.arms[i], t);
            break;
        case NODE_MATCH_ARM: ssa_fntable_collect(node->match_arm.body, t); break;
        case NODE_COMPTIME: ssa_fntable_collect(node->comptime.body, t); break;
        case NODE_TEST: ssa_fntable_collect(node->test_decl.body, t); break;
        case NODE_FN_DECL:
            ssa_fntable_add(t, node, node->fn_decl.name);
            ssa_fntable_collect(node->fn_decl.body, t);
            break;
        default:
            break;
    }
}

/* Resolves `name` to exactly one function node, or NULL if zero or more than
 * one candidate matches (ambiguous shadowing => treat as unresolved). */
static AstNode *ssa_fntable_resolve(SsaFnTable *t, const char *name) {
    AstNode *found = NULL;
    int matches = 0;
    for (int i = 0; i < t->count; i++) {
        if (strcmp(t->entries[i].name, name) == 0) {
            found = t->entries[i].fn_node;
            matches++;
        }
    }
    return matches == 1 ? found : NULL;
}

/* ─── Builder context ─── */

typedef struct {
    SsaBlock *break_target;
    SsaBlock *continue_target;
} SsaLoopCtx;

#define SSA_MAX_LOOP_NESTING 64

typedef struct {
    SsaModule *module;
    SsaFunction *fn;
    SsaBlock *current;
    Compiler scope; /* throwaway — only local_names/local_depths/scope_depth are used */
    SuspendAnalysis *suspend_analysis;
    SsaFnTable *fntable;
    SsaLoopCtx loops[SSA_MAX_LOOP_NESTING];
    int loop_count;
} SsaBuilder;

/* ─── Value / instruction / block construction ─── */

static SsaValue *ssa_resolve(SsaValue *v) {
    while (v && v->replaced_by) v = v->replaced_by;
    return v;
}

static SsaValue *ssa_new_value(SsaBuilder *b, AstNode *origin) {
    SsaValue *v = calloc(1, sizeof(SsaValue));
    v->id = b->fn->next_value_id++;
    v->type = SSA_UNKNOWN;
    v->origin = origin;
    GROW(b->fn->all_values, b->fn->value_count, b->fn->value_capacity, SsaValue *);
    b->fn->all_values[b->fn->value_count++] = v;
    return v;
}

/* Phis created lazily while reading a variable (the Braun-algorithm cases
 * below) must land at the very FRONT of the block's instruction list, not
 * wherever ssa_append's normal tail-insertion would put them — by the time
 * one of these fires, the block may already have real instructions (even its
 * terminator) emitted, e.g. a loop header's condition/branch is written
 * before its body is walked, and the body's read of a loop variable is what
 * triggers the header's phi. Appending at the tail in that case would insert
 * the phi AFTER the branch, corrupting block-terminator invariants. */
static SsaInstr *ssa_prepend_phi(SsaBuilder *b, SsaBlock *blk) {
    SsaInstr *instr = calloc(1, sizeof(SsaInstr));
    instr->kind = SSA_OP_PHI;
    instr->block = blk;
    instr->result = ssa_new_value(b, NULL);
    instr->result->def = instr;
    instr->next = blk->first;
    blk->first = instr;
    if (!blk->last) blk->last = instr;
    return instr;
}

static void ssa_grow_slots(SsaBuilder *b, int needed) {
    if (needed <= b->fn->slot_capacity) return;
    int new_cap = b->fn->slot_capacity ? b->fn->slot_capacity : 8;
    while (new_cap < needed) new_cap *= 2;
    for (int i = 0; i < b->fn->block_count; i++) {
        SsaBlock *blk = b->fn->blocks[i];
        blk->current_def = realloc(blk->current_def, (size_t)new_cap * sizeof(SsaValue *));
        blk->incomplete_phis = realloc(blk->incomplete_phis, (size_t)new_cap * sizeof(SsaInstr *));
        for (int s = b->fn->slot_capacity; s < new_cap; s++) {
            blk->current_def[s] = NULL;
            blk->incomplete_phis[s] = NULL;
        }
    }
    b->fn->slot_capacity = new_cap;
}

static SsaBlock *ssa_new_block(SsaBuilder *b) {
    SsaBlock *blk = calloc(1, sizeof(SsaBlock));
    blk->id = b->fn->next_block_id++;
    if (b->fn->slot_capacity > 0) {
        blk->current_def = calloc((size_t)b->fn->slot_capacity, sizeof(SsaValue *));
        blk->incomplete_phis = calloc((size_t)b->fn->slot_capacity, sizeof(SsaInstr *));
    }
    GROW(b->fn->blocks, b->fn->block_count, b->fn->block_capacity, SsaBlock *);
    b->fn->blocks[b->fn->block_count++] = blk;
    return blk;
}

static void ssa_add_pred(SsaBlock *blk, SsaBlock *pred) {
    GROW(blk->preds, blk->pred_count, blk->pred_capacity, SsaBlock *);
    blk->preds[blk->pred_count++] = pred;
}

static void ssa_add_succ(SsaBlock *blk, SsaBlock *succ) {
    GROW(blk->succs, blk->succ_count, blk->succ_capacity, SsaBlock *);
    blk->succs[blk->succ_count++] = succ;
}

/* Links `from` -> `to` (both pred/succ edges) — every control-flow edge in
 * this builder goes through here so preds/succs never get out of sync. */
static void ssa_link(SsaBlock *from, SsaBlock *to) {
    ssa_add_succ(from, to);
    ssa_add_pred(to, from);
}

static bool ssa_block_terminated(SsaBlock *blk) {
    return blk->last && (blk->last->kind == SSA_OP_JUMP || blk->last->kind == SSA_OP_BRANCH ||
                          blk->last->kind == SSA_OP_RETURN);
}

static SsaInstr *ssa_append(SsaBuilder *b, SsaOpKind kind, AstNode *origin, bool has_result) {
    SsaInstr *instr = calloc(1, sizeof(SsaInstr));
    instr->kind = kind;
    instr->block = b->current;
    if (has_result) {
        instr->result = ssa_new_value(b, origin);
        instr->result->def = instr;
    }
    if (b->current->last) b->current->last->next = instr;
    else b->current->first = instr;
    b->current->last = instr;
    return instr;
}

static void ssa_add_operand(SsaInstr *instr, SsaValue *v) {
    GROW(instr->operands, instr->operand_count, instr->operand_capacity, SsaValue *);
    instr->operands[instr->operand_count++] = ssa_resolve(v);
}

/* Every unconditional control-flow edge should go through here: it keeps the
 * jump instruction's `targets` and the block's pred/succ lists consistent by
 * construction, instead of the two being updated separately at each call
 * site (easy to let them drift, e.g. a jump whose target never actually got
 * recorded on the instruction — harmless for correctness since later passes
 * walk block->succs/preds, not instruction targets, but a silently
 * incomplete dump otherwise). */
static void ssa_emit_jump(SsaBuilder *b, SsaBlock *target) {
    SsaInstr *j = ssa_append(b, SSA_OP_JUMP, NULL, false);
    j->targets = malloc(sizeof(SsaBlock *));
    j->targets[0] = target;
    ssa_link(b->current, target);
}

/* ─── Braun-algorithm variable read/write/phi machinery ───
 * "Variable" here is a local-slot index from the throwaway Compiler's
 * name/depth table (see ssa.h and vm.h's Compiler struct) — reused so SSA
 * construction doesn't have to re-derive Varian's shadowing/scoping rules. */

static SsaValue *ssa_read_variable(SsaBuilder *b, SsaBlock *blk, int slot);

static SsaValue *ssa_try_remove_trivial_phi(SsaInstr *phi) {
    SsaValue *same = NULL;
    for (int i = 0; i < phi->operand_count; i++) {
        SsaValue *op = ssa_resolve(phi->operands[i]);
        if (op == same || op == phi->result) continue; /* unique-value-so-far or self-ref */
        if (same != NULL) return phi->result;           /* merges >=2 distinct values: not trivial */
        same = op;
    }
    /* same == NULL means the phi is unreachable or purely self-referential
     * (e.g. a loop-carried variable never actually written in the loop) —
     * there is no meaningful value; leave the phi as its own value rather
     * than fabricating an "undef" node, since nothing here can ever read an
     * actually-undefined variable (that's a semantic error caught earlier). */
    if (same == NULL) return phi->result;
    phi->result->replaced_by = same;
    return same;
}

static SsaValue *ssa_add_phi_operands(SsaBuilder *b, SsaBlock *blk, int slot, SsaInstr *phi) {
    for (int i = 0; i < blk->pred_count; i++) {
        SsaValue *v = ssa_read_variable(b, blk->preds[i], slot);
        ssa_add_operand(phi, v);
    }
    return ssa_try_remove_trivial_phi(phi);
}

static SsaValue *ssa_read_variable_recursive(SsaBuilder *b, SsaBlock *blk, int slot) {
    SsaValue *val;
    if (!blk->sealed) {
        /* Incomplete phi: predecessor set isn't final yet (loop header
         * waiting on its back edge). Recorded for seal_block to finish. */
        SsaInstr *phi = ssa_prepend_phi(b, blk);
        blk->incomplete_phis[slot] = phi;
        val = phi->result;
    } else if (blk->pred_count == 1) {
        val = ssa_read_variable(b, blk->preds[0], slot);
    } else if (blk->pred_count == 0) {
        /* Unreachable block (e.g. code after an unconditional jump) or the
         * function's entry block reading a never-written slot — both
         * indicate the value is unavailable here; a real read of a
         * genuinely undefined local is a semantic error caught long before
         * Kiln runs, so this path is only ever hit for dead code. */
        val = NULL;
    } else {
        SsaInstr *phi = ssa_prepend_phi(b, blk);
        blk->current_def[slot] = phi->result; /* write before recursing: breaks cycles */
        val = ssa_add_phi_operands(b, blk, slot, phi);
    }
    if (val) blk->current_def[slot] = val;
    return val;
}

static SsaValue *ssa_read_variable(SsaBuilder *b, SsaBlock *blk, int slot) {
    if (slot < b->fn->slot_capacity && blk->current_def[slot])
        return ssa_resolve(blk->current_def[slot]);
    return ssa_read_variable_recursive(b, blk, slot);
}

static void ssa_write_variable(SsaBuilder *b, SsaBlock *blk, int slot, SsaValue *v) {
    ssa_grow_slots(b, slot + 1);
    blk->current_def[slot] = v;
}

static void ssa_seal_block(SsaBuilder *b, SsaBlock *blk) {
    if (blk->sealed) return;
    for (int slot = 0; slot < b->fn->slot_capacity; slot++) {
        if (blk->incomplete_phis[slot]) {
            ssa_add_phi_operands(b, blk, slot, blk->incomplete_phis[slot]);
            blk->incomplete_phis[slot] = NULL;
        }
    }
    blk->sealed = true;
}

/* ─── Expression / statement lowering ─── */

static SsaValue *ssa_build_expr(SsaBuilder *b, AstNode *node);
static void ssa_build_stmt(SsaBuilder *b, AstNode *node);

static void ssa_build_stmts(SsaBuilder *b, AstNode **stmts, int count) {
    for (int i = 0; i < count; i++) {
        if (ssa_block_terminated(b->current)) break; /* dead code after return/break/continue */
        ssa_build_stmt(b, stmts[i]);
    }
}

static SsaOpKind ssa_binop_kind(BinaryOp op) {
    switch (op) {
        case OP_ADD: return SSA_OP_ADD;
        case OP_SUB: return SSA_OP_SUB;
        case OP_MUL: return SSA_OP_MUL;
        case OP_DIV: return SSA_OP_DIV;
        case OP_MOD: return SSA_OP_MOD;
        case OP_EQ: return SSA_OP_CMP_EQ;
        case OP_NE: return SSA_OP_CMP_NE;
        case OP_LT: return SSA_OP_CMP_LT;
        case OP_GT: return SSA_OP_CMP_GT;
        case OP_LE: return SSA_OP_CMP_LE;
        case OP_GE: return SSA_OP_CMP_GE;
        case OP_AND: return SSA_OP_AND;
        case OP_OR: return SSA_OP_OR;
        case OP_BIT_AND: return SSA_OP_BIT_AND;
        case OP_BIT_OR: return SSA_OP_BIT_OR;
        case OP_BIT_XOR: return SSA_OP_BIT_XOR;
        case OP_SHL: return SSA_OP_SHL;
        case OP_SHR: return SSA_OP_SHR;
        case OP_NIL_COALESCE: return SSA_OP_NIL_COALESCE;
    }
    return SSA_OP_UNKNOWN_VALUE;
}

/* A generic, always-safe fallback for expression shapes not yet lowered
 * precisely (try/match values, comptime, propagate, enum literals, ...):
 * still recurse into children so any nested calls/allocations/assignments
 * are captured, but the result itself is opaque. Type inference (A.3) will
 * always resolve an SSA_OP_UNKNOWN_VALUE result to SSA_UNKNOWN, so this can
 * never cause a mis-specialization later — only a missed optimization. */
static SsaValue *ssa_build_opaque(SsaBuilder *b, AstNode *node, AstNode **children, int count) {
    for (int i = 0; i < count; i++) ssa_build_expr(b, children[i]);
    SsaInstr *instr = ssa_append(b, SSA_OP_UNKNOWN_VALUE, node, true);
    (void)instr;
    return instr->result;
}

static SsaValue *ssa_build_expr(SsaBuilder *b, AstNode *node) {
    if (!node) return NULL;
    switch (node->kind) {
        case NODE_INT_LITERAL: {
            SsaInstr *i = ssa_append(b, SSA_OP_CONST_INT, node, true);
            i->int_const = node->literal.int_value;
            return i->result;
        }
        case NODE_FLOAT_LITERAL: {
            SsaInstr *i = ssa_append(b, SSA_OP_CONST_FLOAT, node, true);
            i->float_const = node->literal.float_value;
            return i->result;
        }
        case NODE_STRING_LITERAL: {
            SsaInstr *i = ssa_append(b, SSA_OP_CONST_STRING, node, true);
            i->string_const = node->literal.string_value;
            return i->result;
        }
        case NODE_BOOL_LITERAL: {
            SsaInstr *i = ssa_append(b, SSA_OP_CONST_BOOL, node, true);
            i->bool_const = node->literal.bool_value;
            return i->result;
        }
        case NODE_NULL_LITERAL:
            return ssa_append(b, SSA_OP_CONST_NIL, node, true)->result;

        case NODE_IDENTIFIER: {
            int slot = compiler_find_local(&b->scope, node->identifier.name);
            if (slot >= 0) return ssa_read_variable(b, b->current, slot);
            /* Global or upvalue: not modeled precisely in v1 — always opaque,
             * which is always safe (see ssa_build_opaque doc comment). */
            return ssa_append(b, SSA_OP_UNKNOWN_VALUE, node, true)->result;
        }

        case NODE_BINARY: {
            /* `&&`/`||` short-circuit (Lua/Python style: the surviving
             * operand's own value is the result, not necessarily a bool —
             * see the identical comment in vm.c's compile_node NODE_BINARY
             * case). Evaluating both operands unconditionally here would be
             * a real semantic bug, not just an imprecision: idioms like
             * `x == nil || x.field` rely on the right side never running
             * when the left already decides the result. `??` (nil-coalesce)
             * is NOT short-circuited by the real compiler (it's eager, a
             * plain BC_NIL_COALESCE after evaluating both sides) — only
             * AND/OR need this special path. */
            if (node->binary.op == OP_AND || node->binary.op == OP_OR) {
                SsaValue *left = ssa_build_expr(b, node->binary.left);
                SsaBlock *left_end = b->current;
                int temp_slot =
                    compiler_add_local(&b->scope, node->binary.op == OP_AND ? "__and__" : "__or__");
                ssa_write_variable(b, left_end, temp_slot, left);

                SsaBlock *right_blk = ssa_new_block(b);
                SsaBlock *merge = ssa_new_block(b);
                SsaInstr *branch = ssa_append(b, SSA_OP_BRANCH, node, false);
                ssa_add_operand(branch, left);
                branch->targets = malloc(sizeof(SsaBlock *) * 2);
                if (node->binary.op == OP_AND) {
                    branch->targets[0] = right_blk; /* left truthy -> evaluate right */
                    branch->targets[1] = merge;     /* left falsy -> short-circuit on left */
                } else {
                    branch->targets[0] = merge;     /* left truthy -> short-circuit on left */
                    branch->targets[1] = right_blk; /* left falsy -> evaluate right */
                }
                ssa_link(left_end, right_blk);
                ssa_link(left_end, merge);
                ssa_seal_block(b, right_blk);

                b->current = right_blk;
                SsaValue *right = ssa_build_expr(b, node->binary.right);
                ssa_write_variable(b, b->current, temp_slot, right);
                ssa_emit_jump(b, merge);

                ssa_seal_block(b, merge);
                b->current = merge;
                return ssa_read_variable(b, merge, temp_slot);
            }

            SsaValue *l = ssa_build_expr(b, node->binary.left);
            SsaValue *r = ssa_build_expr(b, node->binary.right);
            SsaInstr *i = ssa_append(b, ssa_binop_kind(node->binary.op), node, true);
            ssa_add_operand(i, l);
            ssa_add_operand(i, r);
            return i->result;
        }

        case NODE_UNARY: {
            SsaValue *v = ssa_build_expr(b, node->unary.operand);
            SsaOpKind k = node->unary.op == OP_NEG ? SSA_OP_NEG
                        : node->unary.op == OP_NOT ? SSA_OP_NOT
                                                    : SSA_OP_BIT_NOT;
            SsaInstr *i = ssa_append(b, k, node, true);
            ssa_add_operand(i, v);
            return i->result;
        }

        case NODE_CALL: {
            AstNode *callee = node->call.callee;
            SsaValue **args = malloc(sizeof(SsaValue *) * (size_t)(node->call.arg_count + 1));
            for (int i = 0; i < node->call.arg_count; i++)
                args[i] = ssa_build_expr(b, node->call.args[i]);

            SsaInstr *i;
            if (callee && callee->kind == NODE_IDENTIFIER &&
                compiler_find_local(&b->scope, callee->identifier.name) < 0) {
                AstNode *target = ssa_fntable_resolve(b->fntable, callee->identifier.name);
                bool eligible = target && !suspend_analysis_get(b->suspend_analysis, target);
                i = ssa_append(b, eligible ? SSA_OP_CALL_DIRECT : SSA_OP_CALL_DYNAMIC, node, true);
                i->callee_fn_node = eligible ? target : NULL;
                i->callee_name = callee->identifier.name;
            } else {
                /* Callee is a local variable holding a function value, or
                 * some other dynamic expression — can't resolve statically. */
                if (callee) ssa_build_expr(b, callee);
                i = ssa_append(b, SSA_OP_CALL_DYNAMIC, node, true);
                i->callee_name = (callee && callee->kind == NODE_IDENTIFIER)
                                      ? callee->identifier.name
                                      : "<dynamic>";
            }
            for (int a = 0; a < node->call.arg_count; a++) ssa_add_operand(i, args[a]);
            free(args);
            return i->result;
        }

        case NODE_DISPATCH_CALL: {
            SsaValue *obj = ssa_build_expr(b, node->dispatch_call.object);
            SsaInstr *i = ssa_append(b, SSA_OP_DISPATCH, node, true);
            i->dispatch_method = node->dispatch_call.method_name;
            ssa_add_operand(i, obj);
            for (int a = 0; a < node->dispatch_call.arg_count; a++)
                ssa_add_operand(i, ssa_build_expr(b, node->dispatch_call.args[a]));
            return i->result;
        }

        case NODE_MEMBER:
        case NODE_QUESTION_DOT: {
            SsaValue *obj = ssa_build_expr(b, node->member.object);
            SsaInstr *i = ssa_append(b, SSA_OP_FIELD_GET, node, true);
            i->field_name = node->member.member;
            ssa_add_operand(i, obj);
            return i->result;
        }

        case NODE_INDEX: {
            SsaValue *obj = ssa_build_expr(b, node->index.object);
            SsaValue *idx = ssa_build_expr(b, node->index.index);
            SsaInstr *i = ssa_append(b, SSA_OP_INDEX_GET, node, true);
            ssa_add_operand(i, obj);
            ssa_add_operand(i, idx);
            return i->result;
        }

        case NODE_STRUCT_LITERAL: {
            SsaValue **vals = malloc(sizeof(SsaValue *) * (size_t)(node->struct_literal.field_count + 1));
            for (int i = 0; i < node->struct_literal.field_count; i++)
                vals[i] = ssa_build_expr(b, node->struct_literal.field_values[i]);
            SsaInstr *i = ssa_append(b, SSA_OP_ALLOC_STRUCT, node, true);
            i->struct_type_name = node->struct_literal.name;
            for (int f = 0; f < node->struct_literal.field_count; f++) ssa_add_operand(i, vals[f]);
            free(vals);
            return i->result;
        }

        case NODE_ARRAY_LITERAL: {
            SsaInstr *i = ssa_append(b, SSA_OP_ALLOC_ARRAY, node, true);
            for (int e = 0; e < node->array_literal.element_count; e++)
                ssa_add_operand(i, ssa_build_expr(b, node->array_literal.elements[e]));
            return i->result;
        }

        case NODE_TUPLE_LITERAL: {
            SsaInstr *i = ssa_append(b, SSA_OP_ALLOC_TUPLE, node, true);
            for (int e = 0; e < node->tuple_literal.element_count; e++)
                ssa_add_operand(i, ssa_build_expr(b, node->tuple_literal.elements[e]));
            return i->result;
        }

        case NODE_INTERPOLATED_STRING:
            return ssa_build_opaque(b, node, node->interpolated_string.parts,
                                     node->interpolated_string.part_count);

        case NODE_ENUM_LITERAL:
            return ssa_build_opaque(b, node, node->enum_literal.values, node->enum_literal.value_count);

        case NODE_PROPAGATE:
            return ssa_build_opaque(b, node, &node->propagate.expr, node->propagate.expr ? 1 : 0);

        case NODE_ASSIGN: {
            /* Assignment as an expression (e.g. inside a larger expression);
             * statement-position assignment is handled by ssa_build_stmt.
             * Reaching here means it's nested — lower identically. */
            ssa_build_stmt(b, node);
            /* Assignments don't yield a useful SSA value in this IR; callers
             * that use an assignment's result (uncommon) get an opaque one. */
            return ssa_append(b, SSA_OP_UNKNOWN_VALUE, node, true)->result;
        }

        default:
            return ssa_build_opaque(b, node, NULL, 0);
    }
}

/* Lowers an assignment target (identifier/member/index) given an
 * already-built RHS value. Shared by plain and compound assignment. */
static void ssa_assign_to(SsaBuilder *b, AstNode *target, SsaValue *rhs) {
    if (target->kind == NODE_IDENTIFIER) {
        int slot = compiler_find_local(&b->scope, target->identifier.name);
        if (slot >= 0) {
            ssa_write_variable(b, b->current, slot, rhs);
        } else {
            /* Global or upvalue write: opaque effect, not modeled precisely. */
            SsaInstr *i = ssa_append(b, SSA_OP_UNKNOWN_EFFECT, target, false);
            ssa_add_operand(i, rhs);
        }
    } else if (target->kind == NODE_MEMBER) {
        SsaValue *obj = ssa_build_expr(b, target->member.object);
        SsaInstr *i = ssa_append(b, SSA_OP_FIELD_SET, target, false);
        i->field_name = target->member.member;
        ssa_add_operand(i, obj);
        ssa_add_operand(i, rhs);
    } else if (target->kind == NODE_INDEX) {
        SsaValue *obj = ssa_build_expr(b, target->index.object);
        SsaValue *idx = ssa_build_expr(b, target->index.index);
        SsaInstr *i = ssa_append(b, SSA_OP_INDEX_SET, target, false);
        ssa_add_operand(i, obj);
        ssa_add_operand(i, idx);
        ssa_add_operand(i, rhs);
    } else {
        SsaInstr *i = ssa_append(b, SSA_OP_UNKNOWN_EFFECT, target, false);
        ssa_add_operand(i, rhs);
    }
}

/* Closures capture their enclosing scope's locals by value at creation time
 * (see ObjClosure's doc comment in vm.h). When ssa_build_stmt reaches a
 * nested NODE_FN_DECL (a closure declaration), it does NOT descend into that
 * function's body for execution purposes — but any of THIS (enclosing)
 * function's own locals that the closure body references are captured right
 * there, and must be treated by escape analysis exactly like any other
 * value escaping through an opaque effect (matching a global/upvalue write)
 * — otherwise a struct that's only ever "used" by being captured into a
 * closure would be wrongly proven non-escaping and stack-allocated, even
 * though the closure (and the capture) can easily outlive this function's
 * own frame. This walk recurses into nested closures-within-closures (a
 * doubly-nested closure can still reference the outermost scope's locals
 * transitively), which is the opposite of ssa_build_stmt's own NODE_FN_DECL
 * handling (which deliberately does NOT descend, since executing this
 * function's body doesn't execute the nested one). Conservative by
 * construction: matching an outer-scope name is treated as a capture even
 * in the rare case the nested function also shadows that name with its own
 * local of the same name — over-marking here only costs a missed stack
 * allocation, never a wrong one. */
static void ssa_scan_captures(SsaBuilder *b, AstNode *node) {
    if (!node) return;
    switch (node->kind) {
        case NODE_BLOCK:
            for (int i = 0; i < node->block.stmt_count; i++) ssa_scan_captures(b, node->block.stmts[i]);
            break;
        case NODE_LET_DECL:
        case NODE_CONST_DECL:
            ssa_scan_captures(b, node->let_decl.initializer);
            break;
        case NODE_FN_DECL:
            for (int i = 0; i < node->fn_decl.decorator_count; i++)
                ssa_scan_captures(b, node->fn_decl.decorator_values[i]);
            ssa_scan_captures(b, node->fn_decl.body);
            break;
        case NODE_EXPR_STMT:
            ssa_scan_captures(b, node->expr_stmt.expr);
            break;
        case NODE_IF:
            ssa_scan_captures(b, node->if_stmt.condition);
            ssa_scan_captures(b, node->if_stmt.then_branch);
            ssa_scan_captures(b, node->if_stmt.else_branch);
            break;
        case NODE_WHILE:
            ssa_scan_captures(b, node->while_stmt.condition);
            ssa_scan_captures(b, node->while_stmt.body);
            break;
        case NODE_FOR:
            ssa_scan_captures(b, node->for_stmt.iterable);
            ssa_scan_captures(b, node->for_stmt.body);
            break;
        case NODE_LOOP:
            ssa_scan_captures(b, node->loop_stmt.body);
            break;
        case NODE_RETURN:
            for (int i = 0; i < node->return_stmt.value_count; i++)
                ssa_scan_captures(b, node->return_stmt.values[i]);
            break;
        case NODE_ASSIGN:
            ssa_scan_captures(b, node->assign.target);
            ssa_scan_captures(b, node->assign.value);
            break;
        case NODE_BINARY:
            ssa_scan_captures(b, node->binary.left);
            ssa_scan_captures(b, node->binary.right);
            break;
        case NODE_UNARY:
            ssa_scan_captures(b, node->unary.operand);
            break;
        case NODE_CALL:
            ssa_scan_captures(b, node->call.callee);
            for (int i = 0; i < node->call.arg_count; i++) ssa_scan_captures(b, node->call.args[i]);
            break;
        case NODE_INDEX:
            ssa_scan_captures(b, node->index.object);
            ssa_scan_captures(b, node->index.index);
            break;
        case NODE_MEMBER:
        case NODE_QUESTION_DOT:
            ssa_scan_captures(b, node->member.object);
            break;
        case NODE_IDENTIFIER: {
            int slot = compiler_find_local(&b->scope, node->identifier.name);
            if (slot >= 0) {
                SsaValue *v = ssa_read_variable(b, b->current, slot);
                SsaInstr *i = ssa_append(b, SSA_OP_UNKNOWN_EFFECT, node, false);
                ssa_add_operand(i, v);
            }
            break;
        }
        case NODE_INTERPOLATED_STRING:
            for (int i = 0; i < node->interpolated_string.part_count; i++)
                ssa_scan_captures(b, node->interpolated_string.parts[i]);
            break;
        case NODE_ARRAY_LITERAL:
        case NODE_TUPLE_LITERAL:
            for (int i = 0; i < node->array_literal.element_count; i++)
                ssa_scan_captures(b, node->array_literal.elements[i]);
            break;
        case NODE_STRUCT_LITERAL:
            for (int i = 0; i < node->struct_literal.field_count; i++)
                ssa_scan_captures(b, node->struct_literal.field_values[i]);
            break;
        case NODE_ENUM_LITERAL:
            for (int i = 0; i < node->enum_literal.value_count; i++)
                ssa_scan_captures(b, node->enum_literal.values[i]);
            break;
        case NODE_MATCH:
            ssa_scan_captures(b, node->match_stmt.value);
            for (int i = 0; i < node->match_stmt.arm_count; i++) ssa_scan_captures(b, node->match_stmt.arms[i]);
            break;
        case NODE_MATCH_ARM:
            ssa_scan_captures(b, node->match_arm.pattern);
            ssa_scan_captures(b, node->match_arm.body);
            break;
        case NODE_ASSERT:
            ssa_scan_captures(b, node->assert_stmt.condition);
            break;
        case NODE_PROPAGATE:
            ssa_scan_captures(b, node->propagate.expr);
            break;
        case NODE_TRY:
            ssa_scan_captures(b, node->try_stmt.try_body);
            ssa_scan_captures(b, node->try_stmt.catch_body);
            break;
        case NODE_COMPTIME:
            ssa_scan_captures(b, node->comptime.body);
            break;
        case NODE_DISPATCH_CALL:
            ssa_scan_captures(b, node->dispatch_call.object);
            for (int i = 0; i < node->dispatch_call.arg_count; i++)
                ssa_scan_captures(b, node->dispatch_call.args[i]);
            break;
        case NODE_CHAN_SEND:
            ssa_scan_captures(b, node->chan_send.channel);
            ssa_scan_captures(b, node->chan_send.value);
            break;
        case NODE_CHAN_RECEIVE:
            ssa_scan_captures(b, node->chan_receive.channel);
            break;
        case NODE_AWAIT:
            ssa_scan_captures(b, node->await.expr);
            break;
        case NODE_TEST:
            ssa_scan_captures(b, node->test_decl.body);
            break;
        /* These are genuinely reachable here even though this scan only
         * ever runs for a function ssa_build_function already proved
         * non-suspending: a NESTED closure declared (but not necessarily
         * called) inside such a function can itself contain await/channel
         * ops without forcing the ENCLOSING function to suspend — only an
         * actual CALL to a suspending function propagates (see
         * suspend_scan_node's NODE_FN_DECL case, which is also a no-op on
         * mere declaration). So a captured identifier can legitimately sit
         * inside one of these constructs here, and must still be found. */
        default:
            break;
    }
}

static void ssa_build_stmt(SsaBuilder *b, AstNode *node) {
    if (!node) return;
    switch (node->kind) {
        case NODE_LET_DECL:
        case NODE_CONST_DECL: {
            SsaValue *init = node->let_decl.initializer ? ssa_build_expr(b, node->let_decl.initializer)
                                                         : ssa_append(b, SSA_OP_CONST_NIL, node, true)->result;
            for (int i = 0; i < node->let_decl.name_count; i++) {
                int slot = compiler_add_local(&b->scope, node->let_decl.names[i]);
                ssa_write_variable(b, b->current, slot, init);
            }
            break;
        }

        case NODE_FN_DECL:
            /* Nested function/closure declaration: doesn't execute here.
             * Its own SSA (if eligible) is built separately by the top-level
             * driver, exactly as suspend_analyze treats it (see
             * suspend_analysis.c's identical NODE_FN_DECL skip). But any of
             * THIS function's own locals the closure body references are
             * captured by value right here — mark them so escape analysis
             * treats them exactly like any other opaque effect (see
             * ssa_scan_captures' doc comment for why this matters). */
            ssa_scan_captures(b, node);
            break;

        case NODE_EXPR_STMT:
            /* An assignment reached as a bare expression-statement (the
             * common case: `x = y` parses as expr_stmt(assign)) should go
             * through the statement-level assignment handling directly —
             * routing it through ssa_build_expr's NODE_ASSIGN case would
             * still do the right assignment but also tack on a pointless
             * extra opaque placeholder value that nothing here ever reads. */
            if (node->expr_stmt.expr && node->expr_stmt.expr->kind == NODE_ASSIGN)
                ssa_build_stmt(b, node->expr_stmt.expr);
            else
                ssa_build_expr(b, node->expr_stmt.expr);
            break;

        case NODE_BLOCK: {
            int saved_local_count = b->scope.local_count;
            ssa_build_stmts(b, node->block.stmts, node->block.stmt_count);
            b->scope.local_count = saved_local_count;
            break;
        }

        case NODE_IF: {
            SsaValue *cond = ssa_build_expr(b, node->if_stmt.condition);
            SsaBlock *origin = b->current;
            SsaBlock *then_blk = ssa_new_block(b);
            SsaBlock *else_blk = node->if_stmt.else_branch ? ssa_new_block(b) : NULL;
            SsaBlock *merge = ssa_new_block(b);

            SsaInstr *branch = ssa_append(b, SSA_OP_BRANCH, node, false);
            ssa_add_operand(branch, cond);
            branch->targets = malloc(sizeof(SsaBlock *) * 2);
            branch->targets[0] = then_blk;
            branch->targets[1] = else_blk ? else_blk : merge;
            ssa_link(origin, then_blk);
            ssa_link(origin, else_blk ? else_blk : merge);
            ssa_seal_block(b, then_blk);
            if (else_blk) ssa_seal_block(b, else_blk);

            b->current = then_blk;
            ssa_build_stmt(b, node->if_stmt.then_branch);
            if (!ssa_block_terminated(b->current)) ssa_emit_jump(b, merge);

            if (else_blk) {
                b->current = else_blk;
                ssa_build_stmt(b, node->if_stmt.else_branch);
                if (!ssa_block_terminated(b->current)) ssa_emit_jump(b, merge);
            }

            ssa_seal_block(b, merge);
            b->current = merge;
            break;
        }

        case NODE_WHILE: {
            SsaBlock *header = ssa_new_block(b);
            SsaBlock *body = ssa_new_block(b);
            SsaBlock *exit = ssa_new_block(b);

            ssa_emit_jump(b, header);
            /* header has 2 preds (origin, body's back-edge) — can't seal yet */

            b->current = header;
            SsaValue *cond = ssa_build_expr(b, node->while_stmt.condition);
            SsaInstr *branch = ssa_append(b, SSA_OP_BRANCH, node, false);
            ssa_add_operand(branch, cond);
            branch->targets = malloc(sizeof(SsaBlock *) * 2);
            branch->targets[0] = body;
            branch->targets[1] = exit;
            ssa_link(header, body);
            ssa_link(header, exit);
            ssa_seal_block(b, body); /* body's only pred is header, known now */

            if (b->loop_count < SSA_MAX_LOOP_NESTING) {
                b->loops[b->loop_count].break_target = exit;
                b->loops[b->loop_count].continue_target = header;
                b->loop_count++;
            }
            b->current = body;
            ssa_build_stmt(b, node->while_stmt.body);
            if (!ssa_block_terminated(b->current)) ssa_emit_jump(b, header); /* the back edge */
            if (b->loop_count > 0) b->loop_count--;

            ssa_seal_block(b, header); /* all preds known now */
            ssa_seal_block(b, exit);   /* header's false-branch + any breaks, all added by now */
            b->current = exit;
            break;
        }

        case NODE_LOOP: {
            SsaBlock *header = ssa_new_block(b);
            SsaBlock *exit = ssa_new_block(b);

            ssa_emit_jump(b, header);

            if (b->loop_count < SSA_MAX_LOOP_NESTING) {
                b->loops[b->loop_count].break_target = exit;
                b->loops[b->loop_count].continue_target = header;
                b->loop_count++;
            }
            b->current = header;
            ssa_build_stmt(b, node->loop_stmt.body);
            if (!ssa_block_terminated(b->current)) ssa_emit_jump(b, header);
            if (b->loop_count > 0) b->loop_count--;

            ssa_seal_block(b, header);
            ssa_seal_block(b, exit);
            b->current = exit;
            break;
        }

        case NODE_FOR: {
            /* Desugars the same way the bytecode compiler does (vm.c's
             * NODE_FOR handling): a range `start..end` becomes var/end
             * locals; an array iterable becomes arr/i/len locals plus an
             * index read each iteration. Top-level (global) for-loops never
             * reach here — SSA is only ever built for function bodies. */
            int saved_local_count = b->scope.local_count;
            bool is_range = node->for_stmt.iterable &&
                            node->for_stmt.iterable->kind == NODE_TUPLE_LITERAL &&
                            node->for_stmt.iterable->tuple_literal.element_count == 2;

            SsaBlock *header = ssa_new_block(b);
            SsaBlock *body = ssa_new_block(b);
            SsaBlock *exit = ssa_new_block(b);
            int var_slot, end_or_len_slot, arr_slot = -1;

            if (is_range) {
                SsaValue *start = ssa_build_expr(b, node->for_stmt.iterable->tuple_literal.elements[0]);
                SsaValue *end = ssa_build_expr(b, node->for_stmt.iterable->tuple_literal.elements[1]);
                var_slot = compiler_add_local(&b->scope, node->for_stmt.var_name);
                end_or_len_slot = compiler_add_local(&b->scope, "__end__");
                ssa_write_variable(b, b->current, var_slot, start);
                ssa_write_variable(b, b->current, end_or_len_slot, end);
            } else {
                SsaValue *arr = ssa_build_expr(b, node->for_stmt.iterable);
                arr_slot = compiler_add_local(&b->scope, "__arr__");
                ssa_write_variable(b, b->current, arr_slot, arr);
                SsaInstr *len_call = ssa_append(b, SSA_OP_DISPATCH, node, true);
                len_call->dispatch_method = "len";
                ssa_add_operand(len_call, arr);
                end_or_len_slot = compiler_add_local(&b->scope, "__len__");
                ssa_write_variable(b, b->current, end_or_len_slot, len_call->result);
                SsaValue *zero = ssa_append(b, SSA_OP_CONST_INT, node, true)->result;
                var_slot = compiler_add_local(&b->scope, "__i__");
                ssa_write_variable(b, b->current, var_slot, zero);
                /* User-visible loop var is written from arr[__i__] each iteration. */
                compiler_add_local(&b->scope, node->for_stmt.var_name);
            }

            ssa_emit_jump(b, header);

            b->current = header;
            SsaValue *cur = ssa_read_variable(b, header, var_slot);
            SsaValue *bound = ssa_read_variable(b, header, end_or_len_slot);
            SsaInstr *cmp = ssa_append(b, SSA_OP_CMP_LT, node, true);
            ssa_add_operand(cmp, cur);
            ssa_add_operand(cmp, bound);
            SsaInstr *branch = ssa_append(b, SSA_OP_BRANCH, node, false);
            ssa_add_operand(branch, cmp->result);
            branch->targets = malloc(sizeof(SsaBlock *) * 2);
            branch->targets[0] = body;
            branch->targets[1] = exit;
            ssa_link(header, body);
            ssa_link(header, exit);
            ssa_seal_block(b, body);

            if (b->loop_count < SSA_MAX_LOOP_NESTING) {
                b->loops[b->loop_count].break_target = exit;
                b->loops[b->loop_count].continue_target = header;
                b->loop_count++;
            }
            b->current = body;
            if (!is_range) {
                int user_var_slot = compiler_find_local(&b->scope, node->for_stmt.var_name);
                SsaValue *arr = ssa_read_variable(b, body, arr_slot);
                SsaValue *idx = ssa_read_variable(b, body, var_slot);
                SsaInstr *idx_get = ssa_append(b, SSA_OP_INDEX_GET, node, true);
                ssa_add_operand(idx_get, arr);
                ssa_add_operand(idx_get, idx);
                ssa_write_variable(b, body, user_var_slot, idx_get->result);
            }
            ssa_build_stmt(b, node->for_stmt.body);
            if (!ssa_block_terminated(b->current)) {
                SsaValue *v = ssa_read_variable(b, b->current, var_slot);
                SsaInstr *one = ssa_append(b, SSA_OP_CONST_INT, node, true);
                one->int_const = 1;
                SsaInstr *inc = ssa_append(b, SSA_OP_ADD, node, true);
                ssa_add_operand(inc, v);
                ssa_add_operand(inc, one->result);
                ssa_write_variable(b, b->current, var_slot, inc->result);
                ssa_emit_jump(b, header);
            }
            if (b->loop_count > 0) b->loop_count--;

            ssa_seal_block(b, header);
            ssa_seal_block(b, exit);
            b->current = exit;
            b->scope.local_count = saved_local_count;
            break;
        }

        case NODE_BREAK:
            if (b->loop_count > 0) ssa_emit_jump(b, b->loops[b->loop_count - 1].break_target);
            break;

        case NODE_CONTINUE:
            if (b->loop_count > 0) ssa_emit_jump(b, b->loops[b->loop_count - 1].continue_target);
            break;

        case NODE_RETURN: {
            /* A bare `return;` (value_count == 0) still yields nil at
             * runtime — the real compiler emits BC_NIL before BC_RETURN for
             * exactly this case (see compile_node's NODE_RETURN handling in
             * vm.c) — so model it the same way rather than a truly
             * value-less return, which would make return-type inference
             * (and any future codegen) treat it as a distinct, unmodeled
             * shape for no real semantic reason. */
            if (node->return_stmt.value_count == 0) {
                SsaValue *nil = ssa_append(b, SSA_OP_CONST_NIL, node, true)->result;
                SsaInstr *ret = ssa_append(b, SSA_OP_RETURN, node, false);
                ssa_add_operand(ret, nil);
                break;
            }
            SsaValue **vals = malloc(sizeof(SsaValue *) * (size_t)node->return_stmt.value_count);
            for (int i = 0; i < node->return_stmt.value_count; i++)
                vals[i] = ssa_build_expr(b, node->return_stmt.values[i]);
            SsaInstr *ret = ssa_append(b, SSA_OP_RETURN, node, false);
            for (int i = 0; i < node->return_stmt.value_count; i++) ssa_add_operand(ret, vals[i]);
            free(vals);
            break;
        }

        case NODE_ASSIGN: {
            SsaValue *rhs;
            if (node->assign.is_compound) {
                SsaValue *old = ssa_build_expr(b, node->assign.target);
                SsaValue *rval = ssa_build_expr(b, node->assign.value);
                SsaInstr *op = ssa_append(b, ssa_binop_kind(node->assign.compound_op), node, true);
                ssa_add_operand(op, old);
                ssa_add_operand(op, rval);
                rhs = op->result;
            } else {
                rhs = ssa_build_expr(b, node->assign.value);
            }
            ssa_assign_to(b, node->assign.target, rhs);
            break;
        }

        case NODE_ASSERT:
            ssa_build_expr(b, node->assert_stmt.condition);
            break;

        case NODE_TRY: {
            /* Conservative approximation, not a precise throw-propagation
             * model: the try body always runs first (a real, single-target
             * jump), and — since we don't track a distinct edge per
             * instruction that might throw — the catch block's predecessor
             * is approximated as "the try block as a whole" rather than
             * every individual point inside it that could throw. Good enough
             * for a diagnostic-only IR; SSA_UNKNOWN propagation (Phase A.3)
             * keeps any imprecision here safe. */
            SsaBlock *try_blk = ssa_new_block(b);
            SsaBlock *catch_blk = node->try_stmt.catch_body ? ssa_new_block(b) : NULL;
            SsaBlock *merge = ssa_new_block(b);

            ssa_emit_jump(b, try_blk);
            ssa_seal_block(b, try_blk);
            if (catch_blk) {
                ssa_link(try_blk, catch_blk);
                ssa_seal_block(b, catch_blk);
            }

            b->current = try_blk;
            ssa_build_stmt(b, node->try_stmt.try_body);
            if (!ssa_block_terminated(b->current)) ssa_emit_jump(b, merge);

            if (catch_blk) {
                b->current = catch_blk;
                if (node->try_stmt.catch_var) compiler_add_local(&b->scope, node->try_stmt.catch_var);
                ssa_build_stmt(b, node->try_stmt.catch_body);
                if (!ssa_block_terminated(b->current)) ssa_emit_jump(b, merge);
            }
            ssa_seal_block(b, merge);
            b->current = merge;
            break;
        }

        case NODE_MATCH: {
            /* Conservative approximation: value + each arm treated as an
             * opaque read plus its own block, all merging afterward — bind
             * names become opaque-valued locals. Precise enum-tag/payload
             * modeling is deferred; SSA_UNKNOWN propagation keeps this safe. */
            ssa_build_expr(b, node->match_stmt.value);
            SsaBlock *origin = b->current;
            SsaBlock *merge = ssa_new_block(b);
            for (int i = 0; i < node->match_stmt.arm_count; i++) {
                AstNode *arm = node->match_stmt.arms[i];
                SsaBlock *arm_blk = ssa_new_block(b);
                ssa_link(origin, arm_blk);
                ssa_seal_block(b, arm_blk);
                b->current = arm_blk;
                int saved = b->scope.local_count;
                for (int bn = 0; bn < arm->match_arm.bind_count; bn++) {
                    int slot = compiler_add_local(&b->scope, arm->match_arm.bind_names[bn]);
                    ssa_write_variable(b, arm_blk, slot,
                                        ssa_append(b, SSA_OP_UNKNOWN_VALUE, arm, true)->result);
                }
                ssa_build_stmt(b, arm->match_arm.body);
                if (!ssa_block_terminated(b->current)) ssa_emit_jump(b, merge);
                b->scope.local_count = saved;
            }
            ssa_seal_block(b, merge);
            b->current = merge;
            break;
        }

        case NODE_COMPTIME:
            ssa_build_stmt(b, node->comptime.body);
            break;

        case NODE_TEST:
            /* Test bodies aren't part of a release binary's normal call
             * graph; nothing to lower here. */
            break;

        default:
            ssa_build_expr(b, node);
            break;
    }
}

/* ─── Per-function driver ─── */

static void ssa_build_function(SsaModule *mod, AstNode *fn_node, SuspendAnalysis *sa,
                                SsaFnTable *fntable) {
    SsaFunction *fn = calloc(1, sizeof(SsaFunction));
    fn->fn_node = fn_node;
    fn->name = fn_node->fn_decl.name;
    GROW(mod->functions, mod->count, mod->capacity, SsaFunction *);
    mod->functions[mod->count++] = fn;

    SsaBuilder b = {0};
    b.module = mod;
    b.fn = fn;
    b.suspend_analysis = sa;
    b.fntable = fntable;
    compiler_init(&b.scope, NULL, NULL, NULL);
    b.scope.in_function = true;
    b.scope.scope_depth = 1; /* mirrors vm.c's real fn_compiler; harmless here either way
                              * since ssa.c never reads local_depths, kept only for fidelity */

    SsaBlock *entry = ssa_new_block(&b);
    ssa_seal_block(&b, entry); /* entry has no predecessors — sealed trivially */
    b.current = entry;

    for (int i = 0; i < fn_node->fn_decl.param_count; i++) {
        int slot = compiler_add_local(&b.scope, fn_node->fn_decl.param_names[i]);
        SsaInstr *param = ssa_append(&b, SSA_OP_PARAM, fn_node, true);
        param->int_const = i; /* parameter index, so type inference can look up its Type* annotation */
        ssa_write_variable(&b, entry, slot, param->result);
    }

    if (fn_node->fn_decl.body) ssa_build_stmt(&b, fn_node->fn_decl.body);

    if (!ssa_block_terminated(b.current)) {
        SsaValue *nil = ssa_append(&b, SSA_OP_CONST_NIL, fn_node, true)->result;
        SsaInstr *ret = ssa_append(&b, SSA_OP_RETURN, fn_node, false); /* implicit, like the compiler's */
        ssa_add_operand(ret, nil);
    }
}

/* Recursively finds every eligible (non-suspending) function declaration —
 * top-level or nested — and builds SSA for each. Ineligible functions are
 * skipped entirely, but their nested children are still visited in case one
 * of THOSE is independently eligible (a suspending outer function can still
 * contain a leaf nested helper). */
static void ssa_collect_and_build(AstNode *node, SsaModule *mod, SuspendAnalysis *sa,
                                   SsaFnTable *fntable) {
    if (!node) return;
    switch (node->kind) {
        case NODE_PROGRAM:
            for (int i = 0; i < node->program.stmt_count; i++)
                ssa_collect_and_build(node->program.stmts[i], mod, sa, fntable);
            break;
        case NODE_BLOCK:
            for (int i = 0; i < node->block.stmt_count; i++)
                ssa_collect_and_build(node->block.stmts[i], mod, sa, fntable);
            break;
        case NODE_IF:
            ssa_collect_and_build(node->if_stmt.then_branch, mod, sa, fntable);
            ssa_collect_and_build(node->if_stmt.else_branch, mod, sa, fntable);
            break;
        case NODE_WHILE: ssa_collect_and_build(node->while_stmt.body, mod, sa, fntable); break;
        case NODE_FOR: ssa_collect_and_build(node->for_stmt.body, mod, sa, fntable); break;
        case NODE_LOOP: ssa_collect_and_build(node->loop_stmt.body, mod, sa, fntable); break;
        case NODE_TRY:
            ssa_collect_and_build(node->try_stmt.try_body, mod, sa, fntable);
            ssa_collect_and_build(node->try_stmt.catch_body, mod, sa, fntable);
            break;
        case NODE_MATCH:
            for (int i = 0; i < node->match_stmt.arm_count; i++)
                ssa_collect_and_build(node->match_stmt.arms[i], mod, sa, fntable);
            break;
        case NODE_MATCH_ARM: ssa_collect_and_build(node->match_arm.body, mod, sa, fntable); break;
        case NODE_COMPTIME: ssa_collect_and_build(node->comptime.body, mod, sa, fntable); break;
        case NODE_TEST: break; /* test bodies are never AOT/native candidates */
        case NODE_FN_DECL:
            if (!suspend_analysis_get(sa, node)) ssa_build_function(mod, node, sa, fntable);
            ssa_collect_and_build(node->fn_decl.body, mod, sa, fntable);
            break;
        default:
            break;
    }
}

SsaModule *ssa_build(AstNode *program, SuspendAnalysis *sa) {
    SsaModule *mod = calloc(1, sizeof(SsaModule));
    mod->program = program;
    if (!sa) return mod; /* nothing proven safe => nothing to build */

    SsaFnTable fntable = {0};
    ssa_fntable_collect(program, &fntable);

    ssa_collect_and_build(program, mod, sa, &fntable);

    free(fntable.entries);
    return mod;
}

/* ─── Cleanup ─── */

static void ssa_block_free(SsaBlock *blk) {
    for (SsaInstr *ins = blk->first; ins;) {
        SsaInstr *next = ins->next;
        free(ins->operands);
        free(ins->targets);
        free(ins);
        ins = next;
    }
    free(blk->preds);
    free(blk->succs);
    free(blk->current_def);
    free(blk->incomplete_phis);
    free(blk);
}

void ssa_module_free(SsaModule *mod) {
    if (!mod) return;
    for (int f = 0; f < mod->count; f++) {
        SsaFunction *fn = mod->functions[f];
        for (int i = 0; i < fn->block_count; i++) ssa_block_free(fn->blocks[i]);
        for (int v = 0; v < fn->value_count; v++) free(fn->all_values[v]);
        free(fn->blocks);
        free(fn->all_values);
        free(fn);
    }
    free(mod->functions);
    free(mod);
}

/* ─── Type inference (Phase A.3) ───
 *
 * Monotonic, optimistic, safe-by-construction: every SsaValue starts
 * SSA_UNKNOWN (set at construction in ssa_new_value) and is only ever
 * narrowed to a concrete type by one of the rules below, each of which
 * requires its inputs to already be concrete — there is no rule that
 * assigns a concrete type "by default". That is the actual safety argument
 * (see the doc comment on ssa_infer_types in ssa.h): a bug here can only
 * leave a value at SSA_UNKNOWN when it could have been specialized (a
 * missed optimization), never mis-specialize one, because reaching a
 * concrete type always requires satisfying a specific narrow precondition. */

static bool ssa_type_matches(SsaType at, const char *aname, SsaType bt, const char *bname) {
    if (at != bt) return false;
    if (at == SSA_STRUCT) return aname && bname && strcmp(aname, bname) == 0;
    return true;
}

/* Joins a set of (possibly still-PENDING) types the way a phi / a function's
 * aggregate return type does: any UNKNOWN input poisons the whole join to
 * UNKNOWN immediately (a genuinely-can't-type path exists); among the inputs
 * that are already concrete, if they all agree (or there are none yet, only
 * PENDING) the join is that concrete type (adopted optimistically pending
 * confirmation) or PENDING if there's no concrete evidence at all yet; two
 * disagreeing concrete inputs is an immediate, permanent UNKNOWN. */
typedef struct {
    SsaType concrete;
    const char *concrete_name;
    bool have_concrete;
    bool poisoned;
} SsaJoin;

static void ssa_join_add(SsaJoin *j, SsaType t, const char *name) {
    if (j->poisoned) return;
    if (t == SSA_UNKNOWN) {
        j->poisoned = true;
        return;
    }
    if (t == SSA_TYPE_PENDING) return;
    if (!j->have_concrete) {
        j->concrete = t;
        j->concrete_name = name;
        j->have_concrete = true;
    } else if (!ssa_type_matches(j->concrete, j->concrete_name, t, name)) {
        j->poisoned = true;
    }
}

static SsaType ssa_join_result(SsaJoin *j, const char **name_out) {
    if (j->poisoned) return SSA_UNKNOWN;
    if (!j->have_concrete) return SSA_TYPE_PENDING;
    *name_out = j->concrete_name;
    return j->concrete;
}

static SsaType ssa_primitive_to_type(PrimitiveKind pk) {
    switch (pk) {
        case PRIMITIVE_INT: return SSA_INT;
        case PRIMITIVE_FLOAT: return SSA_FLOAT;
        case PRIMITIVE_STRING: return SSA_STRING;
        case PRIMITIVE_BOOL: return SSA_BOOL;
        default: return SSA_UNKNOWN; /* BYTE/VOID/PTR/C_* — FFI-flavored, not modeled in v1 */
    }
}

/* Resolves a parsed `Type*` annotation (possibly NULL) to an SsaType. Only
 * primitive and named (struct) annotations are modeled in v1 — array/tuple/
 * function-typed annotations fall back to UNKNOWN, which is always safe. */
static SsaType ssa_type_from_annotation(Type *t, const char **struct_name_out) {
    if (!t) return SSA_UNKNOWN;
    if (t->kind == TYPE_PRIMITIVE) return ssa_primitive_to_type(t->primitive);
    if (t->kind == TYPE_NAMED) {
        *struct_name_out = t->named.name;
        return SSA_STRUCT;
    }
    return SSA_UNKNOWN;
}

static SsaFunction *ssa_module_find_function(SsaModule *mod, AstNode *fn_node) {
    for (int i = 0; i < mod->count; i++)
        if (mod->functions[i]->fn_node == fn_node) return mod->functions[i];
    return NULL;
}

/* A function's inferred "return type" for CALL_DIRECT purposes: the join
 * (see ssa_join_*) of every SSA_OP_RETURN's single operand type across the
 * whole function. A multi-value return anywhere poisons the whole function's
 * return type to UNKNOWN (not modeled as a single value in v1).
 *
 * This uses the same PENDING-aware join as a phi, which is what lets a
 * (mutually-)recursive function's return type still resolve correctly: a
 * recursive call to `fn` from within its own body reads `fn`'s
 * still-being-computed return type, which starts PENDING; once a non-
 * recursive ("base case") return site resolves concretely, the join adopts
 * that type optimistically, the recursive call sites pick it up on the next
 * pass, and if the recursive arithmetic built from them agrees, the fixpoint
 * confirms and settles — see ssa_infer_types' doc comment in ssa.h for the
 * full argument. If the recursive path is instead the ONLY path (no base
 * case reachable, or the base case disagrees), this correctly settles at
 * UNKNOWN instead. */
static SsaType ssa_function_return_type(SsaFunction *fn, const char **struct_name_out) {
    SsaJoin j = {0};
    bool any_return = false;
    for (int bi = 0; bi < fn->block_count; bi++) {
        for (SsaInstr *ins = fn->blocks[bi]->first; ins; ins = ins->next) {
            if (ins->kind != SSA_OP_RETURN) continue;
            any_return = true;
            if (ins->operand_count != 1) return SSA_UNKNOWN; /* multi-return: not modeled as a single value */
            SsaValue *v = ssa_resolve(ins->operands[0]);
            ssa_join_add(&j, v ? v->type : SSA_UNKNOWN, v ? v->struct_type_name : NULL);
        }
    }
    if (!any_return) return SSA_UNKNOWN; /* unreachable / no return sites at all */
    return ssa_join_result(&j, struct_name_out);
}

static SsaType ssa_numeric_promote(SsaType a, SsaType b) {
    if (a == SSA_INT && b == SSA_INT) return SSA_INT;
    if ((a == SSA_INT || a == SSA_FLOAT) && (b == SSA_INT || b == SSA_FLOAT)) return SSA_FLOAT;
    return SSA_UNKNOWN;
}

/* PENDING-aware wrapper for binop rules: an UNKNOWN operand poisons the
 * result immediately (permanent); a still-PENDING operand (with the other
 * not yet UNKNOWN either) defers the result to PENDING too, so the fixpoint
 * revisits once that operand settles, instead of prematurely giving up. */
static SsaType ssa_numeric_promote_pending(SsaType a, SsaType b) {
    if (a == SSA_UNKNOWN || b == SSA_UNKNOWN) return SSA_UNKNOWN;
    if (a == SSA_TYPE_PENDING || b == SSA_TYPE_PENDING) return SSA_TYPE_PENDING;
    return ssa_numeric_promote(a, b);
}

static SsaType ssa_int_only_pending(SsaType a, SsaType b) {
    if (a == SSA_UNKNOWN || b == SSA_UNKNOWN) return SSA_UNKNOWN;
    if (a == SSA_TYPE_PENDING || b == SSA_TYPE_PENDING) return SSA_TYPE_PENDING;
    return (a == SSA_INT && b == SSA_INT) ? SSA_INT : SSA_UNKNOWN;
}

/* Schema field-type registry, built once per ssa_infer_types() call (not
 * per field-access — a fixpoint can revisit the same FIELD_GET many times).
 * Only `schema` declarations carry field type annotations in the AST; plain
 * `struct` declarations parse and discard them (parser.c's
 * parse_struct_decl calls parse_type() for the annotation and never stores
 * the result) — so a field read on a plain-struct receiver is always
 * SSA_UNKNOWN here, which is correct, not a missed case. */
typedef struct {
    const char *struct_name;
    char **field_names;
    Type **field_types;
    int field_count;
} SsaSchemaEntry;

typedef struct {
    SsaSchemaEntry *entries;
    int count, capacity;
} SsaSchemaTable;

static void ssa_schema_collect(AstNode *node, SsaSchemaTable *t) {
    if (!node) return;
    switch (node->kind) {
        case NODE_PROGRAM:
            for (int i = 0; i < node->program.stmt_count; i++) ssa_schema_collect(node->program.stmts[i], t);
            break;
        case NODE_BLOCK:
            for (int i = 0; i < node->block.stmt_count; i++) ssa_schema_collect(node->block.stmts[i], t);
            break;
        case NODE_IF:
            ssa_schema_collect(node->if_stmt.then_branch, t);
            ssa_schema_collect(node->if_stmt.else_branch, t);
            break;
        case NODE_WHILE: ssa_schema_collect(node->while_stmt.body, t); break;
        case NODE_FOR: ssa_schema_collect(node->for_stmt.body, t); break;
        case NODE_LOOP: ssa_schema_collect(node->loop_stmt.body, t); break;
        case NODE_TRY:
            ssa_schema_collect(node->try_stmt.try_body, t);
            ssa_schema_collect(node->try_stmt.catch_body, t);
            break;
        case NODE_FN_DECL: ssa_schema_collect(node->fn_decl.body, t); break;
        case NODE_SCHEMA_DECL:
            GROW(t->entries, t->count, t->capacity, SsaSchemaEntry);
            t->entries[t->count].struct_name = node->schema_decl.name;
            t->entries[t->count].field_names = node->schema_decl.field_names;
            t->entries[t->count].field_types = node->schema_decl.field_types;
            t->entries[t->count].field_count = node->schema_decl.field_count;
            t->count++;
            break;
        default:
            break;
    }
}

static SsaType ssa_schema_field_type(SsaSchemaTable *t, const char *struct_name, const char *field_name,
                                     const char **struct_name_out) {
    for (int i = 0; i < t->count; i++) {
        if (strcmp(t->entries[i].struct_name, struct_name) != 0) continue;
        for (int f = 0; f < t->entries[i].field_count; f++) {
            if (strcmp(t->entries[i].field_names[f], field_name) == 0)
                return ssa_type_from_annotation(t->entries[i].field_types[f], struct_name_out);
        }
        return SSA_UNKNOWN; /* schema found but this field wasn't declared on it */
    }
    return SSA_UNKNOWN; /* not a schema-declared struct at all */
}

/* Computes the type a single instruction's result *would* have given the
 * CURRENT (possibly still-improving) types of its operands. Called
 * repeatedly to a fixpoint by ssa_infer_types. `struct_name_out` is only
 * written when the return value is SSA_STRUCT. */
static SsaType ssa_compute_instr_type(SsaModule *mod, SsaSchemaTable *schemas, SsaFunction *fn,
                                      AstNode *fn_node, SsaInstr *ins, const char **struct_name_out) {
    (void)fn; /* not currently needed by any rule; kept for signature symmetry/future use */
    switch (ins->kind) {
        case SSA_OP_CONST_INT: return SSA_INT;
        case SSA_OP_CONST_FLOAT: return SSA_FLOAT;
        case SSA_OP_CONST_STRING: return SSA_STRING;
        case SSA_OP_CONST_BOOL: return SSA_BOOL;
        case SSA_OP_CONST_NIL: return SSA_UNKNOWN; /* nil has no useful concrete numeric/string type */

        case SSA_OP_PARAM: {
            /* A belief only — Phase C must still guard it at runtime before
             * unboxing, since a caller can violate a declared annotation in
             * a dynamic language. Inference just records what's *declared*. */
            if (!fn_node->fn_decl.fn_type || fn_node->fn_decl.fn_type->kind != TYPE_FUNCTION)
                return SSA_UNKNOWN;
            int idx = (int)ins->int_const;
            if (idx < 0 || idx >= fn_node->fn_decl.fn_type->function.param_count) return SSA_UNKNOWN;
            return ssa_type_from_annotation(fn_node->fn_decl.fn_type->function.param_types[idx],
                                             struct_name_out);
        }

        case SSA_OP_PHI: {
            if (ins->operand_count == 0) return SSA_UNKNOWN;
            SsaJoin j = {0};
            for (int i = 0; i < ins->operand_count; i++) {
                SsaValue *op = ssa_resolve(ins->operands[i]);
                ssa_join_add(&j, op ? op->type : SSA_UNKNOWN, op ? op->struct_type_name : NULL);
            }
            return ssa_join_result(&j, struct_name_out);
        }

        case SSA_OP_ADD: {
            /* Mirrors L_BC_ADD/aot_op_add exactly (vm.c): if EITHER operand
             * is proven STRING, the result is ALWAYS a string regardless of
             * the other operand's type (even if still UNKNOWN/PENDING) —
             * the runtime auto-stringifies anything on the non-string side,
             * so `a.type==VAL_STRING || b.type==VAL_STRING` is
             * unconditionally true whenever one side is a proven string,
             * independent of the other side's eventual type. Genuinely
             * stronger, still-sound, not a shortcut. */
            SsaValue *l = ssa_resolve(ins->operands[0]);
            SsaValue *r = ssa_resolve(ins->operands[1]);
            SsaType lt = l ? l->type : SSA_UNKNOWN, rt = r ? r->type : SSA_UNKNOWN;
            if (lt == SSA_STRING || rt == SSA_STRING) return SSA_STRING;
            return ssa_numeric_promote_pending(lt, rt);
        }
        case SSA_OP_SUB:
        case SSA_OP_MUL:
        case SSA_OP_DIV: {
            SsaValue *l = ssa_resolve(ins->operands[0]);
            SsaValue *r = ssa_resolve(ins->operands[1]);
            return ssa_numeric_promote_pending(l ? l->type : SSA_UNKNOWN, r ? r->type : SSA_UNKNOWN);
        }
        case SSA_OP_MOD: {
            /* vm.c's L_BC_MOD reads both operands as raw int64 unconditionally
             * (no type check at all) — so INT is only a sound conclusion
             * when both operands are already proven INT; anything else is
             * genuinely unmodeled runtime behavior, not just "unknown to
             * us", so UNKNOWN is the only honest answer here (once neither
             * side is still PENDING). */
            SsaValue *l = ssa_resolve(ins->operands[0]);
            SsaValue *r = ssa_resolve(ins->operands[1]);
            return ssa_int_only_pending(l ? l->type : SSA_UNKNOWN, r ? r->type : SSA_UNKNOWN);
        }
        case SSA_OP_BIT_AND:
        case SSA_OP_BIT_OR:
        case SSA_OP_BIT_XOR:
        case SSA_OP_SHL:
        case SSA_OP_SHR: {
            SsaValue *l = ssa_resolve(ins->operands[0]);
            SsaValue *r = ssa_resolve(ins->operands[1]);
            return ssa_int_only_pending(l ? l->type : SSA_UNKNOWN, r ? r->type : SSA_UNKNOWN);
        }

        case SSA_OP_CMP_EQ:
        case SSA_OP_CMP_NE:
            /* value_equal() always returns a real bool regardless of the
             * operand types being compared (vm.c L_BC_EQUAL/NOT_EQUAL) — no
             * dependency on operand types at all, resolves immediately. */
            return SSA_BOOL;
        case SSA_OP_CMP_LT:
        case SSA_OP_CMP_GT:
        case SSA_OP_CMP_LE:
        case SSA_OP_CMP_GE: {
            /* NOT boolean at runtime — Varian's ordering comparisons reuse
             * BINARY_OP_NUM and push val_int/val_float (the raw C `a op b`
             * result), never val_bool (see vm.c's BINARY_OP_NUM macro and
             * project memory on this exact quirk). */
            SsaValue *l = ssa_resolve(ins->operands[0]);
            SsaValue *r = ssa_resolve(ins->operands[1]);
            return ssa_numeric_promote_pending(l ? l->type : SSA_UNKNOWN, r ? r->type : SSA_UNKNOWN);
        }

        case SSA_OP_AND:
        case SSA_OP_OR:
            /* Unreachable in practice — ssa_build_expr intercepts OP_AND/OR
             * before ever emitting these op kinds (see the short-circuit
             * comment there); kept only so the enum/dump stay complete. */
            return SSA_UNKNOWN;
        case SSA_OP_NIL_COALESCE:
            return SSA_UNKNOWN; /* result could be either side's differing type */

        case SSA_OP_NOT:
            return SSA_BOOL; /* no operand dependency, resolves immediately */
        case SSA_OP_NEG: {
            SsaValue *v = ssa_resolve(ins->operands[0]);
            SsaType t = v ? v->type : SSA_UNKNOWN;
            if (t == SSA_UNKNOWN) return SSA_UNKNOWN;
            if (t == SSA_TYPE_PENDING) return SSA_TYPE_PENDING;
            return (t == SSA_INT || t == SSA_FLOAT) ? t : SSA_UNKNOWN;
        }
        case SSA_OP_BIT_NOT: {
            SsaValue *v = ssa_resolve(ins->operands[0]);
            SsaType t = v ? v->type : SSA_UNKNOWN;
            if (t == SSA_UNKNOWN) return SSA_UNKNOWN;
            if (t == SSA_TYPE_PENDING) return SSA_TYPE_PENDING;
            return t == SSA_INT ? SSA_INT : SSA_UNKNOWN;
        }

        case SSA_OP_ALLOC_STRUCT:
            *struct_name_out = ins->struct_type_name;
            return SSA_STRUCT;
        case SSA_OP_ALLOC_ARRAY:
        case SSA_OP_ALLOC_TUPLE:
        case SSA_OP_ALLOC_CLOSURE:
            /* Concrete allocation *kinds*, but not modeled as element-typed
             * SsaTypes in v1 (no SSA_ARRAY/SSA_TUPLE/SSA_CLOSURE variant) —
             * only escape analysis (Phase B) cares which allocation kind
             * this is, which it reads straight off ins->kind, not off the
             * result's SsaType. Safe to leave UNKNOWN here. */
            return SSA_UNKNOWN;

        case SSA_OP_FIELD_GET: {
            /* Only typed if the receiver is a proven-monomorphic named
             * struct AND that struct's declaration has a static field-type
             * annotation for this field name — never "trust the struct decl
             * blindly" for values arriving through an unproven receiver.
             * Note: plain `struct` declarations parse and DISCARD field type
             * annotations (see parser.c's parse_struct_decl: the type after
             * `:` is parsed then thrown away) — only `schema` declarations
             * retain `field_types[]`. So this can only ever resolve a field
             * read on a schema-declared struct; that's a real, deliberate
             * limitation of the current AST, not an inference shortcut. */
            SsaValue *obj = ssa_resolve(ins->operands[0]);
            SsaType ot = obj ? obj->type : SSA_UNKNOWN;
            if (ot == SSA_TYPE_PENDING) return SSA_TYPE_PENDING; /* receiver not resolved yet */
            if (ot != SSA_STRUCT || !obj->struct_type_name) return SSA_UNKNOWN;
            return ssa_schema_field_type(schemas, obj->struct_type_name, ins->field_name, struct_name_out);
        }
        case SSA_OP_FIELD_SET:
        case SSA_OP_INDEX_GET:
        case SSA_OP_INDEX_SET:
            return SSA_UNKNOWN; /* array element types aren't modeled in v1 */

        case SSA_OP_CALL_DIRECT: {
            if (!ins->callee_fn_node) return SSA_UNKNOWN;
            SsaFunction *callee = ssa_module_find_function(mod, ins->callee_fn_node);
            if (!callee) return SSA_UNKNOWN;
            return ssa_function_return_type(callee, struct_name_out);
        }
        case SSA_OP_CALL_DYNAMIC:
        case SSA_OP_DISPATCH:
            /* Unresolved callee / runtime dispatch / FFI — always UNKNOWN in
             * v1 (FFI-signature-informed inference is an explicit, deferred
             * v2 refinement; keeping the surface area small here). */
            return SSA_UNKNOWN;

        case SSA_OP_UNKNOWN_VALUE:
        case SSA_OP_UNKNOWN_EFFECT:
        case SSA_OP_JUMP:
        case SSA_OP_BRANCH:
        case SSA_OP_RETURN:
            return SSA_UNKNOWN;
    }
    return SSA_UNKNOWN;
}

void ssa_infer_types(SsaModule *mod) {
    if (!mod) return;
    SsaSchemaTable schemas = {0};
    if (mod->program) ssa_schema_collect(mod->program, &schemas);

    /* Every result-bearing value starts optimistic (PENDING), not UNKNOWN —
     * see the ssa.h doc comment on why this 3-level lattice (PENDING >
     * concrete > UNKNOWN) is what lets loop-carried accumulators and
     * (mutually) recursive return types resolve correctly. */
    for (int f = 0; f < mod->count; f++) {
        SsaFunction *fn = mod->functions[f];
        for (int bi = 0; bi < fn->block_count; bi++)
            for (SsaInstr *ins = fn->blocks[bi]->first; ins; ins = ins->next)
                if (ins->result) ins->result->type = SSA_TYPE_PENDING;
    }

    bool changed;
    do {
        changed = false;
        for (int f = 0; f < mod->count; f++) {
            SsaFunction *fn = mod->functions[f];
            for (int bi = 0; bi < fn->block_count; bi++) {
                for (SsaInstr *ins = fn->blocks[bi]->first; ins; ins = ins->next) {
                    if (!ins->result) continue;
                    SsaType old = ins->result->type;
                    if (old == SSA_UNKNOWN) continue; /* bottom is absorbing */

                    const char *new_name = NULL;
                    SsaType new_t = ssa_compute_instr_type(mod, &schemas, fn, fn->fn_node, ins, &new_name);

                    if (old == SSA_TYPE_PENDING) {
                        /* Any transition out of PENDING is a real move (down
                         * to a concrete type, or straight to UNKNOWN);
                         * staying PENDING is not a change. */
                        if (new_t != SSA_TYPE_PENDING) {
                            ins->result->type = new_t;
                            ins->result->struct_type_name = new_name;
                            changed = true;
                        }
                    } else {
                        /* old is already concrete: only two legal further
                         * moves — confirm (no-op) or downgrade to UNKNOWN
                         * (either the rule now says UNKNOWN outright, or it
                         * recomputed a DIFFERENT concrete type than before,
                         * which is a genuine contradiction, not a refinement
                         * — concrete types never change identity, only ever
                         * retract to UNKNOWN). A recomputation of PENDING at
                         * this point would be an illegal upgrade attempt
                         * (shouldn't happen given the rules above always
                         * derive from already-settled neighbors once old is
                         * concrete) and is simply ignored, not applied. */
                        if (new_t == SSA_UNKNOWN) {
                            ins->result->type = SSA_UNKNOWN;
                            changed = true;
                        } else if (new_t != SSA_TYPE_PENDING &&
                                   !ssa_type_matches(old, ins->result->struct_type_name, new_t, new_name)) {
                            ins->result->type = SSA_UNKNOWN;
                            changed = true;
                        }
                    }
                }
            }
        }
    } while (changed);

    /* Anything still PENDING never received any evidence at all — genuinely
     * unreachable/dead code (e.g. a phi in a block with 0 predecessors) or a
     * value-cycle with no external anchor. PENDING must never leak out of
     * this function; finalize it to the safe, final UNKNOWN. */
    for (int f = 0; f < mod->count; f++) {
        SsaFunction *fn = mod->functions[f];
        for (int bi = 0; bi < fn->block_count; bi++)
            for (SsaInstr *ins = fn->blocks[bi]->first; ins; ins = ins->next)
                if (ins->result && ins->result->type == SSA_TYPE_PENDING) ins->result->type = SSA_UNKNOWN;
    }

    free(schemas.entries);
}

/* ─── Textual dump (--dump-ssa) ─── */

static const char *ssa_op_name(SsaOpKind k) {
    switch (k) {
        case SSA_OP_CONST_INT: return "const.int";
        case SSA_OP_CONST_FLOAT: return "const.float";
        case SSA_OP_CONST_STRING: return "const.string";
        case SSA_OP_CONST_BOOL: return "const.bool";
        case SSA_OP_CONST_NIL: return "const.nil";
        case SSA_OP_PARAM: return "param";
        case SSA_OP_PHI: return "phi";
        case SSA_OP_ADD: return "add";
        case SSA_OP_SUB: return "sub";
        case SSA_OP_MUL: return "mul";
        case SSA_OP_DIV: return "div";
        case SSA_OP_MOD: return "mod";
        case SSA_OP_BIT_AND: return "bit_and";
        case SSA_OP_BIT_OR: return "bit_or";
        case SSA_OP_BIT_XOR: return "bit_xor";
        case SSA_OP_SHL: return "shl";
        case SSA_OP_SHR: return "shr";
        case SSA_OP_CMP_EQ: return "cmp.eq";
        case SSA_OP_CMP_NE: return "cmp.ne";
        case SSA_OP_CMP_LT: return "cmp.lt";
        case SSA_OP_CMP_GT: return "cmp.gt";
        case SSA_OP_CMP_LE: return "cmp.le";
        case SSA_OP_CMP_GE: return "cmp.ge";
        case SSA_OP_AND: return "and";
        case SSA_OP_OR: return "or";
        case SSA_OP_NIL_COALESCE: return "nil_coalesce";
        case SSA_OP_NEG: return "neg";
        case SSA_OP_NOT: return "not";
        case SSA_OP_BIT_NOT: return "bit_not";
        case SSA_OP_ALLOC_STRUCT: return "alloc.struct";
        case SSA_OP_ALLOC_ARRAY: return "alloc.array";
        case SSA_OP_ALLOC_TUPLE: return "alloc.tuple";
        case SSA_OP_ALLOC_CLOSURE: return "alloc.closure";
        case SSA_OP_FIELD_GET: return "field.get";
        case SSA_OP_FIELD_SET: return "field.set";
        case SSA_OP_INDEX_GET: return "index.get";
        case SSA_OP_INDEX_SET: return "index.set";
        case SSA_OP_CALL_DIRECT: return "call.direct";
        case SSA_OP_CALL_DYNAMIC: return "call.dynamic";
        case SSA_OP_DISPATCH: return "dispatch";
        case SSA_OP_UNKNOWN_VALUE: return "unknown.value";
        case SSA_OP_UNKNOWN_EFFECT: return "unknown.effect";
        case SSA_OP_JUMP: return "jump";
        case SSA_OP_BRANCH: return "branch";
        case SSA_OP_RETURN: return "return";
    }
    return "?";
}

static const char *ssa_type_name(SsaType t) {
    switch (t) {
        case SSA_INT: return "int";
        case SSA_FLOAT: return "float";
        case SSA_BOOL: return "bool";
        case SSA_STRING: return "string";
        case SSA_STRUCT: return "struct";
        case SSA_UNKNOWN: return "unknown";
        case SSA_TYPE_PENDING: return "PENDING(bug: leaked past ssa_infer_types)";
    }
    return "?";
}

static void ssa_dump_value_ref(SsaValue *v, FILE *out) {
    if (!v) {
        fprintf(out, "%%-1");
        return;
    }
    if (v->type == SSA_STRUCT && v->struct_type_name)
        fprintf(out, "%%%d:%s(%s)", v->id, ssa_type_name(v->type), v->struct_type_name);
    else
        fprintf(out, "%%%d:%s", v->id, ssa_type_name(v->type));
}

static void ssa_dump_instr(SsaInstr *ins, FILE *out) {
    fprintf(out, "    ");
    if (ins->result) {
        ssa_dump_value_ref(ins->result, out);
        fprintf(out, " = ");
    }
    fprintf(out, "%s", ssa_op_name(ins->kind));
    if (ins->kind == SSA_OP_CONST_INT) fprintf(out, " %lld", (long long)ins->int_const);
    if (ins->kind == SSA_OP_CONST_FLOAT) fprintf(out, " %g", ins->float_const);
    if (ins->kind == SSA_OP_CONST_BOOL) fprintf(out, " %s", ins->bool_const ? "true" : "false");
    if (ins->kind == SSA_OP_CONST_STRING) fprintf(out, " %s", ins->string_const ? ins->string_const : "");
    if (ins->field_name) fprintf(out, " .%s", ins->field_name);
    if (ins->struct_type_name) fprintf(out, " %s", ins->struct_type_name);
    if (ins->callee_name) fprintf(out, " %s", ins->callee_name);
    if (ins->dispatch_method) fprintf(out, " .%s()", ins->dispatch_method);
    for (int i = 0; i < ins->operand_count; i++) {
        fprintf(out, "%s", i == 0 ? " " : ", ");
        ssa_dump_value_ref(ssa_resolve(ins->operands[i]), out);
    }
    if (ins->kind == SSA_OP_JUMP && ins->targets) fprintf(out, " -> bb%d", ins->targets[0]->id);
    if (ins->kind == SSA_OP_BRANCH && ins->targets)
        fprintf(out, " -> bb%d, bb%d", ins->targets[0]->id, ins->targets[1]->id);
    if (ins->kind == SSA_OP_ALLOC_STRUCT || ins->kind == SSA_OP_ALLOC_ARRAY ||
        ins->kind == SSA_OP_ALLOC_TUPLE || ins->kind == SSA_OP_ALLOC_CLOSURE) {
        if (ins->proven_non_escaping) fprintf(out, " [STACK size=%d]", ins->scope_size_hint);
        else fprintf(out, " [HEAP]");
    }
    fprintf(out, "\n");
}

void ssa_dump(SsaModule *mod, FILE *out) {
    for (int f = 0; f < mod->count; f++) {
        SsaFunction *fn = mod->functions[f];
        fprintf(out, "function %s {\n", fn->name);
        for (int bi = 0; bi < fn->block_count; bi++) {
            SsaBlock *blk = fn->blocks[bi];
            fprintf(out, "  bb%d: (preds:", blk->id);
            for (int p = 0; p < blk->pred_count; p++) fprintf(out, " bb%d", blk->preds[p]->id);
            fprintf(out, ") %s\n", blk->sealed ? "[sealed]" : "[UNSEALED]");
            for (SsaInstr *ins = blk->first; ins; ins = ins->next) ssa_dump_instr(ins, out);
        }
        fprintf(out, "}\n");
    }
}
