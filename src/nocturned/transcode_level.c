/*
 * transcode_level.c — re-encode existing resident transcodes at the
 * [transcode] level_lufs loudness target.
 *
 * Copies written before levelling existed carry transcode_gain_db = NULL.
 * For each such resident row this measures the archive FLAC, re-encodes into
 * <library>/.transcode-level-tmp/ — same filesystem, so the rename over the
 * old copy is atomic, but OUTSIDE resident/, which Syncthing shares with the
 * phone (a temp there would be synced half-written, and a crash would leave
 * it on the phone for good) — then records the new size, gain and source
 * loudness. The phone re-syncs only the finished files. Leftover temps from
 * an interrupted run are deleted at start.
 *
 * Driven by `nocturned transcode-level [--apply]`. Default is dry-run.
 * Idempotent: levelled rows have a non-NULL gain and are not selected again.
 */

#define _GNU_SOURCE

#include "cli.h"
#include "config.h"
#include "db.h"
#include "lock.h"
#include "paths.h"
#include "transcode.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sqlite3.h>

struct level_stats {
    long long planned;
    long long levelled;
    long long missing_source;
    long long format_mismatch;
    long long errors;
};

#define TMP_DIR_NAME ".transcode-level-tmp"

/* Create <library>/.transcode-level-tmp and empty it of an interrupted
 * run's leftovers. 0 on success. */
static int prepare_tmp_dir(const char *dir)
{
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "transcode-level: mkdir %s: %s\n", dir, strerror(errno));
        return -1;
    }
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    char p[4096];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        unlink(p);
    }
    closedir(d);
    return 0;
}

/* "<tmpdir>/<sha><ext of dst>" — ffmpeg picks the container from the
 * extension, so it must match the copy being replaced. */
static char *temp_path_for(const char *tmpdir, const char *sha, const char *dst)
{
    const char *base = strrchr(dst, '/');
    const char *ext = strrchr(base ? base : dst, '.');
    size_t n = strlen(tmpdir) + strlen(sha) + (ext ? strlen(ext) : 0) + 2;
    char *out = malloc(n);
    if (out) snprintf(out, n, "%s/%s%s", tmpdir, sha, ext ? ext : "");
    return out;
}

