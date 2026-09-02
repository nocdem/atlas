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
#include "atlas/sem.h"
#include "atlas/sha256.h"
#include "atlas/syspolicy.h"
#include "atlas/verify.h"
#include "atlas/verify_ops.h"
#include "atlas/verifypolicy.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

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
 * `atlas_verify_intake_apply_in_tx`. `t8env` builds both halves and keeps
 * them consistent -- a real git repository for the observe phase to read
 * (T6's own fixture shape), and a registered repository row, a matching
 * `commits` row and `files` rows for whatever the pass needs to look up
 * (`test_memory_anchor.c`'s own fixture shape) for the apply phase. */
typedef struct t8env {
    fixture fx;
    atlas_buf db_path;
    atlas_db *db;
    int64_t repo_id;
    atlas_repo_info repo;
} t8env;

static void t8_head(t8env *e, atlas_buf *out, atlas_err *err) {
    const char *rev[] = {"rev-parse", "HEAD"};
    atlas_buf raw = ATLAS_BUF_INIT;
    int code = -1;
    T_OK(fx_git(&e->fx, fx_repo(&e->fx), rev, 2u, &code, &raw, err), err);
    T_REQUIRE(code == 0);
    size_t n = raw.len;
    while (n > 0 && (raw.data[n - 1u] == '\n' || raw.data[n - 1u] == '\r')) {
        n--;
    }
    T_OK(atlas_buf_set(out, raw.data, n, err), err);
    atlas_buf_free(&raw);
    T_REQUIRE(out->len > 0);
}

/* `test_verify_intake.c`'s own seed_file, reused: a row in `files` is what
 * lets a backtick token resolve as a PATH anchor and what lets a memory
 * source's own path pass EVIDENCE_ADD's index lookup. The hash is never
 * checked against real content anywhere T8's own pass runs (T8 never asks
 * `atlas.content_hash` to verify), so an arbitrary hex string is honest. */
static void t8_seed_file(t8env *e, const char *path, const char *hash, atlas_err *err) {
    atlas_buf sql = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&sql, err,
                           "INSERT INTO scans(repo_id, started_at, status)"
                           "  VALUES(%lld, '2026-01-01T00:00:00Z', 'ok');"
                           "INSERT INTO files(repo_id, path_raw, path_text, file_type,"
                           "  content_hash, first_seen_scan_id, last_seen_scan_id,"
                           "  first_seen_at, last_seen_at)"
                           "  VALUES(%lld, CAST('%s' AS BLOB), '%s', 'regular', '%s',"
                           "         last_insert_rowid(), last_insert_rowid(),"
                           "         '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z');",
                           (long long)e->repo_id, (long long)e->repo_id, path, path, hash),
         err);
    T_OK(atlas_db_exec_sql(e->db, atlas_buf_cstr(&sql), err), err);
    atlas_buf_free(&sql);
}

static void t8_env_open(t8env *e, atlas_err *err) {
    memset(e, 0, sizeof *e);
    atlas_repo_info_init(&e->repo);
    atlas_buf_init(&e->db_path);
    T_REQUIRE(fx_open(&e->fx, err) == ATLAS_OK);
    T_OK(fx_init_repo(&e->fx, fx_repo(&e->fx), NULL, err), err);
    T_OK(atlas_buf_appendf(&e->db_path, err, "%s/atlas.db", fx_data_dir(&e->fx)), err);
    T_OK(atlas_datadir_ensure(fx_data_dir(&e->fx), err), err);
    T_OK(atlas_db_open(atlas_buf_cstr(&e->db_path), &e->db, err), err);
    T_OK(atlas_db_migrate(e->db, err), err);

    atlas_buf common = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&common, err, "%s/.git", fx_repo(&e->fx)), err);
    atlas_repo_identity id;
    memset(&id, 0, sizeof id);
    id.root = fx_repo(&e->fx);
    id.root_len = strlen(id.root);
    id.common_dir = atlas_buf_cstr(&common);
    id.common_dir_len = common.len;
    id.git_dir = id.common_dir;
    id.git_dir_len = id.common_dir_len;
    id.object_format = "sha1";
    T_OK(atlas_db_repo_add(e->db, "proj", &id, &e->repo_id, err), err);
    atlas_buf_free(&common);
}

