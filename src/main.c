#include "varian.h"
#include "lexer.h"
#include "parser.h"
#include "vm.h"
#include "fmt.h"
#include "test_runner.h"
#include "pkg_manager.h"
#include "lint.h"
#include "lsp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <curl/curl.h>

/* Set before vm_run() for "vn <file>"/"vn run <file>" so lib_http.c's
 * cluster worker threads can independently re-load the same script. */
const char *g_varian_script_path = NULL;
static int g_prelude_line_count = 0;

#ifndef VARIAN_VERSION
#define VARIAN_VERSION "0.1.0"
#endif

/* ─── Read source file ─── */
static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "Could not open file \"%s\".\n", path);
        return NULL;
    }

    fseek(file, 0L, SEEK_END);
    size_t size = ftell(file);
    rewind(file);

    char *buffer = (char *)malloc(size + 1);
    if (!buffer) {
        fprintf(stderr, "Not enough memory to read \"%s\".\n", path);
        fclose(file);
        return NULL;
    }

    size_t bytes_read = fread(buffer, 1, size, file);
    if (bytes_read < size) {
        fprintf(stderr, "Could not read file \"%s\".\n", path);
        free(buffer);
        fclose(file);
        return NULL;
    }

    buffer[bytes_read] = '\0';
    fclose(file);
    return buffer;
}

/* ─── Read all .vn files from a directory, concatenated ─── */
static char *read_directory_sources(const char *dir_path) {
    char *result = NULL;
    size_t result_len = 0;
    DIR *d = opendir(dir_path);
    if (!d) return NULL;

    struct dirent *entry;
    /* Count .vn files */
    int file_count = 0;
    rewinddir(d);
    while ((entry = readdir(d)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len > 3 && strcmp(entry->d_name + len - 3, ".vn") == 0)
            file_count++;
    }

    if (file_count == 0) {
        closedir(d);
        return NULL;
    }

    /* Build full paths */
    char **paths = (char **)calloc(file_count, sizeof(char *));
    int idx = 0;
    rewinddir(d);
    while ((entry = readdir(d)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len > 3 && strcmp(entry->d_name + len - 3, ".vn") == 0) {
            size_t path_len = strlen(dir_path) + 1 + len + 1;
            paths[idx] = (char *)malloc(path_len);
            snprintf(paths[idx], path_len, "%s/%s", dir_path, entry->d_name);
            idx++;
        }
    }
    closedir(d);

    /* Sort alphabetically for deterministic order */
    for (int i = 0; i < file_count - 1; i++) {
        for (int j = i + 1; j < file_count; j++) {
            if (strcmp(paths[i], paths[j]) > 0) {
                char *tmp = paths[i];
                paths[i] = paths[j];
                paths[j] = tmp;
            }
        }
    }

    /* Read and concatenate all files */
    for (int i = 0; i < file_count; i++) {
        char *content = read_file(paths[i]);
        if (!content) { free(paths[i]); continue; }
        size_t content_len = strlen(content);
        char *new_result = (char *)realloc(result, result_len + content_len + 2);
        if (!new_result) { free(result); free(content); free(paths[i]); free(paths); return NULL; }
        result = new_result;
        if (result_len > 0) { result[result_len] = '\n'; result_len++; }
        memcpy(result + result_len, content, content_len);
        result_len += content_len;
        result[result_len] = '\0';
        free(content);
        free(paths[i]);
    }
    free(paths);
    return result;
}

/* Locate the vn_modules prelude directory independent of CWD, so the tooling
 * (vn dev / vn run) works from any project folder, not just the repo root.
 * Search order: $VARIAN_HOME, ./vn_modules (repo dev), then relative to the
 * executable (install layouts). Returns a pointer to a static buffer or NULL. */
static const char *resolve_vn_modules_dir(void) {
    static char found[2048];
    DIR *d;

    const char *home = getenv("VARIAN_HOME");
    if (home && *home) {
        snprintf(found, sizeof(found), "%s/vn_modules", home);
        if ((d = opendir(found))) { closedir(d); return found; }
    }

    if ((d = opendir("vn_modules"))) {
        closedir(d);
        snprintf(found, sizeof(found), "vn_modules");
        return found;
    }

    char exe[2048];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            snprintf(found, sizeof(found), "%s/vn_modules", exe);
            if ((d = opendir(found))) { closedir(d); return found; }
            snprintf(found, sizeof(found), "%s/../share/varian/vn_modules", exe);
            if ((d = opendir(found))) { closedir(d); return found; }
            snprintf(found, sizeof(found), "%s/../lib/varian/vn_modules", exe);
            if ((d = opendir(found))) { closedir(d); return found; }
        }
    }
    return NULL;
}

