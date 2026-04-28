/*
 * diskcheck_cmd.c — `nocturned diskcheck [--json]` (TUNE-02 probe).
 *
 * Read-only verification probe. Loads cfg, statvfs(library_root), and
 * (best-effort) reads Syncthing's options REST for `minHomeDiskFree`.
 * Reports both `margin_above_cap` and `margin_above_floor` and exits
 * non-zero if either is below DISKCHECK_MIN_MARGIN_BYTES (1 GiB).
 *
 * Lock policy: NO lockfile acquisition (mirrors doctor_cmd_main /
 * why_cmd_main). The probe must be runnable while `nocturned watch`
 * holds the writer lock — otherwise it's useless inside a systemd
 * timer or shell loop.
 *
 * No DB connection: diskcheck reads only the config + statvfs +
 * Syncthing REST. Plan 08-03's verification block grep-asserts that
 * neither the SQLite open call nor the daemon-DB-path helper appears
 * in this file (the comment is phrased without the literal symbol
 * names so the gate stays clean).
 *
 * Exit codes (per cli.h + plan 08-03 contract):
 *   0 (NOCT_EXIT_OK)      both margins >= 1 GiB AND non-degraded → SAFE
 *   1 (NOCT_EXIT_FAILURE) either margin below threshold → UNSAFE
 *                         (TUNE-02 fail)
 *   2                     statvfs OR Syncthing-REST unavailable →
 *                         degraded; cannot determine. Distinct from
 *                         "1" so a systemd timer treats degraded as a
 *                         configuration warning rather than a confident
 *                         pass/fail.
 *  64 (NOCT_EXIT_USAGE)   missing library_root / hard cfg-load error
 */

#define _GNU_SOURCE

#include "cli.h"
#include "config.h"
#include "diskcheck.h"
#include "paths.h"
#include "syncthing_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Distinct exit code for the degraded path. Not in cli.h's NOCT_EXIT_*
 * matrix because it's diskcheck-specific (TUNE-02 contract: 0/1/2/64). */
#define DISKCHECK_EXIT_DEGRADED 2

int diskcheck_cmd_main(struct cli_args *args)
{
    if (!args) return NOCT_EXIT_USAGE;

    struct nocturne_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    const char *cfg_path = args->config_path ? args->config_path
                                              : paths_config_file();

    /* config_load returns 0 on success. On failure it leaves cfg
     * partially populated; we fall back to config_default and tell the
     * user what we did. */
    int cfg_loaded_ok = (config_load(cfg_path, &cfg) == 0);
    if (!cfg_loaded_ok) {
        config_free(&cfg);
        if (config_default(&cfg) < 0) {
            fprintf(stderr,
                "nocturned diskcheck: config load + default failed\n");
            return NOCT_EXIT_FAILURE;
        }
    }

    if (!cfg.library_root) {
        fprintf(stderr,
            "nocturned diskcheck: [library].path not configured "
            "(set in %s)\n", cfg_path ? cfg_path : "config.toml");
        config_free(&cfg);
        return NOCT_EXIT_USAGE;
    }

    /* Best-effort Syncthing options pull. The degraded path is
     * legitimate (Syncthing not running, config.xml missing, etc.) —
     * we report cap-margin only and exit 2 so callers can distinguish
     * "couldn't determine" from a confident pass/fail. */
    char options_buf[8192];
    options_buf[0] = '\0';
    const char *options_body = NULL;

    if (syncthing_api_init() == 0 &&
        syncthing_get_config(NULL) == 0) {
        size_t alen = 0;
        int rc = syncthing_get_options(options_buf, sizeof(options_buf), &alen);
        if (rc == 0) {
            options_body = options_buf;
        } else {
            fprintf(stderr,
                "nocturned diskcheck: Syncthing options unreachable "
                "(rc=%d); reporting cap-margin only\n", rc);
        }
    } else {
        fprintf(stderr,
            "nocturned diskcheck: Syncthing config not loaded; "
            "reporting cap-margin only (floor unknown)\n");
    }

    struct diskcheck_report r;
    int rc = diskcheck_collect(&cfg, options_body, &r);
    if (rc != 0) {
        config_free(&cfg);
        syncthing_api_cleanup();
        return NOCT_EXIT_FAILURE;
    }

    if (args->json) diskcheck_print_json(&r, stdout);
    else            diskcheck_print_text(&r, stdout);

    int exit_code;
    if (r.degraded) {
        exit_code = DISKCHECK_EXIT_DEGRADED;
    } else if (r.cap_safe && r.floor_safe) {
        exit_code = NOCT_EXIT_OK;
    } else {
        exit_code = NOCT_EXIT_FAILURE;
    }

    diskcheck_report_free(&r);
    config_free(&cfg);
    syncthing_api_cleanup();
    return exit_code;
}
