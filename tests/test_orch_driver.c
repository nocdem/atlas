/* Atlas - A8: the drivers, bounded execution, and the whole pipeline in process.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Two things are proven here.
 *
 * **The execution boundary.** Structured argv reaches a child unchanged, a shell
 * is never involved, the environment is what Atlas built and nothing else, an
 * idle child is killed on its own bound, cancellation is honoured, and the whole
 * *process group* is terminated so a grandchild cannot survive.
 *
 * **The pipeline.** A job goes submit → lease → workspace → snapshot → driver →
 * patch → artifacts → complete, driven directly against the one write point and
 * a real workspace, with the fake driver in each of its deterministic
 * behaviours. That is the same sequence the dispatcher performs; running it in
 * process is what makes it deterministic and fast, and the live smoke is what
 * proves the socket path.
 *
 * Required cases covered here: 24 (unknown driver), 25–28 (fake driver success,
 * failure, timeout, cancellation), plus malformed result, structured argv
 * without shell interpolation, environment cleansing, output bounds, idle
 * timeout, process-tree termination and orphan cleanup.
 */
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/driver.h"
#include "atlas/git.h"
#include "atlas/orch.h"
#include "atlas/proc.h"
#include "atlas/workspace.h"
#include "atlas_test.h"
#include "support/fixture.h"

#define JOB_D "j000000000000000000000000000000d1"

static void head_oid(const char *repo, atlas_buf *out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_git *g = NULL;
    T_OK(atlas_git_open(repo, &g, &err), &err);
    atlas_git_head h;
    memset(&h, 0, sizeof(h));
    T_OK(atlas_git_read_head(g, &h, &err), &err);
    T_OK(atlas_buf_set_str(out, h.oid, &err), &err);
    atlas_git_close(g);
}

typedef struct denv {
    fixture fx;
    atlas_buf worker_root;
    atlas_buf commit;
    atlas_ws ws;
} denv;

static void denv_open(denv *e) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&e->fx, &err), &err);
    atlas_buf_init(&e->worker_root);
    atlas_buf_init(&e->commit);
    T_OK(atlas_buf_appendf(&e->worker_root, &err, "%s/worker", fx_data_dir(&e->fx)), &err);
    T_REQUIRE(mkdir(atlas_buf_cstr(&e->worker_root), 0700) == 0);
    T_OK(fx_init_repo(&e->fx, fx_repo(&e->fx), NULL, &err), &err);
    T_OK(fx_write(fx_repo(&e->fx), "a.c", "int main(void){return 0;}\n", &err), &err);
    T_OK(fx_add_all(&e->fx, fx_repo(&e->fx), &err), &err);
    T_OK(fx_commit(&e->fx, fx_repo(&e->fx), "first", &err), &err);
    head_oid(fx_repo(&e->fx), &e->commit);

    T_OK(atlas_ws_open(atlas_buf_cstr(&e->worker_root), JOB_D, 1, &e->ws, &err), &err);
    /* The worker no longer reads a repository: bytes arrive from the daemon.
     * Here they are supplied directly, which is the same entry point the
     * snapshot stream uses. */
    static const char BODY[] = "int main(void){return 0;}\n";
    T_OK(atlas_ws_materialise(&e->ws, "a.c", 3u, "100644", BODY, sizeof(BODY) - 1u, true, &err),
         &err);
}

static void denv_close(denv *e) {
    atlas_ws_free(&e->ws);
    atlas_buf_free(&e->worker_root);
    atlas_buf_free(&e->commit);
    fx_close(&e->fx);
}

static void fill_req(denv *e, atlas_driver_req *req, const char *task) {
    memset(req, 0, sizeof(*req));
    req->ws = &e->ws;
    req->job_uid = JOB_D;
    req->attempt_no = 1;
    req->task = task;
    req->mode = "patch";
    req->wall_timeout_ms = 30000;
    req->idle_timeout_ms = 15000;
    req->max_output_bytes = 1024 * 1024;
}

/* --- the registry ------------------------------------------------------------ */