static int record(struct sqlite3 *raw, const char *sha, long long size,
                  const struct transcode_result *res)
{
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(raw,
            "UPDATE residency_state SET transcode_size_bytes=?2, "
            "  transcode_gain_db=?3, transcode_lufs=?4 WHERE sha256=?1",
            -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, sha, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, size);
    /* A levelled encode always reports a finite gain (0 dB when the
     * measurement failed), so the row never stays NULL and is not retried
     * forever on a source ffmpeg cannot measure. */
    sqlite3_bind_double(stmt, 3, isnan(res->gain_db) ? 0.0 : res->gain_db);
    if (isnan(res->source_lufs)) sqlite3_bind_null(stmt, 4);
    else sqlite3_bind_double(stmt, 4, res->source_lufs);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

static void level_one(struct nocturne_db *db, const char *sha,
                      const char *src, const char *dst, const char *fmt,
                      const char *tmpdir,
                      const struct transcode_cfg *tc, int apply,
                      struct level_stats *out)
{
    out->planned++;
    /* The copy is re-encoded in place with the CURRENT config. If the
     * configured format changed since it was written, that would put Opus
     * in an .m4a (or fail): leave it for rotate to replace instead. */
    if (!fmt || strcmp(fmt, tc->format) != 0) {
        fprintf(stderr, "transcode-level: %.12s is %s, config says %s; skipped\n",
                sha, fmt ? fmt : "(unknown)", tc->format);
        out->format_mismatch++;
        return;
    }
    struct stat st;
    if (stat(src, &st) != 0) {
        fprintf(stderr, "transcode-level: source missing for %.12s: %s\n", sha, src);
        out->missing_source++;
        return;
    }
    if (!apply) return;

    char *tmp = temp_path_for(tmpdir, sha, dst);
    if (!tmp) { out->errors++; return; }
    struct transcode_result res;
    if (transcode_audio(src, tmp, tc, &res) != 0) {
        fprintf(stderr, "transcode-level: encode failed for %.12s\n", sha);
        unlink(tmp);
        free(tmp);
        out->errors++;
        return;
    }
    if (stat(tmp, &st) != 0 || rename(tmp, dst) != 0) {
        fprintf(stderr, "transcode-level: replace %s failed: %s\n", dst, strerror(errno));
        unlink(tmp);
        free(tmp);
        out->errors++;
        return;
    }
    free(tmp);
    if (record(db_handle(db), sha, (long long) st.st_size, &res) != 0) {
        fprintf(stderr, "transcode-level: DB update failed for %.12s\n", sha);
        out->errors++;
        return;
    }
    out->levelled++;
}

int transcode_level_cmd_main(struct cli_args *args)
{
    if (!args) return NOCT_EXIT_USAGE;

    const char *pidfile = paths_pidfile();
    int busy_pid = 0;
    struct nocturne_lock *lock = lock_acquire(pidfile, &busy_pid);
    if (!lock) {
        if (errno == EWOULDBLOCK) {
            fprintf(stderr,
                "nocturned: another instance is running (pid=%d)\n", busy_pid);
            return NOCT_EXIT_LOCK_BUSY;
        }
        fprintf(stderr, "nocturned transcode-level: lock_acquire failed: %s\n",
                strerror(errno));
        return 1;
    }

    struct nocturne_config cfg;
    const char *cfg_path = args->config_path ? args->config_path : paths_config_file();
    if (config_load(cfg_path, &cfg) != 0) {
        config_free(&cfg);
        lock_release(lock);
        return 3;
    }
    if (cfg.transcode_level_lufs == 0) {
        fprintf(stderr,
            "transcode-level: [transcode] level_lufs is not set; nothing to do\n");
        config_free(&cfg);
        lock_release(lock);
        return NOCT_EXIT_USAGE;
    }

    struct nocturne_db *db = db_open(paths_db_file(), NULL, NULL);
    if (!db) { config_free(&cfg); lock_release(lock); return 3; }

    struct transcode_cfg tc = {
        .enabled = true,
        .format = cfg.transcode_format ? cfg.transcode_format : "opus",
        .bitrate_kbps = cfg.transcode_bitrate_kbps > 0 ? cfg.transcode_bitrate_kbps : 128,
        .level_lufs = cfg.transcode_level_lufs,
    };
    fprintf(stderr, "transcode-level: target %d LUFS, format=%s bitrate=%dk %s\n",
            tc.level_lufs, tc.format, tc.bitrate_kbps,
            args->apply ? "(APPLY)" : "(dry-run)");

    if (!cfg.library_root) {
        fprintf(stderr, "transcode-level: [library] path is not set\n");
        db_close(db); config_free(&cfg); lock_release(lock);
        return 3;
    }
    char tmpdir[4096];
    snprintf(tmpdir, sizeof(tmpdir), "%s/%s", cfg.library_root, TMP_DIR_NAME);
    if (args->apply && prepare_tmp_dir(tmpdir) != 0) {
        db_close(db); config_free(&cfg); lock_release(lock);
        return 1;
    }

    struct level_stats stats = {0};
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db_handle(db),
            "SELECT r.sha256, t.path, r.transcode_path, r.transcode_format "
            "FROM residency_state r JOIN tracks t ON t.sha256 = r.sha256 "
            "WHERE r.location = 'resident' AND r.transcode_path IS NOT NULL "
            "  AND r.transcode_gain_db IS NULL "
            "ORDER BY r.sha256",
            -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "transcode-level: prepare failed\n");
        db_close(db); config_free(&cfg); lock_release(lock);
        return 3;
    }
    /* Collect first: the UPDATEs below must not run under an open SELECT on
     * the same rows. A short read (step error, OOM) is an error, not a
     * smaller job that reports success. */
    enum { NC = 4 };
    size_t n = 0, cap = 64;
    char **rows = malloc(cap * NC * sizeof(char *));
    int step = SQLITE_ERROR, oom = rows == NULL;
    while (!oom && (step = sqlite3_step(st)) == SQLITE_ROW) {
        if (n == cap) {
            char **nr = realloc(rows, cap * 2 * NC * sizeof(char *));
            if (!nr) { oom = 1; break; }
            rows = nr; cap *= 2;
        }
        for (int c = 0; c < NC; c++) {
            const unsigned char *v = sqlite3_column_text(st, c);
            rows[n * NC + c] = v ? strdup((const char *) v) : NULL;
            if (v && !rows[n * NC + c]) oom = 1;
        }
        n++;
    }
    sqlite3_finalize(st);
    if (oom || step != SQLITE_DONE) {
        fprintf(stderr, "transcode-level: reading the work list failed (%s)\n",
                oom ? "out of memory" : sqlite3_errstr(step));
        stats.errors++;
    } else {
        for (size_t i = 0; i < n; i++) {
            char **r = rows + i * NC;
            if (r[0] && r[1] && r[2])
                level_one(db, r[0], r[1], r[2], r[3], tmpdir, &tc, args->apply, &stats);
        }
    }
    for (size_t i = 0; rows && i < n * NC; i++) free(rows[i]);
    free(rows);

    fprintf(stdout,
        "transcode-level: planned=%lld levelled=%lld missing_source=%lld "
        "format_mismatch=%lld errors=%lld %s\n",
        stats.planned, stats.levelled, stats.missing_source,
        stats.format_mismatch, stats.errors,
        args->apply ? "" : "(dry-run)");

    db_close(db);
    config_free(&cfg);
    lock_release(lock);
    return stats.errors > 0 ? 1 : 0;
}
