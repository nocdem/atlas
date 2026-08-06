/* Atlas - core unit tests: buffers, SHA-256, path representation, JSON writer.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas_test.h"
#include "support/jsoncheck.h"

/* --- buffers ------------------------------------------------------------- */

static void test_buf_basics(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf b = ATLAS_BUF_INIT;

    T_EQ_STR(atlas_buf_cstr(&b), "");
    T_OK(atlas_buf_append_str(&b, "hello", &err), &err);
    T_OK(atlas_buf_append_ch(&b, ' ', &err), &err);
    T_OK(atlas_buf_appendf(&b, &err, "%s-%d", "world", 42), &err);
    T_EQ_STR(atlas_buf_cstr(&b), "hello world-42");
    T_EQ_INT(b.len, 14);

    /* Embedded NUL bytes survive: the buffer is binary-safe. */
    T_OK(atlas_buf_set(&b, "a\0b", 3u, &err), &err);
    T_EQ_INT(b.len, 3);
    T_EQ_MEM(b.data, b.len, "a\0b", 3u);

    atlas_buf_reset(&b);
    T_EQ_INT(b.len, 0);
    T_EQ_STR(atlas_buf_cstr(&b), "");

    /* Reserving without appending must still leave a valid empty C string:
     * encoders reserve up front and may then append nothing at all. */
    atlas_buf fresh = ATLAS_BUF_INIT;
    T_OK(atlas_buf_reserve(&fresh, 64u, &err), &err);
    T_EQ_INT(fresh.len, 0);
    T_EQ_STR(atlas_buf_cstr(&fresh), "");
    atlas_buf_free(&fresh);

    /* Growth across many appends keeps content intact. */
    for (int i = 0; i < 5000; i++) {
        T_OK(atlas_buf_append_ch(&b, 'x', &err), &err);
    }
    T_EQ_INT(b.len, 5000);
    T_CHECK(b.data[4999] == 'x');
    T_CHECK(b.data[5000] == '\0');
    atlas_buf_free(&b);
}

/* --- SHA-256 known-answer tests (required test 31) ----------------------- */

