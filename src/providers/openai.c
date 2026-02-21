/*
 * tiniclaw-c — OpenAI provider
 * POST https://api.openai.com/v1/chat/completions
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <cjson/cJSON.h>
#include "provider.h"
#include "util.h"

extern char *nc_http_post_json(const char *url, const char *auth_header,
                                const char *body, long *http_code_out);

/* ── Build request JSON ─────────────────────────────────────────── */

static char *build_request(NcArena *arena, const NcChatRequest *req) {
    (void)arena;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", req->model);
    cJSON_AddNumberToObject(root, "temperature", req->temperature);
    if (req->max_tokens) cJSON_AddNumberToObject(root, "max_tokens", req->max_tokens);

    cJSON *messages = cJSON_CreateArray();
    for (size_t i = 0; i < req->messages_count; i++) {
        cJSON *msg = cJSON_CreateObject();
        const char *role_str;
        switch (req->messages[i].role) {
            case NC_ROLE_SYSTEM:    role_str = "system"; break;
            case NC_ROLE_ASSISTANT: role_str = "assistant"; break;
            case NC_ROLE_TOOL:      role_str = "tool"; break;
            default:                role_str = "user"; break;
        }
        cJSON_AddStringToObject(msg, "role", role_str);
        cJSON_AddStringToObject(msg, "content", req->messages[i].content ? req->messages[i].content : "");
        if (req->messages[i].role == NC_ROLE_TOOL && req->messages[i].tool_call_id)
            cJSON_AddStringToObject(msg, "tool_call_id", req->messages[i].tool_call_id);
        cJSON_AddItemToArray(messages, msg);
    }
    cJSON_AddItemToObject(root, "messages", messages);

    if (req->tools_count > 0) {
        cJSON *tools = cJSON_CreateArray();
        for (size_t i = 0; i < req->tools_count; i++) {
            cJSON *t = cJSON_CreateObject();
            cJSON_AddStringToObject(t, "type", "function");
            cJSON *fn = cJSON_CreateObject();
            cJSON_AddStringToObject(fn, "name", req->tools[i].name);
            cJSON_AddStringToObject(fn, "description", req->tools[i].description);
            cJSON *params = cJSON_Parse(req->tools[i].parameters_json);
            if (params) cJSON_AddItemToObject(fn, "parameters", params);
            cJSON_AddItemToObject(t, "function", fn);
            cJSON_AddItemToArray(tools, t);
        }
        cJSON_AddItemToObject(root, "tools", tools);
        cJSON_AddStringToObject(root, "tool_choice", "auto");
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;   /* heap-allocated; caller frees */
}

/* ── Parse response ─────────────────────────────────────────────── */

static int parse_response(const char *body, NcChatResponse *out) {
    cJSON *root = cJSON_Parse(body);
    if (!root) { out->error_msg = nc_strdup("Failed to parse response JSON"); return -1; }

    cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (error) {
        cJSON *msg = cJSON_GetObjectItemCaseSensitive(error, "message");
        out->error_msg = nc_strdup(msg && cJSON_IsString(msg) ? msg->valuestring : "API error");
        cJSON_Delete(root);
        return -1;
    }

    cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    if (!choices || !cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        out->error_msg = nc_strdup("No choices in response");
        cJSON_Delete(root);
        return -1;
    }

    cJSON *choice = cJSON_GetArrayItem(choices, 0);
    cJSON *message = cJSON_GetObjectItemCaseSensitive(choice, "message");
    if (!message) { cJSON_Delete(root); return -1; }

    cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
    if (content && cJSON_IsString(content))
        out->content = nc_strdup(content->valuestring);

    cJSON *tool_calls = cJSON_GetObjectItemCaseSensitive(message, "tool_calls");
    if (tool_calls && cJSON_IsArray(tool_calls)) {
        int count = cJSON_GetArraySize(tool_calls);
        out->tool_calls = calloc((size_t)count, sizeof(NcToolCall));
        out->tool_calls_count = (size_t)count;
        for (int i = 0; i < count; i++) {
            cJSON *tc = cJSON_GetArrayItem(tool_calls, i);
            cJSON *id = cJSON_GetObjectItemCaseSensitive(tc, "id");
            cJSON *fn = cJSON_GetObjectItemCaseSensitive(tc, "function");
            out->tool_calls[i].id   = nc_strdup(id && cJSON_IsString(id) ? id->valuestring : "");
            if (fn) {
                cJSON *nm  = cJSON_GetObjectItemCaseSensitive(fn, "name");
                cJSON *arg = cJSON_GetObjectItemCaseSensitive(fn, "arguments");
                out->tool_calls[i].name      = nc_strdup(nm  && cJSON_IsString(nm)  ? nm->valuestring  : "");
                out->tool_calls[i].arguments = nc_strdup(arg && cJSON_IsString(arg) ? arg->valuestring : "{}");
            }
        }
    }

    /* Token usage */
    cJSON *usage = cJSON_GetObjectItemCaseSensitive(root, "usage");
    if (usage) {
        cJSON *pt = cJSON_GetObjectItemCaseSensitive(usage, "prompt_tokens");
        cJSON *ct = cJSON_GetObjectItemCaseSensitive(usage, "completion_tokens");
        cJSON *tt = cJSON_GetObjectItemCaseSensitive(usage, "total_tokens");
        if (pt) out->usage.prompt_tokens     = (uint32_t)pt->valueint;
        if (ct) out->usage.completion_tokens = (uint32_t)ct->valueint;
        if (tt) out->usage.total_tokens      = (uint32_t)tt->valueint;
    }

    out->ok = true;
    cJSON_Delete(root);
    return 0;
}

/* ── VTable implementation ──────────────────────────────────────── */

static int openai_chat(void *self, NcArena *arena,
                        const NcChatRequest *req, NcChatResponse *out) {
    NcOpenAiProvider *p = (NcOpenAiProvider *)self;
    memset(out, 0, sizeof *out);

    char auth[600];
    snprintf(auth, sizeof auth, "Authorization: Bearer %s",
             p->api_key ? p->api_key : "");

    const char *url = p->base_url ? p->base_url : "https://api.openai.com/v1/chat/completions";
    char *body = build_request(arena, req);
    long code = 0;
    char *resp = nc_http_post_json(url, auth, body, &code);
    free(body);

    if (!resp) { out->error_msg = nc_strdup("HTTP request failed"); return -1; }
    int rc = parse_response(resp, out);
    free(resp);
    return rc;
}

static bool openai_supports_tools(void *self) { (void)self; return true; }
static const char *openai_name(void *self) { (void)self; return "openai"; }
static void openai_deinit(void *self) { (void)self; }   /* arena-owned */

static const NcProviderVTable OPENAI_VTABLE = {
    .chat           = openai_chat,
    .stream_chat    = NULL,
    .supports_tools = openai_supports_tools,
    .get_name       = openai_name,
    .deinit         = openai_deinit,
};

NcProvider nc_openai_provider_init(NcArena *arena, const char *api_key, const char *base_url) {
    NcOpenAiProvider *p = NC_ZALLOC(arena, NcOpenAiProvider);
    if (!p) return (NcProvider){0};
    p->api_key  = api_key  ? nc_arena_strdup(arena, api_key)  : NULL;
    p->base_url = base_url ? nc_arena_strdup(arena, base_url) : NULL;
    return (NcProvider){ .ptr = p, .vtable = &OPENAI_VTABLE };
}
