/* Atlas - A17 T2: the daemon's `deploy.remote_*` method group.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Six methods, one file, beside `server_orch_remote.c` (A14) and
 * `server_remote.c` (A16) rather than folded into either: the peer test this
 * group is offered under adds a policy condition neither of theirs alone
 * has (a named deploy key, in addition to -- or instead of -- a named
 * submit key), so one `SO_PEERCRED` comparison would otherwise be made to
 * answer for four different grants.
 *
 *   deploy.remote_propose {job, token}                  -- a submit credential
 *   deploy.remote_get {deploy, token}                    -- submit or deploy
 *   deploy.remote_list {cursor?, token}                  -- submit or deploy
 *   deploy.remote_cancel {deploy, token}                 -- the proposing submit credential
 *   deploy.remote_challenge {deploy, token}              -- the deploy credential
 *   deploy.remote_confirm {deploy, challenge, confirmation, token} -- the deploy credential
 *
 * ## Credential verification is at the IPC edge here, not inside the write
 * transaction
 *
 * A14's `job.remote_submit` carries a bearer token all the way to
 * `atlas_orch_apply_in_tx` and verifies it there, because a submission's
 * refusal (budget, policy, idempotency) has to be one atomic decision with
 * the credential check. T1 built this domain's write functions differently:
 * `atlas_db_deploy_propose_in_tx`, `_confirm_in_tx` and `_cancel_in_tx` all
 * take an already-resolved `key_id`, never a token. So the credential is
 * verified here, on the calling thread, with `atlas_orch_remote_verify`
 * (`src/orch/remote.c`) -- called, never copied, exactly as this file's own
 * brief requires -- before a job carrying the resolved id is ever queued to
 * the writer. This is safe for the same reason `job.remote_get`/`_list`
 * already verify on this same thread before reading: the daemon's serve
 * loop (`atlas_server_serve`, `src/ipc/server.c`) is a single poll-driven
 * thread that finishes one request's whole handling, including the
 * synchronous wait for a writer job to complete, before starting the next
 * one. There is no interleaving to reason about.
 *
 * ## The mint-then-spend challenge is this file's own mechanism, not T1's
 *
 * `deploy.remote_challenge` mints a capability bound to one deploy uid and
 * that deploy's own `patch_digest`, valid for `ATLAS_DECISION_CHALLENGE_TTL_MS`
 * (A16's own TTL, reused rather than restated) and spendable once, by
 * `deploy.remote_confirm`. Migration 33 (T1) adds no table for this -- only
 * `deploys` and `deploy_transitions` -- so there is nowhere to persist a
 * challenge row the way A16's `decision_challenges` does. The challenge
 * therefore lives in a small, bounded, file-static array in this process:
 * mutated only from the single serve-loop thread described above, so it
 * needs no lock of its own any more than `dispatch_state` itself does. It is
 * lost on a daemon restart, which only ever costs an operator one re-mint --
 * D.1's own "minutes between confirm and restart" window is about the
 * *target* system's restart, not this daemon's, and the two are different
 * processes on machines where they are even the same host.
 *
 * Consumption happens only after `deploy.remote_confirm`'s write commits
 * (`atlas_writer_deploy` returning ATLAS_OK), not before dispatch: a
 * challenge whose spend was refused by T1's own CAS (wrong state, wrong
 * prefix, a job still running) must remain spendable, so the operator is not
 * forced to reload Mission Control and re-mint for a condition that may
 * already have cleared.
 *
 * ## What this group cannot name
 *
 * `deploy.remote_apply`, `deploy.remote_install`, `deploy.remote_restart`,
 * `deploy.remote_run` are forbidden names that must never exist in the
 * protocol -- `tests/test_orch_rpc.c` scans for them, A14's own precedent
 * extended to a fourth verb set. Nothing here starts a process, applies a
 * patch, or restarts anything: those verbs belong to the root agent
 * (`deploy/a17/atlas-deploy-agent.sh`), which reads a request file this
 * daemon composed and never touches this socket.
 */
#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "atlas/atlas.h"
#include "atlas/deploy.h"
#include "atlas/gw.h"
#include "atlas/gwpolicy.h"
#include "atlas/hmac.h"
#include "atlas/ipc.h"
#include "atlas/orch_remote.h"
#include "atlas/sha256.h"
#include "server_internal.h"

/* --- predicates -------------------------------------------------------------- */

static bool deploy_key_pool_ready(const atlas_gwpolicy *gw) {
    return gw->remote_deploy_key[0] != '\0' &&
          (gw->tls_mode == ATLAS_GWPOLICY_TLS_REVERSE_PROXY || gw->cleartext_deploy_accepted);
}

/* Coarse: is *any* of the two pools this group ever needs ready at all. Used
 * only to decide whether it is worth entering the per-name lookup in
 * `atlas_server_remote_deploy_method_offered` below -- never to answer
 * whether one particular method is offered, which is why `gwpolicy.h`'s own
 * "offered to nobody" sentence at `remote_deploy_key` is about that finer
 * question and this function alone cannot honour it. */
bool atlas_server_remote_deploy_policy_ready(const atlas_gwpolicy *gw) {
    if (gw == NULL || gw->state != ATLAS_GWPOLICY_ENABLED) {
        return false;
    }
    return atlas_server_remote_submit_policy_ready(gw) || deploy_key_pool_ready(gw);
}

