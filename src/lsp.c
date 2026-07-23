#include "lsp.h"
#include "lint.h"
#include "semantic.h"
#include "fmt.h"
#include "varian.h"
#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>

#include <ctype.h>

static bool is_lumen_file(const char *uri) {
    if (!uri) return false;
    size_t len = strlen(uri);
    return (len > 6 && strcmp(uri + len - 6, ".lumen") == 0);
}

static char *blank_lumen_html(const char *text) {
    char *res = strdup(text);
    const char *start = strstr(res, "<script>");
    if (!start) {
        for (char *p = res; *p; p++) {
            if (*p != '\n' && *p != '\r') *p = ' ';
        }
        return res;
    }
    start += 8;
    const char *end = strstr(start, "</script>");
    if (!end) end = res + strlen(res);
    for (char *p = res; p < start; p++) {
        if (*p != '\n' && *p != '\r') *p = ' ';
    }
    for (char *p = (char*)end; *p; p++) {
        if (*p != '\n' && *p != '\r') *p = ' ';
    }
    return res;
}

#define MAX_DOCS 32
static struct {
    char *uri;
    char *text;
} g_docs[MAX_DOCS];

static AstNode *parse_doc(const char *source, const char *path, Arena **arena_out,
                          int *out_line_offset, int *out_user_offset);
static int g_doc_count = 0;

static void update_doc(const char *uri, const char *text) {
    for (int i = 0; i < g_doc_count; i++) {
        if (strcmp(g_docs[i].uri, uri) == 0) {
            free(g_docs[i].text);
            g_docs[i].text = strdup(text);
            return;
        }
    }
    if (g_doc_count < MAX_DOCS) {
        g_docs[g_doc_count].uri = strdup(uri);
        g_docs[g_doc_count].text = strdup(text);
        g_doc_count++;
    }
}

static const char *get_doc(const char *uri) {
    for (int i = 0; i < g_doc_count; i++) {
        if (strcmp(g_docs[i].uri, uri) == 0) {
            return g_docs[i].text;
        }
    }
    return NULL;
}

static char *decode_json_string(const char **p) {
    if (**p != '"') return NULL;
    (*p)++;
    size_t cap = 256;
    size_t len = 0;
    char *res = malloc(cap);
    while (**p && **p != '"') {
        if (**p == '\\') {
            (*p)++;
            if (!**p) break;
            char c = **p;
            if (c == 'n') res[len++] = '\n';
            else if (c == 'r') res[len++] = '\r';
            else if (c == 't') res[len++] = '\t';
            else if (c == '"') res[len++] = '"';
            else if (c == '\\') res[len++] = '\\';
            else res[len++] = c;
            (*p)++;
        } else {
            res[len++] = **p;
            (*p)++;
        }
        if (len + 2 >= cap) {
            cap *= 2;
            res = realloc(res, cap);
        }
    }
    if (**p == '"') (*p)++;
    res[len] = '\0';
    return res;
}

static const char *find_json_key(const char *json, const char *key) {
    char target[256];
    snprintf(target, sizeof(target), "\"%s\"", key);
    size_t target_len = strlen(target);
    const char *p = json;
    while ((p = strstr(p, target)) != NULL) {
        const char *after = p + target_len;
        while (*after && isspace((unsigned char)*after)) after++;
        if (*after == ':') {
            return after + 1;
        }
        p += target_len;
    }
    return NULL;
}

static char *extract_json_string(const char *json, const char *key) {
    const char *p = find_json_key(json, key);
    if (!p) return NULL;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '"') {
        return decode_json_string(&p);
    }
    return NULL;
}

static int extract_json_int(const char *json, const char *key) {
    const char *p = find_json_key(json, key);
    if (!p) return 0;
    while (*p && isspace((unsigned char)*p)) p++;
    return atoi(p);
}

static char *get_method(const char *json) {
    return extract_json_string(json, "method");
}

static int get_id(const char *json) {
    const char *p = find_json_key(json, "id");
    if (!p) return -1;
    while (*p && isspace((unsigned char)*p)) p++;
    return atoi(p);
}

static char *get_uri(const char *json) {
    return extract_json_string(json, "uri");
}

static char *get_text(const char *json) {
    return extract_json_string(json, "text");
}

static void send_response(const char *json) {
    printf("Content-Length: %zu\r\n\r\n%s", strlen(json), json);
    fflush(stdout);
}

static char *encode_json_string(const char *text) {
    size_t len = strlen(text);
    size_t cap = len * 2 + 3;
    char *res = malloc(cap);
    size_t i = 0, j = 0;
    res[j++] = '"';
    for (; i < len; i++) {
        if (j + 4 >= cap) {
            cap *= 2;
            res = realloc(res, cap);
        }
        if (text[i] == '\n') { res[j++] = '\\'; res[j++] = 'n'; }
        else if (text[i] == '\r') { res[j++] = '\\'; res[j++] = 'r'; }
        else if (text[i] == '\t') { res[j++] = '\\'; res[j++] = 't'; }
        else if (text[i] == '"') { res[j++] = '\\'; res[j++] = '"'; }
        else if (text[i] == '\\') { res[j++] = '\\'; res[j++] = '\\'; }
        else { res[j++] = text[i]; }
    }
    res[j++] = '"';
    res[j] = '\0';
    return res;
}

typedef struct {
    char *diags_json;
    size_t diags_len;
    size_t diags_cap;
    bool first;
} LspLintSink;

static void lsp_lint_sink(void *ud, int line, int column, const char *category, const char *msg) {
    LspLintSink *sink = (LspLintSink *)ud;
    if (!sink->first) {
        if (sink->diags_len + 2 >= sink->diags_cap) {
            sink->diags_cap *= 2;
            sink->diags_json = realloc(sink->diags_json, sink->diags_cap);
        }
        strcat(sink->diags_json, ",");
        sink->diags_len++;
    }
    sink->first = false;

    int l0 = (line > 0) ? line - 1 : 0;
    int c0 = (column > 0) ? column - 1 : 0;

    char *msg_enc = encode_json_string(msg);
    int severity = (strcmp(category, "syntax") == 0) ? 1 : 2;

    char buf[2048];
    snprintf(buf, sizeof(buf),
             "{\"range\":{\"start\":{\"line\":%d,\"character\":%d},\"end\":{\"line\":%d,\"character\":%d}},"
             "\"severity\":%d,\"source\":\"VarianLint\",\"message\":%s}",
             l0, c0, l0, c0 + 1, severity, msg_enc);
    free(msg_enc);

    size_t needed = strlen(buf) + 1;
    if (sink->diags_len + needed >= sink->diags_cap) {
        sink->diags_cap *= 2;
        if (sink->diags_len + needed >= sink->diags_cap) {
            sink->diags_cap = sink->diags_len + needed + 2048;
        }
        sink->diags_json = realloc(sink->diags_json, sink->diags_cap);
    }
    strcat(sink->diags_json, buf);
    sink->diags_len += strlen(buf);
}

static void run_lint_and_publish(const char *uri, const char *text) {
    LspLintSink sink = {0};
    sink.diags_cap = 2048;
    sink.diags_json = malloc(sink.diags_cap);
    sink.diags_json[0] = '\0';
    sink.first = true;

    LintContext ctx = {0};
    ctx.sink = lsp_lint_sink;
    ctx.sink_ud = &sink;

    char *processed = is_lumen_file(uri) ? blank_lumen_html(text) : strdup(text);
    lint_buffer(processed, uri, &ctx);

    int sem_line_offset = 0;
    Arena *sem_arena = NULL;
    int sem_user_offset = 0;
    AstNode *sem_program = parse_doc(processed, uri, &sem_arena, &sem_line_offset,
                                     &sem_user_offset);
    if (sem_program) {
        SemanticResult *sem = semantic_analyze(sem_program);
        if (sem) {
            for (int i = 0; i < sem->count; i++) {
                SemanticDiagnostic *diag = &sem->diagnostics[i];
                if (diag->filename && strcmp(diag->filename, "<prelude>") == 0) continue;
                int rel = diag->offset - sem_user_offset;
                if (rel < 0) continue;
                int line = 1, col = 1;
                for (int j = 0; processed[j] && j < rel; j++) {
                    if (processed[j] == '\n') { line++; col = 1; }
                    else col++;
                }
                lsp_lint_sink(&sink, line, col, diag->code, diag->message);
            }
            semantic_result_free(sem);
        }

        /* Cross-check Lumen <template> bindings against the <script> AST */
        if (is_lumen_file(uri)) {
            int handler_count = 0;
            const char *handlers[128];
            for (int i = 0; i < sem_program->program.stmt_count; i++) {
                AstNode *s = sem_program->program.stmts[i];
                if (s->kind == NODE_FN_DECL && strcmp(s->fn_decl.name, "state") != 0) {
                    if (handler_count < 128) {
                        handlers[handler_count++] = s->fn_decl.name;
                    }
                }
            }

            const char *tpl = strstr(text, "<template>");
            if (tpl) {
                const char *tpl_end = strstr(tpl, "</template>");
                if (tpl_end) {
                    const char *p = tpl;
                    while ((p = strchr(p, '@')) != NULL && p < tpl_end) {
                        p++;
                        while (*p && isalpha(*p)) p++;
                        if (*p == '=' && p[1] == '"') {
                            p += 2;
                            const char *h_start = p;
                            while (*p && *p != '"') p++;
                            int h_len = p - h_start;
                            if (h_len > 0) {
                                bool found = false;
                                for (int i = 0; i < handler_count; i++) {
                                    if (strlen(handlers[i]) == (size_t)h_len && strncmp(handlers[i], h_start, h_len) == 0) {
                                        found = true; break;
                                    }
                                }
                                if (!found) {
                                    int l = 0, c = 0;
                                    for (const char *q = text; q < h_start; q++) {
                                        if (*q == '\n') { l++; c = 0; } else { c++; }
                                    }
                                    char msg[256];
                                    snprintf(msg, sizeof(msg), "Lumen error: Handler '%.*s' is bound in template but not defined in <script>", h_len, h_start);
                                    lsp_lint_sink(&sink, l + 1, c + 1, "syntax", msg);
                                }
                            }
                        }
                    }
                }
            }
        }
        if (sem_arena) arena_destroy(sem_arena);
    }
    free(processed);

    char *uri_enc = encode_json_string(uri);
    size_t out_cap = sink.diags_len + strlen(uri_enc) + 256;
    char *out_json = malloc(out_cap);
    snprintf(out_json, out_cap,
             "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":%s,\"diagnostics\":[%s]}}",
             uri_enc, sink.diags_json);
    send_response(out_json);

    free(uri_enc);
    free(out_json);
    free(sink.diags_json);
}

