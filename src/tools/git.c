/*
 * tiniclaw-c — git tool (thin wrapper around git CLI)
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <cjson/cJSON.h>
#include "tool.h"
#include "arena.h"
#include "util.h"

typedef struct { char workspace_dir[4096]; } GitTool;

static NcToolResult git_execute(void *ptr, NcArena *arena, cJSON *args) {
    GitTool *t = ptr;
    cJSON *sub_j = cJSON_GetObjectItemCaseSensitive(args, "subcommand");
    if (!sub_j || !cJSON_IsString(sub_j)) return nc_tool_result_err("missing 'subcommand'");
    const char *sub = sub_j->valuestring;

    /* Allow-list of safe git subcommands */
    static const char *SAFE[] = {
        "status","log","diff","add","commit","push","pull","fetch","branch",
        "checkout","show","stash","tag","remote","describe","shortlog",
        "blame","grep","ls-files","rev-parse","config","init","clone",NULL
    };
    bool allowed = false;
    for (int i = 0; SAFE[i]; i++) { if (strcmp(sub, SAFE[i]) == 0) { allowed = true; break; } }
    if (!allowed) return nc_tool_result_err("git subcommand not in allowlist");

    /* Build: git <subcommand> [args...] */
    NcBuf cmd; nc_buf_init(&cmd);
    nc_buf_appendf(&cmd, "cd '%s' && git %s", t->workspace_dir, sub);

    cJSON *git_args_j = cJSON_GetObjectItemCaseSensitive(args, "args");
    if (git_args_j && cJSON_IsArray(git_args_j)) {
        cJSON *a = NULL;
        cJSON_ArrayForEach(a, git_args_j) {
            if (cJSON_IsString(a)) {
                nc_buf_appendf(&cmd, " '%s'", a->valuestring);
            }
        }
    }
    nc_buf_appendz(&cmd, " 2>&1");

    FILE *fp = popen(cmd.data, "r");
    nc_buf_free(&cmd);
    if (!fp) return nc_tool_result_err("popen failed");

    NcBuf out; nc_buf_init(&out);
    char chunk[4096];
    while (fgets(chunk, sizeof chunk, fp)) nc_buf_appendz(&out, chunk);
    pclose(fp);

    NcToolResult r = nc_tool_result_ok(out.data ? out.data : "");
    nc_buf_free(&out);
    return r;
}

static const char *git_name(void *p) { (void)p; return "git"; }
static const char *git_desc(void *p) {
    (void)p; return "Run git commands in the workspace repository.";
}
static const char *git_params(void *p) {
    (void)p;
    return "{\"type\":\"object\","
           "\"properties\":{"
           "\"subcommand\":{\"type\":\"string\",\"description\":\"e.g. status, log, diff, commit\"},"
           "\"args\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Extra arguments\"}"
           "},"
           "\"required\":[\"subcommand\"]}";
}
static void git_deinit(void *p) { free(p); }

static const NcToolVTable GIT_VTABLE = { git_execute, git_name, git_desc, git_params, git_deinit };

NcTool nc_tool_git_init(const NcToolContext *ctx) {
    GitTool *t = calloc(1, sizeof *t);
    if (ctx) snprintf(t->workspace_dir, sizeof t->workspace_dir, "%s", ctx->workspace_dir);
    return (NcTool){ .ptr = t, .vtable = &GIT_VTABLE };
}