bool atlas_server_remote_deploy_offered(const atlas_server_ctx *ctx, long long peer_uid) {
    if (ctx == NULL) {
        return false;
    }
    if (!atlas_server_peer_is_gateway(ctx, peer_uid)) {
        return false;
    }
    return atlas_server_remote_deploy_policy_ready(&ctx->gwpolicy);
}

/* Per-method-group readiness -- the fix for the bug the coarse OR above
 * caused: `deploy.remote_challenge`/`_confirm` are offered as *names* only
 * when `remote_deploy_key` is actually set, exactly what `gwpolicy.h`'s own
 * comment at that field promises ("offered to nobody" when empty) and what
 * the coarse predicate above did not honour -- a policy naming only a submit
 * key made `atlas_server_remote_deploy_policy_ready` true, which made
 * `_challenge`/`_confirm` reachable by name even with no deploy credential
 * configured for them at all, so a caller reached this file's own usage
 * refusal (`verify_deploy_credential` failing to find any allowed key)
 * instead of the `unknown method` a caller with no such capability should
 * see, in a naming scheme that lets a caller distinguish the two. Symmetric
 * for `deploy.remote_propose`/`_get`/`_list`/`_cancel`: offered only when a
 * submit key is ready, because proposing (and, for uniformity, the rest of
 * that quartet) needs one to exist regardless of `_get`/`_list`'s own
 * credential check separately accepting the deploy credential as well --
 * that credential-level acceptance is unaffected by whether the *name* is
 * offered to a submit-only caller. */
bool atlas_server_remote_deploy_method_offered(const atlas_server_ctx *ctx, long long peer_uid,
                                               const char *method_name) {
    if (ctx == NULL || method_name == NULL) {
        return false;
    }
    if (!atlas_server_peer_is_gateway(ctx, peer_uid)) {
        return false;
    }
    const atlas_gwpolicy *gw = &ctx->gwpolicy;
    if (gw->state != ATLAS_GWPOLICY_ENABLED) {
        return false;
    }
    bool is_confirm_group = strcmp(method_name, "deploy.remote_challenge") == 0 ||
                            strcmp(method_name, "deploy.remote_confirm") == 0;
    if (is_confirm_group) {
        return deploy_key_pool_ready(gw);
    }
    return atlas_server_remote_submit_policy_ready(gw);
}

static atlas_status require_remote_deploy_method(dispatch_state *ds, const char *method_name,
                                                  atlas_err *err) {
    if (!atlas_server_remote_deploy_method_offered(ds->ctx, (long long)ds->peer_uid,
                                                   method_name)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "this connection may not manage deploys");
    }
    return ATLAS_OK;
}

/* --- credential verification -------------------------------------------------
 *
 * Three shapes, all built over `atlas_orch_remote_verify`: submit-pool only
 * (propose, cancel), deploy-key only (challenge, confirm), and the union of
 * both (get, list) -- reused, never copied, per this file's own header.
 */

static atlas_status take_token(const atlas_ipc_request *req, atlas_buf *out, atlas_err *err) {
    const char *v = NULL;
    if (!atlas_ipc_param_str(req, "token", &v) || v == NULL || v[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "\"token\" is required");
    }
    return atlas_buf_set_str(out, v, err);
}

static atlas_status verify_submit_credential(dispatch_state *ds, const atlas_ipc_request *req,
                                             char key_id_out[ATLAS_APIKEY_SELECTOR_HEX + 1u],
                                             atlas_err *err) {
    key_id_out[0] = '\0';
    atlas_buf tok = ATLAS_BUF_INIT;
    atlas_status st = take_token(req, &tok, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&tok);
        return st;
    }
    const atlas_gwpolicy *gw = &ds->ctx->gwpolicy;
    st = atlas_orch_remote_verify(
        ds->db, &tok,
        (const char (*)[ATLAS_APIKEY_SELECTOR_HEX + 1u])gw->remote_submit_keys,
        gw->remote_submit_count, key_id_out, err);
    atlas_buf_free(&tok);
    return st;
}

/* The deploy credential follows A16's dispose rule, not A14's submit rule:
 * it must hold NO stored scope at all, checked here rather than inside
 * `atlas_orch_remote_verify` (which deliberately does not check `mask`, per
 * A14's own Decision 1 -- a submit credential may hold other scopes, since
 * `jobs:submit` is additive). `atlas_decision_remote_verify`
 * (`src/decision/remote.c`) is the precedent this mirrors: "the remote
 * disposal credential must hold no stored scope." `atlas_orch_remote_verify`
 * itself has no record to hand back, only a resolved `key_id`, so the mask
 * is read with a second, ordinary lookup on the id it just proved is real --
 * the same shape `gateway.auth` and `atlas_decision_remote_verify` both use.
 * `server_internal.h`'s own comment on this function states the identical
 * rule, so the two sites cannot drift apart. */
