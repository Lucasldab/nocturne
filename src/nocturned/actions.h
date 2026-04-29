#ifndef NOCTURNE_NOCTURNED_ACTIONS_H
#define NOCTURNE_NOCTURNED_ACTIONS_H

struct nocturne_db;

struct action_stats {
    long long touched;        /* total tracks acted on (album expands) */
    long long files_removed;  /* unlinked archive + resident files */
    long long bytes_freed;    /* sum of sizes for files_removed */
    long long errors;
};

/* Unsync: clear pin if any, write unsync_overrides row. Resolver picks it
 * up next cycle and demotes. Idempotent (re-running on same sha is a
 * no-op INSERT OR IGNORE). Caller holds the single-writer lock. */
int unsync_track(struct nocturne_db *db, const char *sha,
                 struct action_stats *out);

/* Delete-everywhere: rm archive + resident files, blacklist the sha,
 * cascade DELETE tracks row. Permanent. Caller holds the lock. */
int delete_track_everywhere(struct nocturne_db *db, const char *sha,
                            struct action_stats *out);

/* Apply unsync to every track in an album. */
int unsync_album(struct nocturne_db *db, const char *album_id,
                 struct action_stats *out);

/* Apply delete-everywhere to every track in an album. Removes the (now-
 * empty) album/artist directories on a best-effort basis. */
int delete_album_everywhere(struct nocturne_db *db, const char *album_id,
                            struct action_stats *out);

#endif /* NOCTURNE_NOCTURNED_ACTIONS_H */
