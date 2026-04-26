#ifndef NOCTURNE_NOCTURNED_MIGRATE_H
#define NOCTURNE_NOCTURNED_MIGRATE_H

#include <stdbool.h>
#include <stddef.h>

struct nocturne_db;

struct migrate_stats {
    long long planned;            /* tracks that would move (dry-run + apply) */
    long long moved;              /* tracks moved (apply only) */
    long long already_archived;   /* tracks already under archive/ or resident/ */
    long long skipped_outside;    /* tracks whose path is not under root */
    long long fallback_copies;    /* cross-fs copy+delete fallbacks */
    long long errors;             /* per-track errors (continue on error) */
};

/* If apply == false, no filesystem mutations and no DB writes — only
 * stats.planned is populated. If apply == true, performs link+unlink
 * (or copy+delete on EXDEV) and rewrites tracks.path one row at a time.
 *
 * Returns 0 on success (even if some per-track errors occurred — see
 * stats.errors). Returns -1 on fatal errors (cannot stat library_root,
 * cannot prepare statement, etc.). */
int migrate_run(struct nocturne_db *db, const char *library_root,
                bool apply, struct migrate_stats *out);

/* Test seam: override the link(2) syscall used by migrate_run. Pass NULL
 * to restore the default. The override receives (oldpath, newpath) and
 * must return 0 on success or set errno + return -1. Used by
 * tests/test_migrate.c to inject EXDEV. Always exposed; production code
 * never calls it. */
typedef int (*migrate_link_fn)(const char *oldpath, const char *newpath);
void migrate_set_link_fn_for_testing(migrate_link_fn fn);

#endif /* NOCTURNE_NOCTURNED_MIGRATE_H */