/* ─── Read source with module prelude from vn_modules/ ─── */
char *read_file_with_modules(const char *path) {
    char *main_source = read_file(path);
    if (!main_source) return NULL;

    const char *mod_dir = resolve_vn_modules_dir();
    char *prelude = mod_dir ? read_directory_sources(mod_dir) : NULL;
    if (!prelude) {
        g_prelude_line_count = 0;
        return main_source;
    }

    size_t prelude_len = strlen(prelude);
    g_prelude_line_count = 1; // Since we append a newline below
    for (size_t i = 0; i < prelude_len; i++) {
        if (prelude[i] == '\n') g_prelude_line_count++;
    }

    size_t main_len = strlen(main_source);
    char *combined = (char *)malloc(prelude_len + 1 + main_len + 1);
    if (!combined) { free(prelude); return main_source; }
    memcpy(combined, prelude, prelude_len);
    combined[prelude_len] = '\n';
    memcpy(combined + prelude_len + 1, main_source, main_len);
    combined[prelude_len + 1 + main_len] = '\0';
    free(prelude);
    free(main_source);
    return combined;
}

/* Print "  <line text>\n     ^\n" for a 1-based (line, col) into stderr. */
static void print_source_caret(const char *source, int line, int col) {
    if (line < 1 || !source) return;
    const char *p = source;
    int cur = 1;
    while (cur < line && *p) { if (*p == '\n') cur++; p++; }
    const char *end = p;
    while (*end && *end != '\n') end++;
    fprintf(stderr, "  %.*s\n  ", (int)(end - p), p);
    for (int i = 1; i < col && i < 200; i++) fputc(' ', stderr);
    fprintf(stderr, "^\n");
}

/* ─── Run source string ─── */
static int run_source(const char *source, const char *filename) {
    Lexer lexer;
    lexer_init(&lexer, source, filename);

    Arena *arena = arena_create(0);
    Parser parser;
    parser_init(&parser, &lexer, arena);

    AstNode *program = parser_parse(&parser);
    if (parser.had_error) {
        const char *msg = parser_get_error(&parser);
        int line = 0, col = 0;
        int adjusted_line = 0;
        if (sscanf(msg, "[%d:%d]", &line, &col) == 2) {
            adjusted_line = line - g_prelude_line_count;
            if (adjusted_line <= 0) adjusted_line = line; // fallback
            fprintf(stderr, "Parse error in %s:%d:%d\n", filename ? filename : "<script>", adjusted_line, col);
        } else {
            fprintf(stderr, "Parse error: %s\n", msg);
        }
        if (line > 0) print_source_caret(source, line, col);
        
        if (adjusted_line > 0 && adjusted_line != line) {
            const char *p = strchr(msg, ']');
            if (p) {
                fprintf(stderr, "  [%d:%d]%s\n", adjusted_line, col, p + 1);
            } else {
                fprintf(stderr, "  %s\n", msg);
            }
        } else {
            fprintf(stderr, "  %s\n", msg);
        }
        arena_destroy(arena);
        return 1;
    }

    if (getenv("VN_DEBUG_AST")) {
        printf("=== AST ===\n");
        ast_print(program, 0);
        printf("===========\n");
    }

    Chunk chunk;
    chunk_init(&chunk);

    Compiler compiler;
    compiler_init(&compiler, arena, &chunk, program);

    if (!compiler_compile(&compiler)) {
        fprintf(stderr, "Compile error: %s\n", compiler.error_message);
        chunk_free(&chunk);
        arena_destroy(arena);
        return 1;
    }

    if (getenv("VN_DEBUG_BYTECODE")) {
        printf("=== Bytecode (%d bytes) ===\n", chunk.count);
        for (int i = 0; i < chunk.count; i++) {
            printf("  %3d: 0x%02x", i, chunk.code[i]);
            if (chunk.code[i] == BC_CONSTANT) {
                int idx = (chunk.code[i + 1] << 8) | chunk.code[i + 2];
                printf(" (constant %d)", idx);
                i += 2;
            } else if (chunk.code[i] == BC_CONSTANT_LONG) {
                int idx = (chunk.code[i + 1] << 8) | chunk.code[i + 2];
                printf(" (constant_long %d)", idx);
                i += 2;
            } else if (chunk.code[i] == BC_COMPTIME_EXEC) {
                int result_idx = (chunk.code[i + 1] << 8) | chunk.code[i + 2];
                int fn_idx = (chunk.code[i + 3] << 8) | chunk.code[i + 4];
                printf(" (comptime result_idx=%d fn_idx=%d)", result_idx, fn_idx);
                i += 4;
            } else if (chunk.code[i] == BC_GET_GLOBAL || chunk.code[i] == BC_SET_GLOBAL || chunk.code[i] == BC_DEFINE_GLOBAL ||
                       chunk.code[i] == BC_MEMBER || chunk.code[i] == BC_DISPATCH || chunk.code[i] == BC_REGISTER_METHOD ||
                       chunk.code[i] == BC_STRUCT) {
                int idx = (chunk.code[i + 1] << 8) | chunk.code[i + 2];
                if (idx < chunk.constant_count && chunk.constants[idx].type == VAL_STRING) {
                    printf(" (%s)", chunk.constants[idx].as.string->chars);
                }
                i += 2;
            }
            printf("\n");
        }
        printf("===========================\n");
    }

    VM vm;
    vm_init(&vm, &compiler);
    vm.source = source;
    vm.source_name = filename;
    vm.prelude_line_count = g_prelude_line_count;

    if (!vm_run(&vm, false)) {
        fprintf(stderr, "Runtime error.\n");
        chunk_free(&chunk);
        vm_free(&vm);
        arena_destroy(arena);
        return 1;
    }

    vm_free(&vm);
    chunk_free(&chunk);
    arena_destroy(arena);
    return 0;
}

