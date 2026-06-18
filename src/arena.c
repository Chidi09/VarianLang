#include "varian.h"
#include <stdlib.h>
#include <string.h>

#define ARENA_DEFAULT_SIZE (64 * 1024)  /* 64KB blocks */

Arena *arena_create(size_t capacity) {
    Arena *arena = (Arena *)malloc(sizeof(Arena));
    if (!arena) return NULL;
    if (capacity == 0) capacity = ARENA_DEFAULT_SIZE;
    arena->base = (unsigned char *)malloc(capacity);
    if (!arena->base) {
        free(arena);
        return NULL;
    }
    arena->capacity = capacity;
    arena->offset = 0;
    arena->next = NULL;
    return arena;
}

void *arena_alloc(Arena *arena, size_t size) {
    /* Align to 8 bytes */
    size = (size + 7) & ~7;

    if (arena->offset + size > arena->capacity) {
        /* Allocate a new block */
        size_t new_cap = arena->capacity * 2;
        if (new_cap < size) new_cap = size + ARENA_DEFAULT_SIZE;
        Arena *new_block = arena_create(new_cap);
        if (!new_block) return NULL;
        new_block->next = arena->next;
        arena->next = new_block;
        return arena_alloc(new_block, size);
    }

    void *ptr = arena->base + arena->offset;
    arena->offset += size;
    memset(ptr, 0, size);
    return ptr;
}

void arena_destroy(Arena *arena) {
    while (arena) {
        Arena *next = arena->next;
        free(arena->base);
        free(arena);
        arena = next;
    }
}
