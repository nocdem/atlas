/* Atlas - A8: the durable orchestration control plane.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * ## What this phase is, and what it deliberately is not
 *
 * A8 accepts a *bounded job*, dispatches it to an unprivileged worker, isolates
 * the workspace it runs in, survives a crash on either side, and keeps a
 * complete auditable history of what happened. That is the whole claim.
 *
 * It is **not** authority. A completed job approves nothing, applies nothing and
 * commits nothing. The patch a job produces is an artifact — bytes in a
 * worker-owned directory with a recorded digest — and there is no code path in
 * Atlas that applies it to a registered repository. A7's lifecycle authority is
 * untouched: no orchestration method mints or spends a capability, and the
 * dispatcher cannot reach one. Confusing "a model finished a job" with "a
 * decision was approved" is the single mistake this phase must never make, so
 * the two vocabularies do not meet anywhere in the code.
 *
 * ## The two principals
 *
 * `atlasd` owns every orchestration row. It validates a specification, persists
 * a job before anything is dispatched, grants leases, and is the only writer of
 * state. `atlas-worker` runs the dispatcher, provisions its own workspaces and
 * executes drivers; it holds no database handle, cannot open the index, and
 * reaches Atlas only over the A7.1 socket.
 *
 * The split is not a convention. `atlas-worker` cannot read `/var/lib/atlas`
 * (0700 `atlasd`), so the "worker never writes orchestration state" rule is
 * enforced by the kernel and Atlas' checks are the second layer. That ordering
 * is A7.1's and A8 keeps it.
 *
 * ## Fail-closed at zero
 *
 * Every enum here keeps its unknown or refusing value at zero, for the reason A6
 * keeps UNKNOWN and BLOCKED there: a zeroed struct is one nobody filled in, and
 * the safe reading of that is never "queued", "leased" or "permitted". A
 * `memset` must not be able to produce a runnable job.
 */
#ifndef ATLAS_ORCH_H
#define ATLAS_ORCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "atlas/buf.h"
#include "atlas/error.h"

/* --- versions and domains ------------------------------------------------ */

/* The job-specification schema version. Bumped when the canonical encoding
 * changes shape, which changes every digest — see ATLAS_ORCH_SPEC_DOMAIN. */
#define ATLAS_ORCH_SPEC_VERSION 1

/* Domain separation for the canonical job-specification digest, following A4's
 * rule exactly: domain-separated and length-prefixed, never delimited. With any
 * single-byte delimiter a task of "a|b" with a mode of "c" would encode
 * identically to a task of "a" with a mode of "b|c".
 *
 * Bump this whenever the encoding changes. Every stored `spec_digest` means
 * something different afterwards, which is the point of it being in the string. */
#define ATLAS_ORCH_SPEC_DOMAIN "atlas.orch.spec.v1"

/* The lease token is a bearer secret. Only its digest is stored, and this domain
 * separates that digest from every other SHA-256 in Atlas so a value from one
 * context can never be presented in another. */
#define ATLAS_ORCH_LEASE_DOMAIN "atlas.orch.lease.v1"

/* --- job states ----------------------------------------------------------
 *
 * One explicit persisted state machine. `atlas_orch_transition_allowed` is the
 * single authority on what may follow what, and it is a *function* rather than a
 * table duplicated in a test, so a test cannot pass by agreeing with a second
 * copy of the rules. This is A4's discipline for the decision lifecycle applied
 * to jobs.
 *
 * Terminal states never return to an active state. That is checked centrally
 * rather than at each call site.
 */
