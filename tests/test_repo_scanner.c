/* Atlas - A13: registration records which uid's scanner may read the tree.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Registration derives the uid from the repository root's owner. An operator
 * may name one instead; a uid that may never scan fails the registration rather
 * than being stored, because 0 means "unassigned" and a refusal is not an
 * absence.
 */
#include "atlas_test.h"

#include "atlas/datadir.h"
#include "atlas/db.h"
#include "atlas/service.h"
#include "db/db_internal.h"
#include "support/fixture.h"

#include <unistd.h>

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

static void open_repo(fixture *fx, atlas_db **db, atlas_err *err) {
    T_OK(fx_open(fx, err), err);
    T_OK(fx_init_repo(fx, fx_repo(fx), "sha1", err), err);
    T_OK(fx_write(fx_repo(fx), "a.txt", "x\n", err), err);
    T_OK(fx_add_all(fx, fx_repo(fx), err), err);
    T_OK(fx_commit(fx, fx_repo(fx), "first", err), err);
    T_OK(open_migrated(fx, db, err), err);
}

static void test_registration_derives_the_uid_from_the_root(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    atlas_db *db = NULL;
    open_repo(&fx, &db, &err);

    atlas_repo_info info;
    atlas_repo_info_init(&info);
    T_OK(atlas_service_repo_add_db(db, fx_repo(&fx), "r", false, false, 0, &info, &err), &err);
    T_EQ_INT((int)info.scanner_uid, (int)getuid());

    /* And it is what was stored, not merely what was returned. */
    int64_t stored = -1;
    T_OK(atlas_db_repo_scanner_uid(db, info.id, &stored, &err), &err);
    T_EQ_INT((int)stored, (int)getuid());

    atlas_repo_info_free(&info);
    atlas_db_close(db);
    fx_close(&fx);
}

static void test_an_explicit_uid_overrides_the_derived_one(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    atlas_db *db = NULL;
    open_repo(&fx, &db, &err);

    atlas_repo_info info;
    atlas_repo_info_init(&info);
    T_OK(atlas_service_repo_add_db(db, fx_repo(&fx), "r", false, true, 4242, &info, &err), &err);
    T_EQ_INT((int)info.scanner_uid, 4242);

    atlas_repo_info_free(&info);
    atlas_db_close(db);
    fx_close(&fx);
}

/* `(true, 0)` is the operator naming root, which is refused. `(false, 0)` is
 * "derive from the root's owner" and is not — which is why the flag is a
 * separate parameter rather than a sentinel value. A refusal fails the
 * registration and names the reason; it does not register the repository with
 * an unassigned uid, because that would make a refusal look like an absence. */
static void test_a_refused_uid_fails_registration_with_a_reason(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    atlas_db *db = NULL;
    open_repo(&fx, &db, &err);

    atlas_repo_info info;
    atlas_repo_info_init(&info);
    T_CHECK(atlas_service_repo_add_db(db, fx_repo(&fx), "r", false, true, 0, &info, &err) !=
            ATLAS_OK);
    T_CHECK(err.msg[0] != '\0');
    atlas_repo_info_free(&info);

    /* Nothing was left behind. */
    int64_t count = -1;
    T_OK(atlas_db_query_int64(db, "SELECT COUNT(*) FROM repositories;", &count, &err), &err);
    T_EQ_INT((int)count, 0);

    atlas_db_close(db);
    fx_close(&fx);
}

/* A repository registered before A13 carries 0. The command is how an operator
 * assigns one without re-registering, which is the only path such a repository
 * has — a migration cannot `stat` a root, so it left them all unassigned. */
static void test_the_command_assigns_a_uid_to_an_existing_repository(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    atlas_db *db = NULL;
    open_repo(&fx, &db, &err);

    atlas_repo_info info;
    atlas_repo_info_init(&info);
    T_OK(atlas_service_repo_add_db(db, fx_repo(&fx), "r", false, false, 0, &info, &err), &err);
    int64_t id = info.id;
    atlas_repo_info_free(&info);
    /* Simulate a pre-A13 row. */
    T_OK(atlas_db_repo_set_scanner_uid(db, id, 0, &err), &err);

    atlas_repo_info after;
    atlas_repo_info_init(&after);
    T_OK(atlas_service_repo_set_scanner_db(db, "r", false, 0, &after, &err), &err);
    T_EQ_INT((int)after.scanner_uid, (int)getuid());
    atlas_repo_info_free(&after);

    /* And an explicit uid replaces it. */
    atlas_repo_info named;
    atlas_repo_info_init(&named);
    T_OK(atlas_service_repo_set_scanner_db(db, "r", true, 4242, &named, &err), &err);
    T_EQ_INT((int)named.scanner_uid, 4242);
    atlas_repo_info_free(&named);

    atlas_db_close(db);
    fx_close(&fx);
}

/* The same refusals apply, and a refusal leaves the stored value alone rather
 * than clearing it — a repository that had a working scanner must not lose one
 * because a later command named something impossible. */
static void test_the_command_refuses_and_leaves_the_old_value(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    atlas_db *db = NULL;
    open_repo(&fx, &db, &err);

    atlas_repo_info info;
    atlas_repo_info_init(&info);
    T_OK(atlas_service_repo_add_db(db, fx_repo(&fx), "r", false, true, 4242, &info, &err), &err);
    int64_t id = info.id;
    atlas_repo_info_free(&info);

    atlas_repo_info bad;
    atlas_repo_info_init(&bad);
    T_CHECK(atlas_service_repo_set_scanner_db(db, "r", true, 0, &bad, &err) != ATLAS_OK);
    T_CHECK(err.msg[0] != '\0');
    atlas_repo_info_free(&bad);

    int64_t still = -1;
    T_OK(atlas_db_repo_scanner_uid(db, id, &still, &err), &err);
    T_EQ_INT((int)still, 4242);

    atlas_db_close(db);
    fx_close(&fx);
}

/* A name nobody registered is an error that says so, not a silent success. */
static void test_an_unknown_repository_is_an_error(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    atlas_db *db = NULL;
    open_repo(&fx, &db, &err);

    atlas_repo_info out;
    atlas_repo_info_init(&out);
    T_CHECK(atlas_service_repo_set_scanner_db(db, "nosuch", false, 0, &out, &err) != ATLAS_OK);
    T_CHECK(err.msg[0] != '\0');
    atlas_repo_info_free(&out);

    atlas_db_close(db);
    fx_close(&fx);
}

static const atlas_test TESTS[] = {
    {"registration derives the uid from the root", test_registration_derives_the_uid_from_the_root},
    {"the command assigns a uid to an existing repository",
     test_the_command_assigns_a_uid_to_an_existing_repository},
    {"the command refuses and leaves the old value",
     test_the_command_refuses_and_leaves_the_old_value},
    {"an unknown repository is an error", test_an_unknown_repository_is_an_error},
    {"an explicit uid overrides the derived one", test_an_explicit_uid_overrides_the_derived_one},
    {"a refused uid fails registration with a reason",
     test_a_refused_uid_fails_registration_with_a_reason},
};

ATLAS_TEST_MAIN("repo_scanner", TESTS)
