/* Atlas - A14 T2: migration 32 -- which credential queued a job, beside which uid.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Two ALTER TABLE statements and one index, on `orch_jobs` and
 * `orch_transitions`: `submit_key_id` appended to jobs (DEFAULT '' -- true of
 * every pre-existing row, since the only submission path was local), the index
 * that budgets-per-key reads will use, and `key_id` appended to transitions.
 * This suite proves:
 *
 *   - a fresh database reaches schema 32 with both new columns and the new
 *     index;
 *   - a database stopped at 31 with three pre-existing jobs and their
 *     transitions reaches 32 with every pre-existing column of every
 *     pre-existing row byte-identical, every job reading `submit_key_id = ''`
 *     and every transition reading `key_id = ''`;
 *   - `idx_orch_jobs_submit_key` exists by name after migration;
 *   - `pragma_foreign_key_check` is empty throughout.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "atlas/datadir.h"
#include "atlas/db.h"
#include "atlas/sha256.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

static int schema_of(atlas_db *db) {
    atlas_err err;
    atlas_err_init(&err);
    return atlas_db_schema_version(db, &err);
}

static int64_t count_sql(atlas_db *db, const char *sql) {
    atlas_err err;
    atlas_err_init(&err);
    int64_t v = -1;
    T_OK(atlas_db_query_int64(db, sql, &v, &err), &err);
    return v;
}

static bool column_exists(atlas_db *db, const char *table, const char *column) {
    atlas_err err;
    atlas_err_init(&err);
    sqlite3_stmt *q = NULL;
    if (atlas_db_prepare(db, "SELECT 1 FROM pragma_table_info(?1) WHERE name = ?2;", &q, &err) !=
        ATLAS_OK) {
        return false;
    }
    (void)sqlite3_bind_text(q, 1, table, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(q, 2, column, -1, SQLITE_TRANSIENT);
    bool found = sqlite3_step(q) == SQLITE_ROW;
    atlas_db_finish(db, q);
    return found;
}

static bool object_exists(atlas_db *db, const char *type, const char *name) {
    atlas_err err;
    atlas_err_init(&err);
    sqlite3_stmt *q = NULL;
    if (atlas_db_prepare(db, "SELECT 1 FROM sqlite_schema WHERE type = ?1 AND name = ?2;", &q,
                         &err) != ATLAS_OK) {
        return false;
    }
    (void)sqlite3_bind_text(q, 1, type, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(q, 2, name, -1, SQLITE_TRANSIENT);
    bool found = sqlite3_step(q) == SQLITE_ROW;
    atlas_db_finish(db, q);
    return found;
}

static atlas_status open_fresh(fixture *fx, atlas_db **out, atlas_err *err) {
    atlas_buf path = ATLAS_BUF_INIT;
    atlas_status st = atlas_datadir_ensure(fx_data_dir(fx), err);
    if (st == ATLAS_OK) {
        st = atlas_datadir_db_path(fx_data_dir(fx), &path, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_open(atlas_buf_cstr(&path), out, err);
    }
    atlas_buf_free(&path);
    return st;
}

/* The pre-32 column set for each table, in declaration order. Named explicitly
 * rather than `SELECT *`, on `test_migrate31.c`'s `EVENTS_V30_COLS` pattern:
 * migration 32 appends columns with DEFAULT '', which changes nothing about a
 * pre-existing row, and a `SELECT *` digest taken before and after would
 * report that addition as a difference it is not. */
#define JOBS_V31_COLS                                                                              \
    "id, job_uid, spec_version, spec_digest, submitter_uid, repo_id, repo_name,"                  \
    " repo_identity_hash, source_commit, mode, driver, task_text, allowed_paths, validations,"     \
    " wall_timeout_ms, idle_timeout_ms, max_attempts, max_output_bytes, max_artifact_bytes,"       \
    " max_artifact_count, correlation, parent_job_uid, idempotency_key, state, attempts_started,"  \
    " cancel_requested, state_seq, created_at, created_ms, deadline_ms, terminal_at,"              \
    " run_uid, run_slot"
#define TRANSITIONS_V31_COLS                                                                       \
    "id, job_id, attempt_id, from_state, to_state, reason, actor, actor_uid, detail, at"

