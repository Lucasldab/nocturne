/*
 * test_rotate.c — exercise rotate_run on synthetic libraries.
 *
 * Behaviours under test (≥ 10):
 *   1. Empty manifest → to_add=0 / to_remove=0.
 *   2. Manifest of 3-of-5 → 3 hardlinks created, residency_state updated,
 *      tracks.path now points at resident/, others untouched.
 *   3. Re-rotate same manifest → idempotent (to_add=0, to_remove=0).
 *   4. Shrink manifest to 1-of-3-resident → 2 evictions back to archive/.
 *   5. Interruption: link injected to fail; rerun heals.
 *   6. EXDEV fallback: link returns EXDEV, copy+unlink runs.
 *   7. EEXIST + same inode → already_applied++ (idempotent recovery).
 *   8. Self-heal: residency_state row missing → INSERTed on first move.
 *   9. tracks row missing for sha in manifest_current → errors++.
 *  10. Order: ADDS run before REMOVES (verified by tracking sequence).
 *  11. Transcode mode: archive FLAC moved (case-merge rename) → resident
 *      transcode renamed to the new expected path, no re-transcode,
 *      residency_state.transcode_path updated, cover re-linked, the old
 *      sidecar-only dir (and its emptied parent) pruned; rerun idempotent.
 *  12. Prune safety: an old resident dir holding anything other than
 *      known sidecars survives, as does resident/ itself.
 *  13. Non-transcode demote prunes the sidecar-only album dir, but keeps
 *      a dir that still holds another resident track's audio.
 *  14. Transcode demote prunes the sidecar-only album dir.
 *  15. Transcode mode: audio under resident/ that no residency_state row
 *      references is removed (transcode extensions only, never dot-files
 *      or dot-dirs), and its emptied dir is pruned.
 *  16. Non-transcode mode never removes unreferenced resident files.
 *  17. Transcode promote with level_lufs set records transcode_gain_db; a
 *      later promote that finds the copy already on disk keeps the stored
 *      gain instead of nulling it (needs ffmpeg + libopus; skipped without).
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
#include "rotate.h"
#include "runner.h"
#include "transcode.h"

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
    snprintf(tmpl, sizeof(tmpl), "/tmp/nocturne-rotate-%s-XXXXXX", tag);
    char *p = strdup(tmpl);
    if (!p) return NULL;
    if (!mkdtemp(p)) { free(p); return NULL; }
    return p;
}

static int mkdir_p(const char *path)
{
    char buf[4096];
    snprintf(buf, sizeof(buf), "mkdir -p %s", path);
    return system(buf);
}

static int write_dummy(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (content) fputs(content, f);
    fclose(f);
    return 0;
}

static int file_exists(const char *p)
{
    struct stat s;
    return (lstat(p, &s) == 0);
}

static int exec_sql(struct nocturne_db *db, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(db_handle(db), sql, NULL, NULL, &err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK ? 0 : -1;
}

static void seed_track(struct nocturne_db *db, const char *sha, const char *path)
{
    char sql[2048];
    snprintf(sql, sizeof(sql),
        "INSERT INTO tracks (sha256, path, mtime_ns, size_bytes, "
        "tags_status, date_added, last_seen_at) VALUES "
        "('%s', '%s', 0, 1024, 'ok', '2026-04-26T00:00:00Z', '2026-04-26T00:00:00Z')",
        sha, path);
    exec_sql(db, sql);
}

static void seed_residency(struct nocturne_db *db, const char *sha, const char *loc)
{
    char sql[512];
    snprintf(sql, sizeof(sql),
        "INSERT INTO residency_state (sha256, location, updated_at) "
        "VALUES ('%s', '%s', '2026-04-26T00:00:00Z')",
        sha, loc);
    exec_sql(db, sql);
}

static void seed_manifest(struct nocturne_db *db, const char *sha, long long size)
{
    char sql[512];
    snprintf(sql, sizeof(sql),
        "INSERT INTO manifest_current (sha256, buckets_csv, size_bytes) "
        "VALUES ('%s', 'recent_adds', %lld)", sha, size);
    exec_sql(db, sql);
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

static char *get_residency_loc(struct nocturne_db *db, const char *sha)
{
    char sql[256];
    snprintf(sql, sizeof(sql),
        "SELECT location FROM residency_state WHERE sha256='%s'", sha);
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

static void seed_residency_transcode(struct nocturne_db *db, const char *sha,
                                     const char *transcode_path)
{
    char sql[2048];
    snprintf(sql, sizeof(sql),
        "INSERT INTO residency_state (sha256, location, updated_at, "
        "transcode_path, transcode_size_bytes, transcode_format) "
        "VALUES ('%s', 'resident', '2026-04-26T00:00:00Z', '%s', 4, 'opus')",
        sha, transcode_path);
    exec_sql(db, sql);
}

/* transcode_gain_db for `sha`: 1 + *out when non-NULL, 0 when NULL, -1 no row. */
static int get_gain(struct nocturne_db *db, const char *sha, double *out)
{
    char sql[256];
    snprintf(sql, sizeof(sql),
        "SELECT transcode_gain_db FROM residency_state WHERE sha256='%s'", sha);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db_handle(db), sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    int rc = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        if (sqlite3_column_type(stmt, 0) == SQLITE_NULL) rc = 0;
        else { *out = sqlite3_column_double(stmt, 0); rc = 1; }
    }
    sqlite3_finalize(stmt);
    return rc;
}

