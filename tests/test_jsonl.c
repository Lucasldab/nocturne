/*
 * test_jsonl.c — unit tests for src/nocturned/jsonl.{h,c}.
 *
 * Behaviours under test (≥ 11 cases):
 *   1. Empty file → first read returns 0, offset stays at 0.
 *   2. Single-line file with trailing \n → reads one line, offset
 *      advances past \n, second read returns 0.
 *   3. Single-line file WITHOUT trailing \n → reads return 0, offset
 *      unchanged. Re-opening at the same offset after \n appended
 *      yields the line.
 *   4. Multi-line file → reads return each line in order; final 0.
 *   5. Empty lines (\n\n\n) → three zero-length lines.
 *   6. UTF-8 multibyte content (Cyrillic, CJK) → byte-accurate read-back;
 *      line_len equals bytes before the \n.
 *   7. Mid-stream restart: open file at offset = byte position of 2nd
 *      line; reads return only lines 2..N.
 *   8. Oversize line (>64 KiB without \n) → returns -1 with errno=EMSGSIZE;
 *      subsequent reads skip past the offending region.
 *   9. After read_line, prior `*line_out` pointer becomes stale (reader
 *      reuses the buffer). Caller must consume before the next call.
 *  10. jsonl_close(NULL) is a no-op.
 *  11. File grew between two ingest passes: open → drain → close →
 *      append more lines → reopen at stored offset → only new lines.
 *  12. Oversize line in the middle of a multi-line file: skipped, but
 *      surrounding valid lines still consumed.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "jsonl.h"
#include "runner.h"

/* --- helpers --- */

static char *write_tmpfile(const char *content, size_t len)
{
    char *path = strdup("/tmp/nocturne-jsonl-XXXXXX");
    if (!path) return NULL;
    int fd = mkstemp(path);
    if (fd < 0) { free(path); return NULL; }
    if (len > 0) {
        ssize_t w = write(fd, content, len);
        (void) w;
    }
    close(fd);
    return path;
}

static void append_to(const char *path, const char *content, size_t len)
{
    int fd = open(path, O_WRONLY | O_APPEND);
    if (fd < 0) return;
    ssize_t w = write(fd, content, len);
    (void) w;
    close(fd);
}

static off_t file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return st.st_size;
}

/* --- tests --- */

static void test_empty_file(void)
{
    char *p = write_tmpfile("", 0);
    struct jsonl_reader *r = jsonl_open(p, 0);
    expect(r != NULL, "empty: jsonl_open succeeds");
    const char *line; size_t llen;
    int rc = jsonl_read_line(r, &line, &llen);
    expect(rc == 0, "empty: read_line returns 0 (EOF)");
    expect(jsonl_offset(r) == 0, "empty: offset stays at 0");
    jsonl_close(r);
    unlink(p); free(p);
}

static void test_single_line_with_newline(void)
{
    const char *content = "{\"v\":1}\n";
    char *p = write_tmpfile(content, strlen(content));
    struct jsonl_reader *r = jsonl_open(p, 0);
    expect(r != NULL, "single+nl: open ok");
    const char *line; size_t llen;
    int rc = jsonl_read_line(r, &line, &llen);
    expect(rc == 1, "single+nl: returns 1");
    expect(llen == 7, "single+nl: line_len == 7 (no \\n)");
    expect(line && strcmp(line, "{\"v\":1}") == 0, "single+nl: bytes match");
    expect(jsonl_offset(r) == (off_t) 8, "single+nl: offset advanced past \\n");
    rc = jsonl_read_line(r, &line, &llen);
    expect(rc == 0, "single+nl: second read is EOF");
    expect(jsonl_offset(r) == (off_t) 8, "single+nl: offset still 8 at EOF");
    jsonl_close(r);
    unlink(p); free(p);
}

static void test_single_line_no_newline(void)
{
    const char *content = "{\"v\":1,\"partial\":true}";  /* no trailing \n */
    char *p = write_tmpfile(content, strlen(content));
    struct jsonl_reader *r = jsonl_open(p, 0);
    const char *line; size_t llen;
    int rc = jsonl_read_line(r, &line, &llen);
    expect(rc == 0, "single-nonl: returns 0 (partial line)");
    expect(jsonl_offset(r) == 0, "single-nonl: offset unchanged");
    jsonl_close(r);

    /* Append a \n and re-open at the same (unchanged) offset. */
    append_to(p, "\n", 1);
    r = jsonl_open(p, 0);
    rc = jsonl_read_line(r, &line, &llen);
    expect(rc == 1, "single-nonl: after \\n appended, returns 1");
    expect(llen == strlen(content), "single-nonl: line_len matches original content");
    expect(strcmp(line, content) == 0, "single-nonl: bytes match");
    jsonl_close(r);
    unlink(p); free(p);
}

