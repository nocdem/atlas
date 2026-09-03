/* Atlas - A12.1 T6: reading a registered memory source, by the principal that
 * can actually read it.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * `atlas_memory_read_source` and `atlas_memory_read_external` turn a
 * registered source (a REPO_FILE, REPO_DIR, EXTERNAL_FILE or EXTERNAL_DIR)
 * into the bytes it names. These tests build one real git repository -- a
 * tracked `CLAUDE.md` and an untracked `.claude/memories/` directory -- and
 * read both repository classes against it, plus the external and symlink
 * paths a repository row has nothing to do with.
 *
 * `fx_tree_digest` brackets every test that touches the repository: a reader
 * that modifies what it reads is the one failure this codebase will not
 * tolerate (CLAUDE.md, "Hard rules").
 *
 * T11 fix round, item 6: `#define _GNU_SOURCE 1` used to be here for
 * `usleep` in `t11_wait_for_generation`, which is hoisted into
 * `tests/support/reconcile_env.c` now (see its own `_GNU_SOURCE`). Nothing
 * left in this file needs it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

#include "atlas/atlas.h"
#include "atlas/datadir.h"
#include "atlas/db.h"
#include "atlas/decision.h"
#include "atlas/decision_ops.h"
#include "atlas/memory.h"
#include "atlas/mirror.h"
#include "atlas/pathrep.h"
#include "atlas/sem.h"
#include "atlas/sha256.h"
#include "atlas/syspolicy.h"
#include "atlas/verify.h"
#include "atlas/verify_ops.h"
#include "atlas/verifypolicy.h"
#include "atlas_test.h"
#include "daemon/daemon_internal.h"
#include "db/db_internal.h"
#include "ipc/server_internal.h"
#include "support/fixture.h"
#include "support/reconcile_env.h"

/* A row naming no scanner -- scanner_uid == 0, migration 27's default -- so
 * atlas_repo_open_git reads the tree directly. A13's mirror routing is
 * test_mirror_source.c's own territory and is not restated here; this file is
 * about what a read returns once A13 has already decided where it comes
 * from. */
static void make_info(atlas_repo_info *info, const char *root, atlas_err *err) {
    atlas_repo_info_init(info);
    info->id = 1;
    T_OK(atlas_buf_set_str(&info->root_path, root, err), err);
}

/* A tracked CLAUDE.md and an untracked .claude/memories/a.md: the one fixture
 * shape most tests in this file share. */
static void build_repo(fixture *fx, atlas_err *err) {
    const char *repo = fx_repo(fx);
    T_OK(fx_init_repo(fx, repo, NULL, err), err);
    T_OK(fx_write(repo, "CLAUDE.md", "tracked memory\n", err), err);
    T_OK(fx_add_all(fx, repo, err), err);
    T_OK(fx_commit(fx, repo, "initial commit", err), err);
    T_OK(fx_mkdir(repo, ".claude", err), err);
    T_OK(fx_mkdir(repo, ".claude/memories", err), err);
    T_OK(fx_write(repo, ".claude/memories/a.md", "untracked note\n", err), err);
}

/* --- REPO_FILE -------------------------------------------------------------- */

static void test_repo_file_reads_tracked_bytes(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    build_repo(&fx, &err);

    char before[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(fx_repo(&fx), before, &err), &err);

    atlas_repo_info info;
    make_info(&info, fx_repo(&fx), &err);

    atlas_memory_read_item item;
    size_t count = 0;
    bool from_mirror = true;
    T_OK(atlas_memory_read_source(&info, fx_data_dir(&fx), ATLAS_MEMORY_SOURCE_REPO_FILE,
                                  "CLAUDE.md", strlen("CLAUDE.md"), &item, 1u, &count,
                                  &from_mirror, &err),
         &err);
    T_CHECK_MSG(count == 1u, "expected exactly one item, got %zu", count);
    T_CHECK_MSG(!from_mirror, "a tree-direct read must not report from_mirror");
    T_CHECK_MSG(item.outcome == ATLAS_MEMORY_READ_OK, "expected OK, got %d", (int)item.outcome);
    T_CHECK_MSG(item.bytes.len == strlen("tracked memory\n") &&
                    memcmp(item.bytes.data, "tracked memory\n", item.bytes.len) == 0,
                "tracked content came back wrong");
    T_CHECK_MSG(item.blob_oid.len > 0, "a tracked file's blob_oid is empty");
    T_CHECK_MSG(item.commit_oid.len > 0, "a tracked file's commit_oid is empty");
    T_CHECK_MSG(item.rel_path.len == 0, "a FILE source's rel_path should be empty");

    atlas_memory_read_item_free(&item);
    atlas_repo_info_free(&info);

    char after[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(fx_repo(&fx), after, &err), &err);
    T_CHECK_MSG(strcmp(before, after) == 0, "reading a memory source modified the repository");

    fx_close(&fx);
}

static void test_repo_file_missing_path_is_absent(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    build_repo(&fx, &err);

    char before[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(fx_repo(&fx), before, &err), &err);

    atlas_repo_info info;
    make_info(&info, fx_repo(&fx), &err);

    atlas_memory_read_item item;
    size_t count = 0;
    bool from_mirror = true;
    T_OK(atlas_memory_read_source(&info, fx_data_dir(&fx), ATLAS_MEMORY_SOURCE_REPO_FILE,
                                  "no-such-file.md", strlen("no-such-file.md"), &item, 1u, &count,
                                  &from_mirror, &err),
         &err);
    T_CHECK_MSG(count == 1u, "expected exactly one item, got %zu", count);
    T_CHECK_MSG(!from_mirror, "a tree-direct read must not report from_mirror");
    T_CHECK_MSG(item.outcome == ATLAS_MEMORY_READ_ABSENT, "expected ABSENT, got %d",
                (int)item.outcome);
    T_CHECK_MSG(item.bytes.len == 0, "an absent path returned bytes");

    atlas_memory_read_item_free(&item);
    atlas_repo_info_free(&info);

    char after[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(fx_repo(&fx), after, &err), &err);
    T_CHECK_MSG(strcmp(before, after) == 0, "reading a missing path modified the repository");

    fx_close(&fx);
}

static void test_repo_file_over_the_bound_is_too_large(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    build_repo(&fx, &err);

    /* One byte over ATLAS_MEMORY_MAX_SOURCE_BYTES, untracked: the bound is
     * checked before git is ever asked about the path. */
    size_t n = (size_t)ATLAS_MEMORY_MAX_SOURCE_BYTES + 1u;
    char *big = malloc(n);
    T_REQUIRE(big != NULL);
    memset(big, 'x', n);
    T_OK(fx_write_bytes(fx_repo(&fx), "big.md", strlen("big.md"), big, n, 0644, &err), &err);
    free(big);

    char before[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(fx_repo(&fx), before, &err), &err);

    atlas_repo_info info;
    make_info(&info, fx_repo(&fx), &err);

    atlas_memory_read_item item;
    size_t count = 0;
    bool from_mirror = true;
    T_OK(atlas_memory_read_source(&info, fx_data_dir(&fx), ATLAS_MEMORY_SOURCE_REPO_FILE, "big.md",
                                  strlen("big.md"), &item, 1u, &count, &from_mirror, &err),
         &err);
    T_CHECK_MSG(count == 1u, "expected exactly one item, got %zu", count);
    T_CHECK_MSG(!from_mirror, "a tree-direct read must not report from_mirror");
    T_CHECK_MSG(item.outcome == ATLAS_MEMORY_READ_TOO_LARGE, "expected TOO_LARGE, got %d",
                (int)item.outcome);
    T_CHECK_MSG(item.bytes.len == 0, "an oversized source returned bytes");

    atlas_memory_read_item_free(&item);
    atlas_repo_info_free(&info);

    char after[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(fx_repo(&fx), after, &err), &err);
    T_CHECK_MSG(strcmp(before, after) == 0, "reading an oversized path modified the repository");

    fx_close(&fx);
}

/* A row naming a scanner that is not this process, with no mirror vouched for
 * -- test_mirror_source.c's own "an incomplete mirror is refused" shape, one
 * layer up. atlas_repo_open_git refuses this outright; atlas_memory_read_source
 * must turn that refusal into ATLAS_MEMORY_READ_NO_MIRROR rather than propagate
 * it as a status failure, and must leave `err` clean when it does -- this is
 * the one outcome that converts an error into a result rather than reading it
 * off a filesystem check, so it earns its own test. */
static void test_repo_file_no_mirror_reports_the_outcome(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    build_repo(&fx, &err);

    char before[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(fx_repo(&fx), before, &err), &err);

    atlas_repo_info info;
    make_info(&info, fx_repo(&fx), &err);
    info.scanner_uid = (int64_t)geteuid() + 1; /* deliberately not this process */
    info.mirror_complete = false;

    atlas_memory_read_item item;
    size_t count = 0;
    bool from_mirror = true;
    atlas_status st = atlas_memory_read_source(&info, fx_data_dir(&fx),
                                               ATLAS_MEMORY_SOURCE_REPO_FILE, "CLAUDE.md",
                                               strlen("CLAUDE.md"), &item, 1u, &count,
                                               &from_mirror, &err);
    T_CHECK_MSG(st == ATLAS_OK, "expected ATLAS_OK even with no mirror, got %s: %s",
                atlas_status_name(st), atlas_err_msg(&err));
    T_CHECK_MSG(count == 1u, "expected exactly one item, got %zu", count);
    T_CHECK_MSG(!from_mirror, "nothing was read, so from_mirror must stay false");
    T_CHECK_MSG(item.outcome == ATLAS_MEMORY_READ_NO_MIRROR, "expected NO_MIRROR, got %d",
                (int)item.outcome);
    T_CHECK_MSG(item.bytes.len == 0, "a NO_MIRROR outcome returned bytes");
    T_CHECK_MSG(err.status == ATLAS_OK, "err was left dirty: %s", atlas_err_msg(&err));

    atlas_memory_read_item_free(&item);
    atlas_repo_info_free(&info);

    char after[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(fx_repo(&fx), after, &err), &err);
    T_CHECK_MSG(strcmp(before, after) == 0, "a refused mirror read modified the repository");

    fx_close(&fx);
}

/* --- REPO_DIR ----------------------------------------------------------------- */

/* A13, driven end to end rather than asserted: a mirror's untracked content
 * comes from `git ls-files --others --exclude-standard` (src/git/git.c),
 * which never lists a gitignored path, so a memory directory made entirely of
 * gitignored, untracked notes is never written into the mirror at all. This
 * builds the mirror a real scanner pass would leave for exactly that
 * repository -- the tracked commit, and no `.claude/memories` anywhere in it
 * -- and reads the directory source against it. If the reader still said
 * ABSENT here, it would be claiming to have looked and found nothing, when it
 * never had a way to look at all. */
static void test_repo_dir_gitignored_is_not_mirrored(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);

    const char *repo = fx_repo(&fx);
    T_OK(fx_init_repo(&fx, repo, NULL, &err), &err);
    T_OK(fx_write(repo, "CLAUDE.md", "tracked memory\n", &err), &err);
    T_OK(fx_write(repo, ".gitignore", ".claude/memories/\n", &err), &err);
    T_OK(fx_add_all(&fx, repo, &err), &err);
    T_OK(fx_commit(&fx, repo, "initial commit", &err), &err);
    /* Present on the real tree, registered, and entirely gitignored: an
     * operator's own working notes, exactly what this class exists for. */
    T_OK(fx_mkdir(repo, ".claude", &err), &err);
    T_OK(fx_mkdir(repo, ".claude/memories", &err), &err);
    T_OK(fx_write(repo, ".claude/memories/a.md", "an operator's own note\n", &err), &err);

    char before[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(repo, before, &err), &err);

    /* The mirror a real scanner pass would leave: the same tracked commit,
     * with no .claude/memories anywhere -- its untracked walk never saw a
     * gitignored path to write. Built by hand at the fixed layout
     * atlas_mirror_repo_path names, exactly test_mirror_source.c's own
     * shape. */
    atlas_buf mirror_path = ATLAS_BUF_INIT;
    T_OK(atlas_mirror_repo_path(fx_data_dir(&fx), 1, &mirror_path, &err), &err);
    char mirror_parent[2048];
    (void)snprintf(mirror_parent, sizeof(mirror_parent), "%s/mirror", fx_data_dir(&fx));
    T_OK(fx_mkdir(fx_data_dir(&fx), "mirror", &err), &err);
    T_OK(fx_mkdir(mirror_parent, atlas_buf_cstr(&mirror_path) + strlen(mirror_parent) + 1u, &err),
         &err);
    T_OK(fx_init_repo(&fx, atlas_buf_cstr(&mirror_path), NULL, &err), &err);
    T_OK(fx_write(atlas_buf_cstr(&mirror_path), "CLAUDE.md", "tracked memory\n", &err), &err);
    T_OK(fx_write(atlas_buf_cstr(&mirror_path), ".gitignore", ".claude/memories/\n", &err), &err);
    T_OK(fx_add_all(&fx, atlas_buf_cstr(&mirror_path), &err), &err);
    T_OK(fx_commit(&fx, atlas_buf_cstr(&mirror_path), "initial commit", &err), &err);

    atlas_repo_info info;
    make_info(&info, repo, &err);
    info.scanner_uid = (int64_t)geteuid() + 1; /* deliberately not this process */
    info.mirror_complete = true;

    atlas_memory_read_item items[8];
    size_t count = 0;
    bool from_mirror = false;
    T_OK(atlas_memory_read_source(&info, fx_data_dir(&fx), ATLAS_MEMORY_SOURCE_REPO_DIR,
                                  ".claude/memories", strlen(".claude/memories"), items, 8u,
                                  &count, &from_mirror, &err),
         &err);
    T_REQUIRE(count == 1u);
    T_CHECK_MSG(from_mirror, "a mirror-backed read must report from_mirror at the call level too");
    T_CHECK_MSG(items[0].outcome == ATLAS_MEMORY_READ_NOT_MIRRORED,
                "expected NOT_MIRRORED, got %d -- a gitignored directory must never read as "
                "ABSENT",
                (int)items[0].outcome);
    T_CHECK_MSG(items[0].bytes.len == 0, "a NOT_MIRRORED outcome returned bytes");

    atlas_memory_read_item_free(&items[0]);
    atlas_repo_info_free(&info);
    atlas_buf_free(&mirror_path);

    char after[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(repo, after, &err), &err);
    T_CHECK_MSG(strcmp(before, after) == 0,
               "reading the gitignored directory modified the repository");

    fx_close(&fx);
}

/* The NF1 case: a directory that opens and lists successfully, one visible
 * child and one gitignored sibling the mirror never captured. Driven end to
 * end rather than asserted -- the reviewer's exact scenario. A mirror
 * mirrors every *tracked* path regardless of gitignore (tracking overrides
 * ignore rules; ATLAS_MEMORY_READ_NOT_MIRRORED's own comment says so), so
 * this builds the mirror with the tracked sibling present and the untracked,
 * ignored one absent -- exactly what a real scanner pass would leave. */
static void test_repo_dir_mixed_tracked_and_gitignored_children(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);

    const char *repo = fx_repo(&fx);
    T_OK(fx_init_repo(&fx, repo, NULL, &err), &err);
    T_OK(fx_write(repo, "CLAUDE.md", "tracked memory\n", &err), &err);
    T_OK(fx_write(repo, ".gitignore", ".claude/memories/ignored.md\n", &err), &err);
    T_OK(fx_mkdir(repo, ".claude", &err), &err);
    T_OK(fx_mkdir(repo, ".claude/memories", &err), &err);
    T_OK(fx_write(repo, ".claude/memories/tracked.md", "checked in\n", &err), &err);
    T_OK(fx_add_all(&fx, repo, &err), &err);
    T_OK(fx_commit(&fx, repo, "initial commit", &err), &err);
    /* Untracked, gitignored, and only on the real tree -- a real scanner's
     * `ls-files --others --exclude-standard` walk never lists it. */
    T_OK(fx_write(repo, ".claude/memories/ignored.md", "an operator's own note\n", &err), &err);

    char before[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(repo, before, &err), &err);

    /* The mirror a real scanner pass would leave: the tracked commit,
     * including .claude/memories/tracked.md (tracking overrides gitignore),
     * and no ignored.md anywhere in it. */
    atlas_buf mirror_path = ATLAS_BUF_INIT;
    T_OK(atlas_mirror_repo_path(fx_data_dir(&fx), 1, &mirror_path, &err), &err);
    char mirror_parent[2048];
    (void)snprintf(mirror_parent, sizeof(mirror_parent), "%s/mirror", fx_data_dir(&fx));
    T_OK(fx_mkdir(fx_data_dir(&fx), "mirror", &err), &err);
    T_OK(fx_mkdir(mirror_parent, atlas_buf_cstr(&mirror_path) + strlen(mirror_parent) + 1u, &err),
         &err);
    T_OK(fx_init_repo(&fx, atlas_buf_cstr(&mirror_path), NULL, &err), &err);
    T_OK(fx_write(atlas_buf_cstr(&mirror_path), "CLAUDE.md", "tracked memory\n", &err), &err);
    T_OK(fx_write(atlas_buf_cstr(&mirror_path), ".gitignore", ".claude/memories/ignored.md\n",
                  &err),
         &err);
    T_OK(fx_mkdir(atlas_buf_cstr(&mirror_path), ".claude", &err), &err);
    T_OK(fx_mkdir(atlas_buf_cstr(&mirror_path), ".claude/memories", &err), &err);
    T_OK(fx_write(atlas_buf_cstr(&mirror_path), ".claude/memories/tracked.md", "checked in\n",
                  &err),
         &err);
    T_OK(fx_add_all(&fx, atlas_buf_cstr(&mirror_path), &err), &err);
    T_OK(fx_commit(&fx, atlas_buf_cstr(&mirror_path), "initial commit", &err), &err);

    atlas_repo_info info;
    make_info(&info, repo, &err);
    info.scanner_uid = (int64_t)geteuid() + 1; /* deliberately not this process */
    info.mirror_complete = true;

    atlas_memory_read_item items[8];
    size_t count = 0;
    bool from_mirror = false;
    T_OK(atlas_memory_read_source(&info, fx_data_dir(&fx), ATLAS_MEMORY_SOURCE_REPO_DIR,
                                  ".claude/memories", strlen(".claude/memories"), items, 8u,
                                  &count, &from_mirror, &err),
         &err);
    /* Driven, not asserted: the listing opens cleanly and silently omits the
     * gitignored sibling -- exactly the shape that makes from_mirror load-
     * bearing, since nothing else here would tell a caller a second file
     * ever existed. */
    T_REQUIRE(count == 1u);
    T_CHECK_MSG(from_mirror, "a mirror-backed read must report from_mirror at the call level too");
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&items[0].rel_path), "tracked.md") == 0,
                "expected the one visible child to be \"tracked.md\", got \"%s\"",
                atlas_buf_cstr(&items[0].rel_path));
    T_CHECK_MSG(items[0].outcome == ATLAS_MEMORY_READ_OK, "expected OK, got %d",
                (int)items[0].outcome);
    T_CHECK_MSG(items[0].from_mirror,
                "a mirror-backed directory child must carry from_mirror, or nothing marks this "
                "listing as possibly incomplete");

    atlas_memory_read_item_free(&items[0]);
    atlas_repo_info_free(&info);
    atlas_buf_free(&mirror_path);

    char after[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(repo, after, &err), &err);
    T_CHECK_MSG(strcmp(before, after) == 0,
               "reading the mixed directory modified the repository");

    fx_close(&fx);
}

/* NF3: the directory opens, and finds nothing to keep. A tracked non-`.md`
 * file is enough to make atlas_mirror_put create the directory in the mirror
 * (src/daemon/mirror.c) with no `.md` sibling ever mirrored alongside it --
 * simulated here the same way every other mirror in this file is, by hand,
 * at the layout a real scanner pass would leave. The suffix filter and the
 * per-item stamping loop both have nothing to run against: zero items come
 * back, so nothing on any item can carry the fact that this came from a
 * mirror. Driven end to end, not asserted: the only place left to carry that
 * fact is the call itself. */
static void test_repo_dir_empty_mirror_listing_reports_from_mirror(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);

    const char *repo = fx_repo(&fx);
    T_OK(fx_init_repo(&fx, repo, NULL, &err), &err);
    T_OK(fx_write(repo, "CLAUDE.md", "tracked memory\n", &err), &err);
    T_OK(fx_mkdir(repo, ".claude", &err), &err);
    T_OK(fx_mkdir(repo, ".claude/memories", &err), &err);
    /* Tracked, and not a `.md` name: it is what makes the directory exist in
     * the mirror at all, and it is filtered out of every listing either way. */
    T_OK(fx_write(repo, ".claude/memories/index.json", "{}\n", &err), &err);
    T_OK(fx_add_all(&fx, repo, &err), &err);
    T_OK(fx_commit(&fx, repo, "initial commit", &err), &err);

    char before[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(repo, before, &err), &err);

    /* The mirror a real scanner pass would leave: the tracked commit,
     * including .claude/memories/index.json (tracked, so mirrored regardless
     * of its name), and nothing else in that directory -- there is no
     * untracked `.md` to mirror alongside it. */
    atlas_buf mirror_path = ATLAS_BUF_INIT;
    T_OK(atlas_mirror_repo_path(fx_data_dir(&fx), 1, &mirror_path, &err), &err);
    char mirror_parent[2048];
    (void)snprintf(mirror_parent, sizeof(mirror_parent), "%s/mirror", fx_data_dir(&fx));
    T_OK(fx_mkdir(fx_data_dir(&fx), "mirror", &err), &err);
    T_OK(fx_mkdir(mirror_parent, atlas_buf_cstr(&mirror_path) + strlen(mirror_parent) + 1u, &err),
         &err);
    T_OK(fx_init_repo(&fx, atlas_buf_cstr(&mirror_path), NULL, &err), &err);
    T_OK(fx_write(atlas_buf_cstr(&mirror_path), "CLAUDE.md", "tracked memory\n", &err), &err);
    T_OK(fx_mkdir(atlas_buf_cstr(&mirror_path), ".claude", &err), &err);
    T_OK(fx_mkdir(atlas_buf_cstr(&mirror_path), ".claude/memories", &err), &err);
    T_OK(fx_write(atlas_buf_cstr(&mirror_path), ".claude/memories/index.json", "{}\n", &err),
         &err);
    T_OK(fx_add_all(&fx, atlas_buf_cstr(&mirror_path), &err), &err);
    T_OK(fx_commit(&fx, atlas_buf_cstr(&mirror_path), "initial commit", &err), &err);

    atlas_repo_info info;
    make_info(&info, repo, &err);
    info.scanner_uid = (int64_t)geteuid() + 1; /* deliberately not this process */
    info.mirror_complete = true;

    atlas_memory_read_item items[8];
    size_t count = 0;
    bool from_mirror = false;
    T_OK(atlas_memory_read_source(&info, fx_data_dir(&fx), ATLAS_MEMORY_SOURCE_REPO_DIR,
                                  ".claude/memories", strlen(".claude/memories"), items, 8u,
                                  &count, &from_mirror, &err),
         &err);
    T_CHECK_MSG(count == 0u, "expected zero items, got %zu", count);
    T_CHECK_MSG(from_mirror,
                "an empty mirror-backed listing must still report from_mirror at the call "
                "level -- no item exists to carry it");

    atlas_repo_info_free(&info);
    atlas_buf_free(&mirror_path);

    char after[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(repo, after, &err), &err);
    T_CHECK_MSG(strcmp(before, after) == 0,
               "reading the empty mirror-backed directory modified the repository");

    fx_close(&fx);
}

static void test_repo_dir_finds_untracked_md(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    build_repo(&fx, &err);

    char before[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(fx_repo(&fx), before, &err), &err);

    atlas_repo_info info;
    make_info(&info, fx_repo(&fx), &err);

    atlas_memory_read_item items[8];
    size_t count = 0;
    bool from_mirror = true;
    T_OK(atlas_memory_read_source(&info, fx_data_dir(&fx), ATLAS_MEMORY_SOURCE_REPO_DIR,
                                  ".claude/memories", strlen(".claude/memories"), items, 8u,
                                  &count, &from_mirror, &err),
         &err);
    T_REQUIRE(count == 1u);
    T_CHECK_MSG(!from_mirror, "a tree-direct read must not report from_mirror");
    T_CHECK_MSG(items[0].outcome == ATLAS_MEMORY_READ_OK, "expected OK, got %d",
                (int)items[0].outcome);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&items[0].rel_path), "a.md") == 0,
                "unexpected rel_path \"%s\"", atlas_buf_cstr(&items[0].rel_path));
    T_CHECK_MSG(items[0].blob_oid.len == 0, "a directory entry's blob_oid should be empty");
    T_CHECK_MSG(items[0].commit_oid.len == 0, "a directory entry's commit_oid should be empty");
    T_CHECK_MSG(items[0].bytes.len == strlen("untracked note\n") &&
                    memcmp(items[0].bytes.data, "untracked note\n", items[0].bytes.len) == 0,
                "unexpected directory entry content");

    atlas_memory_read_item_free(&items[0]);
    atlas_repo_info_free(&info);

    char after[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(fx_repo(&fx), after, &err), &err);
    T_CHECK_MSG(strcmp(before, after) == 0, "listing a memory directory modified the repository");

    fx_close(&fx);
}

/* A directory holding a non-.md file and a subdirectory beside two .md files:
 * the first two are skipped, and the two kept entries come back sorted by
 * name regardless of the order they were created in. */
static void test_repo_dir_skips_and_sorts(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    build_repo(&fx, &err);

    const char *repo = fx_repo(&fx);
    /* Created out of alphabetical order on purpose. */
    T_OK(fx_write(repo, ".claude/memories/z.md", "last alphabetically\n", &err), &err);
    T_OK(fx_write(repo, ".claude/memories/notes.txt", "not a memory file\n", &err), &err);
    T_OK(fx_mkdir(repo, ".claude/memories/sub", &err), &err);

    char before[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(repo, before, &err), &err);

    atlas_repo_info info;
    make_info(&info, repo, &err);

    atlas_memory_read_item items[8];
    size_t count = 0;
    bool from_mirror = true;
    T_OK(atlas_memory_read_source(&info, fx_data_dir(&fx), ATLAS_MEMORY_SOURCE_REPO_DIR,
                                  ".claude/memories", strlen(".claude/memories"), items, 8u,
                                  &count, &from_mirror, &err),
         &err);
    T_CHECK_MSG(!from_mirror, "a tree-direct read must not report from_mirror");
    /* a.md (from build_repo) and z.md: notes.txt and sub/ are skipped. */
    T_REQUIRE(count == 2u);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&items[0].rel_path), "a.md") == 0,
                "expected \"a.md\" first, got \"%s\"", atlas_buf_cstr(&items[0].rel_path));
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&items[1].rel_path), "z.md") == 0,
                "expected \"z.md\" second, got \"%s\"", atlas_buf_cstr(&items[1].rel_path));
    T_CHECK_MSG(items[0].outcome == ATLAS_MEMORY_READ_OK && items[1].outcome == ATLAS_MEMORY_READ_OK,
                "both kept entries should read OK");

    atlas_memory_read_item_free(&items[0]);
    atlas_memory_read_item_free(&items[1]);
    atlas_repo_info_free(&info);

    char after[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(repo, after, &err), &err);
    T_CHECK_MSG(strcmp(before, after) == 0, "listing a memory directory modified the repository");

    fx_close(&fx);
}

/* NF4: from_mirror_out is declinable, and a REPO_DIR caller that declines it
 * on a mirror-backed read recreates NF3 exactly -- silently, on any listing
 * that happens to come back empty. Refused outright instead: driven end to
 * end against the exact NF3 fixture (a tracked non-.md file, an empty
 * matching set), passing NULL where every other REPO_DIR test in this file
 * passes &from_mirror. */
static void test_repo_dir_requires_from_mirror_out(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);

    const char *repo = fx_repo(&fx);
    T_OK(fx_init_repo(&fx, repo, NULL, &err), &err);
    T_OK(fx_write(repo, "CLAUDE.md", "tracked memory\n", &err), &err);
    T_OK(fx_mkdir(repo, ".claude", &err), &err);
    T_OK(fx_mkdir(repo, ".claude/memories", &err), &err);
    T_OK(fx_write(repo, ".claude/memories/index.json", "{}\n", &err), &err);
    T_OK(fx_add_all(&fx, repo, &err), &err);
    T_OK(fx_commit(&fx, repo, "initial commit", &err), &err);

    char before[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(repo, before, &err), &err);

    atlas_repo_info info;
    make_info(&info, repo, &err);
    /* scanner_uid stays 0 -- a tree-direct read. The refusal must fire before
     * atlas_repo_open_git is ever asked, whether or not a mirror is even in
     * play: a REPO_DIR caller cannot buy its way out of carrying
     * from_mirror_out by reading a repository that happens not to need one
     * today. */

    atlas_memory_read_item items[8];
    size_t count = 0;
    atlas_status st = atlas_memory_read_source(&info, fx_data_dir(&fx),
                                               ATLAS_MEMORY_SOURCE_REPO_DIR, ".claude/memories",
                                               strlen(".claude/memories"), items, 8u, &count, NULL,
                                               &err);
    T_CHECK_MSG(st == ATLAS_ERR_INTERNAL, "expected ATLAS_ERR_INTERNAL, got %s: %s",
                atlas_status_name(st), atlas_err_msg(&err));
    T_CHECK_MSG(count == 0u,
                "a refused call reports zero items -- there is no result for the caller to "
                "misread as a complete listing");

    atlas_repo_info_free(&info);

    char after[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(repo, after, &err), &err);
    T_CHECK_MSG(strcmp(before, after) == 0,
               "a refused read modified the repository");

    fx_close(&fx);
}

/* --- EXTERNAL_* through atlas_memory_read_source ------------------------------ */

static void test_external_file_via_read_source_is_not_ours(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    build_repo(&fx, &err);

    char before[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(fx_repo(&fx), before, &err), &err);

    atlas_repo_info info;
    make_info(&info, fx_repo(&fx), &err);

    atlas_memory_read_item item;
    size_t count = 0;
    bool from_mirror = true;
    T_OK(atlas_memory_read_source(&info, fx_data_dir(&fx), ATLAS_MEMORY_SOURCE_EXTERNAL_FILE,
                                  "/etc/some-notes.md", strlen("/etc/some-notes.md"), &item, 1u,
                                  &count, &from_mirror, &err),
         &err);
    T_CHECK_MSG(count == 1u, "expected exactly one item, got %zu", count);
    T_CHECK_MSG(!from_mirror, "nothing was read, so from_mirror must stay false");
    T_CHECK_MSG(item.outcome == ATLAS_MEMORY_READ_NOT_OURS, "expected NOT_OURS, got %d",
                (int)item.outcome);
    T_CHECK_MSG(item.bytes.len == 0, "an EXTERNAL_FILE source through read_source read bytes");

    atlas_memory_read_item_free(&item);
    atlas_repo_info_free(&info);

    char after[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(fx_repo(&fx), after, &err), &err);
    T_CHECK_MSG(strcmp(before, after) == 0,
               "an EXTERNAL_FILE read through read_source touched the repository");

    fx_close(&fx);
}

/* --- atlas_memory_read_external: the operator's own CLI ----------------------- */

static void test_read_external_refuses_a_symlink(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);

    const char *root = atlas_buf_cstr(&fx.root);
    T_OK(fx_symlink(root, "/nowhere-in-particular", "ext-link.md", &err), &err);

    char before[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(root, before, &err), &err);

    atlas_buf path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&path, &err, "%s/ext-link.md", root), &err);

    atlas_memory_read_item item;
    size_t count = 0;
    bool from_mirror = true;
    T_OK(atlas_memory_read_external(atlas_buf_cstr(&path), path.len, false, &item, 1u, &count,
                                    &from_mirror, &err),
         &err);
    T_CHECK_MSG(count == 1u, "expected exactly one item, got %zu", count);
    T_CHECK_MSG(!from_mirror, "an external read must never report from_mirror");
    T_CHECK_MSG(item.outcome == ATLAS_MEMORY_READ_SYMLINK, "expected SYMLINK, got %d",
                (int)item.outcome);
    T_CHECK_MSG(item.bytes.len == 0, "a symlinked source returned bytes");

    atlas_memory_read_item_free(&item);
    atlas_buf_free(&path);

    char after[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(root, after, &err), &err);
    T_CHECK_MSG(strcmp(before, after) == 0,
               "reading a symlinked external source modified the fixture tree");

    fx_close(&fx);
}

/* --- T8: the pass -----------------------------------------------------------
 *
 * Unlike the read tests above, T8's own tests need a real database with real
 * rows: `atlas_memory_apply_in_tx` resolves anchors against `files`, binds a
 * claim to `repositories.scanned_head`, and writes through
 * `atlas_verify_intake_apply_in_tx`. `t8env` (`tests/support/reconcile_env.h`)
 * builds both halves and keeps them consistent -- a real git repository for
 * the observe phase to read (T6's own fixture shape), and a registered
 * repository row, a matching `commits` row and `files` rows for whatever the
 * pass needs to look up (`test_memory_anchor.c`'s own fixture shape) for the
 * apply phase.
 *
 * A12.1 T11 fix round, item 6: `t8env`, `t8_env_open`, `t8_bind_head`,
 * `t8_seed_file` and `t8_env_close` used to be `static` in this file. They are
 * hoisted into `tests/support/reconcile_env.c` now that
 * `test_memory_reconcile_live.c` needs the same fixture for the two
 * daemon-forking cases that moved there -- one implementation, not two copies
 * to keep in step. `t8_policy`, `t8_scalar` and `t8_run_pass` immediately
 * below stay here: nothing in the live-daemon file calls them. */

static void t8_policy(atlas_syspolicy *pol, atlas_memory_source_class cls, const char *const *paths,
                      size_t n) {
    memset(pol, 0, sizeof *pol);
    pol->state = ATLAS_SYSPOLICY_SYSTEM;
    pol->memory_source_count = n;
    for (size_t i = 0; i < n; i++) {
        pol->memory_sources[i].cls = cls;
        pol->memory_sources[i].repo_name[0] = '\0';
        (void)snprintf(pol->memory_sources[i].path, sizeof pol->memory_sources[i].path, "%s",
                       paths[i]);
    }
}

static int64_t t8_scalar(t8env *e, const char *sql, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    T_OK(atlas_db_prepare(e->db, sql, &stmt, err), err);
    /* A `SELECT COUNT(*)` always yields exactly one row, even over zero
     * matches -- so anything other than SQLITE_ROW here is the query itself
     * failing, not a genuine zero, and must not be allowed to read the same
     * as one. Without this, `t8_scalar(...) == 0` would have exactly one
     * vacuous shape: a caller asking "is the count zero" getting "yes"
     * because the step never ran rather than because it counted zero rows. */
    int step = sqlite3_step(stmt);
    T_REQUIRE_MSG(step == SQLITE_ROW, "scalar query did not yield a row: %s (%s)", sql,
                 sqlite3_errmsg(e->db->h));
    int64_t v = sqlite3_column_int64(stmt, 0);
    atlas_db_finish(e->db, stmt);
    return v;
}

/* Runs one full pass: observe with no transaction open, then apply inside
 * one -- Decision 9, driven exactly as T10's writer job will drive it. */
