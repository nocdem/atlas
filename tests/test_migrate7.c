/* Atlas - migrating a populated schema-6 database forward to schema 7.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Migration 7 is the first one in Atlas that **rebuilds an existing table**
 * rather than only adding new ones: `decision_challenges` gains a member in its
 * `intent` CHECK, which SQLite cannot widen in place.
 *
 * That makes one thing worth asserting above all others. `decision_events`
 * points at a challenge by row id and carries no foreign key, so a rebuild that
 * renumbered would silently re-point every approval record at somebody else's
 * capability — and nothing about the result would look wrong. The id-preserving
 * copy is checked row by row here.
 *
 * It also asserts A5's guarantee across the boundary: a backup taken at schema 6
 * still verifies without being migrated, because `backup verify` opens the file
 * read-only, and a restored schema-6 database migrates forward through the
 * ordinary supported path.
 */
#define _GNU_SOURCE 1

#include <stdio.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/decision_ops.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

static int64_t count_of(atlas_db *db, const char *sql, atlas_err *err) {
    sqlite3_stmt *st = NULL;
    T_OK(atlas_db_prepare(db, sql, &st, err), err);
    int64_t n = -1;
    if (sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int64(st, 0);
    }
    atlas_db_finish(db, st);
    return n;
}

static void text_of(atlas_db *db, const char *sql, atlas_buf *out, atlas_err *err) {
    sqlite3_stmt *st = NULL;
    T_OK(atlas_db_prepare(db, sql, &st, err), err);
    atlas_buf_reset(out);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(st, 0);
        T_OK(atlas_buf_appendf(out, err, "%s\n", t == NULL ? "" : t), err);
    }
    atlas_db_finish(db, st);
}

/* A schema-6 database with real decision records in it.
 *
 * Built by migrating to head and then *removing* migration 7's effects, which
 * is the only way to get a genuine schema-6 shape out of a build whose
 * migration list ends at 7. The challenge rows are written first, so the ids
 * this test cares about are ids the real write path assigned. */
