#include "semantic.h"
#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* Builtin functions, types, and native C modules registered by Varian VM.
 * Semantic analysis is an independent AST phase running prior to VM execution.
 * Native builtins and C modules registered during VM initialization (src/vm.c
 * and src/lib_*.c) are centralized here to allow symbol resolution before runtime. */
static const char *builtins[] = {
    /* Builtin functions */
    "print", "throw", "ffi_to_string", "assert_eq", "assert_ne", "assert_throws",
    "json_encode", "json_decode", "_lumen_escape_str", "__lumen_log_start",
    "__lumen_log_drain", "__test_enable_arena", "__test_recycle_arena",
    "new_app", "channel", "task_spawn", "len", "type_of", "str", "int", "float",
    "bool", "assert", "exit", "copy", "clone", "fetch", "JSON",
    /* Builtin types / module names */
    "String", "Array", "Object", "Task", "Channel", "Response",
    "auth", "env", "errors", "http", "io", "math", "mock", "postgres",
    "python", "redis", "regex", "_sanitize", "smtp", "sqlite", "task", "time", "_validate",
    "db", "lumen", "shield", "zenith", "ai", "cache", "config", "crypto", "csv",
    "event", "feature", "i18n", "mail", "migration", "observe", "pagination",
    "queue", "ratelimit", "seo", "storage", "ws",
    NULL
};

static bool is_builtin(const char *name) {
    if (!name) return false;
    for (int i = 0; builtins[i] != NULL; i++) {
        if (strcmp(builtins[i], name) == 0) return true;
    }
    return false;
}

typedef enum {
    SCOPE_PROGRAM,
    SCOPE_FUNCTION,
    SCOPE_BLOCK
} ScopeKind;

typedef struct Symbol {
    char name[128];
    SourceLoc loc;
    bool is_const;
    bool is_fn;
    bool is_prelude;
    int param_count; /* -1 if not known */
    Type *type;
} Symbol;

typedef struct Scope Scope;
struct Scope {
    ScopeKind kind;
    Scope *parent;
    Symbol *symbols;
    int symbol_count;
    int symbol_capacity;
};

typedef struct {
    SemanticResult *result;
    Scope *current_scope;
    Scope *program_scope;
    int loop_depth;
    int fn_depth;
    const char *current_impl_type;
} Analyzer;

static void report_error(Analyzer *a, SourceLoc loc, const char *code, const char *fmt, ...) {
    if (a->result->count >= a->result->capacity) {
        int new_cap = a->result->capacity == 0 ? 16 : a->result->capacity * 2;
        a->result->diagnostics = (SemanticDiagnostic *)realloc(a->result->diagnostics, sizeof(SemanticDiagnostic) * new_cap);
        a->result->capacity = new_cap;
    }
    SemanticDiagnostic *diag = &a->result->diagnostics[a->result->count++];
    diag->filename = loc.filename ? strdup(loc.filename) : strdup("<unknown>");
    diag->line = loc.line;
    diag->column = loc.column;
    diag->offset = loc.offset;
    strncpy(diag->code, code, sizeof(diag->code) - 1);
    diag->code[sizeof(diag->code) - 1] = '\0';

    va_list args;
    va_start(args, fmt);
    vsnprintf(diag->message, sizeof(diag->message), fmt, args);
    va_end(args);

    a->result->had_error = true;
}

static Scope *push_scope(Analyzer *a, ScopeKind kind) {
    Scope *s = (Scope *)calloc(1, sizeof(Scope));
    s->kind = kind;
    s->parent = a->current_scope;
    s->symbol_capacity = 32;
    s->symbols = (Symbol *)calloc(s->symbol_capacity, sizeof(Symbol));
    a->current_scope = s;
    return s;
}

static void pop_scope(Analyzer *a) {
    if (!a->current_scope) return;
    Scope *s = a->current_scope;
    a->current_scope = s->parent;
    free(s->symbols);
    free(s);
}

static Symbol *find_symbol_in_scope(Scope *scope, const char *name) {
    if (!scope || !name) return NULL;
    for (int i = 0; i < scope->symbol_count; i++) {
        if (strcmp(scope->symbols[i].name, name) == 0) {
            return &scope->symbols[i];
        }
    }
    return NULL;
}

static Symbol *lookup_symbol(Analyzer *a, const char *name) {
    for (Scope *s = a->current_scope; s != NULL; s = s->parent) {
        for (int i = s->symbol_count - 1; i >= 0; i--) {
            if (strcmp(s->symbols[i].name, name) == 0) return &s->symbols[i];
        }
    }
    return NULL;
}

