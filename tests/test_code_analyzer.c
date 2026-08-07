/* Atlas - the structural graph must not survive its own analyzer.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Every other staleness signal Atlas has is about the *inputs*: a content hash
 * moved, a generation is behind, a pass failed. There is a fourth way a graph
 * goes wrong and none of those can see it.
 *
 *   1. Atlas indexes a repository.
 *   2. Atlas is upgraded, and the upgrade corrects the lexer or the resolver.
 *   3. Not one byte of the repository or the compile database changes.
 *   4. Every generation still lines up, so the pass finds nothing to do.
 *
 * The stored graph is now wrong in exactly the way the upgrade fixed, and it
 * reports itself current. That is the worst shape a wrong answer can take,
 * because nothing about it looks wrong.
 *
 * `code_index_state.analyzer_id` closes it, referencing an interned
 * `code_analyzers` row that is a pair of Atlas-owned constants. The tests here
 * are the four claims that mechanism has to make good on: a mismatch is stale,
 * a sync fixes it, fixing it rebuilds rather than pretending, and the rebuild
 * touches nothing that is not derived.
 *
 * The upgrade is simulated by writing an older version into `code_analyzers`,
 * which is precisely what an older binary would have left behind — the row is
 * what an analyzer records about itself, so a row saying `v0` is
 * indistinguishable from one an Atlas that called itself v0 wrote.
 */
#include <stdio.h>
#include <string.h>

#include "atlas/ai.h"
#include "atlas/atlas.h"
#include "atlas/code.h"
#include "atlas/reconcile.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

typedef struct env {
    fixture fx;
    atlas_db *db;
    atlas_git *g;
    int64_t repo_id;
} env;

static void env_open(env *e, atlas_err *err) {
    T_OK(fx_open(&e->fx, err), err);
    T_OK(fx_init_repo(&e->fx, fx_repo(&e->fx), NULL, err), err);
}

static void env_index(env *e, atlas_err *err) {
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
    T_OK(atlas_db_repo_add(e->db, "fixture", &id, &e->repo_id, err), err);
}

static void env_close(env *e) {
    atlas_git_close(e->g);
    atlas_db_close(e->db);
    fx_close(&e->fx);
}

static void run_pass(env *e, atlas_reconcile_summary *sum, atlas_err *err) {
    atlas_reconcile_opts opts;
    atlas_reconcile_opts_init(&opts);
    atlas_reconcile_summary_init(sum);
    T_OK(atlas_reconcile_run(e->db, e->g, e->repo_id, &opts, sum, err), err);
    T_CHECK(sum->published);
}

static int64_t count_rows(env *e, const char *sql, atlas_err *err) {
    int64_t n = 0;
    T_OK(atlas_db_query_int64(e->db, sql, &n, err), err);
    return n;
}

/* Reads the structural state and whether Atlas calls the graph current. */
static bool code_current(env *e, const char **reason_out, atlas_code_index_state *out,
                         atlas_err *err) {
    atlas_index_state fs;
    atlas_index_state_init(&fs);
    T_OK(atlas_db_index_state_get(e->db, e->repo_id, &fs, err), err);
    atlas_code_index_state_init(out);
    T_OK(atlas_db_code_state_get(e->db, e->repo_id, out, err), err);
    /* The file index is current in every case here — nothing touches the
     * repository between passes — so any staleness reported below is the
     * structural half's own. */
    bool current = atlas_code_index_current(&fs, out, true, reason_out);
    atlas_index_state_free(&fs);
    return current;
}

/* Writes a source tree with something for every stage to do: a resolvable
 * include, a call that resolves, a call that does not, and a compile database
 * so the unit edges exist too. */