static void t8_run_pass(t8env *e, const atlas_syspolicy *pol, atlas_memory_pass_result *result,
                        atlas_err *err) {
    char now[64];
    atlas_now_iso8601(now, sizeof now);

    atlas_memory_observation *obs = malloc(sizeof *obs);
    T_REQUIRE(obs != NULL);
    T_CHECK_MSG(!atlas_db_in_transaction(e->db),
                "observe must be called with no transaction already open");
    T_OK(atlas_memory_observe(e->db, &e->repo, fx_data_dir(&e->fx), pol, obs, err), err);
    T_CHECK_MSG(!atlas_db_in_transaction(e->db),
                "atlas_memory_observe left a transaction open on the handle it was given");

    T_OK(atlas_db_begin(e->db, err), err);
    T_OK(atlas_memory_apply_in_tx(e->db, &e->repo, obs, pol, now, result, err), err);
    T_OK(atlas_db_commit(e->db, err), err);

    atlas_memory_observation_free(obs);
    free(obs);
}

/* (a) acceptance item 1: two sources carrying the byte-identical bullet
 * collapse to one claim, two independently-readable attestations naming
 * their own source actor, two evidence rows (one per source's own path), and
 * -- Decision 2's whole point -- one independent group once the aggregate is
 * computed. */
static void test_pass_collapses_duplicate_source_text(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    const char *repo = fx_repo(&e.fx);
    const char *bullet = "the daemon reads `src/db/db_orch.c`";
    T_OK(fx_mkdir(repo, "src", &err), &err);
    T_OK(fx_mkdir(repo, "src/db", &err), &err);
    T_OK(fx_write(repo, "src/db/db_orch.c", "int x;\n", &err), &err);
    T_OK(fx_write(repo, "note-a.md", bullet, &err), &err);
    T_OK(fx_write(repo, "note-b.md", bullet, &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "src/db/db_orch.c", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                &err);
    t8_seed_file(&e, "note-a.md", "1111111111111111111111111111111111111111111111111111111111111111",
                &err);
    t8_seed_file(&e, "note-b.md", "2222222222222222222222222222222222222222222222222222222222222222",
                &err);

    const char *paths[] = {"note-a.md", "note-b.md"};
    atlas_syspolicy pol;
    t8_policy(&pol, ATLAS_MEMORY_SOURCE_REPO_FILE, paths, 2);

    atlas_memory_pass_result result;
    t8_run_pass(&e, &pol, &result, &err);

    T_CHECK_MSG(result.claims_created == 1, "expected 1 new claim, got %zu", result.claims_created);
    T_CHECK_MSG(result.claims_resolved == 1, "expected 1 resolved duplicate claim, got %zu",
                result.claims_resolved);
    T_CHECK_MSG(result.generation != 0, "expected a generation to be appended");

    char sql[256];
    (void)snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM verify_claims WHERE repo_id = %lld;",
                  (long long)e.repo_id);
    T_CHECK_MSG(t8_scalar(&e, sql, &err) == 1, "expected exactly one stored claim");
    (void)snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM verify_evidence WHERE repo_id = %lld;",
                  (long long)e.repo_id);
    T_CHECK_MSG(t8_scalar(&e, sql, &err) == 2, "expected exactly two evidence rows");
    (void)snprintf(sql, sizeof sql,
                  "SELECT COUNT(*) FROM verify_attestations a"
                  " JOIN verify_claims c ON c.id = a.claim_id WHERE c.repo_id = %lld;",
                  (long long)e.repo_id);
    T_CHECK_MSG(t8_scalar(&e, sql, &err) == 2, "expected exactly two attestations");
    (void)snprintf(sql, sizeof sql,
                  "SELECT COUNT(DISTINCT a.actor_id) FROM verify_attestations a"
                  " JOIN verify_claims c ON c.id = a.claim_id WHERE c.repo_id = %lld;",
                  (long long)e.repo_id);
    T_CHECK_MSG(t8_scalar(&e, sql, &err) == 2,
                "expected each attestation to name its own source's actor");

    /* Drive the aggregate the way an operator's own `verify evaluate` would,
     * to prove the DEPENDENCY_ADD edge the pass wrote actually collapses the
     * union-find rather than merely existing as a row nobody reads. */
    char claim_uid[128];
    {
        sqlite3_stmt *stmt = NULL;
        (void)snprintf(sql, sizeof sql, "SELECT uid FROM verify_claims WHERE repo_id = %lld LIMIT 1;",
                      (long long)e.repo_id);
        T_OK(atlas_db_prepare(e.db, sql, &stmt, &err), &err);
        T_REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
        (void)snprintf(claim_uid, sizeof claim_uid, "%s", (const char *)sqlite3_column_text(stmt, 0));
        atlas_db_finish(e.db, stmt);
    }
    atlas_verify_op eval_op;
    atlas_verify_op_init(&eval_op);
    eval_op.kind = ATLAS_VERIFY_OP_EVALUATE;
    eval_op.channel = ATLAS_VERIFY_CHANNEL_OPERATOR;
    T_OK(atlas_buf_set_str(&eval_op.claim_uid, claim_uid, &err), &err);
    atlas_verify_intake_result eval_res;
    atlas_verify_intake_result_init(&eval_res);
    T_OK(atlas_db_begin(e.db, &err), &err);
    T_OK(atlas_verify_intake_apply_in_tx(e.db, &eval_op, &eval_res, &err), &err);
    T_OK(atlas_db_commit(e.db, &err), &err);
    T_CHECK_MSG(eval_res.assessment.aggregate.independent_groups == 1,
                "expected one independent group after the dependency edge, got %d",
                eval_res.assessment.aggregate.independent_groups);
    atlas_verify_intake_result_free(&eval_res);
    atlas_verify_op_free(&eval_op);

    t8_env_close(&e);
}

/* (b) three copies of one assertion -- two current, one from an older
 * version of a source -- retain three readable provenances. Two passes: the
 * first registers one source and reconciles it while the bullet is its only
 * content; the second edits that source (the bullet survives, unrelated
 * content is added) and registers a second source carrying the same bullet
 * from the start, then reconciles again over a moved HEAD. Nothing from the
 * first pass is touched by the second -- the claim it wrote is bound to a
 * commit the second pass's own claim is not. */
static void test_pass_retains_provenance_across_versions(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    const char *repo = fx_repo(&e.fx);
    const char *bullet = "the daemon reads `src/db/db_orch.c`";
    T_OK(fx_mkdir(repo, "src", &err), &err);
    T_OK(fx_mkdir(repo, "src/db", &err), &err);
    T_OK(fx_write(repo, "src/db/db_orch.c", "int x;\n", &err), &err);
    T_OK(fx_write(repo, "note-a.md", bullet, &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "v1", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "src/db/db_orch.c", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                &err);
    t8_seed_file(&e, "note-a.md", "1111111111111111111111111111111111111111111111111111111111111111",
                &err);

    const char *paths1[] = {"note-a.md"};
    atlas_syspolicy pol1;
    t8_policy(&pol1, ATLAS_MEMORY_SOURCE_REPO_FILE, paths1, 1);
    atlas_memory_pass_result r1;
    t8_run_pass(&e, &pol1, &r1, &err);
    T_CHECK_MSG(r1.claims_created == 1, "pass 1: expected one new claim");

    /* note-a.md's content moves (the bullet survives) and note-b.md joins
     * with the same bullet from the start, all under a new commit -- a moved
     * HEAD, which is what makes pass 2's claim a genuinely different one. */
    char buf[256];
    /* A blank line, so the bullet stays its own paragraph candidate --
     * byte-identical to note-b.md's -- rather than merging with the second
     * line into one bigger candidate neither source shares. */
    (void)snprintf(buf, sizeof buf, "%s\n\nan unrelated second line\n", bullet);
    T_OK(fx_write(repo, "note-a.md", buf, &err), &err);
    T_OK(fx_write(repo, "note-b.md", bullet, &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "v2", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "note-b.md", "3333333333333333333333333333333333333333333333333333333333333333",
                &err);
    /* note-a.md's own row must describe *this* commit's content for the
     * index lookup EVIDENCE_ADD performs; the hash value itself is never
     * checked against real bytes anywhere T8 runs. */
    (void)snprintf(buf, sizeof buf,
                  "UPDATE files SET content_hash = "
                  "'4444444444444444444444444444444444444444444444444444444444444444' "
                  "WHERE repo_id = %lld AND path_text = 'note-a.md';",
                  (long long)e.repo_id);
    T_OK(atlas_db_exec_sql(e.db, buf, &err), &err);

    const char *paths2[] = {"note-a.md", "note-b.md"};
    atlas_syspolicy pol2;
    t8_policy(&pol2, ATLAS_MEMORY_SOURCE_REPO_FILE, paths2, 2);
    atlas_memory_pass_result r2;
    t8_run_pass(&e, &pol2, &r2, &err);
    T_CHECK_MSG(r2.claims_created == 1,
                "pass 2: expected exactly one new claim (a moved basis_commit), got %zu",
                r2.claims_created);
    T_CHECK_MSG(r2.claims_resolved == 1, "pass 2: expected note-b.md to resolve to pass 2's claim");

    char sql[256];
    (void)snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM verify_claims WHERE repo_id = %lld;",
                  (long long)e.repo_id);
    T_CHECK_MSG(t8_scalar(&e, sql, &err) == 2,
                "expected two claims: the first pass's and the second pass's");
    (void)snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM verify_evidence WHERE repo_id = %lld;",
                  (long long)e.repo_id);
    T_CHECK_MSG(t8_scalar(&e, sql, &err) == 3,
                "expected three evidence rows: pass 1's, and pass 2's two");

    t8_env_close(&e);
}

/* (c) a second pass over unchanged bytes writes nothing new and appends no
 * generation. */
static void test_pass_over_unchanged_bytes_is_a_no_op(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    const char *repo = fx_repo(&e.fx);
    const char *bullet = "the daemon reads `src/db/db_orch.c`";
    T_OK(fx_mkdir(repo, "src", &err), &err);
    T_OK(fx_mkdir(repo, "src/db", &err), &err);
    T_OK(fx_write(repo, "src/db/db_orch.c", "int x;\n", &err), &err);
    T_OK(fx_write(repo, "note-a.md", bullet, &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "src/db/db_orch.c", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                &err);
    t8_seed_file(&e, "note-a.md", "1111111111111111111111111111111111111111111111111111111111111111",
                &err);

    const char *paths[] = {"note-a.md"};
    atlas_syspolicy pol;
    t8_policy(&pol, ATLAS_MEMORY_SOURCE_REPO_FILE, paths, 1);

    atlas_memory_pass_result r1;
    t8_run_pass(&e, &pol, &r1, &err);
    T_CHECK_MSG(r1.generation != 0, "pass 1: expected a generation to be appended");

    char sql[256];
    (void)snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM verify_claims WHERE repo_id = %lld;",
                  (long long)e.repo_id);
    int64_t claims_before = t8_scalar(&e, sql, &err);
    (void)snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM verify_evidence WHERE repo_id = %lld;",
                  (long long)e.repo_id);
    int64_t evidence_before = t8_scalar(&e, sql, &err);
    (void)snprintf(sql, sizeof sql,
                  "SELECT COUNT(*) FROM verify_attestations a"
                  " JOIN verify_claims c ON c.id = a.claim_id WHERE c.repo_id = %lld;",
                  (long long)e.repo_id);
    int64_t attest_before = t8_scalar(&e, sql, &err);

    atlas_memory_pass_result r2;
    t8_run_pass(&e, &pol, &r2, &err);
    T_CHECK_MSG(r2.generation == 0, "pass 2: expected no generation over unchanged bytes, got %lld",
                (long long)r2.generation);
    T_CHECK_MSG(r2.claims_created == 0 && r2.versions_added == 0,
                "pass 2: expected nothing new to be created");

    (void)snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM verify_claims WHERE repo_id = %lld;",
                  (long long)e.repo_id);
    T_CHECK_MSG(t8_scalar(&e, sql, &err) == claims_before, "claim count moved on a no-op pass");
    (void)snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM verify_evidence WHERE repo_id = %lld;",
                  (long long)e.repo_id);
    T_CHECK_MSG(t8_scalar(&e, sql, &err) == evidence_before, "evidence count moved on a no-op pass");
    (void)snprintf(sql, sizeof sql,
                  "SELECT COUNT(*) FROM verify_attestations a"
                  " JOIN verify_claims c ON c.id = a.claim_id WHERE c.repo_id = %lld;",
                  (long long)e.repo_id);
    T_CHECK_MSG(t8_scalar(&e, sql, &err) == attest_before, "attestation count moved on a no-op pass");

    t8_env_close(&e);
}

/* (d) a prose-only candidate -- no path, symbol, decision or commit anchor
 * anywhere in it -- lands in memory_unanchored and never becomes a claim. */
static void test_prose_only_candidate_is_unanchored(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    const char *repo = fx_repo(&e.fx);
    T_OK(fx_write(repo, "note-a.md", "just some prose with nothing in it to anchor to", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "note-a.md", "1111111111111111111111111111111111111111111111111111111111111111",
                &err);

    const char *paths[] = {"note-a.md"};
    atlas_syspolicy pol;
    t8_policy(&pol, ATLAS_MEMORY_SOURCE_REPO_FILE, paths, 1);

    atlas_memory_pass_result result;
    t8_run_pass(&e, &pol, &result, &err);
    T_CHECK_MSG(result.claims_created == 0, "a prose-only candidate must not become a claim");
    T_CHECK_MSG(result.unanchored == 1, "expected exactly one unanchored candidate, got %zu",
                result.unanchored);

    char sql[256];
    (void)snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM verify_claims WHERE repo_id = %lld;",
                  (long long)e.repo_id);
    T_CHECK_MSG(t8_scalar(&e, sql, &err) == 0, "no claim should exist for this repository");
    T_CHECK_MSG(t8_scalar(&e, "SELECT COUNT(*) FROM memory_unanchored;", &err) == 1,
                "expected exactly one row in memory_unanchored");

    t8_env_close(&e);
}

/* A finding from this task's own review, not named in the brief: a
 * registered source's own bytes are not always indexed -- T6's own headline
 * fixture is a gitignored `*_DIR` child, an operator's own working notes --
 * and `EVIDENCE_ADD`'s `path_text` lookup refuses a path `files` does not
 * hold. A candidate split from such an item is routed to `memory_unanchored`
 * regardless of whether its own anchors would resolve, rather than either
 * aborting the whole pass or leaving a claim with no evidence for where its
 * own bytes came from. Reproduced here at the simplest level that shows it:
 * a `*_FILE` source whose own path was never indexed, citing a path
 * (`src/db/db_orch.c`) that *is* -- so the anchor would resolve if the pass
 * ever reached it, and the only thing standing between this candidate and a
 * claim is its own source's index status. */
static void test_unindexed_source_path_is_unanchored_not_refused(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    const char *repo = fx_repo(&e.fx);
    T_OK(fx_mkdir(repo, "src", &err), &err);
    T_OK(fx_mkdir(repo, "src/db", &err), &err);
    T_OK(fx_write(repo, "src/db/db_orch.c", "int x;\n", &err), &err);
    T_OK(fx_write(repo, "note-a.md", "the daemon reads `src/db/db_orch.c`", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "src/db/db_orch.c", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                &err);
    /* Deliberately not indexed: "note-a.md" itself has no row in `files`,
     * exactly what a gitignored path or a not-yet-reconciled one leaves. */

    const char *paths[] = {"note-a.md"};
    atlas_syspolicy pol;
    t8_policy(&pol, ATLAS_MEMORY_SOURCE_REPO_FILE, paths, 1);

    atlas_memory_pass_result result;
    t8_run_pass(&e, &pol, &result, &err);
    T_CHECK_MSG(result.claims_created == 0,
                "a candidate from an unindexed source must not become a claim, got %zu claims",
                result.claims_created);
    T_CHECK_MSG(result.unanchored == 1,
                "expected the candidate to be routed to memory_unanchored, got %zu", result.unanchored);

    char sql[256];
    (void)snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM verify_claims WHERE repo_id = %lld;",
                  (long long)e.repo_id);
    T_CHECK_MSG(t8_scalar(&e, sql, &err) == 0, "no claim should exist for this repository");
    T_CHECK_MSG(t8_scalar(&e, "SELECT COUNT(*) FROM memory_unanchored;", &err) == 1,
                "expected exactly one row in memory_unanchored");

    t8_env_close(&e);
}

/* (e) the stored DOCUMENT actor's identity is SELF_DECLARED, and Decision 1's
 * arithmetic -- min(DOCUMENT 400, SELF_DECLARED 350) = 350 -- is what a
 * memory assertion is actually worth. */