typedef enum atlas_orch_state {
    /* Zero is not a state a job can be in. A row that reads UNKNOWN is a row
     * nobody wrote correctly, and every transition out of it is refused. */
    ATLAS_ORCH_STATE_UNKNOWN = 0,
    /* Persisted, validated, and waiting for a dispatcher to ask for work. */
    ATLAS_ORCH_STATE_QUEUED,
    /* A lease has been granted to exactly one attempt. */
    ATLAS_ORCH_STATE_LEASED,
    /* The worker is provisioning the workspace and the source snapshot. */
    ATLAS_ORCH_STATE_PREPARING,
    /* The driver is running. */
    ATLAS_ORCH_STATE_RUNNING,
    /* The declared validation commands are running. */
    ATLAS_ORCH_STATE_VALIDATING,
    /* Terminal: the attempt completed and validation passed. */
    ATLAS_ORCH_STATE_SUCCEEDED,
    /* Terminal: the attempt failed and no attempts remain. */
    ATLAS_ORCH_STATE_FAILED,
    /* An operator asked for cancellation while an attempt was active. The
     * attempt is told at its next heartbeat; the job is not yet cancelled. */
    ATLAS_ORCH_STATE_CANCEL_REQUESTED,
    /* Terminal. */
    ATLAS_ORCH_STATE_CANCELLED,
    /* Terminal: a wall-clock or idle bound was exceeded. */
    ATLAS_ORCH_STATE_TIMED_OUT,
    /* Terminal, and the honest answer when Atlas cannot tell what happened: a
     * lease expired with an attempt that may or may not have run, and retrying
     * could execute work twice. Recorded rather than guessed. */
    ATLAS_ORCH_STATE_RECOVERY_REQUIRED
} atlas_orch_state;

const char *atlas_orch_state_name(atlas_orch_state s);
bool atlas_orch_state_parse(const char *name, atlas_orch_state *out);
/* True for SUCCEEDED, FAILED, CANCELLED, TIMED_OUT and RECOVERY_REQUIRED. */
bool atlas_orch_state_is_terminal(atlas_orch_state s);
/* True for the states in which a lease may exist and a worker may report. */
bool atlas_orch_state_is_active(atlas_orch_state s);

/* The single authority on permitted transitions. Every write consults this and
 * an invalid transition fails closed. */
bool atlas_orch_transition_allowed(atlas_orch_state from, atlas_orch_state to);

/* --- the run -------------------------------------------------------------
 *
 * A11.0. A **run** is the durable grouping one chain of tasks belongs to: a
 * root task, and whatever follows it, under one identity that survives a
 * restart. A8 shipped `parent_job_uid` and never resolved it; the run is what
 * makes a parent chain a fact about stored rows rather than a well-formed
 * string.
 *
 * The run's status is **its own axis** and is derived from nothing. A task
 * ending SUCCEEDED does not accept its run and a task ending FAILED does not
 * block one, because "this attempt finished" and "this line of work is settled"
 * are different claims and Atlas does not let one stand in for the other. That
 * is the same separation A9.2 keeps between a verification state and a
 * lifecycle status, one layer out.
 *
 * A11.0 writes no automatic transition into a terminal status at all. Nothing
 * in this milestone decides that a run is ACCEPTED or BLOCKED; the states exist
 * so that a caller which has decided can record it, and so that the submit path
 * has something to refuse a child against. Who may decide is A11.1's question,
 * and leaving it unanswered here is deliberate rather than unfinished.
 */
typedef enum atlas_orch_run_status {
    /* Zero, and not a status a persisted run can hold. A row that reads UNKNOWN
     * is a row nobody wrote correctly; the schema's CHECK omits it. */
    ATLAS_ORCH_RUN_UNKNOWN = 0,
    /* The run is open: a task may be created in it, subject to there being no
     * other active one. */
    ATLAS_ORCH_RUN_ACTIVE,
    /* Terminal. The work this run existed for was accepted. */
    ATLAS_ORCH_RUN_ACCEPTED,
    /* Terminal. The run cannot proceed. Not a synonym for a failed task: a task
     * may fail and its run stay ACTIVE so a retry can follow. */
    ATLAS_ORCH_RUN_BLOCKED
} atlas_orch_run_status;

const char *atlas_orch_run_status_name(atlas_orch_run_status s);
bool atlas_orch_run_status_parse(const char *name, atlas_orch_run_status *out);
/* True for ACCEPTED and BLOCKED. UNKNOWN is not terminal — it is unwritten, and
 * treating "nobody filled this in" as a settled answer is the one reading that
 * would let a malformed row close a run. */
