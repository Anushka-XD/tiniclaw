/*
 * tiniclaw-c - Anthropic provider
 * POST https://api.anthropic.com/v1/messages
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>
#include "provider.h"
#include "util.h"

/* File-scope write callback - no nested functions (clang compatibility) */
static size_t anthropic_write_cb(void *ptr, size_t sz, size_t nmb, void *ud) {
    NcBuf *b = ud;
    nc_buf_append(b, (char *)ptr, sz * nmb);
    return sz * nmb;
}

/* Build request JSON */
static char *build_anthropic_request(NcArena *arena, const NcChatRequest *req) {
    (void)arena;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", req->model);
    cJSON_AddNumberToObject(root, "max_tokens", req->max_tokens ? req->max_tokens : 8192);

    cJSON *messages = cJSON_CreateArray();
    for (size_t i = 0; i < req->messages_count; i++) {
        if (req->messages[i].role == NC_ROLE_SYSTEM) {
            cJSON_AddStringToObject(root, "system",
                req->messages[i].content ? req->messages[i].content : "");
            continue;
        }
        cJSON *msg = cJSON_CreateObject();
        const char *role_str = req->messages[i].role == NC_ROLE_ASSISTANT ? "assistant" : "user";
        cJSON_AddStringToObject(msg, "role", role_str);
        cJSON_AddStringToObject(msg, "content",
            req->messages[i].content ? req->messages[i].content : "");
        cJSON_AddItemToArray(messages, msg);
    }
    cJSON_AddItemToObject(root, "messages", messages);

    if (req->tools_count > 0) {
        cJSON *tools = cJSON_CreateArray();
        for (size_t i = 0; i < req->tools_count; i++) {
            cJSON *t = cJSON_CreateObject();
            cJSON_AddStringToObject(t, "name", req->tools[i].name);
            cJSON_AddStringToObject(t, "description", req->tools[i].description);
            cJSON *schema = req->tools[i].parameters_json
                ? cJSON_Parse(req->tools[i].parameters_json) : cJSON_CreateObject();
            if (schema) cJSON_AddItemToObject(t, "input_schema", schema);
            cJSON_AddItemToArray(tools, t);
        }
        cJSON_AddItemToObject(root, "tools", tools);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

/* Parse response */
static int parse_anthropic_response(const char *body, NcChatResponse *out) {
    cJSON *root = cJSON_Parse(body);
    if (!root) { out->error_msg = nc_strdup("Parse error"); return -1; }

    cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (err) {
        cJSON *msg = cJSON_GetObjectItemCaseSensitive(err, "message");
        out->error_msg = nc_strdup(msg && cJSON_IsString(msg)
            ? msg->valuestring : "Anthropic API error");
        cJSON_Delete(root);
        return -1;
    }

    cJSON *content = cJSON_GetObjectItemCaseSensitive(root, "content");
    if (!content || !cJSON_IsArray(content)) {
        cJSON_Delete(root);
        out->error_msg = nc_strdup("No content array");
        return -1;
    }

    NcBuf text_buf;
    nc_buf_init(&text_buf);
    int tool_count = 0;
    cJSON *block;

    cJSON_ArrayForEach(block, content) {
        cJSON *type = cJSON_GetObjectItemCaseSensitive(block, "type");
        if (type && cJSON_IsString(type) && strcmp(type->valuestring, "tool_use") == 0)
            tool_count++;
    }
    if (tool_count > 0) {
        out->tool_calls = calloc((size_t)tool_count, sizeof(NcToolCall));
        out->tool_calls_count = (size_t)tool_count;
    }

    int ti = 0;
    cJSON_ArrayForEach(block, content) {
        cJSON *type = cJSON_GetObjectItemCaseSensitive(block, "type");
        if (!type || !cJSON_IsString(type)) continue;

        if (strcmp(type->valuestring, "text") == 0) {
            cJSON *text = cJSON_GetObjectItemCaseSensitive(block, "text");
            if (text && cJSON_IsString(text))
                nc_buf_appendz(&text_buf, text->valuestring);
        } else if (strcmp(type->valuestring, "tool_use") == 0 && ti < tool_count) {
            cJSON *id_j    = cJSON_GetObjectItemCaseSensitive(block, "id");
            cJSON *name_j  = cJSON_GetObjectItemCaseSensitive(block, "name");
            cJSON *input_j = cJSON_GetObjectItemCaseSensitive(block, "input");
            out->tool_calls[ti].id   = nc_strdup(
                id_j && cJSON_IsString(id_j) ? id_j->valuestring : "");
            out->tool_calls[ti].name = nc_strdup(
                name_j && cJSON_IsString(name_j) ? name_j->valuestring : "");
            char *args_str = input_j
                ? cJSON_PrintUnformatted(input_j) : nc_strdup("{}");
            out->tool_calls[ti].arguments = args_str;
            ti++;
        }
    }

    out->content = nc_buf_take(&text_buf);
    if (!out->content) out->content = nc_strdup("");

    cJSON *usage = cJSON_GetObjectItemCaseSensitive(root, "usage");
    if (usage) {
        cJSON *in_j  = cJSON_GetObjectItemCaseSensitive(usage, "input_tokens");
        cJSON *out_j = cJSON_GetObjectItemCaseSensitive(usage, "output_tokens");
        if (in_j)  out->usage.prompt_tokens     = (int)in_j->valuedouble;
        if (out_j) out->usage.completion_tokens = (int)out_j->valuedouble;
        out->usage.total_tokens =
            out->usage.prompt_tokens + out->usage.completion_tokens;
    }

    out->ok = true;
    cJSON_Delete(root);
    return 0;
}

/* VTable implementation */
static int anthropic_chat(void *self, NcArena *arena,
                           const NcChatRequest *req, NcChatResponse *out) {
    NcAnthropicProvider *p = (NcAnthropicProvider *)self;
    memset(out, 0, sizeof *out);

    const char *url = (p->base_url && p->base_url[0])
        ? p->base_url : "https://api.anthropic.com/v1/messages";

    char auth_hdr[600];
    snprintf(auth_hdr, sizeof auth_hdr, "x-api-key: %s",
             p->api_key ? p->api_key : "");

    char *body = build_anthropic_request(arena, req);
    CURL *curl = curl_easy_init();
    if (!curl) {
        free(body);
        out->error_msg = nc_strdup("curl init failed");
        return -1;
    }

    struct curl_slist *hdr = NULL;
    hdr = curl_slist_append(hdr, "Content-Type: application/json");
    hdr = curl_slist_append(hdr, auth_hdr);
    hdr = curl_slist_append(hdr, "anthropic-version: 2023-06-01");
    hdr = curl_slist_append(hdr, "anthropic-beta: tools-2024-04-04");

    NcBuf rb;
    nc_buf_init(&rb);
    long code = 0;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, anthropic_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &rb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdr);
    curl_easy_cleanup(curl);
    free(body);

    char *resp = nc_buf_take(&rb);
    if (!resp) {
        out->error_msg = nc_strdup("HTTP request failed");
        return -1;
    }
    int rc = parse_anthropic_response(resp, out);
    free(resp);
    return rc;
}

static bool anthropic_supports_tools(void *self) { (void)self; return true; }
static const char *anthropic_name(void *self) { (void)self; return "anthropic"; }
static void anthropic_deinit(void *self) { (void)self; }

static const NcProviderVTable ANTHROPIC_VTABLE = {
    .chat           = anthropic_chat,
    .stream_chat    = NULL,
    .supports_tools = anthropic_supports_tools,
    .get_name       = anthropic_name,
    .deinit         = anthropic_deinit,
};

NcProvider nc_anthropic_provider_init(NcArena *arena,
                                       const char *api_key,
                                       const char *base_url) {
    NcAnthropicProvider *p = NC_ZALLOC(arena, NcAnthropicProvider);
    if (!p) return (NcProvider){0};
    p->api_key  = api_key  ? nc_arena_strdup(arena, api_key)  : NULL;
    p->base_url = base_url ? nc_arena_strdup(arena, base_url) : NULL;
    return (NcProvider){ .ptr = p, .vtable = &ANTHROPIC_VTABLE };
}
