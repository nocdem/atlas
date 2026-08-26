/* Atlas - A13: a repository the daemon cannot open is read from its mirror.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The rule is that the repository row decides where its bytes are read from: a
 * repository naming a scanner is read from its mirror and from nothing else,
 * and one naming none is read from its tree. There is no fallback in either
 * direction, and these tests exist to keep it that way.
 *
 * The first design did have a fallback — tree first, mirror on failure — and
 * the machine this season was built on is what refuted it. Both real failures
 * there were partial: 100 loose objects at mode 0400, so the open succeeded and
 * `git log` failed later; and 50 private directories that could not be entered,
 * so every pass completed and covered less than the tree. A rule keyed on
 * "could not open" answers neither.
 */
#include "atlas/error.h"
#include "atlas/git.h"
#include "atlas/mirror.h"
#include "core/service_internal.h"
#include "atlas_test.h"
#include "support/fixture.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* A scanner uid that is deliberately *not* this process, so these tests stand
 * where the daemon stands: a principal that may not read the tree. Passing the
 * running uid would exercise the scanner's own path instead, which is the one
 * case that still reads the tree. */
static int64_t not_me(void) { return (int64_t)geteuid() + 1; }

/* A repository row, built by hand. These tests are about which bytes are
 * opened, so the row carries the three fields that decision reads and nothing
 * else: the root to try first, the id the mirror is named by, and the uid whose
 * scanner may have written it. */
static void make_info(atlas_repo_info *info, int64_t id, const char *root, int64_t scanner_uid,
                      atlas_err *err) {
    atlas_repo_info_init(info);
    info->id = id;
    info->scanner_uid = scanner_uid;
    /* The ordinary case: a run finished and skipped nothing. The test that
     * asserts an incomplete mirror is refused clears this deliberately. */
    info->mirror_complete = true;
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

/* A repository naming a scanner is read from its mirror. */
static void test_a_scanner_backed_repo_reads_its_mirror(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);

    char want[ATLAS_OID_HEX_MAX_INCL];
    T_OK(build_mirror(&fx, 7, want, &err), &err);

    /* `fx_repo` exists but was never `git init`ed here, so `atlas_git_open`
     * refuses it exactly as it refused the unreadable tree. */
    atlas_repo_info info;
    make_info(&info, 7, fx_repo(&fx), not_me(), &err);

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

/* **A readable tree is ignored when the row names a scanner.**
 *
 * This is the case that inverted: the first design preferred the tree whenever
 * it opened, which is precisely why it answered neither real failure. Naming a
 * scanner is an instruction to stop reading the tree, not a hint to try it
 * first. */
static void test_a_readable_root_is_ignored(void) {
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
    make_info(&info, 7, fx_repo(&fx), not_me(), &err);

    atlas_git *g = NULL;
    bool from_mirror = true;
    T_OK(atlas_repo_open_git(&info, fx_data_dir(&fx), &g, &from_mirror, &err), &err);
    T_CHECK_MSG(from_mirror, "the mirror answers even though the tree opens");
    T_REQUIRE(g != NULL);

    atlas_git_head h;
    memset(&h, 0, sizeof(h));
    T_OK(atlas_git_read_head(g, &h, &err), &err);
    T_CHECK_MSG(strcmp(h.oid, mirror_oid) == 0, "and it is the mirror's HEAD, not the tree's");
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
    make_info(&info, 7, fx_repo(&fx), not_me(), &err);

    atlas_git *g = NULL;
    bool from_mirror = false;
    atlas_status st = atlas_repo_open_git(&info, NULL, &g, &from_mirror, &err);
    T_CHECK_MSG(st != ATLAS_OK, "no data directory means no mirror, however good the mirror is");
    T_CHECK_MSG(g == NULL, "and nothing is handed back to close");

    atlas_repo_info_free(&info);
    fx_close(&fx);
}

/* **A named scanner with no mirror yet is refused, not read from its tree.**
 *
 * The tree here is a perfectly good git repository, so a fallback would find it
 * and succeed. That is the failure this asserts against: an operator who names
 * a scanner has said "stop reading this tree", and a mirror that does not exist
 * yet is a reason to wait, never a reason to go back. */
static void test_a_named_scanner_without_a_mirror_refuses(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);

    /* A real, readable, openable repository at the registered root. */
    T_OK(fx_init_repo(&fx, fx_repo(&fx), NULL, &err), &err);
    T_OK(fx_write(fx_repo(&fx), "real.txt", "the tree itself\n", &err), &err);
    T_OK(fx_add_all(&fx, fx_repo(&fx), &err), &err);
    T_OK(fx_commit(&fx, fx_repo(&fx), "the real commit", &err), &err);

    atlas_repo_info info;
    make_info(&info, 7, fx_repo(&fx), not_me(), &err);

    atlas_git *g = NULL;
    bool from_mirror = false;
    atlas_status st = atlas_repo_open_git(&info, fx_data_dir(&fx), &g, &from_mirror, &err);
    T_CHECK_MSG(st != ATLAS_OK, "a named scanner with no mirror refuses rather than reading the tree");
    T_CHECK_MSG(g == NULL, "and nothing is handed back to close");

    atlas_repo_info_free(&info);
    fx_close(&fx);
}