static bool add_symbol(Analyzer *a, const char *name, SourceLoc loc, bool is_const, bool is_fn, int param_count, Type *type, bool is_param) {
    if (!name || !a->current_scope) return false;
    /* Wildcard or synthetic lambdas */
    if (strcmp(name, "_") == 0 || strncmp(name, "__lambda__", 10) == 0) return true;

    /* Check duplicate in current scope only */
    Symbol *existing = find_symbol_in_scope(a->current_scope, name);
    bool from_prelude = loc.filename && strcmp(loc.filename, "<prelude>") == 0;
    if (existing && existing->is_prelude && !from_prelude &&
        a->current_scope->kind == SCOPE_PROGRAM) {
        existing = NULL; /* user declarations intentionally shadow the injected prelude */
    }
    if (existing) {
        /* Allow top-level program scope redeclarations across modules */
        if (a->current_scope == a->program_scope) {
            if (param_count >= 0) existing->param_count = param_count;
            if (is_fn) existing->is_fn = is_fn;
            return true;
        }
        if (is_param) {
            report_error(a, loc, SEM_CODE_DUP_PARAM, "Duplicate parameter '%s'", name);
        } else {
            report_error(a, loc, SEM_CODE_DUP_DECL, "Duplicate declaration of '%s' in the same scope", name);
        }
        return false;
    }

    if (a->current_scope->symbol_count >= a->current_scope->symbol_capacity) {
        int new_cap = a->current_scope->symbol_capacity * 2;
        a->current_scope->symbols = (Symbol *)realloc(a->current_scope->symbols, sizeof(Symbol) * new_cap);
        memset(a->current_scope->symbols + a->current_scope->symbol_capacity, 0, sizeof(Symbol) * (new_cap - a->current_scope->symbol_capacity));
        a->current_scope->symbol_capacity = new_cap;
    }

    Symbol *sym = &a->current_scope->symbols[a->current_scope->symbol_count++];
    strncpy(sym->name, name, sizeof(sym->name) - 1);
    sym->name[sizeof(sym->name) - 1] = '\0';
    sym->loc = loc;
    sym->is_const = is_const;
    sym->is_fn = is_fn;
    sym->is_prelude = loc.filename && strcmp(loc.filename, "<prelude>") == 0;
    sym->param_count = param_count;
    sym->type = type;
    return true;
}

/* Forward declarations for AST traversal */
static void analyze_node(Analyzer *a, AstNode *node);
static void analyze_expr(Analyzer *a, AstNode *node);

/* Top-level symbol collection (Pass 1) */
static void collect_top_level_symbol(Analyzer *a, AstNode *stmt) {
    if (!stmt) return;
    switch (stmt->kind) {
        case NODE_BLOCK:
            if (stmt->block.stmts) {
                for (int i = 0; i < stmt->block.stmt_count; i++) {
                    collect_top_level_symbol(a, stmt->block.stmts[i]);
                }
            }
            break;
        case NODE_FN_DECL:
            if (stmt->fn_decl.name) {
                if (stmt->fn_decl.is_method &&
                    find_symbol_in_scope(a->current_scope, stmt->fn_decl.name)) {
                    break;
                }
                add_symbol(a, stmt->fn_decl.name, stmt->loc, false, true, stmt->fn_decl.param_count, stmt->fn_decl.fn_type, false);
            }
            break;
        case NODE_STRUCT_DECL:
            if (stmt->struct_decl.name) {
                add_symbol(a, stmt->struct_decl.name, stmt->loc, false, false, -1, NULL, false);
            }
            break;
        case NODE_SCHEMA_DECL:
            if (stmt->schema_decl.name) {
                add_symbol(a, stmt->schema_decl.name, stmt->loc, false, false, -1, NULL, false);
            }
            break;
        case NODE_ENUM_DECL:
            if (stmt->enum_decl.name) {
                add_symbol(a, stmt->enum_decl.name, stmt->loc, false, false, -1, NULL, false);
                if (stmt->enum_decl.variant_names) {
                    for (int i = 0; i < stmt->enum_decl.variant_count; i++) {
                        char full_variant[256];
                        snprintf(full_variant, sizeof(full_variant), "%s::%s", stmt->enum_decl.name, stmt->enum_decl.variant_names[i]);
                        add_symbol(a, full_variant, stmt->loc, false, false, -1, NULL, false);
                    }
                }
            }
            break;
        case NODE_ACTOR_DECL:
            if (stmt->actor_decl.name) {
                add_symbol(a, stmt->actor_decl.name, stmt->loc, false, false, -1, NULL, false);
            }
            break;
        case NODE_TRAIT_DECL:
            if (stmt->trait_decl.name) {
                add_symbol(a, stmt->trait_decl.name, stmt->loc, false, false, -1, NULL, false);
            }
            break;
        case NODE_FFI_DECL:
            if (stmt->ffi_decl.name) {
                add_symbol(a, stmt->ffi_decl.name, stmt->loc, false, true, stmt->ffi_decl.param_count, stmt->type, false);
            }
            break;
        case NODE_LET_DECL:
        case NODE_CONST_DECL:
            if (stmt->let_decl.names) {
                for (int i = 0; i < stmt->let_decl.name_count; i++) {
                    add_symbol(a, stmt->let_decl.names[i], stmt->loc, stmt->kind == NODE_CONST_DECL, false, -1, stmt->let_decl.type, false);
                }
            }
            break;
        default:
            break;
    }
}

