# Browser disposal: the remote operator channel, and what it is worth (A16)

A15 built a screen that reads a proposed knowledge record well — every
revision, its claims and their evidence, the gate's answer, and what it links
to — from wherever a browser reaches the gateway. It left the disposing of
that record exactly where A4 put it: an interactive terminal, on the Atlas
machine, as the operator's own uid. The operator built the screen so as not to
use the terminal, and said so the day A15 shipped. This season is the
correction, and it is a correction to a decision that was made and recorded,
not to a defect nobody noticed: A15's own plan left the choice of how far the
browser might go open, in writing, and asked the operator; the question died
in the document instead of reaching them.

The sentence this season exists for:

> **THE OPERATOR BUILT THE BROWSER SO AS NOT TO USE THE TERMINAL, AND A
> CHANNEL THAT IS WEAKER BY CONSTRUCTION IS STILL A CHANNEL — PROVIDED THE
> LEDGER SAYS WHICH ONE.**

## The honest paragraph, in the same breath as the capability

The remote operator channel is *weaker than the local channel by construction*,
and nothing in this season makes it stronger — only reachable from further
away. The local channel's whole worth was that the capability
never touched a network: a local process, the operator's own uid, `/dev/tty`,
a single-use token that lived for 120 seconds inside one machine. On this
channel the operator's disposal credential passes through the gateway
process — a network-facing process A9 designed to hold no authority — and
through whatever terminates TLS in front of it, and Atlas verifies neither. A
compromised gateway holds that credential for as long as a request carrying
it is in flight, and a holder of the credential disposes exactly as the
operator does. The ledger therefore records every such act as
`REMOTE_OPERATOR_CONFIRMED` with the credential's id beside it, never as
`LOCAL_OPERATOR_CONFIRMED`, so that a reader of any row ever written can still
tell the two apart.

**On this deployment the disposal credential travels in the clear.** The
gateway listens on `192.168.0.198:8799` with `tls_mode = NONE`, and the two
disposal routes carry the credential as a bearer header on every request, so
anyone able to observe traffic on that network segment can read it. An Atlas
API credential has no expiry, so a credential captured once disposes of
records exactly as the operator does until the operator notices and runs
`atlas api-key revoke`. The operator was shown this chain on 2026-09-04 and
accepted it for this network by writing
`operator_accepts_cleartext_disposal = yes` into the root-owned gateway
policy; `atlas gateway status` prints that acceptance on every run. Atlas
states this cost and does not judge the trade; the same key on a listener
reachable from a network the operator does not control is a different
decision using the same mechanism.

The operator's own words, quoted and dated, after being shown that TLS in
front of the gateway was the requirement's original, non-negotiable form and
what dropping it would cost: *"https ye gerek yok. kendi aginda sacmalamasin
zaten"* — no need for HTTPS, it is on my own network, 2026-09-04. That is a
decision about a machine and a network the operator controls, not a
recommendation Atlas is making; §The decision and §Decision 6 below carry the
chain that was put to them before they answered.

## 1. What is true today, measured against the tree at `d8e9e49`

**The write table.** The gateway's read-only route table, `API_ROUTES[]`, is
unmoved: still every row a `GET`, still forwarding only to the positive
allowlist `docs/review-surface.md` describes. This season adds a *second*
table, `API_WRITE_ROUTES[]` (`src/gw/gateway.c`), with exactly two rows, both
disposals, both `POST`, both requiring the single ungrantable scope:

```c
static const api_route API_WRITE_ROUTES[] = {
    {"/api/v1/decision/challenge", "decision.remote_challenge", ATLAS_SCOPE_DECISIONS_DISPOSE,
     {"repo", "decision", "revision", "intent", NULL}, {"revision", NULL}},
    {"/api/v1/decision/dispose", "decision.remote_dispose", ATLAS_SCOPE_DECISIONS_DISPOSE,
     {"repo", "decision", "intent", "challenge", "confirmation", NULL}, {NULL}},
};
```

