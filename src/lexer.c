#include "lexer.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

/* ─── Token Type Names ─── */
const char *token_type_name(TokenType type) {
    switch (type) {
        case TOKEN_LPAREN: return "TOKEN_LPAREN";
        case TOKEN_RPAREN: return "TOKEN_RPAREN";
        case TOKEN_LBRACE: return "TOKEN_LBRACE";
        case TOKEN_RBRACE: return "TOKEN_RBRACE";
        case TOKEN_LBRACKET: return "TOKEN_LBRACKET";
        case TOKEN_RBRACKET: return "TOKEN_RBRACKET";
        case TOKEN_SEMICOLON: return "TOKEN_SEMICOLON";
        case TOKEN_COLON: return "TOKEN_COLON";
        case TOKEN_COMMA: return "TOKEN_COMMA";
        case TOKEN_DOT: return "TOKEN_DOT";
        case TOKEN_PLUS: return "TOKEN_PLUS";
        case TOKEN_MINUS: return "TOKEN_MINUS";
        case TOKEN_STAR: return "TOKEN_STAR";
        case TOKEN_SLASH: return "TOKEN_SLASH";
        case TOKEN_PERCENT: return "TOKEN_PERCENT";
        case TOKEN_AMPERSAND: return "TOKEN_AMPERSAND";
        case TOKEN_PIPE: return "TOKEN_PIPE";
        case TOKEN_CARET: return "TOKEN_CARET";
        case TOKEN_TILDE: return "TOKEN_TILDE";
        case TOKEN_AT: return "TOKEN_AT";
        case TOKEN_HASH: return "TOKEN_HASH";
        case TOKEN_QUESTION: return "TOKEN_QUESTION";
        case TOKEN_BANG: return "TOKEN_BANG";
        case TOKEN_LESS: return "TOKEN_LESS";
        case TOKEN_GREATER: return "TOKEN_GREATER";
        case TOKEN_EQUAL: return "TOKEN_EQUAL";
        case TOKEN_UNDERSCORE: return "TOKEN_UNDERSCORE";
        case TOKEN_EQUAL_EQUAL: return "TOKEN_EQUAL_EQUAL";
        case TOKEN_BANG_EQUAL: return "TOKEN_BANG_EQUAL";
        case TOKEN_LESS_EQUAL: return "TOKEN_LESS_EQUAL";
        case TOKEN_GREATER_EQUAL: return "TOKEN_GREATER_EQUAL";
        case TOKEN_PLUS_EQUAL: return "TOKEN_PLUS_EQUAL";
        case TOKEN_MINUS_EQUAL: return "TOKEN_MINUS_EQUAL";
        case TOKEN_STAR_EQUAL: return "TOKEN_STAR_EQUAL";
        case TOKEN_SLASH_EQUAL: return "TOKEN_SLASH_EQUAL";
        case TOKEN_ARROW: return "TOKEN_ARROW";
        case TOKEN_FAT_ARROW: return "TOKEN_FAT_ARROW";
        case TOKEN_DOT_DOT: return "TOKEN_DOT_DOT";
        case TOKEN_DOT_DOT_DOT: return "TOKEN_DOT_DOT_DOT";
        case TOKEN_PIPE_PIPE: return "TOKEN_PIPE_PIPE";
        case TOKEN_AMPERSAND_AMPERSAND: return "TOKEN_AMPERSAND_AMPERSAND";
        case TOKEN_QUESTION_QUESTION: return "TOKEN_QUESTION_QUESTION";
        case TOKEN_DOUBLE_COLON: return "TOKEN_DOUBLE_COLON";
        case TOKEN_IDENTIFIER: return "TOKEN_IDENTIFIER";
        case TOKEN_STRING: return "TOKEN_STRING";
        case TOKEN_INTERPOLATED_STRING: return "TOKEN_INTERPOLATED_STRING";
        case TOKEN_INTEGER: return "TOKEN_INTEGER";
        case TOKEN_FLOAT: return "TOKEN_FLOAT";
        case TOKEN_REGEX: return "TOKEN_REGEX";
        case TOKEN_BYTE_SLICE: return "TOKEN_BYTE_SLICE";
        case TOKEN_LET: return "TOKEN_LET";
        case TOKEN_CONST: return "TOKEN_CONST";
        case TOKEN_FN: return "TOKEN_FN";
        case TOKEN_RETURN: return "TOKEN_RETURN";
        case TOKEN_IF: return "TOKEN_IF";
        case TOKEN_ELSE: return "TOKEN_ELSE";
        case TOKEN_WHILE: return "TOKEN_WHILE";
        case TOKEN_FOR: return "TOKEN_FOR";
        case TOKEN_IN: return "TOKEN_IN";
        case TOKEN_LOOP: return "TOKEN_LOOP";
        case TOKEN_MATCH: return "TOKEN_MATCH";
        case TOKEN_CASE: return "TOKEN_CASE";
        case TOKEN_STRUCT: return "TOKEN_STRUCT";
        case TOKEN_ENUM: return "TOKEN_ENUM";
        case TOKEN_IMPL: return "TOKEN_IMPL";
        case TOKEN_TRAIT: return "TOKEN_TRAIT";
        case TOKEN_TYPE: return "TOKEN_TYPE";
        case TOKEN_USE: return "TOKEN_USE";
        case TOKEN_PUB: return "TOKEN_PUB";
        case TOKEN_MUT: return "TOKEN_MUT";
        case TOKEN_ASYNC: return "TOKEN_ASYNC";
        case TOKEN_AWAIT: return "TOKEN_AWAIT";
        case TOKEN_BREAK: return "TOKEN_BREAK";
        case TOKEN_CONTINUE: return "TOKEN_CONTINUE";
        case TOKEN_TRY: return "TOKEN_TRY";
        case TOKEN_CATCH: return "TOKEN_CATCH";
        case TOKEN_TRUE: return "TOKEN_TRUE";
        case TOKEN_FALSE: return "TOKEN_FALSE";
        case TOKEN_NULL: return "TOKEN_NULL";
        case TOKEN_TYPE_BOOL: return "TOKEN_TYPE_BOOL";
        case TOKEN_TYPE_INT: return "TOKEN_TYPE_INT";
        case TOKEN_TYPE_FLOAT: return "TOKEN_TYPE_FLOAT";
        case TOKEN_TYPE_STRING: return "TOKEN_TYPE_STRING";
        case TOKEN_TYPE_BYTE: return "TOKEN_TYPE_BYTE";
        case TOKEN_TYPE_VOID: return "TOKEN_TYPE_VOID";
        case TOKEN_EOF: return "TOKEN_EOF";
        case TOKEN_ERROR: return "TOKEN_ERROR";
    }
    return "UNKNOWN";
}

