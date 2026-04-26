/*
 * catalog.c — catalog.json emit + import.
 *
 * Emit: SELECT every track row sorted by sha256, build a jansson tree,
 * dump with stable key order. Multi-value fields (artist, album_artist,
 * genre): if the stored string is a JSON array, splat; otherwise wrap
 * the single string in a one-element array. track_number / disc_number
 * stored as "5/12" → integer 5 (drop the total).
 *
 * Path normalisation: load `scan_meta.library_root`; strip prefix from
 * each track's absolute path. Rows whose path doesn't begin with the
 * prefix are skipped (defensive — shouldn't happen in practice).
 *
 * Import: parse JSON, refuse v != 1, UPSERT each track via track_repo.
 * Path reconstructed from library_root + relative.
 */

#define _GNU_SOURCE

#include "catalog.h"
#include "db.h"
#include "track_repo.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <jansson.h>
#include <sqlite3.h>

/* === helpers ============================================================= */

static char *load_library_root(struct sqlite3 *raw)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(raw,
            "SELECT library_root FROM scan_meta ORDER BY last_scan_at DESC LIMIT 1",
            -1, &st, NULL) != SQLITE_OK) return NULL;
    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *t = sqlite3_column_text(st, 0);
        if (t) out = strdup((const char *) t);
    }
    sqlite3_finalize(st);
    return out;
}

/* If `s` parses as a JSON array of strings, return the parsed value
 * (caller decrefs). Otherwise return NULL. */
static json_t *try_parse_json_str_array(const char *s)
{
    if (!s || s[0] != '[') return NULL;
    json_error_t err;
    json_t *v = json_loads(s, 0, &err);
    if (!v) return NULL;
    if (!json_is_array(v)) { json_decref(v); return NULL; }
    return v;
}

/* Build a JSON array for a multi-value column. NULL/empty → empty array. */
static json_t *as_json_array(const char *s)
{
    if (!s || !*s) return json_array();
    json_t *arr = try_parse_json_str_array(s);
    if (arr) return arr;
    json_t *out = json_array();
    json_array_append_new(out, json_string(s));
    return out;
}

/* "5/12" → 5; "5" → 5; NULL/non-numeric → null json. */
static json_t *as_json_track_int(const char *s)
{
    if (!s || !*s) return json_null();
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s) return json_null();
    return json_integer((json_int_t) v);
}

/* Strip `prefix` + '/' from `path` if present; otherwise return strdup of
 * `path`. Caller frees. */
static char *make_relative(const char *path, const char *prefix)
{
    if (!path) return NULL;
    if (!prefix || !*prefix) return strdup(path);
    size_t n = strlen(prefix);
    if (strncmp(path, prefix, n) == 0) {
        const char *tail = path + n;
        while (*tail == '/') tail++;
        return strdup(tail);
    }
    return strdup(path);
}

static char *iso_utc_now(void)
{
    char buf[40];
    time_t now = time(NULL);
    struct tm tm; gmtime_r(&now, &tm);
    snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02uZ",
             (unsigned)(tm.tm_year + 1900), (unsigned)(tm.tm_mon + 1),
             (unsigned)tm.tm_mday, (unsigned)tm.tm_hour,
             (unsigned)tm.tm_min, (unsigned)tm.tm_sec);
    return strdup(buf);
}

/* === emit ================================================================ */