static void build_schema6(const char *path, atlas_err *err) {
    atlas_db *db = NULL;
    T_OK(atlas_db_open(path, &db, err), err);
    T_OK(atlas_db_migrate(db, err), err);

    atlas_repo_identity id;
    memset(&id, 0, sizeof id);
    id.root = "/tmp/atlas-migrate7-repo";
    id.root_len = strlen(id.root);
    id.common_dir = "/tmp/atlas-migrate7-repo/.git";
    id.common_dir_len = strlen(id.common_dir);
    id.git_dir = id.common_dir;
    id.git_dir_len = id.common_dir_len;
    id.object_format = "sha1";
    int64_t repo_id = 0;
    T_OK(atlas_db_repo_add(db, "proj", &id, &repo_id, err), err);

    /* Three decisions, each proposed and approved, so there are consumed
     * challenges with non-trivial ids and events pointing at them. */
    for (int i = 0; i < 3; i++) {
        atlas_decision_op op;
        atlas_decision_op_init(&op, ATLAS_DECISION_OP_PROPOSE);
        T_OK(atlas_buf_set_str(&op.repo_name, "proj", err), err);
        char title[64];
        (void)snprintf(title, sizeof title, "Decision number %d", i);
        T_OK(atlas_buf_set_str(&op.revision.title, title, err), err);
        T_OK(atlas_buf_set_str(&op.revision.decision_text, "It is settled.", err), err);
        op.revision.scope = ATLAS_DECISION_SCOPE_REPOSITORY;
        op.revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
        atlas_decision_result res;
        atlas_decision_result_init(&res);
        T_OK(atlas_decision_apply(db, &op, &res, err), err);
        atlas_buf uid = ATLAS_BUF_INIT;
        T_OK(atlas_buf_set(&uid, res.uid.data, res.uid.len, err), err);
        atlas_decision_result_free(&res);
        atlas_decision_op_free(&op);

        atlas_decision_op ch;
        atlas_decision_op_init(&ch, ATLAS_DECISION_OP_CHALLENGE);
        T_OK(atlas_buf_set_str(&ch.repo_name, "proj", err), err);
        T_OK(atlas_buf_set_str(&ch.uid, atlas_buf_cstr(&uid), err), err);
        ch.intent = ATLAS_DECISION_INTENT_APPROVE;
        atlas_decision_result cr;
        atlas_decision_result_init(&cr);
        T_OK(atlas_decision_apply(db, &ch, &cr, err), err);
        atlas_buf token = ATLAS_BUF_INIT;
        T_OK(atlas_buf_set(&token, cr.token.data, cr.token.len, err), err);
        char confirm[ATLAS_DECISION_CONFIRM_MAX];
        (void)snprintf(confirm, sizeof confirm, "%s", cr.confirm);
        atlas_decision_result_free(&cr);
        atlas_decision_op_free(&ch);

        atlas_decision_op ap;
        atlas_decision_op_init(&ap, ATLAS_DECISION_OP_APPROVE);
        T_OK(atlas_buf_set_str(&ap.repo_name, "proj", err), err);
        T_OK(atlas_buf_set_str(&ap.uid, atlas_buf_cstr(&uid), err), err);
        T_OK(atlas_buf_set(&ap.token, token.data, token.len, err), err);
        T_OK(atlas_buf_set_str(&ap.confirmation, confirm, err), err);
        atlas_decision_result ar;
        atlas_decision_result_init(&ar);
        T_OK(atlas_decision_apply(db, &ap, &ar, err), err);
        atlas_decision_result_free(&ar);
        atlas_decision_op_free(&ap);
        atlas_buf_free(&token);
        atlas_buf_free(&uid);
    }

    /* Now make it look like schema 6: drop A6's table, put the challenge table
     * back to its migration-6 shape *with the same row ids*, and rewind the
     * recorded version. */
    T_OK(atlas_db_exec_sql(
             db,
             "DROP TABLE verify_lifecycle_audit;"
             "DROP TABLE verify_reliability;"
             "DROP TABLE verify_outcomes;"
             "DROP TABLE verify_results;"
             "DROP TABLE verify_attestation_evidence;"
             "DROP TABLE verify_attestations;"
             "DROP TABLE verify_evidence_deps;"
             "DROP TABLE verify_evidence;"
             "DROP TABLE verify_claims;"
             "DROP TABLE verify_actors;"
             "DROP TABLE gw_audit;"
             "DROP TABLE api_keys;"
             "DROP TABLE decision_validations;"
             "CREATE TABLE dc6 ("
             "  id INTEGER PRIMARY KEY, token TEXT NOT NULL UNIQUE, repo_id INTEGER NOT NULL,"
             "  document_id INTEGER NOT NULL REFERENCES decision_documents(id),"
             "  revision_id INTEGER NOT NULL REFERENCES decision_revisions(id),"
             "  revision_no INTEGER NOT NULL, content_hash TEXT NOT NULL,"
             "  intent TEXT NOT NULL CHECK(intent IN ('approve','reject','supersede')),"
             "  supersede_document_id INTEGER, created_at TEXT NOT NULL,"
             "  expires_at TEXT NOT NULL, consumed INTEGER NOT NULL DEFAULT 0,"
             "  consumed_at TEXT);"
             "INSERT INTO dc6 SELECT id, token, repo_id, document_id, revision_id, revision_no,"
             "  content_hash, intent, supersede_document_id, created_at, expires_at, consumed,"
             "  consumed_at FROM decision_challenges;"
             "DROP TABLE decision_challenges;"
             "ALTER TABLE dc6 RENAME TO decision_challenges;"
             "CREATE INDEX idx_decision_challenges_repo"
             "  ON decision_challenges(repo_id, consumed, expires_at);"
             /* A8's tables go too. `atlas_db_migrate` above ran every
              * migration, so this database is at the current schema and is
              * being wound back to look like six; leaving the orchestration
              * tables in place while rewinding the recorded version would
              * present migration 8 with tables it is about to create. That is
              * a property of this simulation, not of the migration. */
             
             /* A11.0's table. Winding back past 8 winds back past 21, and a rewind
              * that leaves a later migration's table behind is not a database at the
              * version it claims: migration 21 would then fail to create it. */
             /* A10.1's table, dropped before A10.0's for the same reason both are
              * dropped at all: a rewind that leaves a later migration's table
              * behind is not a database at the version it claims, and migration
              * 23 would then fail to create it. */
             /* A12.0's three tables, children before parents, and the index
              * migration 25 put on `orch_jobs` before the table it is on: a
              * rewind that leaves a later migration's object behind is not a
              * database at the version it claims, and migration 25 would then
              * fail to create it. */
             "DROP TABLE orch_plan_tasks;"
             "DROP TABLE orch_plan_revisions;"
             "DROP TABLE orch_plans;"
             "DROP INDEX idx_orch_jobs_correlation;"
             "DROP TABLE orch_run_memory;"
             "DROP TABLE orch_usage;"
        "DROP TABLE orch_runs;"
             "DROP TABLE orch_snapshot_entries;"
             "DROP TABLE orch_snapshots;"
             "DROP TABLE orch_observations;"
             "DROP TABLE orch_idempotency;"
             "DROP TABLE orch_artifacts;"
             "DROP TABLE orch_events;"
             "DROP TABLE orch_transitions;"
             "DROP TABLE orch_leases;"
             "DROP TABLE orch_attempts;"
             "DROP TABLE orch_jobs;"
             /* Migration 11's tables, children before parents, and A9.2.3's row
              * with them: a rewind that leaves a later migration's table behind
              * is not a database at the version it claims. */
             /* A9.2.5's table, dropped first: a rewind that leaves a later
              * migration's table behind is not a database at the version it
              * claims, and migration 20 would then fail to create it. */
             "DROP TABLE sem_discovery_obstacles;"
             "DROP TABLE sem_build_inputs;"
             "DROP TABLE sem_repo_config;"
             "DROP TABLE sem_includes;"
             "DROP TABLE sem_edges;"
             "DROP TABLE sem_symbols;"
             "DROP TABLE sem_units;"
             "DROP TABLE sem_compdbs;"
             "DROP TABLE sem_current;"
             "DROP TABLE sem_generations;"
             /* Migration 10's table. Winding back below 7 means winding back
              * past 10 as well, and a rewind that leaves a later migration's
              * table behind is not a schema-6 database. */
             "DROP TABLE decision_edge_events;"
             /* Migration 27 added a column to `repositories`, and migration 30
              * (the T14 fix round) added another. A rewind that leaves a later
              * migration's *column* behind is no more a schema-N database than
              * one that leaves its table behind, and re-running the chain would
              * fail with "duplicate column name". */
             "ALTER TABLE repositories DROP COLUMN scanner_uid;"
             "ALTER TABLE repositories DROP COLUMN mirror_complete;"
             "ALTER TABLE repositories DROP COLUMN mirror_at;"
             "ALTER TABLE repositories DROP COLUMN trailer_scan_high;"
             /* A12.1's eight tables, children before parents: migration 29
              * runs again on top of this rewind, and a rewind that leaves a
              * later migration's table behind is not a database at the
              * version it claims. */
             "DROP TABLE memory_unanchored;"
             "DROP TABLE memory_claim_diffs;"
             "DROP TABLE memory_source_versions;"
             "DROP TABLE memory_generations;"
             "DROP TABLE memory_sources;"
             "DROP TABLE memory_claim_anchors;"
             "DROP TABLE memory_context_packs;"
             "DROP TABLE memory_trailer_bindings;"
             "DELETE FROM schema_migrations WHERE version >= 7;",
             err),
         err);
    atlas_db_close(db);
}

