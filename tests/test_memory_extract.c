/* Atlas - A12.1 T7: the deterministic extractor, its pure half.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * `atlas_memory_extract` is pure -- no database, no process, no clock, no
 * file -- so this suite drives it directly against byte buffers built in
 * process, exactly `tests/test_memory_source.c`'s reason for being "unit"
 * rather than "integration". The anchor-resolution half needs a fixture
 * index and lives in `tests/test_memory_anchor.c` instead.
 */
#include <stdio.h>
#include <string.h>

#include "atlas/buf.h"
#include "atlas/error.h"
#include "atlas/limits.h"
#include "atlas/memory.h"
#include "atlas_test.h"

static void free_all(atlas_memory_proposition *props, size_t n) {
    for (size_t i = 0; i < n; i++) {
        atlas_memory_proposition_free(&props[i]);
    }
}

/* --- the split -------------------------------------------------------------- */

static void test_three_bullets_and_two_paragraphs_make_five_candidates(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf src = ATLAS_BUF_INIT;
    T_OK(atlas_buf_set_str(&src,
                           "Paragraph one line.\n"
                           "\n"
                           "- bullet one\n"
                           "- bullet two\n"
                           "\n"
                           "Paragraph two line one.\n"
                           "Paragraph two line two.\n"
                           "\n"
                           "- bullet three\n",
                           &err),
         &err);

    atlas_memory_proposition props[16];
    size_t count = 0;
    bool bound_reached = true;
    T_OK(atlas_memory_extract(&src, props, 16, &count, &bound_reached, &err), &err);

    T_REQUIRE(count == 5);
    T_CHECK(!bound_reached);
    for (size_t i = 0; i < 5; i++) {
        T_CHECK_MSG(props[i].ordinal == i, "candidate %zu carries ordinal %zu", i,
                    props[i].ordinal);
    }
    T_EQ_STR(atlas_buf_cstr(&props[0].text), "Paragraph one line.");
    T_EQ_STR(atlas_buf_cstr(&props[1].text), "- bullet one");
    T_EQ_STR(atlas_buf_cstr(&props[2].text), "- bullet two");
    T_EQ_STR(atlas_buf_cstr(&props[3].text), "Paragraph two line one.\nParagraph two line two.");
    T_EQ_STR(atlas_buf_cstr(&props[4].text), "- bullet three");

    /* Every proposition already carries anchor_count == 0 and the DESCRIPTIVE
     * / NONE defaults: resolve() has not run. */
    for (size_t i = 0; i < 5; i++) {
        T_CHECK(props[i].anchor_count == 0);
    }

    free_all(props, count);
    atlas_buf_free(&src);
}

static void test_the_129th_candidate_sets_bound_reached(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf src = ATLAS_BUF_INIT;
    for (int i = 0; i < 129; i++) {
        char line[64];
        (void)snprintf(line, sizeof line, "paragraph number %d\n\n", i);
        T_OK(atlas_buf_append_str(&src, line, &err), &err);
    }

    /* A buffer larger than the policy ceiling on purpose -- the ceiling is
     * enforced by the function itself, not merely accommodated by the
     * caller's array size. */
    atlas_memory_proposition props[200];
    size_t count = 0;
    bool bound_reached = false;
    T_OK(atlas_memory_extract(&src, props, 200, &count, &bound_reached, &err), &err);

    T_EQ_INT((int)count, (int)ATLAS_MEMORY_MAX_PROPOSITIONS);
    T_CHECK_MSG(bound_reached, "129 candidates over a %u ceiling did not report the bound",
                ATLAS_MEMORY_MAX_PROPOSITIONS);
    T_CHECK(props[0].ordinal == 0);
    T_CHECK(props[ATLAS_MEMORY_MAX_PROPOSITIONS - 1].ordinal == ATLAS_MEMORY_MAX_PROPOSITIONS - 1);

    free_all(props, count);
    atlas_buf_free(&src);
}

static void test_a_smaller_caller_buffer_is_honoured_too(void) {
    /* The bound is min(cap, ATLAS_MEMORY_MAX_PROPOSITIONS): a caller that
     * hands over fewer slots than the policy ceiling still gets a refusal
     * rather than an overrun. */
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf src = ATLAS_BUF_INIT;
    T_OK(atlas_buf_set_str(&src, "one\n\ntwo\n\nthree\n", &err), &err);

    atlas_memory_proposition props[2];
    size_t count = 0;
    bool bound_reached = false;
    T_OK(atlas_memory_extract(&src, props, 2, &count, &bound_reached, &err), &err);
    T_EQ_INT((int)count, 2);
    T_CHECK(bound_reached);

    free_all(props, count);
    atlas_buf_free(&src);
}

