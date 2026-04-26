/*
 * paths.c — XDG-aware path resolution for nocturned.
 *
 * Three pinned paths the daemon owns:
 *   - DB file       (XDG_DATA_HOME)
 *   - PID lockfile  (XDG_CACHE_HOME)
 *   - config.toml   (XDG_CONFIG_HOME)
 *
 * Buffers are static (per-process); callers must not free.
 */

#define _GNU_SOURCE

#include "paths.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Resolve a path under one of the XDG dirs. `xdg_var` is the env var name,
 * `default_subdir` is the relative path under $HOME used when the env var
 * is unset/empty (e.g. ".local/share"), `tail` is appended after a single
 * slash. Writes into buf and returns buf (or NULL on overflow / no $HOME). */
static const char *resolve_xdg(char *buf, size_t cap,
                               const char *xdg_var,
                               const char *default_subdir,
                               const char *tail)
{
    const char *xdg = getenv(xdg_var);
    if (xdg && xdg[0] == '/') {
        if ((size_t) snprintf(buf, cap, "%s/%s", xdg, tail) >= cap) return NULL;
        return buf;
    }
    const char *home = getenv("HOME");
    if (!home || home[0] != '/') return NULL;
    if ((size_t) snprintf(buf, cap, "%s/%s/%s", home, default_subdir, tail) >= cap) return NULL;
    return buf;
}

const char *paths_db_file(void)
{
    static char buf[1024];
    static const char *cached = NULL;
    if (cached) return cached;
    cached = resolve_xdg(buf, sizeof(buf),
                         "XDG_DATA_HOME", ".local/share",
                         "nocturne/nocturne.db");
    return cached;
}

const char *paths_pidfile(void)
{
    static char buf[1024];
    static const char *cached = NULL;
    if (cached) return cached;
    cached = resolve_xdg(buf, sizeof(buf),
                         "XDG_CACHE_HOME", ".cache",
                         "nocturne/nocturned.pid");
    return cached;
}

const char *paths_config_file(void)
{
    static char buf[1024];
    static const char *cached = NULL;
    if (cached) return cached;
    cached = resolve_xdg(buf, sizeof(buf),
                         "XDG_CONFIG_HOME", ".config",
                         "nocturne/config.toml");
    return cached;
}

int paths_mkdir_p(const char *dir, mode_t mode)
{
    if (!dir || !*dir) { errno = EINVAL; return -1; }

    char buf[4096];
    size_t n = strlen(dir);
    if (n >= sizeof(buf)) { errno = ENAMETOOLONG; return -1; }
    memcpy(buf, dir, n + 1);

    /* Walk path components, creating each. Skip leading '/' so we don't try
     * to mkdir("") at the root. */
    for (size_t i = 1; i <= n; i++) {
        if (buf[i] == '/' || buf[i] == '\0') {
            char saved = buf[i];
            buf[i] = '\0';
            if (mkdir(buf, mode) != 0 && errno != EEXIST) {
                /* If a non-dir exists with this name, fail. */
                struct stat st;
                if (stat(buf, &st) != 0 || !S_ISDIR(st.st_mode)) {
                    return -1;
                }
            }
            buf[i] = saved;
        }
    }
    return 0;
}
