#include "aot_native.h"
#include <stdlib.h>
#include <string.h>

/* See include/aot_native.h for the design, the subset definition, and the
 * argument for why a second (native-to-native) calling convention was
 * necessary. This file is the mechanical half: prove membership in that subset
 * (aot_native_plan), then lower it to C (aot_native_emit_function). */

/* ─── Shared helpers ─── */

static SsaValue *nat_resolve(SsaValue *v) {
    while (v && v->replaced_by) v = v->replaced_by;
    return v;
}

/* An instruction whose result was forwarded away by trivial-phi elimination is
 * dead: it stays in the block list but nothing may read it, and nothing may
 * emit it either (its C local would never be assigned). */
static bool nat_dead(SsaInstr *ins) {
    return ins->result && ins->result->replaced_by;
}

static bool nat_scalar(SsaType t) {
    return t == SSA_INT || t == SSA_FLOAT || t == SSA_BOOL;
}

static const char *nat_ctype(SsaType t) {
    switch (t) {
        case SSA_INT: return "int64_t";
        case SSA_FLOAT: return "double";
        case SSA_BOOL: return "bool";
        default: return NULL;
    }
}

static SsaType nat_type_of(SsaValue *v) {
    SsaValue *r = nat_resolve(v);
    return r ? r->type : SSA_UNKNOWN;
}

/* Marks every block reachable from the entry block. Unreachable blocks are
 * never emitted and never checked for eligibility: nothing in a reachable
 * block can reference them (SSA dominance), and an unreachable block's phis
 * legitimately have zero operands, which would otherwise look like a
 * malformed function and reject the whole thing for no reason. */
static void nat_mark_reachable(SsaFunction *sfn, bool *seen, SsaBlock *blk) {
    if (!blk || blk->id < 0 || blk->id >= sfn->block_count || seen[blk->id]) return;
    seen[blk->id] = true;
    for (int i = 0; i < blk->succ_count; i++) nat_mark_reachable(sfn, seen, blk->succs[i]);
}

static bool *nat_reachable_blocks(SsaFunction *sfn) {
    bool *seen = calloc((size_t)sfn->block_count, sizeof(bool));
    if (!seen) return NULL;
    /* blocks[0] is the entry block: ssa_build_function creates it first. */
    nat_mark_reachable(sfn, seen, sfn->blocks[0]);
    return seen;
}

/* Index of `from` within `to`'s predecessor list — the same index its phi
 * operands use (ssa_add_phi_operands walks blk->preds in order). Returns -1 if
 * absent, which eligibility has already ruled out. */
static int nat_pred_index(SsaBlock *to, SsaBlock *from) {
    for (int i = 0; i < to->pred_count; i++)
        if (to->preds[i] == from) return i;
    return -1;
}

/* The declared type of parameter `idx`, as ssa_infer_types would have assigned
 * it to that function's PARAM value. Used to type-check a native call's
 * arguments against the callee's C signature. */
static SsaType nat_param_type(SsaFunction *sfn, int idx) {
    for (int bi = 0; bi < sfn->block_count; bi++)
        for (SsaInstr *ins = sfn->blocks[bi]->first; ins; ins = ins->next)
            if (ins->kind == SSA_OP_PARAM && !nat_dead(ins) && (int)ins->int_const == idx)
                return nat_type_of(ins->result);
    return SSA_UNKNOWN;
}

/* The single scalar type every RETURN in this function yields, or SSA_UNKNOWN
 * if the returns disagree (or there are none) — a C function has one return
 * type, so disagreement makes the function ineligible. */
static SsaType nat_return_type(SsaFunction *sfn) {
    SsaType found = SSA_UNKNOWN;
    bool any = false;
    for (int bi = 0; bi < sfn->block_count; bi++) {
        for (SsaInstr *ins = sfn->blocks[bi]->first; ins; ins = ins->next) {
            if (ins->kind != SSA_OP_RETURN || ins->operand_count != 1) continue;
            SsaType t = nat_type_of(ins->operands[0]);
            if (!any) { found = t; any = true; }
            else if (t != found) return SSA_UNKNOWN;
        }
    }
    return any ? found : SSA_UNKNOWN;
}

/* ─── Eligibility ─── */

/* Per-op operand/result constraints, excluding CALL_DIRECT (which depends on
 * the callee's eligibility and so is resolved by the fixpoint below). Every
 * rule states exactly what it needs; anything unrecognised falls through to
 * `return false`, so adding a new SsaOpKind can never silently become
 * "natively compilable by default". */
