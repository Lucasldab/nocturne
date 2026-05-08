#ifndef NOCTURNE_NOCTURNED_ROTATE_H
#define NOCTURNE_NOCTURNED_ROTATE_H

#include <stddef.h>

struct nocturne_db;
struct transcode_cfg;

struct rotate_stats {
    long long to_add;          /* tracks moving archive→resident (planned) */
    long long to_remove;       /* tracks moving resident→archive (planned) */
    long long added;
    long long removed;
    long long already_applied; /* link returned EEXIST + same inode */
    long long fallback_copies; /* EXDEV fallbacks */
    long long respawned;       /* DB said resident but file missing → re-promoted */
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

/* Same as rotate_run but with optional transcode. When `tc` is non-NULL and
 * tc->enabled, promote becomes: keep archive/X.flac (no link, no unlink),
 * transcode to resident/X.<ext>, store transcode metadata in
 * residency_state.transcode_*. Demote: unlink the resident transcode,
 * archive untouched. tracks.path is NEVER mutated in transcode mode — it
 * remains the canonical archive reference for stable id semantics. */
int rotate_run_ex(struct nocturne_db *db, const char *library_root,
                  const struct transcode_cfg *tc, struct rotate_stats *out);

/* Test seam: same convention as migrate_set_link_fn_for_testing. The
 * override receives (oldpath, newpath); return 0 success or set errno + -1.
 * Pass NULL to restore the default link(2). */
typedef int (*rotate_link_fn)(const char *oldpath, const char *newpath);
void rotate_set_link_fn_for_testing(rotate_link_fn fn);

#endif /* NOCTURNE_NOCTURNED_ROTATE_H */
