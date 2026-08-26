/* Atlas - A13: a repository the daemon cannot open is read from its mirror.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The failure this closes was measured on a live machine: `atlasd` is its own
 * principal, `/opt/atlas` is not readable by it, and the daemon logged
 * "repository atlas cannot be opened" every ten seconds forever. The tree was
 * fine; the reader was the wrong principal.
 *
 * A second uid is not needed to reproduce the *call* that failed. The daemon's
 * refusal came out of `atlas_git_open` on the registered root, and a root that
 * is not a git repository fails there identically. What these tests assert is
 * therefore the fallback's behaviour, not the permission that motivated it.
 */
#include "atlas/error.h"
#include "atlas/git.h"
#include "atlas/mirror.h"
#include "atlas_test.h"
#include "support/fixture.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* A repository row, built by hand. These tests are about which bytes are
 * opened, so the row carries the three fields that decision reads and nothing
 * else: the root to try first, the id the mirror is named by, and the uid whose
 * scanner may have written it. */
static void make_info(atlas_repo_info *info, int64_t id, const char *root, int64_t scanner_uid,
                      atlas_err *err) {
    atlas_repo_info_init(info);
    info->id = id;
    info->scanner_uid = scanner_uid;
    T_OK(atlas_buf_set_str(&info->root_path, root, err), err);
}

/* Builds a real git repository at `<data_dir>/mirror/<id>`, the way the scanner
 * would have left one, and returns its HEAD. */
static atlas_status build_mirror(fixture *fx, int64_t id, char *oid_out, atlas_err *err) {
    atlas_buf path = ATLAS_BUF_INIT;
    atlas_status st = atlas_mirror_repo_path(fx_data_dir(fx), id, &path, err);
    if (st == ATLAS_OK) {
        char parent[2048];
        (void)snprintf(parent, sizeof(parent), "%s/mirror", fx_data_dir(fx));
        (void)fx_mkdir(fx_data_dir(fx), "mirror", err);
        (void)fx_mkdir(parent, atlas_buf_cstr(&path) + strlen(parent) + 1u, err);
    }
    if (st == ATLAS_OK) {
        st = fx_init_repo(fx, atlas_buf_cstr(&path), NULL, err);
    }
    if (st == ATLAS_OK) {
        st = fx_write(atlas_buf_cstr(&path), "mirrored.txt", "from the scanner\n", err);
    }
    if (st == ATLAS_OK) {
        st = fx_add_all(fx, atlas_buf_cstr(&path), err);
    }
    if (st == ATLAS_OK) {
        st = fx_commit(fx, atlas_buf_cstr(&path), "the mirror's commit", err);
    }
    atlas_git *g = NULL;
    if (st == ATLAS_OK) {
        st = atlas_git_open(atlas_buf_cstr(&path), &g, err);
    }
    if (st == ATLAS_OK) {
        atlas_git_head h;
        memset(&h, 0, sizeof(h));
        st = atlas_git_read_head(g, &h, err);
        if (st == ATLAS_OK) {
            (void)snprintf(oid_out, ATLAS_OID_HEX_MAX_INCL, "%s", h.oid);
        }
        atlas_git_close(g);
    }
    atlas_buf_free(&path);
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
    atlas_repo_info info;
    make_info(&info, 7, fx_repo(&fx), 1000, &err);

    atlas_git *g = NULL;
    bool from_mirror = false;
    T_OK(atlas_repo_open_git(&info, fx_data_dir(&fx), &g, &from_mirror, &err), &err);
    T_CHECK_MSG(from_mirror, "the mirror is what answered");
    T_REQUIRE(g != NULL);

    atlas_git_head h;
    memset(&h, 0, sizeof(h));
    T_OK(atlas_git_read_head(g, &h, &err), &err);
    T_CHECK_MSG(strcmp(h.oid, want) == 0, "the mirror's HEAD is what a pass would see");
    atlas_git_close(g);

    atlas_repo_info_free(&info);
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

    atlas_repo_info info;
    make_info(&info, 7, fx_repo(&fx), 1000, &err);

    atlas_git *g = NULL;
    bool from_mirror = true;
    T_OK(atlas_repo_open_git(&info, fx_data_dir(&fx), &g, &from_mirror, &err), &err);
    T_CHECK_MSG(!from_mirror, "a readable root answers for itself");
    T_REQUIRE(g != NULL);

    atlas_git_head h;
    memset(&h, 0, sizeof(h));
    T_OK(atlas_git_read_head(g, &h, &err), &err);
    T_CHECK_MSG(strcmp(h.oid, mirror_oid) != 0, "and not from the mirror beside it");
    atlas_git_close(g);

    atlas_repo_info_free(&info);
    fx_close(&fx);
}

