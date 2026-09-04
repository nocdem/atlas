/* Atlas - A16: the daemon's remote disposal method group.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Why this is a separate file rather than a further group in
 * `server_decision.c` or `server_gw.c`
 * ---------------------------------------------------------------------------
 * `server_decision.c` already restores a disjoint operator channel on exactly
 * this argument (see its own comment above `OPERATOR_METHODS[]`, around
 * `server_decision.c:2295-2330`): A7 deleted `decision.challenge` and the
 * spend family because they took no capability and asked for no terminal --
 * any process that could open the socket could mint one and spend it. They
 * came back in a group reachable only from the peer whose `SO_PEERCRED` uid
 * equals the *authority* policy's `operator_uid`.
 *
 * This group answers the same two verbs -- mint a capability, spend one --
 * for a second identity that is not that peer and is not gated by that
 * policy. The gateway's uid is named by the *gateway* policy
 * (`/etc/atlas/gateway.conf`), a different root-owned file with a different
 * administrator in mind, and reaching it is not enough on its own: the same
 * policy must also name a disposal credential and must state either that TLS
 * terminates in front of the listener or that the operator has written down
 * accepting a cleartext channel. Two policies, two peers, and a fourth
 * condition neither of the other two groups has. Folding these two methods
 * into `OPERATOR_METHODS[]` would make one `SO_PEERCRED` comparison stand for
 * two different grants; folding them into `GATEWAY_METHODS[]` would make
 * "may ask what a token can read" and "may change a decision's lifecycle
 * state" the same question. Neither one is, so this is its own table, its
 * own gate function, and its own file -- the same reasoning A8 gives for
 * keeping `job.` and `dispatch.` disjoint rather than merged with a role
 * flag.
 *
 * What this group is not
 * -----------------------
 * It is not the gateway's own authority: the gateway process itself holds
 * none, under A7.1, and everything here still runs on the daemon's own
 * writer thread, behind the one function that has written every lifecycle
 * transition since A4 -- `atlas_decision_apply_in_tx`, reached through
 * `atlas_writer_decision` exactly as the operator group reaches it. This
 * file constructs an `atlas_decision_op` with `channel = REMOTE` and a
 * caller-presented bearer credential; it verifies nothing about that
 * credential itself. Verification happens once, inside the transaction that
 * is about to spend the capability the credential earns
 * (`atlas_decision_remote_verify`, `src/decision/remote.c`) -- never here,
 * and never on the strength of anything the gateway merely claims. A key id
 * the gateway asserts is worthless on its own; the daemon holds the only
 * verifiers for one, which is the whole argument for why this credential is
 * authenticated where it is spent rather than where it is received.
 *
 * The bearer token and the capability token are two different strings
 * -----------------------------------------------------------------------
 * `decision.remote_challenge` and `decision.remote_dispose` both take a
 * `token` parameter -- the gateway's own bearer credential, added to every
 * forwarded request from the `Authorization` header, never from anything the
 * browser's form fields name. `decision.remote_dispose` separately takes a
 * `challenge` parameter -- the 32-hex capability `decision.remote_challenge`
 * minted. The two must not be confused: one authenticates *who is asking*,
 * the other proves *what was reviewed*. `op->remote_token` carries the
 * first; `op->token` (the same field the operator group's capability travels
 * on) carries the second.
 *
 * Deliberately not shared with `server_decision.c`
 * --------------------------------------------------
 * The small parameter helpers below (`remote_take_where`,
 * `remote_take_decision_uid`) and the response renderers duplicate the shape
 * of `server_decision.c`'s `take_where`, `take_doc_uid` and `write_result`
 * rather than reusing them. This is `atlas_decision_remote_verify`'s own
 * precedent, restated one layer up: two different callers with two
 * different peers and two different frozen response shapes sharing one
 * helper is one more surface where a future change to either quietly
 * becomes a change to both.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/decision_ops.h"
#include "atlas/safetext.h"
#include "ipc/server_internal.h"

/* Both methods write a short, bounded transaction on the writer thread, like
 * every A4 write. `server_decision.c`'s `DECISION_WRITE_TIMEOUT_MS` is the
 * same value for the same reason: a caller waiting longer than this is
 * waiting on something that has gone wrong, not on work. Restated rather
 * than shared, on this file's own precedent above. */
