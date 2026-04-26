/*
 * report.c — TEXT + JSON reporter for nocturne-tagcheck.
 *
 * Two modes:
 *   - TEXT: streams per-file failure/flag lines to `out` during report_add;
 *           emits the summary footer at report_emit.
 *   - JSON: buffers a deep-copy snapshot of every (path + issues) into
 *           records_buf during report_add; emits the full document at
 *           report_emit.
 *
 * Hand-rolled JSON: we do not pull Jansson into Phase 1's CLI utility —
 * Phase 2's daemon will use it for the wire format. For ~50 lines of
 * structured output here, a small json_escape() + fprintf() pattern keeps
 * the binary tiny and the build fast.
 *
 * Memory: every snapshot owns its `path` and the `detail` field of each
 * issue. report_summary_free walks records_buf and frees them.
 */

#define _GNU_SOURCE

#include "report.h"
#include "tags.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Snapshot of a check_result for JSON emission. The `code` strings are
 * static literals from check.c — never freed here. */
struct snapshot {
    char *path;                       /* owned strdup */
    enum audio_format format;
    bool any_fail;
    bool any_flag;
    bool tag_read_failed;

    struct check_issue *issues;       /* owned array; details strdup'd */
    size_t issue_count;
};

/* Map audio_format to the JSON / text identifier. */
static const char *fmt_id(enum audio_format f)
{
    switch (f) {
    case AUDIO_FORMAT_MP3:        return "mp3";
    case AUDIO_FORMAT_FLAC:       return "flac";
    case AUDIO_FORMAT_OPUS:       return "opus";
    case AUDIO_FORMAT_OGG_VORBIS: return "ogg";
    case AUDIO_FORMAT_MP4:        return "mp4";
    default:                      return "unknown";
    }
}

static const char *severity_id(enum check_severity s)
{
    switch (s) {
    case CHECK_FAIL: return "fail";
    case CHECK_FLAG: return "flag";
    case CHECK_OK:   return "ok";
    default:         return "unknown";
    }
}

void report_summary_init(struct report_summary *summary,
                         enum report_format fmt, FILE *out)
{
    if (!summary) return;
    memset(summary, 0, sizeof(*summary));
    summary->format = fmt;
    summary->out = out;
}

/* Push a fresh snapshot onto records_buf, growing as needed. Returns NULL
 * on OOM. */
static struct snapshot *snapshots_push(struct report_summary *s)
{
    struct snapshot *vec = (struct snapshot *)s->records_buf;
    if (s->records_buf_count == s->records_buf_capacity) {
        size_t newcap = s->records_buf_capacity == 0 ? 32
                                                     : s->records_buf_capacity * 2;
        struct snapshot *grown = realloc(vec, newcap * sizeof(*grown));
        if (!grown) return NULL;
        vec = grown;
        s->records_buf = vec;
        s->records_buf_capacity = newcap;
    }
    struct snapshot *snap = &vec[s->records_buf_count++];
    memset(snap, 0, sizeof(*snap));
    return snap;
}

/* Stream the TEXT-mode per-file block (only called for failed/flagged files). */
static void text_emit_record(FILE *out, const struct check_result *cr)
{
    if (!cr || !cr->rec) return;
    fprintf(out, "%s\n", cr->rec->path ? cr->rec->path : "(null)");
    for (size_t i = 0; i < cr->issue_count; i++) {
        const struct check_issue *iss = &cr->issues[i];
        const char *prefix = (iss->severity == CHECK_FAIL) ? "FAIL"
                           : (iss->severity == CHECK_FLAG) ? "FLAG"
                           : "OK";
        fprintf(out, "  %s: %s — %s\n",
                prefix,
                iss->code ? iss->code : "(null)",
                iss->detail ? iss->detail : "");
    }
}

/* Deep-copy a check_result into a snapshot for later JSON emission. */
static void json_snapshot(struct report_summary *s,
                          const struct check_result *cr)
{
    struct snapshot *snap = snapshots_push(s);
    if (!snap) return;
    if (cr->rec && cr->rec->path) snap->path = strdup(cr->rec->path);
    snap->format = cr->rec ? cr->rec->format : AUDIO_FORMAT_UNKNOWN;
    snap->any_fail = cr->any_fail;
    snap->any_flag = cr->any_flag;
    snap->tag_read_failed = cr->tag_read_failed;

    if (cr->issue_count > 0) {
        snap->issues = calloc(cr->issue_count, sizeof(*snap->issues));
        if (snap->issues) {
            snap->issue_count = cr->issue_count;
            for (size_t i = 0; i < cr->issue_count; i++) {
                snap->issues[i].severity = cr->issues[i].severity;
                snap->issues[i].field    = cr->issues[i].field;
                snap->issues[i].code     = cr->issues[i].code; /* static literal */
                snap->issues[i].detail   = cr->issues[i].detail
                                            ? strdup(cr->issues[i].detail)
                                            : NULL;
            }
        }
    }
}

