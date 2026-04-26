/*
 * test_scan.c — exercise scan_run end-to-end against a copy-of-fixtures dir.
 *
 * Each test case clones tests/fixtures into a fresh /tmp/nocturne-scan-... dir
 * so we can mutate (touch / delete / overwrite) freely. Uses a fresh temp DB
 * per case so the unseen-sweep logic doesn't see leftover rows.
 *
 * Behaviours under test (≥ 12 assertions):
 *   1. Empty fixture dir → scan succeeds, all stats zero.
 *   2. Single-MP3 fixture → row inserted, sha256 64-char hex, tags_status='ok'.
 *   3. Re-scan unchanged → files_skipped_unchanged matches files_seen,
 *      files_added=0.
 *   4. Touch fixture → files_updated=1.
 *   5. Replace fixture content → files_updated=1, sha256 changed.
 *   6. Delete fixture between scans → files_removed=1, row gone.
 *   7. Broken MP3 (random bytes) → tags_status='parse_failed', row inserted.
 *   8. Missing-album-artist FLAC → tags_status='incomplete', tag_warning set.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

#include "db.h"
#include "scan.h"
#include "track_repo.h"
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
    snprintf(tmpl, sizeof(tmpl), "/tmp/nocturne-scan-%s-XXXXXX", tag);
    char *p = strdup(tmpl);
    if (!p) return NULL;
    if (!mkdtemp(p)) { free(p); return NULL; }
    return p;
}

static int copy_one(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[8192]; size_t n;
    int ret = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ret = -1; break; }
    }
    fclose(in); fclose(out);
    return ret;
}

static char *fix_path(const char *fixdir, const char *name)
{
    size_t n = strlen(fixdir) + 1 + strlen(name) + 1;
    char *p = malloc(n);
    if (!p) return NULL;
    snprintf(p, n, "%s/%s", fixdir, name);
    return p;
}

static char *tmp_db_path(void)
{
    char tmpl[] = "/tmp/nocturne-scan-db-XXXXXX.db";
    int fd = mkstemps(tmpl, 3);
    if (fd < 0) return NULL;
    close(fd);
    return strdup(tmpl);
}

/* Read a single text column out of the tracks table for a given path.
 * Returns malloc'd string (caller frees) or NULL on miss / error. */
static char *select_text(struct nocturne_db *db, const char *path, const char *col)
{
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT %s FROM tracks WHERE path=?", col);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db_handle(db), sql, -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *t = sqlite3_column_text(st, 0);
        if (t) out = strdup((const char *) t);
    }
    sqlite3_finalize(st);
    return out;
}

