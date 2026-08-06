/* Atlas - data directory resolution and permission tests.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "atlas/atlas.h"
#include "atlas_test.h"
#include "support/fixture.h"

static void clear_env(void) {
    (void)unsetenv("ATLAS_DATA_DIR");
    (void)unsetenv("XDG_DATA_HOME");
    (void)unsetenv("HOME");
}

static void test_override_wins(void) {
    atlas_err err;
    atlas_err_init(&err);
    clear_env();
    T_REQUIRE(setenv("ATLAS_DATA_DIR", "/env/dir", 1) == 0);
    T_REQUIRE(setenv("XDG_DATA_HOME", "/xdg", 1) == 0);
    T_REQUIRE(setenv("HOME", "/home/someone", 1) == 0);

    atlas_buf out = ATLAS_BUF_INIT;
    atlas_datadir_source src = ATLAS_DATADIR_HOME;
    T_OK(atlas_datadir_resolve("/explicit/dir", &out, &src, &err), &err);
    T_EQ_STR(atlas_buf_cstr(&out), "/explicit/dir");
    T_EQ_INT(src, ATLAS_DATADIR_OVERRIDE);
    T_EQ_STR(atlas_datadir_source_name(src), "--data-dir");
    atlas_buf_free(&out);
    clear_env();
}

static void test_resolution_order(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf out = ATLAS_BUF_INIT;
    atlas_datadir_source src = ATLAS_DATADIR_OVERRIDE;

    clear_env();
    T_REQUIRE(setenv("ATLAS_DATA_DIR", "/env/dir/", 1) == 0);
    T_REQUIRE(setenv("XDG_DATA_HOME", "/xdg", 1) == 0);
    T_REQUIRE(setenv("HOME", "/home/someone", 1) == 0);
    T_OK(atlas_datadir_resolve(NULL, &out, &src, &err), &err);
    /* Trailing slashes are trimmed so "/x/" and "/x" mean the same place. */
    T_EQ_STR(atlas_buf_cstr(&out), "/env/dir");
    T_EQ_INT(src, ATLAS_DATADIR_ENV);

    clear_env();
    T_REQUIRE(setenv("XDG_DATA_HOME", "/xdg", 1) == 0);
    T_REQUIRE(setenv("HOME", "/home/someone", 1) == 0);
    T_OK(atlas_datadir_resolve(NULL, &out, &src, &err), &err);
    T_EQ_STR(atlas_buf_cstr(&out), "/xdg/atlas");
    T_EQ_INT(src, ATLAS_DATADIR_XDG);

    clear_env();
    T_REQUIRE(setenv("HOME", "/home/someone", 1) == 0);
    T_OK(atlas_datadir_resolve(NULL, &out, &src, &err), &err);
    T_EQ_STR(atlas_buf_cstr(&out), "/home/someone/.local/share/atlas");
    T_EQ_INT(src, ATLAS_DATADIR_HOME);

    atlas_buf_free(&out);
    clear_env();
}

static void test_empty_and_relative_are_errors(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf out = ATLAS_BUF_INIT;

    clear_env();
    /* An empty or relative configured path is never silently reinterpreted. */
    T_FAILS_WITH(atlas_datadir_resolve("", &out, NULL, &err), ATLAS_ERR_CONFIG, &err);
    T_FAILS_WITH(atlas_datadir_resolve("relative/dir", &out, NULL, &err), ATLAS_ERR_CONFIG, &err);
    T_CHECK(strstr(atlas_err_msg(&err), "absolute") != NULL);

    T_REQUIRE(setenv("ATLAS_DATA_DIR", "", 1) == 0);
    T_FAILS_WITH(atlas_datadir_resolve(NULL, &out, NULL, &err), ATLAS_ERR_CONFIG, &err);
    T_CHECK(strstr(atlas_err_msg(&err), "empty") != NULL);

    T_REQUIRE(setenv("ATLAS_DATA_DIR", "not/absolute", 1) == 0);
    T_FAILS_WITH(atlas_datadir_resolve(NULL, &out, NULL, &err), ATLAS_ERR_CONFIG, &err);

    clear_env();
    T_REQUIRE(setenv("XDG_DATA_HOME", "relative", 1) == 0);
    T_FAILS_WITH(atlas_datadir_resolve(NULL, &out, NULL, &err), ATLAS_ERR_CONFIG, &err);

    /* With nothing at all to go on, Atlas says so instead of guessing. */
    clear_env();
    T_FAILS_WITH(atlas_datadir_resolve(NULL, &out, NULL, &err), ATLAS_ERR_CONFIG, &err);
    T_CHECK(strstr(atlas_err_msg(&err), "ATLAS_DATA_DIR") != NULL);

    atlas_buf_free(&out);
    clear_env();
}

