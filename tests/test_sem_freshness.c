/* Atlas - A9.2.3: semantic freshness, coverage and the rebuild decision.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The claims this suite exists to hold, and what goes wrong without each:
 *
 *   1. **A generation records what it was built from, and freshness notices
 *      when that moves.** Every A8-CI staleness check compares something that
 *      moves with a *commit*; Atlas indexes the working tree. Without the
 *      source identity, an uncommitted edit leaves the index reporting itself
 *      CURRENT for as long as nobody commits — which is most of a working day.
 *   2. **A pass that finds nothing to do still records that it looked.** A
 *      repository holds sources the compilation database does not name. Editing
 *      one moves the live identity, moves no unit digest, and publishes no
 *      generation — so without re-stamping, the repository is stale again on the
 *      next tick and rebuilds on every sweep for ever. This is the defect that
 *      would have made the acceptance repository unusable, and it is pinned here
 *      rather than discovered there.
 *   3. **Coverage is a second axis and is measured against the tree, not
 *      against the compilation database's own contents.** `2/2 units` over a
 *      three-source repository is not complete coverage, and a generation that
 *      says it is cannot be allowed to support an absence.
 *   4. **The rebuild decision is derived, and refuses by default.** No build
 *      description means no compiler runs, which is what keeps A8-CI's rule
 *      alive after repository changes became a rebuild trigger.
 *
 * Everything here is synthetic: a fixture repository, a fixture compilation
 * database and an isolated data directory. Nothing reaches a live daemon, a
 * live socket, a real database or a registered repository.
 */
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "atlas/datadir.h"
#include "atlas/db.h"
#include "atlas/git.h"
#include "atlas/reconcile.h"
#include "atlas/sem.h"
#include "atlas/sem_ops.h"
#include "atlas/sem_discover.h"
#include "atlas/sem_schedule.h"
#include "atlas/service.h"
#include "atlas/verify.h"
#include "atlas_test.h"
#include "support/fixture.h"

/* --- the environment -------------------------------------------------------- */

typedef struct env {
    fixture fx;
    atlas_db *db;
    atlas_git *g;
    int64_t repo_id;
    atlas_buf exe;
} env;

static void env_open(env *e, atlas_err *err) {
    memset(e, 0, sizeof(*e));
    atlas_buf_init(&e->exe);
    /* The built binary, which is also the child parser. `ATLAS_BIN` is the
     * compile-time path, so a test never reaches an installed Atlas. */
    T_OK(atlas_buf_set_str(&e->exe, ATLAS_BIN, err), err);
    T_OK(fx_open(&e->fx, err), err);
    T_OK(fx_init_repo(&e->fx, fx_repo(&e->fx), NULL, err), err);

    atlas_buf db_path = ATLAS_BUF_INIT;
    T_OK(atlas_datadir_ensure(fx_data_dir(&e->fx), err), err);
    T_OK(atlas_datadir_db_path(fx_data_dir(&e->fx), &db_path, err), err);
    T_OK(atlas_db_open(atlas_buf_cstr(&db_path), &e->db, err), err);
    T_OK(atlas_db_migrate(e->db, err), err);
    atlas_buf_free(&db_path);

    T_OK(atlas_git_open(fx_repo(&e->fx), &e->g, err), err);
    const char *root = atlas_git_root(e->g);
    atlas_repo_identity id;
    memset(&id, 0, sizeof(id));
    id.root = root;
    id.root_len = strlen(root);
    id.common_dir = atlas_git_common_dir(e->g);
    id.common_dir_len = strlen((const char *)id.common_dir);
    id.git_dir = atlas_git_dir(e->g);
    id.git_dir_len = strlen((const char *)id.git_dir);
    id.object_format = atlas_git_object_format(e->g);
    T_OK(atlas_db_repo_add(e->db, "fx", &id, &e->repo_id, err), err);
}

static void env_close(env *e) {
    atlas_git_close(e->g);
    atlas_db_close(e->db);
    atlas_buf_free(&e->exe);
    fx_close(&e->fx);
}

/* The file index must exist before anything semantic: the source identity and
 * every unit's input digest are computed from content hashes it records. */
static void run_file_pass(env *e, atlas_err *err) {
    atlas_reconcile_opts opts;
    atlas_reconcile_opts_init(&opts);
    opts.full = true;
    atlas_reconcile_summary sum;
    atlas_reconcile_summary_init(&sum);
    T_OK(atlas_reconcile_run(e->db, e->g, e->repo_id, &opts, &sum, err), err);
}

static void write_compdb(env *e, const char *const *sources, size_t n, atlas_err *err) {
    atlas_buf doc = ATLAS_BUF_INIT;
    T_OK(atlas_buf_append_str(&doc, "[", err), err);
    for (size_t i = 0; i < n; i++) {
        T_OK(atlas_buf_appendf(&doc, err,
                               "%s{\"directory\":\"%s\","
                               "\"arguments\":[\"cc\",\"-I\",\"include\",\"-std=gnu11\","
                               "\"-c\",\"%s\"],"
                               "\"file\":\"%s\"}",
                               i == 0 ? "" : ",", fx_repo(&e->fx), sources[i], sources[i]),
             err);
    }
    T_OK(atlas_buf_append_str(&doc, "]", err), err);
    T_OK(fx_write(fx_repo(&e->fx), "compile_commands.json", atlas_buf_cstr(&doc), err), err);
    atlas_buf_free(&doc);
}

