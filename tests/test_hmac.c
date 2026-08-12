/* Atlas - HMAC-SHA256, the constant-time compare, and the API-key format.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The HMAC vectors are RFC 4231 sections 4.2 through 4.7, unmodified. An
 * implementation nobody checked against a published vector is a guess, and a
 * wrong HMAC here would not fail loudly — it would produce a verifier that is
 * self-consistent and worthless, so every key Atlas issued would keep working
 * while the security argument in the header stopped being true.
 *
 * The API-key tests are about the refusals. A token parser is almost all
 * refusal, and a refusal is what is easiest to get wrong quietly: a parser that
 * accepts a slightly wrong token still authenticates the right one, so nothing
 * observable goes wrong until somebody exploits the slack.
 */
#include <stdio.h>
#include <string.h>

#include "atlas/apikey.h"
#include "atlas/hmac.h"
#include "atlas_test.h"

static void hex_of(const unsigned char *d, size_t n, char *out) {
    atlas_hex_encode(d, n, out);
}

typedef struct hmac_vector {
    const char *name;
    unsigned char key[131];
    size_t key_len;
    unsigned char data[153];
    size_t data_len;
    const char *expect_hex;
} hmac_vector;

/* RFC 4231. Only the SHA-256 output is pinned; the truncated case (4.6) is
 * checked by comparing the leading 16 bytes of the full digest, because Atlas
 * has no truncating entry point and should not grow one for a test. */
static void test_rfc4231_vectors(void) {
    hmac_vector v;

    /* 4.2: key = 0x0b x 20, data = "Hi There" */
    memset(&v, 0, sizeof(v));
    memset(v.key, 0x0b, 20);
    v.key_len = 20;
    memcpy(v.data, "Hi There", 8);
    v.data_len = 8;
    v.expect_hex = "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7";
    {
        unsigned char out[ATLAS_HMAC_SHA256_LEN];
        char hex[ATLAS_SHA256_HEX_LEN + 1];
        atlas_hmac_sha256(v.key, v.key_len, v.data, v.data_len, out);
        hex_of(out, sizeof(out), hex);
        T_CHECK_MSG(strcmp(hex, v.expect_hex) == 0, "RFC 4231 4.2: got %s", hex);
    }

    /* 4.3: key = "Jefe", data = "what do ya want for nothing?" */
    {
        unsigned char out[ATLAS_HMAC_SHA256_LEN];
        char hex[ATLAS_SHA256_HEX_LEN + 1];
        atlas_hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, out);
        hex_of(out, sizeof(out), hex);
        T_CHECK_MSG(strcmp(hex,
                           "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843") == 0,
                    "RFC 4231 4.3: got %s", hex);
    }

    /* 4.4: key = 0xaa x 20, data = 0xdd x 50 */
    {
        unsigned char key[20], data[50], out[ATLAS_HMAC_SHA256_LEN];
        char hex[ATLAS_SHA256_HEX_LEN + 1];
        memset(key, 0xaa, sizeof(key));
        memset(data, 0xdd, sizeof(data));
        atlas_hmac_sha256(key, sizeof(key), data, sizeof(data), out);
        hex_of(out, sizeof(out), hex);
        T_CHECK_MSG(strcmp(hex,
                           "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe") == 0,
                    "RFC 4231 4.4: got %s", hex);
    }

    /* 4.5: key = 0x01..0x19, data = 0xcd x 50 */
    {
        unsigned char key[25], data[50], out[ATLAS_HMAC_SHA256_LEN];
        char hex[ATLAS_SHA256_HEX_LEN + 1];
        for (size_t i = 0; i < sizeof(key); i++) {
            key[i] = (unsigned char)(i + 1);
        }
        memset(data, 0xcd, sizeof(data));
        atlas_hmac_sha256(key, sizeof(key), data, sizeof(data), out);
        hex_of(out, sizeof(out), hex);
        T_CHECK_MSG(strcmp(hex,
                           "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b") == 0,
                    "RFC 4231 4.5: got %s", hex);
    }

    /* 4.6: truncation case. The full digest's first 16 bytes must match the
     * published 128-bit output. */
    {
        unsigned char key[20], out[ATLAS_HMAC_SHA256_LEN];
        char hex[ATLAS_SHA256_HEX_LEN + 1];
        memset(key, 0x0c, sizeof(key));
        atlas_hmac_sha256(key, sizeof(key), "Test With Truncation", 20, out);
        hex_of(out, 16, hex);
        T_CHECK_MSG(strcmp(hex, "a3b6167473100ee06e0c796c2955552b") == 0,
                    "RFC 4231 4.6 (first 128 bits): got %s", hex);
    }

    /* 4.7: key = 0xaa x 131 — longer than the 64-byte block, so the key is
     * hashed first. This is the branch a naive implementation gets wrong. */
    {
        unsigned char key[131], out[ATLAS_HMAC_SHA256_LEN];
        char hex[ATLAS_SHA256_HEX_LEN + 1];
        const char *data = "Test Using Larger Than Block-Size Key - Hash Key First";
        memset(key, 0xaa, sizeof(key));
        atlas_hmac_sha256(key, sizeof(key), data, strlen(data), out);
        hex_of(out, sizeof(out), hex);
        T_CHECK_MSG(strcmp(hex,
                           "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54") == 0,
                    "RFC 4231 4.7: got %s", hex);
    }

    /* 4.8: the 131-byte key with a long message, which exercises the same
     * key-hashing branch with more than one block of data. */
    {
        unsigned char key[131], out[ATLAS_HMAC_SHA256_LEN];
        char hex[ATLAS_SHA256_HEX_LEN + 1];
        const char *data = "This is a test using a larger than block-size key and a larger than "
                           "block-size data. The key needs to be hashed before being used by the "
                           "HMAC algorithm.";
        memset(key, 0xaa, sizeof(key));
        atlas_hmac_sha256(key, sizeof(key), data, strlen(data), out);
        hex_of(out, sizeof(out), hex);
        T_CHECK_MSG(strcmp(hex,
                           "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2") == 0,
                    "RFC 4231 4.8: got %s", hex);
    }
}