/* ─── Keyword Lookup ─── */
typedef struct {
    const char *word;
    TokenType type;
} KeywordEntry;

static KeywordEntry keywords[] = {
    {"let",    TOKEN_LET},
    {"const",  TOKEN_CONST},
    {"fn",     TOKEN_FN},
    {"return", TOKEN_RETURN},
    {"if",     TOKEN_IF},
    {"else",   TOKEN_ELSE},
    {"while",  TOKEN_WHILE},
    {"for",    TOKEN_FOR},
    {"in",     TOKEN_IN},
    {"loop",   TOKEN_LOOP},
    {"match",  TOKEN_MATCH},
    {"case",   TOKEN_CASE},
    {"struct", TOKEN_STRUCT},
    {"enum",   TOKEN_ENUM},
    {"impl",   TOKEN_IMPL},
    {"trait",  TOKEN_TRAIT},
    {"type",   TOKEN_TYPE},
    {"use",    TOKEN_USE},
    {"pub",    TOKEN_PUB},
    {"mut",    TOKEN_MUT},
    {"async",  TOKEN_ASYNC},
    {"await",  TOKEN_AWAIT},
    {"break",    TOKEN_BREAK},
    {"continue", TOKEN_CONTINUE},
    {"try",    TOKEN_TRY},
    {"catch",  TOKEN_CATCH},
    {"true",   TOKEN_TRUE},
    {"false",  TOKEN_FALSE},
    {"null",   TOKEN_NULL},
    {"bool",   TOKEN_TYPE_BOOL},
    {"int",    TOKEN_TYPE_INT},
    {"float",  TOKEN_TYPE_FLOAT},
    {"string", TOKEN_TYPE_STRING},
    {"byte",   TOKEN_TYPE_BYTE},
    {"void",   TOKEN_TYPE_VOID},
    {NULL,     TOKEN_EOF}
};

