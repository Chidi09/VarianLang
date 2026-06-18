#ifndef LEXER_H
#define LEXER_H

#include "varian.h"
#include <stdint.h>
#include <stdbool.h>

/* ─── Token Types ─── */
typedef enum {
    /* Single-character tokens */
    TOKEN_LPAREN, TOKEN_RPAREN,       /* ( ) */
    TOKEN_LBRACE, TOKEN_RBRACE,       /* { } */
    TOKEN_LBRACKET, TOKEN_RBRACKET,   /* [ ] */
    TOKEN_SEMICOLON,                  /* ; */
    TOKEN_COLON,                      /* : */
    TOKEN_COMMA,                      /* , */
    TOKEN_DOT,                        /* . */
    TOKEN_PLUS, TOKEN_MINUS,          /* + - */
    TOKEN_STAR, TOKEN_SLASH,          /* * / */
    TOKEN_PERCENT,                    /* % */
    TOKEN_AMPERSAND,                  /* & */
    TOKEN_PIPE,                       /* | */
    TOKEN_CARET,                      /* ^ */
    TOKEN_TILDE,                      /* ~ */
    TOKEN_AT,                         /* @ (decorators) */
    TOKEN_HASH,                       /* # (doc comments) */
    TOKEN_QUESTION,                   /* ? (error propagation) */
    TOKEN_BANG,                       /* ! */
    TOKEN_LESS, TOKEN_GREATER,        /* < > */
    TOKEN_EQUAL,                      /* = */
    TOKEN_UNDERSCORE,                 /* _ */

    /* Multi-character tokens */
    TOKEN_EQUAL_EQUAL,                /* == */
    TOKEN_BANG_EQUAL,                 /* != */
    TOKEN_LESS_EQUAL,                 /* <= */
    TOKEN_GREATER_EQUAL,             /* >= */
    TOKEN_PLUS_EQUAL,                 /* += */
    TOKEN_MINUS_EQUAL,               /* -= */
    TOKEN_STAR_EQUAL,                 /* *= */
    TOKEN_SLASH_EQUAL,               /* /= */
    TOKEN_ARROW,                      /* -> */
    TOKEN_FAT_ARROW,                  /* => */
    TOKEN_DOT_DOT,                    /* .. */
    TOKEN_DOT_DOT_DOT,               /* ... */
    TOKEN_PIPE_PIPE,                  /* || */
    TOKEN_AMPERSAND_AMPERSAND,       /* && */
    TOKEN_QUESTION_QUESTION,          /* ?? */
    TOKEN_DOUBLE_COLON,              /* :: */

    /* Literals */
    TOKEN_IDENTIFIER,
    TOKEN_STRING,
    TOKEN_INTERPOLATED_STRING,       /* "hello {name}" */
    TOKEN_INTEGER,
    TOKEN_FLOAT,
    TOKEN_REGEX,                     /* /pattern/flags */
    TOKEN_BYTE_SLICE,                /* b"..." */

    /* Keywords */
    TOKEN_LET, TOKEN_CONST,
    TOKEN_FN, TOKEN_RETURN,
    TOKEN_IF, TOKEN_ELSE,
    TOKEN_WHILE, TOKEN_FOR,
    TOKEN_IN, TOKEN_LOOP,
    TOKEN_MATCH, TOKEN_CASE,
    TOKEN_STRUCT, TOKEN_ENUM,
    TOKEN_IMPL, TOKEN_TRAIT,
    TOKEN_TYPE, TOKEN_USE,
    TOKEN_PUB, TOKEN_MUT,
    TOKEN_ASYNC, TOKEN_AWAIT,
    TOKEN_BREAK, TOKEN_CONTINUE,
    TOKEN_TRY, TOKEN_CATCH,
    TOKEN_TRUE, TOKEN_FALSE,
    TOKEN_NULL,

    /* Type keywords */
    TOKEN_TYPE_BOOL,
    TOKEN_TYPE_INT,
    TOKEN_TYPE_FLOAT,
    TOKEN_TYPE_STRING,
    TOKEN_TYPE_BYTE,
    TOKEN_TYPE_VOID,

    /* Special */
    TOKEN_EOF,
    TOKEN_ERROR
} TokenType;

/* String representation of token types */
const char *token_type_name(TokenType type);

typedef struct {
    TokenType type;
    const char *start;
    int length;
    int line;
    int column;
    /* For string/interpolated string: the raw value */
    char *value;  /* owned string, must be freed */
} Token;

/* The lexer state */
typedef struct {
    const char *source;
    const char *start;   /* start of current lexeme */
    const char *current; /* current position */
    int line;
    int column;
    const char *filename;
    bool had_error;
    char error_message[256];
    bool expr_start;     /* true if next token starts an expression */
} Lexer;

/* Initialize a lexer */
void lexer_init(Lexer *lexer, const char *source, const char *filename);

/* Scan the next token */
Token lexer_next(Lexer *lexer);

/* Peek at the next token without consuming it */
Token lexer_peek(Lexer *lexer);

/* Advance past the next token (consume peeked token) */
void lexer_advance(Lexer *lexer);

/* Free any allocated memory in a token */
void token_free(Token *token);

/* Check if token is a given type */
bool token_is(Token *token, TokenType type);

/* Debug: print a token */
void token_print(Token *token);

#endif /* LEXER_H */
