/*
 * test_why.c — golden-output coverage for `nocturned why <track-id>`.
 *
 * Behaviours under test (≥ 7 named cases, per plan 08-01 acceptance):
 *   1. test_why_hit_full_sha          — exact 64-char id matches → text + json
 *   2. test_why_hit_prefix            — 8-char unique prefix matches
 *   3. test_why_miss                  — id absent from manifest → exit 1
 *   4. test_why_short_prefix_usage_error — prefix < 8 chars → exit 64
 *   5. test_why_unreadable_manifest   — bad path → exit 1, no resolve fallback
 *   6. test_why_ambiguous_prefix      — 8-char prefix matches >=2 → exit 1
 *   7. test_why_json_output_shape     — --json single-line {"id":...,"buckets":[...]}
 *
 * Strategy: drive why_cmd_main() directly with a struct cli_args literal,
 * pointing manifest_path_override at a temp file we write per-case. We
 * dup2 stdout/stderr to pipes to capture output.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cli.h"
#include "runner.h"

/* Forward decl — why_cmd_main is in nocturned-obj/why_cmd.o, linked by the
 * test rule. cli.h does not export it (only cli_parse), so declare here. */
int why_cmd_main(struct cli_args *args);

/* Build a 64-char lowercase hex id by repeating the prefix and padding. */
static char *make_id(const char *first8, char filler, int suffix)
{
    char *id = malloc(65);
    if (!id) return NULL;
    int n = snprintf(id, 9, "%s", first8);
    (void) n;
    for (int i = 8; i < 62; i++) id[i] = filler;
    snprintf(id + 62, 3, "%02d", suffix);
    id[64] = '\0';
    return id;
}

/* Write a minimal manifest.json with two resident entries to `path`.
 *   id1 / id2 must be 64-char hex. buckets1 / buckets2 are JSON-array
 *   payloads (e.g. "[\"loved\",\"top_played\"]"). */
static int write_manifest(const char *path,
                          const char *id1, const char *buckets1,
                          const char *id2, const char *buckets2)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f,
        "{\n"
        "  \"v\": 1,\n"
        "  \"generated_at\": \"2026-04-27T00:00:00Z\",\n"
        "  \"cap_bytes\": 12884901888,\n"
        "  \"used_bytes\": 1024,\n"
        "  \"resident\": [\n"
        "    {\"id\": \"%s\", \"buckets\": %s},\n"
        "    {\"id\": \"%s\", \"buckets\": %s}\n"
        "  ]\n"
        "}\n", id1, buckets1, id2, buckets2);
    fclose(f);
    return 0;
}

static char *make_tmpdir(void)
{
    char tmpl[] = "/tmp/nocturne-why-XXXXXX";
    char *p = mkdtemp(tmpl);
    if (!p) return NULL;
    return strdup(p);
}

static int rm_rf(const char *dir)
{
    if (!dir || strncmp(dir, "/tmp/", 5) != 0) return -1;
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf -- %s", dir);
    return system(cmd);
}

/* Run why_cmd_main with the given args, capturing stdout + stderr into
 * heap buffers. Returns the exit code. Caller frees *out_stdout / *out_stderr. */
static int run_why_capture(struct cli_args *args,
                           char **out_stdout, char **out_stderr)
{
    int sp[2], ep[2];
    if (pipe(sp) != 0) return -99;
    if (pipe(ep) != 0) { close(sp[0]); close(sp[1]); return -99; }

    int saved_out = dup(STDOUT_FILENO);
    int saved_err = dup(STDERR_FILENO);
    fflush(stdout); fflush(stderr);
    dup2(sp[1], STDOUT_FILENO);
    dup2(ep[1], STDERR_FILENO);
    close(sp[1]); close(ep[1]);

    int rc = why_cmd_main(args);

    fflush(stdout); fflush(stderr);
    dup2(saved_out, STDOUT_FILENO); close(saved_out);
    dup2(saved_err, STDERR_FILENO); close(saved_err);

    /* Drain both pipes (non-blocking-ish: small buffers, short outputs). */
    char buf[4096];
    char *so = calloc(1, 1); size_t so_n = 0;
    ssize_t r;
    /* Make read non-blocking by setting O_NONBLOCK on the read end. */
    int flags = fcntl(sp[0], F_GETFL, 0); fcntl(sp[0], F_SETFL, flags | O_NONBLOCK);
    while ((r = read(sp[0], buf, sizeof(buf))) > 0) {
        so = realloc(so, so_n + (size_t) r + 1);
        memcpy(so + so_n, buf, (size_t) r);
        so_n += (size_t) r;
        so[so_n] = '\0';
    }
    close(sp[0]);

    char *se = calloc(1, 1); size_t se_n = 0;
    flags = fcntl(ep[0], F_GETFL, 0); fcntl(ep[0], F_SETFL, flags | O_NONBLOCK);
    while ((r = read(ep[0], buf, sizeof(buf))) > 0) {
        se = realloc(se, se_n + (size_t) r + 1);
        memcpy(se + se_n, buf, (size_t) r);
        se_n += (size_t) r;
        se[se_n] = '\0';
    }
    close(ep[0]);

    *out_stdout = so;
    *out_stderr = se;
    return rc;
}

