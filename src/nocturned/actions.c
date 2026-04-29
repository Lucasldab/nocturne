/*
 * actions.c — unsync + delete-everywhere implementations.
 *
 * Both flow from phone long-press JSONL writes (unsync-phone-*.jsonl /
 * delete-phone-*.jsonl) ingested by the existing phone-stats path, OR from
 * the manual `nocturned unsync <sha>` / `nocturned delete <sha>` CLIs for
 * testing without the phone in the loop.
 *
 * unsync_track:
 *   - Clear pin (UPDATE pins SET pinned=0 WHERE id=?)
 *   - Insert unsync_overrides row (sha, until=NULL, added_at=now)
 *   - resolver.c reads unsync_overrides, demotes regardless of bucket
 *   - rotate.c (transcode-mode demote) deletes resident/<rel>.opus, archive
 *     untouched, residency_state.location='archive'
 *   - All stats survive (likes/plays/pins-history); re-pinning restores
 *
 * delete_track_everywhere:
 *   - Read tracks.path (archive) and residency_state.transcode_path (resident)
 *   - unlink both files (ENOENT is fine)
 *   - INSERT INTO track_blacklist (sha, reason='didnt_like', added_at)
 *   - DELETE FROM tracks WHERE sha256=? (cascades pins/likes/plays/residency
 *     via existing FKs)
 *   - scan.c on next walk refuses to insert a row for any blacklisted sha,
 *     so re-imports (e.g. streamrip re-running the same Spotify CSV) skip
 *
 * Album-level: resolves album_id → all track shas → iterate. After all
 * tracks deleted, attempt rmdir on the now-empty album + artist dirs.
 */

#define _GNU_SOURCE

#include "actions.h"
#include "db.h"
#include "track_repo.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

static void iso_now_buf(char buf[40])
{
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(buf, 40, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* Pull a single TEXT column from a single-row query. Caller frees. */
static char *fetch_text(struct sqlite3 *raw, const char *sql, const char *bind)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(raw, sql, -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(st, 1, bind, -1, SQLITE_TRANSIENT);
    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *t = sqlite3_column_text(st, 0);
        if (t) out = strdup((const char *) t);
    }
    sqlite3_finalize(st);
    return out;
}

static long long fetch_size_bytes(struct sqlite3 *raw, const char *sha)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(raw,
            "SELECT size_bytes FROM tracks WHERE sha256=?",
            -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, sha, -1, SQLITE_TRANSIENT);
    long long n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

int unsync_track(struct nocturne_db *db, const char *sha, struct action_stats *out)
{
    if (!db || !sha || !out) return -1;
    struct sqlite3 *raw = db_handle(db);
    if (!raw) return -1;
    char iso[40]; iso_now_buf(iso);

    /* Clear any pin. */
    {
        sqlite3_stmt *st = NULL;
        sqlite3_prepare_v2(raw,
            "UPDATE pins SET pinned=0, updated_at=? "
            "WHERE id=? AND unit='track'", -1, &st, NULL);
        sqlite3_bind_text(st, 1, iso, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, sha, -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    /* Add the override. */
    {
        sqlite3_stmt *st = NULL;
        sqlite3_prepare_v2(raw,
            "INSERT INTO unsync_overrides (sha256, until, added_at) "
            "VALUES (?, NULL, ?) "
            "ON CONFLICT(sha256) DO UPDATE SET added_at=excluded.added_at",
            -1, &st, NULL);
        sqlite3_bind_text(st, 1, sha, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, iso, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) != SQLITE_DONE) { sqlite3_finalize(st); out->errors++; return -1; }
        sqlite3_finalize(st);
    }
    out->touched++;
    return 0;
}

int delete_track_everywhere(struct nocturne_db *db, const char *sha,
                            struct action_stats *out)
{
    if (!db || !sha || !out) return -1;
    struct sqlite3 *raw = db_handle(db);
    if (!raw) return -1;
    char iso[40]; iso_now_buf(iso);

    long long size = fetch_size_bytes(raw, sha);

    char *archive_path = fetch_text(raw, "SELECT path FROM tracks WHERE sha256=?", sha);
    char *transcode_path = fetch_text(raw,
        "SELECT transcode_path FROM residency_state WHERE sha256=?", sha);

    /* Unlink files (ENOENT non-fatal — file may already be gone). */
    if (archive_path) {
        if (unlink(archive_path) == 0) {
            out->files_removed++;
            out->bytes_freed += size;
        } else if (errno != ENOENT) {
            fprintf(stderr, "delete: unlink(%s): %s\n", archive_path, strerror(errno));
            out->errors++;
        }
    }
    if (transcode_path) {
        if (unlink(transcode_path) == 0) {
            out->files_removed++;
            /* transcode size lookup is in the row we're about to drop; let it go */
        } else if (errno != ENOENT) {
            fprintf(stderr, "delete: unlink(%s): %s\n", transcode_path, strerror(errno));
            out->errors++;
        }
    }

    /* Blacklist FIRST so subsequent scans refuse to re-add even if the row
     * delete races a concurrent scan tick. */
    {
        sqlite3_stmt *st = NULL;
        sqlite3_prepare_v2(raw,
            "INSERT INTO track_blacklist (sha256, reason, added_at) "
            "VALUES (?, 'didnt_like', ?) "
            "ON CONFLICT(sha256) DO UPDATE SET added_at=excluded.added_at",
            -1, &st, NULL);
        sqlite3_bind_text(st, 1, sha, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, iso, -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }

    /* Cascade DELETE on tracks row. FKs in pins/likes/plays/residency_state/
     * manifest_current/weekly_discovery_picks all use ON DELETE CASCADE. */
    {
        sqlite3_stmt *st = NULL;
        sqlite3_prepare_v2(raw, "DELETE FROM tracks WHERE sha256=?", -1, &st, NULL);
        sqlite3_bind_text(st, 1, sha, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) != SQLITE_DONE) {
            sqlite3_finalize(st);
            free(archive_path); free(transcode_path);
            out->errors++; return -1;
        }
        sqlite3_finalize(st);
    }

    out->touched++;
    free(archive_path); free(transcode_path);
    return 0;
}

/* Resolve album_id → list of track shas. Caller frees `shas` (an array of
 * strdup'd strings) and the array itself. Returns count, -1 on error. */
static long long album_track_shas(struct sqlite3 *raw, const char *album_id,
                                  char ***shas_out)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(raw,
            "SELECT sha256 FROM tracks WHERE album_id=? ORDER BY sha256",
            -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, album_id, -1, SQLITE_TRANSIENT);
    char **arr = NULL;
    long long n = 0, cap = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *s = sqlite3_column_text(st, 0);
        if (!s) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            arr = realloc(arr, cap * sizeof(*arr));
        }
        arr[n++] = strdup((const char *) s);
    }
    sqlite3_finalize(st);
    *shas_out = arr;
    return n;
}