static void test_an_over_long_candidate_sets_truncated(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf src = ATLAS_BUF_INIT;
    /* One paragraph, well over ATLAS_MEMORY_MAX_PROPOSITION_BYTES, made of a
     * repeated word rather than a single run so it stays one text/prose line
     * rather than accidentally matching a list-item shape. */
    size_t target = (size_t)ATLAS_MEMORY_MAX_PROPOSITION_BYTES + 500u;
    while (src.len < target) {
        T_OK(atlas_buf_append_str(&src, "assertion ", &err), &err);
    }

    atlas_memory_proposition props[4];
    size_t count = 0;
    bool bound_reached = true;
    T_OK(atlas_memory_extract(&src, props, 4, &count, &bound_reached, &err), &err);

    T_REQUIRE(count == 1);
    T_CHECK(!bound_reached);
    T_CHECK_MSG(props[0].truncated, "a %zu-byte candidate over the %u-byte ceiling was not flagged",
                props[0].text.len, ATLAS_MEMORY_MAX_PROPOSITION_BYTES);
    /* Never trimmed: every byte is still there. */
    T_EQ_INT((int)props[0].text.len, (int)src.len);

    free_all(props, count);
    atlas_buf_free(&src);
}

static void test_crlf_splits_identically_to_lf(void) {
    atlas_err err;
    atlas_err_init(&err);

    atlas_buf lf = ATLAS_BUF_INIT, crlf = ATLAS_BUF_INIT;
    T_OK(atlas_buf_set_str(&lf,
                           "the daemon reads it\n"
                           "\n"
                           "- one\n"
                           "- two\n"
                           "\n"
                           "a second line\n"
                           "joins the first\n",
                           &err),
         &err);
    T_OK(atlas_buf_set_str(&crlf,
                           "the daemon reads it\r\n"
                           "\r\n"
                           "- one\r\n"
                           "- two\r\n"
                           "\r\n"
                           "a second line\r\n"
                           "joins the first\r\n",
                           &err),
         &err);

    atlas_memory_proposition lf_props[8], crlf_props[8];
    size_t lf_count = 0, crlf_count = 0;
    bool bound = false;
    T_OK(atlas_memory_extract(&lf, lf_props, 8, &lf_count, &bound, &err), &err);
    T_OK(atlas_memory_extract(&crlf, crlf_props, 8, &crlf_count, &bound, &err), &err);

    T_REQUIRE(lf_count == crlf_count);
    for (size_t i = 0; i < lf_count; i++) {
        T_CHECK(lf_props[i].ordinal == crlf_props[i].ordinal);
        T_EQ_MEM(crlf_props[i].text.data, crlf_props[i].text.len, lf_props[i].text.data,
                 lf_props[i].text.len);
        T_EQ_MEM(crlf_props[i].normalized.data, crlf_props[i].normalized.len,
                 lf_props[i].normalized.data, lf_props[i].normalized.len);
    }

    free_all(lf_props, lf_count);
    free_all(crlf_props, crlf_count);
    atlas_buf_free(&lf);
    atlas_buf_free(&crlf);
}

static void test_invalid_utf8_survives_verbatim_in_text(void) {
    atlas_err err;
    atlas_err_init(&err);
    /* A lone continuation byte (0x80) is not valid UTF-8 anywhere on its own.
     * Built with atlas_buf_set rather than a C string literal so the byte's
     * value is exact and unambiguous. */
    const unsigned char raw[] = {'a', 's', 's', 'e', 'r', 't', 0x80, 'i', 'o', 'n'};
    atlas_buf src = ATLAS_BUF_INIT;
    T_OK(atlas_buf_set(&src, raw, sizeof raw, &err), &err);

    atlas_memory_proposition props[2];
    size_t count = 0;
    bool bound_reached = false;
    T_OK(atlas_memory_extract(&src, props, 2, &count, &bound_reached, &err), &err);

    T_REQUIRE(count == 1);
    T_EQ_MEM(props[0].text.data, props[0].text.len, raw, sizeof raw);

    free_all(props, count);
    atlas_buf_free(&src);
}

/* --- normalisation ----------------------------------------------------------- */

static void normalize_one(const char *line, atlas_buf *normalized_out, atlas_err *err) {
    atlas_buf src = ATLAS_BUF_INIT;
    T_OK(atlas_buf_set_str(&src, line, err), err);
    atlas_memory_proposition props[1];
    size_t count = 0;
    bool bound_reached = false;
    T_OK(atlas_memory_extract(&src, props, 1, &count, &bound_reached, err), err);
    T_REQUIRE(count == 1);
    T_OK(atlas_buf_set(normalized_out, props[0].normalized.data, props[0].normalized.len, err),
         err);
    free_all(props, count);
    atlas_buf_free(&src);
}

