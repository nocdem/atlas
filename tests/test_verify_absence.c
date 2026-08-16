/* Atlas - A9.2.2: epistemic absence and coverage semantics.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The one invariant this file exists to hold:
 *
 *   **NO EVIDENCE OF X IS NOT EVIDENCE OF NO X.**
 *
 * Everything here is a way of asking whether Atlas confuses "I found no
 * evidence of X" with "X is proven absent". `tests/test_verify_engine.c` is the
 * model this follows: a real database in process, no daemon and no transport,
 * because what is under test is the engine and the schema rather than a wire
 * format. The transport-level checks live in `tests/test_verify_product.c`.
 *
 * The season's fixtures and where each lives:
 *
 *   A  one caller exists, index incomplete ..................... PRESENT
 *   B  zero callers, indirect-call coverage incomplete ......... UNKNOWN
 *   C  zero callers, bounded and complete ...................... ABSENT
 *   D  config key absent from repository, deployment unseen .... UNKNOWN
 *   E  a finite registry completely enumerated ................. ABSENT
 *   F  semantic index stale, zero result ....................... UNKNOWN
 *   G  source changes after an absence proof ................... UNKNOWN now
 *   H  three agents all fail to find X ......................... not ABSENT
 *   I  UNKNOWN then PRESENT .................... acquisition, not an error
 *   J  ABSENT then PRESENT at one snapshot ..................... an error
 *
 * The semantic tables are seeded with direct SQL rather than by running a real
 * compiler. That is deliberate and is not a shortcut: what these fixtures need
 * is *exact control over the coverage state* — a generation that is complete
 * but stale, one that is current but partial, a symbol whose address is taken
 * exactly once — and none of those is reliably producible by compiling a file.
 * The verifiers read these tables and nothing else, so the seeded state is the
 * same input a real index would present. `tests/test_sem.c` is what covers the
 * indexer actually populating them.
 */
#include <string.h>

#include <sqlite3.h>

#include "atlas/atlas.h"
#include "atlas/backup.h"
#include "atlas/datadir.h"
#include "atlas/db.h"
#include "atlas/decision.h"
#include "atlas/decision_ops.h"
#include "atlas/verify.h"
#include "atlas/verifypolicy.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

#define HEAD_COMMIT "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define OTHER_COMMIT "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

typedef struct env {
    fixture fx;
    atlas_buf db_path;
    atlas_db *db;
    int64_t repo_id;
} env;

/* Seeding SQL, formatted into a fixed buffer. A macro rather than a variadic
 * helper because `atlas_buf` has no `v`-form appender and adding one to the
 * library for a test's convenience would be the wrong direction. */
#define EXEC(e, err, ...)                                                                          \
    do {                                                                                           \
        char sql_[2048];                                                                           \
        int wrote_ = snprintf(sql_, sizeof sql_, __VA_ARGS__);                                      \
        T_REQUIRE(wrote_ > 0 && (size_t)wrote_ < sizeof sql_);                                      \
        T_OK(atlas_db_exec_sql((e)->db, sql_, (err)), (err));                                       \
    } while (0)

/* The rowid of the last row inserted into a table, read back rather than taken
 * from `last_insert_rowid` — the handle is not exposed outside `src/db` and
 * reaching into it from a test would be the wrong direction. */
static int64_t last_id(env *e, const char *table, atlas_err *err) {
    char sql[128];
    (void)snprintf(sql, sizeof sql, "SELECT COALESCE(MAX(id), 0) FROM %s;", table);
    sqlite3_stmt *stmt = NULL;
    int64_t id = 0;
    T_OK(atlas_db_prepare(e->db, sql, &stmt, err), err);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        id = sqlite3_column_int64(stmt, 0);
    }
    atlas_db_finish(e->db, stmt);
    return id;
}

static void env_open(env *e, atlas_err *err) {
    memset(e, 0, sizeof *e);
    atlas_buf_init(&e->db_path);
    T_OK(fx_open(&e->fx, err), err);
    T_OK(atlas_buf_appendf(&e->db_path, err, "%s/atlas.db", fx_data_dir(&e->fx)), err);
    T_OK(atlas_datadir_ensure(fx_data_dir(&e->fx), err), err);
    T_OK(atlas_db_open(atlas_buf_cstr(&e->db_path), &e->db, err), err);
    T_OK(atlas_db_migrate(e->db, err), err);

    atlas_repo_identity id;
    memset(&id, 0, sizeof id);
    id.root = "/tmp/atlas-absence-repo";
    id.root_len = strlen(id.root);
    id.common_dir = "/tmp/atlas-absence-repo/.git";
    id.common_dir_len = strlen(id.common_dir);
    id.git_dir = id.common_dir;
    id.git_dir_len = id.common_dir_len;
    id.object_format = "sha1";
    T_OK(atlas_db_repo_add(e->db, "proj", &id, &e->repo_id, err), err);

    EXEC(e, err,
             "INSERT INTO commits(repo_id, oid, parent_count, subject)"
             "  VALUES(%lld, '%s', 0, 'root');"
             "UPDATE repositories SET scanned_head = '%s' WHERE id = %lld;",
             (long long)e->repo_id, HEAD_COMMIT, HEAD_COMMIT, (long long)e->repo_id);
}

static void env_close(env *e) {
    atlas_db_close(e->db);
    atlas_buf_free(&e->db_path);
    fx_close(&e->fx);
}

/* --- seeding a semantic generation with a chosen coverage state -------------
 *
 * `complete` decides whether any translation unit failed; `commit` decides
 * whether the generation describes the repository's current head. The two are
 * independent inputs on purpose — a generation can be complete and stale, or
 * current and partial, and the fixtures need both cases separately because they
 * produce different truth reasons. */
static int64_t seed_generation(env *e, bool complete, const char *commit, atlas_err *err) {
    EXEC(e, err,
             "INSERT INTO sem_generations(repo_id, commit_id, status, started_at, completed_at,"
             "  tu_total, tu_complete, tu_partial, tu_failed, tu_unsupported)"
             "  VALUES(%lld, '%s', 'COMPLETE', '2026-01-01T00:00:00Z', '2026-01-01T00:01:00Z',"
             "         2, %d, 0, %d, 0);",
             (long long)e->repo_id, commit, complete ? 2 : 1, complete ? 0 : 1);
    int64_t gen = last_id(e, "sem_generations", err);
    EXEC(e, err,
             "INSERT INTO sem_current(repo_id, generation_id) VALUES(%lld, %lld)"
             "  ON CONFLICT(repo_id) DO UPDATE SET generation_id = %lld;",
             (long long)e->repo_id, (long long)gen, (long long)gen);
    return gen;
}

/* One function symbol. `internal` is the compiler-computed linkage that decides
 * whether callers outside the indexed tree are excludable. */
static void seed_symbol(env *e, int64_t gen, const char *usr, const char *name, bool internal,
                        atlas_err *err) {
    EXEC(e, err,
             "INSERT INTO sem_symbols(generation_id, usr, name, kind, linkage, file_text, line,"
             "  is_definition, external, evidence)"
             "  VALUES(%lld, '%s', '%s', 'FUNCTION', '%s', 'src/a.c', 10, 1, 0, 'PROVEN');",
             (long long)gen, usr, name, internal ? "INTERNAL" : "EXTERNAL");
}

