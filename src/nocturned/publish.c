/*
 * publish.c — orchestrates catalog + manifest atomic write into out_dir.
 *
 * The catalog is a fresh emit from the tracks table; the manifest is a
 * read of manifest_current + manifest_meta (resolver populates these
 * from `nocturned resolve`). Both files go through atomic_writer_open →
 * write → commit so partial writes are never observable (Pitfall 19).
 */

#define _GNU_SOURCE

#include "publish.h"

#include "atomic_io.h"
#include "catalog.h"
#include "db.h"
#include "paths.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <jansson.h>
#include <sqlite3.h>

static char *iso_utc_now_str(void)
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

static long long meta_long(struct sqlite3 *raw, const char *key, long long fallback)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(raw, "SELECT value FROM manifest_meta WHERE key=?",
                           -1, &st, NULL) != SQLITE_OK) return fallback;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    long long v = fallback;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *t = sqlite3_column_text(st, 0);
        if (t) v = strtoll((const char *) t, NULL, 10);
    }
    sqlite3_finalize(st);
    return v;
}

/* Build the manifest JSON tree from manifest_current + manifest_meta.
 * Returns NULL if manifest_current is empty (caller surfaces error). */
static json_t *load_manifest_tree(struct sqlite3 *raw)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(raw,
            "SELECT sha256, buckets_csv FROM manifest_current ORDER BY sha256 ASC",
            -1, &st, NULL) != SQLITE_OK) return NULL;
    json_t *resident = json_array();
    int n = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *sha = (const char *) sqlite3_column_text(st, 0);
        const char *csv = (const char *) sqlite3_column_text(st, 1);
        if (!sha) continue;
        json_t *t = json_object();
        json_object_set_new(t, "id", json_string(sha));
        json_t *buckets = json_array();
        if (csv) {
            char *dup = strdup(csv);
            char *tok = strtok(dup, ",");
            while (tok) {
                json_array_append_new(buckets, json_string(tok));
                tok = strtok(NULL, ",");
            }
            free(dup);
        }
        json_object_set_new(t, "buckets", buckets);
        json_array_append_new(resident, t);
        n++;
    }
    sqlite3_finalize(st);
    if (n == 0) {
        json_decref(resident);
        return NULL;
    }

    json_t *root = json_object();
    json_object_set_new(root, "v", json_integer(1));
    char *now = iso_utc_now_str();
    json_object_set_new(root, "generated_at", json_string(now ? now : ""));
    free(now);
    json_object_set_new(root, "cap_bytes",  json_integer(meta_long(raw, "cap_bytes", 0)));
    json_object_set_new(root, "used_bytes", json_integer(meta_long(raw, "used_bytes", 0)));
    json_object_set_new(root, "resident", resident);
    return root;
}

/* Read a whole file into a malloc'd buffer. Returns 0 on success. */
static int slurp(const char *path, char **out, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char *buf = NULL;
    size_t len = 0;
    FILE *ms = open_memstream(&buf, &len);
    if (!ms) { fclose(f); return -1; }
    char chunk[65536];
    size_t n;
    int rc = 0;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (fwrite(chunk, 1, n, ms) != n) { rc = -1; break; }
    }
    if (ferror(f)) rc = -1;
    fclose(f);
    if (fclose(ms) != 0) rc = -1;
    if (rc != 0) { free(buf); return -1; }
    *out = buf;
    *out_len = len;
    return 0;
}

/* Returns 1 when the catalog already on disk at `path` is byte-identical
 * to a fresh emit apart from its generated_at stamp, 0 otherwise.
 *
 * The phone reimports the whole catalog (and resets the play queue)
 * whenever catalog.json's mtime moves, and Syncthing re-sends it, so an
 * unchanged catalog must not be rewritten just to bump the timestamp.
 * The check re-emits with the old file's stamp and compares bytes, which
 * keeps generated_at in place for the phone's parser. */
static int catalog_unchanged(struct nocturne_db *db, const char *path)
{
    char *old = NULL;
    size_t old_len = 0;
    if (slurp(path, &old, &old_len) != 0) return 0;

    int same = 0;
    json_error_t je;
    json_t *prev = json_loadb(old, old_len, 0, &je);
    const char *stamp = prev ? json_string_value(json_object_get(prev, "generated_at")) : NULL;
    if (stamp) {
        char *fresh = NULL;
        size_t fresh_len = 0;
        FILE *ms = open_memstream(&fresh, &fresh_len);
        if (ms) {
            int erc = catalog_emit_at(db, ms, stamp);
            if (fclose(ms) == 0 && erc == 0 && fresh_len == old_len &&
                memcmp(fresh, old, old_len) == 0) {
                same = 1;
            }
            free(fresh);
        }
    }
    json_decref(prev);
    free(old);
    return same;
}

static int publish_catalog(struct nocturne_db *db, const char *path)
{
    if (catalog_unchanged(db, path)) return 0;

    struct atomic_writer *cw = atomic_writer_open(path);
    if (!cw) return -1;
    if (catalog_emit(db, atomic_writer_file(cw)) != 0) {
        atomic_writer_abort(cw);
        return -1;
    }
    return atomic_writer_commit(cw);
}

int publish_run(struct nocturne_db *db, const char *out_dir)
{
    if (!db || !out_dir) { errno = EINVAL; return -1; }
    if (paths_mkdir_p(out_dir, 0755) != 0) return -1;

    /* manifest first: if it's empty we exit early without writing the
     * catalog (we'd rather have neither than the catalog drift ahead of
     * a stale or absent manifest on the consumer). */
    json_t *manifest = load_manifest_tree(db_handle(db));
    if (!manifest) {
        fprintf(stderr,
            "nocturned publish: no manifest in DB — run `nocturned resolve` first\n");
        errno = EINVAL;
        return -1;
    }

    /* Catalog. */
    char cat_path[4096];
    snprintf(cat_path, sizeof(cat_path), "%s/catalog.json", out_dir);
    if (publish_catalog(db, cat_path) != 0) {
        json_decref(manifest);
        return -1;
    }

    /* Manifest. */
    char man_path[4096];
    snprintf(man_path, sizeof(man_path), "%s/manifest.json", out_dir);
    struct atomic_writer *mw = atomic_writer_open(man_path);
    if (!mw) { json_decref(manifest); return -1; }
    if (json_dumpf(manifest, atomic_writer_file(mw),
                   JSON_INDENT(2) | JSON_SORT_KEYS) != 0) {
        atomic_writer_abort(mw);
        json_decref(manifest);
        return -1;
    }
    if (atomic_writer_commit(mw) != 0) {
        json_decref(manifest);
        return -1;
    }

    json_decref(manifest);
    return 0;
}
