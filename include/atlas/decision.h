/* Atlas - decision documents, immutable revisions and operator approval.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A2 could record that a model *proposed* something. A4 is the phase in which a
 * proposal can become project policy — and the whole difficulty of the phase is
 * in what that sentence is allowed to mean.
 *
 * Three rules shape this header, and each of them is a rule about honesty
 * rather than about storage.
 *
 * 1. **Atlas cannot prove a natural person acted.** It can observe that an
 *    explicit action arrived through its own operator-only interactive channel,
 *    and that is exactly what `ATLAS_DECISION_ACTOR_LOCAL_OPERATOR_CONFIRMED`
 *    says: an approval happened through that channel. It does not say which
 *    person was at the keyboard, or that one was there at all, and it offers no
 *    non-repudiation. Any process running as the same local user that can drive
 *    a pseudo-terminal may imitate the channel — including an AI agent with
 *    shell access — and nothing here pretends otherwise. There are no keys, no
 *    signatures and no hardware tokens in A4, and adding the words would not
 *    add the property.
 *
 *    What Atlas *does* guarantee is about its own surface, and is checkable:
 *    no MCP tool, no hook, no AI-facing method and no request argument grants
 *    an approval, and conversation text changes no lifecycle state.
 *
 *    A2's `USER_APPROVED_DECISION` provenance therefore stays unwritten. It
 *    claims a natural person approved something, which is the claim this phase
 *    exists to *not* make.
 *
 * 2. **Approved prose is accepted policy, never system instruction.** Approval
 *    changes the status of a record. It does not change the nature of the
 *    bytes: a decision body is still text somebody or something else wrote, it
 *    is still `UNTRUSTED_DATA` wherever it is reported, and it never enters
 *    automatic model context. "The operator approved this document" and "this
 *    document may tell a model what to do" are unrelated statements, and
 *    conflating them would turn an approval button into a prompt-injection
 *    channel.
 *
 * 3. **A revision is immutable and the ledger is canonical.** No content column
 *    of `decision_revisions` is ever updated; a change is a new revision. Every
 *    lifecycle transition is an append to `decision_events`, and the status
 *    columns on the document and the revision are a cache of that ledger,
 *    written in the same transaction and checkable against it.
 *
 * This is also the first Atlas data that is **not** a rebuildable index. See
 * docs/decision-lifecycle.md; the exception is deliberate, narrow, and stated
 * where invariant 1 is stated.
 */
#ifndef ATLAS_DECISION_H
#define ATLAS_DECISION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "atlas/buf.h"
#include "atlas/error.h"
#include "atlas/limits.h"
#include "atlas/sha256.h"

/* --- lifecycle -----------------------------------------------------------
 *
 * A closed Atlas-owned vocabulary. Four states, and the transitions between
 * them are the whole of what an approval workflow is.
 *
 * The same four names describe a *revision's* status and an *event* in the
 * ledger, because they are the same fact seen twice: the ledger records that a
 * transition happened, the status column records where it left the revision. */
typedef enum atlas_decision_state {
    /* Written down, not accepted. Everything a model can produce stops here. */
    ATLAS_DECISION_PROPOSED = 0,
    /* Accepted through the operator channel, and currently effective. */
    ATLAS_DECISION_APPROVED,
    /* Explicitly refused. Terminal: a rejected revision can never be approved,
     * because "we said no and then it quietly became policy" is precisely the
     * failure an approval ledger exists to make impossible. */
    ATLAS_DECISION_REJECTED,
    /* Was approved, and a later approval replaced it. Terminal, and historical
     * rather than deleted: what was policy at a point in time is a fact. */
    ATLAS_DECISION_SUPERSEDED
} atlas_decision_state;

const char *atlas_decision_state_name(atlas_decision_state s);
bool atlas_decision_state_parse(const char *name, atlas_decision_state *out);
/* The transition table, as a function rather than as prose in a comment.
 *
 * This is the single authority on what may follow what; `lifecycle.c` asks it
 * and so do the tests, so a test cannot pass by agreeing with a second copy of
 * the rules. */
