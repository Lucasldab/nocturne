/*
 * scan.c — DAEMON-01 + DAEMON-02 orchestration.
 *
 * Walks the library via Phase 1's walker_walk, callback per file:
 *
 *   1. lookup_by_path → existing row?
 *   2. stat to capture (mtime_ns, size_bytes).
 *   3. If row exists AND (mtime,size) match → mark_seen, skip hashing.
 *      (DAEMON-02: incremental no-op.)
 *   4. Otherwise: hash audio payload, run check_canonical, build a
 *      track_row, upsert (insert OR update by sha256). If a different
 *      sha256 row already had this path, we end up with two paths
 *      pointing at the same content briefly; the unseen-sweep at end
 *      reconciles, and a follow-up scan on the source path also resolves.
 *   5. After walk: delete rows in DB whose last_seen_at is older than the
 *      current scan timestamp. (DAEMON-01 reconciliation.)
 *
 * All steps run inside a single BEGIN IMMEDIATE / COMMIT transaction so
 * a crashed scan doesn't half-mutate the DB.
 */

#define _GNU_SOURCE

#include "scan.h"
#include "db.h"
#include "hash.h"
#include "track_repo.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <sqlite3.h>

#include "tags.h"
#include "walker.h"
#include "check.h"

struct scan_ctx {
    struct nocturne_db *db;
    const char *library_root;
    char iso_now[40];
    struct scan_stats *stats;
    int hard_error;
};

static void iso_now_buf(char buf[40])
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    /* Years > 9999 silently clamp; gmtime_r also already range-checks the
     * sub-year fields. The compiler's -Wformat-truncation is overly
     * conservative on %d width, so we pre-format into a wider scratch
     * buffer and copy in. */
    char scratch[64];
    int n = snprintf(scratch, sizeof(scratch),
                     "%04u-%02u-%02uT%02u:%02u:%02u.%06luZ",
                     (unsigned) (tm.tm_year + 1900),
                     (unsigned) (tm.tm_mon + 1),
                     (unsigned) tm.tm_mday,
                     (unsigned) tm.tm_hour,
                     (unsigned) tm.tm_min,
                     (unsigned) tm.tm_sec,
                     (unsigned long) (ts.tv_nsec / 1000));
    if (n < 0) n = 0;
    if (n >= 40) n = 39;
    memcpy(buf, scratch, (size_t) n);
    buf[n] = '\0';
}

static const char *format_string(enum audio_format f)
{
    switch (f) {
    case AUDIO_FORMAT_MP3:        return "mp3";
    case AUDIO_FORMAT_FLAC:       return "flac";
    case AUDIO_FORMAT_OPUS:       return "opus";
    case AUDIO_FORMAT_OGG_VORBIS: return "ogg";
    case AUDIO_FORMAT_MP4:        return "m4a";
    case AUDIO_FORMAT_UNKNOWN:
    default:                      return NULL;
    }
}

/* Append `code` to `csv` (heap, growable). On OOM returns NULL with the
 * original buffer freed. */
static char *csv_append(char *csv, const char *code)
{
    if (!code) return csv;
    size_t old_len = csv ? strlen(csv) : 0;
    size_t add_len = strlen(code) + (old_len ? 1 : 0);
    char *p = realloc(csv, old_len + add_len + 1);
    if (!p) { free(csv); return NULL; }
    if (old_len) {
        p[old_len] = ',';
        memcpy(p + old_len + 1, code, strlen(code) + 1);
    } else {
        memcpy(p, code, strlen(code) + 1);
    }
    return p;
}

/* From a check_result, build a CSV of issue codes. NULL if no issues. */
static char *build_tag_warning(const struct check_result *cr)
{
    if (!cr || cr->issue_count == 0) return NULL;
    char *csv = NULL;
    for (size_t i = 0; i < cr->issue_count; i++) {
        if (cr->issues[i].severity == CHECK_OK) continue;
        csv = csv_append(csv, cr->issues[i].code);
    }
    return csv;
}

