/* Atlas - subprocess safety tests.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Covers required tests 26 (git command timeout) and 27 (output bound
 * enforcement), plus the invariants that make those bounds meaningful: no shell,
 * no inherited environment, and no surviving children.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas_test.h"

static bool find_tool(const char *name, atlas_buf *out) {
    atlas_err err;
    atlas_err_init(&err);
    if (atlas_proc_which(name, getenv("PATH"), out, &err) != ATLAS_OK) {
        atlas_test_note("%s is not installed; skipping this test", name);
        return false;
    }
    return true;
}

static void test_which(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf exe = ATLAS_BUF_INIT;
    T_OK(atlas_proc_which("git", getenv("PATH"), &exe, &err), &err);
    T_CHECK(atlas_buf_cstr(&exe)[0] == '/');

    /* An absolute path is accepted as-is when executable. */
    atlas_buf same = ATLAS_BUF_INIT;
    T_OK(atlas_proc_which(atlas_buf_cstr(&exe), NULL, &same, &err), &err);
    T_EQ_STR(atlas_buf_cstr(&same), atlas_buf_cstr(&exe));

    atlas_buf missing = ATLAS_BUF_INIT;
    T_FAILS_WITH(atlas_proc_which("atlas-no-such-program-xyz", getenv("PATH"), &missing, &err),
                 ATLAS_ERR_CONFIG, &err);
    /* A relative PATH element must never be searched. */
    atlas_buf rel = ATLAS_BUF_INIT;
    T_FAILS_WITH(atlas_proc_which("git", ":.:relative/bin", &rel, &err), ATLAS_ERR_CONFIG, &err);

    atlas_buf_free(&exe);
    atlas_buf_free(&same);
    atlas_buf_free(&missing);
    atlas_buf_free(&rel);
}

static void test_requires_absolute_argv0(void) {
    atlas_err err;
    atlas_err_init(&err);
    const char *argv[] = {"git", "--version", NULL};
    atlas_proc_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.argv = argv;
    /* PATH resolution lives in one auditable place, so a bare name is refused. */
    T_FAILS_WITH(atlas_proc_run(&opts, NULL, NULL, NULL, NULL, &err), ATLAS_ERR_INTERNAL, &err);
}

static void test_no_shell_interpretation(void) {
    atlas_buf echo = ATLAS_BUF_INIT;
    if (!find_tool("echo", &echo)) {
        atlas_buf_free(&echo);
        return;
    }
    atlas_err err;
    atlas_err_init(&err);

    /* Shell metacharacters must arrive as literal bytes: there is no shell to
     * expand them. */
    const char *payload = "$(id) `id` ; rm -rf / && echo pwned | tee /dev/null > x";
    const char *argv[] = {atlas_buf_cstr(&echo), payload, NULL};
    atlas_proc_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.argv = argv;
    opts.timeout_ms = 10000;

    atlas_buf out = ATLAS_BUF_INIT;
    atlas_proc_result res;
    T_OK(atlas_proc_run(&opts, atlas_proc_sink_buf, &out, NULL, &res, &err), &err);
    T_EQ_INT(res.exit_code, 0);
    /* echo appends one newline; the payload is otherwise untouched. */
    T_CHECK(out.len == strlen(payload) + 1u);
    T_CHECK(memcmp(out.data, payload, strlen(payload)) == 0);

    atlas_buf_free(&out);
    atlas_buf_free(&echo);
}

static void test_environment_is_explicit(void) {
    atlas_buf env_tool = ATLAS_BUF_INIT;
    if (!find_tool("env", &env_tool)) {
        atlas_buf_free(&env_tool);
        return;
    }
    atlas_err err;
    atlas_err_init(&err);

    /* Set a variable the child must not see: the parent environment is never
     * inherited implicitly. */
    T_REQUIRE(setenv("ATLAS_TEST_LEAK_CANARY", "leaked", 1) == 0);

    const char *argv[] = {atlas_buf_cstr(&env_tool), NULL};
    atlas_proc_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.argv = argv;
    opts.timeout_ms = 10000;

    atlas_buf out = ATLAS_BUF_INIT;
    atlas_proc_result res;
    T_OK(atlas_proc_run(&opts, atlas_proc_sink_buf, &out, NULL, &res, &err), &err);
    T_EQ_INT(res.exit_code, 0);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "ATLAS_TEST_LEAK_CANARY") == NULL,
                "the child inherited the parent environment: %s", atlas_buf_cstr(&out));
    T_EQ_INT(out.len, 0);

    /* An explicit environment is delivered exactly. */
    atlas_buf_reset(&out);
    const char *child_env[] = {"ATLAS_ONLY=yes", NULL};
    opts.env = child_env;
    T_OK(atlas_proc_run(&opts, atlas_proc_sink_buf, &out, NULL, &res, &err), &err);
    T_EQ_STR(atlas_buf_cstr(&out), "ATLAS_ONLY=yes\n");

    (void)unsetenv("ATLAS_TEST_LEAK_CANARY");
    atlas_buf_free(&out);
    atlas_buf_free(&env_tool);
}

