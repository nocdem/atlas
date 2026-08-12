# The Atlas product surface, classified

Every operation Atlas exposes, in one place, with the authority each one needs.

This document exists because "is Atlas safe?" cannot be answered by reading a
rule; it is answered by reading a list and checking that nothing on it is in the
wrong column. The list is generated from the source — `server*.c` method tables,
`TOOLS[]` in `src/mcp/mcp_tools.c`, `HOOK_EVENTS[]` in `src/hook/hook.c`, and
`COMMANDS[]` in `src/cli/cli.c` — and the classification is the one the code
actually enforces, not an intention.

Six classes:

| class | meaning |
|---|---|
| **global read** | Reads state that is not scoped to a repository. |
| **repo read** | Reads one repository's records. Scoped by a registered identity. |
| **repo mutation** | Changes the registry. Operator only, and **absent from the protocol**. |
| **index mutation** | Writes the index. Requires the writer lock. |
| **lifecycle mutation** | Changes a decision's state. Requires an operator capability. |
| **administrative** | Backup, restore, prune, service units. |

## A9 additions

Six methods and two surfaces. The classification below is unchanged for
everything that existed before; nothing was reclassified.

| operation | class | authority |
|---|---|---|
| `apikey.create` | administrative | operator uid, or the daemon's own uid in legacy mode |
| `apikey.list` | administrative | same |
| `apikey.revoke` | administrative | same |
| `gateway.auth` | global read | the `gateway_uid` a root-owned policy names |
| `gateway.audit` | index mutation | same — one append-only row, queued to the writer |
| `gateway.audit_list` | global read | same |

The credential methods are gated by their own predicate rather than by
`atlas_server_peer_is_operator`; see `src/ipc/server_apikey.c` for why, and note
that the gateway's uid is not among them. The gateway group is disjoint from
every other group and is hidden the way the dispatcher group is: a peer the
policy does not name gets `unknown method`.

**The remote surfaces add no operation.** Remote MCP exposes the same
`TOOLS[]` the stdio adapter does, filtered by the credential's scopes, and the
web API's 23 routes each forward to a daemon method already in the table above.
A client never names an Atlas method.

## The four A9 absences that carry its guarantees

1. **No remote credential administration.** No MCP tool and no gateway route
   creates, lists, rotates or revokes a credential, or changes its own scopes.
   Absent, not refused.
2. **No operation anywhere returns the plaintext of an existing credential.**
   The index holds a one-way verifier and there is no column to read one from,
   so there is no `apikey.show`, `apikey.reveal` or `apikey.export` — and their
   absence is a property of the storage rather than a refusal.
3. **No write scope is grantable.** `memory:write` exists so every recording
   tool maps to it, and no operator can grant it in A9.
4. **No route reaches the socket unless it matched the fixed table.** The web
   API's parameter allowlist is per route; anything else in a query string is
   ignored rather than forwarded.

## The three absences that carry the guarantees

1. **There is no `repo.add`, `repo.ensure` or `repo.remove` RPC method.** They
   were deleted rather than left refusing: an absent method is answered by the
   dispatcher's unknown-method case, and a refusing one is a refusal a later
   edit can weaken. Registration is a local operator command and nothing else.
2. **There is no index-construction MCP tool, and no index-construction method
   in the ordinary group.** Building an index runs a compiler over repository
   source. A model holding every tool in the list below cannot cause a compiler
   to run.

   `code.index` exists in the **operator-uid** group, added by the A8-CI
   closeout. Without it the only way to reindex under A7.1 was to stop
   `atlas.service` and run the command as the service account — a documented
   workaround standing in for a missing feature, which asked an operator to do
   by hand the one thing the separation exists to prevent. It is offered only to
   the peer the root-owned policy names; `atlas-worker` and every MCP client get
   `unknown method`.
