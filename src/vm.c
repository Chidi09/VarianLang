#include "vm.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdarg.h>



/* ─── Forward declarations ─── */
static void compile_node(Compiler *compiler, AstNode *node);

/* ─── Chunk Implementation ─── */

void chunk_init(Chunk *chunk) {
    chunk->code = NULL;
    chunk->count = 0;
    chunk->capacity = 0;
    chunk->constants = NULL;
    chunk->constant_count = 0;
    chunk->constant_capacity = 0;
    chunk->rle_lines = NULL;
    chunk->rle_counts = NULL;
    chunk->rle_count = 0;
    chunk->rle_capacity = 0;
}

void chunk_free(Chunk *chunk) {
    free(chunk->code);
    free(chunk->rle_lines);
    free(chunk->rle_counts);
    for (int i = 0; i < chunk->constant_count; i++) {
        Value *v = &chunk->constants[i];
        if (v->type == VAL_STRING && v->as.string) {
            free(v->as.string->chars);
            free(v->as.string);
        } else if (v->type == VAL_FUNCTION && v->as.function) {
            ObjFunction *f = v->as.function;
            for (int j = 0; j < f->constant_count; j++) {
                if (f->constants[j].type == VAL_STRING && f->constants[j].as.string) {
                    free(f->constants[j].as.string->chars);
                    free(f->constants[j].as.string);
                }
            }
            free(f->code);
            free(f->rle_lines);
            free(f->rle_counts);
            free(f->constants);
            free(f);
        }
    }
    free(chunk->constants);
    chunk_init(chunk);
}

static void chunk_grow(Chunk *chunk) {
    int old = chunk->capacity;
    chunk->capacity = old < 8 ? 8 : old * 2;
    chunk->code = (uint8_t *)realloc(chunk->code, chunk->capacity);
}

void chunk_write(Chunk *chunk, uint8_t byte, int line) {
    if (chunk->capacity < chunk->count + 1)
        chunk_grow(chunk);
    chunk->code[chunk->count] = byte;
    /* RLE line tracking */
    if (chunk->rle_count > 0 && chunk->rle_lines[chunk->rle_count - 1] == line) {
        chunk->rle_counts[chunk->rle_count - 1]++;
    } else {
        if (chunk->rle_capacity < chunk->rle_count + 1) {
            int old = chunk->rle_capacity;
            chunk->rle_capacity = old < 8 ? 8 : old * 2;
            chunk->rle_lines = (int *)realloc(chunk->rle_lines, sizeof(int) * chunk->rle_capacity);
            chunk->rle_counts = (int *)realloc(chunk->rle_counts, sizeof(int) * chunk->rle_capacity);
        }
        chunk->rle_lines[chunk->rle_count] = line;
        chunk->rle_counts[chunk->rle_count] = 1;
        chunk->rle_count++;
    }
    chunk->count++;
}

int chunk_get_line(Chunk *chunk, int offset) {
    int pos = 0;
    for (int i = 0; i < chunk->rle_count; i++) {
        pos += chunk->rle_counts[i];
        if (offset < pos) return chunk->rle_lines[i];
    }
    return 0;
}

int chunk_add_constant(Chunk *chunk, Value value) {
    if (chunk->constant_capacity < chunk->constant_count + 1) {
        int old = chunk->constant_capacity;
        chunk->constant_capacity = old < 8 ? 8 : old * 2;
        chunk->constants = (Value *)realloc(chunk->constants,
                                            sizeof(Value) * chunk->constant_capacity);
    }
    chunk->constants[chunk->constant_count] = value;
    return chunk->constant_count++;
}

void chunk_write_constant(Chunk *chunk, Value value, int line) {
    int index = chunk_add_constant(chunk, value);
    if (index < 256) {
        chunk_write(chunk, BC_CONSTANT, line);
        chunk_write(chunk, (uint8_t)index, line);
    } else {
        chunk_write(chunk, BC_CONSTANT_LONG, line);
        chunk_write(chunk, (uint8_t)((index >> 8) & 0xFF), line);
        chunk_write(chunk, (uint8_t)(index & 0xFF), line);
    }
}

/* ─── Value Implementation ─── */

Value val_nil(void) {
    Value v;
    v.type = VAL_NIL;
    v.as.integer = 0;
    return v;
}

Value val_bool(bool b) {
    Value v;
    v.type = VAL_BOOL;
    v.as.boolean = b;
    return v;
}

Value val_int(int64_t i) {
    Value v;
    v.type = VAL_INT;
    v.as.integer = i;
    return v;
}

Value val_float(double f) {
    Value v;
    v.type = VAL_FLOAT;
    v.as.floating = f;
    return v;
}

Value val_string(ObjString *s) {
    Value v;
    v.type = VAL_STRING;
    v.as.string = s;
    return v;
}

Value val_array(ObjArray *a) {
    Value v;
    v.type = VAL_ARRAY;
    v.as.array = a;
    return v;
}

Value val_tuple(ObjTuple *t) {
    Value v;
    v.type = VAL_TUPLE;
    v.as.tuple = t;
    return v;
}

Value val_function(ObjFunction *f) {
    Value v;
    v.type = VAL_FUNCTION;
    v.as.function = f;
    return v;
}

Value val_native_fn(void *fn) {
    Value v;
    v.type = VAL_NATIVE_FN;
    v.as.native_fn = fn;
    return v;
}

Value val_struct(ObjStruct *s) {
    Value v;
    v.type = VAL_STRUCT;
    v.as.structure = s;
    return v;
}

Value val_enum(ObjEnum *e) {
    Value v;
    v.type = VAL_ENUM;
    v.as.enum_val = e;
    return v;
}

/* ─── Object Allocation ─── */
static Obj *allocate_object(VM *vm, ValueType type, size_t size) {
    Obj *obj = (Obj *)calloc(1, size);
    if (!obj) return NULL;
    obj->type = type;
    obj->next = vm->objects;
    vm->objects = obj;
    return obj;
}

static uint32_t hash_string(const char *chars, int length) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < length; i++) {
        hash ^= (uint8_t)chars[i];
        hash *= 16777619;
    }
    return hash;
}

ObjString *copy_string(const char *chars, int length) {
    ObjString *s = (ObjString *)calloc(1, sizeof(ObjString));
    if (!s) return NULL;
    s->obj.type = VAL_STRING;
    s->obj.next = NULL;
    s->length = length;
    s->chars = (char *)malloc(length + 1);
    memcpy(s->chars, chars, length);
    s->chars[length] = '\0';
    s->hash = hash_string(chars, length);
    return s;
}

static ObjString *allocate_string(VM *vm, const char *chars, int length) {
    ObjString *s = (ObjString *)allocate_object(vm, VAL_STRING, sizeof(ObjString));
    if (!s) return NULL;
    s->length = length;
    s->chars = (char *)malloc(length + 1);
    memcpy(s->chars, chars, length);
    s->chars[length] = '\0';
    s->hash = hash_string(chars, length);
    return s;
}

ObjArray *new_array(void) {
    ObjArray *a = (ObjArray *)calloc(1, sizeof(ObjArray));
    if (!a) return NULL;
    a->obj.type = VAL_ARRAY;
    a->elements = NULL;
    a->count = 0;
    a->capacity = 0;
    return a;
}

static ObjArray *allocate_array(VM *vm) {
    ObjArray *a = (ObjArray *)allocate_object(vm, VAL_ARRAY, sizeof(ObjArray));
    if (!a) return NULL;
    a->elements = NULL;
    a->count = 0;
    a->capacity = 0;
    return a;
}

ObjTuple *new_tuple(int count) {
    ObjTuple *t = (ObjTuple *)calloc(1, sizeof(ObjTuple));
    if (!t) return NULL;
    t->obj.type = VAL_TUPLE;
    t->count = count;
    t->elements = (Value *)calloc(count, sizeof(Value));
    return t;
}

