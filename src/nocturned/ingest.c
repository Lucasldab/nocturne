/*
 * ingest.c — Phase 7 desktop ingester engine.
 *
 * Consumes per-device JSONL streams from the metadata sync folder:
 *
 *   <meta_dir>/stats/phone-<deviceid>.jsonl   — play / skip events
 *   <meta_dir>/likes-phone-<deviceid>.jsonl   — like / unlike events
 *   <meta_dir>/pins-phone-<deviceid>.jsonl    — pin / unpin events
 *
 * For each file: look up the persisted byte offset in `ingest_offsets`,
 * jsonl_open at that offset, jansson-parse each line, dispatch to a
 * handler (plays / likes / pins), persist the new offset. Re-running on
 * the same input set is a no-op via the offset table (INGEST-02).
 *
 * Likes / pins use last-writer-wins per `(unit, id)` keyed on max `ts`;
 * SQL is `INSERT ... ON CONFLICT DO UPDATE WHERE excluded.ts >= existing.ts`
 * so out-of-order replay (older event arriving after newer) converges
 * deterministically (INGEST-03). The `>=` (not `>`) means same-ts
 * collisions are also deterministic — last writer wins by file glob
 * order, but the resolved state is invariant under reordering of equal-ts
 * events.
 *
 * Plays are append-only; ingest_offsets prevents re-reading already-
 * consumed bytes, so no per-row dedup is needed.
 *
 * Per-line errors (malformed JSON, validation failure, unknown event)
 * log to stderr and increment `parse_error`; the offset still advances
 * past the offending line so the file isn't permanently stuck.
 *
 * Per-file errors (open / read / DB) abort the file but try the next
 * one. Top-level fatal (db_handle null, OOM) returns -1.
 *
 * APPEND-ONLY INVARIANT: source JSONL files are opened by the jsonl
 * module with O_RDONLY only. ingest.c never opens them directly — only
 * via jsonl_open. Verified by CROSS-03 source grep.
 */

#define _GNU_SOURCE

#include "ingest.h"
#include "actions.h"

#include "db.h"
#include "jsonl.h"

#include <ctype.h>
#include <errno.h>
#include <glob.h>
#include <jansson.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include <sqlite3.h>

/* === plausible-ts gate ==================================================
 *
 * Per the locked Phase 7 contract: timestamps are unix-ms, valid range is
 * 2020-01-01 inclusive to one day in the future (clock-skew tolerance). */

#define INGEST_TS_MIN_MS  1577836800000LL  /* 2020-01-01T00:00:00Z */

static long long now_unix_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long) tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static int ts_plausible(long long ts)
{
    if (ts < INGEST_TS_MIN_MS) return 0;
    long long max_ts = now_unix_ms() + 86400000LL; /* +1 day */
    return ts <= max_ts;
}

/* === sha256 hex shape gate ============================================== */

static int is_sha256_hex(const char *s)
{
    if (!s) return 0;
    size_t i;
    for (i = 0; i < 64; i++) {
        char c = s[i];
        if (!c) return 0;
        int ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) return 0;
    }
    return s[64] == '\0';
}

/* === prepared statement cache ===========================================
 *
 * Lazily prepared on first use; finalised by db_register_finalize_hook. */

struct ingest_stmt_cache {
    struct nocturne_db *owner;
    sqlite3_stmt *insert_play;
    sqlite3_stmt *upsert_like;
    sqlite3_stmt *upsert_pin;
    sqlite3_stmt *select_offset;
    sqlite3_stmt *upsert_offset;
    int finalize_hook_registered;
};

static struct ingest_stmt_cache g_cache;