/* The shared helper enforces two identity checks after opening: the registered
 * root must still resolve to itself, and the git dir must be the one recorded.
 * A mirror fails both by construction. Those checks are claims about the real
 * tree, and when the mirror answered the real tree was not opened -- so the
 * claim is not false, it is unasked, and asserting it would refuse a correct
 * answer. This asserts the *status*, because ATLAS_ERR_INTEGRITY is exactly
 * what the bug looks like. */
static void test_shared_helper_accepts_a_mirror(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);

    char want[ATLAS_OID_HEX_MAX_INCL];
    T_OK(build_mirror(&fx, 7, want, &err), &err);

    atlas_repo_info info;
    make_info(&info, 7, fx_repo(&fx), not_me(), &err);

    atlas_git *g = NULL;
    atlas_status st = atlas_service_open_repo_git(&info, fx_data_dir(&fx), &g, &err);
    T_CHECK_MSG(st != ATLAS_ERR_INTEGRITY,
                "the canonical-root check is unasked when the mirror answered");
    T_OK(st, &err);
    T_REQUIRE(g != NULL);

    atlas_git_head h;
    memset(&h, 0, sizeof(h));
    T_OK(atlas_git_read_head(g, &h, &err), &err);
    T_CHECK_MSG(strcmp(h.oid, want) == 0, "and the mirror is what it opened");
    atlas_git_close(g);

    atlas_repo_info_free(&info);
    fx_close(&fx);
}

/* The real root's *status* survives, not only its message.
 *
 * `atlas_git_open` refuses a partial (promisor) repository with an integrity
 * status, and a caller that saw that collapsed into a plain repository error
 * would read "not found" where Atlas meant "refused, and deliberately". This
 * is a regression test: flattening the status turned `test_git_hardening`'s
 * rescan case from 7 into 4, and that suite caught it when this one did not. */
static void test_the_real_roots_status_survives(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);

    /* A path that does not exist at all, so `atlas_git_open` fails with its own
     * status rather than one this test chose. */
    char missing[2048];
    (void)snprintf(missing, sizeof(missing), "%s/nowhere", fx_repo(&fx));

    atlas_git *probe = NULL;
    atlas_err direct;
    atlas_err_init(&direct);
    atlas_status want = atlas_git_open(missing, &probe, &direct);
    T_REQUIRE(want != ATLAS_OK);

    atlas_repo_info info;
    make_info(&info, 7, missing, not_me(), &err);

    atlas_git *g = NULL;
    atlas_status got = atlas_repo_open_git(&info, fx_data_dir(&fx), &g, NULL, &err);
    T_CHECK_MSG(got == want, "the fallback reports the real root's own status");
    T_CHECK_MSG(g == NULL, "and nothing is handed back to close");

    atlas_repo_info_free(&info);
    fx_close(&fx);
}

/* **An incomplete mirror is refused, not read.**
 *
 * The daemon reads the mirror as the repository, so every file the mirror does
 * not hold is a file that no longer exists. Measured on the first live run: a
 * mirror carrying 2007 of a tree's 22012 files produced 20000 deletions. The
 * mirror here is a perfectly good git repository -- it is the *claim about it*
 * that is missing, and that alone must be enough to refuse. */
static void test_an_incomplete_mirror_is_refused(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);

    char oid[ATLAS_OID_HEX_MAX_INCL];
    T_OK(build_mirror(&fx, 7, oid, &err), &err);

    atlas_repo_info info;
    make_info(&info, 7, fx_repo(&fx), not_me(), &err);
    info.mirror_complete = false;

    atlas_git *g = NULL;
    bool from_mirror = false;
    atlas_status st = atlas_repo_open_git(&info, fx_data_dir(&fx), &g, &from_mirror, &err);
    T_CHECK_MSG(st != ATLAS_OK, "a mirror no run has vouched for is not read");
    T_CHECK_MSG(g == NULL, "and nothing is handed back to close");

    atlas_repo_info_free(&info);
    fx_close(&fx);
}

static const atlas_test TESTS[] = {
    {"a scanner-backed repo reads its mirror", test_a_scanner_backed_repo_reads_its_mirror},
    {"a readable root is ignored", test_a_readable_root_is_ignored},
    {"no scanner means no mirror", test_no_scanner_means_no_mirror},
    {"a NULL data dir never consults a mirror", test_null_data_dir_never_consults_a_mirror},
    {"a named scanner without a mirror refuses", test_a_named_scanner_without_a_mirror_refuses},
    {"an incomplete mirror is refused", test_an_incomplete_mirror_is_refused},
    {"the shared helper accepts a mirror", test_shared_helper_accepts_a_mirror},
    {"the real root's status survives", test_the_real_roots_status_survives},
};

ATLAS_TEST_MAIN("mirror_source", TESTS)
