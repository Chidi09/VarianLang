#include "vnb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VNB_MAGIC "VNB0"
/* Bump whenever the on-disk layout or the bytecode/opcode format changes, so a
 * bundle built by an incompatible runtime is rejected instead of misexecuting. */
#define VNB_FORMAT_VERSION 2u

static void collect_functions(Value val, ObjFunction ***funcs, int *count, int *capacity) {
    if (val.type != VAL_FUNCTION) return;
    ObjFunction *fn = val.as.function;
    
    for (int i = 0; i < *count; i++) {
        if ((*funcs)[i] == fn) return;
    }
    
    if (*count >= *capacity) {
        *capacity = *capacity ? *capacity * 2 : 16;
        *funcs = realloc(*funcs, (size_t)(*capacity) * sizeof(ObjFunction *));
    }
    (*funcs)[*count] = fn;
    (*count)++;
    
    for (int i = 0; i < fn->constant_count; i++) {
        collect_functions(fn->constants[i], funcs, count, capacity);
    }
}

int vnb_save(ObjFunction *main_fn, Compiler *compiler, VMAsset *assets, int asset_count, const char *out_path) {
    FILE *f = fopen(out_path, "wb");
    if (!f) return 0;
    
    // Write Header: magic + format version + architecture sentinels.
    // The .vnb format stores native integers, so a bundle is host-architecture
    // specific. An endian sentinel + word-size byte let the loader reject a
    // foreign-arch bundle cleanly instead of silently misinterpreting bytes.
    fwrite(VNB_MAGIC, 1, 4, f);
    uint32_t fmt_version = VNB_FORMAT_VERSION;
    fwrite(&fmt_version, sizeof(uint32_t), 1, f);
    uint32_t endian_sentinel = 0x01020304u;
    fwrite(&endian_sentinel, sizeof(uint32_t), 1, f);
    uint8_t word_size = (uint8_t)sizeof(void *);
    fwrite(&word_size, 1, 1, f);

    // Write FFI declarations
    int ffi_count = compiler ? compiler->ffi_decl_count : 0;
    fwrite(&ffi_count, sizeof(int), 1, f);
    if (ffi_count > 0) {
        fwrite(compiler->ffi_decls, sizeof(FFIDecl), (size_t)ffi_count, f);
    }

    // Write Virtual Assets
    fwrite(&asset_count, sizeof(int), 1, f);
    for (int i = 0; i < asset_count; i++) {
        int path_len = (int)strlen(assets[i].path);
        fwrite(&path_len, sizeof(int), 1, f);
        fwrite(assets[i].path, 1, (size_t)path_len, f);
        fwrite(&assets[i].size, sizeof(int), 1, f);
        fwrite(assets[i].data, 1, (size_t)assets[i].size, f);
    }
    
    int fn_count = 0;
    int fn_capacity = 0;
    ObjFunction **funcs = NULL;
    collect_functions(val_function(main_fn), &funcs, &fn_count, &fn_capacity);
    
    // Ensure main_fn is funcs[0]
    int main_idx = -1;
    for (int i = 0; i < fn_count; i++) {
        if (funcs[i] == main_fn) { main_idx = i; break; }
    }
    if (main_idx > 0) {
        ObjFunction *tmp = funcs[0];
        funcs[0] = funcs[main_idx];
        funcs[main_idx] = tmp;
    }
    
    fwrite(&fn_count, sizeof(int), 1, f);
    
    for (int i = 0; i < fn_count; i++) {
        ObjFunction *fn = funcs[i];
        fwrite(&fn->arity, sizeof(int), 1, f);
        fwrite(&fn->stack_size, sizeof(int), 1, f);
        uint8_t is_mod = fn->is_module_init ? 1 : 0;
        fwrite(&is_mod, 1, 1, f);
        fwrite(&fn->code_count, sizeof(int), 1, f);
        fwrite(fn->code, 1, (size_t)fn->code_count, f);
        
        fwrite(&fn->rle_count, sizeof(int), 1, f);
        fwrite(fn->rle_lines, sizeof(int), (size_t)fn->rle_count, f);
        fwrite(fn->rle_counts, sizeof(int), (size_t)fn->rle_count, f);
        
        fwrite(&fn->constant_count, sizeof(int), 1, f);
        for (int c = 0; c < fn->constant_count; c++) {
            Value cv = fn->constants[c];
            uint8_t tag = 0;
            if (cv.type == VAL_NIL) { tag = 0; fwrite(&tag, 1, 1, f); }
            else if (cv.type == VAL_BOOL) { tag = 1; fwrite(&tag, 1, 1, f); uint8_t b = cv.as.boolean ? 1 : 0; fwrite(&b, 1, 1, f); }
            else if (cv.type == VAL_INT) { tag = 2; fwrite(&tag, 1, 1, f); fwrite(&cv.as.integer, sizeof(int64_t), 1, f); }
            else if (cv.type == VAL_FLOAT) { tag = 3; fwrite(&tag, 1, 1, f); fwrite(&cv.as.floating, sizeof(double), 1, f); }
            else if (cv.type == VAL_STRING) { 
                tag = 4; fwrite(&tag, 1, 1, f); 
                int len = cv.as.string->length; 
                fwrite(&len, sizeof(int), 1, f); 
                fwrite(cv.as.string->chars, 1, (size_t)len, f); 
            }
            else if (cv.type == VAL_FUNCTION) {
                tag = 5; fwrite(&tag, 1, 1, f);
                int idx = -1;
                for (int j = 0; j < fn_count; j++) { if (funcs[j] == cv.as.function) { idx = j; break; } }
                fwrite(&idx, sizeof(int), 1, f);
            }
            else {
                tag = 0; fwrite(&tag, 1, 1, f); // fallback nil
            }
        }
    }
    
    free(funcs);
    fclose(f);
    return 1;
}

