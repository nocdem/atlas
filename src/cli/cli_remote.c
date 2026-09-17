/* Atlas - command line front end.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The CLI parses arguments, calls the service layer, and hands results to a
 * renderer. It contains no SQL, no git invocation and no output formatting.
 */
#include "atlas/cli.h"

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/authority.h"
#include "atlas/backup.h"
#include "atlas/gateway.h"
#include "atlas/daemon.h"
#include "atlas/hook.h"
#include "atlas/integrate.h"
#include "atlas/ipc.h"
#include "atlas/maintenance.h"
#include "atlas/mcp.h"
#include "atlas/sem.h"
#include "atlas/unit.h"
#include "cli/render.h"


#include "cli/cli_internal.h"

/* Which commands this process can answer about an index it does not own.
 *
 * A7.1 puts the index behind a separate OS principal: `/var/lib/atlas` is 0700
 * `atlasd`, and it has to stay that way because `atlas-worker` is a member of
 * the client group and must not be able to read the index. So a client uid can
 * never open that database, and every read has to be a question put to the
 * daemon. This function decides only *whether* a command has such a path; the
 * command itself then runs its normal code with `ctx == NULL`, calling the
 * remote twin of its service function at the same call site, so both renderers
 * and the JSON contract are shared rather than reproduced.
 *
 * A command with no path here is refused with what is actually true, rather
 * than with SQLite's "unable to open database file" — which describes a
 * permission the caller was never going to have and points at a file they
 * cannot see. There is no fallback to a local read anywhere: A7.1's rule is
 * that a client which cannot reach the daemon fails, rather than quietly
 * answering from the pre-cutover per-user database. */
