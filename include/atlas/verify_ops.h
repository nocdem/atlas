/* Atlas - A9.2.1: verification intake, one typed operation, one write point.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A9.2 built an engine that can weigh evidence and a root-owned policy that can
 * act on the result. It shipped with no way for a repository, a model or a tool
 * to *put evidence in*: the three insert functions had no caller outside the
 * verification tests, so on a real deployment the ten verification tables stayed
 * empty while the engine that reads them passed every test it had. This header
 * is the missing half.
 *
 * It exists for the reason `atlas/decision_ops.h` does: the daemon's writer
 * thread needs one typed entry point rather than a job kind per verb, and the
 * IPC layer must be able to validate a request completely before anything is
 * queued. It is separate from `atlas/verify.h` because that header is about
 * meaning and depends on nothing; this one is about applying it and depends on
 * `atlas/db.h`.
 *
 * ## The one rule this header exists to make structural
 *
 * **Intake is not authority, and a submitter does not describe itself.**
 *
 * Everything an intake request carries is a statement by whoever sent it. The
 * three facts that decide how much an attestation is *worth* — the actor's
 * class, the actor's identity, and whether a piece of evidence was produced by
 * something Atlas ran — are therefore never read from the request. They are
 * derived from `atlas_verify_channel`, which the transport edge sets from what
 * the kernel or a credential established, and which no field in any request can
 * reach. A model may say it is called anything; it may not thereby become a
 * compiler.
 *
 * That is the same argument A7.1 makes about `SO_PEERCRED` — "a client
 * describing itself is not evidence about itself" — applied one layer up, and
 * it is why `atlas_verify_actor_class` and `atlas_verify_actor_identity` appear
 * nowhere in `atlas_verify_op`.
 *
 * ## What intake may and may not do
 *
 * A model reaching every operation in this header can create a claim, reference
 * evidence, attest, declare a derivation and ask for an evaluation. It cannot
 * approve, reject, supersede, resolve or revalidate anything; it cannot mint or
 * spend a warrant; it cannot name a verifier's verdict; and it cannot edit the
 * root-owned policy that decides whether a result may move a lifecycle state.
 * If the policy's gates are met, Atlas performs the transition itself through
 * `atlas_verify_autolifecycle_run` — the third and only machine caller of
 * `atlas_decision_apply_in_tx` — and the model's involvement ends at having
 * supplied evidence that turned out to be good enough.
 */
#ifndef ATLAS_VERIFY_OPS_H
#define ATLAS_VERIFY_OPS_H

#include <stdbool.h>
#include <stdint.h>

#include "atlas/buf.h"
#include "atlas/db.h"
#include "atlas/error.h"
#include "atlas/verify.h"
#include "atlas/verifypolicy.h"

/* --- who is on the other end of the intake call ---------------------------
 *
 * Set by the transport edge from what it actually established, never parsed
 * from a request body. This is the single input that decides an actor's class
 * and identity, and therefore the single thing a forger would need to control.
 *
 * **UNKNOWN is zero and is refused**, the house rule: a `memset` must not
 * produce a channel that can write, and an intake path that forgot to set the
 * channel must fail rather than default to the weakest caller that can still
 * speak. */
typedef enum atlas_verify_channel {
    /* Nothing established the caller. Refused at the write point. */
    ATLAS_VERIFY_CHANNEL_UNKNOWN = 0,
    /* An MCP tool or an ordinary RPC method: a model, or something speaking for
     * one. Produces an AI_AGENT actor with SELF_DECLARED identity, always —
     * every descriptive field it supplies about itself is asserted, and §11
     * requires Atlas to report it as asserted rather than pretend to certainty
     * the transport cannot deliver. */
    ATLAS_VERIFY_CHANNEL_MODEL,
    /* The local CLI, or an RPC peer whose uid the root-owned policy names as
     * the operator. Produces a HUMAN actor with PEER_AUTHENTICATED identity.
     *
     * PEER_AUTHENTICATED is a claim about the *uid*, not about a person: A7.1's
     * limits hold word for word, and a model with a shell as that account
     * reaches this channel exactly as a human does. What it is not is anonymous,
     * which is all this distinction asserts. */
    ATLAS_VERIFY_CHANNEL_OPERATOR,
    /* Atlas itself, having *performed* the act: run a deterministic verifier,
     * read a repository object, computed a hash. The only channel that may
     * produce ATLAS_ATTESTED identity, and therefore the only one that may
     * produce TOOL, TEST, RUNTIME_OBSERVATION or ATLAS_VERIFIER actors and the
     * evidence classes that go with them.
     *
     * It is unreachable from every transport: no request parser sets it, and
     * `atlas_verify_channel_parse` does not accept its name. It is set only by
     * Atlas' own code, in the same call in which Atlas did the thing. */
    ATLAS_VERIFY_CHANNEL_ATLAS
} atlas_verify_channel;

