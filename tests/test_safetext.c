/* Atlas - untrusted-text safety tests.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Repository content and Git metadata are data, never instructions and never
 * terminal commands. These tests drive the encoder directly with adversarial
 * payloads; test_terminal.c then proves the whole CLI applies it.
 */
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/safetext.h"
#include "atlas_test.h"

#define ENCODES(raw_literal, expected) \
    check_encode(raw_literal, sizeof(raw_literal) - 1u, expected)

static void check_encode(const void *raw, size_t n, const char *expected) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf out = ATLAS_BUF_INIT;
    atlas_buf back = ATLAS_BUF_INIT;

    T_OK(atlas_text_encode_safe(raw, n, &out, &err), &err);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&out), expected) == 0, "expected \"%s\", got \"%s\"", expected,
                atlas_buf_cstr(&out));

    /* Three properties every encoded value must have. */
    T_CHECK_MSG(atlas_utf8_valid(out.data, out.len), "encoded output is not valid UTF-8");
    for (size_t i = 0; i < out.len; i++) {
        unsigned char c = (unsigned char)out.data[i];
        T_CHECK_MSG(c >= 0x20u && c != 0x7fu, "encoded output still contains byte 0x%02x", c);
    }
    T_OK(atlas_text_decode_safe(atlas_buf_cstr(&out), out.len, &back, &err), &err);
    T_EQ_MEM(back.data, back.len, raw, n);

    atlas_buf_free(&out);
    atlas_buf_free(&back);
}

static void test_plain_text_is_untouched(void) {
    ENCODES("a normal commit subject", "a normal commit subject");
    ENCODES("src/core/buf.c", "src/core/buf.c");
    ENCODES("Ada Lovelace <ada@example.org>", "Ada Lovelace <ada@example.org>");
    /* Ordinary non-ASCII stays readable. */
    ENCODES("caf\xc3\xa9 na\xc3\xafve", "caf\xc3\xa9 na\xc3\xafve");
    ENCODES("\xe2\x82\xac 20", "\xe2\x82\xac 20");
    ENCODES("", "");
}

static void test_c0_controls(void) {
    /* The classic terminal levers. */
    ENCODES("\x1b[31mRED", "%1B[31mRED");                  /* ANSI colour */
    ENCODES("bell\x07", "bell%07");                        /* BEL */
    ENCODES("before\rafter", "before%0Dafter");             /* CR overwrite */
    ENCODES("line1\nline2", "line1%0Aline2");               /* embedded newline */
    ENCODES("col1\tcol2", "col1%09col2");                   /* tab */
    ENCODES("\x08" "backspace", "%08backspace");
    ENCODES("\x00", "%00");
    ENCODES("\x7f", "%7F");                                 /* DEL */
    /* '%' itself, so the encoding stays reversible. */
    ENCODES("100% done", "100%25 done");
}

static void test_terminal_payloads(void) {
    /* OSC window-title injection, terminated by BEL. */
    ENCODES("\x1b]0;pwned\x07", "%1B]0;pwned%07");
    /* OSC 8 hyperlink, which can make text point somewhere it does not say. */
    ENCODES("\x1b]8;;http://evil.example\x1b\\label\x1b]8;;\x1b\\",
            "%1B]8;;http://evil.example%1B\\label%1B]8;;%1B\\");
    /* Cursor movement and screen clear. */
    ENCODES("\x1b[2J\x1b[H", "%1B[2J%1B[H");
    /* A device-control string. */
    ENCODES("\x1bP0;1|", "%1BP0;1|");
    /* C1 CSI as a single code point (U+009B), valid UTF-8 but still a control. */
    ENCODES("\xc2\x9b" "31m", "%C2%9B31m");
    /* C1 range boundaries. */
    ENCODES("\xc2\x80", "%C2%80");
    ENCODES("\xc2\x9f", "%C2%9F");
    /* U+00A0 is just above C1 and is left alone. */
    ENCODES("\xc2\xa0", "\xc2\xa0");
}

