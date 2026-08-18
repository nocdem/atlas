/* Atlas - A8: the orchestration method groups, and the line between them.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Two groups, looked up separately in `server.c` by the peer's uid:
 *
 *   `job.`      submit, get, list, cancel, artifact, run_status — for a submitter.
 *   `dispatch.` lease, heartbeat, event, complete              — for the dispatcher.
 *
 * The selection is on `SO_PEERCRED` and on nothing else. A uid, gid, pid or role
 * in the request body is a client describing itself, which is not evidence about
 * itself, and there is no code path here that reads one.
 *
 * ## What neither group can do
 *
 * Approve, reject, supersede or revalidate a decision; issue or spend a
 * lifecycle capability; add or remove a repository; create, read, verify or
 * restore a backup; plan or apply a prune; or write any table outside the eight
 * `orch_*` ones. Those methods do not exist in the protocol — A5 and A7 deleted
 * them rather than leaving them refusing, and A8 does not put anything back.
 * `tests/test_orch_rpc.c` asks a live daemon for every name such a method would
 * plausibly have, from both a client and a dispatcher connection, and requires
 * every one to answer `unknown method`.
 *
 * ## Why a dispatcher tier is not a privileged tier
 *
 * A7.1's rule is that the socket carries no authority. It still does not. The
 * `dispatch.` group is *disjoint* from the client group rather than a superset
 * of it: it concerns leases, heartbeats, events and results for jobs an operator
 * already created, and it can neither create work nor read another principal's.
 * Every one of its methods additionally requires a bearer lease token, so the
 * uid is necessary and never sufficient. And membership is a root-owned
 * configuration fact — `/etc/atlas/orchestration.conf`, which neither `atlasd`
 * nor `atlas-worker` can write — exactly like the A7.1 client allowlist.
 */
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/db.h"
#include "atlas/git.h"
#include "atlas/orch_ops.h"
#include "atlas/orchpolicy.h"
#include "atlas/sha256.h"
#include "atlas/snapshot.h"
#include "server_internal.h"

/* --- shared refusals -------------------------------------------------------
 *
 * The refusal text names the policy rather than the caller. An error that told a
 * peer which uid *would* have been accepted is an error that enumerates the
 * deployment for anybody who can open the socket. */
static atlas_status orch_disabled(dispatch_state *ds, atlas_err *err) {
    const atlas_orchpolicy *p = &ds->ctx->orchpolicy;
    return atlas_err_set(err, ATLAS_ERR_CONFIG,
                         "orchestration is not enabled on this Atlas (%s)",
                         atlas_orchpolicy_reason_name(p->reason));
}

static atlas_status require_submitter(dispatch_state *ds, atlas_err *err) {
    const atlas_orchpolicy *p = &ds->ctx->orchpolicy;
    if (p->state != ATLAS_ORCHPOLICY_ENABLED) {
        return orch_disabled(ds, err);
    }
    if (!atlas_orchpolicy_permits_submitter(p, ds->peer_uid)) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "this connection may not submit or read jobs");
    }
    return ATLAS_OK;
}

static atlas_status require_dispatcher(dispatch_state *ds, atlas_err *err) {
    const atlas_orchpolicy *p = &ds->ctx->orchpolicy;
    if (p->state != ATLAS_ORCHPOLICY_ENABLED) {
        return orch_disabled(ds, err);
    }
    /* Either dispatcher may reach the group. *Which* jobs each may lease is a
     * separate question, answered by the driver filter on the lease request and
     * enforced against the job's stored driver. */
    if (!atlas_orchpolicy_is_any_dispatcher(p, ds->peer_uid)) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "this connection may not act as the job dispatcher");
    }
    return ATLAS_OK;
}

/* --- writing a result ------------------------------------------------------ */

static atlas_status write_job_summary(dispatch_state *ds, const atlas_orch_result *r,
                                      atlas_err *err) {
    atlas_status st = atlas_json_key_str(ds->j, "job", atlas_buf_cstr(&r->job_uid), err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "state", atlas_orch_state_name(r->state), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "seq", r->seq, err);
    }
    /* A11.0's run, when the operation settled one. Absent rather than empty for
     * a job that belongs to none, so a reader never has to decide whether an
     * empty identifier means "no run" or "a run nobody named". */
    if (st == ATLAS_OK && r->run_uid.len > 0) {
        st = atlas_json_key_str(ds->j, "run", atlas_buf_cstr(&r->run_uid), err);
    }
    return st;
}

/* --- job.submit -------------------------------------------------------------
 *
 * The heaviest validation in A8, and all of it happens before anything is
 * queued. In order: the policy admits the caller, the repository is one the
 * policy permits *and* is registered, the mode and driver are in the policy's
 * vocabularies, the pinned commit is resolved from trusted state, the bounds are
 * checked against the policy's ceilings, and only then is the specification
 * canonicalised, digested and handed to the writer thread.
 *
 * Nothing here accepts a path. A repository is named, and the name is resolved
 * through the registry — so an unregistered directory cannot be reached at all,
 * and a client never supplies a filesystem location.
 */
