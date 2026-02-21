/*
 * tiniclaw-c — config loading from ~/.tiniclaw/config.json
 * Uses cJSON for parsing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <cjson/cJSON.h>
#include "config.h"
#include "platform.h"

/* ── Helpers ────────────────────────────────────────────────────── */

static void safe_str(char *dst, size_t dst_sz, const cJSON *obj, const char *key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v && cJSON_IsString(v) && v->valuestring)
        snprintf(dst, dst_sz, "%s", v->valuestring);
}

static void safe_bool(bool *dst, const cJSON *obj, const char *key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v && cJSON_IsBool(v)) *dst = cJSON_IsTrue(v);
}

static void safe_uint32(uint32_t *dst, const cJSON *obj, const char *key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v && cJSON_IsNumber(v)) *dst = (uint32_t)v->valueint;
}

static void safe_uint16(uint16_t *dst, const cJSON *obj, const char *key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v && cJSON_IsNumber(v)) *dst = (uint16_t)v->valueint;
}

static void safe_double(double *dst, const cJSON *obj, const char *key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v && cJSON_IsNumber(v)) *dst = v->valuedouble;
}

/* ── Defaults ────────────────────────────────────────────────────── */

void nc_config_apply_defaults(NcConfig *cfg) {
    if (!cfg->default_provider[0])    snprintf(cfg->default_provider, sizeof cfg->default_provider, "openrouter");
    if (!cfg->default_model[0])       snprintf(cfg->default_model, sizeof cfg->default_model, "anthropic/claude-sonnet-4");
    if (cfg->default_temperature == 0.0) cfg->default_temperature = 0.7;

    if (!cfg->gateway.host[0])        snprintf(cfg->gateway.host, sizeof cfg->gateway.host, "127.0.0.1");
    if (!cfg->gateway.port)           cfg->gateway.port = 3000;
    if (!cfg->gateway.body_size_limit) cfg->gateway.body_size_limit = 65536;
    if (!cfg->gateway.request_timeout_secs) cfg->gateway.request_timeout_secs = 30;
    if (!cfg->gateway.rate_limit_per_minute) cfg->gateway.rate_limit_per_minute = 60;

    if (!cfg->memory.backend[0])      snprintf(cfg->memory.backend, sizeof cfg->memory.backend, "sqlite");
    if (cfg->memory.vector_weight == 0.0) cfg->memory.vector_weight = 0.7;
    if (cfg->memory.keyword_weight == 0.0) cfg->memory.keyword_weight = 0.3;
    cfg->memory.auto_save = true;
    cfg->memory.hygiene_enabled = true;

    if (!cfg->agent.max_tool_iterations)    cfg->agent.max_tool_iterations = 25;
    if (!cfg->agent.max_history_messages)   cfg->agent.max_history_messages = 50;
    if (!cfg->agent.compaction_keep_recent) cfg->agent.compaction_keep_recent = 10;
    if (!cfg->agent.compaction_max_summary_chars) cfg->agent.compaction_max_summary_chars = 4000;
    if (!cfg->agent.compaction_max_source_chars)  cfg->agent.compaction_max_source_chars = 8000;

    if (!cfg->reliability.max_retries)     cfg->reliability.max_retries = 3;
    if (!cfg->reliability.retry_delay_ms)  cfg->reliability.retry_delay_ms = 1000;

    if (!cfg->heartbeat.interval_minutes)  cfg->heartbeat.interval_minutes = 30;

    if (!cfg->autonomy.level[0])      snprintf(cfg->autonomy.level, sizeof cfg->autonomy.level, "standard");
    if (!cfg->autonomy.max_actions_per_hour) cfg->autonomy.max_actions_per_hour = 20;
    cfg->autonomy.workspace_only = true;

    if (!cfg->security.sandbox_backend[0])
        snprintf(cfg->security.sandbox_backend, sizeof cfg->security.sandbox_backend, "auto");
    cfg->security.secrets_encrypt = true;

    /* Copy nested to flat aliases */
    cfg->temperature                   = cfg->default_temperature;
    cfg->workspace_only                = cfg->autonomy.workspace_only;
    cfg->max_actions_per_hour          = cfg->autonomy.max_actions_per_hour;
    cfg->gateway_port                  = cfg->gateway.port;
    snprintf(cfg->gateway_host, sizeof cfg->gateway_host, "%s", cfg->gateway.host);
    cfg->memory_auto_save              = cfg->memory.auto_save;
    cfg->heartbeat_enabled             = cfg->heartbeat.enabled;
    cfg->heartbeat_interval_minutes    = cfg->heartbeat.interval_minutes;
}

