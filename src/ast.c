#include "ast.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define INITIAL_STMT_CAPACITY 64

/* ─── Type Constructors ─── */
Type *type_primitive(Arena *arena, PrimitiveKind pk) {
    Type *t = (Type *)arena_alloc(arena, sizeof(Type));
    t->kind = TYPE_PRIMITIVE;
    t->primitive = pk;
    return t;
}

Type *type_function(Arena *arena, Type **param_types, int param_count, Type *return_type) {
    Type *t = (Type *)arena_alloc(arena, sizeof(Type));
    t->kind = TYPE_FUNCTION;
    t->function.param_count = param_count;
    t->function.param_types = (Type **)arena_alloc(arena, sizeof(Type *) * param_count);
    for (int i = 0; i < param_count; i++)
        t->function.param_types[i] = param_types[i];
    t->function.return_type = return_type;
    return t;
}

Type *type_named(Arena *arena, const char *name) {
    Type *t = (Type *)arena_alloc(arena, sizeof(Type));
    t->kind = TYPE_NAMED;
    t->named.name = (char *)arena_alloc(arena, strlen(name) + 1);
    strcpy(t->named.name, name);
    return t;
}

Type *type_array(Arena *arena, Type *element_type) {
    Type *t = (Type *)arena_alloc(arena, sizeof(Type));
    t->kind = TYPE_ARRAY;
    t->array.element_type = element_type;
    return t;
}

Type *type_tuple(Arena *arena, Type **types, int count) {
    Type *t = (Type *)arena_alloc(arena, sizeof(Type));
    t->kind = TYPE_TUPLE;
    t->tuple.count = count;
    t->tuple.types = (Type **)arena_alloc(arena, sizeof(Type *) * count);
    for (int i = 0; i < count; i++)
        t->tuple.types[i] = types[i];
    return t;
}

/* ─── Node Constructors ─── */
static AstNode *alloc_node(Arena *arena, NodeKind kind, SourceLoc loc) {
    AstNode *node = (AstNode *)arena_alloc(arena, sizeof(AstNode));
    node->kind = kind;
    node->loc = loc;
    node->type = NULL;
    memset(&node->literal, 0, sizeof(node->literal));
    return node;
}

AstNode *ast_program(Arena *arena, SourceLoc loc) {
    AstNode *node = alloc_node(arena, NODE_PROGRAM, loc);
    node->program.stmts = (AstNode **)arena_alloc(arena, sizeof(AstNode *) * INITIAL_STMT_CAPACITY);
    node->program.stmt_count = 0;
    node->program.stmt_capacity = INITIAL_STMT_CAPACITY;
    return node;
}

void ast_program_add_stmt(AstNode *program, AstNode *stmt) {
    if (program->program.stmt_count >= program->program.stmt_capacity) {
        /* We can't grow arena-allocated arrays, so this is a no-op for safety.
         * The parser pre-allocates sufficient capacity via local arrays.
         * If you hit this, increase INITIAL_STMT_CAPACITY. */
        fprintf(stderr, "Warning: program statement capacity exceeded (%d)\n",
                program->program.stmt_capacity);
        return;
    }
    program->program.stmts[program->program.stmt_count++] = stmt;
}

AstNode *ast_let_decl(Arena *arena, SourceLoc loc, char **names, int name_count,
                      AstNode *initializer, bool is_mutable) {
    AstNode *node = alloc_node(arena, is_mutable ? NODE_LET_DECL : NODE_CONST_DECL, loc);
    if (name_count > 0) {
        node->let_decl.names = (char **)arena_alloc(arena, sizeof(char *) * name_count);
        for (int i = 0; i < name_count; i++) {
            node->let_decl.names[i] = (char *)arena_alloc(arena, strlen(names[i]) + 1);
            strcpy(node->let_decl.names[i], names[i]);
        }
    } else {
        node->let_decl.names = NULL;
    }
    node->let_decl.name_count = name_count;
    node->let_decl.initializer = initializer;
    node->let_decl.is_mutable = is_mutable;
    return node;
}