static void test_constant_time_compare_agrees_with_memcmp(void) {
    unsigned char a[64], b[64];
    for (size_t i = 0; i < sizeof(a); i++) {
        a[i] = (unsigned char)i;
        b[i] = (unsigned char)i;
    }
    T_CHECK(atlas_ct_equal(a, b, sizeof(a)));
    /* A difference in the last byte must be found as surely as one in the
     * first: an early-exit compare would agree here too, so this is the
     * agreement test, not the timing claim. The timing claim is structural —
     * there is no branch on the data in atlas_ct_equal. */
    b[63] ^= 0x01u;
    T_CHECK(!atlas_ct_equal(a, b, sizeof(a)));
    b[63] ^= 0x01u;
    b[0] ^= 0x80u;
    T_CHECK(!atlas_ct_equal(a, b, sizeof(a)));
    /* Zero bytes compare equal, which is what a caller with a zero-length
     * field gets; every caller in Atlas checks the length first. */
    T_CHECK(atlas_ct_equal(a, b, 0));
}

static void test_random_bytes_are_not_a_constant(void) {
    unsigned char a[32], b[32];
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_random_bytes(a, sizeof(a), &err), &err);
    T_OK(atlas_random_bytes(b, sizeof(b), &err), &err);
    /* Two 256-bit draws colliding would be a once-in-2^256 event, so this is a
     * test that the source is wired up at all rather than a statistical one. */
    T_CHECK_MSG(memcmp(a, b, sizeof(a)) != 0, "two urandom draws were identical");
    bool all_zero = true;
    for (size_t i = 0; i < sizeof(a); i++) {
        if (a[i] != 0) {
            all_zero = false;
            break;
        }
    }
    T_CHECK_MSG(!all_zero, "urandom returned 32 zero bytes");
}

/* --- the token format ------------------------------------------------------ */

