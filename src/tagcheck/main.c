/*
 * nocturne-tagcheck — canonical-tag baseline + quarantine utility
 *
 * Exit codes (from cli.h):
 *   0 (TAGCHECK_EXIT_OK)             — all tracks pass canonical schema
 *   1 (TAGCHECK_EXIT_FAILED_TRACKS)  — some tracks failed (report only)
 *   2 (TAGCHECK_EXIT_QUARANTINED)    — some tracks quarantined (or would be in --dry-run)
 *   3 (TAGCHECK_EXIT_ERROR)          — library inaccessible / TagLib error / usage error
 *
 * Exit-code matrix (locked CONTEXT decision):
 *   --quarantine?   failed_count   moved_count   |  exit
 *   no              0              —             |  0
 *   no              >0             —             |  1
 *   yes (real)      0              0             |  0
 *   yes (real)      >0             >0            |  2
 *   yes (dry-run)   0              0             |  0
 *   yes (dry-run)   >0             >0            |  2  (preview signal: action would happen)
 *
 * See .planning/phases/01-tag-baseline/CONTEXT.md.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include <taglib/tag_c.h>

#include "check.h"
#include "cli.h"
#include "quarantine.h"
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
        "                        <quarantine-path>; logs moves to quarantine.log\n"
        "  --dry-run             With --quarantine, preview moves without touching filesystem\n"
        "  --init-quarantine     Create the quarantine directory (mode 0700) at\n"
        "                        <quarantine-path> and exit (or continue if --quarantine\n"
        "                        also given)\n"
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
    struct quarantine_ctx *qctx;     /* NULL when --quarantine not active */
};

static enum walk_result on_file_check(const struct tag_record *rec, void *userdata)
{
    struct main_ctx *ctx = userdata;
    struct check_result cr = {0};
    check_canonical(rec, &cr);
    report_add(ctx->summary, &cr);
    if (ctx->qctx && quarantine_should_move(&cr)) {
        /* quarantine_move handles its own error logging; we still continue. */
        quarantine_move(ctx->qctx, &cr);
    }
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

    /* --init-quarantine: handle BEFORE the walk. If the user combined
     * --init-quarantine with --quarantine we create the dir then continue
     * to a normal quarantine run. Otherwise create + exit 0. */
    if (opts.init_quarantine) {
        if (quarantine_create_dir(opts.quarantine_path) != 0) {
            return TAGCHECK_EXIT_ERROR;
        }
        if (!opts.quarantine) {
            printf("created quarantine directory: %s\n",
                   opts.quarantine_path);
            return TAGCHECK_EXIT_OK;
        }
        /* fall through to combined --init+--quarantine run */
    }

    /* --dry-run alone has no meaning — there's nothing to preview if
     * we're not also going to act. */
    if (opts.dry_run && !opts.quarantine) {
        fprintf(stderr,
                "%s: --dry-run requires --quarantine\n", progname);
        return TAGCHECK_EXIT_ERROR;
    }

    /* TagLib unicode: force UTF-8 returns even on legacy v2.3 tags. */
    taglib_set_strings_unicode((BOOL)1);

    /* Reporter setup. */
    struct report_summary summary;
    report_summary_init(&summary,
                        opts.emit_json ? REPORT_FORMAT_JSON : REPORT_FORMAT_TEXT,
                        stdout);

    /* Quarantine setup (only if --quarantine). */
    struct quarantine_ctx qctx;
    bool qctx_active = false;
    int exit_code = TAGCHECK_EXIT_OK;

    if (opts.quarantine) {
        if (quarantine_init(&qctx, opts.library_path, opts.quarantine_path,
                            opts.dry_run) != 0) {
            report_summary_free(&summary);
            return TAGCHECK_EXIT_ERROR;
        }
        qctx_active = true;
    }

    struct main_ctx ctx = {
        .summary = &summary,
        .qctx = qctx_active ? &qctx : NULL,
    };

    struct walk_stats walk_stats;
    int wr = walker_walk(opts.library_path, on_file_check, &ctx, &walk_stats);
    if (wr != 0) {
        exit_code = TAGCHECK_EXIT_ERROR;
        goto cleanup;
    }

    report_emit(&summary, &walk_stats);

    /* Exit-code resolution per the matrix at the top of this file. */
    if (qctx_active && qctx.moved_count > 0) {
        exit_code = TAGCHECK_EXIT_QUARANTINED;
    } else if (summary.files_failed > 0) {
        exit_code = TAGCHECK_EXIT_FAILED_TRACKS;
    } else {
        exit_code = TAGCHECK_EXIT_OK;
    }

cleanup:
    if (qctx_active) {
        quarantine_close(&qctx);
    }
    report_summary_free(&summary);
    return exit_code;
}
