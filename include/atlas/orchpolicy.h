/* Atlas - A8: the root-owned orchestration policy.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * ## What this decides
 *
 * Whether Atlas runs jobs at all, and if so: which registered repositories may
 * be snapshotted, which modes and drivers exist, who may submit and who may
 * dispatch, and what the ceilings on time, attempts, output and artifacts are.
 *
 * ## Why it is a root-owned file and not a table
 *
 * The principal it constrains is `atlas-worker` — the untrusted account every
 * persistent model process runs as — and, less sharply, whichever uid may
 * submit. Neither of those can create, replace or edit `/etc/atlas/orchestration.conf`:
 * it is reached through `atlas_rootpath_open`, from `/`, with no symlink
 * traversed and every component owned by uid 0 and writable by nobody else.
 *
 * Storing it in the index instead would put it behind `atlasd`, which is better
 * than nothing and worse than root: `atlasd` is the account a bug in the daemon
 * runs as, and a policy the constrained process's own service can rewrite is a
 * policy it can widen. This is the same argument A7 makes for the authority
 * policy and A7.1 for the system policy, and it produces the same answer.
 *
 * ## Fail-closed at zero
 *
 * `ATLAS_ORCHPOLICY_DISABLED` is zero, so a zeroed struct runs no jobs, permits
 * no submitter, has no dispatcher and allows no repository. A policy that is
 * missing, unreadable, malformed, symlinked, or owned or writable by anyone but
 * root leaves orchestration off with a reason. There is no direction in which a
 * degraded policy permits more.
 *
 * An **unrecognised key is an error**, not something skipped — A7.1's rule, for
 * A7.1's reason: a policy Atlas half-understands is one whose author believes
 * they configured something Atlas never read, and one day that something will be
 * a restriction.
 *
 * ## What it cannot do
 *
 * It cannot grant lifecycle authority, and there is no key that would. A8's
 * dispatcher tier is a *disjoint* set of orchestration methods, not a privileged
 * one: every method it adds concerns leases, heartbeats, events and results for
 * jobs an operator already created, and none of them can approve a decision,
 * change the registry, read a backup or write the index outside the
 * orchestration tables. `docs/security/A8_THREAT_MODEL.md` states that in full
 * and `tests/test_orch_rpc.c` asserts it from both sides.
 */
#ifndef ATLAS_ORCHPOLICY_H
#define ATLAS_ORCHPOLICY_H

#include <stdbool.h>
#include <stddef.h>

#include "atlas/orch.h"

/* Compiled-in, absolute, with no environment override and no flag — the rule
 * `ATLAS_AUTHORITY_POLICY_PATH` and `ATLAS_SYSPOLICY_PATH` follow, for the same
 * reason. A caller that can choose the policy has written the policy. */
#define ATLAS_ORCHPOLICY_PATH "/etc/atlas/orchestration.conf"

#define ATLAS_ORCHPOLICY_MAX_REPOS 16
#define ATLAS_ORCHPOLICY_MAX_MODES 8
#define ATLAS_ORCHPOLICY_MAX_DRIVERS 8
#define ATLAS_ORCHPOLICY_MAX_SUBMITTERS 16

typedef enum atlas_orchpolicy_state {
    /* Zero: no orchestration. Submitting a job is refused, no lease is ever
     * granted, and the dispatcher has nothing to ask for. */
    ATLAS_ORCHPOLICY_DISABLED = 0,
    ATLAS_ORCHPOLICY_ENABLED = 1
} atlas_orchpolicy_state;

typedef enum atlas_orchpolicy_reason {
    ATLAS_ORCHPOLICY_REASON_UNKNOWN = 0,
    ATLAS_ORCHPOLICY_REASON_ABSENT,
    ATLAS_ORCHPOLICY_REASON_PATH_UNSAFE,
    ATLAS_ORCHPOLICY_REASON_WRITABLE,
    ATLAS_ORCHPOLICY_REASON_MALFORMED,
    ATLAS_ORCHPOLICY_REASON_ACTIVE
} atlas_orchpolicy_reason;

