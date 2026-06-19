#include "lib_sqlite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sqlite3.h>

static Value lib_sqlite_connect(VM *vm, int arg_count, Value *args) {
    bool is_dispatch = (arg_count >= 2 && args[0].type == VAL_MODULE);
    Value path_val = is_dispatch ? args[1] : args[0];

    if ((is_dispatch && arg_count < 2) || (!is_dispatch && arg_count < 1) || path_val.type != VAL_STRING) {
        runtime_error(vm, "sqlite.connect() requires a database path string");
        return val_nil();
    }
    const char *path = path_val.as.string->chars;
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        runtime_error(vm, "sqlite.connect() failed: %s", sqlite3_errmsg(db));
        sqlite3_close(db);
        return val_nil();
    }
    return val_int((int64_t)(intptr_t)db);
}

static Value lib_sqlite_query(VM *vm, int arg_count, Value *args) {
    bool is_dispatch = (arg_count >= 3 && args[0].type == VAL_MODULE);
    Value conn_val = is_dispatch ? args[1] : args[0];
    Value sql_val = is_dispatch ? args[2] : args[1];
    Value params_val = is_dispatch ? (arg_count >= 4 ? args[3] : val_nil()) : (arg_count >= 3 ? args[2] : val_nil());

    if (conn_val.type != VAL_INT) {
        runtime_error(vm, "sqlite.query(): conn must be an integer (database handle)");
        return val_nil();
    }
    if (sql_val.type != VAL_STRING) {
        runtime_error(vm, "sqlite.query(): sql must be a string");
        return val_nil();
    }

    sqlite3 *db = (sqlite3 *)(intptr_t)conn_val.as.integer;
    const char *sql = sql_val.as.string->chars;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        runtime_error(vm, "sqlite.query() prepare failed: %s", sqlite3_errmsg(db));
        return val_nil();
    }

    // Bind parameters if array is provided
    if (params_val.type == VAL_ARRAY) {
        ObjArray *arr = params_val.as.array;
        for (int i = 0; i < arr->count; i++) {
            Value v = arr->elements[i];
            int bind_idx = i + 1;
            int rc = SQLITE_OK;
            switch (v.type) {
                case VAL_NIL:
                    rc = sqlite3_bind_null(stmt, bind_idx);
                    break;
                case VAL_INT:
                    rc = sqlite3_bind_int64(stmt, bind_idx, v.as.integer);
                    break;
                case VAL_FLOAT:
                    rc = sqlite3_bind_double(stmt, bind_idx, v.as.floating);
                    break;
                case VAL_STRING:
                    rc = sqlite3_bind_text(stmt, bind_idx, v.as.string->chars, -1, SQLITE_TRANSIENT);
                    break;
                case VAL_BOOL:
                    rc = sqlite3_bind_int(stmt, bind_idx, v.as.boolean ? 1 : 0);
                    break;
                default:
                    rc = sqlite3_bind_null(stmt, bind_idx);
                    break;
            }
            if (rc != SQLITE_OK) {
                runtime_error(vm, "sqlite.query() bind failed at index %d: %s", i, sqlite3_errmsg(db));
                sqlite3_finalize(stmt);
                return val_nil();
            }
        }
    }

    // Fetch rows
    int capacity = 8;
    int count = 0;
    Value *elements = malloc(sizeof(Value) * (size_t)capacity);

    int rc = sqlite3_step(stmt);
    int cols = sqlite3_column_count(stmt);

    while (rc == SQLITE_ROW) {
        ObjStruct *s = new_struct(cols);
        s->obj.next = vm->objects;
        vm->objects = (Obj *)s;
        s->type_name = NULL;

        for (int c = 0; c < cols; c++) {
            s->field_names[c] = strdup(sqlite3_column_name(stmt, c));
            int type = sqlite3_column_type(stmt, c);
            if (type == SQLITE_NULL) {
                s->fields[c] = val_nil();
            } else if (type == SQLITE_INTEGER) {
                s->fields[c] = val_int(sqlite3_column_int64(stmt, c));
            } else if (type == SQLITE_FLOAT) {
                s->fields[c] = val_float(sqlite3_column_double(stmt, c));
            } else {
                const char *val = (const char *)sqlite3_column_text(stmt, c);
                ObjString *str = allocate_string(vm, val ? val : "", val ? (int)strlen(val) : 0);
                s->fields[c] = val_string(str);
            }
        }

        if (count >= capacity) {
            capacity *= 2;
            elements = realloc(elements, sizeof(Value) * (size_t)capacity);
        }
        elements[count++] = val_struct(s);
        rc = sqlite3_step(stmt);
    }

    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        runtime_error(vm, "sqlite.query() step failed: %s", sqlite3_errmsg(db));
        free(elements);
        return val_nil();
    }

    ObjArray *result_arr = (ObjArray *)calloc(1, sizeof(ObjArray));
    result_arr->obj.type = VAL_ARRAY;
    result_arr->obj.next = vm->objects;
    vm->objects = (Obj *)result_arr;
    result_arr->count = count;
    result_arr->capacity = capacity;
    result_arr->elements = elements;

    return val_array(result_arr);
}

static Value lib_sqlite_close(VM *vm, int arg_count, Value *args) {
    bool is_dispatch = (arg_count >= 2 && args[0].type == VAL_MODULE);
    Value conn_val = is_dispatch ? args[1] : args[0];

    if (conn_val.type != VAL_INT) {
        runtime_error(vm, "sqlite.close() requires a database handle (integer)");
        return val_nil();
    }
    sqlite3 *db = (sqlite3 *)(intptr_t)conn_val.as.integer;
    sqlite3_close(db);
    return val_nil();
}

void lib_sqlite_init(VM *vm) {
    ObjModule *mod = new_module("sqlite");
    mod->obj.next = vm->objects;
    vm->objects = (Obj *)mod;
    define_global(vm, copy_string("sqlite", 6), val_module(mod));
    vm_register_dispatch(vm, "sqlite", "connect", val_native_fn((void *)lib_sqlite_connect));
    vm_register_dispatch(vm, "sqlite", "query",   val_native_fn((void *)lib_sqlite_query));
    vm_register_dispatch(vm, "sqlite", "close",   val_native_fn((void *)lib_sqlite_close));
}
