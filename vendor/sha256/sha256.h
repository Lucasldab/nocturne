/*
 * sha256.h — minimal SHA-256 streaming API (public domain).
 *
 * Vendored so the daemon doesn't have to link OpenSSL/libcrypto and can be
 * audited as zero-network by inspection (CROSS-03).
 *
 * Algorithm: FIPS 180-4 SHA-256. Output is 32 bytes (SHA256_DIGEST_LEN).
 * Streaming: feed any number of chunks via sha256_update(), then call
 * sha256_final() to produce the digest.
 *
 * Implementation derived from Brad Conte's public-domain reference
 * (github.com/B-Con/crypto-algorithms — released under unlicense / public
 * domain), simplified and renamed to the noct_* namespace.
 */

#ifndef NOCTURNE_VENDOR_SHA256_H
#define NOCTURNE_VENDOR_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_LEN 32

struct sha256_ctx {
    uint8_t  buf[64];
    uint32_t state[8];
    uint64_t bit_len;
    size_t   buf_len;
};

void sha256_init(struct sha256_ctx *ctx);
void sha256_update(struct sha256_ctx *ctx, const void *data, size_t len);
void sha256_final(struct sha256_ctx *ctx, uint8_t out[SHA256_DIGEST_LEN]);

/* Helper: hex-encode digest (64 chars + NUL) into out (must be ≥65 bytes). */
void sha256_hex(const uint8_t digest[SHA256_DIGEST_LEN], char out[65]);

#endif
