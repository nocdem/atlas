/* Atlas - the decision lifecycle: one typed operation, one write point.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * This header exists for the same reason `atlas_ai_op` does: the daemon's
 * writer thread needs a single typed entry point rather than one job kind per
 * verb, and the IPC layer needs to be able to validate a request completely
 * before anything is queued.
 *
 * It is separate from atlas/decision.h because it depends on atlas/db.h, which
 * depends on atlas/decision.h. The split is along the same line as everywhere
 * else in Atlas: decision.h is about meaning, this is about applying it.
 *
 * **`atlas_decision_apply_in_tx` is the only function in Atlas that writes a
 * lifecycle transition.** The actor restriction, the transition table, the
 * challenge consumption, the atomic approve-and-supersede, the cycle check and
 * the cache update all live behind it, and every one of them would be trivially
 * bypassable if a second path reached the tables.
 *
 * It has exactly two callers, both declared below:
 *
 *   - `atlas_decision_apply`, the public entry point, which owns the
 *     transaction and adds nothing else;
 *   - `op_decision_locked` in `src/ai/ai.c`, the A2 bridge, which already owns
 *     one because its A2 row and its A4 document must commit together.
 *
 * So there are two ways to *call* the write point and still only one place the
 * rules live. The alternative — a nested transaction inside the bridge — was
 * rejected because `atlas_db_begin` counts depth and rollback does not; see the
 * comment on `atlas_decision_apply_in_tx`.
 */
#ifndef ATLAS_DECISION_OPS_H
#define ATLAS_DECISION_OPS_H

#include <stdbool.h>
#include <stdint.h>

#include "atlas/buf.h"
#include "atlas/db.h"
#include "atlas/decision.h"
#include "atlas/error.h"

typedef enum atlas_decision_op_kind {
    /* Create a new document with revision 1. */
    ATLAS_DECISION_OP_PROPOSE = 0,
    /* Add a new proposed revision to an existing document. Never edits one:
     * that is the whole difference between this and an update. */
    ATLAS_DECISION_OP_REVISE,
    /* Issue a short-lived, single-use capability bound to one revision. This is
     * the only operation that *creates* the ability to change a lifecycle
     * state, and it changes none itself. */
    ATLAS_DECISION_OP_CHALLENGE,
    ATLAS_DECISION_OP_APPROVE,
    ATLAS_DECISION_OP_REJECT,
    /* Mark one document replaced by another. Operator-only, like approval. */
    ATLAS_DECISION_OP_SUPERSEDE,
    /* Create an A4 document from an A2 `ai_decisions` proposal. The A2 row is
     * read and left exactly as it was. */
    ATLAS_DECISION_OP_PROMOTE,
    /* A6. Record that an operator checked an already-approved revision against
     * one exact repository state, establishing a new validation point.
     *
     * It consumes a capability like every other operator action, and it changes
     * no lifecycle state: the revision was APPROVED before and is APPROVED
     * after, no `decision_events` row is written, and the ledger replay is over
     * exactly the vocabulary it was over before. What it appends is a
     * `decision_validations` row, which is a different ledger recording a
     * different kind of act — not "this became policy" but "somebody checked
     * that it still describes this code". */
    ATLAS_DECISION_OP_REVALIDATE,
    /* Migration 10: attach an explanation to an edge that already exists.
     * Writes one append-only row and nothing else — no revision, no content
     * hash, no status change, no capability. */
    ATLAS_DECISION_OP_EDGE_NOTE,
    /* A9.1. Close out an approved record whose demand has been met: an
     * OBLIGATION that was discharged, an ACCEPTED_RISK that was eliminated.
     *
     * It consumes a capability exactly as approve, reject and supersede do, and
     * it is refused for a kind whose approved form makes no demand. Nothing is
     * deleted and no prose is rewritten: the revision moves from APPROVED to
     * RESOLVED, one `decision_events` row records that it did, and the document
     * stops being effective. */
    ATLAS_DECISION_OP_RESOLVE
} atlas_decision_op_kind;

const char *atlas_decision_op_kind_name(atlas_decision_op_kind k);
/* True when this operation changes a lifecycle state and therefore requires a
 * consumed challenge. Asked by `atlas_decision_apply` itself, so a new
 * operation kind cannot quietly become an unauthenticated transition. */
