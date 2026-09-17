/* Atlas - A14: the gateway's remote job submission group.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * This file holds the methods the gateway's uid may call to queue, query and
 * cancel jobs submitted through bearer credentials: `job.remote_submit`,
 * `job.remote_get`, `job.remote_list`, `job.remote_cancel`, and — from A14R —
 * `job.remote_result`, which returns the three artifacts Atlas itself produced
 * for one terminal job. The revision A14R makes to Decision 7 is argued in full
 * above `method_remote_result` below, and in `docs/remote-submission.md`.
 *
 * ## Why this is a third table, not part of server_orch.c
 *
 * `server_orch.c`'s own opening comment describes the two existing groups
 * (`job.` for a submitter, `dispatch.` for the dispatcher) and adds a paragraph
 * pointing here.  The short answer: the predicate under which this group is
 * offered — the peer is the gateway AND the policy names a submission key AND
 * TLS is in front or cleartext is accepted — is neither the submitter's
 * predicate (a uid the root-owned orchestration policy names) nor the
 * dispatcher's (a uid that policy names separately).  Folding this group into
 * `server_orch.c` would make one `SO_PEERCRED` comparison answer for three
 * different grants; the pattern is the same one that put A16's disposal group
 * in `server_remote.c` beside this file rather than inside `server_decision.c`.
 *
 * ## Why `require_submitter` is never called here and this file never consults
 * `atlas_orchpolicy_permits_submitter`
 *
 * `require_submitter` asks whether `ds->peer_uid` is named as a submitter in
 * the orchestration policy.  The gateway uid is not a submitter; it is a
 * forwarding principal.  The authority to submit travels on the bearer
 * credential, which the daemon verifies in the write transaction — never from
 * a parameter a caller may forge.  This file never consults `require_submitter`
 * and never names `atlas_orchpolicy_permits_submitter`.
 *
 * ## What this group cannot name
 *
 * `job.remote_apply`, `job.remote_artifact`, `job.remote_log`,
 * `job.remote_run` are forbidden names that must never exist in the protocol.
 * `tests/test_orch_rpc.c` scans for them.  The reason for each: "apply" is a
 * lifecycle transition reserved for the decision layer; "artifact" would let a
 * remote credential name a file a *worker* chose to write; "log" would hand
 * over an unbounded transcript; "run" is the dispatch-level concept the gateway
 * may not touch.
 *
 * A14R did not weaken any of the four. `job.remote_result` is not `artifact`
 * under another name: it takes no artifact parameter, so the three names it can
 * return are a property of its signature rather than of a check, and each of
 * the three is composed by Atlas rather than chosen by a worker.
 *
 * A14R-F revises the third reason, narrowly, and leaves the name forbidden.
 * `job.remote_failure` returns a *bounded* tail of one redacted stream -- at
 * most `ATLAS_ORCH_LOG_TAIL_MAX` bytes, inside an event the dispatcher stored
 * under `ATLAS_ORCH_EVENT_MAX` -- and takes no parameter that names a stream, a
 * file or an attempt. What "log" was refused for, an unbounded transcript a
 * caller selects, is still refused; what is returned is what the dispatcher
 * chose to carry, and a reader is told when it carried nothing and why.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "atlas/atlas.h"
#include "atlas/driver.h"
#include "atlas/gwpolicy.h"
#include "atlas/ipc.h"
#include "atlas/orch.h"
#include "atlas/orch_ops.h"
#include "atlas/orch_remote.h"
#include "atlas/orchpolicy.h"
#include "atlas/syspolicy.h"
#include "server_internal.h"

/* --- predicates ------------------------------------------------------------ */

bool atlas_server_remote_submit_policy_ready(const atlas_gwpolicy *gw) {
    if (gw == NULL || gw->state != ATLAS_GWPOLICY_ENABLED) {
        return false;
    }
    if (gw->remote_submit_count == 0) {
        return false;
    }
    /* Decision 8: TLS termination in front or the operator's written cleartext
     * acceptance.  Both conditions are in the policy and only the policy. */
    if (gw->tls_mode != ATLAS_GWPOLICY_TLS_REVERSE_PROXY &&
        !gw->cleartext_submission_accepted) {
        return false;
    }
    return true;
}

bool atlas_server_remote_submit_offered(const atlas_server_ctx *ctx, long long peer_uid) {
    if (ctx == NULL) {
        return false;
    }
    if (!atlas_server_peer_is_gateway(ctx, peer_uid)) {
        return false;
    }
    return atlas_server_remote_submit_policy_ready(&ctx->gwpolicy);
}

/* --- per-request guard ----------------------------------------------------- */

/* Checked at the top of every method.  The predicate was already true for the
 * method name to be offered, but reaching a name and being allowed to use it
 * are different things -- the same separation `server_remote.c` applies. */
static atlas_status require_remote_submitter(dispatch_state *ds, atlas_err *err) {
    if (!atlas_server_remote_submit_offered(ds->ctx, (long long)ds->peer_uid)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "this connection may not submit remote jobs");
    }
    /* The orchestration policy must be ENABLED.  A zeroed orchpolicy (e.g., a
     * fixture daemon without an injection) produces DISABLED, and the same
     * `orch_disabled` sentence both other groups already use is produced here.
     * Note: this does NOT check atlas_orchpolicy_permits_submitter.  The
     * gateway uid is not a named submitter; the credential is the authority. */
    const atlas_orchpolicy *p = &ds->ctx->orchpolicy;
    if (p->state != ATLAS_ORCHPOLICY_ENABLED) {
        return atlas_server_orch_disabled(ds, err);
    }
    return ATLAS_OK;
}

/* --- shared list emitter --------------------------------------------------- */

/* Identical shape as server_orch.c's emit_job.  Not shared: each file's
 * emitter is private to it, following the pattern server_remote.c established
 * for the disposal group -- the files are beside each other, not composing. */
typedef struct remote_list_ctx {
    dispatch_state *ds;
    atlas_err *err;
} remote_list_ctx;

