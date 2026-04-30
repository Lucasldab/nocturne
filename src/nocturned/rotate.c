/*
 * rotate.c — apply manifest_current diff to the path-layout via
 * hardlink+unlink (or copy+unlink on EXDEV).
 *
 * Diff:
 *   manifest_set        = SELECT sha256 FROM manifest_current
 *   current_resident    = SELECT sha256 FROM residency_state WHERE location='resident'
 *   to_add    = manifest_set      - current_resident
 *   to_remove = current_resident  - manifest_set
 *
 * Adds run BEFORE removes (Pitfall 1: phone never below cap during
 * rotation). For each add: link(archive,resident) + unlink(archive),
 * INSERT-OR-REPLACE residency_state row to 'resident', UPDATE tracks.path.
 * For each remove: reverse.
 *
 * Atomicity caveat: the file moves and the DB updates are NOT in a
 * single transaction (cannot be — link/unlink are filesystem ops). We
 * accept that and rely on retry-convergence:
 *   - Crash between link and unlink: file in both places → next run sees
 *     "already_applied" via EEXIST + same inode, completes the unlink.
 *   - Crash between unlink and DB update: file at destination, DB still
 *     points at source → on next run the source-path link attempt fails
 *     with ENOENT; we detect this via stat(dest) and treat the move as
 *     already-applied.
 *
 * Threat-model anchors:
 *   - T-03-02-01: paths are computed by replacing the leading
 *     `archive/` ↔ `resident/` segment of an existing tracks.path; no
 *     `..` resolution; rejection of out-of-tree paths.
 *   - T-03-02-04: same as 01 — string-level rewrite only, no traversal.
 */

#define _GNU_SOURCE

#include "rotate.h"
#include "db.h"
#include "paths.h"
#include "syncthing_api.h"
#include "track_repo.h"
#include "transcode.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

/* ---- test seam (mirrors migrate.c) ---- */
static rotate_link_fn g_link_override = NULL;
void rotate_set_link_fn_for_testing(rotate_link_fn fn) { g_link_override = fn; }
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

/* Copy src → dst byte-for-byte, fsync, leave src in place. Returns 0 on
 * success, -1 on failure (with dst removed). */
static int copy_only(const char *src, const char *dst)
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
    close(d); close(s);
    if (rc != 0) { unlink(dst); return -1; }
    return 0;
}

static int copy_and_unlink(const char *src, const char *dst)
{
    if (copy_only(src, dst) != 0) return -1;
    if (unlink(src) != 0) return -1;
    return 0;
}

/* Replace the leading `archive/` segment of `cur` with `resident/`,
 * or vice versa. `cur` MUST start with <library_root>/<from>/. */
static char *swap_segment(const char *library_root, const char *cur,
                          const char *from, const char *to)
{
    size_t rootn = strlen(library_root);
    size_t fromn = strlen(from);
    if (strncmp(cur, library_root, rootn) != 0 || cur[rootn] != '/') return NULL;
    const char *rel = cur + rootn + 1;
    if (strncmp(rel, from, fromn) != 0 || rel[fromn] != '/') return NULL;
    const char *tail = rel + fromn + 1;
    size_t need = rootn + 1 + strlen(to) + 1 + strlen(tail) + 1;
    char *out = malloc(need);
    if (!out) return NULL;
    snprintf(out, need, "%s/%s/%s", library_root, to, tail);
    return out;
}

/* In-memory string set for the diff. Linear scan is fine — manifest
 * sizes are O(thousands), not millions. */
struct sha_set {
    char **items;
    size_t n;
    size_t cap;
};

static int set_add(struct sha_set *s, const char *sha)
{
    if (s->n + 1 > s->cap) {
        size_t newcap = s->cap ? s->cap * 2 : 64;
        char **t = realloc(s->items, newcap * sizeof(*t));
        if (!t) return -1;
        s->items = t;
        s->cap = newcap;
    }
    s->items[s->n] = strdup(sha);
    if (!s->items[s->n]) return -1;
    s->n++;
    return 0;
}

