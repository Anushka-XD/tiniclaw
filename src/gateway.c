/*
 * tiniclaw-c — HTTP gateway server
 *
 * Endpoints:
 *   GET  /health          — liveness probe
 *   GET  /ready           — readiness probe
 *   POST /pair            — OTP pairing exchange
 *   POST /webhook         — authenticated agent message endpoint
 *   GET  /metrics         — minimal prometheus-style metrics
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <time.h>
#include <cjson/cJSON.h>
#include "gateway.h"
#include "agent.h"
#include "security.h"
#include "arena.h"
#include "util.h"

#define GW_BACKLOG      32
#define GW_BUF_SIZE     (64 * 1024)
#define GW_BODY_MAX     (1024 * 1024)

/* ── HTTP helpers ────────────────────────────────────────────────── */

static void gw_send(int fd, int code, const char *ctype, const char *body) {
    char head[512];
    size_t blen = body ? strlen(body) : 0;
    snprintf(head, sizeof head,
             "HTTP/1.1 %d %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "\r\n",
             code,
             code == 200 ? "OK" :
             code == 201 ? "Created" :
             code == 400 ? "Bad Request" :
             code == 401 ? "Unauthorized" :
             code == 403 ? "Forbidden" :
             code == 404 ? "Not Found" :
             code == 429 ? "Too Many Requests" : "Internal Server Error",
             ctype, blen);
    send(fd, head, strlen(head), 0);
    if (body && blen) send(fd, body, blen, 0);
}

static void gw_json(int fd, int code, const char *json) {
    gw_send(fd, code, "application/json", json);
}

/* ── Request parser (minimal) ────────────────────────────────────── */

typedef struct {
    char method[8];
    char path[256];
    char auth[512];
    char *body;      /* malloc'd, may be NULL */
    size_t body_len;
} GwRequest;

static void gw_request_free(GwRequest *r) { free(r->body); }

static int gw_parse_request(char *buf, size_t len, GwRequest *out) {
    memset(out, 0, sizeof *out);
    /* Method */
    char *p = buf;
    char *sp = strchr(p, ' ');
    if (!sp) return -1;
    size_t mlen = (size_t)(sp - p);
    if (mlen >= sizeof out->method) return -1;
    memcpy(out->method, p, mlen);
    p = sp + 1;
    /* Path */
    sp = strchr(p, ' ');
    if (!sp) return -1;
    size_t plen = (size_t)(sp - p);
    if (plen >= sizeof out->path) plen = sizeof(out->path) - 1;
    memcpy(out->path, p, plen);
    /* Strip query string */
    char *qs = strchr(out->path, '?');
    if (qs) *qs = '\0';

    /* Headers */
    char *headers_end = strstr(buf, "\r\n\r\n");
    if (!headers_end) return -1;

    /* Extract Authorization header */
    char *auth_line = strcasestr(buf, "\r\nAuthorization: ");
    if (auth_line) {
        auth_line += 17;
        char *eol = strstr(auth_line, "\r\n");
        size_t alen = eol ? (size_t)(eol - auth_line) : strlen(auth_line);
        if (alen >= sizeof out->auth) alen = sizeof(out->auth) - 1;
        memcpy(out->auth, auth_line, alen);
    }

    /* Body */
    char *body_start = headers_end + 4;
    size_t body_len  = len - (size_t)(body_start - buf);
    if (body_len > 0 && body_len < GW_BODY_MAX) {
        out->body = malloc(body_len + 1);
        memcpy(out->body, body_start, body_len);
        out->body[body_len] = '\0';
        out->body_len = body_len;
    }
    return 0;
}

/* ── Per-connection handler ─────────────────────────────────────── */

typedef struct {
    int fd;
    NcGateway *gw;
} ConnCtx;

