#include "lib_math.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>

/* ─── Helper: convert Value to double ─── */
static inline double val_to_double(Value v) {
    return (v.type == VAL_FLOAT) ? v.as.floating : (double)v.as.integer;
}

/* ─── Helper: convert Value to int64_t ─── */
static inline int64_t val_to_int64(Value v) {
    return (v.type == VAL_INT) ? v.as.integer : (int64_t)v.as.floating;
}

/* ─── Argument base offset (skip module self-reference) ─── */
static int math_arg_base(int arg_count, Value *args) {
    return (arg_count >= 1 && args[0].type == VAL_MODULE) ? 1 : 0;
}

/* ─── Basic trig ─── */

static Value lib_math_sin(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 1) return val_nil();
    return val_float(sin(val_to_double(args[base])));
}

static Value lib_math_cos(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 1) return val_nil();
    return val_float(cos(val_to_double(args[base])));
}

static Value lib_math_tan(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 1) return val_nil();
    return val_float(tan(val_to_double(args[base])));
}

static Value lib_math_asin(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 1) return val_nil();
    return val_float(asin(val_to_double(args[base])));
}

static Value lib_math_acos(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 1) return val_nil();
    return val_float(acos(val_to_double(args[base])));
}

static Value lib_math_atan(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 1) return val_nil();
    return val_float(atan(val_to_double(args[base])));
}

static Value lib_math_atan2(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 2) return val_nil();
    double y = val_to_double(args[base]);
    double x = val_to_double(args[base + 1]);
    return val_float(atan2(y, x));
}

static Value lib_math_sinh(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 1) return val_nil();
    return val_float(sinh(val_to_double(args[base])));
}

static Value lib_math_cosh(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 1) return val_nil();
    return val_float(cosh(val_to_double(args[base])));
}

static Value lib_math_tanh(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 1) return val_nil();
    return val_float(tanh(val_to_double(args[base])));
}

/* ─── Basic math ─── */

static Value lib_math_sqrt(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 1) return val_nil();
    return val_float(sqrt(val_to_double(args[base])));
}

static Value lib_math_abs(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 1) return val_nil();
    if (args[base].type == VAL_INT) {
        int64_t v = args[base].as.integer;
        return val_int(v < 0 ? -v : v);
    }
    double x = args[base].as.floating;
    return val_float(x < 0 ? -x : x);
}

static Value lib_math_floor(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 1) return val_nil();
    return val_float(floor(val_to_double(args[base])));
}

static Value lib_math_ceil(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 1) return val_nil();
    return val_float(ceil(val_to_double(args[base])));
}

static Value lib_math_round(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 1) return val_nil();
    return val_float(round(val_to_double(args[base])));
}

static Value lib_math_trunc(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 1) return val_nil();
    return val_float(trunc(val_to_double(args[base])));
}

static Value lib_math_sign(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 1) return val_nil();
    if (args[base].type == VAL_INT) {
        int64_t v = args[base].as.integer;
        return val_int((v > 0) - (v < 0));
    }
    double x = val_to_double(args[base]);
    return val_float((x > 0) - (x < 0));
}

/* ─── Power / exp / log ─── */

static Value lib_math_pow(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 2) return val_nil();
    return val_float(pow(val_to_double(args[base]), val_to_double(args[base + 1])));
}

static Value lib_math_exp(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 1) return val_nil();
    return val_float(exp(val_to_double(args[base])));
}

static Value lib_math_exp2(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 1) return val_nil();
    return val_float(exp2(val_to_double(args[base])));
}

static Value lib_math_log(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 1) return val_nil();
    return val_float(log(val_to_double(args[base])));
}

static Value lib_math_log2(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 1) return val_nil();
    return val_float(log2(val_to_double(args[base])));
}

static Value lib_math_log10(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 1) return val_nil();
    return val_float(log10(val_to_double(args[base])));
}

/* ─── Min / Max / Clamp / Lerp ─── */

static Value lib_math_min(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 2) return val_nil();
    return val_float(fmin(val_to_double(args[base]), val_to_double(args[base + 1])));
}

static Value lib_math_max(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 2) return val_nil();
    return val_float(fmax(val_to_double(args[base]), val_to_double(args[base + 1])));
}

