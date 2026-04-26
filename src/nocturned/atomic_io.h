#ifndef NOCTURNE_NOCTURNED_ATOMIC_IO_H
#define NOCTURNE_NOCTURNED_ATOMIC_IO_H

#include <stddef.h>
#include <stdio.h>

/* Write `bytes` (length `len`) atomically to `path`:
 *   1. Write to "<path>.tmp.<pid>" in same directory.
 *   2. fsync the tmp file.
 *   3. rename(tmp, path) — POSIX-atomic on same filesystem.
 *   4. fsync the parent directory (durability).
 * Returns 0 on success, -1 on error (errno set; partial state cleaned up
 * via best-effort unlink of the tmp file). */
int atomic_write_file(const char *path, const void *bytes, size_t len);

/* Streamed-write helper. Caller writes through the FILE* returned by
 * atomic_writer_file(); commits with atomic_writer_commit() OR aborts
 * with atomic_writer_abort(). Both functions free the writer struct. */
struct atomic_writer;
struct atomic_writer *atomic_writer_open(const char *final_path);
FILE *atomic_writer_file(struct atomic_writer *w);
int atomic_writer_commit(struct atomic_writer *w);
void atomic_writer_abort(struct atomic_writer *w);

#endif
