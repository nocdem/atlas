/* Atlas - the gateway-only method group.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Why this file exists
 * --------------------
 * The gateway terminates connections from the Internet. It must therefore hold
 * as little authority as possible — and under A7.1 it holds none over the index
 * at all, because it runs as its own account and the index is 0700 `atlasd`.
 * That is the guarantee, and it is enforced by the filesystem rather than by
 * anything in this file.
 *
 * But a gateway that cannot read the index also cannot verify a credential or
 * record what it served. So it asks the daemon, over the same Unix socket every
 * other client uses, and these are the three questions it may ask:
 *
 *   `gateway.auth`        does this presented token belong to an active
 *                         credential, and what may that credential read?
 *   `gateway.audit`       record that a request happened.
 *   `gateway.audit_list`  read back the sanitized trail.
 *
 * What the gateway is *not*
 * -------------------------
 * It is not an operator and not a dispatcher. Its uid appears in neither the
 * authority policy nor the orchestration policy, so `decision.approve`,
 * `backup.create`, `code.index`, `job.submit` and every `dispatch.` method
 * answer `unknown method` to it — the same answer a name that does not exist
 * gets. **A compromised gateway cannot approve a decision, register a
 * repository, read a backup, run a job or build an index.** That sentence is
 * true because of who it runs as, not because of a check here.
 *
 * It also holds no credential-administration verb. There is no
 * `gateway.apikey_create`, no rotate and no revoke, and their absence is A9.3's
 * requirement made structural: remote credential administration is not refused
 * in A9, it does not exist.
 *
 * The token never leaves this boundary
 * ------------------------------------
 * `gateway.auth` receives the presented token over a Unix socket that only the
 * gateway uid can open, verifies it, and returns a verdict. The token is never
 * stored, never logged, never audited and never echoed into an error. The
 * *reason* it is sent at all rather than verified in the gateway is that the
 * verifier lives in the index, which the gateway cannot read — which is exactly
 * the separation A9 wanted.
 */
#include <string.h>
#include <unistd.h>

#include "atlas/apikey.h"
#include "atlas/gw.h"
#include "atlas/limits.h"
#include "atlas/safetext.h"
#include "server_internal.h"

/* The peer test, asked again at the write point.
 *
 * It delegates to `atlas_server_peer_is_gateway` rather than repeating the
 * rule. Two copies of a security check are two places for one of them to be
 * weaker, and the weaker one is the one an attacker uses — which is not
 * hypothetical here: this function originally checked only the policy case,
 * disagreed with the dispatcher about legacy mode, and made every gateway
 * request on an unseparated machine fail as "unauthenticated" with nothing
 * saying why.
 *
 * Asked twice on purpose, though: the dispatcher decides whether the *name* is
 * offered, and this decides whether the *operation* runs. Reaching a name is
 * never the same as being allowed to use it — A8's rule about routing not being
 * authorisation. */
static bool peer_is_gateway(dispatch_state *ds) {
    return atlas_server_peer_is_gateway(ds->ctx, (long long)ds->peer_uid);
}

/* gateway.auth — verify a presented token and report what it may read.
 *
 * The answer distinguishes only two outcomes to the caller: authenticated, with
 * a principal and a scope set, or not. It deliberately does **not** say whether
 * the selector was unknown, the secret was wrong, the credential was revoked or
 * its stored scopes were unreadable: a caller learning which half of a token was
 * right learns what to vary next. The daemon's own log keeps the distinction,
 * because an operator debugging a 401 needs it and an attacker cannot read it. */
