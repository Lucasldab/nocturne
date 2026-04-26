/*
 * doctor.c — DAEMON-05 health report.
 *
 * Pure data collection: doctor_collect populates a struct doctor_report
 * by querying the DB, statvfs'ing the library mount, reading /proc, and
 * probing the lockfile state. The two `_print_` functions render that
 * struct as text or JSON. Hand-rolled JSON to avoid pulling jansson into
 * this plan (jansson ships with publisher in 02-06).
 *
 * Lock policy: doctor itself is read-only and does NOT acquire the
 * single-writer lock — it must run while `nocturned watch` is holding
 * that lock. Lockfile state is reported via a try-flock-then-release
 * probe.
 */

#define _GNU_SOURCE

#include "doctor.h"
#include "db.h"
#include "paths.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

#define MAX_SAMPLES 5

/* === helpers ============================================================= */

static int read_long_from_file(const char *path, long *out)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long v = -1;
    if (fscanf(f, "%ld", &v) != 1) { fclose(f); return -1; }
    fclose(f);
    *out = v;
    return 0;
}

static char *json_escape(const char *src)
{
    if (!src) return strdup("");
    size_t n = strlen(src);
    char *out = malloc(n * 6 + 1);
    if (!out) return NULL;
    char *o = out;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char) src[i];
        switch (c) {
        case '"':  *o++ = '\\'; *o++ = '"'; break;
        case '\\': *o++ = '\\'; *o++ = '\\'; break;
        case '\b': *o++ = '\\'; *o++ = 'b'; break;
        case '\f': *o++ = '\\'; *o++ = 'f'; break;
        case '\n': *o++ = '\\'; *o++ = 'n'; break;
        case '\r': *o++ = '\\'; *o++ = 'r'; break;
        case '\t': *o++ = '\\'; *o++ = 't'; break;
        default:
            if (c < 0x20) { o += sprintf(o, "\\u%04x", c); }
            else *o++ = (char) c;
        }
    }
    *o = '\0';
    return out;
}

static long pragma_count(struct sqlite3 *raw, const char *sql)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(raw, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    long n = -1;
    if (sqlite3_step(st) == SQLITE_ROW) n = (long) sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

static int collect_samples(struct sqlite3 *raw, const char *sql,
                           char ***samples_out, size_t *n_out)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(raw, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    char **arr = calloc(MAX_SAMPLES, sizeof(*arr));
    size_t n = 0;
    while (n < MAX_SAMPLES && sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *t = sqlite3_column_text(st, 0);
        if (t) arr[n++] = strdup((const char *) t);
    }
    sqlite3_finalize(st);
    *samples_out = arr;
    *n_out = n;
    return 0;
}

/* Walk the orphan path SELECT and access()-test each. Records up to
 * MAX_SAMPLES missing paths into orphan_samples. */
static int collect_orphans(struct sqlite3 *raw, struct doctor_report *r)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(raw, "SELECT path FROM tracks", -1, &st, NULL) != SQLITE_OK)
        return -1;
    r->orphan_samples = calloc(MAX_SAMPLES, sizeof(*r->orphan_samples));
    r->orphan_samples_n = 0;
    r->orphan_count = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *t = sqlite3_column_text(st, 0);
        if (!t) continue;
        if (access((const char *) t, F_OK) != 0) {
            r->orphan_count++;
            if (r->orphan_samples_n < MAX_SAMPLES) {
                r->orphan_samples[r->orphan_samples_n++] = strdup((const char *) t);
            }
        }
    }
    sqlite3_finalize(st);
    return 0;
}

/* Read scan_meta row (single-row best-effort: pick the most recently
 * scanned library_root when there's exactly one user, which is our case). */
