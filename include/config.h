#pragma once
/*
 * tiniclaw-c — full configuration schema
 * Mirrors tiniclaw's config_types.zig + config.zig
 * Loaded from ~/.tiniclaw/config.json
 */
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* ── Provider entry ─────────────────────────────────────────────── */

typedef struct NcProviderEntry {
    char name[64];
    char api_key[512];
    char base_url[512];
} NcProviderEntry;

/* ── Memory config ──────────────────────────────────────────────── */

typedef struct NcMemoryConfig {
    char   backend[32];           /* "sqlite", "markdown", "none" */
    bool   auto_save;
    char   embedding_provider[32];
    double vector_weight;
    double keyword_weight;
    bool   hygiene_enabled;
    bool   snapshot_enabled;
    uint32_t max_memories;
} NcMemoryConfig;

/* ── Autonomy / security config ─────────────────────────────────── */

typedef struct NcAutonomyConfig {
    char     level[16];           /* "supervised", "standard", "full" */
    bool     workspace_only;
    uint32_t max_actions_per_hour;
    uint32_t max_output_bytes;
} NcAutonomyConfig;

/* ── Agent config ───────────────────────────────────────────────── */

typedef struct NcAgentConfig {
    uint32_t max_tool_iterations;
    uint32_t max_history_messages;
    uint64_t token_limit;
    uint64_t message_timeout_secs;
    uint32_t compaction_keep_recent;
    uint32_t compaction_max_summary_chars;
    uint32_t compaction_max_source_chars;
} NcAgentConfig;

/* ── Gateway config ─────────────────────────────────────────────── */

typedef struct NcGatewayConfig {
    char     host[128];
    uint16_t port;
    bool     allow_public_bind;
    uint32_t rate_limit_per_minute;
    uint32_t body_size_limit;    /* bytes, default 65536 */
    uint32_t request_timeout_secs;
} NcGatewayConfig;

/* ── Tunnel config ──────────────────────────────────────────────── */

typedef struct NcTunnelConfig {
    char provider[32];   /* "none","cloudflare","tailscale","ngrok","custom" */
    char token[512];     /* cloudflare/ngrok auth token */
    char domain[256];    /* optional ngrok domain */
    char start_command[512]; /* custom */
    char health_url[512];    /* custom */
    char hostname[256];      /* tailscale */
    bool tailscale_funnel;
} NcTunnelConfig;

/* ── Channel configs ────────────────────────────────────────────── */

typedef struct NcTelegramConfig2 {
    char   bot_token[256];
    char   allowlist[8][128];   /* up to 8 usernames */
    size_t allowlist_count;
} NcTelegramConfig2;

typedef struct NcDiscordConfig2 {
    char   bot_token[256];
    char   guild_id[64];
    char   allowlist[8][128];
    size_t allowlist_count;
} NcDiscordConfig2;

typedef struct NcSlackConfig2 {
    char bot_token[256];
    char channel_id[64];
} NcSlackConfig2;

typedef struct NcMatrixConfig2 {
    char homeserver[256];
    char access_token[512];
    char user_id[128];
} NcMatrixConfig2;

typedef struct NcIrcConfig2 {
    char     host[256];
    uint16_t port;
    char     nick[64];
    char     password[256];
    char     channels[8][64];
    size_t   channels_count;
    bool     tls;
} NcIrcConfig2;

typedef struct NcEmailConfig2 {
    char imap_url[256];
    char smtp_url[256];
    char username[128];
    char password[256];
} NcEmailConfig2;

typedef struct NcChannelsConfig {
    NcTelegramConfig2 telegram;
    NcDiscordConfig2  discord;
    NcSlackConfig2    slack;
    NcMatrixConfig2   matrix;
    NcIrcConfig2      irc;
    NcEmailConfig2    email;
    bool telegram_enabled;
    bool discord_enabled;
    bool slack_enabled;
    bool matrix_enabled;
    bool irc_enabled;
    bool email_enabled;
    bool imessage_enabled;
    bool lark_enabled;
    bool dingtalk_enabled;
    bool whatsapp_enabled;
    bool qq_enabled;
} NcChannelsConfig;

