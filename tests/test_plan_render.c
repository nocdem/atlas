/* Atlas - A12.0: the two renderers for a plan, over one service result.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * `plan_item` is one vtbl method serving a list row and a detail view, exactly
 * as `job_item` is, and this drives both implementations of it directly.
 *
 * ## Why this test exists at all
 *
 * Every other surface in this binary is reachable from `tests/test_cli.c` by
 * running the built program. A plan is not: `plan.get` and `plan.list` are
 * gated by `require_submitter`, which reads the **root-owned** orchestration
 * policy, and an unprivileged uid cannot create one anywhere — that is the point
 * of A7.1 and it is what `tests/test_orch_model.c` proves. So no fixture daemon
 * this suite can start will ever answer a plan read, and the renderers would
 * otherwise ship with the build as their only check.
 *
 * ## The offset hazard, checked rather than described
 *
 * `atlas_renderer_vtbl` is initialised positionally in two files. This
 * repository has already shipped a drift between those two lists, and the
 * failure mode is silent: two adjacent members with compatible signatures
 * misalign without a diagnostic. Calling `v->plan_item` through the vtbl — not
 * the function by name — is what makes a misalignment a failing test rather than
 * a wrong document, because a slot holding `apikey_revoked` would be handed an
 * `atlas_plan_render *` where it expects a key id.
 *
 * ## What is asserted
 *
 * That the two renderers describe one result the same way, that every untrusted
 * value is printed **as it arrived** — already safe-encoded by the daemon, never
 * encoded a second time — and that an absent job, run or measurement is an
 * absent line and an absent key rather than an empty string or a zero.
 */
#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/plan.h"
#include "atlas/service.h"
#include "atlas_test.h"
#include "cli/render.h"
#include "support/jsoncheck.h"

/* Renders one result and hands back everything that reached the stream. */
static void render(const atlas_plan_render *pr, bool json, bool in_list, atlas_buf *out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf_reset(out);

    char *buf = NULL;
    size_t len = 0;
    FILE *fp = open_memstream(&buf, &len);
    T_REQUIRE(fp != NULL);

    atlas_renderer r;
    memset(&r, 0, sizeof(r));
    r.out = fp;
    r.json = json;
    atlas_safe_pool_init(&r.safe);
    r.open_scope = -1;
    r.v = json ? &ATLAS_RENDERER_JSON : &ATLAS_RENDERER_HUMAN;
    if (json) {
        r.j = atlas_json_new(fp, &err);
        T_REQUIRE(r.j != NULL);
    }
    T_OK(r.v->begin(&r, "plan status", &err), &err);
    if (in_list) {
        T_OK(r.v->list_begin(&r, "plans", &err), &err);
    }
    /* Through the vtbl, deliberately. See the header comment. */
    T_OK(r.v->plan_item(&r, pr, &err), &err);
    if (in_list) {
        T_OK(r.v->list_end(&r, "plan", "plans", 1, &err), &err);
    }
    T_OK(r.v->end(&r, &err), &err);
    atlas_safe_pool_free(&r.safe);

    (void)fflush(fp);
    (void)fclose(fp);
    T_OK(atlas_buf_set(out, buf != NULL ? buf : "", len, &err), &err);
    free(buf);
}

/* A plan mid-flight: one compiled revision, a tree task that became a run and a
 * workspace sibling nobody has submitted yet.
 *
 * Every untrusted string is written here **as the daemon would have encoded
 * it** — `%0A` for a newline in the gate floor block, `%25` for a literal per
 * cent in the goal — because the claim under test is that a renderer prints it
 * unchanged. A renderer that encoded a second time would turn `%25` into
 * `%2525`, which is exactly how a value stops round-tripping. */