static void test_bidi_and_separators(void) {
    /* Bidirectional overrides make displayed text differ from stored bytes,
     * which is the Trojan Source trick. */
    ENCODES("\xe2\x80\xae" "gnp.txt", "%E2%80%AEgnp.txt");   /* U+202E RLO */
    ENCODES("\xe2\x80\xaa", "%E2%80%AA");                     /* U+202A LRE */
    ENCODES("\xe2\x81\xa6", "%E2%81%A6");                     /* U+2066 LRI */
    ENCODES("\xe2\x81\xa9", "%E2%81%A9");                     /* U+2069 PDI */
    ENCODES("\xe2\x80\x8e", "%E2%80%8E");                     /* U+200E LRM */
    /* Line and paragraph separators can break line-oriented output. */
    ENCODES("a\xe2\x80\xa8" "b", "a%E2%80%A8b");              /* U+2028 */
    ENCODES("a\xe2\x80\xa9" "b", "a%E2%80%A9b");              /* U+2029 */
    /* A zero-width space is not a control and is left alone. */
    ENCODES("\xe2\x80\x8b", "\xe2\x80\x8b");
}

static void test_invalid_utf8(void) {
    ENCODES("bad\xff\xfename", "bad%FF%FEname");
    ENCODES("\x80", "%80");                  /* lone continuation byte */
    ENCODES("\xc3", "%C3");                  /* truncated sequence */
    ENCODES("\xc0\xaf", "%C0%AF");           /* overlong '/' */
    ENCODES("\xed\xa0\x80", "%ED%A0%80");    /* surrogate */
    ENCODES("\xf5\x80\x80\x80", "%F5%80%80%80"); /* beyond U+10FFFF */
    /* A valid sequence following an invalid byte still decodes normally. */
    ENCODES("\xff" "caf\xc3\xa9", "%FFcaf\xc3\xa9");
}

static void test_policy_predicate(void) {
    /* The policy is asserted directly, not inferred from examples. */
    for (uint32_t cp = 0; cp < 0x20u; cp++) {
        T_CHECK_MSG(atlas_codepoint_is_unsafe(cp), "U+%04X should be escaped", cp);
    }
    T_CHECK(atlas_codepoint_is_unsafe(0x7f));
    for (uint32_t cp = 0x80u; cp <= 0x9fu; cp++) {
        T_CHECK_MSG(atlas_codepoint_is_unsafe(cp), "U+%04X should be escaped", cp);
    }
    T_CHECK(!atlas_codepoint_is_unsafe(0x20));
    T_CHECK(!atlas_codepoint_is_unsafe('a'));
    T_CHECK(!atlas_codepoint_is_unsafe(0xa0));
    T_CHECK(!atlas_codepoint_is_unsafe(0x2764)); /* a heart is not a control */
    T_CHECK(atlas_codepoint_is_unsafe(0x2028));
    T_CHECK(atlas_codepoint_is_unsafe(0x202e));
    T_CHECK(atlas_codepoint_is_unsafe(0x2066));
}

static void test_is_safe_predicate(void) {
    T_CHECK(atlas_text_is_safe("plain", 5u));
    T_CHECK(!atlas_text_is_safe("esc\x1b", 4u));
    T_CHECK(!atlas_text_is_safe("pct%", 4u));
    T_CHECK(!atlas_text_is_safe("bad\xff", 4u));
    T_CHECK(!atlas_text_is_safe("\xc2\x9b", 2u));
    T_CHECK(atlas_text_is_safe("", 0));
}

static void test_encoding_is_idempotent_on_safe_input(void) {
    /* Encoding output that contains no '%' must be a fixed point, which is what
     * lets already-encoded values be printed without double-escaping. */
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf once = ATLAS_BUF_INIT;
    atlas_buf twice = ATLAS_BUF_INIT;
    const char *input = "src/core/buf.c and a subject";
    T_OK(atlas_text_encode_safe(input, strlen(input), &once, &err), &err);
    T_OK(atlas_text_encode_safe(once.data, once.len, &twice, &err), &err);
    T_EQ_STR(atlas_buf_cstr(&twice), atlas_buf_cstr(&once));
    atlas_buf_free(&once);
    atlas_buf_free(&twice);
}

