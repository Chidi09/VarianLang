#include "fmt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

/* ─── Formatter Token Types ─── */
typedef enum {
    FMT_IDENTIFIER,
    FMT_KEYWORD,
    FMT_NUMBER,
    FMT_STRING,
    FMT_OPERATOR,
    FMT_PUNCTUATION,
    FMT_BRACE_OPEN,    /* { */
    FMT_BRACE_CLOSE,   /* } */
    FMT_PAREN_OPEN,    /* ( */
    FMT_PAREN_CLOSE,   /* ) */
    FMT_BRACKET_OPEN,  /* [ */
    FMT_BRACKET_CLOSE, /* ] */
    FMT_SEMICOLON,
    FMT_COMMA,
    FMT_DOT,
    FMT_COLON,
    FMT_COMMENT_LINE,
    FMT_COMMENT_BLOCK,
    FMT_NEWLINE,
    FMT_REGEX,
    FMT_EOF,
} FmtTokenType;

typedef struct {
    FmtTokenType type;
    const char *start;
    int length;
    int line;
} FmtToken;

/* ─── Token Scanner (comment-preserving) ─── */
typedef struct {
    const char *start;
    const char *current;
    const char *source_start;
    int line;
    bool expect_expr;
} FmtScanner;

static bool fmt_token_starts_expr(FmtTokenType type, const char *start, int length) {
    switch (type) {
        case FMT_PAREN_OPEN: case FMT_BRACKET_OPEN: case FMT_BRACE_OPEN:
        case FMT_COMMA: case FMT_COLON: case FMT_SEMICOLON:
        case FMT_OPERATOR: case FMT_NEWLINE: case FMT_EOF:
            return true;
        case FMT_IDENTIFIER: {
            const char *kws[] = {"fn", "let", "const", "return", "if", "else",
                "while", "for", "loop", "match", "case", "try", "catch",
                "assert", "test",
                "struct", "enum", "actor", "type", "throw", "true", "false",
                "null", "pub", "mut", "async", "await", "break", "continue", NULL};
            for (int i = 0; kws[i]; i++) {
                if (length == (int)strlen(kws[i]) && memcmp(start, kws[i], length) == 0)
                    return true;
            }
            return false;
        }
        default:
            return false;
    }
}

static void fmt_scanner_init(FmtScanner *s, const char *source) {
    s->start = source;
    s->current = source;
    s->source_start = source;
    s->line = 1;
    s->expect_expr = true;
}

static bool fmt_is_at_end(FmtScanner *s) {
    return *s->current == '\0';
}

static char fmt_peek(FmtScanner *s) {
    return *s->current;
}

static char fmt_peek_next(FmtScanner *s) {
    if (*(s->current + 1) == '\0') return '\0';
    return *(s->current + 1);
}

static char fmt_advance(FmtScanner *s) {
    char c = *s->current;
    if (c == '\n') s->line++;
    s->current++;
    return c;
}

static void fmt_skip_whitespace(FmtScanner *s) {
    while (!fmt_is_at_end(s)) {
        char c = fmt_peek(s);
        if (c == ' ' || c == '\t' || c == '\r') {
            fmt_advance(s);
        } else if (c == '\n') {
            break;  /* newlines are significant for formatting */
        } else {
            break;
        }
    }
}

static FmtToken fmt_make_token(FmtScanner *s, FmtTokenType type) {
    FmtToken t;
    t.type = type;
    t.start = s->start;
    t.length = (int)(s->current - s->start);
    t.line = s->line;
    s->expect_expr = fmt_token_starts_expr(type, t.start, t.length);
    return t;
}

