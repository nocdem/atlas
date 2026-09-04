/* Atlas - A9 API credentials: scopes, token format, stored verifier.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * An Atlas API key is the credential a *remote* principal presents. It is
 * created by an operator at a local terminal, shown once, and never recoverable
 * afterwards — Atlas stores a verifier, not a secret, so there is no operation
 * that could return the plaintext and no backup that contains one.
 *
 * Token format
 *
 *     atlas_<selector>_<secret>
 *            16 hex     43 base64url
 *
 *   The **selector** is 8 random bytes. It is not a secret: it exists so a
 *   presented token can be looked up by an indexed equality test instead of by
 *   hashing the candidate against every stored salt in turn. That matters twice
 *   — it keeps authentication O(1) as the credential set grows, and it stops the
 *   verification cost from scaling with the number of keys, which would
 *   otherwise be an oracle for how many exist.
 *
 *   The **secret** is 32 bytes — 256 bits — read from `/dev/urandom` and from
 *   nothing else. Never a timestamp, a pid, a user name, repository data, a
 *   machine id or a PRNG Atlas seeded. If the kernel cannot supply them, key
 *   creation fails; Atlas does not issue a credential it cannot make
 *   unpredictable.
 *
 *   The `atlas_` prefix is deliberate and is not security: it is what lets a
 *   secret scanner, a reviewer or a paste into the wrong window be recognised
 *   for what it is.
 *
 * Storage
 *
 *   `key_id` (the selector, hex), a 16-byte random `salt`, and
 *   `verifier = HMAC-SHA256(salt, secret_bytes)`. Recovering the secret from the
 *   verifier is a SHA-256 preimage search over a 256-bit uniform space. See
 *   `include/atlas/hmac.h` for why this is one pass and not a slow KDF.
 *
 *   The plaintext is never written anywhere: not to the index, not to a log, not
 *   to an audit row, not into an error message, and not into argv. It is printed
 *   to stdout exactly once, by the command that created it.
 *
 * Scopes
 *
 *   A closed vocabulary, held as a bitmask. UNKNOWN is zero, for the reason A6
 *   keeps UNKNOWN and BLOCKED there and A8 keeps DISABLED there: a zeroed
 *   credential must authorise nothing. A scope string this binary does not
 *   recognise is refused at creation *and* makes a stored key unusable at
 *   verification — an older Atlas reading a row written by a newer one fails
 *   closed rather than ignoring the part it did not understand.
 *
 *   The wire representation is the string, so a future phase can add
 *   `repo:write` without changing the authentication protocol: the enum grows,
 *   the header does not.
 */
#ifndef ATLAS_APIKEY_H
#define ATLAS_APIKEY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "atlas/buf.h"
#include "atlas/error.h"

/* --- sizes ---------------------------------------------------------------- */

#define ATLAS_APIKEY_SELECTOR_BYTES 8u
#define ATLAS_APIKEY_SELECTOR_HEX 16u /* excluding NUL */
#define ATLAS_APIKEY_SECRET_BYTES 32u
#define ATLAS_APIKEY_SECRET_B64 43u /* base64url of 32 bytes, unpadded */
#define ATLAS_APIKEY_SALT_BYTES 16u
#define ATLAS_APIKEY_VERIFIER_BYTES 32u
#define ATLAS_APIKEY_PREFIX "atlas_"
/* "atlas_" + selector + "_" + secret, plus NUL. */
#define ATLAS_APIKEY_TOKEN_MAX 80u
/* An operator-chosen label. Bounded, and safe-encoded before it is displayed or
 * recorded: it is operator text rather than repository text, but it still
 * reaches a terminal and an audit row. */
#define ATLAS_APIKEY_LABEL_MAX 64u
/* Keys one Atlas index will hold. A ceiling rather than a policy: it bounds the
 * listing and makes "how many credentials exist" a number with an answer. */
#define ATLAS_APIKEY_MAX_KEYS 256u

/* --- scopes --------------------------------------------------------------- */

typedef enum atlas_apikey_scope {
    ATLAS_SCOPE_UNKNOWN = 0,
    ATLAS_SCOPE_CONTEXT_READ,
    ATLAS_SCOPE_REPO_READ,
    ATLAS_SCOPE_DECISIONS_READ,
    ATLAS_SCOPE_GRAPH_READ,
    ATLAS_SCOPE_IMPACT_READ,
    ATLAS_SCOPE_AUDIT_READ,
    /* Not grantable in A9 and deliberately present.
     *
     * Every Atlas tool that records something durable maps to this scope, so
     * denying a write is the ordinary scope check finding a bit that is clear —
     * not a special case somebody has to remember to write. `atlas api-key
     * create` refuses it by name, so no A9 credential can hold it, and the
     * refusal is one line rather than an audit of every tool.
     *
     * A future phase that wants remote writes enables it here, having argued
     * for it. Until then this is the bit that is never set. */
    ATLAS_SCOPE_MEMORY_WRITE,
    /* A16. Not grantable, for the same reason `MEMORY_WRITE` is not: never
     * stored on an `api_keys` row, and `atlas api-key create` refuses it by
     * name. Unlike `MEMORY_WRITE` it is not simply absent from every mask
     * forever — the daemon *derives* it, in `gateway.auth` and again at the
     * write point, for exactly the credential a root-owned
     * `remote_dispose_key` policy line names, and only when that credential's
     * own stored scope list is empty. A key that can read anything is a key
     * that could have been handed to a model over `/mcp`; deriving this scope
     * only for a key holding none is what keeps the disposal credential
     * structurally incapable of being that key. */
    ATLAS_SCOPE_DECISIONS_DISPOSE,
    /* A14. Not grantable, for the same reason `DECISIONS_DISPOSE` is not: never
     * stored on an `api_keys` row, and `atlas api-key create` refuses it by
     * name. Unlike DECISIONS_DISPOSE it is derived for a key that may hold
     * stored read scopes, and Decision 1 says why. The daemon derives it, in
     * `gateway.auth` and at the remote-submit write point, for exactly the
     * credentials the root-owned `remote_submit_key` lines name. */
    ATLAS_SCOPE_JOBS_SUBMIT,
    ATLAS_SCOPE__COUNT
} atlas_apikey_scope;

