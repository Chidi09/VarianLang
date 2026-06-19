#ifndef FFI_H
#define FFI_H

#include "varian.h"
#include <ffi.h>
#include <stdbool.h>

/* ─── FFI Type System ─── */
/* These map Varian FFI declarations to the correct C ABI widths */

#define MAX_FFI_PARAMS 16
#define MAX_FFI_ENTRIES 64
#define MAX_FFI_LIB_NAME 256
#define MAX_FFI_FUNC_NAME 256

typedef enum {
    FFI_VOID,
    FFI_INT,      /* maps to C `int` (32-bit) */
    FFI_DOUBLE,   /* maps to C `double` */
    FFI_FLOAT,    /* maps to C `float` (32-bit) */
    FFI_PTR,      /* maps to C `void*` */
    FFI_CHAR,     /* maps to C `char` */
} FFITypeKind;

/* Get the libffi type descriptor for a FFITypeKind */
ffi_type *ffi_type_from_kind(FFITypeKind kind);

/* ─── FFI Entry (resolved at runtime) ─── */
typedef struct {
    void *fn_ptr;              /* resolved function pointer from dlsym */
    ffi_cif cif;               /* libffi call interface */
    FFITypeKind param_kinds[MAX_FFI_PARAMS];
    int param_count;
    FFITypeKind return_kind;
    ffi_type *arg_types[MAX_FFI_PARAMS]; /* persistent copy for cif */
} VMFFIEntry;

/* ─── FFI Declaration (compile-time metadata) ─── */
/* Stored by the compiler, resolved by the VM at startup */
typedef struct {
    char name[64];             /* Varian function name */
    char lib_name[MAX_FFI_LIB_NAME];  /* shared library path */
    char func_name[MAX_FFI_FUNC_NAME]; /* C symbol name */
    FFITypeKind param_kinds[MAX_FFI_PARAMS];
    int param_count;
    FFITypeKind return_kind;
} FFIDecl;

/* ─── Library Handle Cache ─── */
typedef struct FFILibNode {
    char name[MAX_FFI_LIB_NAME];
    void *handle;  /* from dlopen */
    struct FFILibNode *next;
} FFILibNode;

/* Open a shared library with caching */
void *ffi_open_lib(const char *lib_name);

/* Resolve a symbol from an already-open library */
void *ffi_find_sym(void *lib_handle, const char *sym_name);

/* Initialize a VMFFIEntry from a resolved function pointer and type info */
bool ffi_entry_init(VMFFIEntry *entry, void *fn_ptr,
                    FFITypeKind return_kind,
                    FFITypeKind *param_kinds, int param_count);

/* Perform an FFI call, marshaling from the Varian stack */
void ffi_entry_call(VMFFIEntry *entry, void *ret_val, void **args);

/* Close all cached library handles (call from vm_free) */
void ffi_close_all_libs(void);

#endif /* FFI_H */