static void seed(env *e, atlas_err *err) {
    T_OK(fx_write(fx_repo(&e->fx), "api.h", "int target(void);\n", err), err);
    T_OK(fx_write(fx_repo(&e->fx), "def.c", "int target(void){return 1;}\n", err), err);
    T_OK(fx_write(fx_repo(&e->fx), "use.c",
                  "#include \"api.h\"\nint use(void){return target() + elsewhere();}\n", err),
         err);
    atlas_buf json = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&json, err,
                           "[{\"directory\":\"%s\",\"file\":\"use.c\","
                           "\"arguments\":[\"cc\",\"-I.\",\"-c\",\"use.c\"]}]\n",
                           fx_repo(&e->fx)),
         err);
    T_OK(fx_write(fx_repo(&e->fx), "compile_commands.json", atlas_buf_cstr(&json), err), err);
    atlas_buf_free(&json);
    T_OK(fx_add_all(&e->fx, fx_repo(&e->fx), err), err);
    T_OK(fx_commit(&e->fx, fx_repo(&e->fx), "seed", err), err);
}

/* Rewrites the recorded analyzer version to something no binary produces, which
 * is what an older Atlas would have left behind. */
static void pretend_older_analyzer(env *e, atlas_err *err) {
    T_OK(atlas_db_exec_sql(e->db,
                           "INSERT INTO code_analyzers(name, version, first_seen_at)"
                           " VALUES('atlas-c-lexical', 0, '2026-01-01T00:00:00Z')"
                           " ON CONFLICT(name, version) DO NOTHING;"
                           "UPDATE code_index_state SET analyzer_id ="
                           " (SELECT id FROM code_analyzers WHERE name='atlas-c-lexical'"
                           "   AND version=0);",
                           err),
         err);
}

/* --- the tests ---------------------------------------------------------------- */

static void test_a_fresh_graph_records_this_analyzer(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    seed(&e, &err);
    env_index(&e, &err);
    atlas_reconcile_summary sum;
    run_pass(&e, &sum, &err);
    T_CHECK(!sum.code.analyzer_changed);
    atlas_reconcile_summary_free(&sum);

    const char *reason = NULL;
    atlas_code_index_state cs;
    T_CHECK(code_current(&e, &reason, &cs, &err));
    T_EQ_STR(atlas_buf_cstr(&cs.analyzer_name), ATLAS_CODE_ANALYZER_ID);
    T_EQ_INT(cs.analyzer_version, (int64_t)ATLAS_CODE_ANALYZER_VERSION);
    T_CHECK(atlas_code_analyzer_matches(&cs));
    atlas_code_index_state_free(&cs);

    /* Interned once, not once per pass. */
    for (int i = 0; i < 3; i++) {
        atlas_reconcile_summary again;
        run_pass(&e, &again, &err);
        T_CHECK(!again.code.analyzer_changed);
        atlas_reconcile_summary_free(&again);
    }
    T_EQ_INT(count_rows(&e, "SELECT COUNT(*) FROM code_analyzers;", &err), 1);
    env_close(&e);
}

/* The whole point, in one test: an upgraded binary over an untouched
 * repository. */