#define REMOTE_WRITE_TIMEOUT_MS 5000

/* --- the peer test ---------------------------------------------------------
 *
 * `atlas_server_remote_disposal_offered` is declared in server_internal.h
 * and asked twice, like `peer_is_gateway` in server_gw.c: once by
 * `atlas_server_dispatch` to decide whether the *name* is offered, and again
 * here to decide whether the *operation* runs. Reaching a name is never the
 * same as being allowed to use it. */

/* The policy half of the predicate, with no peer in it -- shared with
 * `gateway.auth`'s scope derivation (`method_gateway_auth`,
 * `src/ipc/server_gw.c`) so the scope that endpoint reports for a
 * credential and the methods the dispatcher actually offers for it can
 * never disagree. See server_internal.h for why it is split out rather than
 * inlined into the one caller this file itself has. */
bool atlas_server_remote_disposal_policy_ready(const atlas_gwpolicy *gw) {
    if (gw == NULL) {
        return false;
    }
    /* Measured, not hypothetical, and added after the two conditions below
     * were already written: `atlas_gwpolicy_parse_buffer` writes each field
     * as it parses the line naming it and `return`s at the first malformed
     * line *without* clearing what it already wrote -- the file's one
     * convention, applied to every key including these two. A policy with
     * both disposal keys, the cleartext acceptance line, `tls_mode = NONE`
     * and one unrelated unrecognised key loads with `state = DISABLED` and
     * `reason = MALFORMED`, and still leaves `remote_dispose_key` populated,
     * `remote_dispose_kinds` set and `cleartext_disposal_accepted` true --
     * satisfying both of the other conditions here. Reading one of those
     * fields without first asking whether the policy that produced it was
     * actually accepted is the mistake this condition exists to catch: a
     * refused policy is not a smaller grant than no policy, it is no grant
     * at all, and `state` is the one field that says which of those this
     * is. */
    if (gw->state != ATLAS_GWPOLICY_ENABLED) {
        return false;
    }
    if (gw->remote_dispose_key[0] == '\0') {
        return false;
    }
    /* Amended 2026-09-04: one condition added, never a check removed. TLS
     * terminating in front of the listener is the shape A9 was built for;
     * the operator's written acceptance of a cleartext channel is this
     * deployment's deliberate, recorded departure from it -- shown the
     * chain and choosing anyway, not a default and not something this
     * predicate may infer on its own. */
    if (gw->tls_mode != ATLAS_GWPOLICY_TLS_REVERSE_PROXY && !gw->cleartext_disposal_accepted) {
        return false;
    }
    return true;
}

bool atlas_server_remote_disposal_offered(const atlas_server_ctx *ctx, long long peer_uid) {
    if (ctx == NULL) {
        return false;
    }
    /* `atlas_server_peer_is_gateway` has a third branch below its root-owned-
     * policy comparison: with no gateway named it falls to the daemon's own
     * uid on an unseparated machine (`server_gw.c`'s "legacy per-user mode").
     * That branch is unreachable for this group specifically, and worth
     * saying so here rather than leaving a reader to work it out from two
     * files: `atlas_gwpolicy_parse_buffer` refuses `state = ENABLED` outright
     * when `gateway_uid <= 0` (`src/gw/gwpolicy.c`, "without it the daemon
     * cannot recognise the gateway"), and `atlas_server_remote_disposal_policy_ready`
     * below requires `state == ENABLED`. So by the time this call reaches the
     * peer test, `ctx->gwpolicy.gateway_uid` is already known positive, which
     * is exactly the condition under which `atlas_server_peer_is_gateway`
     * takes its first branch and returns on the root-owned comparison alone.
     * Order matters for why this is safe rather than merely true: the peer
     * test is asked first, so a caller cannot reach the policy test on the
     * strength of a legacy match this group never offers. */
    if (!atlas_server_peer_is_gateway(ctx, peer_uid)) {
        return false;
    }
    return atlas_server_remote_disposal_policy_ready(&ctx->gwpolicy);
}