static void seed_edge(env *e, int64_t gen, const char *kind, const char *src, const char *dst,
                      const char *evidence, int line, atlas_err *err) {
    EXEC(e, err,
             "INSERT INTO sem_edges(generation_id, kind, src_usr, dst_usr, evidence, file_text,"
             "  line, col) VALUES(%lld, '%s', '%s', '%s', '%s', 'src/a.c', %d, 1);",
             (long long)gen, kind, src, dst, evidence, line);
}

/* Makes the file index current, which `ATLAS_COVDIM_REPOSITORY_SNAPSHOT` reads.
 * Without this every fixture would carry a stale snapshot and the content-hash
 * assertions would be measuring the wrong thing. */
static void seed_index_current(env *e, atlas_err *err) {
    T_OK(atlas_db_index_state_ensure(e->db, e->repo_id, err), err);
    EXEC(e, err,
             "UPDATE repo_index_state SET generation = 1, last_complete_generation = 1,"
             "  watch_state = 'watching', event_gap = 0, pending_full_reconcile = 0"
             " WHERE repo_id = %lld;",
             (long long)e->repo_id);
}

static void propose(env *e, atlas_decision_kind kind, const char *title, atlas_buf *uid_out,
                    atlas_err *err) {
    atlas_decision_op op;
    atlas_decision_op_init(&op, ATLAS_DECISION_OP_PROPOSE);
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_OK(atlas_buf_set_str(&op.repo_name, "proj", err), err);
    op.knowledge_kind = kind;
    op.knowledge_kind_given = true;
    T_OK(atlas_buf_set_str(&op.revision.title, title, err), err);
    T_OK(atlas_buf_set_str(&op.revision.decision_text, "a body for the fixture", err), err);
    op.revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
    T_OK(atlas_decision_apply(e->db, &op, &res, err), err);
    if (uid_out != NULL) {
        T_OK(atlas_buf_set(uid_out, res.uid.data, res.uid.len, err), err);
    }
    atlas_decision_op_free(&op);
    atlas_decision_result_free(&res);
}

static int64_t claim(env *e, const atlas_buf *uid, const char *text,
                     atlas_verify_claim_semantics sem, const char *verifier, const char *input,
                     const char *basis_commit, atlas_err *err) {
    int64_t doc = 0, repo = 0, rev = 0, no = 0;
    bool found = false;
    T_OK(atlas_db_decision_find_uid(e->db, atlas_buf_cstr(uid), &doc, &repo, &found, err), err);
    T_REQUIRE(found);
    char hash[ATLAS_SHA256_HEX_LEN + 1u];
    char state[16];
    T_OK(atlas_db_decision_latest_revision(e->db, doc, &rev, &no, hash, sizeof hash, state,
                                           sizeof state, err),
         err);

    atlas_verify_claim c;
    atlas_verify_claim_init(&c);
    c.repo_id = e->repo_id;
    c.document_id = doc;
    c.revision_id = rev;
    c.semantics = sem;
    T_OK(atlas_buf_set_str(&c.text, text, err), err);
    T_OK(atlas_buf_set_str(&c.domain, "fixture", err), err);
    if (verifier != NULL) {
        T_OK(atlas_buf_set_str(&c.verifier, verifier, err), err);
        T_OK(atlas_buf_set_str(&c.verifier_input, input != NULL ? input : "", err), err);
    }
    if (basis_commit != NULL) {
        T_OK(atlas_buf_set_str(&c.basis_commit, basis_commit, err), err);
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof now);
    T_OK(atlas_db_verify_claim_insert(e->db, &c, now, err), err);
    int64_t id = c.id;
    atlas_verify_claim_free(&c);
    return id;
}

/* Runs one verifier directly. Most fixtures assert at this level rather than
 * through the whole assessment, because it is the verifier and the coverage
 * model that are under test and a policy would only add a second reason for the
 * answer to come out blocked. */
static atlas_verify_truth run(env *e, atlas_verify_verifier v, const char *input,
                              atlas_verify_check *check_out,
                              atlas_verify_coverage_report *cov_out,
                              atlas_verify_truth_reason *why_out, atlas_err *err) {
    atlas_verify_check check = ATLAS_CHECK_UNAVAILABLE;
    atlas_verify_coverage_report cov;
    atlas_verify_coverage_report_init(&cov);
    char scope[512];
    char detail[512];
    T_OK(atlas_verify_run_verifier(e->db, v, e->repo_id, input, &check, &cov, scope, sizeof scope,
                                   detail, sizeof detail, err),
         err);
    atlas_verify_truth_reason why = ATLAS_TREASON_NONE;
    atlas_verify_truth t = atlas_verify_truth_of(v, ATLAS_VERIFY_BASIS_DETERMINISTIC,
                                                 ATLAS_CLAIM_DESCRIPTIVE, check, &cov, &why);
    if (check_out != NULL) {
        *check_out = check;
    }
    if (cov_out != NULL) {
        *cov_out = cov;
    }
    if (why_out != NULL) {
        *why_out = why;
    }
    return t;
}

/* ==========================================================================
 * Fixture A — one caller exists, the index is incomplete. Expected: PRESENT.
 *
 * §7's asymmetry in its positive direction. An incomplete index cannot conjure
 * a call that is not there, so finding one is finding one and no coverage
 * requirement applies. If this test ever starts reporting UNKNOWN, the gate has
 * been applied to both directions and Atlas has become uselessly cautious
 * rather than correctly cautious.
 * ========================================================================== */
static void test_fixture_a_one_caller_over_a_partial_index_is_present(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    int64_t gen = seed_generation(&e, /*complete=*/false, HEAD_COMMIT, &err);
    seed_symbol(&e, gen, "c:@F@target", "target", true, &err);
    seed_symbol(&e, gen, "c:@F@caller", "caller", true, &err);
    seed_edge(&e, gen, "CALLS", "c:@F@caller", "c:@F@target", "PROVEN", 20, &err);

    atlas_verify_check check = ATLAS_CHECK_UNAVAILABLE;
    atlas_verify_coverage_report cov;
    atlas_verify_truth t =
        run(&e, ATLAS_VERIFIER_NO_PROVEN_CALLER, "symbol=target", &check, &cov, NULL, &err);

    T_CHECK_MSG(t == ATLAS_TRUTH_PRESENT,
                "a caller that exists must be PRESENT even over a partial index, got %s",
                atlas_verify_truth_name(t));
    T_EQ_INT((int)check, (int)ATLAS_CHECK_FAIL); /* FAIL of "no caller", i.e. one exists */
    /* The coverage really is partial — this is not passing because the fixture
     * accidentally built a complete generation. */
    T_EQ_INT((int)cov.dims[ATLAS_COVDIM_SEMANTIC_GENERATION], (int)ATLAS_COVERAGE_PARTIAL);

    env_close(&e);
}

/* ==========================================================================
 * Fixture B — zero callers, but the symbol's address is taken. Expected:
 * UNKNOWN.
 *
 * §9. Direct-call completeness is not sufficient for "no caller reaches X". The
 * address escaping means a dispatch table, a callback or a dynamic registration
 * could reach it, and no amount of further indexing recovers the target set.
 * ========================================================================== */
