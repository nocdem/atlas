/* Atlas - migrating a real schema-v2 database forward to v3.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The fixture is not a hand-written v2 schema: it is the actual v1 and v2
 * statements the shipped code applied, so this tests the migration a real user's
 * A0 database will take rather than a reconstruction of it.
 */
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

/* Applies migrations 1 and 2 only, leaving the database exactly where A0 left
 * it. Taken from the shipped list rather than duplicated, so a change to the
 * historical statements cannot silently diverge from what this tests. */
static void migrate_to_v2(atlas_db *db, atlas_err *err) {
    size_t count = 0;
    const atlas_migration *all = atlas_migrations(&count);
    T_REQUIRE(count >= 3u);
    T_OK(atlas_db_migrate_list(db, all, 2u, err), err);
    T_EQ_INT(atlas_db_schema_version(db, err), 2);
}

static atlas_status open_db(fixture *fx, atlas_db **out, atlas_err *err) {
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

static int64_t count_of(atlas_db *db, const char *sql, atlas_err *err) {
    int64_t v = -1;
    T_OK(atlas_db_query_int64(db, sql, &v, err), err);
    return v;
}

static void test_v2_fixture_migrates_forward(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    atlas_db *db = NULL;
    T_OK(open_db(&fx, &db, &err), &err);
    migrate_to_v2(db, &err);

    /* Populate it the way A0 would have: a repository, a scan, files, commits,
     * changes and evidence. The migration has to carry all of this forward. */
    static const char SEED[] =
        "INSERT INTO repositories(id, name, root_path, root_path_text, git_common_dir,"
        " git_common_dir_text, object_format, registered_at, head_state)"
        " VALUES(1,'old',X'2F746D702F72',' /tmp/r',X'2F746D702F722F676974','/tmp/r/git','sha1',"
        "'2026-01-01T00:00:00Z','born');"
        "INSERT INTO scans(id, repo_id, started_at, status) VALUES(1,1,'2026-01-01T00:00:00Z','ok');"
        "INSERT INTO files(id, repo_id, path_raw, path_text, file_type, first_seen_scan_id,"
        " last_seen_scan_id, first_seen_at, last_seen_at)"
        " VALUES(1,1,X'612E63','a.c','regular',1,1,'2026-01-01T00:00:00Z','2026-01-01T00:00:00Z');"
        "INSERT INTO commits(id, repo_id, oid, subject) VALUES(1,1,'abc123','old commit');"
        "INSERT INTO file_changes(id, repo_id, commit_id, change_type, path_raw, path_text,"
        " raw_status) VALUES(1,1,1,'add',X'612E63','a.c','A');"
        "INSERT INTO evidence(repo_id, kind, created_at) VALUES(1,'SOURCE','2026-01-01T00:00:00Z');";
    T_OK(atlas_db_exec_sql(db, SEED, &err), &err);

    int64_t files_before = count_of(db, "SELECT count(*) FROM files;", &err);
    int64_t commits_before = count_of(db, "SELECT count(*) FROM commits;", &err);
    int64_t changes_before = count_of(db, "SELECT count(*) FROM file_changes;", &err);
    int64_t evidence_before = count_of(db, "SELECT count(*) FROM evidence;", &err);

    /* The migration under test. */
    T_OK(atlas_db_migrate(db, &err), &err);
    T_EQ_INT(atlas_db_schema_version(db, &err), ATLAS_SCHEMA_VERSION);
    T_EQ_INT(ATLAS_SCHEMA_VERSION, 3);

    /* Nothing was recreated or dropped. */
    T_EQ_INT(count_of(db, "SELECT count(*) FROM files;", &err), files_before);
    T_EQ_INT(count_of(db, "SELECT count(*) FROM commits;", &err), commits_before);
    T_EQ_INT(count_of(db, "SELECT count(*) FROM file_changes;", &err), changes_before);
    T_EQ_INT(count_of(db, "SELECT count(*) FROM evidence;", &err), evidence_before);
    T_EQ_INT(count_of(db, "SELECT count(*) FROM repositories WHERE name='old';", &err), 1);

    /* The new columns exist and carry the documented backfill: an A0 row
     * described a tracked file, so `tracked` must be 1 and not 0. */
    T_EQ_INT(count_of(db, "SELECT tracked FROM files WHERE id=1;", &err), 1);
    T_EQ_INT(count_of(db, "SELECT ignored FROM files WHERE id=1;", &err), 0);
    T_EQ_INT(count_of(db, "SELECT truncated FROM files WHERE id=1;", &err), 0);
    T_EQ_INT(count_of(db, "SELECT last_generation FROM files WHERE id=1;", &err), 0);
    /* No filesystem identity yet, which is what makes the row a rehash candidate
     * exactly once. Every one of the eight columns must exist and be NULL: a row
     * carrying some of them would be compared partially, and a partial match
     * reports "unchanged" on the strength of the fields that happen to be there. */
    static const char *const IDENTITY_COLUMNS[] = {
        "fs_dev",        "fs_ino",        "fs_size", "fs_mtime_sec",
        "fs_mtime_nsec", "fs_ctime_sec",  "fs_ctime_nsec", "fs_mode",
    };
    T_EQ_INT((int)(sizeof(IDENTITY_COLUMNS) / sizeof(IDENTITY_COLUMNS[0])),
             ATLAS_FS_IDENTITY_COLUMNS);
    for (size_t i = 0; i < sizeof(IDENTITY_COLUMNS) / sizeof(IDENTITY_COLUMNS[0]); i++) {
        char sql[160];
        (void)snprintf(sql, sizeof(sql), "SELECT count(*) FROM files WHERE id=1 AND %s IS NULL;",
                       IDENTITY_COLUMNS[i]);
        T_CHECK_MSG(count_of(db, sql, &err) == 1,
                    "migration 3 must add %s, and an A0 row must have it NULL",
                    IDENTITY_COLUMNS[i]);
    }

    /* Every new object is present. */
    static const char *const NEW_TABLES[] = {"repo_index_state", "repo_events", "repo_commit_tips",
                                             "daemon_state"};
    for (size_t i = 0; i < sizeof(NEW_TABLES) / sizeof(NEW_TABLES[0]); i++) {
        char sql[160];
        (void)snprintf(sql, sizeof(sql),
                       "SELECT count(*) FROM sqlite_schema WHERE type='table' AND name='%s';",
                       NEW_TABLES[i]);
        T_CHECK_MSG(count_of(db, sql, &err) == 1, "migration 3 should create %s", NEW_TABLES[i]);
    }
    static const char *const NEW_INDEXES[] = {"idx_files_repo_generation", "idx_repo_events_repo",
                                              "idx_repo_events_dedup"};
    for (size_t i = 0; i < sizeof(NEW_INDEXES) / sizeof(NEW_INDEXES[0]); i++) {
        char sql[160];
        (void)snprintf(sql, sizeof(sql),
                       "SELECT count(*) FROM sqlite_schema WHERE type='index' AND name='%s';",
                       NEW_INDEXES[i]);
        T_CHECK_MSG(count_of(db, sql, &err) == 1, "migration 3 should create %s", NEW_INDEXES[i]);
    }

    /* The database is still healthy and its foreign keys still resolve. */
    atlas_buf out = ATLAS_BUF_INIT;
    T_OK(atlas_db_integrity_check(db, &out, &err), &err);
    T_EQ_STR(atlas_buf_cstr(&out), "ok");
    T_OK(atlas_db_foreign_key_check(db, &out, &err), &err);
    T_EQ_STR(atlas_buf_cstr(&out), "ok");
    atlas_buf_free(&out);

    /* And the new tables are usable against the migrated rows. */
    T_OK(atlas_db_index_state_ensure(db, 1, &err), &err);
    int64_t gen = 0;
    T_OK(atlas_db_generation_begin(db, 1, &gen, &err), &err);
    T_EQ_INT(gen, 1);

    atlas_db_close(db);
    fx_close(&fx);
}

static void test_migration_is_idempotent(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);
    atlas_db *db = NULL;
    T_OK(open_db(&fx, &db, &err), &err);

    T_OK(atlas_db_migrate(db, &err), &err);
    T_EQ_INT(atlas_db_schema_version(db, &err), ATLAS_SCHEMA_VERSION);
    /* Applying an up-to-date set must be a no-op, not a duplicate-column error. */
    T_OK(atlas_db_migrate(db, &err), &err);
    T_OK(atlas_db_migrate(db, &err), &err);
    T_EQ_INT(atlas_db_schema_version(db, &err), ATLAS_SCHEMA_VERSION);
    T_EQ_INT(count_of(db, "SELECT count(*) FROM schema_migrations;", &err), ATLAS_SCHEMA_VERSION);

    atlas_db_close(db);
    fx_close(&fx);
}

