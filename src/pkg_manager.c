#include "pkg_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdbool.h>
#include <dirent.h>

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
 *  pkg_add & manifest parser
 * ═══════════════════════════════════════════ */

static void trim_str(char *out, const char *in, int max_len) {
    while (*in == ' ' || *in == '\t' || *in == '\r' || *in == '\n' || *in == '"' || *in == '\'') in++;
    int len = (int)strlen(in);
    while (len > 0 && (in[len - 1] == ' ' || in[len - 1] == '\t' || in[len - 1] == '\r' || in[len - 1] == '\n' || in[len - 1] == '"' || in[len - 1] == '\'')) {
        len--;
    }
    if (len >= max_len) len = max_len - 1;
    memcpy(out, in, (size_t)len);
    out[len] = '\0';
}

static void get_dir_name(char *out, int max_len) {
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd))) {
        char *last = strrchr(cwd, '/');
        if (last && *(last + 1) != '\0') {
            strncpy(out, last + 1, (size_t)max_len - 1);
            out[max_len - 1] = '\0';
            return;
        }
    }
    strncpy(out, "app", (size_t)max_len - 1);
    out[max_len - 1] = '\0';
}

bool pkg_manifest_load(ConstellationManifest *manifest, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    
    memset(manifest, 0, sizeof(ConstellationManifest));
    
    enum { SEC_NONE, SEC_PACKAGE, SEC_DEPS, SEC_CAPS, SEC_BUILD } sec = SEC_NONE;
    char line[1024];
    
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        
        char *start = line;
        while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') start++;
        if (*start == '\0') continue;
        
        if (*start == '[' && start[strlen(start) - 1] == '\n') {
            int slen = (int)strlen(start);
            while (slen > 0 && (start[slen - 1] == '\n' || start[slen - 1] == '\r' || start[slen - 1] == ' ' || start[slen - 1] == '\t' || start[slen - 1] == ']')) {
                start[--slen] = '\0';
            }
            if (start[0] == '[') start++;
            
            if (strcmp(start, "package") == 0) sec = SEC_PACKAGE;
            else if (strcmp(start, "deps") == 0) sec = SEC_DEPS;
            else if (strcmp(start, "capabilities") == 0) sec = SEC_CAPS;
            else if (strcmp(start, "build") == 0) sec = SEC_BUILD;
            else sec = SEC_NONE;
            continue;
        }
        
        if (*start == '[') {
            char *end_b = strchr(start, ']');
            if (end_b) {
                *end_b = '\0';
                start++;
                if (strcmp(start, "package") == 0) sec = SEC_PACKAGE;
                else if (strcmp(start, "deps") == 0) sec = SEC_DEPS;
                else if (strcmp(start, "capabilities") == 0) sec = SEC_CAPS;
                else if (strcmp(start, "build") == 0) sec = SEC_BUILD;
                else sec = SEC_NONE;
                continue;
            }
        }
        
        char *eq = strchr(start, '=');
        if (eq) {
            *eq = '\0';
            char key[128];
            char val[512];
            trim_str(key, start, sizeof(key));
            trim_str(val, eq + 1, sizeof(val));
            
            if (sec == SEC_PACKAGE) {
                if (strcmp(key, "name") == 0) strncpy(manifest->name, val, sizeof(manifest->name) - 1);
                else if (strcmp(key, "version") == 0) strncpy(manifest->version, val, sizeof(manifest->version) - 1);
                else if (strcmp(key, "kind") == 0) {
                    if (strcmp(val, "aurora") == 0) manifest->kind = MANIFEST_KIND_AURORA;
                    else if (strcmp(val, "lumen") == 0) manifest->kind = MANIFEST_KIND_LUMEN;
                    else if (strcmp(val, "zenith") == 0) manifest->kind = MANIFEST_KIND_ZENITH;
                    else manifest->kind = MANIFEST_KIND_UNSET;
                }
            } else if (sec == SEC_DEPS) {
                if (manifest->dep_count < 64) {
                    int i = manifest->dep_count++;
                    strncpy(manifest->deps[i].name, key, sizeof(manifest->deps[i].name) - 1);
                    strncpy(manifest->deps[i].val, val, sizeof(manifest->deps[i].val) - 1);
                    
                    if (val[0] == '{') {
                        manifest->deps[i].is_git = true;
                        char *git_pos = strstr(val, "git");
                        if (git_pos) {
                            char *gval = strchr(git_pos, '=');
                            if (gval) {
                                gval++;
                                while (*gval == ' ' || *gval == '\t' || *gval == '"' || *gval == '\'') gval++;
                                char *gend = gval;
                                while (*gend && *gend != '"' && *gend != '\'' && *gend != ',' && *gend != '}') gend++;
                                size_t len = (size_t)(gend - gval);
                                if (len > sizeof(manifest->deps[i].git_url) - 1) len = sizeof(manifest->deps[i].git_url) - 1;
                                memcpy(manifest->deps[i].git_url, gval, len);
                                manifest->deps[i].git_url[len] = '\0';
                            }
                        }
                        char *tag_pos = strstr(val, "tag");
                        if (tag_pos) {
                            char *tval = strchr(tag_pos, '=');
                            if (tval) {
                                tval++;
                                while (*tval == ' ' || *tval == '\t' || *tval == '"' || *tval == '\'') tval++;
                                char *tend = tval;
                                while (*tend && *tend != '"' && *tend != '\'' && *tend != ',' && *tend != '}') tend++;
                                size_t len = (size_t)(tend - tval);
                                if (len > sizeof(manifest->deps[i].git_tag) - 1) len = sizeof(manifest->deps[i].git_tag) - 1;
                                memcpy(manifest->deps[i].git_tag, tval, len);
                                manifest->deps[i].git_tag[len] = '\0';
                            }
                        }
                    } else {
                        manifest->deps[i].is_git = false;
                    }
                }
            } else if (sec == SEC_CAPS) {
                bool bval = (strcmp(val, "true") == 0);
                if (strcmp(key, "ffi") == 0) manifest->cap_ffi = bval;
                else if (strcmp(key, "python") == 0) manifest->cap_python = bval;
                else if (strcmp(key, "net") == 0) manifest->cap_net = bval;
                else if (strcmp(key, "fs") == 0) manifest->cap_fs = bval;
            } else if (sec == SEC_BUILD) {
                if (strcmp(key, "script") == 0) strncpy(manifest->build_script, val, sizeof(manifest->build_script) - 1);
            }
        }
    }
    
    fclose(f);
    return true;
}

