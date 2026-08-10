/* Atlas - the system-deployment policy: who may speak to a shared daemon.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * ## What problem this solves
 *
 * A0–A7 assume one Atlas per user: the daemon runs as the person, the index
 * lives in their home directory, and `atlas_ipc_accept` refuses any peer whose
 * uid is not the daemon's own. That is exactly right for a per-user install and
 * it is why A7 could say the socket carries no authority — every peer on it was
 * already the daemon's own uid, so there was nothing to distinguish.
 *
 * A7.1 separates the principals. The daemon runs as `atlasd`, which owns the
 * database and nothing else does; the model runs as `atlas-worker`, which owns
 * nothing. For that to be useful the daemon has to serve a uid that is not its
 * own — and the moment it does, "which uid may connect?" becomes a real
 * question with a wrong answer.
 *
 * ## The answer, and why it is safe
 *
 * The allowlist is a **root-owned file**, `/etc/atlas/system.conf`, reached
 * through `atlas_rootpath_open` — from `/`, no symlink traversed, every
 * component owned by uid 0 and writable by nobody else. Neither `atlasd` nor
 * `atlas-worker` can create, replace or edit it, and neither can any other
 * unprivileged local account. A caller that could choose who is allowed to
 * connect would not be constrained by the list.
 *
 * Root, and the deployment operator who holds root, are **trusted by design**
 * and are not what this defends against — see `docs/security/A7_1_THREAT_MODEL.md`.
 * The principal it binds is `atlas-worker`: the untrusted account every
 * persistent or autonomous model process runs as.
 *
 * The uid it is checked against comes from `SO_PEERCRED`, which the kernel fills
 * in at connect time from the peer's real credentials. It is not read from the
 * request JSON, not from an environment variable the client set, not from
 * `/proc/<pid>`, and not from anything the peer can influence. A client that
 * sends `{"uid":0}` is describing itself to a field Atlas does not have.
 *
 * ## What it does not do
 *
 * Being on the allowlist buys exactly one thing: the connection is accepted.
 * Every method the connection may then call is the A7 model-safe surface —
 * reads, proposals and session bookkeeping — because the lifecycle, registry,
 * backup, restore and maintenance methods **do not exist in the protocol**.
 * There is no per-uid method table, no role, no capability tier, and there must
 * never be one: a privileged client over this socket would put back exactly the
 * authority A7 removed. `docs/security/A7_1_OPERATIONS.md` says so, and
 * `tests/test_a71_syspolicy.c` asserts it from a second uid.
 *
 * ## Fail-closed
 *
 * Zero is "no system policy", so a zeroed struct serves nobody but the daemon's
 * own uid. A policy that is missing, unreadable, malformed, symlinked, or owned
 * or writable by anyone but root leaves the daemon in legacy per-user mode,
 * where the only acceptable peer is its own uid. Degrading to *fewer* permitted
 * peers is the only safe direction, and it is the only one implemented.
 */
#ifndef ATLAS_SYSPOLICY_H
#define ATLAS_SYSPOLICY_H

#include <stdbool.h>
#include <stddef.h>

/* Compiled-in, absolute, and with no environment override or command-line flag
 * on purpose — the A7 rule about `ATLAS_AUTHORITY_POLICY_PATH`, for the same
 * reason. A caller that can point Atlas at a different policy has written the
 * policy. */
#define ATLAS_SYSPOLICY_PATH "/etc/atlas/system.conf"

/* Small on purpose. This is an operator-maintained list of service accounts,
 * not a user directory; a deployment needing more than this has outgrown the
 * design rather than the constant. */
#define ATLAS_SYSPOLICY_MAX_CLIENTS 32

typedef enum atlas_syspolicy_state {
    /* Zero is legacy per-user mode: serve this uid and nobody else. */
    ATLAS_SYSPOLICY_LEGACY = 0,
    ATLAS_SYSPOLICY_SYSTEM = 1
} atlas_syspolicy_state;

typedef enum atlas_syspolicy_reason {
    ATLAS_SYSPOLICY_REASON_UNKNOWN = 0,
    /* No policy file. The ordinary state of a per-user install. */
    ATLAS_SYSPOLICY_REASON_ABSENT,
    /* A component of the path is a symlink, or the path is malformed. */
    ATLAS_SYSPOLICY_REASON_PATH_UNSAFE,
    /* The policy, or a directory leading to it, is owned by or writable by
     * somebody other than root. */
    ATLAS_SYSPOLICY_REASON_WRITABLE,
    /* Present, root-owned, and not something Atlas can read as a policy. */
    ATLAS_SYSPOLICY_REASON_MALFORMED,
    ATLAS_SYSPOLICY_REASON_ACTIVE
} atlas_syspolicy_reason;

typedef struct atlas_syspolicy {
    atlas_syspolicy_state state;
    atlas_syspolicy_reason reason;
    /* Where the shared daemon listens, and which index it owns. Both come from
     * the root-owned file rather than from the environment, so a client cannot
     * point the daemon at another database and the daemon cannot be talked into
     * answering on another socket. */
    char socket_path[108];
    char data_dir[256];
    /* The group the socket is handed to. Recorded for reporting and for the
     * startup check; the kernel enforces access, not this string. */
    char client_group[64];
    /* Peers permitted in addition to the daemon's own uid. */
    long long client_uids[ATLAS_SYSPOLICY_MAX_CLIENTS];
    size_t client_count;
    char detail[256];
} atlas_syspolicy;

const char *atlas_syspolicy_reason_name(atlas_syspolicy_reason r);
const char *atlas_syspolicy_reason_explain(atlas_syspolicy_reason r);

/* Reads the compiled-in policy path. Never fails: anything other than a
 * complete, root-anchored, well-formed policy is legacy mode with a reason. */
void atlas_syspolicy_load(atlas_syspolicy *out);

/* The same loader against an explicit path, so the decision procedure can be
 * tested against real filesystem shapes rather than against a description of
 * them.
 *
 * Not a bypass, and not reachable from the CLI, IPC, MCP or a hook: nothing
 * parses a path into it, and every caller in the shipped binary passes the
 * compiled-in constant. What it takes is *which* file to inspect; what it
 * enforces about that file is not a parameter, and an unprivileged uid cannot
 * satisfy those requirements anywhere on the filesystem. */
void atlas_syspolicy_load_at(const char *path, atlas_syspolicy *out);

/* True when `uid` may open a connection. Always false in legacy mode: the
 * daemon's own uid is checked separately by the caller, so this function can
 * never be the only thing standing between a stranger and the socket. */
bool atlas_syspolicy_permits(const atlas_syspolicy *p, long long uid);

#endif /* ATLAS_SYSPOLICY_H */
