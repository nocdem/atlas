# A9 — remote MCP and the web control plane

Atlas A9 adds a way to reach an Atlas index from off the machine it runs on:
a **gateway** that terminates HTTP, authenticates a bearer credential, checks
scopes, and forwards only explicitly supported operations to `atlasd` over the
ordinary Unix socket. **That "authenticates" is no longer universally true**:
a root-owned policy may name an anonymous floor for browser reads with no
credential at all. See "Anonymous browser reads, stated honestly" below before
assuming every `/api/` request carries a credential. **A16's two write routes
are the opposite exception: they authenticate a bearer credential and
nothing else** — never a session cookie, never the anonymous floor, whatever
either one is configured to accept for a read. See "A16: the remote operator
channel" below.

Nothing about the local behaviour changes. `atlasd`, `/run/atlas/atlas.sock`,
stdio MCP and the Claude Code integration are exactly as they were.

## Architecture

```
ChatGPT / remote MCP client            Browser
            |                             |
            | HTTPS                       | HTTPS
            | Authorization: Bearer       | session cookie
            v                             v
     +---------------------------------------------+
     |              Atlas Gateway                   |   runs as its own uid
     |  authentication · scopes · limits · audit    |   (gateway_uid)
     +---------------------------------------------+
            |            |               |
          /mcp        /api/v1          (GUI)
            |            |               |
            +------------+---------------+
                         |
                         v
              /run/atlas/atlas.sock          <-- never exposed to the network
                         |
                         v
                      atlasd                  <-- owns the index, 0700 atlasd
                         |
                         v
                   Atlas index
```

The operator's own path is separate and local:

```
operator terminal --> atlas api-key create --> atlasd --> api_keys
                      (prints the secret once)
```

## What the gateway cannot do, and why that is not a promise

The gateway runs as the account named by `gateway_uid` in the root-owned policy.
That uid is **not** the operator uid and **not** a dispatcher uid, so the daemon
answers `unknown method` — the same answer a name that does not exist gets — to:

- `decision.approve`, `decision.reject`, `decision.supersede`,
  `decision.revalidate`, `decision.challenge`
- `backup.create`, `backup.verify`, `code.index`, `maintenance.plan`,
  `maintenance.prune`
- `apikey.create`, `apikey.list`, `apikey.revoke`
- every `dispatch.` method, and every `job.` method but the four `job.remote_*`
  names offered under a root-owned policy (A14), each of which does nothing
  without a named credential in flight; run a job

Under a separated A7.1 deployment it additionally cannot open the index at all,
because the index is `0700 atlasd`.

So: **a compromised gateway cannot approve a decision, register a repository,
read a backup, build an index, administer credentials or read the database.**
It can queue a job when a root-owned policy names a credential for it and that
credential is presented and verified by the daemon in the transaction. That
is true because of *who it runs as* combined with *what the daemon verifies*.
No code in `src/gw` is the boundary; a bug in the gateway cannot give it an
unlisted credential or make the daemon skip the transaction check.

### On an unseparated machine this guarantee does not apply

If you run everything as one account — the ordinary single-user install — then
the gateway runs as the account that owns the index, and a compromised gateway
is a compromised everything. Atlas does not pretend otherwise. The separation is
real only when a root-owned policy names a distinct `gateway_uid` and the index
is `0700 atlasd`.

## TLS

**Atlas terminates no TLS.** An in-process TLS stack would be a new third-party
dependency, which the project's hard rules forbid, so A9 does not have one.

`tls_mode = REVERSE_PROXY` records that something in front — nginx, Caddy, a load
balancer — terminates TLS and forwards over loopback. Atlas cannot verify that
and does not try; it is the operator's statement.

Do not describe A9 as providing TLS. It provides a service designed to sit behind
it.

The default bind is `127.0.0.1`. Binding anything else **requires** an explicit
`tls_mode`, even when the answer is `NONE` — which is then a decision an auditor
can find in the policy rather than the silent consequence of leaving a key out.

### `operator_accepts_cleartext_disposal` (A16)

A16 adds two write routes that dispose of a knowledge record from Mission
Control (below), gated on `tls_mode = REVERSE_PROXY` by default — the daemon
will not offer the disposal method group otherwise. `operator_accepts_cleartext_disposal`
is the one, deliberately narrow exception: a root-owned key, absent by
default, whose only accepted value is `yes`, by which an operator states in
writing that they accept a disposal credential crossing their own network
unencrypted. It is refused (MALFORMED, gateway disabled with a reason) under
three conditions: any value other than the literal `yes`; presence together
with `tls_mode = REVERSE_PROXY`, because there is then nothing to accept;
and presence without both `remote_dispose_key` and `remote_dispose_kinds`,
because there is then nothing named for it to apply to. `REVERSE_PROXY`
remains the shape the gate prefers — this key adds one condition to the
daemon's offer predicate and removes none, so a reader of the code still
sees TLS in front as the intended design and a deployment carrying this key
as a recorded departure from it. `atlas gateway status` prints whether it is
present on every run, exactly as it prints every other fact about this
policy an auditor might otherwise have to go looking for.

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
decision using the same mechanism. Full argument: `docs/browser-disposal.md`.

