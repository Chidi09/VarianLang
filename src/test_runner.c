#include "test_runner.h"
#include "lexer.h"
#include "parser.h"
#include "vm.h"
#include "varian.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

/* ─── File reading ─── */
static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "test: could not open '%s'\n", path);
        return NULL;
    }
    fseek(file, 0L, SEEK_END);
    size_t size = (size_t)ftell(file);
    rewind(file);
    char *buf = (char *)malloc(size + 1);
    if (!buf) { fclose(file); return NULL; }
    size_t nread = fread(buf, 1, size, file);
    buf[nread] = '\0';
    fclose(file);
    return buf;
}

static char *read_directory_sources(const char *dir_path) {
    char *result = NULL;
    size_t result_len = 0;
    DIR *d = opendir(dir_path);
    if (!d) return NULL;

    struct dirent *entry;
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

    for (int i = 0; i < file_count - 1; i++) {
        for (int j = i + 1; j < file_count; j++) {
            if (strcmp(paths[i], paths[j]) > 0) {
                char *tmp = paths[i];
                paths[i] = paths[j];
                paths[j] = tmp;
            }
        }
    }

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

/* ─── Recursive directory scanner ─── */
static void collect_test_files(const char *dir, char ***files, int *count, int *cap) {
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
            collect_test_files(full, files, count, cap);
        } else if (S_ISREG(st.st_mode) && nlen > 8) {
            if (strcmp(entry->d_name + nlen - 8, "_test.vn") == 0) {
                if (*count >= *cap) {
                    *cap = *cap ? *cap * 2 : 16;
                    *files = (char **)realloc(*files, (size_t)(*cap) * sizeof(char *));
                    if (!*files) { free(full); closedir(d); return; }
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

/* ─── Run a single test file ─── */
/* Returns: 0 on success (all tests pass), 1 on failure */
static int run_test_file(const char *path) {
    char *source = read_file_with_modules(path);
    if (!source) return 1;

    Lexer lexer;
    lexer_init(&lexer, source, path);

    Arena *arena = arena_create(0);
    Parser parser;
    parser_init(&parser, &lexer, arena);

    AstNode *program = parser_parse(&parser);
    if (parser.had_error) {
        fprintf(stderr, "Parse error in %s: %s\n", path, parser_get_error(&parser));
        arena_destroy(arena);
        free(source);
        return 1;
    }

    Chunk chunk;
    chunk_init(&chunk);

    Compiler compiler;
    compiler_init(&compiler, arena, &chunk, program);

    if (!compiler_compile(&compiler)) {
        fprintf(stderr, "Compile error in %s: %s\n", path, compiler.error_message);
        chunk_free(&chunk);
        arena_destroy(arena);
        free(source);
        return 1;
    }

    VM vm;
    vm_init(&vm, &compiler);

    /* Copy tests from compiler to VM */
    for (int i = 0; i < compiler.test_count && vm.test_count < MAX_TESTS; i++) {
        vm.tests[vm.test_count].description = strdup(compiler.tests[i].description);
        vm.tests[vm.test_count].func = compiler.tests[i].func;
        vm.test_count++;
    }

    /* Run top-level + tests */
    vm_run(&vm, true);

    int fails = vm.test_fail_count;

    vm_free(&vm);
    chunk_free(&chunk);

    /* Free test descriptions we strdup'd */
    for (int i = 0; i < vm.test_count; i++) {
        if (vm.tests[i].description) {
            /* Don't free compiler.tests[i].description — strdup done above */
            free(vm.tests[i].description);
        }
    }

    arena_destroy(arena);
    free(source);
    return fails > 0 ? 1 : 0;
}

/* ─── Public API ─── */
int test_run_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == -1) {
        fprintf(stderr, "test: path '%s' does not exist\n", path);
        return 1;
    }

    char **files = NULL;
    int count = 0;
    int cap = 0;

    if (S_ISDIR(st.st_mode)) {
        collect_test_files(path, &files, &count, &cap);
    } else {
        files = malloc(sizeof(char *));
        files[0] = strdup(path);
        count = 1;
    }

    if (count == 0) {
        printf("No test files found in '%s'\n", path);
        return 0;
    }

    printf("Found %d test file(s) in '%s'\n", count, path);
    printf("\n");

    int total_failed = 0;

    for (int i = 0; i < count; i++) {
        printf("File: %s\n", files[i]);
        total_failed += run_test_file(files[i]);
        printf("\n");
        free(files[i]);
    }

    free(files);

    return total_failed > 0 ? 1 : 0;
}