/* ─── Lumen file-based dev server (`vn dev`) ───
 * The language has no eval, so this is two passes: (1) run a tiny bootstrap
 * that calls the prelude's _lumen_build_dir() to compile pages/ into ONE
 * runnable app file; (2) run that generated app to serve it. Both passes go
 * through the normal read_file_with_modules + run_source pipeline so the
 * Lumen prelude is available to the generated code. */
/* Pass 1: compile pages/ into one runnable app at `out`. */
static int lumen_build(const char *pages, const char *out, const char *port) {
    const char *boot = ".lumen-boot.vn";
    FILE *bf = fopen(boot, "wb");
    if (!bf) { fprintf(stderr, "lumen: cannot create %s\n", boot); return 1; }
    fprintf(bf, "_lumen_build_dir(\"%s\", \"%s\", %s)\n", pages, out, port);
    fclose(bf);

    char *bsrc = read_file_with_modules(boot);
    if (!bsrc) { remove(boot); return 1; }
    g_varian_script_path = boot;
    int r = run_source(bsrc, boot);
    free(bsrc);
    remove(boot);
    return r;
}

/* Sum the mtimes of every .lumen file in `dir` — a cheap change fingerprint. */
static long lumen_pages_fingerprint(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    long sum = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len > 6 && strcmp(e->d_name + len - 6, ".lumen") == 0) {
            char path[2048];
            snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            struct stat st;
            if (stat(path, &st) == 0) sum += (long)st.st_mtime + (long)len;
        }
    }
    closedir(d);
    return sum;
}

/* Spawn the generated app as a child process (this same binary, `run <app>`). */
static pid_t lumen_spawn_server(const char *app) {
    pid_t pid = fork();
    if (pid == 0) {
        setenv("LUMEN_QUIET", "1", 1);  /* silence the child's own listen banner */
        char exe[2048];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n <= 0) { _exit(127); }
        exe[n] = '\0';
        execl(exe, exe, "run", app, (char *)NULL);
        _exit(127); /* exec failed */
    }
    return pid;
}

/* ─── Lumen dev console — the interactive Nuxt/Next-style startup banner ─── */
#define LUM_RESET   "\033[0m"
#define LUM_AMBER   "\033[38;5;214m"
#define LUM_AMBERB  "\033[1;38;5;214m"
#define LUM_DIM     "\033[2m"
#define LUM_GRAY    "\033[38;5;245m"
#define LUM_WHITEB  "\033[1;38;5;231m"
#define LUM_GREEN   "\033[38;5;42m"
#define LUM_CHIP    "\033[48;5;236m\033[1;38;5;214m"

/* Colour only when writing to a real terminal and the user hasn't opted out. */
static int lumen_color(void) {
    return isatty(STDOUT_FILENO) && getenv("NO_COLOR") == NULL;
}

