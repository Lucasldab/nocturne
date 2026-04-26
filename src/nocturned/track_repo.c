/*
 * track_repo.c — schema-v1 tracks-table CRUD with prepared-statement cache.
 *
 * Statements are prepared lazily on first call and cached in module-static
 * storage keyed by the db handle. db_register_finalize_hook (added in
 * 02-02) wires sqlite3_finalize to db_close so the close sees a clean
 * sqlite3 handle.
 *
 * Module-static cache rationale: a 10k-track scan executes the upsert
 * statement 10k times; preparing once and resetting is materially faster
 * than re-preparing per call. We don't bother with a hashmap of (db ->
 * stmt-set) because the daemon opens exactly one DB; on close the hooks
 * finalise and reset back to NULL so re-opens re-prepare.
 *
 * Threading: scan/resolve/publish are all single-threaded under the
 * single-writer lock from 02-01. No mutex needed.
 */

#define _GNU_SOURCE

#include "track_repo.h"
#include "db.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct stmt_cache {
    struct nocturne_db *owner;
    sqlite3_stmt *insert;
    sqlite3_stmt *update;
    sqlite3_stmt *update_path;
    sqlite3_stmt *mark_seen;
    sqlite3_stmt *lookup_by_path;
    sqlite3_stmt *lookup_by_sha256;
    sqlite3_stmt *delete_unseen;
    sqlite3_stmt *count;
    bool finalize_hook_registered;
};

static struct stmt_cache g_cache;

static void finalize_all(void *ud)
{
    (void) ud;
    sqlite3_finalize(g_cache.insert);              g_cache.insert = NULL;
    sqlite3_finalize(g_cache.update);              g_cache.update = NULL;
    sqlite3_finalize(g_cache.update_path);         g_cache.update_path = NULL;
    sqlite3_finalize(g_cache.mark_seen);           g_cache.mark_seen = NULL;
    sqlite3_finalize(g_cache.lookup_by_path);      g_cache.lookup_by_path = NULL;
    sqlite3_finalize(g_cache.lookup_by_sha256);    g_cache.lookup_by_sha256 = NULL;
    sqlite3_finalize(g_cache.delete_unseen);       g_cache.delete_unseen = NULL;
    sqlite3_finalize(g_cache.count);               g_cache.count = NULL;
    g_cache.owner = NULL;
    g_cache.finalize_hook_registered = false;
}

static int prep(struct nocturne_db *db, sqlite3_stmt **out, const char *sql)
{
    if (!*out) {
        sqlite3 *raw = db_handle(db);
        if (!raw) return -1;
        if (sqlite3_prepare_v2(raw, sql, -1, out, NULL) != SQLITE_OK) {
            fprintf(stderr, "track_repo: prepare failed: %s\n", sqlite3_errmsg(raw));
            return -1;
        }
    } else {
        sqlite3_reset(*out);
        sqlite3_clear_bindings(*out);
    }
    return 0;
}

/* Lazily attach our finalize hook the first time the module is used against
 * `db`. Re-attach if the cache is for a different db (e.g. tests that
 * open / close repeatedly). */
static int ensure_cache_for(struct nocturne_db *db)
{
    if (g_cache.owner != db) {
        finalize_all(NULL);
        g_cache.owner = db;
    }
    if (!g_cache.finalize_hook_registered) {
        if (db_register_finalize_hook(db, finalize_all, NULL) != 0) {
            return -1;
        }
        g_cache.finalize_hook_registered = true;
    }
    return 0;
}

/* Bind a TEXT param; NULL pointer becomes SQL NULL. */
static int bind_text(sqlite3_stmt *s, int idx, const char *v)
{
    if (!v) return sqlite3_bind_null(s, idx);
    return sqlite3_bind_text(s, idx, v, -1, SQLITE_TRANSIENT);
}

static const char *INSERT_SQL =
    "INSERT INTO tracks (sha256, path, mtime_ns, size_bytes, format, "
    "title, artist, album, album_artist, track_number, disc_number, year, "
    "genre, duration_ms, tags_status, tag_warning, date_added, last_seen_at) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

static const char *UPDATE_SQL =
    "UPDATE tracks SET path=?, mtime_ns=?, size_bytes=?, format=?, "
    "title=?, artist=?, album=?, album_artist=?, track_number=?, "
    "disc_number=?, year=?, genre=?, duration_ms=?, tags_status=?, "
    "tag_warning=?, last_seen_at=? WHERE sha256=?";