static ObjTuple *allocate_tuple(VM *vm, int count) {
    ObjTuple *t = (ObjTuple *)allocate_object(vm, VAL_TUPLE, sizeof(ObjTuple));
    if (!t) return NULL;
    t->count = count;
    t->elements = (Value *)calloc(count, sizeof(Value));
    return t;
}

ObjStruct *new_struct(int field_count) {
    ObjStruct *s = (ObjStruct *)calloc(1, sizeof(ObjStruct));
    s->obj.type = VAL_STRUCT;
    s->field_count = field_count;
    s->field_names = (char **)calloc(field_count, sizeof(char *));
    s->fields = (Value *)calloc(field_count, sizeof(Value));
    return s;
}

ObjEnum *new_enum(int value_count) {
    ObjEnum *e = (ObjEnum *)calloc(1, sizeof(ObjEnum));
    e->obj.type = VAL_ENUM;
    e->tag = 0;
    e->count = value_count;
    e->values = (Value *)calloc(value_count, sizeof(Value));
    return e;
}

ObjFunction *new_function(void) {
    ObjFunction *f = (ObjFunction *)calloc(1, sizeof(ObjFunction));
    if (!f) return NULL;
    f->obj.type = VAL_FUNCTION;
    f->arity = 0;
    f->code = NULL;
    f->code_count = 0;
    f->code_capacity = 0;
    f->constants = NULL;
    f->constant_count = 0;
    f->constant_capacity = 0;
    f->stack_size = 0;
    return f;
}

/* ─── Value Operations ─── */
void value_print(Value value) {
    switch (value.type) {
        case VAL_NIL:      printf("nil"); break;
        case VAL_BOOL:     printf("%s", value.as.boolean ? "true" : "false"); break;
        case VAL_INT:      printf("%ld", (long)value.as.integer); break;
        case VAL_FLOAT:    printf("%g", value.as.floating); break;
        case VAL_STRING:   printf("%s", value.as.string->chars); break;
        case VAL_ARRAY: {
            printf("[");
            for (int i = 0; i < value.as.array->count; i++) {
                if (i > 0) printf(", ");
                value_print(value.as.array->elements[i]);
            }
            printf("]");
            break;
        }
        case VAL_TUPLE: {
            printf("(");
            for (int i = 0; i < value.as.tuple->count; i++) {
                if (i > 0) printf(", ");
                value_print(value.as.tuple->elements[i]);
            }
            printf(")");
            break;
        }
        case VAL_FUNCTION: printf("<fn %p>", (void *)value.as.function); break;
        case VAL_NATIVE_FN: printf("<native fn>"); break;
        case VAL_STRUCT: {
            ObjStruct *s = value.as.structure;
            printf("{");
            for (int i = 0; i < s->field_count; i++) {
                if (i > 0) printf(", ");
                printf("%s: ", s->field_names[i]);
                value_print(s->fields[i]);
            }
            printf("}");
            break;
        }
        case VAL_ENUM: {
            ObjEnum *e = value.as.enum_val;
            printf("#%d", e->tag);
            if (e->count > 0) {
                printf("(");
                for (int i = 0; i < e->count; i++) {
                    if (i > 0) printf(", ");
                    value_print(e->values[i]);
                }
                printf(")");
            }
            break;
        }
    }
}

bool value_is_truthy(Value value) {
    switch (value.type) {
        case VAL_NIL:    return false;
        case VAL_BOOL:   return value.as.boolean;
        case VAL_INT:    return value.as.integer != 0;
        case VAL_FLOAT:  return value.as.floating != 0.0;
        case VAL_STRING: return value.as.string->length > 0;
        default:         return true;
    }
}

bool value_equal(Value a, Value b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case VAL_NIL:    return true;
        case VAL_BOOL:   return a.as.boolean == b.as.boolean;
        case VAL_INT:    return a.as.integer == b.as.integer;
        case VAL_FLOAT:  return a.as.floating == b.as.floating;
        case VAL_STRING: return a.as.string->length == b.as.string->length &&
                                memcmp(a.as.string->chars, b.as.string->chars,
                                       a.as.string->length) == 0;
        case VAL_ENUM: {
            if (a.as.enum_val->tag != b.as.enum_val->tag) return false;
            if (a.as.enum_val->count != b.as.enum_val->count) return false;
            for (int i = 0; i < a.as.enum_val->count; i++) {
                if (!value_equal(a.as.enum_val->values[i], b.as.enum_val->values[i]))
                    return false;
            }
            return true;
        }
        default: return false;
    }
}

/* ─── Compiler ─── */

void compiler_init(Compiler *compiler, Arena *arena, Chunk *chunk, AstNode *program) {
    compiler->arena = arena;
    compiler->chunk = chunk;
    compiler->program = program;
    compiler->scope_depth = 0;
    compiler->local_count = 0;
    compiler->loop_count = 0;
    compiler->current_line = 0;
    compiler->had_error = false;
    compiler->error_message[0] = '\0';
}

static void compiler_error(Compiler *compiler, const char *fmt, ...) {
    if (compiler->had_error) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(compiler->error_message, sizeof(compiler->error_message), fmt, args);
    va_end(args);
    compiler->had_error = true;
}

static int compiler_add_local(Compiler *compiler, const char *name) {
    if (compiler->local_count >= 256) {
        compiler_error(compiler, "Too many local variables");
        return 0;
    }
    int idx = compiler->local_count++;
    strncpy(compiler->local_names[idx], name, 63);
    compiler->local_names[idx][63] = '\0';
    compiler->local_depths[idx] = compiler->scope_depth;
    return idx;
}

static int compiler_find_local(Compiler *compiler, const char *name) {
    for (int i = compiler->local_count - 1; i >= 0; i--) {
        if (strcmp(compiler->local_names[i], name) == 0)
            return i;
    }
    return -1;
}

static void compiler_push_loop(Compiler *compiler, int loop_start) {
    if (compiler->loop_count >= MAX_LOOP_NESTING) {
        compiler_error(compiler, "Too many nested loops");
        return;
    }
    LoopInfo *loop = &compiler->loops[compiler->loop_count++];
    loop->loop_start = loop_start;
    loop->break_count = 0;
}

static int compiler_pop_loop(Compiler *compiler, int exit_patch) {
    if (compiler->loop_count <= 0) {
        compiler_error(compiler, "Internal: loop stack underflow");
        return 0;
    }
    compiler->loop_count--;
    LoopInfo *loop = &compiler->loops[compiler->loop_count];
    /* Patch all break jumps to the exit */
    for (int i = 0; i < loop->break_count; i++) {
        int offset = loop->break_jumps[i];
        if (offset > 0) {
            int jump = exit_patch - offset - 2;
            compiler->chunk->code[offset] = (jump >> 8) & 0xFF;
            compiler->chunk->code[offset + 1] = jump & 0xFF;
        }
    }
    return loop->loop_start;
}

static void compiler_record_break(Compiler *compiler, int jump_offset) {
    if (compiler->loop_count <= 0) return;
    LoopInfo *loop = &compiler->loops[compiler->loop_count - 1];
    if (loop->break_count < 64) {
        loop->break_jumps[loop->break_count++] = jump_offset;
    }
}

static void emit_byte(Compiler *compiler, uint8_t byte) {
    chunk_write(compiler->chunk, byte, compiler->current_line);
}

static void emit_bytes(Compiler *compiler, uint8_t b1, uint8_t b2) {
    emit_byte(compiler, b1);
    emit_byte(compiler, b2);
}

static int emit_jump(Compiler *compiler, uint8_t instruction) {
    emit_byte(compiler, instruction);
    emit_byte(compiler, 0xFF);
    emit_byte(compiler, 0xFF);
    return compiler->chunk->count - 2;
}

