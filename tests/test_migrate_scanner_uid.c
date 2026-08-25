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
#include "db/db_internal.h"
#include "support/fixture.h"

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

    T_EQ_INT((int)ATLAS_SCHEMA_VERSION, 27);

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

static const atlas_test TESTS[] = {
    {"migration 27 adds a scanner uid that defaults to unassigned",
     test_migration_27_adds_scanner_uid_defaulting_to_unset},
};

ATLAS_TEST_MAIN("migrate_scanner_uid", TESTS)
