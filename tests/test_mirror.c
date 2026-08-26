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
    T_OK(atlas_mirror_put(root, REL, strlen(REL), true, "hello", 5u, &err), &err);

    atlas_buf got = ATLAS_BUF_INIT;
    T_CHECK(mirror_read(fx_data_dir(&fx), 7, REL, &got));
    T_CHECK_MSG(got.len == 5u && memcmp(got.data, "hello", 5u) == 0, "round trip lost bytes");

    /* Two chunks concatenate. */
    T_OK(atlas_mirror_put(root, REL, strlen(REL), false, " there", 6u, &err), &err);
    T_CHECK(mirror_read(fx_data_dir(&fx), 7, REL, &got));
    T_CHECK_MSG(got.len == 11u && memcmp(got.data, "hello there", 11u) == 0,
                "append did not concatenate");

    /* A second start replaces: a rescanned file must not accumulate. */
    T_OK(atlas_mirror_put(root, REL, strlen(REL), true, "new", 3u, &err), &err);
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
        T_CHECK_MSG(atlas_mirror_put(root, BAD[i], strlen(BAD[i]), true, "x", 1u, &e) != ATLAS_OK,
                    "accepted an unsafe path: \"%s\"", BAD[i]);
    }
    /* An embedded NUL, which strlen would not see. */
    {
        atlas_err e;
        atlas_err_init(&e);
        static const char NUL_PATH[] = "a\0b";
        T_CHECK(atlas_mirror_put(root, NUL_PATH, sizeof NUL_PATH - 1u, true, "x", 1u, &e) !=
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
    T_CHECK_MSG(atlas_mirror_put(root, REL, strlen(REL), true, "x", 1u, &e) != ATLAS_OK,
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
    T_CHECK(atlas_mirror_put(root, REL, strlen(REL), false, "x", 1u, &e) != ATLAS_OK);

    atlas_buf probe = ATLAS_BUF_INIT;
    T_CHECK_MSG(!mirror_read(fx_data_dir(&fx), 7, REL, &probe), "an append created a file");
    atlas_buf_free(&probe);

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
};

ATLAS_TEST_MAIN("mirror", TESTS)