static Value lib_math_clamp(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 3) return val_nil();
    double x = val_to_double(args[base]);
    double lo = val_to_double(args[base + 1]);
    double hi = val_to_double(args[base + 2]);
    return val_float(fmin(fmax(x, lo), hi));
}

static Value lib_math_lerp(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 3) return val_nil();
    double a = val_to_double(args[base]);
    double b = val_to_double(args[base + 1]);
    double t = val_to_double(args[base + 2]);
    return val_float(a + (b - a) * t);
}

/* ─── Angle conversion ─── */

static Value lib_math_deg_to_rad(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 1) return val_nil();
    return val_float(val_to_double(args[base]) * (M_PI / 180.0));
}

static Value lib_math_rad_to_deg(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 1) return val_nil();
    return val_float(val_to_double(args[base]) * (180.0 / M_PI));
}

/* ─── Bitwise operators ─── */

static Value lib_math_bit_and(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 2) return val_nil();
    return val_int(val_to_int64(args[base]) & val_to_int64(args[base + 1]));
}

static Value lib_math_bit_or(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 2) return val_nil();
    return val_int(val_to_int64(args[base]) | val_to_int64(args[base + 1]));
}

static Value lib_math_bit_xor(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 2) return val_nil();
    return val_int(val_to_int64(args[base]) ^ val_to_int64(args[base + 1]));
}

/* ─── Helpers for statistics ─── */

static int compare_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

static int extract_double_array(Value arg, double **out) {
    if (arg.type != VAL_ARRAY) return -1;
    ObjArray *arr = arg.as.array;
    int n = arr->count;
    double *data = (double *)malloc((size_t)n * sizeof(double));
    if (!data) return -1;
    for (int i = 0; i < n; i++)
        data[i] = val_to_double(arr->elements[i]);
    *out = data;
    return n;
}

/* ─── Statistics ─── */

static Value lib_math_sum(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 1) return val_nil();
    double *data;
    int n = extract_double_array(args[base], &data);
    if (n < 0) return val_nil();
    double sum = 0.0;
    for (int i = 0; i < n; i++) sum += data[i];
    free(data);
    return val_float(sum);
}

static Value lib_math_product(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 1) return val_nil();
    double *data;
    int n = extract_double_array(args[base], &data);
    if (n < 0) return val_nil();
    double prod = 1.0;
    for (int i = 0; i < n; i++) prod *= data[i];
    free(data);
    return val_float(prod);
}

static Value lib_math_mean(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 1) return val_nil();
    double *data;
    int n = extract_double_array(args[base], &data);
    if (n < 1) { free(data); return val_nil(); }
    double sum = 0.0;
    for (int i = 0; i < n; i++) sum += data[i];
    free(data);
    return val_float(sum / n);
}

static Value lib_math_variance(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 1) return val_nil();
    double *data;
    int n = extract_double_array(args[base], &data);
    if (n < 2) { free(data); return val_nil(); }
    double sum = 0.0;
    for (int i = 0; i < n; i++) sum += data[i];
    double mean = sum / n;
    double var = 0.0;
    for (int i = 0; i < n; i++) {
        double diff = data[i] - mean;
        var += diff * diff;
    }
    free(data);
    return val_float(var / n);
}

static Value lib_math_stddev(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 1) return val_nil();
    double *data;
    int n = extract_double_array(args[base], &data);
    if (n < 2) { free(data); return val_nil(); }
    double sum = 0.0;
    for (int i = 0; i < n; i++) sum += data[i];
    double mean = sum / n;
    double var = 0.0;
    for (int i = 0; i < n; i++) {
        double diff = data[i] - mean;
        var += diff * diff;
    }
    free(data);
    return val_float(sqrt(var / n));
}

static Value lib_math_median(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 1) return val_nil();
    double *data;
    int n = extract_double_array(args[base], &data);
    if (n < 1) { if (data) free(data); return val_nil(); }
    qsort(data, (size_t)n, sizeof(double), compare_double);
    double result;
    if (n % 2 == 0)
        result = (data[n / 2 - 1] + data[n / 2]) / 2.0;
    else
        result = data[n / 2];
    free(data);
    return val_float(result);
}