/* A row that names no scanner has no writer for its mirror, so there is nothing
 * there to trust. No reachable path produces such a mirror today — uid 0 is
 * refused at assignment — so this asserts the guard rather than a live defect,
 * and it is the guard that makes "the row still names this writer" true by
 * asking rather than by inference about refusals elsewhere. */
static void test_no_scanner_means_no_mirror(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);

    char oid[ATLAS_OID_HEX_MAX_INCL];
    T_OK(build_mirror(&fx, 7, oid, &err), &err);

    atlas_repo_info info;
    make_info(&info, 7, fx_repo(&fx), 0, &err);

    atlas_git *g = NULL;
    bool from_mirror = false;
    atlas_status st = atlas_repo_open_git(&info, fx_data_dir(&fx), &g, &from_mirror, &err);
    T_CHECK_MSG(st != ATLAS_OK, "a mirror is not consulted for a row naming no scanner");
    T_CHECK_MSG(g == NULL, "and nothing is handed back to close");

    atlas_repo_info_free(&info);
    fx_close(&fx);
}

/* A NULL data directory means the tree itself is the only acceptable source.
 * The guarantee is the absent argument, so a caller that supplies nothing gets
 * the behaviour that shipped before A13 rather than a surprise. */
static void test_null_data_dir_never_consults_a_mirror(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);

    char oid[ATLAS_OID_HEX_MAX_INCL];
    T_OK(build_mirror(&fx, 7, oid, &err), &err);

    atlas_repo_info info;
    make_info(&info, 7, fx_repo(&fx), 1000, &err);

    atlas_git *g = NULL;
    bool from_mirror = false;
    atlas_status st = atlas_repo_open_git(&info, NULL, &g, &from_mirror, &err);
    T_CHECK_MSG(st != ATLAS_OK, "no data directory means no mirror, however good the mirror is");
    T_CHECK_MSG(g == NULL, "and nothing is handed back to close");

    atlas_repo_info_free(&info);
    fx_close(&fx);
}

/* Neither readable is the pre-existing failure, unchanged. */
static void test_no_mirror_still_fails(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);

    atlas_repo_info info;
    make_info(&info, 7, fx_repo(&fx), 1000, &err);

    atlas_git *g = NULL;
    bool from_mirror = false;
    atlas_status st = atlas_repo_open_git(&info, fx_data_dir(&fx), &g, &from_mirror, &err);
    T_CHECK_MSG(st != ATLAS_OK, "no root and no mirror is still a refusal");
    T_CHECK_MSG(g == NULL, "and nothing is handed back to close");

    atlas_repo_info_free(&info);
    fx_close(&fx);
}

static const atlas_test TESTS[] = {
    {"falls back to the mirror", test_falls_back_to_the_mirror},
    {"a readable root is preferred", test_a_readable_root_is_preferred},
    {"no scanner means no mirror", test_no_scanner_means_no_mirror},
    {"a NULL data dir never consults a mirror", test_null_data_dir_never_consults_a_mirror},
    {"no mirror still fails", test_no_mirror_still_fails},
};

ATLAS_TEST_MAIN("mirror_fallback", TESTS)
