/* Atlas - A9 API credentials: scopes, token format, stored verifier.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See include/atlas/apikey.h for the format and the reasoning.
 */
#include <stdio.h>
#include <string.h>

#include "atlas/apikey.h"
#include "atlas/hmac.h"

/* --- scopes --------------------------------------------------------------- */

/* One row per member of the enum, in enum order. The order is the canonical
 * rendering order, so one set of scopes always stores as the same bytes. */
typedef struct scope_row {
    atlas_apikey_scope scope;
    const char *name;
    bool grantable;
} scope_row;

static const scope_row SCOPES[] = {
    {ATLAS_SCOPE_CONTEXT_READ, "context:read", true},
    {ATLAS_SCOPE_REPO_READ, "repo:read", true},
    {ATLAS_SCOPE_DECISIONS_READ, "decisions:read", true},
    {ATLAS_SCOPE_GRAPH_READ, "graph:read", true},
    {ATLAS_SCOPE_IMPACT_READ, "impact:read", true},
    {ATLAS_SCOPE_AUDIT_READ, "audit:read", true},
    /* Not grantable. See the header: this is the bit that is never set in A9,
     * and it exists so that denying a write is the ordinary check finding a
     * clear bit rather than a special case in every tool. */
    {ATLAS_SCOPE_MEMORY_WRITE, "memory:write", false},
    /* Not grantable. A16, Decision 2: derived by the daemon for exactly the
     * credential a root-owned `remote_dispose_key` policy line names, and
     * only for one holding no stored scope of its own. */
    {ATLAS_SCOPE_DECISIONS_DISPOSE, "decisions:dispose", false},
};

#define SCOPE_COUNT (sizeof(SCOPES) / sizeof(SCOPES[0]))

const char *atlas_apikey_scope_name(atlas_apikey_scope s) {
    for (size_t i = 0; i < SCOPE_COUNT; i++) {
        if (SCOPES[i].scope == s) {
            return SCOPES[i].name;
        }
    }
    return NULL;
}

atlas_apikey_scope atlas_apikey_scope_parse(const char *s) {
    if (s == NULL) {
        return ATLAS_SCOPE_UNKNOWN;
    }
    for (size_t i = 0; i < SCOPE_COUNT; i++) {
        if (strcmp(SCOPES[i].name, s) == 0) {
            return SCOPES[i].scope;
        }
    }
    return ATLAS_SCOPE_UNKNOWN;
}

bool atlas_apikey_scope_grantable(atlas_apikey_scope s) {
    for (size_t i = 0; i < SCOPE_COUNT; i++) {
        if (SCOPES[i].scope == s) {
            return SCOPES[i].grantable;
        }
    }
    return false;
}

bool atlas_scope_has(atlas_scope_mask m, atlas_apikey_scope s) {
    if (s == ATLAS_SCOPE_UNKNOWN || s >= ATLAS_SCOPE__COUNT) {
        return false;
    }
    return (m & ATLAS_SCOPE_BIT(s)) != 0u;
}

atlas_status atlas_apikey_scopes_parse(const char *list, atlas_scope_mask *out, atlas_err *err) {
    *out = 0u;
    if (list == NULL) {
        return ATLAS_OK;
    }
    atlas_scope_mask m = 0u;
    size_t i = 0;
    while (list[i] != '\0') {
        while (list[i] == ' ') {
            i++;
        }
        if (list[i] == '\0') {
            break;
        }
        size_t start = i;
        while (list[i] != '\0' && list[i] != ' ') {
            i++;
        }
        size_t len = i - start;
        /* The longest name in the vocabulary comfortably fits; anything longer
         * cannot be a scope, and copying it into a fixed buffer would be the
         * one place a length check could be forgotten. */
        char token[32];
        if (len == 0 || len >= sizeof(token)) {
            *out = 0u;
            return atlas_err_set(err, ATLAS_ERR_USAGE, "unknown scope");
        }
        memcpy(token, list + start, len);
        token[len] = '\0';
        atlas_apikey_scope s = atlas_apikey_scope_parse(token);
        if (s == ATLAS_SCOPE_UNKNOWN) {
            /* Fail closed, and name it only when naming it is safe.
             *
             * The token reaches here from an operator's command line or from a
             * stored row, and a refusal that does not say which scope was wrong
             * is a refusal nobody can act on. But this message goes to a
             * terminal and into an error document, and there is no safe pool at
             * this layer — so a value carrying anything but printable ASCII is
             * described rather than reproduced. */
            bool printable = true;
            for (size_t k = 0; k < len; k++) {
                unsigned char c = (unsigned char)token[k];
                if (c < 0x21u || c > 0x7eu) {
                    printable = false;
                    break;
                }
            }
            *out = 0u;
            if (printable) {
                return atlas_err_set(err, ATLAS_ERR_USAGE, "unknown scope \"%s\"", token);
            }
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "unknown scope (its name is not printable ASCII)");
        }
        m |= ATLAS_SCOPE_BIT(s);
    }
    *out = m;
    return ATLAS_OK;
}

