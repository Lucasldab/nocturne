#ifndef NOCTURNE_NOCTURNED_DISCOVER_H
#define NOCTURNE_NOCTURNED_DISCOVER_H

struct nocturne_db;

struct discover_stats {
    long long picked_never_played;
    long long picked_aged_out;
    long long picked_adjacent_to_loved;
    long long picked_random;
    long long total_picked;
    long long candidates_seen;
    char week_start[16];   /* 'YYYY-MM-DD' (Monday) */
};

/* Pick `count` tracks for the current week and persist to
 * weekly_discovery_picks. Idempotent on (sha256, week_start) — re-running
 * the same Monday is a no-op. The week_start is the most recent Monday in
 * local time at time of call.
 *
 * Picker priority:
 *   1. never-played — tracks the user owns but has zero plays for
 *   2. aged-out    — last play was >120 days ago
 *   3. adjacent-to-loved — same album_artist as a pinned track, but the
 *                          track itself has zero plays
 *   4. random      — pure random non-resident fill
 *
 * Per-album cap = 2 (no deluxe-edition spam).
 *
 * `exclude_album_substrings` (optional, may be NULL) is a semicolon-
 * separated list ("Live;Unplugged;Acoustic"). Tracks whose album column
 * contains any of these substrings are filtered from the candidate pool.
 *
 * Caller's responsibility to acquire the single-writer lock before
 * calling. Returns 0 success, -1 fatal. */
int discover_run(struct nocturne_db *db, int count,
                 const char *exclude_album_substrings,
                 struct discover_stats *out);

#endif /* NOCTURNE_NOCTURNED_DISCOVER_H */