`api_handle` still refuses every non-`GET` method for the read table it
guards, exactly as before; `api_handle_write` is a second handler, consulted
after the read table returns no match and before the 404, and it is the only
place in `src/gw` that ever parses a body as form syntax rather than a query
string. No route in either table shares a name with the other.

**The method group's four conditions.** The daemon offers
`decision.remote_challenge` and `decision.remote_dispose`
(`REMOTE_DISPOSAL_METHODS[]`, `src/ipc/server_remote.c`) to a peer only when
**all four** of the following hold, checked in
`atlas_server_remote_disposal_offered` and its policy half,
`atlas_server_remote_disposal_policy_ready`:

1. `ctx->gwpolicy.state == ATLAS_GWPOLICY_ENABLED` — a policy Atlas refused
   still leaves every field it had already parsed populated (`gwpolicy.c`'s
   one convention, applied to every key including these two), so a
   MALFORMED policy is not merely a smaller grant if this condition is
   missing, it is one that still satisfies every other test. This condition
   was added mid-season, after the other three were already written and
   before any of them shipped as the whole predicate — see Decision 9 below.
2. `atlas_server_peer_is_gateway(ctx, peer_uid)` — the ordinary A7.1 kernel
   fact, `SO_PEERCRED` against the root-owned `gateway_uid`.
3. `ctx->gwpolicy.remote_dispose_key[0] != '\0'` — the policy names a
   credential at all.
4. `ctx->gwpolicy.tls_mode == ATLAS_GWPOLICY_TLS_REVERSE_PROXY` **or**
   `ctx->gwpolicy.cleartext_disposal_accepted` — TLS in front is the shape
   the gate prefers; the acceptance key is this deployment's written,
   one-condition departure from it, never a default.

Every other peer, and this peer under any other policy, gets `unknown
method` — the answer a name that does not exist gets. Each method asks the
predicate again for itself, so reaching the *name* through dispatch is never
mistaken for being *allowed to use it*.

*(The plan this season executed described three conditions in its own
prose in one place and stated the fourth, correctly, in its frozen interface
comment; the code has always had four, from the point Decision 9's amendment
landed. This document states what `d8e9e49` actually contains.)*

**The policy's three keys.** `/etc/atlas/gateway.conf` gains:

- `remote_dispose_key = key_<16 lowercase hex>` — the disposal credential's
  id, stored without the `key_` prefix, exactly as `api_keys.key_id` is.
- `remote_dispose_kinds = KIND KIND …` — a space-separated, deduplicated list
  through `atlas_decision_kind_parse`; every one of the eight kinds
  (`DECISION`, `POLICY`, `INVARIANT`, `OPERATIONAL_FACT`, `ACCEPTED_RISK`,
  `OBLIGATION`, `PARKED`, `REJECTED_ALTERNATIVE`) is nameable, not a
  compiled-in subset, because the code that parses this list is the same
  parser every other kind-naming surface uses and narrowing it here would be
  a second, smaller vocabulary nobody asked for.
- `operator_accepts_cleartext_disposal = yes` — the third key, added by
  amendment on 2026-09-04 (§The decision), the operator's written acceptance
  of a cleartext disposal channel. Its grammar is narrow on purpose: the
  value is exactly `yes`; any other value, its presence under
  `tls_mode = REVERSE_PROXY`, and its presence without both disposal keys are
  each MALFORMED, so it can never be read as a feature flag with an "off"
  setting.

Both disposal keys require `web_gui = yes`; naming one without the other is
MALFORMED; naming `decisions:dispose` inside `web_gui_anonymous_scopes` was
already MALFORMED before this season, because that scope's `grantable` bit is
`false` and the anonymous-floor parser refuses any non-grantable name — no
new code, a test added to prove it. `atlas gateway status` prints all three
as a `dispose:` line and a `clear:` line, unconditionally whenever the
gateway is `ENABLED`, on the same argument `acbd7ad` gave for the anonymous
floor's `anon:` line: a capability an auditor must be able to see belongs in
the command Atlas offers for seeing it, not in a root-owned file they may not
be able to open.

