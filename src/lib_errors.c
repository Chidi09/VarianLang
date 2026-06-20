#include "lib_errors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int get_arg_base(int argc, Value *args) {
    if (argc >= 2 && args[0].type == VAL_MODULE) return 1;
    return 0;
}

/* raw-message substring -> (friendly kind, actionable hint) */
typedef struct { const char *needle; const char *kind; const char *hint; } ErrRule;
static const ErrRule RULES[] = {
    {"Undefined variable",                 "UndefinedName",     "Declare it first with `let name = ...`, or check the spelling/scope."},
    {"Struct has no field",                "NoSuchField",       "That field isn't on this struct. Check the name, or use the right struct type."},
    {"has no member",                      "NoSuchMember",      "That module doesn't export that name. See docs/STDLIB.md for what it provides."},
    {"No method",                          "NoSuchMethod",      "That type has no such method. Check the spelling and the value's type."},
    {"has no method",                      "NoSuchMethod",      "That type has no such method. Check the spelling and the value's type."},
    {"Division by zero",                   "DivByZero",         "Guard the denominator before dividing: `if d != 0 { ... }`."},
    {"index out of bounds",                "IndexOutOfBounds",  "Indexes go from 0 to len()-1. Check the length before indexing."},
    {"Operand must be a number",           "TypeMismatch",      "This operator needs numbers. Parse strings to numbers first."},
    {"concatenation requires strings",     "TypeMismatch",      "Use `\"\" + value` to turn a value into a string before joining with `+`."},
    {"expects",                            "WrongArgCount",     "You passed the wrong number of arguments — match the function's parameter list."},
    {"arguments but got",                  "WrongArgCount",     "You passed the wrong number of arguments — match the function's parameter list."},
    {"non-struct value",                   "TypeMismatch",      "You used `.field` on something that isn't a struct (it may be null)."},
    {"Stack overflow",                     "InfiniteRecursion", "A function is calling itself with no base case. Add a stopping condition."},
    {"closed channel",                     "ClosedChannel",     "Don't send after `task.close(ch)`. Check your channel lifecycle."},
    {"Unhandled exception",                "UncaughtError",     "Something `throw`n wasn't caught. Wrap risky code in `try { } catch e { }`."},
    {NULL, NULL, NULL}
};

static const ErrRule *match_rule(const char *msg) {
    for (int i = 0; RULES[i].needle; i++)
        if (strstr(msg, RULES[i].needle)) return &RULES[i];
    return NULL;
}

/* Pull a raw message string out of either a string error or a struct error
 * that carries a "message" field. Returns NULL if neither. */
static const char *err_message(Value v) {
    if (v.type == VAL_STRING) return v.as.string->chars;
    if (v.type == VAL_STRUCT) {
        ObjStruct *s = v.as.structure;
        for (int i = 0; i < s->field_count; i++)
            if (strcmp(s->field_names[i], "message") == 0 && s->fields[i].type == VAL_STRING)
                return s->fields[i].as.string->chars;
    }
    return NULL;
}

/* errors.explain(err) -> a friendly, multi-line string. */
static Value lib_errors_explain(VM *vm, int argc, Value *args) {
    int b = get_arg_base(argc, args);
    const char *msg = (argc > b) ? err_message(args[b]) : NULL;
    if (!msg) msg = "Unknown error";
    const ErrRule *r = match_rule(msg);
    const char *kind = r ? r->kind : "Error";
    const char *hint = r ? r->hint : "Check the line above and the relevant section of the docs.";
    char out[1024];
    snprintf(out, sizeof(out), "x %s\n  what: %s\n  fix:  %s", kind, msg, hint);
    return val_string(allocate_string(vm, out, (int)strlen(out)));
}

/* errors.kind(err) -> short category string ("UndefinedName", ...). */
static Value lib_errors_kind(VM *vm, int argc, Value *args) {
    int b = get_arg_base(argc, args);
    const char *msg = (argc > b) ? err_message(args[b]) : NULL;
    const ErrRule *r = msg ? match_rule(msg) : NULL;
    const char *kind = r ? r->kind : "Error";
    return val_string(allocate_string(vm, kind, (int)strlen(kind)));
}

/* errors.is(err, kind_string) -> bool */
static Value lib_errors_is(VM *vm, int argc, Value *args) {
    int b = get_arg_base(argc, args);
    if (argc < b + 2 || args[b + 1].type != VAL_STRING) return val_bool(false);
    const char *msg = err_message(args[b]);
    const ErrRule *r = msg ? match_rule(msg) : NULL;
    const char *kind = r ? r->kind : "Error";
    return val_bool(strcmp(kind, args[b + 1].as.string->chars) == 0);
}

/* errors.make(kind, message, hint) -> a struct you can `throw` or return. */
static Value lib_errors_make(VM *vm, int argc, Value *args) {
    int b = get_arg_base(argc, args);
    if (argc < b + 3 || args[b].type != VAL_STRING ||
        args[b + 1].type != VAL_STRING || args[b + 2].type != VAL_STRING) {
        runtime_error(vm, "errors.make(kind, message, hint) requires three strings");
        return val_nil();
    }
    ObjStruct *s = new_struct(vm, 3, false);
    s->type_name = strdup("Error");
    s->field_names[0] = strdup("kind");    s->fields[0] = args[b];
    s->field_names[1] = strdup("message"); s->fields[1] = args[b + 1];
    s->field_names[2] = strdup("hint");    s->fields[2] = args[b + 2];
    return val_struct(s);
}

void lib_errors_init(VM *vm) {
    ObjModule *mod = new_module("errors");
    mod->obj.next = vm->objects;
    vm->objects = (Obj *)mod;
    define_global(vm, copy_string("errors", 6), val_module(mod));
    vm_register_dispatch(vm, "errors", "explain", val_native_fn((void *)lib_errors_explain));
    vm_register_dispatch(vm, "errors", "kind",    val_native_fn((void *)lib_errors_kind));
    vm_register_dispatch(vm, "errors", "is",      val_native_fn((void *)lib_errors_is));
    vm_register_dispatch(vm, "errors", "make",    val_native_fn((void *)lib_errors_make));
}
