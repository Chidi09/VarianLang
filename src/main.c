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
        char exe[2048];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n <= 0) { _exit(127); }
        exe[n] = '\0';
        execl(exe, exe, "run", app, (char *)NULL);
        _exit(127); /* exec failed */
    }
    return pid;
}

/* `vn dev`: build pages/, serve, and live-reload on file changes. The browser's
 * client runtime already auto-reconnects on socket close and re-renders, so a
 * rebuild+restart cycle is a full hot reload with no extra client code. A
 * rebuild that fails (e.g. a mid-edit syntax error) keeps the last good server
 * running and prints the error, rather than dropping the page. */
static int lumen_dev(const char *pages, const char *port) {
    const char *app = ".lumen-build.vn";
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

    printf("⚡ Lumen: watching %s for changes (Ctrl-C to stop)\n", pages);
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
            printf("⚡ Lumen: change detected — rebuilding…\n");
            if (lumen_build(pages, app, port) == 0) {
                kill(child, SIGTERM);
                waitpid(child, NULL, 0);
                child = lumen_spawn_server(app);
                printf("⚡ Lumen: reloaded.\n");
            } else {
                fprintf(stderr, "lumen: rebuild failed — keeping the running server.\n");
            }
        }
    }
    return 0;
}

/* Scaffold a starter Lumen project: <name>/pages/index.lumen */
static int lumen_new(const char *name) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/pages", name);
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir);
    if (system(cmd) != 0) { fprintf(stderr, "lumen: cannot create %s\n", dir); return 1; }

    char page[1100];
    snprintf(page, sizeof(page), "%s/index.lumen", dir);
    FILE *f = fopen(page, "wb");
    if (!f) { fprintf(stderr, "lumen: cannot write %s\n", page); return 1; }
    fputs(
        "<template>\n"
        "<main style=\"font-family:system-ui;max-width:640px;margin:64px auto;text-align:center\">\n"
        "  <h1>Welcome to {{ name }}</h1>\n"
        "  <p>Count: <b>{{ count }}</b></p>\n"
        "  <button @click=\"inc\">+1</button>\n"
        "  <button @click=\"dec\">-1</button>\n"
        "</main>\n"
        "</template>\n"
        "<script>\n"
        "fn state() {\n"
        "  return { name: \"Lumen\", count: 0 }\n"
        "}\n"
        "fn inc(s, v) { return s.set(\"count\", s.get(\"count\") + 1) }\n"
        "fn dec(s, v) { return s.set(\"count\", s.get(\"count\") - 1) }\n"
        "</script>\n", f);
    fclose(f);

    printf("⚡ Created Lumen app '%s'\n\n  cd %s\n  vn dev pages\n\nThen open http://localhost:8090\n", name, name);
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