static bool nat_instr_ok(SsaInstr *ins) {
    SsaType rt = ins->result ? nat_type_of(ins->result) : SSA_UNKNOWN;
    SsaType a = ins->operand_count > 0 ? nat_type_of(ins->operands[0]) : SSA_UNKNOWN;
    SsaType b = ins->operand_count > 1 ? nat_type_of(ins->operands[1]) : SSA_UNKNOWN;

    switch (ins->kind) {
        case SSA_OP_CONST_INT: return rt == SSA_INT;
        case SSA_OP_CONST_FLOAT: return rt == SSA_FLOAT;
        case SSA_OP_CONST_BOOL: return rt == SSA_BOOL;
        case SSA_OP_PARAM: return nat_scalar(rt);

        case SSA_OP_PHI:
            if (!nat_scalar(rt)) return false;
            /* Phi operands must already agree exactly: ssa_join_result poisons
             * a mixed-type merge to UNKNOWN, so a scalar phi result implies
             * every operand shares that type. Re-checked rather than trusted. */
            for (int i = 0; i < ins->operand_count; i++)
                if (nat_type_of(ins->operands[i]) != rt) return false;
            return true;

        /* Numeric binops: operands must be INT/FLOAT (never BOOL — vm.c's
         * BINARY_OP_NUM would read a bool through the integer union member,
         * which is genuinely unmodeled behavior, not something to replicate).
         * The result type is whatever inference already computed from vm.c's
         * own promotion rules; operands are cast to match it. */
        case SSA_OP_ADD:
        case SSA_OP_SUB:
        case SSA_OP_MUL:
        case SSA_OP_DIV:
        case SSA_OP_CMP_LT:
        case SSA_OP_CMP_GT:
        case SSA_OP_CMP_LE:
        case SSA_OP_CMP_GE:
            return (rt == SSA_INT || rt == SSA_FLOAT) &&
                   (a == SSA_INT || a == SSA_FLOAT) && (b == SSA_INT || b == SSA_FLOAT);

        case SSA_OP_MOD:
        case SSA_OP_BIT_AND:
        case SSA_OP_BIT_OR:
        case SSA_OP_BIT_XOR:
        case SSA_OP_SHL:
        case SSA_OP_SHR:
            return rt == SSA_INT && a == SSA_INT && b == SSA_INT;

        case SSA_OP_CMP_EQ:
        case SSA_OP_CMP_NE:
            /* value_equal() compares tags first, so operands of differing
             * types always answer false at runtime. Rather than emit that
             * constant, keep the subset to the same-type case. */
            return rt == SSA_BOOL && nat_scalar(a) && a == b;

        case SSA_OP_NEG: return (rt == SSA_INT || rt == SSA_FLOAT) && a == rt;
        case SSA_OP_BIT_NOT: return rt == SSA_INT && a == SSA_INT;
        case SSA_OP_NOT: return rt == SSA_BOOL && nat_scalar(a);

        case SSA_OP_JUMP: return ins->targets != NULL;
        case SSA_OP_BRANCH: return ins->targets != NULL && ins->operand_count == 1 && nat_scalar(a);
        case SSA_OP_RETURN: return ins->operand_count == 1 && nat_scalar(a);

        /* CALL_DIRECT is handled by the fixpoint, not here. Everything else —
         * dynamic calls, dispatch, allocations, field/index access, string and
         * nil constants, opaque global/upvalue traffic, nil-coalesce — is
         * outside the subset by construction. */
        default: return false;
    }
}

/* Everything except CALL_DIRECT resolution: the part of eligibility that does
 * not depend on any other function's verdict, so it only has to run once. */
