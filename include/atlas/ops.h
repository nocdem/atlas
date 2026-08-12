/* Atlas - long-running daemon operations, accepted then polled.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Some operations take longer than a client is willing to hold a socket open
 * for. Backing up a 437 MiB index takes tens of seconds; a full semantic index
 * of a real repository takes minutes. Both were reachable over the socket and
 * both were wrong in the same two ways.
 *
 * The first way was visible: `atlas_server_dispatch` runs inline in the serve
 * loop, so the client's frame-header read timed out at
 * ATLAS_IPC_READ_TIMEOUT_MS while the daemon was still working. The operation
 * then *succeeded* — a complete, verified backup appeared on disk — and the
 * operator was told it had failed. A success reported as a failure is worse
 * than a failure: the next thing somebody does about it is re-run it, or work
 * around it, and both are wrong.
 *
 * The second way was not visible and is worse. The serve loop is non-blocking
 * with per-connection state precisely so that one slow client cannot stall
 * every other one — and a thirty-second backup running inside it stalled all of
 * them anyway. The rule was already written down; the backup path simply was
 * not covered by it.
 *
 * So neither is fixed by a longer timeout. What is needed is the shape the
 * product already uses for `repo.sync`: accept the work, answer immediately
 * with something the caller can ask about, and let the caller poll. This is
 * that, generalised, with the two properties polling needs to be trustworthy:
 *
 *   - **Terminal states are terminal.** A record that reached SUCCEEDED or
 *     FAILED never changes again, so repeating `operation.get` is idempotent
 *     and a client that asks twice cannot see the answer move.
 *   - **The client is not the operation.** The work runs on a daemon thread
 *     that holds no reference to the connection, so a client that disconnects,
 *     is killed, or simply stops asking neither cancels nor corrupts it. That
 *     is a requirement, not a side effect: a backup abandoned half-way because
 *     somebody closed a laptop is exactly the artefact nobody can detect later.
 *
 * What this deliberately is *not*: durable. The table lives in memory and a
 * restart forgets every record. That is the honest design rather than a
 * shortcut, because the underlying operations already have deterministic
 * crash behaviour that a durable record could only describe, never improve —
 * a backup publishes atomically or leaves nothing, and a semantic generation
 * publishes atomically or leaves a RUNNING generation nobody points at while
 * the last valid one is still served. After a restart `operation.get` reports
 * an unknown id as unknown, which is true, and the operator's next question is
 * about the artefact rather than about the operation. `docs/operations.md`
 * says so.
 */
#ifndef ATLAS_OPS_H
#define ATLAS_OPS_H

#include <stdbool.h>
#include <stdint.h>

#include "atlas/atlas.h"
#include "atlas/backup.h"
#include "atlas/buf.h"
#include "atlas/error.h"

/* UNKNOWN is zero, for the reason A6 keeps UNKNOWN and BLOCKED there and A8
 * keeps UNKNOWN and DISABLED: a zeroed record must never read as a finished,
 * successful operation. A `memset` here produces "I have no idea", which is the
 * only safe default for a question whose wrong answer is "your backup is
 * fine". */
typedef enum atlas_op_state {
    ATLAS_OP_UNKNOWN = 0,
    ATLAS_OP_RUNNING,
    ATLAS_OP_SUCCEEDED,
    ATLAS_OP_FAILED
} atlas_op_state;

const char *atlas_op_state_name(atlas_op_state s);

/* True once the record can no longer change. The whole idempotency claim rests
 * on this being asked rather than assumed at each call site. */
bool atlas_op_state_is_terminal(atlas_op_state s);

/* The closed vocabulary of things that can be a long operation. A kind is a
 * fixed string chosen by Atlas, never anything a caller supplied, because it
 * is reported back to a terminal. */
typedef enum atlas_op_kind {
    ATLAS_OP_KIND_UNKNOWN = 0,
    ATLAS_OP_KIND_BACKUP_CREATE,
    /* Verification reads every page: `PRAGMA integrity_check` walks the b-trees
     * and every decision revision is rehashed. On an 815 MiB index that is
     * comfortably longer than a client will hold a socket open, so it is a long
     * operation for the same reason creating one is. Converting `create` and
     * leaving `verify` behind simply moved the timeout one command along. */
    ATLAS_OP_KIND_BACKUP_VERIFY,
    ATLAS_OP_KIND_SEM_INDEX
} atlas_op_kind;

const char *atlas_op_kind_name(atlas_op_kind k);

/* One record, copied out under the lock. Callers never hold a pointer into the
 * table: the row callback rule applies here too, and a reader that kept a
 * pointer would be reading a record the operation thread is still writing. */