/* --- parameter helpers, deliberately not shared -- see the file header --- */

/* Minor (review round 1): the Frozen formats' own params list for both
 * methods names only "repo", never "root". `op->root` and this `"root"`
 * branch exist because `atlas_decision_op` and `atlas_writer_decision` are
 * shared verbatim with `server_decision.c`'s LOCAL methods, which do accept a
 * root path -- this function is `atlas_decision_op`'s general "where"
 * resolver, not a per-method parameter list, so it inherited the branch
 * rather than needing a second one. It resolves only to a repository already
 * in the registry (`atlas_db_check_repo_name`'s sibling path does the same
 * for a name), the gateway never constructs or forwards a "root" key, and no
 * test in this file sends one -- named here so a reader does not have to
 * re-derive that it is inert rather than unreachable. */
static atlas_status remote_take_where(const atlas_ipc_request *req, atlas_decision_op *op,
                                      atlas_err *err) {
    const char *root = NULL;
    const char *repo = NULL;
    if (atlas_ipc_param_str(req, "root", &root) && root != NULL && root[0] != '\0') {
        return atlas_buf_set_str(&op->root, root, err);
    }
    if (atlas_ipc_param_str(req, "repo", &repo) && repo != NULL && repo[0] != '\0') {
        atlas_status st = atlas_db_check_repo_name(repo, err);
        if (st != ATLAS_OK) {
            return st;
        }
        return atlas_buf_set_str(&op->repo_name, repo, err);
    }
    return atlas_err_set(err, ATLAS_ERR_USAGE,
                         "this method needs a \"repo\" name or a \"root\" path");
}

static atlas_status remote_take_decision_uid(const atlas_ipc_request *req, atlas_buf *out,
                                             atlas_err *err) {
    const char *v = NULL;
    if (!atlas_ipc_param_str(req, "decision", &v) || v == NULL || v[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "this method needs a \"decision\" id");
    }
    if (!atlas_decision_uid_is_valid(v)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "\"decision\" is not a decision id; they look like %s followed by "
                             "%u lowercase hex characters",
                             ATLAS_DECISION_UID_PREFIX, (unsigned)ATLAS_DECISION_UID_HEX);
    }
    return atlas_buf_set_str(out, v, err);
}

/* The bearer credential, into `op->remote_expected_key_id` and
 * `op->remote_token`. Every op this group builds carries both: the policy's
 * own key id is what the write point compares the presented credential
 * against, never anything the request supplies, and the presented token is
 * what it authenticates. */
static void remote_take_credential(dispatch_state *ds, atlas_decision_op *op) {
    op->channel = ATLAS_DECISION_CHANNEL_REMOTE;
    op->remote_kinds = ds->ctx->gwpolicy.remote_dispose_kinds;
    (void)snprintf(op->remote_expected_key_id, sizeof(op->remote_expected_key_id), "%s",
                   ds->ctx->gwpolicy.remote_dispose_key);
}

/* Minor (review round 1): an absent `"token"` parameter here produces the
 * same sentence `atlas_decision_remote_verify` (`src/decision/remote.c`)
 * gives a *present but wrong* one -- one sentence covering two conditions
 * from a third file, deliberately, per that file's own header: a caller
 * distinguishing "you sent nothing" from "what you sent was wrong" learns
 * which half of a guess to vary next. In practice this branch is
 * unreachable through the gateway itself, which answers a request with no
 * bearer credential with its own 401 before a JSON-RPC call naming this
 * method is ever made; it is reachable only from a caller on the daemon
 * socket directly, and gives that caller no more information than the
 * gateway would have. */
