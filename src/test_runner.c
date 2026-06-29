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
#include <sys/time.h>

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

/* Aggregate counts across files, for the final summary line. */
typedef struct {
    int passed;
    int failed;
    int skipped;
    int timed_out;
} TestTotals;

/* ─── Run a single test file inside a persistent warm VM ─── */
static int run_test_file_warm(VM *warm_vm, const char *path,
                               const char *filter, int timeout_ms,
                               TestTotals *totals) {
    /* Free ALL tasks from the previous file's run so vm_run creates a
     * fresh init task for this file's top-level code.  Dead test tasks
     * from the mini-scheduler are never reaped by vm_run's round-robin
     * loop, so they accumulate in warm_vm->tasks[] and cause
     * vm_run (warm_vm, true) to skip init-task creation because
     * task_count > 0 even though every entry is dead.
     * Globals are name-addressed at runtime (define_global / get_global
     * do a linear scan of global_names[]), so no compiler seeding is
     * needed — we just need the top-level code to run and register
     * the per-file globals and dispatch entries. */
    for (int i = 0; i < warm_vm->task_count; i++) {
        if (warm_vm->tasks[i]) {
            free(warm_vm->tasks[i]->arena_base);
            warm_vm->tasks[i]->arena_base = NULL;
            free(warm_vm->tasks[i]);
            warm_vm->tasks[i] = NULL;
        }
    }
    warm_vm->task_count = 0;
    warm_vm->free_tasks = NULL;  /* freed above — don't let free-list dangle */

    char *source = read_file(path);
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

    /* Free old main_fn wrapper (struct only — chunk owns code/constants) */
    if (warm_vm->main_fn) {
        free(warm_vm->main_fn);
        warm_vm->main_fn = NULL;
    }

    /* Free old FFI entries from a previous vm_run call */
    free(warm_vm->ffi_entries);
    warm_vm->ffi_entries = NULL;
    warm_vm->ffi_entry_count = 0;

    warm_vm->compiler = &compiler;
    warm_vm->source = source;
    warm_vm->source_name = path;
    warm_vm->prelude_line_count = 0;

    /* Copy tests from compiler to VM */
    warm_vm->test_count = 0;
    for (int i = 0; i < compiler.test_count && warm_vm->test_count < MAX_TESTS; i++) {
        warm_vm->tests[warm_vm->test_count].description = strdup(compiler.tests[i].description);
        warm_vm->tests[warm_vm->test_count].func = compiler.tests[i].func;
        warm_vm->test_count++;
    }

    warm_vm->test_filter = filter;
    warm_vm->test_timeout_ms = timeout_ms;
    warm_vm->test_fail_count = 0;
    warm_vm->test_skip_count = 0;
    warm_vm->test_timeout_count = 0;
    warm_vm->deadline_us = 0;
    warm_vm->timed_out = false;
    warm_vm->had_error = false;
    warm_vm->last_error[0] = '\0';

    /* Run top-level + tests */
    vm_run(warm_vm, true);

    int fails = warm_vm->test_fail_count;

    if (totals) {
        int ran = warm_vm->test_count - warm_vm->test_skip_count;
        totals->passed    += ran - warm_vm->test_fail_count;
        totals->failed    += warm_vm->test_fail_count;
        totals->skipped   += warm_vm->test_skip_count;
        totals->timed_out += warm_vm->test_timeout_count;
    }

    /* Kill any remaining tasks (actor loops, etc.) so they don't leak into the next file */
    for (int i = 0; i < warm_vm->task_count; i++) {
        if (warm_vm->tasks[i]) warm_vm->tasks[i]->dead = true;
    }

    /* Free test descriptions we strdup'd */
    for (int i = 0; i < warm_vm->test_count; i++) {
        free(warm_vm->tests[i].description);
        warm_vm->tests[i].description = NULL;
        warm_vm->tests[i].func = NULL;
    }
    warm_vm->test_count = 0;

    /* Free main_fn struct (owned by this per-file calloc) */
    if (warm_vm->main_fn) {
        free(warm_vm->main_fn);
        warm_vm->main_fn = NULL;
    }

    chunk_free(&chunk);
    arena_destroy(arena);
    free(source);

    return fails > 0 ? 1 : 0;
}

/* ─── Compile-and-run the prelude, returning a persistent warm VM ─── */
static int warm_vm_init(VM *warm_vm, char **out_prelude_source,
                         Chunk *out_prelude_chunk, Arena **out_prelude_arena) {
    char *prelude_source = read_directory_sources("vn_modules");
    if (!prelude_source) {
        fprintf(stderr, "test: could not read vn_modules prelude\n");
        return -1;
    }

    Lexer lexer;
    lexer_init(&lexer, prelude_source, "vn_modules");

    Arena *arena = arena_create(0);
    Parser parser;
    parser_init(&parser, &lexer, arena);

    AstNode *program = parser_parse(&parser);
    if (parser.had_error) {
        fprintf(stderr, "Parse error in prelude: %s\n", parser_get_error(&parser));
        arena_destroy(arena);
        free(prelude_source);
        return -1;
    }

    chunk_init(out_prelude_chunk);

    Compiler compiler;
    compiler_init(&compiler, arena, out_prelude_chunk, program);

    if (!compiler_compile(&compiler)) {
        fprintf(stderr, "Compile error in prelude: %s\n", compiler.error_message);
        chunk_free(out_prelude_chunk);
        arena_destroy(arena);
        free(prelude_source);
        return -1;
    }

    vm_init(warm_vm, &compiler);
    warm_vm->source = prelude_source;
    warm_vm->source_name = "vn_modules";

    /* Run prelude top-level only (no tests) */
    vm_run(warm_vm, false);

    *out_prelude_source = prelude_source;
    *out_prelude_arena = arena;
    return 0;
}