static atlas_status verify_deploy_credential(dispatch_state *ds, const atlas_ipc_request *req,
                                             char key_id_out[ATLAS_APIKEY_SELECTOR_HEX + 1u],
                                             atlas_err *err) {
    key_id_out[0] = '\0';
    atlas_buf tok = ATLAS_BUF_INIT;
    atlas_status st = take_token(req, &tok, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&tok);
        return st;
    }
    const atlas_gwpolicy *gw = &ds->ctx->gwpolicy;
    char allowed[1][ATLAS_APIKEY_SELECTOR_HEX + 1u];
    size_t allowed_count = 0;
    if (gw->remote_deploy_key[0] != '\0') {
        memcpy(allowed[0], gw->remote_deploy_key, sizeof allowed[0]);
        allowed_count = 1u;
    }
    st = atlas_orch_remote_verify(
        ds->db, &tok, (const char (*)[ATLAS_APIKEY_SELECTOR_HEX + 1u])allowed, allowed_count,
        key_id_out, err);
    atlas_buf_free(&tok);
    if (st != ATLAS_OK) {
        return st;
    }
    atlas_apikey_record rec;
    memset(&rec, 0, sizeof rec);
    bool found = false;
    st = atlas_db_apikey_lookup(ds->db, key_id_out, &rec, &found, err);
    if (st != ATLAS_OK) {
        key_id_out[0] = '\0';
        memset(&rec, 0, sizeof rec);
        return st;
    }
    if (!found || rec.mask != 0u) {
        st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                           "the deploy credential holds no other power, and %s holds %s",
                           key_id_out, found ? rec.scopes : "an unresolved mask");
        key_id_out[0] = '\0';
        memset(&rec, 0, sizeof rec);
        return st;
    }
    memset(&rec, 0, sizeof rec);
    return ATLAS_OK;
}

/* `is_deploy_credential` reports which pool the verified key actually came
 * from, so a caller can decide how a read is scoped: the union verify below
 * accepts either pool, and the two must be told apart afterwards. */
static atlas_status verify_submit_or_deploy_credential(
    dispatch_state *ds, const atlas_ipc_request *req,
    char key_id_out[ATLAS_APIKEY_SELECTOR_HEX + 1u], bool *is_deploy_credential, atlas_err *err) {
    key_id_out[0] = '\0';
    *is_deploy_credential = false;
    atlas_buf tok = ATLAS_BUF_INIT;
    atlas_status st = take_token(req, &tok, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&tok);
        return st;
    }
    const atlas_gwpolicy *gw = &ds->ctx->gwpolicy;
    char allowed[ATLAS_GWPOLICY_MAX_SUBMIT_KEYS + 1u][ATLAS_APIKEY_SELECTOR_HEX + 1u];
    size_t n = 0;
    for (size_t i = 0; i < gw->remote_submit_count; i++) {
        memcpy(allowed[n], gw->remote_submit_keys[i], sizeof allowed[n]);
        n++;
    }
    if (gw->remote_deploy_key[0] != '\0') {
        memcpy(allowed[n], gw->remote_deploy_key, sizeof allowed[n]);
        n++;
    }
    st = atlas_orch_remote_verify(ds->db, &tok,
                                  (const char (*)[ATLAS_APIKEY_SELECTOR_HEX + 1u])allowed, n,
                                  key_id_out, err);
    atlas_buf_free(&tok);
    if (st == ATLAS_OK && gw->remote_deploy_key[0] != '\0' &&
        strcmp(key_id_out, gw->remote_deploy_key) == 0) {
        *is_deploy_credential = true;
    }
    return st;
}

/* --- the in-process, single-thread challenge store ---------------------------
 *
 * See the file header for why this is neither a table nor guarded by a
 * mutex. Bounded at ATLAS_DEPLOY_CHALLENGE_MAX outstanding challenges --
 * generous for "at most one CONFIRMED-or-about-to-be deploy per registered
 * repository" -- with the oldest expired-or-consumed slot reclaimed on
 * mint before a fresh one is ever refused for being full.
 */
#define ATLAS_DEPLOY_CHALLENGE_HEX 32u
#define ATLAS_DEPLOY_CHALLENGE_MAX 32u

typedef struct deploy_challenge_slot {
    bool in_use;
    bool consumed;
    char token[ATLAS_DEPLOY_CHALLENGE_HEX + 1u];
    char deploy_uid[ATLAS_DEPLOY_UID_MAX];
    char patch_digest[ATLAS_SHA256_HEX_LEN + 1u];
    int64_t expires_at_ms;
} deploy_challenge_slot;

static deploy_challenge_slot g_deploy_challenges[ATLAS_DEPLOY_CHALLENGE_MAX];

static int64_t wall_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

static void challenge_reap_expired_locked(int64_t now_ms) {
    for (size_t i = 0; i < ATLAS_DEPLOY_CHALLENGE_MAX; i++) {
        deploy_challenge_slot *s = &g_deploy_challenges[i];
        if (s->in_use && (s->consumed || s->expires_at_ms <= now_ms)) {
            memset(s, 0, sizeof *s);
        }
    }
}

/* A second mint for a deploy already holding a live slot supersedes the
 * first, exactly as A16's own one-live-challenge-per-record semantics do:
 * Mission Control re-opening a confirm dialog (or re-rendering one) must
 * never be able to exhaust the table, since at most one deploy per
 * repository is ever PROPOSED at a time and this reap keeps the array at
 * one slot per live deploy rather than one slot per mint attempt. */