/* ── Provider parsing ───────────────────────────────────────────── */

static void parse_providers(NcConfig *cfg, const cJSON *root) {
    /* New format: "models": { "providers": { "openai": { "api_key": "..." } } } */
    const cJSON *models = cJSON_GetObjectItemCaseSensitive(root, "models");
    const cJSON *providers_obj = NULL;
    if (models) providers_obj = cJSON_GetObjectItemCaseSensitive(models, "providers");
    if (!providers_obj) providers_obj = cJSON_GetObjectItemCaseSensitive(root, "providers");

    if (providers_obj && cJSON_IsObject(providers_obj)) {
        cJSON *entry;
        cJSON_ArrayForEach(entry, providers_obj) {
            if (cfg->providers_count >= NC_MAX_PROVIDERS) break;
            NcProviderEntry *pe = &cfg->providers[cfg->providers_count++];
            snprintf(pe->name, sizeof pe->name, "%s", entry->string);
            safe_str(pe->api_key, sizeof pe->api_key, entry, "api_key");
            safe_str(pe->base_url, sizeof pe->base_url, entry, "base_url");
        }
    }

    /* Flat format: "openrouter_api_key": "..." etc. */
    const char *key_suffixes[] = {
        "openai", "anthropic", "groq", "mistral", "together",
        "fireworks", "perplexity", "deepseek", "xai", "cohere",
        "openrouter", "gemini", "ollama", NULL
    };
    for (int i = 0; key_suffixes[i]; i++) {
        char key_name[128];
        snprintf(key_name, sizeof key_name, "%s_api_key", key_suffixes[i]);
        cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key_name);
        if (!v || !cJSON_IsString(v) || !v->valuestring[0]) continue;
        /* Check if already added */
        bool found = false;
        for (size_t j = 0; j < cfg->providers_count; j++) {
            if (strcmp(cfg->providers[j].name, key_suffixes[i]) == 0) { found = true; break; }
        }
        if (!found && cfg->providers_count < NC_MAX_PROVIDERS) {
            NcProviderEntry *pe = &cfg->providers[cfg->providers_count++];
            snprintf(pe->name, sizeof pe->name, "%s", key_suffixes[i]);
            snprintf(pe->api_key, sizeof pe->api_key, "%s", v->valuestring);
        }
    }
}

/* ── Channel parsing ────────────────────────────────────────────── */

