/*
 * tiniclaw-c — tool infrastructure: default tool set
 * Vtable helper functions are static inline in tool.h.
 * This file only provides nc_tools_default() and nc_tool_result_free().
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <cjson/cJSON.h>
#include "tool.h"
#include "arena.h"
#include "util.h"

/* ── Default tool set ────────────────────────────────────────────── */

NcTool *nc_tools_default(NcArena *arena, const NcToolContext *ctx, size_t *out_count) {
    static const int TOOL_COUNT = 18;
    NcTool *tools = NC_ALLOC_N(arena, NcTool, TOOL_COUNT);
    if (!tools) { *out_count = 0; return NULL; }
    int n = 0;
    tools[n++] = nc_tool_shell_init(ctx);
    tools[n++] = nc_tool_file_read_init(ctx);
    tools[n++] = nc_tool_file_write_init(ctx);
    tools[n++] = nc_tool_file_edit_init(ctx);
    tools[n++] = nc_tool_file_append_init(ctx);
    tools[n++] = nc_tool_http_request_init(ctx);
    tools[n++] = nc_tool_git_init(ctx);
    tools[n++] = nc_tool_memory_store_init(ctx);
    tools[n++] = nc_tool_memory_recall_init(ctx);
    tools[n++] = nc_tool_memory_forget_init(ctx);
    tools[n++] = nc_tool_browser_open_init(ctx);
    tools[n++] = nc_tool_web_search_init(ctx);
    tools[n++] = nc_tool_web_fetch_init(ctx);
    tools[n++] = nc_tool_image_init(ctx);
    tools[n++] = nc_tool_spawn_init(ctx);
    tools[n++] = nc_tool_delegate_init(ctx);
    tools[n++] = nc_tool_cron_add_init(ctx);
    tools[n++] = nc_tool_cron_list_init(ctx);
    *out_count = (size_t)n;
    return tools;
}
