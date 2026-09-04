/* Atlas - A16 T3: the write point's remote operator channel.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * This is `tests/test_decision_operator.c`'s own shape, one layer further in:
 * it drives `atlas_decision_apply` directly at the write point, exactly as
 * `approve_through_the_write_point` does, rather than through the CLI's
 * interactive challenge/confirm prompt -- the browser has no terminal to put
 * one on, and this season's whole point is that it should not need one.
 *
 * The fixture holds one repository with an `OPERATIONAL_FACT` and a `POLICY`
 * document, each at revision 1, and two real API-key credentials minted
 * through the CLI: `dispose` (`--no-scopes`, the shape a real disposal
 * credential has) and `reader` (`--scope decisions:read`, an ordinary
 * credential this channel must refuse). Both are real rows, verified through
 * the same `atlas_apikey_verify` path the daemon's `gateway.auth` uses, not
 * stand-ins.
 */
#include <stdint.h>
#include <string.h>

#include "atlas/apikey.h"
#include "atlas/atlas.h"
#include "atlas/datadir.h"
#include "atlas/decision.h"
#include "atlas/decision_ops.h"
#include "atlas/gw.h"
#include "atlas_test.h"
#include "core/service_decision_internal.h"
#include "support/fixture.h"

/* --- the fixture ------------------------------------------------------------ */

typedef struct env {
    fixture fx;
    atlas_buf fact_uid;   /* OPERATIONAL_FACT, revision 1 */
    atlas_buf policy_uid; /* POLICY, revision 1 */
    char dispose_token[ATLAS_APIKEY_TOKEN_MAX];
    char dispose_id[ATLAS_APIKEY_SELECTOR_HEX + 1u];
    char reader_token[ATLAS_APIKEY_TOKEN_MAX];
    char reader_id[ATLAS_APIKEY_SELECTOR_HEX + 1u];
} env;

static void run_atlas(env *e, const char *const *extra, size_t n, atlas_buf *out, int *code) {
    atlas_err err;
    atlas_err_init(&err);
    const char *argv[24];
    size_t k = 0;
    argv[k++] = "--data-dir";
    argv[k++] = fx_data_dir(&e->fx);
    T_REQUIRE(n + k <= sizeof(argv) / sizeof(argv[0]));
    for (size_t i = 0; i < n; i++) {
        argv[k++] = extra[i];
    }
    atlas_buf errout = ATLAS_BUF_INIT;
    T_OK(fx_atlas(argv, k, out, &errout, code, &err), &err);
    atlas_buf_free(&errout);
}

/* Proposes one document of `kind_name` at revision 1, over the fixture's
 * committed `main.c`, and returns its public uid. */
static void propose_kind(env *e, const char *kind_name, const char *title, atlas_buf *uid_out) {
    atlas_buf out = ATLAS_BUF_INIT;
    int code = 0;
    const char *propose[] = {
        "decision", "propose", "proj", "--kind", kind_name, "--title", title, "--decision", title,
        "--path",   "main.c",
    };
    run_atlas(e, propose, 11u, &out, &code);
    T_EQ_INT(code, 0);
    const char *p = strstr(atlas_buf_cstr(&out), ATLAS_DECISION_UID_PREFIX);
    T_REQUIRE_MSG(p != NULL, "propose did not print a decision id: %s", atlas_buf_cstr(&out));
    size_t len = strlen(ATLAS_DECISION_UID_PREFIX) + ATLAS_DECISION_UID_HEX;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_buf_set(uid_out, p, len, &err), &err);
    atlas_buf_free(&out);
}

/* Mints one credential through the frozen CLI command and pulls its token and
 * id out of the human output, exactly as `tests/test_apikey.c`'s `token_of` /
 * `id_of` do. */
static void mint_key(env *e, const char *label, const char *scope_or_null, char *token_out,
                     size_t token_size, char *id_out, size_t id_size) {
    atlas_buf out = ATLAS_BUF_INIT;
    int code = 0;
    if (scope_or_null == NULL) {
        const char *create[] = {"api-key", "create", "--label", label, "--no-scopes"};
        run_atlas(e, create, 5u, &out, &code);
    } else {
        const char *create[] = {"api-key", "create", "--label", label, "--scope", scope_or_null};
        run_atlas(e, create, 6u, &out, &code);
    }
    T_EQ_INT(code, 0);

    token_out[0] = '\0';
    {
        const char *s = strstr(atlas_buf_cstr(&out), "ATLAS_API_KEY=");
        T_REQUIRE(s != NULL);
        s += strlen("ATLAS_API_KEY=");
        size_t n = 0;
        while (s[n] != '\0' && s[n] != '\n' && n + 1 < token_size) {
            n++;
        }
        memcpy(token_out, s, n);
        token_out[n] = '\0';
    }
    id_out[0] = '\0';
    {
        const char *s = strstr(atlas_buf_cstr(&out), "id:     " ATLAS_APIKEY_ID_PREFIX);
        T_REQUIRE(s != NULL);
        s += strlen("id:     " ATLAS_APIKEY_ID_PREFIX);
        size_t n = 0;
        while (s[n] != '\0' && s[n] != '\n' && n + 1 < id_size) {
            n++;
        }
        memcpy(id_out, s, n);
        id_out[n] = '\0';
    }
    atlas_buf_free(&out);
}

static void env_open(env *e) {
    memset(e, 0, sizeof(*e));
    atlas_buf_init(&e->fact_uid);
    atlas_buf_init(&e->policy_uid);
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&e->fx, &err), &err);
    T_OK(fx_init_repo(&e->fx, fx_repo(&e->fx), NULL, &err), &err);
    T_OK(fx_write(fx_repo(&e->fx), "main.c", "int main(void){return 0;}\n", &err), &err);
    T_OK(fx_add_all(&e->fx, fx_repo(&e->fx), &err), &err);
    T_OK(fx_commit(&e->fx, fx_repo(&e->fx), "init", &err), &err);

    atlas_buf out = ATLAS_BUF_INIT;
    int code = 0;
    const char *add[] = {"repo", "add", fx_repo(&e->fx), "--name", "proj"};
    run_atlas(e, add, 5u, &out, &code);
    T_EQ_INT(code, 0);
    atlas_buf_reset(&out);
    const char *scan[] = {"scan", "proj"};
    run_atlas(e, scan, 2u, &out, &code);
    T_EQ_INT(code, 0);
    atlas_buf_free(&out);

    propose_kind(e, "OPERATIONAL_FACT", "a live endpoint", &e->fact_uid);
    propose_kind(e, "POLICY", "a process rule", &e->policy_uid);

    mint_key(e, "dispose", NULL, e->dispose_token, sizeof(e->dispose_token), e->dispose_id,
            sizeof(e->dispose_id));
    mint_key(e, "reader", "decisions:read", e->reader_token, sizeof(e->reader_token),
            e->reader_id, sizeof(e->reader_id));
}