static void finalize_all(void *ud)
{
    (void) ud;
    sqlite3_finalize(g_cache.insert_play);    g_cache.insert_play = NULL;
    sqlite3_finalize(g_cache.upsert_like);    g_cache.upsert_like = NULL;
    sqlite3_finalize(g_cache.upsert_pin);     g_cache.upsert_pin = NULL;
    sqlite3_finalize(g_cache.select_offset);  g_cache.select_offset = NULL;
    sqlite3_finalize(g_cache.upsert_offset);  g_cache.upsert_offset = NULL;
    g_cache.owner = NULL;
    g_cache.finalize_hook_registered = 0;
}

static int prep(struct nocturne_db *db, sqlite3_stmt **out, const char *sql)
{
    if (!*out) {
        sqlite3 *raw = db_handle(db);
        if (!raw) return -1;
        if (sqlite3_prepare_v2(raw, sql, -1, out, NULL) != SQLITE_OK) {
            fprintf(stderr, "ingest: prepare failed: %s\n", sqlite3_errmsg(raw));
            return -1;
        }
    } else {
        sqlite3_reset(*out);
        sqlite3_clear_bindings(*out);
    }
    return 0;
}

static int ensure_cache_for(struct nocturne_db *db)
{
    if (g_cache.owner != db) {
        if (g_cache.owner) finalize_all(NULL);
        g_cache.owner = db;
    }
    if (!g_cache.finalize_hook_registered) {
        if (db_register_finalize_hook(db, finalize_all, NULL) != 0) return -1;
        g_cache.finalize_hook_registered = 1;
    }
    return 0;
}

/* === SQL ================================================================ */

/* All placeholders are explicit ?N to avoid mixed-style ambiguity:
 *   ?1 = sha256, ?2 = ts (used twice — for both played_at strftime
 *   and the ts column), ?3 = played_ms, ?4 = is_skip, ?5 = src,
 *   ?6 = event_kind, ?7 = source_path. */
static const char *SQL_INSERT_PLAY =
    "INSERT INTO plays (sha256, played_at, played_ms, is_skip, src, "
    "                   ts, event_kind, source_path) "
    "VALUES (?1, "
    "        strftime('%Y-%m-%dT%H:%M:%fZ', ?2/1000.0, 'unixepoch'), "
    "        ?3, ?4, ?5, ?2, ?6, ?7)";

static const char *SQL_UPSERT_LIKE =
    "INSERT INTO likes (unit, id, liked, ts) VALUES (?, ?, ?, ?) "
    "ON CONFLICT(unit, id) DO UPDATE SET "
    "  liked = excluded.liked, "
    "  ts    = excluded.ts "
    "WHERE excluded.ts >= likes.ts";

static const char *SQL_UPSERT_PIN =
    "INSERT INTO pins (unit, id, pinned, ts, updated_at) VALUES (?, ?, ?, ?, ?) "
    "ON CONFLICT(unit, id) DO UPDATE SET "
    "  pinned     = excluded.pinned, "
    "  ts         = excluded.ts, "
    "  updated_at = excluded.updated_at "
    "WHERE excluded.ts >= pins.ts";

static const char *SQL_SELECT_OFFSET =
    "SELECT offset FROM ingest_offsets WHERE path = ?";

static const char *SQL_UPSERT_OFFSET =
    "INSERT INTO ingest_offsets (path, offset, last_event_ts, updated_at) "
    "VALUES (?, ?, ?, ?) "
    "ON CONFLICT(path) DO UPDATE SET "
    "  offset = excluded.offset, "
    "  last_event_ts = MAX(ingest_offsets.last_event_ts, excluded.last_event_ts), "
    "  updated_at = excluded.updated_at";

/* === helpers ============================================================ */

/* Look up persisted offset for `relpath`. Returns the offset (>= 0) or
 * 0 if no row exists (i.e. first time we've seen the file). */
static long long lookup_offset(struct nocturne_db *db, const char *relpath)
{
    if (prep(db, &g_cache.select_offset, SQL_SELECT_OFFSET) != 0) return 0;
    sqlite3_bind_text(g_cache.select_offset, 1, relpath, -1, SQLITE_TRANSIENT);
    long long off = 0;
    if (sqlite3_step(g_cache.select_offset) == SQLITE_ROW) {
        off = sqlite3_column_int64(g_cache.select_offset, 0);
    }
    return off;
}

