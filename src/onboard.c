/*
 * tiniclaw-c — onboard wizard
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "config.h"
#include "platform.h"
#include "util.h"

static char *ask(const char *prompt, const char *def) {
    printf("%s", prompt);
    if (def && def[0]) printf(" [%s]", def);
    printf(": ");
    fflush(stdout);
    char line[1024];
    if (!fgets(line, sizeof line, stdin)) return nc_strdup(def ? def : "");
    size_t n = strlen(line);
    if (n > 0 && line[n-1] == '\n') line[n-1] = '\0';
    if (!line[0] && def) return nc_strdup(def);
    return nc_strdup(line);
}

int nc_onboard_run(NcConfig *cfg) {
    printf("\n┌──────────────────────────────────┐\n");
    printf("│  tiniclaw — setup wizard         │\n");
    printf("└──────────────────────────────────┘\n\n");

    printf("Choose your default AI provider:\n");
    printf("  1) OpenAI   2) Anthropic   3) Ollama   4) OpenRouter\n");
    char *choice = ask("Provider [1-4]", "1");
    if (strcmp(choice, "2") == 0)      snprintf(cfg->default_provider, sizeof cfg->default_provider, "anthropic");
    else if (strcmp(choice, "3") == 0) snprintf(cfg->default_provider, sizeof cfg->default_provider, "ollama");
    else if (strcmp(choice, "4") == 0) snprintf(cfg->default_provider, sizeof cfg->default_provider, "openrouter");
    else                               snprintf(cfg->default_provider, sizeof cfg->default_provider, "openai");
    free(choice);

    bool needs_key = strcmp(cfg->default_provider, "ollama") != 0;
    if (needs_key) {
        char prompt[128];
        snprintf(prompt, sizeof prompt, "%s API key", cfg->default_provider);
        char *key = ask(prompt, "");
        if (key && key[0]) {
            snprintf(cfg->providers[0].name,    sizeof cfg->providers[0].name,    "%s", cfg->default_provider);
            snprintf(cfg->providers[0].api_key, sizeof cfg->providers[0].api_key, "%s", key);
            if (cfg->providers_count < 1) cfg->providers_count = 1;
        }
        free(key);
    }

    const char *dm =
        strcmp(cfg->default_provider, "anthropic")  == 0 ? "claude-sonnet-4-5" :
        strcmp(cfg->default_provider, "openrouter") == 0 ? "openai/gpt-4o" :
        strcmp(cfg->default_provider, "ollama")     == 0 ? "llama3.2" : "gpt-4o";
    char *model = ask("Default model", dm);
    if (model && model[0]) snprintf(cfg->default_model, sizeof cfg->default_model, "%s", model);
    free(model);

    char *ws = ask("Workspace directory", cfg->workspace_dir[0] ? cfg->workspace_dir : ".");
    if (ws && ws[0]) snprintf(cfg->workspace_dir, sizeof cfg->workspace_dir, "%s", ws);
    free(ws);

    printf("Memory backend (sqlite / markdown / none) [sqlite]: ");
    fflush(stdout);
    char mline[32] = "";
    if (fgets(mline, sizeof mline, stdin)) {
        size_t n = strlen(mline); if (n > 0 && mline[n-1] == '\n') mline[n-1] = '\0';
    }
    if (!mline[0]) strcpy(mline, "sqlite");
    snprintf(cfg->memory.backend, sizeof cfg->memory.backend, "%s", mline);

    char home[4096]; nc_home_dir(home, sizeof home);
    char config_path[4096];
    snprintf(config_path, sizeof config_path, "%s/.tiniclaw/config.json", home);
    /* Ensure parent directory exists */
    char config_dir[4096];
    snprintf(config_dir, sizeof config_dir, "%s/.tiniclaw", home);
    nc_ensure_dir(config_dir);
    if (nc_config_save(cfg) == 0) {
        printf("\n[ok] Config saved to %s\n", config_path);
    } else {
        printf("\n[err] Failed to save config\n");
        return 1;
    }
    printf("\nRun: tiniclaw agent  — to start a session\n\n");
    return 0;
}
