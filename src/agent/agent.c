/*
 * tiniclaw-c — agent orchestration loop
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <cjson/cJSON.h>
#include "agent.h"
#include "util.h"
#include "platform.h"

/* ── History management ─────────────────────────────────────────── */

static int history_push(NcAgent *a, NcRole role, const char *content) {
    if (a->history_len >= a->history_cap) {
        size_t new_cap = a->history_cap ? a->history_cap * 2 : 32;
        NcOwnedMessage *p = realloc(a->history, sizeof(NcOwnedMessage) * new_cap);
        if (!p) return -1;
        a->history = p;
        a->history_cap = new_cap;
    }
    a->history[a->history_len].role    = role;
    a->history[a->history_len].content = nc_strdup(content ? content : "");
    a->history_len++;
    return 0;
}

void nc_agent_trim_history(NcAgent *a) {
    uint32_t max = a->max_history_messages;
    if (!max || a->history_len <= (size_t)max) return;
    size_t remove = a->history_len - (size_t)max;
    for (size_t i = 0; i < remove; i++) free(a->history[i].content);
    memmove(a->history, a->history + remove,
            sizeof(NcOwnedMessage) * (a->history_len - remove));
    a->history_len -= remove;
}

/* ── Init / deinit ──────────────────────────────────────────────── */

int nc_agent_init(NcAgent *agent, const NcConfig *cfg,
                  NcProvider provider, NcTool *tools, size_t tools_count,
                  NcMemory memory) {
    memset(agent, 0, sizeof *agent);
    agent->provider     = provider;
    agent->tools        = tools;
    agent->tools_count  = tools_count;
    agent->memory       = memory;

    snprintf(agent->model_name,    sizeof agent->model_name,    "%s",
             cfg->default_model[0] ? cfg->default_model : "anthropic/claude-sonnet-4");
    agent->temperature = cfg->default_temperature > 0 ? cfg->default_temperature : 0.7;
    snprintf(agent->workspace_dir, sizeof agent->workspace_dir, "%s", cfg->workspace_dir);

    agent->max_tool_iterations    = cfg->agent.max_tool_iterations    ? cfg->agent.max_tool_iterations    : 25;
    agent->max_history_messages   = cfg->agent.max_history_messages   ? cfg->agent.max_history_messages   : 50;
    agent->auto_save              = cfg->memory.auto_save;
    agent->token_limit            = cfg->agent.token_limit;
    agent->max_tokens             = cfg->max_tokens;
    agent->message_timeout_secs   = cfg->agent.message_timeout_secs;
    agent->compaction_keep_recent = cfg->agent.compaction_keep_recent ? cfg->agent.compaction_keep_recent : 10;

    snprintf(agent->reasoning_effort, sizeof agent->reasoning_effort, "%s",
             cfg->reasoning_effort[0] ? cfg->reasoning_effort : "");
    return 0;
}

void nc_agent_deinit(NcAgent *agent) {
    for (size_t i = 0; i < agent->history_len; i++) free(agent->history[i].content);
    free(agent->history);
    nc_provider_deinit(agent->provider);
    if (agent->memory.ptr) nc_memory_deinit(agent->memory);
    memset(agent, 0, sizeof *agent);
}

void nc_agent_reset_history(NcAgent *agent) {
    for (size_t i = 0; i < agent->history_len; i++) free(agent->history[i].content);
    agent->history_len = 0;
    agent->has_system_prompt = false;
}

/* ── System prompt ──────────────────────────────────────────────── */

char *nc_agent_build_system_prompt(NcArena *arena, const char *workspace_dir,
                                    const NcTool *tools, size_t tools_count,
                                    const char *model_name) {
    NcBuf buf; nc_buf_init(&buf);

    nc_buf_appendf(&buf, "## Project Context\n\n");
    nc_buf_appendf(&buf, "Working directory: `%s`\n\n", workspace_dir);

    /* Inject identity files */
    static const char *IDENTITY_FILES[] = {
        "AGENTS.md","SOUL.md","TOOLS.md","IDENTITY.md",
        "USER.md","HEARTBEAT.md","BOOTSTRAP.md","MEMORY.md", NULL
    };
    for (int i = 0; IDENTITY_FILES[i]; i++) {
        char path[4096];
        snprintf(path, sizeof path, "%s/%s", workspace_dir, IDENTITY_FILES[i]);
        size_t flen = 0;
        char *content = nc_read_file(path, &flen);
        if (!content) continue;
        nc_buf_appendf(&buf, "### %s\n\n%s\n\n", IDENTITY_FILES[i], content);
        free(content);
    }

    nc_buf_appendz(&buf, "## Tools\n\n");
    for (size_t i = 0; i < tools_count; i++) {
        nc_buf_appendf(&buf, "- **%s**: %s\n",
                        nc_tool_name(tools[i]), nc_tool_desc(tools[i]));
    }
    nc_buf_appendz(&buf, "\n");

    nc_buf_appendz(&buf,
        "## Safety\n\n"
        "- Do not exfiltrate private data.\n"
        "- Do not run destructive commands without asking.\n"
        "- Do not bypass oversight or approval mechanisms.\n"
        "- Prefer trash over rm.\n"
        "- When in doubt, ask before acting externally.\n\n");

    char ts[32]; nc_iso8601_now(ts, sizeof ts);
    nc_buf_appendf(&buf, "## Runtime\n\nOS: macOS | Model: %s | Time: %s\n\n",
                    model_name, ts);

    char *result = nc_arena_strdup(arena, buf.data ? buf.data : "");
    nc_buf_free(&buf);
    return result;
}