static void *handle_conn(void *arg) {
    ConnCtx *cc = arg;
    int fd = cc->fd;
    NcGateway *gw = cc->gw;
    free(cc);

    char *buf = malloc(GW_BUF_SIZE);
    ssize_t n = recv(fd, buf, GW_BUF_SIZE - 1, 0);
    if (n <= 0) { free(buf); close(fd); return NULL; }
    buf[n] = '\0';

    GwRequest req;
    if (gw_parse_request(buf, (size_t)n, &req) < 0) {
        gw_json(fd, 400, "{\"error\":\"bad request\"}");
        free(buf); close(fd); return NULL;
    }
    free(buf);

    /* ── Rate limiting ── */
    /* (simple: always allow; hook into NcRateTracker if configured) */

    /* ── Route ── */
    if (strcmp(req.path, "/health") == 0) {
        gw_json(fd, 200, "{\"ok\":true}");

    } else if (strcmp(req.path, "/ready") == 0) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok", true);
        if (gw->agent) cJSON_AddBoolToObject(r, "agent", true);
        char *s = cJSON_Print(r); cJSON_Delete(r);
        gw_json(fd, 200, s); free(s);

    } else if (strcmp(req.path, "/pair") == 0 && strcmp(req.method, "POST") == 0) {
        cJSON *body = req.body ? cJSON_Parse(req.body) : NULL;
        cJSON *otp_j = body ? cJSON_GetObjectItemCaseSensitive(body, "otp") : NULL;
        const char *otp = (otp_j && cJSON_IsString(otp_j)) ? otp_j->valuestring : NULL;
        bool paired = nc_pairing_try(&gw->pairing, otp ? otp : "");
        if (body) cJSON_Delete(body);
        if (paired) {
            cJSON *r = cJSON_CreateObject();
            cJSON_AddStringToObject(r, "token", gw->pairing.bearer_token);
            char *s = cJSON_Print(r); cJSON_Delete(r);
            gw_json(fd, 200, s); free(s);
        } else {
            gw_json(fd, 403, "{\"error\":\"invalid OTP\"}");
        }

    } else if (strcmp(req.path, "/webhook") == 0 && strcmp(req.method, "POST") == 0) {
        /* Auth check */
        bool authed = true;
        if (gw->pairing.paired) {
            /* Expect: Authorization: Bearer <token> */
            const char *bearer = req.auth ? req.auth : "";
            if (strncasecmp(bearer, "Bearer ", 7) == 0) bearer += 7;
            authed = nc_pairing_validate_token(&gw->pairing, bearer);
        }
        if (!authed) { gw_json(fd, 401, "{\"error\":\"unauthorized\"}"); goto cleanup; }
        if (!req.body) { gw_json(fd, 400, "{\"error\":\"empty body\"}"); goto cleanup; }

        cJSON *body = cJSON_Parse(req.body);
        cJSON *msg_j = body ? cJSON_GetObjectItemCaseSensitive(body, "message") : NULL;
        const char *msg = (msg_j && cJSON_IsString(msg_j)) ? msg_j->valuestring : req.body;

        char *response = NULL;
        if (gw->agent) {
            NcArena *arena = nc_arena_new(2 * 1024 * 1024);
            response = nc_agent_turn(gw->agent, arena, msg);
            nc_arena_free(arena);
        } else {
            response = nc_strdup("(no agent configured)");
        }
        if (body) cJSON_Delete(body);

        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "response", response ? response : "");
        free(response);
        char *s = cJSON_Print(r); cJSON_Delete(r);
        gw_json(fd, 200, s); free(s);

    } else if (strcmp(req.path, "/metrics") == 0) {
        char metrics[256];
        snprintf(metrics, sizeof metrics,
                 "# HELP tiniclaw_up Gateway liveness\n"
                 "tiniclaw_up 1\n");
        gw_send(fd, 200, "text/plain", metrics);

    } else {
        gw_json(fd, 404, "{\"error\":\"not found\"}");
    }

cleanup:
    gw_request_free(&req);
    close(fd);
    return NULL;
}

/* ── NcGateway public API ────────────────────────────────────────── */

int nc_gateway_init(NcGateway *gw, NcConfig *cfg, NcAgent *agent) {
    memset(gw, 0, sizeof *gw);
    gw->cfg      = cfg;
    gw->agent    = agent;
    nc_pairing_init(&gw->pairing);
    gw->port     = (cfg && cfg->gateway.port) ? cfg->gateway.port : 7007;
    gw->running  = false;
    gw->server_fd = -1;
    return 0;
}

int nc_gateway_run(NcGateway *gw) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)gw->port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind"); close(fd); return -1;
    }
    if (listen(fd, GW_BACKLOG) < 0) {
        perror("listen"); close(fd); return -1;
    }

    gw->server_fd = fd;
    gw->running = true;
    fprintf(stderr, "tiniclaw gateway listening on :%d\n", gw->port);

    while (gw->running) {
        struct sockaddr_in client;
        socklen_t clen = sizeof client;
        int cfd = accept(fd, (struct sockaddr *)&client, &clen);
        if (cfd < 0) { if (gw->running) continue; break; }

        ConnCtx *cc = malloc(sizeof *cc);
        cc->fd = cfd; cc->gw = gw;
        pthread_t tid;
        pthread_create(&tid, NULL, handle_conn, cc);
        pthread_detach(tid);
    }

    close(fd);
    gw->server_fd = -1;
    return 0;
}

void nc_gateway_stop(NcGateway *gw) {
    gw->running = false;
    if (gw->server_fd >= 0) { close(gw->server_fd); gw->server_fd = -1; }
}