const char *atlas_verify_channel_name(atlas_verify_channel c);
/* Parses the two channels a *transport* may select between. Deliberately
 * refuses `ATLAS` and `UNKNOWN`: a request that could name its own channel
 * could name the one that makes its evidence authentic. */
bool atlas_verify_channel_parse(const char *name, atlas_verify_channel *out);

/* How much a channel asserts, as a total order: UNKNOWN 0 < MODEL 1 <
 * OPERATOR 2 < ATLAS 3.
 *
 * It exists so a transport can let a caller **weaken** its own channel and
 * never raise it, and that asymmetry is the whole point.
 *
 * The peer's uid is the only thing the kernel establishes, and on an
 * unseparated machine — or on a separated one where a person runs a model from
 * their own account, which A7.1 explicitly permits — an MCP session speaks from
 * the operator uid. Deriving the channel from that uid alone recorded a model's
 * attestations as a HUMAN actor with PEER_AUTHENTICATED identity, which is the
 * one thing §10 forbids: the transport *knows* an MCP tool call is a model
 * speaking, and that knowledge is more specific than the uid, not less.
 *
 * So the MCP adapter says so, and the write point believes it only downwards.
 * Claiming less authority than you hold is never a forgery; it is the accurate
 * statement. Claiming more is refused by comparing ranks, and `..._parse`
 * additionally refuses ATLAS and UNKNOWN by name — so no request can reach the
 * channel that makes evidence authentic, whatever it sends and whatever uid it
 * sends it from. */
int atlas_verify_channel_authority(atlas_verify_channel c);

/* The actor class and identity this channel produces. Functions rather than a
 * table each caller keeps, so the mapping has one definition and a test can ask
 * it the same question the write point asks. */
atlas_verify_actor_class atlas_verify_channel_actor_class(atlas_verify_channel c);
atlas_verify_actor_identity atlas_verify_channel_actor_identity(atlas_verify_channel c);

/* --- what may be asserted versus what must be performed --------------------
 *
 * True for the evidence classes whose entire evidentiary weight comes from
 * Atlas having *done* the thing: COMPILER, TEST, RUNTIME and DEPLOYED_CONFIG.
 * A submitter on any channel but ATLAS naming one of these is **refused**.
 *
 * Refused rather than stored-and-discounted, which is `atlas_verify_actor_class_
 * requires_atlas_identity`'s argument and it applies unchanged here: a
 * discounted forgery still appears in the evidence list, still reads as tool
 * output to somebody skimming a UI, and still has to be argued away by whoever
 * finds it. The honest path for a model that believes the compiler proves
 * something is AI_ANALYSIS evidence declaring what it read, plus a request that
 * Atlas run the verifier itself. */
bool atlas_verify_evidence_class_requires_atlas_production(atlas_verify_evidence_class c);

/* --- the operations -------------------------------------------------------- */

typedef enum atlas_verify_op_kind {
    /* Create a claim: one proposition, bound to a repository state. */
    ATLAS_VERIFY_OP_CLAIM_CREATE = 0,
    /* Reference a piece of evidence. Atlas validates the reference against the
     * repository and computes the content identity itself rather than trusting
     * a supplied one — §8. */
    ATLAS_VERIFY_OP_EVIDENCE_ADD,
    /* Ask Atlas to *produce* evidence by running a named allowlisted
     * deterministic verifier. The only way ATLAS_ATTESTED evidence comes into
     * existence, and the legitimate answer to "I want compiler evidence".
     *
     * The caller names the verifier; it does not name the verdict. What the
     * verifier concluded is whatever it concluded. */
    ATLAS_VERIFY_OP_EVIDENCE_PRODUCE,
    /* One actor's verdict on one claim at one moment. */
    ATLAS_VERIFY_OP_ATTESTATION_ADD,
    /* Declare that one piece of evidence derives from another, so the
     * union-find can stop counting a shared root more than once. */
    ATLAS_VERIFY_OP_DEPENDENCY_ADD,
    /* Aggregate everything stored for a claim, record a durable result, and
     * hand it to the root-owned policy engine. May cause Atlas to transition a
     * lifecycle state on its own authority; never lets the caller do so. */
    ATLAS_VERIFY_OP_EVALUATE
} atlas_verify_op_kind;

const char *atlas_verify_op_kind_name(atlas_verify_op_kind k);
/* True when the operation writes a durable verification result and may
 * therefore reach the policy engine. Asked by the write point itself, so a new
 * op kind cannot quietly become an evaluation. */
bool atlas_verify_op_is_evaluation(atlas_verify_op_kind k);

