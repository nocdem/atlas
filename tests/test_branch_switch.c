/* Atlas - P0: a branch switch, and nothing else, moves the whole index.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Why this suite exists separately from `test_daemon`.
 *
 * `test_watcher_gitignore_and_branch_switch` already checked out a branch and
 * then asserted that a change was observed — but it *wrote a file* after the
 * checkout, so what it proved was that a write after a checkout is noticed. A
 * branch switch on its own writes nothing to the working tree that a watcher
 * would otherwise care about: git rewrites `HEAD`, the index, and whichever
 * files differ between the two trees. Whether Atlas follows that is a different
 * question, and it was not being asked.
 *
 * So every case here performs `git switch` **and nothing else**, and then
 * asserts on the *published* index rather than on an event having arrived.
 *
 * The property that matters most is the one A1 states and nothing tested end to
 * end: a published generation is never a mixture of two branches. The pass
 * re-reads HEAD before it commits and abandons itself if it moved, so a
 * half-switched index should be unreachable — and the way to test that is to
 * assert both halves at once. At the moment the index says it is on the new
 * branch, the old branch's exclusive file must already be gone and the new
 * branch's must already be present. Either alone would pass against a mixture.
 */
#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/datadir.h"
#include "atlas/db.h"
#include "atlas/reconcile.h"
#include "atlas_test.h"
#include "support/fixture.h"

#define WAIT_MS 30000

typedef struct live {
    fixture fx;
    fx_daemon d;
} live;

static void cli(live *L, const char *const *args, size_t n, atlas_buf *out, int *code,
                atlas_err *err) {
    T_OK(fx_atlas_with_runtime(&L->fx, &L->d, args, n, out, NULL, code, err), err);
}

static bool says(const atlas_buf *doc, const char *fragment) {
    return strstr(atlas_buf_cstr(doc), fragment) != NULL;
}

/* `atlas status NAME --json`: the indexed state beside the live git state. */
static void status_doc(live *L, const char *repo, atlas_buf *out, atlas_err *err) {
    const char *args[] = {"status", repo, "--json"};
    int code = -1;
    atlas_buf_reset(out);
    cli(L, args, 3u, out, &code, err);
}

/* `atlas file NAME PATH --json`. `deleted` is the field that says whether the
 * path is part of the currently published tree. */
static void file_doc(live *L, const char *repo, const char *path, atlas_buf *out,
                     atlas_err *err) {
    const char *args[] = {"file", repo, path, "--json"};
    int code = -1;
    atlas_buf_reset(out);
    cli(L, args, 4u, out, &code, err);
}

/* Waits until the *indexed* branch is `branch`.
 *
 * The indexed one, not the live one: git changes the live branch the instant it
 * switches, and the question is whether Atlas followed. The needle is matched
 * inside the `repository` object, which is what the status document reports the
 * index as holding. */
static bool wait_indexed_branch(live *L, const char *repo, const char *branch, atlas_buf *doc,
                                atlas_err *err) {
    char needle[256];
    (void)snprintf(needle, sizeof(needle), "\"branch\":\"%s\"", branch);
    for (int i = 0; i < WAIT_MS / 100; i++) {
        status_doc(L, repo, doc, err);
        const char *p = strstr(atlas_buf_cstr(doc), "\"repository\":");
        const char *live_at = strstr(atlas_buf_cstr(doc), "\"live\":");
        const char *hit = p != NULL ? strstr(p, needle) : NULL;
        /* Only count a match that occurs before the `live` object begins. */
        if (hit != NULL && (live_at == NULL || hit < live_at)) {
            return true;
        }
        usleep(100000);
    }
    return false;
}

static bool wait_file_deleted(live *L, const char *repo, const char *path, bool deleted,
                              atlas_err *err) {
    atlas_buf doc = ATLAS_BUF_INIT;
    const char *needle = deleted ? "\"deleted\":true" : "\"deleted\":false";
    bool ok = false;
    for (int i = 0; i < WAIT_MS / 100; i++) {
        file_doc(L, repo, path, &doc, err);
        if (says(&doc, needle)) {
            ok = true;
            break;
        }
        usleep(100000);
    }
    atlas_buf_free(&doc);
    return ok;
}

