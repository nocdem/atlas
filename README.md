# Atlas

> Evidence-backed engineering memory and repository intelligence for
> AI-assisted software development.

Atlas is a local, headless C17 application that indexes Git repositories into a
rebuildable SQLite database. It gives developers and AI tools a shared view of
code, history, decisions, evidence, coverage and staleness through a CLI, local
daemon, MCP server and authenticated HTTP gateway.

Atlas is experimental software at **v0.1.0 / phase A12.0**. It currently targets
Linux. It does not apply patches, commit, push or perform GitHub actions.

## Why Atlas exists

Long-running AI coding sessions lose context, repeat rejected approaches and
turn old conclusions into current facts. Ordinary text memory can preserve a
sentence, but it cannot by itself establish which repository state supported
that sentence or whether the relevant code has since changed.

Atlas keeps those questions separate:

| Ordinary text memory | Atlas |
| --- | --- |
| Stores a useful statement | Stores the statement, its kind, provenance and immutable revisions |
| May reuse an old conclusion | Assesses it against an exact repository state as `FRESH`, `STALE`, `IMPACTED` or `UNKNOWN` |
| A missing result can look negative | Requires sufficient coverage before reporting `ABSENT`; otherwise reports `UNKNOWN` |
| Model output may look authoritative | Model-written records remain proposals; approval is a separate operator capability |

Atlas is not a replacement for a model or an IDE. It is the evidence and
control layer beside them.

## What works today

- **Read-only repository intelligence:** tracked files, hashes, commit history,
  staged and unstaged changes, untracked files and provenance. Git and repository
  contents remain authoritative.
- **Continuously maintained indexes:** an inotify daemon keeps the repository
  index current and reports when observation or coverage was incomplete.
- **Two code-intelligence layers:** a bounded lexical C index records candidates
  and ambiguity; an optional libclang index records compiler-proven facts. The
  layers remain separate and every result carries its resolution strength.
- **Engineering memory and impact gates:** decisions, policies, invariants,
  accepted risks and obligations have immutable revisions, evidence, lifecycle
  state and deterministic stale/impact assessment.
- **AI integration:** Claude Code hooks and MCP tools expose bounded reads and
  proposal-only writes. Repository prose is labelled as untrusted data.
- **Bounded orchestration:** durable jobs, leases, crash recovery, isolated
  workspaces, operator-supplied gates, bounded parallel tasks and multi-stage
  planned runs. A worker result is an artifact, not authority.
- **Remote read access and remote submission:** scoped credentials, remote MCP,
  a read-only web API, Mission Control (which can now also dispose of a record),
  and A14's remote submission — a bearer credential the policy names queues a job
  the daemon verifies and the policy bounds.
- **Local operations:** verified online backups, atomic restore and explicit
  retention policy. These operations are not exposed to a model or remote API.

## A five-minute local start

With the requirements below installed:

```sh
make
make install PREFIX="$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"

atlas doctor
atlas repo add /srv/project --name project
atlas scan project
atlas status project
atlas search project QUERY
atlas code status project
```

The daemon and Claude integration are optional. Start with a local repository
index; add the service, semantic index or MCP integration only when needed.

## Trust boundary and evidence

**Atlas never modifies a repository it indexes.** Git commands pass a read-only
allowlist, executable hooks and external diff drivers are disabled, paths are
opened without following symlinks, and tests compare the entire repository tree
including `.git` before and after Atlas commands.

The repository includes its threat model, known limitations, security reviews,
full test suite and the reports from bounded live pilots. These are engineering
artifacts, not a claim of independent third-party certification:

- [Security model](SECURITY.md)
- [Read-only Git design](docs/git-safety.md)
- [AI trust boundary](docs/ai-trust-boundary.md)
- [Code-intelligence evidence classes](docs/code-intelligence.md)
- [Known backlog and residuals](docs/backlog.md)
- [Development phases and measured pilots](docs/roadmap.md)
- [Orchestration bounds](docs/orchestration.md)

The full A0–A12 development history is kept in the roadmap rather than used as
the product explanation.

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
atlas review apply FILE [--check] [--json]   # terminal only; walks a review sheet
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

# planned runs (A12.0)
atlas plan run --repo NAME --goal TEXT --gate CMD [--gate CMD]... [--parallel N]
atlas plan run --resume PLAN
atlas plan status PLAN | plan show PLAN --rev N | plan list

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
| 8 | `atlas review apply`: at least one sheet entry did not end `APPLIED` |

`8` and `9` are gate *outcomes* rather than errors, and `atlas review apply`'s
`8` is an outcome in the same sense — the command writes one complete document
before exiting with it. Both commands share the value `8` for the same reason
they share nothing else: each is read in the context of the command that
produced it, not as one global vocabulary, and a caller distinguishes them by
which command it ran. In `--json` mode a failing command still writes one
valid JSON document to stdout, with `"ok": false` and an `error` object.

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
  scopes, the web API and the GUI (A16 disposal and A14 submission included)
- [docs/remote-submission.md](docs/remote-submission.md) — what remote submission
  is, what a credential in flight is worth, and the cleartext chain
- [docs/browser-disposal.md](docs/browser-disposal.md) — the browser disposal
  channel and the honest paragraph
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