**The ledger's new column.** `decision_events` gains `key_id TEXT` (migration
31), because `decision_challenges` — the table that already carried "which
channel minted this" — is pruned to 200 rows
(`ATLAS_DECISION_CHALLENGES_RETAIN`), and the ledger is canonical and
append-only. A remote disposal's event row therefore carries the credential's
id directly, and its `detail` text names it in prose too, so the row reads
correctly even to a reader who ignores the column:

```
confirmed through the Atlas remote operator channel with credential %s; this
records that the channel and the credential were used, not which person used
them
```

`decision_challenges` itself gains `channel TEXT NOT NULL DEFAULT 'LOCAL'
CHECK(channel IN ('LOCAL','REMOTE'))` and its own `key_id TEXT`, so a
capability minted through one channel can never be spent through the other.

## 2. The seventeen decisions this season settled, each with its argument

The full text, with every line-numbered citation against the tree the season
was planned from, is in `docs/plans/2026-09-04-browser-disposal.md` §The
seventeen decisions. What follows is the argument in one paragraph each,
verified against the tree at `d8e9e49` rather than against the plan's
intention, with every place execution corrected the plan noted as such.

1. **A session cookie can never dispose; only a bearer token presented on the
   request itself, verified by the daemon inside the transaction that spends
   the capability.** The gateway holds no token for a cookie principal — only
   a key id, a label and an expiry (`session_put`) — so a write route that
   wanted to authorise on a session would have nothing to check against but
   its own claim. The two write routes call `authenticate()` from the
   `Authorization` header alone and never touch `session_get` or
   `anonymous_ok`; CSRF and DNS rebinding are structural consequences (no
   cookie path exists to protect, and a rebound page still presents nothing,
   which the write routes refuse).
2. **The disposal credential holds no stored scope; the policy derives
   exactly one; and a key with any stored scope is refused as the disposal
   credential.** `decisions:dispose` is never written to an `api_keys` row —
   `atlas api-key create` refuses it by name, the same refusal
   `memory:write` already had — and `gateway.auth` derives it, for the key
   the policy names, **only when that key's own stored scope list is
   empty**. A credential that can read anything is a credential that could
   already have been handed to a model over `/mcp`; the deliberate minting
   form is `atlas api-key create --label L --no-scopes`.
3. **The channel is a stored fact on the challenge; `UNKNOWN` is zero and is
   refused; the actor comes from the stored challenge, never from the
   request; and a mismatch is refused in both directions.**
   `atlas_decision_channel` has three members, `UNKNOWN = 0`, `LOCAL`,
   `REMOTE`; every producer sets it explicitly, and the write point refuses
   `UNKNOWN` for every op that mints or spends a capability. `spend_challenge`
   refuses a `REMOTE` challenge spent as `LOCAL` and the reverse, and for
   `REMOTE` refuses a credential id that differs from the one the challenge
   was minted for. `op_approve`, `op_reject` and `op_resolve` then read the
   actor from `c.channel` — the row `spend_challenge` loaded, in
   `channel_actor_detail` — never from the operation a caller submitted, so
   the actor is evidence of the path a capability actually took rather than
   a claim a request made about itself. **Why the caller counts did not
   move:** the credential check lives in `src/decision/remote.c`, called
   from inside `atlas_decision_apply_in_tx` for `channel == REMOTE` only —
   one more internal call, no external caller — so that function keeps
   exactly three callers and `atlas_service_decision_confirm` keeps exactly
   two; the remote methods build ordinary `atlas_decision_op`s and submit
   them through the writer exactly as every other producer does.
4. **The credential travels into the ledger, because the challenge table is
   prunable.** Covered above under "the ledger's new column."