typedef struct atlas_op_record {
    int64_t id;
    atlas_op_kind kind;
    atlas_op_state state;
    /* The repository this concerns, or 0. Reported so an operator polling
     * several operations can tell them apart without keeping notes. */
    int64_t repo_id;
    int64_t started_at_ms;
    int64_t finished_at_ms;
    /* Populated only in a terminal state. `detail` is the operation's own
     * summary — for a backup, the digest and the verification verdict — and is
     * Atlas-owned text, never a repository's. */
    atlas_status result;
    atlas_buf message;
    atlas_buf detail;

    /* The operation's own result, carried as typed fields rather than as a
     * JSON fragment — the rule the writer's job results follow, and for the
     * same reason: a fragment would have to be spliced into a response
     * verbatim, and "write these bytes as JSON" is the hole through which an
     * unescaped value eventually reaches a client.
     *
     * Meaningful only for ATLAS_OP_KIND_BACKUP_CREATE and only in a terminal
     * state. They are reported rather than recomputed by the client, because a
     * client that recomputed them would be describing whatever the file looks
     * like now instead of what the operation produced. */
    int64_t size_bytes;
    int64_t page_size;
    int64_t page_count;
    int schema_version;
    bool source_online;
    char sha256[65];
    char atlas_version[32];
    /* ATLAS_OP_KIND_BACKUP_VERIFY only, and only in a terminal state. Owned by
     * the record and freed with it. */
    atlas_backup_verify_report verify;
} atlas_op_record;

void atlas_op_record_init(atlas_op_record *r);
void atlas_op_record_free(atlas_op_record *r);

typedef struct atlas_ops atlas_ops;

/* Creates the table and starts the one thread that runs accepted operations.
 *
 * One thread, not a pool and not a thread per request: it makes "at most one
 * long operation runs at a time" a property of the mechanism rather than a
 * check somebody has to remember, and it gives shutdown something to join. A
 * detached thread per request would have to be reasoned about at every exit
 * path, and the reasoning would be wrong the first time the daemon stopped
 * during a backup. */
atlas_status atlas_ops_start(const char *data_dir, FILE *log, atlas_ops **out, atlas_err *err);

/* Stops accepting, waits for the operation in flight, and frees the table.
 *
 * It waits rather than cancelling. An operation in flight is a backup being
 * verified or a semantic generation being written, and both are things that
 * must reach a decision point rather than stop half-way. */
void atlas_ops_stop(atlas_ops *ops);

/* Accepts a backup. Returns the new operation's id.
 *
 * `name` has already been validated as one path component by the caller: this
 * layer never validates a path, because a second validator is a second answer.
 *
 * Refuses with ATLAS_ERR_CONFIG when a backup is already in flight, naming the
 * operation that is running. Deterministic refusal beats queueing: two backups
 * of the same index, started seconds apart, differ only in which one an
 * operator ends up looking at. */
atlas_status atlas_ops_submit_backup(atlas_ops *ops, const char *name, bool force, int64_t *id_out,
                                     atlas_err *err);

/* Accepts a verification of an existing backup. Same thread and same rules as
 * `atlas_ops_submit_backup`; refused while another verification is in flight. */
atlas_status atlas_ops_submit_verify(atlas_ops *ops, const char *path, int64_t *id_out,
                                     atlas_err *err);

/* Records that a semantic index has been accepted, and returns its id.
 *
 * The work itself is *not* run here. Index construction writes the index, and
 * every write in the daemon happens on the writer thread — so the writer runs
 * it and reports back through `atlas_ops_finish`. This function only mints the
 * record, which is what lets the client be answered before the work starts.
 *
 * Refuses when an index operation is already in flight for the same
 * repository, naming it. */
atlas_status atlas_ops_begin_sem_index(atlas_ops *ops, int64_t repo_id, int64_t *id_out,
                                       atlas_err *err);

/* Moves a record to a terminal state. Ignored if it is already terminal, which
 * is what makes the transition safe to attempt from an error path that may
 * itself be reached twice. */
void atlas_ops_finish(atlas_ops *ops, int64_t id, atlas_status result, const char *message,
                      const char *detail);

/* Copies one record out. ATLAS_ERR_REPO when the id is unknown — including
 * after a restart, when every id is. */
atlas_status atlas_ops_get(atlas_ops *ops, int64_t id, atlas_op_record *out, atlas_err *err);

/* True when an operation of this kind is in flight. Asked by the writer thread
 * before it starts a queued index, so a shutdown that raced the queue does not
 * begin work nobody is waiting for. */
bool atlas_ops_is_running(atlas_ops *ops, int64_t id);

#endif /* ATLAS_OPS_H */
