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

#include <string.h>

#include "atlas/atlas.h"
#include "atlas/ipc.h"
#include "atlas/orch.h"
#include "atlas/plan.h"
#include "atlas/service.h"
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
} plan_get_args;

static atlas_status build_get(atlas_json *j, void *ud, atlas_err *err) {
    const plan_get_args *a = (const plan_get_args *)ud;
    atlas_status st = atlas_json_key_str(j, "plan", a->plan, err);
    if (st == ATLAS_OK && a->rev_no > 0) {
        /* Absent means "the state, without any document". A revision number is
         * what asks for one. */
        st = atlas_json_key_int(j, "rev_no", a->rev_no, err);
    }
    return st;
}

atlas_status atlas_plan_wire_get(atlas_ctx *ctx, const char *plan, int rev_no,
                                 atlas_ipc_response **out, atlas_buf *raw, atlas_err *err) {
    *out = NULL;
    if (plan == NULL || plan[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "which plan?");
    }
    plan_get_args a = {plan, rev_no};
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
