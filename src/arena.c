/*
 * tiniclaw-c — arena allocator implementation
 */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "arena.h"

/* Align `pos` up to `align` (must be power of 2). */
static size_t align_up(size_t pos, size_t align) {
    return (pos + align - 1) & ~(align - 1);
}

NcArena *nc_arena_new(size_t size) {
    NcArena *a = malloc(sizeof(NcArena));
    if (!a) return NULL;
    a->buf = malloc(size);
    if (!a->buf) { free(a); return NULL; }
    a->cap      = size;
    a->pos      = 0;
    a->owns_buf = true;
    return a;
}

NcArena nc_arena_from_buf(uint8_t *buf, size_t size) {
    return (NcArena){ .buf = buf, .cap = size, .pos = 0, .owns_buf = false };
}

void *nc_arena_alloc(NcArena *a, size_t size, size_t align) {
    size_t p = align_up(a->pos, align);
    if (p + size > a->cap) return NULL;   /* OOM — caller handles */
    a->pos = p + size;
    return a->buf + p;
}

void *nc_arena_zalloc(NcArena *a, size_t size, size_t align) {
    void *p = nc_arena_alloc(a, size, align);
    if (p) memset(p, 0, size);
    return p;
}

char *nc_arena_strdup(NcArena *a, const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = nc_arena_alloc(a, n + 1, 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

char *nc_arena_strndup(NcArena *a, const char *s, size_t n) {
    if (!s) return NULL;
    char *p = nc_arena_alloc(a, n + 1, 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void nc_arena_reset(NcArena *a) {
    a->pos = 0;
}

void nc_arena_free(NcArena *a) {
    if (!a) return;
    if (a->owns_buf) free(a->buf);
    free(a);
}