static void git(live *L, const char *dir, const char *const *args, size_t n, atlas_err *err) {
    T_OK(fx_git_ok(&L->fx, dir, args, n, err), err);
}

/* Two branches with different file sets, and a daemon watching.
 *
 * `main` holds `only-main.c`; `other` holds `only-other.c`; `shared.c` is on
 * both. The exclusive pair is what makes a mixture detectable. */
static void live_start_two_branches(live *L, atlas_err *err) {
    T_OK(fx_open(&L->fx, err), err);
    const char *repo = fx_repo(&L->fx);
    T_OK(fx_init_repo(&L->fx, repo, NULL, err), err);
    T_OK(fx_write(repo, "shared.c", "int shared;\n", err), err);
    T_OK(fx_write(repo, "only-main.c", "int m;\n", err), err);
    T_OK(fx_add_all(&L->fx, repo, err), err);
    T_OK(fx_commit(&L->fx, repo, "main tree", err), err);

    /* Build `other` from the same base, then return to the starting branch, so
     * the daemon starts on a branch whose exclusive file exists. */
    const char *co[] = {"checkout", "-b", "other"};
    git(L, repo, co, 3u, err);
    const char *rm[] = {"rm", "-q", "only-main.c"};
    git(L, repo, rm, 3u, err);
    T_OK(fx_write(repo, "only-other.c", "int o;\n", err), err);
    T_OK(fx_add_all(&L->fx, repo, err), err);
    T_OK(fx_commit(&L->fx, repo, "other tree", err), err);
    const char *back[] = {"checkout", "-"};
    git(L, repo, back, 2u, err);

    const char *add[] = {"--data-dir", fx_data_dir(&L->fx), "repo", "add", repo, "--name",
                         "fixture"};
    int code = 0;
    T_OK(fx_atlas(add, 7u, NULL, NULL, &code, err), err);
    T_EQ_INT(code, 0);

    fx_daemon_init(&L->d);
    T_OK(fx_daemon_start(&L->fx, &L->d, err), err);
    T_OK(fx_daemon_wait_ready(&L->d, WAIT_MS, err), err);
}

static void live_stop(live *L) {
    fx_daemon_stop(&L->d, false);
    fx_daemon_free(&L->d);
    fx_close(&L->fx);
}

/* Reads the starting branch's name, since git's default differs between
 * installations and hard-coding `master` or `main` would make this test a
 * statement about the machine. */
static void starting_branch(live *L, char *out, size_t n, atlas_err *err) {
    atlas_buf doc = ATLAS_BUF_INIT;
    status_doc(L, "fixture", &doc, err);
    const char *p = strstr(atlas_buf_cstr(&doc), "\"branch\":\"");
    T_REQUIRE(p != NULL);
    p += strlen("\"branch\":\"");
    const char *q = strchr(p, '"');
    T_REQUIRE(q != NULL);
    size_t len = (size_t)(q - p);
    T_REQUIRE(len + 1u <= n);
    memcpy(out, p, len);
    out[len] = '\0';
    atlas_buf_free(&doc);
}

/* --- the switch itself ---------------------------------------------------- */

/* A pure `git switch`: HEAD, the branch and the file index all move, and the
 * published generation is never a mixture of the two trees. */
static void test_a_pure_branch_switch_moves_the_index(void) {
    live L;
    atlas_err err;
    atlas_err_init(&err);
    live_start_two_branches(&L, &err);

    char home[128];
    starting_branch(&L, home, sizeof(home), &err);

    atlas_buf doc = ATLAS_BUF_INIT;
    T_CHECK_MSG(wait_indexed_branch(&L, "fixture", home, &doc, &err),
                "the index must start on the branch git is on");
    T_CHECK(wait_file_deleted(&L, "fixture", "only-main.c", false, &err));

    /* The switch, and nothing else. No file is written after this line. */
    const char *sw[] = {"switch", "other"};
    if (fx_git_ok(&L.fx, fx_repo(&L.fx), sw, 2u, &err) != ATLAS_OK) {
        const char *co[] = {"checkout", "other"};
        git(&L, fx_repo(&L.fx), co, 2u, &err);
    }

    T_CHECK_MSG(wait_indexed_branch(&L, "fixture", "other", &doc, &err),
                "the index must follow a branch switch with no other change");

    /* Both halves, asserted at once. Either alone would pass against an index
     * that was half way between the two trees. */
    T_CHECK_MSG(wait_file_deleted(&L, "fixture", "only-main.c", true, &err),
                "the old branch's exclusive file must not be current after the switch");
    T_CHECK_MSG(wait_file_deleted(&L, "fixture", "only-other.c", false, &err),
                "the new branch's exclusive file must be current after the switch");

    /* And the head the index reports is the head git reports. `head_drift` is
     * Atlas' own comparison of the two, so a false here is the strongest
     * available statement that nothing is half-applied. */
    status_doc(&L, "fixture", &doc, &err);
    T_CHECK_MSG(says(&doc, "\"head_drift\":false"),
                "the indexed head must match the live head after a switch");

    atlas_buf_free(&doc);
    live_stop(&L);
}

