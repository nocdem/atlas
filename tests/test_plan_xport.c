/* Atlas - A12.0: the production plan transport's response readers.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * `src/core/service_plan.c` is the client half of the plan driver's seam: in the
 * shipped binary every transport member is a socket round trip, and what it does
 * with the answer is what the loop above it depends on absolutely.
 *
 * Three obligations live here and nowhere else, and all three are silent when
 * broken — the loop cannot tell a decoded goal from an encoded one, and neither
 * end of a corrupted gate list disagrees with itself:
 *
 *   **The single `atlas-safe-1` decode.** The daemon encodes a goal, a gate
 *   floor block, a title and a prompt; these readers decode each exactly once,
 *   so the driver never decodes and never re-encodes. A second decode would
 *   corrupt any of them containing a literal per cent; a missing one would show
 *   a planner the escapes instead of the words.
 *
 *   **The merged gate list carried verbatim.** It is Atlas' own canonical
 *   netstring, not safe text. It must reach `job.submit` byte for byte, because
 *   it is what an accepted stage was gated on, and a re-merge here would be a
 *   second answer to that.
 *
 *   **The conservative value for every absent key.** An absent or unrecognised
 *   status stays UNKNOWN, an absent job is an empty uid, and none of it is an
 *   error — A9.2.5's rule, which is what lets an older daemon answer a newer CLI.
 *
 * ## Why the readers rather than the calls
 *
 * A plan read is gated by `require_submitter`, which reads the **root-owned**
 * orchestration policy, and an unprivileged uid cannot create one anywhere —
 * that is the point of A7.1, and `tests/test_orch_model.c` proves the loader
 * refuses one this process could have written. So no fixture daemon this suite
 * can start will ever answer `plan.get`. The socket half is unreachable; the
 * half that can be wrong in a way nobody notices is not, and it is what this
 * drives, against documents parsed by the real response parser.
 */
#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/ipc.h"
#include "atlas/orch.h"
#include "atlas/plan.h"
#include "atlas/plandriver.h"
#include "atlas_test.h"
#include "core/service_internal.h"

/* Parses one daemon response document, in exactly the shape `atlas_ipc_call`
 * hands back. */
static atlas_ipc_response *parse(const char *doc) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_ipc_response *r = NULL;
    T_OK(atlas_ipc_response_parse(doc, strlen(doc), &r, &err), &err);
    T_REQUIRE(r != NULL);
    return r;
}

/* The plan row: a goal and a gate floor block, both safe-encoded on the wire.
 *
 * The goal carries a literal per cent and a newline, which are the two bytes
 * that separate "decoded once" from every other number of times: `%25` decodes
 * to `%`, and a renderer or a reader that decoded twice would turn it into the
 * start of an escape. */
static void test_the_plan_row_is_decoded_exactly_once(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_ipc_response *r = parse("{\"ok\":true,\"result\":{"
                                  "\"plan\":\"p0123456789abcdef0123456789abcdef\","
                                  "\"repo\":\"proj\",\"max_parallel\":2,"
                                  "\"goal\":\"ship the 100%25 case%0Aand stop\","
                                  "\"gate_floor_text\":\"make pass%0Amake test%0A\"}}");

    atlas_plandriver_plan p;
    atlas_plandriver_plan_init(&p);
    T_OK(atlas_plan_read_plan(r, &p, &err), &err);

    T_EQ_STR(p.plan_uid, "p0123456789abcdef0123456789abcdef");
    T_EQ_STR(p.repo, "proj");
    T_EQ_INT(p.max_parallel, 2);
    /* Raw bytes: the composers hand these to a worker, so an escape reaching one
     * would be Atlas showing a planner its own encoding. */
    T_EQ_STR(atlas_buf_cstr(&p.goal), "ship the 100% case\nand stop");
    T_EQ_STR(atlas_buf_cstr(&p.gate_floor_text), "make pass\nmake test\n");

    atlas_plandriver_plan_free(&p);
    atlas_ipc_response_free(r);
}

