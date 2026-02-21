/*
 * tiniclaw-c — main CLI entry point
 *
 * Commands:
 *   agent     [--message|-m MSG]   Start interactive agent session or one-shot
 *   gateway   [--port N]           HTTP gateway server
 *   daemon                         Background agent daemon
 *   status                         Show current configuration
 *   version                        Print version
 *   onboard                        Interactive setup wizard
 *   doctor                         Run diagnostics
 *   cron      list|add|remove      Manage cron jobs
 *   channel   list                 List configured channels
 *   models    list                 List known models
 *   help                           Show usage
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include "tiniclaw.h"
#include "config.h"
#include "agent.h"
#include "gateway.h"
#include "arena.h"
#include "platform.h"
#include "version.h"

/* ── Global gateway for signal handler ─────────────────────────── */
static NcGateway *g_gateway = NULL;

static void sig_handler(int sig) {
    (void)sig;
    if (g_gateway) nc_gateway_stop(g_gateway);
}

/* ── Usage ──────────────────────────────────────────────────────── */

static void print_usage(void) {
    printf(
        "tiniclaw %s\n\n"
        "USAGE: tiniclaw <command> [options]\n\n"
        "COMMANDS:\n"
        "  agent     [-m MSG]       Interactive agent (or one-shot with -m)\n"
        "  gateway   [--port N]     Start HTTP gateway server\n"
        "  daemon                   Run agent as background daemon\n"
        "  status                   Show configuration summary\n"
        "  version                  Print version string\n"
        "  onboard                  Interactive setup wizard\n"
        "  doctor                   Run health diagnostics\n"
        "  cron      list|add|rm    Manage scheduled jobs\n"
        "  channel   list           List configured channels\n"
        "  models    list           List known provider models\n"
        "  help                     Show this help\n",
        TINICLAW_VERSION
    );
}

/* ── Config loading ─────────────────────────────────────────────── */

static NcConfig *load_config(void) {
    NcConfig *cfg = calloc(1, sizeof *cfg);
    nc_config_apply_defaults(cfg);

    /* Try ~/.tiniclaw/config.json, then ./config.json */
    char home[4096]; nc_home_dir(home, sizeof home);
    char path1[4096];
    snprintf(path1, sizeof path1, "%s/.tiniclaw/config.json", home);
    if (nc_file_exists(path1)) {
        nc_config_load(cfg);  /* loads from default ~/.tiniclaw/config.json */
    } else if (nc_file_exists("config.json")) {
        nc_config_load(cfg);
    }

    /* Override from environment */
    const char *openai_key    = nc_getenv("OPENAI_API_KEY");
    const char *anthropic_key = nc_getenv("ANTHROPIC_API_KEY");
    const char *model_env     = nc_getenv("TINICLAW_MODEL");
    const char *provider_env  = nc_getenv("TINICLAW_PROVIDER");

    if (openai_key && openai_key[0] && !cfg->providers_count) {
        snprintf(cfg->providers[0].name,    sizeof cfg->providers[0].name,    "openai");
        snprintf(cfg->providers[0].api_key, sizeof cfg->providers[0].api_key, "%s", openai_key);
        cfg->providers_count = 1;
        if (!cfg->default_provider[0]) snprintf(cfg->default_provider, sizeof cfg->default_provider, "openai");
    }
    if (anthropic_key && anthropic_key[0] && !cfg->providers_count) {
        snprintf(cfg->providers[0].name,    sizeof cfg->providers[0].name,    "anthropic");
        snprintf(cfg->providers[0].api_key, sizeof cfg->providers[0].api_key, "%s", anthropic_key);
        cfg->providers_count = 1;
        if (!cfg->default_provider[0]) snprintf(cfg->default_provider, sizeof cfg->default_provider, "anthropic");
    }
    if (model_env    && model_env[0])    snprintf(cfg->default_model,    sizeof cfg->default_model,    "%s", model_env);
    if (provider_env && provider_env[0]) snprintf(cfg->default_provider, sizeof cfg->default_provider, "%s", provider_env);

    return cfg;
}

/* ── Sub-commands ────────────────────────────────────────────────── */

static int cmd_agent(NcConfig *cfg, int argc, const char **argv) {
    return nc_agent_run(cfg, argc, argv);
}

static int cmd_gateway(NcConfig *cfg, int argc, const char **argv) {
    /* --port N override */
    for (int i = 0; i < argc - 1; i++) {
        if (strcmp(argv[i], "--port") == 0 || strcmp(argv[i], "-p") == 0) {
            cfg->gateway.port = atoi(argv[i+1]);
        }
    }

    /* Build agent for gateway */
    NcArena *arena = nc_arena_new(4 * 1024 * 1024);
    const char *pname = cfg->default_provider;
    const char *api_key  = nc_config_get_provider_key(cfg, pname);
    const char *base_url = nc_config_get_provider_url(cfg, pname);
    NcProvider provider = nc_provider_create(arena, pname, api_key, base_url);

    NcToolContext tool_ctx = { .workspace_dir = cfg->workspace_dir };
    size_t tools_count = 0;
    NcTool *tools = nc_tools_default(arena, &tool_ctx, &tools_count);

    NcMemory memory = nc_memory_none_init();
    if (strcmp(cfg->memory.backend, "sqlite") == 0) {
        char db_path[4096];
        snprintf(db_path, sizeof db_path, "%s/memory.db",
                 cfg->workspace_dir[0] ? cfg->workspace_dir : ".");
        NcSqliteMemoryConfig mcfg = { .db_path = db_path };
        memory = nc_memory_sqlite_init(&mcfg);
    }

    static NcAgent s_agent;
    nc_agent_init(&s_agent, cfg, provider, tools, tools_count, memory);

    static NcPairingGuard s_pairing;
    nc_pairing_init(&s_pairing);

    static NcGateway s_gateway;
    nc_gateway_init(&s_gateway, cfg, &s_agent);
    s_gateway.pairing = s_pairing;
    g_gateway = &s_gateway;

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    int rc = nc_gateway_run(&s_gateway);
    nc_agent_deinit(&s_agent);
    nc_arena_free(arena);
    return rc;
}

