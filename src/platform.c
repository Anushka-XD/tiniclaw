/*
 * tiniclaw-c — platform helpers (macOS paths, home dir, env)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pwd.h>
#include "platform.h"

void nc_home_dir(char *buf, size_t size) {
    const char *home = getenv("HOME");
    if (home && *home) {
        snprintf(buf, size, "%s", home);
        return;
    }
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir) {
        snprintf(buf, size, "%s", pw->pw_dir);
        return;
    }
    snprintf(buf, size, ".");
}

void nc_config_dir(char *buf, size_t size) {
    char home[4096];
    nc_home_dir(home, sizeof(home));
    snprintf(buf, size, "%s/.tiniclaw", home);
}

void nc_ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return;
    /* mkdir -p equivalent */
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0700);
            *p = '/';
        }
    }
    mkdir(tmp, 0700);
}

const char *nc_getenv(const char *key) {
    return getenv(key);
}

bool nc_file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

long nc_file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long)st.st_size;
}

/* Read entire file into a heap-allocated string. Caller frees. Returns NULL on error. */
char *nc_read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);
    if (out_len) *out_len = (size_t)sz;
    return buf;
}

/* Write string to file. Returns 0 on success. */
int nc_write_file(const char *path, const char *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(data, 1, len, f);
    fclose(f);
    return 0;
}
