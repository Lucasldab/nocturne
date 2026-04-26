/*
 * lock.c — PID lockfile via flock(2).
 *
 * Single-writer discipline (DAEMON-04): every write subcommand acquires
 * this lock before touching the DB. The pidfile lives at
 * paths_pidfile() (XDG_CACHE_HOME/nocturne/nocturned.pid) and contains
 * the holder's pid as ASCII so a second-instance error message can show
 * who's running.
 *
 * Recovery path: flock is released automatically when the holding fd is
 * closed (process death). A stale pidfile from a crash therefore unlocks
 * itself the moment the kernel reaps the holder; the next acquire
 * succeeds without manual cleanup.
 */

#define _GNU_SOURCE

#include "lock.h"
#include "paths.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct nocturne_lock {
    int fd;
    char *path;
};

/* Read up to 31 bytes of ASCII pid from `fd`, parse to int. Returns 0 on
 * any error or if the file is empty / non-numeric. */
static int read_pid_best_effort(int fd)
{
    char buf[32] = {0};
    ssize_t n = pread(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return 0;
    buf[n] = '\0';
    char *end = NULL;
    long v = strtol(buf, &end, 10);
    if (end == buf || v <= 0 || v > 0x7fffffff) return 0;
    return (int) v;
}

/* mkdir -p the parent dir of `file_path` with mode 0700. */
static int ensure_parent_dir(const char *file_path)
{
    char buf[4096];
    size_t n = strlen(file_path);
    if (n >= sizeof(buf)) { errno = ENAMETOOLONG; return -1; }
    memcpy(buf, file_path, n + 1);

    char *parent = dirname(buf);
    if (!parent || !*parent || !strcmp(parent, ".") || !strcmp(parent, "/")) return 0;
    return paths_mkdir_p(parent, 0700);
}

struct nocturne_lock *lock_acquire(const char *path, int *busy_pid)
{
    if (!path) { errno = EINVAL; return NULL; }

    if (ensure_parent_dir(path) != 0) return NULL;

    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return NULL;

    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int saved = errno;
        if (busy_pid) *busy_pid = read_pid_best_effort(fd);
        close(fd);
        errno = saved;
        return NULL;
    }

    /* We hold the lock — write our pid. ftruncate first so a previous
     * holder's longer pid string doesn't leave trailing bytes. */
    if (ftruncate(fd, 0) != 0) {
        int saved = errno;
        close(fd);  /* releases flock */
        errno = saved;
        return NULL;
    }
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%d\n", (int) getpid());
    if (n > 0) {
        ssize_t w = write(fd, buf, (size_t) n);
        (void) w;  /* best-effort; lock validity doesn't depend on write success */
    }
    fsync(fd);

    struct nocturne_lock *l = calloc(1, sizeof(*l));
    if (!l) {
        int saved = errno;
        close(fd);
        errno = saved;
        return NULL;
    }
    l->fd = fd;
    l->path = strdup(path);
    if (!l->path) {
        free(l);
        close(fd);
        errno = ENOMEM;
        return NULL;
    }
    return l;
}

void lock_release(struct nocturne_lock *l)
{
    if (!l) return;
    if (l->path) {
        unlink(l->path);    /* best-effort */
        free(l->path);
    }
    if (l->fd >= 0) close(l->fd);  /* releases flock */
    free(l);
}