atlas_status atlas_apikey_scopes_render(atlas_scope_mask m, atlas_buf *out, atlas_err *err) {
    atlas_buf_reset(out);
    bool first = true;
    for (size_t i = 0; i < SCOPE_COUNT; i++) {
        if (!atlas_scope_has(m, SCOPES[i].scope)) {
            continue;
        }
        atlas_status st = ATLAS_OK;
        if (!first) {
            st = atlas_buf_append_ch(out, ' ', err);
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_append_str(out, SCOPES[i].name, err);
        }
        if (st != ATLAS_OK) {
            return st;
        }
        first = false;
    }
    /* An empty mask renders as the empty string, which parses back to an empty
     * mask. The round trip has to hold or a stored credential would change
     * meaning between writing and reading it. */
    return atlas_buf_append_str(out, "", err);
}

/* --- status --------------------------------------------------------------- */

const char *atlas_apikey_status_name(atlas_apikey_status s) {
    switch (s) {
    case ATLAS_APIKEY_STATUS_ACTIVE:
        return "ACTIVE";
    case ATLAS_APIKEY_STATUS_REVOKED:
        return "REVOKED";
    case ATLAS_APIKEY_STATUS_UNKNOWN:
        break;
    }
    return "UNKNOWN";
}

atlas_apikey_status atlas_apikey_status_parse(const char *s) {
    if (s == NULL) {
        return ATLAS_APIKEY_STATUS_UNKNOWN;
    }
    if (strcmp(s, "ACTIVE") == 0) {
        return ATLAS_APIKEY_STATUS_ACTIVE;
    }
    if (strcmp(s, "REVOKED") == 0) {
        return ATLAS_APIKEY_STATUS_REVOKED;
    }
    return ATLAS_APIKEY_STATUS_UNKNOWN;
}

/* --- encodings ------------------------------------------------------------ */

static const char B64URL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/* base64url without padding, RFC 4648 section 5.
 *
 * Used for the secret only. The alphabet has no `+`, no `/` and no `=`, so a
 * token survives a URL, a shell word and an HTTP header unchanged — which
 * matters because a token that gets re-encoded somewhere in transit becomes a
 * token that fails to authenticate for a reason nobody can see. */
static void b64url_encode(const unsigned char *in, size_t n, char *out) {
    size_t o = 0;
    size_t i = 0;
    while (i + 3 <= n) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | (uint32_t)in[i + 2];
        out[o++] = B64URL[(v >> 18) & 0x3fu];
        out[o++] = B64URL[(v >> 12) & 0x3fu];
        out[o++] = B64URL[(v >> 6) & 0x3fu];
        out[o++] = B64URL[v & 0x3fu];
        i += 3;
    }
    size_t rem = n - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = B64URL[(v >> 18) & 0x3fu];
        out[o++] = B64URL[(v >> 12) & 0x3fu];
    } else if (rem == 2) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++] = B64URL[(v >> 18) & 0x3fu];
        out[o++] = B64URL[(v >> 12) & 0x3fu];
        out[o++] = B64URL[(v >> 6) & 0x3fu];
    }
    out[o] = '\0';
}

static int b64url_value(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '-') {
        return 62;
    }
    if (c == '_') {
        return 63;
    }
    return -1;
}

