/*
 * diskcheck.c — TUNE-02 disk-margin probe data collection + renderers.
 *
 * Pure data collection: diskcheck_collect populates a struct
 * diskcheck_report by statvfs'ing the library mount and parsing the
 * (optional) Syncthing /rest/config/options response body for
 * minHomeDiskFree. The two `_print_` functions render that struct as
 * text or single-line JSON.
 *
 * No outbound network surface here — the libcurl call lives in
 * syncthing_api.c (CROSS-03 invariant: only that file links libcurl).
 * No DB connection; no lock acquisition; no manifest writes.
 *
 * Unit conversion follows Syncthing's own SI base-1000 multipliers as
 * documented at docs.syncthing.net/users/config.html#min-disk-free —
 * matches the numbers shown in Syncthing's GUI verbatim. The "%" unit
 * is the only one that depends on volume size (% of total).
 *
 * Threat anchors (see plan 08-03 <threat_model>):
 *   T-08-12 Malformed options JSON → return -1 → caller degrades
 *   T-08-14 Pathological options body → caller buffer-bound, parse-bound
 *   T-08-17 Read-only contract  → no lock, no DB, verified by grep
 */

#define _GNU_SOURCE

#include "diskcheck.h"
#include "config.h"

#include <errno.h>
#include <inttypes.h>
#include <jansson.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>

/* === module state ======================================================== */

/* Test seam — when set, statvfs reads from this path instead of cfg's
 * library_root. Lets tests verify the degraded-statvfs path without
 * having to spin up a fake mount. */
static const char *g_statvfs_override = NULL;

void diskcheck_set_statvfs_override_path(const char *path)
{
    g_statvfs_override = path;
}

/* === options parser ======================================================
 *
 * Syncthing's options.minHomeDiskFree is an OBJECT with two keys:
 *   { "minHomeDiskFree": { "value": <number>, "unit": "%" | "B" | "kB" |
 *                                              "MB" | "GB" | "TB" } }
 * Default per docs.syncthing.net/users/config.html: value=1, unit="%".
 * If the whole field is missing → defaults apply. If the field is an
 * object but a sub-field is missing → that sub-field defaults too.
 *
 * Multiplier table (SI base-1000 — matches Syncthing's GUI display):
 *   "%"  → total_bytes * value / 100
 *   "B"  → value
 *   "kB" → value * 1000
 *   "MB" → value * 1000 * 1000
 *   "GB" → value * 1000 * 1000 * 1000
 *   "TB" → value * 1000 * 1000 * 1000 * 1000
 *
 * Unknown unit → -1 (caller treats as degraded).
 * Negative value → clamped to 0 (caller's margin reads as ample).
 */
int diskcheck_parse_options(const char *body,
                            long long total_bytes,
                            long long *out_floor)
{
    if (!body || !out_floor) return -1;

    json_error_t jerr;
    json_t *root = json_loads(body, 0, &jerr);
    if (!root) return -1;
    if (!json_is_object(root)) { json_decref(root); return -1; }

    /* Defaults per Syncthing 2.x docs. */
    double value = 1.0;
    const char *unit = "%";

    json_t *mhdf = json_object_get(root, "minHomeDiskFree");
    if (json_is_object(mhdf)) {
        json_t *jv = json_object_get(mhdf, "value");
        if (json_is_real(jv))         value = json_real_value(jv);
        else if (json_is_integer(jv)) value = (double) json_integer_value(jv);
        /* else: leave at default */

        json_t *ju = json_object_get(mhdf, "unit");
        if (json_is_string(ju)) unit = json_string_value(ju);
    }
    /* If mhdf is missing OR is null, defaults stand. Any other JSON type
     * for the field (array / scalar) falls back to defaults too — this
     * is a probe, not a Syncthing config validator. */

    if (value < 0.0) value = 0.0;

    long long floor_bytes;
    if (!strcmp(unit, "%")) {
        if (total_bytes < 0) total_bytes = 0;
        floor_bytes = (long long)((double) total_bytes * value / 100.0);
    } else if (!strcmp(unit, "B")) {
        floor_bytes = (long long) value;
    } else if (!strcmp(unit, "kB")) {
        floor_bytes = (long long)(value * 1000.0);
    } else if (!strcmp(unit, "MB")) {
        floor_bytes = (long long)(value * 1000000.0);
    } else if (!strcmp(unit, "GB")) {
        floor_bytes = (long long)(value * 1000000000.0);
    } else if (!strcmp(unit, "TB")) {
        floor_bytes = (long long)(value * 1000000000000.0);
    } else {
        json_decref(root);
        return -1;
    }
    if (floor_bytes < 0) floor_bytes = 0;

    *out_floor = floor_bytes;
    json_decref(root);
    return 0;
}