static atlas_status method_job_submit(dispatch_state *ds, const atlas_ipc_request *req,
                                      atlas_err *err) {
    atlas_status st = require_submitter(ds, err);
    if (st != ATLAS_OK) {
        return st;
    }
    const atlas_orchpolicy *policy = &ds->ctx->orchpolicy;

    const char *repo = NULL, *mode = NULL, *driver = NULL, *task = NULL;
    if (!atlas_ipc_param_str(req, "repo", &repo) || repo == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a job needs a repository name");
    }
    if (!atlas_ipc_param_str(req, "task", &task) || task == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a job needs task text");
    }
    if (!atlas_ipc_param_str(req, "mode", &mode)) {
        mode = NULL;
    }
    if (!atlas_ipc_param_str(req, "driver", &driver)) {
        driver = NULL;
    }
    /* A repository the policy does not list is refused before the registry is
     * consulted: which directories Atlas will snapshot is an operator's
     * decision, not a submitter's. */
    if (!atlas_orchpolicy_permits_repo(policy, repo)) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "the orchestration policy does not permit jobs against that "
                             "repository");
    }
    if (mode != NULL && !atlas_orchpolicy_permits_mode(policy, mode)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "that job mode is not configured");
    }
    if (driver != NULL && !atlas_orchpolicy_permits_driver(policy, driver)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "that driver is not configured");
    }
    if (mode == NULL) {
        mode = policy->modes[0];
    }
    if (driver == NULL) {
        driver = policy->drivers[0];
    }

    atlas_repo_info ri;
    atlas_repo_info_init(&ri);
    bool found = false;
    st = atlas_db_repo_get(ds->db, repo, &ri, &found, err);
    if (st == ATLAS_OK && !found) {
        st = atlas_err_set(err, ATLAS_ERR_REPO, "no repository named that is registered");
    }
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&ri);
        return st;
    }
    /* The source commit comes from the *index*, not from the request and not
     * from a fresh git call: the daemon's read handle already holds the commit
     * the last pass ingested, and pinning that is what makes the stored
     * specification describe an exact tree. A client may not name a branch,
     * because a moving reference is not a specification. */
    if (ri.scanned_head[0] == '\0') {
        atlas_repo_info_free(&ri);
        return atlas_err_set(err, ATLAS_ERR_REPO,
                             "that repository has never been scanned, so Atlas cannot pin a "
                             "source commit for a job");
    }

    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_SUBMIT);
    if (op == NULL) {
        atlas_repo_info_free(&ri);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory building a job");
    }
    /* From the kernel, never from the document. */
    op->peer_uid = ds->peer_uid;
    op->actor = ATLAS_ORCH_ACTOR_CLIENT;
    op->repo_id = ri.id;
    op->spec.submitter_uid = ds->peer_uid;

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
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op->spec.mode, mode, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op->spec.driver, driver, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op->spec.task_text, task, err);
    }
    atlas_repo_info_free(&ri);

    const char *key = NULL, *corr = NULL, *parent = NULL;
    if (st == ATLAS_OK && atlas_ipc_param_str(req, "idempotency_key", &key) && key != NULL) {
        st = atlas_buf_set_str(&op->spec.idempotency_key, key, err);
    }
    if (st == ATLAS_OK && atlas_ipc_param_str(req, "correlation", &corr) && corr != NULL) {
        st = atlas_buf_set_str(&op->spec.correlation, corr, err);
    }
    if (st == ATLAS_OK && atlas_ipc_param_str(req, "parent", &parent) && parent != NULL) {
        st = atlas_buf_set_str(&op->spec.parent_job_uid, parent, err);
    }

    /* The one aggregate the protocol accepts is an array of strings, so the
     * declared path set arrives as one directly. */
    if (st == ATLAS_OK) {
        const atlas_ipc_array *arr = NULL;
        if (atlas_ipc_param_array(req, "allowed_path", &arr)) {
            size_t n = atlas_ipc_array_len(arr);
            if (n > ATLAS_ORCH_MAX_ALLOWED_PATHS) {
                st = atlas_err_set(err, ATLAS_ERR_USAGE, "a job may declare at most %u paths",
                                   (unsigned)ATLAS_ORCH_MAX_ALLOWED_PATHS);
            }
            for (size_t i = 0; st == ATLAS_OK && i < n; i++) {
                const char *pv = NULL;
                if (!atlas_ipc_array_str(arr, i, &pv) || pv == NULL) {
                    st = atlas_err_set(err, ATLAS_ERR_USAGE,
                                       "declared path %zu is not a string", i);
                    break;
                }
                st = atlas_buf_set_str(&op->spec.allowed_paths[i], pv, err);
                op->spec.allowed_path_count = i + 1u;
            }
        }
    }

    /* A validation command arrives as a *length-prefixed argv encoding*, one
     * string per command — `4:make,4:test,` — and never as a shell string.
     *
     * That is the same canonical encoding the database stores, so there is one
     * encoding in the system rather than a wire form and a storage form that can
     * disagree. It is also the point at which "A8 never passes untrusted text to
     * a shell" stops being a promise: there is no field here that could hold a
     * shell fragment, because a command is a vector of counted arguments. */
    if (st == ATLAS_OK) {
        const atlas_ipc_array *arr = NULL;
        if (atlas_ipc_param_array(req, "validation", &arr)) {
            size_t n = atlas_ipc_array_len(arr);
            if (n > ATLAS_ORCH_MAX_VALIDATIONS) {
                st = atlas_err_set(err, ATLAS_ERR_USAGE,
                                   "a job may declare at most %u validations",
                                   (unsigned)ATLAS_ORCH_MAX_VALIDATIONS);
            }
            for (size_t i = 0; st == ATLAS_OK && i < n; i++) {
                const char *enc = NULL;
                if (!atlas_ipc_array_str(arr, i, &enc) || enc == NULL) {
                    st = atlas_err_set(err, ATLAS_ERR_USAGE, "validation %zu is not a string", i);
                    break;
                }
                atlas_buf one = ATLAS_BUF_INIT;
                st = atlas_buf_appendf(&one, err, "1:%s", enc);
                if (st == ATLAS_OK) {
                    atlas_orch_argv tmp[1];
                    atlas_orch_argv_init(&tmp[0]);
                    size_t got = 0;
                    st = atlas_orch_validations_decode(atlas_buf_cstr(&one), tmp, 1u, &got, err);
                    if (st == ATLAS_OK && got == 1u) {
                        for (size_t k = 0; st == ATLAS_OK && k < tmp[0].count; k++) {
                            st = atlas_orch_argv_push(&op->spec.validations[i],
                                                      tmp[0].args[k].data, tmp[0].args[k].len,
                                                      err);
                        }
                        op->spec.validation_count = i + 1u;
                    } else if (st == ATLAS_OK) {
                        st = atlas_err_set(err, ATLAS_ERR_USAGE,
                                           "validation %zu is not a length-prefixed argv", i);
                    }
                    atlas_orch_argv_free(&tmp[0]);
                }
                atlas_buf_free(&one);
            }
        }
    }

    if (st == ATLAS_OK) {
        (void)atlas_ipc_param_int(req, "wall_timeout_ms", &op->spec.wall_timeout_ms);
        (void)atlas_ipc_param_int(req, "idle_timeout_ms", &op->spec.idle_timeout_ms);
        (void)atlas_ipc_param_int(req, "max_attempts", &op->spec.max_attempts);
        (void)atlas_ipc_param_int(req, "max_output_bytes", &op->spec.max_output_bytes);
        (void)atlas_ipc_param_int(req, "max_artifact_bytes", &op->spec.max_artifact_bytes);
        (void)atlas_ipc_param_int(req, "max_artifact_count", &op->spec.max_artifact_count);
        /* Zero takes the policy's ceiling as the default; a value above it is
         * refused with the ceiling named, never clamped. */
        st = atlas_orchpolicy_apply_limits(policy, &op->spec, err);
    }
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

    atlas_orch_result r;
    atlas_orch_result_init(&r);
    /* Ownership of `op` passes to the writer unconditionally. */
    st = atlas_writer_orch(ds->ctx->writer, op, 5000, &r, err);
    if (st == ATLAS_OK) {
        st = write_job_summary(ds, &r, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_bool(ds->j, "duplicate", r.duplicate, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "spec_digest", r.spec_digest, err);
        }
    }
    atlas_orch_result_free(&r);
    return st;
}

