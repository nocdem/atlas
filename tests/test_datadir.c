/* Atlas - data directory resolution and permission tests.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "atlas/atlas.h"
#include "atlas/syspolicy.h"
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

    /* **A7.1 inserted the system policy between the explicit selectors and the
     * implicit ones**, so the next two cases depend on whether this machine
     * carries one. Both outcomes are asserted rather than one being skipped:
     * the contract is different on the two kinds of machine and both are real.
     *
     * The rule: `--data-dir` and `ATLAS_DATA_DIR` always win, because both are
     * somebody naming an index. `XDG_DATA_HOME` and `$HOME` lose to a system
     * policy, because after a cutover the home directory still holds the
     * pre-migration database and silently answering from it is the failure
     * A7.1 exists to prevent. */
    atlas_syspolicy sp;
    atlas_syspolicy_load(&sp);
    const bool deployed = (sp.state == ATLAS_SYSPOLICY_SYSTEM);
    atlas_test_note(deployed ? "a system policy is active: it outranks XDG_DATA_HOME and HOME"
                             : "no system policy: the pre-A7.1 precedence applies");

    clear_env();
    T_REQUIRE(setenv("XDG_DATA_HOME", "/xdg", 1) == 0);
    T_REQUIRE(setenv("HOME", "/home/someone", 1) == 0);
    T_OK(atlas_datadir_resolve(NULL, &out, &src, &err), &err);
    if (deployed) {
        T_EQ_STR(atlas_buf_cstr(&out), sp.data_dir);
        T_EQ_INT(src, ATLAS_DATADIR_SYSTEM);
    } else {
        T_EQ_STR(atlas_buf_cstr(&out), "/xdg/atlas");
        T_EQ_INT(src, ATLAS_DATADIR_XDG);
    }

    clear_env();
    T_REQUIRE(setenv("HOME", "/home/someone", 1) == 0);
    T_OK(atlas_datadir_resolve(NULL, &out, &src, &err), &err);
    if (deployed) {
        T_EQ_STR(atlas_buf_cstr(&out), sp.data_dir);
        T_EQ_INT(src, ATLAS_DATADIR_SYSTEM);
    } else {
        T_EQ_STR(atlas_buf_cstr(&out), "/home/someone/.local/share/atlas");
        T_EQ_INT(src, ATLAS_DATADIR_HOME);
    }

    /* And the explicit selectors keep winning on both kinds of machine. */
    clear_env();
    T_REQUIRE(setenv("ATLAS_DATA_DIR", "/env/dir", 1) == 0);
    T_OK(atlas_datadir_resolve(NULL, &out, &src, &err), &err);
    T_EQ_STR(atlas_buf_cstr(&out), "/env/dir");
    T_EQ_INT(src, ATLAS_DATADIR_ENV);
    T_OK(atlas_datadir_resolve("/explicit", &out, &src, &err), &err);
    T_EQ_STR(atlas_buf_cstr(&out), "/explicit");
    T_EQ_INT(src, ATLAS_DATADIR_OVERRIDE);

    atlas_buf_free(&out);
    clear_env();
}

