/* Atlas - A4: the canonical content hash, validation and the transition table.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Pure tests: bytes in, a digest or a verdict out. No database, no daemon, no
 * repository, so this belongs in the fast subset.
 *
 * The hash tests are the interesting ones and they are adversarial rather than
 * illustrative: the property being checked is that two *different* revisions
 * cannot produce one digest, which is what an approval binding to a content
 * hash actually depends on. A test that only checks "the same input hashes the
 * same" would pass against a delimiter-joined encoding that collides.
 */
#include <string.h>

#include "atlas/ai.h"
#include "atlas/atlas.h"
#include "atlas/decision.h"
#include "atlas_test.h"

/* --- helpers --------------------------------------------------------------- */

static void set(atlas_buf *b, const char *s) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_buf_set_str(b, s, &err), &err);
}

static void hash_of(atlas_decision_revision *r, char *out) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_decision_content_hash(r, out, &err), &err);
}

static void basic(atlas_decision_revision *r, const char *title, const char *decision) {
    atlas_decision_revision_init(r);
    set(&r->title, title);
    set(&r->decision_text, decision);
}

/* --- the transition table --------------------------------------------------- */

static void test_transition_table_is_the_only_authority(void) {
    /* Every pair, enumerated, for a kind that cannot be resolved. The permitted
     * set is tiny and stated here as data so a change to
     * `atlas_decision_transition_allowed` has to be reflected in a list somebody
     * reads, rather than passing because the test asks the same function it is
     * testing.
     *
     * A9.1 gave the function the document's kind. These twenty-five pairs are the
     * table for every kind whose approved form makes no demand — which is six of
     * the eight — and `test_only_a_demand_can_be_resolved` covers the other two
     * and the one cell they differ in. */
    struct {
        atlas_decision_state from;
        atlas_decision_state to;
        bool allowed;
    } cases[] = {
        {ATLAS_DECISION_PROPOSED, ATLAS_DECISION_APPROVED, true},
        {ATLAS_DECISION_PROPOSED, ATLAS_DECISION_REJECTED, true},
        /* A proposal cannot be superseded: superseding something that was never
         * effective would record that policy changed when none existed. */
        {ATLAS_DECISION_PROPOSED, ATLAS_DECISION_SUPERSEDED, false},
        {ATLAS_DECISION_PROPOSED, ATLAS_DECISION_PROPOSED, false},
        /* Nor resolved. Discharging a demand nobody accepted would make recording
         * a demand and satisfying it one step, and the acceptance is the part an
         * operator has to have seen. */
        {ATLAS_DECISION_PROPOSED, ATLAS_DECISION_RESOLVED, false},

        {ATLAS_DECISION_APPROVED, ATLAS_DECISION_SUPERSEDED, true},
        /* No retraction path. Withdrawing a decision means approving its
         * replacement, which leaves a record of what replaced it. */
        {ATLAS_DECISION_APPROVED, ATLAS_DECISION_REJECTED, false},
        {ATLAS_DECISION_APPROVED, ATLAS_DECISION_PROPOSED, false},
        {ATLAS_DECISION_APPROVED, ATLAS_DECISION_APPROVED, false},
        /* The cell the kind decides. For a DECISION there is nothing to
         * discharge, so it is refused here and permitted for an OBLIGATION. */
        {ATLAS_DECISION_APPROVED, ATLAS_DECISION_RESOLVED, false},

        /* Rule 3. The single most important refusal in the table: "we said no
         * and then it quietly became policy" must be impossible. */
        {ATLAS_DECISION_REJECTED, ATLAS_DECISION_APPROVED, false},
        {ATLAS_DECISION_REJECTED, ATLAS_DECISION_PROPOSED, false},
        {ATLAS_DECISION_REJECTED, ATLAS_DECISION_SUPERSEDED, false},
        {ATLAS_DECISION_REJECTED, ATLAS_DECISION_REJECTED, false},
        {ATLAS_DECISION_REJECTED, ATLAS_DECISION_RESOLVED, false},

        {ATLAS_DECISION_SUPERSEDED, ATLAS_DECISION_APPROVED, false},
        {ATLAS_DECISION_SUPERSEDED, ATLAS_DECISION_PROPOSED, false},
        {ATLAS_DECISION_SUPERSEDED, ATLAS_DECISION_REJECTED, false},
        {ATLAS_DECISION_SUPERSEDED, ATLAS_DECISION_SUPERSEDED, false},
        {ATLAS_DECISION_SUPERSEDED, ATLAS_DECISION_RESOLVED, false},

        /* A9.1. RESOLVED is terminal for the same reason REJECTED is, and with
         * the same remedy: reopening a discharged obligation is a new revision
         * approved through the channel, not a state that quietly comes back. */
        {ATLAS_DECISION_RESOLVED, ATLAS_DECISION_APPROVED, false},
        {ATLAS_DECISION_RESOLVED, ATLAS_DECISION_PROPOSED, false},
        {ATLAS_DECISION_RESOLVED, ATLAS_DECISION_REJECTED, false},
        {ATLAS_DECISION_RESOLVED, ATLAS_DECISION_SUPERSEDED, false},
        {ATLAS_DECISION_RESOLVED, ATLAS_DECISION_RESOLVED, false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        bool got = atlas_decision_transition_allowed(ATLAS_DECISION_KIND_DECISION, cases[i].from,
                                                     cases[i].to);
        T_CHECK_MSG(got == cases[i].allowed, "%s -> %s should be %s",
                    atlas_decision_state_name(cases[i].from),
                    atlas_decision_state_name(cases[i].to),
                    cases[i].allowed ? "allowed" : "refused");
    }
    /* All twenty-five pairs hold for every non-resolvable kind, not just for
     * DECISION: the kind widens the table in one cell and narrows it nowhere, and
     * a future edit that made some other kind unrejectable would be caught here
     * rather than in whichever surface first refused a rejection. */
    for (size_t k = 0; k < atlas_decision_kind_count(); k++) {
        atlas_decision_kind kind = atlas_decision_kind_at(k);
        if (atlas_decision_kind_resolvable(kind)) {
            continue;
        }
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            bool got = atlas_decision_transition_allowed(kind, cases[i].from, cases[i].to);
            T_CHECK_MSG(got == cases[i].allowed, "%s: %s -> %s should be %s",
                        atlas_decision_kind_name(kind),
                        atlas_decision_state_name(cases[i].from),
                        atlas_decision_state_name(cases[i].to),
                        cases[i].allowed ? "allowed" : "refused");
        }
    }
}