static void test_multi_line(void)
{
    const char *content =
        "{\"v\":1,\"i\":1}\n"
        "{\"v\":1,\"i\":2}\n"
        "{\"v\":1,\"i\":3}\n";
    char *p = write_tmpfile(content, strlen(content));
    struct jsonl_reader *r = jsonl_open(p, 0);
    const char *line; size_t llen;
    int n = 0;
    while (jsonl_read_line(r, &line, &llen) == 1) {
        n++;
    }
    expect(n == 3, "multi: read 3 lines");
    expect(jsonl_offset(r) == (off_t) strlen(content),
           "multi: offset == file size after final \\n");
    jsonl_close(r);
    unlink(p); free(p);
}

static void test_empty_lines(void)
{
    const char *content = "\n\n\n";
    char *p = write_tmpfile(content, 3);
    struct jsonl_reader *r = jsonl_open(p, 0);
    const char *line; size_t llen;
    for (int i = 0; i < 3; i++) {
        int rc = jsonl_read_line(r, &line, &llen);
        expect(rc == 1, "empty-lines: returns 1");
        expect(llen == 0, "empty-lines: zero-length line");
    }
    int rc = jsonl_read_line(r, &line, &llen);
    expect(rc == 0, "empty-lines: 4th read is EOF");
    expect(jsonl_offset(r) == 3, "empty-lines: offset == 3");
    jsonl_close(r);
    unlink(p); free(p);
}

static void test_utf8_multibyte(void)
{
    /* Cyrillic + CJK mixed. Bytes: ne (RU) = 4 bytes UTF-8, JP = 9 bytes. */
    const char *content =
        "{\"name\":\"\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82\"}\n"  /* привет */
        "{\"name\":\"\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\"}\n";              /* 日本語 */
    char *p = write_tmpfile(content, strlen(content));
    struct jsonl_reader *r = jsonl_open(p, 0);
    const char *line; size_t llen;
    int rc = jsonl_read_line(r, &line, &llen);
    expect(rc == 1, "utf8: line 1 read");
    /* {"name":"привет"} = 11 ascii + 12 cyrillic bytes = 23. */
    expect(llen == 23, "utf8: cyrillic line is 23 bytes");
    expect(memchr(line, 0, llen) == NULL, "utf8: no embedded NULs in cyrillic line");
    rc = jsonl_read_line(r, &line, &llen);
    expect(rc == 1, "utf8: line 2 read");
    /* {"name":"日本語"} = 11 + 9 = 20 bytes. */
    expect(llen == 20, "utf8: cjk line is 20 bytes");
    rc = jsonl_read_line(r, &line, &llen);
    expect(rc == 0, "utf8: EOF after both lines");
    jsonl_close(r);
    unlink(p); free(p);
}

static void test_mid_stream_restart(void)
{
    const char *content = "AAA\nBBB\nCCC\n";
    char *p = write_tmpfile(content, strlen(content));
    /* Open at offset of 2nd line (4 = past "AAA\n"). */
    struct jsonl_reader *r = jsonl_open(p, 4);
    const char *line; size_t llen;
    int rc = jsonl_read_line(r, &line, &llen);
    expect(rc == 1, "mid-stream: line read");
    expect(llen == 3 && strcmp(line, "BBB") == 0,
           "mid-stream: first line is BBB (start_offset honoured)");
    rc = jsonl_read_line(r, &line, &llen);
    expect(rc == 1 && strcmp(line, "CCC") == 0, "mid-stream: second line CCC");
    rc = jsonl_read_line(r, &line, &llen);
    expect(rc == 0, "mid-stream: EOF after CCC");
    expect(jsonl_offset(r) == 12, "mid-stream: offset advanced to file end");
    jsonl_close(r);
    unlink(p); free(p);
}

static void test_oversize_single(void)
{
    /* 70 KiB of 'x' with no \n. */
    size_t n = 70 * 1024;
    char *big = malloc(n + 1);
    memset(big, 'x', n);
    char *p = write_tmpfile(big, n);
    free(big);

    struct jsonl_reader *r = jsonl_open(p, 0);
    const char *line; size_t llen;
    errno = 0;
    int rc = jsonl_read_line(r, &line, &llen);
    expect(rc == -1, "oversize: returns -1");
    expect(errno == EMSGSIZE, "oversize: errno == EMSGSIZE");
    /* Offset should have advanced past the offending bytes (no `\n`
     * found → to EOF). */
    expect(jsonl_offset(r) == (off_t) n, "oversize: offset advanced to EOF");
    /* Subsequent read returns 0 (EOF). */
    rc = jsonl_read_line(r, &line, &llen);
    expect(rc == 0, "oversize: subsequent read is EOF");
    jsonl_close(r);
    unlink(p); free(p);
}

