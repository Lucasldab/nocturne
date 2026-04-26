/*
 * test_ingest.c — synthetic-fixture tests for the Phase 7 ingest engine.
 *
 * Each test case:
 *   1. mkdtemp a fresh meta_dir + DB.
 *   2. Insert synthetic track rows (so the plays foreign-key resolves).
 *   3. Write JSONL fixtures via fopen + fputs + fclose.
 *   4. Call ingest_run, assert stats counts + DB row state.
 *   5. Cleanup.
 *
 * Behaviour coverage (all bullets from Plan 07-02 Task 3):
 *   1. Empty meta_dir → ingest_run 0; stats all zero.
 *   2. Single play event → one plays row.
 *   3. Multi-play across two source files → src column reflects each device.
 *   4. Like at ts=100 then unlike at ts=200 SAME file → likes.liked=0, ts=200.
 *   5. Like at ts=100 file A, unlike at ts=200 file B (cross-file order).
 *   6. Like at ts=200 then like-again at ts=100 (out-of-order replay) →
 *      likes row stays at ts=200 (older event ignored).
 *   7. Pin album at ts=100, unpin album at ts=200 → pins.pinned=0, ts=200.
 *   8. Pin track + pin album for same id are independent rows (different unit).
 *   9. Mixed sources (stats + likes + pins) → all three tables updated.
 *  10. Idempotent re-run: second ingest reports zero deltas.
 *  11. Partial-line tolerance: file ends mid-line; offset stops at last \n.
 *      Append the rest, re-run, completes the line.
 *  12. Mid-stream restart: kill mid-file, second run resumes from offset.
 *  13. Out-of-range ts (year 1970 / 9999) → line skipped, parse_error++.
 *  14. Malformed JSON → line skipped, parse_error++.
 *  15. Oversize line (>64KB no \n) → oversize_lines++; surrounding valid
 *      lines still processed.
 *  16. dry-run: stats counted, DB row counts unchanged.
 *  17. Album-level like (unit='album') → row in likes; resolver SQL_LOVED
 *      ignores it (only unit='track' joined).
 *  18. File at unexpected path (e.g. stats/desktop-X.jsonl) → glob doesn't
 *      match, file ignored.
 *  19. Unknown event kind ("scrub") → line skipped, parse_error++.
 *  20. v != 1 → line skipped (forward-compat).
 *  21. track field not 64-char hex → line skipped.
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
#include "ingest.h"
#include "runner.h"

/* === helpers === */

/* The same plausible sha256 used across tests. */
/* All three are exactly 64 lowercase hex chars (sha256 shape). */
static const char *SHA_A = "9a3f4e1b2c5d6e7f8091a2b3c4d5e6f70819f0e1d2c3b4a5968778695a4b3c2d";
static const char *SHA_B = "b1c2d3e4f50617283940a1b2c3d4e5f60718293a4b5c6d7e8f900112233445dd";
static const char *SHA_C = "f1e2d3c4b5a6978889706151423324453647586975a4b3c2d1e0f00112233445";

static int rm_rf(const char *path)
{
    if (!path || strncmp(path, "/tmp/", 5) != 0) return -1;
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf -- %s", path);
    return system(cmd);
}

static char *make_tmp_meta(void)
{
    char tmpl[256];
    snprintf(tmpl, sizeof(tmpl), "/tmp/nocturne-ingest-meta-XXXXXX");
    char *p = strdup(tmpl);
    if (!mkdtemp(p)) { free(p); return NULL; }
    /* Ensure stats/ subdir exists in case the test writes a stats file. */
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s/stats", p);
    mkdir(buf, 0755);
    return p;
}

static char *make_tmp_dbpath(void)
{
    char tmpl[256];
    snprintf(tmpl, sizeof(tmpl), "/tmp/nocturne-ingest-db-XXXXXX");
    char *p = strdup(tmpl);
    int fd = mkstemp(p);
    if (fd < 0) { free(p); return NULL; }
    close(fd);
    unlink(p); /* db_open creates fresh */
    return p;
}

static int exec_sql(struct nocturne_db *db, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(db_handle(db), sql, NULL, NULL, &err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK ? 0 : -1;
}

static long long count_rows(struct nocturne_db *db, const char *table)
{
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s", table);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db_handle(db), sql, -1, &st, NULL) != SQLITE_OK) return -1;
    long long n = -1;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

static long long select_int(struct nocturne_db *db, const char *sql)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db_handle(db), sql, -1, &st, NULL) != SQLITE_OK) return -1;
    long long n = -1;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

