#include "lib_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

/* The parser's method-name registry is global and untyped: any `impl`
 * method name (e.g. a user-defined Storage.exists()) makes the parser emit
 * a dispatch call (module prepended as args[0]) for *any* later `io.exists(...)`
 * too, not just calls on that struct. Every io.* native checks for that
 * defensively rather than assuming its name will never collide. */
static int io_arg_base(int arg_count, Value *args) {
    return (arg_count >= 1 && args[0].type == VAL_MODULE) ? 1 : 0;
}

static Value lib_io_read_text(VM *vm, int arg_count, Value *args) {
    int base = io_arg_base(arg_count, args);
    if (arg_count < base + 1 || args[base].type != VAL_STRING)
        return val_nil();

    const char *path = args[base].as.string->chars;

    int asset_size = 0;
    const unsigned char *asset_data = vm_lookup_asset(vm, path, &asset_size);
    if (asset_data) {
        ObjString *s = allocate_string(vm, (const char *)asset_data, asset_size);
        return val_string(s);
    }

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
    int base = io_arg_base(arg_count, args);
    if (arg_count < base + 2 || args[base].type != VAL_STRING || args[base + 1].type != VAL_STRING)
        return val_bool(false);

    const char *path = args[base].as.string->chars;
    ObjString *content = args[base + 1].as.string;

    FILE *f = fopen(path, "w");
    if (!f) return val_bool(false);

    size_t written = fwrite(content->chars, 1, (size_t)content->length, f);
    fclose(f);

    return val_bool(written == (size_t)content->length);
}

/* read_bytes/write_bytes are the same binary-safe (fopen "rb"/"wb",
 * explicit-length allocate_string) operations as read_text/write_text --
 * named separately so callers can document intent (storage.vn uses these). */
static Value lib_io_read_bytes(VM *vm, int arg_count, Value *args) {
    int base = io_arg_base(arg_count, args);
    if (arg_count < base + 1 || args[base].type != VAL_STRING)
        return val_nil();

    const char *path = args[base].as.string->chars;

    int asset_size = 0;
    const unsigned char *asset_data = vm_lookup_asset(vm, path, &asset_size);
    if (asset_data) {
        ObjString *s = allocate_string(vm, (const char *)asset_data, asset_size);
        return val_string(s);
    }

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

static Value lib_io_write_bytes(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = io_arg_base(arg_count, args);
    if (arg_count < base + 2 || args[base].type != VAL_STRING || args[base + 1].type != VAL_STRING)
        return val_bool(false);

    const char *path = args[base].as.string->chars;
    ObjString *content = args[base + 1].as.string;

    FILE *f = fopen(path, "wb");
    if (!f) return val_bool(false);

    size_t written = fwrite(content->chars, 1, (size_t)content->length, f);
    fclose(f);

    return val_bool(written == (size_t)content->length);
}

static Value lib_io_exists(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = io_arg_base(arg_count, args);
    if (arg_count < base + 1 || args[base].type != VAL_STRING)
        return val_bool(false);
    struct stat st;
    return val_bool(stat(args[base].as.string->chars, &st) == 0);
}

static Value lib_io_mkdir(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = io_arg_base(arg_count, args);
    if (arg_count < base + 1 || args[base].type != VAL_STRING)
        return val_bool(false);
    const char *path = args[base].as.string->chars;
    if (mkdir(path, 0755) == 0) return val_bool(true);
    struct stat st;
    /* Already exists as a directory -- treat as success, same as mkdir -p. */
    return val_bool(stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

static Value lib_io_delete(VM *vm, int arg_count, Value *args) {
    (void)vm;
    int base = io_arg_base(arg_count, args);
    if (arg_count < base + 1 || args[base].type != VAL_STRING)
        return val_bool(false);
    const char *path = args[base].as.string->chars;
    /* remove() dispatches to unlink() for files and rmdir() for (empty)
     * directories -- it will not recursively delete a non-empty directory,
     * which is the safer default for a generic io.delete(). */
    return val_bool(remove(path) == 0);
}

static Value lib_io_list_dir(VM *vm, int arg_count, Value *args) {
    int base = io_arg_base(arg_count, args);
    if (arg_count < base + 1 || args[base].type != VAL_STRING)
        return val_nil();
    const char *path = args[base].as.string->chars;
    DIR *dir = opendir(path);
    if (!dir) return val_nil();

    ObjArray *arr = (ObjArray *)calloc(1, sizeof(ObjArray));
    arr->obj.type = VAL_ARRAY;
    arr->obj.next = vm->objects;
    vm->objects = (Obj *)arr;
    arr->count = 0;
    arr->capacity = 8;
    arr->elements = (Value *)calloc((size_t)arr->capacity, sizeof(Value));

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        if (arr->count >= arr->capacity) {
            arr->capacity *= 2;
            arr->elements = (Value *)realloc(arr->elements, (size_t)arr->capacity * sizeof(Value));
        }
        ObjString *name = allocate_string(vm, entry->d_name, (int)strlen(entry->d_name));
        arr->elements[arr->count++] = val_string(name);
    }
    closedir(dir);
    return val_array(arr);
}

void lib_io_init(VM *vm) {
    ObjModule *mod = new_module("io");
    mod->obj.next = vm->objects;
    vm->objects = (Obj *)mod;

    define_global(vm, copy_string("io", 2), val_module(mod));

    vm_register_dispatch(vm, "io", "read_text",  val_native_fn((void *)lib_io_read_text));
    vm_register_dispatch(vm, "io", "write_text", val_native_fn((void *)lib_io_write_text));
    vm_register_dispatch(vm, "io", "read_bytes",  val_native_fn((void *)lib_io_read_bytes));
    vm_register_dispatch(vm, "io", "write_bytes", val_native_fn((void *)lib_io_write_bytes));
    vm_register_dispatch(vm, "io", "exists",      val_native_fn((void *)lib_io_exists));
    vm_register_dispatch(vm, "io", "mkdir",       val_native_fn((void *)lib_io_mkdir));
    vm_register_dispatch(vm, "io", "delete",      val_native_fn((void *)lib_io_delete));
    vm_register_dispatch(vm, "io", "list_dir",    val_native_fn((void *)lib_io_list_dir));
}