static double lumen_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

static int lumen_name_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Collect, sort, and return the .lumen filenames in `dir` (caller frees each
 * entry). Returns the count; fills `names` up to `max`. */
static int lumen_collect_pages(const char *dir, char **names, int max) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max) {
        size_t len = strlen(e->d_name);
        if (len > 6 && strcmp(e->d_name + len - 6, ".lumen") == 0)
            names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof(char *), lumen_name_cmp);
    return n;
}

/* The branded "server is up" banner: a LUMEN chip, the local URL, and a
 * file→route table — the same orienting summary Nuxt/Next/Vite print. */
static void lumen_print_banner(const char *pages, const char *port, double ms) {
    int c = lumen_color();
    const char *A    = c ? LUM_AMBER  : "";
    const char *D    = c ? LUM_DIM    : "";
    const char *G    = c ? LUM_GRAY   : "";
    const char *W    = c ? LUM_WHITEB : "";
    const char *GR   = c ? LUM_GREEN  : "";
    const char *R    = c ? LUM_RESET  : "";
    const char *CHIP = c ? LUM_CHIP   : "";

    char *names[256];
    int n = lumen_collect_pages(pages, names, 256);

    printf("\n  %s LUMEN %s  %sv%s%s   %sthe Varian frontend framework%s\n\n",
           CHIP, R, G, VARIAN_VERSION, R, D, R);
    printf("  %s➜%s  %sLocal%s     %shttp://localhost:%s/%s\n", A, R, W, R, A, port, R);
    printf("  %s➜%s  %sPages%s     %s%d%s %sin %s/%s\n\n", A, R, W, R, W, n, R, D, pages, R);

    for (int i = 0; i < n; i++) {
        size_t bl = strlen(names[i]) - 6;          /* strip ".lumen" */
        char base[256], route[300];
        if (bl >= sizeof(base)) bl = sizeof(base) - 1;
        memcpy(base, names[i], bl);
        base[bl] = '\0';
        if (strcmp(base, "index") == 0) snprintf(route, sizeof(route), "/");
        else                            snprintf(route, sizeof(route), "/%s", base);
        printf("     %s●%s %s%-18s%s %s%s%s\n", A, R, W, route, R, G, names[i], R);
        free(names[i]);
    }
    if (n) printf("\n");
    printf("  %s✔%s ready in %s%.0f ms%s  %s· watching %s/ — edit a page to hot-reload%s\n\n",
           GR, R, W, ms, R, D, pages, R);
}

/* Copy one file byte-for-byte (binary-safe). Returns 0 on success. */
static int lumen_copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[8192];
    size_t r;
    int ok = 0;
    while ((r = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, r, out) != r) { ok = -1; break; }
    }
    fclose(in);
    fclose(out);
    return ok;
}

/* Copy the bundled favicon/manifest set into a project's public/ dir. The
 * assets ship alongside vn_modules so an installed binary finds them too. */
static int lumen_copy_assets(const char *public_dir) {
    const char *mod = resolve_vn_modules_dir();
    if (!mod) return -1;
    char assets[2048];
    snprintf(assets, sizeof(assets), "%s/lumen_assets", mod);
    DIR *d = opendir(assets);
    if (!d) return -1;
    int copied = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char src[4608], dst[4608];
        snprintf(src, sizeof(src), "%s/%s", assets, e->d_name);
        snprintf(dst, sizeof(dst), "%s/%s", public_dir, e->d_name);
        if (lumen_copy_file(src, dst) == 0) copied++;
    }
    closedir(d);
    return copied;
}

/* `vn dev`: build pages/, serve, and live-reload on file changes. The browser's
 * client runtime already auto-reconnects on socket close and re-renders, so a
 * rebuild+restart cycle is a full hot reload with no extra client code. A
 * rebuild that fails (e.g. a mid-edit syntax error) keeps the last good server
 * running and prints the error, rather than dropping the page. */