5. **A remote challenge is minted only for the newest revision, and is
   refused at spend if a newer one landed.** `op_challenge` under `REMOTE`
   refuses when the browser's pinned revision is not the latest;
   `spend_challenge` under `REMOTE` re-reads the latest revision and refuses
   again if one landed in the window between mint and spend. This closes,
   for this channel only, the stranded-newer-`PROPOSED` gap A15 measured
   for `--revision N`; the local flag's own semantics are untouched, because
   a terminal operator naming an older revision explicitly is naming it on
   purpose.
6. **Two root-owned policy keys, both refused rather than clamped, refused
   outright without TLS in front — amended to three on the operator's
   authority.** Covered above under "the policy's three keys."
   `/etc/atlas/gateway.conf` was chosen over `authority.conf` for three
   reasons that each suffice: the file's own stated purpose is constraining
   the gateway's principal, and a credential is that principal; the daemon
   already loads this file and the TLS stance the gate needs is in it, so
   one read answers "is remote disposal on"; and a malformed
   `authority.conf` locks the *local* channel, so a typo in a remote line
   placed there would lock the operator out of their own terminal — a
   malformed gateway policy disables only the gateway, which is the right
   blast radius.
7. **A second route table for two `POST` routes whose bodies are
   query-string syntax, so no JSON is parsed in `src/gw`.** Covered above
   under "the write table." The body must be
   `Content-Type: application/x-www-form-urlencoded` (415 otherwise), at
   most `ATLAS_GW_WRITE_BODY_MAX_BYTES` (4096 bytes; 413 above it), and is
   parsed by the same `build_api_params` that already parses `req->query`.
   Execution found the one trap this shape hides: a browser's `fetch` with a
   `URLSearchParams` body sets `Content-Type` to
   `application/x-www-form-urlencoded;charset=UTF-8` by default in every
   major browser, and the route's `Content-Type` check is an exact-literal
   `strcmp` with no parameter stripping — a page that did not set the header
   itself would fail every disposal with 415. Mission Control's own script
   sets the header explicitly for this reason, and the test asserts it
   character for character rather than trusting a default.
8. **The challenge response carries the digest and never the confirmation
   phrase; the operator types eight characters.** `decision.remote_challenge`
   returns the revision's full `content_hash` and no `confirm` key — a
   browser would auto-fill a returned confirmation, which would make the
   requirement decorative. The page compares the typed prefix against the
   displayed digest as a courtesy; `spend_challenge` compares it against the
   *stored* hash as the actual guard, exactly as the local prompt always has.
9. **The daemon-side group lives in its own file, is offered only under four
   conditions, and answers `unknown method` otherwise; the gateway refuses
   with a sentence first.** Covered above under "the method group's four
   conditions." The fourth condition — `state == ENABLED` — was added after
   a review measured that a policy Atlas had refused still left every
   field the other three conditions check populated, because
   `atlas_gwpolicy_parse_buffer` writes each key as it parses the line
   naming it and returns at the first malformed line without clearing what
   it already wrote, the file's one convention applied without exception. A
   policy naming both disposal keys and the acceptance key, under
   `tls_mode = NONE`, with one unrelated unrecognised key elsewhere, would
   otherwise have satisfied the peer check, the key-name check and the
   TLS-or-acceptance check while `state` correctly read `DISABLED` — a gap
   this document names because the same shape is still latent, unfixed by
   this season, at `src/ipc/server_gw.c:354`, which trusts `gateway_uid`
   from a rejected policy for a different purpose.
10. **`gateway.auth` derives the scope; `decisions:dispose` is in the
    vocabulary with `grantable = false`; and that single bit is what keeps
    the anonymous floor from ever holding it.** `method_gateway_auth`
    (`src/ipc/server_gw.c`) appends `decisions:dispose` to a verified key's
    reported scopes only when that key's own stored mask is zero, the
    policy is ready (the same four-condition predicate, minus the peer
    check, which this endpoint has already passed), and the key's id
    matches `remote_dispose_key`. Two independent fences keep the anonymous
    floor from ever reporting it: the floor's parser refuses any
    non-grantable scope outright, and the write path never consults
    `anonymous_ok` at all.
