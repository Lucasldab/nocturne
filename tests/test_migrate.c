/*
 * test_migrate.c — exercise migrate_run on synthetic libraries.
 *
 * Behaviours under test (≥ 6):
 *   1. Dry-run with N tagged tracks reports planned=N, no FS / DB mutation.
 *   2. --apply moves every track into <root>/archive/<rel>, updates
 *      tracks.path, and creates the directory tree.
 *   3. Second --apply reports planned=0 / moved=0 (idempotent).
 *   4. Track already under <root>/archive/ counts as already_archived.
 *   5. Track outside <root> is skipped (skipped_outside++).
 *   6. EXDEV injection: link returns EXDEV → fallback_copies++ and the
 *      file ends up at the destination.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sqlite3.h>

#include "db.h"
#include "migrate.h"
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
    snprintf(tmpl, sizeof(tmpl), "/tmp/nocturne-migrate-%s-XXXXXX", tag);
    char *p = strdup(tmpl);
    if (!p) return NULL;
    if (!mkdtemp(p)) { free(p); return NULL; }
    return p;
}

static int write_dummy(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (content) fputs(content, f);
    fclose(f);
    return 0;
}

static int mkdir_p(const char *path)
{
    char buf[4096];
    snprintf(buf, sizeof(buf), "mkdir -p %s", path);
    return system(buf);
}

static int exec_sql(struct nocturne_db *db, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(db_handle(db), sql, NULL, NULL, &err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK ? 0 : -1;
}

/* Insert a synthetic track row. Defaults match Phase 2's track_repo
 * insert minimums. */
static void insert_track(struct nocturne_db *db, const char *sha,
                         const char *path)
{
    char sql[2048];
    snprintf(sql, sizeof(sql),
        "INSERT INTO tracks (sha256, path, mtime_ns, size_bytes, "
        "tags_status, date_added, last_seen_at) VALUES "
        "('%s', '%s', 0, 1024, 'ok', '2026-04-26T00:00:00Z', '2026-04-26T00:00:00Z')",
        sha, path);
    exec_sql(db, sql);
}

static int file_exists(const char *p)
{
    struct stat s;
    return (lstat(p, &s) == 0);
}

static char *get_track_path(struct nocturne_db *db, const char *sha)
{
    char sql[256];
    snprintf(sql, sizeof(sql),
        "SELECT path FROM tracks WHERE sha256='%s'", sha);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db_handle(db), sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    char *out = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *t = sqlite3_column_text(stmt, 0);
        if (t) out = strdup((const char *) t);
    }
    sqlite3_finalize(stmt);
    return out;
}

/* EXDEV injection — exposed to tests via migrate_set_link_fn_for_testing.
 * On the first call return -1/EXDEV; subsequent calls fall through to
 * the real link(2) (we don't need to re-enter; copy_and_unlink handles
 * the fallback path entirely). */
static int g_exdev_calls = 0;
static int link_always_exdev(const char *o, const char *n)
{
    (void) o; (void) n;
    g_exdev_calls++;
    errno = EXDEV;
    return -1;
}

