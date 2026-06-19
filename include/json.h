#ifndef JSON_H
#define JSON_H

#include "vm.h"

Value json_decode(VM *vm, const char *json);
char *json_encode(VM *vm, Value v, int *out_len);

void json_skip_ws(const char **p);
char *json_decode_string(const char **p);
Value json_decode_value(VM *vm, const char **p);

Value native_json_encode(VM *vm, int arg_count, Value *args);
Value native_json_decode(VM *vm, int arg_count, Value *args);

#endif