static void check_kat(const char *input, size_t len, const char *expected) {
    char hex[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_sha256_hex(input, len, hex);
    T_CHECK_MSG(strcmp(hex, expected) == 0, "sha256 of %zu bytes: expected %s, got %s", len,
                expected, hex);
}

static void test_sha256_vectors(void) {
    /* FIPS 180-4 / NIST published vectors. */
    check_kat("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check_kat("abc", 3u, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check_kat("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56u,
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    check_kat("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
              "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
              112u, "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

static void test_sha256_streaming(void) {
    /* A digest must not depend on how the input is chunked; block boundaries at
     * 55, 56, 63, 64 and 65 bytes are where padding logic goes wrong. */
    const size_t sizes[] = {0, 1, 54, 55, 56, 57, 63, 64, 65, 119, 128, 1000};
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        size_t len = sizes[s];
        char *data = malloc(len + 1u);
        T_REQUIRE(data != NULL);
        for (size_t i = 0; i < len; i++) {
            data[i] = (char)('a' + (int)(i % 26u));
        }
        char oneshot[ATLAS_SHA256_HEX_LEN + 1u];
        atlas_sha256_hex(data, len, oneshot);

        for (size_t chunk = 1; chunk <= 7u; chunk++) {
            atlas_sha256 ctx;
            atlas_sha256_init(&ctx);
            for (size_t off = 0; off < len; off += chunk) {
                size_t take = (len - off) < chunk ? (len - off) : chunk;
                atlas_sha256_update(&ctx, data + off, take);
            }
            unsigned char digest[ATLAS_SHA256_DIGEST_LEN];
            atlas_sha256_final(&ctx, digest);
            char hex[ATLAS_SHA256_HEX_LEN + 1u];
            atlas_hex_encode(digest, sizeof(digest), hex);
            T_CHECK_MSG(strcmp(hex, oneshot) == 0,
                        "len %zu chunked by %zu differs from one-shot", len, chunk);
        }
        free(data);
    }
}

static void test_sha256_million_a(void) {
    atlas_sha256 ctx;
    atlas_sha256_init(&ctx);
    char block[1000];
    memset(block, 'a', sizeof(block));
    for (int i = 0; i < 1000; i++) {
        atlas_sha256_update(&ctx, block, sizeof(block));
    }
    unsigned char digest[ATLAS_SHA256_DIGEST_LEN];
    atlas_sha256_final(&ctx, digest);
    char hex[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_hex_encode(digest, sizeof(digest), hex);
    T_EQ_STR(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

/* --- path representation ------------------------------------------------- */

static void roundtrip(const void *raw, size_t len, const char *expected_text) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf text = ATLAS_BUF_INIT;
    atlas_buf back = ATLAS_BUF_INIT;

    T_OK(atlas_path_text_encode(raw, len, &text, &err), &err);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&text), expected_text) == 0,
                "encode: expected \"%s\", got \"%s\"", expected_text, atlas_buf_cstr(&text));
    /* The text form must always be valid UTF-8 so it is safe in JSON. */
    T_CHECK(atlas_utf8_valid(text.data, text.len));

    T_OK(atlas_path_text_decode(atlas_buf_cstr(&text), text.len, &back, &err), &err);
    T_EQ_MEM(back.data, back.len, raw, len);

    atlas_buf_free(&text);
    atlas_buf_free(&back);
}

/* Length comes from the literal itself, so a hand-counted length can never make
 * the test assert against the wrong bytes. */
#define ROUNDTRIP(raw_literal, expected_text) \
    roundtrip(raw_literal, sizeof(raw_literal) - 1u, expected_text)

static void test_path_encoding(void) {
    ROUNDTRIP("src/core/buf.c", "src/core/buf.c");
    ROUNDTRIP("with space.txt", "with space.txt");
    ROUNDTRIP("with\ttab.txt", "with%09tab.txt");
    ROUNDTRIP("with\nnewline.txt", "with%0Anewline.txt");
    ROUNDTRIP("100%pure.txt", "100%25pure.txt");
    /* Valid multi-byte UTF-8 passes through untouched. */
    ROUNDTRIP("caf\xc3\xa9/menu.txt", "caf\xc3\xa9/menu.txt");
    /* Invalid bytes are escaped one byte at a time and stay reversible. */
    ROUNDTRIP("bad\xff\xfename.txt", "bad%FF%FEname.txt");
    /* A lone continuation byte and a truncated sequence. */
    ROUNDTRIP("\x80", "%80");
    ROUNDTRIP("\xc3", "%C3");
    /* Overlong encoding of '/' must not be accepted as UTF-8. */
    ROUNDTRIP("\xc0\xaf", "%C0%AF");

    T_CHECK(atlas_path_is_plain("plain.txt", 9u));
    T_CHECK(!atlas_path_is_plain("tab\there", 8u));
    T_CHECK(!atlas_path_is_plain("bad\xff", 4u));
}

static void test_path_decode_rejects_garbage(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf out = ATLAS_BUF_INIT;
    T_FAILS_WITH(atlas_path_text_decode("abc%", 4u, &out, &err), ATLAS_ERR_USAGE, &err);
    atlas_buf_reset(&out);
    T_FAILS_WITH(atlas_path_text_decode("abc%zz", 6u, &out, &err), ATLAS_ERR_USAGE, &err);
    atlas_buf_reset(&out);
    T_FAILS_WITH(atlas_path_text_decode("abc%4", 5u, &out, &err), ATLAS_ERR_USAGE, &err);
    atlas_buf_free(&out);
}

static void test_path_check_relative(void) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_path_check_relative("a/b/c.txt", 9u, &err), &err);
    T_OK(atlas_path_check_relative("weird\nname", 10u, &err), &err);

    T_FAILS_WITH(atlas_path_check_relative("", 0, &err), ATLAS_ERR_USAGE, &err);
    T_FAILS_WITH(atlas_path_check_relative("/etc/passwd", 11u, &err), ATLAS_ERR_USAGE, &err);
    T_FAILS_WITH(atlas_path_check_relative("../escape", 9u, &err), ATLAS_ERR_USAGE, &err);
    T_FAILS_WITH(atlas_path_check_relative("a/../b", 6u, &err), ATLAS_ERR_USAGE, &err);
    T_FAILS_WITH(atlas_path_check_relative("a/./b", 5u, &err), ATLAS_ERR_USAGE, &err);
    T_FAILS_WITH(atlas_path_check_relative("a//b", 4u, &err), ATLAS_ERR_USAGE, &err);
    T_FAILS_WITH(atlas_path_check_relative("trailing/", 9u, &err), ATLAS_ERR_USAGE, &err);
    T_FAILS_WITH(atlas_path_check_relative("nul\0byte", 8u, &err), ATLAS_ERR_USAGE, &err);
}

