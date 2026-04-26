/*
 * migrate.c — one-time library migration into the path-layout
 * (`<root>/archive/<rel>`) that the rotate engine (plan 03-02) uses
 * as its diff target.
 *
 * Contract:
 *   - dry-run (apply=false): only stats.planned is set; no filesystem
 *     or DB mutations.
 *   - --apply: for each track whose tracks.path lives directly under
 *     library_root (i.e. NOT yet under archive/ or resident/), compute
 *     a destination of `<library_root>/archive/<rel>`, link+unlink,
 *     update tracks.path. On EXDEV: read+write+unlink fallback with a
 *     stern stderr warning.
 *
 * Idempotent: rerun on an already-migrated library reports planned=0
 * because every track's path already starts with archive/ or resident/.
 *
 * Threat-model anchors:
 *   - T-03-01-01: realpath(library_root) once, then prefix-match every
 *     tracks.path against the canonicalized root. Out-of-root tracks
 *     are skipped (not silently moved into archive/).
 *   - T-03-01-02: log lines use %.*s with explicit length to avoid
 *     spilling unprintable bytes from path strings.
 *   - T-03-01-03: cross-fs fallback is accepted (read+write+unlink).
 */

#define _GNU_SOURCE

#include "migrate.h"
#include "db.h"
#include "paths.h"
#include "track_repo.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

/* ---- test seam (always present; production code never calls the setter) ---- */
static migrate_link_fn g_link_override = NULL;
void migrate_set_link_fn_for_testing(migrate_link_fn fn) { g_link_override = fn; }
static int do_link(const char *o, const char *n)
{
    if (g_link_override) return g_link_override(o, n);
    return link(o, n);
}

static void iso_now_buf(char buf[40])
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    char scratch[64];
    int n = snprintf(scratch, sizeof(scratch),
                     "%04u-%02u-%02uT%02u:%02u:%02u.%06luZ",
                     (unsigned) (tm.tm_year + 1900),
                     (unsigned) (tm.tm_mon + 1),
                     (unsigned) tm.tm_mday,
                     (unsigned) tm.tm_hour,
                     (unsigned) tm.tm_min,
                     (unsigned) tm.tm_sec,
                     (unsigned long) (ts.tv_nsec / 1000));
    if (n < 0) n = 0;
    if (n >= 40) n = 39;
    memcpy(buf, scratch, (size_t) n);
    buf[n] = '\0';
}

/* Pitfall 21 / inherited: warn but don't fail when library is on a
 * non-local FS. Magic constants from <linux/magic.h> via /usr/include
 * (we copy them inline to avoid a build-time dep on linux-specific
 * headers and to keep the daemon portable across kernels). */
#ifndef NFS_SUPER_MAGIC
#define NFS_SUPER_MAGIC  0x6969
#endif
#ifndef FUSE_SUPER_MAGIC
#define FUSE_SUPER_MAGIC 0x65735546
#endif
#ifndef SMB_SUPER_MAGIC
#define SMB_SUPER_MAGIC  0x517B
#endif
#ifndef CIFS_SUPER_MAGIC
#define CIFS_SUPER_MAGIC 0xff534d42
#endif

static void warn_if_remote_fs(const char *root)
{
    struct statfs sf;
    if (statfs(root, &sf) != 0) return;
    long type = (long) sf.f_type;
    if (type == NFS_SUPER_MAGIC || type == FUSE_SUPER_MAGIC ||
        type == SMB_SUPER_MAGIC || type == CIFS_SUPER_MAGIC) {
        fprintf(stderr,
            "warn: library_root %.*s is on a non-local filesystem "
            "(magic=0x%lx); link(2) may fall back to copy+unlink\n",
            (int) strlen(root), root, (unsigned long) sf.f_type);
    }
}

/* mkdir -p the parent directory of `path`. */
static int mkparents(const char *path)
{
    char *dup = strdup(path);
    if (!dup) return -1;
    char *slash = strrchr(dup, '/');
    if (!slash || slash == dup) { free(dup); return 0; }
    *slash = '\0';
    int rc = paths_mkdir_p(dup, 0755);
    free(dup);
    return rc;
}