static void index_once(env *e, const char *test_roots, atlas_sem_index_summary *sum,
                       atlas_err *err) {
    atlas_sem_index_opts o;
    atlas_sem_index_opts_init(&o);
    o.compdbs = "compile_commands.json";
    o.compdbs_len = strlen("compile_commands.json") + 1u;
    o.atlas_exe = atlas_buf_cstr(&e->exe);
    o.root = atlas_git_root(e->g);
    o.commit_id = "";
    o.repo_identity_hash = "";
    o.test_roots = test_roots;
    int fd = open(atlas_git_root(e->g), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    T_REQUIRE_MSG(fd >= 0, "cannot open the fixture repository root");
    o.root_fd = fd;
    atlas_sem_index_summary_init(sum);
    atlas_status st = atlas_sem_index_run(e->db, e->repo_id, &o, sum, err);
    (void)close(fd);
    T_OK(st, err);
}

static void repo_of(env *e, atlas_repo_info *out, atlas_err *err) {
    bool found = false;
    atlas_repo_info_init(out);
    T_OK(atlas_db_repo_get(e->db, "fx", out, &found, err), err);
    T_REQUIRE_MSG(found, "the fixture repository is not registered");
}

/* A9.2.4. Every plan in this file is computed with the machine-wide default
 * supplied explicitly rather than read from `/etc/atlas/system.conf`.
 *
 * `atlas_sem_plan_for` reads the root-owned policy, which is right for the
 * shipped binary and wrong for a test: the answers would depend on how the
 * machine running the suite happens to be configured, which is the one thing a
 * test must never depend on. `plan_of` asks with the default ON — the shipped
 * `ATLAS_SEM_AUTO_DEFAULT` — and `plan_of_with_default` drives both. */
static void plan_of_with_default(env *e, bool policy_default, atlas_sem_plan *out,
                                 atlas_err *err) {
    atlas_repo_info info;
    repo_of(e, &info, err);
    T_OK(atlas_sem_plan_for_with_default(e->db, &info, false, policy_default, out, err), err);
    atlas_repo_info_free(&info);
}

static void plan_of(env *e, atlas_sem_plan *out, atlas_err *err) {
    plan_of_with_default(e, ATLAS_SEM_AUTO_DEFAULT, out, err);
}

/* A9.2.4. Runs one bounded discovery walk, because nothing is an accepted build
 * input until a walk has accepted it. A test that wrote a compilation database
 * and expected the scheduler to see it would be testing A9.2.3's model. */
static void discover(env *e, atlas_err *err) {
    atlas_repo_info info;
    repo_of(e, &info, err);
    atlas_sem_discovery_result res;
    atlas_sem_discovery_result_init(&res);
    T_OK(atlas_sem_discovery_run(e->db, &info, &res, err), err);
    atlas_repo_info_free(&info);
}

/* --- the vocabularies ------------------------------------------------------- */

static void test_every_zero_is_the_safe_reading(void) {
    /* The rule every Atlas vocabulary follows, asserted rather than assumed: a
     * zeroed struct must never describe a healthy index, and — the one that
     * matters most — must never cause a compiler to run. */
    atlas_sem_plan p;
    memset(&p, 0xff, sizeof p);
    atlas_sem_plan_init(&p);
    T_CHECK(p.activity == ATLAS_SEM_ACT_UNKNOWN);
    T_CHECK(p.freshness == ATLAS_SEM_FRESH_ABSENT);
    T_CHECK(p.scope_discovery == ATLAS_SEM_SCOPE_UNKNOWN);
    T_CHECK(!p.coverage_complete);
    T_CHECK(!p.should_build);
    T_CHECK(!p.auto_rebuild);
    T_CHECK(!p.configured);

    atlas_sem_generation g;
    memset(&g, 0xff, sizeof g);
    atlas_sem_generation_init(&g);
    T_CHECK(g.scope_discovery == ATLAS_SEM_SCOPE_UNKNOWN);
    T_CHECK(g.scope_candidates == 0);
    T_CHECK(g.scope_uncovered == 0);
    T_CHECK(!g.test_scope_known);
    T_CHECK(g.source_identity[0] == '\0');

    T_CHECK(strcmp(atlas_sem_activity_name(ATLAS_SEM_ACT_UNKNOWN), "UNKNOWN") == 0);
    T_CHECK(strcmp(atlas_sem_scope_discovery_name(ATLAS_SEM_SCOPE_UNKNOWN), "UNKNOWN") == 0);
}

static void test_a_test_root_matches_on_a_component_boundary(void) {
    /* A substring match would classify `tests_helper.c` as a test because
     * `tests` is a declared root — and a production source misclassified as a
     * test is wrong in the one direction that matters: it would let "no
     * production caller" be answered ABSENT while a production caller sits in
     * the file it excluded. */
    T_CHECK(atlas_sem_path_is_test("tests", "tests/a.c"));
    T_CHECK(atlas_sem_path_is_test("tests", "tests"));
    T_CHECK(atlas_sem_path_is_test("tests/", "tests/deep/b.c"));
    T_CHECK(!atlas_sem_path_is_test("tests", "tests_helper.c"));
    T_CHECK(!atlas_sem_path_is_test("tests", "src/tests/a.c"));
    T_CHECK(!atlas_sem_path_is_test("tests", "src/a.c"));
    /* No declared roots is not "there are no tests". */
    T_CHECK(!atlas_sem_path_is_test("", "tests/a.c"));
    T_CHECK(!atlas_sem_path_is_test(NULL, "tests/a.c"));
    /* Several roots, and the second one still matches. */
    T_CHECK(atlas_sem_path_is_test("tests\nspec", "spec/x.c"));
}

/* --- A9.2.5 / GAP-4: nested test roots -------------------------------------
 *
 * A declared root is matched from the start of the path on a component
 * boundary, so a test directory nested inside another directory has to be
 * declared by its full relative path. That is the model working as specified —
 * "Atlas does not guess which sources are tests" — and the season's requirement
 * is that it be *provable*, generically, with no repository name anywhere near
 * the code.
 *
 * The names below are deliberately arbitrary. Nothing in Atlas may know what any
 * particular repository calls its test directories. */
static void test_a_nested_test_root_is_matched_when_it_is_declared(void) {
    /* Declared by its full relative path: everything under it is a test. */
    T_CHECK(atlas_sem_path_is_test("sub/tests", "sub/tests/a.c"));
    T_CHECK(atlas_sem_path_is_test("sub/tests", "sub/tests/deep/b.c"));
    T_CHECK(atlas_sem_path_is_test("sub/tests/", "sub/tests/a.c"));
    T_CHECK(atlas_sem_path_is_test("sub/tests", "sub/tests"));

    /* The component boundary holds at depth too: a sibling whose name merely
     * starts with the root's last component is production, and misclassifying it
     * as a test is wrong in the one direction that matters — it would let a
     * "no production caller" answer exclude a file that holds one. */
    T_CHECK(!atlas_sem_path_is_test("sub/tests", "sub/tests_helper.c"));
    T_CHECK(!atlas_sem_path_is_test("sub/tests", "sub/testsuite/a.c"));
    T_CHECK(!atlas_sem_path_is_test("sub/tests", "other/sub/tests/a.c"));

    /* Several nested roots, each independent. */
    const char *roots = "one/tests\ntwo/spec\nthree/deep/t";
    T_CHECK(atlas_sem_path_is_test(roots, "one/tests/a.c"));
    T_CHECK(atlas_sem_path_is_test(roots, "two/spec/b.c"));
    T_CHECK(atlas_sem_path_is_test(roots, "three/deep/t/c.c"));
    T_CHECK(!atlas_sem_path_is_test(roots, "one/spec/a.c"));
    T_CHECK(!atlas_sem_path_is_test(roots, "three/deep/tools/c.c"));
}

static void test_an_undeclared_nested_test_directory_is_not_guessed(void) {
    /* **The rule, stated as a test.** A directory named `tests` sitting inside
     * another directory is *not* a test root unless somebody declared it, and
     * Atlas does not promote it on the strength of its name. Declaring the
     * top-level `tests` says nothing about `sub/tests`.
     *
     * This is deliberately the behaviour rather than a defect to fix: a
     * heuristic that guessed would classify a production source as a test the
     * first time a repository used the word differently, and a production source
     * excluded from a production-scope absence is the failure that matters. What
     * A9.2.5 adds is not a guess — it is that the consequence is visible, and
     * that no negative conclusion may rest on the classification being complete
     * (see `test_declaring_one_test_root_does_not_prove_they_are_all_declared`). */
    T_CHECK(!atlas_sem_path_is_test("tests", "sub/tests/a.c"));
    T_CHECK(!atlas_sem_path_is_test("tests", "vendor/tests/a.c"));
    /* And the generated/ignored case: a path under a build directory is neither
     * guessed as a test nor as anything else by this function. Classification is
     * declaration and nothing else. */
    T_CHECK(!atlas_sem_path_is_test("tests", "build/tests/generated.c"));
    T_CHECK(!atlas_sem_path_is_test("", "build/tests/generated.c"));

    /* With no declared roots at all, nothing is a test — which is "Atlas does
     * not know which sources are tests", not "there are no tests". The
     * generation records that separately as `test_scope_known`. */
    T_CHECK(!atlas_sem_path_is_test("", "tests/a.c"));
    T_CHECK(!atlas_sem_path_is_test(NULL, "sub/tests/a.c"));
}

static void test_declaring_one_test_root_does_not_prove_they_are_all_declared(void) {
    /* A9.2.5's epistemic requirement, and the reason the matcher above is not a
     * defect: **an operator declaring a test root is not evidence that they
     * declared every test root.** It is the same shape as A9.2.4's rule that a
     * pinned compilation-database list is not a completeness claim.
     *
     * So `ATLAS_COVDIM_TESTS` must never be established by any verifier. It is
     * UNKNOWN — the enum's zero — for every claim, which makes any absence that
     * would depend on the test/production split UNAVAILABLE rather than ABSENT.
     * That is checked here rather than left to hold by accident, because the
     * failure mode is silent: somebody sets the dimension COMPLETE from a
     * non-empty root list and a whole class of negative answers quietly becomes
     * believable. */
    atlas_verify_coverage_report cov;
    memset(&cov, 0, sizeof cov);
    T_CHECK_MSG(cov.dims[ATLAS_COVDIM_TESTS] == ATLAS_COVERAGE_UNKNOWN,
                "a zeroed coverage report must leave the tests dimension UNKNOWN");

    /* A dimension set is not sufficient unless it is COMPLETE, and nothing in
     * Atlas sets this one. The guard is that `atlas_verify_coverage_satisfies`
     * refuses UNKNOWN. */
    const atlas_verify_coverage_dim dims[] = {ATLAS_COVDIM_TESTS};
    atlas_verify_coverage_dim failed = ATLAS_COVDIM_SEMANTIC_GENERATION;
    atlas_verify_truth_reason why = ATLAS_TREASON_COVERAGE_UNKNOWN;
    T_CHECK_MSG(!atlas_verify_coverage_satisfies(&cov, dims, 1u, &failed, &why),
                "an absence resting on the tests dimension must not be satisfiable");
    T_CHECK(failed == ATLAS_COVDIM_TESTS);
}

static void test_a_build_description_round_trips_and_refuses_a_newline(void) {
    atlas_err err;
    atlas_err_init(&err);
    const char *items[] = {"a/compile_commands.json", "b/compile_commands.json"};
    atlas_buf packed = ATLAS_BUF_INIT;
    T_OK(atlas_sem_config_pack(items, 2, &packed, &err), &err);
    T_CHECK(strcmp(atlas_buf_cstr(&packed), "a/compile_commands.json\nb/compile_commands.json") ==
            0);

    atlas_buf list = ATLAS_BUF_INIT;
    size_t n = 0;
    T_OK(atlas_sem_config_unpack(atlas_buf_cstr(&packed), &list, &n, &err), &err);
    T_EQ_INT((int64_t)n, 2);
    /* The NUL-separated form the indexer already takes, so the durable
     * configuration feeds it without a third representation in between. */
    const char *p = (const char *)list.data;
    T_CHECK(strcmp(p, items[0]) == 0);
    T_CHECK(strcmp(p + strlen(p) + 1u, items[1]) == 0);

    /* Refused, not truncated at the newline: truncating would name a different
     * file, and the restriction is stated rather than hidden. */
    const char *bad[] = {"a\nb"};
    atlas_buf out = ATLAS_BUF_INIT;
    T_FAILS_WITH(atlas_sem_config_pack(bad, 1, &out, &err), ATLAS_ERR_USAGE, &err);

    atlas_buf_free(&out);
    atlas_buf_free(&list);
    atlas_buf_free(&packed);
}

/* --- freshness -------------------------------------------------------------- */

static void test_an_empty_stored_identity_never_makes_a_generation_stale(void) {
    /* Migration 18's conservatism, at the function that acts on it. A
     * pre-A9.2.3 generation recorded nothing to compare, and "this index did
     * not record what it was built from" is not evidence that the tree moved.
     * Relabelling it stale would be inventing a fact; rebuilding it is what
     * actually happens, and that is the scheduler's business, not this one's. */
    atlas_sem_generation g;
    atlas_sem_generation_init(&g);
    g.status = ATLAS_SEM_GEN_COMPLETE;
    (void)snprintf(g.analyzer_id, sizeof g.analyzer_id, "%s", ATLAS_SEM_ANALYZER_ID);
    g.analyzer_version = ATLAS_SEM_ANALYZER_VERSION;

    const char *reason = NULL;
    T_CHECK(atlas_sem_freshness_of(&g, true, false, "", "", "", "abc", NULL, true, &reason) ==
            ATLAS_SEM_FRESH_CURRENT);
    T_CHECK(reason == NULL);

    /* And with an identity recorded, a different one is STALE with its own
     * reason — the working tree, not the commit. */
    (void)snprintf(g.source_identity, sizeof g.source_identity, "%s", "abc");
    T_CHECK(atlas_sem_freshness_of(&g, true, false, "", "", "", "def", NULL, true, &reason) ==
            ATLAS_SEM_FRESH_STALE);
    T_CHECK(reason != NULL && strcmp(reason, ATLAS_SEM_STALE_SOURCE) == 0);
    T_CHECK(atlas_sem_stale_reason_is_known(ATLAS_SEM_STALE_SOURCE));

    /* An empty *live* identity does not either: "Atlas could not look" is not
     * evidence that anything changed. */
    T_CHECK(atlas_sem_freshness_of(&g, true, false, "", "", "", "", NULL, true, &reason) ==
            ATLAS_SEM_FRESH_CURRENT);
}

static void test_an_uncommitted_edit_makes_the_index_stale(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    T_OK(fx_mkdir(fx_repo(&e.fx), "include", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "include/api.h", "int f(int);\n", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "#include \"api.h\"\nint f(int x){return x;}\n", &err),
         &err);
    const char *srcs[] = {"a.c"};
    write_compdb(&e, srcs, 1, &err);
    run_file_pass(&e, &err);

    atlas_sem_index_summary sum;
    index_once(&e, NULL, &sum, &err);
    T_CHECK(sum.published);

    atlas_sem_plan p;
    plan_of(&e, &p, &err);
    T_CHECK_MSG(p.freshness == ATLAS_SEM_FRESH_CURRENT, "a fresh index is not current");

    /* The edit that every A8-CI check misses: no commit, no compilation
     * database change, no toolchain change. */
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "#include \"api.h\"\nint f(int x){return x+1;}\n", &err),
         &err);
    run_file_pass(&e, &err);

    plan_of(&e, &p, &err);
    T_CHECK_MSG(p.freshness == ATLAS_SEM_FRESH_STALE,
                "an uncommitted edit left the semantic index reporting itself current");
    T_CHECK(p.stale_reason != NULL && strcmp(p.stale_reason, ATLAS_SEM_STALE_SOURCE) == 0);

    env_close(&e);
}