11. **Approve, reject and resolve are the remote intents; supersede and
    revalidate are not offered, at the write point and not only at the
    method.** `decision.remote_challenge` refuses an intent outside the
    three; `op_challenge` and `spend_challenge` refuse a `REMOTE` challenge
    for `SUPERSEDE` or `REVALIDATE` independently of the method's own check,
    with the frozen sentence `supersede and revalidate are not offered from
    the browser; use a terminal on the Atlas machine`. Supersede names a
    second document and revalidate binds a pinned repository state and an
    evidence digest — neither fits a browser review honestly, the same
    argument A15 made for the sheet grammar, one channel over.
12. **`op_approve`'s hard-coded supersession sentence, false for a pinned,
    older revision approved after a newer one, is fixed in this season's
    own diff.** `op_approve` now compares `prev_rev_no` against the newly
    approved revision's own number and chooses between "replaced by a
    **later** revision" and "replaced by an **earlier** revision" rather
    than asserting the first unconditionally — found while establishing
    A15's `--revision N` behaviour, fixed here because this season's
    write-point work already had that exact function open for the actor and
    `key_id` change, and leaving a known-false ledger sentence beside code
    already under review would draw the same finding on the next diff
    regardless. The three dead read routes A15 also recorded stay
    unfixed: this season never touches the read table, and fixing them is
    a different table shape than the one this season's whole argument rests
    on not changing.
13. **A remote challenge is minted only for the newest revision, and spends
    only if it still is — the lifecycle gap avoided structurally.** Same
    mechanism as Decision 5, restated here because it is also the answer to
    the specific defect A15 measured and did not fix.
14. **Migration 31 rebuilds two tables on their own precedents, and its one
    default is a true statement about every existing row.** `decision_events`
    is rebuilt as migration 15 rebuilt it, widening the actor `CHECK` and
    adding `key_id`; `decision_challenges` is rebuilt as migration 13
    rebuilt it, adding `channel` (`DEFAULT 'LOCAL'`) and `key_id`. The
    default is not migration 19's mistake: every challenge row that exists
    before this migration was minted through the only channel that existed,
    a fact re-derived and confirmed rather than assumed — the daemon's own
    operator-challenge method was the second, previously unread producer
    besides the local CLI helper, and both are local by the channel
    vocabulary's own definition, since a Unix socket peer inside this
    machine is what "local" means here. Row preservation is proved by a
    digest over values, not counts, inside the rebuild's own transaction,
    under two named `CHECK`s.
15. **The page's disposal panel holds the credential in memory for the tab's
    life, shows the same facts the terminal prompt shows, and requires the
    typed digest prefix.** Amended by the operator's own ruling
    (`§Decisions the operator answered` below): the key is kept in
    `sessionStorage`, not memory-only and never `localStorage` — one fixed
    key, every access wrapped, cleared when the tab closes. `Dispose:
    approve|reject|resolve` buttons mirror `atlas_review_intent_allowed` and
    `atlas_decision_kind_resolvable` as courtesy only; the write point is the
    guard. No test executes the page's JavaScript; the suite greps the
    served bytes for the required sentences and drives both routes with a
    real bearer credential against a tool daemon.
16. **Audit, log and status say what happened without saying more.** Every
    write-route request appends a `gw_audit` row through the existing
    `audit()`, `key_id` the verified selector, never the token or the
    confirmation. `atlas gateway status` prints `dispose:` and `clear:`
    unconditionally; `/auth/me` gains `"remote_disposal"` and
    `"cleartext_disposal"` booleans, in that order after `"anonymous"`, so
    the page can say whether the panel can work here and show the cleartext
    sentence at the moment of use, without logging in with the disposal key
    itself.