/* --- the migration ---------------------------------------------------------- */

static void test_a_populated_schema_six_database_reaches_seven_losslessly(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_OK(fx_open(&fx, &err), &err);
    atlas_buf path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&path, &err, "%s/atlas.db", fx_data_dir(&fx)), &err);
    build_schema6(atlas_buf_cstr(&path), &err);

    atlas_db *db = NULL;
    T_OK(atlas_db_open(atlas_buf_cstr(&path), &db, &err), &err);
    T_EQ_INT(atlas_db_schema_version(db, &err), 6);

    /* Everything about the challenges, before. Ids and intents together,
     * because the id is the fact `decision_events.challenge_id` depends on. */
    atlas_buf before = ATLAS_BUF_INIT;
    text_of(db,
            "SELECT id || '|' || token || '|' || intent || '|' || consumed || '|' || content_hash"
            "  FROM decision_challenges ORDER BY id;",
            &before, &err);
    atlas_buf events_before = ATLAS_BUF_INIT;
    text_of(db, "SELECT id || '|' || IFNULL(challenge_id,0) || '|' || event FROM decision_events"
                " ORDER BY id;",
            &events_before, &err);
    int64_t docs_before = count_of(db, "SELECT COUNT(*) FROM decision_documents;", &err);
    int64_t revs_before = count_of(db, "SELECT COUNT(*) FROM decision_revisions;", &err);
    T_CHECK(before.len > 0);
    T_EQ_INT(docs_before, 3);
    T_EQ_INT(revs_before, 3);

    /* The migration under test. */
    T_OK(atlas_db_migrate(db, &err), &err);
    /* Forward to the *current* schema, not to seven: a database that stopped at
     * the version this suite was written for would be one no shipped Atlas can
     * open. What the suite is about — that migration seven rebuilt the challenge
     * table without renumbering a row — is asserted below and is unaffected by
     * later migrations running on top of it. */
    T_EQ_INT(atlas_db_schema_version(db, &err), ATLAS_SCHEMA_VERSION);
    /* Migration 31 (A16's T2) landed after this suite was written; migration
     * 31 rebuilds `decision_challenges` again, on top of what this suite
     * asserts about migration 7's own rebuild, and the explicit column list
     * below is what keeps this comparison about seven's columns only. */
    T_EQ_INT(ATLAS_SCHEMA_VERSION, 31);

    atlas_buf after = ATLAS_BUF_INIT;
    text_of(db,
            "SELECT id || '|' || token || '|' || intent || '|' || consumed || '|' || content_hash"
            "  FROM decision_challenges ORDER BY id;",
            &after, &err);
    /* Row for row, id for id. A renumbering here would re-point every approval
     * record at somebody else's capability, and nothing later would notice. */
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&before), atlas_buf_cstr(&after)) == 0,
                "the challenge rebuild changed rows:\nbefore:\n%s\nafter:\n%s",
                atlas_buf_cstr(&before), atlas_buf_cstr(&after));

    atlas_buf events_after = ATLAS_BUF_INIT;
    text_of(db, "SELECT id || '|' || IFNULL(challenge_id,0) || '|' || event FROM decision_events"
                " ORDER BY id;",
            &events_after, &err);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&events_before), atlas_buf_cstr(&events_after)) == 0,
                "the ledger changed across the migration");

    /* Every event that names a challenge still finds it, and finds the one it
     * named. This is the join that would have broken silently. */
    T_EQ_INT(count_of(db,
                      "SELECT COUNT(*) FROM decision_events e"
                      " WHERE e.challenge_id IS NOT NULL"
                      "   AND NOT EXISTS (SELECT 1 FROM decision_challenges c"
                      "                    WHERE c.id = e.challenge_id"
                      "                      AND c.content_hash = e.content_hash);",
                      &err),
             0);

    T_EQ_INT(count_of(db, "SELECT COUNT(*) FROM decision_documents;", &err), docs_before);
    T_EQ_INT(count_of(db, "SELECT COUNT(*) FROM decision_revisions;", &err), revs_before);
    /* And the new table exists and is empty. */
    T_EQ_INT(count_of(db, "SELECT COUNT(*) FROM decision_validations;", &err), 0);
    /* The widened vocabulary is actually widened. */
    T_OK(atlas_db_exec_sql(db,
                           "INSERT INTO decision_challenges"
                           "(token, repo_id, document_id, revision_id, revision_no, content_hash,"
                           " intent, created_at, expires_at)"
                           " SELECT 'probe', repo_id, document_id, revision_id, revision_no,"
                           "        content_hash, 'revalidate', created_at, expires_at"
                           "   FROM decision_challenges LIMIT 1;",
                           &err),
         &err);
    T_EQ_INT(count_of(db, "SELECT COUNT(*) FROM decision_challenges WHERE intent='revalidate';",
                      &err),
             1);

    /* Foreign keys still hold and the file is still structurally sound. */
    atlas_buf report = ATLAS_BUF_INIT;
    T_OK(atlas_db_foreign_key_check(db, &report, &err), &err);
    T_EQ_STR(atlas_buf_cstr(&report), "ok");
    atlas_buf_reset(&report);
    T_OK(atlas_db_integrity_check(db, &report, &err), &err);
    T_EQ_STR(atlas_buf_cstr(&report), "ok");

    atlas_buf_free(&report);
    atlas_buf_free(&before);
    atlas_buf_free(&after);
    atlas_buf_free(&events_before);
    atlas_buf_free(&events_after);
    atlas_db_close(db);
    atlas_buf_free(&path);
    fx_close(&fx);
}

