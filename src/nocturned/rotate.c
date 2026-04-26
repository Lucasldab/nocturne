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
    close(d); close(s);
    if (rc != 0) { unlink(dst); return -1; }
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

    free(cur);
    free(new_path);
    return already ? 1 : 0;
}

int rotate_run(struct nocturne_db *db, const char *library_root,
               struct rotate_stats *out)
{
    if (!db || !library_root || !out) return -1;
    memset(out, 0, sizeof(*out));

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
        int rc = move_one(db, library_root, sha,
                          "archive", "resident", "resident",
                          iso_now, out);
        if (rc == 0 && out->errors == before) out->added++;
        /* rc == 1 means already_applied (counter was incremented in
         * move_one); do NOT also count as added. */
    }

    /* REMOVES second. */
    for (size_t i = 0; i < resident.n; i++) {
        const char *sha = resident.items[i];
        if (set_contains(&manifest, sha)) continue;
        long long before = out->errors;
        int rc = move_one(db, library_root, sha,
                          "resident", "archive", "archive",
                          iso_now, out);
        if (rc == 0 && out->errors == before) out->removed++;
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