static void test_an_older_analyzer_makes_the_graph_stale(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    seed(&e, &err);
    env_index(&e, &err);
    atlas_reconcile_summary sum;
    run_pass(&e, &sum, &err);
    atlas_reconcile_summary_free(&sum);

    int64_t symbols = count_rows(&e, "SELECT COUNT(*) FROM code_symbols;", &err);
    int64_t relations = count_rows(&e, "SELECT COUNT(*) FROM code_relations;", &err);
    T_CHECK(symbols > 0);
    T_CHECK(relations > 0);

    const char *reason = NULL;
    atlas_code_index_state before;
    T_CHECK(code_current(&e, &reason, &before, &err));
    atlas_code_index_state_free(&before);

    /* The upgrade. Nothing else changes: no file is written, no commit is made,
     * the compile database is byte-identical, and every generation is where the
     * last pass left it. */
    pretend_older_analyzer(&e, &err);

    atlas_code_index_state stale;
    T_CHECK_MSG(!code_current(&e, &reason, &stale, &err),
                "a graph from a different analyzer version was reported current");
    T_REQUIRE(reason != NULL);
    T_CHECK_MSG(atlas_code_not_current_reason_is_known(reason),
                "the staleness reason is not one of Atlas' own strings");
    T_CHECK(strstr(reason, "analyzer") != NULL);
    T_CHECK(!atlas_code_analyzer_matches(&stale));
    atlas_code_index_state_free(&stale);

    /* And the next ordinary sync repairs it — not a rebuild anybody asked for,
     * and not a rebuild flag on the command line. The pass notices by itself. */
    atlas_reconcile_summary after;
    run_pass(&e, &after, &err);
    T_CHECK_MSG(after.code.analyzer_changed, "the pass did not report the analyzer change");
    T_CHECK_MSG(after.code.files_parsed > 0,
                "the pass claimed an analyzer upgrade without reparsing anything");
    atlas_reconcile_summary_free(&after);

    atlas_code_index_state now;
    T_CHECK_MSG(code_current(&e, &reason, &now, &err),
                "the graph is still stale after the repairing pass");
    T_EQ_INT(now.analyzer_version, (int64_t)ATLAS_CODE_ANALYZER_VERSION);
    atlas_code_index_state_free(&now);

    /* Rebuilt, not merely relabelled: the same facts are there, and there are
     * not two copies of them. */
    T_EQ_INT(count_rows(&e, "SELECT COUNT(*) FROM code_symbols;", &err), symbols);
    T_EQ_INT(count_rows(&e, "SELECT COUNT(*) FROM code_relations;", &err), relations);
    T_EQ_INT(count_rows(&e,
                        "SELECT COUNT(*) FROM code_relations WHERE resolution='UNIQUE_LEXICAL'"
                        " AND dst_kind='symbol';",
                        &err) > 0,
             1);
    T_EQ_INT(count_rows(&e,
                        "SELECT COUNT(*) FROM code_relations"
                        " WHERE kind IN ('unit_compiles_file','unit_uses_header');",
                        &err) > 0,
             1);
    /* Two analyzer rows now: the old one is history, not a setting, so nothing
     * rewrote it. */
    T_EQ_INT(count_rows(&e, "SELECT COUNT(*) FROM code_analyzers;", &err), 2);
    env_close(&e);
}

/* A rebuild is derived data only. Anything a person or a model put there has to
 * come through untouched, or "rebuild the graph" becomes an operation nobody can
 * safely run. */
