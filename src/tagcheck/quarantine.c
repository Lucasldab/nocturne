/*
 * quarantine.c — atomic-rename quarantine + append-only log.
 *
 * Locked behaviours from CONTEXT:
 *   - rename(2) only — no copy fallback. EXDEV refused with clear error.
 *   - Refuse if quarantine dir missing → "run with --init-quarantine first".
 *   - Refuse if quarantine_root is under library_root (would re-quarantine
 *     on next run).
 *   - Refuse cross-pair confusion (library == quarantine).
 *   - Atomic per-process append via O_APPEND on the log fd.
 *   - flock(LOCK_EX | LOCK_NB) on the log fd refuses concurrent runs.
 *   - Multi-value FLAGs do NOT move — quarantine_should_move returns
 *     true ONLY for any_fail (which includes tag_read_failed).
 *
 * Pitfall mitigations:
 *   18 (file-replace race): rename(2) is atomic. We stat-then-rename for
 *      collision detection; the gap can produce an unnecessary `.dup`
 *      suffix but never silent overwrite.
 *   19 (TOCTOU on log rewrites): O_APPEND ensures atomic per-write
 *      append within the process; flock fences across processes.
 *   22 (path-bytes hygiene): every path-bearing message uses %.*s.
 */

#define _GNU_SOURCE

#include "quarantine.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "tags.h"