17. **Withdrawn by amendment: there is no TLS terminator on this machine,
    and the policy says so in writing.** The season's plan as first
    committed carried a task that put nginx in front of the gateway on
    `192.168.0.198:8799`. The operator declined TLS on their own network
    (§The decision) before that task ran; it never executed and is
    recoverable from the plan's own history if a later deployment wants a
    terminator. `atlas gateway status` on this machine now reads
    `tls: NONE`, a `dispose:` line naming the credential and kinds, and
    `clear: ACCEPTED — operator_accepts_cleartext_disposal = yes: …` — the
    cost, printed on every run rather than left to a file an auditor might
    not read.

## The decision: TLS in front stays the requirement; this deployment's
departure from it is a written, printed acceptance

The plan this season executed made TLS in front of the gateway a hard
requirement with **no cleartext opt-out**, for four reasons stated in full at
`docs/plans/2026-09-04-browser-disposal.md` §The decision and repeated here
because a reader of this document should not have to cross to that one to
see the argument: an anonymous read floor leaks *data*, a cleartext disposal
credential leaks *authority* with no expiry to bound the leak; a read writes
one `gw_audit` row, a disposal writes to the one record in Atlas that is
**not rebuildable**; `REMOTE_OPERATOR_CONFIRMED` is only worth recording if
it means "the credential the policy names was presented over the transport
the policy declares," and cleartext on a shared segment would make it mean
"whoever was on the segment" instead; and the operator had already been
given "TLS in front" as one of tier 3's six non-negotiables when choosing it.

The operator was shown that chain on 2026-09-04 and answered no to a TLS
terminator, in their own words, quoted above. The requirement did not
disappear — it became `operator_accepts_cleartext_disposal = yes`, a
root-owned key that is absent by default, refused under
`tls_mode = REVERSE_PROXY` (nothing to accept), refused without both
disposal keys (nothing named to accept it for), printed by
`atlas gateway status` on every run, and reported to the page by `/auth/me`
so the cleartext sentence appears at the moment of use. One condition was
added to the daemon's offer predicate; none was removed — a reader of the
code still sees TLS as the intended shape and this deployment as a
deliberate, recorded departure from it. The nginx terminator task that would
have put something in front of this listener was removed from the plan
before it ran; nothing in the code refuses one being added later, and
`REVERSE_PROXY` remains the shape the gate prefers.

## Decisions the operator answered before dispatching any code

Two questions the plan left open were ruled on before the first
implementation task ran, rather than being handed to the operator as a menu
with invented labels — the mistake the previous season made and was told
the cost of:

- **Every record kind may be disposed of from the browser**, not the plan's
  own suggested starting line of `OPERATIONAL_FACT PARKED`. The operator's
  own index holds a `POLICY` record among four total; shipping a season that
  could not dispose of the one `POLICY` record present would reproduce A15's
  exact failure, something that works on paper and not on the records the
  operator actually has. The key is root-owned and narrowing it later costs
  one line and a daemon restart.
- **The page keeps the disposal key in `sessionStorage`**, per tab, cleared
  when the tab closes — not memory-only, and never `localStorage`.
  Memory-only means retyping the key on every page load, which is the exact
  annoyance that produced the anonymous read floor a season earlier;
  shipping that annoyance again in a new place would be the same mistake in
  a third form.

Both are policy and page choices, changeable without a code change or
migration if the operator's answer changes later.

## 3. Frozen formats

Every vocabulary member, policy key, route row, method name, request and
response shape, refusal sentence, ledger sentence and UI sentence this season
introduced is pinned verbatim in
`docs/plans/2026-09-04-browser-disposal.md` §Frozen formats, and implemented
in `include/atlas/decision.h`, `include/atlas/decision_ops.h`,
`include/atlas/gwpolicy.h`, `src/decision/remote.c`, `src/decision/lifecycle.c`
and `src/ipc/server_remote.c` exactly as pinned there. This document states
what those formats mean and why; it does not repeat them in a form that
could drift from either the plan or the source.

## 4. What execution established that the plan did not claim

The plan pinned every interface exactly; running the code against it found
four things worth recording, because a plan is a proposal about what code
will do and only running the code establishes what it actually does.

