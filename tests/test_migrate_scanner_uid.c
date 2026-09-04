/* Atlas - A13: migration 27 gives every repository a scanner uid column.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The column records which uid's scanner may report facts about a repository.
 * Its default is 0 and 0 means **unassigned**, never "uid 0" — a repository
 * registered before A13 has no scanner, and a migration that invented one would
 * be recording an intent nobody expressed. */
#include "atlas_test.h"

#include "atlas/datadir.h"
#include "atlas/db.h"
#include "atlas/service.h"
#include "db/db_internal.h"
#include "support/fixture.h"

#include <unistd.h>

/* Opens the fixture's index and brings it to the current schema.
 * `atlas_db_open` takes the database *file*, which `atlas_datadir_db_path`
 * derives from the data directory, and it creates that file without applying a
 * single migration — `atlas_db_migrate` is what does that. */
static atlas_status open_migrated(fixture *fx, atlas_db **out, atlas_err *err) {
    atlas_buf path = ATLAS_BUF_INIT;
    atlas_status st = atlas_datadir_ensure(fx_data_dir(fx), err);
    if (st == ATLAS_OK) {
        st = atlas_datadir_db_path(fx_data_dir(fx), &path, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_open(atlas_buf_cstr(&path), out, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_migrate(*out, err);
    }
    atlas_buf_free(&path);
    return st;
}

static void test_migration_27_adds_scanner_uid_defaulting_to_unset(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    /* Migration 31 (A16's T2) landed after this suite was written. */
    T_EQ_INT((int)ATLAS_SCHEMA_VERSION, 31);

    atlas_db *db = NULL;
    T_OK(open_migrated(&fx, &db, &err), &err);

    int64_t applied = -1;
    T_OK(atlas_db_query_int64(db, "SELECT MAX(version) FROM schema_migrations;", &applied, &err),
         &err);
    T_EQ_INT((int)applied, (int)ATLAS_SCHEMA_VERSION);

    /* Asserted against the schema rather than against a row, because this test
     * registers nothing: the claim is about what the column declares, not about
     * what some repository happens to hold. */
    int64_t declared = -1;
    T_OK(atlas_db_query_int64(db,
                              "SELECT COUNT(*) FROM pragma_table_info('repositories') "
                              "WHERE name = 'scanner_uid' AND \"notnull\" = 1 "
                              "AND dflt_value = '0';",
                              &declared, &err),
         &err);
    T_EQ_INT((int)declared, 1);

    atlas_db_close(db);
    fx_close(&fx);
}

/* The uid round-trips through the typed operations and reaches the struct every
 * read path fills. Zero clears an assignment rather than naming root: the
 * column cannot express both, which is why root is refused as a scanner uid. */
static void test_scanner_uid_round_trips_and_zero_clears(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);
    T_OK(fx_init_repo(&fx, fx_repo(&fx), "sha1", &err), &err);

    atlas_db *db = NULL;
    T_OK(open_migrated(&fx, &db, &err), &err);

    atlas_repo_info reg;
    atlas_repo_info_init(&reg);
    T_OK(atlas_service_repo_add_db(db, fx_repo(&fx), "r", false, false, 0, &reg, &err), &err);
    int64_t repo_id = reg.id;
    /* Registration now derives it from the root's owner (Task 4). */
    T_EQ_INT((int)reg.scanner_uid, (int)getuid());
    atlas_repo_info_free(&reg);

    int64_t got = -1;
    T_OK(atlas_db_repo_scanner_uid(db, repo_id, &got, &err), &err);
    T_EQ_INT((int)got, (int)getuid());

    T_OK(atlas_db_repo_set_scanner_uid(db, repo_id, 1000, &err), &err);
    T_OK(atlas_db_repo_scanner_uid(db, repo_id, &got, &err), &err);
    T_EQ_INT((int)got, 1000);

    atlas_repo_info back;
    atlas_repo_info_init(&back);
    bool found = false;
    T_OK(atlas_db_repo_get(db, "r", &back, &found, &err), &err);
    T_CHECK(found);
    T_EQ_INT((int)back.scanner_uid, 1000);
    atlas_repo_info_free(&back);

    T_OK(atlas_db_repo_set_scanner_uid(db, repo_id, 0, &err), &err);
    T_OK(atlas_db_repo_scanner_uid(db, repo_id, &got, &err), &err);
    T_EQ_INT((int)got, 0);

    atlas_db_close(db);
    fx_close(&fx);
}

static const atlas_test TESTS[] = {
    {"migration 27 adds a scanner uid that defaults to unassigned",
     test_migration_27_adds_scanner_uid_defaulting_to_unset},
    {"the scanner uid round-trips and zero clears it",
     test_scanner_uid_round_trips_and_zero_clears},
};

ATLAS_TEST_MAIN("migrate_scanner_uid", TESTS)