static void challenge_reap_same_deploy_locked(const char *deploy_uid) {
    for (size_t i = 0; i < ATLAS_DEPLOY_CHALLENGE_MAX; i++) {
        deploy_challenge_slot *s = &g_deploy_challenges[i];
        if (s->in_use && strcmp(s->deploy_uid, deploy_uid) == 0) {
            memset(s, 0, sizeof *s);
        }
    }
}

static atlas_status challenge_mint(const char *deploy_uid, const char *patch_digest,
                                   char token_out[ATLAS_DEPLOY_CHALLENGE_HEX + 1u],
                                   int64_t *expires_at_ms_out, atlas_err *err) {
    unsigned char raw[ATLAS_DEPLOY_CHALLENGE_HEX / 2u];
    atlas_status st = atlas_random_bytes(raw, sizeof raw, err);
    if (st != ATLAS_OK) {
        return st;
    }
    atlas_hex_encode(raw, sizeof raw, token_out);

    int64_t now = wall_now_ms();
    challenge_reap_expired_locked(now);
    challenge_reap_same_deploy_locked(deploy_uid);

    size_t slot = ATLAS_DEPLOY_CHALLENGE_MAX;
    for (size_t i = 0; i < ATLAS_DEPLOY_CHALLENGE_MAX; i++) {
        if (!g_deploy_challenges[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot == ATLAS_DEPLOY_CHALLENGE_MAX) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "too many outstanding deploy challenges; try again shortly");
    }
    deploy_challenge_slot *s = &g_deploy_challenges[slot];
    memset(s, 0, sizeof *s);
    s->in_use = true;
    (void)snprintf(s->token, sizeof s->token, "%s", token_out);
    (void)snprintf(s->deploy_uid, sizeof s->deploy_uid, "%s", deploy_uid);
    (void)snprintf(s->patch_digest, sizeof s->patch_digest, "%s", patch_digest);
    s->expires_at_ms = now + ATLAS_DECISION_CHALLENGE_TTL_MS;
    *expires_at_ms_out = s->expires_at_ms;
    return ATLAS_OK;
}

/* Verifies (never consumes) a presented challenge against a deploy uid and
 * its current patch digest. Returns ATLAS_OK with `*ok` true only when a
 * matching, unconsumed, unexpired slot exists.
 *
 * `challenge_check` here and `challenge_consume` below both run outside any
 * database transaction -- deliberately, since neither touches the database at
 * all -- resting entirely on two facts stated once, here, rather than at
 * every call site: (1) the daemon's serve loop (`atlas_server_serve`,
 * `src/ipc/server.c`) dispatches one client request at a time, start to
 * finish, including the synchronous wait on a writer job (A9.2.6), so no
 * second `deploy.remote_confirm` or `deploy.remote_challenge` can run its own
 * check-then-consume between this function's check and its caller's later
 * `challenge_consume` -- there is no other thread that could interleave one;
 * and (2) `<data-dir>/atlas.lock` (invariant 11) guarantees this is the only
 * `atlasd` process holding this index at all, so no sibling process could be
 * racing this one. Together they are why an in-process array with no mutex of
 * its own is a correct capability store rather than merely a convenient one:
 * revoking or spending a challenge is exactly as safe here as any other
 * daemon-local state `dispatch_state` itself holds. */
static void challenge_check(const char *token, const char *deploy_uid, const char *patch_digest,
                            bool *ok, size_t *slot_out) {
    *ok = false;
    *slot_out = ATLAS_DEPLOY_CHALLENGE_MAX;
    int64_t now = wall_now_ms();
    for (size_t i = 0; i < ATLAS_DEPLOY_CHALLENGE_MAX; i++) {
        deploy_challenge_slot *s = &g_deploy_challenges[i];
        if (!s->in_use || s->consumed || s->expires_at_ms <= now) {
            continue;
        }
        if (strcmp(s->token, token) == 0 && strcmp(s->deploy_uid, deploy_uid) == 0 &&
            strcmp(s->patch_digest, patch_digest) == 0) {
            *ok = true;
            *slot_out = i;
            return;
        }
    }
}

/* Marks one challenge slot spent. Safe with no lock of its own for the
 * identical reason `challenge_check` above states in full: the serve loop's
 * one-request-at-a-time dispatch plus the data-directory lock rule out any
 * interleaving confirm or revoke, in-process or from a second daemon. Called
 * only after `atlas_writer_deploy` has returned `ATLAS_OK` for the confirm
 * this slot authorised -- see `method_remote_deploy_confirm` below. */
static void challenge_consume(size_t slot) {
    if (slot < ATLAS_DEPLOY_CHALLENGE_MAX) {
        g_deploy_challenges[slot].consumed = true;
    }
}

/* --- deploy.remote_propose --------------------------------------------------- */

/* Refuses a request naming a field the policy, not the request, decides --
 * A14's Decision 4, extended by one field. `dry_run` lives in the root
 * agent's own conf (D.4), never in a proposal: a caller who believes they
 * configured a dry run and silently got a real one is worse off than one who
 * was told. */