bool atlas_orch_run_status_is_terminal(atlas_orch_run_status s);

/* A run identifier: "r" plus 32 lowercase hex. The prefix differs from a job's
 * "j" so the two can never be confused on sight or by a parser. */
#define ATLAS_ORCH_RUN_UID_HEX 32u
#define ATLAS_ORCH_RUN_UID_MAX 40u

/* Generates a fresh unguessable run identifier. */
atlas_status atlas_orch_new_run_uid(atlas_buf *out, atlas_err *err);

/* --- why a transition happened -------------------------------------------
 *
 * A closed vocabulary, because a free-form reason on an audit row is a place for
 * worker-chosen text to end up looking like an Atlas statement. What the worker
 * says goes in a structured event, labelled as the worker's. */
typedef enum atlas_orch_reason {
    ATLAS_ORCH_REASON_UNKNOWN = 0,
    ATLAS_ORCH_REASON_SUBMITTED,
    ATLAS_ORCH_REASON_LEASE_GRANTED,
    ATLAS_ORCH_REASON_WORKER_PROGRESS,
    ATLAS_ORCH_REASON_WORKER_SUCCESS,
    ATLAS_ORCH_REASON_WORKER_FAILURE,
    ATLAS_ORCH_REASON_VALIDATION_FAILED,
    ATLAS_ORCH_REASON_LEASE_EXPIRED,
    ATLAS_ORCH_REASON_WALL_TIMEOUT,
    ATLAS_ORCH_REASON_IDLE_TIMEOUT,
    ATLAS_ORCH_REASON_CANCEL_REQUESTED,
    ATLAS_ORCH_REASON_CANCEL_CONFIRMED,
    ATLAS_ORCH_REASON_RETRY,
    ATLAS_ORCH_REASON_ATTEMPTS_EXHAUSTED,
    ATLAS_ORCH_REASON_RECOVERY_AMBIGUOUS,
    ATLAS_ORCH_REASON_POLICY_REFUSED,
    ATLAS_ORCH_REASON_ENVELOPE_INVALID
} atlas_orch_reason;

const char *atlas_orch_reason_name(atlas_orch_reason r);

/* --- who caused it -------------------------------------------------------
 *
 * Never taken from a request body. The submitter comes from `SO_PEERCRED` and
 * the dispatcher is identified by the lease it holds. */
typedef enum atlas_orch_actor {
    ATLAS_ORCH_ACTOR_UNKNOWN = 0,
    /* A client connection, identified by its kernel-supplied uid. */
    ATLAS_ORCH_ACTOR_CLIENT,
    /* The dispatcher, holding a valid unexpired lease for the attempt. */
    ATLAS_ORCH_ACTOR_DISPATCHER,
    /* The daemon itself: lease expiry, recovery scanning, timeout enforcement.
     * Nothing outside Atlas can present this actor. */
    ATLAS_ORCH_ACTOR_ATLAS
} atlas_orch_actor;

const char *atlas_orch_actor_name(atlas_orch_actor a);

/* --- what a driver reported ----------------------------------------------- */

typedef enum atlas_orch_exit_kind {
    ATLAS_ORCH_EXIT_UNKNOWN = 0,
    ATLAS_ORCH_EXIT_OK,
    ATLAS_ORCH_EXIT_NONZERO,
    ATLAS_ORCH_EXIT_SIGNALLED,
    ATLAS_ORCH_EXIT_TIMEOUT,
    ATLAS_ORCH_EXIT_CANCELLED,
    ATLAS_ORCH_EXIT_SPAWN_FAILED,
    /* The process exited zero and what it produced was not a result document.
     * Kept distinct from NONZERO on purpose: "Claude exits zero but produces
     * malformed result metadata" is a real failure mode and reading it as
     * success is exactly the mistake. */
    ATLAS_ORCH_EXIT_MALFORMED_RESULT
} atlas_orch_exit_kind;

const char *atlas_orch_exit_kind_name(atlas_orch_exit_kind k);