int catalog_emit(struct nocturne_db *db, FILE *f)
{
    if (!db || !f) return -1;
    struct sqlite3 *raw = db_handle(db);
    if (!raw) return -1;

    char *library_root = load_library_root(raw);
    /* If no scan_meta exists, library_root remains NULL; paths emit as-is. */

    json_t *root = json_object();
    json_object_set_new(root, "v", json_integer(1));
    char *now = iso_utc_now();
    json_object_set_new(root, "generated_at", json_string(now ? now : ""));
    free(now);

    json_t *tracks = json_array();
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT sha256, path, mtime_ns, size_bytes, format, "
        "       title, artist, album, album_artist, track_number, "
        "       disc_number, year, genre, duration_ms, date_added "
        "FROM tracks ORDER BY sha256 ASC";
    if (sqlite3_prepare_v2(raw, sql, -1, &st, NULL) != SQLITE_OK) {
        json_decref(root);
        free(library_root);
        return -1;
    }

    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *sha    = (const char *) sqlite3_column_text(st, 0);
        const char *path   = (const char *) sqlite3_column_text(st, 1);
        long long mtime    = sqlite3_column_int64(st, 2);
        long long size     = sqlite3_column_int64(st, 3);
        const char *fmt    = (const char *) sqlite3_column_text(st, 4);
        const char *title  = (const char *) sqlite3_column_text(st, 5);
        const char *artist = (const char *) sqlite3_column_text(st, 6);
        const char *album  = (const char *) sqlite3_column_text(st, 7);
        const char *aa     = (const char *) sqlite3_column_text(st, 8);
        const char *tn     = (const char *) sqlite3_column_text(st, 9);
        const char *dn     = (const char *) sqlite3_column_text(st, 10);
        const char *year   = (const char *) sqlite3_column_text(st, 11);
        const char *genre  = (const char *) sqlite3_column_text(st, 12);
        long long dur      = sqlite3_column_int64(st, 13);
        const char *dadd   = (const char *) sqlite3_column_text(st, 14);

        char *rel = make_relative(path, library_root);

        json_t *t = json_object();
        json_object_set_new(t, "id", json_string(sha ? sha : ""));
        json_object_set_new(t, "path", json_string(rel ? rel : ""));
        json_object_set_new(t, "title", title ? json_string(title) : json_null());
        json_object_set_new(t, "artist", as_json_array(artist));
        json_object_set_new(t, "album", album ? json_string(album) : json_null());
        json_object_set_new(t, "album_artist", as_json_array(aa));
        json_object_set_new(t, "track", as_json_track_int(tn));
        json_object_set_new(t, "disc", as_json_track_int(dn));
        json_object_set_new(t, "year", as_json_track_int(year));
        json_object_set_new(t, "genre", as_json_array(genre));
        if (sqlite3_column_type(st, 13) == SQLITE_NULL) {
            json_object_set_new(t, "duration_ms", json_null());
        } else {
            json_object_set_new(t, "duration_ms", json_integer(dur));
        }
        json_object_set_new(t, "size_bytes", json_integer(size));
        json_object_set_new(t, "format", fmt ? json_string(fmt) : json_null());
        json_object_set_new(t, "mtime_ns", json_integer(mtime));
        json_object_set_new(t, "date_added", json_string(dadd ? dadd : ""));

        json_array_append_new(tracks, t);
        free(rel);
    }
    sqlite3_finalize(st);

    json_object_set_new(root, "tracks", tracks);

    int rc = json_dumpf(root, f, JSON_INDENT(2) | JSON_SORT_KEYS);

    json_decref(root);
    free(library_root);
    return rc;
}

/* === import ============================================================== */

static const char *json_str_or_null(json_t *o, const char *key)
{
    if (!o) return NULL;
    json_t *v = json_object_get(o, key);
    if (!v || !json_is_string(v)) return NULL;
    return json_string_value(v);
}

/* Serialize a JSON array of strings back into the [..,..] string form
 * scan.c uses for multi-value canonical fields. NULL/empty → NULL.
 * Single-element array → bare value (matches scan.c when count <= 1). */
static char *array_to_storage(json_t *arr)
{
    if (!arr || !json_is_array(arr)) return NULL;
    size_t n = json_array_size(arr);
    if (n == 0) return NULL;
    if (n == 1) {
        json_t *e = json_array_get(arr, 0);
        if (!json_is_string(e)) return NULL;
        return strdup(json_string_value(e));
    }
    char *buf = json_dumps(arr, 0);  /* ["a","b"] */
    return buf;
}

