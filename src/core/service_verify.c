/* Atlas - A9.2: the `verify` command behaviour.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The service layer for claims, attestations and the verification engine. It
 * holds no formatting and the renderers hold no queries, which is the layering
 * rule the whole CLI follows.
 *
 * Two things about *which* operations exist here are security decisions rather
 * than scope decisions, and both are worth stating where somebody adding a
 * function will read them.
 *
 * **`verify run` is an operator action.** It can change a lifecycle state, so
 * it is offered over IPC only in the operator-uid group, beside `code.index`
 * and `backup.create`, and every other peer is told the method does not exist.
 * `verify show` and `verify claims` are ordinary reads and sit in the ordinary
 * group.
 *
 * **Nothing here mints an operator capability.** The engine's authority comes
 * from a root-owned file and produces `VERIFICATION_POLICY` in the ledger, an
 * actor no adapter can write and which is not `LOCAL_OPERATOR_CONFIRMED`. An
 * operator running `verify run` is asking Atlas to evaluate a policy somebody
 * else installed, not approving anything themselves — which is why it needs no
 * terminal, no challenge and no confirmation.
 */
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "core/service_internal.h"
#include "atlas/db.h"
#include "atlas/service.h"
#include "atlas/verify.h"
#include "atlas/verifypolicy.h"

void atlas_verify_report_init(atlas_verify_report *r) {
    if (r == NULL) {
        return;
    }
    memset(r, 0, sizeof *r);
    atlas_verify_assessment_init(&r->assessment);
    atlas_buf_init(&r->claim_uid);
    atlas_buf_init(&r->claim_text);
    atlas_buf_init(&r->domain);
    atlas_buf_init(&r->record_uid);
    atlas_buf_init(&r->record_title);
    r->policy_state = ATLAS_VERIFYPOLICY_DISABLED;
    r->policy_reason = ATLAS_VERIFYPOLICY_REASON_ABSENT;
}

void atlas_verify_report_free(atlas_verify_report *r) {
    if (r == NULL) {
        return;
    }
    atlas_buf_free(&r->claim_uid);
    atlas_buf_free(&r->claim_text);
    atlas_buf_free(&r->domain);
    atlas_buf_free(&r->record_uid);
    atlas_buf_free(&r->record_title);
    memset(r, 0, sizeof *r);
}

/* Fills the descriptive half of a report from the claim and its record.
 *
 * Every text field it copies is `UNTRUSTED_DATA`: a claim's proposition is
 * written by whoever wrote it, and a record's title is prose somebody or
 * something else chose. Approval does not change the nature of bytes, and
 * neither does verification — a VERIFIED claim's text is exactly as untrusted
 * as a proposed one's. Both renderers encode it. */
static atlas_status describe(atlas_db *db, int64_t claim_id, atlas_verify_report *out,
                             atlas_err *err) {
    atlas_verify_claim c;
    atlas_verify_claim_init(&c);
    bool found = false;
    atlas_status st = atlas_db_verify_claim_get(db, claim_id, &c, &found, err);
    if (st == ATLAS_OK && !found) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "no claim has that id");
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&out->claim_uid, c.uid.data, c.uid.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&out->claim_text, c.text.data, c.text.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&out->domain, c.domain.data, c.domain.len, err);
    }
    if (st == ATLAS_OK && c.document_id > 0) {
        st = atlas_db_decision_uid_of(db, c.document_id, &out->record_uid, err);
    }
    atlas_verify_claim_free(&c);
    return st;
}

static void policy_into(const atlas_verifypolicy *p, atlas_verify_report *out) {
    out->policy_state = p->state;
    out->policy_reason = p->reason;
    (void)snprintf(out->policy_id, sizeof out->policy_id, "%s", p->policy_id);
    (void)snprintf(out->policy_hash, sizeof out->policy_hash, "%s", p->policy_hash);
    out->deterministic_enforce = p->deterministic_enforce;
    out->empirical_enforce = p->empirical_enforce;
    out->rule_count = p->rule_count;
}

atlas_status atlas_service_verify_show(atlas_ctx *ctx, int64_t claim_id, atlas_verify_report *out,
                                       atlas_err *err) {
    atlas_db *db = atlas_ctx_db(ctx);
    if (db == NULL) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG, "no index is available to read");
    }
    atlas_verifypolicy p;
    atlas_verifypolicy_load(&p);
    policy_into(&p, out);

    atlas_status st = describe(db, claim_id, out, err);
    if (st != ATLAS_OK) {
        return st;
    }
    /* The read path assesses and **writes nothing**: no result row, no audit
     * row, no transition. `atlas_verify_assess` is side-effect-free so that
     * asking what Atlas thinks cannot change what Atlas thinks — the same
     * property A6's gate has, and for the same reason. */
    return atlas_verify_assess(db, &p, claim_id, &out->assessment, err);
}

atlas_status atlas_service_verify_run(atlas_ctx *ctx, int64_t claim_id, const char *repo_name,
                                      atlas_verify_report *out, atlas_err *err) {
    atlas_db *db = atlas_ctx_db(ctx);
    if (db == NULL) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG, "no index is available to write");
    }
    atlas_verifypolicy p;
    atlas_verifypolicy_load(&p);
    policy_into(&p, out);

    atlas_status st = describe(db, claim_id, out, err);
    if (st != ATLAS_OK) {
        return st;
    }
    return atlas_verify_autolifecycle_run(db, &p, claim_id, repo_name, &out->assessment, err);
}

atlas_status atlas_service_verify_policy(atlas_verify_report *out, atlas_err *err) {
    (void)err;
    /* Reads the root-owned policy and binds nothing, so it is safe to run
     * anywhere — the shape `gateway status` has. With no policy installed it
     * says so and names the path, rather than reporting a state it inferred. */
    atlas_verifypolicy p;
    atlas_verifypolicy_load(&p);
    policy_into(&p, out);
    (void)snprintf(out->policy_path, sizeof out->policy_path, "%s", ATLAS_VERIFYPOLICY_PATH);
    (void)snprintf(out->policy_detail, sizeof out->policy_detail, "%s",
                   atlas_verifypolicy_reason_detail(p.reason));
    return ATLAS_OK;
}