/* --- the no-change loop ----------------------------------------------------- */

static void test_a_no_change_pass_records_that_it_looked(void) {
    /* The regression this suite exists for.
     *
     * `b.c` is in the repository and not in the compilation database, which is
     * the ordinary case on a real tree — the acceptance repository has hundreds.
     * Editing it moves the live source identity, moves no unit digest and moves
     * no scope count, so the pass finds nothing to do and publishes no
     * generation. Without re-stamping the stored identity, the repository is
     * stale again immediately and rebuilds on every sweep for ever. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    T_OK(fx_mkdir(fx_repo(&e.fx), "include", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "include/api.h", "int f(int);\n", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "#include \"api.h\"\nint f(int x){return x;}\n", &err),
         &err);
    /* Named by nothing. */
    T_OK(fx_write(fx_repo(&e.fx), "b.c", "int g(void){return 1;}\n", &err), &err);
    const char *srcs[] = {"a.c"};
    write_compdb(&e, srcs, 1, &err);
    run_file_pass(&e, &err);

    atlas_sem_index_summary sum;
    index_once(&e, NULL, &sum, &err);
    T_CHECK(sum.published);
    const int64_t first_gen = sum.generation_id;

    /* Coverage says so honestly: one of two sources read. */
    atlas_sem_plan p;
    plan_of(&e, &p, &err);
    T_EQ_INT(p.scope_candidates, 2);
    T_EQ_INT(p.scope_covered, 1);
    T_EQ_INT(p.scope_uncovered, 1);
    T_CHECK_MSG(!p.coverage_complete,
                "a generation that read one of two sources reported complete coverage");
    T_CHECK(p.freshness == ATLAS_SEM_FRESH_CURRENT);

    /* Edit the file nothing compiles. */
    T_OK(fx_write(fx_repo(&e.fx), "b.c", "int g(void){return 2;}\n", &err), &err);
    run_file_pass(&e, &err);
    plan_of(&e, &p, &err);
    T_CHECK_MSG(p.freshness == ATLAS_SEM_FRESH_STALE,
                "an edit to an uncompiled source did not make the index stale");

    /* One pass, which does no work and publishes no new generation. */
    index_once(&e, NULL, &sum, &err);
    T_CHECK_MSG(sum.no_change, "the pass rebuilt a generation whose every input was unchanged");
    T_EQ_INT(sum.generation_id, first_gen);

    /* And the repository is current again, so nothing schedules another. */
    plan_of(&e, &p, &err);
    T_CHECK_MSG(p.freshness == ATLAS_SEM_FRESH_CURRENT,
                "a no-change pass left the repository stale, so it would rebuild for ever");
    T_EQ_INT(p.generation_id, first_gen);

    env_close(&e);
}