static const char *UPDATE_PATH_SQL =
    "UPDATE tracks SET path=?, last_seen_at=? WHERE sha256=?";

static const char *MARK_SEEN_SQL =
    "UPDATE tracks SET last_seen_at=? WHERE sha256=?";

static const char *LOOKUP_BY_PATH_SQL =
    "SELECT sha256, mtime_ns, size_bytes FROM tracks WHERE path=?";

static const char *LOOKUP_BY_SHA_SQL =
    "SELECT 1 FROM tracks WHERE sha256=?";

static const char *DELETE_UNSEEN_SQL =
    "DELETE FROM tracks WHERE path LIKE ? AND last_seen_at < ?";

static const char *COUNT_SQL =
    "SELECT COUNT(*) FROM tracks";

int track_repo_insert(struct nocturne_db *db, const struct track_row *row)
{
    if (!db || !row || !row->sha256 || !row->path) return -1;
    if (ensure_cache_for(db) != 0) return -1;
    if (prep(db, &g_cache.insert, INSERT_SQL) != 0) return -1;
    sqlite3_stmt *s = g_cache.insert;
    int i = 1;
    bind_text(s, i++, row->sha256);
    bind_text(s, i++, row->path);
    sqlite3_bind_int64(s, i++, row->mtime_ns);
    sqlite3_bind_int64(s, i++, row->size_bytes);
    bind_text(s, i++, row->format);
    bind_text(s, i++, row->title);
    bind_text(s, i++, row->artist);
    bind_text(s, i++, row->album);
    bind_text(s, i++, row->album_artist);
    bind_text(s, i++, row->track_number);
    bind_text(s, i++, row->disc_number);
    bind_text(s, i++, row->year);
    bind_text(s, i++, row->genre);
    if (row->duration_ms < 0) sqlite3_bind_null(s, i++);
    else sqlite3_bind_int64(s, i++, row->duration_ms);
    bind_text(s, i++, row->tags_status ? row->tags_status : "ok");
    bind_text(s, i++, row->tag_warning);
    bind_text(s, i++, row->date_added ? row->date_added : row->last_seen_at);
    bind_text(s, i++, row->last_seen_at);

    int rc = sqlite3_step(s);
    sqlite3_reset(s);
    return rc == SQLITE_DONE ? 0 : -1;
}

int track_repo_update(struct nocturne_db *db, const struct track_row *row)
{
    if (!db || !row || !row->sha256) return -1;
    if (ensure_cache_for(db) != 0) return -1;
    if (prep(db, &g_cache.update, UPDATE_SQL) != 0) return -1;
    sqlite3_stmt *s = g_cache.update;
    int i = 1;
    bind_text(s, i++, row->path);
    sqlite3_bind_int64(s, i++, row->mtime_ns);
    sqlite3_bind_int64(s, i++, row->size_bytes);
    bind_text(s, i++, row->format);
    bind_text(s, i++, row->title);
    bind_text(s, i++, row->artist);
    bind_text(s, i++, row->album);
    bind_text(s, i++, row->album_artist);
    bind_text(s, i++, row->track_number);
    bind_text(s, i++, row->disc_number);
    bind_text(s, i++, row->year);
    bind_text(s, i++, row->genre);
    if (row->duration_ms < 0) sqlite3_bind_null(s, i++);
    else sqlite3_bind_int64(s, i++, row->duration_ms);
    bind_text(s, i++, row->tags_status ? row->tags_status : "ok");
    bind_text(s, i++, row->tag_warning);
    bind_text(s, i++, row->last_seen_at);
    bind_text(s, i++, row->sha256);

    int rc = sqlite3_step(s);
    int changes = sqlite3_changes(db_handle(db));
    sqlite3_reset(s);
    if (rc != SQLITE_DONE) return -1;
    return changes > 0 ? 0 : -1;
}

int track_repo_upsert(struct nocturne_db *db, const struct track_row *row)
{
    if (!db || !row) return -1;
    int hit = track_repo_lookup_by_sha256(db, row->sha256);
    if (hit < 0) return -1;
    if (hit == 1) return track_repo_update(db, row);
    return track_repo_insert(db, row);
}

int track_repo_update_path(struct nocturne_db *db, const char *sha256,
                           const char *new_path, const char *iso_now)
{
    if (!db || !sha256 || !new_path || !iso_now) return -1;
    if (ensure_cache_for(db) != 0) return -1;
    if (prep(db, &g_cache.update_path, UPDATE_PATH_SQL) != 0) return -1;
    sqlite3_stmt *s = g_cache.update_path;
    bind_text(s, 1, new_path);
    bind_text(s, 2, iso_now);
    bind_text(s, 3, sha256);
    int rc = sqlite3_step(s);
    sqlite3_reset(s);
    return rc == SQLITE_DONE ? 0 : -1;
}

