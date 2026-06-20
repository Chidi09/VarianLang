#include "lib_string.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static Value lib_string_len(VM *vm, int arg_count, Value *args) {
    (void)vm;
    if (arg_count < 1 || args[0].type != VAL_STRING)
        return val_int(0);
    return val_int(args[0].as.string->length);
}

static Value lib_string_upper(VM *vm, int arg_count, Value *args) {
    if (arg_count < 1 || args[0].type != VAL_STRING)
        return val_nil();

    ObjString *s = args[0].as.string;
    char *buf = (char *)malloc((size_t)s->length + 1);
    if (!buf) return val_nil();

    for (int i = 0; i < s->length; i++)
        buf[i] = (char)toupper((unsigned char)s->chars[i]);
    buf[s->length] = '\0';

    ObjString *result = allocate_string(vm, buf, s->length);
    free(buf);
    return val_string(result);
}

static Value lib_string_lower(VM *vm, int arg_count, Value *args) {
    if (arg_count < 1 || args[0].type != VAL_STRING)
        return val_nil();

    ObjString *s = args[0].as.string;
    char *buf = (char *)malloc((size_t)s->length + 1);
    if (!buf) return val_nil();

    for (int i = 0; i < s->length; i++)
        buf[i] = (char)tolower((unsigned char)s->chars[i]);
    buf[s->length] = '\0';

    ObjString *result = allocate_string(vm, buf, s->length);
    free(buf);
    return val_string(result);
}

static Value lib_string_substring(VM *vm, int arg_count, Value *args) {
    if (arg_count < 3 || args[0].type != VAL_STRING ||
        args[1].type != VAL_INT || args[2].type != VAL_INT)
        return val_nil();

    ObjString *s = args[0].as.string;
    int start = (int)args[1].as.integer;
    int end = (int)args[2].as.integer;

    if (start < 0) start = 0;
    if (end > s->length) end = s->length;
    if (start >= end) return val_string(copy_string("", 0));

    int len = end - start;
    ObjString *result = allocate_string(vm, s->chars + start, len);
    return val_string(result);
}

static Value lib_string_trim(VM *vm, int arg_count, Value *args) {
    if (arg_count < 1 || args[0].type != VAL_STRING)
        return val_nil();

    ObjString *s = args[0].as.string;
    const char *start = s->chars;
    const char *end = s->chars + s->length - 1;

    while (start <= end && (unsigned char)*start <= 32) start++;
    while (end >= start && (unsigned char)*end <= 32) end--;

    int len = (int)(end - start + 1);
    if (len <= 0) return val_string(copy_string("", 0));

    ObjString *result = allocate_string(vm, start, len);
    return val_string(result);
}

static Value lib_array_len(VM *vm, int arg_count, Value *args) {
    (void)vm;
    if (arg_count < 1 || args[0].type != VAL_ARRAY)
        return val_int(0);
    return val_int(args[0].as.array->count);
}

static Value lib_array_push(VM *vm, int arg_count, Value *args) {
    (void)vm;
    if (arg_count < 2 || args[0].type != VAL_ARRAY)
        return val_nil();

    ObjArray *src = args[0].as.array;
    int new_count = src->count + 1;
    ObjArray *dst = new_array();
    if (!dst) return val_nil();
    dst->obj.next = vm->objects;
    vm->objects = (Obj *)dst;
    dst->count = new_count;
    dst->capacity = new_count;
    dst->elements = (Value *)calloc(new_count, sizeof(Value));
    if (!dst->elements) return val_nil();
    for (int i = 0; i < src->count; i++)
        dst->elements[i] = src->elements[i];
    dst->elements[src->count] = args[1];
    return val_array(dst);
}

/* ─── string.split(delimiter) returns array of strings ─── */
static Value lib_string_split(VM *vm, int arg_count, Value *args) {
    if (arg_count < 2 || args[0].type != VAL_STRING || args[1].type != VAL_STRING)
        return val_nil();
    ObjString *s = args[0].as.string;
    const char *delim = args[1].as.string->chars;
    int delim_len = args[1].as.string->length;
    ObjArray *arr = new_array();
    arr->obj.next = vm->objects;
    vm->objects = (Obj *)arr;
    /* arr isn't reachable from any GC root until returned -- allocate_string
     * per segment below can trigger a GC cycle that would otherwise free
     * arr (and segments already split off) mid-loop. This is an extremely
     * hot path (e.g. every request's path gets string.split("/")'d by
     * Zenith's router), so it's an easy one to hit under real load. */
    Task *self = vm->current_task;
    self->stack[self->stack_top++] = val_array(arr);
    int start = 0;
    while (start <= s->length) {
        int end = start;
        if (delim_len > 0) {
            const char *found = strstr(s->chars + start, delim);
            end = found ? (int)(found - s->chars) : s->length;
        } else {
            end = s->length;
        }
        int seg_len = end - start;
        if (arr->count >= arr->capacity) {
            int new_cap = arr->capacity ? arr->capacity * 2 : 8;
            arr->elements = (Value *)realloc(arr->elements, (size_t)new_cap * sizeof(Value));
            arr->capacity = new_cap;
        }
        arr->elements[arr->count++] = val_string(allocate_string(vm, s->chars + start, seg_len));
        start = end + delim_len;
        if (delim_len == 0) break;
    }
    self->stack_top--;
    return val_array(arr);
}

