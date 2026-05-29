/*
 * download_cmd.c — `nocturned download [--meta-dir <path>]`.
 *
 * Reads downloads-phone-*.jsonl request lines and dispatches each fresh
 * id to the configured flacget binary. Reports back via
 * downloads-desktop.jsonl which the phone tails.
 *
 * Lock policy: separate lockfile from the single-writer cycle lock —
 * `flacget` calls `nocturned cycle` itself, so we MUST NOT hold the cycle
 * lock here or that nested call deadlocks (see download.h note).
 *
 * Exit codes:
 *   0 — success (per-request errors are non-fatal).
 *   1 — fatal error (status file unwritable, glob fatal).
 *   3 — config error (meta_dir unresolved, flacget_path unresolved).
 *   4 — lock busy (another `nocturned download` already running).
 */

#define _GNU_SOURCE

#include "cli.h"
#include "config.h"
#include "download.h"
#include "lock.h"
#include "paths.h"

#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static char *default_meta_dir(void)
{
    const char *home = getenv("HOME");
    if (!home || !*home) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : NULL;
    }
    if (!home || !*home) return NULL;
    const char *suffix = "/sync/nocturne/meta";
    size_t n = strlen(home) + strlen(suffix) + 1;
    char *out = malloc(n);
    if (!out) return NULL;
    snprintf(out, n, "%s%s", home, suffix);
    return out;
}

/* Default flacget path: $HOME/.local/bin/flacget. Caller frees. */
static char *default_flacget_path(void)
{
    const char *home = getenv("HOME");
    if (!home || !*home) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : NULL;
    }
    if (!home || !*home) return NULL;
    const char *suffix = "/.local/bin/flacget";
    size_t n = strlen(home) + strlen(suffix) + 1;
    char *out = malloc(n);
    if (!out) return NULL;
    snprintf(out, n, "%s%s", home, suffix);
    return out;
}

/* Per-user download lockfile under XDG_CACHE_HOME (or $HOME/.cache).
 * Caller frees. */
static char *download_pidfile(void)
{
    const char *cache = getenv("XDG_CACHE_HOME");
    char *fallback = NULL;
    if (!cache || !*cache) {
        const char *home = getenv("HOME");
        if (!home || !*home) {
            struct passwd *pw = getpwuid(getuid());
            home = pw ? pw->pw_dir : NULL;
        }
        if (!home) return NULL;
        size_t n = strlen(home) + strlen("/.cache") + 1;
        fallback = malloc(n);
        if (!fallback) return NULL;
        snprintf(fallback, n, "%s/.cache", home);
        cache = fallback;
    }
    const char *suffix = "/nocturne/nocturned-download.pid";
    size_t n = strlen(cache) + strlen(suffix) + 1;
    char *out = malloc(n);
    if (out) snprintf(out, n, "%s%s", cache, suffix);
    free(fallback);
    return out;
}

int download_cmd_main(struct cli_args *args)
{
    if (!args) return NOCT_EXIT_USAGE;

    /* 1. Lock — dedicated downloads lockfile (NOT the cycle lock). */
    char *pidfile = download_pidfile();
    if (!pidfile) {
        fprintf(stderr, "nocturned download: cannot resolve pidfile path\n");
        return NOCT_EXIT_FAILURE;
    }
    /* Ensure parent dir exists (paths_mkdir_p walks the prefix). */
    char *slash = strrchr(pidfile, '/');
    if (slash) {
        *slash = '\0';
        paths_mkdir_p(pidfile, 0700);
        *slash = '/';
    }
    int busy_pid = 0;
    struct nocturne_lock *lock = lock_acquire(pidfile, &busy_pid);
    if (!lock) {
        if (errno == EWOULDBLOCK) {
            fprintf(stderr,
                "nocturned: another download dispatcher is running "
                "(pid=%d); lock at %s\n", busy_pid, pidfile);
            free(pidfile);
            return NOCT_EXIT_LOCK_BUSY;
        }
        fprintf(stderr, "nocturned download: lock_acquire failed: %s\n",
                strerror(errno));
        free(pidfile);
        return NOCT_EXIT_FAILURE;
    }
    free(pidfile);

    /* 2. Load config — only used for meta_dir resolution. */
    struct nocturne_config cfg;
    const char *cfg_path = args->config_path ? args->config_path : paths_config_file();
    if (config_load(cfg_path, &cfg) != 0) {
        config_free(&cfg);
        lock_release(lock);
        return 3;
    }

    /* 3. Resolve meta_dir: cli arg > config sync_meta_root > default. */
    char *fallback_meta = NULL;
    const char *meta_dir = NULL;
    if (args->meta_dir && *args->meta_dir) {
        meta_dir = args->meta_dir;
    } else if (cfg.sync_meta_root && *cfg.sync_meta_root) {
        meta_dir = cfg.sync_meta_root;
    } else {
        fallback_meta = default_meta_dir();
        meta_dir = fallback_meta;
    }
    if (!meta_dir) {
        fprintf(stderr, "nocturned download: cannot determine meta directory\n");
        config_free(&cfg);
        lock_release(lock);
        return 3;
    }

    /* 4. Resolve flacget path — for now hard-coded to ~/.local/bin/flacget.
     * Config knob can be added later if other deployments need it. */
    char *flacget_path = default_flacget_path();
    if (!flacget_path) {
        fprintf(stderr, "nocturned download: cannot resolve flacget path\n");
        free(fallback_meta);
        config_free(&cfg);
        lock_release(lock);
        return 3;
    }
    /* Best-effort existence check — the per-request fork will report the
     * actual exec failure if the file vanishes between here and the exec. */
    struct stat st;
    if (stat(flacget_path, &st) != 0) {
        fprintf(stderr,
            "nocturned download: flacget not found at %s — install or "
            "symlink the wrapper there\n", flacget_path);
        free(flacget_path);
        free(fallback_meta);
        config_free(&cfg);
        lock_release(lock);
        return 3;
    }

    /* 5. Run dispatcher. */
    struct download_stats st_out = {0};
    int rc = download_run(meta_dir, flacget_path, &st_out);

    fprintf(stdout,
        "download requests_seen=%ld skipped_done=%ld ok=%ld err=%ld "
        "parse_errors=%ld\n",
        st_out.requests_seen, st_out.requests_skipped_done,
        st_out.requests_processed_ok, st_out.requests_processed_err,
        st_out.lines_skipped_parse_error);

    free(flacget_path);
    free(fallback_meta);
    config_free(&cfg);
    lock_release(lock);
    return rc == 0 ? NOCT_EXIT_OK : NOCT_EXIT_FAILURE;
}
