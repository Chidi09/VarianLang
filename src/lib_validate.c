#include "lib_validate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

static Value lib_validate_is_email(VM *vm, int arg_count, Value *args) {
    (void)vm;
    if (arg_count < 1 || args[0].type != VAL_STRING)
        return val_bool(false);
    const char *s = args[0].as.string->chars;
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
    if (arg_count < 1 || args[0].type != VAL_STRING)
        return val_bool(false);
    const char *s = args[0].as.string->chars;
    if (strncmp(s, "http://", 7) == 0 && s[7] != '\0')
        return val_bool(true);
    if (strncmp(s, "https://", 8) == 0 && s[8] != '\0')
        return val_bool(true);
    return val_bool(false);
}

static Value lib_validate_is_alphanumeric(VM *vm, int arg_count, Value *args) {
    (void)vm;
    if (arg_count < 1 || args[0].type != VAL_STRING)
        return val_bool(false);
    const char *s = args[0].as.string->chars;
    if (*s == '\0') return val_bool(false);
    for (; *s; s++) {
        if (!isalnum((unsigned char)*s))
            return val_bool(false);
    }
    return val_bool(true);
}

static Value lib_validate_min_len(VM *vm, int arg_count, Value *args) {
    (void)vm;
    if (arg_count < 2 || args[0].type != VAL_STRING || args[1].type != VAL_INT)
        return val_bool(false);
    int len = args[0].as.string->length;
    int64_t min = args[1].as.integer;
    return val_bool(len >= min);
}

static Value lib_validate_max_len(VM *vm, int arg_count, Value *args) {
    (void)vm;
    if (arg_count < 2 || args[0].type != VAL_STRING || args[1].type != VAL_INT)
        return val_bool(false);
    int len = args[0].as.string->length;
    int64_t max = args[1].as.integer;
    return val_bool(len <= max);
}

static Value lib_validate_is_uuid(VM *vm, int arg_count, Value *args) {
    (void)vm;
    if (arg_count < 1 || args[0].type != VAL_STRING)
        return val_bool(false);
    const char *s = args[0].as.string->chars;
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

void lib_validate_init(VM *vm) {
    ObjModule *mod = new_module("validate");
    mod->obj.next = vm->objects;
    vm->objects = (Obj *)mod;
    define_global(vm, copy_string("validate", 8), val_module(mod));
    vm_register_dispatch(vm, "validate", "is_email",        val_native_fn((void *)lib_validate_is_email));
    vm_register_dispatch(vm, "validate", "is_url",          val_native_fn((void *)lib_validate_is_url));
    vm_register_dispatch(vm, "validate", "is_alphanumeric", val_native_fn((void *)lib_validate_is_alphanumeric));
    vm_register_dispatch(vm, "validate", "min_len",         val_native_fn((void *)lib_validate_min_len));
    vm_register_dispatch(vm, "validate", "max_len",         val_native_fn((void *)lib_validate_max_len));
    vm_register_dispatch(vm, "validate", "is_uuid",         val_native_fn((void *)lib_validate_is_uuid));
}