/* Binds the repository's own `scanned_head` (and a matching `commits` row) to
 * the real repository's current HEAD, then refetches `e->repo` so the struct
 * the pass reads matches what was just bound -- CLAIM_CREATE's `bind_commit`
 * defaults an empty `basis_commit` to `scanned_head` with no existence check,
 * but EVIDENCE_ADD re-derives its own commit from the claim's (now non-empty)
 * `basis_commit`, which *is* checked against `commits`. Called again after
 * every commit that should move what a fresh pass binds to. */
static void t8_bind_head(t8env *e, atlas_err *err) {
    atlas_buf head = ATLAS_BUF_INIT;
    t8_head(e, &head, err);
    atlas_buf sql = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&sql, err,
                           "INSERT INTO commits(repo_id, oid, parent_count) VALUES(%lld, '%s', 0);"
                           "UPDATE repositories SET scanned_head = '%s' WHERE id = %lld;",
                           (long long)e->repo_id, atlas_buf_cstr(&head), atlas_buf_cstr(&head),
                           (long long)e->repo_id),
         err);
    T_OK(atlas_db_exec_sql(e->db, atlas_buf_cstr(&sql), err), err);
    atlas_buf_free(&sql);
    atlas_buf_free(&head);

    bool found = false;
    atlas_repo_info_free(&e->repo);
    atlas_repo_info_init(&e->repo);
    T_OK(atlas_db_repo_get(e->db, "proj", &e->repo, &found, err), err);
    T_REQUIRE(found);
}

static void t8_env_close(t8env *e) {
    atlas_repo_info_free(&e->repo);
    atlas_db_close(e->db);
    atlas_buf_free(&e->db_path);
    fx_close(&e->fx);
}

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

    /* An observation, not an assertion: whether a policy is installed at
     * `ATLAS_VERIFYPOLICY_PATH` is a fact about this machine, not an
     * invariant this test's own fixture controls (a fresh checkout with no
     * A9.2 deployment step run has no file there at all), so a missing or
     * disabled one here is not this test's failure -- but a present one
     * changes what the assertions below actually establish, and that is
     * worth a caller's own eyes rather than a silent assumption either way. */
    atlas_verifypolicy real_policy;
    atlas_verifypolicy_load(&real_policy);
    atlas_test_note("verification policy at the compiled-in path: state=%d reason=%s policy_id=%s -- "
                    "ENABLED means this test's refusal is proven under a real active policy, not "
                    "merely in the absence of one",
                    (int)real_policy.state, atlas_verifypolicy_reason_name(real_policy.reason),
                    real_policy.state == ATLAS_VERIFYPOLICY_ENABLED ? real_policy.policy_id : "(n/a)");

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
    {"C2: a commit behind a SYMBOL verifier still produces a diff row",
     test_commit_rewrites_file_behind_a_symbol_verifier},
    {"a decision revision's approval is the pass's cause",
     test_decision_revision_approval_is_the_cause},
    {"item 3: drift is an IMPLEMENTATION conflict and leaves the decision untouched",
     test_drift_conflict_leaves_the_decision_untouched},
    {"the vanish sweep's EVALUATE cost is bounded across repeated passes",
     test_vanish_sweep_evaluate_cost_is_bounded_across_repeated_passes},
    {"item 4: git cat-file on the stored blob_oid matches content_sha256",
     test_rebuild_from_git_blob_matches_stored_hash},
    {"plan_for answers the right cause before the pass runs",
     test_plan_for_answers_the_right_cause_before_the_pass},
};

ATLAS_TEST_MAIN("memory_reconcile", TESTS)