/* Required test 26: a command that will not finish is terminated. */
static void test_timeout(void) {
    atlas_buf sleep_tool = ATLAS_BUF_INIT;
    if (!find_tool("sleep", &sleep_tool)) {
        atlas_buf_free(&sleep_tool);
        return;
    }
    atlas_err err;
    atlas_err_init(&err);

    const char *argv[] = {atlas_buf_cstr(&sleep_tool), "30", NULL};
    atlas_proc_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.argv = argv;
    opts.timeout_ms = 300;

    atlas_proc_result res;
    atlas_status st = atlas_proc_run(&opts, NULL, NULL, NULL, &res, &err);
    T_CHECK_MSG(st == ATLAS_ERR_GIT, "expected a timeout failure, got %s", atlas_status_name(st));
    T_CHECK(res.timed_out);
    T_CHECK_MSG(res.term_signal != 0, "the child should have been signalled, exit=%d",
                res.exit_code);
    T_CHECK(strstr(atlas_err_msg(&err), "timed out") != NULL);
    atlas_buf_free(&sleep_tool);
}

/* Required test 27: unbounded output cannot exhaust memory. */
static void test_output_bound(void) {
    atlas_buf yes_tool = ATLAS_BUF_INIT;
    if (!find_tool("yes", &yes_tool)) {
        atlas_buf_free(&yes_tool);
        return;
    }
    atlas_err err;
    atlas_err_init(&err);

    const char *argv[] = {atlas_buf_cstr(&yes_tool), "atlas", NULL};
    atlas_proc_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.argv = argv;
    opts.timeout_ms = 20000;
    opts.max_stdout = 64u * 1024u;

    atlas_buf out = ATLAS_BUF_INIT;
    atlas_proc_result res;
    atlas_status st = atlas_proc_run(&opts, atlas_proc_sink_buf, &out, NULL, &res, &err);
    T_CHECK_MSG(st == ATLAS_ERR_GIT, "expected the output limit to fail the run, got %s",
                atlas_status_name(st));
    T_CHECK(res.stdout_truncated);
    T_CHECK(!res.timed_out);
    T_CHECK(strstr(atlas_err_msg(&err), "limit") != NULL);
    /* The sink saw at most one read chunk beyond the ceiling, never unbounded
     * growth. */
    T_CHECK_MSG(out.len < opts.max_stdout + (128u * 1024u), "captured %zu bytes", out.len);
    atlas_buf_free(&out);
    atlas_buf_free(&yes_tool);
}

static void test_nonzero_exit_is_reported_not_thrown(void) {
    atlas_buf false_tool = ATLAS_BUF_INIT;
    if (!find_tool("false", &false_tool)) {
        atlas_buf_free(&false_tool);
        return;
    }
    atlas_err err;
    atlas_err_init(&err);
    const char *argv[] = {atlas_buf_cstr(&false_tool), NULL};
    atlas_proc_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.argv = argv;
    opts.timeout_ms = 10000;
    atlas_proc_result res;
    /* A non-zero child status is data for the caller, not a run failure. */
    T_OK(atlas_proc_run(&opts, NULL, NULL, NULL, &res, &err), &err);
    T_EQ_INT(res.exit_code, 1);
    T_CHECK(!res.timed_out);
    atlas_buf_free(&false_tool);
}

static void test_missing_executable_reports_clearly(void) {
    atlas_err err;
    atlas_err_init(&err);
    const char *argv[] = {"/nonexistent/atlas-not-here", NULL};
    atlas_proc_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.argv = argv;
    opts.timeout_ms = 5000;
    T_FAILS_WITH(atlas_proc_run(&opts, NULL, NULL, NULL, NULL, &err), ATLAS_ERR_GIT, &err);
    T_CHECK(strstr(atlas_err_msg(&err), "cannot execute") != NULL);
}

static void test_stderr_is_bounded(void) {
    atlas_buf exe = ATLAS_BUF_INIT;
    if (!find_tool("git", &exe)) {
        atlas_buf_free(&exe);
        return;
    }
    atlas_err err;
    atlas_err_init(&err);
    /* An invalid option makes git write to stderr and exit non-zero. */
    const char *argv[] = {atlas_buf_cstr(&exe), "--atlas-no-such-option", NULL};
    atlas_proc_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.argv = argv;
    opts.timeout_ms = 10000;
    opts.max_stderr = 16u;

    atlas_buf errbuf = ATLAS_BUF_INIT;
    atlas_proc_result res;
    T_OK(atlas_proc_run(&opts, NULL, NULL, &errbuf, &res, &err), &err);
    T_CHECK(res.exit_code != 0);
    T_CHECK_MSG(errbuf.len <= 16u, "captured %zu stderr bytes despite a 16 byte ceiling",
                errbuf.len);
    atlas_buf_free(&errbuf);
    atlas_buf_free(&exe);
}

static const atlas_test TESTS[] = {
    {"PATH resolution", test_which},
    {"argv[0] must be absolute", test_requires_absolute_argv0},
    {"no shell interpretation of arguments", test_no_shell_interpretation},
    {"child environment is explicit", test_environment_is_explicit},
    {"command timeout terminates the child", test_timeout},
    {"stdout bound is enforced", test_output_bound},
    {"non-zero exit is reported, not raised", test_nonzero_exit_is_reported_not_thrown},
    {"missing executable is reported clearly", test_missing_executable_reports_clearly},
    {"stderr capture is bounded", test_stderr_is_bounded},
};

ATLAS_TEST_MAIN("proc", TESTS)
