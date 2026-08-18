/* Atlas - A8: the schema 7 -> 8 migration, and what it must leave alone.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Required cases 46 (migration) and 47 (rollback).
 *
 * A migration that adds tables is the easy kind and is still the kind that
 * destroys a deployment, because the interesting property is not "did the new
 * tables appear" but "is everything that was already there byte-for-byte the
 * same afterwards". So the check is a digest of every pre-existing table taken
 * before and compared after, rather than a count.
 *
 * The rollback case injects a deliberately broken migration 8 and requires that
 * the database is left exactly as it was — no orchestration tables, no recorded
 * version, nothing half-created. `atlas_db_migrate_list` runs each migration in
 * its own transaction; this is the test that says so about A8's.
 */
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/db.h"
#include "atlas/decision_ops.h"
#include "atlas/orch_ops.h"
#include "atlas/sha256.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

/* The tables migration 8 creates. Named here rather than discovered, so that a
 * migration which quietly stopped creating one fails. */
static const char *const A8_TABLES[] = {
    "orch_jobs",   "orch_attempts",  "orch_leases",      "orch_transitions",
    "orch_events", "orch_artifacts", "orch_idempotency", "orch_observations",
    "orch_snapshots", "orch_snapshot_entries",
};

/* Everything a schema-7 database already holds, and which A8 must not touch. */
static const char *const PRESERVED_TABLES[] = {
    "schema_migrations",    "repositories",       "files",
    "commits",              "evidence",           "scans",
    "ai_sessions",          "ai_reasons",         "ai_decisions",
    "code_files",           "code_symbols",       "code_relations",
    "decision_documents",   "decision_revisions", "decision_events",
    "decision_challenges",  "decision_validations", "decision_links",
    "repo_index_state",     "repo_events",
};

static bool table_exists(atlas_db *db, const char *name) {
    atlas_err err;
    atlas_err_init(&err);
    sqlite3_stmt *s = NULL;
    T_REQUIRE(atlas_db_prepare(
                  db, "SELECT count(*) FROM sqlite_schema WHERE type='table' AND name=?1;", &s,
                  &err) == ATLAS_OK);
    (void)atlas_db_bind_text_opt(db, s, 1, name, &err);
    int64_t n = 0;
    if (sqlite3_step(s) == SQLITE_ROW) {
        n = sqlite3_column_int64(s, 0);
    }
    atlas_db_finish(db, s);
    return n > 0;
}

/* A digest over every column of every row, in rowid order, including column
 * names — so a reordered, renamed or retyped column is a difference too. */
static void table_digest(atlas_db *db, const char *table, char *out) {
    atlas_err err;
    atlas_err_init(&err);
    char sql[256];
    (void)snprintf(sql, sizeof sql, "SELECT * FROM %s ORDER BY rowid;", table);
    sqlite3_stmt *s = NULL;
    T_OK(atlas_db_prepare(db, sql, &s, &err), &err);
    atlas_sha256 ctx;
    atlas_sha256_init(&ctx);
    while (sqlite3_step(s) == SQLITE_ROW) {
        for (int c = 0; c < sqlite3_column_count(s); c++) {
            const char *name = sqlite3_column_name(s, c);
            atlas_sha256_update(&ctx, name != NULL ? name : "", name != NULL ? strlen(name) : 0u);
            if (sqlite3_column_type(s, c) == SQLITE_NULL) {
                atlas_sha256_update(&ctx, "\x00NULL", 5u);
            } else {
                const unsigned char *t = sqlite3_column_text(s, c);
                int n = sqlite3_column_bytes(s, c);
                atlas_sha256_update(&ctx, t != NULL ? (const char *)t : "", n > 0 ? (size_t)n : 0u);
            }
        }
        atlas_sha256_update(&ctx, "\x01", 1u);
    }
    atlas_db_finish(db, s);
    unsigned char d[ATLAS_SHA256_DIGEST_LEN];
    atlas_sha256_final(&ctx, d);
    atlas_hex_encode(d, sizeof d, out);
}

/* Builds a database at schema 7 with real content in the tables A8 must not
 * disturb, by migrating to the current schema and then winding the recorded
 * version back with A8's tables removed. That is exactly how `test_migrate7`
 * builds a schema-6 database, and for the same reason: there is no historical
 * Atlas binary to run. */