static void test_fixture_b_zero_callers_with_an_escaping_address_is_unknown(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    int64_t gen = seed_generation(&e, /*complete=*/true, HEAD_COMMIT, &err);
    seed_symbol(&e, gen, "c:@F@target", "target", true, &err);
    seed_symbol(&e, gen, "c:@F@registrar", "registrar", true, &err);
    /* No CALLS edge at all — nothing calls it directly. But its address is
     * taken, which is exactly the case a naive "zero results" reading gets
     * wrong. */
    seed_edge(&e, gen, "ADDRESS_TAKEN", "c:@F@registrar", "c:@F@target", "PROVEN", 30, &err);

    atlas_verify_check check = ATLAS_CHECK_UNAVAILABLE;
    atlas_verify_coverage_report cov;
    atlas_verify_truth_reason why = ATLAS_TREASON_NONE;
    atlas_verify_truth t =
        run(&e, ATLAS_VERIFIER_NO_PROVEN_CALLER, "symbol=target", &check, &cov, &why, &err);

    T_CHECK_MSG(t != ATLAS_TRUTH_ABSENT,
                "an unresolved indirect call path became a proof of absence");
    T_EQ_INT((int)t, (int)ATLAS_TRUTH_UNKNOWN);
    T_EQ_INT((int)why, (int)ATLAS_TREASON_INDIRECT_CALLS_UNRESOLVED);
    T_EQ_INT((int)cov.dims[ATLAS_COVDIM_INDIRECT_CALLS], (int)ATLAS_COVERAGE_PARTIAL);
    /* And the check axis agrees rather than reporting a confident PASS that the
     * truth axis then contradicts. One evaluation, one answer. */
    T_EQ_INT((int)check, (int)ATLAS_CHECK_UNAVAILABLE);

    env_close(&e);
}

/* ==========================================================================
 * Fixture C — zero callers over a bounded, complete, internal-linkage case.
 * Expected: ABSENT.
 *
 * The one shape in which Atlas may say "nothing calls this". The symbol is
 * `static`, so no caller outside its translation unit can name it and `dlsym`
 * cannot reach it; its address is never taken, so no indirect path exists; and
 * the generation is complete and current, so the two counts are trustworthy.
 *
 * If this test starts reporting UNKNOWN, ABSENT has become unreachable and the
 * season has produced a system that can never discharge an obligation.
 * ========================================================================== */
static void test_fixture_c_a_bounded_complete_absence_is_absent(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    int64_t gen = seed_generation(&e, /*complete=*/true, HEAD_COMMIT, &err);
    seed_symbol(&e, gen, "c:a.c@F@orphan", "orphan", /*internal=*/true, &err);
    seed_symbol(&e, gen, "c:@F@other", "other", true, &err);
    /* An unrelated call, so the fixture is not passing merely because the edge
     * table is empty. */
    seed_edge(&e, gen, "CALLS", "c:@F@other", "c:@F@other", "PROVEN", 40, &err);

    atlas_verify_check check = ATLAS_CHECK_UNAVAILABLE;
    atlas_verify_coverage_report cov;
    atlas_verify_truth_reason why = ATLAS_TREASON_NONE;
    atlas_verify_truth t =
        run(&e, ATLAS_VERIFIER_NO_PROVEN_CALLER, "symbol=orphan", &check, &cov, &why, &err);

    T_CHECK_MSG(t == ATLAS_TRUTH_ABSENT,
                "a completely bounded absence must be establishable, got %s (%s)",
                atlas_verify_truth_name(t), atlas_verify_truth_reason_name(why));
    T_EQ_INT((int)check, (int)ATLAS_CHECK_PASS);
    T_EQ_INT((int)why, (int)ATLAS_TREASON_ESTABLISHED);
    T_EQ_INT((int)cov.dims[ATLAS_COVDIM_INDIRECT_CALLS], (int)ATLAS_COVERAGE_COMPLETE);
    T_EQ_INT((int)cov.dims[ATLAS_COVDIM_EXTERNAL_CALLERS], (int)ATLAS_COVERAGE_NOT_APPLICABLE);

    env_close(&e);
}

/* External linkage is the other half of fixture C, and it must not pass.
 * Callers outside the indexed repository and `dlsym` are invisible from here,
 * so the same zero counts establish nothing. */
static void test_external_linkage_keeps_an_absence_unknown(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    int64_t gen = seed_generation(&e, /*complete=*/true, HEAD_COMMIT, &err);
    seed_symbol(&e, gen, "c:@F@exported", "exported", /*internal=*/false, &err);

    atlas_verify_truth_reason why = ATLAS_TREASON_NONE;
    atlas_verify_coverage_report cov;
    atlas_verify_truth t =
        run(&e, ATLAS_VERIFIER_NO_PROVEN_CALLER, "symbol=exported", NULL, &cov, &why, &err);

    T_CHECK_MSG(t == ATLAS_TRUTH_UNKNOWN,
                "an external-linkage symbol's callers cannot be enumerated from the index alone");
    T_EQ_INT((int)why, (int)ATLAS_TREASON_EXTERNAL_CALLERS_POSSIBLE);
    T_EQ_INT((int)cov.dims[ATLAS_COVDIM_EXTERNAL_CALLERS], (int)ATLAS_COVERAGE_PARTIAL);

    env_close(&e);
}

/* ==========================================================================
 * Fixture D — a key absent from the repository, with the deployment unseen.
 * Expected: UNKNOWN.
 *
 * §11. Repository absence must not become operational absence. Atlas has no
 * runtime probe, so RUNTIME_STATE and DEPLOYED_CONFIG are UNKNOWN for every
 * verifier — and a claim whose negative would rest on them is therefore UNKNOWN
 * by construction rather than by a rule that remembers to check.
 * ========================================================================== */
static void test_fixture_d_repository_absence_is_not_operational_absence(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    int64_t gen = seed_generation(&e, /*complete=*/true, HEAD_COMMIT, &err);
    (void)gen; /* the symbol genuinely is not there */

    atlas_verify_coverage_report cov;
    atlas_verify_truth t =
        run(&e, ATLAS_VERIFIER_SYMBOL_ABSENT, "symbol=deploy_override", NULL, &cov, NULL, &err);

    /* The repository-scoped question is answerable and answers ABSENT. */
    T_EQ_INT((int)t, (int)ATLAS_TRUTH_ABSENT);

    /* But the operational dimensions were never established, so a claim that
     * needed them could not have reached ABSENT. This is the structural fact
     * §11 rests on, asserted directly. */
    T_EQ_INT((int)cov.dims[ATLAS_COVDIM_RUNTIME_STATE], (int)ATLAS_COVERAGE_UNKNOWN);
    T_EQ_INT((int)cov.dims[ATLAS_COVDIM_DEPLOYED_CONFIG], (int)ATLAS_COVERAGE_UNKNOWN);
    T_CHECK_MSG(!atlas_verify_coverage_sufficient(cov.dims[ATLAS_COVDIM_DEPLOYED_CONFIG]),
                "an unobserved deployment must never count as covered");

    /* And a verifier that *did* require them would be refused. Asked through
     * the real gate rather than by inspection, so the guarantee is the code's
     * rather than this test's. */
    static const atlas_verify_coverage_dim NEEDS_DEPLOYMENT[] = {ATLAS_COVDIM_DEPLOYED_CONFIG};
    atlas_verify_coverage_dim failed = ATLAS_COVDIM_SEMANTIC_GENERATION;
    atlas_verify_truth_reason why = ATLAS_TREASON_NONE;
    T_CHECK(!atlas_verify_coverage_satisfies(&cov, NEEDS_DEPLOYMENT, 1, &failed, &why));
    T_EQ_INT((int)failed, (int)ATLAS_COVDIM_DEPLOYED_CONFIG);

    env_close(&e);
}