static void test_document_actor_is_self_declared_350(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    const char *repo = fx_repo(&e.fx);
    T_OK(fx_mkdir(repo, "src", &err), &err);
    T_OK(fx_mkdir(repo, "src/db", &err), &err);
    T_OK(fx_write(repo, "src/db/db_orch.c", "int x;\n", &err), &err);
    T_OK(fx_write(repo, "note-a.md", "the daemon reads `src/db/db_orch.c`", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "src/db/db_orch.c", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                &err);
    t8_seed_file(&e, "note-a.md", "1111111111111111111111111111111111111111111111111111111111111111",
                &err);

    const char *paths[] = {"note-a.md"};
    atlas_syspolicy pol;
    t8_policy(&pol, ATLAS_MEMORY_SOURCE_REPO_FILE, paths, 1);
    atlas_memory_pass_result result;
    t8_run_pass(&e, &pol, &result, &err);
    T_REQUIRE(result.claims_created == 1);

    sqlite3_stmt *stmt = NULL;
    T_OK(atlas_db_prepare(e.db, "SELECT identity FROM verify_actors WHERE class = 'DOCUMENT';", &stmt,
                          &err),
         &err);
    T_REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    T_CHECK_MSG(strcmp((const char *)sqlite3_column_text(stmt, 0), "SELF_DECLARED") == 0,
                "expected a DOCUMENT actor's stored identity to be SELF_DECLARED, got \"%s\"",
                (const char *)sqlite3_column_text(stmt, 0));
    atlas_db_finish(e.db, stmt);

    int prior = atlas_verify_prior_reliability(ATLAS_ACTOR_DOCUMENT, ATLAS_ACTOR_IDENTITY_SELF_DECLARED);
    T_CHECK_MSG(prior == 350, "expected a DOCUMENT/SELF_DECLARED prior of 350, got %d", prior);

    t8_env_close(&e);
}

/* Debt 1: ATLAS_MEMORY_EXTRACTOR_VERSION is consumed nowhere in `src/` before
 * this task. It lands on EVIDENCE_ADD's `actor_version` (`emit_candidate`,
 * `src/memory/reconcile.c`), and this proves it *landed* rather than merely
 * *exists*: the stored value is exactly the compiled constant, stringified
 * with the same conversion the production code uses (so a bump to the
 * constant moves this test's expectation too, rather than needing a second
 * hand-maintained "1"), and it is load-bearing in exactly the way
 * `ATLAS_SEM_ANALYZER_VERSION` was not for years -- `derive_actor` hashes
 * `actor_version` into the actor uid (`intake.c:427-430`), so two EVIDENCE_ADD
 * calls differing only in `actor_version` mint two different actors. That
 * second half is driven directly through the write point, on the same claim
 * the pass already created, to show the version is not merely stored beside
 * an actor row but is *part of which actor a bump would mint*. */
static void test_extractor_version_lands_in_the_evidence_actor(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    const char *repo = fx_repo(&e.fx);
    T_OK(fx_mkdir(repo, "src", &err), &err);
    T_OK(fx_mkdir(repo, "src/db", &err), &err);
    T_OK(fx_write(repo, "src/db/db_orch.c", "int x;\n", &err), &err);
    T_OK(fx_write(repo, "note-a.md", "the daemon reads `src/db/db_orch.c`", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "src/db/db_orch.c", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                &err);
    t8_seed_file(&e, "note-a.md", "1111111111111111111111111111111111111111111111111111111111111111",
                &err);

    const char *paths[] = {"note-a.md"};
    atlas_syspolicy pol;
    t8_policy(&pol, ATLAS_MEMORY_SOURCE_REPO_FILE, paths, 1);
    atlas_memory_pass_result result;
    t8_run_pass(&e, &pol, &result, &err);
    T_REQUIRE(result.claims_created == 1);

    char expected[16];
    (void)snprintf(expected, sizeof expected, "%d", ATLAS_MEMORY_EXTRACTOR_VERSION);
    sqlite3_stmt *stmt = NULL;
    T_OK(atlas_db_prepare(e.db,
                          "SELECT version, uid FROM verify_actors WHERE class = 'ATLAS_VERIFIER' "
                          "AND name = 'memory-reconciler';",
                          &stmt, &err),
         &err);
    T_REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    T_CHECK_MSG(strcmp((const char *)sqlite3_column_text(stmt, 0), expected) == 0,
                "expected the stored actor_version to be the compiled extractor epoch \"%s\", "
                "got \"%s\"",
                expected, (const char *)sqlite3_column_text(stmt, 0));
    char original_actor_uid[128];
    (void)snprintf(original_actor_uid, sizeof original_actor_uid, "%s",
                  (const char *)sqlite3_column_text(stmt, 1));
    atlas_db_finish(e.db, stmt);

    /* The claim uid, to drive a second EVIDENCE_ADD directly through the
     * write point -- bypassing reconcile.c entirely -- with a different
     * `actor_version`, the same channel and actor description otherwise. */
    char claim_uid[128];
    T_OK(atlas_db_prepare(e.db, "SELECT uid FROM verify_claims LIMIT 1;", &stmt, &err), &err);
    T_REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    (void)snprintf(claim_uid, sizeof claim_uid, "%s", (const char *)sqlite3_column_text(stmt, 0));
    atlas_db_finish(e.db, stmt);

    atlas_verify_op op;
    atlas_verify_op_init(&op);
    op.kind = ATLAS_VERIFY_OP_EVIDENCE_ADD;
    op.channel = ATLAS_VERIFY_CHANNEL_ATLAS;
    T_OK(atlas_buf_set_str(&op.actor_name, "memory-reconciler", &err), &err);
    T_OK(atlas_buf_set_str(&op.actor_provider, "atlas", &err), &err);
    T_OK(atlas_buf_set_str(&op.actor_version, "999", &err), &err); /* a hypothetical future bump */
    T_OK(atlas_buf_set_str(&op.claim_uid, claim_uid, &err), &err);
    op.evidence_class = ATLAS_EVIDENCE_DOCUMENT;
    T_OK(atlas_buf_set_str(&op.path_text, "note-a.md", &err), &err);
    T_OK(atlas_buf_set_str(&op.observed_at, "2026-06-01T00:00:00Z", &err), &err);
    atlas_verify_intake_result res;
    atlas_verify_intake_result_init(&res);
    T_OK(atlas_db_begin(e.db, &err), &err);
    T_OK(atlas_verify_intake_apply_in_tx(e.db, &op, &res, &err), &err);
    T_OK(atlas_db_commit(e.db, &err), &err);
    atlas_verify_op_free(&op);
    atlas_verify_intake_result_free(&res);

    T_OK(atlas_db_prepare(e.db,
                          "SELECT COUNT(DISTINCT uid) FROM verify_actors WHERE class = "
                          "'ATLAS_VERIFIER' AND name = 'memory-reconciler';",
                          &stmt, &err),
         &err);
    T_REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    T_CHECK_MSG(sqlite3_column_int64(stmt, 0) == 2,
                "expected a different actor_version to mint a second actor (a bump is not a "
                "silent no-op), got %lld distinct actors",
                (long long)sqlite3_column_int64(stmt, 0));
    atlas_db_finish(e.db, stmt);

    t8_env_close(&e);
}

/* (f) atlas_memory_observe never opens a transaction. Asserted directly
 * through the internals header's own accessor, and behaviourally: a second
 * connection holding an open read transaction on the same database must not
 * stop the observe phase from succeeding, which a *write* transaction
 * wrapped around its file reads and git process would risk. */
static void test_observe_runs_without_a_transaction(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    const char *repo = fx_repo(&e.fx);
    T_OK(fx_write(repo, "note-a.md", "nothing anchored here\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);

    const char *paths[] = {"note-a.md"};
    atlas_syspolicy pol;
    t8_policy(&pol, ATLAS_MEMORY_SOURCE_REPO_FILE, paths, 1);

    atlas_db *reader = NULL;
    T_OK(atlas_db_open_readonly(atlas_buf_cstr(&e.db_path), &reader, &err), &err);
    T_OK(atlas_db_exec_sql(reader, "BEGIN;", &err), &err);
    T_OK(atlas_db_exec_sql(reader, "SELECT COUNT(*) FROM memory_sources;", &err), &err);

    T_CHECK_MSG(!atlas_db_in_transaction(e.db), "no transaction should be open before observe runs");
    atlas_memory_observation obs;
    atlas_status st = atlas_memory_observe(e.db, &e.repo, fx_data_dir(&e.fx), &pol, &obs, &err);
    T_CHECK_MSG(st == ATLAS_OK,
                "observe must succeed while a second reader holds an open read transaction, got "
                "%s: %s",
                atlas_status_name(st), atlas_err_msg(&err));
    T_CHECK_MSG(!atlas_db_in_transaction(e.db), "observe left a transaction open on its own handle");

    atlas_memory_observation_free(&obs);
    T_OK(atlas_db_exec_sql(reader, "COMMIT;", &err), &err);
    atlas_db_close(reader);

    t8_env_close(&e);
}

/* --- Debt 2, measured rather than reasoned about --------------------------
 *
 * T7's review found that removing one candidate's worth of a 256 KiB source
 * did not remove the cost: 128 candidates of 2048 bytes each, packed with
 * anchor-shaped backtick runs that resolve nothing (so `anchor_count` never
 * reaches its cap of 8 and `atlas_memory_anchor_resolve`'s scan never exits
 * early), is the same total bytes scanned as one un-truncated candidate
 * would have been, and it is all inside `atlas_memory_apply_in_tx`'s one
 * transaction, on the single writer thread. This measures it rather than
 * asserting a bound nobody has set: the number is reported, not gated,
 * because a machine-dependent duration failing a CI run for being 3ms too
 * slow is exactly the kind of bound `CLAUDE.md` warns against stating. */
static void test_cost_debt_apply_duration_at_compiled_worst_case(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    const char *repo = fx_repo(&e.fx);
    atlas_buf content = ATLAS_BUF_INIT;
    /* 128 candidates (list items, so each is its own proposition rather than
     * merging into one paragraph), each packed to just under
     * ATLAS_MEMORY_MAX_PROPOSITION_BYTES with a backtick token that never
     * resolves -- so every one of them is fully scanned for anchors, never
     * truncated and never short-circuited by anchor_count reaching its cap. */
    for (int line = 0; line < 128; line++) {
        T_OK(atlas_buf_appendf(&content, &err, "- "), &err);
        size_t budget = 2000; /* comfortably under the 2048-byte cap with the "- " prefix */
        while (budget > 4) {
            T_OK(atlas_buf_appendf(&content, &err, "`x` "), &err);
            budget -= 4;
        }
        T_OK(atlas_buf_appendf(&content, &err, "\n"), &err);
    }
    T_OK(fx_write_bytes(repo, "note-a.md", strlen("note-a.md"), content.data, content.len, 0644,
                        &err),
         &err);
    atlas_buf_free(&content);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    /* Indexed, deliberately: an unindexed source's candidates are routed to
     * memory_unanchored *without* ever calling atlas_memory_anchor_resolve
     * (this task's own fix for the finding above), which would make this
     * measurement report the fast path instead of the one Debt 2 is about. */
    t8_seed_file(&e, "note-a.md", "1111111111111111111111111111111111111111111111111111111111111111",
                &err);

    const char *paths[] = {"note-a.md"};
    atlas_syspolicy pol;
    t8_policy(&pol, ATLAS_MEMORY_SOURCE_REPO_FILE, paths, 1);

    char now[64];
    atlas_now_iso8601(now, sizeof now);
    atlas_memory_observation *obs = malloc(sizeof *obs);
    T_REQUIRE(obs != NULL);
    T_OK(atlas_memory_observe(e.db, &e.repo, fx_data_dir(&e.fx), &pol, obs, &err), &err);
    T_CHECK_MSG(obs->source_count == 1 && obs->sources[0].candidate_count == 128,
                "expected the compiled ceiling of 128 candidates from one source, got %zu",
                obs->source_count == 1 ? obs->sources[0].candidate_count : 0);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    atlas_memory_pass_result result;
    T_OK(atlas_db_begin(e.db, &err), &err);
    T_OK(atlas_memory_apply_in_tx(e.db, &e.repo, obs, &pol, now, &result, &err), &err);
    T_OK(atlas_db_commit(e.db, &err), &err);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
               (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;

    atlas_test_note(
        "Debt 2, all-refusing case observed: one 256 KiB-class source (128 candidates x ~2000 "
        "anchor-scanned bytes, all unresolving) cost %.1f ms inside atlas_memory_apply_in_tx's "
        "one write transaction (unanchored=%zu). This is NOT production's steady state -- see "
        "the all-resolving measurement below and the T8 report.",
        ms, result.unanchored);
    T_CHECK_MSG(result.unanchored == 128, "expected every candidate to land unanchored, got %zu",
                result.unanchored);

    atlas_memory_observation_free(obs);
    free(obs);
    t8_env_close(&e);
}

/* I3: the measurement above ran zero intake ops -- every candidate refused
 * to resolve, so `emit_candidate` never fired. The claim content key hashes
 * `basis_commit` (`intake.c:643`), so every head move re-mints every claim;
 * production's steady state after any commit is precisely the *all-
 * resolving* case, and that is the one T10 needs timed before it can weigh
 * this job against the real 2000 ms deadline (ATLAS_HOOK_IPC_TIMEOUT_MS /
 * ATLAS_WRITER_YIELD_GRACE_MS, not the 4000 ms an earlier review round
 * carried into the season ledger before correcting it). This builds
 * Decision 10's full
 * stated worst case -- 16 sources, each with 128 candidates that *do*
 * resolve a PATH anchor and run CLAIM_CREATE, EVIDENCE_ADD, ATTESTATION_ADD
 * and the DEPENDENCY_ADD check for real -- and times the same one write
 * transaction. */
static void test_cost_debt_all_resolving_case_at_compiled_worst_case(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    const char *repo = fx_repo(&e.fx);
    T_OK(fx_mkdir(repo, "src", &err), &err);
    T_OK(fx_mkdir(repo, "src/db", &err), &err);
    T_OK(fx_write(repo, "src/db/db_orch.c", "int x;\n", &err), &err);

    const char *paths[ATLAS_MEMORY_MAX_SOURCES];
    char names[ATLAS_MEMORY_MAX_SOURCES][32];
    for (int s = 0; s < (int)ATLAS_MEMORY_MAX_SOURCES; s++) {
        atlas_buf content = ATLAS_BUF_INIT;
        for (int line = 0; line < 128; line++) {
            /* A unique marker per candidate (source and line), so each
             * resolves to its own claim -- "every head move re-mints every
             * claim" is every claim being *new*, not one shared claim
             * looked up 2048 times. Padded with the same resolving PATH
             * anchor repeated to fill the candidate, so the full anchor
             * scan and a real add_anchor dedup check both run on every
             * byte, exactly as the all-refusing measurement did. */
            T_OK(atlas_buf_appendf(&content, &err, "- s%02d l%04d ", s, line), &err);
            size_t budget = 1980;
            while (budget > 24) {
                T_OK(atlas_buf_appendf(&content, &err, "`src/db/db_orch.c` "), &err);
                budget -= 20;
            }
            T_OK(atlas_buf_appendf(&content, &err, "\n"), &err);
        }
        (void)snprintf(names[s], sizeof names[s], "note-%02d.md", s);
        T_OK(fx_write_bytes(repo, names[s], strlen(names[s]), content.data, content.len, 0644,
                            &err),
             &err);
        atlas_buf_free(&content);
        paths[s] = names[s];
    }
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "src/db/db_orch.c", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                &err);
    for (int s = 0; s < (int)ATLAS_MEMORY_MAX_SOURCES; s++) {
        t8_seed_file(&e, names[s], "3333333333333333333333333333333333333333333333333333333333333333",
                    &err);
    }

    atlas_syspolicy pol;
    t8_policy(&pol, ATLAS_MEMORY_SOURCE_REPO_FILE, paths, ATLAS_MEMORY_MAX_SOURCES);

    char now[64];
    atlas_now_iso8601(now, sizeof now);
    atlas_memory_observation *obs = malloc(sizeof *obs);
    T_REQUIRE(obs != NULL);
    T_OK(atlas_memory_observe(e.db, &e.repo, fx_data_dir(&e.fx), &pol, obs, &err), &err);
    T_REQUIRE(obs->source_count == ATLAS_MEMORY_MAX_SOURCES);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    atlas_memory_pass_result result;
    T_OK(atlas_db_begin(e.db, &err), &err);
    T_OK(atlas_memory_apply_in_tx(e.db, &e.repo, obs, &pol, now, &result, &err), &err);
    T_OK(atlas_db_commit(e.db, &err), &err);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
               (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;

    T_CHECK_MSG(result.claims_created == ATLAS_MEMORY_MAX_SOURCES * 128,
                "expected every one of %d candidates to resolve and create a claim, got %zu "
                "created (%zu unanchored)",
                (int)(ATLAS_MEMORY_MAX_SOURCES * 128), result.claims_created, result.unanchored);
    atlas_test_note(
        "Debt 2, all-resolving case observed: Decision 10's full worst case -- 16 sources x 128 "
        "candidates, every one resolving a PATH anchor and running CLAIM_CREATE+EVIDENCE_ADD+"
        "ATTESTATION_ADD+DEPENDENCY_ADD for real -- cost %.1f ms inside atlas_memory_apply_in_tx's "
        "one write transaction (claims_created=%zu). This is production's steady state after any "
        "commit, since the claim content key hashes basis_commit. See the T8 report for what T10 "
        "should do with this number against the real 2000 ms deadline (ATLAS_HOOK_IPC_TIMEOUT_MS "
        "/ ATLAS_WRITER_YIELD_GRACE_MS).",
        ms, result.claims_created);

    atlas_memory_observation_free(obs);
    free(obs);
    t8_env_close(&e);
}

/* EXTERNAL_*: T8 never reads one itself. `test_verify_intake.c`'s own
 * `insert_memory_version` shape, reused: a stored `memory_sources` +
 * `memory_source_versions` pair inserted directly, exactly as a future
 * `memory.put` (T11) would leave it, so the observe phase has something to
 * read back instead of a path it would refuse to open. */
static void test_external_source_reads_the_stored_version_not_the_disk(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    const char *repo = fx_repo(&e.fx);
    T_OK(fx_mkdir(repo, "src", &err), &err);
    T_OK(fx_mkdir(repo, "src/db", &err), &err);
    T_OK(fx_write(repo, "src/db/db_orch.c", "int x;\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "src/db/db_orch.c", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                &err);

    atlas_buf sql = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(
             &sql, &err,
             "INSERT INTO memory_sources(repo_id, source_uid, cls, path_raw, path_text,"
             "  registered_at) VALUES(%lld, 'm0123456789abcdef0123456789abcdef',"
             "  'EXTERNAL_FILE', CAST('/home/u/notes.md' AS BLOB), '/home/u/notes.md', 't0');"
             "INSERT INTO memory_source_versions(source_id, version_uid, commit_oid, blob_oid,"
             "  content_sha256, content_bytes, content, observed_at, recorded_at, read_by_uid)"
             " VALUES(last_insert_rowid(), 'v0123456789abcdef0123456789abcdef', '', '',"
             "  '1111111111111111111111111111111111111111111111111111111111111111', 36,"
             "  'the daemon reads `src/db/db_orch.c`', 't1', 't1', 0);",
             (long long)e.repo_id),
         &err);
    T_OK(atlas_db_exec_sql(e.db, atlas_buf_cstr(&sql), &err), &err);
    atlas_buf_free(&sql);

    atlas_syspolicy pol;
    memset(&pol, 0, sizeof pol);
    pol.state = ATLAS_SYSPOLICY_SYSTEM;
    pol.memory_source_count = 1;
    pol.memory_sources[0].cls = ATLAS_MEMORY_SOURCE_EXTERNAL_FILE;
    (void)snprintf(pol.memory_sources[0].path, sizeof pol.memory_sources[0].path, "%s",
                   "/home/u/notes.md");

    atlas_memory_pass_result result;
    t8_run_pass(&e, &pol, &result, &err);
    T_CHECK_MSG(result.claims_created == 1, "expected one claim from the stored version, got %zu",
                result.claims_created);
    T_CHECK_MSG(result.versions_added == 0,
                "T8 must never write a version row for an EXTERNAL_* source, got %zu",
                result.versions_added);

    char sqlbuf[256];
    (void)snprintf(sqlbuf, sizeof sqlbuf,
                  "SELECT COUNT(*) FROM verify_evidence WHERE repo_id = %lld"
                  "  AND path_text = '/home/u/notes.md';",
                  (long long)e.repo_id);
    T_CHECK_MSG(t8_scalar(&e, sqlbuf, &err) == 1,
                "expected the evidence row to carry the stored version's own path");

    t8_env_close(&e);
}

/* A REPO_DIR source with two children, each carrying a different anchored
 * bullet: both share one actor (Decision 1's "one actor per registered
 * source"), each gets its own claim and its own evidence whose path_text is
 * the source's own path joined with the child's own name, and both share the
 * pass's one candidate budget rather than each getting ATLAS_MEMORY_MAX_
 * PROPOSITIONS of their own. */
static void test_repo_dir_source_versions_each_child_independently(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    const char *repo = fx_repo(&e.fx);
    T_OK(fx_mkdir(repo, "src", &err), &err);
    T_OK(fx_mkdir(repo, "src/db", &err), &err);
    T_OK(fx_write(repo, "src/db/db_orch.c", "int x;\n", &err), &err);
    T_OK(fx_mkdir(repo, "src/gw", &err), &err);
    T_OK(fx_write(repo, "src/gw/gateway.c", "int y;\n", &err), &err);
    T_OK(fx_mkdir(repo, ".claude", &err), &err);
    T_OK(fx_mkdir(repo, ".claude/memories", &err), &err);
    T_OK(fx_write(repo, ".claude/memories/a.md", "the daemon reads `src/db/db_orch.c`", &err), &err);
    T_OK(fx_write(repo, ".claude/memories/b.md", "the gateway reads `src/gw/gateway.c`", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "src/db/db_orch.c", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                &err);
    t8_seed_file(&e, "src/gw/gateway.c", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                &err);
    t8_seed_file(&e, ".claude/memories/a.md",
                "1111111111111111111111111111111111111111111111111111111111111111", &err);
    t8_seed_file(&e, ".claude/memories/b.md",
                "2222222222222222222222222222222222222222222222222222222222222222", &err);

    const char *paths[] = {".claude/memories"};
    atlas_syspolicy pol;
    t8_policy(&pol, ATLAS_MEMORY_SOURCE_REPO_DIR, paths, 1);

    atlas_memory_pass_result result;
    t8_run_pass(&e, &pol, &result, &err);
    T_CHECK_MSG(result.claims_created == 2, "expected two distinct claims, got %zu",
                result.claims_created);
    T_CHECK_MSG(result.versions_added == 2, "expected two version rows, one per child, got %zu",
                result.versions_added);

    char sql[256];
    (void)snprintf(sql, sizeof sql,
                  "SELECT COUNT(DISTINCT actor_id) FROM verify_attestations a"
                  " JOIN verify_claims c ON c.id = a.claim_id WHERE c.repo_id = %lld;",
                  (long long)e.repo_id);
    T_CHECK_MSG(t8_scalar(&e, sql, &err) == 1,
                "expected both children's attestations to share one actor (one registered "
                "source)");
    (void)snprintf(sql, sizeof sql,
                  "SELECT COUNT(*) FROM verify_evidence WHERE repo_id = %lld"
                  "  AND path_text = '.claude/memories/a.md';",
                  (long long)e.repo_id);
    T_CHECK_MSG(t8_scalar(&e, sql, &err) == 1,
                "expected the first child's own path, joined from the source and the child name");
    (void)snprintf(sql, sizeof sql,
                  "SELECT COUNT(*) FROM verify_evidence WHERE repo_id = %lld"
                  "  AND path_text = '.claude/memories/b.md';",
                  (long long)e.repo_id);
    T_CHECK_MSG(t8_scalar(&e, sql, &err) == 1, "expected the second child's own path");

    t8_env_close(&e);
}


/* C1 regression: two byte-identical children of one REPO_DIR share a
 * `source_id`, so `version_exists` merges them into one version row --
 * migration 29's own `UNIQUE(source_id, content_sha256, observed_at)`, and
 * defensible on its own. What was not defensible: both children's ordinals
 * restart at 0 (T7 splits each item independently), so the second
 * `memory_unanchored` insert collides with the first under `UNIQUE
 * (source_version_id, ordinal)`, `INSERT OR IGNORE` silently drops it, and
 * the pass still counted it -- `result.unanchored == 2` while the table held
 * one row. Driven end to end against the exact shape. */
static void test_two_identical_children_land_as_one_unanchored_row(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    const char *repo = fx_repo(&e.fx);
    T_OK(fx_mkdir(repo, ".claude", &err), &err);
    T_OK(fx_mkdir(repo, ".claude/memories", &err), &err);
    const char *prose = "just some prose with nothing in it to anchor to";
    T_OK(fx_write(repo, ".claude/memories/a.md", prose, &err), &err);
    T_OK(fx_write(repo, ".claude/memories/b.md", prose, &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, ".claude/memories/a.md",
                "1111111111111111111111111111111111111111111111111111111111111111", &err);
    t8_seed_file(&e, ".claude/memories/b.md",
                "1111111111111111111111111111111111111111111111111111111111111111", &err);

    const char *paths[] = {".claude/memories"};
    atlas_syspolicy pol;
    t8_policy(&pol, ATLAS_MEMORY_SOURCE_REPO_DIR, paths, 1);

    atlas_memory_pass_result result;
    t8_run_pass(&e, &pol, &result, &err);

    int64_t stored = t8_scalar(&e, "SELECT COUNT(*) FROM memory_unanchored;", &err);
    T_CHECK_MSG((int64_t)result.unanchored == stored,
                "the reported unanchored count (%zu) must equal what memory_unanchored actually "
                "holds (%lld)",
                result.unanchored, (long long)stored);
    T_CHECK_MSG(stored == 1,
                "two byte-identical children share one merged version row and one ordinal, so "
                "exactly one row should land, got %lld",
                (long long)stored);

    t8_env_close(&e);
}

/* I1: a repository this pass could not fully see must not read the same as
 * one that changed nothing. Compares a healthy no-op pass against one whose
 * sole source answers NO_MIRROR (a scanner-named repository with no
 * complete mirror, T6's own outcome for exactly this) -- both give
 * `generation == 0`, and only `read_obstacles` tells them apart. */
static void test_read_obstacle_is_distinguishable_from_unchanged(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    const char *repo = fx_repo(&e.fx);
    T_OK(fx_write(repo, "note-a.md", "nothing anchored here\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);

    const char *paths[] = {"note-a.md"};
    atlas_syspolicy pol;
    t8_policy(&pol, ATLAS_MEMORY_SOURCE_REPO_FILE, paths, 1);

    /* Pass 1 legitimately owes a generation -- the version did not exist
     * before this pass, so its content genuinely moved from nothing to
     * something. Pass 2, over the same unchanged bytes, is the real
     * "healthy, nothing new" baseline this test compares the blind pass
     * against. */
    atlas_memory_pass_result seed;
    t8_run_pass(&e, &pol, &seed, &err);
    atlas_memory_pass_result healthy;
    t8_run_pass(&e, &pol, &healthy, &err);
    T_CHECK_MSG(healthy.generation == 0 && healthy.read_obstacles == 0,
                "expected a healthy no-op pass: generation 0, read_obstacles 0, got generation "
                "%lld read_obstacles %zu",
                (long long)healthy.generation, healthy.read_obstacles);

    /* A scanner is named and no complete mirror exists -- read.c's own
     * NO_MIRROR outcome, T6's `test_repo_file_no_mirror_reports_the_outcome`
     * shape. */
    e.repo.scanner_uid = (int64_t)geteuid() + 1;
    e.repo.mirror_complete = false;

    atlas_memory_pass_result blind;
    t8_run_pass(&e, &pol, &blind, &err);
    T_CHECK_MSG(blind.generation == 0, "a blind read must not fabricate a change, got generation %lld",
                (long long)blind.generation);
    T_CHECK_MSG(blind.read_obstacles == 1,
                "expected the NO_MIRROR outcome to be counted as a read obstacle, got %zu",
                blind.read_obstacles);
    /* Round 3: the outcome comes first in last_read_obstacle, so it survives
     * even a long path being truncated away. */
    T_CHECK_MSG(strncmp(blind.last_read_obstacle, "NO_MIRROR", strlen("NO_MIRROR")) == 0,
                "expected last_read_obstacle to lead with the outcome, got \"%s\"",
                blind.last_read_obstacle);

    t8_env_close(&e);
}

/* I4: a genuine, unexpected write-point failure for one source must not
 * discard the other fifteen. Driven with a real SQLite fault -- a temp
 * trigger that raises on the poisoned source's own claim text -- rather than
 * asserted: this is exactly the shape of an obstacle T8's own code cannot
 * anticipate (a constraint, a full disk, a corrupt page), which is the
 * point of the savepoint recovering from it rather than needing to
 * enumerate every cause in advance. */
static void test_one_source_obstacle_does_not_discard_the_rest(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    const char *repo = fx_repo(&e.fx);
    T_OK(fx_mkdir(repo, "src", &err), &err);
    T_OK(fx_mkdir(repo, "src/db", &err), &err);
    T_OK(fx_write(repo, "src/db/db_orch.c", "int x;\n", &err), &err);
    T_OK(fx_write(repo, "note-a.md", "the daemon reads `src/db/db_orch.c`", &err), &err);
    T_OK(fx_write(repo, "note-poison.md", "POISON the daemon reads `src/db/db_orch.c`", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "src/db/db_orch.c", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                &err);
    t8_seed_file(&e, "note-a.md", "1111111111111111111111111111111111111111111111111111111111111111",
                &err);
    t8_seed_file(&e, "note-poison.md",
                "2222222222222222222222222222222222222222222222222222222222222222", &err);

    T_OK(atlas_db_exec_sql(e.db,
                           "CREATE TEMP TRIGGER t8_poison BEFORE INSERT ON verify_claims"
                           " WHEN NEW.text LIKE 'POISON%'"
                           " BEGIN SELECT RAISE(ABORT, 'I4 test: injected write-point failure'); END;",
                           &err),
         &err);

    const char *paths[] = {"note-a.md", "note-poison.md"};
    atlas_syspolicy pol;
    t8_policy(&pol, ATLAS_MEMORY_SOURCE_REPO_FILE, paths, 2);

    atlas_memory_pass_result result;
    t8_run_pass(&e, &pol, &result, &err);

    T_CHECK_MSG(result.claims_created == 1,
                "expected the healthy source's claim despite the poisoned one, got %zu",
                result.claims_created);
    T_CHECK_MSG(result.intake_bound_hits == 1,
                "expected the poisoned source to be counted as an obstacle, got %zu",
                result.intake_bound_hits);
    T_CHECK_MSG(strstr(result.last_obstacle, "I4 test") != NULL,
                "expected last_obstacle to carry the injected failure's own message, got \"%s\"",
                result.last_obstacle);
    /* Round 2, New-I2: sources_seen is an observation (both sources were
     * attempted), not a write -- it must not be rolled back along with the
     * poisoned source's SQL. */
    T_CHECK_MSG(result.sources_seen == 2,
                "expected both sources counted as seen despite one failing, got %zu",
                result.sources_seen);

    char sql[256];
    (void)snprintf(sql, sizeof sql,
                  "SELECT COUNT(*) FROM verify_claims WHERE repo_id = %lld AND text LIKE 'POISON%%';",
                  (long long)e.repo_id);
    T_CHECK_MSG(t8_scalar(&e, sql, &err) == 0,
                "the poisoned source's partial write must have been rolled back, not committed");
    (void)snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM verify_claims WHERE repo_id = %lld;",
                  (long long)e.repo_id);
    T_CHECK_MSG(t8_scalar(&e, sql, &err) == 1,
                "expected exactly the healthy source's one claim to survive");

    T_OK(atlas_db_exec_sql(e.db, "DROP TRIGGER t8_poison;", &err), &err);
    t8_env_close(&e);
}


/* A scalar SQL function whose only job is to interrupt the connection it is
 * registered on -- `sqlite3_user_data` carries the handle. Used to construct
 * a *real* SQLITE_INTERRUPT rather than a RAISE(ABORT): per SQLite's own
 * documented behaviour for sqlite3_interrupt(), "if the interrupted SQL
 * statement is inside an explicit transaction, then the entire transaction
 * is rolled back automatically" -- exactly the outer-transaction-ending
 * case RAISE(ABORT) cannot reach, because RAISE(ABORT) only ever unwinds to
 * the nearest savepoint. */
static void t8_interrupt_fn(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    (void)argv;
    sqlite3 *h = (sqlite3 *)sqlite3_user_data(ctx);
    sqlite3_interrupt(h);
    sqlite3_result_null(ctx);
}

/* New-C1 (review round 2): a SQLite error that ends the *outer* transaction,
 * not merely a savepoint, must not be treated as "this source failed, the
 * others are fine". Driven with the real fault above rather than asserted:
 * two sources, the poisoned one's own CLAIM_CREATE insert triggers a real
 * interrupt, and the whole transaction goes with it. */
static void test_outer_transaction_ending_fault_abandons_the_pass(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    const char *repo = fx_repo(&e.fx);
    T_OK(fx_mkdir(repo, "src", &err), &err);
    T_OK(fx_mkdir(repo, "src/db", &err), &err);
    T_OK(fx_write(repo, "src/db/db_orch.c", "int x;\n", &err), &err);
    T_OK(fx_write(repo, "note-a.md", "the daemon reads `src/db/db_orch.c`", &err), &err);
    T_OK(fx_write(repo, "note-poison.md", "POISON the daemon reads `src/db/db_orch.c`", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "src/db/db_orch.c", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                &err);
    t8_seed_file(&e, "note-a.md", "1111111111111111111111111111111111111111111111111111111111111111",
                &err);
    t8_seed_file(&e, "note-poison.md",
                "2222222222222222222222222222222222222222222222222222222222222222", &err);

    T_REQUIRE(sqlite3_create_function_v2(e.db->h, "t8_interrupt", 0, SQLITE_UTF8, e.db->h,
                                        t8_interrupt_fn, NULL, NULL, NULL) == SQLITE_OK);
    T_OK(atlas_db_exec_sql(e.db,
                           "CREATE TEMP TRIGGER t8_boom BEFORE INSERT ON verify_claims"
                           " WHEN NEW.text LIKE 'POISON%'"
                           " BEGIN SELECT t8_interrupt(); END;",
                           &err),
         &err);

    const char *paths[] = {"note-a.md", "note-poison.md"};
    atlas_syspolicy pol;
    t8_policy(&pol, ATLAS_MEMORY_SOURCE_REPO_FILE, paths, 2);

    char now[64];
    atlas_now_iso8601(now, sizeof now);
    atlas_memory_observation *obs = malloc(sizeof *obs);
    T_REQUIRE(obs != NULL);
    T_OK(atlas_memory_observe(e.db, &e.repo, fx_data_dir(&e.fx), &pol, obs, &err), &err);

    T_OK(atlas_db_begin(e.db, &err), &err);
    atlas_memory_pass_result result;
    memset(&result, 0, sizeof result);
    atlas_status st = atlas_memory_apply_in_tx(e.db, &e.repo, obs, &pol, now, &result, &err);

    T_CHECK_MSG(st != ATLAS_OK,
                "expected the pass to report failure once the outer transaction ended, got %s",
                atlas_status_name(st));
    T_CHECK_MSG(!atlas_db_in_transaction(e.db),
                "expected Atlas' own tx_depth to be resynchronised once SQLite ended the "
                "transaction, but atlas_db_in_transaction still reports true");

    /* "The pass failed" and "the pass left nothing behind" are different
     * claims -- assert the second too. The whole outer transaction ended,
     * so even note-a.md's own already-released, healthy work never
     * committed: nothing was ever persisted, not merely nothing usable. */
    char sql[256];
    (void)snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM verify_claims WHERE repo_id = %lld;",
                  (long long)e.repo_id);
    T_CHECK_MSG(t8_scalar(&e, sql, &err) == 0,
                "expected the whole transaction's work, including the healthy source's, to be "
                "gone -- not merely the poisoned source's");

    atlas_memory_observation_free(obs);
    free(obs);
    t8_env_close(&e);
}

/* --- T9: generations, the semantic diff, and drift -------------------------
 *
 * All of these extend `t8env`/`t8_run_pass` above rather than building a
 * second fixture shape.
 */

/* The most recent matching row: on a `COMMIT`-caused pass every proposition
 * re-mints (§27's content key hashes `basis_commit`,
 * `src/verify/intake.c:643`), so two rows can share one `text` and only the
 * newest is "this pass's own claim". */
static void t9_claim_uid_for_text(t8env *e, const char *text, atlas_buf *uid_out, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    T_OK(atlas_db_prepare(e->db,
                          "SELECT uid FROM verify_claims WHERE repo_id = ?1 AND text = ?2"
                          " ORDER BY id DESC LIMIT 1;",
                          &stmt, err),
         err);
    T_REQUIRE(sqlite3_bind_int64(stmt, 1, e->repo_id) == SQLITE_OK);
    T_REQUIRE(sqlite3_bind_text(stmt, 2, text, -1, SQLITE_TRANSIENT) == SQLITE_OK);
    T_REQUIRE_MSG(sqlite3_step(stmt) == SQLITE_ROW, "no claim found for text: %s", text);
    T_OK(atlas_buf_set_str(uid_out, (const char *)sqlite3_column_text(stmt, 0), err), err);
    atlas_db_finish(e->db, stmt);
}

/* Every column of one `verify_claims` row, concatenated into one string --
 * acceptance item 2's own literal test, "`SELECT *` captured and compared".
 * `id` and `created_at` are included deliberately: a caller asking whether a
 * row is byte-for-byte the same row, not merely an equivalent one, has to
 * mean its identity and its timestamp too. */
static void t9_claim_row_snapshot(t8env *e, const char *claim_uid, atlas_buf *out, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    T_OK(atlas_db_prepare(e->db,
                          "SELECT id || '|' || uid || '|' || repo_id || '|' || repo_identity_hash"
                          "  || '|' || document_id || '|' || revision_id || '|' || domain"
                          "  || '|' || text || '|' || scope_note || '|' || semantics"
                          "  || '|' || verifier || '|' || verifier_input || '|' || basis_commit"
                          "  || '|' || environment || '|' || created_at"
                          "  || '|' || superseded_by_claim_id || '|' || content_key"
                          "  || '|' || created_by_actor_id"
                          " FROM verify_claims WHERE uid = ?1;",
                          &stmt, err),
         err);
    T_REQUIRE(sqlite3_bind_text(stmt, 1, claim_uid, -1, SQLITE_TRANSIENT) == SQLITE_OK);
    T_REQUIRE_MSG(sqlite3_step(stmt) == SQLITE_ROW, "no claim row for uid: %s", claim_uid);
    T_OK(atlas_buf_set_str(out, (const char *)sqlite3_column_text(stmt, 0), err), err);
    atlas_db_finish(e->db, stmt);
}

static void t9_generation_latest(t8env *e, atlas_buf *cause_out, atlas_buf *head_out,
                                 int64_t *generation_out, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    T_OK(atlas_db_prepare(e->db,
                          "SELECT generation, cause, head_commit FROM memory_generations"
                          " WHERE repo_id = ?1 ORDER BY generation DESC LIMIT 1;",
                          &stmt, err),
         err);
    T_REQUIRE(sqlite3_bind_int64(stmt, 1, e->repo_id) == SQLITE_OK);
    T_REQUIRE_MSG(sqlite3_step(stmt) == SQLITE_ROW, "no generation recorded for this repository");
    if (generation_out != NULL) {
        *generation_out = sqlite3_column_int64(stmt, 0);
    }
    if (cause_out != NULL) {
        T_OK(atlas_buf_set_str(cause_out, (const char *)sqlite3_column_text(stmt, 1), err), err);
    }
    if (head_out != NULL) {
        T_OK(atlas_buf_set_str(head_out, (const char *)sqlite3_column_text(stmt, 2), err), err);
    }
    atlas_db_finish(e->db, stmt);
}

/* `*found_out` is whether this exact generation carries a diff row for this
 * exact claim uid at all -- a caller asserting "no row" passes `kind_out ==
 * NULL` and reads only `*found_out`. */
static void t9_diff_kind_for(t8env *e, int64_t generation, const char *claim_uid, atlas_buf *kind_out,
                             atlas_buf *reason_out, bool *found_out, atlas_err *err) {
    *found_out = false;
    sqlite3_stmt *stmt = NULL;
    T_OK(atlas_db_prepare(e->db,
                          "SELECT d.kind, d.reason FROM memory_claim_diffs d"
                          "  JOIN memory_generations g ON g.id = d.generation_id"
                          " WHERE g.repo_id = ?1 AND g.generation = ?2 AND d.claim_uid = ?3;",
                          &stmt, err),
         err);
    T_REQUIRE(sqlite3_bind_int64(stmt, 1, e->repo_id) == SQLITE_OK);
    T_REQUIRE(sqlite3_bind_int64(stmt, 2, generation) == SQLITE_OK);
    T_REQUIRE(sqlite3_bind_text(stmt, 3, claim_uid, -1, SQLITE_TRANSIENT) == SQLITE_OK);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        *found_out = true;
        if (kind_out != NULL) {
            T_OK(atlas_buf_set_str(kind_out, (const char *)sqlite3_column_text(stmt, 0), err), err);
        }
        if (reason_out != NULL) {
            T_OK(atlas_buf_set_str(reason_out, (const char *)sqlite3_column_text(stmt, 1), err), err);
        }
    }
    atlas_db_finish(e->db, stmt);
}

/* The hash a real scan would have written after a real commit changed the
 * path's bytes -- an opaque, distinct string is enough here (T8's own fixture
 * convention: no verifier in this suite ever checks a hash against real
 * content), but it must be a *different* string from whatever the path was
 * seeded with, or nothing in `verifier_input` moves. */
static void t9_update_file_hash(t8env *e, const char *path_text, const char *new_hash,
                                atlas_err *err) {
    char sql[512];
    (void)snprintf(sql, sizeof sql,
                  "UPDATE files SET content_hash = '%s' WHERE repo_id = %lld AND path_text = '%s';",
                  new_hash, (long long)e->repo_id, path_text);
    T_OK(atlas_db_exec_sql(e->db, sql, err), err);
}

static void t9_op_repo(atlas_decision_op *op, atlas_err *err) {
    T_OK(atlas_buf_set_str(&op->repo_name, "proj", err), err);
}

static void t9_propose(t8env *e, const char *title, atlas_buf *uid_out, atlas_err *err) {
    atlas_decision_op op;
    atlas_decision_op_init(&op, ATLAS_DECISION_OP_PROPOSE);
    t9_op_repo(&op, err);
    T_OK(atlas_buf_set_str(&op.revision.title, title, err), err);
    T_OK(atlas_buf_set_str(&op.revision.decision_text, "a body for the T9 fixture", err), err);
    op.revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_OK(atlas_decision_apply(e->db, &op, &res, err), err);
    T_OK(atlas_buf_set(uid_out, res.uid.data, res.uid.len, err), err);
    atlas_decision_result_free(&res);
    atlas_decision_op_free(&op);
}

/* The whole challenge/approve dance, `test_decision_lifecycle.c`'s own
 * shape: an operator capability is issued and immediately spent, since
 * nothing in this file is testing the operator channel itself. */
static void t9_approve(t8env *e, const char *uid, int64_t revision_no, atlas_err *err) {
    atlas_decision_op cop;
    atlas_decision_op_init(&cop, ATLAS_DECISION_OP_CHALLENGE);
    t9_op_repo(&cop, err);
    T_OK(atlas_buf_set_str(&cop.uid, uid, err), err);
    cop.expect_revision_no = revision_no;
    cop.intent = ATLAS_DECISION_INTENT_APPROVE;
    atlas_decision_result cres;
    atlas_decision_result_init(&cres);
    T_OK(atlas_decision_apply(e->db, &cop, &cres, err), err);

    atlas_decision_op aop;
    atlas_decision_op_init(&aop, ATLAS_DECISION_OP_APPROVE);
    t9_op_repo(&aop, err);
    T_OK(atlas_buf_set_str(&aop.uid, uid, err), err);
    T_OK(atlas_buf_set(&aop.token, cres.token.data, cres.token.len, err), err);
    T_OK(atlas_buf_set_str(&aop.confirmation, cres.confirm, err), err);
    atlas_decision_result ares;
    atlas_decision_result_init(&ares);
    T_OK(atlas_decision_apply(e->db, &aop, &ares, err), err);
    T_CHECK_MSG(ares.state == ATLAS_DECISION_APPROVED, "the approval did not land");

    atlas_decision_result_free(&ares);
    atlas_decision_op_free(&aop);
    atlas_decision_result_free(&cres);
    atlas_decision_op_free(&cop);
}

/* Ported verbatim in shape from `test_verify_absence.c`'s own `seed_generation`
 * -- the exact recipe that already drives `atlas.symbol_present` to a genuine
 * FAIL over a *complete* generation in that file, reused here rather than
 * re-derived, because getting A9.2.2's coverage gate right twice independently
 * is how it gets right once and wrong once. Returns the new generation's own
 * row id, and leaves `sem_current` pointed at it. */
static int64_t t9_seed_sem_generation(t8env *e, bool has_symbol, const char *symbol_usr,
                                      const char *symbol_name, atlas_err *err) {
    char sql[1024];
    (void)snprintf(sql, sizeof sql,
                  "INSERT INTO repo_index_state(repo_id, generation, last_complete_generation,"
                  "  last_reconcile_at, last_complete_at, event_gap, pending_full_reconcile)"
                  "  VALUES(%lld, 1, 1, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 0, 0)"
                  "  ON CONFLICT(repo_id) DO UPDATE SET last_complete_generation = 1,"
                  "    event_gap = 0, pending_full_reconcile = 0;",
                  (long long)e->repo_id);
    T_OK(atlas_db_exec_sql(e->db, sql, err), err);
    (void)snprintf(sql, sizeof sql,
                  "INSERT INTO sem_generations(repo_id, commit_id, status, started_at, completed_at,"
                  "  tu_total, tu_complete, tu_partial, tu_failed, tu_unsupported,"
                  "  analyzer_id, analyzer_version,"
                  "  scope_discovery, scope_candidates, scope_covered, scope_uncovered,"
                  "  discovery, input_count)"
                  "  VALUES(%lld, '%s', 'COMPLETE', '2026-01-01T00:00:00Z', '2026-01-01T00:01:00Z',"
                  "         1, 1, 0, 0, 0, '%s', %d, 'DECLARED', 1, 1, 0, 'COMPLETE', 1);",
                  (long long)e->repo_id, e->repo.scanned_head, ATLAS_SEM_ANALYZER_ID,
                  (int)ATLAS_SEM_ANALYZER_VERSION);
    T_OK(atlas_db_exec_sql(e->db, sql, err), err);
    int64_t gen = t8_scalar(e, "SELECT COALESCE(MAX(id), 0) FROM sem_generations;", err);
    (void)snprintf(sql, sizeof sql,
                  "INSERT INTO sem_current(repo_id, generation_id) VALUES(%lld, %lld)"
                  "  ON CONFLICT(repo_id) DO UPDATE SET generation_id = %lld;",
                  (long long)e->repo_id, (long long)gen, (long long)gen);
    T_OK(atlas_db_exec_sql(e->db, sql, err), err);
    if (has_symbol) {
        (void)snprintf(sql, sizeof sql,
                      "INSERT INTO sem_symbols(generation_id, usr, name, kind, linkage, file_text,"
                      "  line, is_definition, external, evidence)"
                      "  VALUES(%lld, '%s', '%s', 'FUNCTION', 'INTERNAL', 'src/a.c', 10, 1, 0,"
                      "  'PROVEN');",
                      (long long)gen, symbol_usr, symbol_name);
        T_OK(atlas_db_exec_sql(e->db, sql, err), err);
    }
    return gen;
}

/* fix-round-2, New Important 1's own fixture: an *incomplete* generation --
 * `tu_failed = 1` makes `sem_state`'s own `units_complete` false
 * (`src/verify/detverify.c:252-254`), which is what `sem_coverage` folds
 * into `ATLAS_COVDIM_SEMANTIC_GENERATION`/`GENERATED_SOURCE` being PARTIAL
 * rather than COMPLETE -- insufficient for `SYMBOL_PRESENT`'s own coverage
 * requirement (`DIMS_SYMBOL`), so a negative raw result settles
 * `UNAVAILABLE` (`settle()`) rather than a genuine `FAIL`. No `sem_symbols`
 * row either way: this is never about whether the symbol is there, only
 * about whether Atlas could tell. `sem_current` still points at it, so it is
 * the generation every read in this pass sees. */
static int64_t t9_seed_incomplete_sem_generation(t8env *e, atlas_err *err) {
    char sql[1024];
    (void)snprintf(sql, sizeof sql,
                  "INSERT INTO repo_index_state(repo_id, generation, last_complete_generation,"
                  "  last_reconcile_at, last_complete_at, event_gap, pending_full_reconcile)"
                  "  VALUES(%lld, 1, 1, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 0, 0)"
                  "  ON CONFLICT(repo_id) DO UPDATE SET last_complete_generation = 1,"
                  "    event_gap = 0, pending_full_reconcile = 0;",
                  (long long)e->repo_id);
    T_OK(atlas_db_exec_sql(e->db, sql, err), err);
    (void)snprintf(sql, sizeof sql,
                  "INSERT INTO sem_generations(repo_id, commit_id, status, started_at, completed_at,"
                  "  tu_total, tu_complete, tu_partial, tu_failed, tu_unsupported,"
                  "  analyzer_id, analyzer_version,"
                  "  scope_discovery, scope_candidates, scope_covered, scope_uncovered,"
                  "  discovery, input_count)"
                  "  VALUES(%lld, '%s', 'COMPLETE', '2026-01-01T00:00:00Z', '2026-01-01T00:01:00Z',"
                  "         2, 1, 0, 1, 0, '%s', %d, 'DECLARED', 2, 1, 1, 'COMPLETE', 2);",
                  (long long)e->repo_id, e->repo.scanned_head, ATLAS_SEM_ANALYZER_ID,
                  (int)ATLAS_SEM_ANALYZER_VERSION);
    T_OK(atlas_db_exec_sql(e->db, sql, err), err);
    int64_t gen = t8_scalar(e, "SELECT COALESCE(MAX(id), 0) FROM sem_generations;", err);
    (void)snprintf(sql, sizeof sql,
                  "INSERT INTO sem_current(repo_id, generation_id) VALUES(%lld, %lld)"
                  "  ON CONFLICT(repo_id) DO UPDATE SET generation_id = %lld;",
                  (long long)e->repo_id, (long long)gen, (long long)gen);
    T_OK(atlas_db_exec_sql(e->db, sql, err), err);
    return gen;
}

/* (a) acceptance item 2, the `SOURCE_REVISION` half: editing one `REPO_DIR`
 * child bumps the generation by exactly one with cause `SOURCE_REVISION`,
 * writes a diff row only for the touched claim, and the untouched claim's
 * `verify_claims` row is byte-for-byte identical before and after -- checked
 * literally, because this pass never moves `scanned_head`, so §27's content
 * key (which hashes `basis_commit`) does not move for it either. A `REPO_DIR`
 * child, not a `REPO_FILE`: a tracked `REPO_FILE` reads HEAD's blob, so
 * editing it on disk without a commit would be invisible to the pass and
 * this test would be asserting nothing (memory.h's own HEAD-blob contract). */
static void test_source_revision_leaves_untouched_claims_byte_identical(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    const char *repo = fx_repo(&e.fx);
    T_OK(fx_mkdir(repo, "src", &err), &err);
    T_OK(fx_mkdir(repo, "src/db", &err), &err);
    T_OK(fx_write(repo, "src/db/db_orch.c", "int x;\n", &err), &err);
    T_OK(fx_mkdir(repo, "src/gw", &err), &err);
    T_OK(fx_write(repo, "src/gw/gateway.c", "int y;\n", &err), &err);
    T_OK(fx_mkdir(repo, ".claude", &err), &err);
    T_OK(fx_mkdir(repo, ".claude/memories", &err), &err);
    T_OK(fx_write(repo, ".claude/memories/a.md", "the daemon reads `src/db/db_orch.c`", &err), &err);
    T_OK(fx_write(repo, ".claude/memories/b.md", "the gateway reads `src/gw/gateway.c`", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "src/db/db_orch.c", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                &err);
    t8_seed_file(&e, "src/gw/gateway.c", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                &err);
    t8_seed_file(&e, ".claude/memories/a.md",
                "1111111111111111111111111111111111111111111111111111111111111111", &err);
    t8_seed_file(&e, ".claude/memories/b.md",
                "2222222222222222222222222222222222222222222222222222222222222222", &err);

    const char *paths[] = {".claude/memories"};
    atlas_syspolicy pol;
    t8_policy(&pol, ATLAS_MEMORY_SOURCE_REPO_DIR, paths, 1);

    atlas_memory_pass_result r1;
    t8_run_pass(&e, &pol, &r1, &err);
    T_REQUIRE(r1.generation != 0);

    atlas_buf a_uid = ATLAS_BUF_INIT;
    t9_claim_uid_for_text(&e, "the daemon reads `src/db/db_orch.c`", &a_uid, &err);
    atlas_buf a_before = ATLAS_BUF_INIT;
    t9_claim_row_snapshot(&e, atlas_buf_cstr(&a_uid), &a_before, &err);
    /* Every concatenated column is NOT NULL (verify_claims' own CREATE TABLE,
     * plus the content_key/created_by_actor_id ALTER TABLE defaults), so this
     * cannot be a SQLite NULL-propagated empty string; assert it directly
     * rather than trust the schema silently. */
    T_CHECK_MSG(a_before.len > 0 && strstr(atlas_buf_cstr(&a_before), atlas_buf_cstr(&a_uid)) != NULL,
               "the snapshot must be non-empty and carry the claim's own uid, got: %s",
               atlas_buf_cstr(&a_before));

    T_OK(fx_write(repo, ".claude/memories/b.md",
                 "the gateway also reads `src/gw/gateway.c` directly", &err),
         &err);

    atlas_memory_pass_result r2;
    t8_run_pass(&e, &pol, &r2, &err);
    T_CHECK_MSG(r2.generation == r1.generation + 1,
               "expected the generation to bump by exactly one, got %lld after %lld",
               (long long)r2.generation, (long long)r1.generation);
    T_CHECK_MSG(r2.diff_rows == 1, "expected exactly one diff row, got %zu", r2.diff_rows);

    atlas_buf cause = ATLAS_BUF_INIT;
    int64_t gen2 = 0;
    t9_generation_latest(&e, &cause, NULL, &gen2, &err);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&cause), "SOURCE_REVISION") == 0,
               "expected cause SOURCE_REVISION, got %s", atlas_buf_cstr(&cause));

    atlas_buf a_after = ATLAS_BUF_INIT;
    t9_claim_row_snapshot(&e, atlas_buf_cstr(&a_uid), &a_after, &err);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&a_before), atlas_buf_cstr(&a_after)) == 0,
               "the untouched claim's stored row moved:\n  before: %s\n  after:  %s",
               atlas_buf_cstr(&a_before), atlas_buf_cstr(&a_after));

    atlas_buf new_b_uid = ATLAS_BUF_INIT;
    t9_claim_uid_for_text(&e, "the gateway also reads `src/gw/gateway.c` directly", &new_b_uid, &err);
    atlas_buf kind = ATLAS_BUF_INIT;
    bool found = false;
    t9_diff_kind_for(&e, gen2, atlas_buf_cstr(&new_b_uid), &kind, NULL, &found, &err);
    T_CHECK_MSG(found && strcmp(atlas_buf_cstr(&kind), "ADDED") == 0,
               "expected the new bullet's own diff row to be ADDED, found=%d kind=%s", found,
               found ? atlas_buf_cstr(&kind) : "(none)");

    bool a_has_row = false;
    t9_diff_kind_for(&e, gen2, atlas_buf_cstr(&a_uid), NULL, NULL, &a_has_row, &err);
    T_CHECK_MSG(!a_has_row, "the untouched claim should have no diff row this generation");

    atlas_buf_free(&a_uid);
    atlas_buf_free(&a_before);
    atlas_buf_free(&a_after);
    atlas_buf_free(&cause);
    atlas_buf_free(&new_b_uid);
    atlas_buf_free(&kind);
    t8_env_close(&e);
}

/* (b) acceptance item 2, the `COMMIT` half: a commit changing the file one
 * claim's `PATH` anchor points at produces an `IMPACTED` (or `CONTRADICTED`)
 * diff row for that claim, cause `COMMIT`, both `head_commit` and the diff
 * present on the generation -- and an unrelated claim gets no diff row, even
 * though *every* claim re-mints this pass (`basis_commit` moves). "Byte-for-
 * byte stable" is therefore checked at the diff surface here, not at the
 * `verify_claims` row -- `classify_candidate`'s own comment states why that
 * is the honest form of item 2 for this cause, since a fresh row is what a
 * head move produces unconditionally. */
static void test_commit_impacts_only_its_anchored_claim(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    const char *repo = fx_repo(&e.fx);
    T_OK(fx_mkdir(repo, "src", &err), &err);
    T_OK(fx_write(repo, "src/a.c", "int a;\n", &err), &err);
    T_OK(fx_write(repo, "src/b.c", "int b;\n", &err), &err);
    T_OK(fx_write(repo, "note-a.md", "the file `src/a.c` holds a", &err), &err);
    T_OK(fx_write(repo, "note-b.md", "the file `src/b.c` holds b", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "src/a.c", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &err);
    t8_seed_file(&e, "src/b.c", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", &err);
    t8_seed_file(&e, "note-a.md", "1111111111111111111111111111111111111111111111111111111111111111",
                &err);
    t8_seed_file(&e, "note-b.md", "2222222222222222222222222222222222222222222222222222222222222222",
                &err);

    const char *paths[] = {"note-a.md", "note-b.md"};
    atlas_syspolicy pol;
    t8_policy(&pol, ATLAS_MEMORY_SOURCE_REPO_FILE, paths, 2);

    atlas_memory_pass_result r1;
    t8_run_pass(&e, &pol, &r1, &err);
    T_REQUIRE(r1.generation != 0);

    /* Commit a real change to src/a.c and re-index the fact -- the hash a
     * real scan would have written, distinct from the seeded one above. */
    T_OK(fx_write(repo, "src/a.c", "int a = 2;\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "change a", &err), &err);
    t8_bind_head(&e, &err);
    t9_update_file_hash(&e, "src/a.c", "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
                        &err);

    atlas_memory_pass_result r2;
    t8_run_pass(&e, &pol, &r2, &err);
    T_CHECK_MSG(r2.generation == r1.generation + 1,
               "expected exactly one generation bump, got %lld after %lld", (long long)r2.generation,
               (long long)r1.generation);

    atlas_buf cause = ATLAS_BUF_INIT;
    atlas_buf head_commit = ATLAS_BUF_INIT;
    int64_t gen2 = 0;
    t9_generation_latest(&e, &cause, &head_commit, &gen2, &err);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&cause), "COMMIT") == 0, "expected cause COMMIT, got %s",
               atlas_buf_cstr(&cause));
    T_CHECK_MSG(head_commit.len > 0, "expected head_commit to be recorded on the generation");

    /* Every claim's content key hashes basis_commit (S27), so a COMMIT-caused
     * pass re-mints b's row too -- its uid after this pass is NOT the uid
     * captured before it. Looking up the pre-pass uid in gen2's diffs would
     * pass vacuously (that uid was never written to in this generation
     * regardless of whether suppression works); the real test is that b's
     * *current* claim -- the one this pass actually produced -- carries no
     * diff row, because its verifier_input and decision binding did not
     * change. */
    atlas_buf new_a_uid = ATLAS_BUF_INIT;
    t9_claim_uid_for_text(&e, "the file `src/a.c` holds a", &new_a_uid, &err);
    atlas_buf new_b_uid = ATLAS_BUF_INIT;
    t9_claim_uid_for_text(&e, "the file `src/b.c` holds b", &new_b_uid, &err);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&new_a_uid), atlas_buf_cstr(&new_b_uid)) != 0,
               "a and b claim uids must not collide");

    atlas_buf kind = ATLAS_BUF_INIT;
    bool found = false;
    t9_diff_kind_for(&e, gen2, atlas_buf_cstr(&new_a_uid), &kind, NULL, &found, &err);
    T_CHECK_MSG(found &&
                   (strcmp(atlas_buf_cstr(&kind), "IMPACTED") == 0 ||
                    strcmp(atlas_buf_cstr(&kind), "CONTRADICTED") == 0),
               "expected the anchored claim's diff row to be IMPACTED or CONTRADICTED, found=%d "
               "kind=%s",
               found, found ? atlas_buf_cstr(&kind) : "(none)");

    bool b_has_row = false;
    t9_diff_kind_for(&e, gen2, atlas_buf_cstr(&new_b_uid), NULL, NULL, &b_has_row, &err);
    T_CHECK_MSG(!b_has_row, "the unrelated (re-minted) claim should have no diff row this generation");

    T_CHECK_MSG(r2.diff_rows == 1, "expected exactly one diff row this generation, got %zu",
               r2.diff_rows);

    atlas_buf_free(&cause);
    atlas_buf_free(&head_commit);
    atlas_buf_free(&new_a_uid);
    atlas_buf_free(&new_b_uid);
    atlas_buf_free(&kind);
    t8_env_close(&e);
}

/* fix-round-1, C1: two different propositions naming the *same* file --
 * two memory bullets citing one shared source file is the ordinary case for
 * a real `.claude/memories` directory, not a constructed edge case. Before
 * this round's fix, `find_prior_cb` answered "the most recently anchored
 * claim, whichever proposition it belongs to" rather than "this proposition's
 * own predecessor": after any commit that re-mints every claim, both fresh
 * claims would probe the same (PATH, `src/shared.c`) anchor tuple, and
 * whichever one is not the tuple's actual most-recent claim would compare
 * its own text against the *other* bullet's and get a spurious `ADDED` row
 * for a proposition that had not changed at all -- exactly the failure
 * acceptance item 2's "byte-for-byte stable" absence stands on.
 *
 * The commit here touches an unrelated file, never `src/shared.c` itself, so
 * both bullets' own facts stay identical across the remint and the correct
 * outcome is that *neither* gets a diff row -- not merely that they do not
 * collide with each other. */
static void test_two_propositions_sharing_one_anchor_both_remint_cleanly(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    const char *repo = fx_repo(&e.fx);
    T_OK(fx_mkdir(repo, "src", &err), &err);
    T_OK(fx_write(repo, "src/shared.c", "int shared;\n", &err), &err);
    T_OK(fx_mkdir(repo, ".claude", &err), &err);
    T_OK(fx_mkdir(repo, ".claude/memories", &err), &err);
    T_OK(fx_write(repo, ".claude/memories/x.md", "the file `src/shared.c` holds X", &err), &err);
    T_OK(fx_write(repo, ".claude/memories/y.md", "the file `src/shared.c` holds Y", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "src/shared.c", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                &err);
    t8_seed_file(&e, ".claude/memories/x.md",
                "1111111111111111111111111111111111111111111111111111111111111111", &err);
    t8_seed_file(&e, ".claude/memories/y.md",
                "2222222222222222222222222222222222222222222222222222222222222222", &err);

    const char *paths[] = {".claude/memories"};
    atlas_syspolicy pol;
    t8_policy(&pol, ATLAS_MEMORY_SOURCE_REPO_DIR, paths, 1);

    atlas_memory_pass_result r1;
    t8_run_pass(&e, &pol, &r1, &err);
    T_REQUIRE(r1.generation != 0);

    atlas_buf x_uid1 = ATLAS_BUF_INIT;
    t9_claim_uid_for_text(&e, "the file `src/shared.c` holds X", &x_uid1, &err);
    atlas_buf y_uid1 = ATLAS_BUF_INIT;
    t9_claim_uid_for_text(&e, "the file `src/shared.c` holds Y", &y_uid1, &err);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&x_uid1), atlas_buf_cstr(&y_uid1)) != 0,
               "x and y must not have collided into one claim already");

    /* An unrelated commit: HEAD moves, so every claim re-mints (S27's own
     * content key), but src/shared.c's own bytes never change. */
    T_OK(fx_write(repo, "unrelated.c", "int unrelated;\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "unrelated change", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "unrelated.c", "3333333333333333333333333333333333333333333333333333333333333333",
                &err);

    atlas_memory_pass_result r2;
    t8_run_pass(&e, &pol, &r2, &err);
    T_CHECK_MSG(r2.generation == r1.generation + 1, "expected exactly one generation bump, got %lld "
               "after %lld",
               (long long)r2.generation, (long long)r1.generation);

    atlas_buf cause = ATLAS_BUF_INIT;
    int64_t gen2 = 0;
    t9_generation_latest(&e, &cause, NULL, &gen2, &err);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&cause), "COMMIT") == 0, "expected cause COMMIT, got %s",
               atlas_buf_cstr(&cause));

    atlas_buf x_uid2 = ATLAS_BUF_INIT;
    t9_claim_uid_for_text(&e, "the file `src/shared.c` holds X", &x_uid2, &err);
    atlas_buf y_uid2 = ATLAS_BUF_INIT;
    t9_claim_uid_for_text(&e, "the file `src/shared.c` holds Y", &y_uid2, &err);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&x_uid1), atlas_buf_cstr(&x_uid2)) != 0,
               "x's claim did not re-mint on a COMMIT-caused pass");
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&y_uid1), atlas_buf_cstr(&y_uid2)) != 0,
               "y's claim did not re-mint on a COMMIT-caused pass");
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&x_uid2), atlas_buf_cstr(&y_uid2)) != 0,
               "x and y re-minted into the same claim");

    bool x_has_row = false, y_has_row = false;
    t9_diff_kind_for(&e, gen2, atlas_buf_cstr(&x_uid2), NULL, NULL, &x_has_row, &err);
    t9_diff_kind_for(&e, gen2, atlas_buf_cstr(&y_uid2), NULL, NULL, &y_has_row, &err);
    T_CHECK_MSG(!x_has_row,
               "x's re-minted claim got a diff row despite src/shared.c never changing -- the "
               "cross-contamination C1 exists to prevent");
    T_CHECK_MSG(!y_has_row,
               "y's re-minted claim got a diff row despite src/shared.c never changing -- the "
               "cross-contamination C1 exists to prevent");
    T_CHECK_MSG(r2.diff_rows == 0, "expected no diff rows at all this generation, got %zu",
               r2.diff_rows);

    atlas_buf_free(&x_uid1);
    atlas_buf_free(&y_uid1);
    atlas_buf_free(&x_uid2);
    atlas_buf_free(&y_uid2);
    atlas_buf_free(&cause);
    t8_env_close(&e);
}