3. **There is no restore method, and no prune method in the ordinary group.** A
   model that could call every method in that group still cannot replace or
   prune the index — `tests/test_backup_live.c` asks a live daemon for each name
   such a method would plausibly have and requires every one to fail.

   `maintenance.plan` and `maintenance.prune` exist in the **operator-uid**
   group, added by the A8-CI closeout for the reason `backup.create` was: A5
   gave maintenance no RPC surface on the premise that whoever owns the data
   directory can prune it anyway, and A7.1 broke that premise — under a system
   deployment the operator account could neither plan nor prune without becoming
   the service account, which is manual impersonation standing in for a missing
   feature. What A5 wanted, that nothing a model can reach may prune the index,
   is unchanged. `--apply` is still required, the delete is still per batch, and
   there is still no background deleter.

## Long operations

`backup create`, `backup verify` and `code index` can outlast a client's
patience: an 815 MiB backup takes 32 s to write and 15 s to verify, and a full
semantic index of a real repository was measured at 144 s. All three are
**accepted, then polled**.

Verification is in the list because it reads every page — `PRAGMA
integrity_check` walks the b-trees and every decision revision is rehashed.
Converting `create` and leaving `verify` inline simply moved the timeout to the
next command, and did: an 815 MiB backup verified fine on the daemon while the
operator was told "timed out while reading a frame header". The request returns an
`operation_id` as soon as the work is queued; `operation.get` reports its state,
and a record that has reached SUCCEEDED or FAILED never changes again, so
polling is idempotent.

Neither runs in the serve loop, so ordinary reads keep being answered
throughout — measured at 25 ms during a backup that previously blocked
everything for its whole duration. The work holds no reference to the
connection, so a client that disconnects or is killed neither cancels nor
corrupts it.

Operation ids are not small counters: each daemon seeds them so that every id it
issues is above every id any previous daemon issued. Without that the counter
restarted at 1 and an id from before a restart named a *different* operation
afterwards — a client polling it was handed another operation's verdict, which
is a confident wrong answer rather than the "unknown" the contract promises.

Operation records live in the daemon's memory and a restart forgets them. That
is deliberate: a backup publishes atomically or leaves nothing, and a semantic
generation publishes atomically or leaves a RUNNING one that nothing points at
while the last valid one is still served. An unknown id is reported as unknown
and the message points at the artefact, which is what survives.

## Daemon RPC — 73 methods

### Ordinary group — served to any peer the socket policy admits

| method | class |
|---|---|
| `daemon.ping`, `daemon.status` | global read |
| `repo.list` | global read — **the registry, and the resolver every surface shares** |
| `repo.resolve` | global read (reports; registers nothing) |
| `repo.state`, `repo.sync`, `repo.file`, `repo.history`, `repo.search` | repo read |
| `events.since` | repo read |
| `code.status`, `code.file`, `code.symbol`, `code.symbol.search`, `code.deps`, `code.impact` | repo read (lexical) |
| `code.sync` | index mutation — queued to the writer thread |
| `sem.status`, `sem.symbol`, `sem.graph`, `sem.trace`, `sem.impact`, `sem.context` | repo read (compiler-derived) |
| `ai.session.*` (7), `ai.tool.record`, `ai.batch.correlate`, `ai.turn.close`, `ai.reason.record`, `ai.decision.record`, `ai.changed`, `ai.context`, `ai.file.context`, `ai.memory.search` | repo read / A2 session writes, restricted to `MODEL_PROPOSAL`, `MODEL_INFERENCE` and `UNKNOWN` |
| `decision.list`, `decision.get`, `decision.history`, `decision.links`, `decision.orphaned`, `decision.legacy` | repo read |
| `decision.propose`, `decision.revise`, `decision.link_add`, `decision.link_remove`, `decision.edge.note`, `decision.promote` | proposal writes — **cannot approve anything** |
| `gate.check` | repo read |
| `job.submit`, `job.get`, `job.list`, `job.cancel`, `job.artifact` | orchestration, client group |

### Operator-uid group — `SO_PEERCRED` must equal the root-owned policy's `operator_uid`

| method | class |
|---|---|
| `decision.challenge`, `decision.approve`, `decision.reject`, `decision.supersede`, `decision.revalidate` | **lifecycle mutation** |
| `backup.create`, `backup.verify` | administrative — both long operations |
| `operation.get` | global read — the state of one long operation |
| `code.index` | **index mutation** — queued to the writer thread |
| `maintenance.plan` | global read — what a prune would remove; writes nothing |
| `maintenance.prune` | **index mutation** — the one bounded delete, on the writer thread |