/* ==========================================================================
 * Fixture E — a finite registry completely enumerated. Expected: ABSENT.
 *
 * The semantic generation *is* the finite canonical registry of the symbols in
 * the tree: when it is complete and current, enumerating it is exhaustive over
 * its declared scope. Deterministic verification needs no calibration to say
 * so, and §14 requires that this stays possible.
 * ========================================================================== */
static void test_fixture_e_a_complete_registry_establishes_absence(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    int64_t gen = seed_generation(&e, /*complete=*/true, HEAD_COMMIT, &err);
    seed_symbol(&e, gen, "c:@F@known_a", "known_a", true, &err);
    seed_symbol(&e, gen, "c:@F@known_b", "known_b", true, &err);

    atlas_verify_check check = ATLAS_CHECK_UNAVAILABLE;
    atlas_verify_truth_reason why = ATLAS_TREASON_NONE;
    atlas_verify_truth t =
        run(&e, ATLAS_VERIFIER_SYMBOL_ABSENT, "symbol=not_registered", &check, NULL, &why, &err);

    T_EQ_INT((int)t, (int)ATLAS_TRUTH_ABSENT);
    T_EQ_INT((int)check, (int)ATLAS_CHECK_PASS);
    T_EQ_INT((int)why, (int)ATLAS_TREASON_ESTABLISHED);

    env_close(&e);
}

/* ==========================================================================
 * Fixture F — the semantic index is stale. A zero result must be UNKNOWN.
 *
 * The generation is complete, so the A9.2 completeness flag alone would have
 * let this through. What stops it is that the generation describes a commit the
 * repository has left, which A9.2.2 reports as STALE rather than folding into
 * "complete".
 * ========================================================================== */
static void test_fixture_f_a_stale_index_cannot_establish_absence(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    /* Complete, but built from a different commit than the repository's head. */
    (void)seed_generation(&e, /*complete=*/true, OTHER_COMMIT, &err);

    atlas_verify_check check = ATLAS_CHECK_UNAVAILABLE;
    atlas_verify_coverage_report cov;
    atlas_verify_truth_reason why = ATLAS_TREASON_NONE;
    atlas_verify_truth t =
        run(&e, ATLAS_VERIFIER_SYMBOL_ABSENT, "symbol=anything", &check, &cov, &why, &err);

    T_CHECK_MSG(t != ATLAS_TRUTH_ABSENT, "a stale index established an absence");
    T_EQ_INT((int)t, (int)ATLAS_TRUTH_UNKNOWN);
    T_EQ_INT((int)cov.dims[ATLAS_COVDIM_SEMANTIC_GENERATION], (int)ATLAS_COVERAGE_STALE);
    T_EQ_INT((int)check, (int)ATLAS_CHECK_UNAVAILABLE);
    /* **Stale, not incomplete**, and the distinction is the reason this
     * assertion exists rather than only the one above. The generation *is*
     * complete — every translation unit parsed — so a reader told
     * SEMANTIC_INDEX_INCOMPLETE would go and widen a parse that has nothing
     * missing. The remedy for STALE is a fresh index, and a confident
     * instruction to do the wrong thing is worse than no instruction. */
    T_EQ_INT((int)why, (int)ATLAS_TREASON_SEMANTIC_INDEX_STALE);

    env_close(&e);
}

/* No generation published at all is a third answer again: not "part of the tree
 * was missed" and not "the tree moved", but "there is nothing to look at". */
static void test_an_unindexed_repository_says_so_rather_than_reporting_incompleteness(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);
    /* Deliberately no generation at all. */

    atlas_verify_coverage_report cov;
    atlas_verify_truth_reason why = ATLAS_TREASON_NONE;
    atlas_verify_truth t =
        run(&e, ATLAS_VERIFIER_SYMBOL_ABSENT, "symbol=anything", NULL, &cov, &why, &err);

    T_EQ_INT((int)t, (int)ATLAS_TRUTH_UNKNOWN);
    T_EQ_INT((int)why, (int)ATLAS_TREASON_SEMANTIC_INDEX_ABSENT);
    T_EQ_INT((int)cov.dims[ATLAS_COVDIM_SEMANTIC_GENERATION], (int)ATLAS_COVERAGE_UNKNOWN);

    env_close(&e);
}

/* ==========================================================================
 * Fixture G — the source changes after an absence proof.
 *
 * §19. The old result stays bound to the snapshot it examined; the *current*
 * answer becomes UNKNOWN until something re-establishes it. The stored row is
 * history and is not rewritten.
 * ========================================================================== */
static void test_fixture_g_an_absence_does_not_survive_the_source_moving(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);
    seed_index_current(&e, &err);
    (void)seed_generation(&e, /*complete=*/true, HEAD_COMMIT, &err);

    atlas_buf uid = ATLAS_BUF_INIT;
    propose(&e, ATLAS_DECISION_KIND_OBLIGATION, "the debug keystore must go", &uid, &err);
    int64_t c = claim(&e, &uid, "no debug keystore symbol remains", ATLAS_CLAIM_DESCRIPTIVE,
                      "atlas.symbol_absent", "symbol=debug_keystore", HEAD_COMMIT, &err);

    atlas_verifypolicy p;
    memset(&p, 0, sizeof p);
    atlas_verify_assessment first;
    T_OK(atlas_verify_assess(e.db, &p, c, &first, &err), &err);
    T_CHECK_MSG(first.truth == ATLAS_TRUTH_ABSENT, "the bounded absence should hold at first, %s",
                atlas_verify_truth_reason_name(first.truth_reason));

    /* The repository moves. Nothing about the claim or the stored result
     * changes; what changes is what "now" means. */
    EXEC(&e, &err, "UPDATE repositories SET scanned_head = '%s' WHERE id = %lld;",
             OTHER_COMMIT, (long long)e.repo_id);

    atlas_verify_assessment second;
    T_OK(atlas_verify_assess(e.db, &p, c, &second, &err), &err);
    T_CHECK_MSG(second.truth == ATLAS_TRUTH_UNKNOWN,
                "an absence proved against one tree stayed current across a move, got %s",
                atlas_verify_truth_name(second.truth));
    T_EQ_INT((int)second.truth_reason, (int)ATLAS_TREASON_SOURCE_DRIFT);
    T_CHECK(second.source_drift);

    /* And the change is classified as history rather than as a verifier error:
     * the code changed, nobody was wrong. */
    T_EQ_INT((int)atlas_verify_truth_change_of(ATLAS_TRUTH_ABSENT, ATLAS_TRUTH_PRESENT,
                                               /*same_snapshot=*/false),
             (int)ATLAS_TRUTH_CHANGE_HISTORICAL);

    atlas_buf_free(&uid);
    env_close(&e);
}

/* ==========================================================================
 * Fixture H — three agents all fail to find X. Expected: not ABSENT.
 *
 * §15. Empirical evidence never establishes presence or absence however many
 * sources agree, because "nobody found it" is a fact about the searchers. This
 * is asserted against the rule itself rather than through an aggregation, so it
 * cannot pass merely because three attestations happened to score low.
 * ========================================================================== */
