# The Claude Code integration

A2's goal, stated as a test somebody could run: after a one-time setup, a person
works normally in a git repository and never types an `atlas` command, and
afterwards Atlas can say which files changed, which session changed them, and
either why or that nobody said why.

Everything below serves that. Atlas remains provider-neutral — nothing in
`src/ai`, `src/db` or the schema names Claude — and the Claude-specific part is
one plugin directory plus two `#define`s in `src/hook/hook.c`.

## The two halves

**Hooks** are deterministic participation. They fire whether or not the model
thinks to do anything, which is what makes the memory reliable rather than
dependent on the model remembering.

**MCP** is deeper query and deliberate recording. It fires when the model decides
it needs something, which is what keeps the automatic half small.

Splitting it this way is the point. A design that put everything in MCP would
record nothing when the model did not think to call it; one that put everything in
hooks would either inject a lot of context nobody asked for, or record a lot and
explain none of it.

## Components

```
integrations/claude/atlas/
├── .claude-plugin/plugin.json     the manifest
├── hooks/hooks.json               15 lifecycle hooks
├── .mcp.json                      the `memory` stdio server
├── bin/atlas-hook                 launcher, fails open
├── bin/atlas-mcp                  launcher, fails loudly
├── bin/atlas-resolve.sh           finds the atlas executable
├── skills/atlas-memory/SKILL.md   when to query, when to record
└── README.md
```

### Finding the Atlas executable

This is the one genuinely awkward problem, and it is worth stating because the
obvious solutions all fail.

Claude copies an installed plugin into a cache directory whose path changes on
every update. So:

- a relative path from the plugin to the binary is not stable;
- a symlink planted next to the plugin does not survive being re-copied;
- committing a compiled binary into the plugin would put a build artefact in a
  source tree and would be wrong for every platform but one.

The launcher therefore searches, cheaply, on every invocation:

1. `$ATLAS_BIN` — an explicit override, for tests and odd layouts
2. `atlas_executable=` in `$XDG_CONFIG_HOME/atlas/claude-integration.conf`,
   written by `atlas integrate claude install --user` and living in the user's own
   config directory where nothing else rewrites it
3. `PATH` — the ordinary case after `make install`
4. `~/.local/bin/atlas`, `/usr/local/bin/atlas`, `/usr/bin/atlas` — for a desktop
   session that did not inherit a shell's `PATH`, which is common when Claude is
   launched from a GUI

The config file is read with parameter expansion only. A config file the shell is
asked to interpret is a config file that runs whatever anyone can write into it.

### The two launchers fail differently, on purpose

`atlas-hook` **fails open**: no Atlas, no daemon, a broken exec — it prints `{}`,
exits 0, and the session is unaffected. A memory system that can break somebody's
editing session is a memory system they will turn off.

`atlas-mcp` **fails loudly**: it exits non-zero with an explanation. An MCP server
that accepts a connection and answers nothing looks to a user like Atlas working
badly rather than Atlas being absent.

Neither prints anything on stdout before exec. For the MCP launcher that is a
protocol requirement: one stray byte on stdout ends the session.

## Hooks

15 events, all `type: command` in exec form (`args` present), so no shell
tokenises anything and a path containing a space cannot become two arguments.

| event | what Atlas does | injects context |
| --- | --- | --- |
| `SessionStart` | ensure the repository is registered, open or resume the session, answer with the envelope | **yes** |
| `UserPromptSubmit` | count a turn | no |
| `PreToolUse` | record an edit intent and at most one path | no |
| `PostToolUse` | record that the tool reported success | no |
| `PostToolUseFailure` | record that it reported failure | no |
| `PostToolBatch` | ask for an incremental pass, correlate changed paths | no |
| `Stop` | record UNKNOWN for every changed path nobody explained | no |
| `PreCompact` / `PostCompact` | checkpoint bounded counters | no |
| `SessionEnd` | close the session | no |
| `CwdChanged` / `DirectoryAdded` | ensure the newly named repository is indexed, then attach it to the session | no |
| `SubagentStart` / `SubagentStop` | open and close a child session | no |
| `WorktreeRemove` | observed only | no |

### Why SessionStart is the only one that injects

It is the one event whose output contract carries `additionalContext` *and* which
Claude re-delivers on a resume and after a compaction, with `source` saying
which. So the restore path after compaction is the same code as the initial
injection.

`PostCompact` has no `additionalContext` in its output contract. Returning one
would be silently ignored, which is the worst kind of wrong: it reads correctly
and does nothing. `PostCompact` writes its checkpoint and returns `{}`.

### Why `WorktreeCreate` is deliberately not hooked

