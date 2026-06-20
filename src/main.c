#include "varian.h"
#include "lexer.h"
#include "parser.h"
#include "vm.h"
#include "fmt.h"
#include "test_runner.h"
#include "pkg_manager.h"
#include "lint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

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

/* ─── Read source with module prelude from vn_modules/ ─── */
static char *read_file_with_modules(const char *path) {
    char *main_source = read_file(path);
    if (!main_source) return NULL;

    char *prelude = read_directory_sources("vn_modules");
    if (!prelude) return main_source;

    size_t prelude_len = strlen(prelude);
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

/* ─── Run source string ─── */
static int run_source(const char *source, const char *filename) {
    Lexer lexer;
    lexer_init(&lexer, source, filename);

    Arena *arena = arena_create(0);
    Parser parser;
    parser_init(&parser, &lexer, arena);

    AstNode *program = parser_parse(&parser);
    if (parser.had_error) {
        fprintf(stderr, "Parse error: %s\n", parser_get_error(&parser));
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
    printf("  %s test [dir]    Run tests in directory (default: .)\n", prog);
    printf("  %s add <pkg>     Add a package dependency\n", prog);
    printf("  %s wrap <target> Generate wrapper for a foreign library\n", prog);
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
        const char *dir = (argc >= 3) ? argv[2] : ".";
        return test_run_dir(dir);
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
        int result = run_source(source, argv[2]);
        free(source);
        return result;
    }

    /* Default: try to run file directly (backward compat) */
    if (argc == 2) {
        char *source = read_file_with_modules(argv[1]);
        if (!source) return 1;
        int result = run_source(source, argv[1]);
        free(source);
        return result;
    }

    fprintf(stderr, "Unknown command '%s'. Use '%s --help' for usage.\n", argv[1], argv[0]);
    return 1;
#endif
}