static int sha_cmp(const void *a, const void *b)
{
    return strcmp(*(const char **) a, *(const char **) b);
}

static void set_sort(struct sha_set *s)
{
    if (s->n > 1) qsort(s->items, s->n, sizeof(*s->items), sha_cmp);
}

static int set_contains(const struct sha_set *s, const char *sha)
{
    /* set is sorted; binary search */
    size_t lo = 0, hi = s->n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(s->items[mid], sha);
        if (c == 0) return 1;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return 0;
}

static void set_free(struct sha_set *s)
{
    for (size_t i = 0; i < s->n; i++) free(s->items[i]);
    free(s->items);
    memset(s, 0, sizeof(*s));
}

static int load_set(struct sqlite3 *raw, const char *sql, struct sha_set *out)
{
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *t = sqlite3_column_text(stmt, 0);
        if (t && set_add(out, (const char *) t) != 0) {
            sqlite3_finalize(stmt); return -1;
        }
    }
    sqlite3_finalize(stmt);
    set_sort(out);
    return 0;
}

static char *lookup_track_path(struct sqlite3 *raw, const char *sha)
{
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(raw,
            "SELECT path FROM tracks WHERE sha256=?", -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(stmt, 1, sha, -1, SQLITE_TRANSIENT);
    char *out = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *t = sqlite3_column_text(stmt, 0);
        if (t) out = strdup((const char *) t);
    }
    sqlite3_finalize(stmt);
    return out;
}

