/* Atlas - the decision lifecycle state machine and the operator channel.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * `atlas_decision_apply_in_tx` is **the only function in Atlas that writes a
 * lifecycle transition**, the same way `settle()` in resolve.c is the only
 * function that writes a resolution and `atlas_db_evidence_insert` is the only
 * one that writes evidence.
 *
 * Two functions call it, and there are no others:
 *
 *   - `atlas_decision_apply` (below), the public entry point. It adds `BEGIN`,
 *     `COMMIT` and rollback and nothing else — no validation of its own, no
 *     second copy of a rule.
 *   - `op_decision_locked` in `src/ai/ai.c`, the A2 `atlas_record_decision`
 *     bridge, which must write the A2 row and the A4 document it maps to in one
 *     transaction or write neither, and so already owns the transaction.
 *
 * The reason for two entry points rather than a nested transaction is in the
 * comment on `atlas_decision_apply_in_tx`: `atlas_db_begin` counts depth, but
 * rollback does not, so a failure inside a nested transition would discard the
 * caller's work silently. The boundary belongs to whoever owns the whole unit.
 *
 * Everything the phase claims — that no capability Atlas exposes to a model
 * approves a proposal, that an approved revision is immutable, that a rejected
 * one never becomes approved, that approving a replacement supersedes its
 * predecessor atomically, that a challenge is spent exactly once — is enforced
 * in `atlas_decision_apply_in_tx`, once, and would be bypassable if a caller
 * could reach the tables without going through it.
 *
 * Note the shape of the first claim, which is deliberately narrow. A model with
 * shell access can drive a pseudo-terminal and run the CLI, and Atlas cannot
 * tell that invocation from a person's. What is true, and what this file
 * enforces, is that nothing Atlas hands a model — no MCP tool, no hook, no
 * AI-facing method, no request argument — grants an approval. Anything broader
 * would be a claim about the whole machine rather than about Atlas.
 *
 * The order of the checks in `transition()` is deliberate and is the reason
 * this file reads as it does:
 *
 *   1. the capability, because without one nothing may proceed at all;
 *   2. what the capability is bound to, because a valid capability for a
 *      different revision is not a capability for this one;
 *   3. the content hash, because approval binds to bytes rather than to a row;
 *   4. the transition table, because a legal actor may still be asking for an
 *      illegal transition;
 *   5. the write, conditional on the state the check observed, because between
 *      (4) and (5) another writer could have moved it — and it must lose
 *      deterministically rather than be overwritten.
 *
 * Doing (5) unconditionally after (4) would be the classic
 * check-then-act: two approvals of one proposal would both pass step 4 and both
 * write, and the second would silently win. `atlas_db_decision_revision_set_state`
 * carries the expected state in its WHERE clause so that cannot happen.
 */
#include "atlas/decision_ops.h"
#include "atlas/verify.h"

#include <stdio.h>
#include <string.h>

#include "atlas/ai.h"
#include "atlas/atlas.h"
#include "atlas/decision_remote.h"
#include "atlas/safetext.h"
#include "gate/gate_internal.h"

const char *atlas_decision_op_kind_name(atlas_decision_op_kind k) {
    switch (k) {
    case ATLAS_DECISION_OP_PROPOSE: return "propose";
    case ATLAS_DECISION_OP_REVISE: return "revise";
    case ATLAS_DECISION_OP_CHALLENGE: return "challenge";
    case ATLAS_DECISION_OP_APPROVE: return "approve";
    case ATLAS_DECISION_OP_REJECT: return "reject";
    case ATLAS_DECISION_OP_SUPERSEDE: return "supersede";
    case ATLAS_DECISION_OP_PROMOTE: return "promote";
    case ATLAS_DECISION_OP_REVALIDATE: return "revalidate";
    case ATLAS_DECISION_OP_EDGE_NOTE: return "edge note";
    case ATLAS_DECISION_OP_RESOLVE: return "resolve";
    case ATLAS_DECISION_OP_AUTO_APPROVE: return "policy approve";
    case ATLAS_DECISION_OP_AUTO_RESOLVE: return "policy resolve";
    }
    return "propose";
}

bool atlas_decision_op_needs_challenge(atlas_decision_op_kind k) {
    /* Asked by `atlas_decision_apply` rather than remembered at each call site,
     * so that adding an operation kind forces a decision about it here instead
     * of defaulting it into the unauthenticated set. */
    return k == ATLAS_DECISION_OP_APPROVE || k == ATLAS_DECISION_OP_REJECT ||
           k == ATLAS_DECISION_OP_SUPERSEDE || k == ATLAS_DECISION_OP_REVALIDATE ||
           k == ATLAS_DECISION_OP_RESOLVE;
}

bool atlas_decision_op_is_machine(atlas_decision_op_kind k) {
    return k == ATLAS_DECISION_OP_AUTO_APPROVE || k == ATLAS_DECISION_OP_AUTO_RESOLVE;
}

void atlas_decision_op_init(atlas_decision_op *op, atlas_decision_op_kind kind) {
    memset(op, 0, sizeof(*op));
    op->kind = kind;
    atlas_buf_init(&op->repo_name);
    atlas_buf_init(&op->root);
    atlas_buf_init(&op->uid);
    atlas_buf_init(&op->replacement_uid);
    atlas_decision_revision_init(&op->revision);
    atlas_buf_init(&op->provider);
    atlas_buf_init(&op->client);
    atlas_buf_init(&op->session_key);
    atlas_buf_init(&op->dedup_key);
    atlas_buf_init(&op->token);
    atlas_buf_init(&op->confirmation);
    atlas_buf_init(&op->remote_token);
    atlas_buf_init(&op->prior_freshness);
    atlas_buf_init(&op->prior_reasons);
    atlas_buf_init(&op->edge_target_uid);
    atlas_buf_init(&op->edge_event);
    atlas_buf_init(&op->edge_note);
    atlas_buf_init(&op->edge_provenance);
}

void atlas_decision_op_free(atlas_decision_op *op) {
    if (op == NULL) {
        return;
    }
    atlas_buf_free(&op->repo_name);
    atlas_buf_free(&op->root);
    atlas_buf_free(&op->uid);
    atlas_buf_free(&op->replacement_uid);
    atlas_decision_revision_free(&op->revision);
    atlas_buf_free(&op->provider);
    atlas_buf_free(&op->client);
    atlas_buf_free(&op->session_key);
    atlas_buf_free(&op->dedup_key);
    atlas_buf_free(&op->token);
    atlas_buf_free(&op->confirmation);
    /* A16. A credential in a struct is wiped, not merely freed --
     * `gateway.c`'s wipe of the login key is the precedent. `op->remote_token`
     * held a presented bearer token, and `atlas_buf_free` alone would only
     * return the allocation to the heap with the secret still in it. */
    if (op->remote_token.data != NULL) {
        volatile unsigned char *z = (volatile unsigned char *)op->remote_token.data;
        for (size_t i = 0; i < op->remote_token.cap; i++) {
            z[i] = 0;
        }
    }
    atlas_buf_free(&op->remote_token);
    atlas_buf_free(&op->prior_freshness);
    atlas_buf_free(&op->prior_reasons);
    atlas_buf_free(&op->edge_target_uid);
    atlas_buf_free(&op->edge_event);
    atlas_buf_free(&op->edge_note);
    atlas_buf_free(&op->edge_provenance);
}

void atlas_decision_result_init(atlas_decision_result *r) {
    memset(r, 0, sizeof(*r));
    atlas_buf_init(&r->repo_name);
    atlas_buf_init(&r->root_text);
    atlas_buf_init(&r->uid);
    atlas_buf_init(&r->token);
    atlas_buf_init(&r->title);
    atlas_buf_init(&r->replaced_by_uid);
    r->state = ATLAS_DECISION_PROPOSED;
}

void atlas_decision_result_free(atlas_decision_result *r) {
    if (r == NULL) {
        return;
    }
    atlas_buf_free(&r->repo_name);
    atlas_buf_free(&r->root_text);
    atlas_buf_free(&r->uid);
    atlas_buf_free(&r->token);
    atlas_buf_free(&r->title);
    atlas_buf_free(&r->replaced_by_uid);
}

/* --- the searchable projection --------------------------------------------
 *
 * Lowercased ASCII-wise and bounded. The lowercasing is deliberately not
 * locale-aware: a search whose results depend on LC_CTYPE gives two answers on
 * two machines to one question. */
atlas_status atlas_decision_haystack(const atlas_decision_revision *r, atlas_buf *out,
                                     atlas_err *err) {
    atlas_buf_reset(out);
    const atlas_buf *parts[] = {
        &r->title, &r->decision_text, &r->rationale_text, &r->context_text, &r->consequences_text,
    };
    atlas_status st = ATLAS_OK;
    for (size_t p = 0; st == ATLAS_OK && p < sizeof(parts) / sizeof(parts[0]); p++) {
        if (out->len >= ATLAS_DECISION_HAYSTACK_MAX) {
            break;
        }
        if (out->len > 0) {
            st = atlas_buf_append_ch(out, ' ', err);
        }
        for (size_t i = 0; st == ATLAS_OK && i < parts[p]->len; i++) {
            if (out->len >= ATLAS_DECISION_HAYSTACK_MAX) {
                break;
            }
            char c = parts[p]->data[i];
            if (c >= 'A' && c <= 'Z') {
                c = (char)(c - 'A' + 'a');
            }
            if (c == '\n' || c == '\t') {
                c = ' ';
            }
            st = atlas_buf_append_ch(out, c, err);
        }
    }
    for (size_t a = 0; st == ATLAS_OK && a < r->alternative_count; a++) {
        if (out->len >= ATLAS_DECISION_HAYSTACK_MAX) {
            break;
        }
        st = atlas_buf_append_ch(out, ' ', err);
        for (size_t i = 0; st == ATLAS_OK && i < r->alternatives[a].len; i++) {
            if (out->len >= ATLAS_DECISION_HAYSTACK_MAX) {
                break;
            }
            char c = r->alternatives[a].data[i];
            if (c >= 'A' && c <= 'Z') {
                c = (char)(c - 'A' + 'a');
            }
            if (c == '\n' || c == '\t') {
                c = ' ';
            }
            st = atlas_buf_append_ch(out, c, err);
        }
    }
    return st;
}

/* --- context -------------------------------------------------------------- */

typedef struct apply_ctx {
    atlas_db *db;
    atlas_repo_info repo;
    bool repo_found;
    char now[ATLAS_TS_MAX];
    char root_hash[ATLAS_SHA256_HEX_LEN + 1u];
    /* The repository's durable identity — root path, object format and ingested
     * root commits. Part of the canonical content, so it is resolved once here
     * rather than per revision. Empty when the lineage is not yet known. */
    atlas_buf repo_identity;
    int64_t session_id;
    /* A16. Set only when `op->channel == ATLAS_DECISION_CHANNEL_REMOTE`, by
     * `atlas_decision_remote_verify` at the top of `atlas_decision_apply_in_tx`
     * -- the *verified* credential id, never the id a request merely named.
     * Empty on every LOCAL operation. */
    char key_id[ATLAS_APIKEY_SELECTOR_HEX + 1u];
} apply_ctx;