A peer outside this group receives `unknown method` — the same answer as a name
that does not exist. That is deliberate: a refusal distinguishing "you may not"
from "there is no such thing" tells a caller what to try next.

### Dispatcher group — the `atlas-worker` account only

`dispatch.lease`, `dispatch.heartbeat`, `dispatch.event`, `dispatch.complete`,
`dispatch.snapshot.open`, `dispatch.snapshot.chunk` — disjoint from the client
group rather than nested inside it.

## MCP tools — 28, every one a read or a proposal

Each carries a scope in `tool_def`. A remote credential sees and can call only
the tools its scopes permit; the four that record something durable map to
`memory:write`, which no A9 credential can hold. The stdio adapter is
unaffected — a local Claude session is authorised by an operator having
installed the plugin, which is A2's boundary and is unchanged.


Repository context, files, search, history, decisions, gate checks, and the
seven A8-CI tools: `atlas_sem_status`, `atlas_sem_symbol`, `atlas_sem_callers`,
`atlas_sem_callees`, `atlas_sem_trace`, `atlas_sem_impact`,
`atlas_context_build`.

No tool accepts an absolute path. No tool name contains an approval or
revalidation verb, and no schema declares a `token` or `confirmation` argument —
`tests/test_decision_mcp.c` asserts the whole inventory and rejects any name
containing an approval verb. The count is pinned in `tests/test_plugin.c` so a
tool appearing or vanishing is a deliberate change.

## Hooks — 19 events, all metadata-only

Every hook returns valid JSON and exits 0 whatever happened. No hook emits
`decision`, `continue` or a permission verdict, which is what makes a Stop loop
structurally impossible rather than guarded against. `tests/test_plugin.c`
asserts the plugin's configured events and the binary's handled events match
exactly.

## CLI — the operator surface

| command | class |
|---|---|
| `doctor`, `version`, `help`, `daemon status/ping` | global read |
| `repo list` | global read |
| `status`, `search`, `file`, `history`, `diff`, `events`, `code *`, `context build`, `gate *`, `decision list/show/search/links/history/orphaned` | repo read |
| `repo add`, `repo remove` | **repo mutation** — local only, no RPC form |
| `scan`, `sync`, `code sync`, `code index` | **index mutation** — needs the writer lock |
| `decision approve/reject/supersede/revalidate` | **lifecycle mutation** — operator capability |
| `backup create/verify/restore`, `maintenance plan/prune`, `service install` | administrative |
| `operation status ID` | global read — the state of a long operation |
| `mcp`, `hook`, `dispatcher run`, `sem-parse` | adapters — not operator commands |

`sem-parse` is Atlas' protocol with its own child process: it opens no context,
takes no lock and can write nothing.

## The four refusals a caller must be able to tell apart

| answer | meaning | what to do |
|---|---|---|
| `NOT_REGISTERED` | the name is not in the registry — the same answer whether the directory exists, is a git repository, or is nothing | register it |
| absent index | registered; no semantic index was ever built | build one |
| stale index | an index exists and no longer describes the code | rebuild |
| `NOT_AUTHORIZED` | the operation is visible and this caller may not perform it | use the authorised path |

`NOT_REGISTERED` and `NOT_AUTHORIZED` are stable tokens rather than prose, so a
caller can distinguish them without reading English. Every surface emits them:
the MCP resolver, the daemon's method dispatch and the service layer produce
these messages independently, so a token only one of them carried would not be
a shared contract. `tests/test_registry.c::the refusals are deterministically
distinct` asks the CLI and MCP separately and asserts all four are told apart —
including that an *existing but unregistered* directory answers exactly as an
unknown name does. The difference between "there is nothing there" and "there
is a git repository there that nobody registered" is not something a caller
learns by asking; otherwise the refusal becomes a filesystem probe.

The tokens are deliberately **absent** from the operator-uid group, for the
reason given above: there, the correct answer is that the method does not exist.
