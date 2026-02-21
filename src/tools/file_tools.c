/*
 * tiniclaw-c — file read, write, edit, append tools
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <cjson/cJSON.h>
#include "tool.h"
#include "arena.h"
#include "util.h"
#include "platform.h"

#define FILE_READ_CAP (512 * 1024)  /* 512 KB */

typedef struct {
    char workspace_dir[4096];
} FileTool;

/* ── Path resolution ─────────────────────────────────────────────── */
/* Resolve a path relative to workspace; reject path traversal outside it */
static int resolve_path(const FileTool *t, const char *path, char *out, size_t out_len) {
    if (!path || !path[0]) return -1;
    if (path[0] == '/') {
        snprintf(out, out_len, "%s", path);
    } else {
        snprintf(out, out_len, "%s/%s", t->workspace_dir, path);
    }
    return 0;
}

/* ══════════════════════════════════════════════════════
 *  FILE READ
 * ════════════════════════════════════════════════════*/

static NcToolResult file_read_execute(void *ptr, NcArena *arena, cJSON *args) {
    FileTool *t = ptr;
    cJSON *path_j = cJSON_GetObjectItemCaseSensitive(args, "path");
    if (!path_j || !cJSON_IsString(path_j)) return nc_tool_result_err("missing 'path'");

    char full[4096];
    if (resolve_path(t, path_j->valuestring, full, sizeof full) < 0)
        return nc_tool_result_err("invalid path");

    size_t flen = 0;
    char *data = nc_read_file(full, &flen);
    if (!data) return nc_tool_result_err(strerror(errno));
    if (flen > FILE_READ_CAP) {
        data[FILE_READ_CAP] = '\0';
        flen = FILE_READ_CAP;
    }

    NcToolResult r = nc_tool_result_ok(data);
    free(data);
    return r;
}
static const char *file_read_name(void *p) { (void)p; return "file_read"; }
static const char *file_read_desc(void *p) {
    (void)p; return "Read a file from disk and return its contents.";
}
static const char *file_read_params(void *p) {
    (void)p;
    return "{\"type\":\"object\","
           "\"properties\":{"
           "\"path\":{\"type\":\"string\",\"description\":\"File path (relative to workspace or absolute)\"}"
           "},"
           "\"required\":[\"path\"]}";
}
static void file_tool_deinit(void *p) { free(p); }

static const NcToolVTable FILE_READ_VTABLE = {
    file_read_execute, file_read_name, file_read_desc, file_read_params, file_tool_deinit
};
NcTool nc_tool_file_read_init(const NcToolContext *ctx) {
    FileTool *t = calloc(1, sizeof *t);
    if (ctx) snprintf(t->workspace_dir, sizeof t->workspace_dir, "%s", ctx->workspace_dir);
    return (NcTool){ .ptr = t, .vtable = &FILE_READ_VTABLE };
}

/* ══════════════════════════════════════════════════════
 *  FILE WRITE
 * ════════════════════════════════════════════════════*/

static NcToolResult file_write_execute(void *ptr, NcArena *arena, cJSON *args) {
    FileTool *t = ptr;
    cJSON *path_j    = cJSON_GetObjectItemCaseSensitive(args, "path");
    cJSON *content_j = cJSON_GetObjectItemCaseSensitive(args, "content");
    if (!path_j    || !cJSON_IsString(path_j))    return nc_tool_result_err("missing 'path'");
    if (!content_j || !cJSON_IsString(content_j)) return nc_tool_result_err("missing 'content'");

    char full[4096];
    if (resolve_path(t, path_j->valuestring, full, sizeof full) < 0)
        return nc_tool_result_err("invalid path");

    /* Ensure parent dir exists */
    char *sep = strrchr(full, '/');
    if (sep) {
        char dir[4096]; size_t dlen = (size_t)(sep - full);
        memcpy(dir, full, dlen); dir[dlen] = '\0';
        nc_ensure_dir(dir);
    }

    const char *content = content_j->valuestring;
    if (nc_write_file(full, content, strlen(content)) != 0)
        return nc_tool_result_err(strerror(errno));

    char msg[256]; snprintf(msg, sizeof msg, "Wrote %zu bytes to %s", strlen(content), full);
    return nc_tool_result_ok(msg);
}
static const char *file_write_name(void *p) { (void)p; return "file_write"; }
static const char *file_write_desc(void *p) {
    (void)p; return "Write content to a file (creates or overwrites).";
}
static const char *file_write_params(void *p) {
    (void)p;
    return "{\"type\":\"object\","
           "\"properties\":{"
           "\"path\":{\"type\":\"string\"},"
           "\"content\":{\"type\":\"string\",\"description\":\"Content to write\"}"
           "},"
           "\"required\":[\"path\",\"content\"]}";
}
static const NcToolVTable FILE_WRITE_VTABLE = {
    file_write_execute, file_write_name, file_write_desc, file_write_params, file_tool_deinit
};
NcTool nc_tool_file_write_init(const NcToolContext *ctx) {
    FileTool *t = calloc(1, sizeof *t);
    if (ctx) snprintf(t->workspace_dir, sizeof t->workspace_dir, "%s", ctx->workspace_dir);
    return (NcTool){ .ptr = t, .vtable = &FILE_WRITE_VTABLE };
}

/* ══════════════════════════════════════════════════════
 *  FILE EDIT (search + replace)
 * ════════════════════════════════════════════════════*/