static void patch_jump(Compiler *compiler, int offset) {
    int jump = compiler->chunk->count - offset - 2;
    if (jump > UINT16_MAX)
        compiler_error(compiler, "Too much code to jump over");
    compiler->chunk->code[offset] = (jump >> 8) & 0xFF;
    compiler->chunk->code[offset + 1] = jump & 0xFF;
}

static void emit_loop(Compiler *compiler, int loop_start) {
    emit_byte(compiler, BC_LOOP);
    int offset = compiler->chunk->count - loop_start + 2;
    if (offset > UINT16_MAX)
        compiler_error(compiler, "Loop body too large");
    emit_byte(compiler, (offset >> 8) & 0xFF);
    emit_byte(compiler, offset & 0xFF);
}

static void emit_constant(Compiler *compiler, Value value) {
    int idx = chunk_add_constant(compiler->chunk, value);
    if (idx < 256) {
        emit_bytes(compiler, BC_CONSTANT, (uint8_t)idx);
    } else {
        emit_byte(compiler, BC_CONSTANT_LONG);
        emit_byte(compiler, (uint8_t)((idx >> 8) & 0xFF));
        emit_byte(compiler, (uint8_t)(idx & 0xFF));
    }
}

static void emit_constant_idx(Compiler *compiler, int idx) {
    if (idx < 256) {
        emit_bytes(compiler, BC_CONSTANT, (uint8_t)idx);
    } else {
        emit_byte(compiler, BC_CONSTANT_LONG);
        emit_byte(compiler, (uint8_t)((idx >> 8) & 0xFF));
        emit_byte(compiler, (uint8_t)(idx & 0xFF));
    }
}

/* ─── Compilation: Expressions ─── */
static void compile_expression(Compiler *compiler, AstNode *node) {
    if (!node) {
        emit_byte(compiler, BC_NIL);
        return;
    }
    compiler->current_line = node->loc.line;

    switch (node->kind) {
        case NODE_INT_LITERAL:
            emit_constant(compiler, val_int(node->literal.int_value));
            break;

        case NODE_FLOAT_LITERAL:
            emit_constant(compiler, val_float(node->literal.float_value));
            break;

        case NODE_STRING_LITERAL: {
            const char *val = node->literal.string_value ? node->literal.string_value : "";
            int len = (int)strlen(val);
            emit_constant(compiler, val_string(copy_string(val, len)));
            break;
        }

        case NODE_BOOL_LITERAL:
            emit_byte(compiler, node->literal.bool_value ? BC_TRUE : BC_FALSE);
            break;

        case NODE_NULL_LITERAL:
            emit_byte(compiler, BC_NIL);
            break;

        case NODE_IDENTIFIER: {
            int local_idx = compiler_find_local(compiler, node->identifier.name);
            if (local_idx >= 0) {
                emit_bytes(compiler, BC_GET_LOCAL, (uint8_t)local_idx);
            } else {
                emit_byte(compiler, BC_GET_GLOBAL);
                ObjString *s = copy_string(node->identifier.name, (int)strlen(node->identifier.name));
                int idx = chunk_add_constant(compiler->chunk, val_string(s));
                emit_byte(compiler, (uint8_t)idx);
            }
            break;
        }

        case NODE_BINARY: {
            compile_expression(compiler, node->binary.left);
            compile_expression(compiler, node->binary.right);
            switch (node->binary.op) {
                case OP_ADD: emit_byte(compiler, BC_ADD); break;
                case OP_SUB: emit_byte(compiler, BC_SUB); break;
                case OP_MUL: emit_byte(compiler, BC_MUL); break;
                case OP_DIV: emit_byte(compiler, BC_DIV); break;
                case OP_MOD: emit_byte(compiler, BC_MOD); break;
                case OP_EQ:  emit_byte(compiler, BC_EQUAL); break;
                case OP_NE:  emit_byte(compiler, BC_NOT_EQUAL); break;
                case OP_LT:  emit_byte(compiler, BC_LESS); break;
                case OP_GT:  emit_byte(compiler, BC_GREATER); break;
                case OP_LE:  emit_byte(compiler, BC_LESS_EQUAL); break;
                case OP_GE:  emit_byte(compiler, BC_GREATER_EQUAL); break;
                case OP_AND: emit_byte(compiler, BC_AND); break;
                case OP_OR:  emit_byte(compiler, BC_OR); break;
                case OP_BIT_AND: emit_byte(compiler, BC_AND); break;
                case OP_BIT_OR:  emit_byte(compiler, BC_OR); break;
                case OP_BIT_XOR: emit_byte(compiler, BC_EQUAL); break; /* placeholder: XOR */
                case OP_SHL:     emit_byte(compiler, BC_MUL); break;  /* placeholder */
                case OP_SHR:     emit_byte(compiler, BC_DIV); break;  /* placeholder */
                default: compiler_error(compiler, "Unknown binary operator"); break;
            }
            break;
        }

        case NODE_UNARY: {
            compile_expression(compiler, node->unary.operand);
            switch (node->unary.op) {
                case OP_NEG: emit_byte(compiler, BC_NEGATE); break;
                case OP_NOT: emit_byte(compiler, BC_NOT); break;
                default: compiler_error(compiler, "Unknown unary operator"); break;
            }
            break;
        }

        case NODE_CALL: {
            compile_expression(compiler, node->call.callee);
            for (int i = 0; i < node->call.arg_count; i++)
                compile_expression(compiler, node->call.args[i]);
            emit_bytes(compiler, BC_CALL, (uint8_t)node->call.arg_count);
            break;
        }

        case NODE_ARRAY_LITERAL: {
            for (int i = 0; i < node->array_literal.element_count; i++)
                compile_expression(compiler, node->array_literal.elements[i]);
            emit_bytes(compiler, BC_ARRAY, (uint8_t)node->array_literal.element_count);
            break;
        }

        case NODE_TUPLE_LITERAL: {
            for (int i = 0; i < node->tuple_literal.element_count; i++)
                compile_expression(compiler, node->tuple_literal.elements[i]);
            emit_bytes(compiler, BC_TUPLE, (uint8_t)node->tuple_literal.element_count);
            break;
        }

        case NODE_INDEX: {
            compile_expression(compiler, node->index.object);
            compile_expression(compiler, node->index.index);
            emit_byte(compiler, BC_INDEX);
            break;
        }

        case NODE_MEMBER: {
            compile_expression(compiler, node->member.object);
            ObjString *s = copy_string(node->member.member, (int)strlen(node->member.member));
            emit_byte(compiler, BC_MEMBER);
            int idx = chunk_add_constant(compiler->chunk, val_string(s));
            emit_byte(compiler, (uint8_t)idx);
            break;
        }

        case NODE_INTERPOLATED_STRING: {
            if (node->interpolated_string.part_count == 0) {
                emit_byte(compiler, BC_NIL);
                break;
            }
            /* Compile first part and convert to string */
            compile_expression(compiler, node->interpolated_string.parts[0]);
            emit_byte(compiler, BC_INT_TO_STRING);
            for (int i = 1; i < node->interpolated_string.part_count; i++) {
                /* Compile next part, convert to string, then concat */
                compile_expression(compiler, node->interpolated_string.parts[i]);
                emit_byte(compiler, BC_INT_TO_STRING);
                emit_byte(compiler, BC_STRING_CONCAT);
            }
            break;
        }

        default:
            compile_node(compiler, node);
            break;
    }
}

/* Return the number of values a function type returns */
static int function_return_count(Type *type) {
    if (!type || type->kind != TYPE_FUNCTION) return 1;
    Type *ret = type->function.return_type;
    if (!ret) return 1;
    if (ret->kind == TYPE_TUPLE) return ret->tuple.count;
    return 1;
}

