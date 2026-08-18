/* Atlas - A9.2.5: the verdict a semantic read carries, and what it may claim.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The claim this suite exists to hold:
 *
 *   **A SEMANTIC READ THAT FOUND NOTHING HAS NOT ESTABLISHED THAT THERE IS
 *   NOTHING.**
 *
 * A9.2.2 established that for claims and A9.2.3/A9.2.4 built the coverage and
 * discovery axes an absence would have to rest on. None of it reached the answer
 * to `callers of X`, which replied with its rows plus `{freshness, stale_reason,
 * generation_id, indexed_commit}` and stopped. `zero rows` and `zero rows over a
 * tree Atlas read a third of` were the same document.
 *
 * Two halves, and both are needed:
 *
 *   - `test_verdict_*` pin the pure decision. They are the fast ones, they cover
 *     every reason a read cannot settle, and they are what the CLI, the RPC
 *     server and the daemon all inherit by calling one function.
 *   - `test_e2e_*` drive the **built binary** over a fixture whose compilation
 *     database deliberately names a strict subset of its sources, and assert on
 *     the JSON a consumer actually receives. A unit test cannot catch a field
 *     that is computed correctly and never written to the document, and that is
 *     precisely the defect this season exists to fix.
 *
 * Everything here is synthetic: a fixture repository, a fixture compilation
 * database and an isolated data directory. Nothing reaches a live daemon, a live
 * socket, a real database or a registered repository.
 */
#include <stdio.h>
#include <string.h>

#include "atlas/sem.h"
#include "atlas_test.h"
#include "support/fixture.h"

/* --- the pure decision ------------------------------------------------------ */

/* A trust block describing a generation that has earned an absence: published,
 * current, every unit described, every candidate source covered, discovery
 * complete, and a repository somebody is maintaining.
 *
 * Every negative test below starts from this and breaks exactly one thing, so a
 * failure names the field that was not consulted rather than "something is
 * wrong". */
static void sufficient(atlas_sem_trust *t) {
    atlas_sem_trust_init(t);
    t->libclang_available = true;
    t->have_generation = true;
    t->generation_id = 7;
    (void)snprintf(t->indexed_commit, sizeof t->indexed_commit, "%s", "c0ffee");
    (void)snprintf(t->generation_identity, sizeof t->generation_identity, "%s", "identity-a");
    (void)snprintf(t->live_identity, sizeof t->live_identity, "%s", "identity-a");
    t->freshness = ATLAS_SEM_FRESH_CURRENT;
    t->stale_reason = NULL;
    t->units_complete = true;
    t->coverage_complete = true;
    t->scope_discovery = ATLAS_SEM_SCOPE_DECLARED;
    t->scope_candidates = 3;
    t->scope_covered = 3;
    t->scope_uncovered = 0;
    t->generation_discovery = ATLAS_SEM_DISC_COMPLETE;
    t->discovery = ATLAS_SEM_DISC_COMPLETE;
    t->inputs_accepted = 1;
    t->inputs_rejected = 0;
    t->auto_maintenance = true;
}

static void expect_unknown(atlas_sem_trust *t, const char *why, const char *what) {
    atlas_sem_trust_settle(t, 0, false);
    T_CHECK_MSG(t->verdict == ATLAS_SEM_VERDICT_UNKNOWN,
                "%s: expected UNKNOWN, got %s", what, atlas_sem_verdict_name(t->verdict));
    T_CHECK_MSG(t->unknown_reason != NULL && strcmp(t->unknown_reason, why) == 0,
                "%s: expected reason %s, got %s", what, why,
                t->unknown_reason == NULL ? "(null)" : t->unknown_reason);
    /* The reason must be Atlas' own literal, not merely a matching string —
     * every model-facing vocabulary in this layer is interned before it is
     * emitted, and one that is not would cross a socket unchecked. */
    T_CHECK_MSG(atlas_sem_unknown_reason_is_known(t->unknown_reason),
                "%s: the reason is not in Atlas' closed set", what);
}

static void test_verdict_zero_is_the_safe_reading(void) {
    /* A `memset` must not produce an absence proof — the rule every Atlas
     * vocabulary keeps its zero for, and the one that matters most here. */
    atlas_sem_trust t;
    memset(&t, 0xff, sizeof t);
    atlas_sem_trust_init(&t);
    T_CHECK(t.verdict == ATLAS_SEM_VERDICT_UNKNOWN);
    T_CHECK(t.unknown_reason == NULL);
    T_CHECK(!t.have_generation);
    T_CHECK(!t.coverage_complete);
    T_CHECK(!t.units_complete);
    T_CHECK(!t.auto_maintenance);
    T_CHECK(!t.libclang_available);
    T_CHECK(t.freshness == ATLAS_SEM_FRESH_ABSENT);
    T_CHECK(t.scope_discovery == ATLAS_SEM_SCOPE_UNKNOWN);
    T_CHECK(t.discovery == ATLAS_SEM_DISC_UNKNOWN);
    T_CHECK(t.generation_discovery == ATLAS_SEM_DISC_UNKNOWN);

    /* And a zeroed block settles to UNKNOWN rather than to an absence. */
    atlas_sem_trust_settle(&t, 0, false);
    T_CHECK(t.verdict == ATLAS_SEM_VERDICT_UNKNOWN);
}

static void test_verdict_vocabulary(void) {
    T_CHECK(strcmp(atlas_sem_verdict_name(ATLAS_SEM_VERDICT_UNKNOWN), "UNKNOWN") == 0);
    T_CHECK(strcmp(atlas_sem_verdict_name(ATLAS_SEM_VERDICT_PRESENT), "PRESENT") == 0);
    T_CHECK(strcmp(atlas_sem_verdict_name(ATLAS_SEM_VERDICT_ABSENT), "ABSENT") == 0);
    /* An out-of-range value names the safe answer, never the earned one. */
    T_CHECK(strcmp(atlas_sem_verdict_name((atlas_sem_verdict)99), "UNKNOWN") == 0);

    atlas_sem_verdict v = ATLAS_SEM_VERDICT_PRESENT;
    T_CHECK(atlas_sem_verdict_parse("ABSENT", &v) && v == ATLAS_SEM_VERDICT_ABSENT);
    T_CHECK(atlas_sem_verdict_parse("UNKNOWN", &v) && v == ATLAS_SEM_VERDICT_UNKNOWN);
    /* No fallback. Defaulting an unparsed verdict to ABSENT is the one error
     * that would matter, so parsing refuses instead. */
    v = ATLAS_SEM_VERDICT_PRESENT;
    T_CHECK(!atlas_sem_verdict_parse("absent", &v));
    T_CHECK(!atlas_sem_verdict_parse("", &v));
    T_CHECK(!atlas_sem_verdict_parse("NO", &v));
    T_CHECK(v == ATLAS_SEM_VERDICT_PRESENT); /* untouched on refusal */

    T_CHECK(atlas_sem_unknown_reason_is_known(ATLAS_SEM_UNK_COVERAGE));
    T_CHECK(atlas_sem_unknown_reason_is_known(ATLAS_SEM_UNK_DISCOVERY));
    T_CHECK(!atlas_sem_unknown_reason_is_known("something a repository wrote"));
    T_CHECK(!atlas_sem_unknown_reason_is_known(NULL));
    /* Interning returns Atlas' own storage, not the caller's bytes. */
    char copy[128];
    (void)snprintf(copy, sizeof copy, "%s", ATLAS_SEM_UNK_STALE);
    const char *in = atlas_sem_unknown_reason_intern(copy);
    T_CHECK(in != NULL && in != copy && strcmp(in, ATLAS_SEM_UNK_STALE) == 0);
}

