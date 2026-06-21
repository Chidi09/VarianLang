#include "lib_validate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

static int get_arg_base(int arg_count, Value *args) {
    if (arg_count >= 2 && args[0].type == VAL_MODULE)
        return 1;
    return 0;
}

static Value lib_validate_is_email(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = get_arg_base(arg_count, args);
    if (arg_count < base + 1 || args[base].type != VAL_STRING)
        return val_bool(false);
    const char *s = args[base].as.string->chars;
    const char *at = strchr(s, '@');
    if (!at || at == s || at[1] == '\0')
        return val_bool(false);
    const char *dot = strchr(at + 1, '.');
    if (!dot || dot == at + 1 || dot[1] == '\0')
        return val_bool(false);
    for (const char *p = s; *p; p++) {
        if (*p == ' ') return val_bool(false);
    }
    return val_bool(true);
}

static Value lib_validate_is_url(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = get_arg_base(arg_count, args);
    if (arg_count < base + 1 || args[base].type != VAL_STRING)
        return val_bool(false);
    const char *s = args[base].as.string->chars;
    if (strncmp(s, "http://", 7) == 0 && s[7] != '\0')
        return val_bool(true);
    if (strncmp(s, "https://", 8) == 0 && s[8] != '\0')
        return val_bool(true);
    return val_bool(false);
}

static Value lib_validate_is_alphanumeric(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = get_arg_base(arg_count, args);
    if (arg_count < base + 1 || args[base].type != VAL_STRING)
        return val_bool(false);
    const char *s = args[base].as.string->chars;
    if (*s == '\0') return val_bool(false);
    for (; *s; s++) {
        if (!isalnum((unsigned char)*s))
            return val_bool(false);
    }
    return val_bool(true);
}

static Value lib_validate_min_len(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = get_arg_base(arg_count, args);
    if (arg_count < base + 2 || args[base].type != VAL_STRING || args[base + 1].type != VAL_INT)
        return val_bool(false);
    int len = args[base].as.string->length;
    int64_t min = args[base + 1].as.integer;
    return val_bool(len >= min);
}

static Value lib_validate_max_len(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = get_arg_base(arg_count, args);
    if (arg_count < base + 2 || args[base].type != VAL_STRING || args[base + 1].type != VAL_INT)
        return val_bool(false);
    int len = args[base].as.string->length;
    int64_t max = args[base + 1].as.integer;
    return val_bool(len <= max);
}

static Value lib_validate_is_uuid(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = get_arg_base(arg_count, args);
    if (arg_count < base + 1 || args[base].type != VAL_STRING)
        return val_bool(false);
    const char *s = args[base].as.string->chars;
    int groups[5] = {8, 4, 4, 4, 12};
    for (int g = 0; g < 5; g++) {
        if (g > 0) {
            if (*s != '-') return val_bool(false);
            s++;
        }
        for (int i = 0; i < groups[g]; i++) {
            if (!*s) return val_bool(false);
            char c = *s++;
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                return val_bool(false);
        }
    }
    return val_bool(*s == '\0');
}


static Value lib_validate_get_field(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = get_arg_base(arg_count, args);
    if (arg_count < base + 2 || args[base].type != VAL_STRUCT || args[base + 1].type != VAL_STRING)
        return val_nil();
    ObjStruct *s = args[base].as.structure;
    const char *name = args[base + 1].as.string->chars;
    for (int i = 0; i < s->field_count; i++) {
        if (strcmp(s->field_names[i], name) == 0) {
            return s->fields[i];
        }
    }
    return val_nil();
}

static Value lib_validate_get_keys(VM *vm, int arg_count, Value *args) {
    int base = get_arg_base(arg_count, args);
    if (arg_count < base + 1 || args[base].type != VAL_STRUCT)
        return val_nil();
    ObjStruct *s = args[base].as.structure;
    ObjArray *arr = new_array();
    arr->obj.next = vm->objects;
    vm->objects = (Obj *)arr;
    /* arr isn't reachable from any GC root (it's not on a task's stack yet,
     * and isn't returned until this loop finishes) -- allocate_string()
     * below can trigger a GC cycle, which would consider arr unreachable
     * garbage and free it out from under this very loop. Push it onto the
     * current task's stack as a temporary root while building it, pop
     * before returning (the array is about to be pushed right back as this
     * native call's result anyway). */
    Task *self = vm->current_task;
    self->stack[self->stack_top++] = val_array(arr);
    for (int i = 0; i < s->field_count; i++) {
        if (arr->count >= arr->capacity) {
            int new_cap = arr->capacity ? arr->capacity * 2 : 8;
            arr->elements = (Value *)realloc(arr->elements, (size_t)new_cap * sizeof(Value));
            arr->capacity = new_cap;
        }
        arr->elements[arr->count++] = val_string(allocate_string(vm, s->field_names[i], (int)strlen(s->field_names[i])));
    }
    self->stack_top--;
    return val_array(arr);
}

