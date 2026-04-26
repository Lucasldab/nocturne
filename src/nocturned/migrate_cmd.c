/*
 * migrate_cmd.c — `nocturned migrate <library> [--apply]` argv handler.
 *
 * Same lifecycle as scan_cmd.c: lock_acquire (write subcommand), db_open,
 * migrate_run, summary line, db_close, lock_release.
 *
 * Exit codes:
 *   0 — full success (errors==0)
 *   1 — partial (errors>0; DB still consistent, rerunnable)
 *   3 — hard error (root inaccessible, db open failed, migrate_run -1)
 *   4 — lock busy
 */

#define _GNU_SOURCE

#include "cli.h"
#include "db.h"
#include "lock.h"
#include "migrate.h"
#include "paths.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static void err_to_stderr(const char *msg, void *ud)
{
    (void) ud;
    fprintf(stderr, "nocturned: %s\n", msg);
}

int migrate_cmd_main(struct cli_args *args)
{
    if (!args || !args->library_path) {
        fprintf(stderr, "nocturned migrate: missing <library> path\n");
        return NOCT_EXIT_USAGE;
    }

    struct stat st;
    if (lstat(args->library_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "nocturned migrate: %s is not a readable directory: %s\n",
                args->library_path, strerror(errno));
        return 3;
    }

    const char *pidfile = paths_pidfile();
    if (!pidfile) {
        fprintf(stderr, "nocturned migrate: cannot resolve pidfile path\n");
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
            return NOCT_EXIT_LOCK_BUSY;
        }
        fprintf(stderr, "nocturned migrate: lock_acquire failed: %s\n", strerror(errno));
        return 1;
    }

    const char *db_path = paths_db_file();
    if (!db_path) {
        fprintf(stderr, "nocturned migrate: cannot resolve DB path\n");
        lock_release(lock);
        return 3;
    }

    struct nocturne_db *db = db_open(db_path, err_to_stderr, NULL);
    if (!db) {
        lock_release(lock);
        return 3;
    }

    struct migrate_stats stats = {0};
    int rc = migrate_run(db, args->library_path, args->apply ? true : false, &stats);

    fprintf(stdout,
            "migrate: planned=%lld moved=%lld already=%lld "
            "skipped_outside=%lld fallback=%lld errors=%lld%s\n",
            stats.planned, stats.moved, stats.already_archived,
            stats.skipped_outside, stats.fallback_copies, stats.errors,
            args->apply ? "" : " (dry-run)");

    db_close(db);
    lock_release(lock);

    if (rc < 0) return 3;
    return stats.errors > 0 ? 1 : 0;
}