static void test_oversize_in_middle(void)
{
    /* "good\n" + 70 KiB of 'x' + "\n" + "after\n". The middle line
     * is oversize; we should recover and read "after". */
    size_t mid = 70 * 1024;
    char *content = malloc(5 + mid + 1 + 6 + 1);
    size_t pos = 0;
    memcpy(content + pos, "good\n", 5); pos += 5;
    memset(content + pos, 'x', mid); pos += mid;
    content[pos++] = '\n';
    memcpy(content + pos, "after\n", 6); pos += 6;
    char *p = write_tmpfile(content, pos);
    free(content);

    struct jsonl_reader *r = jsonl_open(p, 0);
    const char *line; size_t llen;
    int rc = jsonl_read_line(r, &line, &llen);
    expect(rc == 1 && llen == 4 && strcmp(line, "good") == 0,
           "oversize-mid: first line 'good'");
    errno = 0;
    rc = jsonl_read_line(r, &line, &llen);
    expect(rc == -1 && errno == EMSGSIZE,
           "oversize-mid: middle returns -1 EMSGSIZE");
    /* Offset should be just past the offending \n (5 + mid + 1). */
    expect(jsonl_offset(r) == (off_t) (5 + mid + 1),
           "oversize-mid: offset advanced past oversize line");
    rc = jsonl_read_line(r, &line, &llen);
    expect(rc == 1 && llen == 5 && strcmp(line, "after") == 0,
           "oversize-mid: recovers and reads 'after'");
    rc = jsonl_read_line(r, &line, &llen);
    expect(rc == 0, "oversize-mid: EOF after 'after'");
    jsonl_close(r);
    unlink(p); free(p);
}

static void test_close_null_safe(void)
{
    jsonl_close(NULL); /* must not crash */
    expect(1, "close(NULL): no-op (no crash)");
}

static void test_growing_file(void)
{
    /* Two-pass simulation: read all currently-present lines, persist
     * the offset, append more lines, reopen at the persisted offset,
     * read only the new ones. */
    char *p = write_tmpfile("first\nsecond\n", 13);
    struct jsonl_reader *r = jsonl_open(p, 0);
    const char *line; size_t llen;
    int rc;
    while ((rc = jsonl_read_line(r, &line, &llen)) == 1) { }
    off_t persisted = jsonl_offset(r);
    expect(persisted == 13, "growing: persisted offset == initial size");
    jsonl_close(r);

    append_to(p, "third\nfourth\n", 13);
    r = jsonl_open(p, persisted);
    rc = jsonl_read_line(r, &line, &llen);
    expect(rc == 1 && strcmp(line, "third") == 0,
           "growing: reopen at persisted offset reads 'third'");
    rc = jsonl_read_line(r, &line, &llen);
    expect(rc == 1 && strcmp(line, "fourth") == 0, "growing: then 'fourth'");
    rc = jsonl_read_line(r, &line, &llen);
    expect(rc == 0, "growing: EOF after appended lines");
    jsonl_close(r);
    unlink(p); free(p);
}

static void test_long_but_within_cap(void)
{
    /* Exactly JSONL_MAX_LINE bytes of content + \n should be accepted
     * (this is the upper boundary, not oversize). */
    size_t n = JSONL_MAX_LINE;
    char *big = malloc(n + 2);
    memset(big, 'y', n);
    big[n] = '\n';
    big[n + 1] = '\0';
    char *p = write_tmpfile(big, n + 1);

    struct jsonl_reader *r = jsonl_open(p, 0);
    const char *line; size_t llen;
    int rc = jsonl_read_line(r, &line, &llen);
    expect(rc == 1, "boundary: 64 KiB exact line accepted");
    expect(llen == n, "boundary: line_len == JSONL_MAX_LINE");
    expect(line && line[0] == 'y' && line[n - 1] == 'y',
           "boundary: content preserved");
    expect(jsonl_offset(r) == (off_t) (n + 1),
           "boundary: offset advanced past \\n");
    jsonl_close(r);
    unlink(p); free(p);
    free(big);
}

static void test_offset_helper(void)
{
    expect(jsonl_offset(NULL) == (off_t) -1, "offset(NULL) == -1");
}

static void test_partial_then_full_replay(void)
{
    /* Three lines on disk; we read first two, persist offset, then
     * reopen at persisted offset and read the third. Mimics the
     * mid-pass-restart pattern that the ingester relies on for
     * idempotency. */
    const char *content = "L1\nL2\nL3\n";
    char *p = write_tmpfile(content, strlen(content));
    struct jsonl_reader *r = jsonl_open(p, 0);
    const char *line; size_t llen;
    jsonl_read_line(r, &line, &llen);
    jsonl_read_line(r, &line, &llen);
    off_t after_two = jsonl_offset(r);
    expect(after_two == 6, "restart: offset == 6 after two lines");
    jsonl_close(r);

    r = jsonl_open(p, after_two);
    int rc = jsonl_read_line(r, &line, &llen);
    expect(rc == 1 && strcmp(line, "L3") == 0, "restart: third line is L3");
    rc = jsonl_read_line(r, &line, &llen);
    expect(rc == 0, "restart: EOF after L3");
    expect(jsonl_offset(r) == (off_t) file_size(p),
           "restart: final offset == file size");
    jsonl_close(r);
    unlink(p); free(p);
}

int main(int argc, char **argv)
{
    (void) argc; (void) argv;
    test_empty_file();
    test_single_line_with_newline();
    test_single_line_no_newline();
    test_multi_line();
    test_empty_lines();
    test_utf8_multibyte();
    test_mid_stream_restart();
    test_oversize_single();
    test_oversize_in_middle();
    test_close_null_safe();
    test_growing_file();
    test_long_but_within_cap();
    test_offset_helper();
    test_partial_then_full_replay();
    return test_finish(__FILE__);
}
