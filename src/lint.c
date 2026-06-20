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
    int nesting_depth;
} LintWalker;

#define LINT_MAX_FN_STATEMENTS 40
#define LINT_MAX_NESTING_DEPTH 4

static void report_lint(LintContext *ctx, SourceLoc loc, const char *category, const char *fmt, ...) {
    if (ctx->only_category && strcmp(ctx->only_category, category) != 0) {
        return;
    }
    /* A vn_modules prelude may have been prepended ahead of the file being
     * linted (see lint_file) purely so the parser knows about types/functions
     * declared there -- findings whose line falls inside that prelude belong
     * to a *different* file and are silently dropped here (that file gets
     * linted on its own pass when the directory walk reaches it). */
    if (loc.line <= ctx->line_offset) {
        return;
    }
    int line = loc.line - ctx->line_offset;

    char msg[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    if (ctx->format && strcmp(ctx->format, "json") == 0) {
        // Simple JSON escape for message
        printf("{\"category\":\"%s\",\"file\":\"%s\",\"line\":%d,\"message\":\"%s\"}\n",
               category, loc.filename ? loc.filename : "unknown", line, msg);
    } else {
        printf("%s:%d: [%s] %s\n", loc.filename ? loc.filename : "unknown", line, category, msg);
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

/* Every (module, method) pair currently registered via vm_register_dispatch
 * across the native lib_*.c files (grep is the source of truth -- keep this
 * in sync if a native module gains/renames a method). The parser's method-
 * name registry is global and untyped: ANY `impl Type { fn name(self) }`
 * anywhere in the program makes the parser emit a dispatch call (module
 * prepended as the native fn's first argument) for every later call to
 * `anything.name(...)`, including unrelated native modules -- see the
 * io_arg_base/http_arg_base comments in lib_io.c/lib_http.c for the exact
 * failure mode this caused (io.exists/io.delete/http.get silently broke).
 * Native functions can defend against this defensively, but it's easy to
 * miss, so flag the collision at its actual source: the impl declaration. */
static const char *known_native_methods[] = {
    "len", "push", "hash_sha256", "sign_jwt", "verify_jwt", "create_struct",
    "get", "post", "serve", "serve_with_routes", "test_request", "delete",
    "exists", "list_dir", "mkdir", "read_bytes", "read_text", "write_bytes",
    "write_text", "abs", "ceil", "cos", "floor", "sin", "sqrt", "intercept",
    "restore", "close", "connect", "query", "run", "cmd", "escape_html",
    "strip_html", "trim", "split", "lower", "replace", "starts_with",
    "substring", "upper", "channel", "id", "sleep", "spawn", "yield",
    "now_iso8601", "now_ms", "is_alphanumeric", "is_email", "is_url",
    "is_uuid", "max_len", "min_len", "write_socket", "close_socket",
    "read_socket", "code_at", "from_codes", "sha1_base64", "hash_password",
    "verify_password", "bit_and", "bit_or", "bit_xor",
};
#define KNOWN_NATIVE_METHOD_COUNT (sizeof(known_native_methods) / sizeof(known_native_methods[0]))

static bool is_known_native_method(const char *name) {
    for (size_t i = 0; i < KNOWN_NATIVE_METHOD_COUNT; i++) {
        if (strcmp(known_native_methods[i], name) == 0) return true;
    }
    return false;
}

static bool is_raw_query_function_name(const char *name) {
    /* Unlike is_sql_function_name (which also covers the comptime ORM's
     * select()/compile_select()/QueryBuilder -- none of which take a raw SQL
     * string), this is specifically the native functions that send a literal
     * SQL/command string straight to a driver: sqlite.query/postgres.query/
     * redis.cmd. Only string-literal arguments to *these* are worth checking
     * for a missing LIMIT clause. */
    return name && (strcmp(name, "query") == 0 || strcmp(name, "cmd") == 0);
}

static void check_sql_args_missing_limit(LintContext *ctx, AstNode **args, int arg_count) {
    for (int i = 0; i < arg_count; i++) {
        AstNode *arg = args[i];
        if (arg && arg->kind == NODE_STRING_LITERAL) {
            const char *val = arg->literal.string_value;
            if (val && lint_strcasestr(val, "select") && !lint_strcasestr(val, "limit")) {
                report_lint(ctx, arg->loc, "performance", "SQL query without LIMIT clause");
            }
        }
    }
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

        case NODE_IF: {
            lint_walk(node->if_stmt.condition, walker);
            walker->nesting_depth++;
            if (walker->nesting_depth == LINT_MAX_NESTING_DEPTH + 1) {
                report_lint(walker->ctx, node->loc, "style",
                            "Nesting depth exceeds %d levels", LINT_MAX_NESTING_DEPTH);
            }
            lint_walk(node->if_stmt.then_branch, walker);
            lint_walk(node->if_stmt.else_branch, walker);
            walker->nesting_depth--;
            break;
        }

        case NODE_WHILE: {
            lint_walk(node->while_stmt.condition, walker);
            bool old_loop = walker->in_loop;
            walker->in_loop = true;
            walker->nesting_depth++;
            if (walker->nesting_depth == LINT_MAX_NESTING_DEPTH + 1) {
                report_lint(walker->ctx, node->loc, "style",
                            "Nesting depth exceeds %d levels", LINT_MAX_NESTING_DEPTH);
            }
            lint_walk(node->while_stmt.body, walker);
            walker->nesting_depth--;
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
            walker->nesting_depth++;
            if (walker->nesting_depth == LINT_MAX_NESTING_DEPTH + 1) {
                report_lint(walker->ctx, node->loc, "style",
                            "Nesting depth exceeds %d levels", LINT_MAX_NESTING_DEPTH);
            }
            lint_walk(node->for_stmt.body, walker);
            walker->nesting_depth--;
            walker->in_loop = old_loop;
            walker->current_scope = scope.parent;
            break;
        }

        case NODE_LOOP: {
            bool old_loop = walker->in_loop;
            walker->in_loop = true;
            walker->nesting_depth++;
            if (walker->nesting_depth == LINT_MAX_NESTING_DEPTH + 1) {
                report_lint(walker->ctx, node->loc, "style",
                            "Nesting depth exceeds %d levels", LINT_MAX_NESTING_DEPTH);
            }
            lint_walk(node->loop_stmt.body, walker);
            walker->nesting_depth--;
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
            if (node->call.callee->kind == NODE_MEMBER &&
                is_raw_query_function_name(node->call.callee->member.member)) {
                if (walker->in_loop) {
                    report_lint(walker->ctx, node->loc, "performance", "N+1 query pattern detected: query in loop");
                }
                check_sql_args_missing_limit(walker->ctx, node->call.args, node->call.arg_count);
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

        case NODE_DISPATCH_CALL: {
            /* `obj.method(args)` -- a method call (struct/actor impl, or any
             * native module function whose name collides with a registered
             * impl method, see lib_io.c/lib_http.c's io_arg_base/http_arg_base
             * comments) compiles to this node instead of NODE_CALL. Without
             * this case the object and every argument went unvisited, which
             * made every identifier referenced only via a dispatch call look
             * "unused" -- a real false-positive bug in the unused-let/
             * unused-parameter rules, not just a D4 gap. */
            if (is_raw_query_function_name(node->dispatch_call.method_name)) {
                if (walker->in_loop) {
                    report_lint(walker->ctx, node->loc, "performance", "N+1 query pattern detected: query in loop");
                }
                check_sql_args_missing_limit(walker->ctx, node->dispatch_call.args, node->dispatch_call.arg_count);
            }
            lint_walk(node->dispatch_call.object, walker);
            for (int i = 0; i < node->dispatch_call.arg_count; i++) {
                lint_walk(node->dispatch_call.args[i], walker);
            }
            break;
        }

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

        case NODE_STRING_LITERAL:
            break;

        case NODE_FN_DECL: {
            if (node->fn_decl.impl_type && is_known_native_method(node->fn_decl.name)) {
                report_lint(walker->ctx, node->loc, "collision",
                            "Method '%s' on '%s' shadows a native module function name -- "
                            "any later call to <module>.%s(...) anywhere in the program may be "
                            "misdispatched (module value prepended as the native fn's first arg)",
                            node->fn_decl.name, node->fn_decl.impl_type, node->fn_decl.name);
            }

            LintScope scope;
            scope.parent = walker->current_scope;
            scope.binding_count = 0;

            for (int i = 0; i < node->fn_decl.param_count; i++) {
                if (scope.binding_count < 256) {
                    int idx = scope.binding_count++;
                    strncpy(scope.bindings[idx].name, node->fn_decl.param_names[i], 63);
                    scope.bindings[idx].name[63] = '\0';
                    /* "self" is conventionally unread in many trivial methods
                     * (it's there for the dispatch, not necessarily the body)
                     * -- exempt it to avoid noise; every other param is
                     * tracked for real usage. */
                    scope.bindings[idx].used = (strcmp(node->fn_decl.param_names[i], "self") == 0);
                    scope.bindings[idx].loc = node->loc;
                }
            }

            int saved_depth = walker->nesting_depth;
            walker->nesting_depth = 0;
            walker->current_scope = &scope;
            lint_walk(node->fn_decl.body, walker);
            walker->current_scope = scope.parent;
            walker->nesting_depth = saved_depth;

            for (int i = 0; i < scope.binding_count; i++) {
                if (!scope.bindings[i].used) {
                    report_lint(walker->ctx, scope.bindings[i].loc, "style",
                                "Unused parameter '%s' in function '%s'",
                                scope.bindings[i].name, node->fn_decl.name);
                }
            }

            if (node->fn_decl.body && node->fn_decl.body->kind == NODE_BLOCK &&
                node->fn_decl.body->block.stmt_count > LINT_MAX_FN_STATEMENTS) {
                report_lint(walker->ctx, node->loc, "style",
                            "Function '%s' is %d statements long (threshold: %d)",
                            node->fn_decl.name, node->fn_decl.body->block.stmt_count,
                            LINT_MAX_FN_STATEMENTS);
            }
            break;
        }

        case NODE_TEST:
            lint_walk(node->test_decl.body, walker);
            break;

        case NODE_TRY: {
            lint_walk(node->try_stmt.try_body, walker);
            LintScope scope;
            scope.parent = walker->current_scope;
            scope.binding_count = 0;
            if (node->try_stmt.catch_var) {
                int idx = scope.binding_count++;
                strncpy(scope.bindings[idx].name, node->try_stmt.catch_var, 63);
                scope.bindings[idx].name[63] = '\0';
                scope.bindings[idx].used = false;
                scope.bindings[idx].loc = node->loc;
            }
            walker->current_scope = &scope;
            lint_walk(node->try_stmt.catch_body, walker);
            walker->current_scope = scope.parent;
            break;
        }

        case NODE_CHAN_SEND:
            lint_walk(node->chan_send.channel, walker);
            lint_walk(node->chan_send.value, walker);
            break;

        case NODE_CHAN_RECEIVE:
            lint_walk(node->chan_receive.channel, walker);
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

/* Count files in "vn_modules" (cwd-relative, same convention main.c's
 * read_file_with_modules uses), concatenate them alphabetically, and report
 * how many lines that prelude occupies so the caller can offset reported
 * line numbers back to the actual file being linted. NULL if there's no
 * vn_modules directory to find. */
static char *build_vn_modules_prelude(int *out_line_count) {
    *out_line_count = 0;
    DIR *d = opendir("vn_modules");
    if (!d) return NULL;

    char *names[256];
    int name_count = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && name_count < 256) {
        size_t len = strlen(entry->d_name);
        if (len > 3 && strcmp(entry->d_name + len - 3, ".vn") == 0) {
            names[name_count] = strdup(entry->d_name);
            name_count++;
        }
    }
    closedir(d);
    if (name_count == 0) return NULL;

    for (int i = 0; i < name_count - 1; i++) {
        for (int j = i + 1; j < name_count; j++) {
            if (strcmp(names[i], names[j]) > 0) {
                char *tmp = names[i]; names[i] = names[j]; names[j] = tmp;
            }
        }
    }

    char *result = NULL;
    size_t result_len = 0;
    for (int i = 0; i < name_count; i++) {
        char full[1024];
        snprintf(full, sizeof(full), "vn_modules/%s", names[i]);
        char *content = read_file(full);
        free(names[i]);
        if (!content) continue;
        size_t content_len = strlen(content);
        char *new_result = (char *)realloc(result, result_len + content_len + 2);
        if (!new_result) { free(content); continue; }
        result = new_result;
        if (result_len > 0) { result[result_len] = '\n'; result_len++; }
        memcpy(result + result_len, content, content_len);
        result_len += content_len;
        result[result_len] = '\0';
        free(content);
    }
    if (!result) return NULL;

    int lines = 1;
    for (size_t i = 0; i < result_len; i++) {
        if (result[i] == '\n') lines++;
    }
    *out_line_count = lines;
    return result;
}

static int lint_file(const char *path, LintContext *ctx) {
    char *own_source = read_file(path);
    if (!own_source) {
        fprintf(stderr, "lint: could not read file '%s'\n", path);
        return 1;
    }

    /* Files inside vn_modules/ are linted standalone -- prepending the
     * prelude there would mean a file sees (a duplicate of) itself. Other
     * files get the prelude so the parser knows about types/functions
     * (structs, fn signatures, impl methods) declared in vn_modules before
     * trying to parse code that uses them -- without it, e.g. any file using
     * Zenith's `Response { ... }` literal fails to parse at all. */
    bool inside_vn_modules = (strstr(path, "vn_modules/") != NULL);
    char *source = own_source;
    int line_offset = 0;
    char *prelude = NULL;
    if (!inside_vn_modules) {
        prelude = build_vn_modules_prelude(&line_offset);
        if (prelude) {
            size_t plen = strlen(prelude);
            size_t olen = strlen(own_source);
            char *combined = (char *)malloc(plen + 1 + olen + 1);
            memcpy(combined, prelude, plen);
            combined[plen] = '\n';
            memcpy(combined + plen + 1, own_source, olen);
            combined[plen + 1 + olen] = '\0';
            source = combined;
        }
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
        free(own_source);
        if (source != own_source) free(source);
        free(prelude);
        return 1;
    }

    ctx->line_offset = line_offset;

    LintWalker walker;
    walker.ctx = ctx;
    walker.current_scope = NULL;
    walker.in_loop = false;
    walker.nesting_depth = 0;

    LintScope top_scope;
    top_scope.parent = NULL;
    top_scope.binding_count = 0;
    walker.current_scope = &top_scope;

    lint_walk(program, &walker);

    arena_destroy(arena);
    free(own_source);
    if (source != own_source) free(source);
    free(prelude);
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