static void build_schema7(const char *path, atlas_err *err) {
    atlas_db *db = NULL;
    T_OK(atlas_db_open(path, &db, err), err);
    T_OK(atlas_db_migrate(db, err), err);

    atlas_repo_identity id;
    memset(&id, 0, sizeof id);
    id.root = "/tmp/atlas-migrate8-repo";
    id.root_len = strlen(id.root);
    id.common_dir = "/tmp/atlas-migrate8-repo/.git";
    id.common_dir_len = strlen(id.common_dir);
    id.git_dir = id.common_dir;
    id.git_dir_len = id.common_dir_len;
    id.object_format = "sha1";
    int64_t repo_id = 0;
    T_OK(atlas_db_repo_add(db, "proj", &id, &repo_id, err), err);

    /* One proposed decision, so the A4 tables are not empty: a migration that
     * dropped and rebuilt a table would show up as a digest change rather than
     * as a count that happened to stay zero. */
    atlas_decision_op op;
    atlas_decision_op_init(&op, ATLAS_DECISION_OP_PROPOSE);
    T_OK(atlas_buf_set_str(&op.repo_name, "proj", err), err);
    T_OK(atlas_buf_set_str(&op.revision.title, "keep the index rebuildable", err), err);
    T_OK(atlas_buf_set_str(&op.revision.decision_text, "SQLite is an index, never the record.",
                           err),
         err);
    T_OK(atlas_buf_set_str(&op.revision.rationale_text, "so it can always be rebuilt", err), err);
    op.revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
    atlas_decision_result r;
    atlas_decision_result_init(&r);
    T_OK(atlas_decision_apply(db, &op, &r, err), err);
    atlas_decision_result_free(&r);
    atlas_decision_op_free(&op);

    /* Reverse creation order, which is child-before-parent.
     *
     * With `PRAGMA foreign_keys=ON`, dropping `orch_jobs` first leaves every
     * child holding an unresolvable `REFERENCES orch_jobs(id)`, and the *next*
     * drop fails with "no such table: main.orch_jobs" — an error about the table
     * that was already removed, which reads as though the drop had not worked.
     * The order is the fix, not disabling the pragma: a simulation that turned
     * foreign keys off would be winding the schema back under different rules
     * from the ones it runs under. */
    atlas_buf drop = ATLAS_BUF_INIT;
    for (size_t i = sizeof A8_TABLES / sizeof A8_TABLES[0]; i > 0; i--) {
        T_OK(atlas_buf_appendf(&drop, err, "DROP TABLE %s;", A8_TABLES[i - 1u]), err);
    }
    /* Migration 10's table too: winding back past 8 winds back past 10, and a
     * rewind that leaves a later migration's table behind is not a schema-7
     * database. */
    /* A11.0's table, and migration 10's below it: winding back past 8 winds
     * back past both, and a rewind that leaves a later migration's table behind
     * is not a schema-7 database. */
    T_OK(atlas_buf_append_str(&drop, "DROP TABLE orch_runs;", err), err);
    T_OK(atlas_buf_append_str(&drop,
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
                              "DROP TABLE sem_generations;",
                              err),
         err);
    T_OK(atlas_buf_append_str(&drop, "DROP TABLE decision_edge_events;", err), err);
    T_OK(atlas_buf_append_str(&drop, "DELETE FROM schema_migrations WHERE version >= 8;", err),
         err);
    T_OK(atlas_db_exec_sql(db, atlas_buf_cstr(&drop), err), err);
    atlas_buf_free(&drop);
    atlas_db_close(db);
}