/* A9.1. The one cell the knowledge kind decides, from both directions. */
static void test_only_a_demand_can_be_resolved(void) {
    /* Which kinds may be resolved, stated as data rather than asked of
     * `atlas_decision_kind_resolvable` — otherwise this test would agree with
     * whatever the table says. */
    struct {
        atlas_decision_kind kind;
        bool resolvable;
    } expect[] = {
        {ATLAS_DECISION_KIND_DECISION, false},
        {ATLAS_DECISION_KIND_POLICY, false},
        {ATLAS_DECISION_KIND_INVARIANT, false},
        {ATLAS_DECISION_KIND_OPERATIONAL_FACT, false},
        /* A risk that was eliminated rather than replaced. */
        {ATLAS_DECISION_KIND_ACCEPTED_RISK, true},
        /* The kind the state exists for. */
        {ATLAS_DECISION_KIND_OBLIGATION, true},
        {ATLAS_DECISION_KIND_PARKED, false},
        {ATLAS_DECISION_KIND_REJECTED_ALTERNATIVE, false},
    };
    T_REQUIRE(sizeof(expect) / sizeof(expect[0]) == atlas_decision_kind_count());
    for (size_t i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) {
        T_CHECK_MSG(atlas_decision_kind_resolvable(expect[i].kind) == expect[i].resolvable,
                    "%s resolvable should be %s", atlas_decision_kind_name(expect[i].kind),
                    expect[i].resolvable ? "true" : "false");
        /* The predicate and the table must agree, in both directions, for every
         * kind. Two answers to one question is how a surface ends up minting a
         * capability the write point then refuses. */
        T_CHECK_MSG(atlas_decision_transition_allowed(expect[i].kind, ATLAS_DECISION_APPROVED,
                                                      ATLAS_DECISION_RESOLVED) ==
                        expect[i].resolvable,
                    "%s APPROVED -> RESOLVED disagrees with the predicate",
                    atlas_decision_kind_name(expect[i].kind));
        /* And a resolvable kind gains *only* that cell: everything else about its
         * lifecycle is the same as a decision's. */
        T_CHECK(atlas_decision_transition_allowed(expect[i].kind, ATLAS_DECISION_PROPOSED,
                                                  ATLAS_DECISION_RESOLVED) == false);
        T_CHECK(atlas_decision_transition_allowed(expect[i].kind, ATLAS_DECISION_RESOLVED,
                                                  ATLAS_DECISION_APPROVED) == false);
        T_CHECK(atlas_decision_transition_allowed(expect[i].kind, ATLAS_DECISION_APPROVED,
                                                  ATLAS_DECISION_SUPERSEDED) == true);
        T_CHECK(atlas_decision_transition_allowed(expect[i].kind, ATLAS_DECISION_PROPOSED,
                                                  ATLAS_DECISION_REJECTED) == true);
    }
}