static atlas_status resolve_repo(apply_ctx *ac, const atlas_decision_op *op, atlas_err *err) {
    ac->repo_found = false;
    if (op->repo_name.len > 0) {
        atlas_status st = atlas_db_repo_get(ac->db, atlas_buf_cstr(&op->repo_name), &ac->repo,
                                            &ac->repo_found, err);
        if (st != ATLAS_OK) {
            return st;
        }
    } else if (op->root.len > 0) {
        atlas_status st = atlas_db_repo_get_containing(ac->db, op->root.data, op->root.len,
                                                       &ac->repo, &ac->repo_found, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    if (!ac->repo_found) {
        return atlas_err_set(err, ATLAS_ERR_REPO,
                             "no registered Atlas repository matches this request; register it "
                             "with `atlas repo add` before recording decisions about it");
    }
    /* The same hash the automatic context envelope reports, over the same raw
     * bytes, so a document's durable identity and the identity a consumer sees
     * are one value rather than two that must be kept equal. */
    atlas_sha256_hex(ac->repo.root_path.data, ac->repo.root_path.len, ac->root_hash);
    /* The durable identity, which is what a later re-registration is checked
     * against. A path hash says "same directory"; this says "same repository".
     * Empty is a real answer and is recorded as one. */
    return atlas_db_repo_identity_hash(ac->db, ac->repo.id, &ac->repo_identity, err);
}

/* A2's attribution rule, unchanged and reused rather than reimplemented.
 *
 * A session is found by `(provider, client, session_key)` and by nothing else.
 * A repository never identifies a session. A record that cannot be attached
 * exactly is stored sessionless with a typed reason, because a gap is
 * repairable and a wrong attribution is not.
 *
 * A decision proposal additionally requires the session to be *open*, for the
 * same reason a reason record does: it is content about a conversation, and
 * binding it to one that has ended is a claim Atlas cannot support. After
 * `/clear` the MCP server still holds the old id, so this is what turns a
 * post-clear proposal into an honest gap. */
static atlas_status bind_session(apply_ctx *ac, const atlas_decision_op *op,
                                 atlas_decision_result *out, atlas_err *err) {
    ac->session_id = 0;
    if (op->session_key.len == 0) {
        out->session_unbound = true;
        out->unbound_reason = ATLAS_AI_UNBOUND_NO_SESSION_ID;
        return ATLAS_OK;
    }
    int64_t client_id = 0;
    const char *provider = op->provider.len > 0 ? atlas_buf_cstr(&op->provider) : "unknown";
    const char *client = op->client.len > 0 ? atlas_buf_cstr(&op->client) : "unknown";
    /* The read-only half: asking a question must not create a client row. */
    atlas_status st = atlas_db_ai_client_find(ac->db, provider, client, &client_id, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (client_id == 0) {
        out->session_unbound = true;
        out->unbound_reason = ATLAS_AI_UNBOUND_UNKNOWN_SESSION;
        return ATLAS_OK;
    }
    bool open = false;
    st = atlas_db_ai_session_find_state(ac->db, client_id, atlas_buf_cstr(&op->session_key),
                                        &ac->session_id, &open, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (ac->session_id == 0) {
        out->session_unbound = true;
        out->unbound_reason = ATLAS_AI_UNBOUND_UNKNOWN_SESSION;
        return ATLAS_OK;
    }
    if (!open) {
        ac->session_id = 0;
        out->session_unbound = true;
        out->unbound_reason = ATLAS_AI_UNBOUND_SESSION_CLOSED;
    }
    return ATLAS_OK;
}

/* --- writing a revision ---------------------------------------------------- */

/* Writes one revision, its alternatives, its links, its search projection and
 * its PROPOSED ledger event.
 *
 * The content hash is computed here from the struct rather than taken from a
 * caller: a hash a request supplies is a hash a request chooses, and approval
 * binds to it. */
static atlas_status write_revision(apply_ctx *ac, const atlas_decision_op *op,
                                   atlas_decision_revision *rev, int64_t document_id,
                                   int64_t revision_no, atlas_decision_result *out,
                                   atlas_err *err) {
    rev->document_id = document_id;
    rev->revision_no = revision_no;
    (void)snprintf(rev->created_at, sizeof(rev->created_at), "%s", ac->now);
    rev->session_id = ac->session_id;
    rev->session_unbound = out->session_unbound;
    if (out->unbound_reason != NULL) {
        atlas_status sst = atlas_buf_set_str(&rev->unbound_reason, out->unbound_reason, err);
        if (sst != ATLAS_OK) {
            return sst;
        }
    }
    /* The basis commit is what Atlas last scanned, when it scanned anything.
     * Empty is a real answer and is stored as one: inventing a HEAD for a
     * proposal made against an unscanned worktree would put a false claim in
     * the durable record to avoid an empty column. */
    if (rev->basis_head.len == 0 && ac->repo.scanned_head[0] != '\0') {
        atlas_status sst = atlas_buf_set_str(&rev->basis_head, ac->repo.scanned_head, err);
        if (sst != ATLAS_OK) {
            return sst;
        }
    }
    /* Both are hashed, so both must be set before the digest is taken. */
    {
        atlas_status sst = atlas_buf_set(&rev->basis_repo_identity, ac->repo_identity.data,
                                         ac->repo_identity.len, err);
        if (sst != ATLAS_OK) {
            return sst;
        }
    }

    atlas_status st = atlas_decision_revision_validate(rev, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_decision_content_hash(rev, rev->content_hash, err);
    if (st != ATLAS_OK) {
        return st;
    }

    int64_t rev_id = 0;
    bool duplicate = false;
    st = atlas_db_decision_revision_insert(
        ac->db, rev, op->dedup_key.len > 0 ? atlas_buf_cstr(&op->dedup_key) : NULL, &rev_id,
        &duplicate, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (duplicate) {
        /* A retry. Report the row that already exists rather than creating a
         * second revision that says the same thing: rule 11 is that repeated
         * requests and hook retries are idempotent, and a document that gains a
         * revision every time a hook is redelivered is a document nobody can
         * read. */
        out->duplicate = true;
        out->revision_id = rev_id;
        /* Loaded by the id the dedup key matched, not by "the newest revision".
         *
         * Those are the same row only when nothing was added since the first
         * delivery, and a redelivered hook is exactly the case where something
         * might have been — so reporting the newest revision's number and hash
         * against the deduped revision's id would hand the caller a triple that
         * describes no revision at all. */
        atlas_decision_revision existing;
        atlas_decision_revision_init(&existing);
        bool found = false;
        atlas_status lst = atlas_db_decision_revision_load(ac->db, rev_id, &existing, &found, err);
        if (lst == ATLAS_OK && found) {
            out->revision_no = existing.revision_no;
            out->state = existing.state;
            (void)snprintf(out->content_hash, sizeof(out->content_hash), "%s",
                           existing.content_hash);
        }
        atlas_decision_revision_free(&existing);
        return lst;
    }

    for (size_t i = 0; st == ATLAS_OK && i < rev->alternative_count; i++) {
        st = atlas_db_decision_alternative_add(ac->db, rev_id, (int64_t)i,
                                               rev->alternatives[i].data, rev->alternatives[i].len,
                                               err);
    }
    for (size_t i = 0; st == ATLAS_OK && i < rev->link_count; i++) {
        atlas_decision_link *l = &rev->links[i];
        int64_t target = 0;
        /* Every kind that names a document resolves the same way: the target
         * must exist and must be in this repository. `relates_to` is included
         * because those two checks are about the *reference*, not about what
         * the reference means — a relation to a document Atlas does not hold is
         * a dangling pointer whatever it is called. */
        if (l->kind == ATLAS_DECISION_LINK_SUPERSEDES ||
            l->kind == ATLAS_DECISION_LINK_REPLACED_BY ||
            l->kind == ATLAS_DECISION_LINK_RELATES_TO) {
            int64_t trepo = 0;
            bool found = false;
            st = atlas_db_decision_find_uid(ac->db, atlas_buf_cstr(&l->target_uid), &target, &trepo,
                                            &found, err);
            if (st == ATLAS_OK && !found) {
                st = atlas_err_set(err, ATLAS_ERR_USAGE,
                                   "a decision link names a document Atlas does not hold");
            }
            /* Rule 7: a link between documents may not cross repositories.
             * Two repositories' decisions are two policies, and a link that
             * crossed would let a supersession in one silently retire a
             * decision in the other, and a relation across them would assert a
             * connection between two policies nobody agreed to. */
            if (st == ATLAS_OK && trepo != ac->repo.id) {
                st = atlas_err_set(err, ATLAS_ERR_USAGE,
                                   "a decision may only link to another decision in the same "
                                   "repository");
            }
        }
        if (st == ATLAS_OK) {
            st = atlas_db_decision_link_add(ac->db, rev_id, l, target, ac->now, err);
        }
    }
    if (st == ATLAS_OK) {
        atlas_buf hay = ATLAS_BUF_INIT;
        st = atlas_decision_haystack(rev, &hay, err);
        if (st == ATLAS_OK) {
            st = atlas_db_decision_search_put(ac->db, rev_id, document_id, ac->repo.id, hay.data,
                                              hay.len, err);
        }
        atlas_buf_free(&hay);
    }
    if (st == ATLAS_OK) {
        /* No PROPOSED event names a credential: A16's remote channel disposes
         * of an existing revision, it does not author one, so `key_id` is
         * NULL here and at every call site this season did not touch. */
        st = atlas_db_decision_event_append(
            ac->db, document_id, rev_id, revision_no, "PROPOSED",
            atlas_decision_actor_name(rev->proposed_by), rev->content_hash, 0, 0, 0,
            revision_no == 1 ? "the first revision of this decision"
                             : "a new revision, proposed rather than applied to the approved one",
            NULL, op->dedup_key.len > 0 ? atlas_buf_cstr(&op->dedup_key) : NULL, NULL, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_decision_document_note_revision(ac->db, document_id, revision_no, ac->now,
                                                      err);
    }
    if (st != ATLAS_OK) {
        return st;
    }
    out->revision_id = rev_id;
    out->revision_no = revision_no;
    out->state = ATLAS_DECISION_PROPOSED;
    (void)snprintf(out->content_hash, sizeof(out->content_hash), "%s", rev->content_hash);
    return ATLAS_OK;
}

/* --- propose and revise ----------------------------------------------------- */

/* Deep-copies the op's payload into a revision the write path may fill in.
 *
 * The op is const because the IPC layer built and validated it, and the write
 * path assigns derived fields — the created timestamp, the session binding, the
 * basis head, the content hash. Copying rather than casting away const keeps
 * "what was requested" and "what was written" as two objects, which is what
 * lets the dedup probe below hash the request without disturbing it. */
static atlas_status copy_link(atlas_decision_revision *dst, const atlas_decision_link *src,
                              atlas_err *err) {
    atlas_decision_link l;
    atlas_decision_link_init(&l, src->kind);
    l.symbol_line = src->symbol_line;
    l.change_set_id = src->change_set_id;
    l.analyzer_version = src->analyzer_version;
    struct {
        atlas_buf *to;
        const atlas_buf *from;
    } fields[] = {
        {&l.path_raw, &src->path_raw},
        {&l.path_text, &src->path_text},
        {&l.commit_oid, &src->commit_oid},
        {&l.target_uid, &src->target_uid},
        {&l.symbol_name, &src->symbol_name},
        {&l.symbol_name_text, &src->symbol_name_text},
        {&l.symbol_kind, &src->symbol_kind},
        {&l.basis_commit, &src->basis_commit},
        {&l.file_content_hash, &src->file_content_hash},
        {&l.analyzer_name, &src->analyzer_name},
    };
    atlas_status st = ATLAS_OK;
    for (size_t i = 0; st == ATLAS_OK && i < sizeof(fields) / sizeof(fields[0]); i++) {
        st = atlas_buf_set(fields[i].to, fields[i].from->data, fields[i].from->len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_decision_revision_add_link(dst, &l, err);
    }
    atlas_decision_link_free(&l);
    return st;
}

static atlas_status copy_revision(atlas_decision_revision *dst, const atlas_decision_revision *src,
                                  atlas_err *err) {
    struct {
        atlas_buf *to;
        const atlas_buf *from;
    } fields[] = {
        {&dst->title, &src->title},
        {&dst->context_text, &src->context_text},
        {&dst->decision_text, &src->decision_text},
        {&dst->rationale_text, &src->rationale_text},
        {&dst->consequences_text, &src->consequences_text},
        {&dst->basis_head, &src->basis_head},
    };
    atlas_status st = ATLAS_OK;
    for (size_t i = 0; st == ATLAS_OK && i < sizeof(fields) / sizeof(fields[0]); i++) {
        st = atlas_buf_set(fields[i].to, fields[i].from->data, fields[i].from->len, err);
    }
    for (size_t i = 0; st == ATLAS_OK && i < src->alternative_count; i++) {
        st = atlas_buf_set(&dst->alternatives[i], src->alternatives[i].data,
                           src->alternatives[i].len, err);
        if (st == ATLAS_OK) {
            dst->alternative_count++;
        }
    }
    for (size_t i = 0; st == ATLAS_OK && i < src->link_count; i++) {
        st = copy_link(dst, &src->links[i], err);
    }
    dst->scope = src->scope;
    dst->proposed_by = src->proposed_by;
    dst->imported_from_ai_decision_id = src->imported_from_ai_decision_id;
    return st;
}

/* The actor restriction, at the one place a revision is created.
 *
 * A proposal that arrived over IPC without consuming a challenge may only claim
 * to be a model, whatever it says about itself. The operator proposing through
 * the CLI is not an exception: recording an operator's *proposal* as
 * LOCAL_OPERATOR_CONFIRMED would make that name mean two things — "somebody
 * typed this" and "somebody accepted this" — which is the exact conflation the
 * phase exists to prevent. An operator's proposal becomes effective by being
 * approved, like everyone else's. */
static atlas_status check_proposer(const atlas_decision_revision *r, atlas_err *err) {
    if (atlas_decision_actor_writable_by_adapter(r->proposed_by)) {
        return ATLAS_OK;
    }
    return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                         "a decision revision may be proposed as MODEL_PROPOSAL or "
                         "MODEL_INFERENCE and as nothing else; approval is a separate, "
                         "operator-only act and cannot be asserted by a proposer");
}

static atlas_status op_propose(apply_ctx *ac, const atlas_decision_op *op,
                               atlas_decision_result *out, atlas_err *err) {
    atlas_status st = check_proposer(&op->revision, err);
    if (st != ATLAS_OK) {
        return st;
    }
    atlas_decision_revision rev;
    atlas_decision_revision_init(&rev);
    st = copy_revision(&rev, &op->revision, err);

    int64_t document_id = 0;
    char uid[ATLAS_DECISION_UID_MAX];
    if (st == ATLAS_OK) {
        /* A9.1: the kind is decided here, once, and written by the INSERT that
         * creates the document. `op->knowledge_kind` is DECISION unless a caller
         * asked for something else, which is what makes every client written
         * before this vocabulary existed keep working unchanged. */
        st = atlas_db_decision_document_create(ac->db, ac->repo.id, ac->root_hash,
                                               op->knowledge_kind, ac->now, &document_id, uid,
                                               sizeof(uid), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&out->uid, uid, err);
    }
    if (st == ATLAS_OK) {
        out->document_id = document_id;
        out->document_created = true;
        out->knowledge_kind = op->knowledge_kind;
        st = write_revision(ac, op, &rev, document_id, 1, out, err);
    }
    atlas_decision_revision_free(&rev);
    return st;
}

/* The content hash of a request, without disturbing the request.
 *
 * Borrows the op's buffers into a stack revision, hashes it, and zeroes the
 * borrower before it goes out of scope so that nothing is freed twice. The
 * alternative — a full deep copy just to compute a digest — would allocate the
 * whole payload again on every retry, which is the case this exists to make
 * cheap. */
static atlas_status hash_request(const atlas_decision_revision *src, const apply_ctx *ac,
                                 const char *basis_head, char *hex_out, atlas_err *err) {
    atlas_decision_revision probe;
    atlas_decision_revision_init(&probe);
    /* The probe has to hash exactly what the write path will, or the
     * content-hash idempotency check compares two different things and every
     * retry looks like a change. Both derived fields are borrowed like the
     * rest. */
    atlas_buf basis = ATLAS_BUF_INIT;
    if (src->basis_head.len > 0) {
        probe.basis_head = src->basis_head;
    } else if (basis_head != NULL && basis_head[0] != '\0') {
        basis.data = (char *)(uintptr_t)basis_head;
        basis.len = strlen(basis_head);
        probe.basis_head = basis;
    }
    probe.basis_repo_identity = ac->repo_identity;
    probe.proposed_by = src->proposed_by;
    probe.title = src->title;
    probe.context_text = src->context_text;
    probe.decision_text = src->decision_text;
    probe.rationale_text = src->rationale_text;
    probe.consequences_text = src->consequences_text;
    probe.scope = src->scope;
    probe.alternative_count = src->alternative_count;
    for (size_t i = 0; i < src->alternative_count && i < ATLAS_DECISION_MAX_ALTERNATIVES; i++) {
        probe.alternatives[i] = src->alternatives[i];
    }
    probe.link_count = src->link_count;
    for (size_t i = 0; i < src->link_count && i < ATLAS_DECISION_MAX_LINKS; i++) {
        probe.links[i] = src->links[i];
    }
    atlas_status st = atlas_decision_content_hash(&probe, hex_out, err);
    memset(&probe, 0, sizeof(probe)); /* every buffer was borrowed; own none */
    return st;
}

/* --- the durable account of an edge (migration 10) -------------------------
 *
 * One helper, called from `op_revise` inside the transaction that writes the
 * revision and from `op_edge_note` inside its own. There is no third caller and
 * no path to `decision_edge_events` that does not come through here, which is
 * the rule every other Atlas write point follows.
 *
 * The note is prose, so it is checked for the one confusion that has already
 * cost this project a repair: a rationale that is itself a decision id. That
 * was the A8.2 defect — two meanings on one key — and refusing it structurally
 * is cheaper than detecting it afterwards. */
static atlas_status write_edge_note(apply_ctx *ac, const atlas_decision_op *op,
                                    int64_t source_document_id, int64_t revision_id,
                                    const char *default_event, atlas_err *err) {
    const char *target = atlas_buf_cstr(&op->edge_target_uid);
    if (target == NULL || *target == '\0') {
        return ATLAS_OK; /* no account carried; every op before this one */
    }
    const char *note = atlas_buf_cstr(&op->edge_note);
    if (note == NULL || *note == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "an edge note needs a reason; relating two decisions without "
                             "saying why is what this record exists to prevent");
    }
    if (strlen(note) > ATLAS_DECISION_EDGE_NOTE_MAX) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "an edge note is at most %d bytes",
                             ATLAS_DECISION_EDGE_NOTE_MAX);
    }
    /* The A8.2 guard, applied to the one new prose field. A note that is a
     * document id is a caller that has confused the explanation with the thing
     * being explained. */
    if (atlas_decision_uid_is_valid(note)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "that note is a decision id, not a reason; the note says why the "
                             "relation exists and the target says what it points at");
    }
    if (!atlas_decision_uid_is_valid(target)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "that is not a decision id");
    }

    int64_t target_id = 0, target_repo = 0;
    bool found = false;
    atlas_status st =
        atlas_db_decision_find_uid(ac->db, target, &target_id, &target_repo, &found, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (!found) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "no decision has that id");
    }
    /* Cross-repository isolation, checked here as well as at the edge that
     * built the op: this is the write point, and a write point that trusts its
     * caller is not one. */
    if (target_repo != ac->repo.id) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "that decision belongs to a different repository");
    }
    if (target_id == source_document_id) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a decision cannot relate to itself");
    }

    const char *event = atlas_buf_cstr(&op->edge_event);
    if (event == NULL || *event == '\0') {
        event = default_event;
    }
    if (strcmp(event, ATLAS_DECISION_EDGE_EVENT_ADDED) != 0 &&
        strcmp(event, ATLAS_DECISION_EDGE_EVENT_ANNOTATED) != 0 &&
        strcmp(event, ATLAS_DECISION_EDGE_EVENT_REMOVED) != 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "unknown edge event '%s'", event);
    }
    const char *prov = atlas_buf_cstr(&op->edge_provenance);
    if (prov == NULL || *prov == '\0') {
        prov = "UNKNOWN";
    }
    if (strcmp(prov, "OPERATOR") != 0 && strcmp(prov, "D1_MANIFEST") != 0 &&
        strcmp(prov, "D3_REPAIR") != 0 && strcmp(prov, "UNKNOWN") != 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "unknown edge provenance '%s'", prov);
    }

    return atlas_db_decision_edge_event_append(ac->db, source_document_id, target_id,
                                               atlas_decision_link_kind_name(
                                                   ATLAS_DECISION_LINK_RELATES_TO),
                                               event, note, prov, revision_id, err);
}