/* fix-round-2, New Important 2: `SUPPORTED`'s own producer, and `IMPACTED`
 * gated on an actual commit range rather than on "evaluate ran and passed".
 * Round 1 folded `!verifier_input_equal` and `path_touched` into one
 * "something changed" boolean and always answered `IMPACTED` once evaluate
 * ran -- reachable on a plain `SOURCE_REVISION` pass (a `REPO_DIR` child's
 * own content edited on disk, no commit at all) and untested there, which
 * is exactly the undisclosed deviation round 2 caught. This edits a PATH-
 * anchored bullet's own referenced file with no commit in between (matching
 * test (a)'s own convention: a REPO_FILE reads HEAD's blob and would not see
 * an uncommitted edit at all) and asserts the cause is SOURCE_REVISION and
 * the diff kind is SUPPORTED, never IMPACTED. */
static void test_source_revision_reverification_is_supported_not_impacted(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    const char *repo = fx_repo(&e.fx);
    T_OK(fx_mkdir(repo, "src", &err), &err);
    T_OK(fx_write(repo, "src/lib.c", "int lib_v1;\n", &err), &err);
    T_OK(fx_mkdir(repo, ".claude", &err), &err);
    T_OK(fx_mkdir(repo, ".claude/memories", &err), &err);
    T_OK(fx_write(repo, ".claude/memories/a.md", "the file `src/lib.c` is present", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "src/lib.c", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &err);
    t8_seed_file(&e, ".claude/memories/a.md",
                "1111111111111111111111111111111111111111111111111111111111111111", &err);

    const char *paths[] = {".claude/memories"};
    atlas_syspolicy pol;
    t8_policy(&pol, ATLAS_MEMORY_SOURCE_REPO_DIR, paths, 1);

    atlas_memory_pass_result r1;
    t8_run_pass(&e, &pol, &r1, &err);
    T_REQUIRE(r1.generation != 0);

    atlas_buf uid1 = ATLAS_BUF_INIT;
    t9_claim_uid_for_text(&e, "the file `src/lib.c` is present", &uid1, &err);

    /* No commit: src/lib.c's own content changes on disk (a REPO_DIR child is
     * read from the working tree, memory.h's own contract), re-indexed as a
     * real scan would. The bullet's own text is unchanged. */
    T_OK(fx_write(repo, "src/lib.c", "int lib_v2;\n", &err), &err);
    t9_update_file_hash(&e, "src/lib.c", "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
                        &err);

    atlas_memory_pass_result r2;
    t8_run_pass(&e, &pol, &r2, &err);
    T_CHECK_MSG(r2.generation == r1.generation + 1, "expected exactly one generation bump, got %lld "
               "after %lld",
               (long long)r2.generation, (long long)r1.generation);

    atlas_buf cause = ATLAS_BUF_INIT;
    int64_t gen2 = 0;
    t9_generation_latest(&e, &cause, NULL, &gen2, &err);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&cause), "SOURCE_REVISION") == 0,
               "expected cause SOURCE_REVISION (no commit happened), got %s", atlas_buf_cstr(&cause));

    atlas_buf uid2 = ATLAS_BUF_INIT;
    t9_claim_uid_for_text(&e, "the file `src/lib.c` is present", &uid2, &err);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&uid1), atlas_buf_cstr(&uid2)) != 0,
               "expected the claim to re-mint: its own verifier_input (the file's content hash) "
               "changed");

    atlas_buf kind = ATLAS_BUF_INIT;
    bool found = false;
    t9_diff_kind_for(&e, gen2, atlas_buf_cstr(&uid2), &kind, NULL, &found, &err);
    T_CHECK_MSG(found && strcmp(atlas_buf_cstr(&kind), "SUPPORTED") == 0,
               "expected SUPPORTED (re-verified true, no commit range involved), found=%d kind=%s -- "
               "IMPACTED here would mean a SOURCE_REVISION pass is reporting as if a commit touched "
               "it",
               found, found ? atlas_buf_cstr(&kind) : "(none)");

    atlas_buf_free(&uid1);
    atlas_buf_free(&uid2);
    atlas_buf_free(&cause);
    atlas_buf_free(&kind);
    t8_env_close(&e);
}

/* T9 fix-round-3 (Minor, filed against round 2's own comment at what is now
 * `reconcile.c:1004-1032`): `IMPACTED`'s own definition is positional --
 * "the anchor intersects the commit range's changed paths" -- not "this pass
 * happened to be `COMMIT`-caused". The two readings agree whenever the
 * proposition's own PATH anchor is what a commit touched (the test just
 * above this one, and `test_commit_impacts_only_its_anchored_claim`, both
 * drive that case) and disagree in exactly one situation: a genuine
 * `COMMIT`-caused pass (HEAD really did move) whose commit range does *not*
 * happen to include this proposition's own anchor, while the anchor's
 * verifier input still moved for an unrelated, uncommitted reason. The old,
 * pre-round-1 `head_moved ? IMPACTED : SUPPORTED` would answer IMPACTED
 * here; the current, positional `commit_touched ? IMPACTED : SUPPORTED`
 * answers SUPPORTED -- and that half is what no test pinned before this one.
 *
 * Both readings are driven back to back against the *same* claim (`the file
 * `src/lib.c` is present`, a plain PATH/CONTENT_HASH claim, so `check ==
 * PASS` is unconditional the way New-2's own SOURCE_REVISION test above
 * established -- no CONTRADICTED/PASS ambiguity to hedge, unlike a claim
 * that asserts specific content): first a `COMMIT` pass whose commit range
 * is an unrelated file while `src/lib.c` changes only on disk (SUPPORTED),
 * then a second `COMMIT` pass whose commit range *is* `src/lib.c` itself
 * (IMPACTED) -- proving the ternary is keyed on this proposition's own
 * anchor membership, not merely on which cause produced the pass. */
static void test_impacted_is_positional_not_pass_wide(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    const char *repo = fx_repo(&e.fx);
    T_OK(fx_mkdir(repo, "src", &err), &err);
    T_OK(fx_write(repo, "src/lib.c", "int lib_v1;\n", &err), &err);
    T_OK(fx_write(repo, "note.md", "the file `src/lib.c` is present", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "src/lib.c", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &err);
    t8_seed_file(&e, "note.md", "1111111111111111111111111111111111111111111111111111111111111111",
                &err);

    const char *paths[] = {"note.md"};
    atlas_syspolicy pol;
    t8_policy(&pol, ATLAS_MEMORY_SOURCE_REPO_FILE, paths, 1);

    atlas_memory_pass_result r1;
    t8_run_pass(&e, &pol, &r1, &err);
    T_REQUIRE(r1.generation != 0);

    /* Phase A: a real commit moves HEAD (cause COMMIT), but it commits an
     * *unrelated* file. `fx_add_all` stages every working-tree change, so the
     * commit naming only unrelated.c has to be made -- and HEAD bound to it
     * -- *before* src/lib.c is ever touched; only then is src/lib.c's own
     * content changed, on disk only, never committed, so the claim's
     * verifier input still moves (CONTENT_HASH reads the file fresh every
     * pass), but the commit range this pass observes is {unrelated.c}, not
     * {src/lib.c}. */
    T_OK(fx_write(repo, "unrelated.c", "int u;\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "unrelated", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "unrelated.c", "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
                &err);

    T_OK(fx_write(repo, "src/lib.c", "int lib_v2;\n", &err), &err);
    t9_update_file_hash(&e, "src/lib.c", "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
                        &err);

    atlas_memory_pass_result r2;
    t8_run_pass(&e, &pol, &r2, &err);
    T_CHECK_MSG(r2.generation == r1.generation + 1,
               "expected exactly one generation bump, got %lld after %lld", (long long)r2.generation,
               (long long)r1.generation);

    atlas_buf cause2 = ATLAS_BUF_INIT;
    int64_t gen2 = 0;
    t9_generation_latest(&e, &cause2, NULL, &gen2, &err);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&cause2), "COMMIT") == 0,
               "expected cause COMMIT (HEAD moved on an unrelated commit), got %s",
               atlas_buf_cstr(&cause2));

    atlas_buf uid2 = ATLAS_BUF_INIT;
    t9_claim_uid_for_text(&e, "the file `src/lib.c` is present", &uid2, &err);
    atlas_buf kind2 = ATLAS_BUF_INIT;
    bool found2 = false;
    t9_diff_kind_for(&e, gen2, atlas_buf_cstr(&uid2), &kind2, NULL, &found2, &err);
    T_CHECK_MSG(found2 && strcmp(atlas_buf_cstr(&kind2), "SUPPORTED") == 0,
               "phase A: a COMMIT pass whose commit range does not include this claim's own PATH "
               "anchor must report SUPPORTED, not IMPACTED -- found=%d kind=%s (the pre-round-1 "
               "head_moved-based rule would have answered IMPACTED here)",
               found2, found2 ? atlas_buf_cstr(&kind2) : "(none)");

    /* Phase B, the contrast: this time the commit *is* the change to
     * src/lib.c, so the same claim's own PATH anchor is exactly what the
     * commit range touched. */
    T_OK(fx_write(repo, "src/lib.c", "int lib_v3;\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "touch lib.c", &err), &err);
    t8_bind_head(&e, &err);
    t9_update_file_hash(&e, "src/lib.c", "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
                        &err);

    atlas_memory_pass_result r3;
    t8_run_pass(&e, &pol, &r3, &err);
    T_CHECK_MSG(r3.generation == r2.generation + 1,
               "expected exactly one generation bump, got %lld after %lld", (long long)r3.generation,
               (long long)r2.generation);

    atlas_buf cause3 = ATLAS_BUF_INIT;
    int64_t gen3 = 0;
    t9_generation_latest(&e, &cause3, NULL, &gen3, &err);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&cause3), "COMMIT") == 0,
               "expected cause COMMIT (HEAD moved touching src/lib.c itself), got %s",
               atlas_buf_cstr(&cause3));

    atlas_buf uid3 = ATLAS_BUF_INIT;
    t9_claim_uid_for_text(&e, "the file `src/lib.c` is present", &uid3, &err);
    atlas_buf kind3 = ATLAS_BUF_INIT;
    bool found3 = false;
    t9_diff_kind_for(&e, gen3, atlas_buf_cstr(&uid3), &kind3, NULL, &found3, &err);
    T_CHECK_MSG(found3 && strcmp(atlas_buf_cstr(&kind3), "IMPACTED") == 0,
               "phase B: a COMMIT pass whose commit range does include this claim's own PATH anchor "
               "must report IMPACTED, found=%d kind=%s",
               found3, found3 ? atlas_buf_cstr(&kind3) : "(none)");

    atlas_buf_free(&cause2);
    atlas_buf_free(&uid2);
    atlas_buf_free(&kind2);
    atlas_buf_free(&cause3);
    atlas_buf_free(&uid3);
    atlas_buf_free(&kind3);
    t8_env_close(&e);
}

/* fix-round-1, C2: a bullet naming both a SYMBOL and a PATH gets a `symbol=`
 * verifier input (Decision 4's own precedence, `src/memory/extract.c`),
 * which does not move when the file changes and the symbol does not. Before
 * this round's fix, `verifier_input_equal` was the *only* signal
 * `classify_candidate` had for "did anything about this proposition's
 * referent change", so a commit that rewrote `src/hash.c` while
 * `compute_hash` kept resolving produced no diff row at all for a claim the
 * commit had just invalidated -- item 2's other half: only the impact set is
 * invalidated, but the impact set has to actually be the impact set. This
 * drives the touched-paths set (`atlas_git_log_since` in observe) to prove
 * the file's own change is now visible even though the verifier input never
 * moved. */
static void test_commit_rewrites_file_behind_a_symbol_verifier(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    const char *repo = fx_repo(&e.fx);
    T_OK(fx_mkdir(repo, "src", &err), &err);
    T_OK(fx_write(repo, "src/hash.c", "int compute_hash(void) { return 1; }\n", &err), &err);
    T_OK(fx_write(repo, "note.md", "the function `compute_hash` in `src/hash.c` is correct", &err),
         &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "src/hash.c", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                &err);
    t8_seed_file(&e, "note.md", "1111111111111111111111111111111111111111111111111111111111111111",
                &err);
    (void)t9_seed_sem_generation(&e, true, "c:@F@compute_hash", "compute_hash", &err);

    const char *paths[] = {"note.md"};
    atlas_syspolicy pol;
    t8_policy(&pol, ATLAS_MEMORY_SOURCE_REPO_FILE, paths, 1);

    atlas_memory_pass_result r1;
    t8_run_pass(&e, &pol, &r1, &err);
    T_REQUIRE(r1.generation != 0);

    atlas_buf uid1 = ATLAS_BUF_INIT;
    t9_claim_uid_for_text(&e, "the function `compute_hash` in `src/hash.c` is correct", &uid1, &err);

    /* Rewrites src/hash.c's own bytes; compute_hash keeps resolving (the
     * semantic generation seeded above is not touched), so the SYMBOL-
     * derived verifier input this claim actually carries does not move. */
    T_OK(fx_write(repo, "src/hash.c", "int compute_hash(void) { return 2; }\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "rewrite hash.c", &err), &err);
    t8_bind_head(&e, &err);
    t9_update_file_hash(&e, "src/hash.c", "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
                        &err);

    atlas_memory_pass_result r2;
    t8_run_pass(&e, &pol, &r2, &err);
    T_CHECK_MSG(r2.generation == r1.generation + 1, "expected exactly one generation bump, got %lld "
               "after %lld",
               (long long)r2.generation, (long long)r1.generation);

    atlas_buf cause = ATLAS_BUF_INIT;
    int64_t gen2 = 0;
    t9_generation_latest(&e, &cause, NULL, &gen2, &err);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&cause), "COMMIT") == 0, "expected cause COMMIT, got %s",
               atlas_buf_cstr(&cause));

    atlas_buf uid2 = ATLAS_BUF_INIT;
    t9_claim_uid_for_text(&e, "the function `compute_hash` in `src/hash.c` is correct", &uid2, &err);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&uid1), atlas_buf_cstr(&uid2)) != 0,
               "the claim did not re-mint on a COMMIT-caused pass");

    atlas_buf kind = ATLAS_BUF_INIT;
    bool found = false;
    t9_diff_kind_for(&e, gen2, atlas_buf_cstr(&uid2), &kind, NULL, &found, &err);
    T_CHECK_MSG(found,
               "expected a diff row for the claim whose PATH anchor's file the commit rewrote, even "
               "though its SYMBOL-derived verifier input did not move -- C2's own failure mode "
               "before this round's fix");
    if (found) {
        T_CHECK_MSG(strcmp(atlas_buf_cstr(&kind), "IMPACTED") == 0,
                   "expected IMPACTED (the symbol still resolves), got %s", atlas_buf_cstr(&kind));
    }

    atlas_buf_free(&uid1);
    atlas_buf_free(&uid2);
    atlas_buf_free(&cause);
    atlas_buf_free(&kind);
    t8_env_close(&e);
}

/* fix-round-2, New Important 3: `atlas_memory_touched.bound_hit` had no
 * field on `atlas_memory_pass_result` and no log, so a comment claiming it
 * is "reported, never silent" was not true of anything outside
 * `reconcile.c` itself -- A8-CI's own rule, restated six times this season
 * according to the review, is that every bound that is reached is reported.
 * Drives the cheapest reliable trigger for `bound_hit` rather than the
 * expensive one (a real commit range over
 * `ATLAS_MEMORY_MAX_TOUCHED_PATHS` distinct paths): a stored `head_commit`
 * rewritten to a well-formed but nonexistent object id, so
 * `atlas_git_tip_is_stale` reports `unknown` -- the same "the recorded tip
 * cannot be trusted" case `src/core/reconcile.c` itself falls back to a full
 * walk over. */
static void test_touched_bound_hit_is_reported_on_the_pass_result(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    const char *repo = fx_repo(&e.fx);
    T_OK(fx_write(repo, "note.md", "a plain note with no anchors", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "note.md", "1111111111111111111111111111111111111111111111111111111111111111",
                &err);

    const char *paths[] = {"note.md"};
    atlas_syspolicy pol;
    t8_policy(&pol, ATLAS_MEMORY_SOURCE_REPO_FILE, paths, 1);

    atlas_memory_pass_result r1;
    t8_run_pass(&e, &pol, &r1, &err);
    T_REQUIRE(r1.generation != 0);
    T_CHECK_MSG(!r1.touched_bound_hit, "expected no bound hit on the very first generation");

    char sql[256];
    (void)snprintf(sql, sizeof sql,
                  "UPDATE memory_generations SET head_commit ="
                  " 'ffffffffffffffffffffffffffffffffffffff' WHERE repo_id = %lld;",
                  (long long)e.repo_id);
    T_OK(atlas_db_exec_sql(e.db, sql, &err), &err);

    T_OK(fx_write(repo, "unrelated.c", "int x;\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "unrelated", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "unrelated.c", "2222222222222222222222222222222222222222222222222222222222222222",
                &err);

    atlas_memory_pass_result r2;
    t8_run_pass(&e, &pol, &r2, &err);
    T_CHECK_MSG(r2.touched_bound_hit,
               "expected a reported bound hit: the recorded tip does not exist, so the pass could "
               "not prove which paths a commit range did not touch");

    t8_env_close(&e);
}

/* fix-round-2, I7, measured: a SYMBOL+PATH bullet re-minting on every
 * `COMMIT`-caused pass without either anchor ever vanishing used to leave
 * one orphaned `memory_claim_anchors` row per pass -- round 1 pruned only
 * `anchors[0]` (whichever tuple happened to be the lookup key), so the
 * *other* anchor's row from every prior claim uid stayed forever. Five
 * further unrelated commits, each re-minting the claim, and the row count
 * for its own SYMBOL and PATH tuples must stay at exactly one each --
 * `memory_claim_anchors` is never growing merely because time (and commits)
 * are passing. */
static void test_multi_anchor_reminting_does_not_grow_memory_claim_anchors(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    const char *repo = fx_repo(&e.fx);
    T_OK(fx_mkdir(repo, "src", &err), &err);
    T_OK(fx_write(repo, "src/hash.c", "int compute_hash(void) { return 1; }\n", &err), &err);
    T_OK(fx_write(repo, "note.md", "the function `compute_hash` in `src/hash.c` is correct", &err),
         &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "src/hash.c", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &err);
    t8_seed_file(&e, "note.md", "1111111111111111111111111111111111111111111111111111111111111111",
                &err);
    (void)t9_seed_sem_generation(&e, true, "c:@F@compute_hash", "compute_hash", &err);

    const char *paths[] = {"note.md"};
    atlas_syspolicy pol;
    t8_policy(&pol, ATLAS_MEMORY_SOURCE_REPO_FILE, paths, 1);

    atlas_memory_pass_result r1;
    t8_run_pass(&e, &pol, &r1, &err);
    T_REQUIRE(r1.generation != 0);

    char count_sql[512];
    (void)snprintf(count_sql, sizeof count_sql,
                  "SELECT COUNT(*) FROM memory_claim_anchors"
                  " WHERE repo_id = %lld AND kind = 'SYMBOL' AND value = 'compute_hash';",
                  (long long)e.repo_id);
    T_CHECK_MSG(t8_scalar(&e, count_sql, &err) == 1, "expected exactly one SYMBOL anchor row "
               "after the first pass");
    (void)snprintf(count_sql, sizeof count_sql,
                  "SELECT COUNT(*) FROM memory_claim_anchors"
                  " WHERE repo_id = %lld AND kind = 'PATH' AND value = 'src/hash.c';",
                  (long long)e.repo_id);
    T_CHECK_MSG(t8_scalar(&e, count_sql, &err) == 1, "expected exactly one PATH anchor row "
               "after the first pass");

    for (int i = 0; i < 5; i++) {
        char fname[32];
        (void)snprintf(fname, sizeof fname, "unrelated%d.c", i);
        char body[64];
        (void)snprintf(body, sizeof body, "int u%d;\n", i);
        T_OK(fx_write(repo, fname, body, &err), &err);
        T_OK(fx_add_all(&e.fx, repo, &err), &err);
        T_OK(fx_commit(&e.fx, repo, "unrelated", &err), &err);
        t8_bind_head(&e, &err);
        char h[ATLAS_SHA256_HEX_LEN + 1u];
        atlas_sha256_hex(body, strlen(body), h);
        t8_seed_file(&e, fname, h, &err);

        atlas_memory_pass_result rn;
        t8_run_pass(&e, &pol, &rn, &err);
        (void)rn;
    }

    (void)snprintf(count_sql, sizeof count_sql,
                  "SELECT COUNT(*) FROM memory_claim_anchors"
                  " WHERE repo_id = %lld AND kind = 'SYMBOL' AND value = 'compute_hash';",
                  (long long)e.repo_id);
    int64_t symbol_rows = t8_scalar(&e, count_sql, &err);
    (void)snprintf(count_sql, sizeof count_sql,
                  "SELECT COUNT(*) FROM memory_claim_anchors"
                  " WHERE repo_id = %lld AND kind = 'PATH' AND value = 'src/hash.c';",
                  (long long)e.repo_id);
    int64_t path_rows = t8_scalar(&e, count_sql, &err);

    atlas_test_note("I7 bound observed: 5 further unrelated-commit remints of one SYMBOL+PATH "
                    "bullet left %lld SYMBOL row(s) and %lld PATH row(s) for its own anchors -- "
                    "the fix-round-2 bound is exactly one each, regardless of how many further "
                    "re-mints happen.",
                    (long long)symbol_rows, (long long)path_rows);
    T_CHECK_MSG(symbol_rows == 1, "expected the SYMBOL anchor row count to stay at 1, got %lld",
               (long long)symbol_rows);
    T_CHECK_MSG(path_rows == 1, "expected the PATH anchor row count to stay at 1, got %lld",
               (long long)path_rows);

    t8_env_close(&e);
}

/* (c) approving a decision revision, with nothing else changed, is what the
 * next pass's cause reports. */
static void test_decision_revision_approval_is_the_cause(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    atlas_buf doc_uid = ATLAS_BUF_INIT;
    t9_propose(&e, "a normative rule", &doc_uid, &err);

    const char *repo = fx_repo(&e.fx);
    char bullet[256];
    (void)snprintf(bullet, sizeof bullet, "see decision %s for the rule", atlas_buf_cstr(&doc_uid));
    T_OK(fx_write(repo, "note.md", bullet, &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "note.md", "1111111111111111111111111111111111111111111111111111111111111111",
                &err);

    const char *paths[] = {"note.md"};
    atlas_syspolicy pol;
    t8_policy(&pol, ATLAS_MEMORY_SOURCE_REPO_FILE, paths, 1);

    atlas_memory_pass_result r1;
    t8_run_pass(&e, &pol, &r1, &err);
    T_REQUIRE(r1.generation != 0);

    t9_approve(&e, atlas_buf_cstr(&doc_uid), 1, &err);

    atlas_memory_pass_result r2;
    t8_run_pass(&e, &pol, &r2, &err);
    T_CHECK_MSG(r2.generation == r1.generation + 1, "expected a generation after the approval");

    atlas_buf cause = ATLAS_BUF_INIT;
    t9_generation_latest(&e, &cause, NULL, NULL, &err);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&cause), "DECISION_REVISION") == 0,
               "expected cause DECISION_REVISION, got %s", atlas_buf_cstr(&cause));

    atlas_buf_free(&doc_uid);
    atlas_buf_free(&cause);
    t8_env_close(&e);
}