bool pkg_manifest_save(const ConstellationManifest *manifest, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return false;
    
    fprintf(f, "[package]\n");
    fprintf(f, "name = \"%s\"\n", manifest->name);
    fprintf(f, "version = \"%s\"\n", manifest->version);
    if (manifest->kind == MANIFEST_KIND_AURORA) fprintf(f, "kind = \"aurora\"\n");
    else if (manifest->kind == MANIFEST_KIND_LUMEN) fprintf(f, "kind = \"lumen\"\n");
    else if (manifest->kind == MANIFEST_KIND_ZENITH) fprintf(f, "kind = \"zenith\"\n");
    fprintf(f, "\n");
    
    fprintf(f, "[deps]\n");
    for (int i = 0; i < manifest->dep_count; i++) {
        if (manifest->deps[i].is_git) {
            fprintf(f, "%s = { git = \"%s\", tag = \"%s\" }\n",
                    manifest->deps[i].name, manifest->deps[i].git_url, manifest->deps[i].git_tag);
        } else {
            fprintf(f, "%s = \"%s\"\n", manifest->deps[i].name, manifest->deps[i].val);
        }
    }
    fprintf(f, "\n");
    
    fprintf(f, "[capabilities]\n");
    fprintf(f, "ffi = %s\n", manifest->cap_ffi ? "true" : "false");
    fprintf(f, "python = %s\n", manifest->cap_python ? "true" : "false");
    fprintf(f, "net = %s\n", manifest->cap_net ? "true" : "false");
    fprintf(f, "fs = %s\n\n", manifest->cap_fs ? "true" : "false");
    
    if (manifest->build_script[0] != '\0') {
        fprintf(f, "[build]\n");
        fprintf(f, "script = \"%s\"\n", manifest->build_script);
    }
    
    fclose(f);
    return true;
}

int pkg_migrate_manifest(void) {
    FILE *f = fopen("varian.pkg", "r");
    if (!f) return 0; // nothing to migrate
    
    ConstellationManifest manifest;
    memset(&manifest, 0, sizeof(manifest));
    get_dir_name(manifest.name, sizeof(manifest.name));
    strcpy(manifest.version, "0.1.0");
    
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *start = line;
        while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') start++;
        if (*start == '\0' || *start == '#' || *start == '[') continue;
        
        char *eq = strchr(start, '=');
        if (eq) {
            *eq = '\0';
            char key[64];
            char val[128];
            trim_str(key, start, sizeof(key));
            trim_str(val, eq + 1, sizeof(val));
            if (manifest.dep_count < 64) {
                int i = manifest.dep_count++;
                strcpy(manifest.deps[i].name, key);
                strcpy(manifest.deps[i].val, val);
                manifest.deps[i].is_git = false;
            }
        }
    }
    fclose(f);
    
    if (pkg_manifest_save(&manifest, "constellation.toml")) {
        remove("varian.pkg");
        printf("  migrated varian.pkg -> constellation.toml\n");
        return 1;
    }
    return -1;
}

/* Parse a `vn add` spec into a dependency. Forms:
 *   foo                       -> registry, "latest"
 *   foo@1.2.0  /  foo@^1.2.0  -> registry, that version range
 *   github.com/u/foo          -> git dep (tag "main"), name = "foo"
 *   github.com/u/foo@v1.2.0   -> git dep at tag v1.2.0, name = "foo"
 * A '/' in the base (before any '@') marks it as a git URL. */
static void parse_add_spec(const char *spec, char *name, char *val,
                           bool *is_git, char *git_url, char *git_tag) {
    name[0] = val[0] = git_url[0] = git_tag[0] = '\0';
    *is_git = false;

    char base[512];
    const char *at = strrchr(spec, '@');
    /* Ignore a '@' that's part of a scheme like git@github.com (no '/' after). */
    if (at && strchr(at, '/')) at = NULL;
    const char *ver = NULL;
    if (at) {
        size_t bl = (size_t)(at - spec);
        if (bl >= sizeof(base)) bl = sizeof(base) - 1;
        memcpy(base, spec, bl); base[bl] = '\0';
        ver = at + 1;
    } else {
        strncpy(base, spec, sizeof(base) - 1); base[sizeof(base) - 1] = '\0';
    }

    if (strchr(base, '/')) {
        *is_git = true;
        const char *u = base;
        const char *scheme = strstr(u, "://");
        if (scheme) u = scheme + 3;
        strncpy(git_url, u, 255); git_url[255] = '\0';
        /* strip a trailing ".git" */
        size_t ul = strlen(git_url);
        if (ul > 4 && strcmp(git_url + ul - 4, ".git") == 0) git_url[ul - 4] = '\0';
        /* name = last path segment */
        const char *slash = strrchr(git_url, '/');
        strncpy(name, slash ? slash + 1 : git_url, 63); name[63] = '\0';
        strncpy(git_tag, ver ? ver : "main", 63); git_tag[63] = '\0';
        snprintf(val, 256, "{ git = \"%s\", tag = \"%s\" }", git_url, git_tag);
    } else {
        strncpy(name, base, 63); name[63] = '\0';
        strncpy(val, ver ? ver : "latest", 255); val[255] = '\0';
    }
}

int pkg_add(const char *pkg_name) {
    pkg_migrate_manifest();

    ConstellationManifest manifest;
    FILE *toml = fopen("constellation.toml", "r");
    if (!toml) {
        memset(&manifest, 0, sizeof(manifest));
        get_dir_name(manifest.name, sizeof(manifest.name));
        strcpy(manifest.version, "0.1.0");
        printf("  initialized constellation.toml\n");
    } else {
        fclose(toml);
        if (!pkg_manifest_load(&manifest, "constellation.toml")) {
            fprintf(stderr, "error: could not load constellation.toml\n");
            return 1;
        }
    }

    char name[64], val[256], git_url[256], git_tag[64];
    bool is_git;
    parse_add_spec(pkg_name, name, val, &is_git, git_url, git_tag);
    if (name[0] == '\0') {
        fprintf(stderr, "error: could not parse package spec '%s'\n", pkg_name);
        return 1;
    }

    /* Update in place if already present, else append. */
    int idx = -1;
    for (int i = 0; i < manifest.dep_count; i++) {
        if (strcmp(manifest.deps[i].name, name) == 0) { idx = i; break; }
    }
    if (idx < 0) {
        if (manifest.dep_count >= 64) {
            fprintf(stderr, "error: max dependency limit (64) reached\n");
            return 1;
        }
        idx = manifest.dep_count++;
    }
    strncpy(manifest.deps[idx].name, name, sizeof(manifest.deps[idx].name) - 1);
    manifest.deps[idx].name[sizeof(manifest.deps[idx].name) - 1] = '\0';
    manifest.deps[idx].is_git = is_git;
    if (is_git) {
        strncpy(manifest.deps[idx].git_url, git_url, sizeof(manifest.deps[idx].git_url) - 1);
        strncpy(manifest.deps[idx].git_tag, git_tag, sizeof(manifest.deps[idx].git_tag) - 1);
        strncpy(manifest.deps[idx].val, val, sizeof(manifest.deps[idx].val) - 1);
    } else {
        strncpy(manifest.deps[idx].val, val, sizeof(manifest.deps[idx].val) - 1);
    }

    if (!pkg_manifest_save(&manifest, "constellation.toml")) {
        fprintf(stderr, "error: could not save constellation.toml\n");
        return 1;
    }

    if (is_git)
        printf("  added %s = { git = \"%s\", tag = \"%s\" } to constellation.toml\n", name, git_url, git_tag);
    else
        printf("  added %s = \"%s\" to constellation.toml\n", name, val);
    return 0;
}