/* --- job.cancel ------------------------------------------------------------- */

static atlas_status method_job_cancel(dispatch_state *ds, const atlas_ipc_request *req,
                                      atlas_err *err) {
    atlas_status st = require_submitter(ds, err);
    if (st != ATLAS_OK) {
        return st;
    }
    const char *uid = NULL;
    if (!atlas_ipc_param_str(req, "job", &uid) || uid == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "which job?");
    }
    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_CANCEL);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    op->peer_uid = ds->peer_uid;
    op->actor = ATLAS_ORCH_ACTOR_CLIENT;
    st = atlas_buf_set_str(&op->job_uid, uid, err);
    if (st != ATLAS_OK) {
        atlas_orch_op_free(op);
        free(op);
        return st;
    }
    atlas_orch_result r;
    atlas_orch_result_init(&r);
    st = atlas_writer_orch(ds->ctx->writer, op, 5000, &r, err);
    if (st == ATLAS_OK) {
        st = write_job_summary(ds, &r, err);
    }
    atlas_orch_result_free(&r);
    return st;
}

/* --- job.get and job.list ----------------------------------------------------
 *
 * Both are scoped to the calling principal. A client sees its own jobs and
 * nothing else: whether another principal's job exists is itself information,
 * and a caller who may not act on it need not learn it. */
static atlas_status method_job_get(dispatch_state *ds, const atlas_ipc_request *req,
                                   atlas_err *err) {
    atlas_status st = require_submitter(ds, err);
    if (st != ATLAS_OK) {
        return st;
    }
    const char *uid = NULL;
    if (!atlas_ipc_param_str(req, "job", &uid) || uid == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "which job?");
    }
    atlas_orch_job_view v;
    atlas_orch_job_view_init(&v);
    bool found = false;
    st = atlas_db_orch_job_get(ds->db, uid, &v, &found, err);
    if (st == ATLAS_OK && (!found || v.submitter_uid != ds->peer_uid)) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "no such job");
    }
    if (st != ATLAS_OK) {
        atlas_orch_job_view_free(&v);
        return st;
    }
    struct {
        const char *key;
        const char *val;
    } strs[] = {
        {"job", v.job_uid},        {"state", atlas_orch_state_name(v.state)},
        {"repo", v.repo_name},     {"commit", v.source_commit},
        {"mode", v.mode},          {"driver", v.driver},
        {"spec_digest", v.spec_digest}, {"created_at", v.created_at},
    };
    for (size_t i = 0; st == ATLAS_OK && i < sizeof strs / sizeof strs[0]; i++) {
        st = atlas_json_key_str(ds->j, strs[i].key, strs[i].val, err);
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
    /* Task text is UNTRUSTED_DATA: it is what a submitter typed, it is stored
     * verbatim, and it is safe-encoded here rather than reproduced. The label
     * says so, because a reader that treats it as Atlas' own words is the
     * mistake this field invites. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "task_encoding", "atlas-safe-1", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "task", atlas_safe(&ds->safe, atlas_buf_cstr(&v.task_text)),
                                err);
    }
    atlas_orch_job_view_free(&v);
    return st;
}

typedef struct list_ctx {
    dispatch_state *ds;
    atlas_err *err;
} list_ctx;

static atlas_status emit_job(const atlas_orch_list_row *row, void *ud, atlas_err *err) {
    list_ctx *lc = (list_ctx *)ud;
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
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(lc->ds->j, err);
    }
    return st;
}

static atlas_status method_job_list(dispatch_state *ds, const atlas_ipc_request *req,
                                    atlas_err *err) {
    atlas_status st = require_submitter(ds, err);
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
    list_ctx lc = {ds, err};
    int64_t count = 0, cursor = after;
    bool more = false;
    if (st == ATLAS_OK) {
        st = atlas_db_orch_job_list(ds->db, ds->peer_uid, after, limit, emit_job, &lc, &count,
                                    &cursor, &more, err);
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
        /* Explicit, for A1's reason: a page that silently ends is
         * indistinguishable from the end of the list. */
        st = atlas_json_key_bool(ds->j, "more", more, err);
    }
    return st;
}

/* --- job.artifact ------------------------------------------------------------
 *
 * An artifact is addressed by its **server-assigned id** and never by a path.
 * There is no path column in `orch_artifacts` and no parameter here that could
 * hold one, so "artifact paths are server-resolved identifiers" is a property of
 * the schema and the signature rather than a check that could be forgotten.
 *
 * Content is served only when the worker sent it inline, which it does only for
 * artifacts below `ATLAS_ORCH_ARTIFACT_INLINE_MAX` — the daemon cannot read the
 * worker's workspace (0700 `atlas-worker`), which is the isolation A8 is for. A
 * larger artifact is described by name, size and digest, and `stored` says so
 * explicitly: a reader must never be left to infer that an absent blob means an
 * empty file.
 */
typedef struct artifact_ctx {
    dispatch_state *ds;
    bool with_content;
} artifact_ctx;