**A load-bearing check with no test that would catch its removal.**
`atlas_server_peer_is_gateway(ctx, peer_uid)` is the second of the method
group's four conditions — the kernel fact that only the gateway's own uid may
reach these two methods at all. A review replaced the call with
`(void)peer_uid;` and rebuilt: both the daemon-disposal suite and the
authority suite **still passed**. The chain: every positive test case in the
suite as first written set `gateway_uid = getuid()` and called from
`getuid()`, so the peer check was always true in every case that mattered,
and every negative case was refused by the *policy* half of the predicate
instead, which never touched the deleted line. The shipped code was correct;
the test that would have caught a regression in it did not exist until a
policy naming a different uid (`getuid() + 1`) was added specifically to
observe both disposal methods reach ordinary parameter validation — spend
without ever the peer being asked at all — which is what the mutation
established and what the added test now watches for.

**A refused policy that still satisfies the rest of the offer predicate.**
Covered above as Decision 9's mid-season amendment: measured directly, not
inferred, that `atlas_gwpolicy_parse_buffer`'s convention of writing a field
before checking whether the line that named it made the whole policy
malformed applies without exception to the two disposal keys and the
acceptance key, so a policy Atlas had already refused could otherwise still
answer "yes" to three of the four conditions the daemon checks before
offering the group.

**A named replacement that could relabel its way past an intent check.** A
regression, not a plan defect: a reorder inside the write point's op-kind
handling checked only `op->intent` at the point a capability's actual
operation was decided, while a block a few lines below turned any operation
naming a `replacement_uid` into a supersede regardless of what the intent
said — so a request built as an ordinary `APPROVE` with a replacement named
could still relabel itself into a `SUPERSEDE` after the check meant to catch
it. `op->replacement_uid.len > 0` is now a third disjunct in that check,
found and fixed inside this season's own review cycle before it reached a
tagged commit as a defect.

**The `Content-Type` a real browser sends is not the literal a strict
comparison expects.** Documented above as part of Decision 7: `fetch` with a
`URLSearchParams` body appends `;charset=UTF-8` to
`application/x-www-form-urlencoded` by default in every major browser engine,
and the write route's `Content-Type` check is an exact `strcmp` with no
parameter stripping, so a page that relied on the default would have every
disposal answered `415` in production while every test that set the header
explicitly stayed green. Mission Control's own script sets the header
itself for this reason.

## 5. Stated costs

- **The credential passes through two processes Atlas does not verify.** The
  gateway process, which A9 designed to hold no authority of its own, and
  whatever terminates TLS in front of it when one is installed — on this
  deployment, nothing does, and §The decision states that cost by name.
- **A holder of the credential disposes exactly as the operator does**,
  bounded by the policy's own kinds list and by nothing else the credential
  itself carries: it has no stored scope, so its authority is entirely the
  root-owned line naming it. **Revocation is the bound.** `atlas api-key
  revoke` takes effect on the next request; there is no other ceiling, and
  none is claimed.
- **The page executes no JavaScript under test.** The suite greps the served
  bytes for the bindings and frozen sentences each panel depends on, and
  drives the two write routes with a real bearer credential against a test
  daemon carrying an injected policy (`tests/tools/atlas_gw_daemon.c`,
  P0's precedent for a policy injected through `atlas_daemon_opts` rather
  than a flag, an environment variable or a path override). What is
  established is that the served bytes and the routes agree with each
  other; whether a click renders what the source says it renders is not
  claimed, exactly as A15's document said about its own page.
- **A page reload asks for the key again unless the operator's own answer
  said otherwise** — and it did: the operator ruled `sessionStorage`, so a
  reload inside the same tab does not ask again, but closing the tab does,
  and the key is never written to `localStorage` under any answer.
- **On this deployment the disposal credential travels in the clear** — the
  cleartext chain stated in full at the top of this document, accepted by
  the operator in writing on 2026-09-04, and printed by
  `atlas gateway status` on every run rather than left for an auditor to
  discover.
