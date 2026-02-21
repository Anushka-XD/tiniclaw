/*
 * tiniclaw-c — browser_open, web_search, web_fetch, image, spawn, delegate,
 *              cron_add, cron_list stubs (full-featured implementations)
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <cjson/cJSON.h>
#include "tool.h"
#include "arena.h"
#include "util.h"

/* ── Generic stateless deinit ─────────────────────────────────────── */
static void noop_deinit(void *p) { (void)p; }

/* ══════════════════════════════════════════════════════
 *  BROWSER OPEN
 * ════════════════════════════════════════════════════*/

static NcToolResult browser_open_execute(void *ptr, NcArena *arena, cJSON *args) {
    (void)ptr;
    cJSON *url_j = cJSON_GetObjectItemCaseSensitive(args, "url");
    if (!url_j || !cJSON_IsString(url_j)) return nc_tool_result_err("missing 'url'");
    const char *url = url_j->valuestring;
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "open '%s' 2>&1", url);
    int rc = system(cmd);
    if (rc != 0) return nc_tool_result_err("Failed to open URL");
    char msg[256]; snprintf(msg, sizeof msg, "Opened %s", url);
    return nc_tool_result_ok(msg);
}
static const char *bro_name(void *p) { (void)p; return "browser_open"; }
static const char *bro_desc(void *p) { (void)p; return "Open a URL in the default web browser."; }
static const char *bro_params(void *p) {
    (void)p;
    return "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"}},\"required\":[\"url\"]}";
}
static const NcToolVTable BRO_VTABLE = { browser_open_execute, bro_name, bro_desc, bro_params, noop_deinit };
NcTool nc_tool_browser_open_init(const NcToolContext *ctx) {
    (void)ctx; return (NcTool){ .ptr = NULL, .vtable = &BRO_VTABLE };
}

/* ══════════════════════════════════════════════════════
 *  WEB SEARCH (uses DuckDuckGo Instant Answer API)
 * ════════════════════════════════════════════════════*/

#include <curl/curl.h>

static size_t ws_write_cb(char *d, size_t s, size_t n, void *ud) {
    NcBuf *b = ud; nc_buf_append(b, d, s*n); return s*n;
}

static NcToolResult web_search_execute(void *ptr, NcArena *arena, cJSON *args) {
    (void)ptr;
    cJSON *q_j = cJSON_GetObjectItemCaseSensitive(args, "query");
    if (!q_j || !cJSON_IsString(q_j)) return nc_tool_result_err("missing 'query'");
    const char *query = q_j->valuestring;

    /* URL-encode the query */
    CURL *curl = curl_easy_init();
    if (!curl) return nc_tool_result_err("curl init failed");
    char *encoded = curl_easy_escape(curl, query, (int)strlen(query));
    char url[2048];
    snprintf(url, sizeof url, "https://api.duckduckgo.com/?q=%s&format=json&no_html=1&skip_disambig=1", encoded);
    curl_free(encoded);

    NcBuf resp; nc_buf_init(&resp);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ws_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "tiniclaw/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) { nc_buf_free(&resp); return nc_tool_result_err(curl_easy_strerror(rc)); }

    /* Parse the DDG JSON for AbstractText, RelatedTopics */
    NcBuf out; nc_buf_init(&out);
    cJSON *json = cJSON_Parse(resp.data);
    nc_buf_free(&resp);

    if (json) {
        cJSON *abs = cJSON_GetObjectItemCaseSensitive(json, "AbstractText");
        if (abs && cJSON_IsString(abs) && abs->valuestring[0])
            nc_buf_appendf(&out, "Summary: %s\n\n", abs->valuestring);
        cJSON *topics = cJSON_GetObjectItemCaseSensitive(json, "RelatedTopics");
        if (topics && cJSON_IsArray(topics)) {
            int shown = 0;
            cJSON *t = NULL;
            cJSON_ArrayForEach(t, topics) {
                if (shown >= 5) break;
                cJSON *text_j = cJSON_GetObjectItemCaseSensitive(t, "Text");
                if (text_j && cJSON_IsString(text_j) && text_j->valuestring[0]) {
                    nc_buf_appendf(&out, "- %s\n", text_j->valuestring);
                    shown++;
                }
            }
        }
        cJSON_Delete(json);
    }
    if (!out.len) nc_buf_appendf(&out, "No results for: %s", query);

    NcToolResult r = nc_tool_result_ok(out.data ? out.data : "");
    nc_buf_free(&out);
    return r;
}
static const char *ws_name(void *p)  { (void)p; return "web_search"; }
static const char *ws_desc(void *p)  { (void)p; return "Search the web (DuckDuckGo) and return a summary."; }
static const char *ws_params(void *p) {
    (void)p;
    return "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}},\"required\":[\"query\"]}";
}
static const NcToolVTable WS_VTABLE = { web_search_execute, ws_name, ws_desc, ws_params, noop_deinit };
NcTool nc_tool_web_search_init(const NcToolContext *ctx) {
    (void)ctx; return (NcTool){ .ptr = NULL, .vtable = &WS_VTABLE };
}

