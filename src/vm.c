#include "vm.h"
#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <limits.h>
#include <stdarg.h>
#include <sys/time.h>
#include <time.h>
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

/* realloc that never silently loses the old pointer: on allocation failure
 * it aborts with a message instead of returning NULL (which the callers below
 * cannot recover from without leaking/corrupting). Used only at growth sites
 * in void-returning functions where there is no Value to RAISE through. */
static void *vm_xrealloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size);
    if (!p && size != 0) {
        fprintf(stderr, "fatal: out of memory (realloc of %zu bytes failed)\n", size);
        abort();
    }
    return p;
}

static void chunk_grow(Chunk *chunk) {
    int old = chunk->capacity;
    chunk->capacity = old < 8 ? 8 : old * 2;
    chunk->code = (uint8_t *)vm_xrealloc(chunk->code, chunk->capacity);
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
            chunk->rle_lines = (int *)vm_xrealloc(chunk->rle_lines, sizeof(int) * chunk->rle_capacity);
            chunk->rle_counts = (int *)vm_xrealloc(chunk->rle_counts, sizeof(int) * chunk->rle_capacity);
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
        chunk->constants = (Value *)vm_xrealloc(chunk->constants,
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

Value val_bound_method(ObjBoundMethod *bm) {
    Value v;
    v.type = VAL_BOUND_METHOD;
    v.as.bound_method = bm;
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

/* ─── Shape API ──────────────────────────────────────────────────────────────────────────
 *
 * shape_get_or_create: find or build a shared Shape for (type_name, field_names).
 * shape_index_of: hash-filtered O(n) scan; Stage 2 (PICs) will make this O(1).
 */
Shape *shape_get_or_create(VM *vm, const char *type_name,
                           const char * const *field_names, int field_count) {
    ShapeRegistry *reg = &vm->shape_registry;

    /* Search registry for a matching existing shape. Anonymous (NULL
     * type_name) shapes are matched by field-name set too, so dynamic
     * structs (HTTP headers, sqlite rows, struct.set) that happen to
     * repeat the same field set still get to share a Shape instead of
     * growing the registry once per call. */
    for (int i = 0; i < reg->count; i++) {
        Shape *s = reg->shapes[i];
        if ((s->type_name != NULL) != (type_name != NULL)) continue;
        if (type_name && strcmp(s->type_name, type_name) != 0) continue;
        if (s->field_count != field_count) continue;
        bool match = true;
        for (int j = 0; j < field_count; j++) {
            const char *fn = (field_names && field_names[j]) ? field_names[j] : "";
            if (strcmp(s->field_names[j], fn) != 0) { match = false; break; }
        }
        if (match) return s;
    }

    /* Build a new Shape */
    Shape *shape = (Shape *)calloc(1, sizeof(Shape));
    shape->field_count = field_count;
    shape->type_name   = type_name ? strdup(type_name) : NULL;

    if (field_count > 0) {
        shape->field_names  = (char **)malloc((size_t)field_count * sizeof(char *));
        shape->name_hashes  = (uint32_t *)malloc((size_t)field_count * sizeof(uint32_t));
        for (int i = 0; i < field_count; i++) {
            const char *fn = (field_names && field_names[i]) ? field_names[i] : "";
            shape->field_names[i]  = strdup(fn);
            shape->name_hashes[i]  = hash_string(fn, (int)strlen(fn));
        }
    }

    /* Initialise method cache and field cache to empty */
    memset(shape->method_cache_keys, 0, sizeof(shape->method_cache_keys));
    memset(shape->method_cache_vals, 0, sizeof(shape->method_cache_vals));
    for (int i = 0; i < SHAPE_FIELD_CACHE_SIZE; i++)
        shape->field_cache_vals[i] = -1;

    if (reg->count < SHAPE_REGISTRY_SIZE) {
        reg->shapes[reg->count++] = shape;
    } else {
        fprintf(stderr, "warn: shape registry full for '%s'\n",
                type_name ? type_name : "(anon)");
    }
    return shape;
}

/* Stage 2: method dispatch PIC. Check the shape's direct-mapped cache first;
 * on miss, call vm_find_dispatch and cache the result.  Returns NULL when the
 * method is not found (caller falls back to field-scan or universal "struct"
 * namespace).  name_hash is precomputed at the call site. */
Value *shape_resolve_method(VM *vm, Shape *s, const char *method_name,
                            uint32_t name_hash) {
    int slot = name_hash & (SHAPE_METHOD_CACHE_SIZE - 1);
    if (s->method_cache_keys[slot] == name_hash) {
        Value *cached = &s->method_cache_vals[slot];
        if (cached->type != VAL_NIL) return cached;
        return NULL;  /* cached miss */
    }
    /* Slow path */
    Value *resolved = vm_find_dispatch(vm, s->type_name, method_name);
    s->method_cache_keys[slot] = name_hash;
    if (resolved) {
        s->method_cache_vals[slot] = *resolved;
    } else {
        s->method_cache_vals[slot] = val_nil();  /* cache the miss */
    }
    return resolved;
}

int shape_index_of(const Shape *s, const char *name, uint32_t hash) {
    /* Stage 2: direct-mapped field cache */
    const int slot = hash & (SHAPE_FIELD_CACHE_SIZE - 1);
    if (s->field_cache_keys[slot] == hash) {
        return s->field_cache_vals[slot];  /* cached index or -1 */
    }
    for (int i = 0; i < s->field_count; i++) {
        if (s->name_hashes[i] == hash && strcmp(s->field_names[i], name) == 0) {
            /* Shape is const from the caller's perspective in this
             * header, but the cache is logically mutable — we cast
             * away const since the shape is never shared across
             * threads (single-threaded scheduler). */
            Shape *ms = (Shape *)s;
            ms->field_cache_keys[slot] = hash;
            ms->field_cache_vals[slot] = (int16_t)i;
            return i;
        }
    }
    Shape *ms = (Shape *)s;
    ms->field_cache_keys[slot] = hash;
    ms->field_cache_vals[slot] = -1;
    return -1;
}

/* struct_attach_shape: build/find the shared Shape for (type_name, names[]),
 * attach it to s, and set the backward-compat alias pointers.  Native code
 * that still calls s->field_names[i] will see the shape's copies. */
void struct_attach_shape(VM *vm, ObjStruct *s, const char *type_name,
                         char * const *scratch_names, int field_count) {
    s->shape       = shape_get_or_create(vm, type_name,
                                          (const char * const *)scratch_names,
                                          field_count);
    s->field_names = s->shape ? s->shape->field_names : NULL;
    s->type_name   = s->shape ? s->shape->type_name   : NULL;
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

/* Return a permanent, GC-rooted ObjString for one of the common HTTP method
 * names, allocating it once per VM on first use. Falls back to a normal
 * allocate_string() for anything not in the small fixed set (rare verbs).
 * The returned strings are marked every GC via gc_mark_roots(), so they are
 * never swept and can be shared across every request on this VM. */
ObjString *intern_http_method(VM *vm, const char *method) {
    static const char *NAMES[8] = {
        "GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS", "TRACE"
    };
    for (int i = 0; i < 8; i++) {
        if (strcmp(method, NAMES[i]) == 0) {
            if (!vm->method_interns[i]) {
                vm->method_interns[i] =
                    allocate_string(vm, NAMES[i], (int)strlen(NAMES[i]));
            }
            return vm->method_interns[i];
        }
    }
    return allocate_string(vm, method, (int)strlen(method));
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

    /* Stage 1: Arena structs get no field_names scratch array — they are
     * always created via BC_STRUCT which sets field_names to shape->field_names.
     * Heap structs (native code, escape_promote) still get a scratch
     * field_names array so s->field_names[i] = strdup(...) works unchanged. */
    size_t hsize  = (sizeof(ObjStruct) + 7) & ~(size_t)7;
    size_t fc     = (size_t)(field_count > 0 ? field_count : 0);
    size_t vsize  = (fc * sizeof(Value) + 7) & ~(size_t)7;
    size_t avsize = (fc * sizeof(ValidationRule *) + 7) & ~(size_t)7;
    size_t acsize = (fc * sizeof(int) + 7) & ~(size_t)7;
    size_t total  = hsize + vsize + avsize + acsize;

    bool use_arena = !force_heap && t && t->use_arena && t->arena_base &&
                      (t->arena_offset + total <= TASK_ARENA_SIZE);

    ObjStruct *s;
    if (use_arena) {
        char *base = t->arena_base + t->arena_offset;
        t->arena_offset += total;
        memset(base, 0, total);
        s = (ObjStruct *)base;
        s->fields              = (Value *)(base + hsize);
        s->field_validations   = (ValidationRule **)(base + hsize + vsize);
        s->field_validation_counts = (int *)(base + hsize + vsize + avsize);
        /* field_names stays NULL — set from shape by BC_STRUCT */
        /* NOT linked into vm->objects: bulk-reclaimed on task recycle */
    } else {
        s = (ObjStruct *)calloc(1, sizeof(ObjStruct));
        /* Allocate a scratch field_names array for native code */
        s->field_names         = (fc > 0) ? (char **)calloc(fc, sizeof(char *)) : NULL;
        s->fields              = (Value *)calloc(fc, sizeof(Value));
        s->field_validations   = (ValidationRule **)calloc(fc, sizeof(ValidationRule *));
        s->field_validation_counts = (int *)calloc(fc, sizeof(int));
        if (!s || !s->fields || !s->field_validations || !s->field_validation_counts) {
            fprintf(stderr, "fatal: out of memory in new_struct (%zu fields)\n", fc);
            exit(1);
        }
        s->obj.next = vm->objects;
        vm->objects = (Obj *)s;
    }
    s->obj.type  = VAL_STRUCT;
    s->field_count = field_count;
    s->shape = NULL;
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
 * VAL_STRUCT/VAL_ARRAY/VAL_CLOSURE/VAL_ENUM returns immediately unchanged.
 * Arrays, closures and enums are never themselves arena-backed
 * (only ObjStruct is) -- they're walked in place (not copied) only
 * to find and promote any arena-backed struct nested inside them. */
static Value escape_promote(VM *vm, Value v) {
    Task *t = vm->current_task;
    if (!t || !t->use_arena) return v;
    switch (v.type) {
        case VAL_STRUCT: {
            ObjStruct *s = v.as.structure;
            if (!struct_is_arena_backed(t, s)) return v;
            ObjStruct *copy = new_struct(vm, s->field_count, true /* force_heap */);
            /* Stage 1: Shape is shared — promoted copy just aliases the shape.
             * new_struct allocated a scratch field_names array; free it before
             * overwriting with the shape alias to avoid a leak. */
            if (copy->field_names) { free(copy->field_names); }
            copy->shape      = s->shape;
            copy->field_names = s->shape ? s->shape->field_names : NULL;
            copy->type_name   = s->shape ? s->shape->type_name   : NULL;
            copy->struct_validations = s->struct_validations;
            copy->struct_validation_count = s->struct_validation_count;
            for (int i = 0; i < s->field_count; i++) {
                copy->field_validations[i] = s->field_validations[i];
                copy->field_validation_counts[i] = s->field_validation_counts[i];
                copy->fields[i] = escape_promote(vm, s->fields[i]);
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
        vm->gray_stack = (Obj **)vm_xrealloc(vm->gray_stack, sizeof(Obj *) * vm->gray_capacity);
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
        case VAL_BOUND_METHOD: obj = (Obj *)value.as.bound_method; break;
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
            case VAL_BOUND_METHOD: {
                ObjBoundMethod *bm = (ObjBoundMethod *)obj;
                gc_mark_value(vm, bm->self);
                gc_mark_value(vm, bm->method);
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
                    /* Stage 1: field_names is either a shape alias (don't
                     * free) or embedded in the struct's old-style arena
                     * block (also don't free — it's part of the contiguous
                     * new_struct allocation, not a separate malloc).
                     * Only free field_names when we have a shape AND the
                     * pointer differs (transitional native scratch array).
                     * type_name is always a shape alias or NULL. */
                    if (s->shape && s->field_names != s->shape->field_names) {
                        for (int i = 0; i < s->field_count; i++)
                            free(s->field_names[i]);
                        free(s->field_names);
                    }
                    free(s->fields);
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
                    if (c->captured_slots) free(c->captured_slots);
                    obj_size = sizeof(ObjClosure);
                    break;
                }
                case VAL_BOUND_METHOD: {
                    obj_size = sizeof(ObjBoundMethod);
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

    /* Keep the interned HTTP method strings permanently alive. */
    for (int i = 0; i < 8; i++) {
        if (vm->method_interns[i])
            gc_mark_value(vm, val_string(vm->method_interns[i]));
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
/* value_fprint writes a value's display form to any FILE*; value_print is the
 * stdout wrapper. Splitting them lets the print native also capture into an
 * in-memory stream (open_memstream) for the Lumen devtools "server logs in
 * browser" feature without duplicating this formatting logic. */
static void value_fprint(FILE *f, Value value) {
    switch (value.type) {
        case VAL_NIL:      fprintf(f, "nil"); break;
        case VAL_BOOL:     fprintf(f, "%s", value.as.boolean ? "true" : "false"); break;
        case VAL_INT:      fprintf(f, "%ld", (long)value.as.integer); break;
        case VAL_FLOAT:    fprintf(f, "%g", value.as.floating); break;
        case VAL_STRING:   fprintf(f, "%s", value.as.string->chars); break;
        case VAL_ARRAY: {
            fprintf(f, "[");
            for (int i = 0; i < value.as.array->count; i++) {
                if (i > 0) fprintf(f, ", ");
                value_fprint(f, value.as.array->elements[i]);
            }
            fprintf(f, "]");
            break;
        }
        case VAL_TUPLE: {
            fprintf(f, "(");
            for (int i = 0; i < value.as.tuple->count; i++) {
                if (i > 0) fprintf(f, ", ");
                value_fprint(f, value.as.tuple->elements[i]);
            }
            fprintf(f, ")");
            break;
        }
        case VAL_FUNCTION: fprintf(f, "<fn %p>", (void *)value.as.function); break;
        case VAL_CLOSURE: fprintf(f, "<closure %p>", (void *)value.as.closure); break;
        case VAL_NATIVE_FN: fprintf(f, "<native fn>"); break;
        case VAL_STRUCT: {
            ObjStruct *s = value.as.structure;
            fprintf(f, "{");
            for (int i = 0; i < s->field_count; i++) {
                if (i > 0) fprintf(f, ", ");
                fprintf(f, "%s: ", s->field_names[i]);
                value_fprint(f, s->fields[i]);
            }
            fprintf(f, "}");
            break;
        }
        case VAL_ENUM: {
            ObjEnum *e = value.as.enum_val;
            fprintf(f, "#%d", e->tag);
            if (e->count > 0) {
                fprintf(f, "(");
                for (int i = 0; i < e->count; i++) {
                    if (i > 0) fprintf(f, ", ");
                    value_fprint(f, e->values[i]);
                }
                fprintf(f, ")");
            }
            break;
        }
        case VAL_MODULE:
            fprintf(f, "<module %s>", value.as.module->name);
            break;
        case VAL_TASK:
            fprintf(f, "<task %d>", value.as.task_obj->task->id);
            break;
        case VAL_CHANNEL:
            fprintf(f, "<channel %d/%d>", value.as.channel->count, value.as.channel->capacity);
            break;
        case VAL_ACTOR:
            fprintf(f, "<actor %s>", value.as.actor->type_name);
            break;
        case VAL_BOUND_METHOD:
            fprintf(f, "<bound method>");
            break;
    }
}

void value_print(Value value) { value_fprint(stdout, value); }

/* ─── print capture (Lumen dev: forward server `print()` to the browser) ───
 * A process-global sink, enabled only by `__lumen_log_start()` which the Lumen
 * live handler calls in dev mode around each handler/render. In production it is
 * never enabled, so `print` keeps its original zero-overhead path. `vn dev` is a
 * single-process, cooperatively-scheduled event loop, so a global is safe here
 * (handlers don't yield mid-execution). */
static char  *g_log_cap = NULL;
static size_t g_log_cap_len = 0, g_log_cap_cap = 0;
static bool   g_log_capturing = false;

static void log_cap_append(const char *s, size_t n) {
    if (g_log_cap_len + n + 1 > g_log_cap_cap) {
        size_t nc = g_log_cap_cap ? g_log_cap_cap : 1024;
        while (nc < g_log_cap_len + n + 1) nc *= 2;
        char *np = realloc(g_log_cap, nc);
        if (!np) return;
        g_log_cap = np; g_log_cap_cap = nc;
    }
    memcpy(g_log_cap + g_log_cap_len, s, n);
    g_log_cap_len += n;
    g_log_cap[g_log_cap_len] = '\0';
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
        case VAL_BOUND_METHOD: return a.as.bound_method == b.as.bound_method;
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
    loop->continue_count = 0;
    loop->scope_depth = compiler->scope_depth;
}

/* Patch every recorded `continue` jump in the innermost loop to land at
 * `target` — the loop's continue point. For a range-`for` that is the
 * loop-variable increment (so `continue` advances the counter instead of
 * spinning forever on the same value); for `while`/array-`for`/`loop` it is the
 * back-edge that re-tests the condition. Called once per loop, after the body. */
static void compiler_patch_continues(Compiler *compiler, int target) {
    if (compiler->loop_count <= 0) return;
    LoopInfo *loop = &compiler->loops[compiler->loop_count - 1];
    for (int i = 0; i < loop->continue_count; i++) {
        int offset = loop->continue_jumps[i];
        int jump = target - offset - 2;
        compiler->chunk->code[offset] = (jump >> 8) & 0xFF;
        compiler->chunk->code[offset + 1] = jump & 0xFF;
    }
    loop->continue_count = 0;
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
            /* Logical `and`/`or` short-circuit: the right operand must not be
             * evaluated once the left already decides the result. Without this,
             * idioms like `x == null or x.len() == 0` call .len() on a null and
             * crash. BC_JUMP_IF_FALSE peeks (does not pop), so the operand that
             * survives is the expression's value (Lua/Python style). Bitwise
             * `&`/`|` (OP_BIT_AND/OP_BIT_OR) stay eager below. */
            if (node->binary.op == OP_AND) {
                compile_expression(compiler, node->binary.left);
                int end_jump = emit_jump(compiler, BC_JUMP_IF_FALSE);
                emit_byte(compiler, BC_POP);   /* discard truthy left, take right */
                compile_expression(compiler, node->binary.right);
                patch_jump(compiler, end_jump);
                break;
            }
            if (node->binary.op == OP_OR) {
                compile_expression(compiler, node->binary.left);
                int else_jump = emit_jump(compiler, BC_JUMP_IF_FALSE);
                int end_jump = emit_jump(compiler, BC_JUMP);
                patch_jump(compiler, else_jump);
                emit_byte(compiler, BC_POP);   /* discard falsy left, take right */
                compile_expression(compiler, node->binary.right);
                patch_jump(compiler, end_jump);
                break;
            }
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

static int vm_type_to_str(Type *t, char *buf, int cap) {
    if (!t) return snprintf(buf, cap, "any");
    switch (t->kind) {
    case TYPE_PRIMITIVE: {
        const char *names[] = {
            [PRIMITIVE_BOOL] = "bool", [PRIMITIVE_INT] = "int",
            [PRIMITIVE_FLOAT] = "float", [PRIMITIVE_STRING] = "string",
            [PRIMITIVE_BYTE] = "byte", [PRIMITIVE_VOID] = "void",
            [PRIMITIVE_PTR] = "ptr", [PRIMITIVE_C_INT] = "c_int",
            [PRIMITIVE_C_DOUBLE] = "c_double", [PRIMITIVE_C_FLOAT] = "c_float",
            [PRIMITIVE_C_CHAR] = "c_char",
        };
        if ((int)t->primitive < (int)(sizeof(names)/sizeof(names[0])) && names[t->primitive])
            return snprintf(buf, cap, "%s", names[t->primitive]);
        return snprintf(buf, cap, "primitive");
    }
    case TYPE_NAMED:
        return snprintf(buf, cap, "%s", t->named.name);
    case TYPE_ARRAY: {
        char elem[64];
        vm_type_to_str(t->array.element_type, elem, sizeof(elem));
        return snprintf(buf, cap, "[%s]", elem);
    }
    case TYPE_TUPLE: {
        int pos = snprintf(buf, cap, "(");
        for (int i = 0; i < t->tuple.count && pos < cap - 4; i++) {
            if (i > 0) pos += snprintf(buf + pos, cap - pos, ", ");
            pos += vm_type_to_str(t->tuple.types[i], buf + pos, cap - pos);
        }
        pos += snprintf(buf + pos, cap - pos, ")");
        return pos;
    }
    case TYPE_FUNCTION: {
        int pos = snprintf(buf, cap, "fn(");
        for (int i = 0; i < t->function.param_count && pos < cap - 4; i++) {
            if (i > 0) pos += snprintf(buf + pos, cap - pos, ", ");
            pos += vm_type_to_str(t->function.param_types[i], buf + pos, cap - pos);
        }
        char ret[64];
        vm_type_to_str(t->function.return_type, ret, sizeof(ret));
        pos += snprintf(buf + pos, cap - pos, ") -> %s", ret);
        return pos;
    }
    default:
        return snprintf(buf, cap, "any");
    }
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
                        int idx = compiler_find_local(compiler, name);
                        if (idx < 0 || compiler->local_depths[idx] < compiler->scope_depth) {
                            idx = compiler_add_local(compiler, name);
                        }
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

            /* Pass 1 — Hoisting pass for module-init functions */
            if (node->fn_decl.is_module_init && node->fn_decl.body) {
                AstNode *body_block = node->fn_decl.body;
                for (int i = 0; i < body_block->block.stmt_count; i++) {
                    AstNode *stmt = body_block->block.stmts[i];
                    if (stmt) {
                        if (stmt->kind == NODE_FN_DECL) {
                            compiler_add_local(&fn_compiler, stmt->fn_decl.name);
                        } else if (stmt->kind == NODE_LET_DECL || stmt->kind == NODE_CONST_DECL) {
                            for (int j = 0; j < stmt->let_decl.name_count; j++) {
                                const char *name = stmt->let_decl.names[j];
                                if (strcmp(name, "_") != 0) {
                                    compiler_add_local(&fn_compiler, name);
                                }
                            }
                        }
                    }
                }
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
            func->is_module_init = node->fn_decl.is_module_init;
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
                emit_bytes(compiler, BC_CLOSURE, (uint8_t)fn_compiler.upvalue_count);
                for (int i = 0; i < fn_compiler.upvalue_count; i++) {
                    emit_byte(compiler, fn_compiler.upvalue_is_local[i] ? 1 : 0);
                    emit_byte(compiler, fn_compiler.upvalue_index[i]);
                }
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
             * skip binding so the function value stays on the stack. */
            if (strcmp(node->fn_decl.name, "__lambda__") != 0) {
                if (compiler->in_function) {
                    int idx = compiler_find_local(compiler, node->fn_decl.name);
                    if (idx < 0 || compiler->local_depths[idx] < compiler->scope_depth) {
                        idx = compiler_add_local(compiler, node->fn_decl.name);
                    }
                    emit_bytes(compiler, BC_SET_LOCAL, (uint8_t)idx);
                } else {
                    emit_byte(compiler, BC_DEFINE_GLOBAL);
                    {
                        ObjString *s = copy_string(node->fn_decl.name,
                                                   (int)strlen(node->fn_decl.name));
                        int idx = chunk_add_constant(compiler->chunk, val_string(s));
                        emit_short(compiler, (uint16_t)idx);
                    }
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
            compiler_patch_continues(compiler, compiler->chunk->count);
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

                /* continue -> run the increment, don't skip it */
                compiler_patch_continues(compiler, compiler->chunk->count);
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

                /* continue -> run the increment, don't skip it */
                compiler_patch_continues(compiler, compiler->chunk->count);
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
            } else if (compiler->scope_depth > 0) {
                /* for var in <array-expr> { body } — inside a function (locals).
                 * Desugars to: __arr__=expr; __i__=0; __len__=__arr__.len();
                 *   while __i__ < __len__ { var = __arr__[__i__]; body; __i__++ } */
                compile_expression(compiler, node->for_stmt.iterable);
                int arr_idx = compiler_add_local(compiler, "__arr__");
                emit_bytes(compiler, BC_SET_LOCAL, (uint8_t)arr_idx);

                int zero_idx = chunk_add_constant(compiler->chunk, val_int(0));
                emit_constant_idx(compiler, zero_idx);
                int i_idx = compiler_add_local(compiler, "__i__");
                emit_bytes(compiler, BC_SET_LOCAL, (uint8_t)i_idx);

                emit_bytes(compiler, BC_GET_LOCAL, (uint8_t)arr_idx);
                emit_byte(compiler, BC_DISPATCH);
                {
                    ObjString *s = copy_string("len", 3);
                    int idx = chunk_add_constant(compiler->chunk, val_string(s));
                    emit_short(compiler, (uint16_t)idx);
                }
                emit_byte(compiler, 0); /* arg count */
                int len_idx = compiler_add_local(compiler, "__len__");
                emit_bytes(compiler, BC_SET_LOCAL, (uint8_t)len_idx);

                emit_byte(compiler, BC_NIL);
                int var_idx = compiler_add_local(compiler, var_name);
                emit_bytes(compiler, BC_SET_LOCAL, (uint8_t)var_idx);

                int loop_start = compiler->chunk->count;
                compiler_push_loop(compiler, loop_start);

                emit_bytes(compiler, BC_GET_LOCAL, (uint8_t)i_idx);
                emit_bytes(compiler, BC_GET_LOCAL, (uint8_t)len_idx);
                emit_byte(compiler, BC_LESS);
                int exit_jump = emit_jump(compiler, BC_JUMP_IF_FALSE);
                emit_byte(compiler, BC_POP);

                emit_bytes(compiler, BC_GET_LOCAL, (uint8_t)arr_idx);
                emit_bytes(compiler, BC_GET_LOCAL, (uint8_t)i_idx);
                emit_byte(compiler, BC_INDEX);
                emit_bytes(compiler, BC_SET_LOCAL, (uint8_t)var_idx);
                emit_byte(compiler, BC_POP);

                compile_node(compiler, node->for_stmt.body);

                compiler_patch_continues(compiler, compiler->chunk->count);
                emit_bytes(compiler, BC_GET_LOCAL, (uint8_t)i_idx);
                int one_idx = chunk_add_constant(compiler->chunk, val_int(1));
                emit_constant_idx(compiler, one_idx);
                emit_byte(compiler, BC_ADD);
                emit_bytes(compiler, BC_SET_LOCAL, (uint8_t)i_idx);
                emit_byte(compiler, BC_POP);

                emit_loop(compiler, loop_start);
                patch_jump(compiler, exit_jump);
                emit_byte(compiler, BC_POP);
                int break_target = compiler->chunk->count;
                compiler_pop_loop(compiler, break_target);
            } else {
                /* for var in <array-expr> { body } — at top level (globals). */
                compile_expression(compiler, node->for_stmt.iterable);
                {
                    ObjString *s = copy_string("__arr__", 7);
                    int idx = chunk_add_constant(compiler->chunk, val_string(s));
                    emit_byte(compiler, BC_DEFINE_GLOBAL);
                    emit_short(compiler, (uint16_t)idx);
                }
                int zero_idx = chunk_add_constant(compiler->chunk, val_int(0));
                emit_constant_idx(compiler, zero_idx);
                {
                    ObjString *s = copy_string("__i__", 5);
                    int idx = chunk_add_constant(compiler->chunk, val_string(s));
                    emit_byte(compiler, BC_DEFINE_GLOBAL);
                    emit_short(compiler, (uint16_t)idx);
                }
                {
                    ObjString *s = copy_string("__arr__", 7);
                    int idx = chunk_add_constant(compiler->chunk, val_string(s));
                    emit_byte(compiler, BC_GET_GLOBAL);
                    emit_short(compiler, (uint16_t)idx);
                }
                emit_byte(compiler, BC_DISPATCH);
                {
                    ObjString *s = copy_string("len", 3);
                    int idx = chunk_add_constant(compiler->chunk, val_string(s));
                    emit_short(compiler, (uint16_t)idx);
                }
                emit_byte(compiler, 0);
                {
                    ObjString *s = copy_string("__len__", 7);
                    int idx = chunk_add_constant(compiler->chunk, val_string(s));
                    emit_byte(compiler, BC_DEFINE_GLOBAL);
                    emit_short(compiler, (uint16_t)idx);
                }

                int loop_start = compiler->chunk->count;
                compiler_push_loop(compiler, loop_start);

                {
                    ObjString *s = copy_string("__i__", 5);
                    int idx = chunk_add_constant(compiler->chunk, val_string(s));
                    emit_byte(compiler, BC_GET_GLOBAL);
                    emit_short(compiler, (uint16_t)idx);
                }
                {
                    ObjString *s = copy_string("__len__", 7);
                    int idx = chunk_add_constant(compiler->chunk, val_string(s));
                    emit_byte(compiler, BC_GET_GLOBAL);
                    emit_short(compiler, (uint16_t)idx);
                }
                emit_byte(compiler, BC_LESS);
                int exit_jump = emit_jump(compiler, BC_JUMP_IF_FALSE);
                emit_byte(compiler, BC_POP);

                {
                    ObjString *s = copy_string("__arr__", 7);
                    int idx = chunk_add_constant(compiler->chunk, val_string(s));
                    emit_byte(compiler, BC_GET_GLOBAL);
                    emit_short(compiler, (uint16_t)idx);
                }
                {
                    ObjString *s = copy_string("__i__", 5);
                    int idx = chunk_add_constant(compiler->chunk, val_string(s));
                    emit_byte(compiler, BC_GET_GLOBAL);
                    emit_short(compiler, (uint16_t)idx);
                }
                emit_byte(compiler, BC_INDEX);
                {
                    ObjString *s = copy_string(var_name, (int)strlen(var_name));
                    int idx = chunk_add_constant(compiler->chunk, val_string(s));
                    emit_byte(compiler, BC_DEFINE_GLOBAL);
                    emit_short(compiler, (uint16_t)idx);
                }

                compile_node(compiler, node->for_stmt.body);

                compiler_patch_continues(compiler, compiler->chunk->count);
                {
                    ObjString *s = copy_string("__i__", 5);
                    int idx = chunk_add_constant(compiler->chunk, val_string(s));
                    emit_byte(compiler, BC_GET_GLOBAL);
                    emit_short(compiler, (uint16_t)idx);
                }
                int one_idx = chunk_add_constant(compiler->chunk, val_int(1));
                emit_constant_idx(compiler, one_idx);
                emit_byte(compiler, BC_ADD);
                {
                    ObjString *s = copy_string("__i__", 5);
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
            }
            compiler->local_count = saved_local_count;
            break;
        }

        case NODE_LOOP: {
            int loop_start = compiler->chunk->count;
            compiler_push_loop(compiler, loop_start);
            compile_node(compiler, node->loop_stmt.body);
            compiler_patch_continues(compiler, compiler->chunk->count);
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
                /* Forward jump to the loop's continue target, patched after the
                 * body (see compiler_patch_continues). Jumping straight to
                 * loop_start used to skip a range-for's increment -> infinite
                 * loop on `continue`. */
                int jump_offset = emit_jump(compiler, BC_JUMP);
                if (loop->continue_count < 64) {
                    loop->continue_jumps[loop->continue_count++] = jump_offset;
                }
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

        case NODE_SCHEMA_DECL: {
            /* 1. Register validations runtime-wise just like struct */
            bool has_validations = (node->schema_decl.decorator_count > 0);
            if (!has_validations) {
                for (int i = 0; i < node->schema_decl.field_count; i++) {
                    if (node->schema_decl.field_decorator_counts &&
                        node->schema_decl.field_decorator_counts[i] > 0) {
                        has_validations = true;
                        break;
                    }
                }
            }

            if (has_validations) {
                emit_byte(compiler, BC_REGISTER_VALIDATIONS);
                ObjString *ts = copy_string(node->schema_decl.name, (int)strlen(node->schema_decl.name));
                int ti = chunk_add_constant(compiler->chunk, val_string(ts));
                emit_short(compiler, (uint16_t)ti);
                emit_byte(compiler, (uint8_t)node->schema_decl.decorator_count);
                for (int i = 0; i < node->schema_decl.decorator_count; i++) {
                    ObjString *key = copy_string(node->schema_decl.decorator_keys[i], (int)strlen(node->schema_decl.decorator_keys[i]));
                    int ki = chunk_add_constant(compiler->chunk, val_string(key));
                    emit_short(compiler, (uint16_t)ki);
                    Value arg_val = val_bool(true);
                    if (!decorator_literal_to_value(node->schema_decl.decorator_values[i], &arg_val)) {
                        compiler_error(compiler, "Decorator arguments must be literal values");
                    }
                    int ai = chunk_add_constant(compiler->chunk, arg_val);
                    emit_short(compiler, (uint16_t)ai);
                }
                emit_byte(compiler, (uint8_t)node->schema_decl.field_count);
                for (int i = 0; i < node->schema_decl.field_count; i++) {
                    ObjString *fname = copy_string(node->schema_decl.field_names[i], (int)strlen(node->schema_decl.field_names[i]));
                    int fi = chunk_add_constant(compiler->chunk, val_string(fname));
                    emit_short(compiler, (uint16_t)fi);
                    int fcount = node->schema_decl.field_decorator_counts ? node->schema_decl.field_decorator_counts[i] : 0;
                    emit_byte(compiler, (uint8_t)fcount);
                    for (int j = 0; j < fcount; j++) {
                        ObjString *fkey = copy_string(node->schema_decl.field_decorator_keys[i][j], (int)strlen(node->schema_decl.field_decorator_keys[i][j]));
                        int fki = chunk_add_constant(compiler->chunk, val_string(fkey));
                        emit_short(compiler, (uint16_t)fki);
                        Value arg_val = val_bool(true);
                        if (!decorator_literal_to_value(node->schema_decl.field_decorator_values[i][j], &arg_val)) {
                            compiler_error(compiler, "Decorator arguments must be literal values");
                        }
                        int afi = chunk_add_constant(compiler->chunk, arg_val);
                        emit_short(compiler, (uint16_t)afi);
                    }
                }
            }

            /* 2. Emit global __schema_metadata_Name struct */
            ObjString *empty_s = copy_string("", 0);
            int empty_idx = chunk_add_constant(compiler->chunk, val_string(empty_s));

            for (int i = 0; i < node->schema_decl.field_count; i++) {
                char tbuf[128];
                vm_type_to_str(node->schema_decl.field_types[i], tbuf, sizeof(tbuf));
                ObjString *tstr = copy_string(tbuf, (int)strlen(tbuf));
                emit_constant(compiler, val_string(tstr));

                int fcount = node->schema_decl.field_decorator_counts ? node->schema_decl.field_decorator_counts[i] : 0;
                for (int j = 0; j < fcount; j++) {
                    Value arg_val = val_bool(true);
                    decorator_literal_to_value(node->schema_decl.field_decorator_values[i][j], &arg_val);
                    emit_constant(compiler, arg_val);
                }

                emit_bytes(compiler, BC_STRUCT, (uint8_t)(1 + fcount));
                emit_short(compiler, (uint16_t)empty_idx);

                ObjString *type_k = copy_string("type", 4);
                int type_k_idx = chunk_add_constant(compiler->chunk, val_string(type_k));
                emit_short(compiler, (uint16_t)type_k_idx);

                for (int j = 0; j < fcount; j++) {
                    ObjString *fkey = copy_string(node->schema_decl.field_decorator_keys[i][j], (int)strlen(node->schema_decl.field_decorator_keys[i][j]));
                    int fki = chunk_add_constant(compiler->chunk, val_string(fkey));
                    emit_short(compiler, (uint16_t)fki);
                }
            }

            emit_bytes(compiler, BC_STRUCT, (uint8_t)node->schema_decl.field_count);
            emit_short(compiler, (uint16_t)empty_idx);
            for (int i = 0; i < node->schema_decl.field_count; i++) {
                ObjString *fkey = copy_string(node->schema_decl.field_names[i], (int)strlen(node->schema_decl.field_names[i]));
                int fki = chunk_add_constant(compiler->chunk, val_string(fkey));
                emit_short(compiler, (uint16_t)fki);
            }

            char var_name[256];
            snprintf(var_name, sizeof(var_name), "__schema_metadata_%s", node->schema_decl.name);
            emit_byte(compiler, BC_DEFINE_GLOBAL);
            ObjString *var_s = copy_string(var_name, (int)strlen(var_name));
            int var_idx = chunk_add_constant(compiler->chunk, val_string(var_s));
            emit_short(compiler, (uint16_t)var_idx);
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
        /* Avoid memset of the entire 65KB+ Task struct — reset only the
         * fields that must be clean. The stack, frames, and try_stack are
         * governed by stack_top/frame_count/try_count so stale data below
         * those markers is never read. */
        t->stack_top = 0;
        t->frame_count = 0;
        t->try_count = 0;
        t->throw_value = val_nil();
        t->is_throwing = false;
        t->result = val_nil();
        t->dead = false;
        t->yielded = false;
        t->waiting_actor_reply = false;
        t->actor_reply_ch = val_nil();
        t->is_actor_loop = false;
        t->actor_ref = NULL;
        t->cache_on_return = false;
        t->cache_result_key = 0;
        t->http_listen_fd = -1;
        t->wakeup_time = 0.0;
        t->http_response_fd = -1;
        t->http_response_ssl = NULL;
        t->http_pending_conns = NULL;
        t->arena_base = saved_arena;
        t->arena_offset = 0;
        t->use_arena = false;
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
    memset(vm->dispatch_pic_keys, 0, sizeof(vm->dispatch_pic_keys));
    for (int i = 0; i < VM_DISPATCH_PIC_SIZE; i++)
        vm->dispatch_pic_idxs[i] = -1;
    vm->free_tasks = NULL;
    vm->source = NULL;
    vm->source_name = NULL;
    vm->prelude_line_count = 0;
    memset(vm->dispatch_occupied, 0, sizeof(vm->dispatch_occupied));
    vm->gray_stack = NULL;
    vm->gray_capacity = 0;
    vm->gray_count = 0;
    vm->bytes_allocated = 0;
    vm->next_gc_size = 1024 * 1024;
    vm->intern_table = NULL;
    vm->intern_capacity = 0;
    vm->intern_count = 0;
    memset(vm->method_interns, 0, sizeof(vm->method_interns));
    memset(vm->globals, 0, sizeof(vm->globals));
    vm->ffi_entries = NULL;
    vm->ffi_entry_count = 0;
    vm->assets = NULL;
    vm->asset_count = 0;
    vm->actor_field_count = 0;
    vm->cache_map = NULL;
    vm->cache_map_count = 0;
    vm->cache_map_capacity = 0;
    vm->test_count = 0;
    memset(vm->tests, 0, sizeof(vm->tests));
    vm->validation_registry.count = 0;
    vm->shape_registry.count = 0;
    memset(vm->shape_registry.shapes, 0, sizeof(vm->shape_registry.shapes));
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
#ifndef VN_NO_FFI
    Compiler *compiler = vm->compiler;
    if (!compiler) return true;
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
#else
    (void)vm;
    return true;
#endif
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
            ObjFunction *fn = frame->function;
            /* The source-run path keeps main_fn's line table in the live
             * compiler chunk; the bundle (.vnb) and AOT paths have no compiler
             * but every ObjFunction carries its own RLE table — fall back to it
             * so a runtime error in a built artifact reports a line instead of
             * dereferencing a NULL compiler. */
            if (fn == vm->main_fn && vm->compiler && vm->compiler->chunk) {
                line = chunk_get_line(vm->compiler->chunk, offset);
            } else {
                int pos = 0;
                for (int j = 0; j < fn->rle_count; j++) {
                    pos += fn->rle_counts[j];
                    if (offset < pos) { line = fn->rle_lines[j]; break; }
                }
            }
            if (!vm->suppress_error_print) {
                /* Show the user's own line number: the runtime line is relative
                 * to the combined (prelude + user) source, so subtract the
                 * prelude length. Non-positive => error is inside a prelude
                 * module, so keep the raw number. */
                int disp_line = line - vm->prelude_line_count;
                if (disp_line <= 0) disp_line = line;
                /* A natively-compiled (AOT) frame has no bytecode position, so
                 * `line` can be 0 — don't print a misleading "[line 0]". */
                if (line >= 1) fprintf(stderr, "[line %d] in script\n", disp_line);
                else           fprintf(stderr, "in script (compiled binary)\n");
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
    /* Dev capture path: build the line once into a memory stream, then send it
     * to BOTH the terminal and the Lumen log buffer (so server logs appear in
     * the browser devtools without leaving the terminal). */
    if (g_log_capturing && arg_count > 0) {
        FILE *ms = tmpfile();
        if (ms) {
            for (int i = 0; i < arg_count; i++) {
                value_fprint(ms, args[i]);
                if (i < arg_count - 1) fputc(' ', ms);
            }
            fputc('\n', ms);
            fflush(ms); long pos = ftell(ms); rewind(ms);
            char *buf = malloc(pos ? (size_t)pos : 1);
            size_t sz = buf ? fread(buf, 1, pos, ms) : 0;
            fclose(ms);
            if (buf) {
                fwrite(buf, 1, sz, stdout);
                fflush(stdout);
                log_cap_append(buf, sz);
                free(buf);
            }
            return val_nil();
        }
    }
    for (int i = 0; i < arg_count; i++) {
        value_print(args[i]);
        if (i < arg_count - 1) printf(" ");
    }
    if (arg_count > 0) printf("\n");
    fflush(stdout);
    return val_nil();
}

/* `__lumen_log_start()` — enable print capture and clear the buffer (Lumen dev).
 * `__lumen_log_drain()` — return captured text since last drain, clearing it. */
static Value native_lumen_log_start(VM *vm, int arg_count, Value *args) {
    (void)vm; (void)arg_count; (void)args;
    g_log_capturing = true;
    g_log_cap_len = 0;
    if (g_log_cap) g_log_cap[0] = '\0';
    return val_nil();
}

static Value native_lumen_log_drain(VM *vm, int arg_count, Value *args) {
    (void)vm; (void)arg_count; (void)args;
    if (g_log_cap_len == 0) return val_string(copy_string("", 0));
    Value r = val_string(copy_string(g_log_cap, (int)g_log_cap_len));
    g_log_cap_len = 0;
    if (g_log_cap) g_log_cap[0] = '\0';
    return r;
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

/* Lumen template escaping in one O(n) C pass. The pure-Varian version built the
 * result with `out = out + c` char-by-char, which is O(n^2) in both time and
 * garbage — a ~100KB template (a docs/book page) blew up to multiple GB and got
 * OOM-killed at compile time. Escapes for embedding template text inside a
 * Varian double-quoted string literal: backslash, quote, braces (so Lumen's
 * own {expr} interpolation in the generated code isn't eaten) and the usual
 * control chars. Must stay byte-for-byte identical to _lumen_escape_string. */
static Value native_lumen_escape_str(VM *vm, int arg_count, Value *args) {
    if (arg_count < 1 || args[0].type != VAL_STRING) return val_nil();
    ObjString *in = args[0].as.string;
    const char *s = in->chars;
    int n = in->length;
    /* Worst case every byte doubles. */
    char *out = (char *)malloc((size_t)n * 2 + 1);
    if (!out) return val_nil();
    int j = 0;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        switch (c) {
            case '\\': out[j++] = '\\'; out[j++] = '\\'; break;
            case '"':  out[j++] = '\\'; out[j++] = '"';  break;
            case '{':  out[j++] = '\\'; out[j++] = '{';  break;
            case '}':  out[j++] = '\\'; out[j++] = '}';  break;
            case '\n': out[j++] = '\\'; out[j++] = 'n';  break;
            case '\r': out[j++] = '\\'; out[j++] = 'r';  break;
            case '\t': out[j++] = '\\'; out[j++] = 't';  break;
            default:   out[j++] = c;                     break;
        }
    }
    out[j] = '\0';
    ObjString *result = allocate_string(vm, out, j);
    free(out);
    return val_string(result);
}

/* Report an uncaught throw, INCLUDING the thrown value, so `throw("msg")` and
 * `throw(SomeStruct{...})` surface their payload to the developer instead of a
 * bare "Unhandled exception". A thrown string is shown raw (no JSON quotes);
 * anything else is JSON-encoded. */
static void unhandled_exception(VM *vm, Value v) {
    if (v.type == VAL_STRING && v.as.string) {
        runtime_error(vm, "Unhandled exception: %s", v.as.string->chars);
    } else if (v.type == VAL_NIL) {
        runtime_error(vm, "Unhandled exception");
    } else {
        char *s = stringify_value(vm, v);
        runtime_error(vm, "Unhandled exception: %s", s);
        free(s);
    }
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

    /* 1. Create inner state struct. Every actor.spawn() of the same actor
     * type shares one Shape -- fixed field set per type_name. */
    ObjStruct *state = new_struct(vm, info->field_count, false);
    if (state->field_names) free(state->field_names);
    /* info->field_names is a 2D buffer (char[64][64]), but struct_attach_shape
     * expects an array of char* pointers. Casting the 2D array straight to
     * (char*const*) reinterprets each 64-byte row as eight pointers, so
     * field_names[0] became the raw bytes of the name itself (e.g. "ips" ->
     * 0x737069) and strdup() crashed on that bogus address. Build a proper
     * pointer view into each row first. */
    char *name_ptrs[MAX_ACTOR_TYPES];
    for (int i = 0; i < info->field_count; i++)
        name_ptrs[i] = info->field_names[i];
    struct_attach_shape(vm, state, type_name, name_ptrs, info->field_count);
    for (int i = 0; i < info->field_count; i++) {
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

    /* Invalidate any cached PIC entry for this (type, method) so
     * mock.intercept/mock.restore take effect immediately. */
    int pic_slot = hash & (VM_DISPATCH_PIC_SIZE - 1);
    if (vm->dispatch_pic_keys[pic_slot] == hash) {
        vm->dispatch_pic_keys[pic_slot] = 0;
        vm->dispatch_pic_idxs[pic_slot] = -1;
    }

    int mask = DISPATCH_TABLE_SIZE - 1;
    for (int i = 0; i < DISPATCH_TABLE_SIZE; i++) {
        int idx = (hash + i) & mask;
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

    /* Stage 2: check per-VM direct-mapped PIC cache first. Stores the table
     * index so the returned pointer always points into the real dispatch
     * table (safe across mock.intercept/mock.restore, which update entries
     * in place via vm_register_dispatch). */
    int pic_slot = hash & (VM_DISPATCH_PIC_SIZE - 1);
    if (vm->dispatch_pic_keys[pic_slot] == hash) {
        int16_t idx = vm->dispatch_pic_idxs[pic_slot];
        if (idx >= 0) return &vm->dispatch_functions[idx];
        return NULL;  /* cached miss */
    }

    int mask = DISPATCH_TABLE_SIZE - 1;
    for (int i = 0; i < DISPATCH_TABLE_SIZE; i++) {
        int idx = (hash + i) & mask;
        if (!vm->dispatch_occupied[idx]) {
            vm->dispatch_pic_keys[pic_slot] = hash;
            vm->dispatch_pic_idxs[pic_slot] = -1;  /* cache the miss */
            return NULL;
        }
        if (strcmp(vm->dispatch_type_names[idx], type_name) == 0 &&
            strcmp(vm->dispatch_method_names[idx], method_name) == 0) {
            vm->dispatch_pic_keys[pic_slot] = hash;
            vm->dispatch_pic_idxs[pic_slot] = (int16_t)idx;
            return &vm->dispatch_functions[idx];
        }
    }
    vm->dispatch_pic_keys[pic_slot] = hash;
    vm->dispatch_pic_idxs[pic_slot] = -1;
    return NULL;
}

void close_upvalues(VM *vm, CallFrame *frame) {
    for (Obj *obj = vm->objects; obj != NULL; obj = obj->next) {
        if (obj->type == VAL_CLOSURE) {
            ObjClosure *closure = (ObjClosure *)obj;
            /* Only snapshot closures that captured locals of THIS frame — the
             * slot indices are meaningless against any other frame's slots. */
            if (closure->captured_slots && closure->captured_owner == frame->slots) {
                for (int i = 0; i < closure->captured_count; i++) {
                    if (closure->captured_slots[i] >= 0) {
                        closure->captured[i] = frame->slots[closure->captured_slots[i]];
                        closure->captured_slots[i] = -1;
                    }
                }
            }
        }
    }
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
        /* Must be the stack base, not NULL: the try/catch unwinder computes a
         * catch's restore depth as (frame->slots - stack) + local_count, so a
         * NULL slots base makes that arithmetic garbage and corrupts the stack
         * on any top-level `try`/`catch` (segfault). Every other frame-0 setup
         * uses the stack base; this one was the lone exception. */
        init_task->frames[0].slots = init_task->stack;
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
    define_global(vm, copy_string("_lumen_escape_str", 17), val_native_fn((void *)native_lumen_escape_str));
    define_global(vm, copy_string("__lumen_log_start", 17), val_native_fn((void *)native_lumen_log_start));
    define_global(vm, copy_string("__lumen_log_drain", 17), val_native_fn((void *)native_lumen_log_drain));

    /* Initialize built-in modules */
    {
        extern void lib_math_init(VM *vm);
        extern void lib_env_init(VM *vm);
        extern void lib_io_init(VM *vm);
        extern void lib_string_init(VM *vm);
#ifndef VN_NO_HTTP
        extern void lib_http_init(VM *vm);
#endif
        extern void lib_python_init(VM *vm);
#ifndef VN_NO_POSTGRES
        extern void lib_postgres_init(VM *vm);
#endif
        extern void lib_validate_init(VM *vm);
        extern void lib_sanitize_init(VM *vm);
        extern void lib_auth_init(VM *vm);
        extern void lib_task_init(VM *vm);
#ifndef VN_NO_SQLITE
        extern void lib_sqlite_init(VM *vm);
#endif
#ifndef VN_NO_REDIS
        extern void lib_redis_init(VM *vm);
#endif
#ifndef VN_NO_SMTP
        extern void lib_smtp_init(VM *vm);
#endif
        extern void lib_mock_init(VM *vm);
        extern void lib_time_init(VM *vm);
        extern void lib_regex_init(VM *vm);
        extern void lib_errors_init(VM *vm);
        lib_math_init(vm);
        lib_env_init(vm);
        lib_io_init(vm);
        lib_string_init(vm);
#ifndef VN_NO_HTTP
        lib_http_init(vm);
#endif
        lib_python_init(vm);
#ifndef VN_NO_POSTGRES
        lib_postgres_init(vm);
#endif
        lib_validate_init(vm);
        lib_sanitize_init(vm);
        lib_auth_init(vm);
        lib_task_init(vm);
#ifndef VN_NO_SQLITE
        lib_sqlite_init(vm);
#endif
#ifndef VN_NO_REDIS
        lib_redis_init(vm);
#endif
        lib_mock_init(vm);
#ifndef VN_NO_SMTP
        lib_smtp_init(vm);
#endif
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
#ifndef VN_NO_HTTP
                bool tick_had_error = vm->had_error;
#endif
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
#ifndef VN_NO_HTTP
                    extern void http_finalize_deferred_response(VM *vm, Task *t, bool had_error);
                    http_finalize_deferred_response(vm, t, tick_had_error);
#endif
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
#ifndef VN_NO_HTTP
                    extern void http_cleanup_pending_conns(Task *t);
                    http_cleanup_pending_conns(rt); /* closes fds, frees buffers */
#endif
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
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };
            nanosleep(&ts, NULL);
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
        vm->cache_map = (Value *)vm_xrealloc(vm->cache_map, (size_t)new_cap * sizeof(Value));
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
        /* An error can be raised mid-call after the @cache "intercept the next
         * return" flag was armed; unwinding past that return would otherwise
         * leave it set and corrupt the next cached call. Clear it on unwind. */
        t->cache_on_return = false;
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

/* Raise a runtime error from inside the dispatch loop. If a try block is active
 * on the current task, the error becomes a catchable exception (unwinds to the
 * catch with the message string) and execution continues; otherwise it prints
 * the friendly fatal error + traceback and exits task_run. This makes ordinary
 * language errors (bad index, missing field, wrong arg count, type mismatch)
 * catchable by `try/catch` and Zenith's `app.on_error`, not just `throw`. Use
 * ONLY inside task_run — it expands to DISPATCH()/return. */
#define RAISE(...) do { \
        char _raise_buf[512]; \
        snprintf(_raise_buf, sizeof(_raise_buf), __VA_ARGS__); \
        if (handle_vm_exception(vm, _raise_buf)) DISPATCH(); \
        runtime_error(vm, "%s", _raise_buf); \
        return false; \
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
                    if ((long)la + (long)lb > INT_MAX - 1) {
                        RAISE("String concatenation result too large");
                    }
                    int len = la + lb;
                    char *chars = (char *)malloc((size_t)len + 1);
                    if (!chars) { RAISE("Out of memory in string concatenation"); }
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
                else { RAISE("Operand must be a number"); }
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

                closure->captured_slots = (int *)malloc(sizeof(int) * upvalue_count);
                CallFrame *frame = &t->frames[t->frame_count - 1];
                closure->captured_owner = frame->slots;  /* identity for close_upvalues */
                /* Only a module initializer defers its captures: its hoisted
                 * siblings are captured before assignment, so they must be
                 * snapshotted at frame return. Every other function captures by
                 * value right here (slot marked -1 = already closed), which is
                 * the correct, footgun-free semantics for loops and currying. */
                bool defer = (frame->function && frame->function->is_module_init);
                for (int i = 0; i < upvalue_count; i++) {
                    uint8_t is_local = READ_BYTE();
                    uint8_t index = READ_BYTE();
                    if (is_local) {
                        closure->captured_slots[i] = defer ? index : -1;
                        closure->captured[i] = frame->slots[index];
                    } else {
                        closure->captured_slots[i] = -1;
                        closure->captured[i] = frame->closure->captured[index];
                    }
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

                /* ─── Bound method: extract self+method, reorganize stack so
                 * the existing VAL_NATIVE_FN / VAL_FUNCTION / VAL_CLOSURE
                 * branches below receive self as the first argument ─── */
                if (callee.type == VAL_BOUND_METHOD) {
                    ObjBoundMethod *bm = callee.as.bound_method;
                    Value self_val = bm->self;
                    Value method_val = bm->method;
                    int callee_pos = t->stack_top - 1 - arg_count;
                    for (int i = arg_count - 1; i >= 0; i--)
                        t->stack[callee_pos + 2 + i] = t->stack[callee_pos + 1 + i];
                    t->stack_top++;
                    t->stack[callee_pos] = method_val;
                    t->stack[callee_pos + 1] = self_val;
                    callee = method_val;
                    arg_count++;
                }

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
                            unhandled_exception(vm, t->throw_value);
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
                        RAISE("Expected %d arguments but got %d", fn->arity, arg_count);
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
                        RAISE("Expected %d arguments but got %d", fn->arity, arg_count);
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
                    RAISE("Can only call functions");
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
                if (frame->function && frame->function->is_module_init)
                    close_upvalues(vm, frame);   /* only modules defer-close */
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
                if (frame->function && frame->function->is_module_init)
                    close_upvalues(vm, frame);   /* only modules defer-close */
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
                        RAISE("Array index out of bounds");
                    }
                    PUSH(obj.as.array->elements[i]);
                } else {
                    RAISE("Indexing not supported for this type");
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
                        RAISE("Array index out of bounds");
                    }
                    ObjArray *arr = obj.as.array;
                    if (i >= arr->count) {
                        if (i >= arr->capacity) {
                            int old_cap = arr->capacity;
                            arr->capacity = i + 1;
                            if (arr->capacity < old_cap * 2) arr->capacity = old_cap * 2;
                            if (arr->capacity < 8) arr->capacity = 8;
                            Value *ne = (Value *)realloc(arr->elements, sizeof(Value) * arr->capacity);
                            if (!ne) { arr->capacity = old_cap; RAISE("Out of memory growing array"); }
                            arr->elements = ne;
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
                    RAISE("Index assignment not supported for this type");
                }
                DISPATCH();
            }

            L_BC_STRUCT:
            {
                uint8_t field_count = READ_BYTE();
                ObjString *type_name_str = READ_CONSTANT().as.string;
                ObjStruct *s = new_struct(vm, field_count, false);

                /* Pop field values from the stack before collecting names
                 * (names come after the values in the bytecode stream). */
                for (int i = field_count - 1; i >= 0; i--)
                    s->fields[i] = POP();

                /* Stage 1: collect field names from the bytecode stream, then
                 * look up / build the shared Shape. This gives us the shape
                 * pointer to store on the instance, and the Shape owns the names.
                 * Use a VLA on the stack to avoid a heap alloc for the temp array. */
                const char *tmp_names[64];  /* 64 fields max for VLA safety */
                int safe_fc = field_count < 64 ? field_count : 64;
                for (int i = 0; i < safe_fc; i++) {
                    ObjString *name = READ_CONSTANT().as.string;
                    tmp_names[i] = name->chars;
                }
                /* Consume any extra names beyond VLA limit (shouldn't happen) */
                for (int i = safe_fc; i < field_count; i++)
                    READ_CONSTANT();  /* discard */

                const char *tn = (type_name_str->length > 0) ? type_name_str->chars : NULL;
                s->shape = shape_get_or_create(vm, tn, tmp_names, field_count);
                /* Free the scratch field_names array new_struct allocated for
                 * native-code compatibility; BC_STRUCT uses shape->field_names. */
                if (s->field_names) { free(s->field_names); }
                /* Set the backward-compat alias pointers into the shared Shape */
                s->field_names = s->shape ? s->shape->field_names : NULL;
                s->type_name   = s->shape ? s->shape->type_name   : NULL;

                /* Look up and attach validation rules */
                bool has_validations = false;
                ValidationRegistry *reg = &vm->validation_registry;
                for (int r = 0; r < reg->count; r++) {
                    const char *shape_tn = s->shape ? s->shape->type_name : NULL;
                    if (!shape_tn) continue;
                    if (strcmp(reg->validations[r].type_name, shape_tn) == 0) {
                        StructValidationInfo *info = &reg->validations[r];
                        s->struct_validations = info->struct_validations;
                        s->struct_validation_count = info->struct_validation_count;
                        if (info->struct_validation_count > 0) has_validations = true;
                        for (int i = 0; i < field_count; i++) {
                            const char *fname = s->shape->field_names[i];
                            for (int j = 0; j < info->field_count; j++) {
                                if (strcmp(fname, info->field_names[j]) == 0) {
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

                if (has_validations) {
                    if (!run_struct_validations(vm, s)) return false;
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
                Value *func_val = NULL;
                if (obj.type == VAL_STRUCT) {
                    ObjStruct *s = obj.as.structure;
                    type_name = s->shape ? s->shape->type_name : NULL;
                    /* Stage 2: method dispatch PIC — shape_resolve_method caches
                     * resolved functions on the Shape for O(1) hit. */
                    if (s->shape && s->shape->type_name) {
                        uint32_t mh = hash_string(method_name->chars, method_name->length);
                        func_val = shape_resolve_method(vm, s->shape, method_name->chars, mh);
                    } else {
                        func_val = vm_find_dispatch(vm, type_name, method_name->chars);
                    }
                } else if (obj.type == VAL_MODULE) {
                    type_name = obj.as.module->name;
                    func_val = vm_find_dispatch(vm, type_name, method_name->chars);
                } else if (obj.type == VAL_STRING) {
                    type_name = "string";
                    func_val = vm_find_dispatch(vm, type_name, method_name->chars);
                } else if (obj.type == VAL_ARRAY) {
                    type_name = "array";
                    func_val = vm_find_dispatch(vm, type_name, method_name->chars);
                } else {
                    RAISE("Cannot dispatch method on this type");
                }

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
                                RAISE("Function '%s' expects %d arguments but got %d",
                                              method_name->chars, fn->arity, arg_count);
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
                                RAISE("Function '%s' expects %d arguments but got %d",
                                              method_name->chars, fn->arity, arg_count);
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

                /* M5: universal struct methods (set/get/has/keys) live under the
                 * "struct" namespace. Tried only after the per-type lookup and the
                 * stored-closure-field fallback above, so a user's same-named field
                 * or registered impl always wins over the built-ins. */
                if (!func_val && obj.type == VAL_STRUCT) {
                    func_val = vm_find_dispatch(vm, "struct", method_name->chars);
                }

                if (!func_val) {
                    RAISE("No method '%s' for type '%s'", method_name->chars,
                                  type_name ? type_name : "(anonymous struct)");
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
                        RAISE("Method '%s' expects %d arguments", method_name->chars, fn->arity);
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
                        RAISE("Method '%s' expects %d arguments", method_name->chars, fn->arity);
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
                    RAISE("Method is not callable");
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
                    unhandled_exception(vm, err);
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
                    if (s->shape) {
                        uint32_t name_hash = hash_string(name->chars, name->length);
                        found = shape_index_of(s->shape, name->chars, name_hash);
                    } else {
                        /* Fallback: native-built struct without shape, scan field_names scratch */
                        if (s->field_names) {
                            for (int i = 0; i < s->field_count; i++) {
                                if (s->field_names[i] &&
                                    strcmp(s->field_names[i], name->chars) == 0) {
                                    found = i; break;
                                }
                            }
                        }
                    }
                    if (found >= 0) {
                        PUSH(s->fields[found]);
                    } else {
                        Value *func_val = vm_find_dispatch(vm,
                            s->type_name ? s->type_name : (s->shape ? s->shape->type_name : NULL),
                            name->chars);
                        if (func_val) {
                            ObjBoundMethod *bm = (ObjBoundMethod *)calloc(1, sizeof(ObjBoundMethod));
                            bm->obj.type = VAL_BOUND_METHOD;
                            bm->obj.next = vm->objects;
                            vm->objects = (Obj *)bm;
                            bm->self = obj;
                            bm->method = *func_val;
                            PUSH(val_bound_method(bm));
                        } else {
                            RAISE("Struct has no field '%s'", name->chars);
                        }
                    }
                } else if (obj.type == VAL_MODULE) {
                    Value *func_val = vm_find_dispatch(vm, obj.as.module->name, name->chars);
                    if (func_val) {
                        PUSH(*func_val);
                    } else {
                        RAISE("Module '%s' has no member '%s'",
                                     obj.as.module->name, name->chars);
                    }
                } else if (obj.type == VAL_STRING) {
                    Value *func_val = vm_find_dispatch(vm, "string", name->chars);
                    if (func_val) {
                        PUSH(*func_val);
                    } else {
                        RAISE("String has no method '%s'", name->chars);
                    }
                } else if (obj.type == VAL_ARRAY) {
                    Value *func_val = vm_find_dispatch(vm, "array", name->chars);
                    if (func_val) {
                        PUSH(*func_val);
                    } else {
                        RAISE("Array has no method '%s'", name->chars);
                    }
                } else {
                    RAISE("Cannot access field on non-struct value");
                }
                DISPATCH();
            }

            L_BC_MEMBER_SAFE:
            {
                /* expr?.member: push nil instead of erroring when field absent. */
                ObjString *name = READ_CONSTANT().as.string;
                Value obj = POP();
                if (obj.type == VAL_STRUCT) {
                    ObjStruct *s = obj.as.structure;
                    int found = -1;
                    if (s->shape) {
                        uint32_t name_hash = hash_string(name->chars, name->length);
                        found = shape_index_of(s->shape, name->chars, name_hash);
                    } else if (s->field_names) {
                        for (int i = 0; i < s->field_count; i++) {
                            if (s->field_names[i] && strcmp(s->field_names[i], name->chars) == 0) {
                                found = i; break;
                            }
                        }
                    }
                    PUSH(found >= 0 ? s->fields[found] : val_nil());
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
                    RAISE("Cannot access field on non-struct value");
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
                    if (s->shape) {
                        uint32_t name_hash = hash_string(name->chars, name->length);
                        found = shape_index_of(s->shape, name->chars, name_hash);
                    } else if (s->field_names) {
                        for (int i = 0; i < s->field_count; i++) {
                            if (s->field_names[i] && strcmp(s->field_names[i], name->chars) == 0) {
                                found = i; break;
                            }
                        }
                    }
                    if (found >= 0) {
                        s->fields[found] = val;
                        PUSH(val);
                    } else {
                        RAISE("Struct has no field '%s'", name->chars);
                    }
                } else {
                    RAISE("Cannot set field on non-struct value");
                }
                DISPATCH();
            }

            L_BC_FFI_CALL:
            {
#ifndef VN_NO_FFI
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
                            void **slot = (void **)malloc(sizeof(void *));
                            if (v.type == VAL_INT) {
                                *slot = (void *)(uintptr_t)v.as.integer;
                            } else if (v.type == VAL_STRING) {
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

                void *ret_val = NULL;
                ffi_type *ret_ffi_type = ffi_type_from_kind(entry->return_kind);
                bool has_return = (entry->return_kind != FFI_VOID);

                if (has_return) {
                    ret_val = calloc(1, ret_ffi_type->size);
                }

                ffi_call(&entry->cif, FFI_FN(entry->fn_ptr), ret_val, args);

                t->stack_top -= arg_count;

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

                for (int i = 0; i < storage_used; i++)
                    free(value_storage[i]);
                if (ret_val) free(ret_val);
#else
                runtime_error(vm, "FFI not supported in this build");
                return false;
#endif
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
                long total_len64 = 0;
                for (int i = 0; i < count; i++) {
                    Value v = t->stack[t->stack_top - count + i];
                    if (v.type == VAL_STRING)
                        total_len64 += v.as.string->length;
                }
                if (total_len64 > INT_MAX - 1) {
                    t->stack_top -= count;
                    RAISE("Built string result too large");
                }
                int total_len = (int)total_len64;
                char *chars = (char *)malloc((size_t)total_len + 1);
                if (!chars) { t->stack_top -= count; RAISE("Out of memory building string"); }
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
                    if ((long)a.as.string->length + (long)b.as.string->length > INT_MAX - 1) {
                        RAISE("String concatenation result too large");
                    }
                    int len = a.as.string->length + b.as.string->length;
                    char *chars = (char *)malloc((size_t)len + 1);
                    if (!chars) { RAISE("Out of memory in string concatenation"); }
                    memcpy(chars, a.as.string->chars, a.as.string->length);
                    memcpy(chars + a.as.string->length, b.as.string->chars, b.as.string->length);
                    chars[len] = '\0';
                    ObjString *result = allocate_string(vm, chars, len);
                    free(chars);
                    PUSH(val_string(result));
                } else {
                    RAISE("String concatenation requires strings");
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
                /* Same as gc_sweep */
                if (s->shape && s->field_names != s->shape->field_names) {
                    for (int i = 0; i < s->field_count; i++)
                        free(s->field_names[i]);
                    free(s->field_names);
                }
                free(s->fields);
                free(s->field_validations);
                free(s->field_validation_counts);
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

    /* Free Virtual Assets */
    if (vm->assets) {
        for (int i = 0; i < vm->asset_count; i++) {
            free(vm->assets[i].path);
            free(vm->assets[i].data);
        }
        free(vm->assets);
        vm->assets = NULL;
        vm->asset_count = 0;
    }

    /* Close all loaded FFI library handles */
#ifndef VN_NO_FFI
    ffi_close_all_libs();
#endif

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

    /* Stage 1: free all Shapes in the shape registry */
    ShapeRegistry *sreg = &vm->shape_registry;
    for (int i = 0; i < sreg->count; i++) {
        Shape *sh = sreg->shapes[i];
        if (!sh) continue;
        free(sh->type_name);
        if (sh->field_names) {
            for (int j = 0; j < sh->field_count; j++)
                free(sh->field_names[j]);
            free(sh->field_names);
        }
        free(sh->name_hashes);
        free(sh);
    }
    sreg->count = 0;
}

const unsigned char *vm_lookup_asset(VM *vm, const char *path, int *out_size) {
    if (!vm || !vm->assets || !path) return NULL;

    // Normalize path by stripping leading "./" or "/"
    const char *norm = path;
    if (norm[0] == '.' && norm[1] == '/') norm += 2;
    else if (norm[0] == '/') norm += 1;

    for (int i = 0; i < vm->asset_count; i++) {
        const char *ap = vm->assets[i].path;
        if (ap[0] == '.' && ap[1] == '/') ap += 2;
        else if (ap[0] == '/') ap += 1;

        if (strcmp(ap, norm) == 0) {
            *out_size = vm->assets[i].size;
            return vm->assets[i].data;
        }
    }
    return NULL;
}
