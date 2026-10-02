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
#include <errno.h>
#include <unistd.h>

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
    "DELETE FROM tracks WHERE substr(path, 1, length(?1)) = ?1 AND last_seen_at < ?2";

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

/* Resident transcodes are derived files whose only reference is
 * residency_state.transcode_path. Deleting a tracks row cascades that row
 * away, so the transcode has to go with it or it is orphaned: Syncthing
 * keeps shipping it and rotate never manages it again. (2026-09-16: the
 * case-folding subtree sweep deleted rows while their files stayed, and
 * left 22 such orphans on the phone.)
 *
 * collect_transcodes runs `sql` (binding up to two text params) BEFORE
 * the delete; unlink_transcodes runs only after the delete succeeded. If
 * the enclosing transaction later rolls back, the residency row survives
 * with its file missing and rotate's respawn path re-transcodes it. */
struct path_list { char **v; size_t n; };

static void path_list_free(struct path_list *l)
{
    for (size_t i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    l->v = NULL; l->n = 0;
}

static int collect_transcodes(sqlite3 *raw, const char *sql,
                              const char *p1, const char *p2,
                              struct path_list *out)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(raw, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    if (p1) sqlite3_bind_text(st, 1, p1, -1, SQLITE_TRANSIENT);
    if (p2) sqlite3_bind_text(st, 2, p2, -1, SQLITE_TRANSIENT);
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const unsigned char *t = sqlite3_column_text(st, 0);
        if (!t) continue;
        char **nv = realloc(out->v, (out->n + 1) * sizeof(*nv));
        if (!nv) { sqlite3_finalize(st); return -1; }
        out->v = nv;
        out->v[out->n] = strdup((const char *) t);
        if (!out->v[out->n]) { sqlite3_finalize(st); return -1; }
        out->n++;
    }
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

static void unlink_transcodes(const struct path_list *l)
{
    for (size_t i = 0; i < l->n; i++) {
        if (unlink(l->v[i]) == 0) {
            fprintf(stderr, "scan: removed transcode of dropped track %s\n",
                    l->v[i]);
        } else if (errno != ENOENT) {
            fprintf(stderr, "scan: unlink(%s) failed: %s\n",
                    l->v[i], strerror(errno));
        }
    }
}

int track_repo_delete(struct nocturne_db *db, const char *sha256)
{
    if (!db || !sha256) return -1;
    sqlite3 *raw = db_handle(db);
    if (!raw) return -1;
    struct path_list xs = {0};
    if (collect_transcodes(raw,
            "SELECT transcode_path FROM residency_state "
            "WHERE sha256=? AND transcode_path IS NOT NULL",
            sha256, NULL, &xs) != 0) {
        path_list_free(&xs);
        return -1;
    }
    sqlite3_stmt *del = NULL;
    int ok = 0;
    if (sqlite3_prepare_v2(raw, "DELETE FROM tracks WHERE sha256=?",
                           -1, &del, NULL) == SQLITE_OK) {
        sqlite3_bind_text(del, 1, sha256, -1, SQLITE_TRANSIENT);
        ok = (sqlite3_step(del) == SQLITE_DONE);
        sqlite3_finalize(del);
    }
    if (ok) unlink_transcodes(&xs);
    path_list_free(&xs);
    return ok ? 0 : -1;
}

long track_repo_delete_unseen_under_root(struct nocturne_db *db,
                                         const char *library_root,
                                         const char *cutoff_iso)
{
    if (!db || !library_root || !cutoff_iso) return -1;
    if (ensure_cache_for(db) != 0) return -1;
    if (prep(db, &g_cache.delete_unseen, DELETE_UNSEEN_SQL) != 0) return -1;

    /* Exact, case-sensitive prefix match — NOT `LIKE`. LIKE folds ASCII
     * case and treats `_` as a wildcard, so a subtree scan of
     * "archive/Flawed Mangoes/X" swept rows under "archive/flawed mangoes/X"
     * that it never walked. The cutoff_iso guard only protects rows seen
     * by *this* walk, which a sibling dir's rows never are; their deletion
     * then fired the after-delete trigger and dropped their pins/likes. */
    size_t pn = strlen(library_root);
    char *pattern = malloc(pn + 2);
    if (!pattern) return -1;
    memcpy(pattern, library_root, pn);
    /* Ensure trailing slash so /home/library doesn't match
     * /home/library-old/foo.mp3. */
    if (pn == 0 || pattern[pn - 1] != '/') {
        pattern[pn++] = '/';
    }
    pattern[pn] = '\0';

    struct path_list xs = {0};
    if (collect_transcodes(db_handle(db),
            "SELECT r.transcode_path FROM residency_state r "
            "JOIN tracks t ON t.sha256 = r.sha256 "
            "WHERE substr(t.path, 1, length(?1)) = ?1 AND t.last_seen_at < ?2 "
            "AND r.transcode_path IS NOT NULL",
            pattern, cutoff_iso, &xs) != 0) {
        path_list_free(&xs);
        free(pattern);
        return -1;
    }

    sqlite3_stmt *s = g_cache.delete_unseen;
    bind_text(s, 1, pattern);
    bind_text(s, 2, cutoff_iso);
    int rc = sqlite3_step(s);
    long deleted = (rc == SQLITE_DONE) ? sqlite3_changes(db_handle(db)) : -1;
    sqlite3_reset(s);
    free(pattern);
    if (deleted >= 0) unlink_transcodes(&xs);
    path_list_free(&xs);
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
