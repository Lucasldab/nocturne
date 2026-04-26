#ifndef NOCTURNE_NOCTURNED_INGEST_H
#define NOCTURNE_NOCTURNED_INGEST_H

#include <stddef.h>

struct nocturne_db;

/* Ingest run statistics. All counters are >= 0; init to {0}. */
struct ingest_stats {
    long files_seen;                  /* total source files processed */
    long plays_inserted;              /* rows added to plays */
    long likes_upserted;              /* successful UPSERTs into likes (LWW conflict-resolved) */
    long pins_upserted;               /* successful UPSERTs into pins */
    long offsets_advanced;            /* files whose ingest_offsets row moved forward */
    long lines_skipped_parse_error;   /* malformed JSON / failed validation / unknown event */
    long lines_skipped_oversize;      /* >64 KiB lines (rejected by jsonl reader) */
};

/* Glob `meta_dir` for `stats/phone-*.jsonl`, `likes-phone-*.jsonl`,
 * `pins-phone-*.jsonl`. For each match: read from the persisted byte
 * offset (ingest_offsets table), parse each line as JSON, dispatch to
 * the appropriate handler, persist the new offset.
 *
 *   db        — open daemon DB (Phase 7 schema, user_version >= 4).
 *   meta_dir  — absolute path; missing or empty directory is fine
 *               (returns 0 with stats all zero).
 *   stats_out — caller-owned, filled with run counters. May be NULL.
 *   dry_run   — non-zero: parse + count but skip all DB writes.
 *
 * Returns:
 *    0 — success (per-line errors are non-fatal).
 *   -1 — fatal error (DB write failure, I/O error other than per-line).
 */
int ingest_run(struct nocturne_db *db,
               const char *meta_dir,
               struct ingest_stats *stats_out,
               int dry_run);

#endif /* NOCTURNE_NOCTURNED_INGEST_H */
