/*
 * discover_cmd.c — `nocturned discover` argv handler.
 *
 * Picks the week's Discovery tracks and writes them to weekly_discovery_picks.
 * Resolver picks them up on the next `nocturne cycle` via the `weekly_discovery`
 * bucket (count=20 default; configurable in [buckets.weekly_discovery]).
 *
 * Exit codes:
 *   0 — picked at least 1 track
 *   1 — picked 0 (empty library / no candidates) — non-fatal
 *   3 — hard error (lock busy → exit 4 actually, db open failed, run -1)
 *   4 — lock busy
 */

#define _GNU_SOURCE

#include "cli.h"
#include "config.h"
#include "db.h"
#include "discover.h"
#include "lock.h"
#include "paths.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static void err_to_stderr(const char *msg, void *ud)
{
    (void) ud;
    fprintf(stderr, "nocturned: %s\n", msg);
}

int discover_cmd_main(struct cli_args *args)
{
    (void) args;

    const char *pidfile = paths_pidfile();
    int busy_pid = 0;
    struct nocturne_lock *lock = lock_acquire(pidfile, &busy_pid);
    if (!lock) {
        if (errno == EWOULDBLOCK) {
            fprintf(stderr,
                "nocturned: another instance is running (pid=%d)\n", busy_pid);
            return NOCT_EXIT_LOCK_BUSY;
        }
        fprintf(stderr, "nocturned discover: lock_acquire failed: %s\n",
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

    int count = 20;  /* TODO: read from [discover].count if/when added */

    struct discover_stats stats = {0};
    int rc = discover_run(db, count, cfg.discover_exclude_album_substrings, &stats);

    fprintf(stdout,
        "discover: week=%s picked=%lld "
        "(never_played=%lld aged_out=%lld adjacent_to_loved=%lld random=%lld) "
        "candidates=%lld\n",
        stats.week_start, stats.total_picked,
        stats.picked_never_played, stats.picked_aged_out,
        stats.picked_adjacent_to_loved, stats.picked_random,
        stats.candidates_seen);

    config_free(&cfg);
    db_close(db);
    lock_release(lock);

    if (rc < 0) return 3;
    return stats.total_picked > 0 ? 0 : 1;
}
