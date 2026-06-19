#include "lib_redis.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <hiredis/hiredis.h>

static Value lib_redis_connect(VM *vm, int arg_count, Value *args) {
    bool is_dispatch = (arg_count >= 3 && args[0].type == VAL_MODULE);
    Value host_val = is_dispatch ? args[1] : args[0];
    Value port_val = is_dispatch ? args[2] : args[1];

    if (host_val.type != VAL_STRING || port_val.type != VAL_INT) {
        runtime_error(vm, "redis.connect() requires host (string) and port (integer)");
        return val_nil();
    }
    const char *host = host_val.as.string->chars;
    int port = (int)port_val.as.integer;

    redisContext *ctx = redisConnect(host, port);
    if (!ctx || ctx->err) {
        const char *err_msg = ctx ? ctx->errstr : "connection failed";
        runtime_error(vm, "redis.connect() failed: %s", err_msg);
        if (ctx) redisFree(ctx);
        return val_nil();
    }
    return val_int((int64_t)(intptr_t)ctx);
}

static Value redis_reply_to_value(VM *vm, redisReply *reply) {
    if (!reply) return val_nil();
    switch (reply->type) {
        case REDIS_REPLY_STATUS:
        case REDIS_REPLY_ERROR:
        case REDIS_REPLY_STRING:
            return val_string(allocate_string(vm, reply->str, (int)reply->len));
        case REDIS_REPLY_INTEGER:
            return val_int(reply->integer);
        case REDIS_REPLY_NIL:
            return val_nil();
        case REDIS_REPLY_ARRAY: {
            ObjArray *arr = (ObjArray *)calloc(1, sizeof(ObjArray));
            arr->obj.type = VAL_ARRAY;
            arr->obj.next = vm->objects;
            vm->objects = (Obj *)arr;
            arr->count = (int)reply->elements;
            arr->capacity = (int)reply->elements;
            arr->elements = (Value *)calloc(reply->elements, sizeof(Value));
            for (size_t i = 0; i < reply->elements; i++) {
                arr->elements[i] = redis_reply_to_value(vm, reply->element[i]);
            }
            return val_array(arr);
        }
        default:
            return val_nil();
    }
}

static Value lib_redis_cmd(VM *vm, int arg_count, Value *args) {
    bool is_dispatch = (arg_count >= 3 && args[0].type == VAL_MODULE);
    Value conn_val = is_dispatch ? args[1] : args[0];
    Value cmd_val = is_dispatch ? args[2] : args[1];
    Value params_val = is_dispatch ? (arg_count >= 4 ? args[3] : val_nil()) : (arg_count >= 3 ? args[2] : val_nil());

    if (conn_val.type != VAL_INT) {
        runtime_error(vm, "redis.cmd(): conn must be an integer (connection handle)");
        return val_nil();
    }
    if (cmd_val.type != VAL_STRING) {
        runtime_error(vm, "redis.cmd(): cmd must be a string");
        return val_nil();
    }

    redisContext *ctx = (redisContext *)(intptr_t)conn_val.as.integer;
    const char *cmd_str = cmd_val.as.string->chars;

    int extra_args = 0;
    ObjArray *arr = NULL;
    if (params_val.type == VAL_ARRAY) {
        arr = params_val.as.array;
        extra_args = arr->count;
    }

    int cmd_argc = 1 + extra_args;
    const char **cmd_argv = (const char **)calloc((size_t)cmd_argc, sizeof(char *));
    size_t *cmd_argv_len = (size_t *)calloc((size_t)cmd_argc, sizeof(size_t));

    cmd_argv[0] = cmd_str;
    cmd_argv_len[0] = strlen(cmd_str);

    char **temp_bufs = NULL;
    if (extra_args > 0) {
        temp_bufs = (char **)calloc((size_t)extra_args, sizeof(char *));
        for (int i = 0; i < extra_args; i++) {
            Value v = arr->elements[i];
            char buf[128];
            switch (v.type) {
                case VAL_NIL:
                    temp_bufs[i] = strdup("");
                    break;
                case VAL_INT:
                    snprintf(buf, sizeof(buf), "%" PRId64, v.as.integer);
                    temp_bufs[i] = strdup(buf);
                    break;
                case VAL_FLOAT:
                    snprintf(buf, sizeof(buf), "%g", v.as.floating);
                    temp_bufs[i] = strdup(buf);
                    break;
                case VAL_STRING:
                    temp_bufs[i] = strdup(v.as.string->chars);
                    break;
                case VAL_BOOL:
                    temp_bufs[i] = strdup(v.as.boolean ? "1" : "0");
                    break;
                default:
                    temp_bufs[i] = strdup("");
                    break;
            }
            cmd_argv[1 + i] = temp_bufs[i];
            cmd_argv_len[1 + i] = strlen(temp_bufs[i]);
        }
    }

    redisReply *reply = (redisReply *)redisCommandArgv(ctx, cmd_argc, cmd_argv, cmd_argv_len);

    if (extra_args > 0 && temp_bufs) {
        for (int i = 0; i < extra_args; i++) {
            if (temp_bufs[i]) free(temp_bufs[i]);
        }
        free(temp_bufs);
    }
    free(cmd_argv);
    free(cmd_argv_len);

    if (!reply) {
        runtime_error(vm, "redis.cmd() failed: %s", ctx->errstr);
        return val_nil();
    }

    Value result = redis_reply_to_value(vm, reply);
    freeReplyObject(reply);
    return result;
}

static Value lib_redis_close(VM *vm, int arg_count, Value *args) {
    bool is_dispatch = (arg_count >= 2 && args[0].type == VAL_MODULE);
    Value conn_val = is_dispatch ? args[1] : args[0];

    if (conn_val.type != VAL_INT) {
        runtime_error(vm, "redis.close() requires a connection handle (integer)");
        return val_nil();
    }
    redisContext *ctx = (redisContext *)(intptr_t)conn_val.as.integer;
    redisFree(ctx);
    return val_nil();
}

void lib_redis_init(VM *vm) {
    ObjModule *mod = new_module("redis");
    mod->obj.next = vm->objects;
    vm->objects = (Obj *)mod;
    define_global(vm, copy_string("redis", 5), val_module(mod));
    vm_register_dispatch(vm, "redis", "connect", val_native_fn((void *)lib_redis_connect));
    vm_register_dispatch(vm, "redis", "cmd",     val_native_fn((void *)lib_redis_cmd));
    vm_register_dispatch(vm, "redis", "close",   val_native_fn((void *)lib_redis_close));
}
