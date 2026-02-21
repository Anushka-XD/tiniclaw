/*
 * tiniclaw-c — net_security (URL validation)
 */
#include <string.h>
#include <stdbool.h>
#include "net_security.h"

bool nc_url_is_http_insecure(const char *url) {
    if (!url) return false;
    /* Block plain http:// but allow https:// */
    return strncmp(url, "http://", 7) == 0;
}

bool nc_url_is_valid_https(const char *url) {
    if (!url) return false;
    return strncmp(url, "https://", 8) == 0;
}