static void handle_formatting(int id, const char *uri) {
    const char *text = get_doc(uri);
    if (!text) {
        char buf[256];
        snprintf(buf, sizeof(buf), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":null}", id);
        send_response(buf);
        return;
    }

    int sl = 0, sc = 0;
    int el = 0, ec = 0;
    char *to_format = NULL;
    size_t len = 0;

    if (is_lumen_file(uri)) {
        const char *tag = strstr(text, "<script>");
        if (!tag) {
            char buf[256];
            snprintf(buf, sizeof(buf), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":null}", id);
            send_response(buf);
            return;
        }
        const char *script_start = tag + 8;
        const char *script_end = strstr(script_start, "</script>");
        if (!script_end) script_end = text + strlen(text);

        for (const char *p = text; p < script_start; p++) {
            if (*p == '\n') { sl++; sc = 0; } else { sc++; }
        }
        el = sl; ec = sc;
        for (const char *p = script_start; p < script_end; p++) {
            if (*p == '\n') { el++; ec = 0; } else { ec++; }
        }
        len = script_end - script_start;
        to_format = malloc(len + 1);
        memcpy(to_format, script_start, len);
        to_format[len] = '\0';
    } else {
        to_format = strdup(text);
        len = strlen(to_format);
        for (const char *p = to_format; *p; p++) {
            if (*p == '\n') { el++; ec = 0; } else { ec++; }
        }
    }

    int out_pos = 0;
    char *formatted = fmt_format_source(to_format, len, &out_pos);
    free(to_format);

    if (!formatted) {
        char buf[256];
        snprintf(buf, sizeof(buf), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":null}", id);
        send_response(buf);
        return;
    }

    char *safe_fmt = malloc(out_pos + 1);
    memcpy(safe_fmt, formatted, out_pos);
    safe_fmt[out_pos] = '\0';

    char *fmt_enc = encode_json_string(safe_fmt);
    size_t out_cap = strlen(fmt_enc) + 512;
    char *out_json = malloc(out_cap);
    snprintf(out_json, out_cap,
             "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":[{\"range\":{\"start\":{\"line\":%d,\"character\":%d},\"end\":{\"line\":%d,\"character\":%d}},\"newText\":%s}]}",
             id, sl, sc, el, ec, fmt_enc);
    send_response(out_json);

    free(safe_fmt);
    free(fmt_enc);
    free(out_json);
    free(formatted);
}

/* ────────────────────────────────────────────────
 *  AST helpers for LSP intelligence features
 * ──────────────────────────────────────────────── */

/* Walk the AST depth-first; find the deepest/innermost node whose span
 * contains (line, col) — both 1‑indexed Varian coordinates.  Returns a
 * pointer into arena memory (do not free). */
static AstNode *find_node_at(AstNode *node, int line, int col, int depth, int *best_depth) {
    if (!node) return NULL;
    AstNode *best = NULL;

    /* Default check: the node starts on the target line at or before col.
     * For identifiers we use the exact identifier bounds. */
    bool cursor_here = false;
    if (node->loc.line == line) {
        if (node->kind == NODE_IDENTIFIER) {
            int start = node->loc.column;
            int end = start + (int)strlen(node->identifier.name);
            if (col >= start && col < end)
                cursor_here = true;
        } else if (node->kind == NODE_STRING_LITERAL && node->literal.string_value) {
            int start = node->loc.column;
            int end = start + 2 + (int)strlen(node->literal.string_value);
            if (col >= start && col < end)
                cursor_here = true;
        } else if (node->kind == NODE_INT_LITERAL || node->kind == NODE_FLOAT_LITERAL || node->kind == NODE_BOOL_LITERAL || node->kind == NODE_NULL_LITERAL) {
            if (node->loc.column <= col)
                cursor_here = true;
        } else {
            if (node->loc.column <= col)
                cursor_here = true;
        }
    } else if (node->loc.line < line) {
        cursor_here = true;
    }

    if (cursor_here && depth >= *best_depth) {
        *best_depth = depth;
        best = node;
    }

    /* Recurse into children — DFS, source order */
    AstNode *child_best = NULL;
    int child_depth = *best_depth;
    int new_depth = depth + 1;

    switch (node->kind) {
    case NODE_PROGRAM:
        for (int i = 0; i < node->program.stmt_count; i++) {
            AstNode *c = find_node_at(node->program.stmts[i], line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        break;
    case NODE_LET_DECL:
        if (node->let_decl.initializer) {
            AstNode *c = find_node_at(node->let_decl.initializer, line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        break;
    case NODE_FN_DECL:
        if (node->fn_decl.body) {
            AstNode *c = find_node_at(node->fn_decl.body, line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        break;
    case NODE_BLOCK:
        for (int i = 0; i < node->block.stmt_count; i++) {
            AstNode *c = find_node_at(node->block.stmts[i], line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        break;
    case NODE_EXPR_STMT:
        if (node->expr_stmt.expr) {
            AstNode *c = find_node_at(node->expr_stmt.expr, line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        break;
    case NODE_IF:
        if (node->if_stmt.condition) {
            AstNode *c = find_node_at(node->if_stmt.condition, line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        if (node->if_stmt.then_branch) {
            AstNode *c = find_node_at(node->if_stmt.then_branch, line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        if (node->if_stmt.else_branch) {
            AstNode *c = find_node_at(node->if_stmt.else_branch, line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        break;
    case NODE_WHILE:
        if (node->while_stmt.condition) {
            AstNode *c = find_node_at(node->while_stmt.condition, line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        if (node->while_stmt.body) {
            AstNode *c = find_node_at(node->while_stmt.body, line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        break;
    case NODE_FOR:
        if (node->for_stmt.iterable) {
            AstNode *c = find_node_at(node->for_stmt.iterable, line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        if (node->for_stmt.body) {
            AstNode *c = find_node_at(node->for_stmt.body, line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        break;
    case NODE_LOOP:
        if (node->loop_stmt.body) {
            AstNode *c = find_node_at(node->loop_stmt.body, line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        break;
    case NODE_RETURN:
        for (int i = 0; i < node->return_stmt.value_count; i++) {
            AstNode *c = find_node_at(node->return_stmt.values[i], line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        break;
    case NODE_ASSIGN:
        if (node->assign.target) {
            AstNode *c = find_node_at(node->assign.target, line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        if (node->assign.value) {
            AstNode *c = find_node_at(node->assign.value, line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        break;
    case NODE_BINARY:
        if (node->binary.left) {
            AstNode *c = find_node_at(node->binary.left, line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        if (node->binary.right) {
            AstNode *c = find_node_at(node->binary.right, line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        break;
    case NODE_UNARY:
        if (node->unary.operand) {
            AstNode *c = find_node_at(node->unary.operand, line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        break;
    case NODE_CALL:
        if (node->call.callee) {
            AstNode *c = find_node_at(node->call.callee, line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        for (int i = 0; i < node->call.arg_count; i++) {
            AstNode *c = find_node_at(node->call.args[i], line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        break;
    case NODE_INDEX:
        if (node->index.object) {
            AstNode *c = find_node_at(node->index.object, line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        if (node->index.index) {
            AstNode *c = find_node_at(node->index.index, line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        break;
    case NODE_DISPATCH_CALL:
        if (node->dispatch_call.object) {
            AstNode *c = find_node_at(node->dispatch_call.object, line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        for (int i = 0; i < node->dispatch_call.arg_count; i++) {
            AstNode *c = find_node_at(node->dispatch_call.args[i], line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        break;
    case NODE_MEMBER:
        if (node->member.object) {
            AstNode *c = find_node_at(node->member.object, line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        break;
    case NODE_INTERPOLATED_STRING:
        for (int i = 0; i < node->interpolated_string.part_count; i++) {
            AstNode *c = find_node_at(node->interpolated_string.parts[i], line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        break;
    case NODE_ARRAY_LITERAL:
        for (int i = 0; i < node->array_literal.element_count; i++) {
            AstNode *c = find_node_at(node->array_literal.elements[i], line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        break;
    case NODE_TUPLE_LITERAL:
        for (int i = 0; i < node->tuple_literal.element_count; i++) {
            AstNode *c = find_node_at(node->tuple_literal.elements[i], line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        break;
    case NODE_MATCH:
        if (node->match_stmt.value) {
            AstNode *c = find_node_at(node->match_stmt.value, line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        for (int i = 0; i < node->match_stmt.arm_count; i++) {
            AstNode *c = find_node_at(node->match_stmt.arms[i], line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        break;
    case NODE_MATCH_ARM:
        if (node->match_arm.body) {
            AstNode *c = find_node_at(node->match_arm.body, line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        break;
    case NODE_STRUCT_LITERAL:
        for (int i = 0; i < node->struct_literal.field_count; i++) {
            AstNode *c = find_node_at(node->struct_literal.field_values[i], line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        break;
    case NODE_TRY:
        if (node->try_stmt.try_body) {
            AstNode *c = find_node_at(node->try_stmt.try_body, line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        if (node->try_stmt.catch_body) {
            AstNode *c = find_node_at(node->try_stmt.catch_body, line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        break;
    case NODE_CHAN_SEND:
        if (node->chan_send.channel) {
            AstNode *c = find_node_at(node->chan_send.channel, line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        if (node->chan_send.value) {
            AstNode *c = find_node_at(node->chan_send.value, line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        break;
    case NODE_CHAN_RECEIVE:
        if (node->chan_receive.channel) {
            AstNode *c = find_node_at(node->chan_receive.channel, line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        break;
    case NODE_AWAIT:
        if (node->await.expr) {
            AstNode *c = find_node_at(node->await.expr, line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        break;
    case NODE_COMPTIME:
        if (node->comptime.body) {
            AstNode *c = find_node_at(node->comptime.body, line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        break;
    case NODE_ASSERT:
        if (node->assert_stmt.condition) {
            AstNode *c = find_node_at(node->assert_stmt.condition, line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        break;
    case NODE_TEST:
        if (node->test_decl.body) {
            AstNode *c = find_node_at(node->test_decl.body, line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        break;
    case NODE_PROPAGATE:
        if (node->propagate.expr) {
            AstNode *c = find_node_at(node->propagate.expr, line, col, new_depth, &child_depth);
            if (c) child_best = c;
        }
        break;
    default:
        break;
    }

    if (child_best) return child_best;
    return best;
}

/* Build a short human‑readable "signature" for a declaration node.
 * Returns a malloc'd string (caller frees). */
/* Format a Type into the given buffer. Returns number of chars written. */
static int type_to_str(Type *t, char *buf, int cap) {
    if (!t) return snprintf(buf, cap, "(unknown)");
    switch (t->kind) {
    case TYPE_PRIMITIVE: {
        const char *names[] = {
            [PRIMITIVE_BOOL] = "bool", [PRIMITIVE_INT] = "int",
            [PRIMITIVE_FLOAT] = "float", [PRIMITIVE_STRING] = "string",
            [PRIMITIVE_BYTE] = "byte", [PRIMITIVE_VOID] = "void",
            [PRIMITIVE_PTR] = "ptr", [PRIMITIVE_C_INT] = "c_int",
            [PRIMITIVE_C_DOUBLE] = "c_double", [PRIMITIVE_C_FLOAT] = "c_float",
            [PRIMITIVE_C_CHAR] = "c_char",
        };
        if ((int)t->primitive < (int)(sizeof(names)/sizeof(names[0])) && names[t->primitive])
            return snprintf(buf, cap, "%s", names[t->primitive]);
        return snprintf(buf, cap, "primitive(%d)", t->primitive);
    }
    case TYPE_NAMED:
        return snprintf(buf, cap, "%s", t->named.name);
    case TYPE_ARRAY: {
        char elem[64];
        type_to_str(t->array.element_type, elem, sizeof(elem));
        return snprintf(buf, cap, "[%s]", elem);
    }
    case TYPE_TUPLE: {
        int pos = snprintf(buf, cap, "(");
        for (int i = 0; i < t->tuple.count && pos < cap - 4; i++) {
            if (i > 0) pos += snprintf(buf + pos, cap - pos, ", ");
            pos += type_to_str(t->tuple.types[i], buf + pos, cap - pos);
        }
        pos += snprintf(buf + pos, cap - pos, ")");
        return pos;
    }
    case TYPE_FUNCTION: {
        int pos = snprintf(buf, cap, "fn(");
        for (int i = 0; i < t->function.param_count && pos < cap - 4; i++) {
            if (i > 0) pos += snprintf(buf + pos, cap - pos, ", ");
            pos += type_to_str(t->function.param_types[i], buf + pos, cap - pos);
        }
        char ret[64];
        type_to_str(t->function.return_type, ret, sizeof(ret));
        pos += snprintf(buf + pos, cap - pos, ") -> %s", ret);
        return pos;
    }
    default:
        return snprintf(buf, cap, "(type)");
    }
}

/* Renders a type annotation for display. Returns the number of bytes written.
 * Anything not recognised renders as nothing at all rather than a guess — an
 * absent annotation reads as untyped, which is honest, whereas a wrong one
 * would be actively misleading in a language where annotations are not
 * enforced anyway. */
static int type_render(const Type *t, char *out, size_t cap) {
    if (!t || cap == 0) return 0;
    switch (t->kind) {
    case TYPE_PRIMITIVE: {
        const char *n = NULL;
        switch (t->primitive) {
        case PRIMITIVE_BOOL:   n = "bool";   break;
        case PRIMITIVE_INT:    n = "int";    break;
        case PRIMITIVE_FLOAT:  n = "float";  break;
        case PRIMITIVE_STRING: n = "string"; break;
        case PRIMITIVE_BYTE:   n = "byte";   break;
        case PRIMITIVE_VOID:   n = "void";   break;
        default: return 0;  /* FFI primitives: not part of surface syntax */
        }
        return snprintf(out, cap, "%s", n);
    }
    case TYPE_NAMED:
        if (!t->named.name) return 0;
        return snprintf(out, cap, "%s", t->named.name);
    case TYPE_ARRAY: {
        char inner[128] = {0};
        if (!type_render(t->array.element_type, inner, sizeof(inner))) return 0;
        return snprintf(out, cap, "[%s]", inner);
    }
    default:
        /* TYPE_FUNCTION / TYPE_TUPLE nested inside a signature: omitted rather
         * than rendered half-correctly. */
        return 0;
    }
}

/* Builds "name: type, other" for a function's parameter list. Falls back to
 * bare names wherever an annotation is missing or unrenderable.
 *
 * KNOWN LIMITATION: parser.c:611 gives every UNANNOTATED parameter a synthetic
/* Render parameters. Parameter types are only rendered if the author explicitly
 * wrote a type annotation (tracked via node->fn_decl.param_type_explicit).
 * Unannotated parameters render bare. */
static void params_render(AstNode *node, char *out, size_t cap) {
    out[0] = '\0';
    size_t used = 0;
    const Type *fn_type = node->fn_decl.fn_type;
    /* Only trust fn_type's parameter types if the arity actually agrees —
     * a mismatch means the two came from different parses and pairing them
     * positionally would mislabel every parameter. */
    bool have_types = fn_type && fn_type->kind == TYPE_FUNCTION &&
                      fn_type->function.param_count == node->fn_decl.param_count;

    for (int i = 0; i < node->fn_decl.param_count; i++) {
        const char *pname = node->fn_decl.param_names
                                ? node->fn_decl.param_names[i] : NULL;
        if (!pname) continue;

        char tbuf[128] = {0};
        bool is_explicit = node->fn_decl.param_type_explicit ?
                            node->fn_decl.param_type_explicit[i] : false;
        if (have_types && is_explicit)
            type_render(fn_type->function.param_types[i], tbuf, sizeof(tbuf));

        int n = snprintf(out + used, cap - used, "%s%s%s%s",
                         used ? ", " : "", pname,
                         tbuf[0] ? ": " : "", tbuf);
        if (n < 0 || (size_t)n >= cap - used) { out[used] = '\0'; break; }
        used += (size_t)n;
    }
}

/* Renders `keyword Name { a, b, c }` across multiple lines, with types where
 * the declaration actually retained them. Long member lists are truncated so a
 * hover card stays readable instead of covering the editor. */
static char *members_signature(const char *keyword, const char *name,
                               bool generic, char **members, Type **types,
                               int count) {
    const int MAX_SHOWN = 24;
    size_t cap = 256 + (size_t)count * 96;
    char *out = malloc(cap);
    int n = snprintf(out, cap, "%s %s%s", keyword, name ? name : "?",
                     generic ? "<T>" : "");
    if (count <= 0) return out;

    n += snprintf(out + n, cap - (size_t)n, " {\n");
    int shown = count < MAX_SHOWN ? count : MAX_SHOWN;
    for (int i = 0; i < shown; i++) {
        char tbuf[128] = {0};
        if (types && types[i]) type_render(types[i], tbuf, sizeof(tbuf));
        n += snprintf(out + n, cap - (size_t)n, "    %s%s%s%s\n",
                      members[i] ? members[i] : "?",
                      tbuf[0] ? ": " : "", tbuf,
                      i + 1 < count ? "," : "");
    }
    if (count > shown)
        n += snprintf(out + n, cap - (size_t)n, "    // ... %d more\n", count - shown);
    snprintf(out + n, cap - (size_t)n, "}");
    return out;
}

static char *decl_signature(AstNode *node) {
    char buf[1024];
    switch (node->kind) {
    case NODE_FN_DECL: {
        char params[512];
        params_render(node, params, sizeof(params));

        /* Return type. `void` is suppressed: the parser synthesises it for any
         * function without an explicit `->`, so printing it would assert
         * something the author never wrote. See the note on synthetic `int`
         * parameter types in params_render. */
        char ret[128] = {0};
        const Type *fn_type = node->fn_decl.fn_type;
        if (fn_type && fn_type->kind == TYPE_FUNCTION) {
            const Type *rt = fn_type->function.return_type;
            if (!(rt && rt->kind == TYPE_PRIMITIVE && rt->primitive == PRIMITIVE_VOID))
                type_render(rt, ret, sizeof(ret));
        }

        if (node->fn_decl.impl_type) {
            snprintf(buf, sizeof(buf), "fn %s.%s(%s)%s%s",
                     node->fn_decl.impl_type, node->fn_decl.name, params,
                     ret[0] ? " -> " : "", ret);
        } else {
            snprintf(buf, sizeof(buf), "%sfn %s(%s)%s%s",
                     node->fn_decl.is_async ? "async " : "",
                     node->fn_decl.name, params,
                     ret[0] ? " -> " : "", ret);
        }
        return strdup(buf);
    }
    /* Structs, schemas and enums list their members. Previously these rendered
     * as `struct Message { .. } (3 fields)`, which withheld the one thing the
     * reader actually hovered to find out. */
    case NODE_STRUCT_DECL:
        return members_signature("struct", node->struct_decl.name,
                                 node->struct_decl.type_param_count > 0,
                                 node->struct_decl.field_names,
                                 NULL,
                                 node->struct_decl.field_count);
    case NODE_SCHEMA_DECL:
        /* Only `schema` keeps its field type annotations; a plain `struct`
         * parses and discards them, so there is nothing to show there. */
        return members_signature("schema", node->schema_decl.name,
                                 node->schema_decl.type_param_count > 0,
                                 node->schema_decl.field_names,
                                 node->schema_decl.field_types,
                                 node->schema_decl.field_count);
    case NODE_ENUM_DECL:
        return members_signature("enum", node->enum_decl.name,
                                 node->enum_decl.type_param_count > 0,
                                 node->enum_decl.variant_names,
                                 NULL,
                                 node->enum_decl.variant_count);
    case NODE_ACTOR_DECL:
        snprintf(buf, sizeof(buf), "actor %s (%d fields)",
                 node->actor_decl.name, node->actor_decl.field_count);
        return strdup(buf);
    case NODE_TRAIT_DECL:
        snprintf(buf, sizeof(buf), "trait %s (%d methods)",
                 node->trait_decl.name, node->trait_decl.method_count);
        return strdup(buf);
    case NODE_LET_DECL: {
        char names[512] = {0};
        for (int i = 0; i < node->let_decl.name_count; i++) {
            if (i > 0) strcat(names, ", ");
            strcat(names, node->let_decl.names[i]);
        }
        char type_str[128] = "";
        if (node->let_decl.type)
            type_to_str(node->let_decl.type, type_str, sizeof(type_str));
        if (node->let_decl.is_mutable) {
            if (type_str[0])
                snprintf(buf, sizeof(buf), "mut %s: %s", names, type_str);
            else
                snprintf(buf, sizeof(buf), "mut %s", names);
        } else {
            if (type_str[0])
                snprintf(buf, sizeof(buf), "let %s: %s", names, type_str);
            else
                snprintf(buf, sizeof(buf), "let %s", names);
        }
        return strdup(buf);
    }
    default:
        return strdup("");
    }
}

/* Walk the program's statements (including inside module init functions) to find a declaration matching `name`. */
static AstNode *find_decl(AstNode *node, const char *name) {
    if (!node || !name) return NULL;
    if (node->kind == NODE_PROGRAM) {
        for (int i = 0; i < node->program.stmt_count; i++) {
            AstNode *s = node->program.stmts[i];
            if (!s) continue;
            if (s->kind == NODE_FN_DECL) {
                if (s->fn_decl.is_module_init && s->fn_decl.body) {
                    AstNode *res = find_decl(s->fn_decl.body, name);
                    if (res) return res;
                }
                if (s->fn_decl.name && strcmp(s->fn_decl.name, name) == 0) return s;
            } else if (s->kind == NODE_STRUCT_DECL && strcmp(s->struct_decl.name, name) == 0) {
                return s;
            } else if (s->kind == NODE_SCHEMA_DECL && strcmp(s->schema_decl.name, name) == 0) {
                return s;
            } else if (s->kind == NODE_ENUM_DECL && strcmp(s->enum_decl.name, name) == 0) {
                return s;
            } else if (s->kind == NODE_ACTOR_DECL && strcmp(s->actor_decl.name, name) == 0) {
                return s;
            } else if (s->kind == NODE_TRAIT_DECL && strcmp(s->trait_decl.name, name) == 0) {
                return s;
            } else if (s->kind == NODE_LET_DECL || s->kind == NODE_CONST_DECL) {
                for (int j = 0; j < s->let_decl.name_count; j++) {
                    if (strcmp(s->let_decl.names[j], name) == 0) return s;
                }
            } else if (s->kind == NODE_BLOCK) {
                AstNode *res = find_decl(s, name);
                if (res) return res;
            }
        }
    } else if (node->kind == NODE_BLOCK) {
        for (int i = 0; i < node->block.stmt_count; i++) {
            AstNode *s = node->block.stmts[i];
            if (!s) continue;
            if (s->kind == NODE_FN_DECL) {
                if (s->fn_decl.is_module_init && s->fn_decl.body) {
                    AstNode *res = find_decl(s->fn_decl.body, name);
                    if (res) return res;
                }
                if (s->fn_decl.name && strcmp(s->fn_decl.name, name) == 0) return s;
            } else if (s->kind == NODE_STRUCT_DECL && strcmp(s->struct_decl.name, name) == 0) {
                return s;
            } else if (s->kind == NODE_SCHEMA_DECL && strcmp(s->schema_decl.name, name) == 0) {
                return s;
            } else if (s->kind == NODE_ENUM_DECL && strcmp(s->enum_decl.name, name) == 0) {
                return s;
            } else if (s->kind == NODE_ACTOR_DECL && strcmp(s->actor_decl.name, name) == 0) {
                return s;
            } else if (s->kind == NODE_TRAIT_DECL && strcmp(s->trait_decl.name, name) == 0) {
                return s;
            } else if (s->kind == NODE_LET_DECL || s->kind == NODE_CONST_DECL) {
                for (int j = 0; j < s->let_decl.name_count; j++) {
                    if (strcmp(s->let_decl.names[j], name) == 0) return s;
                }
            }
        }
    }
    return NULL;
}

/* Parse `source` (with vn_modules prelude) into an AST.  Returns the program
 * node on success, NULL on parse error.  *arena_out is set so the caller can
 * arena_destroy when done. */
static AstNode *parse_doc(const char *source, const char *path, Arena **arena_out,
                          int *out_line_offset, int *out_user_offset) {
    bool inside_vn = (strstr(path, "vn_modules/") != NULL);
    char *full_source = NULL;
    int line_offset = 0;
    char *prelude = NULL;
    if (!inside_vn) {
        prelude = (char *)lint_get_vn_prelude(&line_offset);
    }
    if (prelude) {
        size_t plen = strlen(prelude);
        size_t olen = strlen(source);
        full_source = (char *)malloc(plen + 1 + olen + 1);
        memcpy(full_source, prelude, plen);
        full_source[plen] = '\n';
        memcpy(full_source + plen + 1, source, olen);
        full_source[plen + 1 + olen] = '\0';
    } else {
        full_source = is_lumen_file(path) ? blank_lumen_html(source) : strdup(source);
    }
    if (out_user_offset) *out_user_offset = prelude ? (int)strlen(prelude) + 1 : 0;

    Lexer lexer;
    lexer_init(&lexer, full_source, path);
    if (prelude) lexer_set_user_source_offset(&lexer, (int)strlen(prelude) + 1);

    *arena_out = arena_create(0);
    Parser parser;
    parser_init(&parser, &lexer, *arena_out);

    AstNode *program = parser_parse(&parser);
    /* Deliberately keep the tree when the parser reported errors, as long as it
     * produced one. The parser recovers, so a single bad line still yields a
     * usable AST for the rest of the file — and in an editor that is the normal
     * state, not the exception: source is broken while you type it.
     *
     * Discarding it on `parser.had_error` meant one unresolved `use` (or any
     * syntax error anywhere) silently disabled hover, go-to-definition and
     * document symbols for the ENTIRE file, while diagnostics kept working —
     * which reads as "hover is broken" rather than "line 1 has an error". */
    if (!program) {
        arena_destroy(*arena_out);
        *arena_out = NULL;
        free(full_source);
        return NULL;
    }
    free(full_source);
    if (out_line_offset) *out_line_offset = line_offset;
    return program;
}

static char *extract_docstring(const char *source, int decl_line, int line_offset) {
    int target_line = decl_line - 1 - line_offset;
    if (target_line <= 0) return NULL;

    const char *p = source;
    int cur_line = 0;
    while (*p && cur_line < target_line) {
        if (*p == '\n') cur_line++;
        p++;
    }

    const char *lines[100];
    int line_lens[100];
    int count = 0;

    const char *curr = p - 1;
    while (curr > source && count < 100) {
        const char *line_end = curr;
        if (*line_end == '\n') {
            curr--;
            line_end = curr;
            if (curr <= source) break;
        }
        while (curr > source && *curr != '\n') {
            curr--;
        }
        const char *line_start = (curr == source) ? source : curr + 1;

        const char *s = line_start;
        while (s <= line_end && (*s == ' ' || *s == '\t')) s++;

        if (s + 1 <= line_end && s[0] == '/' && s[1] == '/') {
            s += 2;
            if (s <= line_end && *s == '/') s++; /* skip 3rd slash for /// */
            while (s <= line_end && (*s == ' ' || *s == '\t')) s++;
            lines[count] = s;
            line_lens[count] = line_end - s + 1;
            count++;
        } else {
            break;
        }
    }

    if (count == 0) return NULL;

    size_t total = 0;
    for (int i = 0; i < count; i++) total += line_lens[i] + 1;
    char *res = malloc(total + 1);
    res[0] = '\0';
    for (int i = count - 1; i >= 0; i--) {
        strncat(res, lines[i], line_lens[i]);
        strcat(res, "\n");
    }
    return res;
}

static char *get_module_origin(const char *decl_filename, const char *doc_uri) {
    if (!decl_filename || !decl_filename[0]) return NULL;
    if (doc_uri && strstr(doc_uri, decl_filename)) return NULL;

    const char *p = strstr(decl_filename, "vn_modules/");
    if (p) {
        p += 11;
    } else {
        p = strrchr(decl_filename, '/');
        if (p) p++; else p = decl_filename;
    }
    if (!p || !p[0]) return NULL;
    char *mod = strdup(p);
    char *dot = strrchr(mod, '.');
    if (dot) *dot = '\0';
    return mod;
}

static AstNode *find_method_decl(AstNode *node, const char *recv_type, const char *method_name) {
    if (!node || !method_name) return NULL;
    if (node->kind == NODE_PROGRAM) {
        for (int i = 0; i < node->program.stmt_count; i++) {
            AstNode *res = find_method_decl(node->program.stmts[i], recv_type, method_name);
            if (res) return res;
        }
    } else if (node->kind == NODE_BLOCK) {
        for (int i = 0; i < node->block.stmt_count; i++) {
            AstNode *res = find_method_decl(node->block.stmts[i], recv_type, method_name);
            if (res) return res;
        }
    } else if (node->kind == NODE_FN_DECL) {
        if (node->fn_decl.is_method && node->fn_decl.name && strcmp(node->fn_decl.name, method_name) == 0) {
            if (!recv_type || !node->fn_decl.impl_type || strcmp(node->fn_decl.impl_type, recv_type) == 0) {
                return node;
            }
        }
        if (node->fn_decl.is_module_init && node->fn_decl.body) {
            AstNode *res = find_method_decl(node->fn_decl.body, recv_type, method_name);
            if (res) return res;
        }
    }
    return NULL;
}

/* ────────────────────────────────────────────────
 *  handle_hover
 * ──────────────────────────────────────────────── */
#include "native_docs.h"

static const char *resolve_receiver_type(AstNode *program, AstNode *obj) {
    if (!obj) return NULL;
    if (obj->kind == NODE_IDENTIFIER) {
        const char *var_name = obj->identifier.name;
        AstNode *decl = find_decl(program, var_name);
        if (decl && (decl->kind == NODE_LET_DECL || decl->kind == NODE_CONST_DECL)) {
            if (decl->let_decl.type && decl->let_decl.type->kind == TYPE_NAMED) {
                return decl->let_decl.type->named.name;
            }
            if (decl->let_decl.initializer) {
                AstNode *init = decl->let_decl.initializer;
                if (init->kind == NODE_STRUCT_LITERAL) return init->struct_literal.name;
                if (init->kind == NODE_STRING_LITERAL) return "String";
                if (init->kind == NODE_ARRAY_LITERAL) return "Array";
                if (init->kind == NODE_CALL && init->call.callee && init->call.callee->kind == NODE_IDENTIFIER) {
                    const char *cname = init->call.callee->identifier.name;
                    if (strcmp(cname, "fetch") == 0) return "FetchRequest";
                    AstNode *cdecl = find_decl(program, cname);
                    if (cdecl && (cdecl->kind == NODE_STRUCT_DECL || cdecl->kind == NODE_SCHEMA_DECL)) {
                        return cname;
                    }
                }
            }
        }
    } else if (obj->kind == NODE_STRUCT_LITERAL) {
        return obj->struct_literal.name;
    } else if (obj->kind == NODE_STRING_LITERAL) {
        return "String";
    } else if (obj->kind == NODE_ARRAY_LITERAL) {
        return "Array";
    }
    if (obj->type) {
        if (obj->type->kind == TYPE_NAMED) return obj->type->named.name;
        if (obj->type->kind == TYPE_ARRAY) return "Array";
        if (obj->type->kind == TYPE_PRIMITIVE) {
            switch (obj->type->primitive) {
                case PRIMITIVE_STRING: return "String";
                case PRIMITIVE_INT: return "int";
                case PRIMITIVE_FLOAT: return "float";
                case PRIMITIVE_BOOL: return "bool";
                default: break;
            }
        }
    }
    return NULL;
}

static char *lookup_native_doc(const char *name) {
    for (size_t i = 0; i < native_docs_count; i++) {
        if (strcmp(native_docs[i].name, name) == 0) {
            size_t len = strlen(native_docs[i].signature) + strlen(native_docs[i].description) + strlen(native_docs[i].example) + 128;
            char *md = malloc(len);
            if (strlen(native_docs[i].example) > 0) {
                snprintf(md, len, "```varian\n%s\n```\n\n%s\n\n%s", 
                         native_docs[i].signature, native_docs[i].description, native_docs[i].example);
            } else {
                snprintf(md, len, "```varian\n%s\n```\n\n%s", 
                         native_docs[i].signature, native_docs[i].description);
            }
            return md;
        }
    }
    return NULL;
}

#include "lsp_docs.h"

/* Returns a malloc'd copy of the token under (line, col), both 0-based, taken
 * straight from the document text rather than the AST.
 *
 * Needed because the things people point at most — keywords, type names,
 * operators — are never AST nodes, so a node-only hover answers null for them.
 * Works on the raw buffer, so it also still answers inside a region the parser
 * failed on. */
static char *word_at_position(const char *text, int line, int col, bool *is_op) {
    const char *p = text;
    for (int i = 0; i < line && *p; p++)
        if (*p == '\n') i++;
    if (!*p) return NULL;

    const char *line_start = p;
    const char *line_end = strchr(p, '\n');
    int line_len = line_end ? (int)(line_end - line_start) : (int)strlen(line_start);
    if (col < 0 || col > line_len) return NULL;

    *is_op = false;
    int c = col;
    /* A cursor sitting just past the end of a word still refers to that word. */
    if (c == line_len || !(isalnum((unsigned char)line_start[c]) || line_start[c] == '_')) {
        if (c > 0 && (isalnum((unsigned char)line_start[c-1]) || line_start[c-1] == '_')) c--;
    }

    if (isalnum((unsigned char)line_start[c]) || line_start[c] == '_') {
        int s = c, e = c;
        while (s > 0 && (isalnum((unsigned char)line_start[s-1]) || line_start[s-1] == '_')) s--;
        while (e < line_len && (isalnum((unsigned char)line_start[e]) || line_start[e] == '_')) e++;
        if (isdigit((unsigned char)line_start[s])) return NULL;  /* a number, not a word */
        char *w = malloc((size_t)(e - s) + 1);
        memcpy(w, line_start + s, (size_t)(e - s));
        w[e - s] = '\0';
        return w;
    }

    /* Otherwise take the operator run, longest first so `?.` beats `?`. */
    *is_op = true;
    for (int len = 2; len >= 1; len--) {
        int s = col;
        if (s + len > line_len) { if (s > 0) s--; else continue; }
        if (s + len > line_len) continue;
        for (int i = 0; i < lsp_operator_docs_count; i++) {
            const char *op = lsp_operator_docs[i].name;
            if ((int)strlen(op) != len) continue;
            if (strncmp(line_start + s, op, (size_t)len) == 0) return strdup(op);
        }
    }
    return NULL;
}

/* Renders a docs table entry as hover markdown. */
static char *doc_entry_markdown(const LspDocEntry *e) {
    size_t n = strlen(e->signature) + strlen(e->description) + 64;
    char *md = malloc(n);
    snprintf(md, n, "```varian\n%s\n```\n\n%s", e->signature, e->description);
    return md;
}

/* True when the token is being used as a member name rather than as a keyword.
 * The reference is explicit that reserved words are accepted after `.`, so
 * `regex.match(...)` and `resp.not_found` are ordinary property accesses and
 * must not be documented as the `match`/`not` keywords. */
static bool preceded_by_dot(const char *text, int line, int col) {
    const char *p = text;
    for (int i = 0; i < line && *p; p++)
        if (*p == '\n') i++;
    if (!*p) return false;
    int c = col;
    while (c > 0 && (isalnum((unsigned char)p[c-1]) || p[c-1] == '_')) c--;
    while (c > 0 && (p[c-1] == ' ' || p[c-1] == '\t')) c--;
    return c > 0 && p[c-1] == '.';
}

static char *lookup_language_doc(const char *word, bool is_op) {
    if (!word) return NULL;
    const LspDocEntry *tbl = is_op ? lsp_operator_docs : lsp_keyword_docs;
    int count = is_op ? lsp_operator_docs_count : lsp_keyword_docs_count;
    for (int i = 0; i < count; i++)
        if (strcmp(tbl[i].name, word) == 0) return doc_entry_markdown(&tbl[i]);
    return NULL;
}

/* ────────────────────────────────────────────────
 *  handle_hover
 * ──────────────────────────────────────────────── */
static void handle_hover(int id, const char *json, const char *uri) {
    const char *text = get_doc(uri);
    if (!text) {
        char buf[256];
        snprintf(buf, sizeof(buf), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":null}", id);
        send_response(buf);
        return;
    }

    int line_lsp = extract_json_int(json, "line");
    int char_lsp = extract_json_int(json, "character");

    /* Resolved up front so it is available whether or not the file parses. */
    bool word_is_op = false;
    char *cursor_word = word_at_position(text, line_lsp, char_lsp, &word_is_op);

    int line_offset = 0;
    Arena *arena = NULL;
    AstNode *program = parse_doc(text, uri, &arena, &line_offset, NULL);
    if (!program) {
        /* Unparseable file: still answer for keywords, types and operators
         * rather than going silent, which is when hover is most useful. */
        char *md = lookup_language_doc(cursor_word, word_is_op);
        free(cursor_word);
        if (md) {
            char *esc = encode_json_string(md);
            size_t n = strlen(esc) + 128;
            char *buf = malloc(n);
            snprintf(buf, n,
                     "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"contents\":"
                     "{\"kind\":\"markdown\",\"value\":%s}}}", id, esc);
            send_response(buf);
            free(buf); free(esc); free(md);
            return;
        }
        char buf[256];
        snprintf(buf, sizeof(buf), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":null}", id);
        send_response(buf);
        return;
    }

    /* A keyword, primitive type name or operator answers from the language
     * reference immediately, ahead of the AST.
     *
     * It has to come first rather than act as a fallback: find_node_at returns
     * the nearest ENCLOSING node, so a cursor on `while` or `use` resolves to
     * whatever expression surrounds it and reports something confidently wrong
     * ("bool literal" for the `true` in `while true`). These tokens are
     * reserved, so no user symbol can share the name — the only exception is a
     * reserved word used as a member name, which is excluded above. */
    if (cursor_word && !preceded_by_dot(text, line_lsp, char_lsp)) {
        char *doc = lookup_language_doc(cursor_word, word_is_op);
        if (doc) {
            char *esc = encode_json_string(doc);
            size_t n = strlen(esc) + 128;
            char *buf = malloc(n);
            snprintf(buf, n,
                     "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"contents\":"
                     "{\"kind\":\"markdown\",\"value\":%s}}}", id, esc);
            send_response(buf);
            free(buf); free(esc); free(doc); free(cursor_word);
            arena_destroy(arena);
            return;
        }
    }

    /* Convert LSP 0‑indexed → Varian 1‑indexed, then offset by prelude */
    int vline = line_lsp + 1 + line_offset;
    int vcol  = char_lsp + 1;

    int best_depth = -1;
    AstNode *found = find_node_at(program, vline, vcol, 0, &best_depth);

    char *markdown = NULL;
    AstNode *module_origin_decl = NULL;

    /* Helper: check if cursor is on the name part of a declaration */
    #define CURSOR_ON_NAME(node, name_str) ( \
        (name_str) && vcol >= (node)->loc.column && \
        vcol < (node)->loc.column + (int)strlen(name_str) + 1 \
    )

    if (found) {
        switch (found->kind) {
        case NODE_IDENTIFIER: {
            const char *name = found->identifier.name;
            markdown = lookup_native_doc(name);
            if (!markdown) {
                AstNode *decl = find_decl(program, name);
                if (decl) {
                    module_origin_decl = decl;
                    char *sig = decl_signature(decl);
                    char *doc = extract_docstring(text, decl->loc.line, line_offset);
                    size_t mlen = strlen(sig) + (doc ? strlen(doc) : 0) + 128;
                    markdown = malloc(mlen);
                    if (doc) {
                        snprintf(markdown, mlen, "```varian\n%s\n```\n\n%s\n\n**%s**", sig, doc, name);
                        free(doc);
                    } else if (strlen(sig) > 0) {
                        snprintf(markdown, mlen, "```varian\n%s\n```\n\n**%s**", sig, name);
                    } else {
                        snprintf(markdown, mlen, "**%s**", name);
                    }
                    free(sig);
                } else {
                    size_t mlen = strlen(name) + 64;
                    markdown = malloc(mlen);
                    snprintf(markdown, mlen, "**%s**", name);
                }
            }
            break;
        }
        case NODE_FN_DECL:
            if (CURSOR_ON_NAME(found, found->fn_decl.name)) {
                markdown = lookup_native_doc(found->fn_decl.name);
                if (!markdown) {
                    module_origin_decl = found;
                    char *sig = decl_signature(found);
                    char *doc = extract_docstring(text, found->loc.line, line_offset);
                    size_t mlen = strlen(sig) + (doc ? strlen(doc) : 0) + 64;
                    markdown = malloc(mlen);
                    if (doc) {
                        snprintf(markdown, mlen, "```varian\n%s\n```\n\n%s", sig, doc);
                        free(doc);
                    } else {
                        snprintf(markdown, mlen, "```varian\n%s\n```", sig);
                    }
                    free(sig);
                }
            }
            break;
        case NODE_STRUCT_DECL:
            if (CURSOR_ON_NAME(found, found->struct_decl.name)) {
                module_origin_decl = found;
                char *sig = decl_signature(found);
                char *doc = extract_docstring(text, found->loc.line, line_offset);
                size_t mlen = strlen(sig) + (doc ? strlen(doc) : 0) + 64;
                markdown = malloc(mlen);
                if (doc) {
                    snprintf(markdown, mlen, "```varian\n%s\n```\n\n%s", sig, doc);
                    free(doc);
                } else {
                    snprintf(markdown, mlen, "```varian\n%s\n```", sig);
                }
                free(sig);
            }
            break;
        case NODE_SCHEMA_DECL:
            if (CURSOR_ON_NAME(found, found->schema_decl.name)) {
                module_origin_decl = found;
                char *sig = decl_signature(found);
                char *doc = extract_docstring(text, found->loc.line, line_offset);
                size_t mlen = strlen(sig) + (doc ? strlen(doc) : 0) + 64;
                markdown = malloc(mlen);
                if (doc) {
                    snprintf(markdown, mlen, "```varian\n%s\n```\n\n%s", sig, doc);
                    free(doc);
                } else {
                    snprintf(markdown, mlen, "```varian\n%s\n```", sig);
                }
                free(sig);
            }
            break;
        case NODE_ENUM_DECL:
            if (CURSOR_ON_NAME(found, found->enum_decl.name)) {
                module_origin_decl = found;
                char *sig = decl_signature(found);
                char *doc = extract_docstring(text, found->loc.line, line_offset);
                size_t mlen = strlen(sig) + (doc ? strlen(doc) : 0) + 64;
                markdown = malloc(mlen);
                if (doc) {
                    snprintf(markdown, mlen, "```varian\n%s\n```\n\n%s", sig, doc);
                    free(doc);
                } else {
                    snprintf(markdown, mlen, "```varian\n%s\n```", sig);
                }
                free(sig);
            }
            break;
        case NODE_ACTOR_DECL:
            if (CURSOR_ON_NAME(found, found->actor_decl.name)) {
                module_origin_decl = found;
                char *sig = decl_signature(found);
                char *doc = extract_docstring(text, found->loc.line, line_offset);
                size_t mlen = strlen(sig) + (doc ? strlen(doc) : 0) + 64;
                markdown = malloc(mlen);
                if (doc) {
                    snprintf(markdown, mlen, "```varian\n%s\n```\n\n%s", sig, doc);
                    free(doc);
                } else {
                    snprintf(markdown, mlen, "```varian\n%s\n```", sig);
                }
                free(sig);
            }
            break;
        case NODE_TRAIT_DECL:
            if (CURSOR_ON_NAME(found, found->trait_decl.name)) {
                module_origin_decl = found;
                char *sig = decl_signature(found);
                char *doc = extract_docstring(text, found->loc.line, line_offset);
                size_t mlen = strlen(sig) + (doc ? strlen(doc) : 0) + 64;
                markdown = malloc(mlen);
                if (doc) {
                    snprintf(markdown, mlen, "```varian\n%s\n```\n\n%s", sig, doc);
                    free(doc);
                } else {
                    snprintf(markdown, mlen, "```varian\n%s\n```", sig);
                }
                free(sig);
            }
            break;
        case NODE_CALL: {
            const char *fn_name = NULL;
            if (found->call.callee && found->call.callee->kind == NODE_IDENTIFIER)
                fn_name = found->call.callee->identifier.name;
            else if (found->call.callee && found->call.callee->kind == NODE_MEMBER)
                fn_name = found->call.callee->member.member;
            if (fn_name) {
                markdown = lookup_native_doc(fn_name);
                if (!markdown) {
                    AstNode *decl = find_decl(program, fn_name);
                    if (!decl) decl = find_method_decl(program, NULL, fn_name);
                    if (decl) {
                        module_origin_decl = decl;
                        char *sig = decl_signature(decl);
                        char *doc = extract_docstring(text, decl->loc.line, line_offset);
                        size_t mlen = strlen(sig) + (doc ? strlen(doc) : 0) + 128;
                        markdown = malloc(mlen);
                        if (doc) {
                            snprintf(markdown, mlen, "```varian\n%s\n```\n\n%s", sig, doc);
                            free(doc);
                        } else {
                            snprintf(markdown, mlen, "```varian\n%s\n```", sig);
                        }
                        free(sig);
                    }
                }
            }
            if (!markdown) {
                markdown = strdup("function call");
            }
            break;
        }
        case NODE_DISPATCH_CALL: {
            const char *method_name = found->dispatch_call.method_name;
            const char *recv_type = resolve_receiver_type(program, found->dispatch_call.object);
            if (recv_type) {
                char buf[128];
                snprintf(buf, sizeof(buf), "%s.%s", recv_type, method_name);
                markdown = lookup_native_doc(buf);
            }
            if (!markdown && found->dispatch_call.object && found->dispatch_call.object->kind == NODE_IDENTIFIER) {
                char buf[128];
                snprintf(buf, sizeof(buf), "%s.%s", found->dispatch_call.object->identifier.name, method_name);
                markdown = lookup_native_doc(buf);
            }
            if (!markdown) {
                markdown = lookup_native_doc(method_name);
            }
            if (!markdown) {
                AstNode *mdecl = find_method_decl(program, recv_type, method_name);
                if (mdecl) {
                    module_origin_decl = mdecl;
                    char *sig = decl_signature(mdecl);
                    char *doc = extract_docstring(text, mdecl->loc.line, line_offset);
                    size_t mlen = strlen(sig) + (doc ? strlen(doc) : 0) + (recv_type ? strlen(recv_type) : 0) + 128;
                    markdown = malloc(mlen);
                    if (doc) {
                        snprintf(markdown, mlen, "```varian\n%s\n```\n\n%s%s%s%s",
                                 sig, doc,
                                 recv_type ? "\n\nReceiver: `" : "",
                                 recv_type ? recv_type : "",
                                 recv_type ? "`" : "");
                        free(doc);
                    } else {
                        snprintf(markdown, mlen, "```varian\n%s\n```%s%s%s",
                                 sig,
                                 recv_type ? "\n\nReceiver: `" : "",
                                 recv_type ? recv_type : "",
                                 recv_type ? "`" : "");
                    }
                    free(sig);
                }
            }
            if (!markdown) {
                size_t mlen = strlen(method_name) + (recv_type ? strlen(recv_type) : 0) + 128;
                markdown = malloc(mlen);
                if (recv_type) {
                    snprintf(markdown, mlen, "fn %s.%s(...)\n\nReceiver: `%s`", recv_type, method_name, recv_type);
                } else {
                    snprintf(markdown, mlen, "method call: **%s**", method_name);
                }
            }
            break;
        }
        case NODE_MEMBER: {
            size_t mlen = strlen(found->member.member) + 64;
            markdown = malloc(mlen);
            snprintf(markdown, mlen, "member access: **%s**", found->member.member);
            break;
        }
        case NODE_INDEX:
            markdown = strdup("index access");
            break;
        case NODE_QUESTION_DOT:
            markdown = strdup("optional chaining");
            break;
        case NODE_BINARY: {
            const char *op_str = "?";
            switch (found->binary.op) {
                case OP_ADD: op_str = "+"; break; case OP_SUB: op_str = "-"; break;
                case OP_MUL: op_str = "*"; break; case OP_DIV: op_str = "/"; break;
                case OP_MOD: op_str = "%"; break;
                case OP_EQ: op_str = "=="; break; case OP_NE: op_str = "!="; break;
                case OP_LT: op_str = "<"; break; case OP_GT: op_str = ">"; break;
                case OP_LE: op_str = "<="; break; case OP_GE: op_str = ">="; break;
                case OP_AND: op_str = "and"; break; case OP_OR: op_str = "or"; break;
                case OP_BIT_AND: op_str = "&"; break; case OP_BIT_OR: op_str = "|"; break;
                case OP_BIT_XOR: op_str = "^"; break;
                case OP_SHL: op_str = "<<"; break; case OP_SHR: op_str = ">>"; break;
                case OP_NIL_COALESCE: op_str = "??"; break;
            }
            size_t mlen = 128;
            markdown = malloc(mlen);
            snprintf(markdown, mlen, "binary: **%s**", op_str);
            break;
        }
        case NODE_UNARY: {
            const char *uop = found->unary.op == OP_NEG ? "-" :
                              found->unary.op == OP_NOT ? "not" : "~";
            size_t mlen = 64;
            markdown = malloc(mlen);
            snprintf(markdown, mlen, "unary: **%s**", uop);
            break;
        }
        case NODE_STRUCT_LITERAL: {
            const char *sname = found->struct_literal.name;
            AstNode *decl = find_decl(program, sname);
            if (decl) {
                module_origin_decl = decl;
                char *sig = decl_signature(decl);
                char *doc = extract_docstring(text, decl->loc.line, line_offset);
                size_t mlen = strlen(sig) + (doc ? strlen(doc) : 0) + 128;
                markdown = malloc(mlen);
                if (doc) {
                    snprintf(markdown, mlen, "```varian\n%s\n```\n\n%s", sig, doc);
                    free(doc);
                } else {
                    snprintf(markdown, mlen, "```varian\n%s\n```", sig);
                }
                free(sig);
            } else {
                size_t mlen = strlen(sname) + 256;
                for (int i = 0; i < found->struct_literal.field_count; i++) {
                    if (found->struct_literal.field_names[i])
                        mlen += strlen(found->struct_literal.field_names[i]) + 8;
                }
                markdown = malloc(mlen);
                int n = snprintf(markdown, mlen, "```varian\nstruct %s {\n", sname);
                for (int i = 0; i < found->struct_literal.field_count; i++) {
                    n += snprintf(markdown + n, mlen - n, "    %s%s\n",
                                  found->struct_literal.field_names[i],
                                  i + 1 < found->struct_literal.field_count ? "," : "");
                }
                snprintf(markdown + n, mlen - n, "}\n```");
            }
            break;
        }
        case NODE_ENUM_LITERAL: {
            size_t mlen = strlen(found->enum_literal.enum_name) + strlen(found->enum_literal.variant_name) + 128;
            markdown = malloc(mlen);
            snprintf(markdown, mlen, "```varian\n%s::%s\n```", found->enum_literal.enum_name, found->enum_literal.variant_name);
            break;
        }
        case NODE_STRING_LITERAL:
            markdown = strdup("string literal");
            break;
        case NODE_INT_LITERAL:
            markdown = strdup("integer literal");
            break;
        case NODE_FLOAT_LITERAL:
            markdown = strdup("float literal");
            break;
        case NODE_BOOL_LITERAL:
            markdown = strdup("bool literal");
            break;
        case NODE_NULL_LITERAL:
            markdown = strdup("null literal");
            break;
        case NODE_ARRAY_LITERAL:
            markdown = strdup("array literal");
            break;
        case NODE_TUPLE_LITERAL:
            markdown = strdup("tuple literal");
            break;
        case NODE_INTERPOLATED_STRING:
            markdown = strdup("interpolated string");
            break;
        case NODE_IF:
            markdown = strdup("if expression");
            break;
        case NODE_WHILE:
            markdown = strdup("while loop");
            break;
        case NODE_FOR:
            markdown = strdup("for loop");
            break;
        case NODE_LOOP:
            markdown = strdup("infinite loop");
            break;
        case NODE_RETURN:
            markdown = strdup("return");
            break;
        case NODE_BREAK:
            markdown = strdup("break");
            break;
        case NODE_CONTINUE:
            markdown = strdup("continue");
            break;
        case NODE_ASSIGN:
            markdown = strdup("assignment");
            break;
        case NODE_MATCH:
            markdown = strdup("match expression");
            break;
        case NODE_MATCH_ARM:
            markdown = strdup("match arm");
            break;
        case NODE_TRY:
            markdown = strdup("try/catch");
            break;
        case NODE_PROPAGATE:
            markdown = strdup("propagate (?)");
            break;
        case NODE_COMPTIME:
            markdown = strdup("compile-time block");
            break;
        case NODE_ASSERT:
            markdown = strdup("assert");
            break;
        case NODE_BLOCK:
            markdown = strdup("{ ... }");
            break;
        case NODE_EXPR_STMT:
            markdown = strdup("expression");
            break;
        case NODE_FFI_DECL:
            markdown = strdup("FFI declaration");
            break;
        case NODE_TEST:
            markdown = strdup("test");
            break;
        case NODE_LET_DECL:
            markdown = strdup("let declaration");
            break;
        case NODE_CONST_DECL:
            markdown = strdup("const declaration");
            break;
        case NODE_CHAN_SEND:
            markdown = strdup("channel send");
            break;
        case NODE_CHAN_RECEIVE:
            markdown = strdup("channel receive");
            break;
        case NODE_AWAIT:
            markdown = strdup("await");
            break;
        default:
            break;
        }
    }
    #undef CURSOR_ON_NAME

    /* Language reference for keywords, primitive types and operators. Checked
     * after the AST so a user's own symbol always wins over a same-named
     * keyword, but before the generic fallback — these tokens either have no
     * AST node at all, or resolve to one carrying nothing worth showing. */
    if (!markdown || strcmp(markdown, "_(expression)_") == 0) {
        char *doc = lookup_language_doc(cursor_word, word_is_op);
        if (doc) { free(markdown); markdown = doc; }
    }

    if (!markdown) {
        /* Name the construct rather than saying "(expression)", which told the
         * reader nothing and made working hovers look broken. */
        markdown = cursor_word ? NULL : strdup("_(no symbol here)_");
        if (!markdown) {
            size_t n = strlen(cursor_word) + 32;
            markdown = malloc(n);
            snprintf(markdown, n, "`%s`", cursor_word);
        }
    }
    free(cursor_word);

    if (markdown && module_origin_decl && module_origin_decl->loc.filename) {
        char *mod = get_module_origin(module_origin_decl->loc.filename, uri);
        if (mod) {
            size_t new_len = strlen(markdown) + strlen(mod) + 64;
            char *new_md = malloc(new_len);
            snprintf(new_md, new_len, "%s\n\n*From module `%s`*", markdown, mod);
            free(markdown);
            markdown = new_md;
            free(mod);
        }
    }

    char *md_enc = encode_json_string(markdown);
    size_t out_cap = strlen(md_enc) + 256;
    char *out_json = malloc(out_cap);
    snprintf(out_json, out_cap,
             "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"contents\":{\"kind\":\"markdown\",\"value\":%s}}}",
             id, md_enc);
    send_response(out_json);

    free(md_enc);
    free(out_json);
    free(markdown);
    arena_destroy(arena);
}

/* ────────────────────────────────────────────────
 *  handle_completion
 * ──────────────────────────────────────────────── */
static void handle_completion(int id, const char *json, const char *uri) {
    (void)json;
    (void)uri;

    /* Built‑in keywords and type names */
    const char *keywords[] = {
        "let", "const", "fn", "return", "if", "else", "while", "for", "in",
        "loop", "match", "case", "struct", "enum", "actor", "impl", "trait",
        "type", "use", "pub", "mut", "async", "await", "break", "continue",
        "comptime", "try", "catch", "assert", "test", "true", "false", "null",
        "bool", "int", "float", "string", "byte", "void", "self",
        /* Standard library modules */
        "http", "regex", "io", "math", "env", "time", "json", "string", "sqlite",
        "postgres", "redis", "auth", "validate", "sanitize", "crypto", "zenith",
        /* Common methods */
        "len", "push", "get", "post", "query", "split", "trim", "replace",
        "contains", "starts_with", "ends_with", "index_of", "last_index_of",
        "substring", "upper", "lower",
        "read_text", "write_text", "read_bytes", "write_bytes",
        "exists", "delete", "list_dir", "mkdir",
        /* Zenith specific */
        "serve", "serve_tls", "middleware", "json", "html", "text", "param", "status", "redirect",
    };
    int kw_count = sizeof(keywords) / sizeof(keywords[0]);

    /* Build JSON array of completion items */
    size_t cap = 8192;
    size_t len = 0;
    char *items = malloc(cap);
    items[0] = '\0';

    for (int i = 0; i < kw_count; i++) {
        char *label_enc = encode_json_string(keywords[i]);
        char buf[512];
        int kind = 14; /* 14 = Keyword in LSP */
        if (i >= 33 && i <= 38) kind = 6; /* type name → 6 = Class */
        else if (strcmp(keywords[i], "true") == 0 || strcmp(keywords[i], "false") == 0 || strcmp(keywords[i], "null") == 0) kind = 21; /* 21 = Constant */
        else if (strcmp(keywords[i], "self") == 0) kind = 10; /* 10 = Variable */

        snprintf(buf, sizeof(buf), "%s{\"label\":%s,\"kind\":%d}", (len > 0 ? "," : ""), label_enc, kind);
        free(label_enc);

        size_t blen = strlen(buf);
        if (len + blen + 2 >= cap) {
            cap *= 2;
            items = realloc(items, cap);
        }
        strcat(items, buf);
        len += blen;
    }

    size_t out_cap = len + 256;
    char *out_json = malloc(out_cap);
    snprintf(out_json, out_cap,
             "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"isIncomplete\":false,\"items\":[%s]}}",
             id, items);
    send_response(out_json);

    free(items);
    free(out_json);
}

/* ────────────────────────────────────────────────
 *  handle_definition
 * ──────────────────────────────────────────────── */
static void handle_definition(int id, const char *json, const char *uri) {
    const char *text = get_doc(uri);
    if (!text) {
        char buf[256];
        snprintf(buf, sizeof(buf), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":null}", id);
        send_response(buf);
        return;
    }

    int line_lsp = extract_json_int(json, "line");
    int char_lsp = extract_json_int(json, "character");

    int line_offset = 0;
    Arena *arena = NULL;
    AstNode *program = parse_doc(text, uri, &arena, &line_offset, NULL);
    if (!program) {
        char buf[256];
        snprintf(buf, sizeof(buf), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":null}", id);
        send_response(buf);
        return;
    }

    int vline = line_lsp + 1 + line_offset;
    int vcol  = char_lsp + 1;

    int best_depth = -1;
    AstNode *found = find_node_at(program, vline, vcol, 0, &best_depth);

    AstNode *target_decl = NULL;

    if (found) {
        if (found->kind == NODE_IDENTIFIER) {
            target_decl = find_decl(program, found->identifier.name);
        } else if (found->kind == NODE_FN_DECL) {
            target_decl = found; /* definition points to itself */
        } else if (found->kind == NODE_STRUCT_DECL) {
            target_decl = found;
        } else if (found->kind == NODE_SCHEMA_DECL) {
            target_decl = found;
        } else if (found->kind == NODE_ENUM_DECL) {
            target_decl = found;
        } else if (found->kind == NODE_ACTOR_DECL) {
            target_decl = found;
        }
    }

    if (target_decl) {
        /* Skip if declaration is in the prelude */
        int dl_raw = target_decl->loc.line;
        if (dl_raw > line_offset) {
            int dl = dl_raw - 1 - line_offset;
            int dc = target_decl->loc.column - 1;
            char *uri_enc = encode_json_string(uri);
            size_t out_cap = strlen(uri_enc) + 256;
            char *out_json = malloc(out_cap);
            snprintf(out_json, out_cap,
                     "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":"
                     "{\"uri\":%s,\"range\":{\"start\":{\"line\":%d,\"character\":%d},\"end\":{\"line\":%d,\"character\":%d}}}}",
                     id, uri_enc, dl, dc, dl, dc + 1);
            send_response(out_json);
            free(uri_enc);
            free(out_json);
            arena_destroy(arena);
            return;
        }
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":null}", id);
    send_response(buf);
    arena_destroy(arena);
}

/* ────────────────────────────────────────────────
 *  handle_document_symbols
 * ──────────────────────────────────────────────── */
static void handle_document_symbols(int id, const char *json, const char *uri) {
    (void)json;
    const char *text = get_doc(uri);
    if (!text) {
        char buf[256];
        snprintf(buf, sizeof(buf), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":null}", id);
        send_response(buf);
        return;
    }

    int line_offset = 0;
    Arena *arena = NULL;
    AstNode *program = parse_doc(text, uri, &arena, &line_offset, NULL);
    if (!program || program->kind != NODE_PROGRAM) {
        char buf[256];
        snprintf(buf, sizeof(buf), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":null}", id);
        send_response(buf);
        if (arena) arena_destroy(arena);
        return;
    }

    size_t cap = 4096;
    size_t len = 0;
    char *symbols = malloc(cap);
    symbols[0] = '\0';

    for (int i = 0; i < program->program.stmt_count; i++) {
        AstNode *s = program->program.stmts[i];
        /* Skip nodes that belong to the prelude */
        if (s->loc.line <= line_offset) continue;

        int kind_lsp = 0; /* LSP SymbolKind */
        const char *name = NULL;
        SourceLoc *loc = &s->loc;

        switch (s->kind) {
        case NODE_FN_DECL:
            name = s->fn_decl.name;
            kind_lsp = 12; /* Function */
            break;
        case NODE_STRUCT_DECL:
            name = s->struct_decl.name;
            kind_lsp = 5; /* Struct (Class) */
            break;
        case NODE_SCHEMA_DECL:
            name = s->schema_decl.name;
            kind_lsp = 5; /* Struct (Class) */
            break;
        case NODE_ENUM_DECL:
            name = s->enum_decl.name;
            kind_lsp = 10; /* Enum */
            break;
        case NODE_ACTOR_DECL:
            name = s->actor_decl.name;
            kind_lsp = 5; /* Class-like */
            break;
        case NODE_TEST:
            name = s->test_decl.description;
            kind_lsp = 13; /* Method/Test */
            break;
        default:
            break;
        }

        if (name) {
            int sl = loc->line - 1 - line_offset;
            int sc = loc->column - 1;
            char *name_enc = encode_json_string(name);
            char buf[1024];
            snprintf(buf, sizeof(buf),
                     "%s{\"name\":%s,\"kind\":%d,\"range\":{\"start\":{\"line\":%d,\"character\":%d},\"end\":{\"line\":%d,\"character\":%d}},\"selectionRange\":{\"start\":{\"line\":%d,\"character\":%d},\"end\":{\"line\":%d,\"character\":%d}}}",
                     (len > 0 ? "," : ""),
                     name_enc, kind_lsp,
                     sl, sc, sl, sc + 1,
                     sl, sc, sl, sc + 1);
            free(name_enc);
            size_t blen = strlen(buf);
            if (len + blen + 2 >= cap) {
                cap *= 2;
                symbols = realloc(symbols, cap);
            }
            strcat(symbols, buf);
            len += blen;
        }
    }

    size_t out_cap = len + 256;
    char *out_json = malloc(out_cap);
    snprintf(out_json, out_cap,
             "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":[%s]}", id, symbols);
    send_response(out_json);

    free(symbols);
    free(out_json);
    arena_destroy(arena);
}

/* ────────────────────────────────────────────────
 *  handle_semantic_tokens  (textDocument/semanticTokens/full)
 *
 *  Lexer-based fallback highlighting — robust even when the document
 *  does not parse, and it complements the tree-sitter grammar in
 *  editors that prefer (or only have) LSP semantic tokens.
 *
 *  Output is the LSP-encoded delta stream: 5 ints per token
 *  [deltaLine, deltaStartChar, length, tokenType, tokenModifiers].
 *  The tokenType indices below MUST match the legend in
 *  handle_initialize.
 * ──────────────────────────────────────────────── */
enum {
    ST_KEYWORD = 0, ST_TYPE, ST_FUNCTION, ST_VARIABLE, ST_PARAMETER,
    ST_PROPERTY, ST_NUMBER, ST_STRING, ST_OPERATOR, ST_DECORATOR, ST_COMMENT
};

static int st_classify(TokenType t) {
    switch (t) {
    case TOKEN_LET: case TOKEN_CONST: case TOKEN_FN: case TOKEN_RETURN:
    case TOKEN_IF: case TOKEN_ELSE: case TOKEN_WHILE: case TOKEN_FOR:
    case TOKEN_IN: case TOKEN_LOOP: case TOKEN_MATCH: case TOKEN_CASE:
    case TOKEN_STRUCT: case TOKEN_ENUM: case TOKEN_ACTOR: case TOKEN_IMPL:
    case TOKEN_TRAIT: case TOKEN_TYPE: case TOKEN_USE: case TOKEN_PUB:
    case TOKEN_MUT: case TOKEN_ASYNC: case TOKEN_AWAIT: case TOKEN_BREAK:
    case TOKEN_CONTINUE: case TOKEN_COMPTIME: case TOKEN_TRY: case TOKEN_CATCH:
    case TOKEN_ASSERT: case TOKEN_TEST: case TOKEN_TRUE: case TOKEN_FALSE:
    case TOKEN_NULL: case TOKEN_AS:
        return ST_KEYWORD;
    case TOKEN_TYPE_BOOL: case TOKEN_TYPE_INT: case TOKEN_TYPE_FLOAT:
    case TOKEN_TYPE_STRING: case TOKEN_TYPE_BYTE: case TOKEN_TYPE_VOID:
        return ST_TYPE;
    case TOKEN_INTEGER: case TOKEN_FLOAT:
        return ST_NUMBER;
    case TOKEN_STRING: case TOKEN_INTERPOLATED_STRING:
    case TOKEN_BYTE_SLICE: case TOKEN_REGEX:
        return ST_STRING;
    case TOKEN_PLUS: case TOKEN_MINUS: case TOKEN_STAR: case TOKEN_SLASH:
    case TOKEN_PERCENT: case TOKEN_AMPERSAND: case TOKEN_PIPE: case TOKEN_CARET:
    case TOKEN_TILDE: case TOKEN_BANG: case TOKEN_LESS: case TOKEN_GREATER:
    case TOKEN_EQUAL: case TOKEN_EQUAL_EQUAL: case TOKEN_BANG_EQUAL:
    case TOKEN_LESS_EQUAL: case TOKEN_GREATER_EQUAL: case TOKEN_PLUS_EQUAL:
    case TOKEN_MINUS_EQUAL: case TOKEN_STAR_EQUAL: case TOKEN_SLASH_EQUAL:
    case TOKEN_ARROW: case TOKEN_FAT_ARROW: case TOKEN_DOT_DOT:
    case TOKEN_DOT_DOT_DOT: case TOKEN_PIPE_PIPE: case TOKEN_AMPERSAND_AMPERSAND:
    case TOKEN_QUESTION_QUESTION: case TOKEN_QUESTION_DOT: case TOKEN_QUESTION:
    case TOKEN_DOUBLE_COLON: case TOKEN_LEFT_ARROW:
        return ST_OPERATOR;
    default:
        return -1;
    }
}

static void handle_semantic_tokens(int id, const char *uri) {
    const char *text = get_doc(uri);
    if (!text) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"data\":[]}}", id);
        send_response(buf);
        return;
    }

    /* For .lumen files only the <script> region is Varian; blank the rest so
     * positions still line up with the original document. */
    char *processed = is_lumen_file(uri) ? blank_lumen_html(text) : strdup(text);

    Lexer lexer;
    lexer_init(&lexer, processed, uri);

    /* Pass 1: collect tokens (need look-ahead/behind for classification). */
    int cap = 1024, n = 0;
    Token *toks = malloc(sizeof(Token) * cap);
    for (;;) {
        Token t = lexer_next(&lexer);
        if (t.type == TOKEN_EOF) { if (t.value) free(t.value); break; }
        if (n >= cap) { cap *= 2; toks = realloc(toks, sizeof(Token) * cap); }
        toks[n++] = t;
        if (n > 500000) break; /* safety guard for pathological input */
    }

    /* Pass 2: classify with context and encode the delta stream. */
    size_t bufcap = 4096, buflen = 0;
    char *data = malloc(bufcap);
    data[0] = '\0';
    int count = 0;
    int prev_line = 0, prev_col = 0;

    for (int i = 0; i < n; i++) {
        Token *t = &toks[i];
        if (t->length <= 0) continue;
        /* Multi-line tokens can't be represented in the delta model — skip. */
        if (memchr(t->start, '\n', t->length)) continue;

        int type = st_classify(t->type);
        if (t->type == TOKEN_IDENTIFIER) {
            TokenType prev = (i > 0) ? toks[i - 1].type : TOKEN_EOF;
            TokenType next = (i + 1 < n) ? toks[i + 1].type : TOKEN_EOF;
            if (prev == TOKEN_AT) type = ST_DECORATOR;
            else if (prev == TOKEN_DOT || prev == TOKEN_QUESTION_DOT) type = ST_PROPERTY;
            else if (next == TOKEN_LPAREN) type = ST_FUNCTION;
            else if (isupper((unsigned char)t->start[0])) type = ST_TYPE;
            else type = ST_VARIABLE;
        } else if (t->type == TOKEN_AT) {
            type = ST_DECORATOR;
        }
        if (type < 0) continue;

        int line = t->line - 1;
        int col = t->column - 1;
        if (line < 0 || col < 0) continue;

        int dl = line - prev_line;
        int dc = (dl == 0) ? col - prev_col : col;
        if (dl < 0 || (dl == 0 && dc < 0)) continue; /* keep stream monotonic */

        char buf[64];
        snprintf(buf, sizeof(buf), "%s%d,%d,%d,%d,0",
                 (count > 0 ? "," : ""), dl, dc, t->length, type);
        size_t bl = strlen(buf);
        if (buflen + bl + 2 >= bufcap) {
            bufcap = (buflen + bl + 2) * 2;
            data = realloc(data, bufcap);
        }
        strcat(data, buf);
        buflen += bl;
        count++;
        prev_line = line;
        prev_col = col;
    }

    for (int i = 0; i < n; i++) if (toks[i].value) free(toks[i].value);
    free(toks);
    free(processed);

    size_t outcap = buflen + 128;
    char *out = malloc(outcap);
    snprintf(out, outcap,
             "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"data\":[%s]}}", id, data);
    send_response(out);
    free(data);
    free(out);
}

/* ────────────────────────────────────────────────
 *  handle_references
 * ──────────────────────────────────────────────── */
static void handle_references(int id, const char *json, const char *uri) {
    const char *text = get_doc(uri);
    if (!text) {
        char buf[256];
        snprintf(buf, sizeof(buf), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":null}", id);
        send_response(buf);
        return;
    }

    int line_lsp = extract_json_int(json, "line");
    int char_lsp = extract_json_int(json, "character");

    int line_offset = 0;
    Arena *arena = NULL;
    AstNode *program = parse_doc(text, uri, &arena, &line_offset, NULL);
    if (!program) {
        char buf[256];
        snprintf(buf, sizeof(buf), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":null}", id);
        send_response(buf);
        return;
    }

    int vline = line_lsp + 1 + line_offset;
    int vcol = char_lsp + 1;

    int best_depth = -1;
    AstNode *found = find_node_at(program, vline, vcol, 0, &best_depth);

    const char *target_name = NULL;
    if (found && found->kind == NODE_IDENTIFIER) {
        target_name = found->identifier.name;
    }

    if (!target_name) {
        char buf[256];
        snprintf(buf, sizeof(buf), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":[]}", id);
        send_response(buf);
        arena_destroy(arena);
        return;
    }

    char *processed = is_lumen_file(uri) ? blank_lumen_html(text) : strdup(text);

    Lexer lexer;
    lexer_init(&lexer, processed, uri);

    size_t cap = 8192;
    size_t len = 0;
    char *items = malloc(cap);
    items[0] = '\0';
    int count = 0;

    char *uri_enc = encode_json_string(uri);
    int target_len = (int)strlen(target_name);

    for (;;) {
        Token t = lexer_next(&lexer);
        if (t.type == TOKEN_EOF) {
            if (t.value) free(t.value);
            break;
        }

        if (t.type == TOKEN_IDENTIFIER && t.length == target_len &&
            strncmp(t.start, target_name, t.length) == 0) {
            int sl = t.line - 1;
            int sc = t.column - 1;

            char buf[1024];
            snprintf(buf, sizeof(buf),
                     "%s{\"uri\":%s,\"range\":{\"start\":{\"line\":%d,\"character\":%d},\"end\":{\"line\":%d,\"character\":%d}}}",
                     (count > 0 ? "," : ""), uri_enc, sl, sc, sl, sc + t.length);

            size_t blen = strlen(buf);
            if (len + blen + 2 >= cap) {
                cap *= 2;
                items = realloc(items, cap);
            }
            strcat(items, buf);
            len += blen;
            count++;
        }

        if (t.value) free(t.value);
    }

    free(uri_enc);
    free(processed);

    size_t out_cap = len + 256;
    char *out_json = malloc(out_cap);
    snprintf(out_json, out_cap,
             "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":[%s]}", id, items);
    send_response(out_json);

    free(items);
    free(out_json);
    arena_destroy(arena);
}

/* ────────────────────────────────────────────────
 *  handle_rename
 * ──────────────────────────────────────────────── */
static void handle_rename(int id, const char *json, const char *uri) {
    const char *text = get_doc(uri);
    if (!text) {
        char buf[256];
        snprintf(buf, sizeof(buf), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":null}", id);
        send_response(buf);
        return;
    }

    char *new_name = extract_json_string(json, "newName");
    if (!new_name) {
        char buf[256];
        snprintf(buf, sizeof(buf), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":null}", id);
        send_response(buf);
        return;
    }

    int line_lsp = extract_json_int(json, "line");
    int char_lsp = extract_json_int(json, "character");

    int line_offset = 0;
    Arena *arena = NULL;
    AstNode *program = parse_doc(text, uri, &arena, &line_offset, NULL);
    if (!program) {
        char buf[256];
        snprintf(buf, sizeof(buf), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":null}", id);
        send_response(buf);
        free(new_name);
        return;
    }

    int vline = line_lsp + 1 + line_offset;
    int vcol = char_lsp + 1;

    int best_depth = -1;
    AstNode *found = find_node_at(program, vline, vcol, 0, &best_depth);

    const char *target_name = NULL;
    if (found && found->kind == NODE_IDENTIFIER) {
        target_name = found->identifier.name;
    }

    if (!target_name) {
        char buf[256];
        snprintf(buf, sizeof(buf), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":null}", id);
        send_response(buf);
        free(new_name);
        arena_destroy(arena);
        return;
    }

    char *processed = is_lumen_file(uri) ? blank_lumen_html(text) : strdup(text);

    Lexer lexer;
    lexer_init(&lexer, processed, uri);

    size_t cap = 8192;
    size_t len = 0;
    char *edits = malloc(cap);
    edits[0] = '\0';
    int count = 0;

    char *new_name_enc = encode_json_string(new_name);
    int target_len = (int)strlen(target_name);

    for (;;) {
        Token t = lexer_next(&lexer);
        if (t.type == TOKEN_EOF) {
            if (t.value) free(t.value);
            break;
        }

        if (t.type == TOKEN_IDENTIFIER && t.length == target_len &&
            strncmp(t.start, target_name, t.length) == 0) {
            int sl = t.line - 1;
            int sc = t.column - 1;

            char buf[1024];
            snprintf(buf, sizeof(buf),
                     "%s{\"range\":{\"start\":{\"line\":%d,\"character\":%d},\"end\":{\"line\":%d,\"character\":%d}},\"newText\":%s}",
                     (count > 0 ? "," : ""), sl, sc, sl, sc + t.length, new_name_enc);

            size_t blen = strlen(buf);
            if (len + blen + 2 >= cap) {
                cap *= 2;
                edits = realloc(edits, cap);
            }
            strcat(edits, buf);
            len += blen;
            count++;
        }

        if (t.value) free(t.value);
    }

    free(new_name_enc);
    free(processed);

    char *uri_enc = encode_json_string(uri);
    size_t out_cap = len + strlen(uri_enc) + 256;
    char *out_json = malloc(out_cap);
    snprintf(out_json, out_cap,
             "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"changes\":{%s:[%s]}}}",
             id, uri_enc, edits);
    send_response(out_json);

    free(uri_enc);
    free(edits);
    free(out_json);
    free(new_name);
    arena_destroy(arena);
}

/* ────────────────────────────────────────────────
 *  handle_signature_help
 * ──────────────────────────────────────────────── */
static void handle_signature_help(int id, const char *json, const char *uri) {
    const char *text = get_doc(uri);
    if (!text) {
        char buf[256];
        snprintf(buf, sizeof(buf), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"signatures\":[]}}", id);
        send_response(buf);
        return;
    }

    int line_lsp = extract_json_int(json, "line");
    int char_lsp = extract_json_int(json, "character");

    int line_offset = 0;
    Arena *arena = NULL;
    AstNode *program = parse_doc(text, uri, &arena, &line_offset, NULL);
    if (!program) {
        char buf[256];
        snprintf(buf, sizeof(buf), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"signatures\":[]}}", id);
        send_response(buf);
        return;
    }

    char *label = NULL;

    /* Best-effort: scan backwards from cursor for '(' on the same line */
    const char *line_start = text;
    int cur_line = 0;
    while (cur_line < line_lsp && *line_start) {
        if (*line_start == '\n') cur_line++;
        line_start++;
    }

    if (*line_start) {
        const char *p = line_start;
        int col = 0;
        const char *paren = NULL;
        while (*p && *p != '\n' && col <= char_lsp) {
            if (*p == '(') paren = p;
            p++;
            col++;
        }

        if (paren && paren > line_start) {
            const char *name_end = paren - 1;
            while (name_end > line_start && isspace((unsigned char)*name_end)) name_end--;
            const char *name_start = name_end;
            while (name_start > line_start && (isalnum((unsigned char)*(name_start - 1)) || *(name_start - 1) == '_')) name_start--;

            int name_len = (int)(name_end - name_start + 1);
            if (name_len > 0 && name_len < 256) {
                char name_buf[256];
                memcpy(name_buf, name_start, name_len);
                name_buf[name_len] = '\0';

                AstNode *decl = find_decl(program, name_buf);
                if (decl) {
                    label = decl_signature(decl);
                } else {
                    char *ndoc = lookup_native_doc(name_buf);
                    if (ndoc) {
                        label = malloc(strlen(name_buf) + 64);
                        snprintf(label, strlen(name_buf) + 64, "fn %s(...)", name_buf);
                        free(ndoc);
                    }
                }
            }
        }
    }

    if (label) {
        char *label_enc = encode_json_string(label);
        size_t out_cap = strlen(label_enc) + 256;
        char *out_json = malloc(out_cap);
        snprintf(out_json, out_cap,
                 "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"signatures\":[{\"label\":%s,\"parameters\":[]}]}}",
                 id, label_enc);
        send_response(out_json);
        free(label_enc);
        free(out_json);
        free(label);
    } else {
        char buf[256];
        snprintf(buf, sizeof(buf), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"signatures\":[]}}", id);
        send_response(buf);
    }

    arena_destroy(arena);
}

/* ────────────────────────────────────────────────
 *  handle_code_action
 * ──────────────────────────────────────────────── */
static void handle_code_action(int id, const char *json, const char *uri) {
    (void)json;
    (void)uri;
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":[]}", id);
    send_response(buf);
}

/* ────────────────────────────────────────────────
 *  handle_folding_range
 * ──────────────────────────────────────────────── */
static void handle_folding_range(int id, const char *json, const char *uri) {
    (void)json;
    const char *text = get_doc(uri);
    if (!text) {
        char buf[256];
        snprintf(buf, sizeof(buf), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":null}", id);
        send_response(buf);
        return;
    }

#define MAX_FOLD_DEPTH 1024
    int brace_stack[MAX_FOLD_DEPTH];
    int depth = 0;

    size_t cap = 8192;
    size_t len = 0;
    char *ranges = malloc(cap);
    ranges[0] = '\0';
    int count = 0;
    int line = 0;

    for (const char *p = text; *p; p++) {
        if (*p == '{') {
            if (depth < MAX_FOLD_DEPTH) {
                brace_stack[depth] = line;
            }
            depth++;
        } else if (*p == '}') {
            if (depth > 0) {
                depth--;
                int start_line = brace_stack[depth];
                if (line > start_line) {
                    char buf[128];
                    snprintf(buf, sizeof(buf), "%s{\"startLine\":%d,\"endLine\":%d}",
                             (count > 0 ? "," : ""), start_line, line);
                    size_t blen = strlen(buf);
                    if (len + blen + 2 >= cap) {
                        cap = (len + blen + 2) * 2;
                        ranges = realloc(ranges, cap);
                    }
                    strcat(ranges, buf);
                    len += blen;
                    count++;
                }
            }
        } else if (*p == '\n') {
            line++;
        }
    }

    size_t out_cap = len + 256;
    char *out_json = malloc(out_cap);
    snprintf(out_json, out_cap,
             "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":[%s]}", id, ranges);
    send_response(out_json);

    free(ranges);
    free(out_json);
#undef MAX_FOLD_DEPTH
}

/* ────────────────────────────────────────────────
 *  Initialize
 * ──────────────────────────────────────────────── */
static void handle_initialize(int id) {
    char buf[2048];
    snprintf(buf, sizeof(buf),
             "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"capabilities\":{"
             "\"textDocumentSync\":1,"
             "\"hoverProvider\":true,"
             "\"completionProvider\":{\"triggerCharacters\":[\".\",\":\"]},"
             "\"definitionProvider\":true,"
             "\"referencesProvider\":true,"
             "\"renameProvider\":true,"
             "\"signatureHelpProvider\":{\"triggerCharacters\":[\"(\",\",\"]},"
             "\"codeActionProvider\":true,"
             "\"foldingRangeProvider\":true,"
             "\"documentFormattingProvider\":true,"
             "\"documentSymbolProvider\":true,"
             "\"semanticTokensProvider\":{\"legend\":{\"tokenTypes\":["
             "\"keyword\",\"type\",\"function\",\"variable\",\"parameter\","
             "\"property\",\"number\",\"string\",\"operator\",\"decorator\",\"comment\""
             "],\"tokenModifiers\":[]},\"full\":true}"
             "}}}", id);
    send_response(buf);
}

int lsp_main(void) {
    char line[1024];
    while (fgets(line, sizeof(line), stdin)) {
        int content_length = -1;
        if (strncmp(line, "Content-Length:", 15) == 0) {
            content_length = atoi(line + 15);
        }
        while (fgets(line, sizeof(line), stdin)) {
            if (strcmp(line, "\r\n") == 0 || strcmp(line, "\n") == 0) {
                break;
            }
            if (strncmp(line, "Content-Length:", 15) == 0) {
                content_length = atoi(line + 15);
            }
        }

        if (content_length < 0) continue;

        char *json = malloc(content_length + 1);
        int read_bytes = 0;
        while (read_bytes < content_length) {
            int c = fgetc(stdin);
            if (c == EOF) break;
            json[read_bytes++] = c;
        }
        json[read_bytes] = '\0';

        char *method = get_method(json);
        int id = get_id(json);

        if (method) {
            if (strcmp(method, "initialize") == 0) {
                handle_initialize(id);
            } else if (strcmp(method, "shutdown") == 0) {
                char buf[256];
                snprintf(buf, sizeof(buf), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":null}", id);
                send_response(buf);
            } else if (strcmp(method, "exit") == 0) {
                free(method);
                free(json);
                break;
            } else if (strcmp(method, "textDocument/didOpen") == 0 ||
                       strcmp(method, "textDocument/didChange") == 0 ||
                       strcmp(method, "textDocument/didSave") == 0) {
                char *uri = get_uri(json);
                char *text = get_text(json);
                if (uri && text) {
                    update_doc(uri, text);
                    run_lint_and_publish(uri, text);
                } else if (uri && strcmp(method, "textDocument/didSave") == 0) {
                    const char *doc_text = get_doc(uri);
                    if (doc_text) {
                        run_lint_and_publish(uri, doc_text);
                    }
                }
                if (uri) free(uri);
                if (text) free(text);
            } else if (strcmp(method, "textDocument/formatting") == 0) {
                char *uri = get_uri(json);
                if (uri) {
                    handle_formatting(id, uri);
                    free(uri);
                }
            } else if (strcmp(method, "textDocument/hover") == 0) {
                char *uri = get_uri(json);
                if (uri) {
                    handle_hover(id, json, uri);
                    free(uri);
                }
            } else if (strcmp(method, "textDocument/completion") == 0) {
                char *uri = get_uri(json);
                if (uri) {
                    handle_completion(id, json, uri);
                    free(uri);
                }
            } else if (strcmp(method, "textDocument/definition") == 0) {
                char *uri = get_uri(json);
                if (uri) {
                    handle_definition(id, json, uri);
                    free(uri);
                }
            } else if (strcmp(method, "textDocument/documentSymbol") == 0) {
                char *uri = get_uri(json);
                if (uri) {
                    handle_document_symbols(id, json, uri);
                    free(uri);
                }
            } else if (strcmp(method, "textDocument/semanticTokens/full") == 0) {
                char *uri = get_uri(json);
                if (uri) {
                    handle_semantic_tokens(id, uri);
                    free(uri);
                }
            } else if (strcmp(method, "textDocument/references") == 0) {
                char *uri = get_uri(json);
                if (uri) {
                    handle_references(id, json, uri);
                    free(uri);
                }
            } else if (strcmp(method, "textDocument/rename") == 0) {
                char *uri = get_uri(json);
                if (uri) {
                    handle_rename(id, json, uri);
                    free(uri);
                }
            } else if (strcmp(method, "textDocument/signatureHelp") == 0) {
                char *uri = get_uri(json);
                if (uri) {
                    handle_signature_help(id, json, uri);
                    free(uri);
                }
            } else if (strcmp(method, "textDocument/codeAction") == 0) {
                char *uri = get_uri(json);
                if (uri) {
                    handle_code_action(id, json, uri);
                    free(uri);
                }
            } else if (strcmp(method, "textDocument/foldingRange") == 0) {
                char *uri = get_uri(json);
                if (uri) {
                    handle_folding_range(id, json, uri);
                    free(uri);
                }
            }
            free(method);
        }
        free(json);
    }
    return 0;
}
