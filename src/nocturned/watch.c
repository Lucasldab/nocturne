/*
 * watch.c — DAEMON-03 inotify+epoll event loop.
 *
 * This file is split into two layers:
 *
 *   Layer A (this file, plan 02-03 task 1):
 *     - watch_check_inotify_limit: pre-flight against
 *       /proc/sys/fs/inotify/max_user_watches (with a test-seam override).
 *     - debounce_queue: tiny string-keyed deadline queue. Per-dir dedup,
 *       earliest-deadline lookup for epoll_wait timeout.
 *     - watch_now_ms_monotonic: monotonic clock helper.
 *
 *   Layer B (this file, plan 02-03 task 2):
 *     - watch_run: epoll loop driving inotify_add_watch on every directory
 *       under library_root; on event push the parent directory path into
 *       the debounce queue; on drain call scan_run_subtree.
 */

#define _GNU_SOURCE

#include "watch.h"
#include "scan.h"
#include "db.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

/* === Logging ============================================================== */

static void log_line(const char *fmt, ...)
{
    char tsbuf[40];
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm; gmtime_r(&ts.tv_sec, &tm);
    snprintf(tsbuf, sizeof(tsbuf), "%04u-%02u-%02uT%02u:%02u:%02uZ",
             (unsigned) (tm.tm_year + 1900),
             (unsigned) (tm.tm_mon + 1),
             (unsigned) tm.tm_mday,
             (unsigned) tm.tm_hour,
             (unsigned) tm.tm_min,
             (unsigned) tm.tm_sec);
    fprintf(stderr, "[%s watch] ", tsbuf);
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* === Layer A: pure helpers ================================================= */

long long watch_now_ms_monotonic(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long) ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

int watch_check_inotify_limit(size_t dir_count, const char *override_path,
                              char *reason, size_t reason_sz)
{
    const char *path = override_path ? override_path
                                     : "/proc/sys/fs/inotify/max_user_watches";
    FILE *f = fopen(path, "r");
    if (!f) {
        if (reason && reason_sz)
            snprintf(reason, reason_sz, "cannot read %s: %s", path, strerror(errno));
        return -1;
    }
    long long limit = -1;
    if (fscanf(f, "%lld", &limit) != 1) limit = -1;
    fclose(f);
    if (limit < 0) {
        if (reason && reason_sz)
            snprintf(reason, reason_sz, "cannot parse limit at %s", path);
        return -1;
    }
    /* Require dir_count + 100 of headroom so dynamically created subdirs
     * still get watches without forcing a fallback to periodic-rescan. */
    if ((long long) (dir_count + 100) > limit) {
        if (reason && reason_sz)
            snprintf(reason, reason_sz,
                     "insufficient inotify watches: have %lld, need >= %zu "
                     "(raise via: sudo sysctl fs.inotify.max_user_watches=1048576)",
                     limit, dir_count + 100);
        return -1;
    }
    return 0;
}

/* --- debounce_queue --- */

void dq_init(struct debounce_queue *q, int debounce_ms)
{
    q->items = NULL;
    q->count = 0;
    q->capacity = 0;
    q->debounce_ms = debounce_ms;
}

void dq_free(struct debounce_queue *q)
{
    if (!q) return;
    for (size_t i = 0; i < q->count; i++) free(q->items[i].dir);
    free(q->items);
    q->items = NULL;
    q->count = q->capacity = 0;
}

static int dq_grow(struct debounce_queue *q)
{
    size_t cap = q->capacity ? q->capacity * 2 : 16;
    struct dq_entry *p = realloc(q->items, cap * sizeof(*p));
    if (!p) return -1;
    q->items = p;
    q->capacity = cap;
    return 0;
}

int dq_push(struct debounce_queue *q, const char *dir, long long now_ms)
{
    if (!q || !dir) return -1;
    long long deadline = now_ms + q->debounce_ms;
    for (size_t i = 0; i < q->count; i++) {
        if (!strcmp(q->items[i].dir, dir)) {
            q->items[i].deadline_ms = deadline;
            return 0;
        }
    }
    if (q->count == q->capacity && dq_grow(q) != 0) return -1;
    q->items[q->count].dir = strdup(dir);
    if (!q->items[q->count].dir) return -1;
    q->items[q->count].deadline_ms = deadline;
    q->count++;
    return 0;
}

size_t dq_drain_due(struct debounce_queue *q, long long now_ms,
                    dq_drain_cb cb, void *ud)
{
    if (!q || q->count == 0) return 0;
    size_t drained = 0;
    /* Two-finger compaction: write_idx tracks where to keep, read_idx
     * scans. Drained entries get their cb invoked, dir freed, slot
     * recycled. */
    size_t write_idx = 0;
    for (size_t read_idx = 0; read_idx < q->count; read_idx++) {
        if (q->items[read_idx].deadline_ms <= now_ms) {
            if (cb) cb(q->items[read_idx].dir, ud);
            free(q->items[read_idx].dir);
            drained++;
        } else {
            if (write_idx != read_idx) q->items[write_idx] = q->items[read_idx];
            write_idx++;
        }
    }
    q->count = write_idx;
    return drained;
}

long long dq_next_deadline_ms(const struct debounce_queue *q)
{
    if (!q || q->count == 0) return LLONG_MAX;
    long long min = LLONG_MAX;
    for (size_t i = 0; i < q->count; i++) {
        if (q->items[i].deadline_ms < min) min = q->items[i].deadline_ms;
    }
    return min;
}

/* === Layer B: inotify+epoll loop =========================================== */

/* wd → directory path map. Open-addressing hash table; capacity grows
 * geometrically with insertions. */
struct wd_entry {
    int wd;          /* -1 = empty slot */
    char *path;
};

struct wd_map {
    struct wd_entry *slots;
    size_t count;
    size_t capacity;
};

static void wdm_init(struct wd_map *m) { memset(m, 0, sizeof(*m)); }

static void wdm_free(struct wd_map *m)
{
    if (!m) return;
    for (size_t i = 0; i < m->capacity; i++) {
        if (m->slots[i].wd >= 0) free(m->slots[i].path);
    }
    free(m->slots);
    memset(m, 0, sizeof(*m));
}

static size_t wdm_hash(int wd, size_t cap)
{
    /* Simple mix; wds are small positive ints from the kernel. */
    return (size_t) ((unsigned int) wd * 2654435761u) % cap;
}

static int wdm_grow(struct wd_map *m)
{
    size_t old_cap = m->capacity;
    struct wd_entry *old = m->slots;
    size_t new_cap = old_cap ? old_cap * 2 : 64;
    struct wd_entry *fresh = calloc(new_cap, sizeof(*fresh));
    if (!fresh) return -1;
    for (size_t i = 0; i < new_cap; i++) fresh[i].wd = -1;
    m->slots = fresh;
    m->capacity = new_cap;
    m->count = 0;
    for (size_t i = 0; i < old_cap; i++) {
        if (old[i].wd >= 0) {
            /* Re-insert. We can call wdm_set without recursion concern
             * because new_cap > count. */
            size_t idx = wdm_hash(old[i].wd, new_cap);
            while (fresh[idx].wd >= 0) idx = (idx + 1) % new_cap;
            fresh[idx] = old[i];
            m->count++;
        }
    }
    free(old);
    return 0;
}

static int wdm_set(struct wd_map *m, int wd, const char *path)
{
    if (m->capacity == 0 || m->count * 2 >= m->capacity) {
        if (wdm_grow(m) != 0) return -1;
    }
    size_t idx = wdm_hash(wd, m->capacity);
    while (m->slots[idx].wd >= 0) {
        if (m->slots[idx].wd == wd) {
            free(m->slots[idx].path);
            m->slots[idx].path = strdup(path);
            return m->slots[idx].path ? 0 : -1;
        }
        idx = (idx + 1) % m->capacity;
    }
    m->slots[idx].wd = wd;
    m->slots[idx].path = strdup(path);
    if (!m->slots[idx].path) {
        m->slots[idx].wd = -1;
        return -1;
    }
    m->count++;
    return 0;
}

static const char *wdm_get(const struct wd_map *m, int wd)
{
    if (m->capacity == 0) return NULL;
    size_t idx = wdm_hash(wd, m->capacity);
    for (size_t probes = 0; probes < m->capacity; probes++) {
        if (m->slots[idx].wd < 0) return NULL;
        if (m->slots[idx].wd == wd) return m->slots[idx].path;
        idx = (idx + 1) % m->capacity;
    }
    return NULL;
}

static void wdm_remove(struct wd_map *m, int wd)
{
    if (m->capacity == 0) return;
    size_t idx = wdm_hash(wd, m->capacity);
    for (size_t probes = 0; probes < m->capacity; probes++) {
        if (m->slots[idx].wd < 0) return;
        if (m->slots[idx].wd == wd) {
            free(m->slots[idx].path);
            m->slots[idx].wd = -1;
            m->slots[idx].path = NULL;
            m->count--;
            /* Re-insert any contiguous probe-cluster entries to keep the
             * open-addressing chain healthy. */
            size_t next = (idx + 1) % m->capacity;
            while (m->slots[next].wd >= 0) {
                int wd2 = m->slots[next].wd;
                char *p2 = m->slots[next].path;
                m->slots[next].wd = -1;
                m->slots[next].path = NULL;
                m->count--;
                /* path must be heap-owned by us; reinsert by dup-and-free
                 * to keep ownership clear. We instead just call wdm_set
                 * after stashing path. */
                wdm_set(m, wd2, p2);
                free(p2);
                next = (next + 1) % m->capacity;
            }
            return;
        }
        idx = (idx + 1) % m->capacity;
    }
}

/* Recursively walk `root` adding inotify watches for every directory we can
 * read. Returns the number of watches successfully added; on partial
 * failure (some adds returned ENOSPC) sets *enospc=1. */
static const uint32_t WATCH_MASK =
    IN_CREATE | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO |
    IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB;

static int add_watches_recursive(int ifd, struct wd_map *map,
                                 const char *root, int *enospc)
{
    int total = 0;
    /* Use a manual DFS; we have no need to follow symlinks (Pitfall 5). */
    DIR *d = opendir(root);
    if (!d) {
        if (errno == EACCES || errno == ENOENT) return 0;
        return -1;
    }
    int wd = inotify_add_watch(ifd, root, WATCH_MASK);
    if (wd < 0) {
        if (errno == ENOSPC) { *enospc = 1; closedir(d); return 0; }
        closedir(d);
        return -1;
    }
    if (wdm_set(map, wd, root) != 0) {
        inotify_rm_watch(ifd, wd);
        closedir(d);
        return -1;
    }
    total++;

    struct dirent *ent;
    char child[4096];
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        if (ent->d_name[0] == '.') continue;  /* dotfiles per Phase 1 walker */
        if (snprintf(child, sizeof(child), "%s/%s", root, ent->d_name) >= (int) sizeof(child)) {
            continue;
        }
        struct stat st;
        if (lstat(child, &st) != 0) continue;
        if (!S_ISDIR(st.st_mode)) continue;
        int sub = add_watches_recursive(ifd, map, child, enospc);
        if (sub < 0) { closedir(d); return sub; }
        total += sub;
    }
    closedir(d);
    return total;
}