static atlas_status emit_artifact(const atlas_orch_artifact_row *row, void *ud, atlas_err *err) {
    artifact_ctx *ac = (artifact_ctx *)ud;
    dispatch_state *ds = ac->ds;
    atlas_status st = atlas_json_obj_begin(ds->j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "id", row->id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "attempt", row->attempt_no, err);
    }
    /* The name and kind came from a worker. They were checked to be a safe
     * relative path and a name when they were stored; encoding them here is the
     * second layer rather than the first. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "name", atlas_safe(&ds->safe, row->name), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "kind", atlas_safe(&ds->safe, row->kind), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "size_bytes", row->size_bytes, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "sha256", row->sha256, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "stored", row->content_stored, err);
    }
    if (st == ATLAS_OK && ac->with_content) {
        if (!row->content_stored) {
            /* Said plainly rather than answered with an empty string. The bytes
             * exist in the worker's workspace and Atlas cannot read them;
             * pretending otherwise would be a lie about a digest the caller can
             * check for itself. */
            st = atlas_json_key_str(ds->j, "content_unavailable",
                                    "the artifact exceeded the inline bound and its bytes remain "
                                    "in the worker workspace",
                                    err);
        } else {
            /* Artifact bytes are a model's output: UNTRUSTED_DATA, labelled as
             * such and safe-encoded, so a patch full of control bytes cannot
             * reach a terminal as escapes. */
            atlas_buf tmp = ATLAS_BUF_INIT;
            st = atlas_buf_set(&tmp, row->content, row->content_len, err);
            if (st == ATLAS_OK) {
                st = atlas_json_key_str(ds->j, "content_encoding", "atlas-safe-1", err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_key_str(ds->j, "content_provenance", "UNTRUSTED_DATA", err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_key_str(ds->j, "content",
                                        atlas_safe(&ds->safe, atlas_buf_cstr(&tmp)), err);
            }
            atlas_buf_free(&tmp);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(ds->j, err);
    }
    return st;
}

static atlas_status method_job_artifact(dispatch_state *ds, const atlas_ipc_request *req,
                                        atlas_err *err) {
    atlas_status st = require_submitter(ds, err);
    if (st != ATLAS_OK) {
        return st;
    }
    const char *uid = NULL;
    if (!atlas_ipc_param_str(req, "job", &uid) || uid == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "which job?");
    }
    int64_t artifact_id = 0;
    bool want_one = atlas_ipc_param_int(req, "artifact", &artifact_id);
    bool want_content = false;
    (void)atlas_ipc_param_bool(req, "content", &want_content);

    /* The job is resolved first and scoped to the caller, so an artifact id
     * belonging to somebody else's job cannot be reached by guessing a number.
     * A job the caller may not see reports "no such job", not "forbidden". */
    atlas_orch_job_view v;
    atlas_orch_job_view_init(&v);
    bool found = false;
    st = atlas_db_orch_job_get(ds->db, uid, &v, &found, err);
    long long submitter = v.submitter_uid;
    atlas_orch_job_view_free(&v);
    if (st != ATLAS_OK) {
        return st;
    }
    if (!found || submitter != ds->peer_uid) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "no such job");
    }

    st = atlas_json_key_str(ds->j, "job", uid, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "artifacts", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    artifact_ctx ac = {ds, want_content && want_one};
    int64_t count = 0;
    if (st == ATLAS_OK) {
        st = atlas_db_orch_artifacts(ds->db, uid, want_one ? artifact_id : 0, ac.with_content,
                                     emit_artifact, &ac, &count, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "count", count, err);
    }
    return st;
}

/* --- job.run_status (A11.1) --------------------------------------------------------
 *
 * A read, and only a read. There is no `job.run_settle`, no `job.run_accept`
 * and no `job.run_block`: a run's status changes inside the completion that
 * justifies it
 * and nowhere else, which is what makes "a model payload cannot accept a run"
 * true by absence. Adding a method here that writes would undo that in one
 * line, so this one is the only member of its family and is expected to stay
 * that way.
 *
 * Scoped like every other client read: through the run's root task's submitter.
 * A run belonging to somebody else answers "no such run", never "forbidden".
 */
static atlas_status method_run_get(dispatch_state *ds, const atlas_ipc_request *req,
                                   atlas_err *err) {
    atlas_status st = require_submitter(ds, err);
    if (st != ATLAS_OK) {
        return st;
    }
    const char *uid = NULL;
    if (!atlas_ipc_param_str(req, "run", &uid) || uid == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "which run?");
    }
    atlas_orch_run_view rv;
    memset(&rv, 0, sizeof(rv));
    bool found = false;
    st = atlas_db_orch_run_get(ds->db, uid, &rv, &found, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (!found) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "no such run");
    }
    atlas_orch_job_view root;
    atlas_orch_job_view_init(&root);
    bool root_found = false;
    st = atlas_db_orch_job_get(ds->db, rv.root_job_uid, &root, &root_found, err);
    long long submitter = root.submitter_uid;
    atlas_orch_job_view_free(&root);
    if (st != ATLAS_OK) {
        return st;
    }
    if (!root_found || submitter != ds->peer_uid) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "no such run");
    }

    struct {
        const char *key;
        const char *val;
    } strs[] = {
        {"run", rv.run_uid},
        {"root", rv.root_job_uid},
        {"status", atlas_orch_run_status_name(rv.status)},
        {"created_at", rv.created_at},
    };
    for (size_t i = 0; st == ATLAS_OK && i < sizeof strs / sizeof strs[0]; i++) {
        st = atlas_json_key_str(ds->j, strs[i].key, strs[i].val, err);
    }
    /* The active task is emitted only when there is one. An empty run_uid-shaped
     * string would read as an identifier; an absent key reads as what it is. */
    if (st == ATLAS_OK && rv.active_job_uid[0] != '\0') {
        st = atlas_json_key_str(ds->j, "active", rv.active_job_uid, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "active_state",
                                    atlas_orch_state_name(rv.active_state), err);
        }
    }
    return st;
}

/* --- dispatch.lease ---------------------------------------------------------- */

