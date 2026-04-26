/*
 * test_watch.c — pure-function tests for the watch module.
 *
 * The inotify+epoll loop itself is too fragile to drive in unit tests
 * (kernel-event timing, ENOSPC simulation needing CAP_SYS_ADMIN). The
 * integration plan 02-07 covers it via a real fixture-tree smoke test.
 *
 * Behaviours under test (≥ 8 assertions):
 *   1. watch_check_inotify_limit passes when limit is high.
 *   2. watch_check_inotify_limit fails on low limit, with actionable
 *      message containing 'sysctl'.
 *   3. dq_push for a fresh dir adds an entry.
 *   4. dq_push for the same dir within window deduplicates (same count).
 *   5. dq_push updates the deadline on duplicate.
 *   6. dq_drain_due before deadline does nothing.
 *   7. dq_drain_due after deadline invokes cb once and removes entry.
 *   8. dq_next_deadline_ms returns LLONG_MAX on empty.
 *   9. dq_drain_due processes multiple distinct dirs at once.
 *  10. Bonus: dq_free leaves no leaks (verified under SAN=1).
 */

#define _GNU_SOURCE

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "watch.h"
#include "runner.h"

struct cb_log {
    const char *seen[16];
    size_t count;
};

static void cb_record(const char *dir, void *ud)
{
    struct cb_log *l = (struct cb_log *) ud;
    if (l->count < 16) l->seen[l->count++] = dir;
}

int main(int argc, char **argv)
{
    (void) argc; (void) argv;

    /* 1. High-limit fixture — pass. */
    {
        char tmpl[] = "/tmp/nocturne-watch-limit-XXXXXX";
        int fd = mkstemp(tmpl);
        FILE *f = fdopen(fd, "w");
        fprintf(f, "1048576\n");
        fclose(f);
        char reason[256] = {0};
        int rc = watch_check_inotify_limit(100, tmpl, reason, sizeof(reason));
        expect(rc == 0, "watch_check passes when limit > dir_count + 100");
        unlink(tmpl);
    }

    /* 2. Low-limit fixture — fail with sysctl hint. */
    {
        char tmpl[] = "/tmp/nocturne-watch-limit-XXXXXX";
        int fd = mkstemp(tmpl);
        FILE *f = fdopen(fd, "w");
        fprintf(f, "16\n");
        fclose(f);
        char reason[256] = {0};
        int rc = watch_check_inotify_limit(20, tmpl, reason, sizeof(reason));
        expect(rc == -1, "watch_check fails when limit < dir_count + 100");
        expect(strstr(reason, "sysctl") != NULL,
               "watch_check error mentions 'sysctl'");
        expect(strstr(reason, "1048576") != NULL,
               "watch_check error suggests 1048576");
        unlink(tmpl);
    }

    /* 3..8. debounce_queue. */
    {
        struct debounce_queue q;
        dq_init(&q, 1000);

        long long t0 = 1000;
        expect(dq_push(&q, "/a", t0) == 0, "dq_push /a returns 0");
        expect(q.count == 1, "queue has 1 entry after first push");

        /* 4. Dedup. */
        expect(dq_push(&q, "/a", t0 + 200) == 0, "dq_push /a (dup) returns 0");
        expect(q.count == 1, "duplicate push does not grow queue");

        /* 5. Updated deadline = t0 + 200 + 1000 = 2200. */
        expect(dq_next_deadline_ms(&q) == t0 + 200 + 1000,
               "dedup pushed deadline forward to latest event + debounce");

        /* 6. Drain before deadline → no calls. */
        struct cb_log log = {0};
        size_t n = dq_drain_due(&q, t0 + 500, cb_record, &log);
        expect(n == 0 && log.count == 0,
               "dq_drain_due before deadline is a no-op");
        expect(q.count == 1, "queue still holds entry pre-deadline");

        /* 7. Drain after deadline → one call. */
        n = dq_drain_due(&q, t0 + 5000, cb_record, &log);
        expect(n == 1 && log.count == 1,
               "dq_drain_due after deadline invokes cb once");
        expect(q.count == 0, "drained entry removed from queue");

        /* 8. Empty deadline. */
        expect(dq_next_deadline_ms(&q) == LLONG_MAX,
               "empty queue returns LLONG_MAX from dq_next_deadline_ms");

        /* 9. Multiple distinct dirs drain together. */
        dq_push(&q, "/x", 0);
        dq_push(&q, "/y", 0);
        dq_push(&q, "/z", 0);
        struct cb_log log2 = {0};
        n = dq_drain_due(&q, 5000, cb_record, &log2);
        expect(n == 3 && log2.count == 3,
               "multi-dir drain invokes cb for each distinct entry");
        expect(q.count == 0, "queue empty after multi-drain");

        dq_free(&q);
    }

    return test_finish(__FILE__);
}
