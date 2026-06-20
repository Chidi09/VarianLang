#include "parser.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ─── Forward declarations ─── */
static AstNode *parse_stmt(Parser *parser);
static AstNode *parse_expr(Parser *parser);
static AstNode *parse_range(Parser *parser);
static AstNode *parse_assignment(Parser *parser);
static AstNode *parse_or(Parser *parser);
static AstNode *parse_and(Parser *parser);
static AstNode *parse_equality(Parser *parser);
static AstNode *parse_comparison(Parser *parser);
static AstNode *parse_bit_or(Parser *parser);
static AstNode *parse_bit_xor(Parser *parser);
static AstNode *parse_bit_and(Parser *parser);
static AstNode *parse_shift(Parser *parser);
static AstNode *parse_term(Parser *parser);
static AstNode *parse_factor(Parser *parser);
static AstNode *parse_unary(Parser *parser);
static AstNode *parse_call(Parser *parser);
static AstNode *parse_primary(Parser *parser);
static Type *parse_type(Parser *parser);
static Type *parse_type_inner(Parser *parser);
static AstNode *parse_struct_literal(Parser *parser, const char *type_name, SourceLoc loc);
static AstNode *parse_enum_literal(Parser *parser, const char *enum_name, SourceLoc loc);

static AstNode *parse_trait_decl(Parser *parser);
static AstNode *parse_actor_decl(Parser *parser);

/* ─── Token string extraction (not null-terminated, use length) ─── */
static char *token_str(Parser *parser, Token *token) {
    char *s = (char *)arena_alloc(parser->arena, token->length + 1);
    if (s) {
        memcpy(s, token->start, token->length);
        s[token->length] = '\0';
    }
    return s;
}

static char *token_strdup(Token *token) {
    char *s = (char *)malloc(token->length + 1);
    if (s) {
        memcpy(s, token->start, token->length);
        s[token->length] = '\0';
    }
    return s;
}

/* ─── Error Handling ─── */
static void parser_error(Parser *parser, const char *message) {
    if (!parser->had_error) {
        parser->had_error = true;
        snprintf(parser->error_message, sizeof(parser->error_message),
                 "[%d:%d] Error: %s",
                 parser->current.line, parser->current.column, message);
    }
}

static char *parser_read_file(Parser *parser, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)arena_alloc(parser->arena, size + 1);
    if (buf) {
        size_t read_bytes = fread(buf, 1, size, f);
        buf[read_bytes] = '\0';
    }
    fclose(f);
    return buf;
}

static void parser_error_at(Parser *parser, Token *token, const char *message) {
    if (!parser->had_error) {
        parser->had_error = true;
        snprintf(parser->error_message, sizeof(parser->error_message),
                 "[%d:%d] Error at '%s': %s",
                 token->line, token->column,
                 token->value ? token->value : "?",
                 message);
    }
}

/* ─── Struct registry ─── */
static StructDef *parser_find_struct(Parser *parser, const char *name) {
    for (int i = 0; i < parser->struct_count; i++) {
        if (strcmp(parser->structs[i].name, name) == 0)
            return &parser->structs[i];
    }
    return NULL;
}

static void parser_register_struct(Parser *parser, const char *name,
                                    char **field_names, int field_count) {
    if (parser->struct_count >= 128) {
        if (!parser->had_error) {
            parser->had_error = true;
            snprintf(parser->error_message, sizeof(parser->error_message),
                     "Too many struct types");
        }
        return;
    }
    StructDef *sd = &parser->structs[parser->struct_count++];
    sd->name = (char *)malloc(strlen(name) + 1);
    strcpy(sd->name, name);
    sd->field_names = (char **)malloc(sizeof(char *) * field_count);
    for (int i = 0; i < field_count; i++) {
        sd->field_names[i] = (char *)malloc(strlen(field_names[i]) + 1);
        strcpy(sd->field_names[i], field_names[i]);
    }
    sd->field_count = field_count;
}

/* ─── Function signature registry (named arguments) ─── */
static FunctionSig *parser_find_function(Parser *parser, const char *name) {
    for (int i = 0; i < parser->function_count; i++) {
        if (strcmp(parser->functions[i].name, name) == 0)
            return &parser->functions[i];
    }
    return NULL;
}

static void parser_register_function(Parser *parser, const char *name,
                                      char **param_names, int param_count) {
    if (parser->function_count >= 256) return;
    FunctionSig *fs = &parser->functions[parser->function_count++];
    fs->name = (char *)malloc(strlen(name) + 1);
    strcpy(fs->name, name);
    fs->param_names = (char **)malloc(sizeof(char *) * (size_t)(param_count > 0 ? param_count : 1));
    for (int i = 0; i < param_count; i++) {
        fs->param_names[i] = (char *)malloc(strlen(param_names[i]) + 1);
        strcpy(fs->param_names[i], param_names[i]);
    }
    fs->param_count = param_count;
}

/* ─── Enum registry ─── */
static EnumDef *parser_find_enum(Parser *parser, const char *name) {
    for (int i = 0; i < parser->enum_count; i++) {
        if (strcmp(parser->enums[i].name, name) == 0)
            return &parser->enums[i];
    }
    return NULL;
}

static void parser_register_enum(Parser *parser, const char *name,
                                  char **variant_names, int *variant_counts,
                                  int variant_count) {
    if (parser->enum_count >= 128) {
        if (!parser->had_error) {
            parser->had_error = true;
            snprintf(parser->error_message, sizeof(parser->error_message),
                     "Too many enum types");
        }
        return;
    }
    EnumDef *ed = &parser->enums[parser->enum_count++];
    ed->name = (char *)malloc(strlen(name) + 1);
    strcpy(ed->name, name);
    ed->variant_names = (char **)malloc(sizeof(char *) * variant_count);
    ed->variant_counts = (int *)malloc(sizeof(int) * variant_count);
    for (int i = 0; i < variant_count; i++) {
        ed->variant_names[i] = (char *)malloc(strlen(variant_names[i]) + 1);
        strcpy(ed->variant_names[i], variant_names[i]);
        ed->variant_counts[i] = variant_counts[i];
    }
    ed->variant_count = variant_count;
}

/* ─── Method registry ─── */
static bool parser_is_method(Parser *parser, const char *name) {
    for (int i = 0; i < parser->method_count; i++) {
        if (strcmp(parser->method_names[i], name) == 0)
            return true;
    }
    return false;
}

static void parser_register_method(Parser *parser, const char *name) {
    if (parser->method_count >= 256) return;
    parser->method_names[parser->method_count] = (char *)malloc(strlen(name) + 1);
    strcpy(parser->method_names[parser->method_count], name);
    parser->method_count++;
}

/* ─── Token Helpers ─── */
static void advance(Parser *parser) {
    parser->previous = parser->current;
    parser->current = lexer_next(parser->lexer);
}

static void consume(Parser *parser, TokenType type, const char *message) {
    if (parser->current.type == type) {
        advance(parser);
    } else {
        parser_error_at(parser, &parser->current, message);
    }
}

static bool check(Parser *parser, TokenType type) {
    return parser->current.type == type;
}

static bool match(Parser *parser, TokenType type) {
    if (check(parser, type)) {
        advance(parser);
        return true;
    }
    return false;
}

static SourceLoc current_loc(Parser *parser) {
    SourceLoc loc;
    loc.filename = parser->lexer->filename;
    loc.line = parser->previous.line;
    loc.column = parser->previous.column;
    loc.offset = 0;
    return loc;
}

/* ─── Generic type parameter parsing ─── */
static int parse_type_params(Parser *parser, char **params, int max_params) {
    if (!check(parser, TOKEN_LESS))
        return 0;
    advance(parser);
    int count = 0;
    while (!check(parser, TOKEN_GREATER) && !check(parser, TOKEN_EOF)) {
        if (count >= max_params) break;
        if (parser->current.type != TOKEN_IDENTIFIER) break;
        advance(parser);
        params[count] = token_strdup(&parser->previous);
        count++;
        if (!match(parser, TOKEN_COMMA))
            break;
    }
    consume(parser, TOKEN_GREATER, "Expected '>' after type parameters");
    return count;
}

/* ─── Type Parsing ─── */
static Type *parse_primitive_type(Parser *parser) {
    switch (parser->previous.type) {
        case TOKEN_TYPE_BOOL:   return type_primitive(parser->arena, PRIMITIVE_BOOL);
        case TOKEN_TYPE_INT:    return type_primitive(parser->arena, PRIMITIVE_INT);
        case TOKEN_TYPE_FLOAT:  return type_primitive(parser->arena, PRIMITIVE_FLOAT);
        case TOKEN_TYPE_STRING: return type_primitive(parser->arena, PRIMITIVE_STRING);
        case TOKEN_TYPE_BYTE:   return type_primitive(parser->arena, PRIMITIVE_BYTE);
        case TOKEN_TYPE_VOID:   return type_primitive(parser->arena, PRIMITIVE_VOID);
        default: return NULL;
    }
}

static Type *parse_type(Parser *parser) {
    Type *t = parse_type_inner(parser);
    /* Union type: string | int | null. Like every other type annotation in
     * this language, it's parsed for documentation/tooling purposes only --
     * no runtime member, no behavior change. Only the first member is kept
     * as the AST's Type; the rest are consumed and discarded. */
    while (match(parser, TOKEN_PIPE)) {
        parse_type_inner(parser);
    }
    match(parser, TOKEN_QUESTION);
    return t;
}