static char *iso8601_utc_now(char *buf, size_t bufsz)
{
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, bufsz, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

int quarantine_create_dir(const char *quarantine_path)
{
    if (!quarantine_path || quarantine_path[0] == '\0') {
        fprintf(stderr, "nocturne-tagcheck: quarantine path is empty\n");
        return 1;
    }

    if (mkdir(quarantine_path, 0700) == 0) return 0;

    if (errno == EEXIST) {
        struct stat st;
        if (stat(quarantine_path, &st) != 0) {
            fprintf(stderr,
                    "nocturne-tagcheck: cannot stat existing quarantine path "
                    "%.*s: %s\n",
                    (int)strlen(quarantine_path), quarantine_path,
                    strerror(errno));
            return 1;
        }
        if (!S_ISDIR(st.st_mode)) {
            fprintf(stderr,
                    "nocturne-tagcheck: quarantine path exists but is not a "
                    "directory: %.*s\n",
                    (int)strlen(quarantine_path), quarantine_path);
            return 1;
        }
        return 0;  /* already a directory — idempotent. */
    }

    fprintf(stderr,
            "nocturne-tagcheck: mkdir failed on %.*s: %s\n",
            (int)strlen(quarantine_path), quarantine_path, strerror(errno));
    return 1;
}

int quarantine_init(struct quarantine_ctx *ctx,
                    const char *library_path,
                    const char *quarantine_path,
                    bool dry_run)
{
    if (!ctx) return 1;
    memset(ctx, 0, sizeof(*ctx));
    ctx->log_fd = -1;
    ctx->dry_run = dry_run;

    if (!library_path) {
        fprintf(stderr, "nocturne-tagcheck: NULL library path\n");
        return 1;
    }
    if (!quarantine_path) {
        fprintf(stderr, "nocturne-tagcheck: NULL quarantine path\n");
        return 1;
    }

    char *lib_real = realpath(library_path, NULL);
    if (!lib_real) {
        fprintf(stderr,
                "nocturne-tagcheck: library inaccessible: %.*s (%s)\n",
                (int)strlen(library_path), library_path, strerror(errno));
        return 1;
    }
    char *quar_real = realpath(quarantine_path, NULL);
    if (!quar_real) {
        fprintf(stderr,
                "nocturne-tagcheck: quarantine directory does not exist: "
                "%.*s; run with --init-quarantine first\n",
                (int)strlen(quarantine_path), quarantine_path);
        free(lib_real);
        return 1;
    }

    /* Refuse library == quarantine. */
    if (strcmp(lib_real, quar_real) == 0) {
        fprintf(stderr,
                "nocturne-tagcheck: library and quarantine paths cannot be "
                "the same: %.*s\n",
                (int)strlen(lib_real), lib_real);
        free(lib_real);
        free(quar_real);
        return 1;
    }

    /* Refuse if quarantine_root is a sub-path of library_root. */
    size_t lib_len = strlen(lib_real);
    if (strncmp(quar_real, lib_real, lib_len) == 0 &&
        (quar_real[lib_len] == '/' || quar_real[lib_len] == '\0')) {
        fprintf(stderr,
                "nocturne-tagcheck: quarantine path %.*s is inside library "
                "%.*s; refusing (would re-walk on next run)\n",
                (int)strlen(quar_real), quar_real,
                (int)strlen(lib_real), lib_real);
        free(lib_real);
        free(quar_real);
        return 1;
    }

    ctx->library_root = lib_real;
    ctx->quarantine_root = quar_real;

    if (!dry_run) {
        if (asprintf(&ctx->log_path, "%s/quarantine.log",
                     ctx->quarantine_root) < 0) {
            ctx->log_path = NULL;
            fprintf(stderr,
                    "nocturne-tagcheck: failed to format log path "
                    "(out of memory?)\n");
            quarantine_close(ctx);
            return 1;
        }
        ctx->log_fd = open(ctx->log_path,
                           O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                           0600);
        if (ctx->log_fd < 0) {
            fprintf(stderr,
                    "nocturne-tagcheck: cannot open log %.*s: %s\n",
                    (int)strlen(ctx->log_path), ctx->log_path,
                    strerror(errno));
            quarantine_close(ctx);
            return 1;
        }
        if (flock(ctx->log_fd, LOCK_EX | LOCK_NB) != 0) {
            fprintf(stderr,
                    "nocturne-tagcheck: another nocturne-tagcheck is "
                    "currently quarantining (lock held on %.*s); refusing "
                    "to run concurrently\n",
                    (int)strlen(ctx->log_path), ctx->log_path);
            quarantine_close(ctx);
            return 1;
        }
        ctx->log = fdopen(ctx->log_fd, "a");
        if (!ctx->log) {
            fprintf(stderr,
                    "nocturne-tagcheck: fdopen failed on log fd: %s\n",
                    strerror(errno));
            quarantine_close(ctx);
            return 1;
        }
        /* Line-buffered so each line is flushed independently. */
        setvbuf(ctx->log, NULL, _IOLBF, 0);

        char ts[32];
        iso8601_utc_now(ts, sizeof(ts));
        fprintf(ctx->log,
                "# nocturne-tagcheck quarantine session start: %s\n", ts);
    }

    return 0;
}

bool quarantine_should_move(const struct check_result *cr)
{
    if (!cr) return false;
    /* Locked rule: only schema FAILs (and tag_read_failed) move. Multi-value
     * FLAGs are advisory only. cr->any_fail is set true for both FAIL issues
     * and tag_read_failed (see check.c). */
    return cr->any_fail || cr->tag_read_failed;
}

/* Recursive mkdir for the directory portion of `target_path`. Treats
 * EEXIST as success, mode 0700 throughout. Returns 0 on success. */
static int mkdir_p_for_file(const char *target_path)
{
    if (!target_path) return -1;
    char *copy = strdup(target_path);
    if (!copy) return -1;
    char *dir = dirname(copy);  /* may modify `copy` on glibc */
    if (!dir) { free(copy); return -1; }

    /* Walk components and mkdir each. */
    char *partial = strdup(dir);
    if (!partial) { free(copy); return -1; }

    int ret = 0;
    /* mkdir each prefix; partial[0] should be '/' for an absolute path. */
    for (char *p = partial + (partial[0] == '/' ? 1 : 0); *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (partial[0] != '\0' && mkdir(partial, 0700) != 0
                    && errno != EEXIST) {
                fprintf(stderr,
                        "nocturne-tagcheck: mkdir failed on %.*s: %s\n",
                        (int)strlen(partial), partial, strerror(errno));
                ret = -1;
                *p = '/';
                break;
            }
            *p = '/';
        }
    }
    if (ret == 0) {
        if (mkdir(partial, 0700) != 0 && errno != EEXIST) {
            fprintf(stderr,
                    "nocturne-tagcheck: mkdir failed on %.*s: %s\n",
                    (int)strlen(partial), partial, strerror(errno));
            ret = -1;
        }
    }
    free(partial);
    free(copy);
    return ret;
}

