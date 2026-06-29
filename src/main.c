#include "varian.h"
#include "lexer.h"
#include "parser.h"
#include "vm.h"
#include "fmt.h"
#include "test_runner.h"
#include "pkg_manager.h"
#include "vnb.h"
#include "lint.h"
#include "lsp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#endif
#include <sys/stat.h>
#ifdef _WIN32
/* <windows.h> (via winnt.h) defines a TOKEN_TYPE typedef and a TokenType
 * enumerator that collide with VarianLang's lexer token names. Rename the
 * Windows symbols across the windows.h include only; our enum/typedef are
 * restored by the #undef immediately after. */
#define TOKEN_TYPE WIN_TOKEN_TYPE
#define TokenType  WIN_TokenType
#include <windows.h>
#undef TOKEN_TYPE
#undef TokenType
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#endif
#ifndef VN_NO_HTTP
#include <curl/curl.h>
#endif

/* Set before vm_run() for "vn <file>"/"vn run <file>" so lib_http.c's
 * cluster worker threads can independently re-load the same script. */
const char *g_varian_script_path = NULL;
static int g_prelude_line_count = 0;

static uint64_t fnv1a_hash(const char *str, size_t len) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint8_t)str[i];
        hash *= 0x00000100000001B3ULL;
    }
    return hash;
}

static void ensure_dir(const char *path) {
    mkdir(path, 0755);
}

static bool copy_file(const char *src, const char *dst) {
    FILE *sf = fopen(src, "rb");
    if (!sf) return false;
    FILE *df = fopen(dst, "wb");
    if (!df) { fclose(sf); return false; }
    char buf[8192];
    size_t bytes;
    while ((bytes = fread(buf, 1, sizeof(buf), sf)) > 0) {
        fwrite(buf, 1, bytes, df);
    }
    fclose(sf);
    fclose(df);
    return true;
}

static void collect_assets_recursive(const char *dir_path, VMAsset **assets, int *count, int *capacity) {
    DIR *d = opendir(dir_path);
    if (!d) return;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        struct stat st;
        if (stat(full_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                collect_assets_recursive(full_path, assets, count, capacity);
            } else if (S_ISREG(st.st_mode)) {
                FILE *f = fopen(full_path, "rb");
                if (f) {
                    fseek(f, 0, SEEK_END);
                    long sz = ftell(f);
                    rewind(f);
                    unsigned char *data = malloc((size_t)sz);
                    size_t read_bytes = fread(data, 1, (size_t)sz, f);
                    fclose(f);
                    
                    if (*count >= *capacity) {
                        *capacity = *capacity ? *capacity * 2 : 16;
                        *assets = realloc(*assets, (size_t)(*capacity) * sizeof(VMAsset));
                    }
                    (*assets)[*count].path = strdup(full_path);
                    (*assets)[*count].data = data;
                    (*assets)[*count].size = (int)read_bytes;
                    (*count)++;
                }
            }
        }
    }
    closedir(d);
}

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

/* Locate the standard-library prelude directory (the top-level vn_modules/*.vn
 * the runtime auto-loads). This is a TOOLCHAIN location, deliberately resolved
 * independent of CWD: a user's project has its OWN ./vn_modules holding vendored
 * Constellation packages (subdirs, loaded on-demand via `use`), and that must
 * NOT be mistaken for the stdlib — otherwise a project with deps would shadow
 * and lose the entire stdlib. Search order: $VARIAN_HOME, then relative to the
 * executable (repo dev + install layouts), and only as a last resort ./vn_modules
 * (for a standalone tree with no install). Returns a static buffer or NULL. */
static const char *resolve_vn_modules_dir(void) {
    static char found[2048];
    DIR *d;

    const char *home = getenv("VARIAN_HOME");
    if (home && *home) {
        snprintf(found, sizeof(found), "%s/vn_modules", home);
        if ((d = opendir(found))) { closedir(d); return found; }
    }

    char exe[2048];
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof(exe));
    if (n > 0 && n < sizeof(exe)) { exe[n] = '\0'; }
#else
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) { exe[n] = '\0'; }
#endif
    if (n > 0) {
        char *slash = strrchr(exe, '/');
#ifdef _WIN32
        if (!slash) slash = strrchr(exe, '\\');
#endif
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

    /* Last resort only — a source tree run with no install and no $VARIAN_HOME. */
    if ((d = opendir("vn_modules"))) {
        closedir(d);
        snprintf(found, sizeof(found), "vn_modules");
        return found;
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
#ifdef _WIN32
    char exe[2048];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof(exe));
    if (n <= 0 || n >= sizeof(exe)) return -1;
    exe[n] = '\0';
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "\"%s\" run \"%s\"", exe, app);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    SetEnvironmentVariableA("LUMEN_QUIET", "1");
    SetEnvironmentVariableA("LUMEN_DEV", "aurora");
    SetEnvironmentVariableA("VN_VERSION", VARIAN_VERSION);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        return -1;
    }
    CloseHandle(pi.hThread);
    return (pid_t)(intptr_t)pi.hProcess;
#else
    pid_t pid = fork();
    if (pid == 0) {
        setenv("LUMEN_QUIET", "1", 1);
        setenv("LUMEN_DEV", "aurora", 0);
        setenv("VN_VERSION", VARIAN_VERSION, 1);
        char exe[2048];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n <= 0) { _exit(127); }
        exe[n] = '\0';
        execl(exe, exe, "run", app, (char *)NULL);
        _exit(127);
    }
    return pid;
#endif
}

/* ─── Lumen dev console — the interactive Nuxt/Next-style startup banner ─── */
#define LUM_RESET   "\033[0m"
#define LUM_AURA1   "\033[38;5;75m"     /* Aurora blue #4FACFE */
#define LUM_AURA2   "\033[38;5;51m"     /* Aurora cyan #00F2FE */
#define LUM_AURAB   "\033[1;38;5;75m"   /* bold blue */
#define LUM_AMBER   "\033[38;5;214m"    /* Lumen amber (kept for lumen_add/lumen_dev) */
#define LUM_DIM     "\033[2m"
#define LUM_GRAY    "\033[38;5;245m"
#define LUM_WHITEB  "\033[1;38;5;231m"
#define LUM_GREEN   "\033[38;5;42m"
#define LUM_CHIP    "\033[48;5;236m\033[1;38;5;75m"

/* Colour only when writing to a real terminal and the user hasn't opted out. */
static int lumen_color(void) {
    return isatty(STDOUT_FILENO) && getenv("NO_COLOR") == NULL;
}

