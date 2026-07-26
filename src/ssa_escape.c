#include "ssa.h"
#include "vm.h"
#include <stdlib.h>

/* Intraprocedural escape analysis — see the doc comment on ssa_escape_analyze
 * in ssa.h for the policy summary. This file implements it as a single,
 * whole-function, monotonic-growth fixpoint over VALUES (not just allocation
 * sites), mirroring the exact "start optimistic, only ever grow a `true`
 * fact, rescan to a fixpoint" shape already used by suspend_analysis.c and
 * aot.c's release_prune_unused_functions: every value starts `escaping =
 * false`, and a small set of explicit rules can only ever flip it to `true`,
 * never back. Tracking escaping-ness per VALUE (not just per allocation)
 * is what makes the transitive rules (rule 2: stored into an escaping
 * container; phi propagation) fall out naturally — an allocation's result
 * is just one particular value among many that this same fixpoint already
 * covers uniformly.
 *
 * Note on why channel-send/actor-mailbox (the plan's rule 5) has no explicit
 * case below: this pass only ever runs on functions suspend_analyze already
 * proved non-suspending (enforced structurally by ssa_build's caller, which
 * only invokes ssa_build_function for such functions), and a non-suspending
 * function's body can't contain a channel-send/receive or actor call by
 * construction (those unconditionally force SUSPENDS — see
 * suspend_analysis.c). ssa_build_stmt/ssa_build_expr also have no dedicated
 * lowering for those node kinds; if one ever slipped through, it would fall
 * to the generic opaque-value/opaque-effect path, which this pass already
 * treats conservatively (see SSA_OP_UNKNOWN_EFFECT below). */

static SsaValue *ssa_escape_resolve(SsaValue *v) {
    while (v && v->replaced_by) v = v->replaced_by;
    return v;
}

static size_t ssa_round8(size_t n) { return (n + 7) & ~(size_t)7; }

/* Mirrors new_struct's exact byte-size formula (vm.c) so a future native
 * codegen's C-stack-local layout agrees with the runtime's own heap layout
 * for the same field count — deliberately not reinvented independently. */
static int ssa_alloc_size_hint(SsaInstr *ins) {
    size_t fc = (size_t)(ins->operand_count > 0 ? ins->operand_count : 0);
    switch (ins->kind) {
        case SSA_OP_ALLOC_STRUCT: {
            size_t hsize = ssa_round8(sizeof(ObjStruct));
            size_t vsize = ssa_round8(fc * sizeof(Value));
            size_t avsize = ssa_round8(fc * sizeof(ValidationRule *));
            size_t acsize = ssa_round8(fc * sizeof(int));
            return (int)(hsize + vsize + avsize + acsize);
        }
        case SSA_OP_ALLOC_ARRAY: {
            size_t hsize = ssa_round8(sizeof(ObjArray));
            size_t vsize = ssa_round8(fc * sizeof(Value));
            return (int)(hsize + vsize);
        }
        case SSA_OP_ALLOC_TUPLE: {
            size_t hsize = ssa_round8(sizeof(ObjTuple));
            size_t vsize = ssa_round8(fc * sizeof(Value));
            return (int)(hsize + vsize);
        }
        default:
            return 0; /* ALLOC_CLOSURE: never actually emitted by ssa_build (see ssa.c) */
    }
}

static bool ssa_is_alloc(SsaOpKind k) {
    return k == SSA_OP_ALLOC_STRUCT || k == SSA_OP_ALLOC_ARRAY || k == SSA_OP_ALLOC_TUPLE ||
           k == SSA_OP_ALLOC_CLOSURE;
}

static void ssa_escape_mark(bool *escaping, int value_count, SsaValue *v, bool *changed) {
    SsaValue *r = ssa_escape_resolve(v);
    if (!r || r->id < 0 || r->id >= value_count) return;
    if (!escaping[r->id]) {
        escaping[r->id] = true;
        *changed = true;
    }
}

