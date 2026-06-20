#include "vm.h"
#include "parser.h"
#include "lexer.h"
#include "ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declaration of collection helper */
static void collect_functions(Value val, ObjFunction ***funcs, int *count, int *capacity) {
    if (val.type != VAL_FUNCTION) return;
    ObjFunction *fn = val.as.function;
    
    // Check if already collected
    for (int i = 0; i < *count; i++) {
        if ((*funcs)[i] == fn) return;
    }
    
    // Add to collection
    if (*count >= *capacity) {
        *capacity = *capacity ? *capacity * 2 : 16;
        *funcs = realloc(*funcs, (size_t)(*capacity) * sizeof(ObjFunction *));
    }
    (*funcs)[*count] = fn;
    (*count)++;
    
    // Recurse into constants
    for (int i = 0; i < fn->constant_count; i++) {
        collect_functions(fn->constants[i], funcs, count, capacity);
    }
}

static void output_val_serialize(FILE *out, Value val, ObjFunction **funcs, int fn_count) {
    switch (val.type) {
        case VAL_NIL:
            fprintf(out, "val_nil()");
            break;
        case VAL_BOOL:
            fprintf(out, "val_bool(%s)", val.as.boolean ? "true" : "false");
            break;
        case VAL_INT:
            fprintf(out, "val_int(%ld)", (long)val.as.integer);
            break;
        case VAL_FLOAT:
            fprintf(out, "val_float(%g)", val.as.floating);
            break;
        case VAL_STRING: {
            fprintf(out, "val_string(copy_string(\"");
            for (int i = 0; i < val.as.string->length; i++) {
                char c = val.as.string->chars[i];
                if (c == '"') fprintf(out, "\\\"");
                else if (c == '\\') fprintf(out, "\\\\");
                else if (c == '\n') fprintf(out, "\\n");
                else if (c == '\r') fprintf(out, "\\r");
                else if (c == '\t') fprintf(out, "\\t");
                else if (c < 32 || c > 126) fprintf(out, "\\x%02x", (unsigned char)c);
                else fprintf(out, "%c", c);
            }
            fprintf(out, "\", %d))", val.as.string->length);
            break;
        }
        case VAL_FUNCTION: {
            int idx = -1;
            for (int i = 0; i < fn_count; i++) {
                if (val.as.function == funcs[i]) {
                    idx = i;
                    break;
                }
            }
            if (idx >= 0) {
                fprintf(out, "val_function(fn_%d)", idx);
            } else {
                fprintf(out, "val_nil()");
            }
            break;
        }
        default:
            fprintf(out, "val_nil()");
            break;
    }
}