ObjFunction *vnb_load(VM *vm, const char *in_path) {
    FILE *f = fopen(in_path, "rb");
    if (!f) return NULL;
    
    char magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, VNB_MAGIC, 4) != 0) {
        fprintf(stderr, "vnb: '%s' is not a Varian bundle.\n", in_path);
        fclose(f);
        return NULL;
    }
    uint32_t fmt_version = 0;
    if (fread(&fmt_version, sizeof(uint32_t), 1, f) != 1 || fmt_version != VNB_FORMAT_VERSION) {
        fprintf(stderr, "vnb: '%s' was built for bundle format v%u, but this runtime expects v%u. Rebuild with `vn build`.\n",
                in_path, fmt_version, VNB_FORMAT_VERSION);
        fclose(f);
        return NULL;
    }
    uint32_t endian_sentinel = 0;
    if (fread(&endian_sentinel, sizeof(uint32_t), 1, f) != 1 || endian_sentinel != 0x01020304u) {
        fprintf(stderr, "vnb: '%s' was built on a different-endian architecture. Rebuild on this machine.\n", in_path);
        fclose(f);
        return NULL;
    }
    uint8_t word_size = 0;
    if (fread(&word_size, 1, 1, f) != 1 || word_size != (uint8_t)sizeof(void *)) {
        fprintf(stderr, "vnb: '%s' was built for a %u-bit architecture; this runtime is %zu-bit. Rebuild on this machine.\n",
                in_path, (unsigned)word_size * 8u, sizeof(void *) * 8);
        fclose(f);
        return NULL;
    }

    // Read FFI declarations
    int ffi_count = 0;
    if (fread(&ffi_count, sizeof(int), 1, f) != 1) {
        fclose(f); return NULL;
    }
    if (ffi_count > 0) {
#ifndef VN_NO_FFI
        vm->ffi_entries = (VMFFIEntry *)calloc((size_t)ffi_count, sizeof(VMFFIEntry));
        if (!vm->ffi_entries) {
            fclose(f); return NULL;
        }
        vm->ffi_entry_count = ffi_count;
        for (int i = 0; i < ffi_count; i++) {
            FFIDecl decl;
            if (fread(&decl, sizeof(FFIDecl), 1, f) != 1) {
                fclose(f); return NULL;
            }
            void *lib = ffi_open_lib(decl.lib_name);
            if (!lib) {
                fprintf(stderr, "FFI: Cannot open library '%s'\n", decl.lib_name);
                fclose(f); return NULL;
            }
            void *fn_ptr = ffi_find_sym(lib, decl.func_name);
            if (!fn_ptr) {
                fprintf(stderr, "FFI: Cannot find symbol '%s' in '%s'\n", decl.func_name, decl.lib_name);
                fclose(f); return NULL;
            }
            if (!ffi_entry_init(&vm->ffi_entries[i], fn_ptr, decl.return_kind, decl.param_kinds, decl.param_count)) {
                fprintf(stderr, "FFI: Failed to init call interface for '%s'\n", decl.func_name);
                fclose(f); return NULL;
            }
        }
#else
        fprintf(stderr, "FFI: bundled bytecode uses FFI but this build has FFI disabled\n");
        fclose(f); return NULL;
#endif
    }
    
    // All counts/lengths below come straight from the file, so each is range-
    // checked before it drives an allocation or an index — a corrupt or hostile
    // bundle must fail cleanly, never over-allocate or read out of bounds.