/* Scan the next token, preserving comments */
static FmtToken fmt_scan_token(FmtScanner *s) {
    fmt_skip_whitespace(s);
    s->start = s->current;

    if (fmt_is_at_end(s))
        return fmt_make_token(s, FMT_EOF);

    char c = fmt_advance(s);

    /* Newline */
    if (c == '\n') {
        return fmt_make_token(s, FMT_NEWLINE);
    }

    /* Comments and regex */
    if (c == '/') {
        if (fmt_peek(s) == '/') {
            while (fmt_peek(s) != '\n' && !fmt_is_at_end(s))
                fmt_advance(s);
            return fmt_make_token(s, FMT_COMMENT_LINE);
        }
        if (fmt_peek(s) == '*') {
            fmt_advance(s); /* skip * */
            while (!(fmt_peek(s) == '*' && fmt_peek_next(s) == '/') && !fmt_is_at_end(s)) {
                if (fmt_peek(s) == '\n') s->line++;
                fmt_advance(s);
            }
            if (!fmt_is_at_end(s)) {
                fmt_advance(s); /* * */
                fmt_advance(s); /* / */
            }
            return fmt_make_token(s, FMT_COMMENT_BLOCK);
        }
        if (s->expect_expr) {
            /* Regex literal: /pattern/flags */
            while (!fmt_is_at_end(s)) {
                if (fmt_peek(s) == '/') { fmt_advance(s); break; }
                if (fmt_peek(s) == '\\') { fmt_advance(s); if (!fmt_is_at_end(s)) fmt_advance(s); }
                else if (fmt_peek(s) == '\n') break;
                else fmt_advance(s);
            }
            /* Flags */
            while (isalpha(fmt_peek(s))) fmt_advance(s);
            return fmt_make_token(s, FMT_REGEX);
        }
        /* Single / — operator */
        return fmt_make_token(s, FMT_OPERATOR);
    }

    /* Strings */
    if (c == '"' || (c == 'b' && fmt_peek(s) == '"')) {
        if (c == 'b') fmt_advance(s); /* skip b */
        char quote = '"';
        while (fmt_peek(s) != quote && !fmt_is_at_end(s)) {
            if (fmt_peek(s) == '\\') {
                fmt_advance(s); /* skip escape */
                if (!fmt_is_at_end(s)) fmt_advance(s);
            } else {
                if (fmt_peek(s) == '\n') s->line++;
                fmt_advance(s);
            }
        }
        if (!fmt_is_at_end(s)) fmt_advance(s); /* closing quote */
        return fmt_make_token(s, FMT_STRING);
    }

    /* Numbers */
    if (isdigit(c)) {
        while (isdigit(fmt_peek(s))) fmt_advance(s);
        if (fmt_peek(s) == '.' && isdigit(fmt_peek_next(s))) {
            fmt_advance(s); /* . */
            while (isdigit(fmt_peek(s))) fmt_advance(s);
        }
        if (fmt_peek(s) == 'e' || fmt_peek(s) == 'E') {
            fmt_advance(s);
            if (fmt_peek(s) == '+' || fmt_peek(s) == '-') fmt_advance(s);
            while (isdigit(fmt_peek(s))) fmt_advance(s);
        }
        return fmt_make_token(s, FMT_NUMBER);
    }

    /* Identifiers and keywords */
    if (isalpha(c) || c == '_') {
        while (isalnum(fmt_peek(s)) || fmt_peek(s) == '_')
            fmt_advance(s);
        return fmt_make_token(s, FMT_IDENTIFIER);
    }

    /* Multi-character operators */
    if (c == ':' && fmt_peek(s) == ':') { fmt_advance(s); return fmt_make_token(s, FMT_OPERATOR); }
    if (c == '-' && fmt_peek(s) == '>') { fmt_advance(s); return fmt_make_token(s, FMT_OPERATOR); }
    if (c == '=' && fmt_peek(s) == '=') { fmt_advance(s); return fmt_make_token(s, FMT_OPERATOR); }
    if (c == '=' && fmt_peek(s) == '>') { fmt_advance(s); return fmt_make_token(s, FMT_OPERATOR); }
    if (c == '!' && fmt_peek(s) == '=') { fmt_advance(s); return fmt_make_token(s, FMT_OPERATOR); }
    if (c == '<' && fmt_peek(s) == '=') { fmt_advance(s); return fmt_make_token(s, FMT_OPERATOR); }
    if (c == '>' && fmt_peek(s) == '=') { fmt_advance(s); return fmt_make_token(s, FMT_OPERATOR); }
    if (c == '<' && fmt_peek(s) == '-') { fmt_advance(s); return fmt_make_token(s, FMT_OPERATOR); }
    if (c == '+' && fmt_peek(s) == '=') { fmt_advance(s); return fmt_make_token(s, FMT_OPERATOR); }
    if (c == '-' && fmt_peek(s) == '=') { fmt_advance(s); return fmt_make_token(s, FMT_OPERATOR); }
    if (c == '*' && fmt_peek(s) == '=') { fmt_advance(s); return fmt_make_token(s, FMT_OPERATOR); }
    if (c == '/' && fmt_peek(s) == '=') { fmt_advance(s); return fmt_make_token(s, FMT_OPERATOR); }
    if (c == '&' && fmt_peek(s) == '&') { fmt_advance(s); return fmt_make_token(s, FMT_OPERATOR); }
    if (c == '|' && fmt_peek(s) == '|') { fmt_advance(s); return fmt_make_token(s, FMT_OPERATOR); }
    /* ?? (nil-coalesce) and ?. (safe navigation) -- without merging these
     * into one token here, the formatter only ever sees a bare '?' and has
     * to guess (via prev-token heuristics) whether it's the propagate
     * operator (x?), the start of ??, or the start of ?. -- merging them
     * the same way ::/->/=> already are below makes each one an explicit,
     * unambiguous token instead. */
    if (c == '?' && fmt_peek(s) == '?') { fmt_advance(s); return fmt_make_token(s, FMT_OPERATOR); }
    if (c == '?' && fmt_peek(s) == '.') { fmt_advance(s); return fmt_make_token(s, FMT_OPERATOR); }
    if (c == '.' && fmt_peek(s) == '.') {
        fmt_advance(s);
        if (fmt_peek(s) == '.') fmt_advance(s);
        return fmt_make_token(s, FMT_OPERATOR);
    }

    /* Braces, parens, brackets */
    if (c == '{') return fmt_make_token(s, FMT_BRACE_OPEN);
    if (c == '}') return fmt_make_token(s, FMT_BRACE_CLOSE);
    if (c == '(') return fmt_make_token(s, FMT_PAREN_OPEN);
    if (c == ')') return fmt_make_token(s, FMT_PAREN_CLOSE);
    if (c == '[') return fmt_make_token(s, FMT_BRACKET_OPEN);
    if (c == ']') return fmt_make_token(s, FMT_BRACKET_CLOSE);

    /* Single-character punctuators */
    if (c == ';') return fmt_make_token(s, FMT_SEMICOLON);
    if (c == ',') return fmt_make_token(s, FMT_COMMA);
    if (c == '.') return fmt_make_token(s, FMT_DOT);
    if (c == ':') return fmt_make_token(s, FMT_COLON);
    if (c == '@') return fmt_make_token(s, FMT_OPERATOR);
    if (c == '#') return fmt_make_token(s, FMT_OPERATOR);

    /* Everything else is an operator */
    if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
        c == '=' || c == '<' || c == '>' || c == '!' ||
        c == '&' || c == '|' || c == '^' || c == '~' ||
        c == '?' || c == '\\') {
        return fmt_make_token(s, FMT_OPERATOR);
    }

    /* Skip unknown characters */
    return fmt_scan_token(s);
}