/* Attach an explanation to an edge that already exists.
 *
 * Writes one append-only row and nothing else: no revision, so no content hash
 * moves and no approval is disturbed. This is what makes it possible to explain
 * the relations of an already-approved decision without proposing anything —
 * which is the only honest way to do it, because a rationale written after an
 * approval was not part of what was approved. */
static atlas_status op_edge_note(apply_ctx *ac, const atlas_decision_op *op,
                                 atlas_decision_result *out, atlas_err *err) {
    int64_t document_id = 0, doc_repo = 0;
    bool found = false;
    atlas_status st = atlas_db_decision_find_uid(ac->db, atlas_buf_cstr(&op->uid), &document_id,
                                                 &doc_repo, &found, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (!found) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "no decision has that id");
    }
    if (doc_repo != ac->repo.id) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "that decision belongs to a different repository");
    }
    st = write_edge_note(ac, op, document_id, 0, ATLAS_DECISION_EDGE_EVENT_ANNOTATED, err);
    if (st != ATLAS_OK) {
        return st;
    }
    out->document_id = document_id;
    /* The status is reported unchanged, because it is unchanged. */
    int64_t rev_id = 0, rev_no = 0;
    char hash[ATLAS_SHA256_HEX_LEN + 1u];
    char state[16];
    if (atlas_db_decision_latest_revision(ac->db, document_id, &rev_id, &rev_no, hash, sizeof(hash),
                                          state, sizeof(state), err) == ATLAS_OK) {
        out->revision_id = rev_id;
        out->revision_no = rev_no;
        (void)snprintf(out->content_hash, sizeof(out->content_hash), "%s", hash);
        (void)atlas_decision_state_parse(state, &out->state);
    }
    return atlas_db_decision_uid_of(ac->db, document_id, &out->uid, err);
}

static atlas_status op_revise(apply_ctx *ac, const atlas_decision_op *op,
                              atlas_decision_result *out, atlas_err *err) {
    atlas_status st = check_proposer(&op->revision, err);
    if (st != ATLAS_OK) {
        return st;
    }
    int64_t document_id = 0;
    int64_t doc_repo = 0;
    bool found = false;
    st = atlas_db_decision_find_uid(ac->db, atlas_buf_cstr(&op->uid), &document_id, &doc_repo,
                                    &found, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (!found) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "no decision has that id");
    }
    if (doc_repo != ac->repo.id) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "that decision belongs to a different repository");
    }

    /* **A9.1: a revision cannot reclassify a document.**
     *
     * The kind is a property of the durable record, so "revise it into an
     * invariant" is a request to change what a record has always been, and the
     * honest form of that is a new record of the right kind that supersedes this
     * one — which keeps the history of how the knowledge used to be classified
     * instead of quietly rewriting it.
     *
     * A caller that said nothing is not asserting DECISION: `knowledge_kind_given`
     * is what separates the two, so a client that has never heard of kinds can
     * still revise a POLICY. */
    {
        atlas_decision_kind kind = ATLAS_DECISION_KIND_DECISION;
        bool kfound = false;
        st = atlas_db_decision_kind_of(ac->db, document_id, &kind, &kfound, err);
        if (st != ATLAS_OK) {
            return st;
        }
        if (op->knowledge_kind_given && op->knowledge_kind != kind) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "this record's kind is %s and a revision cannot change that; "
                                 "propose a record of kind %s and supersede this one with it",
                                 atlas_decision_kind_name(kind),
                                 atlas_decision_kind_name(op->knowledge_kind));
        }
        out->knowledge_kind = kind;
    }

    int64_t last_id = 0, last_no = 0;
    char last_hash[ATLAS_SHA256_HEX_LEN + 1u];
    char last_state[16];
    st = atlas_db_decision_latest_revision(ac->db, document_id, &last_id, &last_no, last_hash,
                                           sizeof(last_hash), last_state, sizeof(last_state), err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (last_no >= ATLAS_DECISION_MAX_REVISIONS) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "this decision already has %d revisions, which is the limit; a "
                             "decision that needs more is two decisions",
                             ATLAS_DECISION_MAX_REVISIONS);
    }

    /* Content-hash idempotency, on top of the dedup key.
     *
     * A retry that supplies the same content produces the same canonical hash,
     * and a document that gains a revision saying exactly what its newest one
     * already says is noise — which at hook-retry frequency is a lot of noise.
     * Compared against the *newest* revision only: reverting to an older
     * wording is a real change and is recorded as one. */
    char request_hash[ATLAS_SHA256_HEX_LEN + 1u];
    st = hash_request(&op->revision, ac, ac->repo.scanned_head, request_hash, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (last_id > 0 && strcmp(request_hash, last_hash) == 0) {
        out->duplicate = true;
        out->document_id = document_id;
        out->revision_id = last_id;
        out->revision_no = last_no;
        (void)snprintf(out->content_hash, sizeof(out->content_hash), "%s", last_hash);
        (void)atlas_decision_state_parse(last_state, &out->state);
        return atlas_db_decision_uid_of(ac->db, document_id, &out->uid, err);
    }

    /* **Rule 4, and the reason revise is not update.**
     *
     * Whatever the newest revision's state is — proposed, approved, rejected or
     * superseded — a revision produces a *new* PROPOSED revision. An approved
     * revision is never edited, and rule 5 says it stays effective until the
     * replacement is approved: nothing on this path touches
     * `current_revision_id` or the document's status. */
    atlas_decision_revision built;
    atlas_decision_revision_init(&built);
    st = copy_revision(&built, &op->revision, err);
    if (st == ATLAS_OK) {
        out->document_id = document_id;
        st = atlas_db_decision_uid_of(ac->db, document_id, &out->uid, err);
    }
    if (st == ATLAS_OK) {
        st = write_revision(ac, op, &built, document_id, last_no + 1, out, err);
    }
    /* The account of the edge commits with the revision that carries it, or
     * neither is written. Inside this transaction, so a rolled-back revise
     * cannot leave behind a rationale explaining an edge that never existed. */
    if (st == ATLAS_OK) {
        st = write_edge_note(ac, op, document_id, out->revision_id,
                             ATLAS_DECISION_EDGE_EVENT_ADDED, err);
    }
    atlas_decision_revision_free(&built);
    return st;
}

/* --- the operator channel --------------------------------------------------- */

