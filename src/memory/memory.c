/*
 * tiniclaw-c — markdown memory backend
 * Stores entries as <key>.md files in a directory.
 * Implements the NcMemoryVTable from memory.h exactly.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include "memory.h"
#include "arena.h"
#include "util.h"
#include "platform.h"

/* Sanitise a key to a safe filename component */
static void safe_key(const char *key, char *out, size_t outsz) {
    size_t i;
    for (i = 0; key[i] && i + 1 < outsz; i++)
        out[i] = (key[i] == '/' || key[i] == '\0') ? '_' : key[i];
    out[i] = '\0';
}

typedef struct {
    char dir[4096];
} MarkdownMem;

/* store: write content to <dir>/<key>.md, session_id stored as front-matter */
static int md_store(void *ptr, NcArena *arena, const char *key,
                    const char *content, NcMemoryCategoryRef cat,
                    const char *session_id) {
    (void)arena; (void)cat;
    MarkdownMem *m = ptr;
    char safe[256];
    safe_key(key, safe, sizeof safe);
    char path[4096 + 256 + 4];
    snprintf(path, sizeof path, "%s/%s.md", m->dir, safe);

    FILE *f = fopen(path, "w");
    if (!f) return -1;
    if (session_id && session_id[0])
        fprintf(f, "<!-- session: %s -->\n", session_id);
    fprintf(f, "%s", content ? content : "");
    fclose(f);
    return 0;
}

/* recall: substring-match query against all .md files, fill out array */
static int md_recall(void *ptr, NcArena *arena, const char *query,
                     uint32_t limit, NcMemoryEntry **out, size_t *count_out) {
    MarkdownMem *m = ptr;
    *count_out = 0;
    *out = NULL;

    DIR *d = opendir(m->dir);
    if (!d) return -1;

    NcMemoryEntry *results = NC_ALLOC_N(arena, NcMemoryEntry, limit);
    size_t found = 0;

    struct dirent *ent;
    while ((ent = readdir(d)) && found < (size_t)limit) {
        const char *name = ent->d_name;
        size_t nlen = strlen(name);
        if (nlen < 4 || strcmp(name + nlen - 3, ".md") != 0) continue;

        char path[4096 + 256 + 4];
        snprintf(path, sizeof path, "%s/%s", m->dir, name);
        size_t flen = 0;
        char *fcontent = nc_read_file(path, &flen);
        if (!fcontent) continue;

        if (!query || !query[0] || strstr(fcontent, query)) {
            /* strip .md suffix for the key */
            char key_buf[256];
            size_t klen = nlen >= 3 ? nlen - 3 : 0;
            if (klen >= sizeof key_buf) klen = sizeof key_buf - 1;
            memcpy(key_buf, name, klen);
            key_buf[klen] = '\0';
            char *k = nc_arena_strdup(arena, key_buf);
            results[found].key     = k;
            results[found].content = nc_arena_strdup(arena, fcontent);
            results[found].score   = 1.0;
            results[found].id      = k;
            found++;
        }
        free(fcontent);
    }
    closedir(d);
    *out = results;
    *count_out = found;
    return 0;
}

/* get: read one entry by key */
static int md_get(void *ptr, NcArena *arena, const char *key,
                  NcMemoryEntry *out) {
    MarkdownMem *m = ptr;
    char safe[256];
    safe_key(key, safe, sizeof safe);
    char path[4096 + 256 + 4];
    snprintf(path, sizeof path, "%s/%s.md", m->dir, safe);

    size_t flen = 0;
    char *fcontent = nc_read_file(path, &flen);
    if (!fcontent) return -1;

    memset(out, 0, sizeof *out);
    out->key     = nc_arena_strdup(arena, key);
    out->content = nc_arena_strdup(arena, fcontent);
    out->score   = 1.0;
    free(fcontent);
    return 0;
}

/* forget: delete <dir>/<key>.md */
static int md_forget(void *ptr, const char *key) {
    MarkdownMem *m = ptr;
    char safe[256];
    safe_key(key, safe, sizeof safe);
    char path[4096 + 256 + 4];
    snprintf(path, sizeof path, "%s/%s.md", m->dir, safe);
    return remove(path) == 0 ? 0 : -1;
}

/* count: number of .md files */
static uint64_t md_count(void *ptr) {
    MarkdownMem *m = ptr;
    DIR *d = opendir(m->dir);
    if (!d) return 0;
    uint64_t n = 0;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        size_t l = strlen(ent->d_name);
        if (l >= 4 && strcmp(ent->d_name + l - 3, ".md") == 0) n++;
    }
    closedir(d);
    return n;
}

static void md_deinit(void *ptr) { free(ptr); }

static const NcMemoryVTable MD_VTABLE = {
    .store  = md_store,
    .recall = md_recall,
    .get    = md_get,
    .forget = md_forget,
    .count  = md_count,
    .deinit = md_deinit,
};

NcMemory nc_memory_markdown_init(NcArena *arena, const char *dir) {
    (void)arena;
    MarkdownMem *m = calloc(1, sizeof *m);
    if (!m) return (NcMemory){0};
    snprintf(m->dir, sizeof m->dir, "%s", dir ? dir : ".");
    nc_ensure_dir(m->dir);
    return (NcMemory){ .ptr = m, .vtable = &MD_VTABLE };
}