AstNode *ast_fn_decl(Arena *arena, SourceLoc loc, const char *name,
                     Type *fn_type, char **param_names, int param_count,
                     char **type_params, int type_param_count,
                     AstNode *body, bool is_pub, bool is_async,
                     bool is_method, const char *impl_type) {
    AstNode *node = alloc_node(arena, NODE_FN_DECL, loc);
    node->fn_decl.name = (char *)arena_alloc(arena, strlen(name) + 1);
    strcpy(node->fn_decl.name, name);
    node->fn_decl.fn_type = fn_type;
    node->fn_decl.param_count = param_count;
    if (param_count > 0) {
        node->fn_decl.param_names = (char **)arena_alloc(arena, sizeof(char *) * param_count);
        for (int i = 0; i < param_count; i++) {
            node->fn_decl.param_names[i] = (char *)arena_alloc(arena, strlen(param_names[i]) + 1);
            strcpy(node->fn_decl.param_names[i], param_names[i]);
        }
    } else {
        node->fn_decl.param_names = NULL;
    }
    node->fn_decl.type_param_count = type_param_count;
    node->fn_decl.type_params = NULL;
    (void)type_params;
    (void)type_param_count;
    node->fn_decl.body = body;
    node->fn_decl.is_pub = is_pub;
    node->fn_decl.is_async = is_async;
    node->fn_decl.is_method = is_method;
    if (impl_type) {
        node->fn_decl.impl_type = (char *)arena_alloc(arena, strlen(impl_type) + 1);
        strcpy(node->fn_decl.impl_type, impl_type);
    } else {
        node->fn_decl.impl_type = NULL;
    }
    return node;
}

AstNode *ast_block(Arena *arena, SourceLoc loc) {
    AstNode *node = alloc_node(arena, NODE_BLOCK, loc);
    node->block.stmts = (AstNode **)arena_alloc(arena, sizeof(AstNode *) * INITIAL_STMT_CAPACITY);
    node->block.stmt_count = 0;
    node->block.stmt_capacity = INITIAL_STMT_CAPACITY;
    return node;
}

void ast_block_add_stmt(AstNode *block, AstNode *stmt) {
    if (block->block.stmt_count >= block->block.stmt_capacity) {
        fprintf(stderr, "Warning: block statement capacity exceeded (%d)\n",
                block->block.stmt_capacity);
        return;
    }
    block->block.stmts[block->block.stmt_count++] = stmt;
}

AstNode *ast_expr_stmt(Arena *arena, SourceLoc loc, AstNode *expr) {
    AstNode *node = alloc_node(arena, NODE_EXPR_STMT, loc);
    node->expr_stmt.expr = expr;
    return node;
}

AstNode *ast_if(Arena *arena, SourceLoc loc, AstNode *cond,
                AstNode *then_branch, AstNode *else_branch) {
    AstNode *node = alloc_node(arena, NODE_IF, loc);
    node->if_stmt.condition = cond;
    node->if_stmt.then_branch = then_branch;
    node->if_stmt.else_branch = else_branch;
    return node;
}

AstNode *ast_while(Arena *arena, SourceLoc loc, AstNode *cond, AstNode *body) {
    AstNode *node = alloc_node(arena, NODE_WHILE, loc);
    node->while_stmt.condition = cond;
    node->while_stmt.body = body;
    return node;
}

AstNode *ast_for(Arena *arena, SourceLoc loc, const char *var_name,
                 AstNode *iterable, AstNode *body) {
    AstNode *node = alloc_node(arena, NODE_FOR, loc);
    node->for_stmt.var_name = (char *)arena_alloc(arena, strlen(var_name) + 1);
    strcpy(node->for_stmt.var_name, var_name);
    node->for_stmt.iterable = iterable;
    node->for_stmt.body = body;
    return node;
}

AstNode *ast_loop(Arena *arena, SourceLoc loc, AstNode *body) {
    AstNode *node = alloc_node(arena, NODE_LOOP, loc);
    node->loop_stmt.body = body;
    return node;
}

