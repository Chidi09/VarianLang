#include "lint.h"
#include "lexer.h"
#include "parser.h"
#include "vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdarg.h>

typedef struct {
    char name[64];
    bool used;
    SourceLoc loc;
} LintBinding;

typedef struct LintScope LintScope;
struct LintScope {
    LintScope *parent;
    LintBinding bindings[256];
    int binding_count;
};

typedef struct {
    LintContext *ctx;
    LintScope *current_scope;
    bool in_loop;
} LintWalker;

static void report_lint(LintContext *ctx, SourceLoc loc, const char *category, const char *fmt, ...) {
    if (ctx->only_category && strcmp(ctx->only_category, category) != 0) {
        return;
    }
    char msg[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    if (ctx->format && strcmp(ctx->format, "json") == 0) {
        // Simple JSON escape for message
        printf("{\"category\":\"%s\",\"file\":\"%s\",\"line\":%d,\"message\":\"%s\"}\n",
               category, loc.filename ? loc.filename : "unknown", loc.line, msg);
    } else {
        printf("%s:%d: [%s] %s\n", loc.filename ? loc.filename : "unknown", loc.line, category, msg);
    }
    ctx->had_error = true;
}

static bool is_terminator(AstNode *node) {
    if (!node) return false;
    if (node->kind == NODE_RETURN || node->kind == NODE_BREAK || node->kind == NODE_CONTINUE) {
        return true;
    }
    if (node->kind == NODE_EXPR_STMT) {
        AstNode *expr = node->expr_stmt.expr;
        if (expr && expr->kind == NODE_CALL) {
            AstNode *callee = expr->call.callee;
            if (callee && callee->kind == NODE_IDENTIFIER && strcmp(callee->identifier.name, "throw") == 0) {
                return true;
            }
        }
    }
    return false;
}

static bool lookup_ancestor(LintScope *scope, const char *name) {
    LintScope *curr = scope ? scope->parent : NULL;
    while (curr) {
        for (int i = 0; i < curr->binding_count; i++) {
            if (strcmp(curr->bindings[i].name, name) == 0) {
                return true;
            }
        }
        curr = curr->parent;
    }
    return false;
}

static void mark_used(LintScope *scope, const char *name) {
    LintScope *curr = scope;
    while (curr) {
        for (int i = 0; i < curr->binding_count; i++) {
            if (strcmp(curr->bindings[i].name, name) == 0) {
                curr->bindings[i].used = true;
                return;
            }
        }
        curr = curr->parent;
    }
}

static const char *lint_strcasestr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    size_t needle_len = strlen(needle);
    if (needle_len == 0) return haystack;
    for (; *haystack; haystack++) {
        if (strncasecmp(haystack, needle, needle_len) == 0) {
            return haystack;
        }
    }
    return NULL;
}

static bool is_string_concat_chain(AstNode *node) {
    if (!node) return false;
    if (node->kind == NODE_BINARY && node->binary.op == OP_ADD) {
        return true;
    }
    return false;
}

static bool is_sql_function_name(const char *name) {
    if (!name) return false;
    return strcmp(name, "query") == 0 ||
           strcmp(name, "select") == 0 ||
           strcmp(name, "compile_select") == 0 ||
           strcmp(name, "QueryBuilder") == 0;
}

static bool check_sql_call_unsafe(AstNode *callee, AstNode **args, int arg_count) {
    bool is_sql_call = false;
    if (callee->kind == NODE_IDENTIFIER) {
        if (is_sql_function_name(callee->identifier.name)) {
            is_sql_call = true;
        }
    } else if (callee->kind == NODE_MEMBER) {
        if (is_sql_function_name(callee->member.member)) {
            is_sql_call = true;
        } else if (callee->member.object && callee->member.object->kind == NODE_IDENTIFIER) {
            if (is_sql_function_name(callee->member.object->identifier.name)) {
                is_sql_call = true;
            }
        }
    }

    if (is_sql_call) {
        for (int i = 0; i < arg_count; i++) {
            if (is_string_concat_chain(args[i])) {
                return true;
            }
        }
    }
    return false;
}