bool atlas_cli_remote_serves(const cli_state *st) {
    const char *cmd = st->command;
    const char *sub = st->operand_count > 0 ? st->operands[0] : "";

    /* `doctor` needs no path: it opens in INSPECT mode, creates nothing, and
     * reporting an index it cannot read is the correct answer and the reason
     * somebody runs it. */
    if (strcmp(cmd, "doctor") == 0) {
        return true;
    }
    if (strcmp(cmd, "status") == 0 || strcmp(cmd, "search") == 0 ||
        strcmp(cmd, "events") == 0 || strcmp(cmd, "sync") == 0 || strcmp(cmd, "file") == 0 ||
        strcmp(cmd, "history") == 0 || strcmp(cmd, "diff") == 0) {
        return true;
    }
    /* Keyed on the subcommand, not the command: a command whose siblings have
     * no remote form would otherwise reach its handler with a NULL context and
     * dereference it. */
    if (strcmp(cmd, "repo") == 0) {
        /* Only `list`. `add` and `remove` are absent deliberately — nothing over
         * the socket may change the registry — and there is no `repo state`
         * subcommand: the repository-state report belongs to `atlas events`,
         * which is routed in its own block. */
        return strcmp(sub, "list") == 0;
    }
    if (strcmp(cmd, "daemon") == 0) {
        return strcmp(sub, "status") == 0;
    }
    if (strcmp(cmd, "code") == 0) {
        return strcmp(sub, "status") == 0 || strcmp(sub, "file") == 0 ||
               strcmp(sub, "symbol") == 0 || strcmp(sub, "search") == 0 ||
               strcmp(sub, "deps") == 0 || strcmp(sub, "impact") == 0 ||
               /* A8-CI. Four reads, served by the ordinary method group. Under
                * A7.1 these are the only forms that work at all from an
                * operator's account: the index is 0700 `atlasd`. `index` is
                * absent on purpose — it is a write and has no read method. */
               /* A8-CI closeout: indexing is served over the socket now, so an
                * operator never has to stop the service or become the service
                * account. It is offered only to the peer the root-owned policy
                * names, so reaching the name is not the same as being allowed
                * to use it. */
               strcmp(sub, "index") == 0 ||
               /* A9.2.3. Served for the same reason `index` is: under A7.1 the
                * index is 0700 `atlasd`, so an operator's account has no local
                * handle and could not record a build description at all. It is
                * in the operator-uid group, so being served is not being
                * allowed. */
               strcmp(sub, "sem-config") == 0 ||
               strcmp(sub, "sem-status") == 0 || strcmp(sub, "semantic") == 0 ||
               strcmp(sub, "callers") == 0 || strcmp(sub, "callees") == 0 ||
               strcmp(sub, "trace") == 0 || strcmp(sub, "sem-impact") == 0 ||
               strcmp(sub, "tests") == 0 || strcmp(sub, "explain") == 0;
    }
    if (strcmp(cmd, "gate") == 0) {
        return strcmp(sub, "check") == 0 || strcmp(sub, "show") == 0;
    }
    if (strcmp(cmd, "verify") == 0) {
        /* A9.2.1. The six intake verbs are served too, and they have to be:
         * under A7.1 the index is 0700 `atlasd`, so the socket is the only
         * path by which an operator's account can record a claim at all.
         * Being served is not being authorised — `verify.evaluate` may cause
         * Atlas to move a lifecycle state, but the gates are in a root-owned
         * file no peer here can read, let alone edit.
         *
         * **`run` is deliberately absent from this list, because there is no
         * `verify.run` method.** Claiming it was served cost the accuracy of
         * the one message that explains the situation: on a system deployment
         * the command opened a context, failed to get a writable index, and
         * said "no index is available to write" — which reads as a broken
         * install rather than as an operation this account cannot perform
         * here. Saying nothing is served is the truthful answer, and it points
         * at the documented gap instead of at the operator's machine. */
        return strcmp(sub, "show") == 0 ||
               strcmp(sub, "policy") == 0 || strcmp(sub, "claim") == 0 ||
               strcmp(sub, "evidence") == 0 || strcmp(sub, "produce") == 0 ||
               strcmp(sub, "attest") == 0 || strcmp(sub, "depend") == 0 ||
               strcmp(sub, "evaluate") == 0;
    }
    if (strcmp(cmd, "context") == 0) {
        return strcmp(sub, "build") == 0;
    }
    /* A12.1 T16. `status`, `scan` and `reconcile` are served: `scan` and
     * `reconcile` unconditionally submit through the daemon's own operator
     * methods (T11's `memory.put`/`memory.reconcile`) and never dereference
     * `ctx` at all (`src/core/service_memory.c`'s `atlas_service_memory_scan`/
     * `_reconcile` take no `ctx` parameter), which is exactly the surface an
     * A7.1 account with no local database handle needs -- so serving them is
     * safe by construction, not merely untested. `status` reads through the
     * existing `memory.status` operator method exactly as the local read
     * does, `sem_status`'s own shape. `pack`, `diff`, `patch` and `trailer`
     * have no remote form in this build -- they fall through to the generic
     * refusal below, which is honest about it: a read with no remote form is
     * a real, if disclosed, gap (see `src/core/service_memory.c`'s header),
     * not the "write or typo" pair the refusal's own wording assumes. */
    if (strcmp(cmd, "memory") == 0) {
        return strcmp(sub, "status") == 0 || strcmp(sub, "scan") == 0 ||
               strcmp(sub, "reconcile") == 0;
    }
    /* Served over the socket and by nothing else: the operations table lives in
     * the daemon's memory, so there is no local form of this question and never
     * will be. */
    if (strcmp(cmd, "operation") == 0) {
        return strcmp(sub, "status") == 0;
    }
    /* Backup create and verify, and deliberately not restore.
     *
     * A5 gave backup no remote form because the uid that owns the index can
     * copy the file anyway. Under A7.1 that stopped being true and nobody
     * noticed: the index is 0700 `atlasd`, so the operator account could not
     * take a backup at all and got "there is no Atlas index to back up" — which
     * is false. These two are served over the socket and refused by the daemon
     * unless the peer is the uid the root-owned policy names. `restore`
     * remains local-only: replacing the record should require stopping the
     * daemon, which is exactly what the local path already enforces through the
     * writer lock. */
    if (strcmp(cmd, "backup") == 0) {
        return strcmp(sub, "create") == 0 || strcmp(sub, "verify") == 0;
    }
    if (strcmp(cmd, "decision") == 0) {
        return strcmp(sub, "link") == 0 || strcmp(sub, "links") == 0 ||
               strcmp(sub, "list") == 0 || strcmp(sub, "search") == 0 ||
               strcmp(sub, "for-file") == 0 || strcmp(sub, "show") == 0 ||
               strcmp(sub, "export") == 0 || strcmp(sub, "history") == 0 ||
               strcmp(sub, "orphaned") == 0 || strcmp(sub, "legacy") == 0 ||
               /* The operator channel. Served over the socket, and refused by
                * the daemon unless this peer is the uid the root-owned policy
                * names — so reaching the name is not the same as being allowed
                * to use it. `propose` and `revise` are already served by the
                * ordinary decision group. */
               strcmp(sub, "propose") == 0 || strcmp(sub, "revise") == 0 ||
               strcmp(sub, "promote") == 0 || strcmp(sub, "approve") == 0 ||
               strcmp(sub, "reject") == 0 || strcmp(sub, "supersede") == 0 ||
               strcmp(sub, "revalidate") == 0 || strcmp(sub, "resolve") == 0;
    }
    /* A15 T5. Served for the reason the operator channel above is: under
     * A7.1 the index is 0700 `atlasd`, so an operator's own account has no
     * local database handle at all. `run_review` (above) checks authority
     * in this process regardless of `ctx`; the terminal check is
     * `atlas_service_review_apply`'s own (skipped only under `check_only`),
     * not run_review's. `atlas_service_review_apply` dispatches every read
     * it needs to the remote form itself when `ctx == NULL` (`show_revision`
     * in src/core/service_review.c), and `atlas_service_decision_confirm` —
     * the one function it loops — already does the same for its own reads
     * and for the write that spends a capability. Being served is not being
     * authorised: the daemon still refuses unless this peer is the uid the
     * root-owned policy names. */
    if (strcmp(cmd, "review") == 0) {
        return strcmp(sub, "apply") == 0;
    }
    return false;
}