static atlas_status emit_remote_job(const atlas_orch_list_row *row, void *ud, atlas_err *err) {
    remote_list_ctx *lc = (remote_list_ctx *)ud;
    atlas_status st = atlas_json_obj_begin(lc->ds->j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(lc->ds->j, "job", row->job_uid, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(lc->ds->j, "state", atlas_orch_state_name(row->state), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(lc->ds->j, "repo", row->repo_name, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(lc->ds->j, "driver", row->driver, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(lc->ds->j, "created_at", row->created_at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(lc->ds->j, "attempts", row->attempts_started, err);
    }
    /* key_id is always non-empty for remote rows (scoped by key). */
    if (st == ATLAS_OK && row->submit_key_id[0] != '\0') {
        st = atlas_json_key_str(lc->ds->j, "key_id", row->submit_key_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(lc->ds->j, err);
    }
    return st;
}

/* --- job.remote_submit ----------------------------------------------------- */

/* Checks a named parameter that must not be present in a remote submission.
 * Decision 4: a caller who believes they configured a driver and silently got
 * the policy's is worse off than one who was told. */
static atlas_status refuse_if_present(const atlas_ipc_request *req, const char *name,
                                      atlas_err *err) {
    const char *v = NULL;
    int64_t vi = 0;
    bool vb = false;
    const atlas_ipc_array *va = NULL;
    if (atlas_ipc_param_str(req, name, &v) || atlas_ipc_param_int(req, name, &vi) ||
        atlas_ipc_param_bool(req, name, &vb) || atlas_ipc_param_array(req, name, &va)) {
        return atlas_err_set(
            err, ATLAS_ERR_USAGE,
            "a remote submission names the repository, the task and an idempotency key; "
            "%s is decided by the root-owned policy",
            name);
    }
    return ATLAS_OK;
}

static atlas_status method_remote_submit(dispatch_state *ds, const atlas_ipc_request *req,
                                         atlas_err *err) {
    atlas_status st = require_remote_submitter(ds, err);
    if (st != ATLAS_OK) {
        return st;
    }

    /* Decision 4: refused rather than ignored for any parameter that the policy
     * decides.  Six names the local `job.submit` accepts that remote must not.
     * Checked before parsing the accepted parameters so the refusal is the same
     * regardless of whether `repo` was also present. */
    static const char *const FORBIDDEN[] = {
        "driver", "mode", "validation", "parallel", "memory", "parent", NULL};
    for (size_t i = 0; FORBIDDEN[i] != NULL; i++) {
        st = refuse_if_present(req, FORBIDDEN[i], err);
        if (st != ATLAS_OK) {
            return st;
        }
    }

    const char *repo = NULL, *task = NULL, *key = NULL, *token = NULL;
    if (!atlas_ipc_param_str(req, "repo", &repo) || repo == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a job needs a repository name");
    }
    if (!atlas_ipc_param_str(req, "task", &task) || task == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a job needs task text");
    }
    if (!atlas_ipc_param_str(req, "token", &token) || token == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "\"token\" is required");
    }
    /* `key` is optional -- absent means no idempotency key for this submission. */
    (void)atlas_ipc_param_str(req, "key", &key);

    /* client_key length check before the copy, per T3's handoff.  The frozen
     * sentence is `atlas_orch_remote_idempotency_key`'s own; the write point
     * calls that function to validate again, but a method refusal is cheaper
     * than a write-point refusal. */
    if (key != NULL && strlen(key) > ATLAS_ORCH_REMOTE_CLIENT_KEY_MAX) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a remote idempotency key is at most 40 characters of [a-z0-9._-]");
    }

    const atlas_orchpolicy *op_orch = &ds->ctx->orchpolicy;
    const atlas_gwpolicy *gw = &ds->ctx->gwpolicy;

    /* Policy check: the repository must be listed in the orchestration policy. */
    if (!atlas_orchpolicy_permits_repo(op_orch, repo)) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "the orchestration policy does not permit jobs against that "
                             "repository");
    }
    /* Cross-check driver against orchpolicy. */
    if (!atlas_orchpolicy_permits_driver(op_orch, gw->remote_submit_driver)) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "the remote driver %s is not one /etc/atlas/orchestration.conf "
                             "configures",
                             gw->remote_submit_driver);
    }
    /* Cross-check mode against orchpolicy. */
    if (!atlas_orchpolicy_permits_mode(op_orch, gw->remote_submit_mode)) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "the remote mode %s is not one /etc/atlas/orchestration.conf "
                             "configures",
                             gw->remote_submit_mode);
    }
    /* Cross-check live_model requirement. */
    const atlas_driver *drv = atlas_driver_find(gw->remote_submit_driver);
    if (drv != NULL && drv->needs_live_model && !op_orch->live_model) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "the remote driver %s needs a live model and "
                             "/etc/atlas/orchestration.conf has live_model = off",
                             gw->remote_submit_driver);
    }

    /* Resolve the repository. */
    atlas_repo_info ri;
    atlas_repo_info_init(&ri);
    bool found = false;
    st = atlas_db_repo_get(ds->db, repo, &ri, &found, err);
    if (st == ATLAS_OK && !found) {
        st = atlas_err_set(err, ATLAS_ERR_REPO, "no repository named that is registered");
    }
    if (st == ATLAS_OK && ri.scanned_head[0] == '\0') {
        st = atlas_err_set(err, ATLAS_ERR_REPO,
                           "that repository has never been scanned, so Atlas cannot pin a "
                           "source commit for a job");
    }
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&ri);
        return st;
    }

    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_SUBMIT);
    if (op == NULL) {
        atlas_repo_info_free(&ri);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory building a job");
    }

    /* Identity comes from SO_PEERCRED, not from the document. */
    op->peer_uid = ds->peer_uid;
    op->actor = ATLAS_ORCH_ACTOR_CLIENT;
    op->repo_id = ri.id;
    op->spec.submitter_uid = (long long)ds->peer_uid;

    atlas_buf identity = ATLAS_BUF_INIT;
    st = atlas_db_repo_identity_hash(ds->db, ri.id, &identity, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&op->spec.repo_identity_hash, identity.data, identity.len, err);
    }
    atlas_buf_free(&identity);
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op->spec.repo_name, ri.name, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op->spec.source_commit, ri.scanned_head, err);
    }
    atlas_repo_info_free(&ri);

    /* Driver, mode and max_attempts come from the gateway policy, never the request. */
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op->spec.driver, gw->remote_submit_driver, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op->spec.mode, gw->remote_submit_mode, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op->spec.task_text, task, err);
    }

    /* Decision 6: the client's idempotency key fragment travels on the op;
     * the write point namespaces it as `remote.<key_id>.<client>`.
     * spec.idempotency_key is left empty intentionally. */
    if (st == ATLAS_OK && key != NULL) {
        (void)snprintf(op->remote_client_key, sizeof(op->remote_client_key), "%s", key);
    }

    /* Gate lines come from the gateway policy, split here and pushed into
     * the validation array.  Refused with the position-and-message sentence. */
    for (size_t i = 0; st == ATLAS_OK && i < gw->remote_submit_gate_count; i++) {
        atlas_orch_argv tmp;
        atlas_orch_argv_init(&tmp);
        atlas_err split_err;
        atlas_err_init(&split_err);
        if (atlas_orch_gate_split(gw->remote_submit_gates[i], &tmp, &split_err) != ATLAS_OK) {
            st = atlas_err_set(err, ATLAS_ERR_USAGE, "remote gate %zu could not be split: %s",
                               i, atlas_err_msg(&split_err));
            atlas_orch_argv_free(&tmp);
            break;
        }
        for (size_t k = 0; st == ATLAS_OK && k < tmp.count; k++) {
            st = atlas_orch_argv_push(&op->spec.validations[i], tmp.args[k].data,
                                      tmp.args[k].len, err);
        }
        if (st == ATLAS_OK) {
            op->spec.validation_count = i + 1u;
        }
        atlas_orch_argv_free(&tmp);
    }

    /* Fixed remote-submission values: no memory, one slot, no parent. */
    op->memory_mode = ATLAS_ORCH_MEMORY_MODE_OFF;
    op->run_max_parallel = 0; /* write point resolves to 1 */

    /* max_attempts from the gateway policy; the orchestration ceiling is applied
     * by atlas_orchpolicy_apply_limits with its own existing sentence -- no second
     * spelling of that refusal lives here. */
    if (st == ATLAS_OK) {
        op->spec.max_attempts = (int64_t)gw->remote_submit_max_attempts;
        st = atlas_orchpolicy_apply_limits(op_orch, &op->spec, err);
    }

    /* Remote credential material.  Ownership of `op` passes to the write point
     * on orch_write; do not read op after that call. */
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op->remote_token, token, err);
    }
    if (st == ATLAS_OK) {
        op->remote_max_active = gw->remote_submit_max_active;
        op->remote_max_per_day = gw->remote_submit_max_per_day;
        op->remote_max_active_total = gw->remote_submit_max_active_total;
        op->remote_allowed_count = gw->remote_submit_count;
        for (size_t i = 0; i < gw->remote_submit_count; i++) {
            (void)snprintf(op->remote_allowed_ids[i], ATLAS_APIKEY_SELECTOR_HEX + 1u, "%s",
                           gw->remote_submit_keys[i]);
        }
    }

    /* peer_is_operator: set from SO_PEERCRED, never from a parameter. */
    op->peer_is_operator = atlas_server_peer_is_operator(ds->peer_uid);

    if (st == ATLAS_OK) {
        st = atlas_orch_spec_canonicalise(&op->spec, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_orch_spec_validate(&op->spec, err);
    }
    if (st != ATLAS_OK) {
        atlas_orch_op_free(op);
        free(op);
        return st;
    }

    /* Capture values needed for the response before ownership transfers. */
    char resp_driver[ATLAS_ORCH_NAME_MAX + 1u];
    char resp_mode[ATLAS_ORCH_NAME_MAX + 1u];
    int64_t resp_max_attempts = op->spec.max_attempts;
    (void)snprintf(resp_driver, sizeof(resp_driver), "%s",
                   atlas_buf_cstr(&op->spec.driver));
    (void)snprintf(resp_mode, sizeof(resp_mode), "%s",
                   atlas_buf_cstr(&op->spec.mode));

    atlas_orch_result r;
    atlas_orch_result_init(&r);
    atlas_syspolicy pol;
    atlas_syspolicy_load(&pol);
    /* Ownership of op passes here unconditionally. */
    st = atlas_server_orch_write(ds, op, 5000, &pol, &r, err);
    if (st == ATLAS_OK) {
        st = atlas_server_write_job_summary(ds, &r, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_bool(ds->j, "duplicate", r.duplicate, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "spec_digest", r.spec_digest, err);
        }
        /* A14. Frozen response fields for remote submit. */
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "key_id", r.key_id, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "driver", resp_driver, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "mode", resp_mode, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "attempts_max", resp_max_attempts, err);
        }
        /* Budget object: active and daily counts after this submission. */
        if (st == ATLAS_OK) {
            st = atlas_json_key(ds->j, "budget", err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_obj_begin(ds->j, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "active", r.remote_active, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "active_max",
                                    (int64_t)ds->ctx->gwpolicy.remote_submit_max_active, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "today", r.remote_today, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "today_max",
                                    (int64_t)ds->ctx->gwpolicy.remote_submit_max_per_day, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_obj_end(ds->j, err);
        }
    }
    atlas_orch_result_free(&r);
    return st;
}