static double lumen_now_ms(void) {
#ifdef _WIN32
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
#endif
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

/* The branded "server is up" banner — clean Nuxt/Next-style startup summary
 * with the Aurora blue-cyan identity. */
static void lumen_print_banner(const char *pages, const char *port, double ms) {
    int c = lumen_color();
    const char *B    = c ? LUM_AURA1  : "";   /* Aurora blue */
    const char *C    = c ? LUM_AURA2  : "";   /* Aurora cyan */
    const char *BB   = c ? LUM_AURAB  : "";   /* bold blue */
    const char *D    = c ? LUM_DIM    : "";
    const char *G    = c ? LUM_GRAY   : "";
    const char *W    = c ? LUM_WHITEB : "";
    const char *GR   = c ? LUM_GREEN  : "";
    const char *R    = c ? LUM_RESET  : "";

    char *names[256];
    int n = lumen_collect_pages(pages, names, 256);

    /* Title line: diamond + "Aurora" + version + tagline */
    printf("\n  %s☯%s  %sAurora%s %sv%s%s  %s──  %sfullstack Varian platform%s\n\n",
           B, R, BB, R, G, VARIAN_VERSION, R, D, D, R);
    printf("    %s➜%s  %sLocal%s    %shttp://localhost:%s/%s\n", B, R, W, R, B, port, R);
    printf("    %s➜%s  %sPages%s    %s%d%s %sin %s/%s\n\n", B, R, W, R, W, n, R, D, pages, R);

    for (int i = 0; i < n; i++) {
        size_t bl = strlen(names[i]) - 6;
        char base[256], route[300];
        if (bl >= sizeof(base)) bl = sizeof(base) - 1;
        memcpy(base, names[i], bl);
        base[bl] = '\0';
        if (strcmp(base, "index") == 0) snprintf(route, sizeof(route), "/");
        else                            snprintf(route, sizeof(route), "/%s", base);
        printf("       %s●%s %s%-18s%s %s%s%s\n", C, R, W, route, R, G, names[i], R);
        free(names[i]);
    }
    if (n) printf("\n");
    printf("  %s✔%s %sReady%s in %s%.0f ms%s  %s· watching %s/ — edit a page to hot-reload%s\n\n",
           GR, R, W, R, W, ms, R, D, pages, R);
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
#ifdef _WIN32
    HANDLE hChild = (HANDLE)(intptr_t)child;
    for (;;) {
        Sleep(400);
        if (WaitForSingleObject(hChild, 0) == WAIT_OBJECT_0) break;
        long now = lumen_pages_fingerprint(pages);
        if (now != last && now != -1) {
            last = now;
            double rt0 = lumen_now_ms();
            if (lumen_build(pages, app, port) == 0) {
                TerminateProcess(hChild, 0);
                WaitForSingleObject(hChild, INFINITE);
                CloseHandle(hChild);
                child = lumen_spawn_server(app);
                hChild = (HANDLE)(intptr_t)child;
                printf("  %s↻%s %shot-reloaded%s %sin %.0f ms%s\n",
                       A, R, GR, R, D, lumen_now_ms() - rt0, R);
            } else {
                printf("  %s✖%s build error — %skeeping the last good page up%s\n", RED, R, D, R);
            }
            fflush(stdout);
        }
    }
#else
    struct timespec poll = { 0, 400L * 1000000L };
    for (;;) {
        nanosleep(&poll, NULL);
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
#endif
    return 0;
}

/* Scaffold a starter Aurora project: <name>/pages/index.lumen + main.vn +
 * constellation.toml + lib/ + public/ with favicon assets. */
static int lumen_new(const char *name) {
    /* `name` is the path the user passed (e.g. "./shop" or "/tmp/app"); the
     * project's identity is its basename, not the whole path. Paths below keep
     * using `name`; identity strings (toml name, README title) use `base`. */
    const char *base = strrchr(name, '/');
    base = (base && base[1]) ? base + 1 : name;
    char dir[1024], pub[1024], lib_dir[1024];
    snprintf(dir, sizeof(dir), "%s/pages", name);
    snprintf(pub, sizeof(pub), "%s/public", name);
    snprintf(lib_dir, sizeof(lib_dir), "%s/lib", name);
    char cmd[2400];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "if not exist \"%s\" mkdir \"%s\" & if not exist \"%s\" mkdir \"%s\" & if not exist \"%s\" mkdir \"%s\"", dir, dir, pub, pub, lib_dir, lib_dir);
#else
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s' '%s' '%s'", dir, pub, lib_dir);
#endif
    if (system(cmd) != 0) { fprintf(stderr, "lumen: cannot create %s\n", dir); return 1; }

    /* ── constellation.toml ── */
    char toml[1024];
    snprintf(toml, sizeof(toml), "%s/constellation.toml", name);
    FILE *tf = fopen(toml, "wb");
    if (tf) {
        fprintf(tf, "[package]\n");
        fprintf(tf, "name = \"%s\"\n", base);
        fprintf(tf, "version = \"0.1.0\"\n");
        fprintf(tf, "kind = \"aurora\"\n\n");
        fprintf(tf, "[capabilities]\n");
        fprintf(tf, "ffi = false\n");
        fprintf(tf, "python = false\n");
        fprintf(tf, "net = false\n");
        fprintf(tf, "fs = false\n");
        fclose(tf);
    }

    /* ── main.vn (Zenith app with Lumen pages mounted) ── */
    char main_vn[1024];
    snprintf(main_vn, sizeof(main_vn), "%s/main.vn", name);
    FILE *mf = fopen(main_vn, "wb");
    if (mf) {
        fputs("// Aurora — fullstack Varian app.\n", mf);
        fputs("//\n", mf);
        fputs("// Your Lumen UI lives in pages/ and is served by `vn dev` (development)\n", mf);
        fputs("// and `vn build` (production). main.vn is the Zenith backend half:\n", mf);
        fputs("// the custom JSON / API endpoints your frontend calls. Run it with:\n", mf);
        fputs("//   vn run main.vn\n\n", mf);
        fputs("let app = new_app()\n\n", mf);
        fputs("// Example API route. Add your own below; the frontend fetches these.\n", mf);
        fputs("app.get(\"/api/health\", |_req| {\n", mf);
        fputs("  return Response { status: 200, body: \"{\\\"ok\\\":true}\", content_type: \"application/json\" }\n", mf);
        fputs("}, \"Health check\", null)\n\n", mf);
        fputs("app.listen(8091)\n", mf);
        fclose(mf);
    }

    /* ── lib/config.vn (so the scaffolded lib/ isn't an empty promise) ── */
    char libcfg[1100];
    snprintf(libcfg, sizeof(libcfg), "%s/config.vn", lib_dir);
    FILE *lf = fopen(libcfg, "wb");
    if (lf) {
        fputs("// Project configuration. Keep secrets OUT of source — read them from the\n", lf);
        fputs("// environment and provide safe local fallbacks here. `use \"lib/config.vn\"`\n", lf);
        fputs("// from main.vn to share these across the backend.\n\n", lf);
        fprintf(lf, "let APP_NAME = \"%s\"\n", base);
        fputs("let API_PORT = 8091\n", lf);
        fclose(lf);
    }

    /* ── pages/index.lumen (Aurora vocabulary) ── */
    char page[4096];
    snprintf(page, sizeof(page), "%s/index.lumen", dir);
    FILE *f = fopen(page, "wb");
    if (!f) { fprintf(stderr, "lumen: cannot write %s\n", page); return 1; }
    fputs(
        "<template>\n"
        "<Page>\n"
        "  <Section>\n"
        "    <Container size=\"sm\">\n"
        "      <Stack gap=\"6\" align=\"center\">\n"
        "        <Hero eyebrow=\"Welcome\" title=\"Aurora\"\n"
        "              subtitle=\"The fullstack Varian platform — Zenith on the server, Lumen in the browser.\">\n"
        "          <Button variant=\"primary\" on=\"pulse\">Clicked {{ count }} times</Button>\n"
        "        </Hero>\n"
        "        <Text muted>Edit pages/index.lumen and save — it hot-reloads.</Text>\n"
        "      </Stack>\n"
        "    </Container>\n"
        "  </Section>\n"
        "</Page>\n"
        "</template>\n"
        "<script>\n"
        "fn state() {\n"
        "  return { count: 0 }\n"
        "}\n"
        "// Server-driven: the click goes to Zenith over the LumenJS socket, this\n"
        "// runs, and the new HTML is sent back. _v is the (unused) event value.\n"
        "fn pulse(s, _v) {\n"
        "  return s.set(\"count\", s.get(\"count\") + 1)\n"
        "}\n"
        "</script>\n", f);
    fclose(f);

    /* ── README.md ── */
    char readme[1024];
    snprintf(readme, sizeof(readme), "%s/README.md", name);
    FILE *rf = fopen(readme, "wb");
    if (rf) {
        fprintf(rf, "# %s\n\n", base);
        fprintf(rf, "Built with [Aurora](https://aurora.dev) — the fullstack Varian platform.\n\n");
        fprintf(rf, "## Run\n\n```\ncd %s\nvn dev\n```\n\nThen open http://localhost:8090/ — edit `pages/index.lumen` to hot-reload.\n", base);
        fclose(rf);
    }

    int assets = lumen_copy_assets(pub);

    int color = lumen_color();
    const char *B    = color ? LUM_AURA1  : "";
    const char *BB   = color ? LUM_AURAB  : "";
    const char *D    = color ? LUM_DIM    : "";
    const char *W    = color ? LUM_WHITEB : "";
    const char *GR   = color ? LUM_GREEN  : "";
    const char *R    = color ? LUM_RESET  : "";

    printf("\n  %s☯%s  %sAurora%s  %screated %s%s%s\n\n", B, R, BB, R, D, W, name, R);
    printf("  %s✔%s %spages/index.lumen%s\n", GR, R, W, R);
    printf("  %s✔%s %smain.vn%s\n", GR, R, W, R);
    printf("  %s✔%s %sconstellation.toml%s\n", GR, R, W, R);
    printf("  %s✔%s %slib/%s\n", GR, R, W, R);
    if (assets > 0)
        printf("  %s✔%s public/ %s(%d assets — favicons + manifest)%s\n", GR, R, D, assets, R);
    printf("\n  %sNext steps:%s\n\n", D, R);
    printf("    %scd%s %s\n", B, R, name);
    printf("    %svn dev%s\n\n", B, R);
    printf("  %sthen open%s %shttp://localhost:8090/%s\n\n", D, R, B, R);
    return 0;
}

static int lumen_add(const char *comp) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "pages/components");
    char cmd[2048];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "if not exist \"%s\" mkdir \"%s\"", dir, dir);