static void test_fixture_h_agreeing_agents_do_not_establish_absence(void) {
    atlas_verify_truth_reason why = ATLAS_TREASON_NONE;
    /* A coverage report that is complete on every dimension — the most
     * favourable input an empirical claim could possibly present. */
    atlas_verify_coverage_report perfect;
    atlas_verify_coverage_report_init(&perfect);
    for (size_t i = 0; i < ATLAS_VERIFY_COVERAGE_DIMS; i++) {
        perfect.dims[i] = ATLAS_COVERAGE_COMPLETE;
    }

    atlas_verify_truth t = atlas_verify_truth_of(ATLAS_VERIFIER_SYMBOL_ABSENT,
                                                 ATLAS_VERIFY_BASIS_EMPIRICAL,
                                                 ATLAS_CLAIM_DESCRIPTIVE, ATLAS_CHECK_PASS,
                                                 &perfect, &why);
    T_CHECK_MSG(t != ATLAS_TRUTH_ABSENT,
                "an empirical consensus manufactured an absence proof");
    T_EQ_INT((int)t, (int)ATLAS_TRUTH_UNKNOWN);
    T_EQ_INT((int)why, (int)ATLAS_TREASON_EMPIRICAL_BASIS);
}

/* ==========================================================================
 * Fixtures I and J — the calibration consequence. §16.
 *
 * I: UNKNOWN then PRESENT is knowledge acquisition and must not be charged to
 *    anybody as a false negative. Charging it would penalise a verifier for
 *    having been honest about its coverage, which is the behaviour the whole
 *    season is built to encourage.
 * J: ABSENT then PRESENT at the same bound snapshot is a genuine verification
 *    error and is eligible for calibration feedback.
 * ========================================================================== */
static void test_fixture_i_unknown_becoming_present_is_not_a_verifier_error(void) {
    T_EQ_INT((int)atlas_verify_truth_change_of(ATLAS_TRUTH_UNKNOWN, ATLAS_TRUTH_PRESENT, true),
             (int)ATLAS_TRUTH_CHANGE_ACQUISITION);
    T_EQ_INT((int)atlas_verify_truth_change_of(ATLAS_TRUTH_UNKNOWN, ATLAS_TRUTH_ABSENT, true),
             (int)ATLAS_TRUTH_CHANGE_ACQUISITION);
    /* Losing an answer is not an error either: coverage that stopped being
     * sufficient is Atlas correctly withdrawing a claim it can no longer
     * support, which §19 requires. */
    T_EQ_INT((int)atlas_verify_truth_change_of(ATLAS_TRUTH_ABSENT, ATLAS_TRUTH_UNKNOWN, true),
             (int)ATLAS_TRUTH_CHANGE_NONE);
    /* A normative proposition is never a factual error in either direction. */
    T_EQ_INT((int)atlas_verify_truth_change_of(ATLAS_TRUTH_NOT_VERIFIABLE, ATLAS_TRUTH_PRESENT,
                                               true),
             (int)ATLAS_TRUTH_CHANGE_NONE);
}

static void test_fixture_j_absent_contradicted_at_one_snapshot_is_an_error(void) {
    T_EQ_INT((int)atlas_verify_truth_change_of(ATLAS_TRUTH_ABSENT, ATLAS_TRUTH_PRESENT, true),
             (int)ATLAS_TRUTH_CHANGE_ERROR);
    T_EQ_INT((int)atlas_verify_truth_change_of(ATLAS_TRUTH_PRESENT, ATLAS_TRUTH_ABSENT, true),
             (int)ATLAS_TRUTH_CHANGE_ERROR);
    /* The snapshot is what separates an error from history, and it is the
     * binding that decides it — never elapsed time, which would eventually make
     * every long-lived claim look like a failure. */
    T_EQ_INT((int)atlas_verify_truth_change_of(ATLAS_TRUTH_ABSENT, ATLAS_TRUTH_PRESENT, false),
             (int)ATLAS_TRUTH_CHANGE_HISTORICAL);
    /* §21: only the two established values can contradict each other. */
    T_CHECK(atlas_verify_truth_contradicts(ATLAS_TRUTH_PRESENT, ATLAS_TRUTH_ABSENT));
    T_CHECK(!atlas_verify_truth_contradicts(ATLAS_TRUTH_UNKNOWN, ATLAS_TRUTH_ABSENT));
    T_CHECK(!atlas_verify_truth_contradicts(ATLAS_TRUTH_NOT_VERIFIABLE, ATLAS_TRUTH_PRESENT));
}

/* ==========================================================================
 * The two defects the season's audit found, pinned so they cannot return.
 * ========================================================================== */
static void test_symbol_present_does_not_report_not_found_as_proven_absent(void) {
    /* The A9.2 defect: `atlas.symbol_present` returned FAIL on `count == 0`
     * having checked only that *some* generation existed. Over a partial
     * generation the defining translation unit may be the one that failed to
     * parse — and downstream, FAIL becomes CONTRADICTED at confidence 0, with
     * the deterministic verdict overriding the attestation fold entirely. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    (void)seed_generation(&e, /*complete=*/false, HEAD_COMMIT, &err);

    atlas_verify_check check = ATLAS_CHECK_UNAVAILABLE;
    atlas_verify_truth_reason why = ATLAS_TREASON_NONE;
    atlas_verify_truth t =
        run(&e, ATLAS_VERIFIER_SYMBOL_PRESENT, "symbol=missing", &check, NULL, &why, &err);

    T_CHECK_MSG(check != ATLAS_CHECK_FAIL,
                "a partial index contradicted a claim it had not looked at");
    T_EQ_INT((int)check, (int)ATLAS_CHECK_UNAVAILABLE);
    T_EQ_INT((int)t, (int)ATLAS_TRUTH_UNKNOWN);
    /* And the reason names the actual problem rather than the generic "nothing
     * ran". A model told NOT_EVALUATED would go looking for a verifier; told
     * SEMANTIC_INDEX_INCOMPLETE it knows to reindex. */
    T_EQ_INT((int)why, (int)ATLAS_TREASON_SEMANTIC_INDEX_INCOMPLETE);

    env_close(&e);
}

static void test_proven_edge_does_not_report_a_missing_edge_over_a_partial_index(void) {
    /* The second A9.2 defect, and one step worse than the first: the
     * completeness flag was not merely ignored, it was never gathered. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    int64_t gen = seed_generation(&e, /*complete=*/false, HEAD_COMMIT, &err);
    seed_symbol(&e, gen, "c:@F@a", "a", true, &err);
    seed_symbol(&e, gen, "c:@F@b", "b", true, &err);

    atlas_verify_check check = ATLAS_CHECK_UNAVAILABLE;
    atlas_verify_truth t = run(&e, ATLAS_VERIFIER_PROVEN_EDGE, "from=a;to=b", &check, NULL, NULL,
                               &err);

    T_CHECK_MSG(check != ATLAS_CHECK_FAIL,
                "a missing edge over a partial generation was reported as no call");
    T_EQ_INT((int)t, (int)ATLAS_TRUTH_UNKNOWN);

    /* Over a *complete* generation the same missing edge is a genuine finding —
     * the fix must not have made the verifier useless. */
    EXEC(&e, &err, "UPDATE sem_generations SET tu_failed = 0, tu_complete = 2 WHERE id = %lld;",
             (long long)gen);
    t = run(&e, ATLAS_VERIFIER_PROVEN_EDGE, "from=a;to=b", &check, NULL, NULL, &err);
    T_EQ_INT((int)check, (int)ATLAS_CHECK_FAIL);
    T_EQ_INT((int)t, (int)ATLAS_TRUTH_ABSENT);

    env_close(&e);
}