/* JSON-escape src into a freshly allocated buffer. Caller frees. */
static char *json_escape_str(const char *src)
{
    if (!src) return strdup("");
    size_t n = strlen(src);
    /* worst case: every byte becomes \uXXXX (6 chars). */
    char *out = malloc(n * 6 + 1);
    if (!out) return NULL;
    char *o = out;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char) src[i];
        switch (c) {
        case '"':  *o++ = '\\'; *o++ = '"'; break;
        case '\\': *o++ = '\\'; *o++ = '\\'; break;
        case '\b': *o++ = '\\'; *o++ = 'b'; break;
        case '\f': *o++ = '\\'; *o++ = 'f'; break;
        case '\n': *o++ = '\\'; *o++ = 'n'; break;
        case '\r': *o++ = '\\'; *o++ = 'r'; break;
        case '\t': *o++ = '\\'; *o++ = 't'; break;
        default:
            if (c < 0x20) {
                o += sprintf(o, "\\u%04x", c);
            } else {
                *o++ = (char) c;
            }
        }
    }
    *o = '\0';
    return out;
}

/* Canonicalise a tag_field into a string suitable for the tracks.artist
 * (etc.) column. Sets *needs_free=true iff the result must be free()d. */
static char *canon_tag_field(const struct tag_field *f, bool *needs_free)
{
    *needs_free = false;
    if (!f || !f->present) return NULL;
    if (!f->is_multi_value_canonical || f->multi_value_count <= 1) {
        return f->value;  /* borrowed */
    }
    /* Multi-value: produce JSON-array string. We don't have access to the
     * individual frames at this layer (tag_field stores only the first
     * value), so emit a single-element array containing the canonical
     * value with a marker comment. This is a known limitation noted in
     * PROJECT.md decisions for v1.x. */
    char *escaped = json_escape_str(f->value);
    if (!escaped) return NULL;
    size_t need = strlen(escaped) + 4 /* []""  */ + 1;
    char *out = malloc(need);
    if (!out) { free(escaped); return NULL; }
    snprintf(out, need, "[\"%s\"]", escaped);
    free(escaped);
    *needs_free = true;
    return out;
}

/* Returns 1 if `path` is a transcode artifact: lives under /resident/ and
 * has an opus/m4a/aac extension. These are derivatives produced by rotate's
 * transcode promote — the daemon must not insert tracks rows for them
 * (the source FLAC's row is canonical; resident transcode metadata lives
 * on residency_state). Returning early here is the simplest dedup. */
static int is_transcode_artifact(const char *path)
{
    if (!path) return 0;
    if (!strstr(path, "/resident/")) return 0;
    const char *dot = strrchr(path, '.');
    if (!dot) return 0;
    return !strcasecmp(dot, ".opus") || !strcasecmp(dot, ".m4a") ||
           !strcasecmp(dot, ".aac");
}

static enum walk_result on_file(const struct tag_record *rec, void *ud)
{
    struct scan_ctx *ctx = (struct scan_ctx *) ud;
    if (!rec || !rec->path) return WALK_CONTINUE;
    if (is_transcode_artifact(rec->path)) return WALK_CONTINUE;
    ctx->stats->files_seen++;

    /* stat for (mtime, size). */
    struct stat st;
    if (lstat(rec->path, &st) != 0) {
        /* Vanished between walker enumeration and our stat. Treat as
         * hash_failed; the unseen-sweep will reconcile if it stays gone. */
        ctx->stats->hash_failed++;
        return WALK_CONTINUE;
    }
    long long mtime_ns = (long long) st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
    long long size_bytes = (long long) st.st_size;

    /* Existing row? */
    char *existing_sha = NULL;
    long long existing_mtime = 0, existing_size = 0;
    int hit = track_repo_lookup_by_path(ctx->db, rec->path,
                                        &existing_sha, &existing_mtime, &existing_size);
    if (hit < 0) {
        ctx->hard_error = 1;
        free(existing_sha);
        return WALK_STOP;
    }