static void lint_walk(AstNode *node, LintWalker *walker) {
    if (!node) return;

    switch (node->kind) {
        case NODE_PROGRAM: {
            for (int i = 0; i < node->program.stmt_count; i++) {
                lint_walk(node->program.stmts[i], walker);
            }
            break;
        }

        case NODE_LET_DECL: {
            // First walk initializer so it doesn't use the declared variable itself
            lint_walk(node->let_decl.initializer, walker);

            if (node->let_decl.initializer && node->let_decl.initializer->kind == NODE_STRING_LITERAL) {
                const char *str_val = node->let_decl.initializer->literal.string_value;
                if (str_val && strlen(str_val) > 12) {
                    for (int i = 0; i < node->let_decl.name_count; i++) {
                        const char *name = node->let_decl.names[i];
                        if (lint_strcasestr(name, "password") || lint_strcasestr(name, "api_key")) {
                            report_lint(walker->ctx, node->loc, "security", "Hardcoded secret '%s' in let declaration", name);
                        }
                    }
                }
            }

            if (walker->current_scope) {
                for (int i = 0; i < node->let_decl.name_count; i++) {
                    const char *name = node->let_decl.names[i];
                    if (lookup_ancestor(walker->current_scope, name)) {
                        report_lint(walker->ctx, node->loc, "shadowed", "Shadowed binding '%s'", name);
                    }
                    if (walker->current_scope->binding_count < 256) {
                        int idx = walker->current_scope->binding_count++;
                        strncpy(walker->current_scope->bindings[idx].name, name, 63);
                        walker->current_scope->bindings[idx].name[63] = '\0';
                        walker->current_scope->bindings[idx].used = false;
                        walker->current_scope->bindings[idx].loc = node->loc;
                    }
                }
            }
            break;
        }

        case NODE_IDENTIFIER: {
            mark_used(walker->current_scope, node->identifier.name);
            break;
        }

        case NODE_BLOCK: {
            LintScope scope;
            scope.parent = walker->current_scope;
            scope.binding_count = 0;
            walker->current_scope = &scope;

            bool unreachable_reported = false;
            for (int i = 0; i < node->block.stmt_count; i++) {
                if (unreachable_reported) {
                    report_lint(walker->ctx, node->block.stmts[i]->loc, "unreachable", "Unreachable code");
                } else {
                    lint_walk(node->block.stmts[i], walker);
                    if (is_terminator(node->block.stmts[i])) {
                        unreachable_reported = true;
                    }
                }
            }

            // Check unused let bindings
            for (int i = 0; i < scope.binding_count; i++) {
                if (!scope.bindings[i].used) {
                    report_lint(walker->ctx, scope.bindings[i].loc, "unused", "Unused let binding '%s'", scope.bindings[i].name);
                }
            }

            walker->current_scope = scope.parent;
            break;
        }

        case NODE_EXPR_STMT:
            lint_walk(node->expr_stmt.expr, walker);
            break;

        case NODE_IF:
            lint_walk(node->if_stmt.condition, walker);
            lint_walk(node->if_stmt.then_branch, walker);
            lint_walk(node->if_stmt.else_branch, walker);
            break;

        case NODE_WHILE: {
            lint_walk(node->while_stmt.condition, walker);
            bool old_loop = walker->in_loop;
            walker->in_loop = true;
            lint_walk(node->while_stmt.body, walker);
            walker->in_loop = old_loop;
            break;
        }

        case NODE_FOR: {
            lint_walk(node->for_stmt.iterable, walker);
            
            // Loop variable introduced
            LintScope scope;
            scope.parent = walker->current_scope;
            scope.binding_count = 0;
            if (node->for_stmt.var_name) {
                int idx = scope.binding_count++;
                strncpy(scope.bindings[idx].name, node->for_stmt.var_name, 63);
                scope.bindings[idx].name[63] = '\0';
                scope.bindings[idx].used = true; // loop vars used by default to prevent false positives
                scope.bindings[idx].loc = node->loc;
            }
            walker->current_scope = &scope;
            bool old_loop = walker->in_loop;
            walker->in_loop = true;
            lint_walk(node->for_stmt.body, walker);
            walker->in_loop = old_loop;
            walker->current_scope = scope.parent;
            break;
        }

        case NODE_LOOP: {
            bool old_loop = walker->in_loop;
            walker->in_loop = true;
            lint_walk(node->loop_stmt.body, walker);
            walker->in_loop = old_loop;
            break;
        }

        case NODE_RETURN:
            for (int i = 0; i < node->return_stmt.value_count; i++) {
                lint_walk(node->return_stmt.values[i], walker);
            }
            break;

        case NODE_ASSIGN: {
            if (node->assign.value && node->assign.value->kind == NODE_STRING_LITERAL) {
                const char *str_val = node->assign.value->literal.string_value;
                if (str_val && strlen(str_val) > 12) {
                    AstNode *target = node->assign.target;
                    if (target->kind == NODE_IDENTIFIER) {
                        const char *name = target->identifier.name;
                        if (lint_strcasestr(name, "password") || lint_strcasestr(name, "api_key")) {
                            report_lint(walker->ctx, node->loc, "security", "Hardcoded secret assignment to '%s'", name);
                        }
                    } else if (target->kind == NODE_MEMBER) {
                        const char *name = target->member.member;
                        if (lint_strcasestr(name, "password") || lint_strcasestr(name, "api_key")) {
                            report_lint(walker->ctx, node->loc, "security", "Hardcoded secret assignment to field '%s'", name);
                        }
                    }
                }
            }
            lint_walk(node->assign.target, walker);
            lint_walk(node->assign.value, walker);
            break;
        }

        case NODE_BINARY:
            lint_walk(node->binary.left, walker);
            lint_walk(node->binary.right, walker);
            break;

        case NODE_UNARY:
            lint_walk(node->unary.operand, walker);
            break;

        case NODE_CALL: {
            if (check_sql_call_unsafe(node->call.callee, node->call.args, node->call.arg_count)) {
                report_lint(walker->ctx, node->loc, "security", "String-concatenated SQL query detected");
            }
            if (walker->in_loop && node->call.callee->kind == NODE_MEMBER) {
                const char *member = node->call.callee->member.member;
                if (strcmp(member, "query") == 0 || strcmp(member, "cmd") == 0) {
                    report_lint(walker->ctx, node->loc, "performance", "N+1 query pattern detected: query in loop");
                }
            }
            lint_walk(node->call.callee, walker);
            for (int i = 0; i < node->call.arg_count; i++) {
                lint_walk(node->call.args[i], walker);
            }
            break;
        }

        case NODE_INDEX:
            lint_walk(node->index.object, walker);
            lint_walk(node->index.index, walker);
            break;

        case NODE_MEMBER:
            lint_walk(node->member.object, walker);
            break;

        case NODE_INTERPOLATED_STRING:
            for (int i = 0; i < node->interpolated_string.part_count; i++) {
                lint_walk(node->interpolated_string.parts[i], walker);
            }
            break;

        case NODE_ARRAY_LITERAL:
            for (int i = 0; i < node->array_literal.element_count; i++) {
                lint_walk(node->array_literal.elements[i], walker);
            }
            break;

        case NODE_TUPLE_LITERAL:
            for (int i = 0; i < node->tuple_literal.element_count; i++) {
                lint_walk(node->tuple_literal.elements[i], walker);
            }
            break;

        case NODE_MATCH:
            lint_walk(node->match_stmt.value, walker);
            for (int i = 0; i < node->match_stmt.arm_count; i++) {
                lint_walk(node->match_stmt.arms[i], walker);
            }
            break;

        case NODE_STRUCT_LITERAL: {
            for (int i = 0; i < node->struct_literal.field_count; i++) {
                AstNode *val = node->struct_literal.field_values[i];
                if (val && val->kind == NODE_STRING_LITERAL) {
                    const char *str_val = val->literal.string_value;
                    if (str_val && strlen(str_val) > 12) {
                        const char *name = node->struct_literal.field_names[i];
                        if (lint_strcasestr(name, "password") || lint_strcasestr(name, "api_key")) {
                            report_lint(walker->ctx, node->loc, "security", "Hardcoded secret in struct field '%s'", name);
                        }
                    }
                }
            }
            for (int i = 0; i < node->struct_literal.field_count; i++) {
                lint_walk(node->struct_literal.field_values[i], walker);
            }
            break;
        }

        case NODE_STRING_LITERAL: {
            const char *val = node->literal.string_value;
            if (val && lint_strcasestr(val, "select")) {
                if (!lint_strcasestr(val, "limit")) {
                    report_lint(walker->ctx, node->loc, "performance", "SQL query without LIMIT clause");
                }
            }
            break;
        }

        case NODE_FN_DECL: {
            LintScope scope;
            scope.parent = walker->current_scope;
            scope.binding_count = 0;
            
            // Add parameters to prevent parameter shadowing warning inside function
            for (int i = 0; i < node->fn_decl.param_count; i++) {
                if (scope.binding_count < 256) {
                    int idx = scope.binding_count++;
                    strncpy(scope.bindings[idx].name, node->fn_decl.param_names[i], 63);
                    scope.bindings[idx].name[63] = '\0';
                    scope.bindings[idx].used = true; // params are used or tracked under D4
                    scope.bindings[idx].loc = node->loc;
                }
            }

            walker->current_scope = &scope;
            lint_walk(node->fn_decl.body, walker);
            walker->current_scope = scope.parent;
            break;
        }

        case NODE_TEST:
            lint_walk(node->test_decl.body, walker);
            break;

        default:
            break;
    }
}

