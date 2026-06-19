#include "pkg_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdbool.h>

/* ═══════════════════════════════════════════
 *  Minimal JSON parser for Python introspection output
 *
 *  Expects a JSON object of the form:
 *    { "func_name": ["param1", "param2"], ... }
 * ═══════════════════════════════════════════ */

typedef struct {
    char *name;
    char **params;
    int param_count;
} FuncInfo;

static void json_skip_ws(const char **p) {
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r')
        (*p)++;
}

static char *json_parse_string(const char **p) {
    json_skip_ws(p);
    if (**p != '"') return NULL;
    (*p)++; /* skip opening quote */
    const char *start = *p;
    int len = 0;
    while (**p && **p != '"') {
        if (**p == '\\') { (*p)++; if (**p) (*p)++; len++; }
        else { (*p)++; len++; }
    }
    if (**p != '"') return NULL;
    const char *end = *p;
    (*p)++; /* skip closing quote */

    /* Copy with unescaping */
    char *str = (char *)malloc((size_t)len + 1);
    int i = 0;
    const char *s = start;
    while (s < end) {
        if (*s == '\\') {
            s++;
            switch (*s) {
                case '"':  str[i++] = '"'; break;
                case '\\': str[i++] = '\\'; break;
                case '/':  str[i++] = '/'; break;
                case 'n':  str[i++] = '\n'; break;
                case 't':  str[i++] = '\t'; break;
                case 'r':  str[i++] = '\r'; break;
                default:   str[i++] = *s; break;
            }
        } else {
            str[i++] = *s;
        }
        s++;
    }
    str[i] = '\0';
    return str;
}

static int json_parse_array(const char **p, char ***out) {
    json_skip_ws(p);
    if (**p != '[') return -1;
    (*p)++; /* skip '[' */
    json_skip_ws(p);
    if (**p == ']') { (*p)++; *out = NULL; return 0; }

    int cap = 8, count = 0;
    *out = (char **)malloc((size_t)cap * sizeof(char *));
    while (1) {
        json_skip_ws(p);
        char *s = json_parse_string(p);
        if (!s) break;
        if (count >= cap) {
            cap *= 2;
            *out = (char **)realloc(*out, (size_t)cap * sizeof(char *));
        }
        (*out)[count++] = s;
        json_skip_ws(p);
        if (**p == ',') { (*p)++; continue; }
        if (**p == ']') { (*p)++; break; }
        break;
    }
    return count;
}

/* Parse top-level JSON object mapping function names to parameter arrays.
   Returns malloc'd array of FuncInfo, *count is set.
   Caller must free with free_funcs(). */
static FuncInfo *json_parse_funcs(const char *json, int *count) {
    *count = 0;
    const char *p = json;
    json_skip_ws(&p);
    if (*p != '{') return NULL;
    p++; /* skip '{' */
    json_skip_ws(&p);
    if (*p == '}') {
        FuncInfo *empty = (FuncInfo *)malloc(sizeof(FuncInfo));
        *count = 0;
        return empty;
    }

    int cap = 128, cnt = 0;
    FuncInfo *funcs = (FuncInfo *)malloc((size_t)cap * sizeof(FuncInfo));

    while (1) {
        json_skip_ws(&p);
        char *name = json_parse_string(&p);
        if (!name) break;
        json_skip_ws(&p);
        if (*p != ':') { free(name); break; }
        p++; /* skip ':' */
        json_skip_ws(&p);

        char **params = NULL;
        int param_count = json_parse_array(&p, &params);

        if (cnt >= cap) {
            cap *= 2;
            funcs = (FuncInfo *)realloc(funcs, (size_t)cap * sizeof(FuncInfo));
        }
        funcs[cnt].name = name;
        funcs[cnt].params = params;
        funcs[cnt].param_count = param_count >= 0 ? param_count : 0;
        cnt++;

        json_skip_ws(&p);
        if (*p == ',') { p++; continue; }
        if (*p == '}') { p++; break; }
        break;
    }

    *count = cnt;
    return funcs;
}

static void funcs_free(FuncInfo *funcs, int count) {
    if (!funcs) return;
    for (int i = 0; i < count; i++) {
        free(funcs[i].name);
        if (funcs[i].params) {
            for (int j = 0; j < funcs[i].param_count; j++)
                free(funcs[i].params[j]);
            free(funcs[i].params);
        }
    }
    free(funcs);
}

/* Check if a string is a valid Varian identifier */
static bool is_valid_ident(const char *s) {
    if (!s || !*s) return false;
    if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '_'))
        return false;
    for (s++; *s; s++) {
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
              (*s >= '0' && *s <= '9') || *s == '_'))
            return false;
    }
    return true;
}

/* Python introspection script that dumps JSON to stdout */
static const char *INTROSPECT_SCRIPT =
    "import sys, inspect, json\n"
    "try:\n"
    "    m = __import__(sys.argv[1])\n"
    "except Exception as e:\n"
    "    print(json.dumps({'error': str(e)}))\n"
    "    sys.exit(1)\n"
    "result = {}\n"
    "for name, obj in inspect.getmembers(m):\n"
    "    if inspect.isroutine(obj) or inspect.isbuiltin(obj):\n"
    "        try:\n"
    "            sig = inspect.signature(obj)\n"
    "            params = []\n"
    "            for p in sig.parameters.values():\n"
    "                if str(p) == '*': continue\n"
    "                if p.kind == inspect.Parameter.VAR_KEYWORD: continue\n"
    "                params.append(str(p))\n"
    "            result[name] = params\n"
    "        except (ValueError, TypeError):\n"
    "            result[name] = []\n"
    "print(json.dumps(result))\n";

/* ═══════════════════════════════════════════
 *  pkg_add — local dependency scaffolding
 * ═══════════════════════════════════════════ */

