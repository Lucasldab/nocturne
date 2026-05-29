/*
 * download.c — phone-initiated download dispatcher.
 *
 * Per download.h: scans meta_dir for downloads-phone-*.jsonl request lines,
 * dedups against terminal-state entries in downloads-desktop.jsonl, and
 * shells out to `flacget` once per fresh id.
 *
 * No DB writes. Status flows back to the phone via JSONL only.
 */

#define _GNU_SOURCE

#include "download.h"
#include "jsonl.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* === id-set: closed-addressing hash of completed request ids ============ */

#define IDSET_BUCKETS 1024

struct idset_node {
    char *id;
    struct idset_node *next;
};

struct idset {
    struct idset_node *buckets[IDSET_BUCKETS];
};

static unsigned long idset_hash(const char *s)
{
    /* djb2 — adequate for short uuid-shaped strings. */
    unsigned long h = 5381;
    for (; *s; s++) h = ((h << 5) + h) + (unsigned char) *s;
    return h;
}

static int idset_contains(const struct idset *s, const char *id)
{
    if (!s || !id) return 0;
    struct idset_node *n = s->buckets[idset_hash(id) % IDSET_BUCKETS];
    while (n) {
        if (!strcmp(n->id, id)) return 1;
        n = n->next;
    }
    return 0;
}

static int idset_add(struct idset *s, const char *id)
{
    if (idset_contains(s, id)) return 0;
    struct idset_node *n = malloc(sizeof(*n));
    if (!n) return -1;
    n->id = strdup(id);
    if (!n->id) { free(n); return -1; }
    unsigned long b = idset_hash(id) % IDSET_BUCKETS;
    n->next = s->buckets[b];
    s->buckets[b] = n;
    return 0;
}

static void idset_free(struct idset *s)
{
    if (!s) return;
    for (size_t i = 0; i < IDSET_BUCKETS; i++) {
        struct idset_node *n = s->buckets[i];
        while (n) {
            struct idset_node *nx = n->next;
            free(n->id);
            free(n);
            n = nx;
        }
        s->buckets[i] = NULL;
    }
}

/* === time helpers ======================================================== */

static long long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long) tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* === status file ========================================================= */

/* Append one status line to <meta_dir>/downloads-desktop.jsonl. Returns 0
 * on success, -1 on I/O error. Best-effort fsync to keep the phone reader
 * from missing a freshly-appended line. */
static int status_append(const char *meta_dir, const char *id,
                         const char *state, const char *msg)
{
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/downloads-desktop.jsonl", meta_dir);
    if (n < 0 || (size_t) n >= sizeof(path)) return -1;

    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        fprintf(stderr, "download: open %s: %s\n", path, strerror(errno));
        return -1;
    }

    json_t *obj = json_object();
    if (!obj) { close(fd); return -1; }
    json_object_set_new(obj, "v",     json_integer(1));
    json_object_set_new(obj, "id",    json_string(id));
    json_object_set_new(obj, "state", json_string(state));
    json_object_set_new(obj, "ts",    json_integer(now_ms()));
    if (msg && *msg) json_object_set_new(obj, "msg", json_string(msg));

    char *line = json_dumps(obj, JSON_COMPACT);
    json_decref(obj);
    if (!line) { close(fd); return -1; }

    size_t len = strlen(line);
    ssize_t w1 = write(fd, line, len);
    ssize_t w2 = write(fd, "\n", 1);
    free(line);
    if (w1 != (ssize_t) len || w2 != 1) {
        fprintf(stderr, "download: write %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    fsync(fd);
    close(fd);
    return 0;
}

/* === status-set load: parse desktop log, fill `done_ids` with any id whose
 *     most-recent state is `done` or `error` ================================ */

static int load_terminal_ids(const char *meta_dir, struct idset *out)
{
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/downloads-desktop.jsonl", meta_dir);
    if (n < 0 || (size_t) n >= sizeof(path)) return -1;

    struct stat st;
    if (stat(path, &st) != 0) {
        return 0; /* file doesn't exist yet — nothing terminal */
    }

    struct jsonl_reader *jr = jsonl_open(path, 0);
    if (!jr) {
        fprintf(stderr, "download: open %s: %s\n", path, strerror(errno));
        return -1;
    }
    int rc;
    for (;;) {
        const char *line = NULL;
        size_t llen = 0;
        rc = jsonl_read_line(jr, &line, &llen);
        if (rc == 0) break;
        if (rc < 0) {
            if (errno == EMSGSIZE) continue;
            fprintf(stderr, "download: read %s: %s\n", path, strerror(errno));
            break;
        }
        if (llen == 0) continue;
        json_error_t jerr;
        json_t *root = json_loadb(line, llen, 0, &jerr);
        if (!root || !json_is_object(root)) { if (root) json_decref(root); continue; }
        json_t *jid    = json_object_get(root, "id");
        json_t *jstate = json_object_get(root, "state");
        if (json_is_string(jid) && json_is_string(jstate)) {
            const char *state = json_string_value(jstate);
            if (!strcmp(state, "done") || !strcmp(state, "error")) {
                idset_add(out, json_string_value(jid));
            }
        }
        json_decref(root);
    }
    jsonl_close(jr);
    return 0;
}

/* === exec flacget ========================================================
 *
 * fork/exec wrapper. Returns the child's exit status (0 on success), or
 * -1 if the fork itself failed or the child was killed by a signal.
 *
 * Stdin is /dev/null. Stdout + stderr are inherited so the daemon's log
 * captures the wrapper's progress output verbatim — same UX as running
 * flacget interactively. */
static int run_flacget(const char *flacget_path, const char *query)
{
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "download: fork: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        /* child */
        int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (devnull >= 0) {
            dup2(devnull, 0);
            close(devnull);
        }
        char *const argv[] = { (char *) flacget_path, (char *) query, NULL };
        execv(flacget_path, argv);
        fprintf(stderr, "download: exec %s: %s\n", flacget_path, strerror(errno));
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        fprintf(stderr, "download: waitpid: %s\n", strerror(errno));
        return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "download: flacget killed by signal %d\n", WTERMSIG(status));
        return -1;
    }
    return -1;
}

