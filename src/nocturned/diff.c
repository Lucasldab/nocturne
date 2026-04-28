/*
 * diff.c — manifest diff formatter for `nocturned resolve --dry-run --diff`.
 *
 * Pure read-only consumer of `manifest_current` (SQLite) and the freshly-
 * resolved in-memory `struct manifest`. Produces ADDED / REMOVED /
 * SHIFTED-BUCKETS / NET / USED sections in either text or single-line JSON.
 *
 * Design:
 *   - Read prev rows (sha256, buckets_csv, size_bytes) from manifest_current.
 *     Already physically sorted by sha256 (PRIMARY KEY); we use
 *     `ORDER BY sha256` defensively.
 *   - The next-state `struct manifest` is already sorted by sha256 (resolver
 *     post-condition; tests/test_resolver.c locks this).
 *   - Walk both sorted arrays in lockstep — three-way merge:
 *       prev only  -> REMOVED
 *       next only  -> ADDED
 *       both, csv differs -> SHIFTED (with set diff for +gained / -lost)
 *       both, csv equal   -> unchanged (skip)
 *   - used_before is read from manifest_meta.key='used_bytes' (0 if absent
 *     -- first run); used_after is the resolver's freshly-computed total.
 *
 * Threat: T-08-06 (--diff accidentally writes) — this function only ever
 * SELECTs. There are zero INSERT/UPDATE/DELETE statements in this file;
 * verified by grep gate in plan acceptance.
 */

#define _GNU_SOURCE

#include "diff.h"

#include "db.h"
#include "resolver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

/* MiB conversion: round to nearest, headroom-friendly. */
#define BYTES_TO_MIB(b) (((long long)(b) + 524288LL) / 1048576LL)

/* ---- Local representations of the prev/next sets ---- */

struct prev_track {
    char *sha256;
    char *buckets_csv;
    long long size_bytes;
};

struct prev_set {
    struct prev_track *items;
    size_t count;
    size_t cap;
};

static int prev_load(struct sqlite3 *raw, struct prev_set *out)
{
    out->items = NULL; out->count = 0; out->cap = 0;
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(raw,
        "SELECT sha256, COALESCE(buckets_csv, ''), size_bytes "
        "FROM manifest_current ORDER BY sha256",
        -1, &st, NULL);
    if (rc != SQLITE_OK) return -1;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (out->count == out->cap) {
            size_t ncap = out->cap ? out->cap * 2 : 64;
            struct prev_track *grown = realloc(out->items, ncap * sizeof(*grown));
            if (!grown) { sqlite3_finalize(st); return -1; }
            out->items = grown; out->cap = ncap;
        }
        struct prev_track *p = &out->items[out->count++];
        p->sha256 = strdup((const char *) sqlite3_column_text(st, 0));
        p->buckets_csv = strdup((const char *) sqlite3_column_text(st, 1));
        p->size_bytes = sqlite3_column_int64(st, 2);
    }
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

static void prev_free(struct prev_set *s)
{
    for (size_t i = 0; i < s->count; i++) {
        free(s->items[i].sha256);
        free(s->items[i].buckets_csv);
    }
    free(s->items);
    s->items = NULL; s->count = 0; s->cap = 0;
}

/* Read used_before from manifest_meta. Returns 0 if absent (first run). */
static long long read_used_before(struct sqlite3 *raw)
{
    sqlite3_stmt *st = NULL;
    long long out = 0;
    if (sqlite3_prepare_v2(raw,
        "SELECT value FROM manifest_meta WHERE key='used_bytes'",
        -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *v = sqlite3_column_text(st, 0);
            if (v) out = atoll((const char *) v);
        }
        sqlite3_finalize(st);
    }
    return out;
}

/* ---- buckets_csv vs string-array helpers ---- */

/* Join a sorted-alphabetical buckets[] array (resolver invariant) into a
 * comma-separated CSV. Caller owns the heap buffer. */
static char *buckets_to_csv(const char *const *buckets, size_t n)
{
    size_t total = 1; /* trailing NUL */
    for (size_t i = 0; i < n; i++) total += strlen(buckets[i]) + (i ? 1 : 0);
    char *buf = malloc(total);
    if (!buf) return NULL;
    buf[0] = '\0';
    size_t cur = 0;
    for (size_t i = 0; i < n; i++) {
        if (i) buf[cur++] = ',';
        size_t bn = strlen(buckets[i]);
        memcpy(buf + cur, buckets[i], bn);
        cur += bn;
    }
    buf[cur] = '\0';
    return buf;
}

