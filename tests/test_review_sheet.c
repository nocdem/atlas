/* Atlas - A15 T3: the review sheet grammar.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Pure-function tests, `tests/test_memory_source.c`'s shape for the same
 * reason: `atlas_review_sheet_parse` opens no database, creates no process
 * and reads no repository, so every case here is bytes in, a parsed sheet or
 * a refusal out.
 *
 * Every refusal case checks the exact message text (not merely the status),
 * because "the parser refused" is true for the wrong reason too -- a
 * six-field line and a malformed decision id both make `atlas_status !=
 * ATLAS_OK`, and only the message says which grammar rule actually fired.
 * Several cases also assert `sheet.count == 0` after a refusal, and one
 * poisons the sheet with a non-zero pattern before calling: the parser's own
 * doc comment promises an all-refusals-produce-an-empty-sheet property, and a
 * test that only checked the status would pass even if that promise were
 * broken.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/decision.h"
#include "atlas/error.h"
#include "atlas/limits.h"
#include "atlas/review.h"
#include "atlas_test.h"

/* --- building sheets -------------------------------------------------------
 *
 * `%08x%024x` on (n, 0) always produces exactly 32 lowercase hex characters
 * (8 from `n`, 24 zero-padded), so a decision id built this way is always the
 * right shape and varying `n` gives distinct ids without hand-counting hex
 * digits in a string literal. */

static void append(char *buf, size_t cap, size_t *used, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static void append(char *buf, size_t cap, size_t *used, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *used, cap - *used, fmt, ap);
    va_end(ap);
    T_REQUIRE_MSG(n > 0 && (size_t)n < cap - *used, "test buffer too small");
    *used += (size_t)n;
}

/* --- the example sheet ------------------------------------------------------
 *
 * Copied verbatim from the "Frozen formats" section of
 * docs/plans/2026-09-03-review-surface.md. */

static const char EXAMPLE_SHEET[] =
    "atlas-review-sheet/1\n"
    "# lines beginning with # are ignored; blank lines are ignored\n"
    "approve atlas atlas-dec-28f03b0a44a53db88f0deace6e79721b r1 6fb2be08\n"
    "reject  atlas atlas-dec-c711a6d9c4954961a5e9d18240591d8e r3 5146bbb3\n"
    "resolve atlas atlas-dec-314ed60fe9bd11400646934658843bf3 r2 89d53ae3\n";

static void test_example_sheet_parses(void) {
    atlas_review_sheet sheet;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_review_sheet_parse(EXAMPLE_SHEET, sizeof(EXAMPLE_SHEET) - 1u, &sheet, &err), &err);
    T_EQ_INT(sheet.count, 3);

    T_EQ_INT(sheet.entries[0].line, 3);
    T_EQ_INT(sheet.entries[0].intent, ATLAS_DECISION_INTENT_APPROVE);
    T_EQ_STR(sheet.entries[0].repo, "atlas");
    T_EQ_STR(sheet.entries[0].decision, "atlas-dec-28f03b0a44a53db88f0deace6e79721b");
    T_EQ_INT(sheet.entries[0].revision_no, 1);
    T_EQ_STR(sheet.entries[0].prefix, "6fb2be08");

    T_EQ_INT(sheet.entries[1].line, 4);
    T_EQ_INT(sheet.entries[1].intent, ATLAS_DECISION_INTENT_REJECT);
    T_EQ_STR(sheet.entries[1].repo, "atlas");
    T_EQ_STR(sheet.entries[1].decision, "atlas-dec-c711a6d9c4954961a5e9d18240591d8e");
    T_EQ_INT(sheet.entries[1].revision_no, 3);
    T_EQ_STR(sheet.entries[1].prefix, "5146bbb3");

    T_EQ_INT(sheet.entries[2].line, 5);
    T_EQ_INT(sheet.entries[2].intent, ATLAS_DECISION_INTENT_RESOLVE);
    T_EQ_STR(sheet.entries[2].repo, "atlas");
    T_EQ_STR(sheet.entries[2].decision, "atlas-dec-314ed60fe9bd11400646934658843bf3");
    T_EQ_INT(sheet.entries[2].revision_no, 2);
    T_EQ_STR(sheet.entries[2].prefix, "89d53ae3");
}

/* --- the header -------------------------------------------------------------
 *
 * Both the wrong first substantive line and a completely empty file are "no
 * header was ever found", and both produce the same sentence. */

