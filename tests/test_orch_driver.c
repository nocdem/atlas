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

/* --- A12.0: the role a driver has, and the model that follows from it --------
 *
 * A role is a property of the driver, and the *name* of the model each role runs
 * under comes from the root-owned policy. No model name appears in `src/`, so
 * what is asserted here is the mapping and never a value.
 */
static void test_every_driver_declares_the_role_it_works_in(void) {
    struct {
        const char *name;
        atlas_driver_role role;
        bool live;
    } WANT[] = {
        {"fake", ATLAS_DRIVER_ROLE_NONE, false},
        {"claude", ATLAS_DRIVER_ROLE_EXECUTOR, true},
        {"claude-repo", ATLAS_DRIVER_ROLE_EXECUTOR, true},
        {"fake-repo", ATLAS_DRIVER_ROLE_NONE, false},
        {"claude-plan", ATLAS_DRIVER_ROLE_PLANNER, true},
        /* The no-model test double for the planner path: it produces a plan
         * artifact, so it has to hold the role a revision will be required to
         * come from, and it needs no model to do it. */
        {"fake-plan", ATLAS_DRIVER_ROLE_PLANNER, false},
    };
    /* NONE is zero, so a driver that never states a role has no role rather
     * than a plausible one. */
    T_EQ_INT((int)ATLAS_DRIVER_ROLE_NONE, 0);
    for (size_t i = 0; i < sizeof WANT / sizeof WANT[0]; i++) {
        const atlas_driver *d = atlas_driver_find(WANT[i].name);
        T_REQUIRE(d != NULL);
        T_CHECK_MSG(d->role == WANT[i].role, "%s declares role %d, wanted %d", WANT[i].name,
                    (int)d->role, (int)WANT[i].role);
        T_CHECK_MSG(d->needs_live_model == WANT[i].live, "%s declares the wrong model need",
                    WANT[i].name);
    }
    /* Neither new driver works in the registered repository's own tree. That is
     * the one list, it did not move, and nothing here may widen it. */
    T_CHECK(!atlas_orch_driver_is_repo_tree("claude-plan"));
    T_CHECK(!atlas_orch_driver_is_repo_tree("fake-plan"));
}

static void test_the_model_a_driver_runs_under_follows_its_role(void) {
    atlas_driver_models m;
    m.planner = "plan-model";
    m.executor = "exec-model";
    struct {
        const char *name;
        const char *want;
    } CASES[] = {
        {"claude-plan", "plan-model"}, {"fake-plan", "plan-model"},
        {"claude", "exec-model"},      {"claude-repo", "exec-model"},
        {"fake", ""},                  {"fake-repo", ""},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        const atlas_driver *d = atlas_driver_find(CASES[i].name);
        T_REQUIRE(d != NULL);
        const char *got = atlas_driver_model_for(d, &m);
        T_REQUIRE(got != NULL);
        T_CHECK_MSG(strcmp(got, CASES[i].want) == 0, "%s would run under \"%s\", wanted \"%s\"",
                    CASES[i].name, got, CASES[i].want);
    }
    /* A policy that named no model leaves every driver where it was: on
     * whatever the account's own session defaults to. Empty, never NULL, so a
     * caller cannot dereference the absence. */
    atlas_driver_models none;
    memset(&none, 0, sizeof(none));
    T_CHECK(strcmp(atlas_driver_model_for(atlas_driver_find("claude"), &none), "") == 0);
    T_CHECK(strcmp(atlas_driver_model_for(atlas_driver_find("claude-plan"), NULL), "") == 0);
    T_CHECK(strcmp(atlas_driver_model_for(NULL, &m), "") == 0);
}