/* A digest over an explicit column list, in rowid order, including column
 * names -- so a reordered, renamed or retyped column is a difference too.
 * `test_migrate29.c`'s `table_digest`, parameterised on the column list. */
static void table_digest_cols(atlas_db *db, const char *table, const char *cols, char *out) {
    atlas_err err;
    atlas_err_init(&err);
    char sql[512];
    (void)snprintf(sql, sizeof sql, "SELECT %s FROM %s ORDER BY rowid;", cols, table);
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

/* Opens a fresh database and stops it at schema 31, one migration short of
 * the one under test -- `test_migrate31.c`'s shape for `atlas_db_migrate_list`. */
static void open_at_schema_31(fixture *fx, atlas_db **db_out) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(open_fresh(fx, db_out, &err), &err);
    size_t count = 0;
    const atlas_migration *all = atlas_migrations(&count);
    T_REQUIRE(count >= 31u);
    T_OK(atlas_db_migrate_list(*db_out, all, 31u, &err), &err);
    T_EQ_INT(schema_of(*db_out), 31);
}

/* Three jobs and three transitions written with plain SQL against the
 * schema-31 shape. The db layer functions (op_submit, record_transition) are
 * not used: `record_transition`'s new signature requires `key_id` and would
 * not compile against the pre-32 schema, and the point is to prove migration
 * 32 preserves rows it did not touch.
 *
 * `repo_id` is nullable and omitted; the foreign-key check passes because
 * there is no row-level constraint on a NULL reference to `repositories`. */
static void seed_v31_rows(atlas_db *db) {
    atlas_err err;
    atlas_err_init(&err);

    static const char INSERT_JOB[] =
        "INSERT INTO orch_jobs"
        "  (job_uid, spec_version, spec_digest, submitter_uid, repo_name, repo_identity_hash,"
        "   source_commit, mode, driver, task_text, allowed_paths, validations,"
        "   wall_timeout_ms, idle_timeout_ms, max_attempts, max_output_bytes,"
        "   max_artifact_bytes, max_artifact_count, state, created_at, created_ms, deadline_ms)"
        "  VALUES (?1, 1, 'digest00000000000000000000000000', 1000, 'testrepo',"
        "          'identityhash0000000000000000000000000000000000000000000000000000',"
        "          'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa', 'direct', 'fake',"
        "          'task text', '', '', 60000, 30000, 3, 1048576, 1048576, 16,"
        "          'QUEUED', '2026-09-04T00:00:00Z', 0, 86400000);";

    sqlite3_stmt *st = NULL;
    T_OK(atlas_db_prepare(db, INSERT_JOB, &st, &err), &err);

    (void)sqlite3_bind_text(st, 1, "jm32testjob0000000000000000000000a", -1, SQLITE_TRANSIENT);
    T_OK(atlas_db_step_done(db, st, &err), &err);
    st = NULL;
    T_OK(atlas_db_prepare(db, INSERT_JOB, &st, &err), &err);
    (void)sqlite3_bind_text(st, 1, "jm32testjob0000000000000000000000b", -1, SQLITE_TRANSIENT);
    T_OK(atlas_db_step_done(db, st, &err), &err);
    st = NULL;
    T_OK(atlas_db_prepare(db, INSERT_JOB, &st, &err), &err);
    (void)sqlite3_bind_text(st, 1, "jm32testjob0000000000000000000000c", -1, SQLITE_TRANSIENT);
    T_OK(atlas_db_step_done(db, st, &err), &err);

    T_EQ_INT((int)count_sql(db, "SELECT COUNT(*) FROM orch_jobs;"), 3);

    /* One transition per job: UNKNOWN → QUEUED, CLIENT actor, matching what
     * op_submit's direct call to record_transition produces. */
    static const char INSERT_TR[] =
        "INSERT INTO orch_transitions"
        "  (job_id, from_state, to_state, reason, actor, at)"
        "  VALUES (?1, 'UNKNOWN', 'QUEUED', 'SUBMITTED', 'CLIENT', '2026-09-04T00:00:00Z');";

    for (int i = 1; i <= 3; i++) {
        st = NULL;
        T_OK(atlas_db_prepare(db, INSERT_TR, &st, &err), &err);
        (void)sqlite3_bind_int(st, 1, i);
        T_OK(atlas_db_step_done(db, st, &err), &err);
    }

    T_EQ_INT((int)count_sql(db, "SELECT COUNT(*) FROM orch_transitions;"), 3);
}