static int lumen_dev(const char *pages, const char *port) {
    const char *app = ".lumen-build.vn";
    double t0 = lumen_now_ms();
    if (lumen_build(pages, app, port) != 0) {
        fprintf(stderr, "lumen: build failed.\n");
        return 1;
    }

    pid_t child = lumen_spawn_server(app);
    if (child < 0) {
        /* fork unavailable — fall back to a plain blocking serve. */
        char *asrc = read_file_with_modules(app);
        if (!asrc) return 1;
        g_varian_script_path = app;
        int r = run_source(asrc, app);
        free(asrc);
        return r;
    }

    lumen_print_banner(pages, port, lumen_now_ms() - t0);
    fflush(stdout);

    int color = lumen_color();
    const char *A  = color ? LUM_AMBER  : "";
    const char *D  = color ? LUM_DIM    : "";
    const char *GR = color ? LUM_GREEN  : "";
    const char *RED= color ? "\033[38;5;203m" : "";
    const char *R  = color ? LUM_RESET  : "";

    long last = lumen_pages_fingerprint(pages);
    struct timespec poll = { 0, 400L * 1000000L }; /* 400ms */
    for (;;) {
        nanosleep(&poll, NULL);

        /* Did the server exit on its own (e.g. Ctrl-C / crash)? */
        int status;
        if (waitpid(child, &status, WNOHANG) == child) break;

        long now = lumen_pages_fingerprint(pages);
        if (now != last && now != -1) {
            last = now;
            double rt0 = lumen_now_ms();
            if (lumen_build(pages, app, port) == 0) {
                kill(child, SIGTERM);
                waitpid(child, NULL, 0);
                child = lumen_spawn_server(app);
                printf("  %s↻%s %shot-reloaded%s %sin %.0f ms%s\n",
                       A, R, GR, R, D, lumen_now_ms() - rt0, R);
            } else {
                printf("  %s✖%s build error — %skeeping the last good page up%s\n", RED, R, D, R);
            }
            fflush(stdout);
        }
    }
    return 0;
}

/* Scaffold a starter Lumen project: <name>/pages/index.lumen + a public/
 * dir pre-loaded with the favicon set (exactly like create-next-app /
 * nuxi init / create-vite ship a default favicon). */
static int lumen_new(const char *name) {
    char dir[1024], pub[1024];
    snprintf(dir, sizeof(dir), "%s/pages", name);
    snprintf(pub, sizeof(pub), "%s/public", name);
    char cmd[2400];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s' '%s'", dir, pub);
    if (system(cmd) != 0) { fprintf(stderr, "lumen: cannot create %s\n", dir); return 1; }

    char page[1100];
    snprintf(page, sizeof(page), "%s/index.lumen", dir);
    FILE *f = fopen(page, "wb");
    if (!f) { fprintf(stderr, "lumen: cannot write %s\n", page); return 1; }
    fputs(
        "<template>\n"
        "<main style=\"min-height:100vh;display:grid;place-items:center;text-align:center;color:#e8eaf0;background:radial-gradient(1200px 600px at 50% -10%,#1a2236,#0f1422)\">\n"
        "  <section style=\"padding:48px\">\n"
        "    <svg @click=\"pulse\" viewBox=\"0 0 48 48\" width=\"150\" height=\"150\" role=\"button\" aria-label=\"Lumen\" style=\"cursor:pointer;filter:drop-shadow(0 16px 38px rgba(0,0,0,.55))\">\n"
        "      <rect x=\"3\" y=\"3\" width=\"42\" height=\"42\" rx=\"12\" fill=\"#1b2233\"/>\n"
        "      <path d=\"M26 7 L15 27 h7 L19 41 L33 21 h-8 L29 7 Z\" fill=\"{{ color }}\" style=\"transition:fill .35s ease\"/>\n"
        "    </svg>\n"
        "    <h1 style=\"font-size:40px;font-weight:800;margin:30px 0 6px;letter-spacing:-.5px\">Welcome to <span style=\"color:#f5b829\">Lumen</span></h1>\n"
        "    <p style=\"color:#8b93a7;margin:0 0 6px\">Click the logo — it re-renders on the server and morphs the DOM live.</p>\n"
        "    <p style=\"color:#5b6478;font-size:14px;margin:0\">pulse <b style=\"color:#e8eaf0\">{{ count }}</b> · edit <code style=\"color:#8b93a7\">pages/index.lumen</code> to hot-reload</p>\n"
        "  </section>\n"
        "</main>\n"
        "</template>\n"
        "<script>\n"
        "// The logo colour is plain server state. Each click recomputes it and\n"
        "// Lumen morphs only the changed attribute into the live DOM — no client\n"
        "// JS, no hydration. That is the whole reactivity model.\n"
        "fn _hue(n) {\n"
        "  let palette = [\"#f5b829\", \"#ff6b6b\", \"#4dd4ac\", \"#5b9cff\", \"#c77dff\", \"#ff9f43\"]\n"
        "  return palette[n % 6]\n"
        "}\n"
        "fn state() {\n"
        "  return { count: 0, color: \"#f5b829\" }\n"
        "}\n"
        "fn pulse(s, v) {\n"
        "  let n = s.get(\"count\") + 1\n"
        "  return s.set(\"count\", n).set(\"color\", _hue(n))\n"
        "}\n"
        "</script>\n", f);
    fclose(f);

    int assets = lumen_copy_assets(pub);

    int color = lumen_color();
    const char *A    = color ? LUM_AMBER  : "";
    const char *D    = color ? LUM_DIM    : "";
    const char *W    = color ? LUM_WHITEB : "";
    const char *GR   = color ? LUM_GREEN  : "";
    const char *R    = color ? LUM_RESET  : "";
    const char *CHIP = color ? LUM_CHIP   : "";

    printf("\n  %s LUMEN %s  %screated %s%s%s\n\n", CHIP, R, D, W, name, R);
    printf("  %s✔%s pages/index.lumen\n", GR, R);
    if (assets > 0)
        printf("  %s✔%s public/ %s(%d assets — favicons + manifest)%s\n", GR, R, D, assets, R);
    printf("\n  %sNext steps:%s\n\n", D, R);
    printf("    %scd%s %s\n", A, R, name);
    printf("    %svn dev%s\n\n", A, R);
    printf("  %sthen open%s %shttp://localhost:8090/%s\n\n", D, R, A, R);
    return 0;
}