static atlas_status refuse_if_present(const atlas_ipc_request *req, const char *name,
                                      atlas_err *err) {
    const char *v = NULL;
    int64_t vi = 0;
    bool vb = false;
    const atlas_ipc_array *va = NULL;
    if (atlas_ipc_param_str(req, name, &v) || atlas_ipc_param_int(req, name, &vi) ||
        atlas_ipc_param_bool(req, name, &vb) || atlas_ipc_param_array(req, name, &va)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a deploy proposal names only the job; %s is decided by the root "
                             "agent's own configuration",
                             name);
    }
    return ATLAS_OK;
}

static atlas_status write_deploy_row_summary(dispatch_state *ds, const atlas_deploy_row *row,
                                             atlas_err *err) {
    atlas_status st = atlas_json_key_str(ds->j, "deploy", atlas_buf_cstr(&row->deploy_uid), err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "job", atlas_buf_cstr(&row->job_uid), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "state", atlas_deploy_state_name(row->state), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "base_commit", atlas_buf_cstr(&row->base_commit), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "patch_sha256", atlas_buf_cstr(&row->patch_digest), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "patch_bytes", row->patch_bytes, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "created_at", atlas_buf_cstr(&row->created_at), err);
    }
    return st;
}

static atlas_status method_remote_deploy_propose(dispatch_state *ds, const atlas_ipc_request *req,
                                                 atlas_err *err) {
    atlas_status st = require_remote_deploy_method(ds, "deploy.remote_propose", err);
    if (st != ATLAS_OK) {
        return st;
    }
    static const char *const FORBIDDEN[] = {"dry_run", NULL};
    for (size_t i = 0; FORBIDDEN[i] != NULL; i++) {
        st = refuse_if_present(req, FORBIDDEN[i], err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    const char *job_uid = NULL;
    if (!atlas_ipc_param_str(req, "job", &job_uid) || job_uid == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a deploy proposal needs a \"job\"");
    }

    char key_id[ATLAS_APIKEY_SELECTOR_HEX + 1u];
    st = verify_submit_credential(ds, req, key_id, err);
    if (st != ATLAS_OK) {
        return st;
    }

    atlas_deploy_op op;
    atlas_deploy_op_init(&op);
    op.kind = ATLAS_DEPLOY_OP_PROPOSE;
    st = atlas_buf_set_str(&op.job_uid, job_uid, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op.key_id, key_id, err);
    }
    atlas_deploy_op_result out;
    atlas_deploy_op_result_init(&out);
    if (st == ATLAS_OK) {
        st = atlas_writer_deploy(ds->ctx->writer, &op, &out, err);
    }
    atlas_deploy_op_free(&op);

    if (st == ATLAS_OK) {
        atlas_deploy_row row;
        atlas_deploy_row_init(&row);
        bool have = false;
        st = atlas_db_deploy_get(ds->db, atlas_buf_cstr(&out.deploy_uid), &row, &have, err);
        if (st == ATLAS_OK && have) {
            st = write_deploy_row_summary(ds, &row, err);
        }
        atlas_deploy_row_free(&row);
    }
    atlas_deploy_op_result_free(&out);
    return st;
}

/* --- deploy.remote_get -------------------------------------------------------- */

static atlas_status iso8601_age_seconds(const char *ts, int64_t *out) {
    struct tm tm_utc;
    memset(&tm_utc, 0, sizeof tm_utc);
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (sscanf(ts, "%4d-%2d-%2dT%2d:%2d:%2dZ", &y, &mo, &d, &h, &mi, &se) != 6) {
        return ATLAS_ERR_USAGE;
    }
    tm_utc.tm_year = y - 1900;
    tm_utc.tm_mon = mo - 1;
    tm_utc.tm_mday = d;
    tm_utc.tm_hour = h;
    tm_utc.tm_min = mi;
    tm_utc.tm_sec = se;
    time_t then = timegm(&tm_utc);
    if (then == (time_t)-1) {
        return ATLAS_ERR_USAGE;
    }
    time_t now = time(NULL);
    int64_t age = (int64_t)now - (int64_t)then;
    *out = age > 0 ? age : 0;
    return ATLAS_OK;
}

