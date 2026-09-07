/* Atlas - A17 T1: migration 33, the deploy state vocabulary and the
 * deploy database operations.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A deploy is a second, narrower lifecycle beside A8's orchestration one: a
 * SUCCEEDED, patch-mode job's stored `changes.patch` artifact, proposed by the
 * remote credential that queued the job, confirmed by a distinct
 * operator-typed digest prefix (A16's "first eight hex of the content hash",
 * applied here to a patch digest instead of a decision revision), and reported
 * terminal only from a root agent's own queue file -- never inferred, never
 * timed out.
 *
 *   PROPOSED --confirm--> CONFIRMED --result--> SUCCEEDED | FAILED
 *       `--cancel--> CANCELLED
 *
 * Every transition is a compare-and-swap against the state this file's caller
 * observed, exactly one row changes, and a ledger row is appended beside it --
 * A8's `atlas_orch_apply_in_tx` discipline, applied to a second table rather
 * than folded into the first one, because a deploy is not a job and mixing the
 * two vocabularies is the mistake `docs/orchestration.md` already warns A8
 * against making with the decision lifecycle.
 *
 * This file is the storage layer only: the RPC methods, the gateway routes,
 * the credential verification and the spool-file ingestion that call these
 * functions are a later task's work. Nothing here opens a socket, starts a
 * process or reads a file outside `<data-dir>/atlas.db`.
 */
#ifndef ATLAS_DEPLOY_H
#define ATLAS_DEPLOY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "atlas/apikey.h" /* ATLAS_APIKEY_SELECTOR_HEX */
#include "atlas/buf.h"
#include "atlas/db.h"
#include "atlas/error.h"
#include "atlas/limits.h" /* ATLAS_TS_MAX */

/* --- identifiers ----------------------------------------------------------- */

/* An Atlas-generated external deploy identifier: "d" plus 32 lowercase hex,
 * generated the same way `atlas_orch_new_uid` builds a job identifier -- from
 * the kernel's random source, never from a pid, a clock or a counter. */
#define ATLAS_DEPLOY_UID_HEX 32u
#define ATLAS_DEPLOY_UID_MAX 40u

/* --- the state machine ------------------------------------------------------
 *
 * Zero is not a state a deploy can be in, for the reason it is not one for a
 * job (`atlas/orch.h`) or a decision (`atlas/decision.h`): a zeroed struct is
 * one nobody filled in, and the safe reading of that is never "proposed".
 */
typedef enum atlas_deploy_state {
    ATLAS_DEPLOY_UNKNOWN = 0,
    /* A remote credential named a SUCCEEDED, patch-mode job whose patch it
     * queued; nothing has been confirmed yet. */
    ATLAS_DEPLOY_PROPOSED,
    /* A distinct operator-typed confirmation, the correct eight-hex prefix of
     * the patch digest, was accepted. A queue request file is written after
     * this commits; `spooled_at` records whether that succeeded. */
    ATLAS_DEPLOY_CONFIRMED,
    /* Terminal. Reported only from the root agent's own result file. */
    ATLAS_DEPLOY_SUCCEEDED,
    /* Terminal. Reported only from the root agent's own result file. */
    ATLAS_DEPLOY_FAILED,
    /* Terminal. Only reachable from PROPOSED, only by the proposing
     * credential. */
    ATLAS_DEPLOY_CANCELLED
} atlas_deploy_state;

const char *atlas_deploy_state_name(atlas_deploy_state s);
/* UNKNOWN never parses: nothing may ask a stored row to hold the value that
 * means "nobody wrote this row correctly". */
bool atlas_deploy_state_parse(const char *s, atlas_deploy_state *out);
/* True for SUCCEEDED, FAILED and CANCELLED. */
bool atlas_deploy_state_is_terminal(atlas_deploy_state s);

/* --- the transition ledger's actor vocabulary ------------------------------- */

typedef enum atlas_deploy_actor {
    ATLAS_DEPLOY_ACTOR_UNKNOWN = 0,
    /* The bearer credential in flight: proposes, cancels. */
    ATLAS_DEPLOY_ACTOR_REMOTE_CREDENTIAL,
    /* A16's channel, applied here: the operator typed the confirmation on the
     * browser dialog the credential's TLS session carries. Never
     * LOCAL_OPERATOR_CONFIRMED -- a reader of the ledger can always tell which
     * channel minted a row. */
    ATLAS_DEPLOY_ACTOR_REMOTE_OPERATOR_CONFIRMED,
    /* The root agent, reporting a result from its own queue file. Never a
     * request parameter: the daemon never trusts a claimed actor. */
    ATLAS_DEPLOY_ACTOR_DEPLOY_AGENT
} atlas_deploy_actor;