static atlas_status op_challenge(apply_ctx *ac, const atlas_decision_op *op,
                                 atlas_decision_result *out, atlas_err *err) {
    int64_t document_id = 0, doc_repo = 0;
    bool found = false;
    atlas_status st = atlas_db_decision_find_uid(ac->db, atlas_buf_cstr(&op->uid), &document_id,
                                                 &doc_repo, &found, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (!found) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "no decision has that id");
    }
    if (doc_repo != ac->repo.id) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "that decision belongs to a different repository");
    }

    /* A16. A remote challenge always names the revision it read, and 0 --
     * "whichever is newest" -- is not a name: the browser showed the operator
     * one specific revision, and the capability must bind to that one, not to
     * whatever happens to be newest by the time it is spent. */
    if (op->channel == ATLAS_DECISION_CHANNEL_REMOTE && op->expect_revision_no <= 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a remote challenge names the revision it is for; 0 is not a "
                             "revision");
    }

    int64_t rev_id = 0, rev_no = 0;
    char hash[ATLAS_SHA256_HEX_LEN + 1u];
    char state[16];
    if (op->expect_revision_no > 0) {
        st = atlas_db_decision_revision_by_no(ac->db, document_id, op->expect_revision_no, &rev_id,
                                              &found, err);
        if (st == ATLAS_OK && !found) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "this decision has no revision %lld",
                                 (long long)op->expect_revision_no);
        }
        rev_no = op->expect_revision_no;
        if (st == ATLAS_OK) {
            atlas_decision_revision loaded;
            atlas_decision_revision_init(&loaded);
            bool lfound = false;
            st = atlas_db_decision_revision_load(ac->db, rev_id, &loaded, &lfound, err);
            if (st == ATLAS_OK && lfound) {
                (void)snprintf(hash, sizeof(hash), "%s", loaded.content_hash);
                (void)snprintf(state, sizeof(state), "%s",
                               atlas_decision_state_name(loaded.state));
                st = atlas_buf_set(&out->title, loaded.title.data, loaded.title.len, err);
            }
            atlas_decision_revision_free(&loaded);
        }
    } else {
        st = atlas_db_decision_latest_revision(ac->db, document_id, &rev_id, &rev_no, hash,
                                               sizeof(hash), state, sizeof(state), err);
        if (st == ATLAS_OK && rev_id > 0) {
            atlas_decision_revision loaded;
            atlas_decision_revision_init(&loaded);
            bool lfound = false;
            st = atlas_db_decision_revision_load(ac->db, rev_id, &loaded, &lfound, err);
            if (st == ATLAS_OK && lfound) {
                st = atlas_buf_set(&out->title, loaded.title.data, loaded.title.len, err);
            }
            atlas_decision_revision_free(&loaded);
        }
    }
    if (st != ATLAS_OK) {
        return st;
    }
    if (rev_id == 0) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "this decision has no revisions");
    }

    /* A16. The previous season measured, by running it, that approving a
     * challenge pinned to a non-newest revision succeeds and strands the
     * newer proposed revision indefinitely with nothing warning about it. The
     * local channel still permits that -- an operator who typed
     * `--revision N` asked for it -- but a remote challenge is minted only for
     * whichever revision is newest at mint time.
     *
     * ATLAS_ERR_INTEGRITY (review round 1, amending the plan at `c7ebdf8`),
     * not ATLAS_ERR_USAGE: this is a refusal about the document's state, not
     * about the request, and the request itself is perfectly well-formed. The
     * spend-time twin below (`this decision gained revision ... after the
     * challenge was minted`) already carries INTEGRITY for the same event
     * observed a moment later -- a colleague's revision landing before or
     * after this mint -- and USAGE here told a caller who sent nothing wrong
     * to fix a request that was not broken, which is advice they cannot act
     * on. "Read it again" is the actionable half of the sentence either way. */
    if (op->channel == ATLAS_DECISION_CHANNEL_REMOTE) {
        int64_t latest_id = 0, latest_no = 0;
        char latest_hash[ATLAS_SHA256_HEX_LEN + 1u];
        char latest_state[16];
        st = atlas_db_decision_latest_revision(ac->db, document_id, &latest_id, &latest_no,
                                               latest_hash, sizeof(latest_hash), latest_state,
                                               sizeof(latest_state), err);
        if (st != ATLAS_OK) {
            return st;
        }
        if (rev_no != latest_no) {
            return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                 "a remote challenge is minted only for the newest revision; r%lld "
                                 "was reviewed but r%lld is newest -- read it again",
                                 (long long)rev_no, (long long)latest_no);
        }
    }

    atlas_decision_challenge c;
    atlas_decision_challenge_init(&c);
    st = atlas_decision_challenge_token(c.token, sizeof(c.token), err);
    if (st != ATLAS_OK) {
        return st;
    }
    /* A16, migration 31. `op_challenge` *consumes* an `ATLAS_DECISION_OP_CHALLENGE`
     * dispatched at the switch in `atlas_decision_apply_in_tx`; it does not
     * construct one, and the codebase has exactly two places that do --
     * closed, because the season's whole channel argument rests on that set
     * staying so:
     *
     *   - `src/core/service_decision.c`'s `op_new`, inside
     *     `atlas_service_decision_confirm`'s `build_op` path (called from the
     *     CLI and from the local review-apply surface). It reaches
     *     `atlas_decision_apply` directly only when *this process* is the
     *     writer -- no daemon is running, so there is nobody else to ask.
     *   - `src/ipc/server_decision.c`'s `method_challenge`, which builds its
     *     own op from the request it received. This is the producer that
     *     actually runs whenever a daemon owns the index: `apply_op` in
     *     `service_decision.c` routes to it over the socket
     *     (`decision.challenge`) instead of calling `atlas_decision_apply`
     *     in-process, and a registered repository's daemon running is the
     *     ordinary case on an operator's machine.
     *
     * Both are local-terminal producers and both set `op->channel =
     * ATLAS_DECISION_CHANNEL_LOCAL`. `src/ipc/server_remote.c` is the third
     * producer, and the only one that sets REMOTE. So `channel` is read from
     * `op->channel` here -- already checked non-UNKNOWN by
     * `atlas_decision_apply_in_tx` -- rather than assumed; assuming it is
     * exactly the shape of bug guard #1 exists to close, one field earlier. */
    c.channel = op->channel;
    if (c.channel == ATLAS_DECISION_CHANNEL_REMOTE) {
        (void)snprintf(c.key_id, sizeof(c.key_id), "%s", ac->key_id);
    }
    c.repo_id = ac->repo.id;
    c.document_id = document_id;
    c.revision_id = rev_id;
    c.revision_no = rev_no;
    (void)snprintf(c.content_hash, sizeof(c.content_hash), "%s", hash);
    /* The intent is bound too. A capability issued to reject something must not
     * approve it, so the intent is part of the tuple rather than a parameter of
     * the later request. Naming a replacement implies supersession; anything
     * else is what the caller asked for. */
    c.intent = op->intent;
    /* A16, moved ahead of the replacement/supersede handling below (review
     * finding I4): this used to sit after it, so a REMOTE request with
     * `intent = SUPERSEDE` -- `decision.remote_challenge` never reads a
     * `replacement` parameter, so `op->replacement_uid.len` is always 0 for
     * this channel -- fell into the `else if` below and got USAGE's "a
     * supersession needs the decision that replaces this one" instead of
     * this INTEGRITY refusal. Two refusals for what is semantically one
     * event -- "this channel does not do this" -- is this season's own "one
     * class per sentence" rule, and the wrong one of the two is actionable
     * advice a REMOTE caller cannot act on: `decision.remote_challenge`'s own
     * params list has no `replacement` field, and the write table's
     * allowlist drops one before the daemon ever sees it, so supplying it
     * produces the identical wrong refusal forever. Checked first and
     * unconditionally for a REMOTE SUPERSEDE or REVALIDATE, so neither the
     * replacement lookup nor the intent-specific work beyond this function
     * ever runs on its way to being refused anyway; conditioned on
     * `c.channel == ATLAS_DECISION_CHANNEL_REMOTE`, so the LOCAL path -- and
     * its own "no replacement named" refusal -- is unchanged. */
    if (c.channel == ATLAS_DECISION_CHANNEL_REMOTE &&
        (c.intent == ATLAS_DECISION_INTENT_SUPERSEDE ||
         c.intent == ATLAS_DECISION_INTENT_REVALIDATE)) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "supersede and revalidate are not offered from the browser; use a "
                             "terminal on the Atlas machine");
    }
    if (op->replacement_uid.len > 0) {
        c.intent = ATLAS_DECISION_INTENT_SUPERSEDE;
        int64_t rid = 0, rrepo = 0;
        bool rfound = false;
        st = atlas_db_decision_find_uid(ac->db, atlas_buf_cstr(&op->replacement_uid), &rid, &rrepo,
                                        &rfound, err);
        if (st != ATLAS_OK) {
            return st;
        }
        if (!rfound) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "no decision has the id given as the replacement");
        }
        if (rrepo != ac->repo.id) {
            /* Rule 7. Checked when the capability is issued as well as when it
             * is spent, so an operator is told at the prompt rather than after
             * confirming. */
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "a decision may only be superseded by one in the same repository");
        }
        c.supersede_document_id = rid;
    } else if (c.intent == ATLAS_DECISION_INTENT_SUPERSEDE) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a supersession needs the decision that replaces this one");
    }
    (void)snprintf(c.created_at, sizeof(c.created_at), "%s", ac->now);
    {
        /* `atlas_iso8601_before_now` with a negative offset would be a
         * subtraction pretending to be an addition, so the expiry is computed
         * as its own timestamp. Timestamps in Atlas sort lexicographically, so
         * expiry comparison is a string compare. */
        char expires[ATLAS_TS_MAX];
        atlas_iso8601_after_now(expires, sizeof(expires), ATLAS_DECISION_CHALLENGE_TTL_MS);
        (void)snprintf(c.expires_at, sizeof(c.expires_at), "%s", expires);
    }

    /* --- A6: what a revalidation capability is additionally bound to --------
     *
     * The repository state and the evidence digest, captured here and compared
     * again when the capability is spent. That is what makes commit drift and
     * evidence drift refusals rather than surprises: a capability issued
     * against one view of the code cannot be spent against another, exactly as
     * an approval capability cannot be spent against a different revision.
     *
     * Both are read from the database, and deliberately so. This runs on the
     * writer thread inside a transaction, where A1 forbids creating a process
     * or reading a file — so the indexed head comes from the repository row
     * rather than from Git, and the digest from the stored index rather than
     * from the working tree.
     *
     * The assessment the operator will be shown is *not* computed here. It is
     * supplied by the caller that displayed it, so what the validation record
     * preserves is what a human actually saw rather than what a recomputation
     * a moment later would have produced. */
    if (c.intent == ATLAS_DECISION_INTENT_REVALIDATE) {
        if (strcmp(state, "APPROVED") != 0) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "only an approved revision can be revalidated; revision %lld is "
                                 "%s",
                                 (long long)rev_no, state);
        }
        (void)snprintf(c.indexed_commit, sizeof(c.indexed_commit), "%s", ac->repo.scanned_head);
        if (c.indexed_commit[0] == '\0') {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "this repository has not been indexed, so there is no exact "
                                 "state to revalidate against");
        }
        atlas_decision_revision loaded;
        atlas_decision_revision_init(&loaded);
        bool lfound = false;
        st = atlas_db_decision_revision_load(ac->db, rev_id, &loaded, &lfound, err);
        if (st == ATLAS_OK && lfound) {
            st = atlas_gate_evidence_digest_for(ac->db, ac->repo.id, &loaded, c.evidence_digest,
                                                err);
        }
        atlas_decision_revision_free(&loaded);
        if (st != ATLAS_OK) {
            return st;
        }
        /* Both are closed Atlas vocabularies, checked rather than trusted: they
         * arrived from a caller, and a caller is not the authority on what an
         * A6 reason code is. */
        {
            atlas_gate_freshness parsed;
            const char *fresh = atlas_buf_cstr(&op->prior_freshness);
            if (op->prior_freshness.len == 0 || !atlas_gate_freshness_parse(fresh, &parsed)) {
                return atlas_err_set(err, ATLAS_ERR_USAGE,
                                     "a revalidation capability must name the assessment it is "
                                     "being issued against");
            }
            (void)snprintf(c.prior_freshness, sizeof(c.prior_freshness), "%s", fresh);
            atlas_gate_reason codes[ATLAS_GATE_MAX_REASONS];
            size_t n = 0;
            st = atlas_gate_reasons_unpack(atlas_buf_cstr(&op->prior_reasons), codes,
                                           ATLAS_GATE_MAX_REASONS, &n, err);
            if (st != ATLAS_OK) {
                return st;
            }
            (void)snprintf(c.prior_reasons, sizeof(c.prior_reasons), "%s",
                           atlas_buf_cstr(&op->prior_reasons));
        }
        (void)snprintf(out->indexed_commit, sizeof(out->indexed_commit), "%s", c.indexed_commit);
        (void)snprintf(out->evidence_digest, sizeof(out->evidence_digest), "%s",
                       c.evidence_digest);
    }

    /* --- A9.1: what a resolution capability additionally requires -------------
     *
     * The revision must be approved and the document's kind must be one whose
     * approved form makes a demand. Both are checked here, at issue, as well as
     * at the write point — for the reason the supersede replacement is checked
     * twice: an operator learns at the prompt that the operation is meaningless
     * for this record, rather than after typing a confirmation. The write point
     * is still the guarantee. */
    {
        atlas_decision_kind kind = ATLAS_DECISION_KIND_DECISION;
        bool kfound = false;
        st = atlas_db_decision_kind_of(ac->db, document_id, &kind, &kfound, err);
        if (st != ATLAS_OK) {
            return st;
        }
        out->knowledge_kind = kind;
        /* A16. The operator's policy names which kinds may be disposed of
         * from the browser; this is the mint-time half of that check, and
         * `spend_challenge` repeats it at spend time -- the same reason the
         * repository identity is checked twice. */
        if (c.channel == ATLAS_DECISION_CHANNEL_REMOTE &&
            (op->remote_kinds & ATLAS_DECISION_KIND_BIT(kind)) == 0) {
            return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                 "a record of kind %s is not one the remote disposal policy "
                                 "names; dispose of it on a terminal",
                                 atlas_decision_kind_name(kind));
        }
        if (c.intent == ATLAS_DECISION_INTENT_RESOLVE) {
            if (strcmp(state, "APPROVED") != 0) {
                return atlas_err_set(err, ATLAS_ERR_USAGE,
                                     "only an approved revision can be resolved; revision %lld is "
                                     "%s",
                                     (long long)rev_no, state);
            }
            if (!atlas_decision_kind_resolvable(kind)) {
                return atlas_err_set(err, ATLAS_ERR_USAGE,
                                     "a record of kind %s cannot be resolved: resolving records "
                                     "that the demand a record made has been met, and this kind "
                                     "makes none. Supersede it with a record that replaces it "
                                     "instead",
                                     atlas_decision_kind_name(kind));
            }
        }
    }

    int64_t cid = 0;
    st = atlas_db_decision_challenge_insert(ac->db, &c, &cid, err);
    if (st != ATLAS_OK) {
        return st;
    }
    /* Housekeeping: expired, unspent capabilities. A consumed one is part of an
     * approval record and is never removed. */
    {
        int64_t removed = 0;
        atlas_err ignore;
        atlas_err_init(&ignore);
        (void)atlas_db_decision_challenges_prune(ac->db, ac->now,
                                                 ATLAS_DECISION_CHALLENGES_RETAIN, &removed,
                                                 &ignore);
    }

    out->document_id = document_id;
    out->revision_id = rev_id;
    out->revision_no = rev_no;
    (void)snprintf(out->content_hash, sizeof(out->content_hash), "%s", hash);
    (void)snprintf(out->expires_at, sizeof(out->expires_at), "%s", c.expires_at);
    atlas_decision_confirm_phrase(hash, out->confirm, sizeof(out->confirm));
    (void)atlas_decision_state_parse(state, &out->state);
    /* A16. The verified credential this challenge was minted for, so the
     * browser's mint response can show it -- not an actor, because minting
     * transitions nothing yet. */
    if (c.channel == ATLAS_DECISION_CHANNEL_REMOTE) {
        (void)snprintf(out->key_id, sizeof(out->key_id), "%s", c.key_id);
    }
    st = atlas_buf_set_str(&out->token, c.token, err);
    if (st == ATLAS_OK) {
        st = atlas_db_decision_uid_of(ac->db, document_id, &out->uid, err);
    }
    return st;
}