AstNode *ast_return(Arena *arena, SourceLoc loc, AstNode **values, int value_count) {
    AstNode *node = alloc_node(arena, NODE_RETURN, loc);
    if (value_count > 0) {
        node->return_stmt.values = (AstNode **)arena_alloc(arena, sizeof(AstNode *) * value_count);
        for (int i = 0; i < value_count; i++)
            node->return_stmt.values[i] = values[i];
    } else {
        node->return_stmt.values = NULL;
    }
    node->return_stmt.value_count = value_count;
    return node;
}

AstNode *ast_return_one(Arena *arena, SourceLoc loc, AstNode *value) {
    return ast_return(arena, loc, &value, 1);
}

AstNode *ast_assign(Arena *arena, SourceLoc loc, AstNode *target,
                    AstNode *value, bool is_compound, BinaryOp compound_op) {
    AstNode *node = alloc_node(arena, NODE_ASSIGN, loc);
    node->assign.target = target;
    node->assign.value = value;
    node->assign.is_compound = is_compound;
    node->assign.compound_op = compound_op;
    return node;
}

AstNode *ast_binary(Arena *arena, SourceLoc loc, BinaryOp op,
                    AstNode *left, AstNode *right) {
    AstNode *node = alloc_node(arena, NODE_BINARY, loc);
    node->binary.op = op;
    node->binary.left = left;
    node->binary.right = right;
    return node;
}

AstNode *ast_unary(Arena *arena, SourceLoc loc, UnaryOp op, AstNode *operand) {
    AstNode *node = alloc_node(arena, NODE_UNARY, loc);
    node->unary.op = op;
    node->unary.operand = operand;
    return node;
}

AstNode *ast_call(Arena *arena, SourceLoc loc, AstNode *callee,
                  AstNode **args, int arg_count) {
    AstNode *node = alloc_node(arena, NODE_CALL, loc);
    node->call.callee = callee;
    node->call.arg_count = arg_count;
    if (arg_count > 0) {
        node->call.args = (AstNode **)arena_alloc(arena, sizeof(AstNode *) * arg_count);
        for (int i = 0; i < arg_count; i++)
            node->call.args[i] = args[i];
    } else {
        node->call.args = NULL;
    }
    return node;
}

AstNode *ast_index(Arena *arena, SourceLoc loc, AstNode *object, AstNode *index) {
    AstNode *node = alloc_node(arena, NODE_INDEX, loc);
    node->index.object = object;
    node->index.index = index;
    return node;
}

AstNode *ast_member(Arena *arena, SourceLoc loc, AstNode *object, const char *member) {
    AstNode *node = alloc_node(arena, NODE_MEMBER, loc);
    node->member.object = object;
    node->member.member = (char *)arena_alloc(arena, strlen(member) + 1);
    strcpy(node->member.member, member);
    return node;
}

AstNode *ast_int_literal(Arena *arena, SourceLoc loc, int64_t value) {
    AstNode *node = alloc_node(arena, NODE_INT_LITERAL, loc);
    node->literal.int_value = value;
    return node;
}

AstNode *ast_float_literal(Arena *arena, SourceLoc loc, double value) {
    AstNode *node = alloc_node(arena, NODE_FLOAT_LITERAL, loc);
    node->literal.float_value = value;
    return node;
}

AstNode *ast_string_literal(Arena *arena, SourceLoc loc, const char *value) {
    AstNode *node = alloc_node(arena, NODE_STRING_LITERAL, loc);
    if (value) {
        node->literal.string_value = (char *)arena_alloc(arena, strlen(value) + 1);
        strcpy(node->literal.string_value, value);
    } else {
        node->literal.string_value = NULL;
    }
    return node;
}

AstNode *ast_bool_literal(Arena *arena, SourceLoc loc, bool value) {
    AstNode *node = alloc_node(arena, NODE_BOOL_LITERAL, loc);
    node->literal.bool_value = value;
    return node;
}

AstNode *ast_null_literal(Arena *arena, SourceLoc loc) {
    return alloc_node(arena, NODE_NULL_LITERAL, loc);
}