/* Split a CSV into a NULL-terminated array of dup'd strings. Caller frees
 * each element + the array. *out_n receives the count. Returns NULL on OOM. */
static char **csv_split(const char *csv, size_t *out_n)
{
    *out_n = 0;
    if (!csv || !*csv) {
        char **arr = calloc(1, sizeof(*arr));
        return arr;
    }
    size_t n = 1;
    for (const char *p = csv; *p; p++) if (*p == ',') n++;
    char **arr = calloc(n + 1, sizeof(*arr));
    if (!arr) return NULL;
    size_t idx = 0;
    const char *start = csv;
    for (const char *p = csv; ; p++) {
        if (*p == ',' || *p == '\0') {
            size_t len = (size_t)(p - start);
            arr[idx] = malloc(len + 1);
            if (!arr[idx]) {
                for (size_t i = 0; i < idx; i++) free(arr[i]);
                free(arr);
                return NULL;
            }
            memcpy(arr[idx], start, len);
            arr[idx][len] = '\0';
            idx++;
            if (*p == '\0') break;
            start = p + 1;
        }
    }
    *out_n = idx;
    return arr;
}

static void str_array_free(char **arr, size_t n)
{
    if (!arr) return;
    for (size_t i = 0; i < n; i++) free(arr[i]);
    free(arr);
}

/* Compute set difference: items in `a` not in `b`. Both are sorted-
 * alphabetical (resolver invariant + comma-csv-stored-sorted). Returns
 * a NULL-terminated array of malloc'd strings (never NULL elements). */
static char **set_diff(char **a, size_t an, char **b, size_t bn, size_t *out_n)
{
    char **out = calloc(an + 1, sizeof(*out));
    if (!out) { *out_n = 0; return NULL; }
    size_t cnt = 0;
    for (size_t i = 0; i < an; i++) {
        int found = 0;
        for (size_t j = 0; j < bn; j++) {
            if (strcmp(a[i], b[j]) == 0) { found = 1; break; }
        }
        if (!found) {
            out[cnt] = strdup(a[i]);
            if (!out[cnt]) {
                for (size_t k = 0; k < cnt; k++) free(out[k]);
                free(out);
                *out_n = 0;
                return NULL;
            }
            cnt++;
        }
    }
    *out_n = cnt;
    return out;
}

/* ---- Output helpers ---- */

static void print_text_track_line(FILE *out, const char *sha,
                                  const char *buckets_csv, long long size_bytes)
{
    fprintf(out, "  %.12s  %s  %lld MiB\n",
            sha, buckets_csv, BYTES_TO_MIB(size_bytes));
}

static void print_text_shifted_line(FILE *out, const char *sha,
                                    char **gained, size_t gn,
                                    char **lost, size_t ln)
{
    fprintf(out, "  %.12s ", sha);
    for (size_t i = 0; i < gn; i++) fprintf(out, " +%s", gained[i]);
    for (size_t i = 0; i < ln; i++) fprintf(out, " -%s", lost[i]);
    fprintf(out, "\n");
}

/* Hand-rolled JSON-string emit: escapes ", \\, control chars. Track ids
 * and bucket names are ASCII alnum/underscore by construction (sha256 hex,
 * resolver bucket whitelist) — no surrogate pairs to worry about. */
static void emit_json_str(FILE *out, const char *s)
{
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *) s; *p; p++) {
        switch (*p) {
        case '"':  fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\n': fputs("\\n",  out); break;
        case '\t': fputs("\\t",  out); break;
        case '\r': fputs("\\r",  out); break;
        default:
            if (*p < 0x20) fprintf(out, "\\u%04x", *p);
            else fputc(*p, out);
        }
    }
    fputc('"', out);
}

static void emit_json_str_array(FILE *out, char **a, size_t n)
{
    fputc('[', out);
    for (size_t i = 0; i < n; i++) {
        if (i) fputc(',', out);
        emit_json_str(out, a[i]);
    }
    fputc(']', out);
}

/* ---- Diff entry record (built up during the merge walk) ---- */

struct diff_added {
    const char *sha256;        /* borrowed from next */
    long long size_bytes;
    char *buckets_csv;         /* heap; freed at end */
    char **buckets;            /* heap (array of borrows from next) */
    size_t buckets_n;
};

struct diff_removed {
    const char *sha256;        /* borrowed from prev */
    long long size_bytes;
    const char *buckets_csv;   /* borrowed from prev */
    char **buckets;            /* heap split */
    size_t buckets_n;
};