bool atlas_decision_transition_allowed(atlas_decision_state from, atlas_decision_state to);

/* --- who did it ----------------------------------------------------------
 *
 * Deliberately *not* `atlas_provenance`. That vocabulary answers "how does
 * Atlas know this value?"; this one answers "what kind of actor caused this
 * transition?", and the two questions have different wrong answers.
 *
 * There are four actors and there will not casually be a fifth: the set is
 * small precisely so that a reader can check it. */
typedef enum atlas_decision_actor {
    /* A model wrote this down deliberately. The only actor that may propose
     * through MCP, and the only one a hook can produce. */
    ATLAS_DECISION_ACTOR_MODEL_PROPOSAL = 0,
    /* A model derived it rather than being told it. Weaker than a proposal. */
    ATLAS_DECISION_ACTOR_MODEL_INFERENCE,
    /* An explicit action arrived through Atlas' operator-only interactive
     * channel: a real terminal, a short-lived single-use daemon challenge bound
     * to this exact revision and content hash, and a confirmation typed against
     * that hash.
     *
     * Read the name literally. It says the channel was used. It does not name a
     * person, does not prove a person, and is not a signature. A process
     * running as the same user could produce it. */
    ATLAS_DECISION_ACTOR_LOCAL_OPERATOR_CONFIRMED,
    /* Atlas itself, for a transition that follows mechanically from another —
     * the supersession that an approval implies. Never a judgement. */
    ATLAS_DECISION_ACTOR_ATLAS_AUTOMATIC
} atlas_decision_actor;

const char *atlas_decision_actor_name(atlas_decision_actor a);
bool atlas_decision_actor_parse(const char *name, atlas_decision_actor *out);
/* True when an adapter reachable over IPC without the operator channel may
 * record this actor. Refuses LOCAL_OPERATOR_CONFIRMED and ATLAS_AUTOMATIC,
 * mirroring `atlas_provenance_writable_in_a2` and
 * `atlas_code_resolution_writable_in_a3`: the restriction is a function, in one
 * place, checked at the single write point rather than remembered at each
 * call site. */
bool atlas_decision_actor_writable_by_adapter(atlas_decision_actor a);

/* --- what a revision is about --------------------------------------------
 *
 * A closed vocabulary for the breadth of a decision, so that "this is about the
 * whole repository" and "this is about three files" are distinguishable without
 * counting links. */
typedef enum atlas_decision_scope {
    ATLAS_DECISION_SCOPE_UNKNOWN = 0,
    ATLAS_DECISION_SCOPE_REPOSITORY,
    ATLAS_DECISION_SCOPE_SUBSYSTEM,
    ATLAS_DECISION_SCOPE_PATHS
} atlas_decision_scope;

const char *atlas_decision_scope_name(atlas_decision_scope s);
bool atlas_decision_scope_parse(const char *name, atlas_decision_scope *out);

/* --- links ----------------------------------------------------------------
 *
 * What a revision says it concerns.
 *
 * **No link is a foreign key into the A3 structural tables.** A `code_symbols`
 * row is derived data that a rebuild deletes and an analyzer upgrade replaces;
 * a decision is not, and a durable record whose meaning evaporates when a cache
 * is rebuilt is not a durable record. A symbol link is therefore a *selector
 * snapshot* — enough recorded provenance to attempt the resolution again later,
 * and enough to say honestly that it no longer resolves. */
typedef enum atlas_decision_link_kind {
    /* A repository-relative path, by raw bytes like every path in Atlas. */
    ATLAS_DECISION_LINK_PATH = 0,
    /* A commit object id. */
    ATLAS_DECISION_LINK_COMMIT,
    /* An A2 change set: the window in which one session could have changed one
     * repository. A soft reference; the change set may be pruned. */
    ATLAS_DECISION_LINK_CHANGE_SET,
    /* A symbol, by snapshot rather than by row id. See atlas_decision_link. */
    ATLAS_DECISION_LINK_SYMBOL,
    /* This document supersedes another. */
    ATLAS_DECISION_LINK_SUPERSEDES,
    /* This document is replaced by another. Recorded on the superseded side so
     * a reader of the old document is told where to look without a join. */
    ATLAS_DECISION_LINK_REPLACED_BY
} atlas_decision_link_kind;