static void parse_channels(NcConfig *cfg, const cJSON *root) {
    const cJSON *ch = cJSON_GetObjectItemCaseSensitive(root, "channels");
    if (!ch) return;

    const cJSON *tg = cJSON_GetObjectItemCaseSensitive(ch, "telegram");
    if (tg) {
        safe_str(cfg->channels.telegram.bot_token, sizeof cfg->channels.telegram.bot_token, tg, "bot_token");
        cfg->channels.telegram_enabled = cfg->channels.telegram.bot_token[0] != '\0';
    }

    const cJSON *dc = cJSON_GetObjectItemCaseSensitive(ch, "discord");
    if (dc) {
        safe_str(cfg->channels.discord.bot_token, sizeof cfg->channels.discord.bot_token, dc, "bot_token");
        safe_str(cfg->channels.discord.guild_id, sizeof cfg->channels.discord.guild_id, dc, "guild_id");
        cfg->channels.discord_enabled = cfg->channels.discord.bot_token[0] != '\0';
    }

    const cJSON *sl = cJSON_GetObjectItemCaseSensitive(ch, "slack");
    if (sl) {
        safe_str(cfg->channels.slack.bot_token, sizeof cfg->channels.slack.bot_token, sl, "bot_token");
        safe_str(cfg->channels.slack.channel_id, sizeof cfg->channels.slack.channel_id, sl, "channel_id");
        cfg->channels.slack_enabled = cfg->channels.slack.bot_token[0] != '\0';
    }

    const cJSON *mat = cJSON_GetObjectItemCaseSensitive(ch, "matrix");
    if (mat) {
        safe_str(cfg->channels.matrix.homeserver, sizeof cfg->channels.matrix.homeserver, mat, "homeserver");
        safe_str(cfg->channels.matrix.access_token, sizeof cfg->channels.matrix.access_token, mat, "access_token");
        safe_str(cfg->channels.matrix.user_id, sizeof cfg->channels.matrix.user_id, mat, "user_id");
        cfg->channels.matrix_enabled = cfg->channels.matrix.homeserver[0] != '\0';
    }

    const cJSON *irc = cJSON_GetObjectItemCaseSensitive(ch, "irc");
    if (irc) {
        safe_str(cfg->channels.irc.host, sizeof cfg->channels.irc.host, irc, "host");
        safe_uint16(&cfg->channels.irc.port, irc, "port");
        if (!cfg->channels.irc.port) cfg->channels.irc.port = 6697;
        safe_str(cfg->channels.irc.nick, sizeof cfg->channels.irc.nick, irc, "nick");
        safe_str(cfg->channels.irc.password, sizeof cfg->channels.irc.password, irc, "password");
        safe_bool(&cfg->channels.irc.tls, irc, "tls");
        cfg->channels.irc_enabled = cfg->channels.irc.host[0] != '\0';
    }

    cJSON *imsg = cJSON_GetObjectItemCaseSensitive(ch, "imessage");
    if (imsg) safe_bool(&cfg->channels.imessage_enabled, ch, "imessage_enabled");
}

/* ── MCP servers ────────────────────────────────────────────────── */

static void parse_mcp_servers(NcConfig *cfg, const cJSON *root) {
    cJSON *servers = cJSON_GetObjectItemCaseSensitive(root, "mcp_servers");
    if (!servers || !cJSON_IsArray(servers)) return;

    cJSON *item;
    cJSON_ArrayForEach(item, servers) {
        if (cfg->mcp_servers_count >= NC_MAX_MCP) break;
        NcMcpServerConfig *s = &cfg->mcp_servers[cfg->mcp_servers_count++];
        safe_str(s->name, sizeof s->name, item, "name");
        safe_str(s->command, sizeof s->command, item, "command");
        cJSON *args = cJSON_GetObjectItemCaseSensitive(item, "args");
        if (args && cJSON_IsArray(args)) {
            cJSON *arg;
            cJSON_ArrayForEach(arg, args) {
                if (s->args_count >= 8) break;
                if (cJSON_IsString(arg))
                    snprintf(s->args[s->args_count++], 256, "%s", arg->valuestring);
            }
        }
        cJSON *env = cJSON_GetObjectItemCaseSensitive(item, "env");
        if (env && cJSON_IsObject(env)) {
            cJSON *pair;
            cJSON_ArrayForEach(pair, env) {
                if (s->env_count >= 8) break;
                snprintf(s->env_keys[s->env_count], 64, "%s", pair->string);
                if (cJSON_IsString(pair))
                    snprintf(s->env_vals[s->env_count], 256, "%s", pair->valuestring);
                s->env_count++;
            }
        }
    }
}

/* ── Main load ──────────────────────────────────────────────────── */

