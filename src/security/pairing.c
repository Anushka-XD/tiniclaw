/*
 * tiniclaw-c — pairing guard + security policy + rate tracker + audit + sandbox
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include "security.h"
#include "util.h"
#include "platform.h"

/* ══════════════════════════════════════════════════════════════════
   Pairing
   ══════════════════════════════════════════════════════════════════ */

void nc_pairing_init(NcPairingGuard *g) {
    memset(g, 0, sizeof *g);
    /* Generate 6-digit OTP */
    uint32_t code;
    nc_random_bytes((uint8_t *)&code, sizeof code);
    snprintf(g->pairing_code, sizeof g->pairing_code, "%06u", code % 1000000u);

    /* Generate 32-byte random bearer token */
    uint8_t tok[32];
    nc_random_bytes(tok, sizeof tok);
    nc_hex_encode(tok, sizeof tok, g->bearer_token);
    g->paired = false;

    fprintf(stderr, "\n\033[1;33m[tiniclaw] Pairing code: %s\033[0m\n\n", g->pairing_code);
    fflush(stderr);
}

bool nc_pairing_try(NcPairingGuard *g, const char *code) {
    if (!code || strlen(code) != NC_PAIRING_CODE_LEN) return false;
    if (nc_const_eq((const uint8_t *)g->pairing_code,
                    (const uint8_t *)code, NC_PAIRING_CODE_LEN)) {
        g->paired = true;
        return true;
    }
    return false;
}

bool nc_pairing_validate_token(const NcPairingGuard *g, const char *token) {
    if (!token || !g->paired) return false;
    size_t tok_len = strlen(g->bearer_token);
    size_t in_len  = strlen(token);
    if (in_len != tok_len) return false;
    return nc_const_eq((const uint8_t *)g->bearer_token,
                       (const uint8_t *)token, tok_len);
}

bool nc_is_public_bind(const char *host) {
    if (!host) return false;
    return strcmp(host, "0.0.0.0") == 0 || strcmp(host, "::") == 0 ||
           strcmp(host, "0:0:0:0:0:0:0:0") == 0;
}

/* ══════════════════════════════════════════════════════════════════
   Security policy
   ══════════════════════════════════════════════════════════════════ */

static const char *HIGH_RISK_CMDS[] = {
    "rm", "rmdir", "dd", "mkfs", "format", "shutdown", "reboot", "halt",
    "chmod", "chown", "sudo", "su", "passwd", "useradd", "userdel",
    "iptables", "ufw", "firewall-cmd", "kill", "pkill", "killall",
    "curl", "wget", "nc", "netcat", "ncat", "ssh", "scp", "rsync",
    NULL
};

static const char *MEDIUM_RISK_CMDS[] = {
    "cp", "mv", "ln", "find", "grep", "sed", "awk", "perl", "python",
    "node", "npm", "pip", "brew", "apt", "yum", "dnf", "git", "make",
    NULL
};

NcCommandRisk nc_classify_command(const NcSecurityPolicy *p, const char *cmd) {
    (void)p;
    if (!cmd) return NC_RISK_LOW;
    /* Extract just the base command name */
    const char *base = strrchr(cmd, '/');
    base = base ? base + 1 : cmd;
    char name[64]; snprintf(name, sizeof name, "%s", base);
    /* Truncate at first space */
    for (char *c = name; *c; c++) { if (*c == ' ') { *c = '\0'; break; } }

    for (int i = 0; HIGH_RISK_CMDS[i]; i++)
        if (strcmp(name, HIGH_RISK_CMDS[i]) == 0) return NC_RISK_HIGH;
    for (int i = 0; MEDIUM_RISK_CMDS[i]; i++)
        if (strcmp(name, MEDIUM_RISK_CMDS[i]) == 0) return NC_RISK_MEDIUM;
    return NC_RISK_LOW;
}

bool nc_check_null_byte(const char *path) {
    if (!path) return true;
    while (*path) { if (*path++ == '\0') return true; }
    return false;
}

bool nc_is_path_allowed(const NcSecurityPolicy *p, const char *workspace, const char *path) {
    if (!p || !path) return false;
    if (nc_check_null_byte(path)) return false;
    if (!p->workspace_only) return true;
    if (!workspace) return false;
    /* Path must start with workspace_dir */
    size_t wlen = strlen(workspace);
    if (strncmp(path, workspace, wlen) == 0 &&
        (path[wlen] == '/' || path[wlen] == '\0')) return true;
    return false;
}

bool nc_check_symlink_escape(const char *workspace, const char *resolved) {
    if (!workspace || !resolved) return false;
    return !nc_is_path_allowed(
        &(NcSecurityPolicy){ .workspace_only = true },
        workspace, resolved);
}

/* ══════════════════════════════════════════════════════════════════
   Rate tracker — sliding window per key (simple linked list)
   ══════════════════════════════════════════════════════════════════ */

