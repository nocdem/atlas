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
    T_CHECK(atlas_sem_freshness_of(&g, true, false, "", "", "abc", NULL, true, &reason) ==
            ATLAS_SEM_FRESH_CURRENT);
    T_CHECK(reason == NULL);

    /* And with an identity recorded, a different one is STALE with its own
     * reason — the working tree, not the commit. */
    (void)snprintf(g.source_identity, sizeof g.source_identity, "%s", "abc");
    T_CHECK(atlas_sem_freshness_of(&g, true, false, "", "", "def", NULL, true, &reason) ==
            ATLAS_SEM_FRESH_STALE);
    T_CHECK(reason != NULL && strcmp(reason, ATLAS_SEM_STALE_SOURCE) == 0);
    T_CHECK(atlas_sem_stale_reason_is_known(ATLAS_SEM_STALE_SOURCE));

    /* An empty *live* identity does not either: "Atlas could not look" is not
     * evidence that anything changed. */
    T_CHECK(atlas_sem_freshness_of(&g, true, false, "", "", "", NULL, true, &reason) ==
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
    {"the retry governor holds until the source moves",
     test_the_retry_governor_holds_until_the_source_moves},
    {"removing a repository forgets its build description",
     test_removing_a_repository_forgets_its_build_description},
};

ATLAS_TEST_MAIN("sem_freshness", TESTS)