/* Switching back leaves no residue from the branch that was visited. */
static void test_switching_back_leaves_no_residue(void) {
    live L;
    atlas_err err;
    atlas_err_init(&err);
    live_start_two_branches(&L, &err);

    char home[128];
    starting_branch(&L, home, sizeof(home), &err);
    atlas_buf doc = ATLAS_BUF_INIT;
    T_CHECK(wait_indexed_branch(&L, "fixture", home, &doc, &err));

    const char *to_other[] = {"checkout", "other"};
    git(&L, fx_repo(&L.fx), to_other, 2u, &err);
    T_CHECK(wait_indexed_branch(&L, "fixture", "other", &doc, &err));
    T_CHECK(wait_file_deleted(&L, "fixture", "only-other.c", false, &err));

    const char *back[] = {"checkout", home};
    git(&L, fx_repo(&L.fx), back, 2u, &err);
    T_CHECK_MSG(wait_indexed_branch(&L, "fixture", home, &doc, &err),
                "the index must follow the switch back");
    T_CHECK_MSG(wait_file_deleted(&L, "fixture", "only-other.c", true, &err),
                "the visited branch's exclusive file must not survive the switch back");
    T_CHECK_MSG(wait_file_deleted(&L, "fixture", "only-main.c", false, &err),
                "the original branch's exclusive file must be current again");

    atlas_buf_free(&doc);
    live_stop(&L);
}

/* A detached HEAD is tracked as its own thing rather than attributed to the
 * branch it was detached from. */
static void test_detached_head_is_tracked_separately(void) {
    live L;
    atlas_err err;
    atlas_err_init(&err);
    live_start_two_branches(&L, &err);

    char home[128];
    starting_branch(&L, home, sizeof(home), &err);
    atlas_buf doc = ATLAS_BUF_INIT;
    T_CHECK(wait_indexed_branch(&L, "fixture", home, &doc, &err));

    const char *detach[] = {"checkout", "--detach", "other"};
    git(&L, fx_repo(&L.fx), detach, 3u, &err);

    bool ok = false;
    for (int i = 0; i < WAIT_MS / 100; i++) {
        status_doc(&L, "fixture", &doc, &err);
        if (says(&doc, "\"head_state\":\"detached\"")) {
            ok = true;
            break;
        }
        usleep(100000);
    }
    T_CHECK_MSG(ok, "a detached HEAD must be reported as detached, not as its former branch");
    /* And the tree it points at is the one indexed. */
    T_CHECK(wait_file_deleted(&L, "fixture", "only-other.c", false, &err));
    T_CHECK(wait_file_deleted(&L, "fixture", "only-main.c", true, &err));

    atlas_buf_free(&doc);
    live_stop(&L);
}

/* A repository with no commits at all is watched, and its first commit is
 * indexed. Unborn is a state, not an error. */