/* --- JSON writer (required tests 25 and 30) ------------------------------ */

typedef struct json_capture {
    char *data;
    size_t len;
    FILE *fp;
} json_capture;

static void capture_open(json_capture *c) {
    c->data = NULL;
    c->len = 0;
    c->fp = open_memstream(&c->data, &c->len);
    T_REQUIRE(c->fp != NULL);
}

static void capture_close(json_capture *c) {
    if (c->fp != NULL) {
        (void)fclose(c->fp);
        c->fp = NULL;
    }
    free(c->data);
    c->data = NULL;
}

static void expect_valid_json(const json_capture *c) {
    size_t bad = 0;
    T_CHECK_MSG(tjson_valid(c->data, c->len, &bad), "invalid JSON at offset %zu: %.*s", bad,
                (int)c->len, c->data);
}

static void test_json_basic_document(void) {
    atlas_err err;
    atlas_err_init(&err);
    json_capture c;
    capture_open(&c);

    atlas_json *j = atlas_json_new(c.fp, &err);
    T_REQUIRE(j != NULL);
    T_OK(atlas_json_obj_begin(j, &err), &err);
    T_OK(atlas_json_key_str(j, "name", "atlas", &err), &err);
    T_OK(atlas_json_key_int(j, "count", -17, &err), &err);
    T_OK(atlas_json_key_bool(j, "ok", true, &err), &err);
    T_OK(atlas_json_key_null(j, "missing", &err), &err);
    T_OK(atlas_json_key(j, "items", &err), &err);
    T_OK(atlas_json_arr_begin(j, &err), &err);
    T_OK(atlas_json_str(j, "one", &err), &err);
    T_OK(atlas_json_int(j, 2, &err), &err);
    T_OK(atlas_json_obj_begin(j, &err), &err);
    T_OK(atlas_json_key_int(j, "deep", 3, &err), &err);
    T_OK(atlas_json_obj_end(j, &err), &err);
    T_OK(atlas_json_arr_end(j, &err), &err);
    T_OK(atlas_json_obj_end(j, &err), &err);
    T_OK(atlas_json_finish(j, &err), &err);

    (void)fflush(c.fp);
    expect_valid_json(&c);
    T_CHECK(strstr(c.data, "\"name\":\"atlas\"") != NULL);
    T_CHECK(strstr(c.data, "\"count\":-17") != NULL);
    T_CHECK(strstr(c.data, "\"ok\":true") != NULL);
    T_CHECK(strstr(c.data, "\"missing\":null") != NULL);
    T_CHECK(strstr(c.data, "\"items\":[\"one\",2,{\"deep\":3}]") != NULL);
    capture_close(&c);
}

static void test_json_escaping(void) {
    atlas_err err;
    atlas_err_init(&err);
    json_capture c;
    capture_open(&c);

    atlas_json *j = atlas_json_new(c.fp, &err);
    T_REQUIRE(j != NULL);
    T_OK(atlas_json_obj_begin(j, &err), &err);
    T_OK(atlas_json_key_str(j, "quote", "say \"hi\"", &err), &err);
    T_OK(atlas_json_key_str(j, "backslash", "a\\b", &err), &err);
    T_OK(atlas_json_key_str(j, "controls", "tab\there\nnewline\rcr\bbs\fff", &err), &err);
    T_OK(atlas_json_key_bytes(j, "nul_and_low", "a\x01\x1f", 3u, &err), &err);
    T_OK(atlas_json_key_str(j, "unicode", "caf\xc3\xa9", &err), &err);
    T_OK(atlas_json_obj_end(j, &err), &err);
    T_OK(atlas_json_finish(j, &err), &err);
    (void)fflush(c.fp);

    expect_valid_json(&c);
    T_CHECK(strstr(c.data, "\"quote\":\"say \\\"hi\\\"\"") != NULL);
    T_CHECK(strstr(c.data, "\"backslash\":\"a\\\\b\"") != NULL);
    T_CHECK(strstr(c.data, "\"controls\":\"tab\\there\\nnewline\\rcr\\bbs\\fff\"") != NULL);
    T_CHECK(strstr(c.data, "\"nul_and_low\":\"a\\u0001\\u001f\"") != NULL);
    /* Valid UTF-8 is not escaped. */
    T_CHECK(strstr(c.data, "\"unicode\":\"caf\xc3\xa9\"") != NULL);

    /* Round-trip through an independent decoder. */
    atlas_buf got = ATLAS_BUF_INIT;
    T_CHECK(tjson_get_string(c.data, c.len, "controls", &got));
    T_EQ_STR(atlas_buf_cstr(&got), "tab\there\nnewline\rcr\bbs\fff");
    atlas_buf_free(&got);
    capture_close(&c);
}