static void test_missing_header_refused(void) {
    static const char SHEET[] = "approve atlas atlas-dec-28f03b0a44a53db88f0deace6e79721b r1 6fb2be08\n";
    atlas_review_sheet sheet;
    atlas_err err;
    atlas_err_init(&err);
    T_FAILS_WITH(atlas_review_sheet_parse(SHEET, sizeof(SHEET) - 1u, &sheet, &err), ATLAS_ERR_USAGE,
                &err);
    T_EQ_STR(atlas_err_msg(&err),
            "this is not an Atlas review sheet: the first line must be atlas-review-sheet/1");
    T_EQ_INT(sheet.count, 0);

    memset(&sheet, 0xAA, sizeof sheet);
    atlas_err_init(&err);
    T_FAILS_WITH(atlas_review_sheet_parse("", 0, &sheet, &err), ATLAS_ERR_USAGE, &err);
    T_EQ_STR(atlas_err_msg(&err),
            "this is not an Atlas review sheet: the first line must be atlas-review-sheet/1");
    T_EQ_INT(sheet.count, 0);
}

/* --- field count ------------------------------------------------------------ */

static void test_field_count_refused(void) {
    char buf[512];
    size_t used = 0;
    atlas_review_sheet sheet;
    atlas_err err;

    used = 0;
    append(buf, sizeof buf, &used, "atlas-review-sheet/1\n");
    append(buf, sizeof buf, &used,
          "approve atlas atlas-dec-28f03b0a44a53db88f0deace6e79721b r1\n");
    atlas_err_init(&err);
    T_FAILS_WITH(atlas_review_sheet_parse(buf, used, &sheet, &err), ATLAS_ERR_USAGE, &err);
    T_EQ_STR(atlas_err_msg(&err),
            "review sheet line 2: expected 5 fields (intent repository decision rN prefix), "
            "found 4");
    T_EQ_INT(sheet.count, 0);

    /* Six fields -- a line carrying a would-be confirmation. This is the
     * sheet's mirror of the rule that no MCP tool schema declares a
     * "confirmation": property: a sheet has exactly five fields and a sixth
     * one is refused rather than read as an early confirmation. */
    used = 0;
    append(buf, sizeof buf, &used, "atlas-review-sheet/1\n");
    append(buf, sizeof buf, &used,
          "approve atlas atlas-dec-28f03b0a44a53db88f0deace6e79721b r1 6fb2be08 6fb2be08\n");
    atlas_err_init(&err);
    T_FAILS_WITH(atlas_review_sheet_parse(buf, used, &sheet, &err), ATLAS_ERR_USAGE, &err);
    T_EQ_STR(atlas_err_msg(&err),
            "review sheet line 2: expected 5 fields (intent repository decision rN prefix), "
            "found 6");
    T_EQ_INT(sheet.count, 0);
}

/* --- intent ------------------------------------------------------------------ */

static void test_supersede_intent_refused(void) {
    char buf[512];
    size_t used = 0;
    append(buf, sizeof buf, &used, "atlas-review-sheet/1\n");
    append(buf, sizeof buf, &used,
          "supersede atlas atlas-dec-28f03b0a44a53db88f0deace6e79721b r1 6fb2be08\n");

    atlas_review_sheet sheet;
    atlas_err err;
    atlas_err_init(&err);
    T_FAILS_WITH(atlas_review_sheet_parse(buf, used, &sheet, &err), ATLAS_ERR_USAGE, &err);
    T_EQ_STR(atlas_err_msg(&err),
            "review sheet line 2: \"supersede\" is not an intent a sheet may carry (approve, "
            "reject or resolve)");
    T_EQ_INT(sheet.count, 0);

    /* REVALIDATE parses as a real intent and is disallowed the same way --
     * proving the check is atlas_review_intent_allowed() and not merely "did
     * atlas_decision_intent_parse fail". */
    used = 0;
    append(buf, sizeof buf, &used, "atlas-review-sheet/1\n");
    append(buf, sizeof buf, &used,
          "revalidate atlas atlas-dec-28f03b0a44a53db88f0deace6e79721b r1 6fb2be08\n");
    atlas_err_init(&err);
    T_FAILS_WITH(atlas_review_sheet_parse(buf, used, &sheet, &err), ATLAS_ERR_USAGE, &err);
    T_EQ_STR(atlas_err_msg(&err),
            "review sheet line 2: \"revalidate\" is not an intent a sheet may carry (approve, "
            "reject or resolve)");
}

