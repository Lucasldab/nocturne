/*
 * db.c — SQLite handle management for nocturned.
 *
 * On db_open():
 *   1. Ensure parent directory exists.
 *   2. Open the DB with READWRITE|CREATE.
 *   3. Set WAL + synchronous=NORMAL + busy_timeout=5000 + foreign_keys=ON.
 *   4. Run migrations.
 *
 * On db_close():
 *   - PRAGMA wal_checkpoint(TRUNCATE) so the WAL doesn't grow without bound
 *     across daemon restarts (Pitfall 6 from research/PITFALLS.md).
 *   - sqlite3_close_v2 releases finalised statements lazily; callers are
 *     expected to finalise their own prepared statements before close.
 */

#define _GNU_SOURCE

#include "db.h"
#include "migrations.h"
#include "paths.h"

#include <errno.h>
#include <libgen.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define DB_MAX_FINALIZE_HOOKS 8

struct db_finalize_slot {
    db_finalize_fn fn;
    void *ud;
};

struct nocturne_db {
    sqlite3 *handle;
    char *path;
    void (*err_cb)(const char *msg, void *ud);
    void *ud;
    struct db_finalize_slot finalize_hooks[DB_MAX_FINALIZE_HOOKS];
    size_t finalize_count;
};

static void emit_err(struct nocturne_db *db, const char *msg)
{
    if (db && db->err_cb && msg) db->err_cb(msg, db->ud);
}

static int exec_pragma(struct nocturne_db *db, const char *pragma)
{
    char *err = NULL;
    if (sqlite3_exec(db->handle, pragma, NULL, NULL, &err) != SQLITE_OK) {
        char buf[512];
        snprintf(buf, sizeof(buf), "pragma '%s' failed: %s",
                 pragma, err ? err : "(unknown)");
        emit_err(db, buf);
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

/* mkdir -p the parent directory of `file_path`. */
static int ensure_parent_dir(const char *file_path)
{
    char buf[4096];
    size_t n = strlen(file_path);
    if (n >= sizeof(buf)) { errno = ENAMETOOLONG; return -1; }
    memcpy(buf, file_path, n + 1);

    char *parent = dirname(buf);
    if (!parent || !*parent || !strcmp(parent, ".") || !strcmp(parent, "/")) return 0;
    return paths_mkdir_p(parent, 0700);
}

struct nocturne_db *db_open(const char *path,
                            void (*err_cb)(const char *msg, void *ud),
                            void *ud)
{
    if (!path) {
        if (err_cb) err_cb("db_open: NULL path", ud);
        return NULL;
    }

    if (ensure_parent_dir(path) != 0) {
        char buf[512];
        snprintf(buf, sizeof(buf), "db_open: cannot create parent of %s: %s",
                 path, strerror(errno));
        if (err_cb) err_cb(buf, ud);
        return NULL;
    }

    struct nocturne_db *db = calloc(1, sizeof(*db));
    if (!db) {
        if (err_cb) err_cb("db_open: out of memory", ud);
        return NULL;
    }
    db->err_cb = err_cb;
    db->ud = ud;
    db->path = strdup(path);
    if (!db->path) {
        free(db);
        if (err_cb) err_cb("db_open: out of memory (path)", ud);
        return NULL;
    }

    int rc = sqlite3_open_v2(path, &db->handle,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                             NULL);
    if (rc != SQLITE_OK) {
        char buf[512];
        snprintf(buf, sizeof(buf), "sqlite3_open_v2(%s) failed: %s",
                 path, sqlite3_errmsg(db->handle));
        emit_err(db, buf);
        if (db->handle) sqlite3_close_v2(db->handle);
        free(db->path);
        free(db);
        return NULL;
    }

    /* Order matters: WAL first so subsequent pragmas write into a
     * journal_mode=wal database. busy_timeout before any other concurrent
     * operation so the migration BEGIN IMMEDIATE waits politely instead of
     * SQLITE_BUSY-ing immediately. */
    if (exec_pragma(db, "PRAGMA journal_mode=WAL;") != 0      ||
        exec_pragma(db, "PRAGMA synchronous=NORMAL;") != 0    ||
        exec_pragma(db, "PRAGMA busy_timeout=5000;") != 0     ||
        exec_pragma(db, "PRAGMA foreign_keys=ON;") != 0) {
        db_close(db);
        return NULL;
    }

    if (migrations_apply(db) != 0) {
        emit_err(db, "migrations_apply failed");
        db_close(db);
        return NULL;
    }

    return db;
}

int db_register_finalize_hook(struct nocturne_db *db, db_finalize_fn fn, void *ud)
{
    if (!db || !fn) return -1;
    if (db->finalize_count >= DB_MAX_FINALIZE_HOOKS) return -1;
    db->finalize_hooks[db->finalize_count].fn = fn;
    db->finalize_hooks[db->finalize_count].ud = ud;
    db->finalize_count++;
    return 0;
}

void db_close(struct nocturne_db *db)
{
    if (!db) return;
    /* Fire finalize hooks before closing the underlying handle so modules
     * can sqlite3_finalize their cached statements. */
    for (size_t i = 0; i < db->finalize_count; i++) {
        if (db->finalize_hooks[i].fn) {
            db->finalize_hooks[i].fn(db->finalize_hooks[i].ud);
        }
    }
    db->finalize_count = 0;
    if (db->handle) {
        /* Best-effort checkpoint to keep WAL bounded. Errors here aren't
         * fatal — we still want to release the handle. */
        sqlite3_exec(db->handle, "PRAGMA wal_checkpoint(TRUNCATE);",
                     NULL, NULL, NULL);
        sqlite3_close_v2(db->handle);
    }
    free(db->path);
    free(db);
}

struct sqlite3 *db_handle(struct nocturne_db *db)
{
    return db ? db->handle : NULL;
}

int db_begin(struct nocturne_db *db)
{
    if (!db || !db->handle) return -1;
    return sqlite3_exec(db->handle, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

int db_commit(struct nocturne_db *db)
{
    if (!db || !db->handle) return -1;
    return sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

int db_rollback(struct nocturne_db *db)
{
    if (!db || !db->handle) return -1;
    return sqlite3_exec(db->handle, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

int db_schema_version(struct nocturne_db *db)
{
    if (!db || !db->handle) return -1;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->handle, "PRAGMA user_version", -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    int v = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) v = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return v;
}