const char *atlas_deploy_actor_name(atlas_deploy_actor a);
/* UNKNOWN never parses, for the same reason the state does not. */
bool atlas_deploy_actor_parse(const char *s, atlas_deploy_actor *out);

/* --- bounds ------------------------------------------------------------------ */

/* The root agent's result text, bounded because it is stored, encoded and
 * rendered, and because an unbounded field is a memory bound the agent's own
 * queue file would otherwise set. A display bound, not a safety one -- a
 * reader shown a prefix is told it is one. */
#define ATLAS_DEPLOY_RESULT_TEXT_MAX (32u * 1024u)

/* The confirmation is the first this many hex characters of the patch
 * digest -- A16's "revision content hash prefix" rule, applied to a patch. */
#define ATLAS_DEPLOY_CONFIRMATION_HEX 8u

/* Pagination ceiling for `atlas_db_deploy_list`, A8's `ATLAS_ORCH_LIST_MAX`
 * mirrored for the same reason: a caller is always told whether more exist,
 * and a page is never unbounded. */
#define ATLAS_DEPLOY_LIST_MAX 200

/* --- the full row, for a single read ---------------------------------------- */

/* Every column of one `deploys` row, exactly as migration 33 declares them.
 * Text columns are owned `atlas_buf`s (never fixed arrays): a deploy row is
 * read rarely -- once per `deploy.remote_get` or `atlas_deploy_status` call --
 * so there is no hot-path reason to duplicate `atlas_orch_list_row`'s
 * fixed-buffer shape here, and every field looks the same to a caller whether
 * it holds eight bytes or the whole result text.
 *
 * A NULL-able schema column (`confirmed_at`, `spooled_at`, `terminal_at`)
 * reads back as an empty buffer, never a NULL `atlas_buf *` -- the same
 * "empty means not yet set" convention `atlas_repo_info.last_scan_at` already
 * uses. */
typedef struct atlas_deploy_row {
    int64_t id;
    atlas_buf deploy_uid;
    atlas_buf job_uid;
    int64_t repo_id;
    atlas_buf base_commit;
    atlas_buf patch_digest;
    int64_t patch_bytes;
    atlas_deploy_state state;
    atlas_buf proposed_key_id;
    atlas_buf confirmed_key_id;
    atlas_buf created_at;
    int64_t created_ms;
    atlas_buf confirmed_at; /* "" until CONFIRMED */
    atlas_buf spooled_at;   /* "" until the request file was written */
    atlas_buf terminal_at;  /* "" until SUCCEEDED, FAILED or CANCELLED */
    bool result_dry_run;
    atlas_buf result_stage;
    atlas_buf result_rollback;
    atlas_buf result_head_before;
    atlas_buf result_head_after;
    atlas_buf result_version;
    atlas_buf result_text;
} atlas_deploy_row;

void atlas_deploy_row_init(atlas_deploy_row *r);
void atlas_deploy_row_free(atlas_deploy_row *r);

/* --- the root agent's result, as `atlas_db_deploy_finish_in_tx` records it -- */

/* What the root agent's `.res` queue file reports, already parsed by this
 * function's caller -- T1 stores it, T2/T3 parse the bounded `key value` file
 * format D.3 describes and refuse a malformed one as FAILED/INGEST before this
 * is ever called. Every `const char *` may be NULL, read as "".
 *
 * There is deliberately no `outcome` string here: `success` is the boolean a
 * caller already had to decide from the parsed `outcome SUCCEEDED|FAILED`
 * line, and carrying both a bool and a string that must agree is the kind of
 * duplication `docs/engineering-rules.md` already warns against. `stage` and
 * `rollback` are stored exactly as given, with no closed vocabulary enforced
 * here: migration 33 declares no CHECK on either column, because the agent's
 * own stage and rollback vocabularies (D.4) belong to the agent's contract,
 * not to this storage layer, and a stage name Atlas has never heard of is
 * still worth recording rather than refusing. */
typedef struct atlas_deploy_result {
    bool success;
    const char *stage;
    bool dry_run;
    const char *rollback;
    const char *head_before;
    const char *head_after;
    const char *version;
    const char *text; /* bounded by ATLAS_DEPLOY_RESULT_TEXT_MAX bytes */
} atlas_deploy_result;

/* --- a lighter row, for a list ----------------------------------------------- */

typedef struct atlas_deploy_list_row {
    int64_t id;
    char deploy_uid[ATLAS_DEPLOY_UID_MAX];
    char job_uid[ATLAS_DEPLOY_UID_MAX]; /* a job_uid has the same shape budget */
    int64_t repo_id;
    atlas_deploy_state state;
    char proposed_key_id[ATLAS_APIKEY_SELECTOR_HEX + 1u];
    char confirmed_key_id[ATLAS_APIKEY_SELECTOR_HEX + 1u];
    char created_at[ATLAS_TS_MAX];
} atlas_deploy_list_row;