static void test_intent_message_truncated_and_encoded(void) {
    char buf[512];
    size_t used = 0;
    append(buf, sizeof buf, &used, "atlas-review-sheet/1\n");
    /* 40 bytes of 'x': the message quotes only the first 32. */
    char intent[41];
    memset(intent, 'x', 40);
    intent[40] = '\0';
    append(buf, sizeof buf, &used,
          "%s atlas atlas-dec-28f03b0a44a53db88f0deace6e79721b r1 6fb2be08\n", intent);

    atlas_review_sheet sheet;
    atlas_err err;
    atlas_err_init(&err);
    T_FAILS_WITH(atlas_review_sheet_parse(buf, used, &sheet, &err), ATLAS_ERR_USAGE, &err);
    char thirty_two[33];
    memset(thirty_two, 'x', 32);
    thirty_two[32] = '\0';
    char expected[160];
    (void)snprintf(expected, sizeof expected,
                  "review sheet line 2: \"%s\" is not an intent a sheet may carry (approve, "
                  "reject or resolve)",
                  thirty_two);
    T_EQ_STR(atlas_err_msg(&err), expected);

    /* A literal '%' in the offending field comes back safe-encoded, proving
     * this goes through atlas_text_encode_safe() rather than a raw %s of
     * untrusted bytes. */
    used = 0;
    append(buf, sizeof buf, &used, "atlas-review-sheet/1\n");
    append(buf, sizeof buf, &used,
          "%%bad atlas atlas-dec-28f03b0a44a53db88f0deace6e79721b r1 6fb2be08\n");
    atlas_err_init(&err);
    T_FAILS_WITH(atlas_review_sheet_parse(buf, used, &sheet, &err), ATLAS_ERR_USAGE, &err);
    T_EQ_STR(atlas_err_msg(&err),
            "review sheet line 2: \"%25bad\" is not an intent a sheet may carry (approve, "
            "reject or resolve)");
}

/* --- decision id -------------------------------------------------------------- */

static void test_decision_uid_31_hex_refused(void) {
    char buf[512];
    size_t used = 0;
    append(buf, sizeof buf, &used, "atlas-review-sheet/1\n");
    /* 31 hex characters: one short of ATLAS_DECISION_UID_HEX. */
    append(buf, sizeof buf, &used, "approve atlas atlas-dec-%031x r1 6fb2be08\n", 0u);

    atlas_review_sheet sheet;
    atlas_err err;
    atlas_err_init(&err);
    T_FAILS_WITH(atlas_review_sheet_parse(buf, used, &sheet, &err), ATLAS_ERR_USAGE, &err);
    T_EQ_STR(atlas_err_msg(&err), "review sheet line 2: the decision id is malformed");
    T_EQ_INT(sheet.count, 0);
}

/* --- revision ------------------------------------------------------------------ */

static void test_revision_shape_refused(void) {
    static const char *BAD[] = {"r0", "r01", "1"};
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        char buf[512];
        size_t used = 0;
        append(buf, sizeof buf, &used, "atlas-review-sheet/1\n");
        append(buf, sizeof buf, &used,
              "approve atlas atlas-dec-28f03b0a44a53db88f0deace6e79721b %s 6fb2be08\n", BAD[i]);

        atlas_review_sheet sheet;
        atlas_err err;
        atlas_err_init(&err);
        T_FAILS_WITH(atlas_review_sheet_parse(buf, used, &sheet, &err), ATLAS_ERR_USAGE, &err);
        T_EQ_STR(atlas_err_msg(&err),
                "review sheet line 2: the revision must be r followed by a positive number");
        T_EQ_INT(sheet.count, 0);
    }
}

/* --- confirmation prefix -------------------------------------------------------- */

static void test_prefix_shape_refused(void) {
    static const char *BAD[] = {"1234567", "123456789", "ABCDEF12"};
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        char buf[512];
        size_t used = 0;
        append(buf, sizeof buf, &used, "atlas-review-sheet/1\n");
        append(buf, sizeof buf, &used,
              "approve atlas atlas-dec-28f03b0a44a53db88f0deace6e79721b r1 %s\n", BAD[i]);

        atlas_review_sheet sheet;
        atlas_err err;
        atlas_err_init(&err);
        T_FAILS_WITH(atlas_review_sheet_parse(buf, used, &sheet, &err), ATLAS_ERR_USAGE, &err);
        T_EQ_STR(atlas_err_msg(&err),
                "review sheet line 2: the confirmation prefix must be exactly 8 lowercase hex "
                "characters");
        T_EQ_INT(sheet.count, 0);
    }
}

