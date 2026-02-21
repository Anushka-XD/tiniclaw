#pragma once
/*
 * tiniclaw-c — channel vtable interface
 * Mirrors tiniclaw's Channel vtable from channels/root.zig
 */
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "arena.h"

/* ── Channel message ─────────────────────────────────────────────── */

typedef struct NcChannelMessage {
    const char *id;
    const char *sender;
    const char *content;
    const char *channel;      /* channel name literal */
    uint64_t    timestamp;
    const char *reply_target; /* NULL if none */
    int64_t     message_id;   /* 0 if none */
    const char *first_name;   /* NULL if none */
    bool        is_group;
} NcChannelMessage;

/* ── Incoming message handler (agent processes this) ─────────────── */
typedef void (*NcMessageHandler)(const NcChannelMessage *msg, void *ctx);

/* ── Channel vtable ──────────────────────────────────────────────── */

typedef struct NcChannelVTable {
    /* Send a message; arena may be NULL for simple channels */
    int          (*send)(void *self, NcArena *arena, const char *msg, const char *to);
    /* Block and call handler for each incoming message */
    int          (*listen)(void *self, NcMessageHandler handler, void *handler_ctx);
    const char  *(*name)(void *self);
    bool         (*is_configured)(void *self);
    void         (*deinit)(void *self);
} NcChannelVTable;

typedef struct NcChannel {
    void                  *ptr;
    const NcChannelVTable *vtable;
} NcChannel;

static inline int nc_channel_send(NcChannel c, NcArena *arena,
                                   const char *msg, const char *to) {
    if (!c.vtable || !c.vtable->send) return -1;
    return c.vtable->send(c.ptr, arena, msg, to);
}
static inline int nc_channel_listen(NcChannel c, NcMessageHandler h, void *ctx) {
    if (!c.vtable || !c.vtable->listen) return -1;
    return c.vtable->listen(c.ptr, h, ctx);
}
static inline const char *nc_channel_name(NcChannel c) {
    if (!c.vtable || !c.vtable->name) return "unknown";
    return c.vtable->name(c.ptr);
}
static inline bool nc_channel_is_configured(NcChannel c) {
    if (!c.vtable || !c.vtable->is_configured) return false;
    return c.vtable->is_configured(c.ptr);
}
static inline void nc_channel_deinit(NcChannel c) {
    if (c.vtable && c.vtable->deinit) c.vtable->deinit(c.ptr);
}

/* ── Concrete channel constructors ───────────────────────────────── */

/* CLI — stdin/stdout interactive terminal */
NcChannel nc_channel_cli_init(const char *prompt);

/* Telegram — long-polling bot API */
typedef struct NcTelegramConfig {
    const char  *bot_token;
    const char **allowlist;   /* NULL-terminated, NULL = deny all; "*" = allow all */
    int64_t     *chat_ids;    /* NULL-terminated whitelist of chat IDs, NULL = any */
    size_t       chat_ids_count;
} NcTelegramConfig;

NcChannel nc_channel_telegram_init(NcArena *arena, const NcTelegramConfig *cfg);

/* Discord — WebSocket gateway */
typedef struct NcDiscordConfig {
    const char  *bot_token;
    const char  *guild_id;
    const char **allowed_channels; /* NULL-terminated */
    const char **allowlist;
} NcDiscordConfig;

NcChannel nc_channel_discord_init(NcArena *arena, const NcDiscordConfig *cfg);

/* Slack — conversations.history polling */
typedef struct NcSlackConfig {
    const char *bot_token;
    const char *channel_id;
    const char **allowlist;
} NcSlackConfig;

NcChannel nc_channel_slack_init(NcArena *arena, const NcSlackConfig *cfg);

/* WhatsApp — webhook-based (requires gateway) */
typedef struct NcWhatsAppConfig {
    const char *verify_token;
    const char *access_token;
    const char *phone_number_id;
    const char **allowlist;
} NcWhatsAppConfig;

NcChannel nc_channel_whatsapp_init(NcArena *arena, const NcWhatsAppConfig *cfg);

/* Matrix — long-polling /sync */
typedef struct NcMatrixConfig {
    const char *homeserver;
    const char *access_token;
    const char *user_id;
    const char **allowlist;
} NcMatrixConfig;

NcChannel nc_channel_matrix_init(NcArena *arena, const NcMatrixConfig *cfg);

/* IRC — TLS socket */
typedef struct NcIrcConfig {
    const char *host;
    uint16_t    port;
    const char *nick;
    const char *password;
    const char **channels;  /* IRC channels to join */
    bool        tls;
    const char **allowlist;
} NcIrcConfig;

NcChannel nc_channel_irc_init(NcArena *arena, const NcIrcConfig *cfg);

/* iMessage — AppleScript + SQLite (macOS only) */
typedef struct NcIMessageConfig {
    const char **allowlist;
} NcIMessageConfig;

NcChannel nc_channel_imessage_init(NcArena *arena, const NcIMessageConfig *cfg);

/* Email — IMAP/SMTP via libcurl */
typedef struct NcEmailConfig {
    const char *imap_url;
    const char *smtp_url;
    const char *username;
    const char *password;
    const char **allowlist;
} NcEmailConfig;

NcChannel nc_channel_email_init(NcArena *arena, const NcEmailConfig *cfg);

/* Lark/Feishu — HTTP callback */
typedef struct NcLarkConfig {
    const char *app_id;
    const char *app_secret;
    const char *verification_token;
    const char **allowlist;
} NcLarkConfig;

NcChannel nc_channel_lark_init(NcArena *arena, const NcLarkConfig *cfg);

/* DingTalk — WebSocket stream mode */
typedef struct NcDingTalkConfig {
    const char *app_key;
    const char *app_secret;
    const char **allowlist;
} NcDingTalkConfig;

NcChannel nc_channel_dingtalk_init(NcArena *arena, const NcDingTalkConfig *cfg);
