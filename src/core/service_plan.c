/* Atlas - A12.0: the plan domain's four calls, as a client makes them.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Every call here speaks to the daemon over the socket through
 * `atlas_service_orch_call`, and there is deliberately **no offline path**. Plan
 * state lives in the index, `atlasd` is the only writer of it, and a CLI that
 * fell back to opening the database itself would be a second writer — the one
 * thing A1 forbids, and something A7.1 makes impossible anyway because no other
 * account can open the file. A client that cannot reach the daemon is told so
 * and stops.
 *
 * This file formats nothing and decides nothing. It does not derive a plan's
 * status — the daemon does that on every read, from stored rows, and a second
 * derivation here would be a second answer that could disagree with it. It does
 * not compose a prompt, submit a job or settle anything: the plan driver and the
 * renderers are later, and both are built on these calls rather than beside
 * them, so the binary holds one spelling of each request.
 *
 * The one piece of interpretation is in `atlas_plan_wire_revision_add`, and it
 * is a *separation* rather than a decision: a refusal that came from a planner's
 * document carries a sentence and a line, and they are lifted out of the error
 * document into typed members so that no caller has to read Atlas' prose to
 * recover a number.
 */
#define _GNU_SOURCE 1

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/dispatch.h"
#include "atlas/driver.h"
#include "atlas/ipc.h"
#include "atlas/orch.h"
#include "atlas/orchpolicy.h"
#include "atlas/plan.h"
#include "atlas/plandriver.h"
#include "atlas/rundriver.h"
#include "atlas/safetext.h"
#include "atlas/service.h"
#include "atlas/syspolicy.h"
#include "service_internal.h"

/* --- plan.create ------------------------------------------------------------- */