static void test_migration_seven_is_idempotent(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_OK(fx_open(&fx, &err), &err);
    atlas_buf path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&path, &err, "%s/atlas.db", fx_data_dir(&fx)), &err);
    build_schema6(atlas_buf_cstr(&path), &err);

    atlas_db *db = NULL;
    T_OK(atlas_db_open(atlas_buf_cstr(&path), &db, &err), &err);
    T_OK(atlas_db_migrate(db, &err), &err);
    atlas_buf schema_once = ATLAS_BUF_INIT;
    text_of(db, "SELECT type || ' ' || name FROM sqlite_master ORDER BY type, name;", &schema_once,
            &err);

    for (int again = 0; again < 3; again++) {
        T_OK(atlas_db_migrate(db, &err), &err);
        T_EQ_INT(atlas_db_schema_version(db, &err), ATLAS_SCHEMA_VERSION);
    }
    atlas_buf schema_again = ATLAS_BUF_INIT;
    text_of(db, "SELECT type || ' ' || name FROM sqlite_master ORDER BY type, name;",
            &schema_again, &err);
    T_EQ_STR(atlas_buf_cstr(&schema_again), atlas_buf_cstr(&schema_once));
    /* And no leftover from the rebuild. */
    T_EQ_INT(count_of(db, "SELECT COUNT(*) FROM sqlite_master WHERE name LIKE '%_new' OR name = 'dc6';",
                      &err),
             0);

    atlas_buf_free(&schema_once);
    atlas_buf_free(&schema_again);
    atlas_db_close(db);
    atlas_buf_free(&path);
    fx_close(&fx);
}