static void test_a_generated_key_verifies_and_a_near_miss_does_not(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_apikey_material m;
    T_OK(atlas_apikey_generate(&m, &err), &err);

    T_CHECK(strlen(m.key_id) == ATLAS_APIKEY_SELECTOR_HEX);
    T_CHECK(strncmp(m.token, ATLAS_APIKEY_PREFIX, strlen(ATLAS_APIKEY_PREFIX)) == 0);
    /* The key id must appear in the token, because that is how a presented
     * token is looked up. */
    T_CHECK(strstr(m.token, m.key_id) != NULL);

    char sel[ATLAS_APIKEY_SELECTOR_HEX + 1];
    unsigned char secret[ATLAS_APIKEY_SECRET_BYTES];
    T_OK(atlas_apikey_token_parse(m.token, sel, secret, &err), &err);
    T_CHECK(strcmp(sel, m.key_id) == 0);
    T_CHECK(atlas_apikey_verify(secret, sizeof(secret), m.salt, sizeof(m.salt), m.verifier,
                                sizeof(m.verifier)));

    /* One flipped bit in the secret must not verify. */
    secret[0] ^= 0x01u;
    T_CHECK_MSG(!atlas_apikey_verify(secret, sizeof(secret), m.salt, sizeof(m.salt), m.verifier,
                                     sizeof(m.verifier)),
                "a secret differing by one bit verified");
    secret[0] ^= 0x01u;

    /* The same secret under a different salt must not verify: the salt is what
     * makes two keys with the same secret — which cannot happen, but must not
     * matter if it did — different stored rows. */
    unsigned char other_salt[ATLAS_APIKEY_SALT_BYTES];
    T_OK(atlas_random_bytes(other_salt, sizeof(other_salt), &err), &err);
    T_CHECK(!atlas_apikey_verify(secret, sizeof(secret), other_salt, sizeof(other_salt), m.verifier,
                                 sizeof(m.verifier)));

    /* A row of the wrong shape matches nothing rather than everything. */
    T_CHECK(!atlas_apikey_verify(secret, sizeof(secret), m.salt, 4, m.verifier, sizeof(m.verifier)));
    T_CHECK(!atlas_apikey_verify(secret, 4, m.salt, sizeof(m.salt), m.verifier, sizeof(m.verifier)));
    T_CHECK(!atlas_apikey_verify(secret, sizeof(secret), m.salt, sizeof(m.salt), m.verifier, 4));
    T_CHECK(!atlas_apikey_verify(NULL, sizeof(secret), m.salt, sizeof(m.salt), m.verifier,
                                 sizeof(m.verifier)));

    atlas_apikey_material_free(&m);
    /* The wipe must actually clear the plaintext: this is the one copy that
     * ever existed and it must not outlive the print. */
    bool any = false;
    for (size_t i = 0; i < sizeof(m.token); i++) {
        if (m.token[i] != 0) {
            any = true;
            break;
        }
    }
    T_CHECK_MSG(!any, "the plaintext token survived atlas_apikey_material_free");
}

static void test_two_generated_keys_differ_everywhere(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_apikey_material a, b;
    T_OK(atlas_apikey_generate(&a, &err), &err);
    T_OK(atlas_apikey_generate(&b, &err), &err);
    T_CHECK_MSG(strcmp(a.key_id, b.key_id) != 0, "two keys shared a selector");
    T_CHECK_MSG(strcmp(a.token, b.token) != 0, "two keys shared a token");
    T_CHECK_MSG(memcmp(a.salt, b.salt, sizeof(a.salt)) != 0, "two keys shared a salt");
    T_CHECK_MSG(memcmp(a.verifier, b.verifier, sizeof(a.verifier)) != 0,
                "two keys shared a verifier");
    /* A key must not verify against another key's stored row. */
    char sel[ATLAS_APIKEY_SELECTOR_HEX + 1];
    unsigned char secret[ATLAS_APIKEY_SECRET_BYTES];
    T_OK(atlas_apikey_token_parse(a.token, sel, secret, &err), &err);
    T_CHECK(!atlas_apikey_verify(secret, sizeof(secret), b.salt, sizeof(b.salt), b.verifier,
                                 sizeof(b.verifier)));
    atlas_apikey_material_free(&a);
    atlas_apikey_material_free(&b);
}

