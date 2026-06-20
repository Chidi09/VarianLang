#ifndef LIB_ENV_H
#define LIB_ENV_H

#include "vm.h"

/* Initialize the env module: registers "env" global + dispatch entries */
void lib_env_init(VM *vm);

#endif /* LIB_ENV_H */
