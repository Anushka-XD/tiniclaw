/*
 * tiniclaw-c — SQLite memory backend
 * FTS5 full-text search + vector BLOB cosine similarity hybrid search
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <math.h>
#include <sqlite3.h>
#include "memory.h"
#include "util.h"
#include "platform.h"

typedef struct {
    sqlite3 *db;
    double   vector_weight;
    double   keyword_weight;
    bool     hygiene_enabled;
} SqliteMem;

/* ── Schema ─────────────────────────────────────────────────────── */

static const char *SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS memories ("
    "  id        TEXT PRIMARY KEY,"
    "  key       TEXT NOT NULL,"
    "  content   TEXT NOT NULL,"
    "  category  TEXT NOT NULL DEFAULT 'core',"
    "  session_id TEXT,"
    "  timestamp TEXT NOT NULL,"
    "  embedding BLOB"
    ");"
    "CREATE VIRTUAL TABLE IF NOT EXISTS memories_fts USING fts5("
    "  content, key,"
    "  content=memories, content_rowid=rowid"
    ");"
    "CREATE TRIGGER IF NOT EXISTS memories_ai AFTER INSERT ON memories BEGIN"
    "  INSERT INTO memories_fts(rowid, content, key) VALUES (new.rowid, new.content, new.key);"
    "END;"
    "CREATE TRIGGER IF NOT EXISTS memories_ad AFTER DELETE ON memories BEGIN"
    "  INSERT INTO memories_fts(memories_fts, rowid, content, key)"
    "    VALUES('delete', old.rowid, old.content, old.key);"
    "END;";

static int init_db(SqliteMem *m, const char *db_path) {
    if (sqlite3_open(db_path, &m->db) != SQLITE_OK) return -1;
    char *errmsg = NULL;
    if (sqlite3_exec(m->db, SCHEMA_SQL, NULL, NULL, &errmsg) != SQLITE_OK) {
        fprintf(stderr, "[memory] schema error: %s\n", errmsg);
        sqlite3_free(errmsg);
        return -1;
    }
    return 0;
}

/* ── Cosine similarity ──────────────────────────────────────────── */

double nc_cosine_similarity(const float *a, const float *b, size_t n) {
    double dot = 0, na = 0, nb = 0;
    for (size_t i = 0; i < n; i++) {
        dot += (double)a[i] * (double)b[i];
        na  += (double)a[i] * (double)a[i];
        nb  += (double)b[i] * (double)b[i];
    }
    if (na == 0 || nb == 0) return 0;
    return dot / (sqrt(na) * sqrt(nb));
}

double nc_hybrid_score(double v, double k, double vw, double kw) {
    return v * vw + k * kw;
}

/* ── VTable implementations ─────────────────────────────────────── */

static int sqlite_store(void *self, NcArena *arena,
                         const char *key, const char *content,
                         NcMemoryCategoryRef cat, const char *session_id) {
    (void)arena;
    SqliteMem *m = (SqliteMem *)self;
    char ts[32]; nc_iso8601_now(ts, sizeof ts);
    char id[64]; snprintf(id, sizeof id, "mem_%lld", (long long)time(NULL));

    /* Simple UUID-ish ID */
    uint8_t rnd[8]; nc_random_bytes(rnd, sizeof rnd);
    char hex[17]; nc_hex_encode(rnd, sizeof rnd, hex);
    snprintf(id, sizeof id, "%s", hex);

    const char *cat_str = (cat.kind == NC_MEM_CORE) ? "core" :
                          (cat.kind == NC_MEM_DAILY) ? "daily" :
                          (cat.kind == NC_MEM_CONVERSATION) ? "conversation" :
                          (cat.custom_name ? cat.custom_name : "custom");

    sqlite3_stmt *stmt;
    const char *sql =
        "INSERT OR REPLACE INTO memories(id,key,content,category,session_id,timestamp)"
        " VALUES(?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(m->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, content, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, cat_str, -1, SQLITE_STATIC);
    if (session_id) sqlite3_bind_text(stmt, 5, session_id, -1, SQLITE_STATIC);
    else sqlite3_bind_null(stmt, 5);
    sqlite3_bind_text(stmt, 6, ts, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

static int sqlite_recall(void *self, NcArena *arena,
                          const char *query, uint32_t limit,
                          NcMemoryEntry **out, size_t *count_out) {
    SqliteMem *m = (SqliteMem *)self;
    *count_out = 0;
    *out = NULL;

    /* FTS5 search */
    sqlite3_stmt *stmt;
    const char *sql =
        "SELECT m.id, m.key, m.content, m.category, m.timestamp, m.session_id,"
        "       bm25(memories_fts) AS score"
        " FROM memories_fts"
        " JOIN memories m ON memories_fts.rowid = m.rowid"
        " WHERE memories_fts MATCH ?"
        " ORDER BY score LIMIT ?";

    if (sqlite3_prepare_v2(m->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        /* Fallback: full scan */
        const char *sql2 = "SELECT id,key,content,category,timestamp,session_id FROM memories"
                           " ORDER BY timestamp DESC LIMIT ?";
        if (sqlite3_prepare_v2(m->db, sql2, -1, &stmt, NULL) != SQLITE_OK) return -1;
        sqlite3_bind_int(stmt, 1, (int)limit);
    } else {
        sqlite3_bind_text(stmt, 1, query, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, (int)limit);
    }

    NcMemoryEntry *entries = NC_ALLOC_N(arena, NcMemoryEntry, limit);
    if (!entries) { sqlite3_finalize(stmt); return -1; }

    size_t n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && n < (size_t)limit) {
        NcMemoryEntry *e = &entries[n++];
        e->id        = nc_arena_strdup(arena, (const char *)sqlite3_column_text(stmt, 0));
        e->key       = nc_arena_strdup(arena, (const char *)sqlite3_column_text(stmt, 1));
        e->content   = nc_arena_strdup(arena, (const char *)sqlite3_column_text(stmt, 2));
        const char *cat_s = (const char *)sqlite3_column_text(stmt, 3);
        e->category  = (NcMemoryCategoryRef){
            .kind = strcmp(cat_s, "core") == 0 ? NC_MEM_CORE :
                    strcmp(cat_s, "daily") == 0 ? NC_MEM_DAILY :
                    strcmp(cat_s, "conversation") == 0 ? NC_MEM_CONVERSATION : NC_MEM_CUSTOM,
            .custom_name = strcmp(cat_s, "custom") == 0 ? nc_arena_strdup(arena, cat_s) : NULL
        };
        e->timestamp = nc_arena_strdup(arena, (const char *)sqlite3_column_text(stmt, 4));
        const unsigned char *sid = sqlite3_column_text(stmt, 5);
        e->session_id = sid ? nc_arena_strdup(arena, (const char *)sid) : NULL;
        e->score = sqlite3_column_count(stmt) > 6 ? sqlite3_column_double(stmt, 6) : 0.0;
    }
    sqlite3_finalize(stmt);
    *out = entries;
    *count_out = n;
    return 0;
}

static int sqlite_get(void *self, NcArena *arena, const char *key, NcMemoryEntry *out) {
    SqliteMem *m = (SqliteMem *)self;
    sqlite3_stmt *stmt;
    const char *sql = "SELECT id,key,content,category,timestamp,session_id FROM memories WHERE key=? LIMIT 1";
    if (sqlite3_prepare_v2(m->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); return -1; }
    out->id        = nc_arena_strdup(arena, (const char *)sqlite3_column_text(stmt, 0));
    out->key       = nc_arena_strdup(arena, (const char *)sqlite3_column_text(stmt, 1));
    out->content   = nc_arena_strdup(arena, (const char *)sqlite3_column_text(stmt, 2));
    out->timestamp = nc_arena_strdup(arena, (const char *)sqlite3_column_text(stmt, 4));
    sqlite3_finalize(stmt);
    return 0;
}

static int sqlite_forget(void *self, const char *key) {
    SqliteMem *m = (SqliteMem *)self;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(m->db, "DELETE FROM memories WHERE key=?", -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

static uint64_t sqlite_count(void *self) {
    SqliteMem *m = (SqliteMem *)self;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(m->db, "SELECT COUNT(*) FROM memories", -1, &stmt, NULL) != SQLITE_OK) return 0;
    uint64_t n = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) n = (uint64_t)sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
}

static void sqlite_deinit(void *self) {
    SqliteMem *m = (SqliteMem *)self;
    if (m->db) sqlite3_close(m->db);
    free(m);
}

static const NcMemoryVTable SQLITE_VTABLE = {
    .store  = sqlite_store,
    .recall = sqlite_recall,
    .get    = sqlite_get,
    .forget = sqlite_forget,
    .count  = sqlite_count,
    .deinit = sqlite_deinit,
};

NcMemory nc_memory_sqlite_init(const NcSqliteMemoryConfig *cfg) {
    SqliteMem *m = calloc(1, sizeof *m);
    if (!m) return (NcMemory){0};
    m->vector_weight  = cfg->vector_weight > 0 ? cfg->vector_weight : 0.7;
    m->keyword_weight = cfg->keyword_weight > 0 ? cfg->keyword_weight : 0.3;
    m->hygiene_enabled = cfg->hygiene_enabled;
    if (init_db(m, cfg->db_path) != 0) { free(m); return (NcMemory){0}; }
    return (NcMemory){ .ptr = m, .vtable = &SQLITE_VTABLE };
}

/* ── None backend ────────────────────────────────────────────────── */

static int none_store(void *s, NcArena *a, const char *k, const char *c,
                       NcMemoryCategoryRef cat, const char *sid) {
    (void)s;(void)a;(void)k;(void)c;(void)cat;(void)sid; return 0;
}
static int none_recall(void *s, NcArena *a, const char *q, uint32_t l,
                        NcMemoryEntry **o, size_t *c) {
    (void)s;(void)a;(void)q;(void)l; *o=NULL; *c=0; return 0;
}
static int none_get(void *s, NcArena *a, const char *k, NcMemoryEntry *o) {
    (void)s;(void)a;(void)k;(void)o; return -1;
}
static int none_forget(void *s, const char *k) { (void)s;(void)k; return 0; }
static uint64_t none_count(void *s) { (void)s; return 0; }
static void none_deinit(void *s) { (void)s; }

static const NcMemoryVTable NONE_VTABLE = {
    .store=none_store,.recall=none_recall,.get=none_get,
    .forget=none_forget,.count=none_count,.deinit=none_deinit
};
static int NONE_INST = 0;
NcMemory nc_memory_none_init(void) {
    return (NcMemory){ .ptr = &NONE_INST, .vtable = &NONE_VTABLE };
}
