#ifndef NOCTURNE_NOCTURNED_DB_H
#define NOCTURNE_NOCTURNED_DB_H

/* Forward declaration so callers don't need <sqlite3.h> just to hold a
 * struct nocturne_db *. Modules that prepare statements use db_handle()
 * to reach the underlying sqlite3*. */
struct sqlite3;
struct nocturne_db;

/* Open the daemon DB. Creates parent dir if missing. Applies pending
 * migrations. Sets WAL mode, busy_timeout=5000, foreign_keys=ON.
 * Returns NULL on error and writes a message via err_cb (may be NULL). */
struct nocturne_db *db_open(const char *path,
                            void (*err_cb)(const char *msg, void *ud),
                            void *ud);

void db_close(struct nocturne_db *db);

/* Direct sqlite3* escape hatch for prepared-statement code in modules. */
struct sqlite3 *db_handle(struct nocturne_db *db);

/* Transactions: BEGIN IMMEDIATE / COMMIT / ROLLBACK. Returns 0 on success. */
int db_begin(struct nocturne_db *db);
int db_commit(struct nocturne_db *db);
int db_rollback(struct nocturne_db *db);

/* Migration version (PRAGMA user_version). */
int db_schema_version(struct nocturne_db *db);

/* Register a callback fired during db_close, BEFORE sqlite3_close_v2 runs.
 * Modules use this to finalise their cached prepared statements so the
 * close sees a clean handle. Up to 8 hooks per db (asserted on overflow);
 * each hook is called once in registration order. Added in plan 02-02
 * as a small extension to the 02-01 contract. */
typedef void (*db_finalize_fn)(void *ud);
int db_register_finalize_hook(struct nocturne_db *db, db_finalize_fn fn, void *ud);

#endif /* NOCTURNE_NOCTURNED_DB_H */
