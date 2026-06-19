#define _GNU_SOURCE
#include "lib_python.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ═══════════════════════════════════════════
 *  Embedded Python receiver script
 * ═══════════════════════════════════════════ */

static const char PYTHON_RECEIVER[] =
    "import sys,json,importlib\n"
    "with open(sys.argv[1]) as f:\n"
    "  p=json.load(f)\n"
    "try:\n"
    "  m=importlib.import_module(p['m'])\n"
    "  r=getattr(m,p['f'])(*p['a'])\n"
    "  print(json.dumps({'ok':True,'r':r},default=str))\n"
    "except Exception as e:\n"
    "  print(json.dumps({'ok':False,'e':str(e)}))\n"
    ;

/* ═══════════════════════════════════════════
 *  python.run(module, function, args)
 * ═══════════════════════════════════════════ */

static Value lib_python_run(VM *vm, int arg_count, Value *args) {
    if (arg_count < 3) {
        runtime_error(vm, "python.run() requires 3 arguments: module, function, args");
        return val_nil();
    }
    if (args[0].type != VAL_STRING || args[1].type != VAL_STRING) {
        runtime_error(vm, "python.run(): module and function must be strings");
        return val_nil();
    }

    (void)args[0];
    (void)args[1];
    Value args_arr     = args[2];

    /* ── 1. Serialize payload to JSON ── */
    /* Build the request object as a Varian struct */
    char *field_names[3] = {"m", "f", "a"};
    Value field_values[3];
    field_values[0] = args[0];  /* module */
    field_values[1] = args[1];  /* function */
    field_values[2] = args_arr; /* args array */

    ObjStruct *req = (ObjStruct *)calloc(1, sizeof(ObjStruct));
    req->obj.type = VAL_STRUCT;
    req->field_count = 3;
    req->field_names = (char **)calloc(3, sizeof(char *));
    req->fields = (Value *)calloc(3, sizeof(Value));
    req->type_name = NULL;
    for (int i = 0; i < 3; i++) {
        req->field_names[i] = strdup(field_names[i]);
        req->fields[i] = field_values[i];
    }
    Value req_val = val_struct(req);

    int json_len;
    char *json_data = json_encode(vm, req_val, &json_len);
    free(req->field_names[0]); free(req->field_names[1]); free(req->field_names[2]);
    free(req->field_names); free(req->fields); free(req);
    if (!json_data) {
        runtime_error(vm, "python.run(): JSON encode failed");
        return val_nil();
    }

    /* ── 2. Write JSON to temp file ── */
    char tmpl_data[] = "/tmp/varian_py_XXXXXX";
    int fd = mkstemp(tmpl_data);
    if (fd < 0) {
        free(json_data);
        runtime_error(vm, "python.run(): cannot create temp file");
        return val_nil();
    }

    FILE *tmpf = fdopen(fd, "w");
    if (!tmpf) { close(fd); free(json_data); unlink(tmpl_data); return val_nil(); }
    /* Write only the JSON payload, not the null terminator */
    fwrite(json_data, 1, (size_t)json_len, tmpf);
    fclose(tmpf);
    free(json_data);

    /* ── 3. Write receiver script to temp file ── */
    char tmpl_script[] = "/tmp/varian_py_script_XXXXXX";
    fd = mkstemp(tmpl_script);
    if (fd < 0) { unlink(tmpl_data); return val_nil(); }

    tmpf = fdopen(fd, "w");
    if (!tmpf) { close(fd); unlink(tmpl_data); unlink(tmpl_script); return val_nil(); }
    fwrite(PYTHON_RECEIVER, 1, sizeof(PYTHON_RECEIVER) - 1, tmpf);
    fclose(tmpf);

    /* ── 4. Spawn Python subprocess ── */
    char cmd[2048];
    int n = snprintf(cmd, sizeof(cmd), "python3 %s %s", tmpl_script, tmpl_data);
    if (n >= (int)sizeof(cmd)) {
        unlink(tmpl_data); unlink(tmpl_script);
        runtime_error(vm, "python.run(): command too long");
        return val_nil();
    }

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        unlink(tmpl_data); unlink(tmpl_script);
        runtime_error(vm, "python.run(): popen failed");
        return val_nil();
    }

    /* ── 5. Read result from stdout ── */
    #define RESULT_BUF_SIZE 65536
    char *result_buf = (char *)malloc(RESULT_BUF_SIZE);
    if (!result_buf) { pclose(fp); unlink(tmpl_data); unlink(tmpl_script); return val_nil(); }
    size_t result_len = fread(result_buf, 1, RESULT_BUF_SIZE - 1, fp);
    result_buf[result_len] = '\0';
    int status = pclose(fp);

    /* Clean up temp files */
    unlink(tmpl_data);
    unlink(tmpl_script);

    /* ── 6. Parse result ── */
    if (status != 0 || result_len == 0) {
        free(result_buf);
        runtime_error(vm, "python.run(): subprocess failed (exit %d)", status);
        return val_nil();
    }

    /* Remove trailing newline if any */
    while (result_len > 0 && (result_buf[result_len-1] == '\n' ||
                              result_buf[result_len-1] == '\r'))
        result_buf[--result_len] = '\0';

    Value result = json_decode(vm, result_buf);
    free(result_buf);

    /* Check for error response */
    if (result.type == VAL_STRUCT) {
        ObjStruct *rs = result.as.structure;
        for (int i = 0; i < rs->field_count; i++) {
            if (strcmp(rs->field_names[i], "ok") == 0 &&
                rs->fields[i].type == VAL_BOOL &&
                !rs->fields[i].as.boolean) {
                /* Error: find "e" field */
                for (int j = 0; j < rs->field_count; j++) {
                    if (strcmp(rs->field_names[j], "e") == 0 &&
                        rs->fields[j].type == VAL_STRING) {
                        runtime_error(vm, "Python: %s", rs->fields[j].as.string->chars);
                        return val_nil();
                    }
                }
                runtime_error(vm, "Python: unknown error");
                return val_nil();
            }
        }
        /* Success: extract "r" field */
        for (int i = 0; i < rs->field_count; i++) {
            if (strcmp(rs->field_names[i], "r") == 0)
                return rs->fields[i];
        }
    }

    /* Fallback: return the whole struct */
    return result;
}

/* ═══════════════════════════════════════════
 *  Registration
 * ═══════════════════════════════════════════ */

void lib_python_init(VM *vm) {
    ObjModule *mod = new_module("python");
    mod->obj.next = vm->objects;
    vm->objects = (Obj *)mod;

    define_global(vm, copy_string("python", 6), val_module(mod));

    vm_register_dispatch(vm, "python", "run", val_native_fn((void *)lib_python_run));
}