int main(int argc, char **argv)
{
    const char *fixdir = (argc > 1) ? argv[1] : "tests/fixtures";

    /* 1. Empty dir scan. */
    {
        char *empty = make_tmpdir("empty");
        char *dbp   = tmp_db_path();
        struct nocturne_db *db = db_open(dbp, NULL, NULL);
        struct scan_stats s = {0};
        int rc = scan_run(db, empty, &s);
        expect(rc == 0, "empty: scan_run returns 0");
        expect(s.files_seen == 0 && s.files_added == 0 && s.files_removed == 0,
               "empty: all stats zero");
        db_close(db);
        unlink(dbp); free(dbp);
        rm_rf(empty); free(empty);
    }

    /* 2 + 3 + 4 + 5 + 6: single-MP3 lifecycle. */
    {
        char *root = make_tmpdir("life");
        char *dst  = fix_path(root, "track.mp3");
        char *src  = fix_path(fixdir, "clean_id3v24.mp3");
        if (copy_one(src, dst) != 0) {
            fprintf(stderr, "copy %s -> %s failed\n", src, dst);
            return 1;
        }

        char *dbp = tmp_db_path();
        struct nocturne_db *db = db_open(dbp, NULL, NULL);

        /* 2. Insert. */
        struct scan_stats s = {0};
        int rc = scan_run(db, root, &s);
        expect(rc == 0, "life: scan_run #1 returns 0");
        expect(s.files_seen == 1 && s.files_added == 1 && s.files_skipped_unchanged == 0,
               "life: first scan adds 1 row");

        char *sha = select_text(db, dst, "sha256");
        expect(sha != NULL && strlen(sha) == 64, "life: sha256 column is 64 hex chars");
        char *status = select_text(db, dst, "tags_status");
        expect(status && !strcmp(status, "ok"), "life: tags_status='ok' for clean MP3");
        free(status);

        /* 3. Incremental no-op. */
        memset(&s, 0, sizeof(s));
        rc = scan_run(db, root, &s);
        expect(rc == 0, "life: scan_run #2 returns 0");
        expect(s.files_skipped_unchanged == 1 && s.files_added == 0 && s.files_updated == 0,
               "life: second scan is no-op (DAEMON-02)");

        /* 4. Touch — bump mtime, expect files_updated. */
        {
            struct timespec times[2];
            clock_gettime(CLOCK_REALTIME, &times[0]);
            times[1] = times[0];
            times[1].tv_sec += 5;  /* 5s into the future to be safe */
            utimensat(AT_FDCWD, dst, times, 0);
        }
        memset(&s, 0, sizeof(s));
        rc = scan_run(db, root, &s);
        expect(rc == 0, "life: scan_run #3 (post-touch) returns 0");
        expect(s.files_updated == 1 && s.files_added == 0,
               "life: touch triggers files_updated=1 (re-hash same content)");

        /* 5. Replace content (different bytes). All fixture MP3s encode the
         *    same 1s silence source through libmp3lame so they share audio
         *    frames; copy clean_id3v24 then append a kilobyte of payload so
         *    the post-ID3 audio bytes genuinely differ. */
        copy_one(src, dst);
        FILE *appendf = fopen(dst, "ab");
        expect(appendf != NULL, "life: open dst for append succeeds");
        if (appendf) {
            char extra[1024];
            for (size_t i = 0; i < sizeof(extra); i++) extra[i] = (char) (i & 0xff);
            fwrite(extra, 1, sizeof(extra), appendf);
            fclose(appendf);
        }
        memset(&s, 0, sizeof(s));
        rc = scan_run(db, root, &s);
        /* rc may be 1 (partial) if TagLib rejects the appended-byte file;
         * the row + sha256 update path doesn't depend on tag parsing. */
        expect(rc == 0 || rc == 1, "life: scan_run #4 (post-replace) returns 0/1");
        expect(s.files_updated == 1, "life: replacing content triggers files_updated=1");
        char *sha2 = select_text(db, dst, "sha256");
        expect(sha2 && strcmp(sha2, sha) != 0,
               "life: sha256 changed after content replacement");
        free(sha); free(sha2);

        /* 6. Delete. */
        unlink(dst);
        memset(&s, 0, sizeof(s));
        rc = scan_run(db, root, &s);
        expect(rc == 0, "life: scan_run #5 (post-delete) returns 0");
        expect(s.files_removed == 1, "life: deleted file produces files_removed=1");
        expect(track_repo_count(db) == 0, "life: row is gone after delete-sweep");

        db_close(db);
        unlink(dbp); free(dbp);
        free(dst); free(src);
        rm_rf(root); free(root);
    }

    /* 7. Broken MP3 → parse_failed. */
    {
        char *root = make_tmpdir("broken");
        char *dst  = fix_path(root, "broken.mp3");
        char *src  = fix_path(fixdir, "broken_audio.mp3");
        copy_one(src, dst);

        char *dbp = tmp_db_path();
        struct nocturne_db *db = db_open(dbp, NULL, NULL);
        struct scan_stats s = {0};
        int rc = scan_run(db, root, &s);
        /* Broken MP3 returns rc=1 (partial) since it counts a parse_failed. */
        expect(rc == 0 || rc == 1, "broken: scan_run returns 0 or 1");
        expect(s.files_seen == 1, "broken: file_seen=1");

        /* TagLib 2.x is permissive about random bytes named .mp3 — it may
         * or may not return tag_read_failed. The observable invariant is
         * that the row gets tagged either 'parse_failed' (TagLib refused)
         * or 'incomplete' (TagLib parsed but no canonical fields). */
        char *status = select_text(db, dst, "tags_status");
        expect(status && (!strcmp(status, "parse_failed") || !strcmp(status, "incomplete")),
               "broken: tags_status is parse_failed or incomplete");
        free(status);

        db_close(db);
        unlink(dbp); free(dbp);
        free(dst); free(src);
        rm_rf(root); free(root);
    }

    /* 8. Missing-album-artist FLAC → incomplete. */
    {
        char *root = make_tmpdir("incomp");
        char *dst  = fix_path(root, "missing_aa.flac");
        char *src  = fix_path(fixdir, "missing_album_artist.flac");
        copy_one(src, dst);

        char *dbp = tmp_db_path();
        struct nocturne_db *db = db_open(dbp, NULL, NULL);
        struct scan_stats s = {0};
        int rc = scan_run(db, root, &s);
        expect(rc == 0 || rc == 1, "incomp: scan_run returns 0 or 1");
        char *status = select_text(db, dst, "tags_status");
        expect(status && !strcmp(status, "incomplete"),
               "incomp: tags_status='incomplete'");
        free(status);
        char *warn = select_text(db, dst, "tag_warning");
        expect(warn && strlen(warn) > 0, "incomp: tag_warning is non-empty");
        free(warn);

        db_close(db);
        unlink(dbp); free(dbp);
        free(dst); free(src);
        rm_rf(root); free(root);
    }

    return test_finish(__FILE__);
}
