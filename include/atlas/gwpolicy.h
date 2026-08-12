/* Atlas - A9: the root-owned gateway policy.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * ## What this decides
 *
 * Whether Atlas is reachable from off this machine at all, and if so: on which
 * address and port, which browser origins may talk to it, which of the two
 * surfaces (remote MCP, web GUI) exist, what the ceilings on request size,
 * concurrency and rate are, and which uid the gateway process runs as.
 *
 * ## Why it is a root-owned file and not a table or a flag
 *
 * The principal it constrains is the gateway itself — a process that terminates
 * connections from the Internet and is therefore the most likely thing in Atlas
 * to be compromised. A gateway that could widen its own exposure is not
 * constrained by anything. `/etc/atlas/gateway.conf` is reached through
 * `atlas_rootpath_open`, from `/`, with no symlink traversed and every component
 * owned by uid 0 and writable by nobody else.
 *
 * This is the argument A7 makes for the authority policy, A7.1 for the system
 * policy and A8 for the orchestration policy, and it produces the same answer
 * for the same reason.
 *
 * ## Fail-closed at zero
 *
 * `ATLAS_GWPOLICY_DISABLED` is zero, so a zeroed struct listens on nothing,
 * serves neither surface, allows no origin and has no gateway uid. A policy that
 * is missing, unreadable, malformed, symlinked, or owned or writable by anyone
 * but root leaves the gateway off with a reason. **There is no direction in
 * which a degraded policy exposes more.**
 *
 * An **unrecognised key is an error**, not something skipped — A7.1's rule, for
 * A7.1's reason: a policy Atlas half-understands is one whose author believes
 * they configured something Atlas never read, and here that something is very
 * likely to have been a restriction.
 *
 * ## The default is loopback, and binding wider is a second deliberate act
 *
 * With no `listen_addr` the gateway binds `127.0.0.1`. A9 requires that a
 * default deployment never silently exposes a service on every interface, and
 * the way that is guaranteed here is that the wider bind needs both an explicit
 * address *and* an explicit statement of how TLS is terminated. A gateway asked
 * to bind a non-loopback address without `tls_mode` refuses to start.
 *
 * ## TLS is terminated in front, and that is a stated limitation
 *
 * Atlas has one hard rule about dependencies: nothing is downloaded, and a new
 * third-party library needs an upstream tag, digests and a licence entry. An
 * in-process TLS stack would be exactly that, so A9 does not have one. Instead
 * `tls_mode = REVERSE_PROXY` says that something in front — nginx, Caddy, a load
 * balancer — terminates TLS and forwards over loopback, and the operator is
 * responsible for that being true.
 *
 * Do not describe A9 as providing TLS. It provides a service designed to sit
 * behind it, and `docs/remote-access.md` says so in those words.
 */
#ifndef ATLAS_GWPOLICY_H
#define ATLAS_GWPOLICY_H

#include <stdbool.h>
#include <stddef.h>

/* Compiled-in, absolute, with no environment override and no flag — the rule
 * every other Atlas policy path follows. A caller that can choose the policy has
 * written the policy. */
#define ATLAS_GWPOLICY_PATH "/etc/atlas/gateway.conf"

#define ATLAS_GWPOLICY_MAX_ORIGINS 8
#define ATLAS_GWPOLICY_ORIGIN_MAX 128u
#define ATLAS_GWPOLICY_ADDR_MAX 64u
#define ATLAS_GWPOLICY_URL_MAX 256u

typedef enum atlas_gwpolicy_state {
    /* Zero: no gateway. Nothing binds, nothing listens, and `atlas gateway run`
     * reports why and exits. */
    ATLAS_GWPOLICY_DISABLED = 0,
    ATLAS_GWPOLICY_ENABLED = 1
} atlas_gwpolicy_state;

typedef enum atlas_gwpolicy_reason {
    ATLAS_GWPOLICY_REASON_UNKNOWN = 0,
    ATLAS_GWPOLICY_REASON_ABSENT,
    ATLAS_GWPOLICY_REASON_PATH_UNSAFE,
    ATLAS_GWPOLICY_REASON_WRITABLE,
    ATLAS_GWPOLICY_REASON_MALFORMED,
    /* A complete, valid policy that says no.
     *
     * Distinct from ABSENT because they call for different actions: an operator
     * who installed a policy and switched it off should not be told there is no
     * policy, and one who never installed one should not be sent looking for a
     * switch. */
    ATLAS_GWPOLICY_REASON_DISABLED,
    ATLAS_GWPOLICY_REASON_ACTIVE
} atlas_gwpolicy_reason;

const char *atlas_gwpolicy_state_name(atlas_gwpolicy_state s);
const char *atlas_gwpolicy_reason_name(atlas_gwpolicy_reason r);
/* One sentence saying what would change this reason. A refusal nobody can act
 * on gets worked around. */
const char *atlas_gwpolicy_reason_detail(atlas_gwpolicy_reason r);

