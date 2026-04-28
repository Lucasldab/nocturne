/*
 * test_resolve_diff.c — Phase 8 Plan 02: golden-output coverage for the
 * `nocturned resolve --dry-run --diff` formatter (`print_resolve_diff`).
 *
 * Behaviours under test (>= 7 named cases, per plan 08-02 acceptance):
 *   1. test_diff_added_only      — current=[A,B], next=[A,B,C] -> ADDED:1
 *   2. test_diff_removed_only    — current=[A,B], next=[A]     -> REMOVED:1
 *   3. test_diff_shifted_only    — A in both, buckets differ   -> SHIFTED:1
 *   4. test_diff_combined        — ADD + REMOVE + SHIFT all in one diff
 *   5. test_diff_empty           — current == next             -> NET +0
 *   6. test_diff_json_shape      — --json single-line object with the keys
 *                                  added/removed/shifted/net_tracks/net_bytes
 *   7. test_diff_first_run       — manifest_current empty      -> ADDED:N
 *
 * Strategy: build a fresh on-disk SQLite DB via db_open (which runs all
 * migrations), seed `manifest_current` rows directly with INSERT, build a
 * synthetic `struct manifest` for the next-state, then call
 * print_resolve_diff into a memory FILE* (open_memstream) and assert
 * against the captured bytes.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <sqlite3.h>

#include "db.h"
#include "diff.h"
#include "resolver.h"
#include "runner.h"

/* ---- Test fixture helpers ---- */

static char *tmp_db_path(void)
{
    char tmpl[] = "/tmp/nocturne-diff-XXXXXX.db";
    int fd = mkstemps(tmpl, 3);
    if (fd < 0) return NULL;
    close(fd);
    return strdup(tmpl);
}

/* exec_sql — run a single statement, return 0 on success. */
static int exec_sql(struct nocturne_db *db, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(db_handle(db), sql, NULL, NULL, &err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK ? 0 : -1;
}

/* seed_track — insert a tracks row so the manifest_current FK constraint
 * (sha256 -> tracks.sha256) is satisfied. tracks NOT NULL columns:
 * sha256, path, mtime_ns, size_bytes, date_added, last_seen_at. */
static void seed_track(struct nocturne_db *db,
                       const char *sha, long long size_bytes)
{
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db_handle(db),
        "INSERT INTO tracks (sha256, path, mtime_ns, size_bytes, "
        "date_added, last_seen_at) VALUES (?, ?, 0, ?, "
        "'2026-04-27T00:00:00Z', '2026-04-27T00:00:00Z')",
        -1, &st, NULL);
    sqlite3_bind_text(st, 1, sha, -1, SQLITE_TRANSIENT);
    char path[256];
    snprintf(path, sizeof(path), "/tmp/diff-fixture/%s.mp3", sha);
    sqlite3_bind_text(st, 2, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, size_bytes);
    int rc = sqlite3_step(st);
    if (rc != SQLITE_DONE && rc != SQLITE_CONSTRAINT) {
        fprintf(stderr, "seed_track(%s) failed: %s\n", sha,
                sqlite3_errmsg(db_handle(db)));
    }
    sqlite3_finalize(st);
}

/* seed_current — insert a manifest_current row. buckets_csv is comma-joined
 * (matches resolve_cmd.c writer). Auto-seeds the parent tracks row so the
 * FK constraint is satisfied. */
static void seed_current(struct nocturne_db *db,
                         const char *sha, const char *buckets_csv,
                         long long size_bytes)
{
    seed_track(db, sha, size_bytes);
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db_handle(db),
        "INSERT INTO manifest_current (sha256, buckets_csv, size_bytes) "
        "VALUES (?, ?, ?)", -1, &st, NULL);
    sqlite3_bind_text(st, 1, sha, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, buckets_csv, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, size_bytes);
    int rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "seed_current(%s) failed: %s\n", sha,
                sqlite3_errmsg(db_handle(db)));
    }
    sqlite3_finalize(st);
}

/* seed_used_before — set the manifest_meta `used_bytes` row so the
 * formatter has a USED line baseline. */