/* --- duplicates ------------------------------------------------------------------ */

static void test_duplicate_pair_refused(void) {
    char buf[512];
    size_t used = 0;
    append(buf, sizeof buf, &used, "atlas-review-sheet/1\n");
    append(buf, sizeof buf, &used,
          "approve atlas atlas-dec-28f03b0a44a53db88f0deace6e79721b r1 6fb2be08\n");
    append(buf, sizeof buf, &used,
          "reject atlas atlas-dec-28f03b0a44a53db88f0deace6e79721b r2 5146bbb3\n");

    atlas_review_sheet sheet;
    atlas_err err;
    atlas_err_init(&err);
    T_FAILS_WITH(atlas_review_sheet_parse(buf, used, &sheet, &err), ATLAS_ERR_USAGE, &err);
    T_EQ_STR(atlas_err_msg(&err),
            "review sheet line 3: atlas-dec-28f03b0a44a53db88f0deace6e79721b was already named on "
            "line 2; a sheet disposes of a record once");
    T_EQ_INT(sheet.count, 0);
}

/* --- entry count bound ------------------------------------------------------------ */

static void test_64_entries_parse_65_refused(void) {
    /* Boundary pair, not just the over-the-line case: 64 must parse and land
     * every one of them, and only the 65th is refused. Distinct decision ids
     * per entry so the duplicate check never fires here. */
    char *buf = malloc(32768);
    T_REQUIRE(buf != NULL);
    size_t used = 0;
    append(buf, 32768, &used, "atlas-review-sheet/1\n");
    for (unsigned i = 0; i < ATLAS_REVIEW_SHEET_MAX_ENTRIES; i++) {
        append(buf, 32768, &used, "approve atlas atlas-dec-%08x%024x r1 6fb2be08\n", i, 0u);
    }

    atlas_review_sheet sheet;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_review_sheet_parse(buf, used, &sheet, &err), &err);
    T_EQ_INT(sheet.count, ATLAS_REVIEW_SHEET_MAX_ENTRIES);
    T_EQ_INT(sheet.entries[ATLAS_REVIEW_SHEET_MAX_ENTRIES - 1u].line,
            ATLAS_REVIEW_SHEET_MAX_ENTRIES + 1u);

    append(buf, 32768, &used, "approve atlas atlas-dec-%08x%024x r1 6fb2be08\n",
          (unsigned)ATLAS_REVIEW_SHEET_MAX_ENTRIES, 0u);
    memset(&sheet, 0xAA, sizeof sheet);
    atlas_err_init(&err);
    T_FAILS_WITH(atlas_review_sheet_parse(buf, used, &sheet, &err), ATLAS_ERR_USAGE, &err);
    char expected[96];
    (void)snprintf(expected, sizeof expected, "review sheet: more than %u entries; split it",
                  (unsigned)ATLAS_REVIEW_SHEET_MAX_ENTRIES);
    T_EQ_STR(atlas_err_msg(&err), expected);
    T_EQ_INT(sheet.count, 0);

    free(buf);
}

/* --- line length bound -------------------------------------------------------------- */

static void test_line_length_bound(void) {
    static const char *ENTRY =
        "approve atlas atlas-dec-28f03b0a44a53db88f0deace6e79721b r1 6fb2be08";
    size_t entry_len = strlen(ENTRY);
    T_REQUIRE(entry_len < ATLAS_REVIEW_SHEET_MAX_LINE);

    char buf[1024];

    /* Exactly the bound, padded with trailing spaces (which add no extra
     * field): parses. */
    size_t used = 0;
    append(buf, sizeof buf, &used, "atlas-review-sheet/1\n%s", ENTRY);
    size_t pad = ATLAS_REVIEW_SHEET_MAX_LINE - entry_len;
    memset(buf + used, ' ', pad);
    used += pad;
    buf[used++] = '\n';

    atlas_review_sheet sheet;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_review_sheet_parse(buf, used, &sheet, &err), &err);
    T_EQ_INT(sheet.count, 1);

    /* One byte more: refused. Not one of the "Frozen formats" ten -- see
     * src/core/review.c's own header comment for why this exact wording was
     * chosen and disclosed rather than folded into one of the ten. */
    used = 0;
    append(buf, sizeof buf, &used, "atlas-review-sheet/1\n%s", ENTRY);
    pad = ATLAS_REVIEW_SHEET_MAX_LINE - entry_len + 1u;
    memset(buf + used, ' ', pad);
    used += pad;
    buf[used++] = '\n';

    atlas_err_init(&err);
    T_FAILS_WITH(atlas_review_sheet_parse(buf, used, &sheet, &err), ATLAS_ERR_USAGE, &err);
    char expected[96];
    (void)snprintf(expected, sizeof expected, "review sheet line 2: longer than %u bytes",
                  (unsigned)ATLAS_REVIEW_SHEET_MAX_LINE);
    T_EQ_STR(atlas_err_msg(&err), expected);
    T_EQ_INT(sheet.count, 0);
}