/* ── Tool dispatch ──────────────────────────────────────────────── */

NcToolCallResult *nc_agent_dispatch_tools(NcAgent *agent, NcArena *arena,
                                           const NcToolCall *calls, size_t count,
                                           size_t *out_count) {
    *out_count = 0;
    if (!count) return NULL;
    NcToolCallResult *results = calloc(count, sizeof(NcToolCallResult));
    if (!results) return NULL;

    for (size_t i = 0; i < count; i++) {
        const char *name = calls[i].name;
        const char *args_json = calls[i].arguments;

        /* Find the tool */
        NcTool *found = NULL;
        for (size_t j = 0; j < agent->tools_count; j++) {
            if (strcmp(nc_tool_name(agent->tools[j]), name) == 0) {
                found = &agent->tools[j];
                break;
            }
        }

        results[i].tool_call_id = nc_strdup(calls[i].id ? calls[i].id : "");
        results[i].tool_name    = nc_strdup(name ? name : "");

        if (!found) {
            results[i].success  = false;
            results[i].output   = nc_strdup("Tool not found");
            continue;
        }

        cJSON *args = cJSON_Parse(args_json ? args_json : "{}");
        NcToolResult tr = nc_tool_execute(*found, arena, args);
        if (args) cJSON_Delete(args);

        results[i].success = tr.success;
        results[i].output  = tr.success ?
            nc_strdup(tr.output ? tr.output : "") :
            nc_strdup(tr.error_msg ? tr.error_msg : "Tool error");
        free(tr.output);
        free(tr.error_msg);
    }

    *out_count = count;
    return results;
}

/* ── Main turn loop ─────────────────────────────────────────────── */

char *nc_agent_turn(NcAgent *agent, NcArena *arena, const char *user_input) {
    /* Inject system prompt on first turn */
    if (!agent->has_system_prompt) {
        char *sys = nc_agent_build_system_prompt(arena, agent->workspace_dir,
                                                  agent->tools, agent->tools_count,
                                                  agent->model_name);
        history_push(agent, NC_ROLE_SYSTEM, sys);
        agent->has_system_prompt = true;
    }

    history_push(agent, NC_ROLE_USER, user_input);

    /* Build tool specs */
    NcToolSpec *specs = calloc(agent->tools_count, sizeof(NcToolSpec));
    for (size_t i = 0; i < agent->tools_count; i++) {
        specs[i].name             = nc_tool_name(agent->tools[i]);
        specs[i].description      = nc_tool_desc(agent->tools[i]);
        specs[i].parameters_json  = nc_tool_params(agent->tools[i]);
    }

    /* Agentic tool-use loop */
    char *final_response = NULL;
    for (uint32_t iter = 0; iter < agent->max_tool_iterations; iter++) {
        /* Build messages array from history */
        NcMessage *msgs = calloc(agent->history_len, sizeof(NcMessage));
        for (size_t i = 0; i < agent->history_len; i++) {
            msgs[i].role    = agent->history[i].role;
            msgs[i].content = agent->history[i].content;
        }

        NcChatRequest req = {
            .model          = agent->model_name,
            .temperature    = agent->temperature,
            .max_tokens     = agent->max_tokens,
            .messages       = msgs,
            .messages_count = agent->history_len,
            .tools          = specs,
            .tools_count    = nc_provider_supports_tools(agent->provider) ? agent->tools_count : 0,
        };

        NcChatResponse resp;
        int rc = nc_provider_chat(agent->provider, arena, &req, &resp);
        free(msgs);

        if (rc != 0 || !resp.ok) {
            free(specs);
            char *err = nc_strdup(resp.error_msg ? resp.error_msg : "Provider error");
            nc_chat_response_free(&resp);
            return err;
        }

        /* No tool calls → final response */
        if (resp.tool_calls_count == 0) {
            final_response = nc_strdup(resp.content ? resp.content : "");
            history_push(agent, NC_ROLE_ASSISTANT, final_response);
            nc_chat_response_free(&resp);
            break;
        }

        /* Add assistant turn with tool calls to history (simplified: just content) */
        if (resp.content) history_push(agent, NC_ROLE_ASSISTANT, resp.content);

        /* Execute tools */
        size_t result_count = 0;
        NcToolCallResult *tool_results = nc_agent_dispatch_tools(
            agent, arena, resp.tool_calls, resp.tool_calls_count, &result_count);
        nc_chat_response_free(&resp);

        /* Add tool results to history */
        for (size_t i = 0; i < result_count; i++) {
            NcBuf tb; nc_buf_init(&tb);
            nc_buf_appendf(&tb, "[%s] %s", tool_results[i].tool_name,
                            tool_results[i].output ? tool_results[i].output : "");
            history_push(agent, NC_ROLE_TOOL, tb.data);
            nc_buf_free(&tb);
            free(tool_results[i].tool_call_id);
            free(tool_results[i].tool_name);
            free(tool_results[i].output);
        }
        free(tool_results);

        nc_agent_trim_history(agent);
    }

    free(specs);

    /* Auto-save memory */
    if (agent->auto_save && agent->memory.ptr && user_input && final_response) {
        NcMemoryCategoryRef cat = { .kind = NC_MEM_CONVERSATION };
        nc_memory_store(agent->memory, arena, user_input, final_response, cat, NULL);
    }

    return final_response ? final_response : nc_strdup("(no response)");
}