#define RD(p, sz, n) do { if (fread((p), (sz), (size_t)(n), f) != (size_t)(n)) goto fail; } while (0)
#define SANE(x, hi)  ((x) >= 0 && (x) <= (hi))

    // Read Virtual Assets
    int asset_count = 0;
    if (fread(&asset_count, sizeof(int), 1, f) != 1 || !SANE(asset_count, 1 << 20)) {
        fclose(f); return NULL;
    }
    if (asset_count > 0) {
        vm->assets = (VMAsset *)calloc((size_t)asset_count, sizeof(VMAsset));
        if (!vm->assets) { fclose(f); return NULL; }
        vm->asset_count = asset_count;
        for (int i = 0; i < asset_count; i++) {
            int path_len = 0;
            if (fread(&path_len, sizeof(int), 1, f) != 1 || !SANE(path_len, 1 << 16)) { fclose(f); return NULL; }
            vm->assets[i].path = malloc((size_t)path_len + 1);
            if (!vm->assets[i].path || fread(vm->assets[i].path, 1, (size_t)path_len, f) != (size_t)path_len) { fclose(f); return NULL; }
            vm->assets[i].path[path_len] = '\0';

            if (fread(&vm->assets[i].size, sizeof(int), 1, f) != 1 || !SANE(vm->assets[i].size, 1 << 30)) { fclose(f); return NULL; }
            vm->assets[i].data = malloc((size_t)vm->assets[i].size);
            if (!vm->assets[i].data || fread(vm->assets[i].data, 1, (size_t)vm->assets[i].size, f) != (size_t)vm->assets[i].size) { fclose(f); return NULL; }
        }
    }

    int fn_count = 0;
    if (fread(&fn_count, sizeof(int), 1, f) != 1 || fn_count < 1 || fn_count > (1 << 22)) {
        fclose(f); return NULL;
    }

    ObjFunction **funcs = calloc((size_t)fn_count, sizeof(ObjFunction *));
    if (!funcs) { fclose(f); return NULL; }
    for (int i = 0; i < fn_count; i++) funcs[i] = new_function();

    for (int i = 0; i < fn_count; i++) {
        ObjFunction *fn = funcs[i];
        RD(&fn->arity, sizeof(int), 1);
        RD(&fn->stack_size, sizeof(int), 1);
        uint8_t is_mod = 0;
        RD(&is_mod, 1, 1);
        fn->is_module_init = is_mod ? true : false;
        RD(&fn->code_count, sizeof(int), 1);
        if (!SANE(fn->code_count, 1 << 28)) goto fail;
        fn->code_capacity = fn->code_count;
        fn->code = malloc((size_t)fn->code_count + 1);
        if (!fn->code) goto fail;
        RD(fn->code, 1, fn->code_count);

        RD(&fn->rle_count, sizeof(int), 1);
        if (!SANE(fn->rle_count, 1 << 28)) goto fail;
        fn->rle_lines = malloc((size_t)(fn->rle_count ? fn->rle_count : 1) * sizeof(int));
        fn->rle_counts = malloc((size_t)(fn->rle_count ? fn->rle_count : 1) * sizeof(int));
        if (!fn->rle_lines || !fn->rle_counts) goto fail;
        RD(fn->rle_lines, sizeof(int), fn->rle_count);
        RD(fn->rle_counts, sizeof(int), fn->rle_count);

        RD(&fn->constant_count, sizeof(int), 1);
        if (!SANE(fn->constant_count, 1 << 24)) goto fail;
        fn->constant_capacity = fn->constant_count;
        fn->constants = malloc((size_t)(fn->constant_count ? fn->constant_count : 1) * sizeof(Value));
        if (!fn->constants) goto fail;
        for (int c = 0; c < fn->constant_count; c++) {
            uint8_t tag;
            RD(&tag, 1, 1);
            if (tag == 0) { fn->constants[c] = val_nil(); }
            else if (tag == 1) { uint8_t b; RD(&b, 1, 1); fn->constants[c] = val_bool(b); }
            else if (tag == 2) { int64_t v; RD(&v, sizeof(int64_t), 1); fn->constants[c] = val_int(v); }
            else if (tag == 3) { double v; RD(&v, sizeof(double), 1); fn->constants[c] = val_float(v); }
            else if (tag == 4) {
                int len; RD(&len, sizeof(int), 1);
                if (!SANE(len, 1 << 28)) goto fail;
                char *str = malloc((size_t)len + 1);
                if (!str) goto fail;
                if (fread(str, 1, (size_t)len, f) != (size_t)len) { free(str); goto fail; }
                str[len] = '\0';
                ObjString *os = copy_string(str, len);
                free(str);
                fn->constants[c] = val_string(os);
            }
            else if (tag == 5) {
                int idx; RD(&idx, sizeof(int), 1);
                if (idx < 0 || idx >= fn_count) goto fail;   /* bounds-check the function ref */
                fn->constants[c] = val_function(funcs[idx]);
            }
            else { goto fail; }   /* unknown constant tag */
        }
        fn->metadata = val_nil();
    }

    ObjFunction *main_fn = funcs[0];
    free(funcs);
    fclose(f);
    return main_fn;

fail:
    fprintf(stderr, "vnb: '%s' is corrupt or truncated.\n", in_path);
    free(funcs);
    fclose(f);
    return NULL;
#undef RD
#undef SANE
}