/* Count directories under `root` via the same DFS rules as above. */
static size_t count_dirs(const char *root)
{
    DIR *d = opendir(root);
    if (!d) return 0;
    size_t n = 1;  /* root itself */
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

struct watch_state {
    struct nocturne_db *db;
    const char *library_root;
    struct wd_map wdm;
    struct debounce_queue dq;
    int ifd;
    int sfd;     /* signalfd */
    int tfd;     /* timerfd (periodic rescan) */
    int periodic_mode;
};

static void on_drain(const char *dir, void *ud)
{
    struct watch_state *st = (struct watch_state *) ud;
    struct scan_stats stats = {0};
    int rc = scan_run_subtree(st->db, st->library_root, dir, &stats);
    log_line("subtree=%s seen=%zu added=%zu updated=%zu removed=%zu skipped=%zu rc=%d",
             dir, stats.files_seen, stats.files_added, stats.files_updated,
             stats.files_removed, stats.files_skipped_unchanged, rc);
}

int watch_run(struct nocturne_db *db, const char *library_root,
              const struct watch_opts *opts)
{
    struct watch_opts default_opts = { .debounce_ms = 1000, .periodic_rescan_sec = 300 };
    if (!opts) opts = &default_opts;

    if (!db || !library_root) return -1;
    struct stat rst;
    if (lstat(library_root, &rst) != 0 || !S_ISDIR(rst.st_mode)) {
        log_line("library_root %s is not a directory", library_root);
        return -1;
    }

    size_t dirs = count_dirs(library_root);
    char reason[512];
    if (watch_check_inotify_limit(dirs, NULL, reason, sizeof(reason)) != 0) {
        log_line("%s", reason);
        return -1;
    }

    struct watch_state st = {0};
    st.db = db;
    st.library_root = library_root;
    wdm_init(&st.wdm);
    dq_init(&st.dq, opts->debounce_ms);

    st.ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (st.ifd < 0) {
        log_line("inotify_init1 failed: %s", strerror(errno));
        wdm_free(&st.wdm); dq_free(&st.dq);
        return -1;
    }

    int enospc = 0;
    int n_watches = add_watches_recursive(st.ifd, &st.wdm, library_root, &enospc);
    if (n_watches < 0) {
        log_line("add_watches_recursive failed: %s", strerror(errno));
        close(st.ifd); wdm_free(&st.wdm); dq_free(&st.dq);
        return -1;
    }
    if (enospc) {
        log_line("inotify ENOSPC during initial setup; switching to periodic-rescan mode "
                 "(every %ds). Raise fs.inotify.max_user_watches.",
                 opts->periodic_rescan_sec);
        st.periodic_mode = 1;
    } else {
        log_line("library=%s dirs=%zu watches=%d debounce_ms=%d",
                 library_root, dirs, n_watches, opts->debounce_ms);
    }

    /* signalfd for SIGTERM/SIGINT. */
    sigset_t mask; sigemptyset(&mask);
    sigaddset(&mask, SIGTERM); sigaddset(&mask, SIGINT);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0) {
        log_line("sigprocmask failed");
        close(st.ifd); wdm_free(&st.wdm); dq_free(&st.dq);
        return -1;
    }
    st.sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (st.sfd < 0) {
        log_line("signalfd failed: %s", strerror(errno));
        close(st.ifd); wdm_free(&st.wdm); dq_free(&st.dq);
        return -1;
    }

    st.tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (st.tfd < 0) {
        log_line("timerfd_create failed: %s", strerror(errno));
        close(st.sfd); close(st.ifd); wdm_free(&st.wdm); dq_free(&st.dq);
        return -1;
    }
    if (st.periodic_mode) {
        struct itimerspec its = {0};
        its.it_value.tv_sec = opts->periodic_rescan_sec;
        its.it_interval.tv_sec = opts->periodic_rescan_sec;
        timerfd_settime(st.tfd, 0, &its, NULL);
    }

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        log_line("epoll_create1 failed: %s", strerror(errno));
        close(st.tfd); close(st.sfd); close(st.ifd);
        wdm_free(&st.wdm); dq_free(&st.dq);
        return -1;
    }
    struct epoll_event ev = {0};
    ev.events = EPOLLIN; ev.data.fd = st.ifd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, st.ifd, &ev);
    ev.data.fd = st.sfd; epoll_ctl(epfd, EPOLL_CTL_ADD, st.sfd, &ev);
    ev.data.fd = st.tfd; epoll_ctl(epfd, EPOLL_CTL_ADD, st.tfd, &ev);

    int graceful = 0, hard_error = 0;
    /* Generous inotify read buffer. */
    char ibuf[16 * 1024] __attribute__((aligned(8)));

    while (!graceful && !hard_error) {
        long long now = watch_now_ms_monotonic();
        long long next = dq_next_deadline_ms(&st.dq);
        int timeout_ms = -1;
        if (next != LLONG_MAX) {
            long long delta = next - now;
            timeout_ms = (delta > 0) ? (int) delta : 0;
        }
        struct epoll_event events[8];
        int n = epoll_wait(epfd, events, 8, timeout_ms);
        if (n < 0) {
            if (errno == EINTR) continue;
            log_line("epoll_wait failed: %s", strerror(errno));
            hard_error = 1;
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == st.sfd) {
                struct signalfd_siginfo sinfo;
                while (read(st.sfd, &sinfo, sizeof(sinfo)) == sizeof(sinfo)) {
                    log_line("signal=%u; shutting down", sinfo.ssi_signo);
                    graceful = 1;
                }
            } else if (fd == st.tfd) {
                uint64_t expirations = 0;
                if (read(st.tfd, &expirations, sizeof(expirations)) > 0 && st.periodic_mode) {
                    log_line("periodic rescan");
                    struct scan_stats stats = {0};
                    scan_run(st.db, st.library_root, &stats);
                    log_line("periodic seen=%zu added=%zu updated=%zu removed=%zu",
                             stats.files_seen, stats.files_added,
                             stats.files_updated, stats.files_removed);
                }
            } else if (fd == st.ifd) {
                ssize_t got;
                while ((got = read(st.ifd, ibuf, sizeof(ibuf))) > 0) {
                    char *p = ibuf;
                    while (p < ibuf + got) {
                        struct inotify_event *iev = (struct inotify_event *) p;
                        const char *dir = wdm_get(&st.wdm, iev->wd);
                        if (dir) {
                            if ((iev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) &&
                                strcmp(dir, library_root) == 0) {
                                log_line("library_root deleted/moved; aborting");
                                hard_error = 1;
                                break;
                            }
                            /* Newly created directory inside a watched dir → add a watch
                             * before we leave this loop iteration so we don't miss
                             * grandchild events. */
                            if ((iev->mask & IN_CREATE) && (iev->mask & IN_ISDIR) && iev->len > 0) {
                                char child[4096];
                                if (snprintf(child, sizeof(child), "%s/%s",
                                             dir, iev->name) < (int) sizeof(child)) {
                                    int sub_enospc = 0;
                                    add_watches_recursive(st.ifd, &st.wdm,
                                                          child, &sub_enospc);
                                    if (sub_enospc && !st.periodic_mode) {
                                        log_line("ENOSPC adding watch for %s; "
                                                 "switching to periodic rescan", child);
                                        st.periodic_mode = 1;
                                        struct itimerspec its = {0};
                                        its.it_value.tv_sec = opts->periodic_rescan_sec;
                                        its.it_interval.tv_sec = opts->periodic_rescan_sec;
                                        timerfd_settime(st.tfd, 0, &its, NULL);
                                    }
                                }
                            }
                            /* Watch was removed (rmdir on a tracked dir). */
                            if (iev->mask & IN_IGNORED) {
                                wdm_remove(&st.wdm, iev->wd);
                            } else {
                                dq_push(&st.dq, dir, watch_now_ms_monotonic());
                            }
                        }
                        p += sizeof(struct inotify_event) + iev->len;
                    }
                    if (hard_error) break;
                }
            }
        }

        /* Drain debounced events. */
        dq_drain_due(&st.dq, watch_now_ms_monotonic(), on_drain, &st);
    }

    close(epfd);
    close(st.tfd);
    close(st.sfd);
    close(st.ifd);
    wdm_free(&st.wdm);
    dq_free(&st.dq);

    if (hard_error) return -1;
    return 0;
}