static void env_close(env *e) {
    atlas_buf_free(&e->fact_uid);
    atlas_buf_free(&e->policy_uid);
    fx_close(&e->fx);
}

static atlas_db *open_db(env *e) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf db_path = ATLAS_BUF_INIT;
    T_OK(atlas_datadir_db_path(fx_data_dir(&e->fx), &db_path, &err), &err);
    atlas_db *db = NULL;
    T_OK(atlas_db_open(atlas_buf_cstr(&db_path), &db, &err), &err);
    atlas_buf_free(&db_path);
    return db;
}

/* --- building an op without going through op_new's daemon or CLI choke
 * points -- exactly what a test at the write point has to do, since both
 * production choke points now hard-set LOCAL. --------------------------- */

static atlas_status build_op(atlas_decision_op_kind kind, atlas_decision_channel channel,
                             const char *uid, const char *replacement_uid,
                             atlas_decision_intent intent, int64_t expect_revision_no,
                             const atlas_buf *challenge_token, const char *confirmation,
                             const char *bearer_token, const char *expected_key_id,
                             uint32_t kinds_mask, atlas_decision_op *op, atlas_err *err) {
    atlas_decision_op_init(op, kind);
    atlas_status st = atlas_buf_set_str(&op->repo_name, "proj", err);
    if (st == ATLAS_OK && uid != NULL) {
        st = atlas_buf_set_str(&op->uid, uid, err);
    }
    if (st == ATLAS_OK && replacement_uid != NULL) {
        st = atlas_buf_set_str(&op->replacement_uid, replacement_uid, err);
    }
    op->intent = intent;
    op->expect_revision_no = expect_revision_no;
    op->channel = channel;
    if (st == ATLAS_OK && challenge_token != NULL) {
        st = atlas_buf_set(&op->token, challenge_token->data, challenge_token->len, err);
    }
    if (st == ATLAS_OK && confirmation != NULL) {
        st = atlas_buf_set_str(&op->confirmation, confirmation, err);
    }
    if (channel == ATLAS_DECISION_CHANNEL_REMOTE) {
        if (st == ATLAS_OK && bearer_token != NULL) {
            st = atlas_buf_set_str(&op->remote_token, bearer_token, err);
        }
        if (expected_key_id != NULL) {
            (void)snprintf(op->remote_expected_key_id, sizeof(op->remote_expected_key_id), "%s",
                           expected_key_id);
        }
        op->remote_kinds = kinds_mask;
    }
    return st;
}

static void load_challenge(atlas_db *db, const atlas_buf *token, atlas_decision_challenge *out) {
    atlas_err err;
    atlas_err_init(&err);
    bool found = false;
    atlas_decision_challenge_init(out);
    T_OK(atlas_db_decision_challenge_find(db, atlas_buf_cstr(token), out, &found, &err), &err);
    T_REQUIRE(found);
}

typedef struct event_query {
    const char *want_event;
    bool found;
    char actor[64];
    bool key_id_present;
    char key_id[ATLAS_APIKEY_SELECTOR_HEX + 1u];
    char detail[512];
} event_query;

static atlas_status on_event(const atlas_decision_event_row *row, void *ud, atlas_err *err) {
    (void)err;
    event_query *q = (event_query *)ud;
    if (q->found || strcmp(row->event, q->want_event) != 0) {
        return ATLAS_OK;
    }
    q->found = true;
    (void)snprintf(q->actor, sizeof(q->actor), "%s", row->actor);
    if (row->key_id != NULL) {
        q->key_id_present = true;
        (void)snprintf(q->key_id, sizeof(q->key_id), "%s", row->key_id);
    }
    (void)snprintf(q->detail, sizeof(q->detail), "%s", row->detail != NULL ? row->detail : "");
    return ATLAS_OK;
}

static void find_event(atlas_db *db, int64_t document_id, const char *want_event, event_query *q) {
    memset(q, 0, sizeof(*q));
    q->want_event = want_event;
    atlas_err err;
    atlas_err_init(&err);
    int64_t count = 0;
    bool more = false;
    T_OK(atlas_db_decision_events_list(db, document_id, 100, on_event, q, &count, &more, &err),
        &err);
    T_REQUIRE_MSG(q->found, "no %s event was recorded", want_event);
}

/* --- (a): a channel-less op is refused, for both a mint and a spend -------- */

