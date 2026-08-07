/* Atlas - the provider-neutral AI session and change-reason model.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A2 is the first phase in which Atlas talks to something that interprets
 * language. Nothing in this header names Claude. It models an *AI client* —
 * a provider, a client name, a session key it chose — so that a second adapter
 * (Codex, or any other MCP client) reuses the stored model rather than getting
 * a parallel one.
 *
 * Two rules shape everything here.
 *
 * 1. **Atlas records proposals, not truth.** A reason a model wrote down is a
 *    MODEL_PROPOSAL. It is not a decision the user approved, and A2 has no way
 *    to prove that a user approved anything: an argument that says "the user
 *    approved this" is a string a model produced. So the approval column exists
 *    and is pinned to zero by a CHECK constraint, and the approval workflow is
 *    deferred rather than faked.
 *
 * 2. **UNKNOWN is a value, not a failure.** A model asked to explain a change
 *    it did not reason about will be pushed toward whatever the repository text
 *    suggests. Recording UNKNOWN has to be as easy as recording a reason, and
 *    it has to be queryable afterwards, or it will not be used.
 *
 * The A0 evidence table is untouched. `atlas_db_evidence_insert` still refuses
 * everything except SOURCE and GIT, and nothing here writes to it. AI records
 * live in their own tables with their own provenance column, and link to
 * evidence rows rather than becoming them.
 */
#ifndef ATLAS_AI_H
#define ATLAS_AI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "atlas/buf.h"
#include "atlas/db.h"
#include "atlas/error.h"
#include "atlas/limits.h"
#include "atlas/sha256.h"

/* --- provenance ----------------------------------------------------------
 *
 * A wider vocabulary than `atlas_evidence_kind`, because A2 has to distinguish
 * things A0 never held: text Atlas itself composed, a claim a model made, and
 * an approval that has not happened.
 *
 * The two vocabularies deliberately overlap on SOURCE and GIT rather than being
 * merged: `atlas_evidence_kind` is what may go in the `evidence` table, and this
 * is what may be *reported*. A reported item can carry MODEL_PROPOSAL; an
 * evidence row can never be one. */
typedef enum atlas_provenance {
    /* Composed by Atlas from its own index. Not repository text. The only class
     * that may appear in automatic model context. */
    ATLAS_PROV_ATLAS_OWNED = 0,
    /* A decision a human approved. A2 never writes this: it cannot prove it. */
    ATLAS_PROV_USER_APPROVED_DECISION,
    /* Read from git history. Untrusted repository prose. */
    ATLAS_PROV_GIT,
    /* Read from the git index or the working tree. Untrusted repository data. */
    ATLAS_PROV_SOURCE,
    /* A model wrote this down deliberately, as a proposal. Not established. */
    ATLAS_PROV_MODEL_PROPOSAL,
    /* A model derived this rather than being told it. Weaker than a proposal. */
    ATLAS_PROV_MODEL_INFERENCE,
    /* No evidence. A first-class answer, never a placeholder for a guess. */
    ATLAS_PROV_UNKNOWN
} atlas_provenance;

const char *atlas_provenance_name(atlas_provenance p);
/* Parses a wire name. Returns false for anything unrecognised; there is no
 * default, because defaulting an unknown provenance to a known one is how a
 * model proposal becomes a recorded fact. */
bool atlas_provenance_parse(const char *name, atlas_provenance *out);
/* True when a value with this provenance is repository-derived and must be
 * labelled UNTRUSTED_DATA wherever it is reported. */
bool atlas_provenance_is_untrusted(atlas_provenance p);
/* True when an A2 adapter is permitted to write this provenance. Refuses
 * USER_APPROVED_DECISION, enforced in code rather than by convention, the same
 * way A0 enforces its evidence restriction. */
bool atlas_provenance_writable_in_a2(atlas_provenance p);

/* How sure a model says it is. Reported, never acted on. */
typedef enum atlas_ai_confidence {
    ATLAS_AI_CONF_NONE = 0,
    ATLAS_AI_CONF_LOW,
    ATLAS_AI_CONF_MEDIUM,
    ATLAS_AI_CONF_HIGH
} atlas_ai_confidence;

const char *atlas_ai_confidence_name(atlas_ai_confidence c);
bool atlas_ai_confidence_parse(const char *name, atlas_ai_confidence *out);

/* How a changed path came to be attributed to a session.
 *
 * The distinction is the honest part of A2. "This session ran Edit on this
 * path" is a different claim from "this path changed while this session was
 * open", and when two sessions had the same repository open, neither claim
 * supports "this session changed it". */