static bool nat_locally_ok(SsaFunction *sfn, ObjFunction *fn) {
    if (!sfn || !fn) return false;
    if (fn->suspends_maybe) return false;
    if (sfn->block_count <= 0 || !sfn->blocks[0]) return false;
    if (fn->arity != sfn->fn_node->fn_decl.param_count) return false;
    /* A C function has exactly one return type. */
    if (!nat_scalar(nat_return_type(sfn))) return false;
    for (int i = 0; i < fn->arity; i++)
        if (!nat_scalar(nat_param_type(sfn, i))) return false;

    bool *seen = nat_reachable_blocks(sfn);
    if (!seen) return false;

    bool ok = true;
    for (int bi = 0; ok && bi < sfn->block_count; bi++) {
        SsaBlock *blk = sfn->blocks[bi];
        if (!seen[blk->id]) continue;

        /* Duplicate predecessor edges would make "which phi operand belongs to
         * this edge" ambiguous — reject rather than guess. */
        for (int i = 0; ok && i < blk->pred_count; i++)
            for (int j = i + 1; j < blk->pred_count; j++)
                if (blk->preds[i] == blk->preds[j]) { ok = false; break; }

        bool terminated = false;
        for (SsaInstr *ins = blk->first; ok && ins; ins = ins->next) {
            if (nat_dead(ins)) continue;
            if (ins->kind == SSA_OP_PHI && ins->operand_count != blk->pred_count) { ok = false; break; }
            if (ins->kind == SSA_OP_PARAM) {
                /* Emission writes one entry guard per PARAM by scanning the
                 * ENTRY block only, and indexes the C parameter list by
                 * int_const. A PARAM outside the entry block, or with an
                 * out-of-range index, would therefore be unboxed with no guard
                 * in front of it and possibly past the end of the argument
                 * window — the one way this backend could read a mistyped
                 * Value. Neither can happen the way ssa_build_function emits
                 * params today; both are checked rather than relied upon. */
                if (blk != sfn->blocks[0]) { ok = false; break; }
                if (ins->int_const < 0 || ins->int_const >= fn->arity) { ok = false; break; }
            }
            /* CALL_DIRECT's own shape is checked here; whether its callee is
             * eligible is settled by the fixpoint. */
            if (ins->kind == SSA_OP_CALL_DIRECT) {
                if (!ins->callee_fn_node || !ins->result || !nat_scalar(nat_type_of(ins->result)))
                    { ok = false; break; }
            } else if (!nat_instr_ok(ins)) { ok = false; break; }

            if (ins->kind == SSA_OP_RETURN) terminated = true;
            if (ins->kind == SSA_OP_JUMP || ins->kind == SSA_OP_BRANCH) terminated = true;
        }
        /* Every reachable block must end in a terminator, and the terminator
         * must be the last instruction — otherwise the lowered C would fall
         * out of a block into whatever label happens to follow it. */
        if (ok && (!terminated || !blk->last ||
                   (blk->last->kind != SSA_OP_RETURN && blk->last->kind != SSA_OP_JUMP &&
                    blk->last->kind != SSA_OP_BRANCH)))
            ok = false;

        /* A jump/branch target must itself be reachable and in range, since
         * emission turns it into a `goto` at a label only emitted for
         * reachable blocks. */
        if (ok && blk->last->targets) {
            int n = blk->last->kind == SSA_OP_BRANCH ? 2 : 1;
            for (int i = 0; i < n; i++) {
                SsaBlock *tgt = blk->last->targets[i];
                if (!tgt || tgt->id < 0 || tgt->id >= sfn->block_count || !seen[tgt->id] ||
                    nat_pred_index(tgt, blk) < 0) { ok = false; break; }
            }
        }
    }

    free(seen);
    return ok;
}

/* Index into plan->ssa for the function compiled from `node`, or -1. */
static int nat_index_of_node(AotNativePlan *plan, ObjFunction **funcs, AstNode *node) {
    if (!node) return -1;
    for (int i = 0; i < plan->fn_count; i++)
        if (funcs[i]->source_node == node) return i;
    return -1;
}

/* True if every CALL_DIRECT in `sfn` targets a still-eligible function with a
 * matching signature. Argument types must match the callee's parameter types
 * EXACTLY — see aot_native.h on why int->float widening at a call boundary
 * would be a real divergence, not a harmless coercion. */
static bool nat_calls_ok(AotNativePlan *plan, ObjFunction **funcs, SsaFunction *sfn) {
    for (int bi = 0; bi < sfn->block_count; bi++) {
        for (SsaInstr *ins = sfn->blocks[bi]->first; ins; ins = ins->next) {
            if (ins->kind != SSA_OP_CALL_DIRECT || nat_dead(ins)) continue;
            int ci = nat_index_of_node(plan, funcs, ins->callee_fn_node);
            if (ci < 0 || !plan->ssa[ci]) return false;
            ObjFunction *callee = funcs[ci];
            if (ins->operand_count != callee->arity) return false;
            if (nat_type_of(ins->result) != plan->return_type[ci]) return false;
            for (int i = 0; i < ins->operand_count; i++)
                if (nat_type_of(ins->operands[i]) != nat_param_type(plan->ssa[ci], i))
                    return false;
        }
    }
    return true;
}

AotNativePlan *aot_native_plan(SsaModule *mod, ObjFunction **funcs, int fn_count) {
    AotNativePlan *plan = calloc(1, sizeof(AotNativePlan));
    if (!plan) return NULL;
    plan->fn_count = fn_count;
    plan->ssa = calloc((size_t)(fn_count > 0 ? fn_count : 1), sizeof(SsaFunction *));
    plan->return_type = calloc((size_t)(fn_count > 0 ? fn_count : 1), sizeof(SsaType));
    if (!plan->ssa || !plan->return_type) { aot_native_plan_free(plan); return NULL; }

    /* Seed with everything that passes the call-independent checks. */
    for (int i = 0; i < fn_count; i++) {
        SsaFunction *sfn = NULL;
        if (mod && funcs[i]->source_node) {
            for (int j = 0; j < mod->count; j++)
                if (mod->functions[j]->fn_node == funcs[i]->source_node) {
                    sfn = mod->functions[j];
                    break;
                }
        }
        if (sfn && nat_locally_ok(sfn, funcs[i])) {
            plan->ssa[i] = sfn;
            plan->return_type[i] = nat_return_type(sfn);
        }
    }

    /* Monotonic demotion fixpoint: "my callee must also be native" is mutually
     * recursive (that is the whole point — recursion is the feature), so start
     * optimistic and demote until nothing eligible calls anything ineligible.
     * Demotion only ever removes candidates, so this terminates; and because
     * the condition is re-checked after the last change, the surviving set is
     * verified at the fixpoint rather than assumed mid-flight. */
    bool changed;
    do {
        changed = false;
        for (int i = 0; i < fn_count; i++) {
            if (!plan->ssa[i]) continue;
            if (!nat_calls_ok(plan, funcs, plan->ssa[i])) {
                plan->ssa[i] = NULL;
                changed = true;
            }
        }
    } while (changed);

    for (int i = 0; i < fn_count; i++)
        if (plan->ssa[i]) plan->count++;
    return plan;
}