static void test_the_rebuild_keeps_every_durable_record(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    seed(&e, &err);
    env_index(&e, &err);
    atlas_reconcile_summary sum;
    run_pass(&e, &sum, &err);
    atlas_reconcile_summary_free(&sum);

    /* A session with a recorded reason and a recorded decision, written the way
     * an adapter writes them. */
    int64_t client_id = 0;
    T_OK(atlas_db_ai_client_upsert(e.db, "anthropic", "claude-code", &client_id, &err), &err);
    int64_t session_id = 0;
    bool created = false;
    T_OK(atlas_db_ai_session_open(e.db, client_id, "analyzer-test-session", 0, NULL, NULL, NULL,
                                  &session_id, &created, &err),
         &err);
    T_CHECK(session_id > 0);
    T_OK(atlas_db_ai_session_attach_repo(e.db, session_id, e.repo_id, "test", NULL, &err), &err);

    atlas_ai_record_input in;
    memset(&in, 0, sizeof(in));
    in.session_id = session_id;
    in.repo_id = e.repo_id;
    in.provenance = "MODEL_PROPOSAL";
    in.state = "proposed";
    in.confidence = "medium";
    in.summary = "because the tests said so";
    int64_t reason_id = 0;
    bool dup = false;
    T_OK(atlas_db_ai_reason_insert(e.db, &in, &reason_id, &dup, &err), &err);
    T_CHECK(reason_id > 0);

    memset(&in, 0, sizeof(in));
    in.session_id = session_id;
    in.repo_id = e.repo_id;
    in.provenance = "MODEL_PROPOSAL";
    in.state = "proposed";
    in.title = "keep the graph derived";
    in.statement = "a rebuild may delete only structural rows";
    int64_t decision_id = 0;
    T_OK(atlas_db_ai_decision_insert(e.db, &in, &decision_id, &dup, &err), &err);
    T_CHECK(decision_id > 0);

    int64_t sessions = count_rows(&e, "SELECT COUNT(*) FROM ai_sessions;", &err);
    int64_t reasons = count_rows(&e, "SELECT COUNT(*) FROM ai_reasons;", &err);
    int64_t decisions = count_rows(&e, "SELECT COUNT(*) FROM ai_decisions;", &err);
    int64_t evidence = count_rows(&e, "SELECT COUNT(*) FROM evidence;", &err);
    int64_t commits = count_rows(&e, "SELECT COUNT(*) FROM commits;", &err);
    int64_t files = count_rows(&e, "SELECT COUNT(*) FROM files;", &err);
    T_CHECK(sessions > 0);
    T_CHECK(reasons > 0);
    T_CHECK(decisions > 0);
    T_CHECK(files > 0);

    pretend_older_analyzer(&e, &err);
    atlas_reconcile_summary after;
    run_pass(&e, &after, &err);
    T_CHECK(after.code.analyzer_changed);
    atlas_reconcile_summary_free(&after);

    T_EQ_INT(count_rows(&e, "SELECT COUNT(*) FROM ai_sessions;", &err), sessions);
    T_EQ_INT(count_rows(&e, "SELECT COUNT(*) FROM ai_reasons;", &err), reasons);
    T_EQ_INT(count_rows(&e, "SELECT COUNT(*) FROM ai_decisions;", &err), decisions);
    T_EQ_INT(count_rows(&e, "SELECT COUNT(*) FROM evidence;", &err), evidence);
    T_EQ_INT(count_rows(&e, "SELECT COUNT(*) FROM commits;", &err), commits);
    T_EQ_INT(count_rows(&e, "SELECT COUNT(*) FROM files;", &err), files);
    /* And they are the same rows, not same-shaped replacements. */
    T_EQ_INT(count_rows(&e,
                        "SELECT COUNT(*) FROM ai_reasons"
                        " WHERE summary='because the tests said so'"
                        "   AND provenance='MODEL_PROPOSAL' AND approved=0;",
                        &err),
             1);
    T_EQ_INT(count_rows(&e,
                        "SELECT COUNT(*) FROM ai_decisions"
                        " WHERE title='keep the graph derived' AND approved=0;",
                        &err),
             1);
    /* A3 still writes no evidence of its own, rebuild or not. */
    T_EQ_INT(count_rows(&e, "SELECT COUNT(*) FROM evidence WHERE kind NOT IN ('SOURCE','GIT');",
                        &err),
             0);
    env_close(&e);
}

/* The identity a reader is shown is Atlas' own, and stays inside the vocabulary
 * the trust boundary allows through unescaped. */
static void test_the_analyzer_identity_is_atlas_owned(void) {
    /* Fixed constants, not configuration: no environment variable, no column and
     * no argument can move either of them. Checked as a shape so that a future
     * change to the value does not need this test edited, but a change to the
     * *kind* of value does. */
    T_CHECK(ATLAS_CODE_ANALYZER_ID[0] != '\0');
    T_CHECK(ATLAS_CODE_ANALYZER_VERSION >= 1);
    for (const char *p = ATLAS_CODE_ANALYZER_ID; *p != '\0'; p++) {
        bool ok = (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-';
        T_CHECK_MSG(ok, "the analyzer identity is not lowercase, digits and hyphens");
    }
}

static const atlas_test TESTS[] = {
    {"a fresh graph records this analyzer", test_a_fresh_graph_records_this_analyzer},
    {"an older analyzer makes the graph stale", test_an_older_analyzer_makes_the_graph_stale},
    {"the rebuild keeps every durable record", test_the_rebuild_keeps_every_durable_record},
    {"the analyzer identity is Atlas-owned", test_the_analyzer_identity_is_atlas_owned},
};

ATLAS_TEST_MAIN("code_analyzer", TESTS)