static void fill_example(atlas_plan_render *pr) {
    memset(pr, 0, sizeof(*pr));
    pr->detail = true;
    pr->plan = "p0123456789abcdef0123456789abcdef";
    pr->repo = "proj";
    pr->status = "EXECUTING";
    pr->created_at = "2026-08-21T00:00:00Z";
    pr->goal = "make the 100%25 case work";
    pr->gate_floor_text = "make pass%0Amake test%0A";
    pr->gate_floor_count = 2;
    pr->max_parallel = 2;
    pr->rev_no = 1;
    pr->planner_jobs_seen = 1;
    pr->stages_accepted = 0;
    pr->revision_count = 1;
    pr->planner_job = "j0123456789abcdef0123456789abcdef";
    pr->planner_job_state = "SUCCEEDED";

    pr->tasks[0].key = "build";
    pr->tasks[0].kind = "TREE";
    pr->tasks[0].title = "Build the%20thing";
    pr->tasks[0].stage = 1;
    pr->tasks[0].job = "jaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    pr->tasks[0].job_state = "RUNNING";
    pr->tasks[0].run = "rbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    pr->tasks[0].run_status = "ACTIVE";
    pr->tasks[0].usage_model = "a-model";
    pr->tasks[0].usage_cost_micro_usd = 1250000;
    pr->tasks[0].has_cost = true;
    pr->tasks[0].usage_turns = 7;
    pr->tasks[0].has_turns = true;

    /* Not submitted, and nothing measured. */
    pr->tasks[1].key = "notes";
    pr->tasks[1].kind = "SIDE";
    pr->tasks[1].title = "Take notes";
    pr->tasks[1].stage = 1;
    pr->task_count = 2;
}

static void test_the_human_form_prints_what_arrived(void) {
    atlas_plan_render pr;
    fill_example(&pr);
    atlas_buf out = ATLAS_BUF_INIT;
    render(&pr, false, false, &out);
    const char *s = atlas_buf_cstr(&out);

    T_CHECK(strstr(s, "plan          p0123456789abcdef0123456789abcdef\n") != NULL);
    T_CHECK(strstr(s, "repository    proj\n") != NULL);
    T_CHECK(strstr(s, "status        EXECUTING\n") != NULL);
    /* Both counts carry their compiled-in ceiling: a number with no bound beside
     * it does not say whether the plan has budget left. */
    {
        char want[64];
        (void)snprintf(want, sizeof want, "revision      1 of %d\n", ATLAS_PLAN_MAX_REVISIONS);
        T_CHECK_MSG(strstr(s, want) != NULL, "the revision line is missing from:\n%s", s);
        (void)snprintf(want, sizeof want, "planner jobs  1 of %d\n", ATLAS_PLAN_MAX_PLANNER_JOBS);
        T_CHECK_MSG(strstr(s, want) != NULL, "the planner-jobs line is missing from:\n%s", s);
    }
    /* One tree task in the revision, so one stage. */
    T_CHECK(strstr(s, "stages        0 of 1 accepted\n") != NULL);

    /* Untrusted values, labelled and printed exactly as they arrived. */
    T_CHECK(strstr(s, "goal (untrusted, atlas-safe-1)\n  make the 100%25 case work\n") != NULL);
    T_CHECK_MSG(strstr(s, "%2525") == NULL, "the goal was encoded a second time:\n%s", s);
    T_CHECK(strstr(s, "gate floor    2 command(s), atlas-safe-1\n  make pass%0Amake test%0A\n") !=
            NULL);

    T_CHECK(strstr(s, "stage 1 task build [tree] RUNNING run "
                      "rbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb ACTIVE\n") != NULL);
    T_CHECK(strstr(s, "title (untrusted, atlas-safe-1)  Build the%20thing\n") != NULL);
    T_CHECK(strstr(s, "model a-model, cost 1.250000 USD, 7 turn(s)\n") != NULL);

    /* The sibling has no job and nothing measured: an absent line, never an
     * empty identifier and never a cost of zero. */
    T_CHECK(strstr(s, "stage 1 task notes [side]\n") != NULL);
    T_CHECK_MSG(strstr(s, "cost 0.000000") == NULL,
                "an unmeasured task was reported as free:\n%s", s);
    /* Nothing was busy and no document was asked for. */
    T_CHECK(strstr(s, "resumable") == NULL);
    T_CHECK(strstr(s, "plan document") == NULL);
    atlas_buf_free(&out);
}