/* Checks and spends a capability, then reports what it was bound to.
 *
 * Every rejection the phase requires is here, in the order that makes each one
 * decidable: unknown token, wrong intent, wrong repository, expired, already
 * consumed, bound to a different revision, and content that no longer hashes to
 * what the capability names. The last cannot happen through Atlas, because
 * revisions are immutable — and it is checked anyway, because "cannot happen"
 * is a belief and this is a check. */
/* Spends a capability, and reports the kind of the record it was spent on.
 *
 * `out->knowledge_kind` is filled here rather than in each of the five ops that
 * spend a capability, because zero is `DECISION` and a field nobody sets is
 * therefore not empty — it is a confident wrong answer. `atlas decision
 * approve` reported `kind: DECISION` for an APPROVED `INVARIANT` and for an
 * APPROVED `OBLIGATION`: the document, `decision show`, `decision list` and the
 * JSON surface all carried the right kind, and the one surface an operator sees
 * at the moment of approving carried the wrong one. A9.1's rule is that every
 * surface reports kind and status in separate fields, and a surface reporting
 * the *wrong* kind is worse than one reporting neither.
 *
 * One place, five ops: approve, reject, resolve, supersede and revalidate all
 * arrive here, so the answer cannot drift between them. The kind is read from
 * the document, never from the operation — the rule `transition()` follows and
 * for the same reason. */
static atlas_status spend_challenge(apply_ctx *ac, const atlas_decision_op *op,
                                    atlas_decision_intent want, atlas_decision_challenge *out_c,
                                    atlas_decision_result *out, atlas_err *err) {
    if (op->token.len == 0) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "this operation changes a decision's lifecycle state and needs an "
                             "approval challenge; run it through `atlas decision` on a terminal");
    }
    bool found = false;
    atlas_status st = atlas_db_decision_challenge_find(ac->db, atlas_buf_cstr(&op->token), out_c,
                                                       &found, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (!found) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "that approval challenge is not one Atlas issued");
    }
    if (out_c->intent != want) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "that approval challenge was issued to %s, not to %s",
                             atlas_decision_intent_name(out_c->intent),
                             atlas_decision_intent_name(want));
    }
    if (out_c->repo_id != ac->repo.id) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "that approval challenge was issued for a different repository");
    }
    /* A16. A capability minted for one channel cannot be spent through the
     * other -- `decision_challenges.channel` exists for exactly this check.
     * `op->channel` was already checked non-UNKNOWN by
     * `atlas_decision_apply_in_tx`, so only the two real values need
     * distinguishing here. */
    if (out_c->channel != op->channel) {
        if (out_c->channel == ATLAS_DECISION_CHANNEL_REMOTE) {
            return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                 "that approval challenge was minted through the remote channel "
                                 "and cannot be spent locally");
        }
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "that approval challenge was minted through the local channel and "
                             "cannot be spent from the browser");
    }
    if (op->channel == ATLAS_DECISION_CHANNEL_REMOTE) {
        /* The credential that spends a REMOTE challenge must be the one that
         * minted it -- `ac->key_id` is what `atlas_decision_remote_verify`
         * just proved the presented token authenticates as, and it is
         * compared against the value stored on the row, never against
         * anything else the request supplied. */
        if (strcmp(ac->key_id, out_c->key_id) != 0) {
            return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                 "that approval challenge was minted for a different credential");
        }
        /* The newest-revision guard, repeated at spend time: minting already
         * required this revision to be newest, and it must still be by the
         * time the challenge is spent, or an operator who reviewed r1 could
         * end up approving r1 after r2 was proposed without ever seeing r2. */
        int64_t latest_id = 0, latest_no = 0;
        char latest_hash[ATLAS_SHA256_HEX_LEN + 1u];
        char latest_state[16];
        st = atlas_db_decision_latest_revision(ac->db, out_c->document_id, &latest_id, &latest_no,
                                               latest_hash, sizeof(latest_hash), latest_state,
                                               sizeof(latest_state), err);
        if (st != ATLAS_OK) {
            return st;
        }
        if (latest_no != out_c->revision_no) {
            return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                 "this decision gained revision %lld after the challenge was "
                                 "minted; nothing was changed -- read it again",
                                 (long long)latest_no);
        }
        /* The kinds policy, re-checked at spend. No mask is stored on the
         * challenge row -- `op->remote_kinds` here is simply whatever this
         * request presents now, not a comparison against what minted. The
         * mint-time check in `op_challenge` is what binds: a challenge is
         * only ever minted for a kind the policy named at that moment. This
         * re-check exists for what can change *between* mint and spend: an
         * operator narrowing `remote_dispose_kinds` in the root-owned policy
         * and restarting the daemon, leaving an already-outstanding
         * challenge for a kind the current policy no longer names. A wider
         * mask presented here grants nothing new -- the mint already fixed
         * what this challenge is for -- so the only case this check needs to
         * catch, and does, is a policy that has narrowed since. */
        atlas_decision_kind rkind = ATLAS_DECISION_KIND_DECISION;
        bool rkfound = false;
        st = atlas_db_decision_kind_of(ac->db, out_c->document_id, &rkind, &rkfound, err);
        if (st != ATLAS_OK) {
            return st;
        }
        if ((op->remote_kinds & ATLAS_DECISION_KIND_BIT(rkind)) == 0) {
            return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                 "a record of kind %s is not one the remote disposal policy "
                                 "names; dispose of it on a terminal",
                                 atlas_decision_kind_name(rkind));
        }
    }
    if (out_c->consumed) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "that approval challenge has already been used; a challenge "
                             "authorises exactly one transition");
    }
    if (strcmp(out_c->expires_at, ac->now) < 0) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "that approval challenge expired at %s; ask for a new one and read "
                             "the decision again before confirming",
                             out_c->expires_at);
    }

    /* The confirmation the operator typed, compared against the phrase derived
     * from the *stored* content hash rather than from anything in the request.
     *
     * This is not a secret and is not treated as one: it is a short prefix of a
     * hash the CLI just displayed. What it buys is that the confirmation is
     * about one specific revision's bytes — an operator cannot type "yes" and
     * approve whatever happens to be current. */
    char want_phrase[ATLAS_DECISION_CONFIRM_MAX];
    atlas_decision_confirm_phrase(out_c->content_hash, want_phrase, sizeof(want_phrase));
    if (op->confirmation.len != strlen(want_phrase) ||
        strncmp(atlas_buf_cstr(&op->confirmation), want_phrase, strlen(want_phrase)) != 0) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "the confirmation does not match this revision; nothing was changed");
    }

    /* Rehash the stored content and compare. A mismatch means the row was
     * altered outside Atlas, which is a refusal rather than a warning: an
     * approval that binds to a hash the content no longer has binds to
     * nothing. */
    atlas_decision_revision loaded;
    atlas_decision_revision_init(&loaded);
    bool lfound = false;
    st = atlas_db_decision_revision_load(ac->db, out_c->revision_id, &loaded, &lfound, err);
    if (st == ATLAS_OK && !lfound) {
        st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                           "the revision this challenge names is no longer present");
    }
    if (st == ATLAS_OK) {
        char rehash[ATLAS_SHA256_HEX_LEN + 1u];
        st = atlas_decision_content_hash(&loaded, rehash, err);
        if (st == ATLAS_OK && strcmp(rehash, out_c->content_hash) != 0) {
            st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                               "this revision's stored content does not hash to the value the "
                               "challenge was bound to; Atlas refuses to approve it");
        }
        if (st == ATLAS_OK && strcmp(loaded.content_hash, out_c->content_hash) != 0) {
            st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                               "this revision's recorded hash differs from the one the challenge "
                               "was bound to");
        }
    }
    atlas_decision_revision_free(&loaded);
    if (st != ATLAS_OK) {
        return st;
    }

    bool spent = false;
    st = atlas_db_decision_challenge_consume(ac->db, out_c->id, ac->now, &spent, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (!spent) {
        /* Another writer consumed it between the read and this update. With one
         * writer thread that cannot happen inside the daemon; the check is here
         * because the property must hold for reasons other than "there happens
         * to be one writer". */
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "that approval challenge was consumed concurrently; nothing was "
                             "changed");
    }
    if (out != NULL) {
        bool kfound = false;
        st = atlas_db_decision_kind_of(ac->db, out_c->document_id, &out->knowledge_kind, &kfound,
                                       err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    return ATLAS_OK;
}

/* Recomputes a document's cached status the same way the ledger replays.
 *
 * There is one of these rather than a status assignment at each transition, and
 * that is not tidiness: `atlas_db_decision_verify` derives the status by replay,
 * so any transition that reasons about it independently can disagree with the
 * ledger and make `atlas doctor` report corruption on a perfectly legal
 * sequence. Rejecting revision 1 while revision 2 is still proposed is exactly
 * that case — the document is still PROPOSED, not REJECTED.
 *
 * The precedence is the replay's: a document-level supersession is the
 * strongest fact, then an effective revision, then an outstanding proposal, then
 * a resolved one, and only a document with nothing left is REJECTED. The order
 * is the same one `atlas_db_decision_verify` uses, and it has to be.
 *
 * The approved revision is **derived** rather than read from the cache this
 * function exists to write. It used to be read from `current_revision_id`, which
 * worked only because no operation could invalidate it — resolving one can: the
 * revision leaves APPROVED while the document still points at it, so a
 * recomputation that trusted the pointer would report a discharged obligation as
 * effective for ever. The partial unique index over `state = 'APPROVED'` is what
 * makes the derivation a seek that cannot return two answers. */
static atlas_status recompute_status(apply_ctx *ac, int64_t document_id, atlas_err *err) {
    int64_t current = 0;
    atlas_status st = atlas_db_decision_approved_revision(ac->db, document_id, &current, err);
    if (st != ATLAS_OK) {
        return st;
    }
    int64_t superseded_by = 0;
    int64_t proposed = 0;
    int64_t resolved = 0;
    st = atlas_db_decision_document_shape(ac->db, document_id, &superseded_by, &proposed, &resolved,
                                          err);
    if (st != ATLAS_OK) {
        return st;
    }
    const char *status;
    if (superseded_by > 0) {
        status = "SUPERSEDED";
    } else if (current > 0) {
        status = "APPROVED";
    } else if (proposed > 0) {
        status = "PROPOSED";
    } else if (resolved > 0) {
        status = "RESOLVED";
    } else {
        status = "REJECTED";
    }
    return atlas_db_decision_document_set_state(ac->db, document_id, current, status, ac->now, err);
}

/* The conditional transition, plus the cache update that must accompany it.
 *
 * A9.1: the transition table is kind-aware, and the kind it is asked about is
 * **read from the document** here rather than taken from the operation. A caller
 * that supplied it could name a resolvable kind for a record that is not one,
 * and this is the single write point precisely so that no caller gets to
 * describe the record it is changing. */
static atlas_status transition(apply_ctx *ac, int64_t document_id, int64_t revision_id,
                               int64_t revision_no, atlas_decision_state from,
                               atlas_decision_state to, atlas_decision_actor actor,
                               int64_t challenge_id, int64_t superseded_by_revision_id,
                               int64_t superseded_by_document_id, const char *detail,
                               const char *content_hash, const char *key_id, atlas_err *err) {
    atlas_decision_kind kind = ATLAS_DECISION_KIND_DECISION;
    bool found = false;
    atlas_status kst = atlas_db_decision_kind_of(ac->db, document_id, &kind, &found, err);
    if (kst != ATLAS_OK) {
        return kst;
    }
    if (!found) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "that decision document no longer exists, so no transition was made");
    }
    if (!atlas_decision_transition_allowed(kind, from, to)) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "a record of kind %s that is %s cannot become %s",
                             atlas_decision_kind_name(kind), atlas_decision_state_name(from),
                             atlas_decision_state_name(to));
    }
    bool changed = false;
    atlas_status st = atlas_db_decision_revision_set_state(
        ac->db, revision_id, atlas_decision_state_name(from), atlas_decision_state_name(to),
        &changed, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (!changed) {
        /* Rule 12: a conflicting concurrent transition fails deterministically
         * rather than last-write-wins. The UPDATE named the state this call
         * observed, so a row that moved underneath it simply did not match. */
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "this decision revision is no longer %s, so the %s was refused; read "
                             "it again and retry",
                             atlas_decision_state_name(from), atlas_decision_state_name(to));
    }
    /* A16. `key_id` is the caller's to supply and is non-NULL only for the one
     * transition a REMOTE challenge actually authorises: the credential that
     * minted it, read back from the spent challenge row rather than passed in
     * fresh, so the ledger's `key_id` can never differ from what
     * `spend_challenge` already checked it against. Every other caller of this
     * function — the automatic supersession an approval implies, a LOCAL
     * transition, a policy-authorised one — passes NULL, and every event those
     * write still names none. */
    return atlas_db_decision_event_append(ac->db, document_id, revision_id, revision_no,
                                          atlas_decision_state_name(to),
                                          atlas_decision_actor_name(actor), content_hash,
                                          challenge_id, superseded_by_revision_id,
                                          superseded_by_document_id, detail, key_id, NULL, NULL,
                                          err);
}