static void test_empty_and_relative_are_errors(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf out = ATLAS_BUF_INIT;
    atlas_syspolicy sp;
    atlas_syspolicy_load(&sp);
    const bool deployed = (sp.state == ATLAS_SYSPOLICY_SYSTEM);

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

    /* The last two depend on the machine for the reason given above: a system
     * policy is consulted before `XDG_DATA_HOME` and before "nothing at all",
     * so on a deployed machine there is always an answer and these are not
     * errors. That is the point of the policy — after a cutover, "I could not
     * work out which index you meant" must not resolve to the operator's home
     * directory. */
    clear_env();
    T_REQUIRE(setenv("XDG_DATA_HOME", "relative", 1) == 0);
    if (deployed) {
        T_OK(atlas_datadir_resolve(NULL, &out, NULL, &err), &err);
        T_EQ_STR(atlas_buf_cstr(&out), sp.data_dir);
    } else {
        T_FAILS_WITH(atlas_datadir_resolve(NULL, &out, NULL, &err), ATLAS_ERR_CONFIG, &err);
    }

    /* With nothing at all to go on, Atlas says so instead of guessing. */
    clear_env();
    if (deployed) {
        T_OK(atlas_datadir_resolve(NULL, &out, NULL, &err), &err);
        T_EQ_STR(atlas_buf_cstr(&out), sp.data_dir);
    } else {
        T_FAILS_WITH(atlas_datadir_resolve(NULL, &out, NULL, &err), ATLAS_ERR_CONFIG, &err);
        T_CHECK(strstr(atlas_err_msg(&err), "ATLAS_DATA_DIR") != NULL);
    }

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

/* The bug: a read prepared the directory it was about to read from.
 *
 * On a per-user install that was invisible. Under A7.1 the index is 0700 and
 * owned by the service account, so `atlas status` and `atlas repo list` run as
 * an ordinary client failed at `chmod` — reporting "cannot restrict permissions
 * on /var/lib/atlas", which is a fact about something the command should never
 * have attempted. Creating a directory and tightening its mode is an act of
 * ownership; only a command that is about to write performs one. */
static void test_only_a_write_prepares_the_data_directory(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_OK(fx_open(&fx, &err), &err);

    atlas_buf absent = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&absent, &err, "%s/never-created", fx_data_dir(&fx)), &err);

    const atlas_ctx_mode READERS[] = {ATLAS_CTX_READ, ATLAS_CTX_AUTO, ATLAS_CTX_INSPECT};
    for (size_t i = 0; i < sizeof READERS / sizeof READERS[0]; i++) {
        atlas_ctx_opts opts;
        memset(&opts, 0, sizeof opts);
        opts.data_dir_override = atlas_buf_cstr(&absent);
        opts.mode = READERS[i];
        atlas_ctx *ctx = NULL;
        atlas_err oerr;
        atlas_err_init(&oerr);
        /* Whether the open succeeds is not the claim — INSPECT reports an absent
         * index and the others fail to open one. The claim is that nothing
         * appeared on disk either way. */
        if (atlas_ctx_open(&opts, &ctx, &oerr) == ATLAS_OK) {
            atlas_ctx_close(ctx);
        }
        struct stat sb;
        T_CHECK_MSG(stat(atlas_buf_cstr(&absent), &sb) != 0,
                    "mode %d created the data directory", (int)READERS[i]);
    }

    /* A write still prepares it: a first `repo add` or `scan` on a machine where
     * Atlas has never run has to create the directory, and that process owns it. */
    atlas_ctx_opts w;
    memset(&w, 0, sizeof w);
    w.data_dir_override = atlas_buf_cstr(&absent);
    w.mode = ATLAS_CTX_WRITE;
    atlas_ctx *wctx = NULL;
    T_OK(atlas_ctx_open(&w, &wctx, &err), &err);
    atlas_ctx_close(wctx);
    struct stat sb;
    T_REQUIRE(stat(atlas_buf_cstr(&absent), &sb) == 0);
    T_CHECK(S_ISDIR(sb.st_mode));
    T_EQ_INT(sb.st_mode & 07777, 0700);

    atlas_buf_free(&absent);
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
    /* WRITE because only a write prepares the directory, and this test is about
     * *which* directory is prepared. A reader would resolve the same path and
     * then fail to open an index nothing had created, which would prove the
     * same thing less directly. */
    opts.mode = ATLAS_CTX_WRITE;
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
    {"only a write prepares the data directory", test_only_a_write_prepares_the_data_directory},
    {"explicit override wins", test_override_wins},
    {"resolution order", test_resolution_order},
    {"empty and relative paths are configuration errors", test_empty_and_relative_are_errors},
    {"ensure creates a user-only tree and private database", test_ensure_creates_private_tree},
    {"ensure refuses a path that is a file", test_ensure_rejects_a_file},
    {"context honours the override and touches nothing else", test_ctx_uses_override_only},
};

ATLAS_TEST_MAIN("datadir", TESTS)
