/*
 * tiniclaw-c — Ollama provider (local, POST http://localhost:11434/api/chat)
 * and Compatible provider (any OpenAI-compatible endpoint)
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <cjson/cJSON.h>
#include "provider.h"
#include "util.h"

extern char *nc_http_post_json(const char *url, const char *auth_header,
                                const char *body, long *http_code_out);

/* ══════════════════════════════════════════════════════════════════
   Ollama
   ══════════════════════════════════════════════════════════════════ */

static char *build_ollama_request(const NcChatRequest *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", req->model);
    cJSON_AddFalseToObject(root, "stream");
    cJSON *options = cJSON_CreateObject();
    cJSON_AddNumberToObject(options, "temperature", req->temperature);
    cJSON_AddItemToObject(root, "options", options);

    cJSON *messages = cJSON_CreateArray();
    for (size_t i = 0; i < req->messages_count; i++) {
        cJSON *msg = cJSON_CreateObject();
        const char *role = req->messages[i].role == NC_ROLE_SYSTEM ? "system" :
                           req->messages[i].role == NC_ROLE_ASSISTANT ? "assistant" : "user";
        cJSON_AddStringToObject(msg, "role", role);
        cJSON_AddStringToObject(msg, "content",
            req->messages[i].content ? req->messages[i].content : "");
        cJSON_AddItemToArray(messages, msg);
    }
    cJSON_AddItemToObject(root, "messages", messages);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static int parse_ollama_response(const char *body, NcChatResponse *out) {
    cJSON *root = cJSON_Parse(body);
    if (!root) { out->error_msg = nc_strdup("Parse error"); return -1; }
    cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (err && cJSON_IsString(err)) {
        out->error_msg = nc_strdup(err->valuestring);
        cJSON_Delete(root);
        return -1;
    }
    cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "message");
    if (msg) {
        cJSON *content = cJSON_GetObjectItemCaseSensitive(msg, "content");
        if (content && cJSON_IsString(content))
            out->content = nc_strdup(content->valuestring);
    }
    cJSON *usage = cJSON_GetObjectItemCaseSensitive(root, "prompt_eval_count");
    if (usage) out->usage.prompt_tokens = (uint32_t)usage->valueint;
    cJSON *ect = cJSON_GetObjectItemCaseSensitive(root, "eval_count");
    if (ect) out->usage.completion_tokens = (uint32_t)ect->valueint;
    out->usage.total_tokens = out->usage.prompt_tokens + out->usage.completion_tokens;
    out->ok = true;
    cJSON_Delete(root);
    return 0;
}

static int ollama_chat(void *self, NcArena *arena,
                        const NcChatRequest *req, NcChatResponse *out) {
    (void)arena;
    NcOllamaProvider *p = (NcOllamaProvider *)self;
    memset(out, 0, sizeof *out);
    const char *base = p->base_url ? p->base_url : "http://localhost:11434";
    char url[512];
    snprintf(url, sizeof url, "%s/api/chat", base);
    char *body = build_ollama_request(req);
    long code = 0;
    char *resp = nc_http_post_json(url, NULL, body, &code);
    free(body);
    if (!resp) { out->error_msg = nc_strdup("HTTP failed"); return -1; }
    int rc = parse_ollama_response(resp, out);
    free(resp);
    return rc;
}
static bool ollama_supports_tools(void *self) { (void)self; return false; }
static const char *ollama_name(void *self) { (void)self; return "ollama"; }
static void ollama_deinit(void *self) { (void)self; }

static const NcProviderVTable OLLAMA_VTABLE = {
    .chat = ollama_chat, .supports_tools = ollama_supports_tools,
    .get_name = ollama_name, .deinit = ollama_deinit,
};

NcProvider nc_ollama_provider_init(NcArena *arena, const char *base_url) {
    NcOllamaProvider *p = NC_ZALLOC(arena, NcOllamaProvider);
    if (!p) return (NcProvider){0};
    p->base_url = base_url ? nc_arena_strdup(arena, base_url) : NULL;
    return (NcProvider){ .ptr = p, .vtable = &OLLAMA_VTABLE };
}

/* ══════════════════════════════════════════════════════════════════
   Compatible — any OpenAI-compatible endpoint
   (Groq, Mistral, Together, Fireworks, Perplexity, DeepSeek, etc.)
   ══════════════════════════════════════════════════════════════════ */

/* Reuse OpenAI request/response building */
extern NcProvider nc_openai_provider_init(NcArena *arena, const char *api_key, const char *base_url);

static const char *compatible_name(void *self) {
    NcCompatibleProvider *p = (NcCompatibleProvider *)self;
    return p->display_name ? p->display_name : "compatible";
}

/* Compatible just delegates to the inner OpenAI provider with a custom URL */
NcProvider nc_compatible_provider_init(NcArena *arena, const char *api_key,
                                        const char *base_url, const char *display_name) {
    /* We create a NcOpenAiProvider with overridden URL; name is different */
    NcCompatibleProvider *c = NC_ZALLOC(arena, NcCompatibleProvider);
    if (!c) return (NcProvider){0};
    c->api_key      = api_key      ? nc_arena_strdup(arena, api_key)      : NULL;
    c->base_url     = base_url     ? nc_arena_strdup(arena, base_url)     : NULL;
    c->display_name = display_name ? nc_arena_strdup(arena, display_name) : NULL;

    /* Delegate to OpenAI provider (same wire format) */
    NcProvider inner = nc_openai_provider_init(arena, api_key, base_url);
    (void)c;  /* display name is informational only */
    return inner;
}