static atlas_status method_dispatch_lease(dispatch_state *ds, const atlas_ipc_request *req,
                                          atlas_err *err) {
    atlas_status st = require_dispatcher(ds, err);
    if (st != ATLAS_OK) {
        return st;
    }
    const char *who = NULL;
    if (!atlas_ipc_param_str(req, "dispatcher", &who) || who == NULL) {
        who = "";
    }
    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_LEASE);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    op->peer_uid = ds->peer_uid;
    op->actor = ATLAS_ORCH_ACTOR_DISPATCHER;
    /* An identifier the dispatcher chose for itself, recorded so a restart is
     * visible in the history. It is *not* an authorisation and nothing branches
     * on it; the uid decided that, above. */
    st = atlas_buf_set_str(&op->dispatcher_id, who, err);
    if (st == ATLAS_OK) {
        /* A11.1. A lease may name the one job it wants. Absent is A8's "give me
         * whatever is next"; present is a run driver claiming its own run's
         * active task and nothing else. The daemon still decides whether the
         * named job is grantable — a name is a narrowing, never a permission. */
        const char *want_job = NULL;
        if (atlas_ipc_param_str(req, "job", &want_job) && want_job != NULL) {
            st = atlas_buf_set_str(&op->job_uid, want_job, err);
        }
    }
    if (st == ATLAS_OK) {
        /* A netstring-encoded driver list, built with the typed writer like every
         * other list on this protocol. Absent means "any driver". */
        const atlas_ipc_array *arr = NULL;
        if (atlas_ipc_param_array(req, "driver", &arr)) {
            atlas_orch_argv one;
            atlas_orch_argv_init(&one);
            size_t n = atlas_ipc_array_len(arr);
            for (size_t i = 0; st == ATLAS_OK && i < n && i < ATLAS_ORCH_MAX_ARGV; i++) {
                const char *v = NULL;
                if (atlas_ipc_array_str(arr, i, &v) && v != NULL) {
                    st = atlas_orch_argv_push(&one, v, strlen(v), err);
                }
            }
            if (st == ATLAS_OK) {
                st = atlas_orch_validations_encode(&one, 1u, &op->lease_drivers, err);
            }
            atlas_orch_argv_free(&one);
        }
    }
    if (st != ATLAS_OK) {
        atlas_orch_op_free(op);
        free(op);
        return st;
    }
    atlas_orch_result r;
    atlas_orch_result_init(&r);
    st = atlas_writer_orch(ds->ctx->writer, op, 5000, &r, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "granted", r.granted, err);
    }
    if (st == ATLAS_OK && r.granted) {
        /* The token leaves here once and is never retrievable again. */
        struct {
            const char *key;
            const char *val;
        } strs[] = {
            {"job", atlas_buf_cstr(&r.job_uid)},
            {"token", atlas_buf_cstr(&r.token)},
            {"repo", atlas_buf_cstr(&r.repo_name)},
            {"repo_root", atlas_buf_cstr(&r.repo_root)},
            {"commit", atlas_buf_cstr(&r.source_commit)},
            {"mode", atlas_buf_cstr(&r.mode)},
            {"driver", atlas_buf_cstr(&r.driver)},
            {"allowed_paths", atlas_buf_cstr(&r.allowed_paths)},
            {"validations", atlas_buf_cstr(&r.validations)},
            {"spec_digest", r.spec_digest},
        };
        for (size_t i = 0; st == ATLAS_OK && i < sizeof strs / sizeof strs[0]; i++) {
            st = atlas_json_key_str(ds->j, strs[i].key, strs[i].val, err);
        }
        struct {
            const char *key;
            int64_t val;
        } ints[] = {
            {"attempt", r.attempt_no},
            {"expires_ms", r.expires_ms},
            {"wall_timeout_ms", r.wall_timeout_ms},
            {"idle_timeout_ms", r.idle_timeout_ms},
            {"max_output_bytes", r.max_output_bytes},
            {"max_artifact_bytes", r.max_artifact_bytes},
            {"max_artifact_count", r.max_artifact_count},
        };
        for (size_t i = 0; st == ATLAS_OK && i < sizeof ints / sizeof ints[0]; i++) {
            st = atlas_json_key_int(ds->j, ints[i].key, ints[i].val, err);
        }
        /* Task text is what a submitter typed. It reaches the driver, so it is
         * labelled here as untrusted rather than encoded: the worker needs the
         * bytes, and the label is what tells it how to treat them. */
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "task_provenance", "UNTRUSTED_DATA", err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "task", atlas_buf_cstr(&r.task_text), err);
        }
    }
    atlas_orch_result_free(&r);
    return st;
}

/* --- dispatch.heartbeat, dispatch.event, dispatch.complete --------------------
 *
 * Each carries the bearer token and nothing that identifies the worker: the
 * token names the attempt, and everything else a worker might say about itself
 * is recorded as its claim and used for nothing. */
static atlas_status take_token(const atlas_ipc_request *req, atlas_orch_op *op, atlas_err *err) {
    const char *tok = NULL;
    if (!atlas_ipc_param_str(req, "token", &tok) || tok == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a worker message needs its lease token");
    }
    return atlas_buf_set_str(&op->token, tok, err);
}

static atlas_status method_dispatch_heartbeat(dispatch_state *ds, const atlas_ipc_request *req,
                                              atlas_err *err) {
    atlas_status st = require_dispatcher(ds, err);
    if (st != ATLAS_OK) {
        return st;
    }
    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_HEARTBEAT);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    op->peer_uid = ds->peer_uid;
    op->actor = ATLAS_ORCH_ACTOR_DISPATCHER;
    st = take_token(req, op, err);
    const char *phase = NULL;
    if (st == ATLAS_OK && atlas_ipc_param_str(req, "phase", &phase) && phase != NULL) {
        if (!atlas_orch_state_parse(phase, &op->phase)) {
            st = atlas_err_set(err, ATLAS_ERR_USAGE, "that is not a job phase");
        }
    }
    if (st == ATLAS_OK) {
        (void)atlas_ipc_param_int(req, "pid", &op->claimed_pid);
    }
    if (st != ATLAS_OK) {
        atlas_orch_op_free(op);
        free(op);
        return st;
    }
    atlas_orch_result r;
    atlas_orch_result_init(&r);
    st = atlas_writer_orch(ds->ctx->writer, op, 5000, &r, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "state", atlas_orch_state_name(r.state), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "expires_ms", r.expires_ms, err);
    }
    if (st == ATLAS_OK) {
        /* How a worker learns it must stop. There is no signal from the daemon
         * to a worker process and there must not be one: the daemon has no path
         * into the worker's process tree, which is the isolation A8 is for. */
        st = atlas_json_key_bool(ds->j, "cancel_requested", r.cancel_requested, err);
    }
    atlas_orch_result_free(&r);
    return st;
}