int aot_compile(const char *source, const char *filename, const char *out_path) {
    Lexer lexer;
    lexer_init(&lexer, source, filename);

    Arena *arena = arena_create(0);
    Parser parser;
    parser_init(&parser, &lexer, arena);

    AstNode *program = parser_parse(&parser);
    if (parser.had_error) {
        fprintf(stderr, "AOT parse error: %s\n", parser_get_error(&parser));
        arena_destroy(arena);
        return 1;
    }

    Chunk chunk;
    chunk_init(&chunk);

    Compiler compiler;
    compiler_init(&compiler, arena, &chunk, program);

    if (!compiler_compile(&compiler)) {
        fprintf(stderr, "AOT compile error: %s\n", compiler.error_message);
        chunk_free(&chunk);
        arena_destroy(arena);
        return 1;
    }

    // Wrap main function
    ObjFunction main_fn_obj;
    memset(&main_fn_obj, 0, sizeof(ObjFunction));
    main_fn_obj.obj.type = VAL_FUNCTION;
    main_fn_obj.code = chunk.code;
    main_fn_obj.code_count = chunk.count;
    main_fn_obj.constants = chunk.constants;
    main_fn_obj.constant_count = chunk.constant_count;

    ObjFunction **funcs = NULL;
    int fn_count = 0;
    int fn_capacity = 0;

    collect_functions(val_function(&main_fn_obj), &funcs, &fn_count, &fn_capacity);

    FILE *out = fopen(out_path, "w");
    if (!out) {
        fprintf(stderr, "Could not open output file '%s' for writing.\n", out_path);
        free(funcs);
        chunk_free(&chunk);
        arena_destroy(arena);
        return 1;
    }

    // Output Header
    fprintf(out, "/* Generated Ahead-Of-Time Varian C file */\n");
    fprintf(out, "#include \"varian.h\"\n");
    fprintf(out, "#include \"vm.h\"\n");
    fprintf(out, "#include \"ast.h\"\n");
    fprintf(out, "#include \"lexer.h\"\n");
    fprintf(out, "#include <stdio.h>\n");
    fprintf(out, "#include <stdlib.h>\n");
    fprintf(out, "#include <string.h>\n\n");

    fprintf(out, "#define PUSH(v) do { t->stack[t->stack_top] = (v); t->stack_top++; } while (0)\n");
    fprintf(out, "#define POP() (t->stack[--t->stack_top])\n");
    fprintf(out, "#define PEEK(n) (t->stack[t->stack_top - 1 - (n)])\n\n");

    // Output helper function prototypes
    fprintf(out, "/* Helper prototypes */\n");
    fprintf(out, "static void aot_op_add(VM *vm, Task *t);\n");
    fprintf(out, "static void aot_op_sub(VM *vm, Task *t);\n");
    fprintf(out, "static void aot_op_mul(VM *vm, Task *t);\n");
    fprintf(out, "static void aot_op_div(VM *vm, Task *t);\n");
    fprintf(out, "static void aot_op_mod(VM *vm, Task *t);\n");
    fprintf(out, "static void aot_op_negate(VM *vm, Task *t);\n");
    fprintf(out, "static void aot_op_not(VM *vm, Task *t);\n");
    fprintf(out, "static void aot_op_equal(VM *vm, Task *t);\n");
    fprintf(out, "static void aot_op_not_equal(VM *vm, Task *t);\n");
    fprintf(out, "static void aot_op_less(VM *vm, Task *t);\n");
    fprintf(out, "static void aot_op_greater(VM *vm, Task *t);\n");
    fprintf(out, "static void aot_op_less_equal(VM *vm, Task *t);\n");
    fprintf(out, "static void aot_op_greater_equal(VM *vm, Task *t);\n");
    fprintf(out, "static void aot_helper_call(VM *vm, Task *t, int pc, uint8_t arg_count);\n");
    fprintf(out, "static void aot_helper_dispatch(VM *vm, Task *t, int pc, uint16_t name_idx, uint8_t arg_count);\n");
    fprintf(out, "static void aot_helper_ffi_call(VM *vm, Task *t, int pc, uint8_t ffi_idx, uint8_t arg_count);\n");
    fprintf(out, "static void aot_helper_throw(VM *vm, Task *t, int pc);\n\n");

    // Declare functions
    for (int i = 0; i < fn_count; i++) {
        fprintf(out, "void varian_aot_fn_%d(VM *vm, Task *t);\n", i);
    }
    fprintf(out, "\n");

    // Output Helper implementations
    fprintf(out, "/* Static Helper Implementations */\n");
    fprintf(out, "static void aot_op_add(VM *vm, Task *t) {\n");
    fprintf(out, "    Value b = POP();\n");
    fprintf(out, "    Value a = POP();\n");
    fprintf(out, "    if (a.type == VAL_STRING || b.type == VAL_STRING) {\n");
    fprintf(out, "        char buf_a[64], buf_b[64];\n");
    fprintf(out, "        const char *sa, *sb;\n");
    fprintf(out, "        int la, lb;\n");
    fprintf(out, "        if (a.type == VAL_STRING) { sa = a.as.string->chars; la = a.as.string->length; }\n");
    fprintf(out, "        else {\n");
    fprintf(out, "            switch (a.type) {\n");
    fprintf(out, "                case VAL_INT:    la = snprintf(buf_a, sizeof(buf_a), \"%%ld\", (long)a.as.integer); break;\n");
    fprintf(out, "                case VAL_FLOAT:  la = snprintf(buf_a, sizeof(buf_a), \"%%g\", a.as.floating); break;\n");
    fprintf(out, "                case VAL_BOOL:   { const char *s = a.as.boolean ? \"true\" : \"false\"; la = (int)strlen(s); memcpy(buf_a, s, la + 1); break; }\n");
    fprintf(out, "                default:         la = snprintf(buf_a, sizeof(buf_a), \"<object>\"); break;\n");
    fprintf(out, "            }\n");
    fprintf(out, "            sa = buf_a;\n");
    fprintf(out, "        }\n");
    fprintf(out, "        if (b.type == VAL_STRING) { sb = b.as.string->chars; lb = b.as.string->length; }\n");
    fprintf(out, "        else {\n");
    fprintf(out, "            switch (b.type) {\n");
    fprintf(out, "                case VAL_INT:    lb = snprintf(buf_b, sizeof(buf_b), \"%%ld\", (long)b.as.integer); break;\n");
    fprintf(out, "                case VAL_FLOAT:  lb = snprintf(buf_b, sizeof(buf_b), \"%%g\", b.as.floating); break;\n");
    fprintf(out, "                case VAL_BOOL:   { const char *s = b.as.boolean ? \"true\" : \"false\"; lb = (int)strlen(s); memcpy(buf_b, s, lb + 1); break; }\n");
    fprintf(out, "                default:         lb = snprintf(buf_b, sizeof(buf_b), \"<object>\"); break;\n");
    fprintf(out, "            }\n");
    fprintf(out, "            sb = buf_b;\n");
    fprintf(out, "        }\n");
    fprintf(out, "        int len = la + lb;\n");
    fprintf(out, "        char *chars = (char *)malloc(len + 1);\n");
    fprintf(out, "        memcpy(chars, sa, la);\n");
    fprintf(out, "        memcpy(chars + la, sb, lb);\n");
    fprintf(out, "        chars[len] = '\\0';\n");
    fprintf(out, "        ObjString *result = allocate_string(vm, chars, len);\n");
    fprintf(out, "        free(chars);\n");
    fprintf(out, "        PUSH(val_string(result));\n");
    fprintf(out, "    } else if (a.type == VAL_INT && b.type == VAL_INT) {\n");
    fprintf(out, "        PUSH(val_int(a.as.integer + b.as.integer));\n");
    fprintf(out, "    } else {\n");
    fprintf(out, "        double bv = (b.type == VAL_FLOAT) ? b.as.floating : (double)b.as.integer;\n");
    fprintf(out, "        double av = (a.type == VAL_FLOAT) ? a.as.floating : (double)a.as.integer;\n");
    fprintf(out, "        PUSH(val_float(av + bv));\n");
    fprintf(out, "    }\n");
    fprintf(out, "}\n\n");

    fprintf(out, "#define BINARY_OP_NUM_AOT(op) \\\n");
    fprintf(out, "    do { \\\n");
    fprintf(out, "        Value b = POP(); \\\n");
    fprintf(out, "        Value a = POP(); \\\n");
    fprintf(out, "        if (a.type == VAL_INT && b.type == VAL_INT) { \\\n");
    fprintf(out, "            PUSH(val_int(a.as.integer op b.as.integer)); \\\n");
    fprintf(out, "        } else { \\\n");
    fprintf(out, "            double bv = (b.type == VAL_FLOAT) ? b.as.floating : (double)b.as.integer; \\\n");
    fprintf(out, "            double av = (a.type == VAL_FLOAT) ? a.as.floating : (double)a.as.integer; \\\n");
    fprintf(out, "            PUSH(val_float(av op bv)); \\\n");
    fprintf(out, "        } \\\n");
    fprintf(out, "    } while (0)\n\n");

    fprintf(out, "static void aot_op_sub(VM *vm, Task *t) { (void)vm; BINARY_OP_NUM_AOT(-); }\n");
    fprintf(out, "static void aot_op_mul(VM *vm, Task *t) { (void)vm; BINARY_OP_NUM_AOT(*); }\n");
    fprintf(out, "static void aot_op_div(VM *vm, Task *t) {\n");
    fprintf(out, "    Value b = POP();\n");
    fprintf(out, "    Value a = POP();\n");
    fprintf(out, "    if (a.type == VAL_INT && b.type == VAL_INT) {\n");
    fprintf(out, "        if (b.as.integer == 0) { runtime_error(vm, \"Division by zero\"); return; }\n");
    fprintf(out, "        PUSH(val_int(a.as.integer / b.as.integer));\n");
    fprintf(out, "    } else {\n");
    fprintf(out, "        double bv = (b.type == VAL_FLOAT) ? b.as.floating : (double)b.as.integer;\n");
    fprintf(out, "        double av = (a.type == VAL_FLOAT) ? a.as.floating : (double)a.as.integer;\n");
    fprintf(out, "        if (bv == 0.0) { runtime_error(vm, \"Division by zero\"); return; }\n");
    fprintf(out, "        PUSH(val_float(av / bv));\n");
    fprintf(out, "    }\n");
    fprintf(out, "}\n");
    fprintf(out, "static void aot_op_mod(VM *vm, Task *t) {\n");
    fprintf(out, "    int64_t b = POP().as.integer;\n");
    fprintf(out, "    int64_t a = POP().as.integer;\n");
    fprintf(out, "    if (b == 0) { runtime_error(vm, \"Division by zero\"); return; }\n");
    fprintf(out, "    PUSH(val_int(a %% b));\n");
    fprintf(out, "}\n");
    fprintf(out, "static void aot_op_negate(VM *vm, Task *t) {\n");
    fprintf(out, "    Value v = POP();\n");
    fprintf(out, "    if (v.type == VAL_INT) PUSH(val_int(-v.as.integer));\n");
    fprintf(out, "    else if (v.type == VAL_FLOAT) PUSH(val_float(-v.as.floating));\n");
    fprintf(out, "    else { runtime_error(vm, \"Operand must be a number\"); }\n");
    fprintf(out, "}\n");
    fprintf(out, "static void aot_op_not(VM *vm, Task *t) { (void)vm; Value v = POP(); PUSH(val_bool(!value_is_truthy(v))); }\n");
    fprintf(out, "static void aot_op_equal(VM *vm, Task *t) { (void)vm; Value b = POP(); Value a = POP(); PUSH(val_bool(value_equal(a, b))); }\n");
    fprintf(out, "static void aot_op_not_equal(VM *vm, Task *t) { (void)vm; Value b = POP(); Value a = POP(); PUSH(val_bool(!value_equal(a, b))); }\n");
    fprintf(out, "static void aot_op_less(VM *vm, Task *t) { (void)vm; BINARY_OP_NUM_AOT(<); }\n");
    fprintf(out, "static void aot_op_greater(VM *vm, Task *t) { (void)vm; BINARY_OP_NUM_AOT(>); }\n");
    fprintf(out, "static void aot_op_less_equal(VM *vm, Task *t) { (void)vm; BINARY_OP_NUM_AOT(<=); }\n");
    fprintf(out, "static void aot_op_greater_equal(VM *vm, Task *t) { (void)vm; BINARY_OP_NUM_AOT(>=); }\n\n");

    // Call and dispatch helpers
    fprintf(out, "static void aot_helper_call(VM *vm, Task *t, int pc, uint8_t arg_count) {\n");
    fprintf(out, "    CallFrame *frame = &t->frames[t->frame_count - 1];\n");
    fprintf(out, "    uint8_t *code = frame->function->code;\n");
    fprintf(out, "    Value callee = PEEK(arg_count);\n");
    fprintf(out, "    if (callee.type == VAL_NATIVE_FN) {\n");
    fprintf(out, "        Value *args = &t->stack[t->stack_top - arg_count];\n");
    fprintf(out, "        NativeFn fn = (NativeFn)callee.as.native_fn;\n");
    fprintf(out, "        Value result = fn(vm, arg_count, args);\n");
    fprintf(out, "        if (t->yielded) {\n");
    fprintf(out, "            frame->ip = code + pc;\n");
    fprintf(out, "            return;\n");
    fprintf(out, "        }\n");
    fprintf(out, "        t->stack_top -= (arg_count + 1);\n");
    fprintf(out, "        PUSH(result);\n");
    fprintf(out, "        if (t->is_throwing) {\n");
    fprintf(out, "            t->is_throwing = false;\n");
    fprintf(out, "            if (t->frame_count > 0) {\n");
    fprintf(out, "                ObjFunction *caller_fn = t->frames[t->frame_count - 1].function;\n");
    fprintf(out, "                if (caller_fn->metadata.type != VAL_NIL) {\n");
    fprintf(out, "                    Value rv = metadata_get(caller_fn->metadata, \"retry\");\n");
    fprintf(out, "                    if (rv.type == VAL_INT && rv.as.integer > 0) {\n");
    fprintf(out, "                        int max_retries = (int)rv.as.integer - 1;\n");
    fprintf(out, "                        if (caller_fn->metadata.type == VAL_ARRAY) {\n");
    fprintf(out, "                            ObjArray *ma = caller_fn->metadata.as.array;\n");
    fprintf(out, "                            for (int k = 0; k < ma->count; k += 2) {\n");
    fprintf(out, "                                if (ma->elements[k].type == VAL_STRING && strcmp(ma->elements[k].as.string->chars, \"retry\") == 0) {\n");
    fprintf(out, "                                    ma->elements[k + 1] = val_int(max_retries);\n");
    fprintf(out, "                                    break;\n");
    fprintf(out, "                                }\n");
    fprintf(out, "                            }\n");
    fprintf(out, "                        }\n");
    fprintf(out, "                        t->frames[t->frame_count - 1].ip = caller_fn->code;\n");
    fprintf(out, "                    }\n");
    fprintf(out, "                }\n");
    fprintf(out, "            }\n");
    fprintf(out, "        }\n");
    fprintf(out, "        frame->ip = code + pc + 2;\n");
    fprintf(out, "    } else if (callee.type == VAL_FUNCTION) {\n");
    fprintf(out, "        ObjFunction *fn = callee.as.function;\n");
    fprintf(out, "        if (fn->arity != arg_count) {\n");
    fprintf(out, "            runtime_error(vm, \"Expected %%d arguments but got %%d\", fn->arity, arg_count);\n");
    fprintf(out, "            return;\n");
    fprintf(out, "        }\n");
    fprintf(out, "        if (t->frame_count >= TASK_FRAMES_MAX) {\n");
    fprintf(out, "            runtime_error(vm, \"Stack overflow\");\n");
    fprintf(out, "            return;\n");
    fprintf(out, "        }\n");
    fprintf(out, "        CallFrame *new_frame = &t->frames[t->frame_count++];\n");
    fprintf(out, "        new_frame->function = fn;\n");
    fprintf(out, "        new_frame->closure = NULL;\n");
    fprintf(out, "        new_frame->ip = fn->code;\n");
    fprintf(out, "        new_frame->slots = &t->stack[t->stack_top - arg_count];\n");
    fprintf(out, "        new_frame->return_base = t->stack_top - arg_count - 1;\n");
    fprintf(out, "        frame->ip = code + pc + 2;\n");
    fprintf(out, "    } else if (callee.type == VAL_CLOSURE) {\n");
    fprintf(out, "        ObjClosure *closure = callee.as.closure;\n");
    fprintf(out, "        ObjFunction *fn = closure->function;\n");
    fprintf(out, "        if (fn->arity != arg_count) {\n");
    fprintf(out, "            runtime_error(vm, \"Expected %%d arguments but got %%d\", fn->arity, arg_count);\n");
    fprintf(out, "            return;\n");
    fprintf(out, "        }\n");
    fprintf(out, "        if (t->frame_count >= TASK_FRAMES_MAX) {\n");
    fprintf(out, "            runtime_error(vm, \"Stack overflow\");\n");
    fprintf(out, "            return;\n");
    fprintf(out, "        }\n");
    fprintf(out, "        CallFrame *new_frame = &t->frames[t->frame_count++];\n");
    fprintf(out, "        new_frame->function = fn;\n");
    fprintf(out, "        new_frame->closure = closure;\n");
    fprintf(out, "        new_frame->ip = fn->code;\n");
    fprintf(out, "        new_frame->slots = &t->stack[t->stack_top - arg_count];\n");
    fprintf(out, "        new_frame->return_base = t->stack_top - arg_count - 1;\n");
    fprintf(out, "        frame->ip = code + pc + 2;\n");
    fprintf(out, "    } else {\n");
    fprintf(out, "        runtime_error(vm, \"Can only call functions\");\n");
    fprintf(out, "    }\n");
    fprintf(out, "}\n\n");

    // Dispatch helper
    fprintf(out, "static void aot_helper_dispatch(VM *vm, Task *t, int pc, uint16_t name_idx, uint8_t arg_count) {\n");
    fprintf(out, "    CallFrame *frame = &t->frames[t->frame_count - 1];\n");
    fprintf(out, "    uint8_t *code = frame->function->code;\n");
    fprintf(out, "    ObjString *method_name = frame->function->constants[name_idx].as.string;\n");
    fprintf(out, "    Value obj = PEEK(arg_count);\n");
    fprintf(out, "    if (obj.type == VAL_ACTOR) {\n");
    fprintf(out, "        ObjActor *actor = obj.as.actor;\n");
    fprintf(out, "        if (!t->waiting_actor_reply) {\n");
    fprintf(out, "            ObjChannel *reply_obj = (ObjChannel *)calloc(1, sizeof(ObjChannel));\n");
    fprintf(out, "            reply_obj->obj.type = VAL_CHANNEL;\n");
    fprintf(out, "            reply_obj->capacity = 1;\n");
    fprintf(out, "            reply_obj->buffer = (Value *)calloc(1, sizeof(Value));\n");
    fprintf(out, "            reply_obj->obj.next = vm->objects;\n");
    fprintf(out, "            vm->objects = (Obj *)reply_obj;\n");
    fprintf(out, "            Value reply_ch = val_channel(reply_obj);\n");
    fprintf(out, "            ObjString *method_str = allocate_string(vm, method_name->chars, method_name->length);\n");
    fprintf(out, "            ObjArray *args_arr = (ObjArray *)calloc(1, sizeof(ObjArray));\n");
    fprintf(out, "            args_arr->obj.type = VAL_ARRAY;\n");
    fprintf(out, "            args_arr->count = arg_count;\n");
    fprintf(out, "            args_arr->elements = (Value *)malloc(arg_count * sizeof(Value));\n");
    fprintf(out, "            for (int i = 0; i < arg_count; i++)\n");
    fprintf(out, "                args_arr->elements[i] = t->stack[t->stack_top - arg_count + i];\n");
    fprintf(out, "            args_arr->obj.next = vm->objects;\n");
    fprintf(out, "            vm->objects = (Obj *)args_arr;\n");
    fprintf(out, "            ObjTuple *msg_tuple = new_tuple(3);\n");
    fprintf(out, "            msg_tuple->obj.next = vm->objects;\n");
    fprintf(out, "            vm->objects = (Obj *)msg_tuple;\n");
    fprintf(out, "            msg_tuple->elements[0] = val_string(method_str);\n");
    fprintf(out, "            msg_tuple->elements[1] = val_array(args_arr);\n");
    fprintf(out, "            msg_tuple->elements[2] = reply_ch;\n");
    fprintf(out, "            if (!channel_try_send(actor->inbox.as.channel, val_tuple(msg_tuple))) {\n");
    fprintf(out, "                t->yielded = true;\n");
    fprintf(out, "                frame->ip = code + pc;\n");
    fprintf(out, "                return;\n");
    fprintf(out, "            }\n");
    fprintf(out, "            t->actor_reply_ch = reply_ch;\n");
    fprintf(out, "            t->waiting_actor_reply = true;\n");
    fprintf(out, "        }\n");
    fprintf(out, "        Value result;\n");
    fprintf(out, "        if (channel_try_receive(t->actor_reply_ch.as.channel, &result)) {\n");
    fprintf(out, "            t->waiting_actor_reply = false;\n");
    fprintf(out, "            t->actor_reply_ch = val_nil();\n");
    fprintf(out, "            t->stack_top -= (arg_count + 1);\n");
    fprintf(out, "            PUSH(result);\n");
    fprintf(out, "            frame->ip = code + pc + 4;\n");
    fprintf(out, "        } else {\n");
    fprintf(out, "            t->yielded = true;\n");
    fprintf(out, "            frame->ip = code + pc;\n");
    fprintf(out, "        }\n");
    fprintf(out, "        return;\n");
    fprintf(out, "    }\n");
    fprintf(out, "    const char *type_name = NULL;\n");
    fprintf(out, "    if (obj.type == VAL_STRUCT) type_name = obj.as.structure->type_name;\n");
    fprintf(out, "    else if (obj.type == VAL_MODULE) type_name = obj.as.module->name;\n");
    fprintf(out, "    else if (obj.type == VAL_STRING) type_name = \"string\";\n");
    fprintf(out, "    else if (obj.type == VAL_ARRAY) type_name = \"array\";\n");
    fprintf(out, "    else {\n");
    fprintf(out, "        runtime_error(vm, \"Cannot dispatch method on this type\");\n");
    fprintf(out, "        return;\n");
    fprintf(out, "    }\n");
    fprintf(out, "    Value *func_val = vm_find_dispatch(vm, type_name, method_name->chars);\n");
    fprintf(out, "    if (!func_val && obj.type == VAL_STRUCT) {\n");
    fprintf(out, "        ObjStruct *s = obj.as.structure;\n");
    fprintf(out, "        Value field_callee = val_nil();\n");
    fprintf(out, "        bool found_field = false;\n");
    fprintf(out, "        for (int i = 0; i < s->field_count; i++) {\n");
    fprintf(out, "            if (strcmp(s->field_names[i], method_name->chars) == 0 && (s->fields[i].type == VAL_FUNCTION || s->fields[i].type == VAL_CLOSURE)) {\n");
    fprintf(out, "                field_callee = s->fields[i];\n");
    fprintf(out, "                found_field = true;\n");
    fprintf(out, "                break;\n");
    fprintf(out, "            }\n");
    fprintf(out, "        }\n");
    fprintf(out, "        if (found_field) {\n");
    fprintf(out, "            int obj_idx = t->stack_top - arg_count - 1;\n");
    fprintf(out, "            for (int i = 0; i < arg_count; i++) {\n");
    fprintf(out, "                t->stack[obj_idx + i] = t->stack[obj_idx + 1 + i];\n");
    fprintf(out, "            }\n");
    fprintf(out, "            t->stack_top--;\n");
    fprintf(out, "            PUSH(field_callee);\n");
    fprintf(out, "            if (field_callee.type == VAL_FUNCTION) {\n");
    fprintf(out, "                ObjFunction *fn = field_callee.as.function;\n");
    fprintf(out, "                if (fn->arity != arg_count) { runtime_error(vm, \"Arity mismatch\"); return; }\n");
    fprintf(out, "                CallFrame *new_frame = &t->frames[t->frame_count++];\n");
    fprintf(out, "                new_frame->function = fn;\n");
    fprintf(out, "                new_frame->closure = NULL;\n");
    fprintf(out, "                new_frame->ip = fn->code;\n");
    fprintf(out, "                new_frame->slots = &t->stack[t->stack_top - arg_count - 1];\n");
    fprintf(out, "                new_frame->return_base = t->stack_top - arg_count - 1;\n");
    fprintf(out, "            } else {\n");
    fprintf(out, "                ObjClosure *closure = field_callee.as.closure;\n");
    fprintf(out, "                ObjFunction *fn = closure->function;\n");
    fprintf(out, "                if (fn->arity != arg_count) { runtime_error(vm, \"Arity mismatch\"); return; }\n");
    fprintf(out, "                CallFrame *new_frame = &t->frames[t->frame_count++];\n");
    fprintf(out, "                new_frame->function = fn;\n");
    fprintf(out, "                new_frame->closure = closure;\n");
    fprintf(out, "                new_frame->ip = fn->code;\n");
    fprintf(out, "                new_frame->slots = &t->stack[t->stack_top - arg_count - 1];\n");
    fprintf(out, "                new_frame->return_base = t->stack_top - arg_count - 1;\n");
    fprintf(out, "            }\n");
    fprintf(out, "            frame->ip = code + pc + 4;\n");
    fprintf(out, "            return;\n");
    fprintf(out, "        }\n");
    fprintf(out, "    }\n");
    fprintf(out, "    if (!func_val) {\n");
    fprintf(out, "        runtime_error(vm, \"No method '%%s' for type '%%s'\", method_name->chars, type_name ? type_name : \"(anonymous struct)\");\n");
    fprintf(out, "        return;\n");
    fprintf(out, "    }\n");
    fprintf(out, "    Value callee = *func_val;\n");
    fprintf(out, "    PUSH(callee);\n");
    fprintf(out, "    if (callee.type == VAL_NATIVE_FN) {\n");
    fprintf(out, "        uint8_t total = arg_count + 1;\n");
    fprintf(out, "        Value *all_args = &t->stack[t->stack_top - total - 1];\n");
    fprintf(out, "        NativeFn fn = (NativeFn)callee.as.native_fn;\n");
    fprintf(out, "        Value result = fn(vm, total, all_args);\n");
    fprintf(out, "        if (t->yielded) {\n");
    fprintf(out, "            t->stack_top--;\n");
    fprintf(out, "            frame->ip = code + pc;\n");
    fprintf(out, "            return;\n");
    fprintf(out, "        }\n");
    fprintf(out, "        t->stack_top -= (total + 1);\n");
    fprintf(out, "        PUSH(result);\n");
    fprintf(out, "        frame->ip = code + pc + 4;\n");
    fprintf(out, "    } else if (callee.type == VAL_FUNCTION) {\n");
    fprintf(out, "        ObjFunction *fn = callee.as.function;\n");
    fprintf(out, "        uint8_t total_args = arg_count + 1;\n");
    fprintf(out, "        if (fn->arity != total_args) { runtime_error(vm, \"Arity mismatch\"); return; }\n");
    fprintf(out, "        CallFrame *new_frame = &t->frames[t->frame_count++];\n");
    fprintf(out, "        new_frame->function = fn;\n");
    fprintf(out, "        new_frame->closure = NULL;\n");
    fprintf(out, "        new_frame->ip = fn->code;\n");
    fprintf(out, "        new_frame->slots = &t->stack[t->stack_top - total_args - 1];\n");
    fprintf(out, "        new_frame->return_base = t->stack_top - total_args - 1;\n");
    fprintf(out, "        frame->ip = code + pc + 4;\n");
    fprintf(out, "    } else {\n");
    fprintf(out, "        runtime_error(vm, \"Method is not callable\");\n");
    fprintf(out, "    }\n");
    fprintf(out, "}\n\n");

    // FFI Call helper
    fprintf(out, "static void aot_helper_ffi_call(VM *vm, Task *t, int pc, uint8_t ffi_idx, uint8_t arg_count) {\n");
    fprintf(out, "    CallFrame *frame = &t->frames[t->frame_count - 1];\n");
    fprintf(out, "    uint8_t *code = frame->function->code;\n");
    fprintf(out, "    if (ffi_idx >= vm->ffi_entry_count) {\n");
    fprintf(out, "        runtime_error(vm, \"Invalid FFI call index\");\n");
    fprintf(out, "        return;\n");
    fprintf(out, "    }\n");
    fprintf(out, "    VMFFIEntry *entry = &vm->ffi_entries[ffi_idx];\n");
    fprintf(out, "    if (arg_count != entry->param_count) {\n");
    fprintf(out, "        runtime_error(vm, \"FFI: expected %%d args, got %%d\", entry->param_count, arg_count);\n");
    fprintf(out, "        return;\n");
    fprintf(out, "    }\n");
    fprintf(out, "    void *value_storage[MAX_FFI_PARAMS];\n");
    fprintf(out, "    void *args[MAX_FFI_PARAMS];\n");
    fprintf(out, "    int storage_used = 0;\n");
    fprintf(out, "    for (int i = 0; i < arg_count; i++) {\n");
    fprintf(out, "        Value v = t->stack[t->stack_top - arg_count + i];\n");
    fprintf(out, "        switch (entry->param_kinds[i]) {\n");
    fprintf(out, "            case FFI_INT: {\n");
    fprintf(out, "                int32_t *p = (int32_t *)malloc(sizeof(int32_t));\n");
    fprintf(out, "                *p = (int32_t)v.as.integer;\n");
    fprintf(out, "                value_storage[storage_used++] = p;\n");
    fprintf(out, "                args[i] = p;\n");
    fprintf(out, "                break;\n");
    fprintf(out, "            }\n");
    fprintf(out, "            case FFI_DOUBLE: {\n");
    fprintf(out, "                double *p = (double *)malloc(sizeof(double));\n");
    fprintf(out, "                *p = (v.type == VAL_FLOAT) ? v.as.floating : (double)v.as.integer;\n");
    fprintf(out, "                value_storage[storage_used++] = p;\n");
    fprintf(out, "                args[i] = p;\n");
    fprintf(out, "                break;\n");
    fprintf(out, "            }\n");
    fprintf(out, "            case FFI_FLOAT: {\n");
    fprintf(out, "                float *p = (float *)malloc(sizeof(float));\n");
    fprintf(out, "                *p = (float)v.as.floating;\n");
    fprintf(out, "                value_storage[storage_used++] = p;\n");
    fprintf(out, "                args[i] = p;\n");
    fprintf(out, "                break;\n");
    fprintf(out, "            }\n");
    fprintf(out, "            case FFI_CHAR: {\n");
    fprintf(out, "                char *p = (char *)malloc(sizeof(char));\n");
    fprintf(out, "                *p = (char)v.as.integer;\n");
    fprintf(out, "                value_storage[storage_used++] = p;\n");
    fprintf(out, "                args[i] = p;\n");
    fprintf(out, "                break;\n");
    fprintf(out, "            }\n");
    fprintf(out, "            case FFI_VOID:\n");
    fprintf(out, "                args[i] = NULL;\n");
    fprintf(out, "                break;\n");
    fprintf(out, "            case FFI_PTR: {\n");
    fprintf(out, "                void **slot = (void **)malloc(sizeof(void *));\n");
    fprintf(out, "                if (v.type == VAL_INT) {\n");
    fprintf(out, "                    *slot = (void *)(uintptr_t)v.as.integer;\n");
    fprintf(out, "                } else if (v.type == VAL_STRING) {\n");
    fprintf(out, "                    size_t slen = (size_t)v.as.string->length;\n");
    fprintf(out, "                    char *copy = (char *)malloc(slen + 1);\n");
    fprintf(out, "                    memcpy(copy, v.as.string->chars, slen);\n");
    fprintf(out, "                    copy[slen] = '\\0';\n");
    fprintf(out, "                    value_storage[storage_used++] = copy;\n");
    fprintf(out, "                    *slot = copy;\n");
    fprintf(out, "                } else {\n");
    fprintf(out, "                    *slot = NULL;\n");
    fprintf(out, "                }\n");
    fprintf(out, "                value_storage[storage_used++] = slot;\n");
    fprintf(out, "                args[i] = slot;\n");
    fprintf(out, "                break;\n");
    fprintf(out, "            }\n");
    fprintf(out, "        }\n");
    fprintf(out, "    }\n");
    fprintf(out, "    Value result = val_nil();\n");
    fprintf(out, "    switch (entry->return_kind) {\n");
    fprintf(out, "        case FFI_VOID: {\n");
    fprintf(out, "            ffi_call(&entry->cif, FFI_FN(entry->fn_ptr), NULL, args);\n");
    fprintf(out, "            break;\n");
    fprintf(out, "        }\n");
    fprintf(out, "        case FFI_INT: {\n");
    fprintf(out, "            int32_t ret_val;\n");
    fprintf(out, "            ffi_call(&entry->cif, FFI_FN(entry->fn_ptr), &ret_val, args);\n");
    fprintf(out, "            result = val_int(ret_val);\n");
    fprintf(out, "            break;\n");
    fprintf(out, "        }\n");
    fprintf(out, "        case FFI_DOUBLE: {\n");
    fprintf(out, "            double ret_val;\n");
    fprintf(out, "            ffi_call(&entry->cif, FFI_FN(entry->fn_ptr), &ret_val, args);\n");
    fprintf(out, "            result = val_float(ret_val);\n");
    fprintf(out, "            break;\n");
    fprintf(out, "        }\n");
    fprintf(out, "        case FFI_FLOAT: {\n");
    fprintf(out, "            float ret_val;\n");
    fprintf(out, "            ffi_call(&entry->cif, FFI_FN(entry->fn_ptr), &ret_val, args);\n");
    fprintf(out, "            result = val_float(ret_val);\n");
    fprintf(out, "            break;\n");
    fprintf(out, "        }\n");
    fprintf(out, "        case FFI_PTR: {\n");
    fprintf(out, "            void *ret_val = NULL;\n");
    fprintf(out, "            ffi_call(&entry->cif, FFI_FN(entry->fn_ptr), &ret_val, args);\n");
    fprintf(out, "            result = val_int((int64_t)(uintptr_t)ret_val);\n");
    fprintf(out, "            break;\n");
    fprintf(out, "        }\n");
    fprintf(out, "        case FFI_CHAR: {\n");
    fprintf(out, "            char ret_val;\n");
    fprintf(out, "            ffi_call(&entry->cif, FFI_FN(entry->fn_ptr), &ret_val, args);\n");
    fprintf(out, "            result = val_int(ret_val);\n");
    fprintf(out, "            break;\n");
    fprintf(out, "        }\n");
    fprintf(out, "    }\n");
    fprintf(out, "    for (int i = 0; i < storage_used; i++) free(value_storage[i]);\n");
    fprintf(out, "    t->stack_top -= arg_count;\n");
    fprintf(out, "    PUSH(result);\n");
    fprintf(out, "    frame->ip = code + pc + 3;\n");
    fprintf(out, "}\n\n");

    // Throw helper
    fprintf(out, "static void aot_helper_throw(VM *vm, Task *t, int pc) {\n");
    fprintf(out, "    CallFrame *frame = &t->frames[t->frame_count - 1];\n");
    fprintf(out, "    uint8_t *code = frame->function->code;\n");
    fprintf(out, "    Value err = POP();\n");
    fprintf(out, "    if (t->frame_count > 0) {\n");
    fprintf(out, "        ObjFunction *cur_fn = t->frames[t->frame_count - 1].function;\n");
    fprintf(out, "        if (cur_fn->metadata.type != VAL_NIL) {\n");
    fprintf(out, "            Value rv = metadata_get(cur_fn->metadata, \"retry\");\n");
    fprintf(out, "            if (rv.type == VAL_INT && rv.as.integer > 0) {\n");
    fprintf(out, "                int max_retries = (int)rv.as.integer - 1;\n");
    fprintf(out, "                if (cur_fn->metadata.type == VAL_ARRAY) {\n");
    fprintf(out, "                    ObjArray *ma = cur_fn->metadata.as.array;\n");
    fprintf(out, "                    for (int i = 0; i < ma->count; i += 2) {\n");
    fprintf(out, "                        if (ma->elements[i].type == VAL_STRING && strcmp(ma->elements[i].as.string->chars, \"retry\") == 0) {\n");
    fprintf(out, "                            ma->elements[i + 1] = val_int(max_retries);\n");
    fprintf(out, "                            break;\n");
    fprintf(out, "                        }\n");
    fprintf(out, "                    }\n");
    fprintf(out, "                }\n");
    fprintf(out, "                t->frames[t->frame_count - 1].ip = cur_fn->code;\n");
    fprintf(out, "                t->is_throwing = false;\n");
    fprintf(out, "                return;\n");
    fprintf(out, "            }\n");
    fprintf(out, "        }\n");
    fprintf(out, "    }\n");
    fprintf(out, "    if (t->try_count > 0) {\n");
    fprintf(out, "        t->try_count--;\n");
    fprintf(out, "        int target_frame = t->try_stack[t->try_count].frame_index;\n");
    fprintf(out, "        t->frame_count = target_frame + 1;\n");
    fprintf(out, "        t->stack_top = t->try_stack[t->try_count].stack_depth;\n");
    fprintf(out, "        PUSH(err);\n");
    fprintf(out, "        t->frames[target_frame].ip = t->frames[target_frame].function->code + t->try_stack[t->try_count].catch_offset;\n");
    fprintf(out, "    } else {\n");
    fprintf(out, "        t->throw_value = err;\n");
    fprintf(out, "        t->is_throwing = true;\n");
    fprintf(out, "        t->dead = true;\n");
    fprintf(out, "    }\n");
    fprintf(out, "}\n\n");

    // Output all functions
    for (int f = 0; f < fn_count; f++) {
        ObjFunction *fn = funcs[f];
        fprintf(out, "/* Function %d */\n", f);
        fprintf(out, "void varian_aot_fn_%d(VM *vm, Task *t) {\n", f);
        fprintf(out, "    CallFrame *frame = &t->frames[t->frame_count - 1];\n");
        fprintf(out, "    uint8_t *code = frame->function->code;\n");
        fprintf(out, "    int pc = (int)(frame->ip - code);\n\n");

        // Resumption Switch
        fprintf(out, "    switch (pc) {\n");
        int offset = 0;
        while (offset < fn->code_count) {
            fprintf(out, "        case %d: goto lbl_%d;\n", offset, offset);
            // Skip instruction bytes
            int op = fn->code[offset];
            int size = 1;
            if (op == BC_CONSTANT || op == BC_CONSTANT_LONG || op == BC_DEFINE_GLOBAL ||
                op == BC_GET_GLOBAL || op == BC_SET_GLOBAL || op == BC_JUMP ||
                op == BC_JUMP_IF_FALSE || op == BC_JUMP_IF_NOT_NIL || op == BC_JUMP_IF_NIL || op == BC_LOOP ||
                op == BC_MAKE_FUNCTION || op == BC_TRY || op == BC_MEMBER || op == BC_SET_MEMBER) {
                size = 3;
            } else if (op == BC_GET_LOCAL || op == BC_SET_LOCAL || op == BC_CALL ||
                       op == BC_RETURN_N || op == BC_GET_UPVALUE || op == BC_SET_UPVALUE ||
                       op == BC_CLOSURE || op == BC_ARRAY || op == BC_TUPLE ||
                       op == BC_TAG_EQ || op == BC_BUILD_STRING) {
                size = 2;
            } else if (op == BC_COMPTIME_EXEC || op == BC_REGISTER_METHOD) {
                size = 5;
            } else if (op == BC_DISPATCH) {
                size = 4;
            } else if (op == BC_FFI_CALL) {
                size = 3;
            } else if (op == BC_ENUM) {
                size = 3;
            } else if (op == BC_STRUCT) {
                uint8_t fc = fn->code[offset + 1];
                size = 4 + 2 * fc;
            } else if (op == BC_ACTOR_INIT) {
                uint8_t fc = fn->code[offset + 3];
                size = 4 + 2 * fc;
            } else if (op == BC_REGISTER_VALIDATIONS) {
                int off = offset + 1;
                // type_name idx (2)
                off += 2;
                // struct validation count (1)
                uint8_t svc = fn->code[off++];
                off += svc * 4;
                // field_count (1)
                uint8_t fc = fn->code[off++];
                for (int i = 0; i < fc; i++) {
                    off += 2; // fname idx
                    uint8_t fvc = fn->code[off++]; // fvc
                    off += fvc * 4;
                }
                size = off - offset;
            }
            offset += size;
        }
        fprintf(out, "        default: runtime_error(vm, \"AOT: invalid resumption PC %%d\", pc); return;\n");
        fprintf(out, "    }\n\n");

        // Code Blocks
        offset = 0;
        while (offset < fn->code_count) {
            int op = fn->code[offset];
            fprintf(out, "    lbl_%d:;\n", offset);
            
            // Generate logic for each instruction
            switch (op) {
                case BC_CONSTANT:
                case BC_CONSTANT_LONG: {
                    uint16_t idx = (fn->code[offset + 1] << 8) | fn->code[offset + 2];
                    fprintf(out, "        PUSH(frame->function->constants[%d]);\n", idx);
                    offset += 3;
                    break;
                }
                case BC_NIL:
                    fprintf(out, "        PUSH(val_nil());\n");
                    offset += 1;
                    break;
                case BC_TRUE:
                    fprintf(out, "        PUSH(val_bool(true));\n");
                    offset += 1;
                    break;
                case BC_FALSE:
                    fprintf(out, "        PUSH(val_bool(false));\n");
                    offset += 1;
                    break;
                case BC_POP:
                    fprintf(out, "        (void)POP();\n");
                    offset += 1;
                    break;
                case BC_ADD:
                    fprintf(out, "        aot_op_add(vm, t);\n");
                    offset += 1;
                    break;
                case BC_SUB:
                    fprintf(out, "        aot_op_sub(vm, t);\n");
                    offset += 1;
                    break;
                case BC_MUL:
                    fprintf(out, "        aot_op_mul(vm, t);\n");
                    offset += 1;
                    break;
                case BC_DIV:
                    fprintf(out, "        aot_op_div(vm, t);\n");
                    offset += 1;
                    break;
                case BC_MOD:
                    fprintf(out, "        aot_op_mod(vm, t);\n");
                    offset += 1;
                    break;
                case BC_NEGATE:
                    fprintf(out, "        aot_op_negate(vm, t);\n");
                    offset += 1;
                    break;
                case BC_NOT:
                    fprintf(out, "        aot_op_not(vm, t);\n");
                    offset += 1;
                    break;
                case BC_EQUAL:
                    fprintf(out, "        aot_op_equal(vm, t);\n");
                    offset += 1;
                    break;
                case BC_NOT_EQUAL:
                    fprintf(out, "        aot_op_not_equal(vm, t);\n");
                    offset += 1;
                    break;
                case BC_LESS:
                    fprintf(out, "        aot_op_less(vm, t);\n");
                    offset += 1;
                    break;
                case BC_GREATER:
                    fprintf(out, "        aot_op_greater(vm, t);\n");
                    offset += 1;
                    break;
                case BC_LESS_EQUAL:
                    fprintf(out, "        aot_op_less_equal(vm, t);\n");
                    offset += 1;
                    break;
                case BC_GREATER_EQUAL:
                    fprintf(out, "        aot_op_greater_equal(vm, t);\n");
                    offset += 1;
                    break;
                case BC_AND: {
                    fprintf(out, "        {\n");
                    fprintf(out, "            Value a = POP();\n");
                    fprintf(out, "            Value b = POP();\n");
                    fprintf(out, "            PUSH(val_bool(value_is_truthy(a) && value_is_truthy(b)));\n");
                    fprintf(out, "        }\n");
                    offset += 1;
                    break;
                }
                case BC_OR: {
                    fprintf(out, "        {\n");
                    fprintf(out, "            Value a = POP();\n");
                    fprintf(out, "            Value b = POP();\n");
                    fprintf(out, "            PUSH(val_bool(value_is_truthy(a) || value_is_truthy(b)));\n");
                    fprintf(out, "        }\n");
                    offset += 1;
                    break;
                }
                case BC_NIL_COALESCE: {
                    fprintf(out, "        {\n");
                    fprintf(out, "            Value b = POP();\n");
                    fprintf(out, "            Value a = POP();\n");
                    fprintf(out, "            PUSH(a.type == VAL_NIL ? b : a);\n");
                    fprintf(out, "        }\n");
                    offset += 1;
                    break;
                }
                case BC_JUMP_IF_NOT_NIL: {
                    uint16_t j_offset = (fn->code[offset + 1] << 8) | fn->code[offset + 2];
                    fprintf(out, "        if (PEEK(0).type != VAL_NIL) { goto lbl_%d; }\n", offset + 3 + j_offset);
                    offset += 3;
                    break;
                }
                case BC_DEFINE_GLOBAL: {
                    uint16_t idx = (fn->code[offset + 1] << 8) | fn->code[offset + 2];
                    fprintf(out, "        define_global(vm, frame->function->constants[%d].as.string, PEEK(0)); (void)POP();\n", idx);
                    offset += 3;
                    break;
                }
                case BC_GET_GLOBAL: {
                    uint16_t idx = (fn->code[offset + 1] << 8) | fn->code[offset + 2];
                    fprintf(out, "        PUSH(get_global(vm, frame->function->constants[%d].as.string));\n", idx);
                    offset += 3;
                    break;
                }
                case BC_SET_GLOBAL: {
                    uint16_t idx = (fn->code[offset + 1] << 8) | fn->code[offset + 2];
                    fprintf(out, "        set_global(vm, frame->function->constants[%d].as.string, PEEK(0));\n", idx);
                    offset += 3;
                    break;
                }
                case BC_GET_LOCAL: {
                    uint8_t local_idx = fn->code[offset + 1];
                    fprintf(out, "        PUSH(frame->slots[%d]);\n", local_idx);
                    offset += 2;
                    break;
                }
                case BC_SET_LOCAL: {
                    uint8_t local_idx = fn->code[offset + 1];
                    fprintf(out, "        frame->slots[%d] = PEEK(0);\n", local_idx);
                    offset += 2;
                    break;
                }
                case BC_JUMP: {
                    uint16_t j_offset = (fn->code[offset + 1] << 8) | fn->code[offset + 2];
                    fprintf(out, "        goto lbl_%d;\n", offset + 3 + j_offset);
                    offset += 3;
                    break;
                }
                case BC_JUMP_IF_FALSE: {
                    uint16_t j_offset = (fn->code[offset + 1] << 8) | fn->code[offset + 2];
                    fprintf(out, "        if (!value_is_truthy(PEEK(0))) { goto lbl_%d; }\n", offset + 3 + j_offset);
                    offset += 3;
                    break;
                }
                case BC_JUMP_IF_NIL: {
                    uint16_t j_offset = (fn->code[offset + 1] << 8) | fn->code[offset + 2];
                    fprintf(out, "        if (PEEK(0).type == VAL_NIL) { goto lbl_%d; }\n", offset + 3 + j_offset);
                    offset += 3;
                    break;
                }
                case BC_LOOP: {
                    uint16_t j_offset = (fn->code[offset + 1] << 8) | fn->code[offset + 2];
                    fprintf(out, "        goto lbl_%d;\n", offset + 3 - j_offset);
                    offset += 3;
                    break;
                }
                case BC_CALL: {
                    uint8_t arg_count = fn->code[offset + 1];
                    fprintf(out, "        aot_helper_call(vm, t, %d, %d);\n", offset, arg_count);
                    fprintf(out, "        return;\n");
                    offset += 2;
                    break;
                }
                case BC_RETURN: {
                    fprintf(out, "        {\n");
                    fprintf(out, "            Value result = POP();\n");
                    fprintf(out, "            if (t->cache_on_return) {\n");
                    fprintf(out, "                cache_map_put(vm, t->cache_result_key, result);\n");
                    fprintf(out, "                t->cache_on_return = false;\n");
                    fprintf(out, "            }\n");
                    fprintf(out, "            CallFrame *curr_frame = &t->frames[t->frame_count - 1];\n");
                    fprintf(out, "            int base = curr_frame->return_base;\n");
                    fprintf(out, "            t->frame_count--;\n");
                    fprintf(out, "            if (t->frame_count == 0) {\n");
                    fprintf(out, "                t->result = result;\n");
                    fprintf(out, "                t->dead = true;\n");
                    fprintf(out, "                PUSH(result);\n");
                    fprintf(out, "                return;\n");
                    fprintf(out, "            }\n");
                    fprintf(out, "            t->stack_top = base;\n");
                    fprintf(out, "            PUSH(result);\n");
                    fprintf(out, "            return;\n");
                    fprintf(out, "        }\n");
                    offset += 1;
                    break;
                }
                case BC_RETURN_N: {
                    uint8_t rcount = fn->code[offset + 1];
                    fprintf(out, "        {\n");
                    fprintf(out, "            uint8_t rcount = %d;\n", rcount);
                    fprintf(out, "            Value tmp_vals[16];\n");
                    fprintf(out, "            int copy_count = rcount < 16 ? rcount : 16;\n");
                    fprintf(out, "            for (int i = 0; i < copy_count; i++)\n");
                    fprintf(out, "                tmp_vals[i] = t->stack[t->stack_top - rcount + i];\n");
                    fprintf(out, "            CallFrame *curr_frame = &t->frames[t->frame_count - 1];\n");
                    fprintf(out, "            int base = curr_frame->return_base;\n");
                    fprintf(out, "            t->frame_count--;\n");
                    fprintf(out, "            if (t->frame_count == 0) {\n");
                    fprintf(out, "                t->result = (copy_count > 0) ? tmp_vals[0] : val_nil();\n");
                    fprintf(out, "                t->dead = true;\n");
                    fprintf(out, "                for (int i = 0; i < copy_count; i++)\n");
                    fprintf(out, "                    PUSH(tmp_vals[i]);\n");
                    fprintf(out, "                return;\n");
                    fprintf(out, "            }\n");
                    fprintf(out, "            t->stack_top = base;\n");
                    fprintf(out, "            for (int i = 0; i < copy_count; i++)\n");
                    fprintf(out, "                PUSH(tmp_vals[i]);\n");
                    fprintf(out, "            return;\n");
                    fprintf(out, "        }\n");
                    offset += 2;
                    break;
                }
                case BC_GET_UPVALUE: {
                    uint8_t idx = fn->code[offset + 1];
                    fprintf(out, "        PUSH(frame->closure->captured[%d]);\n", idx);
                    offset += 2;
                    break;
                }
                case BC_SET_UPVALUE: {
                    uint8_t idx = fn->code[offset + 1];
                    fprintf(out, "        frame->closure->captured[%d] = PEEK(0);\n", idx);
                    offset += 2;
                    break;
                }
                case BC_CLOSURE: {
                    uint8_t upvalue_count = fn->code[offset + 1];
                    fprintf(out, "        {\n");
                    fprintf(out, "            ObjClosure *closure = new_closure(NULL, %d);\n", upvalue_count);
                    fprintf(out, "            closure->obj.next = vm->objects;\n");
                    fprintf(out, "            vm->objects = (Obj*)closure;\n");
                    fprintf(out, "            for (int i = %d - 1; i >= 0; i--) {\n", upvalue_count);
                    fprintf(out, "                closure->captured[i] = POP();\n");
                    fprintf(out, "            }\n");
                    fprintf(out, "            Value fn_val = POP();\n");
                    fprintf(out, "            closure->function = fn_val.as.function;\n");
                    fprintf(out, "            PUSH(val_closure(closure));\n");
                    fprintf(out, "        }\n");
                    offset += 2;
                    break;
                }
                case BC_ARRAY: {
                    uint8_t count = fn->code[offset + 1];
                    fprintf(out, "        {\n");
                    fprintf(out, "            ObjArray *a = new_array();\n");
                    fprintf(out, "            a->obj.next = vm->objects;\n");
                    fprintf(out, "            vm->objects = (Obj *)a;\n");
                    fprintf(out, "            a->elements = (Value *)malloc(%d * sizeof(Value));\n", count);
                    fprintf(out, "            a->capacity = %d;\n", count);
                    fprintf(out, "            for (int i = %d - 1; i >= 0; i--) {\n", count);
                    fprintf(out, "                a->elements[i] = POP();\n");
                    fprintf(out, "                a->count++;\n");
                    fprintf(out, "            }\n");
                    fprintf(out, "            PUSH(val_array(a));\n");
                    fprintf(out, "        }\n");
                    offset += 2;
                    break;
                }
                case BC_TUPLE: {
                    uint8_t count = fn->code[offset + 1];
                    fprintf(out, "        {\n");
                    fprintf(out, "            ObjTuple *tup = new_tuple(%d);\n", count);
                    fprintf(out, "            tup->obj.next = vm->objects;\n");
                    fprintf(out, "            vm->objects = (Obj *)tup;\n");
                    fprintf(out, "            for (int i = %d - 1; i >= 0; i--)\n", count);
                    fprintf(out, "                tup->elements[i] = POP();\n");
                    fprintf(out, "            PUSH(val_tuple(tup));\n");
                    fprintf(out, "        }\n");
                    offset += 2;
                    break;
                }
                case BC_INDEX:
                    fprintf(out, "        {\n");
                    fprintf(out, "            Value idx = POP();\n");
                    fprintf(out, "            Value obj = POP();\n");
                    fprintf(out, "            if (obj.type == VAL_ARRAY) {\n");
                    fprintf(out, "                int i = (int)idx.as.integer;\n");
                    fprintf(out, "                if (i < 0 || i >= obj.as.array->count) { runtime_error(vm, \"Index out of bounds\"); return; }\n");
                    fprintf(out, "                PUSH(obj.as.array->elements[i]);\n");
                    fprintf(out, "            } else if (obj.type == VAL_TUPLE) {\n");
                    fprintf(out, "                int i = (int)idx.as.integer;\n");
                    fprintf(out, "                if (i < 0 || i >= obj.as.tuple->count) { runtime_error(vm, \"Index out of bounds\"); return; }\n");
                    fprintf(out, "                PUSH(obj.as.tuple->elements[i]);\n");
                    fprintf(out, "            } else {\n");
                    fprintf(out, "                runtime_error(vm, \"Cannot index non-indexable value\");\n");
                    fprintf(out, "            }\n");
                    fprintf(out, "        }\n");
                    offset += 1;
                    break;
                case BC_SET_INDEX:
                    fprintf(out, "        {\n");
                    fprintf(out, "            Value val = POP();\n");
                    fprintf(out, "            Value idx = POP();\n");
                    fprintf(out, "            Value obj = POP();\n");
                    fprintf(out, "            if (obj.type == VAL_ARRAY) {\n");
                    fprintf(out, "                int i = (int)idx.as.integer;\n");
                    fprintf(out, "                if (i < 0 || i >= obj.as.array->count) { runtime_error(vm, \"Index out of bounds\"); return; }\n");
                    fprintf(out, "                obj.as.array->elements[i] = val;\n");
                    fprintf(out, "                PUSH(val);\n");
                    fprintf(out, "            } else {\n");
                    fprintf(out, "                runtime_error(vm, \"Cannot assign by index to this type\");\n");
                    fprintf(out, "            }\n");
                    fprintf(out, "        }\n");
                    offset += 1;
                    break;
                case BC_MEMBER: {
                    uint16_t idx = (fn->code[offset + 1] << 8) | fn->code[offset + 2];
                    fprintf(out, "        {\n");
                    fprintf(out, "            ObjString *name = frame->function->constants[%d].as.string;\n", idx);
                    fprintf(out, "            Value obj = POP();\n");
                    fprintf(out, "            if (obj.type == VAL_STRUCT) {\n");
                    fprintf(out, "                ObjStruct *s = obj.as.structure;\n");
                    fprintf(out, "                int found = -1;\n");
                    fprintf(out, "                uint32_t name_hash = hash_string(name->chars, name->length);\n");
                    fprintf(out, "                for (int ci = 0; ci < s->field_cache_count; ci++) {\n");
                    fprintf(out, "                    if (s->field_cache[ci].hash == name_hash) {\n");
                    fprintf(out, "                        found = s->field_cache[ci].index;\n");
                    fprintf(out, "                        break;\n");
                    fprintf(out, "                    }\n");
                    fprintf(out, "                }\n");
                    fprintf(out, "                if (found < 0) {\n");
                    fprintf(out, "                    for (int i = 0; i < s->field_count; i++) {\n");
                    fprintf(out, "                        if (strcmp(s->field_names[i], name->chars) == 0) {\n");
                    fprintf(out, "                            found = i;\n");
                    fprintf(out, "                            if (s->field_cache_count < STRUCT_CACHE_SIZE) {\n");
                    fprintf(out, "                                s->field_cache[s->field_cache_count].hash = name_hash;\n");
                    fprintf(out, "                                s->field_cache[s->field_cache_count].index = i;\n");
                    fprintf(out, "                                s->field_cache_count++;\n");
                    fprintf(out, "                            }\n");
                    fprintf(out, "                            break;\n");
                    fprintf(out, "                        }\n");
                    fprintf(out, "                    }\n");
                    fprintf(out, "                }\n");
                    fprintf(out, "                if (found >= 0) {\n");
                    fprintf(out, "                    PUSH(s->fields[found]);\n");
                    fprintf(out, "                } else {\n");
                    fprintf(out, "                    runtime_error(vm, \"Struct has no field '%%s'\", name->chars);\n");
                    fprintf(out, "                }\n");
                    fprintf(out, "            } else if (obj.type == VAL_MODULE) {\n");
                    fprintf(out, "                Value *func_val = vm_find_dispatch(vm, obj.as.module->name, name->chars);\n");
                    fprintf(out, "                if (func_val) { PUSH(*func_val); } else { runtime_error(vm, \"Module '%%s' has no member '%%s'\", obj.as.module->name, name->chars); }\n");
                    fprintf(out, "            } else {\n");
                    fprintf(out, "                runtime_error(vm, \"Member access on non-object type\");\n");
                    fprintf(out, "            }\n");
                    fprintf(out, "        }\n");
                    offset += 3;
                    break;
                }
                case BC_SET_MEMBER: {
                    uint16_t idx = (fn->code[offset + 1] << 8) | fn->code[offset + 2];
                    fprintf(out, "        {\n");
                    fprintf(out, "            ObjString *name = frame->function->constants[%d].as.string;\n", idx);
                    fprintf(out, "            Value val = POP();\n");
                    fprintf(out, "            Value obj = POP();\n");
                    fprintf(out, "            if (obj.type == VAL_STRUCT) {\n");
                    fprintf(out, "                ObjStruct *s = obj.as.structure;\n");
                    fprintf(out, "                int found = -1;\n");
                    fprintf(out, "                uint32_t name_hash = hash_string(name->chars, name->length);\n");
                    fprintf(out, "                for (int ci = 0; ci < s->field_cache_count; ci++) {\n");
                    fprintf(out, "                    if (s->field_cache[ci].hash == name_hash) {\n");
                    fprintf(out, "                        found = s->field_cache[ci].index;\n");
                    fprintf(out, "                        break;\n");
                    fprintf(out, "                    }\n");
                    fprintf(out, "                }\n");
                    fprintf(out, "                if (found < 0) {\n");
                    fprintf(out, "                    for (int i = 0; i < s->field_count; i++) {\n");
                    fprintf(out, "                        if (strcmp(s->field_names[i], name->chars) == 0) {\n");
                    fprintf(out, "                            found = i;\n");
                    fprintf(out, "                            if (s->field_cache_count < STRUCT_CACHE_SIZE) {\n");
                    fprintf(out, "                                s->field_cache[s->field_cache_count].hash = name_hash;\n");
                    fprintf(out, "                                s->field_cache[s->field_cache_count].index = i;\n");
                    fprintf(out, "                                s->field_cache_count++;\n");
                    fprintf(out, "                            }\n");
                    fprintf(out, "                            break;\n");
                    fprintf(out, "                        }\n");
                    fprintf(out, "                    }\n");
                    fprintf(out, "                }\n");
                    fprintf(out, "                if (found >= 0) {\n");
                    fprintf(out, "                    s->fields[found] = val;\n");
                    fprintf(out, "                    PUSH(val);\n");
                    fprintf(out, "                } else {\n");
                    fprintf(out, "                    runtime_error(vm, \"Struct has no field '%%s'\", name->chars);\n");
                    fprintf(out, "                }\n");
                    fprintf(out, "            } else {\n");
                    fprintf(out, "                runtime_error(vm, \"Cannot set field on non-struct value\");\n");
                    fprintf(out, "            }\n");
                    fprintf(out, "        }\n");
                    offset += 3;
                    break;
                }
                case BC_DISPATCH: {
                    uint16_t name_idx = (fn->code[offset + 1] << 8) | fn->code[offset + 2];
                    uint8_t arg_count = fn->code[offset + 3];
                    fprintf(out, "        aot_helper_dispatch(vm, t, %d, %d, %d);\n", offset, name_idx, arg_count);
                    fprintf(out, "        return;\n");
                    offset += 4;
                    break;
                }
                case BC_REGISTER_METHOD: {
                    uint16_t t_idx = (fn->code[offset + 1] << 8) | fn->code[offset + 2];
                    uint16_t m_idx = (fn->code[offset + 3] << 8) | fn->code[offset + 4];
                    fprintf(out, "        {\n");
                    fprintf(out, "            ObjString *tname = frame->function->constants[%d].as.string;\n", t_idx);
                    fprintf(out, "            ObjString *mname = frame->function->constants[%d].as.string;\n", m_idx);
                    fprintf(out, "            vm_register_dispatch(vm, tname->chars, mname->chars, PEEK(0));\n");
                    fprintf(out, "        }\n");
                    offset += 5;
                    break;
                }
                case BC_STRUCT: {
                    uint8_t field_count = fn->code[offset + 1];
                    uint16_t type_name_idx = (fn->code[offset + 2] << 8) | fn->code[offset + 3];
                    fprintf(out, "        {\n");
                    fprintf(out, "            uint8_t field_count = %d;\n", field_count);
                    fprintf(out, "            ObjString *type_name_str = frame->function->constants[%d].as.string;\n", type_name_idx);
                    fprintf(out, "            ObjStruct *s = new_struct(vm, field_count, false);\n");
                    fprintf(out, "            s->type_name = (char *)malloc(type_name_str->length + 1);\n");
                    fprintf(out, "            memcpy(s->type_name, type_name_str->chars, type_name_str->length);\n");
                    fprintf(out, "            s->type_name[type_name_str->length] = '\\0';\n");
                    fprintf(out, "            for (int i = field_count - 1; i >= 0; i--)\n");
                    fprintf(out, "                s->fields[i] = POP();\n");
                    
                    int off = offset + 4;
                    for (int i = 0; i < field_count; i++) {
                        uint16_t f_idx = (fn->code[off] << 8) | fn->code[off + 1];
                        fprintf(out, "            {\n");
                        fprintf(out, "                ObjString *name = frame->function->constants[%d].as.string;\n", f_idx);
                        fprintf(out, "                s->field_names[%d] = (char *)malloc(name->length + 1);\n", i);
                        fprintf(out, "                memcpy(s->field_names[%d], name->chars, name->length);\n", i);
                        fprintf(out, "                s->field_names[%d][name->length] = '\\0';\n", i);
                        fprintf(out, "            }\n");
                        off += 2;
                    }
                    
                    fprintf(out, "            bool has_validations = false;\n");
                    fprintf(out, "            ValidationRegistry *reg = &vm->validation_registry;\n");
                    fprintf(out, "            for (int r = 0; r < reg->count; r++) {\n");
                    fprintf(out, "                if (strcmp(reg->validations[r].type_name, s->type_name) == 0) {\n");
                    fprintf(out, "                    StructValidationInfo *info = &reg->validations[r];\n");
                    fprintf(out, "                    s->struct_validations = info->struct_validations;\n");
                    fprintf(out, "                    s->struct_validation_count = info->struct_validation_count;\n");
                    fprintf(out, "                    if (info->struct_validation_count > 0) has_validations = true;\n");
                    fprintf(out, "                    for (int i = 0; i < field_count; i++) {\n");
                    fprintf(out, "                        for (int j = 0; j < info->field_count; j++) {\n");
                    fprintf(out, "                            if (strcmp(s->field_names[i], info->field_names[j]) == 0) {\n");
                    fprintf(out, "                                s->field_validations[i] = info->field_validations[j];\n");
                    fprintf(out, "                                s->field_validation_counts[i] = info->field_validation_counts[j];\n");
                    fprintf(out, "                                if (info->field_validation_counts[j] > 0) has_validations = true;\n");
                    fprintf(out, "                                break;\n");
                    fprintf(out, "                            }\n");
                    fprintf(out, "                        }\n");
                    fprintf(out, "                    }\n");
                    fprintf(out, "                    break;\n");
                    fprintf(out, "                }\n");
                    fprintf(out, "            }\n");
                    fprintf(out, "            if (has_validations) {\n");
                    fprintf(out, "                if (!run_struct_validations(vm, s)) return;\n");
                    fprintf(out, "            }\n");
                    fprintf(out, "            PUSH(val_struct(s));\n");
                    fprintf(out, "        }\n");
                    
                    offset = off;
                    break;
                }
                case BC_ENUM: {
                    uint8_t tag = fn->code[offset + 1];
                    uint8_t value_count = fn->code[offset + 2];
                    fprintf(out, "        {\n");
                    fprintf(out, "            ObjEnum *e = new_enum(%d);\n", value_count);
                    fprintf(out, "            e->obj.next = vm->objects;\n");
                    fprintf(out, "            vm->objects = (Obj *)e;\n");
                    fprintf(out, "            e->tag = %d;\n", tag);
                    fprintf(out, "            for (int i = %d - 1; i >= 0; i--)\n", value_count);
                    fprintf(out, "                e->values[i] = POP();\n");
                    fprintf(out, "            PUSH(val_enum(e));\n");
                    fprintf(out, "        }\n");
                    offset += 3;
                    break;
                }
                case BC_TAG_EQ: {
                    uint8_t tag = fn->code[offset + 1];
                    fprintf(out, "        {\n");
                    fprintf(out, "            Value v = POP();\n");
                    fprintf(out, "            if (v.type == VAL_ENUM) PUSH(val_bool(v.as.enum_val->tag == %d));\n", tag);
                    fprintf(out, "            else PUSH(val_bool(false));\n");
                    fprintf(out, "        }\n");
                    offset += 2;
                    break;
                }
                case BC_UNPACK_ENUM:
                    fprintf(out, "        {\n");
                    fprintf(out, "            Value v = POP();\n");
                    fprintf(out, "            if (v.type == VAL_ENUM) {\n");
                    fprintf(out, "                ObjEnum *e = v.as.enum_val;\n");
                    fprintf(out, "                for (int i = 0; i < e->count; i++) PUSH(e->values[i]);\n");
                    fprintf(out, "            } else {\n");
                    fprintf(out, "                PUSH(val_nil());\n");
                    fprintf(out, "            }\n");
                    fprintf(out, "        }\n");
                    offset += 1;
                    break;
                case BC_PROPAGATE:
                    fprintf(out, "        {\n");
                    fprintf(out, "            Value v = PEEK(0);\n");
                    fprintf(out, "            if (v.type == VAL_NIL) {\n");
                    fprintf(out, "                (void)POP();\n");
                    fprintf(out, "                int arity = frame->function->arity;\n");
                    fprintf(out, "                t->frame_count--;\n");
                    fprintf(out, "                if (t->frame_count == 0) {\n");
                    fprintf(out, "                    PUSH(val_nil());\n");
                    fprintf(out, "                    return;\n");
                    fprintf(out, "                }\n");
                    fprintf(out, "                t->stack_top -= (arity + 1);\n");
                    fprintf(out, "                PUSH(val_nil());\n");
                    fprintf(out, "                return;\n");
                    fprintf(out, "            }\n");
                    fprintf(out, "        }\n");
                    offset += 1;
                    break;
                case BC_THROW:
                    fprintf(out, "        aot_helper_throw(vm, t, %d);\n", offset);
                    fprintf(out, "        return;\n");
                    offset += 1;
                    break;
                case BC_TRY: {
                    uint16_t t_offset = (fn->code[offset + 1] << 8) | fn->code[offset + 2];
                    fprintf(out, "        if (t->try_count >= TASK_TRY_MAX) { runtime_error(vm, \"Too many nested try blocks\"); return; }\n");
                    fprintf(out, "        t->try_stack[t->try_count].catch_offset = %d;\n", offset + 3 + t_offset);
                    fprintf(out, "        t->try_stack[t->try_count].stack_depth = t->stack_top;\n");
                    fprintf(out, "        t->try_stack[t->try_count].frame_index = t->frame_count - 1;\n");
                    fprintf(out, "        t->try_count++;\n");
                    offset += 3;
                    break;
                }
                case BC_POP_TRY:
                    fprintf(out, "        if (t->try_count > 0) t->try_count--;\n");
                    offset += 1;
                    break;
                case BC_FFI_CALL: {
                    uint8_t ffi_idx = fn->code[offset + 1];
                    uint8_t arg_count = fn->code[offset + 2];
                    fprintf(out, "        aot_helper_ffi_call(vm, t, %d, %d, %d);\n", offset, ffi_idx, arg_count);
                    offset += 3;
                    break;
                }
                case BC_COMPTIME_EXEC: {
                    uint16_t result_idx = (fn->code[offset + 1] << 8) | fn->code[offset + 2];
                    fprintf(out, "        PUSH(frame->function->constants[%d]);\n", result_idx);
                    offset += 5;
                    break;
                }
                case BC_AWAIT:
                    fprintf(out, "        {\n");
                    fprintf(out, "            Value v = POP();\n");
                    fprintf(out, "            if (v.type != VAL_TASK) { runtime_error(vm, \"await requires task\"); return; }\n");
                    fprintf(out, "            ObjTask *ot = v.as.task_obj;\n");
                    fprintf(out, "            if (ot->task->dead) { PUSH(ot->task->result); } else {\n");
                    fprintf(out, "                PUSH(v);\n");
                    fprintf(out, "                t->yielded = true;\n");
                    fprintf(out, "                frame->ip = code + %d;\n", offset);
                    fprintf(out, "            }\n");
                    fprintf(out, "        }\n");
                    offset += 1;
                    break;
                case BC_CHAN_SEND:
                    fprintf(out, "        {\n");
                    fprintf(out, "            Value val = POP();\n");
                    fprintf(out, "            Value chan_v = POP();\n");
                    fprintf(out, "            if (chan_v.type != VAL_CHANNEL) { runtime_error(vm, \"send requires channel\"); return; }\n");
                    fprintf(out, "            ObjChannel *ch = chan_v.as.channel;\n");
                    fprintf(out, "            if (ch->closed) { runtime_error(vm, \"send on closed channel\"); return; }\n");
                    fprintf(out, "            if (ch->count < ch->capacity) {\n");
                    fprintf(out, "                ch->buffer[ch->tail] = val;\n");
                    fprintf(out, "                ch->tail = (ch->tail + 1) %% ch->capacity;\n");
                    fprintf(out, "                ch->count++;\n");
                    fprintf(out, "                PUSH(val_nil());\n");
                    fprintf(out, "            } else {\n");
                    fprintf(out, "                PUSH(chan_v);\n");
                    fprintf(out, "                PUSH(val);\n");
                    fprintf(out, "                t->yielded = true;\n");
                    fprintf(out, "                frame->ip = code + %d;\n", offset);
                    fprintf(out, "            }\n");
                    fprintf(out, "        }\n");
                    offset += 1;
                    break;
                case BC_CHAN_RECEIVE:
                    fprintf(out, "        {\n");
                    fprintf(out, "            Value chan_v = POP();\n");
                    fprintf(out, "            if (chan_v.type != VAL_CHANNEL) { runtime_error(vm, \"receive requires channel\"); return; }\n");
                    fprintf(out, "            ObjChannel *ch = chan_v.as.channel;\n");
                    fprintf(out, "            if (ch->count > 0) {\n");
                    fprintf(out, "                Value res = ch->buffer[ch->head];\n");
                    fprintf(out, "                ch->head = (ch->head + 1) %% ch->capacity;\n");
                    fprintf(out, "                ch->count--;\n");
                    fprintf(out, "                PUSH(res);\n");
                    fprintf(out, "            } else if (ch->closed) {\n");
                    fprintf(out, "                PUSH(val_nil());\n");
                    fprintf(out, "            } else {\n");
                    fprintf(out, "                PUSH(chan_v);\n");
                    fprintf(out, "                t->yielded = true;\n");
                    fprintf(out, "                frame->ip = code + %d;\n", offset);
                    fprintf(out, "            }\n");
                    fprintf(out, "        }\n");
                    offset += 1;
                    break;
                case BC_ACTOR_INIT: {
                    uint16_t type_name_idx = (fn->code[offset + 1] << 8) | fn->code[offset + 2];
                    uint8_t field_count = fn->code[offset + 3];
                    fprintf(out, "        {\n");
                    fprintf(out, "            ObjString *type_name = frame->function->constants[%d].as.string;\n", type_name_idx);
                    fprintf(out, "            if (vm->actor_field_count < MAX_ACTOR_TYPES) {\n");
                    fprintf(out, "                ActorFieldInfo *info = &vm->actor_fields[vm->actor_field_count++];\n");
                    fprintf(out, "                strncpy(info->type_name, type_name->chars, 63);\n");
                    fprintf(out, "                info->type_name[63] = '\\0';\n");
                    fprintf(out, "                info->field_count = %d;\n", field_count);
                    
                    int off = offset + 4;
                    for (int i = 0; i < field_count; i++) {
                        uint16_t f_idx = (fn->code[off] << 8) | fn->code[off + 1];
                        fprintf(out, "                {\n");
                        fprintf(out, "                    ObjString *fname = frame->function->constants[%d].as.string;\n", f_idx);
                        fprintf(out, "                    strncpy(info->field_names[%d], fname->chars, 63);\n", i);
                        fprintf(out, "                    info->field_names[%d][63] = '\\0';\n", i);
                        fprintf(out, "                }\n");
                        off += 2;
                    }
                    
                    fprintf(out, "            }\n");
                    fprintf(out, "            ObjModule *mod = new_module(type_name->chars);\n");
                    fprintf(out, "            mod->obj.next = vm->objects;\n");
                    fprintf(out, "            vm->objects = (Obj *)mod;\n");
                    fprintf(out, "            define_global(vm, copy_string(type_name->chars, type_name->length), val_module(mod));\n");
                    fprintf(out, "            vm_register_dispatch(vm, type_name->chars, \"spawn\", val_native_fn((void *)actor_spawn_native));\n");
                    fprintf(out, "        }\n");
                    
                    offset = offset + 4 + 2 * field_count;
                    break;
                }
                case BC_PRINT:
                    fprintf(out, "        {\n");
                    fprintf(out, "            Value v = POP();\n");
                    fprintf(out, "            value_print(v); printf(\"\\n\");\n");
                    fprintf(out, "            PUSH(val_nil());\n");
                    fprintf(out, "        }\n");
                    offset += 1;
                    break;
                case BC_STRING_CONCAT:
                    fprintf(out, "        aot_op_add(vm, t);\n"); // reusing addition for dynamic string concatenation
                    offset += 1;
                    break;
                case BC_BUILD_STRING: {
                    uint8_t count = fn->code[offset + 1];
                    fprintf(out, "        {\n");
                    fprintf(out, "            int count = %d;\n", count);
                    fprintf(out, "            Value *parts = &t->stack[t->stack_top - count];\n");
                    fprintf(out, "            int total_len = 0;\n");
                    fprintf(out, "            char buf[64];\n");
                    fprintf(out, "            for (int i = 0; i < count; i++) {\n");
                    fprintf(out, "                if (parts[i].type == VAL_STRING) total_len += parts[i].as.string->length;\n");
                    fprintf(out, "                else if (parts[i].type == VAL_INT) total_len += snprintf(buf, sizeof(buf), \"%%ld\", (long)parts[i].as.integer);\n");
                    fprintf(out, "                else if (parts[i].type == VAL_FLOAT) total_len += snprintf(buf, sizeof(buf), \"%%g\", parts[i].as.floating);\n");
                    fprintf(out, "                else if (parts[i].type == VAL_BOOL) total_len += parts[i].as.boolean ? 4 : 5;\n");
                    fprintf(out, "                else total_len += 8;\n");
                    fprintf(out, "            }\n");
                    fprintf(out, "            char *res_str = (char *)malloc(total_len + 1);\n");
                    fprintf(out, "            int curr_pos = 0;\n");
                    fprintf(out, "            for (int i = 0; i < count; i++) {\n");
                    fprintf(out, "                if (parts[i].type == VAL_STRING) {\n");
                    fprintf(out, "                    memcpy(res_str + curr_pos, parts[i].as.string->chars, parts[i].as.string->length);\n");
                    fprintf(out, "                    curr_pos += parts[i].as.string->length;\n");
                    fprintf(out, "                } else {\n");
                    fprintf(out, "                    int written = 0;\n");
                    fprintf(out, "                    if (parts[i].type == VAL_INT) written = snprintf(res_str + curr_pos, total_len - curr_pos + 1, \"%%ld\", (long)parts[i].as.integer);\n");
                    fprintf(out, "                    else if (parts[i].type == VAL_FLOAT) written = snprintf(res_str + curr_pos, total_len - curr_pos + 1, \"%%g\", parts[i].as.floating);\n");
                    fprintf(out, "                    else if (parts[i].type == VAL_BOOL) {\n");
                    fprintf(out, "                        const char *s = parts[i].as.boolean ? \"true\" : \"false\";\n");
                    fprintf(out, "                        written = strlen(s); memcpy(res_str + curr_pos, s, written);\n");
                    fprintf(out, "                    } else {\n");
                    fprintf(out, "                        const char *s = \"<object>\"; written = strlen(s); memcpy(res_str + curr_pos, s, written);\n");
                    fprintf(out, "                    }\n");
                    fprintf(out, "                    curr_pos += written;\n");
                    fprintf(out, "                }\n");
                    fprintf(out, "            }\n");
                    fprintf(out, "            res_str[total_len] = '\\0';\n");
                    fprintf(out, "            ObjString *os = allocate_string(vm, res_str, total_len);\n");
                    fprintf(out, "            free(res_str);\n");
                    fprintf(out, "            t->stack_top -= count;\n");
                    fprintf(out, "            PUSH(val_string(os));\n");
                    fprintf(out, "        }\n");
                    offset += 2;
                    break;
                }
                case BC_INT_TO_STRING:
                    fprintf(out, "        {\n");
                    fprintf(out, "            Value v = POP();\n");
                    fprintf(out, "            char buf[64];\n");
                    fprintf(out, "            int len = snprintf(buf, sizeof(buf), \"%%ld\", (long)v.as.integer);\n");
                    fprintf(out, "            PUSH(val_string(copy_string(buf, len)));\n");
                    fprintf(out, "        }\n");
                    offset += 1;
                    break;
                case BC_ASSERT:
                    fprintf(out, "        {\n");
                    fprintf(out, "            Value v = POP();\n");
                    fprintf(out, "            if (!value_is_truthy(v)) { runtime_error(vm, \"Assertion failed\"); return; }\n");
                    fprintf(out, "        }\n");
                    offset += 1;
                    break;
                case BC_HALT:
                    fprintf(out, "        t->dead = true; return;\n");
                    offset += 1;
                    break;
                case BC_REGISTER_VALIDATIONS: {
                    uint16_t type_name_idx = (fn->code[offset + 1] << 8) | fn->code[offset + 2];
                    fprintf(out, "        {\n");
                    fprintf(out, "            ObjString *type_name_str = frame->function->constants[%d].as.string;\n", type_name_idx);
                    fprintf(out, "            char *type_name = (char *)malloc(type_name_str->length + 1);\n");
                    fprintf(out, "            memcpy(type_name, type_name_str->chars, type_name_str->length);\n");
                    fprintf(out, "            type_name[type_name_str->length] = '\\0';\n");
                    
                    int off = offset + 3;
                    uint8_t struct_validation_count = fn->code[off++];
                    fprintf(out, "            uint8_t struct_validation_count = %d;\n", struct_validation_count);
                    fprintf(out, "            ValidationRule *struct_validations = NULL;\n");
                    if (struct_validation_count > 0) {
                        fprintf(out, "            struct_validations = (ValidationRule *)calloc(struct_validation_count, sizeof(ValidationRule));\n");
                        for (int i = 0; i < struct_validation_count; i++) {
                            uint16_t key_idx = (fn->code[off] << 8) | fn->code[off + 1];
                            uint16_t arg_idx = (fn->code[off + 2] << 8) | fn->code[off + 3];
                            fprintf(out, "            {\n");
                            fprintf(out, "                ObjString *key = frame->function->constants[%d].as.string;\n", key_idx);
                            fprintf(out, "                struct_validations[%d].rule_name = (char *)malloc(key->length + 1);\n", i);
                            fprintf(out, "                memcpy(struct_validations[%d].rule_name, key->chars, key->length);\n", i);
                            fprintf(out, "                struct_validations[%d].rule_name[key->length] = '\\0';\n", i);
                            fprintf(out, "                struct_validations[%d].rule_args = (Value *)malloc(sizeof(Value));\n", i);
                            fprintf(out, "                struct_validations[%d].rule_args[0] = frame->function->constants[%d];\n", i, arg_idx);
                            fprintf(out, "                struct_validations[%d].rule_arg_count = 1;\n", i);
                            fprintf(out, "            }\n");
                            off += 4;
                        }
                    }
                    
                    uint8_t field_count = fn->code[off++];
                    fprintf(out, "            uint8_t field_count = %d;\n", field_count);
                    fprintf(out, "            ValidationRule **field_validations = (ValidationRule **)calloc(field_count, sizeof(ValidationRule *));\n");
                    fprintf(out, "            int *field_validation_counts = (int *)calloc(field_count, sizeof(int));\n");
                    fprintf(out, "            char **field_names = (char **)calloc(field_count, sizeof(char *));\n");
                    
                    for (int i = 0; i < field_count; i++) {
                        uint16_t fname_idx = (fn->code[off] << 8) | fn->code[off + 1];
                        off += 2;
                        uint8_t fcount = fn->code[off++];
                        fprintf(out, "            {\n");
                        fprintf(out, "                ObjString *fname = frame->function->constants[%d].as.string;\n", fname_idx);
                        fprintf(out, "                field_names[%d] = (char *)malloc(fname->length + 1);\n", i);
                        fprintf(out, "                memcpy(field_names[%d], fname->chars, fname->length);\n", i);
                        fprintf(out, "                field_names[%d][fname->length] = '\\0';\n", i);
                        fprintf(out, "                field_validation_counts[%d] = %d;\n", i, fcount);
                        if (fcount > 0) {
                            fprintf(out, "                field_validations[%d] = (ValidationRule *)calloc(%d, sizeof(ValidationRule));\n", i, fcount);
                            for (int j = 0; j < fcount; j++) {
                                uint16_t fkey_idx = (fn->code[off] << 8) | fn->code[off + 1];
                                uint16_t farg_idx = (fn->code[off + 2] << 8) | fn->code[off + 3];
                                fprintf(out, "                {\n");
                                fprintf(out, "                    ObjString *fkey = frame->function->constants[%d].as.string;\n", fkey_idx);
                                fprintf(out, "                    field_validations[%d][%d].rule_name = (char *)malloc(fkey->length + 1);\n", i, j);
                                fprintf(out, "                    memcpy(field_validations[%d][%d].rule_name, fkey->chars, fkey->length);\n", i, j);
                                fprintf(out, "                    field_validations[%d][%d].rule_name[fkey->length] = '\\0';\n", i, j);
                                fprintf(out, "                    field_validations[%d][%d].rule_args = (Value *)malloc(sizeof(Value));\n", i, j);
                                fprintf(out, "                    field_validations[%d][%d].rule_args[0] = frame->function->constants[%d];\n", i, j, farg_idx);
                                fprintf(out, "                    field_validations[%d][%d].rule_arg_count = 1;\n", i, j);
                                fprintf(out, "                }\n");
                                off += 4;
                            }
                        }
                        fprintf(out, "            }\n");
                    }
                    
                    fprintf(out, "            ValidationRegistry *reg = &vm->validation_registry;\n");
                    fprintf(out, "            if (reg->count < MAX_STRUCT_VALIDATIONS) {\n");
                    fprintf(out, "                StructValidationInfo *info = &reg->validations[reg->count++];\n");
                    fprintf(out, "                strncpy(info->type_name, type_name, 63);\n");
                    fprintf(out, "                info->type_name[63] = '\\0';\n");
                    fprintf(out, "                info->field_count = field_count;\n");
                    fprintf(out, "                info->struct_validations = struct_validations;\n");
                    fprintf(out, "                info->struct_validation_count = struct_validation_count;\n");
                    fprintf(out, "                info->field_validations = field_validations;\n");
                    fprintf(out, "                info->field_validation_counts = field_validation_counts;\n");
                    fprintf(out, "                info->field_names = field_names;\n");
                    fprintf(out, "            } else {\n");
                    fprintf(out, "                for (int i = 0; i < struct_validation_count; i++) { free(struct_validations[i].rule_name); free(struct_validations[i].rule_args); }\n");
                    fprintf(out, "                free(struct_validations);\n");
                    fprintf(out, "                for (int i = 0; i < field_count; i++) {\n");
                    fprintf(out, "                    free(field_names[i]);\n");
                    fprintf(out, "                    if (field_validations[i]) {\n");
                    fprintf(out, "                        for (int j = 0; j < field_validation_counts[i]; j++) { free(field_validations[i][j].rule_name); free(field_validations[i][j].rule_args); }\n");
                    fprintf(out, "                        free(field_validations[i]);\n");
                    fprintf(out, "                    }\n");
                    fprintf(out, "                }\n");
                    fprintf(out, "            }\n");
                    fprintf(out, "            free(type_name);\n");
                    fprintf(out, "        }\n");
                    
                    offset = off;
                    break;
                }
                default:
                    fprintf(out, "        /* Unknown/unused opcode %d */\n", op);
                    offset += 1;
                    break;
            }
            
            // Output IP sync
            fprintf(out, "        frame->ip = code + %d;\n", offset);
        }
        
        fprintf(out, "}\n\n");
    }

    // Output bytecode and constants static initialization arrays
    fprintf(out, "/* Static byte arrays for function bytecodes */\n");
    for (int i = 0; i < fn_count; i++) {
        ObjFunction *fn = funcs[i];
        fprintf(out, "static const uint8_t fn_code_%d[%d] = {\n    ", i, fn->code_count);
        for (int j = 0; j < fn->code_count; j++) {
            fprintf(out, "0x%02x, ", fn->code[j]);
            if (j % 16 == 15) fprintf(out, "\n    ");
        }
        fprintf(out, "\n};\n\n");
    }

    // Loader Function
    fprintf(out, "ObjFunction *varian_aot_load(VM *vm) {\n");
    
    // Instantiate all function objects
    for (int i = 0; i < fn_count; i++) {
        fprintf(out, "    ObjFunction *fn_%d = (ObjFunction *)calloc(1, sizeof(ObjFunction));\n", i);
        fprintf(out, "    fn_%d->obj.type = VAL_FUNCTION;\n", i);
        fprintf(out, "    fn_%d->arity = %d;\n", i, funcs[i]->arity);
        fprintf(out, "    fn_%d->code = (uint8_t *)fn_code_%d;\n", i, i);
        fprintf(out, "    fn_%d->code_count = %d;\n", i, funcs[i]->code_count);
        fprintf(out, "    fn_%d->aot_func = (AotFunc)varian_aot_fn_%d;\n", i, i);
    }
    fprintf(out, "\n");

    // Populate FFI Declarations into a new compiler struct
    fprintf(out, "    Compiler *comp = (Compiler *)calloc(1, sizeof(Compiler));\n");
    fprintf(out, "    vm->compiler = comp;\n");
    fprintf(out, "    comp->ffi_decl_count = %d;\n", compiler.ffi_decl_count);
    for (int i = 0; i < compiler.ffi_decl_count; i++) {
        FFIDecl *decl = &compiler.ffi_decls[i];
        fprintf(out, "    strcpy(comp->ffi_decls[%d].lib_name, \"%s\");\n", i, decl->lib_name);
        fprintf(out, "    strcpy(comp->ffi_decls[%d].func_name, \"%s\");\n", i, decl->func_name);
        fprintf(out, "    comp->ffi_decls[%d].return_kind = %d;\n", i, decl->return_kind);
        fprintf(out, "    comp->ffi_decls[%d].param_count = %d;\n", i, decl->param_count);
        for (int j = 0; j < decl->param_count; j++) {
            fprintf(out, "    comp->ffi_decls[%d].param_kinds[%d] = %d;\n", i, j, decl->param_kinds[j]);
        }
    }
    fprintf(out, "\n");

    // Recreate constants table for each function
    for (int i = 0; i < fn_count; i++) {
        ObjFunction *fn = funcs[i];
        if (fn->constant_count > 0) {
            fprintf(out, "    fn_%d->constants = (Value *)calloc(%d, sizeof(Value));\n", i, fn->constant_count);
            fprintf(out, "    fn_%d->constant_count = %d;\n", i, fn->constant_count);
            for (int j = 0; j < fn->constant_count; j++) {
                fprintf(out, "    fn_%d->constants[%d] = ", i, j);
                output_val_serialize(out, fn->constants[j], funcs, fn_count);
                fprintf(out, ";\n");
            }
        }
    }
    fprintf(out, "\n");

    // Add FFI resolver triggers or manual FFI resolution
    fprintf(out, "    return fn_0;\n");
    fprintf(out, "}\n");

    fclose(out);
    free(funcs);
    chunk_free(&chunk);
    arena_destroy(arena);
    return 0;
}
