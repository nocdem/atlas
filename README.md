# Atlas

Atlas is a headless engineering-memory and repository-intelligence application
written in C17. It indexes Git repositories into a local SQLite database and
answers questions about them from the command line, over a local daemon socket,
through a Model Context Protocol server, or through an authenticated HTTP
gateway — in human-readable text or in stable JSON.

Atlas is generic. It knows nothing about any particular project; a repository
becomes interesting to Atlas only when an operator registers it.

**Atlas never modifies a repository you register.** Every Git command it runs is
checked against a read-only allowlist before the process is created, hooks and
external diff drivers are disabled, and the working tree and index are only ever
read. The test suite hashes the entire repository tree, including `.git`, before
and after every command and requires the digest to be unchanged. See
[docs/git-safety.md](docs/git-safety.md).

## What Atlas does

**Repository index.** Registered Git worktrees are scanned into a local SQLite
index: tracked files with content hashes, commit history with per-commit file
changes, and the complete working-tree change state — staged, unstaged,
untracked and unmerged, each reported separately. SQLite is a rebuildable index;
Git and the repository contents stay authoritative, and every result preserves
its provenance.

**A daemon that keeps the index current.** After a one-time registration,
filesystem and Git changes are detected with inotify and indexed without running
`atlas scan` by hand. A pass over an unchanged repository reads no file content
at all. When Atlas cannot prove it observed every change, it says so rather than
describing the index as current — see
[docs/watcher-consistency.md](docs/watcher-consistency.md).

**Code intelligence, twice, and honestly labelled.** A first-party bounded
lexical C indexer records symbols, includes and call candidates, with a
resolution class on every fact — `identifier(` is a call *candidate*, two
definitions of one name stay `AMBIGUOUS`, and Atlas never picks between them.
Beside it, an optional semantic index built with libclang records what the
compiler proved: `PROVEN` means exactly that, a call through a function pointer
is capped at `CANDIDATE`, and a path is as strong as its weakest edge. The two
layers never merge and neither promotes the other. See
[docs/code-intelligence.md](docs/code-intelligence.md).

**Engineering memory.** Durable knowledge records with immutable revisions, an
append-only lifecycle ledger, and links to the code they concern. A record says
three orthogonal things, and no code path derives one from another: what sort of
knowledge it is (`DECISION`, `POLICY`, `INVARIANT`, `OPERATIONAL_FACT`,
`ACCEPTED_RISK`, `OBLIGATION`, `PARKED`, `REJECTED_ALTERNATIVE`), how far
through the approval workflow it got (`PROPOSED`, `APPROVED`, `REJECTED`,
`SUPERSEDED`, `RESOLVED`), and what evidence bears on whether it holds. A fourth
axis says whether the thing the record is about is actually **there** —
`PRESENT`, `ABSENT`, `UNKNOWN` or `NOT_VERIFIABLE`. Asked
why something changed with nothing recorded, Atlas answers `UNKNOWN` — it never
invents a historical reason. See
[docs/decision-lifecycle.md](docs/decision-lifecycle.md) and
[docs/verification.md](docs/verification.md).

**Impact gates.** Every approved decision can be assessed against one exact
repository state: `FRESH`, `STALE`, `IMPACTED` or `UNKNOWN`, folding to `PASS`,
`REVIEW_REQUIRED` or `BLOCKED` with distinct exit codes an automation can act
on. `STALE` means the anchors moved and a human has to look again; it does not
mean the decision is wrong. `UNKNOWN` fails closed. See
[docs/impact-gates.md](docs/impact-gates.md).

**AI integration with an implemented trust boundary.** Claude Code hooks and an
MCP server let a model query the index and record reasons and decisions — as
proposals, never as approvals. Automatic model context contains no repository
prose at all, and repository prose reaches a model only through an explicit tool
call labelled `untrusted_data: true`. See
[docs/ai-trust-boundary.md](docs/ai-trust-boundary.md) and
[docs/claude-integration.md](docs/claude-integration.md).