static void test_a_schema_seven_database_reaches_eight_losslessly(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_OK(fx_open(&fx, &err), &err);
    atlas_buf path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&path, &err, "%s/atlas.db", fx_data_dir(&fx)), &err);
    build_schema7(atlas_buf_cstr(&path), &err);

    atlas_db *db = NULL;
    T_OK(atlas_db_open(atlas_buf_cstr(&path), &db, &err), &err);
    T_EQ_INT(atlas_db_schema_version(db, &err), 7);
    for (size_t i = 0; i < sizeof A8_TABLES / sizeof A8_TABLES[0]; i++) {
        T_CHECK_MSG(!table_exists(db, A8_TABLES[i]), "%s exists before migration 8",
                    A8_TABLES[i]);
    }

    char before[sizeof PRESERVED_TABLES / sizeof PRESERVED_TABLES[0]][ATLAS_SHA256_HEX_LEN + 1u];
    for (size_t i = 0; i < sizeof PRESERVED_TABLES / sizeof PRESERVED_TABLES[0]; i++) {
        table_digest(db, PRESERVED_TABLES[i], before[i]);
    }

    T_OK(atlas_db_migrate(db, &err), &err);
    T_EQ_INT(atlas_db_schema_version(db, &err), ATLAS_SCHEMA_VERSION);
    T_EQ_INT(ATLAS_SCHEMA_VERSION, 21);

    for (size_t i = 0; i < sizeof A8_TABLES / sizeof A8_TABLES[0]; i++) {
        T_CHECK_MSG(table_exists(db, A8_TABLES[i]), "migration 8 did not create %s",
                    A8_TABLES[i]);
    }
    /* Row for row, column for column. This is the property that matters: an
     * upgrade must not disturb a single existing byte, and a count would not
     * notice a rebuilt table that happened to end up the same size. */
    for (size_t i = 0; i < sizeof PRESERVED_TABLES / sizeof PRESERVED_TABLES[0]; i++) {
        char after[ATLAS_SHA256_HEX_LEN + 1u];
        table_digest(db, PRESERVED_TABLES[i], after);
        /* schema_migrations legitimately gains its own row 8. */
        if (strcmp(PRESERVED_TABLES[i], "schema_migrations") == 0) {
            continue;
        }
        T_CHECK_MSG(strcmp(before[i], after) == 0, "migration 8 changed %s",
                    PRESERVED_TABLES[i]);
    }

    /* No real job is created during migration. An upgrade that seeded a row
     * would put work in a queue nobody submitted. */
    for (size_t i = 0; i < sizeof A8_TABLES / sizeof A8_TABLES[0]; i++) {
        char sql[128];
        (void)snprintf(sql, sizeof sql, "SELECT count(*) FROM %s;", A8_TABLES[i]);
        sqlite3_stmt *s = NULL;
        T_OK(atlas_db_prepare(db, sql, &s, &err), &err);
        int64_t n = -1;
        if (sqlite3_step(s) == SQLITE_ROW) {
            n = sqlite3_column_int64(s, 0);
        }
        atlas_db_finish(db, s);
        T_CHECK_MSG(n == 0, "migration 8 seeded %lld rows into %s", (long long)n, A8_TABLES[i]);
    }

    /* Idempotent as a set: running it again is a no-op. */
    T_OK(atlas_db_migrate(db, &err), &err);
    T_EQ_INT(atlas_db_schema_version(db, &err), ATLAS_SCHEMA_VERSION);

    atlas_db_close(db);
    atlas_buf_free(&path);
    fx_close(&fx);
}