const char *atlas_decision_link_kind_name(atlas_decision_link_kind k);
bool atlas_decision_link_kind_parse(const char *name, atlas_decision_link_kind *out);

/* Whether a link still points at what it pointed at.
 *
 * Computed when a link is read, from the snapshot and the current index, and
 * never cached: a cached currency is a value that is wrong between the change
 * and the recomputation, and "is this decision still about this code?" is
 * exactly the question that must not be answered from a stale cache.
 *
 * **Atlas never re-points a link.** A renamed or deleted anchor is reported as
 * missing, and an anchor that now matches several symbols is reported as
 * ambiguous with the count. Choosing would be inventing. */
typedef enum atlas_decision_link_currency {
    /* Atlas cannot say. No snapshot was recorded, or the index holds nothing
     * about the anchor — including because the repository was never scanned. */
    ATLAS_DECISION_LINK_UNKNOWN = 0,
    /* The anchor resolves, and its recorded content identity still matches. */
    ATLAS_DECISION_LINK_CURRENT,
    /* The anchor resolves and its content has changed since the snapshot. The
     * decision still stands; the link is flagged for review. */
    ATLAS_DECISION_LINK_CHANGED,
    /* The anchor no longer resolves: the path is gone, or no symbol of that
     * name and kind is recorded any more. */
    ATLAS_DECISION_LINK_MISSING,
    /* Several anchors match the snapshot and Atlas will not choose. */
    ATLAS_DECISION_LINK_AMBIGUOUS
} atlas_decision_link_currency;

const char *atlas_decision_link_currency_name(atlas_decision_link_currency c);

/* One link, as written and as read.
 *
 * The symbol fields are the snapshot. They are all recorded together or not at
 * all, because a partial snapshot resolves *more* loosely than a complete one
 * and would silently widen over time. */
typedef struct atlas_decision_link {
    atlas_decision_link_kind kind;

    /* PATH, and the file half of SYMBOL. Raw bytes are the key; the text form
     * is the lossless safe encoding, exactly as everywhere else in Atlas. */
    atlas_buf path_raw;
    atlas_buf path_text;

    /* COMMIT. Validated hex before it is stored. */
    atlas_buf commit_oid;

    /* CHANGE_SET. A soft reference; 0 for none. */
    int64_t change_set_id;

    /* SUPERSEDES and REPLACED_BY: the other document's public uid. */
    atlas_buf target_uid;

    /* SYMBOL snapshot. */
    atlas_buf symbol_name;      /* raw identifier bytes */
    atlas_buf symbol_name_text; /* safe encoding */
    atlas_buf symbol_kind;      /* an A3 symbol-kind name, or empty for any */
    int64_t symbol_line;        /* 0 when not recorded */
    /* The commit the snapshot was taken against, the content hash of the file
     * it was taken from, and which analyzer produced the facts. All three are
     * needed to say *why* a later resolution differs, and the analyzer identity
     * in particular distinguishes "the code changed" from "Atlas changed its
     * mind about the code". */
    atlas_buf basis_commit;
    atlas_buf file_content_hash;
    atlas_buf analyzer_name;
    int64_t analyzer_version;

    /* Read side only: never stored. */
    atlas_decision_link_currency currency;
    int64_t match_count; /* candidates found when resolving; 0, 1 or more */
} atlas_decision_link;

void atlas_decision_link_init(atlas_decision_link *l, atlas_decision_link_kind kind);
void atlas_decision_link_free(atlas_decision_link *l);

/* --- a revision -----------------------------------------------------------
 *
 * The bounded, structured content of one immutable revision. Every text field
 * is safe-encoded and length-checked before it reaches here, and strict UTF-8
 * is required: a decision document is durable, canonical and human-read, and
 * "we stored whatever bytes arrived" is not a property it may have. */