static void test_the_json_form_mirrors_it(void) {
    atlas_plan_render pr;
    fill_example(&pr);
    atlas_buf out = ATLAS_BUF_INIT;
    render(&pr, true, false, &out);

    size_t bad = 0;
    T_CHECK_MSG(tjson_valid(out.data, out.len, &bad), "not valid JSON at %zu: %s", bad,
                atlas_buf_cstr(&out));

    const struct {
        const char *key;
        const char *want;
    } strs[] = {
        {"plan", "p0123456789abcdef0123456789abcdef"},
        {"repo", "proj"},
        {"status", "EXECUTING"},
        {"goal", "make the 100%25 case work"},
        {"goal_encoding", "atlas-safe-1"},
        {"gate_floor_text", "make pass%0Amake test%0A"},
        {"gate_floor_encoding", "atlas-safe-1"},
        {"task_title_encoding", "atlas-safe-1"},
        {"task_title_provenance", "UNTRUSTED_DATA"},
        {"planner_job_state", "SUCCEEDED"},
    };
    for (size_t i = 0; i < sizeof strs / sizeof strs[0]; i++) {
        atlas_buf got = ATLAS_BUF_INIT;
        if (!tjson_get_string(out.data, out.len, strs[i].key, &got)) {
            atlas_test_fail(__FILE__, __LINE__, "key \"%s\" is missing from %s", strs[i].key,
                            atlas_buf_cstr(&out));
        } else {
            T_CHECK_MSG(strcmp(atlas_buf_cstr(&got), strs[i].want) == 0, "%s came back as \"%s\"",
                        strs[i].key, atlas_buf_cstr(&got));
        }
        atlas_buf_free(&got);
    }
    /* The same ceilings the human form prints, as numbers a caller can compare
     * against rather than as prose. */
    {
        char want[64];
        (void)snprintf(want, sizeof want, "\"revision_limit\":%d", ATLAS_PLAN_MAX_REVISIONS);
        T_CHECK(strstr(atlas_buf_cstr(&out), want) != NULL);
        (void)snprintf(want, sizeof want, "\"planner_job_limit\":%d",
                       ATLAS_PLAN_MAX_PLANNER_JOBS);
        T_CHECK(strstr(atlas_buf_cstr(&out), want) != NULL);
    }
    const char *s = atlas_buf_cstr(&out);
    T_CHECK(strstr(s, "\"usage_model\":\"a-model\"") != NULL);
    T_CHECK(strstr(s, "\"usage_cost_micro_usd\":1250000") != NULL);
    T_CHECK(strstr(s, "\"usage_turns\":7") != NULL);
    /* The sibling: no job, no run, no measurement. Absent keys, and deliberately
     * no empty-string spelling of any of them — an empty identifier reads like
     * an identifier, and a cost of zero reads like a price. */
    T_CHECK_MSG(strstr(s, "\"job\":\"\"") == NULL, "an empty job identifier was emitted: %s", s);
    T_CHECK_MSG(strstr(s, "\"run\":\"\"") == NULL, "an empty run identifier was emitted: %s", s);
    T_CHECK_MSG(strstr(s, "\"usage_cost_micro_usd\":0") == NULL,
                "an unmeasured task was reported as free: %s", s);
    T_CHECK_MSG(strstr(s, "\"busy\"") == NULL, "busy was emitted when nothing was busy: %s", s);
    T_CHECK_MSG(strstr(s, "\"content\"") == NULL, "a document was emitted unasked: %s", s);
    atlas_buf_free(&out);
}

/* `plan show P --rev N`: the revision's own bytes, labelled exactly as
 * `job.artifact` labels the artifact they came from — which is where they came
 * from — and printed as they arrived. */
static void test_a_revision_document_is_labelled_and_printed_as_is(void) {
    atlas_plan_render pr;
    fill_example(&pr);
    pr.content = "atlas-plan-1%0Astage: 1%0A";
    pr.content_rev_no = 1;

    atlas_buf out = ATLAS_BUF_INIT;
    render(&pr, false, false, &out);
    T_CHECK(strstr(atlas_buf_cstr(&out),
                   "plan document r1 (untrusted, atlas-safe-1)\n  atlas-plan-1%0Astage: 1%0A\n") !=
            NULL);
    atlas_buf_free(&out);

    render(&pr, true, false, &out);
    const char *s = atlas_buf_cstr(&out);
    T_CHECK(strstr(s, "\"content_rev_no\":1") != NULL);
    T_CHECK(strstr(s, "\"content_encoding\":\"atlas-safe-1\"") != NULL);
    T_CHECK(strstr(s, "\"content_provenance\":\"UNTRUSTED_DATA\"") != NULL);
    T_CHECK(strstr(s, "\"content\":\"atlas-plan-1%0Astage: 1%0A\"") != NULL);
    atlas_buf_free(&out);
}

/* A busy invocation is neither an acceptance nor a refusal, and both renderers
 * have to say so without saying anything about what the plan *is*. */
