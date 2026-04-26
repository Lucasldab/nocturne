/*
 * test_lock.c — exercise PID lockfile semantics.
 *
 * Behaviours under test (≥ 5 assertions):
 *   1. First lock_acquire on a fresh path succeeds.
 *   2. lock_acquire creates a missing parent directory.
 *   3. While a child holds the lock, parent's lock_acquire returns NULL
 *      with errno=EWOULDBLOCK and *busy_pid == child's pid.
 *   4. After child exits (process death releases flock automatically),
 *      parent can acquire the lock.
 *   5. lock_release unlinks the file.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lock.h"
#include "runner.h"

static int rm_rf(const char *path)
{
    if (!path || strncmp(path, "/tmp/", 5) != 0) return -1;
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf -- %s", path);
    return system(cmd);
}

static char *make_tmpdir(const char *tag)
{
    char tmpl[256];
    snprintf(tmpl, sizeof(tmpl), "/tmp/nocturne-lock-%s-XXXXXX", tag);
    char *p = strdup(tmpl);
    if (!p) return NULL;
    if (!mkdtemp(p)) { free(p); return NULL; }
    return p;
}

static int file_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

static int dir_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

int main(int argc, char **argv)
{
    (void) argc; (void) argv;

    char *tmp = make_tmpdir("base");
    if (!tmp) { fprintf(stderr, "mkdtemp failed: %s\n", strerror(errno)); return 1; }

    /* 1. Fresh acquire succeeds. */
    char path[1024];
    snprintf(path, sizeof(path), "%s/sub/dir/test.pid", tmp);
    int busy = -1;
    struct nocturne_lock *l = lock_acquire(path, &busy);
    expect(l != NULL, "lock_acquire on fresh path returns non-NULL");

    /* 2. Parent dir was created. */
    char parent[1024];
    snprintf(parent, sizeof(parent), "%s/sub/dir", tmp);
    expect(dir_exists(parent), "lock_acquire created the missing parent dir");

    /* Release for the fork test. */
    lock_release(l);
    expect(!file_exists(path), "lock_release unlinks the pidfile");

    /* 3. Fork-based contention: child holds lock, parent must see EWOULDBLOCK. */
    int sync_pipe[2];
    if (pipe(sync_pipe) != 0) {
        fprintf(stderr, "pipe failed\n");
        rm_rf(tmp); free(tmp); return 1;
    }

    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "fork failed\n");
        rm_rf(tmp); free(tmp); return 1;
    }
    if (child == 0) {
        /* child: acquire, signal parent, hold for a moment, release & exit. */
        close(sync_pipe[0]);
        struct nocturne_lock *cl = lock_acquire(path, NULL);
        if (!cl) { _exit(2); }
        char ok = 'A';
        if (write(sync_pipe[1], &ok, 1) != 1) { _exit(3); }
        usleep(300 * 1000);  /* 300ms — long enough for parent to attempt */
        lock_release(cl);
        _exit(0);
    }

    /* parent: wait for child to acquire, then attempt our own. */
    close(sync_pipe[1]);
    char ack = 0;
    ssize_t r = read(sync_pipe[0], &ack, 1);
    expect(r == 1 && ack == 'A', "child signalled lock acquired");
    close(sync_pipe[0]);

    busy = -1;
    errno = 0;
    struct nocturne_lock *l2 = lock_acquire(path, &busy);
    int saved_errno = errno;
    expect(l2 == NULL, "second lock_acquire while child holds returns NULL");
    expect(saved_errno == EWOULDBLOCK,
           "errno == EWOULDBLOCK on contention");
    expect(busy == (int) child, "*busy_pid reports the child's pid");

    /* Wait for the child to finish so flock is released. */
    int status = 0;
    waitpid(child, &status, 0);
    expect(WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "child exited cleanly");

    /* 4. After child release, parent can acquire. */
    busy = -1;
    struct nocturne_lock *l3 = lock_acquire(path, &busy);
    expect(l3 != NULL, "lock_acquire succeeds after child released");
    lock_release(l3);

    rm_rf(tmp);
    free(tmp);
    return test_finish(__FILE__);
}