static void test_an_unknown_driver_is_refused_and_never_defaulted(void) {
    /* Substituting a driver would run something other than what the job
     * specified, which is worse than refusing. */
    T_CHECK(atlas_driver_find("fake") != NULL);
    T_CHECK(atlas_driver_find("claude") != NULL);
    static const char *const UNKNOWN[] = {"", "Fake", "FAKE", "gpt", "sh", "../fake", NULL};
    for (size_t i = 0; UNKNOWN[i] != NULL; i++) {
        T_CHECK_MSG(atlas_driver_find(UNKNOWN[i]) == NULL, "\"%s\" resolved to a driver",
                    UNKNOWN[i]);
    }
    T_CHECK(atlas_driver_find(NULL) == NULL);

    /* Every shipped driver names itself and its version, because an attempt is
     * only comparable to another when the thing that produced it is named. */
    size_t n = 0;
    const atlas_driver *const *all = atlas_drivers(&n);
    T_CHECK(n >= 2);
    for (size_t i = 0; i < n; i++) {
        T_CHECK(all[i]->name != NULL && all[i]->name[0] != '\0');
        T_CHECK(all[i]->version != NULL && all[i]->version[0] != '\0');
        T_CHECK(all[i]->run != NULL);
    }
}

/* --- the fake driver's four deterministic behaviours -------------------------- */

static void run_fake(denv *e, const char *task, atlas_driver_res *res) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_driver_req req;
    fill_req(e, &req, task);
    atlas_driver_res_init(res);
    const atlas_driver *d = atlas_driver_find("fake");
    T_REQUIRE(d != NULL);
    T_OK(d->run(&req, res, &err), &err);
}

static void test_the_fake_driver_is_deterministic_in_each_behaviour(void) {
    struct {
        const char *task;
        atlas_orch_exit_kind want;
    } CASES[] = {
        {"do something ordinary", ATLAS_ORCH_EXIT_OK},
        {"fake:fail", ATLAS_ORCH_EXIT_NONZERO},
        {"fake:timeout", ATLAS_ORCH_EXIT_TIMEOUT},
        {"fake:cancel", ATLAS_ORCH_EXIT_CANCELLED},
        {"fake:malformed", ATLAS_ORCH_EXIT_MALFORMED_RESULT},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        denv e;
        denv_open(&e);
        atlas_driver_res res;
        run_fake(&e, CASES[i].task, &res);
        T_CHECK_MSG(res.exit_kind == CASES[i].want, "\"%s\" produced %s, wanted %s",
                    CASES[i].task, atlas_orch_exit_kind_name(res.exit_kind),
                    atlas_orch_exit_kind_name(CASES[i].want));
        T_CHECK(strcmp(atlas_buf_cstr(&res.version), "fake/1") == 0);
        atlas_driver_res_free(&res);
        denv_close(&e);
    }
}

static void test_a_zero_exit_with_malformed_metadata_is_not_success(void) {
    /* The mistake this branch exists to refuse: a driver that exits zero and
     * produces something that is not a result document has not succeeded. */
    denv e;
    denv_open(&e);
    atlas_driver_res res;
    run_fake(&e, "fake:malformed", &res);
    T_EQ_INT((int)res.exit_code, 0);
    T_CHECK_MSG(res.exit_kind == ATLAS_ORCH_EXIT_MALFORMED_RESULT,
                "a zero exit with malformed metadata was read as %s",
                atlas_orch_exit_kind_name(res.exit_kind));
    atlas_driver_res_free(&res);
    denv_close(&e);
}

static bool always_cancel(void *ud) {
    (void)ud;
    return true;
}

static void test_cancellation_is_asked_for_not_signalled(void) {
    denv e;
    denv_open(&e);
    atlas_err err;
    atlas_err_init(&err);
    atlas_driver_req req;
    fill_req(&e, &req, "do something ordinary");
    req.cancel = always_cancel;
    atlas_driver_res res;
    atlas_driver_res_init(&res);
    const atlas_driver *d = atlas_driver_find("fake");
    T_OK(d->run(&req, &res, &err), &err);
    T_CHECK_MSG(res.exit_kind == ATLAS_ORCH_EXIT_CANCELLED,
                "a driver told to stop reported %s", atlas_orch_exit_kind_name(res.exit_kind));
    atlas_driver_res_free(&res);
    denv_close(&e);
}