static void test_verdict_sufficient_coverage_settles_absent(void) {
    /* The only combination that earns an absence. If this ever passes while one
     * of the tests below also passes, the gate has stopped gating. */
    atlas_sem_trust t;
    sufficient(&t);
    atlas_sem_trust_settle(&t, 0, false);
    T_CHECK_MSG(t.verdict == ATLAS_SEM_VERDICT_ABSENT,
                "a current, fully covered, fully discovered generation must settle ABSENT, got %s",
                atlas_sem_verdict_name(t.verdict));
    T_CHECK(t.unknown_reason == NULL);
}

static void test_verdict_one_row_is_present_whatever_the_coverage(void) {
    /* A9.2.2's asymmetry, and it is the whole reason PRESENT and ABSENT are not
     * one boolean: a caller Atlas *found* exists whatever it failed to look at.
     * Coverage bounds a negative conclusion and bounds nothing about a positive
     * one. */
    atlas_sem_trust t;
    sufficient(&t);
    t.freshness = ATLAS_SEM_FRESH_STALE;
    t.stale_reason = ATLAS_SEM_STALE_SOURCE;
    t.coverage_complete = false;
    t.units_complete = false;
    t.scope_uncovered = 42;
    t.generation_discovery = ATLAS_SEM_DISC_UNKNOWN;
    t.auto_maintenance = false;
    atlas_sem_trust_settle(&t, 1, false);
    T_CHECK_MSG(t.verdict == ATLAS_SEM_VERDICT_PRESENT,
                "one row must settle PRESENT even from a stale, uncovered generation, got %s",
                atlas_sem_verdict_name(t.verdict));
    T_CHECK(t.unknown_reason == NULL);
    /* The generation metadata survives the verdict, because positive rows from a
     * stale generation are evidence about the tree *that* generation described
     * and about no other. */
    T_CHECK(t.generation_id == 7);
    T_CHECK(t.freshness == ATLAS_SEM_FRESH_STALE);
    T_CHECK(t.stale_reason != NULL);

    /* A truncated walk that still emitted a row has found something. */
    sufficient(&t);
    atlas_sem_trust_settle(&t, 5, true);
    T_CHECK(t.verdict == ATLAS_SEM_VERDICT_PRESENT);
}

static void test_verdict_absent_index_cannot_settle(void) {
    atlas_sem_trust t;
    sufficient(&t);
    t.have_generation = false;
    t.freshness = ATLAS_SEM_FRESH_ABSENT;
    expect_unknown(&t, ATLAS_SEM_UNK_NO_GENERATION, "no generation");
}

static void test_verdict_stale_index_cannot_settle(void) {
    atlas_sem_trust t;
    sufficient(&t);
    t.freshness = ATLAS_SEM_FRESH_STALE;
    t.stale_reason = ATLAS_SEM_STALE_SOURCE;
    (void)snprintf(t.live_identity, sizeof t.live_identity, "%s", "identity-b");
    expect_unknown(&t, ATLAS_SEM_UNK_STALE, "stale generation");
}

static void test_verdict_rebuilding_index_cannot_settle(void) {
    /* REBUILDING is not STALE and the reasons differ in what an operator does:
     * one is "wait", the other is "something moved". */
    atlas_sem_trust t;
    sufficient(&t);
    t.freshness = ATLAS_SEM_FRESH_REBUILDING;
    expect_unknown(&t, ATLAS_SEM_UNK_BUILDING, "rebuilding");
}

static void test_verdict_incomplete_units_cannot_settle(void) {
    atlas_sem_trust t;
    sufficient(&t);
    t.units_complete = false;
    t.coverage_complete = false;
    expect_unknown(&t, ATLAS_SEM_UNK_UNITS, "a unit was not fully described");
}

static void test_verdict_uncovered_sources_cannot_settle(void) {
    /* The GAP-1 shape, as a pure decision: every unit the compilation database
     * named was parsed, and the database named two of three sources. */
    atlas_sem_trust t;
    sufficient(&t);
    t.coverage_complete = false;
    t.scope_candidates = 3;
    t.scope_covered = 1;
    t.scope_uncovered = 2;
    expect_unknown(&t, ATLAS_SEM_UNK_COVERAGE, "sources outside the compilation database");
}

static void test_verdict_unknown_scope_cannot_settle(void) {
    /* Every generation built before A9.2.3 reads here, and is conservative for
     * free rather than by a rule that says so. */
    atlas_sem_trust t;
    sufficient(&t);
    t.scope_discovery = ATLAS_SEM_SCOPE_UNKNOWN;
    t.coverage_complete = false;
    expect_unknown(&t, ATLAS_SEM_UNK_SCOPE_UNKNOWN, "no coverage manifest");
}

static void test_verdict_partial_discovery_cannot_settle(void) {
    /* A9.2.4's sentence, enforced one layer out: complete processing of
     * configured inputs does not prove complete discovery of relevant inputs. */
    atlas_sem_trust t;
    sufficient(&t);
    t.generation_discovery = ATLAS_SEM_DISC_PARTIAL;
    t.coverage_complete = false;
    expect_unknown(&t, ATLAS_SEM_UNK_DISCOVERY, "discovery PARTIAL");

    sufficient(&t);
    t.generation_discovery = ATLAS_SEM_DISC_UNKNOWN;
    t.coverage_complete = false;
    expect_unknown(&t, ATLAS_SEM_UNK_DISCOVERY, "discovery UNKNOWN");
}

static void test_verdict_the_verdict_rests_on_the_generations_discovery(void) {
    /* `discovery` is what Atlas can account for *now*; `generation_discovery` is
     * what the index being served was built under. The verdict must rest on the
     * second: a walk that has since completed says nothing about a generation
     * built while it had not. */
    atlas_sem_trust t;
    sufficient(&t);
    t.generation_discovery = ATLAS_SEM_DISC_PARTIAL;
    t.discovery = ATLAS_SEM_DISC_COMPLETE; /* the walk improved after the build */
    t.coverage_complete = false;
    expect_unknown(&t, ATLAS_SEM_UNK_DISCOVERY, "a later complete walk must not settle an older generation");
}

static void test_verdict_disabled_maintenance_cannot_settle(void) {
    /* A repository nobody maintains drifts, and freshness is only ever a
     * statement about the instant it was computed. Reported as its own reason
     * because the remedy is an operator's `--auto`, not a rebuild. */
    atlas_sem_trust t;
    sufficient(&t);
    t.auto_maintenance = false;
    expect_unknown(&t, ATLAS_SEM_UNK_MAINTENANCE, "automatic maintenance disabled");
}

static void test_verdict_no_libclang_cannot_settle(void) {
    atlas_sem_trust t;
    sufficient(&t);
    t.libclang_available = false;
    expect_unknown(&t, ATLAS_SEM_UNK_NO_LIBCLANG, "built without libclang");
}

