/*
 * tiniclaw-c — channel infrastructure + CLI channel
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "channel.h"
#include "arena.h"
#include "util.h"

/*
 * CLI CHANNEL (stdin/stdout)
 */

typedef struct {
    char prompt[64];
} CliChannel;

static int cli_send(void *ptr, NcArena *arena, const char *msg, const char *to) {
    (void)ptr; (void)arena; (void)to;
    printf("%s\n", msg ? msg : "");
    fflush(stdout);
    return 0;
}

static int cli_listen(void *ptr, NcMessageHandler handler, void *hctx) {
    CliChannel *c = ptr;
    char line[8192];
    while (1) {
        if (c->prompt[0]) { printf("%s", c->prompt); fflush(stdout); }
        if (!fgets(line, sizeof line, stdin)) break;
        size_t n = strlen(line);
        if (n > 0 && line[n-1] == '\n') line[n-1] = '\0';
        if (!line[0]) continue;
        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) break;
        NcChannelMessage msg = {
            .content = line,
            .sender  = "user",
            .channel = "cli",
        };
        if (handler) handler(&msg, hctx);
    }
    return 0;
}

static const char *cli_channel_name(void *p) { (void)p; return "cli"; }
static bool cli_is_configured(void *p) { (void)p; return true; }
static void cli_deinit(void *p) { free(p); }

static const NcChannelVTable CLI_VTABLE = {
    .send          = cli_send,
    .listen        = cli_listen,
    .name          = cli_channel_name,
    .is_configured = cli_is_configured,
    .deinit        = cli_deinit,
};

NcChannel nc_channel_cli_init(const char *prompt) {
    CliChannel *c = calloc(1, sizeof *c);
    if (!c) return (NcChannel){0};
    snprintf(c->prompt, sizeof c->prompt, "%s", prompt ? prompt : "> ");
    return (NcChannel){ .ptr = c, .vtable = &CLI_VTABLE };
}
