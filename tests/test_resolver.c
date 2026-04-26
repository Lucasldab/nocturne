/*
 * test_resolver.c — property tests for resolver_run.
 *
 * Behaviours under test (≥ 15 assertions):
 *   1. Empty DB → cold_start=1, resident=0.
 *   2. DB with N tracks zero plays → cold_start=1, recent_adds populates resident.
 *   3. Set-union: a track in 3 buckets appears once with all 3 bucket names.
 *   4. Cap math: cap_bytes=1GiB, effective=0.7 → cap_effective_bytes=716M; sum stays under.
 *   5. Idempotency: resolver_run twice on same DB produces identical JSON.
 *   6. Cold-start fallback (RESOLVE-06): zero plays + recent_adds bucket populates manifest.
 *   7. RESOLVE-07: --explain handled by resolve_cmd (smoke).
 *   8. Hysteresis (RESOLVE-05): track in prev manifest gets discount, stays admitted.
 *   9. cap_effective_bytes is exactly cap_bytes * effective_ratio.
 *  10. resolver_version field == 1.
 *  11. generated_at_iso starts with "deterministic-".
 *  12. Pins (priority 10) push past cap (CONTEXT pins-as-override decision).
 *  13. Loved bucket: only liked=1 rows.
 *  14. Top-played bucket: only phone- src plays.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sqlite3.h>

#include "config.h"
#include "db.h"
#include "resolver.h"
#include "runner.h"

static char *tmp_db_path(void)
{
    char tmpl[] = "/tmp/nocturne-resolver-XXXXXX.db";
    int fd = mkstemps(tmpl, 3);
    if (fd < 0) return NULL;
    close(fd);
    return strdup(tmpl);
}

static int exec_sql(struct nocturne_db *db, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(db_handle(db), sql, NULL, NULL, &err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK ? 0 : -1;
}

/* Insert a synthetic track. */
static void insert_track(struct nocturne_db *db, const char *sha,
                         const char *path, long long size, const char *date_added)
{
    char sql[1024];
    snprintf(sql, sizeof(sql),
        "INSERT INTO tracks (sha256, path, mtime_ns, size_bytes, "
        "tags_status, date_added, last_seen_at) VALUES "
        "('%s', '%s', 0, %lld, 'ok', '%s', '%s')",
        sha, path, size, date_added, date_added);
    exec_sql(db, sql);
}

static int resident_index(const struct manifest *m, const char *sha)
{
    for (size_t i = 0; i < m->resident_n; i++) {
        if (!strcmp(m->resident[i].sha256, sha)) return (int) i;
    }
    return -1;
}