/* Whether `cmd` is a command at all.
 *
 * Without this, an unknown command on a foreign index is answered by the
 * refusal below — which tells somebody who mistyped that the system index is
 * daemon-owned, and never that there is no such command. The dispatch chain's
 * own unknown-command error is produced after a context is opened, which is
 * exactly what a foreign index does not allow, so the check has to happen here.
 *
 * A new command must be added to this list. That duplication is real, and
 * **nothing in this repository's own suite reaches it**: this list is only
 * ever consulted on the foreign-index path (`atlas_datadir_is_foreign`), which
 * needs an active root-owned system policy naming a data directory this uid
 * does not own — `scripts/smoke.sh` always passes `--data-dir`
 * (`ATLAS_DATADIR_OVERRIDE` wins ahead of the system policy in
 * `atlas_datadir_resolve`), so it never takes this branch either, for `review`
 * or for any other command; and a test that ran without an override would risk
 * opening the real HOME-resolved index on any machine with no system policy
 * configured, which this project's own testing rule forbids. A previous
 * version of this comment claimed the CLI smoke matrix caught an omission
 * here — it does not, and could not without either manufacturing a root-owned
 * policy (the thing `docs/security/A7_SECURITY_REVIEW.md`'s tests refuse to
 * do) or that HOME risk. `tests/test_cli.c`'s
 * `test_the_plan_command_is_wired` already says this plainly: "**What this
 * cannot reach is `COMMANDS[]`**... it is still a place this suite cannot
 * check, which is what CLAUDE.md says about it." What actually catches a
 * forgotten entry here is CLAUDE.md's own instruction to run a new command
 * once from the built binary, and a real system deployment. */