/* --- bounds --------------------------------------------------------------
 *
 * Every one of these is a refusal point, never a trimming point. A specification
 * that exceeds a bound is rejected with the bound named; nothing is silently
 * clamped, because a discarded number nobody is told about is a job that runs
 * differently from the one that was submitted. That is A5's rule about
 * `--older-than` applied here.
 */

/* An Atlas-generated external job identifier: "j" plus 32 lowercase hex. */
#define ATLAS_ORCH_UID_HEX 32u
#define ATLAS_ORCH_UID_MAX 40u

/* The lease token handed to a dispatcher once, at grant. 32 random bytes. */
#define ATLAS_ORCH_TOKEN_BYTES 32u
#define ATLAS_ORCH_TOKEN_HEX 64u
#define ATLAS_ORCH_TOKEN_MAX 80u

/* Task text. Bounded because it is copied, hashed, stored and rendered, and
 * because an unbounded field on an IPC boundary is a memory bound set by the
 * caller. It is UNTRUSTED_DATA at every one of those points. */
#define ATLAS_ORCH_TASK_MAX 65536u

/* Declared allowed paths: repository-relative prefixes the job says it intends
 * to touch. A set, canonicalised and sorted before hashing. */
#define ATLAS_ORCH_MAX_ALLOWED_PATHS 64u
#define ATLAS_ORCH_PATH_MAX 512u

/* Validation commands, each an explicit argv vector. Never a shell string:
 * there is no `sh -c` anywhere in A8, and a "command" field that accepted one
 * would be the whole boundary undone. */
#define ATLAS_ORCH_MAX_VALIDATIONS 8u
#define ATLAS_ORCH_MAX_ARGV 32u
#define ATLAS_ORCH_ARG_MAX 4096u

/* Names from closed-ish vocabularies the policy supplies. */
#define ATLAS_ORCH_NAME_MAX 64u

/* Absolute ceilings. The orchestration policy may lower these and may never
 * raise them: a root-owned file decides how much a submitter may ask for, and
 * this decides how much the policy itself may permit. */
#define ATLAS_ORCH_MAX_WALL_TIMEOUT_MS 3600000  /* one hour */
#define ATLAS_ORCH_MAX_IDLE_TIMEOUT_MS 900000   /* fifteen minutes */
#define ATLAS_ORCH_MAX_ATTEMPTS 5
#define ATLAS_ORCH_MAX_OUTPUT_BYTES (16u * 1024u * 1024u)
#define ATLAS_ORCH_MAX_ARTIFACT_BYTES (64u * 1024u * 1024u)
#define ATLAS_ORCH_MAX_ARTIFACT_COUNT 256
#define ATLAS_ORCH_MAX_WORKSPACE_BYTES (2048ll * 1024ll * 1024ll)

/* Lease lifetime and renewal. A lease is short and renewable rather than long:
 * a long lease is a long window in which a dead worker's job is not retried, and
 * an unrenewable one makes a slow but healthy job indistinguishable from a dead
 * one. Renewals are bounded so a wedged worker cannot hold a job forever by
 * heartbeating — the wall-clock bound is separate and is what finally stops it. */
#define ATLAS_ORCH_LEASE_MS 60000

/* How often the daemon sweeps for expired leases and past-deadline jobs.
 *
 * Comfortably under the lease itself, so an abandoned attempt is reclaimed
 * within roughly one lease rather than after a multiple of one. This is the
 * timer `op_recover` documents itself as being driven by; without a caller,
 * every recovery path A8 describes is exercised only by its tests, and a live
 * job whose worker died sits in its transient state for ever. That is exactly
 * what happened, and a job stuck in PREPARING with an expired, unreleased lease
 * is what found it. */
#define ATLAS_ORCH_RECOVER_INTERVAL_MS 20000
#define ATLAS_ORCH_LEASE_MAX_RENEWALS 240