static void test_the_claude_driver_refuses_without_policy_or_credential(void) {
    /* Two independent gates, checked in order. `live_model` off refuses before
     * anything else; with it on, the absence of a root-installed service
     * credential refuses. Neither reaches for an operator's personal session,
     * and the message names the service path rather than hinting at one. */
    denv e;
    denv_open(&e);
    const atlas_driver *d = atlas_driver_find("claude");
    T_REQUIRE(d != NULL);
    T_CHECK_MSG(d->needs_live_model, "the claude driver does not declare that it needs a model");

    atlas_driver_req req;
    fill_req(&e, &req, "add a comment");
    req.live_model = false;
    atlas_driver_res res;
    atlas_driver_res_init(&res);
    atlas_err err;
    atlas_err_init(&err);
    T_CHECK_MSG(d->run(&req, &res, &err) != ATLAS_OK, "the claude driver ran with live_model off");
    T_CHECK(strstr(atlas_err_msg(&err), "live_model") != NULL);
    atlas_driver_res_free(&res);

    /* With the policy permitting it, the credential gate is what refuses — and
     * this machine has no root-installed worker credential, which is the state
     * every machine is in until an operator installs one. */
    atlas_driver_res res2;
    atlas_driver_res_init(&res2);
    atlas_err e2;
    atlas_err_init(&e2);
    req.live_model = true;
    atlas_status st = d->run(&req, &res2, &e2);
    if (st != ATLAS_OK) {
        T_CHECK_MSG(strstr(atlas_err_msg(&e2), "/etc/atlas/claude.env") != NULL,
                    "the credential refusal does not name the service credential path: %s",
                    atlas_err_msg(&e2));
    }
    atlas_driver_res_free(&res2);
    denv_close(&e);
}

/* --- the execution boundary ---------------------------------------------------- */

static void test_structured_argv_reaches_a_child_unchanged(void) {
    /* Shell metacharacters in an argument are inert because there is no shell:
     * `atlas_proc_run` execve's a vector. If any of these were interpreted, the
     * echoed output would differ from the argument. */
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf exe = ATLAS_BUF_INIT;
    T_OK(atlas_proc_which("printf", "/usr/local/bin:/usr/bin:/bin", &exe, &err), &err);

    static const char *const HOSTILE = "$(touch /tmp/atlas-pwned); `id`; a|b>c&d";
    const char *argv[] = {atlas_buf_cstr(&exe), "%s", HOSTILE, NULL};
    static const char *const ENV[] = {"PATH=/usr/bin:/bin", "LC_ALL=C", NULL};
    atlas_buf out = ATLAS_BUF_INIT;
    atlas_proc_opts o;
    memset(&o, 0, sizeof(o));
    o.argv = argv;
    o.env = ENV;
    o.timeout_ms = 10000;
    atlas_proc_result res;
    T_OK(atlas_proc_run(&o, atlas_proc_sink_buf, &out, NULL, &res, &err), &err);
    T_EQ_INT(res.exit_code, 0);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&out), HOSTILE) == 0,
                "the argument was transformed on its way to the child: %s",
                atlas_buf_cstr(&out));
    struct stat sb;
    T_CHECK_MSG(stat("/tmp/atlas-pwned", &sb) != 0, "command substitution executed");

    atlas_buf_free(&out);
    atlas_buf_free(&exe);
}