int pkg_add(const char *pkg_name) {
    /* 1. Ensure vn_modules/ exists */
    mkdir("vn_modules", 0755);

    /* 2. Read or create varian.pkg */
    FILE *f = fopen("varian.pkg", "r");
    if (!f) {
        /* Create new file with [deps] header */
        f = fopen("varian.pkg", "w");
        if (!f) {
            fprintf(stderr, "error: could not create varian.pkg\n");
            return 1;
        }
        fprintf(f, "[deps]\n");
        fprintf(f, "%s = \"latest\"\n", pkg_name);
        fclose(f);
        printf("  created varian.pkg\n");
        printf("  added %s = \"latest\"\n", pkg_name);
        return 0;
    }
    fclose(f);

    /* 3. Append dependency */
    f = fopen("varian.pkg", "a");
    if (!f) {
        fprintf(stderr, "error: could not open varian.pkg\n");
        return 1;
    }
    fprintf(f, "%s = \"latest\"\n", pkg_name);
    fclose(f);
    printf("  added %s = \"latest\" to varian.pkg\n", pkg_name);
    return 0;
}

/* ═══════════════════════════════════════════
 *  pkg_wrap — generate Varian wrapper for
 *  foreign libraries (e.g. python:math)
 * ═══════════════════════════════════════════ */

int pkg_wrap(const char *target) {
    /* Parse target — only "python:<module>" for now */
    const char *prefix = "python:";
    size_t plen = strlen(prefix);

    if (strncmp(target, prefix, plen) != 0) {
        fprintf(stderr, "error: unsupported target '%s'.\n", target);
        fprintf(stderr, "  supported: python:<module>\n");
        return 1;
    }

    const char *mod_name = target + plen;
    if (*mod_name == '\0') {
        fprintf(stderr, "error: missing module name. Use 'python:<module>'.\n");
        return 1;
    }

    printf("  introspecting python module '%s'...\n", mod_name);

    /* ── 1. Write introspection script to temp file ── */
    char tmpfile[] = "/tmp/varian_wrap_XXXXXX";
    int fd = mkstemp(tmpfile);
    if (fd < 0) {
        fprintf(stderr, "error: could not create temp file\n");
        return 1;
    }
    size_t slen = strlen(INTROSPECT_SCRIPT);
    if (write(fd, INTROSPECT_SCRIPT, slen) != (ssize_t)slen) {
        close(fd);
        unlink(tmpfile);
        fprintf(stderr, "error: could not write temp script\n");
        return 1;
    }
    close(fd);

    /* ── 2. Run python3 with the script ── */
    char cmd[8192];
    int n = snprintf(cmd, sizeof(cmd), "python3 %s %s", tmpfile, mod_name);
    if (n >= (int)sizeof(cmd)) {
        unlink(tmpfile);
        fprintf(stderr, "error: command too long\n");
        return 1;
    }

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        unlink(tmpfile);
        fprintf(stderr, "error: could not run python3\n");
        return 1;
    }

    /* Read stdout */
    char output[65536];
    size_t total = 0;
    char buf[4096];
    size_t nread;
    while ((nread = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (total + nread < sizeof(output)) {
            memcpy(output + total, buf, nread);
            total += nread;
        } else {
            break;
        }
    }
    output[total] = '\0';

    int status = pclose(fp);
    unlink(tmpfile);

    if (status != 0) {
        fprintf(stderr, "error: python introspection failed:\n%s\n", output);
        return 1;
    }

    /* ── 3. Parse JSON function list ── */
    int func_count = 0;
    FuncInfo *funcs = json_parse_funcs(output, &func_count);
    if (!funcs) {
        fprintf(stderr, "error: could not parse introspection output\n");
        return 1;
    }
    if (func_count == 0) {
        fprintf(stderr, "warning: no callable functions found in '%s'\n", mod_name);
        funcs_free(funcs, func_count);
        return 0;
    }

    /* ── 4. Create vn_modules/ and write wrapper ── */
    mkdir("vn_modules", 0755);

    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "vn_modules/%s.vn", mod_name);

    FILE *out = fopen(filepath, "w");
    if (!out) {
        fprintf(stderr, "error: could not create '%s'\n", filepath);
        funcs_free(funcs, func_count);
        return 1;
    }

    fprintf(out, "// Auto-generated wrapper for python:%s\n", mod_name);
    fprintf(out, "// Generated by 'vn wrap python:%s'\n", mod_name);
    fprintf(out, "\n");

    for (int i = 0; i < func_count; i++) {
        fprintf(out, "fn %s(", funcs[i].name);

        /* Emit parameters */
        for (int j = 0; j < funcs[i].param_count; j++) {
            if (j > 0) fprintf(out, ", ");

            /* Use clean param name or fallback to p0, p1, ... */
            const char *pn = funcs[i].params[j];
            if (is_valid_ident(pn)) {
                fprintf(out, "%s", pn);
            } else {
                fprintf(out, "p%d", j);
            }
            fprintf(out, ": any");
        }

        fprintf(out, ") -> any {\n");
        fprintf(out, "    return python.run(\"%s\", \"%s\", [", mod_name, funcs[i].name);

        /* Pass arguments as array */
        for (int j = 0; j < funcs[i].param_count; j++) {
            if (j > 0) fprintf(out, ", ");
            const char *pn = funcs[i].params[j];
            if (is_valid_ident(pn)) {
                fprintf(out, "%s", pn);
            } else {
                fprintf(out, "p%d", j);
            }
        }

        fprintf(out, "])\n");
        fprintf(out, "}\n\n");
    }

    fclose(out);
    printf("  generated %s (%d functions)\n", filepath, func_count);

    funcs_free(funcs, func_count);
    return 0;
}