Configuring it *replaces* Claude's own worktree creation with whatever the hook
prints. Where a worktree lives is not Atlas' decision. Worktree changes are
observed through `CwdChanged` and `DirectoryAdded`, which report the same fact
without taking over the mechanism.

### Stop cannot loop

Atlas never emits `decision: block` and never exits 2 from any hook. That is
structural — no code path emits a blocking document — so there is no state in
which a stop loop is possible. `stop_hook_active` is honoured as a courtesy
rather than relied on, and the test asserts no hook output contains a decision.

What `Stop` does instead is close the turn: every changed path with no recorded
reason gets an explicit `UNKNOWN` record, keyed on the turn so a redelivery adds
nothing. Atlas does not ask the model to explain itself and does not invent an
explanation.

### Why the turn close re-correlates

`PostToolBatch` queues a reconciliation it cannot wait for — the pass is a job
behind it on the same writer thread, so waiting would be the writer waiting on
itself. It therefore correlates against the snapshot as it was *before* the
edits.

By the time `Stop` arrives, the watcher-triggered pass has normally published, so
the sweep runs again there. Running it twice is cheap and idempotent; running it
only at batch time would report zero changed paths for the common single-batch
turn, and `Stop` would find nothing to mark `UNKNOWN` — which would quietly defeat
the one thing it exists to do.

## MCP

Sixteen tools. Thirteen read the index; three record what a model wants
remembered. Ten came with A2 and six with A3.

| tool | reads | provenance of the result |
| --- | --- | --- |
| `atlas_status` | daemon and index state | `ATLAS_OWNED` |
| `atlas_repo_overview` | identity, HEAD, freshness, counts | `SOURCE` |
| `atlas_changed_files` | working-tree changes by git scope | `SOURCE` |
| `atlas_file_context` | indexed facts, history, recorded reasons | `GIT` |
| `atlas_search` | paths and commit messages | `GIT` |
| `atlas_memory_search` | recorded reasons and decisions | `MODEL_PROPOSAL` |
| `atlas_session_state` | the current change session | `ATLAS_OWNED` |
| `atlas_record_reason` | — | records `MODEL_PROPOSAL` |
| `atlas_record_unknown_reason` | — | records `UNKNOWN` |
| `atlas_record_decision` | — | records `MODEL_PROPOSAL` |
| `atlas_code_status` | structural currency, generation and counts | `ATLAS_OWNED` |
| `atlas_code_symbol_search` | symbol sites matching a name fragment | `SOURCE` |
| `atlas_code_symbol` | every recorded site of a name, with callers and callees | `SOURCE` |
| `atlas_code_file` | roles, symbols, includes, call candidates, dependents | `SOURCE` |
| `atlas_code_dependencies` | bounded traversal outward or inward | `INFERENCE` |
| `atlas_code_impact` | inbound candidates, split by resolution class | `INFERENCE` |

The six structural tools carry `untrusted_data: true` like every other tool that
can return repository prose, and for the same reason: a symbol name and an
include spelling are chosen by whoever wrote the repository. What is new is that
each result also states the **resolution class** of every fact in it, and that
`atlas_code_impact` says in its own description that its results are candidates
to review rather than proof of breakage. A model that reads them as a compiler's
answer has been told otherwise in the payload.

`atlas_file_context` was **extended rather than duplicated**: it gained a
`structure` object with the file's roles, symbol and include counts, and
structural currency, so the tool a session already calls answers the structural
question too. A second tool covering the same file would have been a second
answer to maintain.

Every schema sets `additionalProperties: false`. A tool that silently accepts an
argument it does not implement lets a caller believe it asked for something.

Every result carries an Atlas envelope: version, `ok`, `degraded`, `provenance`,
`untrusted_data`, and a fixed notice when the answer can carry repository prose.
Results are offered as both a text block and `structuredContent`, built once and
serialised once so the two cannot disagree.

### Which session an MCP write belongs to

The MCP server is a separate process from the hooks. What connects them is
`CLAUDE_CODE_SESSION_ID`: Claude Code supplies it both to command hooks and to
stdio MCP server processes, and it is the same string a hook payload carries as
`session_id`. Both Atlas adapters send the same `provider` (`anthropic`) and
`client` (`claude-code`), so a hook event and an MCP write reach the same Atlas
session exactly when they carry the same external id.

`atlas mcp` reads the variable once at startup and validates it: 1 to 128 bytes
of `[A-Za-z0-9._:-]`, which fits the UUID Claude Code uses. Anything else is
refused and reported on stderr, without echoing the value. Two exclusions are
deliberate rather than tidiness — `/` is how a subagent key is spelled
(`<session>/<agent>`), and any byte the daemon's safe encoding would rewrite
would arrive as a different string from the one the hooks send. An over-long id
is refused, never truncated: a truncated id is a different id, and the one it
collides with is somebody else's.

