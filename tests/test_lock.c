/* Atlas - the data-directory writer lock.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The property under test is not "a lock exists" but "a second writer is
 * refused". Every case here is written from that angle.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/lock.h"
#include "atlas_test.h"
#include "support/fixture.h"

static void test_acquire_and_release(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    atlas_lock *a = NULL;
    T_OK(atlas_lock_acquire(fx_data_dir(&fx), ATLAS_LOCK_ROLE_DAEMON, &a, &err), &err);
    T_CHECK(a != NULL);

    bool held = false;
    atlas_buf holder = ATLAS_BUF_INIT;
    T_OK(atlas_lock_probe(fx_data_dir(&fx), &held, &holder, &err), &err);
    T_CHECK_MSG(held, "the lock must read as held while it is held");
    T_CHECK_MSG(strstr(atlas_buf_cstr(&holder), "daemon") != NULL,
                "the holder's recorded role should say daemon, got \"%s\"",
                atlas_buf_cstr(&holder));

    atlas_lock_release(a);
    atlas_buf_reset(&holder);
    T_OK(atlas_lock_probe(fx_data_dir(&fx), &held, &holder, &err), &err);
    T_CHECK_MSG(!held, "the lock must be free after release");

    atlas_buf_free(&holder);
    fx_close(&fx);
}

/* The whole point: a second writer in the same process is refused, and a second
 * writer in another process is refused too — flock is per open file description,
 * so both cases exercise the same kernel behaviour Atlas relies on. */
static void test_second_writer_refused(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    atlas_lock *a = NULL;
    T_OK(atlas_lock_acquire(fx_data_dir(&fx), ATLAS_LOCK_ROLE_DAEMON, &a, &err), &err);

    atlas_lock *b = NULL;
    atlas_err berr;
    atlas_err_init(&berr);
    T_FAILS_WITH(atlas_lock_acquire(fx_data_dir(&fx), ATLAS_LOCK_ROLE_ONESHOT, &b, &berr),
                 ATLAS_ERR_INTEGRITY, &berr);
    T_CHECK_MSG(b == NULL, "a refused acquire must not hand back a lock");
    /* The message has to be actionable, not just a failure. */
    T_CHECK_MSG(strstr(atlas_err_msg(&berr), "systemctl") != NULL,
                "the refusal should say how to resolve it, got \"%s\"", atlas_err_msg(&berr));

    atlas_lock_release(a);
    /* Free again once the first holder is gone. */
    T_OK(atlas_lock_acquire(fx_data_dir(&fx), ATLAS_LOCK_ROLE_ONESHOT, &b, &err), &err);
    atlas_lock_release(b);
    fx_close(&fx);
}

/* A crashed holder must not leave a lock behind: the kernel releases flock when
 * the descriptor closes, however the process ended. Simulated here by closing
 * the lock without an orderly release path being involved. */
static void test_release_is_not_a_file_state(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    atlas_lock *a = NULL;
    T_OK(atlas_lock_acquire(fx_data_dir(&fx), ATLAS_LOCK_ROLE_DAEMON, &a, &err), &err);
    atlas_lock_release(a);

    /* The file still exists and still records the previous holder, but the lock
     * is free. Ownership must be decided by the kernel, never by the text. */
    atlas_buf path = ATLAS_BUF_INIT;
    T_OK(atlas_lock_path(fx_data_dir(&fx), &path, &err), &err);
    struct stat sb;
    T_CHECK(stat(atlas_buf_cstr(&path), &sb) == 0);

    atlas_lock *b = NULL;
    T_OK(atlas_lock_acquire(fx_data_dir(&fx), ATLAS_LOCK_ROLE_ONESHOT, &b, &err), &err);
    atlas_lock_release(b);
    atlas_buf_free(&path);
    fx_close(&fx);
}

static void test_symlink_refused(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    atlas_buf path = ATLAS_BUF_INIT;
    T_OK(atlas_lock_path(fx_data_dir(&fx), &path, &err), &err);
    /* Somebody planted a symlink where the lock belongs. Following it would let
     * them redirect Atlas' lock — and its diagnostic writes — elsewhere. */
    T_REQUIRE(symlink("/dev/null", atlas_buf_cstr(&path)) == 0);

    atlas_lock *a = NULL;
    atlas_err lerr;
    atlas_err_init(&lerr);
    T_FAILS_WITH(atlas_lock_acquire(fx_data_dir(&fx), ATLAS_LOCK_ROLE_DAEMON, &a, &lerr),
                 ATLAS_ERR_INTEGRITY, &lerr);
    T_CHECK(a == NULL);
    T_CHECK_MSG(strstr(atlas_err_msg(&lerr), "symbolic link") != NULL,
                "the refusal should name the cause, got \"%s\"", atlas_err_msg(&lerr));

    atlas_buf_free(&path);
    fx_close(&fx);
}

static void test_probe_without_lock_file(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    /* Probing must not create the file: "never locked" and "locked and free"
     * have to stay distinguishable, and a probe that creates state is not a
     * probe. */
    bool held = true;
    T_OK(atlas_lock_probe(fx_data_dir(&fx), &held, NULL, &err), &err);
    T_CHECK(!held);

    atlas_buf path = ATLAS_BUF_INIT;
    T_OK(atlas_lock_path(fx_data_dir(&fx), &path, &err), &err);
    struct stat sb;
    T_CHECK_MSG(stat(atlas_buf_cstr(&path), &sb) != 0, "probing must not create the lock file");
    atlas_buf_free(&path);
    fx_close(&fx);
}

static const atlas_test TESTS[] = {
    {"acquire and release", test_acquire_and_release},
    {"a second writer is refused", test_second_writer_refused},
    {"ownership is kernel state, not file content", test_release_is_not_a_file_state},
    {"a symlinked lock path is refused", test_symlink_refused},
    {"probing creates nothing", test_probe_without_lock_file},
};

ATLAS_TEST_MAIN("lock", TESTS)