/* ─── REPL ─── */
static void repl(void) {
    printf("Varian v" VARIAN_VERSION " REPL\n");
    printf("Type 'exit' to quit.\n\n");

    char line[4096];
    for (;;) {
        printf(">> ");
        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n");
            break;
        }

        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';

        if (strcmp(line, "exit") == 0)
            break;

        if (strlen(line) > 0) {
            run_source(line, "<repl>");
        }
    }
}

/* ─── Help ─── */
static void print_help(const char *prog) {
    printf("Varian v" VARIAN_VERSION "\n");
    printf("Usage:\n");
    printf("  %s            Start interactive REPL\n", prog);
    printf("  %s run <file>    Execute a Varian script\n", prog);
    printf("  %s fmt <file>    Format a Varian script in-place\n", prog);
    printf("  %s test [dir] [--filter <substr>] [--timeout <secs>]\n", prog);
    printf("                   Run *_test.vn tests (default dir: .)\n");
    printf("  %s add <pkg>     Add a package dependency\n", prog);
    printf("  %s wrap <target> Generate wrapper for a foreign library\n", prog);
    printf("  %s lsp           Start LSP server\n", prog);
    printf("\n");
    printf("Lumen (frontend):\n");
    printf("  %s lumen new <name>          Scaffold a new Lumen app\n", prog);
    printf("  %s dev [dir] [port]          Serve a pages/ dir with live reload (default ./pages :8090)\n", prog);
    printf("  %s lumen build <dir> <out>   Compile pages/ into one runnable app\n", prog);
    printf("\n");
    printf("  %s --help        Show this help\n", prog);
    printf("\n");
    printf("Environment variables:\n");
    printf("  VN_DEBUG_AST       Print the AST before compilation\n");
    printf("  VN_DEBUG_BYTECODE  Print bytecode disassembly\n");
}

static void collect_fmt_files(const char *dir, char ***files, int *count, int *cap) {
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        size_t dlen = strlen(dir);
        size_t nlen = strlen(entry->d_name);
        char *full = (char *)malloc(dlen + nlen + 2);
        if (!full) continue;
        memcpy(full, dir, dlen);
        full[dlen] = '/';
        memcpy(full + dlen + 1, entry->d_name, nlen);
        full[dlen + 1 + nlen] = '\0';

        struct stat st;
        if (stat(full, &st) == -1) { free(full); continue; }

        if (S_ISDIR(st.st_mode)) {
            collect_fmt_files(full, files, count, cap);
            free(full);
        } else if (S_ISREG(st.st_mode)) {
            bool matches = false;
            if (nlen > 3 && strcmp(entry->d_name + nlen - 3, ".vn") == 0) {
                matches = true;
            } else if (nlen > 6 && strcmp(entry->d_name + nlen - 6, ".vhtml") == 0) {
                matches = true;
            }
            if (matches) {
                if (*count >= *cap) {
                    *cap = *cap ? *cap * 2 : 16;
                    *files = (char **)realloc(*files, (size_t)(*cap) * sizeof(char *));
                }
                (*files)[*count] = full;
                (*count)++;
            } else {
                free(full);
            }
        } else {
            free(full);
        }
    }
    closedir(d);
}

