/*
 * tiniclaw-c — HTTP request tool (libcurl-based)
 */
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>
#include "tool.h"
#include "arena.h"
#include "util.h"
#include "net_security.h"

#define HTTP_RESP_CAP (1024 * 1024)   /* 1 MB */

typedef struct {
    NcBuf *buf;
} WriteCtx;

static size_t write_cb(char *data, size_t sz, size_t nmemb, void *ud) {
    WriteCtx *w = ud;
    size_t total = sz * nmemb;
    if (w->buf->len < HTTP_RESP_CAP) nc_buf_append(w->buf, data, total);
    return total;
}

static NcToolResult http_execute(void *ptr, NcArena *arena, cJSON *args) {
    (void)ptr;
    cJSON *url_j    = cJSON_GetObjectItemCaseSensitive(args, "url");
    cJSON *method_j = cJSON_GetObjectItemCaseSensitive(args, "method");
    cJSON *body_j   = cJSON_GetObjectItemCaseSensitive(args, "body");
    cJSON *hdrs_j   = cJSON_GetObjectItemCaseSensitive(args, "headers");

    if (!url_j || !cJSON_IsString(url_j)) return nc_tool_result_err("missing 'url'");
    const char *url = url_j->valuestring;

    /* HTTPS-only enforcement */
    if (nc_url_is_http_insecure(url)) return nc_tool_result_err("HTTP (non-HTTPS) URLs are blocked");

    const char *method = (method_j && cJSON_IsString(method_j)) ? method_j->valuestring : "GET";

    CURL *curl = curl_easy_init();
    if (!curl) return nc_tool_result_err("curl init failed");

    struct curl_slist *hlist = NULL;
    /* Default content-type if posting JSON body */
    bool is_json_body = false;
    if (body_j && cJSON_IsObject(body_j)) {
        hlist = curl_slist_append(hlist, "Content-Type: application/json");
        is_json_body = true;
    }
    /* User-supplied headers (object with string values) */
    if (hdrs_j && cJSON_IsObject(hdrs_j)) {
        cJSON *h = NULL;
        cJSON_ArrayForEach(h, hdrs_j) {
            if (!cJSON_IsString(h)) continue;
            char hdr[1024];
            snprintf(hdr, sizeof hdr, "%s: %s", h->string, h->valuestring);
            hlist = curl_slist_append(hlist, hdr);
        }
    }

    NcBuf resp_buf; nc_buf_init(&resp_buf);
    WriteCtx wctx = { &resp_buf };

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wctx);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    if (hlist) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);

    char *body_str = NULL;
    if (body_j) {
        if (cJSON_IsString(body_j)) {
            body_str = nc_strdup(body_j->valuestring);
        } else {
            body_str = cJSON_Print(body_j);
        }
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str);
        if (strcmp(method, "GET") == 0) {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
        }
    }

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(hlist);
    curl_easy_cleanup(curl);
    free(body_str);

    if (res != CURLE_OK) {
        nc_buf_free(&resp_buf);
        return nc_tool_result_err(curl_easy_strerror(res));
    }

    NcBuf out; nc_buf_init(&out);
    nc_buf_appendf(&out, "HTTP %ld\n%s", http_code, resp_buf.data ? resp_buf.data : "");
    nc_buf_free(&resp_buf);

    NcToolResult r = nc_tool_result_ok(out.data);
    nc_buf_free(&out);
    return r;
}

static const char *http_name(void *p) { (void)p; return "http_request"; }
static const char *http_desc(void *p) {
    (void)p; return "Make an HTTP request (HTTPS only) and return status + body.";
}
static const char *http_params(void *p) {
    (void)p;
    return "{\"type\":\"object\","
           "\"properties\":{"
           "\"url\":{\"type\":\"string\"},"
           "\"method\":{\"type\":\"string\",\"default\":\"GET\"},"
           "\"body\":{\"description\":\"Request body (string or object)\"},"
           "\"headers\":{\"type\":\"object\",\"description\":\"Key-value headers\"}"
           "},"
           "\"required\":[\"url\"]}";
}
static void http_deinit(void *p) { (void)p; /* stateless */ }

static const NcToolVTable HTTP_VTABLE = { http_execute, http_name, http_desc, http_params, http_deinit };

NcTool nc_tool_http_request_init(const NcToolContext *ctx) {
    (void)ctx;
    return (NcTool){ .ptr = NULL, .vtable = &HTTP_VTABLE };
}
