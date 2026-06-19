#include "lib_mock.h"
#include <string.h>

static Value lib_mock_intercept(VM *vm, int arg_count, Value *args) {
    if (arg_count < 3 || args[0].type != VAL_STRING || args[1].type != VAL_STRING) {
        runtime_error(vm, "mock.intercept() requires type_name, method_name, and fake_fn");
        return val_nil();
    }
    const char *type_name = args[0].as.string->chars;
    const char *method_name = args[1].as.string->chars;
    Value fake_fn = args[2];

    Value *existing = vm_find_dispatch(vm, type_name, method_name);
    Value old_val = existing ? *existing : val_nil();

    vm_register_dispatch(vm, type_name, method_name, fake_fn);

    return old_val;
}

static Value lib_mock_restore(VM *vm, int arg_count, Value *args) {
    if (arg_count < 3 || args[0].type != VAL_STRING || args[1].type != VAL_STRING) {
        runtime_error(vm, "mock.restore() requires type_name, method_name, and saved_value");
        return val_nil();
    }
    const char *type_name = args[0].as.string->chars;
    const char *method_name = args[1].as.string->chars;
    Value saved_value = args[2];

    vm_register_dispatch(vm, type_name, method_name, saved_value);

    return val_nil();
}

void lib_mock_init(VM *vm) {
    ObjModule *mod = new_module("mock");
    mod->obj.next = vm->objects;
    vm->objects = (Obj *)mod;
    define_global(vm, copy_string("mock", 4), val_module(mod));
    vm_register_dispatch(vm, "mock", "intercept", val_native_fn((void *)lib_mock_intercept));
    vm_register_dispatch(vm, "mock", "restore",   val_native_fn((void *)lib_mock_restore));
}
