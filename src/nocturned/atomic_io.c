/*
 * atomic_io.c — POSIX atomic write-rename for catalog.json + manifest.json.
 *
 * Pitfall 19 mitigation: readers must see the previous canonical file OR
 * the next canonical file, never a partial. The standard recipe is:
 *   1. open `<path>.tmp.<pid>` in the same directory (same FS for rename).
 *   2. write all bytes (loop on partial writes).
 *   3. fsync(file_fd).
 *   4. close(file_fd).
 *   5. rename(tmp, path) — this is the atomic boundary.
 *   6. fsync the parent directory so the rename survives a crash.
 *
 * Failure paths best-effort unlink the tmp; the canonical file at `path`
 * is never altered.
 */

#define _GNU_SOURCE

#include "atomic_io.h"
#include "paths.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct atomic_writer {
    char *final_path;
    char *tmp_path;
    int fd;
    FILE *fp;
};

/* Compose tmp path: "<path>.tmp.<pid>". Caller frees. */
static char *make_tmp_path(const char *path)
{
    size_t n = strlen(path) + 32;
    char *p = malloc(n);
    if (!p) return NULL;
    snprintf(p, n, "%s.tmp.%d", path, (int) getpid());
    return p;
}

/* fsync the directory containing `path`. Best-effort: returns 0 even on
 * partial failure since the rename has already succeeded by the time we
 * call this. */
static int fsync_parent_dir(const char *path)
{
    char buf[4096];
    if (snprintf(buf, sizeof(buf), "%s", path) >= (int) sizeof(buf)) return 0;
    char *dir = dirname(buf);
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) return 0;
    fsync(dfd);
    close(dfd);
    return 0;
}

/* Ensure dirname(path) exists. */
static int ensure_parent_dir(const char *path)
{
    char buf[4096];
    if (snprintf(buf, sizeof(buf), "%s", path) >= (int) sizeof(buf)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    char *dir = dirname(buf);
    if (!dir || !*dir) return 0;
    return paths_mkdir_p(dir, 0755);
}

int atomic_write_file(const char *path, const void *bytes, size_t len)
{
    if (!path || (!bytes && len > 0)) { errno = EINVAL; return -1; }

    if (ensure_parent_dir(path) != 0) return -1;

    char *tmp = make_tmp_path(path);
    if (!tmp) return -1;

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) { int e = errno; free(tmp); errno = e; return -1; }

    const char *p = (const char *) bytes;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t w = write(fd, p, remaining);
        if (w < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            close(fd); unlink(tmp); free(tmp);
            errno = e;
            return -1;
        }
        p += w;
        remaining -= (size_t) w;
    }

    if (fsync(fd) != 0) {
        int e = errno;
        close(fd); unlink(tmp); free(tmp);
        errno = e;
        return -1;
    }
    close(fd);

    if (rename(tmp, path) != 0) {
        int e = errno;
        unlink(tmp); free(tmp);
        errno = e;
        return -1;
    }
    free(tmp);

    fsync_parent_dir(path);
    return 0;
}

struct atomic_writer *atomic_writer_open(const char *final_path)
{
    if (!final_path) { errno = EINVAL; return NULL; }
    if (ensure_parent_dir(final_path) != 0) return NULL;
    struct atomic_writer *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->fd = -1;
    w->final_path = strdup(final_path);
    w->tmp_path = make_tmp_path(final_path);
    if (!w->final_path || !w->tmp_path) goto fail;
    w->fd = open(w->tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (w->fd < 0) goto fail;
    w->fp = fdopen(w->fd, "wb");
    if (!w->fp) goto fail;
    return w;
fail:
    {
        int e = errno;
        if (w->fd >= 0) { close(w->fd); if (w->tmp_path) unlink(w->tmp_path); }
        free(w->final_path); free(w->tmp_path); free(w);
        errno = e;
    }
    return NULL;
}

FILE *atomic_writer_file(struct atomic_writer *w)
{
    return w ? w->fp : NULL;
}

int atomic_writer_commit(struct atomic_writer *w)
{
    if (!w) { errno = EINVAL; return -1; }
    int rc = 0;
    if (w->fp) {
        fflush(w->fp);
        if (fsync(fileno(w->fp)) != 0) rc = -1;
        fclose(w->fp);
        w->fp = NULL;
        w->fd = -1;
    }
    if (rc != 0) {
        int e = errno;
        unlink(w->tmp_path);
        free(w->final_path); free(w->tmp_path); free(w);
        errno = e;
        return -1;
    }
    if (rename(w->tmp_path, w->final_path) != 0) {
        int e = errno;
        unlink(w->tmp_path);
        free(w->final_path); free(w->tmp_path); free(w);
        errno = e;
        return -1;
    }
    fsync_parent_dir(w->final_path);
    free(w->final_path); free(w->tmp_path); free(w);
    return 0;
}

void atomic_writer_abort(struct atomic_writer *w)
{
    if (!w) return;
    if (w->fp) { fclose(w->fp); w->fp = NULL; w->fd = -1; }
    if (w->tmp_path) unlink(w->tmp_path);
    free(w->final_path); free(w->tmp_path); free(w);
}