static void seed_used_before(struct nocturne_db *db, long long used)
{
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db_handle(db),
        "INSERT INTO manifest_meta (key, value) VALUES ('used_bytes', ?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
        -1, &st, NULL);
    char buf[64];
    snprintf(buf, sizeof(buf), "%lld", used);
    sqlite3_bind_text(st, 1, buf, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* mk_track — fill a manifest_track with a sha + a NULL-terminated array of
 * bucket names. Bucket strings copied via strdup. Caller must release via
 * free_synth_manifest. */
static void mk_track(struct manifest_track *mt,
                     const char *sha, long long size_bytes,
                     const char *const *buckets)
{
    mt->sha256 = strdup(sha);
    mt->size_bytes = size_bytes;
    size_t n = 0;
    while (buckets[n]) n++;
    mt->buckets = calloc(n, sizeof(*mt->buckets));
    for (size_t i = 0; i < n; i++) mt->buckets[i] = strdup(buckets[i]);
    mt->buckets_n = n;
}

/* free_synth_manifest — free a synthetic struct manifest built by mk_track. */
static void free_synth_manifest(struct manifest *m)
{
    if (!m->resident) return;
    for (size_t i = 0; i < m->resident_n; i++) {
        struct manifest_track *t = &m->resident[i];
        free(t->sha256);
        for (size_t j = 0; j < t->buckets_n; j++) free(t->buckets[j]);
        free(t->buckets);
    }
    free(m->resident);
    m->resident = NULL;
    m->resident_n = 0;
}

/* run_diff — invoke print_resolve_diff capturing stdout into a heap buffer.
 * Returns the formatter's rc; sets *out_buf (caller frees) and *out_len. */
static int run_diff(struct nocturne_db *db, const struct manifest *m,
                    int as_json, char **out_buf, size_t *out_len)
{
    char *buf = NULL;
    size_t len = 0;
    FILE *mem = open_memstream(&buf, &len);
    if (!mem) { *out_buf = NULL; *out_len = 0; return -1; }
    int rc = print_resolve_diff(db, m, as_json, mem);
    fflush(mem);
    fclose(mem);
    *out_buf = buf;
    *out_len = len;
    return rc;
}

/* ---- Tests ---- */

int main(int argc, char **argv)
{
    (void) argc; (void) argv;

    /* 1. test_diff_added_only — current=[A,B] next=[A,B,C] */
    {
        char *dbp = tmp_db_path();
        struct nocturne_db *db = db_open(dbp, NULL, NULL);
        seed_current(db, "aaaaaaaa11111111", "loved", 1048576);
        seed_current(db, "bbbbbbbb22222222", "top_played", 2097152);
        seed_used_before(db, 3145728);

        struct manifest m = {0};
        m.cap_bytes = 12LL * 1024 * 1024 * 1024;
        m.cap_effective_bytes = 8LL * 1024 * 1024 * 1024;
        m.used_bytes = 3145728 + 4194304;
        m.resident = calloc(3, sizeof(*m.resident));
        m.resident_n = 3;
        const char *bA[] = {"loved", NULL};
        const char *bB[] = {"top_played", NULL};
        const char *bC[] = {"recent_adds", NULL};
        mk_track(&m.resident[0], "aaaaaaaa11111111", 1048576, bA);
        mk_track(&m.resident[1], "bbbbbbbb22222222", 2097152, bB);
        mk_track(&m.resident[2], "cccccccc33333333", 4194304, bC);

        char *buf = NULL; size_t len = 0;
        int rc = run_diff(db, &m, 0, &buf, &len);
        expect(rc == 0, "test_diff_added_only: print_resolve_diff returns 0");
        expect(buf && strstr(buf, "ADDED: 1") != NULL,
               "test_diff_added_only: text contains 'ADDED: 1'");
        expect(buf && strstr(buf, "cccccccc3333") != NULL,
               "test_diff_added_only: ADDED line names the new track (sha-12)");
        expect(buf && strstr(buf, "recent_adds") != NULL,
               "test_diff_added_only: ADDED line names the bucket");
        expect(buf && strstr(buf, "REMOVED: 0") != NULL,
               "test_diff_added_only: text contains 'REMOVED: 0'");
        expect(buf && strstr(buf, "SHIFTED BUCKETS: 0") != NULL,
               "test_diff_added_only: text contains 'SHIFTED BUCKETS: 0'");
        expect(buf && strstr(buf, "NET:") != NULL,
               "test_diff_added_only: text contains 'NET:'");
        expect(buf && strstr(buf, "USED:") != NULL,
               "test_diff_added_only: text contains 'USED:'");

        free(buf);
        free_synth_manifest(&m);
        db_close(db); unlink(dbp); free(dbp);
    }

    /* 2. test_diff_removed_only — current=[A,B] next=[A] */
    {
        char *dbp = tmp_db_path();
        struct nocturne_db *db = db_open(dbp, NULL, NULL);
        seed_current(db, "aaaaaaaa11111111", "loved", 1048576);
        seed_current(db, "bbbbbbbb22222222", "top_played", 2097152);
        seed_used_before(db, 3145728);

        struct manifest m = {0};
        m.cap_bytes = 12LL * 1024 * 1024 * 1024;
        m.cap_effective_bytes = 8LL * 1024 * 1024 * 1024;
        m.used_bytes = 1048576;
        m.resident = calloc(1, sizeof(*m.resident));
        m.resident_n = 1;
        const char *bA[] = {"loved", NULL};
        mk_track(&m.resident[0], "aaaaaaaa11111111", 1048576, bA);

        char *buf = NULL; size_t len = 0;
        int rc = run_diff(db, &m, 0, &buf, &len);
        expect(rc == 0, "test_diff_removed_only: print_resolve_diff returns 0");
        expect(buf && strstr(buf, "REMOVED: 1") != NULL,
               "test_diff_removed_only: text contains 'REMOVED: 1'");
        expect(buf && strstr(buf, "bbbbbbbb2222") != NULL,
               "test_diff_removed_only: REMOVED line names the dropped track");
        expect(buf && strstr(buf, "top_played") != NULL,
               "test_diff_removed_only: REMOVED line names the bucket");
        expect(buf && strstr(buf, "ADDED: 0") != NULL,
               "test_diff_removed_only: text contains 'ADDED: 0'");

        free(buf);
        free_synth_manifest(&m);
        db_close(db); unlink(dbp); free(dbp);
    }

    /* 3. test_diff_shifted_only — A bucket changes from loved to loved+top_played */
    {
        char *dbp = tmp_db_path();
        struct nocturne_db *db = db_open(dbp, NULL, NULL);
        seed_current(db, "aaaaaaaa11111111", "loved", 1048576);
        seed_used_before(db, 1048576);

        struct manifest m = {0};
        m.cap_bytes = 12LL * 1024 * 1024 * 1024;
        m.cap_effective_bytes = 8LL * 1024 * 1024 * 1024;
        m.used_bytes = 1048576;
        m.resident = calloc(1, sizeof(*m.resident));
        m.resident_n = 1;
        const char *bA[] = {"loved", "top_played", NULL};
        mk_track(&m.resident[0], "aaaaaaaa11111111", 1048576, bA);

        char *buf = NULL; size_t len = 0;
        int rc = run_diff(db, &m, 0, &buf, &len);
        expect(rc == 0, "test_diff_shifted_only: print_resolve_diff returns 0");
        expect(buf && strstr(buf, "SHIFTED BUCKETS: 1") != NULL,
               "test_diff_shifted_only: text contains 'SHIFTED BUCKETS: 1'");
        expect(buf && strstr(buf, "+top_played") != NULL,
               "test_diff_shifted_only: SHIFTED line shows '+top_played'");
        expect(buf && strstr(buf, "ADDED: 0") != NULL,
               "test_diff_shifted_only: text contains 'ADDED: 0'");
        expect(buf && strstr(buf, "REMOVED: 0") != NULL,
               "test_diff_shifted_only: text contains 'REMOVED: 0'");

        free(buf);
        free_synth_manifest(&m);
        db_close(db); unlink(dbp); free(dbp);
    }

    /* 4. test_diff_combined — A unchanged, B removed, C added, D shifted */
    {
        char *dbp = tmp_db_path();
        struct nocturne_db *db = db_open(dbp, NULL, NULL);
        seed_current(db, "aaaaaaaa11111111", "loved", 1048576);
        seed_current(db, "bbbbbbbb22222222", "top_played", 2097152);
        seed_current(db, "dddddddd44444444", "loved", 1048576);
        seed_used_before(db, 4194304);

        struct manifest m = {0};
        m.cap_bytes = 12LL * 1024 * 1024 * 1024;
        m.cap_effective_bytes = 8LL * 1024 * 1024 * 1024;
        m.used_bytes = 1048576 + 4194304 + 1048576;
        m.resident = calloc(3, sizeof(*m.resident));
        m.resident_n = 3;
        const char *bA[] = {"loved", NULL};
        const char *bC[] = {"recent_adds", NULL};
        const char *bD[] = {"loved", "top_played", NULL};
        mk_track(&m.resident[0], "aaaaaaaa11111111", 1048576, bA);
        mk_track(&m.resident[1], "cccccccc33333333", 4194304, bC);
        mk_track(&m.resident[2], "dddddddd44444444", 1048576, bD);

        char *buf = NULL; size_t len = 0;
        int rc = run_diff(db, &m, 0, &buf, &len);
        expect(rc == 0, "test_diff_combined: print_resolve_diff returns 0");
        expect(buf && strstr(buf, "ADDED: 1") != NULL,
               "test_diff_combined: ADDED:1 (cccccc...)");
        expect(buf && strstr(buf, "REMOVED: 1") != NULL,
               "test_diff_combined: REMOVED:1 (bbbbbb...)");
        expect(buf && strstr(buf, "SHIFTED BUCKETS: 1") != NULL,
               "test_diff_combined: SHIFTED:1 (dddddd...)");
        expect(buf && strstr(buf, "cccccccc3333") != NULL,
               "test_diff_combined: ADDED line names cccccc track");
        expect(buf && strstr(buf, "bbbbbbbb2222") != NULL,
               "test_diff_combined: REMOVED line names bbbbbb track");
        expect(buf && strstr(buf, "dddddddd4444") != NULL,
               "test_diff_combined: SHIFTED line names dddddd track");

        free(buf);
        free_synth_manifest(&m);
        db_close(db); unlink(dbp); free(dbp);
    }

    /* 5. test_diff_empty — current == next */
    {
        char *dbp = tmp_db_path();
        struct nocturne_db *db = db_open(dbp, NULL, NULL);
        seed_current(db, "aaaaaaaa11111111", "loved", 1048576);
        seed_current(db, "bbbbbbbb22222222", "top_played", 2097152);
        seed_used_before(db, 3145728);

        struct manifest m = {0};
        m.cap_bytes = 12LL * 1024 * 1024 * 1024;
        m.cap_effective_bytes = 8LL * 1024 * 1024 * 1024;
        m.used_bytes = 3145728;
        m.resident = calloc(2, sizeof(*m.resident));
        m.resident_n = 2;
        const char *bA[] = {"loved", NULL};
        const char *bB[] = {"top_played", NULL};
        mk_track(&m.resident[0], "aaaaaaaa11111111", 1048576, bA);
        mk_track(&m.resident[1], "bbbbbbbb22222222", 2097152, bB);

        char *buf = NULL; size_t len = 0;
        int rc = run_diff(db, &m, 0, &buf, &len);
        expect(rc == 0, "test_diff_empty: print_resolve_diff returns 0");
        expect(buf && strstr(buf, "ADDED: 0") != NULL,
               "test_diff_empty: ADDED: 0");
        expect(buf && strstr(buf, "REMOVED: 0") != NULL,
               "test_diff_empty: REMOVED: 0");
        expect(buf && strstr(buf, "SHIFTED BUCKETS: 0") != NULL,
               "test_diff_empty: SHIFTED BUCKETS: 0");
        expect(buf && strstr(buf, "NET: +0 tracks") != NULL,
               "test_diff_empty: NET line shows +0 tracks");

        free(buf);
        free_synth_manifest(&m);
        db_close(db); unlink(dbp); free(dbp);
    }

    /* 6. test_diff_json_shape — --json single-line object with required keys */
    {
        char *dbp = tmp_db_path();
        struct nocturne_db *db = db_open(dbp, NULL, NULL);
        seed_current(db, "aaaaaaaa11111111", "loved", 1048576);
        seed_used_before(db, 1048576);

        struct manifest m = {0};
        m.cap_bytes = 12LL * 1024 * 1024 * 1024;
        m.cap_effective_bytes = 8LL * 1024 * 1024 * 1024;
        m.used_bytes = 1048576 + 4194304;
        m.resident = calloc(2, sizeof(*m.resident));
        m.resident_n = 2;
        const char *bA[] = {"loved", NULL};
        const char *bC[] = {"recent_adds", NULL};
        mk_track(&m.resident[0], "aaaaaaaa11111111", 1048576, bA);
        mk_track(&m.resident[1], "cccccccc33333333", 4194304, bC);

        char *buf = NULL; size_t len = 0;
        int rc = run_diff(db, &m, 1 /* as_json */, &buf, &len);
        expect(rc == 0, "test_diff_json_shape: print_resolve_diff returns 0");
        expect(buf && buf[0] == '{',
               "test_diff_json_shape: starts with '{'");
        expect(buf && strstr(buf, "\"added\"") != NULL,
               "test_diff_json_shape: has 'added' key");
        expect(buf && strstr(buf, "\"removed\"") != NULL,
               "test_diff_json_shape: has 'removed' key");
        expect(buf && strstr(buf, "\"shifted\"") != NULL,
               "test_diff_json_shape: has 'shifted' key");
        expect(buf && strstr(buf, "\"net_tracks\"") != NULL,
               "test_diff_json_shape: has 'net_tracks' key");
        expect(buf && strstr(buf, "\"net_bytes\"") != NULL,
               "test_diff_json_shape: has 'net_bytes' key");
        expect(buf && strstr(buf, "\"used_before\"") != NULL,
               "test_diff_json_shape: has 'used_before' key");
        expect(buf && strstr(buf, "\"used_after\"") != NULL,
               "test_diff_json_shape: has 'used_after' key");
        expect(buf && strstr(buf, "\"cap_effective_bytes\"") != NULL,
               "test_diff_json_shape: has 'cap_effective_bytes' key");
        /* single-line: at most one trailing newline. */
        size_t newlines = 0;
        for (size_t i = 0; i < len; i++) if (buf[i] == '\n') newlines++;
        expect(newlines <= 1,
               "test_diff_json_shape: at most one trailing newline (single line)");
        /* Track id present. */
        expect(buf && strstr(buf, "cccccccc33333333") != NULL,
               "test_diff_json_shape: contains the added track's full id");

        free(buf);
        free_synth_manifest(&m);
        db_close(db); unlink(dbp); free(dbp);
    }

    /* 7. test_diff_first_run — manifest_current empty, next has 3 tracks */
    {
        char *dbp = tmp_db_path();
        struct nocturne_db *db = db_open(dbp, NULL, NULL);
        /* No seed_current calls; manifest_current is empty. */

        struct manifest m = {0};
        m.cap_bytes = 12LL * 1024 * 1024 * 1024;
        m.cap_effective_bytes = 8LL * 1024 * 1024 * 1024;
        m.used_bytes = 3 * 1048576;
        m.resident = calloc(3, sizeof(*m.resident));
        m.resident_n = 3;
        const char *bA[] = {"recent_adds", NULL};
        const char *bB[] = {"loved", NULL};
        const char *bC[] = {"top_played", NULL};
        mk_track(&m.resident[0], "aaaaaaaa11111111", 1048576, bA);
        mk_track(&m.resident[1], "bbbbbbbb22222222", 1048576, bB);
        mk_track(&m.resident[2], "cccccccc33333333", 1048576, bC);

        char *buf = NULL; size_t len = 0;
        int rc = run_diff(db, &m, 0, &buf, &len);
        expect(rc == 0, "test_diff_first_run: print_resolve_diff returns 0");
        expect(buf && strstr(buf, "ADDED: 3") != NULL,
               "test_diff_first_run: ADDED: 3 (all three tracks)");
        expect(buf && strstr(buf, "REMOVED: 0") != NULL,
               "test_diff_first_run: REMOVED: 0 (nothing was resident)");
        expect(buf && strstr(buf, "SHIFTED BUCKETS: 0") != NULL,
               "test_diff_first_run: SHIFTED BUCKETS: 0");
        expect(buf && strstr(buf, "USED:") != NULL,
               "test_diff_first_run: USED line present (used_before=0)");

        free(buf);
        free_synth_manifest(&m);
        db_close(db); unlink(dbp); free(dbp);
    }

    return test_finish("tests/test_resolve_diff.c");
}