static void insert_track(struct nocturne_db *db, const char *sha)
{
    /* `tracks.path` is UNIQUE, so we vary the path by sha prefix. */
    char sql[1024];
    snprintf(sql, sizeof(sql),
        "INSERT INTO tracks (sha256, path, mtime_ns, size_bytes, "
        "tags_status, date_added, last_seen_at) VALUES "
        "('%s', '/tmp/%.16s.mp3', 0, 1024, 'ok', "
        "'2026-04-26T00:00:00Z', '2026-04-26T00:00:00Z')", sha, sha);
    exec_sql(db, sql);
}

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(content, f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

static void append_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "a");
    if (!f) return;
    fputs(content, f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

/* === test cases ============================================================ */

static void test_empty_meta(void)
{
    char *meta = make_tmp_meta();
    char *dbp  = make_tmp_dbpath();
    struct nocturne_db *db = db_open(dbp, NULL, NULL);
    struct ingest_stats st = {0};
    int rc = ingest_run(db, meta, &st, 0);
    expect(rc == 0, "empty: ingest_run returns 0");
    expect(st.files_seen == 0, "empty: files_seen == 0");
    expect(st.plays_inserted == 0, "empty: plays_inserted == 0");
    expect(st.likes_upserted == 0, "empty: likes_upserted == 0");
    expect(st.pins_upserted == 0, "empty: pins_upserted == 0");
    expect(count_rows(db, "ingest_offsets") == 0, "empty: no offset rows");
    db_close(db); unlink(dbp); free(dbp); rm_rf(meta); free(meta);
}

static void test_single_play(void)
{
    char *meta = make_tmp_meta();
    char *dbp  = make_tmp_dbpath();
    struct nocturne_db *db = db_open(dbp, NULL, NULL);
    insert_track(db, SHA_A);

    char path[512];
    snprintf(path, sizeof(path), "%s/stats/phone-T1.jsonl", meta);
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"v\":1,\"ts\":1745678910123,\"kind\":\"play\","
        "\"track\":\"%s\",\"played_ms\":231400,\"duration_ms\":237000}\n", SHA_A);
    write_file(path, buf);

    struct ingest_stats st = {0};
    int rc = ingest_run(db, meta, &st, 0);
    expect(rc == 0, "single-play: ingest_run returns 0");
    expect(st.plays_inserted == 1, "single-play: plays_inserted == 1");
    expect(count_rows(db, "plays") == 1, "single-play: 1 row in plays");

    /* Verify resolver-compatibility columns populated. */
    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT COUNT(*) FROM plays WHERE sha256='%s' AND src='phone-T1' "
        "AND is_skip=0 AND ts=1745678910123 AND event_kind='play'", SHA_A);
    expect(select_int(db, sql) == 1, "single-play: legacy + new columns populated");

    db_close(db); unlink(dbp); free(dbp); rm_rf(meta); free(meta);
}

static void test_multi_play_two_files(void)
{
    char *meta = make_tmp_meta();
    char *dbp  = make_tmp_dbpath();
    struct nocturne_db *db = db_open(dbp, NULL, NULL);
    insert_track(db, SHA_A);

    char p1[512], p2[512], buf[512];
    snprintf(p1, sizeof(p1), "%s/stats/phone-AAAA.jsonl", meta);
    snprintf(p2, sizeof(p2), "%s/stats/phone-BBBB.jsonl", meta);

    snprintf(buf, sizeof(buf),
        "{\"v\":1,\"ts\":1745678910123,\"kind\":\"play\","
        "\"track\":\"%s\",\"played_ms\":231400,\"duration_ms\":237000}\n", SHA_A);
    write_file(p1, buf);
    snprintf(buf, sizeof(buf),
        "{\"v\":1,\"ts\":1745678910999,\"kind\":\"play\","
        "\"track\":\"%s\",\"played_ms\":120000,\"duration_ms\":180000}\n", SHA_A);
    write_file(p2, buf);

    struct ingest_stats st = {0};
    ingest_run(db, meta, &st, 0);
    expect(st.plays_inserted == 2, "multi-play: 2 plays_inserted");
    expect(select_int(db, "SELECT COUNT(DISTINCT src) FROM plays") == 2,
           "multi-play: two distinct src values");
    expect(select_int(db,
        "SELECT COUNT(*) FROM plays WHERE src='phone-AAAA'") == 1,
        "multi-play: src 'phone-AAAA' has 1 row");
    expect(select_int(db,
        "SELECT COUNT(*) FROM plays WHERE src='phone-BBBB'") == 1,
        "multi-play: src 'phone-BBBB' has 1 row");

    db_close(db); unlink(dbp); free(dbp); rm_rf(meta); free(meta);
}