static Type *parse_type_inner(Parser *parser) {
    /* Types:
     *   primitive: bool, int, float, string, byte, void
     *   named: identifier
     *   array: [type]
     *   function: fn(type...) -> type
     *   tuple: (type, type...)
     */

    if (match(parser, TOKEN_LBRACKET)) {
        /* Array type: [type] or [type; N] */
        Type *element = parse_type(parser);
        consume(parser, TOKEN_RBRACKET, "Expected ']' after array element type");
        return type_array(parser->arena, element);
    }

    if (match(parser, TOKEN_FN)) {
        /* Function type: fn(type, type...) -> return_type */
        consume(parser, TOKEN_LPAREN, "Expected '(' after 'fn' in type");
        Type *param_types[64];
        int param_count = 0;
        if (!check(parser, TOKEN_RPAREN)) {
            do {
                if (param_count >= 64) {
                    parser_error(parser, "Too many function parameters");
                    break;
                }
                param_types[param_count++] = parse_type(parser);
            } while (match(parser, TOKEN_COMMA));
        }
        consume(parser, TOKEN_RPAREN, "Expected ')' after function parameters");
        Type *return_type = NULL;
        if (match(parser, TOKEN_ARROW)) {
            return_type = parse_type(parser);
        } else {
            return_type = type_primitive(parser->arena, PRIMITIVE_VOID);
        }
        return type_function(parser->arena, param_types, param_count, return_type);
    }

    if (match(parser, TOKEN_LPAREN)) {
        /* Tuple type or grouped type */
        Type *types[64];
        int count = 0;
        types[count++] = parse_type(parser);
        if (match(parser, TOKEN_COMMA)) {
            /* It's a tuple */
            do {
                if (count >= 64) break;
                types[count++] = parse_type(parser);
            } while (match(parser, TOKEN_COMMA));
            consume(parser, TOKEN_RPAREN, "Expected ')' after tuple type");
            return type_tuple(parser->arena, types, count);
        }
        consume(parser, TOKEN_RPAREN, "Expected ')' after type");
        return types[0];
    }

    /* Check for primitive or named type */
    if (check(parser, TOKEN_TYPE_BOOL) || check(parser, TOKEN_TYPE_INT) ||
        check(parser, TOKEN_TYPE_FLOAT) || check(parser, TOKEN_TYPE_STRING) ||
        check(parser, TOKEN_TYPE_BYTE) || check(parser, TOKEN_TYPE_VOID)) {
        advance(parser);
        return parse_primitive_type(parser);
    }

    /* `null` as a type (almost always seen in a union: T | null) -- type
     * annotations are discarded at runtime regardless, so this just needs
     * to parse without error, not carry any real meaning. */
    if (check(parser, TOKEN_NULL)) {
        advance(parser);
        return type_primitive(parser->arena, PRIMITIVE_VOID);
    }

    if (check(parser, TOKEN_IDENTIFIER)) {
        advance(parser);
        const char *tname = token_str(parser, &parser->previous);

        /* Check for FFI-specific type names */
        if (strcmp(tname, "ptr") == 0)
            return type_primitive(parser->arena, PRIMITIVE_PTR);
        if (strcmp(tname, "c_int") == 0)
            return type_primitive(parser->arena, PRIMITIVE_C_INT);
        if (strcmp(tname, "c_double") == 0)
            return type_primitive(parser->arena, PRIMITIVE_C_DOUBLE);
        if (strcmp(tname, "c_float") == 0)
            return type_primitive(parser->arena, PRIMITIVE_C_FLOAT);
        if (strcmp(tname, "c_char") == 0)
            return type_primitive(parser->arena, PRIMITIVE_C_CHAR);

        Type *t = type_named(parser->arena, tname);
        /* Consume optional generic type arguments: Name<Type, Type> */
        if (match(parser, TOKEN_LESS)) {
            Type *args[16];
            int arg_count = 0;
            while (!check(parser, TOKEN_GREATER) && !check(parser, TOKEN_EOF)) {
                if (arg_count >= 16) break;
                args[arg_count++] = parse_type(parser);
                if (!match(parser, TOKEN_COMMA))
                    break;
            }
            consume(parser, TOKEN_GREATER, "Expected '>' after generic type arguments");
            (void)args; (void)arg_count;
        }
        return t;
    }

    parser_error(parser, "Expected type");
    return type_primitive(parser->arena, PRIMITIVE_VOID);
}

/* ─── Statement Parsing ─── */
static AstNode *parse_block(Parser *parser) {
    SourceLoc loc = current_loc(parser);
    AstNode *block = ast_block(parser->arena, loc);
    consume(parser, TOKEN_LBRACE, "Expected '{' to start block");

    /* Use a local array for block statements */
#define MAX_BLOCK_STMTS 1024
    AstNode *stmts[MAX_BLOCK_STMTS];
    int stmt_count = 0;

    while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        if (stmt_count >= MAX_BLOCK_STMTS) {
            parser_error(parser, "Too many statements in block");
            break;
        }
        stmts[stmt_count++] = parse_stmt(parser);
    }

    consume(parser, TOKEN_RBRACE, "Expected '}' to end block");
    block->block.stmts = (AstNode **)arena_alloc(parser->arena, sizeof(AstNode *) * stmt_count);
    block->block.stmt_count = stmt_count;
    memcpy(block->block.stmts, stmts, sizeof(AstNode *) * stmt_count);
    return block;
}

static AstNode *parse_let_decl(Parser *parser) {
    SourceLoc loc = current_loc(parser);
    bool is_mutable = parser->previous.type == TOKEN_LET;

    /* let name, ... = value */
    char *names[64];
    int name_count = 0;

    while (name_count < 64 && (parser->current.type == TOKEN_IDENTIFIER ||
                                 parser->current.type == TOKEN_UNDERSCORE)) {
        advance(parser);
        char *name;
        if (parser->previous.type == TOKEN_UNDERSCORE) {
            name = (char *)malloc(2);
            name[0] = '_';
            name[1] = '\0';
        } else {
            name = token_strdup(&parser->previous);
        }
        names[name_count++] = name;
        if (!match(parser, TOKEN_COMMA))
            break;
    }

    if (name_count == 0) {
        parser_error(parser, "Expected variable name(s) after let/const");
        return ast_null_literal(parser->arena, loc);
    }

    /* Optional type annotation: only for single-name declarations */
    if (name_count == 1 && match(parser, TOKEN_COLON)) {
        parse_type(parser);  /* consume type, but we don't store it in AST for now */
    }

    AstNode *initializer = NULL;
    if (match(parser, TOKEN_EQUAL)) {
        initializer = parse_expr(parser);
    } else if (!is_mutable) {
        parser_error(parser, "Const declaration requires an initializer");
    }

    match(parser, TOKEN_SEMICOLON); /* optional semicolon */
    AstNode *result = ast_let_decl(parser->arena, loc, names, name_count, initializer, is_mutable);
    for (int i = 0; i < name_count; i++)
        free(names[i]);
    return result;
}

static AstNode *parse_fn_decl(Parser *parser) {
    SourceLoc loc = current_loc(parser);
    (void)loc;

    /* fn name(params) -> return_type { body } */
    if (parser->current.type != TOKEN_IDENTIFIER) {
        parser_error(parser, "Expected function name");
        return ast_null_literal(parser->arena, loc);
    }
    advance(parser);
    char *name = token_str(parser, &parser->previous);

    /* Optional generic type parameters */
    char *type_params[8];
    int type_param_count = parse_type_params(parser, type_params, 8);

    /* Parameters */
    char *param_names[64];
    Type *param_types[64];
    int param_count = 0;

    consume(parser, TOKEN_LPAREN, "Expected '(' after function name");

    if (!check(parser, TOKEN_RPAREN)) {
        do {
            if (param_count >= 64) {
                parser_error(parser, "Too many function parameters");
                break;
            }
            /* param_name: type */
            if (parser->current.type != TOKEN_IDENTIFIER) {
                parser_error(parser, "Expected parameter name");
                break;
            }
            advance(parser);
            param_names[param_count] = token_strdup(&parser->previous);

            Type *pt = NULL;
            if (match(parser, TOKEN_COLON)) {
                pt = parse_type(parser);
            } else {
                pt = type_primitive(parser->arena, PRIMITIVE_INT); /* default */
            }
            param_types[param_count] = pt;
            param_count++;
        } while (match(parser, TOKEN_COMMA));
    }

    consume(parser, TOKEN_RPAREN, "Expected ')' after parameters");

    /* Return type(s) */
    Type *return_type = NULL;
    if (match(parser, TOKEN_ARROW)) {
        Type *ret_types[64];
        int ret_count = 0;
        ret_types[ret_count++] = parse_type(parser);
        while (match(parser, TOKEN_COMMA) && ret_count < 64) {
            ret_types[ret_count++] = parse_type(parser);
        }
        if (ret_count == 1) {
            return_type = ret_types[0];
        } else {
            return_type = type_tuple(parser->arena, ret_types, ret_count);
        }
    } else {
        return_type = type_primitive(parser->arena, PRIMITIVE_VOID);
    }

    /* Function type */
    Type *fn_type = type_function(parser->arena, param_types, param_count, return_type);

    /* Body */
    AstNode *body = NULL;
    if (match(parser, TOKEN_LBRACE)) {
        /* We already consumed '{' for the block, but parse_block expects to see it.
         * Back up: put it back conceptually. Actually, we just need to build a block.
         * We already consumed '{', so we need to parse statements until '}'. */
        SourceLoc block_loc = current_loc(parser);
        body = ast_block(parser->arena, block_loc);
#define MAX_BODY_STMTS 1024
        AstNode *stmts[MAX_BODY_STMTS];
        int stmt_count = 0;
        while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
            if (stmt_count >= MAX_BODY_STMTS) break;
            stmts[stmt_count++] = parse_stmt(parser);
        }
        consume(parser, TOKEN_RBRACE, "Expected '}' to end function body");
        body->block.stmts = (AstNode **)arena_alloc(parser->arena, sizeof(AstNode *) * stmt_count);
        body->block.stmt_count = stmt_count;
        memcpy(body->block.stmts, stmts, sizeof(AstNode *) * stmt_count);
    } else if (match(parser, TOKEN_FAT_ARROW)) {
        /* Arrow shorthand: fn name(params) => expr */
        body = ast_block(parser->arena, loc);
        AstNode *expr = parse_expr(parser);
        AstNode *return_stmt = ast_return_one(parser->arena, loc, expr);
        body->block.stmts = (AstNode **)arena_alloc(parser->arena, sizeof(AstNode *));
        body->block.stmts[0] = return_stmt;
        body->block.stmt_count = 1;
    }

    parser_register_function(parser, name, param_names, param_count);

    AstNode *fn_node = ast_fn_decl(parser->arena, loc, name, fn_type,
                        param_names, param_count,
                        type_params, type_param_count,
                        body, false, false, false, NULL,
                        NULL, NULL, 0);
    for (int i = 0; i < param_count; i++)
        free(param_names[i]);
    for (int i = 0; i < type_param_count; i++)
        free(type_params[i]);
    return fn_node;
}

static AstNode *parse_ffi_decl(Parser *parser) {
    SourceLoc loc = current_loc(parser);

    /* We already consumed '@'. Expect 'ffi' */
    if (!match(parser, TOKEN_IDENTIFIER) ||
        parser->previous.length != 3 ||
        memcmp(parser->previous.start, "ffi", 3) != 0) {
        parser_error(parser, "Expected '@ffi'");
        return ast_null_literal(parser->arena, loc);
    }

    /* Parse ("lib_name", "func_name") */
    consume(parser, TOKEN_LPAREN, "Expected '(' after @ffi");

    if (parser->current.type != TOKEN_STRING) {
        parser_error(parser, "Expected library name string in @ffi");
        return ast_null_literal(parser->arena, loc);
    }
    advance(parser);
    char *lib_name;
    if (parser->previous.value) {
        int len = (int)strlen(parser->previous.value);
        lib_name = (char *)malloc(len + 1);
        memcpy(lib_name, parser->previous.value, len + 1);
    } else {
        parser_error(parser, "Invalid library name string");
        return ast_null_literal(parser->arena, loc);
    }

    consume(parser, TOKEN_COMMA, "Expected ',' in @ffi");

    if (parser->current.type != TOKEN_STRING) {
        parser_error(parser, "Expected function name string in @ffi");
        return ast_null_literal(parser->arena, loc);
    }
    advance(parser);
    char *func_name;
    if (parser->previous.value) {
        int len = (int)strlen(parser->previous.value);
        func_name = (char *)malloc(len + 1);
        memcpy(func_name, parser->previous.value, len + 1);
    } else {
        parser_error(parser, "Invalid function name string");
        free(lib_name);
        return ast_null_literal(parser->arena, loc);
    }

    consume(parser, TOKEN_RPAREN, "Expected ')' after @ffi arguments");

    /* Now parse the fn declaration */
    if (!match(parser, TOKEN_FN)) {
        parser_error(parser, "Expected 'fn' after @ffi");
        return ast_null_literal(parser->arena, loc);
    }

    /* fn name(params) -> ret_type */
    if (parser->current.type != TOKEN_IDENTIFIER) {
        parser_error(parser, "Expected function name");
        return ast_null_literal(parser->arena, loc);
    }
    advance(parser);
    char *name = token_str(parser, &parser->previous);

    /* Parameters */
    char *param_names[64];
    Type *param_types[64];
    int param_count = 0;

    consume(parser, TOKEN_LPAREN, "Expected '(' after function name");

    if (!check(parser, TOKEN_RPAREN)) {
        do {
            if (param_count >= 64) {
                parser_error(parser, "Too many function parameters");
                break;
            }
            if (parser->current.type != TOKEN_IDENTIFIER) {
                parser_error(parser, "Expected parameter name");
                break;
            }
            advance(parser);
            param_names[param_count] = token_strdup(&parser->previous);

            Type *pt = NULL;
            if (match(parser, TOKEN_COLON)) {
                pt = parse_type(parser);
            } else {
                pt = type_primitive(parser->arena, PRIMITIVE_PTR); /* default to ptr */
            }
            param_types[param_count] = pt;
            param_count++;
        } while (match(parser, TOKEN_COMMA));
    }

    consume(parser, TOKEN_RPAREN, "Expected ')' after parameters");

    /* Return type */
    Type *return_type = NULL;
    if (match(parser, TOKEN_ARROW)) {
        return_type = parse_type(parser);
    } else {
        return_type = type_primitive(parser->arena, PRIMITIVE_VOID);
    }

    /* Function type — stored in node->type for the compiler */
    Type *fn_type = type_function(parser->arena, param_types, param_count, return_type);

    /* Create FFI declaration node */
    AstNode *node = ast_ffi_decl(parser->arena, loc, name, lib_name, func_name,
                                  param_names, param_count);
    node->type = fn_type;

    /* Register so that method dispatch can find this if needed */
    if (parser_is_method(parser, name)) {
        /* It's a method — handled by dispatch table */
    }

    for (int i = 0; i < param_count; i++)
        free(param_names[i]);
    free(lib_name);
    free(func_name);
    /* name is arena-allocated — not freed */

    return node;
}