static void test_verdict_a_truncated_empty_walk_cannot_settle(void) {
    /* A8-CI's rule about every bound that is reached, applied to the verdict: a
     * walk that stopped at a ceiling has not searched its universe, so it cannot
     * report that the universe was empty. */
    atlas_sem_trust t;
    sufficient(&t);
    atlas_sem_trust_settle(&t, 0, true);
    T_CHECK_MSG(t.verdict == ATLAS_SEM_VERDICT_UNKNOWN,
                "a truncated walk that emitted nothing must not settle ABSENT, got %s",
                atlas_sem_verdict_name(t.verdict));
    T_CHECK(t.unknown_reason != NULL &&
            strcmp(t.unknown_reason, ATLAS_SEM_UNK_TRUNCATED) == 0);
}

static void test_verdict_reason_precedence_is_the_most_actionable(void) {
    /* Several things wrong at once must name the one an operator would fix
     * first. A repository with no index at all is not told about its coverage
     * manifest. */
    atlas_sem_trust t;
    sufficient(&t);
    t.libclang_available = false;
    t.have_generation = false;
    t.freshness = ATLAS_SEM_FRESH_ABSENT;
    t.coverage_complete = false;
    t.scope_uncovered = 9;
    expect_unknown(&t, ATLAS_SEM_UNK_NO_LIBCLANG, "no libclang outranks everything");

    sufficient(&t);
    t.have_generation = false;
    t.freshness = ATLAS_SEM_FRESH_ABSENT;
    t.coverage_complete = false;
    t.scope_uncovered = 9;
    expect_unknown(&t, ATLAS_SEM_UNK_NO_GENERATION, "no generation outranks coverage");

    sufficient(&t);
    t.freshness = ATLAS_SEM_FRESH_STALE;
    t.stale_reason = ATLAS_SEM_STALE_COMMIT;
    t.coverage_complete = false;
    t.scope_uncovered = 9;
    expect_unknown(&t, ATLAS_SEM_UNK_STALE, "staleness outranks coverage");
}

/* --- A9.2.5 / GAP-8: transient failure is not permanent coverage loss --------
 *
 * `tu_failed > 0` makes a generation's coverage incomplete for ever, because the
 * retry governor compares *identities* and identical bytes never retry. Before
 * this season a parse child that was OOM-killed therefore cost a repository the
 * ability to state an absence until somebody happened to edit a file, and
 * nothing anywhere recorded that this had happened. */

static void test_only_a_machine_failure_is_transient(void) {
    /* The classification, and it is closed on purpose. Everything except these
     * two is a property of the input: retrying it spends a compiler run to reach
     * the same answer, which is the storm the bound exists to prevent. */
    T_CHECK(atlas_sem_why_is_transient(ATLAS_SEM_WHY_CHILD_FAILED));
    T_CHECK(atlas_sem_why_is_transient(ATLAS_SEM_WHY_TIMEOUT));

    T_CHECK(!atlas_sem_why_is_transient(ATLAS_SEM_WHY_PARSE_ERROR));
    T_CHECK(!atlas_sem_why_is_transient(ATLAS_SEM_WHY_NO_TU));
    T_CHECK(!atlas_sem_why_is_transient(ATLAS_SEM_WHY_OUTSIDE_REPO));
    T_CHECK(!atlas_sem_why_is_transient(ATLAS_SEM_WHY_ARG_REFUSED));
    T_CHECK(!atlas_sem_why_is_transient(ATLAS_SEM_WHY_TOO_LARGE));
    T_CHECK(!atlas_sem_why_is_transient(ATLAS_SEM_WHY_MISSING_FILE));
    T_CHECK(!atlas_sem_why_is_transient(NULL));
    T_CHECK(!atlas_sem_why_is_transient("something a repository wrote"));

    /* Both whole-pass reasons are known values, and they are different values —
     * the governor's exception depends on being able to tell them apart. */
    T_CHECK(atlas_sem_why_is_known(ATLAS_SEM_WHY_PASS_FAILED));
    T_CHECK(atlas_sem_why_is_known(ATLAS_SEM_WHY_PASS_INTERRUPTED));
    T_CHECK(strcmp(ATLAS_SEM_WHY_PASS_FAILED, ATLAS_SEM_WHY_PASS_INTERRUPTED) != 0);
    /* A whole-pass reason is not a *unit* reason and must never be treated as
     * one: the per-unit retry loop asks `atlas_sem_why_is_transient`, and an
     * interrupted pass is not a unit that can be re-parsed. */
    T_CHECK(!atlas_sem_why_is_transient(ATLAS_SEM_WHY_PASS_INTERRUPTED));
}

static void test_the_unit_retry_bound_is_compile_time_and_small(void) {
    /* The bound is the storm guarantee, so it is asserted rather than trusted:
     * it must be a small positive constant, per unit and per pass. Nothing
     * durable records a retry, so a restart has no half-finished state to
     * interpret, and no timer exists that could wake up and try again. */
    T_CHECK_MSG(ATLAS_SEM_UNIT_TRANSIENT_RETRIES >= 1,
                "a transient unit failure must get at least one further attempt");
    T_CHECK_MSG(ATLAS_SEM_UNIT_TRANSIENT_RETRIES <= 2,
                "the retry bound must stay small: these failures are memory pressure and load, "
                "and more attempts spend compiler runs on a machine already short of memory");

    /* And the pass reports what it spent, so a recovered failure is visible
     * rather than silently absorbed. */
    atlas_sem_index_summary sum;
    memset(&sum, 0xff, sizeof sum);
    atlas_sem_index_summary_init(&sum);
    T_CHECK(sum.units_retried == 0);
}

/* --- A9.2.5 / GAP-7: repository identity ------------------------------------- */

static void test_a_different_repository_identity_is_stale(void) {
    /* `repo_identity_hash` has been written onto every generation since A8-CI
     * and compared by nothing. `src/gate/assess.c` compares exactly this value
     * and revalidates on a mismatch; the semantic layer wrote it and forgot it.
     *
     * The source identity cannot stand in for it: it is built from repository-
     * *relative* paths and content hashes, so a tree with identical content
     * under a different canonical root produces an identical value. */
    atlas_sem_generation g;
    atlas_sem_generation_init(&g);
    g.status = ATLAS_SEM_GEN_COMPLETE;
    (void)snprintf(g.repo_identity_hash, sizeof g.repo_identity_hash, "%s", "lineage-a");
    (void)snprintf(g.analyzer_id, sizeof g.analyzer_id, "%s", ATLAS_SEM_ANALYZER_ID);
    g.analyzer_version = ATLAS_SEM_ANALYZER_VERSION;

    const char *reason = NULL;
    T_CHECK(atlas_sem_freshness_of(&g, true, false, "", "lineage-b", "", "", NULL, true,
                                   &reason) == ATLAS_SEM_FRESH_STALE);
    T_CHECK_MSG(reason != NULL && strcmp(reason, ATLAS_SEM_STALE_REPO_IDENTITY) == 0,
                "expected the repository-identity reason, got %s",
                reason == NULL ? "(null)" : reason);
    T_CHECK(atlas_sem_stale_reason_is_known(reason));

    /* The same identity is not stale. */
    reason = NULL;
    T_CHECK(atlas_sem_freshness_of(&g, true, false, "", "lineage-a", "", "", NULL, true,
                                   &reason) == ATLAS_SEM_FRESH_CURRENT);

    /* Neither empty value is evidence of change: an empty stored identity is a
     * generation built before this was recorded, and an empty live one is Atlas
     * not having looked. Treating either as a change would make every
     * pre-A9.2.5 generation stale for a reason nobody could act on. */
    reason = NULL;
    T_CHECK(atlas_sem_freshness_of(&g, true, false, "", "", "", "", NULL, true, &reason) ==
            ATLAS_SEM_FRESH_CURRENT);
    atlas_sem_generation blank;
    atlas_sem_generation_init(&blank);
    blank.status = ATLAS_SEM_GEN_COMPLETE;
    (void)snprintf(blank.analyzer_id, sizeof blank.analyzer_id, "%s", ATLAS_SEM_ANALYZER_ID);
    blank.analyzer_version = ATLAS_SEM_ANALYZER_VERSION;
    reason = NULL;
    T_CHECK(atlas_sem_freshness_of(&blank, true, false, "", "lineage-b", "", "", NULL, true,
                                   &reason) == ATLAS_SEM_FRESH_CURRENT);
}

