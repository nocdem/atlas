/* Atlas - migration 10: durable evidence about a decision-to-decision edge.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Migration 9 made `relates_to` expressible and left it inexplicable: an edge
 * could be drawn and the reason it was drawn had nowhere to live. Migration 10
 * adds the table that holds it.
 *
 * The design constraint that shapes this suite is that the reason lives
 * **outside** the revision. A revision is immutable and its links are covered by
 * the canonical content hash, so a rationale stored inside one would either
 * change every already-approved digest or force a fresh approval for every
 * document that ever gained an edge. So the migration must be purely additive,
 * and the assertion that matters most is the one at the end of the first case:
 * **no stored content hash moves.** A migration that silently changed one would
 * make `atlas doctor` report every approved decision as tampered with, which is
 * indistinguishable from the corruption it exists to detect.
 *
 * It also covers the refusal that had been missing from the writable path
 * entirely: a database from a *newer* Atlas fell straight through the migration
 * loop and was reported as migrated, because the loop only ever adds. An older
 * binary writing into a schema it does not understand is how a rebuildable index
 * stops being rebuildable. */
#include <stdlib.h>
#include <string.h>

#include "atlas/datadir.h"
#include <sqlite3.h>

#include <stdio.h>

#include "atlas/db.h"
#include "atlas/decision_ops.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

static int schema_of(atlas_db *db) {
    atlas_err err;
    atlas_err_init(&err);
    return atlas_db_schema_version(db, &err);
}

static bool table_exists(atlas_db *db, const char *name) {
    atlas_err err;
    atlas_err_init(&err);
    sqlite3_stmt *q = NULL;
    if (atlas_db_prepare(db, "SELECT 1 FROM sqlite_schema WHERE type='table' AND name = ?1;", &q,
                         &err) != ATLAS_OK) {
        return false;
    }
    (void)sqlite3_bind_text(q, 1, name, -1, SQLITE_TRANSIENT);
    bool found = sqlite3_step(q) == SQLITE_ROW;
    atlas_db_finish(db, q);
    return found;
}

static void exec(atlas_db *db, const char *sql) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_db_exec_sql(db, sql, &err), &err);
}

/* Every stored content hash, in document order. The one thing this migration
 * must not touch. */
static atlas_status hashes(atlas_db *db, atlas_buf *out, atlas_err *err) {
    atlas_buf_reset(out);
    sqlite3_stmt *q = NULL;
    atlas_status st = atlas_db_prepare(
        db, "SELECT content_hash FROM decision_revisions ORDER BY id;", &q, err);
    if (st != ATLAS_OK) {
        return st;
    }
    while (st == ATLAS_OK && sqlite3_step(q) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(q, 0);
        st = atlas_buf_appendf(out, err, "%s\n", v != NULL ? v : "");
    }
    atlas_db_finish(db, q);
    return st;
}

/* --- 1. a schema-nine database reaches ten, additively -------------------- */

static void test_a_schema_nine_database_reaches_ten_additively(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_OK(fx_open(&fx, &err), &err);
    atlas_buf path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&path, &err, "%s/atlas.db", fx_data_dir(&fx)), &err);
    T_OK(atlas_datadir_ensure(fx_data_dir(&fx), &err), &err);

    atlas_db *db = NULL;
    T_OK(atlas_db_open(atlas_buf_cstr(&path), &db, &err), &err);
    T_OK(atlas_db_migrate(db, &err), &err);
    T_EQ_INT(schema_of(db), ATLAS_SCHEMA_VERSION);

    /* A real decision, so the "nothing else moved" assertions are about rows
     * rather than about two empty tables agreeing. */
    atlas_repo_identity id;
    memset(&id, 0, sizeof id);
    id.root = "/tmp/atlas-migrate10-repo";
    id.root_len = strlen(id.root);
    id.common_dir = "/tmp/atlas-migrate10-repo/.git";
    id.common_dir_len = strlen(id.common_dir);
    id.git_dir = id.common_dir;
    id.git_dir_len = id.common_dir_len;
    id.object_format = "sha1";
    int64_t repo_id = 0;
    T_OK(atlas_db_repo_add(db, "proj", &id, &repo_id, &err), &err);

    atlas_decision_op op;
    atlas_decision_op_init(&op, ATLAS_DECISION_OP_PROPOSE);
    T_OK(atlas_buf_set_str(&op.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set_str(&op.revision.title, "keep the index rebuildable", &err), &err);
    T_OK(atlas_buf_set_str(&op.revision.decision_text, "SQLite is an index, never the record.",
                           &err),
         &err);
    op.revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_OK(atlas_decision_apply(db, &op, &res, &err), &err);
    atlas_decision_result_free(&res);
    atlas_decision_op_free(&op);

    atlas_buf before = ATLAS_BUF_INIT;
    T_OK(hashes(db, &before, &err), &err);
    T_REQUIRE_MSG(before.len > 0, "the fixture wrote no revision");

    /* Wind back to nine exactly as an upgrade would find it, then forward. */
    exec(db, "DROP TABLE verify_lifecycle_audit;"
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
             /* A9.2.3's row goes with the rest of the semantic layer: a rewind
              * that leaves a later migration's table behind is not a database at
              * the version it claims. */
             "DROP TABLE sem_repo_config;"
             "DROP TABLE sem_includes;"
             "DROP TABLE sem_edges;"
             "DROP TABLE sem_symbols;"
             "DROP TABLE sem_units;"
             "DROP TABLE sem_compdbs;"
             "DROP TABLE sem_current;"
             "DROP TABLE sem_generations;"
             "DROP TABLE decision_edge_events;"
             "DELETE FROM schema_migrations WHERE version >= 10;");
    T_EQ_INT(schema_of(db), 9);
    T_CHECK(!table_exists(db, "decision_edge_events"));

    T_OK(atlas_db_migrate(db, &err), &err);
    T_EQ_INT(schema_of(db), ATLAS_SCHEMA_VERSION);
    T_CHECK_MSG(table_exists(db, "decision_edge_events"), "migration 10 created nothing");

    /* The assertion the whole design rests on. */
    atlas_buf after = ATLAS_BUF_INIT;
    T_OK(hashes(db, &after, &err), &err);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&before), atlas_buf_cstr(&after)) == 0,
                "migration 10 moved a stored content hash:\nbefore:\n%s\nafter:\n%s",
                atlas_buf_cstr(&before), atlas_buf_cstr(&after));

    /* And it is idempotent as a set: migrating an up-to-date database is a
     * no-op rather than a second CREATE. */
    T_OK(atlas_db_migrate(db, &err), &err);
    T_EQ_INT(schema_of(db), ATLAS_SCHEMA_VERSION);

    atlas_buf_free(&before);
    atlas_buf_free(&after);
    atlas_db_close(db);
    atlas_buf_free(&path);
    fx_close(&fx);
}

