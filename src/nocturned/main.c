/*
 * main.c — nocturned entry point.
 *
 * This plan (02-01) lands the subcommand routing skeleton. Real handlers
 * arrive in plans 02-02 (scan), 02-03 (watch), 02-04 (doctor), 02-05
 * (resolve), 02-06 (publish). `ingest` stays a permanent stub until
 * Phase 7. The PID lockfile is wired here for write subcommands so
 * DAEMON-04 lands with this plan.
 *
 * Lock policy (revisitable in later plans):
 *   - scan / watch / resolve / publish: take exclusive lock, write subcommands.
 *   - doctor: read-only, skip lock so it works while a long watch is running.
 *   - ingest: permanent stub here, lock decision deferred to Phase 7.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "lock.h"
#include "paths.h"

/* Forward decls for subcommand handlers extracted into their own .c files. */
int scan_cmd_main(struct cli_args *args);
int watch_cmd_main(struct cli_args *args);
int doctor_cmd_main(struct cli_args *args);

/* Acquire the daemon-wide single-writer lock. Prints a clear diagnostic and
 * returns the requested exit code (NOCT_EXIT_LOCK_BUSY on contention,
 * NOCT_EXIT_FAILURE on other errors) when acquisition fails; returns 0 with
 * *out_lock set on success. */
static int acquire_lock_or_die(struct nocturne_lock **out_lock)
{
    const char *pidfile = paths_pidfile();
    if (!pidfile) {
        fprintf(stderr, "nocturned: cannot resolve pidfile path (HOME unset?)\n");
        return NOCT_EXIT_FAILURE;
    }
    int busy_pid = 0;
    struct nocturne_lock *l = lock_acquire(pidfile, &busy_pid);
    if (l) {
        *out_lock = l;
        return 0;
    }
    if (errno == EWOULDBLOCK) {
        fprintf(stderr,
                "nocturned: another instance is running (pid=%d); "
                "single-writer lock at %s\n",
                busy_pid, pidfile);
        return NOCT_EXIT_LOCK_BUSY;
    }
    fprintf(stderr, "nocturned: lock_acquire(%s) failed: %s\n",
            pidfile, strerror(errno));
    return NOCT_EXIT_FAILURE;
}

/* scan handler now lives in scan_cmd.c; main.c just dispatches. */

/* watch handler now lives in watch_cmd.c; main.c just dispatches. */

static int cmd_resolve_stub(const struct cli_args *a)
{
    (void) a;
    struct nocturne_lock *lock = NULL;
    int rc = acquire_lock_or_die(&lock);
    if (rc != 0) return rc;
    fprintf(stdout, "stub: resolve handler lands in plan 02-05\n");
    lock_release(lock);
    return NOCT_EXIT_OK;
}

static int cmd_publish_stub(const struct cli_args *a)
{
    (void) a;
    struct nocturne_lock *lock = NULL;
    int rc = acquire_lock_or_die(&lock);
    if (rc != 0) return rc;
    fprintf(stdout, "stub: publish handler lands in plan 02-06\n");
    lock_release(lock);
    return NOCT_EXIT_OK;
}

/* doctor handler now lives in doctor_cmd.c; main.c just dispatches. */

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
    case CMD_SCAN:    return scan_cmd_main(&args);
    case CMD_WATCH:   return watch_cmd_main(&args);
    case CMD_RESOLVE: return cmd_resolve_stub(&args);
    case CMD_PUBLISH: return cmd_publish_stub(&args);
    case CMD_INGEST:  return cmd_ingest_stub(&args);
    case CMD_DOCTOR:  return doctor_cmd_main(&args);
    case CMD_NONE:
    default:
        cli_print_usage(stderr);
        return NOCT_EXIT_USAGE;
    }
}
