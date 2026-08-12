/* Atlas - HMAC-SHA256 (RFC 2104) and a constant-time comparison.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A9 needs to answer one question, quickly and without leaking: does the bearer
 * token a remote client just presented match the credential an operator created
 * locally? Atlas already ships SHA-256 as a *content* digest, and until now that
 * header said, correctly, that it was never used as a security primitive. A9
 * changes that, so the claim is restated rather than quietly abandoned:
 *
 *   The verifier stored for an API key is HMAC-SHA256(salt, secret). The
 *   adversary controls the secret they present; they do not control the salt or
 *   the stored verifier. What is relied upon is the preimage and second-preimage
 *   resistance of SHA-256, which is the property HMAC is built on. Nothing here
 *   relies on collision resistance, and nothing here compares two values an
 *   attacker chose both of.
 *
 * **Why one pass and not a slow KDF, written down because it looks like an
 * omission and is not.** PBKDF2, scrypt and argon2 exist to make a *guessable*
 * secret expensive to guess. An Atlas API key is 256 bits read from
 * `/dev/urandom` and never derived from a password, a timestamp, a pid, a user
 * name, a machine id or anything else an attacker can enumerate; there is no
 * dictionary to iterate over, so an iteration count buys nothing against the
 * only attack that exists. It would, however, buy the attacker something: this
 * verification runs once per HTTP request on an Internet-facing endpoint, so a
 * 100 ms KDF is a 100 ms-per-unauthenticated-request amplifier — a denial of
 * service handed out for free. The entropy of the secret is the guarantee here,
 * and the cost of verification is deliberately negligible.
 *
 * If a future phase ever accepts a credential a human chose, that credential
 * must not use this path.
 *
 * Test vectors from RFC 4231 are pinned in `tests/test_hmac.c`. An
 * implementation nobody checked against a published vector is a guess.
 */
#ifndef ATLAS_HMAC_H
#define ATLAS_HMAC_H

#include <stdbool.h>
#include <stddef.h>

#include "atlas/error.h"
#include "atlas/sha256.h"

#define ATLAS_HMAC_SHA256_LEN ATLAS_SHA256_DIGEST_LEN

/* HMAC-SHA256 per RFC 2104: H((K ^ opad) || H((K ^ ipad) || text)).
 *
 * A key longer than the 64-byte block is hashed first, a shorter one is zero
 * padded — both as the RFC specifies. `out` receives exactly
 * ATLAS_HMAC_SHA256_LEN bytes. */
void atlas_hmac_sha256(const void *key, size_t key_len, const void *data, size_t data_len,
                       unsigned char out[ATLAS_HMAC_SHA256_LEN]);

/* Compares `n` bytes in time that depends on `n` and on nothing else.
 *
 * `memcmp` returns as soon as it finds a difference, which tells anyone who can
 * measure the call how many leading bytes they guessed right — and a verifier
 * comparison is exactly the place that matters. Every byte is read and the
 * results are accumulated, so there is no early exit for a branch predictor or a
 * timer to observe. */
bool atlas_ct_equal(const void *a, const void *b, size_t n);

/* Fills `out` with `n` bytes from the kernel's random source.
 *
 * Fails closed and never substitutes anything: no time, no pid, no fallback
 * PRNG. A credential Atlas could not make unpredictable is a credential Atlas
 * must refuse to create, because the caller cannot tell a weak one from a strong
 * one by looking at it. */
atlas_status atlas_random_bytes(void *out, size_t n, atlas_err *err);

#endif /* ATLAS_HMAC_H */
