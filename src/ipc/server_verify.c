/* Atlas - A9.2.1: the verification method group.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A9.2 exposed the verification engine over no socket method at all. The
 * consequence was not the one intended: under A7.1 the index is 0700 `atlasd`,
 * so from the operator's account there was no way to reach a claim — and
 * `served_over_socket()` in `src/cli/cli.c` already *claimed* `verify show` was
 * served, so the CLI skipped its own refusal and called the local path with a
 * null context. `atlas verify show` segfaulted on every system deployment.
 * This group is what that name was always supposed to resolve to.
 *
 * ## Which group a method goes in is the security decision
 *
 * All of these are in the **ordinary** table, reachable by any peer that may
 * open the socket, and that placement is deliberate rather than convenient:
 *
 *   - The four intake verbs create a claim, reference evidence, attest and
 *     declare a derivation. None of them is authority. A model doing all four
 *     has added rows that the aggregation will weigh, and the weighing is
 *     governed by a root-owned policy this peer cannot read, let alone edit.
 *
 *   - `verify.evaluate` may cause Atlas to transition a lifecycle state. It is
 *     still ordinary, because §17 says exactly that: a model may *request an
 *     evaluation*, and if the policy's gates are met **Atlas** performs the
 *     transition and records the audit. The caller never holds the capability.
 *     What protects the record is that a deterministic verdict requires an
 *     Atlas-attested verifier a model cannot forge, the empirical path is
 *     shadow-only without calibration nothing here can supply, and every gate
 *     is set by a file in `/etc` owned by root.
 *
 *   - `verify.show`, `verify.claims` and `verify.policy` are reads.
 *
 * What is **absent**, and absent rather than refused so a later edit cannot
 * weaken it: there is no method that approves, rejects, supersedes, resolves or
 * revalidates; none that mints or spends a warrant; none that edits, reloads or
 * overrides the verification policy; and none that lets a caller state a
 * verifier's verdict. `tests/test_verify_rpc.c` asks a live daemon for every
 * name such a method would plausibly have and requires `unknown method`.
 */
#include <stdlib.h>
#include <string.h>

#include "atlas/service.h"
#include "atlas/verify_ops.h"
#include "server_internal.h"

#define VERIFY_WRITE_TIMEOUT_MS 15000

/* --- the channel -----------------------------------------------------------
 *
 * **The one thing in this file that must never come from the request body.**
 *
 * A peer whose uid the root-owned policy names as the operator speaks on the
 * OPERATOR channel; every other peer speaks on MODEL. There is no parameter
 * that selects it, `atlas_verify_channel_parse` refuses the ATLAS name, and the
 * kernel is what answers the question — A7.1's rule that a client describing
 * itself is not evidence about itself, which is the whole reason an attestation
 * from a model cannot become tool evidence.
 *
 * PEER_AUTHENTICATED, which the operator channel produces, is a claim about the
 * *uid* and not about a person. A7.1's honesty limits hold word for word: a
 * model with a shell as that account reaches this channel exactly as a human
 * does. What the distinction asserts is only that the caller was not anonymous. */
static atlas_verify_channel channel_for(dispatch_state *ds) {
    return atlas_server_peer_is_operator((long long)ds->peer_uid) ? ATLAS_VERIFY_CHANNEL_OPERATOR
                                                                  : ATLAS_VERIFY_CHANNEL_MODEL;
}

static atlas_verify_op *op_new(atlas_verify_op_kind kind, dispatch_state *ds) {
    atlas_verify_op *op = calloc(1u, sizeof(*op));
    if (op != NULL) {
        atlas_verify_op_init(op);
        op->kind = kind;
        op->channel = channel_for(ds);
    }
    return op;
}

/* Copies one optional string parameter into an owned buffer. A parameter that
 * is absent stays empty; a parameter that is present but not a string is a
 * malformed request and is refused rather than ignored, because a client that
 * sent the wrong type meant something by it. */
static atlas_status take(const atlas_ipc_request *req, const char *key, atlas_buf *out,
                         atlas_err *err) {
    const char *s = NULL;
    if (!atlas_ipc_param_str(req, key, &s) || s == NULL) {
        return ATLAS_OK;
    }
    return atlas_buf_set_str(out, s, err);
}