/* --- the coverage manifest -------------------------------------------------- */

static void test_coverage_is_measured_against_the_tree_not_the_build_description(void) {
    /* The overclaim the manifest exists to end: `2/2 units` over a three-source
     * repository is not complete coverage, and only a denominator taken from
     * the file index can say so. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    T_OK(fx_mkdir(fx_repo(&e.fx), "include", &err), &err);
    T_OK(fx_mkdir(fx_repo(&e.fx), "tests", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "include/api.h", "int f(int);\n", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "#include \"api.h\"\nint f(int x){return x;}\n", &err),
         &err);
    T_OK(fx_write(fx_repo(&e.fx), "tests/t.c", "#include \"api.h\"\nint t(void){return f(1);}\n",
                  &err),
         &err);
    T_OK(fx_write(fx_repo(&e.fx), "unnamed.c", "int u(void){return 0;}\n", &err), &err);
    const char *srcs[] = {"a.c", "tests/t.c"};
    write_compdb(&e, srcs, 2, &err);
    run_file_pass(&e, &err);

    atlas_sem_index_summary sum;
    index_once(&e, "tests", &sum, &err);
    T_CHECK(sum.published);

    atlas_sem_generation g;
    bool found = false;
    T_OK(atlas_db_sem_current(e.db, e.repo_id, &g, &found, &err), &err);
    T_REQUIRE_MSG(found, "no generation was published");

    T_EQ_INT(g.tu_total, 2);
    T_EQ_INT(g.tu_complete, 2);
    /* Every unit the compilation database named parsed, and the repository is
     * still a third unread. The two numbers say different things and are never
     * summed. */
    T_EQ_INT(g.scope_candidates, 3);
    T_EQ_INT(g.scope_covered, 2);
    T_EQ_INT(g.scope_uncovered, 1);
    T_CHECK(g.scope_discovery == ATLAS_SEM_SCOPE_DECLARED);

    /* The test/production split, from the operator's declared roots and from
     * nothing else. */
    T_CHECK_MSG(g.test_scope_known, "declared test roots did not make the unit scope known");
    T_EQ_INT(g.tu_test, 1);
    T_EQ_INT(g.tu_production, 1);

    env_close(&e);
}