void aot_native_plan_free(AotNativePlan *plan) {
    if (!plan) return;
    free(plan->ssa);
    free(plan->return_type);
    free(plan);
}

/* ─── Emission ─── */

/* Emits the C expression for `v`, coerced to `want`. Eligibility guarantees
 * the only coercion ever needed is INT -> FLOAT (vm.c's numeric promotion);
 * call arguments are never coerced at all. */
static void nat_emit_val(FILE *out, SsaValue *v, SsaType want) {
    SsaValue *r = nat_resolve(v);
    if (want == SSA_FLOAT && r->type == SSA_INT) fprintf(out, "(double)v%d", r->id);
    else fprintf(out, "v%d", r->id);
}

/* Truthiness, mirroring value_is_truthy() (vm.c) for the scalar tags the
 * subset admits: int != 0, float != 0.0, bool as-is. */
static void nat_emit_truthy(FILE *out, SsaValue *v) {
    SsaValue *r = nat_resolve(v);
    switch (r->type) {
        case SSA_INT: fprintf(out, "(v%d != 0)", r->id); break;
        case SSA_FLOAT: fprintf(out, "(v%d != 0.0)", r->id); break;
        default: fprintf(out, "v%d", r->id); break;
    }
}

static void nat_emit_binop(FILE *out, SsaInstr *ins, const char *op) {
    SsaType rt = ins->result->type;
    /* Ordering comparisons produce val_int/val_float in vm.c, not val_bool —
     * the C comparison result is cast to the inferred numeric type so the
     * native answer is bit-identical to the boxed one. */
    fprintf(out, "    v%d = (%s)(", ins->result->id, nat_ctype(rt));
    nat_emit_val(out, ins->operands[0], rt);
    fprintf(out, " %s ", op);
    nat_emit_val(out, ins->operands[1], rt);
    fprintf(out, ");\n");
}

static void nat_emit_cmp_eq(FILE *out, SsaInstr *ins, const char *op) {
    SsaType at = nat_type_of(ins->operands[0]);
    fprintf(out, "    v%d = (", ins->result->id);
    nat_emit_val(out, ins->operands[0], at);
    fprintf(out, " %s ", op);
    nat_emit_val(out, ins->operands[1], at);
    fprintf(out, ");\n");
}

/* Unwind out of a native function after an error has already been reported.
 * The scalar return value is meaningless here; every caller re-checks
 * vm->had_error immediately, and task_run's own loop stops on it. Nothing to
 * undo on the way out: the depth budget is a by-value parameter, not a counter
 * that would have to be balanced along every error path. */
static void nat_emit_bail(FILE *out, SsaType rt, const char *indent) {
    fprintf(out, "%sreturn %s;\n", indent, rt == SSA_FLOAT ? "0.0" : "0");
}

/* Parallel copies for the CFG edge `from` -> `to`: every phi in `to` reads its
 * operand for this edge. Sources are staged into temporaries first so the copy
 * set is genuinely parallel — a swap through two phis (`a, b = b, a` across a
 * back edge) would otherwise clobber one of them. */
static void nat_emit_edge_copies(FILE *out, SsaBlock *from, SsaBlock *to, const char *indent) {
    int pi = nat_pred_index(to, from);
    if (pi < 0) return;
    int n = 0;
    for (SsaInstr *ins = to->first; ins; ins = ins->next)
        if (ins->kind == SSA_OP_PHI && !nat_dead(ins)) n++;
    if (n == 0) return;

    fprintf(out, "%s{\n", indent);
    int k = 0;
    for (SsaInstr *ins = to->first; ins; ins = ins->next) {
        if (ins->kind != SSA_OP_PHI || nat_dead(ins)) continue;
        fprintf(out, "%s    %s __phi%d = ", indent, nat_ctype(ins->result->type), k);
        nat_emit_val(out, ins->operands[pi], ins->result->type);
        fprintf(out, ";\n");
        k++;
    }
    k = 0;
    for (SsaInstr *ins = to->first; ins; ins = ins->next) {
        if (ins->kind != SSA_OP_PHI || nat_dead(ins)) continue;
        fprintf(out, "%s    v%d = __phi%d;\n", indent, ins->result->id, k);
        k++;
    }
    fprintf(out, "%s}\n", indent);
}

/* The pure C function: raw scalar parameters, raw scalar return, no CallFrame,
 * no task stack. */