typedef struct atlas_decision_revision {
    int64_t id;          /* row id; 0 before it is written */
    int64_t document_id; /* row id of the owning document */
    int64_t revision_no; /* 1-based, dense, per document */

    atlas_buf title;
    atlas_buf context_text;      /* the problem, the situation */
    atlas_buf decision_text;     /* what was decided */
    atlas_buf rationale_text;    /* why */
    atlas_buf consequences_text; /* what follows from it */

    /* Alternatives considered, in the order given. An ordered list rather than
     * one blob because "we considered three things" is a countable fact and a
     * blob makes it a reading exercise. */
    atlas_buf alternatives[ATLAS_DECISION_MAX_ALTERNATIVES];
    size_t alternative_count;

    atlas_decision_scope scope;

    atlas_decision_link links[ATLAS_DECISION_MAX_LINKS];
    size_t link_count;

    /* Who proposed this revision, and what Atlas could establish about the
     * conversation it came from. A2's attribution rules apply unchanged: a
     * session is found by its key and by nothing else, and a record that cannot
     * be attached exactly is stored sessionless rather than attached to a
     * neighbour. */
    atlas_decision_actor proposed_by;
    int64_t session_id; /* 0 when none */
    bool session_unbound;
    atlas_buf unbound_reason; /* one of the ATLAS_AI_UNBOUND_* literals */

    /* The repository HEAD when the revision was proposed, when it was known.
     * Empty is a real answer: a proposal made with no daemon and no scan has no
     * basis commit, and inventing one would be worse than admitting it. */
    atlas_buf basis_head;
    /* The repository identity **as captured when this revision was written**,
     * stored on the revision and never afterwards changed.
     *
     * Part of the canonical content, because "we decided this about *that*
     * repository" is part of what is approved, and a row id is not durable.
     * Filled by the write path from the repository's identity at that instant;
     * never supplied by a caller.
     *
     * **This is deliberately not the document's `repo_identity_hash`.** That
     * one is *attachment* metadata: it is empty until a repository's history
     * has been ingested, is backfilled when it becomes knowable, and governs
     * relinking. Hashing the document's copy would mean an ordinary
     * propose-then-scan silently changed the input used to verify an already
     * written revision, and `atlas doctor` would report a perfectly healthy
     * record as corrupt. It did, before this field existed.
     *
     * Empty is a real, recorded value meaning "no identity was knowable then",
     * and it stays empty forever. A later revision of the same document
     * captures whatever is knowable at *its* write time, so the two legitimately
     * differ. */
    atlas_buf basis_repo_identity;

    char content_hash[ATLAS_SHA256_HEX_LEN + 1u];
    char created_at[ATLAS_TS_MAX];

    atlas_decision_state state;

    /* Set when this revision was created by promoting an A2 `ai_decisions`
     * proposal, and carrying that row's id. The A2 row is never modified and
     * never deleted; this is a pointer from the new record to the old one. */
    int64_t imported_from_ai_decision_id;
} atlas_decision_revision;

void atlas_decision_revision_init(atlas_decision_revision *r);
void atlas_decision_revision_free(atlas_decision_revision *r);
/* Appends an alternative, refusing past ATLAS_DECISION_MAX_ALTERNATIVES rather
 * than dropping one silently. */
atlas_status atlas_decision_revision_add_alternative(atlas_decision_revision *r, const char *text,
                                                     size_t len, atlas_err *err);
/* Appends a link, taking ownership of `link`'s buffers on success. On failure
 * the caller still owns them. */
atlas_status atlas_decision_revision_add_link(atlas_decision_revision *r,
                                              atlas_decision_link *link, atlas_err *err);

