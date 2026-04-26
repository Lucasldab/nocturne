#ifndef NOCTURNE_NOCTURNED_WATCH_H
#define NOCTURNE_NOCTURNED_WATCH_H

#include <stddef.h>

struct nocturne_db;

struct watch_opts {
    int debounce_ms;            /* default 1000 */
    int periodic_rescan_sec;    /* fallback when ENOSPC: default 300 */
};

/* Long-running event loop. Returns 0 on graceful shutdown (SIGTERM/SIGINT),
 * non-zero on hard error. */
int watch_run(struct nocturne_db *db, const char *library_root,
              const struct watch_opts *opts);

/* === Pure helpers (testable without inotify/epoll) ============================ */

/* Validate that the kernel has enough inotify watches available to cover
 * `dir_count` plus 100 of headroom. `override_path` is normally NULL; tests
 * pass a path to a file containing the limit as ASCII. On failure returns
 * -1 with `reason` (size-bounded) populated. */
int watch_check_inotify_limit(size_t dir_count, const char *override_path,
                              char *reason, size_t reason_sz);

/* Debounce queue — a tiny string-keyed set with deadlines. */
struct dq_entry {
    char *dir;
    long long deadline_ms;
};

struct debounce_queue {
    struct dq_entry *items;
    size_t count;
    size_t capacity;
    int debounce_ms;
};

void dq_init(struct debounce_queue *q, int debounce_ms);
void dq_free(struct debounce_queue *q);

/* Push `dir` into the queue with deadline = now_ms + debounce_ms. If `dir`
 * is already present, its deadline is reset (latest event wins). Returns
 * 0 on success, -1 on OOM. */
int dq_push(struct debounce_queue *q, const char *dir, long long now_ms);

/* For each entry whose deadline ≤ now_ms, invoke cb(dir, ud) and remove the
 * entry. Returns the number of entries drained. */
typedef void (*dq_drain_cb)(const char *dir, void *ud);
size_t dq_drain_due(struct debounce_queue *q, long long now_ms,
                    dq_drain_cb cb, void *ud);

/* Earliest pending deadline, or LLONG_MAX if empty. */
long long dq_next_deadline_ms(const struct debounce_queue *q);

/* Helper: monotonic milliseconds since some unspecified epoch. */
long long watch_now_ms_monotonic(void);

#endif /* NOCTURNE_NOCTURNED_WATCH_H */