static atlas_status method_gateway_auth(dispatch_state *ds, const atlas_ipc_request *req,
                                        atlas_err *err) {
    if (!peer_is_gateway(ds)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "unknown method \"gateway.auth\"");
    }
    const char *token = NULL;
    if (!atlas_ipc_param_str(req, "token", &token) || token == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "\"token\" is required");
    }

    char selector[ATLAS_APIKEY_SELECTOR_HEX + 1];
    unsigned char secret[ATLAS_APIKEY_SECRET_BYTES];
    bool authenticated = false;
    atlas_apikey_record rec;
    memset(&rec, 0, sizeof(rec));

    atlas_err perr;
    atlas_err_init(&perr);
    if (atlas_apikey_token_parse(token, selector, secret, &perr) == ATLAS_OK) {
        bool found = false;
        atlas_err lerr;
        atlas_err_init(&lerr);
        if (atlas_db_apikey_lookup(ds->db, selector, &rec, &found, &lerr) == ATLAS_OK && found) {
            /* Every condition is checked, and every failure produces the same
             * outward answer. `atlas_apikey_verify` is constant-time, and the
             * lookup is an indexed equality test, so the work done for a wrong
             * secret does not differ measurably from the work done for a right
             * one. */
            authenticated = rec.status == ATLAS_APIKEY_STATUS_ACTIVE && !rec.scopes_unreadable &&
                            atlas_apikey_verify(secret, sizeof secret, rec.salt, sizeof rec.salt,
                                                rec.verifier, sizeof rec.verifier);
        }
    }
    /* The secret goes before anything else happens, including before the
     * response is built. */
    memset(secret, 0, sizeof secret);

    atlas_status st = atlas_json_key_bool(ds->j, "authenticated", authenticated, err);
    if (st == ATLAS_OK && authenticated) {
        st = atlas_json_key_str(ds->j, "key_id", rec.key_id, err);
        if (st == ATLAS_OK) {
            /* The label is operator text, validated to printable ASCII at
             * creation and safe-encoded here anyway: it reaches an audit row
             * and a browser, and the encoding is what makes both inert. */
            st = atlas_json_key_str(ds->j, "label", atlas_safe(&ds->safe, rec.label), err);
        }
        if (st == ATLAS_OK) {
            /* A16. `decisions:dispose` is never stored on a key row -- T1's
             * rule, enforced at the one function that writes one
             * (`atlas_db_apikey_insert`). It is derived here instead, for
             * exactly the credential the root-owned gateway policy names as
             * the disposal key, and only when that credential's own stored
             * scope list is empty: a `--no-scopes` credential is inert until
             * this policy gives it its one grant, and a credential that
             * already holds ordinary scopes is never widened by being named
             * here.
             *
             * There is no load-time refusal of naming a non-scopeless
             * credential: `atlas_gwpolicy_parse_buffer` opens no database and
             * checks only `remote_dispose_key`'s shape, so a policy naming an
             * ordinary reader key loads exactly as cleanly as one naming a
             * `--no-scopes` credential does. `rec.mask == 0u`, right here, is
             * the whole of what stops that key's scopes being reported as
             * `decisions:dispose` -- not a second check behind a first one,
             * the only one. The actual guarantee against a *scoped*
             * credential spending a disposal is `atlas_decision_remote_verify`
             * (`src/decision/remote.c`), inside the write transaction: it
             * refuses `rec.mask != 0u` on its own account, independent of
             * whatever this endpoint reports. Removing the check here would
             * not let a scoped credential dispose anything -- it would make
             * `gateway.auth` claim a scope the write point would then refuse
             * to honour, trading a clean 403 (`gateway.auth` never having
             * offered the scope) for a 409 raised deep inside a spend the
             * caller had no way to know would fail.
             *
             * Gated on `atlas_server_remote_disposal_policy_ready`, the same
             * test `decision.remote_challenge` and `decision.remote_dispose`
             * are offered under (minus the peer half, moot here: this
             * request already reached a gateway-only method). A policy this
             * loader refused, or one with no TLS in front and no written
             * cleartext acceptance, derives nothing, so what this endpoint
             * reports a credential can do and what the dispatcher will
             * actually let it do never disagree. */
            const char *scopes = rec.scopes;
            if (rec.mask == 0u && atlas_server_remote_disposal_policy_ready(&ds->ctx->gwpolicy) &&
                strcmp(rec.key_id, ds->ctx->gwpolicy.remote_dispose_key) == 0) {
                scopes = atlas_apikey_scope_name(ATLAS_SCOPE_DECISIONS_DISPOSE);
            }
            st = atlas_json_key_str(ds->j, "scopes", scopes, err);
        }
    }
    /* Nothing else is reported. Not the selector, not whether it existed, not
     * the status, not the salt, and never the token. */
    memset(&rec, 0, sizeof(rec));
    return st;
}

/* Copies a bounded, safe-encoded string parameter into a fixed field.
 *
 * Everything the gateway records passes through here. The encoding is the
 * audit-log injection defence: a crafted tool name or failure message cannot
 * produce anything that reads as a second row, and the encoding is reversible so
 * nothing is lost. Over-long is truncated rather than refused, because an audit
 * row is written after the request it describes was already answered — refusing
 * it would drop the record of the very request most worth keeping. */
