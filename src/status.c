/*
 * tiniclaw-c — status reporter
 */
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "provider.h"
#include "arena.h"
#include "version.h"

int nc_status_run(const NcConfig *cfg) {
    printf("tiniclaw %s\n", TINICLAW_VERSION);
    printf("Provider  : %s\n", cfg->default_provider[0] ? cfg->default_provider : "(none)");
    printf("Model     : %s\n", cfg->default_model[0]    ? cfg->default_model    : "(default)");
    printf("Workspace : %s\n", cfg->workspace_dir[0]    ? cfg->workspace_dir    : ".");
    printf("Memory    : %s\n", cfg->memory.backend[0]   ? cfg->memory.backend   : "sqlite");
    printf("Gateway   : port=%d\n", cfg->gateway.port);
    return 0;
}