**A write attaches to that session or to none.** There is no fallback. Atlas
does not attach a record to the newest session, the only session, or any session
selected by the repository — a repository is not an identifier for a session, and
with two Claude Code windows open on one worktree it identifies the wrong one
about half the time. When Atlas cannot resolve the session exactly, the record is
still stored, `session_unbound` is true, and `unbound_reason` says which case it
was: `no_session_id`, `unknown_session` or `session_closed`. The tool result also
carries a plain-language `attribution` line saying the same thing.

Missing attribution can be repaired later by a person who remembers. Wrong
attribution cannot be repaired at all, because nothing about the row says it is
wrong.

**A generic MCP client** — anything that is not Claude Code — has no
`CLAUDE_CODE_SESSION_ID`, so every record it makes is stored sessionless. It is
fully supported: it can read everything, it can register a granted root, and its
records are kept. What it does not get is a session it did not open. A client
that wants its writes attributed has to have an Atlas session created for its
connection and identify itself with that session's id.

**`claude --resume <id>`** needs nothing special. The hooks reopen the same
external id, `ai.session.open` resumes the existing row rather than replacing it,
and an MCP server started for the resumed conversation carries the same id — so
its writes attach to the same session as before the interruption, keeping the
change set.

**`/clear` is the case Atlas refuses to guess at.** The hooks receive a *new*
session id, while an MCP server process that is already running keeps the id it
was spawned with. Atlas has no way to learn the new id from inside the MCP
process — the environment of a running process is fixed, and there is no
documented Claude Code mechanism that tells a running MCP server its session
changed. So it does not try. What happens depends on one thing:

- **If `SessionEnd` reached Atlas** (it fires with reason `clear`), the old
  session is closed, and a write carrying its id is stored with
  `unbound_reason: session_closed`. It is not rebound to the new conversation and
  it is not recorded against the cleared one.
- **If `SessionEnd` did not reach Atlas** — the daemon was down, the hooks are
  not installed, the event was lost — the old session is still open and the write
  attaches to it. That attribution is exact rather than guessed (the record did
  come through the connection created for that conversation) but it is stale, and
  Atlas has no way to detect that it is.

Atlas does not claim to distinguish the second case; a document that asserted it
did would be describing behaviour that is not implemented. If a conversation must
be attributed exactly across a `/clear`, restart the MCP server, which gives it
the new id.

### Roots are the authorization boundary

After `notifications/initialized`, and only if the client advertised the `roots`
capability, the server sends `roots/list`. The answer is absorbed
asynchronously — blocking on it would mean a server that stops serving while it
waits for permission to serve. `notifications/roots/list_changed` discards
everything cached and asks again, so a revoked root stops authorizing
immediately.

A tool's `repo` argument must name a repository that one of the granted roots
resolved to. It is a **whitelist, not a path comparison**: there is no argument a
caller can construct that reaches a repository the client did not grant. No tool
accepts an absolute path.

The documented fallback when the client advertises no roots is
`CLAUDE_PROJECT_DIR` — deliberately not this process's working directory, which is
wherever Claude happened to launch it from.

### `file:` URI decoding

Every rule is a refusal, because a root decoded permissively authorizes a
directory that merely resembles the right one and does so silently.

| accepted | refused |
| --- | --- |
| empty authority (`file:///p`) | any other authority — a host is not a local path |
| `file://localhost/p` | `file://localhost.evil.example/p` |
| percent-encoded octets, including multi-byte UTF-8 | a truncated or non-hex `%` escape |
| paths containing spaces (`%20`) | a decoded NUL |
| a trailing `/`, which is dropped | `%2F` — an encoded separator Atlas will not guess at |
| | `.` or `..` after decoding, plain or encoded |
| | an empty interior component |
| | `/` itself, which would authorize the filesystem |

### Registering from MCP alone

**An MCP client with no hooks registers repositories by granting roots.** The
first version of this deliberately did not, on the grounds that a mutation
mid-tool-call is a surprise; that made MCP depend on a Claude `SessionStart`
having run first, which contradicts Atlas working with any AI client.

What makes it safe is the bound rather than the absence. `repo.ensure` is called
with the granted root itself and with `exact_root`, which **refuses to resolve
upward**: if the granted root is a subdirectory of a larger worktree, the parent
is *not* registered, because the client did not grant it. Nothing walks the
filesystem, and a refusal is reported with its reason rather than appearing as a
bare "no repository".

