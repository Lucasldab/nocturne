/*
 * doctor_cmd.c — `nocturned doctor [--json]`.
 *
 * Read-only health report. Does NOT acquire the single-writer lockfile
 * because doctor must be runnable while `nocturned watch` holds it. SQLite
 * with WAL handles read-only access concurrently.
 *
 * Exit codes:
 *   0 — healthy (issues_found == 0).
 *   1 — at least one issue surfaced.
 *   3 — hard error (db open failed, doctor_collect returned -1).
 */

#define _GNU_SOURCE

#include "cli.h"
#include "db.h"
#include "doctor.h"
#include "paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void err_to_stderr(const char *msg, void *ud)
{
    (void) ud;
    fprintf(stderr, "nocturned: %s\n", msg);
}

int doctor_cmd_main(struct cli_args *args)
{
    if (!args) return 64;

    const char *db_path = paths_db_file();
    if (!db_path) {
        fprintf(stderr, "nocturned doctor: cannot resolve DB path\n");
        return 3;
    }

    struct nocturne_db *db = db_open(db_path, err_to_stderr, NULL);
    if (!db) return 3;

    struct doctor_report r;
    int rc = doctor_collect(db, &r);
    if (rc != 0) {
        db_close(db);
        return 3;
    }

    if (args->json) doctor_print_json(&r, stdout);
    else            doctor_print_text(&r, stdout);

    int issues = r.issues_found;
    doctor_report_free(&r);
    db_close(db);
    return issues > 0 ? 1 : 0;
}
