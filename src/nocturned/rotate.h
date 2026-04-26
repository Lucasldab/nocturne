#ifndef NOCTURNE_NOCTURNED_ROTATE_H
#define NOCTURNE_NOCTURNED_ROTATE_H

#include <stddef.h>

struct nocturne_db;

struct rotate_stats {
    long long to_add;          /* tracks moving archive→resident (planned) */
    long long to_remove;       /* tracks moving resident→archive (planned) */
    long long added;
    long long removed;
    long long already_applied; /* link returned EEXIST + same inode */
    long long fallback_copies; /* EXDEV fallbacks */
    long long errors;
};

/* rotate_run reads manifest_current and residency_state from db, computes
 * the diff, and applies it on disk under library_root.
 *
 * Add path:    <library_root>/archive/<rel>  → <library_root>/resident/<rel>
 *              via link(2) + unlink(2). On EXDEV: copy+delete fallback.
 * Remove path: reverse.
 *
 * Order: additions are applied BEFORE removals (Pitfall 1 — phone never
 * goes below cap during rotation).
 *
 * After file motion, residency_state is updated row-by-row and
 * manifest_meta.last_rotation_at is upserted.
 *
 * Returns 0 on success, -1 on fatal error. Per-track errors are counted
 * in stats.errors and do not abort the run. */
int rotate_run(struct nocturne_db *db, const char *library_root,
               struct rotate_stats *out);

/* Test seam: same convention as migrate_set_link_fn_for_testing. The
 * override receives (oldpath, newpath); return 0 success or set errno + -1.
 * Pass NULL to restore the default link(2). */
typedef int (*rotate_link_fn)(const char *oldpath, const char *newpath);
void rotate_set_link_fn_for_testing(rotate_link_fn fn);

#endif /* NOCTURNE_NOCTURNED_ROTATE_H */
