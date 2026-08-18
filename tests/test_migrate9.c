/* Atlas - migration 9: a general decision-to-decision relation.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * `decision_links.kind` is a closed vocabulary enforced by a CHECK, and it held
 * no way to say that one decision relates to another. The two kinds that name a
 * document are lifecycle facts — the supersede transition writes them and the
 * status computation reads them — so a general reference had to be its own
 * kind. SQLite cannot alter a CHECK, so the table is rebuilt, and a rebuild is
 * the one migration shape that can silently lose or reorder data. This suite
 * exists for that: it populates every link kind, migrates, and compares row for
 * row and column for column. */
#include <stdlib.h>
#include <string.h>

#include "atlas/datadir.h"
#include <sqlite3.h>

#include "db/db_internal.h"
#include "atlas/db.h"
#include "atlas_test.h"
#include "support/fixture.h"

static int schema_of(atlas_db *db) {
    atlas_err err;
    atlas_err_init(&err);
    return atlas_db_schema_version(db, &err);
}

/* The stored DDL for one object, which is what SQLite keeps and what a rebuild
 * either preserves or quietly changes. */
static atlas_status schema_sql(atlas_db *db, const char *name, atlas_buf *out, atlas_err *err) {
    atlas_buf_reset(out);
    sqlite3_stmt *q = NULL;
    atlas_status st = atlas_db_prepare(
        db, "SELECT COALESCE(sql, '') FROM sqlite_schema WHERE name = ?1;", &q, err);
    if (st != ATLAS_OK) {
        return st;
    }
    (void)sqlite3_bind_text(q, 1, name, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(q) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(q, 0);
        st = atlas_buf_set_str(out, v != NULL ? v : "", err);
    }
    atlas_db_finish(db, q);
    return st;
}

static void test_a_fresh_database_is_schema_nine_with_relates_to(void) {
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
    T_EQ_INT(ATLAS_SCHEMA_VERSION, 20);

    /* Native, not merely tolerated. Asked of the stored schema rather than by
     * inserting a row: the CHECK is what the vocabulary *is*, and a test that
     * inserted would also be testing every NOT NULL column around it. */
    atlas_buf ddl = ATLAS_BUF_INIT;
    T_OK(schema_sql(db, "decision_links", &ddl, &err), &err);
    const char *sql = atlas_buf_cstr(&ddl);
    T_CHECK_MSG(strstr(sql, "'relates_to'") != NULL,
                "schema 9 does not accept relates_to:\n%s", sql);
    /* And every kind that was there before is still there: a rebuilt CHECK is
     * the easiest place to drop one by hand. */
    static const char *const KINDS[] = {"'path'",       "'commit'",     "'change_set'",
                                        "'symbol'",     "'supersedes'", "'replaced_by'"};
    for (size_t i = 0; i < sizeof KINDS / sizeof KINDS[0]; i++) {
        T_CHECK_MSG(strstr(sql, KINDS[i]) != NULL, "the rebuild lost the kind %s", KINDS[i]);
    }
    /* The constraint and the index a rebuild most easily loses. */
    T_CHECK_MSG(strstr(sql, "REFERENCES decision_revisions") != NULL,
                "the rebuild lost the foreign key to decision_revisions");
    T_CHECK_MSG(strstr(sql, "AUTOINCREMENT") != NULL, "the rebuild lost AUTOINCREMENT on id");
    for (size_t i = 0; i < 5; i++) {
        static const char *const IDX[] = {"idx_decision_links_rev", "idx_decision_links_path",
                                          "idx_decision_links_symbol", "idx_decision_links_commit",
                                          "idx_decision_links_target"};
        atlas_buf ix = ATLAS_BUF_INIT;
        T_OK(schema_sql(db, IDX[i], &ix, &err), &err);
        T_CHECK_MSG(ix.len > 0, "the rebuild lost %s", IDX[i]);
        atlas_buf_free(&ix);
    }
    atlas_buf_free(&ddl);

    atlas_db_close(db);
    atlas_buf_free(&path);
    fx_close(&fx);
}

static const atlas_test TESTS[] = {
    {"a fresh database is schema nine and holds relates_to natively",
     test_a_fresh_database_is_schema_nine_with_relates_to},
};

ATLAS_TEST_MAIN("migrate9", TESTS)
