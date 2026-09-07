/* Atlas - A17 T1: migration 33 -- a deploy a credential proposed, a
 * different credential confirmed, and a root agent reported.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Two new tables, additive, neither existing table altered -- see the
 * migration comment in `src/db/migrate.c`. This suite proves:
 *
 *   - a fresh database reaches schema 33 with both new tables, both partial
 *     unique indexes on `deploys`, and no dangling foreign key;
 *   - a database stopped at 32, with pre-existing `repositories` rows, reaches
 *     33 with those rows byte-identical and both new tables present and
 *     empty;
 *   - `PRAGMA index_list(deploys)` reports exactly two partial indexes, named
 *     `idx_deploys_one_active` and `idx_deploys_one_per_job`.
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

/* `pragma_index_list` as a table-valued function -- `column_exists`'s
 * `pragma_table_info` pattern in `tests/test_migrate32.c`, one pragma over. */
static int64_t partial_index_count(atlas_db *db, const char *table) {
    char sql[160];
    (void)snprintf(sql, sizeof(sql),
                   "SELECT COUNT(*) FROM pragma_index_list('%s') WHERE partial = 1;", table);
    return count_sql(db, sql);
}

static bool partial_index_named(atlas_db *db, const char *table, const char *index_name) {
    atlas_err err;
    atlas_err_init(&err);
    char sql[192];
    (void)snprintf(sql, sizeof(sql),
                   "SELECT 1 FROM pragma_index_list('%s') WHERE partial = 1 AND name = ?1;",
                   table);
    sqlite3_stmt *q = NULL;
    if (atlas_db_prepare(db, sql, &q, &err) != ATLAS_OK) {
        return false;
    }
    (void)sqlite3_bind_text(q, 1, index_name, -1, SQLITE_TRANSIENT);
    bool found = sqlite3_step(q) == SQLITE_ROW;
    atlas_db_finish(db, q);
    return found;
}

static void table_digest(atlas_db *db, const char *table, char *out) {
    atlas_err err;
    atlas_err_init(&err);
    char sql[256];
    (void)snprintf(sql, sizeof(sql), "SELECT * FROM %s ORDER BY rowid;", table);
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
    atlas_hex_encode(d, sizeof(d), out);
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

/* A synthetic identity is enough: this suite is about migration 33's own two
 * tables, not about a real tree on disk -- `test_migrate29.c`'s
 * `insert_repo`. */
static void insert_repo(atlas_db *db, const char *name, int64_t *repo_id_out) {
    atlas_err err;
    atlas_err_init(&err);
    char root[256];
    char common[300];
    (void)snprintf(root, sizeof(root), "/tmp/atlas-migrate33-%s", name);
    (void)snprintf(common, sizeof(common), "%s/.git", root);

    atlas_repo_identity id;
    memset(&id, 0, sizeof(id));
    id.root = root;
    id.root_len = strlen(root);
    id.common_dir = common;
    id.common_dir_len = strlen(common);
    id.git_dir = common;
    id.git_dir_len = strlen(common);
    id.object_format = "sha1";
    T_OK(atlas_db_repo_add(db, name, &id, repo_id_out, &err), &err);
}

static const char *const TABLES[] = {"deploys", "deploy_transitions"};

/* --- 1: a fresh database reaches 33 --------------------------------------- */

static void test_fresh_database_reaches_33(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    atlas_db *db = NULL;
    T_OK(open_fresh(&fx, &db, &err), &err);
    T_OK(atlas_db_migrate(db, &err), &err);

    T_EQ_INT((int)ATLAS_SCHEMA_VERSION, 33);
    T_EQ_INT(schema_of(db), 33);

    for (size_t i = 0; i < sizeof(TABLES) / sizeof(TABLES[0]); i++) {
        T_CHECK_MSG(table_exists(db, TABLES[i]), "migration 33 did not create %s", TABLES[i]);
    }

    T_EQ_INT((int)partial_index_count(db, "deploys"), 2);
    T_CHECK(partial_index_named(db, "deploys", "idx_deploys_one_active"));
    T_CHECK(partial_index_named(db, "deploys", "idx_deploys_one_per_job"));

    T_EQ_INT((int)count_sql(db, "SELECT COUNT(*) FROM pragma_foreign_key_check;"), 0);

    atlas_db_close(db);
    fx_close(&fx);
}

/* --- 2: a database stopped at 32 reaches 33 losslessly -------------------- */

static void test_stopped_at_32_reaches_33_losslessly(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    atlas_db *db = NULL;
    T_OK(open_fresh(&fx, &db, &err), &err);

    size_t count = 0;
    const atlas_migration *all = atlas_migrations(&count);
    T_REQUIRE(count >= 33u);
    T_OK(atlas_db_migrate_list(db, all, 32u, &err), &err);
    T_EQ_INT(schema_of(db), 32);

    /* Real rows, so "nothing pre-existing was rewritten" is a claim about
     * every column of every row -- `test_migrate29.c`'s "stopped at 28"
     * pattern, one migration over: migration 33 touches no existing table at
     * all, so any pre-existing table proves the point, and `repositories` is
     * the simplest one to seed. */
    int64_t repo_id = 0;
    int64_t repo2_id = 0;
    insert_repo(db, "proj", &repo_id);
    insert_repo(db, "proj2", &repo2_id);
    for (size_t i = 0; i < sizeof(TABLES) / sizeof(TABLES[0]); i++) {
        T_CHECK_MSG(!table_exists(db, TABLES[i]), "%s exists before migration 33 ran", TABLES[i]);
    }
    char repos_before[ATLAS_SHA256_HEX_LEN + 1u];
    table_digest(db, "repositories", repos_before);

    T_OK(atlas_db_migrate_list(db, all, 33u, &err), &err);
    T_EQ_INT(schema_of(db), 33);

    for (size_t i = 0; i < sizeof(TABLES) / sizeof(TABLES[0]); i++) {
        T_CHECK_MSG(table_exists(db, TABLES[i]), "migration 33 created no %s", TABLES[i]);
        char sql[128];
        (void)snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s;", TABLES[i]);
        T_EQ_INT((int)count_sql(db, sql), 0);
    }

    char repos_after[ATLAS_SHA256_HEX_LEN + 1u];
    table_digest(db, "repositories", repos_after);
    T_CHECK_MSG(strcmp(repos_before, repos_after) == 0,
                "migration 33 rewrote a repositories row:\nbefore: %s\nafter:  %s", repos_before,
                repos_after);

    atlas_repo_info info;
    atlas_repo_info_init(&info);
    bool found = false;
    T_OK(atlas_db_repo_get(db, "proj", &info, &found, &err), &err);
    T_CHECK(found);
    T_EQ_INT((int)info.id, (int)repo_id);
    atlas_repo_info_free(&info);
    (void)repo2_id;

    T_EQ_INT((int)partial_index_count(db, "deploys"), 2);
    T_CHECK(partial_index_named(db, "deploys", "idx_deploys_one_active"));
    T_CHECK(partial_index_named(db, "deploys", "idx_deploys_one_per_job"));

    T_EQ_INT((int)count_sql(db, "SELECT COUNT(*) FROM pragma_foreign_key_check;"), 0);

    atlas_db_close(db);
    fx_close(&fx);
}

static const atlas_test TESTS[] = {
    {"a fresh database reaches 33 with the two new tables and their partial indexes",
     test_fresh_database_reaches_33},
    {"a database stopped at 32 reaches 33 losslessly, with both new tables present and empty",
     test_stopped_at_32_reaches_33_losslessly},
};

ATLAS_TEST_MAIN("migrate33", TESTS)