static AstNode *parse_comptime(Parser *parser) {
    /* comptime { body } or comptime expr */
    SourceLoc loc = current_loc(parser);

    if (match(parser, TOKEN_LBRACE)) {
        /* comptime { ... } */
        AstNode *body = ast_block(parser->arena, loc);
        while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF))
            ast_block_add_stmt(body, parse_stmt(parser));
        consume(parser, TOKEN_RBRACE, "Expected '}' after comptime block");
        return ast_comptime(parser->arena, loc, body);
    }

    /* comptime expr */
    AstNode *expr = parse_expr(parser);
    match(parser, TOKEN_SEMICOLON);
    return ast_comptime(parser->arena, loc, expr);
}

static AstNode *parse_if_stmt(Parser *parser) {
    SourceLoc loc = current_loc(parser);
    AstNode *cond = parse_expr(parser);
    AstNode *then_branch = parse_block(parser);
    AstNode *else_branch = NULL;

    if (match(parser, TOKEN_ELSE)) {
        if (check(parser, TOKEN_IF)) {
            advance(parser);
            else_branch = parse_if_stmt(parser);
        } else {
            else_branch = parse_block(parser);
        }
    }

    return ast_if(parser->arena, loc, cond, then_branch, else_branch);
}

static AstNode *parse_while_stmt(Parser *parser) {
    SourceLoc loc = current_loc(parser);
    AstNode *cond = parse_expr(parser);
    parser->loop_depth++;
    AstNode *body = parse_block(parser);
    parser->loop_depth--;
    return ast_while(parser->arena, loc, cond, body);
}

static AstNode *parse_for_stmt(Parser *parser) {
    SourceLoc loc = current_loc(parser);

    /* for var in iterable { body } */
    if (parser->current.type != TOKEN_IDENTIFIER) {
        parser_error(parser, "Expected loop variable name after 'for'");
        return ast_null_literal(parser->arena, loc);
    }
    advance(parser);
    char *var_name = token_str(parser, &parser->previous);

    consume(parser, TOKEN_IN, "Expected 'in' after loop variable");
    AstNode *iterable = parse_expr(parser);

    parser->loop_depth++;
    AstNode *body = parse_block(parser);
    parser->loop_depth--;

    return ast_for(parser->arena, loc, var_name, iterable, body);
}

static AstNode *parse_loop_stmt(Parser *parser) {
    SourceLoc loc = current_loc(parser);
    parser->loop_depth++;
    AstNode *body = parse_block(parser);
    parser->loop_depth--;
    return ast_loop(parser->arena, loc, body);
}

static AstNode *parse_struct_decl(Parser *parser) {
    SourceLoc loc = current_loc(parser);

    /* Collect decorators before struct */
    char *decorator_keys[32];
    AstNode *decorator_values[32];
    int decorator_count = 0;

    while (check(parser, TOKEN_AT)) {
        advance(parser); /* consume @ */
        if (check(parser, TOKEN_IDENTIFIER)) {
            advance(parser);
            char *key = token_strdup(&parser->previous);
            if (decorator_count >= 32) {
                parser_error(parser, "Too many decorators on struct");
                free(key);
                break;
            }
            decorator_keys[decorator_count] = key;
            if (match(parser, TOKEN_LPAREN)) {
                decorator_values[decorator_count] = parse_expr(parser);
                consume(parser, TOKEN_RPAREN, "Expected ')' after decorator argument");
            } else {
                decorator_values[decorator_count] = ast_bool_literal(parser->arena, current_loc(parser), true);
            }
            decorator_count++;
        } else {
            parser_error(parser, "Expected identifier after '@'");
            break;
        }
    }

    /* struct Name { field1: Type, field2: Type } */
    if (parser->current.type != TOKEN_IDENTIFIER) {
        parser_error(parser, "Expected struct name after 'struct'");
        for (int i = 0; i < decorator_count; i++) free(decorator_keys[i]);
        return ast_null_literal(parser->arena, loc);
    }
    advance(parser);
    char *name = token_str(parser, &parser->previous);

    /* Optional generic type parameters */
    char *type_params[8];
    int type_param_count = parse_type_params(parser, type_params, 8);

    consume(parser, TOKEN_LBRACE, "Expected '{' after struct name");

    char *field_names[64];
    int field_count = 0;
    char *field_decorator_keys[64][32];
    AstNode *field_decorator_values[64][32];
    int field_decorator_counts[64] = {0};

    while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        if (field_count >= 64) {
            parser_error(parser, "Too many struct fields");
            break;
        }

        /* Collect field decorators */
        int fdeco_count = 0;
        while (check(parser, TOKEN_AT)) {
            advance(parser); /* consume @ */
            if (check(parser, TOKEN_IDENTIFIER)) {
                advance(parser);
                char *key = token_strdup(&parser->previous);
                if (fdeco_count >= 32) {
                    parser_error(parser, "Too many decorators on field");
                    free(key);
                    break;
                }
                field_decorator_keys[field_count][fdeco_count] = key;
                if (match(parser, TOKEN_LPAREN)) {
                    field_decorator_values[field_count][fdeco_count] = parse_expr(parser);
                    consume(parser, TOKEN_RPAREN, "Expected ')' after decorator argument");
                } else {
                    field_decorator_values[field_count][fdeco_count] = ast_bool_literal(parser->arena, current_loc(parser), true);
                }
                fdeco_count++;
            } else {
                parser_error(parser, "Expected identifier after '@'");
                break;
            }
        }
        field_decorator_counts[field_count] = fdeco_count;

        if (parser->current.type != TOKEN_IDENTIFIER) {
            parser_error(parser, "Expected field name");
            break;
        }
        advance(parser);
        field_names[field_count++] = token_strdup(&parser->previous);

        /* Optional type annotation */
        if (match(parser, TOKEN_COLON))
            parse_type(parser);

        if (!match(parser, TOKEN_COMMA))
            break;
    }
    consume(parser, TOKEN_RBRACE, "Expected '}' after struct fields");

    /* Register struct type */
    parser_register_struct(parser, name, field_names, field_count);

    /* Prepare field decorator arrays for ast_struct_decl */
    char **fdk_ptrs[64] = {0};
    AstNode **fdv_ptrs[64] = {0};
    int *fdc_ptr = field_decorator_counts;
    for (int i = 0; i < field_count; i++) {
        if (field_decorator_counts[i] > 0) {
            fdk_ptrs[i] = field_decorator_keys[i];
            fdv_ptrs[i] = field_decorator_values[i];
        }
    }

    AstNode *result = ast_struct_decl(parser->arena, loc, name, field_names, field_count,
                                      type_params, type_param_count,
                                      decorator_keys, decorator_values, decorator_count,
                                      fdk_ptrs, fdv_ptrs, fdc_ptr);

    for (int i = 0; i < field_count; i++)
        free(field_names[i]);
    for (int i = 0; i < type_param_count; i++)
        free(type_params[i]);
    for (int i = 0; i < decorator_count; i++)
        free(decorator_keys[i]);
    for (int i = 0; i < field_count; i++) {
        for (int j = 0; j < field_decorator_counts[i]; j++) {
            free(field_decorator_keys[i][j]);
        }
    }

    return result;
}

/* ─── Actor registry ─── */
static void parser_register_actor(Parser *parser, const char *name,
                                   char **field_names, int field_count,
                                   char **method_names, int method_count) {
    if (parser->actor_count >= 128) return;
    ActorDef *ad = &parser->actors[parser->actor_count++];
    ad->name = (char *)malloc(strlen(name) + 1);
    strcpy(ad->name, name);
    ad->field_names = (char **)malloc(sizeof(char *) * field_count);
    for (int i = 0; i < field_count; i++) {
        ad->field_names[i] = (char *)malloc(strlen(field_names[i]) + 1);
        strcpy(ad->field_names[i], field_names[i]);
    }
    ad->field_count = field_count;
    ad->method_names = (char **)malloc(sizeof(char *) * method_count);
    for (int i = 0; i < method_count; i++) {
        ad->method_names[i] = (char *)malloc(strlen(method_names[i]) + 1);
        strcpy(ad->method_names[i], method_names[i]);
    }
    ad->method_count = method_count;
}

