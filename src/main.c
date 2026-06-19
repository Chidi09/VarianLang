#include "varian.h"
#include "lexer.h"
#include "parser.h"
#include "vm.h"
#include "fmt.h"
#include "test_runner.h"
#include "pkg_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

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
                printf(" (constant %d)", chunk.code[i + 1]);
            } else if (chunk.code[i] == BC_GET_GLOBAL || chunk.code[i] == BC_SET_GLOBAL || chunk.code[i] == BC_DEFINE_GLOBAL) {
                int idx = chunk.code[i + 1];
                if (idx < chunk.constant_count && chunk.constants[idx].type == VAL_STRING) {
                    printf(" (%s)", chunk.constants[idx].as.string->chars);
                }
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

/* ─── Main ─── */
int main(int argc, char *argv[]) {
    if (argc == 1) {
        repl();
        return 0;
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_help(argv[0]);
        return 0;
    }

    if (strcmp(argv[1], "fmt") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Usage: %s fmt <file.vn>\n", argv[0]);
            return 1;
        }
        int result = fmt_format_file(argv[2]);
        return result;
    }

    if (strcmp(argv[1], "test") == 0) {
        const char *dir = (argc >= 3) ? argv[2] : ".";
        return test_run_dir(dir);
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
}
