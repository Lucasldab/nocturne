#ifndef NOCTURNE_TAGCHECK_CHECK_H
#define NOCTURNE_TAGCHECK_CHECK_H

#include <stdbool.h>
#include <stddef.h>

#include "tags.h"

/* Each canonical field that must be present per TAG-01. */
enum canonical_field {
    FIELD_TITLE = 0,
    FIELD_ARTIST,
    FIELD_ALBUM,
    FIELD_ALBUM_ARTIST,
    FIELD_TRACK_NUMBER,
    FIELD_DISC_NUMBER,
    FIELD_YEAR,
    FIELD_GENRE,
    FIELD_COUNT  /* Sentinel; size of the enum. */
};

const char *canonical_field_name(enum canonical_field f);

/* Reasons a track can fail the canonical check (TAG-01) or be flagged for
 * repair (TAG-03).
 *
 *   "fail" = pushes exit code to 1 and (with --quarantine) moves the file.
 *   "flag" = advisory; surfaced in report but does not change exit code or
 *            quarantine.
 */
enum check_severity {
    CHECK_OK = 0,
    CHECK_FLAG,    /* Multi-value concatenation suspected — TAG-03 advisory */
    CHECK_FAIL     /* Missing canonical field, wrong encoding, or wrong
                     ID3 version — TAG-01 */
};

struct check_issue {
    enum check_severity severity;
    enum canonical_field field;        /* FIELD_COUNT if not field-specific */
    const char *code;                  /* short stable identifier (e.g.
                                          "missing_field", "id3_not_v24",
                                          "concat_multi_value", "bad_encoding",
                                          "taglib_open_failed") */
    char *detail;                      /* owned; human-readable explanation */
};

struct check_result {
    const struct tag_record *rec;      /* borrowed; same lifetime as record from walker */

    bool tag_read_failed;              /* mirror of rec->tag_read_failed for convenience */
    bool any_fail;                     /* >=1 issue with severity=CHECK_FAIL */
    bool any_flag;                     /* >=1 issue with severity=CHECK_FLAG */

    struct check_issue *issues;        /* owned dynamic array */
    size_t issue_count;
    size_t issue_capacity;
};

/* Run all canonical-schema checks + multi-value heuristic on `rec`.
 * Allocates `issues` array if any issues found. Caller must call
 * `check_result_free`. */
void check_canonical(const struct tag_record *rec, struct check_result *out);

/* Free the issues array inside `cr` (does not free `cr` itself, which is
 * usually stack-allocated). */
void check_result_free(struct check_result *cr);

/* Heuristic: detect concatenated multi-value strings. Returns true if `s`
 * (single, NUL-terminated) contains any of: ';', '/', ' & ', ' and '
 * (case-insensitive on the alphabetic ones), ', ', ' feat. ' (case-insensitive).
 *
 * NULL or empty → false. */
bool check_looks_concatenated_multi_value(const char *s);

#endif
