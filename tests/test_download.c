/*
 * test_download.c — download_run() against a fake flacget.
 *
 * Behaviours under test:
 *   1. flacget exit 0 + result file -> "done" line carries the result msg.
 *   2. flacget exit 1 + result file -> "error" line carries the result msg.
 *   3. exit 1, no result file      -> "error" falls back to "flacget rc=1".
 *   4. exit 1, invalid result file -> same fallback, no crash.
 *   5. exit 3, no result file      -> the existing "not filed" explanation.
 *   6. exit 0, no result file      -> "done" with no msg (old flacget).
 *   7. Every request gets a "running" line before its terminal line.
 *   8. Result temp files are removed from $TMPDIR afterwards.
 *   9. A second run is a no-op (terminal ids are skipped).
 *  10. A msg longer than the buffer, in multibyte UTF-8, arrives truncated
 *      at a character boundary instead of being dropped.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "download.h"
#include "runner.h"

static void write_file(const char *path, const char *content, mode_t mode)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(content, f);
    fclose(f);
    chmod(path, mode);
}

static const char *FAKE =
    "#!/bin/sh\n"
    "case \"$1\" in\n"
    "  ok*)       printf '%s' '{\"ok\":true,\"reason\":\"fetched\","
    "\"msg\":\"Numb \\u00b7 CD 16/44.1 \\u00b7 3:07\"}' > \"$FLACGET_RESULT_FILE\"; exit 0;;\n"
    "  fail*)     printf '%s' '{\"ok\":false,\"reason\":\"no_flac\","
    "\"msg\":\"3 users have it, none as FLAC (lossy only)\"}' > \"$FLACGET_RESULT_FILE\"; exit 1;;\n"
    "  silent*)   exit 1;;\n"
    "  garbage*)  printf 'not json' > \"$FLACGET_RESULT_FILE\"; exit 1;;\n"
    "  notfiled*) exit 3;;\n"
    "  oldok*)    exit 0;;\n"
    "  long*)     python3 -c 'import json,os; json.dump({\"ok\":True,\"reason\":\"fetched\","
    "\"msg\":\"\\u3042\"*600}, open(os.environ[\"FLACGET_RESULT_FILE\"],\"w\"))'; exit 0;;\n"
    "esac\n"
    "exit 9\n";

/* Terminal (last) state + msg for `id`; `running` set if a running line was seen. */
static void last_status(const char *desktop, const char *id,
                        char *state, size_t scap, char *msg, size_t mcap,
                        int *running)
{
    state[0] = msg[0] = '\0';
    *running = 0;
    FILE *f = fopen(desktop, "r");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        json_t *o = json_loads(line, 0, NULL);
        if (!o) continue;
        const char *lid = json_string_value(json_object_get(o, "id"));
        if (lid && !strcmp(lid, id)) {
            const char *st = json_string_value(json_object_get(o, "state"));
            const char *m = json_string_value(json_object_get(o, "msg"));
            if (st && !strcmp(st, "running")) *running = 1;
            snprintf(state, scap, "%s", st ? st : "");
            snprintf(msg, mcap, "%s", m ? m : "");
        }
        json_decref(o);
    }
    fclose(f);
}

static int count_result_temps(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (!strncmp(e->d_name, "nocturne-flacget-", 17)) n++;
    closedir(d);
    return n;
}

int main(void)
{
    char root[] = "/tmp/nocturne-dl-XXXXXX";
    if (!mkdtemp(root)) { expect(0, "mkdtemp"); return test_finish("test_download"); }
    char meta[512], tmpd[512], fake[512], req[600], desk[600];
    snprintf(meta, sizeof(meta), "%s/meta", root);
    snprintf(tmpd, sizeof(tmpd), "%s/tmp", root);
    snprintf(fake, sizeof(fake), "%s/flacget", root);
    mkdir(meta, 0755);
    mkdir(tmpd, 0755);
    setenv("TMPDIR", tmpd, 1);
    write_file(fake, FAKE, 0755);

    snprintf(req, sizeof(req), "%s/downloads-phone-T1.jsonl", meta);
    write_file(req,
        "{\"v\":1,\"id\":\"r-ok\",\"query\":\"ok numb\"}\n"
        "{\"v\":1,\"id\":\"r-fail\",\"query\":\"fail something\"}\n"
        "{\"v\":1,\"id\":\"r-silent\",\"query\":\"silent x\"}\n"
        "{\"v\":1,\"id\":\"r-garbage\",\"query\":\"garbage x\"}\n"
        "{\"v\":1,\"id\":\"r-notfiled\",\"query\":\"notfiled x\"}\n"
        "{\"v\":1,\"id\":\"r-oldok\",\"query\":\"oldok x\"}\n"
        "{\"v\":1,\"id\":\"r-long\",\"query\":\"long x\"}\n", 0644);
    snprintf(desk, sizeof(desk), "%s/downloads-desktop.jsonl", meta);

    struct download_stats st = {0};
    expect(download_run(meta, fake, &st) == 0, "download_run returns 0");
    expect(st.requests_processed_ok == 3 && st.requests_processed_err == 4,
           "3 ok, 4 errors");

    char state[32], msg[512];
    int running;
    last_status(desk, "r-ok", state, sizeof(state), msg, sizeof(msg), &running);
    expect(!strcmp(state, "done"), "ok: done");
    expect(!strcmp(msg, "Numb · CD 16/44.1 · 3:07"), "ok: result msg forwarded on done");
    expect(running, "ok: running line written first");

    last_status(desk, "r-fail", state, sizeof(state), msg, sizeof(msg), &running);
    expect(!strcmp(state, "error"), "fail: error");
    expect(!strcmp(msg, "3 users have it, none as FLAC (lossy only)"),
           "fail: result msg forwarded on error");

    last_status(desk, "r-silent", state, sizeof(state), msg, sizeof(msg), &running);
    expect(!strcmp(state, "error") && !strcmp(msg, "flacget rc=1"),
           "no result file: falls back to flacget rc=1");

    last_status(desk, "r-garbage", state, sizeof(state), msg, sizeof(msg), &running);
    expect(!strcmp(state, "error") && !strcmp(msg, "flacget rc=1"),
           "invalid result file: falls back to flacget rc=1");

    last_status(desk, "r-notfiled", state, sizeof(state), msg, sizeof(msg), &running);
    expect(!strcmp(state, "error") && strstr(msg, "staging/skipped") != NULL,
           "rc=3 without result file: not-filed explanation kept");

    last_status(desk, "r-oldok", state, sizeof(state), msg, sizeof(msg), &running);
    expect(!strcmp(state, "done") && msg[0] == '\0',
           "rc=0 without result file: done, no msg");

    char big[4096];
    {
        state[0] = big[0] = '\0';
        last_status(desk, "r-long", state, sizeof(state), big, sizeof(big), &running);
        size_t bl = strlen(big);
        int all_a = bl > 0 && bl % 3 == 0;
        for (size_t k = 0; all_a && k < bl; k += 3)
            all_a = !memcmp(big + k, "\u3042", 3);
        expect(!strcmp(state, "done") && bl > 500 && bl < 1024 && all_a,
               "long multibyte msg truncated at a character boundary, not dropped");
    }

    expect(count_result_temps(tmpd) == 0, "result temp files cleaned up");

    memset(&st, 0, sizeof(st));
    expect(download_run(meta, fake, &st) == 0 && st.requests_skipped_done == 7 &&
           st.requests_processed_ok == 0 && st.requests_processed_err == 0,
           "second run skips all terminal ids");

    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    (void) system(cmd);
    return test_finish("test_download");
}
