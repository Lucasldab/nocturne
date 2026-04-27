/*
 * main.c — nocturned entry point.
 *
 * This plan (02-01) lands the subcommand routing skeleton. Real handlers
 * arrive in plans 02-02 (scan), 02-03 (watch), 02-04 (doctor), 02-05
 * (resolve), 02-06 (publish), and 07-02 (ingest — replaces the Phase
 * 2/3 stub). The PID lockfile is wired here for write subcommands so
 * DAEMON-04 lands with this plan.
 *
 * Lock policy:
 *   - scan / watch / resolve / publish / ingest: take exclusive lock,
 *     write subcommands.
 *   - doctor: read-only, skip lock so it works while a long watch is
 *     running.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>

#include "cli.h"

/* Forward decls for subcommand handlers extracted into their own .c files. */
int scan_cmd_main(struct cli_args *args);
int watch_cmd_main(struct cli_args *args);
int doctor_cmd_main(struct cli_args *args);
int resolve_cmd_main(struct cli_args *args);
int publish_cmd_main(struct cli_args *args);
int migrate_cmd_main(struct cli_args *args);
int rotate_cmd_main(struct cli_args *args);
int sync_config_cmd_main(struct cli_args *args);
int ingest_cmd_main(struct cli_args *args);
int cycle_cmd_main(struct cli_args *args);

/* Each subcommand handler owns its own lock acquisition (scan_cmd.c,
 * watch_cmd.c, resolve_cmd.c, publish_cmd.c). main.c is now pure dispatch. */

/* scan handler now lives in scan_cmd.c; main.c just dispatches. */

/* watch handler now lives in watch_cmd.c; main.c just dispatches. */

/* resolve handler now lives in resolve_cmd.c; main.c just dispatches. */

/* publish handler now lives in publish_cmd.c; main.c just dispatches. */

/* doctor handler now lives in doctor_cmd.c; main.c just dispatches. */

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
    case CMD_SCAN:    return scan_cmd_main(&args);
    case CMD_WATCH:   return watch_cmd_main(&args);
    case CMD_RESOLVE: return resolve_cmd_main(&args);
    case CMD_PUBLISH: return publish_cmd_main(&args);
    case CMD_INGEST:  return ingest_cmd_main(&args);
    case CMD_DOCTOR:  return doctor_cmd_main(&args);
    case CMD_MIGRATE: return migrate_cmd_main(&args);
    case CMD_ROTATE:  return rotate_cmd_main(&args);
    case CMD_SYNC_CONFIG: return sync_config_cmd_main(&args);
    case CMD_CYCLE:   return cycle_cmd_main(&args);
    case CMD_NONE:
    default:
        cli_print_usage(stderr);
        return NOCT_EXIT_USAGE;
    }
}