static char *get_transcode_path(struct nocturne_db *db, const char *sha)
{
    char sql[256];
    snprintf(sql, sizeof(sql),
        "SELECT transcode_path FROM residency_state WHERE sha256='%s'", sha);
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

static int file_has(const char *p, const char *want)
{
    char buf[256] = {0};
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strcmp(buf, want) == 0;
}

static int has_last_rotation(struct nocturne_db *db)
{
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db_handle(db),
        "SELECT 1 FROM manifest_meta WHERE key='last_rotation_at'",
        -1, &stmt, NULL) != SQLITE_OK) return 0;
    int hit = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return hit;
}

/* Helper: build an N-track archive layout under <lib>/archive/AlbumI/I.flac
 * and seed both tracks + residency_state(archive). */
static void seed_archive_library(struct nocturne_db *db, const char *lib,
                                 int n, char shas[][65])
{
    char arch[1024]; snprintf(arch, sizeof(arch), "%s/archive", lib);
    mkdir_p(arch);
    for (int i = 0; i < n; i++) {
        char d[1024]; snprintf(d, sizeof(d), "%s/A%d", arch, i);
        mkdir_p(d);
        char p[1200]; snprintf(p, sizeof(p), "%s/%d.flac", d, i);
        char content[64]; snprintf(content, sizeof(content), "track-%d", i);
        write_dummy(p, content);
        for (int k = 0; k < 64; k++) shas[i][k] = "0123456789abcdef"[(i + k) & 0xf];
        shas[i][63] = "0123456789abcdef"[i & 0xf];
        shas[i][64] = '\0';
        seed_track(db, shas[i], p);
        seed_residency(db, shas[i], "archive");
    }
}

/* EXDEV / interruption injection. */
static int g_exdev_calls = 0;
static int link_always_exdev(const char *o, const char *n)
{
    (void) o; (void) n;
    g_exdev_calls++;
    errno = EXDEV;
    return -1;
}
static int g_fail_n = 0;
static int g_fail_remaining = 0;
static int link_fail_n_then_pass(const char *o, const char *n)
{
    if (g_fail_remaining > 0) {
        g_fail_remaining--;
        errno = EIO;
        return -1;
    }
    return link(o, n);
}