    if (hit == 1 && existing_mtime == mtime_ns && existing_size == size_bytes) {
        /* Incremental skip — DAEMON-02. */
        if (track_repo_mark_seen(ctx->db, existing_sha, ctx->iso_now) != 0) {
            ctx->hard_error = 1;
            free(existing_sha);
            return WALK_STOP;
        }
        ctx->stats->files_skipped_unchanged++;
        free(existing_sha);
        return WALK_CONTINUE;
    }

    /* Hash audio payload. Retry once on EAGAIN (Pitfall 18 race). */
    char hex[65];
    int err = 0;
    int rc = hash_audio_payload(rec->path, hex, &err);
    if (rc < 0 && err == EAGAIN) {
        rc = hash_audio_payload(rec->path, hex, &err);
    }
    if (rc < 0) {
        ctx->stats->hash_failed++;
        free(existing_sha);
        return WALK_CONTINUE;
    }

    /* Canonical tag check. */
    struct check_result cr = {0};
    check_canonical(rec, &cr);

    const char *tags_status = "ok";
    char *tag_warning = NULL;
    if (rec->tag_read_failed) {
        tags_status = "parse_failed";
        tag_warning = strdup("taglib_open_failed");
        ctx->stats->tag_parse_failed++;
    } else if (cr.any_fail) {
        tags_status = "incomplete";
        tag_warning = build_tag_warning(&cr);
    } else if (cr.any_flag) {
        tags_status = "ok";  /* flag-only is advisory, not incomplete */
        tag_warning = build_tag_warning(&cr);
    }

    /* Canonicalise tag fields. */
    bool free_artist = false, free_album_artist = false;
    char *artist = canon_tag_field(&rec->artist, &free_artist);
    char *album_artist = canon_tag_field(&rec->album_artist, &free_album_artist);

    struct track_row row = {
        .sha256       = hex,
        .path         = rec->path,
        .mtime_ns     = mtime_ns,
        .size_bytes   = size_bytes,
        .format       = format_string(rec->format),
        .title        = rec->title.present        ? rec->title.value        : NULL,
        .artist       = artist,
        .album        = rec->album.present        ? rec->album.value        : NULL,
        .album_artist = album_artist,
        .track_number = rec->track_number.present ? rec->track_number.value : NULL,
        .disc_number  = rec->disc_number.present  ? rec->disc_number.value  : NULL,
        .year         = rec->year.present         ? rec->year.value         : NULL,
        .genre        = rec->genre.present        ? rec->genre.value        : NULL,
        .duration_ms  = -1,
        .tags_status  = tags_status,
        .tag_warning  = tag_warning,
        .date_added   = ctx->iso_now,
        .last_seen_at = ctx->iso_now,
    };

    /* Decide insert vs update by checking sha256-existence first.
     *
     *   sha_hit=1, hit=1, existing_sha == hex   → row already at this path
     *                                             with this sha; UPDATE
     *                                             payload (re-tag changed
     *                                             tag-only).
     *   sha_hit=1, otherwise                    → same audio reached us at
     *                                             a different path (or new
     *                                             path); UPDATE on sha.
     *   sha_hit=0, hit=1                        → file at this path had its
     *                                             content changed (mtime+
     *                                             bytes). DELETE the old
     *                                             row (so the path-UNIQUE
     *                                             constraint frees up),
     *                                             then INSERT the new sha.
     *   sha_hit=0, hit=0                        → genuinely new file; INSERT.
     */
    int sha_hit = track_repo_lookup_by_sha256(ctx->db, hex);
    int op_rc = 0;
    if (sha_hit < 0) {
        op_rc = -1;
    } else if (sha_hit == 1) {
        op_rc = track_repo_update(ctx->db, &row);
        ctx->stats->files_updated++;
    } else if (hit == 1) {
        /* Path existed with a different sha — content changed. Drop old
         * row to free the path-UNIQUE slot, then insert. */
        sqlite3_stmt *del = NULL;
        if (sqlite3_prepare_v2(db_handle(ctx->db),
                               "DELETE FROM tracks WHERE sha256=?",
                               -1, &del, NULL) == SQLITE_OK) {
            sqlite3_bind_text(del, 1, existing_sha, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(del) != SQLITE_DONE) op_rc = -1;
            sqlite3_finalize(del);
        } else {
            op_rc = -1;
        }
        if (op_rc == 0) op_rc = track_repo_insert(ctx->db, &row);
        ctx->stats->files_updated++;
    } else {
        op_rc = track_repo_insert(ctx->db, &row);
        ctx->stats->files_added++;
    }