/* ─── Compilation: Statements ─── */
static void compile_node(Compiler *compiler, AstNode *node) {
    if (!node) return;
    compiler->current_line = node->loc.line;

    switch (node->kind) {
        case NODE_PROGRAM:
            for (int i = 0; i < node->program.stmt_count; i++)
                compile_node(compiler, node->program.stmts[i]);
            emit_byte(compiler, BC_HALT);
            break;

        case NODE_LET_DECL:
        case NODE_CONST_DECL:
            if (node->let_decl.initializer)
                compile_expression(compiler, node->let_decl.initializer);
            else
                emit_byte(compiler, BC_NIL);

            if (compiler->scope_depth > 0) {
                /* Local variable(s) */
                for (int i = node->let_decl.name_count - 1; i >= 0; i--) {
                    const char *name = node->let_decl.names[i];
                    if (strcmp(name, "_") == 0) {
                        emit_byte(compiler, BC_POP);
                    } else {
                        int idx = compiler_add_local(compiler, name);
                        emit_bytes(compiler, BC_SET_LOCAL, (uint8_t)idx);
                        /* Don't pop — the value protects the local slot
                         * from being overwritten by the next eval push */
                    }
                }
            } else {
                /* Global variable(s) */
                for (int i = node->let_decl.name_count - 1; i >= 0; i--) {
                    const char *name = node->let_decl.names[i];
                    if (strcmp(name, "_") == 0) {
                        emit_byte(compiler, BC_POP);
                    } else {
                        emit_byte(compiler, BC_DEFINE_GLOBAL);
                        ObjString *s = copy_string(name, (int)strlen(name));
                        int idx = chunk_add_constant(compiler->chunk, val_string(s));
                        emit_byte(compiler, (uint8_t)idx);
                    }
                }
            }
            break;

        case NODE_FN_DECL: {
            Chunk fn_chunk;
            chunk_init(&fn_chunk);
            Compiler fn_compiler;
            fn_compiler.arena = compiler->arena;
            fn_compiler.chunk = &fn_chunk;
            fn_compiler.scope_depth = 1;
            fn_compiler.had_error = false;
            fn_compiler.error_message[0] = '\0';
            fn_compiler.local_count = 0;
            fn_compiler.loop_count = 0;

            /* Register parameters as locals */
            for (int i = 0; i < node->fn_decl.param_count; i++) {
                compiler_add_local(&fn_compiler, node->fn_decl.param_names[i]);
            }

            if (node->fn_decl.body) {
                for (int i = 0; i < node->fn_decl.body->block.stmt_count; i++)
                    compile_node(&fn_compiler, node->fn_decl.body->block.stmts[i]);
            }
            {
                int rcount = function_return_count(node->fn_decl.fn_type);
                if (rcount == 1) {
                    emit_byte(&fn_compiler, BC_NIL);
                    emit_byte(&fn_compiler, BC_RETURN);
                } else {
                    for (int i = 0; i < rcount; i++)
                        emit_byte(&fn_compiler, BC_NIL);
                    emit_bytes(&fn_compiler, BC_RETURN_N, (uint8_t)rcount);
                }
            }

            /* Create function value without VM tracking */
            ObjFunction *func = (ObjFunction *)calloc(1, sizeof(ObjFunction));
            func->obj.type = VAL_FUNCTION;
            func->arity = node->fn_decl.param_count;
            func->code = fn_chunk.code;
            func->code_count = fn_chunk.count;
            func->code_capacity = fn_chunk.capacity;
            func->constants = fn_chunk.constants;
            func->constant_count = fn_chunk.constant_count;
            func->constant_capacity = fn_chunk.constant_capacity;
            func->rle_lines = fn_chunk.rle_lines;
            func->rle_counts = fn_chunk.rle_counts;
            func->rle_count = fn_chunk.rle_count;

            emit_constant(compiler, val_function(func));
            emit_byte(compiler, BC_DEFINE_GLOBAL);
            {
                ObjString *s = copy_string(node->fn_decl.name,
                                           (int)strlen(node->fn_decl.name));
                int idx = chunk_add_constant(compiler->chunk, val_string(s));
                emit_byte(compiler, (uint8_t)idx);
            }
            break;
        }

        case NODE_BLOCK: {
            int saved_local_count = compiler->local_count;
            for (int i = 0; i < node->block.stmt_count; i++)
                compile_node(compiler, node->block.stmts[i]);
            compiler->local_count = saved_local_count;
            break;
        }

        case NODE_EXPR_STMT:
            compile_expression(compiler, node->expr_stmt.expr);
            emit_byte(compiler, BC_POP);
            break;

        case NODE_IF: {
            compile_expression(compiler, node->if_stmt.condition);
            int else_jump = emit_jump(compiler, BC_JUMP_IF_FALSE);
            emit_byte(compiler, BC_POP);
            compile_node(compiler, node->if_stmt.then_branch);
            int end_jump = emit_jump(compiler, BC_JUMP);
            patch_jump(compiler, else_jump);
            emit_byte(compiler, BC_POP);
            if (node->if_stmt.else_branch)
                compile_node(compiler, node->if_stmt.else_branch);
            patch_jump(compiler, end_jump);
            break;
        }

        case NODE_WHILE: {
            int loop_start = compiler->chunk->count;
            compiler_push_loop(compiler, loop_start);
            compile_expression(compiler, node->while_stmt.condition);
            int exit_jump = emit_jump(compiler, BC_JUMP_IF_FALSE);
            emit_byte(compiler, BC_POP);
            compile_node(compiler, node->while_stmt.body);
            emit_loop(compiler, loop_start);
            patch_jump(compiler, exit_jump);
            emit_byte(compiler, BC_POP);
            /* Break jumps go here (after the false-condition pop) */
            int break_target = compiler->chunk->count;
            compiler_pop_loop(compiler, break_target);
            break;
        }

        case NODE_FOR: {
            const char *var_name = node->for_stmt.var_name;
            int saved_local_count = compiler->local_count;
            bool is_range = node->for_stmt.iterable &&
                node->for_stmt.iterable->kind == NODE_TUPLE_LITERAL &&
                node->for_stmt.iterable->tuple_literal.element_count == 2;

            if (is_range && compiler->scope_depth > 0) {
                /* for var in start..end { body } — inside a function (use locals) */
                AstNode *start = node->for_stmt.iterable->tuple_literal.elements[0];
                AstNode *end = node->for_stmt.iterable->tuple_literal.elements[1];

                int var_idx = compiler_add_local(compiler, var_name);
                compile_expression(compiler, start);
                emit_bytes(compiler, BC_SET_LOCAL, (uint8_t)var_idx);

                compile_expression(compiler, end);
                int end_idx = compiler_add_local(compiler, "__end__");
                emit_bytes(compiler, BC_SET_LOCAL, (uint8_t)end_idx);

                int loop_start = compiler->chunk->count;
                compiler_push_loop(compiler, loop_start);

                emit_bytes(compiler, BC_GET_LOCAL, (uint8_t)var_idx);
                emit_bytes(compiler, BC_GET_LOCAL, (uint8_t)end_idx);
                emit_byte(compiler, BC_LESS);
                int exit_jump = emit_jump(compiler, BC_JUMP_IF_FALSE);
                emit_byte(compiler, BC_POP);

                compile_node(compiler, node->for_stmt.body);

                emit_bytes(compiler, BC_GET_LOCAL, (uint8_t)var_idx);
                int one_idx = chunk_add_constant(compiler->chunk, val_int(1));
                emit_constant_idx(compiler, one_idx);
                emit_byte(compiler, BC_ADD);
                emit_bytes(compiler, BC_SET_LOCAL, (uint8_t)var_idx);
                emit_byte(compiler, BC_POP);

                emit_loop(compiler, loop_start);
                patch_jump(compiler, exit_jump);
                emit_byte(compiler, BC_POP);
                int break_target = compiler->chunk->count;
                compiler_pop_loop(compiler, break_target);
            } else if (is_range && compiler->scope_depth == 0) {
                /* for var in start..end { body } — at top level (use globals) */
                AstNode *start = node->for_stmt.iterable->tuple_literal.elements[0];
                AstNode *end = node->for_stmt.iterable->tuple_literal.elements[1];

                compile_expression(compiler, start);
                {
                    ObjString *s = copy_string(var_name, (int)strlen(var_name));
                    int idx = chunk_add_constant(compiler->chunk, val_string(s));
                    emit_byte(compiler, BC_DEFINE_GLOBAL);
                    emit_byte(compiler, (uint8_t)(idx & 0xFF));
                }

                compile_expression(compiler, end);
                {
                    ObjString *s = copy_string("__end__", 7);
                    int idx = chunk_add_constant(compiler->chunk, val_string(s));
                    emit_byte(compiler, BC_DEFINE_GLOBAL);
                    emit_byte(compiler, (uint8_t)(idx & 0xFF));
                }

                int loop_start = compiler->chunk->count;
                compiler_push_loop(compiler, loop_start);

                {
                    ObjString *s = copy_string(var_name, (int)strlen(var_name));
                    int idx = chunk_add_constant(compiler->chunk, val_string(s));
                    emit_byte(compiler, BC_GET_GLOBAL);
                    emit_byte(compiler, (uint8_t)(idx & 0xFF));
                }
                {
                    ObjString *s = copy_string("__end__", 7);
                    int idx = chunk_add_constant(compiler->chunk, val_string(s));
                    emit_byte(compiler, BC_GET_GLOBAL);
                    emit_byte(compiler, (uint8_t)(idx & 0xFF));
                }
                emit_byte(compiler, BC_LESS);
                int exit_jump = emit_jump(compiler, BC_JUMP_IF_FALSE);
                emit_byte(compiler, BC_POP);

                compile_node(compiler, node->for_stmt.body);

                {
                    ObjString *s = copy_string(var_name, (int)strlen(var_name));
                    int idx = chunk_add_constant(compiler->chunk, val_string(s));
                    emit_byte(compiler, BC_GET_GLOBAL);
                    emit_byte(compiler, (uint8_t)(idx & 0xFF));
                }
                int one_idx = chunk_add_constant(compiler->chunk, val_int(1));
                emit_constant_idx(compiler, one_idx);
                emit_byte(compiler, BC_ADD);
                {
                    ObjString *s = copy_string(var_name, (int)strlen(var_name));
                    int idx = chunk_add_constant(compiler->chunk, val_string(s));
                    emit_byte(compiler, BC_SET_GLOBAL);
                    emit_byte(compiler, (uint8_t)(idx & 0xFF));
                }
                emit_byte(compiler, BC_POP);

                emit_loop(compiler, loop_start);
                patch_jump(compiler, exit_jump);
                emit_byte(compiler, BC_POP);
                int break_target = compiler->chunk->count;
                compiler_pop_loop(compiler, break_target);
            } else {
                /* Non-range: simplified */
                if (!is_range && compiler->scope_depth > 0)
                    compile_expression(compiler, node->for_stmt.iterable);
                emit_byte(compiler, BC_POP);
                int loop_start = compiler->chunk->count;
                compiler_push_loop(compiler, loop_start);
                compile_node(compiler, node->for_stmt.body);
                emit_loop(compiler, loop_start);
                int break_target = compiler->chunk->count;
                compiler_pop_loop(compiler, break_target);
            }
            compiler->local_count = saved_local_count;
            break;
        }

        case NODE_LOOP: {
            int loop_start = compiler->chunk->count;
            compiler_push_loop(compiler, loop_start);
            compile_node(compiler, node->loop_stmt.body);
            emit_loop(compiler, loop_start);
            int break_target = compiler->chunk->count;
            compiler_pop_loop(compiler, break_target);
            break;
        }

        case NODE_RETURN:
            if (node->return_stmt.value_count == 0) {
                emit_byte(compiler, BC_NIL);
                emit_byte(compiler, BC_RETURN);
            } else if (node->return_stmt.value_count == 1) {
                compile_expression(compiler, node->return_stmt.values[0]);
                emit_byte(compiler, BC_RETURN);
            } else {
                for (int i = 0; i < node->return_stmt.value_count; i++)
                    compile_expression(compiler, node->return_stmt.values[i]);
                emit_bytes(compiler, BC_RETURN_N, (uint8_t)node->return_stmt.value_count);
            }
            break;

        case NODE_ASSIGN:
            compile_expression(compiler, node->assign.value);
            if (node->assign.target->kind == NODE_IDENTIFIER) {
                int local_idx = compiler_find_local(compiler, node->assign.target->identifier.name);
                if (local_idx >= 0) {
                    emit_bytes(compiler, BC_SET_LOCAL, (uint8_t)local_idx);
                } else {
                    emit_byte(compiler, BC_SET_GLOBAL);
                    ObjString *s = copy_string(node->assign.target->identifier.name,
                                               (int)strlen(node->assign.target->identifier.name));
                    int idx = chunk_add_constant(compiler->chunk, val_string(s));
                    emit_byte(compiler, (uint8_t)idx);
                }
            }
            break;

        case NODE_BREAK: {
            int jump_offset = emit_jump(compiler, BC_JUMP);
            compiler_record_break(compiler, jump_offset);
            break;
        }

        case NODE_CONTINUE: {
            if (compiler->loop_count > 0) {
                emit_loop(compiler, compiler->loops[compiler->loop_count - 1].loop_start);
            }
            break;
        }

        case NODE_MATCH: {
            /* Store the value in a temp local */
            compile_expression(compiler, node->match_stmt.value);
            int saved = compiler->local_count;
            int temp_idx = -1;
            if (compiler->scope_depth > 0) {
                temp_idx = compiler_add_local(compiler, "__match__");
                emit_bytes(compiler, BC_SET_LOCAL, (uint8_t)temp_idx);
                emit_byte(compiler, BC_POP);
            }

            int end_jumps[64];
            int end_count = 0;

            for (int i = 0; i < node->match_stmt.arm_count; i++) {
                AstNode *arm = node->match_stmt.arms[i];

                /* Pop the false condition from previous arm's JUMP_IF_FALSE */
                if (i > 0)
                    emit_byte(compiler, BC_POP);

                /* Load match value */
                if (temp_idx >= 0) {
                    emit_bytes(compiler, BC_GET_LOCAL, (uint8_t)temp_idx);
                } else {
                    compile_expression(compiler, node->match_stmt.value);
                }

                /* Compile pattern and compare */
                compile_expression(compiler, arm->match_arm.pattern);
                emit_byte(compiler, BC_EQUAL);

                int next_arm = emit_jump(compiler, BC_JUMP_IF_FALSE);
                emit_byte(compiler, BC_POP); /* pop bool result (true path) */

                compile_expression(compiler, arm->match_arm.body);
                end_jumps[end_count++] = emit_jump(compiler, BC_JUMP);

                patch_jump(compiler, next_arm);
            }

            /* Pop false condition from last arm */
            if (node->match_stmt.arm_count > 0)
                emit_byte(compiler, BC_POP);

            /* No arm matched: push nil */
            emit_byte(compiler, BC_NIL);

            /* Patch end jumps */
            for (int i = 0; i < end_count; i++)
                patch_jump(compiler, end_jumps[i]);

            /* Restore local count (remove __match__) */
            compiler->local_count = saved;
            break;
        }

        case NODE_STRUCT_DECL:
            /* Struct declaration — no bytecode emitted */
            break;

        case NODE_STRUCT_LITERAL: {
            for (int i = 0; i < node->struct_literal.field_count; i++)
                compile_expression(compiler, node->struct_literal.field_values[i]);
            emit_bytes(compiler, BC_STRUCT, (uint8_t)node->struct_literal.field_count);
            for (int i = 0; i < node->struct_literal.field_count; i++) {
                ObjString *s = copy_string(node->struct_literal.field_names[i],
                                           (int)strlen(node->struct_literal.field_names[i]));
                int idx = chunk_add_constant(compiler->chunk, val_string(s));
                emit_byte(compiler, (uint8_t)idx);
            }
            break;
        }

        case NODE_ENUM_DECL:
            /* Enum declaration — no bytecode emitted */
            break;

        case NODE_ENUM_LITERAL: {
            for (int i = 0; i < node->enum_literal.value_count; i++)
                compile_expression(compiler, node->enum_literal.values[i]);
            emit_bytes(compiler, BC_ENUM, (uint8_t)node->enum_literal.tag);
            emit_byte(compiler, (uint8_t)node->enum_literal.value_count);
            break;
        }

        case NODE_PROPAGATE: {
            compile_expression(compiler, node->propagate.expr);
            emit_byte(compiler, BC_PROPAGATE);
            break;
        }

        case NODE_TRY: {
            emit_byte(compiler, BC_TRY);
            int catch_pos = compiler->chunk->count;
            emit_byte(compiler, 0xFF);
            emit_byte(compiler, 0xFF);
            compile_node(compiler, node->try_stmt.try_body);
            emit_byte(compiler, BC_POP_TRY);
            int end_jump = emit_jump(compiler, BC_JUMP);
            int offset = compiler->chunk->count - catch_pos - 2;
            compiler->chunk->code[catch_pos] = (offset >> 8) & 0xFF;
            compiler->chunk->code[catch_pos + 1] = offset & 0xFF;
            if (node->try_stmt.catch_body)
                compile_node(compiler, node->try_stmt.catch_body);
            patch_jump(compiler, end_jump);
            break;
        }

        default:
            compile_expression(compiler, node);
            break;
    }
}