static TokenType check_keyword(const char *start, int length) {
    for (int i = 0; keywords[i].word != NULL; i++) {
        if ((int)strlen(keywords[i].word) == length &&
            strncmp(start, keywords[i].word, length) == 0) {
            return keywords[i].type;
        }
    }
    return TOKEN_IDENTIFIER;
}

/* ─── Lexer Helpers ─── */
static bool is_at_end(Lexer *lexer) {
    return *lexer->current == '\0';
}

static char advance(Lexer *lexer) {
    char c = *lexer->current;
    lexer->current++;
    lexer->column++;
    return c;
}

static char peek(Lexer *lexer) {
    return *lexer->current;
}

static char peek_next(Lexer *lexer) {
    if (is_at_end(lexer)) return '\0';
    return *(lexer->current + 1);
}

static bool match(Lexer *lexer, char expected) {
    if (is_at_end(lexer)) return false;
    if (*lexer->current != expected) return false;
    lexer->current++;
    lexer->column++;
    return true;
}

static void skip_whitespace(Lexer *lexer) {
    for (;;) {
        char c = peek(lexer);
        switch (c) {
            case ' ':
            case '\t':
            case '\r':
                advance(lexer);
                break;
            case '\n':
                advance(lexer);
                lexer->line++;
                lexer->column = 1;
                break;
            case '/':
                if (peek_next(lexer) == '/') {
                    /* Line comment */
                    while (peek(lexer) != '\n' && !is_at_end(lexer))
                        advance(lexer);
                    /* Don't consume the newline here */
                } else if (peek_next(lexer) == '*') {
                    /* Block comment */
                    advance(lexer); advance(lexer); /* skip slash-star */
                    while (!(peek(lexer) == '*' && peek_next(lexer) == '/') && !is_at_end(lexer)) {
                        if (peek(lexer) == '\n') {
                            lexer->line++;
                            lexer->column = 1;
                        }
                        advance(lexer);
                    }
                    if (!is_at_end(lexer)) {
                        advance(lexer); /* * */
                        advance(lexer); /* / */
                    }
                } else {
                    return;
                }
                break;
            default:
                return;
        }
    }
}