static atlas_status write_deploy_get_result(dispatch_state *ds, const atlas_deploy_row *row,
                                            atlas_err *err) {
    atlas_status st = write_deploy_row_summary(ds, row, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "proposed_key_id", atlas_buf_cstr(&row->proposed_key_id),
                                err);
    }
    if (st == ATLAS_OK && row->confirmed_key_id.len > 0) {
        st = atlas_json_key_str(ds->j, "confirmed_key_id",
                                atlas_buf_cstr(&row->confirmed_key_id), err);
    }
    if (st == ATLAS_OK && row->confirmed_at.len > 0) {
        st = atlas_json_key_str(ds->j, "confirmed_at", atlas_buf_cstr(&row->confirmed_at), err);
    }
    if (st == ATLAS_OK && row->spooled_at.len > 0) {
        st = atlas_json_key_str(ds->j, "spooled_at", atlas_buf_cstr(&row->spooled_at), err);
    }
    if (st == ATLAS_OK && row->terminal_at.len > 0) {
        st = atlas_json_key_str(ds->j, "terminal_at", atlas_buf_cstr(&row->terminal_at), err);
    }
    if (st == ATLAS_OK && row->state == ATLAS_DEPLOY_CONFIRMED && row->terminal_at.len == 0 &&
        row->confirmed_at.len > 0) {
        int64_t age = 0;
        if (iso8601_age_seconds(atlas_buf_cstr(&row->confirmed_at), &age) == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "age_seconds", age, err);
        }
    }
    if (st == ATLAS_OK && atlas_deploy_state_is_terminal(row->state)) {
        st = atlas_json_key(ds->j, "result", err);
        if (st == ATLAS_OK) {
            st = atlas_json_obj_begin(ds->j, err);
        }
        /* Every string below came from a `.res` file a foreign process
         * (the root agent, on the target host) wrote -- exactly as
         * untrusted as any other value read raw from outside Atlas'
         * own process, and safe-encoded at the point of output per
         * CLAUDE.md's "Untrusted text" rule, the same way `result_text`
         * already was. `dry_run` is a bool and needs none. */
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "stage", atlas_safe(&ds->safe, atlas_buf_cstr(&row->result_stage)),
                                    err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_bool(ds->j, "dry_run", row->result_dry_run, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "rollback",
                                    atlas_safe(&ds->safe, atlas_buf_cstr(&row->result_rollback)), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(
                ds->j, "head_before",
                atlas_safe(&ds->safe, atlas_buf_cstr(&row->result_head_before)), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(
                ds->j, "head_after", atlas_safe(&ds->safe, atlas_buf_cstr(&row->result_head_after)),
                err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(
                ds->j, "installed_version",
                atlas_safe(&ds->safe, atlas_buf_cstr(&row->result_version)), err);
        }
        if (st == ATLAS_OK) {
            /* The root agent's own prose: UNTRUSTED_DATA, safe-encoded on the
             * way out like every other value of this shape. */
            st = atlas_json_key_str(ds->j, "text",
                                    atlas_safe(&ds->safe, atlas_buf_cstr(&row->result_text)), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_obj_end(ds->j, err);
        }
    }
    return st;
}

static atlas_status method_remote_deploy_get(dispatch_state *ds, const atlas_ipc_request *req,
                                             atlas_err *err) {
    atlas_status st = require_remote_deploy_method(ds, "deploy.remote_get", err);
    if (st != ATLAS_OK) {
        return st;
    }
    const char *uid = NULL;
    if (!atlas_ipc_param_str(req, "deploy", &uid) || uid == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "which deploy?");
    }
    char key_id[ATLAS_APIKEY_SELECTOR_HEX + 1u];
    bool is_deploy_cred = false;
    st = verify_submit_or_deploy_credential(ds, req, key_id, &is_deploy_cred, err);
    if (st != ATLAS_OK) {
        return st;
    }

    atlas_deploy_row row;
    atlas_deploy_row_init(&row);
    bool have = false;
    st = atlas_db_deploy_get(ds->db, uid, &row, &have, err);
    /* Scope: the deploy credential is this channel's operator proxy and sees
     * every deploy (it must, to review one nobody has confirmed yet, before
     * `confirmed_key_id` names it); a submit credential sees only what it
     * itself proposed. */
    bool visible = have && (is_deploy_cred || strcmp(atlas_buf_cstr(&row.proposed_key_id), key_id) == 0);
    if (st == ATLAS_OK && !visible) {
        atlas_deploy_row_free(&row);
        return atlas_err_set(err, ATLAS_ERR_USAGE, "no such deploy");
    }
    if (st != ATLAS_OK) {
        atlas_deploy_row_free(&row);
        return st;
    }
    st = write_deploy_get_result(ds, &row, err);
    atlas_deploy_row_free(&row);
    return st;
}

/* --- deploy.remote_list ------------------------------------------------------- */

typedef struct deploy_list_ctx {
    dispatch_state *ds;
    atlas_err *err;
} deploy_list_ctx;

static atlas_status emit_deploy_list_row(const atlas_deploy_list_row *row, void *ud,
                                         atlas_err *err) {
    deploy_list_ctx *lc = (deploy_list_ctx *)ud;
    atlas_status st = atlas_json_obj_begin(lc->ds->j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(lc->ds->j, "deploy", row->deploy_uid, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(lc->ds->j, "job", row->job_uid, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(lc->ds->j, "state", atlas_deploy_state_name(row->state), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(lc->ds->j, "repo_id", row->repo_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(lc->ds->j, "proposed_key_id", row->proposed_key_id, err);
    }
    if (st == ATLAS_OK && row->confirmed_key_id[0] != '\0') {
        st = atlas_json_key_str(lc->ds->j, "confirmed_key_id", row->confirmed_key_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(lc->ds->j, "created_at", row->created_at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(lc->ds->j, err);
    }
    return st;
}

static atlas_status method_remote_deploy_list(dispatch_state *ds, const atlas_ipc_request *req,
                                              atlas_err *err) {
    atlas_status st = require_remote_deploy_method(ds, "deploy.remote_list", err);
    if (st != ATLAS_OK) {
        return st;
    }
    char key_id[ATLAS_APIKEY_SELECTOR_HEX + 1u];
    bool is_deploy_cred = false;
    st = verify_submit_or_deploy_credential(ds, req, key_id, &is_deploy_cred, err);
    if (st != ATLAS_OK) {
        return st;
    }

    int64_t cursor = 0, limit = 0;
    (void)atlas_ipc_param_int(req, "cursor", &cursor);
    (void)atlas_ipc_param_int(req, "limit", &limit);
    if (cursor < 0) {
        cursor = 0;
    }

    /* The deploy credential is this channel's operator proxy (see
     * `method_remote_deploy_get`'s own comment): it lists every deploy, not
     * only ones it happens to have confirmed already, or a PROPOSED deploy
     * nobody has reviewed yet would never appear in its own list. A submit
     * credential is scoped to what it proposed, `job.remote_list`'s own
     * per-credential isolation. */
    const char *scope_key = is_deploy_cred ? NULL : key_id;

    st = atlas_json_key(ds->j, "deploys", err);
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    deploy_list_ctx lc = {ds, err};
    int64_t count = 0, next_cursor = cursor;
    bool more = false;
    if (st == ATLAS_OK) {
        st = atlas_db_deploy_list(ds->db, scope_key, cursor, limit, emit_deploy_list_row, &lc,
                                  &count, &next_cursor, &more, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "count", count, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "cursor", next_cursor, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "more", more, err);
    }
    return st;
}

/* --- deploy.remote_cancel ------------------------------------------------------ */

static atlas_status method_remote_deploy_cancel(dispatch_state *ds, const atlas_ipc_request *req,
                                                atlas_err *err) {
    atlas_status st = require_remote_deploy_method(ds, "deploy.remote_cancel", err);
    if (st != ATLAS_OK) {
        return st;
    }
    const char *uid = NULL;
    if (!atlas_ipc_param_str(req, "deploy", &uid) || uid == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "which deploy?");
    }
    char key_id[ATLAS_APIKEY_SELECTOR_HEX + 1u];
    st = verify_submit_credential(ds, req, key_id, err);
    if (st != ATLAS_OK) {
        return st;
    }

    atlas_deploy_op op;
    atlas_deploy_op_init(&op);
    op.kind = ATLAS_DEPLOY_OP_CANCEL;
    st = atlas_buf_set_str(&op.deploy_uid, uid, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op.key_id, key_id, err);
    }
    atlas_deploy_op_result out;
    atlas_deploy_op_result_init(&out);
    if (st == ATLAS_OK) {
        st = atlas_writer_deploy(ds->ctx->writer, &op, &out, err);
    }
    atlas_deploy_op_free(&op);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "deploy", atlas_buf_cstr(&out.deploy_uid), err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "state",
                                    atlas_deploy_state_name(ATLAS_DEPLOY_CANCELLED), err);
        }
    }
    atlas_deploy_op_result_free(&out);
    return st;
}

/* --- deploy.remote_challenge ---------------------------------------------------
 *
 * A16's mint step, applied to a patch digest instead of a decision content
 * hash: the response carries the full detail an operator's confirmation
 * screen needs (job, base commit, the complete digest, the byte count) so
 * Mission Control can render "the channel's weakness sentence" beside it,
 * exactly as `docs/browser-disposal.md` documents for the sibling flow --
 * this route composes it once here rather than duplicating it a third
 * place. */
static atlas_status method_remote_deploy_challenge(dispatch_state *ds,
                                                    const atlas_ipc_request *req, atlas_err *err) {
    atlas_status st = require_remote_deploy_method(ds, "deploy.remote_challenge", err);
    if (st != ATLAS_OK) {
        return st;
    }
    const char *uid = NULL;
    if (!atlas_ipc_param_str(req, "deploy", &uid) || uid == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "which deploy?");
    }
    char key_id[ATLAS_APIKEY_SELECTOR_HEX + 1u];
    st = verify_deploy_credential(ds, req, key_id, err);
    if (st != ATLAS_OK) {
        return st;
    }

    atlas_deploy_row row;
    atlas_deploy_row_init(&row);
    bool have = false;
    st = atlas_db_deploy_get(ds->db, uid, &row, &have, err);
    if (st == ATLAS_OK && (!have || row.state != ATLAS_DEPLOY_PROPOSED)) {
        atlas_deploy_row_free(&row);
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "no PROPOSED deploy with that id; a challenge can only be issued for "
                             "one awaiting confirmation");
    }
    if (st != ATLAS_OK) {
        atlas_deploy_row_free(&row);
        return st;
    }

    char token[ATLAS_DEPLOY_CHALLENGE_HEX + 1u];
    int64_t expires_at_ms = 0;
    st = challenge_mint(atlas_buf_cstr(&row.deploy_uid), atlas_buf_cstr(&row.patch_digest), token,
                        &expires_at_ms, err);
    if (st == ATLAS_OK) {
        st = write_deploy_row_summary(ds, &row, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "challenge", token, err);
    }
    if (st == ATLAS_OK) {
        char expires_at[ATLAS_TS_MAX];
        atlas_iso8601_after_now(expires_at, sizeof expires_at,
                                expires_at_ms - wall_now_ms() > 0 ? expires_at_ms - wall_now_ms()
                                                                   : 0);
        st = atlas_json_key_str(ds->j, "expires_at", expires_at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "key_id", key_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(
            ds->j, "actor_means",
            "an explicit action arrived through Atlas' remote deploy channel: the credential "
            "named in key_id was presented over the gateway's listener, under whatever transport "
            "security that listener has, which Atlas does not verify. This does not identify a "
            "person, does not prove a person was present, and is not a signature. It is weaker "
            "than the local channel by construction: the credential passed through a "
            "network-facing process.",
            err);
    }
    atlas_deploy_row_free(&row);
    return st;
}

/* --- deploy.remote_confirm ------------------------------------------------------ */

static atlas_status method_remote_deploy_confirm(dispatch_state *ds,
                                                  const atlas_ipc_request *req, atlas_err *err) {
    atlas_status st = require_remote_deploy_method(ds, "deploy.remote_confirm", err);
    if (st != ATLAS_OK) {
        return st;
    }
    const char *uid = NULL;
    if (!atlas_ipc_param_str(req, "deploy", &uid) || uid == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "which deploy?");
    }
    const char *challenge = NULL;
    if (!atlas_ipc_param_str(req, "challenge", &challenge) || challenge == NULL ||
        strlen(challenge) != ATLAS_DEPLOY_CHALLENGE_HEX) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "this operation needs a deploy challenge issued by "
                             "deploy.remote_challenge");
    }
    const char *confirmation = NULL;
    if (!atlas_ipc_param_str(req, "confirmation", &confirmation) || confirmation == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "this operation needs the confirmation shown at the prompt");
    }

    char key_id[ATLAS_APIKEY_SELECTOR_HEX + 1u];
    st = verify_deploy_credential(ds, req, key_id, err);
    if (st != ATLAS_OK) {
        return st;
    }

    /* The digest the challenge must still be bound to is read fresh, not
     * assumed from the mint: a deploy's `patch_digest` never changes after
     * PROPOSE, so this is a defence-in-depth re-read rather than a distinct
     * possibility, on `atlas_db_deploy_confirm_in_tx`'s own precedent of
     * re-checking what mint time already established. */
    atlas_deploy_row row;
    atlas_deploy_row_init(&row);
    bool have = false;
    st = atlas_db_deploy_get(ds->db, uid, &row, &have, err);
    if (st != ATLAS_OK) {
        atlas_deploy_row_free(&row);
        return st;
    }
    if (!have) {
        atlas_deploy_row_free(&row);
        return atlas_err_set(err, ATLAS_ERR_USAGE, "no such deploy");
    }

    bool challenge_ok = false;
    size_t slot = ATLAS_DEPLOY_CHALLENGE_MAX;
    challenge_check(challenge, atlas_buf_cstr(&row.deploy_uid), atlas_buf_cstr(&row.patch_digest),
                    &challenge_ok, &slot);
    atlas_deploy_row_free(&row);
    if (!challenge_ok) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "that is not a valid, unexpired deploy challenge for this deploy");
    }

    atlas_deploy_op op;
    atlas_deploy_op_init(&op);
    op.kind = ATLAS_DEPLOY_OP_CONFIRM;
    st = atlas_buf_set_str(&op.deploy_uid, uid, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op.key_id, key_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op.confirmation, confirmation, err);
    }
    atlas_deploy_op_result out;
    atlas_deploy_op_result_init(&out);
    if (st == ATLAS_OK) {
        st = atlas_writer_deploy(ds->ctx->writer, &op, &out, err);
    }
    atlas_deploy_op_free(&op);

    /* Consumed only once the write actually committed: a refused spend (a
     * job still active, a stale prefix, a state that already moved) leaves
     * the challenge spendable, so the operator is not forced to re-mint for
     * a condition that may already have cleared. */
    if (st == ATLAS_OK) {
        challenge_consume(slot);
        st = atlas_json_key_str(ds->j, "deploy", atlas_buf_cstr(&out.deploy_uid), err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "state",
                                    atlas_deploy_state_name(ATLAS_DEPLOY_CONFIRMED), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_bool(ds->j, "spooled", out.spooled, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "key_id", key_id, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(
                ds->j, "actor_means",
                "an explicit action arrived through Atlas' remote deploy channel: the credential "
                "named in key_id was presented over the gateway's listener, under whatever "
                "transport security that listener has, which Atlas does not verify. This does "
                "not identify a person, does not prove a person was present, and is not a "
                "signature. It is weaker than the local channel by construction: the credential "
                "passed through a network-facing process.",
                err);
        }
    }
    atlas_deploy_op_result_free(&out);
    return st;
}

/* --- the group ---------------------------------------------------------------- */

static const atlas_method_entry REMOTE_DEPLOY_METHODS[] = {
    {"deploy.remote_propose", method_remote_deploy_propose},
    {"deploy.remote_get", method_remote_deploy_get},
    {"deploy.remote_list", method_remote_deploy_list},
    {"deploy.remote_cancel", method_remote_deploy_cancel},
    {"deploy.remote_challenge", method_remote_deploy_challenge},
    {"deploy.remote_confirm", method_remote_deploy_confirm},
};

const atlas_method_entry *atlas_server_remote_deploy_methods(size_t *count_out) {
    if (count_out != NULL) {
        *count_out = sizeof(REMOTE_DEPLOY_METHODS) / sizeof(REMOTE_DEPLOY_METHODS[0]);
    }
    return REMOTE_DEPLOY_METHODS;
}