static void test_lww_like_unlike_same_file(void)
{
    char *meta = make_tmp_meta();
    char *dbp  = make_tmp_dbpath();
    struct nocturne_db *db = db_open(dbp, NULL, NULL);

    char path[512], buf[1024];
    snprintf(path, sizeof(path), "%s/likes-phone-T1.jsonl", meta);
    snprintf(buf, sizeof(buf),
        "{\"v\":1,\"ts\":1745678910100,\"unit\":\"track\",\"id\":\"%s\",\"liked\":true}\n"
        "{\"v\":1,\"ts\":1745678910200,\"unit\":\"track\",\"id\":\"%s\",\"liked\":false}\n",
        SHA_A, SHA_A);
    write_file(path, buf);

    struct ingest_stats st = {0};
    ingest_run(db, meta, &st, 0);
    expect(st.likes_upserted == 2, "lww-same-file: 2 attempts counted");
    expect(count_rows(db, "likes") == 1, "lww-same-file: single likes row");

    char sql[256];
    snprintf(sql, sizeof(sql),
        "SELECT liked FROM likes WHERE unit='track' AND id='%s'", SHA_A);
    expect(select_int(db, sql) == 0, "lww-same-file: unlike (later ts) wins");
    snprintf(sql, sizeof(sql),
        "SELECT ts FROM likes WHERE unit='track' AND id='%s'", SHA_A);
    expect(select_int(db, sql) == 1745678910200LL,
           "lww-same-file: ts == 200 (the later event)");

    db_close(db); unlink(dbp); free(dbp); rm_rf(meta); free(meta);
}

static void test_lww_cross_file(void)
{
    char *meta = make_tmp_meta();
    char *dbp  = make_tmp_dbpath();
    struct nocturne_db *db = db_open(dbp, NULL, NULL);

    /* Two files for the same logical phone — atypical but the LWW must
     * still hold. Use distinct deviceids so each is a valid file name. */
    char p1[512], p2[512], buf[1024];
    snprintf(p1, sizeof(p1), "%s/likes-phone-AAAA.jsonl", meta);
    snprintf(p2, sizeof(p2), "%s/likes-phone-BBBB.jsonl", meta);
    snprintf(buf, sizeof(buf),
        "{\"v\":1,\"ts\":1745678910100,\"unit\":\"track\",\"id\":\"%s\",\"liked\":true}\n",
        SHA_A);
    write_file(p1, buf);
    snprintf(buf, sizeof(buf),
        "{\"v\":1,\"ts\":1745678910200,\"unit\":\"track\",\"id\":\"%s\",\"liked\":false}\n",
        SHA_A);
    write_file(p2, buf);

    struct ingest_stats st = {0};
    ingest_run(db, meta, &st, 0);
    char sql[256];
    snprintf(sql, sizeof(sql),
        "SELECT liked FROM likes WHERE unit='track' AND id='%s'", SHA_A);
    expect(select_int(db, sql) == 0, "lww-cross-file: unlike wins regardless of glob order");

    db_close(db); unlink(dbp); free(dbp); rm_rf(meta); free(meta);
}