/* ─── string.starts_with(prefix) returns bool ─── */
static Value lib_string_starts_with(VM *vm, int arg_count, Value *args) {
    (void)vm;
    if (arg_count < 2 || args[0].type != VAL_STRING || args[1].type != VAL_STRING)
        return val_bool(false);
    ObjString *s = args[0].as.string;
    ObjString *prefix = args[1].as.string;
    if (prefix->length > s->length) return val_bool(false);
    return val_bool(strncmp(s->chars, prefix->chars, (size_t)prefix->length) == 0);
}

/* ─── string.replace(old, new) returns new string ─── */
static Value lib_string_replace(VM *vm, int arg_count, Value *args) {
    if (arg_count < 3 || args[0].type != VAL_STRING ||
        args[1].type != VAL_STRING || args[2].type != VAL_STRING)
        return val_nil();
    ObjString *s = args[0].as.string;
    const char *old_str = args[1].as.string->chars;
    int old_len = args[1].as.string->length;
    const char *new_str = args[2].as.string->chars;
    int new_len = args[2].as.string->length;
    if (old_len == 0) return args[0];
    int count = 0;
    const char *pos = s->chars;
    while ((pos = strstr(pos, old_str)) != NULL) { count++; pos += old_len; }
    if (count == 0) return args[0];
    int result_len = s->length + count * (new_len - old_len);
    char *buf = (char *)malloc((size_t)result_len + 1);
    if (!buf) return val_nil();
    int bi = 0;
    const char *src = s->chars;
    while (*src) {
        if (strncmp(src, old_str, (size_t)old_len) == 0) {
            memcpy(buf + bi, new_str, (size_t)new_len);
            bi += new_len;
            src += old_len;
        } else {
            buf[bi++] = *src++;
        }
    }
    buf[bi] = '\0';
    ObjString *result = allocate_string(vm, buf, result_len);
    free(buf);
    return val_string(result);
}

static Value lib_string_code_at(VM *vm, int arg_count, Value *args) {
    (void)vm;
    if (arg_count < 2 || args[0].type != VAL_STRING || args[1].type != VAL_INT)
        return val_nil();
    ObjString *s = args[0].as.string;
    int idx = (int)args[1].as.integer;
    if (idx < 0 || idx >= s->length) return val_nil();
    return val_int((unsigned char)s->chars[idx]);
}

static Value lib_string_from_codes(VM *vm, int arg_count, Value *args) {
    if (arg_count < 2 || args[0].type != VAL_STRING || args[1].type != VAL_ARRAY)
        return val_nil();
    ObjArray *arr = args[1].as.array;
    char *buf = (char *)malloc((size_t)arr->count + 1);
    if (!buf) return val_nil();
    for (int i = 0; i < arr->count; i++) {
        if (arr->elements[i].type == VAL_INT) {
            buf[i] = (char)(unsigned char)arr->elements[i].as.integer;
        } else {
            buf[i] = '\0';
        }
    }
    buf[arr->count] = '\0';
    ObjString *result = allocate_string(vm, buf, arr->count);
    free(buf);
    return val_string(result);
}

void lib_string_init(VM *vm) {
    vm_register_dispatch(vm, "string", "len",          val_native_fn((void *)lib_string_len));
    vm_register_dispatch(vm, "string", "upper",        val_native_fn((void *)lib_string_upper));
    vm_register_dispatch(vm, "string", "lower",        val_native_fn((void *)lib_string_lower));
    vm_register_dispatch(vm, "string", "substring",    val_native_fn((void *)lib_string_substring));
    vm_register_dispatch(vm, "string", "trim",         val_native_fn((void *)lib_string_trim));
    vm_register_dispatch(vm, "string", "split",        val_native_fn((void *)lib_string_split));
    vm_register_dispatch(vm, "string", "starts_with",  val_native_fn((void *)lib_string_starts_with));
    vm_register_dispatch(vm, "string", "replace",      val_native_fn((void *)lib_string_replace));
    vm_register_dispatch(vm, "string", "code_at",      val_native_fn((void *)lib_string_code_at));
    vm_register_dispatch(vm, "string", "from_codes",   val_native_fn((void *)lib_string_from_codes));

    vm_register_dispatch(vm, "array", "len",  val_native_fn((void *)lib_array_len));
    vm_register_dispatch(vm, "array", "push", val_native_fn((void *)lib_array_push));
}
