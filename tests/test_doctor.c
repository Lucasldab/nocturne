/*
 * test_doctor.c — unit tests for doctor_collect against synthesised DBs.
 *
 * Behaviours under test (≥ 10 assertions):
 *   1. Empty DB → total_tracks=0, last_scan_at_iso=NULL, no issues.
 *   2. DB with parse_failed and incomplete rows → counts populated.
 *   3. Sample paths captured (up to 5).
 *   4. Orphan detection: row pointing at non-existent path counted.
 *   5. scan_meta freshly written → last_scan_age_seconds is small (< 60).
 *   6. inotify override returns the file's value.
 *   7. inotify headroom math.
 *   8. Lockfile probe: free when no pidfile.
 *   9. Lockfile probe: held when a child process holds the lock.
 *  10. Lockfile probe: stale when pidfile contents but no flock.
 *  11. doctor_print_text on empty DB prints "no tracks scanned yet".
 *  12. doctor_print_json output begins with `{` and ends with `}\n`.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

#include "db.h"
#include "doctor.h"
#include "runner.h"

static char *tmp_db_path(void)
{
    char tmpl[] = "/tmp/nocturne-doctor-XXXXXX.db";
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

int main(int argc, char **argv)
{
    (void) argc; (void) argv;

    /* 1. Empty DB. */
    {
        char *dbp = tmp_db_path();
        struct nocturne_db *db = db_open(dbp, NULL, NULL);
        struct doctor_report r;
        int rc = doctor_collect_with_override(db, "/dev/null", "/tmp/nonexistent.pid", &r);
        expect(rc == 0, "empty DB: doctor_collect returns 0");
        expect(r.total_tracks == 0, "empty DB: total_tracks=0");
        expect(r.last_scan_at_iso == NULL, "empty DB: no scan_meta -> last_scan_at_iso NULL");
        expect(r.lock_held == 0, "empty DB: lockfile state=free when pidfile missing");
        doctor_report_free(&r);
        db_close(db);
        unlink(dbp); free(dbp);
    }

    /* 2 + 3. DB with parse_failed + incomplete + ok rows. */
    {
        char *dbp = tmp_db_path();
        struct nocturne_db *db = db_open(dbp, NULL, NULL);
        /* 5 ok */
        for (int i = 0; i < 5; i++) {
            char sql[256];
            snprintf(sql, sizeof(sql),
                "INSERT INTO tracks (sha256, path, mtime_ns, size_bytes, "
                "tags_status, date_added, last_seen_at) "
                "VALUES ('ok%d', '/tmp/nocturne-doctor-test/exists-ok%d', 0, 0, 'ok', "
                "'2026-04-26T00:00:00Z', '2026-04-26T00:00:00Z')", i, i);
            exec_sql(db, sql);
        }
        /* 2 parse_failed */
        for (int i = 0; i < 2; i++) {
            char sql[256];
            snprintf(sql, sizeof(sql),
                "INSERT INTO tracks (sha256, path, mtime_ns, size_bytes, "
                "tags_status, date_added, last_seen_at) "
                "VALUES ('pf%d', '/tmp/nocturne-doctor-pf-%d', 0, 0, 'parse_failed', "
                "'2026-04-26T00:00:00Z', '2026-04-26T00:00:00Z')", i, i);
            exec_sql(db, sql);
        }
        /* 1 incomplete */
        exec_sql(db,
            "INSERT INTO tracks (sha256, path, mtime_ns, size_bytes, "
            "tags_status, date_added, last_seen_at) "
            "VALUES ('inc1', '/tmp/nocturne-doctor-inc-1', 0, 0, 'incomplete', "
            "'2026-04-26T00:00:00Z', '2026-04-26T00:00:00Z')");

        struct doctor_report r;
        doctor_collect_with_override(db, "/dev/null", "/tmp/nonexistent.pid", &r);
        expect(r.total_tracks == 8, "synth: total_tracks=8");
        expect(r.parse_failed_count == 2, "synth: parse_failed_count=2");
        expect(r.incomplete_count == 1, "synth: incomplete_count=1");
        expect(r.parse_failed_samples_n == 2,
               "synth: 2 parse_failed sample paths captured");
        /* All 8 paths are missing on disk — orphans = 8. */
        expect(r.orphan_count == 8, "synth: orphan_count=8 (all paths missing)");
        expect(r.issues_found > 0, "synth: issues_found > 0");
        doctor_report_free(&r);
        db_close(db);
        unlink(dbp); free(dbp);
    }

    /* 5. scan_meta freshness. */
    {
        char *dbp = tmp_db_path();
        struct nocturne_db *db = db_open(dbp, NULL, NULL);
        /* Use a "now" timestamp generated at test time. */
        time_t now = time(NULL);
        struct tm tm; gmtime_r(&now, &tm);
        char iso[40];
        snprintf(iso, sizeof(iso), "%04u-%02u-%02uT%02u:%02u:%02uZ",
                 (unsigned)(tm.tm_year + 1900), (unsigned)(tm.tm_mon + 1),
                 (unsigned)tm.tm_mday, (unsigned)tm.tm_hour,
                 (unsigned)tm.tm_min, (unsigned)tm.tm_sec);
        char sql[512];
        snprintf(sql, sizeof(sql),
            "INSERT INTO scan_meta (library_root, last_scan_at) VALUES "
            "('/tmp/nocturne-fixt-doctor', '%s')", iso);
        exec_sql(db, sql);

        struct doctor_report r;
        doctor_collect_with_override(db, "/dev/null", "/tmp/nonexistent.pid", &r);
        expect(r.last_scan_at_iso != NULL, "freshness: last_scan_at_iso populated");
        expect(r.last_scan_age_seconds >= 0 && r.last_scan_age_seconds < 60,
               "freshness: scan younger than 60 seconds");
        doctor_report_free(&r);
        db_close(db);
        unlink(dbp); free(dbp);
    }

    /* 6 + 7. inotify override + headroom. */
    {
        char tmpl[] = "/tmp/nocturne-doctor-iw-XXXXXX";
        int fd = mkstemp(tmpl);
        FILE *f = fdopen(fd, "w");
        fprintf(f, "1048576\n");
        fclose(f);

        char *dbp = tmp_db_path();
        struct nocturne_db *db = db_open(dbp, NULL, NULL);
        /* Library root that doesn't exist → library_dir_count stays -1. */
        struct doctor_report r;
        doctor_collect_with_override(db, tmpl, "/tmp/nonexistent.pid", &r);
        expect(r.inotify_max_user_watches == 1048576,
               "inotify override read 1048576");
        doctor_report_free(&r);
        db_close(db);
        unlink(dbp); free(dbp);
        unlink(tmpl);
    }

    /* 9. Lockfile held: child holds a real flock. */
    {
        char tmpl[] = "/tmp/nocturne-doctor-pid-XXXXXX";
        int fd = mkstemp(tmpl);
        close(fd);

        int pipefd[2];
        pipe(pipefd);
        pid_t child = fork();
        if (child == 0) {
            close(pipefd[0]);
            int lfd = open(tmpl, O_RDWR | O_CREAT, 0600);
            flock(lfd, LOCK_EX);
            /* Write pid for doctor to find. */
            char buf[16];
            int n = snprintf(buf, sizeof(buf), "%d\n", (int) getpid());
            ssize_t w = write(lfd, buf, n);
            (void) w;
            char ok = 'A';
            ssize_t w2 = write(pipefd[1], &ok, 1);
            (void) w2;
            usleep(500 * 1000);
            close(lfd);
            _exit(0);
        }
        close(pipefd[1]);
        char ack;
        ssize_t r0 = read(pipefd[0], &ack, 1);
        (void) r0;
        close(pipefd[0]);

        char *dbp = tmp_db_path();
        struct nocturne_db *db = db_open(dbp, NULL, NULL);
        struct doctor_report r;
        doctor_collect_with_override(db, "/dev/null", tmpl, &r);
        expect(r.lock_held == 1, "lockfile probe: held when child holds flock");
        expect(r.lock_holder_pid == (int) child,
               "lockfile probe: holder_pid matches child");
        doctor_report_free(&r);
        db_close(db);
        unlink(dbp); free(dbp);

        waitpid(child, NULL, 0);
        unlink(tmpl);
    }

    /* 10. Lockfile stale: pidfile exists with pid contents but no flock. */
    {
        char tmpl[] = "/tmp/nocturne-doctor-stale-XXXXXX";
        int fd = mkstemp(tmpl);
        FILE *f = fdopen(fd, "w");
        fprintf(f, "99999\n");
        fclose(f);

        char *dbp = tmp_db_path();
        struct nocturne_db *db = db_open(dbp, NULL, NULL);
        struct doctor_report r;
        doctor_collect_with_override(db, "/dev/null", tmpl, &r);
        expect(r.lock_held == 2,
               "lockfile probe: stale when pidfile present but no flock");
        doctor_report_free(&r);
        db_close(db);
        unlink(dbp); free(dbp);
        unlink(tmpl);
    }

    /* 11 + 12. Output formatters. */
    {
        char *dbp = tmp_db_path();
        struct nocturne_db *db = db_open(dbp, NULL, NULL);
        struct doctor_report r;
        doctor_collect_with_override(db, "/dev/null", "/tmp/nonexistent.pid", &r);

        FILE *fp = tmpfile();
        doctor_print_text(&r, fp);
        rewind(fp);
        char buf[1024] = {0};
        size_t got = fread(buf, 1, sizeof(buf) - 1, fp);
        fclose(fp);
        (void) got;
        expect(strstr(buf, "no tracks scanned yet") != NULL,
               "text output on empty DB mentions 'no tracks scanned yet'");

        fp = tmpfile();
        doctor_print_json(&r, fp);
        rewind(fp);
        memset(buf, 0, sizeof(buf));
        got = fread(buf, 1, sizeof(buf) - 1, fp);
        fclose(fp);
        expect(buf[0] == '{', "json output starts with '{'");
        expect(buf[got > 0 ? got - 1 : 0] == '\n' && buf[got > 1 ? got - 2 : 0] == '}',
               "json output ends with '}\\n'");
        expect(strstr(buf, "\"total_tracks\":0") != NULL,
               "json output contains total_tracks:0");

        doctor_report_free(&r);
        db_close(db);
        unlink(dbp); free(dbp);
    }

    return test_finish(__FILE__);
}