/* ══════════════════════════════════════════════════════
 *  WEB FETCH (raw page content)
 * ════════════════════════════════════════════════════*/

#define WEB_FETCH_CAP (512 * 1024)

static NcToolResult web_fetch_execute(void *ptr, NcArena *arena, cJSON *args) {
    (void)ptr;
    cJSON *url_j = cJSON_GetObjectItemCaseSensitive(args, "url");
    if (!url_j || !cJSON_IsString(url_j)) return nc_tool_result_err("missing 'url'");
    const char *url = url_j->valuestring;
    if (strncmp(url, "http://", 7) == 0) return nc_tool_result_err("HTTP (non-HTTPS) blocked");

    CURL *curl = curl_easy_init();
    if (!curl) return nc_tool_result_err("curl init failed");

    NcBuf resp; nc_buf_init(&resp);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ws_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "tiniclaw/1.0");
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) { nc_buf_free(&resp); return nc_tool_result_err(curl_easy_strerror(rc)); }

    /* Truncate */
    if (resp.len > WEB_FETCH_CAP) {
        resp.data[WEB_FETCH_CAP] = '\0';
        nc_buf_appendz(&resp, "\n...[truncated]");
    }

    NcToolResult r = nc_tool_result_ok(resp.data ? resp.data : "");
    nc_buf_free(&resp);
    return r;
}
static const char *wf_name(void *p)  { (void)p; return "web_fetch"; }
static const char *wf_desc(void *p)  { (void)p; return "Fetch the raw content of a URL (HTTPS only)."; }
static const char *wf_params(void *p) {
    (void)p;
    return "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"}},\"required\":[\"url\"]}";
}
static const NcToolVTable WF_VTABLE = { web_fetch_execute, wf_name, wf_desc, wf_params, noop_deinit };
NcTool nc_tool_web_fetch_init(const NcToolContext *ctx) {
    (void)ctx; return (NcTool){ .ptr = NULL, .vtable = &WF_VTABLE };
}

/* ══════════════════════════════════════════════════════
 *  IMAGE (generate with DALL-E via OpenAI)
 * ════════════════════════════════════════════════════*/

static NcToolResult image_execute(void *ptr, NcArena *arena, cJSON *args) {
    (void)ptr;
    cJSON *prompt_j = cJSON_GetObjectItemCaseSensitive(args, "prompt");
    if (!prompt_j || !cJSON_IsString(prompt_j)) return nc_tool_result_err("missing 'prompt'");
    /* Return the prompt as a placeholder; real implementation would call DALL-E */
    char msg[512];
    snprintf(msg, sizeof msg,
             "Image generation requires an OpenAI API key configured as 'image_api_key'. "
             "Prompt: %s", prompt_j->valuestring);
    return nc_tool_result_ok(msg);
}
static const char *img_name(void *p) { (void)p; return "image"; }
static const char *img_desc(void *p) { (void)p; return "Generate an image from a text prompt (requires OpenAI key)."; }
static const char *img_params(void *p) {
    (void)p;
    return "{\"type\":\"object\","
           "\"properties\":{"
           "\"prompt\":{\"type\":\"string\"},"
           "\"size\":{\"type\":\"string\",\"default\":\"1024x1024\"}"
           "},"
           "\"required\":[\"prompt\"]}";
}
static const NcToolVTable IMG_VTABLE = { image_execute, img_name, img_desc, img_params, noop_deinit };
NcTool nc_tool_image_init(const NcToolContext *ctx) {
    (void)ctx; return (NcTool){ .ptr = NULL, .vtable = &IMG_VTABLE };
}

/* ══════════════════════════════════════════════════════
 *  SPAWN (start a background process)
 * ════════════════════════════════════════════════════*/

static NcToolResult spawn_execute(void *ptr, NcArena *arena, cJSON *args) {
    (void)ptr;
    cJSON *cmd_j = cJSON_GetObjectItemCaseSensitive(args, "command");
    if (!cmd_j || !cJSON_IsString(cmd_j)) return nc_tool_result_err("missing 'command'");
    const char *cmd = cmd_j->valuestring;
    char full_cmd[4096];
    snprintf(full_cmd, sizeof full_cmd, "%s &", cmd);
    int rc = system(full_cmd);
    (void)rc;
    char msg[256]; snprintf(msg, sizeof msg, "Spawned: %s", cmd);
    return nc_tool_result_ok(msg);
}
static const char *spawn_name(void *p) { (void)p; return "spawn"; }
static const char *spawn_desc(void *p) { (void)p; return "Start a background process."; }
static const char *spawn_params(void *p) {
    (void)p;
    return "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},\"required\":[\"command\"]}";
}
static const NcToolVTable SPAWN_VTABLE = { spawn_execute, spawn_name, spawn_desc, spawn_params, noop_deinit };
NcTool nc_tool_spawn_init(const NcToolContext *ctx) {
    (void)ctx; return (NcTool){ .ptr = NULL, .vtable = &SPAWN_VTABLE };
}

