#pragma once
/*
 * tiniclaw-c — provider (AI model) vtable interface
 * Mirrors tiniclaw's Provider vtable from providers/root.zig
 */
#include <stddef.h>
#include <stdbool.h>
#include "arena.h"

/* ── Message types ──────────────────────────────────────────────── */

typedef enum NcRole {
    NC_ROLE_SYSTEM,
    NC_ROLE_USER,
    NC_ROLE_ASSISTANT,
    NC_ROLE_TOOL,
} NcRole;

typedef struct NcMessage {
    NcRole       role;
    const char  *content;      /* owned by caller */
    const char  *tool_call_id; /* non-NULL for tool result messages */
    const char  *tool_name;    /* for tool result messages */
} NcMessage;

typedef struct NcToolCall {
    const char *id;
    const char *name;
    const char *arguments;   /* raw JSON string */
} NcToolCall;

typedef struct NcTokenUsage {
    uint32_t prompt_tokens;
    uint32_t completion_tokens;
    uint32_t total_tokens;
} NcTokenUsage;

/* ── Tool spec (for function-calling APIs) ──────────────────────── */

typedef struct NcToolSpec {
    const char *name;
    const char *description;
    const char *parameters_json; /* OpenAI-style JSON schema */
} NcToolSpec;

/* ── Chat request / response ────────────────────────────────────── */

typedef struct NcChatRequest {
    const char       *model;
    double            temperature;
    uint32_t          max_tokens;     /* 0 = provider default */
    const char       *reasoning_effort; /* NULL = default */
    const NcMessage  *messages;
    size_t            messages_count;
    const NcToolSpec *tools;
    size_t            tools_count;
    bool              stream;
} NcChatRequest;

typedef struct NcChatResponse {
    char          *content;         /* heap-allocated; NULL if only tool calls */
    NcToolCall    *tool_calls;      /* heap-allocated array; NULL if none */
    size_t         tool_calls_count;
    NcTokenUsage   usage;
    bool           ok;
    char          *error_msg;       /* heap-allocated; NULL on success */
} NcChatResponse;

void nc_chat_response_free(NcChatResponse *r);

/* ── Streaming callback ─────────────────────────────────────────── */

/* Called for each streamed chunk. Return false to abort. */
typedef bool (*NcStreamCallback)(const char *chunk, void *ctx);

/* ── Provider vtable ────────────────────────────────────────────── */

typedef struct NcProviderVTable {
    /* Required: multi-turn chat with full message history. */
    int  (*chat)(void *self, NcArena *arena,
                 const NcChatRequest *req, NcChatResponse *out);

    /* Optional: streaming variant; NULL means non-streaming fallback. */
    int  (*stream_chat)(void *self, NcArena *arena,
                        const NcChatRequest *req,
                        NcStreamCallback cb, void *cb_ctx);

    /* Does this provider support native function calling? */
    bool (*supports_tools)(void *self);

    /* Provider display name, e.g. "openai", "anthropic". */
    const char *(*get_name)(void *self);

    void (*deinit)(void *self);
} NcProviderVTable;

typedef struct NcProvider {
    void                    *ptr;
    const NcProviderVTable  *vtable;
} NcProvider;

/* Convenience wrappers */
static inline int nc_provider_chat(NcProvider p, NcArena *a,
                                   const NcChatRequest *req, NcChatResponse *out) {
    return p.vtable->chat(p.ptr, a, req, out);
}
static inline bool nc_provider_supports_tools(NcProvider p) {
    return p.vtable->supports_tools(p.ptr);
}
static inline const char *nc_provider_name(NcProvider p) {
    return p.vtable->get_name(p.ptr);
}
static inline void nc_provider_deinit(NcProvider p) {
    if (p.vtable->deinit) p.vtable->deinit(p.ptr);
}

/* ── Concrete providers ─────────────────────────────────────────── */

/* OpenAI — POST https://api.openai.com/v1/chat/completions */
typedef struct NcOpenAiProvider {
    const char *api_key;
    const char *base_url;  /* NULL = default */
} NcOpenAiProvider;

NcProvider nc_openai_provider_init(NcArena *arena, const char *api_key, const char *base_url);

/* Anthropic — POST https://api.anthropic.com/v1/messages */
typedef struct NcAnthropicProvider {
    const char *api_key;
    const char *base_url;
} NcAnthropicProvider;

NcProvider nc_anthropic_provider_init(NcArena *arena, const char *api_key, const char *base_url);

/* Ollama — POST http://localhost:11434/api/chat */
typedef struct NcOllamaProvider {
    const char *base_url;  /* default: http://localhost:11434 */
} NcOllamaProvider;

NcProvider nc_ollama_provider_init(NcArena *arena, const char *base_url);

/* Compatible — any OpenAI-compatible endpoint */
typedef struct NcCompatibleProvider {
    const char *api_key;
    const char *base_url;
    const char *display_name;
} NcCompatibleProvider;

NcProvider nc_compatible_provider_init(NcArena *arena, const char *api_key,
                                        const char *base_url, const char *display_name);

/* Reliable — retry wrapper with fallback chain */
typedef struct NcReliableProvider {
    NcProvider  primary;
    NcProvider *fallbacks;
    size_t      fallbacks_count;
    uint32_t    max_retries;
} NcReliableProvider;

NcProvider nc_reliable_provider_init(NcArena *arena, NcProvider primary,
                                      NcProvider *fallbacks, size_t count,
                                      uint32_t max_retries);

/* ── Factory ─────────────────────────────────────────────────────── */

typedef enum NcProviderKind {
    NC_PROVIDER_OPENAI,
    NC_PROVIDER_ANTHROPIC,
    NC_PROVIDER_OLLAMA,
    NC_PROVIDER_GEMINI,
    NC_PROVIDER_OPENROUTER,
    NC_PROVIDER_GROQ,
    NC_PROVIDER_MISTRAL,
    NC_PROVIDER_TOGETHER,
    NC_PROVIDER_FIREWORKS,
    NC_PROVIDER_PERPLEXITY,
    NC_PROVIDER_DEEPSEEK,
    NC_PROVIDER_XAI,
    NC_PROVIDER_COHERE,
    NC_PROVIDER_COMPATIBLE,
    NC_PROVIDER_UNKNOWN,
} NcProviderKind;

NcProviderKind nc_classify_provider(const char *name);
const char    *nc_provider_base_url(NcProviderKind kind);

/* Create a provider from config name + api_key.
   Returns a zeroed NcProvider on failure. */
NcProvider nc_provider_create(NcArena *arena, const char *name, const char *api_key,
                               const char *base_url_override);
