/*
 * nocturne-tagcheck — canonical-tag baseline + quarantine utility
 *
 * Exit codes (from cli.h):
 *   0 (TAGCHECK_EXIT_OK)             — all tracks pass canonical schema
 *   1 (TAGCHECK_EXIT_FAILED_TRACKS)  — some tracks failed (report only)
 *   2 (TAGCHECK_EXIT_QUARANTINED)    — some tracks quarantined (or would be in --dry-run)
 *   3 (TAGCHECK_EXIT_ERROR)          — library inaccessible / TagLib error / usage error
 *
 * See .planning/phases/01-tag-baseline/CONTEXT.md for the locked decisions
 * behind this exit-code matrix.
 *
 * Phase 1 Plan 01-01: this file is a scaffold. Plan 02 wires the walker;
 * Plan 03 the checker + reporter; Plan 04 the quarantine. Until then the
 * binary parses every flag and exits 3 with a "not yet implemented" stub
 * for any non-trivial invocation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include "cli.h"

static void print_usage_to(FILE *out, const char *progname)
{
    fprintf(out,
        "Usage: %s [options] [<library>]\n"
        "\n"
        "Walk a music library, verify canonical tag schema, and optionally\n"
        "move failed tracks into a quarantine directory.\n"
        "\n"
        "Positional:\n"
        "  <library>             Path to the music library (default: %s)\n"
        "\n"
        "Options:\n"
        "  --json                Emit structured JSON report (default: text)\n"
        "                        (not yet implemented in Phase 1 Plan 03)\n"
        "  --quarantine          Move tracks failing the canonical schema into\n"
        "                        the quarantine directory; logs to quarantine.log\n"
        "                        (not yet implemented in Phase 1 Plan 04)\n"
        "  --dry-run             With --quarantine, preview moves without writes\n"
        "                        (not yet implemented in Phase 1 Plan 04)\n"
        "  --init-quarantine     Create the quarantine directory (mode 0700) and exit\n"
        "                        (not yet implemented in Phase 1 Plan 04)\n"
        "  --quarantine-path PATH\n"
        "                        Override default quarantine directory (default: %s)\n"
        "  -h, --help            Show this help and exit\n"
        "  -V, --version         Show version and exit\n"
        "\n"
        "Exit codes: 0=clean  1=failed-tracks  2=quarantined  3=error\n",
        progname,
        TAGCHECK_DEFAULT_LIBRARY,
        TAGCHECK_DEFAULT_QUARANTINE);
}

void cli_print_usage(const char *progname)
{
    print_usage_to(stderr, progname);
}

int cli_parse(int argc, char **argv, struct cli_opts *opts)
{
    static const struct option long_opts[] = {
        { "json",             no_argument,       NULL, 'j' },
        { "quarantine",       no_argument,       NULL, 'q' },
        { "dry-run",          no_argument,       NULL, 'n' },
        { "init-quarantine",  no_argument,       NULL, 'I' },
        { "quarantine-path",  required_argument, NULL, 'Q' },
        { "help",             no_argument,       NULL, 'h' },
        { "version",          no_argument,       NULL, 'V' },
        { NULL, 0, NULL, 0 }
    };

    /* Defaults — library_path is set later only if no positional given so we
     * can detect "user gave nothing" vs. "user explicitly gave default path". */
    opts->library_path     = NULL;
    opts->quarantine_path  = TAGCHECK_DEFAULT_QUARANTINE;
    opts->emit_json        = false;
    opts->quarantine       = false;
    opts->dry_run          = false;
    opts->init_quarantine  = false;
    opts->show_help        = false;
    opts->show_version     = false;

    /* Reset getopt state for safety in case caller used getopt before. */
    optind = 1;
    opterr = 0;

    int c;
    while ((c = getopt_long(argc, argv, "hV", long_opts, NULL)) != -1) {
        switch (c) {
        case 'j': opts->emit_json       = true; break;
        case 'q': opts->quarantine      = true; break;
        case 'n': opts->dry_run         = true; break;
        case 'I': opts->init_quarantine = true; break;
        case 'Q': opts->quarantine_path = optarg; break;
        case 'h': opts->show_help       = true; break;
        case 'V': opts->show_version    = true; break;
        case '?':
        default:
            fprintf(stderr, "%s: unknown or invalid option\n",
                    argv[0] ? argv[0] : "nocturne-tagcheck");
            cli_print_usage(argv[0] ? argv[0] : "nocturne-tagcheck");
            return 1;
        }
    }

    /* Positional handling: 0 → use default; 1 → use it; >1 → usage error. */
    int positional = argc - optind;
    if (positional > 1) {
        fprintf(stderr, "%s: too many positional arguments (got %d, expected 0 or 1)\n",
                argv[0] ? argv[0] : "nocturne-tagcheck", positional);
        cli_print_usage(argv[0] ? argv[0] : "nocturne-tagcheck");
        return 1;
    }

    if (positional == 1) {
        opts->library_path = argv[optind];
    } else {
        opts->library_path = TAGCHECK_DEFAULT_LIBRARY;
    }

    return 0;
}

int main(int argc, char **argv)
{
    struct cli_opts opts;
    const char *progname = (argc > 0 && argv[0]) ? argv[0] : "nocturne-tagcheck";

    if (cli_parse(argc, argv, &opts) != 0) {
        return TAGCHECK_EXIT_ERROR;
    }

    if (opts.show_help) {
        print_usage_to(stdout, progname);
        return TAGCHECK_EXIT_OK;
    }

    if (opts.show_version) {
        printf("nocturne-tagcheck %s\n", TAGCHECK_VERSION);
        return TAGCHECK_EXIT_OK;
    }

    /* Phase 1 Plan 01-01: walker not yet wired. Subsequent plans replace
     * this body with: walker_walk → check_canonical → report_emit (→ quarantine_move). */
    fprintf(stderr, "%s: walker not yet implemented (Phase 1 Plan 02)\n", progname);
    return TAGCHECK_EXIT_ERROR;
}