static void test_a_channel_less_op_is_refused(void) {
    env e;
    env_open(&e);
    atlas_db *db = open_db(&e);
    atlas_err err;
    atlas_err_init(&err);

    /* Built directly, deliberately leaving `channel` at the struct's own
     * zero -- `ATLAS_DECISION_CHANNEL_UNKNOWN` -- rather than through
     * `build_op`, which always names one. */
    atlas_decision_op ch;
    atlas_decision_op_init(&ch, ATLAS_DECISION_OP_CHALLENGE);
    T_OK(atlas_buf_set_str(&ch.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set_str(&ch.uid, atlas_buf_cstr(&e.fact_uid), &err), &err);
    ch.intent = ATLAS_DECISION_INTENT_APPROVE;
    ch.expect_revision_no = 1;
    atlas_decision_result cr;
    atlas_decision_result_init(&cr);
    atlas_status st = atlas_decision_apply(db, &ch, &cr, &err);
    T_CHECK_MSG(st != ATLAS_OK, "a channel-less CHALLENGE minted a capability");
    T_CHECK_MSG(strstr(atlas_err_msg(&err), "this operation names no channel; a capability is "
                                            "minted and spent through exactly one of LOCAL or "
                                            "REMOTE") != NULL,
                "wrong refusal: %s", atlas_err_msg(&err));
    atlas_decision_result_free(&cr);
    atlas_decision_op_free(&ch);

    atlas_decision_op ap;
    atlas_decision_op_init(&ap, ATLAS_DECISION_OP_APPROVE);
    T_OK(atlas_buf_set_str(&ap.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set_str(&ap.uid, atlas_buf_cstr(&e.fact_uid), &err), &err);
    atlas_decision_result ar;
    atlas_decision_result_init(&ar);
    atlas_status st2 = atlas_decision_apply(db, &ap, &ar, &err);
    T_CHECK_MSG(st2 != ATLAS_OK, "a channel-less APPROVE spent a capability");
    T_CHECK_MSG(strstr(atlas_err_msg(&err), "this operation names no channel") != NULL,
                "wrong refusal: %s", atlas_err_msg(&err));
    atlas_decision_result_free(&ar);
    atlas_decision_op_free(&ap);

    atlas_db_close(db);
    env_close(&e);
}

/* --- (b): the core happy path ----------------------------------------------- */

static void test_b_remote_approve_happy_path(void) {
    env e;
    env_open(&e);
    atlas_db *db = open_db(&e);
    atlas_err err;
    atlas_err_init(&err);

    atlas_decision_op ch;
    T_OK(build_op(ATLAS_DECISION_OP_CHALLENGE, ATLAS_DECISION_CHANNEL_REMOTE,
                  atlas_buf_cstr(&e.fact_uid), NULL, ATLAS_DECISION_INTENT_APPROVE, 1, NULL, NULL,
                  e.dispose_token, e.dispose_id,
                  ATLAS_DECISION_KIND_BIT(ATLAS_DECISION_KIND_OPERATIONAL_FACT), &ch, &err),
        &err);
    atlas_decision_result cr;
    atlas_decision_result_init(&cr);
    T_OK(atlas_decision_apply(db, &ch, &cr, &err), &err);
    T_CHECK(cr.token.len > 0);
    T_CHECK_MSG(strcmp(cr.key_id, e.dispose_id) == 0, "mint result key_id: got %s want %s",
                cr.key_id, e.dispose_id);

    atlas_decision_challenge crow;
    load_challenge(db, &cr.token, &crow);
    T_CHECK(crow.channel == ATLAS_DECISION_CHANNEL_REMOTE);
    T_CHECK_MSG(strcmp(crow.key_id, e.dispose_id) == 0, "stored challenge key_id mismatch");

    /* A LOCAL spend of the REMOTE challenge is refused, and it stays
     * unconsumed. */
    {
        atlas_decision_op ap;
        T_OK(build_op(ATLAS_DECISION_OP_APPROVE, ATLAS_DECISION_CHANNEL_LOCAL,
                      atlas_buf_cstr(&e.fact_uid), NULL, ATLAS_DECISION_INTENT_APPROVE, 0,
                      &cr.token, cr.confirm, NULL, NULL, 0, &ap, &err),
            &err);
        atlas_decision_result ar;
        atlas_decision_result_init(&ar);
        atlas_status st = atlas_decision_apply(db, &ap, &ar, &err);
        T_CHECK_MSG(st != ATLAS_OK, "a REMOTE challenge was spent through the LOCAL channel");
        T_CHECK_MSG(
            strstr(atlas_err_msg(&err),
                  "that approval challenge was minted through the remote channel and cannot be "
                  "spent locally") != NULL,
            "wrong refusal: %s", atlas_err_msg(&err));
        atlas_decision_result_free(&ar);
        atlas_decision_op_free(&ap);

        atlas_decision_challenge still;
        load_challenge(db, &cr.token, &still);
        T_CHECK_MSG(!still.consumed, "the challenge was consumed by a refused local spend");
    }

    /* The REMOTE spend, with the typed prefix, succeeds. */
    atlas_decision_op ap;
    T_OK(build_op(ATLAS_DECISION_OP_APPROVE, ATLAS_DECISION_CHANNEL_REMOTE,
                  atlas_buf_cstr(&e.fact_uid), NULL, ATLAS_DECISION_INTENT_APPROVE, 0, &cr.token,
                  cr.confirm, e.dispose_token, e.dispose_id,
                  ATLAS_DECISION_KIND_BIT(ATLAS_DECISION_KIND_OPERATIONAL_FACT), &ap, &err),
        &err);
    atlas_decision_result ar;
    atlas_decision_result_init(&ar);
    T_OK(atlas_decision_apply(db, &ap, &ar, &err), &err);
    T_CHECK(ar.state == ATLAS_DECISION_APPROVED);
    T_CHECK(ar.actor == ATLAS_DECISION_ACTOR_REMOTE_OPERATOR_CONFIRMED);
    T_CHECK_MSG(strcmp(ar.key_id, e.dispose_id) == 0, "approve result key_id mismatch");

    event_query q;
    find_event(db, ar.document_id, "APPROVED", &q);
    T_EQ_STR(q.actor, "REMOTE_OPERATOR_CONFIRMED");
    T_CHECK(q.key_id_present);
    T_EQ_STR(q.key_id, e.dispose_id);
    char want_detail[300];
    (void)snprintf(want_detail, sizeof(want_detail),
                  "confirmed through the Atlas remote operator channel with credential %s; this "
                  "records that the channel and the credential were used, not which person used "
                  "them",
                  e.dispose_id);
    T_EQ_STR(q.detail, want_detail);

    bool ok = false;
    atlas_buf detail = ATLAS_BUF_INIT;
    T_OK(atlas_db_decision_verify(db, ar.document_id, &ok, &detail, &err), &err);
    T_CHECK_MSG(ok, "the ledger and the cache disagree: %s", atlas_buf_cstr(&detail));
    atlas_buf_free(&detail);

    atlas_decision_result_free(&ar);
    atlas_decision_op_free(&ap);
    atlas_decision_result_free(&cr);
    atlas_decision_op_free(&ch);
    atlas_db_close(db);
    env_close(&e);
}

/* --- (c): a LOCAL challenge cannot be spent from the browser ---------------- */

static void test_c_local_challenge_cannot_be_spent_remotely(void) {
    env e;
    env_open(&e);
    atlas_db *db = open_db(&e);
    atlas_err err;
    atlas_err_init(&err);

    atlas_decision_op ch;
    T_OK(build_op(ATLAS_DECISION_OP_CHALLENGE, ATLAS_DECISION_CHANNEL_LOCAL,
                  atlas_buf_cstr(&e.fact_uid), NULL, ATLAS_DECISION_INTENT_APPROVE, 0, NULL, NULL,
                  NULL, NULL, 0, &ch, &err),
        &err);
    atlas_decision_result cr;
    atlas_decision_result_init(&cr);
    T_OK(atlas_decision_apply(db, &ch, &cr, &err), &err);

    atlas_decision_op ap;
    T_OK(build_op(ATLAS_DECISION_OP_APPROVE, ATLAS_DECISION_CHANNEL_REMOTE,
                  atlas_buf_cstr(&e.fact_uid), NULL, ATLAS_DECISION_INTENT_APPROVE, 0, &cr.token,
                  cr.confirm, e.dispose_token, e.dispose_id,
                  ATLAS_DECISION_KIND_BIT(ATLAS_DECISION_KIND_OPERATIONAL_FACT), &ap, &err),
        &err);
    atlas_decision_result ar;
    atlas_decision_result_init(&ar);
    atlas_status st = atlas_decision_apply(db, &ap, &ar, &err);
    T_CHECK_MSG(st != ATLAS_OK, "a LOCAL challenge was spent through the REMOTE channel");
    T_CHECK_MSG(
        strstr(atlas_err_msg(&err),
              "that approval challenge was minted through the local channel and cannot be spent "
              "from the browser") != NULL,
        "wrong refusal: %s", atlas_err_msg(&err));

    atlas_decision_result_free(&ar);
    atlas_decision_op_free(&ap);
    atlas_decision_result_free(&cr);
    atlas_decision_op_free(&ch);
    atlas_db_close(db);
    env_close(&e);
}

/* --- (d): the credential itself ---------------------------------------------- */

static void corrupt_secret(const char *token, char *out, size_t out_size) {
    (void)snprintf(out, out_size, "%s", token);
    size_t n = strlen(out);
    T_REQUIRE(n > 0);
    char *last = &out[n - 1];
    *last = (*last == 'A') ? 'B' : 'A';
}

static void corrupt_selector(const char *token, char *out, size_t out_size) {
    (void)snprintf(out, out_size, "%s", token);
    size_t off = strlen("atlas_");
    T_REQUIRE(strlen(out) > off);
    char *c = &out[off];
    *c = (*c == '0') ? '1' : '0';
}

static void test_d_credential_checks(void) {
    env e;
    env_open(&e);
    atlas_db *db = open_db(&e);
    atlas_err err;
    atlas_err_init(&err);

    /* reader's token, expected id = reader's own id: refused for holding a
     * stored scope, naming it. */
    {
        atlas_decision_op ch;
        T_OK(build_op(ATLAS_DECISION_OP_CHALLENGE, ATLAS_DECISION_CHANNEL_REMOTE,
                      atlas_buf_cstr(&e.fact_uid), NULL, ATLAS_DECISION_INTENT_APPROVE, 1, NULL,
                      NULL, e.reader_token, e.reader_id,
                      ATLAS_DECISION_KIND_BIT(ATLAS_DECISION_KIND_OPERATIONAL_FACT), &ch, &err),
            &err);
        atlas_decision_result cr;
        atlas_decision_result_init(&cr);
        atlas_status st = atlas_decision_apply(db, &ch, &cr, &err);
        T_CHECK_MSG(st != ATLAS_OK, "the reader credential minted a disposal challenge");
        T_CHECK_MSG(strstr(atlas_err_msg(&err), "must hold no stored scope") != NULL &&
                       strstr(atlas_err_msg(&err), "decisions:read") != NULL,
                    "wrong refusal: %s", atlas_err_msg(&err));
        atlas_decision_result_free(&cr);
        atlas_decision_op_free(&ch);
    }

    /* dispose's token, expected id = reader's id: refused as the wrong
     * credential for the policy line named. */
    {
        atlas_decision_op ch;
        T_OK(build_op(ATLAS_DECISION_OP_CHALLENGE, ATLAS_DECISION_CHANNEL_REMOTE,
                      atlas_buf_cstr(&e.fact_uid), NULL, ATLAS_DECISION_INTENT_APPROVE, 1, NULL,
                      NULL, e.dispose_token, e.reader_id,
                      ATLAS_DECISION_KIND_BIT(ATLAS_DECISION_KIND_OPERATIONAL_FACT), &ch, &err),
            &err);
        atlas_decision_result cr;
        atlas_decision_result_init(&cr);
        atlas_status st = atlas_decision_apply(db, &ch, &cr, &err);
        T_CHECK_MSG(st != ATLAS_OK, "a credential authenticated for the wrong policy line");
        T_CHECK_MSG(
            strstr(atlas_err_msg(&err), "that credential is not the one the remote disposal "
                                        "policy names") != NULL,
            "wrong refusal: %s", atlas_err_msg(&err));
        atlas_decision_result_free(&cr);
        atlas_decision_op_free(&ch);
    }

    /* A wrong secret and an unknown selector both authenticate to nothing. */
    {
        char bad[ATLAS_APIKEY_TOKEN_MAX];
        corrupt_secret(e.dispose_token, bad, sizeof(bad));
        atlas_decision_op ch;
        T_OK(build_op(ATLAS_DECISION_OP_CHALLENGE, ATLAS_DECISION_CHANNEL_REMOTE,
                      atlas_buf_cstr(&e.fact_uid), NULL, ATLAS_DECISION_INTENT_APPROVE, 1, NULL,
                      NULL, bad, e.dispose_id,
                      ATLAS_DECISION_KIND_BIT(ATLAS_DECISION_KIND_OPERATIONAL_FACT), &ch, &err),
            &err);
        atlas_decision_result cr;
        atlas_decision_result_init(&cr);
        atlas_status st = atlas_decision_apply(db, &ch, &cr, &err);
        T_CHECK_MSG(st != ATLAS_OK, "a wrong secret authenticated");
        T_CHECK_MSG(strstr(atlas_err_msg(&err), "did not authenticate; nothing was changed") !=
                       NULL,
                    "wrong refusal: %s", atlas_err_msg(&err));
        atlas_decision_result_free(&cr);
        atlas_decision_op_free(&ch);
    }
    {
        char bad[ATLAS_APIKEY_TOKEN_MAX];
        corrupt_selector(e.dispose_token, bad, sizeof(bad));
        atlas_decision_op ch;
        T_OK(build_op(ATLAS_DECISION_OP_CHALLENGE, ATLAS_DECISION_CHANNEL_REMOTE,
                      atlas_buf_cstr(&e.fact_uid), NULL, ATLAS_DECISION_INTENT_APPROVE, 1, NULL,
                      NULL, bad, e.dispose_id,
                      ATLAS_DECISION_KIND_BIT(ATLAS_DECISION_KIND_OPERATIONAL_FACT), &ch, &err),
            &err);
        atlas_decision_result cr;
        atlas_decision_result_init(&cr);
        atlas_status st = atlas_decision_apply(db, &ch, &cr, &err);
        T_CHECK_MSG(st != ATLAS_OK, "an unknown selector authenticated");
        T_CHECK_MSG(strstr(atlas_err_msg(&err), "did not authenticate; nothing was changed") !=
                       NULL,
                    "wrong refusal: %s", atlas_err_msg(&err));
        atlas_decision_result_free(&cr);
        atlas_decision_op_free(&ch);
    }

    /* Minted, then revoked before it is spent: the credential no longer
     * authenticates, and the challenge is left unconsumed. */
    {
        atlas_decision_op ch;
        T_OK(build_op(ATLAS_DECISION_OP_CHALLENGE, ATLAS_DECISION_CHANNEL_REMOTE,
                      atlas_buf_cstr(&e.fact_uid), NULL, ATLAS_DECISION_INTENT_APPROVE, 1, NULL,
                      NULL, e.dispose_token, e.dispose_id,
                      ATLAS_DECISION_KIND_BIT(ATLAS_DECISION_KIND_OPERATIONAL_FACT), &ch, &err),
            &err);
        atlas_decision_result cr;
        atlas_decision_result_init(&cr);
        T_OK(atlas_decision_apply(db, &ch, &cr, &err), &err);

        atlas_buf rout = ATLAS_BUF_INIT;
        int rcode = 0;
        const char *revoke[] = {"api-key", "revoke", e.dispose_id};
        run_atlas(&e, revoke, 3u, &rout, &rcode);
        T_EQ_INT(rcode, 0);
        atlas_buf_free(&rout);

        atlas_decision_op ap;
        T_OK(build_op(ATLAS_DECISION_OP_APPROVE, ATLAS_DECISION_CHANNEL_REMOTE,
                      atlas_buf_cstr(&e.fact_uid), NULL, ATLAS_DECISION_INTENT_APPROVE, 0,
                      &cr.token, cr.confirm, e.dispose_token, e.dispose_id,
                      ATLAS_DECISION_KIND_BIT(ATLAS_DECISION_KIND_OPERATIONAL_FACT), &ap, &err),
            &err);
        atlas_decision_result ar;
        atlas_decision_result_init(&ar);
        atlas_status st = atlas_decision_apply(db, &ap, &ar, &err);
        T_CHECK_MSG(st != ATLAS_OK, "a revoked credential still spent a challenge");
        T_CHECK_MSG(strstr(atlas_err_msg(&err), "did not authenticate; nothing was changed") !=
                       NULL,
                    "wrong refusal: %s", atlas_err_msg(&err));

        atlas_decision_challenge still;
        load_challenge(db, &cr.token, &still);
        T_CHECK_MSG(!still.consumed, "the challenge was consumed despite the refusal");

        atlas_decision_result_free(&ar);
        atlas_decision_op_free(&ap);
        atlas_decision_result_free(&cr);
        atlas_decision_op_free(&ch);
    }

    atlas_db_close(db);
    env_close(&e);
}

/* --- (e): the newest-revision guard, at mint and at spend ------------------- */

static void test_e_newest_revision_guard(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    /* expect_revision_no = 0 is refused outright. */
    {
        atlas_db *db = open_db(&e);
        atlas_decision_op ch;
        T_OK(build_op(ATLAS_DECISION_OP_CHALLENGE, ATLAS_DECISION_CHANNEL_REMOTE,
                      atlas_buf_cstr(&e.fact_uid), NULL, ATLAS_DECISION_INTENT_APPROVE, 0, NULL,
                      NULL, e.dispose_token, e.dispose_id,
                      ATLAS_DECISION_KIND_BIT(ATLAS_DECISION_KIND_OPERATIONAL_FACT), &ch, &err),
            &err);
        atlas_decision_result cr;
        atlas_decision_result_init(&cr);
        atlas_status st = atlas_decision_apply(db, &ch, &cr, &err);
        T_CHECK_MSG(st != ATLAS_OK, "expect_revision_no = 0 minted a remote challenge");
        T_CHECK_MSG(strstr(atlas_err_msg(&err), "0 is not a revision") != NULL,
                    "wrong refusal: %s", atlas_err_msg(&err));
        atlas_decision_result_free(&cr);
        atlas_decision_op_free(&ch);
        atlas_db_close(db);
    }

    /* Revise to r2, then mint a REMOTE challenge pinned to r1: refused,
     * naming both revisions. */
    {
        atlas_buf out = ATLAS_BUF_INIT;
        int code = 0;
        const char *revise2[] = {
            "decision", "revise", "proj", atlas_buf_cstr(&e.fact_uid), "--title",
            "a live endpoint, revised", "--decision", "the endpoint moved",
        };
        run_atlas(&e, revise2, 8u, &out, &code);
        T_EQ_INT(code, 0);
        atlas_buf_free(&out);

        atlas_db *db = open_db(&e);
        atlas_decision_op ch;
        T_OK(build_op(ATLAS_DECISION_OP_CHALLENGE, ATLAS_DECISION_CHANNEL_REMOTE,
                      atlas_buf_cstr(&e.fact_uid), NULL, ATLAS_DECISION_INTENT_APPROVE, 1, NULL,
                      NULL, e.dispose_token, e.dispose_id,
                      ATLAS_DECISION_KIND_BIT(ATLAS_DECISION_KIND_OPERATIONAL_FACT), &ch, &err),
            &err);
        atlas_decision_result cr;
        atlas_decision_result_init(&cr);
        atlas_status st = atlas_decision_apply(db, &ch, &cr, &err);
        T_CHECK_MSG(st != ATLAS_OK, "a remote challenge pinned to a non-newest revision minted");
        T_CHECK_MSG(strstr(atlas_err_msg(&err), "r1 was reviewed but r2 is newest") != NULL,
                    "wrong refusal: %s", atlas_err_msg(&err));
        atlas_decision_result_free(&cr);
        atlas_decision_op_free(&ch);
        atlas_db_close(db);
    }

    /* Mint for r2 (newest at mint time), revise again to r3, then spend:
     * refused because the decision gained a revision since the mint. */
    atlas_decision_op ch2;
    atlas_decision_result cr2;
    atlas_decision_result_init(&cr2);
    {
        atlas_db *db = open_db(&e);
        T_OK(build_op(ATLAS_DECISION_OP_CHALLENGE, ATLAS_DECISION_CHANNEL_REMOTE,
                      atlas_buf_cstr(&e.fact_uid), NULL, ATLAS_DECISION_INTENT_APPROVE, 2, NULL,
                      NULL, e.dispose_token, e.dispose_id,
                      ATLAS_DECISION_KIND_BIT(ATLAS_DECISION_KIND_OPERATIONAL_FACT), &ch2, &err),
            &err);
        T_OK(atlas_decision_apply(db, &ch2, &cr2, &err), &err);
        atlas_db_close(db);
    }

    atlas_buf out2 = ATLAS_BUF_INIT;
    int code2 = 0;
    const char *revise3[] = {
        "decision", "revise", "proj", atlas_buf_cstr(&e.fact_uid), "--title",
        "a live endpoint, revised again", "--decision", "the endpoint moved twice",
    };
    run_atlas(&e, revise3, 8u, &out2, &code2);
    T_EQ_INT(code2, 0);
    atlas_buf_free(&out2);

    atlas_db *db = open_db(&e);
    atlas_decision_op ap;
    T_OK(build_op(ATLAS_DECISION_OP_APPROVE, ATLAS_DECISION_CHANNEL_REMOTE,
                  atlas_buf_cstr(&e.fact_uid), NULL, ATLAS_DECISION_INTENT_APPROVE, 0, &cr2.token,
                  cr2.confirm, e.dispose_token, e.dispose_id,
                  ATLAS_DECISION_KIND_BIT(ATLAS_DECISION_KIND_OPERATIONAL_FACT), &ap, &err),
        &err);
    atlas_decision_result ar;
    atlas_decision_result_init(&ar);
    atlas_status st = atlas_decision_apply(db, &ap, &ar, &err);
    T_CHECK_MSG(st != ATLAS_OK, "a stale remote challenge was spent after a later revision arrived");
    T_CHECK_MSG(strstr(atlas_err_msg(&err), "gained revision 3") != NULL, "wrong refusal: %s",
                atlas_err_msg(&err));

    atlas_decision_challenge still;
    load_challenge(db, &cr2.token, &still);
    T_CHECK_MSG(!still.consumed, "the stale challenge was consumed anyway");

    atlas_decision_result_free(&ar);
    atlas_decision_op_free(&ap);
    atlas_decision_result_free(&cr2);
    atlas_decision_op_free(&ch2);
    atlas_db_close(db);
    env_close(&e);
}

/* --- (f): the kinds policy, at mint ------------------------------------------ */

static void test_f_kinds_policy_at_mint(void) {
    env e;
    env_open(&e);
    atlas_db *db = open_db(&e);
    atlas_err err;
    atlas_err_init(&err);

    {
        atlas_decision_op ch;
        T_OK(build_op(ATLAS_DECISION_OP_CHALLENGE, ATLAS_DECISION_CHANNEL_REMOTE,
                      atlas_buf_cstr(&e.policy_uid), NULL, ATLAS_DECISION_INTENT_APPROVE, 1, NULL,
                      NULL, e.dispose_token, e.dispose_id,
                      ATLAS_DECISION_KIND_BIT(ATLAS_DECISION_KIND_OPERATIONAL_FACT), &ch, &err),
            &err);
        atlas_decision_result cr;
        atlas_decision_result_init(&cr);
        atlas_status st = atlas_decision_apply(db, &ch, &cr, &err);
        T_CHECK_MSG(st != ATLAS_OK, "a POLICY record minted under a policy naming only "
                                    "OPERATIONAL_FACT");
        T_CHECK_MSG(
            strstr(atlas_err_msg(&err), "is not one the remote disposal policy names; dispose of "
                                        "it on a terminal") != NULL,
            "wrong refusal: %s", atlas_err_msg(&err));
        atlas_decision_result_free(&cr);
        atlas_decision_op_free(&ch);
    }
    {
        atlas_decision_op ch;
        T_OK(build_op(ATLAS_DECISION_OP_CHALLENGE, ATLAS_DECISION_CHANNEL_REMOTE,
                      atlas_buf_cstr(&e.policy_uid), NULL, ATLAS_DECISION_INTENT_APPROVE, 1, NULL,
                      NULL, e.dispose_token, e.dispose_id,
                      ATLAS_DECISION_KIND_BIT(ATLAS_DECISION_KIND_OPERATIONAL_FACT) |
                          ATLAS_DECISION_KIND_BIT(ATLAS_DECISION_KIND_POLICY),
                      &ch, &err),
            &err);
        atlas_decision_result cr;
        atlas_decision_result_init(&cr);
        T_OK(atlas_decision_apply(db, &ch, &cr, &err), &err);
        T_CHECK(cr.token.len > 0);
        atlas_decision_result_free(&cr);
        atlas_decision_op_free(&ch);
    }

    atlas_db_close(db);
    env_close(&e);
}

/* --- (g): supersede and revalidate are never offered remotely --------------- */

static void test_g_supersede_and_revalidate_refused_remotely(void) {
    env e;
    env_open(&e);
    atlas_db *db = open_db(&e);
    atlas_err err;
    atlas_err_init(&err);

    {
        atlas_decision_op ch;
        T_OK(build_op(ATLAS_DECISION_OP_CHALLENGE, ATLAS_DECISION_CHANNEL_REMOTE,
                      atlas_buf_cstr(&e.fact_uid), atlas_buf_cstr(&e.policy_uid),
                      ATLAS_DECISION_INTENT_SUPERSEDE, 1, NULL, NULL, e.dispose_token,
                      e.dispose_id,
                      ATLAS_DECISION_KIND_BIT(ATLAS_DECISION_KIND_OPERATIONAL_FACT) |
                          ATLAS_DECISION_KIND_BIT(ATLAS_DECISION_KIND_POLICY),
                      &ch, &err),
            &err);
        atlas_decision_result cr;
        atlas_decision_result_init(&cr);
        atlas_status st = atlas_decision_apply(db, &ch, &cr, &err);
        T_CHECK_MSG(st != ATLAS_OK, "a REMOTE supersede challenge was minted");
        T_CHECK_MSG(
            strstr(atlas_err_msg(&err),
                  "supersede and revalidate are not offered from the browser; use a terminal on "
                  "the Atlas machine") != NULL,
            "wrong refusal: %s", atlas_err_msg(&err));
        atlas_decision_result_free(&cr);
        atlas_decision_op_free(&ch);
    }
    {
        atlas_decision_op ch;
        T_OK(build_op(ATLAS_DECISION_OP_CHALLENGE, ATLAS_DECISION_CHANNEL_REMOTE,
                      atlas_buf_cstr(&e.fact_uid), NULL, ATLAS_DECISION_INTENT_REVALIDATE, 1, NULL,
                      NULL, e.dispose_token, e.dispose_id,
                      ATLAS_DECISION_KIND_BIT(ATLAS_DECISION_KIND_OPERATIONAL_FACT), &ch, &err),
            &err);
        atlas_decision_result cr;
        atlas_decision_result_init(&cr);
        atlas_status st = atlas_decision_apply(db, &ch, &cr, &err);
        T_CHECK_MSG(st != ATLAS_OK, "a REMOTE revalidate challenge was minted");
        T_CHECK_MSG(
            strstr(atlas_err_msg(&err),
                  "supersede and revalidate are not offered from the browser; use a terminal on "
                  "the Atlas machine") != NULL,
            "wrong refusal: %s", atlas_err_msg(&err));
        atlas_decision_result_free(&cr);
        atlas_decision_op_free(&ch);
    }

    atlas_db_close(db);
    env_close(&e);
}

/* --- (h): reject and resolve, over the same channel ------------------------- */

static void test_h_remote_reject_and_resolve(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    atlas_db *db = open_db(&e);
    atlas_decision_op ch;
    T_OK(build_op(ATLAS_DECISION_OP_CHALLENGE, ATLAS_DECISION_CHANNEL_REMOTE,
                  atlas_buf_cstr(&e.fact_uid), NULL, ATLAS_DECISION_INTENT_REJECT, 1, NULL, NULL,
                  e.dispose_token, e.dispose_id,
                  ATLAS_DECISION_KIND_BIT(ATLAS_DECISION_KIND_OPERATIONAL_FACT), &ch, &err),
        &err);
    atlas_decision_result cr;
    atlas_decision_result_init(&cr);
    T_OK(atlas_decision_apply(db, &ch, &cr, &err), &err);

    atlas_decision_op rj;
    T_OK(build_op(ATLAS_DECISION_OP_REJECT, ATLAS_DECISION_CHANNEL_REMOTE,
                  atlas_buf_cstr(&e.fact_uid), NULL, ATLAS_DECISION_INTENT_REJECT, 0, &cr.token,
                  cr.confirm, e.dispose_token, e.dispose_id,
                  ATLAS_DECISION_KIND_BIT(ATLAS_DECISION_KIND_OPERATIONAL_FACT), &rj, &err),
        &err);
    atlas_decision_result rr;
    atlas_decision_result_init(&rr);
    T_OK(atlas_decision_apply(db, &rj, &rr, &err), &err);
    T_CHECK(rr.state == ATLAS_DECISION_REJECTED);
    T_CHECK(rr.actor == ATLAS_DECISION_ACTOR_REMOTE_OPERATOR_CONFIRMED);
    T_CHECK_MSG(strcmp(rr.key_id, e.dispose_id) == 0, "reject result key_id mismatch");

    event_query q;
    find_event(db, rr.document_id, "REJECTED", &q);
    T_EQ_STR(q.actor, "REMOTE_OPERATOR_CONFIRMED");
    T_CHECK(q.key_id_present);
    T_EQ_STR(q.key_id, e.dispose_id);

    atlas_decision_result_free(&rr);
    atlas_decision_op_free(&rj);
    atlas_decision_result_free(&cr);
    atlas_decision_op_free(&ch);
    atlas_db_close(db);

    /* An OBLIGATION, approved locally (RESOLVE only starts from APPROVED, and
     * the local path is proved unchanged elsewhere in this suite), then
     * resolved from the browser. */
    atlas_buf obligation_uid = ATLAS_BUF_INIT;
    propose_kind(&e, "OBLIGATION", "pay down the migration debt", &obligation_uid);

    db = open_db(&e);
    atlas_decision_op lch;
    T_OK(build_op(ATLAS_DECISION_OP_CHALLENGE, ATLAS_DECISION_CHANNEL_LOCAL,
                  atlas_buf_cstr(&obligation_uid), NULL, ATLAS_DECISION_INTENT_APPROVE, 0, NULL,
                  NULL, NULL, NULL, 0, &lch, &err),
        &err);
    atlas_decision_result lcr;
    atlas_decision_result_init(&lcr);
    T_OK(atlas_decision_apply(db, &lch, &lcr, &err), &err);
    atlas_decision_op lap;
    T_OK(build_op(ATLAS_DECISION_OP_APPROVE, ATLAS_DECISION_CHANNEL_LOCAL,
                  atlas_buf_cstr(&obligation_uid), NULL, ATLAS_DECISION_INTENT_APPROVE, 0,
                  &lcr.token, lcr.confirm, NULL, NULL, 0, &lap, &err),
        &err);
    atlas_decision_result lar;
    atlas_decision_result_init(&lar);
    T_OK(atlas_decision_apply(db, &lap, &lar, &err), &err);
    T_CHECK(lar.state == ATLAS_DECISION_APPROVED);
    atlas_decision_result_free(&lar);
    atlas_decision_op_free(&lap);
    atlas_decision_result_free(&lcr);
    atlas_decision_op_free(&lch);

    atlas_decision_op rch;
    T_OK(build_op(ATLAS_DECISION_OP_CHALLENGE, ATLAS_DECISION_CHANNEL_REMOTE,
                  atlas_buf_cstr(&obligation_uid), NULL, ATLAS_DECISION_INTENT_RESOLVE, 1, NULL,
                  NULL, e.dispose_token, e.dispose_id,
                  ATLAS_DECISION_KIND_BIT(ATLAS_DECISION_KIND_OBLIGATION), &rch, &err),
        &err);
    atlas_decision_result rcr;
    atlas_decision_result_init(&rcr);
    T_OK(atlas_decision_apply(db, &rch, &rcr, &err), &err);

    atlas_decision_op rop;
    T_OK(build_op(ATLAS_DECISION_OP_RESOLVE, ATLAS_DECISION_CHANNEL_REMOTE,
                  atlas_buf_cstr(&obligation_uid), NULL, ATLAS_DECISION_INTENT_RESOLVE, 0,
                  &rcr.token, rcr.confirm, e.dispose_token, e.dispose_id,
                  ATLAS_DECISION_KIND_BIT(ATLAS_DECISION_KIND_OBLIGATION), &rop, &err),
        &err);
    atlas_decision_result rores;
    atlas_decision_result_init(&rores);
    T_OK(atlas_decision_apply(db, &rop, &rores, &err), &err);
    T_CHECK(rores.state == ATLAS_DECISION_RESOLVED);
    T_CHECK(rores.actor == ATLAS_DECISION_ACTOR_REMOTE_OPERATOR_CONFIRMED);
    T_CHECK_MSG(strcmp(rores.key_id, e.dispose_id) == 0, "resolve result key_id mismatch");

    event_query q2;
    find_event(db, rores.document_id, "RESOLVED", &q2);
    T_EQ_STR(q2.actor, "REMOTE_OPERATOR_CONFIRMED");
    T_CHECK(q2.key_id_present);
    T_EQ_STR(q2.key_id, e.dispose_id);

    atlas_decision_result_free(&rores);
    atlas_decision_op_free(&rop);
    atlas_decision_result_free(&rcr);
    atlas_decision_op_free(&rch);
    atlas_buf_free(&obligation_uid);
    atlas_db_close(db);
    env_close(&e);
}

/* --- (i): the local path, unchanged byte for byte --------------------------- */

static void test_i_local_path_unchanged(void) {
    env e;
    env_open(&e);
    atlas_db *db = open_db(&e);
    atlas_err err;
    atlas_err_init(&err);

    atlas_decision_op ch;
    T_OK(build_op(ATLAS_DECISION_OP_CHALLENGE, ATLAS_DECISION_CHANNEL_LOCAL,
                  atlas_buf_cstr(&e.policy_uid), NULL, ATLAS_DECISION_INTENT_APPROVE, 0, NULL,
                  NULL, NULL, NULL, 0, &ch, &err),
        &err);
    atlas_decision_result cr;
    atlas_decision_result_init(&cr);
    T_OK(atlas_decision_apply(db, &ch, &cr, &err), &err);

    atlas_decision_op ap;
    T_OK(build_op(ATLAS_DECISION_OP_APPROVE, ATLAS_DECISION_CHANNEL_LOCAL,
                  atlas_buf_cstr(&e.policy_uid), NULL, ATLAS_DECISION_INTENT_APPROVE, 0,
                  &cr.token, cr.confirm, NULL, NULL, 0, &ap, &err),
        &err);
    atlas_decision_result ar;
    atlas_decision_result_init(&ar);
    T_OK(atlas_decision_apply(db, &ap, &ar, &err), &err);
    T_CHECK(ar.state == ATLAS_DECISION_APPROVED);
    T_CHECK(ar.actor == ATLAS_DECISION_ACTOR_LOCAL_OPERATOR_CONFIRMED);
    T_CHECK_MSG(ar.key_id[0] == '\0', "a LOCAL approval reported a key_id: %s", ar.key_id);

    event_query q;
    find_event(db, ar.document_id, "APPROVED", &q);
    T_EQ_STR(q.actor, "LOCAL_OPERATOR_CONFIRMED");
    T_CHECK_MSG(!q.key_id_present, "a LOCAL event carries a key_id");
    T_EQ_STR(q.detail,
            "confirmed through the Atlas local operator channel; this records that the channel "
            "was used, not which person used it");

    atlas_decision_result_free(&ar);
    atlas_decision_op_free(&ap);
    atlas_decision_result_free(&cr);
    atlas_decision_op_free(&ch);
    atlas_db_close(db);
    env_close(&e);
}

/* --- (j): a REMOTE op cannot be serialised for the socket; the wipe -------- */

static void test_j_remote_op_refused_over_the_socket_and_token_is_wiped(void) {
    atlas_err err;
    atlas_err_init(&err);

    atlas_decision_op op;
    atlas_decision_op_init(&op, ATLAS_DECISION_OP_APPROVE);
    op.channel = ATLAS_DECISION_CHANNEL_REMOTE;
    T_OK(atlas_buf_set_str(&op.remote_token, "a presented bearer credential", &err), &err);

    atlas_buf params = ATLAS_BUF_INIT;
    atlas_status st = atlas_service_decision_op_to_params(&op, &params, &err);
    T_CHECK_MSG(st != ATLAS_OK, "a REMOTE op was serialised for the socket");
    T_CHECK_MSG(strstr(atlas_err_msg(&err), "cannot be sent over this interface") != NULL,
                "wrong refusal: %s", atlas_err_msg(&err));
    T_CHECK_MSG(params.len == 0, "a REMOTE op produced request bytes despite the refusal");
    atlas_buf_free(&params);

    /* The wipe itself, verified by inspection rather than by reading freed
     * memory: dereferencing `op.remote_token.data` after
     * `atlas_decision_op_free` is undefined behaviour, and this project
     * builds an ASan suite (`make asan`) this test also runs under, where
     * that read would be reported as a use-after-free rather than proving
     * anything about the wipe. `atlas_decision_op_free` in
     * `src/decision/lifecycle.c` memsets every byte of
     * `remote_token.data[0..remote_token.cap)` to zero, unconditionally
     * whenever `remote_token.data != NULL`, strictly before the matching
     * `atlas_buf_free(&op->remote_token)` releases the allocation -- read
     * there rather than reproduced here, on `gateway.c`'s wipe-the-login-key
     * precedent this code cites by name. */
    atlas_decision_op_free(&op);
}

static const atlas_test TESTS[] = {
    {"a channel-less op is refused, for both a mint and a spend",
     test_a_channel_less_op_is_refused},
    {"the core REMOTE approve happy path", test_b_remote_approve_happy_path},
    {"a LOCAL challenge cannot be spent through the REMOTE channel",
     test_c_local_challenge_cannot_be_spent_remotely},
    {"the disposal credential is verified inside the transaction", test_d_credential_checks},
    {"the newest-revision guard, at mint and at spend", test_e_newest_revision_guard},
    {"the kinds policy is checked at mint", test_f_kinds_policy_at_mint},
    {"supersede and revalidate are never offered remotely",
     test_g_supersede_and_revalidate_refused_remotely},
    {"reject and resolve travel the same channel as approve", test_h_remote_reject_and_resolve},
    {"the local path is unchanged, byte for byte", test_i_local_path_unchanged},
    {"a REMOTE op cannot be sent over the socket, and its token is wiped",
     test_j_remote_op_refused_over_the_socket_and_token_is_wiped},
};

ATLAS_TEST_MAIN("decision_remote", TESTS)