The session-start hook does not ask for `exact_root`: a person who launched
Claude in a subdirectory means the worktree, and Claude's own file access already
spans it.

### Protocol versions

Atlas speaks the handshake-based revisions: `2025-11-25`, `2025-06-18`,
`2025-03-26`, `2024-11-05`. A client asking for one of those gets it echoed. A
client asking for anything else — including a `2026-07-28`-style per-request
revision — gets `2025-06-18` and decides for itself whether to continue, which is
what the specification prescribes.

A top-level array is refused. Batching was removed in `2025-06-18`, and answering
one would be inventing behaviour rather than implementing it.

## One-time setup

`atlas integrate claude print` prints these; Atlas runs none of them.

```sh
atlas integrate claude install --user               # writes one config file
atlas service install --user                        # writes a systemd unit
systemctl --user daemon-reload
systemctl --user enable --now atlas                 # starts the daemon
claude plugin marketplace add <marketplace-dir>     # registers the catalog
claude plugin install atlas@atlas-local --scope user
claude plugin list                                  # confirms it
atlas integrate claude doctor                       # checks the Atlas half
```

After that, **ordinary `claude` sessions load Atlas with no flags**. That is the
whole point of installing rather than pointing.

### Why a marketplace, and why a local one

`claude --plugin-dir <dir>` loads a plugin for *one session*. It is the
development command and nothing else; a user who ran only that would find Atlas
absent the next time they opened Claude.

A permanent install comes from a marketplace, so Atlas ships one:

```
integrations/claude/
├── .claude-plugin/marketplace.json   the catalog, name "atlas-local"
└── atlas/                            the plugin, source "./atlas"
```

`make install` copies the whole directory to
`<prefix>/share/atlas/claude-marketplace`, catalog and plugin together, because
the catalog refers to the plugin by a relative source and the two have to keep
their relationship on disk.

It is a **local path** marketplace, so `claude plugin marketplace add` needs no
network and no hosting. Nothing about the mechanism is Atlas-specific: it is the
documented local-marketplace flow, and `claude plugin validate --strict` passes
on both the catalog and the plugin.

### After Claude caches it

`claude plugin install` copies the plugin into
`~/.claude/plugins/cache/atlas-local/atlas/<version>/`, and that path changes on
every update. The launchers inside the copy therefore cannot hold a path back to
the Atlas binary — which is exactly what the integration record in step 1 solves,
and what `scripts/claude-install-test.sh` proves by running the *cached* launcher
with the Atlas binary deliberately absent from `PATH`.

### Removing it

```sh
claude plugin uninstall atlas@atlas-local
claude plugin marketplace remove atlas-local
atlas integrate claude uninstall --user
systemctl --user disable --now atlas
```

All four use the official mechanism for the thing they remove. **The Atlas index
survives every one of them**, and the install test asserts that by registering a
repository first and counting it afterwards.

`atlas integrate claude` never edits `~/.claude` or any Claude-owned file. Claude
owns its configuration and has documented commands for changing it; a tool that
hand-edits another tool's state breaks when that state's format changes, silently
and in somebody else's product.

`atlas integrate claude doctor` checks the plugin files parse, the launchers are
executable, the socket resolves, and — by running the real MCP server over
in-memory streams — that the handshake completes and the tool list is non-empty.
That is a stronger claim than "the files are present", and it needs neither a
daemon nor Claude installed.

It also reports **how Claude loads the plugin**, read from Claude's own
configuration and never written to it:

| `claude_plugin_state` | means | what fixes it |
| --- | --- | --- |
| `installed-enabled` | ordinary sessions load it | nothing; this is the target |
| `installed-disabled` | installed, explicitly turned off | `claude plugin enable atlas` |
| `development` | present on disk, not installed | the one-time setup above |
| `not-installed` | Claude has never been told about it | the one-time setup above |
| `unknown` | Claude's config directory could not be read | investigate; Atlas will not guess |

These are separate because they are fixed by different commands. Telling somebody
to reinstall a plugin they only need to enable wastes their afternoon, and a
diagnostic that collapses the two is the reason they would.

Two more conditions are reported independently, because either can be true in any
of the states above:

- **MCP unavailable** — `mcp_selftest_ok: false`. The server did not complete its
  own handshake, so tool calls would fail even with the plugin loaded.
- **daemon unavailable** — `daemon_reachable: false`. Hooks and MCP will run and
  report degraded state; coding is unaffected.

`doctor` creates nothing: no data directory, no index, no runtime directory, no
socket, no Claude configuration. It reports `index_present: false` on a machine
where Atlas has never run rather than making the statement true by writing one.