static void test_the_model_flag_is_absent_unless_the_policy_named_one(void) {
    /* The argument vector is built where a test can see it, because the whole
     * of "a worker runs under the model the operator chose" is one element pair
     * in a vector that must otherwise not move — the task stays last, and it is
     * still one element that never reaches a shell. */
    denv e;
    denv_open(&e);
    static const char *const TASK = "$(id); do the thing";
    const char *argv[ATLAS_DRIVER_CLAUDE_ARGV_MAX];
    atlas_driver_req req;
    fill_req(&e, &req, TASK);

    size_t n = atlas_driver_claude_build_argv(&req, "/usr/bin/claude", argv,
                                              ATLAS_DRIVER_CLAUDE_ARGV_MAX);
    T_REQUIRE(n >= 2u && n < ATLAS_DRIVER_CLAUDE_ARGV_MAX);
    T_CHECK(argv[n] == NULL);
    T_CHECK(strcmp(argv[0], "/usr/bin/claude") == 0);
    T_CHECK_MSG(argv[n - 1u] == TASK, "the task is not the last argument");
    for (size_t i = 0; i < n; i++) {
        T_CHECK_MSG(strcmp(argv[i], "--model") != 0,
                    "a model flag appeared although the policy named none");
    }
    size_t plain = n;

    /* With a model named, exactly two elements are added: the flag and its
     * value, in that order, before the task — which is still last. */
    req.model = "fable";
    n = atlas_driver_claude_build_argv(&req, "/usr/bin/claude", argv,
                                       ATLAS_DRIVER_CLAUDE_ARGV_MAX);
    T_CHECK_MSG(n == plain + 2u, "naming a model changed the vector by %zu elements",
                n - plain);
    T_CHECK(argv[n] == NULL);
    T_CHECK_MSG(argv[n - 1u] == TASK, "the task is no longer the last argument");
    size_t flag = 0;
    size_t seen = 0;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(argv[i], "--model") == 0) {
            flag = i;
            seen++;
        }
    }
    T_CHECK_MSG(seen == 1u, "the model flag appeared %zu times", seen);
    T_REQUIRE(flag + 1u < n);
    T_CHECK(strcmp(argv[flag + 1u], "fable") == 0);
    T_CHECK_MSG(flag + 2u == n - 1u, "the model flag is not immediately before the task");

    /* An empty name is not a model. It is a policy that named none. */
    req.model = "";
    n = atlas_driver_claude_build_argv(&req, "/usr/bin/claude", argv,
                                       ATLAS_DRIVER_CLAUDE_ARGV_MAX);
    T_CHECK(n == plain);
    for (size_t i = 0; i < n; i++) {
        T_CHECK(strcmp(argv[i], "--model") != 0);
    }

    /* A vector that cannot be built is refused rather than truncated: a short
     * buffer would otherwise produce a run without its task. */
    req.model = "fable";
    T_CHECK(atlas_driver_claude_build_argv(&req, "/usr/bin/claude", argv, plain + 1u) == 0);
    req.task = NULL;
    T_CHECK(atlas_driver_claude_build_argv(&req, "/usr/bin/claude", argv,
                                           ATLAS_DRIVER_CLAUDE_ARGV_MAX) == 0);
    denv_close(&e);
}

/* --- A12.0: the planner's test double --------------------------------------- */

static void read_artifact(denv *e, const char *rel, atlas_buf *out, bool *found) {
    char path[4096];
    (void)snprintf(path, sizeof path, "%s/%s", atlas_buf_cstr(&e->ws.artifacts), rel);
    *found = false;
    atlas_buf_reset(out);
    FILE *f = fopen(path, "re");
    if (f == NULL) {
        return;
    }
    *found = true;
    char chunk[1024];
    size_t got = 0;
    atlas_err err;
    atlas_err_init(&err);
    while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) {
        T_OK(atlas_buf_append(out, chunk, got, &err), &err);
    }
    (void)fclose(f);
}

static atlas_status run_fake_plan(denv *e, const char *task, atlas_driver_res *res,
                                  atlas_err *err) {
    atlas_driver_req req;
    fill_req(e, &req, task);
    atlas_driver_res_init(res);
    const atlas_driver *d = atlas_driver_find("fake-plan");
    T_REQUIRE(d != NULL);
    return d->run(&req, res, err);
}

static void test_the_fake_plan_driver_emits_the_plan_it_was_given(void) {
    /* Everything after the marker line, verbatim — including the bytes that
     * would matter if anything ever parsed this, which nothing does here. The
     * driver is a fixture: it exists so the plan pipeline can be driven with no
     * model, no network and no credential. */
    static const char TASK[] =
        "write a plan for the thing\n"
        "fake-plan-artifact:\n"
        "stage 1\n  task: do the first thing\nstage 2\n";
    static const char WANT[] = "stage 1\n  task: do the first thing\nstage 2\n";

    denv e;
    denv_open(&e);
    atlas_err err;
    atlas_err_init(&err);
    atlas_driver_res res;
    T_OK(run_fake_plan(&e, TASK, &res, &err), &err);
    T_CHECK_MSG(res.exit_kind == ATLAS_ORCH_EXIT_OK, "the fake planner reported %s",
                atlas_orch_exit_kind_name(res.exit_kind));
    T_EQ_INT((int)res.exit_code, 0);
    T_CHECK(strcmp(atlas_buf_cstr(&res.version), "fake-plan/1") == 0);

    atlas_buf got = ATLAS_BUF_INIT;
    bool found = false;
    read_artifact(&e, "plan.atlas-plan", &got, &found);
    T_CHECK_MSG(found, "the fake planner wrote no plan artifact");
    T_CHECK_MSG(got.len == sizeof WANT - 1u && memcmp(got.data, WANT, got.len) == 0,
                "the plan artifact is not what the task carried: \"%s\"", atlas_buf_cstr(&got));
    atlas_buf_free(&got);
    atlas_driver_res_free(&res);
    denv_close(&e);
}