bool compiler_compile(Compiler *compiler) {
    compile_node(compiler, compiler->program);
    return !compiler->had_error;
}

/* ─── VM Execution ─── */

#define READ_BYTE() (*vm->frames[vm->frame_count - 1].ip++)
#define READ_SHORT() \
    (vm->frames[vm->frame_count - 1].ip += 2, \
     (uint16_t)((vm->frames[vm->frame_count - 1].ip[-2] << 8) | \
                vm->frames[vm->frame_count - 1].ip[-1]))
#define READ_CONSTANT() (vm->frames[vm->frame_count - 1].function->constants[READ_BYTE()])
#define PUSH(v) do { \
    vm->stack[vm->stack_top] = (v); \
    vm->stack_top++; \
} while (0)
#define POP() (vm->stack[--vm->stack_top])
#define PEEK(n) (vm->stack[vm->stack_top - 1 - (n)])

void vm_init(VM *vm, Compiler *compiler) {
    vm->frame_count = 0;
    vm->stack_top = 0;
    vm->objects = NULL;
    vm->global_count = 0;
    vm->compiler = compiler;
    vm->had_error = false;
    vm->main_fn = NULL;
    vm->try_count = 0;
    vm->is_throwing = false;
    vm->throw_value = val_nil();
    memset(vm->globals, 0, sizeof(vm->globals));
}