/* (d) acceptance item 3, drift, end to end: a decision-scoped memory bullet
 * asserting a symbol that the fixture's semantic index later loses becomes
 * an `IMPLEMENTATION` conflict, and the decision's own status and effective
 * approved revision are unchanged -- asserted explicitly, because collapsing
 * drift into `CONTRADICTION` would let a broken implementation retract the
 * design it violates (`verify.h:474-478`). No commit happens between the two
 * passes, so `basis_commit`/`evaluated_commit` stay equal and `SOURCE_DRIFT`
 * cannot demote the conflict to `NONE` -- the scenario this test must not
 * accidentally run in.
 *
 * fix-round-1: `evaluate_claim` (`reconcile.c`) reaches
 * `atlas_verify_intake_apply_in_tx`'s `EVALUATE` handler, which loads
 * `ATLAS_VERIFYPOLICY_PATH` itself (`src/verify/policy.c`, a compiled-in
 * constant with no override -- A7.1's own rule, `docs/engineering-rules.md`)
 * and can spend an `AUTO_APPROVE`/`AUTO_RESOLVE`. The assertion below proves
 * this test runs under whatever real policy this machine has installed
 * there, not merely in the one configuration ("no policy") where nothing
 * could have moved regardless -- checked, not assumed: this repository's own
 * `/etc/atlas/verification.conf` is real, root-owned, `enabled = yes`, and
 * genuinely loads as `ATLAS_VERIFYPOLICY_ENABLED`.
 *
 * That policy's own `allow` list is `OBLIGATION APPROVED RESOLVED
 * atlas.symbol_absent` -- kind `OBLIGATION` only. `t9_propose` never sets
 * `op.revision.kind`, so this document is kind `DECISION` (A9.1's own zero),
 * outside that policy's reach by construction. The refusal this test proves
 * is therefore genuine under a real active policy, but a *narrower* one than
 * the theoretical worst case: a hostile policy naming `DECISION APPROVED
 * RESOLVED <this claim's own verifier>` explicitly would need injecting
 * through `atlas_verify_intake_apply_in_tx` itself, which loads the fixed
 * path unconditionally and takes no policy parameter a caller could
 * substitute (`src/verify/intake.c:1359`, outside this task's four pinned
 * files). Recorded here as a finding rather than closed silently: this test
 * establishes the refusal under a real, enabled, root-owned policy of
 * whatever shape is actually deployed, not under every conceivable one. */
static void test_drift_conflict_leaves_the_decision_untouched(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    /* fix-round-2 (I4's "also fold in"): required, not observed. A test that
     * only reports what it found proves nothing on a different machine --
     * this project's own deployment installs this exact root-owned policy,
     * and the assertion below is what makes that a checked contract of this
     * test rather than an assumption a reader has to trust the comment
     * about. Both the state *and* the outcome are asserted: `state ==
     * ENABLED` and `policy_id` is the exact deployed policy, then
     * `atlas_verifypolicy_find` -- the real function `atlas_verify_
     * intake_apply_in_tx` consults, not a re-reading of the `allow =` line
     * as text -- confirms the policy covers `OBLIGATION` and does not cover
     * `DECISION`, which is the actual reason this test's document stays out
     * of its reach. */
    atlas_verifypolicy real_policy;
    atlas_verifypolicy_load(&real_policy);
    T_REQUIRE_MSG(real_policy.state == ATLAS_VERIFYPOLICY_ENABLED,
                 "this test requires the real, root-owned verification policy this project "
                 "deploys; found state=%d reason=%s instead",
                 (int)real_policy.state, atlas_verifypolicy_reason_name(real_policy.reason));
    T_REQUIRE_MSG(strcmp(real_policy.policy_id, "atlas-a92-obligation-remediation-v1") == 0,
                 "expected the deployed obligation-remediation policy, got policy_id=%s",
                 real_policy.policy_id);
    T_CHECK_MSG(atlas_verifypolicy_find(&real_policy, ATLAS_DECISION_KIND_OBLIGATION,
                                        ATLAS_DECISION_APPROVED, ATLAS_DECISION_RESOLVED) != NULL,
               "expected the deployed policy to cover OBLIGATION APPROVED->RESOLVED");
    T_CHECK_MSG(atlas_verifypolicy_find(&real_policy, ATLAS_DECISION_KIND_DECISION,
                                        ATLAS_DECISION_APPROVED, ATLAS_DECISION_RESOLVED) == NULL,
               "expected the deployed policy to NOT cover DECISION APPROVED->RESOLVED -- this is "
               "the actual reason this test's document stays outside its reach");

    atlas_buf doc_uid = ATLAS_BUF_INIT;
    t9_propose(&e, "compute_hash must exist", &doc_uid, &err);
    t9_approve(&e, atlas_buf_cstr(&doc_uid), 1, &err);

    const char *repo = fx_repo(&e.fx);
    char bullet[256];
    (void)snprintf(bullet, sizeof bullet, "the function `compute_hash` implements decision %s",
                  atlas_buf_cstr(&doc_uid));
    T_OK(fx_write(repo, "note.md", bullet, &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "note.md", "1111111111111111111111111111111111111111111111111111111111111111",
                &err);
    (void)t9_seed_sem_generation(&e, true, "c:@F@compute_hash", "compute_hash", &err);

    const char *paths[] = {"note.md"};
    atlas_syspolicy pol;
    t8_policy(&pol, ATLAS_MEMORY_SOURCE_REPO_FILE, paths, 1);

    atlas_memory_pass_result r1;
    t8_run_pass(&e, &pol, &r1, &err);
    T_REQUIRE(r1.generation != 0);

    int64_t doc_id = 0, doc_repo = 0;
    bool found = false;
    T_OK(atlas_db_decision_find_uid(e.db, atlas_buf_cstr(&doc_uid), &doc_id, &doc_repo, &found, &err),
         &err);
    T_REQUIRE(found);
    char status_before[24];
    T_OK(atlas_db_decision_document_status(e.db, doc_id, status_before, sizeof status_before, &err),
         &err);
    int64_t rev_before = 0;
    T_OK(atlas_db_decision_approved_revision(e.db, doc_id, &rev_before, &err), &err);

    /* The symbol vanishes: a new semantic generation without it, published
     * as current -- what a real reindex after removing it from the fixture
     * source would leave. Nothing about the repository's own HEAD moves. */
    (void)t9_seed_sem_generation(&e, false, NULL, NULL, &err);

    atlas_memory_pass_result r2;
    t8_run_pass(&e, &pol, &r2, &err);
    T_REQUIRE(r2.generation != 0);

    /* T9 fix-round-1: this pass's own diff comes entirely from the vanished-
     * anchor sweep -- no source's content changed, the decision's approved
     * revision did not move, and HEAD did not move -- so none of Decision
     * 7's three tracked signals differ from the last generation.
     * `determine_cause`'s own comment names this exact case and the schema's
     * three-value CHECK constraint (migration 29) has no fourth label for
     * "the semantic index moved under an approved decision"; asserted here
     * rather than left silent, as a documented imprecision rather than a
     * papered-over one. */
    atlas_buf cause2 = ATLAS_BUF_INIT;
    t9_generation_latest(&e, &cause2, NULL, NULL, &err);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&cause2), "SOURCE_REVISION") == 0,
               "a drift-only generation's cause fell through to something other than the "
               "documented SOURCE_REVISION fallback: %s",
               atlas_buf_cstr(&cause2));
    atlas_buf_free(&cause2);

    char sql[512];
    (void)snprintf(sql, sizeof sql,
                  "SELECT COUNT(*) FROM memory_claim_diffs d"
                  "  JOIN memory_generations g ON g.id = d.generation_id"
                  " WHERE g.repo_id = %lld AND d.kind = 'CONTRADICTED' AND d.reason LIKE 'DRIFT %%';",
                  (long long)e.repo_id);
    T_CHECK_MSG(t8_scalar(&e, sql, &err) >= 1,
               "expected a CONTRADICTED diff row carrying a DRIFT reason -- the IMPLEMENTATION "
               "conflict this test exists to prove");

    char status_after[24];
    T_OK(atlas_db_decision_document_status(e.db, doc_id, status_after, sizeof status_after, &err),
         &err);
    int64_t rev_after = 0;
    T_OK(atlas_db_decision_approved_revision(e.db, doc_id, &rev_after, &err), &err);
    T_CHECK_MSG(strcmp(status_before, status_after) == 0,
               "the decision's status moved under an implementation conflict: %s -> %s",
               status_before, status_after);
    T_CHECK_MSG(rev_before == rev_after,
               "the decision's effective approved revision moved under an implementation "
               "conflict: %lld -> %lld",
               (long long)rev_before, (long long)rev_after);

    atlas_buf_free(&doc_uid);
    t8_env_close(&e);
}

/* fix-round-1: measures the bound placed on the vanished-anchor sweep's own
 * compounding cost. Before this round, `evaluate_claim` (one `EVIDENCE_
 * PRODUCE` plus one `EVALUATE`, and `EVALUATE` always writes a fresh
 * `verify_results` row -- `atlas_verify_intake_apply_in_tx`'s own contract)
 * ran again on *every* pass for as long as a referent stayed vanished, and
 * `RETENTION[]` has no entry that can prune `verify_results` -- a table that
 * would otherwise have grown by one row per pass, for as long as a decision
 * stayed approved and its symbol stayed deleted, for ever. This repeats the
 * drift scenario from `test_drift_conflict_leaves_the_decision_untouched`
 * and runs five further passes with nothing else changing, then asserts the
 * row count for this exact claim stayed at one -- the single evaluation the
 * first vanish-detecting pass performed -- rather than growing to six. */
static void test_vanish_sweep_evaluate_cost_is_bounded_across_repeated_passes(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    atlas_buf doc_uid = ATLAS_BUF_INIT;
    t9_propose(&e, "compute_hash must exist", &doc_uid, &err);
    t9_approve(&e, atlas_buf_cstr(&doc_uid), 1, &err);

    const char *repo = fx_repo(&e.fx);
    char bullet[256];
    (void)snprintf(bullet, sizeof bullet, "the function `compute_hash` implements decision %s",
                  atlas_buf_cstr(&doc_uid));
    T_OK(fx_write(repo, "note.md", bullet, &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "note.md", "1111111111111111111111111111111111111111111111111111111111111111",
                &err);
    (void)t9_seed_sem_generation(&e, true, "c:@F@compute_hash", "compute_hash", &err);

    const char *paths[] = {"note.md"};
    atlas_syspolicy pol;
    t8_policy(&pol, ATLAS_MEMORY_SOURCE_REPO_FILE, paths, 1);

    atlas_memory_pass_result r1;
    t8_run_pass(&e, &pol, &r1, &err);
    T_REQUIRE(r1.generation != 0);

    atlas_buf claim_uid = ATLAS_BUF_INIT;
    t9_claim_uid_for_text(&e, bullet, &claim_uid, &err);

    (void)t9_seed_sem_generation(&e, false, NULL, NULL, &err);

    char count_sql[512];
    (void)snprintf(count_sql, sizeof count_sql,
                  "SELECT COUNT(*) FROM verify_results r"
                  "  JOIN verify_claims c ON c.id = r.claim_id WHERE c.uid = '%s';",
                  atlas_buf_cstr(&claim_uid));

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 6; i++) {
        atlas_memory_pass_result rn;
        t8_run_pass(&e, &pol, &rn, &err);
        (void)rn;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;

    int64_t results_count = t8_scalar(&e, count_sql, &err);
    atlas_test_note("vanish-sweep bound observed: 6 further passes over one permanently-vanished, "
                    "decision-bound, verifier-carrying claim cost %.1f ms total and left %lld "
                    "verify_results row(s) for it -- the fix-round-1 bound is exactly one, "
                    "regardless of how many further passes see the same vanished referent.",
                    ms, (long long)results_count);
    T_CHECK_MSG(results_count == 1,
               "expected exactly one verify_results row for a claim whose vanished referent never "
               "changes across repeated passes, got %lld -- the compounding cost this round's fix "
               "exists to bound",
               (long long)results_count);

    atlas_buf_free(&claim_uid);
    atlas_buf_free(&doc_uid);
    t8_env_close(&e);
}

/* fix-round-2, New Important 1: a verdict reached because Atlas could not
 * look must not freeze. An incomplete semantic generation reports zero
 * symbols exactly like a genuinely deleted one, so the vanish sweep's
 * `still_valid` check cannot tell them apart on its own -- `evaluate_claim`
 * is what tells them apart, by settling `UNAVAILABLE` rather than `FAIL`
 * when coverage is insufficient (A9.2.2). Round 1 put `UNDETERMINED` in the
 * sweep's own skip set alongside the genuinely stable kinds, so once a claim
 * settled `UNDETERMINED` it was never re-evaluated again -- even after the
 * index caught up and the honest verdict became `CONTRADICTED`. This drives
 * exactly that sequence: incomplete coverage settles `UNDETERMINED`; the
 * *same* incomplete generation on a second pass produces no new row (the
 * verdict has not changed, and the ordinary last-kind dedup still applies);
 * coverage then completes over a genuinely absent symbol, and the claim
 * reopens to `CONTRADICTED`. */
static void test_undetermined_reopens_once_coverage_completes(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    atlas_buf doc_uid = ATLAS_BUF_INIT;
    t9_propose(&e, "compute_hash must exist", &doc_uid, &err);
    t9_approve(&e, atlas_buf_cstr(&doc_uid), 1, &err);

    const char *repo = fx_repo(&e.fx);
    char bullet[256];
    (void)snprintf(bullet, sizeof bullet, "the function `compute_hash` implements decision %s",
                  atlas_buf_cstr(&doc_uid));
    T_OK(fx_write(repo, "note.md", bullet, &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "note.md", "1111111111111111111111111111111111111111111111111111111111111111",
                &err);
    (void)t9_seed_sem_generation(&e, true, "c:@F@compute_hash", "compute_hash", &err);

    const char *paths[] = {"note.md"};
    atlas_syspolicy pol;
    t8_policy(&pol, ATLAS_MEMORY_SOURCE_REPO_FILE, paths, 1);

    atlas_memory_pass_result r1;
    t8_run_pass(&e, &pol, &r1, &err);
    T_REQUIRE(r1.generation != 0);

    atlas_buf claim_uid = ATLAS_BUF_INIT;
    t9_claim_uid_for_text(&e, bullet, &claim_uid, &err);

    /* The index starts rebuilding and has not finished: zero symbols, same
     * as a real deletion, but coverage says Atlas cannot conclude that yet. */
    (void)t9_seed_incomplete_sem_generation(&e, &err);

    atlas_memory_pass_result r2;
    t8_run_pass(&e, &pol, &r2, &err);
    T_CHECK_MSG(r2.generation == r1.generation + 1, "expected a generation for the first UNDETERMINED "
               "verdict");

    atlas_buf kind1 = ATLAS_BUF_INIT;
    bool found1 = false;
    t9_diff_kind_for(&e, r2.generation, atlas_buf_cstr(&claim_uid), &kind1, NULL, &found1, &err);
    T_CHECK_MSG(found1 && strcmp(atlas_buf_cstr(&kind1), "UNDETERMINED") == 0,
               "expected UNDETERMINED while coverage is incomplete, found=%d kind=%s", found1,
               found1 ? atlas_buf_cstr(&kind1) : "(none)");

    /* Same incomplete generation, nothing else changes: re-evaluated (round-2
     * fix), but the verdict is unchanged, so no new row -- the ordinary
     * last-kind dedup, not the removed skip-set entry. */
    atlas_memory_pass_result r3;
    t8_run_pass(&e, &pol, &r3, &err);
    bool r3_has_row = false;
    if (r3.generation != 0) {
        t9_diff_kind_for(&e, r3.generation, atlas_buf_cstr(&claim_uid), NULL, NULL, &r3_has_row, &err);
    }
    T_CHECK_MSG(!r3_has_row, "an unchanged UNDETERMINED verdict must not re-emit a duplicate row");

    /* Coverage completes, over a source that genuinely no longer has the
     * symbol -- the honest verdict the incomplete look could not give. */
    (void)t9_seed_sem_generation(&e, false, NULL, NULL, &err);

    atlas_memory_pass_result r4;
    t8_run_pass(&e, &pol, &r4, &err);
    T_CHECK_MSG(r4.generation != 0, "expected a generation once coverage completes and the verdict "
               "changes");

    atlas_buf kind2 = ATLAS_BUF_INIT;
    bool found2 = false;
    t9_diff_kind_for(&e, r4.generation, atlas_buf_cstr(&claim_uid), &kind2, NULL, &found2, &err);
    T_CHECK_MSG(found2 && strcmp(atlas_buf_cstr(&kind2), "CONTRADICTED") == 0,
               "expected the UNDETERMINED claim to reopen to CONTRADICTED once coverage completed "
               "over a genuinely absent symbol, found=%d kind=%s", found2,
               found2 ? atlas_buf_cstr(&kind2) : "(none)");

    atlas_buf_free(&claim_uid);
    atlas_buf_free(&kind1);
    atlas_buf_free(&kind2);
    atlas_buf_free(&doc_uid);
    t8_env_close(&e);
}

/* (e) acceptance item 4's first half: for a tracked source, re-reading the
 * stored `blob_oid` through `git cat-file` reproduces the stored
 * `content_sha256`. */
static void test_rebuild_from_git_blob_matches_stored_hash(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    const char *repo = fx_repo(&e.fx);
    T_OK(fx_write(repo, "CLAUDE.md", "a tracked memory note with no anchors at all", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);

    const char *paths[] = {"CLAUDE.md"};
    atlas_syspolicy pol;
    t8_policy(&pol, ATLAS_MEMORY_SOURCE_REPO_FILE, paths, 1);

    atlas_memory_pass_result r1;
    t8_run_pass(&e, &pol, &r1, &err);
    T_CHECK_MSG(r1.versions_added == 1, "expected one version row for the tracked source, got %zu",
               r1.versions_added);

    sqlite3_stmt *stmt = NULL;
    T_OK(atlas_db_prepare(e.db,
                          "SELECT v.blob_oid, v.content_sha256 FROM memory_source_versions v"
                          "  JOIN memory_sources s ON s.id = v.source_id WHERE s.repo_id = ?1;",
                          &stmt, &err),
         &err);
    T_REQUIRE(sqlite3_bind_int64(stmt, 1, e.repo_id) == SQLITE_OK);
    T_REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    char blob_oid[128];
    (void)snprintf(blob_oid, sizeof blob_oid, "%s", (const char *)sqlite3_column_text(stmt, 0));
    char content_sha256[128];
    (void)snprintf(content_sha256, sizeof content_sha256, "%s",
                  (const char *)sqlite3_column_text(stmt, 1));
    atlas_db_finish(e.db, stmt);
    T_CHECK_MSG(blob_oid[0] != '\0', "expected a non-empty blob_oid for a tracked source");

    const char *cat[] = {"cat-file", "blob", blob_oid};
    atlas_buf raw = ATLAS_BUF_INIT;
    int code = -1;
    T_OK(fx_git(&e.fx, repo, cat, 3u, &code, &raw, &err), &err);
    T_REQUIRE(code == 0);
    char hex[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_sha256_hex(raw.data, raw.len, hex);
    T_CHECK_MSG(strcmp(hex, content_sha256) == 0,
               "git cat-file's content did not hash to the stored content_sha256:\n"
               "  cat-file: %s\n  stored:   %s",
               hex, content_sha256);
    atlas_buf_free(&raw);

    t8_env_close(&e);
}

/* (f) `atlas_memory_plan_for` answers `UNKNOWN` on a freshly reconciled
 * repository and the right cause after each of (a)-(c)'s own perturbations,
 * asked before the pass runs and with no side effect. */
static void test_plan_for_answers_the_right_cause_before_the_pass(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    const char *repo = fx_repo(&e.fx);
    T_OK(fx_mkdir(repo, ".claude", &err), &err);
    T_OK(fx_mkdir(repo, ".claude/memories", &err), &err);
    const char *note1 = "a plain note with no anchors";
    T_OK(fx_write(repo, ".claude/memories/a.md", note1, &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    char hash1[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_sha256_hex(note1, strlen(note1), hash1);
    t8_seed_file(&e, ".claude/memories/a.md", hash1, &err);

    const char *paths[] = {".claude/memories"};
    atlas_syspolicy pol;
    t8_policy(&pol, ATLAS_MEMORY_SOURCE_REPO_DIR, paths, 1);

    atlas_memory_gen_cause cause = ATLAS_MEMORY_CAUSE_UNKNOWN;
    T_OK(atlas_memory_plan_for(e.db, &e.repo, &pol, &cause, &err), &err);
    T_CHECK_MSG(cause == ATLAS_MEMORY_CAUSE_SOURCE_REVISION,
               "expected SOURCE_REVISION before the first pass ever runs, got %s",
               atlas_memory_gen_cause_name(cause));

    atlas_memory_pass_result r1;
    t8_run_pass(&e, &pol, &r1, &err);
    T_REQUIRE(r1.generation != 0);

    cause = ATLAS_MEMORY_CAUSE_UNKNOWN;
    T_OK(atlas_memory_plan_for(e.db, &e.repo, &pol, &cause, &err), &err);
    T_CHECK_MSG(cause == ATLAS_MEMORY_CAUSE_UNKNOWN,
               "expected nothing owed on a freshly reconciled repository, got %s",
               atlas_memory_gen_cause_name(cause));

    /* (a)'s own perturbation: edit the registered directory's own child, and
     * re-index it -- plan_for reads the index, never the file. */
    const char *note2 = "an edited note with no anchors";
    T_OK(fx_write(repo, ".claude/memories/a.md", note2, &err), &err);
    char hash2[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_sha256_hex(note2, strlen(note2), hash2);
    t9_update_file_hash(&e, ".claude/memories/a.md", hash2, &err);

    cause = ATLAS_MEMORY_CAUSE_UNKNOWN;
    T_OK(atlas_memory_plan_for(e.db, &e.repo, &pol, &cause, &err), &err);
    T_CHECK_MSG(cause == ATLAS_MEMORY_CAUSE_SOURCE_REVISION,
               "expected SOURCE_REVISION after editing and re-indexing the source, got %s",
               atlas_memory_gen_cause_name(cause));

    atlas_memory_pass_result r2;
    t8_run_pass(&e, &pol, &r2, &err);
    T_REQUIRE(r2.generation == r1.generation + 1);

    /* (c)'s own perturbation: a claim bound to an unapproved decision, then
     * the approval. */
    atlas_buf doc_uid = ATLAS_BUF_INIT;
    t9_propose(&e, "a rule", &doc_uid, &err);
    char bullet[256];
    (void)snprintf(bullet, sizeof bullet, "see decision %s", atlas_buf_cstr(&doc_uid));
    T_OK(fx_write(repo, ".claude/memories/b.md", bullet, &err), &err);
    char hash3[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_sha256_hex(bullet, strlen(bullet), hash3);
    t8_seed_file(&e, ".claude/memories/b.md", hash3, &err);

    atlas_memory_pass_result r3;
    t8_run_pass(&e, &pol, &r3, &err);
    T_REQUIRE(r3.generation == r2.generation + 1);

    cause = ATLAS_MEMORY_CAUSE_UNKNOWN;
    T_OK(atlas_memory_plan_for(e.db, &e.repo, &pol, &cause, &err), &err);
    T_CHECK_MSG(cause == ATLAS_MEMORY_CAUSE_UNKNOWN,
               "expected nothing owed before the approval, got %s",
               atlas_memory_gen_cause_name(cause));

    t9_approve(&e, atlas_buf_cstr(&doc_uid), 1, &err);
    cause = ATLAS_MEMORY_CAUSE_UNKNOWN;
    T_OK(atlas_memory_plan_for(e.db, &e.repo, &pol, &cause, &err), &err);
    T_CHECK_MSG(cause == ATLAS_MEMORY_CAUSE_DECISION_REVISION,
               "expected DECISION_REVISION after the approval, got %s",
               atlas_memory_gen_cause_name(cause));

    atlas_buf_free(&doc_uid);
    t8_env_close(&e);
}

/* fix-round-2, C3: `atlas_db_memory_dir_hash_mismatch` used `LIKE '%.md'`,
 * which SQLite case-folds over ASCII by default, while `src/memory/read.c`'s
 * own suffix check (`memcmp`) does not. A file named `NOTES.MD` -- indexed
 * by a real scan, so it has a `files` row and a `content_hash`, but never
 * read as a memory file by `read.c` and so never given a
 * `memory_source_versions` row -- matched the mismatch check and answered
 * `changed_out = true` for ever, exactly the permanent-`SOURCE_REVISION`
 * loop round 1 set out to close, reopened by a second place deciding "is
 * this a memory file?" with a different case rule. */
static void test_dir_hash_mismatch_matches_read_c_exactly(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    const char *repo = fx_repo(&e.fx);
    T_OK(fx_mkdir(repo, ".claude", &err), &err);
    T_OK(fx_mkdir(repo, ".claude/memories", &err), &err);
    T_OK(fx_mkdir(repo, ".claude/memories/sub", &err), &err);
    const char *note1 = "a plain note with no anchors";
    T_OK(fx_write(repo, ".claude/memories/a.md", note1, &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    char hash1[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_sha256_hex(note1, strlen(note1), hash1);
    t8_seed_file(&e, ".claude/memories/a.md", hash1, &err);

    const char *paths[] = {".claude/memories"};
    atlas_syspolicy pol;
    t8_policy(&pol, ATLAS_MEMORY_SOURCE_REPO_DIR, paths, 1);

    atlas_memory_pass_result r1;
    t8_run_pass(&e, &pol, &r1, &err);
    T_REQUIRE(r1.generation != 0);

    atlas_memory_gen_cause cause = ATLAS_MEMORY_CAUSE_UNKNOWN;
    T_OK(atlas_memory_plan_for(e.db, &e.repo, &pol, &cause, &err), &err);
    T_CHECK_MSG(cause == ATLAS_MEMORY_CAUSE_UNKNOWN,
               "expected nothing owed right after reconciling, got %s",
               atlas_memory_gen_cause_name(cause));

    /* Indexed by a real scan (a files row, a content hash) but never a
     * memory file by read.c's own rule: wrong case, and one level too deep. */
    T_OK(fx_write(repo, ".claude/memories/NOTES.MD", "uppercase suffix, never ingested", &err), &err);
    T_OK(fx_write(repo, ".claude/memories/sub/deep.md", "one level too deep, never ingested", &err),
         &err);
    char hash_upper[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_sha256_hex("uppercase suffix, never ingested", strlen("uppercase suffix, never ingested"),
                     hash_upper);
    char hash_deep[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_sha256_hex("one level too deep, never ingested", strlen("one level too deep, never ingested"),
                     hash_deep);
    t8_seed_file(&e, ".claude/memories/NOTES.MD", hash_upper, &err);
    t8_seed_file(&e, ".claude/memories/sub/deep.md", hash_deep, &err);

    cause = ATLAS_MEMORY_CAUSE_UNKNOWN;
    T_OK(atlas_memory_plan_for(e.db, &e.repo, &pol, &cause, &err), &err);
    T_CHECK_MSG(cause == ATLAS_MEMORY_CAUSE_UNKNOWN,
               "an uppercase-suffixed or too-deep file (neither ever ingested by read.c) must not "
               "read as an owed SOURCE_REVISION, got %s",
               atlas_memory_gen_cause_name(cause));

    /* A real, matching change is still detected: read.c would ingest this
     * one, so its mismatch must still be seen. */
    const char *note2 = "an edited note with no anchors";
    T_OK(fx_write(repo, ".claude/memories/a.md", note2, &err), &err);
    char hash2[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_sha256_hex(note2, strlen(note2), hash2);
    t9_update_file_hash(&e, ".claude/memories/a.md", hash2, &err);

    cause = ATLAS_MEMORY_CAUSE_UNKNOWN;
    T_OK(atlas_memory_plan_for(e.db, &e.repo, &pol, &cause, &err), &err);
    T_CHECK_MSG(cause == ATLAS_MEMORY_CAUSE_SOURCE_REVISION,
               "a genuine one-level .md edit must still be detected, got %s",
               atlas_memory_gen_cause_name(cause));

    t8_env_close(&e);
}

/* fix-round-2, C3's other half: `?2` (the source's own path_text) is spliced
 * directly into a `LIKE` pattern, so a literal `%` or `_` inside it would
 * act as a wildcard rather than a literal character unless escaped first.
 * Driven directly against `atlas_db_memory_dir_hash_mismatch` with a real
 * `%XX`-encoded `path_text` (`atlas_path_text_encode`, matching what
 * `atlas_memory_plan_for` itself feeds this function; `t8_seed_file`'s own
 * raw-passthrough convention does not encode, so this test builds its
 * `files` row directly rather than through that helper) — a raw `%` byte in
 * the source directory's own name encodes to the literal three characters
 * `%25`, which is exactly the escaping this round's fix has to survive.
 *
 * fix-round-3 (Important): as filed in the round-2 re-review, this test
 * could not fail. It seeded exactly one `files` row, and both the escaped
 * pattern (`.claude/mem\%25o/%` ESCAPE '\\') and the unescaped one
 * (`.claude/mem%25o/%`, its stray `%` read as a wildcard matching the empty
 * string) matched that same row -- deleting `like_escape` entirely would
 * have left every assertion below green. A second, decoy `files` row closes
 * that: `.claude/memZZ25o/b.md` is reached only by the *unescaped* reading.
 * There, the pattern's own `%` (from the un-escaped literal `%` in
 * `.claude/mem%25o`) is a wildcard matching "ZZ", then the literal "25o/"
 * matches, then the trailing `%` matches "b.md" -- so the unescaped pattern
 * pulls this row in. The escaped pattern requires a literal `%` character
 * right after "mem" (from `\%`), which "ZZ" is not, so it never matches.
 * Only the real row's own version is ever inserted; the decoy is never
 * given one. With `like_escape` in place (as below), the escaped query
 * never selects the decoy in the first place, so its missing version is
 * irrelevant and `changed` correctly reads false once the real row's
 * version lands -- with the escaping removed, the unescaped query would
 * also select the decoy, find it has no version, and `changed` would flip
 * back to true. Verified by hand: commenting out the backslash-escaping
 * loop in `like_escape` (`src/db/db_memory.c`) turns the second
 * `T_CHECK_MSG` below red; restoring it turns the suite green again. */
static void test_dir_hash_mismatch_escapes_a_literal_percent_in_its_own_path(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    atlas_buf dir_encoded = ATLAS_BUF_INIT;
    T_OK(atlas_path_text_encode(".claude/mem%o", strlen(".claude/mem%o"), &dir_encoded, &err), &err);
    atlas_buf child_encoded = ATLAS_BUF_INIT;
    T_OK(atlas_path_text_encode(".claude/mem%o/a.md", strlen(".claude/mem%o/a.md"), &child_encoded,
                                &err),
         &err);
    /* Not `%XX`-encoded: it contains no byte `atlas_path_text_encode` would
     * ever escape, so its raw and encoded forms are identical, and writing
     * it raw keeps this literal readable as the "only the unescaped LIKE
     * pattern reaches this" argument above. */
    const char *decoy_path = ".claude/memZZ25o/b.md";

    int64_t source_id = 0;
    T_OK(atlas_db_memory_source_upsert(e.db, e.repo_id, ATLAS_MEMORY_SOURCE_REPO_DIR, ".claude/mem%o",
                                       strlen(".claude/mem%o"), atlas_buf_cstr(&dir_encoded),
                                       "2026-01-01T00:00:00Z", &source_id, NULL, &err),
         &err);

    atlas_buf sql = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&sql, &err,
                           "INSERT INTO scans(repo_id, started_at, status)"
                           "  VALUES(%lld, '2026-01-01T00:00:00Z', 'ok');"
                           "INSERT INTO files(repo_id, path_raw, path_text, file_type,"
                           "  content_hash, size_bytes, first_seen_scan_id, last_seen_scan_id,"
                           "  first_seen_at, last_seen_at)"
                           "  VALUES(%lld, CAST('%s' AS BLOB), '%s', 'regular',"
                           "         '1111111111111111111111111111111111111111111111111111111111111111',"
                           "         128, last_insert_rowid(), last_insert_rowid(),"
                           "         '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z');"
                           "INSERT INTO files(repo_id, path_raw, path_text, file_type,"
                           "  content_hash, size_bytes, first_seen_scan_id, last_seen_scan_id,"
                           "  first_seen_at, last_seen_at)"
                           "  VALUES(%lld, CAST('%s' AS BLOB), '%s', 'regular',"
                           "         '2222222222222222222222222222222222222222222222222222222222222222',"
                           "         128, last_insert_rowid(), last_insert_rowid(),"
                           "         '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z');",
                           (long long)e.repo_id, (long long)e.repo_id, atlas_buf_cstr(&child_encoded),
                           atlas_buf_cstr(&child_encoded), (long long)e.repo_id, decoy_path, decoy_path),
         &err);
    T_OK(atlas_db_exec_sql(e.db, atlas_buf_cstr(&sql), &err), &err);
    atlas_buf_free(&sql);

    bool changed = true;
    T_OK(atlas_db_memory_dir_hash_mismatch(e.db, e.repo_id, source_id, atlas_buf_cstr(&dir_encoded),
                                           &changed, &err),
         &err);
    T_CHECK_MSG(changed,
               "expected a mismatch: the file has no memory_source_versions row and none was "
               "recorded");

    T_OK(atlas_db_memory_version_insert(
             e.db, source_id, "", "",
             "1111111111111111111111111111111111111111111111111111111111111111", 1, "x", 1,
             "2026-01-01T00:00:00Z", "2026-01-01T00:00:00Z", (int64_t)geteuid(), NULL, NULL, &err),
         &err);
    changed = true;
    T_OK(atlas_db_memory_dir_hash_mismatch(e.db, e.repo_id, source_id, atlas_buf_cstr(&dir_encoded),
                                           &changed, &err),
         &err);
    T_CHECK_MSG(!changed,
               "expected no mismatch once the version is recorded -- a literal '%%' (encoded '%%25') "
               "in the source's own path must not act as a wildcard and pull in an unrelated row");

    atlas_buf_free(&dir_encoded);
    atlas_buf_free(&child_encoded);
    t8_env_close(&e);
}

/* T9 fix-round-3 (C3, still-open finding), extended in fix-round-4.
 * `atlas_db_memory_dir_hash_mismatch` had no `file_type`, size or deletion
 * predicate at all, so it treated every `files` row one level under a
 * REPO_DIR source's own path as a candidate memory file regardless of
 * whether `src/memory/read.c` would ever give it a
 * `memory_source_versions` row. Five kinds of row pass every predicate the
 * query had before fix-round-3 -- right source, right depth, a literal
 * `.md` suffix -- and are never versioned by `read.c`, so each reported
 * `changed_out = true` for ever, the same permanent-`SOURCE_REVISION` loop
 * the depth fix (round 1) and the case fix (round 2) each closed one door
 * of. Doors 1 and 4 are the two a real scan can actually produce; doors 2
 * and 3 never carry a stored `content_hash` in production and were already
 * excluded by this loop's own `hash == NULL` skip before fix-round-3 added
 * a type predicate -- kept below anyway, as defence in depth, but verified
 * rather than presented as reproductions of a live failure. Door 5 is the
 * one fix-round-3 missed, by the same reasoning that overstated doors 2 and
 * 3: it treated entry *type* as the discriminator for "would `read.c`
 * ingest this", when the real discriminator is "does this row still
 * describe current content" -- type answers that only for a symlink.
 *
 *  1. a symlink named `x.md` -- `read.c`'s listing filter excludes only
 *     `S_ISDIR` (`read.c:414-421`), so a symlink is listed, then refused by
 *     `open_fs_file` with outcome `ATLAS_MEMORY_READ_SYMLINK`
 *     (`read.c:129-135`); a real scan still gives it `file_type = 'symlink'`
 *     with `content_hash` = the hash of its link text, A13's own rule
 *     (`src/core/scan.c:329-341`). Live in production.
 *  2. a fifo/socket/device named `x.md` -- refused with outcome `ABSENT`
 *     (`read.c:140-146`); a real scan gives it `file_type = 'other'` and no
 *     `content_hash` at all (`outcome_file_type` maps every such outcome to
 *     `'other'` and `rec.content_hash` is assigned only under
 *     `e->have_hash`, `src/core/reconcile.c:838-852,912-915`), so the loop's
 *     own NULL-hash skip already excluded it before this round. Not a live
 *     production door; kept as defence in depth.
 *  3. a `files` row recorded `file_type = 'missing'` -- the fourth member of
 *     the CHECK'd vocabulary (`src/db/migrate.c:85`), equally hash-less and
 *     equally already excluded by the same skip. Not a live production
 *     door; kept as defence in depth.
 *  4. a real, regular `.md` file over `ATLAS_MEMORY_MAX_SOURCE_BYTES` --
 *     `read.c:158-163` refuses it with outcome `TOO_LARGE` and no bytes,
 *     this season's own "a bound that is reached is refused, never
 *     trimmed", so `file_type = 'regular'` alone does not exclude it. A
 *     `size_bytes` of NULL -- a row this pass cannot even ask the question
 *     of -- is checked alongside it as the same predicate's other edge,
 *     never read as "small enough". Live in production.
 *  5. a `.md` file deleted from the tree -- `atlas_db_files_mark_deleted`
 *     (`src/db/db_index.c:404-407`) sets `deleted=1` and `deleted_scan_id`
 *     and touches nothing else, so the row keeps its last `file_type =
 *     'regular'`, in-bound `size_bytes` and real `content_hash`. `read.c`'s
 *     `readdir` cannot list a path that is gone (`read.c:397-433`), so no
 *     version is ever recorded for it. Live in production, and reachable
 *     with no race: any `.md` deleted before the memory source was ever
 *     registered reaches this from the very first call.
 *
 * Every row below has no `memory_source_versions` row recorded for it, and
 * every one must report no mismatch -- before its own fix, each did. */
/* One door's own row, inserted directly (the existing tests' own convention
 * for this function -- `t8_seed_file` always writes `file_type = 'regular'`,
 * `deleted = 0`, so it cannot build any of these). `size_bytes` is a
 * `const char *` (rather than a bound parameter) so `NULL` -- door 4's other
 * edge -- can be spelled as the SQL literal instead of a sentinel
 * `int64_t`. `deleted` seeds door 5: true sets `deleted = 1` and
 * `deleted_scan_id` to the same scan row `first_seen_scan_id` /
 * `last_seen_scan_id` already reference, via the same `last_insert_rowid()`
 * this function already relies on to bind them. */
static void t9_seed_door_file(t8env *e, const char *path, const char *file_type, const char *hash,
                              const char *size_bytes_literal, bool deleted, atlas_err *err) {
    atlas_buf sql = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&sql, err,
                           "INSERT INTO scans(repo_id, started_at, status)"
                           "  VALUES(%lld, '2026-01-01T00:00:00Z', 'ok');"
                           "INSERT INTO files(repo_id, path_raw, path_text, file_type,"
                           "  content_hash, size_bytes, first_seen_scan_id, last_seen_scan_id,"
                           "  first_seen_at, last_seen_at, deleted, deleted_scan_id)"
                           "  VALUES(%lld, CAST('%s' AS BLOB), '%s', '%s', '%s', %s,"
                           "         last_insert_rowid(), last_insert_rowid(),"
                           "         '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', %d, %s);",
                           (long long)e->repo_id, (long long)e->repo_id, path, path, file_type, hash,
                           size_bytes_literal, deleted ? 1 : 0,
                           deleted ? "last_insert_rowid()" : "NULL"),
         err);
    T_OK(atlas_db_exec_sql(e->db, atlas_buf_cstr(&sql), err), err);
    atlas_buf_free(&sql);
}

/* T9 fix-round-3 (C3, still-open finding), extended in fix-round-4.
 * `atlas_db_memory_dir_hash_mismatch` had no `file_type`, size or deletion
 * predicate at all, so it treated every `files` row one level under a
 * REPO_DIR source's own path as a candidate memory file regardless of
 * whether `src/memory/read.c` would ever give it a `memory_source_versions`
 * row. Five kinds of row pass every predicate the query had before
 * fix-round-3 -- right source, right depth, a literal `.md` suffix -- and
 * are never versioned by `read.c`, so each reported `changed_out = true` for
 * ever, the same permanent-`SOURCE_REVISION` loop the depth fix (round 1)
 * and the case fix (round 2) each closed one door of. One case per door,
 * added one row at a time so a failing assertion names which door regressed
 * rather than only that some row among six did. Doors 1 and 4 are the two a
 * real scan can actually produce; doors 2 and 3 never carry a stored
 * `content_hash` in production and were already excluded by this loop's own
 * `hash == NULL` skip before fix-round-3 added a type predicate -- kept
 * below anyway, as defence in depth, but asserted as such rather than as
 * reproductions of a live failure. Door 5 is the one fix-round-3 missed, by
 * the same reasoning that overstated doors 2 and 3: it treated entry *type*
 * as the discriminator for "would `read.c` ingest this", when the real
 * discriminator is "does this row still describe current content" -- type
 * answers that only for a symlink:
 *
 *  1. a symlink named `x.md` -- `read.c`'s listing filter excludes only
 *     `S_ISDIR` (`read.c:414-421`), so a symlink is listed, then refused by
 *     `open_fs_file` with outcome `ATLAS_MEMORY_READ_SYMLINK`
 *     (`read.c:129-135`); a real scan still gives it `file_type = 'symlink'`
 *     with `content_hash` = the hash of its link text, A13's own rule
 *     (`src/core/scan.c:329-341`). Live in production.
 *  2. a fifo/socket/device named `x.md` -- refused with outcome `ABSENT`
 *     (`read.c:140-146`); a real scan gives it `file_type = 'other'` and no
 *     `content_hash` at all (`src/core/reconcile.c:838-852,912-915`), so the
 *     loop's own NULL-hash skip already excluded it before this round. Not a
 *     live production door; kept as defence in depth.
 *  3. a `files` row recorded `file_type = 'missing'` -- the fourth member of
 *     the CHECK'd vocabulary (`src/db/migrate.c:85`), equally hash-less and
 *     equally already excluded by the same skip. Not a live production
 *     door; kept as defence in depth.
 *  4. a real, regular `.md` file over `ATLAS_MEMORY_MAX_SOURCE_BYTES` --
 *     `read.c:158-163` refuses it with outcome `TOO_LARGE` and no bytes,
 *     this season's own "a bound that is reached is refused, never
 *     trimmed", so `file_type = 'regular'` alone does not exclude it. A
 *     `size_bytes` of NULL -- a row this pass cannot even ask the question
 *     of -- is checked as door 4's own other edge, on the same footing:
 *     never read as "small enough". Live in production.
 *  5. a `.md` file deleted from the tree -- `atlas_db_files_mark_deleted`
 *     (`src/db/db_index.c:404-407`) sets `deleted=1` and `deleted_scan_id`
 *     and touches nothing else, so the row keeps its last `file_type =
 *     'regular'`, in-bound `size_bytes` and real `content_hash`. `read.c`'s
 *     `readdir` cannot list a path that is gone (`read.c:397-433`), so no
 *     version is ever recorded for it. Live in production, and reachable
 *     with no race: any `.md` deleted before the memory source was ever
 *     registered reaches this from the very first call.
 *
 * Every row below has no `memory_source_versions` row recorded for it, and
 * every one must report no mismatch -- before its own fix, each did. */
static void test_dir_hash_mismatch_excludes_the_five_unreadable_doors(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    atlas_buf dir_encoded = ATLAS_BUF_INIT;
    T_OK(atlas_path_text_encode(".claude/memories", strlen(".claude/memories"), &dir_encoded, &err),
         &err);
    int64_t source_id = 0;
    T_OK(atlas_db_memory_source_upsert(e.db, e.repo_id, ATLAS_MEMORY_SOURCE_REPO_DIR,
                                       ".claude/memories", strlen(".claude/memories"),
                                       atlas_buf_cstr(&dir_encoded), "2026-01-01T00:00:00Z",
                                       &source_id, NULL, &err),
         &err);

    bool changed = true;

    /* Positive control, added per review: every assertion below this point is
     * `!changed`, and a query broken in a way that selects nothing --
     * `?2`'s binding, a `path_text` encoding mismatch, a typo in a column
     * name -- would report `!changed` for every one of them just as
     * correctly-excluded rows would, for the wrong reason. One ordinary,
     * real memory file proves the query is still live before any door is
     * added: unversioned, it must report a mismatch; versioned, it must
     * not. */
    t9_seed_door_file(&e, ".claude/memories/ordinary.md", "regular",
                      "2222222222222222222222222222222222222222222222222222222222222222", "128",
                      false, &err);
    changed = false;
    T_OK(atlas_db_memory_dir_hash_mismatch(e.db, e.repo_id, source_id, atlas_buf_cstr(&dir_encoded),
                                           &changed, &err),
         &err);
    T_CHECK_MSG(changed,
               "positive control: an ordinary regular .md file with no memory_source_versions row "
               "must report a mismatch -- if this is false, the query below is not actually "
               "selecting anything and every door assertion that follows is vacuous");
    T_OK(atlas_db_memory_version_insert(
             e.db, source_id, "", "",
             "2222222222222222222222222222222222222222222222222222222222222222", 1, "x", 1,
             "2026-01-01T00:00:00Z", "2026-01-01T00:00:00Z", (int64_t)geteuid(), NULL, NULL, &err),
         &err);
    changed = true;
    T_OK(atlas_db_memory_dir_hash_mismatch(e.db, e.repo_id, source_id, atlas_buf_cstr(&dir_encoded),
                                           &changed, &err),
         &err);
    T_CHECK_MSG(!changed,
               "positive control: once the ordinary file's version is recorded, it must no longer "
               "report a mismatch");

    /* Door 1: a symlink. */
    t9_seed_door_file(&e, ".claude/memories/link.md", "symlink",
                      "3333333333333333333333333333333333333333333333333333333333333333", "12",
                      false, &err);
    changed = true;
    T_OK(atlas_db_memory_dir_hash_mismatch(e.db, e.repo_id, source_id, atlas_buf_cstr(&dir_encoded),
                                           &changed, &err),
         &err);
    T_CHECK_MSG(!changed,
               "door 1: a symlink named x.md is refused by read.c (outcome SYMLINK) and never "
               "versioned -- it must not report a mismatch");

    /* Door 2: a fifo/socket/device. */
    t9_seed_door_file(&e, ".claude/memories/pipe.md", "other",
                      "4444444444444444444444444444444444444444444444444444444444444444", "0",
                      false, &err);
    changed = true;
    T_OK(atlas_db_memory_dir_hash_mismatch(e.db, e.repo_id, source_id, atlas_buf_cstr(&dir_encoded),
                                           &changed, &err),
         &err);
    T_CHECK_MSG(!changed,
               "door 2: a fifo/socket/device named x.md is refused by read.c (outcome ABSENT) and "
               "never versioned -- it must not report a mismatch");

    /* Door 3: file_type = 'missing'. */
    t9_seed_door_file(&e, ".claude/memories/gone.md", "missing",
                      "5555555555555555555555555555555555555555555555555555555555555555", "40",
                      false, &err);
    changed = true;
    T_OK(atlas_db_memory_dir_hash_mismatch(e.db, e.repo_id, source_id, atlas_buf_cstr(&dir_encoded),
                                           &changed, &err),
         &err);
    T_CHECK_MSG(!changed,
               "door 3: a files row recorded file_type = 'missing' is never given a listing entry "
               "by read.c -- it must not report a mismatch");

    /* Door 4: regular, over ATLAS_MEMORY_MAX_SOURCE_BYTES. */
    char over_limit[32];
    (void)snprintf(over_limit, sizeof over_limit, "%lld",
                  (long long)ATLAS_MEMORY_MAX_SOURCE_BYTES + 1);
    t9_seed_door_file(&e, ".claude/memories/huge.md", "regular",
                      "6666666666666666666666666666666666666666666666666666666666666666", over_limit,
                      false, &err);
    changed = true;
    T_OK(atlas_db_memory_dir_hash_mismatch(e.db, e.repo_id, source_id, atlas_buf_cstr(&dir_encoded),
                                           &changed, &err),
         &err);
    T_CHECK_MSG(!changed,
               "door 4: a regular .md file over ATLAS_MEMORY_MAX_SOURCE_BYTES is refused by "
               "read.c (outcome TOO_LARGE) and never versioned -- it must not report a mismatch");

    /* Door 4's other edge: regular, size_bytes NULL -- a row this check
     * cannot establish is in bound, so it must be excluded on the same
     * footing as one it can establish is over it. */
    t9_seed_door_file(&e, ".claude/memories/unknown-size.md", "regular",
                      "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd", "NULL",
                      false, &err);
    changed = true;
    T_OK(atlas_db_memory_dir_hash_mismatch(e.db, e.repo_id, source_id, atlas_buf_cstr(&dir_encoded),
                                           &changed, &err),
         &err);
    T_CHECK_MSG(!changed,
               "door 4's other edge: a regular .md file with size_bytes NULL cannot be shown to be "
               "in bound -- it must not report a mismatch either");

    /* Door 5 (fix-round-4): a regular, in-bound .md file deleted from the
     * tree. `deleted = true` seeds `deleted = 1` and `deleted_scan_id`,
     * everything else -- `file_type`, `size_bytes`, `content_hash` --
     * identical in shape to the positive control's own row, since that is
     * exactly what `atlas_db_files_mark_deleted` leaves behind. */
    t9_seed_door_file(&e, ".claude/memories/deleted.md", "regular",
                      "7777777777777777777777777777777777777777777777777777777777777777", "128",
                      true, &err);
    changed = true;
    T_OK(atlas_db_memory_dir_hash_mismatch(e.db, e.repo_id, source_id, atlas_buf_cstr(&dir_encoded),
                                           &changed, &err),
         &err);
    T_CHECK_MSG(!changed,
               "door 5: a regular, in-bound .md file marked deleted keeps its file_type, size_bytes "
               "and content_hash (atlas_db_files_mark_deleted touches nothing else) but read.c's "
               "readdir can never list a path that is gone -- it must not report a mismatch either");

    atlas_buf_free(&dir_encoded);
    t8_env_close(&e);
}

/* --- T11: memory.put, memory.status, memory.reconcile ----------------------
 *
 * `memory.put`, `memory.status` and `memory.reconcile` sit in the
 * SO_PEERCRED-gated operator group (`OPERATOR_METHODS[]`,
 * `src/ipc/server_decision.c`), and that gate is `atlas_authority_probe_peer`:
 * a root-owned policy file *and* a root-owned, unwritable running executable.
 * No test binary in this build tree is that executable -- `test_operator_peer.
 * c`'s own comment states the reason and its own precedent is to skip the
 * positive case entirely rather than fabricate a grant. `repo.scanner`, the
 * method beside these three, has never had an RPC-layer test at all for the
 * identical reason (verified by grep: no test references `method_repo_
 * scanner`); `backup.create`'s and `decision.approve`'s RPC-layer tests
 * (`test_backup_live.c`, `test_a71_syspolicy.c`) exercise only the same
 * negative path this file's own new case below does, and drive the *positive*
 * behaviour through the service function the method calls
 * (`atlas_service_backup_create`) rather than through the method itself.
 *
 * These tests follow that precedent rather than reinventing a way past it:
 * the wiring is checked directly (the three names are in the table), the
 * refusal is checked over a real socket against a real daemon (honestly
 * refused, for the structural reason above, not a stand-in), and every
 * behavioural claim -- a stored hash, a bound enforced before a write, a
 * restart's own survival -- is checked by driving `atlas_writer_memory_put`,
 * `atlas_writer_submit_memory_reconcile` and the database reads beneath
 * `memory.status` directly, exactly as `atlas_writer_call_repo_scanner` and
 * `atlas_db_repo_set_scanner_uid` are already tested one layer below their
 * own RPC method. */

/* `t11_scalar`, `t11_writer_open` and `t11_writer_close` used to be `static`
 * here. A12.1 T11 fix round, item 6: hoisted into
 * `tests/support/reconcile_env.h` alongside `t8env`, since
 * `test_memory_reconcile_live.c`'s restart case needs them too. */

static void test_memory_operator_methods_are_wired(void) {
    size_t n = 0;
    const atlas_method_entry *m = atlas_server_operator_methods(&n);
    bool has_put = false;
    bool has_status = false;
    bool has_reconcile = false;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(m[i].name, "memory.put") == 0) {
            has_put = true;
        }
        if (strcmp(m[i].name, "memory.status") == 0) {
            has_status = true;
        }
        if (strcmp(m[i].name, "memory.reconcile") == 0) {
            has_reconcile = true;
        }
    }
    T_CHECK_MSG(has_put, "memory.put is not in the operator method table");
    T_CHECK_MSG(has_status, "memory.status is not in the operator method table");
    T_CHECK_MSG(has_reconcile, "memory.reconcile is not in the operator method table");
}

/* `test_memory_methods_refuse_a_non_operator_peer` used to be here -- it forks
 * a real daemon (see its own comment, moved verbatim), and is one of the two
 * A12.1 T11 fix round, item 6 moves it out to `test_memory_reconcile_live.c`,
 * next to `test_memory_survives_a_daemon_restart`, the other one. Both cost
 * every case in this file `ctest -LE daemon` and parallelism under `ctest -j`
 * for as long as they stayed. */

static void test_put_stores_a_version_with_matching_hash(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    int64_t source_id = 0;
    atlas_buf source_uid = ATLAS_BUF_INIT;
    static const char PATH[] = "/home/u/notes.md";
    T_OK(atlas_db_memory_source_upsert(e.db, e.repo_id, ATLAS_MEMORY_SOURCE_EXTERNAL_FILE, PATH,
                                       strlen(PATH), PATH, "t0", &source_id, &source_uid, &err),
         &err);
    atlas_db_close(e.db);
    e.db = NULL;

    FILE *log = NULL;
    atlas_writer *w = NULL;
    t11_writer_open(&e, &log, &w, &err);

    atlas_memory_put_op op;
    atlas_memory_put_op_init(&op);
    T_OK(atlas_buf_set(&op.source_uid, source_uid.data, source_uid.len, &err), &err);
    static const char CONTENT[] = "the daemon reads notes.md";
    T_OK(atlas_buf_set(&op.content, CONTENT, strlen(CONTENT), &err), &err);
    T_OK(atlas_buf_set_str(&op.observed_at, "2026-01-01T00:00:00Z", &err), &err);
    op.peer_uid = 1000;

    atlas_memory_put_result res;
    atlas_memory_put_result_init(&res);
    T_OK(atlas_writer_memory_put(w, &op, &res, &err), &err);

    char want_hex[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_sha256_hex(CONTENT, strlen(CONTENT), want_hex);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&res.content_sha256), want_hex) == 0,
                "content_sha256 mismatch: got %s want %s", atlas_buf_cstr(&res.content_sha256),
                want_hex);
    T_CHECK_MSG(res.created, "the first put for new content should have created a version");
    T_CHECK_MSG(res.content_bytes == (int64_t)strlen(CONTENT), "content_bytes mismatch: got %lld",
                (long long)res.content_bytes);
    T_CHECK(res.version_uid.len > 0);

    atlas_memory_put_op_free(&op);

    t11_writer_close(log, w);

    atlas_db *rdb = NULL;
    T_OK(atlas_db_open_readonly(atlas_buf_cstr(&e.db_path), &rdb, &err), &err);
    atlas_memory_version_row row;
    atlas_memory_version_row_init(&row);
    bool found = false;
    T_OK(atlas_db_memory_version_latest(rdb, source_id, &row, &found, &err), &err);
    T_REQUIRE(found);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&row.content_sha256), want_hex) == 0,
                "the stored row's hash does not match the bytes that were put");
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&row.version_uid), atlas_buf_cstr(&res.version_uid)) == 0,
                "the stored row's uid does not match what memory.put reported");
    atlas_memory_version_row_free(&row);
    atlas_db_close(rdb);

    atlas_memory_put_result_free(&res);
    atlas_buf_free(&source_uid);
    t8_env_close(&e);
}

/* A13's rule, one layer over: a refusal names no repository and no path.
 * Two different unregistered uids must produce byte-identical refusals, and
 * neither refusal may name the (fabricated) uid that was actually tried --
 * an inventory handed to whoever asked is not a refusal. */
static void test_put_unregistered_source_names_nothing(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);
    atlas_db_close(e.db);
    e.db = NULL;

    FILE *log = NULL;
    atlas_writer *w = NULL;
    t11_writer_open(&e, &log, &w, &err);

    static const char *const UIDS[] = {"mdeadbeefdeadbeefdeadbeefdeadbee",
                                       "mffffffffffffffffffffffffffffff"};
    atlas_buf messages[2] = {ATLAS_BUF_INIT, ATLAS_BUF_INIT};
    for (size_t i = 0; i < 2; i++) {
        atlas_memory_put_op op;
        atlas_memory_put_op_init(&op);
        T_OK(atlas_buf_set_str(&op.source_uid, UIDS[i], &err), &err);
        T_OK(atlas_buf_set_str(&op.content, "x", &err), &err);
        T_OK(atlas_buf_set_str(&op.observed_at, "t0", &err), &err);
        op.peer_uid = 1000;
        atlas_memory_put_result res;
        atlas_memory_put_result_init(&res);
        atlas_err perr;
        atlas_err_init(&perr);
        atlas_status st = atlas_writer_memory_put(w, &op, &res, &perr);
        T_CHECK_MSG(st != ATLAS_OK, "a put against an unregistered source uid was accepted");
        T_OK(atlas_buf_set_str(&messages[i], atlas_err_msg(&perr), &err), &err);
        atlas_memory_put_op_free(&op);
        atlas_memory_put_result_free(&res);
    }
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&messages[0]), atlas_buf_cstr(&messages[1])) == 0,
                "two different unregistered uids produced different refusals: [%s] vs [%s]",
                atlas_buf_cstr(&messages[0]), atlas_buf_cstr(&messages[1]));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&messages[0]), UIDS[0]) == NULL,
                "the refusal named the uid that was tried");
    T_CHECK_MSG(strstr(atlas_buf_cstr(&messages[0]), UIDS[1]) == NULL,
                "the refusal named a uid it was never even given");

    atlas_buf_free(&messages[0]);
    atlas_buf_free(&messages[1]);
    t11_writer_close(log, w);
    t8_env_close(&e);
}