static void test_malformed_tokens_are_refused_without_quoting_them(void) {
    atlas_err err;
    char sel[ATLAS_APIKEY_SELECTOR_HEX + 1];
    unsigned char secret[ATLAS_APIKEY_SECRET_BYTES];

    /* A valid token, mutated one way at a time. Each mutation must be refused,
     * and no refusal may reproduce the input. */
    atlas_err_init(&err);
    atlas_apikey_material m;
    T_OK(atlas_apikey_generate(&m, &err), &err);
    char good[ATLAS_APIKEY_TOKEN_MAX];
    snprintf(good, sizeof(good), "%s", m.token);
    atlas_apikey_material_free(&m);

    const char *bad[] = {
        NULL,
        "",
        "atlas_",
        "atlas__",
        "not-a-token",
        /* the right length, the wrong prefix */
        "atlbs_0123456789abcdef_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        /* uppercase hex in the selector: one credential, one spelling */
        "atlas_0123456789ABCDEF_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        /* a character outside the base64url alphabet */
        "atlas_0123456789abcdef_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA+",
        /* padding is not part of the encoding */
        "atlas_0123456789abcdef_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=",
        /* a separator that is not an underscore */
        "atlas_0123456789abcdef-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        atlas_err_init(&err);
        atlas_status st = atlas_apikey_token_parse(bad[i], sel, secret, &err);
        T_CHECK_MSG(st != ATLAS_OK, "a malformed token was accepted at index %zu", i);
        if (bad[i] != NULL && bad[i][0] != '\0') {
            T_CHECK_MSG(strstr(atlas_err_msg(&err), bad[i]) == NULL,
                        "the refusal quoted the token back at index %zu", i);
        }
    }

    /* Truncation by one character, at each end. */
    {
        char t[ATLAS_APIKEY_TOKEN_MAX];
        snprintf(t, sizeof(t), "%s", good);
        t[strlen(t) - 1] = '\0';
        atlas_err_init(&err);
        T_CHECK(atlas_apikey_token_parse(t, sel, secret, &err) != ATLAS_OK);
    }
    {
        atlas_err_init(&err);
        T_CHECK(atlas_apikey_token_parse(good + 1, sel, secret, &err) != ATLAS_OK);
    }
    /* One extra character makes it a different token, not a longer one. */
    {
        char t[ATLAS_APIKEY_TOKEN_MAX + 4];
        snprintf(t, sizeof(t), "%sA", good);
        atlas_err_init(&err);
        T_CHECK(atlas_apikey_token_parse(t, sel, secret, &err) != ATLAS_OK);
    }
    /* The good one still parses, so the refusals above are about the mutation
     * rather than about the parser refusing everything. */
    atlas_err_init(&err);
    T_OK(atlas_apikey_token_parse(good, sel, secret, &err), &err);
}

static void test_bearer_header_parsing(void) {
    atlas_err err;
    char out[ATLAS_APIKEY_TOKEN_MAX];

    struct {
        const char *value;
        bool ok;
        const char *expect;
    } cases[] = {
        {"Bearer atlas_abc", true, "atlas_abc"},
        {"bearer atlas_abc", true, "atlas_abc"},  /* the scheme is case-insensitive */
        {"BEARER atlas_abc", true, "atlas_abc"},
        {"Bearer   atlas_abc  ", true, "atlas_abc"}, /* surrounding space is trimmed */
        {"Bearer\tatlas_abc", true, "atlas_abc"},
        {"Bearer", false, NULL},
        {"Bearer ", false, NULL},
        {"Basic atlas_abc", false, NULL},
        {"Token atlas_abc", false, NULL},
        {"atlas_abc", false, NULL},
        {"", false, NULL},
        {NULL, false, NULL},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        atlas_err_init(&err);
        atlas_status st = atlas_apikey_bearer_parse(cases[i].value, out, sizeof(out), &err);
        if (cases[i].ok) {
            T_CHECK_MSG(st == ATLAS_OK, "case %zu was refused: %s", i, atlas_err_msg(&err));
            T_CHECK_MSG(strcmp(out, cases[i].expect) == 0, "case %zu produced \"%s\"", i, out);
        } else {
            T_CHECK_MSG(st != ATLAS_OK, "case %zu was accepted", i);
            T_CHECK_MSG(out[0] == '\0', "case %zu left a token behind", i);
        }
    }
}

/* --- scopes ---------------------------------------------------------------- */