static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0L, SEEK_END);
    size_t size = (size_t)ftell(file);
    rewind(file);
    char *buf = (char *)malloc(size + 1);
    if (!buf) { fclose(file); return NULL; }
    size_t nread = fread(buf, 1, size, file);
    buf[nread] = '\0';
    fclose(file);
    return buf;
}

static int lint_file(const char *path, LintContext *ctx) {
    char *source = read_file(path);
    if (!source) {
        fprintf(stderr, "lint: could not read file '%s'\n", path);
        return 1;
    }

    Lexer lexer;
    lexer_init(&lexer, source, path);

    Arena *arena = arena_create(0);
    Parser parser;
    parser_init(&parser, &lexer, arena);

    AstNode *program = parser_parse(&parser);
    if (parser.had_error) {
        fprintf(stderr, "Parse error in %s: %s\n", path, parser_get_error(&parser));
        arena_destroy(arena);
        free(source);
        return 1;
    }

    LintWalker walker;
    walker.ctx = ctx;
    walker.current_scope = NULL;
    walker.in_loop = false;

    LintScope top_scope;
    top_scope.parent = NULL;
    top_scope.binding_count = 0;
    walker.current_scope = &top_scope;

    lint_walk(program, &walker);

    arena_destroy(arena);
    free(source);
    return 0;
}

