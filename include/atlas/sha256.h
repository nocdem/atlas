/* Atlas - internal SHA-256 (FIPS 180-4).
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Atlas ships its own implementation so that the content hash is a documented,
 * deterministic, dependency-free property of the application. It is a content
 * digest only; it is never used as a security primitive against an adversary
 * who controls both sides of a comparison.
 */
#ifndef ATLAS_SHA256_H
#define ATLAS_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define ATLAS_SHA256_DIGEST_LEN 32u
#define ATLAS_SHA256_HEX_LEN 64u /* excluding NUL */

typedef struct atlas_sha256 {
    uint32_t state[8];
    uint64_t bitlen;
    unsigned char block[64];
    size_t block_len;
} atlas_sha256;

void atlas_sha256_init(atlas_sha256 *ctx);
void atlas_sha256_update(atlas_sha256 *ctx, const void *data, size_t len);
void atlas_sha256_final(atlas_sha256 *ctx, unsigned char out[ATLAS_SHA256_DIGEST_LEN]);

/* One-shot helper. `hex_out` must have room for ATLAS_SHA256_HEX_LEN + 1. */
void atlas_sha256_hex(const void *data, size_t len, char *hex_out);

/* Lowercase hex encode. `hex_out` needs 2*len + 1 bytes. */
void atlas_hex_encode(const void *data, size_t len, char *hex_out);

#endif /* ATLAS_SHA256_H */