/* --- 1: a fresh database reaches 32 --------------------------------------- */

static void test_fresh_database_reaches_32(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    atlas_db *db = NULL;
    T_OK(open_fresh(&fx, &db, &err), &err);
    T_OK(atlas_db_migrate(db, &err), &err);

    T_EQ_INT((int)ATLAS_SCHEMA_VERSION, 32);
    T_EQ_INT(schema_of(db), 32);

    T_CHECK(column_exists(db, "orch_jobs", "submit_key_id"));
    T_CHECK(column_exists(db, "orch_transitions", "key_id"));

    T_CHECK(object_exists(db, "index", "idx_orch_jobs_submit_key"));

    T_EQ_INT((int)count_sql(db, "SELECT COUNT(*) FROM pragma_foreign_key_check;"), 0);

    atlas_db_close(db);
    fx_close(&fx);
}

/* --- 2: a database stopped at 31 reaches 32 losslessly ------------------- */

static void test_stopped_at_31_reaches_32_losslessly(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    atlas_db *db = NULL;
    open_at_schema_31(&fx, &db);

    seed_v31_rows(db);

    char jobs_before[ATLAS_SHA256_HEX_LEN + 1u];
    char transitions_before[ATLAS_SHA256_HEX_LEN + 1u];
    table_digest_cols(db, "orch_jobs", JOBS_V31_COLS, jobs_before);
    table_digest_cols(db, "orch_transitions", TRANSITIONS_V31_COLS, transitions_before);

    size_t count = 0;
    const atlas_migration *all = atlas_migrations(&count);
    T_REQUIRE(count >= 32u);
    T_OK(atlas_db_migrate_list(db, all, 32u, &err), &err);
    T_EQ_INT(schema_of(db), 32);

    /* Every column of every pre-existing row, byte-identical. */
    char jobs_after[ATLAS_SHA256_HEX_LEN + 1u];
    char transitions_after[ATLAS_SHA256_HEX_LEN + 1u];
    table_digest_cols(db, "orch_jobs", JOBS_V31_COLS, jobs_after);
    table_digest_cols(db, "orch_transitions", TRANSITIONS_V31_COLS, transitions_after);
    T_CHECK_MSG(strcmp(jobs_before, jobs_after) == 0,
                "migration 32 rewrote an orch_jobs row:\nbefore: %s\nafter:  %s",
                jobs_before, jobs_after);
    T_CHECK_MSG(strcmp(transitions_before, transitions_after) == 0,
                "migration 32 rewrote an orch_transitions row:\nbefore: %s\nafter:  %s",
                transitions_before, transitions_after);

    /* Every migrated job reads submit_key_id = ''; every transition key_id = ''.
     * DEFAULT '' is a true statement about every pre-existing row: the only
     * submission path before migration 32 was local and the column did not exist. */
    T_EQ_INT((int)count_sql(db, "SELECT COUNT(*) FROM orch_jobs WHERE submit_key_id = '';"), 3);
    T_EQ_INT((int)count_sql(db, "SELECT COUNT(*) FROM orch_transitions WHERE key_id = '';"), 3);

    /* The new index exists by name. */
    T_CHECK(object_exists(db, "index", "idx_orch_jobs_submit_key"));

    T_EQ_INT((int)count_sql(db, "SELECT COUNT(*) FROM pragma_foreign_key_check;"), 0);

    atlas_db_close(db);
    fx_close(&fx);
}

static const atlas_test TESTS[] = {
    {"a fresh database reaches 32 with the new columns and index",
     test_fresh_database_reaches_32},
    {"a database stopped at 31 reaches 32 losslessly, and the new columns read ''",
     test_stopped_at_31_reaches_32_losslessly},
};

ATLAS_TEST_MAIN("migrate32", TESTS)
