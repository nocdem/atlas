/* Atlas - the A6 vocabularies and the rules that relate them.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The fold, the reason table, the packed reason list, the exit codes and the
 * evidence digest are pure functions — inputs in, a verdict or a digest out —
 * so this suite needs no repository, no database and no daemon.
 *
 * It asks the same functions the engine asks rather than restating their
 * answers, for the reason `tests/test_decision_model.c` gives about the
 * transition table: a test that carries its own copy of the rules passes by
 * agreeing with itself.
 */
#include <string.h>

#include "atlas/decision.h"
#include "atlas/gate.h"
#include "atlas_test.h"

/* Declared in src/gate/gate_internal.h, which is not on the test include path;
 * the digest is exercised here because it is part of what A6 promises. */
atlas_status atlas_gate_evidence_digest(const atlas_decision_revision *rev, char *hex_out,
                                        atlas_err *err);

/* --- the vocabularies ------------------------------------------------------ */

static void test_every_freshness_name_round_trips(void) {
    static const atlas_gate_freshness ALL[] = {ATLAS_GATE_UNKNOWN, ATLAS_GATE_FRESH,
                                               ATLAS_GATE_STALE, ATLAS_GATE_IMPACTED};
    for (size_t i = 0; i < sizeof ALL / sizeof ALL[0]; i++) {
        const char *name = atlas_gate_freshness_name(ALL[i]);
        atlas_gate_freshness back;
        T_CHECK_MSG(atlas_gate_freshness_parse(name, &back), "\"%s\" does not parse", name);
        T_CHECK_MSG(back == ALL[i], "\"%s\" round-tripped to something else", name);
    }
    atlas_gate_freshness ignored;
    T_CHECK(!atlas_gate_freshness_parse("fresh", &ignored));  /* case matters */
    T_CHECK(!atlas_gate_freshness_parse("PROBABLY_FINE", &ignored));
    T_CHECK(!atlas_gate_freshness_parse(NULL, &ignored));
}

static void test_unknown_and_blocked_are_the_zero_values(void) {
    /* A zeroed assessment is one nobody filled in, and the safe reading of
     * "nobody filled this in" is not "fresh" and not "pass". Both enums are
     * built around that, and if either zero moved, a memset would start
     * producing a permissive default instead of a refusal. */
    T_EQ_INT((int)ATLAS_GATE_UNKNOWN, 0);
    T_EQ_INT((int)ATLAS_GATE_BLOCKED, 0);

    atlas_gate_assessment a;
    atlas_gate_assessment_init(&a);
    T_CHECK(a.freshness == ATLAS_GATE_UNKNOWN);
    atlas_gate_assessment_free(&a);

    atlas_gate_report r;
    atlas_gate_report_init(&r);
    T_CHECK(r.result == ATLAS_GATE_BLOCKED);
    atlas_gate_report_free(&r);
}

static void test_every_reason_names_itself_and_implies_one_freshness(void) {
    for (int i = 0; i < ATLAS_GATE_REASON__COUNT; i++) {
        atlas_gate_reason r = (atlas_gate_reason)i;
        const char *name = atlas_gate_reason_name(r);
        T_REQUIRE_MSG(name != NULL, "reason %d has no name", i);
        T_CHECK_MSG(strcmp(name, "UNKNOWN_REASON") != 0,
                    "reason %d fell through to the placeholder name; every member of the "
                    "vocabulary needs a row in REASONS[]",
                    i);
        atlas_gate_reason back;
        T_CHECK_MSG(atlas_gate_reason_parse(name, &back) && back == r,
                    "\"%s\" does not round-trip", name);
        /* Every reason implies exactly one freshness, and it is looked up
         * rather than assumed. A reason with no classification would silently
         * take UNKNOWN, which is safe but hides the omission. */
        atlas_gate_freshness f = atlas_gate_reason_freshness(r);
        T_CHECK(f == ATLAS_GATE_UNKNOWN || f == ATLAS_GATE_FRESH || f == ATLAS_GATE_STALE ||
                f == ATLAS_GATE_IMPACTED);
    }
    atlas_gate_reason ignored;
    T_CHECK(!atlas_gate_reason_parse("EVERYTHING_IS_FINE", &ignored));
    T_CHECK(!atlas_gate_reason_parse("", &ignored));
}