/* A16. The actor and the ledger's credential come from the *spent challenge's*
 * own channel -- `c->channel`, never from anything the request supplied --
 * because a caller that could name its own actor could write
 * LOCAL_OPERATOR_CONFIRMED about a channel nothing had been through.
 * `spend_challenge` has already refused a channel mismatch by the time any
 * caller of this reaches it, so `c->channel` and `op->channel` agree.
 *
 * `local_detail` is the existing, operation-specific LOCAL sentence
 * (approve's, reject's and resolve's all differ). The REMOTE sentence is one
 * frozen line shared by all three: the fact it records -- that the channel
 * and the named credential were used -- does not vary with which transition
 * that credential authorised, so there is exactly one to keep in sync rather
 * than three.
 *
 * Writes `out->actor` and `out->key_id` and returns the detail string to
 * pass to `transition`, which is either `local_detail` verbatim or
 * `remote_detail_buf` after this fills it -- the caller owns that buffer so
 * nothing here outlives its caller's stack frame. */
static const char *channel_actor_detail(const atlas_decision_challenge *c, const char *local_detail,
                                        atlas_decision_result *out, atlas_decision_actor *actor_out,
                                        const char **key_id_out, char *remote_detail_buf,
                                        size_t remote_detail_buf_len) {
    if (c->channel == ATLAS_DECISION_CHANNEL_REMOTE) {
        *actor_out = ATLAS_DECISION_ACTOR_REMOTE_OPERATOR_CONFIRMED;
        *key_id_out = c->key_id;
        (void)snprintf(remote_detail_buf, remote_detail_buf_len,
                       "confirmed through the Atlas remote operator channel with credential %s; "
                       "this records that the channel and the credential were used, not which "
                       "person used them",
                       c->key_id);
        out->actor = *actor_out;
        (void)snprintf(out->key_id, sizeof(out->key_id), "%s", c->key_id);
        return remote_detail_buf;
    }
    *actor_out = ATLAS_DECISION_ACTOR_LOCAL_OPERATOR_CONFIRMED;
    *key_id_out = NULL;
    out->actor = *actor_out;
    out->key_id[0] = '\0';
    return local_detail;
}