static void test_an_unborn_branch_works(void) {
    live L;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&L.fx, &err), &err);
    const char *repo = fx_repo(&L.fx);
    T_OK(fx_init_repo(&L.fx, repo, NULL, &err), &err);

    const char *add[] = {"--data-dir", fx_data_dir(&L.fx), "repo", "add", repo, "--name",
                         "fixture"};
    int code = 0;
    T_OK(fx_atlas(add, 7u, NULL, NULL, &code, &err), &err);
    T_EQ_INT(code, 0);

    fx_daemon_init(&L.d);
    T_OK(fx_daemon_start(&L.fx, &L.d, &err), &err);
    T_OK(fx_daemon_wait_ready(&L.d, WAIT_MS, &err), &err);

    atlas_buf doc = ATLAS_BUF_INIT;
    bool unborn = false;
    for (int i = 0; i < WAIT_MS / 100; i++) {
        status_doc(&L, "fixture", &doc, &err);
        if (says(&doc, "\"head_state\":\"unborn\"")) {
            unborn = true;
            break;
        }
        usleep(100000);
    }
    T_CHECK_MSG(unborn, "a repository with no commits must be reported as unborn");

    /* And the first commit is picked up. */
    T_OK(fx_write(repo, "first.c", "int f;\n", &err), &err);
    T_OK(fx_add_all(&L.fx, repo, &err), &err);
    T_OK(fx_commit(&L.fx, repo, "first", &err), &err);
    T_CHECK_MSG(wait_file_deleted(&L, "fixture", "first.c", false, &err),
                "the first commit on an unborn branch must be indexed");

    atlas_buf_free(&doc);
    live_stop(&L);
}

/* A history rewrite is detected as one, and answered with a full replay rather
 * than an incremental append from a tip that no longer exists. */
static void test_a_reset_produces_a_branch_rewrite(void) {
    live L;
    atlas_err err;
    atlas_err_init(&err);
    live_start_two_branches(&L, &err);

    char home[128];
    starting_branch(&L, home, sizeof(home), &err);
    atlas_buf doc = ATLAS_BUF_INIT;
    T_CHECK(wait_indexed_branch(&L, "fixture", home, &doc, &err));

    /* One more commit, indexed, then thrown away. */
    T_OK(fx_write(fx_repo(&L.fx), "doomed.c", "int d;\n", &err), &err);
    T_OK(fx_add_all(&L.fx, fx_repo(&L.fx), &err), &err);
    T_OK(fx_commit(&L.fx, fx_repo(&L.fx), "doomed", &err), &err);
    T_CHECK(wait_file_deleted(&L, "fixture", "doomed.c", false, &err));

    const char *reset[] = {"reset", "--hard", "HEAD~1"};
    git(&L, fx_repo(&L.fx), reset, 3u, &err);

    const char *ev[] = {"events", "fixture", "--json", "--limit", "500"};
    bool found = false;
    T_OK(fx_wait_for_substring(&L.fx, &L.d, ev, 5u, "\"kind\":\"branch_rewrite\"", WAIT_MS,
                               &found, &err),
         &err);
    T_CHECK_MSG(found, "a reset that discards a commit must be recorded as a branch rewrite");
    T_CHECK_MSG(wait_file_deleted(&L, "fixture", "doomed.c", true, &err),
                "a file only in the discarded commit must not remain current");

    atlas_buf_free(&doc);
    live_stop(&L);
}

/* P0. A branch switch changes which files git ignores, and nothing in the
 * working tree is written afterwards to trigger a rebuild.
 *
 * The watcher learns about it from the HEAD move, because that is the only
 * signal there is: `.gitignore` differs between the two commits, so switching
 * rewrites it, but a watcher that only rebuilt its ignore inventory when a
 * repository was added or removed would keep applying the old branch's rules
 * indefinitely. */