AstNode *ast_identifier(Arena *arena, SourceLoc loc, const char *name) {
    AstNode *node = alloc_node(arena, NODE_IDENTIFIER, loc);
    node->identifier.name = (char *)arena_alloc(arena, strlen(name) + 1);
    strcpy(node->identifier.name, name);
    node->identifier.depth = -1;
    node->identifier.index = -1;
    return node;
}

AstNode *ast_interpolated_string(Arena *arena, SourceLoc loc) {
    AstNode *node = alloc_node(arena, NODE_INTERPOLATED_STRING, loc);
    node->interpolated_string.parts = NULL;
    node->interpolated_string.part_count = 0;
    return node;
}

void ast_interpolated_add_part(AstNode *node, AstNode *part) {
    (void)node;
    (void)part;
}

AstNode *ast_array_literal(Arena *arena, SourceLoc loc) {
    AstNode *node = alloc_node(arena, NODE_ARRAY_LITERAL, loc);
    node->array_literal.elements = NULL;
    node->array_literal.element_count = 0;
    return node;
}

void ast_array_add_element(AstNode *node, AstNode *elem) {
    (void)node;
    (void)elem;
}

AstNode *ast_tuple_literal(Arena *arena, SourceLoc loc) {
    AstNode *node = alloc_node(arena, NODE_TUPLE_LITERAL, loc);
    node->tuple_literal.elements = NULL;
    node->tuple_literal.element_count = 0;
    return node;
}

void ast_tuple_add_element(AstNode *node, AstNode *elem) {
    (void)node;
    (void)elem;
}

AstNode *ast_break(Arena *arena, SourceLoc loc) {
    return alloc_node(arena, NODE_BREAK, loc);
}

AstNode *ast_continue(Arena *arena, SourceLoc loc) {
    return alloc_node(arena, NODE_CONTINUE, loc);
}

AstNode *ast_match(Arena *arena, SourceLoc loc, AstNode *value) {
    AstNode *node = alloc_node(arena, NODE_MATCH, loc);
    node->match_stmt.value = value;
    node->match_stmt.arms = NULL;
    node->match_stmt.arm_count = 0;
    return node;
}

void ast_match_add_arm(AstNode *match, AstNode *pattern, AstNode *body) {
    (void)match;
    (void)pattern;
    (void)body;
}

AstNode *ast_struct_decl(Arena *arena, SourceLoc loc, const char *name,
                         char **field_names, int field_count,
                         char **type_params, int type_param_count) {
    AstNode *node = alloc_node(arena, NODE_STRUCT_DECL, loc);
    node->struct_decl.name = (char *)arena_alloc(arena, strlen(name) + 1);
    strcpy(node->struct_decl.name, name);
    if (field_count > 0) {
        node->struct_decl.field_names = (char **)arena_alloc(arena, sizeof(char *) * field_count);
        for (int i = 0; i < field_count; i++) {
            node->struct_decl.field_names[i] = (char *)arena_alloc(arena, strlen(field_names[i]) + 1);
            strcpy(node->struct_decl.field_names[i], field_names[i]);
        }
    } else {
        node->struct_decl.field_names = NULL;
    }
    node->struct_decl.field_count = field_count;
    node->struct_decl.type_params = NULL;
    node->struct_decl.type_param_count = type_param_count;
    (void)type_params;
    return node;
}

AstNode *ast_struct_literal(Arena *arena, SourceLoc loc, const char *name,
                            char **field_names, AstNode **field_values, int field_count) {
    AstNode *node = alloc_node(arena, NODE_STRUCT_LITERAL, loc);
    node->struct_literal.name = (char *)arena_alloc(arena, strlen(name) + 1);
    strcpy(node->struct_literal.name, name);
    if (field_count > 0) {
        node->struct_literal.field_names = (char **)arena_alloc(arena, sizeof(char *) * field_count);
        node->struct_literal.field_values = (AstNode **)arena_alloc(arena, sizeof(AstNode *) * field_count);
        for (int i = 0; i < field_count; i++) {
            node->struct_literal.field_names[i] = (char *)arena_alloc(arena, strlen(field_names[i]) + 1);
            strcpy(node->struct_literal.field_names[i], field_names[i]);
            node->struct_literal.field_values[i] = field_values[i];
        }
    } else {
        node->struct_literal.field_names = NULL;
        node->struct_literal.field_values = NULL;
    }
    node->struct_literal.field_count = field_count;
    return node;
}