/* --- job.remote_get -------------------------------------------------------- */

static atlas_status method_remote_get(dispatch_state *ds, const atlas_ipc_request *req,
                                      atlas_err *err) {
    atlas_status st = require_remote_submitter(ds, err);
    if (st != ATLAS_OK) {
        return st;
    }

    const char *uid = NULL, *token = NULL;
    if (!atlas_ipc_param_str(req, "job", &uid) || uid == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "which job?");
    }
    if (!atlas_ipc_param_str(req, "token", &token) || token == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "\"token\" is required");
    }

    /* Verify the credential, then check the job belongs to that key. */
    const atlas_gwpolicy *gw = &ds->ctx->gwpolicy;
    atlas_buf tok = ATLAS_BUF_INIT;
    st = atlas_buf_set_str(&tok, token, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char key_id[ATLAS_APIKEY_SELECTOR_HEX + 1u];
    key_id[0] = '\0';
    st = atlas_orch_remote_verify(ds->db, &tok,
                                  (const char (*)[ATLAS_APIKEY_SELECTOR_HEX + 1u])
                                      gw->remote_submit_keys,
                                  gw->remote_submit_count, key_id, err);
    atlas_buf_free(&tok);
    if (st != ATLAS_OK) {
        return st;
    }

    atlas_orch_job_view v;
    atlas_orch_job_view_init(&v);
    bool found = false;
    st = atlas_db_orch_job_get(ds->db, uid, &v, &found, err);
    /* Scope: visible only when submit_key_id matches the verified key. */
    if (st == ATLAS_OK && (!found || strcmp(v.submit_key_id, key_id) != 0)) {
        atlas_orch_job_view_free(&v);
        return atlas_err_set(err, ATLAS_ERR_USAGE, "no such job");
    }
    if (st != ATLAS_OK) {
        atlas_orch_job_view_free(&v);
        return st;
    }

    /* Emit the same fields as job.get, plus key_id, reason and usage. */
    st = atlas_json_key_str(ds->j, "job", v.job_uid, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "state", atlas_orch_state_name(v.state), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "repo", v.repo_name, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "commit", v.source_commit, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "mode", v.mode, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "driver", v.driver, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "spec_digest", v.spec_digest, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "created_at", v.created_at, err);
    }
    if (st == ATLAS_OK && v.terminal_at[0] != '\0') {
        st = atlas_json_key_str(ds->j, "terminal_at", v.terminal_at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "attempts", v.attempts_started, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "max_attempts", v.max_attempts, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "cancel_requested", v.cancel_requested, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "seq", v.state_seq, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "task_encoding", "atlas-safe-1", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "task",
                                atlas_safe(&ds->safe, atlas_buf_cstr(&v.task_text)), err);
    }
    /* A14. key_id, run, reason and usage -- the remote-only additions. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "key_id", v.submit_key_id, err);
    }
    if (st == ATLAS_OK && v.run_uid[0] != '\0') {
        st = atlas_json_key_str(ds->j, "run", v.run_uid, err);
    }
    /* Newest transition reason, if any. */
    if (st == ATLAS_OK) {
        char reason[64];
        reason[0] = '\0';
        atlas_err rerr;
        atlas_err_init(&rerr);
        if (atlas_db_orch_job_newest_reason(ds->db, v.job_uid, reason, &rerr) == ATLAS_OK &&
            reason[0] != '\0') {
            st = atlas_json_key_str(ds->j, "reason", reason, err);
        }
    }
    atlas_orch_job_view_free(&v);

    /* Usage block: present, model, cost, turns. */
    if (st == ATLAS_OK) {
        atlas_orch_job_usage u;
        atlas_err uerr;
        atlas_err_init(&uerr);
        if (atlas_db_orch_job_usage(ds->db, uid, &u, &uerr) != ATLAS_OK) {
            u.present = false;
        }
        st = atlas_json_key(ds->j, "usage", err);
        if (st == ATLAS_OK) {
            st = atlas_json_obj_begin(ds->j, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_bool(ds->j, "present", u.present, err);
        }
        if (st == ATLAS_OK && u.present) {
            if (st == ATLAS_OK) {
                st = atlas_json_key_str(ds->j, "model", u.model, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_key_bool(ds->j, "has_cost", u.has_cost, err);
            }
            if (st == ATLAS_OK && u.has_cost) {
                st = atlas_json_key_int(ds->j, "cost_micro_usd", u.cost_micro_usd, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_key_bool(ds->j, "has_turns", u.has_turns, err);
            }
            if (st == ATLAS_OK && u.has_turns) {
                st = atlas_json_key_int(ds->j, "turns", u.turns, err);
            }
        }
        if (st == ATLAS_OK) {
            st = atlas_json_obj_end(ds->j, err);
        }
    }
    return st;
}

/* --- job.remote_result -----------------------------------------------------
 *
 * A14's Decision 7 said "no artifact, no log, no gate output travels a remote
 * route or tool", called the resulting gap "the transcript chain", and offered
 * no fix. **A14R revises it, narrowly and on purpose**, and the revision is
 * written in `docs/remote-submission.md` beside the original.
 *
 * What Decision 7 was protecting is unchanged and is still enforced here: a
 * remote credential may not name an artifact, may not ask for a log, and may
 * not reach anything a worker chose to write. `job.remote_apply`,
 * `job.remote_artifact`, `job.remote_log` and `job.remote_run` remain forbidden
 * names and `tests/test_orch_rpc.c` still scans for them.
 *
 * What it was costing is what changed. A steward who submits work and is shown
 * only a state cannot review it, and cannot decide whether to apply it — so the
 * human approval gate that the whole arrangement rests on had nothing to read.
 * The path A14 named instead, "the Atlas machine and the terminal command
 * `atlas job artifact`", **did not exist**: there is no such subcommand, and
 * for an executor driver the bytes were destroyed with the workspace anyway.
 * A guarantee whose escape hatch is not implemented is not a narrower channel,
 * it is a wider one that nobody can audit.
 *
 * The revision is bounded by construction rather than by a check:
 *
 *   - Three names, fixed in `atlas/orch.h`, each produced by Atlas: the patch
 *     from a content diff of the two trees, the gate summary from Atlas' own
 *     verdicts, the answer from the final record of the stream Atlas captured.
 *     There is no parameter here that selects an artifact, so "a caller may not
 *     name one" is a property of the signature.
 *   - Every one is UNTRUSTED_DATA, safe-encoded at this boundary exactly as
 *     `job.artifact` encodes what it returns, and read by no branch anywhere.
 *   - The credential must be the one that submitted the job, and the job's
 *     repository must still be one the orchestration policy permits. Both are
 *     re-checked here rather than inherited: a repository removed from the
 *     policy stops being readable, which is what makes the policy a live grant
 *     instead of a record of one.
 *   - It applies nothing, commits nothing, starts no process and writes no row.
 */

/* One artifact this method may return, filled by the row callback. */
typedef struct result_slot {
    const char *name;
    bool found;
    bool stored;
    int64_t size_bytes;
    char sha256[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_buf content;
} result_slot;

typedef struct result_ctx {
    result_slot *slots;
    size_t count;
} result_ctx;

static atlas_status take_result_artifact(const atlas_orch_artifact_row *row, void *ud,
                                         atlas_err *err) {
    result_ctx *rc = (result_ctx *)ud;
    for (size_t i = 0; i < rc->count; i++) {
        result_slot *s = &rc->slots[i];
        if (row->name == NULL || strcmp(row->name, s->name) != 0) {
            continue;
        }
        /* Newest attempt wins: the rows arrive in id order and a later attempt
         * describes the state the job actually ended in. An earlier attempt's
         * patch is not a smaller answer, it is a different one. */
        s->found = true;
        s->size_bytes = row->size_bytes;
        (void)snprintf(s->sha256, sizeof(s->sha256), "%s", row->sha256 != NULL ? row->sha256 : "");
        atlas_buf_free(&s->content);
        atlas_buf_init(&s->content);
        s->stored = false;
        if (row->content_stored && row->content != NULL) {
            if (atlas_buf_set(&s->content, row->content, row->content_len, err) != ATLAS_OK) {
                return ATLAS_ERR_INTERNAL;
            }
            s->stored = true;
        }
        return ATLAS_OK;
    }
    return ATLAS_OK;
}

/* Emits one artifact as an object under its own key.
 *
 * `available` is always present and is the field a reader checks first; when it
 * is false, `unavailable_reason` says which of the three things happened rather
 * than leaving an empty string to be interpreted. A digest and a size are
 * emitted whenever the row exists at all, so a reader who cannot be given the
 * bytes can still say what they were. */
static atlas_status emit_result_slot(dispatch_state *ds, const result_slot *s, const char *key,
                                     atlas_err *err) {
    atlas_status st = atlas_json_key(ds->j, key, err);
    if (st == ATLAS_OK) {
        st = atlas_json_obj_begin(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "available", s->found && s->stored, err);
    }
    if (st == ATLAS_OK && s->found) {
        st = atlas_json_key_int(ds->j, "size_bytes", s->size_bytes, err);
    }
    if (st == ATLAS_OK && s->found && s->sha256[0] != '\0') {
        st = atlas_json_key_str(ds->j, "sha256", s->sha256, err);
    }
    if (st == ATLAS_OK && s->found && s->stored) {
        /* A model's output, and the patch Atlas derived from what it did:
         * UNTRUSTED_DATA, labelled and safe-encoded, so a patch full of control
         * bytes cannot reach a terminal as escapes. The same two lines
         * `job.artifact` emits, for the same reason. */
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "encoding", "atlas-safe-1", err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "provenance", "UNTRUSTED_DATA", err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "content",
                                    atlas_safe(&ds->safe, atlas_buf_cstr(&s->content)), err);
        }
    }
    if (st == ATLAS_OK && !(s->found && s->stored)) {
        const char *why =
            !s->found ? "the attempt produced no artifact of this name"
                      : "the artifact exceeded the bound one completion may carry, so its bytes "
                        "stayed in the worker workspace and are not readable here";
        st = atlas_json_key_str(ds->j, "unavailable_reason", why, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(ds->j, err);
    }
    return st;
}

static atlas_status method_remote_result(dispatch_state *ds, const atlas_ipc_request *req,
                                         atlas_err *err) {
    atlas_status st = require_remote_submitter(ds, err);
    if (st != ATLAS_OK) {
        return st;
    }

    const char *uid = NULL, *token = NULL;
    if (!atlas_ipc_param_str(req, "job", &uid) || uid == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "which job?");
    }
    if (!atlas_ipc_param_str(req, "token", &token) || token == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "\"token\" is required");
    }

    const atlas_gwpolicy *gw = &ds->ctx->gwpolicy;
    atlas_buf tok = ATLAS_BUF_INIT;
    st = atlas_buf_set_str(&tok, token, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char key_id[ATLAS_APIKEY_SELECTOR_HEX + 1u];
    key_id[0] = '\0';
    st = atlas_orch_remote_verify(ds->db, &tok,
                                  (const char (*)[ATLAS_APIKEY_SELECTOR_HEX + 1u])
                                      gw->remote_submit_keys,
                                  gw->remote_submit_count, key_id, err);
    atlas_buf_free(&tok);
    if (st != ATLAS_OK) {
        return st;
    }

    atlas_orch_job_view v;
    atlas_orch_job_view_init(&v);
    bool found = false;
    st = atlas_db_orch_job_get(ds->db, uid, &v, &found, err);
    /* Scope, in the order that leaks least. A job this credential did not
     * submit and a job whose repository the policy no longer permits both
     * answer "no such job": a refusal that distinguished them would tell a
     * caller which jobs exist, and an inventory handed to whoever asked is not
     * a refusal. */
    if (st == ATLAS_OK && (!found || strcmp(v.submit_key_id, key_id) != 0 ||
                           !atlas_orchpolicy_permits_repo(&ds->ctx->orchpolicy, v.repo_name))) {
        atlas_orch_job_view_free(&v);
        return atlas_err_set(err, ATLAS_ERR_USAGE, "no such job");
    }
    if (st != ATLAS_OK) {
        atlas_orch_job_view_free(&v);
        return st;
    }

    const bool terminal = atlas_orch_state_is_terminal(v.state);
    st = atlas_json_key_str(ds->j, "job", v.job_uid, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "state", atlas_orch_state_name(v.state), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "repo", v.repo_name, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "key_id", v.submit_key_id, err);
    }
    /* The field a caller branches on. A running job is not a job with an empty
     * result: a reader must be able to tell "not yet" from "nothing", and one
     * of those is a reason to ask again. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "availability", terminal ? "terminal" : "not_terminal",
                                err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "attempts", v.attempts_started, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "max_attempts", v.max_attempts, err);
    }
    if (!terminal) {
        atlas_orch_job_view_free(&v);
        return st;
    }
    if (st == ATLAS_OK && v.terminal_at[0] != '\0') {
        st = atlas_json_key_str(ds->j, "terminal_at", v.terminal_at, err);
    }
    if (st == ATLAS_OK) {
        char reason[64];
        reason[0] = '\0';
        atlas_err rerr;
        atlas_err_init(&rerr);
        if (atlas_db_orch_job_newest_reason(ds->db, v.job_uid, reason, &rerr) == ATLAS_OK &&
            reason[0] != '\0') {
            st = atlas_json_key_str(ds->j, "reason", reason, err);
        }
    }
    atlas_orch_job_view_free(&v);

    /* Usage, on the same terms `job.remote_get` reports it, plus the one thing
     * that method does not say: whether the measurement is complete. A worker
     * killed at a bound never wrote a final record, so its cost is unknown —
     * and unknown must not read as zero, which is what a bare absent field
     * invites a caller to assume. */
    if (st == ATLAS_OK) {
        atlas_orch_job_usage u;
        atlas_err uerr;
        atlas_err_init(&uerr);
        if (atlas_db_orch_job_usage(ds->db, uid, &u, &uerr) != ATLAS_OK) {
            memset(&u, 0, sizeof(u));
        }
        st = atlas_json_key(ds->j, "usage", err);
        if (st == ATLAS_OK) {
            st = atlas_json_obj_begin(ds->j, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_bool(ds->j, "present", u.present, err);
        }
        const bool complete = u.present && u.model[0] != '\0' && u.has_cost && u.has_turns;
        if (st == ATLAS_OK) {
            st = atlas_json_key_bool(ds->j, "complete", complete, err);
        }
        if (st == ATLAS_OK && u.model[0] != '\0') {
            st = atlas_json_key_str(ds->j, "model", u.model, err);
        }
        if (st == ATLAS_OK && u.has_cost) {
            st = atlas_json_key_int(ds->j, "cost_micro_usd", u.cost_micro_usd, err);
        }
        if (st == ATLAS_OK && u.has_turns) {
            st = atlas_json_key_int(ds->j, "turns", u.turns, err);
        }
        if (st == ATLAS_OK && !complete) {
            /* Says what is known and stops there. Atlas records a usage row for
             * every completion and fills it from the final record of the
             * worker's stream; a row with nothing in it means that record never
             * arrived, and the two ways that happens — a worker stopped at a
             * bound before it wrote one, and a driver that streams no
             * measurement at all — are not distinguishable from the row.
             * Naming one of them here would be inventing a cause. What must be
             * unambiguous is the part that matters to a reader: unknown is not
             * zero. */
            st = atlas_json_key_str(
                ds->j, "incomplete_reason",
                u.present ? "the attempt's completion carried no final measurement, so what it "
                            "cost is unknown; unknown is not zero"
                          : "no usage row exists for this job",
                err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_obj_end(ds->j, err);
        }
    }

    /* The three artifacts, by name and by nothing else. */
    result_slot slots[3];
    memset(slots, 0, sizeof(slots));
    slots[0].name = ATLAS_ORCH_RESULT_TEXT_NAME;
    slots[1].name = ATLAS_ORCH_RESULT_PATCH_NAME;
    slots[2].name = ATLAS_ORCH_RESULT_GATES_NAME;
    for (size_t i = 0; i < 3; i++) {
        atlas_buf_init(&slots[i].content);
    }
    if (st == ATLAS_OK) {
        result_ctx rc = {slots, 3};
        int64_t n = 0;
        atlas_err aerr;
        atlas_err_init(&aerr);
        /* A failure to read the artifacts is not a failure of the method: the
         * state, the reason and the usage are already true and useful, and each
         * slot reports its own absence. */
        (void)atlas_db_orch_artifacts(ds->db, uid, 0, true, take_result_artifact, &rc, &n, &aerr);
    }
    static const char *const KEYS[3] = {"final_text", "patch", "validations"};
    for (size_t i = 0; st == ATLAS_OK && i < 3; i++) {
        st = emit_result_slot(ds, &slots[i], KEYS[i], err);
    }
    for (size_t i = 0; i < 3; i++) {
        atlas_buf_free(&slots[i].content);
    }
    return st;
}

/* --- job.remote_failure ------------------------------------------------------
 *
 * A14R-F. Why one terminal job did not succeed, read from the ledger and from
 * the failed attempt's own event stream — for the case `job.remote_result`
 * cannot serve: a worker that never started, or one that stopped before it
 * wrote a final answer. A14R's rule that a worker's `logs/stdout.log` is
 * "reachable under no name" is **revised here, narrowly**: a bounded, redacted
 * tail of it, carried by the dispatcher as an ordinary event under its own
 * lease, is readable through this method. The transcript itself is still
 * reachable from no remote route, `job.remote_log` is still a forbidden name,
 * and what changed is written in `docs/remote-submission.md`.
 *
 * What bounds it is construction, not a check:
 *
 *   - The method takes the job and nothing else. There is no stream, file or
 *     attempt parameter, so "a caller may not name a file" stays a property of
 *     the signature.
 *   - Every worker byte it returns was already stored under
 *     `ATLAS_ORCH_EVENT_MAX` by the `dispatch.event` write point, and the tail
 *     inside a `log_tail` event is `ATLAS_ORCH_LOG_TAIL_MAX` bytes of a stream
 *     the driver had already redacted. Nothing here opens a workspace.
 *   - This daemon reads only the header the dispatcher composed — fixed keys,
 *     integers and yes/no, ending at the first `--` line — and hands the bytes
 *     after it through safe-encoded and labelled. No branch reads them.
 *   - The scope check is `job.remote_result`'s, verbatim: the credential must
 *     be the one that submitted the job, and the repository must still be one
 *     the orchestration policy permits; both refuse as "no such job".
 *   - It writes no row, starts no process and applies nothing.
 */

typedef struct failure_ctx {
    dispatch_state *ds;
    int64_t attempts;
    bool newest_set;
    atlas_orch_attempt_row newest; /* copied into the fixed buffers below */
    char newest_state[32];
    char newest_exit_kind[32];
    char newest_failure_reason[64];
} failure_ctx;

static void copy_small(char *dst, size_t cap, const char *src) {
    (void)snprintf(dst, cap, "%s", src != NULL ? src : "");
}

static atlas_status emit_attempt_row(const atlas_orch_attempt_row *row, void *ud, atlas_err *err) {
    failure_ctx *fc = (failure_ctx *)ud;
    dispatch_state *ds = fc->ds;
    fc->attempts++;
    /* The newest attempt is the one the job ended in; the `cause` line below
     * is composed from it. Rows arrive oldest first, so the last one wins. */
    fc->newest_set = true;
    fc->newest.attempt_no = row->attempt_no;
    fc->newest.exit_code = row->exit_code;
    copy_small(fc->newest_state, sizeof fc->newest_state, row->state);
    copy_small(fc->newest_exit_kind, sizeof fc->newest_exit_kind, row->exit_kind);
    copy_small(fc->newest_failure_reason, sizeof fc->newest_failure_reason, row->failure_reason);

    atlas_status st = atlas_json_obj_begin(ds->j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "attempt", row->attempt_no, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "state", row->state != NULL ? row->state : "", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "driver", atlas_safe(&ds->safe, row->driver), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "driver_version",
                                atlas_safe(&ds->safe, row->driver_version), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "exit_kind", row->exit_kind != NULL ? row->exit_kind : "",
                                err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "exit_code", row->exit_code, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "failure_reason",
                                row->failure_reason != NULL ? row->failure_reason : "", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "event_count", row->event_count, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "artifact_count", row->artifact_count, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "dispatcher", atlas_safe(&ds->safe, row->dispatcher_id),
                                err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "started_at", row->started_at != NULL ? row->started_at : "",
                                err);
    }
    if (st == ATLAS_OK && row->ended_at != NULL) {
        st = atlas_json_key_str(ds->j, "ended_at", row->ended_at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(ds->j, err);
    }
    return st;
}

static atlas_status emit_transition_row(const atlas_orch_transition_row *row, void *ud,
                                        atlas_err *err) {
    dispatch_state *ds = ((failure_ctx *)ud)->ds;
    atlas_status st = atlas_json_obj_begin(ds->j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "seq", row->id, err);
    }
    if (st == ATLAS_OK && row->attempt_no > 0) {
        st = atlas_json_key_int(ds->j, "attempt", row->attempt_no, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "from", row->from_state != NULL ? row->from_state : "",
                                err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "to", row->to_state != NULL ? row->to_state : "", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "reason", row->reason != NULL ? row->reason : "", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "actor", row->actor != NULL ? row->actor : "", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "at", row->at != NULL ? row->at : "", err);
    }
    /* Composed by Atlas at every write point that fills it, and still encoded:
     * a renderer validates rather than trusts. */
    if (st == ATLAS_OK && row->detail != NULL && row->detail[0] != '\0') {
        st = atlas_json_key_str(ds->j, "detail", atlas_safe(&ds->safe, row->detail), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(ds->j, err);
    }
    return st;
}

/* One event, as the worker-labelled narrative it is. `log_tail` events are
 * skipped here: they are rendered once, parsed, under `worker_log`. */
static atlas_status emit_event_row(const atlas_orch_event_row *row, void *ud, atlas_err *err) {
    dispatch_state *ds = ((failure_ctx *)ud)->ds;
    if (row->kind != NULL && strcmp(row->kind, ATLAS_ORCH_EVENT_KIND_LOG_TAIL) == 0) {
        return ATLAS_OK;
    }
    atlas_status st = atlas_json_obj_begin(ds->j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "attempt", row->attempt_no, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "seq", row->seq, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "kind", atlas_safe(&ds->safe, row->kind), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "at", row->at != NULL ? row->at : "", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "provenance", "UNTRUSTED_DATA", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "payload",
                                atlas_safe_n(&ds->safe, row->payload, row->payload_len), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(ds->j, err);
    }
    return st;
}

/* The newest `failure` event of the job, copied out for `dispatcher_error`. */
typedef struct failure_event {
    bool found;
    int64_t attempt_no;
    char at[40];
    atlas_buf message;
} failure_event;

static atlas_status take_failure_event(const atlas_orch_event_row *row, void *ud, atlas_err *err) {
    failure_event *fe = (failure_event *)ud;
    fe->found = true;
    fe->attempt_no = row->attempt_no;
    copy_small(fe->at, sizeof fe->at, row->at);
    return atlas_buf_set(&fe->message, row->payload, row->payload_len, err);
}

/* One parsed `log_tail` event per stream. The header is Atlas' grammar; the
 * bytes after `--` are the worker's and are copied, never read. */
typedef struct log_tail {
    bool found;      /* an event for this stream exists */
    bool well_formed; /* its header parsed */
    bool present;    /* the driver wrote the stream */
    int64_t total_bytes;
    int64_t tail_bytes;
    bool truncated;
    int64_t nul_replaced;
    int64_t attempt_no;
    atlas_buf content;
} log_tail;

typedef struct log_tails {
    log_tail stdout_tail;
    log_tail stderr_tail;
} log_tails;

static bool line_int(const char *v, int64_t *out) {
    char *end = NULL;
    long long n = strtoll(v, &end, 10);
    if (end == v || *end != '\0' || n < 0) {
        return false;
    }
    *out = (int64_t)n;
    return true;
}

/* Parses one payload. Returns false when the header is not the grammar this
 * daemon reads; a malformed header is reported as such rather than guessed at.
 * `stream_out` receives which stream the event describes. */
static bool parse_log_tail(const void *payload, size_t len, log_tail *t, char stream_out[16],
                           atlas_err *err) {
    const char *p = (const char *)payload;
    size_t i = 0;
    bool magic = false;
    stream_out[0] = '\0';
    memset(t, 0, sizeof(*t));
    atlas_buf_init(&t->content);
    while (i < len) {
        const char *nl = memchr(p + i, '\n', len - i);
        size_t ll = nl != NULL ? (size_t)(nl - (p + i)) : len - i;
        char line[128];
        if (ll >= sizeof line) {
            return false;
        }
        memcpy(line, p + i, ll);
        line[ll] = '\0';
        i += ll + (nl != NULL ? 1u : 0u);
        if (!magic) {
            if (strcmp(line, ATLAS_ORCH_LOG_TAIL_MAGIC) != 0) {
                return false;
            }
            magic = true;
            continue;
        }
        if (strcmp(line, "--") == 0) {
            /* Everything after this line is the worker's bytes. */
            if (atlas_buf_set(&t->content, p + i, len - i, err) != ATLAS_OK) {
                return false;
            }
            t->found = true;
            t->well_formed = true;
            return true;
        }
        char *eq = strchr(line, '=');
        if (eq == NULL) {
            return false;
        }
        *eq = '\0';
        const char *key = line, *val = eq + 1;
        if (strcmp(key, "stream") == 0) {
            if (strcmp(val, "stdout") != 0 && strcmp(val, "stderr") != 0) {
                return false;
            }
            (void)snprintf(stream_out, 16, "%s", val);
        } else if (strcmp(key, "present") == 0) {
            t->present = strcmp(val, "yes") == 0;
        } else if (strcmp(key, "total_bytes") == 0) {
            if (!line_int(val, &t->total_bytes)) {
                return false;
            }
        } else if (strcmp(key, "tail_bytes") == 0) {
            if (!line_int(val, &t->tail_bytes)) {
                return false;
            }
        } else if (strcmp(key, "truncated") == 0) {
            t->truncated = strcmp(val, "yes") == 0;
        } else if (strcmp(key, "nul_replaced") == 0) {
            if (!line_int(val, &t->nul_replaced)) {
                return false;
            }
        } else {
            /* An unknown key is a header this daemon does not read. */
            return false;
        }
    }
    /* No `--` line: a header with no body is not the grammar. */
    return false;
}

static atlas_status take_log_tail(const atlas_orch_event_row *row, void *ud, atlas_err *err) {
    log_tails *ts = (log_tails *)ud;
    log_tail t;
    char stream[16];
    bool ok = parse_log_tail(row->payload, row->payload_len, &t, stream, err);
    log_tail *slot = NULL;
    if (ok && strcmp(stream, "stdout") == 0) {
        slot = &ts->stdout_tail;
    } else if (ok && strcmp(stream, "stderr") == 0) {
        slot = &ts->stderr_tail;
    }
    if (slot == NULL) {
        /* Malformed, or a stream this daemon does not know: recorded on both
         * slots as "found but not well-formed" only when nothing better exists,
         * so a good event for the same stream is never displaced. */
        atlas_buf_free(&t.content);
        if (!ts->stdout_tail.found) {
            ts->stdout_tail.found = true;
        }
        if (!ts->stderr_tail.found) {
            ts->stderr_tail.found = true;
        }
        return ATLAS_OK;
    }
    /* Rows arrive oldest first, so the newest attempt's event wins. */
    atlas_buf_free(&slot->content);
    *slot = t;
    slot->attempt_no = row->attempt_no;
    return ATLAS_OK;
}

static atlas_status emit_log_tail(dispatch_state *ds, const log_tail *t, const char *key,
                                  bool any_attempt, atlas_err *err) {
    atlas_status st = atlas_json_key(ds->j, key, err);
    if (st == ATLAS_OK) {
        st = atlas_json_obj_begin(ds->j, err);
    }
    const bool available = t->found && t->well_formed && t->present;
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "available", available, err);
    }
    if (st == ATLAS_OK && available) {
        st = atlas_json_key_int(ds->j, "attempt", t->attempt_no, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "total_bytes", t->total_bytes, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "tail_bytes", t->tail_bytes, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_bool(ds->j, "truncated", t->truncated, err);
        }
        if (st == ATLAS_OK && t->nul_replaced > 0) {
            st = atlas_json_key_int(ds->j, "nul_replaced", t->nul_replaced, err);
        }
        /* The same two lines `job.remote_result` emits for a worker's bytes,
         * for the same reason. */
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "encoding", "atlas-safe-1", err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "provenance", "UNTRUSTED_DATA", err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "content",
                                    atlas_safe_n(&ds->safe, t->content.data, t->content.len),
                                    err);
        }
    }
    if (st == ATLAS_OK && !available) {
        const char *why;
        if (!any_attempt) {
            why = "the job never reached an attempt, so no worker ran and no log exists";
        } else if (!t->found) {
            why = "the dispatcher that carried this attempt sent no log excerpt: it predates "
                  "this capability or stopped before it could send one; if the driver wrote a "
                  "log it is in the failed attempt's workspace on the Atlas machine, which this "
                  "daemon cannot read";
        } else if (!t->well_formed) {
            why = "a log excerpt event exists but its header is not in the format this daemon "
                  "reads, so its bytes are not returned";
        } else {
            why = "the driver wrote no log for this stream";
        }
        st = atlas_json_key_str(ds->j, "unavailable_reason", why, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(ds->j, err);
    }
    return st;
}