static atlas_status op_approve(apply_ctx *ac, const atlas_decision_op *op,
                               atlas_decision_result *out, atlas_err *err) {
    atlas_decision_challenge c;
    atlas_decision_challenge_init(&c);
    atlas_status st = spend_challenge(ac, op, ATLAS_DECISION_INTENT_APPROVE, &c, out, err);
    if (st != ATLAS_OK) {
        return st;
    }

    atlas_decision_actor actor;
    const char *key_id = NULL;
    char remote_detail[256];
    const char *detail = channel_actor_detail(
        &c,
        "confirmed through the Atlas local operator channel; this records that the channel was "
        "used, not which person used it",
        out, &actor, &key_id, remote_detail, sizeof(remote_detail));

    /* What is currently effective, before anything changes. */
    int64_t prev_rev_id = 0, prev_rev_no = 0;
    {
        atlas_decision_revision cur;
        atlas_decision_revision_init(&cur);
        bool found = false;
        /* The approved revision, if any, is the document's cached current. It
         * is read through the ledger-backed column rather than searched for,
         * and the partial unique index guarantees there is at most one. */
        int64_t doc_current = 0;
        atlas_status rst = atlas_db_decision_current_revision(ac->db, c.document_id, &doc_current,
                                                              err);
        if (rst == ATLAS_OK && doc_current > 0 && doc_current != c.revision_id) {
            rst = atlas_db_decision_revision_load(ac->db, doc_current, &cur, &found, err);
            if (rst == ATLAS_OK && found) {
                prev_rev_id = cur.id;
                prev_rev_no = cur.revision_no;
            }
        }
        atlas_decision_revision_free(&cur);
        if (rst != ATLAS_OK) {
            return rst;
        }
    }

    /* **Rule 6: atomic.**
     *
     * The supersession happens before the approval, and both are in this one
     * transaction. The order matters because of the partial unique index that
     * enforces rule 9 — at most one APPROVED revision per document — which
     * would reject the approval outright if the predecessor were still
     * approved. So the constraint does not merely document the invariant; it
     * makes getting the order wrong a hard failure rather than a state with two
     * effective revisions. */
    if (prev_rev_id > 0) {
        /* Decision 12. The previous season found `op_approve` writing this
         * detail unconditionally, in a sequence where the newly approved
         * revision is *older* than the one it superseded -- so the ledger
         * recorded the reverse of what happened. The direction is now
         * observed rather than assumed: `prev_rev_no` is the revision being
         * superseded and `c.revision_no` is the one replacing it, so which
         * sentence applies is a comparison of the two, not a constant. */
        const char *supersede_detail =
            prev_rev_no < c.revision_no
                ? "replaced by a later revision of the same decision, which was approved in the "
                  "same transaction"
                : "replaced by an earlier revision of the same decision, approved after it in the "
                  "same transaction";
        st = transition(ac, c.document_id, prev_rev_id, prev_rev_no, ATLAS_DECISION_APPROVED,
                        ATLAS_DECISION_SUPERSEDED, ATLAS_DECISION_ACTOR_ATLAS_AUTOMATIC, c.id,
                        c.revision_id, 0, supersede_detail, NULL, NULL, err);
        if (st != ATLAS_OK) {
            return st;
        }
        out->superseded_revision_no = prev_rev_no;
    }

    st = transition(ac, c.document_id, c.revision_id, c.revision_no, ATLAS_DECISION_PROPOSED,
                    ATLAS_DECISION_APPROVED, actor, c.id, 0, 0, detail, c.content_hash, key_id, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_decision_document_set_state(ac->db, c.document_id, c.revision_id, "APPROVED",
                                              ac->now, err);
    if (st != ATLAS_OK) {
        return st;
    }
    out->document_id = c.document_id;
    out->revision_id = c.revision_id;
    out->revision_no = c.revision_no;
    out->state = ATLAS_DECISION_APPROVED;
    (void)snprintf(out->content_hash, sizeof(out->content_hash), "%s", c.content_hash);
    return atlas_db_decision_uid_of(ac->db, c.document_id, &out->uid, err);
}

static atlas_status op_reject(apply_ctx *ac, const atlas_decision_op *op,
                              atlas_decision_result *out, atlas_err *err) {
    atlas_decision_challenge c;
    atlas_decision_challenge_init(&c);
    atlas_status st = spend_challenge(ac, op, ATLAS_DECISION_INTENT_REJECT, &c, out, err);
    if (st != ATLAS_OK) {
        return st;
    }
    atlas_decision_actor actor;
    const char *key_id = NULL;
    char remote_detail[256];
    const char *detail =
        channel_actor_detail(&c, "refused through the Atlas local operator channel", out, &actor,
                             &key_id, remote_detail, sizeof(remote_detail));
    st = transition(ac, c.document_id, c.revision_id, c.revision_no, ATLAS_DECISION_PROPOSED,
                    ATLAS_DECISION_REJECTED, actor, c.id, 0, 0, detail, c.content_hash, key_id, err);
    if (st != ATLAS_OK) {
        return st;
    }
    /* Rejecting a revision does not retract whatever is already approved, and
     * does not make the *document* rejected while another revision is still
     * outstanding. Both follow from recomputing the status the way the ledger
     * replays it rather than deciding it here. */
    st = recompute_status(ac, c.document_id, err);
    if (st != ATLAS_OK) {
        return st;
    }
    out->document_id = c.document_id;
    out->revision_id = c.revision_id;
    out->revision_no = c.revision_no;
    out->state = ATLAS_DECISION_REJECTED;
    (void)snprintf(out->content_hash, sizeof(out->content_hash), "%s", c.content_hash);
    return atlas_db_decision_uid_of(ac->db, c.document_id, &out->uid, err);
}

/* A9.1. Close out an approved record whose demand has been met.
 *
 * It is `op_reject`'s shape rather than `op_supersede`'s, and that is the design:
 * nothing replaces the record, nothing is deleted, no prose is rewritten, no
 * second document is named, and no `superseded_by_document_id` is set — because
 * a resolved obligation was not replaced by anything. What changes is one
 * revision's state and one ledger row, and the document stops being effective.
 *
 * The kind check is in `transition`, which reads the kind from the document
 * rather than from this operation, and in `op_challenge`, which refuses to mint
 * a capability for a record the operation is meaningless for. Two checks, one
 * authority: the write point. */
static atlas_status op_resolve(apply_ctx *ac, const atlas_decision_op *op,
                               atlas_decision_result *out, atlas_err *err) {
    atlas_decision_challenge c;
    atlas_decision_challenge_init(&c);
    atlas_status st = spend_challenge(ac, op, ATLAS_DECISION_INTENT_RESOLVE, &c, out, err);
    if (st != ATLAS_OK) {
        return st;
    }
    atlas_decision_actor actor;
    const char *key_id = NULL;
    char remote_detail[256];
    const char *detail = channel_actor_detail(
        &c,
        "the demand this record made was recorded as met through the Atlas local operator "
        "channel; this records that the channel was used, not which person used it",
        out, &actor, &key_id, remote_detail, sizeof(remote_detail));
    st = transition(ac, c.document_id, c.revision_id, c.revision_no, ATLAS_DECISION_APPROVED,
                    ATLAS_DECISION_RESOLVED, actor, c.id, 0, 0, detail, c.content_hash, key_id, err);
    if (st != ATLAS_OK) {
        return st;
    }
    /* Recomputed rather than assigned, for the reason rejection recomputes: a
     * document with another revision still outstanding is PROPOSED, not
     * RESOLVED, and deciding that here would disagree with the ledger replay. */
    st = recompute_status(ac, c.document_id, err);
    if (st != ATLAS_OK) {
        return st;
    }
    bool kfound = false;
    st = atlas_db_decision_kind_of(ac->db, c.document_id, &out->knowledge_kind, &kfound, err);
    if (st != ATLAS_OK) {
        return st;
    }
    out->document_id = c.document_id;
    out->revision_id = c.revision_id;
    out->revision_no = c.revision_no;
    out->state = ATLAS_DECISION_RESOLVED;
    (void)snprintf(out->content_hash, sizeof(out->content_hash), "%s", c.content_hash);
    return atlas_db_decision_uid_of(ac->db, c.document_id, &out->uid, err);
}

static atlas_status op_supersede(apply_ctx *ac, const atlas_decision_op *op,
                                 atlas_decision_result *out, atlas_err *err) {
    atlas_decision_challenge c;
    atlas_decision_challenge_init(&c);
    atlas_status st = spend_challenge(ac, op, ATLAS_DECISION_INTENT_SUPERSEDE, &c, out, err);
    if (st != ATLAS_OK) {
        return st;
    }
    /* A16. A REMOTE challenge can never reach here in practice -- `op_challenge`
     * already refuses to mint one with a SUPERSEDE intent -- but "cannot
     * happen" is a belief and this is the single write point, so it is
     * checked anyway, exactly as `spend_challenge` rehashes stored content
     * that cannot have changed. The whole transaction rolls back, so the
     * challenge this would otherwise have consumed is not spent either. */
    if (c.channel == ATLAS_DECISION_CHANNEL_REMOTE) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "supersede and revalidate are not offered from the browser; use a "
                             "terminal on the Atlas machine");
    }
    out->actor = ATLAS_DECISION_ACTOR_LOCAL_OPERATOR_CONFIRMED;
    if (c.supersede_document_id <= 0) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "that challenge names no replacement decision");
    }

    /* Rule 8: cycles are impossible. Checked here as well as when the challenge
     * was issued, because the chain can have grown in between. */
    bool cycles = false;
    st = atlas_db_decision_supersede_reaches(ac->db, c.document_id, c.supersede_document_id,
                                             &cycles, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (cycles) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "that supersession would create a cycle, or the chain is longer than "
                             "Atlas will walk; nothing was changed");
    }

    /* The effective revision of the superseded document, if there is one, goes
     * from APPROVED to SUPERSEDED. A document with nothing approved is marked
     * superseded at the document level and its revisions keep their states —
     * superseding something that was never effective must not fabricate a
     * transition out of a state it was never in. */
    int64_t current = 0;
    st = atlas_db_decision_current_revision(ac->db, c.document_id, &current, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (current > 0) {
        atlas_decision_revision cur;
        atlas_decision_revision_init(&cur);
        bool found = false;
        st = atlas_db_decision_revision_load(ac->db, current, &cur, &found, err);
        if (st == ATLAS_OK && found) {
            st = transition(ac, c.document_id, cur.id, cur.revision_no, ATLAS_DECISION_APPROVED,
                            ATLAS_DECISION_SUPERSEDED,
                            ATLAS_DECISION_ACTOR_LOCAL_OPERATOR_CONFIRMED, c.id, 0,
                            c.supersede_document_id,
                            "superseded by another decision through the Atlas local operator "
                            "channel",
                            cur.content_hash, NULL, err);
        }
        atlas_decision_revision_free(&cur);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    st = atlas_db_decision_document_set_superseded_by(ac->db, c.document_id,
                                                      c.supersede_document_id, ac->now, err);
    if (st == ATLAS_OK) {
        st = atlas_db_decision_document_set_state(ac->db, c.document_id, 0, "SUPERSEDED", ac->now,
                                                  err);
    }
    if (st != ATLAS_OK) {
        return st;
    }
    out->document_id = c.document_id;
    out->state = ATLAS_DECISION_SUPERSEDED;
    st = atlas_db_decision_uid_of(ac->db, c.document_id, &out->uid, err);
    if (st == ATLAS_OK) {
        st = atlas_db_decision_uid_of(ac->db, c.supersede_document_id, &out->replaced_by_uid, err);
    }
    return st;
}

/* --- promoting an A2 proposal ------------------------------------------------ */

/* A6. Records that an operator checked an approved revision against one exact
 * repository state.
 *
 * **Nothing about the decision changes.** No revision is edited, no state
 * transitions, no `decision_events` row is written, and the previous assessment
 * is preserved rather than replaced — the point of the record is that a concern
 * existed and was addressed, and a record that dropped the concern would be a
 * record of nothing. What changes is the point in history that later
 * assessments measure their change range from.
 *
 * Every rejection the phase requires falls out of the capability plus two
 * comparisons, and both comparisons are database reads:
 *
 *   - replay, expiry, wrong revision, wrong intent, wrong repository and a
 *     content hash that moved: `spend_challenge`, unchanged from A4;
 *   - **commit drift**: the indexed head is not what it was when the capability
 *     was issued, so the operator confirmed against a repository state that is
 *     no longer the one being recorded;
 *   - **evidence drift**: the anchors resolve differently from when the
 *     capability was issued, so what the operator was shown is not what would
 *     be recorded.
 *
 * Neither needs Git and neither needs the filesystem, which is what lets this
 * run where it must: on the writer thread, inside the transaction that spends
 * the capability. */
static atlas_status op_revalidate(apply_ctx *ac, const atlas_decision_op *op,
                                  atlas_decision_result *out, atlas_err *err) {
    atlas_decision_challenge c;
    atlas_decision_challenge_init(&c);
    atlas_status st = spend_challenge(ac, op, ATLAS_DECISION_INTENT_REVALIDATE, &c, out, err);
    if (st != ATLAS_OK) {
        return st;
    }
    /* A16. See the identical check in `op_supersede`: a REMOTE challenge can
     * never carry a REVALIDATE intent -- `op_challenge` refuses to mint one --
     * so this cannot happen through Atlas, and is checked anyway. */
    if (c.channel == ATLAS_DECISION_CHANNEL_REMOTE) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "supersede and revalidate are not offered from the browser; use a "
                             "terminal on the Atlas machine");
    }
    out->actor = ATLAS_DECISION_ACTOR_LOCAL_OPERATOR_CONFIRMED;

    /* The revision must still be the effective one. A revalidation of something
     * that was superseded while the capability was in flight would establish a
     * validation point for a revision nothing reads. */
    int64_t current = 0;
    st = atlas_db_decision_current_revision(ac->db, c.document_id, &current, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (current != c.revision_id) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "revision %lld is no longer this decision's approved revision; "
                             "nothing was recorded",
                             (long long)c.revision_no);
    }

    /* Commit drift. */
    if (c.indexed_commit[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "that revalidation capability names no repository state");
    }
    if (strcmp(c.indexed_commit, ac->repo.scanned_head) != 0) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "the index moved from %s to %s while this was being confirmed; "
                             "nothing was recorded",
                             c.indexed_commit,
                             ac->repo.scanned_head[0] != '\0' ? ac->repo.scanned_head : "nothing");
    }

    /* Evidence drift. Recomputed here through the same function that produced
     * the bound value, so the two cannot differ by being computed differently. */
    atlas_decision_revision rev;
    atlas_decision_revision_init(&rev);
    bool found = false;
    st = atlas_db_decision_revision_load(ac->db, c.revision_id, &rev, &found, err);
    if (st == ATLAS_OK && !found) {
        st = atlas_err_set(err, ATLAS_ERR_INTEGRITY, "that revision is no longer there");
    }
    char digest[ATLAS_SHA256_HEX_LEN + 1u];
    digest[0] = '\0';
    if (st == ATLAS_OK) {
        st = atlas_gate_evidence_digest_for(ac->db, ac->repo.id, &rev, digest, err);
    }
    atlas_decision_revision_free(&rev);
    if (st != ATLAS_OK) {
        return st;
    }
    if (strcmp(digest, c.evidence_digest) != 0) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "the evidence this decision is bound to changed while this was being "
                             "confirmed; nothing was recorded");
    }

    atlas_db_gate_validation v;
    memset(&v, 0, sizeof v);
    v.document_id = c.document_id;
    v.revision_id = c.revision_id;
    v.revision_no = c.revision_no;
    (void)snprintf(v.content_hash, sizeof v.content_hash, "%s", c.content_hash);
    v.repo_id = ac->repo.id;
    (void)snprintf(v.repo_identity_hash, sizeof v.repo_identity_hash, "%s",
                   atlas_buf_cstr(&ac->repo_identity));
    (void)snprintf(v.validated_at_commit, sizeof v.validated_at_commit, "%s", c.indexed_commit);
    (void)snprintf(v.evidence_digest, sizeof v.evidence_digest, "%s", digest);
    v.challenge_id = c.id;
    (void)snprintf(v.prior_freshness, sizeof v.prior_freshness, "%s",
                   c.prior_freshness[0] != '\0' ? c.prior_freshness : "UNKNOWN");
    (void)snprintf(v.prior_reasons, sizeof v.prior_reasons, "%s", c.prior_reasons);
    (void)snprintf(v.created_at, sizeof v.created_at, "%s", ac->now);

    int64_t vid = 0;
    st = atlas_db_gate_validation_insert(ac->db, &v, &vid, err);
    if (st != ATLAS_OK) {
        return st;
    }

    out->document_id = c.document_id;
    out->revision_id = c.revision_id;
    out->revision_no = c.revision_no;
    /* Unchanged, and said out loud: revalidation is not a transition. */
    out->state = ATLAS_DECISION_APPROVED;
    out->validation_id = vid;
    (void)snprintf(out->content_hash, sizeof(out->content_hash), "%s", c.content_hash);
    (void)snprintf(out->indexed_commit, sizeof(out->indexed_commit), "%s", c.indexed_commit);
    (void)snprintf(out->evidence_digest, sizeof(out->evidence_digest), "%s", digest);
    return atlas_db_decision_uid_of(ac->db, c.document_id, &out->uid, err);
}

static atlas_status op_promote(apply_ctx *ac, const atlas_decision_op *op,
                               atlas_decision_result *out, atlas_err *err) {
    atlas_decision_revision legacy;
    atlas_decision_revision_init(&legacy);
    bool found = false;
    atlas_status st =
        atlas_db_decision_legacy_get(ac->db, ac->repo.id, op->legacy_id, &legacy, &found, err);
    if (st != ATLAS_OK) {
        atlas_decision_revision_free(&legacy);
        return st;
    }
    if (!found) {
        atlas_decision_revision_free(&legacy);
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "this repository has no A2 decision proposal with that id");
    }
    /* A promoted document is PROPOSED. It is not approved, not partially
     * approved, and carries no operator event: an A2 row could never have been
     * approved, and a migration that made one look approved would be the single
     * most damaging thing this phase could do. */
    int64_t document_id = 0;
    char uid[ATLAS_DECISION_UID_MAX];
    /* A9.1: a promoted A2 proposal is a DECISION, and the caller's kind is not
     * consulted. An `ai_decisions` row was written when Atlas had exactly one
     * semantic category, so `DECISION` is the only classification the source
     * evidence supports — inferring a richer one from prose would be Atlas
     * deciding what somebody meant. Reclassifying afterwards is the supersede
     * path, like every other reclassification. */
    st = atlas_db_decision_document_create(ac->db, ac->repo.id, ac->root_hash,
                                           ATLAS_DECISION_KIND_DECISION, ac->now, &document_id, uid,
                                           sizeof(uid), err);
    if (st == ATLAS_OK) {
        out->knowledge_kind = ATLAS_DECISION_KIND_DECISION;
        st = atlas_buf_set_str(&out->uid, uid, err);
    }
    if (st == ATLAS_OK) {
        out->document_id = document_id;
        out->document_created = true;
        /* **The session comes from the request, never from the A2 row**, and
         * that one rule gives the right answer in both cases this path serves.
         *
         * When `atlas_record_decision` materialises a document as part of the
         * same call, the request carries the live session key, `bind_session`
         * resolves it by exact key, and the revision records it — because that
         * session really did propose this record, now.
         *
         * When an operator promotes a historical A2 row from the CLI, there is
         * no session key in the request, so the revision is sessionless. The A2
         * row still records which session proposed the original, and
         * `imported_from_ai_decision_id` points at it. Copying that session
         * onto the new revision would claim it proposed a record created years
         * later, at somebody else's request. A2's rule applies: an honest gap
         * with a pointer beats a plausible attribution. */
        st = write_revision(ac, op, &legacy, document_id, 1, out, err);
    }
    atlas_decision_revision_free(&legacy);
    return st;
}

/* --- the entry point ---------------------------------------------------------- */

/* A9.2. A transition a root-owned verification policy authorised.
 *
 * This is the machine counterpart of `op_approve` and `op_resolve`, and it is
 * written to be their equal in strictness rather than their shortcut. Compare
 * it with `spend_challenge` line by line: both bind to one document, one
 * revision and one content hash; both rehash the stored content and refuse a
 * mismatch; both consume a single-use capability with an UPDATE that names the
 * state it observed, so a replay loses deterministically. The *only* thing that
 * differs between the two paths is who is able to mint the capability — an
 * operator at a terminal, or the verification engine under a policy neither
 * Atlas nor any model can edit.
 *
 * That equivalence is the whole security argument for automating anything here.
 * If the machine path bound more loosely than the human one, every gate in
 * front of it would be arguing about which evidence justifies a capability that
 * is easier to satisfy than the one a person needs.
 *
 * Three things this function will not do:
 *
 *   - it never *creates* the warrant. `atlas_verify_autolifecycle_run` writes
 *     the audit row before calling in, in the same transaction, so a transition
 *     and its justification commit together or neither does;
 *   - it never approves over an existing approved revision. That would be a
 *     supersession, and supersession needs an argument Atlas cannot make
 *     mechanically. The engine blocks the case before it gets here and this
 *     refuses it again, because two checks and one authority is the pattern;
 *   - it never widens what the state machine allows. `transition` reads the
 *     kind from the document and asks `atlas_decision_transition_allowed`,
 *     exactly as the operator path does.
 */
