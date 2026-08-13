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
#include "atlas/json.h"
#include "atlas/safetext.h"
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
    atlas_verify_detail_free(&r->detail);
    memset(r, 0, sizeof *r);
}

/* Loads the readable evidence and attestations, and marks which independent
 * group each attestation landed in.
 *
 * The grouping is recomputed here rather than carried out of the assessment,
 * and that is deliberate: `atlas_verify_independent_groups` is a pure function
 * of the same inputs, so asking it twice cannot produce a different partition
 * than the one the score was computed from. Threading the partition out of the
 * aggregation would have made the display a second consumer of the algorithm's
 * internals — and the first thing a later edit would then be tempted to do is
 * feed something from the display back in. Nothing loaded here reaches a
 * verdict; A9.2's rule that `atlas_verify_assess` writes nothing is untouched,
 * because this reads nothing but rows.
 *
 * A failure to load the detail is not a failure to assess: an answer with no
 * visible evidence is worse than the same answer with it, but it is still the
 * answer, and refusing to report a verdict because a display query failed
 * would turn a cosmetic fault into an outage. */
static atlas_status load_detail(atlas_db *db, const atlas_verifypolicy *p, int64_t claim_id,
                                atlas_verify_report *out, atlas_err *err) {
    char stale_before[ATLAS_TS_MAX];
    long long age = p != NULL && p->max_evidence_age > 0 ? p->max_evidence_age
                                                         : ATLAS_VERIFY_DEFAULT_MAX_EVIDENCE_AGE;
    atlas_iso8601_before_now(stale_before, sizeof stale_before, age * 1000);

    atlas_status st = atlas_db_verify_detail_load(db, claim_id, stale_before, &out->detail, err);
    if (st != ATLAS_OK) {
        return st;
    }
    atlas_verify_inputs in;
    memset(&in, 0, sizeof in);
    st = atlas_db_verify_inputs_load(db, claim_id, stale_before, &in, err);
    if (st == ATLAS_OK) {
        (void)atlas_verify_independent_groups(in.items, in.count, in.dep_from, in.dep_to,
                                              in.dep_count);
        for (size_t i = 0; i < in.count; i++) {
            for (size_t k = 0; k < out->detail.attestation_count; k++) {
                if (out->detail.attestations[k].id == in.items[i].attestation_id) {
                    out->detail.attestations[k].group = in.items[i].group;
                    break;
                }
            }
        }
    }
    atlas_verify_inputs_free(&in);
    return st;
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

/* --- A9.2.1: the daemon-side forms -----------------------------------------
 *
 * These take a raw handle rather than an `atlas_ctx`, so the CLI and the daemon
 * call one implementation — A8-CI's rule that parity between surfaces is
 * structural rather than two functions somebody keeps in step.
 *
 * A claim may be named by rowid or by public uid. The uid is what every other
 * surface reports and therefore what a client has; the rowid is what A9.2's CLI
 * took. Both resolve here so the two spellings cannot answer differently. */
/* Resolves a claim named by its uid to the rowid the engine works in.
 *
 * A claim is reported by its uid everywhere — `verify claim` prints one, MCP
 * returns one, the gateway returns one — and by its rowid nowhere. Both
 * spellings are accepted and they cannot collide, because a uid never parses as
 * a number.
 *
 * One helper, two callers, deliberately. `verify show` grew this and `verify
 * run` did not, so the command that *reads* took the id every surface hands you
 * and the command that *enforces* answered "no claim has that id" to the same
 * string. An operator following the documented workflow could not run the one
 * command in Atlas that performs a machine transition, and nothing in the
 * message suggested a rowid was wanted. */
static atlas_status resolve_claim(atlas_db *db, int64_t *claim_id, const char *claim_uid,
                                  const char *what, atlas_err *err) {
    if (*claim_id == 0 && claim_uid != NULL && *claim_uid != '\0') {
        atlas_verify_claim c;
        atlas_verify_claim_init(&c);
        bool found = false;
        atlas_status fst = atlas_db_verify_claim_find(db, claim_uid, &c, &found, err);
        if (fst == ATLAS_OK && !found) {
            fst = atlas_err_set(err, ATLAS_ERR_USAGE, "no claim has that id");
        }
        if (fst == ATLAS_OK) {
            *claim_id = c.id;
        }
        atlas_verify_claim_free(&c);
        if (fst != ATLAS_OK) {
            return fst;
        }
    }
    if (*claim_id == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "name the claim to %s", what);
    }
    return ATLAS_OK;
}

