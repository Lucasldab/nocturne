/*
 * test_publisher.c — atomic_io + publish_run.
 *
 * Behaviours under test (≥ 10 assertions):
 *   1. atomic_write_file produces target file with exact byte content.
 *   2. atomic_write_file leaves no .tmp.<pid> artifact under happy path.
 *   3. atomic_write_file: if target dir is missing, creates it.
 *   4. atomic_write_file: invalid args return -1 / EINVAL.
 *   5. atomic_writer streamed write: commit produces the same content
 *      as a one-shot write of the same bytes.
 *   6. atomic_writer abort leaves no target file.
 *   7. atomic_write_file overwrites an existing file atomically.
 *   8. publish_run produces both catalog.json and manifest.json (when
 *      manifest_current is populated).
 *   9. catalog.json carries v=1 and tracks array.
 *  10. manifest.json carries v=1 and resident array.
 *  11. publish_run with empty manifest_current returns the documented
 *      error path (rc != 0).
 *  12. publish_run leaves no .tmp leaks in out_dir.
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
#include <dirent.h>

#include <jansson.h>
#include <sqlite3.h>

#include "atomic_io.h"
#include "db.h"
#include "publish.h"
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
    snprintf(tmpl, sizeof(tmpl), "/tmp/nocturne-pub-%s-XXXXXX", tag);
    char *p = strdup(tmpl);
    if (!p) return NULL;
    if (!mkdtemp(p)) { free(p); return NULL; }
    return p;
}

static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(n + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, n, f);
    buf[n] = '\0';
    if (out_len) *out_len = (size_t) n;
    fclose(f);
    return buf;
}

/* Returns 1 if any entry under `dir` matches "*.tmp.*". */
static int has_tmp_artifact(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *ent;
    int found = 0;
    while ((ent = readdir(d)) != NULL) {
        if (strstr(ent->d_name, ".tmp.")) { found = 1; break; }
    }
    closedir(d);
    return found;
}

