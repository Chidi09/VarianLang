#include "suspend_analysis.h"
#include <stdlib.h>
#include <string.h>

/* ─── Collection pass: find every function (and FFI) declaration in the
 * program, top-level or nested. Function declarations can only appear as
 * statements inside a program/block-like container in Varian (there is no
 * anonymous function-literal expression node — nested/local functions and
 * closures are always named `fn` statements), so this walker only needs to
 * follow statement containers, not every expression shape. ─── */

static void suspend_grow(SuspendAnalysis *sa) {
    if (sa->count < sa->capacity) return;
    sa->capacity = sa->capacity ? sa->capacity * 2 : 16;
    sa->functions = realloc(sa->functions, (size_t)sa->capacity * sizeof(SuspendFunc));
}

static void suspend_register(SuspendAnalysis *sa, AstNode *node, const char *name) {
    suspend_grow(sa);
    sa->functions[sa->count].fn_node = node;
    sa->functions[sa->count].name = name;
    sa->functions[sa->count].suspends = false;
    sa->count++;
}

static void suspend_collect(AstNode *node, SuspendAnalysis *sa) {
    if (!node) return;
    switch (node->kind) {
        case NODE_PROGRAM:
            for (int i = 0; i < node->program.stmt_count; i++)
                suspend_collect(node->program.stmts[i], sa);
            break;
        case NODE_BLOCK:
            for (int i = 0; i < node->block.stmt_count; i++)
                suspend_collect(node->block.stmts[i], sa);
            break;
        case NODE_IF:
            suspend_collect(node->if_stmt.then_branch, sa);
            suspend_collect(node->if_stmt.else_branch, sa);
            break;
        case NODE_WHILE:
            suspend_collect(node->while_stmt.body, sa);
            break;
        case NODE_FOR:
            suspend_collect(node->for_stmt.body, sa);
            break;
        case NODE_LOOP:
            suspend_collect(node->loop_stmt.body, sa);
            break;
        case NODE_TRY:
            suspend_collect(node->try_stmt.try_body, sa);
            suspend_collect(node->try_stmt.catch_body, sa);
            break;
        case NODE_MATCH:
            for (int i = 0; i < node->match_stmt.arm_count; i++)
                suspend_collect(node->match_stmt.arms[i], sa);
            break;
        case NODE_MATCH_ARM:
            suspend_collect(node->match_arm.body, sa);
            break;
        case NODE_COMPTIME:
            suspend_collect(node->comptime.body, sa);
            break;
        case NODE_TEST:
            suspend_collect(node->test_decl.body, sa);
            break;
        case NODE_FN_DECL:
            suspend_register(sa, node, node->fn_decl.name);
            suspend_collect(node->fn_decl.body, sa);
            break;
        case NODE_FFI_DECL:
            suspend_register(sa, node, node->ffi_decl.name);
            break;
        default:
            break;
    }
}

/* ─── Per-owner suspend scan: does executing `owner`'s body ever reach a
 * suspending operation? Mirrors the shape of aot.c's release_scan_node, with
 * two departures: a nested NODE_FN_DECL is NOT descended into (defining a
 * local function/closure doesn't execute it — only a later call does, which
 * is handled separately by name), and every call/dispatch/await/channel op
 * is checked against the conservative suspend rules below. ─── */

static void suspend_scan_node(AstNode *node, SuspendAnalysis *sa, SuspendFunc *owner);

static void suspend_mark_owner(SuspendAnalysis *sa, SuspendFunc *owner) {
    if (!owner->suspends) {
        owner->suspends = true;
        sa->changed = true;
    }
}

/* True if `name` matches at least one collected declaration, and at least one
 * matching declaration currently has `suspends == true` (FFI entries never
 * do). Ambiguous same-name matches (shadowing) resolve toward the
 * conservative direction: any match being true is enough to propagate. */
static bool suspend_name_resolves_to_suspending(SuspendAnalysis *sa, const char *name,
                                                 bool *found_any) {
    bool any_true = false;
    *found_any = false;
    if (!name) return false;
    for (int i = 0; i < sa->count; i++) {
        if (strcmp(sa->functions[i].name, name) == 0) {
            *found_any = true;
            if (sa->functions[i].suspends) any_true = true;
        }
    }
    return any_true;
}

