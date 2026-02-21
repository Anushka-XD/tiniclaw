/*
 * tiniclaw-c — utility helpers
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __APPLE__
#  include <mach/mach_time.h>
#  include <sys/time.h>
#endif

#include "util.h"
#include "arena.h"

/* ── String helpers ─────────────────────────────────────────────── */

char *nc_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = malloc(n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

char *nc_strndup(const char *s, size_t n) {
    if (!s) return NULL;
    char *p = malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

int nc_strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        int d = tolower((unsigned char)*a) - tolower((unsigned char)*b);
        if (d != 0) return d;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int nc_strncasecmp(const char *a, const char *b, size_t n) {
    while (n-- && *a && *b) {
        int d = tolower((unsigned char)*a) - tolower((unsigned char)*b);
        if (d != 0) return d;
        a++; b++;
    }
    if (n == (size_t)-1) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

char *nc_strtrim(char *s) {
    if (!s) return s;
    while (isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return s;
}

int nc_snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    if (size > 0) buf[size - 1] = '\0';
    return n;
}

size_t nc_strlcat(char *dst, const char *src, size_t dst_size) {
    size_t dst_len = strnlen(dst, dst_size);
    size_t src_len = strlen(src);
    if (dst_len < dst_size - 1) {
        size_t copy = dst_size - dst_len - 1;
        if (copy > src_len) copy = src_len;
        memcpy(dst + dst_len, src, copy);
        dst[dst_len + copy] = '\0';
    }
    return dst_len + src_len;
}

/* ── Dynamic buffer ─────────────────────────────────────────────── */

void nc_buf_init(NcBuf *b) {
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

void nc_buf_free(NcBuf *b) {
    free(b->data);
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

static int nc_buf_grow(NcBuf *b, size_t needed) {
    size_t new_cap = b->cap ? b->cap * 2 : 256;
    while (new_cap < needed) new_cap *= 2;
    char *p = realloc(b->data, new_cap);
    if (!p) return -1;
    b->data = p;
    b->cap  = new_cap;
    return 0;
}

int nc_buf_append(NcBuf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        if (nc_buf_grow(b, b->len + n + 1) < 0) return -1;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

int nc_buf_appendz(NcBuf *b, const char *s) {
    return nc_buf_append(b, s, strlen(s));
}

int nc_buf_appendf(NcBuf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (needed < 0) return -1;
    if (b->len + (size_t)needed + 1 > b->cap) {
        if (nc_buf_grow(b, b->len + (size_t)needed + 1) < 0) return -1;
    }
    va_start(ap, fmt);
    vsnprintf(b->data + b->len, (size_t)needed + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)needed;
    return 0;
}

void nc_buf_reset(NcBuf *b) { b->len = 0; if (b->data) b->data[0] = '\0'; }

char *nc_buf_take(NcBuf *b) {
    char *p = b->data;
    b->data = NULL; b->len = 0; b->cap = 0;
    return p;
}

/* ── JSON string escaping ───────────────────────────────────────── */

int nc_json_escape(NcBuf *out, const char *s) {
    if (nc_buf_append(out, "\"", 1) < 0) return -1;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '"':  if (nc_buf_appendz(out, "\\\"") < 0) return -1; break;
            case '\\': if (nc_buf_appendz(out, "\\\\") < 0) return -1; break;
            case '\n': if (nc_buf_appendz(out, "\\n")  < 0) return -1; break;
            case '\r': if (nc_buf_appendz(out, "\\r")  < 0) return -1; break;
            case '\t': if (nc_buf_appendz(out, "\\t")  < 0) return -1; break;
            default:
                if (c < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04x", c);
                    if (nc_buf_appendz(out, esc) < 0) return -1;
                } else {
                    if (nc_buf_append(out, (const char *)&c, 1) < 0) return -1;
                }
                break;
        }
    }
    return nc_buf_append(out, "\"", 1);
}

/* ── Time helpers ───────────────────────────────────────────────── */

void nc_iso8601_now(char *buf, size_t size) {
    time_t t = time(NULL);
    struct tm *tm = gmtime(&t);
    strftime(buf, size, "%Y-%m-%dT%H:%M:%SZ", tm);
}

uint64_t nc_mono_ms(void) {
#ifdef __APPLE__
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom / 1000000ULL;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
#endif
}

/* ── Hex encode/decode ──────────────────────────────────────────── */

static const char HEX[] = "0123456789abcdef";

void nc_hex_encode(const uint8_t *data, size_t len, char *out) {
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = HEX[data[i] >> 4];
        out[i * 2 + 1] = HEX[data[i] & 0x0f];
    }
    out[len * 2] = '\0';
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int nc_hex_decode(const char *hex, size_t hex_len, uint8_t *out, size_t out_size) {
    if (hex_len & 1) return -1;
    size_t byte_len = hex_len / 2;
    if (out_size < byte_len) return -1;
    for (size_t i = 0; i < byte_len; i++) {
        int hi = hex_val(hex[i * 2]);
        int lo = hex_val(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)byte_len;
}

/* ── Misc ────────────────────────────────────────────────────────── */

bool nc_const_eq(const uint8_t *a, const uint8_t *b, size_t n) {
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff |= a[i] ^ b[i];
    return diff == 0;
}

void nc_random_bytes(uint8_t *out, size_t n) {
#ifdef __APPLE__
    arc4random_buf(out, n);
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) { fread(out, 1, n, f); fclose(f); }
#endif
}

void nc_log_stderr(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}