static void take_audit_text(dispatch_state *ds, const atlas_ipc_request *req, const char *key,
                            char *dst, size_t dst_size) {
    dst[0] = '\0';
    const char *v = NULL;
    if (!atlas_ipc_param_str(req, key, &v) || v == NULL) {
        return;
    }
    const char *safe = atlas_safe(&ds->safe, v);
    size_t n = strlen(safe);
    if (n >= dst_size) {
        n = dst_size - 1u;
    }
    memcpy(dst, safe, n);
    dst[n] = '\0';
}

/* gateway.audit — record one request.
 *
 * Queued to the writer and answered immediately. The gateway never learns
 * whether the row landed, which is what makes "audit failure does not break
 * request handling" structural rather than something a caller must remember. */
static atlas_status method_gateway_audit(dispatch_state *ds, const atlas_ipc_request *req,
                                         atlas_err *err) {
    if (!peer_is_gateway(ds)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "unknown method \"gateway.audit\"");
    }

    atlas_gw_audit_entry e;
    atlas_gw_audit_entry_init(&e);

    const char *iface = NULL;
    if (!atlas_ipc_param_str(req, "interface", &iface) || iface == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "\"interface\" is required");
    }
    e.iface = atlas_gw_interface_parse(iface);
    if (e.iface == ATLAS_GW_IFACE_UNKNOWN) {
        /* Refused rather than defaulted. A row that cannot say which surface a
         * request arrived on cannot answer the question the table exists for,
         * and picking one would be inventing the answer. */
        return atlas_err_set(err, ATLAS_ERR_USAGE, "\"interface\" is not one Atlas records");
    }

    take_audit_text(ds, req, "key_id", e.key_id, sizeof e.key_id);
    take_audit_text(ds, req, "label", e.label, sizeof e.label);
    take_audit_text(ds, req, "operation", e.operation, sizeof e.operation);
    take_audit_text(ds, req, "detail", e.detail, sizeof e.detail);
    if (e.operation[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "\"operation\" is required");
    }

    bool allowed = false;
    (void)atlas_ipc_param_bool(req, "allowed", &allowed);
    /* DENIED is the zero and the default. A row nobody filled in must not claim
     * a request was permitted. */
    e.decision = allowed ? ATLAS_GW_ALLOWED : ATLAS_GW_DENIED;

    const char *outcome = NULL;
    if (atlas_ipc_param_str(req, "outcome", &outcome) && outcome != NULL) {
        if (strcmp(outcome, "OK") == 0) {
            e.outcome = ATLAS_GW_OUTCOME_OK;
        } else if (strcmp(outcome, "FAILED") == 0) {
            e.outcome = ATLAS_GW_OUTCOME_FAILED;
        }
        /* Anything else stays UNKNOWN, which is the zero: a request whose
         * outcome was never recorded did not succeed as far as this table is
         * concerned. */
    }

    int64_t n = 0;
    if (atlas_ipc_param_int(req, "status", &n) && n >= 0 && n <= 7) {
        e.status = (int32_t)n;
    }
    if (atlas_ipc_param_int(req, "duration_ms", &n) && n >= 0) {
        e.duration_ms = n;
    }

    atlas_err qerr;
    atlas_err_init(&qerr);
    bool queued = atlas_writer_gw_audit(ds->ctx->writer, &e, &qerr) == ATLAS_OK;
    /* Reported, never fatal. A gateway that could be made to fail by filling
     * the write queue would be a gateway an attacker can switch off. */
    return atlas_json_key_bool(ds->j, "queued", queued, err);
}

/* gateway.audit_list — read the sanitized trail.
 *
 * Every field here was safe-encoded on the way in, so this reproduces stored
 * bytes rather than re-encoding them — the A8.2 double-encoding rule. There is
 * no column that could hold a secret, so there is nothing to omit. */
/* One row of the trail, written straight into the response document.
 *
 * The callback's pointers are borrowed and everything is consumed here, so
 * nothing outlives the statement. Every text field was safe-encoded on the way
 * in, so this reproduces stored bytes rather than encoding them again — the
 * A8.2 double-encoding rule. */
typedef struct audit_sink {
    dispatch_state *ds;
} audit_sink;