static atlas_status op_auto(apply_ctx *ac, const atlas_decision_op *op,
                            atlas_decision_result *out, atlas_err *err) {
    if (op->warrant_id <= 0) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "%s is a policy-authorised transition and needs a warrant; there is "
                             "no request that can supply one",
                             atlas_decision_op_kind_name(op->kind));
    }

    int64_t document_id = 0, doc_repo = 0;
    bool found = false;
    atlas_status st = atlas_db_decision_find_uid(ac->db, atlas_buf_cstr(&op->uid), &document_id,
                                                 &doc_repo, &found, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (!found) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "no knowledge record has that id");
    }
    if (doc_repo != ac->repo.id) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "that knowledge record belongs to a different repository");
    }

    const atlas_decision_state from = op->kind == ATLAS_DECISION_OP_AUTO_APPROVE
                                          ? ATLAS_DECISION_PROPOSED
                                          : ATLAS_DECISION_APPROVED;
    const atlas_decision_state to = op->kind == ATLAS_DECISION_OP_AUTO_APPROVE
                                        ? ATLAS_DECISION_APPROVED
                                        : ATLAS_DECISION_RESOLVED;

    /* Approving over something already approved is a supersession, and this
     * path does not make those. Checked before the warrant is examined so the
     * refusal names the real reason rather than a hash mismatch. */
    if (op->kind == ATLAS_DECISION_OP_AUTO_APPROVE) {
        int64_t current = 0;
        st = atlas_db_decision_approved_revision(ac->db, document_id, &current, err);
        if (st != ATLAS_OK) {
            return st;
        }
        if (current > 0) {
            return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                 "this record already has an approved revision, so approving "
                                 "another would replace it; replacing an approved record is an "
                                 "operator action and no policy performs it");
        }
    }

    /* The revision the warrant is about. Derived from the document and the
     * transition rather than taken from the request, for the reason `transition`
     * reads the kind from the document: the single write point must not let a
     * caller describe the thing it is changing. */
    int64_t revision_id = 0, revision_no = 0;
    if (op->kind == ATLAS_DECISION_OP_AUTO_RESOLVE) {
        /* The approved revision, derived through the partial unique index that
         * guarantees there is at most one, never read from the document's
         * cached pointer — `recompute_status` explains why that pointer is the
         * one resolving invalidates. */
        st = atlas_db_decision_approved_revision(ac->db, document_id, &revision_id, err);
    } else {
        char hash[ATLAS_SHA256_HEX_LEN + 1u];
        char state[16];
        st = atlas_db_decision_latest_revision(ac->db, document_id, &revision_id, &revision_no,
                                               hash, sizeof hash, state, sizeof state, err);
        if (st == ATLAS_OK && revision_id > 0 &&
            strcmp(state, atlas_decision_state_name(ATLAS_DECISION_PROPOSED)) != 0) {
            /* The newest revision is not the proposed one. Refused rather than
             * searching backwards for one that is: approving a revision that
             * is not the latest would make a policy adopt something an author
             * has already moved on from. */
            return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                 "the newest revision of this record is %s, not PROPOSED", state);
        }
    }
    if (st != ATLAS_OK) {
        return st;
    }
    if (revision_id <= 0) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "this record has no %s revision, so there is nothing to transition",
                             atlas_decision_state_name(from));
    }

    /* Rehash the stored content and compare against what the revision records,
     * exactly as `spend_challenge` does. A warrant bound to a digest the content
     * no longer has binds to nothing, and a policy acting on altered bytes is
     * the failure this check exists to make impossible. */
    atlas_decision_revision loaded;
    atlas_decision_revision_init(&loaded);
    bool lfound = false;
    st = atlas_db_decision_revision_load(ac->db, revision_id, &loaded, &lfound, err);
    if (st == ATLAS_OK && !lfound) {
        st = atlas_err_set(err, ATLAS_ERR_INTEGRITY, "that revision is no longer present");
    }
    char content_hash[ATLAS_SHA256_HEX_LEN + 1u];
    content_hash[0] = '\0';
    if (st == ATLAS_OK) {
        revision_no = loaded.revision_no;
        char rehash[ATLAS_SHA256_HEX_LEN + 1u];
        st = atlas_decision_content_hash(&loaded, rehash, err);
        if (st == ATLAS_OK && strcmp(rehash, loaded.content_hash) != 0) {
            st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                               "this revision's stored content does not hash to its recorded "
                               "value; Atlas refuses to transition it");
        }
        if (st == ATLAS_OK) {
            (void)snprintf(content_hash, sizeof content_hash, "%s", loaded.content_hash);
        }
    }
    atlas_decision_revision_free(&loaded);
    if (st != ATLAS_OK) {
        return st;
    }

    /* The warrant itself. Every binding is in the query rather than read back
     * and compared, so there is no window in which one value could be examined
     * and another acted on. */
    bool ok = false;
    st = atlas_db_verify_warrant_check(ac->db, op->warrant_id, document_id, revision_id,
                                       atlas_decision_state_name(to), content_hash, &ok, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (!ok) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "no live verification warrant authorises this exact transition of "
                             "this revision at this content hash");
    }
    bool spent = false;
    st = atlas_db_verify_warrant_consume(ac->db, op->warrant_id, ac->now, &spent, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (!spent) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "that verification warrant was already spent; a warrant authorises "
                             "exactly one transition");
    }

    st = transition(ac, document_id, revision_id, revision_no, from, to,
                    ATLAS_DECISION_ACTOR_VERIFICATION_POLICY, 0, 0, 0,
                    "authorised by a root-owned verification policy against a recorded "
                    "verification result; this records which policy acted, not that the record "
                    "is true and not that a person agreed",
                    content_hash, NULL, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = recompute_status(ac, document_id, err);
    if (st != ATLAS_OK) {
        return st;
    }
    bool kfound = false;
    st = atlas_db_decision_kind_of(ac->db, document_id, &out->knowledge_kind, &kfound, err);
    if (st != ATLAS_OK) {
        return st;
    }
    out->document_id = document_id;
    out->revision_id = revision_id;
    out->revision_no = revision_no;
    out->state = to;
    (void)snprintf(out->content_hash, sizeof(out->content_hash), "%s", content_hash);
    return atlas_db_decision_uid_of(ac->db, document_id, &out->uid, err);
}

atlas_status atlas_decision_apply_in_tx(atlas_db *db, const atlas_decision_op *op,
                                        atlas_decision_result *out, atlas_err *err) {
    if (atlas_db_is_readonly(db)) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "a decision transition was attempted on a read-only handle");
    }
    apply_ctx ac;
    memset(&ac, 0, sizeof(ac));
    ac.db = db;
    atlas_repo_info_init(&ac.repo);
    atlas_buf_init(&ac.repo_identity);
    atlas_now_iso8601(ac.now, sizeof(ac.now));

    /* No `atlas_db_begin` here: the transaction belongs to the caller.
     *
     * `atlas_decision_apply` opens one around this; `op_decision` in src/ai
     * opens one around this *and* the A2 row it pairs with. A begin here would
     * nest — `atlas_db_begin` counts depth — and the matching commit would only
     * decrement the counter, so the outer transaction would never be committed
     * and everything would be silently rolled back at close. That is exactly
     * what a stray second begin did during this correction, and it produced a
     * `propose` that reported success and wrote nothing. */
    atlas_status st = resolve_repo(&ac, op, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&out->repo_name, ac.repo.name, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&out->root_text, ac.repo.root_path_text.data,
                           ac.repo.root_path_text.len, err);
    }
    if (st == ATLAS_OK) {
        out->repo_id = ac.repo.id;
        /* Attribution is resolved for every operation, including the operator
         * ones — where it is resolved precisely so that it can be *discarded*.
         *
         * An approval is sessionless. It is never attached to a Claude session,
         * even when one is open and even when the request carried its id,
         * because attaching it would record that a conversation approved
         * something. Below, the operator branches never read `ac.session_id`. */
        st = bind_session(&ac, op, out, err);
    }
    /* A16. The channel, checked before anything about a capability is asked.
     *
     * Every op that mints one (CHALLENGE) or spends one (the five
     * `atlas_decision_op_needs_challenge` kinds) must name which path it
     * travelled. UNKNOWN is refused here rather than left for `spend_challenge`
     * or `op_challenge` to notice, because a caller that forgot to name a
     * channel is not a smaller version of a real request -- and refusing it at
     * one entry point, rather than trusting every future operation kind to
     * remember, is the same argument `atlas_decision_op_needs_challenge`
     * already makes about the token itself, one line below.
     *
     * REMOTE additionally authenticates here, before `op_challenge` or
     * `spend_challenge` runs: the verified key id is kept in `ac.key_id` for
     * both to read, because the actor and the ledger's `key_id` must come from
     * what Atlas just verified, never from anything the request merely
     * claimed. */
    if (st == ATLAS_OK) {
        bool needs_channel =
            atlas_decision_op_needs_challenge(op->kind) || op->kind == ATLAS_DECISION_OP_CHALLENGE;
        if (needs_channel) {
            if (op->channel == ATLAS_DECISION_CHANNEL_UNKNOWN) {
                st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                   "this operation names no channel; a capability is minted and "
                                   "spent through exactly one of LOCAL or REMOTE");
            } else if (op->channel == ATLAS_DECISION_CHANNEL_REMOTE) {
                st = atlas_decision_remote_verify(db, &op->remote_token, op->remote_expected_key_id,
                                                  ac.key_id, err);
            }
        }
    }
    if (st == ATLAS_OK) {
        if (atlas_decision_op_needs_challenge(op->kind)) {
            /* **The capability requirement, enforced here rather than only
             * inside each operation.**
             *
             * `spend_challenge` refuses an empty token too, but that is a check
             * each `op_*` has to remember to call: a future operation kind that
             * changed a lifecycle state and forgot would sail through. Asking
             * the classifier at the single entry point makes the guarantee a
             * property of `atlas_decision_apply` instead of a habit, which is
             * what the comment on `atlas_decision_op_needs_challenge` claims —
             * and a comment in this codebase is a contract. */
            if (op->token.len == 0) {
                st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                   "%s changes a decision's lifecycle state and needs an approval "
                                   "challenge; run it through `atlas decision` on a terminal",
                                   atlas_decision_op_kind_name(op->kind));
            }
            /* Sessionless, explicitly and unconditionally. */
            ac.session_id = 0;
            out->session_unbound = true;
            out->unbound_reason = NULL;
        }
    }
    if (st == ATLAS_OK) {
        switch (op->kind) {
        case ATLAS_DECISION_OP_PROPOSE: st = op_propose(&ac, op, out, err); break;
        case ATLAS_DECISION_OP_REVISE: st = op_revise(&ac, op, out, err); break;
        case ATLAS_DECISION_OP_CHALLENGE: st = op_challenge(&ac, op, out, err); break;
        case ATLAS_DECISION_OP_APPROVE: st = op_approve(&ac, op, out, err); break;
        case ATLAS_DECISION_OP_REJECT: st = op_reject(&ac, op, out, err); break;
        case ATLAS_DECISION_OP_SUPERSEDE: st = op_supersede(&ac, op, out, err); break;
        case ATLAS_DECISION_OP_PROMOTE: st = op_promote(&ac, op, out, err); break;
        case ATLAS_DECISION_OP_REVALIDATE: st = op_revalidate(&ac, op, out, err); break;
        case ATLAS_DECISION_OP_EDGE_NOTE: st = op_edge_note(&ac, op, out, err); break;
        case ATLAS_DECISION_OP_RESOLVE: st = op_resolve(&ac, op, out, err); break;
        case ATLAS_DECISION_OP_AUTO_APPROVE:
        case ATLAS_DECISION_OP_AUTO_RESOLVE: st = op_auto(&ac, op, out, err); break;
        }
    }
    atlas_repo_info_free(&ac.repo);
    atlas_buf_free(&ac.repo_identity);
    return st;
}

atlas_status atlas_decision_apply(atlas_db *db, const atlas_decision_op *op,
                                  atlas_decision_result *out, atlas_err *err) {
    atlas_status st = atlas_db_begin(db, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_decision_apply_in_tx(db, op, out, err);
    if (st != ATLAS_OK) {
        /* Whole or nothing. A half-applied approval — a spent challenge with no
         * event, or a superseded predecessor with no successor — is the one
         * outcome the ledger must never contain. */
        atlas_db_rollback(db);
        return st;
    }
    st = atlas_db_commit(db, err);
    if (st != ATLAS_OK) {
        atlas_db_rollback(db);
    }
    return st;
}
