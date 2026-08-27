/* Atlas - A13: the mirror, and the names it must refuse.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The paths written here come from a scanner, which reads names an untrusted
 * repository chose. So the interesting cases are not the round trip — they are
 * the refusals, and the symlink case above all: that is what the whole
 * `O_NOFOLLOW` discipline exists for, and it is what a careless rewrite would
 * quietly lose.
 */
#include "atlas_test.h"

#include "daemon/mirror.h"
#include "support/fixture.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Reads a mirrored file back through the filesystem, which is where a consumer
 * would find it. Returns false when it is absent. */
static bool mirror_read(const char *data_dir, int64_t repo, const char *rel, atlas_buf *out) {
    atlas_buf path = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    if (atlas_buf_appendf(&path, &err, "%s/mirror/%lld/%s", data_dir, (long long)repo, rel) !=
        ATLAS_OK) {
        atlas_buf_free(&path);
        return false;
    }
    FILE *f = fopen(atlas_buf_cstr(&path), "rb");
    atlas_buf_free(&path);
    if (f == NULL) {
        return false;
    }
    atlas_buf_reset(out);
    char chunk[512];
    size_t n = 0;
    while ((n = fread(chunk, 1u, sizeof chunk, f)) > 0) {
        (void)atlas_buf_append(out, chunk, n, &err);
    }
    (void)fclose(f);
    return true;
}

static void test_a_file_round_trips_and_a_second_start_replaces(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    int root = -1;
    T_OK(atlas_mirror_open_repo(fx_data_dir(&fx), 7, &root, &err), &err);
    T_REQUIRE(root >= 0);

    static const char REL[] = "a/b/c.txt";
    T_OK(atlas_mirror_put(root, REL, strlen(REL), true, false, "hello", 5u, &err), &err);

    atlas_buf got = ATLAS_BUF_INIT;
    T_CHECK(mirror_read(fx_data_dir(&fx), 7, REL, &got));
    T_CHECK_MSG(got.len == 5u && memcmp(got.data, "hello", 5u) == 0, "round trip lost bytes");

    /* Two chunks concatenate. */
    T_OK(atlas_mirror_put(root, REL, strlen(REL), false, false, " there", 6u, &err), &err);
    T_CHECK(mirror_read(fx_data_dir(&fx), 7, REL, &got));
    T_CHECK_MSG(got.len == 11u && memcmp(got.data, "hello there", 11u) == 0,
                "append did not concatenate");

    /* A second start replaces: a rescanned file must not accumulate. */
    T_OK(atlas_mirror_put(root, REL, strlen(REL), true, false, "new", 3u, &err), &err);
    T_CHECK(mirror_read(fx_data_dir(&fx), 7, REL, &got));
    T_CHECK_MSG(got.len == 3u && memcmp(got.data, "new", 3u) == 0,
                "a restarted file accumulated instead of replacing");

    atlas_buf_free(&got);
    (void)close(root);
    fx_close(&fx);
}

static void test_unsafe_names_are_refused_and_create_nothing(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    int root = -1;
    T_OK(atlas_mirror_open_repo(fx_data_dir(&fx), 7, &root, &err), &err);

    static const char *const BAD[] = {"../escape", "/absolute", "a//b", "a/./b", "a/../b", "..",
                                      ".",         ""};
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        atlas_err e;
        atlas_err_init(&e);
        T_CHECK_MSG(atlas_mirror_put(root, BAD[i], strlen(BAD[i]), true, false, "x", 1u, &e) != ATLAS_OK,
                    "accepted an unsafe path: \"%s\"", BAD[i]);
    }
    /* An embedded NUL, which strlen would not see. */
    {
        atlas_err e;
        atlas_err_init(&e);
        static const char NUL_PATH[] = "a\0b";
        T_CHECK(atlas_mirror_put(root, NUL_PATH, sizeof NUL_PATH - 1u, true, false, "x", 1u, &e) !=
                ATLAS_OK);
    }

    /* Nothing escaped: the fixture's own tree holds no stray entry. */
    atlas_buf probe = ATLAS_BUF_INIT;
    T_CHECK_MSG(!mirror_read(fx_data_dir(&fx), 7, "escape", &probe), "an unsafe path wrote a file");
    atlas_buf_free(&probe);

    (void)close(root);
    fx_close(&fx);
}

/* The case the discipline exists for. A repository that plants a symlink must
 * not redirect a mirror write to somewhere the daemon can reach and it cannot. */
static void test_a_symlinked_component_refuses(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    int root = -1;
    T_OK(atlas_mirror_open_repo(fx_data_dir(&fx), 7, &root, &err), &err);

    /* `a` is a link out of the mirror. Writing `a/x` must refuse rather than
     * land in the target. */
    T_REQUIRE(symlinkat("/tmp", root, "a") == 0);
    static const char REL[] = "a/x";
    atlas_err e;
    atlas_err_init(&e);
    T_CHECK_MSG(atlas_mirror_put(root, REL, strlen(REL), true, false, "x", 1u, &e) != ATLAS_OK,
                "a write followed a symlinked component");

    (void)close(root);
    fx_close(&fx);
}

/* A chunk for a file nobody started means the stream broke. Creating one would
 * turn a detectable failure into a silently truncated file. */
static void test_an_append_to_an_unstarted_file_refuses(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    int root = -1;
    T_OK(atlas_mirror_open_repo(fx_data_dir(&fx), 7, &root, &err), &err);

    static const char REL[] = "never/started.txt";
    atlas_err e;
    atlas_err_init(&e);
    T_CHECK(atlas_mirror_put(root, REL, strlen(REL), false, false, "x", 1u, &e) != ATLAS_OK);

    atlas_buf probe = ATLAS_BUF_INIT;
    T_CHECK_MSG(!mirror_read(fx_data_dir(&fx), 7, REL, &probe), "an append created a file");
    atlas_buf_free(&probe);

    (void)close(root);
    fx_close(&fx);
}

