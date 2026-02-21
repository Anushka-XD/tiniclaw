#pragma once
/*
 * tiniclaw-c — string/utility helpers
 */
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "arena.h"

/* ── String helpers ─────────────────────────────────────────────── */

/* heap-owned strdup (use arena variant when inside a request) */
char   *nc_strdup(const char *s);
char   *nc_strndup(const char *s, size_t n);

/* case-insensitive comparison */
int     nc_strcasecmp(const char *a, const char *b);
int     nc_strncasecmp(const char *a, const char *b, size_t n);

/* trim leading/trailing whitespace in-place; returns same pointer */
char   *nc_strtrim(char *s);

/* safe snprintf that always NUL-terminates; returns chars written (excl NUL) */
int     nc_snprintf(char *buf, size_t size, const char *fmt, ...);

/* append src to dst (growing if needed); similar to strlcat semantics */
size_t  nc_strlcat(char *dst, const char *src, size_t dst_size);

/* ── Dynamic string buffer (heap-backed) ────────────────────────── */

typedef struct NcBuf {
    char   *data;
    size_t  len;
    size_t  cap;
} NcBuf;

void  nc_buf_init(NcBuf *b);
void  nc_buf_free(NcBuf *b);
int   nc_buf_append(NcBuf *b, const char *s, size_t n);
int   nc_buf_appendz(NcBuf *b, const char *s);       /* NUL-terminated */
int   nc_buf_appendf(NcBuf *b, const char *fmt, ...);
/* Reset len to 0 but keep capacity. */
void  nc_buf_reset(NcBuf *b);
/* Transfer ownership: returns heap pointer, caller must free(). Resets b. */
char *nc_buf_take(NcBuf *b);

/* ── JSON string escaping ───────────────────────────────────────── */

/* Write JSON-escaped string (with surrounding quotes) into buf.
   Returns bytes written (excluding NUL), or -1 on overflow. */
int nc_json_escape(NcBuf *out, const char *s);

/* ── Time helpers ───────────────────────────────────────────────── */

/* ISO-8601 UTC timestamp into buf, e.g. "2026-02-21T12:34:56Z". */
void nc_iso8601_now(char *buf, size_t size);

/* Monotonic millisecond counter */
uint64_t nc_mono_ms(void);

/* ── Hex encode/decode ──────────────────────────────────────────── */

/* Writes len*2 chars + NUL into out (must be >= len*2+1 bytes). */
void nc_hex_encode(const uint8_t *data, size_t len, char *out);

/* Decodes hex string (must be even length) into out_buf.
   Returns number of decoded bytes, or -1 on error. */
int  nc_hex_decode(const char *hex, size_t hex_len, uint8_t *out, size_t out_size);

/* ── Misc ────────────────────────────────────────────────────────── */

/* Constant-time byte comparison (for token validation). */
bool nc_const_eq(const uint8_t *a, const uint8_t *b, size_t n);

/* Generate `n` random bytes using arc4random_buf (macOS). */
void nc_random_bytes(uint8_t *out, size_t n);

/* Print to stderr; always flushes. */
void nc_log_stderr(const char *fmt, ...);
