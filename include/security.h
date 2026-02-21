#pragma once
/*
 * tiniclaw-c — security layer
 * Covers: secrets (ChaCha20-Poly1305), pairing guard, security policy,
 * audit logging, rate tracker, and sandbox vtable.
 */
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "arena.h"

/* ══════════════════════════════════════════════════════════════════
   Secrets — ChaCha20-Poly1305 encrypted API key storage
   ══════════════════════════════════════════════════════════════════ */

#define NC_KEY_LEN   32
#define NC_NONCE_LEN 12
#define NC_TAG_LEN   16

/* Prefix used for encrypted secret values on disk. */
#define NC_ENC_PREFIX "enc2:"

typedef struct NcSecretStore {
    char key_path[4096];
    bool enabled;
} NcSecretStore;

void nc_secret_store_init(NcSecretStore *s, const char *dir, bool enabled);

/* Encrypt plaintext → "enc2:<hex>". Caller frees result with free(). */
char *nc_secret_store_encrypt(const NcSecretStore *s, const char *plaintext);

/* Decrypt "enc2:<hex>" → plaintext. Caller frees result with free().
   If not an enc2: prefix, returns a duplicate of the input (passthrough). */
char *nc_secret_store_decrypt(const NcSecretStore *s, const char *ciphertext);

/* Low-level primitives */
int nc_chacha20_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                        const uint8_t *plain, size_t plain_len,
                        uint8_t *out); /* out must be plain_len + 16 bytes */

int nc_chacha20_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                        const uint8_t *cipher, size_t cipher_len, /* includes 16-byte tag */
                        uint8_t *out); /* out must be cipher_len - 16 bytes */

void nc_hmac_sha256(const uint8_t *key, size_t key_len,
                    const uint8_t *msg, size_t msg_len,
                    uint8_t out[32]);

/* ══════════════════════════════════════════════════════════════════
   Pairing guard — 6-digit one-time code → bearer token
   ══════════════════════════════════════════════════════════════════ */

#define NC_PAIRING_CODE_LEN  6
#define NC_BEARER_TOKEN_LEN  32   /* hex-encoded 16-byte random token */

typedef struct NcPairingGuard {
    char    pairing_code[NC_PAIRING_CODE_LEN + 1]; /* 6-digit OTP */
    char    bearer_token[NC_BEARER_TOKEN_LEN * 2 + 1];
    bool    paired;
} NcPairingGuard;

void nc_pairing_init(NcPairingGuard *g);

/* Attempt to pair using the given code. Returns true on success and sets paired=true. */
bool nc_pairing_try(NcPairingGuard *g, const char *code);

/* Validate a bearer token for an incoming request. */
bool nc_pairing_validate_token(const NcPairingGuard *g, const char *token);

/* Returns true if the host string represents a public bind (0.0.0.0 / ::). */
bool nc_is_public_bind(const char *host);

/* ══════════════════════════════════════════════════════════════════
   Security policy — command allowlists, path validation, risk tiers
   ══════════════════════════════════════════════════════════════════ */

typedef enum NcAutonomyLevel {
    NC_AUTONOMY_SUPERVISED,  /* confirm every action */
    NC_AUTONOMY_STANDARD,    /* confirm high-risk only */
    NC_AUTONOMY_FULL,        /* no confirmation needed */
} NcAutonomyLevel;

typedef enum NcCommandRisk {
    NC_RISK_LOW,
    NC_RISK_MEDIUM,
    NC_RISK_HIGH,
} NcCommandRisk;

typedef struct NcSecurityPolicy {
    NcAutonomyLevel  autonomy;
    bool             workspace_only;
    const char     **shell_allowlist;  /* NULL-terminated; NULL = default safe set */
    const char     **path_allowlist;   /* NULL-terminated; NULL = workspace_dir only */
    uint32_t         max_actions_per_hour;
    uint32_t         max_output_bytes;
} NcSecurityPolicy;

NcCommandRisk nc_classify_command(const NcSecurityPolicy *p, const char *cmd);
bool          nc_is_path_allowed(const NcSecurityPolicy *p, const char *workspace,
                                  const char *path);
bool          nc_check_null_byte(const char *path);
bool          nc_check_symlink_escape(const char *workspace, const char *resolved);

/* ══════════════════════════════════════════════════════════════════
   Rate tracker — sliding-window per-key rate limiting
   ══════════════════════════════════════════════════════════════════ */

/* Opaque sliding-window rate limiter. */
typedef struct NcRateTracker NcRateTracker;

NcRateTracker *nc_rate_tracker_new(uint32_t limit_per_window, uint64_t window_secs);
void           nc_rate_tracker_free(NcRateTracker *t);
/* Returns true if request is allowed; false if rate-limited. */
bool           nc_rate_tracker_allow(NcRateTracker *t, const char *key);

/* ══════════════════════════════════════════════════════════════════
   Audit logging — signed JSON event trail
   ══════════════════════════════════════════════════════════════════ */

typedef enum NcAuditEventType {
    NC_AUDIT_MESSAGE_RECEIVED,
    NC_AUDIT_TOOL_EXECUTED,
    NC_AUDIT_COMMAND_BLOCKED,
    NC_AUDIT_PAIRING_ATTEMPT,
    NC_AUDIT_GATEWAY_REQUEST,
    NC_AUDIT_SECRET_ACCESS,
} NcAuditEventType;

typedef struct NcAuditEvent {
    NcAuditEventType type;
    const char      *actor;       /* user/channel/ip */
    const char      *action;      /* command, tool name, etc. */
    const char      *result;      /* "ok", "blocked", "error:<msg>" */
    uint64_t         timestamp_s;
} NcAuditEvent;

typedef struct NcAuditLogger {
    char  log_path[4096];
    bool  enabled;
    int   fd;
} NcAuditLogger;

void nc_audit_logger_init(NcAuditLogger *l, const char *log_path, bool enabled);
void nc_audit_logger_close(NcAuditLogger *l);
void nc_audit_log(NcAuditLogger *l, const NcAuditEvent *ev);

/* ══════════════════════════════════════════════════════════════════
   Sandbox vtable — OS-level process isolation
   ══════════════════════════════════════════════════════════════════ */

typedef enum NcSandboxBackend {
    NC_SANDBOX_NONE,
    NC_SANDBOX_LANDLOCK,   /* Linux only */
    NC_SANDBOX_FIREJAIL,   /* Linux: firejail binary */
    NC_SANDBOX_BUBBLEWRAP, /* Linux: bwrap binary */
    NC_SANDBOX_DOCKER,     /* any: docker run */
} NcSandboxBackend;

typedef struct NcSandboxVTable {
    /* Wrap argv to run inside the sandbox. Returns new argv (arena-allocated). */
    const char **(*wrap_argv)(void *self, NcArena *arena,
                               const char **argv, size_t argc, size_t *out_argc);
    const char  *(*name)(void *self);
    bool         (*is_available)(void *self);
    void         (*deinit)(void *self);
} NcSandboxVTable;

typedef struct NcSandbox {
    void                  *ptr;
    const NcSandboxVTable *vtable;
} NcSandbox;

/* Auto-detect best available sandbox for this platform. */
NcSandbox nc_sandbox_detect(NcArena *arena, const char *workspace_dir);
NcSandbox nc_sandbox_noop(void);
