/*
 * check.c — canonical-schema checker (TAG-01) + multi-value heuristic (TAG-03).
 *
 * Pure: given a `tag_record`, produces a deterministic `check_result`. Does
 * not touch the filesystem or call into TagLib — those happened upstream
 * in tags.c / walker.c.
 *
 * Multi-value heuristic separators (locked CONTEXT decision):
 *   ';'         literal semicolon
 *   '/'         literal forward slash      [false-positive: "AC/DC"]
 *   ' & '       ampersand with surrounding spaces
 *   ' and '     case-insensitive
 *   ', '        comma-space
 *   ' feat. '   case-insensitive
 *
 * The AC/DC case is documented as a known false-positive: flagged for
 * human review (CHECK_FLAG), never auto-quarantined. The user is the
 * final disambiguator per track.
 */

#define _GNU_SOURCE

#include "check.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>      /* strcasestr is in <string.h> with _GNU_SOURCE */

static const char *kFieldNames[FIELD_COUNT] = {
    [FIELD_TITLE]         = "title",
    [FIELD_ARTIST]        = "artist",
    [FIELD_ALBUM]         = "album",
    [FIELD_ALBUM_ARTIST]  = "album_artist",
    [FIELD_TRACK_NUMBER]  = "track_number",
    [FIELD_DISC_NUMBER]   = "disc_number",
    [FIELD_YEAR]          = "year",
    [FIELD_GENRE]         = "genre",
};

const char *canonical_field_name(enum canonical_field f)
{
    if ((int)f < 0 || f >= FIELD_COUNT) return "unknown";
    return kFieldNames[f];
}

static const struct tag_field *field_lookup(const struct tag_record *rec,
                                            enum canonical_field f)
{
    if (!rec) return NULL;
    switch (f) {
    case FIELD_TITLE:        return &rec->title;
    case FIELD_ARTIST:       return &rec->artist;
    case FIELD_ALBUM:        return &rec->album;
    case FIELD_ALBUM_ARTIST: return &rec->album_artist;
    case FIELD_TRACK_NUMBER: return &rec->track_number;
    case FIELD_DISC_NUMBER:  return &rec->disc_number;
    case FIELD_YEAR:         return &rec->year;
    case FIELD_GENRE:        return &rec->genre;
    default:                 return NULL;
    }
}

bool check_looks_concatenated_multi_value(const char *s)
{
    if (!s || s[0] == '\0') return false;
    if (strchr(s, ';')) return true;
    if (strchr(s, '/')) return true;
    if (strstr(s, " & ")) return true;
    if (strstr(s, ", ")) return true;
    /* strcasestr is glibc; available on Arch with _GNU_SOURCE. */
    if (strcasestr(s, " and ")) return true;
    if (strcasestr(s, " feat. ")) return true;
    return false;
}

/* Push a new issue onto the result, growing the array as needed. The
 * returned pointer is valid until the next push (which may realloc). */
static void issues_push(struct check_result *cr,
                        enum check_severity sev,
                        enum canonical_field field,
                        const char *code,
                        char *detail_owned)
{
    if (cr->issue_count == cr->issue_capacity) {
        size_t newcap = cr->issue_capacity == 0 ? 4 : cr->issue_capacity * 2;
        struct check_issue *grown = realloc(cr->issues,
                                            newcap * sizeof(*grown));
        if (!grown) {
            /* OOM: silently drop the issue. */
            free(detail_owned);
            return;
        }
        cr->issues = grown;
        cr->issue_capacity = newcap;
    }
    cr->issues[cr->issue_count].severity = sev;
    cr->issues[cr->issue_count].field    = field;
    cr->issues[cr->issue_count].code     = code;       /* static literal */
    cr->issues[cr->issue_count].detail   = detail_owned;
    cr->issue_count++;

    if (sev == CHECK_FAIL) cr->any_fail = true;
    else if (sev == CHECK_FLAG) cr->any_flag = true;
}

/* sprintf-style detail allocator. Returns NULL on OOM (caller continues). */
static char *fmt_detail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *s = NULL;
    int n = vasprintf(&s, fmt, ap);
    va_end(ap);
    if (n < 0) return NULL;
    return s;
}

void check_canonical(const struct tag_record *rec, struct check_result *out)
{
    /* Caller-provided out is expected zero-initialised. Ensure that. */
    out->rec = rec;
    out->tag_read_failed = false;
    out->any_fail = false;
    out->any_flag = false;
    out->issues = NULL;
    out->issue_count = 0;
    out->issue_capacity = 0;

    if (!rec) return;

    /* --- Read failure short-circuits all field checks. --- */
    if (rec->tag_read_failed) {
        out->tag_read_failed = true;
        const char *why = rec->read_error ? rec->read_error : "(unknown)";
        issues_push(out, CHECK_FAIL, FIELD_COUNT, "taglib_open_failed",
                    fmt_detail("taglib could not open file: %s", why));
        return;
    }

    /* --- Encoding / version check (TAG-01). --- */
    if (rec->format == AUDIO_FORMAT_MP3) {
        if (rec->id3_version != 4) {
            issues_push(out, CHECK_FAIL, FIELD_COUNT, "id3_not_v24",
                        fmt_detail("requires ID3v2.4 (got v2.%d or none)",
                                   (int)rec->id3_version));
        }
    } else if (rec->format == AUDIO_FORMAT_UNKNOWN) {
        issues_push(out, CHECK_FAIL, FIELD_COUNT, "unknown_format",
                    fmt_detail("file format not recognised by extension"));
    }
    /* FLAC / OPUS / OGG / MP4: spec-mandated UTF-8; no extra check needed. */

    /* --- Per-field presence check (TAG-01). --- */
    for (enum canonical_field f = FIELD_TITLE; f < FIELD_COUNT; f++) {
        const struct tag_field *tf = field_lookup(rec, f);
        if (!tf) continue;
        bool absent = (!tf->present || tf->value == NULL ||
                       tf->value[0] == '\0');
        if (absent) {
            issues_push(out, CHECK_FAIL, f, "missing_field",
                        fmt_detail("missing canonical field: %s",
                                   canonical_field_name(f)));
        }
    }

    /* --- Multi-value heuristic (TAG-03) — ARTIST and GENRE only. --- */
    enum canonical_field heur_fields[] = { FIELD_ARTIST, FIELD_GENRE };
    for (size_t i = 0; i < sizeof(heur_fields) / sizeof(heur_fields[0]); i++) {
        enum canonical_field f = heur_fields[i];
        const struct tag_field *tf = field_lookup(rec, f);
        if (!tf || !tf->present || !tf->value) continue;

        if (tf->is_multi_value_canonical) continue; /* canonical → no flag */

        if (check_looks_concatenated_multi_value(tf->value)) {
            /* Truncate displayed value to 80 chars in the detail string. */
            char snippet[96];
            snprintf(snippet, sizeof(snippet), "%.80s",
                     tf->value ? tf->value : "");
            issues_push(out, CHECK_FLAG, f, "concat_multi_value",
                        fmt_detail("value '%s' looks concatenated; consider "
                                   "re-tagging as multi-value", snippet));
        }
    }
}

void check_result_free(struct check_result *cr)
{
    if (!cr) return;
    if (cr->issues) {
        for (size_t i = 0; i < cr->issue_count; i++) {
            free(cr->issues[i].detail);
        }
        free(cr->issues);
    }
    cr->issues = NULL;
    cr->issue_count = 0;
    cr->issue_capacity = 0;
    cr->any_fail = false;
    cr->any_flag = false;
    cr->tag_read_failed = false;
    cr->rec = NULL;
}