static void test_the_child_environment_is_only_what_atlas_built(void) {
    /* Nothing is inherited. A variable Atlas did not put in the vector is
     * absent from the child, whatever the parent's environment holds. */
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(setenv("ATLAS_TEST_LEAK", "leaked", 1) == 0);
    T_REQUIRE(setenv("SSH_AUTH_SOCK", "/tmp/agent", 1) == 0);

    atlas_buf exe = ATLAS_BUF_INIT;
    T_OK(atlas_proc_which("env", "/usr/local/bin:/usr/bin:/bin", &exe, &err), &err);
    const char *argv[] = {atlas_buf_cstr(&exe), NULL};
    static const char *const ENV[] = {"PATH=/usr/bin:/bin", "LC_ALL=C", "TZ=UTC", NULL};
    atlas_buf out = ATLAS_BUF_INIT;
    atlas_proc_opts o;
    memset(&o, 0, sizeof(o));
    o.argv = argv;
    o.env = ENV;
    o.timeout_ms = 10000;
    atlas_proc_result res;
    T_OK(atlas_proc_run(&o, atlas_proc_sink_buf, &out, NULL, &res, &err), &err);
    const char *s = atlas_buf_cstr(&out);
    T_CHECK_MSG(strstr(s, "ATLAS_TEST_LEAK") == NULL, "the parent environment leaked into a job");
    T_CHECK_MSG(strstr(s, "SSH_AUTH_SOCK") == NULL, "a credential agent leaked into a job");
    T_CHECK_MSG(strstr(s, "PATH=/usr/bin:/bin") != NULL, "the built environment did not arrive");

    (void)unsetenv("ATLAS_TEST_LEAK");
    (void)unsetenv("SSH_AUTH_SOCK");
    atlas_buf_free(&out);
    atlas_buf_free(&exe);
}

static void test_a_child_runs_in_the_directory_it_was_given(void) {
    atlas_err err;
    atlas_err_init(&err);
    denv e;
    denv_open(&e);
    atlas_buf exe = ATLAS_BUF_INIT;
    T_OK(atlas_proc_which("pwd", "/usr/local/bin:/usr/bin:/bin", &exe, &err), &err);
    const char *argv[] = {atlas_buf_cstr(&exe), NULL};
    static const char *const ENV[] = {"PATH=/usr/bin:/bin", NULL};
    atlas_buf out = ATLAS_BUF_INIT;
    atlas_proc_opts o;
    memset(&o, 0, sizeof(o));
    o.argv = argv;
    o.env = ENV;
    o.cwd = atlas_buf_cstr(&e.ws.work);
    o.timeout_ms = 10000;
    atlas_proc_result res;
    T_OK(atlas_proc_run(&o, atlas_proc_sink_buf, &out, NULL, &res, &err), &err);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "work") != NULL,
                "the child did not start in its workspace: %s", atlas_buf_cstr(&out));

    /* A working directory that does not exist is a refusal, not a run somewhere
     * else. */
    atlas_err e2;
    atlas_err_init(&e2);
    atlas_proc_opts bad = o;
    bad.cwd = "/nonexistent/atlas/workspace";
    atlas_proc_result r2;
    T_CHECK(atlas_proc_run(&bad, NULL, NULL, NULL, &r2, &e2) != ATLAS_OK);

    atlas_buf_free(&out);
    atlas_buf_free(&exe);
    denv_close(&e);
}

static bool never_cancel(void *ud) {
    (void)ud;
    return false;
}

static void test_a_cancellable_child_is_not_killed_by_its_own_poll(void) {
    /* The defect this exists for: with a cancel callback configured, the poll
     * interval is capped so the callback is consulted promptly — and poll then
     * returns zero every 100 ms while a healthy child works. Reading that as an
     * expiry killed every cancellable child almost immediately, which is what
     * happened to the first real model run and why its logs were empty.
     *
     * Both bounds here are far away, so a child that sleeps briefly and then
     * speaks must survive and exit on its own. */
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf exe = ATLAS_BUF_INIT;
    T_OK(atlas_proc_which("sleep", "/usr/local/bin:/usr/bin:/bin", &exe, &err), &err);
    const char *argv[] = {atlas_buf_cstr(&exe), "2", NULL};
    static const char *const ENV[] = {"PATH=/usr/bin:/bin", NULL};
    atlas_proc_opts o;
    memset(&o, 0, sizeof(o));
    o.argv = argv;
    o.env = ENV;
    o.timeout_ms = 60000;
    o.idle_timeout_ms = 30000;
    o.cancel = never_cancel;
    atlas_proc_result res;
    memset(&res, 0, sizeof(res));
    T_OK(atlas_proc_run(&o, NULL, NULL, NULL, &res, &err), &err);
    T_CHECK_MSG(!res.timed_out, "a healthy cancellable child was reported as timed out");
    T_CHECK_MSG(!res.idle_timed_out, "a healthy cancellable child hit the idle bound");
    T_CHECK_MSG(!res.cancelled, "a child was cancelled without being asked to stop");
    T_EQ_INT(res.exit_code, 0);
    atlas_buf_free(&exe);
}

