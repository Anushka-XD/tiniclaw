#pragma once
/*
 * tiniclaw-c — gateway (HTTP server), cron scheduler, tunnel manager, MCP client
 */
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include "arena.h"
#include "config.h"
#include "agent.h"
#include "security.h"

/* ══════════════════════════════════════════════════════════════════
   Gateway — lightweight HTTP server
   Endpoints: /health  /ready  /pair  /webhook  /whatsapp  /telegram
   ══════════════════════════════════════════════════════════════════ */

#define NC_GATEWAY_MAX_BODY   65536   /* 64 KB */
#define NC_GATEWAY_TIMEOUT_S  30

typedef struct NcGateway {
    NcConfig         *cfg;
    NcAgent          *agent;
    NcPairingGuard    pairing;
    NcRateTracker    *rate_limiter;
    NcAuditLogger    *audit;
    uint16_t          port;
    char              host[128];
    int               server_fd;
    bool              running;
} NcGateway;

int  nc_gateway_init(NcGateway *gw, NcConfig *cfg, NcAgent *agent);
int  nc_gateway_run(NcGateway *gw);   /* blocks */
void nc_gateway_stop(NcGateway *gw);
void nc_gateway_deinit(NcGateway *gw);

/* Entry point for "tiniclaw gateway" command */
int nc_gateway_cmd(NcConfig *cfg, int argc, const char **argv);

/* ══════════════════════════════════════════════════════════════════
   Cron scheduler
   Supports: cron expressions, `at` (one-shot), `every` (interval)
   State persisted to JSON file
   ══════════════════════════════════════════════════════════════════ */

typedef enum NcJobType { NC_JOB_SHELL, NC_JOB_AGENT } NcJobType;
typedef enum NcScheduleKind { NC_SCHED_CRON, NC_SCHED_AT, NC_SCHED_EVERY } NcScheduleKind;
typedef enum NcDeliveryMode { NC_DELIVERY_NONE, NC_DELIVERY_ALWAYS,
                               NC_DELIVERY_ON_ERROR, NC_DELIVERY_ON_SUCCESS } NcDeliveryMode;

typedef struct NcCronJob {
    uint64_t       id;
    char           name[128];
    NcJobType      type;
    char           command[1024];
    NcScheduleKind schedule_kind;
    char           cron_expr[64];    /* for NC_SCHED_CRON */
    int64_t        at_timestamp;     /* for NC_SCHED_AT */
    uint64_t       every_ms;         /* for NC_SCHED_EVERY */
    char           timezone[64];
    bool           enabled;
    NcDeliveryMode delivery_mode;
    char           delivery_channel[64];
    char           delivery_to[128];
    int64_t        last_run;
    int64_t        next_run;
} NcCronJob;

typedef struct NcCronRun {
    uint64_t job_id;
    uint64_t run_id;
    int64_t  started_at;
    int64_t  finished_at;
    bool     success;
    char    *output;    /* heap-allocated */
} NcCronRun;

typedef struct NcCronScheduler {
    NcCronJob *jobs;
    size_t     jobs_count;
    size_t     jobs_cap;
    char       jobs_path[4096];
    NcAgent   *agent;
    bool       running;
    pthread_t  thread;
} NcCronScheduler;

int  nc_cron_init(NcCronScheduler *s, NcConfig *cfg, NcAgent *agent);
void nc_cron_deinit(NcCronScheduler *s);
int  nc_cron_start(NcCronScheduler *s);   /* spawns background thread */
void nc_cron_stop(NcCronScheduler *s);

int  nc_cron_add(NcCronScheduler *s, const NcCronJob *job);
int  nc_cron_remove(NcCronScheduler *s, uint64_t job_id);
int  nc_cron_update(NcCronScheduler *s, uint64_t job_id, const NcCronJob *updated);
NcCronJob *nc_cron_list(NcCronScheduler *s, size_t *count_out);

/* Parse a cron expression. Returns true if valid and sets next_run. */
bool nc_cron_next_run(const char *expr, const char *tz, int64_t after, int64_t *out);

/* "tiniclaw cron" command handler */
int nc_cron_cmd(NcConfig *cfg, int argc, const char **argv);

/* ══════════════════════════════════════════════════════════════════
   Tunnel manager — cloudflared / ngrok / tailscale / custom binary
   ══════════════════════════════════════════════════════════════════ */

typedef enum NcTunnelState {
    NC_TUNNEL_STOPPED,
    NC_TUNNEL_STARTING,
    NC_TUNNEL_RUNNING,
    NC_TUNNEL_ERROR,
} NcTunnelState;

typedef struct NcTunnelAdapter {
    void                   *ptr;
    const struct NcTunnelVTable *vtable;
} NcTunnelAdapter;

typedef struct NcTunnelVTable {
    int           (*start)(void *self);
    void          (*stop)(void *self);
    const char   *(*get_url)(void *self);
    NcTunnelState (*state)(void *self);
    const char   *(*name)(void *self);
    void          (*deinit)(void *self);
} NcTunnelVTable;

NcTunnelAdapter nc_tunnel_create(NcArena *arena, const NcTunnelConfig *cfg);

/* ══════════════════════════════════════════════════════════════════
   MCP — Model Context Protocol stdio client (JSON-RPC 2.0)
   ══════════════════════════════════════════════════════════════════ */

typedef struct NcMcpTool {
    char name[128];
    char description[512];
    char input_schema[4096];
} NcMcpTool;

typedef struct NcMcpServer {
    NcMcpServerConfig  cfg;
    pid_t              pid;
    int                stdin_fd;
    int                stdout_fd;
    int                stderr_fd;
    uint32_t           next_id;
    NcMcpTool         *tools;
    size_t             tools_count;
} NcMcpServer;

int  nc_mcp_server_connect(NcMcpServer *s, NcArena *arena);
void nc_mcp_server_disconnect(NcMcpServer *s);

/* Call a tool on the MCP server. Returns heap-allocated JSON result; caller frees. */
char *nc_mcp_call_tool(NcMcpServer *s, NcArena *arena,
                        const char *tool_name, const char *args_json);

/* Wrap all MCP tools into NcTool vtable entries.
   Returns heap-allocated array; tools_count set. Caller frees. */
NcTool *nc_mcp_wrap_tools(NcArena *arena, NcMcpServer *servers,
                            size_t servers_count, size_t *tools_count);