static void check_type_annotation_mismatch(Analyzer *a, SourceLoc loc, Type *ann, AstNode *init) {
    if (!ann || !init) return;
    if (ann->kind != TYPE_PRIMITIVE) return;

    PrimitiveKind pk = ann->primitive;
    NodeKind ik = init->kind;

    bool mismatch = false;
    const char *actual = "unknown";

    if (pk == PRIMITIVE_INT) {
        if (ik == NODE_FLOAT_LITERAL) { mismatch = true; actual = "float"; }
        else if (ik == NODE_STRING_LITERAL) { mismatch = true; actual = "string"; }
        else if (ik == NODE_BOOL_LITERAL) { mismatch = true; actual = "bool"; }
    } else if (pk == PRIMITIVE_FLOAT) {
        if (ik == NODE_INT_LITERAL) { mismatch = true; actual = "int"; }
        else if (ik == NODE_STRING_LITERAL) { mismatch = true; actual = "string"; }
        else if (ik == NODE_BOOL_LITERAL) { mismatch = true; actual = "bool"; }
    } else if (pk == PRIMITIVE_STRING) {
        if (ik == NODE_INT_LITERAL) { mismatch = true; actual = "int"; }
        else if (ik == NODE_FLOAT_LITERAL) { mismatch = true; actual = "float"; }
        else if (ik == NODE_BOOL_LITERAL) { mismatch = true; actual = "bool"; }
    } else if (pk == PRIMITIVE_BOOL) {
        if (ik == NODE_INT_LITERAL) { mismatch = true; actual = "int"; }
        else if (ik == NODE_FLOAT_LITERAL) { mismatch = true; actual = "float"; }
        else if (ik == NODE_STRING_LITERAL) { mismatch = true; actual = "string"; }
    }

    if (mismatch) {
        report_error(a, loc, SEM_CODE_TYPE_MISMATCH, "Type mismatch in initializer: expected primitive type but got %s literal", actual);
    }
}