/* Bytes over ATLAS_MEMORY_MAX_SOURCE_BYTES are refused before anything is
 * queued: the row count must not move. */
static void test_put_over_bound_is_refused_before_queueing(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    int64_t source_id = 0;
    atlas_buf source_uid = ATLAS_BUF_INIT;
    static const char PATH[] = "/home/u/big.md";
    T_OK(atlas_db_memory_source_upsert(e.db, e.repo_id, ATLAS_MEMORY_SOURCE_EXTERNAL_FILE, PATH,
                                       strlen(PATH), PATH, "t0", &source_id, &source_uid, &err),
         &err);
    int64_t before = t11_scalar(e.db, "SELECT COUNT(*) FROM memory_source_versions;", &err);
    atlas_db_close(e.db);
    e.db = NULL;

    FILE *log = NULL;
    atlas_writer *w = NULL;
    t11_writer_open(&e, &log, &w, &err);

    atlas_memory_put_op op;
    atlas_memory_put_op_init(&op);
    T_OK(atlas_buf_set(&op.source_uid, source_uid.data, source_uid.len, &err), &err);
    size_t big = (size_t)ATLAS_MEMORY_MAX_SOURCE_BYTES + 1u;
    char *buf = malloc(big);
    T_REQUIRE(buf != NULL);
    memset(buf, 'a', big);
    T_OK(atlas_buf_set(&op.content, buf, big, &err), &err);
    free(buf);
    T_OK(atlas_buf_set_str(&op.observed_at, "t0", &err), &err);
    op.peer_uid = 1000;

    atlas_memory_put_result res;
    atlas_memory_put_result_init(&res);
    atlas_status st = atlas_writer_memory_put(w, &op, &res, &err);
    T_CHECK_MSG(st != ATLAS_OK, "an oversized put was accepted");
    atlas_memory_put_op_free(&op);
    atlas_memory_put_result_free(&res);
    t11_writer_close(log, w);

    atlas_db *rdb = NULL;
    T_OK(atlas_db_open_readonly(atlas_buf_cstr(&e.db_path), &rdb, &err), &err);
    int64_t after = t11_scalar(rdb, "SELECT COUNT(*) FROM memory_source_versions;", &err);
    atlas_db_close(rdb);
    T_CHECK_MSG(after == before, "the version row count moved from %lld to %lld despite the refusal",
                (long long)before, (long long)after);

    atlas_buf_free(&source_uid);
    t8_env_close(&e);
}

/* Only an EXTERNAL_* source accepts memory.put -- a REPO_* source is read
 * directly by the daemon or by a named scanner's mirror, and a client handing
 * over bytes for one would let a caller assert content the daemon never
 * itself read. */
static void test_put_refuses_a_repo_class_source(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    int64_t source_id = 0;
    atlas_buf source_uid = ATLAS_BUF_INIT;
    static const char PATH[] = "a.c";
    T_OK(atlas_db_memory_source_upsert(e.db, e.repo_id, ATLAS_MEMORY_SOURCE_REPO_FILE, PATH,
                                       strlen(PATH), PATH, "t0", &source_id, &source_uid, &err),
         &err);
    atlas_db_close(e.db);
    e.db = NULL;

    FILE *log = NULL;
    atlas_writer *w = NULL;
    t11_writer_open(&e, &log, &w, &err);

    atlas_memory_put_op op;
    atlas_memory_put_op_init(&op);
    T_OK(atlas_buf_set(&op.source_uid, source_uid.data, source_uid.len, &err), &err);
    T_OK(atlas_buf_set_str(&op.content, "x", &err), &err);
    T_OK(atlas_buf_set_str(&op.observed_at, "t0", &err), &err);
    op.peer_uid = 1000;
    atlas_memory_put_result res;
    atlas_memory_put_result_init(&res);
    atlas_status st = atlas_writer_memory_put(w, &op, &res, &err);
    T_CHECK_MSG(st != ATLAS_OK, "a put against a REPO_FILE source was accepted");
    atlas_memory_put_op_free(&op);
    atlas_memory_put_result_free(&res);

    t11_writer_close(log, w);
    atlas_buf_free(&source_uid);
    t8_env_close(&e);
}

/* rel_path is a class-consistency check, never stored: absent or malformed
 * for a *_DIR source is refused, and T6's own DIR contract (one path
 * component, ATLAS_MEMORY_DIR_SUFFIX) is enforced rather than merely hoped
 * for. */
static void test_put_dir_source_needs_a_dot_md_child(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    int64_t source_id = 0;
    atlas_buf source_uid = ATLAS_BUF_INIT;
    static const char PATH[] = "/home/u/notes";
    T_OK(atlas_db_memory_source_upsert(e.db, e.repo_id, ATLAS_MEMORY_SOURCE_EXTERNAL_DIR, PATH,
                                       strlen(PATH), PATH, "t0", &source_id, &source_uid, &err),
         &err);
    atlas_db_close(e.db);
    e.db = NULL;

    FILE *log = NULL;
    atlas_writer *w = NULL;
    t11_writer_open(&e, &log, &w, &err);

    static const char *const BAD_REL[] = {"", "sub/x.md", "notes.txt", ".", ".."};
    for (size_t i = 0; i < sizeof BAD_REL / sizeof BAD_REL[0]; i++) {
        atlas_memory_put_op op;
        atlas_memory_put_op_init(&op);
        T_OK(atlas_buf_set(&op.source_uid, source_uid.data, source_uid.len, &err), &err);
        T_OK(atlas_buf_set_str(&op.rel_path, BAD_REL[i], &err), &err);
        T_OK(atlas_buf_set_str(&op.content, "x", &err), &err);
        T_OK(atlas_buf_set_str(&op.observed_at, "t0", &err), &err);
        op.peer_uid = 1000;
        atlas_memory_put_result res;
        atlas_memory_put_result_init(&res);
        atlas_status st = atlas_writer_memory_put(w, &op, &res, &err);
        T_CHECK_MSG(st != ATLAS_OK, "rel_path \"%s\" should have been refused for a *_DIR source",
                    BAD_REL[i]);
        atlas_memory_put_op_free(&op);
        atlas_memory_put_result_free(&res);
    }

    atlas_memory_put_op op;
    atlas_memory_put_op_init(&op);
    T_OK(atlas_buf_set(&op.source_uid, source_uid.data, source_uid.len, &err), &err);
    T_OK(atlas_buf_set_str(&op.rel_path, "notes.md", &err), &err);
    T_OK(atlas_buf_set_str(&op.content, "x", &err), &err);
    T_OK(atlas_buf_set_str(&op.observed_at, "t0", &err), &err);
    op.peer_uid = 1000;
    atlas_memory_put_result res;
    atlas_memory_put_result_init(&res);
    T_OK(atlas_writer_memory_put(w, &op, &res, &err), &err);
    atlas_memory_put_op_free(&op);
    atlas_memory_put_result_free(&res);

    t11_writer_close(log, w);
    atlas_buf_free(&source_uid);
    t8_env_close(&e);
}

/* atlas_db_memory_version_exists' own dedup: the same content put twice for
 * one source is one row, and the second call reports created=false while
 * still naming the row that answers for these bytes. */
static void test_put_same_content_twice_is_not_a_new_version(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    int64_t source_id = 0;
    atlas_buf source_uid = ATLAS_BUF_INIT;
    static const char PATH[] = "/home/u/notes.md";
    T_OK(atlas_db_memory_source_upsert(e.db, e.repo_id, ATLAS_MEMORY_SOURCE_EXTERNAL_FILE, PATH,
                                       strlen(PATH), PATH, "t0", &source_id, &source_uid, &err),
         &err);
    atlas_db_close(e.db);
    e.db = NULL;

    FILE *log = NULL;
    atlas_writer *w = NULL;
    t11_writer_open(&e, &log, &w, &err);

    atlas_buf uid1 = ATLAS_BUF_INIT;
    atlas_buf uid2 = ATLAS_BUF_INIT;
    for (int i = 0; i < 2; i++) {
        atlas_memory_put_op op;
        atlas_memory_put_op_init(&op);
        T_OK(atlas_buf_set(&op.source_uid, source_uid.data, source_uid.len, &err), &err);
        T_OK(atlas_buf_set_str(&op.content, "the same bytes every time", &err), &err);
        T_OK(atlas_buf_set_str(&op.observed_at, i == 0 ? "t0" : "t1", &err), &err);
        op.peer_uid = 1000;
        atlas_memory_put_result res;
        atlas_memory_put_result_init(&res);
        T_OK(atlas_writer_memory_put(w, &op, &res, &err), &err);
        if (i == 0) {
            T_CHECK_MSG(res.created, "the first put of new content should have created a version");
            T_OK(atlas_buf_set(&uid1, res.version_uid.data, res.version_uid.len, &err), &err);
        } else {
            T_CHECK_MSG(!res.created, "putting identical content again should not create a new "
                                      "version");
            T_OK(atlas_buf_set(&uid2, res.version_uid.data, res.version_uid.len, &err), &err);
        }
        atlas_memory_put_op_free(&op);
        atlas_memory_put_result_free(&res);
    }
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&uid1), atlas_buf_cstr(&uid2)) == 0,
                "a dedup'd put reported a different version uid: %s vs %s", atlas_buf_cstr(&uid1),
                atlas_buf_cstr(&uid2));

    t11_writer_close(log, w);

    atlas_db *rdb = NULL;
    T_OK(atlas_db_open_readonly(atlas_buf_cstr(&e.db_path), &rdb, &err), &err);
    char sql[256];
    (void)snprintf(sql, sizeof sql,
                  "SELECT COUNT(*) FROM memory_source_versions WHERE source_id = %lld;",
                  (long long)source_id);
    T_CHECK_MSG(t11_scalar(rdb, sql, &err) == 1,
                "identical content put twice should still be exactly one version row");
    atlas_db_close(rdb);

    atlas_buf_free(&uid1);
    atlas_buf_free(&uid2);
    atlas_buf_free(&source_uid);
    t8_env_close(&e);
}

/* --- T11 fix round -----------------------------------------------------------
 *
 * Four of the six items are proved here, against the real IPC-layer method
 * functions rather than a level below them: `atlas_server_memory_status` and
 * `atlas_server_memory_put`, driven with a hand-built `dispatch_state`, below
 * the SO_PEERCRED operator gate that `atlas_server_dispatch` applies one layer
 * up. That gate is `atlas_authority_probe_peer` -- a root-owned policy file
 * *and* a root-owned, unwritable running executable -- and no test binary in
 * this build tree is that executable (this section's own older comment, and
 * `test_operator_peer.c`'s). Nothing here fabricates a grant of that
 * authority: what these cases drive is the method's own logic, exactly as
 * `atlas_writer_memory_put` is already driven one layer below its own RPC
 * method throughout this file. `ds.ctx` is `NULL` for the two `memory.status`
 * cases (the method never touches it) and a real `atlas_server_ctx` with a
 * live writer for the two `memory.put` cases (the method checks
 * `ds->ctx->writer` before reaching one). */

/* Important 1: the projection itself, directly -- `atlas_db_memory_version_
 * latest_meta` must report exactly what `atlas_db_memory_version_latest`
 * reports, minus the content buffer, so `emit_source`'s switch to it changes
 * nothing about what `memory.status` already reported. */
static void test_version_latest_meta_omits_content(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    int64_t source_id = 0;
    atlas_buf source_uid = ATLAS_BUF_INIT;
    static const char PATH[] = "/home/u/notes.md";
    T_OK(atlas_db_memory_source_upsert(e.db, e.repo_id, ATLAS_MEMORY_SOURCE_EXTERNAL_FILE, PATH,
                                       strlen(PATH), PATH, "t0", &source_id, &source_uid, &err),
         &err);
    atlas_db_close(e.db);
    e.db = NULL;

    FILE *log = NULL;
    atlas_writer *w = NULL;
    t11_writer_open(&e, &log, &w, &err);

    static const char CONTENT[] = "the daemon reads notes.md";
    atlas_memory_put_op op;
    atlas_memory_put_op_init(&op);
    T_OK(atlas_buf_set(&op.source_uid, source_uid.data, source_uid.len, &err), &err);
    T_OK(atlas_buf_set(&op.content, CONTENT, strlen(CONTENT), &err), &err);
    T_OK(atlas_buf_set_str(&op.observed_at, "2026-01-01T00:00:00Z", &err), &err);
    op.peer_uid = 1000;
    atlas_memory_put_result res;
    atlas_memory_put_result_init(&res);
    T_OK(atlas_writer_memory_put(w, &op, &res, &err), &err);
    atlas_memory_put_op_free(&op);
    atlas_memory_put_result_free(&res);
    t11_writer_close(log, w);

    atlas_db *rdb = NULL;
    T_OK(atlas_db_open_readonly(atlas_buf_cstr(&e.db_path), &rdb, &err), &err);

    atlas_memory_version_row full;
    atlas_memory_version_row_init(&full);
    bool found_full = false;
    T_OK(atlas_db_memory_version_latest(rdb, source_id, &full, &found_full, &err), &err);
    T_REQUIRE(found_full);
    T_CHECK_MSG(full.content.len == strlen(CONTENT),
                "atlas_db_memory_version_latest did not read the content back");

    atlas_memory_version_row meta;
    atlas_memory_version_row_init(&meta);
    bool found_meta = false;
    T_OK(atlas_db_memory_version_latest_meta(rdb, source_id, &meta, &found_meta, &err), &err);
    T_REQUIRE(found_meta);
    T_CHECK_MSG(meta.content.len == 0,
                "atlas_db_memory_version_latest_meta fetched content it must not fetch");
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&meta.version_uid), atlas_buf_cstr(&full.version_uid)) == 0,
                "the metadata-only read disagreed with the full read about the version uid");
    T_CHECK_MSG(
        strcmp(atlas_buf_cstr(&meta.content_sha256), atlas_buf_cstr(&full.content_sha256)) == 0,
        "the metadata-only read disagreed with the full read about content_sha256");
    T_CHECK_MSG(meta.content_bytes == full.content_bytes,
                "the metadata-only read disagreed with the full read about content_bytes");

    atlas_memory_version_row_free(&full);
    atlas_memory_version_row_free(&meta);
    atlas_db_close(rdb);
    atlas_buf_free(&source_uid);
    t8_env_close(&e);
}

