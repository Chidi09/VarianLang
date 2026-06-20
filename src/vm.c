#include "vm.h"
#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdarg.h>
#include <sys/time.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>



/* ─── Forward declarations ─── */
static void compile_node(Compiler *compiler, AstNode *node);
Value *vm_find_dispatch(VM *vm, const char *type_name, const char *method_name);

/* ─── Validation Registry ───
 * Population happens at runtime via BC_REGISTER_VALIDATIONS (emitted by
 * compile_node for NODE_STRUCT_DECL); see the opcode handler in vm_run. */
static bool decorator_literal_to_value(AstNode *node, Value *out) {
    switch (node->kind) {
        case NODE_INT_LITERAL:    *out = val_int(node->literal.int_value); return true;
        case NODE_FLOAT_LITERAL:  *out = val_float(node->literal.float_value); return true;
        case NODE_BOOL_LITERAL:   *out = val_bool(node->literal.bool_value); return true;
        case NODE_STRING_LITERAL: {
            const char *s = node->literal.string_value ? node->literal.string_value : "";
            *out = val_string(copy_string(s, (int)strlen(s)));
            return true;
        }
        default: return false;
    }
}

static bool call_validate_function(VM *vm, const char *rule_name, Value value, Value *args, int arg_count) {
    Value validate_mod = vm->globals[0];
    for (int i = 0; i < vm->global_count; i++) {
        if (strcmp(vm->global_names[i], "validate") == 0) {
            validate_mod = vm->globals[i];
            break;
        }
    }
    if (validate_mod.type != VAL_MODULE) return false;

    ObjModule *mod = validate_mod.as.module;
    Value *func_val = vm_find_dispatch(vm, mod->name, rule_name);
    if (!func_val || func_val->type != VAL_NATIVE_FN) return false;

    NativeFn fn = (NativeFn)func_val->as.native_fn;
    Value all_args[8];
    all_args[0] = value;
    for (int i = 0; i < arg_count && i < 7; i++) {
        all_args[i + 1] = args[i];
    }
    Value result = fn(vm, arg_count + 1, all_args);
    return result.type == VAL_BOOL && result.as.boolean;
}

bool run_struct_validations(VM *vm, ObjStruct *s) {
    /* Run struct-level validations, passing the whole struct as the value */
    for (int i = 0; i < s->struct_validation_count; i++) {
        ValidationRule *rule = &s->struct_validations[i];
        if (!call_validate_function(vm, rule->rule_name, val_struct(s), rule->rule_args, rule->rule_arg_count)) {
            runtime_error(vm, "Struct-level validation failed for '%s': %s", s->type_name, rule->rule_name);
            return false;
        }
    }

    /* Run field-level validations */
    for (int i = 0; i < s->field_count; i++) {
        int vcount = s->field_validation_counts ? s->field_validation_counts[i] : 0;
        if (vcount > 0 && s->field_validations && s->field_validations[i]) {
            Value field_value = s->fields[i];
            for (int j = 0; j < vcount; j++) {
                ValidationRule *rule = &s->field_validations[i][j];
                if (!call_validate_function(vm, rule->rule_name, field_value, rule->rule_args, rule->rule_arg_count)) {
                    runtime_error(vm, "Validation failed for field '%s': %s", s->field_names[i], rule->rule_name);
                    return false;
                }
            }
        }
    }
    return true;
}

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

Value val_closure(ObjClosure *c) {
    Value v;
    v.type = VAL_CLOSURE;
    v.as.closure = c;
    return v;
}

Value val_enum(ObjEnum *e) {
    Value v;
    v.type = VAL_ENUM;
    v.as.enum_val = e;
    return v;
}

Value val_module(ObjModule *m) {
    Value v;
    v.type = VAL_MODULE;
    v.as.module = m;
    return v;
}

Value val_task_obj(ObjTask *t) {
    Value v;
    v.type = VAL_TASK;
    v.as.task_obj = t;
    return v;
}

Value val_channel(ObjChannel *c) {
    Value v;
    v.type = VAL_CHANNEL;
    v.as.channel = c;
    return v;
}

Value val_actor(ObjActor *a) {
    Value v;
    v.type = VAL_ACTOR;
    v.as.actor = a;
    return v;
}

/* ─── Object Allocation ─── */
static void gc_collect(VM *vm);

/* Phase 2: per-request arena — general-purpose bump allocator routed from
 * the task's arena block when use_arena is true. Falls back to malloc when
 * the arena is exhausted or not enabled. */
void *task_arena_alloc(Task *t, size_t size) {
    if (t && t->use_arena && t->arena_base) {
        size_t aligned = (size + 7) & ~(size_t)7;
        if (t->arena_offset + aligned <= TASK_ARENA_SIZE) {
            void *ptr = t->arena_base + t->arena_offset;
            t->arena_offset += aligned;
            return ptr;
        }
    }
    return malloc(size);
}

void task_arena_enable(Task *t) {
    if (!t->arena_base) {
        t->arena_base = (char *)malloc(TASK_ARENA_SIZE);
        if (!t->arena_base) return;
    }
    t->arena_offset = 0;
    t->use_arena = true;
}

static Obj *allocate_object(VM *vm, ValueType type, size_t size) {
    /* GC no longer self-triggers from inside an allocation. gc_mark_roots()
     * only scans the stacks of tasks registered in vm->tasks -- if a native
     * function is mid-construction of an object (built across several
     * allocating calls before it's stored anywhere a root can see it, e.g.
     * sqlite.query() building one row struct at a time, or json_decode()
     * building a nested array), an allocation triggering a sweep right then
     * can free that not-yet-reachable object out from under its own
     * constructor. Reproduced as real heap-use-after-free crashes under
     * load (see the matching comments in lib_sqlite.c/json.c/lib_string.c/
     * lib_validate.c/lib_http.c -- those are kept as defense in depth, not
     * because this fix alone wasn't enough). Collection now only happens at
     * the round-robin scheduler's safepoint (top of the loop in vm_run,
     * between ticks) where every task is guaranteed to be between
     * instructions, not mid-construction of anything. */
    /* Phase 2: per-request arena — DISABLED for AOT debugging */
    /*{
        Task *t = vm->current_task;
        if (t && t->use_arena && t->arena_base) {
            size_t aligned = (size + 7) & ~(size_t)7;
            if (t->arena_offset + aligned <= TASK_ARENA_SIZE) {
                Obj *obj = (Obj *)(t->arena_base + t->arena_offset);
                t->arena_offset += aligned;
                memset(obj, 0, size);
                obj->type = type;
                obj->is_marked = true;
                obj->next = NULL;
                return obj;
            }
        }
    }*/
    Obj *obj = (Obj *)calloc(1, size);
    if (!obj) return NULL;
    obj->type = type;
    obj->is_marked = false;
    obj->next = vm->objects;
    vm->objects = obj;
    vm->bytes_allocated += size;
    return obj;
}