atlas_status atlas_service_verify_show_on(atlas_db *db, int64_t claim_id, const char *claim_uid,
                                          atlas_verify_report *out, atlas_err *err) {
    if (db == NULL) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG, "no index is available to read");
    }
    {
        atlas_status rst = resolve_claim(db, &claim_id, claim_uid, "show", err);
        if (rst != ATLAS_OK) {
            return rst;
        }
    }
    atlas_verifypolicy p;
    atlas_verifypolicy_load(&p);
    policy_into(&p, out);
    (void)snprintf(out->policy_path, sizeof out->policy_path, "%s", ATLAS_VERIFYPOLICY_PATH);

    atlas_status st = describe(db, claim_id, out, err);
    if (st != ATLAS_OK) {
        return st;
    }
    (void)load_detail(db, &p, claim_id, out, err);
    return atlas_verify_assess(db, &p, claim_id, &out->assessment, err);
}

atlas_status atlas_service_verify_show(atlas_ctx *ctx, int64_t claim_id, atlas_verify_report *out,
                                       atlas_err *err) {
    /* A9.2 read `ctx->db` through `atlas_ctx_db` before testing anything, and
     * `atlas_ctx_db` dereferences its argument — so on a system deployment,
     * where the CLI reaches this with no context because the index is 0700
     * `atlasd`, `atlas verify show` segfaulted. The null test has to be on the
     * context, not on the handle it would have returned. */
    atlas_db *db = ctx == NULL ? NULL : atlas_ctx_db(ctx);
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
    (void)load_detail(db, &p, claim_id, out, err);
    return atlas_verify_assess(db, &p, claim_id, &out->assessment, err);
}

atlas_status atlas_service_verify_run(atlas_ctx *ctx, int64_t claim_id, const char *claim_uid,
                                      const char *repo_name, atlas_verify_report *out,
                                      atlas_err *err) {
    atlas_db *db = ctx == NULL ? NULL : atlas_ctx_db(ctx);
    if (db == NULL) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG, "no index is available to write");
    }
    {
        atlas_status rst = resolve_claim(db, &claim_id, claim_uid, "run", err);
        if (rst != ATLAS_OK) {
            return rst;
        }
    }
    atlas_verifypolicy p;
    atlas_verifypolicy_load(&p);
    policy_into(&p, out);

    atlas_status st = describe(db, claim_id, out, err);
    if (st != ATLAS_OK) {
        return st;
    }
    (void)load_detail(db, &p, claim_id, out, err);
    return atlas_verify_autolifecycle_run(db, &p, claim_id, repo_name, &out->assessment, err);
}

/* --- A9.2.1: one serialization, used by the daemon and by the CLI's JSON
 *             renderer -------------------------------------------------------
 *
 * A9.2 had one surface and could keep the shape in the renderer. A9.2.1 has
 * four, and the invariant Atlas states about them — "human and JSON output
 * consume identical service results" — is only true if there is one writer. So
 * the shape lives here and `render_json.c` calls it, which is also what lets
 * the CLI's socket path re-emit exactly what the daemon produced.
 *
 * **The three axes are printed as three fields, never as one.** A9.1's rule
 * about kind and status, extended: a single badge carrying a knowledge kind, a
 * lifecycle status and a verification state is the presentation these seasons
 * exist to prevent.
 *
 * **A confidence score carries no percent sign and a probability is absent
 * rather than null when calibration does not support it.** They are different
 * types with different printers and this is one of the printers. */