## Credentials

### Creating one

Credential administration is local and operator-only. It needs no web service,
no remote call and no external identity provider:

```sh
atlas api-key create \
  --label chatgpt \
  --scope context:read \
  --scope repo:read \
  --scope decisions:read \
  --scope graph:read \
  --scope impact:read
```

```
API key created.

id:     key_102cedaa5e32f266
label:  chatgpt
scopes:
  context:read
  repo:read
  decisions:read
  graph:read
  impact:read
created: 2026-08-12T16:37:38Z

ATLAS_API_KEY=atlas_102cedaa5e32f266_8iodQFrL2Gsl93MJbQ8nuAklx9A6hG8nv-J9lLlOMlw

This secret will not be shown again. Atlas stores a one-way verifier, so no
Atlas command, backup or database read can return it.
```

### The one-time secret is structural, not a discipline

After that print, **no copy of the plaintext exists anywhere**. The index holds
`HMAC-SHA256(salt, secret)`; there is no column a plaintext could be written to,
no read that returns one, and no method — local or remote — that could produce
one. `tests/test_apikey.c` searches the database file as raw bytes for both the
whole token and its secret half, because a query can only find a leak in a
column somebody thought to check.

If the secret is lost, rotate:

```sh
atlas api-key rotate key_102cedaa5e32f266 --label chatgpt --scope repo:read
```

Rotation is create-then-revoke inside one transaction, so there is never a moment
when neither key works, and the link is recorded from both ends.

### Token format

```
atlas_<selector>_<secret>
       16 hex     43 base64url
```

The **selector** is 8 random bytes and is not a secret: it makes verification an
indexed lookup instead of a scan, which keeps authentication O(1) and stops the
cost of a wrong guess from revealing how many credentials exist. The **secret**
is 32 bytes — 256 bits — from `/dev/urandom` and from nothing else. Never a
timestamp, a pid, a user name, repository data or a machine id. If the kernel
cannot supply the bytes, creation fails; Atlas does not issue a credential it
could not make unpredictable.

### Why one HMAC pass and not a slow KDF

PBKDF2, scrypt and argon2 exist to make a *guessable* secret expensive to guess.
An Atlas API key is 256 bits of uniform randomness and is never derived from a
password, so there is no dictionary to iterate over and an iteration count buys
nothing against the only attack that exists.

It would, however, buy the attacker something: verification runs once per HTTP
request on an Internet-facing endpoint, so a 100 ms KDF is a
100 ms-per-unauthenticated-request amplifier — a denial of service handed out for
free. The entropy of the secret is the guarantee; the cost of verification is
deliberately negligible. The HMAC is checked against RFC 4231 vectors.

If a future phase ever accepts a credential a human chose, that credential must
not use this path.

### Listing and revoking

```sh
atlas api-key list                      # metadata only; never a secret
atlas api-key revoke key_102cedaa5e32f266
```

Revocation takes effect on commit. There is nothing to invalidate: the gateway
asks the daemon on every request, so there is no cached verdict anywhere that
could outlive it. **Revoking does not require stopping the daemon** — the command
routes over the socket to an operator-gated method when a daemon holds the writer
lock.

Revoking twice is not an error and does not claim to have revoked anything.

### Remote credential administration does not exist in A9

Not refused — absent. There is no MCP tool, no gateway route and no method the
gateway's uid can reach that creates, lists, rotates or revokes a credential, and
none that changes a credential's own scopes. `tests/test_gw_remote.c` asks the
running gateway for every name such a tool would plausibly have and requires each
to answer `unknown tool`.

## Scopes

| Scope | What it reads |
|---|---|
| `context:read` | task context packages, recorded memory, session state |
| `repo:read` | repository identity, HEAD, index freshness, changed files, file context, search |
| `decisions:read` | knowledge records of every kind, their history, freshness gates |
| `graph:read` | the structural and compiler-derived code graph |
| `impact:read` | change-impact analysis |
| `audit:read` | the gateway's own activity trail |

A scope name this binary does not recognise **fails closed** — at creation and at
verification, so an older Atlas reading a row a newer one wrote refuses it rather
than silently dropping the bit it did not understand.

`memory:write` exists in the vocabulary and **cannot be granted**. Every tool
that records something durable maps to it, which makes "no A9 credential can
write" the ordinary scope check finding a clear bit rather than a rule every tool
has to remember.

Permission checks happen server-side. The tool listing hides what a credential
may not call, but that is a convenience for the client: naming a hidden tool
directly reaches the same check and is refused.

## Remote MCP

Endpoint: `POST /mcp`, MCP Streamable HTTP, one JSON-RPC message per request,
one JSON document per response.

```
POST /mcp HTTP/1.1
Authorization: Bearer atlas_<selector>_<secret>
Content-Type: application/json
Content-Length: ...

{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}
```