/* --- 2. a failure rolls the whole migration back -------------------------- */

/* `atlas_db_migrate` wraps each migration in one transaction, and this is what
 * that buys: a migration that cannot complete leaves the schema version and
 * every row exactly as they were, rather than half a table and a version that
 * claims the work was done. The failure is induced the way a real one would
 * arrive — the object already exists — by winding the version back and leaving
 * the table in place. */
static void test_a_failed_migration_ten_leaves_nine_untouched(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_OK(fx_open(&fx, &err), &err);
    atlas_buf path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&path, &err, "%s/atlas.db", fx_data_dir(&fx)), &err);
    T_OK(atlas_datadir_ensure(fx_data_dir(&fx), &err), &err);

    atlas_db *db = NULL;
    T_OK(atlas_db_open(atlas_buf_cstr(&path), &db, &err), &err);
    T_OK(atlas_db_migrate(db, &err), &err);

    /* Version back to nine, table left behind: migration 10 must now fail on
     * its first statement. */
    exec(db, "DELETE FROM schema_migrations WHERE version >= 10;");
    T_EQ_INT(schema_of(db), 9);

    atlas_err merr;
    atlas_err_init(&merr);
    atlas_status st = atlas_db_migrate(db, &merr);
    T_CHECK_MSG(st != ATLAS_OK, "migration 10 succeeded against an existing table");
    T_CHECK_MSG(strstr(atlas_err_msg(&merr), "rolled back") != NULL,
                "the failure did not report a rollback: %s", atlas_err_msg(&merr));
    /* Rolled back whole: still nine, and the table that caused the failure is
     * still exactly the one that was there. */
    T_EQ_INT(schema_of(db), 9);
    T_CHECK(table_exists(db, "decision_edge_events"));
    T_CHECK_MSG(table_exists(db, "decision_documents"), "an unrelated table was lost");

    atlas_db_close(db);
    atlas_buf_free(&path);
    fx_close(&fx);
}

/* --- 3. a database from the future is refused ----------------------------- */

/* The gap this closes: the migration loop only ever *adds*, so a database at a
 * version this build has never heard of fell through it untouched and was
 * reported as migrated. The read-only path already refused; the writable one —
 * the path that can do the damage — did not. */
