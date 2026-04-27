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
#include <stdlib.h>
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
        "  migrate <lib>     Move flat library into archive/<rel> (dry-run by default)\n"
        "  rotate            Apply manifest_current diff via hardlink+unlink\n"
        "  sync-config       Emit (or apply) Syncthing folder XML\n"
        "  ingest [--meta-dir <path>] [--dry-run]\n"
        "                    Replay phone JSONL into DB (offset-tracked, idempotent)\n"
        "  cycle [<library>] Run scan -> ingest -> resolve -> rotate -> publish\n"
        "                    in sequence (intended for systemd timer / cron)\n"
        "  why <track-id>    Explain why a track is on the phone (read-only)\n"
        "                    <track-id>: full 64-char sha256 hex OR unique >=8-char prefix\n"
        "  doctor            Print library + DB health report\n"
        "\n"
        "Options:\n"
        "  -h, --help              Print this help and exit\n"
        "  -V, --version           Print version and exit\n"
        "      --config <path>     Override config.toml location\n"
        "      --out <dir>         (publish) Output directory for catalog/manifest\n"
        "      --dry-run           (resolve) Don't write; report what would change\n"
        "      --explain           (resolve) Per-track inclusion reason output\n"
        "      --debounce-ms N     (watch) Coalesce events for N ms (default 1000)\n"
        "      --periodic-rescan-sec N\n"
        "                          (watch) Periodic rescan interval when in ENOSPC\n"
        "                          fallback mode (default 300)\n"
        "      --json              (doctor) Emit JSON instead of text\n"
        "      --apply             (migrate, sync-config) Execute (default: dry-run/print)\n"
        "      --print             (sync-config) Print XML to stdout (default)\n"
        "      --side desktop|phone\n"
        "                          (sync-config) Which endpoint to emit (default: desktop)\n"
        "      --meta-dir <path>   (ingest) Override sync metadata folder (default: config sync_meta.path)\n"
        "      --manifest <path>   (why) Override <sync_meta>/manifest.json source path\n"
        "\n"
        "WiFi-only sync is a Syncthing-Fork app-level setting on the phone — not in this XML.\n"
        "Trashcan/file versioning is disabled (type=\"none\") on sync-files for both sides.\n"
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
    if (!strcmp(s, "why"))     return CMD_WHY;
    if (!strcmp(s, "resolve")) return CMD_RESOLVE;
    if (!strcmp(s, "publish")) return CMD_PUBLISH;
    if (!strcmp(s, "ingest"))  return CMD_INGEST;
    if (!strcmp(s, "doctor"))  return CMD_DOCTOR;
    if (!strcmp(s, "migrate")) return CMD_MIGRATE;
    if (!strcmp(s, "rotate"))  return CMD_ROTATE;
    if (!strcmp(s, "sync-config")) return CMD_SYNC_CONFIG;
    if (!strcmp(s, "cycle"))   return CMD_CYCLE;
    if (!strcmp(s, "help"))    return CMD_HELP;
    if (!strcmp(s, "version")) return CMD_VERSION;
    return CMD_NONE;
}

enum nocturned_subcommand cli_parse(int argc, char **argv, struct cli_args *out)
{
    static const struct option long_opts[] = {
        { "help",                no_argument,       NULL, 'h' },
        { "version",             no_argument,       NULL, 'V' },
        { "config",              required_argument, NULL, 'c' },
        { "out",                 required_argument, NULL, 'o' },
        { "dry-run",             no_argument,       NULL, 1000 },
        { "explain",             no_argument,       NULL, 1001 },
        { "debounce-ms",         required_argument, NULL, 1002 },
        { "periodic-rescan-sec", required_argument, NULL, 1003 },
        { "json",                no_argument,       NULL, 1004 },
        { "apply",               no_argument,       NULL, 1005 },
        { "print",               no_argument,       NULL, 1006 },
        { "side",                required_argument, NULL, 1007 },
        { "meta-dir",            required_argument, NULL, 1008 },
        { "manifest",            required_argument, NULL, 1009 },
        { 0, 0, 0, 0 }
    };

    if (out) {
        out->cmd                  = CMD_NONE;
        out->library_path         = NULL;
        out->out_dir              = NULL;
        out->config_path          = NULL;
        out->dry_run              = 0;
        out->explain              = 0;
        out->debounce_ms          = 0;
        out->periodic_rescan_sec  = 0;
        out->json                 = 0;
        out->apply                = 0;
        out->sync_config_side     = NULL;
        out->sync_config_print    = 0;
        out->meta_dir             = NULL;
        out->track_id                = NULL;
        out->manifest_path_override  = NULL;
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
        case 1002: out->debounce_ms = atoi(optarg); break;
        case 1003: out->periodic_rescan_sec = atoi(optarg); break;
        case 1004: out->json = 1; break;
        case 1005: out->apply = 1; break;
        case 1006: out->sync_config_print = 1; break;
        case 1007: out->sync_config_side = optarg; break;
        case 1008: out->meta_dir = optarg; break;
        case 1009: out->manifest_path_override = optarg; break;
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

    /* First trailing positional is the library path for scan/watch/migrate/cycle. */
    if ((sub == CMD_SCAN || sub == CMD_WATCH || sub == CMD_MIGRATE || sub == CMD_CYCLE) && optind < argc) {
        out->library_path = argv[optind++];
    }

    /* `why` takes a single trailing positional: the track id (full sha256 hex
     * or >=8-char prefix). Validation of charset/length happens in
     * why_cmd_main; we only capture the string here. */
    if (sub == CMD_WHY && optind < argc) {
        out->track_id = argv[optind++];
    }

    return sub;
}
