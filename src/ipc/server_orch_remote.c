/* Atlas - A14: the gateway's remote job submission group.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * This file holds the four methods the gateway's uid may call to queue, query
 * and cancel jobs submitted through bearer credentials: `job.remote_submit`,
 * `job.remote_get`, `job.remote_list`, `job.remote_cancel`.
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
 * lifecycle transition reserved for the decision layer; "artifact" would
 * expose a worker's output to a remote credential, closing the chain Decision 7
 * writes down but does not close; "log" likewise; "run" is the dispatch-level
 * concept the gateway may not touch.
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
    {"job.remote_list", method_remote_list},
    {"job.remote_cancel", method_remote_cancel},
};

const atlas_method_entry *atlas_server_remote_submit_methods(size_t *count_out) {
    *count_out = sizeof REMOTE_SUBMIT_METHODS / sizeof REMOTE_SUBMIT_METHODS[0];
    return REMOTE_SUBMIT_METHODS;
}