static void test_branch_switch_lands_on_the_new_branchs_ignore_rules(void) {
    live L;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&L.fx, &err), &err);
    const char *repo = fx_repo(&L.fx);
    T_OK(fx_init_repo(&L.fx, repo, NULL, &err), &err);

    /* On the starting branch, `artifacts/` is ordinary. */
    T_OK(fx_write(repo, ".gitignore", "\n", &err), &err);
    T_OK(fx_mkdir(repo, "artifacts", &err), &err);
    T_OK(fx_write(repo, "artifacts/kept.c", "int k;\n", &err), &err);
    T_OK(fx_add_all(&L.fx, repo, &err), &err);
    T_OK(fx_commit(&L.fx, repo, "artifacts are ordinary", &err), &err);

    /* On `ignoring`, it is not. */
    const char *co[] = {"checkout", "-b", "ignoring"};
    git(&L, repo, co, 3u, &err);
    T_OK(fx_write(repo, ".gitignore", "artifacts/\n", &err), &err);
    const char *rm[] = {"rm", "-r", "-q", "--cached", "artifacts"};
    git(&L, repo, rm, 5u, &err);
    T_OK(fx_add_all(&L.fx, repo, &err), &err);
    T_OK(fx_commit(&L.fx, repo, "artifacts are ignored", &err), &err);
    const char *back[] = {"checkout", "-"};
    git(&L, repo, back, 2u, &err);

    const char *add[] = {"--data-dir", fx_data_dir(&L.fx), "repo", "add", repo, "--name",
                         "fixture"};
    int code = 0;
    T_OK(fx_atlas(add, 7u, NULL, NULL, &code, &err), &err);
    T_EQ_INT(code, 0);
    fx_daemon_init(&L.d);
    T_OK(fx_daemon_start(&L.fx, &L.d, &err), &err);
    T_OK(fx_daemon_wait_ready(&L.d, WAIT_MS, &err), &err);

    T_CHECK_MSG(wait_file_deleted(&L, "fixture", "artifacts/kept.c", false, &err),
                "the tracked file must be indexed on the branch that tracks it");

    /* The switch, and nothing else. */
    const char *sw[] = {"checkout", "ignoring"};
    git(&L, repo, sw, 2u, &err);

    atlas_buf doc = ATLAS_BUF_INIT;
    T_CHECK_MSG(wait_indexed_branch(&L, "fixture", "ignoring", &doc, &err),
                "the index must follow the switch");
    /* The file is no longer tracked on this branch, so it must not be current —
     * and it is only reachable through the ignore rules the *new* branch
     * carries. */
    T_CHECK_MSG(wait_file_deleted(&L, "fixture", "artifacts/kept.c", true, &err),
                "after switching to a branch that ignores the tree, its file must not be current");

    atlas_buf_free(&doc);
    live_stop(&L);
}

/* --- linked worktrees ------------------------------------------------------ */

/* Two registered worktrees of one repository keep independent branch, HEAD and
 * file index, and a branch switch in one does not move the other.
 *
 * They share a git common directory, so they share the watch descriptors on it —
 * which is exactly the case the old single-owner watch map got wrong, silently
 * delivering every shared-ref event to whichever worktree was registered last. */
static void test_two_worktrees_are_independent_across_a_switch(void) {
    live L;
    atlas_err err;
    atlas_err_init(&err);
    live_start_two_branches(&L, &err);

    char home[128];
    starting_branch(&L, home, sizeof(home), &err);

    atlas_buf wt = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&wt, &err, "%s/wt2", atlas_buf_cstr(&L.fx.root)), &err);
    const char *addwt[] = {"worktree", "add", atlas_buf_cstr(&wt), "other"};
    atlas_err werr;
    atlas_err_init(&werr);
    if (fx_git_ok(&L.fx, fx_repo(&L.fx), addwt, 4u, &werr) != ATLAS_OK) {
        atlas_test_note("this git cannot create a linked worktree; skipping");
        atlas_buf_free(&wt);
        live_stop(&L);
        return;
    }

    /* Registration takes the write lock, so the daemon is stopped for it. */
    fx_daemon_stop(&L.d, false);
    const char *reg[] = {"--data-dir", fx_data_dir(&L.fx), "repo", "add", atlas_buf_cstr(&wt),
                         "--name", "second"};
    int code = -1;
    T_OK(fx_atlas(reg, 7u, NULL, NULL, &code, &err), &err);
    T_EQ_INT(code, 0);
    T_OK(fx_daemon_start(&L.fx, &L.d, &err), &err);
    T_OK(fx_daemon_wait_ready(&L.d, WAIT_MS, &err), &err);

    atlas_buf doc = ATLAS_BUF_INIT;
    T_CHECK_MSG(wait_indexed_branch(&L, "fixture", home, &doc, &err),
                "the first worktree starts on its own branch");
    T_CHECK_MSG(wait_indexed_branch(&L, "second", "other", &doc, &err),
                "the second worktree starts on its own branch");

    /* Each holds its own exclusive file, and not the other's. */
    T_CHECK(wait_file_deleted(&L, "fixture", "only-main.c", false, &err));
    T_CHECK(wait_file_deleted(&L, "second", "only-other.c", false, &err));

    /* A third branch, switched to in the *second* worktree only. */
    const char *mk[] = {"switch", "-c", "third"};
    if (fx_git_ok(&L.fx, atlas_buf_cstr(&wt), mk, 3u, &err) != ATLAS_OK) {
        const char *co[] = {"checkout", "-b", "third"};
        git(&L, atlas_buf_cstr(&wt), co, 3u, &err);
    }

    T_CHECK_MSG(wait_indexed_branch(&L, "second", "third", &doc, &err),
                "the worktree that switched must follow");
    /* And the other one did not move. Read after the first has settled, so this
     * is not merely a race that has not resolved yet. */
    status_doc(&L, "fixture", &doc, &err);
    char needle[192];
    (void)snprintf(needle, sizeof(needle), "\"branch\":\"%s\"", home);
    T_CHECK_MSG(says(&doc, needle),
                "a branch switch in one worktree must not move another worktree's index");
    T_CHECK_MSG(!says(&doc, "\"branch\":\"third\""),
                "one worktree's branch must never appear in another's index state");

    atlas_buf_free(&doc);
    atlas_buf_free(&wt);
    live_stop(&L);
}

