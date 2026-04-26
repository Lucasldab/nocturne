/*
 * scan_cmd.c — `nocturned scan <library>` argv handler.
 *
 * Acquires the single-writer lock from 02-01, opens the daemon DB, runs
 * scan_run, prints a one-line stats summary. Exit codes:
 *
 *   0 — full success.
 *   1 — partial (hash_failed or parse_failed > 0; DB still consistent).
 *   3 — hard error (root inaccessible, db open failed, scan_run -1).
 *   4 — lock busy (another nocturned writing).
 *
 * Stats line format (parseable by humans and by the integration test):
 *   scan: seen=N added=A updated=U removed=R skipped=S parse_failed=P hash_failed=H elapsed=Tms
 */

#define _GNU_SOURCE

#include "cli.h"
#include "db.h"
#include "lock.h"
#include "paths.h"
#include "scan.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static void err_to_stderr(const char *msg, void *ud)
{
    (void) ud;
    fprintf(stderr, "nocturned: %s\n", msg);
}

int scan_cmd_main(struct cli_args *args)
{
    if (!args || !args->library_path) {
        fprintf(stderr, "nocturned scan: missing <library> path\n");
        return 64; /* USAGE */
    }

    struct stat st;
    if (lstat(args->library_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "nocturned scan: %s is not a readable directory: %s\n",
                args->library_path, strerror(errno));
        return 3;
    }

    const char *pidfile = paths_pidfile();
    if (!pidfile) {
        fprintf(stderr, "nocturned scan: cannot resolve pidfile path\n");
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
        fprintf(stderr, "nocturned scan: lock_acquire failed: %s\n", strerror(errno));
        return 1;
    }

    const char *db_path = paths_db_file();
    if (!db_path) {
        fprintf(stderr, "nocturned scan: cannot resolve DB path\n");
        lock_release(lock);
        return 3;
    }

    struct nocturne_db *db = db_open(db_path, err_to_stderr, NULL);
    if (!db) {
        lock_release(lock);
        return 3;
    }

    struct scan_stats stats = {0};
    int rc = scan_run(db, args->library_path, &stats);

    fprintf(stdout,
            "scan: seen=%zu added=%zu updated=%zu removed=%zu "
            "skipped=%zu parse_failed=%zu hash_failed=%zu elapsed=%zums\n",
            stats.files_seen, stats.files_added, stats.files_updated,
            stats.files_removed, stats.files_skipped_unchanged,
            stats.tag_parse_failed, stats.hash_failed, stats.elapsed_ms);

    db_close(db);
    lock_release(lock);

    if (rc < 0) return 3;
    if (rc > 0) return 1;
    return 0;
}