static void test_decode_rejects_malformed(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf out = ATLAS_BUF_INIT;
    T_FAILS_WITH(atlas_text_decode_safe("abc%", 4u, &out, &err), ATLAS_ERR_USAGE, &err);
    atlas_buf_reset(&out);
    T_FAILS_WITH(atlas_text_decode_safe("abc%z1", 6u, &out, &err), ATLAS_ERR_USAGE, &err);
    atlas_buf_reset(&out);
    T_FAILS_WITH(atlas_text_decode_safe("%4", 2u, &out, &err), ATLAS_ERR_USAGE, &err);
    atlas_buf_free(&out);
}

static void test_pool(void) {
    atlas_safe_pool p;
    atlas_safe_pool_init(&p);

    /* Several values usable at once, which is what a printf call needs. */
    const char *a = atlas_safe(&p, "\x1b[31ma");
    const char *b = atlas_safe(&p, "b\x07");
    const char *c = atlas_safe(&p, "plain");
    T_EQ_STR(a, "%1B[31ma");
    T_EQ_STR(b, "b%07");
    T_EQ_STR(c, "plain");

    /* NULL is rendered as empty rather than crashing a format call. */
    T_EQ_STR(atlas_safe(&p, NULL), "");

    /* Byte-oriented entry point. */
    T_EQ_STR(atlas_safe_n(&p, "x\x00y", 3u), "x%00y");

    /* Reuse beyond the slot count must not corrupt anything still in flight. */
    for (int i = 0; i < 100; i++) {
        (void)atlas_safe(&p, "churn\x1b");
    }
    T_EQ_STR(atlas_safe(&p, "last\x1b"), "last%1B");
    atlas_safe_pool_free(&p);
}

static void test_utf8_decoder(void) {
    uint32_t cp = 0;
    T_EQ_INT(atlas_utf8_decode((const unsigned char *)"a", 1u, &cp), 1);
    T_EQ_INT(cp, 'a');
    T_EQ_INT(atlas_utf8_decode((const unsigned char *)"\xc3\xa9", 2u, &cp), 2);
    T_EQ_INT(cp, 0xe9);
    T_EQ_INT(atlas_utf8_decode((const unsigned char *)"\xe2\x82\xac", 3u, &cp), 3);
    T_EQ_INT(cp, 0x20ac);
    T_EQ_INT(atlas_utf8_decode((const unsigned char *)"\xf0\x9f\x98\x80", 4u, &cp), 4);
    T_EQ_INT(cp, 0x1f600);
    /* Every ill-formed case reports zero rather than a plausible code point. */
    T_EQ_INT(atlas_utf8_decode((const unsigned char *)"\xff", 1u, &cp), 0);
    T_EQ_INT(atlas_utf8_decode((const unsigned char *)"\xc3", 1u, &cp), 0);
    T_EQ_INT(atlas_utf8_decode((const unsigned char *)"\xc3\x28", 2u, &cp), 0);
    T_EQ_INT(atlas_utf8_decode((const unsigned char *)"\xc0\xaf", 2u, &cp), 0);
    T_EQ_INT(atlas_utf8_decode((const unsigned char *)"\xed\xa0\x80", 3u, &cp), 0);
    T_EQ_INT(atlas_utf8_decode((const unsigned char *)"", 0, &cp), 0);
}

static const atlas_test TESTS[] = {
    {"plain text is untouched", test_plain_text_is_untouched},
    {"C0 controls are escaped", test_c0_controls},
    {"terminal control payloads are escaped", test_terminal_payloads},
    {"bidi controls and separators are escaped", test_bidi_and_separators},
    {"invalid UTF-8 is escaped byte by byte", test_invalid_utf8},
    {"the escape policy itself", test_policy_predicate},
    {"is-safe predicate", test_is_safe_predicate},
    {"encoding safe input is a fixed point", test_encoding_is_idempotent_on_safe_input},
    {"decoding rejects malformed escapes", test_decode_rejects_malformed},
    {"scratch pool", test_pool},
    {"utf-8 decoder", test_utf8_decoder},
};

ATLAS_TEST_MAIN("safetext", TESTS)