static void test_a_future_schema_is_refused_on_the_writable_path(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_OK(fx_open(&fx, &err), &err);
    atlas_buf path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&path, &err, "%s/atlas.db", fx_data_dir(&fx)), &err);
    T_OK(atlas_datadir_ensure(fx_data_dir(&fx), &err), &err);

    atlas_db *db = NULL;
    T_OK(atlas_db_open(atlas_buf_cstr(&path), &db, &err), &err);
    T_OK(atlas_db_migrate(db, &err), &err);

    /* A version from a build that does not exist yet.
     *
     * This number has to stay ahead of `ATLAS_SCHEMA_VERSION`: the test is
     * about a database an *older* binary must refuse, so the moment the real
     * schema catches up, a fixed number stops simulating the future and starts
     * colliding with a migration that genuinely applied — which is exactly what
     * happened when A9.2 added migration 14. Derived from
     * `ATLAS_SCHEMA_VERSION` instead, so it is always one ahead and the collision
     * cannot recur. */
    char future[128];
    (void)snprintf(future, sizeof future,
                   "INSERT INTO schema_migrations(version, name, applied_at)"
                   " VALUES(%d, 'from the future', '2030-01-01T00:00:00Z');",
                   ATLAS_SCHEMA_VERSION + 1);
    exec(db, future);
    T_EQ_INT(schema_of(db), ATLAS_SCHEMA_VERSION + 1);

    atlas_err ferr;
    atlas_err_init(&ferr);
    T_CHECK_MSG(atlas_db_migrate(db, &ferr) != ATLAS_OK,
                "a schema from the future was accepted on the writable path");
    T_CHECK_MSG(strstr(atlas_err_msg(&ferr), "newer Atlas") != NULL,
                "the refusal does not say why: %s", atlas_err_msg(&ferr));
    /* Refused, not rewritten. The row is still there. */
    T_EQ_INT(schema_of(db), ATLAS_SCHEMA_VERSION + 1);

    atlas_db_close(db);
    atlas_buf_free(&path);
    fx_close(&fx);
}

/* --- 4. the shape of what was added --------------------------------------- */

/* The table is append-only by construction and by discipline: the vocabularies
 * are CHECK constraints so a row outside them cannot be written at all, and
 * `db_gate.c`'s rule applies here — there is no UPDATE and no DELETE anywhere
 * that touches it. The first half is asserted here; the second is asserted by
 * reading the source, because an absence cannot be observed at runtime. */
static void test_the_edge_ledger_is_a_closed_vocabulary(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_OK(fx_open(&fx, &err), &err);
    atlas_buf path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&path, &err, "%s/atlas.db", fx_data_dir(&fx)), &err);
    T_OK(atlas_datadir_ensure(fx_data_dir(&fx), &err), &err);

    atlas_db *db = NULL;
    T_OK(atlas_db_open(atlas_buf_cstr(&path), &db, &err), &err);
    T_OK(atlas_db_migrate(db, &err), &err);

    atlas_buf ddl = ATLAS_BUF_INIT;
    sqlite3_stmt *q = NULL;
    T_OK(atlas_db_prepare(db, "SELECT COALESCE(sql,'') FROM sqlite_schema WHERE name = ?1;", &q,
                          &err),
         &err);
    (void)sqlite3_bind_text(q, 1, "decision_edge_events", -1, SQLITE_TRANSIENT);
    if (sqlite3_step(q) == SQLITE_ROW) {
        T_OK(atlas_buf_set_str(&ddl, (const char *)sqlite3_column_text(q, 0), &err), &err);
    }
    atlas_db_finish(db, q);
    const char *sql = atlas_buf_cstr(&ddl);

    /* AUTOINCREMENT, because ordering is the id and a reused rowid would make
     * an older event sort after a newer one. */
    T_CHECK_MSG(strstr(sql, "AUTOINCREMENT") != NULL, "the ledger's id is reusable:\n%s", sql);
    T_CHECK_MSG(strstr(sql, "'ADDED'") != NULL && strstr(sql, "'ANNOTATED'") != NULL &&
                    strstr(sql, "'REMOVED'") != NULL,
                "the event vocabulary is not closed:\n%s", sql);
    T_CHECK_MSG(strstr(sql, "'OPERATOR'") != NULL && strstr(sql, "'UNKNOWN'") != NULL,
                "the provenance vocabulary is not closed:\n%s", sql);
    /* Both endpoints are real foreign keys. A decision record is never deleted,
     * so unlike A4's cross-model pointers these cannot outlive their row. */
    T_CHECK_MSG(strstr(sql, "REFERENCES decision_documents(id)") != NULL,
                "an endpoint is not a foreign key:\n%s", sql);

    /* An event outside the vocabulary is refused by the database itself, not
     * merely by the code above it. */
    atlas_err verr;
    atlas_err_init(&verr);
    T_CHECK_MSG(atlas_db_exec_sql(db,
                                  "INSERT INTO decision_edge_events"
                                  "(source_document_id, target_document_id, kind, event, note,"
                                  " provenance, revision_id, created_at)"
                                  " VALUES(1, 2, 'relates_to', 'INVENTED', 'x', 'OPERATOR', 0,"
                                  " '2026-01-01T00:00:00Z');",
                                  &verr) != ATLAS_OK,
                "the schema accepted an event outside its vocabulary");

    atlas_buf_free(&ddl);
    atlas_db_close(db);
    atlas_buf_free(&path);
    fx_close(&fx);
}

static const atlas_test TESTS[] = {
    {"a schema-nine database reaches ten without moving a content hash",
     test_a_schema_nine_database_reaches_ten_additively},
    {"a failed migration ten leaves nine untouched",
     test_a_failed_migration_ten_leaves_nine_untouched},
    {"a database from a newer Atlas is refused on the writable path",
     test_a_future_schema_is_refused_on_the_writable_path},
    {"the edge ledger's vocabularies are closed by the schema",
     test_the_edge_ledger_is_a_closed_vocabulary},
};

ATLAS_TEST_MAIN("migrate10", TESTS)