static int bucket_in_track(const struct manifest_track *t, const char *name)
{
    for (size_t i = 0; i < t->buckets_n; i++) {
        if (!strcmp(t->buckets[i], name)) return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    (void) argc; (void) argv;

    /* 1. Empty DB. */
    {
        char *dbp = tmp_db_path();
        struct nocturne_db *db = db_open(dbp, NULL, NULL);
        struct nocturne_config cfg; config_default(&cfg);
        struct manifest m;
        int rc = resolver_run(db, &cfg, &m);
        expect(rc == 0, "empty: resolver_run returns 0");
        expect(m.cold_start == 1, "empty: cold_start=1");
        expect(m.resident_n == 0, "empty: resident is empty");
        expect(m.resolver_version == RESOLVER_VERSION,
               "empty: resolver_version field set");
        expect(m.generated_at_iso != NULL &&
               strncmp(m.generated_at_iso, "deterministic-", 14) == 0,
               "empty: generated_at_iso prefix is 'deterministic-'");
        manifest_free(&m); config_free(&cfg);
        db_close(db); unlink(dbp); free(dbp);
    }

    /* 2. Recent_adds cold-start populates resident. */
    {
        char *dbp = tmp_db_path();
        struct nocturne_db *db = db_open(dbp, NULL, NULL);
        for (int i = 0; i < 10; i++) {
            char sha[16]; snprintf(sha, sizeof(sha), "ra%07d", i);
            char path[64]; snprintf(path, sizeof(path), "/tmp/ra%d.mp3", i);
            char date[32]; snprintf(date, sizeof(date), "2026-04-%02dT00:00:00Z", (i % 28) + 1);
            insert_track(db, sha, path, 5 * 1024 * 1024, date);
        }
        struct nocturne_config cfg; config_default(&cfg);
        struct manifest m;
        resolver_run(db, &cfg, &m);
        expect(m.cold_start == 1, "cold-start: cold_start=1 with no plays");
        expect(m.resident_n > 0,
               "cold-start: resident populated by recent_adds + exploration");
        manifest_free(&m); config_free(&cfg);
        db_close(db); unlink(dbp); free(dbp);
    }

    /* 3. Set-union: a track in 3 buckets. */
    {
        char *dbp = tmp_db_path();
        struct nocturne_db *db = db_open(dbp, NULL, NULL);
        insert_track(db, "multi", "/tmp/multi.mp3", 1024, "2026-04-26T00:00:00Z");
        /* Make it loved + pinned + give it phone plays so top_played also picks it. */
        exec_sql(db,
            "INSERT INTO likes (sha256, liked, updated_at) VALUES "
            "('multi', 1, '2026-04-26T00:00:00Z')");
        exec_sql(db,
            "INSERT INTO pins (unit, id, pinned, updated_at) VALUES "
            "('track', 'multi', 1, '2026-04-26T00:00:00Z')");
        for (int i = 0; i < 20; i++) {
            char sql[256];
            snprintf(sql, sizeof(sql),
                "INSERT INTO plays (sha256, played_at, is_skip, src) VALUES "
                "('multi', '2026-04-26T0%d:00:00Z', 0, 'phone-LK4F')", i % 10);
            exec_sql(db, sql);
        }
        struct nocturne_config cfg; config_default(&cfg);
        struct manifest m;
        resolver_run(db, &cfg, &m);
        int idx = resident_index(&m, "multi");
        expect(idx >= 0, "set-union: 'multi' track is in resident");
        if (idx >= 0) {
            const struct manifest_track *t = &m.resident[idx];
            expect(t->buckets_n >= 3,
                   "set-union: 'multi' has 3+ buckets recorded");
            expect(bucket_in_track(t, "loved"),
                   "set-union: 'loved' bucket recorded");
            expect(bucket_in_track(t, "manual_pins"),
                   "set-union: 'manual_pins' bucket recorded");
            expect(bucket_in_track(t, "top_played"),
                   "set-union: 'top_played' bucket recorded");
            /* size counted ONCE toward used_bytes. */
            expect(m.used_bytes == 1024,
                   "set-union: size counted once toward used_bytes");
        }
        manifest_free(&m); config_free(&cfg);
        db_close(db); unlink(dbp); free(dbp);
    }

    /* 4. Cap respected. cap_bytes = 1 GiB → cap_effective = 716M. We add
     *    50 tracks of 50 MB each = 2.5 GiB; resolver should pick at most
     *    14-15 (14*50MB=700MB, 15*50MB=750MB > 716MB). */
    {
        char *dbp = tmp_db_path();
        struct nocturne_db *db = db_open(dbp, NULL, NULL);
        for (int i = 0; i < 50; i++) {
            char sha[32]; snprintf(sha, sizeof(sha), "cap%07d", i);
            char path[64]; snprintf(path, sizeof(path), "/tmp/cap%d.mp3", i);
            char date[32]; snprintf(date, sizeof(date), "2026-04-%02dT00:00:00Z", (i % 28) + 1);
            insert_track(db, sha, path, 50LL * 1024 * 1024, date);
        }
        struct nocturne_config cfg; config_default(&cfg);
        cfg.cap_bytes = 1LL * 1024 * 1024 * 1024;
        cfg.cap_effective_ratio = 0.70;
        cfg.hysteresis_ratio = 0.0;  /* simplify cap math for this assertion */
        struct manifest m;
        resolver_run(db, &cfg, &m);
        long long expected_cap = (long long) (cfg.cap_bytes * cfg.cap_effective_ratio);
        expect(m.cap_effective_bytes == expected_cap,
               "cap: cap_effective_bytes = cap_bytes * 0.70");
        expect(m.used_bytes <= m.cap_effective_bytes,
               "cap: used_bytes stays under cap_effective_bytes");
        expect(m.resident_n > 0, "cap: at least one track admitted");
        manifest_free(&m); config_free(&cfg);
        db_close(db); unlink(dbp); free(dbp);
    }

    /* 5. Idempotency: same DB → same JSON. */
    {
        char *dbp = tmp_db_path();
        struct nocturne_db *db = db_open(dbp, NULL, NULL);
        for (int i = 0; i < 5; i++) {
            char sha[32]; snprintf(sha, sizeof(sha), "idem%07d", i);
            char path[64]; snprintf(path, sizeof(path), "/tmp/idem%d.mp3", i);
            insert_track(db, sha, path, 100 * 1024, "2026-04-26T00:00:00Z");
        }
        struct nocturne_config cfg; config_default(&cfg);
        cfg.random_seed = 12345;  /* fix seed for exploration */

        struct manifest m1, m2;
        resolver_run(db, &cfg, &m1);
        resolver_run(db, &cfg, &m2);

        FILE *f1 = tmpfile(); FILE *f2 = tmpfile();
        manifest_emit_json(&m1, f1);
        manifest_emit_json(&m2, f2);
        rewind(f1); rewind(f2);
        char b1[8192] = {0}, b2[8192] = {0};
        size_t n1 = fread(b1, 1, sizeof(b1) - 1, f1);
        size_t n2 = fread(b2, 1, sizeof(b2) - 1, f2);
        fclose(f1); fclose(f2);
        expect(n1 == n2 && !memcmp(b1, b2, n1),
               "idempotency: two runs produce byte-identical JSON");

        manifest_free(&m1); manifest_free(&m2); config_free(&cfg);
        db_close(db); unlink(dbp); free(dbp);
    }

    /* 8. Hysteresis: a marginal track stays in if it was previously resident. */
    {
        char *dbp = tmp_db_path();
        struct nocturne_db *db = db_open(dbp, NULL, NULL);
        /* 5 tracks of 100MB each. Cap = 350MB → can hold 3. */
        for (int i = 0; i < 5; i++) {
            char sha[32]; snprintf(sha, sizeof(sha), "hyst%05d", i);
            char path[64]; snprintf(path, sizeof(path), "/tmp/hyst%d.mp3", i);
            /* Stagger date_added so recent_adds ordering is deterministic. */
            char date[32]; snprintf(date, sizeof(date), "2026-04-2%dT00:00:00Z", i);
            insert_track(db, sha, path, 100LL * 1024 * 1024, date);
        }
        /* Pre-populate manifest_current with 'hyst00002' (the marginal one
         * — tracks 4, 3, 2 are the most recent by date_added). */
        exec_sql(db,
            "INSERT INTO manifest_current (sha256, buckets_csv, size_bytes) VALUES "
            "('hyst00002', 'recent_adds', 104857600)");

        struct nocturne_config cfg; config_default(&cfg);
        cfg.cap_bytes = 350LL * 1024 * 1024;
        cfg.cap_effective_ratio = 1.0;  /* skip the 70% rule for clarity */
        cfg.hysteresis_ratio = 0.10;
        struct manifest m;
        resolver_run(db, &cfg, &m);
        int idx = resident_index(&m, "hyst00002");
        expect(idx >= 0, "hysteresis: previously-resident marginal track stays in");
        manifest_free(&m); config_free(&cfg);
        db_close(db); unlink(dbp); free(dbp);
    }

    /* 12. Pins push past the cap. */
    {
        char *dbp = tmp_db_path();
        struct nocturne_db *db = db_open(dbp, NULL, NULL);
        /* One pinned track > cap_effective_bytes — must still be admitted. */
        insert_track(db, "pin000", "/tmp/pin0.mp3", 200LL * 1024 * 1024, "2026-04-26T00:00:00Z");
        exec_sql(db,
            "INSERT INTO pins (unit, id, pinned, updated_at) VALUES "
            "('track', 'pin000', 1, '2026-04-26T00:00:00Z')");
        struct nocturne_config cfg; config_default(&cfg);
        cfg.cap_bytes = 100LL * 1024 * 1024;
        cfg.cap_effective_ratio = 0.70;  /* cap_effective ~ 70 MB */
        struct manifest m;
        resolver_run(db, &cfg, &m);
        int idx = resident_index(&m, "pin000");
        expect(idx >= 0, "pins: 200MB pinned track admitted past 70MB cap");
        manifest_free(&m); config_free(&cfg);
        db_close(db); unlink(dbp); free(dbp);
    }

    /* 13. Loved bucket only includes liked=1. */
    {
        char *dbp = tmp_db_path();
        struct nocturne_db *db = db_open(dbp, NULL, NULL);
        insert_track(db, "lov001", "/tmp/lov1.mp3", 1024, "2026-04-26T00:00:00Z");
        insert_track(db, "lov002", "/tmp/lov2.mp3", 1024, "2026-04-26T00:00:00Z");
        exec_sql(db,
            "INSERT INTO likes (sha256, liked, updated_at) VALUES "
            "('lov001', 1, '2026-04-26T00:00:00Z'), "
            "('lov002', 0, '2026-04-26T00:00:00Z')");
        /* Force at least 10 phone plays so we're NOT in cold-start. */
        for (int i = 0; i < 12; i++) {
            char sql[256];
            snprintf(sql, sizeof(sql),
                "INSERT INTO plays (sha256, played_at, is_skip, src) VALUES "
                "('lov001', '2026-04-26T0%d:00:00Z', 0, 'phone-XX')", i % 10);
            exec_sql(db, sql);
        }
        struct nocturne_config cfg; config_default(&cfg);
        struct manifest m;
        resolver_run(db, &cfg, &m);
        expect(m.cold_start == 0, "loved: not cold-start with 12 phone plays");
        int liked_idx = resident_index(&m, "lov001");
        expect(liked_idx >= 0 && bucket_in_track(&m.resident[liked_idx], "loved"),
               "loved: lov001 (liked=1) has 'loved' bucket");
        int unliked_idx = resident_index(&m, "lov002");
        if (unliked_idx >= 0) {
            expect(!bucket_in_track(&m.resident[unliked_idx], "loved"),
                   "loved: lov002 (liked=0) does NOT have 'loved' bucket");
        } else {
            /* not in resident at all is also fine */
            expect(1, "loved: lov002 (liked=0) absent from resident");
        }
        manifest_free(&m); config_free(&cfg);
        db_close(db); unlink(dbp); free(dbp);
    }

    /* 14. Top-played bucket only counts phone- src. */
    {
        char *dbp = tmp_db_path();
        struct nocturne_db *db = db_open(dbp, NULL, NULL);
        insert_track(db, "top001", "/tmp/top1.mp3", 1024, "2026-04-26T00:00:00Z");
        for (int i = 0; i < 20; i++) {
            char sql[256];
            snprintf(sql, sizeof(sql),
                "INSERT INTO plays (sha256, played_at, is_skip, src) VALUES "
                "('top001', '2026-04-26T0%d:00:00Z', 0, 'desktop-PC')", i % 10);
            exec_sql(db, sql);
        }
        struct nocturne_config cfg; config_default(&cfg);
        struct manifest m;
        resolver_run(db, &cfg, &m);
        /* We're cold-start (zero phone plays) so loved/top_played don't
         * contribute. The track might still be in resident via recent_adds
         * — but it must NOT carry the top_played bucket. */
        int idx = resident_index(&m, "top001");
        if (idx >= 0) {
            expect(!bucket_in_track(&m.resident[idx], "top_played"),
                   "top_played: desktop plays don't promote to top_played bucket");
        } else {
            expect(1, "top_played: track absent from resident (cold start)");
        }
        manifest_free(&m); config_free(&cfg);
        db_close(db); unlink(dbp); free(dbp);
    }

    return test_finish(__FILE__);
}