uint32_t hash_string(const char *chars, int length) {
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

ObjString *allocate_string(VM *vm, const char *chars, int length) {
    ObjString *s = (ObjString *)allocate_object(vm, VAL_STRING, sizeof(ObjString));
    if (!s) return NULL;
    s->length = length;
    s->chars = (char *)malloc((size_t)length + 1);
    if (!s->chars) return NULL;
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

ObjStruct *new_struct(VM *vm, int field_count, bool force_heap) {
    Task *t = vm->current_task;

    size_t hsize  = (sizeof(ObjStruct) + 7) & ~(size_t)7;
    size_t fc     = (size_t)(field_count > 0 ? field_count : 0);
    size_t nsize  = (fc * sizeof(char *) + 7) & ~(size_t)7;
    size_t vsize  = (fc * sizeof(Value) + 7) & ~(size_t)7;
    size_t avsize = (fc * sizeof(ValidationRule *) + 7) & ~(size_t)7;
    size_t acsize = (fc * sizeof(int) + 7) & ~(size_t)7;
    size_t total  = hsize + nsize + vsize + avsize + acsize;

    bool use_arena = !force_heap && t && t->use_arena && t->arena_base &&
                      (t->arena_offset + total <= TASK_ARENA_SIZE);

    ObjStruct *s;
    if (use_arena) {
        char *base = t->arena_base + t->arena_offset;
        t->arena_offset += total;
        memset(base, 0, total);
        s = (ObjStruct *)base;
        s->field_names = (char **)(base + hsize);
        s->fields = (Value *)(base + hsize + nsize);
        s->field_validations = (ValidationRule **)(base + hsize + nsize + vsize);
        s->field_validation_counts = (int *)(base + hsize + nsize + vsize + avsize);
        /* Deliberately NOT linked into vm->objects: arena structs are bulk-
         * reclaimed when this task's arena resets on recycle, not GC-swept
         * individually. See escape_promote() for how references that must
         * outlive this request get safely copied out before that happens. */
    } else {
        s = (ObjStruct *)calloc(1, sizeof(ObjStruct));
        s->field_names = (char **)calloc(fc, sizeof(char *));
        s->fields = (Value *)calloc(fc, sizeof(Value));
        s->field_validations = (ValidationRule **)calloc(fc, sizeof(ValidationRule *));
        s->field_validation_counts = (int *)calloc(fc, sizeof(int));
        s->obj.next = vm->objects;
        vm->objects = (Obj *)s;
    }
    s->obj.type = VAL_STRUCT;
    s->field_count = field_count;
    return s;
}

/* True if `s` physically lives inside task t's 64KB arena block. */
static bool struct_is_arena_backed(Task *t, ObjStruct *s) {
    if (!t || !t->arena_base) return false;
    const char *p = (const char *)s;
    return p >= t->arena_base && p < t->arena_base + TASK_ARENA_SIZE;
}

/* Deep-copies any arena-backed ObjStruct reachable from `v` into a fresh
 * heap-backed (force_heap) copy, recursively, rewriting pointers as it
 * goes. Call this at every point a Value can outlive the task that built
 * it -- right now that is exactly three opcodes: BC_SET_GLOBAL,
 * BC_DEFINE_GLOBAL (both via define_global/set_global), and BC_CHAN_SEND
 * (channels double as actor mailboxes in this codebase, so this single
 * call site covers both). Cheap fast path: anything that isn't a
 * VAL_STRUCT/VAL_ARRAY/VAL_CLOSURE/VAL_ENUM returns immediately unchanged (ints,
 * floats, strings, bools, nil, functions -- strings are NEVER arena-backed
 * in this design, see "Why this design" above, so they need no check
 * here at all). Arrays, closures and enums are never themselves arena-backed
 * either (only ObjStruct is) -- they're walked in place (not copied) only
 * to find and promote any arena-backed struct nested inside them. */
static Value escape_promote(VM *vm, Value v) {
    Task *t = vm->current_task;
    if (!t || !t->use_arena) return v;
    switch (v.type) {
        case VAL_STRUCT: {
            ObjStruct *s = v.as.structure;
            if (!struct_is_arena_backed(t, s)) return v;
            ObjStruct *copy = new_struct(vm, s->field_count, true /* force_heap */);
            copy->type_name = s->type_name;             /* never arena-backed, safe to share */
            copy->struct_validations = s->struct_validations;
            copy->struct_validation_count = s->struct_validation_count;
            for (int i = 0; i < s->field_count; i++) {
                copy->field_names[i] = s->field_names[i]; /* never arena-backed, safe to share */
                copy->field_validations[i] = s->field_validations[i];
                copy->field_validation_counts[i] = s->field_validation_counts[i];
                copy->fields[i] = escape_promote(vm, s->fields[i]); /* recurse: a field can itself be an arena struct */
            }
            return val_struct(copy);
        }
        case VAL_ARRAY: {
            ObjArray *a = v.as.array;
            for (int i = 0; i < a->count; i++)
                a->elements[i] = escape_promote(vm, a->elements[i]);
            return v;
        }
        case VAL_CLOSURE: {
            ObjClosure *c = v.as.closure;
            for (int i = 0; i < c->captured_count; i++)
                c->captured[i] = escape_promote(vm, c->captured[i]);
            return v;
        }
        case VAL_ENUM: {
            ObjEnum *e = v.as.enum_val;
            for (int i = 0; i < e->count; i++)
                e->values[i] = escape_promote(vm, e->values[i]);
            return v;
        }
        default:
            return v;
    }
}


ObjClosure *new_closure(ObjFunction *f, int captured_count) {
    ObjClosure *c = (ObjClosure *)calloc(1, sizeof(ObjClosure));
    c->obj.type = VAL_CLOSURE;
    c->function = f;
    c->captured_count = captured_count;
    if (captured_count > 0) {
        c->captured = (Value *)calloc(captured_count, sizeof(Value));
    } else {
        c->captured = NULL;
    }
    return c;
}

ObjEnum *new_enum(int value_count) {
    ObjEnum *e = (ObjEnum *)calloc(1, sizeof(ObjEnum));
    e->obj.type = VAL_ENUM;
    e->tag = 0;
    e->count = value_count;
    e->values = (Value *)calloc(value_count, sizeof(Value));
    return e;
}

ObjModule *new_module(const char *name) {
    ObjModule *m = (ObjModule *)calloc(1, sizeof(ObjModule));
    m->obj.type = VAL_MODULE;
    m->name = (char *)malloc(strlen(name) + 1);
    if (m->name) strcpy(m->name, name);
    return m;
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

/* ─── GC: Gray Stack ─── */
static void gc_push_gray(VM *vm, Obj *obj) {
    if (vm->gray_count >= vm->gray_capacity) {
        int old = vm->gray_capacity;
        vm->gray_capacity = old < 64 ? 64 : old * 2;
        vm->gray_stack = (Obj **)realloc(vm->gray_stack, sizeof(Obj *) * vm->gray_capacity);
    }
    vm->gray_stack[vm->gray_count++] = obj;
}

static void gc_mark_value(VM *vm, Value value) {
    Obj *obj = NULL;
    switch (value.type) {
        case VAL_STRING:   obj = (Obj *)value.as.string; break;
        case VAL_ARRAY:    obj = (Obj *)value.as.array; break;
        case VAL_TUPLE:    obj = (Obj *)value.as.tuple; break;
        case VAL_FUNCTION: obj = (Obj *)value.as.function; break;
        case VAL_CLOSURE:  obj = (Obj *)value.as.closure; break;
        case VAL_STRUCT:   obj = (Obj *)value.as.structure; break;
        case VAL_ENUM:     obj = (Obj *)value.as.enum_val; break;
        case VAL_MODULE:   obj = (Obj *)value.as.module; break;
        case VAL_TASK:     obj = (Obj *)value.as.task_obj; break;
        case VAL_CHANNEL:  obj = (Obj *)value.as.channel; break;
        case VAL_ACTOR:    obj = (Obj *)value.as.actor; break;
        default: return;
    }
    if (obj && !obj->is_marked) {
        obj->is_marked = true;
        gc_push_gray(vm, obj);
    }
}

static void gc_trace(VM *vm) {
    while (vm->gray_count > 0) {
        Obj *obj = vm->gray_stack[--vm->gray_count];
        switch (obj->type) {
            case VAL_STRUCT: {
                ObjStruct *s = (ObjStruct *)obj;
                for (int i = 0; i < s->field_count; i++)
                    gc_mark_value(vm, s->fields[i]);
                break;
            }
            case VAL_ARRAY: {
                ObjArray *a = (ObjArray *)obj;
                for (int i = 0; i < a->count; i++)
                    gc_mark_value(vm, a->elements[i]);
                break;
            }
            case VAL_TUPLE: {
                ObjTuple *t = (ObjTuple *)obj;
                for (int i = 0; i < t->count; i++)
                    gc_mark_value(vm, t->elements[i]);
                break;
            }
            case VAL_ENUM: {
                ObjEnum *e = (ObjEnum *)obj;
                for (int i = 0; i < e->count; i++)
                    gc_mark_value(vm, e->values[i]);
                break;
            }
            case VAL_FUNCTION: {
                ObjFunction *f = (ObjFunction *)obj;
                for (int i = 0; i < f->constant_count; i++)
                    gc_mark_value(vm, f->constants[i]);
                break;
            }
            case VAL_CLOSURE: {
                ObjClosure *c = (ObjClosure *)obj;
                if (c->function) {
                    // Mark the function so it doesn't get collected (even though it's typically untracked)
                    gc_mark_value(vm, val_function(c->function));
                }
                for (int i = 0; i < c->captured_count; i++) {
                    gc_mark_value(vm, c->captured[i]);
                }
                break;
            }
            case VAL_ACTOR: {
                ObjActor *a = (ObjActor *)obj;
                gc_mark_value(vm, a->state);
                gc_mark_value(vm, a->inbox);
                break;
            }
            case VAL_CHANNEL: {
                ObjChannel *c = (ObjChannel *)obj;
                for (int i = 0; i < c->count; i++) {
                    int idx = (c->head + i) % c->capacity;
                    gc_mark_value(vm, c->buffer[idx]);
                }
                break;
            }
            case VAL_MODULE:
            case VAL_TASK:
                /* Task/Module has no heap Value references */
                break;
            default: break;  /* VAL_STRING has no references */
        }
    }
}

static size_t gc_sweep(VM *vm) {
    size_t freed = 0;
    /* Remove unmarked strings from intern table */
    for (int i = 0; i < vm->intern_capacity; i++) {
        if (vm->intern_table[i] && !((Obj *)vm->intern_table[i])->is_marked) {
            vm->intern_table[i] = NULL;
            vm->intern_count--;
        }
    }
    Obj **prev = &vm->objects;
    Obj *obj = vm->objects;
    while (obj) {
        if (!obj->is_marked) {
            Obj *next = obj->next;
            *prev = next;
            size_t obj_size = sizeof(Obj);
            switch (obj->type) {
                case VAL_STRING: {
                    ObjString *s = (ObjString *)obj;
                    free(s->chars);
                    obj_size = sizeof(ObjString);
                    break;
                }
                case VAL_ARRAY:  free(((ObjArray *)obj)->elements);
                    obj_size = sizeof(ObjArray); break;
                case VAL_TUPLE:  free(((ObjTuple *)obj)->elements);
                    obj_size = sizeof(ObjTuple); break;
                case VAL_ENUM:   free(((ObjEnum *)obj)->values);
                    obj_size = sizeof(ObjEnum); break;
                case VAL_STRUCT: {
                    ObjStruct *s = (ObjStruct *)obj;
                    for (int i = 0; i < s->field_count; i++)
                        free(s->field_names[i]);
                    free(s->field_names);
                    free(s->fields);
                    free(s->type_name);
                    free(s->field_validations);
                    free(s->field_validation_counts);
                    obj_size = sizeof(ObjStruct);
                    break;
                }
                case VAL_ACTOR: {
                    ObjActor *a = (ObjActor *)obj;
                    free(a->type_name);
                    obj_size = sizeof(ObjActor);
                    break;
                }
                case VAL_FUNCTION: {
                    ObjFunction *f = (ObjFunction *)obj;
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
                    obj_size = sizeof(ObjFunction);
                    break;
                }
                case VAL_CLOSURE: {
                    ObjClosure *c = (ObjClosure *)obj;
                    free(c->captured);
                    obj_size = sizeof(ObjClosure);
                    break;
                }
                default: break;
            }
            free(obj);
            freed += obj_size;
            obj = next;
        } else {
            obj->is_marked = false;
            prev = &obj->next;
            obj = obj->next;
        }
    }
    return freed;
}

static void gc_mark_roots(VM *vm) {
    /* Mark globals */
    for (int i = 0; i < vm->global_count; i++)
        gc_mark_value(vm, vm->globals[i]);

    /* Mark main_fn */
    if (vm->main_fn) {
        Obj *obj = (Obj *)vm->main_fn;
        if (!obj->is_marked) {
            obj->is_marked = true;
            gc_push_gray(vm, obj);
        }
    }

    /* Mark ALL tasks' stacks AND frame functions */
    for (int ti = 0; ti < vm->task_count; ti++) {
        Task *t = vm->tasks[ti];
        if (t->dead) continue;
        for (int i = 0; i < t->stack_top; i++)
            gc_mark_value(vm, t->stack[i]);
        for (int i = 0; i < t->frame_count; i++) {
            if (t->frames[i].function) {
                Obj *fn_obj = (Obj *)t->frames[i].function;
                if (!fn_obj->is_marked) {
                    fn_obj->is_marked = true;
                    gc_push_gray(vm, fn_obj);
                }
            }
        }
        /* Mark actor reply channel if waiting */
        if (t->waiting_actor_reply)
            gc_mark_value(vm, t->actor_reply_ch);
        /* Mark actor ref for actor loop tasks */
        if (t->is_actor_loop && t->actor_ref) {
            Obj *aobj = (Obj *)t->actor_ref;
            if (!aobj->is_marked) {
                aobj->is_marked = true;
                gc_push_gray(vm, aobj);
            }
        }
    }

    /* Mark dispatch table function values */
    for (int i = 0; i < DISPATCH_TABLE_SIZE; i++) {
        if (vm->dispatch_occupied[i])
            gc_mark_value(vm, vm->dispatch_functions[i]);
    }
}

static void gc_collect(VM *vm) {
    gc_mark_roots(vm);
    gc_trace(vm);
    size_t freed = gc_sweep(vm);
    vm->bytes_allocated -= (freed < vm->bytes_allocated) ? freed : vm->bytes_allocated;
    vm->next_gc_size = (vm->bytes_allocated < 1024) ? 1024 * 1024 : vm->bytes_allocated * 2;
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
        case VAL_CLOSURE: printf("<closure %p>", (void *)value.as.closure); break;
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
        case VAL_MODULE:
            printf("<module %s>", value.as.module->name);
            break;
        case VAL_TASK:
            printf("<task %d>", value.as.task_obj->task->id);
            break;
        case VAL_CHANNEL:
            printf("<channel %d/%d>", value.as.channel->count, value.as.channel->capacity);
            break;
        case VAL_ACTOR:
            printf("<actor %s>", value.as.actor->type_name);
            break;
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
        case VAL_CLOSURE: return a.as.closure == b.as.closure;
        default: return false;
    }
}

/* ─── Compiler ─── */

void compiler_init(Compiler *compiler, Arena *arena, Chunk *chunk, AstNode *program) {
    compiler->enclosing = NULL;
    compiler->arena = arena;
    compiler->chunk = chunk;
    compiler->program = program;
    compiler->scope_depth = 0;
    compiler->local_count = 0;
    compiler->loop_count = 0;
    compiler->current_line = 0;
    compiler->in_function = false;
    compiler->had_error = false;
    compiler->error_message[0] = '\0';
    compiler->ffi_decl_count = 0;
    compiler->test_count = 0;
    compiler->upvalue_count = 0;
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

static int compiler_add_upvalue(Compiler *compiler, uint8_t index, bool is_local) {
    for (int i = 0; i < compiler->upvalue_count; i++) {
        if (compiler->upvalue_index[i] == index &&
            compiler->upvalue_is_local[i] == is_local) {
            return i;
        }
    }
    if (compiler->upvalue_count >= 64) {
        compiler_error(compiler, "Too many captured variables in one closure");
        return 0;
    }
    int idx = compiler->upvalue_count++;
    compiler->upvalue_is_local[idx] = is_local;
    compiler->upvalue_index[idx] = index;
    return idx;
}

static int compiler_resolve_upvalue(Compiler *compiler, const char *name) {
    if (compiler->enclosing == NULL) {
        return -1;
    }
    int local_idx = compiler_find_local(compiler->enclosing, name);
    if (local_idx >= 0) {
        return compiler_add_upvalue(compiler, (uint8_t)local_idx, true);
    }
    int up_idx = compiler_resolve_upvalue(compiler->enclosing, name);
    if (up_idx >= 0) {
        return compiler_add_upvalue(compiler, (uint8_t)up_idx, false);
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
    loop->scope_depth = compiler->scope_depth;
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

/* Pop all locals declared at a given scope depth */
static void emit_pop_scope(Compiler *compiler, int depth) {
    for (int i = compiler->local_count - 1; i >= 0; i--) {
        if (compiler->local_depths[i] == depth)
            emit_byte(compiler, BC_POP);
    }
}

static void emit_bytes(Compiler *compiler, uint8_t b1, uint8_t b2) {
    emit_byte(compiler, b1);
    emit_byte(compiler, b2);
}

static void emit_short(Compiler *compiler, uint16_t value) {
    emit_byte(compiler, (uint8_t)((value >> 8) & 0xFF));
    emit_byte(compiler, (uint8_t)(value & 0xFF));
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
    emit_byte(compiler, BC_CONSTANT);
    emit_short(compiler, (uint16_t)idx);
}

static void emit_constant_idx(Compiler *compiler, int idx) {
    emit_byte(compiler, BC_CONSTANT);
    emit_short(compiler, (uint16_t)idx);
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
                int up_idx = compiler_resolve_upvalue(compiler, node->identifier.name);
                if (up_idx >= 0) {
                    emit_bytes(compiler, BC_GET_UPVALUE, (uint8_t)up_idx);
                } else {
                    emit_byte(compiler, BC_GET_GLOBAL);
                    ObjString *s = copy_string(node->identifier.name, (int)strlen(node->identifier.name));
                    int idx = chunk_add_constant(compiler->chunk, val_string(s));
                    emit_short(compiler, (uint16_t)idx);
                }
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
                case OP_NIL_COALESCE: emit_byte(compiler, BC_NIL_COALESCE); break;
                default: compiler_error(compiler, "Unknown binary operator"); break;
            }
            break;
        }

        case NODE_QUESTION_DOT: {
            compile_expression(compiler, node->member.object);
            int nil_jump = emit_jump(compiler, BC_JUMP_IF_NIL);
            ObjString *s = copy_string(node->member.member, (int)strlen(node->member.member));
            emit_byte(compiler, BC_MEMBER_SAFE);
            int idx = chunk_add_constant(compiler->chunk, val_string(s));
            emit_short(compiler, (uint16_t)idx);
            int end_jump = emit_jump(compiler, BC_JUMP);
            patch_jump(compiler, nil_jump);
            emit_byte(compiler, BC_POP);
            emit_byte(compiler, BC_NIL);
            patch_jump(compiler, end_jump);
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
            /* Check if this is an FFI function call */
            if (node->call.callee->kind == NODE_IDENTIFIER) {
                const char *name = node->call.callee->identifier.name;
                int ffi_idx = -1;
                for (int i = 0; i < compiler->ffi_decl_count; i++) {
                    if (strcmp(compiler->ffi_decls[i].name, name) == 0) {
                        ffi_idx = i;
                        break;
                    }
                }
                if (ffi_idx >= 0) {
                    /* FFI call — emit BC_FFI_CALL */
                    for (int i = 0; i < node->call.arg_count; i++)
                        compile_expression(compiler, node->call.args[i]);
                    emit_bytes(compiler, BC_FFI_CALL, (uint8_t)ffi_idx);
                    emit_byte(compiler, (uint8_t)node->call.arg_count);
                    break;
                }
            }
            /* Normal call */
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
            emit_short(compiler, (uint16_t)idx);
            break;
        }

        case NODE_INTERPOLATED_STRING: {
            int count = node->interpolated_string.part_count;
            if (count == 0) {
                emit_byte(compiler, BC_NIL);
                break;
            }
            for (int i = 0; i < count; i++) {
                compile_expression(compiler, node->interpolated_string.parts[i]);
                emit_byte(compiler, BC_INT_TO_STRING);
            }
            emit_bytes(compiler, BC_BUILD_STRING, (uint8_t)count);
            break;
        }

        case NODE_CHAN_SEND:
            compile_expression(compiler, node->chan_send.channel);
            compile_expression(compiler, node->chan_send.value);
            emit_byte(compiler, BC_CHAN_SEND);
            break;
        case NODE_CHAN_RECEIVE:
            compile_expression(compiler, node->chan_receive.channel);
            emit_byte(compiler, BC_CHAN_RECEIVE);
            break;
        case NODE_AWAIT:
            compile_expression(compiler, node->await.expr);
            emit_byte(compiler, BC_AWAIT);
            break;

        default:
            compile_node(compiler, node);
            break;
    }
}

/* Convert a Varian primitive FFI type to an FFITypeKind */
static FFITypeKind primitive_to_ffi_kind(PrimitiveKind pk) {
    switch (pk) {
        case PRIMITIVE_PTR:     return FFI_PTR;
        case PRIMITIVE_C_INT:   return FFI_INT;
        case PRIMITIVE_C_DOUBLE: return FFI_DOUBLE;
        case PRIMITIVE_C_FLOAT:  return FFI_FLOAT;
        case PRIMITIVE_C_CHAR:   return FFI_CHAR;
        default: return FFI_PTR;
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

            if (compiler->in_function) {
                /* Local variable(s) inside function */
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
                /* Global variable(s) at top level */
                for (int i = node->let_decl.name_count - 1; i >= 0; i--) {
                    const char *name = node->let_decl.names[i];
                    if (strcmp(name, "_") == 0) {
                        emit_byte(compiler, BC_POP);
                    } else {
                        emit_byte(compiler, BC_DEFINE_GLOBAL);
                        ObjString *s = copy_string(name, (int)strlen(name));
                        int idx = chunk_add_constant(compiler->chunk, val_string(s));
                        emit_short(compiler, (uint16_t)idx);
                    }
                }
            }
            break;

        case NODE_ASSERT:
            compile_expression(compiler, node->assert_stmt.condition);
            emit_byte(compiler, BC_ASSERT);
            break;

        case NODE_FN_DECL: {
            Chunk fn_chunk;
            chunk_init(&fn_chunk);
            Compiler fn_compiler;
            fn_compiler.enclosing = compiler;
            fn_compiler.arena = compiler->arena;
            fn_compiler.chunk = &fn_chunk;
            fn_compiler.scope_depth = 1;
            fn_compiler.had_error = false;
            fn_compiler.error_message[0] = '\0';
            fn_compiler.local_count = 0;
            fn_compiler.loop_count = 0;
            fn_compiler.in_function = true;
            fn_compiler.ffi_decl_count = 0;
            fn_compiler.test_count = 0;
            fn_compiler.current_line = compiler->current_line;
            fn_compiler.program = compiler->program;
            fn_compiler.upvalue_count = 0;

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
            func->metadata = val_nil();

            /* Compile decorators into metadata array: [key1, val1, key2, val2, ...] */
            if (node->fn_decl.decorator_count > 0) {
                int dc = node->fn_decl.decorator_count;
                ObjArray *meta_arr = (ObjArray *)calloc(1, sizeof(ObjArray));
                meta_arr->obj.type = VAL_ARRAY;
                meta_arr->count = dc * 2;
                meta_arr->elements = (Value *)calloc(dc * 2, sizeof(Value));
                for (int i = 0; i < dc; i++) {
                    /* Key: compile the decorator key string into a constant */
                    ObjString *ks = copy_string(node->fn_decl.decorator_keys[i],
                                                (int)strlen(node->fn_decl.decorator_keys[i]));
                    meta_arr->elements[i * 2] = val_string(ks);
                    /* Value: compile the decorator value expression */
                    /* For simplicity, evaluate literal values at compile time */
                    AstNode *val_node = node->fn_decl.decorator_values[i];
                    if (val_node->kind == NODE_BOOL_LITERAL) {
                        meta_arr->elements[i * 2 + 1] = val_bool(val_node->literal.bool_value);
                    } else if (val_node->kind == NODE_INT_LITERAL) {
                        meta_arr->elements[i * 2 + 1] = val_int(val_node->literal.int_value);
                    } else if (val_node->kind == NODE_STRING_LITERAL) {
                        ObjString *vs = copy_string(val_node->literal.string_value,
                                                    (int)strlen(val_node->literal.string_value));
                        meta_arr->elements[i * 2 + 1] = val_string(vs);
                    } else {
                        meta_arr->elements[i * 2 + 1] = val_bool(true);
                    }
                }
                func->metadata = val_array(meta_arr);
            }

            emit_constant(compiler, val_function(func));
            if (fn_compiler.upvalue_count > 0) {
                for (int i = 0; i < fn_compiler.upvalue_count; i++) {
                    if (fn_compiler.upvalue_is_local[i]) {
                        emit_bytes(compiler, BC_GET_LOCAL, fn_compiler.upvalue_index[i]);
                    } else {
                        emit_bytes(compiler, BC_GET_UPVALUE, fn_compiler.upvalue_index[i]);
                    }
                }
                emit_bytes(compiler, BC_CLOSURE, (uint8_t)fn_compiler.upvalue_count);
            }
            if (node->fn_decl.is_method && node->fn_decl.impl_type) {
                emit_byte(compiler, BC_REGISTER_METHOD);
                {
                    ObjString *ts = copy_string(node->fn_decl.impl_type,
                                                (int)strlen(node->fn_decl.impl_type));
                    int ti = chunk_add_constant(compiler->chunk, val_string(ts));
                    emit_short(compiler, (uint16_t)ti);
                }
                {
                    ObjString *ms = copy_string(node->fn_decl.name,
                                                (int)strlen(node->fn_decl.name));
                    int mi = chunk_add_constant(compiler->chunk, val_string(ms));
                    emit_short(compiler, (uint16_t)mi);
                }
            }
            /* Lambdas (name "__lambda__") are expressions, not statements —
             * skip BC_DEFINE_GLOBAL so the function value stays on the stack. */
            if (strcmp(node->fn_decl.name, "__lambda__") != 0) {
                emit_byte(compiler, BC_DEFINE_GLOBAL);
                {
                    ObjString *s = copy_string(node->fn_decl.name,
                                               (int)strlen(node->fn_decl.name));
                    int idx = chunk_add_constant(compiler->chunk, val_string(s));
                    emit_short(compiler, (uint16_t)idx);
                }
            }
            break;
        }

        case NODE_TEST: {
            Chunk fn_chunk;
            chunk_init(&fn_chunk);
            Compiler fn_compiler;
            fn_compiler.enclosing = NULL;
            fn_compiler.arena = compiler->arena;
            fn_compiler.chunk = &fn_chunk;
            fn_compiler.scope_depth = 1;
            fn_compiler.had_error = false;
            fn_compiler.error_message[0] = '\0';
            fn_compiler.local_count = 0;
            fn_compiler.loop_count = 0;
            fn_compiler.in_function = true;
            fn_compiler.ffi_decl_count = 0;
            fn_compiler.test_count = 0;
            fn_compiler.upvalue_count = 0;

            if (node->test_decl.body) {
                for (int i = 0; i < node->test_decl.body->block.stmt_count; i++)
                    compile_node(&fn_compiler, node->test_decl.body->block.stmts[i]);
            }
            /* Implicit return */
            emit_byte(&fn_compiler, BC_NIL);
            emit_byte(&fn_compiler, BC_RETURN);

            ObjFunction *func = (ObjFunction *)calloc(1, sizeof(ObjFunction));
            func->obj.type = VAL_FUNCTION;
            func->arity = 0;
            func->code = fn_chunk.code;
            func->code_count = fn_chunk.count;
            func->code_capacity = fn_chunk.capacity;
            func->constants = fn_chunk.constants;
            func->constant_count = fn_chunk.constant_count;
            func->constant_capacity = fn_chunk.constant_capacity;
            func->rle_lines = fn_chunk.rle_lines;
            func->rle_counts = fn_chunk.rle_counts;
            func->rle_count = fn_chunk.rle_count;
            func->metadata = val_nil();

            /* Store in compiler's test registry (not emitted as global) */
            if (compiler->test_count < MAX_TESTS) {
                compiler->tests[compiler->test_count].description =
                    strdup(node->test_decl.description);
                compiler->tests[compiler->test_count].func = func;
                compiler->test_count++;
            }
            break;
        }

        case NODE_BLOCK: {
            int saved_local_count = compiler->local_count;
            compiler->scope_depth++;
            for (int i = 0; i < node->block.stmt_count; i++)
                compile_node(compiler, node->block.stmts[i]);
            /* Pop locals declared inside this block (at the current inner depth) */
            emit_pop_scope(compiler, compiler->scope_depth);
            compiler->scope_depth--;
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
                    emit_short(compiler, (uint16_t)idx);
                }

                compile_expression(compiler, end);
                {
                    ObjString *s = copy_string("__end__", 7);
                    int idx = chunk_add_constant(compiler->chunk, val_string(s));
                    emit_byte(compiler, BC_DEFINE_GLOBAL);
                    emit_short(compiler, (uint16_t)idx);
                }

                int loop_start = compiler->chunk->count;
                compiler_push_loop(compiler, loop_start);

                {
                    ObjString *s = copy_string(var_name, (int)strlen(var_name));
                    int idx = chunk_add_constant(compiler->chunk, val_string(s));
                    emit_byte(compiler, BC_GET_GLOBAL);
                    emit_short(compiler, (uint16_t)idx);
                }
                {
                    ObjString *s = copy_string("__end__", 7);
                    int idx = chunk_add_constant(compiler->chunk, val_string(s));
                    emit_byte(compiler, BC_GET_GLOBAL);
                    emit_short(compiler, (uint16_t)idx);
                }
                emit_byte(compiler, BC_LESS);
                int exit_jump = emit_jump(compiler, BC_JUMP_IF_FALSE);
                emit_byte(compiler, BC_POP);

                compile_node(compiler, node->for_stmt.body);

                {
                    ObjString *s = copy_string(var_name, (int)strlen(var_name));
                    int idx = chunk_add_constant(compiler->chunk, val_string(s));
                    emit_byte(compiler, BC_GET_GLOBAL);
                    emit_short(compiler, (uint16_t)idx);
                }
                int one_idx = chunk_add_constant(compiler->chunk, val_int(1));
                emit_constant_idx(compiler, one_idx);
                emit_byte(compiler, BC_ADD);
                {
                    ObjString *s = copy_string(var_name, (int)strlen(var_name));
                    int idx = chunk_add_constant(compiler->chunk, val_string(s));
                    emit_byte(compiler, BC_SET_GLOBAL);
                    emit_short(compiler, (uint16_t)idx);
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
            if (node->assign.target->kind == NODE_IDENTIFIER) {
                compile_expression(compiler, node->assign.value);
                int local_idx = compiler_find_local(compiler, node->assign.target->identifier.name);
                if (local_idx >= 0) {
                    emit_bytes(compiler, BC_SET_LOCAL, (uint8_t)local_idx);
                } else {
                    int up_idx = compiler_resolve_upvalue(compiler, node->assign.target->identifier.name);
                    if (up_idx >= 0) {
                        emit_bytes(compiler, BC_SET_UPVALUE, (uint8_t)up_idx);
                    } else {
                        emit_byte(compiler, BC_SET_GLOBAL);
                        ObjString *s = copy_string(node->assign.target->identifier.name,
                                                   (int)strlen(node->assign.target->identifier.name));
                        int idx = chunk_add_constant(compiler->chunk, val_string(s));
                        emit_short(compiler, (uint16_t)idx);
                    }
                }
            } else if (node->assign.target->kind == NODE_MEMBER) {
                compile_expression(compiler, node->assign.target->member.object);
                compile_expression(compiler, node->assign.value);
                emit_byte(compiler, BC_SET_MEMBER);
                ObjString *s = copy_string(node->assign.target->member.member,
                                           (int)strlen(node->assign.target->member.member));
                int idx = chunk_add_constant(compiler->chunk, val_string(s));
                emit_short(compiler, (uint16_t)idx);
            } else if (node->assign.target->kind == NODE_INDEX) {
                compile_expression(compiler, node->assign.target->index.object);
                compile_expression(compiler, node->assign.target->index.index);
                compile_expression(compiler, node->assign.value);
                emit_byte(compiler, BC_SET_INDEX);
            }
            break;

        case NODE_BREAK: {
            if (compiler->loop_count > 0) {
                LoopInfo *loop = &compiler->loops[compiler->loop_count - 1];
                for (int i = compiler->local_count - 1; i >= 0; i--) {
                    if (compiler->local_depths[i] > loop->scope_depth)
                        emit_byte(compiler, BC_POP);
                }
            }
            int jump_offset = emit_jump(compiler, BC_JUMP);
            compiler_record_break(compiler, jump_offset);
            break;
        }

        case NODE_CONTINUE: {
            if (compiler->loop_count > 0) {
                LoopInfo *loop = &compiler->loops[compiler->loop_count - 1];
                for (int i = compiler->local_count - 1; i >= 0; i--) {
                    if (compiler->local_depths[i] > loop->scope_depth)
                        emit_byte(compiler, BC_POP);
                }
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

                if (arm->match_arm.bind_count > 0) {
                    /* Destructuring pattern: compare tag, unpack, bind */
                    int tag = arm->match_arm.pattern->enum_literal.tag;
                    emit_byte(compiler, BC_TAG_EQ);
                    emit_byte(compiler, (uint8_t)tag);

                    int next_arm = emit_jump(compiler, BC_JUMP_IF_FALSE);
                    emit_byte(compiler, BC_POP); /* pop bool */

                    /* Reload match value and unpack */
                    if (temp_idx >= 0) {
                        emit_bytes(compiler, BC_GET_LOCAL, (uint8_t)temp_idx);
                    } else {
                        compile_expression(compiler, node->match_stmt.value);
                    }
                    emit_byte(compiler, BC_UNPACK_ENUM);

                    /* Bind extracted values */
                    int saved_arm = compiler->local_count;
                    for (int j = arm->match_arm.bind_count - 1; j >= 0; j--) {
                        const char *bname = arm->match_arm.bind_names[j];
                        if (compiler->in_function) {
                            int idx = compiler_add_local(compiler, bname);
                            emit_bytes(compiler, BC_SET_LOCAL, (uint8_t)idx);
                            emit_byte(compiler, BC_POP);
                        } else {
                            emit_byte(compiler, BC_DEFINE_GLOBAL);
                            ObjString *s = copy_string(bname, (int)strlen(bname));
                            int idx = chunk_add_constant(compiler->chunk, val_string(s));
                            emit_short(compiler, (uint16_t)idx);
                        }
                    }

                    compile_expression(compiler, arm->match_arm.body);
                    compiler->local_count = saved_arm;
                    end_jumps[end_count++] = emit_jump(compiler, BC_JUMP);
                    patch_jump(compiler, next_arm);
                } else {
                    /* Literal pattern: compile expression and compare */
                    compile_expression(compiler, arm->match_arm.pattern);
                    emit_byte(compiler, BC_EQUAL);

                    int next_arm = emit_jump(compiler, BC_JUMP_IF_FALSE);
                    emit_byte(compiler, BC_POP); /* pop bool result (true path) */

                    compile_expression(compiler, arm->match_arm.body);
                    end_jumps[end_count++] = emit_jump(compiler, BC_JUMP);

                    patch_jump(compiler, next_arm);
                }
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

        case NODE_STRUCT_DECL: {
            /* Check if struct has validation decorators */
            bool has_validations = (node->struct_decl.decorator_count > 0);
            if (!has_validations) {
                for (int i = 0; i < node->struct_decl.field_count; i++) {
                    if (node->struct_decl.field_decorator_counts &&
                        node->struct_decl.field_decorator_counts[i] > 0) {
                        has_validations = true;
                        break;
                    }
                }
            }

            if (has_validations) {
                /* Emit bytecode to register validations at runtime */
                emit_byte(compiler, BC_REGISTER_VALIDATIONS);

                /* Struct name */
                ObjString *ts = copy_string(node->struct_decl.name,
                                            (int)strlen(node->struct_decl.name));
                int ti = chunk_add_constant(compiler->chunk, val_string(ts));
                emit_short(compiler, (uint16_t)ti);

                /* Struct-level validation count */
                emit_byte(compiler, (uint8_t)node->struct_decl.decorator_count);

                /* Struct-level validation rules: key idx, then the decorator's
                 * literal argument (e.g. @min_len(3) -> 3; bare @is_email -> true) */
                for (int i = 0; i < node->struct_decl.decorator_count; i++) {
                    ObjString *key = copy_string(node->struct_decl.decorator_keys[i],
                                                 (int)strlen(node->struct_decl.decorator_keys[i]));
                    int ki = chunk_add_constant(compiler->chunk, val_string(key));
                    emit_short(compiler, (uint16_t)ki);

                    Value arg_val = val_bool(true);
                    if (!decorator_literal_to_value(node->struct_decl.decorator_values[i], &arg_val)) {
                        compiler_error(compiler, "Decorator arguments must be literal values");
                    }
                    int ai = chunk_add_constant(compiler->chunk, arg_val);
                    emit_short(compiler, (uint16_t)ai);
                }

                /* Field count */
                emit_byte(compiler, (uint8_t)node->struct_decl.field_count);

                /* Field names and their validations */
                for (int i = 0; i < node->struct_decl.field_count; i++) {
                    ObjString *fname = copy_string(node->struct_decl.field_names[i],
                                                   (int)strlen(node->struct_decl.field_names[i]));
                    int fi = chunk_add_constant(compiler->chunk, val_string(fname));
                    emit_short(compiler, (uint16_t)fi);

                    int fcount = 0;
                    if (node->struct_decl.field_decorator_counts) {
                        fcount = node->struct_decl.field_decorator_counts[i];
                    }
                    emit_byte(compiler, (uint8_t)fcount);

                    if (fcount > 0 && node->struct_decl.field_decorator_keys &&
                        node->struct_decl.field_decorator_keys[i]) {
                        for (int j = 0; j < fcount; j++) {
                            ObjString *fkey = copy_string(node->struct_decl.field_decorator_keys[i][j],
                                                          (int)strlen(node->struct_decl.field_decorator_keys[i][j]));
                            int fki = chunk_add_constant(compiler->chunk, val_string(fkey));
                            emit_short(compiler, (uint16_t)fki);

                            Value arg_val = val_bool(true);
                            if (!decorator_literal_to_value(node->struct_decl.field_decorator_values[i][j], &arg_val)) {
                                compiler_error(compiler, "Decorator arguments must be literal values");
                            }
                            int afi = chunk_add_constant(compiler->chunk, arg_val);
                            emit_short(compiler, (uint16_t)afi);
                        }
                    }
                }
            }
            break;
        }

        case NODE_ACTOR_DECL: {
            /* Actor init — emit BC_ACTOR_INIT with type name + field info */
            emit_byte(compiler, BC_ACTOR_INIT);
            ObjString *ts = copy_string(node->actor_decl.name,
                                        (int)strlen(node->actor_decl.name));
            int ti = chunk_add_constant(compiler->chunk, val_string(ts));
            emit_short(compiler, (uint16_t)ti);
            emit_byte(compiler, (uint8_t)node->actor_decl.field_count);
            for (int i = 0; i < node->actor_decl.field_count; i++) {
                ObjString *fs = copy_string(node->actor_decl.field_names[i],
                                            (int)strlen(node->actor_decl.field_names[i]));
                int fi = chunk_add_constant(compiler->chunk, val_string(fs));
                emit_short(compiler, (uint16_t)fi);
            }
            break;
        }

        case NODE_STRUCT_LITERAL: {
            for (int i = 0; i < node->struct_literal.field_count; i++)
                compile_expression(compiler, node->struct_literal.field_values[i]);
            emit_bytes(compiler, BC_STRUCT, (uint8_t)node->struct_literal.field_count);
            {
                ObjString *ts = copy_string(node->struct_literal.name,
                                            (int)strlen(node->struct_literal.name));
                int ti = chunk_add_constant(compiler->chunk, val_string(ts));
                emit_short(compiler, (uint16_t)ti);
            }
            for (int i = 0; i < node->struct_literal.field_count; i++) {
                ObjString *s = copy_string(node->struct_literal.field_names[i],
                                           (int)strlen(node->struct_literal.field_names[i]));
                int idx = chunk_add_constant(compiler->chunk, val_string(s));
                emit_short(compiler, (uint16_t)idx);
            }
            break;
        }

        case NODE_ENUM_DECL:
            /* Enum declaration — no bytecode emitted */
            break;

        case NODE_TRAIT_DECL:
            /* Trait declaration — no bytecode emitted */
            break;

        case NODE_FFI_DECL: {
            /* Register this function in the compiler's FFI declaration list */
            if (compiler->ffi_decl_count >= MAX_FFI_ENTRIES) {
                compiler_error(compiler, "Too many FFI declarations");
                break;
            }
            int idx = compiler->ffi_decl_count++;
            FFIDecl *decl = &compiler->ffi_decls[idx];

            strncpy(decl->name, node->ffi_decl.name, 63);
            decl->name[63] = '\0';
            strncpy(decl->lib_name, node->ffi_decl.lib_name, MAX_FFI_LIB_NAME - 1);
            decl->lib_name[MAX_FFI_LIB_NAME - 1] = '\0';
            strncpy(decl->func_name, node->ffi_decl.func_name, MAX_FFI_FUNC_NAME - 1);
            decl->func_name[MAX_FFI_FUNC_NAME - 1] = '\0';

            /* Extract FFI types from node->type (function type) */
            decl->param_count = 0;
            decl->return_kind = FFI_PTR; /* default */

            if (node->type && node->type->kind == TYPE_FUNCTION) {
                /* Param types */
                for (int i = 0; i < node->type->function.param_count && i < MAX_FFI_PARAMS; i++) {
                    Type *pt = node->type->function.param_types[i];
                    if (pt->kind == TYPE_PRIMITIVE) {
                        decl->param_kinds[i] = primitive_to_ffi_kind(pt->primitive);
                        decl->param_count++;
                    } else {
                        decl->param_kinds[i] = FFI_PTR;
                        decl->param_count++;
                    }
                }
                /* Return type */
                Type *rt = node->type->function.return_type;
                if (rt && rt->kind == TYPE_PRIMITIVE)
                    decl->return_kind = primitive_to_ffi_kind(rt->primitive);
            }

            /* No bytecode emitted — the function is handled at VM init */
            break;
        }

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

        case NODE_DISPATCH_CALL: {
            compile_expression(compiler, node->dispatch_call.object);
            for (int i = 0; i < node->dispatch_call.arg_count; i++)
                compile_expression(compiler, node->dispatch_call.args[i]);
            emit_byte(compiler, BC_DISPATCH);
            {
                ObjString *s = copy_string(node->dispatch_call.method_name,
                                           (int)strlen(node->dispatch_call.method_name));
                int idx = chunk_add_constant(compiler->chunk, val_string(s));
                emit_short(compiler, (uint16_t)idx);
            }
            emit_byte(compiler, (uint8_t)node->dispatch_call.arg_count);
            break;
        }

        case NODE_COMPTIME: {
            /* Compile the inner body into a temporary function */
            Chunk tmp_chunk;
            chunk_init(&tmp_chunk);
            Compiler tmp_comp;
            tmp_comp.enclosing = NULL;
            tmp_comp.arena = compiler->arena;
            tmp_comp.chunk = &tmp_chunk;
            tmp_comp.scope_depth = 0;
            tmp_comp.had_error = false;
            tmp_comp.error_message[0] = '\0';
            tmp_comp.local_count = 0;
            tmp_comp.loop_count = 0;
            tmp_comp.in_function = true;
            tmp_comp.ffi_decl_count = 0;
            tmp_comp.upvalue_count = 0;

            compile_node(&tmp_comp, node->comptime.body);

            /* Remove trailing BC_POP (expression statement) to keep
             * the last value on the stack as the comptime result */
            if (tmp_chunk.count > 0 &&
                tmp_chunk.code[tmp_chunk.count - 1] == BC_POP)
                tmp_chunk.count--;

            emit_byte(&tmp_comp, BC_RETURN);

            /* Create ObjFunction from the temp chunk */
            ObjFunction *fn = (ObjFunction *)calloc(1, sizeof(ObjFunction));
            fn->obj.type = VAL_FUNCTION;
            fn->arity = 0;
            fn->code = tmp_chunk.code;
            fn->code_count = tmp_chunk.count;
            fn->code_capacity = tmp_chunk.capacity;
            fn->constants = tmp_chunk.constants;
            fn->constant_count = tmp_chunk.constant_count;
            fn->constant_capacity = tmp_chunk.constant_capacity;
            fn->rle_lines = tmp_chunk.rle_lines;
            fn->rle_counts = tmp_chunk.rle_counts;
            fn->rle_count = tmp_chunk.rle_count;

             /* Store fn in outer constants */
            int fn_idx = chunk_add_constant(compiler->chunk, val_function(fn));
            /* Reserve result slot */
            int result_idx = chunk_add_constant(compiler->chunk, val_nil());

            emit_byte(compiler, BC_COMPTIME_EXEC);
            emit_short(compiler, (uint16_t)result_idx);
            emit_short(compiler, (uint16_t)fn_idx);
            break;
        }

        case NODE_TRY: {
            emit_byte(compiler, BC_TRY);
            int catch_pos = compiler->chunk->count;
            emit_byte(compiler, 0xFF);
            emit_byte(compiler, 0xFF);
            emit_byte(compiler, (uint8_t)compiler->local_count);
            compile_node(compiler, node->try_stmt.try_body);
            emit_byte(compiler, BC_POP_TRY);
            int end_jump = emit_jump(compiler, BC_JUMP);
            int offset = compiler->chunk->count - catch_pos - 3;
            compiler->chunk->code[catch_pos] = (offset >> 8) & 0xFF;
            compiler->chunk->code[catch_pos + 1] = offset & 0xFF;
            if (node->try_stmt.catch_body) {
                /* The thrown value is already sitting on top of the stack
                 * when control reaches here (pushed by the unwind code in
                 * BC_CALL/BC_DISPATCH right before jumping to catch_offset). */
                bool bound_local = false;
                int saved_local_count = compiler->local_count;
                if (node->try_stmt.catch_var && compiler->in_function) {
                    compiler_add_local(compiler, node->try_stmt.catch_var);
                    bound_local = true;
                } else if (node->try_stmt.catch_var) {
                    emit_byte(compiler, BC_DEFINE_GLOBAL);
                    ObjString *s = copy_string(node->try_stmt.catch_var,
                                               (int)strlen(node->try_stmt.catch_var));
                    int idx = chunk_add_constant(compiler->chunk, val_string(s));
                    emit_short(compiler, (uint16_t)idx);
                } else {
                    emit_byte(compiler, BC_POP);
                }
                compile_node(compiler, node->try_stmt.catch_body);
                if (bound_local) {
                    emit_byte(compiler, BC_POP);
                    compiler->local_count = saved_local_count;
                }
            }
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

/* ─── Per-task macros (used inside task_run) ─── */
/* These require local variables: task (Task*), vm (VM*) */
#define TASK_READ_BYTE()  (*task->frames[task->frame_count - 1].ip++)
#define TASK_READ_SHORT() \
    (task->frames[task->frame_count - 1].ip += 2, \
     (uint16_t)((task->frames[task->frame_count - 1].ip[-2] << 8) | \
                task->frames[task->frame_count - 1].ip[-1]))
#define TASK_READ_CONSTANT() (task->frames[task->frame_count - 1].function->constants[TASK_READ_SHORT()])
#define TASK_PUSH(v) do { \
    task->stack[task->stack_top] = (v); \
    task->stack_top++; \
} while (0)
#define TASK_POP() (task->stack[--task->stack_top])
#define TASK_PEEK(n) (task->stack[task->stack_top - 1 - (n)])

/* ─── Task allocation (with free-list) ─── */
Task *task_new(VM *vm) {
    Task *t = vm->free_tasks;
    if (t) {
        vm->free_tasks = (Task *)t->http_response_ssl;
        char *saved_arena = t->arena_base;
        memset(t, 0, sizeof(Task));
        t->arena_base = saved_arena;
        t->arena_offset = 0;
        t->dead = false;
        t->http_listen_fd = -1;
        t->http_response_fd = -1;
        t->http_response_ssl = NULL;
        t->http_pending_conns = NULL;
        t->wakeup_time = 0.0;
        return t;
    }
    t = (Task *)calloc(1, sizeof(Task));
    if (!t) return NULL;
    t->dead = false;
    t->http_listen_fd = -1;
    t->http_response_fd = -1;
    t->http_response_ssl = NULL;
    t->http_pending_conns = NULL;
    t->wakeup_time = 0.0;
    t->arena_base = NULL;
    t->arena_offset = 0;
    t->use_arena = false;
    return t;
}

void vm_register_task(VM *vm, Task *t) {
    if (vm->task_count >= vm->task_capacity) {
        int new_cap = vm->task_capacity ? vm->task_capacity * 2 : 8;
        Task **new_tasks = (Task **)realloc(vm->tasks, (size_t)new_cap * sizeof(Task *));
        if (!new_tasks) return;
        vm->tasks = new_tasks;
        vm->task_capacity = new_cap;
    }
    t->id = vm->task_count;
    vm->tasks[vm->task_count++] = t;
}

void vm_init(VM *vm, Compiler *compiler) {
    vm->tasks = NULL;
    vm->task_count = 0;
    vm->task_capacity = 0;
    vm->current_task = NULL;
    vm->current_task_index = 0;
    vm->objects = NULL;
    vm->global_count = 0;
    vm->compiler = compiler;
    vm->had_error = false;
    vm->suppress_error_print = false;
    vm->last_error[0] = '\0';
    vm->main_fn = NULL;
    vm->io_activity_this_tick = false;
    vm->free_tasks = NULL;
    vm->source = NULL;
    vm->source_name = NULL;
    memset(vm->dispatch_occupied, 0, sizeof(vm->dispatch_occupied));
    vm->gray_stack = NULL;
    vm->gray_capacity = 0;
    vm->gray_count = 0;
    vm->bytes_allocated = 0;
    vm->next_gc_size = 1024 * 1024;
    vm->intern_table = NULL;
    vm->intern_capacity = 0;
    vm->intern_count = 0;
    memset(vm->globals, 0, sizeof(vm->globals));
    vm->ffi_entries = NULL;
    vm->ffi_entry_count = 0;
    vm->actor_field_count = 0;
    vm->cache_map = NULL;
    vm->cache_map_count = 0;
    vm->cache_map_capacity = 0;
    vm->test_count = 0;
    memset(vm->tests, 0, sizeof(vm->tests));
    vm->validation_registry.count = 0;
    vm->test_filter = NULL;
    vm->test_skip_count = 0;
    vm->test_timeout_ms = 0;
    vm->test_timeout_count = 0;
    vm->deadline_us = 0;
    vm->timed_out = false;
    vm->loop_tick = 0;
}

/* ─── FFI Resolution ─── */
/* Called at the start of vm_run to resolve all FFI declarations */
static bool vm_resolve_ffi(VM *vm) {
    Compiler *compiler = vm->compiler;
    if (compiler->ffi_decl_count == 0) return true;

    vm->ffi_entries = (VMFFIEntry *)calloc((size_t)compiler->ffi_decl_count, sizeof(VMFFIEntry));
    if (!vm->ffi_entries) return false;
    vm->ffi_entry_count = compiler->ffi_decl_count;

    for (int i = 0; i < compiler->ffi_decl_count; i++) {
        FFIDecl *decl = &compiler->ffi_decls[i];
        VMFFIEntry *entry = &vm->ffi_entries[i];

        void *lib = ffi_open_lib(decl->lib_name);
        if (!lib) {
            fprintf(stderr, "FFI: Cannot open library '%s'\n", decl->lib_name);
            return false;
        }

        void *fn_ptr = ffi_find_sym(lib, decl->func_name);
        if (!fn_ptr) {
            fprintf(stderr, "FFI: Cannot find symbol '%s' in '%s'\n",
                    decl->func_name, decl->lib_name);
            return false;
        }

        if (!ffi_entry_init(entry, fn_ptr, decl->return_kind,
                           decl->param_kinds, decl->param_count)) {
            fprintf(stderr, "FFI: Failed to init call interface for '%s'\n", decl->func_name);
            return false;
        }
    }

    return true;
}

void runtime_error(VM *vm, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(vm->last_error, sizeof(vm->last_error), format, args);
    va_end(args);
    if (!vm->suppress_error_print) {
        fprintf(stderr, "%s\n", vm->last_error);
    }
    Task *t = vm->current_task;
    if (t) {
        for (int i = t->frame_count - 1; i >= 0; i--) {
            CallFrame *frame = &t->frames[i];
            int offset = (int)(frame->ip - frame->function->code - 1);
            if (offset < 0) offset = 0;
            int line = 0;
            if (frame->function == vm->main_fn) {
                line = chunk_get_line(vm->compiler->chunk, offset);
            } else {
                ObjFunction *fn = frame->function;
                int pos = 0;
                for (int j = 0; j < fn->rle_count; j++) {
                    pos += fn->rle_counts[j];
                    if (offset < pos) { line = fn->rle_lines[j]; break; }
                }
            }
            if (!vm->suppress_error_print) {
                fprintf(stderr, "[line %d] in script\n", line);
                if (vm->source && line >= 1) {
                    const char *p = vm->source;
                    int cur = 1;
                    while (cur < line && *p) {
                        if (*p == '\n') cur++;
                        p++;
                    }
                    const char *end = p;
                    while (*end && *end != '\n') end++;
                    fprintf(stderr, "    %.*s\n", (int)(end - p), p);
                }
            }
        }
    }
    vm->had_error = true;
}

void define_global(VM *vm, ObjString *name, Value value) {
    value = escape_promote(vm, value);
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

Value get_global(VM *vm, ObjString *name) {
    for (int i = 0; i < vm->global_count; i++) {
        if (strcmp(vm->global_names[i], name->chars) == 0)
            return vm->globals[i];
    }
    runtime_error(vm, "Undefined variable '%s'", name->chars);
    return val_nil();
}

void set_global(VM *vm, ObjString *name, Value value) {
    value = escape_promote(vm, value);
    for (int i = 0; i < vm->global_count; i++) {
        if (strcmp(vm->global_names[i], name->chars) == 0) {
            vm->globals[i] = value;
            return;
        }
    }
    runtime_error(vm, "Undefined variable '%s'", name->chars);
}

static Value native_test_enable_arena(VM *vm, int arg_count, Value *args) {
    (void)arg_count; (void)args;
    if (vm->current_task) {
        task_arena_enable(vm->current_task);
    }
    return val_nil();
}

/* Test-only: simulates exactly what the round-robin scheduler's end-of-pass
 * reap does to a recycled Task (see vm_run's "Push to free-list" comment) --
 * resets arena_offset to 0 so the NEXT arena allocation overwrites the same
 * bytes a previous request's arena-backed objects used to occupy, then
 * re-enables the arena for further allocations. Lets a single-task test
 * script exercise the real "does a promoted reference survive a recycle"
 * scenario without needing a full http.serve() round-robin pass. */
static Value native_test_recycle_arena(VM *vm, int arg_count, Value *args) {
    (void)arg_count; (void)args;
    Task *t = vm->current_task;
    if (t && t->arena_base) {
        t->arena_offset = 0;
        t->use_arena = true;
    }
    return val_nil();
}

static Value native_print(VM *vm, int arg_count, Value *args) {
    (void)vm;
    for (int i = 0; i < arg_count; i++) {
        value_print(args[i]);
        if (i < arg_count - 1) printf(" ");
    }
    if (arg_count > 0) printf("\n");
    fflush(stdout);
    return val_nil();
}

static Value native_throw(VM *vm, int arg_count, Value *args) {
    Task *t = vm->current_task;
    if (t) {
        t->throw_value = (arg_count > 0) ? args[0] : val_nil();
        t->is_throwing = true;
    }
    return val_nil();
}

static char *stringify_value(VM *vm, Value v) {
    int len = 0;
    char *s = json_encode(vm, v, &len);
    if (!s) {
        s = strdup("nil");
    }
    return s;
}

static Value native_assert_eq(VM *vm, int arg_count, Value *args) {
    if (arg_count < 2) {
        runtime_error(vm, "assert_eq requires 2 arguments");
        return val_nil();
    }
    if (!value_equal(args[0], args[1])) {
        char *s1 = stringify_value(vm, args[0]);
        char *s2 = stringify_value(vm, args[1]);
        runtime_error(vm, "assert_eq failed: expected %s, got %s", s1, s2);
        free(s1);
        free(s2);
    }
    return val_nil();
}

static Value native_assert_ne(VM *vm, int arg_count, Value *args) {
    if (arg_count < 2) {
        runtime_error(vm, "assert_ne requires 2 arguments");
        return val_nil();
    }
    if (value_equal(args[0], args[1])) {
        char *s1 = stringify_value(vm, args[0]);
        char *s2 = stringify_value(vm, args[1]);
        runtime_error(vm, "assert_ne failed: expected %s to not equal %s", s1, s2);
        free(s1);
        free(s2);
    }
    return val_nil();
}

static Value native_assert_throws(VM *vm, int arg_count, Value *args) {
    if (arg_count < 1 || (args[0].type != VAL_FUNCTION && args[0].type != VAL_CLOSURE)) {
        runtime_error(vm, "assert_throws requires a function or closure");
        return val_nil();
    }
    Value callee = args[0];
    ObjFunction *fn = (callee.type == VAL_CLOSURE) ? callee.as.closure->function : callee.as.function;

    Task *new_t = task_new(vm);
    if (!new_t) return val_nil();
    new_t->frames[0].function = fn;
    new_t->frames[0].closure = (callee.type == VAL_CLOSURE) ? callee.as.closure : NULL;
    new_t->frames[0].ip = fn->code;
    new_t->frames[0].slots = new_t->stack;
    new_t->frames[0].return_base = 0;
    new_t->frame_count = 1;

    Task *prev = vm->current_task;
    vm->current_task = new_t;
    bool old_suppress = vm->suppress_error_print;
    bool old_had_error = vm->had_error;
    vm->suppress_error_print = true;
    vm->had_error = false;

    task_run(vm, new_t);

    bool threw = vm->had_error || new_t->is_throwing || new_t->dead; // if it exited early due to error
    vm->had_error = old_had_error;
    vm->suppress_error_print = old_suppress;
    vm->current_task = prev;

    new_t->dead = true;

    if (!threw) {
        runtime_error(vm, "assert_throws failed: function did not throw an exception");
    } else {
        // Clear any error string set by the throws inside the function so it doesn't leak out
        vm->last_error[0] = '\0';
    }
    return val_nil();
}

/* Builtin: convert a C pointer (int) to a managed ObjString */
static Value native_ffi_to_string(VM *vm, int arg_count, Value *args) {
    if (arg_count < 1 || args[0].type != VAL_INT)
        return val_nil();
    const char *cstr = (const char *)(uintptr_t)args[0].as.integer;
    if (!cstr) return val_nil();
    size_t len = strlen(cstr);
    ObjString *s = allocate_string(vm, cstr, (int)len);
    return val_string(s);
}

/* Forward declarations needed by actor functions */
Value *vm_find_dispatch(VM *vm, const char *type_name, const char *method_name);
bool task_run(VM *vm, Task *task);

/* ─── Channel helpers (used by actor system) ─── */
bool channel_try_receive(ObjChannel *ch, Value *result) {
    if (ch->count > 0) {
        *result = ch->buffer[ch->head];
        ch->head = (ch->head + 1) % ch->capacity;
        ch->count--;
        return true;
    }
    return false;
}

bool channel_try_send(ObjChannel *ch, Value val) {
    if (ch->count < ch->capacity) {
        ch->buffer[ch->tail] = val;
        ch->tail = (ch->tail + 1) % ch->capacity;
        ch->count++;
        return true;
    }
    return false;
}

/* ─── Actor functions ─── */
/* Called by the scheduler to process one message for an actor loop task.
   Creates a temporary task to execute the method and sends the result
   to the reply channel. The inbox channel must have a message. */
static bool actor_scheduler_process(VM *vm, Task *task) {
    ObjActor *actor = task->actor_ref;
    if (!actor) return false;

    Value msg_val;
    if (!channel_try_receive(actor->inbox.as.channel, &msg_val))
        return false;

    if (msg_val.type != VAL_TUPLE) return false;
    ObjTuple *msg = msg_val.as.tuple;
    if (msg->count < 3) return false;

    char *method_name = msg->elements[0].as.string->chars;
    ObjArray *msg_args = msg->elements[1].as.array;
    Value reply_ch = msg->elements[2];

    Value *func_val = vm_find_dispatch(vm, actor->type_name, method_name);
    if (!func_val || func_val->type != VAL_FUNCTION)
        return false;

    ObjFunction *method_fn = func_val->as.function;

    Task *tmp_task = task_new(vm);
    tmp_task->stack[tmp_task->stack_top++] = actor->state;
    for (int i = 0; i < msg_args->count; i++)
        tmp_task->stack[tmp_task->stack_top++] = msg_args->elements[i];

    tmp_task->frames[0].function = method_fn;
    tmp_task->frames[0].closure = NULL;
    tmp_task->frames[0].ip = method_fn->code;
    tmp_task->frames[0].slots = tmp_task->stack;
    tmp_task->frames[0].return_base = 0;
    tmp_task->frame_count = 1;

    Task *prev = vm->current_task;
    vm->current_task = tmp_task;
    task_run(vm, tmp_task);
    vm->current_task = prev;

    Value result = val_nil();
    if (tmp_task->stack_top > 0)
        result = tmp_task->stack[tmp_task->stack_top - 1];

    if (reply_ch.type == VAL_CHANNEL)
        channel_try_send(reply_ch.as.channel, result);

    tmp_task->dead = true;
    return true;
}

/* actor_spawn_native is registered as the "spawn" method on each actor module.
   It creates the ObjActor, its inner VAL_STRUCT state, inbox channel,
   and background loop task. */
Value actor_spawn_native(VM *vm, int arg_count, Value *args) {
    if (arg_count < 1 || args[0].type != VAL_MODULE) {
        runtime_error(vm, "actor.spawn: expected module as first argument");
        return val_nil();
    }
    char *type_name = args[0].as.module->name;

    /* Find actor field info */
    ActorFieldInfo *info = NULL;
    for (int i = 0; i < vm->actor_field_count; i++) {
        if (strcmp(vm->actor_fields[i].type_name, type_name) == 0) {
            info = &vm->actor_fields[i];
            break;
        }
    }
    if (!info) {
        runtime_error(vm, "actor.spawn: unknown actor type '%s'", type_name);
        return val_nil();
    }

    /* 1. Create inner state struct */
    ObjStruct *state = new_struct(vm, info->field_count, false);
    state->type_name = (char *)malloc(strlen(type_name) + 1);
    strcpy(state->type_name, type_name);
    for (int i = 0; i < info->field_count; i++) {
        state->field_names[i] = (char *)malloc(strlen(info->field_names[i]) + 1);
        strcpy(state->field_names[i], info->field_names[i]);
        state->fields[i] = val_nil();
    }

    /* 2. Create inbox channel (capacity 64) */
    ObjChannel *inbox = (ObjChannel *)calloc(1, sizeof(ObjChannel));
    inbox->obj.type = VAL_CHANNEL;
    inbox->capacity = 64;
    inbox->buffer = (Value *)calloc(64, sizeof(Value));

    /* 3. Create ObjActor */
    ObjActor *obj_actor = (ObjActor *)calloc(1, sizeof(ObjActor));
    obj_actor->obj.type = VAL_ACTOR;
    obj_actor->type_name = (char *)malloc(strlen(type_name) + 1);
    strcpy(obj_actor->type_name, type_name);
    obj_actor->state = val_struct(state);
    obj_actor->inbox = val_channel(inbox);

    /* Link into GC */
    inbox->obj.next = vm->objects;
    vm->objects = (Obj *)inbox;
    obj_actor->obj.next = vm->objects;
    vm->objects = (Obj *)obj_actor;

    /* Create a background task for the actor loop */
    Task *loop_task = task_new(vm);
    loop_task->is_actor_loop = true;
    loop_task->actor_ref = obj_actor;
    obj_actor->loop_task = loop_task;
    vm_register_task(vm, loop_task);

    return val_actor(obj_actor);
}

/* Sequential FNV-1a hash: hashes two strings without allocation */
static uint32_t fnv1a_hash_two(const char *a, const char *b) {
    uint32_t hash = 2166136261u;
    while (*a) { hash ^= (uint8_t)(*a); hash *= 16777619u; a++; }
    hash ^= (uint8_t)':'; hash *= 16777619u;
    while (*b) { hash ^= (uint8_t)(*b); hash *= 16777619u; b++; }
    return hash;
}

/* Dispatch table helpers — FNV-1a open-addressing hash table */
void vm_register_dispatch(VM *vm, const char *type_name, const char *method_name, Value func) {
    uint32_t hash = fnv1a_hash_two(type_name, method_name);

    for (int i = 0; i < DISPATCH_TABLE_SIZE; i++) {
        int idx = (hash + i) % DISPATCH_TABLE_SIZE;
        if (vm->dispatch_occupied[idx]) {
            if (strcmp(vm->dispatch_type_names[idx], type_name) == 0 &&
                strcmp(vm->dispatch_method_names[idx], method_name) == 0) {
                vm->dispatch_functions[idx] = func;
                return;
            }
        } else {
            strncpy(vm->dispatch_type_names[idx], type_name, 63);
            vm->dispatch_type_names[idx][63] = '\0';
            strncpy(vm->dispatch_method_names[idx], method_name, 63);
            vm->dispatch_method_names[idx][63] = '\0';
            vm->dispatch_functions[idx] = func;
            vm->dispatch_occupied[idx] = true;
            return;
        }
    }
}

Value *vm_find_dispatch(VM *vm, const char *type_name, const char *method_name) {
    /* type_name is NULL for anonymous structs (e.g. http.create_struct()
     * results) -- never registered in the dispatch table, so this is
     * always a clean "not found" rather than a NULL-deref in the hash. */
    if (!type_name) return NULL;
    uint32_t hash = fnv1a_hash_two(type_name, method_name);

    for (int i = 0; i < DISPATCH_TABLE_SIZE; i++) {
        int idx = (hash + i) % DISPATCH_TABLE_SIZE;
        if (!vm->dispatch_occupied[idx])
            return NULL;
        if (strcmp(vm->dispatch_type_names[idx], type_name) == 0 &&
            strcmp(vm->dispatch_method_names[idx], method_name) == 0) {
            return &vm->dispatch_functions[idx];
        }
    }
    return NULL;
}

/* ─── Forward declarations ─── */

bool vm_run(VM *vm, bool run_tests) {
    vm->test_fail_count = 0;
    /* Create main script function if not already loaded (e.g. via AOT) */
    if (!vm->main_fn) {
        vm->main_fn = (ObjFunction *)calloc(1, sizeof(ObjFunction));
        vm->main_fn->obj.type = VAL_FUNCTION;
        vm->main_fn->code = vm->compiler->chunk->code;
        vm->main_fn->code_count = vm->compiler->chunk->count;
        vm->main_fn->code_capacity = vm->compiler->chunk->capacity;
        vm->main_fn->constants = vm->compiler->chunk->constants;
        vm->main_fn->constant_count = vm->compiler->chunk->constant_count;
        vm->main_fn->constant_capacity = vm->compiler->chunk->constant_capacity;
    }

    /* Create and set up the initial task if none registered */
    if (vm->task_count == 0) {
        Task *init_task = task_new(vm);
        if (!init_task) return false;
        init_task->frames[0].function = vm->main_fn;
        init_task->frames[0].closure = NULL;
        init_task->frames[0].ip = vm->main_fn->code;
        init_task->frames[0].slots = NULL;
        init_task->frames[0].return_base = 0;
        init_task->frame_count = 1;
        vm_register_task(vm, init_task);
        vm->current_task = init_task;
    }

    define_global(vm, copy_string("print", 5), val_native_fn((void *)native_print));
    define_global(vm, copy_string("__test_enable_arena", 19), val_native_fn((void *)native_test_enable_arena));
    define_global(vm, copy_string("__test_recycle_arena", 20), val_native_fn((void *)native_test_recycle_arena));
    define_global(vm, copy_string("throw", 5), val_native_fn((void *)native_throw));
    define_global(vm, copy_string("ffi_to_string", 13), val_native_fn((void *)native_ffi_to_string));
    define_global(vm, copy_string("assert_eq", 9), val_native_fn((void *)native_assert_eq));
    define_global(vm, copy_string("assert_ne", 9), val_native_fn((void *)native_assert_ne));
    define_global(vm, copy_string("assert_throws", 13), val_native_fn((void *)native_assert_throws));
    extern Value native_json_encode(VM *vm, int arg_count, Value *args);
    extern Value native_json_decode(VM *vm, int arg_count, Value *args);
    define_global(vm, copy_string("json_encode", 11), val_native_fn((void *)native_json_encode));
    define_global(vm, copy_string("json_decode", 11), val_native_fn((void *)native_json_decode));

    /* Initialize built-in modules */
    {
        extern void lib_math_init(VM *vm);
        extern void lib_env_init(VM *vm);
        extern void lib_io_init(VM *vm);
        extern void lib_string_init(VM *vm);
        extern void lib_http_init(VM *vm);
        extern void lib_python_init(VM *vm);
        extern void lib_postgres_init(VM *vm);
        extern void lib_validate_init(VM *vm);
        extern void lib_sanitize_init(VM *vm);
        extern void lib_auth_init(VM *vm);
        extern void lib_task_init(VM *vm);
        extern void lib_sqlite_init(VM *vm);
        extern void lib_redis_init(VM *vm);
        extern void lib_mock_init(VM *vm);
        extern void lib_smtp_init(VM *vm);
        extern void lib_time_init(VM *vm);
        extern void lib_regex_init(VM *vm);
        extern void lib_errors_init(VM *vm);
        lib_math_init(vm);
        lib_env_init(vm);
        lib_io_init(vm);
        lib_string_init(vm);
        lib_http_init(vm);
        lib_python_init(vm);
        lib_postgres_init(vm);
        lib_validate_init(vm);
        lib_sanitize_init(vm);
        lib_auth_init(vm);
        lib_task_init(vm);
        lib_sqlite_init(vm);
        lib_redis_init(vm);
        lib_mock_init(vm);
        lib_smtp_init(vm);
        lib_time_init(vm);
        lib_regex_init(vm);
        lib_errors_init(vm);
    }

#define BINARY_OP_NUM(op) \
    do { \
        Value b = TASK_POP(); \
        Value a = TASK_POP(); \
        if (a.type == VAL_INT && b.type == VAL_INT) { \
            TASK_PUSH(val_int(a.as.integer op b.as.integer)); \
        } else { \
            double bv = (b.type == VAL_FLOAT) ? b.as.floating : (double)b.as.integer; \
            double av = (a.type == VAL_FLOAT) ? a.as.floating : (double)a.as.integer; \
            TASK_PUSH(val_float(av op bv)); \
        } \
    } while (0)

    /* Resolve FFI declarations */
    if (!vm_resolve_ffi(vm)) {
        return false;
    }

    /* ─── Round-robin scheduler ─── */
    while (true) {
        /* GC safepoint: every task is between ticks here (none mid-
         * execution, nothing native-code-constructed-but-not-yet-rooted in
         * flight) -- see allocate_object()'s comment for why collection
         * only happens here now instead of self-triggering from inside an
         * allocation. */
        if (vm->bytes_allocated >= vm->next_gc_size && vm->next_gc_size > 0) {
            gc_collect(vm);
        }

        bool any_alive = false;
        bool made_progress = false;
        vm->io_activity_this_tick = false;
        for (int i = 0; i < vm->task_count; i++) {
            Task *t = vm->tasks[i];
            if (t->dead) continue;
            /* Actor loop tasks don't keep the process alive by themselves */
            if (!t->is_actor_loop)
                any_alive = true;

            if (t->wakeup_time > 0) {
                struct timeval tv;
                gettimeofday(&tv, NULL);
                double now = tv.tv_sec + tv.tv_usec / 1000000.0;
                if (now < t->wakeup_time) continue;
                t->wakeup_time = 0;
            }

            if (t->is_actor_loop) {
                /* Native actor loop: process one message if available */
                if (actor_scheduler_process(vm, t)) {
                    made_progress = true;
                }
            } else {
                /* Normal Varian task */
                vm->current_task = t;
                vm->current_task_index = i;
                t->yielded = false;
                vm->had_error = false;
                uint8_t *prev_ip = NULL;
                if (t->frame_count > 0) {
                    prev_ip = t->frames[t->frame_count - 1].ip;
                }
                task_run(vm, t);
                bool tick_had_error = vm->had_error;
                if (vm->had_error) {
                    t->dead = true;
                    vm->had_error = false; /* don't let one task's error kill the next */
                }
                if (t->dead && t->http_response_fd >= 0) {
                    /* A long-lived HTTP handler (WebSocket/SSE) that had to
                     * yield mid-execution -- see call_handler() in
                     * lib_http.c -- has now actually finished, normally or
                     * via an uncaught error partway through (e.g. a bad
                     * message on an open WebSocket). Either way this must
                     * not crash the server -- just finalize this one
                     * connection's response/close. */
                    extern void http_finalize_deferred_response(VM *vm, Task *t, bool had_error);
                    http_finalize_deferred_response(vm, t, tick_had_error);
                }
                if (t->dead || t->frame_count == 0 || (t->frame_count > 0 && t->frames[t->frame_count - 1].ip != prev_ip)) {
                    made_progress = true;
                }
            }
        }

        /* Reap every task that died this pass. Nothing previously freed a
         * Task's memory at all -- every task.spawn()'d task and every HTTP
         * request handler (see call_handler in lib_http.c) leaked a multi-KB
         * Task struct (TASK_STACK_SIZE Values + TASK_FRAMES_MAX call frames)
         * forever, which under sustained request volume is a real
         * unbounded-memory-growth bug, not a rounding error. Safe to do
         * here, once per full pass over every task, rather than the instant
         * a task is noticed dead: anything that still needed a value off a
         * dying task's stack (e.g. http_finalize_deferred_response reading
         * the handler's return value, or this same pass's send_http_response
         * for a synchronously-completed handler) has already finished using
         * it by the time we get here. */
        int reap_write = 0;
        for (int ri = 0; ri < vm->task_count; ri++) {
            Task *rt = vm->tasks[ri];
            if (rt->dead) {
                if (rt->http_pending_conns) {
                    extern void http_cleanup_pending_conns(Task *t);
                    http_cleanup_pending_conns(rt); /* closes fds, frees buffers */
                }
                /* Push to free-list instead of free() — Phase 1: Task Pooling */
                /* Phase 2: reset arena (keep the base allocation for reuse) */
                rt->arena_offset = 0;
                rt->use_arena = false;
                rt->http_response_ssl = (void *)vm->free_tasks; /* repurpose as next */
                vm->free_tasks = rt;
            } else {
                vm->tasks[reap_write++] = rt;
            }
        }
        vm->task_count = reap_write;

        /* Stop only when no tasks remain alive */
        if (!any_alive) break;

        if (!made_progress && !vm->io_activity_this_tick) {
            usleep(1000); // Sleep for 1ms to prevent hot spinning when truly idle
        }
    }

    /* ─── Test execution mini-scheduler ─── */
    if (run_tests) {
        for (int i = 0; i < vm->test_count; i++) {
            TestRecord *tr = &vm->tests[i];

            if (vm->test_filter && (!tr->description || !strstr(tr->description, vm->test_filter))) {
                vm->test_skip_count++;
                continue;
            }

            Task *t = task_new(vm);
            if (!t) continue;
            vm_register_task(vm, t);
            t->frames[0].function = tr->func;
            t->frames[0].closure = NULL;
            t->frames[0].ip = tr->func->code;
            t->frames[0].slots = t->stack;
            t->frames[0].return_base = 0;
            t->frame_count = 1;

            vm->had_error = false;
            vm->timed_out = false;

            /* Optional per-test wall-clock deadline (vn test --timeout N).
             * task_run() enforces it on loop back-edges; the yield-point
             * check below covers tests that block in native I/O instead. */
            vm->deadline_us = 0;
            if (vm->test_timeout_ms > 0) {
                struct timeval tv;
                gettimeofday(&tv, NULL);
                vm->deadline_us = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec +
                                  (int64_t)vm->test_timeout_ms * 1000;
            }

            /* Mini-scheduler: handle yields from await, actors, etc. */
            while (!t->dead && !vm->had_error) {
                t->yielded = false;
                vm->current_task = t;
                task_run(vm, t);

                /* Run background tasks (e.g. actor loops) so the test doesn't deadlock */
                for (int j = 0; j < vm->task_count; j++) {
                    Task *other = vm->tasks[j];
                    if (other != t && !other->dead) {
                        if (other->is_actor_loop) {
                            actor_scheduler_process(vm, other);
                        } else {
                            other->yielded = false;
                            vm->current_task = other;
                            task_run(vm, other);
                        }
                    }
                }

                if (vm->deadline_us && !vm->timed_out) {
                    struct timeval now;
                    gettimeofday(&now, NULL);
                    int64_t now_us = (int64_t)now.tv_sec * 1000000 + now.tv_usec;
                    if (now_us >= vm->deadline_us) { vm->timed_out = true; break; }
                }
            }

            vm->deadline_us = 0;

            if (vm->timed_out) {
                printf("  ⏱️  TIMEOUT: %s (exceeded %dms)\n",
                       tr->description, vm->test_timeout_ms);
                /* Kill the aborted test's task and any background tasks it
                 * spawned so a runaway loop can't bleed into later tests. */
                for (int j = 0; j < vm->task_count; j++) {
                    if (vm->tasks[j]) vm->tasks[j]->dead = true;
                }
                vm->had_error = false;
                vm->last_error[0] = '\0';
                vm->test_fail_count++;
                vm->test_timeout_count++;
            } else if (vm->had_error) {
                printf("  \u274c FAIL: %s\n", tr->description);
                if (vm->last_error[0] != '\0') {
                    printf("     %s\n", vm->last_error);
                }
                vm->had_error = false; /* reset for next test */
                vm->last_error[0] = '\0';
                vm->test_fail_count++;
            } else {
                printf("  \u2705 PASS: %s\n", tr->description);
            }
        }
    }

    return !vm->had_error && vm->test_fail_count == 0;
}

/* ─── Metadata decorator helpers ─── */
/* Check if a Value (metadata array) contains a given key with a truthy value */
static bool metadata_has(Value metadata, const char *key) {
    if (metadata.type != VAL_ARRAY) return false;
    ObjArray *arr = metadata.as.array;
    for (int i = 0; i < arr->count; i += 2) {
        if (arr->elements[i].type == VAL_STRING &&
            strcmp(arr->elements[i].as.string->chars, key) == 0) {
            return value_is_truthy(arr->elements[i + 1]);
        }
    }
    return false;
}

/* Get the value associated with a metadata key */
Value metadata_get(Value metadata, const char *key) {
    if (metadata.type != VAL_ARRAY) return val_nil();
    ObjArray *arr = metadata.as.array;
    for (int i = 0; i < arr->count; i += 2) {
        if (arr->elements[i].type == VAL_STRING &&
            strcmp(arr->elements[i].as.string->chars, key) == 0) {
            return arr->elements[i + 1];
        }
    }
    return val_nil();
}

/* Cache map helpers */
static int cache_map_find(VM *vm, uint64_t key_hash) {
    for (int i = 0; i < vm->cache_map_count; i += 2) {
        if (vm->cache_map[i].type == VAL_INT &&
            vm->cache_map[i].as.integer == (int64_t)key_hash) {
            return i;
        }
    }
    return -1;
}

void cache_map_put(VM *vm, uint64_t key_hash, Value result) {
    if (vm->cache_map_count + 2 > vm->cache_map_capacity) {
        int new_cap = vm->cache_map_capacity ? vm->cache_map_capacity * 2 : 16;
        vm->cache_map = (Value *)realloc(vm->cache_map, new_cap * sizeof(Value));
        vm->cache_map_capacity = new_cap;
    }
    vm->cache_map[vm->cache_map_count++] = val_int((int64_t)key_hash);
    vm->cache_map[vm->cache_map_count++] = result;
}

/* Compute a simple hash for cache key: fn ptr + arg values */
static uint64_t cache_key_hash(ObjFunction *fn, int arg_count, Value *args) {
    uint64_t h = (uint64_t)(uintptr_t)fn;
    for (int i = 0; i < arg_count; i++) {
        h ^= (uint64_t)args[i].type << (i * 8);
        if (args[i].type == VAL_INT)
            h ^= (uint64_t)args[i].as.integer;
    }
    return h;
}

static bool handle_vm_exception(VM *vm, const char *msg) {
    Task *t = vm->current_task;
    if (t && t->try_count > 0) {
        t->try_count--;
        int target_frame = t->try_stack[t->try_count].frame_index;
        t->frame_count = target_frame + 1;
        t->stack_top = t->try_stack[t->try_count].stack_depth;
        t->stack[t->stack_top++] = val_string(copy_string(msg, (int)strlen(msg)));
        t->frames[target_frame].ip =
            t->frames[target_frame].function->code +
            t->try_stack[t->try_count].catch_offset;
        return true;
    }
    return false;
}

bool task_run(VM *vm, Task *task) {
    Task * const t = task;
    (void)vm;
    (void)t;

#define READ_BYTE()  TASK_READ_BYTE()
#define READ_SHORT() TASK_READ_SHORT()
#define READ_CONSTANT() TASK_READ_CONSTANT()
#define PUSH(v) TASK_PUSH(v)
#define POP() TASK_POP()
#define PEEK(n) TASK_PEEK(n)

    uint8_t instruction;
    while (!vm->had_error && !t->dead && !t->yielded) {
        CallFrame *frame = &t->frames[t->frame_count - 1];
        if (frame->function->aot_func) {
            frame->function->aot_func(vm, t);
            continue;
        }
            static void *dispatch_table[256];
    static _Atomic bool dispatch_table_initialized = false;
    static pthread_mutex_t dispatch_table_lock = PTHREAD_MUTEX_INITIALIZER;
    /* dispatch_table is one process-wide instance (static), but task_run()
     * is now called from multiple cluster worker threads (each with its
     * own VM, sharing this one C function) -- lock-guarded double-checked
     * init makes this race-free regardless of thread spawn ordering,
     * rather than relying on it happening to be safe in practice. The
     * atomic flag keeps the fast (already-initialized) path lock-free. */
    if (!atomic_load_explicit(&dispatch_table_initialized, memory_order_acquire)) {
    pthread_mutex_lock(&dispatch_table_lock);
    if (!atomic_load_explicit(&dispatch_table_initialized, memory_order_relaxed)) {
        for (int idx = 0; idx < 256; idx++) {
            dispatch_table[idx] = &&L_BC_DEFAULT;
        }
        dispatch_table[BC_CONSTANT] = &&L_BC_CONSTANT;
        dispatch_table[BC_NIL] = &&L_BC_NIL;
        dispatch_table[BC_TRUE] = &&L_BC_TRUE;
        dispatch_table[BC_FALSE] = &&L_BC_FALSE;
        dispatch_table[BC_POP] = &&L_BC_POP;
        dispatch_table[BC_ADD] = &&L_BC_ADD;
        dispatch_table[BC_SUB] = &&L_BC_SUB;
        dispatch_table[BC_MUL] = &&L_BC_MUL;
        dispatch_table[BC_DIV] = &&L_BC_DIV;
        dispatch_table[BC_MOD] = &&L_BC_MOD;
        dispatch_table[BC_NEGATE] = &&L_BC_NEGATE;
        dispatch_table[BC_NOT] = &&L_BC_NOT;
        dispatch_table[BC_EQUAL] = &&L_BC_EQUAL;
        dispatch_table[BC_NOT_EQUAL] = &&L_BC_NOT_EQUAL;
        dispatch_table[BC_LESS] = &&L_BC_LESS;
        dispatch_table[BC_GREATER] = &&L_BC_GREATER;
        dispatch_table[BC_LESS_EQUAL] = &&L_BC_LESS_EQUAL;
        dispatch_table[BC_GREATER_EQUAL] = &&L_BC_GREATER_EQUAL;
        dispatch_table[BC_AND] = &&L_BC_AND;
        dispatch_table[BC_OR] = &&L_BC_OR;
        dispatch_table[BC_NIL_COALESCE] = &&L_BC_NIL_COALESCE;
        dispatch_table[BC_DEFINE_GLOBAL] = &&L_BC_DEFINE_GLOBAL;
        dispatch_table[BC_GET_GLOBAL] = &&L_BC_GET_GLOBAL;
        dispatch_table[BC_SET_GLOBAL] = &&L_BC_SET_GLOBAL;
        dispatch_table[BC_GET_LOCAL] = &&L_BC_GET_LOCAL;
        dispatch_table[BC_SET_LOCAL] = &&L_BC_SET_LOCAL;
        dispatch_table[BC_JUMP] = &&L_BC_JUMP;
        dispatch_table[BC_JUMP_IF_FALSE] = &&L_BC_JUMP_IF_FALSE;
        dispatch_table[BC_JUMP_IF_NIL] = &&L_BC_JUMP_IF_NIL;
        dispatch_table[BC_LOOP] = &&L_BC_LOOP;
        dispatch_table[BC_CALL] = &&L_BC_CALL;
        dispatch_table[BC_RETURN] = &&L_BC_RETURN;
        dispatch_table[BC_RETURN_N] = &&L_BC_RETURN_N;
        dispatch_table[BC_GET_UPVALUE] = &&L_BC_GET_UPVALUE;
        dispatch_table[BC_SET_UPVALUE] = &&L_BC_SET_UPVALUE;
        dispatch_table[BC_CLOSURE] = &&L_BC_CLOSURE;
        dispatch_table[BC_ARRAY] = &&L_BC_ARRAY;
        dispatch_table[BC_TUPLE] = &&L_BC_TUPLE;
        dispatch_table[BC_INDEX] = &&L_BC_INDEX;
        dispatch_table[BC_SET_INDEX] = &&L_BC_SET_INDEX;
        dispatch_table[BC_MEMBER] = &&L_BC_MEMBER;
        dispatch_table[BC_MEMBER_SAFE] = &&L_BC_MEMBER_SAFE;
        dispatch_table[BC_SET_MEMBER] = &&L_BC_SET_MEMBER;
        dispatch_table[BC_DISPATCH] = &&L_BC_DISPATCH;
        dispatch_table[BC_REGISTER_METHOD] = &&L_BC_REGISTER_METHOD;
        dispatch_table[BC_STRUCT] = &&L_BC_STRUCT;
        dispatch_table[BC_ENUM] = &&L_BC_ENUM;
        dispatch_table[BC_PROPAGATE] = &&L_BC_PROPAGATE;
        dispatch_table[BC_UNPACK_ENUM] = &&L_BC_UNPACK_ENUM;
        dispatch_table[BC_TAG_EQ] = &&L_BC_TAG_EQ;
        dispatch_table[BC_THROW] = &&L_BC_THROW;
        dispatch_table[BC_TRY] = &&L_BC_TRY;
        dispatch_table[BC_POP_TRY] = &&L_BC_POP_TRY;
        dispatch_table[BC_FFI_CALL] = &&L_BC_FFI_CALL;
        dispatch_table[BC_COMPTIME_EXEC] = &&L_BC_COMPTIME_EXEC;
        dispatch_table[BC_AWAIT] = &&L_BC_AWAIT;
        dispatch_table[BC_CHAN_SEND] = &&L_BC_CHAN_SEND;
        dispatch_table[BC_CHAN_RECEIVE] = &&L_BC_CHAN_RECEIVE;
        dispatch_table[BC_ACTOR_INIT] = &&L_BC_ACTOR_INIT;
        dispatch_table[BC_PRINT] = &&L_BC_PRINT;
        dispatch_table[BC_STRING_CONCAT] = &&L_BC_STRING_CONCAT;
        dispatch_table[BC_BUILD_STRING] = &&L_BC_BUILD_STRING;
        dispatch_table[BC_INT_TO_STRING] = &&L_BC_INT_TO_STRING;
        dispatch_table[BC_CONSTANT_LONG] = &&L_BC_CONSTANT_LONG;
        dispatch_table[BC_ASSERT] = &&L_BC_ASSERT;
        dispatch_table[BC_HALT] = &&L_BC_HALT;
        dispatch_table[BC_REGISTER_VALIDATIONS] = &&L_BC_REGISTER_VALIDATIONS;
        atomic_store_explicit(&dispatch_table_initialized, true, memory_order_release);
    }
    pthread_mutex_unlock(&dispatch_table_lock);
    }

#define DISPATCH() \
    do { \
        if (vm->had_error || t->dead || t->yielded) goto end_vm; \
        CallFrame *frame = &t->frames[t->frame_count - 1]; \
        if (frame->function->aot_func) { \
            frame->function->aot_func(vm, t); \
            goto L_BC_LOOP_TOP; \
        } \
        instruction = READ_BYTE(); \
        goto *dispatch_table[instruction]; \
    } while (0)

L_BC_LOOP_TOP:
    DISPATCH();
            L_BC_CONSTANT:
            {
                Value constant = READ_CONSTANT();
                PUSH(constant);
                DISPATCH();
            }
            L_BC_CONSTANT_LONG:
            {
                uint16_t idx = READ_SHORT();
                Value constant = t->frames[t->frame_count - 1].function->constants[idx];
                PUSH(constant);
                DISPATCH();
            }
            L_BC_COMPTIME_EXEC:
            {
                /* Evaluated inline, in lexical position, so any function or
                 * global defined earlier in the program is already visible —
                 * unlike a startup pre-pass, which would run before any of
                 * the script's own top-level definitions existed. */
                uint16_t result_idx = READ_SHORT();
                uint16_t fn_idx = READ_SHORT();
                ObjFunction *cur_fn = t->frames[t->frame_count - 1].function;

                if (fn_idx >= cur_fn->constant_count ||
                    cur_fn->constants[fn_idx].type != VAL_FUNCTION) {
                    runtime_error(vm, "comptime: invalid fn constant");
                    return false;
                }
                ObjFunction *fn = cur_fn->constants[fn_idx].as.function;

                Task *tmp_task = task_new(vm);
                if (!tmp_task) {
                    runtime_error(vm, "comptime: failed to allocate task");
                    return false;
                }
                tmp_task->frames[0].function = fn;
                tmp_task->frames[0].closure = NULL;
                tmp_task->frames[0].ip = fn->code;
                tmp_task->frames[0].slots = tmp_task->stack;
                tmp_task->frames[0].return_base = 0;
                tmp_task->frame_count = 1;

                Task *prev = vm->current_task;
                vm->current_task = tmp_task;
                bool ok = task_run(vm, tmp_task);
                vm->current_task = prev;


                if (!ok) return false;

                Value result = val_nil();
                if (tmp_task->stack_top > 0)
                    result = tmp_task->stack[tmp_task->stack_top - 1];
                tmp_task->dead = true;
                tmp_task->arena_offset = 0;
                tmp_task->use_arena = false;
                tmp_task->http_response_ssl = (void *)vm->free_tasks;
                vm->free_tasks = tmp_task;

                cur_fn->constants[result_idx] = result;
                PUSH(result);
                DISPATCH();
            }
            L_BC_AWAIT:
            {
                Value v = POP();
                if (v.type != VAL_TASK) {
                    runtime_error(vm, "await requires a task value");
                    return false;
                }
                ObjTask *ot = v.as.task_obj;
                if (ot->task->dead) {
                    /* Task done — push its result */
                    PUSH(ot->task->result);
                } else {
                    /* Task not done — yield and retry this instruction */
                    PUSH(v);
                    t->yielded = true;
                    t->frames[t->frame_count - 1].ip--;
                }
                DISPATCH();
            }
            L_BC_CHAN_SEND:
            {
                Value val = POP();
                Value chan_v = POP();
                if (chan_v.type != VAL_CHANNEL) {
                    runtime_error(vm, "send requires a channel");
                    return false;
                }
                ObjChannel *ch = chan_v.as.channel;
                if (ch->closed) {
                    runtime_error(vm, "send on closed channel");
                    return false;
                }
                if (ch->count < ch->capacity) {
                    val = escape_promote(vm, val);
                    ch->buffer[ch->tail] = val;
                    ch->tail = (ch->tail + 1) % ch->capacity;
                    ch->count++;
                    PUSH(val_nil());
                } else {
                    /* Buffer full — retry later */
                    PUSH(chan_v);
                    PUSH(val);
                    t->yielded = true;
                    t->frames[t->frame_count - 1].ip--;
                }
                DISPATCH();
            }
            L_BC_CHAN_RECEIVE:
            {
                Value chan_v = POP();
                if (chan_v.type != VAL_CHANNEL) {
                    runtime_error(vm, "receive requires a channel");
                    return false;
                }
                ObjChannel *ch = chan_v.as.channel;
                if (ch->count > 0) {
                    Value result = ch->buffer[ch->head];
                    ch->head = (ch->head + 1) % ch->capacity;
                    ch->count--;
                    PUSH(result);
                } else if (ch->closed) {
                    PUSH(val_nil());
                } else {
                    /* Empty — retry later */
                    PUSH(chan_v);
                    t->yielded = true;
                    t->frames[t->frame_count - 1].ip--;
                }
                DISPATCH();
            }
            L_BC_ACTOR_INIT:
            {
                ObjString *type_name = READ_CONSTANT().as.string;
                uint8_t field_count = READ_BYTE();
                if (vm->actor_field_count < MAX_ACTOR_TYPES) {
                    ActorFieldInfo *info = &vm->actor_fields[vm->actor_field_count++];
                    strncpy(info->type_name, type_name->chars, 63);
                    info->type_name[63] = '\0';
                    info->field_count = field_count;
                    for (int i = 0; i < field_count; i++) {
                        ObjString *fname = READ_CONSTANT().as.string;
                        strncpy(info->field_names[i], fname->chars, 63);
                        info->field_names[i][63] = '\0';
                    }
                } else {
                    /* Skip field name constants */
                    for (int i = 0; i < field_count; i++)
                        (void)READ_BYTE();
                }
                ObjModule *mod = new_module(type_name->chars);
                mod->obj.next = vm->objects;
                vm->objects = (Obj *)mod;
                define_global(vm, copy_string(type_name->chars, type_name->length), val_module(mod));
                vm_register_dispatch(vm, type_name->chars, "spawn", val_native_fn((void *)actor_spawn_native));
                DISPATCH();
            }
            L_BC_NIL:
            PUSH(val_nil()); DISPATCH();
            L_BC_TRUE:
            PUSH(val_bool(true)); DISPATCH();
            L_BC_FALSE:
            PUSH(val_bool(false)); DISPATCH();
            L_BC_POP:
            (void)POP(); DISPATCH();

            L_BC_ADD:
            {
                Value b = POP();
                Value a = POP();
                if (a.type == VAL_STRING || b.type == VAL_STRING) {
                    char buf_a[64], buf_b[64];
                    const char *sa, *sb;
                    int la, lb;
                    if (a.type == VAL_STRING) { sa = a.as.string->chars; la = a.as.string->length; }
                    else {
                        switch (a.type) {
                            case VAL_INT:    la = snprintf(buf_a, sizeof(buf_a), "%ld", (long)a.as.integer); break;
                            case VAL_FLOAT:  la = snprintf(buf_a, sizeof(buf_a), "%g", a.as.floating); break;
                            case VAL_BOOL:   { const char *s = a.as.boolean ? "true" : "false"; la = (int)strlen(s); memcpy(buf_a, s, la + 1); break; }
                            default:         la = snprintf(buf_a, sizeof(buf_a), "<object>"); break;
                        }
                        sa = buf_a;
                    }
                    if (b.type == VAL_STRING) { sb = b.as.string->chars; lb = b.as.string->length; }
                    else {
                        switch (b.type) {
                            case VAL_INT:    lb = snprintf(buf_b, sizeof(buf_b), "%ld", (long)b.as.integer); break;
                            case VAL_FLOAT:  lb = snprintf(buf_b, sizeof(buf_b), "%g", b.as.floating); break;
                            case VAL_BOOL:   { const char *s = b.as.boolean ? "true" : "false"; lb = (int)strlen(s); memcpy(buf_b, s, lb + 1); break; }
                            default:         lb = snprintf(buf_b, sizeof(buf_b), "<object>"); break;
                        }
                        sb = buf_b;
                    }
                    int len = la + lb;
                    char *chars = (char *)malloc(len + 1);
                    memcpy(chars, sa, la);
                    memcpy(chars + la, sb, lb);
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
                DISPATCH();
            }
            L_BC_SUB:
            BINARY_OP_NUM(-); DISPATCH();
            L_BC_MUL:
            BINARY_OP_NUM(*); DISPATCH();
            L_BC_DIV:
            {
                Value b = POP();
                Value a = POP();
                if (a.type == VAL_INT && b.type == VAL_INT) {
                    if (b.as.integer == 0) {
                        if (handle_vm_exception(vm, "Division by zero")) {
                            DISPATCH();
                        }
                        runtime_error(vm, "Division by zero");
                        return false;
                    }
                    PUSH(val_int(a.as.integer / b.as.integer));
                } else {
                    double bv = (b.type == VAL_FLOAT) ? b.as.floating : (double)b.as.integer;
                    double av = (a.type == VAL_FLOAT) ? a.as.floating : (double)a.as.integer;
                    if (bv == 0.0) {
                        if (handle_vm_exception(vm, "Division by zero")) {
                            DISPATCH();
                        }
                        runtime_error(vm, "Division by zero");
                        return false;
                    }
                    PUSH(val_float(av / bv));
                }
                DISPATCH();
            }
            L_BC_MOD:
            {
                int64_t b = POP().as.integer;
                int64_t a = POP().as.integer;
                if (b == 0) {
                    if (handle_vm_exception(vm, "Division by zero")) {
                        DISPATCH();
                    }
                    runtime_error(vm, "Division by zero");
                    return false;
                }
                PUSH(val_int(a % b));
                DISPATCH();
            }

            L_BC_NEGATE:
            {
                Value v = POP();
                if (v.type == VAL_INT) PUSH(val_int(-v.as.integer));
                else if (v.type == VAL_FLOAT) PUSH(val_float(-v.as.floating));
                else { runtime_error(vm, "Operand must be a number"); return false; }
                DISPATCH();
            }
            L_BC_NOT:
            {
                Value v = POP();
                PUSH(val_bool(!value_is_truthy(v)));
                DISPATCH();
            }

            L_BC_ASSERT:
            {
                Value val = POP();
                if (!value_is_truthy(val)) {
                    runtime_error(vm, "Assertion failed");
                    return false;
                }
                DISPATCH();
            }

            L_BC_EQUAL:
            {
                Value b = POP();
                Value a = POP();
                PUSH(val_bool(value_equal(a, b)));
                DISPATCH();
            }
            L_BC_NOT_EQUAL:
            {
                Value b = POP();
                Value a = POP();
                PUSH(val_bool(!value_equal(a, b)));
                DISPATCH();
            }
            L_BC_LESS:
            BINARY_OP_NUM(<); DISPATCH();
            L_BC_GREATER:
            BINARY_OP_NUM(>); DISPATCH();
            L_BC_LESS_EQUAL:
            BINARY_OP_NUM(<=); DISPATCH();
            L_BC_GREATER_EQUAL:
            BINARY_OP_NUM(>=); DISPATCH();

            L_BC_AND:
            {
                Value b = POP();
                Value a = POP();
                PUSH(val_bool(value_is_truthy(a) && value_is_truthy(b)));
                DISPATCH();
            }
            L_BC_OR:
            {
                Value b = POP();
                Value a = POP();
                PUSH(val_bool(value_is_truthy(a) || value_is_truthy(b)));
                DISPATCH();
            }
            L_BC_NIL_COALESCE:
            {
                Value b = POP();
                Value a = POP();
                PUSH(a.type == VAL_NIL ? b : a);
                DISPATCH();
            }

            L_BC_DEFINE_GLOBAL:
            {
                uint16_t idx = READ_SHORT();
                ObjString *name = t->frames[t->frame_count - 1].function->constants[idx].as.string;
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
                DISPATCH();
            }
            L_BC_CLOSURE:
            {
                uint8_t upvalue_count = READ_BYTE();
                ObjClosure *closure = new_closure(NULL, upvalue_count);
                closure->obj.next = vm->objects;
                vm->objects = (Obj*)closure;
                for (int i = upvalue_count - 1; i >= 0; i--) {
                    closure->captured[i] = POP();
                }
                Value fn_val = POP();
                closure->function = fn_val.as.function;
                PUSH(val_closure(closure));
                DISPATCH();
            }
            L_BC_GET_UPVALUE:
            {
                uint8_t idx = READ_BYTE();
                PUSH(t->frames[t->frame_count - 1].closure->captured[idx]);
                DISPATCH();
            }
            L_BC_SET_UPVALUE:
            {
                uint8_t idx = READ_BYTE();
                t->frames[t->frame_count - 1].closure->captured[idx] = PEEK(0);
                DISPATCH();
            }
            L_BC_GET_GLOBAL:
            {
                uint16_t idx = READ_SHORT();
                ObjString *name = t->frames[t->frame_count - 1].function->constants[idx].as.string;
                Value value = get_global(vm, name);
                PUSH(value);
                DISPATCH();
            }
            L_BC_SET_GLOBAL:
            {
                uint16_t idx = READ_SHORT();
                ObjString *name = t->frames[t->frame_count - 1].function->constants[idx].as.string;
                Value value = PEEK(0);
                set_global(vm, name, value);
                DISPATCH();
            }

            L_BC_JUMP:
            {
                uint16_t offset = READ_SHORT();
                t->frames[t->frame_count - 1].ip += offset;
                DISPATCH();
            }
            L_BC_JUMP_IF_FALSE:
            {
                uint16_t offset = READ_SHORT();
                if (!value_is_truthy(PEEK(0)))
                    t->frames[t->frame_count - 1].ip += offset;
                DISPATCH();
            }
            L_BC_JUMP_IF_NIL:
            {
                uint16_t offset = READ_SHORT();
                if (PEEK(0).type == VAL_NIL)
                    t->frames[t->frame_count - 1].ip += offset;
                DISPATCH();
            }
            L_BC_LOOP:
            {
                uint16_t offset = READ_SHORT();
                t->frames[t->frame_count - 1].ip -= offset;
                /* Test-only deadline guard. Free in the common case (no
                 * deadline set); when one is, gettimeofday is amortized
                 * across many iterations so a tight loop still pays almost
                 * nothing while remaining interruptible within ~a few ms. */
                if (vm->deadline_us && (++vm->loop_tick & 0x3FFF) == 0) {
                    struct timeval now;
                    gettimeofday(&now, NULL);
                    int64_t now_us = (int64_t)now.tv_sec * 1000000 + now.tv_usec;
                    if (now_us >= vm->deadline_us) {
                        vm->timed_out = true;
                        vm->had_error = true;
                        goto end_vm;
                    }
                }
                DISPATCH();
            }

            L_BC_CALL:
            {
                uint8_t arg_count = READ_BYTE();
                Value callee = PEEK(arg_count);

                if (callee.type == VAL_NATIVE_FN) {
                    Value *args = &t->stack[t->stack_top - arg_count];
                    NativeFn fn = (NativeFn)callee.as.native_fn;
                    Value result = fn(vm, arg_count, args);
                    /* If the native function yielded (e.g. http.serve_tls()
                     * called directly rather than through BC_DISPATCH -- any
                     * native fn not in the parser's static method-name
                     * whitelist goes through plain BC_CALL instead), leave
                     * the stack untouched so the rewound retry can re-read
                     * the same callee+args via PEEK(arg_count) again. Popping
                     * here unconditionally (the old behavior) corrupted the
                     * stack on retry -- BC_DISPATCH already had the matching
                     * check below, BC_CALL never did because no native fn
                     * reachable only via plain BC_CALL had ever needed to
                     * yield-and-retry before. */
                    if (t->yielded) {
                        break;
                    }
                    t->stack_top -= (arg_count + 1);
                    PUSH(result);
                    if (t->is_throwing) {
                        t->is_throwing = false;
                        /* ─── @retry: restart the CALLER function ─── */
                        if (t->frame_count > 0) {
                            ObjFunction *caller_fn = t->frames[t->frame_count - 1].function;
                            if (caller_fn->metadata.type != VAL_NIL) {
                                Value rv = metadata_get(caller_fn->metadata, "retry");
                                int max_retries = (rv.type == VAL_INT) ? (int)rv.as.integer :
                                                  (rv.type == VAL_BOOL && rv.as.boolean) ? 1 : 0;
                                if (max_retries > 0) {
                                    max_retries--;
                                    if (caller_fn->metadata.type == VAL_ARRAY) {
                                        ObjArray *ma = caller_fn->metadata.as.array;
                                        for (int i = 0; i < ma->count; i += 2) {
                                            if (ma->elements[i].type == VAL_STRING &&
                                                strcmp(ma->elements[i].as.string->chars, "retry") == 0) {
                                                ma->elements[i + 1] = val_int(max_retries);
                                                break;
                                            }
                                        }
                                    }
                                    /* Restart the caller function from its beginning */
                                    t->stack_top = t->frames[t->frame_count - 1].slots - t->stack +
                                                   caller_fn->arity;
                                    t->frames[t->frame_count - 1].ip = caller_fn->code;
                                    break;
                                }
                            }
                        }
                        if (t->try_count > 0) {
                            t->try_count--;
                            int target_frame = t->try_stack[t->try_count].frame_index;
                            t->frame_count = target_frame + 1;
                            t->stack_top = t->try_stack[t->try_count].stack_depth;
                            PUSH(t->throw_value);
                            t->frames[target_frame].ip =
                                t->frames[target_frame].function->code +
                                t->try_stack[t->try_count].catch_offset;
                        } else {
                            runtime_error(vm, "Unhandled exception");
                            return false;
                        }
                    }
                } else if (callee.type == VAL_FUNCTION) {
                    ObjFunction *fn = callee.as.function;

                    /* ─── @cache decorator ─── */
                    if (fn->metadata.type != VAL_NIL && metadata_has(fn->metadata, "cache")) {
                        Value *args = &t->stack[t->stack_top - arg_count];
                        uint64_t kh = cache_key_hash(fn, arg_count, args);
                        int ci = cache_map_find(vm, kh);
                        if (ci >= 0) {
                            /* Cache hit — pop args + callee, push cached result */
                            t->stack_top -= (arg_count + 1);
                            PUSH(vm->cache_map[ci + 1]);
                            break;
                        }
                        /* Cache miss — save key, intercept BC_RETURN to store result */
                        t->cache_on_return = true;
                        t->cache_result_key = kh;
                    }

                    if (fn->arity != arg_count) {
                        runtime_error(vm, "Expected %d arguments but got %d", fn->arity, arg_count);
                        return false;
                    }
                    if (t->frame_count >= TASK_FRAMES_MAX) {
                        runtime_error(vm, "Stack overflow");
                        return false;
                    }
                    CallFrame *new_frame = &t->frames[t->frame_count++];
                    new_frame->function = fn;
                    new_frame->closure = NULL;
                    new_frame->ip = fn->code;
                    new_frame->slots = &t->stack[t->stack_top - arg_count];
                    /* Stack: [..., callee, arg0, ..., argN-1]. Returning
                     * must pop the callee too, one slot before the args. */
                    new_frame->return_base = t->stack_top - arg_count - 1;

                } else if (callee.type == VAL_CLOSURE) {
                    ObjClosure *closure = callee.as.closure;
                    ObjFunction *fn = closure->function;

                    if (fn->arity != arg_count) {
                        runtime_error(vm, "Expected %d arguments but got %d", fn->arity, arg_count);
                        return false;
                    }
                    if (t->frame_count >= TASK_FRAMES_MAX) {
                        runtime_error(vm, "Stack overflow");
                        return false;
                    }
                    CallFrame *new_frame = &t->frames[t->frame_count++];
                    new_frame->function = fn;
                    new_frame->closure = closure;
                    new_frame->ip = fn->code;
                    new_frame->slots = &t->stack[t->stack_top - arg_count];
                    new_frame->return_base = t->stack_top - arg_count - 1;

                } else {
                    runtime_error(vm, "Can only call functions");
                    return false;
                }
                DISPATCH();
            }

            L_BC_GET_LOCAL:
            {
                uint8_t local_idx = READ_BYTE();
                PUSH(t->frames[t->frame_count - 1].slots[local_idx]);
                DISPATCH();
            }

            L_BC_SET_LOCAL:
            {
                uint8_t local_idx = READ_BYTE();
                t->frames[t->frame_count - 1].slots[local_idx] = PEEK(0);
                DISPATCH();
            }

            L_BC_RETURN:
            {
                Value result = POP();
                /* ─── @cache: save result before returning ─── */
                if (t->cache_on_return) {
                    cache_map_put(vm, t->cache_result_key, result);
                    t->cache_on_return = false;
                }
                CallFrame *frame = &t->frames[t->frame_count - 1];
                /* Reset to the call site's recorded base, discarding args
                 * AND any locals/temporaries pushed during the function
                 * body (e.g. for-loop counters) that an early `return` from
                 * inside a nested block never got to pop. Using the base
                 * recorded at call time (rather than re-deriving it from
                 * `slots`) is required because different call sites push
                 * the callee at different positions relative to slots
                 * (BC_CALL: before args: BC_DISPATCH: after args). */
                int base = frame->return_base;
                t->frame_count--;
                if (t->frame_count == 0) {
                    /* Task completed — save return value, mark dead */
                    t->result = result;
                    t->dead = true;
                    PUSH(result);
                    goto end_vm;
                }
                if (base < 0) {
                    runtime_error(vm, "Stack underflow on return");
                    return false;
                }
                t->stack_top = base;
                PUSH(result);
                DISPATCH();
            }

            L_BC_RETURN_N:
            {
                uint8_t rcount = READ_BYTE();
                Value tmp_vals[16];
                int copy_count = rcount < 16 ? rcount : 16;
                for (int i = 0; i < copy_count; i++)
                    tmp_vals[i] = t->stack[t->stack_top - rcount + i];

                CallFrame *frame = &t->frames[t->frame_count - 1];
                /* See BC_RETURN for why this uses the recorded base. */
                int base = frame->return_base;
                t->frame_count--;
                if (t->frame_count == 0) {
                    /* Task completed — save first return value */
                    t->result = (copy_count > 0) ? tmp_vals[0] : val_nil();
                    t->dead = true;
                    for (int i = 0; i < copy_count; i++)
                        PUSH(tmp_vals[i]);
                    goto end_vm;
                }
                if (base < 0) {
                    runtime_error(vm, "Stack underflow on return");
                    return false;
                }
                t->stack_top = base;
                for (int i = 0; i < copy_count; i++)
                    PUSH(tmp_vals[i]);
                DISPATCH();
            }

            L_BC_ARRAY:
            {
                uint8_t count = READ_BYTE();
                ObjArray *arr = allocate_array(vm);
                arr->count = count;
                arr->capacity = count;
                arr->elements = (Value *)calloc(count, sizeof(Value));
                for (int i = count - 1; i >= 0; i--)
                    arr->elements[i] = POP();
                PUSH(val_array(arr));
                DISPATCH();
            }

            L_BC_TUPLE:
            {
                uint8_t count = READ_BYTE();
                ObjTuple *tup = allocate_tuple(vm, count);
                for (int i = count - 1; i >= 0; i--)
                    tup->elements[i] = POP();
                PUSH(val_tuple(tup));
                DISPATCH();
            }

            L_BC_INDEX:
            {
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
                DISPATCH();
            }

            L_BC_SET_INDEX:
            {
                Value val = POP();
                Value index = POP();
                Value obj = POP();
                if (obj.type == VAL_ARRAY && index.type == VAL_INT) {
                    int i = (int)index.as.integer;
                    if (i < 0) {
                        runtime_error(vm, "Array index out of bounds");
                        return false;
                    }
                    ObjArray *arr = obj.as.array;
                    if (i >= arr->count) {
                        if (i >= arr->capacity) {
                            int old_cap = arr->capacity;
                            arr->capacity = i + 1;
                            if (arr->capacity < old_cap * 2) arr->capacity = old_cap * 2;
                            if (arr->capacity < 8) arr->capacity = 8;
                            arr->elements = realloc(arr->elements, sizeof(Value) * arr->capacity);
                        }
                        for (int j = arr->count; j <= i; j++) {
                            arr->elements[j] = val_nil();
                        }
                        arr->count = i + 1;
                    }
                    arr->elements[i] = val;
                    /* Assignment yields the assigned value */
                    PUSH(val);
                } else {
                    runtime_error(vm, "Index assignment not supported for this type");
                    return false;
                }
                DISPATCH();
            }

            L_BC_STRUCT:
            {
                uint8_t field_count = READ_BYTE();
                ObjString *type_name_str = READ_CONSTANT().as.string;
                ObjStruct *s = new_struct(vm, field_count, false);
                s->type_name = (char *)malloc(type_name_str->length + 1);
                memcpy(s->type_name, type_name_str->chars, type_name_str->length);
                s->type_name[type_name_str->length] = '\0';
                for (int i = field_count - 1; i >= 0; i--)
                    s->fields[i] = POP();
                for (int i = 0; i < field_count; i++) {
                    ObjString *name = READ_CONSTANT().as.string;
                    s->field_names[i] = (char *)malloc(name->length + 1);
                    memcpy(s->field_names[i], name->chars, name->length);
                    s->field_names[i][name->length] = '\0';
                }

                /* Look up and attach validation rules, matching by field name —
                 * a struct literal's field order need not match the decl order. */
                bool has_validations = false;
                ValidationRegistry *reg = &vm->validation_registry;
                for (int r = 0; r < reg->count; r++) {
                    if (strcmp(reg->validations[r].type_name, s->type_name) == 0) {
                        StructValidationInfo *info = &reg->validations[r];
                        s->struct_validations = info->struct_validations;
                        s->struct_validation_count = info->struct_validation_count;
                        if (info->struct_validation_count > 0) has_validations = true;
                        for (int i = 0; i < field_count; i++) {
                            for (int j = 0; j < info->field_count; j++) {
                                if (strcmp(s->field_names[i], info->field_names[j]) == 0) {
                                    s->field_validations[i] = info->field_validations[j];
                                    s->field_validation_counts[i] = info->field_validation_counts[j];
                                    if (info->field_validation_counts[j] > 0) has_validations = true;
                                    break;
                                }
                            }
                        }
                        break;
                    }
                }

                /* Run validations */
                if (has_validations) {
                    if (!run_struct_validations(vm, s)) {
                        return false;
                    }
                }

                PUSH(val_struct(s));
                DISPATCH();
            }

            L_BC_REGISTER_METHOD:
            {
                ObjString *type_name = READ_CONSTANT().as.string;
                ObjString *method_name = READ_CONSTANT().as.string;
                Value func = PEEK(0);
                vm_register_dispatch(vm, type_name->chars, method_name->chars, func);
                DISPATCH();
            }

             L_BC_REGISTER_VALIDATIONS:
             {
                uint16_t type_name_idx = READ_SHORT();
                ObjString *type_name_str = t->frames[t->frame_count - 1].function->constants[type_name_idx].as.string;
                char *type_name = (char *)malloc(type_name_str->length + 1);
                memcpy(type_name, type_name_str->chars, type_name_str->length);
                type_name[type_name_str->length] = '\0';

                uint8_t struct_validation_count = READ_BYTE();
                ValidationRule *struct_validations = NULL;
                if (struct_validation_count > 0) {
                    struct_validations = (ValidationRule *)calloc(struct_validation_count, sizeof(ValidationRule));
                    for (int i = 0; i < struct_validation_count; i++) {
                        uint16_t key_idx = READ_SHORT();
                        ObjString *key = t->frames[t->frame_count - 1].function->constants[key_idx].as.string;
                        struct_validations[i].rule_name = (char *)malloc(key->length + 1);
                        memcpy(struct_validations[i].rule_name, key->chars, key->length);
                        struct_validations[i].rule_name[key->length] = '\0';
                        struct_validations[i].rule_args = (Value *)malloc(sizeof(Value));
                        uint16_t arg_idx = READ_SHORT();
                        struct_validations[i].rule_args[0] = t->frames[t->frame_count - 1].function->constants[arg_idx];
                        struct_validations[i].rule_arg_count = 1;
                    }
                }

                uint8_t field_count = READ_BYTE();
                ValidationRule **field_validations = (ValidationRule **)calloc(field_count, sizeof(ValidationRule *));
                int *field_validation_counts = (int *)calloc(field_count, sizeof(int));
                char **field_names = (char **)calloc(field_count, sizeof(char *));

                for (int i = 0; i < field_count; i++) {
                    uint16_t fname_idx = READ_SHORT();
                    ObjString *fname = t->frames[t->frame_count - 1].function->constants[fname_idx].as.string;
                    field_names[i] = (char *)malloc(fname->length + 1);
                    memcpy(field_names[i], fname->chars, fname->length);
                    field_names[i][fname->length] = '\0';

                    uint8_t fcount = READ_BYTE();
                    field_validation_counts[i] = fcount;
                    if (fcount > 0) {
                        field_validations[i] = (ValidationRule *)calloc(fcount, sizeof(ValidationRule));
                        for (int j = 0; j < fcount; j++) {
                            uint16_t fkey_idx = READ_SHORT();
                            ObjString *fkey = t->frames[t->frame_count - 1].function->constants[fkey_idx].as.string;
                            field_validations[i][j].rule_name = (char *)malloc(fkey->length + 1);
                            memcpy(field_validations[i][j].rule_name, fkey->chars, fkey->length);
                            field_validations[i][j].rule_name[fkey->length] = '\0';
                            field_validations[i][j].rule_args = (Value *)malloc(sizeof(Value));
                            uint16_t farg_idx = READ_SHORT();
                            field_validations[i][j].rule_args[0] = t->frames[t->frame_count - 1].function->constants[farg_idx];
                            field_validations[i][j].rule_arg_count = 1;
                        }
                    }
                }

                /* Register in validation registry */
                ValidationRegistry *reg = &vm->validation_registry;
                if (reg->count < MAX_STRUCT_VALIDATIONS) {
                    StructValidationInfo *info = &reg->validations[reg->count++];
                    strncpy(info->type_name, type_name, 63);
                    info->type_name[63] = '\0';
                    info->field_count = field_count;
                    info->struct_validations = struct_validations;
                    info->struct_validation_count = struct_validation_count;
                    info->field_validations = field_validations;
                    info->field_validation_counts = field_validation_counts;
                    info->field_names = field_names;
                } else {
                    /* Cleanup on overflow */
                    for (int i = 0; i < struct_validation_count; i++) {
                        free(struct_validations[i].rule_name);
                        free(struct_validations[i].rule_args);
                    }
                    free(struct_validations);
                    for (int i = 0; i < field_count; i++) {
                        free(field_names[i]);
                        if (field_validations[i]) {
                            for (int j = 0; j < field_validation_counts[i]; j++) {
                                free(field_validations[i][j].rule_name);
                                free(field_validations[i][j].rule_args);
                            }
                            free(field_validations[i]);
                        }
                    }
                    free(field_validations);
                    free(field_validation_counts);
                    free(field_names);
                }
                free(type_name);
                DISPATCH();
            }

            L_BC_DISPATCH:
            {
                ObjString *method_name = READ_CONSTANT().as.string;
                uint8_t arg_count = READ_BYTE();
                Value obj = PEEK(arg_count);

                /* ─── Actor async dispatch state machine ─── */
                if (obj.type == VAL_ACTOR) {
                    ObjActor *actor = obj.as.actor;

                    if (!t->waiting_actor_reply) {
                        /* First pass: setup reply channel and send message */
                        ObjChannel *reply_obj = (ObjChannel *)calloc(1, sizeof(ObjChannel));
                        reply_obj->obj.type = VAL_CHANNEL;
                        reply_obj->capacity = 1;
                        reply_obj->buffer = (Value *)calloc(1, sizeof(Value));
                        reply_obj->obj.next = vm->objects;
                        vm->objects = (Obj *)reply_obj;

                        Value reply_ch = val_channel(reply_obj);

                        /* Build message tuple: (method_name, args_array, reply_ch) */
                        ObjString *method_str = allocate_string(vm, method_name->chars, method_name->length);

                        ObjArray *args_arr = (ObjArray *)calloc(1, sizeof(ObjArray));
                        args_arr->obj.type = VAL_ARRAY;
                        args_arr->count = arg_count;
                        args_arr->elements = (Value *)malloc(arg_count * sizeof(Value));
                        for (int i = 0; i < arg_count; i++)
                            args_arr->elements[i] = t->stack[t->stack_top - arg_count + i];
                        args_arr->obj.next = vm->objects;
                        vm->objects = (Obj *)args_arr;

                        ObjTuple *msg_tuple = new_tuple(3);
                        msg_tuple->obj.next = vm->objects;
                        vm->objects = (Obj *)msg_tuple;
                        msg_tuple->elements[0] = val_string(method_str);
                        msg_tuple->elements[1] = val_array(args_arr);
                        msg_tuple->elements[2] = reply_ch;

                        /* Push message to actor's inbox */
                        if (!channel_try_send(actor->inbox.as.channel, val_tuple(msg_tuple))) {
                            /* Inbox full — discard msg allocations (GC will collect them)
                             * and retry from the top. BC_DISPATCH is opcode(1) +
                             * method_name constant idx(2) + arg_count(1) = 4 bytes. */
                            t->yielded = true;
                            t->frames[t->frame_count - 1].ip -= 4;
                            break;
                        }

                        /* Now safe to set the reply state */
                        t->actor_reply_ch = reply_ch;
                        t->waiting_actor_reply = true;
                    }

                    /* Second (and subsequent) pass: try to receive reply */
                    Value result;
                    if (channel_try_receive(t->actor_reply_ch.as.channel, &result)) {
                        /* Success! Clean up and push result */
                        t->waiting_actor_reply = false;
                        t->actor_reply_ch = val_nil();
                        t->stack_top -= (arg_count + 1);
                        PUSH(result);
                    } else {
                        /* Not ready yet — yield and retry BC_DISPATCH (4-byte instruction) */
                        t->yielded = true;
                        t->frames[t->frame_count - 1].ip -= 4;
                    }
                    break;
                }

                /* ─── Normal dispatch for structs, modules, strings, arrays ─── */
                const char *type_name = NULL;
                if (obj.type == VAL_STRUCT) {
                    type_name = obj.as.structure->type_name;
                } else if (obj.type == VAL_MODULE) {
                    type_name = obj.as.module->name;
                } else if (obj.type == VAL_STRING) {
                    type_name = "string";
                } else if (obj.type == VAL_ARRAY) {
                    type_name = "array";
                } else {
                    runtime_error(vm, "Cannot dispatch method on this type");
                    return false;
                }
                Value *func_val = vm_find_dispatch(vm, type_name, method_name->chars);

                /* Fallback: a VAL_STRUCT with no registered dispatch entry
                 * but a same-named field holding a function/closure is a
                 * plain Varian "namespace object" (e.g. built via
                 * http.create_struct with function-valued fields), not a
                 * registered impl type -- call the stored value directly
                 * with just the given args. No implicit `self`: it's a
                 * stored closure, not a method, so the receiver is dropped
                 * rather than prepended. */
                if (!func_val && obj.type == VAL_STRUCT) {
                    ObjStruct *s = obj.as.structure;
                    Value field_callee = val_nil();
                    bool found_field = false;
                    for (int i = 0; i < s->field_count; i++) {
                        if (strcmp(s->field_names[i], method_name->chars) == 0 &&
                            (s->fields[i].type == VAL_FUNCTION || s->fields[i].type == VAL_CLOSURE)) {
                            field_callee = s->fields[i];
                            found_field = true;
                            break;
                        }
                    }
                    if (found_field) {
                        int obj_idx = t->stack_top - arg_count - 1;
                        for (int i = 0; i < arg_count; i++) {
                            t->stack[obj_idx + i] = t->stack[obj_idx + 1 + i];
                        }
                        t->stack_top--; /* obj slot removed -- no self for a stored closure */
                        PUSH(field_callee);
                        if (field_callee.type == VAL_FUNCTION) {
                            ObjFunction *fn = field_callee.as.function;
                            if (fn->arity != arg_count) {
                                runtime_error(vm, "Function '%s' expects %d arguments but got %d",
                                              method_name->chars, fn->arity, arg_count);
                                return false;
                            }
                            if (t->frame_count >= TASK_FRAMES_MAX) {
                                runtime_error(vm, "Stack overflow");
                                return false;
                            }
                            CallFrame *new_frame = &t->frames[t->frame_count++];
                            new_frame->function = fn;
                            new_frame->closure = NULL;
                            new_frame->ip = fn->code;
                            new_frame->slots = &t->stack[t->stack_top - arg_count - 1];
                            new_frame->return_base = t->stack_top - arg_count - 1;
                        } else {
                            ObjClosure *closure = field_callee.as.closure;
                            ObjFunction *fn = closure->function;
                            if (fn->arity != arg_count) {
                                runtime_error(vm, "Function '%s' expects %d arguments but got %d",
                                              method_name->chars, fn->arity, arg_count);
                                return false;
                            }
                            if (t->frame_count >= TASK_FRAMES_MAX) {
                                runtime_error(vm, "Stack overflow");
                                return false;
                            }
                            CallFrame *new_frame = &t->frames[t->frame_count++];
                            new_frame->function = fn;
                            new_frame->closure = closure;
                            new_frame->ip = fn->code;
                            new_frame->slots = &t->stack[t->stack_top - arg_count - 1];
                            new_frame->return_base = t->stack_top - arg_count - 1;
                        }
                        break;
                    }
                }

                if (!func_val) {
                    runtime_error(vm, "No method '%s' for type '%s'", method_name->chars,
                                  type_name ? type_name : "(anonymous struct)");
                    return false;
                }
                Value callee = *func_val;
                PUSH(callee);
                if (callee.type == VAL_NATIVE_FN) {
                    /* Stack: [obj, args..., callee]. Pass self + all args. */
                    uint8_t total = arg_count + 1;
                    Value *all_args = &t->stack[t->stack_top - total - 1];
                    NativeFn fn = (NativeFn)callee.as.native_fn;
                    Value result = fn(vm, total, all_args);
                    /* If the native function yielded (e.g. http.serve), restore the stack
                       by popping the callee, so BC_DISPATCH can re-execute cleanly. */
                    if (t->yielded) {
                        t->stack_top--;
                        break;
                    }
                    t->stack_top -= (total + 1);
                    PUSH(result);
                } else if (callee.type == VAL_FUNCTION) {
                    ObjFunction *fn = callee.as.function;
                    uint8_t total_args = arg_count + 1;
                    if (fn->arity != total_args) {
                        runtime_error(vm, "Method '%s' expects %d arguments", method_name->chars, fn->arity);
                        return false;
                    }
                    if (t->frame_count >= TASK_FRAMES_MAX) {
                        runtime_error(vm, "Stack overflow");
                        return false;
                    }
                    CallFrame *new_frame = &t->frames[t->frame_count++];
                    new_frame->function = fn;
                    new_frame->closure = NULL;
                    new_frame->ip = fn->code;
                    new_frame->slots = &t->stack[t->stack_top - total_args - 1];
                    /* Stack: [obj, arg0, ..., argN-1, callee] — callee is
                     * pushed *after* obj+args here (unlike BC_CALL), so the
                     * reset point is exactly slots' index, not one before. */
                    new_frame->return_base = t->stack_top - total_args - 1;
                } else if (callee.type == VAL_CLOSURE) {
                    ObjClosure *closure = callee.as.closure;
                    ObjFunction *fn = closure->function;
                    uint8_t total_args = arg_count + 1;
                    if (fn->arity != total_args) {
                        runtime_error(vm, "Method '%s' expects %d arguments", method_name->chars, fn->arity);
                        return false;
                    }
                    if (t->frame_count >= TASK_FRAMES_MAX) {
                        runtime_error(vm, "Stack overflow");
                        return false;
                    }
                    CallFrame *new_frame = &t->frames[t->frame_count++];
                    new_frame->function = fn;
                    new_frame->closure = closure;
                    new_frame->ip = fn->code;
                    new_frame->slots = &t->stack[t->stack_top - total_args - 1];
                    new_frame->return_base = t->stack_top - total_args - 1;
                } else {
                    runtime_error(vm, "Method is not callable");
                    return false;
                }
                DISPATCH();
            }

            L_BC_ENUM:
            {
                uint8_t tag = READ_BYTE();
                uint8_t value_count = READ_BYTE();
                ObjEnum *e = new_enum(value_count);
                e->obj.next = vm->objects;
                vm->objects = (Obj *)e;
                e->tag = tag;
                for (int i = value_count - 1; i >= 0; i--)
                    e->values[i] = POP();
                PUSH(val_enum(e));
                DISPATCH();
            }

            L_BC_TAG_EQ:
            {
                uint8_t tag = READ_BYTE();
                Value v = POP();
                if (v.type == VAL_ENUM) {
                    PUSH(val_bool(v.as.enum_val->tag == (int)tag));
                } else {
                    PUSH(val_bool(false));
                }
                DISPATCH();
            }

            L_BC_UNPACK_ENUM:
            {
                Value v = POP();
                if (v.type == VAL_ENUM) {
                    ObjEnum *e = v.as.enum_val;
                    for (int i = 0; i < e->count; i++)
                        PUSH(e->values[i]);
                } else {
                    PUSH(val_nil());
                }
                DISPATCH();
            }

            L_BC_PROPAGATE:
            {
                Value v = PEEK(0);
                if (v.type == VAL_NIL) {
                    (void)POP();
                    CallFrame *frame = &t->frames[t->frame_count - 1];
                    int arity = frame->function->arity;
                    t->frame_count--;
                    if (t->frame_count == 0) {
                        PUSH(val_nil());
                        goto end_vm;
                    }
                    t->stack_top -= (arity + 1);
                    PUSH(val_nil());
                }
                DISPATCH();
            }

            L_BC_THROW:
            {
                Value err = POP();

                /* ─── @retry decorator: retry on throw ─── */
                if (t->frame_count > 0) {
                    ObjFunction *cur_fn = t->frames[t->frame_count - 1].function;
                    if (cur_fn->metadata.type != VAL_NIL) {
                        Value rv = metadata_get(cur_fn->metadata, "retry");
                        if (rv.type == VAL_INT && rv.as.integer > 0) {
                            int max_retries = (int)rv.as.integer - 1;
                            if (cur_fn->metadata.type == VAL_ARRAY) {
                                ObjArray *ma = cur_fn->metadata.as.array;
                                for (int i = 0; i < ma->count; i += 2) {
                                    if (ma->elements[i].type == VAL_STRING &&
                                        strcmp(ma->elements[i].as.string->chars, "retry") == 0) {
                                        ma->elements[i + 1] = val_int(max_retries);
                                        break;
                                    }
                                }
                            }
                            t->frames[t->frame_count - 1].ip = cur_fn->code;
                            t->is_throwing = false;
                            break;
                        }
                    }
                }

                if (t->try_count > 0) {
                    t->try_count--;
                    int target_frame = t->try_stack[t->try_count].frame_index;
                    t->frame_count = target_frame + 1;
                    t->stack_top = t->try_stack[t->try_count].stack_depth;
                    PUSH(err);
                    t->frames[target_frame].ip =
                        t->frames[target_frame].function->code +
                        t->try_stack[t->try_count].catch_offset;
                } else {
                    runtime_error(vm, "Unhandled exception");
                    return false;
                }
                DISPATCH();
            }

            L_BC_TRY:
            {
                uint16_t offset = READ_SHORT();
                uint8_t local_count = READ_BYTE();
                if (t->try_count >= TASK_TRY_MAX) {
                    runtime_error(vm, "Too many nested try blocks");
                    return false;
                }
                CallFrame *active = &t->frames[t->frame_count - 1];
                t->try_stack[t->try_count].catch_offset =
                    (int)(active->ip - active->function->code) + offset;
                t->try_stack[t->try_count].stack_depth =
                    (int)(active->slots - t->stack) + local_count;
                t->try_stack[t->try_count].frame_index = t->frame_count - 1;
                t->try_count++;
                DISPATCH();
            }

            L_BC_POP_TRY:
            {
                if (t->try_count > 0)
                    t->try_count--;
                DISPATCH();
            }

            L_BC_MEMBER:
            {
                ObjString *name = READ_CONSTANT().as.string;
                Value obj = POP();
                if (obj.type == VAL_STRUCT) {
                    ObjStruct *s = obj.as.structure;
                    int found = -1;
                    /* Phase 3: check field cache first */
                    uint32_t name_hash = hash_string(name->chars, name->length);
                    for (int ci = 0; ci < s->field_cache_count; ci++) {
                        if (s->field_cache[ci].hash == name_hash) {
                            found = s->field_cache[ci].index;
                            break;
                        }
                    }
                    if (found < 0) {
                        for (int i = 0; i < s->field_count; i++) {
                            if (strcmp(s->field_names[i], name->chars) == 0) {
                                found = i;
                                /* Cache it */
                                if (s->field_cache_count < STRUCT_CACHE_SIZE) {
                                    s->field_cache[s->field_cache_count].hash = name_hash;
                                    s->field_cache[s->field_cache_count].index = i;
                                    s->field_cache_count++;
                                }
                                break;
                            }
                        }
                    }
                    if (found >= 0) {
                        PUSH(s->fields[found]);
                    } else {
                        runtime_error(vm, "Struct has no field '%s'", name->chars);
                        return false;
                    }
                } else if (obj.type == VAL_MODULE) {
                    /* Module member access: look up in dispatch table */
                    Value *func_val = vm_find_dispatch(vm, obj.as.module->name, name->chars);
                    if (func_val) {
                        PUSH(*func_val);
                    } else {
                        runtime_error(vm, "Module '%s' has no member '%s'",
                                     obj.as.module->name, name->chars);
                        return false;
                    }
                } else if (obj.type == VAL_STRING) {
                    /* String method access: look up in dispatch table */
                    Value *func_val = vm_find_dispatch(vm, "string", name->chars);
                    if (func_val) {
                        PUSH(*func_val);
                    } else {
                        runtime_error(vm, "String has no method '%s'", name->chars);
                        return false;
                    }
                } else if (obj.type == VAL_ARRAY) {
                    /* Array method access: look up in dispatch table */
                    Value *func_val = vm_find_dispatch(vm, "array", name->chars);
                    if (func_val) {
                        PUSH(*func_val);
                    } else {
                        runtime_error(vm, "Array has no method '%s'", name->chars);
                        return false;
                    }
                } else {
                    runtime_error(vm, "Cannot access field on non-struct value");
                    return false;
                }
                DISPATCH();
            }

            L_BC_MEMBER_SAFE:
            {
                /* expr?.member -- BC_JUMP_IF_NIL already short-circuited the
                 * case where expr itself is nil; this handles the other half
                 * of "safe navigation": expr is a real, valid value, it
                 * just doesn't have this particular field/method. Every
                 * branch below mirrors L_BC_MEMBER exactly except pushing
                 * nil instead of calling runtime_error() -- that's the
                 * entire difference. Keep the two in sync if either changes. */
                ObjString *name = READ_CONSTANT().as.string;
                Value obj = POP();
                if (obj.type == VAL_STRUCT) {
                    ObjStruct *s = obj.as.structure;
                    int found = -1;
                    uint32_t name_hash = hash_string(name->chars, name->length);
                    for (int ci = 0; ci < s->field_cache_count; ci++) {
                        if (s->field_cache[ci].hash == name_hash) {
                            found = s->field_cache[ci].index;
                            break;
                        }
                    }
                    if (found < 0) {
                        for (int i = 0; i < s->field_count; i++) {
                            if (strcmp(s->field_names[i], name->chars) == 0) {
                                found = i;
                                if (s->field_cache_count < STRUCT_CACHE_SIZE) {
                                    s->field_cache[s->field_cache_count].hash = name_hash;
                                    s->field_cache[s->field_cache_count].index = i;
                                    s->field_cache_count++;
                                }
                                break;
                            }
                        }
                    }
                    if (found >= 0) {
                        PUSH(s->fields[found]);
                    } else {
                        PUSH(val_nil());
                    }
                } else if (obj.type == VAL_MODULE) {
                    Value *func_val = vm_find_dispatch(vm, obj.as.module->name, name->chars);
                    PUSH(func_val ? *func_val : val_nil());
                } else if (obj.type == VAL_STRING) {
                    Value *func_val = vm_find_dispatch(vm, "string", name->chars);
                    PUSH(func_val ? *func_val : val_nil());
                } else if (obj.type == VAL_ARRAY) {
                    Value *func_val = vm_find_dispatch(vm, "array", name->chars);
                    PUSH(func_val ? *func_val : val_nil());
                } else {
                    /* A genuinely unsupported type for member access at all
                     * (e.g. ?. on an int) is still a real bug worth
                     * surfacing, not a "maybe absent" case -- not silenced. */
                    runtime_error(vm, "Cannot access field on non-struct value");
                    return false;
                }
                DISPATCH();
            }

            L_BC_SET_MEMBER:
            {
                ObjString *name = READ_CONSTANT().as.string;
                Value val = POP();
                Value obj = POP();
                if (obj.type == VAL_STRUCT) {
                    ObjStruct *s = obj.as.structure;
                    int found = -1;
                    /* Phase 3: check field cache first */
                    uint32_t name_hash = hash_string(name->chars, name->length);
                    for (int ci = 0; ci < s->field_cache_count; ci++) {
                        if (s->field_cache[ci].hash == name_hash) {
                            found = s->field_cache[ci].index;
                            break;
                        }
                    }
                    if (found < 0) {
                        for (int i = 0; i < s->field_count; i++) {
                            if (strcmp(s->field_names[i], name->chars) == 0) {
                                found = i;
                                if (s->field_cache_count < STRUCT_CACHE_SIZE) {
                                    s->field_cache[s->field_cache_count].hash = name_hash;
                                    s->field_cache[s->field_cache_count].index = i;
                                    s->field_cache_count++;
                                }
                                break;
                            }
                        }
                    }
                    if (found >= 0) {
                        s->fields[found] = val;
                        PUSH(val);
                    } else {
                        runtime_error(vm, "Struct has no field '%s'", name->chars);
                        return false;
                    }
                } else {
                    runtime_error(vm, "Cannot set field on non-struct value");
                    return false;
                }
                DISPATCH();
            }

            L_BC_FFI_CALL:
            {
                uint8_t ffi_idx = READ_BYTE();
                uint8_t arg_count = READ_BYTE();

                if (ffi_idx >= vm->ffi_entry_count) {
                    runtime_error(vm, "Invalid FFI call index");
                    return false;
                }
                VMFFIEntry *entry = &vm->ffi_entries[ffi_idx];

                if (arg_count != entry->param_count) {
                    runtime_error(vm, "FFI: expected %d args, got %d",
                                  entry->param_count, arg_count);
                    return false;
                }

                /* Marshal arguments from Varian stack to libffi */
                void *value_storage[MAX_FFI_PARAMS];
                void *args[MAX_FFI_PARAMS];
                int storage_used = 0;

                for (int i = 0; i < arg_count; i++) {
                    Value v = t->stack[t->stack_top - arg_count + i];
                    switch (entry->param_kinds[i]) {
                        case FFI_INT: {
                            int32_t *p = (int32_t *)malloc(sizeof(int32_t));
                            *p = (int32_t)v.as.integer;
                            value_storage[storage_used++] = p;
                            args[i] = p;
                            break;
                        }
                        case FFI_DOUBLE: {
                            double *p = (double *)malloc(sizeof(double));
                            *p = v.as.floating;
                            value_storage[storage_used++] = p;
                            args[i] = p;
                            break;
                        }
                        case FFI_FLOAT: {
                            float *p = (float *)malloc(sizeof(float));
                            *p = (float)v.as.floating;
                            value_storage[storage_used++] = p;
                            args[i] = p;
                            break;
                        }
                        case FFI_PTR: {
                            /* ffi_call expects args[i] to point TO the argument
                             * value, so we need a slot containing the void*. */
                            void **slot = (void **)malloc(sizeof(void *));
                            if (v.type == VAL_INT) {
                                *slot = (void *)(uintptr_t)v.as.integer;
                            } else if (v.type == VAL_STRING) {
                                /* CRITICAL: Copy the string to prevent C from
                                 * mutating Varian's interned string data. */
                                size_t slen = (size_t)v.as.string->length;
                                char *copy = (char *)malloc(slen + 1);
                                memcpy(copy, v.as.string->chars, slen);
                                copy[slen] = '\0';
                                value_storage[storage_used++] = copy;
                                *slot = copy;
                            } else {
                                *slot = NULL;
                            }
                            value_storage[storage_used++] = slot;
                            args[i] = slot;
                            break;
                        }
                        case FFI_CHAR: {
                            char *p = (char *)malloc(sizeof(char));
                            *p = (char)v.as.integer;
                            value_storage[storage_used++] = p;
                            args[i] = p;
                            break;
                        }
                        default:
                            args[i] = NULL;
                            break;
                    }
                }

                /* Prepare return value buffer */
                void *ret_val = NULL;
                ffi_type *ret_ffi_type = ffi_type_from_kind(entry->return_kind);
                bool has_return = (entry->return_kind != FFI_VOID);

                if (has_return) {
                    ret_val = calloc(1, ret_ffi_type->size);
                }

                /* Call via libffi */
                ffi_call(&entry->cif, FFI_FN(entry->fn_ptr), ret_val, args);

                /* Pop arguments from stack */
                t->stack_top -= arg_count;

                /* Push return value */
                switch (entry->return_kind) {
                    case FFI_VOID:
                        PUSH(val_nil());
                        break;
                    case FFI_INT:
                        PUSH(val_int(*(int32_t *)ret_val));
                        break;
                    case FFI_DOUBLE:
                        PUSH(val_float(*(double *)ret_val));
                        break;
                    case FFI_FLOAT:
                        PUSH(val_float(*(float *)ret_val));
                        break;
                    case FFI_PTR: {
                        void *ptr_val = *(void **)ret_val;
                        PUSH(val_int((int64_t)(uintptr_t)ptr_val));
                        break;
                    }
                    case FFI_CHAR:
                        PUSH(val_int(*(char *)ret_val));
                        break;
                }

                /* Free temporary storage */
                for (int i = 0; i < storage_used; i++)
                    free(value_storage[i]);
                if (ret_val) free(ret_val);

                DISPATCH();
            }

            L_BC_PRINT:
            {
                Value v = POP();
                value_print(v);
                printf("\n");
                DISPATCH();
            }

            L_BC_BUILD_STRING:
            {
                uint8_t count = READ_BYTE();
                int total_len = 0;
                for (int i = 0; i < count; i++) {
                    Value v = t->stack[t->stack_top - count + i];
                    if (v.type == VAL_STRING)
                        total_len += v.as.string->length;
                }
                char *chars = (char *)malloc(total_len + 1);
                int pos = 0;
                for (int i = 0; i < count; i++) {
                    Value v = t->stack[t->stack_top - count + i];
                    if (v.type == VAL_STRING) {
                        memcpy(chars + pos, v.as.string->chars, v.as.string->length);
                        pos += v.as.string->length;
                    }
                }
                t->stack_top -= count;
                chars[total_len] = '\0';
                ObjString *result = allocate_string(vm, chars, total_len);
                free(chars);
                PUSH(val_string(result));
                DISPATCH();
            }

            L_BC_STRING_CONCAT:
            {
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
                DISPATCH();
            }

            L_BC_INT_TO_STRING:
            {
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
                DISPATCH();
            }

            L_BC_HALT:
                /* Mark task as dead so the scheduler knows to stop */
                t->dead = true;
                goto end_vm;

            L_BC_DEFAULT:
                runtime_error(vm, "Unknown opcode %d", instruction);
                return false;

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
            case VAL_MODULE: free(((ObjModule *)obj)->name); break;
            case VAL_TASK:   break;
            case VAL_CHANNEL: free(((ObjChannel *)obj)->buffer); break;
            case VAL_ENUM:   free(((ObjEnum *)obj)->values); break;
            case VAL_ACTOR: {
                ObjActor *a = (ObjActor *)obj;
                free(a->type_name);
                break;
            }
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
            case VAL_CLOSURE: {
                ObjClosure *c = (ObjClosure *)obj;
                free(c->captured);
                break;
            }
            default: break;
        }
        free(obj);
        obj = next;
    }
    vm->objects = NULL;

    /* Free FFI entries */
    free(vm->ffi_entries);
    vm->ffi_entries = NULL;
    vm->ffi_entry_count = 0;

    /* Close all loaded FFI library handles */
    ffi_close_all_libs();

    /* Free the task free-list */
    Task *ft = vm->free_tasks;
    while (ft) {
        Task *next = (Task *)ft->http_response_ssl;
        free(ft->arena_base);
        free(ft);
        ft = next;
    }
    vm->free_tasks = NULL;

    /* Free arena memory for any live tasks still in the tasks array */
    for (int i = 0; i < vm->task_count; i++) {
        free(vm->tasks[i]->arena_base);
        vm->tasks[i]->arena_base = NULL;
    }

    /* Free the tasks array itself */
    free(vm->tasks);
    vm->tasks = NULL;
    vm->task_count = 0;
    vm->task_capacity = 0;
}