/* --- the canonical content hash -------------------------------------------
 *
 * A domain-separated, length-prefixed encoding of exactly the content fields,
 * hashed with Atlas' own SHA-256.
 *
 * Length-prefixed rather than delimited because a delimiter is a byte the
 * content can contain: with `title|decision` an "a|b" title and a "b" decision
 * hash the same as an "a" title and a "b|b" decision, and a hash that two
 * different documents share is not an identity. Domain-separated so that this
 * digest can never collide with a file content hash, a root hash or a compile
 * command hash — those are all SHA-256 of raw bytes, and a bare SHA-256 says
 * nothing about what was hashed.
 *
 * **Everything immutable that changes what was approved is hashed.** That
 * includes each link's whole snapshot — the basis commit it was taken against,
 * the file content hash captured at the time, and the analyzer name and version
 * that produced the structural facts — plus the revision's own basis HEAD, the
 * durable repository identity, and the proposing actor.
 *
 * An earlier version excluded the snapshot provenance, on the argument that
 * hashing it would make a revision's identity depend on which commit happened
 * to be checked out. That argument was wrong twice over. It *should* depend on
 * that: a decision taken against commit X is not the same decision taken
 * against commit Y, and an approval that did not cover the basis would let the
 * basis be rewritten under it. And the retry case the argument was protecting
 * is served by the explicit dedup key, which is what a retry actually carries.
 *
 * What is **not** hashed is everything database-local or recomputed: the
 * document and revision row ids, the revision number, the creation timestamp,
 * the session binding, the lifecycle state, the dedup key, the import pointer,
 * the `%XX` display encodings that are derived from the raw bytes beside them,
 * and every live currency result. A live currency is an observation about the
 * index made after the fact; hashing it would make an approved revision's
 * identity change when the code changed, which is precisely backwards. The
 * full field-by-field table is in docs/decision-lifecycle.md.
 *
 * The document's identity and revision number are not hashed either, so two
 * documents that genuinely say the same thing about the same repository at the
 * same basis do share a content hash. That is a fact rather than a collision;
 * exactness at approval comes from binding all three: document uid, revision
 * number, content hash.
 *
 * Links participate, in a canonical order Atlas imposes rather than the order
 * they were supplied: a set of links reordered is the same set, and a revision
 * whose hash changed because a caller shuffled its arguments would make every
 * retry a new revision. Alternatives keep their order, because a list of
 * alternatives is ordered by the proposer's judgement.
 *
 * Writes lowercase hex into `hex_out`, which needs ATLAS_SHA256_HEX_LEN + 1. */
#define ATLAS_DECISION_HASH_DOMAIN "atlas.decision.revision.v2"
atlas_status atlas_decision_content_hash(const atlas_decision_revision *r, char *hex_out,
                                         atlas_err *err);
/* The exact byte string that is hashed, for tests and for `decision show
 * --json`'s `canonical_bytes` accounting. Never rendered to a terminal. */
atlas_status atlas_decision_canonical_bytes(const atlas_decision_revision *r, atlas_buf *out,
                                            atlas_err *err);

/* --- validation -----------------------------------------------------------
 *
 * One function, called at the single write point and again by the IPC layer
 * before anything is queued, because a bound that is only checked at the edge
 * is a bound that a second edge will not have. */

/* Strict UTF-8, no NUL, no C0 or C1 control except that a body may contain
 * newlines and tabs. `field` names the field in the error message and is an
 * Atlas string literal, never caller text. */
atlas_status atlas_decision_check_text(const char *field, const char *text, size_t len,
                                       size_t max_len, bool allow_newlines, atlas_err *err);
/* Everything above, applied to a whole revision. */
atlas_status atlas_decision_revision_validate(const atlas_decision_revision *r, atlas_err *err);

/* A public uid is `atlas-dec-` followed by exactly ATLAS_DECISION_UID_HEX
 * lowercase hex characters, and nothing else. Checked on the way in as well as
 * on the way out: it is the one decision-derived value the automatic context
 * envelope may carry, so its shape is a boundary rather than a convention. */