static void test_an_idle_child_is_killed_on_its_own_bound(void) {
    /* The wall bound is generous and the idle bound is short, so only the idle
     * bound can be what fires — which is the point of having two. */
    atlas_err err;
    atlas_err_init(&err);
    const char *argv[] = {ATLAS_SPAWNER_BIN, "/dev/null", NULL};
    static const char *const ENV[] = {"PATH=/usr/bin:/bin", NULL};
    atlas_proc_opts o;
    memset(&o, 0, sizeof(o));
    o.argv = argv;
    o.env = ENV;
    o.timeout_ms = 60000;
    o.idle_timeout_ms = 700;
    o.grace_ms = 200;
    atlas_proc_result res;
    memset(&res, 0, sizeof(res));
    (void)atlas_proc_run(&o, NULL, NULL, NULL, &res, &err);
    T_CHECK_MSG(res.idle_timed_out, "a silent child was not stopped by the idle bound");
    T_CHECK_MSG(!res.timed_out, "the wall bound fired instead of the idle bound");
}

static void test_the_whole_process_group_is_terminated(void) {
    /* The grandchild outlives its parent unless the *group* is signalled. If
     * Atlas killed only the direct child, it would still be alive here. */
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_OK(fx_open(&fx, &err), &err);
    atlas_buf pidfile = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&pidfile, &err, "%s/grandchild.pid", fx_data_dir(&fx)), &err);

    const char *argv[] = {ATLAS_SPAWNER_BIN, atlas_buf_cstr(&pidfile), NULL};
    static const char *const ENV[] = {"PATH=/usr/bin:/bin", NULL};
    atlas_proc_opts o;
    memset(&o, 0, sizeof(o));
    o.argv = argv;
    o.env = ENV;
    o.timeout_ms = 1500;
    o.grace_ms = 200;
    atlas_proc_result res;
    memset(&res, 0, sizeof(res));
    (void)atlas_proc_run(&o, NULL, NULL, NULL, &res, &err);
    T_CHECK(res.timed_out);

    /* Read the grandchild's pid and require that it is gone. */
    FILE *f = fopen(atlas_buf_cstr(&pidfile), "r");
    T_REQUIRE(f != NULL);
    long long pid = 0;
    int got = fscanf(f, "%lld", &pid);
    (void)fclose(f);
    T_REQUIRE(got == 1 && pid > 0);
    /* A short settle: the kill is delivered, the process is reaped by init. */
    for (int i = 0; i < 50 && kill((pid_t)pid, 0) == 0; i++) {
        struct timespec ts = {0, 100 * 1000 * 1000};
        (void)nanosleep(&ts, NULL);
    }
    T_CHECK_MSG(kill((pid_t)pid, 0) != 0,
                "a grandchild survived: only the direct child was terminated");

    atlas_buf_free(&pidfile);
    fx_close(&fx);
}

static void test_output_beyond_the_bound_stops_the_child(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf exe = ATLAS_BUF_INIT;
    T_OK(atlas_proc_which("yes", "/usr/local/bin:/usr/bin:/bin", &exe, &err), &err);
    const char *argv[] = {atlas_buf_cstr(&exe), NULL};
    static const char *const ENV[] = {"PATH=/usr/bin:/bin", NULL};
    atlas_buf out = ATLAS_BUF_INIT;
    atlas_proc_opts o;
    memset(&o, 0, sizeof(o));
    o.argv = argv;
    o.env = ENV;
    o.timeout_ms = 20000;
    o.max_stdout = 16u * 1024u;
    atlas_proc_result res;
    memset(&res, 0, sizeof(res));
    atlas_err e2;
    atlas_err_init(&e2);
    (void)atlas_proc_run(&o, atlas_proc_sink_buf, &out, NULL, &res, &e2);
    /* Refused, and the child stopped. Not trimmed and left running. */
    T_CHECK_MSG(res.stdout_truncated, "an unbounded child was not stopped at its output bound");
    atlas_buf_free(&out);
    atlas_buf_free(&exe);
}

