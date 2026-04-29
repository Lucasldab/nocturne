/*
 * transcode_migrate.c — convert existing FLAC residents into Opus (or AAC).
 *
 * Pre-state (legacy rotate): the daemon's existing rotate.c uses
 * link()+unlink() to MOVE FLACs from archive/ into resident/. Net effect:
 *   - tracks.path points at resident/<rel>
 *   - archive/<rel> doesn't exist on disk
 *   - resident/<rel> is a hardlink to the inode that USED to be archive/<rel>
 *
 * For transcode mode to work, archive/<rel> must exist as the canonical
 * source (the FLAC). We recreate the archive copy by hardlinking from the
 * resident FLAC (zero extra storage), then we transcode resident/<rel> →
 * resident/<rel>.opus via ffmpeg, drop the resident FLAC hardlink (archive
 * keeps its hardlink), and write residency_state with the transcode
 * metadata.
 *
 * Tracks NOT currently resident (location='archive') are no-ops here.
 *
 * Idempotent on re-run: tracks already at archive/<rel> with a transcode
 * file present + DB row are skipped.
 *
 * Driven by `nocturned transcode-migrate [--apply]`. Default is dry-run.
 */

#define _GNU_SOURCE

#include "cli.h"
#include "config.h"
#include "db.h"
#include "lock.h"
#include "paths.h"
#include "track_repo.h"
#include "transcode.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

struct migrate_stats {
    long long planned;
    long long migrated;
    long long already;
    long long errors;
    long long skipped_archived; /* track was already at location='archive' */
};

static int mkparents(const char *path)
{
    char *dup = strdup(path);
    if (!dup) return -1;
    for (char *p = dup + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(dup, 0775) != 0 && errno != EEXIST) {
                free(dup);
                return -1;
            }
            *p = '/';
        }
    }
    free(dup);
    return 0;
}

static char *swap_segment_simple(const char *cur,
                                 const char *from, const char *to)
{
    /* `cur` is an absolute path. We only swap the FIRST occurrence of
     * "/<from>/" → "/<to>/". Mirrors rotate.c's swap_segment but doesn't
     * require a library_root prefix check (caller stat'd the path). */
    char needle[64];
    snprintf(needle, sizeof(needle), "/%s/", from);
    const char *p = strstr(cur, needle);
    if (!p) return NULL;
    size_t prefix_len = (size_t)(p - cur);
    size_t from_len = strlen(from);
    size_t to_len = strlen(to);
    size_t tail_len = strlen(p + 1 + from_len);  /* skip leading '/' + from */
    char *out = malloc(prefix_len + 1 + to_len + tail_len + 1);
    if (!out) return NULL;
    memcpy(out, cur, prefix_len);
    out[prefix_len] = '/';
    memcpy(out + prefix_len + 1, to, to_len);
    memcpy(out + prefix_len + 1 + to_len, p + 1 + from_len, tail_len);
    out[prefix_len + 1 + to_len + tail_len] = '\0';
    return out;
}

static char *path_with_ext(const char *path, const char *new_ext)
{
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    size_t prefix_len = dot ? (size_t)(dot - path) : strlen(path);
    size_t ext_len = strlen(new_ext);
    char *out = malloc(prefix_len + ext_len + 1);
    if (!out) return NULL;
    memcpy(out, path, prefix_len);
    memcpy(out + prefix_len, new_ext, ext_len);
    out[prefix_len + ext_len] = '\0';
    return out;
}