static Value lib_math_min_arr(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 1) return val_nil();
    double *data;
    int n = extract_double_array(args[base], &data);
    if (n < 1) { free(data); return val_nil(); }
    double m = data[0];
    for (int i = 1; i < n; i++)
        if (data[i] < m) m = data[i];
    free(data);
    return val_float(m);
}

static Value lib_math_max_arr(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 1) return val_nil();
    double *data;
    int n = extract_double_array(args[base], &data);
    if (n < 1) { free(data); return val_nil(); }
    double m = data[0];
    for (int i = 1; i < n; i++)
        if (data[i] > m) m = data[i];
    free(data);
    return val_float(m);
}

/* ─── Random ─── */

static Value lib_math_random(VM *vm, int arg_count, Value *args) {
    (void)vm;
    (void)arg_count;
    (void)args;
    return val_float((double)rand() / (double)RAND_MAX);
}

static Value lib_math_randint(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 2) return val_nil();
    int64_t lo = val_to_int64(args[base]);
    int64_t hi = val_to_int64(args[base + 1]);
    if (lo > hi) { int64_t t = lo; lo = hi; hi = t; }
    int64_t range = hi - lo + 1;
    int64_t r = lo + (int64_t)((double)rand() / ((double)RAND_MAX + 1.0) * range);
    return val_int(r);
}

static Value lib_math_randfloat(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 2) return val_nil();
    double lo = val_to_double(args[base]);
    double hi = val_to_double(args[base + 1]);
    double t = (double)rand() / (double)RAND_MAX;
    return val_float(lo + t * (hi - lo));
}

static Value lib_math_seed(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 1) return val_nil();
    srand((unsigned int)val_to_int64(args[base]));
    return val_nil();
}

