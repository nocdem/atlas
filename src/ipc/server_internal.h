/* Atlas - shared state between the IPC serve loop and its method groups.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Not a public header.
 *
 * A1 had eight methods and one file. A2 adds a group of them — sessions, change
 * sets, reasons, decisions and the context envelope — and putting those in the
 * same file as the serve loop would bury the loop under them. The dispatch
 * table is still one table, assembled in server.c from the groups, so there is
 * no second dispatcher to drift from the first.
 */
#ifndef ATLAS_IPC_SERVER_INTERNAL_H
#define ATLAS_IPC_SERVER_INTERNAL_H

#include "atlas/db.h"
#include "atlas/error.h"
#include "atlas/ipc.h"
#include "atlas/json.h"
#include "atlas/safetext.h"
#include "daemon/daemon_internal.h"

/* What one method invocation has to work with.
 *
 * `db` is a read-only handle opened for this request and closed after it, so
 * every request observes a committed snapshot taken at its own start and no
 * reader accumulates an open transaction that pins the WAL. A method that needs
 * to write hands the work to the writer thread; it never gets a writable
 * handle, because there is exactly one of those and the writer owns it. */
typedef struct dispatch_state {
    atlas_server_ctx *ctx;
    atlas_db *db; /* read-only */
    atlas_json *j;
    atlas_safe_pool safe;
    /* A8. The kernel's answer about the peer, taken from SO_PEERCRED at accept
     * time and carried here unchanged.
     *
     * A7.1 says the socket carries no *authority*, and that is now narrower
     * than it was: the registry, restore and maintenance methods still do not
     * exist in the protocol, and the lifecycle and backup-read methods exist
     * only in a group gated on the operator uid the root-owned policy names.
     * Nothing an ordinary client can reach replaces or prunes the index. What
     * A8 adds is a *disjoint* group of orchestration methods
     * reachable only from the single uid a root-owned policy names as the
     * dispatcher — leases, heartbeats, events and results for jobs an operator
     * already created. It confers no authority over the record Atlas protects,
     * it cannot approve a decision, change the registry or read a backup, and
     * being on that list is itself a root-owned configuration fact rather than
     * something a client can assert.
     *
     * The identity must come from here and nowhere else. A uid, gid, pid or
     * role in the request body is a client describing itself, which is not
     * evidence about itself; `tests/test_a71_syspolicy.c` and
     * `tests/test_orch_rpc.c` assert that from both directions. */
    int64_t peer_uid;
    int64_t peer_pid;
} dispatch_state;

typedef atlas_status (*atlas_method_fn)(dispatch_state *ds, const atlas_ipc_request *req,
                                        atlas_err *err);

typedef struct atlas_method_entry {
    const char *name;
    atlas_method_fn fn;
} atlas_method_entry;

/* The A2 method group: repository resolution and everything under `ai.`. */
const atlas_method_entry *atlas_server_ai_methods(size_t *count_out);
/* The A3 method group: everything under `code.`. Looked up through the same
 * dispatch as the others, in server.c. */
const atlas_method_entry *atlas_server_code_methods(size_t *count_out);
/* A8-CI: the four compiler-derived semantic reads. */
const atlas_method_entry *atlas_server_sem_methods(size_t *count_out);
/* The A4 method group: everything under `decision.`. Looked up through the same
 * dispatch as the others, in server.c. */
const atlas_method_entry *atlas_server_decision_methods(size_t *count_out);
/* A9.2.1: verification intake, evaluation and reads, all in the ordinary group.
 * Intake is a proposal rather than authority; nothing here approves, rejects,
 * supersedes, resolves, revalidates, or mints or spends a warrant. */
const atlas_method_entry *atlas_server_verify_methods(size_t *count_out);

/* The A8 method groups, and there are deliberately two of them.
 *
 * `job.` is the client surface: submit, read, list, cancel, and fetch an
 * artifact by its server-assigned id. Reachable from a uid the orchestration
 * policy lists as a submitter.
 *
 * `dispatch.` is the worker surface: lease, heartbeat, event, complete.
 * Reachable only from the single uid the policy names as the dispatcher, and
 * every one of them additionally requires a bearer lease token, so being the
 * right uid is necessary and not sufficient.
 *
 * The two sets are disjoint and the lookup is by peer uid from SO_PEERCRED, so
 * an ordinary client cannot forge a dispatcher message and a dispatcher cannot
 * create its own work. Neither set can reach lifecycle authority, the registry,
 * a backup or any table outside the eight `orch_*` ones. */