static AstNode *parse_actor_decl(Parser *parser) {
    SourceLoc loc = current_loc(parser);

    /* actor Name { field: Type = default, ..., fn method(self, ...) { ... } } */
    if (parser->current.type != TOKEN_IDENTIFIER) {
        parser_error(parser, "Expected actor name after 'actor'");
        return ast_null_literal(parser->arena, loc);
    }
    advance(parser);
    char *name = token_str(parser, &parser->previous);

    consume(parser, TOKEN_LBRACE, "Expected '{' after actor name");

    char *field_names[64];
    int field_count = 0;
    AstNode *method_stmts[64];
    int method_count = 0;
    char *method_names[64];

    while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        if (check(parser, TOKEN_FN)) {
            /* Method declaration */
            if (method_count >= 64) {
                parser_error(parser, "Too many actor methods");
                break;
            }
            advance(parser); /* consume 'fn' */

            if (parser->current.type != TOKEN_IDENTIFIER) {
                parser_error(parser, "Expected method name");
                break;
            }
            advance(parser);
            char *method_name = token_str(parser, &parser->previous);
            parser_register_method(parser, method_name);

            /* Parameters */
            consume(parser, TOKEN_LPAREN, "Expected '(' after method name");
            char *param_names[64];
            Type *param_types[64];
            int param_count = 0;

            if (!check(parser, TOKEN_RPAREN)) {
                do {
                    if (param_count >= 64) break;
                    if (parser->current.type != TOKEN_IDENTIFIER) break;
                    advance(parser);
                    param_names[param_count] = token_strdup(&parser->previous);
                    Type *pt = NULL;
                    if (match(parser, TOKEN_COLON))
                        pt = parse_type(parser);
                    else
                        pt = type_primitive(parser->arena, PRIMITIVE_INT);
                    param_types[param_count] = pt;
                    param_count++;
                } while (match(parser, TOKEN_COMMA));
            }
            consume(parser, TOKEN_RPAREN, "Expected ')' after parameters");

            /* Return type */
            Type *return_type = NULL;
            if (match(parser, TOKEN_ARROW))
                return_type = parse_type(parser);
            else
                return_type = type_primitive(parser->arena, PRIMITIVE_VOID);

            Type *fn_type = type_function(parser->arena, param_types, param_count, return_type);

            /* Body */
            AstNode *body = NULL;
            if (match(parser, TOKEN_LBRACE)) {
                body = ast_block(parser->arena, loc);
                AstNode *body_stmts[1024];
                int body_count = 0;
                while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
                    if (body_count >= 1024) break;
                    body_stmts[body_count++] = parse_stmt(parser);
                }
                consume(parser, TOKEN_RBRACE, "Expected '}' to end method body");
                body->block.stmts = (AstNode **)arena_alloc(parser->arena, sizeof(AstNode *) * body_count);
                body->block.stmt_count = body_count;
                memcpy(body->block.stmts, body_stmts, sizeof(AstNode *) * body_count);
            } else if (match(parser, TOKEN_FAT_ARROW)) {
                body = ast_block(parser->arena, loc);
                AstNode *expr = parse_expr(parser);
                AstNode *ret = ast_return_one(parser->arena, loc, expr);
                body->block.stmts = (AstNode **)arena_alloc(parser->arena, sizeof(AstNode *));
                body->block.stmts[0] = ret;
                body->block.stmt_count = 1;
            }

            AstNode *fn_node = ast_fn_decl(parser->arena, loc, method_name, fn_type,
                                param_names, param_count, NULL, 0,
                                body, false, false, true, name,
                                NULL, NULL, 0);
            for (int i = 0; i < param_count; i++)
                free(param_names[i]);

            method_stmts[method_count] = fn_node;
            method_names[method_count] = (char *)malloc(strlen(method_name) + 1);
            strcpy(method_names[method_count], method_name);
            method_count++;

        } else {
            /* Field declaration */
            if (field_count >= 64) {
                parser_error(parser, "Too many actor fields");
                break;
            }
            if (parser->current.type != TOKEN_IDENTIFIER) {
                parser_error(parser, "Expected field name or 'fn'");
                break;
            }
            advance(parser);
            char *fname = token_strdup(&parser->previous);

            /* Optional type annotation */
            if (match(parser, TOKEN_COLON))
                parse_type(parser);

            /* Optional default value */
            if (match(parser, TOKEN_EQUAL))
                (void)parse_expr(parser);  /* consume but ignore for now */

            field_names[field_count++] = fname;

            if (!match(parser, TOKEN_COMMA))
                break;
        }
    }

    consume(parser, TOKEN_RBRACE, "Expected '}' after actor body");

    /* Register the actor's struct layout for later field access */
    parser_register_struct(parser, name, field_names, field_count);

    /* Register actor for spawn lookup */
    parser_register_actor(parser, name, field_names, field_count, method_names, method_count);

    /* Wrap everything in a block: struct decl + methods + actor module spawn registration */
    AstNode *block = ast_block(parser->arena, loc);
    int total_stmts = 0;
    AstNode *all_stmts[128];

    /* Add actor init marker (emits BC_ACTOR_INIT at compile time) */
    all_stmts[total_stmts++] = ast_actor_decl(parser->arena, loc, name, field_names, field_count);

    /* Add method declarations */
    for (int i = 0; i < method_count; i++)
        all_stmts[total_stmts++] = method_stmts[i];

    /* Register spawn method for the actor type's module */
    /* At runtime, the spawn function is a builtin native function registered in init */
    /* We add a NODE_FFI_DECL-like marker: we'll just rely on runtime registration */
    /* Instead, we emit a struct_decl marker so the compiler registers the type */

    block->block.stmts = (AstNode **)arena_alloc(parser->arena, sizeof(AstNode *) * total_stmts);
    block->block.stmt_count = total_stmts;
    memcpy(block->block.stmts, all_stmts, sizeof(AstNode *) * total_stmts);

    for (int i = 0; i < field_count; i++)
        free(field_names[i]);
    for (int i = 0; i < method_count; i++)
        free(method_names[i]);

    return block;
}

static AstNode *parse_enum_decl(Parser *parser) {
    SourceLoc loc = current_loc(parser);

    /* enum Name { Variant, Variant(Type), ... } */
    if (parser->current.type != TOKEN_IDENTIFIER) {
        parser_error(parser, "Expected enum name after 'enum'");
        return ast_null_literal(parser->arena, loc);
    }
    advance(parser);
    char *name = token_str(parser, &parser->previous);

    char *type_params[8];
    int type_param_count = parse_type_params(parser, type_params, 8);

    consume(parser, TOKEN_LBRACE, "Expected '{' after enum name");

    char *variant_names[64];
    int variant_counts[64];
    int variant_count = 0;

    while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        if (variant_count >= 64) {
            parser_error(parser, "Too many enum variants");
            break;
        }
        if (parser->current.type != TOKEN_IDENTIFIER) {
            parser_error(parser, "Expected variant name");
            break;
        }
        advance(parser);
        variant_names[variant_count] = token_strdup(&parser->previous);
        variant_counts[variant_count] = 0;

        /* Optional associated values: Variant(Type, Type) */
        if (match(parser, TOKEN_LPAREN)) {
            int count = 0;
            while (!check(parser, TOKEN_RPAREN) && !check(parser, TOKEN_EOF)) {
                parse_type(parser);
                count++;
                if (!match(parser, TOKEN_COMMA))
                    break;
            }
            consume(parser, TOKEN_RPAREN, "Expected ')' after variant types");
            variant_counts[variant_count] = count;
        }

        variant_count++;
        if (!match(parser, TOKEN_COMMA))
            break;
    }
    consume(parser, TOKEN_RBRACE, "Expected '}' after enum variants");

    parser_register_enum(parser, name, variant_names, variant_counts, variant_count);

    AstNode *result = ast_enum_decl(parser->arena, loc, name, variant_names, variant_counts, variant_count,
                                      type_params, type_param_count);
    for (int i = 0; i < variant_count; i++)
        free(variant_names[i]);
    for (int i = 0; i < type_param_count; i++)
        free(type_params[i]);
    return result;
}

static AstNode *parse_enum_literal(Parser *parser, const char *enum_name, SourceLoc loc) {
    /* Name::Variant or Name::Variant(args) */
    consume(parser, TOKEN_DOUBLE_COLON, "Expected '::' after enum type name");

    if (parser->current.type != TOKEN_IDENTIFIER) {
        parser_error(parser, "Expected variant name after '::'");
        return ast_null_literal(parser->arena, loc);
    }
    advance(parser);
    char *variant_name = token_strdup(&parser->previous);

    /* Find the variant in the enum */
    EnumDef *ed = parser_find_enum(parser, enum_name);
    if (!ed) {
        parser_error(parser, "Unknown enum type");
        free(variant_name);
        return ast_null_literal(parser->arena, loc);
    }

    int tag = -1;
    for (int i = 0; i < ed->variant_count; i++) {
        if (strcmp(ed->variant_names[i], variant_name) == 0) {
            tag = i;
            break;
        }
    }
    if (tag < 0) {
        parser_error(parser, "Unknown variant");
        free(variant_name);
        return ast_null_literal(parser->arena, loc);
    }

    /* Parse optional arguments */
    AstNode *values[64];
    int value_count = 0;
    if (match(parser, TOKEN_LPAREN)) {
        while (!check(parser, TOKEN_RPAREN) && !check(parser, TOKEN_EOF)) {
            if (value_count >= 64) break;
            values[value_count++] = parse_expr(parser);
            if (!match(parser, TOKEN_COMMA))
                break;
        }
        consume(parser, TOKEN_RPAREN, "Expected ')' after variant arguments");
    }

    AstNode *result = ast_enum_literal(parser->arena, loc, enum_name, variant_name, tag, values, value_count);
    free(variant_name);
    return result;
}

static AstNode *parse_trait_decl(Parser *parser) {
    SourceLoc loc = current_loc(parser);

    /* trait Name { fn method(self, ...) -> Type; ... } */
    if (parser->current.type != TOKEN_IDENTIFIER) {
        parser_error(parser, "Expected trait name after 'trait'");
        return ast_null_literal(parser->arena, loc);
    }
    advance(parser);
    char *name = token_str(parser, &parser->previous);

    consume(parser, TOKEN_LBRACE, "Expected '{' after trait name");

    char *method_names[64];
    int method_count = 0;

    while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        if (method_count >= 64) break;

        if (!match(parser, TOKEN_FN)) {
            parser_error(parser, "Expected 'fn' in trait");
            break;
        }

        if (parser->current.type != TOKEN_IDENTIFIER) break;
        advance(parser);
        method_names[method_count] = token_strdup(&parser->previous);
        method_count++;

        /* Skip rest of signature: (params) -> type */
        if (match(parser, TOKEN_LPAREN)) {
            int depth = 1;
            while (depth > 0 && !check(parser, TOKEN_EOF)) {
                if (check(parser, TOKEN_LPAREN)) depth++;
                if (check(parser, TOKEN_RPAREN)) depth--;
                if (depth > 0) advance(parser);
            }
            if (depth == 0) advance(parser);
        }
        if (match(parser, TOKEN_ARROW))
            parse_type(parser);

        match(parser, TOKEN_SEMICOLON);
        match(parser, TOKEN_COMMA);
    }
    consume(parser, TOKEN_RBRACE, "Expected '}' after trait methods");

    AstNode *result = ast_trait_decl(parser->arena, loc, name, method_names, method_count);
    for (int i = 0; i < method_count; i++)
        free(method_names[i]);
    return result;
}

static AstNode *parse_struct_literal(Parser *parser, const char *type_name, SourceLoc loc) {
    /* TypeName { field1: expr, field2: expr } */
    StructDef *sd = parser_find_struct(parser, type_name);
    if (!sd) {
        if (!parser->had_error) {
            parser->had_error = true;
            snprintf(parser->error_message, sizeof(parser->error_message),
                     "Unknown struct type '%s'", type_name);
        }
        return ast_null_literal(parser->arena, loc);
    }

    consume(parser, TOKEN_LBRACE, "Expected '{' after struct type");

    char *field_names[64];
    AstNode *field_values[64];
    int field_count = 0;

    while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        if (field_count >= 64) break;

        if (parser->current.type != TOKEN_IDENTIFIER) break;
        advance(parser);
        char *fname = token_strdup(&parser->previous);
        consume(parser, TOKEN_COLON, "Expected ':' after field name");
        field_names[field_count] = fname;
        field_values[field_count] = parse_expr(parser);
        field_count++;

        if (!match(parser, TOKEN_COMMA))
            break;
    }
    consume(parser, TOKEN_RBRACE, "Expected '}' after struct literal");

    AstNode *result = ast_struct_literal(parser->arena, loc, type_name,
                                         field_names, field_values, field_count);
    for (int i = 0; i < field_count; i++)
        free(field_names[i]);
    return result;
}