static void collect_vn_files(const char *dir, char ***files, int *count, int *cap) {
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        size_t dlen = strlen(dir);
        size_t nlen = strlen(entry->d_name);
        char *full = (char *)malloc(dlen + nlen + 2);
        if (!full) continue;
        memcpy(full, dir, dlen);
        full[dlen] = '/';
        memcpy(full + dlen + 1, entry->d_name, nlen);
        full[dlen + 1 + nlen] = '\0';

        struct stat st;
        if (stat(full, &st) == -1) { free(full); continue; }

        if (S_ISDIR(st.st_mode)) {
            collect_vn_files(full, files, count, cap);
        } else if (S_ISREG(st.st_mode) && nlen > 3 && strcmp(entry->d_name + nlen - 3, ".vn") == 0) {
            if (*count >= *cap) {
                *cap = *cap ? *cap * 2 : 16;
                *files = (char **)realloc(*files, (size_t)(*cap) * sizeof(char *));
            }
            (*files)[*count] = full;
            (*count)++;
        } else {
            free(full);
        }
    }
    closedir(d);
}

int run_lint(const char *path, const char *only_category, const char *format) {
    LintContext ctx;
    ctx.only_category = only_category;
    ctx.format = format;
    ctx.had_error = false;

    struct stat st;
    if (stat(path, &st) == -1) {
        fprintf(stderr, "lint: path '%s' does not exist\n", path);
        return 1;
    }

    if (S_ISDIR(st.st_mode)) {
        char **files = NULL;
        int count = 0;
        int cap = 0;
        collect_vn_files(path, &files, &count, &cap);

        for (int i = 0; i < count; i++) {
            lint_file(files[i], &ctx);
            free(files[i]);
        }
        free(files);
    } else {
        lint_file(path, &ctx);
    }

    return ctx.had_error ? 1 : 0;
}