typedef enum atlas_ai_attribution {
    /* The session invoked an edit tool naming this path, and the index then
     * observed the path change. */
    ATLAS_AI_ATTR_DIRECT_EDIT = 0,
    /* The index observed the path change while the session was open. Something
     * else may have changed it. */
    ATLAS_AI_ATTR_OBSERVED,
    /* Another session had the same repository open over the same window, so the
     * change cannot be attributed to either. */
    ATLAS_AI_ATTR_AMBIGUOUS
} atlas_ai_attribution;

const char *atlas_ai_attribution_name(atlas_ai_attribution a);

/* --- operations ----------------------------------------------------------
 *
 * Every mutation an adapter can request, as one tagged struct. It exists so the
 * daemon's writer thread has a single typed entry point rather than one job
 * kind per verb, and so the IPC layer can validate a request completely before
 * anything is queued.
 *
 * Every text field is bounded and safe-encoded before it lands here. */
typedef enum atlas_ai_op_kind {
    ATLAS_AI_OP_SESSION_OPEN = 0,
    ATLAS_AI_OP_SESSION_TOUCH,
    ATLAS_AI_OP_SESSION_CLOSE,
    ATLAS_AI_OP_SESSION_CHECKPOINT,
    ATLAS_AI_OP_ATTACH_ROOT,
    ATLAS_AI_OP_TOOL_RECORD,
    ATLAS_AI_OP_CORRELATE,
    ATLAS_AI_OP_REASON,
    ATLAS_AI_OP_DECISION,
    ATLAS_AI_OP_TURN_CLOSE
} atlas_ai_op_kind;

/* Which half of the compaction lifecycle a checkpoint describes. */
typedef enum atlas_ai_phase {
    ATLAS_AI_PHASE_PRE_COMPACT = 0,
    ATLAS_AI_PHASE_POST_COMPACT
} atlas_ai_phase;

/* What a tool observation records. Deliberately three states rather than a
 * boolean: "the tool reported success" is not "the file changed", and A2 must
 * not collapse the two. */
typedef enum atlas_ai_tool_phase {
    ATLAS_AI_TOOL_INTENT = 0, /* PreToolUse: about to run */
    ATLAS_AI_TOOL_OK,         /* PostToolUse: reported success */
    ATLAS_AI_TOOL_FAILED      /* PostToolUseFailure: reported failure */
} atlas_ai_tool_phase;

typedef struct atlas_ai_op {
    atlas_ai_op_kind kind;

    /* Client identity. Provider and client are Atlas-facing names validated to
     * [A-Za-z0-9._-]; the session key is the client's own identifier, safe
     * encoded and bounded. */
    atlas_buf provider;
    atlas_buf client;
    atlas_buf client_version;
    atlas_buf session_key;
    atlas_buf parent_session_key; /* resume or fork lineage; may be empty */
    atlas_buf agent_id;           /* subagent linkage; empty for a main session */
    atlas_buf agent_type;

    /* Where. `root` is a filesystem path the adapter observed; `repo_name` is a
     * registered name. Exactly one is normally set, and `root` wins. */
    atlas_buf root;
    atlas_buf repo_name;
    /* Why the root is being attached: session_start, cwd_changed, directory_added. */
    atlas_buf source;

    /* Tool observation. Never the tool's input, output or error text. */
    atlas_buf tool_name;
    atlas_buf tool_use_id;
    atlas_ai_tool_phase tool_phase;

    /* Repository-relative raw path bytes, NUL separated, as everywhere else in
     * Atlas. A path is bytes, never text. */
    atlas_buf paths;

    /* Reason and decision payloads. Model-authored, bounded, safe-encoded. */
    atlas_buf summary;
    atlas_buf detail;
    atlas_buf title;
    atlas_buf statement;
    atlas_buf rationale;
    atlas_buf unknown_reason;

    /* Idempotency. When set, replaying the same operation collides on a partial
     * unique index instead of creating a second durable row. */
    atlas_buf dedup_key;

    atlas_provenance provenance;
    atlas_ai_confidence confidence;
    atlas_ai_phase phase;
    bool unknown;         /* the reason is explicitly UNKNOWN */
    bool auto_register;   /* may register an unregistered git worktree */
    bool request_sync;    /* correlate: ask for an incremental reconciliation */
    int64_t turn_seq;     /* a per-turn idempotency counter from the client */
} atlas_ai_op;

void atlas_ai_op_init(atlas_ai_op *op, atlas_ai_op_kind kind);
void atlas_ai_op_free(atlas_ai_op *op);