static void test_vocabularies_are_closed(void) {
    atlas_decision_state s = ATLAS_DECISION_APPROVED;
    /* No default. An unrecognised value must not become a known one — that is
     * how a garbled state becomes an approval. */
    T_CHECK(!atlas_decision_state_parse("approved", &s)); /* case matters */
    T_CHECK(!atlas_decision_state_parse("", &s));
    T_CHECK(!atlas_decision_state_parse("ACCEPTED", &s));
    T_CHECK(atlas_decision_state_parse("APPROVED", &s) && s == ATLAS_DECISION_APPROVED);

    atlas_decision_actor a = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
    T_CHECK(!atlas_decision_actor_parse("USER_APPROVED_DECISION", &a));
    T_CHECK(!atlas_decision_actor_parse("HUMAN", &a));
    T_CHECK(atlas_decision_actor_parse("LOCAL_OPERATOR_CONFIRMED", &a) &&
            a == ATLAS_DECISION_ACTOR_LOCAL_OPERATOR_CONFIRMED);

    /* The restriction, mirroring atlas_provenance_writable_in_a2. */
    T_CHECK(atlas_decision_actor_writable_by_adapter(ATLAS_DECISION_ACTOR_MODEL_PROPOSAL));
    T_CHECK(atlas_decision_actor_writable_by_adapter(ATLAS_DECISION_ACTOR_MODEL_INFERENCE));
    T_CHECK(
        !atlas_decision_actor_writable_by_adapter(ATLAS_DECISION_ACTOR_LOCAL_OPERATOR_CONFIRMED));
    T_CHECK(!atlas_decision_actor_writable_by_adapter(ATLAS_DECISION_ACTOR_ATLAS_AUTOMATIC));
}

/* --- the canonical content hash ---------------------------------------------- */