/* Persist a new offset row. */
static int upsert_offset(struct nocturne_db *db, const char *relpath,
                         long long new_offset, long long last_event_ts)
{
    if (prep(db, &g_cache.upsert_offset, SQL_UPSERT_OFFSET) != 0) return -1;
    sqlite3_bind_text(g_cache.upsert_offset,  1, relpath, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(g_cache.upsert_offset, 2, new_offset);
    sqlite3_bind_int64(g_cache.upsert_offset, 3, last_event_ts);
    sqlite3_bind_int64(g_cache.upsert_offset, 4, now_unix_ms());
    int rc = sqlite3_step(g_cache.upsert_offset);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* Derive `src` field for plays from the source filename:
 *   "stats/phone-LK4F.jsonl" → "phone-LK4F"
 * Returns a heap string the caller frees, or NULL on bad input. */
static char *src_from_relpath(const char *relpath)
{
    if (!relpath) return NULL;
    /* Find last '/'. */
    const char *base = strrchr(relpath, '/');
    base = base ? base + 1 : relpath;
    /* Strip ".jsonl" suffix if present. */
    size_t n = strlen(base);
    const char *ext = ".jsonl";
    size_t elen = strlen(ext);
    if (n > elen && !strcmp(base + n - elen, ext)) n -= elen;
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, base, n);
    out[n] = '\0';
    return out;
}

/* Strip prefix `meta_dir/` from `abs` to get a relative path under
 * meta_dir. Returns a heap string the caller frees, or NULL if the path
 * doesn't share the prefix. */
static char *make_relpath(const char *meta_dir, const char *abs)
{
    if (!meta_dir || !abs) return NULL;
    size_t mlen = strlen(meta_dir);
    /* Strip trailing slash from meta_dir if present. */
    while (mlen > 0 && meta_dir[mlen - 1] == '/') mlen--;
    size_t alen = strlen(abs);
    if (alen <= mlen + 1) return NULL;
    if (strncmp(abs, meta_dir, mlen) != 0) return NULL;
    if (abs[mlen] != '/') return NULL;
    return strdup(abs + mlen + 1);
}

/* === handlers ===========================================================
 *
 * Each handler validates one parsed JSON line, then UPSERTs it. Returns 0
 * on success (line counted), 1 if the line was skipped (parse_error
 * counted). Returns -1 on fatal DB error. */

static int handle_play(json_t *root, const char *src_relpath,
                       struct nocturne_db *db, struct ingest_stats *stats,
                       int dry_run, long long *event_ts_out)
{
    json_t *jv     = json_object_get(root, "v");
    json_t *jts    = json_object_get(root, "ts");
    json_t *jkind  = json_object_get(root, "kind");
    json_t *jtrack = json_object_get(root, "track");
    json_t *jpms   = json_object_get(root, "played_ms");
    json_t *jdms   = json_object_get(root, "duration_ms");

    if (!json_is_integer(jv) || json_integer_value(jv) != 1) return 1;
    if (!json_is_integer(jts)) return 1;
    if (!json_is_string(jkind)) return 1;
    if (!json_is_string(jtrack)) return 1;
    if (!json_is_integer(jpms)) return 1;
    /* duration_ms is required by spec but tolerated as int or absent
     * during early Phase 6 dev. We only use it if present. */
    (void) jdms;

    long long ts = json_integer_value(jts);
    if (!ts_plausible(ts)) return 1;

    const char *kind = json_string_value(jkind);
    if (strcmp(kind, "play") != 0 && strcmp(kind, "skip") != 0) return 1;
    int is_skip = strcmp(kind, "skip") == 0;

    const char *track = json_string_value(jtrack);
    if (!is_sha256_hex(track)) return 1;

    long long played_ms = json_integer_value(jpms);
    if (played_ms < 0) return 1;

    if (event_ts_out && ts > *event_ts_out) *event_ts_out = ts;

    if (dry_run) {
        stats->plays_inserted++;
        return 0;
    }

    char *src = src_from_relpath(src_relpath);
    if (!src) return -1;

    if (prep(db, &g_cache.insert_play, SQL_INSERT_PLAY) != 0) {
        free(src);
        return -1;
    }
    /* Bind order matches SQL_INSERT_PLAY (with ?2 referenced twice for
     * the strftime computation): 1=sha256, 2=ts, 3=played_ms, 4=is_skip,
     * 5=src, 6=event_kind, 7=source_path. */
    sqlite3_bind_text (g_cache.insert_play, 1, track, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(g_cache.insert_play, 2, ts);
    sqlite3_bind_int64(g_cache.insert_play, 3, played_ms);
    sqlite3_bind_int  (g_cache.insert_play, 4, is_skip);
    sqlite3_bind_text (g_cache.insert_play, 5, src, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (g_cache.insert_play, 6, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (g_cache.insert_play, 7, src_relpath, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(g_cache.insert_play);
    free(src);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "ingest: insert play failed: %s\n",
                sqlite3_errmsg(db_handle(db)));
        return -1;
    }
    stats->plays_inserted++;
    return 0;
}

static int handle_like(json_t *root, struct nocturne_db *db,
                       struct ingest_stats *stats, int dry_run,
                       long long *event_ts_out)
{
    json_t *jv     = json_object_get(root, "v");
    json_t *jts    = json_object_get(root, "ts");
    json_t *junit  = json_object_get(root, "unit");
    json_t *jid    = json_object_get(root, "id");
    json_t *jliked = json_object_get(root, "liked");

    if (!json_is_integer(jv) || json_integer_value(jv) != 1) return 1;
    if (!json_is_integer(jts)) return 1;
    if (!json_is_string(junit)) return 1;
    if (!json_is_string(jid)) return 1;
    if (!json_is_boolean(jliked)) return 1;

    long long ts = json_integer_value(jts);
    if (!ts_plausible(ts)) return 1;

    const char *unit = json_string_value(junit);
    if (strcmp(unit, "track") != 0 && strcmp(unit, "album") != 0) return 1;

    const char *id = json_string_value(jid);
    if (!id || !*id) return 1;
    /* For tracks we require sha256 hex shape; album ids are opaque. */
    if (!strcmp(unit, "track") && !is_sha256_hex(id)) return 1;

    int liked = json_is_true(jliked) ? 1 : 0;

    if (event_ts_out && ts > *event_ts_out) *event_ts_out = ts;

    if (dry_run) {
        stats->likes_upserted++;
        return 0;
    }

    if (prep(db, &g_cache.upsert_like, SQL_UPSERT_LIKE) != 0) return -1;
    sqlite3_bind_text (g_cache.upsert_like, 1, unit, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (g_cache.upsert_like, 2, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int  (g_cache.upsert_like, 3, liked);
    sqlite3_bind_int64(g_cache.upsert_like, 4, ts);
    int rc = sqlite3_step(g_cache.upsert_like);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "ingest: upsert like failed: %s\n",
                sqlite3_errmsg(db_handle(db)));
        return -1;
    }
    stats->likes_upserted++;
    return 0;
}

/* handle_action: phone-emitted "unsync" / "delete" commands.
 *
 * Schema: {"v":1,"ts":<ms>,"unit":"track|album","id":"<sha|album_id>",
 *          "action":"unsync|delete"}
 *
 * Dispatched to actions.c (unsync_track / delete_track_everywhere /
 * unsync_album / delete_album_everywhere). Idempotent on the daemon side
 * (unsync uses ON CONFLICT, delete is no-op for a missing tracks row), so
 * re-ingesting the same line is safe.
 *
 * Per-line errors return 1 (skip line, continue file). DB errors return -1
 * (fail the file ingest).
 */
static int handle_action(json_t *root, struct nocturne_db *db,
                         struct ingest_stats *stats, int dry_run,
                         long long *event_ts_out)
{
    json_t *jv      = json_object_get(root, "v");
    json_t *jts     = json_object_get(root, "ts");
    json_t *junit   = json_object_get(root, "unit");
    json_t *jid     = json_object_get(root, "id");
    json_t *jaction = json_object_get(root, "action");

    if (!json_is_integer(jv) || json_integer_value(jv) != 1) return 1;
    if (!json_is_integer(jts)) return 1;
    if (!json_is_string(junit)) return 1;
    if (!json_is_string(jid)) return 1;
    if (!json_is_string(jaction)) return 1;

    long long ts = json_integer_value(jts);
    if (!ts_plausible(ts)) return 1;

    const char *unit = json_string_value(junit);
    if (strcmp(unit, "track") != 0 && strcmp(unit, "album") != 0) return 1;

    const char *id = json_string_value(jid);
    if (!id || !*id) return 1;
    if (!strcmp(unit, "track") && !is_sha256_hex(id)) return 1;

    const char *action = json_string_value(jaction);
    int is_delete;
    if (!strcmp(action, "unsync")) is_delete = 0;
    else if (!strcmp(action, "delete")) is_delete = 1;
    else return 1;

    if (event_ts_out && ts > *event_ts_out) *event_ts_out = ts;
    if (dry_run) return 0;

    /* LWW gate for track-unit unsync: skip if there's a newer pin event for
     * this track. Without this, the per-cycle ingest order (pins → actions)
     * lets a stale unsync action overwrite a fresher pin: handle_pin sets
     * pinned=1 and deletes the override during pin processing, then this
     * handler runs unsync_track which flips pinned=0 and re-adds the
     * override — even though the user's pin is more recent than the
     * unsync. Track-only check; album-unit actions always apply (they're
     * a no-op in the current daemon anyway per album_track_shas). */
    if (!is_delete && !strcmp(unit, "track")) {
        sqlite3_stmt *q = NULL;
        if (sqlite3_prepare_v2(db_handle(db),
                "SELECT ts FROM pins WHERE unit='track' AND id=?",
                -1, &q, NULL) == SQLITE_OK) {
            sqlite3_bind_text(q, 1, id, -1, SQLITE_TRANSIENT);
            long long pin_ts = 0;
            if (sqlite3_step(q) == SQLITE_ROW) {
                pin_ts = sqlite3_column_int64(q, 0);
            }
            sqlite3_finalize(q);
            if (pin_ts > ts) {
                fprintf(stderr,
                    "ingest: unsync action for %.16s... skipped — "
                    "pin event is newer (pin_ts=%lld > action_ts=%lld)\n",
                    id, pin_ts, ts);
                stats->pins_upserted++;
                return 0;
            }
        }
    }

    struct action_stats astats = {0};
    int rc;
    if (!strcmp(unit, "album")) {
        rc = is_delete ? delete_album_everywhere(db, id, &astats)
                       : unsync_album(db, id, &astats);
    } else {
        rc = is_delete ? delete_track_everywhere(db, id, &astats)
                       : unsync_track(db, id, &astats);
    }
    if (rc < 0) return -1;
    /* No dedicated stats counter; bucket under pins_upserted for visibility
     * since phone-side write paths share that fan-in. */
    stats->pins_upserted++;
    return 0;
}

static int handle_pin(json_t *root, struct nocturne_db *db,
                      struct ingest_stats *stats, int dry_run,
                      long long *event_ts_out)
{
    json_t *jv      = json_object_get(root, "v");
    json_t *jts     = json_object_get(root, "ts");
    json_t *junit   = json_object_get(root, "unit");
    json_t *jid     = json_object_get(root, "id");
    json_t *jpinned = json_object_get(root, "pinned");

    if (!json_is_integer(jv) || json_integer_value(jv) != 1) return 1;
    if (!json_is_integer(jts)) return 1;
    if (!json_is_string(junit)) return 1;
    if (!json_is_string(jid)) return 1;
    if (!json_is_boolean(jpinned)) return 1;

    long long ts = json_integer_value(jts);
    if (!ts_plausible(ts)) return 1;

    const char *unit = json_string_value(junit);
    if (strcmp(unit, "track") != 0 && strcmp(unit, "album") != 0) return 1;

    const char *id = json_string_value(jid);
    if (!id || !*id) return 1;
    if (!strcmp(unit, "track") && !is_sha256_hex(id)) return 1;

    int pinned = json_is_true(jpinned) ? 1 : 0;

    if (event_ts_out && ts > *event_ts_out) *event_ts_out = ts;

    if (dry_run) {
        stats->pins_upserted++;
        return 0;
    }

    /* Legacy `updated_at` column is ISO-8601; render from ts. */
    char iso[40];
    {
        time_t secs = (time_t) (ts / 1000);
        long long ms = ts % 1000;
        struct tm tm;
        gmtime_r(&secs, &tm);
        size_t k = strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S", &tm);
        snprintf(iso + k, sizeof(iso) - k, ".%03lldZ", ms);
    }

    if (prep(db, &g_cache.upsert_pin, SQL_UPSERT_PIN) != 0) return -1;
    sqlite3_bind_text (g_cache.upsert_pin, 1, unit, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (g_cache.upsert_pin, 2, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int  (g_cache.upsert_pin, 3, pinned);
    sqlite3_bind_int64(g_cache.upsert_pin, 4, ts);
    sqlite3_bind_text (g_cache.upsert_pin, 5, iso, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(g_cache.upsert_pin);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "ingest: upsert pin failed: %s\n",
                sqlite3_errmsg(db_handle(db)));
        return -1;
    }
    stats->pins_upserted++;

    /* Re-pinning a track clears any prior unsync override. Without this,
     * a track that was once unsync'd stays excluded from the manifest
     * (resolve_cmd's WHERE NOT EXISTS filter against unsync_overrides)
     * even after the user explicitly pins it again — the silent
     * "I pinned it but it won't download" symptom. Track-only: album-
     * unit pins/unsyncs aren't supported by the override table anyway. */
    if (pinned && !strcmp(unit, "track")) {
        sqlite3_stmt *del = NULL;
        if (sqlite3_prepare_v2(db_handle(db),
                "DELETE FROM unsync_overrides WHERE sha256=?",
                -1, &del, NULL) == SQLITE_OK) {
            sqlite3_bind_text(del, 1, id, -1, SQLITE_TRANSIENT);
            sqlite3_step(del);
            sqlite3_finalize(del);
        }
    }
    return 0;
}

/* === per-file ingest ====================================================
 *
 * Process one source file end-to-end:
 *   1. Look up persisted offset (relative to meta_dir).
 *   2. jsonl_open at that offset.
 *   3. Loop: read line, jansson-parse, dispatch to handler.
 *   4. Persist new offset if it advanced.
 *
 * `kind` is one of "stats", "likes", "pins" — selects the handler.
 * Returns 0 on per-file success, -1 on fatal (DB error / OOM).
 * Per-line errors (parse / validation / oversize) are logged + counted
 * but never fail the file. */

enum file_kind { KIND_STATS, KIND_LIKES, KIND_PINS, KIND_ACTIONS };

static int ingest_one_file(struct nocturne_db *db, const char *meta_dir,
                           const char *abs_path, enum file_kind kind,
                           struct ingest_stats *stats, int dry_run)
{
    char *relpath = make_relpath(meta_dir, abs_path);
    if (!relpath) {
        /* Unexpected — glob returned a path not under meta_dir. */
        fprintf(stderr, "ingest: glob returned out-of-tree path: %s\n", abs_path);
        return 0;
    }

    long long old_offset = lookup_offset(db, relpath);

    /* RACE-05: if the file was truncated (phone reinstall / Syncthing
     * conflict resolution), the persisted offset overshoots the new
     * file size.  lseek past EOF succeeds silently, so every subsequent
     * read returns 0 bytes and the offset is never advanced — new events
     * are permanently invisible.  LWW semantics make a full re-play safe:
     * older-or-equal-ts rows are discarded by the UPSERT. */
    {
        struct stat _st;
        if (stat(abs_path, &_st) == 0 && old_offset > (long long)_st.st_size) {
            fprintf(stderr, "ingest: %s: offset %lld overshoots size %lld; resetting\n",
                    relpath, old_offset, (long long)_st.st_size);
            old_offset = 0;
        }
    }

    struct jsonl_reader *jr = jsonl_open(abs_path, (off_t) old_offset);
    if (!jr) {
        fprintf(stderr, "ingest: open %s: %s\n", abs_path, strerror(errno));
        free(relpath);
        return 0; /* per-file recoverable */
    }

    /* Wrap per-file writes in a transaction so partial-file failure
     * rolls back the offset advance + any writes from this file. */
    sqlite3 *raw = db_handle(db);
    if (!dry_run) {
        char *err = NULL;
        if (sqlite3_exec(raw, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
            fprintf(stderr, "ingest: BEGIN failed: %s\n",
                    err ? err : sqlite3_errmsg(raw));
            sqlite3_free(err);
            jsonl_close(jr);
            free(relpath);
            return -1;
        }
    }

    long long max_event_ts = 0;
    int rc;
    int file_failed = 0;
    int linenum = 0;
    for (;;) {
        const char *line = NULL;
        size_t llen = 0;
        rc = jsonl_read_line(jr, &line, &llen);
        if (rc == 0) break; /* EOF or trailing partial */
        if (rc < 0) {
            if (errno == EMSGSIZE) {
                stats->lines_skipped_oversize++;
                fprintf(stderr, "ingest: %s: oversize line skipped\n", relpath);
                continue;
            }
            fprintf(stderr, "ingest: %s: read error: %s\n",
                    relpath, strerror(errno));
            file_failed = 1;
            break;
        }
        linenum++;

        /* Empty line is allowed (per locked spec): skip silently. */
        if (llen == 0) continue;

        json_error_t jerr;
        json_t *root = json_loadb(line, llen, 0, &jerr);
        if (!root) {
            stats->lines_skipped_parse_error++;
            fprintf(stderr, "ingest: %s:%d: parse error: %s\n",
                    relpath, linenum, jerr.text);
            continue;
        }
        if (!json_is_object(root)) {
            stats->lines_skipped_parse_error++;
            json_decref(root);
            continue;
        }

        int hr;
        switch (kind) {
        case KIND_STATS:   hr = handle_play(root, relpath, db, stats, dry_run, &max_event_ts); break;
        case KIND_LIKES:   hr = handle_like(root, db, stats, dry_run, &max_event_ts); break;
        case KIND_PINS:    hr = handle_pin(root, db, stats, dry_run, &max_event_ts); break;
        case KIND_ACTIONS: hr = handle_action(root, db, stats, dry_run, &max_event_ts); break;
        default: hr = 1; break;
        }
        json_decref(root);

        if (hr == 1) {
            stats->lines_skipped_parse_error++;
            fprintf(stderr, "ingest: %s:%d: validation failed; line skipped\n",
                    relpath, linenum);
        } else if (hr < 0) {
            file_failed = 1;
            break;
        }
    }

    long long new_offset = (long long) jsonl_offset(jr);
    jsonl_close(jr);

    if (!file_failed && new_offset > old_offset && !dry_run) {
        if (upsert_offset(db, relpath, new_offset, max_event_ts) != 0) {
            file_failed = 1;
        } else {
            stats->offsets_advanced++;
        }
    } else if (!file_failed && new_offset > old_offset && dry_run) {
        /* In dry-run we still report the advance even though we didn't
         * write the offset row, so callers see the would-be progress. */
        stats->offsets_advanced++;
    }

    if (!dry_run) {
        if (file_failed) {
            sqlite3_exec(raw, "ROLLBACK", NULL, NULL, NULL);
        } else {
            char *err = NULL;
            if (sqlite3_exec(raw, "COMMIT", NULL, NULL, &err) != SQLITE_OK) {
                fprintf(stderr, "ingest: COMMIT failed: %s\n",
                        err ? err : sqlite3_errmsg(raw));
                sqlite3_free(err);
                file_failed = 1;
            }
        }
    }

    free(relpath);
    return file_failed ? -1 : 0;
}

/* === glob-based file discovery ========================================== */

static int run_glob(struct nocturne_db *db, const char *meta_dir,
                    const char *pattern, enum file_kind kind,
                    struct ingest_stats *stats, int dry_run)
{
    /* Build full pattern: "<meta_dir>/<pattern>". */
    size_t mlen = strlen(meta_dir);
    while (mlen > 0 && meta_dir[mlen - 1] == '/') mlen--;
    size_t plen = strlen(pattern);
    char *full = malloc(mlen + 1 + plen + 1);
    if (!full) return -1;
    memcpy(full, meta_dir, mlen);
    full[mlen] = '/';
    memcpy(full + mlen + 1, pattern, plen);
    full[mlen + 1 + plen] = '\0';

    glob_t gl;
    int gr = glob(full, 0, NULL, &gl);
    free(full);
    if (gr == GLOB_NOMATCH) {
        return 0;
    }
    if (gr != 0) {
        fprintf(stderr, "ingest: glob('%s') failed (code=%d)\n", pattern, gr);
        return 0; /* recoverable: skip this glob, try the next kind */
    }

    int rc = 0;
    for (size_t i = 0; i < gl.gl_pathc; i++) {
        stats->files_seen++;
        if (ingest_one_file(db, meta_dir, gl.gl_pathv[i], kind, stats, dry_run) != 0) {
            rc = -1;
            /* Don't break — we still want to try the rest of the files. */
        }
    }
    globfree(&gl);
    return rc;
}

/* === public entry ======================================================= */

int ingest_run(struct nocturne_db *db, const char *meta_dir,
               struct ingest_stats *stats_out, int dry_run)
{
    if (!db) return -1;
    if (!meta_dir || !*meta_dir) {
        if (stats_out) memset(stats_out, 0, sizeof(*stats_out));
        return 0;
    }

    struct ingest_stats local_stats = {0};
    struct ingest_stats *stats = stats_out ? stats_out : &local_stats;
    memset(stats, 0, sizeof(*stats));

    if (ensure_cache_for(db) != 0) return -1;

    /* Three glob patterns per locked decisions (PLAN.md):
     *   stats/phone-*.jsonl     (under stats/)
     *   likes-phone-*.jsonl     (top-level)
     *   pins-phone-*.jsonl      (top-level) */
    int rc = 0;
    if (run_glob(db, meta_dir, "stats/phone-*.jsonl",   KIND_STATS,   stats, dry_run) < 0) rc = -1;
    if (run_glob(db, meta_dir, "likes-phone-*.jsonl",   KIND_LIKES,   stats, dry_run) < 0) rc = -1;
    if (run_glob(db, meta_dir, "pins-phone-*.jsonl",    KIND_PINS,    stats, dry_run) < 0) rc = -1;
    if (run_glob(db, meta_dir, "actions-phone-*.jsonl", KIND_ACTIONS, stats, dry_run) < 0) rc = -1;

    return rc;
}