static void runtime_error(VM *vm, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, "\n");
    for (int i = vm->frame_count - 1; i >= 0; i--) {
        CallFrame *frame = &vm->frames[i];
        int offset = (int)(frame->ip - frame->function->code - 1);
        if (offset < 0) offset = 0;
        int line = 0;
        if (frame->function == vm->main_fn) {
            line = chunk_get_line(vm->compiler->chunk, offset);
        } else {
            /* Walk RLE data in the function */
            ObjFunction *fn = frame->function;
            int pos = 0;
            for (int j = 0; j < fn->rle_count; j++) {
                pos += fn->rle_counts[j];
                if (offset < pos) { line = fn->rle_lines[j]; break; }
            }
        }
        fprintf(stderr, "[line %d] in script\n", line);
    }
    vm->had_error = true;
}

static void define_global(VM *vm, ObjString *name, Value value) {
    for (int i = 0; i < vm->global_count; i++) {
        if (strcmp(vm->global_names[i], name->chars) == 0) {
            vm->globals[i] = value;
            return;
        }
    }
    if (vm->global_count >= 1024) {
        runtime_error(vm, "Too many global variables");
        return;
    }
    strncpy(vm->global_names[vm->global_count], name->chars, 63);
    vm->global_names[vm->global_count][63] = '\0';
    vm->globals[vm->global_count] = value;
    vm->global_count++;
}

static Value get_global(VM *vm, ObjString *name) {
    for (int i = 0; i < vm->global_count; i++) {
        if (strcmp(vm->global_names[i], name->chars) == 0)
            return vm->globals[i];
    }
    runtime_error(vm, "Undefined variable '%s'", name->chars);
    return val_nil();
}

static void set_global(VM *vm, ObjString *name, Value value) {
    for (int i = 0; i < vm->global_count; i++) {
        if (strcmp(vm->global_names[i], name->chars) == 0) {
            vm->globals[i] = value;
            return;
        }
    }
    runtime_error(vm, "Undefined variable '%s'", name->chars);
}

static int native_print(VM *vm) {
    Value v = POP();
    value_print(v);
    printf("\n");
    return 0;
}

static int native_throw(VM *vm) {
    vm->throw_value = POP();
    vm->is_throwing = true;
    return 0;
}