/* --- whole-sheet byte bound ---------------------------------------------------------- */

static void test_sheet_larger_than_max_bytes_refused(void) {
    size_t n = (size_t)ATLAS_REVIEW_SHEET_MAX_BYTES + 1u;
    char *buf = malloc(n);
    T_REQUIRE(buf != NULL);
    memset(buf, 'x', n); /* content is irrelevant: the length check runs first */

    atlas_review_sheet sheet;
    atlas_err err;
    atlas_err_init(&err);
    T_FAILS_WITH(atlas_review_sheet_parse(buf, n, &sheet, &err), ATLAS_ERR_USAGE, &err);
    char expected[96];
    (void)snprintf(expected, sizeof expected, "review sheet: larger than %u bytes",
                  (unsigned)ATLAS_REVIEW_SHEET_MAX_BYTES);
    T_EQ_STR(atlas_err_msg(&err), expected);
    T_EQ_INT(sheet.count, 0);

    free(buf);
}

/* --- illegal bytes --------------------------------------------------------------------- */

static void test_byte_0x80_refused(void) {
    char buf[256];
    size_t used = 0;
    append(buf, sizeof buf, &used, "atlas-review-sheet/1\n");
    static const char *ENTRY =
        "approve atlas atlas-dec-28f03b0a44a53db88f0deace6e79721b r1 6fb2be08";
    memcpy(buf + used, ENTRY, strlen(ENTRY));
    used += strlen(ENTRY);
    buf[used++] = (char)0x80;
    buf[used++] = '\n';

    atlas_review_sheet sheet;
    atlas_err err;
    atlas_err_init(&err);
    T_FAILS_WITH(atlas_review_sheet_parse(buf, used, &sheet, &err), ATLAS_ERR_USAGE, &err);
    T_EQ_STR(atlas_err_msg(&err),
            "review sheet line 2: a byte outside printable ASCII; a sheet carries identifiers, "
            "never prose");
    T_EQ_INT(sheet.count, 0);
}

static void test_stray_cr_not_before_nl_refused(void) {
    char buf[256];
    size_t used = 0;
    append(buf, sizeof buf, &used, "atlas-review-sheet/1\n");
    static const char *ENTRY =
        "approve atlas atlas-dec-28f03b0a44a53db88f0deace6e79721b r1 6fb2be08";
    memcpy(buf + used, ENTRY, strlen(ENTRY));
    used += strlen(ENTRY);
    /* A '\r' with a byte after it before the line's own '\n' -- not the CRLF
     * shape, so it is just another illegal byte. */
    buf[used++] = '\r';
    buf[used++] = 'x';
    buf[used++] = '\n';

    atlas_review_sheet sheet;
    atlas_err err;
    atlas_err_init(&err);
    T_FAILS_WITH(atlas_review_sheet_parse(buf, used, &sheet, &err), ATLAS_ERR_USAGE, &err);
    T_EQ_STR(atlas_err_msg(&err),
            "review sheet line 2: a byte outside printable ASCII; a sheet carries identifiers, "
            "never prose");
    T_EQ_INT(sheet.count, 0);
}

static void test_crlf_line_endings_parse(void) {
    static const char SHEET[] =
        "atlas-review-sheet/1\r\n"
        "# a CRLF comment\r\n"
        "approve atlas atlas-dec-28f03b0a44a53db88f0deace6e79721b r1 6fb2be08\r\n";
    atlas_review_sheet sheet;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_review_sheet_parse(SHEET, sizeof(SHEET) - 1u, &sheet, &err), &err);
    T_EQ_INT(sheet.count, 1);
    T_EQ_INT(sheet.entries[0].line, 3);
    T_EQ_STR(sheet.entries[0].repo, "atlas");
    T_EQ_STR(sheet.entries[0].prefix, "6fb2be08");
}

