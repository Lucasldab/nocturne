/*
 * sync_config_cmd.c — `nocturned sync-config [--print|--apply]
 *                                              [--side desktop|phone]`.
 *
 * --print (default): emit XML to stdout, no lock, no network.
 * --apply: lock + REST PUT against local Syncthing for desktop side.
 *          --apply --side phone is rejected (the daemon cannot reach
 *          the phone; user must paste the --print output into
 *          Syncthing-Fork manually).
 */

#define _GNU_SOURCE

#include "cli.h"
#include "config.h"
#include "lock.h"
#include "paths.h"
#include "sync_config.h"
#include "syncthing_api.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int sync_config_cmd_main(struct cli_args *args)
{
    if (!args) return NOCT_EXIT_USAGE;

    enum sync_config_side side = SIDE_DESKTOP;
    if (args->sync_config_side &&
        !strcmp(args->sync_config_side, "phone")) {
        side = SIDE_PHONE;
    } else if (args->sync_config_side &&
               strcmp(args->sync_config_side, "desktop") != 0) {
        fprintf(stderr,
            "nocturned sync-config: unknown --side '%s' "
            "(allowed: desktop, phone)\n", args->sync_config_side);
        return NOCT_EXIT_USAGE;
    }

    struct nocturne_config cfg;
    const char *cfg_path = args->config_path ?
                           args->config_path : paths_config_file();
    if (config_load(cfg_path, &cfg) != 0) {
        config_free(&cfg);
        return 3;
    }

    if (args->apply) {
        if (side == SIDE_PHONE) {
            fprintf(stderr,
                "nocturned sync-config: --apply not supported for "
                "--side phone (daemon cannot reach the phone). Use "
                "--print and follow docs/phone-setup.md.\n");
            config_free(&cfg);
            return NOCT_EXIT_USAGE;
        }

        const char *pidfile = paths_pidfile();
        int busy_pid = 0;
        struct nocturne_lock *lock = lock_acquire(pidfile, &busy_pid);
        if (!lock) {
            if (errno == EWOULDBLOCK) {
                fprintf(stderr,
                    "nocturned: another instance is running (pid=%d); "
                    "single-writer lock at %s\n", busy_pid, pidfile);
                config_free(&cfg);
                return NOCT_EXIT_LOCK_BUSY;
            }
            fprintf(stderr, "nocturned sync-config: lock_acquire failed: %s\n",
                    strerror(errno));
            config_free(&cfg);
            return 1;
        }

        syncthing_api_init();
        if (syncthing_get_config(getenv("NOCTURNE_SYNCTHING_CONFIG")) != 0) {
            fprintf(stderr,
                "nocturned sync-config: syncthing config.xml not found; "
                "cannot --apply. Use --print and edit manually.\n");
            lock_release(lock);
            config_free(&cfg);
            return 3;
        }
        int rc = sync_config_apply(&cfg);
        lock_release(lock);
        config_free(&cfg);
        return rc == 0 ? NOCT_EXIT_OK : 1;
    }

    /* --print (default) */
    int rc = sync_config_emit(side, &cfg, stdout);
    config_free(&cfg);
    return rc == 0 ? NOCT_EXIT_OK : 1;
}
