/*
 * tiniclaw-c — provider vtable + HTTP helpers shared by all providers
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>
#include "provider.h"
#include "util.h"

/* ── cURL write callback ─────────────────────────────────────────── */

typedef struct CurlBuf {
    char  *data;
    size_t len;
    size_t cap;
} CurlBuf;

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *ud) {
    CurlBuf *b = (CurlBuf *)ud;
    size_t n = size * nmemb;
    if (b->len + n + 1 > b->cap) {
        size_t new_cap = b->cap ? b->cap * 2 : 4096;
        while (new_cap < b->len + n + 1) new_cap *= 2;
        char *p = realloc(b->data, new_cap);
        if (!p) return 0;
        b->data = p;
        b->cap  = new_cap;
    }
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

/* POST JSON to url, return body as heap string (caller frees), or NULL on error. */
char *nc_http_post_json(const char *url, const char *auth_header,
                         const char *body, long *http_code_out) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    CurlBuf buf = {0};
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (auth_header) headers = curl_slist_append(headers, auth_header);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    CURLcode res = curl_easy_perform(curl);
    if (http_code_out) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_code_out);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) { free(buf.data); return NULL; }
    return buf.data;   /* heap-allocated */
}

/* Free a ChatResponse's heap contents. */
void nc_chat_response_free(NcChatResponse *r) {
    if (!r) return;
    free(r->content);
    if (r->tool_calls) {
        for (size_t i = 0; i < r->tool_calls_count; i++) {
            free((char *)r->tool_calls[i].id);
            free((char *)r->tool_calls[i].name);
            free((char *)r->tool_calls[i].arguments);
        }
        free(r->tool_calls);
    }
    free(r->error_msg);
    memset(r, 0, sizeof *r);
}