static int process_fmt_file(const char *path, bool check_only, bool show_diff) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "fmt: could not open '%s'\n", path);
        return 1;
    }
    fseek(file, 0L, SEEK_END);
    size_t size = (size_t)ftell(file);
    rewind(file);
    char *source = (char *)malloc(size + 1);
    if (!source) { fclose(file); return 1; }
    size_t nread = fread(source, 1, size, file);
    source[nread] = '\0';
    fclose(file);

    int out_pos = 0;
    char *out = fmt_format_source(source, size, &out_pos);
    if (!out) {
        free(source);
        return 1;
    }

    bool is_different = (size != (size_t)out_pos || memcmp(source, out, size) != 0);

    if (is_different) {
        if (check_only) {
            printf("%s\n", path);
        }
        if (show_diff) {
            char temp_path[] = "/tmp/vn_fmt_XXXXXX";
            int temp_fd = mkstemp(temp_path);
            if (temp_fd != -1) {
                ssize_t written = write(temp_fd, out, (size_t)out_pos);
                (void)written;
                close(temp_fd);
                char cmd[2048];
                snprintf(cmd, sizeof(cmd), "diff -u \"%s\" \"%s\"", path, temp_path);
                int rc = system(cmd);
                (void)rc;
                unlink(temp_path);
            }
        }
        if (!check_only && !show_diff) {
            FILE *outfile = fopen(path, "wb");
            if (!outfile) {
                fprintf(stderr, "fmt: could not write '%s'\n", path);
                free(source);
                free(out);
                return 1;
            }
            fwrite(out, 1, (size_t)out_pos, outfile);
            fclose(outfile);
            printf("formatted %s\n", path);
        }
    }

    free(source);
    free(out);
    return is_different ? 1 : 0;
}

static int process_fmt_stdin() {
    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) return 1;
    int c;
    while ((c = getchar()) != EOF) {
        if (len + 1 >= cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
        buf[len++] = (char)c;
    }
    buf[len] = '\0';

    int out_len = 0;
    char *formatted = fmt_format_source(buf, len, &out_len);
    free(buf);
    if (!formatted) return 1;

    fwrite(formatted, 1, out_len, stdout);
    free(formatted);
    return 0;
}

