#pragma once
/*
 * tiniclaw-c — tool vtable interface
 * Mirrors tiniclaw's Tool vtable from tools/root.zig
 */
#include <stddef.h>
#include <stdbool.h>
#include "arena.h"
#include <cjson/cJSON.h>

/* ── Tool result ──────────────────────────────────────────────────── */

typedef struct NcToolResult {
    bool  success;
    char *output;     /* heap-allocated or arena; caller frees */
    char *error_msg;  /* heap-allocated or arena; NULL on success */
} NcToolResult;

static inline NcToolResult nc_tool_ok(char *output) {
    return (NcToolResult){ .success = true, .output = output, .error_msg = NULL };
}
static inline NcToolResult nc_tool_fail(char *error_msg) {
    return (NcToolResult){ .success = false, .output = NULL, .error_msg = error_msg };
}

/* Convenience aliases used widely across tool implementations */
#include "util.h"
static inline NcToolResult nc_tool_result_ok(const char *s) {
    return nc_tool_ok(nc_strdup(s ? s : ""));
}
static inline NcToolResult nc_tool_result_err(const char *s) {
    return nc_tool_fail(nc_strdup(s ? s : "error"));
}
static inline void nc_tool_result_free(NcToolResult *r) {
    if (!r) return;
    free(r->output);
    free(r->error_msg);
    r->output = NULL;
    r->error_msg = NULL;
}

/* ── Tool vtable ─────────────────────────────────────────────────── */

typedef struct NcToolVTable {
    NcToolResult (*execute)(void *self, NcArena *arena, cJSON *args);
    const char  *(*name)(void *self);
    const char  *(*description)(void *self);
    const char  *(*parameters_json)(void *self);
    void         (*deinit)(void *self);
} NcToolVTable;

typedef struct NcTool {
    void               *ptr;
    const NcToolVTable *vtable;
} NcTool;

static inline NcToolResult nc_tool_execute(NcTool t, NcArena *a, cJSON *args) {
    return t.vtable->execute(t.ptr, a, args);
}
static inline const char *nc_tool_name(NcTool t) { return t.vtable->name(t.ptr); }
static inline const char *nc_tool_desc(NcTool t) { return t.vtable->description(t.ptr); }
static inline const char *nc_tool_params(NcTool t) { return t.vtable->parameters_json(t.ptr); }
static inline void nc_tool_deinit(NcTool t) { if (t.vtable->deinit) t.vtable->deinit(t.ptr); }

/* ── Tool constructors ───────────────────────────────────────────── */

/* Context shared across all stateful tools */
typedef struct NcToolContext {
    const char *workspace_dir;
    void       *memory;   /* NcMemory* — optional, for memory tools */
    void       *agent;    /* NcAgent*  — optional, for delegate/subagent */
} NcToolContext;

NcTool nc_tool_shell_init(const NcToolContext *ctx);
NcTool nc_tool_file_read_init(const NcToolContext *ctx);
NcTool nc_tool_file_write_init(const NcToolContext *ctx);
NcTool nc_tool_file_edit_init(const NcToolContext *ctx);
NcTool nc_tool_file_append_init(const NcToolContext *ctx);
NcTool nc_tool_http_request_init(const NcToolContext *ctx);
NcTool nc_tool_git_init(const NcToolContext *ctx);
NcTool nc_tool_memory_store_init(const NcToolContext *ctx);
NcTool nc_tool_memory_recall_init(const NcToolContext *ctx);
NcTool nc_tool_memory_forget_init(const NcToolContext *ctx);
NcTool nc_tool_browser_open_init(const NcToolContext *ctx);
NcTool nc_tool_web_search_init(const NcToolContext *ctx);
NcTool nc_tool_web_fetch_init(const NcToolContext *ctx);
NcTool nc_tool_delegate_init(const NcToolContext *ctx);
NcTool nc_tool_spawn_init(const NcToolContext *ctx);
NcTool nc_tool_image_init(const NcToolContext *ctx);
NcTool nc_tool_screenshot_init(const NcToolContext *ctx);
NcTool nc_tool_composio_init(const NcToolContext *ctx);
NcTool nc_tool_hardware_info_init(const NcToolContext *ctx);
NcTool nc_tool_cron_add_init(const NcToolContext *ctx);
NcTool nc_tool_cron_list_init(const NcToolContext *ctx);
NcTool nc_tool_cron_remove_init(const NcToolContext *ctx);
NcTool nc_tool_cron_run_init(const NcToolContext *ctx);

/* Build the full default tool set for an agent.
   Returns a heap-allocated array; caller frees.
   tools_count is set to number of tools. */
NcTool *nc_tools_default(NcArena *arena, const NcToolContext *ctx, size_t *tools_count);
