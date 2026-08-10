/* Atlas - operator authority: what Atlas will let a caller change, and why.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A4 established an operator channel and described its limits honestly: a
 * same-UID process able to drive a pseudo-terminal — including an AI agent with
 * shell access — may imitate it, and `LOCAL_OPERATOR_CONFIRMED` identifies the
 * channel rather than a person.
 *
 * A7 asks what follows from that, and the answer is that a terminal cannot be
 * the gate. **Nothing observable from inside a process distinguishes a human
 * from a program running as the same user.** Not `isatty`, not `/dev/tty`, not
 * the ownership of the pseudo-terminal, not an environment variable, not the
 * name of the parent process, not a session id, not typing `YES`, and not how
 * long the answer took to arrive. Every one of those is producible by a few
 * lines of code, and `tests/test_decision_operator.c` has produced them since
 * A4 — that suite allocates a pty and types the confirmation, which is the
 * demonstration rather than a simulation of one.
 *
 * So authority is not detected. It is **configured, outside the reach of the
 * principal it constrains, or it does not exist** — and when it does not exist,
 * Atlas refuses the operations that would need it rather than performing them
 * behind a prompt that proves nothing.
 *
 * ## What a grant requires
 *
 * Every condition is a property of the filesystem that the constrained uid
 * cannot change:
 *
 *   1. A policy file at `ATLAS_AUTHORITY_POLICY_PATH`, reached without
 *      traversing a single symbolic link.
 *   2. That file, and every directory on the way to it, owned by uid 0 and
 *      writable by nobody else.
 *   3. `operator_uid = N` in it, and `getuid() == N`.
 *   4. The running `atlas` executable owned by uid 0 and writable by nobody
 *      else, along with every directory leading to it.
 *
 * Condition 4 is not decoration. If the uid Atlas is protecting against can
 * replace the `atlas` binary, it can ship one whose probe always grants — so a
 * check running from a writable binary is a check that reports whatever the
 * attacker last compiled. An authority system that omits it is describing its
 * own source code rather than the machine it runs on.
 *
 * ## What a grant does not do
 *
 * It does not protect the database. A process running as the uid that owns
 * `atlas.db` can open it with SQLite and write any row it likes, with no Atlas
 * code involved and no Atlas check reachable. The lock protects the
 * Atlas-mediated route and nothing else. Protecting the record itself requires
 * the daemon, the data directory and the index to be owned by a uid the model
 * does not have — which is a deployment decision, not a code change, and
 * `docs/security/A7_SECURITY_REVIEW.md` states exactly what it involves.
 *
 * It also does not identify a person. `operator_uid` names an OS principal. If
 * a human shares that account with an agent, the agent has the authority; that
 * is what sharing an account means, and no amount of prompting changes it.
 */
#ifndef ATLAS_AUTHORITY_H
#define ATLAS_AUTHORITY_H

#include <stdbool.h>
#include <stddef.h>

#include "atlas/error.h"

/* The policy Atlas consults. A compiled-in absolute path on purpose: an
 * environment variable or a flag would be chosen by the caller, and a caller
 * that can choose the policy is not constrained by it. */
#define ATLAS_AUTHORITY_POLICY_PATH "/etc/atlas/authority.conf"

/* Zero is LOCKED, deliberately, for the reason A6 keeps UNKNOWN and BLOCKED at
 * zero: a zeroed struct is one nobody filled in, and the safe reading of that
 * is never "permitted". */
typedef enum atlas_authority_state {
    ATLAS_AUTHORITY_LOCKED = 0,
    ATLAS_AUTHORITY_GRANTED = 1
} atlas_authority_state;

/* Why a probe answered as it did. Reported to the operator verbatim, because
 * "locked" without a reason is indistinguishable from a bug. */
typedef enum atlas_authority_reason {
    ATLAS_AUTHORITY_REASON_UNKNOWN = 0,
    /* No policy file exists. The ordinary state of a machine nobody has
     * configured, and the state this machine is in. */
    ATLAS_AUTHORITY_REASON_NO_POLICY,
    /* A component of the policy path is a symbolic link, so whoever can create
     * links there chooses the policy. */
    ATLAS_AUTHORITY_REASON_POLICY_PATH_UNSAFE,
    /* The policy, or a directory leading to it, is owned by or writable by
     * somebody other than root. */
    ATLAS_AUTHORITY_REASON_POLICY_WRITABLE,
    /* Present, root-owned, and not something Atlas can read as a policy. */
    ATLAS_AUTHORITY_REASON_POLICY_MALFORMED,
    /* A valid policy that names a different uid than the one asking. */
    ATLAS_AUTHORITY_REASON_NOT_THE_OPERATOR,
    /* The `atlas` executable can be replaced by a non-root uid, so any check it
     * performs can be replaced with one that always says yes. */
    ATLAS_AUTHORITY_REASON_BINARY_WRITABLE,
    /* Everything held. */
    ATLAS_AUTHORITY_REASON_GRANTED
} atlas_authority_reason;