static bool token_starts_expr(TokenType type) {
    switch (type) {
        case TOKEN_LPAREN: case TOKEN_LBRACKET: case TOKEN_LBRACE:
        case TOKEN_PLUS: case TOKEN_MINUS: case TOKEN_STAR:
        case TOKEN_BANG: case TOKEN_TILDE:
        case TOKEN_PLUS_EQUAL: case TOKEN_MINUS_EQUAL:
        case TOKEN_STAR_EQUAL: case TOKEN_SLASH_EQUAL:
        case TOKEN_EQUAL: case TOKEN_FAT_ARROW: case TOKEN_ARROW:
        case TOKEN_LESS: case TOKEN_GREATER:
        case TOKEN_LESS_EQUAL: case TOKEN_GREATER_EQUAL:
        case TOKEN_EQUAL_EQUAL: case TOKEN_BANG_EQUAL:
        case TOKEN_AMPERSAND: case TOKEN_PIPE: case TOKEN_CARET:
        case TOKEN_AMPERSAND_AMPERSAND: case TOKEN_PIPE_PIPE:
        case TOKEN_QUESTION: case TOKEN_QUESTION_QUESTION:
        case TOKEN_COMMA: case TOKEN_SEMICOLON: case TOKEN_COLON:
        case TOKEN_RETURN: case TOKEN_IF: case TOKEN_ELSE:
        case TOKEN_WHILE: case TOKEN_FOR: case TOKEN_LOOP:
        case TOKEN_MATCH: case TOKEN_CASE:
        case TOKEN_LET: case TOKEN_CONST: case TOKEN_FN:
        case TOKEN_TRY: case TOKEN_CATCH:
        case TOKEN_TRUE: case TOKEN_FALSE: case TOKEN_NULL:
        case TOKEN_AT: case TOKEN_HASH:
        case TOKEN_EOF:
            return true;
        default:
            return false;
    }
}

static Token make_token(Lexer *lexer, TokenType type) {
    Token token;
    token.type = type;
    token.start = lexer->start;
    token.length = (int)(lexer->current - lexer->start);
    token.line = lexer->line;
    token.column = lexer->column - token.length;
    token.value = NULL;
    lexer->expr_start = token_starts_expr(type);
    return token;
}

static Token make_error_token(Lexer *lexer, const char *message) {
    Token token;
    token.type = TOKEN_ERROR;
    token.start = lexer->start;
    token.length = (int)strlen(message);
    token.line = lexer->line;
    token.column = lexer->column;
    token.value = (char *)malloc(strlen(message) + 1);
    if (token.value) strcpy(token.value, message);
    lexer->had_error = true;
    snprintf(lexer->error_message, sizeof(lexer->error_message), "%s", message);
    return token;
}



/* ─── Number Literal ─── */
static Token number_literal(Lexer *lexer) {
    while (isdigit(peek(lexer))) advance(lexer);

    /* Check for float */
    bool is_float = false;
    if (peek(lexer) == '.' && isdigit(peek_next(lexer))) {
        is_float = true;
        advance(lexer); /* consume . */
        while (isdigit(peek(lexer))) advance(lexer);

        /* Optional exponent */
        if (peek(lexer) == 'e' || peek(lexer) == 'E') {
            advance(lexer);
            if (peek(lexer) == '+' || peek(lexer) == '-') advance(lexer);
            while (isdigit(peek(lexer))) advance(lexer);
        }
    }

    return make_token(lexer, is_float ? TOKEN_FLOAT : TOKEN_INTEGER);
}