static AstNode *parse_impl_block(Parser *parser) {
    SourceLoc loc = current_loc(parser);

    /* impl TypeName { fn method(self, ...) { ... } ... } */
    if (parser->current.type != TOKEN_IDENTIFIER) {
        parser_error(parser, "Expected type name after 'impl'");
        return ast_null_literal(parser->arena, loc);
    }
    advance(parser);
    char *type_name = token_str(parser, &parser->previous);

    consume(parser, TOKEN_LBRACE, "Expected '{' after impl type name");

    /* Parse method declarations — wrap them in a block as stmts */
    AstNode *stmts[64];
    int stmt_count = 0;

    while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        if (stmt_count >= 64) break;

        if (!match(parser, TOKEN_FN)) {
            parser_error(parser, "Expected 'fn' inside impl block");
            break;
        }

        /* Parse method name */
        if (parser->current.type != TOKEN_IDENTIFIER) {
            parser_error(parser, "Expected method name");
            break;
        }
        advance(parser);
        char *method_name = token_str(parser, &parser->previous);

        /* Register as method */
        parser_register_method(parser, method_name);

        /* Parameters — first is 'self' which we handle as a regular param */
        consume(parser, TOKEN_LPAREN, "Expected '(' after method name");

        char *param_names[64];
        Type *param_types[64];
        int param_count = 0;

        if (!check(parser, TOKEN_RPAREN)) {
            do {
                if (param_count >= 64) break;
                if (parser->current.type != TOKEN_IDENTIFIER) break;
                advance(parser);
                param_names[param_count] = token_strdup(&parser->previous);
                Type *pt = NULL;
                if (match(parser, TOKEN_COLON))
                    pt = parse_type(parser);
                else
                    pt = type_primitive(parser->arena, PRIMITIVE_INT);
                param_types[param_count] = pt;
                param_count++;
            } while (match(parser, TOKEN_COMMA));
        }
        consume(parser, TOKEN_RPAREN, "Expected ')' after parameters");

        /* Return type */
        Type *return_type = NULL;
        if (match(parser, TOKEN_ARROW))
            return_type = parse_type(parser);
        else
            return_type = type_primitive(parser->arena, PRIMITIVE_VOID);

        /* Function type */
        Type *fn_type = type_function(parser->arena, param_types, param_count, return_type);

        /* Body */
        AstNode *body = NULL;
        if (match(parser, TOKEN_LBRACE)) {
            body = ast_block(parser->arena, loc);
            AstNode *body_stmts[1024];
            int body_count = 0;
            while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
                if (body_count >= 1024) break;
                body_stmts[body_count++] = parse_stmt(parser);
            }
            consume(parser, TOKEN_RBRACE, "Expected '}' to end method body");
            body->block.stmts = (AstNode **)arena_alloc(parser->arena, sizeof(AstNode *) * body_count);
            body->block.stmt_count = body_count;
            memcpy(body->block.stmts, body_stmts, sizeof(AstNode *) * body_count);
        } else if (match(parser, TOKEN_FAT_ARROW)) {
            body = ast_block(parser->arena, loc);
            AstNode *expr = parse_expr(parser);
            AstNode *ret = ast_return_one(parser->arena, loc, expr);
            body->block.stmts = (AstNode **)arena_alloc(parser->arena, sizeof(AstNode *));
            body->block.stmts[0] = ret;
            body->block.stmt_count = 1;
        }

        AstNode *fn_node = ast_fn_decl(parser->arena, loc, method_name, fn_type,
                            param_names, param_count, NULL, 0,
                            body, false, false, true, type_name,
                            NULL, NULL, 0);
        for (int i = 0; i < param_count; i++)
            free(param_names[i]);

        stmts[stmt_count++] = fn_node;
    }

    consume(parser, TOKEN_RBRACE, "Expected '}' after impl block");

    /* Wrap methods in a block statement */
    AstNode *block = ast_block(parser->arena, loc);
    block->block.stmts = (AstNode **)arena_alloc(parser->arena, sizeof(AstNode *) * stmt_count);
    block->block.stmt_count = stmt_count;
    memcpy(block->block.stmts, stmts, sizeof(AstNode *) * stmt_count);
    return block;
}

static AstNode *parse_return_stmt(Parser *parser) {
    SourceLoc loc = current_loc(parser);
    AstNode *values[64];
    int value_count = 0;
    if (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_SEMICOLON) &&
        !check(parser, TOKEN_EOF)) {
        values[value_count++] = parse_expr(parser);
        while (match(parser, TOKEN_COMMA) && value_count < 64) {
            values[value_count++] = parse_expr(parser);
        }
    }
    match(parser, TOKEN_SEMICOLON);
    return ast_return(parser->arena, loc, values, value_count);
}

/* ─── Main Statement Parser ─── */
static AstNode *parse_assert_stmt(Parser *parser);
static AstNode *parse_test_decl(Parser *parser);

static AstNode *parse_stmt(Parser *parser) {
    if (check(parser, TOKEN_LET) || check(parser, TOKEN_CONST)) {
        advance(parser);
        return parse_let_decl(parser);
    }

    /* Collect decorators before fn */
    char *decorator_keys[32];
    AstNode *decorator_values[32];
    int decorator_count = 0;

    while (check(parser, TOKEN_AT)) {
        advance(parser); /* consume @ */

        if (check(parser, TOKEN_IDENTIFIER)) {
            /* Check for @ffi — existing ffi decorator */
            if (parser->current.length == 3 &&
                memcmp(parser->current.start, "ffi", 3) == 0) {
                return parse_ffi_decl(parser);
            }

            /* General decorator: @name or @name(args) */
            advance(parser);
            char *key = token_strdup(&parser->previous);
            if (decorator_count >= 32) {
                parser_error(parser, "Too many decorators");
                free(key);
                break;
            }
            decorator_keys[decorator_count] = key;

            if (match(parser, TOKEN_LPAREN)) {
                /* @name(expr) */
                decorator_values[decorator_count] = parse_expr(parser);
                consume(parser, TOKEN_RPAREN, "Expected ')' after decorator argument");
            } else {
                /* @name — value is just `true` */
                decorator_values[decorator_count] = ast_bool_literal(parser->arena, current_loc(parser), true);
            }
            decorator_count++;
        } else {
            parser_error(parser, "Expected identifier after '@'");
            break;
        }
    }

    if (match(parser, TOKEN_FN)) {
        if (decorator_count > 0) {
            /* Parse fn with decorators */
            AstNode *fn = parse_fn_decl(parser);
            fn->fn_decl.decorator_keys = (char **)arena_alloc(parser->arena, sizeof(char *) * decorator_count);
            fn->fn_decl.decorator_values = (AstNode **)arena_alloc(parser->arena, sizeof(AstNode *) * decorator_count);
            for (int i = 0; i < decorator_count; i++) {
                fn->fn_decl.decorator_keys[i] = (char *)arena_alloc(parser->arena, strlen(decorator_keys[i]) + 1);
                strcpy(fn->fn_decl.decorator_keys[i], decorator_keys[i]);
                free(decorator_keys[i]);
                fn->fn_decl.decorator_values[i] = decorator_values[i];
            }
            fn->fn_decl.decorator_count = decorator_count;
            return fn;
        }
        return parse_fn_decl(parser);
    }

    /* No fn after decorators — error or ignore decorators for non-fn statements */
    if (decorator_count > 0) {
        parser_error(parser, "Decorators can only be applied to function declarations");
        for (int i = 0; i < decorator_count; i++) free(decorator_keys[i]);
        return ast_null_literal(parser->arena, current_loc(parser));
    }

    if (match(parser, TOKEN_IF)) {
        return parse_if_stmt(parser);
    }

    if (match(parser, TOKEN_WHILE)) {
        return parse_while_stmt(parser);
    }

    if (match(parser, TOKEN_FOR)) {
        return parse_for_stmt(parser);
    }

    if (match(parser, TOKEN_LOOP)) {
        return parse_loop_stmt(parser);
    }

    if (match(parser, TOKEN_RETURN)) {
        return parse_return_stmt(parser);
    }

    if (match(parser, TOKEN_BREAK)) {
        if (parser->loop_depth <= 0)
            parser_error(parser, "break outside loop");
        match(parser, TOKEN_SEMICOLON);
        return ast_break(parser->arena, current_loc(parser));
    }

    if (match(parser, TOKEN_CONTINUE)) {
        if (parser->loop_depth <= 0)
            parser_error(parser, "continue outside loop");
        match(parser, TOKEN_SEMICOLON);
        return ast_continue(parser->arena, current_loc(parser));
    }

    if (match(parser, TOKEN_LBRACE)) {
        return parse_block(parser);
    }

    if (match(parser, TOKEN_STRUCT)) {
        return parse_struct_decl(parser);
    }

    if (match(parser, TOKEN_ENUM)) {
        return parse_enum_decl(parser);
    }

    if (match(parser, TOKEN_ACTOR)) {
        return parse_actor_decl(parser);
    }

    if (match(parser, TOKEN_IMPL)) {
        return parse_impl_block(parser);
    }

    if (match(parser, TOKEN_TRAIT)) {
        return parse_trait_decl(parser);
    }

    if (match(parser, TOKEN_COMPTIME)) {
        return parse_comptime(parser);
    }

    if (match(parser, TOKEN_TRY)) {
        SourceLoc loc = current_loc(parser);
        AstNode *try_body = parse_block(parser);
        AstNode *catch_body = NULL;
        char *catch_var = NULL;

        if (match(parser, TOKEN_CATCH)) {
            /* Optional catch variable: catch err { ... } */
            if (parser->current.type == TOKEN_IDENTIFIER) {
                advance(parser);
                catch_var = token_str(parser, &parser->previous);
            }
            catch_body = parse_block(parser);
        }

        return ast_try(parser->arena, loc, try_body, catch_body, catch_var);
    }

    if (match(parser, TOKEN_ASSERT)) {
        return parse_assert_stmt(parser);
    }

    if (match(parser, TOKEN_TEST)) {
        return parse_test_decl(parser);
    }

    /* Expression statement */
    AstNode *expr = parse_expr(parser);
    match(parser, TOKEN_SEMICOLON);
    return ast_expr_stmt(parser->arena, current_loc(parser), expr);
}

/* ─── Assert Statement ─── */
static AstNode *parse_assert_stmt(Parser *parser) {
    SourceLoc loc = current_loc(parser);
    AstNode *condition = parse_expr(parser);
    return ast_assert_stmt(parser->arena, loc, condition);
}

