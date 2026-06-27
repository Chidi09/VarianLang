#include "lib_env.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define setenv(key, val, overwrite) _putenv_s(key, val)
#endif

static int env_arg_base(int arg_count, Value *args) {
    return (arg_count >= 1 && args[0].type == VAL_MODULE) ? 1 : 0;
}

static Value lib_env_get(VM *vm, int arg_count, Value *args) {
    int base = env_arg_base(arg_count, args);
    if (arg_count < base + 1 || args[base].type != VAL_STRING) {
        return val_nil();
    }
    const char *key = args[base].as.string->chars;
    const char *val = getenv(key);
    
    if (val != NULL) {
        return val_string(copy_string(val, strlen(val)));
    }
    
    if (arg_count >= base + 2 && args[base + 1].type == VAL_STRING) {
        return val_string(copy_string(args[base + 1].as.string->chars, args[base + 1].as.string->length));
    }
    
    return val_nil();
}

static Value lib_env_require(VM *vm, int arg_count, Value *args) {
    int base = env_arg_base(arg_count, args);
    if (arg_count < base + 1 || args[base].type != VAL_STRING) {
        runtime_error(vm, "env.require expects a string argument");
        return val_nil();
    }
    const char *key = args[base].as.string->chars;
    const char *val = getenv(key);
    
    if (val == NULL) {
        runtime_error(vm, "Environment variable required but not set: %s", key);
        return val_nil();
    }
    
    return val_string(copy_string(val, strlen(val)));
}

static Value lib_env_load(VM *vm, int arg_count, Value *args) {
    int base = env_arg_base(arg_count, args);
    const char *path = ".env";
    if (arg_count >= base + 1 && args[base].type == VAL_STRING) {
        path = args[base].as.string->chars;
    }

    FILE *f = fopen(path, "r");
    if (!f) return val_bool(0); // false if no file

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = p;
        char *val = eq + 1;

        // Trim trailing newlines on val
        char *end = val + strlen(val) - 1;
        while (end >= val && (*end == '\n' || *end == '\r')) {
            *end = '\0';
            end--;
        }

        setenv(key, val, 1);
    }
    fclose(f);
    return val_bool(1);
}

void lib_env_init(VM *vm) {
    ObjModule *mod = new_module("env");
    mod->obj.next = vm->objects;
    vm->objects = (Obj *)mod;
    define_global(vm, copy_string("env", 3), val_module(mod));

    vm_register_dispatch(vm, "env", "get", val_native_fn((void *)lib_env_get));
    vm_register_dispatch(vm, "env", "require", val_native_fn((void *)lib_env_require));
    vm_register_dispatch(vm, "env", "load", val_native_fn((void *)lib_env_load));
}