static void test_busy_is_reported_and_is_not_a_verdict(void) {
    atlas_plan_render pr;
    fill_example(&pr);
    pr.busy = true;

    atlas_buf out = ATLAS_BUF_INIT;
    render(&pr, false, false, &out);
    T_CHECK(strstr(atlas_buf_cstr(&out), "took nothing") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&out), "run again") != NULL);
    /* The status is still the plan's own, unchanged by the invocation. */
    T_CHECK(strstr(atlas_buf_cstr(&out), "status        EXECUTING\n") != NULL);
    atlas_buf_free(&out);

    render(&pr, true, false, &out);
    T_CHECK(strstr(atlas_buf_cstr(&out), "\"busy\":true") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&out), "\"status\":\"EXECUTING\"") != NULL);
    atlas_buf_free(&out);
}

/* A list row is four columns and, in JSON, an anonymous object inside `plans`
 * carrying four members and no more: the wide part of a plan belongs to the
 * detail view, and a page of them is not the place for eight prompts. */
static void test_a_list_row_is_shallow_in_both_forms(void) {
    atlas_plan_render pr;
    fill_example(&pr);
    pr.detail = false;
    pr.in_list = true;

    atlas_buf out = ATLAS_BUF_INIT;
    render(&pr, false, true, &out);
    const char *s = atlas_buf_cstr(&out);
    T_CHECK_MSG(strstr(s, "p0123456789abcdef0123456789abcdef") != NULL, "the row is missing:\n%s",
                s);
    T_CHECK_MSG(strstr(s, "EXECUTING") != NULL, "the status is missing:\n%s", s);
    T_CHECK_MSG(strstr(s, "goal") == NULL, "a list row printed the goal:\n%s", s);
    T_CHECK_MSG(strstr(s, "stage 1 task") == NULL, "a list row printed its tasks:\n%s", s);
    atlas_buf_free(&out);

    render(&pr, true, true, &out);
    size_t bad = 0;
    T_CHECK_MSG(tjson_valid(out.data, out.len, &bad), "not valid JSON at %zu: %s", bad,
                atlas_buf_cstr(&out));
    s = atlas_buf_cstr(&out);
    T_CHECK(strstr(s, "\"plans\":[{") != NULL);
    T_CHECK(strstr(s, "\"status\":\"EXECUTING\"") != NULL);
    T_CHECK_MSG(strstr(s, "\"goal\"") == NULL, "a list row carried the goal: %s", s);
    T_CHECK_MSG(strstr(s, "\"tasks\"") == NULL, "a list row carried its tasks: %s", s);
    atlas_buf_free(&out);
}

/* An unfilled result — a creation the daemon was too busy to take — must render
 * without printing an identifier nobody was given. */
static void test_a_plan_that_was_never_created_prints_no_identifier(void) {
    atlas_plan_render pr;
    memset(&pr, 0, sizeof(pr));
    pr.detail = true;
    pr.status = atlas_plan_status_name(ATLAS_PLAN_STATUS_UNKNOWN);
    pr.busy = true;

    atlas_buf out = ATLAS_BUF_INIT;
    render(&pr, false, false, &out);
    const char *s = atlas_buf_cstr(&out);
    T_CHECK_MSG(strstr(s, "plan          ") == NULL, "an empty plan identifier was printed:\n%s",
                s);
    T_CHECK(strstr(s, "status        UNKNOWN\n") != NULL);
    T_CHECK(strstr(s, "took nothing") != NULL);
    atlas_buf_free(&out);

    render(&pr, true, false, &out);
    size_t bad = 0;
    T_CHECK_MSG(tjson_valid(out.data, out.len, &bad), "not valid JSON at %zu: %s", bad,
                atlas_buf_cstr(&out));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"plan\":\"\"") == NULL,
                "an empty plan identifier was emitted: %s", atlas_buf_cstr(&out));
    atlas_buf_free(&out);
}

static const atlas_test TESTS[] = {
    {"the human form prints what arrived", test_the_human_form_prints_what_arrived},
    {"the JSON form mirrors it", test_the_json_form_mirrors_it},
    {"a revision document is labelled and printed as-is",
     test_a_revision_document_is_labelled_and_printed_as_is},
    {"busy is reported and is not a verdict", test_busy_is_reported_and_is_not_a_verdict},
    {"a list row is shallow in both forms", test_a_list_row_is_shallow_in_both_forms},
    {"a plan that was never created prints no identifier",
     test_a_plan_that_was_never_created_prints_no_identifier},
};

ATLAS_TEST_MAIN("plan_render", TESTS)