/* ─── Test Declaration ─── */
static AstNode *parse_test_decl(Parser *parser) {
    SourceLoc loc = current_loc(parser);

    /* test "description" { body } */
    if (parser->current.type != TOKEN_STRING) {
        parser_error(parser, "Expected test description string");
        return ast_null_literal(parser->arena, loc);
    }
    advance(parser);
    char *description = token_strdup(&parser->previous);

    AstNode *body = NULL;
    if (match(parser, TOKEN_LBRACE)) {
        SourceLoc block_loc = current_loc(parser);
        body = ast_block(parser->arena, block_loc);
#define MAX_TEST_BODY_STMTS 4096
        AstNode *stmts[MAX_TEST_BODY_STMTS];
        int stmt_count = 0;
        while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
            if (stmt_count >= MAX_TEST_BODY_STMTS) break;
            stmts[stmt_count++] = parse_stmt(parser);
        }
        consume(parser, TOKEN_RBRACE, "Expected '}' to end test body");
        body->block.stmts = (AstNode **)arena_alloc(parser->arena, sizeof(AstNode *) * stmt_count);
        body->block.stmt_count = stmt_count;
        memcpy(body->block.stmts, stmts, sizeof(AstNode *) * stmt_count);
    } else {
        parser_error(parser, "Expected '{' after test description");
        free(description);
        return ast_null_literal(parser->arena, loc);
    }

    return ast_test_decl(parser->arena, loc, description, body);
}

/* ─── Expression Parsing (Pratt-style) ─── */
static AstNode *parse_expr(Parser *parser) {
    return parse_range(parser);
}

static AstNode *parse_range(Parser *parser) {
    AstNode *expr = parse_assignment(parser);

    if (match(parser, TOKEN_DOT_DOT)) {
        AstNode *right = parse_assignment(parser);
        SourceLoc loc = current_loc(parser);
        /* Build as a tuple (start, end) — runtime will interpret for loops */
        AstNode *elems[2] = {expr, right};
        AstNode *tuple = ast_tuple_literal(parser->arena, loc);
        tuple->tuple_literal.elements = (AstNode **)arena_alloc(parser->arena, sizeof(AstNode *) * 2);
        memcpy(tuple->tuple_literal.elements, elems, sizeof(AstNode *) * 2);
        tuple->tuple_literal.element_count = 2;
        return tuple;
    }

    return expr;
}

static AstNode *parse_nil_coalesce(Parser *parser) {
    AstNode *expr = parse_or(parser);

    while (match(parser, TOKEN_QUESTION_QUESTION)) {
        AstNode *right = parse_or(parser);
        expr = ast_binary(parser->arena, current_loc(parser), OP_NIL_COALESCE, expr, right);
    }

    return expr;
}

static AstNode *parse_assignment(Parser *parser) {
    AstNode *expr = parse_nil_coalesce(parser);

    if (match(parser, TOKEN_EQUAL)) {
        AstNode *value = parse_assignment(parser);
        return ast_assign(parser->arena, current_loc(parser), expr, value, false, OP_ADD);
    }
    if (match(parser, TOKEN_PLUS_EQUAL)) {
        AstNode *value = parse_assignment(parser);
        return ast_assign(parser->arena, current_loc(parser), expr, value, true, OP_ADD);
    }
    if (match(parser, TOKEN_MINUS_EQUAL)) {
        AstNode *value = parse_assignment(parser);
        return ast_assign(parser->arena, current_loc(parser), expr, value, true, OP_SUB);
    }
    if (match(parser, TOKEN_STAR_EQUAL)) {
        AstNode *value = parse_assignment(parser);
        return ast_assign(parser->arena, current_loc(parser), expr, value, true, OP_MUL);
    }
    if (match(parser, TOKEN_SLASH_EQUAL)) {
        AstNode *value = parse_assignment(parser);
        return ast_assign(parser->arena, current_loc(parser), expr, value, true, OP_DIV);
    }
    if (match(parser, TOKEN_LEFT_ARROW)) {
        /* Channel send: ch <- value */
        AstNode *value = parse_assignment(parser);
        return ast_chan_send(parser->arena, current_loc(parser), expr, value);
    }

    return expr;
}

static AstNode *parse_or(Parser *parser) {
    AstNode *expr = parse_and(parser);

    while (match(parser, TOKEN_PIPE_PIPE)) {
        AstNode *right = parse_and(parser);
        expr = ast_binary(parser->arena, current_loc(parser), OP_OR, expr, right);
    }

    return expr;
}

static AstNode *parse_and(Parser *parser) {
    AstNode *expr = parse_equality(parser);

    while (match(parser, TOKEN_AMPERSAND_AMPERSAND)) {
        AstNode *right = parse_equality(parser);
        expr = ast_binary(parser->arena, current_loc(parser), OP_AND, expr, right);
    }

    return expr;
}

static AstNode *parse_equality(Parser *parser) {
    AstNode *expr = parse_comparison(parser);

    while (true) {
        if (match(parser, TOKEN_EQUAL_EQUAL)) {
            AstNode *right = parse_comparison(parser);
            expr = ast_binary(parser->arena, current_loc(parser), OP_EQ, expr, right);
        } else if (match(parser, TOKEN_BANG_EQUAL)) {
            AstNode *right = parse_comparison(parser);
            expr = ast_binary(parser->arena, current_loc(parser), OP_NE, expr, right);
        } else {
            break;
        }
    }

    return expr;
}

static AstNode *parse_comparison(Parser *parser) {
    AstNode *expr = parse_bit_or(parser);

    while (true) {
        if (match(parser, TOKEN_LESS)) {
            AstNode *right = parse_bit_or(parser);
            expr = ast_binary(parser->arena, current_loc(parser), OP_LT, expr, right);
        } else if (match(parser, TOKEN_GREATER)) {
            AstNode *right = parse_bit_or(parser);
            expr = ast_binary(parser->arena, current_loc(parser), OP_GT, expr, right);
        } else if (match(parser, TOKEN_LESS_EQUAL)) {
            AstNode *right = parse_bit_or(parser);
            expr = ast_binary(parser->arena, current_loc(parser), OP_LE, expr, right);
        } else if (match(parser, TOKEN_GREATER_EQUAL)) {
            AstNode *right = parse_bit_or(parser);
            expr = ast_binary(parser->arena, current_loc(parser), OP_GE, expr, right);
        } else {
            break;
        }
    }

    return expr;
}

static AstNode *parse_bit_or(Parser *parser) {
    AstNode *expr = parse_bit_xor(parser);

    while (match(parser, TOKEN_PIPE)) {
        AstNode *right = parse_bit_xor(parser);
        expr = ast_binary(parser->arena, current_loc(parser), OP_BIT_OR, expr, right);
    }

    return expr;
}

static AstNode *parse_bit_xor(Parser *parser) {
    AstNode *expr = parse_bit_and(parser);

    while (match(parser, TOKEN_CARET)) {
        AstNode *right = parse_bit_and(parser);
        expr = ast_binary(parser->arena, current_loc(parser), OP_BIT_XOR, expr, right);
    }

    return expr;
}

static AstNode *parse_bit_and(Parser *parser) {
    AstNode *expr = parse_shift(parser);

    while (match(parser, TOKEN_AMPERSAND)) {
        AstNode *right = parse_shift(parser);
        expr = ast_binary(parser->arena, current_loc(parser), OP_BIT_AND, expr, right);
    }

    return expr;
}

static AstNode *parse_shift(Parser *parser) {
    AstNode *expr = parse_term(parser);

    /* Simulate shift operators, which we don't have as tokens yet */
    /* We could add them later. For now, skip. */

    return expr;
}

static AstNode *parse_term(Parser *parser) {
    AstNode *expr = parse_factor(parser);

    while (true) {
        if (match(parser, TOKEN_PLUS)) {
            AstNode *right = parse_factor(parser);
            expr = ast_binary(parser->arena, current_loc(parser), OP_ADD, expr, right);
        } else if (match(parser, TOKEN_MINUS)) {
            AstNode *right = parse_factor(parser);
            expr = ast_binary(parser->arena, current_loc(parser), OP_SUB, expr, right);
        } else {
            break;
        }
    }

    return expr;
}

static AstNode *parse_factor(Parser *parser) {
    AstNode *expr = parse_unary(parser);

    while (true) {
        if (match(parser, TOKEN_STAR)) {
            AstNode *right = parse_unary(parser);
            expr = ast_binary(parser->arena, current_loc(parser), OP_MUL, expr, right);
        } else if (match(parser, TOKEN_SLASH)) {
            AstNode *right = parse_unary(parser);
            expr = ast_binary(parser->arena, current_loc(parser), OP_DIV, expr, right);
        } else if (match(parser, TOKEN_PERCENT)) {
            AstNode *right = parse_unary(parser);
            expr = ast_binary(parser->arena, current_loc(parser), OP_MOD, expr, right);
        } else {
            break;
        }
    }

    return expr;
}

static AstNode *parse_unary(Parser *parser) {
    if (match(parser, TOKEN_MINUS)) {
        AstNode *operand = parse_unary(parser);
        return ast_unary(parser->arena, current_loc(parser), OP_NEG, operand);
    }
    if (match(parser, TOKEN_BANG)) {
        AstNode *operand = parse_unary(parser);
        return ast_unary(parser->arena, current_loc(parser), OP_NOT, operand);
    }
    if (match(parser, TOKEN_TILDE)) {
        AstNode *operand = parse_unary(parser);
        return ast_unary(parser->arena, current_loc(parser), OP_BIT_NOT, operand);
    }
    if (match(parser, TOKEN_AWAIT)) {
        AstNode *operand = parse_unary(parser);
        return ast_await(parser->arena, current_loc(parser), operand);
    }
    if (match(parser, TOKEN_LEFT_ARROW)) {
        /* Channel receive: <- ch */
        AstNode *chan = parse_unary(parser);
        return ast_chan_receive(parser->arena, current_loc(parser), chan);
    }

    return parse_call(parser);
}

static AstNode *finish_call(Parser *parser, AstNode *callee) {
    AstNode *args[256];
    char *arg_names[256];
    int arg_count = 0;
    bool any_named = false;

    if (!check(parser, TOKEN_RPAREN)) {
        do {
            if (arg_count >= 256) {
                parser_error(parser, "Too many arguments");
                break;
            }
            AstNode *e = parse_expr(parser);
            char *pname = NULL;
            /* Named argument: IDENTIFIER ':' expr -- a bare identifier is
             * never otherwise followed directly by ':' in expression
             * position, so this is unambiguous without extra lookahead. */
            if (e->kind == NODE_IDENTIFIER && check(parser, TOKEN_COLON)) {
                advance(parser);
                pname = e->identifier.name;
                e = parse_expr(parser);
                any_named = true;
            }
            args[arg_count] = e;
            arg_names[arg_count] = pname;
            arg_count++;
        } while (match(parser, TOKEN_COMMA));
    }

    consume(parser, TOKEN_RPAREN, "Expected ')' after arguments");

    if (!any_named)
        return ast_call(parser->arena, current_loc(parser), callee, args, arg_count);

    /* Named arguments only work calling a plain `fn` by name -- impl
     * methods/lambdas go through NODE_DISPATCH_CALL, a separate path. */
    if (callee->kind != NODE_IDENTIFIER) {
        parser_error(parser, "Named arguments are only supported when calling a function by name");
        return ast_call(parser->arena, current_loc(parser), callee, args, arg_count);
    }
    FunctionSig *sig = parser_find_function(parser, callee->identifier.name);
    if (!sig) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Unknown function '%s' for named arguments", callee->identifier.name);
        parser_error(parser, msg);
        return ast_call(parser->arena, current_loc(parser), callee, args, arg_count);
    }

    AstNode *ordered[64];
    for (int i = 0; i < sig->param_count; i++) ordered[i] = NULL;
    for (int i = 0; i < arg_count; i++) {
        if (!arg_names[i]) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Cannot mix positional and named arguments in call to '%s'",
                     callee->identifier.name);
            parser_error(parser, msg);
            return ast_call(parser->arena, current_loc(parser), callee, args, arg_count);
        }
        int found = -1;
        for (int j = 0; j < sig->param_count; j++) {
            if (strcmp(sig->param_names[j], arg_names[i]) == 0) { found = j; break; }
        }
        if (found < 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Unknown parameter '%s' for function '%s'",
                     arg_names[i], callee->identifier.name);
            parser_error(parser, msg);
            return ast_call(parser->arena, current_loc(parser), callee, args, arg_count);
        }
        ordered[found] = args[i];
    }
    for (int i = 0; i < sig->param_count; i++) {
        if (!ordered[i]) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Missing argument for parameter '%s' in call to '%s'",
                     sig->param_names[i], callee->identifier.name);
            parser_error(parser, msg);
            return ast_call(parser->arena, current_loc(parser), callee, args, arg_count);
        }
    }
    return ast_call(parser->arena, current_loc(parser), callee, ordered, sig->param_count);
}