/* Important 2: a read that fails must not answer the same as a source that
 * has never been put. `memory_source_versions.content_bytes` is renamed out
 * from under the query `atlas_db_memory_version_latest_meta` depends on --
 * `ALTER TABLE ... RENAME COLUMN` rather than `DROP COLUMN`, since a dropped
 * column that a table-level CHECK still names is refused by SQLite and this
 * one is ordinary column renaming, not a CHECK's business -- so
 * `atlas_db_prepare` fails with "no such column", a genuine read failure with
 * nothing wrong about the row itself. Driven through
 * `atlas_server_memory_status` directly: `ds.ctx` stays `NULL` because the
 * method never reaches it before this failure. */
static void test_status_reports_a_failed_read_rather_than_an_absent_version(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    int64_t source_id = 0;
    atlas_buf source_uid = ATLAS_BUF_INIT;
    static const char PATH[] = "/home/u/notes.md";
    T_OK(atlas_db_memory_source_upsert(e.db, e.repo_id, ATLAS_MEMORY_SOURCE_EXTERNAL_FILE, PATH,
                                       strlen(PATH), PATH, "t0", &source_id, &source_uid, &err),
         &err);
    T_OK(atlas_db_exec_sql(e.db,
                           "ALTER TABLE memory_source_versions"
                           " RENAME COLUMN content_bytes TO content_bytes_gone;",
                           &err),
         &err);
    atlas_db_close(e.db);
    e.db = NULL;

    atlas_db *rdb = NULL;
    T_OK(atlas_db_open_readonly(atlas_buf_cstr(&e.db_path), &rdb, &err), &err);

    dispatch_state ds;
    memset(&ds, 0, sizeof ds);
    ds.db = rdb;
    atlas_safe_pool_init(&ds.safe);
    FILE *sink = fopen("/dev/null", "we");
    T_REQUIRE_MSG(sink != NULL, "cannot open a json sink");
    atlas_err jerr;
    atlas_err_init(&jerr);
    ds.j = atlas_json_new(sink, &jerr);
    T_REQUIRE(ds.j != NULL);

    atlas_buf body = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&body, &err,
                           "{\"id\":\"t\",\"method\":\"memory.status\",\"params\":{\"repo\":"
                           "\"proj\"}}"),
         &err);
    atlas_ipc_request *req = NULL;
    T_OK(atlas_ipc_request_parse(body.data, body.len, &req, &err), &err);

    atlas_err merr;
    atlas_err_init(&merr);
    T_OK(atlas_json_obj_begin(ds.j, &merr), &merr);
    atlas_status st = atlas_server_memory_status(&ds, req, &merr);
    T_CHECK_MSG(st != ATLAS_OK, "a genuine read failure was reported as a clean status read");
    T_CHECK_MSG(atlas_err_msg(&merr)[0] != '\0', "a failed read left no explanation behind");

    atlas_ipc_request_free(req);
    atlas_buf_free(&body);
    atlas_json_free(ds.j);
    (void)fclose(sink);
    atlas_safe_pool_free(&ds.safe);
    atlas_db_close(rdb);
    atlas_buf_free(&source_uid);
    t8_env_close(&e);
}

/* Minor 2: an emptied external file is content, not the absence of a put.
 * Driven through `atlas_server_memory_put` directly with a real writer in
 * `ds.ctx` (the fix lives in the request's own hex decoder, so the write must
 * actually land), and confirmed two ways: the stored column is a real,
 * zero-length blob rather than NULL (`typeof`/`length` read directly, since a
 * zero-length blob's own `sqlite3_column_blob` pointer can itself be NULL and
 * `content.len == 0` alone cannot tell the two apart), and the reconciliation
 * pass reads the empty source back as zero propositions rather than as an
 * error. */
static void test_put_empty_content_is_a_zero_length_blob_not_null(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);
    /* t8_bind_head needs a real HEAD to bind to -- t8_env_open alone leaves
     * the fixture repository with no commit at all. */
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "int x;\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "seed", &err), &err);
    t8_bind_head(&e, &err);

    int64_t source_id = 0;
    atlas_buf source_uid = ATLAS_BUF_INIT;
    static const char PATH[] = "/home/u/notes.md";
    T_OK(atlas_db_memory_source_upsert(e.db, e.repo_id, ATLAS_MEMORY_SOURCE_EXTERNAL_FILE, PATH,
                                       strlen(PATH), PATH, "t0", &source_id, &source_uid, &err),
         &err);
    atlas_db_close(e.db);
    e.db = NULL;

    FILE *log = NULL;
    atlas_writer *w = NULL;
    t11_writer_open(&e, &log, &w, &err);

    atlas_server_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.db_path = atlas_buf_cstr(&e.db_path);
    ctx.data_dir = fx_data_dir(&e.fx);
    ctx.writer = w;

    dispatch_state ds;
    memset(&ds, 0, sizeof ds);
    ds.ctx = &ctx;
    atlas_safe_pool_init(&ds.safe);
    FILE *sink = fopen("/dev/null", "we");
    T_REQUIRE_MSG(sink != NULL, "cannot open a json sink");
    atlas_err jerr;
    atlas_err_init(&jerr);
    ds.j = atlas_json_new(sink, &jerr);
    T_REQUIRE(ds.j != NULL);

    atlas_buf body = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&body, &err,
                           "{\"id\":\"t\",\"method\":\"memory.put\",\"params\":{\"source\":\"%s\","
                           "\"content\":\"\",\"observed_at\":\"t0\"}}",
                           atlas_buf_cstr(&source_uid)),
         &err);
    atlas_ipc_request *req = NULL;
    T_OK(atlas_ipc_request_parse(body.data, body.len, &req, &err), &err);

    atlas_err merr;
    atlas_err_init(&merr);
    T_OK(atlas_json_obj_begin(ds.j, &merr), &merr);
    T_OK(atlas_server_memory_put(&ds, req, &merr), &merr);

    atlas_ipc_request_free(req);
    atlas_buf_free(&body);
    atlas_json_free(ds.j);
    (void)fclose(sink);
    atlas_safe_pool_free(&ds.safe);
    t11_writer_close(log, w);

    atlas_db *rdb = NULL;
    T_OK(atlas_db_open_readonly(atlas_buf_cstr(&e.db_path), &rdb, &err), &err);
    sqlite3_stmt *stmt = NULL;
    T_OK(atlas_db_prepare(rdb,
                          "SELECT typeof(content), length(content) FROM memory_source_versions"
                          " WHERE source_id = ?1 ORDER BY id DESC LIMIT 1;",
                          &stmt, &err),
         &err);
    T_REQUIRE(sqlite3_bind_int64(stmt, 1, source_id) == SQLITE_OK);
    T_REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    const char *ty = (const char *)sqlite3_column_text(stmt, 0);
    T_CHECK_MSG(ty != NULL && strcmp(ty, "blob") == 0,
                "an emptied put stored typeof(content) = %s, not 'blob'",
                ty != NULL ? ty : "(null)");
    T_CHECK_MSG(sqlite3_column_int64(stmt, 1) == 0,
                "an emptied put's stored blob was not zero length");
    atlas_db_finish(rdb, stmt);
    atlas_db_close(rdb);

    /* And the reconciliation pass reads this back as "no propositions", not
     * as an error: observe_external_source hands an empty, non-NULL
     * atlas_buf to atlas_memory_extract (atlas_buf_set's own n == 0
     * guarantee), which produces zero candidates and ATLAS_OK. */
    T_OK(atlas_db_open(atlas_buf_cstr(&e.db_path), &e.db, &err), &err);
    const char *paths[] = {PATH};
    atlas_syspolicy pol;
    t8_policy(&pol, ATLAS_MEMORY_SOURCE_EXTERNAL_FILE, paths, 1);
    atlas_memory_pass_result result;
    t8_run_pass(&e, &pol, &result, &err);
    T_CHECK_MSG(result.claims_created == 0,
                "an emptied external source unexpectedly produced a claim");

    atlas_buf_free(&source_uid);
    t8_env_close(&e);
}

/* Minor 4: `observed_at` is bounded like every other parser, and refused
 * rather than truncated. Driven through `atlas_server_memory_put` directly
 * with `ds.ctx = NULL`: the length check runs before the function ever looks
 * at `ds->ctx`, so the refusal (and the row count check below) never depends
 * on a writer existing at all. */
static void test_put_refuses_an_oversized_observed_at_before_queueing(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    int64_t source_id = 0;
    atlas_buf source_uid = ATLAS_BUF_INIT;
    static const char PATH[] = "/home/u/notes.md";
    T_OK(atlas_db_memory_source_upsert(e.db, e.repo_id, ATLAS_MEMORY_SOURCE_EXTERNAL_FILE, PATH,
                                       strlen(PATH), PATH, "t0", &source_id, &source_uid, &err),
         &err);
    int64_t before = t11_scalar(e.db, "SELECT COUNT(*) FROM memory_source_versions;", &err);

    dispatch_state ds;
    memset(&ds, 0, sizeof ds);
    ds.db = e.db;
    atlas_safe_pool_init(&ds.safe);
    FILE *sink = fopen("/dev/null", "we");
    T_REQUIRE_MSG(sink != NULL, "cannot open a json sink");
    atlas_err jerr;
    atlas_err_init(&jerr);
    ds.j = atlas_json_new(sink, &jerr);
    T_REQUIRE(ds.j != NULL);

    char big_observed[ATLAS_VERIFY_NAME_MAX + 2u];
    memset(big_observed, 'a', sizeof big_observed - 1u);
    big_observed[sizeof big_observed - 1u] = '\0';

    atlas_buf body = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&body, &err,
                           "{\"id\":\"t\",\"method\":\"memory.put\",\"params\":{\"source\":\"%s\","
                           "\"content\":\"\",\"observed_at\":\"%s\"}}",
                           atlas_buf_cstr(&source_uid), big_observed),
         &err);
    atlas_ipc_request *req = NULL;
    T_OK(atlas_ipc_request_parse(body.data, body.len, &req, &err), &err);

    atlas_err merr;
    atlas_err_init(&merr);
    T_OK(atlas_json_obj_begin(ds.j, &merr), &merr);
    atlas_status st = atlas_server_memory_put(&ds, req, &merr);
    T_CHECK_MSG(st != ATLAS_OK, "an oversized observed_at was accepted");

    atlas_ipc_request_free(req);
    atlas_buf_free(&body);
    atlas_json_free(ds.j);
    (void)fclose(sink);
    atlas_safe_pool_free(&ds.safe);

    int64_t after = t11_scalar(e.db, "SELECT COUNT(*) FROM memory_source_versions;", &err);
    T_CHECK_MSG(after == before,
                "the version row count moved from %lld to %lld despite the refusal",
                (long long)before, (long long)after);

    atlas_buf_free(&source_uid);
    t8_env_close(&e);
}

/* `t11_wait_for_generation` and `test_memory_survives_a_daemon_restart`
 * used to be here. A12.1 T11 fix round, item 6: the restart case forks a
 * real daemon (see its own comment, moved verbatim to
 * `test_memory_reconcile_live.c`), and `t11_wait_for_generation` moved
 * with it into `tests/support/reconcile_env.h` -- nothing else in this
 * file calls it. */

/* --- T15: the proposed patch ------------------------------------------------
 *
 * `atlas_memory_patch_build` re-extracts a registered source's *current*
 * content and correlates each fresh candidate to a stored claim by anchor and
 * exact text -- it does not run T8's pass. So these fixtures seed claims,
 * anchors and `verify_results` rows directly, `test_memory_pack.c`'s own
 * disclosed choice (`pk_claim`/`pk_anchor`/`pk_result`) for the identical
 * reason stated there: this is about the patch function itself, not about
 * T7/T8's extraction pipeline. A `t15_` prefix follows this file's own
 * per-task naming (`t8_`, `t9_`, `t11_`) rather than reusing `pk_`, which
 * names `test_memory_pack.c`'s own env type. */

static void t15_register_source(t8env *e, const char *path, atlas_buf *uid_out, atlas_err *err) {
    int64_t id = 0;
    char now[64];
    atlas_now_iso8601(now, sizeof now);
    T_OK(atlas_db_memory_source_upsert(e->db, e->repo_id, ATLAS_MEMORY_SOURCE_REPO_FILE, path,
                                       strlen(path), path, now, &id, uid_out, err),
         err);
}

/* A claim, inserted directly rather than through
 * `atlas_verify_intake_apply_in_tx` -- `pk_claim`'s own comment, verbatim
 * reason. `text` must be byte-for-byte what the fixture's own memory file
 * says on the line this claim stands for: `atlas_memory_patch_build`
 * correlates a freshly re-extracted candidate to a claim by an exact text
 * match under a shared anchor, so a claim whose stored text does not match
 * the file's own bytes correlates to nothing. */
static void t15_claim(t8env *e, const char *text, atlas_buf *uid_out, int64_t *id_out,
                      atlas_err *err) {
    atlas_verify_claim c;
    atlas_verify_claim_init(&c);
    c.repo_id = e->repo_id;
    T_OK(atlas_buf_set_str(&c.text, text, err), err);
    T_OK(atlas_buf_set_str(&c.domain, "test", err), err);
    char now[64];
    atlas_now_iso8601(now, sizeof now);
    T_OK(atlas_db_verify_claim_insert(e->db, &c, now, err), err);
    *id_out = c.id;
    T_OK(atlas_buf_set(uid_out, c.uid.data, c.uid.len, err), err);
    atlas_verify_claim_free(&c);
}

/* Fix round (I1): `t15_claim`'s own body with `semantics` set to NORMATIVE
 * instead of left at the zero-initialised DESCRIPTIVE, so a test can exercise
 * the SUPERSEDED arm's NORMATIVE exclusion without disturbing `t15_claim`'s
 * seven other callers, every one of which wants DESCRIPTIVE. */
static void t15_claim_normative(t8env *e, const char *text, atlas_buf *uid_out, int64_t *id_out,
                                atlas_err *err) {
    atlas_verify_claim c;
    atlas_verify_claim_init(&c);
    c.repo_id = e->repo_id;
    c.semantics = ATLAS_CLAIM_NORMATIVE;
    T_OK(atlas_buf_set_str(&c.text, text, err), err);
    T_OK(atlas_buf_set_str(&c.domain, "test", err), err);
    char now[64];
    atlas_now_iso8601(now, sizeof now);
    T_OK(atlas_db_verify_claim_insert(e->db, &c, now, err), err);
    *id_out = c.id;
    T_OK(atlas_buf_set(uid_out, c.uid.data, c.uid.len, err), err);
    atlas_verify_claim_free(&c);
}

static void t15_anchor(t8env *e, const char *claim_uid, const char *path, atlas_err *err) {
    T_OK(atlas_db_memory_anchor_add(e->db, e->repo_id, claim_uid, ATLAS_MEMORY_ANCHOR_PATH, path, err),
         err);
}

/* A `verify_results` row, by raw SQL -- `pk_result`'s own shape, widened
 * with an explicit `basis` rather than hardcoding `DETERMINISTIC`: T15's
 * whole point is that "deterministically CONTRADICTED" is `state ==
 * CONTRADICTED && basis == DETERMINISTIC` and nothing else, so a fixture
 * that could only ever produce `DETERMINISTIC` could never exercise the
 * distinction the widened `atlas_db_verify_result_latest` exists for. */
static void t15_result(t8env *e, int64_t claim_id, const char *state, const char *basis,
                       const char *conflict, atlas_err *err) {
    char sql[512];
    char now[64];
    atlas_now_iso8601(now, sizeof now);
    (void)snprintf(sql, sizeof sql,
                  "INSERT INTO verify_results(claim_id, state, basis, confidence_score,"
                  "  calibration, algorithm, family_version, conflict, stale, created_at)"
                  " VALUES(%lld, '%s', '%s', 0, 'INSUFFICIENT_DATA', 'test', 1, '%s', 0, '%s');",
                  (long long)claim_id, state, basis, conflict, now);
    T_OK(atlas_db_exec_sql(e->db, sql, err), err);
}

/* A minimal `memory_generations` row, so `t15_diff` below has something to
 * reference -- the row's own digests and identity are never read by
 * anything these tests exercise, so arbitrary non-empty strings are honest. */
static void t15_seed_generation(t8env *e, int64_t *gen_id_out, atlas_err *err) {
    char now[64];
    atlas_now_iso8601(now, sizeof now);
    T_OK(atlas_db_memory_generation_insert(e->db, e->repo_id, 1, ATLAS_MEMORY_CAUSE_SOURCE_REVISION,
                                           "deadbeef", "", "d", "s", 0, now, gen_id_out, err),
         err);
}

static void t15_diff(t8env *e, int64_t generation_id, const char *claim_uid,
                     atlas_memory_diff_kind kind, atlas_err *err) {
    T_OK(atlas_db_memory_claim_diff_add(e->db, generation_id, claim_uid, kind, "", err), err);
}

/* Counts non-overlapping occurrences of `needle` in `haystack`. */
static size_t t15_count(const char *haystack, const char *needle) {
    size_t n = 0;
    const char *p = haystack;
    size_t nlen = strlen(needle);
    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += nlen;
    }
    return n;
}

/* (a) One deterministically CONTRADICTED descriptive line among three
 * healthy ones: the diff proposes exactly one deletion, its context lines
 * are the source's own bytes, and the source file on disk is byte-identical
 * after the build (`fx_tree_digest`) -- the local form of the repository's
 * first hard rule, never modify a registered target repository from Atlas'
 * own code. */
static void test_patch_build_proposes_exactly_the_contradicted_line(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);
    const char *repo = fx_repo(&e.fx);

    const char *l1 = "- `alpha.c` needs review of its checksum path";
    const char *l2 = "- `beta.c` behaves correctly under load";
    const char *l3 = "- `gamma.c` retries automatically on failure";
    const char *l4 = "- `delta.c` logs every error condition";
    char content[512];
    (void)snprintf(content, sizeof content, "%s\n%s\n%s\n%s\n", l1, l2, l3, l4);
    T_OK(fx_write(repo, "memo.md", content, &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);

    t8_seed_file(&e, "alpha.c", "1111111111111111111111111111111111111111111111111111111111111111",
                &err);
    t8_seed_file(&e, "beta.c", "2222222222222222222222222222222222222222222222222222222222222222",
                &err);
    t8_seed_file(&e, "gamma.c", "3333333333333333333333333333333333333333333333333333333333333333",
                &err);
    t8_seed_file(&e, "delta.c", "4444444444444444444444444444444444444444444444444444444444444444",
                &err);

    atlas_buf u1 = ATLAS_BUF_INIT, u2 = ATLAS_BUF_INIT, u3 = ATLAS_BUF_INIT, u4 = ATLAS_BUF_INIT;
    int64_t i1 = 0, i2 = 0, i3 = 0, i4 = 0;
    t15_claim(&e, l1, &u1, &i1, &err);
    t15_claim(&e, l2, &u2, &i2, &err);
    t15_claim(&e, l3, &u3, &i3, &err);
    t15_claim(&e, l4, &u4, &i4, &err);
    t15_anchor(&e, atlas_buf_cstr(&u1), "alpha.c", &err);
    t15_anchor(&e, atlas_buf_cstr(&u2), "beta.c", &err);
    t15_anchor(&e, atlas_buf_cstr(&u3), "gamma.c", &err);
    t15_anchor(&e, atlas_buf_cstr(&u4), "delta.c", &err);
    t15_result(&e, i1, "CONTRADICTED", "DETERMINISTIC", "CONTRADICTION", &err);
    t15_result(&e, i2, "VERIFIED", "DETERMINISTIC", "NONE", &err);
    t15_result(&e, i3, "VERIFIED", "DETERMINISTIC", "NONE", &err);
    t15_result(&e, i4, "VERIFIED", "DETERMINISTIC", "NONE", &err);

    atlas_buf source_uid = ATLAS_BUF_INIT;
    t15_register_source(&e, "memo.md", &source_uid, &err);

    char before[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(repo, before, &err), &err);

    atlas_buf diff = ATLAS_BUF_INIT, findings = ATLAS_BUF_INIT;
    T_OK(atlas_memory_patch_build(e.db, &e.repo, fx_data_dir(&e.fx), atlas_buf_cstr(&source_uid), &diff,
                                  &findings, &err),
         &err);

    char after[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(repo, after, &err), &err);
    T_CHECK_MSG(strcmp(before, after) == 0, "a patch build modified the repository");

    size_t hunks = t15_count(atlas_buf_cstr(&diff), "@@ -");
    T_CHECK_MSG(hunks == 1u, "expected exactly one hunk, got %zu; diff=\n%s", hunks,
               atlas_buf_cstr(&diff));

    char removed[256];
    (void)snprintf(removed, sizeof removed, "-%s\n", l1);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&diff), removed) != NULL,
               "expected the contradicted line as a removal; diff=\n%s", atlas_buf_cstr(&diff));

    char ctx2[256], ctx3[256], ctx4[256];
    (void)snprintf(ctx2, sizeof ctx2, " %s\n", l2);
    (void)snprintf(ctx3, sizeof ctx3, " %s\n", l3);
    (void)snprintf(ctx4, sizeof ctx4, " %s\n", l4);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&diff), ctx2) != NULL, "expected l2 as context; diff=\n%s",
               atlas_buf_cstr(&diff));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&diff), ctx3) != NULL, "expected l3 as context; diff=\n%s",
               atlas_buf_cstr(&diff));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&diff), ctx4) != NULL, "expected l4 as context; diff=\n%s",
               atlas_buf_cstr(&diff));

    char removed2[256], removed3[256], removed4[256];
    (void)snprintf(removed2, sizeof removed2, "-%s\n", l2);
    (void)snprintf(removed3, sizeof removed3, "-%s\n", l3);
    (void)snprintf(removed4, sizeof removed4, "-%s\n", l4);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&diff), removed2) == NULL, "l2 must not be proposed for removal");
    T_CHECK_MSG(strstr(atlas_buf_cstr(&diff), removed3) == NULL, "l3 must not be proposed for removal");
    T_CHECK_MSG(strstr(atlas_buf_cstr(&diff), removed4) == NULL, "l4 must not be proposed for removal");

    T_CHECK_MSG(t15_count(atlas_buf_cstr(&findings), "RETAINED") == 3u,
               "expected the three healthy lines to be findings, not hunks; findings=\n%s",
               atlas_buf_cstr(&findings));

    atlas_buf_free(&diff);
    atlas_buf_free(&findings);
    atlas_buf_free(&source_uid);
    atlas_buf_free(&u1);
    atlas_buf_free(&u2);
    atlas_buf_free(&u3);
    atlas_buf_free(&u4);
    t8_env_close(&e);
}

/* (b) A drifted line -- CONTRADICTED, DETERMINISTIC, conflict IMPLEMENTATION
 * -- produces a finding and no hunk. IMPLEMENTATION means the code diverged
 * from what was approved; the approved thing is not the thing that is
 * wrong, so proposing deletion here would be automatically adopting a
 * design because current code implements it, a named non-goal of this
 * season. */
static void test_patch_build_implementation_conflict_is_a_finding_not_a_hunk(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);
    const char *repo = fx_repo(&e.fx);

    const char *l1 = "- `epsilon.c` implements the approved widget design";
    char content[256];
    (void)snprintf(content, sizeof content, "%s\n", l1);
    T_OK(fx_write(repo, "drift.md", content, &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "epsilon.c", "5555555555555555555555555555555555555555555555555555555555555555",
                &err);

    atlas_buf uid = ATLAS_BUF_INIT;
    int64_t cid = 0;
    t15_claim(&e, l1, &uid, &cid, &err);
    t15_anchor(&e, atlas_buf_cstr(&uid), "epsilon.c", &err);
    t15_result(&e, cid, "CONTRADICTED", "DETERMINISTIC", "IMPLEMENTATION", &err);

    atlas_buf source_uid = ATLAS_BUF_INIT;
    t15_register_source(&e, "drift.md", &source_uid, &err);

    atlas_buf diff = ATLAS_BUF_INIT, findings = ATLAS_BUF_INIT;
    T_OK(atlas_memory_patch_build(e.db, &e.repo, fx_data_dir(&e.fx), atlas_buf_cstr(&source_uid), &diff,
                                  &findings, &err),
         &err);

    T_CHECK_MSG(diff.len == 0, "an IMPLEMENTATION-conflicted line must not be proposed; diff=\n%s",
               atlas_buf_cstr(&diff));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&findings), "IMPLEMENTATION_DRIFT") != NULL,
               "expected an IMPLEMENTATION_DRIFT finding; findings=\n%s", atlas_buf_cstr(&findings));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&findings), atlas_buf_cstr(&uid)) != NULL,
               "expected the finding to name the claim; findings=\n%s", atlas_buf_cstr(&findings));

    atlas_buf_free(&diff);
    atlas_buf_free(&findings);
    atlas_buf_free(&source_uid);
    atlas_buf_free(&uid);
    t8_env_close(&e);
}

/* (c) An unanchored line -- no backtick, no decision uid, no 40-hex commit --
 * produces neither a hunk nor a finding: nothing here can correlate it to
 * any stored assessment, and A9.2.2's asymmetry says that absence is not
 * evidence either way.
 *
 * Fix round (I3): a no-op implementation and this test's original single-line
 * fixture agreed (`diff.len == 0`, no `claim=` anywhere) for the same reason
 * -- there was nothing else in the source for a per-candidate finding to be
 * emitted about, so an implementation that emits a finding for every
 * candidate regardless of anchoring would have passed just as well. A second,
 * *healthy* anchored line (`t15_claim`/`t15_anchor`/`t15_result`, already used
 * by test (a)) makes the absence differential: the exact count of per-line
 * findings must be one, and it must name the healthy line's own claim, not
 * the unanchored one -- a no-op now yields zero and fails, and an
 * over-broad implementation that reports the unanchored line too yields two
 * and fails. A healthy neighbour rather than a contradicted one keeps this
 * case about correlation, not about hunk rendering, which (a) already
 * covers.
 *
 * Counted on `ordinal=`, not `claim=`: every `emit_finding` output carries
 * `ordinal=` (`patch.c`'s own shape -- the two summary findings,
 * `NONE_PROPOSED` and `UNREADABLE`, are built by hand and carry neither), but
 * `claim=` is written only when a claim uid is passed, so an implementation
 * that emitted a per-line finding for the unanchored line *without* a claim
 * uid attached would still read as one `claim=` occurrence and pass a
 * `claim=`-only count -- caught below by mutation. */
static void test_patch_build_unanchored_line_produces_neither(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);
    const char *repo = fx_repo(&e.fx);

    const char *l1 = "this note mentions nothing checkable at all";
    const char *l2 = "- `theta.c` starts up cleanly on every platform";
    char content[256];
    (void)snprintf(content, sizeof content, "%s\n%s\n", l1, l2);
    T_OK(fx_write(repo, "plain.md", content, &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "theta.c", "7777777777777777777777777777777777777777777777777777777777777777",
                &err);

    atlas_buf u2 = ATLAS_BUF_INIT;
    int64_t i2 = 0;
    t15_claim(&e, l2, &u2, &i2, &err);
    t15_anchor(&e, atlas_buf_cstr(&u2), "theta.c", &err);
    t15_result(&e, i2, "VERIFIED", "DETERMINISTIC", "NONE", &err);

    atlas_buf source_uid = ATLAS_BUF_INIT;
    t15_register_source(&e, "plain.md", &source_uid, &err);

    atlas_buf diff = ATLAS_BUF_INIT, findings = ATLAS_BUF_INIT;
    T_OK(atlas_memory_patch_build(e.db, &e.repo, fx_data_dir(&e.fx), atlas_buf_cstr(&source_uid), &diff,
                                  &findings, &err),
         &err);

    T_CHECK_MSG(diff.len == 0, "neither line here is proposed for deletion; diff=\n%s",
               atlas_buf_cstr(&diff));
    T_CHECK_MSG(t15_count(atlas_buf_cstr(&findings), "ordinal=") == 1u,
               "expected exactly one per-line finding (the healthy anchored line, not the "
               "unanchored one); findings=\n%s",
               atlas_buf_cstr(&findings));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&findings), "claim=") != NULL &&
               strstr(atlas_buf_cstr(&findings), atlas_buf_cstr(&u2)) != NULL,
               "expected the one finding to carry a claim= naming the healthy line's own claim; "
               "findings=\n%s",
               atlas_buf_cstr(&findings));

    atlas_buf_free(&diff);
    atlas_buf_free(&findings);
    atlas_buf_free(&source_uid);
    atlas_buf_free(&u2);
    t8_env_close(&e);
}

/* (d) A source with nothing to propose returns an empty diff and says so in
 * findings -- both halves asserted, and closed by mutation (the T15 report
 * records removing the summary-finding write and re-running this test). */
static void test_patch_build_nothing_to_propose_says_so_in_findings(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);
    const char *repo = fx_repo(&e.fx);

    T_OK(fx_write(repo, "quiet.md", "", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);

    atlas_buf source_uid = ATLAS_BUF_INIT;
    t15_register_source(&e, "quiet.md", &source_uid, &err);

    atlas_buf diff = ATLAS_BUF_INIT, findings = ATLAS_BUF_INIT;
    T_OK(atlas_memory_patch_build(e.db, &e.repo, fx_data_dir(&e.fx), atlas_buf_cstr(&source_uid), &diff,
                                  &findings, &err),
         &err);

    T_CHECK_MSG(diff.len == 0, "an empty source must propose nothing; diff=\n%s",
               atlas_buf_cstr(&diff));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&findings), "NONE_PROPOSED") != NULL,
               "expected findings to say nothing was proposed; findings=\n%s",
               atlas_buf_cstr(&findings));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&findings), "quiet.md") != NULL,
               "expected findings to name the source's own path; findings=\n%s",
               atlas_buf_cstr(&findings));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&findings), atlas_buf_cstr(&source_uid)) != NULL,
               "expected findings to name the source uid; findings=\n%s", atlas_buf_cstr(&findings));

    atlas_buf_free(&diff);
    atlas_buf_free(&findings);
    atlas_buf_free(&source_uid);
    t8_env_close(&e);
}

/* Bonus, beyond the plan's four: a claim whose most recently recorded diff
 * kind is SUPERSEDED is proposed for deletion even though it carries no
 * CONTRADICTED result at all -- the predicate's second, independent OR-arm,
 * exercised on its own rather than left implemented and untested. */
static void test_patch_build_superseded_diff_kind_proposes_deletion(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);
    const char *repo = fx_repo(&e.fx);

    const char *l1 = "- `zeta.c` needs another look before merging";
    char content[256];
    (void)snprintf(content, sizeof content, "%s\n", l1);
    T_OK(fx_write(repo, "superseded.md", content, &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "zeta.c", "6666666666666666666666666666666666666666666666666666666666666666",
                &err);

    atlas_buf uid = ATLAS_BUF_INIT;
    int64_t cid = 0;
    t15_claim(&e, l1, &uid, &cid, &err);
    t15_anchor(&e, atlas_buf_cstr(&uid), "zeta.c", &err);
    /* Deliberately no verify_results row at all: state stays UNVERIFIED, so
     * the deterministically-CONTRADICTED arm cannot be what proposes this
     * line's deletion. */
    int64_t gen_id = 0;
    t15_seed_generation(&e, &gen_id, &err);
    t15_diff(&e, gen_id, atlas_buf_cstr(&uid), ATLAS_MEMORY_DIFF_SUPERSEDED, &err);

    atlas_buf source_uid = ATLAS_BUF_INIT;
    t15_register_source(&e, "superseded.md", &source_uid, &err);

    atlas_buf diff = ATLAS_BUF_INIT, findings = ATLAS_BUF_INIT;
    T_OK(atlas_memory_patch_build(e.db, &e.repo, fx_data_dir(&e.fx), atlas_buf_cstr(&source_uid), &diff,
                                  &findings, &err),
         &err);

    char removed[256];
    (void)snprintf(removed, sizeof removed, "-%s\n", l1);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&diff), removed) != NULL,
               "a SUPERSEDED claim must be proposed for deletion; diff=\n%s", atlas_buf_cstr(&diff));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&findings), atlas_buf_cstr(&uid)) == NULL,
               "a proposed deletion must not also be listed as a finding; findings=\n%s",
               atlas_buf_cstr(&findings));

    atlas_buf_free(&diff);
    atlas_buf_free(&findings);
    atlas_buf_free(&source_uid);
    atlas_buf_free(&uid);
    t8_env_close(&e);
}

/* Fix round (I1): a SUPERSEDED diff kind must not, on its own, bypass the
 * IMPLEMENTATION exclusion the header states as an absolute. Same shape as
 * the bonus test above, plus a `verify_results` row whose conflict is
 * IMPLEMENTATION -- the shipped predicate's `superseded` term carried no
 * term for `conflict` at all, so this claim's line was proposed for deletion
 * before this round; it must now be a finding instead. */
static void test_patch_build_superseded_implementation_conflict_is_a_finding_not_a_hunk(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);
    const char *repo = fx_repo(&e.fx);

    const char *l1 = "- `iota.c` implements the approved retry design";
    char content[256];
    (void)snprintf(content, sizeof content, "%s\n", l1);
    T_OK(fx_write(repo, "superseded-impl.md", content, &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "iota.c", "8888888888888888888888888888888888888888888888888888888888888888",
                &err);

    atlas_buf uid = ATLAS_BUF_INIT;
    int64_t cid = 0;
    t15_claim(&e, l1, &uid, &cid, &err);
    t15_anchor(&e, atlas_buf_cstr(&uid), "iota.c", &err);
    t15_result(&e, cid, "CONTRADICTED", "DETERMINISTIC", "IMPLEMENTATION", &err);
    int64_t gen_id = 0;
    t15_seed_generation(&e, &gen_id, &err);
    t15_diff(&e, gen_id, atlas_buf_cstr(&uid), ATLAS_MEMORY_DIFF_SUPERSEDED, &err);

    atlas_buf source_uid = ATLAS_BUF_INIT;
    t15_register_source(&e, "superseded-impl.md", &source_uid, &err);

    atlas_buf diff = ATLAS_BUF_INIT, findings = ATLAS_BUF_INIT;
    T_OK(atlas_memory_patch_build(e.db, &e.repo, fx_data_dir(&e.fx), atlas_buf_cstr(&source_uid), &diff,
                                  &findings, &err),
         &err);

    T_CHECK_MSG(diff.len == 0,
               "a SUPERSEDED claim with an IMPLEMENTATION conflict must not be proposed for "
               "deletion; diff=\n%s",
               atlas_buf_cstr(&diff));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&findings), "IMPLEMENTATION_DRIFT") != NULL,
               "expected an IMPLEMENTATION_DRIFT finding; findings=\n%s", atlas_buf_cstr(&findings));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&findings), atlas_buf_cstr(&uid)) != NULL,
               "expected the finding to name the claim; findings=\n%s", atlas_buf_cstr(&findings));

    atlas_buf_free(&diff);
    atlas_buf_free(&findings);
    atlas_buf_free(&source_uid);
    atlas_buf_free(&uid);
    t8_env_close(&e);
}

/* Fix round (I1): a SUPERSEDED diff kind must not, on its own, bypass the
 * NORMATIVE exclusion either. Deliberately no `verify_results` row at all
 * (as in the original SUPERSEDED bonus test), so only the semantics guard
 * can be what keeps this out of a hunk. */