typedef struct atlas_orchpolicy {
    atlas_orchpolicy_state state;
    atlas_orchpolicy_reason reason;

    /* The uid the dispatcher must be running as. Compared against SO_PEERCRED
     * and nothing else. Zero means no dispatcher is configured, and then no
     * connection can ever reach a dispatcher method. */
    long long dispatcher_uid;

    /* Who may create, cancel and read jobs. A submitter is still subject to the
     * A7.1 client allowlist first: this list is narrower, never wider. */
    long long submitter_uids[ATLAS_ORCHPOLICY_MAX_SUBMITTERS];
    size_t submitter_count;

    /* Registered repository names that may be snapshotted. A name, not a path:
     * the daemon resolves it through the registry, so a repository that is not
     * registered cannot be named at all, and a path never enters the decision. */
    char repos[ATLAS_ORCHPOLICY_MAX_REPOS][ATLAS_ORCH_NAME_MAX + 1u];
    size_t repo_count;

    char modes[ATLAS_ORCHPOLICY_MAX_MODES][ATLAS_ORCH_NAME_MAX + 1u];
    size_t mode_count;

    char drivers[ATLAS_ORCHPOLICY_MAX_DRIVERS][ATLAS_ORCH_NAME_MAX + 1u];
    size_t driver_count;

    /* Ceilings. Each may only lower the compiled-in absolute bound in
     * `atlas/orch.h`, never raise it: the policy decides how much a submitter
     * may ask for, and the header decides how much the policy may permit. */
    long long max_wall_timeout_ms;
    long long max_idle_timeout_ms;
    long long max_attempts;
    long long max_output_bytes;
    long long max_artifact_bytes;
    long long max_artifact_count;

    /* Where the dispatcher owns its workspaces. Root-declared so that a worker
     * cannot choose where it writes, and checked by the dispatcher against the
     * directory it actually opens. */
    char worker_root[256];

    /* Whether a driver that calls a live model may run at all. Off by default
     * and off when the key is absent: enabling live model execution is a
     * deliberate operator act, not the consequence of installing a policy. */
    bool live_model;

    /* --- A8.1: the operator's own model dispatcher --------------------------
     *
     * A second dispatcher uid, permitted to lease **only** jobs whose driver
     * needs a live model, and expected to be the operator's own account.
     *
     * This is a deliberate, operator-configured relaxation of the A7.1/A8 rule
     * that every model process runs as `atlas-worker`, and it should be read as
     * exactly that rather than as a loophole. Claude Code authenticates with a
     * session that lives in a person's home directory; there is no service
     * credential on this machine, and Atlas must not copy, read or relocate a
     * personal one. The only way to use that session is to run the process as
     * the person who owns it.
     *
     * What it costs is stated plainly in `docs/orchestration.md`: a job run by
     * this dispatcher has the operator's own filesystem authority, not
     * `atlas-worker`'s. The A8 job record, lease, bounds, snapshot and audit
     * trail are unchanged — what changes is the OS principal the driver runs
     * as, and only for drivers that need a model.
     *
     * Zero means no model dispatcher, which is the default and leaves A8
     * exactly as it was. */
    long long model_dispatcher_uid;
    /* Where that dispatcher owns its workspaces. Must be writable by the model
     * dispatcher uid rather than by `atlas-worker`. */
    char model_worker_root[256];
    /* `operator_session`: the model driver uses the dispatcher's own logged-in
     * session — its HOME, its existing credentials — and Atlas never reads,
     * copies or stores any of it. `service` (the default) requires the
     * root-installed credential file and refuses without it. */
    bool model_uses_operator_session;

    char detail[256];
} atlas_orchpolicy;

const char *atlas_orchpolicy_reason_name(atlas_orchpolicy_reason r);
const char *atlas_orchpolicy_reason_explain(atlas_orchpolicy_reason r);

/* Reads the compiled-in policy path. Never fails: anything other than a
 * complete, root-anchored, well-formed policy is DISABLED with a reason. */
void atlas_orchpolicy_load(atlas_orchpolicy *out);

/* The same loader against an explicit path, so the decision procedure can be
 * tested against real filesystem shapes rather than a description of them. Not
 * a bypass: what it takes is *which* file to inspect, and what it enforces about
 * that file is not a parameter. Every caller in the shipped binary passes the
 * compiled-in constant. */
void atlas_orchpolicy_load_at(const char *path, atlas_orchpolicy *out);

/* Membership questions. All false when the policy is disabled. */
bool atlas_orchpolicy_permits_submitter(const atlas_orchpolicy *p, long long uid);
bool atlas_orchpolicy_is_dispatcher(const atlas_orchpolicy *p, long long uid);
/* True for the configured model dispatcher uid. Always false when none is
 * configured, which is the default. */
bool atlas_orchpolicy_is_model_dispatcher(const atlas_orchpolicy *p, long long uid);
/* True when `uid` may reach the dispatcher method group at all — either
 * dispatcher. Which *jobs* it may lease is a separate question, answered by the
 * driver filter on the lease request. */
bool atlas_orchpolicy_is_any_dispatcher(const atlas_orchpolicy *p, long long uid);
bool atlas_orchpolicy_permits_repo(const atlas_orchpolicy *p, const char *name);
bool atlas_orchpolicy_permits_mode(const atlas_orchpolicy *p, const char *name);
bool atlas_orchpolicy_permits_driver(const atlas_orchpolicy *p, const char *name);

/* Applies the policy's ceilings to a specification whose numeric fields may be
 * zero meaning "not given". Zero takes the policy's ceiling as the default; a
 * value above the ceiling is refused with the ceiling named, never clamped. */
atlas_status atlas_orchpolicy_apply_limits(const atlas_orchpolicy *p, atlas_orch_spec *s,
                                           atlas_err *err);

#endif /* ATLAS_ORCHPOLICY_H */