## Escape hatches

| variable | effect |
| --- | --- |
| `ATLAS_CLAUDE_DISABLE=1` | every hook returns `{}` immediately, without even a diagnostic |
| `ATLAS_CLAUDE_NO_AUTO_REGISTER=1` | `SessionStart` will not register an unregistered repository |
| `ATLAS_BIN` | overrides which executable the launchers run |
| `ATLAS_CLAUDE_PLUGIN_DIR` | overrides where `integrate` looks for the plugin |

A user should be able to take Atlas out of the loop in one variable rather than by
editing a plugin.


## A4: decisions, automatically

Four MCP tools, and a skill that tells Claude when to reach for them. The user
never has to remember an Atlas command to make Claude remember something.

| tool | returns | writes |
| --- | --- | --- |
| `atlas_decisions` | compact: ids, status, provenance, titles — **no bodies** | no |
| `atlas_decision` | one whole revision, with every link's currency | no |
| `atlas_decision_history` | revisions and the lifecycle timeline | no |
| `atlas_propose_decision` | the new document's id and state | yes, as a proposal |

### Progressive disclosure, and why

The split is not politeness about response size. A decision body is untrusted
prose, and pulling every body in a repository into a model's context because it
asked "are there any decisions about auth?" would put a large amount of somebody
else's text in front of the model for no reason. A caller reads a body when it
has decided it needs that one.

### What Claude does without being asked

- Before changing code a decision may govern: `atlas_decisions` with the path or
  a few words, then `atlas_decision` for anything relevant.
- When an architectural, protocol, security, compatibility or operational choice
  is actually made: `atlas_propose_decision`, with the context, the decision,
  the rationale, the alternatives and the paths.
- For an ordinary edit: `atlas_record_reason`, not a decision. A schema shape, a
  locking rule, a trust boundary, a dependency, a wire format and a
  compatibility promise are decisions; a rename is not.
- When the rationale is not known: say so. An invented rationale reads exactly
  like a real one and nobody will ever check it.

### What the tool surface cannot do, structurally

There is no approval tool, no rejection tool and no supersession tool, and **no
MCP tool schema declares a `token` or a `confirmation`**. Every schema sets
`additionalProperties: false`, so a member no schema declares is a member no
caller can send: the absence is structural rather than guarded.

`tests/test_decision_mcp.c` asserts the whole inventory, rejects any tool name
containing an approval verb, and checks the emitted `tools/list` document for
capability-shaped property names.

**This is a statement about Atlas' surface, not about what a model can do.** An
agent with shell access can run `atlas decision approve` through a
pseudo-terminal, and Atlas cannot tell that from a person. The skill therefore
instructs Claude to give the user the command and not to run it — an
instruction, not a barrier, and described as one. A statement in a conversation
that the user approved something is a fact about the conversation: recorded as a
proposal, never as an approval.

### `atlas_record_decision` bridges into A4

A2's flatter tool still exists, keeps its schema and keeps its response, so a
plugin installed before A4 keeps working. What changed is its **outcome**: a
successful call now also materialises a real A4 decision document, in the same
transaction, through the ordinary promote path — and the response gains a
`decision` member carrying the new id.

Without that, an official client would keep producing records that live only in
the legacy tables and that a user has to promote by hand, which would make the
A4 decision model opt-in rather than default.

- Attribution comes from the request, resolved by exact session key. The
  document is attributed to the session that made the call, or to none — never
  to a neighbour. The A2 row keeps its own attribution and the A4 revision
  points at it.
- Retries are absorbed: the tool sends a content-derived idempotency key scoped
  to the session, so a redelivered call creates neither a second legacy row nor
  a second document.
- A generic MCP client with no session id records sessionlessly, with a typed
  reason.
- The structured `atlas_propose_decision` remains the preferred tool, and the
  skill says so.

Historical A2 rows are unaffected and remain explicitly promotable with
`atlas decision promote`.

### Automatic context

The envelope gains integers and nothing else:

```
decisions_proposed=12 decisions_approved=4 decisions_rejected=1 decisions_superseded=2
decisions_needing_review=1
```

`decisions_approved` reports the real lifecycle state now; it was pinned to zero
for two phases because nothing could produce an approval. No title, no
rationale, no path, no symbol name and no decision id — approval makes no
difference to that rule.

### Hooks

No hook event was added and no hook output contract changed. Hooks still fail
open, still emit no `decision`, `continue` or permission verdict, and still
store metadata rather than content. A decision is proposed through an explicit
tool call, never as a side effect of a lifecycle event.