static void test_repository_identity_outranks_a_moved_commit(void) {
    /* "This index describes a different repository" outranks "this index
     * describes an older commit of the same one", and an operator told the
     * second when the first is true looks in the wrong place. */
    atlas_sem_generation g;
    atlas_sem_generation_init(&g);
    g.status = ATLAS_SEM_GEN_COMPLETE;
    (void)snprintf(g.repo_identity_hash, sizeof g.repo_identity_hash, "%s", "lineage-a");
    (void)snprintf(g.commit_id, sizeof g.commit_id, "%s", "commit-a");
    (void)snprintf(g.analyzer_id, sizeof g.analyzer_id, "%s", ATLAS_SEM_ANALYZER_ID);
    g.analyzer_version = ATLAS_SEM_ANALYZER_VERSION;

    const char *reason = NULL;
    T_CHECK(atlas_sem_freshness_of(&g, true, false, "commit-b", "lineage-b", "", "", NULL, true,
                                   &reason) == ATLAS_SEM_FRESH_STALE);
    T_CHECK_MSG(reason != NULL && strcmp(reason, ATLAS_SEM_STALE_REPO_IDENTITY) == 0,
                "the repository identity must be reported before the commit, got %s",
                reason == NULL ? "(null)" : reason);

    /* Same repository, moved commit: the commit reason, unchanged from A8-CI. */
    reason = NULL;
    T_CHECK(atlas_sem_freshness_of(&g, true, false, "commit-b", "lineage-a", "", "", NULL, true,
                                   &reason) == ATLAS_SEM_FRESH_STALE);
    T_CHECK(reason != NULL && strcmp(reason, ATLAS_SEM_STALE_COMMIT) == 0);
}

/* --- the end-to-end contract ------------------------------------------------
 *
 * Drives the built binary, because a field that is computed correctly and never
 * written to the document is exactly the defect this season fixes, and no unit
 * test can see it. */

typedef struct e2e {
    fixture fx;
} e2e;

static void run(e2e *E, const char *const *args, size_t n, atlas_buf *out, int *code,
                atlas_err *err) {
    /* Every invocation carries the fixture data directory. `fx_atlas` does not
     * add it, and a test without it opens the developer's real database. */
    const char *full[24];
    T_REQUIRE_MSG(n + 2u <= 24u, "too many arguments");
    full[0] = "--data-dir";
    full[1] = fx_data_dir(&E->fx);
    for (size_t i = 0; i < n; i++) {
        full[i + 2u] = args[i];
    }
    /* stderr is appended to `out` rather than discarded. A command that fails
     * here writes its reason there and nowhere else, and a test that reported
     * only "exited 1" cost an hour under a sanitizer build once already. */
    atlas_buf se = ATLAS_BUF_INIT;
    T_OK(fx_atlas(full, n + 2u, out, &se, code, err), err);
    if (se.len > 0 && out != NULL) {
        (void)atlas_buf_appendf(out, err, " [stderr: %s]", atlas_buf_cstr(&se));
    }
    atlas_buf_free(&se);
}

static void run_ok(e2e *E, const char *const *args, size_t n, atlas_buf *out, atlas_err *err) {
    int code = -1;
    atlas_buf local = ATLAS_BUF_INIT;
    run(E, args, n, out != NULL ? out : &local, &code, err);
    T_CHECK_MSG(code == 0, "`atlas %s ...` exited %d: %s", args[0], code,
                atlas_buf_cstr(out != NULL ? out : &local));
    atlas_buf_free(&local);
}

/* The compilation database, naming exactly the sources given. The whole point of
 * the fixture is that this is a *strict subset* of what the tree holds. */
static void write_compdb(e2e *E, const char *const *sources, size_t n, atlas_err *err) {
    atlas_buf doc = ATLAS_BUF_INIT;
    T_OK(atlas_buf_append_str(&doc, "[", err), err);
    for (size_t i = 0; i < n; i++) {
        T_OK(atlas_buf_appendf(&doc, err,
                               "%s{\"directory\":\"%s\","
                               "\"arguments\":[\"cc\",\"-std=gnu11\",\"-c\",\"%s\"],"
                               "\"file\":\"%s\"}",
                               i == 0 ? "" : ",", fx_repo(&E->fx), sources[i], sources[i]),
             err);
    }
    T_OK(atlas_buf_append_str(&doc, "]", err), err);
    T_OK(fx_write(fx_repo(&E->fx), "compile_commands.json", atlas_buf_cstr(&doc), err), err);
    atlas_buf_free(&doc);
}

/* A repository with a caller that lives outside the compilation database.
 *
 * `nodus/tests/deep.c` calls `orphan`, and the first compilation database names
 * only `src/lib.c`. That is the shape the season was bought with: a PROVEN caller
 * that no amount of correct processing of the configured inputs can see. */
static void e2e_open(e2e *E, atlas_err *err) {
    memset(E, 0, sizeof(*E));
    T_OK(fx_open(&E->fx, err), err);
    T_OK(fx_init_repo(&E->fx, fx_repo(&E->fx), NULL, err), err);
    T_OK(fx_mkdir(fx_repo(&E->fx), "src", err), err);
    T_OK(fx_mkdir(fx_repo(&E->fx), "nodus", err), err);
    T_OK(fx_mkdir(fx_repo(&E->fx), "nodus/tests", err), err);
    T_OK(fx_write(fx_repo(&E->fx), "src/lib.h",
                  "int orphan(int x);\nint entry(int x);\nint never_called_anywhere(int x);\n",
                  err),
         err);
    T_OK(fx_write(fx_repo(&E->fx), "src/lib.c",
                  "#include \"lib.h\"\n"
                  "int orphan(int x) { return x * 2; }\n"
                  "int never_called_anywhere(int x) { return x - 1; }\n"
                  "int entry(int x) { return x + 1; }\n",
                  err),
         err);
    T_OK(fx_write(fx_repo(&E->fx), "nodus/tests/deep.c",
                  "#include \"../../src/lib.h\"\n"
                  "int deep_caller(void) { return orphan(3); }\n",
                  err),
         err);

    const char *only_lib[] = {"src/lib.c"};
    write_compdb(E, only_lib, 1u, err);
    T_OK(fx_add_all(&E->fx, fx_repo(&E->fx), err), err);
    T_OK(fx_commit(&E->fx, fx_repo(&E->fx), "first", err), err);

    const char *add[] = {"repo", "add", fx_repo(&E->fx), "--name", "fixture"};
    run_ok(E, add, 5u, NULL, err);
    const char *scan[] = {"scan", "fixture"};
    run_ok(E, scan, 2u, NULL, err);
    const char *sync[] = {"code", "sync", "fixture"};
    run_ok(E, sync, 3u, NULL, err);
    /* `--auto` so the repository is one somebody maintains; without it the
     * verdict would be UNKNOWN for that reason and the coverage case this test
     * exists for would never be reached. */
    const char *cfg[] = {"code",    "sem-config",            "fixture",
                         "--compdb", "compile_commands.json", "--auto"};
    run_ok(E, cfg, 6u, NULL, err);
    const char *idx[] = {"code", "index", "fixture"};
    run_ok(E, idx, 3u, NULL, err);
}