int nc_config_load_path(NcConfig *cfg, const char *path) {
    memset(cfg, 0, sizeof *cfg);

    snprintf(cfg->config_path, sizeof cfg->config_path, "%s", path);
    /* Workspace dir = config dir */
    char *slash = strrchr(cfg->config_path, '/');
    if (slash) {
        size_t len = (size_t)(slash - cfg->config_path);
        snprintf(cfg->workspace_dir, sizeof cfg->workspace_dir, "%.*s", (int)len, cfg->config_path);
    } else {
        snprintf(cfg->workspace_dir, sizeof cfg->workspace_dir, ".");
    }

    /* Also check for workspace_dir override in CWD */
    char cwd[4096];
    if (getcwd(cwd, sizeof cwd)) {
        snprintf(cfg->workspace_dir, sizeof cfg->workspace_dir, "%s", cwd);
    }

    size_t json_len;
    char *json = nc_read_file(path, &json_len);
    if (!json) {
        nc_config_apply_defaults(cfg);
        return -1;
    }

    cJSON *root = cJSON_ParseWithLength(json, json_len);
    free(json);
    if (!root) {
        nc_config_apply_defaults(cfg);
        return -1;
    }

    /* Top-level fields */
    safe_str(cfg->default_provider, sizeof cfg->default_provider, root, "default_provider");
    safe_str(cfg->default_model, sizeof cfg->default_model, root, "default_model");
    safe_double(&cfg->default_temperature, root, "default_temperature");
    safe_str(cfg->reasoning_effort, sizeof cfg->reasoning_effort, root, "reasoning_effort");

    /* Nested agent defaults model (OpenClaw-compat) */
    const cJSON *agents = cJSON_GetObjectItemCaseSensitive(root, "agents");
    if (agents) {
        const cJSON *defs = cJSON_GetObjectItemCaseSensitive(agents, "defaults");
        if (defs) {
            const cJSON *model = cJSON_GetObjectItemCaseSensitive(defs, "model");
            if (model) safe_str(cfg->default_model, sizeof cfg->default_model, model, "primary");
        }
    }

    parse_providers(cfg, root);
    parse_channels(cfg, root);
    parse_mcp_servers(cfg, root);

    /* Memory */
    const cJSON *mem = cJSON_GetObjectItemCaseSensitive(root, "memory");
    if (mem) {
        safe_str(cfg->memory.backend, sizeof cfg->memory.backend, mem, "backend");
        safe_bool(&cfg->memory.auto_save, mem, "auto_save");
        safe_str(cfg->memory.embedding_provider, sizeof cfg->memory.embedding_provider, mem, "embedding_provider");
        safe_double(&cfg->memory.vector_weight, mem, "vector_weight");
        safe_double(&cfg->memory.keyword_weight, mem, "keyword_weight");
        safe_bool(&cfg->memory.hygiene_enabled, mem, "hygiene_enabled");
        safe_bool(&cfg->memory.snapshot_enabled, mem, "snapshot_enabled");
    }

    /* Gateway */
    const cJSON *gw = cJSON_GetObjectItemCaseSensitive(root, "gateway");
    if (gw) {
        safe_str(cfg->gateway.host, sizeof cfg->gateway.host, gw, "host");
        safe_uint16(&cfg->gateway.port, gw, "port");
        safe_bool(&cfg->gateway.allow_public_bind, gw, "allow_public_bind");
        safe_uint32(&cfg->gateway.rate_limit_per_minute, gw, "rate_limit_per_minute");
    }

    /* Tunnel */
    const cJSON *tun = cJSON_GetObjectItemCaseSensitive(root, "tunnel");
    if (tun) {
        safe_str(cfg->tunnel.provider, sizeof cfg->tunnel.provider, tun, "provider");
        safe_str(cfg->tunnel.token, sizeof cfg->tunnel.token, tun, "token");
        safe_str(cfg->tunnel.domain, sizeof cfg->tunnel.domain, tun, "domain");
        safe_str(cfg->tunnel.hostname, sizeof cfg->tunnel.hostname, tun, "hostname");
        safe_bool(&cfg->tunnel.tailscale_funnel, tun, "funnel");
    }

    /* Security */
    const cJSON *sec = cJSON_GetObjectItemCaseSensitive(root, "security");
    if (sec) {
        safe_str(cfg->security.sandbox_backend, sizeof cfg->security.sandbox_backend, sec, "sandbox");
        safe_bool(&cfg->security.secrets_encrypt, sec, "secrets_encrypt");
        safe_bool(&cfg->security.audit_enabled, sec, "audit_enabled");
        safe_str(cfg->security.audit_log_path, sizeof cfg->security.audit_log_path, sec, "audit_log_path");
        safe_uint32(&cfg->security.audit_retention_days, sec, "audit_retention_days");
    }

    /* Autonomy */
    const cJSON *aut = cJSON_GetObjectItemCaseSensitive(root, "autonomy");
    if (aut) {
        safe_str(cfg->autonomy.level, sizeof cfg->autonomy.level, aut, "level");
        safe_bool(&cfg->autonomy.workspace_only, aut, "workspace_only");
        safe_uint32(&cfg->autonomy.max_actions_per_hour, aut, "max_actions_per_hour");
        safe_uint32(&cfg->autonomy.max_output_bytes, aut, "max_output_bytes");
    }

    /* Agent config */
    const cJSON *ag = cJSON_GetObjectItemCaseSensitive(root, "agent");
    if (ag) {
        safe_uint32(&cfg->agent.max_tool_iterations, ag, "max_tool_iterations");
        safe_uint32(&cfg->agent.max_history_messages, ag, "max_history_messages");
        safe_uint32(&cfg->agent.compaction_keep_recent, ag, "compaction_keep_recent");
        safe_uint32(&cfg->agent.compaction_max_summary_chars, ag, "compaction_max_summary_chars");
        safe_uint32(&cfg->agent.compaction_max_source_chars, ag, "compaction_max_source_chars");
    }

    /* Heartbeat */
    const cJSON *hb = cJSON_GetObjectItemCaseSensitive(root, "heartbeat");
    if (hb) {
        safe_bool(&cfg->heartbeat.enabled, hb, "enabled");
        safe_uint32(&cfg->heartbeat.interval_minutes, hb, "interval_minutes");
    }

    /* Cron */
    const cJSON *cr = cJSON_GetObjectItemCaseSensitive(root, "cron");
    if (cr) {
        safe_bool(&cfg->cron.enabled, cr, "enabled");
    }

    /* Reliability */
    const cJSON *rel = cJSON_GetObjectItemCaseSensitive(root, "reliability");
    if (rel) {
        safe_uint32(&cfg->reliability.max_retries, rel, "max_retries");
        safe_uint32(&cfg->reliability.retry_delay_ms, rel, "retry_delay_ms");
        safe_bool(&cfg->reliability.fallback_enabled, rel, "fallback_enabled");
    }

    cJSON_Delete(root);
    nc_config_apply_defaults(cfg);
    return 0;
}