static Value lib_math_shuffle(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = math_arg_base(arg_count, args);
    if (arg_count < base + 1) return val_nil();
    if (args[base].type != VAL_ARRAY) return val_nil();
    ObjArray *src = args[base].as.array;
    int n = src->count;

    ObjArray *dst = new_array();
    if (!dst) return val_nil();
    dst->obj.next = vm->objects;
    vm->objects = (Obj *)dst;

    /* Root the array on the task stack during construction */
    Task *self = vm->current_task;
    self->stack[self->stack_top++] = val_array(dst);

    /* Allocate and copy elements */
    if (n > 0) {
        dst->elements = (Value *)malloc((size_t)n * sizeof(Value));
        if (!dst->elements) { self->stack_top--; return val_nil(); }
        dst->capacity = n;
        for (int i = 0; i < n; i++)
            dst->elements[i] = src->elements[i];
        dst->count = n;

        /* Fisher-Yates shuffle (in-place on the new copy) */
        for (int i = n - 1; i > 0; i--) {
            int j = rand() % (i + 1);
            Value tmp = dst->elements[i];
            dst->elements[i] = dst->elements[j];
            dst->elements[j] = tmp;
        }
    }

    self->stack_top--;
    return val_array(dst);
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

    /* ─── Register dispatch entries for module methods ─── */

    /* Basic trig */
    vm_register_dispatch(vm, "math", "sin",     val_native_fn((void *)lib_math_sin));
    vm_register_dispatch(vm, "math", "cos",     val_native_fn((void *)lib_math_cos));
    vm_register_dispatch(vm, "math", "tan",     val_native_fn((void *)lib_math_tan));
    vm_register_dispatch(vm, "math", "asin",    val_native_fn((void *)lib_math_asin));
    vm_register_dispatch(vm, "math", "acos",    val_native_fn((void *)lib_math_acos));
    vm_register_dispatch(vm, "math", "atan",    val_native_fn((void *)lib_math_atan));
    vm_register_dispatch(vm, "math", "atan2",   val_native_fn((void *)lib_math_atan2));
    vm_register_dispatch(vm, "math", "sinh",    val_native_fn((void *)lib_math_sinh));
    vm_register_dispatch(vm, "math", "cosh",    val_native_fn((void *)lib_math_cosh));
    vm_register_dispatch(vm, "math", "tanh",    val_native_fn((void *)lib_math_tanh));

    /* Basic math */
    vm_register_dispatch(vm, "math", "sqrt",    val_native_fn((void *)lib_math_sqrt));
    vm_register_dispatch(vm, "math", "abs",     val_native_fn((void *)lib_math_abs));
    vm_register_dispatch(vm, "math", "floor",   val_native_fn((void *)lib_math_floor));
    vm_register_dispatch(vm, "math", "ceil",    val_native_fn((void *)lib_math_ceil));
    vm_register_dispatch(vm, "math", "round",   val_native_fn((void *)lib_math_round));
    vm_register_dispatch(vm, "math", "trunc",   val_native_fn((void *)lib_math_trunc));
    vm_register_dispatch(vm, "math", "sign",    val_native_fn((void *)lib_math_sign));

    /* Power / exp / log */
    vm_register_dispatch(vm, "math", "pow",     val_native_fn((void *)lib_math_pow));
    vm_register_dispatch(vm, "math", "exp",     val_native_fn((void *)lib_math_exp));
    vm_register_dispatch(vm, "math", "exp2",    val_native_fn((void *)lib_math_exp2));
    vm_register_dispatch(vm, "math", "log",     val_native_fn((void *)lib_math_log));
    vm_register_dispatch(vm, "math", "log2",    val_native_fn((void *)lib_math_log2));
    vm_register_dispatch(vm, "math", "log10",   val_native_fn((void *)lib_math_log10));

    /* Min / Max / Clamp / Lerp */
    vm_register_dispatch(vm, "math", "min",     val_native_fn((void *)lib_math_min));
    vm_register_dispatch(vm, "math", "max",     val_native_fn((void *)lib_math_max));
    vm_register_dispatch(vm, "math", "clamp",   val_native_fn((void *)lib_math_clamp));
    vm_register_dispatch(vm, "math", "lerp",    val_native_fn((void *)lib_math_lerp));

    /* Angle conversion */
    vm_register_dispatch(vm, "math", "deg_to_rad", val_native_fn((void *)lib_math_deg_to_rad));
    vm_register_dispatch(vm, "math", "rad_to_deg", val_native_fn((void *)lib_math_rad_to_deg));

    /* Bitwise */
    vm_register_dispatch(vm, "math", "bit_and", val_native_fn((void *)lib_math_bit_and));
    vm_register_dispatch(vm, "math", "bit_or",  val_native_fn((void *)lib_math_bit_or));
    vm_register_dispatch(vm, "math", "bit_xor", val_native_fn((void *)lib_math_bit_xor));

    /* Statistics */
    vm_register_dispatch(vm, "math", "sum",     val_native_fn((void *)lib_math_sum));
    vm_register_dispatch(vm, "math", "product", val_native_fn((void *)lib_math_product));
    vm_register_dispatch(vm, "math", "mean",    val_native_fn((void *)lib_math_mean));
    vm_register_dispatch(vm, "math", "median",  val_native_fn((void *)lib_math_median));
    vm_register_dispatch(vm, "math", "stddev",  val_native_fn((void *)lib_math_stddev));
    vm_register_dispatch(vm, "math", "variance", val_native_fn((void *)lib_math_variance));
    vm_register_dispatch(vm, "math", "min_arr", val_native_fn((void *)lib_math_min_arr));
    vm_register_dispatch(vm, "math", "max_arr", val_native_fn((void *)lib_math_max_arr));

    /* Random */
    vm_register_dispatch(vm, "math", "random",    val_native_fn((void *)lib_math_random));
    vm_register_dispatch(vm, "math", "randint",   val_native_fn((void *)lib_math_randint));
    vm_register_dispatch(vm, "math", "randfloat", val_native_fn((void *)lib_math_randfloat));
    vm_register_dispatch(vm, "math", "seed",      val_native_fn((void *)lib_math_seed));
    vm_register_dispatch(vm, "math", "shuffle",   val_native_fn((void *)lib_math_shuffle));

    /* ─── Constants ─── */
    vm_register_dispatch(vm, "math", "pi",      val_float(M_PI));
    vm_register_dispatch(vm, "math", "e",       val_float(M_E));
    vm_register_dispatch(vm, "math", "tau",     val_float(2.0 * M_PI));
    vm_register_dispatch(vm, "math", "inf",     val_float(INFINITY));
    vm_register_dispatch(vm, "math", "nan",     val_float(NAN));
    vm_register_dispatch(vm, "math", "epsilon", val_float(2.220446049250313e-16));
}
