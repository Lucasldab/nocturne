/*
 * transcode_cmd.c — `nocturned transcode <src> <dst> [--format f] [--bitrate N]`
 *
 * Standalone wrapper around transcode_audio() for hand-testing quality and
 * size before wiring into rotate. No DB access, no lock, no config write.
 * Reads [transcode] block from config for defaults; CLI flags override.
 *
 * Exit codes:
 *   0  — ffmpeg succeeded
 *   1  — ffmpeg failed (non-zero exit, signal, or fork/exec error)
 *  64  — usage error (missing positionals, bad config)
 */

#define _GNU_SOURCE

#include "cli.h"
#include "config.h"
#include "transcode.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int transcode_cmd_main(struct cli_args *args)
{
    if (!args || !args->transcode_src || !args->transcode_dst) {
        fprintf(stderr,
                "nocturned transcode: usage: transcode <src> <dst> "
                "[--format opus|aac] [--bitrate N]\n");
        return NOCT_EXIT_USAGE;
    }

    /* Verify src is readable upfront — ffmpeg's error message is fine but
     * we want a deterministic exit code on usage-class problems. */
    struct stat st;
    if (stat(args->transcode_src, &st) != 0 || !S_ISREG(st.st_mode)) {
        fprintf(stderr, "nocturned transcode: %s: %s\n",
                args->transcode_src, strerror(errno));
        return NOCT_EXIT_USAGE;
    }

    /* Resolve effective config: CLI flags > config file > built-in defaults. */
    struct nocturne_config cfg;
    if (config_default(&cfg) != 0) {
        fprintf(stderr, "nocturned transcode: OOM in config_default\n");
        return 1;
    }
    if (args->config_path) {
        if (config_load(args->config_path, &cfg) != 0) {
            config_free(&cfg);
            return NOCT_EXIT_USAGE;
        }
    }

    const char *fmt = args->transcode_format
                      ? args->transcode_format
                      : (cfg.transcode_format ? cfg.transcode_format : "opus");
    int kbps = args->transcode_bitrate_kbps > 0
               ? args->transcode_bitrate_kbps
               : (cfg.transcode_bitrate_kbps > 0 ? cfg.transcode_bitrate_kbps : 128);

    struct transcode_cfg tc = {
        .enabled = true,
        .format = fmt,
        .bitrate_kbps = kbps,
    };

    fprintf(stderr,
            "transcode: %s -> %s (codec=%s bitrate=%dk)\n",
            args->transcode_src, args->transcode_dst, fmt, kbps);

    int rc = transcode_audio(args->transcode_src, args->transcode_dst, &tc);
    config_free(&cfg);

    if (rc != 0) return 1;

    /* Stat the dst so we can print a quick savings summary; non-fatal. */
    struct stat dst_st;
    if (stat(args->transcode_dst, &dst_st) == 0 && st.st_size > 0) {
        double ratio = (double) dst_st.st_size / (double) st.st_size;
        fprintf(stderr,
                "transcode: %lld -> %lld bytes (%.1f%% of original)\n",
                (long long) st.st_size, (long long) dst_st.st_size,
                ratio * 100.0);
    }
    return 0;
}