/* ═══════════════════════════════════════════
 *  pkg_wrap & FFI / python
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

/* ═══════════════════════════════════════════
 *  Lockfile & Git installation
 * ═══════════════════════════════════════════ */
#include <openssl/sha.h>

typedef struct {
    char name[64];
    char version[32];
    char git_url[256];
    char commit[64];
    char sha256[65];
    char build_script_hash[65];
} LockEntry;

typedef struct {
    LockEntry entries[128];
    int count;
} ConstellationLock;

static bool sha256_file(const char *path, char *out_hex) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    
    unsigned char buf[8192];
    size_t bytes;
    while ((bytes = fread(buf, 1, sizeof(buf), f)) > 0) {
        SHA256_Update(&ctx, buf, bytes);
    }
    
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_Final(hash, &ctx);
    fclose(f);
    
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        snprintf(out_hex + i * 2, 3, "%02x", hash[i]);
    }
    out_hex[SHA256_DIGEST_LENGTH * 2] = '\0';
    return true;
}

#include <curl/curl.h>

struct MemoryBuffer {
    char *data;
    size_t size;
};

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t real_size = size * nmemb;
    struct MemoryBuffer *mem = (struct MemoryBuffer *)userdata;
    char *ptr_new = realloc(mem->data, mem->size + real_size + 1);
    if (!ptr_new) return 0;
    mem->data = ptr_new;
    memcpy(&(mem->data[mem->size]), ptr, real_size);
    mem->size += real_size;
    mem->data[mem->size] = 0;
    return real_size;
}

static char *fetch_index(void) {
    const char *url = getenv("CONSTELLATION_INDEX_URL");
    if (!url) {
        url = "https://raw.githubusercontent.com/varian-lang/constellation-index/main/index.json";
    }
    
    if (url[0] == '/') {
        FILE *f = fopen(url, "r");
        if (!f) {
            fprintf(stderr, "error: could not open local registry index '%s'\n", url);
            return NULL;
        }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        rewind(f);
        char *buf = malloc(sz + 1);
        size_t n = fread(buf, 1, sz, f);
        buf[n] = '\0';
        fclose(f);
        return buf;
    }
    
    printf("  fetching registry index from %s...\n", url);
    
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    
    struct MemoryBuffer buf = {NULL, 0};
    buf.data = malloc(1);
    buf.data[0] = '\0';
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Varian/0.1.0");
    
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK) {
        fprintf(stderr, "error: failed to fetch registry index: %s\n", curl_easy_strerror(res));
        free(buf.data);
        return NULL;
    }
    
    return buf.data;
}

typedef struct {
    char version[32];
    char git_url[256];
    char git_tag[64];
    char sha256[65];
} RegistryVersion;

typedef struct {
    char name[64];
    RegistryVersion versions[64];
    int version_count;
} RegistryPackage;

static bool parse_registry_index(const char *json, RegistryPackage *packages, int *package_count) {
    const char *p = json;
    json_skip_ws(&p);
    if (*p != '{') return false;
    p++;
    
    *package_count = 0;
    while (1) {
        json_skip_ws(&p);
        if (*p == '}') { p++; break; }
        
        char *pkg_name = json_parse_string(&p);
        if (!pkg_name) return false;
        
        json_skip_ws(&p);
        if (*p != ':') { free(pkg_name); return false; }
        p++;
        
        json_skip_ws(&p);
        if (*p != '{') { free(pkg_name); return false; }
        p++;
        
        int pkg_idx = (*package_count)++;
        strncpy(packages[pkg_idx].name, pkg_name, sizeof(packages[pkg_idx].name) - 1);
        packages[pkg_idx].name[sizeof(packages[pkg_idx].name) - 1] = '\0';
        packages[pkg_idx].version_count = 0;
        free(pkg_name);
        
        while (1) {
            json_skip_ws(&p);
            if (*p == '}') { p++; break; }
            
            char *key = json_parse_string(&p);
            if (!key) return false;
            
            json_skip_ws(&p);
            if (*p != ':') { free(key); return false; }
            p++;
            
            if (strcmp(key, "versions") == 0) {
                json_skip_ws(&p);
                if (*p != '{') { free(key); return false; }
                p++;
                
                while (1) {
                    json_skip_ws(&p);
                    if (*p == '}') { p++; break; }
                    
                    char *ver_str = json_parse_string(&p);
                    if (!ver_str) { free(key); return false; }
                    
                    json_skip_ws(&p);
                    if (*p != ':') { free(key); free(ver_str); return false; }
                    p++;
                    
                    json_skip_ws(&p);
                    if (*p != '{') { free(key); free(ver_str); return false; }
                    p++;
                    
                    int ver_idx = packages[pkg_idx].version_count++;
                    strncpy(packages[pkg_idx].versions[ver_idx].version, ver_str, sizeof(packages[pkg_idx].versions[ver_idx].version) - 1);
                    packages[pkg_idx].versions[ver_idx].version[sizeof(packages[pkg_idx].versions[ver_idx].version) - 1] = '\0';
                    free(ver_str);
                    
                    while (1) {
                        json_skip_ws(&p);
                        if (*p == '}') { p++; break; }
                        
                        char *vkey = json_parse_string(&p);
                        if (!vkey) { free(key); return false; }
                        
                        json_skip_ws(&p);
                        if (*p != ':') { free(key); free(vkey); return false; }
                        p++;
                        
                        char *vval = json_parse_string(&p);
                        if (!vval) { free(key); free(vkey); return false; }
                        
                        if (strcmp(vkey, "git") == 0) {
                            strncpy(packages[pkg_idx].versions[ver_idx].git_url, vval, sizeof(packages[pkg_idx].versions[ver_idx].git_url) - 1);
                            packages[pkg_idx].versions[ver_idx].git_url[sizeof(packages[pkg_idx].versions[ver_idx].git_url) - 1] = '\0';
                        } else if (strcmp(vkey, "tag") == 0) {
                            strncpy(packages[pkg_idx].versions[ver_idx].git_tag, vval, sizeof(packages[pkg_idx].versions[ver_idx].git_tag) - 1);
                            packages[pkg_idx].versions[ver_idx].git_tag[sizeof(packages[pkg_idx].versions[ver_idx].git_tag) - 1] = '\0';
                        } else if (strcmp(vkey, "sha256") == 0) {
                            strncpy(packages[pkg_idx].versions[ver_idx].sha256, vval, sizeof(packages[pkg_idx].versions[ver_idx].sha256) - 1);
                            packages[pkg_idx].versions[ver_idx].sha256[sizeof(packages[pkg_idx].versions[ver_idx].sha256) - 1] = '\0';
                        }
                        
                        free(vkey);
                        free(vval);
                        
                        json_skip_ws(&p);
                        if (*p == ',') { p++; continue; }
                        if (*p == '}') { p++; break; }
                        return false;
                    }
                    
                    json_skip_ws(&p);
                    if (*p == ',') { p++; continue; }
                    if (*p == '}') { p++; break; }
                    return false;
                }
            } else {
                json_skip_ws(&p);
                if (*p == '{') {
                    int brace_depth = 1;
                    p++;
                    while (brace_depth > 0 && *p) {
                        if (*p == '{') brace_depth++;
                        else if (*p == '}') brace_depth--;
                        p++;
                    }
                } else if (*p == '"') {
                    char *dummy = json_parse_string(&p);
                    free(dummy);
                } else {
                    while (*p && *p != ',' && *p != '}') p++;
                }
            }
            free(key);
            
            json_skip_ws(&p);
            if (*p == ',') { p++; continue; }
            if (*p == '}') { p++; break; }
            return false;
        }
        
        json_skip_ws(&p);
        if (*p == ',') { p++; continue; }
        if (*p == '}') { p++; break; }
        return false;
    }
    return true;
}

