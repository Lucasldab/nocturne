/*
 * test_db.c — exercise db_open / db_close / migrations + transactions.
 *
 * Behaviours under test (≥ 8 assertions):
 *   1. Fresh open creates DB and parent dir; user_version == 1.
 *   2. journal_mode == "wal".
 *   3. busy_timeout == 5000.
 *   4. foreign_keys == 1.
 *   5. tracks/scan_meta/app_meta tables exist.
 *   6. INSERT into tracks round-trips.
 *   7. db_begin → INSERT → db_rollback leaves table empty.
 *   8. Re-open does NOT replay migration (a marker row inserted between
 *      opens survives, proving CREATE TABLE IF NOT EXISTS didn't drop).
 *   9. Opening with parent dir missing creates it.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sqlite3.h>

#include "db.h"
#include "runner.h"

static int rm_rf(const char *path)
{
    if (!path || strncmp(path, "/tmp/", 5) != 0) return -1;
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf -- %s", path);
    return system(cmd);
}

static char *make_tmpdir(const char *tag)
{
    char tmpl[256];
    snprintf(tmpl, sizeof(tmpl), "/tmp/nocturne-db-%s-XXXXXX", tag);
    char *p = strdup(tmpl);
    if (!p) return NULL;
    if (!mkdtemp(p)) { free(p); return NULL; }
    return p;
}

/* Run a one-shot pragma/select that returns one INTEGER column; -1 on error. */
static long long pragma_int(struct sqlite3 *raw, const char *sql)
{
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    long long v = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) v = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return v;
}

/* Read journal_mode (string). Caller frees. */
static char *pragma_string(struct sqlite3 *raw, const char *sql)
{
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    char *out = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *t = sqlite3_column_text(stmt, 0);
        if (t) out = strdup((const char *) t);
    }
    sqlite3_finalize(stmt);
    return out;
}

/* Returns 1 if a table exists in the main schema. */
static int table_exists(struct sqlite3 *raw, const char *name)
{
    sqlite3_stmt *stmt = NULL;
    int found = 0;
    if (sqlite3_prepare_v2(raw,
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?",
            -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) found = 1;
    sqlite3_finalize(stmt);
    return found;
}

static int insert_track(struct sqlite3 *raw, const char *sha, const char *path)
{
    const char *sql =
        "INSERT INTO tracks (sha256, path, mtime_ns, size_bytes, "
        "tags_status, date_added, last_seen_at) "
        "VALUES (?, ?, 0, 0, 'ok', '2026-04-26T00:00:00Z', '2026-04-26T00:00:00Z')";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, sha, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, path, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

static long long count_tracks(struct sqlite3 *raw)
{
    return pragma_int(raw, "SELECT COUNT(*) FROM tracks");
}

int main(int argc, char **argv)
{
    (void) argc; (void) argv;

    char *tmp = make_tmpdir("open");
    if (!tmp) { fprintf(stderr, "mkdtemp failed: %s\n", strerror(errno)); return 1; }

    /* 1. Fresh open creates DB; user_version == 1. */
    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/sub/dir/nocturne.db", tmp);
    struct nocturne_db *db = db_open(db_path, NULL, NULL);
    expect(db != NULL, "db_open(): fresh DB created with nested parent dirs");
    if (!db) { rm_rf(tmp); free(tmp); return test_finish(__FILE__); }

    /* user_version is bumped each migration; assert >= 1 to be future-proof
     * against new schema migrations landing in later plans. */
    expect(db_schema_version(db) >= 1, "user_version advanced after migration");

    /* Pragma checks. */
    char *jm = pragma_string(db_handle(db), "PRAGMA journal_mode");
    expect(jm && !strcmp(jm, "wal"), "PRAGMA journal_mode == 'wal'");
    free(jm);

    expect(pragma_int(db_handle(db), "PRAGMA busy_timeout") == 5000,
           "PRAGMA busy_timeout == 5000");

    expect(pragma_int(db_handle(db), "PRAGMA foreign_keys") == 1,
           "PRAGMA foreign_keys == 1");

    /* Table existence. */
    expect(table_exists(db_handle(db), "tracks"),    "tracks table exists");
    expect(table_exists(db_handle(db), "scan_meta"), "scan_meta table exists");
    expect(table_exists(db_handle(db), "app_meta"),  "app_meta table exists");

    /* INSERT round-trip. */
    expect(insert_track(db_handle(db), "deadbeef00", "/tmp/x.mp3") == 0,
           "INSERT INTO tracks succeeds with required columns");
    expect(count_tracks(db_handle(db)) == 1, "SELECT COUNT(*) == 1 after insert");

    /* db_begin → INSERT → db_rollback leaves COUNT unchanged. */
    long long before = count_tracks(db_handle(db));
    expect(db_begin(db) == 0, "db_begin returns 0");
    expect(insert_track(db_handle(db), "rollback01", "/tmp/y.mp3") == 0,
           "INSERT during txn succeeds");
    expect(db_rollback(db) == 0, "db_rollback returns 0");
    expect(count_tracks(db_handle(db)) == before,
           "rollback removed the txn-scoped insert");

    /* db_begin → INSERT → db_commit persists. */
    expect(db_begin(db) == 0, "db_begin returns 0 (commit path)");
    expect(insert_track(db_handle(db), "commit0001", "/tmp/z.mp3") == 0,
           "INSERT during txn succeeds (commit path)");
    expect(db_commit(db) == 0, "db_commit returns 0");
    expect(count_tracks(db_handle(db)) == before + 1,
           "commit persisted the row");

    db_close(db);

    /* 2. Re-open. Marker row from previous open must survive — proves
     *    migrations did NOT replay (CREATE-fresh would have dropped data). */
    db = db_open(db_path, NULL, NULL);
    expect(db != NULL, "db_open(): re-open succeeds");
    expect(db_schema_version(db) >= 1, "user_version still set after re-open");
    expect(count_tracks(db_handle(db)) == before + 1,
           "marker rows survived re-open (migration was idempotent)");
    db_close(db);

    rm_rf(tmp);
    free(tmp);
    return test_finish(__FILE__);
}