static atlas_status build_create(atlas_json *j, void *ud, atlas_err *err) {
    const atlas_plan_create_opts *o = (const atlas_plan_create_opts *)ud;
    atlas_status st = atlas_json_key_str(j, "repo", o->repo, err);
    if (st == ATLAS_OK) {
        /* The operator's own words, sent verbatim. They are never concatenated
         * into anything and the daemon stores them as they arrived. */
        st = atlas_json_key_str(j, "goal", o->goal, err);
    }
    if (st == ATLAS_OK && o->max_parallel > 0) {
        /* Zero is not sent: it is the same request as not asking, and the daemon
         * resolves an absent bound to its own default. */
        st = atlas_json_key_int(j, "parallel", o->max_parallel, err);
    }
    /* The gate floor, each command sent as a length-prefixed argv encoding rather
     * than as a command line, so no element of one can be confused with a
     * separator whatever it contains. The split happens here, on ASCII spaces,
     * through the one splitter — `atlas_orch_gate_split`, which is also what the
     * `atlas-plan-1` parser splits a planner's `gate:` line with. No shell is
     * involved at any point. */
    if (st == ATLAS_OK && o->gate_count > 0) {
        st = atlas_json_key(j, "gate_floor", err);
        if (st == ATLAS_OK) {
            st = atlas_json_arr_begin(j, err);
        }
        for (size_t i = 0; st == ATLAS_OK && i < o->gate_count; i++) {
            atlas_orch_argv one;
            atlas_orch_argv_init(&one);
            st = atlas_orch_gate_split(o->gates[i], &one, err);
            atlas_buf enc = ATLAS_BUF_INIT;
            if (st == ATLAS_OK) {
                st = atlas_orch_validations_encode(&one, 1u, &enc, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_str(j, atlas_buf_cstr(&enc), err);
            }
            atlas_buf_free(&enc);
            atlas_orch_argv_free(&one);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_arr_end(j, err);
        }
    }
    return st;
}

atlas_status atlas_plan_wire_create(atlas_ctx *ctx, const atlas_plan_create_opts *o,
                                    atlas_buf *plan_uid_out, atlas_err *err) {
    if (o->repo == NULL || o->repo[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "which repository?");
    }
    if (o->goal == NULL || o->goal[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a plan needs a goal");
    }
    if (o->gate_count == 0) {
        /* The same refusal the daemon gives, given here too so an operator who
         * typed no gate learns it without a round trip. It is stated in the same
         * words deliberately: two spellings of one rule read as two rules. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a plan needs at least one gate; the operator brings the gate floor "
                             "and the planner may only add to it");
    }
    if (o->gate_count > ATLAS_ORCH_MAX_VALIDATIONS) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a plan's gate floor holds at most %u commands",
                             (unsigned)ATLAS_ORCH_MAX_VALIDATIONS);
    }
    /* Copied rather than const-cast: a cast that discards a qualifier is right
     * today and wrong after one edit. */
    atlas_plan_create_opts local = *o;
    atlas_ipc_response *resp = NULL;
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_status st =
        atlas_service_orch_call(ctx, "plan.create", build_create, &local, &resp, &raw, err);
    if (st == ATLAS_OK && plan_uid_out != NULL) {
        const char *uid = NULL;
        if (!atlas_ipc_result_str(resp, "plan", &uid) || uid == NULL) {
            st = atlas_err_set(err, ATLAS_ERR_INTERNAL,
                               "the Atlas daemon created a plan and did not name it");
        } else {
            st = atlas_buf_set_str(plan_uid_out, uid, err);
        }
    }
    atlas_ipc_response_free(resp);
    atlas_buf_free(&raw);
    return st;
}

/* --- plan.revision_add -------------------------------------------------------- */

static atlas_status build_revision(atlas_json *j, void *ud, atlas_err *err) {
    const atlas_plan_revision_opts *o = (const atlas_plan_revision_opts *)ud;
    atlas_status st = atlas_json_key_str(j, "plan", o->plan, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "planner_job", o->planner_job, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "reason", o->reason, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "rev_no", o->rev_no, err);
    }
    return st;
}

atlas_status atlas_plan_wire_revision_add(atlas_ctx *ctx, const atlas_plan_revision_opts *o,
                                          int *rev_no_out, atlas_buf *refusal_out,
                                          int *line_out, atlas_err *err) {
    if (o->plan == NULL || o->plan[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "which plan?");
    }
    if (o->planner_job == NULL || o->planner_job[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "which planner job?");
    }
    if (o->reason == NULL || o->reason[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a revision records why it exists");
    }
    atlas_plan_revision_opts local = *o;
    atlas_ipc_response *resp = NULL;
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_status st =
        atlas_service_orch_call(ctx, "plan.revision_add", build_revision, &local, &resp, &raw, err);
    if (st == ATLAS_OK) {
        int64_t rev = 0;
        if (rev_no_out != NULL && atlas_ipc_result_int(resp, "rev_no", &rev)) {
            *rev_no_out = (int)rev;
        }
    } else {
        /* A refusal that came from the document, lifted out of the error
         * document's `detail` object. Every other refusal carries none, and then
         * both of these are left exactly as the caller left them — which is what
         * makes a non-empty `refusal_out` the discriminator. */
        const char *sentence = NULL;
        int64_t line = 0;
        if (refusal_out != NULL && atlas_ipc_error_detail_str(resp, "refusal", &sentence) &&
            sentence != NULL) {
            atlas_err scratch;
            atlas_err_init(&scratch);
            /* The refusal must not overwrite the refusal: if the copy fails there
             * is nothing better to say than what `err` already says. */
            (void)atlas_buf_set_str(refusal_out, sentence, &scratch);
        }
        if (line_out != NULL && atlas_ipc_error_detail_int(resp, "line", &line)) {
            *line_out = (int)line;
        }
    }
    atlas_ipc_response_free(resp);
    atlas_buf_free(&raw);
    return st;
}

/* --- plan.get and plan.list ---------------------------------------------------
 *
 * Both hand the parsed response back. See `atlas/service.h`: a plan is a wide
 * document and the shape a renderer wants is the renderer's question.
 */

typedef struct plan_get_args {
    const char *plan;
    int rev_no;
    bool task_detail;
} plan_get_args;

static atlas_status build_get(atlas_json *j, void *ud, atlas_err *err) {
    const plan_get_args *a = (const plan_get_args *)ud;
    atlas_status st = atlas_json_key_str(j, "plan", a->plan, err);
    if (st == ATLAS_OK && a->rev_no > 0) {
        /* Absent means "the state, without any document". A revision number is
         * what asks for one. */
        st = atlas_json_key_int(j, "rev_no", a->rev_no, err);
    }
    if (st == ATLAS_OK && a->task_detail) {
        /* Sent only when asked for, so an operator's read carries no prompts and
         * the daemon does eight fewer queries. False is the same request as not
         * asking, so it is not sent. */
        st = atlas_json_key_bool(j, "task_detail", true, err);
    }
    return st;
}

atlas_status atlas_plan_wire_get(atlas_ctx *ctx, const char *plan, int rev_no, bool task_detail,
                                 atlas_ipc_response **out, atlas_buf *raw, atlas_err *err) {
    *out = NULL;
    if (plan == NULL || plan[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "which plan?");
    }
    plan_get_args a = {plan, rev_no, task_detail};
    return atlas_service_orch_call(ctx, "plan.get", build_get, &a, out, raw, err);
}

typedef struct plan_list_args {
    int64_t after;
    int64_t limit;
} plan_list_args;

static atlas_status build_list(atlas_json *j, void *ud, atlas_err *err) {
    const plan_list_args *a = (const plan_list_args *)ud;
    atlas_status st = ATLAS_OK;
    if (a->after > 0) {
        st = atlas_json_key_int(j, "after", a->after, err);
    }
    if (st == ATLAS_OK && a->limit > 0) {
        st = atlas_json_key_int(j, "limit", a->limit, err);
    }
    return st;
}

atlas_status atlas_plan_wire_list(atlas_ctx *ctx, int64_t after, int64_t limit,
                                  atlas_ipc_response **out, atlas_buf *raw, atlas_err *err) {
    *out = NULL;
    plan_list_args a = {after, limit};
    return atlas_service_orch_call(ctx, "plan.list", build_list, &a, out, raw, err);
}

/* ===========================================================================
 * A12.0 T7: the production plan-driver transport, and the four commands.
 *
 * `atlas/plandriver.h` describes this seam and why it exists: in the shipped
 * binary every member is a socket round trip, because plan and orchestration
 * state live in the index and `atlasd` is its only writer. What is abstracted is
 * the *carriage* of a call and never its validation — what the IPC edge refuses,
 * it still refuses.
 *
 * Three obligations this file carries and nothing below it does:
 *
 *   **The single `atlas-safe-1` decode happens here.** The daemon encodes a
 *   goal, a gate floor block, a title and a prompt on the wire; these fill the
 *   driver's structs with raw bytes, and the loop never decodes and never
 *   re-encodes. A decode on the far side of the seam would corrupt a goal
 *   containing a literal `%` the moment a fixture handed over raw bytes.
 *
 *   **The merged gate list is carried opaquely.** It arrives as Atlas' own
 *   canonical netstring, travels through `atlas_plandriver_task.validations`
 *   untouched, and is re-encoded onto `job.submit` through the *same*
 *   encode/decode pair `job.submit` already parses with. It is never split,
 *   never merged and never displayed: the merge is the plan write point's, it
 *   happened once when the revision compiled, and a second implementation here
 *   would be a second answer to what an accepted stage was gated on.
 *
 *   **`BUSY:` is a report, not an invocation error.** The daemon's promise that
 *   it took nothing is recorded on the transport and answered with `ATLAS_OK`,
 *   so the loop stops cleanly on a plan that is untouched and resumable rather
 *   than reporting a failure of Atlas. A11.1's contract, one layer up.
 * ======================================================================== */

typedef struct plan_xport {
    /* Resolved once. Every member that reaches the daemon by hand — the two
     * drives — uses this, so one process holds one answer to where the daemon
     * listens. */
    atlas_buf sock;
    /* The caller's, loaded from its compiled-in root-owned path and already
     * checked ENABLED. Outlives this synchronous call. */
    const atlas_orchpolicy *pol;
    /* This uid's driver partition, derived by the one derivation. A repo-tree
     * driver is never on it — the plan driver reaches a repo-tree task through
     * the run driver and never through a lease of its own. */
    char filter[256];
    bool have_filter;
    /* The daemon said `BUSY:` to something. Not a verdict about the plan: it is
     * carried out to the report so an operator is told the invocation may simply
     * be repeated. */
    bool saw_busy;
    FILE *log;
} plan_xport;

static void x_say(plan_xport *x, const char *fmt, ...) {
    if (x->log == NULL) {
        return;
    }
    char at[ATLAS_TS_MAX];
    atlas_now_iso8601(at, sizeof(at));
    va_list ap;
    va_start(ap, fmt);
    (void)fprintf(x->log, "%s plan ", at);
    (void)vfprintf(x->log, fmt, ap);
    (void)fputc('\n', x->log);
    va_end(ap);
    (void)fflush(x->log);
}

/* The single decode, into an owned buffer. An absent key is empty rather than an
 * error: A9.2.5's rule for a key a daemon did not send, and the conservative
 * value on every one of these. */
static atlas_status take_decoded(const char *encoded, atlas_buf *out, atlas_err *err) {
    if (encoded == NULL) {
        return atlas_buf_set(out, "", 0u, err);
    }
    return atlas_text_decode_safe(encoded, strlen(encoded), out, err);
}

/* The same, into a fixed field. Decoding never lengthens, so the bound is only
 * reached by a value the daemon should not have sent; it is truncated at the
 * field rather than refused, because a title is display text and losing a plan
 * over one would be the worse failure. */
static atlas_status take_decoded_fixed(const char *encoded, char *out, size_t out_size,
                                       atlas_err *err) {
    atlas_buf tmp = ATLAS_BUF_INIT;
    atlas_status st = take_decoded(encoded, &tmp, err);
    if (st == ATLAS_OK) {
        size_t n = tmp.len < out_size - 1u ? tmp.len : out_size - 1u;
        if (n > 0) {
            memcpy(out, tmp.data, n);
        }
        out[n] = '\0';
    }
    atlas_buf_free(&tmp);
    return st;
}

/* --- plan.create ------------------------------------------------------------ */

static atlas_status x_plan_create(void *ctx, const atlas_plan_create_req *req,
                                  char plan_uid_out[ATLAS_ORCH_UID_MAX], atlas_err *err) {
    plan_xport *x = (plan_xport *)ctx;
    (void)x;
    plan_uid_out[0] = '\0';
    if (req->gate_count > 8u) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a plan's gate floor holds at most %u commands",
                             (unsigned)ATLAS_ORCH_MAX_VALIDATIONS);
    }
    atlas_plan_create_opts o;
    memset(&o, 0, sizeof(o));
    o.repo = req->repo;
    o.goal = req->goal;
    o.max_parallel = req->max_parallel;
    for (size_t i = 0; i < req->gate_count; i++) {
        o.gates[o.gate_count++] = req->gates[i];
    }
    atlas_buf uid = ATLAS_BUF_INIT;
    atlas_status st = atlas_plan_wire_create(NULL, &o, &uid, err);
    if (st == ATLAS_OK) {
        (void)snprintf(plan_uid_out, ATLAS_ORCH_UID_MAX, "%s", atlas_buf_cstr(&uid));
    }
    atlas_buf_free(&uid);
    return st;
}

/* --- plan.get, in its two shapes -------------------------------------------- */

atlas_status atlas_plan_read_plan(const atlas_ipc_response *r, atlas_plandriver_plan *out,
                                  atlas_err *err) {
    const char *v = NULL;
    if (atlas_ipc_result_str(r, "plan", &v) && v != NULL) {
        (void)snprintf(out->plan_uid, sizeof out->plan_uid, "%s", v);
    }
    if (atlas_ipc_result_str(r, "repo", &v) && v != NULL) {
        (void)snprintf(out->repo, sizeof out->repo, "%s", v);
    }
    int64_t n = 0;
    if (atlas_ipc_result_int(r, "max_parallel", &n)) {
        out->max_parallel = (int)n;
    }
    /* The two the composers read, decoded once, here. */
    v = NULL;
    (void)atlas_ipc_result_str(r, "goal", &v);
    atlas_status st = take_decoded(v, &out->goal, err);
    if (st == ATLAS_OK) {
        v = NULL;
        (void)atlas_ipc_result_str(r, "gate_floor_text", &v);
        st = take_decoded(v, &out->gate_floor_text, err);
    }
    return st;
}

static atlas_status x_plan_get(void *ctx, const char *plan_uid, atlas_plandriver_plan *out,
                               atlas_err *err) {
    plan_xport *x = (plan_xport *)ctx;
    (void)x;
    atlas_ipc_response *r = NULL;
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_status st = atlas_plan_wire_get(NULL, plan_uid, 0, false, &r, &raw, err);
    if (st == ATLAS_OK) {
        st = atlas_plan_read_plan(r, out, err);
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return st;
}

atlas_status atlas_plan_read_state(const atlas_ipc_response *r, atlas_plan_state *out,
                                   atlas_err *err) {
    const char *v = NULL;
    atlas_status st = ATLAS_OK;
    if (atlas_ipc_result_str(r, "status", &v) && v != NULL) {
        (void)atlas_plan_status_parse(v, &out->status);
    }
    int64_t n = 0;
    if (atlas_ipc_result_int(r, "rev_no", &n)) {
        out->rev_no = (int)n;
    }
    if (atlas_ipc_result_int(r, "planner_jobs_seen", &n)) {
        out->planner_jobs_seen = (int)n;
    }
    if (atlas_ipc_result_int(r, "stages_accepted", &n)) {
        out->stages_accepted = (int)n;
    }
    (void)atlas_ipc_result_bool(r, "replan_wanted", &out->replan_wanted);
    if (atlas_ipc_result_str(r, "planner_job", &v) && v != NULL) {
        (void)snprintf(out->planner_job_uid, sizeof out->planner_job_uid, "%s", v);
    }
    if (atlas_ipc_result_str(r, "planner_job_state", &v) && v != NULL) {
        (void)atlas_orch_state_parse(v, &out->planner_job_state);
    }
    size_t count = 0;
    (void)atlas_ipc_result_arr_len(r, "tasks", &count);
    for (size_t i = 0; st == ATLAS_OK && i < count && out->task_count < ATLAS_PLAN_MAX_TASKS;
         i++) {
        atlas_plan_task_view *t = &out->tasks[out->task_count];
        if (!atlas_ipc_result_arr_obj_str(r, "tasks", i, "key", &v) || v == NULL) {
            continue;
        }
        (void)snprintf(t->task_key, sizeof t->task_key, "%s", v);
        if (atlas_ipc_result_arr_obj_int(r, "tasks", i, "stage", &n)) {
            t->stage_no = (int)n;
        }
        if (atlas_ipc_result_arr_obj_str(r, "tasks", i, "kind", &v) && v != NULL) {
            t->is_tree = strcmp(v, "TREE") == 0;
        }
        /* A planner's bytes, decoded here because the replan composer states
         * completed work from this field and would otherwise print the
         * encoding rather than the title. */
        v = NULL;
        (void)atlas_ipc_result_arr_obj_str(r, "tasks", i, "title", &v);
        st = take_decoded_fixed(v, t->title, sizeof t->title, err);
        if (st != ATLAS_OK) {
            break;
        }
        if (atlas_ipc_result_arr_obj_str(r, "tasks", i, "job", &v) && v != NULL) {
            (void)snprintf(t->job_uid, sizeof t->job_uid, "%s", v);
        }
        if (atlas_ipc_result_arr_obj_str(r, "tasks", i, "job_state", &v) && v != NULL) {
            (void)atlas_orch_state_parse(v, &t->job_state);
        }
        if (atlas_ipc_result_arr_obj_str(r, "tasks", i, "run", &v) && v != NULL) {
            (void)snprintf(t->run_uid, sizeof t->run_uid, "%s", v);
        }
        if (atlas_ipc_result_arr_obj_str(r, "tasks", i, "run_status", &v) && v != NULL) {
            (void)atlas_orch_run_status_parse(v, &t->run_status);
        }
        out->task_count++;
    }
    return st;
}

/* The derived state, as the daemon derived it. Nothing here re-derives a status:
 * a second derivation would be a second answer that could disagree about whether
 * a plan is finished. An unrecognised or absent status leaves UNKNOWN, which the
 * loop reports as a defect in Atlas rather than as an answer about this plan. */
static atlas_status x_plan_state(void *ctx, const char *plan_uid, atlas_plan_state *out,
                                 atlas_err *err) {
    plan_xport *x = (plan_xport *)ctx;
    (void)x;
    atlas_ipc_response *r = NULL;
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_status st = atlas_plan_wire_get(NULL, plan_uid, 0, false, &r, &raw, err);
    if (st == ATLAS_OK) {
        st = atlas_plan_read_state(r, out, err);
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return st;
}

/* One task row of one revision.
 *
 * `plan.get` serves the tasks of the *latest* revision, so a caller that asked
 * for an older one is answered about a different set. The revision the response
 * describes is compared against the one that was asked for and a mismatch is
 * refused rather than served: a resumed driver that submitted a stage from a
 * superseded revision would be running work the plan no longer holds. */
atlas_status atlas_plan_read_task(const atlas_ipc_response *r, const char *plan_uid, int rev_no,
                                  const char *task_key, atlas_plandriver_task *out,
                                  atlas_err *err) {
    atlas_status st = ATLAS_OK;
    int64_t have = 0;
    (void)atlas_ipc_result_int(r, "rev_no", &have);
    if ((int)have != rev_no) {
        st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                           "plan %s now holds revision %lld, so revision %d's task %s is no "
                           "longer what this plan is doing",
                           plan_uid, (long long)have, rev_no, task_key);
    }
    bool found = false;
    if (st == ATLAS_OK) {
        size_t count = 0;
        (void)atlas_ipc_result_arr_len(r, "tasks", &count);
        for (size_t i = 0; st == ATLAS_OK && i < count && !found; i++) {
            const char *v = NULL;
            if (!atlas_ipc_result_arr_obj_str(r, "tasks", i, "key", &v) || v == NULL ||
                strcmp(v, task_key) != 0) {
                continue;
            }
            found = true;
            (void)snprintf(out->task_key, sizeof out->task_key, "%s", v);
            int64_t n = 0;
            if (atlas_ipc_result_arr_obj_int(r, "tasks", i, "stage", &n)) {
                out->stage_no = (int)n;
            }
            if (atlas_ipc_result_arr_obj_str(r, "tasks", i, "kind", &v) && v != NULL) {
                out->is_tree = strcmp(v, "TREE") == 0;
            }
            v = NULL;
            (void)atlas_ipc_result_arr_obj_str(r, "tasks", i, "title", &v);
            st = take_decoded_fixed(v, out->title, sizeof out->title, err);
            if (st == ATLAS_OK) {
                v = NULL;
                (void)atlas_ipc_result_arr_obj_str(r, "tasks", i, "prompt", &v);
                st = take_decoded(v, &out->prompt, err);
            }
            /* Atlas' own canonical netstring. **Not decoded**: it is not safe
             * text, it is the encoding the plan write point merged into and the
             * one `job.submit` parses, and it must reach the submission byte for
             * byte. Absent for a side task, which declares no gate. */
            if (st == ATLAS_OK) {
                v = NULL;
                if (atlas_ipc_result_arr_obj_str(r, "tasks", i, "validations", &v) && v != NULL) {
                    st = atlas_buf_set_str(&out->validations, v, err);
                }
            }
        }
    }
    if (st == ATLAS_OK && !found) {
        st = atlas_err_set(err, ATLAS_ERR_INTEGRITY, "plan %s revision %d holds no task %s",
                           plan_uid, rev_no, task_key);
    }
    return st;
}

static atlas_status x_plan_task(void *ctx, const char *plan_uid, int rev_no, const char *task_key,
                                atlas_plandriver_task *out, atlas_err *err) {
    plan_xport *x = (plan_xport *)ctx;
    (void)x;
    atlas_ipc_response *r = NULL;
    atlas_buf raw = ATLAS_BUF_INIT;
    /* `task_detail` and no `rev_no`: the two members a submission needs, and not
     * the revision's whole document, which no caller on this path reads. */
    atlas_status st = atlas_plan_wire_get(NULL, plan_uid, 0, true, &r, &raw, err);
    if (st == ATLAS_OK) {
        st = atlas_plan_read_task(r, plan_uid, rev_no, task_key, out, err);
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return st;
}

/* --- plan.revision_add ------------------------------------------------------ */

static atlas_status x_plan_revision_add(void *ctx, const char *plan_uid, const char *planner_job,
                                        int rev_no, const char *reason, atlas_plan_refusal *ref,
                                        atlas_err *err) {
    plan_xport *x = (plan_xport *)ctx;
    (void)x;
    atlas_plan_revision_opts o;
    memset(&o, 0, sizeof(o));
    o.plan = plan_uid;
    o.planner_job = planner_job;
    o.reason = reason;
    o.rev_no = rev_no;
    atlas_buf sentence = ATLAS_BUF_INIT;
    int line = 0;
    atlas_status st = atlas_plan_wire_revision_add(NULL, &o, NULL, &sentence, &line, err);
    if (st != ATLAS_OK && sentence.len > 0 && ref != NULL) {
        /* A refusal the *document* earned. Every other refusal leaves `ref`
         * exactly as the caller left it, which is what makes a non-empty
         * sentence the discriminator. */
        (void)snprintf(ref->sentence, sizeof ref->sentence, "%s", atlas_buf_cstr(&sentence));
        ref->line = line;
    }
    atlas_buf_free(&sentence);
    return st;
}

/* --- job.submit ------------------------------------------------------------- */

typedef struct plan_submit_args {
    const atlas_plan_job_req *req;
    /* The merged list, decoded once into argv vectors so each can be re-encoded
     * as the single-command wire element `job.submit` parses. The round trip is
     * through the same encode/decode pair on both sides, so it is byte-exact. */
    atlas_orch_argv gates[ATLAS_ORCH_MAX_VALIDATIONS];
    size_t gate_count;
} plan_submit_args;

static atlas_status build_plan_submit(atlas_json *j, void *ud, atlas_err *err) {
    plan_submit_args *a = (plan_submit_args *)ud;
    const atlas_plan_job_req *q = a->req;
    atlas_status st = atlas_json_key_str(j, "repo", q->repo, err);
    if (st == ATLAS_OK) {
        /* The composed prompt, raw bytes, sent verbatim. The daemon stores it as
         * UNTRUSTED_DATA and it is never concatenated into anything here. */
        st = atlas_json_key_str(j, "task", q->task, err);
    }
    const struct {
        const char *k;
        const char *v;
    } opt[] = {
        {"driver", q->driver},
        {"mode", q->mode},
        /* RULING 5. One builder-produced string, sent as both: without the
         * idempotency key a resumed driver creates a second job for one slot,
         * and without the correlation the derived reader cannot find the job at
         * all — it reads jobs only by correlation. */
        {"correlation", q->correlation},
        {"idempotency_key", q->correlation},
        {"parent", q->parent_job_uid},
    };
    for (size_t i = 0; st == ATLAS_OK && i < sizeof opt / sizeof opt[0]; i++) {
        if (opt[i].v != NULL && opt[i].v[0] != '\0') {
            st = atlas_json_key_str(j, opt[i].k, opt[i].v, err);
        }
    }
    const struct {
        const char *k;
        int64_t v;
    } nums[] = {
        {"max_attempts", q->max_attempts},
        {"wall_timeout_ms", q->wall_timeout_ms},
        {"idle_timeout_ms", q->idle_timeout_ms},
        /* Root submissions only; zero is the same request as not asking, and
         * naming it on a child is refused at the write point. */
        {"parallel", q->run_max_parallel},
    };
    for (size_t i = 0; st == ATLAS_OK && i < sizeof nums / sizeof nums[0]; i++) {
        if (nums[i].v > 0) {
            st = atlas_json_key_int(j, nums[i].k, nums[i].v, err);
        }
    }
    if (st == ATLAS_OK && a->gate_count > 0) {
        st = atlas_json_key(j, "validation", err);
        if (st == ATLAS_OK) {
            st = atlas_json_arr_begin(j, err);
        }
        for (size_t i = 0; st == ATLAS_OK && i < a->gate_count; i++) {
            atlas_buf enc = ATLAS_BUF_INIT;
            st = atlas_orch_validations_encode(&a->gates[i], 1u, &enc, err);
            if (st == ATLAS_OK) {
                st = atlas_json_str(j, atlas_buf_cstr(&enc), err);
            }
            atlas_buf_free(&enc);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_arr_end(j, err);
        }
    }
    return st;
}

static atlas_status x_job_submit(void *ctx, const atlas_plan_job_req *req,
                                 char job_uid_out[ATLAS_ORCH_UID_MAX], atlas_err *err) {
    plan_xport *x = (plan_xport *)ctx;
    (void)x;
    job_uid_out[0] = '\0';
    plan_submit_args a;
    memset(&a, 0, sizeof(a));
    a.req = req;
    for (size_t i = 0; i < ATLAS_ORCH_MAX_VALIDATIONS; i++) {
        atlas_orch_argv_init(&a.gates[i]);
    }
    atlas_status st = ATLAS_OK;
    if (req->validations != NULL && req->validations[0] != '\0') {
        st = atlas_orch_validations_decode(req->validations, a.gates, ATLAS_ORCH_MAX_VALIDATIONS,
                                           &a.gate_count, err);
    }
    atlas_ipc_response *resp = NULL;
    atlas_buf raw = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_service_orch_call(NULL, "job.submit", build_plan_submit, &a, &resp, &raw, err);
    }
    if (st == ATLAS_OK) {
        const char *v = NULL;
        if (!atlas_ipc_result_str(resp, "job", &v) || v == NULL) {
            st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                               "the Atlas daemon accepted a task without naming it");
        } else {
            (void)snprintf(job_uid_out, ATLAS_ORCH_UID_MAX, "%s", v);
        }
    }
    atlas_ipc_response_free(resp);
    atlas_buf_free(&raw);
    for (size_t i = 0; i < ATLAS_ORCH_MAX_VALIDATIONS; i++) {
        atlas_orch_argv_free(&a.gates[i]);
    }
    return st;
}

/* --- job.get, plus the evidence a replan prompt is allowed to carry ---------- */

typedef struct one_job_args {
    const char *job;
    int64_t artifact_id;
    bool with_content;
} one_job_args;

static atlas_status build_one_job(atlas_json *j, void *ud, atlas_err *err) {
    const one_job_args *a = (const one_job_args *)ud;
    atlas_status st = atlas_json_key_str(j, "job", a->job, err);
    if (st == ATLAS_OK && a->artifact_id > 0) {
        st = atlas_json_key_int(j, "artifact", a->artifact_id, err);
    }
    if (st == ATLAS_OK && a->with_content) {
        st = atlas_json_key_bool(j, "content", true, err);
    }
    return st;
}

/* The gate output a replan prompt excerpts, from the job's own stored `gate.log`
 * artifact and from nowhere else.
 *
 * Best-effort by contract: `atlas_plan_compose_replan` omits the excerpt section
 * entirely when there is nothing to excerpt, so a job with no stored gate log —
 * one that exceeded the inline bound, or never ran a gate — leaves this empty
 * rather than making the replan fail. Anything that goes wrong on this path is
 * *cleared*, for the same reason: evidence Atlas could not read is evidence it
 * does not state, and it is never a reason to refuse the replan. */
static void take_gate_log(const char *job_uid, atlas_buf *out, atlas_err *err) {
    one_job_args list = {job_uid, 0, false};
    atlas_ipc_response *r = NULL;
    atlas_buf raw = ATLAS_BUF_INIT;
    int64_t id = 0;
    if (atlas_service_orch_call(NULL, "job.artifact", build_one_job, &list, &r, &raw, err) ==
        ATLAS_OK) {
        size_t n = 0;
        (void)atlas_ipc_result_arr_len(r, "artifacts", &n);
        for (size_t i = 0; i < n; i++) {
            const char *name = NULL;
            bool stored = false;
            if (!atlas_ipc_result_arr_obj_str(r, "artifacts", i, "name", &name) || name == NULL ||
                strcmp(name, "gate.log") != 0) {
                continue;
            }
            (void)atlas_ipc_result_arr_obj_bool(r, "artifacts", i, "stored", &stored);
            int64_t got = 0;
            if (stored && atlas_ipc_result_arr_obj_int(r, "artifacts", i, "id", &got)) {
                id = got;
            }
        }
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    atlas_err_init(err);
    if (id <= 0) {
        return;
    }
    one_job_args one = {job_uid, id, true};
    r = NULL;
    atlas_buf raw2 = ATLAS_BUF_INIT;
    if (atlas_service_orch_call(NULL, "job.artifact", build_one_job, &one, &r, &raw2, err) ==
        ATLAS_OK) {
        const char *content = NULL;
        if (atlas_ipc_result_arr_obj_str(r, "artifacts", 0, "content", &content) &&
            content != NULL) {
            /* Safe-encoded by the daemon; decoded once, here, so the composer
             * bounds and labels the worker's real bytes. */
            if (atlas_text_decode_safe(content, strlen(content), out, err) != ATLAS_OK) {
                atlas_buf_reset(out);
            }
        }
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw2);
    atlas_err_init(err);
}

static atlas_status x_job_get(void *ctx, const char *job_uid, atlas_plan_job_view *out,
                              atlas_err *err) {
    plan_xport *x = (plan_xport *)ctx;
    (void)x;
    one_job_args a = {job_uid, 0, false};
    atlas_ipc_response *r = NULL;
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_status st = atlas_service_orch_call(NULL, "job.get", build_one_job, &a, &r, &raw, err);
    if (st == ATLAS_OK) {
        const char *v = NULL;
        if (atlas_ipc_result_str(r, "state", &v) && v != NULL) {
            (void)atlas_orch_state_parse(v, &out->state);
        }
        if (atlas_ipc_result_str(r, "run", &v) && v != NULL) {
            (void)snprintf(out->run_uid, sizeof out->run_uid, "%s", v);
        }
        if (atlas_ipc_result_str(r, "run_status", &v) && v != NULL) {
            (void)atlas_orch_run_status_parse(v, &out->run_status);
        }
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    if (st != ATLAS_OK) {
        return st;
    }
    /* `failed_gate` stays empty: `job.get` exposes no failed-gate index, and
     * `atlas_plan_compose_replan` writes "(none recorded)" rather than naming a
     * gate nobody established. Naming one from anywhere else would be Atlas
     * inventing evidence. */
    take_gate_log(job_uid, &out->gate_output, err);
    return ATLAS_OK;
}

/* --- the two drives --------------------------------------------------------- */

/* One stage's run, through A11.1's run driver and the wiring `atlas job run`
 * uses. A BLOCKED run is an answer and is never turned into an error; a `BUSY`
 * is recorded on the transport and answered with OK, which is what makes the
 * loop stop cleanly on a plan that is untouched and resumable. */
static atlas_status x_drive_run(void *ctx, const char *run_uid, atlas_err *err) {
    plan_xport *x = (plan_xport *)ctx;
    atlas_rundriver_report rep;
    atlas_rundriver_report_init(&rep);
    atlas_status st = atlas_service_run_drive(x->pol, run_uid, x->log, &rep, err);
    if (st == ATLAS_OK && rep.busy) {
        x->saw_busy = true;
        x_say(x, "run %s is already held by another driver; nothing was started", run_uid);
    }
    if (st != ATLAS_OK && atlas_ipc_message_is_busy(err->msg)) {
        x->saw_busy = true;
        atlas_err_init(err);
        st = ATLAS_OK;
    }
    atlas_rundriver_report_free(&rep);
    return st;
}

/* One named workspace job's attempt, in this process, once.
 *
 * The lease asks for **this uid's driver partition** and never for a driver by
 * name, which is what keeps the operator's model partition and the untrusted
 * `atlas-worker` one apart: a job whose stored driver is outside the filter is
 * simply not granted, and that is an ordinary answer meaning "an operator's
 * dispatcher will carry this". A repo-tree driver is on no filter at all. */
static atlas_status x_drive_workspace_job(void *ctx, const char *job_uid, atlas_err *err) {
    plan_xport *x = (plan_xport *)ctx;
    if (!x->have_filter) {
        /* This uid runs no workspace driver, so the job belongs to a dispatcher
         * this process is not. Not a failure of the plan. */
        x_say(x, "job %s runs under a driver this process does not carry; an operator's "
                 "dispatcher will take it",
              job_uid);
        return ATLAS_OK;
    }
    char id[128];
    (void)snprintf(id, sizeof id, "atlas-plan/%lld", (long long)getpid());
    atlas_dispatch_opts o;
    memset(&o, 0, sizeof(o));
    o.socket_path = atlas_buf_cstr(&x->sock);
    o.worker_root = x->pol->model_worker_root;
    o.dispatcher_id = id;
    o.drivers = x->filter;
    o.operator_session = x->pol->model_uses_operator_session;
    o.live_model = x->pol->live_model;
    /* A12.0. Which model each role runs under, from the root-owned policy. The
     * driver's role picks one of the two at the attempt. */
    o.models.planner = x->pol->planner_model;
    o.models.executor = x->pol->executor_model;
    /* Comfortably below the lease lifetime, so a healthy job never loses its
     * lease while working. The same figure the background dispatcher uses. */
    o.heartbeat_ms = ATLAS_ORCH_LEASE_MS / 4;
    o.log = x->log;
    bool ran = false;
    atlas_status st = atlas_dispatch_run_one(&o, job_uid, &ran, err);
    if (st != ATLAS_OK && atlas_ipc_message_is_busy(err->msg)) {
        /* The daemon took nothing. Reported rather than raised: the plan is
         * untouched and the same invocation may simply be repeated. */
        x->saw_busy = true;
        atlas_err_init(err);
        return ATLAS_OK;
    }
    if (st == ATLAS_OK && !ran) {
        x_say(x, "job %s was not granted here; an operator's dispatcher will take it", job_uid);
    }
    return st;
}

/* --- the transport as a handle ----------------------------------------------
 *
 * One construction, used by `atlas_service_plan_run` and by the end-to-end test
 * that hosts the shipped driver against a real socket. See
 * `service_internal.h` for why the seam exists and for the one substitution it
 * permits, which is the policy's provenance and nothing else. */
atlas_status atlas_service_plan_xport_new(const atlas_orchpolicy *pol, FILE *log, plan_xport **out,
                                          atlas_err *err) {
    *out = NULL;
    plan_xport *x = calloc(1u, sizeof(*x));
    atlas_status st = ATLAS_OK;
    if (x == NULL) {
        st = atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    } else {
        atlas_buf_init(&x->sock);
        x->pol = pol;
        x->log = log;
        st = atlas_ipc_socket_path(&x->sock, err);
        if (st == ATLAS_OK) {
            /* The partition this process may carry a workspace job in, derived by
             * the one derivation. A uid the policy names no model driver for
             * simply carries none — its planner jobs and siblings wait for an
             * operator's dispatcher, which is an answer and not a failure — so
             * the refusal that derivation gives a *dispatcher* is not one here. */
            atlas_err scratch;
            atlas_err_init(&scratch);
            const bool is_model = atlas_orchpolicy_is_model_dispatcher(pol, (long long)getuid());
            x->have_filter =
                is_model && atlas_service_orch_driver_filter(pol, true, x->filter,
                                                             sizeof x->filter, &scratch) == ATLAS_OK;
            *out = x;
        } else {
            atlas_service_plan_xport_free(x);
        }
    }
    return st;
}

void atlas_service_plan_xport_free(plan_xport *x) {
    if (x != NULL) {
        atlas_buf_free(&x->sock);
        free(x);
    }
}

bool atlas_service_plan_xport_saw_busy(const plan_xport *x) {
    return x != NULL && x->saw_busy;
}

void atlas_service_plan_xport_wire(plan_xport *x, atlas_plandriver_transport *t) {
    memset(t, 0, sizeof(*t));
    t->ctx = x;
    t->plan_create = x_plan_create;
    t->plan_get = x_plan_get;
    t->plan_state = x_plan_state;
    t->plan_task = x_plan_task;
    t->plan_revision_add = x_plan_revision_add;
    t->job_submit = x_job_submit;
    t->job_get = x_job_get;
    t->drive_run = x_drive_run;
    t->drive_workspace_job = x_drive_workspace_job;
}

/* --- filling a render struct from one `plan.get` response -------------------- */

/* Borrowed pointers throughout: every string points into the parsed response,
 * which the caller keeps alive across the sink call. */
static void fill_plan_render(const atlas_ipc_response *r, atlas_plan_render *pr, bool detail) {
    memset(pr, 0, sizeof(*pr));
    pr->detail = detail;
    const struct {
        const char **field;
        const char *key;
    } strs[] = {
        {&pr->plan, "plan"},
        {&pr->repo, "repo"},
        {&pr->status, "status"},
        {&pr->created_at, "created_at"},
        {&pr->goal, "goal"},
        {&pr->gate_floor_text, "gate_floor_text"},
        {&pr->planner_job, "planner_job"},
        {&pr->planner_job_state, "planner_job_state"},
        {&pr->content, "content"},
    };
    for (size_t i = 0; i < sizeof strs / sizeof strs[0]; i++) {
        const char *v = NULL;
        if (atlas_ipc_result_str(r, strs[i].key, &v)) {
            *strs[i].field = v;
        }
    }
    const struct {
        int64_t *field;
        const char *key;
    } nums[] = {
        {&pr->max_parallel, "max_parallel"},
        {&pr->gate_floor_count, "gate_floor_count"},
        {&pr->rev_no, "rev_no"},
        {&pr->planner_jobs_seen, "planner_jobs_seen"},
        {&pr->stages_accepted, "stages_accepted"},
        {&pr->content_rev_no, "content_rev_no"},
    };
    for (size_t i = 0; i < sizeof nums / sizeof nums[0]; i++) {
        int64_t v = 0;
        if (atlas_ipc_result_int(r, nums[i].key, &v)) {
            *nums[i].field = v;
        }
    }
    (void)atlas_ipc_result_bool(r, "replan_wanted", &pr->replan_wanted);
    size_t n = 0;
    if (atlas_ipc_result_arr_len(r, "revisions", &n)) {
        pr->revision_count = (int64_t)n;
    }
    n = 0;
    (void)atlas_ipc_result_arr_len(r, "tasks", &n);
    for (size_t i = 0; i < n && pr->task_count < ATLAS_PLAN_MAX_TASKS; i++) {
        atlas_plan_task_render *t = &pr->tasks[pr->task_count];
        if (!atlas_ipc_result_arr_obj_str(r, "tasks", i, "key", &t->key) || t->key == NULL) {
            continue;
        }
        (void)atlas_ipc_result_arr_obj_str(r, "tasks", i, "kind", &t->kind);
        (void)atlas_ipc_result_arr_obj_str(r, "tasks", i, "title", &t->title);
        (void)atlas_ipc_result_arr_obj_str(r, "tasks", i, "job", &t->job);
        (void)atlas_ipc_result_arr_obj_str(r, "tasks", i, "job_state", &t->job_state);
        (void)atlas_ipc_result_arr_obj_str(r, "tasks", i, "run", &t->run);
        (void)atlas_ipc_result_arr_obj_str(r, "tasks", i, "run_status", &t->run_status);
        (void)atlas_ipc_result_arr_obj_int(r, "tasks", i, "stage", &t->stage);
        (void)atlas_ipc_result_arr_obj_str(r, "tasks", i, "usage_model", &t->usage_model);
        t->has_cost = atlas_ipc_result_arr_obj_int(r, "tasks", i, "usage_cost_micro_usd",
                                                   &t->usage_cost_micro_usd);
        t->has_turns = atlas_ipc_result_arr_obj_int(r, "tasks", i, "usage_turns", &t->usage_turns);
        pr->task_count++;
    }
}

/* --- plan status, plan show and plan list ----------------------------------- */

static atlas_status one_plan(atlas_ctx *ctx, const char *plan, int rev_no, atlas_plan_sink sink,
                             void *ud, atlas_err *err) {
    atlas_ipc_response *r = NULL;
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_status st = atlas_plan_wire_get(ctx, plan, rev_no, false, &r, &raw, err);
    if (st == ATLAS_OK && sink != NULL) {
        atlas_plan_render pr;
        fill_plan_render(r, &pr, true);
        st = sink(&pr, ud, err);
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return st;
}

atlas_status atlas_service_plan_status(atlas_ctx *ctx, const char *plan, atlas_plan_sink sink,
                                       void *ud, atlas_err *err) {
    return one_plan(ctx, plan, 0, sink, ud, err);
}

atlas_status atlas_service_plan_show(atlas_ctx *ctx, const char *plan, int rev_no,
                                     atlas_plan_sink sink, void *ud, atlas_err *err) {
    if (rev_no <= 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "atlas plan show needs --rev N, the revision to print");
    }
    return one_plan(ctx, plan, rev_no, sink, ud, err);
}

atlas_status atlas_service_plan_list(atlas_ctx *ctx, int64_t after, int64_t limit,
                                     atlas_plan_sink sink, void *ud, int64_t *count_out,
                                     bool *more_out, atlas_err *err) {
    atlas_ipc_response *r = NULL;
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_status st = atlas_plan_wire_list(ctx, after, limit, &r, &raw, err);
    if (st == ATLAS_OK) {
        int64_t n = 0;
        (void)atlas_ipc_result_int(r, "count", &n);
        if (count_out != NULL) {
            *count_out = n;
        }
        bool more = false;
        (void)atlas_ipc_result_bool(r, "more", &more);
        if (more_out != NULL) {
            *more_out = more;
        }
        for (int64_t i = 0; st == ATLAS_OK && i < n; i++) {
            atlas_plan_render pr;
            memset(&pr, 0, sizeof(pr));
            pr.in_list = true;
            if (!atlas_ipc_result_arr_obj_str(r, "plans", (size_t)i, "plan", &pr.plan)) {
                continue;
            }
            (void)atlas_ipc_result_arr_obj_str(r, "plans", (size_t)i, "repo", &pr.repo);
            (void)atlas_ipc_result_arr_obj_str(r, "plans", (size_t)i, "status", &pr.status);
            (void)atlas_ipc_result_arr_obj_str(r, "plans", (size_t)i, "created_at",
                                               &pr.created_at);
            if (sink != NULL) {
                st = sink(&pr, ud, err);
            }
        }
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return st;
}

/* --- plan run --------------------------------------------------------------- */

atlas_status atlas_service_plan_run(atlas_ctx *ctx, const atlas_plan_run_opts *o,
                                    atlas_plan_sink sink, void *ud, atlas_err *err) {
    const bool resuming = o->resume != NULL && o->resume[0] != '\0';
    /* The local refusals, mirroring `atlas job run`'s and in the same words where
     * the rule is the same: two spellings of one rule read as two rules. Every
     * one of them is checked before the policy is read, because a caller who
     * typed a contradiction learns it whether or not this machine runs
     * orchestration at all. */
    if (!resuming && (o->repo == NULL || o->repo[0] == '\0' || o->goal == NULL ||
                      o->goal[0] == '\0')) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "atlas plan run needs --repo and --goal, or --resume PLAN");
    }
    if (resuming && (o->repo != NULL || o->goal != NULL)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "--resume names an existing plan, so --repo and --goal do not apply");
    }
    if (resuming && o->gate_count > 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "--gate applies when a plan is created; the gate floor a plan was "
                             "created with cannot be changed by resuming it");
    }
    if (resuming && o->max_parallel != 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "--parallel applies when a plan is created; the bound a plan was "
                             "created with cannot be changed by resuming it");
    }
    if (!resuming && o->gate_count == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a plan needs at least one gate; the operator brings the gate floor "
                             "and the planner may only add to it");
    }
    if (!resuming && o->goal != NULL && strlen(o->goal) > (size_t)ATLAS_PLAN_GOAL_MAX) {
        /* Refused rather than truncated. A goal Atlas shortened is a goal nobody
         * wrote, and the planner would be answering it. */
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a goal is at most %d bytes; this one is %zu",
                             ATLAS_PLAN_GOAL_MAX, strlen(o->goal));
    }
    if (o->max_parallel != 0 &&
        (o->max_parallel < 1 || o->max_parallel > ATLAS_ORCH_RUN_MAX_PARALLEL)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a plan runs between 1 and %d tasks at once",
                             ATLAS_ORCH_RUN_MAX_PARALLEL);
    }

    /* The policy decides whether a live model may run, whose session it uses and
     * which drivers this process may carry. Read here from its compiled-in
     * root-owned path, exactly as the background dispatcher and the run driver
     * read it, and overridable by nothing. */
    atlas_orchpolicy pol;
    atlas_orchpolicy_load(&pol);
    if (pol.state != ATLAS_ORCHPOLICY_ENABLED) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG, "orchestration is not enabled (%s: %s)",
                             atlas_orchpolicy_reason_name(pol.reason),
                             atlas_orchpolicy_reason_explain(pol.reason));
    }

    plan_xport *x = NULL;
    atlas_status st = atlas_service_plan_xport_new(&pol, o->log, &x, err);

    atlas_plandriver_report rep;
    atlas_plandriver_report_init(&rep);
    if (st == ATLAS_OK) {
        atlas_plandriver_opts po;
        memset(&po, 0, sizeof(po));
        atlas_service_plan_xport_wire(x, &po.transport);
        po.plan_uid = o->resume;
        po.repo = o->repo;
        po.goal = o->goal;
        po.gate_floor = o->gates;
        po.gate_count = o->gate_count;
        po.max_parallel = (int)o->max_parallel;
        /* The production driver names. The *policy* still authorises every one of
         * them: a driver named here is a narrowing and never a permission. */
        po.planner_driver = "claude-plan";
        po.tree_driver = "claude-repo";
        po.side_driver = "claude";
        /* NULL leaves the daemon to pick the policy's mode, exactly as an absent
         * `--mode` does on `job submit`. */
        po.mode = NULL;
        po.log = o->log;
        st = atlas_plandriver_run(&po, &rep, err);
    }

    /* What the plan is, read back rather than assembled from the report: the
     * report says what this invocation did, and an operator wants what the plan
     * now *is*. One `plan.get`, rendered exactly as `plan status` renders it,
     * with the invocation's own two facts — busy, and this process's own view of
     * it — laid over the top. */
    if (st == ATLAS_OK && sink != NULL) {
        const bool busy = rep.busy || atlas_service_plan_xport_saw_busy(x);
        if (rep.plan_uid.len == 0) {
            /* No plan exists to read: a creation that the daemon was too busy to
             * take. Said plainly rather than with an empty document. */
            atlas_plan_render pr;
            memset(&pr, 0, sizeof(pr));
            pr.detail = true;
            pr.status = atlas_plan_status_name(ATLAS_PLAN_STATUS_UNKNOWN);
            pr.busy = busy;
            st = sink(&pr, ud, err);
        } else {
            atlas_ipc_response *r = NULL;
            atlas_buf raw = ATLAS_BUF_INIT;
            st = atlas_plan_wire_get(ctx, atlas_buf_cstr(&rep.plan_uid), 0, false, &r, &raw, err);
            if (st == ATLAS_OK) {
                atlas_plan_render pr;
                fill_plan_render(r, &pr, true);
                pr.busy = busy;
                st = sink(&pr, ud, err);
            }
            atlas_ipc_response_free(r);
            atlas_buf_free(&raw);
        }
    }

    atlas_plandriver_report_free(&rep);
    atlas_service_plan_xport_free(x);
    return st;
}