static int load_scan_meta(struct sqlite3 *raw, struct doctor_report *r)
{
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT library_root, last_scan_at FROM scan_meta "
                      "ORDER BY last_scan_at DESC LIMIT 1";
    if (sqlite3_prepare_v2(raw, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *root = sqlite3_column_text(st, 0);
        const unsigned char *iso  = sqlite3_column_text(st, 1);
        if (root) r->library_root = strdup((const char *) root);
        if (iso)  r->last_scan_at_iso = strdup((const char *) iso);
    }
    sqlite3_finalize(st);
    return 0;
}

/* Parse ISO-8601 UTC into seconds-since-epoch via strptime. Best-effort:
 * returns -1 on parse failure. */
static long parse_iso_utc(const char *iso)
{
    if (!iso) return -1;
    struct tm tm = {0};
    /* Accept "YYYY-MM-DDTHH:MM:SS..." — ignore subseconds tail. */
    if (sscanf(iso, "%4d-%2d-%2dT%2d:%2d:%2d",
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) return -1;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return (long) timegm(&tm);
}

static size_t count_dirs(const char *root)
{
    extern long long watch_now_ms_monotonic(void);  /* unused, just to silence */
    (void) watch_now_ms_monotonic;
    DIR *d = opendir(root);
    if (!d) return 0;
    size_t n = 1;
    struct dirent *ent;
    char child[4096];
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        if (ent->d_name[0] == '.') continue;
        if (snprintf(child, sizeof(child), "%s/%s", root, ent->d_name) >= (int) sizeof(child)) continue;
        struct stat st;
        if (lstat(child, &st) != 0) continue;
        if (!S_ISDIR(st.st_mode)) continue;
        n += count_dirs(child);
    }
    closedir(d);
    return n;
}

/* Probe lockfile state. Returns 0=free, 1=held-by-live-process, 2=stale.
 * Sets *holder_pid in cases 1/2. `path` is the pidfile path (override
 * for tests). */
static int probe_lockfile(const char *path, int *holder_pid)
{
    *holder_pid = 0;
    if (access(path, F_OK) != 0) return 0;

    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return 0;

    /* Read pid before we change the lock state. */
    char buf[32] = {0};
    pread(fd, buf, sizeof(buf) - 1, 0);
    char *end = NULL;
    long pid = strtol(buf, &end, 10);
    if (end == buf || pid <= 0 || pid > 0x7fffffff) pid = 0;
    *holder_pid = (int) pid;

    /* Try non-blocking exclusive lock. If we get it, no one else is holding. */
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
        /* Released immediately — no real holder. The pidfile exists either
         * because a previous process crashed (stale) or because we're seeing
         * a freshly-written pidfile mid-construction. We report stale only
         * if there's actually a pid in the file. */
        flock(fd, LOCK_UN);
        close(fd);
        return (pid > 0) ? 2 : 0;
    }
    int saved = errno;
    close(fd);
    if (saved == EWOULDBLOCK) return 1;
    return 0;
}

/* Compute statvfs on the library mount. */
static void statvfs_fill(const char *root, struct doctor_report *r)
{
    r->mount_total_bytes = -1;
    r->mount_free_bytes = -1;
    r->mount_free_percent = -1;
    if (!root) return;
    struct statvfs vfs;
    if (statvfs(root, &vfs) != 0) return;
    r->mount_total_bytes = (long long) vfs.f_blocks * vfs.f_frsize;
    r->mount_free_bytes  = (long long) vfs.f_bavail * vfs.f_frsize;
    if (r->mount_total_bytes > 0) {
        r->mount_free_percent =
            (int) ((100LL * r->mount_free_bytes) / r->mount_total_bytes);
    }
}

/* === public API ========================================================== */

int doctor_collect_with_override(struct nocturne_db *db,
                                 const char *inotify_override,
                                 const char *pidfile_override,
                                 struct doctor_report *r)
{
    if (!db || !r) return -1;
    memset(r, 0, sizeof(*r));
    r->inotify_max_user_watches = -1;
    r->library_dir_count = -1;
    r->inotify_headroom = -1;
    r->mount_total_bytes = -1;
    r->mount_free_bytes = -1;
    r->mount_free_percent = -1;
    r->last_scan_age_seconds = -1;

    struct sqlite3 *raw = db_handle(db);
    if (!raw) return -1;