/* ─── M5: universal struct methods (state ergonomics) ───
 * Registered under the "struct" namespace; reachable on any struct via the
 * BC_DISPATCH fallback. These let component state read like a map without the
 * _tpl_bind boilerplate:  state = state.set("count", state.count + 1).
 * set() is IMMUTABLE — it returns a NEW struct (works for existing or new
 * keys) — which is required because structs may be arena-backed and cannot
 * grow in place. The receiver self is args[0]. */
static Value lib_struct_set(VM *vm, int arg_count, Value *args) {
    if (arg_count < 3 || args[0].type != VAL_STRUCT || args[1].type != VAL_STRING) {
        runtime_error(vm, "struct.set(key, value) requires a struct and a string key");
        return val_nil();
    }
    ObjStruct *src = args[0].as.structure;
    const char *key = args[1].as.string->chars;
    int idx = -1;
    for (int i = 0; i < src->field_count; i++)
        if (strcmp(src->field_names[i], key) == 0) { idx = i; break; }
    int n = src->field_count + (idx < 0 ? 1 : 0);
    ObjStruct *s = new_struct(vm, n, false);
    s->type_name = src->type_name ? strdup(src->type_name) : NULL;
    for (int i = 0; i < src->field_count; i++) {
        s->field_names[i] = strdup(src->field_names[i]);
        s->fields[i] = src->fields[i];
    }
    if (idx >= 0) {
        s->fields[idx] = args[2];
    } else {
        s->field_names[src->field_count] = strdup(key);
        s->fields[src->field_count] = args[2];
    }
    return val_struct(s);
}

static Value lib_struct_get(VM *vm, int arg_count, Value *args) {
    (void)vm;
    if (arg_count < 2 || args[0].type != VAL_STRUCT || args[1].type != VAL_STRING)
        return val_nil();
    ObjStruct *s = args[0].as.structure;
    const char *key = args[1].as.string->chars;
    for (int i = 0; i < s->field_count; i++)
        if (strcmp(s->field_names[i], key) == 0) return s->fields[i];
    return val_nil();
}

static Value lib_struct_has(VM *vm, int arg_count, Value *args) {
    (void)vm;
    if (arg_count < 2 || args[0].type != VAL_STRUCT || args[1].type != VAL_STRING)
        return val_bool(false);
    ObjStruct *s = args[0].as.structure;
    const char *key = args[1].as.string->chars;
    for (int i = 0; i < s->field_count; i++)
        if (strcmp(s->field_names[i], key) == 0) return val_bool(true);
    return val_bool(false);
}

static Value lib_struct_keys(VM *vm, int arg_count, Value *args) {
    if (arg_count < 1 || args[0].type != VAL_STRUCT) return val_nil();
    ObjStruct *s = args[0].as.structure;
    ObjArray *arr = new_array();
    arr->obj.next = vm->objects;
    vm->objects = (Obj *)arr;
    Task *self = vm->current_task;
    self->stack[self->stack_top++] = val_array(arr);  /* temp GC root */
    for (int i = 0; i < s->field_count; i++) {
        if (arr->count >= arr->capacity) {
            int new_cap = arr->capacity ? arr->capacity * 2 : 8;
            arr->elements = (Value *)realloc(arr->elements, (size_t)new_cap * sizeof(Value));
            arr->capacity = new_cap;
        }
        arr->elements[arr->count++] = val_string(allocate_string(vm, s->field_names[i], (int)strlen(s->field_names[i])));
    }
    self->stack_top--;
    return val_array(arr);
}

void lib_validate_init(VM *vm) {
    ObjModule *mod = new_module("_validate");
    mod->obj.next = vm->objects;
    vm->objects = (Obj *)mod;
    define_global(vm, copy_string("_validate", 9), val_module(mod));
    vm_register_dispatch(vm, "_validate", "is_email",        val_native_fn((void *)lib_validate_is_email));
    vm_register_dispatch(vm, "_validate", "is_url",          val_native_fn((void *)lib_validate_is_url));
    vm_register_dispatch(vm, "_validate", "is_alphanumeric", val_native_fn((void *)lib_validate_is_alphanumeric));
    vm_register_dispatch(vm, "_validate", "min_len",         val_native_fn((void *)lib_validate_min_len));
    vm_register_dispatch(vm, "_validate", "max_len",         val_native_fn((void *)lib_validate_max_len));
    vm_register_dispatch(vm, "_validate", "is_uuid",         val_native_fn((void *)lib_validate_is_uuid));
    vm_register_dispatch(vm, "_validate", "get_field",       val_native_fn((void *)lib_validate_get_field));
    vm_register_dispatch(vm, "_validate", "get_keys",        val_native_fn((void *)lib_validate_get_keys));

    /* M5: universal struct methods, reachable on any struct value. */
    vm_register_dispatch(vm, "struct", "set",  val_native_fn((void *)lib_struct_set));
    vm_register_dispatch(vm, "struct", "get",  val_native_fn((void *)lib_struct_get));
    vm_register_dispatch(vm, "struct", "has",  val_native_fn((void *)lib_struct_has));
    vm_register_dispatch(vm, "struct", "keys", val_native_fn((void *)lib_struct_keys));
}