/* A11.5a-R. How long after Atlas last refused a write it keeps declining to
 * judge an expired lease.
 *
 * Half a lease. The run driver heartbeats every `ATLAS_ORCH_LEASE_MS / 4` and
 * retries a refusal for a few seconds on top, so one grace window comfortably
 * contains a full heartbeat cycle: if there is a gap between passes big enough
 * for the sweep to run in, it is big enough for the heartbeat that follows it.
 *
 * It is deliberately not derived from how long the contention lasted. A grace
 * that grew with the outage would eventually stop reclaiming anything, and the
 * point is to forgive a worker for Atlas' silence, not to stop asking whether it
 * is alive. */
#define ATLAS_ORCH_CONTENTION_GRACE_MS (ATLAS_ORCH_LEASE_MS / 2)

/* A11.5a-R2. When Atlas last refused an orchestration write because the writer
 * was busy with something unbounded, on the same wall clock a lease expiry is
 * stored on, or zero if it has not refused one since this daemon started.
 *
 * Daemon-wide because the refusals arrive on two threads — the serve loop
 * answering clients and the watcher running its sweep — and either is equally
 * good evidence that a heartbeat would have been refused too. Written only by
 * the code that *receives* a refusal from the writer, so a client cannot
 * manufacture one: `contended_until_ms` on an operation is stamped from here by
 * the daemon and is never read from an IPC parameter. A9.2.6's contract is
 * untouched — nothing here changes what the writer does or when it refuses.
 *
 * Resets to zero on restart, which is the safe direction: no grace, leases
 * reclaimable exactly as before. */
void atlas_orch_contention_note(int64_t at_ms);
int64_t atlas_orch_contention_seen(void);

/* Whether an expired lease should be left alone this time round, because Atlas
 * was refusing writes recently enough that its holder may not have been able to
 * renew it.
 *
 * The one implementation of the rule. `op_recover` asks it before reclaiming an
 * attempt and `require_lease` asks it before refusing a heartbeat or a
 * completion, and those two answers have to agree: a sweep that spares a worker
 * while the write path still rejects it spares nothing. `deadline_ms` is checked
 * by both callers first and is never extended here — contention forgives Atlas'
 * own silence, not the submitter's bound. */
bool atlas_orch_lease_in_grace(int64_t deadline_ms, int64_t at_ms, int64_t contended_until_ms);

/* Structured worker events per attempt, and bytes per event. Both refuse rather
 * than trim: an event stream that silently stops recording looks like a job that
 * went quiet. */
#define ATLAS_ORCH_MAX_EVENTS 2000
#define ATLAS_ORCH_EVENT_MAX 8192u

/* Pagination. A caller is always told whether more exist. */
#define ATLAS_ORCH_LIST_MAX 200

/* Artifact bytes one RPC will return inline. Larger artifacts are described,
 * never streamed through the control socket. */
#define ATLAS_ORCH_ARTIFACT_INLINE_MAX (256u * 1024u)

/* --- A11.1: the bound on one run -------------------------------------------
 *
 * How many times a worker may actually be *started* inside one run: the root
 * task's own run, and at most two follow-ups. One constant, compiled in, with
 * no policy key and no flag — the season asked for a single explicit contract
 * rather than a retry framework, and a bound a caller can raise is not a bound.
 *
 * It counts **worker starts**, not tasks, not attempts and not submissions. The
 * count is derived from the ledger — every transition to RUNNING within the run
 * — because that is the one event that happens immediately before an exec and
 * is durable before it. A crash therefore spends budget, and a `BUSY` that
 * never reached a lease does not: a caller that was refused before anything ran
 * has consumed nothing, which is exactly what makes retrying it safe.
 *
 * `ATLAS_ORCH_MAX_ATTEMPTS` is a different bound with a different subject: how
 * many attempts one *task* may make. Both apply, and the tighter one wins,
 * which is why this is never compared against it. */
#define ATLAS_ORCH_RUN_MAX_WORKER_STARTS 3

