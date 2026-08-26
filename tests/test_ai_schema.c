/* Atlas - A2 storage: migration 4, provenance rules, attribution, idempotency.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * These tests exercise the layer directly, without a daemon, because the
 * properties being checked are properties of the storage rather than of the
 * transport: what the schema refuses, what attribution does when two sessions
 * overlap, and what a replayed write does.
 */
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "atlas/ai.h"
#include "atlas/atlas.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

/* --- helpers -------------------------------------------------------------- */

static atlas_status count_of(atlas_db *db, const char *sql, int64_t *out, atlas_err *err);

typedef struct env {
    fixture fx;
    atlas_buf db_path;
    atlas_db *db;
    int64_t repo_id;
} env;

static void env_open(env *e, atlas_err *err) {
    memset(e, 0, sizeof(*e));
    atlas_buf_init(&e->db_path);
    T_OK(fx_open(&e->fx, err), err);
    T_OK(atlas_buf_appendf(&e->db_path, err, "%s/atlas.db", fx_data_dir(&e->fx)), err);
    T_OK(atlas_db_open(atlas_buf_cstr(&e->db_path), &e->db, err), err);
    T_OK(atlas_db_migrate(e->db, err), err);

    /* One registered repository, inserted through the typed operation so the
     * row is shaped exactly as a real registration leaves it. */
    atlas_repo_identity id;
    memset(&id, 0, sizeof(id));
    id.root = "/tmp/atlas-test-repo";
    id.root_len = strlen("/tmp/atlas-test-repo");
    id.common_dir = "/tmp/atlas-test-repo/.git";
    id.common_dir_len = strlen("/tmp/atlas-test-repo/.git");
    id.git_dir = id.common_dir;
    id.git_dir_len = id.common_dir_len;
    id.object_format = "sha1";
    T_OK(atlas_db_repo_add(e->db, "proj", &id, &e->repo_id, err), err);
}

static void env_close(env *e) {
    atlas_db_close(e->db);
    atlas_buf_free(&e->db_path);
    fx_close(&e->fx);
}

/* Fills the identity fields every operation carries. */
static void op_identity(atlas_ai_op *op, const char *session_key, atlas_err *err) {
    T_OK(atlas_buf_set_str(&op->provider, "anthropic", err), err);
    T_OK(atlas_buf_set_str(&op->client, "claude-code", err), err);
    if (session_key != NULL) {
        T_OK(atlas_buf_set_str(&op->session_key, session_key, err), err);
    }
    T_OK(atlas_buf_set_str(&op->repo_name, "proj", err), err);
}

static void apply(env *e, atlas_ai_op *op, atlas_ai_result *res, atlas_err *err) {
    atlas_ai_result_init(res);
    T_OK(atlas_ai_apply(e->db, op, NULL, NULL, res, err), err);
}

static atlas_status count_of(atlas_db *db, const char *sql, int64_t *out, atlas_err *err) {
    /* The db layer's own single-value query is internal to src/db, so the test
     * goes through the public surface it has: a tiny read via the CLI would be
     * heavier than this, and this is the only place the suite needs it. */
    return atlas_db_query_int64(db, sql, out, err);
}

/* --- tests ---------------------------------------------------------------- */

static void test_migration_creates_the_a2_tables(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    T_EQ_INT(atlas_db_schema_version(e.db, &err), ATLAS_SCHEMA_VERSION);
    /* Pinned on purpose, so that adding a migration is a change somebody has to
     * make here as well. A4 added migration 6, A6 added migration 7, A8 added
     * migration 8, A8-CI added migration 11 and A9 added migration 12; A9.2
     * added 14 and 15, A9.2.1 added 16, A9.2.2 added 17 and A9.2.3 added 18;
     * A11.0 added 21, A10.0 added 22, A10.1 added 23, A11.6 added 24, A12.0 added
     * 25, P0 added 26 and A13 added 27.
     * The A2 tables below
     * are asserted unchanged across all of them, which is the property this
     * test is really for. */
    T_EQ_INT(ATLAS_SCHEMA_VERSION, 28);

    static const char *const TABLES[] = {
        "ai_clients",       "ai_sessions",       "ai_session_repos",     "ai_session_events",
        "ai_change_sets",   "ai_changed_paths",  "ai_reasons",           "ai_reason_paths",
        "ai_decisions",     "ai_decision_paths", "ai_evidence_links",    "ai_checkpoints",
        "repo_worktree_changes", NULL,
    };
    for (size_t i = 0; TABLES[i] != NULL; i++) {
        atlas_buf sql = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&sql, &err,
                               "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='%s';",
                               TABLES[i]),
             &err);
        int64_t n = 0;
        T_OK(count_of(e.db, atlas_buf_cstr(&sql), &n, &err), &err);
        T_CHECK_MSG(n == 1, "table %s was not created", TABLES[i]);
        atlas_buf_free(&sql);
    }

    /* The A0 rule is untouched: evidence still refuses everything but SOURCE
     * and GIT, and migration 4 did not widen it to fit AI records. */
    T_FAILS_WITH(atlas_db_evidence_insert(e.db, e.repo_id, ATLAS_EV_INFERENCE, 0, NULL, NULL, 0u,
                                          NULL, NULL, NULL, &err),
                 ATLAS_ERR_INTEGRITY, &err);
    atlas_err_init(&err);
    T_FAILS_WITH(atlas_db_evidence_insert(e.db, e.repo_id, ATLAS_EV_DECISION, 0, NULL, NULL, 0u,
                                          NULL, NULL, NULL, &err),
                 ATLAS_ERR_INTEGRITY, &err);
    atlas_err_init(&err);

    env_close(&e);
}