`GET /mcp` answers 405. The Streamable HTTP specification requires exactly that
of a server offering no SSE stream, and Atlas offers none — every response is a
single JSON document, which is a complete and conformant implementation of the
transport.

The tool implementations are **the same ones stdio MCP uses**. There is one
`TOOLS[]` table and one run function per tool; the remote path differs only in
the transport, the scope check, and three things that only make sense with a
long-lived stdio peer:

- no `roots/list` request — a stateless transport has no client to ask;
- no session binding — there is no `CLAUDE_CODE_SESSION_ID`;
- no `initialize` handshake required — each POST is answered on its own.

With no roots, a repository is named explicitly, or resolved to the single
registered repository when there is exactly one. Anything else is a typed
refusal, never a guess.

### Connecting ChatGPT

1. Create a read-only credential (above) and capture the secret once.
2. Put a TLS terminator in front of the gateway and point it at the loopback
   listener.
3. Register the connector with:
   - **URL**: `https://<your-host>/mcp`
   - **Authentication**: bearer token
   - **Token**: the `ATLAS_API_KEY` value

The credential must be read-only. It must not permit repository writes, decision
mutation, process execution, configuration mutation, admin operations or
credential management — and in A9 it structurally cannot, because no grantable
scope reaches any of them.

## A9.1: knowledge kinds over the remote surfaces

A9.1 gave every knowledge record a `kind` beside its `status`, and the remote
surfaces expose both in the same shape the local ones do — see
`docs/decision-lifecycle.md` for what the kinds mean.

**Web API.** `GET /api/v1/decisions` gains a `kind` query parameter beside
`status`. Both are optional, both are independent, and both are validated by the
daemon against their closed vocabularies — an unrecognised value is a refusal
rather than an empty result, because a filter that matched nothing and a filter
that was misspelt are indistinguishable otherwise. Every document object carries
`"kind"` beside `"status"`, and the response carries `total_by_kind` beside the
five `total_*` status counts.

Adding the parameter meant adding it to the row in `API_ROUTES[]`, which is the
only way a query-string value reaches a daemon call. Nothing else in a query
string is forwarded.

**Remote MCP.** `atlas_decisions` gains a `kind` filter; `atlas_propose_decision`
gains a `kind` argument. Both are ordinary tool arguments, forwarded and validated
by the daemon.

**`atlas_revise_decision` is new, and it is a gap fix rather than a grant.**
`decision.revise` has existed since A4 and writes a PROPOSED revision by a
`MODEL_PROPOSAL` actor — exactly what `atlas_propose_decision` writes — so MCP
being unable to express it meant a model could only write a *new* record beside
an out-of-date one. Its scope is `memory:write`, which **cannot be granted**, so
no A9 credential can call it: remotely it is listed to nobody and refused to
everybody, and it is reachable only over stdio MCP where the local operator's own
session is the peer.

**Mission Control.** The Knowledge screen has two independent selects — kind and
status — and shows both as separate columns in the list and separate rows in the
detail pane. The two use **visually different treatments on purpose**: a status is
an outlined pill in the traffic-light palette, a kind is a filled uppercase chip
in a neutral tone, with `ACCEPTED_RISK` and `OBLIGATION` picking up colour because
those are the two a reader must not skim past. So an APPROVED INVARIANT, an
APPROVED ACCEPTED_RISK and an APPROVED DECISION are three visibly different rows
without opening anything.

Nothing about the boundary moved. There is still no route and no tool that
approves, rejects, supersedes, revalidates or resolves anything. A9.1's one new
lifecycle verb, `decision.resolve`, is in the operator-uid RPC group — a group the
gateway's uid is not in, so the gateway is told the method does not exist.

## A15: the review surface, and the three forwarded parameters

A15 adds Mission Control's Review view: a repository and status pick, a list of
records, a detail pane composing five existing read routes, and a review sheet
held in the browser's own `localStorage`. See `docs/review-surface.md` for the
full argument; this section is the remote-access half of it.

**Three rows forward one more query-string name each, and nothing else about
the table changes.** `/api/v1/decision` now forwards `revision`, so a specific
revision of a document can be fetched rather than only its effective one;
`/api/v1/gate` now forwards `decision`, so a gate assessment can be narrowed to
one record instead of every `APPROVED` one in the repository; `/api/v1/code/impact`
now forwards `symbol` beside `path`, closing a defect the Impact view already
had (see `docs/backlog.md`). Each is a name the daemon already read on its own
side of the socket; adding it to a row is the only way a query-string value
ever reaches a daemon call, and no new row, method or scope was needed for any
of the three.

**The Review view reads and queues; it never disposes.** It composes
`decision/history`, `verify/claims`, `verify/claim`, `gate` (for an `APPROVED`
record) and `code/impact` — five routes that already existed or already read —
into one detail pane per record, and keeps a plain-text list of
`(intent, repository, decision, revision, hash prefix)` lines in the browser's
own storage. Queuing a record writes nothing to the daemon at all; the list is
built from values the page itself already fetched and validated, and it is
carried off the browser only by the operator copying its text into a file and
running `atlas review apply FILE` on the machine, on a terminal, where the
disposal actually happens.