/* How much of a failing gate's real output travels into the follow-up task's
 * text. Bounded because it is UNTRUSTED_DATA a compiler or a test runner
 * produced, it is stored, hashed and rendered, and an unbounded excerpt would
 * be a memory bound set by whatever the gate happened to print. Truncation here
 * is reported in the text itself rather than silent — the excerpt says it is an
 * excerpt. */
#define ATLAS_ORCH_GATE_EXCERPT_MAX 4096u

/* --- the canonical job specification --------------------------------------
 *
 * Everything immutable that changes what was asked for. The digest covers all of
 * it and nothing database-local: no row id, no creation timestamp, no state, no
 * attempt count. Two submissions that encode identically are the same request,
 * which is what makes the idempotency key meaningful rather than decorative.
 *
 * `repo_identity_hash` rather than a path, for A4's reason: a path is chosen by
 * whoever made the directory, and a durable identity is not.
 */
typedef struct atlas_orch_argv {
    atlas_buf args[ATLAS_ORCH_MAX_ARGV];
    size_t count;
} atlas_orch_argv;

typedef struct atlas_orch_spec {
    int spec_version;

    /* Trusted connection facts. Never read from the request body. */
    long long submitter_uid;

    /* Resolved by the daemon from trusted state, never from the request. */
    atlas_buf repo_name;
    atlas_buf repo_identity_hash;
    atlas_buf source_commit; /* exact, resolved and pinned; never a branch */

    atlas_buf mode;   /* from the policy's vocabulary */
    atlas_buf driver; /* from the policy's vocabulary */

    /* UNTRUSTED_DATA. Stored, hashed, labelled at every boundary, and never
     * placed in automatic model context. */
    atlas_buf task_text;

    atlas_buf allowed_paths[ATLAS_ORCH_MAX_ALLOWED_PATHS];
    size_t allowed_path_count;

    atlas_orch_argv validations[ATLAS_ORCH_MAX_VALIDATIONS];
    size_t validation_count;

    int64_t wall_timeout_ms;
    int64_t idle_timeout_ms;
    int64_t max_attempts;
    int64_t max_output_bytes;
    int64_t max_artifact_bytes;
    int64_t max_artifact_count;

    atlas_buf correlation;     /* optional, bounded, printable */
    atlas_buf parent_job_uid;  /* optional */
    atlas_buf idempotency_key; /* optional; unique per submitter when present */
} atlas_orch_spec;

void atlas_orch_spec_init(atlas_orch_spec *s);
void atlas_orch_spec_free(atlas_orch_spec *s);

void atlas_orch_argv_init(atlas_orch_argv *a);
void atlas_orch_argv_free(atlas_orch_argv *a);
/* Appends one argument. Refuses NUL bytes, over-long arguments and a vector
 * longer than ATLAS_ORCH_MAX_ARGV. */
atlas_status atlas_orch_argv_push(atlas_orch_argv *a, const char *arg, size_t len,
                                  atlas_err *err);

/* Canonicalises in place: sorts and deduplicates the allowed-path set, and
 * rejects anything that is not a safe repository-relative prefix. Called before
 * the digest so that the same set submitted in a different order is the same
 * specification. Validation commands keep their order — they are a list, and a
 * list's order is part of what was asked for. */
atlas_status atlas_orch_spec_canonicalise(atlas_orch_spec *s, atlas_err *err);

/* The canonical digest: domain-separated, length-prefixed, over every field
 * above in a fixed order. `out` receives 64 lowercase hex characters plus NUL.
 *
 * Adding a field means adding a row to the table in `docs/orchestration.md`
 * with a reason, and bumping ATLAS_ORCH_SPEC_DOMAIN. */
atlas_status atlas_orch_spec_digest(const atlas_orch_spec *s, char out[65], atlas_err *err);

/* Shape checks that do not need the policy: bounds, printability, absence of
 * NUL, no traversal in a declared path, an exactly-40-hex source commit. The
 * policy applies its own limits on top, and both must pass. */
atlas_status atlas_orch_spec_validate(const atlas_orch_spec *s, atlas_err *err);

