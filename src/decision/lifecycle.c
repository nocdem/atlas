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

#include <stdio.h>
#include <string.h>

#include "atlas/ai.h"
#include "atlas/atlas.h"
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
    }
    return "propose";
}

bool atlas_decision_op_needs_challenge(atlas_decision_op_kind k) {
    /* Asked by `atlas_decision_apply` rather than remembered at each call site,
     * so that adding an operation kind forces a decision about it here instead
     * of defaulting it into the unauthenticated set. */
    return k == ATLAS_DECISION_OP_APPROVE || k == ATLAS_DECISION_OP_REJECT ||
           k == ATLAS_DECISION_OP_SUPERSEDE || k == ATLAS_DECISION_OP_REVALIDATE;
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
        st = atlas_db_decision_event_append(
            ac->db, document_id, rev_id, revision_no, "PROPOSED",
            atlas_decision_actor_name(rev->proposed_by), rev->content_hash, 0, 0, 0,
            revision_no == 1 ? "the first revision of this decision"
                             : "a new revision, proposed rather than applied to the approved one",
            op->dedup_key.len > 0 ? atlas_buf_cstr(&op->dedup_key) : NULL, NULL, err);
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
        st = atlas_db_decision_document_create(ac->db, ac->repo.id, ac->root_hash, ac->now,
                                               &document_id, uid, sizeof(uid), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&out->uid, uid, err);
    }
    if (st == ATLAS_OK) {
        out->document_id = document_id;
        out->document_created = true;
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

    atlas_decision_challenge c;
    atlas_decision_challenge_init(&c);
    st = atlas_decision_challenge_token(c.token, sizeof(c.token), err);
    if (st != ATLAS_OK) {
        return st;
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
static atlas_status spend_challenge(apply_ctx *ac, const atlas_decision_op *op,
                                    atlas_decision_intent want, atlas_decision_challenge *out_c,
                                    atlas_err *err) {
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
 * strongest fact, then an effective revision, then an outstanding proposal, and
 * only a document with nothing left is REJECTED. */
static atlas_status recompute_status(apply_ctx *ac, int64_t document_id, atlas_err *err) {
    int64_t current = 0;
    atlas_status st = atlas_db_decision_current_revision(ac->db, document_id, &current, err);
    if (st != ATLAS_OK) {
        return st;
    }
    int64_t superseded_by = 0;
    int64_t proposed = 0;
    st = atlas_db_decision_document_shape(ac->db, document_id, &superseded_by, &proposed, err);
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
    } else {
        status = "REJECTED";
    }
    return atlas_db_decision_document_set_state(ac->db, document_id, current, status, ac->now, err);
}

/* The conditional transition, plus the cache update that must accompany it. */
static atlas_status transition(apply_ctx *ac, int64_t document_id, int64_t revision_id,
                               int64_t revision_no, atlas_decision_state from,
                               atlas_decision_state to, atlas_decision_actor actor,
                               int64_t challenge_id, int64_t superseded_by_revision_id,
                               int64_t superseded_by_document_id, const char *detail,
                               const char *content_hash, atlas_err *err) {
    if (!atlas_decision_transition_allowed(from, to)) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "a decision revision that is %s cannot become %s",
                             atlas_decision_state_name(from), atlas_decision_state_name(to));
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
    return atlas_db_decision_event_append(ac->db, document_id, revision_id, revision_no,
                                          atlas_decision_state_name(to),
                                          atlas_decision_actor_name(actor), content_hash,
                                          challenge_id, superseded_by_revision_id,
                                          superseded_by_document_id, detail, NULL, NULL, err);
}

static atlas_status op_approve(apply_ctx *ac, const atlas_decision_op *op,
                               atlas_decision_result *out, atlas_err *err) {
    atlas_decision_challenge c;
    atlas_decision_challenge_init(&c);
    atlas_status st = spend_challenge(ac, op, ATLAS_DECISION_INTENT_APPROVE, &c, err);
    if (st != ATLAS_OK) {
        return st;
    }

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
        st = transition(ac, c.document_id, prev_rev_id, prev_rev_no, ATLAS_DECISION_APPROVED,
                        ATLAS_DECISION_SUPERSEDED, ATLAS_DECISION_ACTOR_ATLAS_AUTOMATIC, c.id,
                        c.revision_id, 0,
                        "replaced by a later revision of the same decision, which was approved in "
                        "the same transaction",
                        NULL, err);
        if (st != ATLAS_OK) {
            return st;
        }
        out->superseded_revision_no = prev_rev_no;
    }

    st = transition(ac, c.document_id, c.revision_id, c.revision_no, ATLAS_DECISION_PROPOSED,
                    ATLAS_DECISION_APPROVED, ATLAS_DECISION_ACTOR_LOCAL_OPERATOR_CONFIRMED, c.id, 0,
                    0,
                    "confirmed through the Atlas local operator channel; this records that the "
                    "channel was used, not which person used it",
                    c.content_hash, err);
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
    atlas_status st = spend_challenge(ac, op, ATLAS_DECISION_INTENT_REJECT, &c, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = transition(ac, c.document_id, c.revision_id, c.revision_no, ATLAS_DECISION_PROPOSED,
                    ATLAS_DECISION_REJECTED, ATLAS_DECISION_ACTOR_LOCAL_OPERATOR_CONFIRMED, c.id, 0,
                    0, "refused through the Atlas local operator channel", c.content_hash, err);
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

static atlas_status op_supersede(apply_ctx *ac, const atlas_decision_op *op,
                                 atlas_decision_result *out, atlas_err *err) {
    atlas_decision_challenge c;
    atlas_decision_challenge_init(&c);
    atlas_status st = spend_challenge(ac, op, ATLAS_DECISION_INTENT_SUPERSEDE, &c, err);
    if (st != ATLAS_OK) {
        return st;
    }
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
                            cur.content_hash, err);
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
    atlas_status st = spend_challenge(ac, op, ATLAS_DECISION_INTENT_REVALIDATE, &c, err);
    if (st != ATLAS_OK) {
        return st;
    }

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
    st = atlas_db_decision_document_create(ac->db, ac->repo.id, ac->root_hash, ac->now,
                                           &document_id, uid, sizeof(uid), err);
    if (st == ATLAS_OK) {
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