typedef struct atlas_verify_op {
    atlas_verify_op_kind kind;

    /* Established by the transport, never parsed from the request body. */
    atlas_verify_channel channel;

    /* Where. Exactly one is normally set; `root` wins, as in A2 and A4. */
    atlas_buf repo_name;
    atlas_buf root;

    /* --- the speaker's own description of itself -------------------------
     *
     * Every field here is UNTRUSTED_DATA and is stored as asserted metadata.
     * §11: Atlas distinguishes AI actors without hard-coding a vendor, and
     * where the transport cannot establish a model's identity cryptographically
     * — which is everywhere, today — the metadata is marked asserted rather
     * than authenticated. `identity` on the stored row says which. */
    atlas_buf actor_name;
    atlas_buf actor_provider;
    atlas_buf actor_family;
    atlas_buf actor_version;
    atlas_buf actor_role;
    atlas_buf session_key;
    atlas_buf run_id;
    atlas_buf parent_actor_uid;

    /* --- CLAIM_CREATE ---------------------------------------------------- */
    atlas_buf document_uid; /* the knowledge record, optional */
    atlas_buf domain;
    atlas_buf text;
    atlas_buf scope_note;
    atlas_verify_claim_semantics semantics;
    bool semantics_given;
    atlas_buf verifier;       /* which allowlisted verifier applies, if any */
    atlas_buf verifier_input; /* its bounded structured argument */
    atlas_buf basis_commit;   /* what repository state the claim is *of* */
    atlas_buf environment;    /* deployment identity for a runtime claim */

    /* --- EVIDENCE_ADD / EVIDENCE_PRODUCE --------------------------------- */
    atlas_buf claim_uid;
    atlas_verify_evidence_class evidence_class;
    atlas_buf commit_oid;
    atlas_buf path_text;
    atlas_buf symbol;
    int64_t line_start;
    int64_t line_end;
    atlas_buf target;
    atlas_buf probe;
    atlas_buf observed;
    atlas_buf observed_at;

    /* --- ATTESTATION_ADD -------------------------------------------------- */
    atlas_verify_verdict verdict;
    int self_confidence; /* 0..100, or -1 for none */
    atlas_buf method;
    atlas_buf supersedes_uid;
    /* Evidence this attestation rests on, by uid. Declaring them is what lets
     * the union-find see that three actors read one document — §12. */
    atlas_buf evidence_uids;

    /* --- DEPENDENCY_ADD --------------------------------------------------- */
    atlas_buf derived_uid; /* the evidence that derives */
    atlas_buf source_uid;  /* what it derives from */
} atlas_verify_op;

void atlas_verify_op_init(atlas_verify_op *op);
void atlas_verify_op_free(atlas_verify_op *op);

/* What an intake operation produced. */
typedef struct atlas_verify_intake_result {
    int64_t claim_id;
    int64_t evidence_id;
    int64_t attestation_id;
    int64_t actor_id;
    int64_t repo_id;

    atlas_buf uid;       /* the object created or resolved to */
    atlas_buf actor_uid;
    atlas_buf repo_name;

    /* True when a deterministic content key matched a row that already existed
     * and this call resolved to it rather than writing a second one. §27: a
     * retry must not become a corroboration. */
    bool duplicate;

    /* EVIDENCE_PRODUCE: what the verifier actually concluded, and Atlas' own
     * sentence about what it established. Never repository bytes. */
    atlas_verify_check check;
    char verified_scope[512];
    char detail[512];

    /* A9.2.2. What the verifier was able to observe, per dimension, and what
     * that makes of the proposition on the truth axis.
     *
     * `verify produce` reports these for the same reason `verify show` does: a
     * caller told only that a check came back UNAVAILABLE knows that Atlas
     * declined and not why, and "the semantic generation is partial" and "the
     * symbol's address is taken" call for entirely different responses. Both
     * come from closed Atlas-owned vocabularies. */
    atlas_verify_truth truth;
    atlas_verify_truth_reason truth_reason;
    atlas_verify_coverage_report coverage;

    /* EVALUATE: the whole assessment, including whether Atlas transitioned. */
    atlas_verify_assessment assessment;
} atlas_verify_intake_result;

void atlas_verify_intake_result_init(atlas_verify_intake_result *r);
void atlas_verify_intake_result_free(atlas_verify_intake_result *r);

/* **The only function in Atlas that writes a verification intake row.**
 *
 * The channel check, the actor derivation, the forged-producer refusal, the
 * reference validation, the content-key idempotency and the source binding all
 * live behind it, and every one would be bypassable if a second path reached
 * the tables. That is the rule `settle()`, `atlas_db_evidence_insert`,
 * `atlas_decision_apply_in_tx` and `atlas_orch_apply_in_tx` follow.
 *
 * It owns no transaction. `atlas_verify_intake_apply` is begin + this + commit;
 * a caller that already owns a wider unit of work calls this directly. */
atlas_status atlas_verify_intake_apply_in_tx(atlas_db *db, const atlas_verify_op *op,
                                             atlas_verify_intake_result *out, atlas_err *err);

/* The public entry point: a transaction around the write point and nothing
 * else. */
atlas_status atlas_verify_intake_apply(atlas_db *db, const atlas_verify_op *op,
                                       atlas_verify_intake_result *out, atlas_err *err);

#endif /* ATLAS_VERIFY_OPS_H */