/* Decodes exactly ATLAS_APIKEY_SECRET_B64 characters into
 * ATLAS_APIKEY_SECRET_BYTES bytes. Returns false on any character outside the
 * alphabet. 43 characters encode 32 bytes with 2 bits left over; those bits must
 * be zero, or two distinct tokens would decode to the same secret and one of
 * them would be a second valid spelling of somebody's credential. */
static bool b64url_decode_secret(const char *in, unsigned char *out) {
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < ATLAS_APIKEY_SECRET_B64; i++) {
        int v = b64url_value(in[i]);
        if (v < 0) {
            return false;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= ATLAS_APIKEY_SECRET_BYTES) {
                return false;
            }
            out[o++] = (unsigned char)((acc >> bits) & 0xffu);
        }
    }
    if (o != ATLAS_APIKEY_SECRET_BYTES) {
        return false;
    }
    /* The trailing bits must be zero: a canonical encoding has exactly one
     * spelling. */
    if (bits != 0 && (acc & ((1u << bits) - 1u)) != 0u) {
        return false;
    }
    return true;
}

static bool is_lower_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

/* --- generation ----------------------------------------------------------- */

atlas_status atlas_apikey_generate(atlas_apikey_material *out, atlas_err *err) {
    memset(out, 0, sizeof(*out));

    unsigned char selector[ATLAS_APIKEY_SELECTOR_BYTES];
    unsigned char secret[ATLAS_APIKEY_SECRET_BYTES];

    atlas_status st = atlas_random_bytes(selector, sizeof(selector), err);
    if (st == ATLAS_OK) {
        st = atlas_random_bytes(secret, sizeof(secret), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_random_bytes(out->salt, sizeof(out->salt), err);
    }
    if (st != ATLAS_OK) {
        /* Nothing partial survives a randomness failure. */
        memset(selector, 0, sizeof(selector));
        memset(secret, 0, sizeof(secret));
        atlas_apikey_material_free(out);
        return st;
    }

    atlas_hex_encode(selector, sizeof(selector), out->key_id);

    char secret_text[ATLAS_APIKEY_SECRET_B64 + 1];
    b64url_encode(secret, sizeof(secret), secret_text);

    int n = snprintf(out->token, sizeof(out->token), ATLAS_APIKEY_PREFIX "%s_%s", out->key_id,
                     secret_text);
    if (n <= 0 || (size_t)n >= sizeof(out->token)) {
        memset(selector, 0, sizeof(selector));
        memset(secret, 0, sizeof(secret));
        memset(secret_text, 0, sizeof(secret_text));
        atlas_apikey_material_free(out);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "cannot format an API key token");
    }

    atlas_hmac_sha256(out->salt, sizeof(out->salt), secret, sizeof(secret), out->verifier);

    {
        volatile unsigned char *p = secret;
        for (size_t i = 0; i < sizeof(secret); i++) {
            p[i] = 0;
        }
        volatile char *q = secret_text;
        for (size_t i = 0; i < sizeof(secret_text); i++) {
            q[i] = 0;
        }
    }
    memset(selector, 0, sizeof(selector));
    return ATLAS_OK;
}

void atlas_apikey_material_free(atlas_apikey_material *m) {
    if (m == NULL) {
        return;
    }
    volatile unsigned char *p = (volatile unsigned char *)m;
    for (size_t i = 0; i < sizeof(*m); i++) {
        p[i] = 0;
    }
}