int main(int argc, char **argv)
{
    (void) argc; (void) argv;

    /* 1, 2. atomic_write_file happy path. */
    {
        char *dir = make_tmpdir("aw1");
        char path[1024];
        snprintf(path, sizeof(path), "%s/foo.json", dir);
        int rc = atomic_write_file(path, "{\"k\":1}", 7);
        expect(rc == 0, "atomic_write_file: returns 0");
        size_t len = 0;
        char *c = read_file(path, &len);
        expect(c != NULL && len == 7 && !memcmp(c, "{\"k\":1}", 7),
               "atomic_write_file: content matches input bytes");
        free(c);
        expect(!has_tmp_artifact(dir),
               "atomic_write_file: no .tmp.<pid> artifact left behind");
        rm_rf(dir); free(dir);
    }

    /* 3. Creates parent dir. */
    {
        char *dir = make_tmpdir("aw3");
        char path[1024];
        snprintf(path, sizeof(path), "%s/sub/dir/foo.json", dir);
        int rc = atomic_write_file(path, "x", 1);
        expect(rc == 0, "atomic_write_file: creates missing parent dirs");
        struct stat st;
        expect(stat(path, &st) == 0, "atomic_write_file: target file exists");
        rm_rf(dir); free(dir);
    }

    /* 4. EINVAL on NULL. */
    {
        errno = 0;
        int rc = atomic_write_file(NULL, "x", 1);
        expect(rc == -1 && errno == EINVAL,
               "atomic_write_file: NULL path returns -1/EINVAL");
    }

    /* 5. Streamed write. */
    {
        char *dir = make_tmpdir("aw5");
        char path[1024];
        snprintf(path, sizeof(path), "%s/streamed.json", dir);
        struct atomic_writer *w = atomic_writer_open(path);
        expect(w != NULL, "atomic_writer_open returns non-NULL");
        FILE *f = atomic_writer_file(w);
        fputs("hello", f);
        int rc = atomic_writer_commit(w);
        expect(rc == 0, "atomic_writer_commit returns 0");
        size_t len = 0;
        char *c = read_file(path, &len);
        expect(c && len == 5 && !memcmp(c, "hello", 5),
               "streamed atomic write produces correct content");
        free(c);
        expect(!has_tmp_artifact(dir),
               "atomic_writer commit: no tmp artifact");
        rm_rf(dir); free(dir);
    }

    /* 6. Abort. */
    {
        char *dir = make_tmpdir("aw6");
        char path[1024];
        snprintf(path, sizeof(path), "%s/aborted.json", dir);
        struct atomic_writer *w = atomic_writer_open(path);
        fputs("bye", atomic_writer_file(w));
        atomic_writer_abort(w);
        struct stat st;
        expect(stat(path, &st) != 0,
               "atomic_writer_abort: no target file produced");
        expect(!has_tmp_artifact(dir),
               "atomic_writer_abort: cleans up tmp artifact");
        rm_rf(dir); free(dir);
    }

    /* 7. Overwrite existing file. */
    {
        char *dir = make_tmpdir("aw7");
        char path[1024];
        snprintf(path, sizeof(path), "%s/data.json", dir);
        atomic_write_file(path, "old", 3);
        atomic_write_file(path, "newer-data", 10);
        size_t len = 0;
        char *c = read_file(path, &len);
        expect(c && len == 10 && !memcmp(c, "newer-data", 10),
               "atomic_write_file: overwrite atomically replaces previous content");
        free(c);
        rm_rf(dir); free(dir);
    }

    /* 8 + 9 + 10. publish_run end-to-end. */
    {
        char *dir = make_tmpdir("pub");
        char dbpath[1024];
        snprintf(dbpath, sizeof(dbpath), "%s/x.db", dir);
        struct nocturne_db *db = db_open(dbpath, NULL, NULL);
        /* Synth: 2 tracks + a manifest_current with one of them. */
        char *err = NULL;
        sqlite3_exec(db_handle(db),
            "INSERT INTO tracks (sha256, path, mtime_ns, size_bytes, "
            "title, artist, album, album_artist, year, genre, format, "
            "tags_status, date_added, last_seen_at) VALUES "
            "('"
            "0000000000000000000000000000000000000000000000000000000000000001"
            "', '/lib/a.mp3', 0, 1024, 'Title A', 'Artist A', 'Album A', "
            "'AA', '2020', 'Rock', 'mp3', 'ok', "
            "'2026-04-26T00:00:00Z', '2026-04-26T00:00:00Z'),"
            "('"
            "0000000000000000000000000000000000000000000000000000000000000002"
            "', '/lib/b.flac', 0, 2048, 'Title B', 'Artist B', 'Album B', "
            "'BB', '2021', 'Jazz', 'flac', 'ok', "
            "'2026-04-26T00:00:00Z', '2026-04-26T00:00:00Z')",
            NULL, NULL, &err);
        sqlite3_free(err);
        sqlite3_exec(db_handle(db),
            "INSERT INTO scan_meta (library_root, last_scan_at) VALUES "
            "('/lib', '2026-04-26T00:00:00Z')",
            NULL, NULL, NULL);
        sqlite3_exec(db_handle(db),
            "INSERT INTO manifest_current (sha256, buckets_csv, size_bytes) VALUES "
            "('0000000000000000000000000000000000000000000000000000000000000001', "
            " 'recent_adds', 1024)",
            NULL, NULL, NULL);
        sqlite3_exec(db_handle(db),
            "INSERT INTO manifest_meta (key, value) VALUES "
            "('cap_bytes', '12884901888'), "
            "('used_bytes', '1024'), "
            "('cold_start', '0'), "
            "('generated_at', 'deterministic-abcdef012345')",
            NULL, NULL, NULL);

        char outdir[1024];
        snprintf(outdir, sizeof(outdir), "%s/out", dir);
        int rc = publish_run(db, outdir);
        expect(rc == 0, "publish_run returns 0 with populated manifest_current");

        char catpath[1024]; snprintf(catpath, sizeof(catpath), "%s/catalog.json", outdir);
        char manpath[1024]; snprintf(manpath, sizeof(manpath), "%s/manifest.json", outdir);

        json_error_t je;
        json_t *cat = json_load_file(catpath, 0, &je);
        expect(cat != NULL, "catalog.json parses as JSON");
        if (cat) {
            json_t *v = json_object_get(cat, "v");
            expect(v && json_integer_value(v) == 1, "catalog.v == 1");
            json_t *tr = json_object_get(cat, "tracks");
            expect(tr && json_is_array(tr) && json_array_size(tr) == 2,
                   "catalog.tracks has 2 entries");
            /* Path is RELATIVE: should not start with / */
            json_t *t0 = json_array_get(tr, 0);
            json_t *p = t0 ? json_object_get(t0, "path") : NULL;
            const char *pstr = p ? json_string_value(p) : NULL;
            expect(pstr && pstr[0] != '/',
                   "catalog: track path is relative (no leading /)");
            json_decref(cat);
        }

        json_t *man = json_load_file(manpath, 0, &je);
        expect(man != NULL, "manifest.json parses as JSON");
        if (man) {
            json_t *v = json_object_get(man, "v");
            expect(v && json_integer_value(v) == 1, "manifest.v == 1");
            json_t *res = json_object_get(man, "resident");
            expect(res && json_is_array(res) && json_array_size(res) == 1,
                   "manifest.resident has 1 entry from manifest_current");
            json_t *cap = json_object_get(man, "cap_bytes");
            expect(cap && json_integer_value(cap) > 0,
                   "manifest.cap_bytes > 0");
            json_decref(man);
        }

        expect(!has_tmp_artifact(outdir),
               "publish_run: no .tmp leaks in out_dir");

        db_close(db);
        rm_rf(dir); free(dir);
    }

    /* 11. publish_run with empty manifest_current returns non-zero. */
    {
        char *dir = make_tmpdir("emp");
        char dbpath[1024];
        snprintf(dbpath, sizeof(dbpath), "%s/x.db", dir);
        struct nocturne_db *db = db_open(dbpath, NULL, NULL);
        sqlite3_exec(db_handle(db),
            "INSERT INTO scan_meta (library_root, last_scan_at) VALUES "
            "('/lib', '2026-04-26T00:00:00Z')",
            NULL, NULL, NULL);

        char outdir[1024];
        snprintf(outdir, sizeof(outdir), "%s/out", dir);
        int rc = publish_run(db, outdir);
        expect(rc != 0,
               "publish_run with empty manifest_current returns non-zero");
        db_close(db);
        rm_rf(dir); free(dir);
    }

    return test_finish(__FILE__);
}