static int upsert_residency_with_transcode(struct sqlite3 *raw, const char *sha,
                                           const char *iso,
                                           const char *transcode_path,
                                           long long transcode_size,
                                           const char *transcode_format)
{
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(raw,
            "INSERT INTO residency_state (sha256, location, updated_at, "
            "  transcode_path, transcode_size_bytes, transcode_format) "
            "VALUES (?, 'resident', ?, ?, ?, ?) "
            "ON CONFLICT(sha256) DO UPDATE SET location='resident', "
            "  updated_at=excluded.updated_at, "
            "  transcode_path=excluded.transcode_path, "
            "  transcode_size_bytes=excluded.transcode_size_bytes, "
            "  transcode_format=excluded.transcode_format",
            -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, sha, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, iso, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, transcode_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, transcode_size);
    sqlite3_bind_text(stmt, 5, transcode_format, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

static void iso_now_buf(char buf[40])
{
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(buf, 40, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static int migrate_one(struct nocturne_db *db, const char *sha,
                       const char *cur_path, const char *location,
                       const struct transcode_cfg *tc, int apply,
                       const char *iso, struct migrate_stats *out)
{
    if (strcmp(location, "resident") != 0) {
        out->skipped_archived++;
        return 0;
    }

    /* Compute the archive twin of the resident path. */
    char *archive_path = swap_segment_simple(cur_path, "resident", "archive");
    if (!archive_path) {
        fprintf(stderr,
            "transcode-migrate: %.*s tracks.path %.*s has no /resident/ "
            "segment; skipping\n",
            (int) strlen(sha), sha, (int) strlen(cur_path), cur_path);
        out->errors++;
        return -1;
    }

    /* Compute resident transcode destination (different extension). */
    const char *new_ext = transcode_dst_ext(tc);
    if (!new_ext) {
        fprintf(stderr, "transcode-migrate: unsupported format %s\n",
                tc->format ? tc->format : "(null)");
        out->errors++;
        free(archive_path);
        return -1;
    }
    char *transcode_path = path_with_ext(cur_path, new_ext);
    if (!transcode_path) {
        out->errors++;
        free(archive_path);
        return -1;
    }

    /* Idempotence: if archive_path already exists AND transcode_path exists
     * AND DB row already has the transcode metadata, this row is done. We
     * detect by checking the on-disk files; the DB row is updated below
     * regardless to refresh updated_at. */
    struct stat ast, tst;
    int archive_present = (stat(archive_path, &ast) == 0 && S_ISREG(ast.st_mode));
    int transcode_present = (stat(transcode_path, &tst) == 0 && S_ISREG(tst.st_mode));

    if (archive_present && transcode_present) {
        out->already++;
        free(archive_path); free(transcode_path);
        return 0;
    }

    out->planned++;
    if (!apply) {
        free(archive_path); free(transcode_path);
        return 0;
    }

    /* Step 1: ensure archive_path exists by hardlinking from cur_path.
     * Hardlinks share the inode → zero extra storage. EEXIST + same inode
     * = already done; any other error = real failure. */
    if (!archive_present) {
        if (mkparents(archive_path) != 0) {
            fprintf(stderr,
                "transcode-migrate: mkdir -p %s failed: %s\n",
                archive_path, strerror(errno));
            out->errors++;
            free(archive_path); free(transcode_path);
            return -1;
        }
        if (link(cur_path, archive_path) != 0) {
            int e = errno;
            if (e == EEXIST) {
                /* Verify it's the same inode; otherwise refuse. */
                struct stat a, b;
                if (stat(cur_path, &a) == 0 && stat(archive_path, &b) == 0 &&
                    a.st_dev == b.st_dev && a.st_ino == b.st_ino) {
                    /* fine, archive copy already in place */
                } else {
                    fprintf(stderr,
                        "transcode-migrate: %s exists with a different inode "
                        "than %s; refusing to overwrite\n",
                        archive_path, cur_path);
                    out->errors++;
                    free(archive_path); free(transcode_path);
                    return -1;
                }
            } else {
                fprintf(stderr,
                    "transcode-migrate: link(%s, %s) failed: %s\n",
                    cur_path, archive_path, strerror(e));
                out->errors++;
                free(archive_path); free(transcode_path);
                return -1;
            }
        }
    }

    /* Step 2: transcode resident FLAC → resident transcode (opus/m4a). */
    if (!transcode_present) {
        if (mkparents(transcode_path) != 0) {
            fprintf(stderr,
                "transcode-migrate: mkdir -p %s failed: %s\n",
                transcode_path, strerror(errno));
            out->errors++;
            free(archive_path); free(transcode_path);
            return -1;
        }
        int trc = transcode_audio(cur_path, transcode_path, tc);
        if (trc != 0) {
            fprintf(stderr,
                "transcode-migrate: transcode %s failed (rc=%d)\n",
                cur_path, trc);
            unlink(transcode_path);  /* drop partial output */
            out->errors++;
            free(archive_path); free(transcode_path);
            return -1;
        }
        if (stat(transcode_path, &tst) != 0) {
            fprintf(stderr, "transcode-migrate: stat(%s) failed: %s\n",
                    transcode_path, strerror(errno));
            out->errors++;
            free(archive_path); free(transcode_path);
            return -1;
        }
    }

    /* Step 3: drop the resident FLAC hardlink (archive copy still present). */
    if (unlink(cur_path) != 0 && errno != ENOENT) {
        fprintf(stderr, "transcode-migrate: unlink(%s) failed: %s\n",
                cur_path, strerror(errno));
        out->errors++;
        free(archive_path); free(transcode_path);
        return -1;
    }

    /* Step 4: tracks.path = archive_path. */
    if (track_repo_update_path(db, sha, archive_path, iso) != 0) {
        fprintf(stderr,
            "transcode-migrate: tracks.path update for %.*s failed\n",
            (int) strlen(sha), sha);
        out->errors++;
        free(archive_path); free(transcode_path);
        return -1;
    }

    /* Step 5: residency_state row → location='resident' + transcode_*. */
    if (upsert_residency_with_transcode(db_handle(db), sha, iso,
                                        transcode_path,
                                        (long long) tst.st_size,
                                        tc->format) != 0) {
        fprintf(stderr,
            "transcode-migrate: residency upsert for %.*s failed\n",
            (int) strlen(sha), sha);
        out->errors++;
        free(archive_path); free(transcode_path);
        return -1;
    }

    out->migrated++;
    free(archive_path);
    free(transcode_path);
    return 0;
}

int transcode_migrate_cmd_main(struct cli_args *args)
{
    if (!args) return NOCT_EXIT_USAGE;

    /* Lock — destructive, single-writer. */
    const char *pidfile = paths_pidfile();
    int busy_pid = 0;
    struct nocturne_lock *lock = lock_acquire(pidfile, &busy_pid);
    if (!lock) {
        if (errno == EWOULDBLOCK) {
            fprintf(stderr,
                "nocturned: another instance is running (pid=%d)\n", busy_pid);
            return NOCT_EXIT_LOCK_BUSY;
        }
        fprintf(stderr, "nocturned transcode-migrate: lock_acquire failed: %s\n",
                strerror(errno));
        return 1;
    }

    const char *db_path = paths_db_file();
    struct nocturne_db *db = db_open(db_path, NULL, NULL);
    if (!db) { lock_release(lock); return 3; }

    struct nocturne_config cfg;
    const char *cfg_path = args->config_path ? args->config_path : paths_config_file();
    if (config_load(cfg_path, &cfg) != 0) {
        config_free(&cfg);
        db_close(db); lock_release(lock);
        return 3;
    }

    struct transcode_cfg tc = {
        .enabled = true,  /* this command implies transcode */
        .format = args->transcode_format
                  ? args->transcode_format
                  : (cfg.transcode_format ? cfg.transcode_format : "opus"),
        .bitrate_kbps = args->transcode_bitrate_kbps > 0
                        ? args->transcode_bitrate_kbps
                        : (cfg.transcode_bitrate_kbps > 0
                           ? cfg.transcode_bitrate_kbps : 128),
    };

    fprintf(stderr,
        "transcode-migrate: format=%s bitrate=%dk %s\n",
        tc.format, tc.bitrate_kbps, args->apply ? "(APPLY)" : "(dry-run)");

    char iso[40];
    iso_now_buf(iso);
    struct migrate_stats stats = {0};

    /* Walk every tracks row joined to its residency state. */
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db_handle(db),
            "SELECT t.sha256, t.path, COALESCE(r.location,'archive') "
            "FROM tracks t LEFT JOIN residency_state r ON r.sha256=t.sha256 "
            "ORDER BY t.sha256",
            -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "transcode-migrate: prepare failed\n");
        config_free(&cfg); db_close(db); lock_release(lock);
        return 3;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *sha = (const char *) sqlite3_column_text(st, 0);
        const char *path = (const char *) sqlite3_column_text(st, 1);
        const char *loc = (const char *) sqlite3_column_text(st, 2);
        if (!sha || !path || !loc) continue;
        migrate_one(db, sha, path, loc, &tc, args->apply, iso, &stats);
    }
    sqlite3_finalize(st);

    fprintf(stdout,
        "transcode-migrate: planned=%lld migrated=%lld already=%lld "
        "skipped_archived=%lld errors=%lld %s\n",
        stats.planned, stats.migrated, stats.already,
        stats.skipped_archived, stats.errors,
        args->apply ? "" : "(dry-run)");

    config_free(&cfg);
    db_close(db);
    lock_release(lock);
    return stats.errors > 0 ? 1 : 0;
}