static void test_lww_out_of_order(void)
{
    /* In one file: like at ts=200 first, then like-again at ts=100 (the
     * older event arrives second). The newer one (200) must remain. */
    char *meta = make_tmp_meta();
    char *dbp  = make_tmp_dbpath();
    struct nocturne_db *db = db_open(dbp, NULL, NULL);
    char path[512], buf[1024];
    snprintf(path, sizeof(path), "%s/likes-phone-T1.jsonl", meta);
    snprintf(buf, sizeof(buf),
        "{\"v\":1,\"ts\":1745678910200,\"unit\":\"track\",\"id\":\"%s\",\"liked\":true}\n"
        "{\"v\":1,\"ts\":1745678910100,\"unit\":\"track\",\"id\":\"%s\",\"liked\":false}\n",
        SHA_A, SHA_A);
    write_file(path, buf);

    struct ingest_stats st = {0};
    ingest_run(db, meta, &st, 0);
    char sql[256];
    snprintf(sql, sizeof(sql),
        "SELECT liked FROM likes WHERE unit='track' AND id='%s'", SHA_A);
    expect(select_int(db, sql) == 1,
           "lww-out-of-order: older event ignored, ts=200 like wins");
    snprintf(sql, sizeof(sql),
        "SELECT ts FROM likes WHERE unit='track' AND id='%s'", SHA_A);
    expect(select_int(db, sql) == 1745678910200LL,
           "lww-out-of-order: ts stays at 200");

    db_close(db); unlink(dbp); free(dbp); rm_rf(meta); free(meta);
}

static void test_pin_unpin_album(void)
{
    char *meta = make_tmp_meta();
    char *dbp  = make_tmp_dbpath();
    struct nocturne_db *db = db_open(dbp, NULL, NULL);
    char path[512], buf[1024];
    snprintf(path, sizeof(path), "%s/pins-phone-T1.jsonl", meta);
    snprintf(buf, sizeof(buf),
        "{\"v\":1,\"ts\":1745678910100,\"unit\":\"album\",\"id\":\"alb_xyz\",\"pinned\":true}\n"
        "{\"v\":1,\"ts\":1745678910200,\"unit\":\"album\",\"id\":\"alb_xyz\",\"pinned\":false}\n");
    write_file(path, buf);

    struct ingest_stats st = {0};
    ingest_run(db, meta, &st, 0);
    expect(select_int(db,
        "SELECT pinned FROM pins WHERE unit='album' AND id='alb_xyz'") == 0,
        "pin-album: unpin (later ts) wins");

    db_close(db); unlink(dbp); free(dbp); rm_rf(meta); free(meta);
}

static void test_pin_track_and_album_same_id(void)
{
    /* The "same id" only collides if unit also collides. (track,SHA_A)
     * and (album,SHA_A) are distinct rows. We use SHA_A for the track
     * id and use a real-looking opaque id for the album. */
    char *meta = make_tmp_meta();
    char *dbp  = make_tmp_dbpath();
    struct nocturne_db *db = db_open(dbp, NULL, NULL);
    char path[512], buf[1024];
    snprintf(path, sizeof(path), "%s/pins-phone-T1.jsonl", meta);
    snprintf(buf, sizeof(buf),
        "{\"v\":1,\"ts\":1745678910100,\"unit\":\"track\",\"id\":\"%s\",\"pinned\":true}\n"
        "{\"v\":1,\"ts\":1745678910100,\"unit\":\"album\",\"id\":\"%s\",\"pinned\":true}\n",
        SHA_A, SHA_A);
    write_file(path, buf);

    struct ingest_stats st = {0};
    ingest_run(db, meta, &st, 0);
    expect(count_rows(db, "pins") == 2, "pin-track-album: two independent rows");

    db_close(db); unlink(dbp); free(dbp); rm_rf(meta); free(meta);
}

static void test_mixed_sources(void)
{
    char *meta = make_tmp_meta();
    char *dbp  = make_tmp_dbpath();
    struct nocturne_db *db = db_open(dbp, NULL, NULL);
    insert_track(db, SHA_A);

    char p[512], buf[1024];
    snprintf(p, sizeof(p), "%s/stats/phone-T1.jsonl", meta);
    snprintf(buf, sizeof(buf),
        "{\"v\":1,\"ts\":1745678910100,\"kind\":\"play\","
        "\"track\":\"%s\",\"played_ms\":1000,\"duration_ms\":1000}\n", SHA_A);
    write_file(p, buf);
    snprintf(p, sizeof(p), "%s/likes-phone-T1.jsonl", meta);
    snprintf(buf, sizeof(buf),
        "{\"v\":1,\"ts\":1745678910100,\"unit\":\"track\",\"id\":\"%s\",\"liked\":true}\n", SHA_A);
    write_file(p, buf);
    snprintf(p, sizeof(p), "%s/pins-phone-T1.jsonl", meta);
    snprintf(buf, sizeof(buf),
        "{\"v\":1,\"ts\":1745678910100,\"unit\":\"track\",\"id\":\"%s\",\"pinned\":true}\n", SHA_A);
    write_file(p, buf);

    struct ingest_stats st = {0};
    ingest_run(db, meta, &st, 0);
    expect(st.files_seen == 3, "mixed: 3 files seen");
    expect(st.plays_inserted == 1, "mixed: 1 play");
    expect(st.likes_upserted == 1, "mixed: 1 like");
    expect(st.pins_upserted == 1, "mixed: 1 pin");
    expect(st.offsets_advanced == 3, "mixed: 3 offsets advanced");
    expect(count_rows(db, "ingest_offsets") == 3, "mixed: 3 ingest_offsets rows");

    db_close(db); unlink(dbp); free(dbp); rm_rf(meta); free(meta);
}