typedef enum atlas_gwpolicy_tls {
    /* Zero. Only a loopback bind is permitted, because nothing has said how
     * transport security is provided. */
    ATLAS_GWPOLICY_TLS_UNSET = 0,
    /* Something in front terminates TLS and forwards over loopback. Atlas does
     * not verify this and cannot; it is the operator's statement. */
    ATLAS_GWPOLICY_TLS_REVERSE_PROXY,
    /* The operator states that this deployment is on a trusted network segment
     * and wants no transport security. Explicit so that it appears in the
     * policy an auditor reads, rather than being the silent consequence of
     * leaving a key out. */
    ATLAS_GWPOLICY_TLS_NONE
} atlas_gwpolicy_tls;

const char *atlas_gwpolicy_tls_name(atlas_gwpolicy_tls t);

typedef struct atlas_gwpolicy {
    atlas_gwpolicy_state state;
    atlas_gwpolicy_reason reason;
    /* The component that failed, when one did. Never a caller-supplied string
     * beyond the compiled-in path. */
    char detail[256];

    /* Defaults to 127.0.0.1. A non-loopback value requires `tls_mode`. */
    char listen_addr[ATLAS_GWPOLICY_ADDR_MAX];
    int listen_port;

    /* How the endpoint is reached from outside, if it is. Used only to print
     * connection instructions; nothing routes on it. */
    char public_url[ATLAS_GWPOLICY_URL_MAX];

    atlas_gwpolicy_tls tls_mode;

    /* The uid the gateway process runs as.
     *
     * This is what the daemon compares against `SO_PEERCRED` before offering
     * the `gateway.` method group. Zero means no gateway may reach it, which is
     * the fail-closed default: the group is hidden exactly as the dispatcher
     * group is, so a peer that is not the gateway gets `unknown method` rather
     * than a refusal that would tell it what to try next. */
    long long gateway_uid;

    /* Browser origins that may call the API. Exact strings, compared whole,
     * scheme and port included. An empty list means no cross-origin browser
     * request is accepted at all — which is correct for a GUI served from the
     * same origin, and is the default. */
    char origins[ATLAS_GWPOLICY_MAX_ORIGINS][ATLAS_GWPOLICY_ORIGIN_MAX];
    size_t origin_count;

    /* Which surfaces exist. Both default off: enabling the gateway is one
     * decision and exposing a particular surface on it is another. */
    bool remote_mcp;
    bool web_gui;

    /* Ceilings. Each may only lower the compiled-in absolute bound in
     * `atlas/limits.h`, never raise it — A8's rule, so the policy decides how
     * much a deployment permits and the header decides how much the policy may
     * permit. */
    long long max_request_bytes;
    long long max_concurrent;
    long long rate_limit_per_minute;
    long long session_ttl_seconds;

    /* Whether a forwarded client address may be believed for rate limiting.
     *
     * Off by default, and the consequence is stated rather than hidden: behind
     * a reverse proxy every request appears to come from the proxy, so per-peer
     * rate limiting degrades to a global one. Believing a header by default
     * would be worse — an attacker would simply vary it, and the limit would
     * become unenforceable while continuing to look enforced. */
    bool trust_forwarded_for;
} atlas_gwpolicy;

/* Loads the compiled-in policy path. Never fails: an unreadable, malformed or
 * unsafe policy is a DISABLED result with a reason, because a gateway that
 * refuses to start is the safe outcome and the caller must not have to
 * distinguish "could not load" from "loaded and says no". */
void atlas_gwpolicy_load(atlas_gwpolicy *out);

/* The same, from an explicit path. **Tests only**, and the compiled-in entry
 * point above is what production uses — for the reason
 * `ATLAS_GWPOLICY_PATH` is compiled in at all. The root-ownership walk still
 * applies, so a test fixture under a user-owned directory loads as DISABLED,
 * which is what the malformed-matrix cases assert. */
void atlas_gwpolicy_load_at(const char *path, atlas_gwpolicy *out);

/* Parses policy bytes whose provenance somebody else established.
 *
 * Split out so the whole key matrix is testable. The root-ownership walk is what
 * makes a policy trustworthy and it can only succeed for a genuinely root-owned
 * file, so a test forced through it could exercise exactly one outcome:
 * refusal. Every malformed case — an unknown key, a ceiling above the
 * compiled-in bound, a non-loopback bind with no TLS stance, a wildcard origin —
 * would then be unreachable from the suite, which is the opposite of what those
 * refusals deserve.
 *
 * This establishes **nothing** about where the bytes came from. Production
 * reaches it only through `atlas_gwpolicy_load_at`. */
void atlas_gwpolicy_parse_buffer(const char *buf, size_t len, atlas_gwpolicy *out);

/* True when `addr` is a loopback literal. Exposed because the "a wider bind
 * needs an explicit TLS stance" rule is a refusal, and a refusal is what is
 * easiest to get wrong quietly. */
bool atlas_gwpolicy_is_loopback(const char *addr);

#endif /* ATLAS_GWPOLICY_H */
