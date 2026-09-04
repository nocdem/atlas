/* Atlas - the operator-only credential methods.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Why these exist
 * ---------------
 * A9 says credential administration is a **local operator** function, and it
 * still is: these are in the operator group, offered only to the peer whose
 * `SO_PEERCRED` uid the root-owned authority policy names — the same gate as
 * `decision.approve` and `backup.create`, and the same silence for everybody
 * else. The gateway's uid is not that uid, so a remote client holding every
 * scope Atlas can grant still gets `unknown method` for all three names.
 *
 * They exist because the local path alone was not enough, in two ways that only
 * appear on a machine where Atlas is actually running.
 *
 *   **Revocation must not require stopping the daemon.** `atlas api-key revoke`
 *   takes the data-directory writer lock, and a running daemon holds it. So on
 *   every machine with a live daemon the command failed — and "stop the service
 *   to revoke a leaked credential" is not an answer to a leaked credential.
 *
 *   **Under A7.1 the operator cannot open the index at all.** It is 0700
 *   `atlasd`, so the account a root-owned policy names as the operator was the
 *   one account that could not create its own credentials. That is the same
 *   defect A7.1 created for backup, and it is fixed the same way rather than
 *   with a documented instruction to become the service account.
 *
 * What they are not
 * -----------------
 * There is no method here that returns a secret for an existing credential,
 * because no such operation exists anywhere in Atlas: the index holds a
 * one-way verifier and there is no column a plaintext could be read from.
 * `apikey.create` returns a plaintext exactly once, for the credential it just
 * minted, which is the only moment one exists.
 */
#include <string.h>
#include <unistd.h>

#include "atlas/apikey.h"
#include "atlas/gw.h"
#include "atlas/safetext.h"
#include "server_internal.h"

/* Who may administer credentials.
 *
 * Three cases, and the middle one is the reason this is not simply
 * `atlas_server_peer_is_operator`.
 *
 *   **A root-owned authority policy names an operator.** That uid, and no
 *   other. This is the separated A7.1 deployment and the only case in which
 *   this gate is a real boundary.
 *
 *   **A system deployment with no authority policy.** Refused. A machine that
 *   has been separated and has not said who its operator is has not authorised
 *   anybody, and guessing would be inventing the answer.
 *
 *   **Legacy per-user mode.** The daemon's own uid, and this needs its argument
 *   written down. On an unseparated machine that account owns `atlas.db`
 *   outright: it can insert, update or delete any row in `api_keys` with
 *   `sqlite3` and no Atlas code involved. Refusing it here would relocate the
 *   verb and protect nothing — A7's rule that a check an adversary simply walks
 *   around reads as protection in a review and provides none. What refusing
 *   *would* accomplish is making `atlas api-key revoke` fail on every ordinary
 *   machine with a running daemon, because the local path cannot take a writer
 *   lock the daemon holds.
 *
 * Note what this is not: A7 guards the decision lifecycle because that mints a
 * coherent record — consumed challenge, ledger event, `LOCAL_OPERATOR_CONFIRMED`
 * — which nothing downstream can distinguish from a human's. A credential is an
 * access grant, not a record about a person, and in legacy mode the account
 * asking already has the access. Nothing here mints a lifecycle capability. */
static bool peer_may_administer(dispatch_state *ds) {
    if (atlas_server_peer_is_operator((long long)ds->peer_uid)) {
        return true;
    }
    if (ds->ctx->syspolicy.state == ATLAS_SYSPOLICY_SYSTEM) {
        return false;
    }
    return (long long)ds->peer_uid == (long long)getuid();
}

bool atlas_server_peer_may_administer_credentials(dispatch_state *ds) {
    return peer_may_administer(ds);
}

/* Renders one credential's metadata. There is no field here that could hold a
 * secret — `atlas_apikey_record` has none — so there is nothing to omit and
 * nothing a later edit could accidentally include. */
static atlas_status write_key(dispatch_state *ds, const atlas_apikey_record *k, atlas_err *err) {
    atlas_json *j = ds->j;
    atlas_status st = atlas_json_obj_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "id", k->key_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "label", atlas_safe(&ds->safe, k->label), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "scopes", k->scopes, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(j, "scopes_unreadable", k->scopes_unreadable, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "status", atlas_apikey_status_name(k->status), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "created_at", k->created_at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "revoked_at", k->revoked_at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "last_used_at", k->last_used_at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "rotated_from", k->rotated_from, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "rotated_to", k->rotated_to, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(j, err);
    }
    return st;
}

static atlas_status list_row(const atlas_apikey_record *k, void *ud, atlas_err *err) {
    return write_key((dispatch_state *)ud, k, err);
}

static atlas_status method_apikey_list(dispatch_state *ds, const atlas_ipc_request *req,
                                       atlas_err *err) {
    (void)req;
    if (!peer_may_administer(ds)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "unknown method \"apikey.list\"");
    }
    atlas_status st = atlas_json_key(ds->j, "api_keys", err);
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    int64_t count = 0;
    if (st == ATLAS_OK) {
        st = atlas_db_apikey_list(ds->db, list_row, ds, &count, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "count", count, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "secrets_included", false, err);
    }
    return st;
}

