/*
 * hash.c — content-addressed sha256 of audio payloads.
 *
 * Per CONTEXT decision: track identity is sha256 of audio bytes. For MP3,
 * the ID3v2 header is stripped before hashing so re-tagging an MP3 leaves
 * the track identity stable. For FLAC / Opus / OGG / M4A the whole file
 * is hashed — their metadata containers are interleaved and stripping
 * cleanly is a v1.x improvement, not Phase 2.
 *
 * Pitfall 18 (file-replace race) mitigation:
 *   1. lstat()  → capture (dev, ino, size, mtim).
 *   2. open(O_RDONLY|O_NOATIME|O_CLOEXEC). Falls back without O_NOATIME
 *      on EPERM (per Pitfall 22 — non-owners can't set NOATIME).
 *   3. fstat() → must agree with lstat(); EAGAIN otherwise.
 *   4. Stream read.
 *   5. Final fstat() → size/mtim must not have changed.
 *
 * The streaming reader uses a 64KiB buffer; never loads the whole file.
 */

#define _GNU_SOURCE

#include "hash.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../../vendor/sha256/sha256.h"

#define READ_CHUNK (64 * 1024)

static int has_ext_mp3(const char *path)
{
    if (!path) return 0;
    size_t n = strlen(path);
    if (n < 4) return 0;
    return (tolower((unsigned char) path[n - 4]) == '.' &&
            tolower((unsigned char) path[n - 3]) == 'm' &&
            tolower((unsigned char) path[n - 2]) == 'p' &&
            tolower((unsigned char) path[n - 1]) == '3');
}

/* Decode a syncsafe big-endian 28-bit integer from 4 bytes (top bit of each
 * byte is masked off, per ID3v2 spec). */
static uint32_t decode_syncsafe(const unsigned char b[4])
{
    return ((uint32_t) (b[0] & 0x7f) << 21) |
           ((uint32_t) (b[1] & 0x7f) << 14) |
           ((uint32_t) (b[2] & 0x7f) <<  7) |
           ((uint32_t) (b[3] & 0x7f));
}

long hash_skip_id3v2(int fd)
{
    if (fd < 0) { errno = EBADF; return -1; }

    unsigned char hdr[10];
    ssize_t got = pread(fd, hdr, sizeof(hdr), 0);
    if (got < 0) return -1;
    if (got < (ssize_t) sizeof(hdr) || hdr[0] != 'I' || hdr[1] != 'D' || hdr[2] != '3') {
        if (lseek(fd, 0, SEEK_SET) == (off_t) -1) return -1;
        return 0;
    }

    /* hdr[3]=major, hdr[4]=revision, hdr[5]=flags, hdr[6..9]=syncsafe size */
    unsigned char flags = hdr[5];
    uint32_t size = decode_syncsafe(&hdr[6]);
    long offset = 10 + (long) size;
    if (flags & 0x10) offset += 10;  /* footer present (v2.4 only) */

    if (lseek(fd, offset, SEEK_SET) == (off_t) -1) return -1;
    return offset;
}

static int statbufs_match(const struct stat *a, const struct stat *b)
{
    return a->st_dev == b->st_dev &&
           a->st_ino == b->st_ino &&
           a->st_size == b->st_size &&
           a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
           a->st_mtim.tv_nsec == b->st_mtim.tv_nsec;
}

int hash_audio_payload(const char *path, char *out_hex, int *errno_out)
{
    if (!path || !out_hex) {
        if (errno_out) *errno_out = EINVAL;
        return -1;
    }

    struct stat lst;
    if (lstat(path, &lst) != 0) {
        if (errno_out) *errno_out = errno;
        return -1;
    }
    if (S_ISDIR(lst.st_mode)) {
        if (errno_out) *errno_out = EISDIR;
        return -1;
    }

    int fd = open(path, O_RDONLY | O_NOATIME | O_CLOEXEC);
    if (fd < 0 && errno == EPERM) {
        /* Non-owner: drop NOATIME, retry. */
        fd = open(path, O_RDONLY | O_CLOEXEC);
    }
    if (fd < 0) {
        if (errno_out) *errno_out = errno;
        return -1;
    }

    struct stat fst;
    if (fstat(fd, &fst) != 0) {
        int e = errno;
        close(fd);
        if (errno_out) *errno_out = e;
        return -1;
    }
    if (!statbufs_match(&lst, &fst)) {
        close(fd);
        if (errno_out) *errno_out = EAGAIN;
        return -1;
    }

    if (has_ext_mp3(path)) {
        if (hash_skip_id3v2(fd) < 0) {
            int e = errno;
            close(fd);
            if (errno_out) *errno_out = e;
            return -1;
        }
    } else {
        if (lseek(fd, 0, SEEK_SET) == (off_t) -1) {
            int e = errno;
            close(fd);
            if (errno_out) *errno_out = e;
            return -1;
        }
    }

    struct sha256_ctx ctx;
    sha256_init(&ctx);

    unsigned char *buf = malloc(READ_CHUNK);
    if (!buf) {
        close(fd);
        if (errno_out) *errno_out = ENOMEM;
        return -1;
    }

    for (;;) {
        ssize_t n = read(fd, buf, READ_CHUNK);
        if (n < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            free(buf);
            close(fd);
            if (errno_out) *errno_out = e;
            return -1;
        }
        if (n == 0) break;
        sha256_update(&ctx, buf, (size_t) n);
    }
    free(buf);

    /* Pitfall 18 final guard. */
    struct stat post;
    if (fstat(fd, &post) != 0 ||
        post.st_size != fst.st_size ||
        post.st_mtim.tv_sec != fst.st_mtim.tv_sec ||
        post.st_mtim.tv_nsec != fst.st_mtim.tv_nsec) {
        close(fd);
        if (errno_out) *errno_out = EAGAIN;
        return -1;
    }
    close(fd);

    uint8_t digest[SHA256_DIGEST_LEN];
    sha256_final(&ctx, digest);
    sha256_hex(digest, out_hex);
    return 0;
}