/* === collect ============================================================= */

int diskcheck_collect(const struct nocturne_config *cfg,
                      const char *options_body,
                      struct diskcheck_report *r)
{
    if (!r || !cfg) return -1;
    memset(r, 0, sizeof(*r));
    r->mount_avail_bytes = -1;
    r->mount_total_bytes = -1;
    r->syncthing_floor_bytes = -1;
    r->margin_above_cap = LLONG_MIN;
    r->margin_above_floor = LLONG_MIN;
    r->min_margin_required = DISKCHECK_MIN_MARGIN_BYTES;
    r->cap_safe = 0;
    r->floor_safe = 0;
    r->degraded = 0;

    if (cfg->library_root) {
        r->library_root = strdup(cfg->library_root);
    }

    /* statvfs the library mount (or the override path for tests). */
    const char *vfs_path = g_statvfs_override ? g_statvfs_override
                                              : cfg->library_root;
    if (vfs_path && *vfs_path) {
        struct statvfs vfs;
        if (statvfs(vfs_path, &vfs) == 0) {
            r->mount_total_bytes = (long long) vfs.f_blocks * (long long) vfs.f_frsize;
            r->mount_avail_bytes = (long long) vfs.f_bavail * (long long) vfs.f_frsize;
        } else {
            r->degraded = 1;   /* statvfs failed — caller exits 2 */
        }
    } else {
        /* No library_root configured — can't probe the mount at all. */
        r->degraded = 1;
    }

    r->cap_bytes = cfg->cap_bytes;
    r->cap_effective_bytes =
        (long long)((double) cfg->cap_bytes * cfg->cap_effective_ratio);

    if (options_body && *options_body) {
        long long floor = -1;
        int rc = diskcheck_parse_options(options_body,
                                         r->mount_total_bytes,
                                         &floor);
        if (rc == 0) {
            r->syncthing_floor_bytes = floor;
        } else {
            r->degraded = 1;   /* malformed body — caller exits 2 */
        }
    } else {
        r->degraded = 1;       /* no body — Syncthing unreachable */
    }

    if (r->mount_avail_bytes >= 0) {
        r->margin_above_cap = r->mount_avail_bytes - r->cap_bytes;
    }
    if (r->mount_avail_bytes >= 0 && r->syncthing_floor_bytes >= 0) {
        r->margin_above_floor = r->mount_avail_bytes - r->syncthing_floor_bytes;
    }

    r->cap_safe = (r->margin_above_cap != LLONG_MIN &&
                   r->margin_above_cap >= DISKCHECK_MIN_MARGIN_BYTES);
    r->floor_safe = (r->margin_above_floor != LLONG_MIN &&
                     r->margin_above_floor >= DISKCHECK_MIN_MARGIN_BYTES);

    return 0;
}

void diskcheck_report_free(struct diskcheck_report *r)
{
    if (!r) return;
    free(r->library_root);
    memset(r, 0, sizeof(*r));
}

/* === renderers =========================================================== */

/* Format `bytes` as "<n>.<d> GiB" (one decimal, binary GiB). Returns the
 * buffer pointer for caller's convenience. Negative or LLONG_MIN → "?". */
static const char *fmt_gib(long long bytes, char *buf, size_t cap)
{
    if (bytes == LLONG_MIN || bytes < 0) {
        snprintf(buf, cap, "?");
        return buf;
    }
    /* Binary GiB so 12 GiB cap displays as "12.0 GiB" not 12.88. */
    double gib = (double) bytes / (double)(1LL << 30);
    /* Avoid -0.0 for visual cleanliness. */
    if (gib > -0.05 && gib < 0.0) gib = 0.0;
    snprintf(buf, cap, "%.1f GiB", gib);
    return buf;
}

/* Like fmt_gib but accepts negative margins (prints sign). */
static const char *fmt_gib_signed(long long bytes, char *buf, size_t cap)
{
    if (bytes == LLONG_MIN) {
        snprintf(buf, cap, "?");
        return buf;
    }
    double gib = (double) bytes / (double)(1LL << 30);
    snprintf(buf, cap, "%+.1f GiB", gib);
    return buf;
}