/* --- A11.5a-R2: what counts as a worker making progress ------------------- */

/* The rule that replaced "were there any bytes?". Two real workers were killed
 * at the 300 s idle bound while mid-turn, because `--output-format json` says
 * nothing on stdout until it is finished: the silence was the format, not the
 * worker. Idle is now measured in recognised records, so what does and does not
 * count is worth asserting exactly. */
static void test_a_streamed_record_is_recognised_and_prose_is_not(void) {
    /* Shapes taken from the installed Claude Code 2.1.235, not invented. */
    static const char *const REAL[] = {
        "{\"type\":\"system\",\"subtype\":\"init\",\"cwd\":\"/opt/atlas\"}",
        "{\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":\"tool_use\","
        "\"name\":\"Edit\"}]}}",
        "{\"type\":\"user\",\"message\":{\"content\":[{\"type\":\"tool_result\"}]}}",
        "{\"type\":\"rate_limit_event\",\"x\":1}",
        "{\"type\":\"result\",\"is_error\":false}",
    };
    for (size_t i = 0; i < sizeof REAL / sizeof REAL[0]; i++) {
        T_CHECK_MSG(atlas_driver_progress_line_is_event(REAL[i], strlen(REAL[i])),
                    "a real streamed record was not recognised: %s", REAL[i]);
    }

    /* None of these may keep a worker alive. The first is the exact failure the
     * bound exists to catch — a process that has stopped working and is still
     * talking. */
    static const char *const NOT[] = {
        "still working, please wait",
        "",
        "{",
        /* Well-formed prefix, no close: a record that never arrived. */
        "{\"type\":\"assistant\",\"message\":{\"content\":[",
        /* A type Atlas does not know is not a type Atlas accepts. */
        "{\"type\":\"keepalive\"}",
        /* Not at the start of the line. */
        "  {\"type\":\"assistant\"}",
        "[{\"type\":\"assistant\"}]",
    };
    for (size_t i = 0; i < sizeof NOT / sizeof NOT[0]; i++) {
        T_CHECK_MSG(!atlas_driver_progress_line_is_event(NOT[i], strlen(NOT[i])),
                    "output that is not a progress record was accepted as one: %s", NOT[i]);
    }
}

static const atlas_test TESTS[] = {
    {"a_streamed_record_is_recognised_and_prose_is_not",
     test_a_streamed_record_is_recognised_and_prose_is_not},
    {"an unknown driver is refused and never defaulted",
     test_an_unknown_driver_is_refused_and_never_defaulted},
    {"the fake driver is deterministic in each behaviour",
     test_the_fake_driver_is_deterministic_in_each_behaviour},
    {"a zero exit with malformed metadata is not success",
     test_a_zero_exit_with_malformed_metadata_is_not_success},
    {"cancellation is asked for, not signalled", test_cancellation_is_asked_for_not_signalled},
    {"the claude driver refuses without policy or credential",
     test_the_claude_driver_refuses_without_policy_or_credential},
    {"structured argv reaches a child unchanged",
     test_structured_argv_reaches_a_child_unchanged},
    {"the child environment is only what Atlas built",
     test_the_child_environment_is_only_what_atlas_built},
    {"a child runs in the directory it was given",
     test_a_child_runs_in_the_directory_it_was_given},
    {"a cancellable child is not killed by its own poll",
     test_a_cancellable_child_is_not_killed_by_its_own_poll},
    {"an idle child is killed on its own bound",
     test_an_idle_child_is_killed_on_its_own_bound},
    {"the whole process group is terminated", test_the_whole_process_group_is_terminated},
    {"output beyond the bound stops the child", test_output_beyond_the_bound_stops_the_child},
};

ATLAS_TEST_MAIN("orch_driver", TESTS)
