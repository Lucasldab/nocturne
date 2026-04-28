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
 * Threat anchors (see plan 08-03 <threat_model>):
 *   T-08-12 Malformed options JSON → return -1 → caller degrades
 *   T-08-14 Pathological options body → caller buffer-bound, parse-bound
 *   T-08-17 Read-only contract  → no lock, no DB, verified by grep
 */

#define _GNU_SOURCE

#include "diskcheck.h"
#include "config.h"

#include <errno.h>
#include <jansson.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>

/* === module state ======================================================== */

static const char *g_statvfs_override = NULL;

void diskcheck_set_statvfs_override_path(const char *path)
{
    g_statvfs_override = path;
}

/* === options parser ====================================================== */

int diskcheck_parse_options(const char *body,
                            long long total_bytes,
                            long long *out_floor)
{
    (void) body; (void) total_bytes; (void) out_floor;
    return -1;  /* RED stub: real impl in GREEN commit */
}

/* === collect ============================================================= */

int diskcheck_collect(const struct nocturne_config *cfg,
                      const char *options_body,
                      struct diskcheck_report *r)
{
    (void) cfg; (void) options_body;
    if (!r) return -1;
    memset(r, 0, sizeof(*r));
    r->mount_avail_bytes = -1;
    r->mount_total_bytes = -1;
    r->syncthing_floor_bytes = -1;
    r->margin_above_cap = LLONG_MIN;
    r->margin_above_floor = LLONG_MIN;
    r->min_margin_required = DISKCHECK_MIN_MARGIN_BYTES;
    r->degraded = 1;
    return 0;  /* RED stub: real impl in GREEN commit */
}

void diskcheck_report_free(struct diskcheck_report *r)
{
    if (!r) return;
    free(r->library_root);
    memset(r, 0, sizeof(*r));
}

/* === renderers (RED stubs — emit minimal scaffolding so callers link) ==== */

void diskcheck_print_text(const struct diskcheck_report *r, FILE *f)
{
    if (!r || !f) return;
    fprintf(f, "nocturned diskcheck (stub)\n");
}

void diskcheck_print_json(const struct diskcheck_report *r, FILE *f)
{
    if (!r || !f) return;
    fprintf(f, "{}\n");
}