void report_add(struct report_summary *summary, const struct check_result *cr)
{
    if (!summary || !cr) return;

    summary->files_seen++;
    if (cr->any_fail || cr->tag_read_failed) {
        summary->files_failed++;
    } else if (cr->any_flag) {
        summary->files_flagged_only++;
    } else {
        summary->files_passed++;
    }
    if (cr->tag_read_failed) summary->files_taglib_open_failed++;

    /* Field-level miss counters. */
    for (size_t i = 0; i < cr->issue_count; i++) {
        const struct check_issue *iss = &cr->issues[i];
        if (iss->severity == CHECK_FAIL && iss->code &&
            strcmp(iss->code, "missing_field") == 0 &&
            iss->field >= 0 && iss->field < FIELD_COUNT) {
            summary->per_field_missing[iss->field]++;
        }
    }

    if (summary->format == REPORT_FORMAT_TEXT) {
        if (cr->any_fail || cr->any_flag || cr->tag_read_failed) {
            text_emit_record(summary->out, cr);
        }
    } else if (summary->format == REPORT_FORMAT_JSON) {
        json_snapshot(summary, cr);
    }
}

/* JSON helpers. Hand-roll: no jansson in this binary. */

/* Escape a UTF-8 string into out as a JSON string literal (without the
 * surrounding quotes — caller writes those). Replaces invalid UTF-8 starts
 * with U+FFFD encoded as �. Conservative: we re-encode as \uXXXX for
 * any byte < 0x20, and pass through valid UTF-8 multibyte sequences. */
static void json_escape(FILE *out, const char *s)
{
    if (!s) return;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned char c = *p;
        if (c == '"')        { fputs("\\\"", out); p++; continue; }
        if (c == '\\')       { fputs("\\\\", out); p++; continue; }
        if (c == '\n')       { fputs("\\n", out);  p++; continue; }
        if (c == '\r')       { fputs("\\r", out);  p++; continue; }
        if (c == '\t')       { fputs("\\t", out);  p++; continue; }
        if (c == '\b')       { fputs("\\b", out);  p++; continue; }
        if (c == '\f')       { fputs("\\f", out);  p++; continue; }
        if (c < 0x20) {
            fprintf(out, "\\u%04x", c);
            p++;
            continue;
        }
        if (c < 0x80) {
            fputc(c, out);
            p++;
            continue;
        }

        /* Multi-byte UTF-8: validate length + continuation bytes. */
        int extra = 0;
        if      ((c & 0xE0) == 0xC0) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0) extra = 3;
        else {
            /* Invalid lead byte → emit U+FFFD and skip. */
            fputs("\\ufffd", out);
            p++;
            continue;
        }
        bool ok = true;
        for (int i = 1; i <= extra; i++) {
            if ((p[i] & 0xC0) != 0x80) { ok = false; break; }
        }
        if (!ok) {
            fputs("\\ufffd", out);
            p++;
            continue;
        }
        for (int i = 0; i <= extra; i++) {
            fputc(p[i], out);
        }
        p += extra + 1;
    }
}

static void json_field_name(FILE *out, enum canonical_field f)
{
    if ((int)f < 0 || f >= FIELD_COUNT) {
        fputs("null", out);
        return;
    }
    fputc('"', out);
    fputs(canonical_field_name(f), out);
    fputc('"', out);
}

static void emit_text_footer(struct report_summary *s,
                             const struct walk_stats *ws)
{
    FILE *o = s->out;
    fputs("----\n", o);
    fprintf(o, "Walked: %zu files in %zu dirs (skipped %zu non-audio, "
               "%zu dotfiles)\n",
            ws ? ws->files_seen_total : s->files_seen,
            ws ? ws->dirs_visited : 0,
            ws ? ws->skipped_non_audio : 0,
            ws ? ws->skipped_dotfiles : 0);
    /* Pass percentage: protect against /0. */
    double pct = 0.0;
    if (s->files_seen > 0) {
        pct = 100.0 * (double)s->files_passed / (double)s->files_seen;
    }
    fprintf(o, "Passed: %zu (%.1f%%)\n", s->files_passed, pct);
    fprintf(o, "Flagged (advisory, multi-value): %zu\n",
            s->files_flagged_only);
    fprintf(o, "Failed (canonical schema): %zu\n", s->files_failed);
    for (enum canonical_field f = FIELD_TITLE; f < FIELD_COUNT; f++) {
        if (s->per_field_missing[f] > 0) {
            fprintf(o, "  missing %s: %zu\n",
                    canonical_field_name(f), s->per_field_missing[f]);
        }
    }
    if (s->files_taglib_open_failed > 0) {
        fprintf(o, "  taglib open failed: %zu\n", s->files_taglib_open_failed);
    }
    /* main() owns the exit-code resolution (it knows about --quarantine and
     * moved_count). The reporter only summarises check results. */
}