/* Build a comma-separated string of distinct FAIL codes from cr->issues.
 * Caller frees. Returns NULL on OOM. */
static char *reasons_join(const struct check_result *cr)
{
    if (!cr || cr->issue_count == 0) return strdup("");
    /* Worst-case length: sum of code lengths + commas. */
    size_t cap = 1;
    for (size_t i = 0; i < cr->issue_count; i++) {
        if (cr->issues[i].severity != CHECK_FAIL) continue;
        cap += (cr->issues[i].code ? strlen(cr->issues[i].code) : 0) + 1;
    }
    if (cap == 1) return strdup("");
    char *buf = calloc(1, cap + 16);
    if (!buf) return NULL;
    /* Append codes; dedupe trivially via linear scan. */
    size_t off = 0;
    for (size_t i = 0; i < cr->issue_count; i++) {
        if (cr->issues[i].severity != CHECK_FAIL) continue;
        const char *code = cr->issues[i].code;
        if (!code) continue;
        /* Already present? */
        if (off > 0) {
            char needle[128];
            snprintf(needle, sizeof(needle), "%s", code);
            char *hay = buf;
            bool dup = false;
            while ((hay = strstr(hay, needle)) != NULL) {
                /* Confirm word-bounded match. */
                bool left_ok = (hay == buf || *(hay - 1) == ',');
                size_t nl = strlen(needle);
                bool right_ok = (hay[nl] == '\0' || hay[nl] == ',');
                if (left_ok && right_ok) { dup = true; break; }
                hay++;
            }
            if (dup) continue;
        }
        if (off > 0) {
            buf[off++] = ',';
        }
        size_t cl = strlen(code);
        memcpy(buf + off, code, cl);
        off += cl;
    }
    buf[off] = '\0';
    return buf;
}

/* Compute relative path candidate `target` = quar_root/<src - lib_root>/.
 * If src is not under lib_root, returns NULL. Caller frees `target` via
 * free(). Also returns the relative segment via *rel_out (pointer into
 * src; NOT freed by caller). */
static char *compute_target(const char *src,
                            const char *lib_root,
                            const char *quar_root,
                            const char **rel_out)
{
    if (!src || !lib_root || !quar_root) return NULL;
    size_t lib_len = strlen(lib_root);
    if (strncmp(src, lib_root, lib_len) != 0) return NULL;
    /* Boundary: src[lib_len] must be '/' or '\0'. */
    if (src[lib_len] != '/' && src[lib_len] != '\0') return NULL;
    const char *rel = src + lib_len;
    while (*rel == '/') rel++;
    if (*rel == '\0') return NULL; /* src == lib_root itself; impossible for files */

    char *target = NULL;
    if (asprintf(&target, "%s/%s", quar_root, rel) < 0) return NULL;
    if (rel_out) *rel_out = rel;
    return target;
}

/* Resolve a non-colliding target path. If `target` exists, append a
 * `.dup-<ts>` suffix. Up to 16 attempts. Returns a freshly malloc'd path
 * (caller frees) or NULL on failure. */
static char *resolve_collision(const char *target)
{
    if (!target) return NULL;
    struct stat st;
    if (lstat(target, &st) != 0) {
        /* Doesn't exist — original target wins. */
        if (errno == ENOENT) return strdup(target);
        return strdup(target);
    }

    for (int attempt = 0; attempt < 16; attempt++) {
        char *candidate = NULL;
        long ts = (long)time(NULL);
        if (attempt == 0) {
            if (asprintf(&candidate, "%s.dup-%ld", target, ts) < 0) {
                return NULL;
            }
        } else {
            unsigned int salt = ((unsigned int)rand() ^ (unsigned int)attempt) & 0xFFFFu;
            if (asprintf(&candidate, "%s.dup-%ld-%04x", target, ts, salt) < 0) {
                return NULL;
            }
        }
        if (lstat(candidate, &st) != 0 && errno == ENOENT) {
            return candidate;
        }
        free(candidate);
    }
    return NULL;
}