/* ─── Keyword table ─── */
typedef struct { const char *word; bool is_decl; } FmtKeyword;

static FmtKeyword fmt_keywords[] = {
    {"fn", true}, {"let", true}, {"const", true}, {"return", false},
    {"if", true}, {"else", false}, {"while", true}, {"for", true},
    {"in", false}, {"loop", true}, {"match", true}, {"case", false},
    {"struct", true}, {"enum", true}, {"actor", true},
    {"impl", true}, {"trait", true}, {"type", false}, {"use", false},
    {"pub", false}, {"mut", false}, {"async", false}, {"await", false},
    {"break", false}, {"continue", false}, {"comptime", true},
    {"try", true}, {"catch", false}, {"assert", false}, {"test", true},
    {"true", false}, {"false", false},
    {"null", false}, {"throw", false},
    {"bool", false}, {"int", false}, {"float", false},
    {"string", false}, {"byte", false}, {"void", false},
    {NULL, false}
};

static bool fmt_is_keyword(const char *start, int length, bool *is_decl) {
    for (int i = 0; fmt_keywords[i].word; i++) {
        if (length == (int)strlen(fmt_keywords[i].word) &&
            memcmp(start, fmt_keywords[i].word, length) == 0) {
            if (is_decl) *is_decl = fmt_keywords[i].is_decl;
            return true;
        }
    }
    return false;
}

/* ─── Token classification helpers ─── */
static bool fmt_is_opening_brace_keyword(const char *start, int length) {
    const char *kws[] = {"fn", "if", "else", "while", "for", "loop",
                         "struct", "enum", "actor", "trait", "impl",
                         "match", "try", "catch", "comptime", NULL};
    for (int i = 0; kws[i]; i++) {
        if (length == (int)strlen(kws[i]) &&
            memcmp(start, kws[i], length) == 0)
            return true;
    }
    return false;
}

