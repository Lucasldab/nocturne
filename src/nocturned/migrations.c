/*
 * migrations.c — schema migration runner.
 *
 * Migration SQL is xxd-embedded at compile time; the source SQL files live
 * under schema/ and are baked into _schema_NNNN.h headers by the Makefile
 * (cd schema && xxd -i NNNN_*.sql > ../src/nocturned/_schema_NNNN.h). The
 * generated header defines two symbols per file:
 *   <stem>_sql      — unsigned char[] (NOT null-terminated by xxd)
 *   <stem>_sql_len  — unsigned int
 *
 * Apply policy: walk MIGRATIONS in order; for each entry whose .version is
 * > current PRAGMA user_version, exec the SQL inside a transaction. The SQL
 * itself sets `PRAGMA user_version = N` at its tail (see the schema dir),
 * so after exec we re-read user_version and verify it advanced.
 */

#define _GNU_SOURCE

#include "migrations.h"
#include "db.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "_schema_0001_init.h"
#include "_schema_0002_resolver.h"
#include "_schema_0003_residency.h"

/* xxd emits `unsigned int <name>_len = N;` (non-const) so we can't use the
 * length in a static initializer. Indirect through a pointer to the length
 * symbol — that pointer IS a compile-time constant. */
struct migration_entry {
    int version;
    const unsigned char *sql_blob;
    const unsigned int *sql_len;
};

/* xxd prepends `__` when the source filename starts with a digit, so the
 * generated symbols for schema/0001_init.sql are __0001_init_sql{,_len}. */
static const struct migration_entry MIGRATIONS[] = {
    { 1, __0001_init_sql,      &__0001_init_sql_len },
    { 2, __0002_resolver_sql,  &__0002_resolver_sql_len },
    { 3, __0003_residency_sql, &__0003_residency_sql_len },
};

static const size_t MIGRATIONS_COUNT = sizeof(MIGRATIONS) / sizeof(MIGRATIONS[0]);

int migrations_target_version(void)
{
    int max = 0;
    for (size_t i = 0; i < MIGRATIONS_COUNT; i++) {
        if (MIGRATIONS[i].version > max) max = MIGRATIONS[i].version;
    }
    return max;
}

/* Read PRAGMA user_version. Returns -1 on error. */
static int read_user_version(struct sqlite3 *raw)
{
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(raw, "PRAGMA user_version", -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    int v = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        v = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return v;
}

int migrations_apply(struct nocturne_db *db)
{
    if (!db) return -1;
    struct sqlite3 *raw = db_handle(db);
    if (!raw) return -1;

    int current = read_user_version(raw);
    if (current < 0) return -1;

    for (size_t i = 0; i < MIGRATIONS_COUNT; i++) {
        const struct migration_entry *m = &MIGRATIONS[i];
        if (m->version <= current) continue;

        /* Heap-copy because xxd-i emits a non-null-terminated array but
         * sqlite3_exec wants a C string. */
        char *sql = (char *) malloc((size_t) (*m->sql_len) + 1);
        if (!sql) return -1;
        memcpy(sql, m->sql_blob, (*m->sql_len));
        sql[(*m->sql_len)] = '\0';

        char *err = NULL;
        if (sqlite3_exec(raw, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
            sqlite3_free(err);
            free(sql);
            return -1;
        }

        if (sqlite3_exec(raw, sql, NULL, NULL, &err) != SQLITE_OK) {
            fprintf(stderr, "migration %d failed: %s\n", m->version,
                    err ? err : "(no message)");
            sqlite3_free(err);
            sqlite3_exec(raw, "ROLLBACK", NULL, NULL, NULL);
            free(sql);
            return -1;
        }

        if (sqlite3_exec(raw, "COMMIT", NULL, NULL, &err) != SQLITE_OK) {
            sqlite3_free(err);
            free(sql);
            return -1;
        }
        free(sql);

        int new_version = read_user_version(raw);
        if (new_version != m->version) {
            fprintf(stderr,
                    "migration %d ran but user_version=%d (expected %d); "
                    "schema/*.sql must end with PRAGMA user_version = N\n",
                    m->version, new_version, m->version);
            return -1;
        }
        current = new_version;
    }

    return 0;
}