/* ==========================================================================
 * §29 — adversarial. A model may state a hypothesis; it may not manufacture an
 * authenticated absence proof.
 * ========================================================================== */
static void test_no_intake_path_can_assert_coverage_or_absence(void) {
    /* The guarantee is an **absence of a parameter**, not a check on one. If a
     * field is ever added that could carry a truth value or a coverage state in
     * from a caller, this scan is what notices.
     *
     * `atlas_verify_op` is the whole model-facing intake surface: every MCP
     * tool, every CLI intake verb and every RPC method builds one. Nothing in
     * it may name truth or coverage. */
    static const char *const FORBIDDEN[] = {
        "truth", "coverage", "absent", "present", "complete",
    };
    atlas_buf src = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    {
        FILE *fp = fopen(ATLAS_SRC_DIR "/include/atlas/verify_ops.h", "rb");
        T_REQUIRE(fp != NULL);
        char chunk[8192];
        size_t n;
        while ((n = fread(chunk, 1u, sizeof chunk, fp)) > 0) {
            T_OK(atlas_buf_append(&src, chunk, n, &err), &err);
        }
        (void)fclose(fp);
    }

    /* Only the `atlas_verify_op` struct: the *result* struct legitimately
     * reports truth and coverage back out, and reporting is not accepting. */
    const char *begin = strstr(atlas_buf_cstr(&src), "typedef struct atlas_verify_op {");
    T_REQUIRE(begin != NULL);
    const char *end = strstr(begin, "} atlas_verify_op;");
    T_REQUIRE(end != NULL);

    atlas_buf field = ATLAS_BUF_INIT;
    T_OK(atlas_buf_set(&field, begin, (size_t)(end - begin), &err), &err);
    /* Field declarations only — the prose in this header discusses absence at
     * length and must be free to. A member is `<type> <name>;` at the start of
     * an indented line, so the search is for the name followed by a semicolon. */
    for (size_t i = 0; i < sizeof FORBIDDEN / sizeof FORBIDDEN[0]; i++) {
        char needle[64];
        (void)snprintf(needle, sizeof needle, " %s;", FORBIDDEN[i]);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&field), needle) == NULL,
                    "atlas_verify_op gained a `%s` member: a caller that can name its own "
                    "coverage or truth can manufacture an absence proof",
                    FORBIDDEN[i]);
    }
    atlas_buf_free(&field);
    atlas_buf_free(&src);
}

static void test_a_model_cannot_forge_a_complete_generation(void) {
    /* Coverage is derived from index state and from nothing a caller supplied.
     * The demonstration: the same claim, the same input, the same everything —
     * only the generation's own completeness differs, and only Atlas writes
     * that. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    int64_t gen = seed_generation(&e, /*complete=*/false, HEAD_COMMIT, &err);
    atlas_verify_truth before =
        run(&e, ATLAS_VERIFIER_SYMBOL_ABSENT, "symbol=ghost", NULL, NULL, NULL, &err);
    T_EQ_INT((int)before, (int)ATLAS_TRUTH_UNKNOWN);

    /* Only the indexer can write this row; there is no request that reaches it. */
    EXEC(&e, &err, "UPDATE sem_generations SET tu_failed = 0, tu_complete = 2 WHERE id = %lld;",
             (long long)gen);
    atlas_verify_truth after =
        run(&e, ATLAS_VERIFIER_SYMBOL_ABSENT, "symbol=ghost", NULL, NULL, NULL, &err);
    T_EQ_INT((int)after, (int)ATLAS_TRUTH_ABSENT);

    env_close(&e);
}

static void test_unknown_never_satisfies_a_policy_requiring_absence(void) {
    /* §18. The obligation-remediation policy exists to close an obligation when
     * a symbol is gone. It must not close one when Atlas merely could not
     * look — which is the single most consequential confusion in the system,
     * because the outcome is an outstanding blocker marked done. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);
    seed_index_current(&e, &err);
    /* Partial: Atlas cannot establish the absence. */
    (void)seed_generation(&e, /*complete=*/false, HEAD_COMMIT, &err);

    atlas_buf uid = ATLAS_BUF_INIT;
    propose(&e, ATLAS_DECISION_KIND_OBLIGATION, "remove the debug keystore", &uid, &err);
    int64_t c = claim(&e, &uid, "no debug keystore symbol remains", ATLAS_CLAIM_DESCRIPTIVE,
                      "atlas.symbol_absent", "symbol=debug_keystore", HEAD_COMMIT, &err);

    static const char POLICY[] = "enabled = yes\ndeterministic_enforce = yes\n"
                                 "min_confidence = 0\n"
                                 "allow = OBLIGATION PROPOSED APPROVED atlas.symbol_absent\n";
    atlas_verifypolicy p;
    atlas_verifypolicy_parse_buffer(POLICY, strlen(POLICY), &p);

    atlas_verify_assessment a;
    T_OK(atlas_verify_autolifecycle_run(e.db, &p, c, "proj", &a, &err), &err);

    T_CHECK_MSG(!a.transitioned,
                "an obligation was discharged by a search Atlas could not complete");
    T_EQ_INT((int)a.truth, (int)ATLAS_TRUTH_UNKNOWN);
    T_CHECK_MSG(a.aggregate.verdict != ATLAS_POLICY_AUTO, "UNKNOWN satisfied an AUTO gate");

    atlas_buf_free(&uid);
    env_close(&e);
}

/* ==========================================================================
 * Vocabulary invariants — the zeros, and the single-producer rule.
 * ========================================================================== */
static void test_every_zero_is_the_safe_value(void) {
    T_EQ_INT((int)ATLAS_TRUTH_UNKNOWN, 0);
    T_EQ_INT((int)ATLAS_COVERAGE_UNKNOWN, 0);
    T_EQ_INT((int)ATLAS_TREASON_NONE, 0);
    T_EQ_INT((int)ATLAS_TRUTH_CHANGE_NONE, 0);

    /* A zeroed report must not be sufficient for anything. This is the
     * invariant in its most compressed form: `memset` must never produce a
     * proof of absence. */
    atlas_verify_coverage_report zero;
    memset(&zero, 0, sizeof zero);
    for (size_t i = 0; i < ATLAS_VERIFY_COVERAGE_DIMS; i++) {
        T_CHECK_MSG(!atlas_verify_coverage_sufficient(zero.dims[i]),
                    "a zeroed coverage report satisfied dimension %s",
                    atlas_verify_coverage_dim_name((atlas_verify_coverage_dim)i));
    }
    T_EQ_INT((int)atlas_verify_coverage_summary(&zero), (int)ATLAS_COVERAGE_UNKNOWN);

    /* UNKNOWN is the one state that is neither complete nor a statement about
     * the world, and it must never be sufficient. */
    T_CHECK(!atlas_verify_coverage_sufficient(ATLAS_COVERAGE_UNKNOWN));
    T_CHECK(!atlas_verify_coverage_sufficient(ATLAS_COVERAGE_PARTIAL));
    T_CHECK(!atlas_verify_coverage_sufficient(ATLAS_COVERAGE_STALE));
    T_CHECK(atlas_verify_coverage_sufficient(ATLAS_COVERAGE_COMPLETE));
    T_CHECK(atlas_verify_coverage_sufficient(ATLAS_COVERAGE_NOT_APPLICABLE));

    T_CHECK(!atlas_verify_truth_is_established(ATLAS_TRUTH_UNKNOWN));
    T_CHECK(!atlas_verify_truth_is_established(ATLAS_TRUTH_NOT_VERIFIABLE));
    T_CHECK(atlas_verify_truth_is_established(ATLAS_TRUTH_PRESENT));
    T_CHECK(atlas_verify_truth_is_established(ATLAS_TRUTH_ABSENT));
}