/* A shared ref event reaches both worktrees, and removing one leaves the other
 * still receiving them.
 *
 * The refs live in the common directory, so both worktrees subscribe to the same
 * physical descriptors. Under the old map the second registration took them over
 * outright; removing either then released a descriptor the survivor was relying
 * on, and the survivor went quiet without anything recording that it had. */
static void test_a_surviving_worktree_still_follows_its_branch(void) {
    live L;
    atlas_err err;
    atlas_err_init(&err);
    live_start_two_branches(&L, &err);

    char home[128];
    starting_branch(&L, home, sizeof(home), &err);

    atlas_buf wt = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&wt, &err, "%s/wt2", atlas_buf_cstr(&L.fx.root)), &err);
    const char *addwt[] = {"worktree", "add", atlas_buf_cstr(&wt), "other"};
    atlas_err werr;
    atlas_err_init(&werr);
    if (fx_git_ok(&L.fx, fx_repo(&L.fx), addwt, 4u, &werr) != ATLAS_OK) {
        atlas_test_note("this git cannot create a linked worktree; skipping");
        atlas_buf_free(&wt);
        live_stop(&L);
        return;
    }
    fx_daemon_stop(&L.d, false);
    const char *reg[] = {"--data-dir", fx_data_dir(&L.fx), "repo", "add", atlas_buf_cstr(&wt),
                         "--name", "second"};
    int code = -1;
    T_OK(fx_atlas(reg, 7u, NULL, NULL, &code, &err), &err);
    T_EQ_INT(code, 0);
    T_OK(fx_daemon_start(&L.fx, &L.d, &err), &err);
    T_OK(fx_daemon_wait_ready(&L.d, WAIT_MS, &err), &err);

    atlas_buf doc = ATLAS_BUF_INIT;
    T_CHECK(wait_indexed_branch(&L, "fixture", home, &doc, &err));
    T_CHECK(wait_indexed_branch(&L, "second", "other", &doc, &err));

    /* Remove the second registration. The first must go on following its own
     * branch afterwards — which it can only do if it still holds the metadata
     * watches it shared with the one that left. */
    fx_daemon_stop(&L.d, false);
    const char *rm[] = {"--data-dir", fx_data_dir(&L.fx), "repo", "remove", "second", "--yes"};
    T_OK(fx_atlas(rm, 6u, NULL, NULL, &code, &err), &err);
    T_EQ_INT(code, 0);
    T_OK(fx_daemon_start(&L.fx, &L.d, &err), &err);
    T_OK(fx_daemon_wait_ready(&L.d, WAIT_MS, &err), &err);
    T_CHECK(wait_indexed_branch(&L, "fixture", home, &doc, &err));

    /* And the worktree itself goes, not only its registration — which is what an
     * operator finishing with a worktree actually does, and what frees `other`
     * for the surviving worktree to check out. Atlas never removes a worktree;
     * the test does, as the operator would. */
    const char *wtrm[] = {"worktree", "remove", "--force", atlas_buf_cstr(&wt)};
    git(&L, fx_repo(&L.fx), wtrm, 4u, &err);

    /* The switch, in the surviving worktree, with nothing else written. */
    const char *sw[] = {"checkout", "other"};
    git(&L, fx_repo(&L.fx), sw, 2u, &err);
    T_CHECK_MSG(wait_indexed_branch(&L, "fixture", "other", &doc, &err),
                "the surviving worktree must still follow a branch switch after its sibling was "
                "removed");
    T_CHECK_MSG(wait_file_deleted(&L, "fixture", "only-main.c", true, &err),
                "and its index must move with it");

    atlas_buf_free(&doc);
    atlas_buf_free(&wt);
    live_stop(&L);
}