**Orchestration.** A durable job queue with an explicit state machine, expiring
leases, crash recovery, an unprivileged dispatcher, isolated per-attempt
workspaces and bounded command execution. A completed job is not an authority:
its patch is an artifact with a recorded digest, and no code path applies it to
a registered repository. On top of it, an operator-started run loop: `atlas job
run` starts one worker in a registered repository's own tree, checks the pinned
commit before and after, runs the declared verification gates itself, answers a
failure with exactly one follow-up task, and settles the run `ACCEPTED` or
`BLOCKED` within a bound of three worker starts. A run may hold up to a
configurable number of tasks active at once (`--parallel`, default 1, ceiling
8); parallel siblings are workspace tasks under the existing isolation, **at
most one active task per run ever works in the repository's own tree**, and a
run settles only when every task in it is terminal. See
[docs/orchestration.md](docs/orchestration.md).

**Remote access.** An HTTP gateway that authenticates a bearer credential,
checks scopes and forwards only explicitly supported reads to the daemon;
remote MCP over the same tool implementations the stdio adapter uses; a
versioned read-only web API; and an embedded Mission Control page. Credentials
are minted locally by the operator and the plaintext is shown once — after
that, no copy exists anywhere. **Atlas terminates no TLS**; it is designed to
sit behind a reverse proxy that does. No remote caller can create, rotate or
revoke a credential, and no remote credential can hold a write scope. See
[docs/remote-access.md](docs/remote-access.md).

**Operations.** Verified online backups taken through SQLite's backup API while
the daemon keeps writing, an atomic restore that keeps what it displaced, and a
written retention policy for every table in the schema. None of it is reachable
from a model. See [docs/operations.md](docs/operations.md).

## Status

Atlas is at phase **A11.6**. Each phase built on the last and none removed a
guarantee:

| Phase | What it added |
| --- | --- |
| A0 | the read-only foundation: registration, scanning, history, search |
| A1 | the daemon: inotify watching, incremental indexing, local IPC |
| A2 | AI integration: hooks, MCP, sessions, the model-context trust boundary |
| A3 | lexical code intelligence with resolution classes |
| A4 | decision documents, immutable revisions, the operator approval channel |
| A5 | verified backups, atomic restore, the written retention policy |
| A6 | deterministic impact gates and stale-decision detection |
| A7 | trust-boundary hardening; the model-facing surface reduced to reads and proposals |
| A7.1 | OS authority separation: the daemon, the worker and the operator as distinct principals |
| A8 | the orchestration control plane: jobs, leases, dispatcher, workspaces |
| A8-CI | the semantic index (libclang), the proven call graph, task context |
| A9 | secure remote access: gateway, API keys, remote MCP, web API, GUI |
| A9.1 | knowledge kinds and the `RESOLVED` lifecycle state |
| A9.2 | evidence, verification and policy-authorised automatic lifecycle |
| A9.2.1 | the verification intake surface and the channel ceiling |
| A9.2.2 | epistemic absence: the truth axis, first-class coverage, and the absence-proof rule |
| A9.2.3 | semantic freshness and coverage the daemon maintains itself |
| A9.2.4 | build-input discovery, and an activation policy that does not depend on memory |
| A9.2.5 | semantic trust closure: every load-bearing semantic answer carries its verdict |
| A9.2.6 | daemon responsiveness: a waiter that can stop waiting, and the `BUSY` refusal |
| O10 | production evidence ingestion proved at the boundary a client reaches |
| A11.0 | the durable run a chain of tasks belongs to; a parent that resolves |
| A11.1–A11.4 | the foreground run driver, gates Atlas runs itself, one follow-up per failure, the bound |
| A10.0 | what a worker attempt cost, per attempt and never estimated |
| A10.1 | the bounded cross-run memory package, measured in an A/B experiment |
| A11.6 | bounded parallel tasks in a run; the repository's tree kept exclusive, settlement deferred to quiescence |

## Requirements

- a C17 compiler (GCC or Clang)
- CMake 3.16 or newer, and Make
- pkg-config
- SQLite3 development headers and library
- Git (resolved from `PATH` at runtime, not baked in at build time)
- optionally libclang, for the semantic index — a build without it still
  indexes, answers decisions and serves every other command, and reports the
  absence rather than returning empty results