static void test_a_failed_migration_leaves_the_database_as_it_was(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_OK(fx_open(&fx, &err), &err);
    atlas_buf path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&path, &err, "%s/atlas.db", fx_data_dir(&fx)), &err);
    build_schema6(atlas_buf_cstr(&path), &err);

    /* Plant an obstruction the migration must trip over: a table with the name
     * the rebuild uses for its temporary. Each migration runs inside its own
     * transaction and is rolled back whole, so the failure must leave a
     * complete, working schema-6 database rather than half of a seven. */
    atlas_db *db = NULL;
    T_OK(atlas_db_open(atlas_buf_cstr(&path), &db, &err), &err);
    T_OK(atlas_db_exec_sql(db, "CREATE TABLE decision_challenges_new(x);", &err), &err);
    atlas_buf before = ATLAS_BUF_INIT;
    text_of(db,
            "SELECT id || '|' || token || '|' || intent FROM decision_challenges ORDER BY id;",
            &before, &err);
    int64_t docs = count_of(db, "SELECT COUNT(*) FROM decision_documents;", &err);
    atlas_db_close(db);

    T_OK(atlas_db_open(atlas_buf_cstr(&path), &db, &err), &err);
    T_CHECK_MSG(atlas_db_migrate(db, &err) != ATLAS_OK,
                "the migration should have failed on the planted obstruction");

    /* Still six, still whole, still every row. */
    T_EQ_INT(atlas_db_schema_version(db, &err), 6);
    atlas_buf after = ATLAS_BUF_INIT;
    text_of(db,
            "SELECT id || '|' || token || '|' || intent FROM decision_challenges ORDER BY id;",
            &after, &err);
    T_EQ_STR(atlas_buf_cstr(&after), atlas_buf_cstr(&before));
    T_EQ_INT(count_of(db, "SELECT COUNT(*) FROM decision_documents;", &err), docs);
    T_EQ_INT(count_of(db, "SELECT COUNT(*) FROM sqlite_master WHERE name='decision_validations';",
                      &err),
             0);
    atlas_buf report = ATLAS_BUF_INIT;
    T_OK(atlas_db_integrity_check(db, &report, &err), &err);
    T_EQ_STR(atlas_buf_cstr(&report), "ok");

    atlas_buf_free(&report);
    atlas_buf_free(&before);
    atlas_buf_free(&after);
    atlas_db_close(db);
    atlas_buf_free(&path);
    fx_close(&fx);
}