static void test_without_declared_test_roots_the_unit_scope_is_unknown(void) {
    /* Not "there are no tests". Atlas does not guess: a directory called
     * `tests` is a directory somebody named, and classifying on that basis
     * would invent the scope information a production-only absence rests on. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    T_OK(fx_mkdir(fx_repo(&e.fx), "include", &err), &err);
    T_OK(fx_mkdir(fx_repo(&e.fx), "tests", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "include/api.h", "int f(int);\n", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "#include \"api.h\"\nint f(int x){return x;}\n", &err),
         &err);
    T_OK(fx_write(fx_repo(&e.fx), "tests/t.c", "#include \"api.h\"\nint t(void){return f(1);}\n",
                  &err),
         &err);
    const char *srcs[] = {"a.c", "tests/t.c"};
    write_compdb(&e, srcs, 2, &err);
    run_file_pass(&e, &err);

    atlas_sem_index_summary sum;
    index_once(&e, NULL, &sum, &err);

    atlas_sem_generation g;
    bool found = false;
    T_OK(atlas_db_sem_current(e.db, e.repo_id, &g, &found, &err), &err);
    T_REQUIRE(found);
    T_CHECK_MSG(!g.test_scope_known, "Atlas classified a directory nobody declared as tests");
    T_EQ_INT(g.tu_test, 0);
    T_EQ_INT(g.tu_production, 0);

    env_close(&e);
}

/* --- the rebuild decision --------------------------------------------------- */

static void test_no_build_input_means_no_compiler_runs(void) {
    /* A8-CI's rule, kept alive after **A9.2.4 reversed the activation default**.
     *
     * A9.2.3 kept it by refusing to run for a repository nobody had configured.
     * That is no longer the rule, so this asserts what is: a repository with no
     * discoverable compilation database runs no compiler, whatever the policy
     * says — and it says so as NO_INPUTS rather than as DISABLED, because the
     * remedy is to generate a build description rather than to change a
     * setting. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    T_OK(fx_write(fx_repo(&e.fx), "a.c", "int f(void){return 0;}\n", &err), &err);
    run_file_pass(&e, &err);
    discover(&e, &err);

    atlas_sem_plan p;
    plan_of(&e, &p, &err);
    T_CHECK_MSG(!p.should_build, "a repository with no build input scheduled a compiler run");
    T_CHECK(p.activity == ATLAS_SEM_ACT_NO_INPUTS);
    T_CHECK(p.hold_reason != NULL && strcmp(p.hold_reason, ATLAS_SEM_HOLD_NO_INPUTS) == 0);
    T_CHECK(atlas_sem_hold_reason_is_known(p.hold_reason));
    /* And the reason that comes back is Atlas' own literal, not a copy. */
    T_CHECK(atlas_sem_hold_intern(ATLAS_SEM_HOLD_NO_INPUTS) == p.hold_reason);
    /* The walk ran and covered the whole bounded universe, so discovery is
     * COMPLETE even though it found nothing. "I looked and there is nothing
     * here" is a different statement from "I have not looked". */
    T_CHECK(p.discovery == ATLAS_SEM_DISC_COMPLETE);
    T_EQ_INT(p.inputs_accepted, 0);

    /* A root-owned policy that says no holds the repository even once it has an
     * input, and says so in its own words. */
    const char *srcs[] = {"a.c"};
    write_compdb(&e, srcs, 1, &err);
    discover(&e, &err);
    plan_of_with_default(&e, false, &p, &err);
    T_CHECK_MSG(!p.should_build, "a policy-disabled repository scheduled a compiler run");
    T_CHECK(p.activity == ATLAS_SEM_ACT_DISABLED);
    T_CHECK(p.hold_reason != NULL &&
            strcmp(p.hold_reason, ATLAS_SEM_HOLD_POLICY_DEFAULT_OFF) == 0);
    T_CHECK(p.auto_intent == ATLAS_SEM_INTENT_UNSET);

    env_close(&e);
}

static void test_an_operator_refusal_outlives_the_policy(void) {
    /* §20 and §25. The default being permissive is only acceptable because an
     * operator's explicit refusal is never lifted behind their back — so the
     * refusal is checked with the machine-wide default ON, which is the only
     * configuration in which it could be overruled. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    T_OK(fx_write(fx_repo(&e.fx), "a.c", "int f(void){return 0;}\n", &err), &err);
    const char *srcs[] = {"a.c"};
    write_compdb(&e, srcs, 1, &err);
    run_file_pass(&e, &err);
    discover(&e, &err);

    atlas_sem_config cfg;
    atlas_sem_config_init(&cfg);
    cfg.repo_id = e.repo_id;
    cfg.auto_intent = ATLAS_SEM_INTENT_DISABLED;
    cfg.auto_intent_by = ATLAS_SEM_INTENT_BY_OPERATOR;
    T_OK(atlas_db_sem_config_set(e.db, &cfg, &err), &err);
    atlas_sem_config_free(&cfg);

    atlas_sem_plan p;
    plan_of_with_default(&e, true, &p, &err);
    T_CHECK_MSG(!p.should_build, "an explicitly disabled repository scheduled a compiler run");
    T_CHECK(p.activity == ATLAS_SEM_ACT_EXPLICITLY_DISABLED);
    T_CHECK(p.hold_reason != NULL &&
            strcmp(p.hold_reason, ATLAS_SEM_HOLD_EXPLICIT_DISABLE) == 0);
    /* And the surface can say *whose* decision it was, which is the whole
     * reason the intent and its provenance are separate fields. */
    T_CHECK(p.auto_intent == ATLAS_SEM_INTENT_DISABLED);
    T_CHECK(p.auto_intent_by == ATLAS_SEM_INTENT_BY_OPERATOR);

    /* Re-enabling converges without anybody running a rebuild by hand. */
    atlas_sem_config on;
    atlas_sem_config_init(&on);
    on.repo_id = e.repo_id;
    on.auto_intent = ATLAS_SEM_INTENT_ENABLED;
    on.auto_intent_by = ATLAS_SEM_INTENT_BY_OPERATOR;
    T_OK(atlas_db_sem_config_set(e.db, &on, &err), &err);
    atlas_sem_config_free(&on);
    plan_of_with_default(&e, false, &p, &err);
    T_CHECK_MSG(p.should_build,
                "an explicitly enabled repository did not schedule a build under a policy "
                "default of off");
    T_CHECK(p.auto_intent == ATLAS_SEM_INTENT_ENABLED);

    env_close(&e);
}

static void test_a_migrated_default_is_not_an_operator_refusal(void) {
    /* §4 and §26. A stored `auto_rebuild = 0` could not distinguish an
     * operator's `--no-auto` from nobody ever having said anything, so migration
     * 19 records the second as UNSET/MIGRATION rather than inventing the first.
     * What that must produce is a repository the default decides — never one
     * reported as explicitly disabled. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    T_OK(fx_write(fx_repo(&e.fx), "a.c", "int f(void){return 0;}\n", &err), &err);
    const char *srcs[] = {"a.c"};
    write_compdb(&e, srcs, 1, &err);
    run_file_pass(&e, &err);
    discover(&e, &err);

    /* Exactly what migration 19 leaves behind for a pre-A9.2.4 row. */
    atlas_sem_config cfg;
    atlas_sem_config_init(&cfg);
    cfg.repo_id = e.repo_id;
    cfg.auto_intent = ATLAS_SEM_INTENT_UNSET;
    cfg.auto_intent_by = ATLAS_SEM_INTENT_BY_MIGRATION;
    T_OK(atlas_db_sem_config_set(e.db, &cfg, &err), &err);
    atlas_sem_config_free(&cfg);

    atlas_sem_plan p;
    plan_of_with_default(&e, true, &p, &err);
    T_CHECK_MSG(p.should_build, "a migrated default was treated as an operator refusal");
    T_CHECK(p.activity != ATLAS_SEM_ACT_EXPLICITLY_DISABLED);
    T_CHECK(p.auto_intent == ATLAS_SEM_INTENT_UNSET);
    T_CHECK(p.auto_intent_by == ATLAS_SEM_INTENT_BY_MIGRATION);

    env_close(&e);
}

/* --- A9.2.5 / GAP-8: the per-unit retry, end to end -------------------------
 *
 * `tu_failed > 0` makes a generation's coverage incomplete for ever, because the
 * retry governor compares identities and identical bytes never retry. So a parse
 * child that was OOM-killed cost a repository the ability to state an absence
 * until somebody happened to edit a file.
 *
 * The retry that fixes it is scheduler behaviour, so it is tested by running a
 * real pass against a parse child that really fails — `tests/tools/atlas_flaky_parse.c`,
 * supplied through `atlas_sem_index_opts.atlas_exe`, which has been the child's
 * executable path since A8-CI. No production code knows a test is running. */

/* Copies the flaky child into the fixture so its counters are per-test, and
 * returns its path in `out`. `failures` leading invocations will fail. */
static void install_flaky_parse(env *e, long failures, atlas_buf *out, atlas_err *err) {
    /* Copied rather than used in place, so two tests running at once cannot
     * share a counter and so the state files land inside the fixture that owns
     * them. */
    FILE *in = fopen(ATLAS_FLAKY_PARSE_BIN, "rb");
    T_REQUIRE_MSG(in != NULL, "cannot open the flaky parse child");
    atlas_buf_reset(out);
    T_OK(atlas_buf_appendf(out, err, "%s/flaky-parse", fx_data_dir(&e->fx)), err);
    FILE *f = fopen(atlas_buf_cstr(out), "wb");
    T_REQUIRE_MSG(f != NULL, "cannot write the flaky parse child");
    unsigned char chunk[8192];
    size_t got = 0;
    while ((got = fread(chunk, 1, sizeof chunk, in)) > 0) {
        T_REQUIRE_MSG(fwrite(chunk, 1, got, f) == got, "short write copying the parse child");
    }
    (void)fclose(in);
    (void)fclose(f);
    T_REQUIRE_MSG(chmod(atlas_buf_cstr(out), 0700) == 0, "cannot make the child executable");

    char path[4096];
    (void)snprintf(path, sizeof path, "%s.failures", atlas_buf_cstr(out));
    f = fopen(path, "w");
    T_REQUIRE_MSG(f != NULL, "cannot set the failure count");
    (void)fprintf(f, "%ld\n", failures);
    (void)fclose(f);
}

/* How many times the child was actually invoked. The bound is the whole
 * guarantee, so it is asserted rather than assumed. */
static long flaky_calls(const atlas_buf *exe) {
    char path[4096];
    (void)snprintf(path, sizeof path, "%s.calls", atlas_buf_cstr(exe));
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return 0;
    }
    long n = 0;
    if (fscanf(f, "%ld", &n) != 1) {
        n = 0;
    }
    (void)fclose(f);
    return n;
}