bool atlas_decision_op_needs_challenge(atlas_decision_op_kind k);

typedef struct atlas_decision_op {
    atlas_decision_op_kind kind;

    /* Where. Exactly one is normally set; `root` wins, as in A2. */
    atlas_buf repo_name;
    atlas_buf root;

    /* Which document, by public uid. Empty for PROPOSE and PROMOTE. */
    atlas_buf uid;
    /* SUPERSEDE: the document that replaces `uid`. */
    atlas_buf replacement_uid;

    /* PROPOSE and REVISE: the content. Validated before it reaches the writer
     * and validated again at the write point. */
    atlas_decision_revision revision;

    /* A9.1. PROPOSE: which sort of knowledge record to create. Defaults to
     * DECISION, because the enum's zero is DECISION and a caller that says
     * nothing means what every caller written before A9.1 meant.
     *
     * On REVISE it is checked rather than applied: a revision of a document
     * whose kind differs is refused, naming supersede. A kind is a property of
     * the document, so "revise it into a different kind" is a request to change
     * what a durable record has always been, and the honest form of that is a
     * new record that replaces this one. `knowledge_kind_given` distinguishes
     * "the caller asked for DECISION" from "the caller said nothing", so a
     * revise of a POLICY by a client that has never heard of kinds is not
     * refused for asserting DECISION it never asserted. */
    atlas_decision_kind knowledge_kind;
    bool knowledge_kind_given;

    /* Attribution, following A2's rule exactly: a session is found by its key
     * and by nothing else, and a record that cannot be attached is stored
     * sessionless rather than attached to a neighbour. */
    atlas_buf provider;
    atlas_buf client;
    atlas_buf session_key;

    /* Idempotency for a retried propose or revise. */
    atlas_buf dedup_key;

    /* The operator channel. `token` is the challenge the CLI was issued, and
     * `confirmation` is what the operator typed. Both are checked at the write
     * point; there is no boolean anywhere that says "this came from a
     * terminal", because a boolean in a request is a boolean a request can
     * assert. The capability is the evidence. */
    atlas_buf token;
    atlas_buf confirmation;

    /* CHALLENGE: which revision the caller believes it is acting on. 0 means
     * the newest. Binding to it is what makes "the document changed while you
     * were reading it" a refusal rather than a surprise. */
    int64_t expect_revision_no;
    /* CHALLENGE: what the capability will be allowed to do.
     *
     * Part of the bound tuple rather than a parameter of the later request,
     * because a capability issued to reject something must not approve it. A
     * request that spends a capability names its own operation, and the two
     * must agree — so an attacker who obtained a rejection capability cannot
     * turn it into an approval by asking differently. */
    atlas_decision_intent intent;

    /* PROMOTE: the `ai_decisions` row id. */
    int64_t legacy_id;

    /* CHALLENGE with a REVALIDATE intent: the assessment the operator is being
     * shown, so the validation record preserves what was actually seen rather
     * than what a recomputation a moment later would have produced.
     *
     * Both are closed A6 vocabularies and are checked against them at the write
     * point rather than trusted: they arrive from a caller, and a caller is not
     * the authority on what an A6 reason code is. They are plain buffers rather
     * than A6 types because atlas/gate.h depends on atlas/decision.h. */
    atlas_buf prior_freshness;
    atlas_buf prior_reasons;

    /* Migration 10: the durable account of a decision-to-decision edge.
     *
     * Carried on the op rather than written beside it so that the account and
     * the edge commit together. On a REVISE the note describes the edge the
     * revision gains (`ADDED`) or the one it drops (`REMOVED`), and it is
     * written inside the same transaction as the revision — a rationale that
     * could survive a rolled-back revise would explain an edge that does not
     * exist. On an EDGE_NOTE it is `ANNOTATED`: an explanation attached to an
     * edge that is already there, which writes no revision at all and so
     * changes no content hash and moves no status.
     *
     * Empty `edge_target_uid` means the op carries no edge account, which is
     * every op that predates this. */
    atlas_buf edge_target_uid;
    atlas_buf edge_event;      /* ADDED | ANNOTATED | REMOVED */
    atlas_buf edge_note;       /* the rationale, or why it was withdrawn */
    atlas_buf edge_provenance; /* OPERATOR | D1_MANIFEST | D3_REPAIR | UNKNOWN */
} atlas_decision_op;