static void test_scope_vocabulary_is_closed_and_fails_closed(void) {
    atlas_err err;
    atlas_scope_mask m = 0xffffffffu;

    /* An unknown scope is a refusal with a zeroed mask, never a mask missing
     * one bit. A parser that dropped what it did not understand would silently
     * grant a credential fewer restrictions than were written down. */
    atlas_err_init(&err);
    T_CHECK(atlas_apikey_scopes_parse("repo:read wat:read", &m, &err) != ATLAS_OK);
    T_CHECK_MSG(m == 0u, "an unknown scope left bits set");
    T_CHECK(strstr(atlas_err_msg(&err), "wat:read") != NULL);

    /* Case matters: the vocabulary is exact. */
    atlas_err_init(&err);
    T_CHECK(atlas_apikey_scopes_parse("REPO:READ", &m, &err) != ATLAS_OK);
    T_CHECK(m == 0u);

    /* A very long token cannot be a scope and must not be copied anywhere. */
    atlas_err_init(&err);
    T_CHECK(atlas_apikey_scopes_parse(
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &m,
                &err) != ATLAS_OK);
    T_CHECK(m == 0u);

    /* UNKNOWN is never granted by any mask, including an all-bits one. */
    T_CHECK_MSG(!atlas_scope_has(0xffffffffu, ATLAS_SCOPE_UNKNOWN),
                "an all-bits mask granted UNKNOWN");
    T_CHECK(!atlas_scope_has(0u, ATLAS_SCOPE_REPO_READ));

    /* The round trip is exact and canonical. */
    atlas_err_init(&err);
    T_OK(atlas_apikey_scopes_parse("graph:read repo:read context:read", &m, &err), &err);
    T_CHECK(atlas_scope_has(m, ATLAS_SCOPE_REPO_READ));
    T_CHECK(atlas_scope_has(m, ATLAS_SCOPE_GRAPH_READ));
    T_CHECK(atlas_scope_has(m, ATLAS_SCOPE_CONTEXT_READ));
    T_CHECK(!atlas_scope_has(m, ATLAS_SCOPE_AUDIT_READ));
    {
        atlas_buf rendered = ATLAS_BUF_INIT;
        T_OK(atlas_apikey_scopes_render(m, &rendered, &err), &err);
        /* Enum order, not the order they were written in. */
        T_CHECK_MSG(strcmp(atlas_buf_cstr(&rendered), "context:read repo:read graph:read") == 0,
                    "canonical rendering was \"%s\"", atlas_buf_cstr(&rendered));
        atlas_scope_mask again = 0u;
        T_OK(atlas_apikey_scopes_parse(atlas_buf_cstr(&rendered), &again, &err), &err);
        T_CHECK(again == m);
        atlas_buf_free(&rendered);
    }

    /* An empty list is an empty mask and not an error: a credential with no
     * scopes authorises nothing, which is coherent to store. */
    atlas_err_init(&err);
    T_OK(atlas_apikey_scopes_parse("", &m, &err), &err);
    T_CHECK(m == 0u);
    atlas_err_init(&err);
    T_OK(atlas_apikey_scopes_parse("   ", &m, &err), &err);
    T_CHECK(m == 0u);
}

static void test_the_write_scope_exists_and_cannot_be_granted(void) {
    /* A9's write denial is structural: the scope is in the vocabulary so every
     * write tool maps to it, and it is not grantable so no credential holds it.
     * If a later phase makes it grantable, that is a deliberate edit here. */
    T_CHECK(atlas_apikey_scope_parse("memory:write") == ATLAS_SCOPE_MEMORY_WRITE);
    T_CHECK_MSG(!atlas_apikey_scope_grantable(ATLAS_SCOPE_MEMORY_WRITE),
                "memory:write became grantable, which would let a remote credential write");
    T_CHECK(atlas_apikey_scope_grantable(ATLAS_SCOPE_REPO_READ));
    T_CHECK(!atlas_apikey_scope_grantable(ATLAS_SCOPE_UNKNOWN));

    /* Every read scope A9 promises is present and named exactly. */
    const char *required[] = {"context:read", "repo:read",   "decisions:read",
                              "graph:read",   "impact:read", "audit:read"};
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        atlas_apikey_scope s = atlas_apikey_scope_parse(required[i]);
        T_CHECK_MSG(s != ATLAS_SCOPE_UNKNOWN, "the scope \"%s\" is missing", required[i]);
        T_CHECK_MSG(atlas_apikey_scope_grantable(s), "the scope \"%s\" is not grantable",
                    required[i]);
        T_CHECK(strcmp(atlas_apikey_scope_name(s), required[i]) == 0);
    }
}

