/* Atlas - HMAC-SHA256 (RFC 2104), constant-time comparison, kernel randomness.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See include/atlas/hmac.h for why this is one pass rather than a slow KDF.
 */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "atlas/hmac.h"

#define HMAC_BLOCK 64u

void atlas_hmac_sha256(const void *key, size_t key_len, const void *data, size_t data_len,
                       unsigned char out[ATLAS_HMAC_SHA256_LEN]) {
    unsigned char k[HMAC_BLOCK];
    unsigned char pad[HMAC_BLOCK];
    unsigned char inner[ATLAS_SHA256_DIGEST_LEN];
    atlas_sha256 ctx;

    memset(k, 0, sizeof(k));
    if (key_len > HMAC_BLOCK) {
        /* RFC 2104: a key longer than the block is replaced by its own hash. */
        atlas_sha256_init(&ctx);
        atlas_sha256_update(&ctx, key, key_len);
        atlas_sha256_final(&ctx, k);
    } else if (key_len > 0) {
        memcpy(k, key, key_len);
    }

    for (size_t i = 0; i < HMAC_BLOCK; i++) {
        pad[i] = (unsigned char)(k[i] ^ 0x36u);
    }
    atlas_sha256_init(&ctx);
    atlas_sha256_update(&ctx, pad, sizeof(pad));
    if (data_len > 0) {
        atlas_sha256_update(&ctx, data, data_len);
    }
    atlas_sha256_final(&ctx, inner);

    for (size_t i = 0; i < HMAC_BLOCK; i++) {
        pad[i] = (unsigned char)(k[i] ^ 0x5cu);
    }
    atlas_sha256_init(&ctx);
    atlas_sha256_update(&ctx, pad, sizeof(pad));
    atlas_sha256_update(&ctx, inner, sizeof(inner));
    atlas_sha256_final(&ctx, out);

    /* The expanded key and the inner digest are key-equivalent material: either
     * would let somebody forge a verifier without knowing the secret. Cleared
     * rather than left on a stack frame a later call reuses. `memset` through a
     * volatile pointer so the compiler may not treat the store as dead. */
    {
        volatile unsigned char *p = k;
        for (size_t i = 0; i < sizeof(k); i++) {
            p[i] = 0;
        }
        p = pad;
        for (size_t i = 0; i < sizeof(pad); i++) {
            p[i] = 0;
        }
        p = inner;
        for (size_t i = 0; i < sizeof(inner); i++) {
            p[i] = 0;
        }
    }
    memset(&ctx, 0, sizeof(ctx));
}

bool atlas_ct_equal(const void *a, const void *b, size_t n) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    unsigned char diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff = (unsigned char)(diff | (unsigned char)(x[i] ^ y[i]));
    }
    return diff == 0;
}

atlas_status atlas_random_bytes(void *out, size_t n, atlas_err *err) {
    if (n == 0) {
        return ATLAS_OK;
    }
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno,
                                   "cannot open /dev/urandom; Atlas will not substitute a "
                                   "predictable source for a credential");
    }
    unsigned char *p = (unsigned char *)out;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            int e = errno;
            (void)close(fd);
            return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, e, "cannot read /dev/urandom");
        }
        if (r == 0) {
            (void)close(fd);
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "/dev/urandom returned no bytes");
        }
        got += (size_t)r;
    }
    (void)close(fd);
    return ATLAS_OK;
}