static atlas_status write_audit_row(const atlas_gw_audit_entry *e, void *ud, atlas_err *err) {
    audit_sink *s = (audit_sink *)ud;
    atlas_json *j = s->ds->j;
    atlas_status st = atlas_json_obj_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "id", e->id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "at", e->at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "interface", atlas_gw_interface_name(e->iface), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "key_id", e->key_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "label", e->label, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "operation", e->operation, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "decision", atlas_gw_decision_name(e->decision), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "outcome", atlas_gw_outcome_name(e->outcome), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "status", e->status, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "duration_ms", e->duration_ms, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "detail", e->detail, err);
    }
    /* There is no field here that could hold a secret, so there is nothing to
     * omit and nothing a future edit could accidentally include. */
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(j, err);
    }
    return st;
}

/* gateway.audit_list — read the sanitized trail, newest first. */
static atlas_status method_gateway_audit_list(dispatch_state *ds, const atlas_ipc_request *req,
                                              atlas_err *err) {
    if (!peer_is_gateway(ds)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "unknown method \"gateway.audit_list\"");
    }
    int64_t limit = 0;
    int64_t before = 0;
    (void)atlas_ipc_param_int(req, "limit", &limit);
    (void)atlas_ipc_param_int(req, "cursor", &before);
    if (limit <= 0 || limit > ATLAS_GW_AUDIT_MAX_ROWS) {
        limit = 100;
    }
    if (before < 0) {
        before = 0;
    }
    const char *key_filter = NULL;
    (void)atlas_ipc_param_str(req, "key_id", &key_filter);

    atlas_status st = atlas_json_key(ds->j, "events", err);
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    if (st != ATLAS_OK) {
        return st;
    }

    audit_sink sink = {ds};
    int64_t count = 0;
    bool more = false;
    st = atlas_db_gw_audit_list(ds->db, limit, before, key_filter, write_audit_row, &sink, &count,
                                &more, err);
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "count", count, err);
    }
    /* Reaching the page limit is reported rather than being inferred from a
     * full page: a page that happens to be exactly `limit` long is not evidence
     * of anything. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "more", more, err);
    }
    return st;
}

static const atlas_method_entry GATEWAY_METHODS[] = {
    {"gateway.auth", method_gateway_auth},
    {"gateway.audit", method_gateway_audit},
    {"gateway.audit_list", method_gateway_audit_list},
};

const atlas_method_entry *atlas_server_gateway_methods(size_t *count_out) {
    if (count_out != NULL) {
        *count_out = sizeof(GATEWAY_METHODS) / sizeof(GATEWAY_METHODS[0]);
    }
    return GATEWAY_METHODS;
}

bool atlas_server_peer_is_gateway(const atlas_server_ctx *ctx, long long peer_uid) {
    if (ctx == NULL) {
        return false;
    }
    /* A root-owned policy names the gateway. This is the separated deployment,
     * and it is the only case in which this group is a real boundary. */
    if (ctx->gwpolicy.gateway_uid > 0) {
        return ctx->gwpolicy.gateway_uid == peer_uid;
    }
    /* Under a **system** deployment with no gateway policy the group stays
     * hidden. Fail closed: a machine that has been separated and has not
     * configured a gateway has not asked for one. */
    if (ctx->syspolicy.state == ATLAS_SYSPOLICY_SYSTEM) {
        return false;
    }
    /* Legacy per-user mode: the daemon's own uid, and this needs its argument
     * written down rather than assumed.
     *
     * On an unseparated machine the gateway would run as the same account that
     * owns the index. That account can already open `atlas.db` with `sqlite3`
     * and read `api_keys` with no Atlas code involved, so offering it
     * `gateway.auth` grants it nothing it does not already have — the same
     * reasoning A7 gives for not guarding backup there, where a check an
     * adversary simply walks around reads as protection in a review and
     * provides none.
     *
     * What this buys is that A9 is usable on a single-user machine at all,
     * which is how most people will run it. What it does **not** buy is any
     * separation: on such a machine a compromised gateway is a compromised
     * everything, and `docs/remote-access.md` says exactly that rather than
     * letting a reader infer the A7.1 guarantee applies. The separation is real
     * only when a root-owned policy names a distinct `gateway_uid` and the
     * index is 0700 `atlasd`. */
    return peer_uid == (long long)getuid();
}
