/*
 * loudness.c — measure integrated loudness with ffmpeg and turn it into a
 * per-track gain for the resident transcode.
 *
 * Measurement is ffmpeg's loudnorm filter in analysis mode (EBU R128
 * integrated loudness + true peak), whose JSON summary lands on stderr:
 *
 *   ffmpeg -nostdin -hide_banner -nostats -i SRC -map 0:a
 *          -af loudnorm=print_format=json -f null -
 *
 * Only the two numbers are used; loudnorm's own (dynamic) normalisation is
 * never applied — the encode uses a plain `volume=` filter so the sound is
 * only ever scaled, not compressed.
 */

#define _GNU_SOURCE

#include "loudness.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

double loudness_gain(double i, double tp, double target)
{
    if (isnan(i) || isinf(i) || i <= LOUDNESS_SILENCE_LUFS) return 0.0;
    double g = target - i;
    if (!isnan(tp) && !isinf(tp)) {
        double ceiling = LOUDNESS_PEAK_CEILING_DBTP - tp;
        if (g > ceiling) g = ceiling;
    }
    if (g > LOUDNESS_MAX_BOOST_DB) g = LOUDNESS_MAX_BOOST_DB;
    if (g < LOUDNESS_MAX_CUT_DB)   g = LOUDNESS_MAX_CUT_DB;
    return g;
}

/* Find `"key" : "<number>"` and strtod the number. 0 on success. */
static int read_key(const char *text, const char *key, double *out)
{
    const char *p = strstr(text, key);
    if (!p) return -1;
    p += strlen(key);
    p = strchr(p, ':');
    if (!p) return -1;
    p = strchr(p, '"');
    if (!p) return -1;
    p++;
    char *end = NULL;
    errno = 0;
    double v = strtod(p, &end);
    if (end == p || errno == ERANGE) return -1;
    *out = v;
    return 0;
}

int loudness_parse(const char *text, double *i_out, double *tp_out)
{
    if (!text || !i_out || !tp_out) return -1;
    double i, tp;
    if (read_key(text, "\"input_i\"", &i) != 0) return -1;
    if (read_key(text, "\"input_tp\"", &tp) != 0) return -1;
    *i_out = i;
    *tp_out = tp;
    return 0;
}

int loudness_measure(const char *src, double *i_out, double *tp_out)
{
    if (!src || !i_out || !tp_out) { errno = EINVAL; return -1; }

    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) != 0) {
        perror("loudness: pipe");
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        perror("loudness: fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(pipefd[1], STDERR_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, STDOUT_FILENO);
        char *argv[] = {
            "ffmpeg", "-nostdin", "-hide_banner", "-nostats",
            "-i", (char *) src, "-map", "0:a",
            "-af", "loudnorm=print_format=json",
            "-f", "null", "-", NULL,
        };
        execvp("ffmpeg", argv);
        _exit(127);
    }
    close(pipefd[1]);

    size_t cap = 16384, len = 0;
    char *buf = malloc(cap);
    int ok = buf != NULL;
    for (;;) {
        if (ok && len + 4096 + 1 > cap) {
            if (cap >= (1u << 20)) {
                /* Keep only the tail: the summary is printed last. */
                memmove(buf, buf + len / 2, len - len / 2);
                len -= len / 2;
            } else {
                char *nb = realloc(buf, cap * 2);
                if (nb) { buf = nb; cap *= 2; } else ok = 0;
            }
        }
        char scratch[4096];
        ssize_t r = read(pipefd[0], ok ? buf + len : scratch, 4096);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        if (ok) len += (size_t) r;
    }
    close(pipefd[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}

    int rc = -1;
    if (ok && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        buf[len] = '\0';
        rc = loudness_parse(buf, i_out, tp_out);
        if (rc != 0) fprintf(stderr, "loudness: no loudnorm summary for %s\n", src);
    } else {
        fprintf(stderr, "loudness: ffmpeg analysis failed for %s\n", src);
    }
    free(buf);
    return rc;
}