int quarantine_move(struct quarantine_ctx *ctx, const struct check_result *cr)
{
    if (!ctx || !cr || !cr->rec || !cr->rec->path) {
        return 1;
    }
    const char *src = cr->rec->path;

    const char *rel = NULL;
    char *target_initial = compute_target(src, ctx->library_root,
                                          ctx->quarantine_root, &rel);
    if (!target_initial) {
        fprintf(stderr,
                "nocturne-tagcheck: cowardly refusing to move file outside "
                "library root: %.*s\n",
                (int)strlen(src), src);
        ctx->failed_moves++;
        return 1;
    }

    char *reasons = reasons_join(cr);
    if (!reasons) reasons = strdup("");

    if (ctx->dry_run) {
        fprintf(stderr,
                "(dry-run) MOVE %.*s -> %.*s [reasons: %s]\n",
                (int)strlen(src), src,
                (int)strlen(target_initial), target_initial,
                reasons);
        ctx->moved_count++;
        free(target_initial);
        free(reasons);
        return 0;
    }

    /* Real run: ensure intermediate dirs exist. */
    if (mkdir_p_for_file(target_initial) != 0) {
        free(target_initial);
        free(reasons);
        ctx->failed_moves++;
        return 1;
    }

    char *target_final = resolve_collision(target_initial);
    if (!target_final) {
        fprintf(stderr,
                "nocturne-tagcheck: too many collisions resolving target "
                "%.*s\n",
                (int)strlen(target_initial), target_initial);
        free(target_initial);
        free(reasons);
        ctx->failed_moves++;
        return 1;
    }

    if (rename(src, target_final) != 0) {
        if (errno == EXDEV) {
            fprintf(stderr,
                    "nocturne-tagcheck: refusing cross-device move (library "
                    "and quarantine on different filesystems): %.*s\n",
                    (int)strlen(src), src);
        } else {
            fprintf(stderr,
                    "nocturne-tagcheck: rename failed: %.*s -> %.*s: %s\n",
                    (int)strlen(src), src,
                    (int)strlen(target_final), target_final,
                    strerror(errno));
        }
        ctx->failed_moves++;
        free(target_initial);
        free(target_final);
        free(reasons);
        return 1;
    }

    /* Compute target_rel for log line (path relative to quarantine_root). */
    const char *tgt_rel = target_final;
    size_t qrl = strlen(ctx->quarantine_root);
    if (strncmp(target_final, ctx->quarantine_root, qrl) == 0 &&
        target_final[qrl] == '/') {
        tgt_rel = target_final + qrl + 1;
    }

    if (ctx->log) {
        char ts[32];
        iso8601_utc_now(ts, sizeof(ts));
        fprintf(ctx->log, "%s\t%s\t%s\t%s\n", ts, rel, tgt_rel, reasons);
    }

    ctx->moved_count++;
    free(target_initial);
    free(target_final);
    free(reasons);
    return 0;
}

void quarantine_close(struct quarantine_ctx *ctx)
{
    if (!ctx) return;

    if (ctx->log) {
        char ts[32];
        iso8601_utc_now(ts, sizeof(ts));
        fprintf(ctx->log,
                "# nocturne-tagcheck quarantine session end: %s; moved=%zu, "
                "failed=%zu\n", ts, ctx->moved_count, ctx->failed_moves);
        fclose(ctx->log);  /* releases flock when fd closes */
        ctx->log = NULL;
        ctx->log_fd = -1;  /* fclose closed it */
    } else if (ctx->log_fd >= 0) {
        close(ctx->log_fd);
        ctx->log_fd = -1;
    }

    free(ctx->library_root);     ctx->library_root = NULL;
    free(ctx->quarantine_root);  ctx->quarantine_root = NULL;
    free(ctx->log_path);         ctx->log_path = NULL;
    /* counters remain — caller may want to read moved_count after close. */
}
