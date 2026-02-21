/*
 * tiniclaw-c — doctor (health diagnostics)
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "config.h"
#include "provider.h"
#include "arena.h"

static void check(const char *label, bool ok, const char *detail) {
    printf("  %s %-30s %s\n", ok ? "[ok]" : "[!!]", label, detail ? detail : "");
}

int nc_doctor_run(const NcConfig *cfg) {
    printf("tiniclaw doctor\n");
    printf("──────────────────────────────────────\n");

    check("config",   cfg != NULL, "loaded");
    check("provider", cfg->default_provider[0] != '\0', cfg->default_provider);
    check("model",    cfg->default_model[0]    != '\0', cfg->default_model);
    check("workspace",cfg->workspace_dir[0]    != '\0', cfg->workspace_dir);
    check("memory",   cfg->memory.backend[0]   != '\0', cfg->memory.backend);

    /* Test provider connectivity (no-op if no key) */
    bool has_key = false;
    for (int i = 0; i < cfg->providers_count; i++) {
        if (strcmp(cfg->providers[i].name, cfg->default_provider) == 0
            && cfg->providers[i].api_key[0]) {
            has_key = true; break;
        }
    }
    check("api_key", has_key, has_key ? "present" : "not set");

    printf("──────────────────────────────────────\n");
    return 0;
}
