/*
 * tiniclaw-c — shell tool (runs a command in the workspace)
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <cjson/cJSON.h>
#include "tool.h"
#include "arena.h"
#include "util.h"
#include "security.h"

#define SHELL_OUTPUT_CAP (256 * 1024)  /* 256 KB max output */

typedef struct {
    char workspace_dir[4096];
    NcSecurityPolicy *policy;  /* may be NULL */
} ShellTool;

static NcToolResult shell_execute(void *ptr, NcArena *arena, cJSON *args) {
    ShellTool *t = ptr;
    cJSON *cmd_j = cJSON_GetObjectItemCaseSensitive(args, "command");
    if (!cmd_j || !cJSON_IsString(cmd_j)) return nc_tool_result_err("missing 'command'");
    const char *cmd = cmd_j->valuestring;

    /* Security check */
    if (t->policy) {
        NcCommandRisk risk = nc_classify_command(t->policy, cmd);
        if (risk == NC_RISK_HIGH) return nc_tool_result_err("Command blocked by security policy");
    }

    /* Optional cwd override */
    cJSON *cwd_j = cJSON_GetObjectItemCaseSensitive(args, "cwd");
    const char *cwd = (cwd_j && cJSON_IsString(cwd_j)) ? cwd_j->valuestring : t->workspace_dir;

    /* Build command: run in cwd via subshell */
    char *full_cmd;
    if (cwd && cwd[0]) {
        size_t len = strlen(cwd) + strlen(cmd) + 64;
        full_cmd = malloc(len);
        snprintf(full_cmd, len, "cd '%s' && %s 2>&1", cwd, cmd);
    } else {
        size_t len = strlen(cmd) + 16;
        full_cmd = malloc(len);
        snprintf(full_cmd, len, "%s 2>&1", cmd);
    }

    FILE *fp = popen(full_cmd, "r");
    free(full_cmd);
    if (!fp) return nc_tool_result_err(strerror(errno));

    NcBuf buf; nc_buf_init(&buf);
    char chunk[4096];
    while (fgets(chunk, sizeof chunk, fp)) {
        nc_buf_appendz(&buf, chunk);
        if (buf.len >= SHELL_OUTPUT_CAP) {
            nc_buf_appendz(&buf, "\n...[truncated]");
            break;
        }
    }
    int status = pclose(fp);
    if (status != 0 && (!buf.data || !buf.data[0])) {
        char em[64];
        snprintf(em, sizeof em, "Command exited with status %d", status);
        nc_buf_free(&buf);
        return nc_tool_result_err(em);
    }

    char *out = buf.data ? buf.data : nc_strdup("");
    /* Ownership: buf.data is malloc'd, tool_result_ok dupes it */
    NcToolResult r = nc_tool_result_ok(out);
    free(buf.data); buf.data = NULL;
    return r;
}

static const char *shell_name(void *ptr) { (void)ptr; return "shell"; }
static const char *shell_desc(void *ptr) {
    (void)ptr;
    return "Run a shell command in the workspace and return stdout+stderr.";
}
static const char *shell_params(void *ptr) {
    (void)ptr;
    return "{\"type\":\"object\","
           "\"properties\":{"
           "\"command\":{\"type\":\"string\",\"description\":\"Shell command to run\"},"
           "\"cwd\":{\"type\":\"string\",\"description\":\"Working directory (optional)\"}"
           "},"
           "\"required\":[\"command\"]}";
}
static void shell_deinit(void *ptr) { free(ptr); }

static const NcToolVTable SHELL_VTABLE = {
    shell_execute, shell_name, shell_desc, shell_params, shell_deinit
};

NcTool nc_tool_shell_init(const NcToolContext *ctx) {
    ShellTool *t = calloc(1, sizeof *t);
    if (ctx) {
        snprintf(t->workspace_dir, sizeof t->workspace_dir, "%s",
                 ctx->workspace_dir ? ctx->workspace_dir : ".");
        t->policy = NULL;  /* policy not in NcToolContext, set after init if needed */
    }
    return (NcTool){ .ptr = t, .vtable = &SHELL_VTABLE };
}