static void nat_emit_native_body(FILE *out, AotNativePlan *plan, ObjFunction **funcs, int index) {
    SsaFunction *sfn = plan->ssa[index];
    ObjFunction *fn = funcs[index];
    SsaType rt = plan->return_type[index];
    bool *seen = nat_reachable_blocks(sfn);
    if (!seen) return;

    fprintf(out, "/* Native body for '%s' (typed, unboxed, native-to-native ABI). */\n",
            fn->source_node && fn->source_node->fn_decl.name ? fn->source_node->fn_decl.name : "?");
    fprintf(out, "static %s varian_nat_fn_%d(VM *vm, int __depth", nat_ctype(rt), index);
    for (int i = 0; i < fn->arity; i++)
        fprintf(out, ", %s p%d", nat_ctype(nat_param_type(sfn, i)), i);
    fprintf(out, ") {\n");

    /* Native calls push no CallFrame, so the interpreter's frame_count check
     * cannot see this recursion; __depth is what stops it from running the C
     * stack off the end. It is a by-value parameter rather than a counter on
     * the VM deliberately: a VM field is a memory round-trip on every call AND
     * return, which measured ~6x slower on call-heavy code, and would need
     * balancing along every error path. A parameter lives in a register, needs
     * no unwind bookkeeping, and is naturally correct per call chain. */
    fprintf(out, "    if (__depth >= TASK_FRAMES_MAX) {\n");
    fprintf(out, "        runtime_error(vm, \"Stack overflow\");\n");
    fprintf(out, "        return %s;\n", rt == SSA_FLOAT ? "0.0" : "0");
    fprintf(out, "    }\n");

    for (int bi = 0; bi < sfn->block_count; bi++) {
        if (!seen[sfn->blocks[bi]->id]) continue;
        for (SsaInstr *ins = sfn->blocks[bi]->first; ins; ins = ins->next) {
            if (!ins->result || nat_dead(ins)) continue;
            fprintf(out, "    %s v%d = 0;\n", nat_ctype(ins->result->type), ins->result->id);
        }
    }
    fprintf(out, "    goto blk_%d;\n", sfn->blocks[0]->id);

    for (int bi = 0; bi < sfn->block_count; bi++) {
        SsaBlock *blk = sfn->blocks[bi];
        if (!seen[blk->id]) continue;
        fprintf(out, "  blk_%d:;\n", blk->id);

        for (SsaInstr *ins = blk->first; ins; ins = ins->next) {
            if (nat_dead(ins)) continue;
            switch (ins->kind) {
                /* Phis are materialized by the copies their predecessors emit
                 * on each incoming edge, not at the phi's own position. */
                case SSA_OP_PHI: break;

                case SSA_OP_CONST_INT:
                    fprintf(out, "    v%d = %lldLL;\n", ins->result->id, (long long)ins->int_const);
                    break;
                case SSA_OP_CONST_FLOAT:
                    fprintf(out, "    v%d = %.17g;\n", ins->result->id, ins->float_const);
                    break;
                case SSA_OP_CONST_BOOL:
                    fprintf(out, "    v%d = %s;\n", ins->result->id, ins->bool_const ? "true" : "false");
                    break;

                case SSA_OP_PARAM:
                    fprintf(out, "    v%d = p%d;\n", ins->result->id, (int)ins->int_const);
                    break;

                case SSA_OP_ADD: nat_emit_binop(out, ins, "+"); break;
                case SSA_OP_SUB: nat_emit_binop(out, ins, "-"); break;
                case SSA_OP_MUL: nat_emit_binop(out, ins, "*"); break;
                case SSA_OP_CMP_LT: nat_emit_binop(out, ins, "<"); break;
                case SSA_OP_CMP_GT: nat_emit_binop(out, ins, ">"); break;
                case SSA_OP_CMP_LE: nat_emit_binop(out, ins, "<="); break;
                case SSA_OP_CMP_GE: nat_emit_binop(out, ins, ">="); break;

                case SSA_OP_DIV:
                    /* Same guard and message as aot_op_div, on the same
                     * int/float split inference already committed to. */
                    fprintf(out, "    if (");
                    nat_emit_val(out, ins->operands[1], ins->result->type);
                    fprintf(out, " == %s) {\n", ins->result->type == SSA_INT ? "0" : "0.0");
                    fprintf(out, "        runtime_error(vm, \"Division by zero\");\n");
                    nat_emit_bail(out, rt, "        ");
                    fprintf(out, "    }\n");
                    nat_emit_binop(out, ins, "/");
                    break;

                case SSA_OP_MOD:
                    fprintf(out, "    if (v%d == 0) {\n", nat_resolve(ins->operands[1])->id);
                    fprintf(out, "        runtime_error(vm, \"Division by zero\");\n");
                    nat_emit_bail(out, rt, "        ");
                    fprintf(out, "    }\n");
                    nat_emit_binop(out, ins, "%");
                    break;

                case SSA_OP_BIT_AND: nat_emit_binop(out, ins, "&"); break;
                case SSA_OP_BIT_OR: nat_emit_binop(out, ins, "|"); break;
                case SSA_OP_BIT_XOR: nat_emit_binop(out, ins, "^"); break;

                case SSA_OP_SHL:
                case SSA_OP_SHR:
                    /* aot_op_shift rejects counts outside [0, 64) before
                     * shifting — the same check, since C leaves an
                     * out-of-range shift undefined. */
                    fprintf(out, "    if (v%d < 0 || v%d >= 64) {\n",
                            nat_resolve(ins->operands[1])->id, nat_resolve(ins->operands[1])->id);
                    fprintf(out, "        runtime_error(vm, \"Shift count must be between 0 and 63\");\n");
                    nat_emit_bail(out, rt, "        ");
                    fprintf(out, "    }\n");
                    if (ins->kind == SSA_OP_SHL)
                        fprintf(out, "    v%d = (int64_t)((uint64_t)v%d << (uint64_t)v%d);\n",
                                ins->result->id, nat_resolve(ins->operands[0])->id,
                                nat_resolve(ins->operands[1])->id);
                    else
                        fprintf(out, "    v%d = v%d >> v%d;\n", ins->result->id,
                                nat_resolve(ins->operands[0])->id,
                                nat_resolve(ins->operands[1])->id);
                    break;

                case SSA_OP_CMP_EQ: nat_emit_cmp_eq(out, ins, "=="); break;
                case SSA_OP_CMP_NE: nat_emit_cmp_eq(out, ins, "!="); break;

                case SSA_OP_NEG:
                    fprintf(out, "    v%d = -v%d;\n", ins->result->id,
                            nat_resolve(ins->operands[0])->id);
                    break;
                case SSA_OP_BIT_NOT:
                    fprintf(out, "    v%d = ~v%d;\n", ins->result->id,
                            nat_resolve(ins->operands[0])->id);
                    break;
                case SSA_OP_NOT:
                    fprintf(out, "    v%d = !", ins->result->id);
                    nat_emit_truthy(out, ins->operands[0]);
                    fprintf(out, ";\n");
                    break;

                case SSA_OP_CALL_DIRECT: {
                    /* A genuine C call. No CallFrame, no boxing, no entry
                     * guard: the argument types were proven by this function's
                     * own SSA typing, not merely declared, and the fixpoint
                     * verified they match the callee's signature exactly. */
                    int ci = nat_index_of_node(plan, funcs, ins->callee_fn_node);
                    fprintf(out, "    v%d = varian_nat_fn_%d(vm, __depth + 1", ins->result->id, ci);
                    for (int i = 0; i < ins->operand_count; i++) {
                        fprintf(out, ", ");
                        nat_emit_val(out, ins->operands[i], nat_param_type(plan->ssa[ci], i));
                    }
                    fprintf(out, ");\n");
                    /* A raw scalar return carries no error channel, so the
                     * flag the callee set is checked here instead — the
                     * native equivalent of task_run's `while (!vm->had_error)`. */
                    fprintf(out, "    if (vm->had_error) {\n");
                    nat_emit_bail(out, rt, "        ");
                    fprintf(out, "    }\n");
                    break;
                }

                case SSA_OP_JUMP:
                    nat_emit_edge_copies(out, blk, ins->targets[0], "    ");
                    fprintf(out, "    goto blk_%d;\n", ins->targets[0]->id);
                    break;

                case SSA_OP_BRANCH:
                    fprintf(out, "    if (");
                    nat_emit_truthy(out, ins->operands[0]);
                    fprintf(out, ") {\n");
                    nat_emit_edge_copies(out, blk, ins->targets[0], "        ");
                    fprintf(out, "        goto blk_%d;\n", ins->targets[0]->id);
                    fprintf(out, "    } else {\n");
                    nat_emit_edge_copies(out, blk, ins->targets[1], "        ");
                    fprintf(out, "        goto blk_%d;\n", ins->targets[1]->id);
                    fprintf(out, "    }\n");
                    break;

                case SSA_OP_RETURN:
                    fprintf(out, "    return ");
                    nat_emit_val(out, ins->operands[0], rt);
                    fprintf(out, ";\n");
                    break;

                default:
                    /* Unreachable: the eligibility pass rejected every other
                     * op kind before emission was ever attempted. */
                    break;
            }
        }
    }

    fprintf(out, "}\n\n");
    free(seen);
}