/* ─── String Literal ─── */
static Token string_literal(Lexer *lexer, char quote, bool is_byte) {
    /* Build the string content, handling escapes */
    /* First pass: count to determine if interpolation exists */
    const char *scan = lexer->current;
    bool has_interpolation = false;
    int content_len = 0;

    while (*scan != quote && *scan != '\n' && *scan != '\0') {
        if (*scan == '\\') {
            scan++;
            if (*scan == '\0') break;
            scan++;
            content_len++;
        } else if (*scan == '{' && *(scan + 1) != quote && *(scan + 1) != '\0') {
            has_interpolation = true;
            break;
        } else {
            scan++;
            content_len++;
        }
    }

    if (has_interpolation) {
        /* Build interpolated string: parts separated by \1 marker */
        /* For now, just treat as interpolated string */
        size_t buf_size = 4096;
        char *buf = (char *)malloc(buf_size);
        int buf_pos = 0;
        bool in_expr = false;
        int brace_depth = 0;

        while (!is_at_end(lexer)) {
            char c = advance(lexer);
            if (c == quote && !in_expr) {
                break;
            }
            if (c == '\n' && !in_expr) {
                lexer->line++;
                lexer->column = 1;
                break;
            }
            if (c == '\\') {
                char next = advance(lexer);
                switch (next) {
                    case 'n': buf[buf_pos++] = '\n'; break;
                    case 't': buf[buf_pos++] = '\t'; break;
                    case 'r': buf[buf_pos++] = '\r'; break;
                    case '\\': buf[buf_pos++] = '\\'; break;
                    case '"': buf[buf_pos++] = '"'; break;
                    case '{': buf[buf_pos++] = '{'; break;
                    case '0': buf[buf_pos++] = '\0'; break;
                    default: buf[buf_pos++] = '\\'; buf[buf_pos++] = next; break;
                }
            } else if (c == '{' && !in_expr) {
                in_expr = true;
                brace_depth = 1;
                buf[buf_pos++] = '\1'; /* MARKER_START_EXPR */
            } else if (in_expr) {
                if (c == '{') brace_depth++;
                if (c == '}') {
                    brace_depth--;
                    if (brace_depth == 0) {
                        in_expr = false;
                        buf[buf_pos++] = '\2'; /* MARKER_END_EXPR */
                        continue;
                    }
                }
                buf[buf_pos++] = c;
            } else {
                buf[buf_pos++] = c;
            }

            if (buf_pos >= (int)buf_size - 4) {
                buf_size *= 2;
                buf = (char *)realloc(buf, buf_size);
            }
        }
        buf[buf_pos] = '\0';

        Token token = make_token(lexer, TOKEN_INTERPOLATED_STRING);
        token.value = buf;
        return token;
    }

    /* Plain string */
    size_t buf_size = 1024;
    char *buf = (char *)malloc(buf_size);
    int buf_pos = 0;

    while (!is_at_end(lexer)) {
        char c = advance(lexer);
        if (c == quote) break;
        if (c == '\n') {
            lexer->line++;
            lexer->column = 1;
            break;
        }
        if (c == '\\') {
            char next = advance(lexer);
            switch (next) {
                case 'n': buf[buf_pos++] = '\n'; break;
                case 't': buf[buf_pos++] = '\t'; break;
                case 'r': buf[buf_pos++] = '\r'; break;
                case '\\': buf[buf_pos++] = '\\'; break;
                case '"': buf[buf_pos++] = '"'; break;
                case '\'': buf[buf_pos++] = '\''; break;
                case '0': buf[buf_pos++] = '\0'; break;
                case 'x': {
                    char hex[3] = {peek(lexer), peek_next(lexer), '\0'};
                    if (isxdigit(hex[0]) && isxdigit(hex[1])) {
                        buf[buf_pos++] = (char)strtol(hex, NULL, 16);
                        advance(lexer); advance(lexer);
                    }
                    break;
                }
                default: buf[buf_pos++] = next; break;
            }
        } else {
            buf[buf_pos++] = c;
        }

        if (buf_pos >= (int)buf_size - 4) {
            buf_size *= 2;
            buf = (char *)realloc(buf, buf_size);
        }
    }
    /* For byte slices, store (length << 1) | 1 prefix so parser knows the exact length */
    if (is_byte) {
        /* Reallocate buf with room for length prefix */
        char *packed = (char *)malloc(buf_pos + 5);
        packed[0] = (char)(buf_pos & 0xFF);
        packed[1] = (char)((buf_pos >> 8) & 0xFF);
        packed[2] = (char)((buf_pos >> 16) & 0xFF);
        packed[3] = (char)((buf_pos >> 24) & 0xFF);
        memcpy(packed + 4, buf, buf_pos);
        free(buf);
        buf = packed;
    } else {
        buf[buf_pos] = '\0';
    }

    Token token = make_token(lexer, is_byte ? TOKEN_BYTE_SLICE : TOKEN_STRING);
    token.value = buf;
    return token;
}