Nothing is downloaded during the build. There is no Python, Node.js, Go, or
Rust anywhere in the build, the tests, or the runtime. Exactly one dependency
is vendored — yyjson, used only to parse untrusted JSON on the IPC boundary —
pinned by tag and digest in
[third_party/yyjson/PROVENANCE.md](third_party/yyjson/PROVENANCE.md).

Linux only: the watcher is inotify, and peer identity on the socket is
`SO_PEERCRED`.

## Build, test, install

```sh
make            # release build -> build/atlas
make debug      # debug build   -> build-debug/atlas
make test       # release build + full CTest suite
make smoke      # release build + CLI smoke test (compiled JSON checker)
make asan       # AddressSanitizer + LeakSanitizer build, then the suite
make ubsan      # UndefinedBehaviorSanitizer build, then the suite
make tsan       # ThreadSanitizer build, then the suite
sudo make install   # installs to /usr/local/bin/atlas
make clean
```

`make install` honours `PREFIX`:

```sh
make install PREFIX=$HOME/.local     # -> ~/.local/bin/atlas
```

CMake is the canonical build system; the Makefile is a thin wrapper:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd build && ctest --output-on-failure
```

Warnings in Atlas' own code are errors (`-DATLAS_WERROR=ON`, the default).

## Quick start

```sh
atlas doctor                    # observes; creates nothing, not even an index
atlas repo add /srv/project --name project
atlas scan project
atlas status project
atlas search project QUERY
```

To keep the index current automatically, run the daemon as a systemd user
service:

```sh
atlas service install --user
systemctl --user daemon-reload
systemctl --user enable --now atlas
```

Registration is an operator action and a local write, so it needs the daemon
stopped (`systemctl --user stop atlas`, register, start again). Nothing
registers a repository except an operator: no hook, MCP root grant or session
event can, so a model looking at a directory never causes Atlas to index it.

To survive an SSH logout the service needs
`sudo loginctl enable-linger $USER` — see
[docs/systemd-user-service.md](docs/systemd-user-service.md).

For Claude Code integration:

```sh
atlas integrate claude install --user  # tells the plugin where Atlas is
claude plugin marketplace add /path/to/atlas/integrations/claude
claude plugin install atlas@atlas-local --scope user
atlas integrate claude doctor          # checks the whole chain
```

For remote access:

```sh
atlas api-key create --label ci --scope repo:read --scope decisions:read
atlas gateway status     # what the root-owned policy says; binds nothing
atlas gateway run        # serves remote MCP and, optionally, the web GUI
```

## Command line

```
atlas [GLOBAL]... COMMAND [GLOBAL]... [ARGS]...
```

Global options are accepted before or after the subcommand; `--` ends option
parsing. `--json` emits one stable JSON document on stdout, `--data-dir DIR`
overrides the data directory, and `atlas help` lists every option.

```
# repository index
atlas doctor
atlas repo add PATH [--name NAME] | repo list | repo remove NAME --yes
atlas scan|status|search|file|history|diff NAME ...
atlas daemon run|status|ping
atlas sync NAME [--wait] [--full]
atlas events NAME [--since CURSOR]

# code intelligence (lexical and semantic)
atlas code status|sync|file|search|symbol|deps|impact NAME ...

# engineering memory
atlas decision list|show|search|history|for-file|export NAME ...
atlas decision propose|revise NAME ...
atlas decision approve|reject|supersede|revalidate|resolve NAME ID   # terminal only
atlas decision link add|remove|note REPO SOURCE TARGET
atlas decision links|orphaned|legacy|promote ...
atlas gate check NAME | gate show NAME ID
atlas verify show CLAIM-ID | verify run NAME CLAIM-ID | verify policy

# operations (local CLI only; no RPC method, no MCP tool)
atlas backup create|verify|restore ...
atlas maintenance plan | maintenance prune --apply
atlas operation status ID

# orchestration
atlas job submit|get|cancel|list ...      # submit takes --parent JOB and --parallel N
atlas job run --repo NAME --task TEXT --gate CMD [--gate CMD]... [--parallel N]
atlas job run --resume RUN
atlas job run-status RUN
atlas dispatcher run [--once]