static void test_only_one_reason_means_fresh(void) {
    /* FRESH is reachable by exactly one route, and it is the route that means
     * "every check declined to weaken this". If a second reason ever implied
     * FRESH, an assessment could report a positive verdict for a reason nobody
     * had thought about. */
    int fresh_reasons = 0;
    for (int i = 0; i < ATLAS_GATE_REASON__COUNT; i++) {
        if (atlas_gate_reason_freshness((atlas_gate_reason)i) == ATLAS_GATE_FRESH) {
            fresh_reasons++;
        }
    }
    T_EQ_INT(fresh_reasons, 1);
    T_CHECK(atlas_gate_reason_freshness(ATLAS_GATE_REASON_NO_RELEVANT_CHANGE) ==
            ATLAS_GATE_FRESH);
}

/* --- the fold --------------------------------------------------------------- */

static void test_the_fold_is_exactly_the_documented_table(void) {
    T_CHECK(atlas_gate_fold(ATLAS_GATE_PASS, ATLAS_GATE_FRESH) == ATLAS_GATE_PASS);
    T_CHECK(atlas_gate_fold(ATLAS_GATE_PASS, ATLAS_GATE_STALE) == ATLAS_GATE_REVIEW_REQUIRED);
    T_CHECK(atlas_gate_fold(ATLAS_GATE_PASS, ATLAS_GATE_IMPACTED) == ATLAS_GATE_REVIEW_REQUIRED);
    T_CHECK(atlas_gate_fold(ATLAS_GATE_PASS, ATLAS_GATE_UNKNOWN) == ATLAS_GATE_BLOCKED);

    /* REVIEW_REQUIRED can still be taken to BLOCKED, and cannot be lifted. */
    T_CHECK(atlas_gate_fold(ATLAS_GATE_REVIEW_REQUIRED, ATLAS_GATE_FRESH) ==
            ATLAS_GATE_REVIEW_REQUIRED);
    T_CHECK(atlas_gate_fold(ATLAS_GATE_REVIEW_REQUIRED, ATLAS_GATE_UNKNOWN) ==
            ATLAS_GATE_BLOCKED);
}

static void test_blocked_absorbs(void) {
    /* Once a query has failed to prove one thing, proving the next does not
     * make it safe: the unproven one is still unproven, and it is the one the
     * caller would have acted on. */
    for (int f = 0; f < 4; f++) {
        T_CHECK_MSG(atlas_gate_fold(ATLAS_GATE_BLOCKED, (atlas_gate_freshness)f) ==
                        ATLAS_GATE_BLOCKED,
                    "a %s assessment lifted a blocked gate",
                    atlas_gate_freshness_name((atlas_gate_freshness)f));
    }
}

static void test_the_exit_codes_are_the_stable_contract(void) {
    /* 0..7 keep their A0 meanings; a gate outcome is not an error and gets its
     * own numbers. Both non-success outcomes must be non-zero so that
     * `atlas gate check && deploy` cannot proceed on either, and they must
     * differ so an automation cannot treat "look at this" and "Atlas could not
     * tell" as one thing. */
    T_EQ_INT(atlas_gate_exit_code(ATLAS_GATE_PASS), 0);
    T_EQ_INT(atlas_gate_exit_code(ATLAS_GATE_REVIEW_REQUIRED), ATLAS_EXIT_GATE_REVIEW_REQUIRED);
    T_EQ_INT(atlas_gate_exit_code(ATLAS_GATE_BLOCKED), ATLAS_EXIT_GATE_BLOCKED);
    T_EQ_INT(ATLAS_EXIT_GATE_REVIEW_REQUIRED, 8);
    T_EQ_INT(ATLAS_EXIT_GATE_BLOCKED, 9);
    T_CHECK(ATLAS_EXIT_GATE_REVIEW_REQUIRED != ATLAS_EXIT_GATE_BLOCKED);
    /* And neither collides with an A0 status. */
    T_CHECK(ATLAS_EXIT_GATE_REVIEW_REQUIRED > 7);
}

/* --- a verdict never disagrees with its own explanation ---------------------- */

static void test_the_verdict_is_the_weakest_of_its_reasons(void) {
    atlas_gate_assessment a;
    atlas_gate_assessment_init(&a);
    atlas_gate_assessment_note(&a, ATLAS_GATE_REASON_NO_RELEVANT_CHANGE);
    T_CHECK(a.freshness == ATLAS_GATE_FRESH);
    atlas_gate_assessment_note(&a, ATLAS_GATE_REASON_DEPENDENCY_CHANGED);
    T_CHECK(a.freshness == ATLAS_GATE_IMPACTED);
    atlas_gate_assessment_note(&a, ATLAS_GATE_REASON_DIRECT_EVIDENCE_CHANGED);
    T_CHECK(a.freshness == ATLAS_GATE_STALE);
    atlas_gate_assessment_note(&a, ATLAS_GATE_REASON_TRAVERSAL_LIMIT);
    T_CHECK(a.freshness == ATLAS_GATE_UNKNOWN);
    /* And nothing strengthens it again. */
    atlas_gate_assessment_note(&a, ATLAS_GATE_REASON_NO_RELEVANT_CHANGE);
    T_CHECK(a.freshness == ATLAS_GATE_UNKNOWN);
    atlas_gate_assessment_free(&a);
}