static void test_a_failed_migration_eight_leaves_seven_untouched(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_OK(fx_open(&fx, &err), &err);
    atlas_buf path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&path, &err, "%s/atlas.db", fx_data_dir(&fx)), &err);
    build_schema7(atlas_buf_cstr(&path), &err);

    atlas_db *db = NULL;
    T_OK(atlas_db_open(atlas_buf_cstr(&path), &db, &err), &err);
    T_EQ_INT(atlas_db_schema_version(db, &err), 7);

    char before[sizeof PRESERVED_TABLES / sizeof PRESERVED_TABLES[0]][ATLAS_SHA256_HEX_LEN + 1u];
    for (size_t i = 0; i < sizeof PRESERVED_TABLES / sizeof PRESERVED_TABLES[0]; i++) {
        table_digest(db, PRESERVED_TABLES[i], before[i]);
    }

    /* A migration 8 that creates its first table and then fails. If the
     * migration were not one transaction, `orch_jobs` would survive — and the
     * next real upgrade would then fail for ever with "table already exists",
     * which is precisely the state a half-applied migration leaves an operator
     * in. */
    static const char *const BROKEN[] = {
        "CREATE TABLE orch_jobs(id INTEGER PRIMARY KEY);",
        "CREATE TABLE this is not valid sql;",
        NULL,
    };
    size_t base_count = 0;
    const atlas_migration *base = atlas_migrations(&base_count);
    T_REQUIRE(base_count >= 8u);
    /* Sized from the schema version rather than pinned at a literal, so a new
     * migration does not silently overflow it. A9.2.2 found this at 16 with 17
     * migrations to copy: the `T_REQUIRE` caught it, which is what it is for,
     * but a bound that has to be edited by hand every season is one that will
     * eventually be edited to whatever makes the test pass. */
    atlas_migration list[ATLAS_SCHEMA_VERSION];
    T_REQUIRE(base_count <= sizeof list / sizeof list[0]);
    memcpy(list, base, base_count * sizeof list[0]);
    /* Replace the real migration 8 with the broken one. */
    list[7].statements = BROKEN;

    T_FAILS_WITH(atlas_db_migrate_list(db, list, base_count, &err), ATLAS_ERR_DB, &err);
    T_CHECK_MSG(strstr(atlas_err_msg(&err), "rolled back") != NULL,
                "the error should say the migration was rolled back, got: %s",
                atlas_err_msg(&err));

    /* Nothing half-created, and the version did not advance. */
    T_EQ_INT(atlas_db_schema_version(db, &err), 7);
    for (size_t i = 0; i < sizeof A8_TABLES / sizeof A8_TABLES[0]; i++) {
        T_CHECK_MSG(!table_exists(db, A8_TABLES[i]), "%s survived a failed migration 8",
                    A8_TABLES[i]);
    }
    for (size_t i = 0; i < sizeof PRESERVED_TABLES / sizeof PRESERVED_TABLES[0]; i++) {
        char after[ATLAS_SHA256_HEX_LEN + 1u];
        table_digest(db, PRESERVED_TABLES[i], after);
        T_CHECK_MSG(strcmp(before[i], after) == 0, "a failed migration 8 changed %s",
                    PRESERVED_TABLES[i]);
    }

    /* And the real migration still applies cleanly afterwards, which is what
     * makes the rollback a recoverable state rather than a wedged one. */
    T_OK(atlas_db_migrate(db, &err), &err);
    T_EQ_INT(atlas_db_schema_version(db, &err), ATLAS_SCHEMA_VERSION);

    atlas_db_close(db);
    atlas_buf_free(&path);
    fx_close(&fx);
}

static void test_a_reader_refuses_an_unsupported_future_schema(void) {
    /* A read-only handle must never be the thing that upgrades the schema, and
     * a *future* schema is one this binary cannot interpret at all. Both
     * directions are refused rather than guessed at. */
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_OK(fx_open(&fx, &err), &err);
    atlas_buf path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&path, &err, "%s/atlas.db", fx_data_dir(&fx)), &err);
    {
        atlas_db *db = NULL;
        T_OK(atlas_db_open(atlas_buf_cstr(&path), &db, &err), &err);
        T_OK(atlas_db_migrate(db, &err), &err);
        /* Claim a version from the future. */
        T_OK(atlas_db_exec_sql(
                 db,
                 "INSERT INTO schema_migrations(version, name, applied_at)"
                 " VALUES(99, 'from the future', '2099-01-01T00:00:00Z');",
                 &err),
             &err);
        atlas_db_close(db);
    }
    atlas_db *ro = NULL;
    T_OK(atlas_db_open_readonly(atlas_buf_cstr(&path), &ro, &err), &err);
    atlas_err e2;
    atlas_err_init(&e2);
    T_CHECK_MSG(atlas_db_migrate(ro, &e2) != ATLAS_OK,
                "a read-only handle accepted a schema it cannot speak");
    atlas_db_close(ro);
    atlas_buf_free(&path);
    fx_close(&fx);
}

static const atlas_test TESTS[] = {
    {"a schema-seven database reaches eight losslessly",
     test_a_schema_seven_database_reaches_eight_losslessly},
    {"a failed migration eight leaves seven untouched",
     test_a_failed_migration_eight_leaves_seven_untouched},
    {"a reader refuses an unsupported future schema",
     test_a_reader_refuses_an_unsupported_future_schema},
};

ATLAS_TEST_MAIN("migrate8", TESTS)
