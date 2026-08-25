/* Atlas - A13: which uid may scan a repository, and which may never.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The answer is the owner of the repository root. It is not derived from who
 * ran `atlas repo add`: registration is a local write under the data-directory
 * lock with no socket peer to ask, and which uid performs it depends on the
 * deployment — the operator's own in a per-user install, the daemon's in a
 * system one. An answer that changes with the deployment is not an identity.
 */
#include "atlas_test.h"

#include "atlas/scanner_uid.h"
#include "support/fixture.h"

#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void test_the_roots_owner_is_the_scanner_uid(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    int64_t uid = -1;
    T_OK(atlas_scanner_uid_of_root(fx_repo(&fx), &uid, &err), &err);
    T_EQ_INT((int)uid, (int)getuid());

    fx_close(&fx);
}

/* A root that cannot be read is an error carrying a message, never a silent 0:
 * 0 is the column's "unassigned" value, so a failed read that produced it would
 * be indistinguishable from a deliberate absence. */
static void test_a_missing_root_is_an_error_not_zero(void) {
    atlas_err err;
    atlas_err_init(&err);
    int64_t uid = -1;
    T_CHECK(atlas_scanner_uid_of_root("/nonexistent/atlas/a13", &uid, &err) != ATLAS_OK);
    T_CHECK(err.msg[0] != '\0');
}

/* A file is not a repository root, and answering with its owner would be
 * answering a question that was not asked. */
static void test_a_root_that_is_not_a_directory_is_refused(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);
    T_OK(fx_write(fx_repo(&fx), "afile", "x\n", &err), &err);

    atlas_buf p = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&p, &err, "%s/afile", fx_repo(&fx)), &err);
    int64_t uid = -1;
    T_CHECK(atlas_scanner_uid_of_root(atlas_buf_cstr(&p), &uid, &err) != ATLAS_OK);
    atlas_buf_free(&p);

    fx_close(&fx);
}

/* Root is refused in every deployment, because 0 is how the column records
 * "no scanner assigned" and the two must stay distinguishable. */
static void test_root_is_always_refused(void) {
    T_CHECK(atlas_scanner_uid_refusal(0) != NULL);
}

/* An ordinary uid that owns its tree is exactly the case this season exists
 * for, and it is refused by nothing.
 *
 * On the machine that produced this season the operator's uid is also
 * `model_dispatcher_uid`, A8.1's named exception to "every model process runs
 * as `atlas-worker`". Refusing it would refuse the very principal A13 exists to
 * let scan, so this case is what keeps that refusal from being reintroduced. */
static void test_an_ordinary_uid_is_permitted(void) {
    T_CHECK(atlas_scanner_uid_refusal((int64_t)getuid()) == NULL);
}

static const atlas_test TESTS[] = {
    {"the root's owner is the scanner uid", test_the_roots_owner_is_the_scanner_uid},
    {"a missing root is an error, not a zero", test_a_missing_root_is_an_error_not_zero},
    {"a root that is not a directory is refused", test_a_root_that_is_not_a_directory_is_refused},
    {"root is always refused as a scanner uid", test_root_is_always_refused},
    {"an ordinary uid is permitted", test_an_ordinary_uid_is_permitted},
};

ATLAS_TEST_MAIN("scanner_uid", TESTS)
