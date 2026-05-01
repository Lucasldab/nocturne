/*
 * cycle_cmd.c — `nocturned cycle` orchestrator.
 *
 * Runs the daily-rotation pipeline as a single atomic operation:
 *   scan -> ingest -> resolve -> rotate -> publish
 *
 * Designed for systemd-timer / cron driving. Each step is the same handler
 * the standalone subcommand uses (no duplicated logic). Stops on the first
 * non-zero exit and propagates it. The single PID lockfile each handler
 * acquires individually serialises the steps against any concurrent
 * `nocturned watch` or manual invocation.
 */

#define _GNU_SOURCE

#include "cli.h"

#include <stdio.h>

int scan_cmd_main(struct cli_args *args);
int ingest_cmd_main(struct cli_args *args);
int resolve_cmd_main(struct cli_args *args);
int rotate_cmd_main(struct cli_args *args);
int publish_cmd_main(struct cli_args *args);

int cycle_cmd_main(struct cli_args *args)
{
    int rc;

    if (!args->library_path) {
        fprintf(stderr,
            "nocturned cycle: library path required as positional arg\n"
            "  e.g. nocturned cycle /home/lucas/music\n");
        return NOCT_EXIT_USAGE;
    }

    fprintf(stderr, "cycle: [1/5] scan\n");
    rc = scan_cmd_main(args);
    if (rc != NOCT_EXIT_OK) {
        fprintf(stderr, "cycle: scan failed (rc=%d), aborting\n", rc);
        return rc;
    }

    /* ingest is best-effort: phone may not have produced any new JSONL,
     * which is not an error. The handler exits 0 on a no-op.
     *
     * 2026-05-01: per-file failures used to abort the entire cycle
     * (manifest never published, Syncthing stuck out-of-sync). One bad
     * action / parse error / DB transaction collision should NOT kill
     * resolve+rotate+publish. Treat ingest non-zero as a warning and
     * continue — the operator can investigate via journalctl. The lock
     * collision case (NOCT_EXIT_LOCK_BUSY) IS still fatal: a parallel
     * writer means we can't safely proceed. */
    fprintf(stderr, "cycle: [2/5] ingest\n");
    rc = ingest_cmd_main(args);
    if (rc == NOCT_EXIT_LOCK_BUSY) {
        fprintf(stderr, "cycle: ingest lock-busy (rc=%d), aborting\n", rc);
        return rc;
    }
    if (rc != NOCT_EXIT_OK) {
        fprintf(stderr, "cycle: ingest had per-file errors (rc=%d), continuing\n", rc);
        /* fall through to resolve so manifest still publishes */
    }

    fprintf(stderr, "cycle: [3/5] resolve\n");
    rc = resolve_cmd_main(args);
    if (rc != NOCT_EXIT_OK && rc != 1 /* cold-start non-zero */) {
        fprintf(stderr, "cycle: resolve failed (rc=%d), aborting\n", rc);
        return rc;
    }

    fprintf(stderr, "cycle: [4/5] rotate\n");
    rc = rotate_cmd_main(args);
    if (rc != NOCT_EXIT_OK) {
        fprintf(stderr, "cycle: rotate failed (rc=%d), aborting\n", rc);
        return rc;
    }

    fprintf(stderr, "cycle: [5/5] publish\n");
    rc = publish_cmd_main(args);
    if (rc != NOCT_EXIT_OK) {
        fprintf(stderr, "cycle: publish failed (rc=%d), aborting\n", rc);
        return rc;
    }

    fprintf(stderr, "cycle: done\n");
    return NOCT_EXIT_OK;
}