struct diff_shifted {
    const char *sha256;        /* borrowed from next (== prev) */
    char **gained;             /* heap */
    size_t gained_n;
    char **lost;               /* heap */
    size_t lost_n;
};

/* ---- Main entry point ---- */

int print_resolve_diff(struct nocturne_db *db,
                       const struct manifest *next,
                       int as_json,
                       FILE *out)
{
    if (!db || !next || !out) return -1;
    struct sqlite3 *raw = db_handle(db);

    struct prev_set prev = {0};
    if (prev_load(raw, &prev) != 0) {
        prev_free(&prev);
        return -1;
    }
    long long used_before = read_used_before(raw);

    /* Build flat lists: added, removed, shifted. Worst case sizes bounded by
     * prev.count + next->resident_n. */
    size_t bound = prev.count + next->resident_n + 1;
    struct diff_added   *adds = calloc(bound, sizeof(*adds));
    struct diff_removed *rems = calloc(bound, sizeof(*rems));
    struct diff_shifted *shifts = calloc(bound, sizeof(*shifts));
    size_t na = 0, nr = 0, ns = 0;
    long long added_bytes = 0;
    long long removed_bytes = 0;

    if (!adds || !rems || !shifts) {
        free(adds); free(rems); free(shifts);
        prev_free(&prev);
        return -1;
    }

    /* Three-way merge on sha256-sorted arrays. */
    size_t pi = 0, ni = 0;
    while (pi < prev.count && ni < next->resident_n) {
        const struct prev_track *p = &prev.items[pi];
        const struct manifest_track *n = &next->resident[ni];
        int cmp = strcmp(p->sha256, n->sha256);
        if (cmp < 0) {
            /* prev only -> REMOVED */
            char **bs; size_t bn;
            bs = csv_split(p->buckets_csv, &bn);
            rems[nr].sha256 = p->sha256;
            rems[nr].size_bytes = p->size_bytes;
            rems[nr].buckets_csv = p->buckets_csv;
            rems[nr].buckets = bs;
            rems[nr].buckets_n = bn;
            removed_bytes += p->size_bytes;
            nr++;
            pi++;
        } else if (cmp > 0) {
            /* next only -> ADDED */
            char *csv = buckets_to_csv((const char *const *) n->buckets, n->buckets_n);
            adds[na].sha256 = n->sha256;
            adds[na].size_bytes = n->size_bytes;
            adds[na].buckets_csv = csv;
            adds[na].buckets = n->buckets;
            adds[na].buckets_n = n->buckets_n;
            added_bytes += n->size_bytes;
            na++;
            ni++;
        } else {
            /* both -> compare buckets_csv */
            char *csv_n = buckets_to_csv((const char *const *) n->buckets, n->buckets_n);
            if (csv_n && strcmp(p->buckets_csv, csv_n) == 0) {
                /* unchanged */
                free(csv_n);
            } else {
                /* SHIFTED */
                size_t pn = 0;
                char **prev_b = csv_split(p->buckets_csv, &pn);
                size_t gn = 0, ln = 0;
                char **gained = set_diff(n->buckets, n->buckets_n,
                                         prev_b, pn, &gn);
                char **lost   = set_diff(prev_b, pn,
                                         n->buckets, n->buckets_n, &ln);
                shifts[ns].sha256 = n->sha256;
                shifts[ns].gained = gained;
                shifts[ns].gained_n = gn;
                shifts[ns].lost = lost;
                shifts[ns].lost_n = ln;
                ns++;
                str_array_free(prev_b, pn);
                free(csv_n);
            }
            pi++;
            ni++;
        }
    }
    while (pi < prev.count) {
        const struct prev_track *p = &prev.items[pi];
        char **bs; size_t bn;
        bs = csv_split(p->buckets_csv, &bn);
        rems[nr].sha256 = p->sha256;
        rems[nr].size_bytes = p->size_bytes;
        rems[nr].buckets_csv = p->buckets_csv;
        rems[nr].buckets = bs;
        rems[nr].buckets_n = bn;
        removed_bytes += p->size_bytes;
        nr++;
        pi++;
    }
    while (ni < next->resident_n) {
        const struct manifest_track *n = &next->resident[ni];
        char *csv = buckets_to_csv((const char *const *) n->buckets, n->buckets_n);
        adds[na].sha256 = n->sha256;
        adds[na].size_bytes = n->size_bytes;
        adds[na].buckets_csv = csv;
        adds[na].buckets = n->buckets;
        adds[na].buckets_n = n->buckets_n;
        added_bytes += n->size_bytes;
        na++;
        ni++;
    }

    long long net_tracks = (long long) na - (long long) nr;
    long long net_bytes  = added_bytes - removed_bytes;
    long long used_after = next->used_bytes;

    /* ---- Emit ---- */

    if (as_json) {
        fputc('{', out);

        fputs("\"added\":[", out);
        for (size_t i = 0; i < na; i++) {
            if (i) fputc(',', out);
            fputc('{', out);
            fputs("\"id\":", out);
            emit_json_str(out, adds[i].sha256);
            fputs(",\"buckets\":", out);
            emit_json_str_array(out, adds[i].buckets, adds[i].buckets_n);
            fprintf(out, ",\"size_bytes\":%lld", adds[i].size_bytes);
            fputc('}', out);
        }
        fputs("],", out);

        fputs("\"removed\":[", out);
        for (size_t i = 0; i < nr; i++) {
            if (i) fputc(',', out);
            fputc('{', out);
            fputs("\"id\":", out);
            emit_json_str(out, rems[i].sha256);
            fputs(",\"buckets\":", out);
            emit_json_str_array(out, rems[i].buckets, rems[i].buckets_n);
            fprintf(out, ",\"size_bytes\":%lld", rems[i].size_bytes);
            fputc('}', out);
        }
        fputs("],", out);

        fputs("\"shifted\":[", out);
        for (size_t i = 0; i < ns; i++) {
            if (i) fputc(',', out);
            fputc('{', out);
            fputs("\"id\":", out);
            emit_json_str(out, shifts[i].sha256);
            fputs(",\"added_buckets\":", out);
            emit_json_str_array(out, shifts[i].gained, shifts[i].gained_n);
            fputs(",\"removed_buckets\":", out);
            emit_json_str_array(out, shifts[i].lost, shifts[i].lost_n);
            fputc('}', out);
        }
        fputs("],", out);

        fprintf(out, "\"net_tracks\":%lld,",          net_tracks);
        fprintf(out, "\"net_bytes\":%lld,",           net_bytes);
        fprintf(out, "\"used_before\":%lld,",         used_before);
        fprintf(out, "\"used_after\":%lld,",          used_after);
        fprintf(out, "\"cap_effective_bytes\":%lld",  next->cap_effective_bytes);

        fputc('}', out);
        fputc('\n', out);
    } else {
        fprintf(out, "ADDED: %zu tracks, +%lld MiB\n",
                na, BYTES_TO_MIB(added_bytes));
        for (size_t i = 0; i < na; i++) {
            print_text_track_line(out, adds[i].sha256,
                                  adds[i].buckets_csv ? adds[i].buckets_csv : "",
                                  adds[i].size_bytes);
        }

        fprintf(out, "REMOVED: %zu tracks, -%lld MiB\n",
                nr, BYTES_TO_MIB(removed_bytes));
        for (size_t i = 0; i < nr; i++) {
            print_text_track_line(out, rems[i].sha256,
                                  rems[i].buckets_csv ? rems[i].buckets_csv : "",
                                  rems[i].size_bytes);
        }

        fprintf(out,
            "SHIFTED BUCKETS: %zu tracks (still resident, attribution changed)\n",
            ns);
        for (size_t i = 0; i < ns; i++) {
            print_text_shifted_line(out, shifts[i].sha256,
                                    shifts[i].gained, shifts[i].gained_n,
                                    shifts[i].lost,   shifts[i].lost_n);
        }

        fprintf(out, "NET: %+lld tracks, %+lld MiB\n",
                net_tracks, BYTES_TO_MIB(net_bytes));
        fprintf(out,
            "USED:  %lld MiB -> %lld MiB  (cap_effective %lld MiB)\n",
            BYTES_TO_MIB(used_before), BYTES_TO_MIB(used_after),
            BYTES_TO_MIB(next->cap_effective_bytes));
    }

    /* ---- Free ---- */
    for (size_t i = 0; i < na; i++) {
        free(adds[i].buckets_csv);
    }
    for (size_t i = 0; i < nr; i++) {
        str_array_free(rems[i].buckets, rems[i].buckets_n);
    }
    for (size_t i = 0; i < ns; i++) {
        str_array_free(shifts[i].gained, shifts[i].gained_n);
        str_array_free(shifts[i].lost,   shifts[i].lost_n);
    }
    free(adds); free(rems); free(shifts);
    prev_free(&prev);
    return 0;
}
