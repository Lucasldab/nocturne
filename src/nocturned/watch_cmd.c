/*
 * watch_cmd.c — `nocturned watch <library>` argv handler.
 *
 * Acquires the single-writer PID lock for the lifetime of the watch loop;
 * a second `watch` (or `scan`/`resolve`/`publish`) attempt while this
 * holds returns NOCT_EXIT_LOCK_BUSY.
 *
 * Exit codes:
 *   0  graceful shutdown (SIGTERM / SIGINT received).
 *   1  generic failure.
 *   3  hard error (root inaccessible, db open failed, watch_run -1).
 *   4  lock busy.
 */

#define _GNU_SOURCE

#include "cli.h"
#include "db.h"
#include "lock.h"
#include "paths.h"
#include "watch.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void err_to_stderr(const char *msg, void *ud)
{
    (void) ud;
    fprintf(stderr, "nocturned: %s\n", msg);
}

int watch_cmd_main(struct cli_args *args)
{
    if (!args || !args->library_path) {
        fprintf(stderr, "nocturned watch: missing <library> path\n");
        return 64;  /* USAGE */
    }

    struct stat st;
    if (lstat(args->library_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "nocturned watch: %s is not a readable directory: %s\n",
                args->library_path, strerror(errno));
        return 3;
    }

    const char *pidfile = paths_pidfile();
    if (!pidfile) {
        fprintf(stderr, "nocturned watch: cannot resolve pidfile path\n");
        return 1;
    }
    int busy_pid = 0;
    struct nocturne_lock *lock = lock_acquire(pidfile, &busy_pid);
    if (!lock) {
        if (errno == EWOULDBLOCK) {
            fprintf(stderr,
                    "nocturned: another instance is running (pid=%d); "
                    "single-writer lock at %s\n",
                    busy_pid, pidfile);
            return 4;
        }
        fprintf(stderr, "nocturned watch: lock_acquire failed: %s\n", strerror(errno));
        return 1;
    }

    const char *db_path = paths_db_file();
    if (!db_path) {
        fprintf(stderr, "nocturned watch: cannot resolve DB path\n");
        lock_release(lock);
        return 3;
    }

    struct nocturne_db *db = db_open(db_path, err_to_stderr, NULL);
    if (!db) {
        lock_release(lock);
        return 3;
    }

    struct watch_opts opts = { .debounce_ms = 1000, .periodic_rescan_sec = 300 };
    if (args->debounce_ms > 0) opts.debounce_ms = args->debounce_ms;
    if (args->periodic_rescan_sec > 0) opts.periodic_rescan_sec = args->periodic_rescan_sec;

    int rc = watch_run(db, args->library_path, &opts);

    db_close(db);
    lock_release(lock);
    return rc < 0 ? 3 : 0;
}