int track_repo_mark_seen(struct nocturne_db *db, const char *sha256,
                         const char *iso_now)
{
    if (!db || !sha256 || !iso_now) return -1;
    if (ensure_cache_for(db) != 0) return -1;
    if (prep(db, &g_cache.mark_seen, MARK_SEEN_SQL) != 0) return -1;
    sqlite3_stmt *s = g_cache.mark_seen;
    bind_text(s, 1, iso_now);
    bind_text(s, 2, sha256);
    int rc = sqlite3_step(s);
    sqlite3_reset(s);
    return rc == SQLITE_DONE ? 0 : -1;
}

int track_repo_lookup_by_path(struct nocturne_db *db, const char *path,
                              char **sha256_out,
                              long long *mtime_ns_out,
                              long long *size_bytes_out)
{
    if (!db || !path) return -1;
    if (ensure_cache_for(db) != 0) return -1;
    if (prep(db, &g_cache.lookup_by_path, LOOKUP_BY_PATH_SQL) != 0) return -1;
    sqlite3_stmt *s = g_cache.lookup_by_path;
    bind_text(s, 1, path);
    int rc = sqlite3_step(s);
    int hit = 0;
    if (rc == SQLITE_ROW) {
        hit = 1;
        if (sha256_out) {
            const unsigned char *t = sqlite3_column_text(s, 0);
            *sha256_out = t ? strdup((const char *) t) : NULL;
        }
        if (mtime_ns_out) *mtime_ns_out = sqlite3_column_int64(s, 1);
        if (size_bytes_out) *size_bytes_out = sqlite3_column_int64(s, 2);
    }
    sqlite3_reset(s);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) return -1;
    return hit;
}

int track_repo_lookup_by_sha256(struct nocturne_db *db, const char *sha256)
{
    if (!db || !sha256) return -1;
    if (ensure_cache_for(db) != 0) return -1;
    if (prep(db, &g_cache.lookup_by_sha256, LOOKUP_BY_SHA_SQL) != 0) return -1;
    sqlite3_stmt *s = g_cache.lookup_by_sha256;
    bind_text(s, 1, sha256);
    int rc = sqlite3_step(s);
    int hit = (rc == SQLITE_ROW) ? 1 : 0;
    sqlite3_reset(s);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) return -1;
    return hit;
}

long track_repo_delete_unseen_under_root(struct nocturne_db *db,
                                         const char *library_root,
                                         const char *cutoff_iso)
{
    if (!db || !library_root || !cutoff_iso) return -1;
    if (ensure_cache_for(db) != 0) return -1;
    if (prep(db, &g_cache.delete_unseen, DELETE_UNSEEN_SQL) != 0) return -1;

    /* SQL LIKE pattern: prefix + '%'. We don't escape underscore/percent
     * characters in the path because library_root is a real filesystem
     * path the user supplied; misinterpretation here only widens the
     * delete set, never narrows it dangerously, and the cutoff_iso guard
     * prevents collateral damage on rows seen this scan. */
    size_t pn = strlen(library_root);
    char *pattern = malloc(pn + 3);
    if (!pattern) return -1;
    memcpy(pattern, library_root, pn);
    /* Ensure trailing slash before the wildcard so /home/library doesn't
     * match /home/library-old/foo.mp3. */
    if (pn == 0 || pattern[pn - 1] != '/') {
        pattern[pn++] = '/';
    }
    pattern[pn++] = '%';
    pattern[pn] = '\0';

    sqlite3_stmt *s = g_cache.delete_unseen;
    bind_text(s, 1, pattern);
    bind_text(s, 2, cutoff_iso);
    int rc = sqlite3_step(s);
    long deleted = (rc == SQLITE_DONE) ? sqlite3_changes(db_handle(db)) : -1;
    sqlite3_reset(s);
    free(pattern);
    return deleted;
}

long long track_repo_count(struct nocturne_db *db)
{
    if (!db) return -1;
    if (ensure_cache_for(db) != 0) return -1;
    if (prep(db, &g_cache.count, COUNT_SQL) != 0) return -1;
    sqlite3_stmt *s = g_cache.count;
    long long n = -1;
    if (sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int64(s, 0);
    sqlite3_reset(s);
    return n;
}