/* The boxed<->native boundary: unbox guarded arguments, make one C call, box
 * the result, and run the scheduler's ordinary return sequence. */
static void nat_emit_wrapper(FILE *out, AotNativePlan *plan, ObjFunction **funcs, int index) {
    SsaFunction *sfn = plan->ssa[index];
    ObjFunction *fn = funcs[index];
    SsaType rt = plan->return_type[index];
    const char *name = fn->source_node && fn->source_node->fn_decl.name
                     ? fn->source_node->fn_decl.name : "?";

    fprintf(out, "/* Function %d: guarded boxed->native boundary for '%s'. Falls back to\n", index, name);
    fprintf(out, " * varian_aot_fn_%d_boxed on any argument-type mismatch. */\n", index);
    fprintf(out, "void varian_aot_fn_%d(VM *vm, Task *t) {\n", index);
    fprintf(out, "    CallFrame *frame = &t->frames[t->frame_count - 1];\n");

    fprintf(out, "#ifdef VARIAN_SSA_SHADOW_MODE\n");
    fprintf(out, "    /* Inside a shadow reference run: stay entirely boxed. */\n");
    fprintf(out, "    if (vm->ssa_shadow_ref) { varian_aot_fn_%d_boxed(vm, t); return; }\n", index);
    fprintf(out, "#endif\n");

    /* A non-suspending body is never re-entered mid-function, but the boxed
     * body it delegates to can be (it makes trampolined calls), so a non-zero
     * resumption pc must route there rather than restart natively. */
    fprintf(out, "    if (frame->ip != frame->function->code) { varian_aot_fn_%d_boxed(vm, t); return; }\n",
            index);

    /* Entry guards: one runtime tag check per parameter. Never elided — a
     * declared (or parser-defaulted) annotation is a belief, not a contract. */
    for (int i = 0; i < fn->arity; i++) {
        SsaType pt = nat_param_type(sfn, i);
        const char *tag = pt == SSA_INT ? "VAL_INT" : pt == SSA_FLOAT ? "VAL_FLOAT" : "VAL_BOOL";
        fprintf(out, "    if (frame->slots[%d].type != %s) { varian_aot_fn_%d_boxed(vm, t); return; }\n",
                i, tag, index);
    }

    /* Shadow mode: run the boxed reference body first over a snapshot of all
     * task state it can touch, then restore and run natively for real.
     *
     * A boxed body containing a call does NOT run to completion — it pushes a
     * CallFrame and returns to the scheduler — so the harness drives the
     * trampoline itself until the frame it started from has returned.
     * VM.ssa_shadow_ref is held for the duration so every nested wrapper goes
     * straight to its boxed body: that keeps the reference run genuinely
     * all-boxed, and keeps its cost linear rather than exponential in call
     * depth.
     *
     * The argument slots must be saved and restored BY VALUE, not just the
     * stack pointer: a function that assigns to its own parameter (`n = n - 1`
     * in a loop) mutates frame->slots[] in place, so the reference run would
     * otherwise hand the native run already-consumed arguments and manufacture
     * a divergence that does not exist in a real build. */
    fprintf(out, "#ifdef VARIAN_SSA_SHADOW_MODE\n");
    fprintf(out, "    Value __shadow_ref = val_nil();\n");
    fprintf(out, "    int __shadow_valid = 0;\n");
    fprintf(out, "    {\n");
    fprintf(out, "        int __top = t->stack_top, __fc = t->frame_count;\n");
    fprintf(out, "        bool __dead = t->dead, __cache = t->cache_on_return;\n");
    fprintf(out, "        Value __res = t->result;\n");
    fprintf(out, "        CallFrame __fr = *frame;\n");
    if (fn->arity > 0) {
        fprintf(out, "        Value __args[%d];\n", fn->arity);
        fprintf(out, "        for (int __i = 0; __i < %d; __i++) __args[__i] = frame->slots[__i];\n",
                fn->arity);
    }
    fprintf(out, "        vm->ssa_shadow_ref++;\n");
    fprintf(out, "        varian_aot_fn_%d_boxed(vm, t);\n", index);
    fprintf(out, "        while (t->frame_count >= __fc && !vm->had_error && !t->dead && !t->yielded) {\n");
    fprintf(out, "            CallFrame *__f = &t->frames[t->frame_count - 1];\n");
    fprintf(out, "            if (!__f->function->aot_func) break;\n");
    fprintf(out, "            __f->function->aot_func(vm, t);\n");
    fprintf(out, "        }\n");
    fprintf(out, "        vm->ssa_shadow_ref--;\n");
    fprintf(out, "        if (!vm->had_error && t->frame_count == __fc - 1 && t->stack_top > 0) {\n");
    fprintf(out, "            __shadow_ref = t->stack[t->stack_top - 1];\n");
    fprintf(out, "            __shadow_valid = 1;\n");
    fprintf(out, "        }\n");
    fprintf(out, "        t->stack_top = __top; t->frame_count = __fc;\n");
    fprintf(out, "        t->dead = __dead; t->cache_on_return = __cache; t->result = __res;\n");
    fprintf(out, "        t->frames[t->frame_count - 1] = __fr;\n");
    fprintf(out, "        frame = &t->frames[t->frame_count - 1];\n");
    if (fn->arity > 0)
        fprintf(out, "        for (int __i = 0; __i < %d; __i++) frame->slots[__i] = __args[__i];\n",
                fn->arity);
    fprintf(out, "    }\n");
    fprintf(out, "#endif\n");

    /* Seed the depth budget from the interpreter's own frame count, so native
     * recursion draws on the same allowance rather than a second one. */
    fprintf(out, "    %s __r = varian_nat_fn_%d(vm, t->frame_count", nat_ctype(rt), index);
    for (int i = 0; i < fn->arity; i++) {
        SsaType pt = nat_param_type(sfn, i);
        const char *member = pt == SSA_INT ? "integer" : pt == SSA_FLOAT ? "floating" : "boolean";
        fprintf(out, ", frame->slots[%d].as.%s", i, member);
    }
    fprintf(out, ");\n");
    fprintf(out, "    if (vm->had_error) return;\n");

    fprintf(out, "    {\n");
    fprintf(out, "        Value result = %s(__r);\n",
            rt == SSA_INT ? "val_int" : rt == SSA_FLOAT ? "val_float" : "val_bool");
    fprintf(out, "#ifdef VARIAN_SSA_SHADOW_MODE\n");
    fprintf(out, "        aot_native_shadow_check(vm, \"%s\", result, __shadow_ref, __shadow_valid);\n",
            name);
    fprintf(out, "#endif\n");
    fprintf(out, "        if (t->cache_on_return) {\n");
    fprintf(out, "            cache_map_put(vm, t->cache_result_key, result);\n");
    fprintf(out, "            t->cache_on_return = false;\n");
    fprintf(out, "        }\n");
    fprintf(out, "        CallFrame *curr_frame = &t->frames[t->frame_count - 1];\n");
    if (fn->is_module_init)
        fprintf(out, "        close_upvalues(vm, curr_frame);\n");
    fprintf(out, "        int base = curr_frame->return_base;\n");
    fprintf(out, "        t->frame_count--;\n");
    fprintf(out, "        if (t->frame_count == 0) {\n");
    fprintf(out, "            t->result = result;\n");
    fprintf(out, "            t->dead = true;\n");
    fprintf(out, "            t->stack[t->stack_top++] = result;\n");
    fprintf(out, "            return;\n");
    fprintf(out, "        }\n");
    fprintf(out, "        t->stack_top = base;\n");
    fprintf(out, "        t->stack[t->stack_top++] = result;\n");
    fprintf(out, "    }\n");
    fprintf(out, "}\n\n");
}

