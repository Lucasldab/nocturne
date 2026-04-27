#ifndef NOCTURNE_NOCTURNED_DIFF_H
#define NOCTURNE_NOCTURNED_DIFF_H

/*
 * diff.h — manifest diff formatter for `nocturned resolve --dry-run --diff`.
 *
 * Given a freshly-resolved (in-memory) manifest and a SQLite handle whose
 * `manifest_current` table holds the previously-published resident set,
 * print the delta in either text or JSON form. Read-only: zero writes
 * to the DB, zero file emission, zero Syncthing calls.
 *
 * Sections produced (text mode):
 *   ADDED:   N tracks, +B MiB     <sha-12>  <buckets-csv>     <size> MiB
 *   REMOVED: N tracks, -B MiB     <sha-12>  <buckets-csv>     <size> MiB
 *   SHIFTED BUCKETS: N tracks ... <sha-12>  +<gained> -<lost>
 *   NET:     +/-N tracks, +/-B MiB
 *   USED:    <before> MiB -> <after> MiB  (cap_effective <cap-eff> MiB)
 *
 * JSON mode emits a single-line object with the same data:
 *   {"added":[{"id":..,"buckets":[..],"size_bytes":N}],
 *    "removed":[..],
 *    "shifted":[{"id":..,"added_buckets":[..],"removed_buckets":[..]}],
 *    "net_tracks":N,"net_bytes":B,
 *    "used_before":U0,"used_after":U1,"cap_effective_bytes":C}
 *
 * Returns 0 on success, -1 on internal error (SQL prep / OOM). On success,
 * `out` has been written to and flushed by the caller's convention (we do
 * not fclose).
 */

#include <stdio.h>

struct nocturne_db;
struct manifest;

int print_resolve_diff(struct nocturne_db *db,
                       const struct manifest *next,
                       int as_json,
                       FILE *out);

#endif /* NOCTURNE_NOCTURNED_DIFF_H */
