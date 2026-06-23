#ifndef FFI_H
#define FFI_H

#include "varian.h"
#include <stdbool.h>

#define MAX_FFI_PARAMS 16
#define MAX_FFI_ENTRIES 64
#define MAX_FFI_LIB_NAME 256
#define MAX_FFI_FUNC_NAME 256

typedef enum {
    FFI_VOID,
    FFI_INT,
    FFI_DOUBLE,
    FFI_FLOAT,
    FFI_PTR,
    FFI_CHAR,
} FFITypeKind;

#ifndef VN_NO_FFI
#include <ffi.h>

ffi_type *ffi_type_from_kind(FFITypeKind kind);

typedef struct {
    void *fn_ptr;
    ffi_cif cif;
    FFITypeKind param_kinds[MAX_FFI_PARAMS];
    int param_count;
    FFITypeKind return_kind;
    ffi_type *arg_types[MAX_FFI_PARAMS];
} VMFFIEntry;

typedef struct {
    char name[64];
    char lib_name[MAX_FFI_LIB_NAME];
    char func_name[MAX_FFI_FUNC_NAME];
    FFITypeKind param_kinds[MAX_FFI_PARAMS];
    int param_count;
    FFITypeKind return_kind;
} FFIDecl;

typedef struct FFILibNode {
    char name[MAX_FFI_LIB_NAME];
    void *handle;
    struct FFILibNode *next;
} FFILibNode;

void *ffi_open_lib(const char *lib_name);
void *ffi_find_sym(void *lib_handle, const char *sym_name);
bool ffi_entry_init(VMFFIEntry *entry, void *fn_ptr,
                    FFITypeKind return_kind,
                    FFITypeKind *param_kinds, int param_count);
void ffi_entry_call(VMFFIEntry *entry, void *ret_val, void **args);
void ffi_close_all_libs(void);
#else
/* Stub types so vm.h compiles without libffi */
typedef struct { int _dummy; } VMFFIEntry;
typedef struct {
    char name[64];
    char lib_name[MAX_FFI_LIB_NAME];
    char func_name[MAX_FFI_FUNC_NAME];
    FFITypeKind param_kinds[MAX_FFI_PARAMS];
    int param_count;
    FFITypeKind return_kind;
} FFIDecl;
#endif

#endif /* FFI_H */