static void test_a_reason_that_does_not_fit_still_weakens_the_verdict(void) {
    /* Overflow drops the surplus reason and keeps the verdict. A decision with
     * thirteen problems must not report a better answer than one with twelve. */
    atlas_gate_assessment a;
    atlas_gate_assessment_init(&a);
    atlas_gate_assessment_note(&a, ATLAS_GATE_REASON_NO_RELEVANT_CHANGE);
    /* Fill the list with distinct STALE/IMPACTED reasons. */
    static const atlas_gate_reason FILL[] = {
        ATLAS_GATE_REASON_DIRECT_EVIDENCE_CHANGED, ATLAS_GATE_REASON_LINKED_PATH_MISSING,
        ATLAS_GATE_REASON_LINKED_SYMBOL_MISSING,   ATLAS_GATE_REASON_LINKED_SYMBOL_AMBIGUOUS,
        ATLAS_GATE_REASON_LINKED_COMMIT_MISSING,   ATLAS_GATE_REASON_DEPENDENCY_CHANGED,
    };
    for (size_t i = 0; i < sizeof FILL / sizeof FILL[0]; i++) {
        atlas_gate_assessment_note(&a, FILL[i]);
    }
    while (a.reason_count < ATLAS_GATE_MAX_REASONS) {
        /* Distinct UNKNOWN reasons until the list is exactly full. */
        static const atlas_gate_reason MORE[] = {
            ATLAS_GATE_REASON_INDEX_LAG,          ATLAS_GATE_REASON_STRUCTURAL_INDEX_STALE,
            ATLAS_GATE_REASON_UNREACHABLE_BASE,   ATLAS_GATE_REASON_HISTORY_REWRITTEN,
            ATLAS_GATE_REASON_EVIDENCE_UNRESOLVED, ATLAS_GATE_REASON_REPOSITORY_AMBIGUOUS,
        };
        size_t before = a.reason_count;
        atlas_gate_assessment_note(&a, MORE[before % (sizeof MORE / sizeof MORE[0])]);
        if (a.reason_count == before) {
            break;
        }
    }
    T_EQ_INT((int)a.reason_count, ATLAS_GATE_MAX_REASONS);
    T_CHECK(a.freshness == ATLAS_GATE_UNKNOWN);
    atlas_gate_assessment_free(&a);
}

static void test_duplicate_reasons_are_absorbed(void) {
    atlas_gate_assessment a;
    atlas_gate_assessment_init(&a);
    for (int i = 0; i < 20; i++) {
        atlas_gate_assessment_note(&a, ATLAS_GATE_REASON_DIRECT_EVIDENCE_CHANGED);
    }
    T_EQ_INT((int)a.reason_count, 1);
    T_CHECK(a.freshness == ATLAS_GATE_STALE);
    atlas_gate_assessment_free(&a);
}

/* --- the packed reason list -------------------------------------------------- */

