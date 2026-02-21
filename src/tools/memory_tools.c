/*
 * tiniclaw-c — memory tools: store, recall, forget
 * Implements tool vtables that delegate to NcMemory backend.
 */
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>
#include "tool.h"
#include "memory.h"
#include "arena.h"
#include "util.h"

typedef struct { NcMemory *mem; } MemTool;

/* ── Memory Store ────────────────────────────────────────────────── */

static NcToolResult mem_store_execute(void *ptr, NcArena *arena, cJSON *args) {
    MemTool *t = ptr;
    if (!t->mem || !t->mem->ptr) return nc_tool_result_err("memory backend not configured");

    cJSON *key_j = cJSON_GetObjectItemCaseSensitive(args, "key");
    cJSON *val_j = cJSON_GetObjectItemCaseSensitive(args, "value");
    if (!key_j || !cJSON_IsString(key_j)) return nc_tool_result_err("missing 'key'");
    if (!val_j || !cJSON_IsString(val_j)) return nc_tool_result_err("missing 'value'");

    NcMemoryCategoryRef cat = { .kind = NC_MEM_CORE };
    cJSON *cat_j = cJSON_GetObjectItemCaseSensitive(args, "category");
    if (cat_j && cJSON_IsString(cat_j)) {
        if      (strcmp(cat_j->valuestring, "daily")        == 0) cat.kind = NC_MEM_DAILY;
        else if (strcmp(cat_j->valuestring, "conversation") == 0) cat.kind = NC_MEM_CONVERSATION;
        else if (strcmp(cat_j->valuestring, "custom")       == 0) {
            cat.kind        = NC_MEM_CUSTOM;
            cat.custom_name = cat_j->valuestring;
        }
    }

    int rc = nc_memory_store(*t->mem, arena, key_j->valuestring, val_j->valuestring, cat, NULL);
    return rc == 0 ? nc_tool_result_ok("Stored.") : nc_tool_result_err("Store failed");
}

static const char *mem_store_name(void *p)   { (void)p; return "memory_store"; }
static const char *mem_store_desc(void *p)   { (void)p; return "Store a key-value pair in long-term memory."; }
static const char *mem_store_params(void *p) {
    (void)p;
    return "{\"type\":\"object\","
           "\"properties\":{"
           "\"key\":{\"type\":\"string\"},"
           "\"value\":{\"type\":\"string\"},"
           "\"category\":{\"type\":\"string\","
           "\"enum\":[\"core\",\"daily\",\"conversation\",\"custom\"]}"
           "},"
           "\"required\":[\"key\",\"value\"]}";
}
static void mem_tool_deinit(void *p) { free(p); }

static const NcToolVTable MEM_STORE_VTABLE = {
    .execute        = mem_store_execute,
    .name           = mem_store_name,
    .description    = mem_store_desc,
    .parameters_json = mem_store_params,
    .deinit         = mem_tool_deinit,
};

NcTool nc_tool_memory_store_init(const NcToolContext *ctx) {
    MemTool *t = calloc(1, sizeof *t);
    if (!t) return (NcTool){0};
    if (ctx) t->mem = ctx->memory;
    return (NcTool){ .ptr = t, .vtable = &MEM_STORE_VTABLE };
}

/* ── Memory Recall ───────────────────────────────────────────────── */

static NcToolResult mem_recall_execute(void *ptr, NcArena *arena, cJSON *args) {
    MemTool *t = ptr;
    if (!t->mem || !t->mem->ptr) return nc_tool_result_err("memory backend not configured");

    cJSON *query_j = cJSON_GetObjectItemCaseSensitive(args, "query");
    if (!query_j || !cJSON_IsString(query_j)) return nc_tool_result_err("missing 'query'");
    cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(args, "limit");
    int limit = (limit_j && cJSON_IsNumber(limit_j)) ? (int)limit_j->valuedouble : 5;
    if (limit < 1) limit = 1;
    if (limit > 50) limit = 50;

    NcMemoryEntry *entries = NULL;
    size_t count = 0;
    int rc = nc_memory_recall(*t->mem, arena, query_j->valuestring,
                              (uint32_t)limit, &entries, &count);
    if (rc != 0 || !entries || count == 0)
        return nc_tool_result_ok("(no matching memories)");

    NcBuf out;
    nc_buf_init(&out);
    for (size_t i = 0; i < count; i++) {
        nc_buf_appendf(&out, "[%zu] %s\n%s\n\n",
                       i + 1,
                       entries[i].key     ? entries[i].key     : "",
                       entries[i].content ? entries[i].content : "");
    }
    NcToolResult r = nc_tool_result_ok(out.data ? out.data : "");
    nc_buf_free(&out);
    return r;
}

static const char *mem_recall_name(void *p)   { (void)p; return "memory_recall"; }
static const char *mem_recall_desc(void *p)   { (void)p; return "Search long-term memory with a query string."; }
static const char *mem_recall_params(void *p) {
    (void)p;
    return "{\"type\":\"object\","
           "\"properties\":{"
           "\"query\":{\"type\":\"string\",\"description\":\"Search query\"},"
           "\"limit\":{\"type\":\"integer\",\"default\":5}"
           "},"
           "\"required\":[\"query\"]}";
}

static const NcToolVTable MEM_RECALL_VTABLE = {
    .execute        = mem_recall_execute,
    .name           = mem_recall_name,
    .description    = mem_recall_desc,
    .parameters_json = mem_recall_params,
    .deinit         = mem_tool_deinit,
};

NcTool nc_tool_memory_recall_init(const NcToolContext *ctx) {
    MemTool *t = calloc(1, sizeof *t);
    if (!t) return (NcTool){0};
    if (ctx) t->mem = ctx->memory;
    return (NcTool){ .ptr = t, .vtable = &MEM_RECALL_VTABLE };
}

/* ── Memory Forget ───────────────────────────────────────────────── */

static NcToolResult mem_forget_execute(void *ptr, NcArena *arena, cJSON *args) {
    (void)arena;
    MemTool *t = ptr;
    if (!t->mem || !t->mem->ptr) return nc_tool_result_err("memory backend not configured");

    cJSON *key_j = cJSON_GetObjectItemCaseSensitive(args, "key");
    if (!key_j || !cJSON_IsString(key_j)) return nc_tool_result_err("missing 'key'");

    int rc = nc_memory_forget(*t->mem, key_j->valuestring);
    return rc == 0 ? nc_tool_result_ok("Forgotten.") : nc_tool_result_err("Not found or delete failed");
}

static const char *mem_forget_name(void *p)   { (void)p; return "memory_forget"; }
static const char *mem_forget_desc(void *p)   { (void)p; return "Remove a memory entry by key."; }
static const char *mem_forget_params(void *p) {
    (void)p;
    return "{\"type\":\"object\","
           "\"properties\":{"
           "\"key\":{\"type\":\"string\"}"
           "},"
           "\"required\":[\"key\"]}";
}

static const NcToolVTable MEM_FORGET_VTABLE = {
    .execute        = mem_forget_execute,
    .name           = mem_forget_name,
    .description    = mem_forget_desc,
    .parameters_json = mem_forget_params,
    .deinit         = mem_tool_deinit,
};

NcTool nc_tool_memory_forget_init(const NcToolContext *ctx) {
    MemTool *t = calloc(1, sizeof *t);
    if (!t) return (NcTool){0};
    if (ctx) t->mem = ctx->memory;
    return (NcTool){ .ptr = t, .vtable = &MEM_FORGET_VTABLE };
}