bool atlas_cli_is_a_command(const char *cmd) {
    static const char *const COMMANDS[] = {
        "doctor",  "repo",    "scan",      "status",  "search",  "file",     "history",
        "diff",    "daemon",  "sync",      "events",  "code",    "decision", "gate",
        "job",     "dispatcher", "scanner", "backup", "maintenance", "service", "mcp", "hook",
        "integrate", "version", "help", "context", "operation", "api-key", "gateway",
        "verify",   "plan",   "memory",  "review",
    };
    for (size_t i = 0; i < sizeof COMMANDS / sizeof COMMANDS[0]; i++) {
        if (strcmp(cmd, COMMANDS[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Whether this invocation would write the index.
 *
 * `mode_for` answers it for `scan`, `repo add` and `repo remove`, which it must
 * because it also chooses the context mode. It answers AUTO for `code index`
 * and `code sync`, which is right for a per-user install — AUTO takes the
 * writer lock when it is free — and wrong for the question asked here.
 *
 * The consequence was that on a system deployment both fell through to the
 * generic "not served over the socket" answer, which does not say that the
 * operation exists and that this account may not perform it. The deterministic
 * NOT_AUTHORIZED distinction was implemented and unreachable on the one
 * deployment where it is the whole point.
 *
 * Two names, not an inventory: these are the only commands that write the index
 * and are not already WRITE. A third would be a deliberate edit here, and the
 * failure if it is forgotten is a vaguer message rather than a wrong one. */
static bool would_write_index(const cli_state *st) {
    if (atlas_cli_mode_for(st) == ATLAS_CTX_WRITE) {
        return true;
    }
    return strcmp(st->command, "code") == 0 && st->operand_count > 0 &&
           (strcmp(st->operands[0], "index") == 0 || strcmp(st->operands[0], "sync") == 0);
}

atlas_status atlas_cli_remote_refuse(const cli_state *st, atlas_err *err) {
    /* A write against somebody else's index is not a permissions problem to be
     * reported from inside a chmod; it is a thing this account does not do.
     * Registration and scanning under a system deployment are the operator
     * ceremony in docs/security/A7_1_OPERATIONS.md.
     *
     * NOT_AUTHORIZED leads, as a stable token rather than prose, so a caller
     * tells it from NOT_REGISTERED without reading English. The two answer
     * different questions — "Atlas does not hold this" and "Atlas holds it and
     * you may not do this to it" — and a caller that cannot tell them apart
     * will retry the wrong one. */
    if (would_write_index(st)) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "NOT_AUTHORIZED: `atlas %s%s%s` writes the index, and the index is "
                             "owned by the Atlas service account. The operation exists and this "
                             "account may not perform it. Indexing, registration and scanning are "
                             "operator operations performed as the service account; see "
                             "docs/security/A7_1_OPERATIONS.md.",
                             st->command, st->operand_count > 0 ? " " : "",
                             st->operand_count > 0 ? st->operands[0] : "");
    }
    /* Deliberately says "is not served" rather than "has no daemon-served form
     * yet". The second wording asserts that the name exists, and a mistyped
     * subcommand reaches here too — `atlas code sem-symbol`, which has never
     * been a command, was told the system index is daemon-owned and that its
     * command is merely unavailable. Both halves were false, and the reader's
     * next move is to go looking for a feature nobody has removed.
     *
     * The honest answer covers both cases without a second copy of the command
     * inventory. A list here would be a fourth one — after `COMMANDS[]`,
     * `remote_serves` and the dispatch chain — and its drift would turn a
     * working command into "unknown" on every system deployment, which is a
     * worse fault than an imprecise sentence about a typo. `atlas help` is the
     * one authority, so the message points at it. */
    return atlas_err_set(err, ATLAS_ERR_CONFIG,
                         "the system index is owned by the Atlas service account and cannot be "
                         "read directly, and `atlas %s%s%s` is not served over the socket — it is "
                         "either a write operation or not a command. Every read-only command is "
                         "served; `backup restore` and `maintenance` are local operator "
                         "operations with no RPC surface by design. Run `atlas help` for the "
                         "command list.",
                         st->command, st->operand_count > 0 ? " " : "",
                         st->operand_count > 0 ? st->operands[0] : "");
}