/* --- A5 across the boundary ------------------------------------------------- */

static void run_cli(fixture *fx, const char *const *extra, size_t n, int want_code) {
    atlas_err err;
    atlas_err_init(&err);
    const char *argv[16];
    size_t k = 0;
    argv[k++] = "--data-dir";
    argv[k++] = fx_data_dir(fx);
    for (size_t i = 0; i < n; i++) {
        argv[k++] = extra[i];
    }
    atlas_buf out = ATLAS_BUF_INIT;
    atlas_buf eout = ATLAS_BUF_INIT;
    int code = -1;
    T_OK(fx_atlas(argv, k, &out, &eout, &code, &err), &err);
    T_CHECK_MSG(code == want_code, "%s exited %d (wanted %d)\n%s\n%s", extra[0], code, want_code,
                atlas_buf_cstr(&out), atlas_buf_cstr(&eout));
    atlas_buf_free(&out);
    atlas_buf_free(&eout);
}

static void test_a_schema_six_backup_still_verifies_without_being_migrated(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_OK(fx_open(&fx, &err), &err);
    atlas_buf path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&path, &err, "%s/atlas.db", fx_data_dir(&fx)), &err);
    build_schema6(atlas_buf_cstr(&path), &err);

    /* Copy it aside as a "backup taken before the upgrade". Verification must
     * open it read-only and leave it exactly as it was — A5's rule, and the one
     * a schema bump is most likely to break, because the obvious way to inspect
     * a database is `atlas_db_open`, which migrates. */
    atlas_buf backup = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&backup, &err, "%s/six.atlasbak", fx_data_dir(&fx)), &err);
    const char *create[] = {"backup", "create", atlas_buf_cstr(&backup)};
    run_cli(&fx, create, 3u, 0);

    /* The created backup is at head, because `backup create` copies the live
     * database — which this test has just left at six. Verify must accept it
     * and must not change it. */
    char digest_before[ATLAS_SHA256_HEX_LEN + 1u];
    char digest_after[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_buf dir = ATLAS_BUF_INIT;
    T_OK(atlas_buf_set_str(&dir, fx_data_dir(&fx), &err), &err);
    T_OK(fx_tree_digest(atlas_buf_cstr(&dir), digest_before, &err), &err);
    const char *verify[] = {"backup", "verify", atlas_buf_cstr(&backup)};
    run_cli(&fx, verify, 3u, 0);
    T_OK(fx_tree_digest(atlas_buf_cstr(&dir), digest_after, &err), &err);
    T_CHECK_MSG(strcmp(digest_before, digest_after) == 0,
                "backup verify modified something under the data directory");

    /* And the backup file itself is still at the version it was written at. */
    {
        atlas_db *ro = NULL;
        T_OK(atlas_db_open_readonly(atlas_buf_cstr(&backup), &ro, &err), &err);
        int v = atlas_db_schema_version(ro, &err);
        T_CHECK_MSG(v == 6, "verify migrated the backup: it is now schema %d", v);
        atlas_db_close(ro);
    }

    atlas_buf_free(&dir);
    atlas_buf_free(&backup);
    atlas_buf_free(&path);
    fx_close(&fx);
}