/* ─── Regex Literal ─── */
static Token regex_literal(Lexer *lexer) {
    /* Capture /pattern/flags */
    size_t buf_size = 1024;
    char *buf = (char *)malloc(buf_size);
    int buf_pos = 0;

    while (!is_at_end(lexer)) {
        char c = advance(lexer);
        if (c == '/') break;
        if (c == '\\') {
            buf[buf_pos++] = c;
            if (!is_at_end(lexer)) {
                buf[buf_pos++] = advance(lexer);
            }
        } else if (c == '\n') {
            free(buf);
            return make_error_token(lexer, "Unterminated regex literal");
        } else {
            buf[buf_pos++] = c;
        }
        if (buf_pos >= (int)buf_size - 4) {
            buf_size *= 2;
            buf = (char *)realloc(buf, buf_size);
        }
    }

    /* Capture flags */
    while (isalpha(peek(lexer))) {
        buf[buf_pos++] = advance(lexer);
    }
    buf[buf_pos] = '\0';

    Token token = make_token(lexer, TOKEN_REGEX);
    token.value = buf;
    return token;
}

/* ─── Identifier ─── */
static Token identifier(Lexer *lexer) {
    while (isalnum(peek(lexer)) || peek(lexer) == '_')
        advance(lexer);

    int length = (int)(lexer->current - lexer->start);
    TokenType type = check_keyword(lexer->start, length);
    return make_token(lexer, type);
}
/* ─── Main Lex Function ─── */
void lexer_init(Lexer *lexer, const char *source, const char *filename) {
    lexer->source = source;
    lexer->start = source;
    lexer->current = source;
    lexer->line = 1;
    lexer->column = 1;
    lexer->filename = filename;
    lexer->had_error = false;
    lexer->error_message[0] = '\0';
    lexer->expr_start = true;
}

