/*
 * resolve_cmd.c — `nocturned resolve [--dry-run] [--explain] [--config p]`.
 *
 * Computes the manifest and (unless --dry-run) writes manifest_current +
 * manifest_meta. Holds the single-writer lock for the duration. Same exit
 * code matrix as scan_cmd: 0 success, 1 partial/cold-start, 3 hard error,
 * 4 lock busy.
 */

#define _GNU_SOURCE

#include "cli.h"
#include "config.h"
#include "db.h"
#include "lock.h"
#include "paths.h"
#include "resolver.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

static void err_to_stderr(const char *msg, void *ud)
{
    (void) ud;
    fprintf(stderr, "nocturned: %s\n", msg);
}

static int write_manifest_to_db(struct nocturne_db *db, const struct manifest *m)
{
    struct sqlite3 *raw = db_handle(db);
    char *err = NULL;
    if (sqlite3_exec(raw, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        sqlite3_free(err); return -1;
    }
    sqlite3_exec(raw, "DELETE FROM manifest_current", NULL, NULL, NULL);

    sqlite3_stmt *ins = NULL;
    sqlite3_prepare_v2(raw,
        "INSERT INTO manifest_current (sha256, buckets_csv, size_bytes) VALUES (?, ?, ?)",
        -1, &ins, NULL);
    for (size_t i = 0; i < m->resident_n; i++) {
        const struct manifest_track *t = &m->resident[i];
        char buckets_csv[512] = {0};
        size_t cur = 0;
        for (size_t j = 0; j < t->buckets_n; j++) {
            int n = snprintf(buckets_csv + cur, sizeof(buckets_csv) - cur,
                             "%s%s", j ? "," : "", t->buckets[j]);
            if (n < 0 || cur + (size_t) n >= sizeof(buckets_csv)) break;
            cur += n;
        }
        sqlite3_bind_text(ins, 1, t->sha256, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 2, buckets_csv, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(ins, 3, t->size_bytes);
        sqlite3_step(ins);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);

    /* Update manifest_meta. */
    sqlite3_stmt *meta = NULL;
    sqlite3_prepare_v2(raw,
        "INSERT INTO manifest_meta (key, value) VALUES (?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
        -1, &meta, NULL);

    char buf[64];
    /* used_bytes */
    snprintf(buf, sizeof(buf), "%lld", m->used_bytes);
    sqlite3_bind_text(meta, 1, "used_bytes", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(meta, 2, buf, -1, SQLITE_TRANSIENT);
    sqlite3_step(meta); sqlite3_reset(meta);
    /* cap_bytes — required by publisher (02-06) so manifest.json carries it. */
    snprintf(buf, sizeof(buf), "%lld", m->cap_bytes);
    sqlite3_bind_text(meta, 1, "cap_bytes", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(meta, 2, buf, -1, SQLITE_TRANSIENT);
    sqlite3_step(meta); sqlite3_reset(meta);
    /* cap_effective_bytes — the post-headroom budget actually used by the resolver. */
    snprintf(buf, sizeof(buf), "%lld", m->cap_effective_bytes);
    sqlite3_bind_text(meta, 1, "cap_effective_bytes", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(meta, 2, buf, -1, SQLITE_TRANSIENT);
    sqlite3_step(meta); sqlite3_reset(meta);
    /* cold_start */
    sqlite3_bind_text(meta, 1, "cold_start", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(meta, 2, m->cold_start ? "1" : "0", -1, SQLITE_TRANSIENT);
    sqlite3_step(meta); sqlite3_reset(meta);
    /* generated_at */
    sqlite3_bind_text(meta, 1, "generated_at", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(meta, 2, m->generated_at_iso ? m->generated_at_iso : "",
                      -1, SQLITE_TRANSIENT);
    sqlite3_step(meta); sqlite3_reset(meta);
    sqlite3_finalize(meta);

    if (sqlite3_exec(raw, "COMMIT", NULL, NULL, &err) != SQLITE_OK) {
        sqlite3_free(err); return -1;
    }
    return 0;
}

int resolve_cmd_main(struct cli_args *args)
{
    if (!args) return NOCT_EXIT_USAGE;

    /* `--diff` is a read-only tuning view. It implies `--dry-run`
     * (Plan 08-02 contract: never mutate manifest_current under --diff).
     * Reject the combination explicitly so a future fat-finger
     * `nocturned resolve --diff` does NOT silently rotate. */
    if (args->diff && !args->dry_run) {
        fprintf(stderr, "nocturned resolve: --diff requires --dry-run\n");
        return NOCT_EXIT_USAGE;
    }

    const char *pidfile = paths_pidfile();
    int busy_pid = 0;
    struct nocturne_lock *lock = NULL;

    if (args->diff) {
        /* Skip-on-busy: the tuning loop must work while `nocturned watch`
         * holds the writer lock. SQLite WAL gives us a consistent read
         * snapshot even if the writer is mid-transaction. We still try
         * the lock first so a clean-slate run benefits from exclusion;
         * we only proceed unlocked when the lock is *contended*. */
        lock = lock_acquire(pidfile, &busy_pid);
        if (!lock) {
            if (errno == EWOULDBLOCK) {
                fprintf(stderr,
                    "note: another instance holds the writer lock "
                    "(pid=%d); --diff proceeded without lock "
                    "(read-only against WAL)\n", busy_pid);
                /* lock stays NULL; release path is no-op-safe. */
            } else {
                fprintf(stderr, "nocturned resolve: lock_acquire failed: %s\n",
                        strerror(errno));
                return NOCT_EXIT_FAILURE;
            }
        }
    } else {
        /* Existing exclusive-blocking path; unchanged from pre-08-02. */
        lock = lock_acquire(pidfile, &busy_pid);
        if (!lock) {
            if (errno == EWOULDBLOCK) {
                fprintf(stderr,
                    "nocturned: another instance is running (pid=%d); "
                    "single-writer lock at %s\n", busy_pid, pidfile);
                return NOCT_EXIT_LOCK_BUSY;
            }
            fprintf(stderr, "nocturned resolve: lock_acquire failed: %s\n",
                    strerror(errno));
            return NOCT_EXIT_FAILURE;
        }
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

    struct manifest m;
    int rc = resolver_run(db, &cfg, &m);
    if (rc != 0) {
        config_free(&cfg); db_close(db); lock_release(lock);
        return 3;
    }

    if (!args->dry_run) {
        if (write_manifest_to_db(db, &m) != 0) {
            manifest_free(&m); config_free(&cfg);
            db_close(db); lock_release(lock);
            return 3;
        }
    }

    if (args->explain) {
        for (size_t i = 0; i < m.resident_n; i++) {
            const struct manifest_track *t = &m.resident[i];
            printf("%.12s  ", t->sha256);
            for (size_t j = 0; j < t->buckets_n; j++) {
                printf("%s%s", j ? "," : "", t->buckets[j]);
            }
            printf("  %lld\n", t->size_bytes);
        }
    }

    fprintf(stdout,
        "resolve: residents=%zu used=%lld cap=%lld effective=%lld cold_start=%s%s\n",
        m.resident_n, m.used_bytes, m.cap_bytes, m.cap_effective_bytes,
        m.cold_start ? "yes" : "no",
        args->dry_run ? " (dry-run)" : "");

    /* Nudge (NOT auto-invoke) — the rotate engine is a separate write
     * subcommand the user runs explicitly after resolve. UI-priority
     * pin: keyboard / min-keystrokes; the nudge tells the user the
     * next step without forcing it. */
    if (!args->dry_run) {
        fprintf(stderr,
            "note: run `nocturned rotate` to apply the manifest to disk\n");
    }

    int ret = m.cold_start ? 1 : 0;
    manifest_free(&m); config_free(&cfg);
    db_close(db); lock_release(lock);
    return ret;
}
