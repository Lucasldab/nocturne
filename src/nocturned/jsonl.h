#ifndef NOCTURNE_NOCTURNED_JSONL_H
#define NOCTURNE_NOCTURNED_JSONL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Maximum bytes for a single JSONL line (excluding the trailing newline).
 * Lines exceeding this cap are rejected with errno=EMSGSIZE; the offset
 * advances past the offending line so the file isn't permanently stuck. */
#define JSONL_MAX_LINE 65536

struct jsonl_reader;

/* Open the file at `path`, seek to byte `start_offset`. Returns NULL on
 * open(2) failure (errno set). The reader owns the fd; caller must
 * jsonl_close() to release.
 *
 * Per the locked append-only invariant (Phase 7 D-XX): this opens with
 * O_RDONLY only — never O_RDWR / O_WRONLY. The CROSS-03 source-grep
 * audit asserts this.
 */
struct jsonl_reader *jsonl_open(const char *path, off_t start_offset);

/* Read one complete (newline-terminated) line into the reader's internal
 * buffer.
 *
 *   *line_out  → points to internal buffer; valid until next read_line/close.
 *                NUL-terminated (line_len does NOT include the terminator).
 *   *line_len_out → number of bytes BEFORE the trailing newline.
 *
 * Returns:
 *   1  → line read; offset advanced past the newline.
 *   0  → EOF or trailing partial line (no newline yet); offset unchanged.
 *   -1 → I/O error (errno set), or oversize line (errno=EMSGSIZE) — in
 *        the oversize case, the offset IS advanced past the offending line
 *        so the file isn't permanently stuck. Caller logs, continues.
 *
 * UTF-8 safe: line_len is byte count, content is unchanged.
 */
int jsonl_read_line(struct jsonl_reader *r,
                    const char **line_out,
                    size_t *line_len_out);

/* Current offset (byte count from file start). Always at a newline
 * boundary or at start_offset if no full line has been read. */
off_t jsonl_offset(const struct jsonl_reader *r);

/* Close the fd, free internal buffers. Safe on NULL. */
void jsonl_close(struct jsonl_reader *r);

#endif /* NOCTURNE_NOCTURNED_JSONL_H */