static void test_a_normative_claim_is_not_verifiable_rather_than_unknown(void) {
    /* §13. UNKNOWN says "more evidence would settle it". For "architecture A
     * will be the best design in 2030" no evidence would, and reporting it as
     * UNKNOWN invites somebody to go and look. */
    atlas_verify_coverage_report perfect;
    atlas_verify_coverage_report_init(&perfect);
    for (size_t i = 0; i < ATLAS_VERIFY_COVERAGE_DIMS; i++) {
        perfect.dims[i] = ATLAS_COVERAGE_COMPLETE;
    }
    atlas_verify_truth_reason why = ATLAS_TREASON_NONE;
    atlas_verify_truth t = atlas_verify_truth_of(ATLAS_VERIFIER_SYMBOL_ABSENT,
                                                 ATLAS_VERIFY_BASIS_DETERMINISTIC,
                                                 ATLAS_CLAIM_NORMATIVE, ATLAS_CHECK_PASS, &perfect,
                                                 &why);
    T_EQ_INT((int)t, (int)ATLAS_TRUTH_NOT_VERIFIABLE);
    T_EQ_INT((int)why, (int)ATLAS_TREASON_NOT_FACTUAL);

    /* And a JUDGMENT basis reaches the same answer by the other route. */
    t = atlas_verify_truth_of(ATLAS_VERIFIER_NONE, ATLAS_VERIFY_BASIS_JUDGMENT,
                              ATLAS_CLAIM_DESCRIPTIVE, ATLAS_CHECK_PASS, &perfect, &why);
    T_EQ_INT((int)t, (int)ATLAS_TRUTH_NOT_VERIFIABLE);
}

static void test_every_verifier_declares_what_its_negative_rests_on(void) {
    /* A verifier that can conclude ABSENT and names no dimensions would be one
     * whose absence rests on nothing — the exact hole this season closes. The
     * enumeration is over the real vocabulary, so a verifier added later
     * without deciding cannot ship. */
    for (size_t i = 0; i < atlas_verify_verifier_count(); i++) {
        atlas_verify_verifier v = atlas_verify_verifier_at(i);
        if (v == ATLAS_VERIFIER_NONE) {
            continue;
        }
        bool can_be_absent =
            atlas_verify_verifier_truth_of_check(v, ATLAS_CHECK_PASS) == ATLAS_TRUTH_ABSENT ||
            atlas_verify_verifier_truth_of_check(v, ATLAS_CHECK_FAIL) == ATLAS_TRUTH_ABSENT;
        const atlas_verify_coverage_dim *dims = NULL;
        size_t count = atlas_verify_verifier_absence_dims(v, &dims);
        T_CHECK_MSG(!can_be_absent || count > 0,
                    "%s can conclude ABSENT and declares no coverage dimension for it",
                    atlas_verify_verifier_name(v));
        /* Both directions must be a real answer: a verifier whose PASS and FAIL
         * both mean the same thing is not a verifier. */
        T_CHECK_MSG(atlas_verify_verifier_truth_of_check(v, ATLAS_CHECK_PASS) !=
                        atlas_verify_verifier_truth_of_check(v, ATLAS_CHECK_FAIL),
                    "%s reports the same truth for PASS and FAIL",
                    atlas_verify_verifier_name(v));
        /* UNAVAILABLE is never a finding. */
        T_EQ_INT((int)atlas_verify_verifier_truth_of_check(v, ATLAS_CHECK_UNAVAILABLE),
                 (int)ATLAS_TRUTH_UNKNOWN);
    }
}

static void test_coverage_renders_and_reads_back_exactly(void) {
    /* §28's roundtrip, at the vocabulary level: what is stored must read back
     * as what was stored, or a restored database would carry a different
     * coverage state from the one that was verified. */
    atlas_verify_coverage_report in;
    atlas_verify_coverage_report_init(&in);
    in.dims[ATLAS_COVDIM_SEMANTIC_GENERATION] = ATLAS_COVERAGE_COMPLETE;
    in.dims[ATLAS_COVDIM_INDIRECT_CALLS] = ATLAS_COVERAGE_PARTIAL;
    in.dims[ATLAS_COVDIM_EXTERNAL_CALLERS] = ATLAS_COVERAGE_NOT_APPLICABLE;
    in.dims[ATLAS_COVDIM_REPOSITORY_SNAPSHOT] = ATLAS_COVERAGE_STALE;

    char text[512];
    size_t n = atlas_verify_coverage_render(&in, text, sizeof text);
    T_CHECK(n > 0);

    atlas_verify_coverage_report out;
    T_CHECK(atlas_verify_coverage_parse_detail(text, &out));
    for (size_t i = 0; i < ATLAS_VERIFY_COVERAGE_DIMS; i++) {
        T_CHECK_MSG(in.dims[i] == out.dims[i], "dimension %s did not survive the roundtrip",
                    atlas_verify_coverage_dim_name((atlas_verify_coverage_dim)i));
    }

    /* An unrecognised dimension leaves UNKNOWN rather than the caller's value:
     * a row from a newer Atlas is read conservatively, never optimistically. */
    atlas_verify_coverage_report future;
    (void)atlas_verify_coverage_parse_detail("a_dimension_from_the_future=COMPLETE", &future);
    for (size_t i = 0; i < ATLAS_VERIFY_COVERAGE_DIMS; i++) {
        T_EQ_INT((int)future.dims[i], (int)ATLAS_COVERAGE_UNKNOWN);
    }
}

/* ==========================================================================
 * §28 — backup and restore must round-trip the new persisted semantics
 * exactly.
 *
 * The reason this is not covered by A5's existing checks: `PRAGMA
 * integrity_check` walks pages, and `atlas_db_backup_inspect` rehashes decision
 * revisions — neither of them reads a `verify_results` row. So a truth value or
 * a coverage detail that failed to survive a restore would leave a structurally
 * perfect database that quietly answers a different question, which is exactly
 * the class of fault A5 says it cannot detect. Checked here by comparing the
 * rows themselves.
 * ========================================================================== */