/* The speaker's own description of itself, §11. Every field here is asserted
 * and is stored as asserted: the transport carries no cryptographic statement
 * about which model is speaking, and the row's `identity` column is what says
 * so. Copied wholesale because none of it decides anything — it is metadata
 * that makes "which actor was this?" answerable, not an input to a verdict. */
static atlas_status take_actor(const atlas_ipc_request *req, atlas_verify_op *op, atlas_err *err) {
    static const struct {
        const char *key;
        size_t off;
    } FIELDS[] = {
        {"actor", offsetof(atlas_verify_op, actor_name)},
        {"provider", offsetof(atlas_verify_op, actor_provider)},
        {"family", offsetof(atlas_verify_op, actor_family)},
        {"model_version", offsetof(atlas_verify_op, actor_version)},
        {"role", offsetof(atlas_verify_op, actor_role)},
        {"session", offsetof(atlas_verify_op, session_key)},
        {"run", offsetof(atlas_verify_op, run_id)},
        {"orchestrator", offsetof(atlas_verify_op, parent_actor_uid)},
    };
    for (size_t i = 0; i < sizeof FIELDS / sizeof FIELDS[0]; i++) {
        atlas_buf *b = (atlas_buf *)((char *)op + FIELDS[i].off);
        atlas_status st = take(req, FIELDS[i].key, b, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    return ATLAS_OK;
}

static atlas_status submit(dispatch_state *ds, atlas_verify_op *op,
                           atlas_verify_intake_result *result, atlas_err *err) {
    if (ds->ctx->writer == NULL) {
        atlas_verify_op_free(op);
        free(op);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "this Atlas daemon has no writer, so nothing can be recorded");
    }
    /* Takes ownership unconditionally, including on its own failure paths. */
    return atlas_writer_verify(ds->ctx->writer, op, VERIFY_WRITE_TIMEOUT_MS, result, err);
}

/* The common members of an intake result. `uid` and `actor` are Atlas-minted
 * identifiers from a fixed alphabet, so they carry no repository or model byte
 * and need no encoding; anything that did would be encoded here. */
static atlas_status write_result(dispatch_state *ds, const atlas_verify_intake_result *r,
                                 atlas_err *err) {
    atlas_status st = atlas_json_key_str(ds->j, "uid", atlas_buf_cstr(&r->uid), err);
    if (st == ATLAS_OK && r->actor_uid.len > 0) {
        st = atlas_json_key_str(ds->j, "actor", atlas_buf_cstr(&r->actor_uid), err);
    }
    if (st == ATLAS_OK && r->claim_id != 0) {
        st = atlas_json_key_int(ds->j, "claim_id", r->claim_id, err);
    }
    if (st == ATLAS_OK && r->evidence_id != 0) {
        st = atlas_json_key_int(ds->j, "evidence_id", r->evidence_id, err);
    }
    if (st == ATLAS_OK && r->attestation_id != 0) {
        st = atlas_json_key_int(ds->j, "attestation_id", r->attestation_id, err);
    }
    if (st == ATLAS_OK) {
        /* §27/§28. Reported rather than silent, so a client can tell that its
         * retry resolved to the row it already created — and so that a *reader*
         * of an audit trail can see that a repeated submission did not become a
         * second corroboration. */
        st = atlas_json_key_bool(ds->j, "duplicate", r->duplicate, err);
    }
    return st;
}

/* --- intake ----------------------------------------------------------------- */

static atlas_status method_claim_create(dispatch_state *ds, const atlas_ipc_request *req,
                                        atlas_err *err) {
    atlas_verify_op *op = op_new(ATLAS_VERIFY_OP_CLAIM_CREATE, ds);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    static const struct {
        const char *key;
        size_t off;
    } FIELDS[] = {
        {"repo", offsetof(atlas_verify_op, repo_name)},
        {"decision", offsetof(atlas_verify_op, document_uid)},
        {"domain", offsetof(atlas_verify_op, domain)},
        {"text", offsetof(atlas_verify_op, text)},
        {"scope", offsetof(atlas_verify_op, scope_note)},
        {"verifier", offsetof(atlas_verify_op, verifier)},
        {"verifier_input", offsetof(atlas_verify_op, verifier_input)},
        {"commit", offsetof(atlas_verify_op, basis_commit)},
        {"environment", offsetof(atlas_verify_op, environment)},
    };
    atlas_status st = ATLAS_OK;
    for (size_t i = 0; st == ATLAS_OK && i < sizeof FIELDS / sizeof FIELDS[0]; i++) {
        st = take(req, FIELDS[i].key, (atlas_buf *)((char *)op + FIELDS[i].off), err);
    }
    if (st == ATLAS_OK) {
        st = take_actor(req, op, err);
    }
    /* §6. DESCRIPTIVE unless the caller says otherwise, and a value that is not
     * in the vocabulary is refused rather than defaulted — the difference
     * between "observes what is" and "declares what ought to be" is the one
     * `atlas_verify_basis_may_verify_semantics` exists to police, and silently
     * choosing for a confused caller would decide it for them. */
    const char *sem = NULL;
    if (st == ATLAS_OK && atlas_ipc_param_str(req, "semantics", &sem) && sem != NULL) {
        if (!atlas_verify_claim_semantics_parse(sem, &op->semantics)) {
            st = atlas_err_set(err, ATLAS_ERR_USAGE,
                               "a claim is DESCRIPTIVE or NORMATIVE; it says whether it observes "
                               "what is or declares what ought to be");
        } else {
            op->semantics_given = true;
        }
    }
    if (st != ATLAS_OK) {
        atlas_verify_op_free(op);
        free(op);
        return st;
    }
    atlas_verify_intake_result res;
    atlas_verify_intake_result_init(&res);
    st = submit(ds, op, &res, err);
    if (st == ATLAS_OK) {
        st = write_result(ds, &res, err);
    }
    atlas_verify_intake_result_free(&res);
    return st;
}

static atlas_status method_evidence_add(dispatch_state *ds, const atlas_ipc_request *req,
                                        atlas_err *err) {
    atlas_verify_op *op = op_new(ATLAS_VERIFY_OP_EVIDENCE_ADD, ds);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    static const struct {
        const char *key;
        size_t off;
    } FIELDS[] = {
        {"claim", offsetof(atlas_verify_op, claim_uid)},
        {"commit", offsetof(atlas_verify_op, commit_oid)},
        {"path", offsetof(atlas_verify_op, path_text)},
        {"symbol", offsetof(atlas_verify_op, symbol)},
        {"target", offsetof(atlas_verify_op, target)},
        {"probe", offsetof(atlas_verify_op, probe)},
        {"observed", offsetof(atlas_verify_op, observed)},
        {"observed_at", offsetof(atlas_verify_op, observed_at)},
    };
    atlas_status st = ATLAS_OK;
    for (size_t i = 0; st == ATLAS_OK && i < sizeof FIELDS / sizeof FIELDS[0]; i++) {
        st = take(req, FIELDS[i].key, (atlas_buf *)((char *)op + FIELDS[i].off), err);
    }
    if (st == ATLAS_OK) {
        st = take_actor(req, op, err);
    }
    const char *cls = NULL;
    if (st == ATLAS_OK) {
        if (!atlas_ipc_param_str(req, "class", &cls) || cls == NULL ||
            !atlas_verify_evidence_class_parse(cls, &op->evidence_class)) {
            st = atlas_err_set(err, ATLAS_ERR_USAGE,
                               "evidence must say what sort of thing it is; there is no "
                               "unclassified evidence");
        }
    }
    if (st == ATLAS_OK) {
        (void)atlas_ipc_param_int(req, "line_start", &op->line_start);
        (void)atlas_ipc_param_int(req, "line_end", &op->line_end);
    }
    if (st != ATLAS_OK) {
        atlas_verify_op_free(op);
        free(op);
        return st;
    }
    atlas_verify_intake_result res;
    atlas_verify_intake_result_init(&res);
    st = submit(ds, op, &res, err);
    if (st == ATLAS_OK) {
        st = write_result(ds, &res, err);
    }
    atlas_verify_intake_result_free(&res);
    return st;
}

/* §9. Atlas runs the verifier and records what it found.
 *
 * There is no parameter for the verdict and there must never be one: this is
 * the operation whose entire value is that the conclusion came from Atlas
 * having looked. A caller chooses which allowlisted verifier applies; what it
 * concluded is whatever it concluded. */
static atlas_status method_evidence_produce(dispatch_state *ds, const atlas_ipc_request *req,
                                            atlas_err *err) {
    atlas_verify_op *op = op_new(ATLAS_VERIFY_OP_EVIDENCE_PRODUCE, ds);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    atlas_status st = take(req, "claim", &op->claim_uid, err);
    if (st == ATLAS_OK) {
        st = take(req, "verifier", &op->verifier, err);
    }
    if (st != ATLAS_OK) {
        atlas_verify_op_free(op);
        free(op);
        return st;
    }
    atlas_verify_intake_result res;
    atlas_verify_intake_result_init(&res);
    st = submit(ds, op, &res, err);
    if (st == ATLAS_OK) {
        st = write_result(ds, &res, err);
    }
    if (st == ATLAS_OK) {
        /* Atlas' own closed vocabulary and Atlas' own sentence. Neither carries
         * a repository byte, which is why they may be reported unencoded. */
        st = atlas_json_key_str(ds->j, "check", atlas_verify_check_name(res.check), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "verified_scope", res.verified_scope, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "detail", res.detail, err);
    }
    atlas_verify_intake_result_free(&res);
    return st;
}

static atlas_status method_attestation_add(dispatch_state *ds, const atlas_ipc_request *req,
                                           atlas_err *err) {
    atlas_verify_op *op = op_new(ATLAS_VERIFY_OP_ATTESTATION_ADD, ds);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    static const struct {
        const char *key;
        size_t off;
    } FIELDS[] = {
        {"claim", offsetof(atlas_verify_op, claim_uid)},
        {"method", offsetof(atlas_verify_op, method)},
        {"scope", offsetof(atlas_verify_op, scope_note)},
        {"evidence", offsetof(atlas_verify_op, evidence_uids)},
        {"supersedes", offsetof(atlas_verify_op, supersedes_uid)},
        {"commit", offsetof(atlas_verify_op, basis_commit)},
    };
    atlas_status st = ATLAS_OK;
    for (size_t i = 0; st == ATLAS_OK && i < sizeof FIELDS / sizeof FIELDS[0]; i++) {
        st = take(req, FIELDS[i].key, (atlas_buf *)((char *)op + FIELDS[i].off), err);
    }
    if (st == ATLAS_OK) {
        st = take_actor(req, op, err);
    }
    const char *v = NULL;
    if (st == ATLAS_OK) {
        if (!atlas_ipc_param_str(req, "verdict", &v) || v == NULL ||
            !atlas_verify_verdict_parse(v, &op->verdict)) {
            st = atlas_err_set(err, ATLAS_ERR_USAGE,
                               "an attestation is SUPPORT, CONTRADICT or INCONCLUSIVE");
        }
    }
    if (st == ATLAS_OK) {
        int64_t conf = -1;
        if (atlas_ipc_param_int(req, "self_confidence", &conf)) {
            if (conf < 0 || conf > 100) {
                st = atlas_err_set(err, ATLAS_ERR_USAGE,
                                   "self-reported confidence is 0..100, or absent; it is data "
                                   "about the source and is never used as Atlas' confidence");
            } else {
                op->self_confidence = (int)conf;
            }
        }
    }
    if (st != ATLAS_OK) {
        atlas_verify_op_free(op);
        free(op);
        return st;
    }
    atlas_verify_intake_result res;
    atlas_verify_intake_result_init(&res);
    st = submit(ds, op, &res, err);
    if (st == ATLAS_OK) {
        st = write_result(ds, &res, err);
    }
    atlas_verify_intake_result_free(&res);
    return st;
}

static atlas_status method_dependency_add(dispatch_state *ds, const atlas_ipc_request *req,
                                          atlas_err *err) {
    atlas_verify_op *op = op_new(ATLAS_VERIFY_OP_DEPENDENCY_ADD, ds);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    atlas_status st = take(req, "evidence", &op->derived_uid, err);
    if (st == ATLAS_OK) {
        st = take(req, "derives_from", &op->source_uid, err);
    }
    if (st == ATLAS_OK && (op->derived_uid.len == 0 || op->source_uid.len == 0)) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE,
                           "a derivation names the evidence that derives and the evidence it "
                           "derives from");
    }
    if (st != ATLAS_OK) {
        atlas_verify_op_free(op);
        free(op);
        return st;
    }
    atlas_verify_intake_result res;
    atlas_verify_intake_result_init(&res);
    st = submit(ds, op, &res, err);
    if (st == ATLAS_OK) {
        st = write_result(ds, &res, err);
    }
    atlas_verify_intake_result_free(&res);
    return st;
}