static void test_idempotent_rerun(void)
{
    char *meta = make_tmp_meta();
    char *dbp  = make_tmp_dbpath();
    struct nocturne_db *db = db_open(dbp, NULL, NULL);
    insert_track(db, SHA_A);

    char path[512], buf[1024];
    snprintf(path, sizeof(path), "%s/stats/phone-T1.jsonl", meta);
    snprintf(buf, sizeof(buf),
        "{\"v\":1,\"ts\":1745678910100,\"kind\":\"play\","
        "\"track\":\"%s\",\"played_ms\":1000,\"duration_ms\":1000}\n"
        "{\"v\":1,\"ts\":1745678910200,\"kind\":\"play\","
        "\"track\":\"%s\",\"played_ms\":1000,\"duration_ms\":1000}\n",
        SHA_A, SHA_A);
    write_file(path, buf);

    struct ingest_stats st1 = {0};
    ingest_run(db, meta, &st1, 0);
    expect(st1.plays_inserted == 2, "idempotent: first run inserts 2");

    struct ingest_stats st2 = {0};
    ingest_run(db, meta, &st2, 0);
    expect(st2.plays_inserted == 0, "idempotent: second run inserts 0");
    expect(st2.likes_upserted == 0, "idempotent: second run no likes");
    expect(st2.pins_upserted == 0, "idempotent: second run no pins");
    expect(st2.offsets_advanced == 0, "idempotent: second run no offset advance");
    expect(count_rows(db, "plays") == 2, "idempotent: still 2 plays rows");

    db_close(db); unlink(dbp); free(dbp); rm_rf(meta); free(meta);
}

static void test_partial_line_then_full(void)
{
    char *meta = make_tmp_meta();
    char *dbp  = make_tmp_dbpath();
    struct nocturne_db *db = db_open(dbp, NULL, NULL);
    insert_track(db, SHA_A);

    char path[512], buf[1024];
    snprintf(path, sizeof(path), "%s/stats/phone-T1.jsonl", meta);
    /* First write: one complete line + a partial line (no trailing \n). */
    snprintf(buf, sizeof(buf),
        "{\"v\":1,\"ts\":1745678910100,\"kind\":\"play\","
        "\"track\":\"%s\",\"played_ms\":1000,\"duration_ms\":1000}\n"
        "{\"v\":1,\"ts\":1745678910200,\"kind\":\"play\","
        "\"track\":\"%s\",\"played_ms\":1500", SHA_A, SHA_A);
    write_file(path, buf);

    struct ingest_stats st = {0};
    ingest_run(db, meta, &st, 0);
    expect(st.plays_inserted == 1, "partial: only complete line ingested");
    expect(count_rows(db, "plays") == 1, "partial: 1 plays row");

    /* Append the rest of the partial line + a third complete one. */
    snprintf(buf, sizeof(buf),
        ",\"duration_ms\":2000}\n"
        "{\"v\":1,\"ts\":1745678910300,\"kind\":\"play\","
        "\"track\":\"%s\",\"played_ms\":2000,\"duration_ms\":3000}\n", SHA_A);
    append_file(path, buf);

    struct ingest_stats st2 = {0};
    ingest_run(db, meta, &st2, 0);
    expect(st2.plays_inserted == 2, "partial-then-full: now 2 more plays");
    expect(count_rows(db, "plays") == 3, "partial-then-full: 3 total plays");

    db_close(db); unlink(dbp); free(dbp); rm_rf(meta); free(meta);
}