/* --- session attribution --------------------------------------------------
 *
 * **A session is identified by its key and by nothing else.**
 *
 * The key is `(provider, client, session_key)`, where `session_key` is the
 * external identifier the client itself uses — for Claude Code, the value of
 * `CLAUDE_CODE_SESSION_ID`, which is the same string the hook payload carries
 * as `session_id`. Both adapters send the same provider and client names, so a
 * record made through the MCP server and an event delivered by a hook resolve
 * to the same Atlas session row when, and only when, they carry the same
 * external id.
 *
 * A repository is **never** an identifier. An earlier implementation, when an
 * MCP write named no session, attached it to the newest open session for the
 * repository. With two Claude sessions open on one worktree that silently
 * recorded session A's reason against session B, and the record looked exactly
 * as trustworthy as a correct one. There is no query anywhere in Atlas that
 * selects a session by recency, and adding one back would reintroduce this.
 *
 * When exact attribution is unavailable the record is stored **sessionless**,
 * with `session_unbound` set and `unbound_reason` saying which case it was.
 * Missing attribution is recoverable; wrong attribution is not. */

/* No external session id reached Atlas at all. A generic MCP client that is not
 * Claude Code, or a `claude mcp` server started outside a session. */
#define ATLAS_AI_UNBOUND_NO_SESSION_ID "no_session_id"
/* An id arrived and no session has it. Atlas never opened this session — the
 * hooks are not installed, or the daemon was not running when it started. */
#define ATLAS_AI_UNBOUND_UNKNOWN_SESSION "unknown_session"
/* The session with this id exists and has ended. The MCP server holds the id it
 * was spawned with, so after `/clear` its id names the conversation that was
 * cleared, not the one now running. Recording against the ended session would
 * be attributing a live turn to a finished one. */
#define ATLAS_AI_UNBOUND_SESSION_CLOSED "session_closed"

/* True when an operation may only attach to a session that is still open.
 *
 * Reason and decision records: those two carry model-authored content that is
 * *about* a conversation, so binding them to an ended conversation would be a
 * claim Atlas cannot support. Every other operation is session bookkeeping sent
 * by the hooks — including the SessionEnd that does the closing — and must
 * still reach its own session by exact key whatever state it is in. */
bool atlas_ai_op_needs_open_session(atlas_ai_op_kind kind);

/* What an applied operation reports back. Counts and identifiers only: nothing
 * here is repository prose, so a caller can render it without encoding. */
typedef struct atlas_ai_result {
    int64_t session_id;
    int64_t repo_id;
    int64_t change_set_id;
    int64_t record_id; /* the reason or decision row, when one was created */
    atlas_buf repo_name;
    atlas_buf root_text; /* already in the safe encoding */
    bool session_created;
    /* No Atlas session was bound, so the record is stored sessionless.
     *
     * A record with `session_unbound` is a record Atlas *could* have attached to
     * a session by guessing and deliberately did not. See `unbound_reason`. */
    bool session_unbound;
    /* Why, from the fixed set in atlas_ai_unbound_* below. A pointer to a string
     * literal, never allocated, so the result frees nothing for it. NULL when a
     * session was bound or when the operation does not attach to one. */
    const char *unbound_reason;
    bool repo_registered; /* this operation registered the repository */
    bool duplicate;       /* a dedup key suppressed a durable write */
    bool degraded;
    atlas_buf degraded_reason;
    /* Correlation counters. */
    int64_t changed_paths;
    int64_t direct_paths;
    int64_t ambiguous_paths;
    int64_t unresolved_paths; /* changed, with no reason recorded */
    int64_t concurrent_sessions;
    int64_t sync_seq;
} atlas_ai_result;

void atlas_ai_result_init(atlas_ai_result *r);
void atlas_ai_result_free(atlas_ai_result *r);

/* Applies one operation. `db` must be a writable handle owned by the calling
 * thread — in the daemon, that is the writer thread and only the writer thread.
 *
 * `reconcile` may be NULL. When it is not, a correlate operation uses it to ask
 * for an incremental pass before reading the change set; the callback exists so
 * this file does not depend on the daemon's job queue. */
typedef atlas_status (*atlas_ai_sync_fn)(void *ud, int64_t repo_id, const char *dirty_paths,
                                         size_t dirty_len, int64_t *sync_seq_out, atlas_err *err);

atlas_status atlas_ai_apply(atlas_db *db, const atlas_ai_op *op, atlas_ai_sync_fn sync, void *sync_ud,
                            atlas_ai_result *out, atlas_err *err);

