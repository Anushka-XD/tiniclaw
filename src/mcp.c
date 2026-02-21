/*
 * tiniclaw-c — MCP (Model Context Protocol) JSON-RPC 2.0 stdio bridge
 *
 * Connects to an external MCP server over stdio (spawn child + pipe).
 * Supports: tools/list, tools/call
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <cjson/cJSON.h>
#include "gateway.h"
#include "arena.h"
#include "util.h"

/* ── Internal connection state (stored in stderr_fd as sentinel) ─── */
/* We reuse the NcMcpServer fields directly:
 *   pid       = child pid
 *   stdin_fd  = write end (to child stdin)
 *   stdout_fd = read end  (from child stdout)
 *   stderr_fd = read end  (from child stderr); -1 when not connected
 */

static cJSON *mcp_rpc(NcMcpServer *s, const char *method, cJSON *params) {
    if (s->stdin_fd < 0 || s->stdout_fd < 0) return NULL;

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddStringToObject(req, "method", method);
    cJSON_AddNumberToObject(req, "id", (double)s->next_id++);
    if (params) cJSON_AddItemToObject(req, "params", params);
    else cJSON_AddObjectToObject(req, "params");

    char *line = cJSON_Print(req);
    cJSON_Delete(req);
    /* Collapse to single line + newline for line-framed JSON-RPC */
    for (char *p = line; *p; p++) if (*p == '\n') *p = ' ';
    dprintf(s->stdin_fd, "%s\n", line);
    free(line);

    /* Read response line */
    char resp_buf[65536];
    ssize_t n = read(s->stdout_fd, resp_buf, sizeof resp_buf - 1);
    if (n <= 0) return NULL;
    resp_buf[n] = '\0';
    /* Strip trailing newline */
    for (ssize_t i = n - 1; i >= 0 && (resp_buf[i] == '\n' || resp_buf[i] == '\r'); i--)
        resp_buf[i] = '\0';
    return cJSON_Parse(resp_buf);
}

/* ── Public API ─────────────────────────────────────────────────── */

int nc_mcp_server_connect(NcMcpServer *s, NcArena *arena) {
    (void)arena;
    if (!s || !s->cfg.command[0]) return -1;

    int to_child[2], from_child[2], err_pipe[2];
    if (pipe(to_child) < 0)   return -1;
    if (pipe(from_child) < 0) { close(to_child[0]); close(to_child[1]); return -1; }
    if (pipe(err_pipe) < 0)   {
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        /* Child */
        dup2(to_child[0],   STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        dup2(err_pipe[1],   STDERR_FILENO);
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        close(err_pipe[0]); close(err_pipe[1]);

        /* Build argv from cfg */
        const char *argv[12];
        int ai = 0;
        argv[ai++] = s->cfg.command;
        for (size_t i = 0; i < s->cfg.args_count && i < 8; i++)
            argv[ai++] = s->cfg.args[i];
        argv[ai] = NULL;

        /* Set env vars */
        for (size_t i = 0; i < s->cfg.env_count && i < 8; i++) {
            char ev[384];
            snprintf(ev, sizeof ev, "%s=%s", s->cfg.env_keys[i], s->cfg.env_vals[i]);
            putenv(nc_strdup(ev)); /* intentional: child process memory */
        }

        execvp(s->cfg.command, (char *const *)argv);
        _exit(1);
    }

    /* Parent */
    close(to_child[0]); close(from_child[1]); close(err_pipe[1]);
    s->pid       = pid;
    s->stdin_fd  = to_child[1];
    s->stdout_fd = from_child[0];
    s->stderr_fd = err_pipe[0];
    s->next_id   = 1;

    /* MCP initialize handshake */
    cJSON *init_params = cJSON_CreateObject();
    cJSON_AddStringToObject(init_params, "protocolVersion", "2025-03-26");
    cJSON *ci = cJSON_CreateObject();
    cJSON_AddStringToObject(ci, "name", "tiniclaw");
    cJSON_AddStringToObject(ci, "version", "0.1.0");
    cJSON_AddItemToObject(init_params, "clientInfo", ci);
    cJSON *resp = mcp_rpc(s, "initialize", init_params);
    if (resp) cJSON_Delete(resp);

    /* Send initialized notification (no response expected) */
    cJSON *notif = cJSON_CreateObject();
    cJSON_AddStringToObject(notif, "jsonrpc", "2.0");
    cJSON_AddStringToObject(notif, "method", "notifications/initialized");
    char *ns = cJSON_Print(notif);
    cJSON_Delete(notif);
    for (char *p = ns; *p; p++) if (*p == '\n') *p = ' ';
    dprintf(s->stdin_fd, "%s\n", ns);
    free(ns);

    /* Discover tools */
    cJSON *tlist = mcp_rpc(s, "tools/list", cJSON_CreateObject());
    if (tlist) {
        cJSON *result = cJSON_GetObjectItemCaseSensitive(tlist, "result");
        cJSON *tools  = result ? cJSON_GetObjectItemCaseSensitive(result, "tools") : NULL;
        if (cJSON_IsArray(tools)) {
            int cnt = cJSON_GetArraySize(tools);
            s->tools = calloc((size_t)cnt, sizeof *s->tools);
            int added = 0;
            cJSON *t = NULL;
            cJSON_ArrayForEach(t, tools) {
                cJSON *n_j = cJSON_GetObjectItemCaseSensitive(t, "name");
                cJSON *d_j = cJSON_GetObjectItemCaseSensitive(t, "description");
                cJSON *sc  = cJSON_GetObjectItemCaseSensitive(t, "inputSchema");
                if (!n_j || !cJSON_IsString(n_j)) continue;
                NcMcpTool *mt = &s->tools[added++];
                snprintf(mt->name, sizeof mt->name, "%s", n_j->valuestring);
                if (d_j && cJSON_IsString(d_j))
                    snprintf(mt->description, sizeof mt->description, "%s", d_j->valuestring);
                if (sc) {
                    char *sc_str = cJSON_PrintUnformatted(sc);
                    if (sc_str) {
                        snprintf(mt->input_schema, sizeof mt->input_schema, "%s", sc_str);
                        free(sc_str);
                    }
                }
            }
            s->tools_count = (size_t)added;
        }
        cJSON_Delete(tlist);
    }

    return 0;
}

void nc_mcp_server_disconnect(NcMcpServer *s) {
    if (!s || s->pid <= 0) return;
    if (s->stdin_fd  >= 0) { close(s->stdin_fd);  s->stdin_fd  = -1; }
    if (s->stdout_fd >= 0) { close(s->stdout_fd); s->stdout_fd = -1; }
    if (s->stderr_fd >= 0) { close(s->stderr_fd); s->stderr_fd = -1; }
    kill(s->pid, SIGTERM);
    waitpid(s->pid, NULL, 0);
    s->pid = 0;
    free(s->tools);
    s->tools = NULL;
    s->tools_count = 0;
}

char *nc_mcp_call_tool(NcMcpServer *s, NcArena *arena,
                        const char *tool_name, const char *args_json) {
    (void)arena;
    if (!s || s->stdin_fd < 0) return nc_strdup("(MCP server not connected)");

    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "name", tool_name ? tool_name : "");
    if (args_json && args_json[0]) {
        cJSON *args = cJSON_Parse(args_json);
        if (args) cJSON_AddItemToObject(params, "arguments", args);
    }

    cJSON *resp = mcp_rpc(s, "tools/call", params);
    if (!resp) return nc_strdup("(MCP call failed: no response)");

    cJSON *error  = cJSON_GetObjectItemCaseSensitive(resp, "error");
    if (error) {
        cJSON *emsg = cJSON_GetObjectItemCaseSensitive(error, "message");
        const char *msg = (emsg && cJSON_IsString(emsg)) ? emsg->valuestring : "MCP error";
        char *r = nc_strdup(msg);
        cJSON_Delete(resp);
        return r;
    }

    cJSON *result  = cJSON_GetObjectItemCaseSensitive(resp, "result");
    cJSON *content = result ? cJSON_GetObjectItemCaseSensitive(result, "content") : NULL;

    /* Concatenate text content items */
    NcBuf out; nc_buf_init(&out);
    if (cJSON_IsArray(content)) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, content) {
            cJSON *type_j = cJSON_GetObjectItemCaseSensitive(item, "type");
            cJSON *text_j = cJSON_GetObjectItemCaseSensitive(item, "text");
            if (type_j && cJSON_IsString(type_j) &&
                strcmp(type_j->valuestring, "text") == 0 &&
                text_j && cJSON_IsString(text_j)) {
                nc_buf_appendz(&out, text_j->valuestring);
            }
        }
    }
    cJSON_Delete(resp);

    char *r = nc_strdup(out.data ? out.data : "");
    nc_buf_free(&out);
    return r;
}

