#include "lib_sanitize.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Skip leading module arg when called via dispatch (.trim(str) pattern) */
static int str_arg_idx(int arg_count, Value *args) {
    if (arg_count >= 2 && args[0].type == VAL_MODULE)
        return 1;
    return 0;
}

static Value lib_sanitize_strip_html(VM *vm, int arg_count, Value *args) {
    int idx = str_arg_idx(arg_count, args);
    if (arg_count <= idx || args[idx].type != VAL_STRING)
        return val_string(copy_string("", 0));
    const char *s = args[idx].as.string->chars;
    int len = args[idx].as.string->length;
    char *buf = (char *)malloc(len + 1);
    if (!buf) return val_string(copy_string("", 0));
    int j = 0;
    int in_tag = 0;
    for (int i = 0; i < len; i++) {
        if (s[i] == '<') {
            in_tag = 1;
        } else if (s[i] == '>') {
            in_tag = 0;
        } else if (!in_tag) {
            buf[j++] = s[i];
        }
    }
    buf[j] = '\0';
    ObjString *result = allocate_string(vm, buf, j);
    free(buf);
    return val_string(result);
}

static Value lib_sanitize_escape_html(VM *vm, int arg_count, Value *args) {
    int idx = str_arg_idx(arg_count, args);
    if (arg_count <= idx || args[idx].type != VAL_STRING)
        return val_string(copy_string("", 0));
    const char *s = args[idx].as.string->chars;
    int len = args[idx].as.string->length;
    int cap = len * 6 + 1;
    char *buf = (char *)malloc(cap);
    if (!buf) return val_string(copy_string("", 0));
    int j = 0;
    for (int i = 0; i < len; i++) {
        switch (s[i]) {
            case '&':  j += snprintf(buf + j, cap - j, "&amp;"); break;
            case '<':  j += snprintf(buf + j, cap - j, "&lt;"); break;
            case '>':  j += snprintf(buf + j, cap - j, "&gt;"); break;
            case '"':  j += snprintf(buf + j, cap - j, "&quot;"); break;
            case '\'': j += snprintf(buf + j, cap - j, "&#39;"); break;
            default:   buf[j++] = s[i]; break;
        }
    }
    buf[j] = '\0';
    ObjString *result = allocate_string(vm, buf, j);
    free(buf);
    return val_string(result);
}

static Value lib_sanitize_trim(VM *vm, int arg_count, Value *args) {
    int idx = str_arg_idx(arg_count, args);
    if (arg_count <= idx || args[idx].type != VAL_STRING)
        return val_string(copy_string("", 0));
    const char *s = args[idx].as.string->chars;
    int len = args[idx].as.string->length;
    const char *start = s;
    const char *end = s + len - 1;
    while (start <= end && (unsigned char)*start <= 32) start++;
    while (end >= start && (unsigned char)*end <= 32) end--;
    int new_len = (int)(end - start + 1);
    if (new_len <= 0)
        return val_string(copy_string("", 0));
    ObjString *result = allocate_string(vm, start, new_len);
    return val_string(result);
}

void lib_sanitize_init(VM *vm) {
    ObjModule *mod = new_module("sanitize");
    mod->obj.next = vm->objects;
    vm->objects = (Obj *)mod;
    define_global(vm, copy_string("sanitize", 8), val_module(mod));
    vm_register_dispatch(vm, "sanitize", "strip_html",  val_native_fn((void *)lib_sanitize_strip_html));
    vm_register_dispatch(vm, "sanitize", "escape_html", val_native_fn((void *)lib_sanitize_escape_html));
    vm_register_dispatch(vm, "sanitize", "trim",         val_native_fn((void *)lib_sanitize_trim));
}
