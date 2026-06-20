#include "lib_time.h"
#include <sys/time.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

static int time_arg_base(int arg_count, Value *args) {
    return (arg_count >= 1 && args[0].type == VAL_MODULE) ? 1 : 0;
}

static Value lib_time_now_ms(VM *vm, int arg_count, Value *args) {
    (void)vm; (void)arg_count; (void)args;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t ms = (int64_t)tv.tv_sec * 1000 + (int64_t)tv.tv_usec / 1000;
    return val_int(ms);
}

static Value lib_time_now_iso8601(VM *vm, int arg_count, Value *args) {
    int base = time_arg_base(arg_count, args);
    (void)base;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t secs = tv.tv_sec;
    struct tm utc;
    gmtime_r(&secs, &utc);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
              utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
              utc.tm_hour, utc.tm_min, utc.tm_sec, (int)(tv.tv_usec / 1000));
    ObjString *s = allocate_string(vm, buf, (int)strlen(buf));
    return val_string(s);
}

void lib_time_init(VM *vm) {
    ObjModule *mod = new_module("time");
    mod->obj.next = vm->objects;
    vm->objects = (Obj *)mod;
    define_global(vm, copy_string("time", 4), val_module(mod));
    vm_register_dispatch(vm, "time", "now_ms",     val_native_fn((void *)lib_time_now_ms));
    vm_register_dispatch(vm, "time", "now_iso8601", val_native_fn((void *)lib_time_now_iso8601));
}