static void analyze_expr(Analyzer *a, AstNode *node) {
    if (!node) return;
    switch (node->kind) {
        case NODE_INT_LITERAL:
        case NODE_FLOAT_LITERAL:
        case NODE_STRING_LITERAL:
        case NODE_BOOL_LITERAL:
        case NODE_NULL_LITERAL:
            break;

        case NODE_IDENTIFIER: {
            const char *name = node->identifier.name;
            if (!name || strcmp(name, "_") == 0) break;
            if (a->current_impl_type && strcmp(name, "self") == 0) break;

            Symbol *sym = lookup_symbol(a, name);
            if (!sym && !is_builtin(name)) {
                report_error(a, node->loc, SEM_CODE_UNDEF_ID, "Undefined identifier '%s'", name);
            }
            break;
        }

        case NODE_BINARY:
            analyze_expr(a, node->binary.left);
            analyze_expr(a, node->binary.right);
            break;

        case NODE_UNARY:
            analyze_expr(a, node->unary.operand);
            break;

        case NODE_CALL: {
            analyze_expr(a, node->call.callee);
            if (node->call.args) {
                for (int i = 0; i < node->call.arg_count; i++) {
                    analyze_expr(a, node->call.args[i]);
                }
            }
            /* Arity check for ordinary statically known functions */
            if (node->call.callee && node->call.callee->kind == NODE_IDENTIFIER) {
                const char *fn_name = node->call.callee->identifier.name;
                Symbol *sym = lookup_symbol(a, fn_name);
                if (sym && sym->is_fn && !sym->is_prelude && sym->param_count >= 0) {
                    if (node->call.arg_count != sym->param_count) {
                        report_error(a, node->loc, SEM_CODE_ARITY_MISMATCH,
                                     "Function '%s' expects %d argument(s), but got %d",
                                     fn_name, sym->param_count, node->call.arg_count);
                    }
                }
            }
            break;
        }

        case NODE_MEMBER:
        case NODE_QUESTION_DOT:
            analyze_expr(a, node->member.object);
            /* node->member.member is field/method name, not standalone symbol */
            break;

        case NODE_INDEX:
            analyze_expr(a, node->index.object);
            analyze_expr(a, node->index.index);
            break;

        case NODE_DISPATCH_CALL:
            analyze_expr(a, node->dispatch_call.object);
            if (node->dispatch_call.args) {
                for (int i = 0; i < node->dispatch_call.arg_count; i++) {
                    analyze_expr(a, node->dispatch_call.args[i]);
                }
            }
            break;

        case NODE_ARRAY_LITERAL:
            if (node->array_literal.elements) {
                for (int i = 0; i < node->array_literal.element_count; i++) {
                    analyze_expr(a, node->array_literal.elements[i]);
                }
            }
            break;

        case NODE_TUPLE_LITERAL:
            if (node->tuple_literal.elements) {
                for (int i = 0; i < node->tuple_literal.element_count; i++) {
                    analyze_expr(a, node->tuple_literal.elements[i]);
                }
            }
            break;

        case NODE_INTERPOLATED_STRING:
            if (node->interpolated_string.parts) {
                for (int i = 0; i < node->interpolated_string.part_count; i++) {
                    analyze_expr(a, node->interpolated_string.parts[i]);
                }
            }
            break;

        case NODE_ASSIGN:
            if (node->assign.target && node->assign.target->kind == NODE_IDENTIFIER) {
                Symbol *sym = lookup_symbol(a, node->assign.target->identifier.name);
                if (sym && sym->is_const) {
                    report_error(a, node->loc, SEM_CODE_ASSIGN_CONST, "Cannot assign to const binding '%s'", sym->name);
                }
            }
            analyze_expr(a, node->assign.target);
            analyze_expr(a, node->assign.value);
            break;

        case NODE_STRUCT_LITERAL:
            if (node->struct_literal.name) {
                Symbol *sym = lookup_symbol(a, node->struct_literal.name);
                if (!sym && !is_builtin(node->struct_literal.name)) {
                    report_error(a, node->loc, SEM_CODE_UNDEF_ID, "Undefined identifier '%s'", node->struct_literal.name);
                }
            }
            if (node->struct_literal.field_values) {
                for (int i = 0; i < node->struct_literal.field_count; i++) {
                    analyze_expr(a, node->struct_literal.field_values[i]);
                }
            }
            break;

        case NODE_ENUM_LITERAL:
            if (node->enum_literal.enum_name) {
                Symbol *sym = lookup_symbol(a, node->enum_literal.enum_name);
                if (!sym && !is_builtin(node->enum_literal.enum_name)) {
                    report_error(a, node->loc, SEM_CODE_UNDEF_ID, "Undefined identifier '%s'", node->enum_literal.enum_name);
                }
            }
            if (node->enum_literal.values) {
                for (int i = 0; i < node->enum_literal.value_count; i++) {
                    analyze_expr(a, node->enum_literal.values[i]);
                }
            }
            break;

        case NODE_PROPAGATE:
            analyze_expr(a, node->propagate.expr);
            break;

        case NODE_AWAIT:
            analyze_expr(a, node->await.expr);
            break;

        case NODE_CHAN_SEND:
            analyze_expr(a, node->chan_send.channel);
            analyze_expr(a, node->chan_send.value);
            break;

        case NODE_CHAN_RECEIVE:
            analyze_expr(a, node->chan_receive.channel);
            break;

        case NODE_FN_DECL:
            analyze_node(a, node);
            break;

        case NODE_COMPTIME:
            analyze_node(a, node->comptime.body);
            break;

        default:
            break;
    }
}