static bool parse_version(const char *ver_str, int *major, int *minor, int *patch) {
    *major = 0; *minor = 0; *patch = 0;
    return sscanf(ver_str, "%d.%d.%d", major, minor, patch) >= 1;
}

static bool version_matches(const char *ver_str, const char *range_spec) {
    if (strcmp(range_spec, "*") == 0 || strcmp(range_spec, "latest") == 0) {
        return true;
    }
    
    int v_maj, v_min, v_pat;
    if (!parse_version(ver_str, &v_maj, &v_min, &v_pat)) return false;
    
    if (range_spec[0] == '^') {
        int r_maj, r_min, r_pat;
        if (!parse_version(range_spec + 1, &r_maj, &r_min, &r_pat)) return false;
        if (v_maj != r_maj) return false;
        if (v_maj > 0) {
            if (v_min < r_min) return false;
            if (v_min == r_min && v_pat < r_pat) return false;
        } else {
            if (r_min > 0) {
                if (v_min != r_min) return false;
                if (v_pat < r_pat) return false;
            } else {
                if (v_min != 0) return false;
                if (v_pat != r_pat) return false;
            }
        }
        return true;
    }
    
    if (range_spec[0] == '~') {
        int r_maj, r_min, r_pat;
        if (!parse_version(range_spec + 1, &r_maj, &r_min, &r_pat)) return false;
        if (v_maj != r_maj || v_min != r_min) return false;
        if (v_pat < r_pat) return false;
        return true;
    }
    
    return strcmp(ver_str, range_spec) == 0;
}

static int compare_versions(const char *v1, const char *v2) {
    int maj1, min1, pat1;
    int maj2, min2, pat2;
    parse_version(v1, &maj1, &min1, &pat1);
    parse_version(v2, &maj2, &min2, &pat2);
    if (maj1 != maj2) return maj1 - maj2;
    if (min1 != min2) return min1 - min2;
    return pat1 - pat2;
}

static bool resolve_registry_package(const char *name, const char *range_spec, char *out_git_url, char *out_tag, char *out_sha256) {
    char *index_json = fetch_index();
    if (!index_json) return false;
    
    static RegistryPackage packages[128];
    int package_count = 0;
    if (!parse_registry_index(index_json, packages, &package_count)) {
        fprintf(stderr, "error: failed to parse registry index JSON\n");
        free(index_json);
        return false;
    }
    free(index_json);
    
    int pkg_idx = -1;
    for (int i = 0; i < package_count; i++) {
        if (strcmp(packages[i].name, name) == 0) {
            pkg_idx = i;
            break;
        }
    }
    
    if (pkg_idx < 0) {
        fprintf(stderr, "error: package '%s' not found in registry index\n", name);
        return false;
    }
    
    int best_ver_idx = -1;
    for (int i = 0; i < packages[pkg_idx].version_count; i++) {
        const char *ver_str = packages[pkg_idx].versions[i].version;
        if (version_matches(ver_str, range_spec)) {
            if (best_ver_idx < 0 || compare_versions(ver_str, packages[pkg_idx].versions[best_ver_idx].version) > 0) {
                best_ver_idx = i;
            }
        }
    }
    
    if (best_ver_idx < 0) {
        fprintf(stderr, "error: no version of package '%s' matches range spec '%s'\n", name, range_spec);
        return false;
    }
    
    strncpy(out_git_url, packages[pkg_idx].versions[best_ver_idx].git_url, 255);
    out_git_url[255] = '\0';
    strncpy(out_tag, packages[pkg_idx].versions[best_ver_idx].git_tag, 63);
    out_tag[63] = '\0';
    strncpy(out_sha256, packages[pkg_idx].versions[best_ver_idx].sha256, 64);
    out_sha256[64] = '\0';
    
    printf("  resolved '%s' (%s) -> version %s (git: %s, tag: %s)\n",
           name, range_spec, packages[pkg_idx].versions[best_ver_idx].version,
           out_git_url, out_tag);
    return true;
}

static bool lock_load(ConstellationLock *lock, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    memset(lock, 0, sizeof(ConstellationLock));
    char line[1024];
    int cur = -1;
    while (fgets(line, sizeof(line), f)) {
        char *start = line;
        while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') start++;
        if (*start == '\0' || *start == '#') continue;
        if (strstr(start, "[[package]]")) {
            if (lock->count < 128) {
                cur = lock->count++;
            } else {
                cur = -1;
            }
            continue;
        }
        if (cur >= 0) {
            char *eq = strchr(start, '=');
            if (eq) {
                *eq = '\0';
                char key[128];
                char val[512];
                trim_str(key, start, sizeof(key));
                trim_str(val, eq + 1, sizeof(val));
                if (strcmp(key, "name") == 0) strncpy(lock->entries[cur].name, val, sizeof(lock->entries[cur].name) - 1);
                else if (strcmp(key, "version") == 0) strncpy(lock->entries[cur].version, val, sizeof(lock->entries[cur].version) - 1);
                else if (strcmp(key, "git") == 0) strncpy(lock->entries[cur].git_url, val, sizeof(lock->entries[cur].git_url) - 1);
                else if (strcmp(key, "commit") == 0) strncpy(lock->entries[cur].commit, val, sizeof(lock->entries[cur].commit) - 1);
                else if (strcmp(key, "sha256") == 0) strncpy(lock->entries[cur].sha256, val, sizeof(lock->entries[cur].sha256) - 1);
                else if (strcmp(key, "build_hash") == 0) strncpy(lock->entries[cur].build_script_hash, val, sizeof(lock->entries[cur].build_script_hash) - 1);
            }
        }
    }
    fclose(f);
    return true;
}