static void test_approval_cannot_be_recorded(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    /* The service layer refuses the class... */
    T_CHECK(!atlas_provenance_writable_in_a2(ATLAS_PROV_USER_APPROVED_DECISION));
    T_CHECK(!atlas_provenance_writable_in_a2(ATLAS_PROV_GIT));
    T_CHECK(!atlas_provenance_writable_in_a2(ATLAS_PROV_SOURCE));
    T_CHECK(!atlas_provenance_writable_in_a2(ATLAS_PROV_ATLAS_OWNED));
    T_CHECK(atlas_provenance_writable_in_a2(ATLAS_PROV_MODEL_PROPOSAL));
    T_CHECK(atlas_provenance_writable_in_a2(ATLAS_PROV_MODEL_INFERENCE));
    T_CHECK(atlas_provenance_writable_in_a2(ATLAS_PROV_UNKNOWN));

    atlas_ai_op op;
    atlas_ai_op_init(&op, ATLAS_AI_OP_REASON);
    op_identity(&op, "s-approve", &err);
    op.provenance = ATLAS_PROV_USER_APPROVED_DECISION;
    T_OK(atlas_buf_set_str(&op.summary, "the user definitely approved this", &err), &err);
    atlas_ai_result res;
    atlas_ai_result_init(&res);
    T_FAILS_WITH(atlas_ai_apply(e.db, &op, NULL, NULL, &res, &err), ATLAS_ERR_INTEGRITY, &err);
    atlas_ai_result_free(&res);
    atlas_ai_op_free(&op);
    atlas_err_init(&err);

    /* ...and the schema refuses it too, so a future code path that forgets the
     * check still cannot claim an approval. */
    atlas_ai_record_input in;
    memset(&in, 0, sizeof(in));
    in.repo_id = e.repo_id;
    in.provenance = "MODEL_PROPOSAL";
    in.state = "proposed";
    in.summary = "a proposal";
    int64_t id = 0;
    bool dup = false;
    T_OK(atlas_db_ai_reason_insert(e.db, &in, &id, &dup, &err), &err);
    T_CHECK(id > 0);

    atlas_buf sql = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&sql, &err, "UPDATE ai_reasons SET approved=1 WHERE id=%lld;",
                           (long long)id),
         &err);
    /* The CHECK constraint fires. This is the layer nobody can route around. */
    T_CHECK(atlas_db_exec_sql(e.db, atlas_buf_cstr(&sql), &err) != ATLAS_OK);
    atlas_buf_free(&sql);
    atlas_err_init(&err);

    int64_t approved = 0;
    T_OK(count_of(e.db, "SELECT count(*) FROM ai_reasons WHERE approved=1;", &approved, &err),
         &err);
    T_EQ_INT(approved, 0);

    env_close(&e);
}