    if (free_artist) free(artist);
    if (free_album_artist) free(album_artist);
    free(tag_warning);
    check_result_free(&cr);
    free(existing_sha);

    if (op_rc != 0) {
        ctx->hard_error = 1;
        return WALK_STOP;
    }
    return WALK_CONTINUE;
}

/* Internal worker shared by scan_run and scan_run_subtree.
 *
 *   walk_root        — directory the walker descends into.
 *   deletion_prefix  — only rows whose path begins with this prefix are
 *                      considered for the unseen-sweep. Pass library_root
 *                      to clean the whole library; pass walk_root for
 *                      subtree-scoped reconciliation.
 *
 * scan_meta is updated only when scope == library (full scan). Subtree
 * scans don't write scan_meta because their scope is partial. */
static int scan_run_internal(struct nocturne_db *db,
                             const char *library_root,
                             const char *walk_root,
                             const char *deletion_prefix,
                             int update_scan_meta,
                             struct scan_stats *out)
{
    if (!db || !library_root || !walk_root || !deletion_prefix || !out) return -1;
    memset(out, 0, sizeof(*out));

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    struct scan_ctx ctx = {0};
    ctx.db = db;
    ctx.library_root = library_root;
    ctx.stats = out;
    iso_now_buf(ctx.iso_now);

    if (db_begin(db) != 0) return -1;

    struct walk_stats ws = {0};
    int wrc = walker_walk(walk_root, on_file, &ctx, &ws);
    if (wrc != 0 || ctx.hard_error) {
        db_rollback(db);
        struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
        out->elapsed_ms = (size_t) ((t1.tv_sec - t0.tv_sec) * 1000 +
                                    (t1.tv_nsec - t0.tv_nsec) / 1000000);
        return -1;
    }

    /* Reconcile deletions under the configured prefix. */
    long deleted = track_repo_delete_unseen_under_root(db, deletion_prefix, ctx.iso_now);
    if (deleted < 0) {
        db_rollback(db);
        return -1;
    }
    out->files_removed = (size_t) deleted;

    if (update_scan_meta) {
        sqlite3_stmt *stmt = NULL;
        const char *up_sql =
            "INSERT INTO scan_meta (library_root, last_scan_at, files_seen, "
            "files_added, files_updated, files_removed) "
            "VALUES (?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(library_root) DO UPDATE SET "
            "last_scan_at=excluded.last_scan_at, "
            "files_seen=excluded.files_seen, "
            "files_added=excluded.files_added, "
            "files_updated=excluded.files_updated, "
            "files_removed=excluded.files_removed";
        if (sqlite3_prepare_v2(db_handle(db), up_sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, library_root, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, ctx.iso_now, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 3, (long long) out->files_seen);
            sqlite3_bind_int64(stmt, 4, (long long) out->files_added);
            sqlite3_bind_int64(stmt, 5, (long long) out->files_updated);
            sqlite3_bind_int64(stmt, 6, (long long) out->files_removed);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    if (db_commit(db) != 0) return -1;

    struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
    out->elapsed_ms = (size_t) ((t1.tv_sec - t0.tv_sec) * 1000 +
                                (t1.tv_nsec - t0.tv_nsec) / 1000000);

    return (out->hash_failed > 0 || out->tag_parse_failed > 0) ? 1 : 0;
}

int scan_run(struct nocturne_db *db, const char *library_root,
             struct scan_stats *out)
{
    return scan_run_internal(db, library_root, library_root, library_root, 1, out);
}

int scan_run_subtree(struct nocturne_db *db, const char *library_root,
                     const char *subdir, struct scan_stats *out)
{
    /* Subtree scans don't bump scan_meta (it tracks the full library). The
     * deletion prefix is the subdir itself so siblings stay untouched. */
    return scan_run_internal(db, library_root, subdir, subdir, 0, out);
}