static atlas_status method_dispatch_event(dispatch_state *ds, const atlas_ipc_request *req,
                                          atlas_err *err) {
    atlas_status st = require_dispatcher(ds, err);
    if (st != ATLAS_OK) {
        return st;
    }
    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_EVENT);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    op->peer_uid = ds->peer_uid;
    op->actor = ATLAS_ORCH_ACTOR_DISPATCHER;
    st = take_token(req, op, err);
    if (st == ATLAS_OK && !atlas_ipc_param_int(req, "seq", &op->event_seq)) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "an event needs its sequence number");
    }
    const char *kind = NULL, *payload = NULL;
    if (st == ATLAS_OK) {
        if (!atlas_ipc_param_str(req, "kind", &kind) || kind == NULL) {
            kind = "log";
        }
        st = atlas_buf_set_str(&op->event_kind, kind, err);
    }
    if (st == ATLAS_OK && atlas_ipc_param_str(req, "payload", &payload) && payload != NULL) {
        st = atlas_buf_set_str(&op->event_payload, payload, err);
    }
    if (st != ATLAS_OK) {
        atlas_orch_op_free(op);
        free(op);
        return st;
    }
    atlas_orch_result r;
    atlas_orch_result_init(&r);
    st = atlas_writer_orch(ds->ctx->writer, op, 5000, &r, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "cancel_requested", r.cancel_requested, err);
    }
    atlas_orch_result_free(&r);
    return st;
}

/* Decodes lowercase hex into bytes. Refuses anything that is not a pair of hex
 * digits rather than skipping it: a manifest whose content field was half
 * understood is one whose digest will not match, and saying so at the parse is
 * clearer than saying so at the check. */
static atlas_status unhex(const char *in, size_t len, atlas_buf *out, atlas_err *err) {
    atlas_status st = atlas_buf_reserve(out, len / 2u + 1u, err);
    for (size_t i = 0; st == ATLAS_OK && i + 1u < len + 1u && i < len; i += 2u) {
        int hi = -1, lo = -1;
        for (int k = 0; k < 2; k++) {
            char c = in[i + (size_t)k];
            int v = (c >= '0' && c <= '9')   ? c - '0'
                    : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                                             : -1;
            if (k == 0) {
                hi = v;
            } else {
                lo = v;
            }
        }
        if (hi < 0 || lo < 0) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "artifact content is not lowercase hex");
        }
        char byte = (char)((hi << 4) | lo);
        st = atlas_buf_append(out, &byte, 1u, err);
    }
    return st;
}