static int upsert_residency(struct sqlite3 *raw, const char *sha,
                            const char *location, const char *iso)
{
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(raw,
            "INSERT INTO residency_state (sha256, location, updated_at) "
            "VALUES (?, ?, ?) "
            "ON CONFLICT(sha256) DO UPDATE SET location=excluded.location, "
            "updated_at=excluded.updated_at",
            -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, sha, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, location, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, iso, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* Same as upsert_residency but also writes the transcode_path / size /
 * format columns. Used only by the transcode-promote path. */
static int upsert_residency_transcode(struct sqlite3 *raw, const char *sha,
                                      const char *iso,
                                      const char *transcode_path,
                                      long long transcode_size,
                                      const char *transcode_format)
{
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(raw,
            "INSERT INTO residency_state (sha256, location, updated_at, "
            "  transcode_path, transcode_size_bytes, transcode_format) "
            "VALUES (?, 'resident', ?, ?, ?, ?) "
            "ON CONFLICT(sha256) DO UPDATE SET location='resident', "
            "  updated_at=excluded.updated_at, "
            "  transcode_path=excluded.transcode_path, "
            "  transcode_size_bytes=excluded.transcode_size_bytes, "
            "  transcode_format=excluded.transcode_format",
            -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, sha, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, iso, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, transcode_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, transcode_size);
    sqlite3_bind_text(stmt, 5, transcode_format, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* Demote in transcode mode: clear transcode_* columns alongside flipping
 * location to 'archive'. */
static int upsert_residency_archive_clear_transcode(struct sqlite3 *raw,
                                                    const char *sha,
                                                    const char *iso)
{
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(raw,
            "INSERT INTO residency_state (sha256, location, updated_at, "
            "  transcode_path, transcode_size_bytes, transcode_format) "
            "VALUES (?, 'archive', ?, NULL, NULL, NULL) "
            "ON CONFLICT(sha256) DO UPDATE SET location='archive', "
            "  updated_at=excluded.updated_at, "
            "  transcode_path=NULL, "
            "  transcode_size_bytes=NULL, "
            "  transcode_format=NULL",
            -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, sha, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, iso, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* Read the current transcode_path for a sha (NULL if absent). Caller frees. */
static char *lookup_transcode_path(struct sqlite3 *raw, const char *sha)
{
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(raw,
            "SELECT transcode_path FROM residency_state WHERE sha256=?",
            -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(stmt, 1, sha, -1, SQLITE_TRANSIENT);
    char *out = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *t = sqlite3_column_text(stmt, 0);
        if (t) out = strdup((const char *) t);
    }
    sqlite3_finalize(stmt);
    return out;
}

/* Replace the file extension on `path` with `new_ext` (which must include
 * the leading dot). If `path` has no dot in its basename, append. Caller
 * frees. */
static char *path_with_ext(const char *path, const char *new_ext)
{
    if (!path || !new_ext) return NULL;
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    size_t prefix_len = dot ? (size_t)(dot - path) : strlen(path);
    size_t ext_len = strlen(new_ext);
    char *out = malloc(prefix_len + ext_len + 1);
    if (!out) return NULL;
    memcpy(out, path, prefix_len);
    memcpy(out + prefix_len, new_ext, ext_len);
    out[prefix_len + ext_len] = '\0';
    return out;
}

/* Album-folder cover sidecars to propagate from archive/ → resident/.
 * Phone-side AlbumArtRepository falls back to these when MMR returns no
 * embedded picture (opus drops it during transcode). Without the link,
 * Syncthing only ships the resident audio and the phone gets no art. */
static const char *const COVER_SIDECAR_NAMES[] = {
    "cover.jpg", "cover.png", "folder.jpg", "folder.png", NULL,
};

/* Hardlink (or copy on EXDEV) every cover sidecar that exists in
 * `archive_audio_path`'s directory into the corresponding directory of
 * `resident_audio_path`. Idempotent: skips when the destination already
 * exists. Best-effort — sidecar failures are logged but never fail the
 * rotate (a missing cover is cosmetic, not a sync correctness issue). */
static void link_album_covers(const char *archive_audio_path,
                              const char *resident_audio_path)
{
    if (!archive_audio_path || !resident_audio_path) return;
    const char *a_slash = strrchr(archive_audio_path, '/');
    const char *r_slash = strrchr(resident_audio_path, '/');
    if (!a_slash || !r_slash) return;
    size_t a_dirlen = (size_t)(a_slash - archive_audio_path);
    size_t r_dirlen = (size_t)(r_slash - resident_audio_path);

    for (int i = 0; COVER_SIDECAR_NAMES[i]; i++) {
        const char *name = COVER_SIDECAR_NAMES[i];
        size_t nlen = strlen(name);
        char *src = malloc(a_dirlen + 1 + nlen + 1);
        char *dst = malloc(r_dirlen + 1 + nlen + 1);
        if (!src || !dst) { free(src); free(dst); continue; }
        memcpy(src, archive_audio_path, a_dirlen);
        src[a_dirlen] = '/';
        memcpy(src + a_dirlen + 1, name, nlen + 1);
        memcpy(dst, resident_audio_path, r_dirlen);
        dst[r_dirlen] = '/';
        memcpy(dst + r_dirlen + 1, name, nlen + 1);

        struct stat sst;
        if (stat(src, &sst) != 0 || !S_ISREG(sst.st_mode)) {
            free(src); free(dst);
            continue;
        }
        struct stat dst_st;
        if (stat(dst, &dst_st) == 0) {
            free(src); free(dst);
            continue;  /* sidecar already present — leave it alone */
        }

        int rc = do_link(src, dst);
        if (rc != 0 && errno == EXDEV) {
            if (copy_only(src, dst) != 0) {
                fprintf(stderr,
                    "rotate: cover sidecar %s → %s cross-fs copy failed: %s\n",
                    src, dst, strerror(errno));
            }
        } else if (rc != 0 && errno != EEXIST) {
            fprintf(stderr,
                "rotate: cover sidecar link(%s, %s) failed: %s\n",
                src, dst, strerror(errno));
        }
        free(src); free(dst);
    }
}

/* Transcode-mode promote (archive → resident). archive/X.flac stays. We
 * compute resident_transcode_path = swap_segment + extension swap, run
 * ffmpeg into it, stat for size, write residency_state with location +
 * transcode metadata. tracks.path UNCHANGED.
 *
 * Returns 0 on success, -1 on error (counter bumped). 1 means already
 * resident with a transcode file present (idempotent re-run). */
static int promote_transcode(struct nocturne_db *db, const char *library_root,
                             const char *sha,
                             const struct transcode_cfg *tc,
                             const char *iso, struct rotate_stats *out)
{
    struct sqlite3 *raw = db_handle(db);
    char *archive_path = lookup_track_path(raw, sha);
    if (!archive_path) {
        fprintf(stderr, "rotate: no tracks row for %.*s; skipping\n",
                (int) strlen(sha), sha);
        out->errors++;
        return -1;
    }

    /* tracks.path must point under archive/. If it's under resident/ this
     * library hasn't been migrated yet — refuse rather than silently doing
     * the wrong thing. */
    char *resident_dir_path = swap_segment(library_root, archive_path, "archive", "resident");
    if (!resident_dir_path) {
        fprintf(stderr,
            "rotate: %.*s tracks.path %.*s is not under archive/; "
            "run `nocturned transcode-migrate --apply` first\n",
            (int) strlen(sha), sha,
            (int) strlen(archive_path), archive_path);
        out->errors++;
        free(archive_path);
        return -1;
    }

    const char *new_ext = transcode_dst_ext(tc);
    if (!new_ext) {
        fprintf(stderr,
            "rotate: unsupported transcode format %s\n",
            tc->format ? tc->format : "(null)");
        out->errors++;
        free(archive_path);
        free(resident_dir_path);
        return -1;
    }

    char *transcode_path = path_with_ext(resident_dir_path, new_ext);
    free(resident_dir_path);
    if (!transcode_path) {
        out->errors++;
        free(archive_path);
        return -1;
    }

    /* Idempotence: existing transcode at the same path → skip the ffmpeg
     * run, just refresh the residency row. */
    struct stat st;
    int already = (stat(transcode_path, &st) == 0 && S_ISREG(st.st_mode));

    if (!already) {
        if (mkparents(transcode_path) != 0) {
            fprintf(stderr, "rotate: mkdir -p for %.*s failed: %s\n",
                    (int) strlen(transcode_path), transcode_path,
                    strerror(errno));
            out->errors++;
            free(archive_path); free(transcode_path);
            return -1;
        }
        int trc = transcode_audio(archive_path, transcode_path, tc);
        if (trc != 0) {
            fprintf(stderr, "rotate: transcode failed for %.*s (rc=%d)\n",
                    (int) strlen(sha), sha, trc);
            out->errors++;
            /* Best-effort cleanup of partial output. */
            unlink(transcode_path);
            free(archive_path); free(transcode_path);
            return -1;
        }
        if (stat(transcode_path, &st) != 0) {
            fprintf(stderr, "rotate: stat(%s) post-transcode failed: %s\n",
                    transcode_path, strerror(errno));
            out->errors++;
            free(archive_path); free(transcode_path);
            return -1;
        }
    }

    if (upsert_residency_transcode(raw, sha, iso, transcode_path,
                                   (long long) st.st_size, tc->format) != 0) {
        fprintf(stderr, "rotate: residency_state upsert for %.*s failed\n",
                (int) strlen(sha), sha);
        out->errors++;
        free(archive_path); free(transcode_path);
        return -1;
    }

    /* Cover sidecar propagation: archive/<album>/cover.jpg has to reach the
     * phone alongside the transcode for AlbumArtRepository to render art for
     * opus tracks (which lose embedded picture during the encode). Idempotent
     * + best-effort — failures don't fail the promote. */
    link_album_covers(archive_path, transcode_path);

    free(archive_path);
    free(transcode_path);
    if (already) out->already_applied++;
    return already ? 1 : 0;
}

/* Transcode-mode demote (resident → archive). Unlink the resident
 * transcode (if present), clear residency_state.transcode_*, set
 * location='archive'. archive/X.flac unchanged. */
static int demote_transcode(struct nocturne_db *db, const char *sha,
                            const char *iso, struct rotate_stats *out)
{
    struct sqlite3 *raw = db_handle(db);
    char *transcode_path = lookup_transcode_path(raw, sha);
    if (transcode_path) {
        if (unlink(transcode_path) != 0 && errno != ENOENT) {
            fprintf(stderr, "rotate: unlink(%s) failed: %s\n",
                    transcode_path, strerror(errno));
            /* Non-fatal — DB still gets updated; user can clean up. */
        }
        free(transcode_path);
    }
    if (upsert_residency_archive_clear_transcode(raw, sha, iso) != 0) {
        fprintf(stderr, "rotate: residency clear for %.*s failed\n",
                (int) strlen(sha), sha);
        out->errors++;
        return -1;
    }
    return 0;
}

/* Apply one direction of motion. `from`/`to` are the path segment
 * names; `target_location` is what residency_state should record after
 * this move.
 *
 * Returns:
 *    0 — file moved successfully
 *    1 — already_applied (file was at destination; only DB updated)
 *   -1 — per-track failure (counted in errors before return) */
static int move_one(struct nocturne_db *db, const char *library_root,
                    const char *sha, const char *from, const char *to,
                    const char *target_location, const char *iso,
                    struct rotate_stats *out)
{
    struct sqlite3 *raw = db_handle(db);
    char *cur = lookup_track_path(raw, sha);
    if (!cur) {
        fprintf(stderr, "rotate: no tracks row for %.*s; skipping\n",
                (int) strlen(sha), sha);
        out->errors++;
        return -1;
    }

    /* The current path may legitimately be EITHER under from/ (normal
     * case) or already under to/ (recovery: previous run unlinked
     * source but DB update didn't persist). Handle both. */
    char *new_path = swap_segment(library_root, cur, from, to);
    char *recovery_path = NULL;
    if (!new_path) {
        /* Maybe the path is already at to/ — try the inverse swap to
         * see if cur lives under to/. */
        recovery_path = swap_segment(library_root, cur, to, from);
        if (recovery_path) {
            /* cur is at to/<rel>; from-side is recovery_path. The file
             * IS where target_location says it should be. Just update
             * DB metadata. */
            free(recovery_path);
            int u = upsert_residency(raw, sha, target_location, iso);
            free(cur);
            if (u != 0) { out->errors++; return -1; }
            out->already_applied++;
            return 1;
        }
        fprintf(stderr,
            "rotate: %.*s tracks.path %.*s does not match expected "
            "%s/ or %s/ layout; skipping\n",
            (int) strlen(sha), sha, (int) strlen(cur), cur, from, to);
        out->errors++;
        free(cur);
        return -1;
    }

    if (mkparents(new_path) != 0) {
        fprintf(stderr,
            "rotate: mkdir -p for %.*s failed: %s\n",
            (int) strlen(new_path), new_path, strerror(errno));
        out->errors++;
        free(cur); free(new_path);
        return -1;
    }

    int rc = do_link(cur, new_path);
    int link_errno = errno;

    int already = 0;
    if (rc != 0 && link_errno == EEXIST) {
        struct stat a, b;
        if (stat(cur, &a) == 0 && stat(new_path, &b) == 0 &&
            a.st_dev == b.st_dev && a.st_ino == b.st_ino) {
            /* same inode → fall through to unlink + DB update.
             * Count as already_applied: the file is already where
             * residency_state will say it should be. */
            rc = 0;
            already = 1;
            out->already_applied++;
        } else {
            fprintf(stderr,
                "rotate: %.*s: destination %.*s exists with different "
                "inode; skipping\n",
                (int) strlen(sha), sha,
                (int) strlen(new_path), new_path);
            out->errors++;
            free(cur); free(new_path);
            return -1;
        }
    }
    if (rc != 0 && link_errno == EXDEV) {
        fprintf(stderr,
            "rotate: %.*s on cross-fs; falling back to copy+unlink\n",
            (int) strlen(sha), sha);
        if (copy_and_unlink(cur, new_path) != 0) {
            fprintf(stderr,
                "rotate: cross-fs copy failed for %.*s: %s\n",
                (int) strlen(sha), sha, strerror(errno));
            out->errors++;
            free(cur); free(new_path);
            return -1;
        }
        out->fallback_copies++;
        /* track_repo + residency below */
    } else if (rc != 0) {
        fprintf(stderr,
            "rotate: link(%.*s, %.*s) failed: %s\n",
            (int) strlen(cur), cur,
            (int) strlen(new_path), new_path, strerror(link_errno));
        out->errors++;
        free(cur); free(new_path);
        return -1;
    } else {
        if (unlink(cur) != 0 && errno != ENOENT) {
            fprintf(stderr,
                "rotate: unlink(%.*s) failed: %s\n",
                (int) strlen(cur), cur, strerror(errno));
            out->errors++;
        }
    }

    if (track_repo_update_path(db, sha, new_path, iso) != 0) {
        fprintf(stderr, "rotate: tracks.path update for %.*s failed\n",
                (int) strlen(sha), sha);
        out->errors++;
    }
    if (upsert_residency(raw, sha, target_location, iso) != 0) {
        fprintf(stderr, "rotate: residency_state upsert for %.*s failed\n",
                (int) strlen(sha), sha);
        out->errors++;
    }

    /* Promote-only sidecar propagation: when moving archive→resident, also
     * hardlink the album's cover.jpg-style sidecars into resident/<album>/
     * so Syncthing ships them to the phone for AlbumArtRepository fallback.
     * `cur` is the old archive path; its directory still holds the sidecar
     * after the audio unlink. Skip on demote — sidecar can stay where it is. */
    if (!strcmp(target_location, "resident")) {
        link_album_covers(cur, new_path);
    }

    free(cur);
    free(new_path);
    return already ? 1 : 0;
}

int rotate_run(struct nocturne_db *db, const char *library_root,
               struct rotate_stats *out)
{
    return rotate_run_ex(db, library_root, NULL, out);
}

int rotate_run_ex(struct nocturne_db *db, const char *library_root,
                  const struct transcode_cfg *tc, struct rotate_stats *out)
{
    if (!db || !library_root || !out) return -1;
    memset(out, 0, sizeof(*out));
    int transcode_on = (tc && tc->enabled);

    char iso_now[40];
    iso_now_buf(iso_now);

    struct sqlite3 *raw = db_handle(db);
    if (!raw) return -1;

    struct sha_set manifest = {0};
    struct sha_set resident = {0};
    if (load_set(raw, "SELECT sha256 FROM manifest_current", &manifest) != 0) {
        set_free(&manifest); return -1;
    }
    if (load_set(raw,
            "SELECT sha256 FROM residency_state WHERE location='resident'",
            &resident) != 0) {
        set_free(&manifest); set_free(&resident); return -1;
    }

    /* Diff. */
    for (size_t i = 0; i < manifest.n; i++) {
        if (!set_contains(&resident, manifest.items[i])) out->to_add++;
    }
    for (size_t i = 0; i < resident.n; i++) {
        if (!set_contains(&manifest, resident.items[i])) out->to_remove++;
    }

    /* ADDS first (Pitfall 1: never go below cap during rotation). */
    for (size_t i = 0; i < manifest.n; i++) {
        const char *sha = manifest.items[i];
        if (set_contains(&resident, sha)) continue;
        long long before = out->errors;
        int rc;
        if (transcode_on) {
            rc = promote_transcode(db, library_root, sha, tc, iso_now, out);
        } else {
            rc = move_one(db, library_root, sha,
                          "archive", "resident", "resident",
                          iso_now, out);
        }
        if (rc == 0 && out->errors == before) out->added++;
        /* rc == 1 means already_applied (counter was incremented inside the
         * promote helper); do NOT also count as added. */
    }

    /* REMOVES second. */
    for (size_t i = 0; i < resident.n; i++) {
        const char *sha = resident.items[i];
        if (set_contains(&manifest, sha)) continue;
        long long before = out->errors;
        int rc;
        if (transcode_on) {
            rc = demote_transcode(db, sha, iso_now, out);
        } else {
            rc = move_one(db, library_root, sha,
                          "resident", "archive", "archive",
                          iso_now, out);
        }
        if (rc == 0 && out->errors == before) out->removed++;
    }

    /* Cover sidecar backfill: walk every track that is currently resident
     * (post-add, post-remove) and ensure cover.jpg-style sidecars exist next
     * to the resident audio file. Idempotent — link_album_covers skips when
     * the dst sidecar already exists, so this is cheap on steady-state runs.
     *
     * Catches:
     *   1. tracks promoted before this code shipped (no cover ever linked)
     *   2. albums whose cover.jpg landed in archive AFTER the track promote
     *   3. resident dirs that lost the sidecar somehow (manual cleanup, etc.)
     *
     * Best-effort: failures log inside link_album_covers but never bump
     * out->errors. */
    {
        const char *sql_resident_with_paths =
            transcode_on
              ? "SELECT t.path, r.transcode_path FROM tracks t "
                "JOIN residency_state r ON r.sha256 = t.sha256 "
                "WHERE r.location = 'resident' AND r.transcode_path IS NOT NULL"
              : "SELECT t.path, NULL FROM tracks t "
                "JOIN residency_state r ON r.sha256 = t.sha256 "
                "WHERE r.location = 'resident'";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(raw, sql_resident_with_paths, -1, &stmt, NULL) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const unsigned char *track_path = sqlite3_column_text(stmt, 0);
                const unsigned char *xcode_path = sqlite3_column_text(stmt, 1);
                if (!track_path) continue;
                if (transcode_on) {
                    /* tracks.path = archive/...flac; transcode_path = resident/...opus */
                    if (xcode_path) {
                        link_album_covers((const char *) track_path,
                                          (const char *) xcode_path);
                    }
                } else {
                    /* tracks.path = resident/...flac; archive dir computed by swap. */
                    char *archive_audio = swap_segment(library_root,
                                                       (const char *) track_path,
                                                       "resident", "archive");
                    if (archive_audio) {
                        link_album_covers(archive_audio, (const char *) track_path);
                        free(archive_audio);
                    }
                }
            }
            sqlite3_finalize(stmt);
        }
    }

    /* manifest_meta.last_rotation_at */
    {
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(raw,
            "INSERT INTO manifest_meta (key, value) VALUES ('last_rotation_at', ?) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
            -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, iso_now, -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    /* Best-effort Syncthing rescan POST. Failure is non-fatal —
     * Syncthing's own watcher will pick up the directory changes
     * within its scan interval. Per CONTEXT.md locked decision:
     * "Failure to reach Syncthing logs warning but does not fail the
     * rotate." Folder id comes from env (NOCTURNE_SYNCFILES_FOLDER_ID)
     * with a default of "sync-files" — plan 03-04 will move this to
     * the TOML config. */
    {
        const char *folder_id = getenv("NOCTURNE_SYNCFILES_FOLDER_ID");
        if (!folder_id || !*folder_id) folder_id = "sync-files";
        int rc = syncthing_rescan(folder_id);
        if (rc == -1) {
            fprintf(stderr,
                "warn: syncthing rescan POST failed; rotate succeeded "
                "but Syncthing will pick up changes via its own scan "
                "interval\n");
        }
        /* rc == 1 (config not loaded) is silent here — rotate_cmd.c
         * already printed the warning at start. */
    }

    set_free(&manifest);
    set_free(&resident);
    return 0;
}
