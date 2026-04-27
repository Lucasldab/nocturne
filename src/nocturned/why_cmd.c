/*
 * why_cmd.c — `nocturned why <track-id>` (TUNE-03 daemon CLI).
 *
 * Read-only: looks up a resident track in <sync_meta>/manifest.json and
 * prints its qualifying buckets. NO lockfile acquisition (mirrors
 * doctor_cmd_main); coexists with `nocturned watch`.
 *
 * Real implementation lands in Task 2 of plan 08-01.
 */
#define _GNU_SOURCE

#include "cli.h"
#include <stdio.h>

int why_cmd_main(struct cli_args *args)
{
    (void) args;
    fprintf(stderr, "nocturned why: stub (Task 2 of plan 08-01 implements this)\n");
    return NOCT_EXIT_FAILURE;
}
