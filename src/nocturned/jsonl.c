/*
 * jsonl.c — line-oriented JSONL reader with byte-offset tracking.
 *
 * Used by the Phase 7 ingester (src/nocturned/ingest.c) to consume
 * `stats/phone-*.jsonl`, `likes-phone-*.jsonl`, `pins-phone-*.jsonl`
 * from the metadata sync folder one event at a time, advancing a
 * persisted byte offset (ingest_offsets table) past each fully-read
 * `\n`-terminated record.
 *
 * Design pinned by the locked Phase 7 contract:
 *
 * 1. APPEND-ONLY INVARIANT — open with O_RDONLY only. Never O_RDWR /
 *    O_WRONLY. Asserted by CROSS-03 source grep (tests/test_no_network.sh).
 *
 * 2. TRAILING PARTIAL LINE TOLERANCE — a file with no trailing `\n`
 *    leaves the offset BEFORE the partial. Next ingest pass re-reads
 *    those bytes (Syncthing may have flushed the rest of the line in
 *    between). EOF is reported as 0; offset is unchanged.
 *
 * 3. OVERSIZE LINE GUARD (DoS mitigation) — `JSONL_MAX_LINE` (64 KiB)
 *    bounds the per-line memory budget. A line that overshoots without
 *    a newline returns -1 with errno=EMSGSIZE; the offset advances past
 *    the offending bytes (to the next `\n` or EOF) so the file isn't
 *    permanently stuck. The next call resumes cleanly.
 *
 * 4. UTF-8 NAIVETY — content bytes pass through verbatim. We only look
 *    for `\n` (0x0A); any UTF-8 multibyte sequence is unmolested.
 *
 * 5. NO-CHURN HOT PATH — one heap allocation up front (4 KiB), grown
 *    by doubling up to JSONL_MAX_LINE+1. No per-line malloc.
 *
 * Buffer model:
 *   buf[0 .. buf_len)       — currently buffered bytes
 *   buf_pos                 — next byte to scan / return
 *   offset                  — absolute file offset of buf[buf_pos]
 *                             (always at a newline boundary or at the
 *                             original start_offset)
 *   bytes_read_total        — absolute file offset of buf[buf_len]
 *                             (i.e. one past the last buffered byte)
 *
 * Invariant: offset == bytes_read_total - (buf_len - buf_pos)
 */

#define _GNU_SOURCE

#include "jsonl.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define JSONL_INITIAL_BUF 4096

struct jsonl_reader {
    int fd;
    /* Internal buffer + bookkeeping. */
    char  *buf;
    size_t buf_cap;        /* allocated bytes (excluding trailing NUL slot) */
    size_t buf_len;        /* bytes valid in buf[0..buf_len) */
    size_t buf_pos;        /* next byte to return / scan */
    off_t  offset;         /* absolute offset of buf[buf_pos] */
    int    eof;            /* read(2) returned 0 at least once */
    int    err;            /* sticky read(2) error: errno snapshot, 0 if none */
};

/* Compact: drop bytes [0, buf_pos) by sliding the tail to the front.
 * Cheap when buf_pos is large; no-op when buf_pos == 0. */
static void jsonl_compact(struct jsonl_reader *r)
{
    if (r->buf_pos == 0) return;
    size_t remaining = r->buf_len - r->buf_pos;
    if (remaining > 0) {
        memmove(r->buf, r->buf + r->buf_pos, remaining);
    }
    r->buf_len = remaining;
    r->buf_pos = 0;
}

/* Grow the buffer toward `target` (at most JSONL_MAX_LINE + 1).
 * Returns 0 on success, -1 on OOM (errno=ENOMEM) or already-at-cap. */
static int jsonl_grow(struct jsonl_reader *r, size_t target)
{
    if (r->buf_cap >= target) return 0;
    size_t new_cap = r->buf_cap ? r->buf_cap : JSONL_INITIAL_BUF;
    while (new_cap < target) {
        new_cap *= 2;
        if (new_cap > (size_t) JSONL_MAX_LINE + 1) {
            new_cap = (size_t) JSONL_MAX_LINE + 1;
            break;
        }
    }
    char *nb = realloc(r->buf, new_cap + 1); /* +1 for NUL terminator slot */
    if (!nb) { errno = ENOMEM; return -1; }
    r->buf = nb;
    r->buf_cap = new_cap;
    return 0;
}

/* Read more bytes into buf[buf_len .. buf_cap). Returns:
 *   >0 → bytes appended (buf_len advanced).
 *   0  → EOF reached without bytes (sticky: r->eof set).
 *   -1 → read error (sticky: r->err set, errno preserved).
 * Returns 0 immediately when buf_len == buf_cap (caller must grow first). */
static ssize_t jsonl_refill(struct jsonl_reader *r)
{
    if (r->buf_len >= r->buf_cap) return 0;
    if (r->eof) return 0;
    if (r->err) { errno = r->err; return -1; }

    size_t avail = r->buf_cap - r->buf_len;
    ssize_t n;
    do {
        n = read(r->fd, r->buf + r->buf_len, avail);
    } while (n < 0 && errno == EINTR);

    if (n < 0) { r->err = errno; return -1; }
    if (n == 0) { r->eof = 1; return 0; }
    r->buf_len += (size_t) n;
    return n;
}