static void test_hash_is_stable_and_domain_separated(void) {
    atlas_decision_revision a, b;
    basic(&a, "Use SQLite WAL", "Enable WAL journalling.");
    basic(&b, "Use SQLite WAL", "Enable WAL journalling.");

    char ha[ATLAS_SHA256_HEX_LEN + 1u], hb[ATLAS_SHA256_HEX_LEN + 1u];
    hash_of(&a, ha);
    hash_of(&b, hb);
    T_CHECK_MSG(strcmp(ha, hb) == 0, "identical content must hash identically");
    T_EQ_INT((int)strlen(ha), (int)ATLAS_SHA256_HEX_LEN);

    /* Domain separation: the digest of a revision must never be the bare
     * SHA-256 of its own text, or it could be confused with a file content
     * hash, a root hash or a compile-command hash — all of which are SHA-256 of
     * raw bytes and all of which appear in the same database. */
    char plain[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_sha256_hex("Enable WAL journalling.", strlen("Enable WAL journalling."), plain);
    T_CHECK(strcmp(ha, plain) != 0);

    atlas_decision_revision_free(&a);
    atlas_decision_revision_free(&b);
}

static void test_hash_cannot_be_collided_by_moving_a_delimiter(void) {
    /* The reason the encoding is length-prefixed rather than delimited.
     *
     * With any single-byte delimiter, a title of "a<D>b" and a decision of "c"
     * encodes to the same byte stream as a title of "a" and a decision of
     * "b<D>c". Those are two genuinely different decisions, and a digest they
     * share is not an identity — an approval bound to it would be an approval
     * of either. Several plausible delimiters are tried, because getting this
     * right for `|` and wrong for `\0` would be no better. */
    static const char *const delims[] = {"|", "\n", "\x01", ":", "\t", NULL};
    for (size_t i = 0; delims[i] != NULL; i++) {
        char left[64], right[64];
        (void)snprintf(left, sizeof(left), "a%sb", delims[i]);
        (void)snprintf(right, sizeof(right), "b%sc", delims[i]);

        atlas_decision_revision x, y;
        basic(&x, left, "c");
        basic(&y, "a", right);

        char hx[ATLAS_SHA256_HEX_LEN + 1u], hy[ATLAS_SHA256_HEX_LEN + 1u];
        hash_of(&x, hx);
        hash_of(&y, hy);
        T_CHECK_MSG(strcmp(hx, hy) != 0,
                    "delimiter %zu: two different revisions collided on one content hash", i);
        atlas_decision_revision_free(&x);
        atlas_decision_revision_free(&y);
    }
}

static void test_every_content_field_changes_the_hash(void) {
    /* A field that is stored but not hashed is a field an approval does not
     * cover: it could be changed after approval without invalidating anything.
     * So each is perturbed in turn and the digest must move. */
    char base_hash[ATLAS_SHA256_HEX_LEN + 1u];
    {
        atlas_decision_revision r;
        basic(&r, "T", "D");
        hash_of(&r, base_hash);
        atlas_decision_revision_free(&r);
    }

    /* context */
    {
        atlas_decision_revision r;
        basic(&r, "T", "D");
        set(&r.context_text, "x");
        char h[ATLAS_SHA256_HEX_LEN + 1u];
        hash_of(&r, h);
        T_CHECK_MSG(strcmp(h, base_hash) != 0, "context must be covered by the content hash");
        atlas_decision_revision_free(&r);
    }
    /* rationale */
    {
        atlas_decision_revision r;
        basic(&r, "T", "D");
        set(&r.rationale_text, "x");
        char h[ATLAS_SHA256_HEX_LEN + 1u];
        hash_of(&r, h);
        T_CHECK_MSG(strcmp(h, base_hash) != 0, "rationale must be covered by the content hash");
        atlas_decision_revision_free(&r);
    }
    /* consequences */
    {
        atlas_decision_revision r;
        basic(&r, "T", "D");
        set(&r.consequences_text, "x");
        char h[ATLAS_SHA256_HEX_LEN + 1u];
        hash_of(&r, h);
        T_CHECK_MSG(strcmp(h, base_hash) != 0, "consequences must be covered by the content hash");
        atlas_decision_revision_free(&r);
    }
    /* scope */
    {
        atlas_decision_revision r;
        basic(&r, "T", "D");
        r.scope = ATLAS_DECISION_SCOPE_REPOSITORY;
        char h[ATLAS_SHA256_HEX_LEN + 1u];
        hash_of(&r, h);
        T_CHECK_MSG(strcmp(h, base_hash) != 0, "scope must be covered by the content hash");
        atlas_decision_revision_free(&r);
    }
    /* an alternative */
    {
        atlas_decision_revision r;
        atlas_err err;
        atlas_err_init(&err);
        basic(&r, "T", "D");
        T_OK(atlas_decision_revision_add_alternative(&r, "keep it", strlen("keep it"), &err), &err);
        char h[ATLAS_SHA256_HEX_LEN + 1u];
        hash_of(&r, h);
        T_CHECK_MSG(strcmp(h, base_hash) != 0, "alternatives must be covered by the content hash");
        atlas_decision_revision_free(&r);
    }
    /* a link */
    {
        atlas_decision_revision r;
        atlas_err err;
        atlas_err_init(&err);
        basic(&r, "T", "D");
        atlas_decision_link l;
        atlas_decision_link_init(&l, ATLAS_DECISION_LINK_PATH);
        set(&l.path_raw, "src/db/db.c");
        T_OK(atlas_decision_revision_add_link(&r, &l, &err), &err);
        atlas_decision_link_free(&l);
        char h[ATLAS_SHA256_HEX_LEN + 1u];
        hash_of(&r, h);
        T_CHECK_MSG(strcmp(h, base_hash) != 0, "links must be covered by the content hash");
        atlas_decision_revision_free(&r);
    }
}

static void test_link_order_does_not_change_the_hash(void) {
    /* Links are a set: naming three paths in a different order is the same
     * decision about the same three paths. If order mattered, a retry that
     * shuffled its arguments would create a new revision of an unchanged
     * document, and rule 11 — retries are idempotent — would be false in
     * exactly the case it exists for. */
    static const char *const paths[] = {"a.c", "b.c", "c.c"};
    char forward[ATLAS_SHA256_HEX_LEN + 1u], reverse[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_err err;
    atlas_err_init(&err);

    for (int pass = 0; pass < 2; pass++) {
        atlas_decision_revision r;
        basic(&r, "T", "D");
        for (int i = 0; i < 3; i++) {
            atlas_decision_link l;
            atlas_decision_link_init(&l, ATLAS_DECISION_LINK_PATH);
            set(&l.path_raw, paths[pass == 0 ? i : 2 - i]);
            T_OK(atlas_decision_revision_add_link(&r, &l, &err), &err);
            atlas_decision_link_free(&l);
        }
        hash_of(&r, pass == 0 ? forward : reverse);
        atlas_decision_revision_free(&r);
    }
    T_CHECK_MSG(strcmp(forward, reverse) == 0,
                "the same set of links in a different order must hash the same");
}

static void test_alternative_order_does_change_the_hash(void) {
    /* And the converse: a list of alternatives is ordered by the proposer's
     * judgement, so reordering it says something different and must be a
     * different revision. */
    char forward[ATLAS_SHA256_HEX_LEN + 1u], reverse[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_err err;
    atlas_err_init(&err);
    static const char *const alts[] = {"first", "second"};
    for (int pass = 0; pass < 2; pass++) {
        atlas_decision_revision r;
        basic(&r, "T", "D");
        for (int i = 0; i < 2; i++) {
            const char *a = alts[pass == 0 ? i : 1 - i];
            T_OK(atlas_decision_revision_add_alternative(&r, a, strlen(a), &err), &err);
        }
        hash_of(&r, pass == 0 ? forward : reverse);
        atlas_decision_revision_free(&r);
    }
    T_CHECK_MSG(strcmp(forward, reverse) != 0,
                "reordered alternatives are a different revision");
}

static void test_empty_and_absent_alternatives_differ(void) {
    /* The count is hashed before the elements, so a list of one empty string
     * cannot encode identically to no list at all. */
    atlas_err err;
    atlas_err_init(&err);
    atlas_decision_revision none, one_empty;
    basic(&none, "T", "D");
    basic(&one_empty, "T", "D");
    T_OK(atlas_decision_revision_add_alternative(&one_empty, "", 0, &err), &err);

    char hn[ATLAS_SHA256_HEX_LEN + 1u], he[ATLAS_SHA256_HEX_LEN + 1u];
    hash_of(&none, hn);
    hash_of(&one_empty, he);
    T_CHECK(strcmp(hn, he) != 0);
    atlas_decision_revision_free(&none);
    atlas_decision_revision_free(&one_empty);
}

static void test_snapshot_provenance_is_hashed(void) {
    /* **The immutable snapshot is part of what was approved.**
     *
     * An earlier version excluded the basis commit, the captured file content
     * hash and the analyzer identity, on the argument that hashing them would
     * make a revision's identity depend on which commit happened to be checked
     * out. That argument was wrong: it *should* depend on that. A decision
     * taken against commit X is not the decision taken against commit Y, and an
     * approval that did not cover the captured hash would let a CHANGED link be
     * rewritten into a CURRENT one under an approved record without anybody
     * approving anything.
     *
     * Each field is perturbed on its own, so a field that is stored but not
     * hashed is caught individually rather than hidden by its neighbours. */
    atlas_err err;
    atlas_err_init(&err);

    struct {
        const char *what;
        const char *basis;
        const char *file_hash;
        const char *analyzer;
        int64_t analyzer_version;
        int64_t line;
        const char *kind;
    } cases[] = {
        {"baseline", "c0", "f0", "atlas-lexical-c", 1, 10, "function"},
        {"basis commit", "c1", "f0", "atlas-lexical-c", 1, 10, "function"},
        {"captured file hash", "c0", "f1", "atlas-lexical-c", 1, 10, "function"},
        {"analyzer name", "c0", "f0", "atlas-other", 1, 10, "function"},
        {"analyzer version", "c0", "f0", "atlas-lexical-c", 2, 10, "function"},
        {"symbol line", "c0", "f0", "atlas-lexical-c", 1, 11, "function"},
        {"symbol kind", "c0", "f0", "atlas-lexical-c", 1, 10, "variable"},
    };
    char hashes[sizeof(cases) / sizeof(cases[0])][ATLAS_SHA256_HEX_LEN + 1u];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        atlas_decision_revision r;
        basic(&r, "T", "D");
        atlas_decision_link l;
        atlas_decision_link_init(&l, ATLAS_DECISION_LINK_SYMBOL);
        set(&l.symbol_name, "atlas_db_open");
        set(&l.symbol_kind, cases[i].kind);
        set(&l.basis_commit, cases[i].basis);
        set(&l.file_content_hash, cases[i].file_hash);
        set(&l.analyzer_name, cases[i].analyzer);
        l.analyzer_version = cases[i].analyzer_version;
        l.symbol_line = cases[i].line;
        T_OK(atlas_decision_revision_add_link(&r, &l, &err), &err);
        atlas_decision_link_free(&l);
        hash_of(&r, hashes[i]);
        atlas_decision_revision_free(&r);
    }
    for (size_t i = 1; i < sizeof(cases) / sizeof(cases[0]); i++) {
        T_CHECK_MSG(strcmp(hashes[0], hashes[i]) != 0,
                    "changing the %s must change the content hash; an approval that did not "
                    "cover it would let it be rewritten under an approved record",
                    cases[i].what);
    }
}

static void test_the_revision_basis_and_repository_are_hashed(void) {
    /* The revision's own basis HEAD, the repository it was written against, and
     * the actor that proposed it. Each is immutable and each changes what was
     * approved.
     *
     * The repository identity in particular: without it, an identical decision
     * about two different projects has one digest, and a document moved between
     * them by any means would still verify. */
    char base[ATLAS_SHA256_HEX_LEN + 1u];
    {
        atlas_decision_revision r;
        basic(&r, "T", "D");
        set(&r.basis_head, "aaaa");
        set(&r.basis_repo_identity, "repo-one");
        r.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
        hash_of(&r, base);
        atlas_decision_revision_free(&r);
    }
    struct {
        const char *what;
        const char *basis;
        const char *identity;
        atlas_decision_actor actor;
    } cases[] = {
        {"basis head", "bbbb", "repo-one", ATLAS_DECISION_ACTOR_MODEL_PROPOSAL},
        {"repository identity", "aaaa", "repo-two", ATLAS_DECISION_ACTOR_MODEL_PROPOSAL},
        /* Mutating MODEL_INFERENCE to MODEL_PROPOSAL would upgrade the apparent
         * standing of an approved record without changing a word of it. */
        {"proposing actor", "aaaa", "repo-one", ATLAS_DECISION_ACTOR_MODEL_INFERENCE},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        atlas_decision_revision r;
        basic(&r, "T", "D");
        set(&r.basis_head, cases[i].basis);
        set(&r.basis_repo_identity, cases[i].identity);
        r.proposed_by = cases[i].actor;
        char h[ATLAS_SHA256_HEX_LEN + 1u];
        hash_of(&r, h);
        T_CHECK_MSG(strcmp(base, h) != 0, "changing the %s must change the content hash",
                    cases[i].what);
        atlas_decision_revision_free(&r);
    }
}

static void test_live_currency_is_not_hashed(void) {
    /* The other half of the rule, and the reason it is not simply "hash
     * everything".
     *
     * A link's currency and match count are *observations made when the link is
     * read*. Hashing them would make an approved revision's identity change
     * every time the code changed — so an approval would silently stop
     * verifying the moment somebody edited a file, which is exactly backwards.
     * They are also never stored, so they could not be reproduced at verify
     * time even if one wanted to. */
    atlas_err err;
    atlas_err_init(&err);
    char hashes[3][ATLAS_SHA256_HEX_LEN + 1u];
    atlas_decision_link_currency states[] = {
        ATLAS_DECISION_LINK_CURRENT,
        ATLAS_DECISION_LINK_CHANGED,
        ATLAS_DECISION_LINK_MISSING,
    };
    for (size_t i = 0; i < 3u; i++) {
        atlas_decision_revision r;
        basic(&r, "T", "D");
        atlas_decision_link l;
        atlas_decision_link_init(&l, ATLAS_DECISION_LINK_PATH);
        set(&l.path_raw, "src/db.c");
        l.currency = states[i];
        l.match_count = (int64_t)i + 1;
        T_OK(atlas_decision_revision_add_link(&r, &l, &err), &err);
        atlas_decision_link_free(&l);
        hash_of(&r, hashes[i]);
        atlas_decision_revision_free(&r);
    }
    T_CHECK_MSG(strcmp(hashes[0], hashes[1]) == 0 && strcmp(hashes[1], hashes[2]) == 0,
                "a live currency result must not change the content hash");
}

static void test_derived_display_encodings_are_not_hashed(void) {
    /* `path_text` and `symbol_name_text` are the `%XX` display encodings of the
     * raw bytes beside them. They are derived, so hashing them would hash the
     * same information twice and would make a revision's identity depend on the
     * encoder rather than on the content. The raw bytes *are* hashed, and the
     * test proves that by changing them. */
    atlas_err err;
    atlas_err_init(&err);
    char a[ATLAS_SHA256_HEX_LEN + 1u], b[ATLAS_SHA256_HEX_LEN + 1u], c[ATLAS_SHA256_HEX_LEN + 1u];
    for (int pass = 0; pass < 3; pass++) {
        atlas_decision_revision r;
        basic(&r, "T", "D");
        atlas_decision_link l;
        atlas_decision_link_init(&l, ATLAS_DECISION_LINK_PATH);
        set(&l.path_raw, pass == 2 ? "src/other.c" : "src/db.c");
        /* Pass 1 carries a deliberately inconsistent display encoding. */
        set(&l.path_text, pass == 1 ? "something-else" : "src/db.c");
        T_OK(atlas_decision_revision_add_link(&r, &l, &err), &err);
        atlas_decision_link_free(&l);
        hash_of(&r, pass == 0 ? a : (pass == 1 ? b : c));
        atlas_decision_revision_free(&r);
    }
    T_CHECK_MSG(strcmp(a, b) == 0, "a derived display encoding must not change the hash");
    T_CHECK_MSG(strcmp(a, c) != 0, "the raw path bytes must change the hash");
}

/* --- validation ------------------------------------------------------------- */

static void test_validation_rejects_control_characters_and_bad_utf8(void) {
    atlas_err err;
    atlas_err_init(&err);

    /* A NUL anywhere: a decision document is durable and human-read, and half a
     * title stored because of an embedded NUL is a silent truncation. */
    T_CHECK(atlas_decision_check_text("t", "a\0b", 3u, 100u, true, &err) != ATLAS_OK);

    /* An ANSI escape in a title. This is the payload test 18 is about: the
     * approval prompt shows the title, and an escape there could rewrite the
     * prompt around it. It is refused at the point of writing, so it never
     * reaches storage, let alone a terminal. */
    T_CHECK(atlas_decision_check_text("t", "\x1b[31mred", 9u, 100u, true, &err) != ATLAS_OK);
    T_CHECK(atlas_decision_check_text("t", "a\x07", 2u, 100u, true, &err) != ATLAS_OK);
    T_CHECK(atlas_decision_check_text("t", "a\x7f", 2u, 100u, true, &err) != ATLAS_OK);

    /* C1, which is two UTF-8 bytes and would survive a naive byte scan. */
    T_CHECK(atlas_decision_check_text("t", "a\xc2\x9b", 3u, 100u, true, &err) != ATLAS_OK);

    /* Invalid UTF-8: a lone continuation byte, a truncated sequence, an
     * overlong encoding of '/', and a surrogate. Each is a second spelling of
     * some byte sequence, and a canonical digest over non-canonical input is
     * not canonical. */
    T_CHECK(atlas_decision_check_text("t", "\x80", 1u, 100u, true, &err) != ATLAS_OK);
    T_CHECK(atlas_decision_check_text("t", "\xe2\x82", 2u, 100u, true, &err) != ATLAS_OK);
    T_CHECK(atlas_decision_check_text("t", "\xc0\xaf", 2u, 100u, true, &err) != ATLAS_OK);
    T_CHECK(atlas_decision_check_text("t", "\xed\xa0\x80", 3u, 100u, true, &err) != ATLAS_OK);

    /* Valid multi-byte UTF-8 is accepted: this is a bound on structure, not an
     * ASCII-only rule. */
    T_OK(atlas_decision_check_text("t", "caf\xc3\xa9 \xe2\x82\xac", 9u, 100u, true, &err), &err);

    /* A newline is a body character and not a title character. A multi-line
     * title in a confirmation display is the beginning of a forged prompt. */
    T_OK(atlas_decision_check_text("body", "one\ntwo", 7u, 100u, true, &err), &err);
    T_CHECK(atlas_decision_check_text("title", "one\ntwo", 7u, 100u, false, &err) != ATLAS_OK);

    /* Over the limit is refused rather than truncated. */
    T_CHECK(atlas_decision_check_text("t", "abcdef", 6u, 3u, true, &err) != ATLAS_OK);
}

static void test_revision_validation_requires_a_title_and_a_decision(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_decision_revision r;
    atlas_decision_revision_init(&r);
    T_CHECK(atlas_decision_revision_validate(&r, &err) != ATLAS_OK);
    set(&r.title, "T");
    T_CHECK(atlas_decision_revision_validate(&r, &err) != ATLAS_OK);
    set(&r.decision_text, "D");
    T_OK(atlas_decision_revision_validate(&r, &err), &err);
    atlas_decision_revision_free(&r);
}

static void test_alternatives_are_refused_past_the_ceiling(void) {
    /* Refused, not dropped. A decision that silently records three of five
     * alternatives claims the other two were never considered. */
    atlas_err err;
    atlas_err_init(&err);
    atlas_decision_revision r;
    basic(&r, "T", "D");
    for (int i = 0; i < ATLAS_DECISION_MAX_ALTERNATIVES; i++) {
        T_OK(atlas_decision_revision_add_alternative(&r, "x", 1u, &err), &err);
    }
    T_CHECK(atlas_decision_revision_add_alternative(&r, "one too many", 12u, &err) != ATLAS_OK);
    T_EQ_INT((int)r.alternative_count, ATLAS_DECISION_MAX_ALTERNATIVES);
    atlas_decision_revision_free(&r);
}

/* --- identifiers -------------------------------------------------------------- */

static void test_uid_shape_is_a_boundary(void) {
    /* The uid is the one decision-derived value the automatic context envelope
     * may carry, so its shape is checked rather than assumed. */
    atlas_err err;
    atlas_err_init(&err);
    char uid[ATLAS_DECISION_UID_MAX];
    T_OK(atlas_decision_uid_derive("ab12", 7, "2026-08-07T00:00:00Z", 0u, uid, sizeof(uid), &err),
         &err);
    T_CHECK_MSG(atlas_decision_uid_is_valid(uid), "derived uid %s must be valid", uid);
    T_CHECK(strncmp(uid, ATLAS_DECISION_UID_PREFIX, strlen(ATLAS_DECISION_UID_PREFIX)) == 0);

    /* 128 bits. These identifiers are exported and outlive the database that
     * minted them, so the width is part of the contract. */
    T_EQ_INT((int)ATLAS_DECISION_UID_HEX, 32);
    T_EQ_INT((int)strlen(uid), (int)(strlen(ATLAS_DECISION_UID_PREFIX) + 32u));

    T_CHECK(!atlas_decision_uid_is_valid(""));
    T_CHECK(!atlas_decision_uid_is_valid("atlas-dec-"));
    T_CHECK(!atlas_decision_uid_is_valid("atlas-dec-XYZ"));
    /* Mixed case is refused: the canonical form is lowercase, and accepting
     * both would make two spellings of one identifier. */
    T_CHECK(!atlas_decision_uid_is_valid("atlas-dec-0123456789ABCDEF0123456789abcdef"));
    /* One short, one long. */
    T_CHECK(!atlas_decision_uid_is_valid("atlas-dec-0123456789abcdef0123456789abcde"));
    T_CHECK(!atlas_decision_uid_is_valid("atlas-dec-0123456789abcdef0123456789abcdef0"));
    /* The old 16-hex form is no longer valid, which is what makes the widening
     * a real change rather than a cosmetic one. */
    T_CHECK(!atlas_decision_uid_is_valid("atlas-dec-0123456789abcdef"));
    T_CHECK(!atlas_decision_uid_is_valid("../../etc/passwd"));
    T_CHECK(!atlas_decision_uid_is_valid("atlas-dec-0123456789abcdef0123456789abcde\n"));
    {
        /* Grossly overlong input must be rejected on shape, not truncated to
         * something valid. */
        char overlong[512];
        memset(overlong, 'a', sizeof(overlong) - 1u);
        memcpy(overlong, ATLAS_DECISION_UID_PREFIX, strlen(ATLAS_DECISION_UID_PREFIX));
        overlong[sizeof(overlong) - 1u] = '\0';
        T_CHECK(!atlas_decision_uid_is_valid(overlong));
    }

    /* Every byte of a valid uid is one the envelope allowlist already permits,
     * which is why it may be reported automatically. Checked against the real
     * checker rather than against a copy of the allowlist. */
    T_CHECK(atlas_ai_context_is_bounded(uid, strlen(uid)));
}

static void test_uids_are_unique_across_identical_inputs(void) {
    /* **The derivation is no longer deterministic, and that is the point.**
     *
     * It used to be (root hash, row id, timestamp), which is reproducible — so
     * two machines indexing the same repository would mint the same identifier
     * for two unrelated decisions created in the same second. These ids are
     * durable, exported, and end up in databases that get merged and restored,
     * so the derivation now mixes in kernel entropy and the inputs alone no
     * longer determine the output. */
    atlas_err err;
    atlas_err_init(&err);
    enum { N = 64 };
    char uids[N][ATLAS_DECISION_UID_MAX];
    for (int i = 0; i < N; i++) {
        /* Identical inputs every time. */
        T_OK(atlas_decision_uid_derive("same-identity", 1, "2026-08-07T00:00:00Z", 0u, uids[i],
                                       sizeof(uids[i]), &err),
             &err);
        T_CHECK(atlas_decision_uid_is_valid(uids[i]));
    }
    for (int i = 0; i < N; i++) {
        for (int k = i + 1; k < N; k++) {
            T_CHECK_MSG(strcmp(uids[i], uids[k]) != 0,
                        "identical inputs produced the same uid twice (%d and %d): the derivation "
                        "is not mixing in entropy",
                        i, k);
        }
    }

    /* The attempt counter changes the result too, which is what makes the
     * collision retry in `atlas_db_decision_document_create` able to make
     * progress even if the entropy source were repeating. */
    char a[ATLAS_DECISION_UID_MAX];
    char b[ATLAS_DECISION_UID_MAX];
    T_OK(atlas_decision_uid_derive("x", 1, "t", 0u, a, sizeof(a), &err), &err);
    T_OK(atlas_decision_uid_derive("x", 1, "t", 1u, b, sizeof(b), &err), &err);
    T_CHECK(strcmp(a, b) != 0);

    /* A buffer that cannot hold the canonical form is an error, not a silent
     * truncation to something that would still pass a prefix check. */
    char tiny[8];
    T_CHECK(atlas_decision_uid_derive("x", 1, "t", 0u, tiny, sizeof(tiny), &err) != ATLAS_OK);
}

static void test_confirm_phrase_is_a_prefix_of_the_hash(void) {
    char phrase[ATLAS_DECISION_CONFIRM_MAX];
    const char *hash = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    atlas_decision_confirm_phrase(hash, phrase, sizeof(phrase));
    T_EQ_INT((int)strlen(phrase), (int)ATLAS_DECISION_CONFIRM_HEX);
    T_CHECK(strncmp(phrase, hash, ATLAS_DECISION_CONFIRM_HEX) == 0);

    /* A short or absent hash must not read past its end. */
    atlas_decision_confirm_phrase("ab", phrase, sizeof(phrase));
    T_CHECK(strcmp(phrase, "ab") == 0);
    atlas_decision_confirm_phrase(NULL, phrase, sizeof(phrase));
    T_CHECK(phrase[0] == '\0');
}

static void test_challenge_tokens_are_random_and_distinct(void) {
    atlas_err err;
    atlas_err_init(&err);
    char a[ATLAS_DECISION_CHALLENGE_HEX + 1u], b[ATLAS_DECISION_CHALLENGE_HEX + 1u];
    T_OK(atlas_decision_challenge_token(a, sizeof(a), &err), &err);
    T_OK(atlas_decision_challenge_token(b, sizeof(b), &err), &err);
    T_EQ_INT((int)strlen(a), (int)ATLAS_DECISION_CHALLENGE_HEX);
    T_CHECK_MSG(strcmp(a, b) != 0, "two challenge tokens must not be equal");
    for (size_t i = 0; i < strlen(a); i++) {
        bool hex = (a[i] >= '0' && a[i] <= '9') || (a[i] >= 'a' && a[i] <= 'f');
        T_CHECK(hex);
    }
}

static const atlas_test TESTS[] = {
    {"the transition table is the only authority", test_transition_table_is_the_only_authority},
    {"only a demand can be resolved", test_only_a_demand_can_be_resolved},
    {"vocabularies are closed", test_vocabularies_are_closed},
    {"the hash is stable and domain separated", test_hash_is_stable_and_domain_separated},
    {"no delimiter collision", test_hash_cannot_be_collided_by_moving_a_delimiter},
    {"every content field is hashed", test_every_content_field_changes_the_hash},
    {"link order does not matter", test_link_order_does_not_change_the_hash},
    {"alternative order does matter", test_alternative_order_does_change_the_hash},
    {"empty and absent alternatives differ", test_empty_and_absent_alternatives_differ},
    {"snapshot provenance is hashed", test_snapshot_provenance_is_hashed},
    {"the basis, repository and actor are hashed",
     test_the_revision_basis_and_repository_are_hashed},
    {"live currency is not hashed", test_live_currency_is_not_hashed},
    {"derived display encodings are not hashed", test_derived_display_encodings_are_not_hashed},
    {"validation refuses controls and bad UTF-8",
     test_validation_rejects_control_characters_and_bad_utf8},
    {"a revision needs a title and a decision",
     test_revision_validation_requires_a_title_and_a_decision},
    {"alternatives are refused past the ceiling", test_alternatives_are_refused_past_the_ceiling},
    {"the uid shape is a boundary", test_uid_shape_is_a_boundary},
    {"uids are unique across identical inputs",
     test_uids_are_unique_across_identical_inputs},
    {"the confirmation is a hash prefix", test_confirm_phrase_is_a_prefix_of_the_hash},
    {"challenge tokens are random", test_challenge_tokens_are_random_and_distinct},
};

ATLAS_TEST_MAIN("decision_model", TESTS)