static void test_json_invalid_utf8(void) {
    atlas_err err;
    atlas_err_init(&err);
    json_capture c;
    capture_open(&c);

    /* Invalid bytes become U+FFFD, and the exact bytes remain available in hex. */
    const char bad[] = {'b', 'a', 'd', (char)0xff, (char)0xfe, '.', 'c'};
    atlas_json *j = atlas_json_new(c.fp, &err);
    T_REQUIRE(j != NULL);
    T_OK(atlas_json_obj_begin(j, &err), &err);
    T_OK(atlas_json_key_bytes(j, "path", bad, sizeof(bad), &err), &err);
    T_OK(atlas_json_key_hex(j, "path_bytes_hex", bad, sizeof(bad), &err), &err);
    T_OK(atlas_json_obj_end(j, &err), &err);
    T_OK(atlas_json_finish(j, &err), &err);
    (void)fflush(c.fp);

    expect_valid_json(&c);
    /* Two invalid bytes, so two replacement characters. */
    T_CHECK(strstr(c.data, "\"path\":\"bad\xef\xbf\xbd\xef\xbf\xbd.c\"") != NULL);
    T_CHECK(strstr(c.data, "\"path_bytes_hex\":\"6261646666666 e\"") == NULL);
    atlas_buf hex = ATLAS_BUF_INIT;
    T_CHECK(tjson_get_string(c.data, c.len, "path_bytes_hex", &hex));
    T_EQ_STR(atlas_buf_cstr(&hex), "626164fffe2e63");
    atlas_buf_free(&hex);
    capture_close(&c);
}

static void test_json_structure_errors(void) {
    atlas_err err;
    atlas_err_init(&err);
    json_capture c;
    capture_open(&c);

    /* A value written where a key is required must be refused rather than
     * silently producing a broken document. */
    atlas_json *j = atlas_json_new(c.fp, &err);
    T_REQUIRE(j != NULL);
    T_OK(atlas_json_obj_begin(j, &err), &err);
    T_FAILS_WITH(atlas_json_str(j, "orphan", &err), ATLAS_ERR_INTERNAL, &err);
    /* Once broken, the writer keeps reporting the failure. */
    T_FAILS_WITH(atlas_json_obj_end(j, &err), ATLAS_ERR_INTERNAL, &err);
    atlas_json_free(j);
    capture_close(&c);

    capture_open(&c);
    atlas_err_init(&err);
    j = atlas_json_new(c.fp, &err);
    T_REQUIRE(j != NULL);
    T_OK(atlas_json_arr_begin(j, &err), &err);
    /* Closing the wrong container type is refused. */
    T_FAILS_WITH(atlas_json_obj_end(j, &err), ATLAS_ERR_INTERNAL, &err);
    atlas_json_free(j);
    capture_close(&c);

    capture_open(&c);
    atlas_err_init(&err);
    j = atlas_json_new(c.fp, &err);
    T_REQUIRE(j != NULL);
    T_OK(atlas_json_obj_begin(j, &err), &err);
    /* Finishing with an open container is refused. */
    T_FAILS_WITH(atlas_json_finish(j, &err), ATLAS_ERR_INTERNAL, &err);
    capture_close(&c);
}