/* --- a branch that moves while a pass is in flight -------------------------- */

/* What the barrier is for.
 *
 * A1's rule is that a pass re-reads HEAD before it commits and **abandons**
 * itself if HEAD moved, so a published generation is never a mixture of two
 * branches. The window that rule protects is between the last hash and that
 * re-read, and it is microseconds wide: racing a real daemon into it is a coin
 * toss, not a test, which is why this property has never had one.
 *
 * `atlas_reconcile_opts.before_head_recheck` is a function pointer that is NULL
 * in production and is set by nothing a user can reach — no CLI flag, no
 * environment variable, no IPC field, no policy key. With it NULL the pass is
 * the shipped one. Here it switches the branch, from inside the window, on the
 * pass's own thread. The pass then re-reads HEAD, sees it moved, and must
 * abandon rather than publish. */
typedef struct flip_ctx {
    fixture *fx;
    const char *repo;
    const char *to;
    int fired;
} flip_ctx;

static void flip_branch(void *ud) {
    flip_ctx *c = (flip_ctx *)ud;
    if (c->fired++ > 0) {
        return; /* once: the retry must be allowed to succeed */
    }
    atlas_err err;
    atlas_err_init(&err);
    const char *co[] = {"checkout", c->to};
    T_OK(fx_git_ok(c->fx, c->repo, co, 2u, &err), &err);
}