/* ─── Main ─── */
int main(int argc, char *argv[]) {
    /* Must happen once, before any thread (incl. cluster worker threads
     * spawned later by lib_http.c) can call into libcurl -- curl_easy_init()
     * does this lazily on first use otherwise, which is not safe if two
     * threads race to be the "first" caller. */
    curl_global_init(CURL_GLOBAL_ALL);
#ifdef VARIAN_AOT_STANDALONE
    (void)argc; (void)argv;
    VM vm;
    memset(&vm, 0, sizeof(VM));
    
    Compiler comp;
    memset(&comp, 0, sizeof(Compiler));
    vm_init(&vm, &comp);
    
    extern ObjFunction *varian_aot_load(VM *vm);
    vm.main_fn = varian_aot_load(&vm);
    
    bool success = vm_run(&vm, false);
    
    vm_free(&vm);
    if (vm.compiler) free(vm.compiler);
    return success ? 0 : 1;
#else
    if (argc == 1) {
        repl();
        return 0;
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_help(argv[0]);
        return 0;
    }

    if (strcmp(argv[1], "lsp") == 0) {
        return lsp_main();
    }

    if (strcmp(argv[1], "fmt") == 0) {
        const char *path = NULL;
        bool check_only = false;
        bool show_diff = false;
        bool use_stdin = false;

        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--check") == 0) {
                check_only = true;
            } else if (strcmp(argv[i], "--diff") == 0) {
                show_diff = true;
            } else if (strcmp(argv[i], "--stdin") == 0) {
                use_stdin = true;
            } else {
                path = argv[i];
            }
        }

        if (use_stdin) {
            return process_fmt_stdin();
        }

        if (!path) {
            path = ".";
        }

        struct stat st;
        if (stat(path, &st) == -1) {
            fprintf(stderr, "fmt: path '%s' does not exist\n", path);
            return 1;
        }

        int diff_count = 0;
        if (S_ISDIR(st.st_mode)) {
            char **files = NULL;
            int count = 0;
            int file_cap = 0;
            collect_fmt_files(path, &files, &count, &file_cap);

            for (int i = 0; i < count; i++) {
                diff_count += process_fmt_file(files[i], check_only, show_diff);
                free(files[i]);
            }
            free(files);
        } else {
            diff_count += process_fmt_file(path, check_only, show_diff);
        }

        if (check_only && diff_count > 0) {
            return 1;
        }
        return 0;
    }

    if (strcmp(argv[1], "test") == 0) {
        const char *dir = ".";
        const char *filter = NULL;
        int timeout_ms = 0;

        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
                filter = argv[++i];
            } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
                double secs = atof(argv[++i]);
                if (secs > 0) timeout_ms = (int)(secs * 1000);
            } else {
                dir = argv[i];
            }
        }
        return test_run_dir(dir, filter, timeout_ms);
    }

    if (strcmp(argv[1], "lint") == 0) {
        const char *path = ".";
        const char *only_category = NULL;
        const char *format = NULL;

        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--only") == 0 && i + 1 < argc) {
                only_category = argv[++i];
            } else if (strcmp(argv[i], "--format") == 0 && i + 1 < argc) {
                format = argv[++i];
            } else {
                path = argv[i];
            }
        }
        return run_lint(path, only_category, format);
    }

    if (strcmp(argv[1], "add") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Usage: %s add <package>\n", argv[0]);
            return 1;
        }
        return pkg_add(argv[2]);
    }

    if (strcmp(argv[1], "wrap") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Usage: %s wrap <target>\n", argv[0]);
            fprintf(stderr, "  targets: python:<module>\n");
            return 1;
        }
        return pkg_wrap(argv[2]);
    }

    if (strcmp(argv[1], "compile") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s compile <file.vn> [output.c]\n", argv[0]);
            return 1;
        }
        const char *out_path = (argc >= 4) ? argv[3] : "aot_output.c";
        char *source = read_file_with_modules(argv[2]);
        if (!source) return 1;
        int result = aot_compile(source, argv[2], out_path);
        free(source);
        return result;
    }

    if (strcmp(argv[1], "run") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Usage: %s run <file.vn>\n", argv[0]);
            return 1;
        }
        char *source = read_file_with_modules(argv[2]);
        if (!source) return 1;
        g_varian_script_path = argv[2];
        int result = run_source(source, argv[2]);
        free(source);
        return result;
    }

    /* `vn dev [pagesdir] [port]` — Lumen file-based dev server. */
    if (strcmp(argv[1], "dev") == 0) {
        const char *pages = (argc >= 3) ? argv[2] : "pages";
        const char *port  = (argc >= 4) ? argv[3] : "8090";
        return lumen_dev(pages, port);
    }

    /* `vn lumen <new|dev|build> ...` — Lumen project tooling. */
    if (strcmp(argv[1], "lumen") == 0) {
        if (argc >= 3 && strcmp(argv[2], "new") == 0) {
            if (argc < 4) { fprintf(stderr, "Usage: %s lumen new <name>\n", argv[0]); return 1; }
            return lumen_new(argv[3]);
        }
        if (argc >= 3 && strcmp(argv[2], "dev") == 0) {
            const char *pages = (argc >= 4) ? argv[3] : "pages";
            const char *port  = (argc >= 5) ? argv[4] : "8090";
            return lumen_dev(pages, port);
        }
        if (argc >= 3 && strcmp(argv[2], "build") == 0) {
            if (argc < 5) { fprintf(stderr, "Usage: %s lumen build <pagesdir> <out.vn> [port]\n", argv[0]); return 1; }
            const char *port = (argc >= 6) ? argv[5] : "8090";
            return lumen_build(argv[3], argv[4], port);
        }
        fprintf(stderr, "Usage: %s lumen <new <name> | dev [dir] [port] | build <dir> <out.vn> [port]>\n", argv[0]);
        return 1;
    }

    /* Default: try to run file directly (backward compat) */
    if (argc == 2) {
        char *source = read_file_with_modules(argv[1]);
        if (!source) return 1;
        
        FILE *dump = fopen("debug_source.vn", "wb");
        if (dump) {
            fwrite(source, 1, strlen(source), dump);
            fclose(dump);
        }
        
        g_varian_script_path = argv[1];
        int result = run_source(source, argv[1]);
        free(source);
        return result;
    }

    fprintf(stderr, "Unknown command '%s'. Use '%s --help' for usage.\n", argv[1], argv[0]);
    return 1;
#endif
}