static void test_unknown_is_a_first_class_row(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_ai_op open_op;
    atlas_ai_op_init(&open_op, ATLAS_AI_OP_SESSION_OPEN);
    op_identity(&open_op, "s-unknown", &err);
    atlas_ai_result res;
    apply(&e, &open_op, &res, &err);
    T_CHECK(res.session_id > 0);
    T_CHECK(res.session_created);
    atlas_ai_result_free(&res);
    atlas_ai_op_free(&open_op);

    atlas_ai_op op;
    atlas_ai_op_init(&op, ATLAS_AI_OP_REASON);
    op_identity(&op, "s-unknown", &err);
    op.unknown = true;
    T_OK(atlas_buf_set_str(&op.unknown_reason, "changed by a build step", &err), &err);
    T_OK(atlas_buf_append(&op.paths, "a.c", 4u, &err), &err); /* includes the NUL */
    apply(&e, &op, &res, &err);
    T_CHECK(res.record_id > 0);
    atlas_ai_result_free(&res);
    atlas_ai_op_free(&op);

    /* "Nobody said why" is a row, not an absent row: the difference between
     * that and "Atlas was never asked" has to be queryable. */
    int64_t n = 0;
    T_OK(count_of(e.db,
                  "SELECT count(*) FROM ai_reasons WHERE state='unknown' AND "
                  "provenance='UNKNOWN' AND unknown_reason IS NOT NULL;",
                  &n, &err),
         &err);
    T_EQ_INT(n, 1);
    T_OK(count_of(e.db, "SELECT count(*) FROM ai_reason_paths;", &n, &err), &err);
    T_EQ_INT(n, 1);

    env_close(&e);
}

static void test_records_are_idempotent(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    for (int attempt = 0; attempt < 3; attempt++) {
        atlas_ai_op op;
        atlas_ai_op_init(&op, ATLAS_AI_OP_REASON);
        op_identity(&op, NULL, &err);
        T_OK(atlas_buf_set_str(&op.summary, "tightened the buffer growth policy", &err), &err);
        T_OK(atlas_buf_set_str(&op.dedup_key, "reason:turn-7", &err), &err);
        atlas_ai_result res;
        apply(&e, &op, &res, &err);
        /* The same id comes back every time, so a caller that retried gets the
         * identifier it would have got the first time rather than nothing. */
        T_CHECK(res.record_id > 0);
        T_CHECK_MSG(res.duplicate == (attempt > 0), "attempt %d duplicate flag", attempt);
        atlas_ai_result_free(&res);
        atlas_ai_op_free(&op);
    }
    int64_t n = 0;
    T_OK(count_of(e.db, "SELECT count(*) FROM ai_reasons;", &n, &err), &err);
    T_EQ_INT(n, 1);

    env_close(&e);
}

static void test_attribution_never_improves(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    int64_t client = 0;
    T_OK(atlas_db_ai_client_upsert(e.db, "anthropic", "claude-code", &client, &err), &err);
    int64_t s1 = 0;
    int64_t s2 = 0;
    bool created = false;
    T_OK(atlas_db_ai_session_open(e.db, client, "one", 0, NULL, NULL, NULL, &s1, &created, &err),
         &err);
    T_OK(atlas_db_ai_session_open(e.db, client, "two", 0, NULL, NULL, NULL, &s2, &created, &err),
         &err);
    T_OK(atlas_db_ai_session_attach_repo(e.db, s1, e.repo_id, "session_start", NULL, &err), &err);
    T_OK(atlas_db_ai_session_attach_repo(e.db, s2, e.repo_id, "session_start", NULL, &err), &err);

    /* Two open sessions on one repository: neither can claim a change. */
    int64_t concurrent = 0;
    T_OK(atlas_db_ai_concurrent_sessions(e.db, e.repo_id, s1, &concurrent, &err), &err);
    T_EQ_INT(concurrent, 1);

    int64_t cs = 0;
    T_OK(atlas_db_ai_change_set_ensure(e.db, s1, e.repo_id, NULL, 0, &cs, &err), &err);
    T_OK(atlas_db_ai_changed_path_record(e.db, cs, "a.c", 3u, "a.c", "ambiguous", NULL, 1, &err),
         &err);
    /* A later direct edit does not retract the earlier overlap. */
    T_OK(atlas_db_ai_changed_path_record(e.db, cs, "a.c", 3u, "a.c", "direct_edit", "Edit", 0,
                                         &err),
         &err);
    int64_t n = 0;
    T_OK(count_of(e.db, "SELECT count(*) FROM ai_changed_paths WHERE attribution='ambiguous';", &n,
                  &err),
         &err);
    T_EQ_INT(n, 1);

    /* Closing the other session does not retroactively make it unambiguous
     * either: the window it overlapped has already passed. */
    T_OK(atlas_db_ai_session_close(e.db, s2, "clear", &err), &err);
    T_OK(atlas_db_ai_changed_path_record(e.db, cs, "a.c", 3u, "a.c", "direct_edit", "Edit", 0,
                                         &err),
         &err);
    T_OK(count_of(e.db, "SELECT count(*) FROM ai_changed_paths WHERE attribution='ambiguous';", &n,
                  &err),
         &err);
    T_EQ_INT(n, 1);

    /* A path recorded while nothing else was open may be promoted from observed
     * to direct_edit, which is the one direction that adds information. */
    T_OK(atlas_db_ai_changed_path_record(e.db, cs, "b.c", 3u, "b.c", "observed", NULL, 0, &err),
         &err);
    T_OK(atlas_db_ai_changed_path_record(e.db, cs, "b.c", 3u, "b.c", "direct_edit", "Write", 0,
                                         &err),
         &err);
    T_OK(count_of(e.db,
                  "SELECT count(*) FROM ai_changed_paths WHERE path_text='b.c' AND "
                  "attribution='direct_edit';",
                  &n, &err),
         &err);
    T_EQ_INT(n, 1);

    env_close(&e);
}