static void test_a_branch_switch_during_a_pass_is_abandoned_not_published(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);
    const char *repo = fx_repo(&fx);
    T_OK(fx_init_repo(&fx, repo, NULL, &err), &err);
    T_OK(fx_write(repo, "shared.c", "int shared;\n", &err), &err);
    T_OK(fx_write(repo, "only-main.c", "int m;\n", &err), &err);
    T_OK(fx_add_all(&fx, repo, &err), &err);
    T_OK(fx_commit(&fx, repo, "main tree", &err), &err);

    const char *co[] = {"checkout", "-b", "other"};
    T_OK(fx_git_ok(&fx, repo, co, 3u, &err), &err);
    const char *rm[] = {"rm", "-q", "only-main.c"};
    T_OK(fx_git_ok(&fx, repo, rm, 3u, &err), &err);
    T_OK(fx_write(repo, "only-other.c", "int o;\n", &err), &err);
    T_OK(fx_add_all(&fx, repo, &err), &err);
    T_OK(fx_commit(&fx, repo, "other tree", &err), &err);
    const char *back[] = {"checkout", "-"};
    T_OK(fx_git_ok(&fx, repo, back, 2u, &err), &err);

    atlas_buf db_path = ATLAS_BUF_INIT;
    atlas_db *db = NULL;
    atlas_git *g = NULL;
    T_OK(atlas_datadir_ensure(fx_data_dir(&fx), &err), &err);
    T_OK(atlas_datadir_db_path(fx_data_dir(&fx), &db_path, &err), &err);
    T_OK(atlas_db_open(atlas_buf_cstr(&db_path), &db, &err), &err);
    T_OK(atlas_db_migrate(db, &err), &err);
    T_OK(atlas_git_open(repo, &g, &err), &err);

    atlas_repo_identity id;
    memset(&id, 0, sizeof(id));
    const char *root = atlas_git_root(g);
    id.root = root;
    id.root_len = strlen(root);
    id.common_dir = atlas_git_common_dir(g);
    id.common_dir_len = strlen((const char *)id.common_dir);
    id.git_dir = atlas_git_dir(g);
    id.git_dir_len = strlen((const char *)id.git_dir);
    id.object_format = atlas_git_object_format(g);
    int64_t repo_id = 0;
    T_OK(atlas_db_repo_add(db, "fixture", &id, &repo_id, &err), &err);

    /* A clean pass first, so there is a published generation to compare against
     * and so the abandoned one cannot be confused with "nothing had run yet". */
    {
        atlas_reconcile_opts opts;
        atlas_reconcile_opts_init(&opts);
        opts.full = true;
        atlas_reconcile_summary sum;
        atlas_reconcile_summary_init(&sum);
        T_OK(atlas_reconcile_run(db, g, repo_id, &opts, &sum, &err), &err);
        T_CHECK_MSG(sum.published, "the first pass must publish");
        atlas_reconcile_summary_free(&sum);
    }
    atlas_index_state before;
    atlas_index_state_init(&before);
    T_OK(atlas_db_index_state_get(db, repo_id, &before, &err), &err);
    int64_t published_before = before.last_complete_generation;

    /* Now a pass whose branch moves inside the window. */
    flip_ctx c = {&fx, repo, "other", 0};
    atlas_reconcile_opts opts;
    atlas_reconcile_opts_init(&opts);
    opts.full = true;
    opts.before_head_recheck = flip_branch;
    opts.before_head_recheck_ud = &c;
    atlas_reconcile_summary sum;
    atlas_reconcile_summary_init(&sum);
    T_OK(atlas_reconcile_run(db, g, repo_id, &opts, &sum, &err), &err);

    T_CHECK_MSG(c.fired == 1, "the barrier must have fired exactly once, fired %d", c.fired);
    T_CHECK_MSG(!sum.published,
                "a pass whose HEAD moved before the commit must be abandoned, not published");

    /* And nothing was published: the stored generation did not move, so no
     * reader can have seen a tree that is half one branch and half the other. */
    atlas_index_state after;
    atlas_index_state_init(&after);
    T_OK(atlas_db_index_state_get(db, repo_id, &after, &err), &err);
    T_CHECK_MSG(after.last_complete_generation == published_before,
                "an abandoned pass must not move the published generation: %lld -> %lld",
                (long long)published_before, (long long)after.last_complete_generation);

    /* The retry — which is what the daemon does — succeeds and lands wholly on
     * the new branch. Bounded: the barrier fires once, so this cannot spin. */
    atlas_reconcile_opts again;
    atlas_reconcile_opts_init(&again);
    again.full = true;
    atlas_reconcile_summary sum2;
    atlas_reconcile_summary_init(&sum2);
    T_OK(atlas_reconcile_run(db, g, repo_id, &again, &sum2, &err), &err);
    T_CHECK_MSG(sum2.published, "the retry after an abandoned pass must publish");
    T_CHECK_MSG(strcmp(sum2.branch, "other") == 0,
                "and must describe the branch git is actually on, got %s", sum2.branch);

    atlas_reconcile_summary_free(&sum);
    atlas_reconcile_summary_free(&sum2);
    atlas_index_state_free(&before);
    atlas_index_state_free(&after);
    atlas_git_close(g);
    atlas_db_close(db);
    atlas_buf_free(&db_path);
    fx_close(&fx);
}

static const atlas_test TESTS[] = {
    {"a pure branch switch moves HEAD, the branch and the file index",
     test_a_pure_branch_switch_moves_the_index},
    {"switching back leaves no residue from the branch that was visited",
     test_switching_back_leaves_no_residue},
    {"a detached HEAD is tracked separately", test_detached_head_is_tracked_separately},
    {"an unborn branch works and its first commit is indexed", test_an_unborn_branch_works},
    {"a reset that discards a commit produces a branch rewrite",
     test_a_reset_produces_a_branch_rewrite},
    {"a branch switch lands on the new branch's ignore rules",
     test_branch_switch_lands_on_the_new_branchs_ignore_rules},
    {"two worktrees keep independent branches across a switch",
     test_two_worktrees_are_independent_across_a_switch},
    {"a branch switch during a pass is abandoned, never published",
     test_a_branch_switch_during_a_pass_is_abandoned_not_published},
    {"a surviving worktree still follows its branch after its sibling is removed",
     test_a_surviving_worktree_still_follows_its_branch},
};

ATLAS_TEST_MAIN("branch_switch", TESTS)