#else
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir);
#endif
    if (system(cmd) != 0) {
        fprintf(stderr, "Lumen UI: cannot create directory %s\n", dir);
        return 1;
    }
    
    const char *content = NULL;
    const char *filename = NULL;
    
    if (strcasecmp(comp, "button") == 0) {
        filename = "pages/components/Button.lumen";
        content = 
            "<template>\n"
            "  <button class=\"btn {{ variant }} {{ size }}\" @click=\"click\">\n"
            "    {{! children }}\n"
            "  </button>\n"
            "</template>\n"
            "\n"
            "<style>\n"
            ".btn {\n"
            "  display: inline-flex;\n"
            "  align-items: center;\n"
            "  justify-content: center;\n"
            "  border-radius: 8px;\n"
            "  font-weight: 500;\n"
            "  font-family: inherit;\n"
            "  cursor: pointer;\n"
            "  transition: all 0.2s ease;\n"
            "  border: 1px solid transparent;\n"
            "  outline: none;\n"
            "}\n"
            ".btn:focus-visible {\n"
            "  box-shadow: 0 0 0 2px var(--lumen-ring);\n"
            "}\n"
            "/* Variants */\n"
            ".primary {\n"
            "  background: var(--lumen-primary);\n"
            "  color: var(--lumen-primary-fg);\n"
            "}\n"
            ".primary:hover {\n"
            "  background: var(--lumen-primary-hover);\n"
            "}\n"
            ".secondary {\n"
            "  background: var(--lumen-muted);\n"
            "  color: var(--lumen-fg);\n"
            "  border-color: var(--lumen-border);\n"
            "}\n"
            ".secondary:hover {\n"
            "  background: var(--lumen-border);\n"
            "}\n"
            ".outline {\n"
            "  background: transparent;\n"
            "  color: var(--lumen-fg);\n"
            "  border-color: var(--lumen-border);\n"
            "}\n"
            ".outline:hover {\n"
            "  background: var(--lumen-muted);\n"
            "}\n"
            "/* Sizes */\n"
            ".sm {\n"
            "  padding: 6px 12px;\n"
            "  font-size: 13px;\n"
            "}\n"
            ".md {\n"
            "  padding: 10px 18px;\n"
            "  font-size: 14px;\n"
            "}\n"
            ".lg {\n"
            "  padding: 14px 24px;\n"
            "  font-size: 16px;\n"
            "}\n"
            "</style>\n"
            "\n"
            "<script>\n"
            "fn state() {\n"
            "  return { variant: \"primary\", size: \"md\" }\n"
            "}\n"
            "fn click(s, v) {\n"
            "  return s\n"
            "}\n"
            "</script>\n";
    } else if (strcasecmp(comp, "card") == 0) {
        filename = "pages/components/Card.lumen";
        content = 
            "<template>\n"
            "  <div class=\"card\">\n"
            "    {{! children }}\n"
            "  </div>\n"
            "</template>\n"
            "\n"
            "<style>\n"
            ".card {\n"
            "  background: var(--lumen-card);\n"
            "  border: 1px solid var(--lumen-border);\n"
            "  border-radius: 12px;\n"
            "  padding: 24px;\n"
            "  box-shadow: 0 4px 20px rgba(0,0,0,0.15);\n"
            "  color: var(--lumen-fg);\n"
            "}\n"
            "</style>\n"
            "\n"
            "<script>\n"
            "fn state() {\n"
            "  return {}\n"
            "}\n"
            "</script>\n";
    } else if (strcasecmp(comp, "input") == 0) {
        filename = "pages/components/Input.lumen";
        content = 
            "<template>\n"
            "  <input class=\"input\" type=\"{{ type }}\" placeholder=\"{{ placeholder }}\" value=\"{{ value }}\" @input=\"change\" />\n"
            "</template>\n"
            "\n"
            "<style>\n"
            ".input {\n"
            "  background: var(--lumen-input);\n"
            "  border: 1px solid var(--lumen-border);\n"
            "  color: var(--lumen-fg);\n"
            "  padding: 10px 14px;\n"
            "  border-radius: 8px;\n"
            "  font-size: 14px;\n"
            "  font-family: inherit;\n"
            "  width: 100%;\n"
            "  box-sizing: border-box;\n"
            "  outline: none;\n"
            "  transition: border-color 0.2s ease, box-shadow 0.2s ease;\n"
            "}\n"
            ".input:focus {\n"
            "  border-color: var(--lumen-primary);\n"
            "  box-shadow: 0 0 0 2px var(--lumen-ring);\n"
            "}\n"
            "</style>\n"
            "\n"
            "<script>\n"
            "fn state() {\n"
            "  return { type: \"text\", placeholder: \"\", value: \"\" }\n"
            "}\n"
            "fn change(s, v) {\n"
            "  return s.set(\"value\", v)\n"
            "}\n"
            "</script>\n";
    } else if (strcasecmp(comp, "dialog") == 0) {
        filename = "pages/components/Dialog.lumen";
        content = 
            "<template>\n"
            "  <div class=\"dialog-overlay {{#if open}}active{{/if}}\" @click=\"close\">\n"
            "    <div class=\"dialog-content\" @click=\"noop\">\n"
            "      <header class=\"dialog-header\">\n"
            "        <h2 class=\"dialog-title\">{{ title }}</h2>\n"
            "        <button class=\"close-btn\" @click=\"close\">\n"
            "          <svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1em\" height=\"1em\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M18 6 6 18\"/><path d=\"m6 6 12 12\"/></svg>\n"
            "        </button>\n"
            "      </header>\n"
            "      <div class=\"dialog-body\">\n"
            "        {{! children }}\n"
            "      </div>\n"
            "    </div>\n"
            "  </div>\n"
            "</template>\n"
            "\n"
            "<style>\n"
            ".dialog-overlay {\n"
            "  position: fixed;\n"
            "  inset: 0;\n"
            "  background: var(--lumen-overlay);\n"
            "  display: flex;\n"
            "  align-items: center;\n"
            "  justify-content: center;\n"
            "  opacity: 0;\n"
            "  pointer-events: none;\n"
            "  transition: opacity 0.3s ease;\n"
            "  z-index: 1000;\n"
            "}\n"
            ".dialog-overlay.active {\n"
            "  opacity: 1;\n"
            "  pointer-events: auto;\n"
            "}\n"
            ".dialog-content {\n"
            "  background: var(--lumen-card);\n"
            "  border: 1px solid var(--lumen-border);\n"
            "  border-radius: 12px;\n"
            "  width: 90%;\n"
            "  max-width: 500px;\n"
            "  box-shadow: 0 24px 70px rgba(0,0,0,0.5);\n"
            "  transform: scale(0.95);\n"
            "  transition: transform 0.3s ease;\n"
            "  color: var(--lumen-fg);\n"
            "}\n"
            ".dialog-overlay.active .dialog-content {\n"
            "  transform: scale(1);\n"
            "}\n"
            ".dialog-header {\n"
            "  display: flex;\n"
            "  align-items: center;\n"
            "  justify-content: space-between;\n"
            "  padding: 16px 20px;\n"
            "  border-bottom: 1px solid var(--lumen-border);\n"
            "}\n"
            ".dialog-title {\n"
            "  margin: 0;\n"
            "  font-size: 18px;\n"
            "  font-weight: 700;\n"
            "}\n"
            ".close-btn {\n"
            "  background: transparent;\n"
            "  border: none;\n"
            "  color: var(--lumen-muted-fg);\n"
            "  cursor: pointer;\n"
            "  font-size: 16px;\n"
            "  padding: 4px;\n"
            "}\n"
            ".close-btn:hover {\n"
            "  color: var(--lumen-fg);\n"
            "}\n"
            ".dialog-body {\n"
            "  padding: 20px;\n"
            "}\n"
            "</style>\n"
            "\n"
            "<script>\n"
            "fn state() {\n"
            "  return { open: false, title: \"Dialog\" }\n"
            "}\n"
            "fn close(s, v) {\n"
            "  return s.set(\"open\", false)\n"
            "}\n"
            "fn noop(s, v) {\n"
            "  return s\n"
            "}\n"
            "</script>\n";
    } else if (strcasecmp(comp, "badge") == 0) {
        filename = "pages/components/Badge.lumen";
        content = 
            "<template>\n"
            "  <span class=\"badge {{ variant }}\">\n"
            "    {{! children }}\n"
            "  </span>\n"
            "</template>\n"
            "\n"
            "<style>\n"
            ".badge {\n"
            "  display: inline-flex;\n"
            "  align-items: center;\n"
            "  border-radius: 9999px;\n"
            "  padding: 2px 10px;\n"
            "  font-size: 12px;\n"
            "  font-weight: 600;\n"
            "  line-height: 1.2;\n"
            "  border: 1px solid transparent;\n"
            "}\n"
            ".default {\n"
            "  background: var(--lumen-muted);\n"
            "  color: var(--lumen-fg);\n"
            "  border-color: var(--lumen-border);\n"
            "}\n"
            ".primary {\n"
            "  background: var(--lumen-primary);\n"
            "  color: var(--lumen-primary-fg);\n"
            "  border-color: var(--lumen-primary);\n"
            "}\n"
            ".success {\n"
            "  background: var(--lumen-success-bg);\n"
            "  color: var(--lumen-success);\n"
            "  border-color: transparent;\n"
            "}\n"
            ".danger {\n"
            "  background: var(--lumen-danger-bg);\n"
            "  color: var(--lumen-danger);\n"
            "  border-color: transparent;\n"
            "}\n"
            "</style>\n"
            "\n"
            "<script>\n"
            "fn state() {\n"
            "  return { variant: \"default\" }\n"
            "}\n"
            "</script>\n";
    } else if (strcasecmp(comp, "accordion") == 0) {
        filename = "pages/components/Accordion.lumen";
        content = 
            "<template>\n"
            "  <div class=\"accordion\">\n"
            "    <button class=\"trigger\" @click=\"toggle\">\n"
            "      <span class=\"title\">{{ title }}</span>\n"
            "      <span class=\"icon {{#if open}}active{{/if}}\">\n"
            "        <svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1em\" height=\"1em\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"m9 18 6-6-6-6\"/></svg>\n"
            "      </span>\n"
            "    </button>\n"
            "    <div class=\"content {{#if open}}expanded{{/if}}\">\n"
            "      <div class=\"inner\">\n"
            "        {{! children }}\n"
            "      </div>\n"
            "    </div>\n"
            "  </div>\n"
            "</template>\n"
            "\n"
            "<style>\n"
            ".accordion {\n"
            "  border-bottom: 1px solid var(--lumen-border);\n"
            "  width: 100%;\n"
            "}\n"
            ".trigger {\n"
            "  width: 100%;\n"
            "  display: flex;\n"
            "  justify-content: space-between;\n"
            "  align-items: center;\n"
            "  padding: 16px 0;\n"
            "  background: transparent;\n"
            "  border: none;\n"
            "  color: var(--lumen-fg);\n"
            "  font-size: 15px;\n"
            "  font-weight: 600;\n"
            "  cursor: pointer;\n"
            "  text-align: left;\n"
            "  outline: none;\n"
            "}\n"
            ".trigger:hover {\n"
            "  color: var(--lumen-primary);\n"
            "}\n"
            ".title {\n"
            "  flex: 1;\n"
            "}\n"
            ".icon {\n"
            "  font-size: 12px;\n"
            "  color: var(--lumen-muted-fg);\n"
            "  transition: transform 0.2s ease;\n"
            "}\n"
            ".icon.active {\n"
            "  transform: rotate(90deg);\n"
            "}\n"
            ".content {\n"
            "  max-height: 0;\n"
            "  overflow: hidden;\n"
            "  transition: max-height 0.2s ease-out;\n"
            "}\n"
            ".content.expanded {\n"
            "  max-height: 500px;\n"
            "}\n"
            ".inner {\n"
            "  padding: 0 0 16px 0;\n"
            "  color: var(--lumen-muted-fg);\n"
            "  font-size: 14px;\n"
            "  line-height: 1.5;\n"
            "}\n"
            "</style>\n"
            "\n"
            "<script>\n"
            "fn state() {\n"
            "  return { open: false, title: \"\" }\n"
            "}\n"
            "fn toggle(s, v) {\n"
            "  return s.set(\"open\", !s.get(\"open\"))\n"
            "}\n"
            "</script>\n";
    } else if (strcasecmp(comp, "alert") == 0) {
        filename = "pages/components/Alert.lumen";
        content = 
            "<template>\n"
            "  <div class=\"alert {{ variant }} {{#if dismissed}}hidden{{/if}}\">\n"
            "    <div class=\"alert-content\">\n"
            "      <h5 class=\"alert-title\">{{ title }}</h5>\n"
            "      <div class=\"alert-description\">\n"
            "        {{! children }}\n"
            "      </div>\n"
            "    </div>\n"
            "    <button class=\"close-btn\" @click=\"dismiss\">\n"
            "      <svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1em\" height=\"1em\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M18 6 6 18\"/><path d=\"m6 6 12 12\"/></svg>\n"
            "    </button>\n"
            "  </div>\n"
            "</template>\n"
            "\n"
            "<style>\n"
            ".alert {\n"
            "  position: relative;\n"
            "  width: 100%;\n"
            "  border-radius: 8px;\n"
            "  border: 1px solid;\n"
            "  padding: 16px;\n"
            "  display: flex;\n"
            "  gap: 12px;\n"
            "  box-sizing: border-box;\n"
            "}\n"
            ".hidden {\n"
            "  display: none;\n"
            "}\n"
            ".default {\n"
            "  background: var(--lumen-card);\n"
            "  color: var(--lumen-fg);\n"
            "  border-color: var(--lumen-border);\n"
            "}\n"
            ".info {\n"
            "  background: var(--lumen-info-bg);\n"
            "  color: var(--lumen-info);\n"
            "  border-color: transparent;\n"
            "}\n"
            ".warning {\n"
            "  background: rgba(245, 184, 41, 0.1);\n"
            "  color: var(--lumen-primary);\n"
            "  border-color: transparent;\n"
            "}\n"
            ".danger {\n"
            "  background: var(--lumen-danger-bg);\n"
            "  color: var(--lumen-danger);\n"
            "  border-color: transparent;\n"
            "}\n"
            ".alert-content {\n"
            "  flex: 1;\n"
            "}\n"
            ".alert-title {\n"
            "  margin: 0 0 4px 0;\n"
            "  font-size: 14px;\n"
            "  font-weight: 600;\n"
            "  line-height: 1;\n"
            "  letter-spacing: -0.01em;\n"
            "}\n"
            ".alert-description {\n"
            "  margin: 0;\n"
            "  font-size: 13px;\n"
            "  opacity: 0.9;\n"
            "  line-height: 1.4;\n"
            "}\n"
            ".close-btn {\n"
            "  background: transparent;\n"
            "  border: none;\n"
            "  color: inherit;\n"
            "  opacity: 0.6;\n"
            "  cursor: pointer;\n"
            "  padding: 0;\n"
            "  font-size: 14px;\n"
            "  align-self: flex-start;\n"
            "}\n"
            ".close-btn:hover {\n"
            "  opacity: 1;\n"
            "}\n"
            "</style>\n"
            "\n"
            "<script>\n"
            "fn state() {\n"
            "  return { variant: \"default\", title: \"Alert\", dismissed: false }\n"
            "}\n"
            "fn dismiss(s, v) {\n"
            "  return s.set(\"dismissed\", true)\n"
            "}\n"
            "</script>\n";
    } else if (strcasecmp(comp, "progress") == 0) {
        filename = "pages/components/Progress.lumen";
        content = 
            "<template>\n"
            "  <div class=\"progress-bar\">\n"
            "    <div class=\"progress-indicator\" style=\"width: {{ value }}%\"></div>\n"
            "  </div>\n"
            "</template>\n"
            "\n"
            "<style>\n"
            ".progress-bar {\n"
            "  position: relative;\n"
            "  width: 100%;\n"
            "  height: 8px;\n"
            "  background: var(--lumen-muted);\n"
            "  border-radius: 9999px;\n"
            "  overflow: hidden;\n"
            "}\n"
            ".progress-indicator {\n"
            "  height: 100%;\n"
            "  background: var(--lumen-primary);\n"
            "  border-radius: 9999px;\n"
            "  transition: width 0.3s ease;\n"
            "}\n"
            "</style>\n"
            "\n"
            "<script>\n"
            "fn state() {\n"
            "  return { value: 0 }\n"
            "}\n"
            "</script>\n";
    } else if (strcasecmp(comp, "select") == 0) {
        filename = "pages/components/Select.lumen";
        content = 
            "<template>\n"
            "  <div class=\"select-container\">\n"
            "    <button class=\"select-trigger\" @click=\"toggle\">\n"
            "      <span>{{#if selected}}{{ selected }}{{else}}{{ placeholder }}{{/if}}</span>\n"
            "      <span class=\"chevron\">\n"
            "        <svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1em\" height=\"1em\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"m6 9 6 6 6-6\"/></svg>\n"
            "      </span>\n"
            "    </button>\n"
            "    <div class=\"select-dropdown {{#if open}}active{{/if}}\">\n"
            "      {{! children }}\n"
            "    </div>\n"
            "  </div>\n"
            "</template>\n"
            "\n"
            "<style>\n"
            ".select-container {\n"
            "  position: relative;\n"
            "  width: 100%;\n"
            "}\n"
            ".select-trigger {\n"
            "  width: 100%;\n"
            "  background: var(--lumen-input);\n"
            "  border: 1px solid var(--lumen-border);\n"
            "  color: var(--lumen-fg);\n"
            "  padding: 10px 14px;\n"
            "  border-radius: 8px;\n"
            "  font-size: 14px;\n"
            "  font-family: inherit;\n"
            "  display: flex;\n"
            "  justify-content: space-between;\n"
            "  align-items: center;\n"
            "  cursor: pointer;\n"
            "  outline: none;\n"
            "  transition: border-color 0.2s ease;\n"
            "}\n"
            ".select-trigger:focus {\n"
            "  border-color: var(--lumen-primary);\n"
            "}\n"
            ".chevron {\n"
            "  font-size: 10px;\n"
            "  color: var(--lumen-muted-fg);\n"
            "}\n"
            ".select-dropdown {\n"
            "  position: absolute;\n"
            "  top: 100%;\n"
            "  left: 0;\n"
            "  width: 100%;\n"
            "  margin-top: 4px;\n"
            "  background: var(--lumen-card);\n"
            "  border: 1px solid var(--lumen-border);\n"
            "  border-radius: 8px;\n"
            "  box-shadow: 0 10px 25px rgba(0,0,0,0.3);\n"
            "  z-index: 100;\n"
            "  opacity: 0;\n"
            "  pointer-events: none;\n"
            "  transform: translateY(-4px);\n"
            "  transition: opacity 0.15s ease, transform 0.15s ease;\n"
            "  max-height: 200px;\n"
            "  overflow-y: auto;\n"
            "}\n"
            ".select-dropdown.active {\n"
            "  opacity: 1;\n"
            "  pointer-events: auto;\n"
            "  transform: translateY(0);\n"
            "}\n"
            "</style>\n"
            "\n"
            "<script>\n"
            "fn state() {\n"
            "  return { open: false, placeholder: \"Select an option\", selected: \"\" }\n"
            "}\n"
            "fn toggle(s, v) {\n"
            "  return s.set(\"open\", !s.get(\"open\"))\n"
            "}\n"
            "</script>\n";
    } else if (strcasecmp(comp, "checkbox") == 0) {
        filename = "pages/components/Checkbox.lumen";
        content = 
            "<template>\n"
            "  <label class=\"checkbox-label\" @click=\"toggle\">\n"
            "    <div class=\"checkbox-box {{#if checked}}checked{{/if}}\">\n"
            "      {{#if checked}}<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1em\" height=\"1em\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M20 6 9 17l-5-5\"/></svg>{{/if}}\n"
            "    </div>\n"
            "    <span class=\"label-text\">{{ label }}</span>\n"
            "  </label>\n"
            "</template>\n"
            "\n"
            "<style>\n"
            ".checkbox-label {\n"
            "  display: inline-flex;\n"
            "  align-items: center;\n"
            "  gap: 8px;\n"
            "  cursor: pointer;\n"
            "  user-select: none;\n"
            "  color: var(--lumen-fg);\n"
            "  font-size: 14px;\n"
            "}\n"
            ".checkbox-box {\n"
            "  width: 18px;\n"
            "  height: 18px;\n"
            "  border-radius: 4px;\n"
            "  border: 1px solid var(--lumen-border);\n"
            "  background: var(--lumen-input);\n"
            "  display: flex;\n"
            "  align-items: center;\n"
            "  justify-content: center;\n"
            "  font-size: 12px;\n"
            "  font-weight: bold;\n"
            "  color: var(--lumen-primary-fg);\n"
            "  transition: background 0.2s ease, border-color 0.2s ease;\n"
            "}\n"
            ".checkbox-box.checked {\n"
            "  background: var(--lumen-primary);\n"
            "  border-color: var(--lumen-primary);\n"
            "}\n"
            "</style>\n"
            "\n"
            "<script>\n"
            "fn state() {\n"
            "  return { checked: false, label: \"\" }\n"
            "}\n"
            "fn toggle(s, v) {\n"
            "  return s.set(\"checked\", !s.get(\"checked\"))\n"
            "}\n"
            "</script>\n";
    } else if (strcasecmp(comp, "switch") == 0) {
        filename = "pages/components/Switch.lumen";
        content = 
            "<template>\n"
            "  <button class=\"switch-track {{#if checked}}active{{/if}}\" @click=\"toggle\">\n"
            "    <div class=\"switch-thumb {{#if checked}}active{{/if}}\"></div>\n"
            "  </button>\n"
            "</template>\n"
            "\n"
            "<style>\n"
            ".switch-track {\n"
            "  width: 44px;\n"
            "  height: 24px;\n"
            "  border-radius: 9999px;\n"
            "  background: var(--lumen-muted);\n"
            "  border: 1px solid var(--lumen-border);\n"
            "  position: relative;\n"
            "  cursor: pointer;\n"
            "  outline: none;\n"
            "  padding: 0;\n"
            "  transition: background 0.2s ease;\n"
            "}\n"
            ".switch-track.active {\n"
            "  background: var(--lumen-primary);\n"
            "  border-color: var(--lumen-primary);\n"
            "}\n"
            ".switch-thumb {\n"
            "  width: 18px;\n"
            "  height: 18px;\n"
            "  border-radius: 50%;\n"
            "  background: var(--lumen-primary-fg);\n"
            "  position: absolute;\n"
            "  top: 2px;\n"
            "  left: 2px;\n"
            "  transition: transform 0.2s ease, background 0.2s ease;\n"
            "}\n"
            ".switch-thumb.active {\n"
            "  transform: translateX(20px);\n"
            "  background: var(--lumen-primary-fg);\n"
            "}\n"
            "</style>\n"
            "\n"
            "<script>\n"
            "fn state() {\n"
            "  return { checked: false }\n"
            "}\n"
            "fn toggle(s, v) {\n"
            "  return s.set(\"checked\", !s.get(\"checked\"))\n"
            "}\n"
            "</script>\n";
    } else if (strcasecmp(comp, "separator") == 0) {
        filename = "pages/components/Separator.lumen";
        content = 
            "<template>\n"
            "  <div class=\"separator {{ orientation }}\"></div>\n"
            "</template>\n"
            "\n"
            "<style>\n"
            ".separator {\n"
            "  background: var(--lumen-border);\n"
            "}\n"
            ".horizontal {\n"
            "  width: 100%;\n"
            "  height: 1px;\n"
            "}\n"
            ".vertical {\n"
            "  width: 1px;\n"
            "  height: 100%;\n"
            "}\n"
            "</style>\n"
            "\n"
            "<script>\n"
            "fn state() {\n"
            "  return { orientation: \"horizontal\" }\n"
            "}\n"
            "</script>\n";
    } else if (strcasecmp(comp, "theme-toggle") == 0) {
        filename = "pages/components/ThemeToggle.lumen";
        content = 
            "<template>\n"
            "  <button class=\"theme-toggle\" onclick=\"document.documentElement.classList.toggle('dark'); localStorage.setItem('lumen-theme', document.documentElement.classList.contains('dark') ? 'dark' : 'light')\">\n"
            "    <svg class=\"light-icon\" xmlns=\"http://www.w3.org/2000/svg\" width=\"20\" height=\"20\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><circle cx=\"12\" cy=\"12\" r=\"4\"/><path d=\"M12 2v2\"/><path d=\"M12 20v2\"/><path d=\"m4.93 4.93 1.41 1.41\"/><path d=\"m17.66 17.66 1.41 1.41\"/><path d=\"M2 12h2\"/><path d=\"M20 12h2\"/><path d=\"m6.34 17.66-1.41 1.41\"/><path d=\"m19.07 4.93-1.41 1.41\"/></svg>\n"
            "    <svg class=\"dark-icon\" xmlns=\"http://www.w3.org/2000/svg\" width=\"20\" height=\"20\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M12 3a6 6 0 0 0 9 9 9 9 0 1 1-9-9Z\"/></svg>\n"
            "  </button>\n"
            "</template>\n"
            "\n"
            "<style>\n"
            ".theme-toggle {\n"
            "  background: transparent;\n"
            "  border: 1px solid var(--lumen-border);\n"
            "  border-radius: 8px;\n"
            "  cursor: pointer;\n"
            "  color: var(--lumen-fg);\n"
            "  padding: 8px;\n"
            "  display: inline-flex;\n"
            "  align-items: center;\n"
            "  justify-content: center;\n"
            "  transition: background 0.2s ease;\n"
            "}\n"
            ".theme-toggle:hover {\n"
            "  background: var(--lumen-muted);\n"
            "}\n"
            "html.dark .light-icon { display: none; }\n"
            "html:not(.dark) .dark-icon { display: none; }\n"
            "</style>\n"
            "\n"
            "<script>\n"
            "fn state() {\n"
            "  return {}\n"
            "}\n"
            "</script>\n";
    } else {
        fprintf(stderr, "Lumen UI: unknown component '%s'. Available: button, card, input, dialog, badge, accordion, alert, progress, select, checkbox, switch, separator, theme-toggle\n", comp);
        return 1;
    }
    
    FILE *f = fopen(filename, "wb");
    if (!f) {
        fprintf(stderr, "Lumen UI: cannot write to %s\n", filename);
        return 1;
    }
    fputs(content, f);
    fclose(f);
    
    int color = lumen_color();
    const char *GR   = color ? LUM_GREEN   : "";
    const char *R    = color ? LUM_RESET   : "";
    const char *D    = color ? LUM_DIM     : "";
    const char *W    = color ? LUM_WHITEB  : "";
    const char *A    = color ? LUM_AMBER   : "";
    const char *CHIP = color ? "\033[48;5;236m\033[1;38;5;214m LUMEN UI \033[0m" : "LUMEN UI";
    
    printf("\n  %s  %sv0.1.0 · official component registry%s\n\n", CHIP, D, R);
    printf("  %s✔%s  Added %s%s%s to %s%s%s\n\n", GR, R, W, comp, R, A, filename, R);
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
    printf("  %s remove <pkg>  Remove a package\n", prog);
    printf("  %s install [--frozen]\n", prog);
    printf("  %s update      Update dependencies\n", prog);
    printf("  %s search <q>  Search the registry\n", prog);
    printf("  %s publish     Publish package to the registry\n", prog);
    printf("\nLanguage Tooling:\n");
    printf("  %s wrap <target> Generate wrapper for a foreign library\n", prog);
    printf("  %s build <file>  Build app.vnb (or --release for native binary)\n", prog);
    printf("  %s lsp           Start LSP server\n", prog);
    printf("\n");
    printf("Aurora (fullstack):\n");
    printf("  %s new <name>                Scaffold a new Aurora project\n", prog);
    printf("  %s dev [dir] [port]          Serve a pages/ dir with live reload (default ./pages :8090)\n", prog);
    printf("  %s build <file>              Build for production\n", prog);
    printf("  %s lumen new <name>          Scaffold a Lumen-only frontend (alias)\n", prog);
    printf("  %s lumen add <comp>          Copy a Lumen UI component to pages/components/\n", prog);
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
            } else if (nlen > 7 && strcmp(entry->d_name + nlen - 7, ".lumen") == 0) {
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

    size_t plen = strlen(path);
    bool is_lumen = (plen > 6 && strcmp(path + plen - 6, ".lumen") == 0);

    int out_pos = 0;
    char *out;
    if (is_lumen) {
        out = fmt_format_lumen_source(source, size, &out_pos);
    } else {
        out = fmt_format_source(source, size, &out_pos);
    }
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
#ifdef _WIN32
            char win_temp[MAX_PATH];
            GetTempFileNameA(".", "vnf", 0, win_temp);
            FILE *tf = fopen(win_temp, "wb");
            if (tf) {
                fwrite(out, 1, (size_t)out_pos, tf);
                fclose(tf);
                char cmd[2048];
                snprintf(cmd, sizeof(cmd), "diff -u \"%s\" \"%s\"", path, win_temp);
                int rc = system(cmd);
                (void)rc;
                _unlink(win_temp);
            }
#else
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
#endif
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
#ifdef _WIN32
    { WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa); }