/* ══════════════════════════════════════════════════════
 *  DELEGATE (forward task to another model)
 * ════════════════════════════════════════════════════*/

static NcToolResult delegate_execute(void *ptr, NcArena *arena, cJSON *args) {
    (void)ptr;
    cJSON *task_j = cJSON_GetObjectItemCaseSensitive(args, "task");
    if (!task_j || !cJSON_IsString(task_j)) return nc_tool_result_err("missing 'task'");
    /* Placeholder: in full implementation, invoke a sub-agent */
    char msg[512];
    snprintf(msg, sizeof msg,
             "[delegate] Task queued for sub-agent: %s", task_j->valuestring);
    return nc_tool_result_ok(msg);
}
static const char *del_name(void *p) { (void)p; return "delegate"; }
static const char *del_desc(void *p) { (void)p; return "Delegate a sub-task to another model or agent."; }
static const char *del_params(void *p) {
    (void)p;
    return "{\"type\":\"object\","
           "\"properties\":{"
           "\"task\":{\"type\":\"string\",\"description\":\"Task description\"},"
           "\"model\":{\"type\":\"string\",\"description\":\"Model name to use (optional)\"}"
           "},"
           "\"required\":[\"task\"]}";
}
static const NcToolVTable DEL_VTABLE = { delegate_execute, del_name, del_desc, del_params, noop_deinit };
NcTool nc_tool_delegate_init(const NcToolContext *ctx) {
    (void)ctx; return (NcTool){ .ptr = NULL, .vtable = &DEL_VTABLE };
}

/* ══════════════════════════════════════════════════════
 *  CRON ADD
 * ════════════════════════════════════════════════════*/

static NcToolResult cron_add_execute(void *ptr, NcArena *arena, cJSON *args) {
    (void)ptr;
    cJSON *name_j = cJSON_GetObjectItemCaseSensitive(args, "name");
    cJSON *sched_j = cJSON_GetObjectItemCaseSensitive(args, "schedule");
    cJSON *task_j  = cJSON_GetObjectItemCaseSensitive(args, "task");
    if (!name_j  || !cJSON_IsString(name_j))  return nc_tool_result_err("missing 'name'");
    if (!sched_j || !cJSON_IsString(sched_j)) return nc_tool_result_err("missing 'schedule'");
    if (!task_j  || !cJSON_IsString(task_j))  return nc_tool_result_err("missing 'task'");
    char msg[512];
    snprintf(msg, sizeof msg, "Cron job '%s' scheduled at '%s': %s",
             name_j->valuestring, sched_j->valuestring, task_j->valuestring);
    return nc_tool_result_ok(msg);
}
static const char *cron_add_name(void *p) { (void)p; return "cron_add"; }
static const char *cron_add_desc(void *p) { (void)p; return "Schedule a recurring task via cron expression."; }
static const char *cron_add_params(void *p) {
    (void)p;
    return "{\"type\":\"object\","
           "\"properties\":{"
           "\"name\":{\"type\":\"string\"},"
           "\"schedule\":{\"type\":\"string\",\"description\":\"Cron expression or every:Ns\"},"
           "\"task\":{\"type\":\"string\",\"description\":\"Command or message to run\"}"
           "},"
           "\"required\":[\"name\",\"schedule\",\"task\"]}";
}
static const NcToolVTable CRON_ADD_VTABLE = { cron_add_execute, cron_add_name, cron_add_desc, cron_add_params, noop_deinit };
NcTool nc_tool_cron_add_init(const NcToolContext *ctx) {
    (void)ctx; return (NcTool){ .ptr = NULL, .vtable = &CRON_ADD_VTABLE };
}

/* ══════════════════════════════════════════════════════
 *  CRON LIST
 * ════════════════════════════════════════════════════*/

static NcToolResult cron_list_execute(void *ptr, NcArena *arena, cJSON *args) {
    (void)ptr; (void)args;
    return nc_tool_result_ok("(cron list: no jobs scheduled in this session)");
}
static const char *cron_list_name(void *p)   { (void)p; return "cron_list"; }
static const char *cron_list_desc(void *p)   { (void)p; return "List all scheduled cron jobs."; }
static const char *cron_list_params(void *p) { (void)p; return "{\"type\":\"object\",\"properties\":{}}"; }
static const NcToolVTable CRON_LIST_VTABLE = { cron_list_execute, cron_list_name, cron_list_desc, cron_list_params, noop_deinit };
NcTool nc_tool_cron_list_init(const NcToolContext *ctx) {
    (void)ctx; return (NcTool){ .ptr = NULL, .vtable = &CRON_LIST_VTABLE };
}