# remote access
atlas api-key create|list|revoke|rotate ...
atlas gateway status|run

# integration
atlas mcp
atlas hook EVENT
atlas service print|install|uninstall --user
atlas integrate claude print|doctor|install|uninstall
```

Every `atlas code` result states whether the index is current, which generation
it describes, the resolution or evidence class of each fact, and whether the
result was truncated and why. Bounds refuse or report; nothing is silently
clamped or trimmed.

### Exit codes

| Code | Meaning |
| --- | --- |
| 0 | success |
| 1 | internal error, including allocation failure |
| 2 | usage error |
| 3 | configuration error, including the data directory |
| 4 | repository error: unknown name, not a repository, path not indexed |
| 5 | database or migration error |
| 6 | Git execution error: failure, timeout, output bound, or parse failure |
| 7 | integrity or safety invariant violated |
| 8 | `atlas gate`: `REVIEW_REQUIRED` — a relevant decision is stale or impacted |
| 9 | `atlas gate`: `BLOCKED` — Atlas could not prove a safe answer |

`8` and `9` are gate *outcomes* rather than errors; the gate writes one
complete document before exiting with either. In `--json` mode a failing
command still writes one valid JSON document to stdout, with `"ok": false` and
an `error` object.

## Data directory

Resolved in this order: `--data-dir DIR`, then `ATLAS_DATA_DIR`, then
`XDG_DATA_HOME/atlas`, then `$HOME/.local/share/atlas`. An empty or relative
value is a configuration error, not something Atlas guesses at. Directories are
created mode `0700` and the database mode `0600`, because an index can describe
private repositories. The socket lives under `XDG_RUNTIME_DIR/atlas/`, never
`/tmp`.

Under a system deployment (A7.1) the index is owned by a dedicated service
account and there is no fallback to the per-user database; see
[docs/security/A7_1_OPERATIONS.md](docs/security/A7_1_OPERATIONS.md).

## The security model, briefly

**Target repositories are read-only.** Git can be *configured* to execute
helpers while performing a read — `core.fsmonitor` runs on `git status` and
`git ls-files`, the two commands every scan performs — so an argv allowlist
alone is not enough. Atlas builds every git invocation in one place with a
constructed environment (nothing is inherited), `-c` overrides that disable
hooks, fsmonitor, external diff, pagers, askpass and transports, per-command
read-only flags, and a subcommand allowlist checked before the fork. There is
no shell anywhere in Atlas: no `system()`, no `popen()`, no `/bin/sh -c`.
Symlinks inside a repository are never traversed. Each invocation also carries
`-c safe.directory=<canonical root>` for the exact path Atlas resolved from its
own registry — global and system git configuration stay unread, so neither an
operator's nor a repository's own declaration can influence anything. A partial
(promisor) clone is refused at open, because Git 2.39 cannot be told never to
fetch a missing object on demand. Full detail and the adversarial tests are in
[docs/git-safety.md](docs/git-safety.md).

**Repository content is untrusted data.** Filenames, commit subjects, author
identities, branch names and Git's own error text are reported, never obeyed.
Before any of it reaches a terminal or a JSON document it is reversibly
encoded, so an ANSI sequence cannot recolour output and a bidirectional
override cannot make output read differently from the bytes it describes. Safe
text is terminal-safe, not model-safe: printable prose like "ignore all
previous instructions" has nothing to escape, which is why automatic model
context contains no repository prose at all — not even the repository's name or
root path, since both are derived from a directory somebody chose to name.

**Approvals, precisely.** `APPROVED` means an explicit action arrived through
Atlas' local operator channel, and no more than that:

- Atlas exposes no approval, rejection or supersession capability through MCP,
  hooks, or any AI-facing Atlas method. Conversation text and model-generated
  RPC arguments cannot change a lifecycle state.
- The local operator channel requires an interactive terminal and a deliberate
  confirmation typed against the revision's content hash.
- **A same-UID process that can drive a pseudo-terminal may imitate that
  channel** — and that includes an AI agent with shell access. Atlas' own test
  suite drives it exactly that way, on purpose.
- `LOCAL_OPERATOR_CONFIRMED` identifies the *channel*, not a person. It is not
  cryptographic identity, does not establish that a person was present, is not
  a signature and provides no non-repudiation.

Approval changes a status, never the nature of the bytes: an approved decision
is accepted project policy *and* still untrusted data, labelled
`UNTRUSTED_DATA` wherever it is reported, and no decision prose enters
automatic model context at any status.

**Verification never overrides authority.** A9.2's verification records what
evidence bears on a claim. Independence is computed from declared derivation
edges, so three models reading one document count once, and repetition
contributes nothing. A confidence score is an integer out of 100 and never a
probability. A root-owned policy may authorise narrow, mechanically-justified
lifecycle transitions, recorded against a distinct `VERIFICATION_POLICY` actor
and spending a single-use warrant — but no score, sample count or number of
agreeing sources accepts a risk or adopts an architecture, and nothing a model
can reach approves, rejects, supersedes or resolves anything.

**Repository identity survives removal.** Decision records are never deleted;
removing a repository detaches them. `repo_identity_hash` is a
**path-qualified lineage fingerprint**: the canonical root path, the object
format and the sorted set of ingested root commits. Automatic reattachment
requires all three to match exactly, so an unrelated project created at the
same path inherits nothing, and the same repository cloned to a different path
is not reattached automatically either. `atlas decision orphaned` lists
records currently attached to no repository.

**OS separation, where it is deployed.** Under A7.1 the daemon runs as its own
account and solely owns the index and backups; every persistent model process
runs as an untrusted worker account that cannot read the index or replace the
binary; and root-owned policy files, reachable through compiled-in paths with
no environment override, decide who may open the socket and what each peer may
call. The guarantees that matter are kernel-enforced, not Atlas-enforced. On an
unseparated machine those guarantees do not apply, and the documentation says
so rather than implying otherwise. See
[docs/security/A7_1_THREAT_MODEL.md](docs/security/A7_1_THREAT_MODEL.md).

## Limitations Atlas states rather than hides

- History is served from the index, so it reflects the last scan; `atlas
  status` reports head drift so a stale index is visible.
- The lexical index is not a compiler: candidates stay candidates, ambiguity is
  recorded rather than resolved, and a missing compile database costs precision
  and says so. The semantic index answers only what the compiler proved, and
  Atlas never claims to know every target of a function pointer.
- Impact and gate results are candidates for review, bounded and deterministic,
  each carrying the weakest resolution or evidence class along its path. A
  truncated traversal is reported, never absorbed.
- Change attribution is a claim about opportunity, not causation: `ambiguous`
  stays ambiguous, and a record whose session cannot be identified exactly is
  stored sessionless rather than attached to a neighbour.
- A backup is a plain, unencrypted, unsigned SQLite file; the reported SHA-256
  detects damage and accident and is not a signature. SQLite has no per-page
  checksum, so a byte flipped inside an ordinary value leaves a structurally
  valid database and no check Atlas can run will find it — decision revisions
  are the exception, because each is rehashed from its stored content. Each
  backup is a whole copy of the index; there is no differential mode, and no
  recovery to an instant between backups. `backup restore` and `maintenance
  prune` require the daemon to be stopped, and there is no background deleter.
- The watcher cannot see what inotify cannot report (some bind mounts, network
  filesystems); those repositories are covered by periodic reconciliation only,
  and Atlas never claims currency it cannot prove.
- Working-tree files above 256 MiB are recorded with size and object id but not
  content-hashed. Submodule contents are never inspected. Merge commits
  contribute no per-parent file changes.
- Empirical (model-based) verification requires historical calibration, has
  none on a fresh machine, and stays in shadow until it does. Deterministic
  verification does not wait for calibration, because a mechanical truth
  condition Atlas evaluated does not depend on anyone's track record.
- **No evidence of X is not evidence of no X.** Atlas reports `ABSENT` only
  where it can show, dimension by dimension, that its coverage was sufficient
  for that bounded claim; everywhere else the answer is `UNKNOWN`, which is
  epistemic uncertainty and never a negative fact. The practical limits that
  follow are real ones: an absence can be established over the semantic index
  and cannot be established about a running system, because Atlas has no
  runtime probe — so a key absent from the repository never becomes a key the
  deployment does not set. An external-linkage symbol's callers cannot be
  enumerated from the index alone, and a symbol whose address escapes has
  indirect callers Atlas cannot rule out. Every one of those answers `UNKNOWN`
  rather than guessing.
- Applying a job's patch, committing, pushing and every GitHub verb are absent
  from the orchestration layer, deliberately — see the status section of
  [docs/orchestration.md](docs/orchestration.md).

## Documentation

- [docs/architecture.md](docs/architecture.md) — layers, invariants, ownership
- [docs/data-model.md](docs/data-model.md) — schema and migrations
- [docs/provenance.md](docs/provenance.md) — evidence types and the JSON contract
- [docs/git-safety.md](docs/git-safety.md) — the read-only guarantee in detail
- [docs/daemon-and-ipc.md](docs/daemon-and-ipc.md) — the daemon, threads, the
  single-writer model, and the wire protocol
- [docs/watcher-consistency.md](docs/watcher-consistency.md) — what "current"
  means, and what Atlas does when it cannot prove it
- [docs/systemd-user-service.md](docs/systemd-user-service.md) — running the
  daemon as a user service
- [docs/code-intelligence.md](docs/code-intelligence.md) — both code indexes:
  resolution and evidence classes, what is claimed, and what is explicitly not
- [docs/decision-lifecycle.md](docs/decision-lifecycle.md) — the knowledge
  model: kinds, the state machine, the operator channel and its honest limits
- [docs/verification.md](docs/verification.md) — claims, attestations,
  independence, and the automatic lifecycle policy
- [docs/semantic-freshness.md](docs/semantic-freshness.md) — how the daemon
  keeps the compiler-derived index current, what a generation says it covered,
  and why source-current is not the same as coverage-complete
- [docs/semantic-discovery.md](docs/semantic-discovery.md) — how Atlas *finds* a
  repository's compilation databases, how it knows whether that search was
  complete, and why complete processing of configured inputs does not prove
  complete discovery of relevant inputs
- [docs/engineering-rules.md](docs/engineering-rules.md) — the per-season layer
  maps and the full text of every non-negotiable rule, with the reasoning
- [docs/extending.md](docs/extending.md) — one checklist per extensible
  vocabulary, table, method table and bound
- [docs/impact-gates.md](docs/impact-gates.md) — freshness and gate semantics,
  reason codes, exit codes, the revalidation workflow, traversal limits
- [docs/operations.md](docs/operations.md) — backup, verification, restore,
  retention, and the limitations of each
- [docs/orchestration.md](docs/orchestration.md) — jobs, leases, the
  dispatcher, workspaces, and what is deferred
- [docs/remote-access.md](docs/remote-access.md) — the gateway, credentials,
  scopes, the web API and the GUI
- [docs/ai-trust-boundary.md](docs/ai-trust-boundary.md) — what safe text does
  and does not protect against, and how the boundary is implemented
- [docs/claude-integration.md](docs/claude-integration.md) — the hooks, the MCP
  tools, the plugin, and the one-time setup
- [docs/security/A7_THREAT_MODEL.md](docs/security/A7_THREAT_MODEL.md) and
  [docs/security/A7_SECURITY_REVIEW.md](docs/security/A7_SECURITY_REVIEW.md) —
  the dedicated security review
- [docs/security/A7_1_THREAT_MODEL.md](docs/security/A7_1_THREAT_MODEL.md) and
  [docs/security/A7_1_OPERATIONS.md](docs/security/A7_1_OPERATIONS.md) — the
  separated deployment
- [docs/backlog.md](docs/backlog.md) — known engineering and security backlog
- [docs/roadmap.md](docs/roadmap.md) — where the phases are going
- [third_party/yyjson/PROVENANCE.md](third_party/yyjson/PROVENANCE.md) — the
  one vendored dependency, its exact upstream identity and its digests
- [SECURITY.md](SECURITY.md) — threat model and reporting

## License

Apache License 2.0. See [LICENSE](LICENSE).