/* Row callbacks receive borrowed pointers valid only for the call, as
 * everywhere else in Atlas. Copy anything that must outlive it. */
typedef atlas_status (*atlas_deploy_list_cb)(const atlas_deploy_list_row *row, void *ud,
                                             atlas_err *err);

/* --- writes: the state machine ----------------------------------------------
 *
 * Every `_in_tx` function assumes it runs inside a transaction its caller
 * already opened with `atlas_db_begin` -- A8's `atlas_orch_apply_in_tx`
 * convention, kept exactly, because `atlas_db_deploy_confirmed_uid` (below)
 * must be askable from *inside* the orchestration submit transaction A11.0
 * already opens, and the four write functions here must be askable from
 * inside a transaction the daemon's IPC layer opens once per RPC method --
 * never a transaction opened and closed inside this file, which would make
 * the credential check and the write two separate races instead of one
 * atomic decision.
 */

/* Proposes a deploy from a job's stored patch artifact.
 *
 * Refuses, inside the transaction, in this order: no job with this uid; the
 * job has not SUCCEEDED; the job's mode is not "patch"; the job carries no
 * stored `changes.patch` artifact, or the artifact's content was not stored,
 * or it is empty; `key_id` does not match the credential that queued the job
 * (`orch_jobs.submit_key_id`); this repository already has a deploy in
 * flight (PROPOSED or CONFIRMED) -- `idx_deploys_one_active` is the schema
 * guarantee, this is the sentence that names it, on A11.0's "the schema is
 * the guarantee, the C check names it" precedent; this job already has a
 * deploy that is PROPOSED, CONFIRMED or SUCCEEDED -- `idx_deploys_one_per_job`
 * is that guarantee's own schema fact, and a FAILED or CANCELLED deploy does
 * not count, so the same job may be proposed again after either.
 *
 * On success, `*deploy_uid_out` holds the freshly generated uid and a
 * `deploy_transitions` row records UNKNOWN -> PROPOSED, actor
 * REMOTE_CREDENTIAL, `key_id`. */
atlas_status atlas_db_deploy_propose_in_tx(atlas_db *db, const char *job_uid, const char *key_id,
                                           atlas_buf *deploy_uid_out, atlas_err *err);

/* The uid of the CONFIRMED deploy for this repository, or an empty buffer
 * when none exists. Always succeeds when the query itself succeeds -- this is
 * a read, not a refusal, and it is the caller's job to refuse a root
 * submission when the result is non-empty. A PROPOSED deploy does not count:
 * only a CONFIRMED one is the minutes-away-from-a-restart window D.1 exists
 * for, and a root submission landing while a deploy is merely PROPOSED is
 * exactly as safe as one landing before it was proposed at all.
 *
 * Asked inside both of A14's submit write points (`job.submit` and
 * `job.remote_submit`), inside the same transaction as the submission itself,
 * on A11.0's "every check inside the submit transaction" precedent: a check
 * performed before the transaction opens is worthless against a confirm that
 * lands in the gap. */
atlas_status atlas_db_deploy_confirmed_uid(atlas_db *db, int64_t repo_id, atlas_buf *uid_or_empty,
                                           atlas_err *err);

/* Confirms a PROPOSED deploy. Refuses, inside the transaction, in this order:
 * no deploy with this uid; the deploy is not PROPOSED; `confirmation` is not
 * exactly the first `ATLAS_DEPLOY_CONFIRMATION_HEX` characters of the stored
 * patch digest (a length mismatch is a mismatch, not a prefix match against a
 * shorter string); one or more `orch_jobs` rows are not terminal
 * (`atlas_orch_state_is_terminal`'s vocabulary, counted globally -- a deploy
 * restarts the whole daemon, which ends every job regardless of which
 * repository it targets, so the count is not scoped to this deploy's own
 * repository).
 *
 * On success the row moves to CONFIRMED, `confirmed_key_id` and
 * `confirmed_at` are recorded, and a `deploy_transitions` row records
 * PROPOSED -> CONFIRMED, actor REMOTE_OPERATOR_CONFIRMED, `key_id`. */
atlas_status atlas_db_deploy_confirm_in_tx(atlas_db *db, const char *deploy_uid,
                                           const char *key_id, const char *confirmation,
                                           atlas_err *err);

/* Cancels a PROPOSED deploy. Refuses "no such deploy" both when no deploy
 * carries this uid and when one does but `key_id` is not the credential that
 * proposed it -- A14 T3's `op_cancel` precedent: a mismatched credential is
 * told the same thing an absent deploy is, so a caller cannot use this to
 * enumerate another credential's deploys. Refuses distinctly when the deploy
 * exists, was proposed by this credential, but is not PROPOSED (already
 * CONFIRMED, or already terminal).
 *
 * On success the row moves to CANCELLED and a `deploy_transitions` row
 * records PROPOSED -> CANCELLED, actor REMOTE_CREDENTIAL, `key_id`. */
