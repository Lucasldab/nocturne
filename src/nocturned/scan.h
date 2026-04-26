#ifndef NOCTURNE_NOCTURNED_SCAN_H
#define NOCTURNE_NOCTURNED_SCAN_H

#include <stddef.h>

struct nocturne_db;

struct scan_stats {
    size_t files_seen;
    size_t files_added;
    size_t files_updated;
    size_t files_removed;
    size_t files_skipped_unchanged;
    size_t tag_parse_failed;
    size_t hash_failed;
    size_t elapsed_ms;
};

/* Walk `library_root` once, syncing the tracks table to disk content.
 *
 * Returns 0 on full success, 1 on partial (some hash_failed / parse_failed
 * but the run still produced a usable DB), -1 on hard error (root
 * inaccessible, db error). Stats always populated, even on error. */
int scan_run(struct nocturne_db *db, const char *library_root,
             struct scan_stats *out);

#endif /* NOCTURNE_NOCTURNED_SCAN_H */
