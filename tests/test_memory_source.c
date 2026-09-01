/* Atlas - A12.1: the memory vocabularies, and the policy grammar that names a
 * memory source.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Enumerated tests over closed vocabularies, in the shape
 * `tests/test_verify_model.c` and `tests/test_gate_model.c` use: every member is
 * visited, and every expectation is written out here rather than derived from
 * the same table the implementation reads. A test that agreed with a second copy
 * of the rules would pass while the rules were wrong.
 *
 * Nothing here opens a database, creates a process or reads a repository. The
 * vocabularies are pure functions of an enum, and the policy value grammar is a
 * pure function of a byte range — which is exactly why
 * `atlas_memory_source_value_parse` is exposed: a grammar that could only be
 * exercised through a root-owned file on disk could not be enumerated at all.
 */
#include <string.h>

#include "atlas/memory.h"
#include "atlas_test.h"

/* --- zeros ---------------------------------------------------------------- */

static void test_unknown_is_zero_in_every_vocabulary(void) {
    /* A6 keeps UNKNOWN at zero, A8 keeps DISABLED there, A9.2 follows, and so
     * does this season: a `memset` must never produce a member that asserts
     * something. Checked as values rather than trusted, because a later edit
     * that reorders an enum would move them silently. */
    T_EQ_INT((int)ATLAS_MEMORY_SOURCE_UNKNOWN, 0);
    T_EQ_INT((int)ATLAS_MEMORY_DIFF_UNKNOWN, 0);
    T_EQ_INT((int)ATLAS_MEMORY_ANCHOR_UNKNOWN, 0);
    T_EQ_INT((int)ATLAS_MEMORY_PACK_UNKNOWN, 0);
    T_EQ_INT((int)ATLAS_MEMORY_CAUSE_UNKNOWN, 0);
}

static void test_the_zero_member_is_named_unknown_and_never_parses(void) {
    /* Both halves matter and they are not the same claim. The *name* is
     * "UNKNOWN" so that a zero which reached a renderer says so instead of
     * naming a member that asserts something. The *parse* refuses that spelling
     * so that a caller cannot store one: zero means "nobody filled this in", and
     * a parser that accepted the word would let a request write the absence of a
     * statement as though it were a statement. */
    T_EQ_STR(atlas_memory_source_class_name(ATLAS_MEMORY_SOURCE_UNKNOWN), "UNKNOWN");
    T_EQ_STR(atlas_memory_diff_kind_name(ATLAS_MEMORY_DIFF_UNKNOWN), "UNKNOWN");
    T_EQ_STR(atlas_memory_anchor_kind_name(ATLAS_MEMORY_ANCHOR_UNKNOWN), "UNKNOWN");
    T_EQ_STR(atlas_memory_pack_status_name(ATLAS_MEMORY_PACK_UNKNOWN), "UNKNOWN");
    T_EQ_STR(atlas_memory_gen_cause_name(ATLAS_MEMORY_CAUSE_UNKNOWN), "UNKNOWN");

    atlas_memory_source_class cls = ATLAS_MEMORY_SOURCE_REPO_FILE;
    atlas_memory_diff_kind diff = ATLAS_MEMORY_DIFF_ADDED;
    atlas_memory_anchor_kind anchor = ATLAS_MEMORY_ANCHOR_PATH;
    atlas_memory_pack_status pack = ATLAS_MEMORY_PACK_CURRENT;
    atlas_memory_gen_cause cause = ATLAS_MEMORY_CAUSE_COMMIT;

    T_CHECK_MSG(!atlas_memory_source_class_parse("UNKNOWN", &cls),
                "the source class parser accepted the zero member's name");
    T_CHECK_MSG(!atlas_memory_diff_kind_parse("UNKNOWN", &diff),
                "the diff kind parser accepted the zero member's name");
    T_CHECK_MSG(!atlas_memory_anchor_kind_parse("UNKNOWN", &anchor),
                "the anchor kind parser accepted the zero member's name");
    T_CHECK_MSG(!atlas_memory_pack_status_parse("UNKNOWN", &pack),
                "the pack status parser accepted the zero member's name");
    T_CHECK_MSG(!atlas_memory_gen_cause_parse("UNKNOWN", &cause),
                "the gen cause parser accepted the zero member's name");

    /* A refused parse leaves the caller's variable alone, so a caller that
     * ignored the return value is not handed a member it never asked for. */
    T_EQ_INT((int)cls, (int)ATLAS_MEMORY_SOURCE_REPO_FILE);
    T_EQ_INT((int)diff, (int)ATLAS_MEMORY_DIFF_ADDED);
    T_EQ_INT((int)anchor, (int)ATLAS_MEMORY_ANCHOR_PATH);
    T_EQ_INT((int)pack, (int)ATLAS_MEMORY_PACK_CURRENT);
    T_EQ_INT((int)cause, (int)ATLAS_MEMORY_CAUSE_COMMIT);
}