atlas_status atlas_apikey_token_parse(const char *token,
                                      char selector_out[ATLAS_APIKEY_SELECTOR_HEX + 1],
                                      unsigned char secret_out[ATLAS_APIKEY_SECRET_BYTES],
                                      atlas_err *err) {
    memset(selector_out, 0, ATLAS_APIKEY_SELECTOR_HEX + 1);
    memset(secret_out, 0, ATLAS_APIKEY_SECRET_BYTES);

    /* Every refusal below uses the same message. A parser that says *how* a
     * token was wrong tells whoever is guessing which half to keep trying, and
     * an error string is the one place a near-miss secret would otherwise be
     * written down. */
    static const char *const BAD = "the API key is not a valid Atlas key";

    if (token == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "%s", BAD);
    }
    const size_t prefix_len = sizeof(ATLAS_APIKEY_PREFIX) - 1u;
    size_t len = strlen(token);
    if (len != prefix_len + ATLAS_APIKEY_SELECTOR_HEX + 1u + ATLAS_APIKEY_SECRET_B64) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "%s", BAD);
    }
    if (memcmp(token, ATLAS_APIKEY_PREFIX, prefix_len) != 0) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "%s", BAD);
    }
    const char *sel = token + prefix_len;
    for (size_t i = 0; i < ATLAS_APIKEY_SELECTOR_HEX; i++) {
        if (!is_lower_hex(sel[i])) {
            return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "%s", BAD);
        }
    }
    if (sel[ATLAS_APIKEY_SELECTOR_HEX] != '_') {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "%s", BAD);
    }
    const char *secret_text = sel + ATLAS_APIKEY_SELECTOR_HEX + 1u;
    unsigned char secret[ATLAS_APIKEY_SECRET_BYTES];
    if (!b64url_decode_secret(secret_text, secret)) {
        memset(secret, 0, sizeof(secret));
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "%s", BAD);
    }
    memcpy(selector_out, sel, ATLAS_APIKEY_SELECTOR_HEX);
    selector_out[ATLAS_APIKEY_SELECTOR_HEX] = '\0';
    memcpy(secret_out, secret, sizeof(secret));
    memset(secret, 0, sizeof(secret));
    return ATLAS_OK;
}

bool atlas_apikey_verify(const unsigned char *secret, size_t secret_len, const unsigned char *salt,
                         size_t salt_len, const unsigned char *verifier, size_t verifier_len) {
    /* A stored row of the wrong shape is not a key that matches everything; it
     * is a key nothing matches. */
    if (secret == NULL || salt == NULL || verifier == NULL) {
        return false;
    }
    if (secret_len != ATLAS_APIKEY_SECRET_BYTES || salt_len != ATLAS_APIKEY_SALT_BYTES ||
        verifier_len != ATLAS_APIKEY_VERIFIER_BYTES) {
        return false;
    }
    unsigned char got[ATLAS_HMAC_SHA256_LEN];
    atlas_hmac_sha256(salt, salt_len, secret, secret_len, got);
    bool ok = atlas_ct_equal(got, verifier, ATLAS_APIKEY_VERIFIER_BYTES);
    memset(got, 0, sizeof(got));
    return ok;
}

atlas_status atlas_apikey_bearer_parse(const char *header_value, char *out, size_t out_size,
                                       atlas_err *err) {
    if (out_size > 0) {
        out[0] = '\0';
    }
    static const char *const BAD = "the Authorization header must be \"Bearer <atlas API key>\"";
    if (header_value == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "%s", BAD);
    }
    const char *p = header_value;
    /* RFC 7235: the auth-scheme token is case-insensitive. */
    static const char scheme[] = "bearer";
    for (size_t i = 0; i < sizeof(scheme) - 1u; i++) {
        char c = p[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != scheme[i]) {
            return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "%s", BAD);
        }
    }
    p += sizeof(scheme) - 1u;
    if (*p != ' ' && *p != '\t') {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "%s", BAD);
    }
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    size_t len = strlen(p);
    /* Trailing whitespace is trimmed because a header value legitimately picks
     * some up in transit; anything else after the token is refused, not
     * ignored. */
    while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t')) {
        len--;
    }
    if (len == 0 || len >= out_size) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "%s", BAD);
    }
    memcpy(out, p, len);
    out[len] = '\0';
    return ATLAS_OK;
}

bool atlas_apikey_label_valid(const char *label) {
    if (label == NULL) {
        return false;
    }
    size_t n = strlen(label);
    if (n == 0 || n > ATLAS_APIKEY_LABEL_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)label[i];
        /* Printable ASCII only, and no quote, backslash or percent: a label is
         * displayed in a terminal, in JSON and in an audit row, and refusing
         * the characters those would have to escape means the label an operator
         * reads back is the label they typed. */
        if (c < 0x21u || c > 0x7eu) {
            /* A single interior space is allowed; leading and trailing are not. */
            if (c == ' ' && i > 0 && i + 1 < n) {
                continue;
            }
            return false;
        }
        if (c == '"' || c == '\\' || c == '%') {
            return false;
        }
    }
    return true;
}