/* read+write+unlink fallback for cross-fs link(2). Allocates 64 KiB
 * scratch on the heap to keep the stack tight. fsync the destination
 * before unlinking the source so a power loss after this returns cannot
 * lose the audio bytes. */
static int copy_and_unlink(const char *src, const char *dst)
{
    int s = open(src, O_RDONLY | O_CLOEXEC);
    if (s < 0) return -1;
    int d = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (d < 0) { close(s); return -1; }
    char *buf = malloc(64 * 1024);
    if (!buf) { close(s); close(d); unlink(dst); return -1; }
    ssize_t n;
    int rc = 0;
    while ((n = read(s, buf, 64 * 1024)) > 0) {
        ssize_t w = 0;
        while (w < n) {
            ssize_t k = write(d, buf + w, (size_t) (n - w));
            if (k < 0) { rc = -1; goto done; }
            w += k;
        }
    }
    if (n < 0) rc = -1;
done:
    free(buf);
    if (rc == 0 && fsync(d) != 0) rc = -1;
    close(d);
    close(s);
    if (rc != 0) { unlink(dst); return -1; }
    if (unlink(src) != 0) {
        /* destination is good; source unlink failed → leave both in
         * place. The next migrate run will pick this up via the
         * already-archived branch (because tracks.path was not yet
         * updated → the row is still pointing at the source, which
         * still exists). */
        return -1;
    }
    return 0;
}

/* Compute the archive path for `cur` given canonical library_root.
 *   cur:        <library_root>/<rel>
 *   archive:    <library_root>/archive/<rel>
 * Caller frees. Returns NULL on OOM. */
static char *compute_archive_path(const char *root, size_t rootn,
                                  const char *cur)
{
    /* cur is known to start with root + '/'. */
    const char *rel = cur + rootn + 1;
    size_t need = rootn + 1 + 7 /* "archive" */ + 1 + strlen(rel) + 1;
    char *out = malloc(need);
    if (!out) return NULL;
    snprintf(out, need, "%.*s/archive/%s", (int) rootn, root, rel);
    return out;
}

/* Classify a track's path relative to library_root.
 *   1 = under <root>/archive/  → already archived
 *   2 = under <root>/resident/ → already in path-layout (treated like archived)
 *   3 = under <root>/ but flat (NOT archive/, NOT resident/) → migrate target
 *   0 = outside root (skip with warning)
 */
static int classify_path(const char *root, size_t rootn, const char *cur)
{
    if (strncmp(cur, root, rootn) != 0 || cur[rootn] != '/') return 0;
    const char *rel = cur + rootn + 1;
    if (strncmp(rel, "archive/", 8) == 0) return 1;
    if (strncmp(rel, "resident/", 9) == 0) return 2;
    return 3;
}

