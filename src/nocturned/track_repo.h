#ifndef NOCTURNE_NOCTURNED_TRACK_REPO_H
#define NOCTURNE_NOCTURNED_TRACK_REPO_H

#include <stdbool.h>
#include <stddef.h>

struct nocturne_db;

struct track_row {
    const char *sha256;          /* 64-char hex */
    const char *path;            /* absolute */
    long long mtime_ns;
    long long size_bytes;
    const char *format;          /* "mp3" / "flac" / "opus" / "ogg" / "m4a"; NULL if unknown */
    const char *title;
    const char *artist;          /* canonical (JSON-array string when multi-value) */
    const char *album;
    const char *album_artist;
    const char *track_number;
    const char *disc_number;
    const char *year;
    const char *genre;
    long long duration_ms;       /* -1 if unknown */
    const char *tags_status;     /* "ok" / "incomplete" / "parse_failed" */
    const char *tag_warning;     /* nullable, comma-separated check codes */
    const char *date_added;      /* ISO-8601 UTC, set on first insert */
    const char *last_seen_at;    /* ISO-8601 UTC, set every scan pass */
};

/* Insert a brand-new track. Returns 0 on success, -1 on error. */
int track_repo_insert(struct nocturne_db *db, const struct track_row *row);

/* Update existing row keyed by sha256. date_added is preserved by the
 * UPDATE statement; everything else replaced. Returns 0 on success / row
 * found, -1 on error or no-match. */
int track_repo_update(struct nocturne_db *db, const struct track_row *row);

/* UPSERT helper: tries update by sha256 first, falls back to insert. */
int track_repo_upsert(struct nocturne_db *db, const struct track_row *row);

/* Update existing row's path, keyed by sha256. Used when the same audio
 * payload moves to a new path on disk. Also bumps last_seen_at. */
int track_repo_update_path(struct nocturne_db *db, const char *sha256,
                           const char *new_path, const char *iso_now);

/* Mark a row as seen this pass (updates last_seen_at). Used in the
 * unchanged-skip path so we still distinguish present from removed. */
int track_repo_mark_seen(struct nocturne_db *db, const char *sha256,
                         const char *iso_now);

/* Lookup by absolute path. On hit returns 1, fills *sha256_out (heap, caller
 * frees), *mtime_ns_out, *size_bytes_out. Returns 0 if not found, -1 on
 * error. */
int track_repo_lookup_by_path(struct nocturne_db *db, const char *path,
                              char **sha256_out,
                              long long *mtime_ns_out,
                              long long *size_bytes_out);

/* Lookup by sha256. Returns 1 on hit, 0 on miss, -1 on error. */
int track_repo_lookup_by_sha256(struct nocturne_db *db, const char *sha256);

/* Delete every row whose path begins with `library_root` and whose
 * last_seen_at is strictly less than `cutoff_iso`. Returns the number of
 * rows deleted (>= 0) or -1 on error. */
long track_repo_delete_unseen_under_root(struct nocturne_db *db,
                                         const char *library_root,
                                         const char *cutoff_iso);

/* Total row count (debug / test helper). Returns -1 on error. */
long long track_repo_count(struct nocturne_db *db);

#endif /* NOCTURNE_NOCTURNED_TRACK_REPO_H */