static void test_out_of_range_ts(void)
{
    char *meta = make_tmp_meta();
    char *dbp  = make_tmp_dbpath();
    struct nocturne_db *db = db_open(dbp, NULL, NULL);
    insert_track(db, SHA_A);
    char path[512], buf[1024];
    snprintf(path, sizeof(path), "%s/stats/phone-T1.jsonl", meta);
    snprintf(buf, sizeof(buf),
        /* ts=0 is way before 2020 */
        "{\"v\":1,\"ts\":0,\"kind\":\"play\","
        "\"track\":\"%s\",\"played_ms\":100,\"duration_ms\":100}\n"
        /* ts=99999999999999 is way after now+1d */
        "{\"v\":1,\"ts\":99999999999999,\"kind\":\"play\","
        "\"track\":\"%s\",\"played_ms\":100,\"duration_ms\":100}\n",
        SHA_A, SHA_A);
    write_file(path, buf);

    struct ingest_stats st = {0};
    ingest_run(db, meta, &st, 0);
    expect(st.plays_inserted == 0, "ts-range: both lines rejected");
    expect(st.lines_skipped_parse_error == 2, "ts-range: 2 parse_errors");

    db_close(db); unlink(dbp); free(dbp); rm_rf(meta); free(meta);
}

static void test_malformed_json(void)
{
    char *meta = make_tmp_meta();
    char *dbp  = make_tmp_dbpath();
    struct nocturne_db *db = db_open(dbp, NULL, NULL);
    insert_track(db, SHA_A);
    char path[512], buf[1024];
    snprintf(path, sizeof(path), "%s/stats/phone-T1.jsonl", meta);
    snprintf(buf, sizeof(buf),
        "{not json at all}\n"
        "{\"v\":1,\"ts\":1745678910100,\"kind\":\"play\","
        "\"track\":\"%s\",\"played_ms\":100,\"duration_ms\":100}\n", SHA_A);
    write_file(path, buf);

    struct ingest_stats st = {0};
    ingest_run(db, meta, &st, 0);
    expect(st.plays_inserted == 1, "malformed: valid line still ingested");
    expect(st.lines_skipped_parse_error == 1, "malformed: 1 parse_error");

    db_close(db); unlink(dbp); free(dbp); rm_rf(meta); free(meta);
}

static void test_oversize_line_in_stats(void)
{
    char *meta = make_tmp_meta();
    char *dbp  = make_tmp_dbpath();
    struct nocturne_db *db = db_open(dbp, NULL, NULL);
    insert_track(db, SHA_A);

    char path[512];
    snprintf(path, sizeof(path), "%s/stats/phone-T1.jsonl", meta);

    /* Manually write: one good line, then 70 KiB of 'x' + \n, then
     * another good line. The middle line is oversize. */
    FILE *f = fopen(path, "w");
    fprintf(f, "{\"v\":1,\"ts\":1745678910100,\"kind\":\"play\","
               "\"track\":\"%s\",\"played_ms\":1,\"duration_ms\":1}\n", SHA_A);
    size_t big = 70 * 1024;
    char *xs = malloc(big);
    memset(xs, 'x', big);
    fwrite(xs, 1, big, f);
    free(xs);
    fputc('\n', f);
    fprintf(f, "{\"v\":1,\"ts\":1745678910300,\"kind\":\"play\","
               "\"track\":\"%s\",\"played_ms\":2,\"duration_ms\":2}\n", SHA_A);
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    struct ingest_stats st = {0};
    ingest_run(db, meta, &st, 0);
    expect(st.plays_inserted == 2, "oversize: 2 valid plays processed");
    expect(st.lines_skipped_oversize == 1, "oversize: 1 oversize counted");

    db_close(db); unlink(dbp); free(dbp); rm_rf(meta); free(meta);
}

static void test_dry_run(void)
{
    char *meta = make_tmp_meta();
    char *dbp  = make_tmp_dbpath();
    struct nocturne_db *db = db_open(dbp, NULL, NULL);
    insert_track(db, SHA_A);
    char path[512], buf[1024];
    snprintf(path, sizeof(path), "%s/stats/phone-T1.jsonl", meta);
    snprintf(buf, sizeof(buf),
        "{\"v\":1,\"ts\":1745678910100,\"kind\":\"play\","
        "\"track\":\"%s\",\"played_ms\":100,\"duration_ms\":100}\n"
        "{\"v\":1,\"ts\":1745678910200,\"kind\":\"play\","
        "\"track\":\"%s\",\"played_ms\":100,\"duration_ms\":100}\n",
        SHA_A, SHA_A);
    write_file(path, buf);

    struct ingest_stats st = {0};
    ingest_run(db, meta, &st, /*dry_run=*/1);
    expect(st.plays_inserted == 2, "dry-run: counts events");
    expect(count_rows(db, "plays") == 0, "dry-run: zero plays in DB");
    expect(count_rows(db, "ingest_offsets") == 0,
           "dry-run: zero ingest_offsets rows in DB");

    /* A real run after dry-run should still insert everything. */
    struct ingest_stats st2 = {0};
    ingest_run(db, meta, &st2, 0);
    expect(st2.plays_inserted == 2, "post-dry-run: real run still inserts 2");

    db_close(db); unlink(dbp); free(dbp); rm_rf(meta); free(meta);
}

