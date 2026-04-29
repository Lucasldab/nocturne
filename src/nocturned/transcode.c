/*
 * transcode.c — fork+execvp wrapper around ffmpeg.
 *
 * Phase: per-resident lossy re-encode. Bluetooth audio re-encodes anything
 * to SBC/AAC anyway, so shipping FLAC bytes to the phone wastes Syncthing
 * bandwidth and resident-cap storage. This module performs the encode; it
 * does NOT decide WHEN to encode (that's rotate's job, or a one-shot CLI
 * for hand-testing) and it does NOT touch the DB.
 *
 * ffmpeg flags:
 *   -nostdin               never block on stdin (we exec without a tty)
 *   -loglevel error        suppress informational chatter; warnings + errors only
 *   -y                     overwrite dst
 *   -i <src>               input
 *   -map 0:a -map 0:v?     copy audio + (optional) embedded cover stream
 *   -c:v copy              don't re-encode the cover bitmap
 *   -c:a libopus           or `aac` — see codec_for_format()
 *   -b:a <kbps>k           target bitrate
 *   -vbr on                only meaningful for libopus; harmless for aac
 *   -compression_level 10  libopus knob; ignored by aac
 *   -map_metadata 0        copy tags
 *
 * Errors propagate by ffmpeg's exit code; we return -2 on any non-zero. Our
 * own fork/exec failures return -1 with errno preserved for the caller.
 */

#define _GNU_SOURCE

#include "transcode.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

const char *transcode_dst_ext(const struct transcode_cfg *cfg)
{
    if (!cfg || !cfg->format) return NULL;
    if (!strcmp(cfg->format, "opus")) return ".opus";
    if (!strcmp(cfg->format, "aac"))  return ".m4a";
    if (!strcmp(cfg->format, "flac")) return ".flac";
    return NULL;
}

/* Codec name for ffmpeg's -c:a. NULL on unsupported format. */
static const char *codec_for_format(const char *fmt)
{
    if (!fmt) return NULL;
    if (!strcmp(fmt, "opus")) return "libopus";
    if (!strcmp(fmt, "aac"))  return "aac";
    return NULL;
}

int transcode_audio(const char *src, const char *dst,
                    const struct transcode_cfg *cfg)
{
    if (!src || !dst || !cfg) {
        errno = EINVAL;
        return -1;
    }
    const char *codec = codec_for_format(cfg->format);
    if (!codec) {
        fprintf(stderr,
                "transcode: unsupported format %s (want opus|aac)\n",
                cfg->format ? cfg->format : "(null)");
        errno = EINVAL;
        return -1;
    }
    int kbps = cfg->bitrate_kbps > 0 ? cfg->bitrate_kbps : 128;

    char bitrate_arg[32];
    snprintf(bitrate_arg, sizeof(bitrate_arg), "%dk", kbps);

    /* Cover handling diverges by container:
     *   - Ogg/Opus rejects MJPEG attached_pic streams as "Unsupported codec
     *     id". Embedded art for Opus must be packed into the
     *     METADATA_BLOCK_PICTURE Vorbis comment, which ffmpeg can't do in a
     *     single pass. v1: drop the picture stream (-vn). Phone-side
     *     AlbumArtRepository falls back to album-folder art / generic icon.
     *     v2: post-process with opustags or a second ffmpeg pass to embed.
     *   - M4A/AAC accepts JPEG/PNG attached_pic via -c:v copy. Preserve.
     */
    bool is_opus = !strcmp(cfg->format, "opus");

    char *argv[32];
    int n = 0;
    argv[n++] = "ffmpeg";
    argv[n++] = "-nostdin";
    argv[n++] = "-loglevel"; argv[n++] = "error";
    argv[n++] = "-y";
    argv[n++] = "-i"; argv[n++] = (char *) src;
    argv[n++] = "-map"; argv[n++] = "0:a";
    if (is_opus) {
        argv[n++] = "-vn";
    } else {
        argv[n++] = "-map"; argv[n++] = "0:v?";
        argv[n++] = "-c:v"; argv[n++] = "copy";
        argv[n++] = "-disposition:v"; argv[n++] = "attached_pic";
    }
    argv[n++] = "-c:a"; argv[n++] = (char *) codec;
    argv[n++] = "-b:a"; argv[n++] = bitrate_arg;
    if (is_opus) {
        argv[n++] = "-vbr"; argv[n++] = "on";
        argv[n++] = "-compression_level"; argv[n++] = "10";
    }
    argv[n++] = "-map_metadata"; argv[n++] = "0";
    argv[n++] = (char *) dst;
    argv[n] = NULL;

    pid_t pid = fork();
    if (pid < 0) {
        perror("transcode: fork");
        return -1;
    }
    if (pid == 0) {
        /* child */
        execvp("ffmpeg", argv);
        /* exec only returns on error */
        fprintf(stderr, "transcode: execvp(ffmpeg) failed: %s\n", strerror(errno));
        _exit(127);
    }

    /* parent — wait for child. */
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        perror("transcode: waitpid");
        return -1;
    }
    if (WIFEXITED(status)) {
        int rc = WEXITSTATUS(status);
        if (rc == 0) return 0;
        fprintf(stderr, "transcode: ffmpeg exited %d\n", rc);
        return -2;
    }
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "transcode: ffmpeg killed by signal %d\n", WTERMSIG(status));
        return -2;
    }
    /* Stopped/continued shouldn't happen with default waitpid flags. */
    fprintf(stderr, "transcode: ffmpeg ended with unexpected status 0x%x\n", status);
    return -2;
}