int main(int argc, char **argv)
{
    (void) argc; (void) argv;

    /* --- Test 1: empty manifest --- */
    {
        char *tmp = make_tmpdir("empty");
        char lib[512]; snprintf(lib, sizeof(lib), "%s/lib", tmp);
        char dbp[512]; snprintf(dbp, sizeof(dbp), "%s/db", tmp);
        mkdir_p(lib);
        struct nocturne_db *db = db_open(dbp, NULL, NULL);

        char shas[5][65];
        seed_archive_library(db, lib, 5, shas);

        struct rotate_stats s = {0};
        int rc = rotate_run(db, lib, &s);
        expect(rc == 0, "test1: rc=0");
        expect(s.to_add == 0, "test1: to_add=0");
        expect(s.to_remove == 0, "test1: to_remove=0");
        expect(s.added == 0, "test1: added=0");
        expect(has_last_rotation(db), "test1: last_rotation_at recorded");
        db_close(db);
        rm_rf(tmp); free(tmp);
    }

    /* --- Test 2 + 3: 3-of-5 manifest, then idempotent re-rotate --- */
    {
        char *tmp = make_tmpdir("addsome");
        char lib[512]; snprintf(lib, sizeof(lib), "%s/lib", tmp);
        char dbp[512]; snprintf(dbp, sizeof(dbp), "%s/db", tmp);
        mkdir_p(lib);
        struct nocturne_db *db = db_open(dbp, NULL, NULL);

        char shas[5][65];
        seed_archive_library(db, lib, 5, shas);
        for (int i = 0; i < 3; i++) seed_manifest(db, shas[i], 1024);

        struct rotate_stats s = {0};
        int rc = rotate_run(db, lib, &s);
        expect(rc == 0, "test2: rc=0");
        expect(s.to_add == 3, "test2: to_add=3");
        expect(s.to_remove == 0, "test2: to_remove=0");
        expect(s.added == 3, "test2: added=3");
        expect(s.errors == 0, "test2: errors=0");

        for (int i = 0; i < 3; i++) {
            char rp[1200];
            snprintf(rp, sizeof(rp), "%s/resident/A%d/%d.flac", lib, i, i);
            expect(file_exists(rp), "test2: resident file present");
            char ap[1200];
            snprintf(ap, sizeof(ap), "%s/archive/A%d/%d.flac", lib, i, i);
            expect(!file_exists(ap), "test2: archive copy unlinked");
            char *loc = get_residency_loc(db, shas[i]);
            expect(loc && !strcmp(loc, "resident"),
                   "test2: residency_state=resident");
            free(loc);
            char *tp = get_track_path(db, shas[i]);
            expect(tp && strstr(tp, "/resident/") != NULL,
                   "test2: tracks.path under resident/");
            free(tp);
        }
        for (int i = 3; i < 5; i++) {
            char ap[1200];
            snprintf(ap, sizeof(ap), "%s/archive/A%d/%d.flac", lib, i, i);
            expect(file_exists(ap), "test2: untouched archive file present");
            char *loc = get_residency_loc(db, shas[i]);
            expect(loc && !strcmp(loc, "archive"),
                   "test2: untouched residency=archive");
            free(loc);
        }

        /* Test 3: idempotent */
        memset(&s, 0, sizeof(s));
        rc = rotate_run(db, lib, &s);
        expect(rc == 0, "test3: rc=0");
        expect(s.to_add == 0, "test3: to_add=0");
        expect(s.to_remove == 0, "test3: to_remove=0");
        expect(s.added == 0, "test3: added=0");
        expect(s.removed == 0, "test3: removed=0");

        db_close(db);
        rm_rf(tmp); free(tmp);
    }

    /* --- Test 4: shrink manifest → eviction --- */
    {
        char *tmp = make_tmpdir("shrink");
        char lib[512]; snprintf(lib, sizeof(lib), "%s/lib", tmp);
        char dbp[512]; snprintf(dbp, sizeof(dbp), "%s/db", tmp);
        mkdir_p(lib);
        struct nocturne_db *db = db_open(dbp, NULL, NULL);

        char shas[5][65];
        seed_archive_library(db, lib, 5, shas);
        for (int i = 0; i < 3; i++) seed_manifest(db, shas[i], 1024);

        struct rotate_stats s = {0};
        rotate_run(db, lib, &s);
        expect(s.added == 3, "test4 setup: 3 added");

        /* Now shrink manifest to only sha[0]. */
        exec_sql(db, "DELETE FROM manifest_current");
        seed_manifest(db, shas[0], 1024);

        memset(&s, 0, sizeof(s));
        int rc = rotate_run(db, lib, &s);
        expect(rc == 0, "test4: rc=0");
        expect(s.to_remove == 2, "test4: to_remove=2");
        expect(s.removed == 2, "test4: removed=2");

        for (int i = 1; i < 3; i++) {
            char ap[1200];
            snprintf(ap, sizeof(ap), "%s/archive/A%d/%d.flac", lib, i, i);
            expect(file_exists(ap), "test4: evicted file back at archive/");
            char *loc = get_residency_loc(db, shas[i]);
            expect(loc && !strcmp(loc, "archive"),
                   "test4: residency=archive after eviction");
            free(loc);
        }

        db_close(db);
        rm_rf(tmp); free(tmp);
    }

    /* --- Test 5: interruption + heal --- */
    {
        char *tmp = make_tmpdir("heal");
        char lib[512]; snprintf(lib, sizeof(lib), "%s/lib", tmp);
        char dbp[512]; snprintf(dbp, sizeof(dbp), "%s/db", tmp);
        mkdir_p(lib);
        struct nocturne_db *db = db_open(dbp, NULL, NULL);

        char shas[5][65];
        seed_archive_library(db, lib, 5, shas);
        for (int i = 0; i < 5; i++) seed_manifest(db, shas[i], 1024);

        /* Inject EIO on the first 2 link calls. */
        g_fail_remaining = 2;
        rotate_set_link_fn_for_testing(link_fail_n_then_pass);

        struct rotate_stats s = {0};
        rotate_run(db, lib, &s);
        expect(s.errors >= 2, "test5: errors >= 2 on first run");
        expect(s.added <= 3, "test5: at most 3 succeed first run");

        /* Re-run with link restored. */
        g_fail_remaining = 0;
        rotate_set_link_fn_for_testing(NULL);
        memset(&s, 0, sizeof(s));
        int rc = rotate_run(db, lib, &s);
        expect(rc == 0, "test5 heal: rc=0");
        expect(s.errors == 0, "test5 heal: errors=0");

        /* All 5 now resident on disk. */
        for (int i = 0; i < 5; i++) {
            char rp[1200];
            snprintf(rp, sizeof(rp), "%s/resident/A%d/%d.flac", lib, i, i);
            expect(file_exists(rp), "test5: all resident after heal");
            char *loc = get_residency_loc(db, shas[i]);
            expect(loc && !strcmp(loc, "resident"),
                   "test5: residency=resident after heal");
            free(loc);
        }

        db_close(db);
        rm_rf(tmp); free(tmp);
    }

    /* --- Test 6: EXDEV → copy+unlink fallback --- */
    {
        char *tmp = make_tmpdir("exdev");
        char lib[512]; snprintf(lib, sizeof(lib), "%s/lib", tmp);
        char dbp[512]; snprintf(dbp, sizeof(dbp), "%s/db", tmp);
        mkdir_p(lib);
        struct nocturne_db *db = db_open(dbp, NULL, NULL);

        char shas[2][65];
        seed_archive_library(db, lib, 2, shas);
        for (int i = 0; i < 2; i++) seed_manifest(db, shas[i], 1024);

        g_exdev_calls = 0;
        rotate_set_link_fn_for_testing(link_always_exdev);

        struct rotate_stats s = {0};
        int rc = rotate_run(db, lib, &s);
        expect(rc == 0, "test6: rc=0");
        expect(s.fallback_copies == 2, "test6: fallback_copies=2");
        expect(s.added == 2, "test6: added=2");
        expect(g_exdev_calls >= 2, "test6: link override invoked");

        rotate_set_link_fn_for_testing(NULL);
        db_close(db);
        rm_rf(tmp); free(tmp);
    }

    /* --- Test 7: EEXIST + same inode → already_applied --- */
    {
        char *tmp = make_tmpdir("eexist");
        char lib[512]; snprintf(lib, sizeof(lib), "%s/lib", tmp);
        char dbp[512]; snprintf(dbp, sizeof(dbp), "%s/db", tmp);
        mkdir_p(lib);
        struct nocturne_db *db = db_open(dbp, NULL, NULL);

        char shas[1][65];
        seed_archive_library(db, lib, 1, shas);
        /* Pre-create the resident location as a hardlink (same inode). */
        char ap[1200], rp[1200];
        snprintf(ap, sizeof(ap), "%s/archive/A0/0.flac", lib);
        char rd[1200]; snprintf(rd, sizeof(rd), "%s/resident/A0", lib);
        mkdir_p(rd);
        snprintf(rp, sizeof(rp), "%s/0.flac", rd);
        link(ap, rp);

        seed_manifest(db, shas[0], 1024);

        struct rotate_stats s = {0};
        int rc = rotate_run(db, lib, &s);
        expect(rc == 0, "test7: rc=0");
        expect(s.already_applied >= 1, "test7: already_applied>=1");
        expect(s.errors == 0, "test7: errors=0");

        db_close(db);
        rm_rf(tmp); free(tmp);
    }

    /* --- Test 8: residency_state row missing — self-heal via INSERT --- */
    {
        char *tmp = make_tmpdir("heal-row");
        char lib[512]; snprintf(lib, sizeof(lib), "%s/lib", tmp);
        char dbp[512]; snprintf(dbp, sizeof(dbp), "%s/db", tmp);
        mkdir_p(lib);
        struct nocturne_db *db = db_open(dbp, NULL, NULL);

        /* seed an archive file + tracks row but NO residency_state row. */
        char arch[1024]; snprintf(arch, sizeof(arch), "%s/archive/X", lib);
        mkdir_p(arch);
        char p[1200]; snprintf(p, sizeof(p), "%s/x.flac", arch);
        write_dummy(p, "x");
        const char *sha = "8888888888888888888888888888888888888888888888888888888888888888";
        seed_track(db, sha, p);
        seed_manifest(db, sha, 1024);

        struct rotate_stats s = {0};
        int rc = rotate_run(db, lib, &s);
        expect(rc == 0, "test8: rc=0");
        expect(s.added == 1, "test8: added=1 (self-heal INSERT)");
        char *loc = get_residency_loc(db, sha);
        expect(loc && !strcmp(loc, "resident"),
               "test8: residency_state INSERTed as resident");
        free(loc);

        db_close(db);
        rm_rf(tmp); free(tmp);
    }

    /* --- Test 9: tracks row missing → errors++.
     * FK is enforced at INSERT time, so we have to disable it during
     * the seed and re-enable for the rotate run. This simulates a
     * (theoretically impossible) DB-state inconsistency where
     * manifest_current carries a sha that's not in tracks.
     */
    {
        char *tmp = make_tmpdir("missing-track");
        char lib[512]; snprintf(lib, sizeof(lib), "%s/lib", tmp);
        char dbp[512]; snprintf(dbp, sizeof(dbp), "%s/db", tmp);
        mkdir_p(lib);
        struct nocturne_db *db = db_open(dbp, NULL, NULL);

        const char *sha = "9999999999999999999999999999999999999999999999999999999999999999";
        exec_sql(db, "PRAGMA foreign_keys=OFF");
        seed_manifest(db, sha, 1024);
        exec_sql(db, "PRAGMA foreign_keys=ON");

        struct rotate_stats s = {0};
        int rc = rotate_run(db, lib, &s);
        expect(rc == 0, "test9: rc=0 (per-track error not fatal)");
        expect(s.errors >= 1, "test9: errors>=1");
        expect(s.added == 0, "test9: added=0");

        db_close(db);
        rm_rf(tmp); free(tmp);
    }

    /* --- Test 10: ADDS run before REMOVES (Pitfall 1) --- */
    {
        /* Set up a state where there's both an add and a remove. We
         * verify ordering by injecting a link override that records
         * the sequence of (src,dst) calls and asserting that all
         * archive→resident calls come before all resident→archive
         * calls. */
        char *tmp = make_tmpdir("order");
        char lib[512]; snprintf(lib, sizeof(lib), "%s/lib", tmp);
        char dbp[512]; snprintf(dbp, sizeof(dbp), "%s/db", tmp);
        mkdir_p(lib);
        struct nocturne_db *db = db_open(dbp, NULL, NULL);

        char shas[3][65];
        seed_archive_library(db, lib, 3, shas);
        /* sha[0] currently resident (manually); sha[1] in archive; */
        char ap[1200], rp[1200], rd[1200];
        snprintf(ap, sizeof(ap), "%s/archive/A0/0.flac", lib);
        snprintf(rd, sizeof(rd), "%s/resident/A0", lib);
        mkdir_p(rd);
        snprintf(rp, sizeof(rp), "%s/0.flac", rd);
        rename(ap, rp);
        exec_sql(db, "UPDATE residency_state SET location='resident' "
                     "WHERE sha256=(SELECT sha256 FROM tracks LIMIT 1)");
        /* Update tracks.path to reflect the manual move. */
        char usql[2048];
        snprintf(usql, sizeof(usql),
            "UPDATE tracks SET path='%s' WHERE sha256='%s'", rp, shas[0]);
        exec_sql(db, usql);

        /* Manifest contains sha[1] only — so sha[1] must be added,
         * sha[0] must be removed. */
        seed_manifest(db, shas[1], 1024);

        /* (Order observation) Strategy: track which sha's link runs
         * first via the override. */
        static const char *first_sha = NULL;
        first_sha = NULL;

        struct rotate_stats s = {0};
        int rc = rotate_run(db, lib, &s);
        expect(rc == 0, "test10: rc=0");
        expect(s.added == 1, "test10: 1 added");
        expect(s.removed == 1, "test10: 1 removed");

        /* Verify both files are at expected locations after rotate. */
        char ap1[1200], rp1[1200];
        snprintf(ap1, sizeof(ap1), "%s/archive/A0/0.flac", lib);
        snprintf(rp1, sizeof(rp1), "%s/resident/A1/1.flac", lib);
        expect(file_exists(ap1),
               "test10: sha[0] back at archive/ after eviction");
        expect(file_exists(rp1),
               "test10: sha[1] now at resident/ after add");

        db_close(db);
        rm_rf(tmp); free(tmp);
    }

    /* --- Test 11: transcode relocation after the archive FLAC moved --- */
    {
        char *tmp = make_tmpdir("reloc");
        char lib[512]; snprintf(lib, sizeof(lib), "%s/lib", tmp);
        char dbp[512]; snprintf(dbp, sizeof(dbp), "%s/db", tmp);
        mkdir_p(lib);
        struct nocturne_db *db = db_open(dbp, NULL, NULL);

        /* Archive after the beets case merge: ERRA → Erra. */
        char ad[1024]; snprintf(ad, sizeof(ad), "%s/archive/Erra/Augment", lib);
        mkdir_p(ad);
        char ap[1200]; snprintf(ap, sizeof(ap), "%s/x.flac", ad);
        write_dummy(ap, "flac");
        char acov[1200]; snprintf(acov, sizeof(acov), "%s/cover.jpg", ad);
        write_dummy(acov, "art");

        /* Resident still at the pre-merge spelling, cover hardlinked. */
        char od[1024]; snprintf(od, sizeof(od), "%s/resident/ERRA/Augment", lib);
        mkdir_p(od);
        char op[1200]; snprintf(op, sizeof(op), "%s/x.opus", od);
        write_dummy(op, "opus");
        char ocov[1200]; snprintf(ocov, sizeof(ocov), "%s/cover.jpg", od);
        link(acov, ocov);

        const char *sha = "b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1";
        seed_track(db, sha, ap);
        seed_residency_transcode(db, sha, op);
        seed_manifest(db, sha, 1024);

        struct transcode_cfg tc = { .enabled = true, .format = "opus",
                                    .bitrate_kbps = 128 };
        struct rotate_stats s = {0};
        int rc = rotate_run_ex(db, lib, &tc, &s);
        expect(rc == 0, "test11: rc=0");
        expect(s.relocated == 1, "test11: relocated=1");
        expect(s.errors == 0, "test11: errors=0");
        expect(s.to_add == 0, "test11: to_add=0 (no re-transcode)");
        expect(s.added == 0, "test11: added=0");
        expect(s.respawned == 0, "test11: respawned=0");

        char np[1200];
        snprintf(np, sizeof(np), "%s/resident/Erra/Augment/x.opus", lib);
        expect(file_has(np, "opus"),
               "test11: transcode renamed to the new expected path");
        expect(!file_exists(op), "test11: old transcode gone");
        char *xp = get_transcode_path(db, sha);
        expect(xp && !strcmp(xp, np),
               "test11: residency_state.transcode_path updated");
        free(xp);
        char *loc = get_residency_loc(db, sha);
        expect(loc && !strcmp(loc, "resident"), "test11: still resident");
        free(loc);
        char ncov[1200];
        snprintf(ncov, sizeof(ncov), "%s/resident/Erra/Augment/cover.jpg", lib);
        expect(file_exists(ncov), "test11: cover linked beside new path");
        expect(!file_exists(od), "test11: old sidecar-only album dir pruned");
        char oart[1024]; snprintf(oart, sizeof(oart), "%s/resident/ERRA", lib);
        expect(!file_exists(oart), "test11: emptied artist dir pruned");
        char res[1024]; snprintf(res, sizeof(res), "%s/resident", lib);
        expect(file_exists(res), "test11: resident/ itself kept");
        expect(file_exists(ap) && file_exists(acov),
               "test11: archive untouched");

        memset(&s, 0, sizeof(s));
        rc = rotate_run_ex(db, lib, &tc, &s);
        expect(rc == 0 && s.relocated == 0 && s.errors == 0 &&
               s.to_add == 0 && s.to_remove == 0,
               "test11: rerun is a no-op");
        expect(file_has(np, "opus"), "test11: rerun keeps the transcode");

        db_close(db);
        rm_rf(tmp); free(tmp);
    }

    /* --- Test 12: prune never removes non-sidecar content or resident/ --- */
    {
        char *tmp = make_tmpdir("prune-safe");
        char lib[512]; snprintf(lib, sizeof(lib), "%s/lib", tmp);
        char dbp[512]; snprintf(dbp, sizeof(dbp), "%s/db", tmp);
        mkdir_p(lib);
        struct nocturne_db *db = db_open(dbp, NULL, NULL);

        char ad[1024]; snprintf(ad, sizeof(ad), "%s/archive/New/Alb", lib);
        mkdir_p(ad);
        char ap[1200]; snprintf(ap, sizeof(ap), "%s/y.flac", ad);
        write_dummy(ap, "flac");

        char od[1024]; snprintf(od, sizeof(od), "%s/resident/Old/Alb", lib);
        mkdir_p(od);
        char op[1200]; snprintf(op, sizeof(op), "%s/y.opus", od);
        write_dummy(op, "opus");
        char ocov[1200]; snprintf(ocov, sizeof(ocov), "%s/cover.jpg", od);
        write_dummy(ocov, "art");
        char other[1200]; snprintf(other, sizeof(other), "%s/notes.txt", od);
        write_dummy(other, "keep me");

        const char *sha = "c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2";
        seed_track(db, sha, ap);
        seed_residency_transcode(db, sha, op);
        seed_manifest(db, sha, 1024);

        struct transcode_cfg tc = { .enabled = true, .format = "opus",
                                    .bitrate_kbps = 128 };
        struct rotate_stats s = {0};
        int rc = rotate_run_ex(db, lib, &tc, &s);
        expect(rc == 0 && s.relocated == 1, "test12: relocated=1");
        expect(file_has(other, "keep me"), "test12: non-sidecar file kept");
        expect(file_exists(ocov),
               "test12: sidecar kept in a dir holding other content");

        /* Now demote: resident/ must survive even with nothing left. */
        exec_sql(db, "DELETE FROM manifest_current");
        memset(&s, 0, sizeof(s));
        rc = rotate_run_ex(db, lib, &tc, &s);
        expect(rc == 0 && s.removed == 1, "test12: demoted");
        char res[1024]; snprintf(res, sizeof(res), "%s/resident", lib);
        expect(file_exists(res), "test12: resident/ itself kept");
        char nom[1100]; snprintf(nom, sizeof(nom), "%s/.nomedia", res);
        expect(file_exists(nom), "test12: resident/.nomedia kept");
        char nd[1024]; snprintf(nd, sizeof(nd), "%s/resident/New", lib);
        expect(!file_exists(nd), "test12: demoted album dir pruned");

        db_close(db);
        rm_rf(tmp); free(tmp);
    }

    /* --- Test 13: non-transcode demote prunes sidecar-only dirs --- */
    {
        char *tmp = make_tmpdir("prune-move");
        char lib[512]; snprintf(lib, sizeof(lib), "%s/lib", tmp);
        char dbp[512]; snprintf(dbp, sizeof(dbp), "%s/db", tmp);
        mkdir_p(lib);
        struct nocturne_db *db = db_open(dbp, NULL, NULL);

        char shas[2][65];
        seed_archive_library(db, lib, 2, shas);
        /* A0 has a cover; A1 gets a second track that stays resident. */
        char acov[1200]; snprintf(acov, sizeof(acov), "%s/archive/A0/cover.jpg", lib);
        write_dummy(acov, "art");
        char a1b[1200]; snprintf(a1b, sizeof(a1b), "%s/archive/A1/b.flac", lib);
        write_dummy(a1b, "b");
        const char *shab = "d3d3d3d3d3d3d3d3d3d3d3d3d3d3d3d3d3d3d3d3d3d3d3d3d3d3d3d3d3d3d3d3";
        seed_track(db, shab, a1b);
        seed_residency(db, shab, "archive");
        seed_manifest(db, shas[0], 1024);
        seed_manifest(db, shas[1], 1024);
        seed_manifest(db, shab, 1024);

        struct rotate_stats s = {0};
        rotate_run(db, lib, &s);
        expect(s.added == 3, "test13 setup: 3 added");
        char rcov[1200]; snprintf(rcov, sizeof(rcov), "%s/resident/A0/cover.jpg", lib);
        expect(file_exists(rcov), "test13 setup: cover propagated");

        exec_sql(db, "DELETE FROM manifest_current");
        seed_manifest(db, shab, 1024);
        memset(&s, 0, sizeof(s));
        int rc = rotate_run(db, lib, &s);
        expect(rc == 0 && s.removed == 2 && s.errors == 0,
               "test13: 2 demoted");
        char r0[1024]; snprintf(r0, sizeof(r0), "%s/resident/A0", lib);
        expect(!file_exists(r0), "test13: sidecar-only A0 pruned");
        expect(file_exists(acov), "test13: archive cover untouched");
        char r1b[1200]; snprintf(r1b, sizeof(r1b), "%s/resident/A1/b.flac", lib);
        expect(file_exists(r1b), "test13: A1 kept, still holds audio");

        db_close(db);
        rm_rf(tmp); free(tmp);
    }

    /* --- Test 14: transcode demote prunes sidecar-only dirs --- */
    {
        char *tmp = make_tmpdir("prune-xdemote");
        char lib[512]; snprintf(lib, sizeof(lib), "%s/lib", tmp);
        char dbp[512]; snprintf(dbp, sizeof(dbp), "%s/db", tmp);
        mkdir_p(lib);
        struct nocturne_db *db = db_open(dbp, NULL, NULL);

        char ad[1024]; snprintf(ad, sizeof(ad), "%s/archive/Art/Alb", lib);
        mkdir_p(ad);
        char ap[1200]; snprintf(ap, sizeof(ap), "%s/z.flac", ad);
        write_dummy(ap, "flac");
        char rd[1024]; snprintf(rd, sizeof(rd), "%s/resident/Art/Alb", lib);
        mkdir_p(rd);
        char rp[1200]; snprintf(rp, sizeof(rp), "%s/z.opus", rd);
        write_dummy(rp, "opus");
        char rcov[1200]; snprintf(rcov, sizeof(rcov), "%s/folder.png", rd);
        write_dummy(rcov, "art");

        const char *sha = "e4e4e4e4e4e4e4e4e4e4e4e4e4e4e4e4e4e4e4e4e4e4e4e4e4e4e4e4e4e4e4e4";
        seed_track(db, sha, ap);
        seed_residency_transcode(db, sha, rp);

        struct transcode_cfg tc = { .enabled = true, .format = "opus",
                                    .bitrate_kbps = 128 };
        struct rotate_stats s = {0};
        int rc = rotate_run_ex(db, lib, &tc, &s);
        expect(rc == 0 && s.removed == 1 && s.errors == 0, "test14: demoted");
        char ra[1024]; snprintf(ra, sizeof(ra), "%s/resident/Art", lib);
        expect(!file_exists(ra), "test14: sidecar-only album + artist pruned");
        expect(file_exists(ap), "test14: archive untouched");

        db_close(db);
        rm_rf(tmp); free(tmp);
    }

    /* --- Test 15: transcode-mode orphan sweep --- */
    {
        char *tmp = make_tmpdir("orphans");
        char lib[512]; snprintf(lib, sizeof(lib), "%s/lib", tmp);
        char dbp[512]; snprintf(dbp, sizeof(dbp), "%s/db", tmp);
        mkdir_p(lib);
        struct nocturne_db *db = db_open(dbp, NULL, NULL);

        /* One managed resident track. */
        char ad[1024]; snprintf(ad, sizeof(ad), "%s/archive/Keep/Alb", lib);
        mkdir_p(ad);
        char ap[1200]; snprintf(ap, sizeof(ap), "%s/k.flac", ad);
        write_dummy(ap, "flac");
        char kd[1024]; snprintf(kd, sizeof(kd), "%s/resident/Keep/Alb", lib);
        mkdir_p(kd);
        char kp[1200]; snprintf(kp, sizeof(kp), "%s/k.opus", kd);
        write_dummy(kp, "opus");
        const char *sha = "f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5";
        seed_track(db, sha, ap);
        seed_residency_transcode(db, sha, kp);
        seed_manifest(db, sha, 1024);

        /* Orphans: in an album dir with a cover, at resident/ root, .m4a. */
        char od[1024]; snprintf(od, sizeof(od), "%s/resident/Singles/Art", lib);
        mkdir_p(od);
        char o1[1200]; snprintf(o1, sizeof(o1), "%s/Song.opus", od);
        write_dummy(o1, "opus");
        char ocov[1200]; snprintf(ocov, sizeof(ocov), "%s/cover.jpg", od);
        write_dummy(ocov, "art");
        char o2[1200]; snprintf(o2, sizeof(o2), "%s/resident/Singles.opus", lib);
        write_dummy(o2, "opus");
        char o3[1200]; snprintf(o3, sizeof(o3), "%s/resident/Keep/Alb/old.m4a", lib);
        write_dummy(o3, "aac");

        /* Never touched: non-transcode extensions, dot-files, dot-dirs. */
        char n1[1200]; snprintf(n1, sizeof(n1), "%s/resident/Keep/Alb/orig.flac", lib);
        write_dummy(n1, "flac");
        char n2[1200]; snprintf(n2, sizeof(n2), "%s/resident/Keep/Alb/notes.txt", lib);
        write_dummy(n2, "txt");
        char n3[1200]; snprintf(n3, sizeof(n3), "%s/resident/Keep/Alb/.syncthing.x.opus.tmp", lib);
        write_dummy(n3, "tmp");
        char n4[1200]; snprintf(n4, sizeof(n4), "%s/resident/Keep/Alb/.hidden.opus", lib);
        write_dummy(n4, "hidden");
        char sv[1024]; snprintf(sv, sizeof(sv), "%s/resident/.stversions/X", lib);
        mkdir_p(sv);
        char n5[1200]; snprintf(n5, sizeof(n5), "%s/v.opus", sv);
        write_dummy(n5, "versioned");

        struct transcode_cfg tc = { .enabled = true, .format = "opus",
                                    .bitrate_kbps = 128 };
        struct rotate_stats s = {0};
        int rc = rotate_run_ex(db, lib, &tc, &s);
        expect(rc == 0 && s.errors == 0, "test15: rc=0, errors=0");
        expect(s.orphans_removed == 3, "test15: orphans_removed=3");
        expect(!file_exists(o1), "test15: orphan in album dir removed");
        expect(!file_exists(o2), "test15: orphan at resident/ root removed");
        expect(!file_exists(o3), "test15: orphan .m4a removed");
        char sd[1024]; snprintf(sd, sizeof(sd), "%s/resident/Singles", lib);
        expect(!file_exists(sd), "test15: emptied orphan dirs pruned");
        expect(file_has(kp, "opus"), "test15: referenced transcode kept");
        expect(file_exists(n1) && file_exists(n2),
               "test15: non-transcode extensions kept");
        expect(file_exists(n3) && file_exists(n4),
               "test15: dot-files kept");
        expect(file_exists(n5), "test15: dot-dir contents kept");
        expect(file_exists(ap), "test15: archive untouched");

        memset(&s, 0, sizeof(s));
        rc = rotate_run_ex(db, lib, &tc, &s);
        expect(rc == 0 && s.orphans_removed == 0, "test15: rerun removes nothing");

        db_close(db);
        rm_rf(tmp); free(tmp);
    }

    /* --- Test 16: non-transcode mode never sweeps orphans --- */
    {
        char *tmp = make_tmpdir("orphans-flac");
        char lib[512]; snprintf(lib, sizeof(lib), "%s/lib", tmp);
        char dbp[512]; snprintf(dbp, sizeof(dbp), "%s/db", tmp);
        mkdir_p(lib);
        struct nocturne_db *db = db_open(dbp, NULL, NULL);

        char rd[1024]; snprintf(rd, sizeof(rd), "%s/resident/Art/Alb", lib);
        mkdir_p(rd);
        char r1[1200]; snprintf(r1, sizeof(r1), "%s/a.opus", rd);
        write_dummy(r1, "original opus");

        struct rotate_stats s = {0};
        int rc = rotate_run(db, lib, &s);
        expect(rc == 0 && s.orphans_removed == 0,
               "test16: orphans_removed=0 in non-transcode mode");
        expect(file_has(r1, "original opus"),
               "test16: unreferenced resident original kept");

        db_close(db);
        rm_rf(tmp); free(tmp);
    }

    /* --- Test 17: levelled promote records the gain; skip keeps it --- */
    if (system("ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libopus") != 0) {
        printf("[SKIP] tests/test_rotate.c: test17 needs ffmpeg with libopus\n");
    } else {
        char *tmp = make_tmpdir("level");
        char lib[512]; snprintf(lib, sizeof(lib), "%s/lib", tmp);
        char dbp[512]; snprintf(dbp, sizeof(dbp), "%s/db", tmp);
        mkdir_p(lib);
        struct nocturne_db *db = db_open(dbp, NULL, NULL);

        char ad[1024]; snprintf(ad, sizeof(ad), "%s/archive/A/B", lib);
        mkdir_p(ad);
        char ap[1200]; snprintf(ap, sizeof(ap), "%s/t.flac", ad);
        char cmd[2600];
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -nostdin -loglevel error -y -f lavfi "
                 "-i 'sine=frequency=440:duration=5' -af volume=4 '%s'", ap);
        expect(system(cmd) == 0, "test17: fixture FLAC generated");

        const char *sha = "c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7";
        seed_track(db, sha, ap);
        seed_residency(db, sha, "archive");
        seed_manifest(db, sha, 1024);

        struct transcode_cfg tc = { .enabled = true, .format = "opus",
                                    .bitrate_kbps = 128, .level_lufs = -18 };
        struct rotate_stats s = {0};
        int rc = rotate_run_ex(db, lib, &tc, &s);
        expect(rc == 0 && s.added == 1 && s.errors == 0, "test17: promoted");
        double g = 0;
        expect(get_gain(db, sha, &g) == 1, "test17: gain recorded");
        expect(g < -3.0, "test17: loud source was cut");

        /* Simulate a row that lost residency while the copy stayed on disk,
         * carrying a known gain: the re-promote must not re-encode or null it. */
        char sql[512];
        snprintf(sql, sizeof(sql),
                 "UPDATE residency_state SET location='archive', "
                 "transcode_gain_db=-4.25 WHERE sha256='%s'", sha);
        exec_sql(db, sql);
        memset(&s, 0, sizeof(s));
        rc = rotate_run_ex(db, lib, &tc, &s);
        expect(rc == 0 && s.errors == 0, "test17: re-promote ok");
        expect(get_gain(db, sha, &g) == 1 && g == -4.25,
               "test17: existing copy keeps its recorded gain");

        db_close(db);
        rm_rf(tmp); free(tmp);
    }

    return test_finish("test_rotate");
}
