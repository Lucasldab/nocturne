#ifndef NOCTURNE_TAGCHECK_CLI_H
#define NOCTURNE_TAGCHECK_CLI_H

#include <stdbool.h>

#define TAGCHECK_DEFAULT_LIBRARY  "/home/lucas/music/library"
#define TAGCHECK_DEFAULT_QUARANTINE "/home/lucas/music/quarantine"

/* Exit codes per CONTEXT decisions:
 *   0 = all tracks pass canonical schema
 *   1 = some tracks failed (report only)
 *   2 = some tracks quarantined
 *   3 = library inaccessible / TagLib error / usage error
 */
#define TAGCHECK_EXIT_OK              0
#define TAGCHECK_EXIT_FAILED_TRACKS   1
#define TAGCHECK_EXIT_QUARANTINED     2
#define TAGCHECK_EXIT_ERROR           3

struct cli_opts {
    const char *library_path;       /* Positional arg; defaults to TAGCHECK_DEFAULT_LIBRARY */
    const char *quarantine_path;    /* Defaults to TAGCHECK_DEFAULT_QUARANTINE (sibling of library) */
    bool emit_json;                 /* --json */
    bool quarantine;                /* --quarantine */
    bool dry_run;                   /* --dry-run */
    bool init_quarantine;           /* --init-quarantine */
    bool show_help;                 /* --help / -h */
    bool show_version;              /* --version / -V */
};

/* Parse argv into opts. Returns 0 on success, non-zero on usage error
 * (after printing usage to stderr). */
int cli_parse(int argc, char **argv, struct cli_opts *opts);

/* Print usage to the given stream (stdout for --help, stderr for errors). */
void cli_print_usage(const char *progname);

/* Project version string, baked at compile time. */
#define TAGCHECK_VERSION "0.1.0"

#endif
