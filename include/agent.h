#pragma once
/*
 * tiniclaw-c — agent orchestration
 * Mirrors tiniclaw's Agent struct from agent/root.zig
 */
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "arena.h"
#include "provider.h"
#include "tool.h"
#include "memory.h"
#include "security.h"
#include "config.h"

#define NC_DEFAULT_MAX_TOOL_ITERATIONS  25
#define NC_DEFAULT_MAX_HISTORY          50

/* ── Owned message (conversation history) ────────────────────────── */

typedef struct NcOwnedMessage {
    NcRole  role;
    char   *content;   /* heap-allocated */
} NcOwnedMessage;

/* ── Stream callback for partial output ──────────────────────────── */
typedef bool (*NcAgentStreamCb)(const char *chunk, void *ctx);

/* ── Agent struct ────────────────────────────────────────────────── */

typedef struct NcAgent {
    NcProvider      provider;
    NcTool         *tools;
    size_t          tools_count;
    NcMemory        memory;          /* NcMemory.ptr == NULL → disabled */
    NcSecurityPolicy *policy;        /* NULL → no policy */
    NcAuditLogger   *audit;          /* NULL → no audit */

    char   model_name[128];
    double temperature;
    char   workspace_dir[4096];

    uint32_t max_tool_iterations;
    uint32_t max_history_messages;
    bool     auto_save;
    uint64_t token_limit;            /* 0 = unlimited */
    uint32_t max_tokens;             /* 0 = provider default */
    char     reasoning_effort[16];
    uint64_t message_timeout_secs;   /* 0 = none */

    /* Compaction settings */
    uint32_t compaction_keep_recent;
    uint32_t compaction_max_summary_chars;
    uint32_t compaction_max_source_chars;

    /* Streaming */
    NcAgentStreamCb stream_callback;
    void           *stream_ctx;

    /* Conversation history */
    NcOwnedMessage *history;
    size_t          history_len;
    size_t          history_cap;

    /* Stats */
    uint64_t total_tokens;
    bool     has_system_prompt;
    bool     last_turn_compacted;
} NcAgent;

/* ── Lifecycle ───────────────────────────────────────────────────── */

/* Initialize agent from config. provider, tools, memory must already be created. */
int  nc_agent_init(NcAgent *agent, const NcConfig *cfg,
                   NcProvider provider, NcTool *tools, size_t tools_count,
                   NcMemory memory);

void nc_agent_deinit(NcAgent *agent);

/* ── Turn / chat ─────────────────────────────────────────────────── */

/* Run one agent turn. `user_input` → assistant response + tool calls.
   Returns heap-allocated response string; caller frees.
   Returns NULL on error. */
char *nc_agent_turn(NcAgent *agent, NcArena *arena, const char *user_input);

/* Reset conversation history (keep agent config). */
void nc_agent_reset_history(NcAgent *agent);

/* ── CLI entry point ─────────────────────────────────────────────── */

/* Interactive REPL loop or one-shot mode.
   argc/argv are the sub_args after "agent". */
int nc_agent_run(const NcConfig *cfg, int argc, const char **argv);

/* ── Prompt building ─────────────────────────────────────────────── */

/* Build the system prompt from workspace identity files + tools.
   Returns heap-allocated string; caller frees. */
char *nc_agent_build_system_prompt(NcArena *arena, const char *workspace_dir,
                                    const NcTool *tools, size_t tools_count,
                                    const char *model_name);

/* ── Tool dispatch ───────────────────────────────────────────────── */

typedef struct NcToolCallResult {
    char *tool_call_id;  /* heap-allocated */
    char *tool_name;     /* heap-allocated */
    char *output;        /* heap-allocated */
    bool  success;
} NcToolCallResult;

/* Execute all tool calls from a response.
   Returns heap-allocated array; caller frees each entry + array. */
NcToolCallResult *nc_agent_dispatch_tools(NcAgent *agent, NcArena *arena,
                                           const NcToolCall *calls, size_t count,
                                           size_t *out_count);

/* ── History compaction ──────────────────────────────────────────── */

/* Trim history to max_history_messages. */
void nc_agent_trim_history(NcAgent *agent);