static void e2e_close(e2e *E) { fx_close(&E->fx); }

static bool has(const atlas_buf *b, const char *needle) {
    return strstr(atlas_buf_cstr(b), needle) != NULL;
}

static void test_e2e_zero_rows_over_partial_coverage_is_unknown(void) {
    atlas_err err;
    atlas_err_init(&err);
    e2e E;
    e2e_open(&E, &err);

    /* The state the fixture is in, asserted rather than assumed — a test that
     * did not check this could pass for the wrong reason. */
    atlas_buf status = ATLAS_BUF_INIT;
    const char *st[] = {"code", "sem-status", "fixture", "--json"};
    run_ok(&E, st, 4u, &status, &err);
    T_CHECK_MSG(has(&status, "\"scope_uncovered\":1"),
                "the fixture must have exactly one uncovered source: %s", atlas_buf_cstr(&status));

    /* `orphan` has a PROVEN caller in `nodus/tests/deep.c`, which the
     * compilation database does not name. The walk finds nothing, and nothing
     * it finds is evidence that nothing exists. */
    atlas_buf out = ATLAS_BUF_INIT;
    const char *q[] = {"code", "callers", "fixture", "orphan", "--json"};
    run_ok(&E, q, 5u, &out, &err);
    T_CHECK_MSG(has(&out, "\"nodes\":[]"), "expected an empty result set: %s",
                atlas_buf_cstr(&out));
    T_CHECK_MSG(has(&out, "\"result_verdict\":\"UNKNOWN\""),
                "a zero-row answer over partial coverage must carry UNKNOWN: %s",
                atlas_buf_cstr(&out));
    T_CHECK_MSG(has(&out, ATLAS_SEM_UNK_COVERAGE),
                "the answer must say why it could not settle: %s", atlas_buf_cstr(&out));
    /* The coverage numbers travel with the verdict, so a reader can act on it
     * without a second call to a different command. */
    T_CHECK_MSG(has(&out, "\"scope_uncovered\":1"),
                "the coverage manifest must travel with the answer: %s", atlas_buf_cstr(&out));

    atlas_buf_free(&out);
    atlas_buf_free(&status);
    e2e_close(&E);
}

static void test_e2e_full_coverage_settles_absent_and_present(void) {
    atlas_err err;
    atlas_err_init(&err);
    e2e E;
    e2e_open(&E, &err);

    /* Widen the compilation database to the whole tree and rebuild. No source
     * byte changes; only what Atlas was told to read. */
    const char *all[] = {"src/lib.c", "nodus/tests/deep.c"};
    write_compdb(&E, all, 2u, &err);
    T_OK(fx_add_all(&E.fx, fx_repo(&E.fx), &err), &err);
    T_OK(fx_commit(&E.fx, fx_repo(&E.fx), "full compdb", &err), &err);
    const char *scan[] = {"scan", "fixture"};
    run_ok(&E, scan, 2u, NULL, &err);
    const char *sync[] = {"code", "sync", "fixture"};
    run_ok(&E, sync, 3u, NULL, &err);
    const char *idx[] = {"code", "index", "fixture"};
    run_ok(&E, idx, 3u, NULL, &err);

    /* The caller was there the whole time. */
    atlas_buf out = ATLAS_BUF_INIT;
    const char *q[] = {"code", "callers", "fixture", "orphan", "--json"};
    run_ok(&E, q, 5u, &out, &err);
    T_CHECK_MSG(has(&out, "deep_caller"),
                "the caller the first index could not see must now be found: %s",
                atlas_buf_cstr(&out));
    T_CHECK_MSG(has(&out, "\"result_verdict\":\"PRESENT\""),
                "a found row must settle PRESENT: %s", atlas_buf_cstr(&out));
    atlas_buf_free(&out);

    /* And a symbol nothing calls, over a tree Atlas has now read completely, is
     * the one case that earns ABSENT. */
    atlas_buf out2 = ATLAS_BUF_INIT;
    const char *q2[] = {"code", "callers", "fixture", "never_called_anywhere", "--json"};
    run_ok(&E, q2, 5u, &out2, &err);
    T_CHECK_MSG(has(&out2, "\"nodes\":[]"), "expected an empty result set: %s",
                atlas_buf_cstr(&out2));
    T_CHECK_MSG(has(&out2, "\"result_verdict\":\"ABSENT\""),
                "complete coverage over a current generation must settle ABSENT: %s",
                atlas_buf_cstr(&out2));
    atlas_buf_free(&out2);

    e2e_close(&E);
}

static void test_e2e_a_symbol_outside_the_index_is_not_a_usage_error(void) {
    /* "You asked for something that does not exist" and "Atlas did not read the
     * file it is in" are different claims, and exit 2 — the operator-typo class
     * — merges them. */
    atlas_err err;
    atlas_err_init(&err);
    e2e E;
    e2e_open(&E, &err);

    atlas_buf out = ATLAS_BUF_INIT;
    int code = -1;
    const char *q[] = {"code", "callers", "fixture", "deep_caller", "--json"};
    run(&E, q, 5u, &out, &code, &err);
    T_CHECK_MSG(code != 2, "a symbol in an uncovered file must not be a usage error: %s",
                atlas_buf_cstr(&out));
    T_CHECK_MSG(has(&out, "\"result_verdict\":\"UNKNOWN\""),
                "a symbol Atlas never read must answer UNKNOWN: %s", atlas_buf_cstr(&out));
    atlas_buf_free(&out);
    e2e_close(&E);
}

/* Every key the trust block carries. If a surface is missing one, the answer it
 * gives cannot be reasoned about the way the others can. */
static const char *const TRUST_KEYS[] = {
    "\"result_verdict\"",     "\"freshness\"",           "\"have_generation\"",
    "\"generation_id\"",      "\"generation_identity\"", "\"live_identity\"",
    "\"coverage_complete\"",  "\"units_complete\"",      "\"scope_discovery\"",
    "\"scope_candidates\"",   "\"scope_covered\"",       "\"scope_uncovered\"",
    "\"generation_discovery\"", "\"discovery\"",         "\"inputs_accepted\"",
    "\"inputs_rejected\"",    "\"auto_maintenance\"",    "\"libclang_available\"",
};

/* Keys that exist only in the **root** object of a semantic document. The nested
 * `generation` object legitimately reuses `scope_*` and `discovery`, so those are
 * excluded and a substring scan over these is sound.
 *
 * A duplicate is not untidiness: `atlas_ipc_result_str` reads through
 * `yyjson_obj_get`, a linear scan returning the **first** match, while jq and
 * Python's `json` take the last. `sem-status` carried fourteen duplicated keys
 * until A9.2.5's own review found them. */
