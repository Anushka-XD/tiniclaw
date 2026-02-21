/*
 * tiniclaw-c — stub channel implementations
 *   Discord, Slack, WhatsApp, Matrix, IRC, iMessage, Email, Lark, DingTalk
 *
 * Each provides a correctly-typed NcChannel that reports is_configured()
 * and returns a descriptive "not implemented" error on send/listen.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include "channel.h"
#include "arena.h"
#include "util.h"

/*
 * STUB_CHANNEL(id, name_str, token_field, cfg_type)
 * Expands to a complete set of vtable functions + init for a stub channel.
 * The is_configured check reads cfg_type->token_field[0] != '\0'.
 */
#define STUB_CHANNEL(id, name_str, cfg_type, token_field) \
    static int id##_send(void *p, NcArena *a, const char *m, const char *t) { \
        (void)p; (void)a; (void)m; (void)t; \
        fprintf(stderr, name_str ": send not implemented\n"); return -1; \
    } \
    static int id##_listen(void *p, NcMessageHandler h, void *hctx) { \
        (void)p; (void)h; (void)hctx; \
        fprintf(stderr, name_str ": listen not implemented\n"); return -1; \
    } \
    static const char *id##_name(void *p) { (void)p; return name_str; } \
    static bool id##_configured(void *p) { \
        cfg_type *c = p; return c && c->token_field && c->token_field[0] != '\0'; \
    } \
    static void id##_deinit(void *p) { free(p); } \
    static const NcChannelVTable id##_VT = { \
        .send = id##_send, .listen = id##_listen, \
        .name = id##_name, .is_configured = id##_configured, \
        .deinit = id##_deinit, \
    }; \
    NcChannel nc_channel_##id##_init(NcArena *arena, const cfg_type *cfg) { \
        (void)arena; \
        cfg_type *c = calloc(1, sizeof *c); \
        if (!c) return (NcChannel){0}; \
        if (cfg) *c = *cfg; \
        return (NcChannel){ .ptr = c, .vtable = &id##_VT }; \
    }

STUB_CHANNEL(discord,  "discord",  NcDiscordConfig,  bot_token)
STUB_CHANNEL(slack,    "slack",    NcSlackConfig,    bot_token)
STUB_CHANNEL(whatsapp, "whatsapp", NcWhatsAppConfig, access_token)
STUB_CHANNEL(matrix,   "matrix",  NcMatrixConfig,   access_token)
STUB_CHANNEL(lark,     "lark",    NcLarkConfig,     app_id)
STUB_CHANNEL(dingtalk, "dingtalk", NcDingTalkConfig, app_key)

/* IRC — has different config shape (host+nick instead of token) */
static int irc_send(void *p, NcArena *a, const char *m, const char *t) {
    (void)p; (void)a; (void)m; (void)t;
    fprintf(stderr, "irc: send not implemented\n"); return -1;
}
static int irc_listen(void *p, NcMessageHandler h, void *hctx) {
    (void)p; (void)h; (void)hctx;
    fprintf(stderr, "irc: listen not implemented\n"); return -1;
}
static const char *irc_name(void *p) { (void)p; return "irc"; }
static bool irc_configured(void *p) {
    NcIrcConfig *c = p; return c && c->host && c->host[0] != '\0';
}
static void irc_deinit(void *p) { free(p); }
static const NcChannelVTable IRC_VT = {
    .send = irc_send, .listen = irc_listen,
    .name = irc_name, .is_configured = irc_configured,
    .deinit = irc_deinit,
};
NcChannel nc_channel_irc_init(NcArena *arena, const NcIrcConfig *cfg) {
    (void)arena;
    NcIrcConfig *c = calloc(1, sizeof *c);
    if (!c) return (NcChannel){0};
    if (cfg) *c = *cfg;
    return (NcChannel){ .ptr = c, .vtable = &IRC_VT };
}

/* iMessage — macOS only, no token, just allowlist */
static int imsg_send(void *p, NcArena *a, const char *m, const char *t) {
    (void)p; (void)a; (void)m; (void)t;
    fprintf(stderr, "imessage: send not implemented\n"); return -1;
}
static int imsg_listen(void *p, NcMessageHandler h, void *hctx) {
    (void)p; (void)h; (void)hctx;
    fprintf(stderr, "imessage: listen not implemented\n"); return -1;
}
static const char *imsg_name(void *p) { (void)p; return "imessage"; }
static bool imsg_configured(void *p) { (void)p; return false; }
static void imsg_deinit(void *p) { free(p); }
static const NcChannelVTable IMSG_VT = {
    .send = imsg_send, .listen = imsg_listen,
    .name = imsg_name, .is_configured = imsg_configured,
    .deinit = imsg_deinit,
};
NcChannel nc_channel_imessage_init(NcArena *arena, const NcIMessageConfig *cfg) {
    (void)arena;
    NcIMessageConfig *c = calloc(1, sizeof *c);
    if (!c) return (NcChannel){0};
    if (cfg) *c = *cfg;
    return (NcChannel){ .ptr = c, .vtable = &IMSG_VT };
}

/* Email — IMAP/SMTP */
static int email_send(void *p, NcArena *a, const char *m, const char *t) {
    (void)p; (void)a; (void)m; (void)t;
    fprintf(stderr, "email: send not implemented\n"); return -1;
}
static int email_listen(void *p, NcMessageHandler h, void *hctx) {
    (void)p; (void)h; (void)hctx;
    fprintf(stderr, "email: listen not implemented\n"); return -1;
}
static const char *email_name(void *p) { (void)p; return "email"; }
static bool email_configured(void *p) {
    NcEmailConfig *c = p; return c && c->username && c->username[0] != '\0';
}
static void email_deinit(void *p) { free(p); }
static const NcChannelVTable EMAIL_VT = {
    .send = email_send, .listen = email_listen,
    .name = email_name, .is_configured = email_configured,
    .deinit = email_deinit,
};
NcChannel nc_channel_email_init(NcArena *arena, const NcEmailConfig *cfg) {
    (void)arena;
    NcEmailConfig *c = calloc(1, sizeof *c);
    if (!c) return (NcChannel){0};
    if (cfg) *c = *cfg;
    return (NcChannel){ .ptr = c, .vtable = &EMAIL_VT };
}