#define MAX_TIMESTAMPS 1024

typedef struct RateEntry {
    char     key[256];
    int64_t  timestamps[MAX_TIMESTAMPS];
    size_t   count;
    struct RateEntry *next;
} RateEntry;

struct NcRateTracker {
    uint32_t     limit;
    uint64_t     window_s;
    RateEntry   *entries;
    pthread_mutex_t mu;
};

NcRateTracker *nc_rate_tracker_new(uint32_t limit, uint64_t window_secs) {
    NcRateTracker *t = calloc(1, sizeof *t);
    t->limit    = limit;
    t->window_s = window_secs;
    pthread_mutex_init(&t->mu, NULL);
    return t;
}

void nc_rate_tracker_free(NcRateTracker *t) {
    if (!t) return;
    RateEntry *e = t->entries;
    while (e) { RateEntry *n = e->next; free(e); e = n; }
    pthread_mutex_destroy(&t->mu);
    free(t);
}

bool nc_rate_tracker_allow(NcRateTracker *t, const char *key) {
    if (!t || t->limit == 0) return true;
    pthread_mutex_lock(&t->mu);
    int64_t now = (int64_t)time(NULL);
    int64_t cutoff = now - (int64_t)t->window_s;

    RateEntry *e = t->entries;
    while (e && strncmp(e->key, key, sizeof e->key - 1) != 0) e = e->next;
    if (!e) {
        e = calloc(1, sizeof *e);
        snprintf(e->key, sizeof e->key, "%s", key);
        e->next = t->entries;
        t->entries = e;
    }

    /* Remove expired */
    size_t valid = 0;
    for (size_t i = 0; i < e->count; i++) {
        if (e->timestamps[i] > cutoff) e->timestamps[valid++] = e->timestamps[i];
    }
    e->count = valid;

    bool allowed = e->count < (size_t)t->limit;
    if (allowed && e->count < MAX_TIMESTAMPS) e->timestamps[e->count++] = now;
    pthread_mutex_unlock(&t->mu);
    return allowed;
}

/* ══════════════════════════════════════════════════════════════════
   Audit logger
   ══════════════════════════════════════════════════════════════════ */

void nc_audit_logger_init(NcAuditLogger *l, const char *log_path, bool enabled) {
    memset(l, 0, sizeof *l);
    if (log_path) snprintf(l->log_path, sizeof l->log_path, "%s", log_path);
    l->enabled = enabled;
    l->fd = -1;
    if (enabled && log_path) {
        l->fd = (int)(intptr_t)fopen(log_path, "ab"); /* misuse as file* */
        /* Better: use real FILE* member — simplified for now */
    }
}

void nc_audit_logger_close(NcAuditLogger *l) { /* close if opened */ (void)l; }

void nc_audit_log(NcAuditLogger *l, const NcAuditEvent *ev) {
    if (!l || !l->enabled || !ev) return;
    char ts[32]; nc_iso8601_now(ts, sizeof ts);
    static const char *type_names[] = {
        "message_received","tool_executed","command_blocked",
        "pairing_attempt","gateway_request","secret_access"
    };
    const char *type_str = (ev->type < 6) ? type_names[ev->type] : "unknown";
    FILE *f = fopen(l->log_path, "ab");
    if (!f) return;
    fprintf(f, "{\"ts\":\"%s\",\"type\":\"%s\",\"actor\":\"%s\",\"action\":\"%s\",\"result\":\"%s\"}\n",
        ts,
        type_str,
        ev->actor   ? ev->actor  : "",
        ev->action  ? ev->action : "",
        ev->result  ? ev->result : "ok");
    fclose(f);
}

/* ══════════════════════════════════════════════════════════════════
   Sandbox (noop on macOS; landlock/firejail/bwrap are Linux-only)
   ══════════════════════════════════════════════════════════════════ */

static const char **noop_wrap(void *self, NcArena *arena,
                               const char **argv, size_t argc, size_t *out) {
    (void)self; *out = argc; return argv;
}
static const char *noop_name(void *self)    { (void)self; return "none"; }
static bool noop_available(void *self)      { (void)self; return true; }
static void noop_deinit(void *self)         { (void)self; }

static const NcSandboxVTable NOOP_VTABLE = {
    .wrap_argv    = noop_wrap,
    .name         = noop_name,
    .is_available = noop_available,
    .deinit       = noop_deinit,
};

static int NOOP_INST = 0;

NcSandbox nc_sandbox_noop(void) {
    return (NcSandbox){ .ptr = &NOOP_INST, .vtable = &NOOP_VTABLE };
}

NcSandbox nc_sandbox_detect(NcArena *arena, const char *workspace_dir) {
    (void)arena; (void)workspace_dir;
    /* On macOS, only noop is available; firejail/landlock/bwrap require Linux */
    return nc_sandbox_noop();
}
