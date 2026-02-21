/*
 * tiniclaw-c — Telegram channel (long-polling bot API)
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>
#include "channel.h"
#include "arena.h"
#include "util.h"

#define TG_API "https://api.telegram.org/bot"

typedef struct {
    char             bot_token[256];
    char             default_chat_id[64];
    long             offset;
    NcMessageHandler handler;
    void            *handler_ctx;
} TelegramChannel;

/* Curl write callback */
static size_t tg_write(char *d, size_t s, size_t n, void *ud) {
    NcBuf *b = ud;
    nc_buf_append(b, d, s * n);
    return s * n;
}

static cJSON *tg_post(const char *bot_token, const char *method,
                      const char *body, NcBuf *out) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    char url[512];
    snprintf(url, sizeof url, "%s%s/%s", TG_API, bot_token, method);
    struct curl_slist *hdrs = curl_slist_append(NULL, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "{}");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, tg_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return cJSON_Parse(out->data ? out->data : "{}");
}

static int tg_send(void *ptr, NcArena *arena, const char *msg, const char *to) {
    (void)arena;
    TelegramChannel *t = ptr;
    const char *chat = (to && to[0]) ? to : t->default_chat_id;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "chat_id", chat);
    cJSON_AddStringToObject(req, "text", msg ? msg : "");
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    NcBuf resp;
    nc_buf_init(&resp);
    cJSON *r = tg_post(t->bot_token, "sendMessage", body, &resp);
    free(body);
    nc_buf_free(&resp);
    int ok = 0;
    if (r) {
        cJSON *ok_j = cJSON_GetObjectItemCaseSensitive(r, "ok");
        ok = cJSON_IsTrue(ok_j);
        cJSON_Delete(r);
    }
    return ok ? 0 : -1;
}

static int tg_listen(void *ptr, NcMessageHandler handler, void *hctx) {
    TelegramChannel *t = ptr;
    while (1) {
        cJSON *req = cJSON_CreateObject();
        cJSON_AddNumberToObject(req, "offset", (double)t->offset);
        cJSON_AddNumberToObject(req, "timeout", 30);
        char *body = cJSON_PrintUnformatted(req);
        cJSON_Delete(req);
        NcBuf resp;
        nc_buf_init(&resp);
        cJSON *updates = tg_post(t->bot_token, "getUpdates", body, &resp);
        free(body);
        nc_buf_free(&resp);
        if (!updates) continue;

        cJSON *result = cJSON_GetObjectItemCaseSensitive(updates, "result");
        if (cJSON_IsArray(result)) {
            cJSON *upd = NULL;
            cJSON_ArrayForEach(upd, result) {
                cJSON *uid_j = cJSON_GetObjectItemCaseSensitive(upd, "update_id");
                if (uid_j) t->offset = (long)uid_j->valuedouble + 1;

                cJSON *msg_j = cJSON_GetObjectItemCaseSensitive(upd, "message");
                if (!msg_j) continue;
                cJSON *text_j = cJSON_GetObjectItemCaseSensitive(msg_j, "text");
                cJSON *from_j = cJSON_GetObjectItemCaseSensitive(msg_j, "from");
                cJSON *chat_j = cJSON_GetObjectItemCaseSensitive(msg_j, "chat");
                if (!text_j || !cJSON_IsString(text_j)) continue;

                char sender[64] = "unknown";
                if (from_j) {
                    cJSON *username = cJSON_GetObjectItemCaseSensitive(from_j, "username");
                    if (username && cJSON_IsString(username))
                        snprintf(sender, sizeof sender, "%s", username->valuestring);
                }
                char chat_id[32] = "";
                if (chat_j) {
                    cJSON *cid = cJSON_GetObjectItemCaseSensitive(chat_j, "id");
                    if (cid)
                        snprintf(chat_id, sizeof chat_id, "%lld",
                                 (long long)cid->valuedouble);
                }

                NcChannelMessage cm = {
                    .content      = text_j->valuestring,
                    .sender       = sender,
                    .channel      = "telegram",
                    .reply_target = chat_id,
                };
                if (handler) handler(&cm, hctx);
            }
        }
        cJSON_Delete(updates);
    }
    return 0;
}

static const char *tg_name(void *p) { (void)p; return "telegram"; }
static bool tg_configured(void *p) {
    TelegramChannel *t = p;
    return t && t->bot_token[0] != '\0';
}
static void tg_deinit(void *p) { free(p); }

static const NcChannelVTable TG_VTABLE = {
    .send          = tg_send,
    .listen        = tg_listen,
    .name          = tg_name,
    .is_configured = tg_configured,
    .deinit        = tg_deinit,
};

NcChannel nc_channel_telegram_init(NcArena *arena, const NcTelegramConfig *cfg) {
    (void)arena;
    TelegramChannel *t = calloc(1, sizeof *t);
    if (!t) return (NcChannel){0};
    if (cfg && cfg->bot_token)
        snprintf(t->bot_token, sizeof t->bot_token, "%s", cfg->bot_token);
    return (NcChannel){ .ptr = t, .vtable = &TG_VTABLE };
}
