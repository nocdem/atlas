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
#include <stdint.h>

#include "atlas/apikey.h"
#include "atlas/orch.h"

/* Compiled-in, absolute, with no environment override and no flag — the rule
 * every other Atlas policy path follows. A caller that can choose the policy has
 * written the policy. */
#define ATLAS_GWPOLICY_PATH "/etc/atlas/gateway.conf"

#define ATLAS_GWPOLICY_MAX_ORIGINS 8
/* A14. At most four `remote_submit_key` lines; bounded in the parser,
 * reflected in atlas_gwpolicy.remote_submit_keys[]. */
#define ATLAS_GWPOLICY_MAX_SUBMIT_KEYS 4
/* A14. Each `remote_submit_gate` line: one printable-ASCII gate expression,
 * at most 255 chars plus NUL. */
#define ATLAS_GWPOLICY_GATE_LINE_MAX 256u
/* A14. Absolute ceilings: the policy may not permit more than this many
 * simultaneous remote submissions or this many per calendar day. */
#define ATLAS_GWPOLICY_SUBMIT_MAX_ACTIVE_CEILING 8
#define ATLAS_GWPOLICY_SUBMIT_MAX_PER_DAY_CEILING 64
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

    /* Scopes granted to a request against `/api/` that presents no *live*
     * principal, when `web_gui = yes`: no bearer token at all, and either no
     * session cookie or one that does not resolve to a live session (expired,
     * forged, or simply stale because a gateway restart forgot every
     * in-memory session — `gateway.c:573-577`, deliberately). A bearer token
     * that *was* presented and failed authentication is not covered by this —
     * it stays refused, because it carries a selector the audit trail can
     * name, and falling through would spend that signal on a request that
     * already failed once. See `anonymous_ok` in `gateway.c` for the full
     * reasoning and the case this key exists to help: a browser holding a
     * cookie from before the gateway's last restart lands on this floor
     * instead of a hard 401 that only a manual logout clears.
     *
     * This is a deliberate, operator-chosen widening of the threat model: it
     * makes every read those scopes cover available to anyone who can reach
     * the listener, with no credential at all. Zero — the default, and what a
     * policy naming no key at all produces — is today's behaviour exactly:
     * `/api/` still answers 401 to a request with no live principal. A
     * session or a bearer token that *does* authenticate is never masked down
     * to this: the mask only ever fills a gap where there was no principal,
     * never narrows one that exists.
     *
     * Every bit must satisfy `atlas_apikey_scope_grantable` — `memory:write`
     * is the standing example of a scope that does not, and an operator
     * cannot make it grantable by naming it here, any more than
     * `atlas api-key create` can. An unrecognised or ungrantable name makes
     * the whole policy MALFORMED, P0's rule: out of range is malformed, never
     * clamped.
     *
     * Naming this key while `web_gui = no` is MALFORMED too, not merely
     * inert: `/api/` is reachable independently of `web_gui` (a bearer token
     * reaches it whether or not the browser surface is on), so a key that
     * silently did nothing there would be exactly the shape P0's rule warns
     * about — a documented behaviour that is not the implemented one. Refusing
     * it means a policy an operator can still read back is one whose written
     * behaviour and actual behaviour agree. */
    atlas_scope_mask web_gui_anonymous_scopes;

    /* A16. The disposal credential and the record kinds it may dispose of from
     * the browser — the mechanism that makes "Dispose from this browser" exist
     * on a deployment at all. This is where A7/A7.1's rule lands a second
     * time: the gateway process is the principal a browser disposal reaches
     * first, and it cannot edit this file, so naming the credential and the
     * kinds here — rather than in a table the gateway's own uid could reach —
     * is what keeps a compromised gateway from being able to widen what it
     * may dispose of.
     *
     * Empty `remote_dispose_key` (the default) means remote disposal is off
     * entirely: no daemon method group is offered, and there is no narrower
     * grant to fall back to. A default that granted a *subset* of kinds
     * instead of nothing would be worse than granting nothing, because an
     * operator would only discover the narrowing the day a record of the
     * missing kind refused them — so the two fields travel together, and a
     * policy naming one without the other is MALFORMED, not a smaller grant.
     *
     * `remote_dispose_key` is stored **without** the display prefix, exactly
     * as `api_keys.key_id` is (`ATLAS_APIKEY_ID_PREFIX`, `atlas/gw.h` — not
     * included here on purpose, see gwpolicy.c). `remote_dispose_kinds` is a
     * bitmask built from `ATLAS_DECISION_KIND_BIT`, over any subset of
     * `atlas_decision_kind` the operator names — the loader places no
     * narrower vocabulary in front of the operator's own choice of which
     * kinds may be disposed of from a browser than `atlas_decision_kind_parse`
     * already accepts everywhere else.
     *
     * **Load time verifies shape only.** This loader has no database handle
     * and runs inside the gateway process too, which under A7.1 cannot open
     * the index at all — so a policy naming a credential that does not
     * exist, is revoked, or holds a nonempty stored scope list still loads
     * ENABLED, and `atlas gateway status` still prints it: the status line
     * is the policy's claim, not the credential's liveness. Existence,
     * status, `scopes_unreadable`, the verifier match, the empty-stored-scope
     * requirement and identity against this field are all checked *at use*,
     * inside the write transaction, by `atlas_decision_remote_verify`
     * (`src/decision/remote.c`), across **three** distinct outward sentences
     * — see the `remote_dispose_key` branch in `gwpolicy.c` for which check
     * produces which. */
    char remote_dispose_key[ATLAS_APIKEY_SELECTOR_HEX + 1u];
    uint32_t remote_dispose_kinds;

    /* A16, amended 2026-09-04. The operator's written acceptance that the
     * disposal credential above crosses the network unencrypted. This is a
     * recorded decision, not a feature toggle: the operator was shown the
     * chain — no in-process TLS, `tls_mode = NONE` on this deployment, a
     * bearer credential presented on every disposal request, an Atlas
     * credential with no expiry — and declined TLS for their own network on
     * 2026-09-04 anyway. Never a default (a memset leaves it false); refused
     * under `tls_mode = REVERSE_PROXY`, where there is nothing to accept; and
     * refused without both fields above, where there is nothing for it to
     * apply to. `atlas gateway status` prints it on every run — an
     * authentication-adjacent fact an auditor must see there, not a ceiling
     * safe to leave to a root-owned file they may not be able to open. */
    bool cleartext_disposal_accepted;

    /* A14. The submission credentials and the job specification they imply.
     * Everything a submitted job IS — driver, mode, gates, attempts, active
     * ceiling, per-day ceiling — is here; nothing about a submitted job's
     * shape is decided by the request. The gateway verifies the bearer against
     * one of these ids and copies all seven lines onto the op.
     *
     * `remote_submit_count` = 0 (the default) means remote submission is off:
     * no daemon method group is offered. Like the disposal credential, the
     * group exists iff these fields are non-empty, and a credential not in
     * this list has no path to `jobs:submit`. */
    char remote_submit_keys[ATLAS_GWPOLICY_MAX_SUBMIT_KEYS][ATLAS_APIKEY_SELECTOR_HEX + 1u];
    size_t remote_submit_count;                              /* 0 = remote submission off */
    char remote_submit_driver[ATLAS_ORCH_NAME_MAX + 1u];
    char remote_submit_mode[ATLAS_ORCH_NAME_MAX + 1u];
    char remote_submit_gates[ATLAS_ORCH_MAX_VALIDATIONS][ATLAS_GWPOLICY_GATE_LINE_MAX];
    size_t remote_submit_gate_count;
    long long remote_submit_max_attempts;
    long long remote_submit_max_active;
    long long remote_submit_max_per_day;
    /* True only when the policy carries `operator_accepts_cleartext_submission = yes`.
     * Never a default; refused under REVERSE_PROXY; refused without a submit key; and
     * never implied by `cleartext_disposal_accepted`, for the reason in §The decision
     * on cleartext in the A14 plan. */
    bool cleartext_submission_accepted;

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