void aot_native_emit_prelude(FILE *out, AotNativePlan *plan, ObjFunction **funcs) {
    fprintf(out, "#ifdef VARIAN_SSA_SHADOW_MODE\n");
    fprintf(out, "/* Differential verification: compares a native body's result against the\n");
    fprintf(out, " * boxed reference body's result and aborts loudly on any divergence. */\n");
    fprintf(out, "static void aot_native_shadow_check(VM *vm, const char *name, Value got,\n");
    fprintf(out, "                                    Value expect, int valid) {\n");
    fprintf(out, "    if (!valid || vm->had_error) return;\n");
    fprintf(out, "    if (got.type == expect.type) {\n");
    fprintf(out, "        if (got.type == VAL_INT   && got.as.integer  == expect.as.integer)  return;\n");
    fprintf(out, "        if (got.type == VAL_FLOAT && got.as.floating == expect.as.floating) return;\n");
    fprintf(out, "        if (got.type == VAL_BOOL  && got.as.boolean  == expect.as.boolean)  return;\n");
    fprintf(out, "    }\n");
    fprintf(out, "    fprintf(stderr, \"[Kiln] SSA SHADOW DIVERGENCE in '%%s': native tag=%%d \"\n");
    fprintf(out, "                    \"i=%%lld f=%%g vs boxed tag=%%d i=%%lld f=%%g\\n\",\n");
    fprintf(out, "            name, (int)got.type, (long long)got.as.integer, got.as.floating,\n");
    fprintf(out, "            (int)expect.type, (long long)expect.as.integer, expect.as.floating);\n");
    fprintf(out, "    abort();\n");
    fprintf(out, "}\n");
    fprintf(out, "#endif\n\n");

    if (plan->count == 0) return;
    fprintf(out, "/* Native function prototypes (declared up front for mutual recursion) */\n");
    for (int i = 0; i < plan->fn_count; i++) {
        if (!plan->ssa[i]) continue;
        fprintf(out, "static %s varian_nat_fn_%d(VM *vm, int", nat_ctype(plan->return_type[i]), i);
        for (int p = 0; p < funcs[i]->arity; p++)
            fprintf(out, ", %s", nat_ctype(nat_param_type(plan->ssa[i], p)));
        fprintf(out, ");\n");
    }
    fprintf(out, "\n");
}

void aot_native_emit_function(FILE *out, AotNativePlan *plan, ObjFunction **funcs, int index) {
    if (!plan->ssa[index]) return;
    nat_emit_native_body(out, plan, funcs, index);
    nat_emit_wrapper(out, plan, funcs, index);
}