static atlas_status remote_take_token(const atlas_ipc_request *req, atlas_buf *out,
                                      atlas_err *err) {
    const char *v = NULL;
    if (!atlas_ipc_param_str(req, "token", &v) || v == NULL || v[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "the credential presented for this disposal did not authenticate; "
                             "nothing was changed");
    }
    return atlas_buf_set_str(out, v, err);
}

static atlas_decision_op *remote_op_new(atlas_decision_op_kind kind) {
    atlas_decision_op *op = calloc(1u, sizeof(*op));
    if (op != NULL) {
        atlas_decision_op_init(op, kind);
    }
    return op;
}

static atlas_status remote_submit(dispatch_state *ds, atlas_decision_op *op,
                                  atlas_decision_result *result, atlas_err *err) {
    if (ds->ctx->writer == NULL) {
        atlas_decision_op_free(op);
        free(op);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "this Atlas daemon has no writer, so nothing can be recorded");
    }
    return atlas_writer_decision(ds->ctx->writer, op, REMOTE_WRITE_TIMEOUT_MS, result, err);
}

/* --- decision.remote_challenge --------------------------------------------- */

/* The mint response. Deliberately not `server_decision.c`'s `write_result`
 * plus the local method's four extra keys: the frozen shape here omits
 * `confirm` (a browser confirms by typing a digest prefix it was already
 * shown, not by being told the phrase) and adds `key_id` (so the browser can
 * display, before spending anything, which credential the daemon verified).
 * See the file header for why this is a deliberate duplicate rather than a
 * shared helper. */
static atlas_status write_remote_challenge_result(dispatch_state *ds,
                                                   const atlas_decision_result *r,
                                                   atlas_err *err) {
    atlas_status st = atlas_json_key_str(ds->j, "repo", atlas_buf_cstr(&r->repo_name), err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "repo_id", r->repo_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "decision", atlas_buf_cstr(&r->uid), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "revision", r->revision_no, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "content_hash", r->content_hash, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "state", atlas_decision_state_name(r->state), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "kind", atlas_decision_kind_name(r->knowledge_kind), err);
    }
    if (st == ATLAS_OK) {
        /* Model- or operator-authored prose, displayed to whoever is about to
         * confirm a disposal, encoded on the way out like every other
         * untrusted value that reaches a client. */
        st = atlas_json_key_str(ds->j, "title", atlas_safe(&ds->safe, atlas_buf_cstr(&r->title)),
                                err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "token", atlas_buf_cstr(&r->token), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "expires_at", r->expires_at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "key_id", r->key_id, err);
    }
    return st;
}

static atlas_status method_remote_challenge(dispatch_state *ds, const atlas_ipc_request *req,
                                            atlas_err *err) {
    if (!atlas_server_remote_disposal_offered(ds->ctx, ds->peer_uid)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "unknown method \"decision.remote_challenge\"");
    }

    atlas_decision_op *op = remote_op_new(ATLAS_DECISION_OP_CHALLENGE);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    remote_take_credential(ds, op);

    atlas_status st = remote_take_where(req, op, err);
    if (st == ATLAS_OK) {
        st = remote_take_decision_uid(req, &op->uid, err);
    }
    if (st == ATLAS_OK) {
        /* Absent reads as 0, which `op_challenge` refuses for a REMOTE
         * channel with its own frozen sentence -- a browser names the exact
         * revision it displayed, and there is no "whichever is newest"
         * shorthand here the way a local `--revision` omission has. */
        (void)atlas_ipc_param_int(req, "revision", &op->expect_revision_no);
    }
    if (st == ATLAS_OK) {
        const char *intent = NULL;
        /* No default. A local mint may omit `intent` and mean "approve" --
         * this one may not, because a browser always knows which of the
         * three buttons was pressed, and defaulting here would let a
         * malformed request mint a capability for an action nobody asked
         * for. */
        if (!atlas_ipc_param_str(req, "intent", &intent) || intent == NULL ||
            !atlas_decision_intent_parse(intent, &op->intent)) {
            st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"intent\" is approve, reject or resolve");
        }
    }
    if (st == ATLAS_OK) {
        st = remote_take_token(req, &op->remote_token, err);
    }
    if (st != ATLAS_OK) {
        atlas_decision_op_free(op);
        free(op);
        return st;
    }

    atlas_decision_result result;
    atlas_decision_result_init(&result);
    st = remote_submit(ds, op, &result, err);
    if (st == ATLAS_OK) {
        st = write_remote_challenge_result(ds, &result, err);
    }
    atlas_daemon_log(ds->ctx->log, "info", "decision.remote_challenge: %s",
                     st == ATLAS_OK ? "issued" : atlas_safe(&ds->safe, atlas_err_msg(err)));
    atlas_decision_result_free(&result);
    return st;
}