static void suspend_scan_many(AstNode **nodes, int count, SuspendAnalysis *sa,
                               SuspendFunc *owner) {
    for (int i = 0; i < count; i++) suspend_scan_node(nodes[i], sa, owner);
}

static void suspend_scan_node(AstNode *node, SuspendAnalysis *sa, SuspendFunc *owner) {
    if (!node) return;
    switch (node->kind) {
        case NODE_PROGRAM:
            suspend_scan_many(node->program.stmts, node->program.stmt_count, sa, owner);
            break;
        case NODE_BLOCK:
            suspend_scan_many(node->block.stmts, node->block.stmt_count, sa, owner);
            break;
        case NODE_LET_DECL:
        case NODE_CONST_DECL:
            suspend_scan_node(node->let_decl.initializer, sa, owner);
            break;
        case NODE_FN_DECL:
            /* Merely declaring a nested function/closure doesn't execute it;
             * its own suspend status is computed separately under its own
             * owner. A call to it (by name) is what matters here. */
            break;
        case NODE_EXPR_STMT:
            suspend_scan_node(node->expr_stmt.expr, sa, owner);
            break;
        case NODE_IF:
            suspend_scan_node(node->if_stmt.condition, sa, owner);
            suspend_scan_node(node->if_stmt.then_branch, sa, owner);
            suspend_scan_node(node->if_stmt.else_branch, sa, owner);
            break;
        case NODE_WHILE:
            suspend_scan_node(node->while_stmt.condition, sa, owner);
            suspend_scan_node(node->while_stmt.body, sa, owner);
            break;
        case NODE_FOR:
            suspend_scan_node(node->for_stmt.iterable, sa, owner);
            suspend_scan_node(node->for_stmt.body, sa, owner);
            break;
        case NODE_LOOP:
            suspend_scan_node(node->loop_stmt.body, sa, owner);
            break;
        case NODE_RETURN:
            suspend_scan_many(node->return_stmt.values, node->return_stmt.value_count, sa, owner);
            break;
        case NODE_ASSIGN:
            suspend_scan_node(node->assign.target, sa, owner);
            suspend_scan_node(node->assign.value, sa, owner);
            break;
        case NODE_BINARY:
            suspend_scan_node(node->binary.left, sa, owner);
            suspend_scan_node(node->binary.right, sa, owner);
            break;
        case NODE_UNARY:
            suspend_scan_node(node->unary.operand, sa, owner);
            break;
        case NODE_CALL: {
            AstNode *callee = node->call.callee;
            if (callee && callee->kind == NODE_IDENTIFIER) {
                bool found = false;
                bool resolves_suspending =
                    suspend_name_resolves_to_suspending(sa, callee->identifier.name, &found);
                /* Unresolved callee (not a known fn/ffi decl — e.g. dynamic
                 * name, prelude builtin/native) => conservatively suspends. */
                if (!found || resolves_suspending) suspend_mark_owner(sa, owner);
            } else {
                /* Call through a non-identifier expression (index, member,
                 * another call's result, ...) can't be statically resolved. */
                suspend_mark_owner(sa, owner);
                suspend_scan_node(callee, sa, owner);
            }
            suspend_scan_many(node->call.args, node->call.arg_count, sa, owner);
            break;
        }
        case NODE_INDEX:
            suspend_scan_node(node->index.object, sa, owner);
            suspend_scan_node(node->index.index, sa, owner);
            break;
        case NODE_MEMBER:
        case NODE_QUESTION_DOT:
            suspend_scan_node(node->member.object, sa, owner);
            break;
        case NODE_IDENTIFIER:
            /* A bare reference to a function value (not calling it) doesn't
             * itself suspend the referencing function. */
            break;
        case NODE_INTERPOLATED_STRING:
            suspend_scan_many(node->interpolated_string.parts,
                               node->interpolated_string.part_count, sa, owner);
            break;
        case NODE_ARRAY_LITERAL:
        case NODE_TUPLE_LITERAL:
            suspend_scan_many(node->array_literal.elements, node->array_literal.element_count,
                               sa, owner);
            break;
        case NODE_STRUCT_LITERAL:
            suspend_scan_many(node->struct_literal.field_values, node->struct_literal.field_count,
                               sa, owner);
            break;
        case NODE_ENUM_LITERAL:
            suspend_scan_many(node->enum_literal.values, node->enum_literal.value_count, sa, owner);
            break;
        case NODE_MATCH:
            suspend_scan_node(node->match_stmt.value, sa, owner);
            suspend_scan_many(node->match_stmt.arms, node->match_stmt.arm_count, sa, owner);
            break;
        case NODE_MATCH_ARM:
            suspend_scan_node(node->match_arm.pattern, sa, owner);
            suspend_scan_node(node->match_arm.body, sa, owner);
            break;
        case NODE_CHAN_SEND:
            suspend_mark_owner(sa, owner);
            suspend_scan_node(node->chan_send.channel, sa, owner);
            suspend_scan_node(node->chan_send.value, sa, owner);
            break;
        case NODE_CHAN_RECEIVE:
            suspend_mark_owner(sa, owner);
            suspend_scan_node(node->chan_receive.channel, sa, owner);
            break;
        case NODE_AWAIT:
            suspend_mark_owner(sa, owner);
            suspend_scan_node(node->await.expr, sa, owner);
            break;
        case NODE_ASSERT:
            suspend_scan_node(node->assert_stmt.condition, sa, owner);
            break;
        case NODE_TEST:
            suspend_scan_node(node->test_decl.body, sa, owner);
            break;
        case NODE_PROPAGATE:
            suspend_scan_node(node->propagate.expr, sa, owner);
            break;
        case NODE_TRY:
            suspend_scan_node(node->try_stmt.try_body, sa, owner);
            suspend_scan_node(node->try_stmt.catch_body, sa, owner);
            break;
        case NODE_COMPTIME:
            suspend_scan_node(node->comptime.body, sa, owner);
            break;
        case NODE_DISPATCH_CALL:
            /* Runtime method dispatch (includes actor message sends) can't be
             * statically resolved to a single known function. */
            suspend_mark_owner(sa, owner);
            suspend_scan_node(node->dispatch_call.object, sa, owner);
            suspend_scan_many(node->dispatch_call.args, node->dispatch_call.arg_count, sa, owner);
            break;
        case NODE_INT_LITERAL:
        case NODE_FLOAT_LITERAL:
        case NODE_STRING_LITERAL:
        case NODE_BOOL_LITERAL:
        case NODE_NULL_LITERAL:
        case NODE_BREAK:
        case NODE_CONTINUE:
        case NODE_ACTOR_DECL:
        case NODE_STRUCT_DECL:
        case NODE_SCHEMA_DECL:
        case NODE_ENUM_DECL:
        case NODE_TRAIT_DECL:
        case NODE_FFI_DECL:
            break;
    }
}

SuspendAnalysis *suspend_analyze(AstNode *program) {
    SuspendAnalysis *sa = calloc(1, sizeof(SuspendAnalysis));
    if (!sa) return NULL;

    suspend_collect(program, sa);

    do {
        sa->changed = false;
        for (int i = 0; i < sa->count; i++) {
            if (sa->functions[i].fn_node->kind != NODE_FN_DECL) continue; /* FFI: no body */
            suspend_scan_node(sa->functions[i].fn_node->fn_decl.body, sa, &sa->functions[i]);
        }
    } while (sa->changed);

    return sa;
}

void suspend_analysis_free(SuspendAnalysis *sa) {
    if (!sa) return;
    free(sa->functions);
    free(sa);
}

bool suspend_analysis_get(SuspendAnalysis *sa, AstNode *fn_node) {
    if (!sa) return true;
    for (int i = 0; i < sa->count; i++)
        if (sa->functions[i].fn_node == fn_node) return sa->functions[i].suspends;
    return true; /* fail-safe: not analyzed => assume it can suspend */
}