/* --- evaluation and reads ---------------------------------------------------- */

static atlas_status method_evaluate(dispatch_state *ds, const atlas_ipc_request *req,
                                    atlas_err *err) {
    atlas_verify_op *op = op_new(ATLAS_VERIFY_OP_EVALUATE, ds);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    atlas_status st = take(req, "claim", &op->claim_uid, err);
    if (st == ATLAS_OK) {
        st = take(req, "repo", &op->repo_name, err);
    }
    if (st != ATLAS_OK) {
        atlas_verify_op_free(op);
        free(op);
        return st;
    }
    atlas_verify_intake_result res;
    atlas_verify_intake_result_init(&res);
    st = submit(ds, op, &res, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "claim", atlas_buf_cstr(&res.uid), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_service_verify_write_assessment(ds->j, &res.assessment, err);
    }
    atlas_verify_intake_result_free(&res);
    return st;
}

static atlas_status method_show(dispatch_state *ds, const atlas_ipc_request *req, atlas_err *err) {
    int64_t claim_id = 0;
    const char *uid = NULL;
    (void)atlas_ipc_param_int(req, "claim_id", &claim_id);
    (void)atlas_ipc_param_str(req, "claim", &uid);

    atlas_verify_report rep;
    atlas_verify_report_init(&rep);
    /* A read: `atlas_verify_assess` writes nothing, which is what makes asking
     * what Atlas thinks unable to change what Atlas thinks. A6's property, for
     * A6's reason. The read-only handle this request holds could not write in
     * any case, which is the structural half of the same guarantee. */
    atlas_status st = atlas_service_verify_show_on(ds->db, claim_id, uid, &rep, err);
    if (st == ATLAS_OK) {
        st = atlas_service_verify_write_report(ds->j, &ds->safe, &rep, err);
    }
    atlas_verify_report_free(&rep);
    return st;
}

