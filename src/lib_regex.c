#include "lib_regex.h"
#include <regex.h>
#include <stdlib.h>
#include <string.h>

/* POSIX Extended Regular Expressions (regcomp/regexec, libc) exposed as a
 * `regex` module. POSIX ERE has no \d/\w shorthands -- use [0-9], [A-Za-z],
 * etc. -- but supports anchors, alternation, quantifiers and capture groups.
 *
 * Module-style calls (regex.test(p, s)) arrive with the module value as
 * args[0], so every entry point resolves its real first argument via base. */

static int rx_arg_base(int arg_count, Value *args) {
    if (arg_count >= 1 && args[0].type == VAL_MODULE)
        return 1;
    return 0;
}

/* Compile `pattern`. `flags` is an optional string; an 'i' enables case-
 * insensitive matching. Returns true on success (caller must regfree). */
static bool rx_compile(regex_t *re, const char *pattern, const char *flags) {
    int cflags = REG_EXTENDED;
    if (flags) {
        for (const char *f = flags; *f; f++) {
            if (*f == 'i') cflags |= REG_ICASE;
            else if (*f == 'm') cflags |= REG_NEWLINE;
        }
    }
    return regcomp(re, pattern, cflags) == 0;
}

/* Pull (pattern, subject, [flags]) out of args; returns false if malformed. */
static bool rx_two_strings(int arg_count, Value *args, int base,
                           const char **pattern, const char **subject,
                           const char **flags) {
    if (arg_count < base + 2 ||
        args[base].type != VAL_STRING || args[base + 1].type != VAL_STRING)
        return false;
    *pattern = args[base].as.string->chars;
    *subject = args[base + 1].as.string->chars;
    *flags = NULL;
    if (arg_count > base + 2 && args[base + 2].type == VAL_STRING)
        *flags = args[base + 2].as.string->chars;
    return true;
}

/* regex.test(pattern, subject, [flags]) -> bool */
static Value lib_regex_test(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = rx_arg_base(arg_count, args);
    const char *pattern, *subject, *flags;
    if (!rx_two_strings(arg_count, args, base, &pattern, &subject, &flags))
        return val_bool(false);
    regex_t re;
    if (!rx_compile(&re, pattern, flags)) return val_bool(false);
    int rc = regexec(&re, subject, 0, NULL, 0);
    regfree(&re);
    return val_bool(rc == 0);
}

/* regex.match(pattern, subject, [flags]) -> first full match string, or null */
static Value lib_regex_match(VM *vm, int arg_count, Value *args) {
    int base = rx_arg_base(arg_count, args);
    const char *pattern, *subject, *flags;
    if (!rx_two_strings(arg_count, args, base, &pattern, &subject, &flags))
        return val_nil();
    regex_t re;
    if (!rx_compile(&re, pattern, flags)) return val_nil();
    regmatch_t m[1];
    Value result = val_nil();
    if (regexec(&re, subject, 1, m, 0) == 0 && m[0].rm_so >= 0) {
        int len = (int)(m[0].rm_eo - m[0].rm_so);
        result = val_string(allocate_string(vm, subject + m[0].rm_so, len));
    }
    regfree(&re);
    return result;
}

/* regex.groups(pattern, subject, [flags]) -> [full, g1, g2, ...] or null.
 * A group that did not participate in the match is the empty string. */
static Value lib_regex_groups(VM *vm, int arg_count, Value *args) {
    int base = rx_arg_base(arg_count, args);
    const char *pattern, *subject, *flags;
    if (!rx_two_strings(arg_count, args, base, &pattern, &subject, &flags))
        return val_nil();
    regex_t re;
    if (!rx_compile(&re, pattern, flags)) return val_nil();

    size_t ngroups = re.re_nsub + 1;
    regmatch_t *m = (regmatch_t *)calloc(ngroups, sizeof(regmatch_t));
    if (!m) { regfree(&re); return val_nil(); }

    Value result = val_nil();
    if (regexec(&re, subject, ngroups, m, 0) == 0) {
        ObjArray *arr = new_array();
        arr->obj.next = vm->objects;
        vm->objects = (Obj *)arr;
        /* Keep arr reachable across allocate_string GC cycles. */
        Task *self = vm->current_task;
        self->stack[self->stack_top++] = val_array(arr);
        for (size_t g = 0; g < ngroups; g++) {
            const char *s = "";
            int len = 0;
            if (m[g].rm_so >= 0) { s = subject + m[g].rm_so; len = (int)(m[g].rm_eo - m[g].rm_so); }
            if (arr->count >= arr->capacity) {
                int nc = arr->capacity ? arr->capacity * 2 : 8;
                arr->elements = (Value *)realloc(arr->elements, (size_t)nc * sizeof(Value));
                arr->capacity = nc;
            }
            arr->elements[arr->count++] = val_string(allocate_string(vm, s, len));
        }
        self->stack_top--;
        result = val_array(arr);
    }
    free(m);
    regfree(&re);
    return result;
}

