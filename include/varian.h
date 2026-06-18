#ifndef VARIAN_H
#define VARIAN_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Varian Language Runtime & Compiler
 * Phase 1: Core Compiler & Runtime Engine
 */

/* Version */
#define VARIAN_VERSION "0.1.0"

/* Source location tracking */
typedef struct {
    const char *filename;
    int line;
    int column;
    int offset;
} SourceLoc;

/* Memory arena for fast, contiguous allocations */
typedef struct Arena Arena;
struct Arena {
    unsigned char *base;
    size_t capacity;
    size_t offset;
    Arena *next;
};

Arena *arena_create(size_t capacity);
void *arena_alloc(Arena *arena, size_t size);
void arena_destroy(Arena *arena);

#endif /* VARIAN_H */