/* A13. A symlink is mirrored as a symlink, with the tree's link text.
 *
 * **The link text is the content.** Atlas hashes a tracked symlink's text and
 * never opens its target, so a mirror that wrote a regular file holding the
 * path — or skipped the entry — would differ from the tree in a way the daemon
 * reads as a deletion. Measured before this existed: one symlink in /opt/dna
 * was enough to leave that repository's mirror permanently incomplete.
 *
 * The target here does not exist, deliberately: nothing is required to resolve,
 * because nothing follows it. */
static void test_a_symlink_is_mirrored_as_one(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);

    int root = -1;
    T_OK(atlas_mirror_open_repo(fx_data_dir(&fx), 3, &root, &err), &err);
    T_REQUIRE(root >= 0);

    static const char TARGET[] = "../nowhere/at/all";
    T_OK(atlas_mirror_put_symlink(root, "link", 4u, TARGET, sizeof(TARGET) - 1u, &err), &err);

    char back[256];
    ssize_t n = readlinkat(root, "link", back, sizeof(back));
    T_CHECK_MSG(n == (ssize_t)(sizeof(TARGET) - 1u), "it is a symlink, not a file");
    if (n > 0) {
        back[n] = '\0';
        T_CHECK_MSG(strcmp(back, TARGET) == 0, "and it carries the tree's link text");
    }

    /* Replacing it works the same way a rescanned file does. */
    static const char OTHER[] = "elsewhere";
    T_OK(atlas_mirror_put_symlink(root, "link", 4u, OTHER, sizeof(OTHER) - 1u, &err), &err);
    n = readlinkat(root, "link", back, sizeof(back));
    if (n > 0) {
        back[n] = '\0';
        T_CHECK_MSG(strcmp(back, OTHER) == 0, "a rescan replaces the link text");
    }

    (void)close(root);
    fx_close(&fx);
}

/* A13. A **broken** symlink is mirrored like any other.
 *
 * Atlas hashes a symlink's text and never opens its target, so whether the
 * target exists has nothing to do with whether the entry belongs in the mirror.
 * Found on the live tree: one link into a directory that no longer existed was
 * counted unreadable, which left the whole repository's mirror incomplete and
 * therefore refused -- a repository kept out of the index by a dangling link in
 * a docs folder. */
static void test_a_broken_symlink_is_mirrored_too(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);

    int root = -1;
    T_OK(atlas_mirror_open_repo(fx_data_dir(&fx), 4, &root, &err), &err);
    T_REQUIRE(root >= 0);

    /* A target that does not exist and never will. */
    static const char GONE[] = "/opt/nothing-here/at/all";
    T_OK(atlas_mirror_put_symlink(root, "dangling", 8u, GONE, sizeof(GONE) - 1u, &err), &err);

    char back[256];
    ssize_t n = readlinkat(root, "dangling", back, sizeof(back));
    T_CHECK_MSG(n == (ssize_t)(sizeof(GONE) - 1u), "a broken link is still a link");
    if (n > 0) {
        back[n] = '\0';
        T_CHECK_MSG(strcmp(back, GONE) == 0, "and it carries the text the tree held");
    }

    (void)close(root);
    fx_close(&fx);
}

/* A13. The executable bit survives the mirror.
 *
 * **Git tracks exactly one mode bit**, and the mirror carries the mirrored
 * index alongside the files -- so a tree's executable file written 0600 compares
 * `100644` against an index holding `100755`, and git calls that a
 * modification. Measured on the live tree: a clean repository read as dirty
 * with 24 files changed, not one of which differed by a byte. Every one was a
 * script. */
static void test_the_executable_bit_survives(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);

    int root = -1;
    T_OK(atlas_mirror_open_repo(fx_data_dir(&fx), 5, &root, &err), &err);
    T_REQUIRE(root >= 0);

    T_OK(atlas_mirror_put(root, "script.sh", 9u, true, true, "#!/bin/sh\n", 10u, &err), &err);
    T_OK(atlas_mirror_put(root, "plain.txt", 9u, true, false, "text\n", 5u, &err), &err);

    struct stat sb;
    memset(&sb, 0, sizeof(sb));
    T_REQUIRE(fstatat(root, "script.sh", &sb, AT_SYMLINK_NOFOLLOW) == 0);
    T_CHECK_MSG((sb.st_mode & S_IXUSR) != 0, "an executable file lost its bit in the mirror");

    memset(&sb, 0, sizeof(sb));
    T_REQUIRE(fstatat(root, "plain.txt", &sb, AT_SYMLINK_NOFOLLOW) == 0);
    T_CHECK_MSG((sb.st_mode & S_IXUSR) == 0, "a plain file gained one");

    (void)close(root);
    fx_close(&fx);
}

static const atlas_test TESTS[] = {
    {"a file round-trips and a second start replaces",
     test_a_file_round_trips_and_a_second_start_replaces},
    {"unsafe names are refused and create nothing",
     test_unsafe_names_are_refused_and_create_nothing},
    {"a symlinked component refuses", test_a_symlinked_component_refuses},
    {"an append to an unstarted file refuses", test_an_append_to_an_unstarted_file_refuses},
    {"a symlink is mirrored as one", test_a_symlink_is_mirrored_as_one},
    {"a broken symlink is mirrored too", test_a_broken_symlink_is_mirrored_too},
    {"the executable bit survives", test_the_executable_bit_survives},
};

ATLAS_TEST_MAIN("mirror", TESTS)