static void test_truth_and_coverage_survive_a_backup_and_restore(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);
    seed_index_current(&e, &err);

    /* One result per truth value, with a different coverage shape on each, so a
     * roundtrip that dropped or defaulted any of them is visible. */
    static const struct {
        const char *truth;
        const char *reason;
        const char *summary;
        const char *detail;
    } ROWS[] = {
        {"PRESENT", "ESTABLISHED", "COMPLETE",
         "semantic_generation=COMPLETE;indirect_calls=NOT_APPLICABLE"},
        {"ABSENT", "ESTABLISHED", "COMPLETE",
         "semantic_generation=COMPLETE;indirect_calls=COMPLETE;external_callers=NOT_APPLICABLE"},
        {"UNKNOWN", "INDIRECT_CALLS_UNRESOLVED", "PARTIAL",
         "semantic_generation=COMPLETE;indirect_calls=PARTIAL"},
        {"UNKNOWN", "SEMANTIC_INDEX_STALE", "STALE", "semantic_generation=STALE"},
        {"NOT_VERIFIABLE", "NOT_FACTUAL", "NOT_APPLICABLE", "runtime_state=NOT_APPLICABLE"},
    };

    atlas_buf uid = ATLAS_BUF_INIT;
    propose(&e, ATLAS_DECISION_KIND_OBLIGATION, "a record to hang claims on", &uid, &err);
    for (size_t i = 0; i < sizeof ROWS / sizeof ROWS[0]; i++) {
        int64_t c = claim(&e, &uid, "a proposition", ATLAS_CLAIM_DESCRIPTIVE, NULL, NULL,
                          HEAD_COMMIT, &err);
        EXEC(&e, &err,
             "INSERT INTO verify_results(claim_id, state, basis, confidence_score, calibration,"
             "  algorithm, family_version, created_at, truth, truth_reason, coverage_summary,"
             "  coverage_detail)"
             " VALUES(%lld, 'VERIFIED', 'DETERMINISTIC', 100, 'INSUFFICIENT_DATA',"
             "        'atlas-reliability-v1', 1, '2026-01-01T00:00:00Z', '%s', '%s', '%s', '%s');",
             (long long)c, ROWS[i].truth, ROWS[i].reason, ROWS[i].summary, ROWS[i].detail);
    }
    /* The database must be closed before it is copied and replaced: the restore
     * takes the data-directory lock exclusively, which is what proves no daemon
     * is running. */
    atlas_db_close(e.db);
    e.db = NULL;

    char backup_path[1024];
    (void)snprintf(backup_path, sizeof backup_path, "%s/roundtrip.atlasbak",
                   fx_data_dir(&e.fx));
    atlas_backup_create_opts copts;
    memset(&copts, 0, sizeof copts);
    copts.output = backup_path;
    atlas_backup_report brep;
    atlas_backup_report_init(&brep);
    T_OK(atlas_service_backup_create(fx_data_dir(&e.fx), &copts, &brep, &err), &err);
    atlas_backup_report_free(&brep);

    atlas_backup_restore_opts ropts;
    memset(&ropts, 0, sizeof ropts);
    ropts.input = backup_path;
    ropts.confirmed = true;
    atlas_backup_restore_report rrep;
    atlas_backup_restore_report_init(&rrep);
    T_OK(atlas_service_backup_restore(fx_data_dir(&e.fx), &ropts, &rrep, &err), &err);
    atlas_backup_restore_report_free(&rrep);

    T_OK(atlas_db_open(atlas_buf_cstr(&e.db_path), &e.db, &err), &err);

    /* Every column, compared to what was written. A count would pass on a
     * database that had defaulted every value to its zero — which is the one
     * failure mode a migration with zero defaults makes easy to miss. */
    static const char SQL[] = "SELECT truth, truth_reason, coverage_summary, coverage_detail"
                              "  FROM verify_results ORDER BY id;";
    sqlite3_stmt *stmt = NULL;
    T_OK(atlas_db_prepare(e.db, SQL, &stmt, &err), &err);
    size_t seen = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && seen < sizeof ROWS / sizeof ROWS[0]) {
        const char *truth = (const char *)sqlite3_column_text(stmt, 0);
        const char *reason = (const char *)sqlite3_column_text(stmt, 1);
        const char *summary = (const char *)sqlite3_column_text(stmt, 2);
        const char *detail = (const char *)sqlite3_column_text(stmt, 3);
        T_CHECK_MSG(truth != NULL && strcmp(truth, ROWS[seen].truth) == 0,
                    "row %zu truth: wrote %s, read %s", seen, ROWS[seen].truth,
                    truth != NULL ? truth : "(null)");
        T_CHECK_MSG(reason != NULL && strcmp(reason, ROWS[seen].reason) == 0,
                    "row %zu reason: wrote %s, read %s", seen, ROWS[seen].reason,
                    reason != NULL ? reason : "(null)");
        T_CHECK_MSG(summary != NULL && strcmp(summary, ROWS[seen].summary) == 0,
                    "row %zu coverage summary: wrote %s, read %s", seen, ROWS[seen].summary,
                    summary != NULL ? summary : "(null)");
        T_CHECK_MSG(detail != NULL && strcmp(detail, ROWS[seen].detail) == 0,
                    "row %zu coverage detail: wrote %s, read %s", seen, ROWS[seen].detail,
                    detail != NULL ? detail : "(null)");
        /* And the detail still parses back into the same report, so what
         * survived is usable rather than merely present. */
        atlas_verify_coverage_report parsed;
        T_CHECK(atlas_verify_coverage_parse_detail(detail, &parsed));
        seen++;
    }
    atlas_db_finish(e.db, stmt);
    T_CHECK_MSG(seen == sizeof ROWS / sizeof ROWS[0], "expected %zu results after restore, saw %zu",
                sizeof ROWS / sizeof ROWS[0], seen);

    atlas_buf_free(&uid);
    env_close(&e);
}

static const atlas_test TESTS[] = {
    {"fixture a one caller over a partial index is present",
     test_fixture_a_one_caller_over_a_partial_index_is_present},
    {"fixture b zero callers with an escaping address is unknown",
     test_fixture_b_zero_callers_with_an_escaping_address_is_unknown},
    {"fixture c a bounded complete absence is absent",
     test_fixture_c_a_bounded_complete_absence_is_absent},
    {"external linkage keeps an absence unknown",
     test_external_linkage_keeps_an_absence_unknown},
    {"fixture d repository absence is not operational absence",
     test_fixture_d_repository_absence_is_not_operational_absence},
    {"fixture e a complete registry establishes absence",
     test_fixture_e_a_complete_registry_establishes_absence},
    {"fixture f a stale index cannot establish absence",
     test_fixture_f_a_stale_index_cannot_establish_absence},
    {"an unindexed repository says so rather than reporting incompleteness",
     test_an_unindexed_repository_says_so_rather_than_reporting_incompleteness},
    {"fixture g an absence does not survive the source moving",
     test_fixture_g_an_absence_does_not_survive_the_source_moving},
    {"fixture h agreeing agents do not establish absence",
     test_fixture_h_agreeing_agents_do_not_establish_absence},
    {"fixture i unknown becoming present is not a verifier error",
     test_fixture_i_unknown_becoming_present_is_not_a_verifier_error},
    {"fixture j absent contradicted at one snapshot is an error",
     test_fixture_j_absent_contradicted_at_one_snapshot_is_an_error},
    {"symbol present does not report not found as proven absent",
     test_symbol_present_does_not_report_not_found_as_proven_absent},
    {"proven edge does not report a missing edge over a partial index",
     test_proven_edge_does_not_report_a_missing_edge_over_a_partial_index},
    {"no intake path can assert coverage or absence",
     test_no_intake_path_can_assert_coverage_or_absence},
    {"a model cannot forge a complete generation",
     test_a_model_cannot_forge_a_complete_generation},
    {"unknown never satisfies a policy requiring absence",
     test_unknown_never_satisfies_a_policy_requiring_absence},
    {"every zero is the safe value",
     test_every_zero_is_the_safe_value},
    {"a normative claim is not verifiable rather than unknown",
     test_a_normative_claim_is_not_verifiable_rather_than_unknown},
    {"every verifier declares what its negative rests on",
     test_every_verifier_declares_what_its_negative_rests_on},
    {"coverage renders and reads back exactly",
     test_coverage_renders_and_reads_back_exactly},
    {"truth and coverage survive a backup and restore",
     test_truth_and_coverage_survive_a_backup_and_restore},
};

ATLAS_TEST_MAIN("verify_absence", TESTS)
