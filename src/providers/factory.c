/*
 * tiniclaw-c — provider factory
 * Creates the right provider from a name + API key.
 */
#include <string.h>
#include <stdlib.h>
#include "provider.h"
#include "util.h"

NcProviderKind nc_classify_provider(const char *name) {
    if (!name) return NC_PROVIDER_UNKNOWN;
    if (strcmp(name, "openai") == 0)      return NC_PROVIDER_OPENAI;
    if (strcmp(name, "anthropic") == 0)   return NC_PROVIDER_ANTHROPIC;
    if (strcmp(name, "ollama") == 0)      return NC_PROVIDER_OLLAMA;
    if (strcmp(name, "gemini") == 0)      return NC_PROVIDER_GEMINI;
    if (strcmp(name, "openrouter") == 0)  return NC_PROVIDER_OPENROUTER;
    if (strcmp(name, "groq") == 0)        return NC_PROVIDER_GROQ;
    if (strcmp(name, "mistral") == 0)     return NC_PROVIDER_MISTRAL;
    if (strcmp(name, "together") == 0)    return NC_PROVIDER_TOGETHER;
    if (strcmp(name, "fireworks") == 0)   return NC_PROVIDER_FIREWORKS;
    if (strcmp(name, "perplexity") == 0)  return NC_PROVIDER_PERPLEXITY;
    if (strcmp(name, "deepseek") == 0)    return NC_PROVIDER_DEEPSEEK;
    if (strcmp(name, "xai") == 0)         return NC_PROVIDER_XAI;
    if (strcmp(name, "cohere") == 0)      return NC_PROVIDER_COHERE;
    /* custom: prefix */
    if (strncmp(name, "custom:", 7) == 0) return NC_PROVIDER_COMPATIBLE;
    return NC_PROVIDER_UNKNOWN;
}

const char *nc_provider_base_url(NcProviderKind kind) {
    switch (kind) {
        case NC_PROVIDER_OPENAI:      return "https://api.openai.com/v1/chat/completions";
        case NC_PROVIDER_ANTHROPIC:   return "https://api.anthropic.com/v1/messages";
        case NC_PROVIDER_OLLAMA:      return "http://localhost:11434";
        case NC_PROVIDER_GEMINI:      return "https://generativelanguage.googleapis.com/v1beta/openai/chat/completions";
        case NC_PROVIDER_OPENROUTER:  return "https://openrouter.ai/api/v1/chat/completions";
        case NC_PROVIDER_GROQ:        return "https://api.groq.com/openai/v1/chat/completions";
        case NC_PROVIDER_MISTRAL:     return "https://api.mistral.ai/v1/chat/completions";
        case NC_PROVIDER_TOGETHER:    return "https://api.together.xyz/v1/chat/completions";
        case NC_PROVIDER_FIREWORKS:   return "https://api.fireworks.ai/inference/v1/chat/completions";
        case NC_PROVIDER_PERPLEXITY:  return "https://api.perplexity.ai/chat/completions";
        case NC_PROVIDER_DEEPSEEK:    return "https://api.deepseek.com/v1/chat/completions";
        case NC_PROVIDER_XAI:         return "https://api.x.ai/v1/chat/completions";
        case NC_PROVIDER_COHERE:      return "https://api.cohere.ai/compatibility/v1/chat/completions";
        default:                      return NULL;
    }
}

NcProvider nc_provider_create(NcArena *arena, const char *name,
                               const char *api_key, const char *base_url_override) {
    NcProviderKind kind = nc_classify_provider(name);
    const char *url = base_url_override ? base_url_override : nc_provider_base_url(kind);

    switch (kind) {
        case NC_PROVIDER_OPENAI:
            return nc_openai_provider_init(arena, api_key, url);
        case NC_PROVIDER_ANTHROPIC:
            return nc_anthropic_provider_init(arena, api_key, url);
        case NC_PROVIDER_OLLAMA:
            return nc_ollama_provider_init(arena, url);
        default:
            /* All other providers are OpenAI-compatible */
            return nc_openai_provider_init(arena, api_key, url);
    }
}