bool vm_run(VM *vm) {
    /* Create main script function (not tracked in objects list) */
    vm->main_fn = (ObjFunction *)calloc(1, sizeof(ObjFunction));
    vm->main_fn->obj.type = VAL_FUNCTION;
    vm->main_fn->code = vm->compiler->chunk->code;
    vm->main_fn->code_count = vm->compiler->chunk->count;
    vm->main_fn->code_capacity = vm->compiler->chunk->capacity;
    vm->main_fn->constants = vm->compiler->chunk->constants;
    vm->main_fn->constant_count = vm->compiler->chunk->constant_count;
    vm->main_fn->constant_capacity = vm->compiler->chunk->constant_capacity;

    CallFrame *frame = &vm->frames[vm->frame_count++];
    frame->function = vm->main_fn;
    frame->ip = vm->main_fn->code;
    frame->slots = NULL;  /* main script uses globals, not locals */

    define_global(vm, copy_string("print", 5), val_native_fn((void *)native_print));
    define_global(vm, copy_string("throw", 5), val_native_fn((void *)native_throw));

#define BINARY_OP_NUM(op) \
    do { \
        Value b = POP(); \
        Value a = POP(); \
        if (a.type == VAL_INT && b.type == VAL_INT) { \
            PUSH(val_int(a.as.integer op b.as.integer)); \
        } else { \
            double bv = (b.type == VAL_FLOAT) ? b.as.floating : (double)b.as.integer; \
            double av = (a.type == VAL_FLOAT) ? a.as.floating : (double)a.as.integer; \
            PUSH(val_float(av op bv)); \
        } \
    } while (0)

    while (!vm->had_error) {
        uint8_t instruction = READ_BYTE();

        switch (instruction) {
            case BC_CONSTANT: {
                Value constant = READ_CONSTANT();
                PUSH(constant);
                break;
            }
            case BC_CONSTANT_LONG: {
                uint16_t idx = READ_SHORT();
                Value constant = vm->frames[vm->frame_count - 1].function->constants[idx];
                PUSH(constant);
                break;
            }
            case BC_NIL:    PUSH(val_nil()); break;
            case BC_TRUE:   PUSH(val_bool(true)); break;
            case BC_FALSE:  PUSH(val_bool(false)); break;
            case BC_POP:    (void)POP(); break;

            case BC_ADD: {
                Value b = POP();
                Value a = POP();
                if (a.type == VAL_STRING && b.type == VAL_STRING) {
                    int len = a.as.string->length + b.as.string->length;
                    char *chars = (char *)malloc(len + 1);
                    memcpy(chars, a.as.string->chars, a.as.string->length);
                    memcpy(chars + a.as.string->length, b.as.string->chars, b.as.string->length);
                    chars[len] = '\0';
                    ObjString *result = allocate_string(vm, chars, len);
                    free(chars);
                    PUSH(val_string(result));
                } else if (a.type == VAL_INT && b.type == VAL_INT) {
                    PUSH(val_int(a.as.integer + b.as.integer));
                } else {
                    double bv = (b.type == VAL_FLOAT) ? b.as.floating : (double)b.as.integer;
                    double av = (a.type == VAL_FLOAT) ? a.as.floating : (double)a.as.integer;
                    PUSH(val_float(av + bv));
                }
                break;
            }
            case BC_SUB: BINARY_OP_NUM(-); break;
            case BC_MUL: BINARY_OP_NUM(*); break;
            case BC_DIV: {
                Value b = POP();
                Value a = POP();
                if (a.type == VAL_INT && b.type == VAL_INT) {
                    if (b.as.integer == 0) { runtime_error(vm, "Division by zero"); return false; }
                    PUSH(val_int(a.as.integer / b.as.integer));
                } else {
                    double bv = (b.type == VAL_FLOAT) ? b.as.floating : (double)b.as.integer;
                    double av = (a.type == VAL_FLOAT) ? a.as.floating : (double)a.as.integer;
                    if (bv == 0.0) { runtime_error(vm, "Division by zero"); return false; }
                    PUSH(val_float(av / bv));
                }
                break;
            }
            case BC_MOD: {
                int64_t b = POP().as.integer;
                int64_t a = POP().as.integer;
                if (b == 0) { runtime_error(vm, "Division by zero"); return false; }
                PUSH(val_int(a % b));
                break;
            }

            case BC_NEGATE: {
                Value v = POP();
                if (v.type == VAL_INT) PUSH(val_int(-v.as.integer));
                else if (v.type == VAL_FLOAT) PUSH(val_float(-v.as.floating));
                else { runtime_error(vm, "Operand must be a number"); return false; }
                break;
            }
            case BC_NOT: {
                Value v = POP();
                PUSH(val_bool(!value_is_truthy(v)));
                break;
            }

            case BC_EQUAL: {
                Value b = POP();
                Value a = POP();
                PUSH(val_bool(value_equal(a, b)));
                break;
            }
            case BC_NOT_EQUAL: {
                Value b = POP();
                Value a = POP();
                PUSH(val_bool(!value_equal(a, b)));
                break;
            }
            case BC_LESS:    BINARY_OP_NUM(<); break;
            case BC_GREATER: BINARY_OP_NUM(>); break;
            case BC_LESS_EQUAL:    BINARY_OP_NUM(<=); break;
            case BC_GREATER_EQUAL: BINARY_OP_NUM(>=); break;

            case BC_AND: {
                Value b = POP();
                Value a = POP();
                PUSH(val_bool(value_is_truthy(a) && value_is_truthy(b)));
                break;
            }
            case BC_OR: {
                Value b = POP();
                Value a = POP();
                PUSH(val_bool(value_is_truthy(a) || value_is_truthy(b)));
                break;
            }

            case BC_DEFINE_GLOBAL: {
                ObjString *name = READ_CONSTANT().as.string;
                Value value = PEEK(0);
                define_global(vm, name, value);
                (void)POP();
                if (getenv("VN_TRACE") && value.type == VAL_FUNCTION) {
                    fprintf(stderr, "  FN %s (%d bytes):", name->chars, value.as.function->code_count);
                    for (int i = 0; i < value.as.function->code_count; i++)
                        fprintf(stderr, " %02x", value.as.function->code[i]);
                    fprintf(stderr, " | consts=%d\n", value.as.function->constant_count);
                    for (int j = 0; j < value.as.function->constant_count; j++) {
                        if (value.as.function->constants[j].type == VAL_INT)
                            fprintf(stderr, "    const[%d] = int %ld\n", j, (long)value.as.function->constants[j].as.integer);
                    }
                }
                break;
            }
            case BC_GET_GLOBAL: {
                ObjString *name = READ_CONSTANT().as.string;
                Value value = get_global(vm, name);
                PUSH(value);
                break;
            }
            case BC_SET_GLOBAL: {
                ObjString *name = READ_CONSTANT().as.string;
                Value value = PEEK(0);
                set_global(vm, name, value);
                break;
            }

            case BC_JUMP: {
                uint16_t offset = READ_SHORT();
                vm->frames[vm->frame_count - 1].ip += offset;
                break;
            }
            case BC_JUMP_IF_FALSE: {
                uint16_t offset = READ_SHORT();
                if (!value_is_truthy(PEEK(0)))
                    vm->frames[vm->frame_count - 1].ip += offset;
                break;
            }
            case BC_LOOP: {
                uint16_t offset = READ_SHORT();
                vm->frames[vm->frame_count - 1].ip -= offset;
                break;
            }

            case BC_CALL: {
                uint8_t arg_count = READ_BYTE();
                Value callee = PEEK(arg_count);

                if (callee.type == VAL_NATIVE_FN) {
                    int (*fn)(VM *) = (int (*)(VM *))callee.as.native_fn;
                    fn(vm);
                    if (vm->is_throwing) {
                        vm->is_throwing = false;
                        if (vm->try_count > 0) {
                            vm->try_count--;
                            vm->stack_top = vm->try_stack[vm->try_count].stack_depth;
                            PUSH(vm->throw_value);
                            vm->frames[vm->frame_count - 1].ip =
                                vm->frames[vm->frame_count - 1].function->code +
                                vm->try_stack[vm->try_count].catch_offset;
                        } else {
                            runtime_error(vm, "Unhandled exception");
                            return false;
                        }
                    }
                } else if (callee.type == VAL_FUNCTION) {
                    ObjFunction *fn = callee.as.function;
                    if (fn->arity != arg_count) {
                        runtime_error(vm, "Expected %d arguments but got %d", fn->arity, arg_count);
                        return false;
                    }
                    if (vm->frame_count >= FRAMES_MAX) {
                        runtime_error(vm, "Stack overflow");
                        return false;
                    }
                    CallFrame *new_frame = &vm->frames[vm->frame_count++];
                    new_frame->function = fn;
                    new_frame->ip = fn->code;
                    /* slots points to first argument on stack */
                    new_frame->slots = &vm->stack[vm->stack_top - arg_count];
                } else {
                    runtime_error(vm, "Can only call functions");
                    return false;
                }
                break;
            }

            case BC_GET_LOCAL: {
                uint8_t local_idx = READ_BYTE();
                PUSH(vm->frames[vm->frame_count - 1].slots[local_idx]);
                break;
            }

            case BC_SET_LOCAL: {
                uint8_t local_idx = READ_BYTE();
                vm->frames[vm->frame_count - 1].slots[local_idx] = PEEK(0);
                break;
            }

            case BC_RETURN: {
                Value result = POP();
                CallFrame *frame = &vm->frames[vm->frame_count - 1];
                int arity = frame->function->arity;
                vm->frame_count--;
                if (vm->frame_count == 0) {
                    /* Main script returning - push result and halt */
                    PUSH(result);
                    goto end_vm;
                }
                /* Pop args, callee, then push result for caller */
                vm->stack_top -= (arity + 1);  /* args + callee */
                PUSH(result);
                break;
            }

            case BC_RETURN_N: {
                uint8_t rcount = READ_BYTE();
                Value *return_vals = &vm->stack[vm->stack_top - rcount];
                CallFrame *frame = &vm->frames[vm->frame_count - 1];
                int arity = frame->function->arity;
                vm->frame_count--;
                if (vm->frame_count == 0) {
                    /* Main script returning - push all values and halt */
                    for (int i = 0; i < rcount; i++)
                        PUSH(return_vals[i]);
                    goto end_vm;
                }
                /* Pop args + callee, then push all return values for caller */
                vm->stack_top -= (arity + 1);
                for (int i = 0; i < rcount; i++)
                    PUSH(return_vals[i]);
                break;
            }

            case BC_ARRAY: {
                uint8_t count = READ_BYTE();
                ObjArray *arr = allocate_array(vm);
                arr->count = count;
                arr->capacity = count;
                arr->elements = (Value *)calloc(count, sizeof(Value));
                for (int i = count - 1; i >= 0; i--)
                    arr->elements[i] = POP();
                PUSH(val_array(arr));
                break;
            }

            case BC_TUPLE: {
                uint8_t count = READ_BYTE();
                ObjTuple *tup = allocate_tuple(vm, count);
                for (int i = count - 1; i >= 0; i--)
                    tup->elements[i] = POP();
                PUSH(val_tuple(tup));
                break;
            }

            case BC_INDEX: {
                Value index = POP();
                Value obj = POP();
                if (obj.type == VAL_ARRAY && index.type == VAL_INT) {
                    int i = (int)index.as.integer;
                    if (i < 0 || i >= obj.as.array->count) {
                        runtime_error(vm, "Array index out of bounds");
                        return false;
                    }
                    PUSH(obj.as.array->elements[i]);
                } else {
                    runtime_error(vm, "Indexing not supported for this type");
                    return false;
                }
                break;
            }

            case BC_STRUCT: {
                uint8_t field_count = READ_BYTE();
                ObjStruct *s = new_struct(field_count);
                s->obj.next = vm->objects;
                vm->objects = (Obj *)s;
                for (int i = field_count - 1; i >= 0; i--)
                    s->fields[i] = POP();
                for (int i = 0; i < field_count; i++) {
                    ObjString *name = READ_CONSTANT().as.string;
                    s->field_names[i] = (char *)malloc(name->length + 1);
                    memcpy(s->field_names[i], name->chars, name->length);
                    s->field_names[i][name->length] = '\0';
                }
                PUSH(val_struct(s));
                break;
            }

            case BC_ENUM: {
                uint8_t tag = READ_BYTE();
                uint8_t value_count = READ_BYTE();
                ObjEnum *e = new_enum(value_count);
                e->obj.next = vm->objects;
                vm->objects = (Obj *)e;
                e->tag = tag;
                for (int i = value_count - 1; i >= 0; i--)
                    e->values[i] = POP();
                PUSH(val_enum(e));
                break;
            }

            case BC_PROPAGATE: {
                Value v = PEEK(0);
                if (v.type == VAL_NIL) {
                    (void)POP();
                    CallFrame *frame = &vm->frames[vm->frame_count - 1];
                    int arity = frame->function->arity;
                    vm->frame_count--;
                    if (vm->frame_count == 0) {
                        PUSH(val_nil());
                        goto end_vm;
                    }
                    vm->stack_top -= (arity + 1);
                    PUSH(val_nil());
                }
                break;
            }

            case BC_THROW: {
                Value err = POP();
                if (vm->try_count > 0) {
                    vm->try_count--;
                    vm->stack_top = vm->try_stack[vm->try_count].stack_depth;
                    PUSH(err);
                    frame->ip = vm->frames[vm->frame_count - 1].function->code +
                                vm->try_stack[vm->try_count].catch_offset;
                } else {
                    runtime_error(vm, "Unhandled exception");
                    return false;
                }
                break;
            }

            case BC_TRY: {
                uint16_t offset = READ_SHORT();
                if (vm->try_count >= MAX_TRY_NESTING) {
                    runtime_error(vm, "Too many nested try blocks");
                    return false;
                }
                CallFrame *active = &vm->frames[vm->frame_count - 1];
                vm->try_stack[vm->try_count].catch_offset =
                    (int)(active->ip - active->function->code) + offset;
                vm->try_stack[vm->try_count].stack_depth = vm->stack_top;
                vm->try_count++;
                break;
            }

            case BC_POP_TRY: {
                if (vm->try_count > 0)
                    vm->try_count--;
                break;
            }

            case BC_MEMBER: {
                ObjString *name = READ_CONSTANT().as.string;
                Value obj = POP();
                if (obj.type == VAL_STRUCT) {
                    ObjStruct *s = obj.as.structure;
                    int found = -1;
                    for (int i = 0; i < s->field_count; i++) {
                        if (strcmp(s->field_names[i], name->chars) == 0) {
                            found = i;
                            break;
                        }
                    }
                    if (found >= 0) {
                        PUSH(s->fields[found]);
                    } else {
                        runtime_error(vm, "Struct has no field '%s'", name->chars);
                        return false;
                    }
                } else {
                    runtime_error(vm, "Cannot access field on non-struct value");
                    return false;
                }
                break;
            }

            case BC_PRINT: {
                Value v = POP();
                value_print(v);
                printf("\n");
                break;
            }

            case BC_STRING_CONCAT: {
                Value b = POP();
                Value a = POP();
                if (a.type == VAL_STRING && b.type == VAL_STRING) {
                    int len = a.as.string->length + b.as.string->length;
                    char *chars = (char *)malloc(len + 1);
                    memcpy(chars, a.as.string->chars, a.as.string->length);
                    memcpy(chars + a.as.string->length, b.as.string->chars, b.as.string->length);
                    chars[len] = '\0';
                    ObjString *result = allocate_string(vm, chars, len);
                    free(chars);
                    PUSH(val_string(result));
                } else {
                    runtime_error(vm, "String concatenation requires strings");
                    return false;
                }
                break;
            }

            case BC_INT_TO_STRING: {
                Value v = POP();
                if (v.type == VAL_INT) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%ld", (long)v.as.integer);
                    ObjString *s = allocate_string(vm, buf, (int)strlen(buf));
                    PUSH(val_string(s));
                } else if (v.type == VAL_FLOAT) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%g", v.as.floating);
                    ObjString *s = allocate_string(vm, buf, (int)strlen(buf));
                    PUSH(val_string(s));
                } else if (v.type == VAL_BOOL) {
                    const char *s = v.as.boolean ? "true" : "false";
                    ObjString *str = allocate_string(vm, s, (int)strlen(s));
                    PUSH(val_string(str));
                } else {
                    PUSH(v);
                }
                break;
            }

            case BC_HALT:
                goto end_vm;

            default:
                runtime_error(vm, "Unknown opcode %d", instruction);
                return false;
        }
    }