const atlas_method_entry *atlas_server_orch_client_methods(size_t *count_out);
const atlas_method_entry *atlas_server_orch_dispatch_methods(size_t *count_out);

/* Resolves the `repo` parameter with the CLI's error text. Shared because two
 * groups need it and two copies would answer differently. */
atlas_status atlas_server_require_repo(dispatch_state *ds, const atlas_ipc_request *req,
                                       atlas_repo_info *out, atlas_err *err);

/* Writes one repository's index state as a set of object members. Shared by
 * repo.list, repo.state and the A2 context builder so the three cannot describe
 * the same repository differently. */
atlas_status atlas_server_write_repo_state(dispatch_state *ds, const atlas_repo_info *ri,
                                           atlas_err *err);

/* True when this repository's index may be described as current, with the fixed
 * Atlas string explaining why not when it may not. `*reason_out` is NULL when it
 * is current. Computed in one place so no caller reconstructs it from flags. */
bool atlas_server_index_current(const atlas_index_state *s, const char **reason_out);

/* The five operator-channel methods, and the peer test that gates them. A7
 * deleted these; they are back in a disjoint group only the policy's operator
 * uid can reach. See the comment above `OPERATOR_METHODS` in
 * `src/ipc/server_decision.c` for what that does and does not guarantee. */
const atlas_method_entry *atlas_server_operator_methods(size_t *count_out);
bool atlas_server_peer_is_operator(long long peer_uid);

/* `backup.create` and `backup.verify`, in the same operator-gated group and
 * behind the same `SO_PEERCRED` test.
 *
 * A5 gave backup no RPC surface on the reasoning that the uid owning the index
 * can copy the file anyway. A7.1 broke that premise without noticing: under a
 * system deployment the index is 0700 `atlasd`, so the operator uid — the one
 * the root-owned policy names — was the one account that could not take a
 * backup at all. These two methods restore the operation to the account that is
 * supposed to have it, and no further: `backup.restore` deliberately has no RPC
 * form, because replacing the record should require stopping the daemon. */
const atlas_method_entry *atlas_server_backup_methods(size_t *count_out);

/* A9. The three questions the gateway may ask, in a group offered only to the
 * peer whose `SO_PEERCRED` uid equals the `gateway_uid` a root-owned policy
 * names. Disjoint from every other group and hidden the same way the dispatcher
 * group is: a name the peer does not hold answers `unknown method`, which is
 * what a name that does not exist gets.
 *
 * The gateway is neither an operator nor a dispatcher, so this group confers no
 * authority over the record Atlas protects. It cannot approve a decision,
 * change the registry, read a backup, run a job or build an index — and it holds
 * no credential-administration verb at all, because remote credential
 * administration does not exist in A9 rather than being refused. */
/* A9. Credential administration, in the **operator** group beside
 * `decision.approve` and `backup.create` — not in the gateway group, and not
 * reachable by any remote client. See src/ipc/server_apikey.c for why the local
 * CLI path alone was not enough: a running daemon holds the writer lock, so
 * revocation would have required stopping the service. */
const atlas_method_entry *atlas_server_apikey_methods(size_t *count_out);
/* The gate for that group. See src/ipc/server_apikey.c for why it is not simply
 * `atlas_server_peer_is_operator`: on an unseparated machine the daemon's own
 * uid owns the index outright, and refusing it there would relocate the verb,
 * protect nothing, and break revocation on every machine with a live daemon. */
bool atlas_server_peer_may_administer_credentials(dispatch_state *ds);

const atlas_method_entry *atlas_server_gateway_methods(size_t *count_out);
bool atlas_server_peer_is_gateway(const atlas_server_ctx *ctx, long long peer_uid);

#endif /* ATLAS_IPC_SERVER_INTERNAL_H */
