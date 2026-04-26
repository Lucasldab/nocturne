#ifndef NOCTURNE_TAGCHECK_QUARANTINE_H
#define NOCTURNE_TAGCHECK_QUARANTINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "check.h"

struct quarantine_ctx {
    char *library_root;        /* owned; resolved via realpath. Used for
                                  relative-path computation. */
    char *quarantine_root;     /* owned; resolved via realpath. Mirror of
                                  library_root layout. */
    bool dry_run;              /* true → don't move; don't open log */
    FILE *log;                 /* append-only handle; NULL in dry_run mode */
    char *log_path;            /* owned; <quarantine_root>/quarantine.log */
    int log_fd;                /* for flock(2); -1 in dry_run mode */
    size_t moved_count;        /* total successful moves (or would-move
                                  count in dry-run) */
    size_t failed_moves;       /* moves attempted but failed */
};

/* Initialize. Resolves both paths via realpath, refuses if either is
 * missing. In real-run mode: opens the log for append, takes
 * flock(LOCK_EX|LOCK_NB) so a second concurrent --quarantine refuses. In
 * dry-run mode: no log work.
 *
 * Returns 0 on success, non-zero on failure (after writing message to
 * stderr). */
int quarantine_init(struct quarantine_ctx *ctx,
                    const char *library_path,
                    const char *quarantine_path,
                    bool dry_run);

/* Create the quarantine directory if it doesn't exist. mkdir mode 0700.
 * Returns 0 on success, non-zero on failure. */
int quarantine_create_dir(const char *quarantine_path);

/* Decide whether a check_result should be quarantined.
 * Per locked rule: only schema FAILs (TAG-01 violations). FLAGs
 * (multi-value concatenation suspects) are NOT moved. */
bool quarantine_should_move(const struct check_result *cr);

/* Move (rename) the file referenced by cr->rec->path into the quarantine
 * subtree, preserving the path relative to the library root. Creates
 * intermediate dirs as needed. Appends a line to quarantine.log.
 *
 * Behaviour:
 *   - If ctx->dry_run: print to stderr the move that WOULD happen;
 *     increment moved_count; do not touch filesystem; do not write log.
 *   - Source not under library_root: refuse; return non-zero.
 *   - Target file already exists: append `.dup-<unix_ts>` suffix; if that
 *     also collides, fall back to `.dup-<unix_ts>-<rand>`.
 *   - rename(2) returns EXDEV (cross-device): refuse with clear error;
 *     do not copy-then-unlink.
 *
 * Returns 0 on success (including successful dry-run), non-zero on
 * failure. */
int quarantine_move(struct quarantine_ctx *ctx, const struct check_result *cr);

/* Close the log, release flock, free owned strings. Idempotent. */
void quarantine_close(struct quarantine_ctx *ctx);

#endif