AstNode *ast_enum_decl(Arena *arena, SourceLoc loc, const char *name,
                       char **variant_names, int *variant_counts, int variant_count,
                       char **type_params, int type_param_count) {
    AstNode *node = alloc_node(arena, NODE_ENUM_DECL, loc);
    node->enum_decl.name = (char *)arena_alloc(arena, strlen(name) + 1);
    strcpy(node->enum_decl.name, name);
    if (variant_count > 0) {
        node->enum_decl.variant_names = (char **)arena_alloc(arena, sizeof(char *) * variant_count);
        node->enum_decl.variant_counts = (int *)arena_alloc(arena, sizeof(int) * variant_count);
        for (int i = 0; i < variant_count; i++) {
            node->enum_decl.variant_names[i] = (char *)arena_alloc(arena, strlen(variant_names[i]) + 1);
            strcpy(node->enum_decl.variant_names[i], variant_names[i]);
            node->enum_decl.variant_counts[i] = variant_counts[i];
        }
    } else {
        node->enum_decl.variant_names = NULL;
        node->enum_decl.variant_counts = NULL;
    }
    node->enum_decl.variant_count = variant_count;
    node->enum_decl.type_params = NULL;
    node->enum_decl.type_param_count = type_param_count;
    (void)type_params;
    return node;
}

AstNode *ast_enum_literal(Arena *arena, SourceLoc loc, const char *enum_name,
                          const char *variant_name, int tag,
                          AstNode **values, int value_count) {
    AstNode *node = alloc_node(arena, NODE_ENUM_LITERAL, loc);
    node->enum_literal.enum_name = (char *)arena_alloc(arena, strlen(enum_name) + 1);
    strcpy(node->enum_literal.enum_name, enum_name);
    node->enum_literal.variant_name = (char *)arena_alloc(arena, strlen(variant_name) + 1);
    strcpy(node->enum_literal.variant_name, variant_name);
    node->enum_literal.tag = tag;
    if (value_count > 0) {
        node->enum_literal.values = (AstNode **)arena_alloc(arena, sizeof(AstNode *) * value_count);
        for (int i = 0; i < value_count; i++)
            node->enum_literal.values[i] = values[i];
    } else {
        node->enum_literal.values = NULL;
    }
    node->enum_literal.value_count = value_count;
    return node;
}

AstNode *ast_match_arm(Arena *arena, SourceLoc loc, AstNode *pattern, AstNode *body,
                       char **bind_names, int bind_count) {
    AstNode *node = alloc_node(arena, NODE_MATCH_ARM, loc);
    node->match_arm.pattern = pattern;
    node->match_arm.body = body;
    if (bind_count > 0 && bind_names) {
        node->match_arm.bind_names = (char **)arena_alloc(arena, sizeof(char *) * bind_count);
        for (int i = 0; i < bind_count; i++) {
            node->match_arm.bind_names[i] = (char *)arena_alloc(arena, strlen(bind_names[i]) + 1);
            strcpy(node->match_arm.bind_names[i], bind_names[i]);
        }
    } else {
        node->match_arm.bind_names = NULL;
    }
    node->match_arm.bind_count = bind_count;
    return node;
}