static atlas_status method_apikey_create(dispatch_state *ds, const atlas_ipc_request *req,
                                         atlas_err *err) {
    if (!peer_may_administer(ds)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "unknown method \"apikey.create\"");
    }
    atlas_apikey_job op;
    memset(&op, 0, sizeof op);
    op.kind = ATLAS_APIKEY_JOB_CREATE;

    const char *label = NULL;
    if (!atlas_ipc_param_str(req, "label", &label) || label == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "\"label\" is required");
    }
    if (!atlas_apikey_label_valid(label)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a label must be 1 to %u printable characters, without a quote, "
                             "backslash or percent",
                             (unsigned)ATLAS_APIKEY_LABEL_MAX);
    }
    (void)snprintf(op.label, sizeof op.label, "%s", label);

    const char *scopes = NULL;
    if (!atlas_ipc_param_str(req, "scopes", &scopes) || scopes == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "\"scopes\" is required");
    }
    /* Parsed here, and validated again at the write point. The edge produces the
     * better message; the write point is the guarantee. */
    atlas_status pst = atlas_apikey_scopes_parse(scopes, &op.scopes, err);
    if (pst != ATLAS_OK) {
        return pst;
    }
    /* A16: the deliberate `--no-scopes` form, forwarded as its own parameter
     * rather than inferred from an empty `scopes` string — inferring it here
     * would let any caller who merely sent no scopes get the relaxation
     * Decision 2 reserves for an operator who typed the flag. Absent means
     * false, exactly as the local CLI's zero-initialised opts do. */
    bool no_scopes = false;
    (void)atlas_ipc_param_bool(req, "no_scopes", &no_scopes);
    op.no_scopes = no_scopes;

    const char *rotate = NULL;
    if (atlas_ipc_param_str(req, "rotate_from", &rotate) && rotate != NULL && rotate[0] != '\0') {
        if (!atlas_apikey_id_normalise(rotate, op.rotate_from)) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "\"rotate_from\" is not a key id");
        }
    }

    atlas_apikey_job_result res;
    memset(&res, 0, sizeof res);
    atlas_status st = atlas_writer_apikey(ds->ctx->writer, &op, &res, err);
    if (st != ATLAS_OK) {
        atlas_apikey_created_free(&res.created);
        return st;
    }

    /* The plaintext crosses the socket exactly once, to the operator's own uid,
     * and is wiped from this process before the response is sent. It is not
     * logged here, not audited, and not stored: the row that was just written
     * holds a one-way verifier and nothing else. */
    st = atlas_json_key(ds->j, "api_key", err);
    if (st == ATLAS_OK) {
        st = atlas_json_obj_begin(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "id", res.created.key_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "label", atlas_safe(&ds->safe, res.created.label), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "scopes", res.created.scopes, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "created_at", res.created.created_at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "rotated_from", res.created.rotated_from, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "previous_revoked", res.created.previous_revoked, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "secret", res.created.token, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "shown_once", true, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "recoverable", false, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(ds->j, err);
    }
    atlas_apikey_created_free(&res.created);
    return st;
}

static atlas_status method_apikey_revoke(dispatch_state *ds, const atlas_ipc_request *req,
                                         atlas_err *err) {
    if (!peer_may_administer(ds)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "unknown method \"apikey.revoke\"");
    }
    const char *id = NULL;
    if (!atlas_ipc_param_str(req, "key_id", &id) || id == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "\"key_id\" is required");
    }
    atlas_apikey_job op;
    memset(&op, 0, sizeof op);
    op.kind = ATLAS_APIKEY_JOB_REVOKE;
    if (!atlas_apikey_id_normalise(id, op.key_id)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "\"key_id\" is not a key id");
    }

    atlas_apikey_job_result res;
    memset(&res, 0, sizeof res);
    atlas_status st = atlas_writer_apikey(ds->ctx->writer, &op, &res, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_json_key_str(ds->j, "id", op.key_id, err);
    if (st == ATLAS_OK) {
        /* False means no active key with that id. Not an error — the outcome
         * asked for already holds — and reported as a different fact. */
        st = atlas_json_key_bool(ds->j, "changed", res.changed, err);
    }
    return st;
}

/* Three names, and deliberately no fourth.
 *
 * There is no `apikey.show`, no `apikey.reveal` and no `apikey.export`, because
 * no operation anywhere in Atlas can return the plaintext of an existing
 * credential: the index holds a one-way verifier and there is no column to read
 * one from. Their absence is a property of the storage, not a refusal here. */
static const atlas_method_entry APIKEY_METHODS[] = {
    {"apikey.create", method_apikey_create},
    {"apikey.list", method_apikey_list},
    {"apikey.revoke", method_apikey_revoke},
};

const atlas_method_entry *atlas_server_apikey_methods(size_t *count_out) {
    if (count_out != NULL) {
        *count_out = sizeof(APIKEY_METHODS) / sizeof(APIKEY_METHODS[0]);
    }
    return APIKEY_METHODS;
}
