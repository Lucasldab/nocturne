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
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include <taglib/tag_c.h>

#include "check.h"
#include "cli.h"
#include "report.h"
#include "tags.h"
#include "walker.h"

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

    opts->library_path     = NULL;
    opts->quarantine_path  = TAGCHECK_DEFAULT_QUARANTINE;
    opts->emit_json        = false;
    opts->quarantine       = false;
    opts->dry_run          = false;
    opts->init_quarantine  = false;
    opts->show_help        = false;
    opts->show_version     = false;

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

struct main_ctx {
    struct report_summary *summary;
};

static enum walk_result on_file_check(const struct tag_record *rec, void *userdata)
{
    struct main_ctx *ctx = userdata;
    struct check_result cr = {0};
    check_canonical(rec, &cr);
    report_add(ctx->summary, &cr);
    check_result_free(&cr);
    return WALK_CONTINUE;
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

    /* Plan 04 territory — bail with a clear "not yet" message. */
    if (opts.init_quarantine || opts.quarantine || opts.dry_run) {
        fprintf(stderr,
                "%s: --quarantine/--init-quarantine/--dry-run not yet "
                "implemented (Phase 1 Plan 04)\n",
                progname);
        return TAGCHECK_EXIT_ERROR;
    }

    /* Force TagLib to return UTF-8 even from legacy v2.3 ID3 files. */
    taglib_set_strings_unicode((BOOL)1);

    struct report_summary summary;
    report_summary_init(&summary,
                        opts.emit_json ? REPORT_FORMAT_JSON : REPORT_FORMAT_TEXT,
                        stdout);

    struct main_ctx ctx = { .summary = &summary };
    struct walk_stats walk_stats;
    int wr = walker_walk(opts.library_path, on_file_check, &ctx, &walk_stats);
    if (wr != 0) {
        report_summary_free(&summary);
        return TAGCHECK_EXIT_ERROR;
    }

    report_emit(&summary, &walk_stats);

    /* Exit-code resolution per locked CONTEXT decisions:
     *   files_failed > 0           → 1
     *   else                       → 0
     * files_flagged_only > 0 alone does NOT push to 1 — multi-value FLAGs
     * are advisory only (TAG-03 locked rule). Plan 04 lifts to 2 when
     * --quarantine moves files. */
    int exit_code = (summary.files_failed > 0) ? TAGCHECK_EXIT_FAILED_TRACKS
                                               : TAGCHECK_EXIT_OK;

    report_summary_free(&summary);
    return exit_code;
}