/* `index_once` with the parse child replaced. */
static void index_with_exe(env *e, const char *exe, atlas_sem_index_summary *sum,
                           atlas_status *st_out, atlas_err *err) {
    atlas_sem_index_opts o;
    atlas_sem_index_opts_init(&o);
    o.compdbs = "compile_commands.json";
    o.compdbs_len = strlen("compile_commands.json") + 1u;
    o.atlas_exe = exe;
    o.root = atlas_git_root(e->g);
    o.commit_id = "";
    o.repo_identity_hash = "";
    int fd = open(atlas_git_root(e->g), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    T_REQUIRE_MSG(fd >= 0, "cannot open the fixture repository root");
    o.root_fd = fd;
    atlas_sem_index_summary_init(sum);
    *st_out = atlas_sem_index_run(e->db, e->repo_id, &o, sum, err);
    (void)close(fd);
}

static void test_a_transient_child_failure_is_retried_once_and_recovers(void) {
    /* (1) the first child call fails transiently, (2) the second succeeds for the
     * same unit, (3) the generation publishes complete, (4) the unit does not run
     * more than twice. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    T_OK(fx_write(fx_repo(&e.fx), "a.c", "int f(void){return 0;}\n", &err), &err);
    const char *srcs[] = {"a.c"};
    write_compdb(&e, srcs, 1, &err);
    run_file_pass(&e, &err);

    atlas_buf exe = ATLAS_BUF_INIT;
    install_flaky_parse(&e, 1, &exe, &err);

    atlas_sem_index_summary sum;
    atlas_status st = ATLAS_OK;
    index_with_exe(&e, atlas_buf_cstr(&exe), &sum, &st, &err);
    T_OK(st, &err);

    T_CHECK_MSG(flaky_calls(&exe) == 2,
                "the unit must be attempted exactly twice, was attempted %ld times",
                flaky_calls(&exe));
    T_CHECK_MSG(sum.units_retried == 1, "expected one recorded retry, got %lld",
                (long long)sum.units_retried);
    T_CHECK_MSG(sum.published, "a recovered unit must still publish a generation");
    T_CHECK_MSG(sum.units_complete == 1, "the retried unit must end COMPLETE, got %lld",
                (long long)sum.units_complete);
    T_CHECK_MSG(sum.units_failed == 0, "no unit should be left failed, got %lld",
                (long long)sum.units_failed);
    T_CHECK_MSG(sum.symbols > 0, "the successful attempt must have produced real facts");

    /* And the published generation is genuinely complete, not merely reported so. */
    atlas_sem_generation g;
    bool found = false;
    T_OK(atlas_db_sem_current(e.db, e.repo_id, &g, &found, &err), &err);
    T_REQUIRE_MSG(found, "no generation was published");
    T_CHECK(g.status == ATLAS_SEM_GEN_COMPLETE);
    T_CHECK_MSG(g.tu_failed == 0 && g.tu_partial == 0 && g.tu_unsupported == 0,
                "the generation must hold no undescribed unit");

    atlas_buf_free(&exe);
    env_close(&e);
}

static void test_a_unit_that_fails_twice_stays_failed_and_is_not_retried_again(void) {
    /* (5) both attempts fail -> the generation is published but incomplete, and
     * (4 again) the bound holds: exactly two attempts, never a third. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    T_OK(fx_write(fx_repo(&e.fx), "a.c", "int f(void){return 0;}\n", &err), &err);
    const char *srcs[] = {"a.c"};
    write_compdb(&e, srcs, 1, &err);
    run_file_pass(&e, &err);

    atlas_buf exe = ATLAS_BUF_INIT;
    install_flaky_parse(&e, 99, &exe, &err); /* always fails */

    atlas_sem_index_summary sum;
    atlas_status st = ATLAS_OK;
    index_with_exe(&e, atlas_buf_cstr(&exe), &sum, &st, &err);
    T_OK(st, &err);

    T_CHECK_MSG(flaky_calls(&exe) == 2,
                "the bound is one retry: expected exactly two attempts, got %ld",
                flaky_calls(&exe));
    T_CHECK_MSG(sum.units_retried == 1, "the spent retry must be reported, got %lld",
                (long long)sum.units_retried);
    T_CHECK_MSG(sum.units_failed == 1, "a unit that failed twice must be recorded failed");
    T_CHECK_MSG(sum.units_complete == 0, "nothing completed");

    /* A generation is still published — one lost unit must not lose the pass —
     * and it says it is incomplete rather than reading as complete. */
    atlas_sem_generation g;
    bool found = false;
    T_OK(atlas_db_sem_current(e.db, e.repo_id, &g, &found, &err), &err);
    if (found) {
        T_CHECK_MSG(g.tu_failed == 1,
                    "the published generation must carry the failed unit, got %lld",
                    (long long)g.tu_failed);
        T_CHECK_MSG(atlas_sem_coverage_gap(g.scope_discovery, g.discovery,
                                           g.tu_partial == 0 && g.tu_failed == 0 &&
                                               g.tu_unsupported == 0,
                                           g.scope_uncovered) != NULL,
                    "a generation holding a failed unit must not read as complete coverage");
    }

    /* (6) The scheduler must not spin on it. The source identity has not moved,
     * so asking twice gives the same answer and neither is an unbounded build. */
    atlas_sem_plan p1;
    atlas_sem_plan p2;
    plan_of(&e, &p1, &err);
    plan_of(&e, &p2, &err);
    T_CHECK_MSG(p1.should_build == p2.should_build,
                "the plan must be stable on an unmoved source identity");
    T_CHECK_MSG(p1.activity == p2.activity, "and so must the activity");

    atlas_buf_free(&exe);
    env_close(&e);
}

static void test_a_deterministic_compiler_error_is_never_retried(void) {
    /* (7) The real parse child, over a source the compiler cannot accept. The
     * unit is not complete, and **no retry is spent**: retrying identical bytes
     * through the same compiler reaches the same answer, which is the storm the
     * transient classification exists to prevent. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    T_OK(fx_write(fx_repo(&e.fx), "a.c",
                  "#include \"there_is_no_such_header_anywhere.h\"\n"
                  "int f(void){ return undefined_thing_entirely(; }\n",
                  &err),
         &err);
    const char *srcs[] = {"a.c"};
    write_compdb(&e, srcs, 1, &err);
    run_file_pass(&e, &err);

    atlas_sem_index_summary sum;
    index_once(&e, NULL, &sum, &err);

    T_CHECK_MSG(sum.units_retried == 0,
                "a compiler error must not spend a retry, %lld were spent",
                (long long)sum.units_retried);
    T_CHECK_MSG(sum.units_complete == 0, "a unit the compiler rejected is not complete");
    T_CHECK_MSG(sum.units_parsed == 1, "the unit was attempted exactly once");

    atlas_buf_free(&e.exe);
    atlas_buf_init(&e.exe);
    T_OK(atlas_buf_set_str(&e.exe, ATLAS_BIN, &err), &err);
    env_close(&e);
}

/* The trust block for the fixture's published generation, gathered the way every
 * production surface gathers it. The policy default is supplied rather than read
 * from `/etc`, for `plan_of`'s reason: a test must not depend on how the machine
 * running it happens to be configured. */
static void trust_after(env *e, atlas_sem_trust *out, atlas_err *err) {
    atlas_repo_info info;
    repo_of(e, &info, err);
    atlas_sem_generation g;
    bool found = false;
    T_OK(atlas_db_sem_current(e->db, info.id, &g, &found, err), err);
    atlas_sem_trust_now_with_default(e->db, &info, &g, found, false, ATLAS_SEM_AUTO_DEFAULT, out);
    atlas_repo_info_free(&info);
}

typedef struct sym_count_sink {
    size_t n;
} sym_count_sink;

static atlas_status count_symbol(const atlas_sem_symbol_row *row, void *ud, atlas_err *err) {
    (void)row;
    (void)err;
    ((sym_count_sink *)ud)->n++;
    return ATLAS_OK;
}

static void test_a_failed_unit_is_never_carried_forward_as_complete(void) {
    /* **The defect this test exists for could turn a failed parse into a proven
     * absence.**
     *
     * The carry-forward decision used to ask only whether a unit's input digest
     * matched, with no status filter, and the carry branch writes
     * `status = COMPLETE`. So a unit that failed in pass N — a parse child killed
     * twice — was rewritten as COMPLETE in pass N+1, with the zero symbols and
     * zero edges it never produced. One unrelated edit anywhere in the tree moves
     * the source identity and triggers that pass.
     *
     * Before A9.2.5 that was a wrong counter. A9.2.5 makes `units_complete` a
     * gate on `ATLAS_SEM_VERDICT_ABSENT`, so it became a path to a *proven
     * absence* over a file Atlas has never successfully parsed — with every field
     * in the trust block saying the coverage was whole. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    T_OK(fx_write(fx_repo(&e.fx), "a.c", "int f(void){return 0;}\n", &err), &err);
    const char *srcs[] = {"a.c"};
    write_compdb(&e, srcs, 1, &err);
    run_file_pass(&e, &err);
    /* A walk, so the generation records COMPLETE discovery and the *units*
     * dimension is the one under test. Without it discovery is UNKNOWN and
     * outranks everything, which would make this test pass for the wrong
     * reason. */
    discover(&e, &err);

    /* Pass 1: the child always fails, so the unit ends FAILED. */
    atlas_buf exe = ATLAS_BUF_INIT;
    install_flaky_parse(&e, 99, &exe, &err);
    atlas_sem_index_summary first;
    atlas_status st = ATLAS_OK;
    index_with_exe(&e, atlas_buf_cstr(&exe), &first, &st, &err);
    T_OK(st, &err);
    T_REQUIRE_MSG(first.units_failed == 1, "the fixture must leave one failed unit");
    T_CHECK(first.symbols == 0);

    /* **The epistemic state after pass 1**, asserted before anything about
     * counters: a generation holding a unit Atlas could not parse must not let an
     * empty answer read as an absence. */
    atlas_sem_trust t;
    trust_after(&e, &t, &err);
    atlas_sem_trust_settle(&t, 0, false);
    T_CHECK_MSG(t.verdict == ATLAS_SEM_VERDICT_UNKNOWN,
                "a generation with a failed unit must answer UNKNOWN, got %s",
                atlas_sem_verdict_name(t.verdict));
    T_CHECK_MSG(t.unknown_reason != NULL &&
                    strcmp(t.unknown_reason, ATLAS_SEM_UNK_UNITS) == 0,
                "and must say which dimension failed, got %s",
                t.unknown_reason == NULL ? "(null)" : t.unknown_reason);
    T_CHECK(!t.units_complete);

    /* Pass 2: the real parse child, identical bytes and identical configuration,
     * so the digest a naive carry rule would match is exactly the same. The unit
     * must be **re-parsed**, not carried. */
    atlas_sem_index_summary second;
    index_once(&e, NULL, &second, &err);

    T_CHECK_MSG(second.units_reused == 0,
                "a failed unit must never be reused: %lld unit(s) were carried forward",
                (long long)second.units_reused);
    T_CHECK_MSG(second.units_parsed == 1, "the failed unit must be parsed again, parsed %lld",
                (long long)second.units_parsed);
    T_CHECK_MSG(second.units_complete == 1, "and it succeeds this time");
    T_CHECK_MSG(second.symbols > 0,
                "a unit carried forward as COMPLETE would hold zero facts; this one must hold "
                "the facts a real parse produced");

    atlas_sem_generation g2;
    bool found2 = false;
    T_OK(atlas_db_sem_current(e.db, e.repo_id, &g2, &found2, &err), &err);
    T_REQUIRE_MSG(found2, "no generation was published by the second pass");
    T_CHECK_MSG(g2.tu_failed == 0, "the second generation holds no failed unit");
    T_CHECK_MSG(g2.symbol_count > 0, "and it holds real symbols");

    /* **And now the epistemic half, which is the reason any of this matters.**
     *
     * Counters are evidence about the bug; the verdict is the consequence. After
     * the second pass an empty answer may finally settle ABSENT — and it must do
     * so because the file was *read*, not because a failure was relabelled. */
    trust_after(&e, &t, &err);
    atlas_sem_trust_settle(&t, 0, false);
    T_CHECK_MSG(t.verdict == ATLAS_SEM_VERDICT_ABSENT,
                "after a real parse an empty answer may settle ABSENT, got %s (%s)",
                atlas_sem_verdict_name(t.verdict),
                t.unknown_reason == NULL ? "-" : t.unknown_reason);
    T_CHECK(t.units_complete && t.coverage_complete);

    /* The symbol the failed pass never saw is genuinely in the index now, and a
     * read that finds it settles PRESENT. */
    atlas_sem_symbols_report syms;
    atlas_sem_symbols_report_init(&syms);
    sym_count_sink sink = {0};
    int64_t total = 0;
    bool trunc = false;
    T_OK(atlas_db_sem_symbols_by_name(e.db, g2.id, "f", NULL, NULL, 64, count_symbol, &sink,
                                      &total, &trunc, &err),
         &err);
    atlas_sem_symbols_report_free(&syms);
    T_CHECK_MSG(sink.n > 0, "the re-parsed unit must actually contain its symbol");
    trust_after(&e, &t, &err);
    atlas_sem_trust_settle(&t, (int64_t)sink.n, false);
    T_CHECK_MSG(t.verdict == ATLAS_SEM_VERDICT_PRESENT,
                "a read that found the symbol settles PRESENT, got %s",
                atlas_sem_verdict_name(t.verdict));

    atlas_buf_free(&exe);
    env_close(&e);
}

/* A9.2.5 / GAP-8, the whole-pass half. */
static void test_an_interrupted_pass_gets_exactly_one_more_attempt(void) {
    /* The governor could not tell a pass that failed *because of the machine* —
     * out of memory, a database error — from one that failed because of the
     * inputs. Both pinned `fail_identity`, so a transient interruption held a
     * repository on HOLD_FAILED_UNCHANGED until somebody happened to edit a
     * file. The exception is bounded by `fail_count`, which is durable, so a
     * second interruption holds and a daemon restart reaches the same answer.
     * There is no timer: nothing can wake up and try a third time. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    T_OK(fx_write(fx_repo(&e.fx), "a.c", "int f(void){return 0;}\n", &err), &err);
    const char *srcs[] = {"a.c"};
    write_compdb(&e, srcs, 1, &err);
    run_file_pass(&e, &err);

    atlas_sem_config cfg;
    atlas_sem_config_init(&cfg);
    cfg.repo_id = e.repo_id;
    cfg.auto_intent = ATLAS_SEM_INTENT_ENABLED;
    cfg.auto_intent_by = ATLAS_SEM_INTENT_BY_OPERATOR;
    const char *dbs[] = {"compile_commands.json"};
    T_OK(atlas_sem_config_pack(dbs, 1, &cfg.compdbs, &err), &err);
    T_OK(atlas_db_sem_config_set(e.db, &cfg, &err), &err);
    atlas_sem_config_free(&cfg);
    discover(&e, &err);

    atlas_sem_plan p;
    plan_of(&e, &p, &err);
    T_CHECK(p.should_build);

    /* One interruption. The source has not moved, and the attempt is still
     * allowed — exactly once. */
    T_OK(atlas_db_sem_config_record_attempt(e.db, e.repo_id, p.source_identity, false,
                                            ATLAS_SEM_WHY_PASS_INTERRUPTED, &err),
         &err);
    plan_of(&e, &p, &err);
    T_EQ_INT(p.fail_count, 1);
    T_CHECK_MSG(p.should_build,
                "one interrupted pass must be worth one more attempt with the source unmoved");

    /* A second interruption at the same identity: the bound is reached and the
     * repository holds, visibly, with the reason recorded. */
    T_OK(atlas_db_sem_config_record_attempt(e.db, e.repo_id, p.source_identity, false,
                                            ATLAS_SEM_WHY_PASS_INTERRUPTED, &err),
         &err);
    plan_of(&e, &p, &err);
    T_EQ_INT(p.fail_count, 2);
    T_CHECK_MSG(!p.should_build, "the retry bound is one; a second interruption must hold");
    T_CHECK(p.activity == ATLAS_SEM_ACT_FAILED);
    T_CHECK(p.hold_reason != NULL &&
            strcmp(p.hold_reason, ATLAS_SEM_HOLD_FAILED_UNCHANGED) == 0);

    /* And the exception is *only* for an interruption. A deterministic failure
     * holds on the first one, unchanged from A9.2.3. */
    T_OK(atlas_db_sem_config_record_attempt(e.db, e.repo_id, p.source_identity, true, "", &err),
         &err);
    T_OK(atlas_db_sem_config_record_attempt(e.db, e.repo_id, p.source_identity, false,
                                            ATLAS_SEM_WHY_BUILD_DESCRIPTION, &err),
         &err);
    plan_of(&e, &p, &err);
    T_EQ_INT(p.fail_count, 1);
    T_CHECK_MSG(!p.should_build,
                "a build-description failure must hold on the first attempt, as before");

    env_close(&e);
}

static void test_the_retry_governor_holds_until_the_source_moves(void) {
    /* Conservative on purpose. Retrying after an interval would spin on a
     * repository that does not compile — every attempt running a compiler over
     * the whole tree, for ever, achieving nothing. What makes another attempt
     * worth making is that the inputs changed. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    T_OK(fx_write(fx_repo(&e.fx), "a.c", "int f(void){return 0;}\n", &err), &err);
    const char *srcs[] = {"a.c"};
    write_compdb(&e, srcs, 1, &err);
    run_file_pass(&e, &err);

    atlas_sem_config cfg;
    atlas_sem_config_init(&cfg);
    cfg.repo_id = e.repo_id;
    cfg.auto_intent = ATLAS_SEM_INTENT_ENABLED;
    cfg.auto_intent_by = ATLAS_SEM_INTENT_BY_OPERATOR;
    const char *dbs[] = {"compile_commands.json"};
    T_OK(atlas_sem_config_pack(dbs, 1, &cfg.compdbs, &err), &err);
    T_OK(atlas_db_sem_config_set(e.db, &cfg, &err), &err);
    atlas_sem_config_free(&cfg);
    /* A9.2.4: nothing is an accepted build input until a walk has accepted it. */
    discover(&e, &err);

    atlas_sem_plan p;
    plan_of(&e, &p, &err);
    T_CHECK_MSG(p.should_build, "an enabled repository with no index did not schedule one");
    T_CHECK(p.activity == ATLAS_SEM_ACT_UNAVAILABLE);

    /* Record a failure at the identity as it is now. */
    T_OK(atlas_db_sem_config_record_attempt(e.db, e.repo_id, p.source_identity, false,
                                            ATLAS_SEM_WHY_CHILD_FAILED, &err),
         &err);
    plan_of(&e, &p, &err);
    T_CHECK_MSG(!p.should_build, "a deterministic failure was retried with nothing changed");
    T_CHECK(p.activity == ATLAS_SEM_ACT_FAILED);
    T_EQ_INT(p.fail_count, 1);
    T_CHECK(p.hold_reason != NULL &&
            strcmp(p.hold_reason, ATLAS_SEM_HOLD_FAILED_UNCHANGED) == 0);

    /* Move the source. The inputs have changed, so another attempt is worth
     * making — and this is the only thing that lifts the hold. */
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "int f(void){return 1;}\n", &err), &err);
    run_file_pass(&e, &err);
    plan_of(&e, &p, &err);
    T_CHECK_MSG(p.should_build, "a changed source did not lift the retry hold");

    /* A success clears the record entirely, so the next failure starts over. */
    T_OK(atlas_db_sem_config_record_attempt(e.db, e.repo_id, p.source_identity, true, "", &err),
         &err);
    plan_of(&e, &p, &err);
    T_EQ_INT(p.fail_count, 0);

    env_close(&e);
}

