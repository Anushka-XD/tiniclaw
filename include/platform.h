#pragma once
#include <stddef.h>
#include <stdbool.h>

void        nc_home_dir(char *buf, size_t size);
void        nc_config_dir(char *buf, size_t size);
void        nc_ensure_dir(const char *path);
const char *nc_getenv(const char *key);
bool        nc_file_exists(const char *path);
long        nc_file_size(const char *path);
char       *nc_read_file(const char *path, size_t *out_len);
int         nc_write_file(const char *path, const char *data, size_t len);

/* Forward declarations for sub-commands */
struct NcConfig;
int nc_status_run(const struct NcConfig *cfg);
int nc_onboard_run(struct NcConfig *cfg);
int nc_doctor_run(const struct NcConfig *cfg);