static void test_ensure_creates_private_tree(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_OK(fx_open(&fx, &err), &err);

    atlas_buf nested = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&nested, &err, "%s/deep/nested/atlas", fx_data_dir(&fx)), &err);
    T_OK(atlas_datadir_ensure(atlas_buf_cstr(&nested), &err), &err);

    struct stat sb;
    T_REQUIRE(stat(atlas_buf_cstr(&nested), &sb) == 0);
    T_CHECK(S_ISDIR(sb.st_mode));
    /* User-only: the index can describe private repositories. */
    T_EQ_INT(sb.st_mode & 07777, 0700);

    /* Idempotent. */
    T_OK(atlas_datadir_ensure(atlas_buf_cstr(&nested), &err), &err);

    /* The database file must not be world- or group-readable either. */
    atlas_buf dbpath = ATLAS_BUF_INIT;
    T_OK(atlas_datadir_db_path(atlas_buf_cstr(&nested), &dbpath, &err), &err);
    T_CHECK(strstr(atlas_buf_cstr(&dbpath), "/atlas.db") != NULL);

    atlas_db *db = NULL;
    T_OK(atlas_db_open(atlas_buf_cstr(&dbpath), &db, &err), &err);
    T_OK(atlas_db_migrate(db, &err), &err);
    atlas_db_close(db);

    T_REQUIRE(stat(atlas_buf_cstr(&dbpath), &sb) == 0);
    T_CHECK_MSG((sb.st_mode & 07777) == 0600, "database mode is %04o, expected 0600",
                (unsigned)(sb.st_mode & 07777));
    T_CHECK_MSG((sb.st_mode & (S_IROTH | S_IWOTH)) == 0, "database is world-accessible");
    T_CHECK_MSG((sb.st_mode & (S_IRGRP | S_IWGRP)) == 0, "database is group-accessible");

    atlas_buf_free(&nested);
    atlas_buf_free(&dbpath);
    fx_close(&fx);
}

static void test_ensure_rejects_a_file(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_OK(fx_open(&fx, &err), &err);

    T_OK(fx_write(fx_data_dir(&fx), "notadir", "x\n", &err), &err);
    atlas_buf path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&path, &err, "%s/notadir", fx_data_dir(&fx)), &err);
    T_FAILS_WITH(atlas_datadir_ensure(atlas_buf_cstr(&path), &err), ATLAS_ERR_CONFIG, &err);
    T_CHECK(strstr(atlas_err_msg(&err), "not a directory") != NULL);

    atlas_buf_free(&path);
    fx_close(&fx);
}

static void test_ctx_uses_override_only(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_OK(fx_open(&fx, &err), &err);

    /* Point the environment at a path that must never be created, proving the
     * override is what is used. */
    clear_env();
    T_REQUIRE(setenv("ATLAS_DATA_DIR", "/atlas-should-never-create-this", 1) == 0);

    atlas_buf dir = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&dir, &err, "%s/ctx", fx_data_dir(&fx)), &err);

    atlas_ctx_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.data_dir_override = atlas_buf_cstr(&dir);
    atlas_ctx *ctx = NULL;
    T_OK(atlas_ctx_open(&opts, &ctx, &err), &err);
    T_EQ_STR(atlas_ctx_data_dir(ctx), atlas_buf_cstr(&dir));
    T_EQ_INT(atlas_ctx_data_dir_source(ctx), ATLAS_DATADIR_OVERRIDE);
    T_CHECK(strstr(atlas_ctx_db_path(ctx), atlas_buf_cstr(&dir)) != NULL);
    atlas_ctx_close(ctx);

    struct stat sb;
    T_CHECK_MSG(stat("/atlas-should-never-create-this", &sb) != 0,
                "the environment path was created despite an explicit override");

    atlas_buf_free(&dir);
    clear_env();
    fx_close(&fx);
}

static const atlas_test TESTS[] = {
    {"explicit override wins", test_override_wins},
    {"resolution order", test_resolution_order},
    {"empty and relative paths are configuration errors", test_empty_and_relative_are_errors},
    {"ensure creates a user-only tree and private database", test_ensure_creates_private_tree},
    {"ensure refuses a path that is a file", test_ensure_rejects_a_file},
    {"context honours the override and touches nothing else", test_ctx_uses_override_only},
};

ATLAS_TEST_MAIN("datadir", TESTS)
