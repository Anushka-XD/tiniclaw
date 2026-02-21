#pragma once
/*
 * tiniclaw-c — memory vtable interface
 * Mirrors tiniclaw's Memory vtable from memory/root.zig
 */
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "arena.h"

/* ── Memory categories ──────────────────────────────────────────── */

typedef enum NcMemoryCategory {
    NC_MEM_CORE,
    NC_MEM_DAILY,
    NC_MEM_CONVERSATION,
    NC_MEM_CUSTOM,    /* uses custom_name */
} NcMemoryCategory;

typedef struct NcMemoryCategoryRef {
    NcMemoryCategory kind;
    const char      *custom_name; /* only valid if kind == NC_MEM_CUSTOM */
} NcMemoryCategoryRef;

/* ── Memory entry ────────────────────────────────────────────────── */

typedef struct NcMemoryEntry {
    const char          *id;
    const char          *key;
    const char          *content;
    NcMemoryCategoryRef  category;
    const char          *timestamp;
    const char          *session_id;   /* NULL if none */
    double               score;        /* 0.0 if not scored */
} NcMemoryEntry;

/* ── Memory vtable ───────────────────────────────────────────────── */

typedef struct NcMemoryVTable {
    /* Store a new memory. Returns 0 on success. */
    int   (*store)(void *self, NcArena *arena,
                   const char *key, const char *content,
                   NcMemoryCategoryRef cat, const char *session_id);

    /* Recall memories matching query (hybrid FTS5 + vector).
       Writes up to `limit` results into `out`; sets *count_out. */
    int   (*recall)(void *self, NcArena *arena,
                    const char *query, uint32_t limit,
                    NcMemoryEntry **out, size_t *count_out);

    /* Get a specific memory by key. */
    int   (*get)(void *self, NcArena *arena,
                 const char *key, NcMemoryEntry *out);

    /* Forget (delete) a memory by key. */
    int   (*forget)(void *self, const char *key);

    /* Count stored memories. */
    uint64_t (*count)(void *self);

    /* Flush/close (called on shutdown). */
    void  (*deinit)(void *self);
} NcMemoryVTable;

typedef struct NcMemory {
    void                *ptr;
    const NcMemoryVTable *vtable;
} NcMemory;

static inline int nc_memory_store(NcMemory m, NcArena *a,
                                   const char *key, const char *content,
                                   NcMemoryCategoryRef cat, const char *session_id) {
    return m.vtable->store(m.ptr, a, key, content, cat, session_id);
}
static inline int nc_memory_recall(NcMemory m, NcArena *a,
                                    const char *query, uint32_t limit,
                                    NcMemoryEntry **out, size_t *count_out) {
    return m.vtable->recall(m.ptr, a, query, limit, out, count_out);
}
static inline int nc_memory_forget(NcMemory m, const char *key) {
    return m.vtable->forget(m.ptr, key);
}
static inline uint64_t nc_memory_count(NcMemory m) { return m.vtable->count(m.ptr); }
static inline void nc_memory_deinit(NcMemory m) { if (m.vtable->deinit) m.vtable->deinit(m.ptr); }

/* ── Concrete backends ───────────────────────────────────────────── */

/* SQLite backend — FTS5 full-text + vector cosine similarity search */
typedef struct NcSqliteMemoryConfig {
    const char *db_path;        /* e.g. ~/.tiniclaw/memory.db */
    double      vector_weight;  /* 0.0–1.0, default 0.7 */
    double      keyword_weight; /* 0.0–1.0, default 0.3 */
    bool        hygiene_enabled;
    bool        embedding_enabled;
    const char *embedding_url;  /* NULL = use OpenAI */
    const char *embedding_key;
} NcSqliteMemoryConfig;

NcMemory nc_memory_sqlite_init(const NcSqliteMemoryConfig *cfg);

/* Markdown backend — flat file-based storage */
NcMemory nc_memory_markdown_init(NcArena *arena, const char *dir_path);

/* None backend — no-op */
NcMemory nc_memory_none_init(void);

/* ── Embedding provider vtable ───────────────────────────────────── */

typedef struct NcEmbeddingVTable {
    /* Embed text into a float vector. out must be pre-allocated.
       Returns dimension count, or -1 on error. */
    int  (*embed)(void *self, const char *text, float *out, size_t out_size);
    void (*deinit)(void *self);
} NcEmbeddingVTable;

typedef struct NcEmbeddingProvider {
    void                      *ptr;
    const NcEmbeddingVTable   *vtable;
} NcEmbeddingProvider;

/* Cosine similarity between two vectors of length n. */
double nc_cosine_similarity(const float *a, const float *b, size_t n);

/* Hybrid score merge (vector + keyword). */
double nc_hybrid_score(double vector_score, double keyword_score,
                       double vector_weight, double keyword_weight);
