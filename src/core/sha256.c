/* Atlas - SHA-256 (FIPS 180-4).
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Straightforward reference implementation; correctness is pinned by the
 * known-answer vectors in tests/test_sha256.c.
 */
#include "atlas/sha256.h"

#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

static uint32_t rotr32(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32u - n));
}

static void sha256_compress(atlas_sha256 *ctx, const unsigned char block[64]) {
    uint32_t w[64];
    for (unsigned i = 0; i < 16u; i++) {
        w[i] = ((uint32_t)block[i * 4u] << 24) | ((uint32_t)block[i * 4u + 1u] << 16) |
               ((uint32_t)block[i * 4u + 2u] << 8) | (uint32_t)block[i * 4u + 3u];
    }
    for (unsigned i = 16u; i < 64u; i++) {
        uint32_t s0 = rotr32(w[i - 15u], 7) ^ rotr32(w[i - 15u], 18) ^ (w[i - 15u] >> 3);
        uint32_t s1 = rotr32(w[i - 2u], 17) ^ rotr32(w[i - 2u], 19) ^ (w[i - 2u] >> 10);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];

    for (unsigned i = 0; i < 64u; i++) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + s1 + ch + K[i] + w[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void atlas_sha256_init(atlas_sha256 *ctx) {
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
    ctx->bitlen = 0;
    ctx->block_len = 0;
    memset(ctx->block, 0, sizeof(ctx->block));
}

void atlas_sha256_update(atlas_sha256 *ctx, const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    while (len > 0) {
        size_t room = 64u - ctx->block_len;
        size_t take = len < room ? len : room;
        memcpy(ctx->block + ctx->block_len, p, take);
        ctx->block_len += take;
        p += take;
        len -= take;
        if (ctx->block_len == 64u) {
            sha256_compress(ctx, ctx->block);
            ctx->bitlen += 512u;
            ctx->block_len = 0;
        }
    }
}

void atlas_sha256_final(atlas_sha256 *ctx, unsigned char out[ATLAS_SHA256_DIGEST_LEN]) {
    uint64_t total_bits = ctx->bitlen + (uint64_t)ctx->block_len * 8u;
    size_t i = ctx->block_len;

    ctx->block[i++] = 0x80u;
    if (i > 56u) {
        while (i < 64u) {
            ctx->block[i++] = 0;
        }
        sha256_compress(ctx, ctx->block);
        i = 0;
    }
    while (i < 56u) {
        ctx->block[i++] = 0;
    }
    for (unsigned b = 0; b < 8u; b++) {
        ctx->block[56u + b] = (unsigned char)((total_bits >> (56u - 8u * b)) & 0xffu);
    }
    sha256_compress(ctx, ctx->block);

    for (unsigned w = 0; w < 8u; w++) {
        out[w * 4u] = (unsigned char)((ctx->state[w] >> 24) & 0xffu);
        out[w * 4u + 1u] = (unsigned char)((ctx->state[w] >> 16) & 0xffu);
        out[w * 4u + 2u] = (unsigned char)((ctx->state[w] >> 8) & 0xffu);
        out[w * 4u + 3u] = (unsigned char)(ctx->state[w] & 0xffu);
    }
    /* Leave no residue of the message in the context. */
    memset(ctx->block, 0, sizeof(ctx->block));
    ctx->block_len = 0;
}

void atlas_hex_encode(const void *data, size_t len, char *hex_out) {
    static const char digits[] = "0123456789abcdef";
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < len; i++) {
        hex_out[i * 2u] = digits[(p[i] >> 4) & 0x0fu];
        hex_out[i * 2u + 1u] = digits[p[i] & 0x0fu];
    }
    hex_out[len * 2u] = '\0';
}

void atlas_sha256_hex(const void *data, size_t len, char *hex_out) {
    atlas_sha256 ctx;
    unsigned char digest[ATLAS_SHA256_DIGEST_LEN];
    atlas_sha256_init(&ctx);
    atlas_sha256_update(&ctx, data, len);
    atlas_sha256_final(&ctx, digest);
    atlas_hex_encode(digest, sizeof(digest), hex_out);
}