/* A migration that fails part way must leave nothing behind: the whole point of
 * running each one in its own transaction. */
static void test_failing_migration_rolls_back_completely(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);
    atlas_db *db = NULL;
    T_OK(open_db(&fx, &db, &err), &err);
    T_OK(atlas_db_migrate(db, &err), &err);

    static const char *const BROKEN_STMTS[] = {
        "CREATE TABLE m4_partial(x INTEGER);",
        "CREATE TABLE m4_partial(x INTEGER);", /* the same name twice: fails */
        NULL,
    };
    const atlas_migration broken = {ATLAS_SCHEMA_VERSION + 1, "deliberately broken", BROKEN_STMTS};

    atlas_err merr;
    atlas_err_init(&merr);
    T_FAILS_WITH(atlas_db_migrate_list(db, &broken, 1u, &merr), ATLAS_ERR_DB, &merr);
    T_CHECK(strstr(atlas_err_msg(&merr), "rolled back") != NULL);

    /* The first statement's table must be gone with the rest of it. */
    T_EQ_INT(count_of(db,
                      "SELECT count(*) FROM sqlite_schema WHERE type='table' AND"
                      " name='m4_partial';",
                      &err),
             0);
    T_EQ_INT(atlas_db_schema_version(db, &err), ATLAS_SCHEMA_VERSION);

    atlas_buf out = ATLAS_BUF_INIT;
    T_OK(atlas_db_integrity_check(db, &out, &err), &err);
    T_EQ_STR(atlas_buf_cstr(&out), "ok");
    atlas_buf_free(&out);

    atlas_db_close(db);
    fx_close(&fx);
}