static void test_patch_build_superseded_normative_is_a_finding_not_a_hunk(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);
    const char *repo = fx_repo(&e.fx);

    const char *l1 = "- `kappa.c` must always retry three times before failing";
    char content[256];
    (void)snprintf(content, sizeof content, "%s\n", l1);
    T_OK(fx_write(repo, "superseded-norm.md", content, &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "kappa.c", "9999999999999999999999999999999999999999999999999999999999999999",
                &err);

    atlas_buf uid = ATLAS_BUF_INIT;
    int64_t cid = 0;
    t15_claim_normative(&e, l1, &uid, &cid, &err);
    t15_anchor(&e, atlas_buf_cstr(&uid), "kappa.c", &err);
    int64_t gen_id = 0;
    t15_seed_generation(&e, &gen_id, &err);
    t15_diff(&e, gen_id, atlas_buf_cstr(&uid), ATLAS_MEMORY_DIFF_SUPERSEDED, &err);

    atlas_buf source_uid = ATLAS_BUF_INIT;
    t15_register_source(&e, "superseded-norm.md", &source_uid, &err);

    atlas_buf diff = ATLAS_BUF_INIT, findings = ATLAS_BUF_INIT;
    T_OK(atlas_memory_patch_build(e.db, &e.repo, fx_data_dir(&e.fx), atlas_buf_cstr(&source_uid), &diff,
                                  &findings, &err),
         &err);

    T_CHECK_MSG(diff.len == 0,
               "a SUPERSEDED claim with NORMATIVE semantics must not be proposed for deletion; "
               "diff=\n%s",
               atlas_buf_cstr(&diff));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&findings), "NORMATIVE") != NULL,
               "expected a NORMATIVE finding; findings=\n%s", atlas_buf_cstr(&findings));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&findings), atlas_buf_cstr(&uid)) != NULL,
               "expected the finding to name the claim; findings=\n%s", atlas_buf_cstr(&findings));

    atlas_buf_free(&diff);
    atlas_buf_free(&findings);
    atlas_buf_free(&source_uid);
    atlas_buf_free(&uid);
    t8_env_close(&e);
}

/* Bonus, beyond the plan's four: EMPIRICAL-basis CONTRADICTED must not be
 * treated as deterministically CONTRADICTED. This is the case the T15
 * context exists for -- before `atlas_db_verify_result_latest` was widened
 * to return `basis`, nothing distinguished this row from the DETERMINISTIC
 * one in test (a), and this test fails against that narrower read. */
static void test_patch_build_empirical_basis_is_not_deterministically_contradicted(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);
    const char *repo = fx_repo(&e.fx);

    const char *l1 = "- `eta.c` fails under a specific load pattern";
    char content[256];
    (void)snprintf(content, sizeof content, "%s\n", l1);
    T_OK(fx_write(repo, "empirical.md", content, &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "eta.c", "7777777777777777777777777777777777777777777777777777777777777777",
                &err);

    atlas_buf uid = ATLAS_BUF_INIT;
    int64_t cid = 0;
    t15_claim(&e, l1, &uid, &cid, &err);
    t15_anchor(&e, atlas_buf_cstr(&uid), "eta.c", &err);
    t15_result(&e, cid, "CONTRADICTED", "EMPIRICAL", "CONTRADICTION", &err);

    atlas_buf source_uid = ATLAS_BUF_INIT;
    t15_register_source(&e, "empirical.md", &source_uid, &err);

    atlas_buf diff = ATLAS_BUF_INIT, findings = ATLAS_BUF_INIT;
    T_OK(atlas_memory_patch_build(e.db, &e.repo, fx_data_dir(&e.fx), atlas_buf_cstr(&source_uid), &diff,
                                  &findings, &err),
         &err);

    T_CHECK_MSG(diff.len == 0,
               "an EMPIRICAL-basis CONTRADICTED claim must not be proposed; diff=\n%s",
               atlas_buf_cstr(&diff));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&findings), "RETAINED") != NULL,
               "expected a RETAINED finding; findings=\n%s", atlas_buf_cstr(&findings));

    atlas_buf_free(&diff);
    atlas_buf_free(&findings);
    atlas_buf_free(&source_uid);
    atlas_buf_free(&uid);
    t8_env_close(&e);
}

/* Fix round (M5): an EMPIRICAL-basis IMPLEMENTATION conflict must still be
 * labelled IMPLEMENTATION_DRIFT, not the generic RETAINED. Before this round
 * the label required `basis == DETERMINISTIC` in addition to `conflict ==
 * IMPLEMENTATION`, so this exact row -- CONTRADICTED, EMPIRICAL,
 * IMPLEMENTATION -- lost the drift signal even though it was already, and
 * remains, excluded from deletion on any basis. Deliberately reuses
 * "mu.c"-style naming distinct from the DETERMINISTIC case in test (b)
 * (`epsilon.c`) so the two are never confused in a failure message. */
static void test_patch_build_empirical_implementation_conflict_is_labelled_drift(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);
    const char *repo = fx_repo(&e.fx);

    const char *l1 = "- `mu.c` implements the approved caching design";
    char content[256];
    (void)snprintf(content, sizeof content, "%s\n", l1);
    T_OK(fx_write(repo, "empirical-impl.md", content, &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "mu.c", "1212121212121212121212121212121212121212121212121212121212121212",
                &err);

    atlas_buf uid = ATLAS_BUF_INIT;
    int64_t cid = 0;
    t15_claim(&e, l1, &uid, &cid, &err);
    t15_anchor(&e, atlas_buf_cstr(&uid), "mu.c", &err);
    t15_result(&e, cid, "CONTRADICTED", "EMPIRICAL", "IMPLEMENTATION", &err);

    atlas_buf source_uid = ATLAS_BUF_INIT;
    t15_register_source(&e, "empirical-impl.md", &source_uid, &err);

    atlas_buf diff = ATLAS_BUF_INIT, findings = ATLAS_BUF_INIT;
    T_OK(atlas_memory_patch_build(e.db, &e.repo, fx_data_dir(&e.fx), atlas_buf_cstr(&source_uid), &diff,
                                  &findings, &err),
         &err);

    T_CHECK_MSG(diff.len == 0,
               "an EMPIRICAL-basis IMPLEMENTATION conflict must not be proposed; diff=\n%s",
               atlas_buf_cstr(&diff));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&findings), "IMPLEMENTATION_DRIFT") != NULL,
               "expected an IMPLEMENTATION_DRIFT finding even on an EMPIRICAL basis; "
               "findings=\n%s",
               atlas_buf_cstr(&findings));

    atlas_buf_free(&diff);
    atlas_buf_free(&findings);
    atlas_buf_free(&source_uid);
    atlas_buf_free(&uid);
    t8_env_close(&e);
}

/* Fix round (I2): a deterministically CONTRADICTED result whose evidence has
 * all gone stale must not be proposed for deletion. Before this round,
 * `atlas_memory_patch_build` passed `NULL` for `stale_out` and the predicate
 * had no `stale` term at all, so this exact row -- CONTRADICTED,
 * DETERMINISTIC, stale=1 -- reached a hunk indistinguishable from test (a)'s
 * still-current one. `t15_result` writes `stale` as a raw SQL literal
 * (`db_verify.c:1535`'s own column), so the fixture sets it to 1 directly
 * rather than trying to make a verdict age on its own. */
static void test_patch_build_stale_contradicted_is_a_finding_not_a_hunk(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);
    const char *repo = fx_repo(&e.fx);

    const char *l1 = "- `lambda.c` fails whenever the queue is empty";
    char content[256];
    (void)snprintf(content, sizeof content, "%s\n", l1);
    T_OK(fx_write(repo, "stale.md", content, &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "lambda.c", "1010101010101010101010101010101010101010101010101010101010101010",
                &err);

    atlas_buf uid = ATLAS_BUF_INIT;
    int64_t cid = 0;
    t15_claim(&e, l1, &uid, &cid, &err);
    t15_anchor(&e, atlas_buf_cstr(&uid), "lambda.c", &err);
    t15_result(&e, cid, "CONTRADICTED", "DETERMINISTIC", "CONTRADICTION", &err);
    char sql[256];
    (void)snprintf(sql, sizeof sql, "UPDATE verify_results SET stale = 1 WHERE claim_id = %lld;",
                  (long long)cid);
    T_OK(atlas_db_exec_sql(e.db, sql, &err), &err);

    atlas_buf source_uid = ATLAS_BUF_INIT;
    t15_register_source(&e, "stale.md", &source_uid, &err);

    atlas_buf diff = ATLAS_BUF_INIT, findings = ATLAS_BUF_INIT;
    T_OK(atlas_memory_patch_build(e.db, &e.repo, fx_data_dir(&e.fx), atlas_buf_cstr(&source_uid), &diff,
                                  &findings, &err),
         &err);

    T_CHECK_MSG(diff.len == 0, "a stale CONTRADICTED claim must not be proposed; diff=\n%s",
               atlas_buf_cstr(&diff));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&findings), "RETAINED") != NULL,
               "expected a RETAINED finding; findings=\n%s", atlas_buf_cstr(&findings));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&findings), atlas_buf_cstr(&uid)) != NULL,
               "expected the finding to name the claim; findings=\n%s", atlas_buf_cstr(&findings));

    atlas_buf_free(&diff);
    atlas_buf_free(&findings);
    atlas_buf_free(&source_uid);
    atlas_buf_free(&uid);
    t8_env_close(&e);
}

/* Fix round (R1). The re-review of this round's own I1/I2 fix found the
 * identical shape once more, one guard narrower: `det_contradicted` carried
 * all three absolutes -- DESCRIPTIVE semantics, no IMPLEMENTATION conflict,
 * not stale -- and `superseded` carried only the first two, so the `||`
 * between them let a *stale* verdict reach a hunk through the SUPERSEDED arm
 * alone, exactly the failure I2 fixed for the CONTRADICTED arm one arm over.
 * This is I2's own fixture (CONTRADICTED, DETERMINISTIC, CONTRADICTION
 * conflict, `stale` forced to 1 by raw SQL, `t15_result`'s own column) plus a
 * SUPERSEDED diff row for the same claim: before this round's
 * `patch_may_delete`, `superseded`'s three terms (diff kind, semantics,
 * conflict) were all satisfied on their own and this line was deleted
 * regardless of `det_contradicted`'s correct refusal. Against `8d19468` (the
 * first fix round, before `patch_may_delete`) this test fails: `diff.len`
 * comes back non-zero. */
static void test_patch_build_superseded_stale_is_a_finding_not_a_hunk(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);
    const char *repo = fx_repo(&e.fx);

    const char *l1 = "- `omega.c` fails whenever the cache is cold";
    char content[256];
    (void)snprintf(content, sizeof content, "%s\n", l1);
    T_OK(fx_write(repo, "stale-superseded.md", content, &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "omega.c", "1313131313131313131313131313131313131313131313131313131313131313",
                &err);

    atlas_buf uid = ATLAS_BUF_INIT;
    int64_t cid = 0;
    t15_claim(&e, l1, &uid, &cid, &err);
    t15_anchor(&e, atlas_buf_cstr(&uid), "omega.c", &err);
    t15_result(&e, cid, "CONTRADICTED", "DETERMINISTIC", "CONTRADICTION", &err);
    char sql[256];
    (void)snprintf(sql, sizeof sql, "UPDATE verify_results SET stale = 1 WHERE claim_id = %lld;",
                  (long long)cid);
    T_OK(atlas_db_exec_sql(e.db, sql, &err), &err);
    int64_t gen_id = 0;
    t15_seed_generation(&e, &gen_id, &err);
    t15_diff(&e, gen_id, atlas_buf_cstr(&uid), ATLAS_MEMORY_DIFF_SUPERSEDED, &err);

    atlas_buf source_uid = ATLAS_BUF_INIT;
    t15_register_source(&e, "stale-superseded.md", &source_uid, &err);

    atlas_buf diff = ATLAS_BUF_INIT, findings = ATLAS_BUF_INIT;
    T_OK(atlas_memory_patch_build(e.db, &e.repo, fx_data_dir(&e.fx), atlas_buf_cstr(&source_uid), &diff,
                                  &findings, &err),
         &err);

    T_CHECK_MSG(diff.len == 0,
               "a stale claim must not be proposed for deletion through the SUPERSEDED arm "
               "either; diff=\n%s",
               atlas_buf_cstr(&diff));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&findings), "RETAINED") != NULL,
               "expected a RETAINED finding; findings=\n%s", atlas_buf_cstr(&findings));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&findings), atlas_buf_cstr(&uid)) != NULL,
               "expected the finding to name the claim; findings=\n%s", atlas_buf_cstr(&findings));

    atlas_buf_free(&diff);
    atlas_buf_free(&findings);
    atlas_buf_free(&source_uid);
    atlas_buf_free(&uid);
    t8_env_close(&e);
}

/* Coverage (M7's coverage half, this round). Every existing T15 test above
 * drives a REPO_FILE source (`t15_register_source`'s own class); nothing
 * exercised `atlas_memory_patch_build`'s own `*_DIR` item loop
 * (`patch.c:665-688`) -- only `atlas_memory_read_source` directly, this
 * file's own REPO_DIR section far above. Two untracked children, one
 * deterministically CONTRADICTED and one healthy: both must be processed
 * independently, each keyed by the source's own path joined with the child's
 * own name (`item_path`'s own construction), sharing one candidate budget
 * rather than each getting its own. The vocabulary question M7 raised and
 * this round again declines -- what an empty `*_DIR`/`EXTERNAL_*` listing
 * should be called -- is untouched by this test, which is about a *non-empty*
 * listing; see docs/backlog.md. */
static void test_patch_build_repo_dir_versions_each_child(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);
    const char *repo = fx_repo(&e.fx);

    T_OK(fx_write(repo, "keep.md", "unrelated tracked file\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);

    T_OK(fx_mkdir(repo, ".claude", &err), &err);
    T_OK(fx_mkdir(repo, ".claude/memories", &err), &err);
    const char *l1 = "- `nu.c` must retry the checksum before failing";
    const char *l2 = "- `xi.c` behaves correctly under load";
    char c1[256], c2[256];
    (void)snprintf(c1, sizeof c1, "%s\n", l1);
    (void)snprintf(c2, sizeof c2, "%s\n", l2);
    T_OK(fx_write(repo, ".claude/memories/a.md", c1, &err), &err);
    T_OK(fx_write(repo, ".claude/memories/b.md", c2, &err), &err);

    t8_seed_file(&e, "nu.c", "1616161616161616161616161616161616161616161616161616161616161616", &err);
    t8_seed_file(&e, "xi.c", "1717171717171717171717171717171717171717171717171717171717171717", &err);

    atlas_buf u1 = ATLAS_BUF_INIT, u2 = ATLAS_BUF_INIT;
    int64_t i1 = 0, i2 = 0;
    t15_claim(&e, l1, &u1, &i1, &err);
    t15_claim(&e, l2, &u2, &i2, &err);
    t15_anchor(&e, atlas_buf_cstr(&u1), "nu.c", &err);
    t15_anchor(&e, atlas_buf_cstr(&u2), "xi.c", &err);
    t15_result(&e, i1, "CONTRADICTED", "DETERMINISTIC", "CONTRADICTION", &err);
    t15_result(&e, i2, "VERIFIED", "DETERMINISTIC", "NONE", &err);

    const char *dir_path = ".claude/memories";
    atlas_buf source_uid = ATLAS_BUF_INIT;
    int64_t source_id = 0;
    char now[64];
    atlas_now_iso8601(now, sizeof now);
    T_OK(atlas_db_memory_source_upsert(e.db, e.repo_id, ATLAS_MEMORY_SOURCE_REPO_DIR, dir_path,
                                       strlen(dir_path), dir_path, now, &source_id, &source_uid, &err),
         &err);

    char before[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(repo, before, &err), &err);

    atlas_buf diff = ATLAS_BUF_INIT, findings = ATLAS_BUF_INIT;
    T_OK(atlas_memory_patch_build(e.db, &e.repo, fx_data_dir(&e.fx), atlas_buf_cstr(&source_uid), &diff,
                                  &findings, &err),
         &err);

    char after[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(repo, after, &err), &err);
    T_CHECK_MSG(strcmp(before, after) == 0, "a REPO_DIR patch build modified the repository");

    char removed[256];
    (void)snprintf(removed, sizeof removed, "-%s\n", l1);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&diff), removed) != NULL,
               "expected the contradicted child's line as a removal; diff=\n%s",
               atlas_buf_cstr(&diff));

    char path_hdr[256];
    (void)snprintf(path_hdr, sizeof path_hdr, "--- a/%s/a.md\n", dir_path);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&diff), path_hdr) != NULL,
               "expected the hunk's path to be the dir source joined with the child's own name; "
               "diff=\n%s",
               atlas_buf_cstr(&diff));

    T_CHECK_MSG(t15_count(atlas_buf_cstr(&findings), "RETAINED") == 1u,
               "expected the healthy child's line to be a finding, not a hunk; findings=\n%s",
               atlas_buf_cstr(&findings));

    atlas_buf_free(&diff);
    atlas_buf_free(&findings);
    atlas_buf_free(&source_uid);
    atlas_buf_free(&u1);
    atlas_buf_free(&u2);
    t8_env_close(&e);
}

/* Coverage (M7's coverage half, this round): an EXTERNAL_FILE source through
 * `atlas_memory_patch_build` itself. `t15_register_source` only ever
 * registers REPO_FILE; this exercises the `!is_repo_cls && ext_found` branch
 * (`patch.c:690-693`), which reads the source's already-stored version
 * instead of a filesystem path -- `atlas_memory_observe`'s own EXTERNAL_*
 * shape, restated as a read here. Deliberately the `ext_found == true` case
 * only: the `ext_found == false` case falls to NONE_PROPOSED, which is the
 * open vocabulary question M7 raised and this round again declines (see
 * docs/backlog.md), not a gap this test is meant to close. */
static void test_patch_build_external_file_reads_the_stored_version(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);
    const char *repo = fx_repo(&e.fx);

    T_OK(fx_write(repo, "keep.md", "unrelated tracked file\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "omicron.c", "1414141414141414141414141414141414141414141414141414141414141414",
                &err);

    const char *l1 = "- `omicron.c` must retry the handshake on failure";
    char content[256];
    (void)snprintf(content, sizeof content, "%s\n", l1);

    atlas_buf uid = ATLAS_BUF_INIT;
    int64_t cid = 0;
    t15_claim(&e, l1, &uid, &cid, &err);
    t15_anchor(&e, atlas_buf_cstr(&uid), "omicron.c", &err);
    t15_result(&e, cid, "CONTRADICTED", "DETERMINISTIC", "CONTRADICTION", &err);

    const char *path = "/home/u/ext-note.md";
    atlas_buf source_uid = ATLAS_BUF_INIT;
    int64_t source_id = 0;
    char now[64];
    atlas_now_iso8601(now, sizeof now);
    T_OK(atlas_db_memory_source_upsert(e.db, e.repo_id, ATLAS_MEMORY_SOURCE_EXTERNAL_FILE, path,
                                       strlen(path), path, now, &source_id, &source_uid, &err),
         &err);

    char sha[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_sha256_hex(content, strlen(content), sha);
    int64_t version_id = 0;
    atlas_buf version_uid = ATLAS_BUF_INIT;
    T_OK(atlas_db_memory_version_insert(e.db, source_id, "", "", sha, (int64_t)strlen(content), content,
                                        strlen(content), now, now, 0, &version_id, &version_uid, &err),
         &err);

    atlas_buf diff = ATLAS_BUF_INIT, findings = ATLAS_BUF_INIT;
    T_OK(atlas_memory_patch_build(e.db, &e.repo, fx_data_dir(&e.fx), atlas_buf_cstr(&source_uid), &diff,
                                  &findings, &err),
         &err);

    char removed[256];
    (void)snprintf(removed, sizeof removed, "-%s\n", l1);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&diff), removed) != NULL,
               "expected the contradicted line from the stored version as a removal; diff=\n%s",
               atlas_buf_cstr(&diff));

    char hdr[256];
    (void)snprintf(hdr, sizeof hdr, "--- a/%s\n", path);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&diff), hdr) != NULL,
               "expected the hunk's path to be the EXTERNAL_FILE source's own path_text; diff=\n%s",
               atlas_buf_cstr(&diff));

    atlas_buf_free(&diff);
    atlas_buf_free(&findings);
    atlas_buf_free(&version_uid);
    atlas_buf_free(&source_uid);
    atlas_buf_free(&uid);
    t8_env_close(&e);
}

/* Coverage (M7's coverage half, this round): the UNREADABLE finding
 * (`process_item`'s outcome-not-OK branch, `patch.c:359-366`, "UNREADABLE
 * path=%s outcome=%s") driven through `atlas_memory_patch_build` itself.
 * This file's own REPO_FILE section, far above, drives
 * `atlas_memory_read_source` directly to the same TOO_LARGE outcome
 * (`test_repo_file_over_the_bound_is_too_large`); nothing before this round
 * exercised the *finding* `atlas_memory_patch_build` emits for it. */
static void test_patch_build_repo_file_too_large_is_unreadable_via_patch_build(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);
    const char *repo = fx_repo(&e.fx);

    T_OK(fx_write(repo, "keep.md", "unrelated tracked file\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);

    size_t n = (size_t)ATLAS_MEMORY_MAX_SOURCE_BYTES + 1u;
    char *big = malloc(n);
    T_REQUIRE(big != NULL);
    memset(big, 'x', n);
    T_OK(fx_write_bytes(repo, "toobig.md", strlen("toobig.md"), big, n, 0644, &err), &err);
    free(big);

    atlas_buf source_uid = ATLAS_BUF_INIT;
    t15_register_source(&e, "toobig.md", &source_uid, &err);

    char before[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(repo, before, &err), &err);

    atlas_buf diff = ATLAS_BUF_INIT, findings = ATLAS_BUF_INIT;
    T_OK(atlas_memory_patch_build(e.db, &e.repo, fx_data_dir(&e.fx), atlas_buf_cstr(&source_uid), &diff,
                                  &findings, &err),
         &err);

    char after[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(repo, after, &err), &err);
    T_CHECK_MSG(strcmp(before, after) == 0,
               "a patch build over an oversized source modified the repository");

    T_CHECK_MSG(diff.len == 0, "an oversized source must propose no deletions; diff=\n%s",
               atlas_buf_cstr(&diff));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&findings), "UNREADABLE path=toobig.md outcome=TOO_LARGE\n") !=
                   NULL,
               "expected an UNREADABLE/TOO_LARGE finding; findings=\n%s", atlas_buf_cstr(&findings));

    atlas_buf_free(&diff);
    atlas_buf_free(&findings);
    atlas_buf_free(&source_uid);
    t8_env_close(&e);
}

/* Fix round (M5), tested here for the first time (R3, this round). M5
 * widened the label precedence's *set*, not the precedence itself --
 * IMPLEMENTATION_DRIFT was already tested before NORMATIVE in the label
 * `if`/`else if` chain (`patch.c:582-588`) for a DETERMINISTIC-basis
 * IMPLEMENTATION conflict, whatever the claim's semantics: that branch never
 * checked `semantics`, so a DETERMINISTIC-basis fixture here would pass
 * identically before and after M5 and would demonstrate nothing M5 changed.
 * `basis` is EMPIRICAL here on purpose -- the exact term M5 dropped from the
 * IMPLEMENTATION_DRIFT branch's own condition -- so this is the newly
 * reachable case: before M5 the first `if` required
 * `basis == DETERMINISTIC` and failed on EMPIRICAL, falling through to
 * `else if (semantics != DESCRIPTIVE)`, which this NORMATIVE claim satisfies,
 * so the pre-M5 label was NORMATIVE; after M5 the first `if` no longer checks
 * `basis` and this row satisfies it, so the label is IMPLEMENTATION_DRIFT.
 * Display-only either way: a NORMATIVE claim is excluded from a hunk by
 * `semantics` on both arms regardless of which label the else-branch prints
 * (this claim's `diff.len == 0` below is unaffected by which label wins). */
static void test_patch_build_normative_implementation_conflict_labels_drift(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);
    const char *repo = fx_repo(&e.fx);

    const char *l1 = "- `pi.c` must always validate its input before use";
    char content[256];
    (void)snprintf(content, sizeof content, "%s\n", l1);
    T_OK(fx_write(repo, "normative-impl.md", content, &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "pi.c", "1515151515151515151515151515151515151515151515151515151515151515",
                &err);

    atlas_buf uid = ATLAS_BUF_INIT;
    int64_t cid = 0;
    t15_claim_normative(&e, l1, &uid, &cid, &err);
    t15_anchor(&e, atlas_buf_cstr(&uid), "pi.c", &err);
    t15_result(&e, cid, "CONTRADICTED", "EMPIRICAL", "IMPLEMENTATION", &err);

    atlas_buf source_uid = ATLAS_BUF_INIT;
    t15_register_source(&e, "normative-impl.md", &source_uid, &err);

    atlas_buf diff = ATLAS_BUF_INIT, findings = ATLAS_BUF_INIT;
    T_OK(atlas_memory_patch_build(e.db, &e.repo, fx_data_dir(&e.fx), atlas_buf_cstr(&source_uid), &diff,
                                  &findings, &err),
         &err);

    T_CHECK_MSG(diff.len == 0,
               "a NORMATIVE claim with an IMPLEMENTATION conflict must not be proposed; diff=\n%s",
               atlas_buf_cstr(&diff));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&findings), "IMPLEMENTATION_DRIFT") != NULL,
               "expected IMPLEMENTATION_DRIFT to win the label precedence over NORMATIVE; "
               "findings=\n%s",
               atlas_buf_cstr(&findings));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&findings), "NORMATIVE") == NULL,
               "NORMATIVE must not appear once IMPLEMENTATION_DRIFT has already won; findings=\n%s",
               atlas_buf_cstr(&findings));

    atlas_buf_free(&diff);
    atlas_buf_free(&findings);
    atlas_buf_free(&source_uid);
    atlas_buf_free(&uid);
    t8_env_close(&e);
}

static const atlas_test TESTS[] = {
    {"REPO_FILE reads tracked bytes", test_repo_file_reads_tracked_bytes},
    {"REPO_FILE missing path is ABSENT", test_repo_file_missing_path_is_absent},
    {"REPO_FILE over the bound is TOO_LARGE", test_repo_file_over_the_bound_is_too_large},
    {"REPO_FILE with no mirror reports NO_MIRROR", test_repo_file_no_mirror_reports_the_outcome},
    {"REPO_DIR gitignored is NOT_MIRRORED", test_repo_dir_gitignored_is_not_mirrored},
    {"REPO_DIR mixed tracked/gitignored children", test_repo_dir_mixed_tracked_and_gitignored_children},
    {"REPO_DIR empty mirror listing reports from_mirror", test_repo_dir_empty_mirror_listing_reports_from_mirror},
    {"REPO_DIR finds untracked .md", test_repo_dir_finds_untracked_md},
    {"REPO_DIR skips and sorts", test_repo_dir_skips_and_sorts},
    {"REPO_DIR requires from_mirror_out", test_repo_dir_requires_from_mirror_out},
    {"EXTERNAL_FILE via read_source is NOT_OURS",
     test_external_file_via_read_source_is_not_ours},
    {"read_external refuses a symlink", test_read_external_refuses_a_symlink},
    /* T8: the pass. */
    {"pass collapses duplicate source text into one independent group",
     test_pass_collapses_duplicate_source_text},
    {"pass retains three provenances across versions",
     test_pass_retains_provenance_across_versions},
    {"pass over unchanged bytes is a no-op", test_pass_over_unchanged_bytes_is_a_no_op},
    {"prose-only candidate is unanchored", test_prose_only_candidate_is_unanchored},
    {"an unindexed source path is unanchored, not refused",
     test_unindexed_source_path_is_unanchored_not_refused},
    {"stored DOCUMENT actor is SELF_DECLARED at prior 350",
     test_document_actor_is_self_declared_350},
    {"Debt 1: the extractor version lands in the evidence actor",
     test_extractor_version_lands_in_the_evidence_actor},
    {"observe runs without a transaction", test_observe_runs_without_a_transaction},
    {"Debt 2: apply duration at the compiled worst case, measured (all-refusing)",
     test_cost_debt_apply_duration_at_compiled_worst_case},
    {"I3: Debt 2, the all-resolving case at the compiled worst case, measured",
     test_cost_debt_all_resolving_case_at_compiled_worst_case},
    {"EXTERNAL_* source reads the stored version, not the disk",
     test_external_source_reads_the_stored_version_not_the_disk},
    {"REPO_DIR source versions each child independently",
     test_repo_dir_source_versions_each_child_independently},
    {"C1: two identical REPO_DIR children land as one unanchored row",
     test_two_identical_children_land_as_one_unanchored_row},
    {"I1: a dead scanner is distinguishable from an unchanged repository",
     test_read_obstacle_is_distinguishable_from_unchanged},
    {"I4: one source's obstacle does not discard the other sources",
     test_one_source_obstacle_does_not_discard_the_rest},
    {"New-C1: an outer-transaction-ending fault abandons the pass",
     test_outer_transaction_ending_fault_abandons_the_pass},
    /* T9: generations, the semantic diff, and drift. */
    {"item 2 (SOURCE_REVISION): untouched claims stay byte-identical",
     test_source_revision_leaves_untouched_claims_byte_identical},
    {"item 2 (COMMIT): only the anchored claim is impacted",
     test_commit_impacts_only_its_anchored_claim},
    {"C1: two propositions sharing one anchor both re-mint cleanly",
     test_two_propositions_sharing_one_anchor_both_remint_cleanly},
    {"New Important 2: a SOURCE_REVISION re-verification is SUPPORTED, not IMPACTED",
     test_source_revision_reverification_is_supported_not_impacted},
    {"T9 fix-round-3 Minor: IMPACTED is positional, not pass-wide",
     test_impacted_is_positional_not_pass_wide},
    {"C2: a commit behind a SYMBOL verifier still produces a diff row",
     test_commit_rewrites_file_behind_a_symbol_verifier},
    {"New Important 3: touched_bound_hit is reported on the pass result",
     test_touched_bound_hit_is_reported_on_the_pass_result},
    {"I7: multi-anchor reminting does not grow memory_claim_anchors",
     test_multi_anchor_reminting_does_not_grow_memory_claim_anchors},
    {"a decision revision's approval is the pass's cause",
     test_decision_revision_approval_is_the_cause},
    {"item 3: drift is an IMPLEMENTATION conflict and leaves the decision untouched",
     test_drift_conflict_leaves_the_decision_untouched},
    {"the vanish sweep's EVALUATE cost is bounded across repeated passes",
     test_vanish_sweep_evaluate_cost_is_bounded_across_repeated_passes},
    {"New Important 1: UNDETERMINED reopens once coverage completes",
     test_undetermined_reopens_once_coverage_completes},
    {"item 4: git cat-file on the stored blob_oid matches content_sha256",
     test_rebuild_from_git_blob_matches_stored_hash},
    {"plan_for answers the right cause before the pass runs",
     test_plan_for_answers_the_right_cause_before_the_pass},
    {"C3: dir_hash_mismatch matches read.c's own case rule and pattern-escapes its path",
     test_dir_hash_mismatch_matches_read_c_exactly},
    {"C3: dir_hash_mismatch escapes a literal '%' in its own path",
     test_dir_hash_mismatch_escapes_a_literal_percent_in_its_own_path},
    {"C3 fix-round-3/4: dir_hash_mismatch excludes the five unreadable doors",
     test_dir_hash_mismatch_excludes_the_five_unreadable_doors},
    /* T11: memory.put, memory.status, memory.reconcile. */
    {"the three memory RPC names are wired into the operator method table",
     test_memory_operator_methods_are_wired},
    {"memory.put stores a version whose content_sha256 matches the bytes",
     test_put_stores_a_version_with_matching_hash},
    {"memory.put against an unregistered source names nothing",
     test_put_unregistered_source_names_nothing},
    {"memory.put refuses content over the bound before queueing anything",
     test_put_over_bound_is_refused_before_queueing},
    {"memory.put refuses a REPO_* source", test_put_refuses_a_repo_class_source},
    {"memory.put's rel_path must name one *_DIR child ending in .md",
     test_put_dir_source_needs_a_dot_md_child},
    {"memory.put on unchanged content does not create a new version",
     test_put_same_content_twice_is_not_a_new_version},
    /* T11 fix round: memory.put/status/reconcile refuse a non-operator peer
     * over a real socket, and reconciled memory survives a daemon restart, are
     * both in test_memory_reconcile_live.c now -- item 6. */
    {"Important 1: memory.status's metadata-only read never fetches content",
     test_version_latest_meta_omits_content},
    {"Important 2: memory.status reports a failed read, not an absent version",
     test_status_reports_a_failed_read_rather_than_an_absent_version},
    {"Minor 2: an emptied put stores a zero-length blob, not NULL",
     test_put_empty_content_is_a_zero_length_blob_not_null},
    {"Minor 4: memory.put refuses an oversized observed_at before queueing",
     test_put_refuses_an_oversized_observed_at_before_queueing},
    /* T15: the proposed patch. */
    {"(a) the one deterministically CONTRADICTED line is the only deletion proposed",
     test_patch_build_proposes_exactly_the_contradicted_line},
    {"(b) an IMPLEMENTATION conflict is a finding, never a hunk",
     test_patch_build_implementation_conflict_is_a_finding_not_a_hunk},
    {"(c) an unanchored line produces neither a hunk nor a finding",
     test_patch_build_unanchored_line_produces_neither},
    {"(d) nothing to propose returns an empty diff and says so in findings",
     test_patch_build_nothing_to_propose_says_so_in_findings},
    {"bonus: a SUPERSEDED diff kind proposes deletion on its own",
     test_patch_build_superseded_diff_kind_proposes_deletion},
    {"fix round I1: a SUPERSEDED claim with an IMPLEMENTATION conflict is a finding, never a hunk",
     test_patch_build_superseded_implementation_conflict_is_a_finding_not_a_hunk},
    {"fix round I1: a SUPERSEDED claim with NORMATIVE semantics is a finding, never a hunk",
     test_patch_build_superseded_normative_is_a_finding_not_a_hunk},
    {"bonus: EMPIRICAL basis is not deterministically CONTRADICTED",
     test_patch_build_empirical_basis_is_not_deterministically_contradicted},
    {"fix round M5: an EMPIRICAL-basis IMPLEMENTATION conflict is still labelled IMPLEMENTATION_DRIFT",
     test_patch_build_empirical_implementation_conflict_is_labelled_drift},
    {"fix round I2: a stale CONTRADICTED claim is a finding, never a hunk",
     test_patch_build_stale_contradicted_is_a_finding_not_a_hunk},
    {"fix round R1: a stale claim is a finding, never a hunk, through the SUPERSEDED arm too",
     test_patch_build_superseded_stale_is_a_finding_not_a_hunk},
    {"coverage (M7): a REPO_DIR source versions each child independently through patch_build",
     test_patch_build_repo_dir_versions_each_child},
    {"coverage (M7): an EXTERNAL_FILE source reads its stored version through patch_build",
     test_patch_build_external_file_reads_the_stored_version},
    {"coverage (M7): a TOO_LARGE REPO_FILE is an UNREADABLE finding through patch_build",
     test_patch_build_repo_file_too_large_is_unreadable_via_patch_build},
    {"fix round R3: an EMPIRICAL-basis IMPLEMENTATION conflict labels IMPLEMENTATION_DRIFT "
     "over NORMATIVE (the case M5 widened)",
     test_patch_build_normative_implementation_conflict_labels_drift},
};

ATLAS_TEST_MAIN("memory_reconcile", TESTS)
