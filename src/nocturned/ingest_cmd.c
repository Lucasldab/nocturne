/*
 * ingest_cmd.c — `nocturned ingest [--meta-dir <path>] [--dry-run]`.
 *
 * Glob `<meta_dir>/{stats/phone-*.jsonl, likes-phone-*.jsonl,
 * pins-phone-*.jsonl}`, replay events into SQLite (LWW for likes/pins,
 * append-only for plays), persist per-file byte offsets so re-running
 * is a no-op.
 *
 * Lock policy (Phase 2 convention): write subcommand → take the
 * single-writer PID lockfile. Concurrent ingest exits 4.
 *
 * Exit codes:
 *   0 — success.
 *   1 — fatal error (db_open failed, ingest_run returned -1).
 *   3 — config error.
 *   4 — lock busy.
 */

#define _GNU_SOURCE

#include "cli.h"
#include "config.h"
#include "db.h"
#include "ingest.h"
#include "lock.h"
#include "paths.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>

static void err_to_stderr(const char *msg, void *ud)
{
    (void) ud;
    fprintf(stderr, "nocturned: %s\n", msg);
}

/* Compute the default meta_dir when neither --meta-dir nor config
 * sync_meta.path is set. Mirrors the documented default
 * `~/sync/nocturne/meta`. Returned heap string the caller frees, or
 * NULL on OOM / no HOME. */
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

int ingest_cmd_main(struct cli_args *args)
{
    if (!args) return NOCT_EXIT_USAGE;

    /* 1. Lock — write subcommand. */
    const char *pidfile = paths_pidfile();
    if (!pidfile) {
        fprintf(stderr, "nocturned ingest: cannot resolve pidfile path\n");
        return NOCT_EXIT_FAILURE;
    }
    int busy_pid = 0;
    struct nocturne_lock *lock = lock_acquire(pidfile, &busy_pid);
    if (!lock) {
        if (errno == EWOULDBLOCK) {
            fprintf(stderr,
                "nocturned: another instance is running (pid=%d); "
                "single-writer lock at %s\n", busy_pid, pidfile);
            return NOCT_EXIT_LOCK_BUSY;
        }
        fprintf(stderr, "nocturned ingest: lock_acquire failed: %s\n",
                strerror(errno));
        return NOCT_EXIT_FAILURE;
    }

    /* 2. Open DB. */
    const char *db_path = paths_db_file();
    if (!db_path) {
        fprintf(stderr, "nocturned ingest: cannot resolve DB path\n");
        lock_release(lock);
        return NOCT_EXIT_FAILURE;
    }
    struct nocturne_db *db = db_open(db_path, err_to_stderr, NULL);
    if (!db) { lock_release(lock); return NOCT_EXIT_FAILURE; }

    /* 3. Load config (cli arg → XDG → defaults). */
    struct nocturne_config cfg;
    const char *cfg_path = args->config_path ? args->config_path : paths_config_file();
    if (config_load(cfg_path, &cfg) != 0) {
        config_free(&cfg);
        db_close(db); lock_release(lock);
        return 3;
    }

    /* 4. Resolve meta_dir: cli arg > config sync_meta_root > default. */
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
        fprintf(stderr, "nocturned ingest: cannot determine meta directory "
                        "(set [sync_meta] path in config or pass --meta-dir)\n");
        config_free(&cfg);
        db_close(db); lock_release(lock);
        return 3;
    }

    /* 5. Run ingest. */
    struct ingest_stats st = {0};
    int rc = ingest_run(db, meta_dir, &st, args->dry_run);

    /* 6. Print summary line. */
    fprintf(stdout,
        "ingested files=%ld plays=%ld likes=%ld pins=%ld "
        "offsets_advanced=%ld parse_errors=%ld oversize_lines=%ld%s\n",
        st.files_seen, st.plays_inserted, st.likes_upserted,
        st.pins_upserted, st.offsets_advanced,
        st.lines_skipped_parse_error, st.lines_skipped_oversize,
        args->dry_run ? " (dry-run)" : "");

    /* 7. Cleanup. */
    free(fallback_meta);
    config_free(&cfg);
    db_close(db);
    lock_release(lock);

    return rc == 0 ? NOCT_EXIT_OK : NOCT_EXIT_FAILURE;
}