static bool lock_save(const ConstellationLock *lock, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return false;
    for (int i = 0; i < lock->count; i++) {
        fprintf(f, "[[package]]\n");
        fprintf(f, "name = \"%s\"\n", lock->entries[i].name);
        fprintf(f, "version = \"%s\"\n", lock->entries[i].version);
        fprintf(f, "git = \"%s\"\n", lock->entries[i].git_url);
        fprintf(f, "commit = \"%s\"\n", lock->entries[i].commit);
        fprintf(f, "sha256 = \"%s\"\n", lock->entries[i].sha256);
        if (lock->entries[i].build_script_hash[0] != '\0')
            fprintf(f, "build_hash = \"%s\"\n", lock->entries[i].build_script_hash);
        fprintf(f, "\n");
    }
    fclose(f);
    return true;
}

static bool download_and_extract_git(const char *pkg_name, const char *git_url, const char *tag, char *out_commit, char *out_sha256) {
    char cmd[4096];
    char tmp_dir[512];
    char tmp_tarball[512];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/constellation_clone_%s", pkg_name);
    snprintf(tmp_tarball, sizeof(tmp_tarball), "/tmp/constellation_tarball_%s.tar.gz", pkg_name);
    
    snprintf(cmd, sizeof(cmd), "rm -rf %s %s", tmp_dir, tmp_tarball);
    system(cmd);
    
    printf("  cloning %s (%s) from %s...\n", pkg_name, tag, git_url);
    if (git_url[0] == '/') {
        snprintf(cmd, sizeof(cmd), "git clone --depth 1 --branch %s %s %s 2>/dev/null || git clone %s %s 2>/dev/null", tag, git_url, tmp_dir, git_url, tmp_dir);
    } else {
        snprintf(cmd, sizeof(cmd), "git clone --depth 1 --branch %s https://%s %s 2>/dev/null || git clone https://%s %s 2>/dev/null", tag, git_url, tmp_dir, git_url, tmp_dir);
    }
    if (system(cmd) != 0) {
        fprintf(stderr, "error: failed to clone git repository %s\n", git_url);
        return false;
    }
    
    snprintf(cmd, sizeof(cmd), "cd %s && git checkout %s 2>/dev/null", tmp_dir, tag);
    system(cmd);
    
    FILE *fp = popen(snprintf(cmd, sizeof(cmd), "cd %s && git rev-parse HEAD", tmp_dir) ? cmd : "", "r");
    if (fp) {
        if (fgets(out_commit, 64, fp)) {
            char *nl = strchr(out_commit, '\n');
            if (nl) *nl = '\0';
        }
        pclose(fp);
    }
    if (out_commit[0] == '\0') {
        strncpy(out_commit, tag, 63);
    }
    
    snprintf(cmd, sizeof(cmd), "rm -rf %s/.git", tmp_dir);
    system(cmd);
    
    snprintf(cmd, sizeof(cmd), "tar -czf %s -C %s .", tmp_tarball, tmp_dir);
    if (system(cmd) != 0) {
        fprintf(stderr, "error: failed to create tarball for %s\n", pkg_name);
        return false;
    }
    
    if (!sha256_file(tmp_tarball, out_sha256)) {
        fprintf(stderr, "error: failed to compute sha256 for %s\n", pkg_name);
        return false;
    }
    
    snprintf(cmd, sizeof(cmd), "mkdir -p vn_modules/%s && rm -rf vn_modules/%s/* && tar -xzf %s -C vn_modules/%s",
             pkg_name, pkg_name, tmp_tarball, pkg_name);
    if (system(cmd) != 0) {
        fprintf(stderr, "error: failed to extract package %s\n", pkg_name);
        return false;
    }
    
    snprintf(cmd, sizeof(cmd), "rm -rf %s %s", tmp_dir, tmp_tarball);
    system(cmd);
    return true;
}

static bool restore_locked_dep(const char *pkg_name, const char *git_url, const char *commit, const char *expected_sha256) {
    char cmd[4096];
    char tmp_dir[512];
    char tmp_tarball[512];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/constellation_clone_%s", pkg_name);
    snprintf(tmp_tarball, sizeof(tmp_tarball), "/tmp/constellation_tarball_%s.tar.gz", pkg_name);
    
    snprintf(cmd, sizeof(cmd), "rm -rf %s %s", tmp_dir, tmp_tarball);
    system(cmd);
    
    printf("  restoring %s (%s) from %s...\n", pkg_name, commit, git_url);
    if (git_url[0] == '/') {
        snprintf(cmd, sizeof(cmd), "git clone %s %s 2>/dev/null", git_url, tmp_dir);
    } else {
        snprintf(cmd, sizeof(cmd), "git clone https://%s %s 2>/dev/null", git_url, tmp_dir);
    }
    if (system(cmd) != 0) {
        fprintf(stderr, "error: failed to clone git repository %s\n", git_url);
        return false;
    }
    
    snprintf(cmd, sizeof(cmd), "cd %s && git checkout %s 2>/dev/null", tmp_dir, commit);
    if (system(cmd) != 0) {
        fprintf(stderr, "error: failed to checkout commit %s for %s\n", commit, pkg_name);
        return false;
    }
    
    snprintf(cmd, sizeof(cmd), "rm -rf %s/.git", tmp_dir);
    system(cmd);
    
    snprintf(cmd, sizeof(cmd), "tar -czf %s -C %s .", tmp_tarball, tmp_dir);
    if (system(cmd) != 0) {
        fprintf(stderr, "error: failed to create tarball for %s\n", pkg_name);
        return false;
    }
    
    char sha256[65];
    if (!sha256_file(tmp_tarball, sha256)) {
        fprintf(stderr, "error: failed to compute sha256 for %s\n", pkg_name);
        return false;
    }
    
    if (strcmp(sha256, expected_sha256) != 0) {
        fprintf(stderr, "error: SHA256 integrity verification failed for %s!\n"
                        "  expected: %s\n"
                        "  actual:   %s\n", pkg_name, expected_sha256, sha256);
        return false;
    }
    
    snprintf(cmd, sizeof(cmd), "mkdir -p vn_modules/%s && rm -rf vn_modules/%s/* && tar -xzf %s -C vn_modules/%s",
             pkg_name, pkg_name, tmp_tarball, pkg_name);
    if (system(cmd) != 0) {
        fprintf(stderr, "error: failed to extract package %s\n", pkg_name);
        return false;
    }
    
    snprintf(cmd, sizeof(cmd), "rm -rf %s %s", tmp_dir, tmp_tarball);
    system(cmd);
    return true;
}

#define MAX_QUEUE_DEPS 256

typedef struct {
    char name[64];
    char val[256];
    bool is_git;
    char git_url[256];
    char git_tag[64];
    char required_by[64];
} QueueDep;