/* ─── Formatter Engine ─── */
char *fmt_format_source(const char *source, size_t size, int *out_pos_ret) {
    /* Scan all tokens first */
    FmtScanner scanner;
    fmt_scanner_init(&scanner, source);
    #define MAX_TOKENS 65536
    FmtToken tokens[MAX_TOKENS];
    int token_count = 0;

    while (token_count < MAX_TOKENS) {
        FmtToken tok = fmt_scan_token(&scanner);
        tokens[token_count++] = tok;
        if (tok.type == FMT_EOF) break;
    }

    /* Build output */
    size_t out_cap = size * 2 + 4096;
    char *out = (char *)calloc(out_cap, 1);
    if (!out) return NULL;
    int out_pos = 0;
    int indent = 0;
    bool need_indent = true;
    int inline_brace_stack[256];
    int inline_brace_sp = 0;

    /* Track previous significant token for spacing */
    FmtToken prev;

    prev.type = FMT_NEWLINE;
    prev.start = NULL;
    prev.length = 0;
    prev.line = 0;

    /* Whether we just wrote an opening { and should line-break */
    bool after_open_brace = false;
    bool after_comma = false;
    bool prev_is_decl_keyword = false;
    /* Tracks whether we're between the opening and closing | of a closure
     * param list (|x, y| body) -- a single | is ambiguous on its own
     * (bitwise-or vs. closure delimiter), so this is resolved positionally:
     * a | where the previous token couldn't be a valid left operand opens
     * a param list; the next bare | after that closes it. Param lists
     * never contain a nested |, so a simple toggle is sufficient. */
    bool in_closure_params = false;
    /* How many newlines emitted since the last real content, capped at 2
     * (one to end the content's line, one more to preserve a single blank
     * line) -- collapsing 3+ consecutive source newlines down to exactly
     * one blank line, the same convention as gofmt/rustfmt/prettier,
     * instead of the previous all-or-nothing behavior that silently
     * deleted every intentional blank line in the entire file. */
    int consecutive_newlines_emitted = 0;

/* Emit a string */
#define EMIT(s, len) do { \
    size_t _len = (size_t)(len); \
    while ((size_t)out_pos + _len >= out_cap) { \
        out_cap *= 2; \
        out = (char *)realloc(out, out_cap); \
    } \
    memcpy(out + out_pos, (s), _len); \
    out_pos += (int)_len; \
} while(0)

#define EMITS(s) EMIT((s), strlen(s))

#define EMIT_INDENT() do { \
    for (int _i = 0; _i < indent * 4; _i++) EMITS(" "); \
} while(0)

/* Strip trailing whitespace, then push a newline and set indent flag */
#define NEWLINE_AND_INDENT() do { \
    while (out_pos > 0 && (out[out_pos - 1] == ' ' || out[out_pos - 1] == '\t')) out_pos--; \
    EMITS("\n"); \
    need_indent = true; \
} while(0)

