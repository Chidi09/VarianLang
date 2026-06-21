#ifndef VNB_H
#define VNB_H

#include "vm.h"

int vnb_save(ObjFunction *main_fn, Compiler *compiler, VMAsset *assets, int asset_count, const char *out_path);
ObjFunction *vnb_load(VM *vm, const char *in_path);

#endif