/* ─── Reset warm VM globals/dispatch and RE-RUN the compiled prelude ─── */
static void warm_vm_reset_and_reload_prelude(VM *warm_vm, Chunk *prelude_chunk,
                                              const char *prelude_source,
                                              const char *prelude_name) {
    warm_vm->global_count = 0;
    /* Keep the O(1) globals index in sync with the count reset above, or it
     * retains stale name->slot entries that alias re-defined prelude/test
     * globals onto the wrong slots (manifests as "Undefined variable 'print'"). */
    for (int i = 0; i < GLOBAL_TABLE_SIZE; i++) warm_vm->global_index[i] = -1;
    memset(warm_vm->dispatch_occupied, 0, sizeof(warm_vm->dispatch_occupied));
    memset(warm_vm->dispatch_pic_keys, 0, sizeof(warm_vm->dispatch_pic_keys));
    for (int i = 0; i < VM_DISPATCH_PIC_SIZE; i++)
        warm_vm->dispatch_pic_idxs[i] = -1;
    warm_vm->validation_registry.count = 0;
    warm_vm->shape_registry.count = 0;
    memset(warm_vm->shape_registry.shapes, 0, sizeof(warm_vm->shape_registry.shapes));
    warm_vm->actor_field_count = 0;
    warm_vm->task_count = 0;
    warm_vm->free_tasks = NULL;
    warm_vm->compiler = NULL;
    warm_vm->source = prelude_source;
    warm_vm->source_name = prelude_name;
    warm_vm->prelude_line_count = 0;

    /* Restore main_fn to point at the prelude chunk (run_test_file_warm frees it) */
    if (!warm_vm->main_fn) {
        warm_vm->main_fn = (ObjFunction *)calloc(1, sizeof(ObjFunction));
        warm_vm->main_fn->obj.type = VAL_FUNCTION;
        warm_vm->main_fn->code = prelude_chunk->code;
        warm_vm->main_fn->code_count = prelude_chunk->count;
        warm_vm->main_fn->code_capacity = prelude_chunk->capacity;
        warm_vm->main_fn->constants = prelude_chunk->constants;
        warm_vm->main_fn->constant_count = prelude_chunk->constant_count;
        warm_vm->main_fn->constant_capacity = prelude_chunk->constant_capacity;
    }

    /* Free old FFI entries (vm_run will not re-create them if compiler is NULL) */
    free(warm_vm->ffi_entries);
    warm_vm->ffi_entries = NULL;
    warm_vm->ffi_entry_count = 0;

    vm_run(warm_vm, false);
}

/* ─── Public API ─── */
int test_run_dir(const char *path, const char *filter, int timeout_ms) {
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
    if (filter) printf("Filter: tests matching \"%s\"\n", filter);
    if (timeout_ms > 0) printf("Timeout: %dms per test\n", timeout_ms);
    printf("\n");

    /* ─── Warm-VM setup: compile prelude ONCE ─── */
    VM warm_vm;
    char *prelude_source = NULL;
    Chunk prelude_chunk;
    Arena *prelude_arena = NULL;
    printf("[warm-vm] Loading prelude (vn_modules/*.vn)...\n");
    int vmerr = warm_vm_init(&warm_vm, &prelude_source,
                              &prelude_chunk, &prelude_arena);
    if (vmerr != 0) {
        if (files) {
            for (int i = 0; i < count; i++) free(files[i]);
            free(files);
        }
        return 1;
    }
    printf("[warm-vm] Prelude loaded. %d globals defined.\n\n", warm_vm.global_count);

    int total_failed = 0;
    TestTotals totals = {0, 0, 0, 0};

    for (int i = 0; i < count; i++) {
        printf("File: %s\n", files[i]);

        int ret = run_test_file_warm(&warm_vm, files[i], filter, timeout_ms, &totals);
        total_failed += ret;

        /* Per-file isolation: reset globals and re-run compiled prelude bytecode */
        warm_vm_reset_and_reload_prelude(&warm_vm, &prelude_chunk, prelude_source, "vn_modules");

        printf("\n");
        free(files[i]);
    }

    free(files);

    /* Print the summary BEFORE teardown. A test may leave a lingering native
     * thread alive (a live server / pubsub tick, or the detached pthread in
     * lib_http), which can deadlock vm_free on exit. We already have every
     * result, so report it and terminate immediately, bypassing the hang —
     * the OS reclaims all memory, file handles, and threads. (void-cast the
     * otherwise-unused warm-VM/prelude resources to silence -Wunused.) */
    (void)warm_vm; (void)prelude_chunk; (void)prelude_arena; (void)prelude_source;

    printf("─────────────────────────────────────\n");
    printf("Summary: %d passed, %d failed", totals.passed, totals.failed);
    if (totals.timed_out > 0) printf(" (%d timed out)", totals.timed_out);
    if (totals.skipped > 0)   printf(", %d skipped", totals.skipped);
    printf("\n");
    fflush(stdout);
    fflush(stderr);

    _Exit(total_failed > 0 ? 1 : 0);  /* immediate, skips the hanging teardown */
    return total_failed > 0 ? 1 : 0;  /* not reached */
}