void atlas_decision_op_init(atlas_decision_op *op, atlas_decision_op_kind kind);
void atlas_decision_op_free(atlas_decision_op *op);

typedef struct atlas_decision_result {
    int64_t repo_id;
    atlas_buf repo_name;
    atlas_buf root_text;

    atlas_buf uid;
    int64_t document_id;
    int64_t revision_id;
    int64_t revision_no;
    char content_hash[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_decision_state state;
    /* A9.1. The document's kind, reported by every operation that resolved one:
     * `propose` echoes what it created, and the operator operations echo what
     * they acted on, so a caller never has to guess and a refusal can say what
     * the record actually is. */
    atlas_decision_kind knowledge_kind;

    /* CHALLENGE only. `title` is the revision's title — untrusted prose that
     * the CLI displays, encoded, so the operator sees what they are approving.
     * `confirm` is the phrase they must type. */
    atlas_buf token;
    atlas_buf title;
    char confirm[ATLAS_DECISION_CONFIRM_MAX];
    char expires_at[ATLAS_TS_MAX];

    /* CHALLENGE with a REVALIDATE intent, and REVALIDATE itself: the exact
     * repository state and evidence digest the capability is bound to. Reported
     * so the operator prompt can show what is being revalidated against. */
    char indexed_commit[ATLAS_OID_HEX_MAX_INCL];
    char evidence_digest[ATLAS_SHA256_HEX_LEN + 1u];
    /* REVALIDATE only: the ledger row that was appended. */
    int64_t validation_id;

    /* APPROVE only, when the approval replaced an effective revision. */
    int64_t superseded_revision_no;
    /* SUPERSEDE only. */
    atlas_buf replaced_by_uid;

    bool document_created;
    bool duplicate; /* a dedup key or an identical content hash absorbed it */
    bool session_unbound;
    const char *unbound_reason; /* a string literal, never allocated */
} atlas_decision_result;

void atlas_decision_result_init(atlas_decision_result *r);
void atlas_decision_result_free(atlas_decision_result *r);

/* Applies one operation.
 *
 * `db` must be a writable handle owned by the calling thread — in the daemon,
 * the writer thread and only the writer thread. The whole operation runs in one
 * transaction, so a challenge cannot be spent without the transition it
 * authorised, and an approval cannot happen without the supersession it
 * implies.
 *
 * A conflicting concurrent transition fails with ATLAS_ERR_INTEGRITY and a
 * message naming what it expected, rather than winning by being second. */
atlas_status atlas_decision_apply(atlas_db *db, const atlas_decision_op *op,
                                  atlas_decision_result *out, atlas_err *err);

/* The same operation, assuming the caller already holds a write transaction.
 *
 * This exists for exactly one caller: A2's `atlas_record_decision` path, which
 * has to write an A2 row *and* materialise the A4 document it maps to, and must
 * do both or neither. Opening a nested transaction would work — `atlas_db_begin`
 * counts depth — but the rollback would not: `atlas_decision_apply` rolls back
 * on failure, and rolling back a nested depth would discard the caller's work
 * too, silently. So the transaction boundary belongs to whoever owns the whole
 * unit, and this is the entry point for a caller that does.
 *
 * Every rule `atlas_decision_apply` enforces is enforced here; the wrapper adds
 * only begin, commit and rollback. There is still exactly one place that writes
 * a lifecycle transition. */
atlas_status atlas_decision_apply_in_tx(atlas_db *db, const atlas_decision_op *op,
                                        atlas_decision_result *out, atlas_err *err);

/* Builds the searchable projection of a revision: lowercased, bounded, and
 * assembled from the revision's own text. Exposed so the perf fixture and the
 * tests can produce the same bytes the writer does. */
atlas_status atlas_decision_haystack(const atlas_decision_revision *r, atlas_buf *out,
                                     atlas_err *err);

#endif /* ATLAS_DECISION_OPS_H */