static void test_json_depth_limit(void) {
    atlas_err err;
    atlas_err_init(&err);
    json_capture c;
    capture_open(&c);
    atlas_json *j = atlas_json_new(c.fp, &err);
    T_REQUIRE(j != NULL);
    atlas_status st = ATLAS_OK;
    size_t depth = 0;
    while (st == ATLAS_OK && depth < ATLAS_JSON_MAX_DEPTH + 4u) {
        st = atlas_json_arr_begin(j, &err);
        depth++;
    }
    /* The nesting stack is bounded, and exceeding it is an error, not a crash. */
    T_CHECK_MSG(st == ATLAS_ERR_INTERNAL, "expected a depth limit, reached depth %zu", depth);
    T_CHECK(depth <= ATLAS_JSON_MAX_DEPTH + 1u);
    atlas_json_free(j);
    capture_close(&c);
}

static void test_utf8_validator(void) {
    T_CHECK(atlas_utf8_valid("", 0));
    T_CHECK(atlas_utf8_valid("ascii", 5u));
    T_CHECK(atlas_utf8_valid("\xc3\xa9", 2u));
    T_CHECK(atlas_utf8_valid("\xe2\x82\xac", 3u));
    T_CHECK(atlas_utf8_valid("\xf0\x9f\x98\x80", 4u));
    T_CHECK(!atlas_utf8_valid("\xff", 1u));
    T_CHECK(!atlas_utf8_valid("\xc3", 1u));
    T_CHECK(!atlas_utf8_valid("\xc3\x28", 2u));
    T_CHECK(!atlas_utf8_valid("\xc0\xaf", 2u));         /* overlong */
    T_CHECK(!atlas_utf8_valid("\xed\xa0\x80", 3u));     /* surrogate */
    T_CHECK(!atlas_utf8_valid("\xf5\x80\x80\x80", 4u)); /* > U+10FFFF */
}

static void test_status_names_and_errors(void) {
    T_EQ_STR(atlas_status_name(ATLAS_OK), "ok");
    T_EQ_STR(atlas_status_name(ATLAS_ERR_USAGE), "usage");
    T_EQ_STR(atlas_status_name(ATLAS_ERR_INTEGRITY), "integrity");
    /* Exit codes are a stable contract. */
    T_EQ_INT(ATLAS_OK, 0);
    T_EQ_INT(ATLAS_ERR_INTERNAL, 1);
    T_EQ_INT(ATLAS_ERR_USAGE, 2);
    T_EQ_INT(ATLAS_ERR_CONFIG, 3);
    T_EQ_INT(ATLAS_ERR_REPO, 4);
    T_EQ_INT(ATLAS_ERR_DB, 5);
    T_EQ_INT(ATLAS_ERR_GIT, 6);
    T_EQ_INT(ATLAS_ERR_INTEGRITY, 7);

    atlas_err err;
    atlas_err_init(&err);
    T_EQ_STR(atlas_err_msg(&err), "");
    (void)atlas_err_set(&err, ATLAS_ERR_REPO, "no repo named %s", "x");
    T_EQ_STR(atlas_err_msg(&err), "no repo named x");
    T_EQ_INT(err.status, ATLAS_ERR_REPO);
    /* A very long message is truncated, never overflowed. */
    char big[4096];
    memset(big, 'z', sizeof(big) - 1u);
    big[sizeof(big) - 1u] = '\0';
    (void)atlas_err_set(&err, ATLAS_ERR_DB, "%s", big);
    T_CHECK(strlen(atlas_err_msg(&err)) < ATLAS_ERR_MSG_MAX);
}

static const atlas_test TESTS[] = {
    {"buf basics", test_buf_basics},
    {"sha256 known-answer vectors", test_sha256_vectors},
    {"sha256 chunking independence", test_sha256_streaming},
    {"sha256 one million a", test_sha256_million_a},
    {"path text encoding round-trip", test_path_encoding},
    {"path text decoding rejects malformed escapes", test_path_decode_rejects_garbage},
    {"relative path validation", test_path_check_relative},
    {"json basic document", test_json_basic_document},
    {"json escaping", test_json_escaping},
    {"json invalid utf-8 representation", test_json_invalid_utf8},
    {"json structural errors", test_json_structure_errors},
    {"json depth limit", test_json_depth_limit},
    {"utf-8 validator", test_utf8_validator},
    {"status names and error formatting", test_status_names_and_errors},
};

ATLAS_TEST_MAIN("core", TESTS)