static atlas_status method_dispatch_complete(dispatch_state *ds, const atlas_ipc_request *req,
                                             atlas_err *err) {
    atlas_status st = require_dispatcher(ds, err);
    if (st != ATLAS_OK) {
        return st;
    }
    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_COMPLETE);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    op->peer_uid = ds->peer_uid;
    op->actor = ATLAS_ORCH_ACTOR_DISPATCHER;
    st = take_token(req, op, err);
    if (st == ATLAS_OK) {
        (void)atlas_ipc_param_bool(req, "success", &op->success);
        (void)atlas_ipc_param_int(req, "exit_code", &op->exit_code);
        (void)atlas_ipc_param_int(req, "pid", &op->claimed_pid);
        const char *kind = NULL;
        op->exit_kind = ATLAS_ORCH_EXIT_UNKNOWN;
        if (atlas_ipc_param_str(req, "exit_kind", &kind) && kind != NULL) {
            /* A closed vocabulary, matched exactly. An unrecognised value stays
             * UNKNOWN rather than being reproduced: a worker must not be able to
             * write its own words into a column a reader treats as Atlas'. */
            static const atlas_orch_exit_kind KINDS[] = {
                ATLAS_ORCH_EXIT_OK,        ATLAS_ORCH_EXIT_NONZERO,
                ATLAS_ORCH_EXIT_SIGNALLED, ATLAS_ORCH_EXIT_TIMEOUT,
                ATLAS_ORCH_EXIT_CANCELLED, ATLAS_ORCH_EXIT_SPAWN_FAILED,
                ATLAS_ORCH_EXIT_MALFORMED_RESULT};
            for (size_t i = 0; i < sizeof KINDS / sizeof KINDS[0]; i++) {
                if (strcmp(kind, atlas_orch_exit_kind_name(KINDS[i])) == 0) {
                    op->exit_kind = KINDS[i];
                    break;
                }
            }
        }
        op->failure_reason = op->success ? ATLAS_ORCH_REASON_WORKER_SUCCESS
                                         : ATLAS_ORCH_REASON_WORKER_FAILURE;
        /* A11.1. A closed vocabulary again, and a deliberately short one: the
         * three failures whose *kind* changes what the run does next. Anything
         * else stays WORKER_FAILURE, which is the conservative reading — it
         * retries within the task's own bound rather than ending the run.
         *
         * A worker cannot reach this. It is set by the run driver from its own
         * gate execution and its own pinned-commit check, and there is no path
         * from a model's output to any of these names. */
        const char *why = NULL;
        if (!op->success && atlas_ipc_param_str(req, "reason", &why) && why != NULL) {
            static const atlas_orch_reason REASONS[] = {ATLAS_ORCH_REASON_VALIDATION_FAILED,
                                                        ATLAS_ORCH_REASON_POLICY_REFUSED,
                                                        ATLAS_ORCH_REASON_WALL_TIMEOUT};
            for (size_t i = 0; i < sizeof REASONS / sizeof REASONS[0]; i++) {
                if (strcmp(why, atlas_orch_reason_name(REASONS[i])) == 0) {
                    op->failure_reason = REASONS[i];
                    break;
                }
            }
        }
        (void)atlas_ipc_param_int(req, "failed_gate", &op->failed_gate);
        const char *detail = NULL;
        if (st == ATLAS_OK && atlas_ipc_param_str(req, "detail", &detail) && detail != NULL) {
            /* UNTRUSTED_DATA, bounded here as well as at the write point. It is
             * quoted into a follow-up task's text and read by nothing. */
            size_t n = strlen(detail);
            st = atlas_buf_set(&op->failure_detail, detail,
                               n < ATLAS_ORCH_GATE_EXCERPT_MAX ? n : ATLAS_ORCH_GATE_EXCERPT_MAX,
                               err);
        }
        const char *dv = NULL;
        if (st == ATLAS_OK && atlas_ipc_param_str(req, "driver_version", &dv) && dv != NULL) {
            st = atlas_buf_set_str(&op->driver_version, dv, err);
        }
    }

    /* The artifact manifest. Each entry is one string:
     * `<name>\x1f<kind>\x1f<sha256>\x1f<size>` — separated by a byte that cannot
     * occur in any of the fields, since a name is a safe relative path, a kind
     * and a digest are names, and a size is decimal.
     *
     * A11.1 adds an optional fifth field, the content itself as lowercase hex.
     * The A8 dispatcher never sends one: its artifacts live in a workspace it
     * owns and Atlas describes them rather than holding them. The run driver has
     * no workspace at all, so an artifact it does not carry inline is an
     * artifact nobody can ever read — and "the worker's result survives a
     * restart" is one of the things this milestone has to be able to show. Hex
     * rather than raw bytes because the field is inside a unit-separated string
     * and an artifact log contains arbitrary ones.
     *
     * Four fields and five are both valid, which is what keeps an A8 dispatcher
     * speaking to an A11 daemon unchanged.
     *
     * There is deliberately no path field. An artifact is addressed by its
     * server-assigned id, never by a location a worker chose. */
    if (st == ATLAS_OK) {
        const atlas_ipc_array *arr = NULL;
        if (atlas_ipc_param_array(req, "artifact", &arr)) {
            size_t n = atlas_ipc_array_len(arr);
            if (n > (size_t)ATLAS_ORCH_MAX_ARTIFACT_COUNT) {
                st = atlas_err_set(err, ATLAS_ERR_USAGE, "too many artifacts");
            }
            if (st == ATLAS_OK && n > 0) {
                op->artifacts = calloc(n, sizeof(*op->artifacts));
                if (op->artifacts == NULL) {
                    st = atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
                }
            }
            for (size_t i = 0; st == ATLAS_OK && i < n; i++) {
                const char *ent = NULL;
                if (!atlas_ipc_array_str(arr, i, &ent) || ent == NULL) {
                    st = atlas_err_set(err, ATLAS_ERR_USAGE, "artifact %zu is not a string", i);
                    break;
                }
                atlas_orch_artifact_init(&op->artifacts[i]);
                op->artifact_count = i + 1u;
                const char *f[5] = {ent, NULL, NULL, NULL, NULL};
                size_t nf = 1;
                for (const char *p = ent; *p != '\0' && nf < 5; p++) {
                    if (*p == '\x1f') {
                        f[nf++] = p + 1;
                    }
                }
                if (nf < 4) {
                    st = atlas_err_set(err, ATLAS_ERR_USAGE,
                                       "artifact %zu is not a manifest entry", i);
                    break;
                }
                size_t lens[5] = {0, 0, 0, 0, 0};
                for (size_t k = 0; k < nf; k++) {
                    const char *sep = strchr(f[k], '\x1f');
                    lens[k] = sep != NULL ? (size_t)(sep - f[k]) : strlen(f[k]);
                }
                st = atlas_buf_set(&op->artifacts[i].name, f[0], lens[0], err);
                if (st == ATLAS_OK) {
                    st = atlas_buf_set(&op->artifacts[i].kind, f[1], lens[1], err);
                }
                if (st == ATLAS_OK) {
                    st = atlas_buf_set(&op->artifacts[i].sha256, f[2], lens[2], err);
                }
                if (st == ATLAS_OK) {
                    char num[32];
                    if (lens[3] == 0 || lens[3] >= sizeof(num)) {
                        st = atlas_err_set(err, ATLAS_ERR_USAGE,
                                           "artifact %zu has no usable size", i);
                    } else {
                        memcpy(num, f[3], lens[3]);
                        num[lens[3]] = '\0';
                        op->artifacts[i].size_bytes = strtoll(num, NULL, 10);
                    }
                }
                if (st == ATLAS_OK && nf == 5 && lens[4] > 0) {
                    if (lens[4] % 2u != 0u || lens[4] / 2u > ATLAS_ORCH_ARTIFACT_INLINE_MAX) {
                        st = atlas_err_set(err, ATLAS_ERR_USAGE,
                                           "artifact %zu carries content Atlas will not store "
                                           "inline",
                                           i);
                    } else {
                        st = unhex(f[4], lens[4], &op->artifacts[i].content, err);
                        if (st == ATLAS_OK) {
                            /* The declared size must be the size of what
                             * arrived. A manifest that describes one thing and
                             * carries another is a record that cannot be
                             * checked afterwards, and the digest is verified
                             * against the bytes, not against the claim. */
                            char hex[ATLAS_SHA256_HEX_LEN + 1u];
                            atlas_sha256_hex(op->artifacts[i].content.data,
                                             op->artifacts[i].content.len, hex);
                            if ((int64_t)op->artifacts[i].content.len !=
                                    op->artifacts[i].size_bytes ||
                                strcmp(hex, atlas_buf_cstr(&op->artifacts[i].sha256)) != 0) {
                                st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                                   "artifact %zu does not match what it declares",
                                                   i);
                            } else {
                                op->artifacts[i].content_stored = true;
                            }
                        }
                    }
                }
            }
        }
    }
    if (st != ATLAS_OK) {
        atlas_orch_op_free(op);
        free(op);
        return st;
    }

    atlas_orch_result r;
    atlas_orch_result_init(&r);
    st = atlas_writer_orch(ds->ctx->writer, op, 10000, &r, err);
    if (st == ATLAS_OK) {
        st = write_job_summary(ds, &r, err);
    }
    /* A11.1. What the completion did to the run. Reported so the run driver can
     * print it and stop; it decides nothing from these — the decision has
     * already been taken, inside the transaction, by the daemon. */
    if (st == ATLAS_OK && r.run_status != ATLAS_ORCH_RUN_UNKNOWN) {
        st = atlas_json_key_str(ds->j, "run_status", atlas_orch_run_status_name(r.run_status),
                                err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "worker_starts", r.worker_starts, err);
        }
        if (st == ATLAS_OK && r.follow_up_job_uid.len > 0) {
            st = atlas_json_key_str(ds->j, "follow_up", atlas_buf_cstr(&r.follow_up_job_uid),
                                    err);
        }
    }
    atlas_orch_result_free(&r);
    return st;
}

/* --- dispatch.snapshot.open and dispatch.snapshot.chunk ----------------------
 *
 * The daemon reads the repository; the worker receives bytes. Neither method
 * accepts a repository, a commit or a path — the attempt names the job, the job
 * names both, and the registry names the canonical location. There is no field
 * here that could carry a host path, which is why "the worker cannot supply or
 * replace the repository" is a property of the signature rather than a check.
 *
 * Both are bound to an attempt by its lease token, so an expired, released or
 * stale lease can retrieve nothing. */