int unsync_album(struct nocturne_db *db, const char *album_id,
                 struct action_stats *out)
{
    if (!db || !album_id || !out) return -1;
    char **shas = NULL;
    long long n = album_track_shas(db_handle(db), album_id, &shas);
    if (n < 0) return -1;
    for (long long i = 0; i < n; i++) {
        unsync_track(db, shas[i], out);
        free(shas[i]);
    }
    free(shas);
    return 0;
}

/* rmdir parents up to library_root if empty. Best-effort. */
static void prune_empty_dirs(const char *path)
{
    char *dup = strdup(path);
    if (!dup) return;
    char *slash = strrchr(dup, '/');
    while (slash && slash > dup) {
        *slash = '\0';
        if (rmdir(dup) != 0) break;  /* not empty / no perms / hit boundary */
        slash = strrchr(dup, '/');
    }
    free(dup);
}

int delete_album_everywhere(struct nocturne_db *db, const char *album_id,
                            struct action_stats *out)
{
    if (!db || !album_id || !out) return -1;
    char **shas = NULL;
    long long n = album_track_shas(db_handle(db), album_id, &shas);
    if (n < 0) return -1;

    /* Capture one path before deleting so we know the album dir to prune. */
    char *sample_archive = NULL;
    char *sample_transcode = NULL;
    if (n > 0) {
        sample_archive = fetch_text(db_handle(db),
            "SELECT path FROM tracks WHERE sha256=?", shas[0]);
        sample_transcode = fetch_text(db_handle(db),
            "SELECT transcode_path FROM residency_state WHERE sha256=?", shas[0]);
    }

    for (long long i = 0; i < n; i++) {
        delete_track_everywhere(db, shas[i], out);
        free(shas[i]);
    }
    free(shas);

    /* Prune empty album + artist dirs (rmdir refuses non-empty dirs). */
    if (sample_archive) prune_empty_dirs(sample_archive);
    if (sample_transcode) prune_empty_dirs(sample_transcode);
    free(sample_archive);
    free(sample_transcode);
    return 0;
}
