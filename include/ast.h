#ifndef AST_H
#define AST_H

#include "varian.h"
#include "lexer.h"
#include <stdint.h>
#include <stdbool.h>

/* ─── Type Representations ─── */
typedef enum {
    TYPE_PRIMITIVE,
    TYPE_FUNCTION,
    TYPE_NAMED,
    TYPE_ARRAY,
    TYPE_TUPLE,
} TypeKind;

typedef enum {
    PRIMITIVE_BOOL,
    PRIMITIVE_INT,
    PRIMITIVE_FLOAT,
    PRIMITIVE_STRING,
    PRIMITIVE_BYTE,
    PRIMITIVE_VOID,
} PrimitiveKind;

typedef struct Type {
    TypeKind kind;
    union {
        PrimitiveKind primitive;
        struct {
            struct Type **param_types;
            int param_count;
            struct Type *return_type;
        } function;
        struct {
            char *name;
        } named;
        struct {
            struct Type *element_type;
        } array;
        struct {
            struct Type **types;
            int count;
        } tuple;
    };
} Type;

/* ─── AST Node Types ─── */
typedef enum {
    NODE_PROGRAM,

    /* Declarations */
    NODE_LET_DECL,
    NODE_CONST_DECL,
    NODE_FN_DECL,

    /* Statements */
    NODE_BLOCK,
    NODE_EXPR_STMT,
    NODE_IF,
    NODE_WHILE,
    NODE_FOR,
    NODE_LOOP,
    NODE_RETURN,
    NODE_ASSIGN,

    /* Expressions */
    NODE_BINARY,
    NODE_UNARY,
    NODE_CALL,
    NODE_INDEX,
    NODE_MEMBER,

    /* Literals & References */
    NODE_INT_LITERAL,
    NODE_FLOAT_LITERAL,
    NODE_STRING_LITERAL,
    NODE_BOOL_LITERAL,
    NODE_NULL_LITERAL,
    NODE_IDENTIFIER,
    NODE_INTERPOLATED_STRING,
    NODE_ARRAY_LITERAL,
    NODE_TUPLE_LITERAL,

    /* Loop control */
    NODE_BREAK,
    NODE_CONTINUE,

    /* Structs */
    NODE_STRUCT_DECL,
    NODE_STRUCT_LITERAL,

    /* Enums */
    NODE_ENUM_DECL,
    NODE_ENUM_LITERAL,

    /* Pattern matching */
    NODE_MATCH,
    NODE_MATCH_ARM,

    /* Error handling */
    NODE_PROPAGATE,
    NODE_TRY,
} NodeKind;

/* Forward declaration */
typedef struct AstNode AstNode;

/* Binary operators */
typedef enum {
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE,
    OP_AND, OP_OR,
    OP_BIT_AND, OP_BIT_OR, OP_BIT_XOR, OP_SHL, OP_SHR,
} BinaryOp;

/* Unary operators */
typedef enum {
    OP_NEG, OP_NOT, OP_BIT_NOT,
} UnaryOp;

/* ─── AST Node ─── */
struct AstNode {
    NodeKind kind;
    SourceLoc loc;
    Type *type;  /* resolved type (set during semantic analysis) */

    union {
        /* Program */
        struct {
            AstNode **stmts;
            int stmt_count;
            int stmt_capacity;
        } program;

        /* Let/Const declaration */
        struct {
            char **names;
            int name_count;
            AstNode *initializer;
            bool is_mutable;
        } let_decl;

        /* Function declaration */
        struct {
            char *name;
            Type *fn_type;
            char **param_names;
            int param_count;
            AstNode *body;
            bool is_pub;
            bool is_async;
        } fn_decl;

        /* Block */
        struct {
            AstNode **stmts;
            int stmt_count;
            int stmt_capacity;
        } block;

        /* Expression statement */
        struct {
            AstNode *expr;
        } expr_stmt;

        /* If/Else */
        struct {
            AstNode *condition;
            AstNode *then_branch;
            AstNode *else_branch;  /* may be NULL */
        } if_stmt;