static void test_album_like_does_not_pollute_track(void)
{
    /* Album-level likes are stored but not joined by resolver SQL_LOVED.
     * We assert: row exists with unit='album'; no row with unit='track'
     * for the same id. */
    char *meta = make_tmp_meta();
    char *dbp  = make_tmp_dbpath();
    struct nocturne_db *db = db_open(dbp, NULL, NULL);
    char path[512], buf[1024];
    snprintf(path, sizeof(path), "%s/likes-phone-T1.jsonl", meta);
    snprintf(buf, sizeof(buf),
        "{\"v\":1,\"ts\":1745678910100,\"unit\":\"album\","
        "\"id\":\"alb_xyz\",\"liked\":true}\n");
    write_file(path, buf);

    struct ingest_stats st = {0};
    ingest_run(db, meta, &st, 0);
    expect(select_int(db,
        "SELECT COUNT(*) FROM likes WHERE unit='album' AND id='alb_xyz'") == 1,
        "album-like: row exists with unit='album'");
    expect(select_int(db,
        "SELECT COUNT(*) FROM likes WHERE unit='track' AND id='alb_xyz'") == 0,
        "album-like: no row with unit='track'");

    db_close(db); unlink(dbp); free(dbp); rm_rf(meta); free(meta);
}

static void test_unexpected_path_ignored(void)
{
    char *meta = make_tmp_meta();
    char *dbp  = make_tmp_dbpath();
    struct nocturne_db *db = db_open(dbp, NULL, NULL);
    insert_track(db, SHA_A);
    /* Write to a non-matching path: stats/desktop-X.jsonl. The glob
     * pattern is stats/phone-*.jsonl so this file should be ignored. */
    char path[512], buf[1024];
    snprintf(path, sizeof(path), "%s/stats/desktop-X.jsonl", meta);
    snprintf(buf, sizeof(buf),
        "{\"v\":1,\"ts\":1745678910100,\"kind\":\"play\","
        "\"track\":\"%s\",\"played_ms\":1,\"duration_ms\":1}\n", SHA_A);
    write_file(path, buf);

    struct ingest_stats st = {0};
    ingest_run(db, meta, &st, 0);
    expect(st.files_seen == 0, "unexpected-path: files_seen == 0");
    expect(st.plays_inserted == 0, "unexpected-path: no plays ingested");

    db_close(db); unlink(dbp); free(dbp); rm_rf(meta); free(meta);
}

static void test_unknown_kind(void)
{
    char *meta = make_tmp_meta();
    char *dbp  = make_tmp_dbpath();
    struct nocturne_db *db = db_open(dbp, NULL, NULL);
    insert_track(db, SHA_A);
    char path[512], buf[1024];
    snprintf(path, sizeof(path), "%s/stats/phone-T1.jsonl", meta);
    snprintf(buf, sizeof(buf),
        "{\"v\":1,\"ts\":1745678910100,\"kind\":\"scrub\","
        "\"track\":\"%s\",\"played_ms\":1,\"duration_ms\":1}\n", SHA_A);
    write_file(path, buf);

    struct ingest_stats st = {0};
    ingest_run(db, meta, &st, 0);
    expect(st.plays_inserted == 0, "unknown-kind: no plays ingested");
    expect(st.lines_skipped_parse_error == 1, "unknown-kind: parse_error++");

    db_close(db); unlink(dbp); free(dbp); rm_rf(meta); free(meta);
}