static void analyze_node(Analyzer *a, AstNode *node) {
    if (!node) return;

    switch (node->kind) {
        case NODE_PROGRAM:
            if (node->program.stmts) {
                for (int i = 0; i < node->program.stmt_count; i++) {
                    AstNode *stmt = node->program.stmts[i];
                    if (stmt && (!stmt->loc.filename || strcmp(stmt->loc.filename, "<prelude>") != 0)) {
                        analyze_node(a, stmt);
                    }
                }
            }
            break;

        case NODE_BLOCK:
            push_scope(a, SCOPE_BLOCK);
            if (node->block.stmts) {
                for (int i = 0; i < node->block.stmt_count; i++) {
                    analyze_node(a, node->block.stmts[i]);
                }
            }
            pop_scope(a);
            break;

        case NODE_LET_DECL:
        case NODE_CONST_DECL:
            if (node->let_decl.initializer) {
                analyze_expr(a, node->let_decl.initializer);
                check_type_annotation_mismatch(a, node->loc, node->let_decl.type, node->let_decl.initializer);
            }
            if (a->current_scope != a->program_scope && node->let_decl.names) {
                for (int i = 0; i < node->let_decl.name_count; i++) {
                    add_symbol(a, node->let_decl.names[i], node->loc, node->kind == NODE_CONST_DECL, false, -1, node->let_decl.type, false);
                }
            }
            break;

        case NODE_FN_DECL:
            if (a->current_scope != a->program_scope && node->fn_decl.name) {
                add_symbol(a, node->fn_decl.name, node->loc, false, true, node->fn_decl.param_count, node->fn_decl.fn_type, false);
            }
            push_scope(a, SCOPE_FUNCTION);
            a->fn_depth++;
            int old_loop_depth = a->loop_depth;
            a->loop_depth = 0;
            const char *old_impl = a->current_impl_type;
            if (node->fn_decl.is_method || node->fn_decl.impl_type) {
                a->current_impl_type = node->fn_decl.impl_type ? node->fn_decl.impl_type : "self";
            }

            if (node->fn_decl.param_names) {
                for (int i = 0; i < node->fn_decl.param_count; i++) {
                    add_symbol(a, node->fn_decl.param_names[i], node->loc, false, false, -1, NULL, true);
                }
            }

            if (node->fn_decl.body) {
                analyze_node(a, node->fn_decl.body);
            }

            a->current_impl_type = old_impl;
            a->loop_depth = old_loop_depth;
            a->fn_depth--;
            pop_scope(a);
            break;

        case NODE_EXPR_STMT:
            analyze_expr(a, node->expr_stmt.expr);
            break;

        case NODE_IF:
            analyze_expr(a, node->if_stmt.condition);
            analyze_node(a, node->if_stmt.then_branch);
            if (node->if_stmt.else_branch) {
                analyze_node(a, node->if_stmt.else_branch);
            }
            break;

        case NODE_WHILE:
            analyze_expr(a, node->while_stmt.condition);
            a->loop_depth++;
            analyze_node(a, node->while_stmt.body);
            a->loop_depth--;
            break;

        case NODE_FOR:
            analyze_expr(a, node->for_stmt.iterable);
            push_scope(a, SCOPE_BLOCK);
            a->loop_depth++;
            if (node->for_stmt.var_name) {
                add_symbol(a, node->for_stmt.var_name, node->loc, false, false, -1, NULL, false);
            }
            analyze_node(a, node->for_stmt.body);
            a->loop_depth--;
            pop_scope(a);
            break;

        case NODE_LOOP:
            a->loop_depth++;
            analyze_node(a, node->loop_stmt.body);
            a->loop_depth--;
            break;

        case NODE_BREAK:
            if (a->loop_depth <= 0) {
                report_error(a, node->loc, SEM_CODE_BREAK_OUTSIDE, "break statement outside loop");
            }
            break;

        case NODE_CONTINUE:
            if (a->loop_depth <= 0) {
                report_error(a, node->loc, SEM_CODE_CONTINUE_OUTSIDE, "continue statement outside loop");
            }
            break;

        case NODE_RETURN:
            if (a->fn_depth <= 0) {
                report_error(a, node->loc, SEM_CODE_RETURN_OUTSIDE, "return statement outside function");
            }
            if (node->return_stmt.values) {
                for (int i = 0; i < node->return_stmt.value_count; i++) {
                    analyze_expr(a, node->return_stmt.values[i]);
                }
            }
            break;

        case NODE_ASSIGN:
            if (node->assign.target && node->assign.target->kind == NODE_IDENTIFIER) {
                Symbol *sym = lookup_symbol(a, node->assign.target->identifier.name);
                if (sym && sym->is_const) {
                    report_error(a, node->loc, SEM_CODE_ASSIGN_CONST, "Cannot assign to const binding '%s'", sym->name);
                }
            }
            analyze_expr(a, node->assign.target);
            analyze_expr(a, node->assign.value);
            break;

        case NODE_STRUCT_DECL:
            if (node->struct_decl.field_names) {
                for (int i = 0; i < node->struct_decl.field_count; i++) {
                    for (int j = i + 1; j < node->struct_decl.field_count; j++) {
                        if (strcmp(node->struct_decl.field_names[i], node->struct_decl.field_names[j]) == 0) {
                            report_error(a, node->loc, SEM_CODE_DUP_DECL, "Duplicate field '%s' in struct '%s'",
                                         node->struct_decl.field_names[i], node->struct_decl.name);
                        }
                    }
                }
            }
            break;

        case NODE_SCHEMA_DECL:
            if (node->schema_decl.field_names) {
                for (int i = 0; i < node->schema_decl.field_count; i++) {
                    for (int j = i + 1; j < node->schema_decl.field_count; j++) {
                        if (strcmp(node->schema_decl.field_names[i], node->schema_decl.field_names[j]) == 0) {
                            report_error(a, node->loc, SEM_CODE_DUP_DECL, "Duplicate field '%s' in schema '%s'",
                                         node->schema_decl.field_names[i], node->schema_decl.name);
                        }
                    }
                }
            }
            break;

        case NODE_MATCH:
            analyze_expr(a, node->match_stmt.value);
            if (node->match_stmt.arms) {
                for (int i = 0; i < node->match_stmt.arm_count; i++) {
                    AstNode *arm = node->match_stmt.arms[i];
                    if (arm && arm->kind == NODE_MATCH_ARM) {
                        push_scope(a, SCOPE_BLOCK);
                        if (arm->match_arm.bind_names) {
                            for (int k = 0; k < arm->match_arm.bind_count; k++) {
                                add_symbol(a, arm->match_arm.bind_names[k], arm->loc, false, false, -1, NULL, false);
                            }
                        }
                        if (arm->match_arm.pattern) {
                            if (arm->match_arm.pattern->kind == NODE_ENUM_LITERAL && arm->match_arm.bind_count > 0) {
                                const char *ename = arm->match_arm.pattern->enum_literal.enum_name;
                                if (ename) {
                                    Symbol *sym = lookup_symbol(a, ename);
                                    if (!sym && !is_builtin(ename)) {
                                        report_error(a, arm->loc, SEM_CODE_UNDEF_ID, "Undefined identifier '%s'", ename);
                                    }
                                }
                            } else {
                                analyze_expr(a, arm->match_arm.pattern);
                            }
                        }
                        analyze_node(a, arm->match_arm.body);
                        pop_scope(a);
                    }
                }
            }
            break;

        case NODE_TRY:
            analyze_node(a, node->try_stmt.try_body);
            push_scope(a, SCOPE_BLOCK);
            if (node->try_stmt.catch_var) {
                add_symbol(a, node->try_stmt.catch_var, node->loc, false, false, -1, NULL, false);
            }
            analyze_node(a, node->try_stmt.catch_body);
            pop_scope(a);
            break;

        case NODE_ASSERT:
            analyze_expr(a, node->assert_stmt.condition);
            break;

        case NODE_TEST:
            push_scope(a, SCOPE_BLOCK);
            analyze_node(a, node->test_decl.body);
            pop_scope(a);
            break;

        default:
            break;
    }
}

SemanticResult *semantic_analyze(AstNode *program) {
    SemanticResult *result = (SemanticResult *)calloc(1, sizeof(SemanticResult));
    if (!program) return result;

    Analyzer a;
    memset(&a, 0, sizeof(a));
    a.result = result;

    /* Create program (global) scope */
    a.program_scope = push_scope(&a, SCOPE_PROGRAM);

    /* Pass 1: Collect top-level declarations */
    if (program->kind == NODE_PROGRAM && program->program.stmts) {
        for (int i = 0; i < program->program.stmt_count; i++) {
            collect_top_level_symbol(&a, program->program.stmts[i]);
        }
    }

    /* Pass 2: Full semantic analysis */
    analyze_node(&a, program);

    /* Clean up scope stack */
    while (a.current_scope) {
        pop_scope(&a);
    }

    return result;
}

void semantic_result_free(SemanticResult *result) {
    if (!result) return;
    for (int i = 0; i < result->count; i++) {
        free(result->diagnostics[i].filename);
    }
    free(result->diagnostics);
    free(result);
}
