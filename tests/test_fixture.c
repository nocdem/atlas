/* Atlas - the test fixture's own hygiene.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The suite asserts a great deal about what Atlas leaves behind. This asserts
 * the same thing about the suite itself, because the fixture had been leaving
 * one empty directory under TMPDIR per test process — a leak that no test could
 * see, since the directory is only released when the process that created it
 * ends and a test runs inside that process.
 *
 * So the observation has to be made from outside a process that used the
 * fixture, which is what these tests do: fork a child, let it use the fixture
 * the way a real suite does, wait for it to exit, and then look at what is left.
 * The child is given a private TMPDIR inside the parent's own fixture, so the
 * assertion is about that child and cannot be disturbed by the other suites
 * ctest runs in parallel.
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "atlas_test.h"
#include "support/fixture.h"

/* Sorted names directly under `dir`, joined by '\n'. Small by construction: the
 * directory being listed is a fixture the test just created. */
static atlas_status list_dir(const char *dir, atlas_buf *out, atlas_err *err) {
    char *names[64];
    size_t n = 0;
    DIR *d = opendir(dir);
    if (d == NULL) {
        return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "cannot list the directory");
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        if (n >= sizeof(names) / sizeof(names[0])) {
            (void)closedir(d);
            for (size_t i = 0; i < n; i++) {
                free(names[i]);
            }
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "too many entries to list");
        }
        names[n] = strdup(e->d_name);
        if (names[n] == NULL) {
            (void)closedir(d);
            for (size_t i = 0; i < n; i++) {
                free(names[i]);
            }
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
        }
        n++;
    }
    (void)closedir(d);

    for (size_t i = 0; i + 1 < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            if (strcmp(names[j], names[i]) < 0) {
                char *t = names[i];
                names[i] = names[j];
                names[j] = t;
            }
        }
    }
    atlas_status st = ATLAS_OK;
    for (size_t i = 0; i < n && st == ATLAS_OK; i++) {
        st = atlas_buf_appendf(out, err, "%s\n", names[i]);
    }
    for (size_t i = 0; i < n; i++) {
        free(names[i]);
    }
    return st;
}

/* What a child does with the fixture before it exits. */
typedef enum child_mode {
    /* Runs a CLI command, which is what creates the private runtime directory,
     * and closes its fixture properly. */
    CHILD_SUCCESS = 0,
    /* Same, but never reaches its fx_close(): the shape of a test that failed an
     * assertion and abandoned the rest of its body. */
    CHILD_ABANDONED,
    /* Uses only the CLI helper, so the runtime directory is the only thing it
     * ever creates. This is the exact leak this suite exists for. */
    CHILD_CLI_ONLY
} child_mode;

/* Runs one child against `tmpdir` and returns its exit status. */
static int run_child(const char *tmpdir, child_mode mode) {
    (void)fflush(NULL); /* so the child does not re-emit the parent's buffers */
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        /* The child owns nothing its parent created. */
        if (setenv("TMPDIR", tmpdir, 1) != 0) {
            _exit(70);
        }
        fx_reset_after_fork();

        atlas_err err;
        atlas_err_init(&err);
        fixture fx;
        bool have_fixture = false;
        if (mode != CHILD_CLI_ONLY) {
            if (fx_open(&fx, &err) != ATLAS_OK) {
                _exit(71);
            }
            have_fixture = true;
        }

        const char *args[] = {"--version"};
        atlas_buf out = ATLAS_BUF_INIT;
        atlas_buf errout = ATLAS_BUF_INIT;
        int code = -1;
        atlas_status st = fx_atlas(args, 1, &out, &errout, &code, &err);
        atlas_buf_free(&out);
        atlas_buf_free(&errout);
        if (st != ATLAS_OK || code != 0) {
            _exit(72);
        }

        if (have_fixture && mode == CHILD_SUCCESS) {
            fx_close(&fx);
        }
        /* exit(), not _exit(): the whole point is that the registered handler
         * runs. An abandoned child still gets here, because a test binary whose
         * body was abandoned still returns through main(). */
        exit(mode == CHILD_ABANDONED ? 1 : 0);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -2;
}

/* Gives a child a private TMPDIR, runs it, and asserts the directory is exactly
 * as empty afterwards as it was before. */