/* --- round trips ---------------------------------------------------------- */

static void test_every_source_class_round_trips(void) {
    static const struct {
        atlas_memory_source_class c;
        const char *name;
    } CASES[] = {
        {ATLAS_MEMORY_SOURCE_REPO_FILE, "REPO_FILE"},
        {ATLAS_MEMORY_SOURCE_REPO_DIR, "REPO_DIR"},
        {ATLAS_MEMORY_SOURCE_EXTERNAL_FILE, "EXTERNAL_FILE"},
        {ATLAS_MEMORY_SOURCE_EXTERNAL_DIR, "EXTERNAL_DIR"},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        T_EQ_STR(atlas_memory_source_class_name(CASES[i].c), CASES[i].name);
        atlas_memory_source_class got = ATLAS_MEMORY_SOURCE_UNKNOWN;
        T_CHECK_MSG(atlas_memory_source_class_parse(CASES[i].name, &got), "\"%s\" did not parse",
                    CASES[i].name);
        T_EQ_INT((int)got, (int)CASES[i].c);
        T_EQ_STR(atlas_memory_source_class_name(got), CASES[i].name);
    }
    atlas_memory_source_class got = ATLAS_MEMORY_SOURCE_UNKNOWN;
    T_CHECK(!atlas_memory_source_class_parse("repo_file", &got));
    T_CHECK(!atlas_memory_source_class_parse("", &got));
    T_CHECK(!atlas_memory_source_class_parse(NULL, &got));
}

static void test_every_diff_kind_round_trips(void) {
    static const struct {
        atlas_memory_diff_kind k;
        const char *name;
    } CASES[] = {
        {ATLAS_MEMORY_DIFF_ADDED, "ADDED"},
        {ATLAS_MEMORY_DIFF_CHANGED, "CHANGED"},
        {ATLAS_MEMORY_DIFF_SUPPORTED, "SUPPORTED"},
        {ATLAS_MEMORY_DIFF_CONTRADICTED, "CONTRADICTED"},
        {ATLAS_MEMORY_DIFF_STALE, "STALE"},
        {ATLAS_MEMORY_DIFF_IMPACTED, "IMPACTED"},
        {ATLAS_MEMORY_DIFF_SUPERSEDED, "SUPERSEDED"},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        T_EQ_STR(atlas_memory_diff_kind_name(CASES[i].k), CASES[i].name);
        atlas_memory_diff_kind got = ATLAS_MEMORY_DIFF_UNKNOWN;
        T_CHECK_MSG(atlas_memory_diff_kind_parse(CASES[i].name, &got), "\"%s\" did not parse",
                    CASES[i].name);
        T_EQ_INT((int)got, (int)CASES[i].k);
        T_EQ_STR(atlas_memory_diff_kind_name(got), CASES[i].name);
    }
    atlas_memory_diff_kind got = ATLAS_MEMORY_DIFF_UNKNOWN;
    T_CHECK(!atlas_memory_diff_kind_parse("added", &got));
    T_CHECK(!atlas_memory_diff_kind_parse(NULL, &got));
}

static void test_every_anchor_kind_round_trips(void) {
    static const struct {
        atlas_memory_anchor_kind k;
        const char *name;
    } CASES[] = {
        {ATLAS_MEMORY_ANCHOR_PATH, "PATH"},
        {ATLAS_MEMORY_ANCHOR_SYMBOL, "SYMBOL"},
        {ATLAS_MEMORY_ANCHOR_DECISION, "DECISION"},
        {ATLAS_MEMORY_ANCHOR_COMMIT, "COMMIT"},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        T_EQ_STR(atlas_memory_anchor_kind_name(CASES[i].k), CASES[i].name);
        atlas_memory_anchor_kind got = ATLAS_MEMORY_ANCHOR_UNKNOWN;
        T_CHECK_MSG(atlas_memory_anchor_kind_parse(CASES[i].name, &got), "\"%s\" did not parse",
                    CASES[i].name);
        T_EQ_INT((int)got, (int)CASES[i].k);
        T_EQ_STR(atlas_memory_anchor_kind_name(got), CASES[i].name);
    }
    atlas_memory_anchor_kind got = ATLAS_MEMORY_ANCHOR_UNKNOWN;
    T_CHECK(!atlas_memory_anchor_kind_parse("path", &got));
    T_CHECK(!atlas_memory_anchor_kind_parse(NULL, &got));
}