/* ── CLI entry point ─────────────────────────────────────────────── */

int nc_agent_run(const NcConfig *cfg, int argc, const char **argv) {
    NcArena *arena = nc_arena_new(4 * 1024 * 1024);  /* 4 MB per-session */
    if (!arena) { fprintf(stderr, "OOM\n"); return 1; }

    /* Resolve provider */
    const char *provider_name = cfg->default_provider;
    const char *api_key = nc_config_get_provider_key(cfg, provider_name);
    const char *base_url = nc_config_get_provider_url(cfg, provider_name);
    NcProvider provider = nc_provider_create(arena, provider_name, api_key, base_url);
    if (!provider.ptr) {
        fprintf(stderr, "Failed to create provider '%s'\n", provider_name);
        nc_arena_free(arena);
        return 1;
    }

    /* Tools */
    NcToolContext tool_ctx = { .workspace_dir = cfg->workspace_dir };
    size_t tools_count = 0;
    NcTool *tools = nc_tools_default(arena, &tool_ctx, &tools_count);

    /* Memory */
    NcMemory memory;
    if (strcmp(cfg->memory.backend, "sqlite") == 0) {
        char db_path[4096];
        snprintf(db_path, sizeof db_path, "%s/memory.db",
                 cfg->workspace_dir[0] ? cfg->workspace_dir : ".");
        NcSqliteMemoryConfig mcfg = {
            .db_path        = db_path,
            .vector_weight  = cfg->memory.vector_weight,
            .keyword_weight = cfg->memory.keyword_weight,
            .hygiene_enabled = cfg->memory.hygiene_enabled,
        };
        memory = nc_memory_sqlite_init(&mcfg);
    } else {
        memory = nc_memory_none_init();
    }

    NcAgent agent;
    nc_agent_init(&agent, cfg, provider, tools, tools_count, memory);

    /* Parse flags */
    const char *one_shot_msg = NULL;
    bool non_interactive = false;
    for (int i = 0; i < argc; i++) {
        if ((strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--message") == 0) && i + 1 < argc) {
            one_shot_msg = argv[++i];
            non_interactive = true;
        }
    }

    if (non_interactive && one_shot_msg) {
        char *resp = nc_agent_turn(&agent, arena, one_shot_msg);
        printf("%s\n", resp ? resp : "(no response)");
        free(resp);
    } else {
        /* REPL */
        printf("tiniclaw agent — type 'exit' or Ctrl-D to quit\n");
        char line[8192];
        while (1) {
            printf("> ");
            fflush(stdout);
            if (!fgets(line, sizeof line, stdin)) break;
            /* Strip newline */
            size_t n = strlen(line);
            if (n > 0 && line[n-1] == '\n') line[n-1] = '\0';
            if (!line[0]) continue;
            if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) break;
            nc_arena_reset(arena);
            char *resp = nc_agent_turn(&agent, arena, line);
            printf("\n%s\n\n", resp ? resp : "(no response)");
            free(resp);
        }
    }

    nc_agent_deinit(&agent);
    nc_arena_free(arena);
    return 0;
}