/* ── Wrap MCP tools as NcTool vtables ───────────────────────────── */

typedef struct {
    NcMcpServer *server;
    char name[128];
    char description[512];
    char *params_json; /* heap-allocated; freed in deinit */
} McpToolCtx;

static NcToolResult mcp_tool_execute(void *ptr, NcArena *arena, cJSON *args) {
    McpToolCtx *t = ptr;
    char *args_json = args ? cJSON_PrintUnformatted(args) : NULL;
    char *result = nc_mcp_call_tool(t->server, arena, t->name,
                                     args_json ? args_json : "{}");
    free(args_json);
    NcToolResult r = nc_tool_result_ok(result ? result : "");
    free(result);
    return r;
}
static const char *mcp_tool_name(void *p)   { return ((McpToolCtx *)p)->name; }
static const char *mcp_tool_desc(void *p)   { return ((McpToolCtx *)p)->description; }
static const char *mcp_tool_params(void *p) {
    McpToolCtx *t = p;
    return t->params_json ? t->params_json : "{}";
}
static void mcp_tool_deinit(void *p) {
    McpToolCtx *t = p;
    free(t->params_json);
    free(t);
}

static const NcToolVTable MCP_TOOL_VTABLE = {
    mcp_tool_execute, mcp_tool_name, mcp_tool_desc, mcp_tool_params, mcp_tool_deinit
};

NcTool *nc_mcp_wrap_tools(NcArena *arena, NcMcpServer *servers,
                            size_t servers_count, size_t *tools_count) {
    *tools_count = 0;
    if (!servers || servers_count == 0) return NULL;

    /* Count total tools */
    size_t total = 0;
    for (size_t i = 0; i < servers_count; i++) total += servers[i].tools_count;
    if (total == 0) return NULL;

    NcTool *tools = NC_ALLOC_N(arena, NcTool, total);
    size_t added = 0;

    for (size_t si = 0; si < servers_count; si++) {
        NcMcpServer *s = &servers[si];
        for (size_t ti = 0; ti < s->tools_count; ti++) {
            NcMcpTool *mt = &s->tools[ti];
            McpToolCtx *ctx = calloc(1, sizeof *ctx);
            ctx->server = s;
            snprintf(ctx->name, sizeof ctx->name, "%s", mt->name);
            snprintf(ctx->description, sizeof ctx->description, "%s", mt->description);
            if (mt->input_schema[0]) ctx->params_json = nc_strdup(mt->input_schema);
            tools[added++] = (NcTool){ .ptr = ctx, .vtable = &MCP_TOOL_VTABLE };
        }
    }

    *tools_count = added;
    return tools;
}