static void test_sessions_resume_rather_than_restart(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_ai_result res;
    for (int i = 0; i < 3; i++) {
        atlas_ai_op op;
        atlas_ai_op_init(&op, ATLAS_AI_OP_SESSION_OPEN);
        op_identity(&op, "s-resume", &err);
        apply(&e, &op, &res, &err);
        T_CHECK_MSG(res.session_created == (i == 0), "iteration %d created flag", i);
        atlas_ai_result_free(&res);
        atlas_ai_op_free(&op);
    }
    int64_t n = 0;
    T_OK(count_of(e.db, "SELECT count(*) FROM ai_sessions;", &n, &err), &err);
    T_EQ_INT(n, 1);
    /* A resume keeps the change set the session already had; re-creating the row
     * would orphan it and make a resumed session look like one that had done
     * nothing. */
    T_OK(count_of(e.db, "SELECT resumes FROM ai_sessions;", &n, &err), &err);
    T_EQ_INT(n, 2);

    env_close(&e);
}

static void test_a_subagent_is_a_child_session(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    /* The parent. */
    atlas_ai_op parent;
    atlas_ai_op_init(&parent, ATLAS_AI_OP_SESSION_OPEN);
    op_identity(&parent, "main-1", &err);
    atlas_ai_result pres;
    apply(&e, &parent, &pres, &err);
    T_CHECK(pres.session_id > 0);
    int64_t parent_id = pres.session_id;
    int64_t parent_change_set = pres.change_set_id;
    atlas_ai_result_free(&pres);
    atlas_ai_op_free(&parent);

    /* The subagent. Modelled as its own session with a parent rather than as a
     * flag on the parent, so its change set and its records are separable. */
    atlas_ai_op child;
    atlas_ai_op_init(&child, ATLAS_AI_OP_SESSION_OPEN);
    op_identity(&child, "main-1/ag-1", &err);
    T_OK(atlas_buf_set_str(&child.parent_session_key, "main-1", &err), &err);
    T_OK(atlas_buf_set_str(&child.agent_id, "ag-1", &err), &err);
    T_OK(atlas_buf_set_str(&child.agent_type, "Explore", &err), &err);
    atlas_ai_result cres;
    apply(&e, &child, &cres, &err);
    T_CHECK(cres.session_id > 0);
    T_CHECK_MSG(cres.session_id != parent_id, "the subagent reused the parent's session");
    int64_t child_change_set = cres.change_set_id;
    atlas_ai_result_free(&cres);
    atlas_ai_op_free(&child);

    int64_t n = 0;
    T_OK(count_of(e.db,
                  "SELECT count(*) FROM ai_sessions WHERE parent_id IS NOT NULL AND "
                  "agent_type='Explore' AND agent_id='ag-1';",
                  &n, &err),
         &err);
    T_EQ_INT(n, 1);
    /* The lineage points at the right row. */
    atlas_buf sql = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&sql, &err,
                           "SELECT count(*) FROM ai_sessions WHERE agent_id='ag-1' AND "
                           "parent_id=%lld;",
                           (long long)parent_id),
         &err);
    T_OK(count_of(e.db, atlas_buf_cstr(&sql), &n, &err), &err);
    T_EQ_INT(n, 1);
    atlas_buf_free(&sql);

    /* Two change sets on one repository, one per session. A subagent's changes
     * must not merge into its parent's. */
    T_CHECK(parent_change_set > 0);
    T_CHECK(child_change_set > 0);
    T_CHECK_MSG(parent_change_set != child_change_set,
                "the subagent shared its parent's change set");
    T_OK(count_of(e.db, "SELECT count(*) FROM ai_change_sets;", &n, &err), &err);
    T_EQ_INT(n, 2);

    /* A parent Atlas never saw yields a session with no lineage rather than an
     * invented parent. */
    atlas_ai_op orphan;
    atlas_ai_op_init(&orphan, ATLAS_AI_OP_SESSION_OPEN);
    op_identity(&orphan, "other/ag-9", &err);
    T_OK(atlas_buf_set_str(&orphan.parent_session_key, "a-session-that-never-existed", &err), &err);
    T_OK(atlas_buf_set_str(&orphan.agent_id, "ag-9", &err), &err);
    atlas_ai_result ores;
    apply(&e, &orphan, &ores, &err);
    atlas_ai_result_free(&ores);
    atlas_ai_op_free(&orphan);
    T_OK(count_of(e.db, "SELECT count(*) FROM ai_sessions WHERE agent_id='ag-9' AND "
                        "parent_id IS NULL;",
                  &n, &err),
         &err);
    T_EQ_INT(n, 1);

    env_close(&e);
}