static atlas_status method_claims(dispatch_state *ds, const atlas_ipc_request *req,
                                  atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_server_require_repo(ds, req, &info, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }
    const char *decision = NULL;
    (void)atlas_ipc_param_str(req, "decision", &decision);
    int64_t limit = 0;
    (void)atlas_ipc_param_int(req, "limit", &limit);
    st = atlas_service_verify_claims_on(ds->db, ds->j, &ds->safe, info.id, decision, limit, err);
    atlas_repo_info_free(&info);
    return st;
}

/* Reads the root-owned policy. It opens no index and binds nothing, so it
 * answers on a machine where Atlas has never run — the shape `gateway status`
 * has, and exactly when somebody asks it. */
static atlas_status method_policy(dispatch_state *ds, const atlas_ipc_request *req,
                                  atlas_err *err) {
    (void)req;
    atlas_verify_report rep;
    atlas_verify_report_init(&rep);
    atlas_status st = atlas_service_verify_policy(&rep, err);
    if (st == ATLAS_OK) {
        st = atlas_service_verify_write_policy(ds->j, &rep, err);
    }
    atlas_verify_report_free(&rep);
    return st;
}

static const atlas_method_entry VERIFY_METHODS[] = {
    {"verify.claim_create", method_claim_create},
    {"verify.evidence_add", method_evidence_add},
    {"verify.evidence_produce", method_evidence_produce},
    {"verify.attestation_add", method_attestation_add},
    {"verify.dependency_add", method_dependency_add},
    {"verify.evaluate", method_evaluate},
    {"verify.show", method_show},
    {"verify.claims", method_claims},
    {"verify.policy", method_policy},
};

const atlas_method_entry *atlas_server_verify_methods(size_t *count_out) {
    if (count_out != NULL) {
        *count_out = sizeof(VERIFY_METHODS) / sizeof(VERIFY_METHODS[0]);
    }
    return VERIFY_METHODS;
}