typedef struct atlas_authority {
    atlas_authority_state state;
    atlas_authority_reason reason;
    /* The uid the policy names, when there was a readable one. -1 otherwise.
     * Reported so an operator can see the mismatch rather than guess it. */
    long long operator_uid;
    /* The uid that asked. */
    long long caller_uid;
    /* The path whose inspection produced `reason`, when one did. Owned by the
     * struct, bounded, and never a value a caller supplied. */
    char detail[256];
} atlas_authority;

/* The operations a grant is required for.
 *
 * There is one, and the scope was argued rather than assumed. The question a
 * candidate has to answer is **not** "would a model like to do this?" but "does
 * refusing it here stop anything?" — because a check an adversary can walk
 * around is worse than no check: it reads as protection in a review and
 * provides none.
 *
 * Against the adversary A7 is written for — a process running as the uid that
 * owns the data directory — these were considered and deliberately left
 * unguarded:
 *
 *   `backup create`     the index is readable by that uid, so `cp` produces the
 *                       same file. A guard moves the verb, not the bytes.
 *   `backup restore`    the index is writable by that uid, so `sqlite3` or `mv`
 *                       replaces it with no Atlas code involved.
 *   `maintenance prune` likewise: `DELETE FROM repo_events` needs no Atlas.
 *   `repo add`/`remove` the registry is a table in that same writable file.
 *
 * In the *separated* deployment those four are already prevented, by the
 * filesystem: a model that is not the service uid cannot open the data
 * directory at all. So an Atlas-level check on them is redundant where
 * separation exists and cosmetic where it does not, which is every case.
 * Guarding them would also make Atlas unusable for the ordinary single-user
 * install, where there is no adversary and the only effect is to stop the owner
 * taking a backup.
 *
 * The lifecycle is genuinely different, and the difference is what Atlas
 * produces rather than what it moves. Approving through Atlas mints a coherent
 * record: a consumed challenge, an event in the append-only ledger, a status
 * cache written in the same transaction, and a revision marked
 * `LOCAL_OPERATOR_CONFIRMED`. Nothing downstream can tell that record from one
 * a person caused. Refusing it does not stop a same-uid process from writing
 * `state='APPROVED'` into SQLite by hand — but that forgery no longer agrees
 * with the ledger, so `atlas_db_decision_verify` reports it and `atlas doctor`
 * surfaces it. The guard converts an undetectable forgery into a detectable
 * one, which is a real property and the only one claimed. */
typedef enum atlas_authority_op {
    /* Minting or spending a lifecycle capability: approve, reject, supersede,
     * revalidate. Proposing and revising are not here; a proposal claims
     * nothing about who decided. */
    ATLAS_AUTHORITY_OP_DECISION_LIFECYCLE = 0
} atlas_authority_op;

const char *atlas_authority_op_name(atlas_authority_op op);
const char *atlas_authority_reason_name(atlas_authority_reason r);
/* One sentence, suitable for showing an operator, saying what the reason means
 * and what would change it. Never includes a caller-supplied string. */
const char *atlas_authority_reason_explain(atlas_authority_reason r);

/* Inspects the machine and answers. Never fails: an error of any kind is a
 * locked profile with a reason, because a probe that could fail open would be
 * the whole vulnerability. */
void atlas_authority_probe(atlas_authority *out);

/* The same probe against an explicit policy path and executable path, so the
 * decision procedure can be tested against real filesystem shapes rather than
 * against a description of them.
 *
 * This is not a bypass and cannot be used as one: it is not reachable from the
 * CLI, from IPC, from MCP or from a hook — nothing parses a path into it —
 * and every caller in the shipped binary passes the compiled-in constants.
 * What it takes is *which* paths to inspect; what it enforces about them is not
 * a parameter. A policy at a caller-chosen path still has to be root-owned on a
 * root-owned chain, which is exactly the condition an unprivileged caller
 * cannot manufacture anywhere on the filesystem. */
void atlas_authority_probe_at(const char *policy_path, const char *exe_path,
                              atlas_authority *out);

/* ATLAS_OK when `op` may proceed. Otherwise a refusal carrying the reason and
 * the remedy, with the status the CLI exits on.
 *
 * Called at the process entry points — CLI dispatch — rather than deep in the
 * write path. That placement is deliberate and is argued in the review: the
 * library is not a boundary, because anything already executing in this process
 * as this uid can write `atlas.db` directly and needs no Atlas function at all.
 * The boundary is the point where a request enters the process. */
atlas_status atlas_authority_require(atlas_authority_op op, atlas_err *err);

#endif /* ATLAS_AUTHORITY_H */