struct jsonl_reader *jsonl_open(const char *path, off_t start_offset)
{
    if (!path) { errno = EINVAL; return NULL; }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    if (start_offset > 0) {
        if (lseek(fd, start_offset, SEEK_SET) == (off_t) -1) {
            int e = errno;
            close(fd);
            errno = e;
            return NULL;
        }
    }
    struct jsonl_reader *r = calloc(1, sizeof(*r));
    if (!r) {
        int e = errno;
        close(fd);
        errno = e ? e : ENOMEM;
        return NULL;
    }
    r->fd = fd;
    r->offset = start_offset;
    if (jsonl_grow(r, JSONL_INITIAL_BUF) != 0) {
        int e = errno;
        close(fd);
        free(r);
        errno = e;
        return NULL;
    }
    return r;
}

/* Drain forward until we reach the next `\n` or EOF. Used to recover
 * from an oversize line: we've already given up on returning the bytes,
 * but we still need to advance the offset past the offending region so
 * the next call resumes cleanly. Returns 0 on success (whether by
 * finding `\n` or by hitting EOF), -1 on read error. On success, the
 * reader is reset (buf_pos=buf_len=0) and `offset` points either past
 * the `\n` or at the file's current end. */
static int jsonl_drain_oversize(struct jsonl_reader *r)
{
    /* The buffer is currently full of non-newline bytes (we hit cap
     * without finding `\n`). Discard them: advance offset, reset buffer. */
    r->offset += (off_t) (r->buf_len - r->buf_pos);
    r->buf_pos = 0;
    r->buf_len = 0;

    /* Now read raw bytes (no need to keep them) until we see `\n` or EOF.
     * Reuse the existing buffer as a scratch area. */
    while (!r->eof) {
        ssize_t n = jsonl_refill(r);
        if (n < 0) return -1;
        if (n == 0) {
            /* EOF without finding a newline. Offset stays at end. */
            r->buf_pos = 0;
            r->buf_len = 0;
            return 0;
        }
        char *nl = memchr(r->buf, '\n', r->buf_len);
        if (nl) {
            size_t consumed = (size_t) (nl - r->buf) + 1; /* include `\n` */
            r->offset += (off_t) consumed;
            /* Slide remaining bytes to front so the next read_line picks
             * up after the offending region. */
            size_t leftover = r->buf_len - consumed;
            if (leftover > 0) {
                memmove(r->buf, r->buf + consumed, leftover);
            }
            r->buf_len = leftover;
            r->buf_pos = 0;
            return 0;
        }
        /* All buffered bytes are still non-newline noise. Discard. */
        r->offset += (off_t) r->buf_len;
        r->buf_len = 0;
    }
    return 0;
}

int jsonl_read_line(struct jsonl_reader *r,
                    const char **line_out,
                    size_t *line_len_out)
{
    if (!r || !line_out || !line_len_out) { errno = EINVAL; return -1; }
    *line_out = NULL;
    *line_len_out = 0;

    for (;;) {
        /* 1. Scan the unscanned tail for `\n`. */
        if (r->buf_pos < r->buf_len) {
            char *nl = memchr(r->buf + r->buf_pos, '\n',
                              r->buf_len - r->buf_pos);
            if (nl) {
                size_t line_len = (size_t) (nl - (r->buf + r->buf_pos));
                /* Overwrite `\n` with `\0` so callers can use line_out
                 * as a C string without copying. */
                *nl = '\0';
                *line_out = r->buf + r->buf_pos;
                *line_len_out = line_len;
                /* Advance: include the `\n` in the offset bump. */
                r->buf_pos += line_len + 1;
                r->offset  += (off_t) (line_len + 1);
                return 1;
            }
        }

        /* 2. No newline in the buffered region. Either we need more
         *    bytes from disk or we've hit the line-size cap.
         *
         * The cap is JSONL_MAX_LINE bytes of content (the `\n` is
         * extra). So if we've buffered > JSONL_MAX_LINE non-newline
         * bytes, the line is definitively oversize. We allow the
         * buffer to grow to JSONL_MAX_LINE+1 so a line of exactly
         * JSONL_MAX_LINE bytes followed by `\n` still fits. */

        /* 3. Compact (drop already-returned bytes), then ensure capacity
         *    for at least one more chunk, then refill. */
        if (r->buf_pos > 0) jsonl_compact(r);

        size_t held = r->buf_len; /* held since buf_pos==0 after compact */
        if (held > (size_t) JSONL_MAX_LINE) {
            /* > JSONL_MAX_LINE non-newline bytes already on the wire:
             * line is definitively oversize. Drain past the next `\n`
             * (or to EOF) and report EMSGSIZE. */
            if (jsonl_drain_oversize(r) != 0) return -1;
            errno = EMSGSIZE;
            return -1;
        }

        /* Grow if we're full. */
        if (r->buf_len == r->buf_cap) {
            size_t want = r->buf_cap * 2;
            if (want > (size_t) JSONL_MAX_LINE + 1) {
                want = (size_t) JSONL_MAX_LINE + 1;
            }
            if (want == r->buf_cap) {
                /* Already at JSONL_MAX_LINE+1 cap and still no `\n`:
                 * the line is oversize. */
                if (jsonl_drain_oversize(r) != 0) return -1;
                errno = EMSGSIZE;
                return -1;
            }
            if (jsonl_grow(r, want) != 0) return -1;
        }

        ssize_t n = jsonl_refill(r);
        if (n < 0) return -1;
        if (n == 0) {
            /* EOF. If we have buffered bytes without a trailing `\n`,
             * they're a partial line — leave the offset where it is and
             * report EOF (caller persists the unchanged offset). */
            return 0;
        }
        /* Loop: re-scan with fresh bytes. */
    }
}

off_t jsonl_offset(const struct jsonl_reader *r)
{
    return r ? r->offset : (off_t) -1;
}

void jsonl_close(struct jsonl_reader *r)
{
    if (!r) return;
    if (r->fd >= 0) close(r->fd);
    free(r->buf);
    free(r);
}