static const char *const ROOT_ONLY_KEYS[] = {
    "\"result_verdict\"",      "\"unknown_reason\"",       "\"freshness\"",
    "\"stale_reason\"",        "\"have_generation\"",      "\"generation_id\"",
    "\"generation_identity\"", "\"live_identity\"",        "\"coverage_complete\"",
    "\"units_complete\"",      "\"generation_discovery\"", "\"inputs_accepted\"",
    "\"inputs_rejected\"",     "\"auto_maintenance\"",     "\"libclang_available\"",
};

static void check_no_duplicate_root_keys(const atlas_buf *doc, const char *surface) {
    for (size_t i = 0; i < sizeof ROOT_ONLY_KEYS / sizeof ROOT_ONLY_KEYS[0]; i++) {
        const char *first = strstr(atlas_buf_cstr(doc), ROOT_ONLY_KEYS[i]);
        T_REQUIRE_MSG(first != NULL, "%s is missing %s", surface, ROOT_ONLY_KEYS[i]);
        T_CHECK_MSG(strstr(first + 1, ROOT_ONLY_KEYS[i]) == NULL,
                    "%s carries %s twice; Atlas' remote parser reads the first occurrence and "
                    "jq reads the last: %s",
                    surface, ROOT_ONLY_KEYS[i], atlas_buf_cstr(doc));
    }
}

/* Copies the value that follows `"key":` into `out`, up to the next `,` or `}`.
 * Enough to compare two documents field by field without a JSON parser. */
static bool value_of(const atlas_buf *doc, const char *quoted_key, char *out, size_t cap) {
    const char *p = strstr(atlas_buf_cstr(doc), quoted_key);
    if (p == NULL) {
        return false;
    }
    p += strlen(quoted_key);
    if (*p != ':') {
        return false;
    }
    p++;
    size_t n = 0;
    bool in_str = false;
    while (*p != '\0' && n + 1u < cap) {
        if (*p == '"') {
            in_str = !in_str;
        } else if (!in_str && (*p == ',' || *p == '}')) {
            break;
        }
        out[n++] = *p++;
    }
    out[n] = '\0';
    return true;
}

/* Every trust field must hold the same value on both documents. This is the
 * assertion the season's parity claim actually rests on: the two serializers
 * agreeing on which keys exist is not the same as agreeing on what they say. */
static void check_same_trust_values(const atlas_buf *a, const atlas_buf *b, const char *what) {
    for (size_t i = 0; i < sizeof TRUST_KEYS / sizeof TRUST_KEYS[0]; i++) {
        char va[512];
        char vb[512];
        bool ga = value_of(a, TRUST_KEYS[i], va, sizeof va);
        bool gb = value_of(b, TRUST_KEYS[i], vb, sizeof vb);
        T_CHECK_MSG(ga && gb, "%s: %s missing from one side (local=%d socket=%d)", what,
                    TRUST_KEYS[i], (int)ga, (int)gb);
        if (ga && gb) {
            T_CHECK_MSG(strcmp(va, vb) == 0, "%s: %s differs — local %s, socket %s", what,
                        TRUST_KEYS[i], va, vb);
        }
    }
}

static void check_trust_block(const atlas_buf *doc, const char *surface) {
    for (size_t i = 0; i < sizeof TRUST_KEYS / sizeof TRUST_KEYS[0]; i++) {
        T_CHECK_MSG(has(doc, TRUST_KEYS[i]), "%s is missing %s from its trust block: %s",
                    surface, TRUST_KEYS[i], atlas_buf_cstr(doc));
    }
}

static void test_e2e_every_load_bearing_surface_carries_the_trust_block(void) {
    /* **Seven surfaces, one block.** The requirement is not that each one
     * computes a correct verdict — it is that a consumer can read any of them
     * the same way. Before A9.2.5 the CLI renderer and the IPC server were two
     * independently maintained serializers and had already drifted over
     * `have_generation`; nothing but a test that enumerates the keys can hold
     * seven surfaces to one shape.
     *
     * This drives the CLI, which under a per-user data directory is the *local*
     * path. The remote path is the same service report through the same writer;
     * `docs/semantic-trust.md` records the runtime comparison across CLI, RPC
     * and MCP that confirms it field for field. */
    atlas_err err;
    atlas_err_init(&err);
    e2e E;
    e2e_open(&E, &err);

    struct {
        const char *name;
        const char *args[6];
        size_t n;
    } CASES[] = {
        {"code sem-status", {"code", "sem-status", "fixture", "--json"}, 4u},
        {"code semantic", {"code", "semantic", "fixture", "orphan", "--json"}, 5u},
        {"code callers", {"code", "callers", "fixture", "orphan", "--json"}, 5u},
        {"code callees", {"code", "callees", "fixture", "entry", "--json"}, 5u},
        {"code sem-impact", {"code", "sem-impact", "fixture", "orphan", "--json"}, 5u},
        {"code tests", {"code", "tests", "fixture", "orphan", "--json"}, 5u},
        {"code explain", {"code", "explain", "fixture", "orphan", "--json"}, 5u},
        {"code trace", {"code", "trace", "fixture", "orphan", "entry", "--json"}, 6u},
        {"context build",
         {"context", "build", "--repo", "fixture", "--task", "orphan"},
         6u},
    };

    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        atlas_buf out = ATLAS_BUF_INIT;
        int code = -1;
        /* `context build` needs `--json` too; it is appended by the table above
         * for the others. A non-zero exit is itself a failure worth naming. */
        const char *args[8];
        size_t n = CASES[i].n;
        for (size_t k = 0; k < n; k++) {
            args[k] = CASES[i].args[k];
        }
        if (strcmp(CASES[i].name, "context build") == 0) {
            args[n++] = "--json";
        }
        run(&E, args, n, &out, &code, &err);
        T_CHECK_MSG(code == 0, "%s exited %d: %s", CASES[i].name, code, atlas_buf_cstr(&out));
        if (code == 0) {
            check_trust_block(&out, CASES[i].name);
            check_no_duplicate_root_keys(&out, CASES[i].name);
        }
        atlas_buf_free(&out);
    }
    e2e_close(&E);
}

static void test_e2e_a_repository_with_no_index_still_carries_the_block(void) {
    /* **The path an early return is most likely to skip, and did.**
     *
     * Moving `freshness` and `generation_id` into the trust block left the RPC
     * `sem.status` method's "no generation" branch emitting neither: a freshly
     * registered repository reported no currency at all over the socket, which
     * `tests/test_registry.c` caught and this suite did not — because every
     * fixture here indexes first.
     *
     * A repository Atlas has never indexed is precisely where a caller most
     * needs to be told what the answer is worth, so it is pinned here now. */
    atlas_err err;
    atlas_err_init(&err);
    e2e E;
    memset(&E, 0, sizeof E);
    T_OK(fx_open(&E.fx, &err), &err);
    T_OK(fx_init_repo(&E.fx, fx_repo(&E.fx), NULL, &err), &err);
    T_OK(fx_write(fx_repo(&E.fx), "a.c", "int f(void){return 0;}\n", &err), &err);
    T_OK(fx_add_all(&E.fx, fx_repo(&E.fx), &err), &err);
    T_OK(fx_commit(&E.fx, fx_repo(&E.fx), "first", &err), &err);

    const char *add[] = {"repo", "add", fx_repo(&E.fx), "--name", "fixture"};
    run_ok(&E, add, 5u, NULL, &err);
    /* Deliberately no `scan`, no `code sync`, no `code index`. */

    atlas_buf out = ATLAS_BUF_INIT;
    const char *st[] = {"code", "sem-status", "fixture", "--json"};
    run_ok(&E, st, 4u, &out, &err);
    check_trust_block(&out, "code sem-status (never indexed)");
    T_CHECK_MSG(has(&out, "\"result_verdict\":\"UNKNOWN\""),
                "a repository with no index must answer UNKNOWN: %s", atlas_buf_cstr(&out));
    T_CHECK_MSG(has(&out, ATLAS_SEM_UNK_NO_GENERATION),
                "and must say that nothing has been published: %s", atlas_buf_cstr(&out));
    T_CHECK_MSG(has(&out, "\"have_generation\":false"),
                "have_generation must be present and false: %s", atlas_buf_cstr(&out));
    atlas_buf_free(&out);
    e2e_close(&E);
}