static void test_readonly_handle_will_not_migrate(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    atlas_buf path = ATLAS_BUF_INIT;
    T_OK(atlas_datadir_ensure(fx_data_dir(&fx), &err), &err);
    T_OK(atlas_datadir_db_path(fx_data_dir(&fx), &path, &err), &err);

    atlas_db *rw = NULL;
    T_OK(atlas_db_open(atlas_buf_cstr(&path), &rw, &err), &err);
    migrate_to_v2(rw, &err);
    atlas_db_close(rw);

    /* Two readers racing to migrate is precisely the corruption the
     * single-writer rule prevents, so a reader refuses and says how to fix it. */
    atlas_db *ro = NULL;
    T_OK(atlas_db_open_readonly(atlas_buf_cstr(&path), &ro, &err), &err);
    T_CHECK(atlas_db_is_readonly(ro));
    atlas_err merr;
    atlas_err_init(&merr);
    T_FAILS_WITH(atlas_db_migrate(ro, &merr), ATLAS_ERR_DB, &merr);
    T_CHECK(strstr(atlas_err_msg(&merr), "read-only") != NULL);
    atlas_db_close(ro);

    /* An up-to-date database opened read-only is fine. */
    T_OK(atlas_db_open(atlas_buf_cstr(&path), &rw, &err), &err);
    T_OK(atlas_db_migrate(rw, &err), &err);
    atlas_db_close(rw);
    T_OK(atlas_db_open_readonly(atlas_buf_cstr(&path), &ro, &err), &err);
    T_OK(atlas_db_migrate(ro, &err), &err);
    atlas_db_close(ro);

    atlas_buf_free(&path);
    fx_close(&fx);
}

static const atlas_test TESTS[] = {
    {"a real schema-v2 database migrates forward intact", test_v2_fixture_migrates_forward},
    {"migration is idempotent", test_migration_is_idempotent},
    {"a failing migration rolls back completely", test_failing_migration_rolls_back_completely},
    {"a read-only handle refuses to migrate", test_readonly_handle_will_not_migrate},
};

ATLAS_TEST_MAIN("migrate3", TESTS)