int main(int argc, char **argv)
{
    (void) argc; (void) argv;

    /* --- Test 1 + 2 + 3: dry-run, apply, idempotent --- */
    {
        char *tmp = make_tmpdir("basic");
        char lib[512]; snprintf(lib, sizeof(lib), "%s/lib", tmp);
        mkdir_p(lib);
        char a1[512]; snprintf(a1, sizeof(a1), "%s/Artist1/Album1", lib);
        char a2[512]; snprintf(a2, sizeof(a2), "%s/Artist2/Album2", lib);
        mkdir_p(a1); mkdir_p(a2);
        char p1[512], p2[512], p3[512];
        snprintf(p1, sizeof(p1), "%s/01.flac", a1);
        snprintf(p2, sizeof(p2), "%s/02.flac", a1);
        snprintf(p3, sizeof(p3), "%s/01.flac", a2);
        write_dummy(p1, "alpha");
        write_dummy(p2, "beta");
        write_dummy(p3, "gamma");

        char dbp[512]; snprintf(dbp, sizeof(dbp), "%s/db", tmp);
        struct nocturne_db *db = db_open(dbp, NULL, NULL);
        expect(db != NULL, "test1: db_open");
        insert_track(db, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa1", p1);
        insert_track(db, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa2", p2);
        insert_track(db, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa3", p3);

        /* Dry-run */
        struct migrate_stats s = {0};
        int rc = migrate_run(db, lib, false, &s);
        expect(rc == 0, "test1: dry-run rc=0");
        expect(s.planned == 3, "test1: dry-run planned=3");
        expect(s.moved == 0, "test1: dry-run moved=0");
        expect(file_exists(p1), "test1: dry-run leaves source p1");
        expect(file_exists(p2), "test1: dry-run leaves source p2");
        char arch1[1024];
        snprintf(arch1, sizeof(arch1), "%s/archive/Artist1/Album1/01.flac", lib);
        expect(!file_exists(arch1), "test1: dry-run no archive/ file");

        /* Apply */
        memset(&s, 0, sizeof(s));
        rc = migrate_run(db, lib, true, &s);
        expect(rc == 0, "test2: apply rc=0");
        expect(s.planned == 3, "test2: apply planned=3");
        expect(s.moved == 3, "test2: apply moved=3");
        expect(s.errors == 0, "test2: apply errors=0");
        expect(!file_exists(p1), "test2: source p1 gone");
        expect(file_exists(arch1), "test2: archive/Artist1/Album1/01.flac present");
        char arch3[1024];
        snprintf(arch3, sizeof(arch3), "%s/archive/Artist2/Album2/01.flac", lib);
        expect(file_exists(arch3), "test2: archive/Artist2/Album2/01.flac present");

        char *new1 = get_track_path(db,
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa1");
        expect(new1 != NULL && strstr(new1, "/archive/") != NULL,
               "test2: tracks.path updated to archive/");
        free(new1);

        /* Idempotent re-apply */
        memset(&s, 0, sizeof(s));
        rc = migrate_run(db, lib, true, &s);
        expect(rc == 0, "test3: rerun rc=0");
        expect(s.planned == 0, "test3: rerun planned=0");
        expect(s.moved == 0, "test3: rerun moved=0");
        expect(s.already_archived == 3, "test3: rerun already_archived=3");

        db_close(db);
        rm_rf(tmp);
        free(tmp);
    }

    /* --- Test 4: track already at archive/ counts as already_archived --- */
    {
        char *tmp = make_tmpdir("already");
        char lib[512]; snprintf(lib, sizeof(lib), "%s/lib", tmp);
        char arch[1024]; snprintf(arch, sizeof(arch), "%s/archive/x", lib);
        mkdir_p(arch);
        char p[1024]; snprintf(p, sizeof(p), "%s/t.flac", arch);
        write_dummy(p, "x");

        char dbp[512]; snprintf(dbp, sizeof(dbp), "%s/db", tmp);
        struct nocturne_db *db = db_open(dbp, NULL, NULL);
        insert_track(db, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb1", p);

        struct migrate_stats s = {0};
        int rc = migrate_run(db, lib, true, &s);
        expect(rc == 0, "test4: rc=0");
        expect(s.planned == 0, "test4: planned=0");
        expect(s.already_archived == 1, "test4: already_archived=1");

        db_close(db);
        rm_rf(tmp);
        free(tmp);
    }

    /* --- Test 5: track outside library_root is skipped --- */
    {
        char *tmp = make_tmpdir("outside");
        char lib[512]; snprintf(lib, sizeof(lib), "%s/lib", tmp);
        mkdir_p(lib);
        char outside_dir[512]; snprintf(outside_dir, sizeof(outside_dir), "%s/elsewhere", tmp);
        mkdir_p(outside_dir);
        char op[1024]; snprintf(op, sizeof(op), "%s/foreign.flac", outside_dir);
        write_dummy(op, "foreign");

        char dbp[512]; snprintf(dbp, sizeof(dbp), "%s/db", tmp);
        struct nocturne_db *db = db_open(dbp, NULL, NULL);
        insert_track(db, "ccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc1", op);

        struct migrate_stats s = {0};
        int rc = migrate_run(db, lib, true, &s);
        expect(rc == 0, "test5: rc=0");
        expect(s.planned == 0, "test5: planned=0");
        expect(s.skipped_outside == 1, "test5: skipped_outside=1");
        expect(file_exists(op), "test5: outside file untouched");

        db_close(db);
        rm_rf(tmp);
        free(tmp);
    }

    /* --- Test 6: EXDEV injection → copy+unlink fallback --- */
    {
        char *tmp = make_tmpdir("exdev");
        char lib[512]; snprintf(lib, sizeof(lib), "%s/lib", tmp);
        char d[512]; snprintf(d, sizeof(d), "%s/A/B", lib);
        mkdir_p(d);
        char p[1024]; snprintf(p, sizeof(p), "%s/01.flac", d);
        write_dummy(p, "exdev-payload");

        char dbp[512]; snprintf(dbp, sizeof(dbp), "%s/db", tmp);
        struct nocturne_db *db = db_open(dbp, NULL, NULL);
        insert_track(db, "ddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd1", p);

        g_exdev_calls = 0;
        migrate_set_link_fn_for_testing(link_always_exdev);

        struct migrate_stats s = {0};
        int rc = migrate_run(db, lib, true, &s);
        expect(rc == 0, "test6: rc=0");
        expect(s.planned == 1, "test6: planned=1");
        expect(s.moved == 1, "test6: moved=1");
        expect(s.fallback_copies == 1, "test6: fallback_copies=1");
        expect(g_exdev_calls >= 1, "test6: link override invoked");

        char arch[1024];
        snprintf(arch, sizeof(arch), "%s/archive/A/B/01.flac", lib);
        expect(file_exists(arch), "test6: copy landed at archive/");
        expect(!file_exists(p), "test6: source unlinked after copy");

        migrate_set_link_fn_for_testing(NULL);
        db_close(db);
        rm_rf(tmp);
        free(tmp);
    }

    return test_finish("test_migrate");
}