static QueueDep resolve_queue[MAX_QUEUE_DEPS];
static int resolve_head = 0;
static int resolve_tail = 0;
static bool g_resolve_conflict = false;
static char g_conflict_msg[512];

/* Can two version specs for the same package both be satisfied? Wildcards
 * always can; otherwise the major must match, and two *exact* pins that differ
 * cannot. Conservative on purpose — better a loud false-conflict than a silent
 * wrong resolution. */
static bool ranges_compatible(const char *a, const char *b) {
    if (strcmp(a, b) == 0) return true;
    if (!strcmp(a, "latest") || !strcmp(a, "*") || !strcmp(b, "latest") || !strcmp(b, "*")) return true;
    const char *ap = a; if (*ap == '^' || *ap == '~') ap++;
    const char *bp = b; if (*bp == '^' || *bp == '~') bp++;
    int am = 0, bm = 0, t = 0;
    parse_version(ap, &am, &t, &t);
    parse_version(bp, &bm, &t, &t);
    if (am != bm) return false;                 /* different major -> incompatible */
    bool a_exact = (a[0] != '^' && a[0] != '~');
    bool b_exact = (b[0] != '^' && b[0] != '~');
    if (a_exact && b_exact) return false;       /* two different exact pins */
    return true;
}

static void queue_push_from(const char *name, const char *val, bool is_git,
                            const char *git_url, const char *git_tag, const char *required_by) {
    for (int i = 0; i < resolve_tail; i++) {
        if (strcmp(resolve_queue[i].name, name) == 0) {
            /* Same package reached twice — a diamond dependency. Only an
             * incompatible version requirement is a real problem. */
            if (!is_git && !resolve_queue[i].is_git && !ranges_compatible(resolve_queue[i].val, val)) {
                g_resolve_conflict = true;
                snprintf(g_conflict_msg, sizeof(g_conflict_msg),
                         "version conflict for '%s': '%s' (via %s) vs '%s' (via %s) cannot both be satisfied",
                         name, resolve_queue[i].val, resolve_queue[i].required_by,
                         val, required_by ? required_by : "root");
            }
            return;
        }
    }
    if (resolve_tail >= MAX_QUEUE_DEPS) {
        fprintf(stderr, "error: maximum resolution queue size exceeded\n");
        return;
    }
    int idx = resolve_tail++;
    strncpy(resolve_queue[idx].name, name, sizeof(resolve_queue[idx].name) - 1);
    resolve_queue[idx].name[sizeof(resolve_queue[idx].name) - 1] = '\0';
    strncpy(resolve_queue[idx].val, val, sizeof(resolve_queue[idx].val) - 1);
    resolve_queue[idx].val[sizeof(resolve_queue[idx].val) - 1] = '\0';
    resolve_queue[idx].is_git = is_git;
    strncpy(resolve_queue[idx].git_url, git_url, sizeof(resolve_queue[idx].git_url) - 1);
    resolve_queue[idx].git_url[sizeof(resolve_queue[idx].git_url) - 1] = '\0';
    strncpy(resolve_queue[idx].git_tag, git_tag, sizeof(resolve_queue[idx].git_tag) - 1);
    resolve_queue[idx].git_tag[sizeof(resolve_queue[idx].git_tag) - 1] = '\0';
    strncpy(resolve_queue[idx].required_by, required_by ? required_by : "root", sizeof(resolve_queue[idx].required_by) - 1);
    resolve_queue[idx].required_by[sizeof(resolve_queue[idx].required_by) - 1] = '\0';
}

static void queue_push(const char *name, const char *val, bool is_git, const char *git_url, const char *git_tag) {
    queue_push_from(name, val, is_git, git_url, git_tag, "root");
}

/* A package that declares native capabilities (ffi/python/net/fs) or a build
 * script can run code beyond plain sandboxed Varian. Per the Constellation
 * trust model, that requires explicit consent — never a silent side effect of
 * install. Interactive: prompt y/N. Non-interactive (CI): require
 * CONSTELLATION_TRUST=1, otherwise refuse. */
static bool constellation_consent(const char *pkg) {
    if (getenv("CONSTELLATION_TRUST")) return true;
    if (!isatty(STDIN_FILENO)) {
        fprintf(stderr, "  refused: '%s' needs native capabilities; set CONSTELLATION_TRUST=1 to approve non-interactively.\n", pkg);
        return false;
    }
    fprintf(stderr, "  Approve native capabilities for '%s'? [y/N] ", pkg);
    fflush(stderr);
    char buf[16];
    if (!fgets(buf, sizeof(buf), stdin)) return false;
    return buf[0] == 'y' || buf[0] == 'Y';
}