end_vm:
    return !vm->had_error;
}

void vm_free(VM *vm) {
    /* Free main function struct (not code/constants, owned by chunk) */
    if (vm->main_fn) {
        free(vm->main_fn);
        vm->main_fn = NULL;
    }

    Obj *obj = vm->objects;
    while (obj) {
        Obj *next = obj->next;
        switch (obj->type) {
            case VAL_STRING: free(((ObjString *)obj)->chars); break;
            case VAL_ARRAY:  free(((ObjArray *)obj)->elements); break;
            case VAL_TUPLE:  free(((ObjTuple *)obj)->elements); break;
            case VAL_ENUM:   free(((ObjEnum *)obj)->values); break;
            case VAL_STRUCT: {
                ObjStruct *s = (ObjStruct *)obj;
                for (int i = 0; i < s->field_count; i++) {
                    free(s->field_names[i]);
                }
                free(s->field_names);
                free(s->fields);
                break;
            }
            case VAL_FUNCTION: {
                ObjFunction *f = (ObjFunction *)obj;
                /* Free string constants inside the function */
                for (int i = 0; i < f->constant_count; i++) {
                    if (f->constants[i].type == VAL_STRING && f->constants[i].as.string) {
                        free(f->constants[i].as.string->chars);
                        free(f->constants[i].as.string);
                    }
                }
                free(f->code);
                free(f->rle_lines);
                free(f->rle_counts);
                free(f->constants);
                break;
            }
            default: break;
        }
        free(obj);
        obj = next;
    }
    vm->objects = NULL;
}