static void ssa_escape_analyze_function(SsaFunction *fn) {
    if (fn->value_count == 0) return;
    bool *escaping = calloc((size_t)fn->value_count, sizeof(bool));

    bool changed;
    do {
        changed = false;
        for (int bi = 0; bi < fn->block_count; bi++) {
            for (SsaInstr *ins = fn->blocks[bi]->first; ins; ins = ins->next) {
                switch (ins->kind) {
                    case SSA_OP_RETURN:
                        /* v1 policy: any returned value always escapes — no
                         * interprocedural "caller keeps it local" analysis. */
                        for (int i = 0; i < ins->operand_count; i++)
                            ssa_escape_mark(escaping, fn->value_count, ins->operands[i], &changed);
                        break;

                    case SSA_OP_CALL_DIRECT:
                    case SSA_OP_CALL_DYNAMIC:
                    case SSA_OP_DISPATCH:
                        /* v1 policy: ANY call escapes ALL of its operands,
                         * unconditionally — including a dispatch call's
                         * receiver (operands[0]), which is effectively an
                         * implicit first argument. Deliberately the most
                         * conservative rule in the pass: see ssa.h's doc
                         * comment on ssa_escape_analyze for why. */
                        for (int i = 0; i < ins->operand_count; i++)
                            ssa_escape_mark(escaping, fn->value_count, ins->operands[i], &changed);
                        break;

                    case SSA_OP_UNKNOWN_EFFECT:
                        /* Covers global/upvalue writes (rule 1) and anything
                         * else this IR doesn't model precisely — always
                         * conservative by construction (see ssa_assign_to
                         * in ssa.c, the only place that emits this). */
                        for (int i = 0; i < ins->operand_count; i++)
                            ssa_escape_mark(escaping, fn->value_count, ins->operands[i], &changed);
                        break;

                    case SSA_OP_FIELD_SET:
                    case SSA_OP_INDEX_SET: {
                        /* Rule 2 (transitive): the value being stored only
                         * escapes if the CONTAINER it's stored into already
                         * escapes. Since "already escapes" can itself only
                         * become true later in the fixpoint (e.g. the
                         * container is returned on a later pass), rescanning
                         * every instruction every iteration (rather than a
                         * one-shot pass) is what lets this propagate
                         * correctly regardless of instruction order. */
                        SsaValue *obj = ssa_escape_resolve(ins->operands[0]);
                        SsaValue *val = ssa_escape_resolve(ins->operands[ins->operand_count - 1]);
                        if (obj && val && obj->id >= 0 && obj->id < fn->value_count && escaping[obj->id])
                            ssa_escape_mark(escaping, fn->value_count, val, &changed);
                        break;
                    }

                    case SSA_OP_PHI:
                        /* If a phi's merged result escapes, every input that
                         * feeds it must be treated as escaping too — we
                         * can't statically tell which input actually flows
                         * out at runtime. */
                        if (ins->result && ins->result->id >= 0 && ins->result->id < fn->value_count &&
                            escaping[ins->result->id]) {
                            for (int i = 0; i < ins->operand_count; i++)
                                ssa_escape_mark(escaping, fn->value_count, ins->operands[i], &changed);
                        }
                        break;

                    default:
                        /* Plain reads (FIELD_GET/INDEX_GET on the object
                         * being read FROM), arithmetic, constants, params,
                         * branches — none of these leak an allocation. */
                        break;
                }
            }
        }
    } while (changed);

    for (int bi = 0; bi < fn->block_count; bi++) {
        for (SsaInstr *ins = fn->blocks[bi]->first; ins; ins = ins->next) {
            if (!ssa_is_alloc(ins->kind) || !ins->result) continue;
            bool esc = ins->result->id >= 0 && ins->result->id < fn->value_count &&
                       escaping[ins->result->id];
            ins->proven_non_escaping = !esc;
            ins->scope_size_hint = esc ? 0 : ssa_alloc_size_hint(ins);
        }
    }

    free(escaping);
}

void ssa_escape_analyze(SsaModule *mod) {
    if (!mod) return;
    for (int f = 0; f < mod->count; f++) ssa_escape_analyze_function(mod->functions[f]);
}
