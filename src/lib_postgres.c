#include "lib_postgres.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <libpq-fe.h>

static Value lib_postgres_connect(VM *vm, int arg_count, Value *args) {
    if (arg_count < 1 || args[0].type != VAL_STRING) {
        runtime_error(vm, "postgres.connect() requires a connection string");
        return val_nil();
    }
    const char *conn_str = args[0].as.string->chars;
    PGconn *conn = PQconnectdb(conn_str);
    if (PQstatus(conn) != CONNECTION_OK) {
        runtime_error(vm, "postgres.connect() failed: %s", PQerrorMessage(conn));
        PQfinish(conn);
        return val_nil();
    }
    return val_int((int64_t)(intptr_t)conn);
}

static Value lib_postgres_query(VM *vm, int arg_count, Value *args) {
    if (arg_count < 2) {
        runtime_error(vm, "postgres.query() requires at least 2 arguments: conn, sql [, args]");
        return val_nil();
    }
    if (args[0].type != VAL_INT) {
        runtime_error(vm, "postgres.query(): conn must be an integer (connection handle)");
        return val_nil();
    }
    if (args[1].type != VAL_STRING) {
        runtime_error(vm, "postgres.query(): sql must be a string");
        return val_nil();
    }

    PGconn *conn = (PGconn *)(intptr_t)args[0].as.integer;
    const char *sql = args[1].as.string->chars;

    int nParams = 0;
    const char **paramValues = NULL;
    /* temporary storage for stringified param values (owned by us) */
    char **paramBufs = NULL;

    if (arg_count >= 3 && args[2].type == VAL_ARRAY) {
        ObjArray *arr = args[2].as.array;
        nParams = arr->count;
        if (nParams > 0) {
            paramValues = (const char **)calloc(nParams, sizeof(char *));
            paramBufs = (char **)calloc(nParams, sizeof(char *));
            if (!paramValues || !paramBufs) {
                free((void *)paramValues);
                free(paramBufs);
                runtime_error(vm, "postgres.query(): out of memory");
                return val_nil();
            }
            for (int i = 0; i < nParams; i++) {
                Value v = arr->elements[i];
                char buf[128];
                switch (v.type) {
                    case VAL_NIL:
                        paramValues[i] = NULL;
                        paramBufs[i] = NULL;
                        break;
                    case VAL_INT:
                        snprintf(buf, sizeof(buf), "%" PRId64, v.as.integer);
                        paramBufs[i] = strdup(buf);
                        paramValues[i] = paramBufs[i];
                        break;
                    case VAL_FLOAT:
                        snprintf(buf, sizeof(buf), "%g", v.as.floating);
                        paramBufs[i] = strdup(buf);
                        paramValues[i] = paramBufs[i];
                        break;
                    case VAL_STRING:
                        paramBufs[i] = strdup(v.as.string->chars);
                        paramValues[i] = paramBufs[i];
                        break;
                    case VAL_BOOL:
                        paramBufs[i] = strdup(v.as.boolean ? "true" : "false");
                        paramValues[i] = paramBufs[i];
                        break;
                    default: {
                        char tmp[64];
                        snprintf(tmp, sizeof(tmp), "<param %d>", i);
                        paramBufs[i] = strdup(tmp);
                        paramValues[i] = paramBufs[i];
                        break;
                    }
                }
            }
        }
    }

    PGresult *res = PQexecParams(conn, sql, nParams, NULL, paramValues, NULL, NULL, 0);

    for (int i = 0; i < nParams; i++) {
        if (paramBufs && paramBufs[i]) free(paramBufs[i]);
    }
    free((void *)paramValues);
    free(paramBufs);

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        const char *err = res ? PQresultErrorMessage(res) : "unknown error";
        runtime_error(vm, "postgres.query(): %s", err);
        if (res) PQclear(res);
        return val_nil();
    }

    int nRows = PQntuples(res);
    int nCols = PQnfields(res);

    ObjArray *result_arr = (ObjArray *)calloc(1, sizeof(ObjArray));
    result_arr->obj.type = VAL_ARRAY;
    result_arr->obj.next = vm->objects;
    vm->objects = (Obj *)result_arr;
    result_arr->count = nRows;
    result_arr->capacity = nRows;
    result_arr->elements = (Value *)calloc(nRows, sizeof(Value));

    for (int r = 0; r < nRows; r++) {
        ObjStruct *s = (ObjStruct *)calloc(1, sizeof(ObjStruct));
        s->obj.type = VAL_STRUCT;
        s->obj.next = vm->objects;
        vm->objects = (Obj *)s;
        s->field_count = nCols;
        s->field_names = (char **)calloc(nCols, sizeof(char *));
        s->fields = (Value *)calloc(nCols, sizeof(Value));
        s->type_name = NULL;

        for (int c = 0; c < nCols; c++) {
            s->field_names[c] = strdup(PQfname(res, c));
            if (PQgetisnull(res, r, c)) {
                s->fields[c] = val_nil();
            } else {
                const char *val = PQgetvalue(res, r, c);
                ObjString *str = allocate_string(vm, val, (int)strlen(val));
                s->fields[c] = val_string(str);
            }
        }
        result_arr->elements[r] = val_struct(s);
    }

    PQclear(res);
    return val_array(result_arr);
}

static Value lib_postgres_close(VM *vm, int arg_count, Value *args) {
    (void)vm;
    if (arg_count < 1 || args[0].type != VAL_INT) {
        runtime_error(vm, "postgres.close() requires a connection handle (integer)");
        return val_nil();
    }
    PGconn *conn = (PGconn *)(intptr_t)args[0].as.integer;
    PQfinish(conn);
    return val_nil();
}

void lib_postgres_init(VM *vm) {
    ObjModule *mod = new_module("postgres");
    mod->obj.next = vm->objects;
    vm->objects = (Obj *)mod;
    define_global(vm, copy_string("postgres", 8), val_module(mod));
    vm_register_dispatch(vm, "postgres", "connect", val_native_fn((void *)lib_postgres_connect));
    vm_register_dispatch(vm, "postgres", "query", val_native_fn((void *)lib_postgres_query));
    vm_register_dispatch(vm, "postgres", "close", val_native_fn((void *)lib_postgres_close));
}