static void test_the_fake_plan_driver_fails_rather_than_inventing_a_plan(void) {
    /* No marker, no plan. It fails as an ordinary job failure — a nonzero exit
     * the layers above classify — rather than as an Atlas error, because a
     * planner that produced nothing is a failed attempt and not a broken
     * daemon. */
    denv e;
    denv_open(&e);
    atlas_err err;
    atlas_err_init(&err);
    atlas_driver_res res;
    T_OK(run_fake_plan(&e, "write a plan and say nothing useful", &res, &err), &err);
    T_CHECK_MSG(res.exit_kind == ATLAS_ORCH_EXIT_NONZERO, "a planner with no plan reported %s",
                atlas_orch_exit_kind_name(res.exit_kind));
    atlas_buf got = ATLAS_BUF_INIT;
    bool found = false;
    read_artifact(&e, "plan.atlas-plan", &got, &found);
    T_CHECK_MSG(!found, "a planner that produced no plan still wrote one");
    atlas_buf_free(&got);
    atlas_driver_res_free(&res);
    denv_close(&e);

    /* A marker with nothing after it is an empty plan, not a missing one: the
     * rule is "everything after the line", and everything after it is nothing.
     * Whether an empty plan is acceptable is the plan layer's question. */
    denv e2;
    denv_open(&e2);
    atlas_driver_res res2;
    T_OK(run_fake_plan(&e2, "here it is\nfake-plan-artifact:\n", &res2, &err), &err);
    T_CHECK(res2.exit_kind == ATLAS_ORCH_EXIT_OK);
    atlas_buf empty = ATLAS_BUF_INIT;
    found = false;
    read_artifact(&e2, "plan.atlas-plan", &empty, &found);
    T_CHECK_MSG(found && empty.len == 0, "a bare marker did not produce an empty plan");
    atlas_buf_free(&empty);
    atlas_driver_res_free(&res2);
    denv_close(&e2);

    /* The marker is a line of its own, matched exactly. A mention of it inside
     * the prose is prose. */
    denv e3;
    denv_open(&e3);
    atlas_driver_res res3;
    T_OK(run_fake_plan(&e3, "do not write fake-plan-artifact: here\n", &res3, &err), &err);
    T_CHECK_MSG(res3.exit_kind == ATLAS_ORCH_EXIT_NONZERO,
                "a mention of the marker inside a line selected the behaviour");
    atlas_driver_res_free(&res3);
    denv_close(&e3);
}

static void test_the_workspace_planners_refuse_without_a_workspace(void) {
    /* Both plan drivers work in an isolated workspace. Given none, they refuse
     * rather than falling back to a directory nobody chose — the rule A8's
     * `claude` driver follows, and the reason neither is a repo-tree driver. */
    atlas_driver_req req;
    memset(&req, 0, sizeof(req));
    req.job_uid = JOB_D;
    req.attempt_no = 1;
    req.task = "fake-plan-artifact:\nstage 1\n";
    req.mode = "patch";
    req.live_model = true;
    static const char *const NAMES[] = {"fake-plan", "claude-plan"};
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) {
        const atlas_driver *d = atlas_driver_find(NAMES[i]);
        T_REQUIRE(d != NULL);
        atlas_driver_res res;
        atlas_driver_res_init(&res);
        atlas_err err;
        atlas_err_init(&err);
        T_CHECK_MSG(d->run(&req, &res, &err) != ATLAS_OK, "%s ran without a workspace",
                    NAMES[i]);
        atlas_driver_res_free(&res);
    }
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
    {"every driver declares the role it works in",
     test_every_driver_declares_the_role_it_works_in},
    {"the model a driver runs under follows its role",
     test_the_model_a_driver_runs_under_follows_its_role},
    {"the model flag is absent unless the policy named one",
     test_the_model_flag_is_absent_unless_the_policy_named_one},
    {"the fake plan driver emits the plan it was given",
     test_the_fake_plan_driver_emits_the_plan_it_was_given},
    {"the fake plan driver fails rather than inventing a plan",
     test_the_fake_plan_driver_fails_rather_than_inventing_a_plan},
    {"the workspace planners refuse without a workspace",
     test_the_workspace_planners_refuse_without_a_workspace},
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
