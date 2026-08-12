# A9 — remote MCP and the web control plane

Atlas A9 adds a way to reach an Atlas index from off the machine it runs on:
a **gateway** that terminates HTTP, authenticates a bearer credential, checks
scopes, and forwards only explicitly supported operations to `atlasd` over the
ordinary Unix socket.

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
- every `job.` and every `dispatch.` method

Under a separated A7.1 deployment it additionally cannot open the index at all,
because the index is `0700 atlasd`.

So: **a compromised gateway cannot approve a decision, register a repository,
read a backup, run a job, build an index, administer credentials or read the
database.** That is true because of *who it runs as*. No code in the gateway is
what makes it true, and a bug in the gateway cannot make it false.

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
| `decisions:read` | decision documents, history, freshness gates |
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