    r->total_tracks       = pragma_count(raw, "SELECT COUNT(*) FROM tracks");
    r->parse_failed_count = pragma_count(raw, "SELECT COUNT(*) FROM tracks WHERE tags_status='parse_failed'");
    r->incomplete_count   = pragma_count(raw, "SELECT COUNT(*) FROM tracks WHERE tags_status='incomplete'");

    collect_samples(raw,
        "SELECT path FROM tracks WHERE tags_status='parse_failed' LIMIT 5",
        &r->parse_failed_samples, &r->parse_failed_samples_n);

    collect_orphans(raw, r);

    load_scan_meta(raw, r);
    if (r->last_scan_at_iso) {
        long t = parse_iso_utc(r->last_scan_at_iso);
        if (t >= 0) {
            time_t now = time(NULL);
            r->last_scan_age_seconds = (long) (now - t);
        }
    }

    /* inotify pre-flight numbers. */
    long max_watches = -1;
    const char *iw_path = inotify_override ? inotify_override
                                           : "/proc/sys/fs/inotify/max_user_watches";
    if (read_long_from_file(iw_path, &max_watches) == 0) {
        r->inotify_max_user_watches = max_watches;
    }
    if (r->library_root) {
        r->library_dir_count = (long) count_dirs(r->library_root);
        statvfs_fill(r->library_root, r);
    }
    if (r->inotify_max_user_watches >= 0 && r->library_dir_count >= 0) {
        r->inotify_headroom = r->inotify_max_user_watches - r->library_dir_count;
    }

    /* Lockfile state. */
    const char *lock_path = pidfile_override ? pidfile_override : paths_pidfile();
    if (lock_path) {
        r->lock_held = probe_lockfile(lock_path, &r->lock_holder_pid);
    }

    /* Roll-up. */
    int issues = 0;
    if (r->parse_failed_count > 0) issues++;
    if (r->orphan_count > 0) issues++;
    if (r->inotify_headroom >= 0 && r->inotify_headroom < 100) issues++;
    if (r->mount_free_percent >= 0 && r->mount_free_percent < 5) issues++;
    if (r->last_scan_age_seconds >= 0 && r->last_scan_age_seconds > 86400 * 7) issues++;
    if (r->lock_held == 2) issues++;
    r->issues_found = issues;

    return 0;
}

int doctor_collect(struct nocturne_db *db, struct doctor_report *out)
{
    return doctor_collect_with_override(db, NULL, NULL, out);
}

void doctor_report_free(struct doctor_report *r)
{
    if (!r) return;
    for (size_t i = 0; i < r->parse_failed_samples_n; i++) free(r->parse_failed_samples[i]);
    free(r->parse_failed_samples);
    for (size_t i = 0; i < r->orphan_samples_n; i++) free(r->orphan_samples[i]);
    free(r->orphan_samples);
    free(r->library_root);
    free(r->last_scan_at_iso);
    memset(r, 0, sizeof(*r));
}

/* --- text output --- */

static const char *lock_state_str(int s)
{
    switch (s) {
    case 0: return "free";
    case 1: return "held";
    case 2: return "stale";
    default: return "unknown";
    }
}

