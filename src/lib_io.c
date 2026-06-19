#include "lib_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Value lib_io_read_text(VM *vm, int arg_count, Value *args) {
    if (arg_count < 1 || args[0].type != VAL_STRING)
        return val_nil();

    const char *path = args[0].as.string->chars;

    FILE *f = fopen(path, "rb");
    if (!f) return val_nil();

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    if (file_size < 0) { fclose(f); return val_nil(); }
    rewind(f);

    char *buf = (char *)malloc((size_t)file_size + 1);
    if (!buf) { fclose(f); return val_nil(); }

    size_t bytes_read = fread(buf, 1, (size_t)file_size, f);
    buf[bytes_read] = '\0';
    fclose(f);

    ObjString *s = allocate_string(vm, buf, (int)bytes_read);
    free(buf);
    return val_string(s);
}

static Value lib_io_write_text(VM *vm, int arg_count, Value *args) {
    (void)vm;
    if (arg_count < 2 || args[0].type != VAL_STRING || args[1].type != VAL_STRING)
        return val_bool(false);

    const char *path = args[0].as.string->chars;
    ObjString *content = args[1].as.string;

    FILE *f = fopen(path, "w");
    if (!f) return val_bool(false);

    size_t written = fwrite(content->chars, 1, (size_t)content->length, f);
    fclose(f);

    return val_bool(written == (size_t)content->length);
}

void lib_io_init(VM *vm) {
    ObjModule *mod = new_module("io");
    mod->obj.next = vm->objects;
    vm->objects = (Obj *)mod;

    define_global(vm, copy_string("io", 2), val_module(mod));

    vm_register_dispatch(vm, "io", "read_text",  val_native_fn((void *)lib_io_read_text));
    vm_register_dispatch(vm, "io", "write_text", val_native_fn((void *)lib_io_write_text));
}
