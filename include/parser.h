#ifndef PARSER_H
#define PARSER_H

#include "varian.h"
#include "lexer.h"
#include "ast.h"

/* Struct definition stored during parsing */
typedef struct {
    char *name;
    char **field_names;
    int field_count;
} StructDef;

/* Actor definition stored during parsing */
typedef struct {
    char *name;
    char **field_names;
    int field_count;
    /* Method names */
    char **method_names;
    int method_count;
} ActorDef;

/* Enum definition stored during parsing */
typedef struct {
    char *name;
    char **variant_names;
    int *variant_counts;  /* number of associated values per variant */
    int variant_count;
} EnumDef;

/* ─── Parser State ─── */
typedef struct {
    Lexer *lexer;
    Token current;
    Token previous;
    Arena *arena;
    bool had_error;
    char error_message[512];
    int loop_depth;  /* for break/continue tracking */
    /* Struct type registry (compile-time only) */
    StructDef structs[128];
    int struct_count;
    /* Actor type registry */
    ActorDef actors[128];
    int actor_count;
    /* Enum type registry */
    EnumDef enums[128];
    int enum_count;
    /* Method name registry */
    char *method_names[256];
    int method_count;
} Parser;

/* Initialize the parser */
void parser_init(Parser *parser, Lexer *lexer, Arena *arena);

/* Parse the entire source into a program AST node */
AstNode *parser_parse(Parser *parser);

/* Parse a single expression */
AstNode *parser_parse_expr(Parser *parser);

/* Get error info */
const char *parser_get_error(Parser *parser);

#endif /* PARSER_H */