static void test_rpc_answers_exactly_as_the_local_path_does(void) {
    /* **The surface the local tests structurally cannot see.**
     *
     * Every other `test_e2e_*` here drives the CLI against a per-user data
     * directory, which is the *local writer* path. Under A7.1 the index is 0700
     * `atlasd`, so on a deployed machine every operator invocation and every MCP
     * tool call goes over the socket instead — and the socket has its own
     * serializer and its own symbol resolver.
     *
     * They had already diverged. `resolve_one` was taught that "no symbol by that
     * name" is an ordinary empty answer; its RPC twin `one_usr` was not, so the
     * season's headline rule was true locally and false on the only surface a
     * deployed operator and every model actually use. A substring check over
     * local stdout cannot notice that, because the local path was right.
     *
     * So this drives a real daemon and asserts the two agree. */
    atlas_err err;
    atlas_err_init(&err);
    e2e E;
    e2e_open(&E, &err);

    fx_daemon d;
    fx_daemon_init(&d);
    T_OK(fx_daemon_start(&E.fx, &d, &err), &err);
    T_OK(fx_daemon_wait_ready(&d, 30000, &err), &err);

    /* `deep_caller` lives in the file the compilation database does not name, so
     * this generation has never seen it. Locally that is UNKNOWN with a trust
     * block; over the socket it used to be an error document, exit 2, no
     * verdict — the operator-typo class applied to the case the season exists
     * for. */
    atlas_buf out = ATLAS_BUF_INIT;
    int code = -1;
    const char *q[] = {"code", "callers", "fixture", "deep_caller", "--json"};
    T_OK(fx_atlas_with_runtime(&E.fx, &d, q, 5u, &out, NULL, &code, &err), &err);
    T_CHECK_MSG(code != 2, "over the socket, a symbol not in the index must not be a usage "
                           "error: exit %d, %s",
                code, atlas_buf_cstr(&out));
    T_CHECK_MSG(has(&out, "\"result_verdict\":\"UNKNOWN\""),
                "the socket must answer UNKNOWN with a trust block: %s", atlas_buf_cstr(&out));
    check_trust_block(&out, "code callers (over the socket)");
    atlas_buf_free(&out);

    /* **Negative case one: insufficient coverage + zero rows over the socket.**
     * A successful RPC answer carrying UNKNOWN — not an error, not an absence. */
    atlas_buf rpc_unknown = ATLAS_BUF_INIT;
    const char *q2[] = {"code", "callers", "fixture", "orphan", "--json"};
    T_OK(fx_atlas_with_runtime(&E.fx, &d, q2, 5u, &rpc_unknown, NULL, &code, &err), &err);
    T_CHECK_MSG(code == 0, "a normal query over the socket must succeed, exit %d: %s", code,
                atlas_buf_cstr(&rpc_unknown));
    check_trust_block(&rpc_unknown, "code callers orphan (over the socket)");
    T_CHECK_MSG(has(&rpc_unknown, "\"nodes\":[]"), "expected an empty result set: %s",
                atlas_buf_cstr(&rpc_unknown));
    T_CHECK_MSG(has(&rpc_unknown, "\"result_verdict\":\"UNKNOWN\""),
                "partial coverage over the socket must answer UNKNOWN: %s",
                atlas_buf_cstr(&rpc_unknown));

    /* And the socket says exactly what the local path says, value by value. The
     * daemon is stopped for the local run so the CLI takes the writer path. */
    fx_daemon_stop(&d, false);
    atlas_buf local_unknown = ATLAS_BUF_INIT;
    run_ok(&E, q2, 5u, &local_unknown, &err);
    check_same_trust_values(&local_unknown, &rpc_unknown, "callers orphan, local vs socket");
    atlas_buf_free(&local_unknown);
    atlas_buf_free(&rpc_unknown);

    /* **Negative case two: current, complete coverage + zero rows.** Widen the
     * compilation database to the whole tree and rebuild, so the same shape of
     * query over the same socket may now settle ABSENT. */
    const char *all[] = {"src/lib.c", "nodus/tests/deep.c"};
    write_compdb(&E, all, 2u, &err);
    T_OK(fx_add_all(&E.fx, fx_repo(&E.fx), &err), &err);
    T_OK(fx_commit(&E.fx, fx_repo(&E.fx), "full compdb", &err), &err);
    const char *scan[] = {"scan", "fixture"};
    run_ok(&E, scan, 2u, NULL, &err);
    const char *sync[] = {"code", "sync", "fixture"};
    run_ok(&E, sync, 3u, NULL, &err);
    const char *idx[] = {"code", "index", "fixture"};
    run_ok(&E, idx, 3u, NULL, &err);

    fx_daemon d2;
    fx_daemon_init(&d2);
    T_OK(fx_daemon_start(&E.fx, &d2, &err), &err);
    T_OK(fx_daemon_wait_ready(&d2, 30000, &err), &err);

    atlas_buf rpc_absent = ATLAS_BUF_INIT;
    const char *q4[] = {"code", "callers", "fixture", "never_called_anywhere", "--json"};
    T_OK(fx_atlas_with_runtime(&E.fx, &d2, q4, 5u, &rpc_absent, NULL, &code, &err), &err);
    T_CHECK_MSG(code == 0, "the ABSENT case must be a successful RPC answer, exit %d: %s", code,
                atlas_buf_cstr(&rpc_absent));
    check_trust_block(&rpc_absent, "code callers never_called_anywhere (over the socket)");
    T_CHECK_MSG(has(&rpc_absent, "\"nodes\":[]"), "expected an empty result set: %s",
                atlas_buf_cstr(&rpc_absent));
    T_CHECK_MSG(has(&rpc_absent, "\"result_verdict\":\"ABSENT\""),
                "complete coverage over the socket must settle ABSENT: %s",
                atlas_buf_cstr(&rpc_absent));

    fx_daemon_stop(&d2, false);
    atlas_buf local_absent = ATLAS_BUF_INIT;
    run_ok(&E, q4, 5u, &local_absent, &err);
    T_CHECK_MSG(has(&local_absent, "\"result_verdict\":\"ABSENT\""),
                "and the local path agrees: %s", atlas_buf_cstr(&local_absent));
    check_same_trust_values(&local_absent, &rpc_absent, "callers absent, local vs socket");
    atlas_buf_free(&local_absent);
    atlas_buf_free(&rpc_absent);

    /* A genuine usage error is still exit 2 over the socket: a request naming
     * neither a symbol nor a usr is a question Atlas cannot answer as asked. */
    T_OK(fx_daemon_start(&E.fx, &d2, &err), &err);
    T_OK(fx_daemon_wait_ready(&d2, 30000, &err), &err);
    atlas_buf usage = ATLAS_BUF_INIT;
    const char *q5[] = {"code", "trace", "fixture", "--json"};
    T_OK(fx_atlas_with_runtime(&E.fx, &d2, q5, 4u, &usage, NULL, &code, &err), &err);
    T_CHECK_MSG(code == 2, "a malformed request must stay a usage error, exit %d: %s", code,
                atlas_buf_cstr(&usage));
    atlas_buf_free(&usage);
    fx_daemon_stop(&d2, false);
    fx_daemon_free(&d2);

    T_OK(fx_daemon_start(&E.fx, &d, &err), &err);
    T_OK(fx_daemon_wait_ready(&d, 30000, &err), &err);

    /* `sem-status` over the socket carries the block and, since A9.2.5, carries
     * every key exactly once — Atlas' own remote parser reads the first
     * occurrence of a duplicated key while every other consumer reads the last. */
    atlas_buf out3 = ATLAS_BUF_INIT;
    const char *q3[] = {"code", "sem-status", "fixture", "--json"};
    T_OK(fx_atlas_with_runtime(&E.fx, &d, q3, 4u, &out3, NULL, &code, &err), &err);
    T_CHECK_MSG(code == 0, "sem-status over the socket must succeed: %s", atlas_buf_cstr(&out3));
    check_trust_block(&out3, "code sem-status (over the socket)");
    check_no_duplicate_root_keys(&out3, "code sem-status (over the socket)");
    atlas_buf_free(&out3);

    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    e2e_close(&E);
}