static void test_removing_a_repository_forgets_its_build_description(void) {
    /* `repositories.id` is a reused rowid. A description left behind would hand
     * the next repository to take this id somebody else's opt-in — the A4
     * defect, applied to the row that decides whether a compiler runs. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_sem_config cfg;
    atlas_sem_config_init(&cfg);
    cfg.repo_id = e.repo_id;
    cfg.auto_intent = ATLAS_SEM_INTENT_ENABLED;
    cfg.auto_intent_by = ATLAS_SEM_INTENT_BY_OPERATOR;
    const char *dbs[] = {"compile_commands.json"};
    T_OK(atlas_sem_config_pack(dbs, 1, &cfg.compdbs, &err), &err);
    T_OK(atlas_db_sem_config_set(e.db, &cfg, &err), &err);
    atlas_sem_config_free(&cfg);

    bool removed = false;
    T_OK(atlas_db_repo_remove(e.db, "fx", &removed, &err), &err);
    T_CHECK(removed);

    atlas_sem_config back;
    atlas_sem_config_init(&back);
    T_OK(atlas_db_sem_config_get(e.db, e.repo_id, &back, &err), &err);
    T_CHECK_MSG(!back.present,
                "a removed repository's build description survived to authorise a compiler run "
                "for whatever takes its rowid next");
    atlas_sem_config_free(&back);

    env_close(&e);
}

static const atlas_test TESTS[] = {
    {"every zero is the safe reading", test_every_zero_is_the_safe_reading},
    {"a nested test root is matched when it is declared",
     test_a_nested_test_root_is_matched_when_it_is_declared},
    {"an undeclared nested test directory is not guessed",
     test_an_undeclared_nested_test_directory_is_not_guessed},
    {"declaring one test root does not prove they are all declared",
     test_declaring_one_test_root_does_not_prove_they_are_all_declared},
    {"a declared test root matches on a path-component boundary",
     test_a_test_root_matches_on_a_component_boundary},
    {"a build description round-trips and refuses a newline",
     test_a_build_description_round_trips_and_refuses_a_newline},
    {"an empty stored identity never makes a generation stale",
     test_an_empty_stored_identity_never_makes_a_generation_stale},
    {"an uncommitted edit makes the semantic index stale",
     test_an_uncommitted_edit_makes_the_index_stale},
    {"a pass that finds nothing to do still records that it looked",
     test_a_no_change_pass_records_that_it_looked},
    {"coverage is measured against the tree, not the build description",
     test_coverage_is_measured_against_the_tree_not_the_build_description},
    {"without declared test roots the unit scope is unknown",
     test_without_declared_test_roots_the_unit_scope_is_unknown},
    {"no build input means no compiler runs",
     test_no_build_input_means_no_compiler_runs},
    {"an operator refusal outlives the policy", test_an_operator_refusal_outlives_the_policy},
    {"a migrated default is not an operator refusal",
     test_a_migrated_default_is_not_an_operator_refusal},
    {"a failed unit is never carried forward as complete",
     test_a_failed_unit_is_never_carried_forward_as_complete},
    {"a transient child failure is retried once and recovers",
     test_a_transient_child_failure_is_retried_once_and_recovers},
    {"a unit that fails twice stays failed and is not retried again",
     test_a_unit_that_fails_twice_stays_failed_and_is_not_retried_again},
    {"a deterministic compiler error is never retried",
     test_a_deterministic_compiler_error_is_never_retried},
    {"an interrupted pass gets exactly one more attempt",
     test_an_interrupted_pass_gets_exactly_one_more_attempt},
    {"the retry governor holds until the source moves",
     test_the_retry_governor_holds_until_the_source_moves},
    {"removing a repository forgets its build description",
     test_removing_a_repository_forgets_its_build_description},
};

ATLAS_TEST_MAIN("sem_freshness", TESTS)