void diskcheck_print_text(const struct diskcheck_report *r, FILE *f)
{
    if (!r || !f) return;
    char b1[64], b2[64], b3[64], b4[64], b5[64], b6[64], b7[64];

    fprintf(f, "nocturned diskcheck\n");
    fprintf(f, "===================\n\n");
    fprintf(f, "library_root:           %s\n",
            r->library_root ? r->library_root : "(unset)");
    fprintf(f, "mount_total:            %s\n",
            fmt_gib(r->mount_total_bytes, b1, sizeof(b1)));
    fprintf(f, "mount_avail:            %s\n\n",
            fmt_gib(r->mount_avail_bytes, b2, sizeof(b2)));
    fprintf(f, "cap_bytes:              %s\n",
            fmt_gib(r->cap_bytes, b3, sizeof(b3)));
    fprintf(f, "cap_effective:          %s  (informational)\n",
            fmt_gib(r->cap_effective_bytes, b4, sizeof(b4)));
    fprintf(f, "margin_above_cap:       %s  %s (>= %s required)\n\n",
            fmt_gib_signed(r->margin_above_cap, b5, sizeof(b5)),
            r->cap_safe ? "[OK]" : "[UNSAFE]",
            fmt_gib(r->min_margin_required, b6, sizeof(b6)));
    if (r->syncthing_floor_bytes >= 0) {
        fprintf(f, "syncthing_min_floor:    %s\n",
                fmt_gib(r->syncthing_floor_bytes, b7, sizeof(b7)));
        fprintf(f, "margin_above_floor:     %s  %s\n\n",
                fmt_gib_signed(r->margin_above_floor, b1, sizeof(b1)),
                r->floor_safe ? "[OK]" : "[UNSAFE]");
    } else {
        fprintf(f, "syncthing_min_floor:    (unknown — Syncthing unreachable)\n");
        fprintf(f, "margin_above_floor:     (unknown)\n\n");
    }

    const char *overall;
    if (r->degraded)                        overall = "DEGRADED (cannot determine)";
    else if (r->cap_safe && r->floor_safe)  overall = "SAFE";
    else                                    overall = "UNSAFE (TUNE-02 fail)";
    fprintf(f, "overall: %s\n", overall);
}

/* --- json output ---
 *
 * Hand-rolled to match doctor.c's pattern (avoids jansson alloc on a
 * read-mostly probe; output is single-line and byte-stable). Keys use
 * lower_snake_case (matches doctor.c convention).
 *
 * Unknown values render as `null`; the threshold flags render as
 * lowercase `true`/`false` (matches publish.c convention).
 */

static void emit_str_or_null(FILE *f, const char *s)
{
    if (!s) { fprintf(f, "null"); return; }
    /* No control chars expected in library_root paths; but escape
     * the bare minimum needed to keep the JSON parseable. */
    fputc('"', f);
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char) *p;
        switch (c) {
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f);  break;
        case '\r': fputs("\\r", f);  break;
        case '\t': fputs("\\t", f);  break;
        default:
            if (c < 0x20) fprintf(f, "\\u%04x", c);
            else fputc((int) c, f);
        }
    }
    fputc('"', f);
}

static void emit_ll_or_null(FILE *f, long long v, int unknown_when_neg)
{
    if (v == LLONG_MIN || (unknown_when_neg && v < 0)) {
        fprintf(f, "null");
    } else {
        fprintf(f, "%lld", v);
    }
}

void diskcheck_print_json(const struct diskcheck_report *r, FILE *f)
{
    if (!r || !f) return;
    fputc('{', f);
    fputs("\"library_root\":", f);              emit_str_or_null(f, r->library_root);
    fputs(",\"mount_avail_bytes\":", f);        emit_ll_or_null(f, r->mount_avail_bytes, 1);
    fputs(",\"mount_total_bytes\":", f);        emit_ll_or_null(f, r->mount_total_bytes, 1);
    fputs(",\"cap_bytes\":", f);                fprintf(f, "%lld", r->cap_bytes);
    fputs(",\"cap_effective_bytes\":", f);      fprintf(f, "%lld", r->cap_effective_bytes);
    fputs(",\"syncthing_floor_bytes\":", f);    emit_ll_or_null(f, r->syncthing_floor_bytes, 1);
    fputs(",\"min_margin_required_bytes\":", f);fprintf(f, "%lld", r->min_margin_required);
    fputs(",\"margin_above_cap\":", f);         emit_ll_or_null(f, r->margin_above_cap, 0);
    fputs(",\"margin_above_floor\":", f);       emit_ll_or_null(f, r->margin_above_floor, 0);
    fputs(",\"cap_safe\":", f);                 fputs(r->cap_safe   ? "true" : "false", f);
    fputs(",\"floor_safe\":", f);               fputs(r->floor_safe ? "true" : "false", f);
    fputs(",\"degraded\":", f);                 fputs(r->degraded   ? "true" : "false", f);
    fputs("}\n", f);
}