/* === per-request handler ================================================= */

/* Parse one request line and dispatch if the id hasn't been completed yet.
 * Returns 0 on success (whether the request was processed or skipped), 1
 * on per-line validation failure, -1 on fatal error. */
static int handle_request_line(const char *line, size_t llen,
                               const char *meta_dir,
                               const char *flacget_path,
                               struct idset *done,
                               struct download_stats *stats)
{
    json_error_t jerr;
    json_t *root = json_loadb(line, llen, 0, &jerr);
    if (!root) {
        fprintf(stderr, "download: parse: %s\n", jerr.text);
        return 1;
    }
    if (!json_is_object(root)) { json_decref(root); return 1; }

    json_t *jv     = json_object_get(root, "v");
    json_t *jid    = json_object_get(root, "id");
    json_t *jquery = json_object_get(root, "query");

    int ok = json_is_integer(jv) && json_integer_value(jv) == 1
          && json_is_string(jid)
          && json_is_string(jquery);
    if (!ok) { json_decref(root); return 1; }

    const char *id    = json_string_value(jid);
    const char *query = json_string_value(jquery);
    if (!id || !*id || !query || !*query) { json_decref(root); return 1; }

    stats->requests_seen++;

    if (idset_contains(done, id)) {
        stats->requests_skipped_done++;
        json_decref(root);
        return 0;
    }

    /* Mark as in-flight before exec so the phone sees movement even if
     * flacget runs for several minutes. */
    status_append(meta_dir, id, "running", NULL);

    int rc = run_flacget(flacget_path, query);
    if (rc == 0) {
        status_append(meta_dir, id, "done", NULL);
        idset_add(done, id);
        stats->requests_processed_ok++;
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "flacget rc=%d", rc);
        status_append(meta_dir, id, "error", buf);
        idset_add(done, id);
        stats->requests_processed_err++;
    }
    json_decref(root);
    return 0;
}

/* === per-file scan ======================================================= */

static int scan_request_file(const char *path,
                             const char *meta_dir,
                             const char *flacget_path,
                             struct idset *done,
                             struct download_stats *stats)
{
    struct jsonl_reader *jr = jsonl_open(path, 0);
    if (!jr) {
        fprintf(stderr, "download: open %s: %s\n", path, strerror(errno));
        return 0; /* per-file recoverable */
    }
    int rc;
    for (;;) {
        const char *line = NULL;
        size_t llen = 0;
        rc = jsonl_read_line(jr, &line, &llen);
        if (rc == 0) break;
        if (rc < 0) {
            if (errno == EMSGSIZE) {
                stats->lines_skipped_parse_error++;
                continue;
            }
            fprintf(stderr, "download: read %s: %s\n", path, strerror(errno));
            break;
        }
        if (llen == 0) continue;
        int hr = handle_request_line(line, llen, meta_dir, flacget_path,
                                     done, stats);
        if (hr == 1) stats->lines_skipped_parse_error++;
        else if (hr < 0) { jsonl_close(jr); return -1; }
    }
    jsonl_close(jr);
    return 0;
}

/* === public entry ======================================================== */

int download_run(const char *meta_dir, const char *flacget_path,
                 struct download_stats *stats_out)
{
    struct download_stats local = {0};
    struct download_stats *stats = stats_out ? stats_out : &local;
    memset(stats, 0, sizeof(*stats));

    if (!meta_dir || !*meta_dir) return 0;
    if (!flacget_path || !*flacget_path) {
        fprintf(stderr, "download: no flacget_path configured\n");
        return -1;
    }

    /* Build the dedup set up front so re-running on the same input set is a
     * no-op. */
    struct idset done = {{0}};
    if (load_terminal_ids(meta_dir, &done) != 0) {
        idset_free(&done);
        return -1;
    }

    char pattern[4096];
    int n = snprintf(pattern, sizeof(pattern),
                     "%s/downloads-phone-*.jsonl", meta_dir);
    if (n < 0 || (size_t) n >= sizeof(pattern)) {
        idset_free(&done);
        return -1;
    }

    glob_t gl;
    int gr = glob(pattern, 0, NULL, &gl);
    if (gr == GLOB_NOMATCH) {
        idset_free(&done);
        return 0;
    }
    if (gr != 0) {
        fprintf(stderr, "download: glob('%s') failed (code=%d)\n", pattern, gr);
        idset_free(&done);
        return 0;
    }

    int rc = 0;
    for (size_t i = 0; i < gl.gl_pathc; i++) {
        if (scan_request_file(gl.gl_pathv[i], meta_dir, flacget_path,
                              &done, stats) < 0) {
            rc = -1;
        }
    }
    globfree(&gl);
    idset_free(&done);
    return rc;
}