/* Every absent key leaves the caller's own value, and no absence is an error.
 * This is what an older daemon answering a newer CLI looks like. */
static void test_absent_keys_leave_the_conservative_value(void) {
    atlas_err err;
    atlas_err_init(&err);

    atlas_ipc_response *r = parse("{\"ok\":true,\"result\":{}}");
    atlas_plan_state st;
    memset(&st, 0, sizeof(st));
    T_OK(atlas_plan_read_state(r, &st, &err), &err);
    T_CHECK(st.status == ATLAS_PLAN_STATUS_UNKNOWN);
    T_EQ_INT(st.rev_no, 0);
    T_EQ_INT(st.planner_jobs_seen, 0);
    T_EQ_INT(st.task_count, 0);
    T_EQ_STR(st.planner_job_uid, "");
    T_CHECK(!st.replan_wanted);
    atlas_ipc_response_free(r);

    /* A status word this binary does not know is UNKNOWN too — never an error,
     * and never a guess. "UNKNOWN" itself is not in the parse table, so a daemon
     * presenting it reads the same way. */
    const char *unknowns[] = {"{\"ok\":true,\"result\":{\"status\":\"ASCENDANT\"}}",
                              "{\"ok\":true,\"result\":{\"status\":\"UNKNOWN\"}}"};
    for (size_t i = 0; i < sizeof unknowns / sizeof unknowns[0]; i++) {
        r = parse(unknowns[i]);
        memset(&st, 0, sizeof(st));
        st.status = ATLAS_PLAN_STATUS_UNKNOWN;
        T_OK(atlas_plan_read_state(r, &st, &err), &err);
        T_CHECK_MSG(st.status == ATLAS_PLAN_STATUS_UNKNOWN, "%s parsed to %s", unknowns[i],
                    atlas_plan_status_name(st.status));
        atlas_ipc_response_free(r);
    }

    /* And a status this binary does know parses, so the test above is not
     * passing because nothing parses at all. */
    r = parse("{\"ok\":true,\"result\":{\"status\":\"NEEDS_REPLAN\",\"replan_wanted\":true,"
              "\"rev_no\":2,\"planner_jobs_seen\":3,\"stages_accepted\":1,"
              "\"planner_job\":\"jaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
              "\"planner_job_state\":\"SUCCEEDED\"}}");
    memset(&st, 0, sizeof(st));
    T_OK(atlas_plan_read_state(r, &st, &err), &err);
    T_CHECK(st.status == ATLAS_PLAN_STATUS_NEEDS_REPLAN);
    T_CHECK(st.replan_wanted);
    T_EQ_INT(st.rev_no, 2);
    T_EQ_INT(st.planner_jobs_seen, 3);
    T_EQ_INT(st.stages_accepted, 1);
    T_EQ_STR(st.planner_job_uid, "jaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    T_CHECK(st.planner_job_state == ATLAS_ORCH_STATE_SUCCEEDED);
    atlas_ipc_response_free(r);
}

/* A task's tasks array, including the titles the replan composer states
 * completed work from. A task nobody submitted has an empty uid and UNKNOWN
 * state, which is what the struct documents and is never read as "finished". */
static void test_the_task_array_is_read_whole(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_ipc_response *r =
        parse("{\"ok\":true,\"result\":{\"status\":\"EXECUTING\",\"rev_no\":1,\"tasks\":["
              "{\"key\":\"build\",\"stage\":1,\"kind\":\"TREE\",\"title\":\"Build the 50%25 bit\","
              "\"job\":\"jaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"job_state\":\"RUNNING\","
              "\"run\":\"rbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"run_status\":\"ACTIVE\"},"
              "{\"key\":\"notes\",\"stage\":1,\"kind\":\"SIDE\",\"title\":\"Take notes\"}]}}");
    atlas_plan_state st;
    memset(&st, 0, sizeof(st));
    T_OK(atlas_plan_read_state(r, &st, &err), &err);

    T_EQ_INT(st.task_count, 2);
    T_EQ_STR(st.tasks[0].task_key, "build");
    T_EQ_INT(st.tasks[0].stage_no, 1);
    T_CHECK(st.tasks[0].is_tree);
    /* Decoded once: a planner's words, not its encoding. */
    T_EQ_STR(st.tasks[0].title, "Build the 50% bit");
    T_EQ_STR(st.tasks[0].job_uid, "jaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    T_CHECK(st.tasks[0].job_state == ATLAS_ORCH_STATE_RUNNING);
    T_EQ_STR(st.tasks[0].run_uid, "rbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    T_CHECK(st.tasks[0].run_status == ATLAS_ORCH_RUN_ACTIVE);

    T_EQ_STR(st.tasks[1].task_key, "notes");
    T_CHECK(!st.tasks[1].is_tree);
    T_EQ_STR(st.tasks[1].job_uid, "");
    T_CHECK(st.tasks[1].job_state == ATLAS_ORCH_STATE_UNKNOWN);
    T_EQ_STR(st.tasks[1].run_uid, "");
    atlas_ipc_response_free(r);
}

/* The claim the whole seam rests on: the merged gate list survives the wire
 * unchanged, and the prompt beside it does not.
 *
 * The expectation is built with the same encoder the plan write point merged
 * with, never written out, so this cannot pass by agreeing with a respelling of
 * the format. */
static void test_a_tasks_gates_survive_verbatim_and_its_prompt_is_decoded(void) {
    atlas_err err;
    atlas_err_init(&err);

    atlas_orch_argv merged[2];
    atlas_orch_argv_init(&merged[0]);
    atlas_orch_argv_init(&merged[1]);
    T_OK(atlas_orch_gate_split("make pass", &merged[0], &err), &err);
    /* An argument holding a per cent escape, deliberately: `%41` is exactly the
     * sequence a stray safe-text decode would silently rewrite to `A`. A gate
     * list of ordinary words survives a wrong decode unchanged, so a test built
     * only from those would pass whether or not the rule holds. `%` is printable
     * ASCII, so `atlas_orch_argv_push` accepts it and a real operator could
     * type it. */
    T_OK(atlas_orch_gate_split("make cover-%41", &merged[1], &err), &err);
    atlas_buf enc = ATLAS_BUF_INIT;
    T_OK(atlas_orch_validations_encode(merged, 2u, &enc, &err), &err);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&enc), "%41") != NULL,
                "the fixture's gate list lost its escape: %s", atlas_buf_cstr(&enc));

    /* `%%` throughout: this is a printf format, and the document it produces is
     * the one the daemon would have written — `%25` for a literal per cent in a
     * prompt, `%0A` for a newline. */
    atlas_buf doc = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&doc, &err,
                           "{\"ok\":true,\"result\":{\"rev_no\":1,\"tasks\":["
                           "{\"key\":\"build\",\"stage\":2,\"kind\":\"TREE\","
                           "\"title\":\"Build it\",\"prompt_encoding\":\"atlas-safe-1\","
                           "\"prompt\":\"do 100%%25 of it%%0Aplease\","
                           "\"validations\":\"%s\"},"
                           "{\"key\":\"notes\",\"stage\":2,\"kind\":\"SIDE\","
                           "\"title\":\"Notes\",\"prompt\":\"write it down%%0A\"}]}}",
                           atlas_buf_cstr(&enc)),
         &err);
    atlas_ipc_response *r = parse(atlas_buf_cstr(&doc));

    atlas_plandriver_task t;
    atlas_plandriver_task_init(&t);
    T_OK(atlas_plan_read_task(r, "p1", 1, "build", &t, &err), &err);
    T_EQ_STR(t.task_key, "build");
    T_EQ_INT(t.stage_no, 2);
    T_CHECK(t.is_tree);
    T_EQ_STR(t.title, "Build it");
    /* Raw bytes for the worker. */
    T_EQ_STR(atlas_buf_cstr(&t.prompt), "do 100% of it\nplease");
    /* Byte for byte. Not decoded, not re-merged, not re-encoded. */
    T_CHECK_MSG(t.validations.len == enc.len &&
                    memcmp(t.validations.data, enc.data, enc.len) == 0,
                "the merged gate list came back as \"%s\", not \"%s\"",
                atlas_buf_cstr(&t.validations), atlas_buf_cstr(&enc));
    atlas_plandriver_task_free(&t);

    /* A workspace sibling declares no gate: an empty list, which is what the
     * driver submits as "". */
    atlas_plandriver_task side;
    atlas_plandriver_task_init(&side);
    T_OK(atlas_plan_read_task(r, "p1", 1, "notes", &side, &err), &err);
    T_CHECK(!side.is_tree);
    T_EQ_INT((int)side.validations.len, 0);
    T_EQ_STR(atlas_buf_cstr(&side.prompt), "write it down\n");
    atlas_plandriver_task_free(&side);

    atlas_ipc_response_free(r);
    atlas_buf_free(&doc);
    atlas_buf_free(&enc);
    atlas_orch_argv_free(&merged[0]);
    atlas_orch_argv_free(&merged[1]);
}

