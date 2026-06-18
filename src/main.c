#include "varian.h"
#include "lexer.h"
#include "parser.h"
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ─── Run source string ─── */
static int run_source(const char *source, const char *filename) {
    /* Phase 1 pipeline: Lexer → Parser → Compiler (bytecode) → VM */

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

    /* Print AST in debug mode */
    if (getenv("VN_DEBUG_AST")) {
        printf("=== AST ===\n");
        ast_print(program, 0);
        printf("===========\n");
    }

    /* Compile to bytecode */
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

    /* Run in VM */
    VM vm;
    vm_init(&vm, &compiler);

    if (!vm_run(&vm)) {
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

        /* Trim newline */
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

/* ─── Main ─── */
int main(int argc, char *argv[]) {
    if (argc == 1) {
        repl();
        return 0;
    }

    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        printf("Varian v" VARIAN_VERSION "\n");
        printf("Usage: %s [file.vn]\n", argv[0]);
        printf("  With no arguments, starts an interactive REPL.\n");
        printf("\n");
        printf("Environment variables:\n");
        printf("  VN_DEBUG_AST       Print the AST before compilation\n");
        printf("  VN_DEBUG_BYTECODE  Print bytecode disassembly\n");
        return 0;
    }

    if (argc != 2) {
        fprintf(stderr, "Usage: %s [file.vn]\n", argv[0]);
        return 1;
    }

    char *source = read_file(argv[1]);
    if (!source) return 1;

    int result = run_source(source, argv[1]);
    free(source);
    return result;
}