/* ── Security config ────────────────────────────────────────────── */

typedef struct NcSecurityConfig {
    char sandbox_backend[32];    /* "auto","none","landlock","firejail","bubblewrap","docker" */
    bool secrets_encrypt;
    bool audit_enabled;
    char audit_log_path[4096];
    uint32_t audit_retention_days;
} NcSecurityConfig;

/* ── Reliability config ─────────────────────────────────────────── */

typedef struct NcReliabilityConfig {
    uint32_t max_retries;
    uint32_t retry_delay_ms;
    bool     fallback_enabled;
} NcReliabilityConfig;

/* ── Cron config ────────────────────────────────────────────────── */

typedef struct NcCronConfig {
    bool   enabled;
    char   jobs_path[4096];    /* path to cron_jobs.json */
    uint32_t max_jobs;
} NcCronConfig;

/* ── Heartbeat config ───────────────────────────────────────────── */

typedef struct NcHeartbeatConfig {
    bool     enabled;
    uint32_t interval_minutes;
} NcHeartbeatConfig;

/* ── MCP server config ──────────────────────────────────────────── */

typedef struct NcMcpServerConfig {
    char   name[64];
    char   command[256];
    char   args[8][256];
    size_t args_count;
    /* env key=value pairs */
    char   env_keys[8][64];
    char   env_vals[8][256];
    size_t env_count;
} NcMcpServerConfig;

/* ── Top-level Config ───────────────────────────────────────────── */

#define NC_MAX_PROVIDERS 32
#define NC_MAX_MCP       16

typedef struct NcConfig {
    /* Computed at load time */
    char workspace_dir[4096];
    char config_path[4096];

    /* Provider entries */
    NcProviderEntry providers[NC_MAX_PROVIDERS];
    size_t          providers_count;

    /* Defaults */
    char   default_provider[64];
    char   default_model[128];
    double default_temperature;
    char   reasoning_effort[16]; /* "" = none */

    /* MCP servers */
    NcMcpServerConfig mcp_servers[NC_MAX_MCP];
    size_t            mcp_servers_count;

    /* Sub-configs */
    NcAgentConfig      agent;
    NcMemoryConfig     memory;
    NcGatewayConfig    gateway;
    NcTunnelConfig     tunnel;
    NcChannelsConfig   channels;
    NcSecurityConfig   security;
    NcAutonomyConfig   autonomy;
    NcReliabilityConfig reliability;
    NcCronConfig       cron;
    NcHeartbeatConfig  heartbeat;

    /* Convenience flat aliases (copied from nested on load) */
    double   temperature;
    uint32_t max_tokens;         /* 0 = provider default */
    bool     workspace_only;
    uint32_t max_actions_per_hour;
    uint16_t gateway_port;
    char     gateway_host[128];
    bool     memory_auto_save;
    bool     heartbeat_enabled;
    uint32_t heartbeat_interval_minutes;
} NcConfig;

/* Load config from ~/.tiniclaw/config.json.
   Returns 0 on success, -1 on error. */
int  nc_config_load(NcConfig *cfg);

/* Load config from explicit path. */
int  nc_config_load_path(NcConfig *cfg, const char *path);

/* Write config to disk. */
int  nc_config_save(const NcConfig *cfg);

/* Get API key for a named provider. NULL if not found. */
const char *nc_config_get_provider_key(const NcConfig *cfg, const char *provider);
const char *nc_config_get_provider_url(const NcConfig *cfg, const char *provider);

/* Apply defaults for missing fields. */
void nc_config_apply_defaults(NcConfig *cfg);

/* Resolve the config directory path into buf. */
void nc_config_dir(char *buf, size_t size);
