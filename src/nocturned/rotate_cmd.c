/*
 * rotate_cmd.c — `nocturned rotate` argv handler.
 *
 * Reads library_root from config (no positional). Same lock/db lifecycle
 * as scan_cmd / resolve_cmd. After plan 03-03 lands, this command also
 * initialises libcurl + Syncthing config and triggers a /rest/db/scan
 * POST after the file motion. For 03-02 it's filesystem-only.
 */

#define _GNU_SOURCE

#include "cli.h"
#include "config.h"
#include "db.h"
#include "lock.h"
#include "paths.h"
#include "rotate.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static void err_to_stderr(const char *msg, void *ud)
{
    (void) ud;
    fprintf(stderr, "nocturned: %s\n", msg);
}

int rotate_cmd_main(struct cli_args *args)
{
    if (!args) return NOCT_EXIT_USAGE;

    const char *pidfile = paths_pidfile();
    int busy_pid = 0;
    struct nocturne_lock *lock = lock_acquire(pidfile, &busy_pid);
    if (!lock) {
        if (errno == EWOULDBLOCK) {
            fprintf(stderr,
                "nocturned: another instance is running (pid=%d); "
                "single-writer lock at %s\n", busy_pid, pidfile);
            return NOCT_EXIT_LOCK_BUSY;
        }
        fprintf(stderr, "nocturned rotate: lock_acquire failed: %s\n",
                strerror(errno));
        return 1;
    }

    const char *db_path = paths_db_file();
    struct nocturne_db *db = db_open(db_path, err_to_stderr, NULL);
    if (!db) { lock_release(lock); return 3; }

    struct nocturne_config cfg;
    const char *cfg_path = args->config_path ? args->config_path : paths_config_file();
    if (config_load(cfg_path, &cfg) != 0) {
        config_free(&cfg);
        db_close(db); lock_release(lock);
        return 3;
    }

    if (!cfg.library_root || !cfg.library_root[0]) {
        fprintf(stderr,
            "nocturned rotate: [library].path missing from %s\n", cfg_path);
        config_free(&cfg);
        db_close(db); lock_release(lock);
        return 3;
    }

    struct rotate_stats stats = {0};
    int rc = rotate_run(db, cfg.library_root, &stats);

    fprintf(stdout,
        "rotate: to_add=%lld added=%lld to_remove=%lld removed=%lld "
        "already=%lld fallback=%lld errors=%lld\n",
        stats.to_add, stats.added, stats.to_remove, stats.removed,
        stats.already_applied, stats.fallback_copies, stats.errors);

    config_free(&cfg);
    db_close(db);
    lock_release(lock);

    if (rc < 0) return 3;
    return stats.errors > 0 ? 1 : 0;
}
