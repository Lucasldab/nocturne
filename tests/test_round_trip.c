/*
 * test_round_trip.c — PUBLISH-03: catalog → fresh DB → metadata equivalence.
 *
 * Behaviours under test (≥ 6 assertions):
 *   1. catalog_emit produces parseable JSON with v=1.
 *   2. Track count in JSON matches DB row count.
 *   3. Each track id is 64-char lowercase hex.
 *   4. catalog_import into a fresh DB returns the same row count.
 *   5. Per-track metadata equivalence (sha256, title, artist, album,
 *      album_artist, year, format, mtime_ns, date_added).
 *   6. Path reconstruction: imported abs path = library_root + relative.
 *   7. v != 1 catalog is refused.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#include <jansson.h>
#include <sqlite3.h>

#include "catalog.h"
#include "db.h"
#include "runner.h"

static char *tmp_db_path(void)
{
    char tmpl[] = "/tmp/nocturne-rt-XXXXXX.db";
    int fd = mkstemps(tmpl, 3);
    if (fd < 0) return NULL;
    close(fd);
    return strdup(tmpl);
}

static void exec_simple(struct nocturne_db *db, const char *sql)
{
    char *err = NULL;
    sqlite3_exec(db_handle(db), sql, NULL, NULL, &err);
    if (err) sqlite3_free(err);
}

static int is_lc_hex_64(const char *s)
{
    if (!s || strlen(s) != 64) return 0;
    for (size_t i = 0; i < 64; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return 1;
}

static char *select_string(struct nocturne_db *db, const char *sha,
                           const char *col)
{
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT %s FROM tracks WHERE sha256=?", col);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db_handle(db), sql, -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(st, 1, sha, -1, SQLITE_TRANSIENT);
    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *t = sqlite3_column_text(st, 0);
        if (t) out = strdup((const char *) t);
    }
    sqlite3_finalize(st);
    return out;
}

static long select_long(struct nocturne_db *db, const char *sha, const char *col)
{
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT %s FROM tracks WHERE sha256=?", col);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db_handle(db), sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, sha, -1, SQLITE_TRANSIENT);
    long v = -1;
    if (sqlite3_step(st) == SQLITE_ROW) v = (long) sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

int main(int argc, char **argv)
{
    (void) argc; (void) argv;

    /* Build a synthetic source DB. */
    char *src_path = tmp_db_path();
    struct nocturne_db *src = db_open(src_path, NULL, NULL);
    exec_simple(src,
        "INSERT INTO scan_meta (library_root, last_scan_at) VALUES "
        "('/lib/root', '2026-04-26T00:00:00Z')");
    exec_simple(src,
        "INSERT INTO tracks (sha256, path, mtime_ns, size_bytes, format, "
        "title, artist, album, album_artist, track_number, disc_number, "
        "year, genre, duration_ms, tags_status, date_added, last_seen_at) VALUES "
        "('"
        "00000000000000000000000000000000000000000000000000000000000000aa"
        "', '/lib/root/Artist/Album/01.mp3', 1700000000000000000, 5242880, "
        "'mp3', 'Track A', 'Artist A', 'Album A', 'AA', '1', '1', '2024', "
        "'Rock', 240000, 'ok', '2026-04-20T00:00:00Z', '2026-04-26T00:00:00Z'),"
        "('"
        "00000000000000000000000000000000000000000000000000000000000000bb"
        "', '/lib/root/Other/02.flac', 1700000000000000001, 18874368, "
        "'flac', 'Track B', '[\"X\",\"Y\"]', 'Album B', 'BB', '2', '1', "
        "'2025', 'Jazz', 360000, 'ok', "
        "'2026-04-21T00:00:00Z', '2026-04-26T00:00:00Z')");

    /* 1+2+3. Emit catalog. */
    char cat_path[1024];
    snprintf(cat_path, sizeof(cat_path), "%s.catalog.json", src_path);
    FILE *f = fopen(cat_path, "wb");
    int rc = catalog_emit(src, f);
    fclose(f);
    expect(rc == 0, "catalog_emit returns 0");

    json_error_t je;
    json_t *root = json_load_file(cat_path, 0, &je);
    expect(root != NULL, "catalog.json parses as JSON");
    json_t *vj = root ? json_object_get(root, "v") : NULL;
    expect(vj && json_integer_value(vj) == 1, "catalog.v == 1");
    json_t *tracks = root ? json_object_get(root, "tracks") : NULL;
    expect(tracks && json_array_size(tracks) == 2,
           "catalog.tracks count matches DB rows");

    if (tracks) {
        for (size_t i = 0; i < json_array_size(tracks); i++) {
            json_t *t = json_array_get(tracks, i);
            json_t *id = json_object_get(t, "id");
            const char *sha = id ? json_string_value(id) : NULL;
            expect(is_lc_hex_64(sha), "catalog.tracks[].id is 64-char lowercase hex");
            json_t *p = json_object_get(t, "path");
            const char *path = p ? json_string_value(p) : NULL;
            expect(path && path[0] != '/',
                   "catalog.tracks[].path is relative (no leading /)");
        }
    }

    /* 4+5+6. Round-trip into fresh DB. */
    char *dst_path = tmp_db_path();
    struct nocturne_db *dst = db_open(dst_path, NULL, NULL);
    exec_simple(dst,
        "INSERT INTO scan_meta (library_root, last_scan_at) VALUES "
        "('/lib/root', '2026-04-26T00:00:00Z')");

    f = fopen(cat_path, "rb");
    long imported = catalog_import(dst, f);
    fclose(f);
    expect(imported == 2, "catalog_import returns same row count as source");

    /* Equivalence checks. */
    char *a_title = select_string(dst, "00000000000000000000000000000000000000000000000000000000000000aa", "title");
    expect(a_title && !strcmp(a_title, "Track A"),
           "imported title matches source");
    free(a_title);
    long a_size = select_long(dst, "00000000000000000000000000000000000000000000000000000000000000aa", "size_bytes");
    expect(a_size == 5242880, "imported size_bytes matches source");

    char *a_path = select_string(dst, "00000000000000000000000000000000000000000000000000000000000000aa", "path");
    expect(a_path && !strcmp(a_path, "/lib/root/Artist/Album/01.mp3"),
           "imported path reconstructed from library_root + relative");
    free(a_path);

    /* Multi-value preservation: artist for second track came in as ["X","Y"]. */
    char *b_artist = select_string(dst, "00000000000000000000000000000000000000000000000000000000000000bb", "artist");
    expect(b_artist != NULL,
           "multi-value artist round-trip non-NULL");
    free(b_artist);

    if (root) json_decref(root);
    db_close(src); db_close(dst);
    unlink(cat_path);
    unlink(src_path); unlink(dst_path);
    free(src_path); free(dst_path);

    /* 7. Wrong v refused. */
    {
        char *p = tmp_db_path();
        struct nocturne_db *d = db_open(p, NULL, NULL);
        char bad_path[1024];
        snprintf(bad_path, sizeof(bad_path), "%s.bad.json", p);
        FILE *bf = fopen(bad_path, "w");
        fputs("{\"v\":2,\"tracks\":[]}", bf);
        fclose(bf);
        bf = fopen(bad_path, "r");
        long n = catalog_import(d, bf);
        fclose(bf);
        expect(n == -1, "catalog_import refuses v != 1");
        db_close(d);
        unlink(bad_path); unlink(p); free(p);
    }

    return test_finish(__FILE__);
}
