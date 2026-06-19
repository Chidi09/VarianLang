#include "varian_ffi.h"
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdio.h>

/* ─── Library handle cache (linked list) ─── */
static FFILibNode *lib_cache = NULL;

void *ffi_open_lib(const char *lib_name) {
    /* Check cache first */
    for (FFILibNode *node = lib_cache; node; node = node->next) {
        if (strcmp(node->name, lib_name) == 0)
            return node->handle;
    }

    /* Try to open */
    void *handle = dlopen(lib_name, RTLD_LAZY | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "FFI: dlopen('%s') failed: %s\n", lib_name, dlerror());
        return NULL;
    }

    /* Cache it */
    FFILibNode *node = (FFILibNode *)malloc(sizeof(FFILibNode));
    if (!node) { dlclose(handle); return NULL; }
    strncpy(node->name, lib_name, MAX_FFI_LIB_NAME - 1);
    node->name[MAX_FFI_LIB_NAME - 1] = '\0';
    node->handle = handle;
    node->next = lib_cache;
    lib_cache = node;

    return handle;
}

void *ffi_find_sym(void *lib_handle, const char *sym_name) {
    if (!lib_handle) return NULL;
    void *sym = dlsym(lib_handle, sym_name);
    if (!sym) {
        fprintf(stderr, "FFI: dlsym('%s') failed: %s\n", sym_name, dlerror());
    }
    return sym;
}

/* ─── FFI type mapping ─── */
ffi_type *ffi_type_from_kind(FFITypeKind kind) {
    switch (kind) {
        case FFI_VOID:    return &ffi_type_void;
        case FFI_INT:     return &ffi_type_sint32;
        case FFI_DOUBLE:  return &ffi_type_double;
        case FFI_FLOAT:   return &ffi_type_float;
        case FFI_PTR:     return &ffi_type_pointer;
        case FFI_CHAR:    return &ffi_type_schar;
        default:          return &ffi_type_void;
    }
}

/* ─── FFI Entry initialization ─── */
bool ffi_entry_init(VMFFIEntry *entry, void *fn_ptr,
                    FFITypeKind return_kind,
                    FFITypeKind *param_kinds, int param_count) {
    if (!entry || !fn_ptr) return false;

    entry->fn_ptr = fn_ptr;
    entry->return_kind = return_kind;
    entry->param_count = param_count;
    if (param_count > 0)
        memcpy(entry->param_kinds, param_kinds, param_count * sizeof(FFITypeKind));

    /* Build the ffi_cif — store arg_types persistently (ffi_prep_cif stores pointer) */
    for (int i = 0; i < param_count; i++)
        entry->arg_types[i] = ffi_type_from_kind(param_kinds[i]);

    /* NOTE: Variadic C functions (e.g. printf) require ffi_prep_cif_var instead
     * of ffi_prep_cif. Varian does NOT support variadic FFI bindings yet. If a
     * variadic function is needed, the FFI type system must first distinguish
     * fixed from variable args, and the cif must be prepared with
     * ffi_prep_cif_var(&cif, FFI_DEFAULT_ABI, nfixed, ntotal, rtype, atypes). */
    ffi_status status = ffi_prep_cif(&entry->cif, FFI_DEFAULT_ABI,
                                      (unsigned int)param_count,
                                      ffi_type_from_kind(return_kind),
                                      entry->arg_types);
    return status == FFI_OK;
}

/* ─── Close all cached libraries ─── */
void ffi_close_all_libs(void) {
    FFILibNode *node = lib_cache;
    while (node) {
        FFILibNode *next = node->next;
        if (node->handle)
            dlclose(node->handle);
        free(node);
        node = next;
    }
    lib_cache = NULL;
}

/* ─── FFI call ─── */
void ffi_entry_call(VMFFIEntry *entry, void *ret_val, void **args) {
    ffi_call(&entry->cif, FFI_FN(entry->fn_ptr), ret_val, args);
}
