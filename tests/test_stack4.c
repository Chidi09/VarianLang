#include <stdio.h>
#include <stdlib.h>
#include "include/vm.h"
#include "include/parser.h"

extern VM *init_vm(void);
extern void free_vm(VM *vm);
extern bool compile(const char *source, ObjFunction **out_fn);
extern bool run(VM *vm, ObjFunction *function);

int main() {
    VM *vm = init_vm();
    ObjFunction *fn;
    const char *src = "let str = ffi_to_string(0);\nprint(str);";
    if (compile(src, &fn)) {
        run(vm, fn);
        printf("Stack top at end: %d\n", vm->stack_top);
    }
    free_vm(vm);
    return 0;
}