static atlas_status method_snapshot_open(dispatch_state *ds, const atlas_ipc_request *req,
                                         atlas_err *err) {
    atlas_status st = require_dispatcher(ds, err);
    if (st != ATLAS_OK) {
        return st;
    }
    const char *tok = NULL;
    if (!atlas_ipc_param_str(req, "token", &tok) || tok == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a snapshot request needs its lease token");
    }
    int64_t attempt_id = 0;
    st = atlas_db_orch_attempt_for_token(ds->db, tok, &attempt_id, err);
    if (st != ATLAS_OK) {
        return st;
    }
    /* Enumeration writes the manifest, so it goes to the writer thread — the
     * one writable handle. A read-only dispatch handle must never write. */
    atlas_snapshot_meta meta;
    memset(&meta, 0, sizeof(meta));
    st = atlas_writer_snapshot(ds->ctx->writer, attempt_id, 120000, &meta, err);
    if (st != ATLAS_OK) {
        return st;
    }
    struct {
        const char *k;
        const char *v;
    } strs[] = {{"commit", meta.commit}, {"tree", meta.tree}, {"digest", meta.digest}};
    for (size_t i = 0; st == ATLAS_OK && i < sizeof strs / sizeof strs[0]; i++) {
        st = atlas_json_key_str(ds->j, strs[i].k, strs[i].v, err);
    }
    struct {
        const char *k;
        int64_t v;
    } ints[] = {
        {"protocol", meta.protocol},
        {"entries", meta.entries},
        {"total_bytes", meta.total_bytes},
        {"chunk_bytes", (int64_t)ATLAS_SNAPSHOT_CHUNK_BYTES},
        {"refused_symlinks", meta.refused_symlinks},
        {"refused_gitlinks", meta.refused_gitlinks},
        {"refused_other", meta.refused_other},
        {"refused_sizes", meta.refused_sizes},
    };
    for (size_t i = 0; st == ATLAS_OK && i < sizeof ints / sizeof ints[0]; i++) {
        st = atlas_json_key_int(ds->j, ints[i].k, ints[i].v, err);
    }
    return st;
}

/* Lowercase hex. A JSON string cannot carry arbitrary bytes, and hex is the
 * encoding whose decoder cannot be argued with: no escapes, no ambiguity, a
 * fixed 2:1 expansion, and a malformed body fails at the first bad nibble
 * rather than arriving silently shorter. */
static atlas_status write_hex(atlas_json *j, const char *key, const atlas_buf *raw,
                              atlas_err *err) {
    atlas_buf hex = ATLAS_BUF_INIT;
    atlas_status st = atlas_buf_reserve(&hex, raw->len * 2u + 1u, err);
    static const char D[] = "0123456789abcdef";
    for (size_t i = 0; st == ATLAS_OK && i < raw->len; i++) {
        unsigned char c = (unsigned char)raw->data[i];
        char pair[2] = {D[c >> 4], D[c & 0x0fu]};
        st = atlas_buf_append(&hex, pair, 2u, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, key, atlas_buf_cstr(&hex), err);
    }
    atlas_buf_free(&hex);
    return st;
}

static atlas_status method_snapshot_chunk(dispatch_state *ds, const atlas_ipc_request *req,
                                          atlas_err *err) {
    atlas_status st = require_dispatcher(ds, err);
    if (st != ATLAS_OK) {
        return st;
    }
    const char *tok = NULL;
    if (!atlas_ipc_param_str(req, "token", &tok) || tok == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a snapshot request needs its lease token");
    }
    int64_t index = -1, offset = 0;
    if (!atlas_ipc_param_int(req, "index", &index)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a chunk request needs an entry index");
    }
    (void)atlas_ipc_param_int(req, "offset", &offset);

    int64_t attempt_id = 0;
    st = atlas_db_orch_attempt_for_token(ds->db, tok, &attempt_id, err);
    if (st != ATLAS_OK) {
        return st;
    }
    /* A pure read: the manifest is already persisted, and the bytes come from
     * the repository. No writer involvement, so chunks do not serialise behind
     * the write queue. */
    atlas_snapshot_chunk chunk;
    st = atlas_snapshot_read(ds->db, attempt_id, index, offset, &chunk, err);
    if (st != ATLAS_OK) {
        atlas_snapshot_chunk_free(&chunk);
        return st;
    }
    /* The path is repository bytes and is safe-encoded on the way out; the
     * worker decodes it back before materialising, and checks it again. */
    st = atlas_json_key_str(ds->j, "path", atlas_safe(&ds->safe, atlas_buf_cstr(&chunk.path)),
                            err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "path_encoding", "atlas-safe-1", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "mode", chunk.mode, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "sha256", chunk.sha256, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "size", chunk.size_bytes, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "offset", chunk.offset, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "bytes", (int64_t)chunk.data.len, err);
    }
    if (st == ATLAS_OK) {
        st = write_hex(ds->j, "data", &chunk.data, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "eof", chunk.eof, err);
    }
    atlas_snapshot_chunk_free(&chunk);
    return st;
}

/* --- the two tables ---------------------------------------------------------
 *
 * Kept apart on purpose. One table with a per-entry flag would put the
 * separation inside a struct field that a later edit could set wrongly on one
 * row; two tables make "which group is this in?" a question about which array a
 * name appears in, which is visible in a diff. */
static const atlas_method_entry ORCH_CLIENT_METHODS[] = {
    {"job.submit", method_job_submit},
    {"job.get", method_job_get},
    {"job.list", method_job_list},
    {"job.cancel", method_job_cancel},
    {"job.artifact", method_job_artifact},
    {"job.run_status", method_run_get},
};

static const atlas_method_entry ORCH_DISPATCH_METHODS[] = {
    {"dispatch.lease", method_dispatch_lease},
    {"dispatch.heartbeat", method_dispatch_heartbeat},
    {"dispatch.event", method_dispatch_event},
    {"dispatch.complete", method_dispatch_complete},
    {"dispatch.snapshot.open", method_snapshot_open},
    {"dispatch.snapshot.chunk", method_snapshot_chunk},
};

const atlas_method_entry *atlas_server_orch_client_methods(size_t *count_out) {
    if (count_out != NULL) {
        *count_out = sizeof ORCH_CLIENT_METHODS / sizeof ORCH_CLIENT_METHODS[0];
    }
    return ORCH_CLIENT_METHODS;
}

const atlas_method_entry *atlas_server_orch_dispatch_methods(size_t *count_out) {
    if (count_out != NULL) {
        *count_out = sizeof ORCH_DISPATCH_METHODS / sizeof ORCH_DISPATCH_METHODS[0];
    }
    return ORCH_DISPATCH_METHODS;
}
