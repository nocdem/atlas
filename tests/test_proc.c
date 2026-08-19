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

/* --- A11.5a-R2: idleness measured in activity, not in bytes ---------------- */

/* Counts chunks the caller decides are progress. The predicate is the *shape*
 * the driver uses — assembled lines checked against a vocabulary — reduced to
 * what this layer is responsible for: `atlas_proc_run` must ask, and must
 * believe the answer. */
typedef struct act_state {
    int64_t truthy;
    int64_t asked;
    bool answer;
} act_state;

static bool act_says(const char *chunk, size_t n, void *ud) {
    act_state *a = (act_state *)ud;
    a->asked++;
    (void)chunk;
    (void)n;
    if (a->answer) {
        a->truthy++;
    }
    return a->answer;
}

/* A worker that keeps producing progress outlives an idle bound shorter than
 * its run. This is the case that killed two real workers at 301 s: they were
 * working the whole time, and Atlas was measuring the wrong thing. */
static void test_activity_keeps_a_working_child_alive(void) {
    atlas_err err;
    atlas_err_init(&err);
    act_state a = {0, 0, true};

    /* Twelve records, 120 ms apart: about 1.4 s of work under a 400 ms idle
     * bound. Without the predicate this child is killed; with it, it finishes. */
    const char *argv[] = {ATLAS_STREAMER_BIN, "events", "12", "120", NULL};
    atlas_proc_opts o;
    memset(&o, 0, sizeof o);
    o.argv = argv;
    o.timeout_ms = 20000;
    o.idle_timeout_ms = 400;
    o.activity = act_says;
    o.activity_ud = &a;

    atlas_buf outbuf = ATLAS_BUF_INIT;
    atlas_proc_result r;
    memset(&r, 0, sizeof r);
    T_OK(atlas_proc_run(&o, atlas_proc_sink_buf, &outbuf, NULL, &r, &err), &err);

    T_CHECK_MSG(!r.idle_timed_out, "a child producing progress was killed as idle");
    T_CHECK(!r.timed_out);
    T_EQ_INT(r.exit_code, 0);
    T_CHECK_MSG(a.asked > 0, "the activity predicate was never consulted");
    atlas_buf_free(&outbuf);
}

/* The other direction, and the reason the bound still exists: output that the
 * caller does not accept as progress buys the child nothing. A worker that has
 * stopped working and is still printing is exactly what must still be killed. */
static void test_output_that_is_not_activity_does_not_keep_a_child_alive(void) {
    atlas_err err;
    atlas_err_init(&err);
    act_state a = {0, 0, false};

    const char *argv[] = {ATLAS_STREAMER_BIN, "prose", "40", "60", NULL};
    atlas_proc_opts o;
    memset(&o, 0, sizeof o);
    o.argv = argv;
    o.timeout_ms = 20000;
    o.idle_timeout_ms = 300;
    o.activity = act_says;
    o.activity_ud = &a;

    atlas_buf outbuf = ATLAS_BUF_INIT;
    atlas_proc_result r;
    memset(&r, 0, sizeof r);
    (void)atlas_proc_run(&o, atlas_proc_sink_buf, &outbuf, NULL, &r, &err);

    T_CHECK_MSG(r.idle_timed_out, "a chattering child was not stopped by the idle bound");
    T_CHECK_MSG(a.asked > 0, "the activity predicate was never consulted");
    T_EQ_INT((int)a.truthy, 0);
    atlas_buf_free(&outbuf);
}

/* With no predicate every byte still counts, which is what every git invocation
 * in this repository has always relied on. */
static void test_without_a_predicate_bytes_still_count(void) {
    atlas_err err;
    atlas_err_init(&err);
    const char *argv[] = {ATLAS_STREAMER_BIN, "prose", "10", "80", NULL};
    atlas_proc_opts o;
    memset(&o, 0, sizeof o);
    o.argv = argv;
    o.timeout_ms = 20000;
    o.idle_timeout_ms = 400;
    /* activity deliberately NULL */

    atlas_buf outbuf = ATLAS_BUF_INIT;
    atlas_proc_result r;
    memset(&r, 0, sizeof r);
    T_OK(atlas_proc_run(&o, atlas_proc_sink_buf, &outbuf, NULL, &r, &err), &err);
    T_CHECK_MSG(!r.idle_timed_out, "plain output stopped refreshing the idle bound");
    atlas_buf_free(&outbuf);
}

static const atlas_test TESTS[] = {
    {"activity_keeps_a_working_child_alive", test_activity_keeps_a_working_child_alive},
    {"output_that_is_not_activity_does_not_keep_a_child_alive",
     test_output_that_is_not_activity_does_not_keep_a_child_alive},
    {"without_a_predicate_bytes_still_count", test_without_a_predicate_bytes_still_count},
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