#endif
#ifndef VN_NO_HTTP
    curl_global_init(CURL_GLOBAL_ALL);
#endif
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
    } else if (strcmp(argv[1], "remove") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s remove <pkg_name>\n", argv[0]);
            return 1;
        }
        return pkg_remove(argv[2]);
    } else if (strcmp(argv[1], "publish") == 0) {
        return pkg_publish();
    } else if (strcmp(argv[1], "install") == 0) {
        bool frozen = false;
        if (argc == 3 && strcmp(argv[2], "--frozen") == 0) {
            frozen = true;
        } else if (argc > 2) {
            fprintf(stderr, "Usage: %s install [--frozen]\n", argv[0]);
            return 1;
        }
        return pkg_install(frozen);
    }

    if (strcmp(argv[1], "update") == 0) {
        if (argc > 2) {
            fprintf(stderr, "Usage: %s update\n", argv[0]);
            return 1;
        }
        return pkg_update();
    }

    if (strcmp(argv[1], "search") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Usage: %s search <query>\n", argv[0]);
            return 1;
        }
        return pkg_search(argv[2]);
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
            fprintf(stderr, "Usage: %s run <file.vn | file.vnb>\n", argv[0]);
            return 1;
        }
        const char *target = argv[2];
        size_t len = strlen(target);
        if (len > 4 && strcmp(target + len - 4, ".vnb") == 0) {
            VM vm;
            vm_init(&vm, NULL);
            ObjFunction *main_fn = vnb_load(&vm, target);
            if (!main_fn) {
                fprintf(stderr, "Failed to load %s\n", target);
                return 1;
            }
            vm.source_name = target;
            vm.main_fn = main_fn;
            
            int res = vm_run(&vm, false) ? 0 : 1;
            vm_free(&vm);
            return res;
        }
        
        char *source = read_file_with_modules(target);
        if (!source) return 1;
        g_varian_script_path = target;
        int result = run_source(source, target);
        free(source);
        return result;
    }
    
    if (strcmp(argv[1], "build") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s build <file.vn> [--release] [out_basename]\n", argv[0]);
            return 1;
        }
        const char *entry = argv[2];
        const char *out_basename = "app";
        bool release = false;
        bool static_link = false;
        const char *cc_compiler = getenv("CC");
        if (!cc_compiler) cc_compiler = "cc";
        const char *target_triple = NULL;
        
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--release") == 0) {
                release = true;
            } else if (strcmp(argv[i], "--static") == 0) {
                static_link = true;
            } else if (strncmp(argv[i], "--cc=", 5) == 0) {
                cc_compiler = argv[i] + 5;
            } else if (strncmp(argv[i], "--target=", 9) == 0) {
                target_triple = argv[i] + 9;
            } else {
                out_basename = argv[i];
            }
        }
        
        /* Auto-install any declared dependency that isn't vendored yet, so
         * `use "<pkg>"` resolves during the build instead of erroring. */
        {
            struct stat st_toml;
            if (stat("constellation.toml", &st_toml) == 0) {
                ConstellationManifest m;
                if (pkg_manifest_load(&m, "constellation.toml")) {
                    bool any_missing = false;
                    for (int i = 0; i < m.dep_count; i++) {
                        char dp[600];
                        snprintf(dp, sizeof(dp), "vn_modules/%s", m.deps[i].name);
                        struct stat ds;
                        if (stat(dp, &ds) != 0 || !S_ISDIR(ds.st_mode)) { any_missing = true; break; }
                    }
                    if (any_missing) {
                        printf("[Kiln] Installing missing dependencies...\n");
                        if (pkg_install(false) != 0) {
                            fprintf(stderr, "[Kiln] dependency install failed.\n");
                            return 1;
                        }
                    }
                }
            }
        }

        const char *build_entry = entry;
        bool temp_lumen_entry = false;
        struct stat st_pages;
        if (stat("pages", &st_pages) == 0 && S_ISDIR(st_pages.st_mode)) {
            bool is_aurora = false;
            struct stat st_toml2;
            if (stat("constellation.toml", &st_toml2) == 0) {
                ConstellationManifest m2;
                if (pkg_manifest_load(&m2, "constellation.toml")) {
                    if (m2.kind == MANIFEST_KIND_AURORA) is_aurora = true;
                }
            }
            if (is_aurora) {
                printf("[Kiln] Detected Aurora project. Pre-compiling routes...\n");
            } else {
                printf("[Kiln] Detected Lumen project (pages/ directory present). Pre-compiling routes...\n");
            }
            if (lumen_build("pages", ".lumen-build.vn", "8090") != 0) {
                fprintf(stderr, "[Kiln] Pre-compilation of Lumen pages failed.\n");
                return 1;
            }
            build_entry = ".lumen-build.vn";
            temp_lumen_entry = true;
        }
        
        char *source = read_file_with_modules(build_entry);
        if (!source) {
            if (temp_lumen_entry) remove(build_entry);
            return 1;
        }
        
        // Scan public/ directory and collect assets
        VMAsset *assets = NULL;
        int asset_count = 0;
        int asset_capacity = 0;
        struct stat st_public;
        if (stat("public", &st_public) == 0 && S_ISDIR(st_public.st_mode)) {
            printf("[Kiln] Embedding public/ assets...\n");
            collect_assets_recursive("public", &assets, &asset_count, &asset_capacity);
        }
        
        // Cache lookup (incorporates source + release flag + asset contents)
        uint64_t hash_val = fnv1a_hash(source, strlen(source));
        hash_val = fnv1a_hash((const char *)&release, sizeof(bool)) ^ hash_val;
        hash_val = fnv1a_hash((const char *)&static_link, sizeof(bool)) ^ hash_val;
        if (cc_compiler) hash_val ^= fnv1a_hash(cc_compiler, strlen(cc_compiler));
        if (target_triple) hash_val ^= fnv1a_hash(target_triple, strlen(target_triple));
        for (int i = 0; i < asset_count; i++) {
            hash_val ^= fnv1a_hash(assets[i].path, strlen(assets[i].path));
            hash_val ^= fnv1a_hash((const char *)assets[i].data, (size_t)assets[i].size);
        }
        
        char hash_str[32];
        snprintf(hash_str, sizeof(hash_str), "%016lx", hash_val);
        char cache_path[512];
        snprintf(cache_path, sizeof(cache_path), ".kiln/cache/%s", hash_str);
        
        struct stat st;
        if (stat(cache_path, &st) == 0) {
            char target_path[512];
            if (release) {
                snprintf(target_path, sizeof(target_path), "%s", out_basename);
            } else {
                snprintf(target_path, sizeof(target_path), "%s.vnb", out_basename);
            }
            if (copy_file(cache_path, target_path)) {
#ifndef _WIN32
                if (release) {
                    chmod(target_path, 0755);
                }
#endif
                printf("[Kiln] Cache hit! Reused cached artifact: %s\n", target_path);
                free(source);
                if (temp_lumen_entry) remove(build_entry);
                if (assets) {
                    for (int i = 0; i < asset_count; i++) {
                        free(assets[i].path);
                        free(assets[i].data);
                    }
                    free(assets);
                }
                return 0;
            }
        }
        
        if (release) {
            char out_c[256];
            snprintf(out_c, sizeof(out_c), "%s.c", out_basename);
            int res = aot_compile(source, build_entry, out_c);
            free(source);
            if (temp_lumen_entry) remove(build_entry);
            if (res != 0) {
                if (assets) {
                    for (int i = 0; i < asset_count; i++) { free(assets[i].path); free(assets[i].data); }
                    free(assets);
                }
                return res;
            }
            
            FILE *f = fopen(out_c, "a");
            if (f) {
                // Write asset arrays in generated C file
                for (int i = 0; i < asset_count; i++) {
                    fprintf(f, "static const unsigned char asset_data_%d[] = {\n  ", i);
                    for (int j = 0; j < assets[i].size; j++) {
                        fprintf(f, "0x%02x, ", assets[i].data[j]);
                        if (j % 16 == 15) fprintf(f, "\n  ");
                    }
                    fprintf(f, "\n};\n\n");
                }
                
                fprintf(f, "\nconst char *g_varian_script_path = NULL;\n"
                           "char *read_file_with_modules(const char *path) { (void)path; return NULL; }\n\n"
                           "int main() {\n"
                           "  VM vm;\n"
                           "  vm_init(&vm, NULL);\n");
                
                // Populate assets array in VM inside generated main
                if (asset_count > 0) {
                    fprintf(f, "  vm.assets = (VMAsset *)calloc(%d, sizeof(VMAsset));\n", asset_count);
                    fprintf(f, "  vm.asset_count = %d;\n", asset_count);
                    for (int i = 0; i < asset_count; i++) {
                        fprintf(f, "  vm.assets[%d].path = strdup(\"%s\");\n", i, assets[i].path);
                        fprintf(f, "  vm.assets[%d].data = (unsigned char *)asset_data_%d;\n", i, i);
                        fprintf(f, "  vm.assets[%d].size = %d;\n", i, assets[i].size);
                    }
                }
                
                fprintf(f, "  ObjFunction *main_fn = varian_aot_load(&vm);\n"
                           "  vm.main_fn = main_fn;\n"
                           "  int res = vm_run(&vm, false) ? 0 : 1;\n"
                           "  vm_free(&vm);\n"
                           "  return res;\n"
                           "}\n");
                fclose(f);
            }
            
            char exe_dir[2048] = ".";
            char exe[2048];
#ifdef _WIN32
            DWORD exe_n = GetModuleFileNameA(NULL, exe, sizeof(exe));
            if (exe_n > 0 && exe_n < sizeof(exe)) { exe[exe_n] = '\0'; }
#else
            ssize_t exe_n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
            if (exe_n > 0) { exe[exe_n] = '\0'; }
#endif
            if (exe_n > 0) {
                char *slash = strrchr(exe, '/');
#ifdef _WIN32
                if (!slash) slash = strrchr(exe, '\\');
#endif
                if (slash) {
                    *slash = '\0';
                    strncpy(exe_dir, exe, sizeof(exe_dir) - 1);
                    exe_dir[sizeof(exe_dir) - 1] = '\0';
                }
            }
            
            char cmd[4096];
            char target_flag[256] = "";
            if (target_triple) {
                snprintf(target_flag, sizeof(target_flag), "-target %s", target_triple);
            }
            const char *static_flag = static_link ? "-static" : "";
            snprintf(cmd, sizeof(cmd), "%s %s %s -O2 -I%s/include %s -o %s %s/libvarian.a -lm -lffi -ldl -lcurl -lpq -lcrypto -lssl -lsqlite3 -lhiredis -lpthread -luring", cc_compiler, static_flag, target_flag, exe_dir, out_c, out_basename, exe_dir);
            printf("Compiling native binary: %s\n", cmd);
            res = system(cmd);
            if (res == 0) {
                ensure_dir(".kiln");
                ensure_dir(".kiln/cache");
                copy_file(out_basename, cache_path);
                printf("[Kiln] Cached new build: %s\n", cache_path);
                if (assets) {
                    for (int i = 0; i < asset_count; i++) { free(assets[i].path); free(assets[i].data); }
                    free(assets);
                }
                return 0;
            }
            if (assets) {
                for (int i = 0; i < asset_count; i++) { free(assets[i].path); free(assets[i].data); }
                free(assets);
            }
            return 1;
        } else {
            Lexer lexer;
            lexer_init(&lexer, source, build_entry);
            Arena *arena = arena_create(0);
            Parser parser;
            parser_init(&parser, &lexer, arena);
            AstNode *program = parser_parse(&parser);
            if (parser.had_error) {
                fprintf(stderr, "Parse error\n");
                arena_destroy(arena);
                free(source);
                if (temp_lumen_entry) remove(build_entry);
                if (assets) {
                    for (int i = 0; i < asset_count; i++) { free(assets[i].path); free(assets[i].data); }
                    free(assets);
                }
                return 1;
            }
            
            Chunk chunk;
            chunk_init(&chunk);
            Compiler compiler;
            compiler_init(&compiler, arena, &chunk, program);
            if (!compiler_compile(&compiler)) {
                fprintf(stderr, "Compile error\n");
                arena_destroy(arena);
                free(source);
                if (temp_lumen_entry) remove(build_entry);
                if (assets) {
                    for (int i = 0; i < asset_count; i++) { free(assets[i].path); free(assets[i].data); }
                    free(assets);
                }
                return 1;
            }
            
            ObjFunction *main_fn = (ObjFunction *)calloc(1, sizeof(ObjFunction));
            main_fn->obj.type = VAL_FUNCTION;
            main_fn->code = chunk.code;
            main_fn->code_count = chunk.count;
            main_fn->code_capacity = chunk.capacity;
            main_fn->constants = chunk.constants;
            main_fn->constant_count = chunk.constant_count;
            main_fn->constant_capacity = chunk.constant_capacity;
            main_fn->rle_lines = chunk.rle_lines;
            main_fn->rle_counts = chunk.rle_counts;
            main_fn->rle_count = chunk.rle_count;
            
            char out_vnb[256];
            snprintf(out_vnb, sizeof(out_vnb), "%s.vnb", out_basename);
            vnb_save(main_fn, &compiler, assets, asset_count, out_vnb);
            
            chunk_free(&chunk);
            arena_destroy(arena);
            free(source);
            if (temp_lumen_entry) remove(build_entry);
            printf("Built portable bundle: %s\n", out_vnb);
            
            ensure_dir(".kiln");
            ensure_dir(".kiln/cache");
            copy_file(out_vnb, cache_path);
            printf("[Kiln] Cached new build: %s\n", cache_path);
            if (assets) {
                for (int i = 0; i < asset_count; i++) { free(assets[i].path); free(assets[i].data); }
                free(assets);
            }
            return 0;
        }
    }

    /* `vn dev [pagesdir] [port]` — Lumen file-based dev server. */
    if (strcmp(argv[1], "dev") == 0) {
        const char *pages = (argc >= 3) ? argv[2] : "pages";
        const char *port  = (argc >= 4) ? argv[3] : "8090";
        return lumen_dev(pages, port);
    }

    /* `vn new <name>` — scaffold a new Aurora project (alias: `vn lumen new`). */
    if (strcmp(argv[1], "new") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s new <name>\n", argv[0]); return 1; }
        return lumen_new(argv[2]);
    }

    /* `vn lumen <new|add|dev|build> ...` — Lumen project tooling. */
    if (strcmp(argv[1], "lumen") == 0) {
        if (argc >= 3 && strcmp(argv[2], "new") == 0) {
            if (argc < 4) { fprintf(stderr, "Usage: %s lumen new <name>\n", argv[0]); return 1; }
            return lumen_new(argv[3]);
        }
        if (argc >= 3 && strcmp(argv[2], "add") == 0) {
            if (argc < 4) { fprintf(stderr, "Usage: %s lumen add <component>\n", argv[0]); return 1; }
            return lumen_add(argv[3]);
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
        fprintf(stderr, "Usage: %s lumen <new <name> | add <component> | dev [dir] [port] | build <dir> <out.vn> [port]>\n", argv[0]);
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
