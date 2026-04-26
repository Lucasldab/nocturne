#ifndef NOCTURNE_TAGCHECK_REPORT_H
#define NOCTURNE_TAGCHECK_REPORT_H

#include <stdbool.h>
#include <stdio.h>

#include "check.h"
#include "walker.h"

enum report_format {
    REPORT_FORMAT_TEXT = 0,    /* human-readable, default */
    REPORT_FORMAT_JSON = 1     /* structured, for piping */
};

/* Accumulator the walker callback feeds into. Owned by main(); freed via
 * report_summary_free. */
struct report_summary {
    enum report_format format;
    FILE *out;                          /* typically stdout */

    /* Counters */
    size_t files_seen;
    size_t files_passed;                /* no FAIL and no FLAG */
    size_t files_flagged_only;          /* FLAG but no FAIL */
    size_t files_failed;                /* >=1 FAIL (also includes tag_read_failed) */
    size_t files_taglib_open_failed;    /* subset of files_failed */

    /* Per-field failure counts (for the summary table at the end). */
    size_t per_field_missing[FIELD_COUNT];

    /* Owned snapshots — JSON mode buffers tracks until report_emit; TEXT
     * mode streams during accumulation and leaves this empty. */
    void *records_buf;                  /* opaque vector; layout chosen by report.c */
    size_t records_buf_count;
    size_t records_buf_capacity;
};

/* Initialize a summary. Caller-allocated `summary`; reporter takes
 * ownership of any allocations it makes inside (records_buf, etc.). */
void report_summary_init(struct report_summary *summary,
                         enum report_format fmt, FILE *out);

/* Add one file's check_result to the summary. In TEXT mode, may write to
 * `out` immediately. In JSON mode, snapshots into records_buf for emission
 * at report_emit. */
void report_add(struct report_summary *summary, const struct check_result *cr);

/* Emit the final report (summary footer for text; full document for JSON). */
void report_emit(struct report_summary *summary,
                 const struct walk_stats *walk_stats);

void report_summary_free(struct report_summary *summary);

#endif