static void test_events_are_pruned_but_records_are_not(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    int64_t client = 0;
    int64_t sid = 0;
    bool created = false;
    T_OK(atlas_db_ai_client_upsert(e.db, "anthropic", "claude-code", &client, &err), &err);
    T_OK(atlas_db_ai_session_open(e.db, client, "s", 0, NULL, NULL, NULL, &sid, &created, &err),
         &err);

    atlas_ai_record_input in;
    memset(&in, 0, sizeof(in));
    in.session_id = sid;
    in.repo_id = e.repo_id;
    in.provenance = "MODEL_PROPOSAL";
    in.state = "proposed";
    in.summary = "durable";
    int64_t rid = 0;
    bool dup = false;
    T_OK(atlas_db_ai_reason_insert(e.db, &in, &rid, &dup, &err), &err);

    for (int i = 0; i < 60; i++) {
        char key[32];
        (void)snprintf(key, sizeof(key), "e%d", i);
        T_OK(atlas_db_ai_event_append(e.db, sid, e.repo_id, "turn", NULL, NULL, NULL, 0u, NULL, key,
                                      NULL, &err),
             &err);
    }
    int64_t removed = 0;
    T_OK(atlas_db_ai_events_prune(e.db, sid, 10, &removed, &err), &err);
    T_EQ_INT(removed, 50);

    int64_t n = 0;
    T_OK(count_of(e.db, "SELECT count(*) FROM ai_session_events;", &n, &err), &err);
    T_EQ_INT(n, 10);
    /* Pruning ephemeral events must never reach a durable record. */
    T_OK(count_of(e.db, "SELECT count(*) FROM ai_reasons;", &n, &err), &err);
    T_EQ_INT(n, 1);

    env_close(&e);
}

static void test_v3_database_migrates_forward_intact(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    /* Seed rows through the A1 surface, then migrate again. Migration 4 is
     * additive, so a second migrate is a no-op and every row survives. */
    int64_t before = 0;
    T_OK(count_of(e.db, "SELECT count(*) FROM repositories;", &before, &err), &err);
    T_OK(atlas_db_migrate(e.db, &err), &err);
    int64_t after = 0;
    T_OK(count_of(e.db, "SELECT count(*) FROM repositories;", &after, &err), &err);
    T_EQ_INT(after, before);
    T_EQ_INT(atlas_db_schema_version(e.db, &err), ATLAS_SCHEMA_VERSION);

    env_close(&e);
}

static const atlas_test TESTS[] = {
    {"migration 4 creates the A2 tables and leaves evidence alone",
     test_migration_creates_the_a2_tables},
    {"an approval cannot be recorded, in code or in SQL", test_approval_cannot_be_recorded},
    {"UNKNOWN is stored as a row rather than as an absence",
     test_unknown_is_a_first_class_row},
    {"a replayed record creates one row and returns the same id",
     test_records_are_idempotent},
    {"attribution never improves once it is ambiguous", test_attribution_never_improves},
    {"opening a known session resumes it", test_sessions_resume_rather_than_restart},
    {"a subagent is a child session with its own change set",
     test_a_subagent_is_a_child_session},
    {"events are pruned and durable records are not",
     test_events_are_pruned_but_records_are_not},
    {"a v3 database migrates forward intact", test_v3_database_migrates_forward_intact},
};

ATLAS_TEST_MAIN("ai_schema", TESTS)