static void test_reasons_pack_in_a_canonical_order(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_gate_assessment a;
    atlas_gate_assessment_init(&a);
    atlas_gate_assessment_note(&a, ATLAS_GATE_REASON_TRAVERSAL_LIMIT);
    atlas_gate_assessment_note(&a, ATLAS_GATE_REASON_DIRECT_EVIDENCE_CHANGED);

    atlas_gate_assessment b;
    atlas_gate_assessment_init(&b);
    atlas_gate_assessment_note(&b, ATLAS_GATE_REASON_DIRECT_EVIDENCE_CHANGED);
    atlas_gate_assessment_note(&b, ATLAS_GATE_REASON_TRAVERSAL_LIMIT);

    atlas_buf pa = ATLAS_BUF_INIT;
    atlas_buf pb = ATLAS_BUF_INIT;
    T_OK(atlas_gate_reasons_pack(&a, &pa, &err), &err);
    T_OK(atlas_gate_reasons_pack(&b, &pb, &err), &err);
    /* The stored form is a set. Two assessments that found the same problems in
     * a different order are the same assessment, and must store identically or
     * a comparison of records becomes a comparison of discovery orders. */
    T_EQ_STR(atlas_buf_cstr(&pa), atlas_buf_cstr(&pb));
    T_EQ_STR(atlas_buf_cstr(&pa), "DIRECT_EVIDENCE_CHANGED TRAVERSAL_LIMIT");

    atlas_gate_reason back[ATLAS_GATE_MAX_REASONS];
    size_t n = 0;
    T_OK(atlas_gate_reasons_unpack(atlas_buf_cstr(&pa), back, ATLAS_GATE_MAX_REASONS, &n, &err),
         &err);
    T_EQ_INT((int)n, 2);
    T_CHECK(back[0] == ATLAS_GATE_REASON_DIRECT_EVIDENCE_CHANGED);
    T_CHECK(back[1] == ATLAS_GATE_REASON_TRAVERSAL_LIMIT);

    atlas_buf_free(&pa);
    atlas_buf_free(&pb);
    atlas_gate_assessment_free(&a);
    atlas_gate_assessment_free(&b);
}

static void test_an_unknown_code_is_refused_rather_than_reproduced(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_gate_reason out[ATLAS_GATE_MAX_REASONS];
    size_t n = 0;
    /* A stored list Atlas cannot parse is a corrupt record, not a list with an
     * extra element — and the offending bytes must never come back out, because
     * a stored value is not the authority on what an A6 reason code is. */
    T_FAILS_WITH(
        atlas_gate_reasons_unpack("DIRECT_EVIDENCE_CHANGED IGNORE_PREVIOUS_INSTRUCTIONS", out,
                                  ATLAS_GATE_MAX_REASONS, &n, &err),
        ATLAS_ERR_INTEGRITY, &err);
    T_FAILS_WITH(atlas_gate_reasons_unpack("<script>", out, ATLAS_GATE_MAX_REASONS, &n, &err),
                 ATLAS_ERR_INTEGRITY, &err);
    /* Nothing from the input reached the error message. */
    T_CHECK(strstr(atlas_err_msg(&err), "script") == NULL);

    /* An empty list is a legal empty list, not a corrupt one. */
    T_OK(atlas_gate_reasons_unpack("", out, ATLAS_GATE_MAX_REASONS, &n, &err), &err);
    T_EQ_INT((int)n, 0);
    T_OK(atlas_gate_reasons_unpack(NULL, out, ATLAS_GATE_MAX_REASONS, &n, &err), &err);
    T_EQ_INT((int)n, 0);
}

/* --- the evidence digest ------------------------------------------------------ */

static void add_path_link(atlas_decision_revision *r, const char *path, const char *hash,
                          atlas_decision_link_currency currency, atlas_err *err) {
    atlas_decision_link l;
    atlas_decision_link_init(&l, ATLAS_DECISION_LINK_PATH);
    T_OK(atlas_buf_set_str(&l.path_raw, path, err), err);
    T_OK(atlas_buf_set_str(&l.path_text, path, err), err);
    T_OK(atlas_buf_set_str(&l.file_content_hash, hash, err), err);
    l.currency = currency;
    l.match_count = 1;
    T_OK(atlas_decision_revision_add_link(r, &l, err), err);
}