static void test_status_vocabulary_keeps_unknown_at_zero(void) {
    atlas_apikey_status zeroed;
    memset(&zeroed, 0, sizeof(zeroed));
    T_CHECK_MSG(zeroed == ATLAS_APIKEY_STATUS_UNKNOWN,
                "a zeroed status is not UNKNOWN, so a memset would produce a usable key");
    T_CHECK(atlas_apikey_status_parse("ACTIVE") == ATLAS_APIKEY_STATUS_ACTIVE);
    T_CHECK(atlas_apikey_status_parse("REVOKED") == ATLAS_APIKEY_STATUS_REVOKED);
    T_CHECK(atlas_apikey_status_parse("active") == ATLAS_APIKEY_STATUS_UNKNOWN);
    T_CHECK(atlas_apikey_status_parse(NULL) == ATLAS_APIKEY_STATUS_UNKNOWN);
    T_CHECK(atlas_apikey_status_parse("") == ATLAS_APIKEY_STATUS_UNKNOWN);
}

static void test_labels_are_validated_not_rewritten(void) {
    T_CHECK(atlas_apikey_label_valid("chatgpt"));
    T_CHECK(atlas_apikey_label_valid("chatgpt read only"));
    T_CHECK(atlas_apikey_label_valid("a"));
    T_CHECK(!atlas_apikey_label_valid(""));
    T_CHECK(!atlas_apikey_label_valid(NULL));
    T_CHECK_MSG(!atlas_apikey_label_valid(" leading"), "a leading space was accepted");
    T_CHECK_MSG(!atlas_apikey_label_valid("trailing "), "a trailing space was accepted");
    T_CHECK_MSG(!atlas_apikey_label_valid("new\nline"), "a newline was accepted into a label");
    T_CHECK_MSG(!atlas_apikey_label_valid("bell\x07"), "a control byte was accepted");
    T_CHECK_MSG(!atlas_apikey_label_valid("esc\x1b[31m"), "an escape sequence was accepted");
    T_CHECK_MSG(!atlas_apikey_label_valid("quote\"d"), "a quote was accepted");
    T_CHECK_MSG(!atlas_apikey_label_valid("back\\slash"), "a backslash was accepted");
    T_CHECK_MSG(!atlas_apikey_label_valid("per%cent"), "a percent was accepted");
    {
        char too_long[ATLAS_APIKEY_LABEL_MAX + 8];
        memset(too_long, 'a', sizeof(too_long) - 1);
        too_long[sizeof(too_long) - 1] = '\0';
        T_CHECK_MSG(!atlas_apikey_label_valid(too_long), "an over-long label was accepted");
    }
    {
        char exact[ATLAS_APIKEY_LABEL_MAX + 1];
        memset(exact, 'a', ATLAS_APIKEY_LABEL_MAX);
        exact[ATLAS_APIKEY_LABEL_MAX] = '\0';
        T_CHECK(atlas_apikey_label_valid(exact));
    }
}

static const atlas_test TESTS[] = {
    {"RFC 4231 HMAC-SHA256 vectors", test_rfc4231_vectors},
    {"the constant-time compare agrees with memcmp", test_constant_time_compare_agrees_with_memcmp},
    {"random bytes come from the kernel", test_random_bytes_are_not_a_constant},
    {"a generated key verifies and a near miss does not",
     test_a_generated_key_verifies_and_a_near_miss_does_not},
    {"two generated keys differ everywhere", test_two_generated_keys_differ_everywhere},
    {"malformed tokens are refused without quoting them",
     test_malformed_tokens_are_refused_without_quoting_them},
    {"the bearer header is parsed strictly", test_bearer_header_parsing},
    {"the scope vocabulary is closed and fails closed",
     test_scope_vocabulary_is_closed_and_fails_closed},
    {"the write scope exists and cannot be granted",
     test_the_write_scope_exists_and_cannot_be_granted},
    {"the status vocabulary keeps UNKNOWN at zero",
     test_status_vocabulary_keeps_unknown_at_zero},
    {"labels are validated rather than rewritten", test_labels_are_validated_not_rewritten},
};

ATLAS_TEST_MAIN("hmac", TESTS)