/* --- decision.remote_dispose ------------------------------------------------ */

/* `server_decision.c:559`'s `write_result` shape, duplicated -- see the file
 * header. */
static atlas_status write_remote_dispose_result(dispatch_state *ds,
                                                 const atlas_decision_result *r, atlas_err *err) {
    atlas_status st = atlas_json_key_str(ds->j, "repo", atlas_buf_cstr(&r->repo_name), err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "repo_id", r->repo_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "decision", atlas_buf_cstr(&r->uid), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "revision", r->revision_no, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "content_hash", r->content_hash, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "state", atlas_decision_state_name(r->state), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "kind", atlas_decision_kind_name(r->knowledge_kind), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "created", r->document_created, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "duplicate", r->duplicate, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "session_unbound", r->session_unbound, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "unbound_reason", r->unbound_reason, err);
    }
    return st;
}

static atlas_status method_remote_dispose(dispatch_state *ds, const atlas_ipc_request *req,
                                          atlas_err *err) {
    if (!atlas_server_remote_disposal_offered(ds->ctx, ds->peer_uid)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "unknown method \"decision.remote_dispose\"");
    }

    const char *intent_text = NULL;
    atlas_decision_intent intent = ATLAS_DECISION_INTENT_APPROVE;
    if (!atlas_ipc_param_str(req, "intent", &intent_text) || intent_text == NULL ||
        !atlas_decision_intent_parse(intent_text, &intent)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "\"intent\" is approve, reject or resolve");
    }
    /* The op kind the intent names -- `spend_method`'s own mapping in
     * `server_decision.c`, chosen there by which local subcommand was typed
     * and chosen here by this field instead, because one endpoint serves all
     * three browser actions. Supersede and revalidate parse here too rather
     * than being rejected at this layer: the write point already refuses
     * both for a REMOTE channel with the season's frozen sentence
     * (`op_challenge` at mint, `op_supersede`/`op_revalidate` at spend), and
     * a challenge minted through this same policy could never have been
     * minted with either intent in the first place, so `spend_challenge`'s
     * own intent-mismatch refusal fires first in practice. Narrowing the
     * vocabulary a second time here would be a second copy of a rule the
     * write point already owns. */
    atlas_decision_op_kind kind;
    switch (intent) {
    case ATLAS_DECISION_INTENT_APPROVE: kind = ATLAS_DECISION_OP_APPROVE; break;
    case ATLAS_DECISION_INTENT_REJECT: kind = ATLAS_DECISION_OP_REJECT; break;
    case ATLAS_DECISION_INTENT_SUPERSEDE: kind = ATLAS_DECISION_OP_SUPERSEDE; break;
    case ATLAS_DECISION_INTENT_REVALIDATE: kind = ATLAS_DECISION_OP_REVALIDATE; break;
    case ATLAS_DECISION_INTENT_RESOLVE: kind = ATLAS_DECISION_OP_RESOLVE; break;
    default: return atlas_err_set(err, ATLAS_ERR_USAGE, "\"intent\" is approve, reject or resolve");
    }

    atlas_decision_op *op = remote_op_new(kind);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    remote_take_credential(ds, op);

    atlas_status st = remote_take_where(req, op, err);
    if (st == ATLAS_OK) {
        st = remote_take_decision_uid(req, &op->uid, err);
    }
    if (st == ATLAS_OK) {
        st = remote_take_token(req, &op->remote_token, err);
    }
    /* `challenge` carries the 32-hex capability into `op->token` -- the same
     * field the operator group's `spend_method` calls `token`, renamed on
     * this wire so it is never confused with the bearer credential above.
     * `confirmation` is what the operator typed. Both are read here and in
     * `spend_method`, and nowhere a model reaches: the first is offered only
     * to the operator's uid, the second only to the gateway's uid behind TLS
     * and a named credential. */
    if (st == ATLAS_OK) {
        const char *v = NULL;
        if (!atlas_ipc_param_str(req, "challenge", &v) || v == NULL || v[0] == '\0') {
            st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                               "this operation needs a disposal challenge issued by "
                               "decision.remote_challenge");
        } else if (strlen(v) != ATLAS_DECISION_CHALLENGE_HEX) {
            st = atlas_err_set(err, ATLAS_ERR_INTEGRITY, "that is not an Atlas disposal challenge");
        } else {
            st = atlas_buf_set_str(&op->token, v, err);
        }
    }
    if (st == ATLAS_OK) {
        const char *v = NULL;
        if (!atlas_ipc_param_str(req, "confirmation", &v) || v == NULL) {
            st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                               "this operation needs the confirmation shown at the prompt");
        } else if (strlen(v) > ATLAS_DECISION_CONFIRM_MAX - 1u) {
            st = atlas_err_set(err, ATLAS_ERR_INTEGRITY, "that confirmation is too long");
        } else {
            st = atlas_buf_set_str(&op->confirmation, v, err);
        }
    }
    if (st != ATLAS_OK) {
        atlas_decision_op_free(op);
        free(op);
        return st;
    }

    atlas_decision_result result;
    atlas_decision_result_init(&result);
    st = remote_submit(ds, op, &result, err);
    if (st == ATLAS_OK) {
        st = write_remote_dispose_result(ds, &result, err);
    }
    if (st == ATLAS_OK && result.superseded_revision_no > 0) {
        st = atlas_json_key_int(ds->j, "superseded_revision", result.superseded_revision_no, err);
    }
    if (st == ATLAS_OK) {
        /* Read back from what the write point actually recorded, exactly as
         * `spend_method` reports it -- a literal here would be the same
         * drift risk that comment warns about, one layer over. */
        st = atlas_json_key_str(ds->j, "actor", atlas_decision_actor_name(result.actor), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "key_id", result.key_id, err);
    }
    if (st == ATLAS_OK) {
        /* The season's frozen sentence, said in the response and not only in
         * documentation: a client reading this cannot honestly report that
         * Atlas identified a person, and cannot honestly report that this
         * channel is as strong as a terminal on the Atlas machine. */
        st = atlas_json_key_str(
            ds->j, "actor_means",
            "an explicit action arrived through Atlas' remote operator channel: the credential "
            "named in key_id was presented over the gateway's listener, under whatever "
            "transport security that listener has, which Atlas does not verify. This does not "
            "identify a person, does not prove a person was present, and is not a signature. It "
            "is weaker than the local channel by construction: the credential passed through a "
            "network-facing process.",
            err);
    }
    atlas_daemon_log(ds->ctx->log, "info", "decision.remote_dispose: %s",
                     st == ATLAS_OK ? "spent" : atlas_safe(&ds->safe, atlas_err_msg(err)));
    atlas_decision_result_free(&result);
    return st;
}

/* --- the group -------------------------------------------------------------- */

static const atlas_method_entry REMOTE_DISPOSAL_METHODS[] = {
    {"decision.remote_challenge", method_remote_challenge},
    {"decision.remote_dispose", method_remote_dispose},
};

const atlas_method_entry *atlas_server_remote_disposal_methods(size_t *count_out) {
    if (count_out != NULL) {
        *count_out = sizeof(REMOTE_DISPOSAL_METHODS) / sizeof(REMOTE_DISPOSAL_METHODS[0]);
    }
    return REMOTE_DISPOSAL_METHODS;
}