static void test_a_restored_schema_six_database_migrates_forward(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_OK(fx_open(&fx, &err), &err);
    atlas_buf path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&path, &err, "%s/atlas.db", fx_data_dir(&fx)), &err);
    build_schema6(atlas_buf_cstr(&path), &err);

    atlas_buf backup = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&backup, &err, "%s/six.atlasbak", fx_data_dir(&fx)), &err);
    const char *create[] = {"backup", "create", atlas_buf_cstr(&backup)};
    run_cli(&fx, create, 3u, 0);

    /* Move the live database on, then restore the older one over it. */
    {
        atlas_db *db = NULL;
        T_OK(atlas_db_open(atlas_buf_cstr(&path), &db, &err), &err);
        T_OK(atlas_db_migrate(db, &err), &err);
        T_EQ_INT(atlas_db_schema_version(db, &err), ATLAS_SCHEMA_VERSION);
        atlas_db_close(db);
    }
    const char *restore[] = {"backup", "restore", atlas_buf_cstr(&backup), "--yes"};
    run_cli(&fx, restore, 4u, 0);

    /* The ordinary supported path takes it forward, and the decision records
     * that were in the schema-6 backup are all there afterwards. */
    atlas_db *db = NULL;
    T_OK(atlas_db_open(atlas_buf_cstr(&path), &db, &err), &err);
    T_OK(atlas_db_migrate(db, &err), &err);
    T_EQ_INT(atlas_db_schema_version(db, &err), ATLAS_SCHEMA_VERSION);
    T_EQ_INT(count_of(db, "SELECT COUNT(*) FROM decision_documents;", &err), 3);
    T_EQ_INT(count_of(db, "SELECT COUNT(*) FROM decision_validations;", &err), 0);
    T_EQ_INT(count_of(db,
                      "SELECT COUNT(*) FROM decision_events e WHERE e.challenge_id IS NOT NULL"
                      "  AND NOT EXISTS (SELECT 1 FROM decision_challenges c"
                      "                   WHERE c.id = e.challenge_id);",
                      &err),
             0);
    atlas_db_close(db);

    atlas_buf_free(&backup);
    atlas_buf_free(&path);
    fx_close(&fx);
}

static const atlas_test TESTS[] = {
    {"a populated schema-six database reaches seven losslessly",
     test_a_populated_schema_six_database_reaches_seven_losslessly},
    {"migration seven is idempotent", test_migration_seven_is_idempotent},
    {"a failed migration leaves the database as it was",
     test_a_failed_migration_leaves_the_database_as_it_was},
    {"a schema-six backup still verifies without being migrated",
     test_a_schema_six_backup_still_verifies_without_being_migrated},
    {"a restored schema-six database migrates forward",
     test_a_restored_schema_six_database_migrates_forward},
};

ATLAS_TEST_MAIN("migrate7", TESTS)