/* --- blank lines and comments -------------------------------------------------------------- */

static void test_blank_and_comment_lines_ignored(void) {
    /* Physical line numbers, not a count of substantive lines: gaps of blank
     * and comment lines sit between the two entries, and each entry's
     * recorded `line` must still be its real position in the file. */
    static const char SHEET[] =
        "atlas-review-sheet/1\n"  /* 1 */
        "\n"                      /* 2: blank */
        "# a comment\n"           /* 3: comment */
        "\n"                      /* 4: blank */
        "   \n"                   /* 5: whitespace-only, still blank */
        "approve atlas atlas-dec-28f03b0a44a53db88f0deace6e79721b r1 6fb2be08\n" /* 6 */
        "\n"                      /* 7: blank */
        "reject atlas atlas-dec-c711a6d9c4954961a5e9d18240591d8e r2 5146bbb3\n"; /* 8 */

    atlas_review_sheet sheet;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_review_sheet_parse(SHEET, sizeof(SHEET) - 1u, &sheet, &err), &err);
    T_EQ_INT(sheet.count, 2);
    T_EQ_INT(sheet.entries[0].line, 6);
    T_EQ_INT(sheet.entries[1].line, 8);
}

/* --- refuses the whole sheet, never a partial one ------------------------------------------ */

static void test_a_bad_line_refuses_the_whole_sheet(void) {
    char buf[512];
    size_t used = 0;
    append(buf, sizeof buf, &used, "atlas-review-sheet/1\n");
    append(buf, sizeof buf, &used,
          "approve atlas atlas-dec-28f03b0a44a53db88f0deace6e79721b r1 6fb2be08\n");
    append(buf, sizeof buf, &used,
          "reject atlas atlas-dec-c711a6d9c4954961a5e9d18240591d8e r2 5146bbb3\n");
    append(buf, sizeof buf, &used, "bogus atlas atlas-dec-314ed60fe9bd11400646934658843bf3 r2 89d53ae3\n");

    atlas_review_sheet sheet;
    memset(&sheet, 0xAA, sizeof sheet); /* poisoned, to prove the parser zeroes it */
    atlas_err err;
    atlas_err_init(&err);
    T_FAILS_WITH(atlas_review_sheet_parse(buf, used, &sheet, &err), ATLAS_ERR_USAGE, &err);
    T_EQ_INT(sheet.count, 0);
}

/* --- a repository name the registry would refuse -------------------------------------------
 *
 * Not one of the ten frozen sentences and not in the brief's enumerated test
 * list -- src/core/review.c's own header comment discloses why this wraps
 * atlas_db_check_repo_name's message behind a line number instead of
 * restating its grammar. Covered here so the call is exercised at all. */

static void test_bad_repo_name_wraps_the_db_check(void) {
    char buf[512];
    size_t used = 0;
    append(buf, sizeof buf, &used, "atlas-review-sheet/1\n");
    /* '!' is printable ASCII (so it passes the sheet's own byte check) and is
     * not in atlas_db_check_repo_name's allowed character set. */
    append(buf, sizeof buf, &used,
          "approve at!as atlas-dec-28f03b0a44a53db88f0deace6e79721b r1 6fb2be08\n");

    atlas_review_sheet sheet;
    atlas_err err;
    atlas_err_init(&err);
    T_FAILS_WITH(atlas_review_sheet_parse(buf, used, &sheet, &err), ATLAS_ERR_USAGE, &err);
    T_CHECK_MSG(strncmp(atlas_err_msg(&err), "review sheet line 2: ", 21) == 0,
               "expected a line-numbered wrapper, got: %s", atlas_err_msg(&err));
    T_EQ_INT(sheet.count, 0);
}

/* --- the verdict vocabulary ------------------------------------------------------------------ */

static void test_verdict_zero_is_unknown_and_refuses_to_parse(void) {
    T_EQ_STR(atlas_review_verdict_name(ATLAS_REVIEW_UNKNOWN), "UNKNOWN");
    atlas_review_verdict v = ATLAS_REVIEW_READY;
    T_CHECK(!atlas_review_verdict_parse("UNKNOWN", &v));
    /* A refused parse leaves the caller's variable alone. */
    T_EQ_INT(v, ATLAS_REVIEW_READY);
}