**The browser session and the bearer token are one principal type, and that is
why nothing capable of disposing of a record was placed behind either.** A
session cookie (the browser's shape) and a bearer token (the remote-MCP shape)
resolve to the identical `principal` inside the gateway — the authorization
engine does not know which mechanism produced it — and a model's API key and
an operator's browser session are the same *kind* of credential, distinguished
only by which scopes a root-owned policy granted one or the other. A
capability placed behind either would therefore be one scope grant away from
the other, which is why disposing of a record stayed on the one channel that
never runs through this listener at all: an interactive terminal, on the
machine, as the operator's own uid.

**Amended by A16.** That last sentence was true of *every* principal this
listener could produce on 2026-09-04 and is no longer true of all of them: a
bearer credential a root-owned policy names as the disposal key now can
dispose of a record, over the two routes the next section describes. It
remains true of everything else this listener can produce — a session
cookie can never dispose, whatever scopes it carries, because the two write
routes never call `session_get`; the anonymous floor can never hold the
disposal scope, because that scope's `grantable` bit is `false`; and every
credential except the one the policy names by id is refused the scope by
`gateway.auth` regardless of what it can read. The sentence above is
therefore corrected rather than deleted: disposing of a record moved off
"never" and onto "only a bearer credential the operator named in a
root-owned file, and never the credential that proposed the record it
disposes of" — see the next section.

## A16: the remote operator channel

A16 adds the first two `POST` routes this gateway serves, and the first
daemon method group offered to the gateway's own uid: `decision.remote_challenge`
and `decision.remote_dispose`, in `src/ipc/server_remote.c`, disjoint from
every method group that existed before it. Full argument, all seventeen
decisions and every finding running the season's plan produced beyond what
it claimed: `docs/browser-disposal.md`. This section is the remote-access
half.

**The two routes.**

```
POST /api/v1/decision/challenge   repo, decision, revision, intent
POST /api/v1/decision/dispose     repo, decision, intent, challenge, confirmation
```

Both require the `decisions:dispose` scope and answer `404` when the policy
names no disposal key at all — the shape `/mcp` already uses when
`remote_mcp` is off, so a caller debugging the panel gets a sentence and the
daemon's own silence stays the actual guard.

**The form body, and why not JSON.** Neither route accepts
`Content-Type: application/json`. Both require
`application/x-www-form-urlencoded` — the same query-string syntax
`build_api_params` already parses for every `GET` route — parsed from the
request body instead of the query string. That is a deliberate cost, not an
oversight: the gateway parses no JSON anywhere else except one bespoke login
key, by hand, and a `yyjson` call site inside `src/gw` would extend that
vendored library's stated contract for a project rule this season did not
want to argue past. The consequence worth naming: a browser's `fetch` with a
`URLSearchParams` body sets `Content-Type` to
`application/x-www-form-urlencoded;charset=UTF-8` by default, and the
route's own `Content-Type` check is an exact-literal comparison with no
parameter stripping, so a caller must set the header itself or every
disposal fails with `415`. Mission Control's own script does.

**Bearer-only, and why a cookie cannot dispose.** Both routes resolve their
principal from `authenticate()` — the `Authorization: Bearer` header on the
request itself — and never call `session_get` or `anonymous_ok`. The reason
is structural, not a policy choice made twice: the gateway holds no token
for a cookie principal, only a key id, a label and an expiry, so there is
nothing a write route could verify a session's *credential* against even if
it wanted to. The daemon verifies the presented bearer a second time, inside
the transaction that mints or spends the capability, against the key table
as it is at that moment — a key revoked between the gateway's check and the
writer's turn spends nothing.

**The derived scope.** `decisions:dispose` is in the scope vocabulary with
`grantable = false`: no `atlas api-key create` call can name it, and it is
never written to an `api_keys` row. `gateway.auth` derives it, for exactly
the key the root-owned `remote_dispose_key` line names, and only when that
key's own stored scope list is empty — a `--no-scopes` credential
(`atlas api-key create --label L --no-scopes`, A16's addition to that
command) is inert until this policy line gives it its one grant, and a
credential already holding ordinary read scopes is never widened by being
named here.

**The two policy keys, and their MALFORMED conditions.** `remote_dispose_key`
(exactly `key_` followed by 16 lowercase hex) and `remote_dispose_kinds`
(one or more of the eight decision kinds, space-separated, no duplicates)
travel together — one without the other is MALFORMED — and both require
`web_gui = yes` and, absent `operator_accepts_cleartext_disposal`,
`tls_mode = REVERSE_PROXY`. `decisions:dispose` named inside
`web_gui_anonymous_scopes` was already MALFORMED before this season, because
the anonymous-floor parser refuses any non-grantable scope by name. The
third key, `operator_accepts_cleartext_disposal`, is described above under
"TLS".

**The `dispose:` status line.** `atlas gateway status` prints `dispose:` and
`clear:` unconditionally whenever the gateway is `ENABLED`, naming the
credential id and kinds, or saying plainly that the browser can read and
queue but never dispose.

**`/auth/me`'s new fields.** The success body gains `"remote_disposal"`
(the policy names a disposal key at all) and `"cleartext_disposal"` (the
policy also carries the acceptance key), in that order after `"anonymous"`,
so Mission Control can say whether the disposal panel can work here and show
the cleartext sentence at the moment of use, without presenting the
disposal credential just to ask.

**The honest paragraph.**
The remote operator channel is *weaker than the local channel by construction*,
and nothing in this season makes it stronger — only reachable from further
away. The local channel's whole
worth was that the capability never touched a network: a local process, the
operator's own uid, `/dev/tty`, a single-use token that lived for 120
seconds inside one machine. On this channel the operator's disposal
credential passes through the gateway process — a network-facing process
A9 designed to hold no authority — and through whatever terminates TLS in
front of it, and Atlas verifies neither. A compromised gateway holds that
credential for as long as a request carrying it is in flight, and a holder
of the credential disposes exactly as the operator does. The ledger
therefore records every such act as `REMOTE_OPERATOR_CONFIRMED` with the
credential's id beside it, never as `LOCAL_OPERATOR_CONFIRMED`, so that a
reader of any row ever written can still tell the two apart.

## A14: remote submission

A14 adds four more routes to `API_WRITE_ROUTES[]` and four `job.remote_*`
methods to `src/ipc/server_orch_remote.c`. They are offered to the gateway uid
under three conditions: the gateway uid is in the peer, at least one
`remote_submit_key` is named in the policy, and the TLS or acceptance condition
holds. The credential on the request — not the gateway's uid — is the authority;
`require_submitter` is never called on this path.

**The four routes:**

```
POST /api/v1/job/submit   repo, task, key         (ATLAS_SCOPE_JOBS_SUBMIT)
POST /api/v1/job/get      job                     (ATLAS_SCOPE_JOBS_SUBMIT)
POST /api/v1/job/list     after, limit            (ATLAS_SCOPE_JOBS_SUBMIT)
POST /api/v1/job/cancel   job                     (ATLAS_SCOPE_JOBS_SUBMIT)
```

**The four MCP tools:** `atlas_job_submit`, `atlas_job_status`, `atlas_job_list`,
`atlas_job_cancel` — `remote_only = true`, absent from the stdio adapter, present
only on `/mcp` for a session carrying `ATLAS_SCOPE_JOBS_SUBMIT`.

**The derived scope.** `jobs:submit` (`ATLAS_SCOPE_JOBS_SUBMIT`) is in `SCOPES[]`
with `grantable = false`. It is derived for exactly the keys the `remote_submit_key`
lines name. `atlas api-key create --scope jobs:submit` is refused. One credential,
one power: `remote_submit_key` and `remote_dispose_key` may never name the same id.

**The two acceptance keys** (one per capability). `operator_accepts_cleartext_submission`
is distinct from `operator_accepts_cleartext_disposal`: a submission starts a worker
that runs as the operator's own account, which is a different consequence from moving
a record, and the plan does not reuse the disposal acceptance as cover for it.

**The `submit:` and `clear-submit:` status lines.** `atlas gateway status` prints
both unconditionally when the gateway is `ENABLED`, naming key ids, driver, mode,
gate count and bounds, or saying plainly that nothing reachable over the network
can queue a job. `(checked at submit)` names the fact that the driver and mode are
cross-checked against the orchestration policy at submit time, not here.

**`/auth/me`'s new fields.** Gains `remote_submission` (bool), `remote_submission_driver`
(string) and `cleartext_submission` (bool), so Mission Control's Jobs view can say
whether submission is available and show the cleartext chain at the moment of use.

**The honest paragraph.** Remote submission reaches the operator's own account from
wherever the bearer credential can be presented. The gateway has no authority of its
own; it is the carrier of a credential the daemon verifies in the write transaction.
The policy — not the request — decides the driver, the mode, the gate floor and the
bounds. The credential is verified in the transaction that creates the job; a revoked
key queues nothing even if the gateway's own check ran before the revocation.

**The cleartext chain.** On this deployment a submission credential presented by a
browser travels in the clear. The gateway listens on `192.168.0.198:8799` with
`tls_mode = NONE`, and the four submission routes carry the credential as a bearer
header on every request, so anyone able to observe traffic on that network segment
can read it. An Atlas API credential has no expiry, so a credential captured once
queues work — a worker that runs as the operator's own account, within the policy's
daily bound — until the operator notices and runs `atlas api-key revoke`. The
credential the MCP tunnel presents does not cross that segment: the tunnel client
runs on this host and posts to this host's own address, so its exposure is the file
it is read from in the operator's home directory — readable by exactly the account a
remotely submitted worker runs as — and the far side of the tunnel. The operator
accepted this chain on 2026-09-04 by writing `operator_accepts_cleartext_submission
= yes` into the root-owned gateway policy. Full argument and the operator's words:
`docs/remote-submission.md`.

## Audit

Every request through the gateway is recorded in `gw_audit`: when, which
credential, which interface, which operation, allowed or denied, succeeded or
failed, how long it took.

Never recorded: raw API keys, Authorization headers, request bodies, or any
secret Atlas returns. There is no column that could hold one.

A **denied** row names the selector that was presented, in `detail` — the
selector is not secret (it is half of what the client sent in the clear, and
exists so a token can be looked up by an indexed test). An operator needs to
know *which* credential was rejected four hundred times; "something was
rejected" is not actionable. It is deliberately kept out of `key_id`, which
means "the principal Atlas authenticated" and must never hold a value somebody
merely claimed.

Every text field is safe-encoded before it is written, which is the audit-log
injection defence — and a header value carrying a control byte is refused at the
HTTP parser, so nothing downstream ever receives one.

**Audit failure does not break request handling.** The row is queued to the
daemon's writer and the gateway never learns whether it landed. That is a
deliberate trade: Atlas prefers answering with a gap in the trail to refusing a
request because it could not write one.

Retention: `gw_audit` is the second prunable table in Atlas — see
`RETENTION[]` in `src/core/service_maintenance.c` for the argument. It is removed
only by `atlas maintenance prune --apply`, never on a timer, at startup or on low
disk.

## Running it

```sh
atlas gateway status     # what the policy says; binds nothing, safe anywhere
atlas gateway run        # serves until SIGTERM
```

`deploy/a9/` holds a systemd unit and a documented configuration template. The
unit runs as a dedicated `atlas-gateway` account — **not** the operator's, which
is the whole separation — with `ProtectSystem=strict`, no `ReadWritePaths` at
all, and `InaccessiblePaths=/opt` so the registered repositories are the daemon's
business and never the gateway's.

Neither the unit nor the policy is installed by Atlas. `atlas gateway run`
refuses to start unless the root-owned policy says `enabled = yes`, and reports
which condition failed and what would change it.

## Configuration

`/etc/atlas/gateway.conf`, root-owned, reached without traversing a symlink.
An unrecognised key is an error, not something skipped.

```ini
enabled = yes
gateway_uid = 995

listen_addr = 127.0.0.1
listen_port = 8787
public_url = https://atlas.example.com

# REVERSE_PROXY or NONE. Required for any non-loopback bind.
tls_mode = REVERSE_PROXY

remote_mcp = yes
web_gui = no

# DELIBERATELY NOT A DEFAULT. Absent, /api/ still refuses a request carrying
# neither a session cookie nor a bearer token, exactly as it always has. Named,
# every such request is granted exactly these scopes with no credential at
# all. Read "Anonymous browser reads, stated honestly" below before setting
# this on anything reachable beyond loopback.
# web_gui_anonymous_scopes = context:read repo:read decisions:read graph:read impact:read

# Exact origins, whole-string. `*` is refused.
allowed_origin = https://atlas.example.com

max_request_bytes = 1048576
max_concurrent = 64
rate_limit_per_minute = 600
session_ttl_seconds = 43200
trust_forwarded_for = no
```

A ceiling may only lower the compiled-in bound in `include/atlas/limits.h`, never
raise it. A policy that is missing, malformed, symlinked, or writable by anyone
but root leaves the gateway **disabled** with a reason. There is no direction in
which a degraded policy exposes more.

### Rate limiting, stated honestly

The limit is a fixed window over the gateway's total forwarded request rate.
Behind a reverse proxy every request appears to come from the proxy unless
`trust_forwarded_for` is set, so it degrades to a global limit rather than a
per-peer one. That is said here rather than hidden: a limit that looks per-peer
and is not is worse than one nobody believed in.

`trust_forwarded_for` is off by default because believing a header an attacker
can vary would make the limit unenforceable while continuing to look enforced.

### Anonymous browser reads, stated honestly

**This is not part of A9 as it shipped, and it deliberately moves the threat
model A9 built** — the one sentence at the top of this document, "authenticates
a bearer credential," is no longer true of every `/api/` request once this key
is set. It exists because an operator asked for it on a specific, named
deployment: Mission Control is served from a cleartext LAN listener
(`tls_mode = NONE`) with no browser origin restricted, and every `/api/` read
needed a session cookie obtained by posting an API key to `/auth/login` —
sessions live in gateway memory (`gateway.c:573-577`) and a restart forgets
them on purpose, so a repeated gateway restart meant repeatedly re-pasting a
key just to look at the page. The operator was told the cost before asking for
this — reproduced in full below — and reaffirmed the decision on **2026-09-04**.

**The mechanism.** `web_gui_anonymous_scopes` is a root-owned policy key, read
only when `web_gui = yes` — naming it with `web_gui = no`, or absent, is itself
**MALFORMED** and disables the gateway with a reason, on the same principle as
every other key here that could not take effect: a documented behaviour that
is not the implemented one is worse than no key at all. Absent (with
`web_gui = yes`), nothing changes: a request to `/api/` or `/auth/me` with no
bearer token, and with no session cookie that resolves to a live session, is
still refused with 401, exactly as before this key existed. Named, such a
request is granted exactly the scopes listed — never more, never a scope the
operator did not write down, and never `audit:read` by default: naming it is
the only way to get it, because a reader who can see the audit trail is a
capability wider than "read the project," and Atlas will not decide that for
an operator who did not ask. A session or bearer credential that *does*
authenticate is never masked down to this floor; the floor only ever fills the
gap left by a request with no live principal.

A wrong bearer token is treated differently from a stale session cookie, on
purpose. Any presented `Authorization` header — a bearer token that failed, or
one that is not even shaped like one — stays refused outright, never sliding
to the floor; the common, motivating case is a bearer token, because it
carries a selector the daemon's own DENIED row can name, and spending that
audit signal on a request that already failed once would be worse than simply
refusing it. A session cookie that does not resolve — expired, forged, or
simply left over from before the gateway's last restart, since sessions live
only in memory and a restart forgets every one of them — carries no such
signal to lose, and is exactly the case this key exists to help: an operator's
browser holding a pre-restart cookie lands on the anonymous floor instead of a
hard 401 that only a manual logout clears.

**A precise correction to "nothing changes."** The 401 *refusal* shape is
byte-identical whether or not this key is set, and that is what the paragraph
above and its tests mean by "exactly as before." The `/auth/me` *success*
body is not: it now always carries an `"anonymous"` boolean (`false` for a
real session, `true` only for the policy-granted floor), on every deployment,
whether or not `web_gui_anonymous_scopes` is ever set. That is a response-shape
change, stated here because an earlier draft of this document implied more
than that — the only consumer is the bundled Mission Control page, and it now
reads the field, but a caller checking `/auth/me`'s exact success body against
an old copy would see the difference.

**The cost, stated in full, exactly as it was stated to the operator before
they decided.** Setting this key means:

1. **Anyone who can reach the listener** — on this deployment,
   `192.168.0.198:8799`, `tls_mode = NONE`, no origin restricted — **reads
   every scope named here with no credential at all.** On a cleartext LAN
   listener, "anyone who can reach the listener" means anyone on the network
   segment: every decision, every claim, repository file contents and the
   whole structural and semantic graph that the named scopes cover, to any
   device on that network, unauthenticated. **This sentence was, briefly,
   narrower than the actual exposure** — see "The `Host` check" below — and is
   accurate again now that the remedy described there is in place.
2. **The audit trail stops saying who.** A `gw_audit` row for an anonymous
   read carries the fixed identity `key_id = "anonymous"` (see `gateway.c`'s
   `GW_ANON_KEY_ID`) rather than a credential's selector, because there is no
   credential to name. The trail still says a read happened, on which route,
   and when — it no longer says which person or process made it, because
   nothing this deployment can observe distinguishes one anonymous reader from
   another.
3. **This is a decision about a machine and a network, not a code change
   Atlas is recommending.** Atlas states the cost; it does not judge the
   trade. An operator who controls both the listener's address and everything
   on its network segment may reasonably decide the convenience is worth it on
   that specific machine — that is exactly the decision made here, on
   2026-09-04, after the cost above was read in full — and the same setting on
   a listener reachable from an untrusted network is a different decision with
   the same mechanism and a much worse outcome.

Mission Control's Audit view (`/api/v1/audit`, `ATLAS_SCOPE_AUDIT_READ`) will
answer 403 to an anonymous reader unless `audit:read` is named explicitly. That
is the honest consequence of the paragraph above, not a defect: an anonymous
reader who cannot be distinguished from any other anonymous reader is not a
reader Atlas will show the audit trail to by default.

### The `Host` check: DNS rebinding was sufficient without it

**Found by adversarial review, same day as the feature, and closed the same
day: a LAN user merely *visiting a hostile web page* — no network position, no
credential, nothing beyond ordinary browsing — was sufficient to read
everything the anonymous floor grants**, which is a materially wider exposure
than "anyone who can reach the listener" describes. The chain: an attacker
serves a page from a name with a short DNS TTL; once a LAN visitor's browser
has loaded it, the attacker's DNS answers that same name with the gateway's
own address; the page's own script then fetches `/api/v1/...` against that
name. A browser compares origins as (scheme, host, port) *strings*, so it
treats the second load as same-origin with the first — it attaches no `Origin`
header (so no CORS check ever runs), and no `atlas_session` cookie (none was
ever set for the attacker's name, only for the gateway's real one). The
request therefore presents nothing, and "presents nothing" is exactly what the
anonymous floor was built to accept. Before the floor existed, the same
rebound request still hit `/api/`'s ordinary 401: the credential it needed was
a cookie, and a cookie belongs to the gateway's own name, never to the
attacker's. Replacing "must present a credential" with "must present nothing"
is what made the rebinding worth doing, and it is the anonymous floor's own
addition, not a pre-existing gap.

**The remedy: `host_matches_listener` (`src/gw/gateway.c`), one clause in
`anonymous_ok`.** The request's `Host` header must equal the policy's own
`listen_addr` and `listen_port`, compared whole and case-insensitively, never
by prefix or suffix — the rule an Origin already follows here, for the same
reason a suffix match on a hostname is how `atlas.example.com.attacker.net`
gets mistaken for `atlas.example.com`. A client may omit the port when it
equals the scheme's default; since Atlas terminates no TLS, that can only be
port 80, and only when the policy is in fact bound to it — on a typical
deployment (e.g. port 8799) the clause never fires. **A request with no `Host`
at all is refused the floor**, not guessed at: an HTTP/1.0 client, or an
oversized header Atlas' own parser declined to store, both leave the field
empty, and an absent input must never read as a match.

**What this is not.** It is not an authorisation boundary, and the operator's
decision above is unchanged by it: the scopes named in `web_gui_anonymous_scopes`
are still granted to anyone who can reach the listener with the right `Host`,
which on this deployment is still anyone on the network segment — nothing
about *who* may read is narrower now. What the check restores is a *narrower
equivalence* than "authenticated": that "can reach this listener" and "can
read this data" mean the same set of requests for browser-mediated access,
which is the sentence the operator actually authorised on 2026-09-04 and the
one the rebinding path had quietly widened. A request carrying a real
credential — a live session or a bearer token — is judged on that credential
exactly as before, on any `Host`, because a session cookie could only ever
have been issued by this gateway's own `/auth/login` in the first place.

No policy key names additional accepted hostnames (for a reverse proxy or a
DNS name in front of the gateway); this deployment reaches it by raw address,
and adding a second, broader-matching source of truth during a security fix
was judged not worth the extra surface. An operator who later needs one can
add it following the same discipline as every other key here: root-owned,
absent by default, compared whole. Until then, a `Host` that names anything
else is an ordinary 401 refusal of the floor — never a crash, a bypass, or a
confusing error — and a request with a real credential is unaffected either
way.

## Security controls

- Bearer credential only. Never a query parameter, never a path parameter, never
  a cookie for the MCP surface.
- Constant-time verifier comparison; one outward answer for every authentication
  failure, so a caller cannot learn which half of a guess was right.
- `Content-Length` checked against the ceiling before a byte of body is read.
- `Transfer-Encoding` refused outright; duplicate `Content-Length` refused even
  when the two agree.
- Only `CRLFCRLF` ends a request head, so a bare-LF terminator times out rather
  than being interpreted.
- No generic header map — a header Atlas does not act on has nowhere to be
  stored.
- Paths are never decoded and never joined to a filesystem path; an encoded
  traversal is a route that matches nothing.
- Bounded concurrency, bounded request and response sizes, read deadlines.
- Origin allowlist compared whole; no CORS header at all for an unlisted origin.
- Security headers on every response, errors included.
- No route becomes a socket message unless it matched the fixed route table.
- No shell, no process creation, no filesystem write path anywhere in the
  gateway.

## Recovery and disabling

To stop remote access immediately:

```sh
# Revoke the credential (takes effect on the next request)
atlas api-key revoke key_<id>

# Or stop the gateway entirely
sudo systemctl stop atlas-gateway
```

Setting `enabled = no` in the policy prevents it starting again. Neither affects
`atlasd`, the local socket, stdio MCP or the Claude integration.

## Concurrency

The gateway is one thread per connection, capped by `max_concurrent`. Three
things are shared between those threads and each is either locked or absent:

- the rate-limit window is behind a mutex — without it the limit is not merely
  approximate, it is a data race on a counter and the bound it appears to
  enforce is not enforced;
- the browser session table is behind a mutex, and lookups use a constant-time
  comparison for the reason credential verification does;
- there is deliberately **no shared `atlas_safe_pool`**. A pool is a ring of
  scratch buffers with a mutable cursor, so one shared between threads does not
  merely race — it hands two threads the same slot, and a value encoded by one
  appears in the other's log line. Every call site declares its own on the
  stack, as every other pool in Atlas does.

The stop flag is an `atomic_bool` rather than a `volatile bool`: `volatile`
orders nothing between threads and promises nothing about tearing.

`tests/test_gw_remote.c` drives six concurrent clients through a real socket,
mixing authenticated and rejected requests so the logging and audit paths run
together. It is worth little without ThreadSanitizer and everything with it.

## Known limitations

- Atlas terminates no TLS (above).
- Rate limiting is global rather than per-peer behind a proxy (above).
- The gateway's isolation guarantee holds only under a separated A7.1 deployment
  (above).
- `SQLite` has no per-page checksum, so A5's statement about what backup
  verification cannot detect is unchanged by A9.
- The audit trail records that a request happened, not what it returned.
- Rate limiting is a fixed window rather than a leaky bucket, so a burst at a
  window boundary can briefly exceed the nominal rate.
- Browser sessions are held in gateway memory: a restart ends every session, and
  there is no way to enumerate or revoke one other than restarting.