static AstNode *parse_call(Parser *parser) {
    AstNode *expr = parse_primary(parser);

    /* Check for struct literal: Identifier { ... } */
    if (expr->kind == NODE_IDENTIFIER && check(parser, TOKEN_LBRACE)) {
        StructDef *sd = parser_find_struct(parser, expr->identifier.name);
        if (sd) {
            return parse_struct_literal(parser, expr->identifier.name, expr->loc);
        }
    }

    /* Check for :: — enum literal, static method, or namespace access */
    if (expr->kind == NODE_IDENTIFIER && check(parser, TOKEN_DOUBLE_COLON)) {
        EnumDef *ed = parser_find_enum(parser, expr->identifier.name);
        if (ed) {
            return parse_enum_literal(parser, expr->identifier.name, expr->loc);
        }
        /* Struct static method: Type::method(args) — consume :: and call the method directly */
        advance(parser);
        if (parser->current.type == TOKEN_IDENTIFIER) {
            advance(parser);
            char *member = token_str(parser, &parser->previous);
            if (check(parser, TOKEN_LPAREN) && parser_is_method(parser, member)) {
                expr = ast_identifier(parser->arena, current_loc(parser), member);
            } else {
                expr = ast_member(parser->arena, current_loc(parser), expr, member);
            }
            /* The next iteration of the while loop will handle ( or . */
        }
    }

    while (true) {
        if (match(parser, TOKEN_LPAREN)) {
            expr = finish_call(parser, expr);
        } else if (match(parser, TOKEN_LBRACKET)) {
            /* Index access */
            AstNode *index = parse_expr(parser);
            consume(parser, TOKEN_RBRACKET, "Expected ']' after index");
            expr = ast_index(parser->arena, current_loc(parser), expr, index);
        } else if (match(parser, TOKEN_DOT)) {
            /* Member access or method call. After '.' a keyword is
             * unambiguous as a name, so accept identifiers and any
             * keyword-like token (alphabetic/underscore start) -- this is
             * what lets methods be named `match`, `test`, `type`, etc. */
            char dot_first = parser->current.start[0];
            bool dot_name_like = parser->current.type == TOKEN_IDENTIFIER ||
                (dot_first >= 'a' && dot_first <= 'z') ||
                (dot_first >= 'A' && dot_first <= 'Z') || dot_first == '_';
            if (!dot_name_like) {
                parser_error(parser, "Expected member name after '.'");
                break;
            }
            advance(parser);
            char *member = token_str(parser, &parser->previous);

            /* Check for method call: .method(args) */
            if (check(parser, TOKEN_LPAREN) && parser_is_method(parser, member)) {
                advance(parser);
                AstNode *args[256];
                int arg_count = 0;
                if (!check(parser, TOKEN_RPAREN)) {
                    do {
                        if (arg_count >= 256) break;
                        args[arg_count++] = parse_expr(parser);
                    } while (match(parser, TOKEN_COMMA));
                }
                consume(parser, TOKEN_RPAREN, "Expected ')' after arguments");
                expr = ast_dispatch_call(parser->arena, current_loc(parser),
                                          expr, member, args, arg_count);
            } else {
                /* Field access */
                expr = ast_member(parser->arena, current_loc(parser), expr, member);
            }
        } else if (match(parser, TOKEN_QUESTION_DOT)) {
            /* Safe navigation: expr?.member (keywords allowed as names too) */
            char qd_first = parser->current.start[0];
            bool qd_name_like = parser->current.type == TOKEN_IDENTIFIER ||
                (qd_first >= 'a' && qd_first <= 'z') ||
                (qd_first >= 'A' && qd_first <= 'Z') || qd_first == '_';
            if (!qd_name_like) {
                parser_error(parser, "Expected member name after '?.'");
                break;
            }
            advance(parser);
            char *member = token_str(parser, &parser->previous);
            expr = ast_question_dot(parser->arena, current_loc(parser), expr, member);
        } else if (match(parser, TOKEN_QUESTION)) {
            /* Error propagation: expr? */
            expr = ast_propagate(parser->arena, current_loc(parser), expr);
        } else {
            break;
        }
    }

    return expr;
}