static void test_e2e_the_two_trust_surfaces_stay_apart(void) {
    /* A3's structural index and A8-CI's semantic index are different trust
     * surfaces and their currency is not one value. A repository whose
     * structural index is current may have a semantic index that is not, and a
     * surface that merged them would let one vouch for the other. */
    atlas_err err;
    atlas_err_init(&err);
    e2e E;
    e2e_open(&E, &err);

    /* Edit a source without touching the build description, and reconcile only
     * the *structural* side. */
    T_OK(fx_write(fx_repo(&E.fx), "src/lib.c",
                  "#include \"lib.h\"\n"
                  "int orphan(int x) { return x * 3; }\n"
                  "int never_called_anywhere(int x) { return x - 2; }\n"
                  "int entry(int x) { return x + 7; }\n",
                  &err),
         &err);
    const char *scan[] = {"scan", "fixture", "--full"};
    run_ok(&E, scan, 3u, NULL, &err);
    const char *sync[] = {"code", "sync", "fixture"};
    run_ok(&E, sync, 3u, NULL, &err);

    atlas_buf code_status = ATLAS_BUF_INIT;
    const char *cs[] = {"code", "status", "fixture", "--json"};
    run_ok(&E, cs, 4u, &code_status, &err);
    atlas_buf sem_status = ATLAS_BUF_INIT;
    const char *ss[] = {"code", "sem-status", "fixture", "--json"};
    run_ok(&E, ss, 4u, &sem_status, &err);

    T_CHECK_MSG(has(&code_status, "\"code_index_current\":true"),
                "the structural index should be current after `code sync`: %s",
                atlas_buf_cstr(&code_status));
    T_CHECK_MSG(has(&sem_status, "\"freshness\":\"STALE\""),
                "the semantic index must be stale after an unindexed source edit: %s",
                atlas_buf_cstr(&sem_status));

    /* And the query answer must inherit the semantic surface's verdict, not the
     * structural surface's currency. */
    atlas_buf out = ATLAS_BUF_INIT;
    const char *q[] = {"code", "callers", "fixture", "never_called_anywhere", "--json"};
    run_ok(&E, q, 5u, &out, &err);
    T_CHECK_MSG(has(&out, "\"result_verdict\":\"UNKNOWN\""),
                "a stale semantic index must not settle an absence because the structural "
                "index is current: %s",
                atlas_buf_cstr(&out));

    atlas_buf_free(&out);
    atlas_buf_free(&sem_status);
    atlas_buf_free(&code_status);
    e2e_close(&E);
}

static const atlas_test TESTS[] = {
    {"a zeroed trust block claims nothing", test_verdict_zero_is_the_safe_reading},
    {"the verdict vocabulary refuses what it does not know", test_verdict_vocabulary},
    {"sufficient coverage settles an absence", test_verdict_sufficient_coverage_settles_absent},
    {"one row is PRESENT whatever the coverage",
     test_verdict_one_row_is_present_whatever_the_coverage},
    {"no generation cannot settle", test_verdict_absent_index_cannot_settle},
    {"a stale generation cannot settle", test_verdict_stale_index_cannot_settle},
    {"a rebuilding generation cannot settle", test_verdict_rebuilding_index_cannot_settle},
    {"an undescribed translation unit cannot settle",
     test_verdict_incomplete_units_cannot_settle},
    {"uncovered sources cannot settle", test_verdict_uncovered_sources_cannot_settle},
    {"a generation with no coverage manifest cannot settle",
     test_verdict_unknown_scope_cannot_settle},
    {"partial build-input discovery cannot settle",
     test_verdict_partial_discovery_cannot_settle},
    {"the verdict rests on the generation's discovery, not the live one",
     test_verdict_the_verdict_rests_on_the_generations_discovery},
    {"a repository nobody maintains cannot settle",
     test_verdict_disabled_maintenance_cannot_settle},
    {"an Atlas without libclang cannot settle", test_verdict_no_libclang_cannot_settle},
    {"a truncated empty walk cannot settle", test_verdict_a_truncated_empty_walk_cannot_settle},
    {"the reason named is the most actionable one",
     test_verdict_reason_precedence_is_the_most_actionable},
    {"only a machine failure is transient", test_only_a_machine_failure_is_transient},
    {"the unit retry bound is compile-time and small",
     test_the_unit_retry_bound_is_compile_time_and_small},
    {"a generation built for another repository identity is stale",
     test_a_different_repository_identity_is_stale},
    {"repository identity is reported before a moved commit",
     test_repository_identity_outranks_a_moved_commit},
    {"zero rows over partial coverage answers UNKNOWN",
     test_e2e_zero_rows_over_partial_coverage_is_unknown},
    {"full coverage settles ABSENT, and a found row PRESENT",
     test_e2e_full_coverage_settles_absent_and_present},
    {"a symbol in an uncovered file is not a usage error",
     test_e2e_a_symbol_outside_the_index_is_not_a_usage_error},
    {"every load-bearing surface carries the trust block",
     test_e2e_every_load_bearing_surface_carries_the_trust_block},
    {"a repository with no index still carries the trust block",
     test_e2e_a_repository_with_no_index_still_carries_the_block},
    {"the socket answers exactly as the local path does",
     test_rpc_answers_exactly_as_the_local_path_does},
    {"the structural and semantic trust surfaces stay apart",
     test_e2e_the_two_trust_surfaces_stay_apart},
};

ATLAS_TEST_MAIN("sem_trust", TESTS)