static int cmd_cron(NcConfig *cfg, int argc, const char **argv) {
    static NcCronScheduler s;
    nc_cron_init(&s, cfg, NULL);
    if (argc == 0 || strcmp(argv[0], "list") == 0) {
        size_t count = 0;
        NcCronJob *jobs = nc_cron_list(&s, &count);
        printf("%-20s %-30s %s\n", "Name", "Schedule", "Command");
        for (size_t i = 0; i < count; i++)
            printf("%-20s %-30s %s\n", jobs[i].name, jobs[i].cron_expr, jobs[i].command);
    } else if (argc >= 3 && strcmp(argv[0], "add") == 0) {
        /* add <name> <schedule> <command> */
        NcCronJob job = {0};
        snprintf(job.name,      sizeof job.name,      "%s", argv[1]);
        snprintf(job.cron_expr, sizeof job.cron_expr, "%s", argv[2]);
        snprintf(job.command,   sizeof job.command,   "%s", argc > 3 ? argv[3] : "echo tick");
        job.type          = NC_JOB_SHELL;
        job.schedule_kind = NC_SCHED_CRON;
        job.enabled       = true;
        nc_cron_add(&s, &job);
        printf("Added job '%s'\n", argv[1]);
        nc_cron_start(&s);
        printf("Press Ctrl-C to stop.\n");
        pause();
    } else {
        printf("cron usage: tiniclaw cron list|add <name> <schedule> <cmd>\n");
    }
    nc_cron_deinit(&s);
    return 0;
}

static int cmd_channel(NcConfig *cfg, int argc, const char **argv) {
    (void)cfg; (void)argc; (void)argv;
    printf("Configured channels:\n");
    if (cfg->channels.telegram.bot_token[0])   printf("  telegram\n");
    if (cfg->channels.discord.bot_token[0])    printf("  discord\n");
    if (cfg->channels.slack.bot_token[0])      printf("  slack\n");
    if (cfg->channels.matrix.access_token[0])  printf("  matrix\n");
    if (cfg->channels.irc.host[0])             printf("  irc\n");
    if (cfg->channels.email.smtp_url[0])       printf("  email\n");
    if (cfg->channels.imessage_enabled)        printf("  imessage\n");
    printf("  cli (always available)\n");
    return 0;
}

static int cmd_models(NcConfig *cfg, int argc, const char **argv) {
    (void)cfg; (void)argc; (void)argv;
    printf("Known models:\n");
    static const char *models[] = {
        "gpt-4o", "gpt-4o-mini", "gpt-4-turbo", "o1", "o3",
        "claude-opus-4", "claude-sonnet-4-5", "claude-haiku-4",
        "gemini-2.0-flash", "gemini-1.5-pro",
        "llama3.2", "llama3.1", "mistral", "mixtral",
        NULL
    };
    for (int i = 0; models[i]; i++) printf("  %s\n", models[i]);
    return 0;
}

/* ── main ────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    if (argc < 2) { print_usage(); return 0; }

    const char *cmd = argv[1];
    const char **sub_argv = (const char **)(argv + 2);
    int sub_argc = argc - 2;

    /* Handle --help / -h globally */
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        print_usage(); return 0;
    }
    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0) {
        printf("tiniclaw %s\n", TINICLAW_VERSION); return 0;
    }

    NcConfig *cfg = load_config();
    int rc = 0;

    if (strcmp(cmd, "agent") == 0) {
        rc = cmd_agent(cfg, sub_argc, sub_argv);
    } else if (strcmp(cmd, "gateway") == 0) {
        rc = cmd_gateway(cfg, sub_argc, sub_argv);
    } else if (strcmp(cmd, "daemon") == 0) {
        /* Run gateway in background */
        rc = cmd_gateway(cfg, sub_argc, sub_argv);
    } else if (strcmp(cmd, "status") == 0) {
        rc = nc_status_run(cfg);
    } else if (strcmp(cmd, "onboard") == 0) {
        rc = nc_onboard_run(cfg);
    } else if (strcmp(cmd, "doctor") == 0) {
        rc = nc_doctor_run(cfg);
    } else if (strcmp(cmd, "cron") == 0) {
        rc = cmd_cron(cfg, sub_argc, sub_argv);
    } else if (strcmp(cmd, "channel") == 0) {
        rc = cmd_channel(cfg, sub_argc, sub_argv);
    } else if (strcmp(cmd, "models") == 0) {
        rc = cmd_models(cfg, sub_argc, sub_argv);
    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        print_usage();
        rc = 1;
    }

    free(cfg);
    return rc;
}