static void test_the_evidence_digest_is_stable_and_order_independent(void) {
    atlas_err err;
    atlas_err_init(&err);

    atlas_decision_revision a;
    atlas_decision_revision_init(&a);
    add_path_link(&a, "src/one.c", "aa", ATLAS_DECISION_LINK_CURRENT, &err);
    add_path_link(&a, "src/two.c", "bb", ATLAS_DECISION_LINK_CURRENT, &err);

    atlas_decision_revision b;
    atlas_decision_revision_init(&b);
    add_path_link(&b, "src/two.c", "bb", ATLAS_DECISION_LINK_CURRENT, &err);
    add_path_link(&b, "src/one.c", "aa", ATLAS_DECISION_LINK_CURRENT, &err);

    char ha[ATLAS_SHA256_HEX_LEN + 1u];
    char hb[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(atlas_gate_evidence_digest(&a, ha, &err), &err);
    T_OK(atlas_gate_evidence_digest(&b, hb, &err), &err);
    T_EQ_INT((int)strlen(ha), ATLAS_SHA256_HEX_LEN);
    T_CHECK_MSG(strcmp(ha, hb) == 0,
                "reordering a set of links changed its digest, so every capability would be "
                "invalidated by an argument shuffle");

    /* And it is a digest of what the links resolve to, so a currency change is
     * drift. This is the whole reason a revalidation capability carries one. */
    a.links[0].currency = ATLAS_DECISION_LINK_CHANGED;
    char hc[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(atlas_gate_evidence_digest(&a, hc, &err), &err);
    T_CHECK_MSG(strcmp(ha, hc) != 0, "a link that stopped being current left the digest alone");

    atlas_decision_revision_free(&a);
    atlas_decision_revision_free(&b);
}

static void test_the_evidence_digest_is_length_prefixed(void) {
    atlas_err err;
    atlas_err_init(&err);
    /* With any single-byte delimiter, a path of "a/b" beside a path of "c"
     * would encode identically to a path of "a" beside a path of "b/c". The
     * digest is length-prefixed precisely so that it does not, and this is the
     * pair that would collide if somebody replaced the prefixes with
     * separators. */
    atlas_decision_revision a;
    atlas_decision_revision_init(&a);
    add_path_link(&a, "a", "x", ATLAS_DECISION_LINK_CURRENT, &err);
    add_path_link(&a, "bc", "x", ATLAS_DECISION_LINK_CURRENT, &err);

    atlas_decision_revision b;
    atlas_decision_revision_init(&b);
    add_path_link(&b, "ab", "x", ATLAS_DECISION_LINK_CURRENT, &err);
    add_path_link(&b, "c", "x", ATLAS_DECISION_LINK_CURRENT, &err);

    char ha[ATLAS_SHA256_HEX_LEN + 1u];
    char hb[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(atlas_gate_evidence_digest(&a, ha, &err), &err);
    T_OK(atlas_gate_evidence_digest(&b, hb, &err), &err);
    T_CHECK_MSG(strcmp(ha, hb) != 0, "the evidence digest is delimited rather than length-prefixed");

    atlas_decision_revision_free(&a);
    atlas_decision_revision_free(&b);
}

static void test_the_evidence_digest_is_domain_separated(void) {
    /* A bare SHA-256 says nothing about what was hashed, so an evidence digest
     * must never be confusable with a content hash, a file hash or a root hash.
     * The domain is the guarantee, and the two A6/A4 domains must differ. */
    T_CHECK(strcmp(ATLAS_GATE_EVIDENCE_DOMAIN, ATLAS_DECISION_HASH_DOMAIN) != 0);
    T_CHECK(strncmp(ATLAS_GATE_EVIDENCE_DOMAIN, "atlas.gate.", 11u) == 0);
}

static void test_an_empty_revision_still_digests(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_decision_revision r;
    atlas_decision_revision_init(&r);
    char h[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(atlas_gate_evidence_digest(&r, h, &err), &err);
    T_EQ_INT((int)strlen(h), ATLAS_SHA256_HEX_LEN);
    atlas_decision_revision_free(&r);
}

static const atlas_test TESTS[] = {
    {"every freshness name round-trips", test_every_freshness_name_round_trips},
    {"UNKNOWN and BLOCKED are the zero values", test_unknown_and_blocked_are_the_zero_values},
    {"every reason names itself and implies one freshness",
     test_every_reason_names_itself_and_implies_one_freshness},
    {"only one reason means FRESH", test_only_one_reason_means_fresh},
    {"the fold is exactly the documented table", test_the_fold_is_exactly_the_documented_table},
    {"BLOCKED absorbs", test_blocked_absorbs},
    {"the exit codes are the stable contract", test_the_exit_codes_are_the_stable_contract},
    {"the verdict is the weakest of its reasons",
     test_the_verdict_is_the_weakest_of_its_reasons},
    {"a reason that does not fit still weakens the verdict",
     test_a_reason_that_does_not_fit_still_weakens_the_verdict},
    {"duplicate reasons are absorbed", test_duplicate_reasons_are_absorbed},
    {"reasons pack in a canonical order", test_reasons_pack_in_a_canonical_order},
    {"an unknown code is refused rather than reproduced",
     test_an_unknown_code_is_refused_rather_than_reproduced},
    {"the evidence digest is stable and order independent",
     test_the_evidence_digest_is_stable_and_order_independent},
    {"the evidence digest is length-prefixed", test_the_evidence_digest_is_length_prefixed},
    {"the evidence digest is domain-separated", test_the_evidence_digest_is_domain_separated},
    {"an empty revision still digests", test_an_empty_revision_still_digests},
};

ATLAS_TEST_MAIN("gate_model", TESTS)