static void test_every_pack_status_round_trips(void) {
    static const struct {
        atlas_memory_pack_status s;
        const char *name;
    } CASES[] = {
        {ATLAS_MEMORY_PACK_CURRENT, "CURRENT"},
        {ATLAS_MEMORY_PACK_STALE, "STALE"},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        T_EQ_STR(atlas_memory_pack_status_name(CASES[i].s), CASES[i].name);
        atlas_memory_pack_status got = ATLAS_MEMORY_PACK_UNKNOWN;
        T_CHECK_MSG(atlas_memory_pack_status_parse(CASES[i].name, &got), "\"%s\" did not parse",
                    CASES[i].name);
        T_EQ_INT((int)got, (int)CASES[i].s);
        T_EQ_STR(atlas_memory_pack_status_name(got), CASES[i].name);
    }
    atlas_memory_pack_status got = ATLAS_MEMORY_PACK_UNKNOWN;
    T_CHECK(!atlas_memory_pack_status_parse("current", &got));
    T_CHECK(!atlas_memory_pack_status_parse(NULL, &got));
}

static void test_every_gen_cause_round_trips(void) {
    static const struct {
        atlas_memory_gen_cause c;
        const char *name;
    } CASES[] = {
        {ATLAS_MEMORY_CAUSE_SOURCE_REVISION, "SOURCE_REVISION"},
        {ATLAS_MEMORY_CAUSE_DECISION_REVISION, "DECISION_REVISION"},
        {ATLAS_MEMORY_CAUSE_COMMIT, "COMMIT"},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        T_EQ_STR(atlas_memory_gen_cause_name(CASES[i].c), CASES[i].name);
        atlas_memory_gen_cause got = ATLAS_MEMORY_CAUSE_UNKNOWN;
        T_CHECK_MSG(atlas_memory_gen_cause_parse(CASES[i].name, &got), "\"%s\" did not parse",
                    CASES[i].name);
        T_EQ_INT((int)got, (int)CASES[i].c);
        T_EQ_STR(atlas_memory_gen_cause_name(got), CASES[i].name);
    }
    atlas_memory_gen_cause got = ATLAS_MEMORY_CAUSE_UNKNOWN;
    T_CHECK(!atlas_memory_gen_cause_parse("commit", &got));
    T_CHECK(!atlas_memory_gen_cause_parse(NULL, &got));
}

/* --- who reads the bytes -------------------------------------------------- */

static void test_is_repo_is_true_for_exactly_the_two_repo_classes(void) {
    /* The one implementation of "who reads this". A REPO_ class names a path
     * inside a registered repository, which the daemon reaches the way it
     * reaches every other file in that tree; an EXTERNAL_ class names an
     * absolute path outside every repository, which is a different question
     * about a different reader. Enumerated over the whole vocabulary rather than
     * sampled, because the interesting failure is a member added later that
     * nobody classified. */
    T_CHECK(atlas_memory_source_class_is_repo(ATLAS_MEMORY_SOURCE_REPO_FILE));
    T_CHECK(atlas_memory_source_class_is_repo(ATLAS_MEMORY_SOURCE_REPO_DIR));
    T_CHECK(!atlas_memory_source_class_is_repo(ATLAS_MEMORY_SOURCE_EXTERNAL_FILE));
    T_CHECK(!atlas_memory_source_class_is_repo(ATLAS_MEMORY_SOURCE_EXTERNAL_DIR));
    /* A zero is not a repository class. It is not any class. */
    T_CHECK(!atlas_memory_source_class_is_repo(ATLAS_MEMORY_SOURCE_UNKNOWN));
}

static const atlas_test TESTS[] = {
    {"UNKNOWN is zero in every vocabulary", test_unknown_is_zero_in_every_vocabulary},
    {"the zero member is named UNKNOWN and never parses",
     test_the_zero_member_is_named_unknown_and_never_parses},
    {"every source class round trips", test_every_source_class_round_trips},
    {"every diff kind round trips", test_every_diff_kind_round_trips},
    {"every anchor kind round trips", test_every_anchor_kind_round_trips},
    {"every pack status round trips", test_every_pack_status_round_trips},
    {"every generation cause round trips", test_every_gen_cause_round_trips},
    {"is_repo is true for exactly the two repository classes",
     test_is_repo_is_true_for_exactly_the_two_repo_classes},
};

ATLAS_TEST_MAIN("memory_source", TESTS)