int nc_config_load(NcConfig *cfg) {
    char dir[4096], path[4096];
    nc_config_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/config.json", dir);
    return nc_config_load_path(cfg, path);
}


const char *nc_config_get_provider_key(const NcConfig *cfg, const char *provider) {
    for (size_t i = 0; i < cfg->providers_count; i++) {
        if (strcmp(cfg->providers[i].name, provider) == 0)
            return cfg->providers[i].api_key[0] ? cfg->providers[i].api_key : NULL;
    }
    return NULL;
}

const char *nc_config_get_provider_url(const NcConfig *cfg, const char *provider) {
    for (size_t i = 0; i < cfg->providers_count; i++) {
        if (strcmp(cfg->providers[i].name, provider) == 0)
            return cfg->providers[i].base_url[0] ? cfg->providers[i].base_url : NULL;
    }
    return NULL;
}

int nc_config_save(const NcConfig *cfg) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "default_provider", cfg->default_provider);
    cJSON_AddStringToObject(root, "default_model", cfg->default_model);
    cJSON_AddNumberToObject(root, "default_temperature", cfg->default_temperature);

    cJSON *models = cJSON_CreateObject();
    cJSON *providers_obj = cJSON_CreateObject();
    for (size_t i = 0; i < cfg->providers_count; i++) {
        cJSON *pe = cJSON_CreateObject();
        cJSON_AddStringToObject(pe, "api_key", cfg->providers[i].api_key);
        if (cfg->providers[i].base_url[0])
            cJSON_AddStringToObject(pe, "base_url", cfg->providers[i].base_url);
        cJSON_AddItemToObject(providers_obj, cfg->providers[i].name, pe);
    }
    cJSON_AddItemToObject(models, "providers", providers_obj);
    cJSON_AddItemToObject(root, "models", models);

    cJSON *gw = cJSON_CreateObject();
    cJSON_AddStringToObject(gw, "host", cfg->gateway.host);
    cJSON_AddNumberToObject(gw, "port", cfg->gateway.port);
    cJSON_AddItemToObject(root, "gateway", gw);

    char *json = cJSON_Print(root);
    cJSON_Delete(root);

    char path[4096];
    snprintf(path, sizeof path, "%s", cfg->config_path);
    int rc = nc_write_file(path, json, strlen(json));
    free(json);
    return rc;
}