/* regex.find_all(pattern, subject, [flags]) -> array of full match strings. */
static Value lib_regex_find_all(VM *vm, int arg_count, Value *args) {
    int base = rx_arg_base(arg_count, args);
    const char *pattern, *subject, *flags;
    if (!rx_two_strings(arg_count, args, base, &pattern, &subject, &flags))
        return val_nil();
    regex_t re;
    if (!rx_compile(&re, pattern, flags)) return val_nil();

    ObjArray *arr = new_array();
    arr->obj.next = vm->objects;
    vm->objects = (Obj *)arr;
    Task *self = vm->current_task;
    self->stack[self->stack_top++] = val_array(arr);

    const char *cursor = subject;
    regmatch_t m[1];
    /* REG_NOTBOL on the 2nd+ exec so '^' only anchors at the true start. */
    int eflags = 0;
    while (regexec(&re, cursor, 1, m, eflags) == 0 && m[0].rm_so >= 0) {
        int len = (int)(m[0].rm_eo - m[0].rm_so);
        if (arr->count >= arr->capacity) {
            int nc = arr->capacity ? arr->capacity * 2 : 8;
            arr->elements = (Value *)realloc(arr->elements, (size_t)nc * sizeof(Value));
            arr->capacity = nc;
        }
        arr->elements[arr->count++] = val_string(allocate_string(vm, cursor + m[0].rm_so, len));
        int step = (int)m[0].rm_eo;
        /* Empty match: step one char forward so we don't spin in place. */
        if (m[0].rm_eo == m[0].rm_so) step += 1;
        if (cursor[(int)m[0].rm_eo] == '\0') break;   /* nothing left to scan */
        cursor += step;
        eflags = REG_NOTBOL;
    }
    self->stack_top--;
    regfree(&re);
    return val_array(arr);
}

/* Expand "\0".."\9" backreferences in `repl` for one match into `out`. */
static int rx_expand(char *out, int out_cap, const char *repl,
                     const char *subject, regmatch_t *m, size_t ngroups) {
    int o = 0;
    for (const char *p = repl; *p && o < out_cap - 1; p++) {
        if (*p == '\\' && p[1] >= '0' && p[1] <= '9') {
            size_t g = (size_t)(p[1] - '0');
            if (g < ngroups && m[g].rm_so >= 0) {
                int len = (int)(m[g].rm_eo - m[g].rm_so);
                for (int k = 0; k < len && o < out_cap - 1; k++)
                    out[o++] = subject[m[g].rm_so + k];
            }
            p++;
        } else if (*p == '\\' && p[1] == '\\') {
            out[o++] = '\\';
            p++;
        } else {
            out[o++] = *p;
        }
    }
    out[o] = '\0';
    return o;
}

/* regex.replace(pattern, subject, replacement, [flags]) -> string, all matches
 * replaced. `replacement` may use \0 (whole match) and \1..\9 (groups). */
static Value lib_regex_replace(VM *vm, int arg_count, Value *args) {
    int base = rx_arg_base(arg_count, args);
    if (arg_count < base + 3 || args[base].type != VAL_STRING ||
        args[base + 1].type != VAL_STRING || args[base + 2].type != VAL_STRING)
        return val_nil();
    const char *pattern = args[base].as.string->chars;
    const char *subject = args[base + 1].as.string->chars;
    const char *repl = args[base + 2].as.string->chars;
    const char *flags = (arg_count > base + 3 && args[base + 3].type == VAL_STRING)
                        ? args[base + 3].as.string->chars : NULL;

    regex_t re;
    if (!rx_compile(&re, pattern, flags)) return args[base + 1];

    size_t ngroups = re.re_nsub + 1;
    regmatch_t *m = (regmatch_t *)calloc(ngroups, sizeof(regmatch_t));
    if (!m) { regfree(&re); return val_nil(); }

    size_t cap = strlen(subject) * 2 + 64;
    char *out = (char *)malloc(cap);
    if (!out) { free(m); regfree(&re); return val_nil(); }
    int o = 0;

    const char *cursor = subject;
    int eflags = 0;
    char expand[4096];
    while (regexec(&re, cursor, ngroups, m, eflags) == 0 && m[0].rm_so >= 0) {
        /* Copy the text before the match. */
        for (int k = 0; k < m[0].rm_so; k++) {
            if ((size_t)o >= cap - 1) { cap *= 2; out = (char *)realloc(out, cap); }
            out[o++] = cursor[k];
        }
        /* Append the expanded replacement. */
        int el = rx_expand(expand, sizeof(expand), repl, cursor, m, ngroups);
        for (int k = 0; k < el; k++) {
            if ((size_t)o >= cap - 1) { cap *= 2; out = (char *)realloc(out, cap); }
            out[o++] = expand[k];
        }
        int step = (int)m[0].rm_eo;
        if (m[0].rm_eo == m[0].rm_so) {
            /* Empty match: emit one char and advance to avoid looping. */
            if (cursor[step] == '\0') { cursor += step; break; }
            if ((size_t)o >= cap - 1) { cap *= 2; out = (char *)realloc(out, cap); }
            out[o++] = cursor[step];
            step += 1;
        }
        cursor += step;
        eflags = REG_NOTBOL;
        if (*cursor == '\0') break;
    }
    /* Trailing remainder after the last match. */
    for (const char *p = cursor; *p; p++) {
        if ((size_t)o >= cap - 1) { cap *= 2; out = (char *)realloc(out, cap); }
        out[o++] = *p;
    }
    out[o] = '\0';

    Value result = val_string(allocate_string(vm, out, o));
    free(out);
    free(m);
    regfree(&re);
    return result;
}

void lib_regex_init(VM *vm) {
    ObjModule *mod = new_module("regex");
    mod->obj.next = vm->objects;
    vm->objects = (Obj *)mod;
    define_global(vm, copy_string("regex", 5), val_module(mod));
    vm_register_dispatch(vm, "regex", "test",     val_native_fn((void *)lib_regex_test));
    vm_register_dispatch(vm, "regex", "match",    val_native_fn((void *)lib_regex_match));
    vm_register_dispatch(vm, "regex", "groups",   val_native_fn((void *)lib_regex_groups));
    vm_register_dispatch(vm, "regex", "find_all", val_native_fn((void *)lib_regex_find_all));
    vm_register_dispatch(vm, "regex", "replace",  val_native_fn((void *)lib_regex_replace));
}