static void test_every_other_verdict_round_trips(void) {
    static const struct {
        atlas_review_verdict v;
        const char *name;
    } CASES[] = {
        {ATLAS_REVIEW_READY, "READY"},         {ATLAS_REVIEW_APPLIED, "APPLIED"},
        {ATLAS_REVIEW_ABANDONED, "ABANDONED"}, {ATLAS_REVIEW_MOVED, "MOVED"},
        {ATLAS_REVIEW_DISPOSED, "DISPOSED"},   {ATLAS_REVIEW_MISSING, "MISSING"},
        {ATLAS_REVIEW_REFUSED, "REFUSED"},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        T_EQ_STR(atlas_review_verdict_name(CASES[i].v), CASES[i].name);
        atlas_review_verdict got = ATLAS_REVIEW_UNKNOWN;
        T_CHECK_MSG(atlas_review_verdict_parse(CASES[i].name, &got), "\"%s\" did not parse",
                   CASES[i].name);
        T_EQ_INT(got, CASES[i].v);
    }
    atlas_review_verdict got = ATLAS_REVIEW_UNKNOWN;
    T_CHECK(!atlas_review_verdict_parse("ready", &got));
    T_CHECK(!atlas_review_verdict_parse(NULL, &got));
}

/* --- the intent predicate -------------------------------------------------------------------- */

static void test_intent_allowed_true_for_exactly_three(void) {
    static const struct {
        atlas_decision_intent i;
        bool allowed;
    } CASES[] = {
        {ATLAS_DECISION_INTENT_APPROVE, true}, {ATLAS_DECISION_INTENT_REJECT, true},
        {ATLAS_DECISION_INTENT_RESOLVE, true}, {ATLAS_DECISION_INTENT_SUPERSEDE, false},
        {ATLAS_DECISION_INTENT_REVALIDATE, false},
    };
    unsigned allowed_count = 0;
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        bool got = atlas_review_intent_allowed(CASES[i].i);
        T_CHECK_MSG(got == CASES[i].allowed, "intent %s: expected %s, got %s",
                   atlas_decision_intent_name(CASES[i].i), CASES[i].allowed ? "true" : "false",
                   got ? "true" : "false");
        if (got) {
            allowed_count++;
        }
    }
    T_EQ_INT(allowed_count, 3);
}

static const atlas_test TESTS[] = {
    {"the example sheet parses to three entries", test_example_sheet_parses},
    {"a missing header is refused", test_missing_header_refused},
    {"the wrong field count is refused", test_field_count_refused},
    {"supersede and revalidate are refused as sheet intents", test_supersede_intent_refused},
    {"the intent message is truncated to 32 bytes and safe-encoded",
     test_intent_message_truncated_and_encoded},
    {"a decision id with 31 hex characters is refused", test_decision_uid_31_hex_refused},
    {"r0, r01 and a bare 1 are all refused as revisions", test_revision_shape_refused},
    {"a 7-hex, 9-hex and uppercase prefix are all refused", test_prefix_shape_refused},
    {"a duplicate (repo, decision) pair is refused", test_duplicate_pair_refused},
    {"64 entries parse and the 65th is refused", test_64_entries_parse_65_refused},
    {"a 256-byte line parses and a 257-byte line is refused", test_line_length_bound},
    {"a sheet larger than the byte ceiling is refused", test_sheet_larger_than_max_bytes_refused},
    {"a byte outside printable ASCII is refused", test_byte_0x80_refused},
    {"a stray CR not immediately before LF is refused", test_stray_cr_not_before_nl_refused},
    {"CRLF line endings parse", test_crlf_line_endings_parse},
    {"blank lines and comments are ignored, and line numbers stay physical",
     test_blank_and_comment_lines_ignored},
    {"a bad line refuses the whole sheet, never a partial one",
     test_a_bad_line_refuses_the_whole_sheet},
    {"a repository name the registry would refuse wraps that check's own message",
     test_bad_repo_name_wraps_the_db_check},
    {"the verdict vocabulary's zero member is UNKNOWN and never parses",
     test_verdict_zero_is_unknown_and_refuses_to_parse},
    {"every other verdict round trips", test_every_other_verdict_round_trips},
    {"atlas_review_intent_allowed is true for exactly three intents",
     test_intent_allowed_true_for_exactly_three},
};

ATLAS_TEST_MAIN("review_sheet", TESTS)
