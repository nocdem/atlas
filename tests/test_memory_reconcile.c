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
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/memory.h"
#include "atlas/mirror.h"
#include "atlas_test.h"
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
};

ATLAS_TEST_MAIN("memory_reconcile", TESTS)
