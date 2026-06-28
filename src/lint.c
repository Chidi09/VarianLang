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
    int line = loc.line - ctx->line_offset + ctx->line_base;

    char msg[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    const char *fname = ctx->current_path ? ctx->current_path : (loc.filename ? loc.filename : "unknown");

    if (ctx->sink) {
        ctx->sink(ctx->sink_ud, line, loc.column, category, msg);
        ctx->had_error = true;
        return;
    }

    if (ctx->format && strcmp(ctx->format, "json") == 0) {
        // Simple JSON escape for message
        printf("{\"category\":\"%s\",\"file\":\"%s\",\"line\":%d,\"message\":\"%s\"}\n",
               category, fname, line, msg);
    } else {
        printf("%s:%d: [%s] %s\n", fname, line, category, msg);
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
    "abs", "acos", "append", "asin", "atan", "atan2", "bit_and", "bit_or",
    "bit_xor", "build", "ceil", "channel", "clamp", "close",
    "close_socket", "cmd", "code_at", "connect", "constant_time_eq",
    "contains", "cos", "cosh", "create_struct", "decrypt", "deg_to_rad",
    "delete", "drain_changes", "encrypt", "ends_with", "escape_html",
    "exists", "exp", "explain", "fields", "find_all", "floor",
    "from_codes", "generate_token", "get", "get_field", "get_keys",
    "groups", "has", "hash_password", "hash_password_v2", "hash_sha256",
    "id", "index_of", "intercept", "is", "is_alphanumeric", "is_email",
    "is_url", "is_uuid", "join", "keys", "kind", "last_index_of", "len",
    "lerp", "limit", "list_dir", "load", "log", "log10", "log2", "lower",
    "make", "match", "max", "max_arr", "max_len", "mean", "median", "min",
    "min_arr", "min_len", "mkdir", "now_iso8601", "now_ms", "offset",
    "order_by", "paginate", "parse_multipart", "post", "post_stream",
    "pow", "product", "push", "query", "rad_to_deg", "randfloat",
    "randint", "random", "read_bytes", "read_socket", "read_text",
    "replace", "require", "restore", "round", "run", "seed", "select",
    "send", "serve", "serve_tls", "serve_with_routes", "set",
    "sha1_base64", "shuffle", "sign", "sign_jwt", "sign_jwt_v2", "sin",
    "sinh", "sleep", "spawn", "split", "sqrt", "starts_with", "stddev",
    "strip_html", "substring", "sum", "tanh", "test", "test_request",
    "totp_generate", "totp_secret", "totp_verify", "trim", "trunc",
    "try_receive", "upper", "variance", "verify_jwt", "verify_jwt_v2",
    "verify_password", "verify_password_v2", "where", "write_bytes",
    "write_socket", "write_text", "yield",
};
#define KNOWN_NATIVE_METHOD_COUNT (sizeof(known_native_methods) / sizeof(known_native_methods[0]))

/* The parser accepts @anything(args) on a struct/field with no validation
 * at all (see parse_struct_decl in src/parser.c) -- a typo like
 * @min_lenn(3) silently does nothing, ever, with no error at parse time,
 * compile time, or runtime. This is every decorator name actually wired
 * up to a real effect (validation rules from lib_validate.c, plus the
 * handful of non-validation decorators used elsewhere in this codebase). */
static const char *known_decorator_names[] = {
    "validate", "is_email", "is_url", "is_alphanumeric", "min_len", "max_len",
    "is_uuid", "cache", "retry", "ffi",
};
#define KNOWN_DECORATOR_COUNT (sizeof(known_decorator_names) / sizeof(known_decorator_names[0]))

static bool is_known_decorator(const char *name) {
    for (size_t i = 0; i < KNOWN_DECORATOR_COUNT; i++) {
        if (strcmp(known_decorator_names[i], name) == 0) return true;
    }
    return false;
}

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

static bool check_has_terminal_fetch_method(AstNode *expr) {
    if (!expr) return false;
    if (expr->kind == NODE_DISPATCH_CALL) {
        const char *m = expr->dispatch_call.method_name;
        if (strcmp(m, "get") == 0 || strcmp(m, "post") == 0 || 
            strcmp(m, "put") == 0 || strcmp(m, "delete") == 0 ||
            strcmp(m, "json") == 0) {
            return true;
        }
        return check_has_terminal_fetch_method(expr->dispatch_call.object);
    }
    return false;
}

static void check_module_usages(LintWalker *walker, AstNode *node) {
    if (!node) return;
    if (node->kind == NODE_CALL) {
        // A. Fetch Module Checks — handled in NODE_EXPR_STMT

        // B. Mail Module Checks (SMTP/Resend Arguments & Address Format)
        AstNode *callee = node->call.callee;
        const char *fn_name = NULL;
        if (callee && callee->kind == NODE_IDENTIFIER) {
            fn_name = callee->identifier.name;
        } else if (callee && callee->kind == NODE_MEMBER) {
            fn_name = callee->member.member;
        }

        if (fn_name) {
            bool is_smtp = (strcmp(fn_name, "send_smtp") == 0);
            bool is_resend = (strcmp(fn_name, "send_resend") == 0);
            if (is_smtp || is_resend) {
                int expected = is_smtp ? 6 : 5;
                if (node->call.arg_count != expected) {
                    report_lint(walker->ctx, node->loc, "arguments", 
                                "Argument count mismatch for %s: expected %d, got %d", 
                                fn_name, expected, node->call.arg_count);
                }
                // Check address format validation
                // SMTP: from is arg index 2, to is arg index 3
                // Resend: from is arg index 1, to is arg index 2
                int from_idx = is_smtp ? 2 : 1;
                int to_idx = is_smtp ? 3 : 2;
                if (from_idx < node->call.arg_count) {
                    AstNode *from_arg = node->call.args[from_idx];
                    if (from_arg && from_arg->kind == NODE_STRING_LITERAL) {
                        const char *email = from_arg->literal.string_value;
                        if (email && !strchr(email, '@')) {
                            report_lint(walker->ctx, from_arg->loc, "arguments", "Invalid email address format for parameter");
                        }
                    }
                }
                if (to_idx < node->call.arg_count) {
                    AstNode *to_arg = node->call.args[to_idx];
                    if (to_arg && to_arg->kind == NODE_STRING_LITERAL) {
                        const char *email = to_arg->literal.string_value;
                        if (email && !strchr(email, '@')) {
                            report_lint(walker->ctx, to_arg->loc, "arguments", "Invalid email address format for parameter");
                        }
                    }
                }
            }
        }
    } else if (node->kind == NODE_DISPATCH_CALL) {
        // C. Migration Module Checks (Raw SQL Guard)
        // Detect calls to `.register()` or `.register_up()` on Migrator objects.
        const char *method = node->dispatch_call.method_name;
        if (strcmp(method, "register") == 0 || strcmp(method, "register_up") == 0) {
            int sql_idx = (strcmp(method, "register") == 0) ? 1 : 1; // up_sql is at 1 or we can check all arguments
            for (int i = 0; i < node->dispatch_call.arg_count; i++) {
                AstNode *arg = node->dispatch_call.args[i];
                if (arg && arg->kind == NODE_STRING_LITERAL) {
                    const char *sql = arg->literal.string_value;
                    if (sql && lint_strcasestr(sql, "DROP TABLE")) {
                        report_lint(walker->ctx, arg->loc, "security", "Unsafe raw SQL DROP TABLE statement in migration");
                    }
                }
            }
        }

        // D. Insecure rate limiting calls (cors/rate_limit in clusters)
        // TODO: Implement proper cross-module analysis. The naive approach
        // of flagging every listen_cluster call produces too many false
        // positives (e.g. when the rate limiter is redis-backed and safe).
    }
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

        case NODE_EXPR_STMT: {
            AstNode *expr = node->expr_stmt.expr;
            if (expr) {
                // If it is a fetch() call, check if it's unterminated.
                if (expr->kind == NODE_CALL && expr->call.callee && expr->call.callee->kind == NODE_IDENTIFIER &&
                    strcmp(expr->call.callee->identifier.name, "fetch") == 0) {
                    report_lint(walker->ctx, expr->loc, "unused", "Unused FetchRequest: construct has no terminal method call (e.g. .get() or .post()).");
                } else if (expr->kind == NODE_DISPATCH_CALL) {
                    // Check if it originates from fetch but doesn't terminate with get/post/json/delete/put
                    // First traverse the chain to see if the root callee is fetch
                    AstNode *curr = expr;
                    while (curr && curr->kind == NODE_DISPATCH_CALL) {
                        curr = curr->dispatch_call.object;
                    }
                    if (curr && curr->kind == NODE_CALL && curr->call.callee && curr->call.callee->kind == NODE_IDENTIFIER &&
                        strcmp(curr->call.callee->identifier.name, "fetch") == 0) {
                        if (!check_has_terminal_fetch_method(expr)) {
                            report_lint(walker->ctx, expr->loc, "unused", "Unused FetchRequest: construct has no terminal method call (e.g. .get() or .post()).");
                        }
                    }
                }
            }
            lint_walk(node->expr_stmt.expr, walker);
            break;
        }

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
            check_module_usages(walker, node);
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
            check_module_usages(walker, node);
            // listen_cluster: rate-limiter-safe when backed by redis (which it is).
            // A proper cross-module check would require full-program analysis.
            if (0) {} // placeholder
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

        case NODE_STRUCT_DECL: {
            /* Duplicate field names: the parser accepts this silently, and
             * whichever field comes last simply shadows the earlier one in
             * generated code -- the first declaration is dead, not an
             * error, which is exactly the kind of bug a typo produces and
             * nothing else currently catches. */
            for (int i = 0; i < node->struct_decl.field_count; i++) {
                for (int j = i + 1; j < node->struct_decl.field_count; j++) {
                    if (strcmp(node->struct_decl.field_names[i], node->struct_decl.field_names[j]) == 0) {
                        report_lint(walker->ctx, node->loc, "duplicate",
                                    "Struct '%s' declares field '%s' more than once",
                                    node->struct_decl.name, node->struct_decl.field_names[i]);
                    }
                }
            }
            for (int i = 0; i < node->struct_decl.decorator_count; i++) {
                if (!is_known_decorator(node->struct_decl.decorator_keys[i])) {
                    report_lint(walker->ctx, node->loc, "unknown-decorator",
                                "Unknown decorator '@%s' on struct '%s' (typo? this silently does nothing)",
                                node->struct_decl.decorator_keys[i], node->struct_decl.name);
                }
            }
            if (node->struct_decl.field_decorator_counts) {
                for (int i = 0; i < node->struct_decl.field_count; i++) {
                    for (int j = 0; j < node->struct_decl.field_decorator_counts[i]; j++) {
                        const char *dname = node->struct_decl.field_decorator_keys[i][j];
                        if (!is_known_decorator(dname)) {
                            report_lint(walker->ctx, node->loc, "unknown-decorator",
                                        "Unknown decorator '@%s' on field '%s' of struct '%s' (typo? this silently does nothing)",
                                        dname, node->struct_decl.field_names[i], node->struct_decl.name);
                        }
                    }
                }
            }
            break;
        }

        case NODE_SCHEMA_DECL: {
            for (int i = 0; i < node->schema_decl.field_count; i++) {
                for (int j = i + 1; j < node->schema_decl.field_count; j++) {
                    if (strcmp(node->schema_decl.field_names[i], node->schema_decl.field_names[j]) == 0) {
                        report_lint(walker->ctx, node->loc, "duplicate",
                                    "Schema '%s' declares field '%s' more than once",
                                    node->schema_decl.name, node->schema_decl.field_names[i]);
                    }
                }
            }
            for (int i = 0; i < node->schema_decl.decorator_count; i++) {
                if (!is_known_decorator(node->schema_decl.decorator_keys[i])) {
                    report_lint(walker->ctx, node->loc, "unknown-decorator",
                                "Unknown decorator '@%s' on schema '%s' (typo? this silently does nothing)",
                                node->schema_decl.decorator_keys[i], node->schema_decl.name);
                }
            }
            if (node->schema_decl.field_decorator_counts) {
                for (int i = 0; i < node->schema_decl.field_count; i++) {
                    for (int j = 0; j < node->schema_decl.field_decorator_counts[i]; j++) {
                        const char *dname = node->schema_decl.field_decorator_keys[i][j];
                        if (!is_known_decorator(dname)) {
                            report_lint(walker->ctx, node->loc, "unknown-decorator",
                                        "Unknown decorator '@%s' on field '%s' of schema '%s' (typo? this silently does nothing)",
                                        dname, node->schema_decl.field_names[i], node->schema_decl.name);
                        }
                    }
                }
            }
            break;
        }

        case NODE_ACTOR_DECL: {
            for (int i = 0; i < node->actor_decl.field_count; i++) {
                for (int j = i + 1; j < node->actor_decl.field_count; j++) {
                    if (strcmp(node->actor_decl.field_names[i], node->actor_decl.field_names[j]) == 0) {
                        report_lint(walker->ctx, node->loc, "duplicate",
                                    "Actor '%s' declares field '%s' more than once",
                                    node->actor_decl.name, node->actor_decl.field_names[i]);
                    }
                }
            }
            break;
        }

        case NODE_ENUM_DECL: {
            for (int i = 0; i < node->enum_decl.variant_count; i++) {
                for (int j = i + 1; j < node->enum_decl.variant_count; j++) {
                    if (strcmp(node->enum_decl.variant_names[i], node->enum_decl.variant_names[j]) == 0) {
                        report_lint(walker->ctx, node->loc, "duplicate",
                                    "Enum '%s' declares variant '%s' more than once",
                                    node->enum_decl.name, node->enum_decl.variant_names[i]);
                    }
                }
            }
            break;
        }

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
                     * -- exempt it to avoid noise. Also exempt any param whose
                     * name starts with '_': that's the universal "intentionally
                     * unused" convention, and it matters here because Lumen event
                     * handlers have a fixed (state, value) arity -- a handler that
                     * ignores the event value (fn click(s, _v)) would otherwise
                     * warn on every single one. Every other param is tracked. */
                    const char *pn = node->fn_decl.param_names[i];
                    scope.bindings[idx].used = (strcmp(pn, "self") == 0) || (pn[0] == '_');
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

        case NODE_CONST_DECL:
            lint_walk(node->let_decl.initializer, walker);
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

        case NODE_QUESTION_DOT:
            lint_walk(node->member.object, walker);
            break;

        case NODE_MATCH_ARM:
            lint_walk(node->match_arm.pattern, walker);
            lint_walk(node->match_arm.body, walker);
            break;

        case NODE_ENUM_LITERAL:
            for (int i = 0; i < node->enum_literal.value_count; i++) {
                lint_walk(node->enum_literal.values[i], walker);
            }
            break;

        case NODE_AWAIT:
            lint_walk(node->await.expr, walker);
            break;

        case NODE_PROPAGATE:
            lint_walk(node->propagate.expr, walker);
            break;

        case NODE_ASSERT:
            lint_walk(node->assert_stmt.condition, walker);
            break;

        case NODE_COMPTIME:
            lint_walk(node->comptime.body, walker);
            break;

        case NODE_BREAK:
        case NODE_CONTINUE:
        case NODE_INT_LITERAL:
        case NODE_FLOAT_LITERAL:
        case NODE_BOOL_LITERAL:
        case NODE_NULL_LITERAL:
        case NODE_TRAIT_DECL:
        case NODE_FFI_DECL:
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

static char *g_cached_prelude = NULL;
static int g_cached_prelude_lines = 0;
static bool g_prelude_cached = false;

static char *get_cached_vn_modules_prelude(int *out_line_count) {
    if (!g_prelude_cached) {
        g_cached_prelude = build_vn_modules_prelude(&g_cached_prelude_lines);
        g_prelude_cached = true;
    }
    *out_line_count = g_cached_prelude_lines;
    return g_cached_prelude;
}

const char *lint_get_vn_prelude(int *out_line_count) {
    return get_cached_vn_modules_prelude(out_line_count);
}

int lint_buffer(const char *user_source, const char *path, LintContext *ctx) {
    /* Files inside vn_modules/ are linted standalone -- prepending the
     * prelude there would mean a file sees (a duplicate of) itself. Other
     * files get the prelude so the parser knows about types/functions
     * (structs, fn signatures, impl methods) declared in vn_modules before
     * trying to parse code that uses them -- without it, e.g. any file using
     * Zenith's `Response { ... }` literal fails to parse at all. */
    bool inside_vn_modules = (strstr(path, "vn_modules/") != NULL);
    char *source = NULL;       /* the buffer actually handed to the lexer */
    int line_offset = 0;
    char *prelude = NULL;
    if (!inside_vn_modules) {
        prelude = get_cached_vn_modules_prelude(&line_offset);
    }
    if (prelude) {
        size_t plen = strlen(prelude);
        size_t olen = strlen(user_source);
        source = (char *)malloc(plen + 1 + olen + 1);
        memcpy(source, prelude, plen);
        source[plen] = '\n';
        memcpy(source + plen + 1, user_source, olen);
        source[plen + 1 + olen] = '\0';
    } else {
        source = strdup(user_source);
    }

    Lexer lexer;
    lexer_init(&lexer, source, path);

    Arena *arena = arena_create(0);
    Parser parser;
    parser_init(&parser, &lexer, arena);

    AstNode *program = parser_parse(&parser);
    if (parser.had_error) {
        const char *emsg = parser_get_error(&parser);
        if (ctx->sink) {
            /* Parser error messages are prefixed "[line:col] ...". Pull the
             * location out, drop the prelude offset, and surface as a syntax
             * diagnostic only if it lands in the user's own code. */
            int el = 0, ec = 0;
            if (sscanf(emsg, "[%d:%d]", &el, &ec) == 2 && el > line_offset) {
                const char *msg_text = strchr(emsg, ']');
                if (msg_text) {
                    msg_text++;
                    while (*msg_text == ' ') msg_text++;
                } else {
                    msg_text = emsg;
                }
                ctx->sink(ctx->sink_ud, el - line_offset + ctx->line_base, ec, "syntax", msg_text);
                ctx->had_error = true;
            }
        } else {
            fprintf(stderr, "Parse error in %s: %s\n", path, emsg);
        }
        arena_destroy(arena);
        free(source);
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
    free(source);
    return 0;
}

/* ─── SFC (Single File Component) helpers ─── */
typedef struct {
    const char *start;
    int len;
    int start_line;
} SfcBlock;

static int count_lines_before(const char *source, const char *pos) {
    int lines = 1;
    for (const char *p = source; p < pos; p++) {
        if (*p == '\n') lines++;
    }
    return lines;
}

static bool find_sfc_block(const char *source, const char *tag, SfcBlock *block) {
    char open_tag[64];
    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    char close_tag[64];
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);
    const char *open = strstr(source, open_tag);
    if (!open) return false;
    const char *close = strstr(open + strlen(open_tag), close_tag);
    if (!close) return false;
    block->start = open + strlen(open_tag);
    block->len = (int)(close - block->start);
    block->start_line = count_lines_before(source, block->start);
    return true;
}

/* ─── New Lumen/Aurora lint rules ─── */

static bool is_known_lumen_component(const char *name, int name_len) {
    static const char *builtins[] = {
        "Page", "Container", "Section", "Stack", "Row", "Grid",
        "Card", "Heading", "Text", "Eyebrow", "Button", "Badge",
        "Feature", "Divider", "Spacer", "Hero", "Nav", "Footer",
        "Split", "Field", "Stat", "Tag", "Avatar", "Alert",
        "Empty", "Skeleton"
    };
    for (size_t i = 0; i < sizeof(builtins)/sizeof(builtins[0]); i++) {
        if ((int)strlen(builtins[i]) == name_len &&
            strncmp(builtins[i], name, name_len) == 0) return true;
    }
    return false;
}

/* 1.3a: --lmn- token typo */
static void check_lmn_typo(LintContext *ctx, const char *text, int text_len, int base_line) {
    (void)text_len;
    const char *p = text;
    const char *end = text + text_len;
    while (p < end) {
        const char *found = lint_strcasestr(p, "--lmn-");
        if (!found || found >= end) break;
        int line = base_line;
        for (const char *t = text; t < found; t++) {
            if (*t == '\n') line++;
        }
        SourceLoc loc;
        loc.filename = NULL;
        loc.line = line;
        loc.column = 0;
        report_lint(ctx, loc, "style",
                    "'--lmn-' looks like a typo — use '--lumen-' instead");
        p = found + 6;
    }
}

/* 1.3b: Hardcoded color (#rrggbb or rgb(...)) in style block */
static bool is_lumen_color_token(const char *name, int name_len) {
    if (name_len < 8) return false;
    if (strncmp(name, "--lumen-", 8) != 0) return false;
    return true;
}

static void check_hardcoded_color(LintContext *ctx, const char *text, int text_len, int base_line) {
    const char *end = text + text_len;
    const char *p = text;
    while (p < end) {
        int line = base_line;
        for (const char *t = text; t < p; t++) {
            if (*t == '\n') line++;
        }
        /* #rrggbb / #rgb */
        if (*p == '#' && (p + 7 <= end || p + 4 <= end)) {
            bool is_hex = true;
            int hex_len = 0;
            const char *h = p + 1;
            while (h < end && ((*h >= '0' && *h <= '9') || (*h >= 'a' && *h <= 'f') || (*h >= 'A' && *h <= 'F'))) {
                hex_len++;
                h++;
            }
            if ((hex_len == 6 || hex_len == 3) && (h == end || *h == ';' || *h == ' ' || *h == '\t' || *h == '\n' || *h == ',' || *h == ')' || *h == '!' || *h == '.')) {
                /* Only flag if var(--lumen-*) exists in the same block */
                if (lint_strcasestr(text, "--lumen-")) {
                    char hex_str[8];
                    int copy = hex_len < 7 ? hex_len : 6;
                    memcpy(hex_str, p + 1, copy);
                    hex_str[copy] = '\0';
                    SourceLoc loc;
                    loc.filename = NULL;
                    loc.line = line;
                    loc.column = 0;
                    report_lint(ctx, loc, "style",
                                "Hardcoded color '#%s' — consider using a --lumen-* token instead", hex_str);
                }
                p = h;
                continue;
            }
        }
        /* rgb(...) or rgba(...) */
        if ((strncmp(p, "rgb(", 4) == 0 || strncmp(p, "rgba(", 5) == 0) && lint_strcasestr(text, "--lumen-")) {
            SourceLoc loc;
            loc.filename = NULL;
            loc.line = line;
            loc.column = 0;
            report_lint(ctx, loc, "style",
                        "Hardcoded rgb/rgba value — consider using a --lumen-* token instead");
            while (p < end && *p != ';' && *p != '}') p++;
            continue;
        }
        p++;
    }
}

/* 1.3c: Unknown component tag in template */
static void check_unknown_component(LintContext *ctx, const char *text, int text_len, int base_line) {
    const char *end = text + text_len;
    const char *p = text;
    while (p < end) {
        if (*p != '<') { p++; continue; }
        p++;
        if (p >= end) break;
        if (*p == '!' && p + 2 < end && p[1] == '-' && p[2] == '-') {
            p += 3;
            while (p < end) {
                if (*p == '-' && p + 2 < end && p[1] == '-' && p[2] == '>') { p += 3; break; }
                p++;
            }
            continue;
        }
        if (*p == '/') { p++; continue; }
        if (*p >= 'A' && *p <= 'Z') {
            const char *tag_start = p;
            while (p < end && *p != ' ' && *p != '>' && *p != '/' && *p != '\n' && *p != '\t' && *p != '\r') p++;
            int tag_len = (int)(p - tag_start);
            if (tag_len > 0 && !is_known_lumen_component(tag_start, tag_len)) {
                char tag_name[128];
                int copy = tag_len < 127 ? tag_len : 127;
                memcpy(tag_name, tag_start, copy);
                tag_name[copy] = '\0';
                int line = base_line;
                for (const char *t = text; t < tag_start; t++) {
                    if (*t == '\n') line++;
                }
                SourceLoc loc;
                loc.filename = NULL;
                loc.line = line;
                loc.column = 0;
                report_lint(ctx, loc, "correctness",
                            "Unknown component tag '<%s>' — not a built-in Lumen component", tag_name);
            }
        }
    }
}

/* 1.3d: Client-JS advisory */
static void check_client_js_advisory(LintContext *ctx, const char *source, int source_len, int base_line) {
    (void)source_len;
    (void)base_line;
    const char *p = source;
    while (*p) {
        if (strncmp(p, "<client>", 8) == 0) {
            const char *block_start = p + 8;
            const char *block_end = strstr(block_start, "</client>");
            if (!block_end) break;
            bool only_fetch_or_dom = true;
            const char *b = block_start;
            while (b < block_end) {
                while (b < block_end && (*b == ' ' || *b == '\t' || *b == '\n' || *b == '\r')) b++;
                if (b >= block_end) break;
                if (strncmp(b, "fetch(", 6) == 0 ||
                    strstr(b, "document.querySelector") == b ||
                    strstr(b, "document.getElementById") == b) {
                    while (b < block_end && *b != ';' && *b != '\n') b++;
                    if (b < block_end && *b == ';') b++;
                    continue;
                }
                if (strstr(b, "classList.") ||
                    strstr(b, "style.display") ||
                    strstr(b, "innerText") == b ||
                    strstr(b, "textContent") == b) {
                    while (b < block_end && *b != ';' && *b != '\n') b++;
                    if (b < block_end && *b == ';') b++;
                    continue;
                }
                only_fetch_or_dom = false;
                break;
            }
            if (only_fetch_or_dom) {
                int line = 1;
                for (const char *t = source; t < p; t++) if (*t == '\n') line++;
                SourceLoc loc;
                loc.filename = NULL;
                loc.line = line;
                loc.column = 0;
                report_lint(ctx, loc, "style",
                    "Client JS block only uses fetch/toggle — consider an on= handler instead");
            }
            p = block_end + 9;
        } else {
            p++;
        }
    }
}

static void check_no_js_extension(LintContext *ctx, const char *source, int source_len, int base_line) {
    (void)source_len;
    (void)base_line;
    if (lint_strcasestr(source, ".js")) {
        int line = 1;
        const char *p = source;
        const char *match = lint_strcasestr(source, ".js");
        while (p < match) {
            if (*p == '\n') line++;
            p++;
        }
        SourceLoc loc;
        loc.filename = NULL;
        loc.line = line;
        loc.column = 0;
        report_lint(ctx, loc, "correctness", "Raw .js usages are not allowed in strictly TypeScript Aurora. Use .ts instead.");
    }
}

static int lint_lumen_file(const char *source, const char *path, LintContext *ctx) {
    SfcBlock script = {0}, tblock = {0}, sblock = {0};
    bool has_script = find_sfc_block(source, "script", &script);
    bool has_template = find_sfc_block(source, "template", &tblock);
    bool has_style = find_sfc_block(source, "style", &sblock);

    int saved_offset = ctx->line_offset;
    int saved_base = ctx->line_base;
    const char *saved_path = ctx->current_path;
    ctx->line_offset = 0;
    ctx->line_base = 0;
    ctx->current_path = path;

    if (has_template) {
        check_lmn_typo(ctx, tblock.start, tblock.len, tblock.start_line);
        check_unknown_component(ctx, tblock.start, tblock.len, tblock.start_line);
    }
    if (has_style) {
        check_lmn_typo(ctx, sblock.start, sblock.len, sblock.start_line);
        check_hardcoded_color(ctx, sblock.start, sblock.len, sblock.start_line);
    }
    check_client_js_advisory(ctx, source, (int)strlen(source), 1);
    check_no_js_extension(ctx, source, (int)strlen(source), 1);

    ctx->line_offset = saved_offset;
    ctx->line_base = saved_base;
    ctx->current_path = saved_path;

    if (has_script) {
        char *script_content = (char *)malloc(script.len + 1);
        memcpy(script_content, script.start, script.len);
        script_content[script.len] = '\0';

        ctx->line_base = script.start_line - 1;
        ctx->current_path = path;
        int rc = lint_buffer(script_content, path, ctx);
        free(script_content);
        ctx->line_base = saved_base;
        ctx->current_path = saved_path;
        return rc;
    }

    return ctx->had_error ? 1 : 0;
}

static int lint_file(const char *path, LintContext *ctx) {
    char *own_source = read_file(path);
    if (!own_source) {
        fprintf(stderr, "lint: could not read file '%s'\n", path);
        return 1;
    }
    size_t len = strlen(path);
    if (len > 6 && strcmp(path + len - 6, ".lumen") == 0) {
        int rc = lint_lumen_file(own_source, path, ctx);
        free(own_source);
        return rc;
    }
    int rc = lint_buffer(own_source, path, ctx);
    free(own_source);
    return rc;
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
        } else if (S_ISREG(st.st_mode) && ((nlen > 3 && strcmp(entry->d_name + nlen - 3, ".vn") == 0) || (nlen > 6 && strcmp(entry->d_name + nlen - 6, ".lumen") == 0))) {
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
    ctx.line_offset = 0;
    ctx.line_base = 0;
    ctx.current_path = NULL;
    ctx.sink = NULL;
    ctx.sink_ud = NULL;

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
