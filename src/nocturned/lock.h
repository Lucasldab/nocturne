#ifndef NOCTURNE_NOCTURNED_LOCK_H
#define NOCTURNE_NOCTURNED_LOCK_H

struct nocturne_lock;

/* flock(LOCK_EX|LOCK_NB) on `path`. Creates the parent dir (mode 0700) if
 * missing, opens the lockfile mode 0600 with O_CLOEXEC, and writes the
 * caller's pid into it.
 *
 * On success: returns a non-NULL handle. *busy_pid is left untouched.
 * On contention: returns NULL and writes the pid currently holding the
 *   lock to *busy_pid (best-effort; 0 if the file is empty or unparseable).
 *   errno is left at EWOULDBLOCK.
 * On other errors: returns NULL with errno set; *busy_pid is left untouched. */
struct nocturne_lock *lock_acquire(const char *path, int *busy_pid);

/* Releases the flock and unlinks the file. Safe to call with NULL. */
void lock_release(struct nocturne_lock *l);

#endif /* NOCTURNE_NOCTURNED_LOCK_H */
