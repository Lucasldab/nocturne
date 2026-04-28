#ifndef NOCTURNE_NOCTURNED_DISKCHECK_H
#define NOCTURNE_NOCTURNED_DISKCHECK_H

/*
 * diskcheck.h — TUNE-02 disk-margin probe (read-only).
 *
 * `nocturned diskcheck` reports two margins for the configured library
 * mount and exits non-zero when either is below DISKCHECK_MIN_MARGIN_BYTES
 * (1 GiB):
 *
 *   margin_above_cap   = mount_avail - cfg.cap_bytes
 *   margin_above_floor = mount_avail - syncthing_min_floor_bytes
 *
 * `syncthing_min_floor_bytes` is derived from Syncthing's options REST
 * (`GET /rest/config/options` → `minHomeDiskFree.{value,unit}`); when
 * Syncthing is unreachable the floor is reported as null and exit code
 * 2 (degraded — cannot determine) is returned instead of 0/1.
 *
 * The probe is purely observational:
 *   - no DB writes, no manifest writes, no Syncthing config mutations
 *   - no rotation, no resolver pass
 *   - no single-writer lock acquisition (mirrors `nocturned doctor`)
 *
 * The 1 GiB threshold is the constant DISKCHECK_MIN_MARGIN_BYTES so the
 * acceptance rubric (Plan 08-04) cites the same number.
 */

#include <stddef.h>
#include <stdio.h>

/* Forward-declare so callers don't need to pull config.h into their TUs. */
struct nocturne_config;

/* TUNE-02 minimum margin: 1 GiB above cap AND above Syncthing floor.
 * Binary GiB (2^30 = 1073741824), as the rubric is phrased in "1 GB
 * binary" terms and matches the daemon's other byte-tracking code. */
#define DISKCHECK_MIN_MARGIN_BYTES (1LL << 30)

struct diskcheck_report {
    char *library_root;             /* owned; from cfg.library_root */
    long long mount_avail_bytes;    /* -1 if statvfs failed */
    long long mount_total_bytes;    /* -1 if statvfs failed */
    long long cap_bytes;            /* from cfg.cap_bytes */
    long long cap_effective_bytes;  /* cfg.cap_bytes * cap_effective_ratio (informational) */
    long long syncthing_floor_bytes;/* -1 if Syncthing degraded */

    long long min_margin_required;  /* DISKCHECK_MIN_MARGIN_BYTES */
    /* margins use a sentinel because they may legitimately be negative
     * (when avail < cap or avail < floor — both flag "unsafe").
     * LLONG_MIN means "not computed because an input was unknown". */
    long long margin_above_cap;
    long long margin_above_floor;

    int cap_safe;     /* 1 if margin_above_cap   >= MIN_MARGIN, else 0 */
    int floor_safe;   /* 1 if margin_above_floor >= MIN_MARGIN, else 0 */
    int degraded;     /* 1 if any input was unavailable (statvfs or Syncthing) */
};

/* Parse a Syncthing /rest/config/options response and convert
 * minHomeDiskFree to bytes given the volume's total size.
 *
 *   body          NUL-terminated UTF-8 JSON (truncated bodies allowed —
 *                 invalid JSON triggers degraded path)
 *   total_bytes   Volume size from statvfs(library_root); used for the
 *                 "%" unit. Pass -1 if unknown — % parse will fall back
 *                 to 0 (caller treats as fully degraded).
 *   out_floor     Filled with bytes; not written on error returns
 *
 * Returns:
 *    0  success
 *   -1  json_loads failed, unknown unit, or value not numeric
 */
int diskcheck_parse_options(const char *body,
                            long long total_bytes,
                            long long *out_floor);

/* Populate *r from cfg + statvfs(cfg->library_root) + the (optional)
 * Syncthing options body. If `options_body` is NULL or empty, the
 * floor is treated as unknown and r->degraded = 1.
 *
 * Returns 0 on success (report populated even when degraded);
 * -1 on hard error (cfg or r NULL). */
int diskcheck_collect(const struct nocturne_config *cfg,
                      const char *options_body,
                      struct diskcheck_report *r);

void diskcheck_report_free(struct diskcheck_report *r);

void diskcheck_print_text(const struct diskcheck_report *r, FILE *f);
void diskcheck_print_json(const struct diskcheck_report *r, FILE *f);

/* Test seam: override the path passed to statvfs. Set to NULL to clear.
 * Used by tests/test_disk_margin.c to exercise the degraded-statvfs
 * path without rewriting the production path. */
void diskcheck_set_statvfs_override_path(const char *path);

#endif /* NOCTURNE_NOCTURNED_DISKCHECK_H */