long catalog_import(struct nocturne_db *db, FILE *f)
{
    if (!db || !f) return -1;
    json_error_t je;
    json_t *root = json_loadf(f, 0, &je);
    if (!root) {
        fprintf(stderr, "catalog_import: parse error at line %d: %s\n",
                je.line, je.text);
        return -1;
    }
    json_t *v = json_object_get(root, "v");
    if (!v || !json_is_integer(v) || json_integer_value(v) != 1) {
        fprintf(stderr, "catalog_import: unsupported schema version\n");
        json_decref(root);
        return -1;
    }
    json_t *tracks = json_object_get(root, "tracks");
    if (!tracks || !json_is_array(tracks)) {
        json_decref(root);
        return -1;
    }

    char *library_root = load_library_root(db_handle(db));

    long imported = 0;
    size_t n = json_array_size(tracks);
    for (size_t i = 0; i < n; i++) {
        json_t *t = json_array_get(tracks, i);
        if (!json_is_object(t)) continue;
        const char *sha  = json_str_or_null(t, "id");
        const char *rel  = json_str_or_null(t, "path");
        if (!sha || !rel) continue;

        char abs_path[4096];
        if (library_root && library_root[0] == '/') {
            snprintf(abs_path, sizeof(abs_path), "%s/%s", library_root, rel);
        } else {
            snprintf(abs_path, sizeof(abs_path), "%s", rel);
        }

        json_t *artist_arr = json_object_get(t, "artist");
        json_t *aa_arr     = json_object_get(t, "album_artist");
        json_t *genre_arr  = json_object_get(t, "genre");

        char *artist = array_to_storage(artist_arr);
        char *aa     = array_to_storage(aa_arr);
        char *genre  = array_to_storage(genre_arr);

        char tn_buf[16], dn_buf[16], year_buf[16];
        json_t *jtn   = json_object_get(t, "track");
        json_t *jdn   = json_object_get(t, "disc");
        json_t *jyear = json_object_get(t, "year");
        const char *tn_s = NULL, *dn_s = NULL, *year_s = NULL;
        if (jtn && json_is_integer(jtn)) {
            snprintf(tn_buf, sizeof(tn_buf), "%lld", (long long) json_integer_value(jtn));
            tn_s = tn_buf;
        }
        if (jdn && json_is_integer(jdn)) {
            snprintf(dn_buf, sizeof(dn_buf), "%lld", (long long) json_integer_value(jdn));
            dn_s = dn_buf;
        }
        if (jyear && json_is_integer(jyear)) {
            snprintf(year_buf, sizeof(year_buf), "%lld", (long long) json_integer_value(jyear));
            year_s = year_buf;
        }

        json_t *jdur = json_object_get(t, "duration_ms");
        long long dur = (jdur && json_is_integer(jdur)) ? json_integer_value(jdur) : -1;
        json_t *jsize = json_object_get(t, "size_bytes");
        long long size = (jsize && json_is_integer(jsize)) ? json_integer_value(jsize) : 0;
        json_t *jmtime = json_object_get(t, "mtime_ns");
        long long mtime = (jmtime && json_is_integer(jmtime)) ? json_integer_value(jmtime) : 0;

        struct track_row row = {
            .sha256       = sha,
            .path         = abs_path,
            .mtime_ns     = mtime,
            .size_bytes   = size,
            .format       = json_str_or_null(t, "format"),
            .title        = json_str_or_null(t, "title"),
            .artist       = artist,
            .album        = json_str_or_null(t, "album"),
            .album_artist = aa,
            .track_number = tn_s,
            .disc_number  = dn_s,
            .year         = year_s,
            .genre        = genre,
            .duration_ms  = dur,
            .tags_status  = "ok",
            .tag_warning  = NULL,
            .date_added   = json_str_or_null(t, "date_added"),
            .last_seen_at = json_str_or_null(t, "date_added"),
        };
        if (track_repo_upsert(db, &row) == 0) imported++;

        free(artist); free(aa); free(genre);
    }

    free(library_root);
    json_decref(root);
    return imported;
}