static atlas_status method_remote_failure(dispatch_state *ds, const atlas_ipc_request *req,
                                          atlas_err *err) {
    atlas_status st = require_remote_submitter(ds, err);
    if (st != ATLAS_OK) {
        return st;
    }

    const char *uid = NULL, *token = NULL;
    if (!atlas_ipc_param_str(req, "job", &uid) || uid == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "which job?");
    }
    if (!atlas_ipc_param_str(req, "token", &token) || token == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "\"token\" is required");
    }

    const atlas_gwpolicy *gw = &ds->ctx->gwpolicy;
    atlas_buf tok = ATLAS_BUF_INIT;
    st = atlas_buf_set_str(&tok, token, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char key_id[ATLAS_APIKEY_SELECTOR_HEX + 1u];
    key_id[0] = '\0';
    st = atlas_orch_remote_verify(ds->db, &tok,
                                  (const char (*)[ATLAS_APIKEY_SELECTOR_HEX + 1u])
                                      gw->remote_submit_keys,
                                  gw->remote_submit_count, key_id, err);
    atlas_buf_free(&tok);
    if (st != ATLAS_OK) {
        return st;
    }

    atlas_orch_job_view v;
    atlas_orch_job_view_init(&v);
    bool found = false;
    st = atlas_db_orch_job_get(ds->db, uid, &v, &found, err);
    /* `job.remote_result`'s scope, in the order that leaks least. */
    if (st == ATLAS_OK && (!found || strcmp(v.submit_key_id, key_id) != 0 ||
                           !atlas_orchpolicy_permits_repo(&ds->ctx->orchpolicy, v.repo_name))) {
        atlas_orch_job_view_free(&v);
        return atlas_err_set(err, ATLAS_ERR_USAGE, "no such job");
    }
    if (st != ATLAS_OK) {
        atlas_orch_job_view_free(&v);
        return st;
    }

    const bool terminal = atlas_orch_state_is_terminal(v.state);
    /* A static name, so it outlives the view it was read from. */
    const char *state_name = atlas_orch_state_name(v.state);
    st = atlas_json_key_str(ds->j, "job", v.job_uid, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "state", state_name, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "repo", v.repo_name, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "key_id", v.submit_key_id, err);
    }
    /* Not a gate on the rest of the answer, unlike `job.remote_result`: a job
     * stuck in QUEUED is exactly one a reader asks this about, and its answer
     * is "no attempt yet", stated below, not an empty document. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "availability", terminal ? "terminal" : "not_terminal",
                                err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "max_attempts", v.max_attempts, err);
    }
    if (st == ATLAS_OK && v.terminal_at[0] != '\0') {
        st = atlas_json_key_str(ds->j, "terminal_at", v.terminal_at, err);
    }
    char reason[64];
    reason[0] = '\0';
    if (st == ATLAS_OK) {
        atlas_err rerr;
        atlas_err_init(&rerr);
        if (atlas_db_orch_job_newest_reason(ds->db, v.job_uid, reason, &rerr) == ATLAS_OK &&
            reason[0] != '\0') {
            st = atlas_json_key_str(ds->j, "reason", reason, err);
        }
    }
    atlas_orch_job_view_free(&v);

    failure_ctx fc;
    memset(&fc, 0, sizeof(fc));
    fc.ds = ds;

    /* Attempts, oldest first. A read failure here is a failure of the method:
     * the attempt rows are the record this method exists to return. */
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "attempts", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_orch_job_attempts(ds->db, uid, emit_attempt_row, &fc, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }

    /* One line Atlas composes from its own closed vocabularies — the newest
     * transition reason and the newest attempt's exit classification. No
     * worker prose enters it. */
    if (st == ATLAS_OK) {
        /* The bounded labels plus two full-width int64 values can exceed 256
         * bytes. Keep the whole explanation in debug/sanitizer builds too. */
        char cause[512];
        if (!fc.newest_set) {
            (void)snprintf(cause, sizeof cause,
                           "no attempt has started; the job is %s%s, newest transition reason %s",
                           state_name, terminal ? "" : " and waiting for a dispatcher",
                           reason[0] != '\0' ? reason : "UNKNOWN");
        } else {
            (void)snprintf(cause, sizeof cause,
                           "attempt %lld ended %s: the worker exited %s (code %lld), "
                           "failure_reason %s; the job's newest transition reason is %s",
                           (long long)fc.newest.attempt_no, fc.newest_state, fc.newest_exit_kind,
                           (long long)fc.newest.exit_code, fc.newest_failure_reason,
                           reason[0] != '\0' ? reason : "UNKNOWN");
        }
        st = atlas_json_key_str(ds->j, "cause", cause, err);
    }

    /* The ledger. */
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "transitions", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_orch_job_transitions(ds->db, uid, emit_transition_row, &fc, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }

    /* The dispatcher's own record of what went wrong, when it sent one. */
    if (st == ATLAS_OK) {
        failure_event fe;
        memset(&fe, 0, sizeof(fe));
        atlas_buf_init(&fe.message);
        atlas_err ferr;
        atlas_err_init(&ferr);
        (void)atlas_db_orch_job_events(ds->db, uid, ATLAS_ORCH_EVENT_KIND_FAILURE, 1,
                                       take_failure_event, &fe, &ferr);
        st = atlas_json_key(ds->j, "dispatcher_error", err);
        if (st == ATLAS_OK) {
            st = atlas_json_obj_begin(ds->j, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_bool(ds->j, "available", fe.found, err);
        }
        if (st == ATLAS_OK && fe.found) {
            st = atlas_json_key_int(ds->j, "attempt", fe.attempt_no, err);
            if (st == ATLAS_OK) {
                st = atlas_json_key_str(ds->j, "at", fe.at, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_key_str(ds->j, "provenance", "UNTRUSTED_DATA", err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_key_str(ds->j, "message",
                                        atlas_safe_n(&ds->safe, fe.message.data, fe.message.len),
                                        err);
            }
        }
        if (st == ATLAS_OK && !fe.found) {
            st = atlas_json_key_str(
                ds->j, "unavailable_reason",
                fc.attempts == 0
                    ? "no attempt has started, so no dispatcher has reported anything"
                    : "no failure record was carried: the attempt's exit classification above is "
                      "the whole record, or the dispatcher that ran it predates this capability",
                err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_obj_end(ds->j, err);
        }
        atlas_buf_free(&fe.message);
    }

    /* The newest events, the worker-labelled narrative. Bounded so an attempt
     * that emitted the maximum still answers in one frame. */
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "events", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    if (st == ATLAS_OK) {
        atlas_err eerr;
        atlas_err_init(&eerr);
        (void)atlas_db_orch_job_events(ds->db, uid, NULL, ATLAS_ORCH_FAILURE_EVENTS_MAX,
                                       emit_event_row, &fc, &eerr);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }

    /* The two stream tails, parsed from their headers. */
    if (st == ATLAS_OK) {
        log_tails ts;
        memset(&ts, 0, sizeof(ts));
        atlas_buf_init(&ts.stdout_tail.content);
        atlas_buf_init(&ts.stderr_tail.content);
        atlas_err terr;
        atlas_err_init(&terr);
        /* Two per attempt at most; the newest attempt's pair wins. */
        (void)atlas_db_orch_job_events(ds->db, uid, ATLAS_ORCH_EVENT_KIND_LOG_TAIL,
                                       2 * ATLAS_ORCH_MAX_ATTEMPTS, take_log_tail, &ts, &terr);
        st = atlas_json_key(ds->j, "worker_log", err);
        if (st == ATLAS_OK) {
            st = atlas_json_obj_begin(ds->j, err);
        }
        if (st == ATLAS_OK) {
            st = emit_log_tail(ds, &ts.stdout_tail, "stdout", fc.attempts > 0, err);
        }
        if (st == ATLAS_OK) {
            st = emit_log_tail(ds, &ts.stderr_tail, "stderr", fc.attempts > 0, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_obj_end(ds->j, err);
        }
        atlas_buf_free(&ts.stdout_tail.content);
        atlas_buf_free(&ts.stderr_tail.content);
    }
    return st;
}

/* --- job.remote_list ------------------------------------------------------- */

static atlas_status method_remote_list(dispatch_state *ds, const atlas_ipc_request *req,
                                       atlas_err *err) {
    atlas_status st = require_remote_submitter(ds, err);
    if (st != ATLAS_OK) {
        return st;
    }

    const char *token = NULL;
    if (!atlas_ipc_param_str(req, "token", &token) || token == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "\"token\" is required");
    }

    const atlas_gwpolicy *gw = &ds->ctx->gwpolicy;
    atlas_buf tok = ATLAS_BUF_INIT;
    st = atlas_buf_set_str(&tok, token, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char key_id[ATLAS_APIKEY_SELECTOR_HEX + 1u];
    key_id[0] = '\0';
    st = atlas_orch_remote_verify(ds->db, &tok,
                                  (const char (*)[ATLAS_APIKEY_SELECTOR_HEX + 1u])
                                      gw->remote_submit_keys,
                                  gw->remote_submit_count, key_id, err);
    atlas_buf_free(&tok);
    if (st != ATLAS_OK) {
        return st;
    }

    int64_t after = 0, limit = 0;
    (void)atlas_ipc_param_int(req, "after", &after);
    (void)atlas_ipc_param_int(req, "limit", &limit);
    if (after < 0) {
        after = 0;
    }

    st = atlas_json_key(ds->j, "jobs", err);
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    remote_list_ctx lc = {ds, err};
    int64_t count = 0, cursor = after;
    bool more = false;
    if (st == ATLAS_OK) {
        st = atlas_db_orch_job_list_by_key(ds->db, key_id, after, limit, emit_remote_job, &lc,
                                           &count, &cursor, &more, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "count", count, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "cursor", cursor, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "more", more, err);
    }
    return st;
}

/* --- job.remote_cancel ----------------------------------------------------- */

static atlas_status method_remote_cancel(dispatch_state *ds, const atlas_ipc_request *req,
                                         atlas_err *err) {
    atlas_status st = require_remote_submitter(ds, err);
    if (st != ATLAS_OK) {
        return st;
    }

    const char *uid = NULL, *token = NULL;
    if (!atlas_ipc_param_str(req, "job", &uid) || uid == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "which job?");
    }
    if (!atlas_ipc_param_str(req, "token", &token) || token == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "\"token\" is required");
    }

    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_CANCEL);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    op->peer_uid = ds->peer_uid;
    op->actor = ATLAS_ORCH_ACTOR_CLIENT;
    /* peer_is_operator: set from SO_PEERCRED, never a parameter. */
    op->peer_is_operator = atlas_server_peer_is_operator(ds->peer_uid);

    atlas_status st2 = atlas_buf_set_str(&op->job_uid, uid, err);
    if (st2 == ATLAS_OK) {
        st2 = atlas_buf_set_str(&op->remote_token, token, err);
    }
    if (st2 == ATLAS_OK) {
        const atlas_gwpolicy *gw = &ds->ctx->gwpolicy;
        op->remote_allowed_count = gw->remote_submit_count;
        for (size_t i = 0; i < gw->remote_submit_count; i++) {
            (void)snprintf(op->remote_allowed_ids[i], ATLAS_APIKEY_SELECTOR_HEX + 1u, "%s",
                           gw->remote_submit_keys[i]);
        }
    }
    if (st2 != ATLAS_OK) {
        atlas_orch_op_free(op);
        free(op);
        return st2;
    }

    atlas_orch_result r;
    atlas_orch_result_init(&r);
    st = atlas_server_orch_write(ds, op, 5000, NULL, &r, err);
    if (st == ATLAS_OK) {
        st = atlas_server_write_job_summary(ds, &r, err);
    }
    atlas_orch_result_free(&r);
    return st;
}

/* --- method table ---------------------------------------------------------- */

static const atlas_method_entry REMOTE_SUBMIT_METHODS[] = {
    {"job.remote_submit", method_remote_submit},
    {"job.remote_get", method_remote_get},
    {"job.remote_result", method_remote_result},
    {"job.remote_failure", method_remote_failure},
    {"job.remote_list", method_remote_list},
    {"job.remote_cancel", method_remote_cancel},
};

const atlas_method_entry *atlas_server_remote_submit_methods(size_t *count_out) {
    *count_out = sizeof REMOTE_SUBMIT_METHODS / sizeof REMOTE_SUBMIT_METHODS[0];
    return REMOTE_SUBMIT_METHODS;
}