/* `plan.get` serves the *latest* revision's tasks, whatever revision a caller
 * had in mind. A driver resumed after a replan compiled would otherwise submit a
 * superseded revision's stage — work the plan no longer holds — under the
 * correlation of the revision it thought it was on. Refused, not served. */
static void test_a_superseded_revision_is_refused_rather_than_served(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_ipc_response *r =
        parse("{\"ok\":true,\"result\":{\"rev_no\":2,\"tasks\":["
              "{\"key\":\"build\",\"stage\":1,\"kind\":\"TREE\",\"title\":\"t\","
              "\"prompt\":\"p\"}]}}");

    atlas_plandriver_task t;
    atlas_plandriver_task_init(&t);
    T_FAILS_WITH(atlas_plan_read_task(r, "p1", 1, "build", &t, &err), ATLAS_ERR_INTEGRITY, &err);
    T_CHECK_MSG(strstr(atlas_err_msg(&err), "now holds revision 2") != NULL, "it said: %s",
                atlas_err_msg(&err));
    atlas_plandriver_task_free(&t);

    /* And the revision that *is* current serves. */
    atlas_err_init(&err);
    atlas_plandriver_task ok_task;
    atlas_plandriver_task_init(&ok_task);
    T_OK(atlas_plan_read_task(r, "p1", 2, "build", &ok_task, &err), &err);
    T_EQ_STR(ok_task.task_key, "build");
    atlas_plandriver_task_free(&ok_task);

    /* A key the revision does not hold is a refusal, never a half-filled row. */
    atlas_err_init(&err);
    atlas_plandriver_task missing;
    atlas_plandriver_task_init(&missing);
    T_FAILS_WITH(atlas_plan_read_task(r, "p1", 2, "nosuch", &missing, &err), ATLAS_ERR_INTEGRITY,
                 &err);
    T_EQ_STR(missing.task_key, "");
    atlas_plandriver_task_free(&missing);

    atlas_ipc_response_free(r);
}

static const atlas_test TESTS[] = {
    {"the plan row is decoded exactly once", test_the_plan_row_is_decoded_exactly_once},
    {"absent keys leave the conservative value", test_absent_keys_leave_the_conservative_value},
    {"the task array is read whole", test_the_task_array_is_read_whole},
    {"a task's gates survive verbatim and its prompt is decoded",
     test_a_tasks_gates_survive_verbatim_and_its_prompt_is_decoded},
    {"a superseded revision is refused rather than served",
     test_a_superseded_revision_is_refused_rather_than_served},
};

ATLAS_TEST_MAIN("plan_xport", TESTS)