static AstNode *parse_primary(Parser *parser) {
    SourceLoc loc = current_loc(parser);

    if (match(parser, TOKEN_FALSE)) {
        return ast_bool_literal(parser->arena, loc, false);
    }
    if (match(parser, TOKEN_TRUE)) {
        return ast_bool_literal(parser->arena, loc, true);
    }
    if (match(parser, TOKEN_NULL)) {
        return ast_null_literal(parser->arena, loc);
    }

    if (match(parser, TOKEN_INTEGER)) {
        int64_t value = strtoll(parser->previous.start, NULL, 10);
        return ast_int_literal(parser->arena, loc, value);
    }

    if (match(parser, TOKEN_FLOAT)) {
        double value = strtod(parser->previous.start, NULL);
        return ast_float_literal(parser->arena, loc, value);
    }

    if (match(parser, TOKEN_STRING)) {
        return ast_string_literal(parser->arena, loc, parser->previous.value);
    }

    if (match(parser, TOKEN_BYTE_SLICE)) {
        /* Extract length from packed prefix */
        char *val = parser->previous.value;
        if (val) {
            int len = (unsigned char)val[0] | ((unsigned char)val[1] << 8) |
                      ((unsigned char)val[2] << 16) | ((unsigned char)val[3] << 24);
            char *content = (char *)arena_alloc(parser->arena, len + 1);
            memcpy(content, val + 4, len);
            content[len] = '\0';
            return ast_string_literal(parser->arena, loc, content);
        }
        return ast_string_literal(parser->arena, loc, "");
    }

    if (match(parser, TOKEN_INTERPOLATED_STRING)) {
        /* Parse interpolated string: split on \1/\2 markers */
        AstNode *node = ast_interpolated_string(parser->arena, loc);
        const char *str = parser->previous.value;
        if (str) {
            AstNode *parts[64];
            int part_count = 0;
            const char *p = str;

            while (*p && part_count < 64) {
                if (*p == '\1') {
                    /* Start of embedded expression */
                    p++;
                    const char *expr_start = p;
                    while (*p && *p != '\2') p++;
                    if (*p == '\2') {
                        int expr_len = (int)(p - expr_start);
                        if (expr_len > 0) {
                            char *expr_src = (char *)arena_alloc(parser->arena, expr_len + 1);
                            memcpy(expr_src, expr_start, expr_len);
                            expr_src[expr_len] = '\0';
                            Lexer expr_lexer;
                            lexer_init(&expr_lexer, expr_src, "<interp>");
                            Parser expr_parser;
                            parser_init(&expr_parser, &expr_lexer, parser->arena);
                            AstNode *expr = parser_parse_expr(&expr_parser);
                            if (expr) parts[part_count++] = expr;
                        }
                        p++; /* skip \2 */
                    }
                } else {
                    /* Plain string segment */
                    const char *seg_start = p;
                    while (*p && *p != '\1') p++;
                    int seg_len = (int)(p - seg_start);
                    if (seg_len > 0) {
                        char *seg = (char *)arena_alloc(parser->arena, seg_len + 1);
                        memcpy(seg, seg_start, seg_len);
                        seg[seg_len] = '\0';
                        parts[part_count++] = ast_string_literal(parser->arena, loc, seg);
                    }
                }
            }

            if (part_count > 0) {
                node->interpolated_string.parts = (AstNode **)arena_alloc(parser->arena, sizeof(AstNode *) * part_count);
                memcpy(node->interpolated_string.parts, parts, sizeof(AstNode *) * part_count);
                node->interpolated_string.part_count = part_count;
            }
        }
        return node;
    }

    if (match(parser, TOKEN_REGEX)) {
        /* Store regex as string literal for now */
        return ast_string_literal(parser->arena, loc, parser->previous.value);
    }

    if (match(parser, TOKEN_COMPTIME)) {
        return parse_comptime(parser);
    }

    if (match(parser, TOKEN_IDENTIFIER)) {
        return ast_identifier(parser->arena, loc, token_str(parser, &parser->previous));
    }

    /* Match expression */
    if (match(parser, TOKEN_MATCH)) {
        AstNode *value = parse_expr(parser);
        AstNode *match_node = ast_match(parser->arena, loc, value);
        consume(parser, TOKEN_LBRACE, "Expected '{' after match expression");

        AstNode *arms[256];
        int arm_count = 0;
        while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
            if (arm_count >= 256) break;
            AstNode *pattern = parse_expr(parser);

            /* Detect enum destructuring pattern: checks if pattern is an enum literal
             * where payload positions are identifiers (variable bindings) */
            char *bind_names[8];
            int bind_count = 0;
            if (pattern->kind == NODE_ENUM_LITERAL && pattern->enum_literal.value_count > 0) {
                bool all_identifiers = true;
                for (int i = 0; i < pattern->enum_literal.value_count; i++) {
                    if (pattern->enum_literal.values[i]->kind == NODE_IDENTIFIER) {
                        bind_names[bind_count++] = pattern->enum_literal.values[i]->identifier.name;
                    } else {
                        all_identifiers = false;
                        break;
                    }
                }
                if (!all_identifiers)
                    bind_count = 0;
            }

            consume(parser, TOKEN_FAT_ARROW, "Expected '=>' after match pattern");
            AstNode *body = parse_expr(parser);
            if (match(parser, TOKEN_COMMA)) { /* optional comma */ }
            arms[arm_count++] = ast_match_arm(parser->arena, loc, pattern, body, bind_names, bind_count);
        }
        consume(parser, TOKEN_RBRACE, "Expected '}' after match arms");
        match_node->match_stmt.arms = (AstNode **)arena_alloc(parser->arena, sizeof(AstNode *) * arm_count);
        memcpy(match_node->match_stmt.arms, arms, sizeof(AstNode *) * arm_count);
        match_node->match_stmt.arm_count = arm_count;
        return match_node;
    }

    /* Grouping or tuple */
    if (match(parser, TOKEN_LPAREN)) {
        if (check(parser, TOKEN_RPAREN)) {
            /* Empty tuple () */
            advance(parser);
            return ast_tuple_literal(parser->arena, loc);
        }

        AstNode *first = parse_expr(parser);

        if (match(parser, TOKEN_COMMA)) {
            /* Tuple */
            AstNode *elems[256];
            int count = 0;
            elems[count++] = first;
            do {
                if (count >= 256) break;
                elems[count++] = parse_expr(parser);
            } while (match(parser, TOKEN_COMMA));
            consume(parser, TOKEN_RPAREN, "Expected ')' after tuple");
            AstNode *tuple = ast_tuple_literal(parser->arena, loc);
            tuple->tuple_literal.elements = (AstNode **)arena_alloc(parser->arena, sizeof(AstNode *) * count);
            memcpy(tuple->tuple_literal.elements, elems, sizeof(AstNode *) * count);
            tuple->tuple_literal.element_count = count;
            return tuple;
        }

        consume(parser, TOKEN_RPAREN, "Expected ')' after expression");
        return first;
    }

    /* Array literal */
    if (match(parser, TOKEN_LBRACKET)) {
        AstNode *elems[256];
        int count = 0;
        while (!check(parser, TOKEN_RBRACKET) && !check(parser, TOKEN_EOF)) {
            if (count >= 256) break;
            elems[count++] = parse_expr(parser);
            if (!match(parser, TOKEN_COMMA)) break;
        }
        consume(parser, TOKEN_RBRACKET, "Expected ']' after array literal");
        AstNode *arr = ast_array_literal(parser->arena, loc);
        arr->array_literal.elements = (AstNode **)arena_alloc(parser->arena, sizeof(AstNode *) * count);
        memcpy(arr->array_literal.elements, elems, sizeof(AstNode *) * count);
        arr->array_literal.element_count = count;
        return arr;
    }

    /* Object literal:  { name: "Ada", "age": 30, active: true }
     *
     * A brace in *expression* position (after `=`, `return`, a call argument,
     * etc.) builds an anonymous object. Statement-position braces are already
     * taken as blocks by parse_statement before we ever get here, so there is
     * no ambiguity. Keys may be written as a bare identifier (name) or as a
     * string literal ("age") -- the string form is what lets keys contain
     * characters an identifier can't. Each value is any expression.
     *
     * It desugars to the same anonymous struct the rest of the language already
     * uses: type name "Object", these field names, these values. That means an
     * object literal is a normal value everywhere -- json_encode() serializes
     * it, `obj.name` reads a field, you can return it, nest it, pass it around --
     * with no separate "map" type or special rules to learn. */
    if (match(parser, TOKEN_LBRACE)) {
        char *field_names[64];
        AstNode *field_values[64];
        int field_count = 0;

        while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
            if (field_count >= 64) {
                parser_error(parser, "Object literal has too many fields (max 64)");
                break;
            }

            char *key;
            if (parser->current.type == TOKEN_STRING) {
                advance(parser);
                key = strdup(parser->previous.value); /* "age" -> age */
            } else if (parser->current.type == TOKEN_IDENTIFIER) {
                advance(parser);
                key = token_strdup(&parser->previous); /* name -> name */
            } else {
                parser_error(parser, "Expected a field name (identifier or \"string\") in object literal");
                break;
            }

            consume(parser, TOKEN_COLON, "Expected ':' after object field name");
            field_names[field_count] = key;
            field_values[field_count] = parse_expr(parser);
            field_count++;

            if (!match(parser, TOKEN_COMMA))
                break;
        }
        consume(parser, TOKEN_RBRACE, "Expected '}' to close object literal");

        AstNode *obj = ast_struct_literal(parser->arena, loc, "Object",
                                          field_names, field_values, field_count);
        for (int i = 0; i < field_count; i++)
            free(field_names[i]);
        return obj;
    }

    /* Lambda expression */
    if (match(parser, TOKEN_PIPE)) {
        /* |params| expr - closure */
        char *param_names[64];
        int param_count = 0;
        if (!check(parser, TOKEN_PIPE)) {
            do {
                if (param_count >= 64) break;
                if (parser->current.type != TOKEN_IDENTIFIER) break;
                advance(parser);
                param_names[param_count++] = token_strdup(&parser->previous);
            } while (match(parser, TOKEN_COMMA));
        }
        consume(parser, TOKEN_PIPE, "Expected '|' after lambda parameters");

        AstNode *body;
        if (match(parser, TOKEN_FAT_ARROW)) {
            body = parse_expr(parser);
        } else {
            body = parse_block(parser);
        }

        /* Lambda is sugar for: fn(params) { body } */
        AstNode *block = ast_block(parser->arena, loc);
        if (body->kind == NODE_BLOCK) {
            block->block.stmt_count = 1;
            block->block.stmts = (AstNode **)arena_alloc(parser->arena, sizeof(AstNode *));
            block->block.stmts[0] = body;
        } else {
            /* Arrow expression: wrap in return */
            block->block.stmt_count = 1;
            block->block.stmts = (AstNode **)arena_alloc(parser->arena, sizeof(AstNode *));
            block->block.stmts[0] = ast_return_one(parser->arena, loc, body);
        }

        Type *void_type = type_primitive(parser->arena, PRIMITIVE_VOID);
        Type **param_type_list = (Type **)arena_alloc(parser->arena, sizeof(Type *) * param_count);
        for (int i = 0; i < param_count; i++)
            param_type_list[i] = type_primitive(parser->arena, PRIMITIVE_INT);
        Type *fn_type = type_function(parser->arena, param_type_list, param_count, void_type);

        AstNode *fn = ast_fn_decl(parser->arena, loc, "__lambda__", fn_type,
                                  param_names, param_count, NULL, 0,
                                  block, false, false, false, NULL,
                                  NULL, NULL, 0);
        for (int i = 0; i < param_count; i++)
            free(param_names[i]);
        return fn;
    }

    /* Error fallback */
    Token err_token = parser->current;
    parser_error_at(parser, &err_token, "Expected expression");
    return ast_null_literal(parser->arena, loc);
}

/* ─── Top-Level Parsing ─── */
void parser_init(Parser *parser, Lexer *lexer, Arena *arena) {
    parser->lexer = lexer;
    parser->arena = arena;
    parser->had_error = false;
    parser->error_message[0] = '\0';
    parser->loop_depth = 0;
    parser->struct_count = 0;
    parser->enum_count = 0;
    parser->method_count = 0;
    parser->function_count = 0;

    /* Pre-register built-in method names so parser emits BC_DISPATCH for them */
    {
        const char *builtin_methods[] = {
            "len", "upper", "lower", "substring", "trim", "spawn",
            "serve", "serve_with_routes", "push", "split", "starts_with", "replace",
            "write_socket", "close_socket", "read_socket", "code_at", "from_codes",
            "sha1_base64", "hash_password", "verify_password",
            "bit_and", "bit_or", "bit_xor", "index_of", "contains",
            "read", "write", "close"
        };
        int n = sizeof(builtin_methods) / sizeof(builtin_methods[0]);
        for (int i = 0; i < n && i < 256; i++) {
            parser->method_names[i] = (char *)builtin_methods[i];
            parser->method_count++;
        }
    }

    /* Prime the first token */
    advance(parser);
}

extern char *parser_read_file(Parser *parser, const char *path);

AstNode *parser_parse(Parser *parser) {
    AstNode *program = ast_program(parser->arena, current_loc(parser));

    AstNode **stmts = NULL;
    int stmt_count = 0;
    int stmt_capacity = 0;

    while (!check(parser, TOKEN_EOF) && !parser->had_error) {
        if (match(parser, TOKEN_USE)) {
            consume(parser, TOKEN_STRING, "Expected file path after 'use'");
            char *path = token_str(parser, &parser->previous);
            int len = strlen(path);
            char *clean_path = (char *)arena_alloc(parser->arena, len);
            strncpy(clean_path, path + 1, len - 2);
            clean_path[len - 2] = '\0';

            char *content = parser_read_file(parser, clean_path);
            if (!content) {
                char err_buf[512];
                snprintf(err_buf, sizeof(err_buf), "Could not read included file: %s", clean_path);
                parser_error(parser, err_buf);
            } else {
                Lexer sub_lexer;
                lexer_init(&sub_lexer, content, clean_path);

                Parser sub_parser;
                parser_init(&sub_parser, &sub_lexer, parser->arena);
                sub_parser.struct_count = parser->struct_count;
                for(int i=0; i<parser->struct_count; i++) sub_parser.structs[i] = parser->structs[i];
                sub_parser.enum_count = parser->enum_count;
                for(int i=0; i<parser->enum_count; i++) sub_parser.enums[i] = parser->enums[i];
                sub_parser.function_count = parser->function_count;
                for(int i=0; i<parser->function_count; i++) sub_parser.functions[i] = parser->functions[i];
                sub_parser.method_count = parser->method_count;
                for(int i=0; i<parser->method_count; i++) sub_parser.method_names[i] = parser->method_names[i];

                AstNode *sub_prog = parser_parse(&sub_parser);

                parser->struct_count = sub_parser.struct_count;
                for(int i=0; i<parser->struct_count; i++) parser->structs[i] = sub_parser.structs[i];
                parser->enum_count = sub_parser.enum_count;
                for(int i=0; i<parser->enum_count; i++) parser->enums[i] = sub_parser.enums[i];
                parser->function_count = sub_parser.function_count;
                for(int i=0; i<parser->function_count; i++) parser->functions[i] = sub_parser.functions[i];
                parser->method_count = sub_parser.method_count;
                for(int i=0; i<parser->method_count; i++) parser->method_names[i] = sub_parser.method_names[i];

                if (sub_prog && !sub_parser.had_error) {
                    for (int i = 0; i < sub_prog->program.stmt_count; i++) {
                        if (stmt_count >= stmt_capacity) {
                            stmt_capacity = stmt_capacity < 8 ? 8 : stmt_capacity * 2;
                            stmts = (AstNode **)realloc(stmts, sizeof(AstNode *) * stmt_capacity);
                        }
                        stmts[stmt_count++] = sub_prog->program.stmts[i];
                    }
                } else {
                    parser->had_error = true;
                    strcpy(parser->error_message, sub_parser.error_message);
                }
            }
            continue;
        }

        AstNode *stmt = parse_stmt(parser);
        if (stmt) {
            if (stmt_count >= stmt_capacity) {
                stmt_capacity = stmt_capacity < 8 ? 8 : stmt_capacity * 2;
                stmts = (AstNode **)realloc(stmts, sizeof(AstNode *) * stmt_capacity);
            }
            stmts[stmt_count++] = stmt;
        }
    }

    if (parser->had_error) {
        if (stmts) free(stmts);
        return NULL;
    }

    program->program.stmts = (AstNode **)arena_alloc(parser->arena, sizeof(AstNode *) * stmt_count);
    if (stmts) {
        memcpy(program->program.stmts, stmts, sizeof(AstNode *) * stmt_count);
        free(stmts);
    }
    program->program.stmt_count = stmt_count;
    program->program.stmt_capacity = stmt_count;
    return program;
}

AstNode *parser_parse_expr(Parser *parser) {
    return parse_expr(parser);
}

const char *parser_get_error(Parser *parser) {
    return parser->had_error ? parser->error_message : NULL;
}