AstNode *ast_trait_decl(Arena *arena, SourceLoc loc, const char *name,
                         char **method_names, int method_count) {
    AstNode *node = alloc_node(arena, NODE_TRAIT_DECL, loc);
    node->trait_decl.name = (char *)arena_alloc(arena, strlen(name) + 1);
    strcpy(node->trait_decl.name, name);
    if (method_count > 0) {
        node->trait_decl.method_names = (char **)arena_alloc(arena, sizeof(char *) * method_count);
        for (int i = 0; i < method_count; i++) {
            node->trait_decl.method_names[i] = (char *)arena_alloc(arena, strlen(method_names[i]) + 1);
            strcpy(node->trait_decl.method_names[i], method_names[i]);
        }
    } else {
        node->trait_decl.method_names = NULL;
    }
    node->trait_decl.method_count = method_count;
    return node;
}

AstNode *ast_dispatch_call(Arena *arena, SourceLoc loc, AstNode *object,
                            const char *method_name, AstNode **args, int arg_count) {
    AstNode *node = alloc_node(arena, NODE_DISPATCH_CALL, loc);
    node->dispatch_call.object = object;
    node->dispatch_call.method_name = (char *)arena_alloc(arena, strlen(method_name) + 1);
    strcpy(node->dispatch_call.method_name, method_name);
    node->dispatch_call.arg_count = arg_count;
    if (arg_count > 0) {
        node->dispatch_call.args = (AstNode **)arena_alloc(arena, sizeof(AstNode *) * arg_count);
        for (int i = 0; i < arg_count; i++)
            node->dispatch_call.args[i] = args[i];
    } else {
        node->dispatch_call.args = NULL;
    }
    return node;
}

AstNode *ast_propagate(Arena *arena, SourceLoc loc, AstNode *expr) {
    AstNode *node = alloc_node(arena, NODE_PROPAGATE, loc);
    node->propagate.expr = expr;
    return node;
}

AstNode *ast_try(Arena *arena, SourceLoc loc, AstNode *try_body,
                 AstNode *catch_body, const char *catch_var) {
    AstNode *node = alloc_node(arena, NODE_TRY, loc);
    node->try_stmt.try_body = try_body;
    node->try_stmt.catch_body = catch_body;
    if (catch_var) {
        node->try_stmt.catch_var = (char *)arena_alloc(arena, strlen(catch_var) + 1);
        strcpy(node->try_stmt.catch_var, catch_var);
    } else {
        node->try_stmt.catch_var = NULL;
    }
    return node;
}

/* ─── Debug Print ─── */
static void print_indent(int indent) {
    for (int i = 0; i < indent; i++) printf("  ");
}

static const char *binary_op_name(BinaryOp op) {
    switch (op) {
        case OP_ADD: return "+";
        case OP_SUB: return "-";
        case OP_MUL: return "*";
        case OP_DIV: return "/";
        case OP_MOD: return "%";
        case OP_EQ: return "==";
        case OP_NE: return "!=";
        case OP_LT: return "<";
        case OP_GT: return ">";
        case OP_LE: return "<=";
        case OP_GE: return ">=";
        case OP_AND: return "&&";
        case OP_OR: return "||";
        case OP_BIT_AND: return "&";
        case OP_BIT_OR: return "|";
        case OP_BIT_XOR: return "^";
        case OP_SHL: return "<<";
        case OP_SHR: return ">>";
    }
    return "?";
}

