/* Atlas - A13 Plan 6: a repository the daemon cannot open is read from its mirror.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The failure this closes was measured on a live machine: `atlasd` is its own
 * principal, `/opt/atlas` is not readable by it, and the daemon logged
 * "repository atlas cannot be opened" every ten seconds forever. The tree was
 * fine; the reader was the wrong principal.
 *
 * A second uid is not needed to reproduce the *call* that failed. The daemon's
 * refusal came out of `atlas_git_open` on the registered root, and a root that
 * is not a git repository fails there identically. What the test asserts is
 * therefore the fallback's behaviour, not the permission that motivated it.
 */
#include "atlas/datadir.h"
#include "atlas/error.h"
#include "atlas/git.h"
#include "atlas_test.h"
#include "daemon/daemon_internal.h"
#include "daemon/mirror.h"
#include "support/fixture.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Builds a real git repository at `<data_dir>/mirror/<id>`, the way the scanner
 * would have left one, and returns its HEAD. */
static atlas_status build_mirror(fixture *fx, int64_t id, char *oid_out, atlas_err *err) {
    int fd = -1;
    atlas_status st = atlas_mirror_open_repo(fx_data_dir(fx), id, &fd, err);
    if (st != ATLAS_OK) {
        return st;
    }
    (void)close(fd);

    char path[2048];
    (void)snprintf(path, sizeof(path), "%s/mirror/%lld", fx_data_dir(fx), (long long)id);

    st = fx_init_repo(fx, path, NULL, err);
    if (st == ATLAS_OK) {
        st = fx_write(path, "mirrored.txt", "from the scanner\n", err);
    }
    if (st == ATLAS_OK) {
        st = fx_add_all(fx, path, err);
    }
    if (st == ATLAS_OK) {
        st = fx_commit(fx, path, "the mirror's commit", err);
    }
    if (st != ATLAS_OK) {
        return st;
    }

    atlas_git *g = NULL;
    st = atlas_git_open(path, &g, err);
    if (st != ATLAS_OK) {
        return st;
    }
    atlas_git_head h;
    memset(&h, 0, sizeof(h));
    st = atlas_git_read_head(g, &h, err);
    if (st == ATLAS_OK) {
        (void)snprintf(oid_out, ATLAS_OID_HEX_MAX_INCL, "%s", h.oid);
    }
    atlas_git_close(g);
    return st;
}

/* The whole point: an unopenable root plus a mirror resolves to the mirror. */
static void test_falls_back_to_the_mirror(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);

    char want[ATLAS_OID_HEX_MAX_INCL];
    T_OK(build_mirror(&fx, 7, want, &err), &err);

    /* `fx_repo` exists but was never `git init`ed here, so `atlas_git_open`
     * refuses it exactly as it refused the unreadable tree. */
    atlas_git *g = NULL;
    bool from_mirror = false;
    T_OK(atlas_daemon_open_index_root(fx_data_dir(&fx), 7, fx_repo(&fx), &g, &from_mirror, &err),
         &err);
    T_CHECK_MSG(from_mirror, "the mirror is what answered");
    T_REQUIRE(g != NULL);

    atlas_git_head h;
    memset(&h, 0, sizeof(h));
    T_OK(atlas_git_read_head(g, &h, &err), &err);
    T_CHECK_MSG(strcmp(h.oid, want) == 0, "the mirror's HEAD is what reconcile would see");
    atlas_git_close(g);

    fx_close(&fx);
}

/* A repository the daemon *can* read is still read directly. Reading the thing
 * itself is better evidence than reading a copy of it, so the fallback must not
 * quietly become the default. */
static void test_a_readable_root_is_preferred(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);

    T_OK(fx_init_repo(&fx, fx_repo(&fx), NULL, &err), &err);
    T_OK(fx_write(fx_repo(&fx), "real.txt", "the tree itself\n", &err), &err);
    T_OK(fx_add_all(&fx, fx_repo(&fx), &err), &err);
    T_OK(fx_commit(&fx, fx_repo(&fx), "the real commit", &err), &err);

    char mirror_oid[ATLAS_OID_HEX_MAX_INCL];
    T_OK(build_mirror(&fx, 7, mirror_oid, &err), &err);

    atlas_git *g = NULL;
    bool from_mirror = true;
    T_OK(atlas_daemon_open_index_root(fx_data_dir(&fx), 7, fx_repo(&fx), &g, &from_mirror, &err),
         &err);
    T_CHECK_MSG(!from_mirror, "a readable root answers for itself");
    T_REQUIRE(g != NULL);

    atlas_git_head h;
    memset(&h, 0, sizeof(h));
    T_OK(atlas_git_read_head(g, &h, &err), &err);
    T_CHECK_MSG(strcmp(h.oid, mirror_oid) != 0, "and not from the mirror beside it");
    atlas_git_close(g);

    fx_close(&fx);
}

/* Neither readable is the pre-existing failure, unchanged. The fallback's own
 * fallback is the behaviour that shipped before it. */
static void test_no_mirror_still_fails(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);

    atlas_git *g = NULL;
    bool from_mirror = false;
    atlas_status st =
        atlas_daemon_open_index_root(fx_data_dir(&fx), 7, fx_repo(&fx), &g, &from_mirror, &err);
    T_CHECK_MSG(st != ATLAS_OK, "no root and no mirror is still a refusal");
    T_CHECK_MSG(g == NULL, "and nothing is handed back to close");

    fx_close(&fx);
}

static const atlas_test TESTS[] = {
    {"falls back to the mirror", test_falls_back_to_the_mirror},
    {"a readable root is preferred", test_a_readable_root_is_preferred},
    {"no mirror still fails", test_no_mirror_still_fails},
};

ATLAS_TEST_MAIN("mirror_fallback", TESTS)