int main(int argc, char **argv)
{
    (void) argc; (void) argv;

    /* test_why_hit_full_sha + test_why_json_output_shape */
    {
        char *dir = make_tmpdir();
        char path[1024];
        snprintf(path, sizeof(path), "%s/manifest.json", dir);

        char *id1 = make_id("aaaaaaaa", 'a', 1);
        char *id2 = make_id("bbbbbbbb", 'b', 2);
        write_manifest(path,
            id1, "[\"loved\",\"top_played\"]",
            id2, "[\"recent_adds\"]");

        struct cli_args args = {0};
        args.cmd = CMD_WHY;
        args.track_id = id1;
        args.manifest_path_override = path;

        char *so = NULL, *se = NULL;
        int rc = run_why_capture(&args, &so, &se);
        expect(rc == 0, "test_why_hit_full_sha: exit 0");
        expect(so && strstr(so, id1) != NULL,
               "test_why_hit_full_sha: stdout contains the full id");
        expect(so && strstr(so, "loved") != NULL,
               "test_why_hit_full_sha: stdout contains 'loved' bucket");
        expect(so && strstr(so, "top_played") != NULL,
               "test_why_hit_full_sha: stdout contains 'top_played' bucket");
        free(so); free(se);

        /* JSON variant — same data, --json. */
        args.json = 1;
        rc = run_why_capture(&args, &so, &se);
        expect(rc == 0, "test_why_json_output_shape: exit 0");
        expect(so && so[0] == '{' && strstr(so, "\"id\"") != NULL,
               "test_why_json_output_shape: starts with { and has id key");
        expect(so && strstr(so, "\"buckets\"") != NULL,
               "test_why_json_output_shape: has buckets key");
        expect(so && strstr(so, "\"loved\"") != NULL && strstr(so, "\"top_played\"") != NULL,
               "test_why_json_output_shape: contains both bucket names");
        /* Single line + trailing newline. */
        size_t slen = so ? strlen(so) : 0;
        int newlines = 0;
        for (size_t i = 0; i < slen; i++) if (so[i] == '\n') newlines++;
        expect(newlines == 1,
               "test_why_json_output_shape: single trailing newline (one line)");
        free(so); free(se);

        free(id1); free(id2);
        rm_rf(dir); free(dir);
    }

    /* test_why_hit_prefix */
    {
        char *dir = make_tmpdir();
        char path[1024];
        snprintf(path, sizeof(path), "%s/manifest.json", dir);

        char *id1 = make_id("aaaaaaaa", 'a', 1);
        char *id2 = make_id("bbbbbbbb", 'b', 2);
        write_manifest(path,
            id1, "[\"loved\",\"top_played\"]",
            id2, "[\"recent_adds\"]");

        struct cli_args args = {0};
        args.cmd = CMD_WHY;
        args.track_id = "aaaaaaaa";  /* 8-char unique prefix */
        args.manifest_path_override = path;

        char *so = NULL, *se = NULL;
        int rc = run_why_capture(&args, &so, &se);
        expect(rc == 0, "test_why_hit_prefix: exit 0 on 8-char unique prefix");
        expect(so && strstr(so, id1) != NULL,
               "test_why_hit_prefix: prints the matched full id");
        expect(so && strstr(so, "loved") != NULL,
               "test_why_hit_prefix: stdout includes 'loved'");
        free(so); free(se);

        free(id1); free(id2);
        rm_rf(dir); free(dir);
    }

    /* test_why_miss */
    {
        char *dir = make_tmpdir();
        char path[1024];
        snprintf(path, sizeof(path), "%s/manifest.json", dir);

        char *id1 = make_id("aaaaaaaa", 'a', 1);
        char *id2 = make_id("bbbbbbbb", 'b', 2);
        write_manifest(path,
            id1, "[\"loved\"]",
            id2, "[\"recent_adds\"]");

        char *miss = make_id("deadbeef", 'd', 9);

        struct cli_args args = {0};
        args.cmd = CMD_WHY;
        args.track_id = miss;
        args.manifest_path_override = path;

        char *so = NULL, *se = NULL;
        int rc = run_why_capture(&args, &so, &se);
        expect(rc == 1, "test_why_miss: exit 1 (track not in manifest)");
        expect(so == NULL || so[0] == '\0',
               "test_why_miss: stdout is empty");
        expect(se && (strstr(se, "not in manifest") != NULL ||
                      strstr(se, "not resident") != NULL),
               "test_why_miss: stderr names the failure");
        free(so); free(se);

        free(id1); free(id2); free(miss);
        rm_rf(dir); free(dir);
    }

    /* test_why_short_prefix_usage_error */
    {
        char *dir = make_tmpdir();
        char path[1024];
        snprintf(path, sizeof(path), "%s/manifest.json", dir);

        char *id1 = make_id("aaaaaaaa", 'a', 1);
        char *id2 = make_id("bbbbbbbb", 'b', 2);
        write_manifest(path,
            id1, "[\"loved\"]",
            id2, "[\"recent_adds\"]");

        struct cli_args args = {0};
        args.cmd = CMD_WHY;
        args.track_id = "aa";  /* < 8 chars */
        args.manifest_path_override = path;

        char *so = NULL, *se = NULL;
        int rc = run_why_capture(&args, &so, &se);
        expect(rc == 64,
               "test_why_short_prefix_usage_error: exit 64 (NOCT_EXIT_USAGE)");
        expect(se && strstr(se, "8") != NULL,
               "test_why_short_prefix_usage_error: stderr mentions 8-char minimum");
        free(so); free(se);

        free(id1); free(id2);
        rm_rf(dir); free(dir);
    }

    /* test_why_unreadable_manifest */
    {
        struct cli_args args = {0};
        args.cmd = CMD_WHY;
        char *id = make_id("aaaaaaaa", 'a', 1);
        args.track_id = id;
        args.manifest_path_override = "/tmp/nocturne-why-does-not-exist-1234567890.json";

        char *so = NULL, *se = NULL;
        int rc = run_why_capture(&args, &so, &se);
        expect(rc == 1,
               "test_why_unreadable_manifest: exit 1 (NOCT_EXIT_FAILURE)");
        expect(se && strstr(se, "/tmp/nocturne-why-does-not-exist") != NULL,
               "test_why_unreadable_manifest: stderr names the bad path");
        free(so); free(se);
        free(id);
    }

    /* test_why_ambiguous_prefix */
    {
        char *dir = make_tmpdir();
        char path[1024];
        snprintf(path, sizeof(path), "%s/manifest.json", dir);

        /* Two ids that share the 8-char prefix "ffffffff" but differ in
         * the rest. We use distinct fillers + suffixes to keep them
         * uniquely 64-char. */
        char *id1 = make_id("ffffffff", '1', 1);  /* "ffffffff" + 54 '1's + "01" */
        char *id2 = make_id("ffffffff", '2', 2);  /* "ffffffff" + 54 '2's + "02" */
        write_manifest(path,
            id1, "[\"loved\"]",
            id2, "[\"recent_adds\"]");

        struct cli_args args = {0};
        args.cmd = CMD_WHY;
        args.track_id = "ffffffff";  /* 8-char ambiguous prefix */
        args.manifest_path_override = path;

        char *so = NULL, *se = NULL;
        int rc = run_why_capture(&args, &so, &se);
        expect(rc == 1, "test_why_ambiguous_prefix: exit 1");
        expect(se && strstr(se, "ambiguous") != NULL,
               "test_why_ambiguous_prefix: stderr mentions 'ambiguous'");
        free(so); free(se);

        free(id1); free(id2);
        rm_rf(dir); free(dir);
    }

    return test_finish(__FILE__);
}
