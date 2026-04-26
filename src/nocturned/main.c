/*
 * main.c — nocturned entry point.
 *
 * This plan (02-01) lands the subcommand routing skeleton. Real handlers
 * arrive in plans 02-02 (scan), 02-03 (watch), 02-04 (doctor), 02-05
 * (resolve), 02-06 (publish). `ingest` stays a permanent stub until
 * Phase 7. The PID lockfile is wired here for write subcommands so
 * DAEMON-04 lands with this plan.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"

static int cmd_scan_stub(const struct cli_args *a)
{
    (void) a;
    fprintf(stdout, "stub: scan handler lands in plan 02-02\n");
    return NOCT_EXIT_OK;
}

static int cmd_watch_stub(const struct cli_args *a)
{
    (void) a;
    fprintf(stdout, "stub: watch handler lands in plan 02-03\n");
    return NOCT_EXIT_OK;
}

static int cmd_resolve_stub(const struct cli_args *a)
{
    (void) a;
    fprintf(stdout, "stub: resolve handler lands in plan 02-05\n");
    return NOCT_EXIT_OK;
}

static int cmd_publish_stub(const struct cli_args *a)
{
    (void) a;
    fprintf(stdout, "stub: publish handler lands in plan 02-06\n");
    return NOCT_EXIT_OK;
}

static int cmd_doctor_stub(const struct cli_args *a)
{
    (void) a;
    fprintf(stdout, "stub: doctor handler lands in plan 02-04\n");
    return NOCT_EXIT_OK;
}

static int cmd_ingest_stub(const struct cli_args *a)
{
    (void) a;
    /* Permanent stub: real ingest is Phase 7. The argv table keeps the
     * subcommand reserved so docs and shell completion stay stable. */
    fprintf(stdout, "not implemented (Phase 7)\n");
    return NOCT_EXIT_OK;
}

int main(int argc, char **argv)
{
    struct cli_args args;
    enum nocturned_subcommand cmd = cli_parse(argc, argv, &args);

    switch (cmd) {
    case CMD_HELP:
        cli_print_usage(stdout);
        return NOCT_EXIT_OK;
    case CMD_VERSION:
        cli_print_version(stdout);
        return NOCT_EXIT_OK;
    case CMD_SCAN:    return cmd_scan_stub(&args);
    case CMD_WATCH:   return cmd_watch_stub(&args);
    case CMD_RESOLVE: return cmd_resolve_stub(&args);
    case CMD_PUBLISH: return cmd_publish_stub(&args);
    case CMD_INGEST:  return cmd_ingest_stub(&args);
    case CMD_DOCTOR:  return cmd_doctor_stub(&args);
    case CMD_NONE:
    default:
        cli_print_usage(stderr);
        return NOCT_EXIT_USAGE;
    }
}