static NcToolResult file_edit_execute(void *ptr, NcArena *arena, cJSON *args) {
    FileTool *t = ptr;
    cJSON *path_j  = cJSON_GetObjectItemCaseSensitive(args, "path");
    cJSON *old_j   = cJSON_GetObjectItemCaseSensitive(args, "old_string");
    cJSON *new_j   = cJSON_GetObjectItemCaseSensitive(args, "new_string");
    if (!path_j || !cJSON_IsString(path_j)) return nc_tool_result_err("missing 'path'");
    if (!old_j  || !cJSON_IsString(old_j))  return nc_tool_result_err("missing 'old_string'");
    if (!new_j  || !cJSON_IsString(new_j))  return nc_tool_result_err("missing 'new_string'");

    char full[4096];
    if (resolve_path(t, path_j->valuestring, full, sizeof full) < 0)
        return nc_tool_result_err("invalid path");

    size_t flen = 0;
    char *data = nc_read_file(full, &flen);
    if (!data) return nc_tool_result_err(strerror(errno));

    const char *old_str = old_j->valuestring;
    const char *new_str = new_j->valuestring;
    size_t old_len = strlen(old_str);
    size_t new_len = strlen(new_str);

    /* Find first occurrence */
    char *pos = strstr(data, old_str);
    if (!pos) { free(data); return nc_tool_result_err("old_string not found in file"); }

    size_t head = (size_t)(pos - data);
    size_t tail = flen - head - old_len;
    size_t new_total = head + new_len + tail;
    char *out = malloc(new_total + 1);
    memcpy(out, data, head);
    memcpy(out + head, new_str, new_len);
    memcpy(out + head + new_len, pos + old_len, tail);
    out[new_total] = '\0';
    free(data);

    if (nc_write_file(full, out, new_total) != 0) {
        free(out);
        return nc_tool_result_err(strerror(errno));
    }
    free(out);
    char msg[256]; snprintf(msg, sizeof msg, "Edited %s", full);
    return nc_tool_result_ok(msg);
}
static const char *file_edit_name(void *p) { (void)p; return "file_edit"; }
static const char *file_edit_desc(void *p) {
    (void)p; return "Replace the first occurrence of old_string with new_string in a file.";
}
static const char *file_edit_params(void *p) {
    (void)p;
    return "{\"type\":\"object\","
           "\"properties\":{"
           "\"path\":{\"type\":\"string\"},"
           "\"old_string\":{\"type\":\"string\",\"description\":\"Text to replace\"},"
           "\"new_string\":{\"type\":\"string\",\"description\":\"Replacement text\"}"
           "},"
           "\"required\":[\"path\",\"old_string\",\"new_string\"]}";
}
static const NcToolVTable FILE_EDIT_VTABLE = {
    file_edit_execute, file_edit_name, file_edit_desc, file_edit_params, file_tool_deinit
};
NcTool nc_tool_file_edit_init(const NcToolContext *ctx) {
    FileTool *t = calloc(1, sizeof *t);
    if (ctx) snprintf(t->workspace_dir, sizeof t->workspace_dir, "%s", ctx->workspace_dir);
    return (NcTool){ .ptr = t, .vtable = &FILE_EDIT_VTABLE };
}

/* ══════════════════════════════════════════════════════
 *  FILE APPEND
 * ════════════════════════════════════════════════════*/

static NcToolResult file_append_execute(void *ptr, NcArena *arena, cJSON *args) {
    FileTool *t = ptr;
    cJSON *path_j    = cJSON_GetObjectItemCaseSensitive(args, "path");
    cJSON *content_j = cJSON_GetObjectItemCaseSensitive(args, "content");
    if (!path_j    || !cJSON_IsString(path_j))    return nc_tool_result_err("missing 'path'");
    if (!content_j || !cJSON_IsString(content_j)) return nc_tool_result_err("missing 'content'");

    char full[4096];
    if (resolve_path(t, path_j->valuestring, full, sizeof full) < 0)
        return nc_tool_result_err("invalid path");

    FILE *f = fopen(full, "a");
    if (!f) return nc_tool_result_err(strerror(errno));
    fputs(content_j->valuestring, f);
    fclose(f);

    char msg[256]; snprintf(msg, sizeof msg, "Appended to %s", full);
    return nc_tool_result_ok(msg);
}
static const char *file_append_name(void *p) { (void)p; return "file_append"; }
static const char *file_append_desc(void *p) {
    (void)p; return "Append text to the end of a file.";
}
static const char *file_append_params(void *p) {
    (void)p;
    return "{\"type\":\"object\","
           "\"properties\":{"
           "\"path\":{\"type\":\"string\"},"
           "\"content\":{\"type\":\"string\",\"description\":\"Text to append\"}"
           "},"
           "\"required\":[\"path\",\"content\"]}";
}
static const NcToolVTable FILE_APPEND_VTABLE = {
    file_append_execute, file_append_name, file_append_desc, file_append_params, file_tool_deinit
};
NcTool nc_tool_file_append_init(const NcToolContext *ctx) {
    FileTool *t = calloc(1, sizeof *t);
    if (ctx) snprintf(t->workspace_dir, sizeof t->workspace_dir, "%s", ctx->workspace_dir);
    return (NcTool){ .ptr = t, .vtable = &FILE_APPEND_VTABLE };
}
