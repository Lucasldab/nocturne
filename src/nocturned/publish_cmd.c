/*
 * publish_cmd.c — `nocturned publish [--out <dir>]`.
 *
 * Acquires the single-writer lock; opens DB; resolves out_dir from
 * --out OR config sync_meta.path OR ${HOME}/sync/music-meta. Writes
 * catalog.json + manifest.json atomically.
 *
 * Exit codes: 0 success, 1 empty manifest_current (informational),
 * 3 hard error, 4 lock busy.
 */

#define _GNU_SOURCE

#include "cli.h"
#include "config.h"
#include "db.h"
#include "lock.h"
#include "paths.h"
#include "publish.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void err_to_stderr(const char *msg, void *ud)
{
    (void) ud;
    fprintf(stderr, "nocturned: %s\n", msg);
}

int publish_cmd_main(struct cli_args *args)
{
    if (!args) return 64;

    const char *pidfile = paths_pidfile();
    int busy_pid = 0;
    struct nocturne_lock *lock = lock_acquire(pidfile, &busy_pid);
    if (!lock) {
        if (errno == EWOULDBLOCK) {
            fprintf(stderr,
                "nocturned: another instance is running (pid=%d); "
                "single-writer lock at %s\n", busy_pid, pidfile);
            return 4;
        }
        fprintf(stderr, "nocturned publish: lock_acquire failed: %s\n",
                strerror(errno));
        return 1;
    }

    const char *db_path = paths_db_file();
    struct nocturne_db *db = db_open(db_path, err_to_stderr, NULL);
    if (!db) { lock_release(lock); return 3; }

    struct nocturne_config cfg;
    config_load(args->config_path ? args->config_path : paths_config_file(), &cfg);

    /* Determine output dir. */
    const char *out_dir = NULL;
    char default_buf[1024];
    if (args->out_dir) {
        out_dir = args->out_dir;
    } else if (cfg.sync_meta_root) {
        out_dir = cfg.sync_meta_root;
    } else {
        const char *home = getenv("HOME");
        if (home) {
            snprintf(default_buf, sizeof(default_buf), "%s/sync/music-meta", home);
            out_dir = default_buf;
        }
    }
    if (!out_dir) {
        fprintf(stderr, "nocturned publish: cannot resolve output directory\n");
        config_free(&cfg); db_close(db); lock_release(lock);
        return 3;
    }

    int rc = publish_run(db, out_dir);
    int ret;
    if (rc != 0) {
        ret = 1;  /* empty manifest is the documented informational exit */
    } else {
        ret = 0;
        fprintf(stdout, "publish: out=%s\n", out_dir);
    }

    config_free(&cfg); db_close(db); lock_release(lock);
    return ret;
}