atlas_status atlas_service_verify_write_assessment(atlas_json *j,
                                                   const atlas_verify_assessment *a,
                                                   atlas_err *err) {
    const atlas_verify_aggregate *g = &a->aggregate;
    /* The three axes, in three separate fields — the rule A9.1 and A9.2 both
     * state, applied to the shape the *daemon* sends rather than only to the
     * one the CLI renders locally.
     *
     * These two were missing here, and the local and remote paths therefore
     * disagreed about a record's kind and its lifecycle status. Every surface
     * that reads over the socket lost both: MCP relays this object verbatim,
     * the gateway forwards it, and Mission Control binds `c.kind` and
     * `c.status` — so the page rendered nothing for the two rows, and on a
     * system deployment, where the index is `0700 atlasd` and the socket is the
     * only path, `atlas verify show --json` reported the zero values. Zero is
     * `DECISION` and `PROPOSED`, so what a caller received was not a gap but a
     * confident wrong answer about an APPROVED OBLIGATION. */
    atlas_status st = atlas_json_key_str(j, "kind", atlas_decision_kind_name(a->kind), err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "status", atlas_decision_state_name(a->from), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "state", atlas_verify_state_name(g->state), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "basis", atlas_verify_basis_name(a->basis), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "semantics", atlas_verify_claim_semantics_name(a->semantics),
                                err);
    }
    /* An integer out of 100. Never a percentage: `atlas_verify_calibration`
     * gates the only field that may be read as a probability. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "confidence_score", g->confidence, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "calibration", atlas_verify_calibration_name(g->calibration),
                                err);
    }
    /* Emitted only when calibration supports it — **absent**, not null and not
     * zero. A null would invite a client to render "0%", which is the exact lie
     * the two-field split exists to make impossible. */
    if (st == ATLAS_OK && g->calibration == ATLAS_CALIBRATION_CALIBRATED &&
        g->calibrated_probability >= 0) {
        st = atlas_json_key_int(j, "calibrated_probability", g->calibrated_probability, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "policy_verdict", atlas_verify_policy_verdict_name(g->verdict),
                                err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "support_count", g->support_count, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "contradict_count", g->contradict_count, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "inconclusive_count", g->inconclusive_count, err);
    }
    /* The number that makes "an actor is not evidence" visible: three models
     * reading one document are three attestations and one group. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "independent_groups", g->independent_groups, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "independent_families", g->independent_families, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "attestation_total", (int64_t)a->attestation_total, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(j, "truncated", a->truncated, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "conflict", atlas_verify_conflict_name(g->conflict), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(j, "stale", g->stale, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "verifier", atlas_verify_verifier_name(a->verifier), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "check", atlas_verify_check_name(a->check), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "algorithm", ATLAS_VERIFY_ALGORITHM, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "family_version", g->family_version, err);
    }
    /* §4/§5. What this assessment was of, and whether the ground moved. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(j, "claim_commit",
                                    a->claim_commit[0] != '\0' ? a->claim_commit : NULL, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(
            j, "evaluated_commit", a->evaluated_commit[0] != '\0' ? a->evaluated_commit : NULL,
            err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "sem_generation", a->sem_generation, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(j, "source_drift", a->source_drift, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(j, "actionable", a->actionable, err);
    }
    /* Whether Atlas actually moved a lifecycle state on its own authority. The
     * single most important field for an auditor of an automating system, so it
     * is always present rather than only when true. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(j, "transitioned", a->transitioned, err);
    }
    if (st == ATLAS_OK && a->result_id != 0) {
        st = atlas_json_key_int(j, "result_id", a->result_id, err);
    }
    if (st == ATLAS_OK && a->audit_id != 0) {
        st = atlas_json_key_int(j, "audit_id", a->audit_id, err);
    }
    if (st == ATLAS_OK && a->transitioned) {
        st = atlas_json_key_str(j, "from_status", atlas_decision_state_name(a->from), err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "to_status", atlas_decision_state_name(a->to), err);
        }
    }
    /* Atlas' own fixed sentences from a closed vocabulary. No repository byte
     * and no model byte reaches them, which is why they need no encoding. */
    if (st == ATLAS_OK) {
        st = atlas_json_key(j, "reasons", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(j, err);
    }
    for (size_t i = 0; st == ATLAS_OK && i < g->reason_count; i++) {
        st = atlas_json_obj_begin(j, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "code", atlas_verify_reason_name(g->reasons[i]), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "meaning", atlas_verify_reason_description(g->reasons[i]),
                                    err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_obj_end(j, err);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "reason_total", (int64_t)g->reason_total, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(j, "verified_scope",
                                    a->verified_scope[0] != '\0' ? a->verified_scope : NULL, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(j, "detail", a->detail[0] != '\0' ? a->detail : NULL, err);
    }
    return st;
}

atlas_status atlas_service_verify_write_policy(atlas_json *j, const atlas_verify_report *r,
                                               atlas_err *err) {
    atlas_status st = atlas_json_key(j, "policy", err);
    if (st == ATLAS_OK) {
        st = atlas_json_obj_begin(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "state", atlas_verifypolicy_state_name(r->policy_state), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "reason", atlas_verifypolicy_reason_name(r->policy_reason), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(j, "id", r->policy_id[0] != '\0' ? r->policy_id : NULL, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(j, "hash", r->policy_hash[0] != '\0' ? r->policy_hash : NULL,
                                    err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(j, "path", r->policy_path[0] != '\0' ? r->policy_path : NULL,
                                    err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(j, "detail",
                                    r->policy_detail[0] != '\0' ? r->policy_detail : NULL, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(j, "deterministic_enforce", r->deterministic_enforce, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(j, "empirical_enforce", r->empirical_enforce, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "rules", (int64_t)r->rule_count, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(j, err);
    }
    return st;
}

/* The evidence and the attestations, as one shape every surface re-emits.
 *
 * Two pairs of fields here are the ones a reader must not see collapsed, and
 * they are separate keys for that reason rather than for tidiness:
 *
 *   `class` / `producer_identity` — what sort of thing the evidence is, and how
 *   well Atlas knows who produced it. AI_ANALYSIS produced by a SELF_DECLARED
 *   actor and COMPILER evidence ATLAS_ATTESTED are both legitimate rows and
 *   mean entirely different things; a UI that prints only the first is telling
 *   somebody a model is a compiler.
 *
 *   `actor` / `group` — who spoke, and which independent evidence group they
 *   landed in. Two attestations in one group corroborate each other not at all,
 *   however many actors they represent, and printing the actor count as if it
 *   were an evidence count is precisely the error this season exists to stop.
 *
 * Every field either produced by a model or read out of a repository is
 * safe-encoded here. Atlas' own vocabularies and its minted uids are not: they
 * come from closed enums and a fixed alphabet, so they carry no byte anybody
 * else chose. */
atlas_status atlas_service_verify_write_detail(atlas_json *j, atlas_safe_pool *safe,
                                               const atlas_verify_detail *d, atlas_err *err) {
    atlas_status st = atlas_json_key(j, "evidence", err);
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(j, err);
    }
    for (size_t i = 0; st == ATLAS_OK && i < d->evidence_count; i++) {
        const atlas_verify_evidence_detail *e = &d->evidence[i];
        st = atlas_json_obj_begin(j, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "uid", atlas_buf_cstr(&e->uid), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "class", atlas_verify_evidence_class_name(e->cls), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "family", atlas_verify_evidence_family_name(e->family), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "producer_class",
                                    atlas_verify_actor_class_name(e->producer_class), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "producer_identity",
                                    atlas_verify_actor_identity_name(e->producer_identity), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "producer", atlas_buf_cstr(&e->producer_uid), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "producer_name",
                                    atlas_safe(safe, atlas_buf_cstr(&e->producer_name)), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "commit", atlas_buf_cstr(&e->commit_oid), err);
        }
        /* Already stored `%XX`-encoded, so it is emitted as-is. */
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "path", atlas_buf_cstr(&e->path_text), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "symbol", atlas_safe(safe, atlas_buf_cstr(&e->symbol)), err);
        }
        if (st == ATLAS_OK && e->line_start > 0) {
            st = atlas_json_key_int(j, "line_start", e->line_start, err);
        }
        if (st == ATLAS_OK && e->line_end > 0) {
            st = atlas_json_key_int(j, "line_end", e->line_end, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "target", atlas_safe(safe, atlas_buf_cstr(&e->target)), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "observed", atlas_safe(safe, atlas_buf_cstr(&e->observed)),
                                    err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "observed_at", atlas_buf_cstr(&e->observed_at), err);
        }
        if (st == ATLAS_OK && e->tool.len > 0) {
            st = atlas_json_key_str(j, "tool", atlas_safe(safe, atlas_buf_cstr(&e->tool)), err);
        }
        if (st == ATLAS_OK && e->proof_class.len > 0) {
            st = atlas_json_key_str(j, "proof_class", atlas_buf_cstr(&e->proof_class), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_bool(j, "stale", e->stale, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_obj_end(j, err);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "evidence_total", (int64_t)d->evidence_total, err);
    }

    if (st == ATLAS_OK) {
        st = atlas_json_key(j, "attestations", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(j, err);
    }
    for (size_t i = 0; st == ATLAS_OK && i < d->attestation_count; i++) {
        const atlas_verify_attestation_detail *a = &d->attestations[i];
        st = atlas_json_obj_begin(j, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "uid", atlas_buf_cstr(&a->uid), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "verdict", atlas_verify_verdict_name(a->verdict), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "actor", atlas_buf_cstr(&a->actor_uid), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "actor_class", atlas_verify_actor_class_name(a->actor_class),
                                    err);
        }
        /* The honest half of §10: SELF_DECLARED says Atlas took the speaker's
         * word for who it is. Never omitted, because its absence would read as
         * certainty. */
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "actor_identity",
                                    atlas_verify_actor_identity_name(a->actor_identity), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "actor_name",
                                    atlas_safe(safe, atlas_buf_cstr(&a->actor_name)), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "actor_provider",
                                    atlas_safe(safe, atlas_buf_cstr(&a->actor_provider)), err);
        }
        if (st == ATLAS_OK && a->actor_family.len > 0) {
            st = atlas_json_key_str(j, "actor_family",
                                    atlas_safe(safe, atlas_buf_cstr(&a->actor_family)), err);
        }
        if (st == ATLAS_OK && a->actor_version.len > 0) {
            st = atlas_json_key_str(j, "actor_version",
                                    atlas_safe(safe, atlas_buf_cstr(&a->actor_version)), err);
        }
        if (st == ATLAS_OK && a->actor_role.len > 0) {
            st = atlas_json_key_str(j, "actor_role",
                                    atlas_safe(safe, atlas_buf_cstr(&a->actor_role)), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "method", atlas_safe(safe, atlas_buf_cstr(&a->method)), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "scope", atlas_safe(safe, atlas_buf_cstr(&a->scope_note)),
                                    err);
        }
        if (st == ATLAS_OK && a->basis_commit.len > 0) {
            st = atlas_json_key_str(j, "commit", atlas_buf_cstr(&a->basis_commit), err);
        }
        /* The actor's own number, and named so nothing can mistake it for
         * Atlas'. Absent rather than -1 when it did not give one. */
        if (st == ATLAS_OK && a->self_confidence >= 0) {
            st = atlas_json_key_int(j, "self_confidence", a->self_confidence, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_bool(j, "superseded", a->superseded, err);
        }
        if (st == ATLAS_OK && a->group >= 0) {
            st = atlas_json_key_int(j, "group", a->group, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_obj_end(j, err);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "attestation_rows", (int64_t)d->attestation_total, err);
    }
    /* A6's rule, again: a bound that was reached is reported, so a truncated
     * evidence list can never be read as a complete one. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(j, "detail_truncated", d->limit_reached, err);
    }
    return st;
}

atlas_status atlas_service_verify_write_report(atlas_json *j, atlas_safe_pool *safe,
                                               const atlas_verify_report *r, atlas_err *err) {
    atlas_status st = atlas_json_key_str(j, "claim", atlas_buf_cstr(&r->claim_uid), err);
    /* UNTRUSTED_DATA. Verification changes a status, never the nature of bytes:
     * a VERIFIED claim's proposition is exactly as untrusted as a proposed
     * one's, and "Atlas verified this" reads like a warrant for the sentence
     * when it is a statement about a truth condition. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "text", atlas_safe(safe, atlas_buf_cstr(&r->claim_text)), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "domain", atlas_safe(safe, atlas_buf_cstr(&r->domain)), err);
    }
    if (st == ATLAS_OK && r->record_uid.len > 0) {
        st = atlas_json_key_str(j, "decision", atlas_buf_cstr(&r->record_uid), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_service_verify_write_assessment(j, &r->assessment, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_service_verify_write_detail(j, safe, &r->detail, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_service_verify_write_policy(j, r, err);
    }
    return st;
}

/* --- A9.2.1: listing claims -------------------------------------------------
 *
 * The read that makes the rest usable: without it a client that created a claim
 * and lost the response has no way to find it again, which would make the
 * idempotency work pointless in practice.
 *
 * Bounded, and the bound is reported. A3's rule about `candidate_count`: a
 * partial list must never read as a complete one. */
typedef struct claims_ctx {
    atlas_json *j;
    atlas_safe_pool *safe;
    atlas_status st;
    size_t emitted;
} claims_ctx;

static atlas_status claim_row(const atlas_verify_claim *c, void *vctx, atlas_err *err) {
    claims_ctx *cc = (claims_ctx *)vctx;
    atlas_status st = atlas_json_obj_begin(cc->j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(cc->j, "claim", atlas_buf_cstr(&c->uid), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(cc->j, "text", atlas_safe(cc->safe, atlas_buf_cstr(&c->text)), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(cc->j, "domain", atlas_safe(cc->safe, atlas_buf_cstr(&c->domain)),
                                err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(cc->j, "scope", atlas_safe(cc->safe, atlas_buf_cstr(&c->scope_note)),
                                err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(cc->j, "semantics", atlas_verify_claim_semantics_name(c->semantics),
                                err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(cc->j, "verifier",
                                    c->verifier.len > 0 ? atlas_buf_cstr(&c->verifier) : NULL, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(cc->j, "commit",
                                    c->basis_commit.len > 0 ? atlas_buf_cstr(&c->basis_commit)
                                                            : NULL,
                                    err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(cc->j, "created_at", atlas_buf_cstr(&c->created_at), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(cc->j, err);
    }
    cc->emitted++;
    return st;
}

atlas_status atlas_service_verify_claims_on(atlas_db *db, atlas_json *j, atlas_safe_pool *safe,
                                            int64_t repo_id, const char *decision_uid,
                                            int64_t limit, atlas_err *err) {
    if (db == NULL) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG, "no index is available to read");
    }
    int64_t document_id = 0;
    int64_t revision_id = 0;
    if (decision_uid != NULL && *decision_uid != '\0') {
        bool found = false;
        int64_t doc_repo = 0;
        atlas_status fst =
            atlas_db_decision_find_uid(db, decision_uid, &document_id, &doc_repo, &found, err);
        if (fst == ATLAS_OK && !found) {
            fst = atlas_err_set(err, ATLAS_ERR_CONFIG, "no knowledge record by that id exists");
        }
        if (fst == ATLAS_OK) {
            fst = atlas_db_decision_approved_revision(db, document_id, &revision_id, err);
        }
        if (fst != ATLAS_OK) {
            return fst;
        }
    }

    claims_ctx cc = {j, safe, ATLAS_OK, 0};
    atlas_status st = atlas_json_key(j, "claims", err);
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(j, err);
    }
    bool truncated = false;
    if (st == ATLAS_OK) {
        st = atlas_db_verify_claims_for_repo(db, repo_id, document_id, revision_id, limit,
                                             claim_row, &cc, &truncated, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "count", (int64_t)cc.emitted, err);
    }
    /* Reported, always. A6's rule: a bound that is reached is reported, and a
     * truncated list can never read as a complete one. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(j, "truncated", truncated, err);
    }
    return st;
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