/* The canonical string, or NULL for UNKNOWN and out-of-range. */
const char *atlas_apikey_scope_name(atlas_apikey_scope s);
/* Parses one scope string. Returns ATLAS_SCOPE_UNKNOWN for anything not in the
 * vocabulary — the caller must treat that as a refusal, never as "no scope". */
atlas_apikey_scope atlas_apikey_scope_parse(const char *s);
/* True when an operator may grant this scope with `atlas api-key create`. */
bool atlas_apikey_scope_grantable(atlas_apikey_scope s);

typedef uint32_t atlas_scope_mask;

#define ATLAS_SCOPE_BIT(s) ((atlas_scope_mask)1u << (unsigned)(s))

/* True when the mask grants `s`. UNKNOWN is never granted by any mask, which is
 * what makes an unrecognised scope name a refusal rather than a hole. */
bool atlas_scope_has(atlas_scope_mask m, atlas_apikey_scope s);

/* Parses a canonical space-separated scope list into a mask.
 *
 * Fails closed on the first token it does not recognise, naming it, and leaves
 * `*out` zero. An empty list is a mask of zero and is *not* an error here — a
 * credential with no scopes is a credential that authorises nothing, which is a
 * coherent thing to store and a useless thing to hold. */
atlas_status atlas_apikey_scopes_parse(const char *list, atlas_scope_mask *out, atlas_err *err);
/* Renders a mask as the canonical space-separated list, in enum order, so the
 * stored string for one set of scopes is always the same bytes. */
atlas_status atlas_apikey_scopes_render(atlas_scope_mask m, atlas_buf *out, atlas_err *err);

/* --- status --------------------------------------------------------------- */

typedef enum atlas_apikey_status {
    /* Zero, and it authorises nothing. A row whose status Atlas cannot read is
     * not an active key. */
    ATLAS_APIKEY_STATUS_UNKNOWN = 0,
    ATLAS_APIKEY_STATUS_ACTIVE,
    ATLAS_APIKEY_STATUS_REVOKED
} atlas_apikey_status;

const char *atlas_apikey_status_name(atlas_apikey_status s);
atlas_apikey_status atlas_apikey_status_parse(const char *s);

/* --- generation and verification ------------------------------------------ */

/* Everything one freshly minted credential consists of.
 *
 * `token` is the only copy of the plaintext that will ever exist. The caller
 * prints it once and then calls `atlas_apikey_material_free`, which wipes it. */
typedef struct atlas_apikey_material {
    char key_id[ATLAS_APIKEY_SELECTOR_HEX + 1];
    char token[ATLAS_APIKEY_TOKEN_MAX];
    unsigned char salt[ATLAS_APIKEY_SALT_BYTES];
    unsigned char verifier[ATLAS_APIKEY_VERIFIER_BYTES];
} atlas_apikey_material;

/* Mints one credential from kernel randomness.
 *
 * Every byte of both the selector and the secret comes from
 * `atlas_random_bytes`. A failure there is returned unchanged and nothing is
 * produced: there is no path in which this function succeeds with a value it
 * derived some other way. */
atlas_status atlas_apikey_generate(atlas_apikey_material *out, atlas_err *err);

/* Wipes the plaintext and the verifier material. Call it on every path,
 * including the error ones. */
void atlas_apikey_material_free(atlas_apikey_material *m);

/* Splits a presented token into its selector and its decoded secret.
 *
 * Refuses anything that is not exactly the documented shape: the prefix, 16
 * lowercase hex, one underscore, 43 base64url characters, and nothing after. A
 * malformed token is never partially accepted and the error text never quotes
 * any part of it — an error message is a place a secret leaks into a log.
 *
 * `secret_out` receives ATLAS_APIKEY_SECRET_BYTES bytes on success. */
atlas_status atlas_apikey_token_parse(const char *token, char selector_out[ATLAS_APIKEY_SELECTOR_HEX + 1],
                                      unsigned char secret_out[ATLAS_APIKEY_SECRET_BYTES],
                                      atlas_err *err);

/* True when `secret` matches the stored verifier under the stored salt.
 *
 * The comparison is `atlas_ct_equal`, so the time it takes does not depend on
 * how many leading bytes were right. */
bool atlas_apikey_verify(const unsigned char *secret, size_t secret_len,
                         const unsigned char *salt, size_t salt_len,
                         const unsigned char *verifier, size_t verifier_len);

/* Extracts the token from an `Authorization` header value.
 *
 * Requires exactly `Bearer` — the scheme is compared case-insensitively as
 * RFC 7235 requires — followed by one or more spaces and then the token. A
 * header carrying a different scheme, no scheme, or trailing content after the
 * token is refused rather than salvaged. `out` must hold ATLAS_APIKEY_TOKEN_MAX
 * bytes. */
atlas_status atlas_apikey_bearer_parse(const char *header_value, char *out, size_t out_size,
                                       atlas_err *err);

/* Validates an operator-supplied label: 1..ATLAS_APIKEY_LABEL_MAX bytes of
 * printable ASCII excluding control characters and the ones that would need
 * escaping in every place a label is displayed. Refused rather than rewritten,
 * so the label an operator reads back is the label they typed. */
bool atlas_apikey_label_valid(const char *label);

#endif /* ATLAS_APIKEY_H */