static void assert_child_leaves_nothing(child_mode mode, const char *what) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_OK(fx_open(&fx, &err), &err);

    atlas_buf tmpdir = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&tmpdir, &err, "%s/childtmp", fx_data_dir(&fx)), &err);
    T_REQUIRE(mkdir(atlas_buf_cstr(&tmpdir), S_IRWXU) == 0);

    atlas_buf before = ATLAS_BUF_INIT;
    T_OK(list_dir(atlas_buf_cstr(&tmpdir), &before, &err), &err);
    T_CHECK_MSG(before.len == 0, "%s: the private TMPDIR did not start empty: %s", what,
                atlas_buf_cstr(&before));

    int code = run_child(atlas_buf_cstr(&tmpdir), mode);
    T_CHECK_MSG(code == (mode == CHILD_ABANDONED ? 1 : 0), "%s: child exited %d", what, code);

    atlas_buf after = ATLAS_BUF_INIT;
    T_CHECK(list_dir(atlas_buf_cstr(&tmpdir), &after, &err) == ATLAS_OK);
    T_CHECK_MSG(after.len == 0, "%s: the child left %s behind under its TMPDIR", what,
                atlas_buf_cstr(&after));

    atlas_buf_free(&before);
    atlas_buf_free(&after);
    atlas_buf_free(&tmpdir);
    fx_close(&fx);
}

static void test_a_process_that_only_ran_the_cli_leaves_no_temporary_directory(void) {
    /* The regression itself. Before the fix this left one `atlas-rt.XXXXXX`. */
    assert_child_leaves_nothing(CHILD_CLI_ONLY, "cli only");
}

static void test_a_process_that_closed_its_fixture_leaves_no_temporary_directory(void) {
    assert_child_leaves_nothing(CHILD_SUCCESS, "success path");
}

static void test_a_process_that_abandoned_its_fixture_leaves_no_temporary_directory(void) {
    assert_child_leaves_nothing(CHILD_ABANDONED, "failure path");
}

static void test_a_forked_child_does_not_release_its_parents_temporary_tree(void) {
    /* The exit handler is inherited across fork(). If it were not guarded by the
     * pid that armed it, the children above would have deleted this process's
     * fixtures on their way out — and the suite would have started failing in
     * whichever test ran next, for reasons pointing nowhere near here. */
    atlas_err err;
    atlas_err_init(&err);
    fixture parent_fx;
    T_OK(fx_open(&parent_fx, &err), &err);
    T_OK(fx_write(fx_repo(&parent_fx), "kept.txt", "still here\n", &err), &err);

    atlas_buf tmpdir = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&tmpdir, &err, "%s/childtmp", fx_data_dir(&parent_fx)), &err);
    T_REQUIRE(mkdir(atlas_buf_cstr(&tmpdir), S_IRWXU) == 0);

    T_CHECK(run_child(atlas_buf_cstr(&tmpdir), CHILD_SUCCESS) == 0);
    T_CHECK(run_child(atlas_buf_cstr(&tmpdir), CHILD_ABANDONED) == 1);

    struct stat sb;
    T_CHECK_MSG(stat(fx_repo(&parent_fx), &sb) == 0 && S_ISDIR(sb.st_mode),
                "a child removed the parent's repository");
    atlas_buf kept = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&kept, &err, "%s/kept.txt", fx_repo(&parent_fx)), &err);
    T_CHECK_MSG(stat(atlas_buf_cstr(&kept), &sb) == 0, "a child removed the parent's files");

    atlas_buf_free(&kept);
    atlas_buf_free(&tmpdir);
    fx_close(&parent_fx);
}

static const atlas_test TESTS[] = {
    {"cli-only process leaves no temporary directory",
     test_a_process_that_only_ran_the_cli_leaves_no_temporary_directory},
    {"closed fixture leaves no temporary directory",
     test_a_process_that_closed_its_fixture_leaves_no_temporary_directory},
    {"abandoned fixture leaves no temporary directory",
     test_a_process_that_abandoned_its_fixture_leaves_no_temporary_directory},
    {"a forked child does not release its parent's tree",
     test_a_forked_child_does_not_release_its_parents_temporary_tree},
};

ATLAS_TEST_MAIN("fixture", TESTS)