bool atlas_decision_uid_is_valid(const char *uid);
/* Derives the uid for a new document.
 *
 * 128 bits, from values Atlas itself chose plus fresh local entropy: the
 * repository identity hash, the document's row id, its creation timestamp, and
 * ATLAS_DECISION_UID_ENTROPY_BYTES read from the kernel CSPRNG. Nothing
 * repository-derived and nothing model-derived reaches it, which is what lets
 * the result appear in automatic model context.
 *
 * The entropy is what makes it safe across databases. Without it the input is
 * (root hash, small integer, one-second timestamp), and two machines indexing
 * the same repository would mint the same identifier for two unrelated
 * decisions created in the same second — which is exactly the case durable,
 * exported identifiers have to survive.
 *
 * `attempt` is mixed in so a caller retrying after a UNIQUE collision gets a
 * different value from the same inputs even in the pathological case where the
 * entropy source repeats.
 *
 * Fails rather than falling back when the CSPRNG is unavailable: a uid built
 * from a predictable input is one that collides, and a silently weakened
 * identifier is not noticed until two records merge.
 *
 * It is an **identifier, not a secret**. Nothing treats knowing one as
 * authorisation. `out` needs ATLAS_DECISION_UID_MAX bytes. */
#define ATLAS_DECISION_UID_ENTROPY_BYTES 16u
atlas_status atlas_decision_uid_derive(const char *identity_hash, int64_t document_id,
                                       const char *created_at, unsigned attempt, char *out,
                                       size_t out_size, atlas_err *err);

/* --- the operator channel -------------------------------------------------
 *
 * What Atlas can and cannot say about an approval, in the smallest number of
 * moving parts that supports the claim.
 *
 * A challenge is issued by the daemon's writer thread, bound to one repository,
 * one document, one revision and one content hash, valid for
 * ATLAS_DECISION_CHALLENGE_TTL_MS, and consumable exactly once. Consumption and
 * the lifecycle transition happen in the same writer transaction, so a
 * challenge cannot be spent without a transition and a transition cannot happen
 * without spending one.
 *
 * **What this is not.** It does not establish that a person acted. The daemon cannot
 * see the client's terminal; it can only see a request from a process running
 * as the same uid. The terminal requirement lives in the CLI, and a same-uid
 * process can skip the CLI. What the mechanism actually buys is that an
 * approval cannot be produced by a model's text, by a hook payload, by an
 * environment variable, by a repository file, by an MCP tool, or by replaying a
 * captured request — which is a real and checkable set of properties, and is
 * all that is claimed. */
typedef enum atlas_decision_intent {
    ATLAS_DECISION_INTENT_APPROVE = 0,
    ATLAS_DECISION_INTENT_REJECT,
    ATLAS_DECISION_INTENT_SUPERSEDE
} atlas_decision_intent;

const char *atlas_decision_intent_name(atlas_decision_intent i);
bool atlas_decision_intent_parse(const char *name, atlas_decision_intent *out);

typedef struct atlas_decision_challenge {
    int64_t id;
    char token[ATLAS_DECISION_CHALLENGE_HEX + 1u];
    int64_t repo_id;
    int64_t document_id;
    int64_t revision_id;
    int64_t revision_no;
    char content_hash[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_decision_intent intent;
    int64_t supersede_document_id; /* SUPERSEDE only; 0 otherwise */
    char created_at[ATLAS_TS_MAX];
    char expires_at[ATLAS_TS_MAX];
    bool consumed;
} atlas_decision_challenge;

void atlas_decision_challenge_init(atlas_decision_challenge *c);

/* Fills `token` with ATLAS_DECISION_CHALLENGE_HEX lowercase hex characters read
 * from the kernel CSPRNG.
 *
 * Fails rather than falling back. A predictable challenge token would let a
 * process guess a capability instead of being given one, and a "random enough"
 * fallback is the kind of thing that is never noticed until it matters. */
atlas_status atlas_decision_challenge_token(char *out, size_t out_size, atlas_err *err);

/* The confirmation the operator must type: the first ATLAS_DECISION_CONFIRM_HEX
 * characters of the revision's content hash. Comparing against this is a
 * constant string comparison of Atlas-owned hex — the operator's input never
 * reaches a parser. */
void atlas_decision_confirm_phrase(const char *content_hash, char *out, size_t out_size);

#endif /* ATLAS_DECISION_H */