static int pkg_install_internal(bool frozen, bool force_update) {
    pkg_migrate_manifest();
    
    ConstellationManifest manifest;
    if (!pkg_manifest_load(&manifest, "constellation.toml")) {
        fprintf(stderr, "error: no constellation.toml found in current directory\n");
        return 1;
    }
    
    ConstellationLock lock;
    bool has_lock = false;
    if (!force_update) {
        has_lock = lock_load(&lock, "constellation.lock");
    }
    if (!has_lock && frozen) {
        fprintf(stderr, "error: --frozen requested but constellation.lock is missing\n");
        return 1;
    }
    
    ConstellationLock new_lock;
    memset(&new_lock, 0, sizeof(new_lock));
    bool lock_modified = false;
    
    mkdir("vn_modules", 0755);
    
    resolve_head = 0;
    resolve_tail = 0;
    g_resolve_conflict = false;
    g_conflict_msg[0] = '\0';
    memset(resolve_queue, 0, sizeof(resolve_queue));

    for (int i = 0; i < manifest.dep_count; i++) {
        queue_push(manifest.deps[i].name, manifest.deps[i].val,
                   manifest.deps[i].is_git, manifest.deps[i].git_url,
                   manifest.deps[i].git_tag);
    }

    while (resolve_head < resolve_tail) {
        if (g_resolve_conflict) {
            fprintf(stderr, "error: %s\n", g_conflict_msg);
            return 1;
        }
        QueueDep dep = resolve_queue[resolve_head++];
        
        const char *name = dep.name;
        char git_url[256];
        char tag[64];
        bool is_git = dep.is_git;
        char resolved_sha256[65];
        resolved_sha256[0] = '\0';
        
        int lock_idx = -1;
        if (has_lock) {
            for (int j = 0; j < lock.count; j++) {
                if (strcmp(lock.entries[j].name, name) == 0) {
                    lock_idx = j;
                    break;
                }
            }
        }
        
        if (lock_idx >= 0) {
            strncpy(git_url, lock.entries[lock_idx].git_url, sizeof(git_url) - 1);
            git_url[sizeof(git_url) - 1] = '\0';
            strncpy(tag, lock.entries[lock_idx].version, sizeof(tag) - 1);
            tag[sizeof(tag) - 1] = '\0';
        } else {
            if (is_git) {
                strncpy(git_url, dep.git_url, sizeof(git_url) - 1);
                git_url[sizeof(git_url) - 1] = '\0';
                strncpy(tag, dep.git_tag, sizeof(tag) - 1);
                tag[sizeof(tag) - 1] = '\0';
            } else {
                if (!resolve_registry_package(name, dep.val, git_url, tag, resolved_sha256)) {
                    return 1;
                }
            }
        }
        
        bool resolved = false;
        char commit[64];
        char sha256[65];
        memset(commit, 0, sizeof(commit));
        memset(sha256, 0, sizeof(sha256));
        
        if (lock_idx >= 0) {
            if (!restore_locked_dep(name, lock.entries[lock_idx].git_url, lock.entries[lock_idx].commit, lock.entries[lock_idx].sha256)) {
                return 1;
            }
            strncpy(commit, lock.entries[lock_idx].commit, sizeof(commit) - 1);
            strncpy(sha256, lock.entries[lock_idx].sha256, sizeof(sha256) - 1);
            
            int idx = new_lock.count++;
            new_lock.entries[idx] = lock.entries[lock_idx];
            resolved = true;
        } else {
            if (frozen) {
                fprintf(stderr, "error: dependency %s is not recorded in constellation.lock under --frozen\n", name);
                return 1;
            }
            
            if (!download_and_extract_git(name, git_url, tag, commit, sha256)) {
                return 1;
            }
            
            if (resolved_sha256[0] != '\0' && strcmp(sha256, resolved_sha256) != 0) {
                fprintf(stderr, "error: SHA256 integrity verification failed for registry package %s!\n"
                                "  expected: %s\n"
                                "  actual:   %s\n", name, resolved_sha256, sha256);
                return 1;
            }
            
            int idx = new_lock.count++;
            strncpy(new_lock.entries[idx].name, name, sizeof(new_lock.entries[idx].name) - 1);
            strncpy(new_lock.entries[idx].version, tag, sizeof(new_lock.entries[idx].version) - 1);
            strncpy(new_lock.entries[idx].git_url, git_url, sizeof(new_lock.entries[idx].git_url) - 1);
            strncpy(new_lock.entries[idx].commit, commit, sizeof(new_lock.entries[idx].commit) - 1);
            strncpy(new_lock.entries[idx].sha256, sha256, sizeof(new_lock.entries[idx].sha256) - 1);
            lock_modified = true;
            resolved = true;
        }
        
        if (resolved) {
            char dep_toml_path[512];
            snprintf(dep_toml_path, sizeof(dep_toml_path), "vn_modules/%s/constellation.toml", name);
            
            ConstellationManifest dep_manifest;
            if (pkg_manifest_load(&dep_manifest, dep_toml_path)) {
                if (dep_manifest.cap_ffi || dep_manifest.cap_python || dep_manifest.cap_fs || dep_manifest.cap_net || dep_manifest.build_script[0] != '\0') {
                    printf("\033[1;33m  [TRUST] Package '%s' requests native capabilities:\033[0m\n", name);
                    if (dep_manifest.cap_ffi) printf("    - ffi\n");
                    if (dep_manifest.cap_python) printf("    - python\n");
                    if (dep_manifest.cap_fs) printf("    - fs\n");
                    if (dep_manifest.cap_net) printf("    - net\n");
                    if (dep_manifest.build_script[0] != '\0') {
                        printf("    - build script: %s\n", dep_manifest.build_script);
                    }

                    /* Gate the install on explicit consent. Without it, remove
                     * the vendored files so nothing untrusted is left on disk. */
                    if (!constellation_consent(name)) {
                        char rmcmd[600];
                        snprintf(rmcmd, sizeof(rmcmd), "rm -rf vn_modules/%s", name);
                        system(rmcmd);
                        fprintf(stderr, "  aborted: '%s' not approved — install stopped.\n", name);
                        return 1;
                    }

                    if (dep_manifest.build_script[0] != '\0' && new_lock.count > 0) {
                        /* Record the build-script hash so a later change re-prompts
                         * (trust-on-first-use, pinned). */
                        char bs_path[600];
                        snprintf(bs_path, sizeof(bs_path), "vn_modules/%s/%s", name, dep_manifest.build_script);
                        char bs_hash[65];
                        if (sha256_file(bs_path, bs_hash)) {
                            strncpy(new_lock.entries[new_lock.count - 1].build_script_hash, bs_hash, sizeof(new_lock.entries[0].build_script_hash) - 1);
                        }
                    }
                    printf("  approved — capabilities recorded in constellation.lock\n");

                    /* Run the consented build script via the vn runtime, from the
                     * package's own directory. Consent is the security boundary
                     * here (like npm's postinstall) — full capability sandboxing
                     * is the future hardening; a build failure aborts the install
                     * and removes the half-built package. */
                    if (dep_manifest.build_script[0] != '\0') {
                        char self[1024];
                        ssize_t sn = readlink("/proc/self/exe", self, sizeof(self) - 1);
                        if (sn > 0) {
                            self[sn] = '\0';
                            char bcmd[2400];
                            snprintf(bcmd, sizeof(bcmd), "cd vn_modules/%s && '%s' run '%s'",
                                     name, self, dep_manifest.build_script);
                            printf("  running build script '%s' for %s...\n", dep_manifest.build_script, name);
                            if (system(bcmd) != 0) {
                                fprintf(stderr, "  build script for '%s' failed — install aborted.\n", name);
                                char rmcmd[600];
                                snprintf(rmcmd, sizeof(rmcmd), "rm -rf vn_modules/%s", name);
                                system(rmcmd);
                                return 1;
                            }
                            /* The build script is install-time only — remove it so
                             * `use "<pkg>"` never concatenates it into the package's
                             * importable source (it would otherwise re-run at runtime).
                             * Its generated output stays. */
                            char rmscript[700];
                            snprintf(rmscript, sizeof(rmscript), "vn_modules/%s/%s", name, dep_manifest.build_script);
                            remove(rmscript);
                        }
                    }
                }
                
                printf("  parsing transitive dependencies of %s...\n", name);
                for (int j = 0; j < dep_manifest.dep_count; j++) {
                    queue_push_from(dep_manifest.deps[j].name, dep_manifest.deps[j].val,
                                    dep_manifest.deps[j].is_git, dep_manifest.deps[j].git_url,
                                    dep_manifest.deps[j].git_tag, name);
                }
            }
        }
    }

    /* A conflict surfaced while expanding the last package's deps would not be
     * caught by the top-of-loop check — fail before writing a bad lock. */
    if (g_resolve_conflict) {
        fprintf(stderr, "error: %s\n", g_conflict_msg);
        return 1;
    }

    if (has_lock) {
        if (new_lock.count != lock.count) {
            lock_modified = true;
        } else {
            for (int i = 0; i < new_lock.count; i++) {
                if (strcmp(new_lock.entries[i].name, lock.entries[i].name) != 0 ||
                    strcmp(new_lock.entries[i].commit, lock.entries[i].commit) != 0 ||
                    strcmp(new_lock.entries[i].sha256, lock.entries[i].sha256) != 0) {
                    lock_modified = true;
                    break;
                }
            }
        }
    }
    
    if (lock_modified || !has_lock || force_update) {
        if (lock_save(&new_lock, "constellation.lock")) {
            printf("  wrote constellation.lock\n");
        }
    }
    
    DIR *d = opendir("vn_modules");
    if (d) {
        struct dirent *entry;
        while ((entry = readdir(d)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            
            bool in_lock = false;
            for (int j = 0; j < new_lock.count; j++) {
                if (strcmp(new_lock.entries[j].name, entry->d_name) == 0) {
                    in_lock = true;
                    break;
                }
            }
            
            char full_path[512];
            snprintf(full_path, sizeof(full_path), "vn_modules/%s", entry->d_name);
            struct stat st;
            if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                if (!in_lock && strcmp(entry->d_name, "lumen_assets") != 0) {
                    printf("  pruning unused dependency: %s\n", entry->d_name);
                    char cmd[1024];
                    snprintf(cmd, sizeof(cmd), "rm -rf %s", full_path);
                    system(cmd);
                }
            }
        }
        closedir(d);
    }
    
    printf("  dependencies successfully installed\n");
    return 0;
}