atlas_status atlas_db_deploy_cancel_in_tx(atlas_db *db, const char *deploy_uid,
                                          const char *key_id, atlas_err *err);

/* Records that the request queue file for a CONFIRMED deploy was written.
 * Refuses when no deploy carries this uid, or when it is not CONFIRMED.
 *
 * This does not change `state` -- a spooled CONFIRMED deploy is still
 * CONFIRMED, waiting on the root agent's result -- so it writes no
 * `deploy_transitions` row: that ledger records state changes, and recording
 * a from-CONFIRMED-to-CONFIRMED entry would misdescribe what happened as a
 * transition when none occurred. `spooled_at` is the durable record of this
 * event; a caller wanting an audit trail of it reads that column. The CAS
 * discipline is still exactly one row changed on `deploys` itself: the
 * predicate is `state = 'CONFIRMED'`, so calling this twice on an
 * already-spooled row is refused rather than silently repeated. */
atlas_status atlas_db_deploy_mark_spooled_in_tx(atlas_db *db, const char *deploy_uid,
                                                atlas_err *err);

/* Records the root agent's result for a CONFIRMED deploy, moving it to
 * SUCCEEDED or FAILED per `r->success`. Refuses when no deploy carries this
 * uid, when it is not CONFIRMED, or when `r->text` is longer than
 * `ATLAS_DEPLOY_RESULT_TEXT_MAX` bytes -- refused rather than truncated, on
 * every other bound in Atlas' own precedent.
 *
 * On success a `deploy_transitions` row records CONFIRMED -> SUCCEEDED or
 * CONFIRMED -> FAILED, actor DEPLOY_AGENT, an empty `key_id` -- the root
 * agent carries no credential, only a queue file Atlas already trusts because
 * it wrote the matching request. */
atlas_status atlas_db_deploy_finish_in_tx(atlas_db *db, const char *deploy_uid,
                                          const atlas_deploy_result *r, atlas_err *err);

/* --- reads -------------------------------------------------------------------
 *
 * Reads take no transaction of their own to require: SQLite's default
 * isolation is enough for a single-statement-equivalent read, exactly as
 * every other `atlas_db_*_get`/`_list` pair in Atlas.
 */

/* The caller owns `*out`: call `atlas_deploy_row_init` on it before this and
 * `atlas_deploy_row_free` after, whether or not `*have` comes back true --
 * `atlas_repo_info`'s own `_init`-before/`_free`-after convention. This
 * function never initialises `*out` itself, so a caller that skips the init
 * hands every `atlas_buf_set_str` call inside it a struct full of garbage
 * pointers rather than an empty buffer. */
atlas_status atlas_db_deploy_get(atlas_db *db, const char *deploy_uid, atlas_deploy_row *out,
                                 bool *have, atlas_err *err);

/* Lists deploys in id order, newest last, paginated by `after_id`/`limit` on
 * A8's `atlas_db_orch_job_list_by_key`/`_list_remote` pair exactly:
 * `key_id_or_null == NULL` lists every deploy (the operator's view); a
 * non-NULL `key_id_or_null` lists only deploys where it is the proposing
 * *or* the confirming credential -- the same principal may hold
 * `remote_submit_key` and `remote_deploy_key` selectors that differ, and
 * A14's "`remote_dispose_key` and `remote_submit_key` may never name the same
 * id" rule keeps `deploys:confirm` and `jobs:submit` on two different keys in
 * the ordinary case, so a caller's own view spans both roles it may hold. */
atlas_status atlas_db_deploy_list(atlas_db *db, const char *key_id_or_null, int64_t after_id,
                                  int64_t limit, atlas_deploy_list_cb cb, void *ud,
                                  int64_t *count_out, int64_t *cursor_out, bool *more_out,
                                  atlas_err *err);

/* The uids of every CONFIRMED deploy whose `spooled_at` is still empty --
 * exactly the daemon startup sweep D.1 describes: a deploy the daemon
 * confirmed but could not spool a request file for (a crash, a restart, a
 * `results` directory the path unit had not yet created) stays CONFIRMED
 * forever otherwise, and `idx_deploys_one_active` would then block every
 * later deploy for that repository.
 *
 * `uids_out` holds the matching uids concatenated with no delimiter: each is
 * exactly `ATLAS_DEPLOY_UID_HEX + 1` bytes ("d" plus 32 lowercase hex), a
 * fixed width that needs no netstring framing, so a caller decodes by slicing
 * fixed-size chunks. Empty when none are owed. */
atlas_status atlas_db_deploy_confirmed_unspooled(atlas_db *db, atlas_buf *uids_out,
                                                 atlas_err *err);

#endif /* ATLAS_DEPLOY_H */
