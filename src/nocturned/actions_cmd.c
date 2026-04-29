/*
 * actions_cmd.c — `nocturned unsync` + `nocturned delete` CLI handlers.
 *
 * Single-writer lock acquired for the duration. Both ops are no-ops if the
 * target sha doesn't exist (no row → nothing to unsync/delete; exit 0).
 *
 * Exit codes:
 *   0  success (touched >= 1)
 *   1  partial (touched > 0 && errors > 0) or no rows matched
 *   3  hard error (lock failure, db open failed, run -1, missing positional)
 *   4  lock busy
 */

#define _GNU_SOURCE

#include "actions.h"
#include "cli.h"
#include "db.h"
#include "lock.h"
#include "paths.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int run_with_lock(struct cli_args *args, int is_delete)
{
    if (!args || !args->action_target) {
        fprintf(stderr,
            "nocturned %s: missing positional (sha256 hex or album_id)\n",
            is_delete ? "delete" : "unsync");
        return NOCT_EXIT_USAGE;
    }

    if (is_delete && !args->action_yes) {
        fprintf(stderr,
            "nocturned delete: this is destructive (unlinks archive/ + "
            "resident/ files, blacklists the content, drops DB rows).\n"
            "Re-run with --yes to confirm.\n");
        return NOCT_EXIT_USAGE;
    }

    const char *pidfile = paths_pidfile();
    int busy_pid = 0;
    struct nocturne_lock *lock = lock_acquire(pidfile, &busy_pid);
    if (!lock) {
        if (errno == EWOULDBLOCK) {
            fprintf(stderr,
                "nocturned: another instance is running (pid=%d)\n", busy_pid);
            return NOCT_EXIT_LOCK_BUSY;
        }
        fprintf(stderr, "nocturned: lock_acquire failed: %s\n", strerror(errno));
        return 1;
    }

    const char *db_path = paths_db_file();
    struct nocturne_db *db = db_open(db_path, NULL, NULL);
    if (!db) { lock_release(lock); return 3; }

    struct action_stats stats = {0};
    int rc;
    if (args->action_is_album) {
        rc = is_delete
             ? delete_album_everywhere(db, args->action_target, &stats)
             : unsync_album(db, args->action_target, &stats);
    } else {
        rc = is_delete
             ? delete_track_everywhere(db, args->action_target, &stats)
             : unsync_track(db, args->action_target, &stats);
    }

    fprintf(stdout,
        "%s: target=%s%s touched=%lld files_removed=%lld bytes_freed=%lld errors=%lld\n",
        is_delete ? "delete" : "unsync",
        args->action_target,
        args->action_is_album ? " (album)" : "",
        stats.touched, stats.files_removed, stats.bytes_freed, stats.errors);

    db_close(db);
    lock_release(lock);

    if (rc < 0) return 3;
    if (stats.touched == 0) return 1;
    return stats.errors > 0 ? 1 : 0;
}

int unsync_cmd_main(struct cli_args *args) { return run_with_lock(args, 0); }
int delete_cmd_main(struct cli_args *args) { return run_with_lock(args, 1); }