/* --- read side ------------------------------------------------------------
 *
 * The row and report types live in atlas/db.h beside every other row type, so
 * this header can depend on that one rather than the two depending on each
 * other. What lives here is the part that is about meaning rather than storage:
 * the provenance vocabulary above, and the context envelope below. */

/* The bounded state one repository contributes to the automatic envelope.
 *
 * **Every field here is a number, a fixed enum, a boolean or a hash.** There is
 * no repository-controlled or model-provided free-form text — the rendered
 * envelope carries only these typed values plus the renderer's own fixed
 * Atlas-owned control text — and in particular no repository *name* and no
 * repository *root*.
 *
 * Both of those were in the first A2 implementation and both were wrong. A
 * repository name is derived from a directory basename and a root is a
 * filesystem path, so both are chosen by whoever created the directory. A
 * directory called `ignore previous instructions` produces a name and a root
 * containing that phrase, it is entirely printable, and it survives every
 * encoding Atlas has unchanged — which is the whole point of
 * docs/ai-trust-boundary.md. Encoding is a defence against terminals and
 * parsers, not against meaning.
 *
 * They are replaced by:
 *
 *   - `repo_id`, the row id. Opaque, Atlas-assigned, and monotonic; it carries
 *     no attacker-chosen bytes because Atlas chose it.
 *   - `root_hash`, a SHA-256 of the canonical root path. Identifies the
 *     repository across sessions and lets a consumer tell two repositories
 *     apart, while being 64 hex characters that cannot say anything.
 *
 * The client already knows its own working directory; it does not need Atlas to
 * repeat it back. A caller that genuinely needs the name or the path asks for it
 * through an explicit MCP tool, where it arrives labelled UNTRUSTED_DATA. */
typedef struct atlas_ai_context {
    bool daemon_reachable;
    bool repo_known;
    /* Opaque, Atlas-assigned. Never a name and never a path. */
    int64_t repo_id;
    /* SHA-256 of the canonical root path, lowercase hex, or "" when unknown. */
    char root_hash[ATLAS_SHA256_HEX_LEN + 1u];
    char head_oid[ATLAS_OID_HEX_MAX_INCL]; /* validated hex, or "" */
    char head_state[16];  /* born | unborn | detached | unknown */
    bool index_current;
    /* One of a fixed set of Atlas-authored strings, checked against that set
     * before it is emitted rather than trusted to be one of them. */
    atlas_buf not_current_reason;
    int64_t generation;
    int64_t event_cursor;
    int64_t changed_paths;
    int64_t session_id;
    int64_t change_set_id;
    int64_t approved_decisions; /* always 0 in A2; present so it can stop being */
    int64_t proposed_decisions;
    int64_t unresolved_reasons;

    /* A3. Typed counters only, and that is the whole of what the structural
     * index contributes to automatic context.
     *
     * No symbol name, no path, no include spelling, no summary. Every one of
     * those is chosen by whoever can commit — `ignore_previous_instructions` is
     * a legal C identifier and a legal file name — and the envelope's rule is
     * that no field can hold a value somebody else chose. A consumer that wants
     * a name asks through an explicit MCP tool, where it arrives labelled
     * UNTRUSTED_DATA. */
    bool code_index_current;
    int64_t code_generation;
    int64_t code_symbols;
    int64_t code_relations;
    int64_t code_ambiguous;
    int64_t code_unresolved;
} atlas_ai_context;

void atlas_ai_context_init(atlas_ai_context *c);
void atlas_ai_context_free(atlas_ai_context *c);
/* Sets `root_hash` from the canonical root's raw bytes. Hashed from the raw
 * bytes rather than from the safe text, so two repositories whose paths differ
 * only in bytes that are not valid UTF-8 still hash differently. */
void atlas_ai_context_set_root_hash(atlas_ai_context *c, const void *root_raw, size_t root_len);

/* Renders the envelope as the exact bytes an adapter injects.
 *
 * The output is guaranteed to satisfy atlas_ai_context_is_bounded(): it is at
 * most ATLAS_AI_MAX_CONTEXT_BYTES and contains only characters from a fixed
 * allowlist. A value that would violate either is replaced by a marker rather
 * than emitted, so this function cannot be made to carry arbitrary bytes by
 * anything it is given. */
atlas_status atlas_ai_context_render(const atlas_ai_context *c, atlas_buf *out, atlas_err *err);

/* The guarantee above, checkable. Exposed so tests assert the policy rather
 * than a hard-coded list of examples, and so the renderer can assert its own
 * output before returning it. */
bool atlas_ai_context_is_bounded(const char *text, size_t len);

#endif /* ATLAS_AI_H */