void ast_print(AstNode *node, int indent) {
    print_indent(indent);

    switch (node->kind) {
        case NODE_PROGRAM:
            printf("(program\n");
            for (int i = 0; i < node->program.stmt_count; i++)
                ast_print(node->program.stmts[i], indent + 1);
            print_indent(indent);
            printf(")\n");
            break;

        case NODE_LET_DECL:
            printf("(let");
            for (int i = 0; i < node->let_decl.name_count; i++)
                printf(" %s", node->let_decl.names[i]);
            if (node->let_decl.initializer) {
                printf("\n");
                ast_print(node->let_decl.initializer, indent + 1);
                print_indent(indent);
            }
            printf(")\n");
            break;

        case NODE_CONST_DECL:
            printf("(const");
            for (int i = 0; i < node->let_decl.name_count; i++)
                printf(" %s", node->let_decl.names[i]);
            if (node->let_decl.initializer) {
                printf("\n");
                ast_print(node->let_decl.initializer, indent + 1);
                print_indent(indent);
            }
            printf(")\n");
            break;

        case NODE_FN_DECL:
            printf("(fn %s\n", node->fn_decl.name);
            if (node->fn_decl.body)
                ast_print(node->fn_decl.body, indent + 1);
            print_indent(indent);
            printf(")\n");
            break;

        case NODE_BLOCK:
            printf("(block\n");
            for (int i = 0; i < node->block.stmt_count; i++)
                ast_print(node->block.stmts[i], indent + 1);
            print_indent(indent);
            printf(")\n");
            break;

        case NODE_EXPR_STMT:
            printf("(expr\n");
            ast_print(node->expr_stmt.expr, indent + 1);
            print_indent(indent);
            printf(")\n");
            break;

        case NODE_IF:
            printf("(if\n");
            ast_print(node->if_stmt.condition, indent + 1);
            ast_print(node->if_stmt.then_branch, indent + 1);
            if (node->if_stmt.else_branch)
                ast_print(node->if_stmt.else_branch, indent + 1);
            print_indent(indent);
            printf(")\n");
            break;

        case NODE_WHILE:
            printf("(while\n");
            ast_print(node->while_stmt.condition, indent + 1);
            ast_print(node->while_stmt.body, indent + 1);
            print_indent(indent);
            printf(")\n");
            break;

        case NODE_FOR:
            printf("(for %s\n", node->for_stmt.var_name);
            ast_print(node->for_stmt.iterable, indent + 1);
            ast_print(node->for_stmt.body, indent + 1);
            print_indent(indent);
            printf(")\n");
            break;

        case NODE_LOOP:
            printf("(loop\n");
            ast_print(node->loop_stmt.body, indent + 1);
            print_indent(indent);
            printf(")\n");
            break;

        case NODE_RETURN:
            printf("(return\n");
            for (int i = 0; i < node->return_stmt.value_count; i++)
                ast_print(node->return_stmt.values[i], indent + 1);
            print_indent(indent);
            printf(")\n");
            break;

        case NODE_ASSIGN:
            printf("(assign\n");
            ast_print(node->assign.target, indent + 1);
            ast_print(node->assign.value, indent + 1);
            print_indent(indent);
            printf(")\n");
            break;

        case NODE_BINARY:
            printf("(%s\n", binary_op_name(node->binary.op));
            ast_print(node->binary.left, indent + 1);
            ast_print(node->binary.right, indent + 1);
            print_indent(indent);
            printf(")\n");
            break;

        case NODE_UNARY:
            printf("(%s\n", node->unary.op == OP_NEG ? "-" :
                             node->unary.op == OP_NOT ? "!" : "~");
            ast_print(node->unary.operand, indent + 1);
            print_indent(indent);
            printf(")\n");
            break;

        case NODE_CALL:
            printf("(call\n");
            ast_print(node->call.callee, indent + 1);
            for (int i = 0; i < node->call.arg_count; i++)
                ast_print(node->call.args[i], indent + 1);
            print_indent(indent);
            printf(")\n");
            break;

        case NODE_INDEX:
            printf("(index\n");
            ast_print(node->index.object, indent + 1);
            ast_print(node->index.index, indent + 1);
            print_indent(indent);
            printf(")\n");
            break;

        case NODE_MEMBER:
            printf("(. %s)\n", node->member.member);
            break;

        case NODE_INT_LITERAL:
            printf("%ld\n", (long)node->literal.int_value);
            break;

        case NODE_FLOAT_LITERAL:
            printf("%g\n", node->literal.float_value);
            break;

        case NODE_STRING_LITERAL:
            printf("\"%s\"\n", node->literal.string_value ? node->literal.string_value : "");
            break;

        case NODE_BOOL_LITERAL:
            printf("%s\n", node->literal.bool_value ? "true" : "false");
            break;

        case NODE_NULL_LITERAL:
            printf("null\n");
            break;

        case NODE_IDENTIFIER:
            printf("%s\n", node->identifier.name);
            break;

        case NODE_INTERPOLATED_STRING:
            printf("(interpolated %d parts)\n", node->interpolated_string.part_count);
            break;

        case NODE_ARRAY_LITERAL:
            printf("(array [%d elements]\n", node->array_literal.element_count);
            for (int i = 0; i < node->array_literal.element_count; i++)
                ast_print(node->array_literal.elements[i], indent + 1);
            print_indent(indent);
            printf(")\n");
            break;

        case NODE_TUPLE_LITERAL:
            printf("(tuple\n");
            for (int i = 0; i < node->tuple_literal.element_count; i++)
                ast_print(node->tuple_literal.elements[i], indent + 1);
            print_indent(indent);
            printf(")\n");
            break;

        case NODE_BREAK:
            printf("break\n");
            break;

        case NODE_CONTINUE:
            printf("continue\n");
            break;

        case NODE_STRUCT_DECL:
            printf("(struct %s", node->struct_decl.name);
            for (int i = 0; i < node->struct_decl.field_count; i++)
                printf(" %s", node->struct_decl.field_names[i]);
            printf(")\n");
            break;

        case NODE_STRUCT_LITERAL:
            printf("(struct-lit %s\n", node->struct_literal.name);
            for (int i = 0; i < node->struct_literal.field_count; i++) {
                print_indent(indent + 1);
                printf("%s: ", node->struct_literal.field_names[i]);
                ast_print(node->struct_literal.field_values[i], 0);
            }
            print_indent(indent);
            printf(")\n");
            break;

        case NODE_ENUM_DECL:
            printf("(enum %s", node->enum_decl.name);
            for (int i = 0; i < node->enum_decl.variant_count; i++)
                printf(" %s(%d)", node->enum_decl.variant_names[i], node->enum_decl.variant_counts[i]);
            printf(")\n");
            break;

        case NODE_ENUM_LITERAL:
            printf("(enum-lit %s::%s\n", node->enum_literal.enum_name, node->enum_literal.variant_name);
            for (int i = 0; i < node->enum_literal.value_count; i++)
                ast_print(node->enum_literal.values[i], indent + 1);
            print_indent(indent);
            printf(")\n");
            break;

        case NODE_MATCH:
            printf("(match\n");
            for (int i = 0; i < node->match_stmt.arm_count; i++)
                ast_print(node->match_stmt.arms[i], indent + 1);
            print_indent(indent);
            printf(")\n");
            break;

        case NODE_MATCH_ARM:
            printf("(=>\n");
            ast_print(node->match_arm.pattern, indent + 1);
            ast_print(node->match_arm.body, indent + 1);
            print_indent(indent);
            printf(")\n");
            break;

        case NODE_TRAIT_DECL:
            printf("(trait %s", node->trait_decl.name);
            for (int i = 0; i < node->trait_decl.method_count; i++)
                printf(" %s", node->trait_decl.method_names[i]);
            printf(")\n");
            break;

        case NODE_DISPATCH_CALL:
            printf("(dispatch %s\n", node->dispatch_call.method_name);
            ast_print(node->dispatch_call.object, indent + 1);
            for (int i = 0; i < node->dispatch_call.arg_count; i++)
                ast_print(node->dispatch_call.args[i], indent + 1);
            print_indent(indent);
            printf(")\n");
            break;

        case NODE_PROPAGATE:
            printf("(propagate\n");
            ast_print(node->propagate.expr, indent + 1);
            print_indent(indent);
            printf(")\n");
            break;

        case NODE_TRY:
            printf("(try\n");
            ast_print(node->try_stmt.try_body, indent + 1);
            if (node->try_stmt.catch_body) {
                print_indent(indent + 1);
                printf("(catch");
                if (node->try_stmt.catch_var) printf(" %s", node->try_stmt.catch_var);
                printf("\n");
                ast_print(node->try_stmt.catch_body, indent + 2);
                print_indent(indent + 1);
                printf(")\n");
            }
            print_indent(indent);
            printf(")\n");
            break;
    }
}

void ast_free(AstNode *node) {
    (void)node;
}
