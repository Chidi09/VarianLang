#include "lib_math.h"
#include <math.h>
#include <stdio.h>

/* ─── Native math functions ─── */

static Value lib_math_sin(VM *vm, int arg_count, Value *args) {
    (void)vm;
    if (arg_count < 1) return val_nil();
    double x = (args[0].type == VAL_FLOAT) ? args[0].as.floating : (double)args[0].as.integer;
    return val_float(sin(x));
}

static Value lib_math_cos(VM *vm, int arg_count, Value *args) {
    (void)vm;
    if (arg_count < 1) return val_nil();
    double x = (args[0].type == VAL_FLOAT) ? args[0].as.floating : (double)args[0].as.integer;
    return val_float(cos(x));
}

static Value lib_math_sqrt(VM *vm, int arg_count, Value *args) {
    (void)vm;
    if (arg_count < 1) return val_nil();
    double x = (args[0].type == VAL_FLOAT) ? args[0].as.floating : (double)args[0].as.integer;
    return val_float(sqrt(x));
}

static Value lib_math_abs(VM *vm, int arg_count, Value *args) {
    (void)vm;
    if (arg_count < 1) return val_nil();
    if (args[0].type == VAL_INT) {
        int64_t v = args[0].as.integer;
        return val_int(v < 0 ? -v : v);
    }
    double x = args[0].as.floating;
    return val_float(x < 0 ? -x : x);
}

static Value lib_math_floor(VM *vm, int arg_count, Value *args) {
    (void)vm;
    if (arg_count < 1) return val_nil();
    double x = (args[0].type == VAL_FLOAT) ? args[0].as.floating : (double)args[0].as.integer;
    return val_float(floor(x));
}

static Value lib_math_ceil(VM *vm, int arg_count, Value *args) {
    (void)vm;
    if (arg_count < 1) return val_nil();
    double x = (args[0].type == VAL_FLOAT) ? args[0].as.floating : (double)args[0].as.integer;
    return val_float(ceil(x));
}

static int math_arg_base(int arg_count, Value *args) {
    return (arg_count >= 1 && args[0].type == VAL_MODULE) ? 1 : 0;
}

static Value lib_math_bit_and(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 2) return val_nil();
    int64_t a = (args[base].type == VAL_INT) ? args[base].as.integer : (int64_t)args[base].as.floating;
    int64_t b = (args[base + 1].type == VAL_INT) ? args[base + 1].as.integer : (int64_t)args[base + 1].as.floating;
    return val_int(a & b);
}

static Value lib_math_bit_or(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 2) return val_nil();
    int64_t a = (args[base].type == VAL_INT) ? args[base].as.integer : (int64_t)args[base].as.floating;
    int64_t b = (args[base + 1].type == VAL_INT) ? args[base + 1].as.integer : (int64_t)args[base + 1].as.floating;
    return val_int(a | b);
}

static Value lib_math_bit_xor(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 2) return val_nil();
    int64_t a = (args[base].type == VAL_INT) ? args[base].as.integer : (int64_t)args[base].as.floating;
    int64_t b = (args[base + 1].type == VAL_INT) ? args[base + 1].as.integer : (int64_t)args[base + 1].as.floating;
    return val_int(a ^ b);
}

/* ─── Registration ─── */

void lib_math_init(VM *vm) {
    /* Create the "math" module object */
    ObjModule *mod = new_module("math");

    /* Link into GC before registering globals */
    mod->obj.next = vm->objects;
    vm->objects = (Obj *)mod;

    /* Register module as global */
    define_global(vm, copy_string("math", 4), val_module(mod));

    /* Register dispatch entries for module methods */
    vm_register_dispatch(vm, "math", "sin",   val_native_fn((void *)lib_math_sin));
    vm_register_dispatch(vm, "math", "cos",   val_native_fn((void *)lib_math_cos));
    vm_register_dispatch(vm, "math", "sqrt",  val_native_fn((void *)lib_math_sqrt));
    vm_register_dispatch(vm, "math", "abs",   val_native_fn((void *)lib_math_abs));
    vm_register_dispatch(vm, "math", "floor", val_native_fn((void *)lib_math_floor));
    vm_register_dispatch(vm, "math", "ceil",  val_native_fn((void *)lib_math_ceil));
    vm_register_dispatch(vm, "math", "bit_and", val_native_fn((void *)lib_math_bit_and));
    vm_register_dispatch(vm, "math", "bit_or",  val_native_fn((void *)lib_math_bit_or));
    vm_register_dispatch(vm, "math", "bit_xor", val_native_fn((void *)lib_math_bit_xor));
}