int pkg_install(bool frozen) {
    return pkg_install_internal(frozen, false);
}

int pkg_remove(const char *pkg_name) {
    pkg_migrate_manifest();
    
    ConstellationManifest manifest;
    if (!pkg_manifest_load(&manifest, "constellation.toml")) {
        fprintf(stderr, "error: no constellation.toml found in current directory\n");
        return 1;
    }
    
    int idx = -1;
    for (int i = 0; i < manifest.dep_count; i++) {
        if (strcmp(manifest.deps[i].name, pkg_name) == 0) {
            idx = i;
            break;
        }
    }
    
    if (idx < 0) {
        fprintf(stderr, "error: dependency %s not found in constellation.toml\n", pkg_name);
        return 1;
    }
    
    for (int i = idx; i < manifest.dep_count - 1; i++) {
        manifest.deps[i] = manifest.deps[i + 1];
    }
    manifest.dep_count--;
    
    if (!pkg_manifest_save(&manifest, "constellation.toml")) {
        fprintf(stderr, "error: could not save constellation.toml\n");
        return 1;
    }
    
    printf("  removed %s from constellation.toml\n", pkg_name);
    
    return pkg_install_internal(false, false);
}

int pkg_update(void) {
    printf("  updating dependencies (forcing fresh git resolution)...\n");
    return pkg_install_internal(false, true);
}

int pkg_search(const char *query) {
    char *index_json = fetch_index();
    if (!index_json) return 1;
    
    static RegistryPackage packages[128];
    int package_count = 0;
    if (!parse_registry_index(index_json, packages, &package_count)) {
        fprintf(stderr, "error: failed to parse registry index JSON\n");
        free(index_json);
        return 1;
    }
    free(index_json);
    
    printf("Found packages matching '%s':\n\n", query);
    int matches = 0;
    for (int i = 0; i < package_count; i++) {
        if (strcmp(query, "*") == 0 || strstr(packages[i].name, query) != NULL) {
            printf("  %s\n", packages[i].name);
            printf("    versions: ");
            for (int j = 0; j < packages[i].version_count; j++) {
                if (j > 0) printf(", ");
                printf("%s", packages[i].versions[j].version);
            }
            printf("\n\n");
            matches++;
        }
    }
    if (matches == 0) {
        printf("  No packages found.\n");
    }
    return 0;
}

int pkg_publish(void) {
    ConstellationManifest manifest;
    if (!pkg_manifest_load(&manifest, "constellation.toml")) {
        fprintf(stderr, "error: no constellation.toml found in current directory. Cannot publish.\n");
        return 1;
    }
    
    printf("\033[1;36m⬡  Constellation\033[0m  packaging %s v%s\n", manifest.name, manifest.version);

    if (manifest.cap_ffi || manifest.cap_python || manifest.cap_fs || manifest.cap_net) {
        printf("  capabilities: ");
        if (manifest.cap_ffi) printf("ffi ");
        if (manifest.cap_python) printf("python ");
        if (manifest.cap_fs) printf("fs ");
        if (manifest.cap_net) printf("net ");
        printf("\n");
    }
    if (manifest.build_script[0] != '\0') {
        printf("  build script: %s\n", manifest.build_script);
    }

    /* Build the same tarball `vn install` would fetch (tracked files only, via
     * `git archive`) and hash it, so the maintainer gets the exact sha256 the
     * registry index must pin. */
    char commit[64] = "";
    FILE *cp = popen("git rev-parse HEAD 2>/dev/null", "r");
    if (cp) { if (fgets(commit, sizeof(commit), cp)) { char *nl = strchr(commit, '\n'); if (nl) *nl = '\0'; } pclose(cp); }
    if (commit[0] == '\0') {
        fprintf(stderr, "error: `vn publish` must run inside a git repository (the package source).\n");
        return 1;
    }

    const char *tarball = "/tmp/constellation_publish.tar.gz";
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "git archive --format=tar.gz -o %s HEAD 2>/dev/null", tarball);
    if (system(cmd) != 0) {
        fprintf(stderr, "error: failed to archive the repository.\n");
        return 1;
    }
    char sha256[65];
    if (!sha256_file(tarball, sha256)) {
        fprintf(stderr, "error: failed to hash the package tarball.\n");
        return 1;
    }
    remove(tarball);

    char git_remote[256] = "<your-git-url>";
    FILE *rp = popen("git config --get remote.origin.url 2>/dev/null", "r");
    if (rp) { if (fgets(git_remote, sizeof(git_remote), rp)) { char *nl = strchr(git_remote, '\n'); if (nl) *nl = '\0'; } pclose(rp); }

    /* Honest: this does NOT push anywhere. It prepares the exact index entry to
     * submit. Constellation is a thin git-hosted index (see CONSTELLATION_PLAN.md);
     * publishing = adding this entry to the index repo (a PR), not a server upload. */
    printf("\n  Package prepared. Add this entry to the Constellation index, then\n");
    printf("  open a PR against the index repo (no upload to us — your git hosts the code):\n\n");
    printf("  \"%s\": {\n", manifest.name);
    printf("    \"versions\": {\n");
    printf("      \"%s\": {\n", manifest.version);
    printf("        \"git\": \"%s\",\n", git_remote);
    printf("        \"tag\": \"v%s\",\n", manifest.version);
    printf("        \"sha256\": \"%s\"\n", sha256);
    printf("      }\n    }\n  }\n\n");
    printf("  (tag this commit `v%s` and push it so the sha256 stays valid.)\n", manifest.version);

    return 0;
}
