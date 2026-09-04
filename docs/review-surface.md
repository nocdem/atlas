# The review surface, and where a proposal is disposed of (A15)

A4 gave Atlas a lifecycle and one channel that may move it — an interactive
terminal, on the machine, typing a hash prefix. A9 gave Atlas a web surface
that may only read. Between them sat an inconvenience an operator felt on
every proposal: the record is usually written by a model over MCP, the only
place that renders it well is a terminal nobody wants to read a diff-shaped
document in, and the only surface that renders it well could do nothing about
it.

The sentence this season exists for:

> **A PROPOSAL NOBODY CAN REVIEW COMFORTABLY IS A PROPOSAL NOBODY REVIEWS.**

A15 adds **no migration**. `MIGRATIONS[]` still ends at 30 (A12.1's). It adds
no RPC method, no gateway route, no MCP tool, no scope and no actor. What it
adds is a Review view in Mission Control that reads what already exists, a
plain-text list an operator builds by clicking through that view, and one new
local command, `atlas review apply FILE`, that walks that list through the
operator channel A4 already built — the same function, the same capability,
the same `/dev/tty` prompt, once per line.

Nothing here grants new authority. A record queued in a browser is not
reviewed, approved, rejected or resolved by that act; the only thing that
disposes of a record is still `atlas_service_decision_confirm`, reached now
from two call sites instead of one, and `atlas_decision_apply_in_tx` still has
exactly three callers. What follows is the argument for the parts that are
new, and, at the end, the findings running the season's own plan produced that
the plan itself did not claim.

## 1. What is true today, measured against the tree

**The route table.** `API_ROUTES[]` (`src/gw/gateway.c`) holds **26 rows**,
counted by `grep -c` for this document rather than pinned as a number the
table itself is tested against — the table's own test asserts a *property*
of every row instead: `tests/test_gateway.c`'s
`test_every_api_route_forwards_to_a_read_on_the_reviewed_allowlist`. Its real
guarantee is a **positive allowlist**, `READ_METHODS[]`, naming every method
any row is permitted to forward to today; a row naming anything else fails on
that line the moment it is added, and the array is filled in by hand, never
generated from the table, so a name reaches it only on purpose. The test's
own comment states why: an earlier version checked only *absence* from a few
known-write groups and a hand-kept name list, and it would have passed a row
naming `verify.evaluate`, `decision.propose`, `decision.revise` or several
other genuine writes, because none of those names happened to match anything
the negative checks were written to catch — "a negative list can only ever
list what somebody already thought of." A separate set of negative checks
still runs beside the allowlist — not an operator method, not
`gateway.auth` or `gateway.audit`, not a member of the backup, apikey or
orchestration method groups, and a grantable scope — but the test's own
comment is explicit that these are **the stated reasons a name must never
appear, not the guarantee itself**: the same comment records that the first
version's own copy of the backup group's names had already drifted from
`BACKUP_METHODS[]` — missing `operation.get` and `code.sem_config` — which is
why those checks now read their groups from the accessor functions
(`atlas_server_backup_methods` and the others) rather than from a second,
restatable list. The route count itself is asserted nowhere in the test, on
purpose: a table that grows or shrinks by a row must not change whether the
test passes, only the property of each row must.

Outside that table, `atlas_gateway_serve_bytes` matches **six literal
paths** — `GET /healthz`, `POST /mcp`, `POST /auth/login`, `POST /auth/logout`,
`GET /auth/me`, and `GET /` with `/index.html` as an alias for the same
handler — plus one method-on-any-path preflight case, `OPTIONS`, which is not
counted among the six because it does not name a path at all. None of the
seven mutates a decision, an evidence row or a claim; the two that write
anything write only the gateway's own bookkeeping (next).

**What actually writes, on this listener.** Two things, and only two.
`gw_audit` gets one appended row from every request that reaches `/api/`,
`/auth/*` or `/mcp` — `audit()` (`gateway.c:430`), queued to the daemon's
writer thread, its fate never reported back to the caller that triggered it,
which is A9's own rule that audit failure must not break request handling.
And `/auth/login` and `/auth/logout` mutate an in-memory table,
`gw_sessions[ATLAS_GW_MAX_SESSIONS]` (`gateway.c:597`) — a bounded array of
session rows that exists only in the gateway process's own memory and is
gone the moment it restarts. Nothing else on this listener is a write.
Reading a revision, a claim, a gate assessment or an impact walk touches
neither table.

**The fact beneath the table.** A route naming `decision.approve` would still
be answered `unknown method` if someone put one in `API_ROUTES[]` by mistake,
because the operator method group is not reached by matching a route at all:
it is offered only to a peer whose `SO_PEERCRED` uid the root-owned policy
names as the operator (`atlas_server_peer_is_operator`,
`src/ipc/server_decision.c:2738`, consulted at `src/ipc/server.c:1250`). The
gateway process runs as its own uid — `atlas-gateway`, uid 992 on this
machine, never the operator's — so it is not in the set the check admits,
regardless of what the route table says. This is A7.1's argument one layer
up: what the gateway cannot do is true because of who it runs as, and no line
in `src/gw` can widen it. That is what makes the route-table property
`tests/test_gateway.c` checks a *second* fence rather than the only one: the
first fence is a kernel fact about a Unix socket peer, and it does not move
whatever this season's table says.

**Two facts A15 does not change and restates for the record.** A bearer token
(the remote-MCP shape) and a session cookie (the browser's shape) resolve to
*one* `principal` inside the gateway — `gateway.c:1410-1413`'s own comment says
the authorization engine does not know which mechanism produced it. And a
model's API key and the browser's session cookie are the same *kind* of
principal, distinguished only by which scopes a root-owned policy granted the
key behind either one. Both facts are why disposal never moved onto this
listener; see §2.

## 2. The tier decision, and its chain

The roadmap named three tiers and deliberately did not choose. This plan
chose **tier 1** — the UI reviews, the terminal disposes — and the choice is a
chain, not a preference:

1. Reading a proposal well is expensive to build and cheap to authorise: every
   revision, its claims, each claim's evidence and attestations, the gate's
   freshness where the record is approved, and what it links to are all reads
   the gateway already serves or nearly serves. Rendering them is the
   expensive half of the work, and rendering is what a browser is good at.
2. Disposing of a proposal cannot move onto the listener the model reaches
   without moving the threat model, because the listener does not distinguish
   the model's principal from the browser's — §1's last paragraph. Any
   capability placed on `/api/` is one scope bit away from the same key that
   proposed the record being reviewed, and on this machine that bit would
   travel over cleartext HTTP (`tls_mode = NONE`), in the clear, on the LAN.
3. So the queue moves to where reading is easy — a browser under the
   operator's own session — and disposing stays exactly where the only channel
   for it already lives: an interactive terminal, on the machine, as the
   operator's own uid. `LOCAL_OPERATOR_CONFIRMED` keeps meaning exactly what
   it meant before this season: a local process with a controlling terminal
   on both standard streams, holding a single-use capability bound to one
   revision's content hash, valid 120 seconds. A same-uid process driving a
   pseudo-terminal reaches it exactly as a person does — `tests/test_decision_operator.c`
   does so on purpose — and every comparison in this document is against that
   sentence, never a stronger one nobody had before A15.

**What tier 1 leaves undone, so the choice reads as the operator's and not as
an oversight.** The operator's stated purpose was to dispose of a record from
wherever they are. After A15, reading happens wherever a browser reaches the
gateway; disposing still needs a terminal on the Atlas machine itself, as
uid 1000 — which an `ssh` session from anywhere provides, at the cost of that
session and a copied file, never at the cost of a phone with no shell. And
the queue itself has no cross-device form: it lives in one browser's
`localStorage`, under one origin, and moves between the browser and the
terminal only by the operator's own hand — copying the sheet's text and
saving it as a file. Both are stated costs of tier 1, not defects tier 1
introduced; a wider answer is tier 2 or tier 3, and tier 2 was considered and
rejected in the same planning pass: showing the single-use capability in the
browser needs a new daemon method offered to the gateway's uid, which is
tier 3's cost with none of tier 3's protections, and the local command mints
its own capability regardless of what the browser shows.

## 3. Tier 3, costed and not built

If a genuine remote operator channel is ever wanted, six requirements from
the roadmap are not negotiable, and a design missing any one of them should be
refused rather than shipped. Each is stated here with the concrete mechanism
and the further cost this season's planning found while pricing it:

1. **Its own channel identity.** A new member of `atlas_decision_actor`,
   `ATLAS_DECISION_ACTOR_REMOTE_OPERATOR_CONFIRMED`, never a reuse of
   `LOCAL_OPERATOR_CONFIRMED` — reusing the local name would make every ledger
   row ever written retrospectively ambiguous, which is the one cost that
   cannot be paid back. Adding it needs its own migration, rebuilding
   `decision_events` to widen the actor `CHECK` exactly as migration 15 did,
   with the same row-count verification; whether `decision_challenges` also
   needs a channel column so a remotely minted capability cannot be spent
   locally, and whatever `atlas_db_decision_verify`'s replay would need to
   check about the new actor, are both unread — obligations of that season,
   not claims of this one.
2. **Its own scope, ungrantable to any model credential.** A `decisions:dispose`
   scope in `SCOPES[]` with `grantable = false`, on the `memory:write`
   precedent — never stored on a key row, derived only at verification from a
   root-owned policy line naming exactly which key may hold it.
3. **Absent from the MCP surface entirely**, so the two halves of the listener
   never share a door — the same test shape `tests/test_gw_remote.c` already
   uses to prove the most powerful A9 credential today reaches no write tool.
4. **TLS in front, which Atlas would still not be providing.** Atlas
   terminates no TLS and must never be described as doing so; the daemon
   would have to refuse to offer the remote-operator method group at all
   unless the gateway policy declares `tls_mode = REVERSE_PROXY`. On this
   machine, as configured today, that check fails (`tls: NONE`), so tier 3
   would refuse itself at its own first gate until a terminator is put in
   front of the listener and the policy says so.
5. **Replay protection bound to the content hash**, reusing the existing
   challenge shape rather than inventing a second one: mint against
   `(repo, document, revision, content_hash, intent)`, compare the typed
   confirmation at the write point exactly as today.
6. **A root-owned policy naming which kinds it may act on**, refused rather
   than clamped when malformed — a defensible starting line is
   `OPERATIONAL_FACT` and `PARKED` remotely, with `POLICY`, `INVARIANT` and
   `ACCEPTED_RISK` staying local-only.

Three further costs the roadmap's six points did not itemise, found while
pricing them in full: the daemon would have to authenticate the bearer token
itself, inside the transaction that spends the capability, because a key id
the gateway merely *claims* is exactly the shape of problem a remote
submission surface would also have to solve — a caller describing itself is
not evidence about itself; `api_handle` refuses every non-`GET` method for
the whole route table today, so a mutating remote-operator route needs either
a second table or a method column, not a one-line exception; and the gateway
parses no JSON request body today except the login key, by hand — a `yyjson`
call site inside `src/gw` would extend that vendored library's stated
contract and would need its own written argument. The honest weakening,
stated in the same paragraph that would announce tier 3: a compromised
gateway process would hold the operator's own bearer token for as long as the
operator used the remote channel, which the local channel never exposes at
all. Tier 3 is a season of its own, with its own authorisation, and it is
*weaker than the local channel by construction* — nothing above makes it
stronger, only reachable from further away.

## 4. The nine decisions this plan settled

1. **The queue is a review sheet in the operator's browser, and it stores no
   authority.** A queue held by the daemon would need a write route — the
   first one on `/api/`, under a scope a model's key could equally hold, which
   would let a model place "I would approve this" on its own proposal and have
   it appear as if it were the operator's intent. Keeping the queue in
   `localStorage`, under the gateway's own origin, keeps that impossible: the
   sheet is text the operator copies out, not a row Atlas wrote for them.
2. **The sheet reaches the terminal as a file argument, never on standard
   input.** `atlas_terminal_available` requires *both* standard streams to be
   terminals; a piped sheet makes standard input a pipe and the whole command
   refuses before a single challenge is minted.
3. **The walker loops the one confirm function with the revision pinned, and
   refuses before minting when the record moved.** `atlas_service_decision_confirm`
   is called once per sheet entry with `revision_no` set to the revision the
   operator reviewed. Before that call, the walker reads the record through
   the ordinary service read and refuses the entry — costing no challenge row
   at all — when the newest revision is no longer the one the sheet names, or
   that revision's content hash no longer starts with the sheet's prefix, or
   the record's status is no longer the one the intent needs.
4. **No route is added; three rows gain one more forwarded parameter each,**
   and a *property* of the table is tested rather than its count (§1).
5. **Drift in the browser is exactly one field, labelled with the one shape it
   can mean.** The only A12.1 output the gateway uid can read at all is
   `verify.show`'s `conflict` field. When it reads `IMPLEMENTATION`, the page
   shows the frozen label below and nothing wider; when it is anything else,
   the page says nothing about drift, because A12.1's reconciler produces that
   value for exactly one shape and no other, and a page implying broader
   detection would be advertising something the code does not do.
6. **Review-surface prose names the channel and never a person**, and the page
   and the walker both join the same scan that has checked `CLAUDE.md` since
   A12.1: `tests/test_decision_mcp.c`'s forbidden-phrase list, and its
   required-wording list, now cover `src/gw/ui/mission-control.html` and
   `src/core/service_review.c` too.
7. **Sheet intents are `approve`, `reject` and `resolve`; `supersede` and
   `revalidate` stay terminal-only**, because both need more than a
   five-field line can honestly carry — a second document, or a pinned
   repository state and an evidence digest — and a browser review does not
   establish either.
8. **The sheet's bound is written down, and the worst case follows from it.**
   `ATLAS_REVIEW_SHEET_MAX_ENTRIES` is 64, below `ATLAS_DECISION_CHALLENGES_RETAIN`
   (200), so a walk in which every entry is abandoned cannot by itself reach
   the retention ceiling. Above the bound the sheet is refused whole, never
   truncated.
9. **No test executes the page's JavaScript, and this document says so once
   more.** The suite greps the served bytes for the bindings each panel
   depends on and for the frozen sentences, and drives the routes those
   bindings name through a real session cookie. Surface parity beyond that —
   whether a click actually renders what the source says it renders — is not
   claimed by the suite, and §6 below is exactly the kind of thing that gap
   can hide.

## 5. Frozen formats

**The review sheet.** A plain ASCII list, one line per entry:

```
atlas-review-sheet/1
# lines beginning with # are ignored; blank lines are ignored
approve atlas atlas-dec-28f03b0a44a53db88f0deace6e79721b r1 6fb2be08
reject  atlas atlas-dec-c711a6d9c4954961a5e9d18240591d8e r3 5146bbb3
resolve atlas atlas-dec-314ed60fe9bd11400646934658843bf3 r2 89d53ae3
```

Five whitespace-separated fields per entry — `intent`, `repository`,
`decision`, `revision`, `confirm-prefix` — and **no sixth field for a
confirmation**: the browser's mirror of the same rule
`tests/test_decision_mcp.c` already enforces on every MCP tool schema, that
none may declare a `"confirmation":` property. The full grammar, every
refusal sentence, the verdict vocabulary, the command's human and JSON output
shapes, and the three edited route rows are pinned in
`docs/plans/2026-09-03-review-surface.md` §Frozen formats and implemented
verbatim in `include/atlas/review.h` and `src/core/review.c`; this document
does not repeat them a second time in a form that could drift from either.

**The exit-code contract change.** `atlas review apply` exits `0` when every
entry ended `APPLIED` (or `READY` under `--check`), and **`8`** — a
command-specific value above the seven-value exit-code contract, following
`gate`'s own `8`/`9` — when at least one entry ended otherwise. It exits `2`
for five distinct causes, verified against `src/core/review.c` and
`src/core/service_review.c`: a malformed sheet, a sheet with no entries, `--yes`,
`--json` without `--check`, and no interactive terminal on both standard
streams. A sixth cause exits `3`, not `2`, and is not one of those five: when
`read_sheet_bounded` (`src/core/service_review.c`) cannot open or read the
sheet file at all, it returns `ATLAS_ERR_CONFIG` — the same status
`src/core/unit.c`'s own read path returns when it cannot read a file, the
existing precedent this reuses rather than inventing a sixth `2` cause for a
sheet whose bytes were never read. Neither `README.md` nor `CLAUDE.md`'s
exit-code table needs a new line for this: both already list `3` as the
generic configuration-error code, and a file the caller pointed at that
cannot be opened is exactly that, not something specific to `review apply`.
A locked authority profile exits with whatever `atlas decision
approve` exits with, because `run_review` makes the identical
`atlas_authority_require` call first, before the sheet is even opened. This is
recorded in the season's own document as well as at the two places the
contract itself lives — `README.md`'s exit-code table and `CLAUDE.md`'s
"Exit codes (stable contract)" — because a reader of this document should not
have to cross to either one to learn that this season changed the contract at
all.

**The Review view's fixed sentences**, verbatim, because `tests/test_decision_mcp.c`
requires their needles and a paraphrase would fail the build:

> Queuing a record here stores no authority anywhere. To dispose of these
> records, save this sheet as a file on the Atlas machine and run
> `atlas review apply FILE` in a terminal there. Atlas records each disposal
> as LOCAL_OPERATOR_CONFIRMED, which names the channel, not a person.

> IMPLEMENTATION conflict: a decision-bound symbol anchor no longer resolves
> in a coverage-complete semantic generation. This is the only shape A12.1's
> reconciler can produce. It is a finding against the code, not a rewrite of
> the decision.

## 6. What execution established that the plan did not claim

The plan named two questions it deliberately left open — what a pinned,
non-newest revision does, and what `gate.check` answers for a record that is
not approved — and required the executing tasks to establish the answer by
running code rather than by reasoning about it. Both are recorded here and,
in full, beside the CLI features they govern in `docs/decision-lifecycle.md`.

**`gate.check`, and a record that is not `APPROVED`.** `atlas_gate_run` lists
only documents with status `APPROVED` as candidates for assessment
(`src/core/service_gate.c:365`), so naming any other record —
`PROPOSED`, `REJECTED`, `SUPERSEDED` or `RESOLVED` — narrows the assessment to
zero items. `atlas_gate_narrow_to_one` (`service_gate.c:456`) treats zero
items as a refusal rather than an empty list: HTTP 404, with the message
`no approved decision "<uid>" is attached to this repository`. A `PROPOSED`
uid, a `REJECTED` one, a `SUPERSEDED` one and a uid that does not exist at
all produce the byte-identical response — a caller cannot tell any of those
apart from this answer alone. **The filter is "not APPROVED", never
"PROPOSED"**: a reader who narrows the sentence to PROPOSED has been misled
by the more common case into missing the general rule.

**A pinned, non-newest revision, approved.** `op_challenge`
(`src/decision/lifecycle.c:911-919`) refuses a pinned revision only when it
does not exist at all — nothing there, or anywhere in `op_approve`, compares
the pinned revision to "the latest one"; `spend_challenge`
(`lifecycle.c:1164-1246`), the function every one of the five lifecycle verbs
spends its capability through, checks the token, the intent, the repository,
that it is unconsumed and unexpired, the typed confirmation and a rehash of
the stored content — and nothing about the document's status or the
revision's rank. Approving a revision that exists but is not the newest
**succeeds**: that revision becomes the document's effective, `APPROVED` one,
and a newer `PROPOSED` revision sitting beside it is left completely
untouched — not superseded, not rejected, not silently promoted, not even
read. Established by running
`test_a_pinned_revision_that_is_not_the_newest`
(`tests/test_decision_operator.c`), not inferred from the source — for the
*lifecycle* half of the claim only. That test drives `atlas_decision_apply`
directly, at the write point, the way `approve_through_the_write_point` does,
because the interactive form is refused in the locked profile an
unprivileged test runs in; it never carries `--revision N` through
`run_decision_confirm` or `atlas_service_decision_confirm`. So **the CLI
flag's own plumbing — that a value typed after `--revision` actually reaches
the service call — is read from `src/cli/cli.c:848-858` and `:2069-2071`, not
measured by anything that runs**. Nothing in the lifecycle warns about the
resulting state, refuses it, or offers a path out of it beyond an operator
noticing and superseding or resolving the older revision by hand.

**The queue functions hold no `await` between loading the sheet and saving
it.** `reviewQueueAdd` and `reviewQueueRemove`
(`src/gw/ui/mission-control.html`) each read the whole queue from
`localStorage`, mutate the in-memory array, and write it back — synchronously,
with no `await` anywhere between the read and the write. That absence is not
an oversight to close; it is the property that makes two in-flight click
handlers from the same tab unable to interleave, which is what makes the
sheet's duplicate-replacement rule and its 64-entry bound reliable at all. An
edit that inserts an `await` between those two calls — a confirmation dialog
on a queue action would be the obvious way — would silently reopen a race
between two clicks, and nothing in this suite would catch it: `tests/test_gw_remote.c`
greps served bytes and drives routes through a session cookie, and executes
none of the page's JavaScript (Decision 9). The functions themselves now
carry a comment saying so, beside the two calls the invariant depends on.

## 7. Stated costs

- **One browser.** The queue lives in one browser's `localStorage`, under one
  origin. A cleared site store empties it; nothing about a sheet is durable
  evidence of anything, and it was never meant to be — the evidence is the
  ledger event `atlas review apply` writes when a sheet is actually spent.
- **`ssh`, or nothing.** Disposing of a record still needs an interactive
  terminal on the Atlas machine, as the operator's own uid. Reaching one from
  off the machine costs exactly an `ssh` session and a copied file — never a
  network route, a scope or a new credential.
- **No JavaScript is executed by the suite**, ever, for this view (Decision 9).
  What is established is that the served bytes carry the bindings and
  sentences each panel depends on, and that the routes those bindings name
  answer through a real session cookie — not that a click renders correctly
  for someone looking at a screen. §6's queue-race paragraph is exactly the
  shape of thing that gap can hide.
- **Roughly a dozen audit rows per record reviewed.** The plan's own
  worst-case count: one `decision`, one `decision/history`, one
  `verify/claims`, one `gate` when the record is `APPROVED`, one
  `verify/claim` per claim opened, one `decision?revision=N` per earlier
  revision opened, and at most `REVIEW_UI_MAX_IMPACT_LINKS` (8) impact walks —
  each one a read, and each one appends its own `gw_audit` row (§1). That
  count is the plan's, not re-measured for this document.
- **The read frequency is higher than the plan's own worst-case line
  states, and the correction is recorded here rather than silently
  reproduced.** The plan's §Worst-case cost says the sheet is re-read "on
  load … ≤ 64 reads", which understates when: `viewReview`
  (`mission-control.html`) calls `renderReviewSheet()` on every load *and*
  on every change to the repository or status selects, because both are
  wired to re-run the whole view. The ≤ 64 bound on any one re-read is still
  correct; the sentence describing how often a re-read happens is not. A
  second correction goes with it: `renderReviewSheet` clears the sheet panel
  before awaiting those re-reads (`clear(node)` runs before the
  `Promise.all` that fetches each queued entry's current state), so the
  panel a reviewer sees while the reads are in flight is an empty box, not a
  line saying a read is in progress. Neither is a defect the queue's
  correctness depends on — the walker's own pre-check re-reads every entry
  again, authoritatively, before minting anything — but both are what the
  page actually does, not what the plan described it as doing.
- **No cross-device queue, and disposal needs a shell on the machine.** Both
  are §2's stated costs of choosing tier 1, restated here because this is the
  document a later season reads before choosing to widen them.
- **The sheet cannot dispose of a PROPOSED revision that sits on top of an
  APPROVED one, and nothing in this season closes that.** `op_revise`
  (`src/decision/lifecycle.c`) gates a new revision on kind, revision count
  and content-hash idempotency, never on the document's status, so an
  APPROVED record acquiring a newer PROPOSED revision is the ordinary result
  of revising approved policy. `required_status_for`
  (`src/core/service_review.c`) compares an intent against the *document's*
  status, which still reads APPROVED, so a sheet line naming that new
  revision passes MOVED and the hash check and is refused DISPOSED for
  approve/reject, and — for an APPROVED `OBLIGATION` or `ACCEPTED_RISK` —
  reads `READY` under `--check` and is then refused inside `op_challenge`'s
  own revision-state check the moment it is actually run: a dry run says
  READY for an entry the lifecycle refuses. The Review view now declines to
  offer a queue button in this state rather than promise a disposal the
  walker cannot perform (`showReviewDetail`, `mission-control.html`), but
  the sheet grammar still has no way to name "the approved revision, not the
  latest one" — such a record can only be disposed of from `atlas decision
  approve|reject|resolve` on a terminal. The contained fix, moving
  `required_status_for` onto the revision's own state rather than the
  document's, is a design change with its own test and is recorded in
  `docs/backlog.md`, not made here.
