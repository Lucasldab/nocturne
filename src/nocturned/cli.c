/*
 * cli.c — argv parser for nocturned.
 *
 * getopt_long-driven, mirroring src/tagcheck/cli style. Subcommand is the
 * first positional; remaining positionals (e.g. library path) are taken
 * from optind onwards. Options recognised globally so any subcommand may
 * carry them; per-subcommand validation is done at dispatch time.
 */

#define _GNU_SOURCE

#include "cli.h"

#include <getopt.h>
#include <stdio.h>
#include <string.h>

void cli_print_usage(FILE *f)
{
    fprintf(f,
        "nocturned — single-writer daemon for the nocturne library\n"
        "\n"
        "Usage: nocturned <subcommand> [options] [args...]\n"
        "\n"
        "Subcommands:\n"
        "  scan <library>    Walk the library, hash audio bytes, sync DB rows\n"
        "  watch <library>   Run a long-lived inotify watcher; rescan on changes\n"
        "  resolve           Compute the manifest from current DB + config buckets\n"
        "  publish           Atomically write catalog.json + manifest.json\n"
        "  ingest            (Phase 7) Replay phone JSONL into DB\n"
        "  doctor            Print library + DB health report\n"
        "\n"
        "Options:\n"
        "  -h, --help              Print this help and exit\n"
        "  -V, --version           Print version and exit\n"
        "      --config <path>     Override config.toml location\n"
        "      --out <dir>         (publish) Output directory for catalog/manifest\n"
        "      --dry-run           (resolve) Don't write; report what would change\n"
        "      --explain           (resolve) Per-track inclusion reason output\n"
        "\n");
}

void cli_print_version(FILE *f)
{
    fprintf(f, "nocturned 0.2.0-dev (phase 02-daemon-foundation)\n");
}

static enum nocturned_subcommand subcommand_from_string(const char *s)
{
    if (!s) return CMD_NONE;
    if (!strcmp(s, "scan"))    return CMD_SCAN;
    if (!strcmp(s, "watch"))   return CMD_WATCH;
    if (!strcmp(s, "resolve")) return CMD_RESOLVE;
    if (!strcmp(s, "publish")) return CMD_PUBLISH;
    if (!strcmp(s, "ingest"))  return CMD_INGEST;
    if (!strcmp(s, "doctor"))  return CMD_DOCTOR;
    if (!strcmp(s, "help"))    return CMD_HELP;
    if (!strcmp(s, "version")) return CMD_VERSION;
    return CMD_NONE;
}

enum nocturned_subcommand cli_parse(int argc, char **argv, struct cli_args *out)
{
    static const struct option long_opts[] = {
        { "help",     no_argument,       NULL, 'h' },
        { "version",  no_argument,       NULL, 'V' },
        { "config",   required_argument, NULL, 'c' },
        { "out",      required_argument, NULL, 'o' },
        { "dry-run",  no_argument,       NULL, 1000 },
        { "explain",  no_argument,       NULL, 1001 },
        { 0, 0, 0, 0 }
    };

    if (out) {
        out->cmd          = CMD_NONE;
        out->library_path = NULL;
        out->out_dir      = NULL;
        out->config_path  = NULL;
        out->dry_run      = 0;
        out->explain      = 0;
    }
    if (!out || argc < 1) return CMD_NONE;

    /* Scan options across the entire argv; subcommand string is taken as the
     * first non-option argument. POSIX-style: stop at first non-option, then
     * resume after it for any trailing options. */
    int c;
    int seen_subcmd = 0;
    optind = 1;
    /* Allow "+" leading to stop at first non-option, but we want flags
     * anywhere — use the default GNU permutation. The subcommand will fall
     * out of optind once getopt_long returns -1. */
    while ((c = getopt_long(argc, argv, "hVc:o:", long_opts, NULL)) != -1) {
        switch (c) {
        case 'h': out->cmd = CMD_HELP;    return CMD_HELP;
        case 'V': out->cmd = CMD_VERSION; return CMD_VERSION;
        case 'c': out->config_path = optarg; break;
        case 'o': out->out_dir = optarg; break;
        case 1000: out->dry_run = 1; break;
        case 1001: out->explain = 1; break;
        case '?':
        default:
            out->cmd = CMD_NONE;
            return CMD_NONE;
        }
        (void) seen_subcmd;
    }

    if (optind >= argc) {
        out->cmd = CMD_HELP;
        return CMD_HELP;
    }

    enum nocturned_subcommand sub = subcommand_from_string(argv[optind]);
    if (sub == CMD_NONE) {
        fprintf(stderr, "nocturned: unknown subcommand '%s'\n", argv[optind]);
        out->cmd = CMD_NONE;
        return CMD_NONE;
    }
    optind++;
    out->cmd = sub;

    /* First trailing positional is the library path for scan/watch. */
    if ((sub == CMD_SCAN || sub == CMD_WATCH) && optind < argc) {
        out->library_path = argv[optind++];
    }

    return sub;
}
