#pragma once
/*
 * tiniclaw-c — arena allocator
 * A bump-pointer arena: allocate from a fixed buffer; free everything at once.
 * Used for per-request/per-turn lifetime management, mirroring Zig's ArenaAllocator.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define NC_ARENA_DEFAULT_SIZE (1024 * 1024)  /* 1 MB default */

typedef struct NcArena {
    uint8_t *buf;
    size_t   cap;
    size_t   pos;
    bool     owns_buf;   /* false if buf was supplied externally */
} NcArena;

/* Create an arena backed by a heap allocation of `size` bytes. */
NcArena *nc_arena_new(size_t size);

/* Create an arena over an existing stack/static buffer (no heap). */
NcArena  nc_arena_from_buf(uint8_t *buf, size_t size);

/* Allocate `size` bytes aligned to `align` from the arena. Returns NULL on OOM. */
void    *nc_arena_alloc(NcArena *a, size_t size, size_t align);

/* Convenience: allocate and zero-fill. */
void    *nc_arena_zalloc(NcArena *a, size_t size, size_t align);

/* Duplicate a string into the arena. */
char    *nc_arena_strdup(NcArena *a, const char *s);

/* Duplicate a string slice (not NUL-terminated source) into the arena. */
char    *nc_arena_strndup(NcArena *a, const char *s, size_t n);

/* Reset the arena (reuse all memory, does NOT free the backing buffer). */
void     nc_arena_reset(NcArena *a);

/* Free the arena and its backing buffer (if owned). */
void     nc_arena_free(NcArena *a);

/* Typed allocation helpers */
#define NC_ALLOC(arena, T)        ((T *)nc_arena_alloc((arena), sizeof(T), _Alignof(T)))
#define NC_ALLOC_N(arena, T, n)   ((T *)nc_arena_alloc((arena), sizeof(T)*(n), _Alignof(T)))
#define NC_ZALLOC(arena, T)       ((T *)nc_arena_zalloc((arena), sizeof(T), _Alignof(T)))