static void test_marker_and_emphasis_normalise_byte_identically(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf a = ATLAS_BUF_INIT, b = ATLAS_BUF_INIT;
    normalize_one("- The **daemon** reads   X", &a, &err);
    normalize_one("* the daemon reads X", &b, &err);
    T_EQ_MEM(a.data, a.len, b.data, b.len);
    T_EQ_STR(atlas_buf_cstr(&a), "the daemon reads x");
    atlas_buf_free(&a);
    atlas_buf_free(&b);
}

static void test_numbered_marker_normalises_like_a_bullet(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf a = ATLAS_BUF_INIT, b = ATLAS_BUF_INIT;
    normalize_one("1. The daemon reads X", &a, &err);
    normalize_one("- the daemon reads X", &b, &err);
    T_EQ_MEM(a.data, a.len, b.data, b.len);
    atlas_buf_free(&a);
    atlas_buf_free(&b);
}

static void test_an_identifier_underscore_is_not_emphasis(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf a = ATLAS_BUF_INIT;
    normalize_one("- calls foo_bar directly", &a, &err);
    T_EQ_STR(atlas_buf_cstr(&a), "calls foo_bar directly");
    atlas_buf_free(&a);
}

static void test_a_non_ascii_byte_keeps_its_value(void) {
    atlas_err err;
    atlas_err_init(&err);
    /* "- CafÉ" -- 'É' is U+00C9, 0xC3 0x89 in UTF-8. Neither byte is in the
     * ASCII range, so normalisation must not touch either of them even
     * though the rest of the line is lowercased. */
    const unsigned char raw[] = {'-', ' ', 'C', 'a', 'f', 0xC3, 0x89, '\0'};
    atlas_buf src = ATLAS_BUF_INIT;
    T_OK(atlas_buf_set(&src, raw, sizeof raw - 1u, &err), &err);

    atlas_memory_proposition props[1];
    size_t count = 0;
    bool bound_reached = false;
    T_OK(atlas_memory_extract(&src, props, 1, &count, &bound_reached, &err), &err);
    T_REQUIRE(count == 1);

    /* "caf" lowercased, then the two untouched bytes of 'É'. */
    const unsigned char want[] = {'c', 'a', 'f', 0xC3, 0x89};
    T_EQ_MEM(props[0].normalized.data, props[0].normalized.len, want, sizeof want);

    free_all(props, count);
    atlas_buf_free(&src);
}

/* --- argument refusals -------------------------------------------------------- */

static void test_a_zero_capacity_buffer_is_refused(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf src = ATLAS_BUF_INIT;
    T_OK(atlas_buf_set_str(&src, "text", &err), &err);
    atlas_memory_proposition props[1];
    size_t count = 1;
    bool bound_reached = true;
    atlas_status st = atlas_memory_extract(&src, props, 0, &count, &bound_reached, &err);
    T_EQ_INT((int)st, (int)ATLAS_ERR_INTERNAL);
    T_CHECK(count == 0);
    T_CHECK(!bound_reached);
    atlas_buf_free(&src);
}

static const atlas_test TESTS[] = {
    {"three bullets and two paragraphs make five candidates",
     test_three_bullets_and_two_paragraphs_make_five_candidates},
    {"the 129th candidate sets bound_reached", test_the_129th_candidate_sets_bound_reached},
    {"a smaller caller buffer is honoured too", test_a_smaller_caller_buffer_is_honoured_too},
    {"an over-long candidate sets truncated, never trimmed",
     test_an_over_long_candidate_sets_truncated},
    {"CRLF input splits identically to LF", test_crlf_splits_identically_to_lf},
    {"invalid UTF-8 survives verbatim in text", test_invalid_utf8_survives_verbatim_in_text},
    {"a list marker and emphasis normalise byte-identically",
     test_marker_and_emphasis_normalise_byte_identically},
    {"a numbered marker normalises like a bullet marker",
     test_numbered_marker_normalises_like_a_bullet},
    {"an identifier's underscore is not emphasis", test_an_identifier_underscore_is_not_emphasis},
    {"a non-ASCII byte keeps its value", test_a_non_ascii_byte_keeps_its_value},
    {"a zero-capacity buffer is refused", test_a_zero_capacity_buffer_is_refused},
};

ATLAS_TEST_MAIN("memory_extract", TESTS)