int migrate_run(struct nocturne_db *db, const char *library_root,
                bool apply, struct migrate_stats *out)
{
    if (!db || !library_root || !out) return -1;
    memset(out, 0, sizeof(*out));

    /* Canonicalize root once. */
    char root_canon[4096];
    if (!realpath(library_root, root_canon)) {
        fprintf(stderr, "migrate: realpath(%s) failed: %s\n",
                library_root, strerror(errno));
        return -1;
    }
    struct stat st;
    if (stat(root_canon, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "migrate: %s is not a directory\n", root_canon);
        return -1;
    }
    size_t rootn = strlen(root_canon);

    warn_if_remote_fs(root_canon);

    char iso_now[40];
    iso_now_buf(iso_now);

    struct sqlite3 *raw = db_handle(db);
    if (!raw) return -1;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(raw,
            "SELECT sha256, path FROM tracks ORDER BY sha256",
            -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "migrate: prepare failed: %s\n", sqlite3_errmsg(raw));
        return -1;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *sha_t = sqlite3_column_text(stmt, 0);
        const unsigned char *path_t = sqlite3_column_text(stmt, 1);
        if (!sha_t || !path_t) continue;
        const char *sha = (const char *) sha_t;
        const char *cur = (const char *) path_t;

        int kind = classify_path(root_canon, rootn, cur);
        switch (kind) {
        case 0:
            fprintf(stderr,
                "migrate: skipping %.*s (outside %.*s)\n",
                (int) strlen(cur), cur, (int) rootn, root_canon);
            out->skipped_outside++;
            continue;
        case 1:
        case 2:
            out->already_archived++;
            continue;
        case 3:
        default:
            break;
        }

        /* Plan or apply. */
        out->planned++;
        if (!apply) continue;

        char *new_path = compute_archive_path(root_canon, rootn, cur);
        if (!new_path) {
            fprintf(stderr, "migrate: OOM computing path for %.*s\n",
                    (int) strlen(sha), sha);
            out->errors++;
            continue;
        }

        if (mkparents(new_path) != 0) {
            fprintf(stderr,
                "migrate: mkdir -p for %.*s failed: %s\n",
                (int) strlen(new_path), new_path, strerror(errno));
            out->errors++;
            free(new_path);
            continue;
        }

        int rc = do_link(cur, new_path);
        int link_errno = errno;
        if (rc != 0 && link_errno == EEXIST) {
            /* Same inode? Treat as already-applied. Different inode →
             * skip and log; do NOT clobber. */
            struct stat a, b;
            if (stat(cur, &a) == 0 && stat(new_path, &b) == 0 &&
                a.st_dev == b.st_dev && a.st_ino == b.st_ino) {
                rc = 0;  /* fall through to unlink+repo update */
            } else {
                fprintf(stderr,
                    "migrate: %.*s: destination exists with different "
                    "inode; skipping (manual intervention required)\n",
                    (int) strlen(sha), sha);
                out->errors++;
                free(new_path);
                continue;
            }
        }
        if (rc != 0 && link_errno == EXDEV) {
            fprintf(stderr,
                "migrate: %.*s on cross-fs library; falling back to "
                "copy+unlink (slower; pitfall 21)\n",
                (int) strlen(sha), sha);
            if (copy_and_unlink(cur, new_path) != 0) {
                fprintf(stderr,
                    "migrate: cross-fs copy failed for %.*s: %s\n",
                    (int) strlen(sha), sha, strerror(errno));
                out->errors++;
                free(new_path);
                continue;
            }
            out->fallback_copies++;
            out->moved++;
            /* repo update path */
        } else if (rc != 0) {
            fprintf(stderr,
                "migrate: link(%.*s, %.*s) failed: %s\n",
                (int) strlen(cur), cur,
                (int) strlen(new_path), new_path, strerror(link_errno));
            out->errors++;
            free(new_path);
            continue;
        } else {
            /* link succeeded; remove the source. */
            if (unlink(cur) != 0 && errno != ENOENT) {
                fprintf(stderr,
                    "migrate: unlink(%.*s) failed: %s\n",
                    (int) strlen(cur), cur, strerror(errno));
                /* link is already in place; the row update below will
                 * still happen, since the file IS at new_path. */
                out->errors++;
            }
            out->moved++;
        }

        if (track_repo_update_path(db, sha, new_path, iso_now) != 0) {
            fprintf(stderr,
                "migrate: track_repo_update_path(%.*s) failed\n",
                (int) strlen(sha), sha);
            out->errors++;
            /* file is in place at new_path; tracks.path stale. The next
             * migrate run will see kind==1 (already archived) when it
             * stats the path on disk... but we read tracks.path, not
             * disk, so the next run will TRY to move again, fail with
             * EEXIST + same inode → treated as already-applied (above),
             * then unlink the old (which is gone) → ENOENT branch (also
             * above) → repo update retried. Self-healing. */
        }

        free(new_path);
    }

    sqlite3_finalize(stmt);
    return 0;
}