void doctor_print_text(const struct doctor_report *r, FILE *f)
{
    if (!r || !f) return;
    if (r->total_tracks == 0 && !r->last_scan_at_iso) {
        fprintf(f, "no tracks scanned yet — run `nocturned scan <path>` first\n");
        return;
    }
    fprintf(f, "nocturned doctor\n");
    fprintf(f, "================\n\n");

    fprintf(f, "Library: %s\n", r->library_root ? r->library_root : "(unknown)");
    fprintf(f, "Last scan: %s",
            r->last_scan_at_iso ? r->last_scan_at_iso : "(never)");
    if (r->last_scan_age_seconds >= 0) {
        fprintf(f, " (%ld seconds ago)", r->last_scan_age_seconds);
    }
    fputc('\n', f);
    fprintf(f, "Total tracks: %ld\n\n", r->total_tracks);

    fprintf(f, "Tag health:\n");
    fprintf(f, "  parse_failed: %ld\n", r->parse_failed_count);
    for (size_t i = 0; i < r->parse_failed_samples_n; i++) {
        fprintf(f, "    - %s\n", r->parse_failed_samples[i]);
    }
    fprintf(f, "  incomplete:   %ld\n\n", r->incomplete_count);

    fprintf(f, "Orphan rows (in DB but missing on disk): %ld\n", r->orphan_count);
    for (size_t i = 0; i < r->orphan_samples_n; i++) {
        fprintf(f, "    - %s\n", r->orphan_samples[i]);
    }
    fputc('\n', f);

    fprintf(f, "inotify:\n");
    fprintf(f, "  max_user_watches: %ld\n", r->inotify_max_user_watches);
    fprintf(f, "  library_dir_count: %ld\n", r->library_dir_count);
    fprintf(f, "  headroom: %ld\n\n", r->inotify_headroom);

    fprintf(f, "Disk on library mount:\n");
    if (r->mount_total_bytes >= 0) {
        fprintf(f, "  total: %lld bytes\n", r->mount_total_bytes);
        fprintf(f, "  free:  %lld bytes (%d%%)\n\n",
                r->mount_free_bytes, r->mount_free_percent);
    } else {
        fprintf(f, "  (statvfs unavailable)\n\n");
    }

    fprintf(f, "Lockfile: %s", lock_state_str(r->lock_held));
    if (r->lock_holder_pid > 0) fprintf(f, " (pid=%d)", r->lock_holder_pid);
    fputc('\n', f);

    fputc('\n', f);
    fprintf(f, "Issues found: %d%s\n", r->issues_found,
            r->issues_found == 0 ? " (healthy)" : "");
}

/* --- json output --- */

static void emit_str_or_null(FILE *f, const char *s)
{
    if (!s) { fprintf(f, "null"); return; }
    char *e = json_escape(s);
    fprintf(f, "\"%s\"", e ? e : "");
    free(e);
}

static void emit_string_array(FILE *f, char **arr, size_t n)
{
    fputc('[', f);
    for (size_t i = 0; i < n; i++) {
        if (i) fputc(',', f);
        emit_str_or_null(f, arr[i]);
    }
    fputc(']', f);
}

void doctor_print_json(const struct doctor_report *r, FILE *f)
{
    if (!r || !f) return;
    fprintf(f, "{");
    fprintf(f, "\"total_tracks\":%ld,", r->total_tracks);
    fprintf(f, "\"parse_failed_count\":%ld,", r->parse_failed_count);
    fprintf(f, "\"incomplete_count\":%ld,", r->incomplete_count);
    fprintf(f, "\"parse_failed_samples\":");
    emit_string_array(f, r->parse_failed_samples, r->parse_failed_samples_n);
    fputc(',', f);
    fprintf(f, "\"orphan_count\":%ld,", r->orphan_count);
    fprintf(f, "\"orphan_samples\":");
    emit_string_array(f, r->orphan_samples, r->orphan_samples_n);
    fputc(',', f);
    fprintf(f, "\"inotify_max_user_watches\":%ld,", r->inotify_max_user_watches);
    fprintf(f, "\"library_dir_count\":%ld,", r->library_dir_count);
    fprintf(f, "\"inotify_headroom\":%ld,", r->inotify_headroom);
    fprintf(f, "\"mount_total_bytes\":%lld,", r->mount_total_bytes);
    fprintf(f, "\"mount_free_bytes\":%lld,", r->mount_free_bytes);
    fprintf(f, "\"mount_free_percent\":%d,", r->mount_free_percent);
    fprintf(f, "\"library_root\":");      emit_str_or_null(f, r->library_root);
    fprintf(f, ",\"last_scan_at_iso\":"); emit_str_or_null(f, r->last_scan_at_iso);
    fprintf(f, ",\"last_scan_age_seconds\":%ld,", r->last_scan_age_seconds);
    fprintf(f, "\"lock_held\":%d,", r->lock_held);
    fprintf(f, "\"lock_holder_pid\":%d,", r->lock_holder_pid);
    fprintf(f, "\"issues_found\":%d", r->issues_found);
    fprintf(f, "}\n");
}