static void test_v_not_one(void)
{
    char *meta = make_tmp_meta();
    char *dbp  = make_tmp_dbpath();
    struct nocturne_db *db = db_open(dbp, NULL, NULL);
    insert_track(db, SHA_A);
    char path[512], buf[1024];
    snprintf(path, sizeof(path), "%s/stats/phone-T1.jsonl", meta);
    snprintf(buf, sizeof(buf),
        "{\"v\":2,\"ts\":1745678910100,\"kind\":\"play\","
        "\"track\":\"%s\",\"played_ms\":1,\"duration_ms\":1}\n", SHA_A);
    write_file(path, buf);

    struct ingest_stats st = {0};
    ingest_run(db, meta, &st, 0);
    expect(st.plays_inserted == 0, "v!=1: line skipped");
    expect(st.lines_skipped_parse_error == 1, "v!=1: parse_error++");

    db_close(db); unlink(dbp); free(dbp); rm_rf(meta); free(meta);
}

static void test_track_not_hex(void)
{
    char *meta = make_tmp_meta();
    char *dbp  = make_tmp_dbpath();
    struct nocturne_db *db = db_open(dbp, NULL, NULL);
    char path[512], buf[1024];
    snprintf(path, sizeof(path), "%s/stats/phone-T1.jsonl", meta);
    /* track is short / not 64 hex chars. */
    snprintf(buf, sizeof(buf),
        "{\"v\":1,\"ts\":1745678910100,\"kind\":\"play\","
        "\"track\":\"abcd\",\"played_ms\":1,\"duration_ms\":1}\n");
    write_file(path, buf);

    struct ingest_stats st = {0};
    ingest_run(db, meta, &st, 0);
    expect(st.plays_inserted == 0, "track-not-hex: line skipped");
    expect(st.lines_skipped_parse_error == 1, "track-not-hex: parse_error++");

    db_close(db); unlink(dbp); free(dbp); rm_rf(meta); free(meta);
}

static void test_mid_stream_restart(void)
{
    /* Two lines in file. First ingest reads them. Append a third. Second
     * ingest only reads the third. This exercises the persisted offset
     * path the same way the partial-line test does. */
    char *meta = make_tmp_meta();
    char *dbp  = make_tmp_dbpath();
    struct nocturne_db *db = db_open(dbp, NULL, NULL);
    insert_track(db, SHA_A);
    insert_track(db, SHA_B);
    insert_track(db, SHA_C);

    char path[512], buf[2048];
    snprintf(path, sizeof(path), "%s/stats/phone-T1.jsonl", meta);
    snprintf(buf, sizeof(buf),
        "{\"v\":1,\"ts\":1745678910100,\"kind\":\"play\","
        "\"track\":\"%s\",\"played_ms\":1,\"duration_ms\":1}\n"
        "{\"v\":1,\"ts\":1745678910200,\"kind\":\"play\","
        "\"track\":\"%s\",\"played_ms\":1,\"duration_ms\":1}\n", SHA_A, SHA_B);
    write_file(path, buf);

    struct ingest_stats st1 = {0};
    ingest_run(db, meta, &st1, 0);
    expect(st1.plays_inserted == 2, "restart: first run reads 2");

    /* Append a third event. */
    snprintf(buf, sizeof(buf),
        "{\"v\":1,\"ts\":1745678910300,\"kind\":\"play\","
        "\"track\":\"%s\",\"played_ms\":1,\"duration_ms\":1}\n", SHA_C);
    append_file(path, buf);

    struct ingest_stats st2 = {0};
    ingest_run(db, meta, &st2, 0);
    expect(st2.plays_inserted == 1,
           "restart: second run reads only the appended line");

    db_close(db); unlink(dbp); free(dbp); rm_rf(meta); free(meta);
}

int main(int argc, char **argv)
{
    (void) argc; (void) argv;

    test_empty_meta();
    test_single_play();
    test_multi_play_two_files();
    test_lww_like_unlike_same_file();
    test_lww_cross_file();
    test_lww_out_of_order();
    test_pin_unpin_album();
    test_pin_track_and_album_same_id();
    test_mixed_sources();
    test_idempotent_rerun();
    test_partial_line_then_full();
    test_out_of_range_ts();
    test_malformed_json();
    test_oversize_line_in_stats();
    test_dry_run();
    test_album_like_does_not_pollute_track();
    test_unexpected_path_ignored();
    test_unknown_kind();
    test_v_not_one();
    test_track_not_hex();
    test_mid_stream_restart();

    return test_finish(__FILE__);
}
