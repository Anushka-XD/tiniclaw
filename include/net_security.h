/*
 * tiniclaw-c — net_security header
 */
#pragma once
#include <stdbool.h>

bool nc_url_is_http_insecure(const char *url);
bool nc_url_is_valid_https(const char *url);