/* --- canonical serialisation of the two list-shaped fields ----------------
 *
 * Netstring-style: each element is its decimal byte length, a colon, the bytes,
 * and a comma. Length-prefixed for the same reason the digest is, and readable
 * enough that a stored row can be understood without a decoder. Used both for
 * storage and, indirectly, for hashing.
 */
atlas_status atlas_orch_paths_encode(const atlas_buf *paths, size_t count, atlas_buf *out,
                                     atlas_err *err);
atlas_status atlas_orch_paths_decode(const char *text, atlas_buf *paths, size_t cap,
                                     size_t *count_out, atlas_err *err);
atlas_status atlas_orch_validations_encode(const atlas_orch_argv *v, size_t count, atlas_buf *out,
                                           atlas_err *err);
atlas_status atlas_orch_validations_decode(const char *text, atlas_orch_argv *v, size_t cap,
                                           size_t *count_out, atlas_err *err);

/* One gate as it travels from a client to the daemon: the canonical encoding of
 * a *single* command, `1:<argc>:<len>:<arg>,...`, byte for byte what the sender
 * wrote and what the database will store.
 *
 * This exists so the wire form has one reader. The daemon used to inline the
 * decode and wrap the element in a second count first, which made the sender's
 * count read as an argument count; a multi-word gate was reduced to a fragment
 * of its own encoding and stored, and a gate whose digits did not line up was
 * refused as malformed. Both faces of that are asserted in
 * `tests/test_orch_validation_wire.c`, which is the only reason a caller should
 * prefer this over calling the plural form with a capacity of one. */
atlas_status atlas_orch_validation_wire_decode(const char *enc, atlas_orch_argv *out,
                                               atlas_err *err);

/* True when `path` is a safe repository-relative prefix: not absolute, no `..`
 * component, no `.` component, no NUL, no backslash, no leading or embedded
 * whitespace-only component, and printable. The one place this question is
 * answered, so a workspace check and a specification check cannot disagree. */
bool atlas_orch_relpath_is_safe(const char *path, size_t len);

/* --- identifiers ---------------------------------------------------------- */

/* --- A11.1: which drivers work in the repository's own tree -----------------
 *
 * The one list, asked by name, and the whole scope of A11.1's reversal of "a
 * job works on a snapshot in a workspace".
 *
 * It lives here rather than as a flag on `atlas_driver` because the daemon has
 * to answer it about a *stored* driver name, at the moment a lease would be
 * granted, without linking the driver table's `run` functions into that
 * decision — and because one list cannot drift from itself.
 *
 * Two things follow from a true answer, and both are refusals:
 *
 *   * **No lease that did not name this driver is ever granted one.** The A8
 *     dispatcher polls with an empty filter, which means "any"; without this it
 *     would take an A11 task, provision a workspace the driver does not use,
 *     run it somewhere it was not meant to run, and complete it — settling the
 *     run without a gate having run where the changes are.
 *   * **This is the driver whose completion settles a run.** A run whose task
 *     ran under an A8 workspace driver has no A11.1 driver behind it, so
 *     nothing has decided anything about it, and A11.0's answer — that its
 *     status is its own axis and nothing derives it — still stands for it.
 *
 * An unknown name answers false. What the predicate guards is a repo-tree
 * execution; a name Atlas cannot resolve is one no dispatcher can execute at
 * all, so withholding the grant would protect nothing and would cost the
 * operator the recorded failure A8 already produces for it. */
bool atlas_orch_driver_is_repo_tree(const char *name);

/* A fresh job identifier from the kernel's random source. Fails rather than
 * falling back to anything weaker: a predictable job id is a job another local
 * process can name before it exists. */
atlas_status atlas_orch_new_uid(atlas_buf *out, atlas_err *err);
/* A fresh lease token, 32 random bytes as 64 hex characters. */
atlas_status atlas_orch_new_token(atlas_buf *out, atlas_err *err);
/* The stored form of a token. The token itself is never written to the
 * database, a log or an artifact. */
atlas_status atlas_orch_token_digest(const char *token, char out[65], atlas_err *err);

#endif /* ATLAS_ORCH_H */
