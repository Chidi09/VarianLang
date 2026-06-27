#include "lib_sqlite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sqlite3.h>

// Process-global deduped set of changed table names from sqlite3_update_hook
// TODO: guard with a mutex if the VM ever becomes multi-threaded
static char **g_changed_tables = NULL;
static int g_changed_count = 0;
static int g_changed_cap = 0;

static void _sqlite_change_cb(void *ctx, int op, const char *dbname, const char *table, sqlite3_int64 rowid) {
    (void)ctx; (void)op; (void)dbname; (void)rowid;
    if (!table || strncmp(table, "sqlite_", 7) == 0) return;
    for (int i = 0; i < g_changed_count; i++) {
        if (strcmp(g_changed_tables[i], table) == 0) return;
    }
    if (g_changed_count >= g_changed_cap) {
        int new_cap = g_changed_cap ? g_changed_cap * 2 : 8;
        g_changed_tables = realloc(g_changed_tables, sizeof(char *) * (size_t)new_cap);
        g_changed_cap = new_cap;
    }
    g_changed_tables[g_changed_count++] = strdup(table);
}

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
    sqlite3_update_hook(db, _sqlite_change_cb, NULL);
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
    /* result_arr and each in-progress row struct are rooted on the current
     * task's stack for the whole loop below: neither is reachable any other
     * way (not returned yet, and the old plain-malloc `elements` staging
     * buffer was never a GC root at all) while allocate_string() per column
     * can trigger a GC cycle -- which would otherwise free rows already
     * built (or the row currently being filled in) out from under this very
     * loop. Reproduced as a real heap-use-after-free crash under load, not
     * a theoretical concern -- see the matching fix in lib_validate.c. */
    Task *self = vm->current_task;
    ObjArray *result_arr = (ObjArray *)calloc(1, sizeof(ObjArray));
    result_arr->obj.type = VAL_ARRAY;
    result_arr->obj.next = vm->objects;
    vm->objects = (Obj *)result_arr;
    result_arr->capacity = 8;
    result_arr->elements = malloc(sizeof(Value) * (size_t)result_arr->capacity);
    self->stack[self->stack_top++] = val_array(result_arr);

    int rc = sqlite3_step(stmt);
    int cols = sqlite3_column_count(stmt);

    while (rc == SQLITE_ROW) {
        ObjStruct *s = new_struct(vm, cols, false);
        s->type_name = NULL;
        self->stack[self->stack_top++] = val_struct(s);

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
        self->stack_top--; /* pop s -- it's about to be rooted via result_arr instead */

        if (result_arr->count >= result_arr->capacity) {
            result_arr->capacity *= 2;
            result_arr->elements = realloc(result_arr->elements, sizeof(Value) * (size_t)result_arr->capacity);
        }
        result_arr->elements[result_arr->count++] = val_struct(s);
        rc = sqlite3_step(stmt);
    }

    sqlite3_finalize(stmt);
    self->stack_top--; /* pop result_arr -- about to be returned (and re-pushed by the caller) */

    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        runtime_error(vm, "sqlite.query() step failed: %s", sqlite3_errmsg(db));
        return val_nil();
    }

    return val_array(result_arr);
}

static Value lib_sqlite_drain_changes(VM *vm, int arg_count, Value *args) {
    (void)arg_count; (void)args;
    Task *self = vm->current_task;
    ObjArray *arr = (ObjArray *)calloc(1, sizeof(ObjArray));
    arr->obj.type = VAL_ARRAY;
    arr->obj.next = vm->objects;
    vm->objects = (Obj *)arr;
    arr->capacity = g_changed_count > 0 ? g_changed_count : 1;
    arr->elements = malloc(sizeof(Value) * (size_t)arr->capacity);
    arr->count = 0;
    self->stack[self->stack_top++] = val_array(arr);

    for (int i = 0; i < g_changed_count; i++) {
        ObjString *s = allocate_string(vm, g_changed_tables[i], (int)strlen(g_changed_tables[i]));
        if (arr->count >= arr->capacity) {
            arr->capacity *= 2;
            arr->elements = realloc(arr->elements, sizeof(Value) * (size_t)arr->capacity);
        }
        arr->elements[arr->count++] = val_string(s);
        free(g_changed_tables[i]);
        g_changed_tables[i] = NULL;
    }
    g_changed_count = 0;

    self->stack_top--;
    return val_array(arr);
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
    vm_register_dispatch(vm, "sqlite", "connect",       val_native_fn((void *)lib_sqlite_connect));
    vm_register_dispatch(vm, "sqlite", "query",         val_native_fn((void *)lib_sqlite_query));
    vm_register_dispatch(vm, "sqlite", "close",         val_native_fn((void *)lib_sqlite_close));
    vm_register_dispatch(vm, "sqlite", "drain_changes", val_native_fn((void *)lib_sqlite_drain_changes));
}