Token lexer_next(Lexer *lexer) {
    skip_whitespace(lexer);
    lexer->start = lexer->current;

    if (is_at_end(lexer))
        return make_token(lexer, TOKEN_EOF);

    char c = advance(lexer);

    /* Identifiers and keywords */
    if (isalpha(c) || c == '_')
        return identifier(lexer);

    /* Numbers */
    if (isdigit(c))
        return number_literal(lexer);

    /* Strings */
    if (c == '"')
        return string_literal(lexer, '"', false);

    if (c == '\'')
        return string_literal(lexer, '\'', false);

    /* Byte slice */
    if (c == 'b' && peek(lexer) == '"') {
        advance(lexer); /* consume " */
        return string_literal(lexer, '"', true);
    }

    /* Regex literal - needs context, handled in parser.
     * For tokenizer, '/' is division or regex start. */

    /* Single-character tokens */
    switch (c) {
        case '(': return make_token(lexer, TOKEN_LPAREN);
        case ')': return make_token(lexer, TOKEN_RPAREN);
        case '{': return make_token(lexer, TOKEN_LBRACE);
        case '}': return make_token(lexer, TOKEN_RBRACE);
        case '[': return make_token(lexer, TOKEN_LBRACKET);
        case ']': return make_token(lexer, TOKEN_RBRACKET);
        case ';': return make_token(lexer, TOKEN_SEMICOLON);
        case ',': return make_token(lexer, TOKEN_COMMA);
        case '~': return make_token(lexer, TOKEN_TILDE);
        case '^': return make_token(lexer, TOKEN_CARET);
        case '%': return make_token(lexer, TOKEN_PERCENT);
        case '@': return make_token(lexer, TOKEN_AT);
        case '#': return make_token(lexer, TOKEN_HASH);
        case '?': return make_token(lexer, TOKEN_QUESTION);
    }

    /* Multi-character tokens */
    switch (c) {
        case ':':
            if (match(lexer, ':')) return make_token(lexer, TOKEN_DOUBLE_COLON);
            if (match(lexer, '=')) { /* := not in spec yet, treat as colon */
                lexer->current--;
                lexer->column--;
            }
            return make_token(lexer, TOKEN_COLON);

        case '+':
            if (match(lexer, '=')) return make_token(lexer, TOKEN_PLUS_EQUAL);
            return make_token(lexer, TOKEN_PLUS);

        case '-':
            if (match(lexer, '>')) return make_token(lexer, TOKEN_ARROW);
            if (match(lexer, '=')) return make_token(lexer, TOKEN_MINUS_EQUAL);
            return make_token(lexer, TOKEN_MINUS);

        case '*':
            if (match(lexer, '=')) return make_token(lexer, TOKEN_STAR_EQUAL);
            return make_token(lexer, TOKEN_STAR);

        case '/':
            if (match(lexer, '=')) return make_token(lexer, TOKEN_SLASH_EQUAL);
            if (lexer->expr_start) {
                /* Regex literal: /pattern/flags */
                lexer->start = lexer->current - 1; /* include leading / */
                Token t = regex_literal(lexer);
                lexer->expr_start = false;
                return t;
            }
            return make_token(lexer, TOKEN_SLASH);

        case '!':
            if (match(lexer, '=')) return make_token(lexer, TOKEN_BANG_EQUAL);
            return make_token(lexer, TOKEN_BANG);

        case '=':
            if (match(lexer, '=')) return make_token(lexer, TOKEN_EQUAL_EQUAL);
            if (match(lexer, '>')) return make_token(lexer, TOKEN_FAT_ARROW);
            return make_token(lexer, TOKEN_EQUAL);

        case '<':
            if (match(lexer, '=')) return make_token(lexer, TOKEN_LESS_EQUAL);
            return make_token(lexer, TOKEN_LESS);

        case '>':
            if (match(lexer, '=')) return make_token(lexer, TOKEN_GREATER_EQUAL);
            return make_token(lexer, TOKEN_GREATER);

        case '.':
            if (match(lexer, '.')) {
                if (match(lexer, '.')) return make_token(lexer, TOKEN_DOT_DOT_DOT);
                return make_token(lexer, TOKEN_DOT_DOT);
            }
            return make_token(lexer, TOKEN_DOT);

        case '&':
            if (match(lexer, '&')) return make_token(lexer, TOKEN_AMPERSAND_AMPERSAND);
            return make_token(lexer, TOKEN_AMPERSAND);

        case '|':
            if (match(lexer, '|')) return make_token(lexer, TOKEN_PIPE_PIPE);
            return make_token(lexer, TOKEN_PIPE);

        case '_':
            return make_token(lexer, TOKEN_UNDERSCORE);
    }

    return make_error_token(lexer, "Unexpected character");
}

Token lexer_peek(Lexer *lexer) {
    /* Save state */
    const char *saved_start = lexer->start;
    const char *saved_current = lexer->current;
    int saved_line = lexer->line;
    int saved_column = lexer->column;
    bool saved_error = lexer->had_error;
    char saved_error_msg[256];
    memcpy(saved_error_msg, lexer->error_message, 256);

    Token token = lexer_next(lexer);

    /* Restore state */
    lexer->start = saved_start;
    lexer->current = saved_current;
    lexer->line = saved_line;
    lexer->column = saved_column;
    lexer->had_error = saved_error;
    memcpy(lexer->error_message, saved_error_msg, 256);

    return token;
}

void lexer_advance(Lexer *lexer) {
    /* Just skip current token by calling next and freeing */
    Token token = lexer_next(lexer);
    token_free(&token);
}

void token_free(Token *token) {
    if (token->value) {
        free(token->value);
        token->value = NULL;
    }
}

bool token_is(Token *token, TokenType type) {
    return token->type == type;
}

void token_print(Token *token) {
    printf("[%s] '", token_type_name(token->type));
    if (token->value) {
        printf("%s", token->value);
    } else {
        fwrite(token->start, 1, token->length, stdout);
    }
    printf("' at %d:%d\n", token->line, token->column);
}
