/*
 * test_scan.c — full scan-orchestration tests land in Task 3 of plan 02-02.
 * Task 2 leaves a link-only smoke so the Makefile pattern stays exercised.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "db.h"
#include "track_repo.h"
#include "runner.h"

int main(int argc, char **argv)
{
    (void) argc; (void) argv;

    /* Smoke: open a temp DB, count rows (should be 0), close.
     * Real scenarios land in Task 3. */
    char tmpl[] = "/tmp/nocturne-scan-test-XXXXXX.db";
    int fd = mkstemps(tmpl, 3);
    if (fd >= 0) close(fd);

    struct nocturne_db *db = db_open(tmpl, NULL, NULL);
    expect(db != NULL, "smoke: db_open succeeded");
    if (db) {
        expect(track_repo_count(db) == 0, "smoke: empty DB has 0 tracks");
        db_close(db);
    }
    unlink(tmpl);
    return test_finish(__FILE__);
}