#define STRIP_TRAILING() do { \
    while (out_pos > 0 && (out[out_pos - 1] == ' ' || out[out_pos - 1] == '\t')) out_pos--; \
} while(0)

    for (int i = 0; i < token_count; i++) {
        FmtToken *tok = &tokens[i];
        bool prev_was_nl = (prev.type == FMT_NEWLINE || prev.type == FMT_EOF);
        if (tok->type != FMT_NEWLINE) consecutive_newlines_emitted = 0;

        if (tok->type == FMT_EOF) break;

        /* ── NEWLINE ── */
        if (tok->type == FMT_NEWLINE) {
            if (consecutive_newlines_emitted < 2) {
                after_open_brace = false;
                after_comma = false;
                NEWLINE_AND_INDENT();
                consecutive_newlines_emitted++;
            }
            prev = *tok;
            prev_is_decl_keyword = false;
            continue;
        }

        /* ── LINE COMMENT (preserve in place) ── */
        if (tok->type == FMT_COMMENT_LINE) {
            if (!prev_was_nl) NEWLINE_AND_INDENT();
            else if (need_indent) EMIT_INDENT();
            EMIT(tok->start, tok->length);
            NEWLINE_AND_INDENT();
            after_open_brace = false;
            after_comma = false;
            /* The real newline token right after this comment in the
             * source still comes next in the stream -- mark prev as if it
             * were that newline (not this comment) so the NEWLINE case
             * above correctly collapses it instead of emitting a second,
             * blank-line-producing one. */
            /* The real newline token right after this comment in the
             * source is the SAME line-ending this just emitted, not a
             * second, separate one -- consume it outright rather than
             * letting the generic NEWLINE case see it (which can't tell
             * "this is the line the comment already ended" apart from
             * "this starts a genuine blank line" using a counter alone). */
            if (i + 1 < token_count && tokens[i + 1].type == FMT_NEWLINE) i++;
            prev = *tok;
            prev.type = FMT_NEWLINE;
            prev_is_decl_keyword = false;
            continue;
        }

        /* ── BLOCK COMMENT ── */
        if (tok->type == FMT_COMMENT_BLOCK) {
            /* Check for multi-line */
            const char *nl = (const char *)memchr(tok->start, '\n', tok->length);
            if (nl) {
                if (!prev_was_nl) NEWLINE_AND_INDENT();
                if (need_indent) EMIT_INDENT();
                const char *p = tok->start, *end = tok->start + tok->length;
                bool first = true;
                while (p < end) {
                    if (!first) { NEWLINE_AND_INDENT(); EMITS(" "); }
                    first = false;
                    const char *le = (const char *)memchr(p, '\n', end - p);
                    if (!le) le = end;
                    EMIT(p, (int)(le - p));
                    p = le + 1;
                }
                NEWLINE_AND_INDENT();
                /* Same fix as the line-comment case above: the next token
                 * is the real source newline after this comment, which
                 * needs to see prev as already-a-newline to collapse
                 * correctly instead of producing a blank line. */
                /* Same reasoning as the line-comment case above. */
                if (i + 1 < token_count && tokens[i + 1].type == FMT_NEWLINE) i++;
                prev = *tok;
                prev.type = FMT_NEWLINE;
                prev_is_decl_keyword = false;
                continue;
            } else {
                /* Inline block comment */
                if (!prev_was_nl && prev.type != FMT_BRACE_OPEN && prev.type != FMT_PAREN_OPEN && prev.type != FMT_COMMA) EMITS(" ");
                EMIT(tok->start, tok->length);
                EMITS(" ");
            }
            prev = *tok;
            prev_is_decl_keyword = false;
            continue;
        }

        /* ── SEMICOLON ── */
        if (tok->type == FMT_SEMICOLON) {
            EMITS(";");
            after_open_brace = false;
            after_comma = false;
            prev = *tok;
            prev_is_decl_keyword = false;
            continue;
        }

        /* ── COMMA ── */
        if (tok->type == FMT_COMMA) {
            EMITS(", ");
            after_comma = true;
            prev = *tok;
            prev_is_decl_keyword = false;
            continue;
        }

        /* ── CLOSING BRACE/PAREN/BRACKET ── */
        if (tok->type == FMT_BRACE_CLOSE) {
            indent--;
            if (indent < 0) indent = 0;
            bool inline_close = (inline_brace_sp > 0 && inline_brace_stack[inline_brace_sp - 1]);
            if (inline_brace_sp > 0) inline_brace_sp--;
            if (!prev_was_nl && prev.type != FMT_BRACE_OPEN) {
                if (inline_close) {
                    EMITS(" ");
                } else {
                    NEWLINE_AND_INDENT();
                }
            }
            if (need_indent) { EMIT_INDENT(); need_indent = false; }
            EMITS("}");
            after_open_brace = false;
            after_comma = false;
            prev = *tok;
            prev_is_decl_keyword = false;
            continue;
        }
        if (tok->type == FMT_PAREN_CLOSE) {
            EMITS(")");
            prev = *tok;
            prev_is_decl_keyword = false;
            continue;
        }
        if (tok->type == FMT_BRACKET_CLOSE) {
            EMITS("]");
            prev = *tok;
            prev_is_decl_keyword = false;
            continue;
        }

        /* ── Now handle indentation before the token ── */
        if (need_indent) {
            EMIT_INDENT();
            need_indent = false;
        }

        /* ── Spacing decisions ── */
        bool space_before = false;
        bool prev_was_punct = (prev.type == FMT_PAREN_OPEN || prev.type == FMT_BRACKET_OPEN ||
                               prev.type == FMT_COMMA || prev.type == FMT_OPERATOR ||
                               prev.type == FMT_SEMICOLON);

        /* OPENING BRACE */
        if (tok->type == FMT_BRACE_OPEN) {
            /* A closure's closing | (e.g. |req, next| {) already emitted
             * its own trailing space -- same reasoning as already-excluded
             * ( and [. */
            bool prev_is_closure_pipe_close = (prev.type == FMT_OPERATOR && prev.length == 1 && prev.start[0] == '|');
            /* Same-line brace rule: space before { unless after ( or decl keyword */
            if (prev.type != FMT_PAREN_OPEN && prev.type != FMT_BRACKET_OPEN && !prev_is_closure_pipe_close &&
                !prev_was_nl && !prev_is_decl_keyword)
                space_before = true;
            EMITS(space_before ? " {" : "{");
            indent++;
            after_open_brace = true;
            /* Determine if this brace block is inline (same line as content) */
            bool is_inline = false;
            int next_i = i + 1;
            while (next_i < token_count && tokens[next_i].type == FMT_NEWLINE) next_i++;
            if (next_i < token_count && tokens[next_i].type != FMT_EOF &&
                tokens[next_i].type != FMT_BRACE_CLOSE &&
                tokens[next_i].line == tok->line) {
                is_inline = true;
            }
            if (inline_brace_sp < 256) inline_brace_stack[inline_brace_sp++] = is_inline;
            prev = *tok;
            prev_is_decl_keyword = false;
            continue;
        }

        /* OPENING PAREN */
        if (tok->type == FMT_PAREN_OPEN) {
            if (prev.type == FMT_IDENTIFIER && fmt_is_opening_brace_keyword(prev.start, prev.length)) {
                /* if (, while (, fn ( -- but if this keyword is also
                 * flagged is_decl in fmt_keywords (e.g. "while"/"if"),
                 * it already emitted its own trailing space, so adding
                 * " (" here too would double it up. */
                EMITS(prev_is_decl_keyword ? "(" : " (");
            } else if (prev.type == FMT_IDENTIFIER) {
                EMITS("(");   /* function call: foo( */
            } else {
                EMITS("(");
            }
            prev = *tok;
            prev_is_decl_keyword = false;
            continue;
        }

        /* OPENING BRACKET */
        if (tok->type == FMT_BRACKET_OPEN) {
            EMITS("[");
            prev = *tok;
            prev_is_decl_keyword = false;
            continue;
        }

        /* DOT — no spaces */
        if (tok->type == FMT_DOT) {
            EMITS(".");
            prev = *tok;
            prev_is_decl_keyword = false;
            continue;
        }

        /* COLON — single space after, no space before */
        if (tok->type == FMT_COLON) {
            EMITS(": ");
            prev = *tok;
            prev_is_decl_keyword = false;
            continue;
        }

        /* ── OPERATORS & PUNCTUATION ── */
        if (tok->type == FMT_OPERATOR) {
            /* Handle ! — always prefix logical-not (a bare ! is never
             * binary; != is already its own 2-char token, see above), so
             * unlike unary -, there's no ambiguity to resolve: just a
             * normal leading space (if one is needed at all) and no space
             * before the operand it negates. */
            if (tok->length == 1 && tok->start[0] == '!') {
                /* prev_is_decl_keyword covers "while"/"if"/etc., which
                 * already emitted their own trailing space (see is_decl in
                 * fmt_keywords) -- without this check, "while !x" would
                 * get a second, doubled-up space here. */
                if (!prev_is_decl_keyword && !prev_was_nl && !prev_was_punct) EMITS(" ");
                EMITS("!");
                prev = *tok;
                prev_is_decl_keyword = false;
                continue;
            }
            /* Handle .. and ... (range) — no spaces */
            if (tok->length >= 2 && tok->start[0] == '.') {
                EMIT(tok->start, tok->length);
                prev = *tok;
                prev_is_decl_keyword = false;
                continue;
            }
            /* Handle :: — no spaces */
            if (tok->length == 2 && tok->start[0] == ':' && tok->start[1] == ':') {
                EMITS("::");
                prev = *tok;
                prev_is_decl_keyword = false;
                continue;
            }
            /* Arrow and fat arrow — spaces around */
            if ((tok->length == 2 && tok->start[0] == '-' && tok->start[1] == '>') ||
                (tok->length == 2 && tok->start[0] == '=' && tok->start[1] == '>')) {
                EMITS(" ");
                EMIT(tok->start, tok->length);
                EMITS(" ");
                prev = *tok;
                prev_is_decl_keyword = false;
                continue;
            }
            /* Handle unary minus: space before only if preceded by ident/num/parenclose */
            if (tok->length == 1 && tok->start[0] == '-') {
                bool unary = prev_was_nl || prev.type == FMT_PAREN_OPEN || prev.type == FMT_BRACKET_OPEN ||
                             prev.type == FMT_OPERATOR || prev.type == FMT_COMMA;
                if (!unary) EMITS(" ");
                EMITS("-");
                prev = *tok;
                prev_is_decl_keyword = false;
                continue;
            }
            /* Handle ?. — safe navigation: no space before or after, like . */
            if (tok->length == 2 && tok->start[0] == '?' && tok->start[1] == '.') {
                EMITS("?.");
                prev = *tok;
                prev_is_decl_keyword = false;
                continue;
            }
            /* Handle ?? — nil-coalesce: a normal binary operator, space
             * before and after, same as || or &&. */
            if (tok->length == 2 && tok->start[0] == '?' && tok->start[1] == '?') {
                if (!prev_was_nl && !prev_was_punct) EMITS(" ");
                EMITS("??");
                EMITS(" ");
                prev = *tok;
                prev_is_decl_keyword = false;
                continue;
            }
            /* Handle ? — postfix (no space before) */
            if (tok->length == 1 && tok->start[0] == '?') {
                bool postfix = (prev.type == FMT_PAREN_CLOSE || prev.type == FMT_BRACKET_CLOSE ||
                                prev.type == FMT_IDENTIFIER || prev.type == FMT_NUMBER);
                if (postfix) {
                    EMITS("?");
                } else {
                    if (!prev_was_nl && !prev_was_punct) EMITS(" ");
                    EMIT(tok->start, tok->length);
                    EMITS(" ");
                }
                prev = *tok;
                prev_is_decl_keyword = false;
                continue;
            }
            /* Handle generics < and > — no spaces around */
            if (tok->length == 1 && (tok->start[0] == '<' || tok->start[0] == '>')) {
                bool is_generic = false;
                if (tok->start[0] == '<' && prev.type == FMT_IDENTIFIER) {
                    /* A single token of lookahead can't tell Foo<T> apart
                     * from a plain comparison like "i < keys" -- both have
                     * IDENTIFIER < IDENTIFIER. Scan forward instead: real
                     * generic content between < and > is only
                     * identifiers/commas/newlines; a comparison's right
                     * side hits something else first (., (, an operator,
                     * a closing paren/brace, ;) well before any matching >
                     * -- if it ever closes with one at all. */
                    int n = i + 1;
                    bool plausible = true;
                    int scanned = 0;
                    while (n < token_count && scanned < 20) {
                        if (tokens[n].type == FMT_NEWLINE) { n++; continue; }
                        if (tokens[n].type == FMT_OPERATOR && tokens[n].length == 1 && tokens[n].start[0] == '>') {
                            is_generic = plausible;
                            break;
                        }
                        if (tokens[n].type != FMT_IDENTIFIER && tokens[n].type != FMT_COMMA) {
                            plausible = false;
                            break;
                        }
                        n++;
                        scanned++;
                    }
                }
                if (tok->start[0] == '>' && !prev_was_nl && !prev_was_punct &&
                    prev.type == FMT_IDENTIFIER) {
                    int n = i + 1;
                    while (n < token_count && tokens[n].type == FMT_NEWLINE) n++;
                    if (n < token_count && (tokens[n].type == FMT_BRACE_OPEN ||
                        tokens[n].type == FMT_PAREN_OPEN || tokens[n].type == FMT_COMMA ||
                        tokens[n].type == FMT_SEMICOLON || tokens[n].type == FMT_OPERATOR ||
                        tokens[n].type == FMT_BRACE_CLOSE || tokens[n].type == FMT_EOF))
                        is_generic = true;
                }
                if (is_generic) {
                    EMIT(tok->start, tok->length);
                } else {
                    if (!prev_was_nl && !prev_was_punct) EMITS(" ");
                    EMIT(tok->start, tok->length);
                    EMITS(" ");
                }
                prev = *tok;
                prev_is_decl_keyword = false;
                continue;
            }
            /* Handle | — closure param delimiter vs. bitwise-or. A | can't
             * be told apart from the next token alone; what disambiguates
             * it is whether the *previous* token could be a valid left
             * operand. If it can't (e.g. after "return", "(", ",", or a
             * newline), this | can only be opening a closure's param list,
             * never a binary operator. See in_closure_params above. */
            if (tok->length == 1 && tok->start[0] == '|') {
                if (in_closure_params) {
                    /* Always emit the trailing space here (covers a bare-
                     * expression closure body, e.g. |x| x + 1) -- the {
                     * rule below checks for this exact token to avoid
                     * doubling it up for the far more common |x| { ... }. */
                    EMITS("|");
                    EMITS(" ");
                    in_closure_params = false;
                } else {
                    /* A keyword (e.g. "return") tokenizes as plain
                     * FMT_IDENTIFIER here -- fmt has no separate keyword
                     * token type at this stage (see fmt_is_keyword) -- so
                     * checking prev.type alone would wrongly call "return |"
                     * a binary use, since "return" looks like a valid
                     * left-hand identifier by type alone. */
                    bool prev_is_kw = fmt_is_keyword(prev.start, prev.length, NULL);
                    bool could_be_binary = !prev_is_kw &&
                                            (prev.type == FMT_IDENTIFIER || prev.type == FMT_NUMBER ||
                                             prev.type == FMT_PAREN_CLOSE || prev.type == FMT_BRACKET_CLOSE);
                    if (!prev_was_nl && !prev_was_punct) EMITS(" ");
                    EMITS("|");
                    if (could_be_binary) {
                        EMITS(" ");
                    } else {
                        /* Zero-arg closure (| |): the lexer is whitespace-
                         * sensitive here -- || with no space between is a
                         * single TOKEN_PIPE_PIPE (logical-or), while | |
                         * with a space is two separate TOKEN_PIPEs (empty
                         * param list). Collapsing the space would silently
                         * change what this parses as, not just its style,
                         * so when the very next non-newline token is also
                         * a bare |, a space MUST be preserved here. */
                        int n = i + 1;
                        while (n < token_count && tokens[n].type == FMT_NEWLINE) n++;
                        if (n < token_count && tokens[n].type == FMT_OPERATOR &&
                            tokens[n].length == 1 && tokens[n].start[0] == '|') {
                            EMITS(" ");
                        }
                        in_closure_params = true;
                    }
                }
                prev = *tok;
                prev_is_decl_keyword = false;
                continue;
            }
            /* Other operators — space around */
            if (!prev_was_nl && !prev_was_punct) EMITS(" ");
            EMIT(tok->start, tok->length);
            EMITS(" ");
            prev = *tok;
            prev_is_decl_keyword = false;
            continue;
        }

        /* ── KEYWORDS AND IDENTIFIERS ── */
        bool is_decl_kw = false;
        bool is_kw = fmt_is_keyword(tok->start, tok->length, &is_decl_kw);

        if (is_kw) {
            /* Space before keyword if needed */
            if (!prev_was_nl && !prev_was_punct && prev.type != FMT_DOT && prev.type != FMT_COMMA && prev.type != FMT_COLON) EMITS(" ");
            EMIT(tok->start, tok->length);
            /* Space after decl keywords (fn, let, struct, etc.) */
            if (is_decl_kw) EMITS(" ");
            after_open_brace = false;
            prev = *tok;
            prev_is_decl_keyword = is_decl_kw;
            continue;
        }

        /* ── IDENTIFIERS ── */
        if (tok->type == FMT_IDENTIFIER) {
            if (prev_is_decl_keyword) {
                /* Already have space after decl keyword — just emit ident */
            } else if (!prev_was_nl && !prev_was_punct &&
                       prev.type != FMT_DOT && prev.type != FMT_COMMA &&
                       prev.type != FMT_PAREN_OPEN && prev.type != FMT_COLON) {
                EMITS(" ");
            }
            EMIT(tok->start, tok->length);
            after_open_brace = false;
            prev = *tok;
            prev_is_decl_keyword = false;
            continue;
        }

        /* ── NUMBERS ── */
        if (tok->type == FMT_NUMBER) {
            if (!prev_was_nl && !prev_was_punct && prev.type != FMT_COMMA &&
                prev.type != FMT_PAREN_OPEN && prev.type != FMT_BRACKET_OPEN &&
                prev.type != FMT_OPERATOR && !after_comma && !after_open_brace) EMITS(" ");
            EMIT(tok->start, tok->length);
            after_open_brace = false;
            prev = *tok;
            prev_is_decl_keyword = false;
            continue;
        }

        /* ── STRINGS ── */
        if (tok->type == FMT_STRING) {
            if (!prev_was_nl && !prev_was_punct && prev.type != FMT_COMMA &&
                prev.type != FMT_PAREN_OPEN && prev.type != FMT_BRACKET_OPEN &&
                !after_comma && !prev_is_decl_keyword) EMITS(" ");
            EMIT(tok->start, tok->length);
            after_open_brace = false;
            prev = *tok;
            prev_is_decl_keyword = false;
            continue;
        }

        /* ── REGEX ── */
        if (tok->type == FMT_REGEX) {
            if (!prev_was_nl && prev.type != FMT_COMMA && prev.type != FMT_PAREN_OPEN &&
                prev.type != FMT_BRACKET_OPEN && prev.type != FMT_BRACE_OPEN &&
                prev.type != FMT_OPERATOR) EMITS(" ");
            EMIT(tok->start, tok->length);
            after_open_brace = false;
            prev = *tok;
            prev_is_decl_keyword = false;
            continue;
        }

        /* Fallback */
        EMIT(tok->start, tok->length);
        prev = *tok;
        prev_is_decl_keyword = false;
    }

    /* Trim trailing whitespace and ensure final newline */
    STRIP_TRAILING();
    if (out_pos > 0 && out[out_pos - 1] == '\n') out_pos--;
    EMITS("\n");

    *out_pos_ret = out_pos;
    return out;
}

int fmt_format_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "fmt: could not open '%s'\n", path);
        return 1;
    }
    fseek(file, 0L, SEEK_END);
    size_t size = (size_t)ftell(file);
    rewind(file);
    char *source = (char *)malloc(size + 1);
    if (!source) { fclose(file); return 1; }
    size_t nread = fread(source, 1, size, file);
    source[nread] = '\0';
    fclose(file);

    int out_pos = 0;
    char *out = fmt_format_source(source, size, &out_pos);
    free(source);
    if (!out) return 1;

    FILE *outfile = fopen(path, "wb");
    if (!outfile) {
        fprintf(stderr, "fmt: could not write '%s'\n", path);
        free(out);
        return 1;
    }
    fwrite(out, 1, (size_t)out_pos, outfile);
    fclose(outfile);
    free(out);
    printf("formatted %s\n", path);
    return 0;
}