        /* While */
        struct {
            AstNode *condition;
            AstNode *body;
        } while_stmt;

        /* For */
        struct {
            char *var_name;
            AstNode *iterable;
            AstNode *body;
        } for_stmt;

        /* Loop (infinite) */
        struct {
            AstNode *body;
        } loop_stmt;

        /* Return */
        struct {
            AstNode **values;
            int value_count;
        } return_stmt;

        /* Assignment */
        struct {
            AstNode *target;
            AstNode *value;
            BinaryOp compound_op;  /* OP_ADD for +=, etc. - OP_ADD means simple = with extra tracking */
            bool is_compound;
        } assign;

        /* Binary expression */
        struct {
            BinaryOp op;
            AstNode *left;
            AstNode *right;
        } binary;

        /* Unary expression */
        struct {
            UnaryOp op;
            AstNode *operand;
        } unary;

        /* Call */
        struct {
            AstNode *callee;
            AstNode **args;
            int arg_count;
        } call;

        /* Index */
        struct {
            AstNode *object;
            AstNode *index;
        } index;

        /* Member access */
        struct {
            AstNode *object;
            char *member;
        } member;

        /* Literals */
        struct {
            union {
                int64_t int_value;
                double float_value;
                char *string_value;
                bool bool_value;
            };
        } literal;

        /* Interpolated string - list of parts */
        struct {
            AstNode **parts;
            int part_count;
        } interpolated_string;

        /* Array literal */
        struct {
            AstNode **elements;
            int element_count;
        } array_literal;

        /* Tuple literal */
        struct {
            AstNode **elements;
            int element_count;
        } tuple_literal;

        /* Identifier */
        struct {
            char *name;
            /* resolved at semantic analysis */
            int depth;    /* scope depth for local variables */
            int index;    /* index in scope */
        } identifier;

        /* Match */
        struct {
            AstNode *value;
            AstNode **arms;
            int arm_count;
        } match_stmt;

        /* Struct declaration */
        struct {
            char *name;
            char **field_names;
            int field_count;
        } struct_decl;

        /* Struct literal */
        struct {
            char *name;  /* struct type name */
            char **field_names;
            AstNode **field_values;
            int field_count;
        } struct_literal;

        /* Enum declaration */
        struct {
            char *name;
            char **variant_names;
            int *variant_counts;  /* number of associated values per variant */
            int variant_count;
        } enum_decl;

        /* Enum literal */
        struct {
            char *enum_name;
            char *variant_name;
            int tag;
            AstNode **values;
            int value_count;
        } enum_literal;

        /* Propagate (expr?) */
        struct {
            AstNode *expr;
        } propagate;

        /* Try/Catch */
        struct {
            AstNode *try_body;
            AstNode *catch_body;
            char *catch_var;  /* may be NULL */
        } try_stmt;

        /* Match arm */
        struct {
            AstNode *pattern;  /* expression pattern for now */
            AstNode *body;
        } match_arm;
    };
};

/* ─── AST Construction Helpers ─── */
AstNode *ast_program(Arena *arena, SourceLoc loc);
void ast_program_add_stmt(AstNode *program, AstNode *stmt);

AstNode *ast_let_decl(Arena *arena, SourceLoc loc, char **names, int name_count,
                      AstNode *initializer, bool is_mutable);
AstNode *ast_fn_decl(Arena *arena, SourceLoc loc, const char *name,
                     Type *fn_type, char **param_names, int param_count,
                     AstNode *body, bool is_pub, bool is_async);
AstNode *ast_block(Arena *arena, SourceLoc loc);
void ast_block_add_stmt(AstNode *block, AstNode *stmt);
AstNode *ast_expr_stmt(Arena *arena, SourceLoc loc, AstNode *expr);
AstNode *ast_if(Arena *arena, SourceLoc loc, AstNode *cond,
                AstNode *then_branch, AstNode *else_branch);
