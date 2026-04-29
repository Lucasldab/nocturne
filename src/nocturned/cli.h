#ifndef NOCTURNE_NOCTURNED_CLI_H
#define NOCTURNE_NOCTURNED_CLI_H

#include <stdio.h>

/* Exit code matrix — exposed in cli.h so every subcommand handler agrees.
 * Prefixed to avoid clobbering POSIX <stdlib.h>'s EXIT_FAILURE / EXIT_SUCCESS. */
enum {
    NOCT_EXIT_OK         = 0,
    NOCT_EXIT_FAILURE    = 1,
    NOCT_EXIT_LOCK_BUSY  = 4,
    NOCT_EXIT_USAGE      = 64
};

enum nocturned_subcommand {
    CMD_NONE = 0,
    CMD_HELP,
    CMD_VERSION,
    CMD_SCAN,
    CMD_WATCH,
    CMD_RESOLVE,
    CMD_PUBLISH,
    CMD_INGEST,
    CMD_DOCTOR,
    CMD_MIGRATE,
    CMD_ROTATE,
    CMD_SYNC_CONFIG,
    CMD_CYCLE,
    CMD_WHY,
    CMD_DISKCHECK,
    CMD_TRANSCODE,
    CMD_TRANSCODE_MIGRATE,
    CMD_DISCOVER,
    CMD_UNSYNC,
    CMD_DELETE
};

/* Parsed argv for the daemon. Strings are pointers into argv (no ownership). */
struct cli_args {
    enum nocturned_subcommand cmd;
    const char *library_path;     /* positional for scan/watch */
    const char *out_dir;          /* --out for publish */
    const char *config_path;      /* --config for any subcommand */
    int dry_run;                  /* --dry-run (resolve) */
    int explain;                  /* --explain   (resolve) */
    int diff;                     /* --diff       (resolve --dry-run; tuning workflow) */
    int debounce_ms;              /* --debounce-ms (watch); 0 = default */
    int periodic_rescan_sec;      /* --periodic-rescan-sec (watch); 0 = default */
    int json;                     /* --json (doctor) */
    int apply;                    /* --apply (migrate, sync-config) */
    const char *sync_config_side; /* --side desktop|phone (sync-config) */
    int sync_config_print;        /* --print (sync-config; default if neither) */
    const char *meta_dir;         /* --meta-dir (ingest); NULL = config sync_meta.path = ~/sync/nocturne/meta */
    const char *track_id;                /* positional for `why` (sha256 hex full or >=8-char prefix) */
    const char *manifest_path_override;  /* --manifest <path> for `why` */

    /* `transcode` positionals: src + dst. Format/bitrate come from --format
     * and --bitrate (long opts) or fall back to config / built-in defaults. */
    const char *transcode_src;
    const char *transcode_dst;
    const char *transcode_format;       /* opus|aac; NULL → config */
    int transcode_bitrate_kbps;         /* 0 → config */

    /* `unsync` / `delete` positional: sha256 (full hex) or album_id (--album). */
    const char *action_target;
    int action_is_album;                /* --album flag */
    int action_yes;                     /* --yes (skip delete confirm prompt) */
};

/* Parse argv. Returns the chosen subcommand (CMD_HELP / CMD_VERSION / CMD_NONE
 * are valid). Fills *out with positional + option values. On usage error
 * prints to stderr and returns CMD_NONE with a non-zero set on out->cmd. */
enum nocturned_subcommand cli_parse(int argc, char **argv, struct cli_args *out);

void cli_print_usage(FILE *f);
void cli_print_version(FILE *f);

#endif /* NOCTURNE_NOCTURNED_CLI_H */