static void emit_json(struct report_summary *s, const struct walk_stats *ws)
{
    FILE *o = s->out;
    fputs("{\n", o);

    /* Summary block. */
    fputs("  \"summary\": {\n", o);
    fprintf(o, "    \"files_seen\": %zu,\n",            s->files_seen);
    fprintf(o, "    \"files_passed\": %zu,\n",          s->files_passed);
    fprintf(o, "    \"files_flagged_only\": %zu,\n",    s->files_flagged_only);
    fprintf(o, "    \"files_failed\": %zu,\n",          s->files_failed);
    fprintf(o, "    \"files_taglib_open_failed\": %zu,\n",
            s->files_taglib_open_failed);
    fputs("    \"per_field_missing\": {\n", o);
    for (enum canonical_field f = FIELD_TITLE; f < FIELD_COUNT; f++) {
        fprintf(o, "      \"%s\": %zu%s\n",
                canonical_field_name(f),
                s->per_field_missing[f],
                (f + 1 < FIELD_COUNT) ? "," : "");
    }
    fputs("    }\n", o);
    fputs("  },\n", o);

    /* Walk stats block. */
    fputs("  \"walk\": {\n", o);
    fprintf(o, "    \"dirs_visited\": %zu,\n",
            ws ? ws->dirs_visited : 0);
    fprintf(o, "    \"files_seen_total\": %zu,\n",
            ws ? ws->files_seen_total : 0);
    fprintf(o, "    \"audio_files_seen\": %zu,\n",
            ws ? ws->audio_files_seen : 0);
    fprintf(o, "    \"skipped_non_audio\": %zu,\n",
            ws ? ws->skipped_non_audio : 0);
    fprintf(o, "    \"skipped_dotfiles\": %zu,\n",
            ws ? ws->skipped_dotfiles : 0);
    fprintf(o, "    \"skipped_symlinks_outside_root\": %zu,\n",
            ws ? ws->skipped_symlinks_outside_root : 0);
    fprintf(o, "    \"taglib_open_failures\": %zu\n",
            ws ? ws->taglib_open_failures : 0);
    fputs("  },\n", o);

    /* Tracks array. */
    fputs("  \"tracks\": [\n", o);
    const struct snapshot *vec = (const struct snapshot *)s->records_buf;
    for (size_t i = 0; i < s->records_buf_count; i++) {
        const struct snapshot *snap = &vec[i];
        fputs("    {\n", o);
        fputs("      \"path\": \"", o);
        json_escape(o, snap->path ? snap->path : "");
        fputs("\",\n", o);
        fprintf(o, "      \"format\": \"%s\",\n", fmt_id(snap->format));
        fprintf(o, "      \"any_fail\": %s,\n",
                snap->any_fail ? "true" : "false");
        fprintf(o, "      \"any_flag\": %s,\n",
                snap->any_flag ? "true" : "false");
        fprintf(o, "      \"tag_read_failed\": %s,\n",
                snap->tag_read_failed ? "true" : "false");
        fputs("      \"issues\": [", o);
        for (size_t j = 0; j < snap->issue_count; j++) {
            const struct check_issue *iss = &snap->issues[j];
            if (j > 0) fputc(',', o);
            fputs("\n        { ", o);
            fprintf(o, "\"severity\": \"%s\", ", severity_id(iss->severity));
            fputs("\"code\": \"", o);
            json_escape(o, iss->code ? iss->code : "");
            fputs("\", ", o);
            fputs("\"field\": ", o);
            json_field_name(o, iss->field);
            fputs(", ", o);
            fputs("\"detail\": \"", o);
            json_escape(o, iss->detail ? iss->detail : "");
            fputs("\" }", o);
        }
        if (snap->issue_count > 0) fputs("\n      ", o);
        fputs("]\n", o);
        fprintf(o, "    }%s\n", (i + 1 < s->records_buf_count) ? "," : "");
    }
    fputs("  ]\n", o);

    fputs("}\n", o);
}

void report_emit(struct report_summary *summary,
                 const struct walk_stats *walk_stats)
{
    if (!summary || !summary->out) return;
    if (summary->format == REPORT_FORMAT_TEXT) {
        emit_text_footer(summary, walk_stats);
    } else if (summary->format == REPORT_FORMAT_JSON) {
        emit_json(summary, walk_stats);
    }
}

void report_summary_free(struct report_summary *summary)
{
    if (!summary) return;
    struct snapshot *vec = (struct snapshot *)summary->records_buf;
    if (vec) {
        for (size_t i = 0; i < summary->records_buf_count; i++) {
            free(vec[i].path);
            if (vec[i].issues) {
                for (size_t j = 0; j < vec[i].issue_count; j++) {
                    free(vec[i].issues[j].detail);
                }
                free(vec[i].issues);
            }
        }
        free(vec);
    }
    summary->records_buf = NULL;
    summary->records_buf_count = 0;
    summary->records_buf_capacity = 0;
}
