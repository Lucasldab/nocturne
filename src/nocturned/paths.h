#ifndef NOCTURNE_NOCTURNED_PATHS_H
#define NOCTURNE_NOCTURNED_PATHS_H

#include <sys/types.h>

/* Resolved at first call, cached thread-locally. Callers must not free. */
const char *paths_db_file(void);     /* ${XDG_DATA_HOME:-$HOME/.local/share}/nocturne/nocturne.db */
const char *paths_pidfile(void);     /* ${XDG_CACHE_HOME:-$HOME/.cache}/nocturne/nocturned.pid */
const char *paths_config_file(void); /* ${XDG_CONFIG_HOME:-$HOME/.config}/nocturne/config.toml */

/* mkdir -p equivalent. Returns 0 on success or if already exists, -1 on
 * error with errno set. `mode` is applied to created components. */
int paths_mkdir_p(const char *dir, mode_t mode);

#endif /* NOCTURNE_NOCTURNED_PATHS_H */