AstNode *ast_while(Arena *arena, SourceLoc loc, AstNode *cond, AstNode *body);
AstNode *ast_for(Arena *arena, SourceLoc loc, const char *var_name,
                 AstNode *iterable, AstNode *body);
AstNode *ast_loop(Arena *arena, SourceLoc loc, AstNode *body);
AstNode *ast_return(Arena *arena, SourceLoc loc, AstNode **values, int value_count);
AstNode *ast_return_one(Arena *arena, SourceLoc loc, AstNode *value);
AstNode *ast_assign(Arena *arena, SourceLoc loc, AstNode *target,
                    AstNode *value, bool is_compound, BinaryOp compound_op);
AstNode *ast_binary(Arena *arena, SourceLoc loc, BinaryOp op,
                    AstNode *left, AstNode *right);
AstNode *ast_unary(Arena *arena, SourceLoc loc, UnaryOp op, AstNode *operand);
AstNode *ast_call(Arena *arena, SourceLoc loc, AstNode *callee,
                  AstNode **args, int arg_count);
AstNode *ast_index(Arena *arena, SourceLoc loc, AstNode *object, AstNode *index);
AstNode *ast_member(Arena *arena, SourceLoc loc, AstNode *object, const char *member);
AstNode *ast_int_literal(Arena *arena, SourceLoc loc, int64_t value);
AstNode *ast_float_literal(Arena *arena, SourceLoc loc, double value);
AstNode *ast_string_literal(Arena *arena, SourceLoc loc, const char *value);
AstNode *ast_bool_literal(Arena *arena, SourceLoc loc, bool value);
AstNode *ast_null_literal(Arena *arena, SourceLoc loc);
AstNode *ast_identifier(Arena *arena, SourceLoc loc, const char *name);
AstNode *ast_interpolated_string(Arena *arena, SourceLoc loc);
void ast_interpolated_add_part(AstNode *node, AstNode *part);
AstNode *ast_array_literal(Arena *arena, SourceLoc loc);
void ast_array_add_element(AstNode *node, AstNode *elem);
AstNode *ast_tuple_literal(Arena *arena, SourceLoc loc);
void ast_tuple_add_element(AstNode *node, AstNode *elem);

AstNode *ast_break(Arena *arena, SourceLoc loc);
AstNode *ast_continue(Arena *arena, SourceLoc loc);

AstNode *ast_struct_decl(Arena *arena, SourceLoc loc, const char *name,
                         char **field_names, int field_count);
AstNode *ast_struct_literal(Arena *arena, SourceLoc loc, const char *name,
                            char **field_names, AstNode **field_values, int field_count);

AstNode *ast_enum_decl(Arena *arena, SourceLoc loc, const char *name,
                       char **variant_names, int *variant_counts, int variant_count);
AstNode *ast_enum_literal(Arena *arena, SourceLoc loc, const char *enum_name,
                          const char *variant_name, int tag,
                          AstNode **values, int value_count);

AstNode *ast_match(Arena *arena, SourceLoc loc, AstNode *value);
void ast_match_add_arm(AstNode *match, AstNode *pattern, AstNode *body);
AstNode *ast_match_arm(Arena *arena, SourceLoc loc, AstNode *pattern, AstNode *body);

AstNode *ast_propagate(Arena *arena, SourceLoc loc, AstNode *expr);
AstNode *ast_try(Arena *arena, SourceLoc loc, AstNode *try_body,
                 AstNode *catch_body, const char *catch_var);

/* ─── Type Construction ─── */
Type *type_primitive(Arena *arena, PrimitiveKind pk);
Type *type_function(Arena *arena, Type **param_types, int param_count, Type *return_type);
Type *type_named(Arena *arena, const char *name);
Type *type_array(Arena *arena, Type *element_type);
Type *type_tuple(Arena *arena, Type **types, int count);

/* ─── Debug ─── */
void ast_print(AstNode *node, int indent);

/* ─── Free ─── */
void ast_free(AstNode *node);  /* only for non-arena allocated nodes */

#endif /* AST_H */
