# Atlas

Atlas is a headless engineering-memory and repository-intelligence application
written in C17. It indexes Git repositories into a local SQLite database and
answers questions about them from the command line, in human-readable text or in
stable JSON.

Atlas is generic. It knows nothing about any particular project; a repository
becomes interesting to Atlas only when you register it.

**Atlas never modifies a repository you register.** Every Git command it runs is
checked against a read-only allowlist before the process is created, hooks and
external diff drivers are disabled, and the working tree and index are only ever
read. See [docs/git-safety.md](docs/git-safety.md).

## Status: phase A9

A9 adds secure remote access: an HTTP gateway that authenticates a bearer
credential, checks scopes and forwards only explicitly supported reads to
`atlasd`; remote MCP over the same tool implementations the stdio adapter uses;
a versioned read-only web API; and an embedded Mission Control page.

```sh
atlas api-key create --label chatgpt --scope repo:read --scope decisions:read
atlas gateway status     # what the root-owned policy says; binds nothing
atlas gateway run        # serves remote MCP and, optionally, the web GUI
```

The secret is printed once and cannot be retrieved afterwards. **Atlas
terminates no TLS** — it is designed to sit behind a reverse proxy that does.
Credential administration is local and operator-only; no remote caller can
create, rotate or revoke a credential, and none can write anything at all. See
`docs/remote-access.md`.

## Status: phase A6

A6 turns Atlas' stored decisions into deterministic safety gates: it detects
when code changes may have made an approved decision stale, and refuses to let
an AI-facing surface present a stale or unverifiable decision as current
authority.

### What A6 adds

- `atlas gate check NAME` and `atlas gate show NAME DECISION-ID` — every
  approved decision assessed against one exact repository state, as `FRESH`,
  `STALE`, `IMPACTED` or `UNKNOWN`, with stable machine-readable reason codes.
  The query folds to `PASS`, `REVIEW_REQUIRED` or `BLOCKED`.
- **exit codes an automation can act on.** `0` for `PASS`, **`8`** for
  `REVIEW_REQUIRED`, **`9`** for `BLOCKED`. They are distinct because an
  automation that treats "a human should look at this" and "Atlas could not
  tell" identically will eventually be handed the second and behave as though it
  got the first. `0`–`7` keep their meanings exactly.
- **`UNKNOWN` fails closed.** Atlas never answers `FRESH` when the index is
  behind Git, an anchor will not resolve, history does not reach the validation
  point, a traversal bound was hit, or stored state disagrees with itself.
- **`STALE` means a human has to look again, not that the decision is wrong.**
  Atlas observed that the anchors moved. Whether an architectural decision
  survives a change to the code it concerns is a question about intent, and
  Atlas holds bytes and graph edges.
- `atlas decision revalidate NAME DECISION-ID` — an append-only operator record
  that a stale decision was checked against one exact repository state. It
  reuses A4's terminal-only single-use capability unchanged and adds two
  bindings: the indexed commit and a digest of what the anchors resolve to, so
  commit drift and evidence drift are both refusals. It never edits the approved
  revision, never changes its status, and preserves the assessment that prompted
  it.
- **nothing a model can reach may change any of it.** One read-only MCP tool,
  `atlas_gate_check`, forwarding to one read-only RPC method. There is no
  operation anywhere that clears, overrides or caches a freshness result.
  `tests/test_gate_trust.c` asks a live daemon for every method name such an
  operation would plausibly have and requires every one to fail.
- **no orchestration.** A6 provides a reusable gate evaluator for a future
  orchestration layer and implements none.

The whole contract, including the traversal limits and what the gate cannot
detect, is in [docs/impact-gates.md](docs/impact-gates.md). A6 adds migration 7,
taking the schema to 7.

## Phase A5

A5 is the operational phase: verified backups, an atomic restore, and a written
retention policy for every table in the schema.

### What A5 adds

- `atlas backup create|verify|restore` — an online snapshot taken through
  SQLite's backup API from a read-only connection, so a running daemon keeps
  writing while it is taken and the result is the database as of exactly one
  commit boundary, write-ahead log included. Not a file copy of `atlas.db`,
  `atlas.db-wal` and `atlas.db-shm`, which are meaningful only together and only
  at an instant no reader can name.
- **verification that creates nothing.** `backup verify` opens the file
  read-only, needs no data directory, repairs nothing, and checks the SQLite
  header, the declared length against the actual one, `integrity_check`,
  `foreign_key_check`, the tables the recorded schema requires, that the schema
  is not from a newer Atlas, **every decision revision rehashed from its stored
  content**, and every document's status replayed from its ledger. An unusable
  backup is an answer: a complete document, then a non-zero exit.
- **an atomic restore that keeps what it displaced.** It takes the writer lock
  exclusively — so a running daemon refuses it rather than racing it — verifies
  the backup completely before touching anything, refuses every symlinked
  component of the data directory, snapshots the existing index, stages a copy,
  and publishes by rename. Everything that can fail, fails with the original
  database byte-identical; the suite injects a failure at each of six points and
  compares the file after every one.
- `atlas maintenance plan|prune` — **every table classified**, with a written
  reason for each, printed by `plan`. Exactly one table is prunable in A5:
  `repo_events`, which already carried a documented ceiling. Decisions,
  revisions, the lifecycle ledger, AI reasons and proposals, attribution and
  evidence are protected from every automatic rule. There is no background
  deleter, and `prune` without `--apply` is a usage error rather than a quiet
  plan.
- **none of it is reachable from a model.** No RPC method, no MCP tool, no hook
  can create, read or restore a backup, or plan or apply a prune. The absence is
  structural, and `tests/test_backup_live.c` proves it by asking a live daemon.

Backups are **not encrypted and not signed**, and Atlas makes no durability
claim against disk or kernel failure. See
[docs/operations.md](docs/operations.md) for the whole contract, including what
verification cannot catch.

## Phase A4

A4 turns model-generated proposals into durable decision documents with
immutable revisions, an append-only lifecycle ledger, links to the code they
concern, and an approval step that is reachable only through an interactive
terminal.

### What A4 adds

- `atlas decision list|show|search|history|for-file|propose|revise|approve|reject|supersede|export|legacy|promote`
  — mirrored by ten daemon RPCs and four MCP tools
- **immutable revisions.** A revision is never edited; a change is a new
  revision with the next number, and the previous one keeps its own state and
  its own place in the ledger. Each is identified by a domain-separated,
  length-prefixed canonical content hash, and an approval binds to that hash
  rather than to a row.
- **an append-only lifecycle ledger** over four states — `PROPOSED`, `APPROVED`,
  `REJECTED`, `SUPERSEDED`. The ledger is canonical; the status columns are a
  cache of it that `atlas doctor` checks by replay and **never repairs**.
- **durable links** to paths, commits, change sets, symbols and other decisions.
  A symbol link is a selector *snapshot*, never a foreign key into the
  structural tables — so a rebuild or an analyzer upgrade preserves every
  decision exactly. Currency is computed on read: `CURRENT`, `CHANGED`,
  `MISSING`, `AMBIGUOUS`, `UNKNOWN`. **Atlas never re-points a renamed or
  ambiguous anchor**, and a changed link never revokes an approval — it is
  flagged for review.
- **an operator approval channel**: a real terminal, a short-lived single-use
  capability bound to one repository, document, revision and content hash, and a
  confirmation typed against that hash. `--yes`, piped stdin, environment
  variables, JSON input and every MCP tool are refused.
- **A2's `atlas_record_decision` bridges into A4.** It keeps its schema and its
  response, and a successful call now materialises a real A4 decision document
  in the same transaction — so an official client never leaves records that
  exist only in the legacy tables. `ai_decisions.approved` is still pinned to 0
  by its own CHECK, and historical rows remain explicitly promotable.
- **A repository is identified by a path-qualified lineage fingerprint.**
  `repo_identity_hash` commits to the canonical root path, the object format and
  the sorted set of ingested root commits, and an automatic reattachment
  requires all three to match exactly. Two consequences follow, and both are
  intended: an unrelated project created at the same path inherits nothing,
  because the root commits differ; and the same repository cloned to a different
  path is **not** reattached automatically either, because the root path differs.
  Manual relinking is deferred, so the second case stays an orphan for now.
  Decisions are never deleted, and `atlas decision orphaned` lists any that are
  currently attached to no repository.
- 128-bit decision identifiers (`atlas-dec-` and 32 hex), generated from
  Atlas-chosen values plus kernel entropy, unique in the database, with
  collision retry — because these ids are exported and outlive the database
  that minted them.
- schema v6.

### What A4 does not claim

`APPROVED` means an explicit action arrived through Atlas' local operator
channel. Precisely:

- Atlas exposes **no** approval, rejection or supersession capability through
  MCP, hooks, or any AI-facing Atlas method.
- Conversation text and model-generated RPC arguments **cannot** change a
  lifecycle state.
- The local operator channel requires an interactive terminal and a deliberate
  confirmation typed against the revision's content hash.
- **A same-UID process that can drive a pseudo-terminal may imitate that
  channel** — and that includes an AI agent with shell access. Atlas' own test
  suite drives it exactly that way, on purpose.
- `LOCAL_OPERATOR_CONFIRMED` identifies the *channel*, not a person. It is not
  cryptographic identity, does not establish that a person was present, is not
  a signature and provides no non-repudiation.

There is no cryptographic signing and no hardware-token support in A4.

Approval also does not change what the text *is*. An approved decision is
accepted project policy expressed in prose somebody wrote; it is still untrusted
data, it is still labelled `UNTRUSTED_DATA` wherever it is reported, and no
decision prose enters automatic model context at any status.

Full detail, the state machine, and the explicit non-claims are in
[docs/decision-lifecycle.md](docs/decision-lifecycle.md).

## Phase A3

A3 makes Atlas answer structural questions about C source, with the resolution
class attached to every answer. It is a **lexical** indexer, not a compiler and
not a pretend one: it reports what the bytes say, says how sure it is, and never
picks between two definitions of a name.

### What A3 adds

- `atlas code status|sync|file|search|symbol|deps|impact` — the structural
  commands, mirrored one-for-one by seven daemon RPCs and six MCP tools
- a first-party bounded lexical C indexer for `.c`, `.h` and `.inc`: symbols,
  includes, call candidates, translation units and file roles, with no Clang, no
  tree-sitter, no ctags and no new dependency of any kind
- eight resolution classes on every fact — `SOURCE_EXACT`, `BUILD_METADATA`,
  `UNIQUE_LEXICAL`, `AMBIGUOUS`, `UNRESOLVED`, `CONDITIONAL`, `MODEL_PROPOSAL`,
  `UNKNOWN` — so "these bytes are an identifier followed by `(`" and "this calls
  that function" stay different claims
- `compile_commands.json` read as **data**: an argument allowlist, paths checked
  against the repository, the `command` string hashed and discarded rather than
  stored, and nothing in it ever executed. There is a test that plants an
  executable marker in four places inside a compile database and asserts it never
  ran.
- bounded dependency and impact traversal with cycle detection, deterministic
  ordering, a reason per result, and separate sections for exact, unique-lexical,
  ambiguous and unresolved
- schema v5: structurally indexed files with typed roles, translation units,
  symbols, occurrences, relations, ambiguity candidates, bounded errors, and the
  interned identity of the analyzer that produced them
- **the graph knows which analyzer built it.** Upgrade Atlas with a corrected
  lexer and every existing structural index becomes stale, even though not one
  repository byte changed — because none of the other staleness signals can see
  that. The next ordinary sync rebuilds, and the rebuild touches derived rows
  only: sessions, recorded reasons, decisions, evidence and history come through
  untouched.
- structural counts — and only counts — added to the automatic model context

Full detail in [docs/code-intelligence.md](docs/code-intelligence.md), including
the explicit list of things A3 does **not** claim.

## Phase A2

A2 makes Atlas participate in a Claude Code session automatically. After a
one-time setup nobody types an `atlas` command during ordinary work: hooks open a
change session and correlate what changed with who was in a position to change
it, and an MCP server answers repository questions and records why things
changed — or records that nobody said why.

```sh
make
make test
sudo make install                      # optional; only this step needs root

atlas integrate claude install --user  # tells the plugin where Atlas is
atlas service install --user
systemctl --user daemon-reload
systemctl --user enable --now atlas
claude plugin marketplace add /path/to/atlas/integrations/claude
claude plugin install atlas@atlas-local --scope user

atlas integrate claude doctor          # checks the whole chain
```

**Register each repository yourself, with the daemon stopped:**

```sh
systemctl --user stop atlas
atlas repo add /path --name dna
systemctl --user start atlas
```

A7 removed automatic registration. Until then, opening a Claude session in a
directory registered it, and so did granting an MCP root — both of which are
chosen by, or influenced by, the model, which made "a model looked here"
sufficient for Atlas to start reading, hashing and indexing a tree nobody had
vouched for. There is no longer any RPC method, MCP tool or hook that registers
anything; a session in an unregistered directory is reported as unregistered and
nothing else happens. See `docs/security/A7_SECURITY_REVIEW.md`.

The daemon must be stopped because registration is a local operation under the
data-directory write lock, which the daemon holds while it runs — the same
contract `backup restore` and `maintenance prune` have had since A5.

### What A2 adds

- `atlas mcp` — a stdio Model Context Protocol server with ten tools: repository
  overview, changed files by git scope, file context with recorded history,
  bounded search, memory search, session state, and three recording tools
- `atlas hook <event>` — fifteen Claude Code lifecycle hooks. Every one fails
  open: a missing daemon produces a valid minimal answer and never blocks.
- `atlas integrate claude print|doctor|install --user|uninstall --user`
- a Claude Code plugin plus a local marketplace in `integrations/claude`,
  installed by `make install` to `<prefix>/share/atlas/claude-marketplace`, so a
  permanent user-scope install needs no network
- schema v4: AI sessions, change sets, attributed changed paths, change-reason
  and decision proposals, and a per-path working-tree change snapshot
- an implemented model-context trust boundary — see below

Full detail in [docs/claude-integration.md](docs/claude-integration.md).

### The trust boundary, in one paragraph

**Automatic context contains no repository prose at all.** No branch names, no
commit subjects, no author names, no file paths — because printable prose passes
any encoding unchanged, and "ignore all previous instructions" has nothing to
escape. What Atlas injects is versions, fixed vocabularies, integers, a validated
hex object id, an opaque repository id and a hash of its root — never the
repository name or the root path themselves, because both are derived from a
directory somebody chose to name. Bounded to 4 KiB and checked against a fixed
ASCII allowlist before it leaves. Repository prose reaches
a model only through an explicit tool call, labelled with its provenance and
`untrusted_data: true`. See
[docs/ai-trust-boundary.md](docs/ai-trust-boundary.md).

### What Atlas stores about a session

Metadata: which session, which repositories, which tool ran, whether it
succeeded, at most one normalized path per tool call, which paths the index
observed changing, and the reasons and decisions somebody asked Atlas to record.

It does **not** store prompts, assistant messages, transcripts, tool inputs, tool
outputs, error text, shell commands, source snippets, environment variables or
credentials. The hook adapter never reads those fields; the test suite drives
every event with payloads containing all of them and then searches the resulting
database as raw bytes.

### Phase A1: the daemon

A1 added the daemon. After registering a repository once and enabling the
service, filesystem and Git changes are detected and indexed **without running
`atlas scan` by hand**.

`sudo` above applies **only** to installing the binary system-wide. The daemon
and everything it writes run as your normal user. To keep it running after an SSH
logout you also need `sudo loginctl enable-linger $USER` — see
[docs/systemd-user-service.md](docs/systemd-user-service.md).

- `atlas daemon run`, a foreground daemon managed by systemd (no double fork, no
  pid file — systemd owns supervision)
- automatic inotify watching of every registered worktree, its Git directory and
  the shared refs, with debouncing and periodic reconciliation
- **incremental indexing**: a pass over an unchanged repository reads no file
  content at all, and one changed file costs one file. Measured on a 5000-file
  fixture: 5000 examined, 0 read. See `scripts/perf.sh`.
- incremental history ingestion (`git log HEAD --not <stored tip>`), with
  force-push and rebase detected rather than walked past
- per-file discovery inside new untracked directories, honouring `.gitignore`,
  which closes the A0 limitation the roadmap made an A1 acceptance criterion
- a bounded, versioned, length-framed local IPC protocol over a 0600 Unix socket
  in `$XDG_RUNTIME_DIR`, with `SO_PEERCRED` checking
- `atlas daemon status|ping`, `atlas sync`, `atlas events`, `atlas service
  print|install --user|uninstall --user`
- a durable, monotonic event journal for A2 consumers, with an explicit cursor
- an honest currency model: when Atlas cannot prove it observed every change, it
  says so, and does not describe the index as current until a full pass has run.
  See [docs/watcher-consistency.md](docs/watcher-consistency.md).

### What A0 established, and A1 keeps

A0 is the read-only foundation. It is deliberately small, because everything
later depends on trusting what it reports.

#### What A0 implements

- registering Git repositories by canonical root, with a unique user-facing name;
  several worktrees of one repository can be registered independently
- scanning tracked files: type, language, Git mode, Git index object id,
  working-tree content hash, size, executable and symlink state
- indexing Git history: commits, parents, author identity, timestamps, subject,
  body, and per-commit file changes including renames and copies
- searching indexed file paths and commit messages, ranked with SQLite FTS5 when
  available and with a clearly reported degraded fallback when it is not
- reporting indexed state next to live Git state, including index drift
- the complete working-tree change state: staged, unstaged, untracked and
  unmerged, each reported separately
- SOURCE and GIT evidence for every indexed fact
- stable JSON output for every command
- numbered, transactional, idempotent schema migrations

#### What Atlas still deliberately does not implement

None of the following exist yet, and Atlas does not pretend otherwise:

- **decision documents and ADRs read from the repository.** A2 records reasons
  and decisions that a model or a person hands it; it does not discover or parse
  any that are already written down in the tree. A3 answers the structural half
  of that; the recorded-reason half is still what A2 built.
- **a human approval workflow.** A2 records *proposals*. It has no way to prove a
  human agreed to one — an argument asserting approval is a string a model
  produced — so `approved` is pinned to zero by a schema `CHECK`, and lifting
  that is a deliberate future migration rather than an accident.
- **any inferred historical reason.** Asked why something changed with nothing
  recorded, Atlas still answers `UNKNOWN`. A commit subject is what the author
  wrote in the subject line, which is a different and weaker claim.
- `compile_commands.json` parsing. Atlas records that the file exists, whether it
  is a regular file or a symlink, and its content hash. It does not read its
  contents.
- clangd integration, symbol extraction, call graphs, dependency graphs
- impact analysis or stale-document gates
- any LLM API call or network access of any kind. Claude is the client; Atlas is
  a local service and never speaks to anything but its own socket and git.
- any write path into a target repository, in any form

## Requirements

- a C17 compiler (GCC or Clang)
- CMake 3.16 or newer, and Make
- pkg-config
- SQLite3 development headers and library
- Git (resolved from `PATH` at runtime, not baked in at build time)

Nothing is downloaded during the build. There is no Python, Node.js, Go, or Rust
anywhere in the build, the tests, or the runtime.

## Build, test, install

```sh
make            # release build -> build/atlas
make debug      # debug build   -> build-debug/atlas
make test       # release build + full CTest suite
make smoke      # release build + CLI smoke test (compiled JSON checker)
make asan       # AddressSanitizer + LeakSanitizer build, then the suite
make ubsan      # UndefinedBehaviorSanitizer build, then the suite
sudo make install   # installs to /usr/local/bin/atlas
make clean
```

`make install` honours `PREFIX`:

```sh
make install PREFIX=$HOME/.local     # -> ~/.local/bin/atlas
```

CMake is the canonical build system; the Makefile is a thin wrapper. To drive
CMake directly:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd build && ctest --output-on-failure
```

Warnings in Atlas' own code are errors (`-DATLAS_WERROR=ON`, the default).

## Command line

```
atlas doctor
atlas repo add PATH [--name NAME]
atlas repo list
atlas repo remove NAME --yes
atlas scan NAME
atlas status NAME
atlas search NAME QUERY
atlas file NAME PATH
atlas history NAME PATH
atlas diff NAME

atlas code status NAME
atlas code sync NAME [--rebuild]
atlas code file NAME PATH
atlas code search NAME QUERY
atlas code symbol NAME SYMBOL
atlas code deps NAME PATH [--depth N] [--reverse]
atlas code impact NAME PATH [--depth N]

atlas version
atlas help
```

Every `atlas code` result states the same four things, whatever the renderer:
whether the structural index is current, which generation it describes, the
resolution class of each fact, and whether the result was truncated and why.

### Grammar

```
atlas [GLOBAL]... COMMAND [GLOBAL]... [ARGS]...
```

Global options are accepted **before or after** the subcommand, so both of these
are valid and produce identical output:

```sh
atlas --json repo list
atlas repo list --json
```

`--` ends option parsing, so an operand that begins with `-` can still be passed.

| Option | Meaning |
| --- | --- |
| `--json` | emit one stable JSON document on stdout |
| `--data-dir DIR` | use `DIR` instead of the resolved data directory |
| `--limit N` | cap results per result kind (default 50) |
| `--max-commits N` | stop ingesting history after N commits |
| `--no-history` | scan tracked files only |
| `--no-untracked` | omit untracked paths from `diff` |
| `--timeout-ms N` | per-Git-invocation timeout |
| `--yes` | confirm a destructive metadata operation |
| `-q`, `--quiet` | suppress non-essential output |
| `-h`, `--help` | print help |
| `-V`, `--version` | print the version |

### Examples

```sh
$ atlas doctor
Atlas 0.1.0 (phase A0)
  build compiler    GNU 12.2.0
  git               /usr/bin/git (git version 2.39.5)
  sqlite            3.40.1 runtime, 3.40.1 at build time
  data directory    /home/you/.local/share/atlas (from HOME)
  database          /home/you/.local/share/atlas/atlas.db
  schema version    2 (expected 2)
  journal mode      wal
  foreign keys      on
  fts5              available
  search mode       fts5
  integrity check   ok
  foreign key check ok
  repositories      1
status: ok

$ atlas repo add /srv/project --name project
$ atlas scan project
scanned project at 4c1f9a20e8bd (born, branch main)
  files             128 total, 128 added, 0 modified, 0 deleted, 0 unchanged
  commits           412 new of 412 seen, 1104 file changes
  evidence          540 records
  worktree          clean
  compile db        not found
scan id 1

$ atlas file project src/core/buf.c
src/core/buf.c
  type              regular
  language          c
  git index oid     5626abf0f72e58d7a153368ba57db4c673c0e171
  content hash      sha256:74f19758d7d32f2e56d471b5c4fb979c268cb3f4a15fca82943ccc34efd7b324
  recorded changes  3
  last commit       9f2c1ab44e01 2026-08-01T09:14:22Z  tighten buffer growth
  evidence          SOURCE (git index and working tree), GIT (commit history)
  reason            UNKNOWN: A0 records facts only and never infers why

$ atlas diff project
  base              4c1f9a20e8bd (born, branch main)
staged:
  add        +2 -0        NOTES.md
unstaged:
  modify     +1 -1        src/core/buf.c
untracked:
  untracked  -            scratch.log  184 bytes  sha256:9f86d081884c7d65
  summary           1 staged, 1 unstaged, 1 untracked, 0 unmerged
  evidence          GIT (index and working tree), SOURCE (untracked file identity)
```

`atlas diff` reports the four scopes separately, because a path that is staged and
then modified again is two facts, not one. For untracked paths it records identity
only: the path, the size and a content hash, never the file contents.

Machine output distinguishes the same things, so a consumer never has to infer a
scope from a status letter:

```sh
$ atlas --json diff project | jq '{base_head, staged: (.staged|length),
    unstaged: (.unstaged|length), untracked: (.untracked|length),
    binary_changes, truncated}'
```

Every JSON document also carries `"text_encoding"`, naming the encoding used for
repository-originated text; see [docs/provenance.md](docs/provenance.md).

```sh
$ atlas --json status project | jq .head_drift
false
```

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

`8` and `9` are gate *outcomes* rather than errors. `atlas gate check` writes one
complete document and no error object before exiting with either, which is the
contract `atlas daemon ping` already follows.

In `--json` mode a failing command still writes one valid JSON document to
stdout, with `"ok": false` and an `error` object, and the message also goes to
stderr.

## Data directory

Atlas resolves its data directory in this order:

1. `--data-dir DIR`
2. `ATLAS_DATA_DIR`
3. `XDG_DATA_HOME/atlas`
4. `$HOME/.local/share/atlas`

An empty or relative value in any of these sources is a configuration error, not
something Atlas guesses at. Directories are created with mode `0700` and the
database file with mode `0600`, because an index can describe private
repositories. The database lives at `<data-dir>/atlas.db`.

The test suite always overrides the data directory with a temporary path and
never opens a real user database.

## Target repositories are read-only

Git can be *configured* to execute helpers while performing a read, so an argv
allowlist alone is not enough. `core.fsmonitor` runs on `git status` and
`git ls-files`, the two commands every scan performs. Atlas closes that and the
rest with a constructed environment and a `-c` prefix; see
[docs/git-safety.md](docs/git-safety.md) for the full policy and the adversarial
tests that prove it.


This is the property everything else rests on:

- Git subcommands are checked against a read-only allowlist (`rev-parse`,
  `ls-files`, `log`, `status`, `diff`, `symbolic-ref`, `cat-file`) before the
  process is created
- `GIT_OPTIONAL_LOCKS=0` stops Git from refreshing and rewriting the index while
  reading status
- repository-controlled execution vectors are disabled with `-c` overrides, which
  outrank anything the repository's own config sets: hooks, external diff
  drivers, fsmonitor, pagers, and automatic gc and maintenance
- `GIT_DIR` and `GIT_WORK_TREE` are never inherited, so a stale environment
  cannot retarget a command
- there is no shell anywhere: programs are executed with `execve` and an explicit
  argument vector. No `system()`, no `popen()`, no `/bin/sh -c`.
- symlinks inside a repository are never traversed. A tracked symlink has its
  link text hashed; the file it points at is never opened.
- `atlas repo remove` deletes Atlas metadata only

The suite proves this rather than asserting it: it hashes the entire repository
tree, including `.git`, before and after every command and requires the digest to
be unchanged.

## Content hashes

Atlas records its own SHA-256 of working-tree content, independently of the Git
object format, so a file's identity does not change when a repository migrates
from SHA-1 to SHA-256. The implementation is first-party and pinned by
known-answer vectors. For a tracked symlink the hashed content is the link text,
which is exactly what Git stores in the blob.

## Paths that are not text

Repository paths are arbitrary bytes. They may contain spaces, tabs, newlines,
and sequences that are not valid UTF-8. Atlas stores the exact bytes and, next to
them, a lossless printable encoding: bytes that are not valid UTF-8, control
bytes, `%` and DEL become `%XX`. ASCII paths are identical in both forms, the
encoding is reversible, and the printable form is accepted as input, so a path
copied out of Atlas output can be pasted straight back in.

## Current limitations

- history is served from the index, so it reflects the last scan rather than the
  live repository; `atlas status` reports head drift so a stale index is visible
- `--limit` applies per result kind, so `search` can return up to `--limit` file
  hits and `--limit` commit hits
- rename detection is whatever `git log -M -C` reports, not an independent
  analysis
- merge commits are recorded, but A0 does not walk their per-parent diffs, so a
  merge contributes no file changes
- submodules are recorded as gitlink entries; their contents belong to another
  repository and are not read
- working-tree files above 256 MiB are recorded with size and Git object id but
  are not content-hashed
- an approval records that Atlas' local operator channel was used, not that a
  particular person acted; a process running as the same local user can produce
  one, and Atlas says so rather than implying otherwise
- removing a repository detaches its decisions rather than deleting them, and a
  detached decision is invisible to every listing until the same canonical root
  is registered again
- a backup is a plain, unencrypted, unsigned SQLite file; the reported SHA-256
  detects damage and accident and is not a signature
- SQLite has no per-page checksum, so a byte flipped inside an ordinary value
  leaves a structurally valid database and no check Atlas can run will find it;
  decision revisions are the exception, because each is rehashed from its stored
  content
- each backup is a whole copy of the index; there is no differential mode, and
  no recovery to an instant between backups
- `backup restore` and `maintenance prune` are writers, so both require the
  daemon to be stopped
- only `repo_events` is prunable; a large index is large because of `files`,
  `commits` and the structural graph, and nothing prunes those by age
- Atlas does not parse decision records out of a repository and does not write
  them into one; `atlas decision export` writes to stdout
- history is read with `git log`, so a repository with an enormous history takes
  time proportional to it; `--max-commits` bounds the walk and the result is
  reported as bounded
- Linux only. The watcher is inotify; there is no kqueue or FSEvents backend.
- `atlas diff` reports at most `--limit` entries (default 2000) and sets
  `truncated` beyond that; the per-scope counts remain exact
- `atlas diff` still reports a wholly untracked directory as one **collapsed**
  entry, deliberately: it is the cheap question, and it is what `git status`
  shows a human. Per-file discovery is additive and lives in the indexer — the
  daemon (and `atlas sync`) record every file inside a new untracked directory
  individually, with its own path, size and hash, honouring `.gitignore`. Ask
  `atlas file` or `atlas events` for those.
- Atlas cannot read a repository owned by another user, or a partial (promisor)
  clone; both fail closed with a clear error. See "Repositories Atlas will refuse".
- submodule contents are never inspected (`--ignore-submodules=all`)
- the watcher cannot see what inotify cannot report — some bind mounts, and
  network filesystems that do not implement it. Those repositories are covered by
  periodic reconciliation only, and Atlas cannot detect the situation in advance.
  It never claims currency it cannot prove; see
  [docs/watcher-consistency.md](docs/watcher-consistency.md).
- reconciliation is per repository, not per path: one changed file triggers one
  `lstat` per tracked file (about 480 ms on a 5000-file fixture) even though only
  the changed file's content is read. See [docs/backlog.md](docs/backlog.md).
- **the structural index is lexical, not compiled.** `identifier(` is a call
  *candidate*; a name defined once is `UNIQUE_LEXICAL`, which means "one lexical
  match", not "the compiler agrees". A call through a function pointer, a call
  produced by a macro, and a definition inside an `#if` are all recorded as
  candidates or as `UNKNOWN`, never as exact. Two definitions of one name stay
  `AMBIGUOUS` with both recorded; Atlas does not choose.
- **`#include` resolution is a separate fact from the include itself.** The
  directive is `SOURCE_EXACT` because the bytes say so; where it leads may be
  `SOURCE_EXACT`, `BUILD_METADATA`, `UNIQUE_LEXICAL`, `AMBIGUOUS` or
  `UNRESOLVED`, and a system header is honestly the last of those.
- **impact results are candidates to review, not a proof of breakage.** They are
  bounded, deterministic, and each one carries the path and the weakest
  resolution class along it.
- **C only.** `.c`, `.h`, `.inc`. `.C`, `.cpp` and `.hpp` are deliberately not
  treated as C, because A3 does not guess at C++.
- **the structural index is rebuilt, not migrated, when the analyzer changes.**
  A version bump costs one full structural pass over the repository. That is the
  price of not reporting a graph as current when the algorithm behind it has
  been corrected.
- **no compile database is an ordinary state, not an error**, and it costs
  precision rather than correctness: without one, an include that a `-I`
  directory would have resolved exactly falls back to a repository-wide lexical
  match and says so.
- **change attribution is a claim about opportunity, not about causation.** Atlas
  records `direct_edit` when a session's edit tool named a path *and* the index
  then saw it change, `observed` when only the second happened, and `ambiguous`
  when another session had the same repository open over the same window. It
  never reports a single cause when more than one was possible, and once a path
  is ambiguous it stays ambiguous.
- **a change set is correlated at the end of a tool batch and again at the end of
  a turn**, because the reconciliation a batch asks for cannot be waited on from
  inside the writer thread. A change made and reverted between those two points
  is not attributed to anything.
- recorded reasons and decisions are **proposals**. Atlas has no approval
  workflow and cannot prove a human agreed to one.
- **a record belongs to the session whose id it carries, or to no session.** An
  MCP write is attached by exact external session id — for Claude Code, the
  `CLAUDE_CODE_SESSION_ID` the hooks also see — and never by repository, recency
  or "the only one open". When Atlas cannot identify the session exactly the
  record is still stored, with `session_unbound` and a reason saying why. A
  client that is not Claude Code has no such id, so its records are sessionless.
  After `/clear` a running MCP server still holds the id it was started with; its
  writes are reported unattached rather than credited to the new conversation.
  See [docs/claude-integration.md](docs/claude-integration.md).
- MCP reads only indexed data. It is not a filesystem reader: it accepts no
  absolute path, and it answers about repositories the client granted through
  `roots/list` and no others.

## Documentation

- [docs/architecture.md](docs/architecture.md) — layers, invariants, ownership
- [docs/data-model.md](docs/data-model.md) — schema and migrations
- [docs/provenance.md](docs/provenance.md) — evidence types and the JSON contract
- [docs/git-safety.md](docs/git-safety.md) — the read-only guarantee in detail
- [docs/daemon-and-ipc.md](docs/daemon-and-ipc.md) — the daemon, threads, the
  single-writer model, and the wire protocol
- [docs/watcher-consistency.md](docs/watcher-consistency.md) — what "current"
  means, and what Atlas does when it cannot prove it
- [docs/systemd-user-service.md](docs/systemd-user-service.md) — running it as a
  user service
- [docs/claude-integration.md](docs/claude-integration.md) — the hooks, the MCP
  tools, the plugin, and the one-time setup
- [docs/ai-trust-boundary.md](docs/ai-trust-boundary.md) — what safe text does
  and does not protect against, and how A2 implements the boundary it cannot
- [docs/backlog.md](docs/backlog.md) — known engineering and security backlog
- [docs/code-intelligence.md](docs/code-intelligence.md) — the A3 structural
  index: resolution classes, what is claimed, and what is explicitly not
- [docs/decision-lifecycle.md](docs/decision-lifecycle.md) — the A4 decision
  model: the state machine, the operator channel and its honest limits, code
  links, migration and recovery
- [docs/impact-gates.md](docs/impact-gates.md) — the A6 contract: freshness and
  gate semantics, the reason-code vocabulary, exit codes, the human revalidation
  workflow, the snapshot model, traversal limits and known limitations.
- [docs/operations.md](docs/operations.md) — the A5 operational contract:
  backup, verification, atomic restore, the retention classification, and the
  limitations of each
- [docs/roadmap.md](docs/roadmap.md) — A6
- [third_party/yyjson/PROVENANCE.md](third_party/yyjson/PROVENANCE.md) — the one
  vendored dependency, its exact upstream identity and its digests
- [SECURITY.md](SECURITY.md) — threat model and reporting

## License

Apache License 2.0. See [LICENSE](LICENSE).

## Repository content is untrusted data

Filenames, commit subjects and bodies, author identities, branch names and Git's
own error text all come from outside Atlas. They are reported, never obeyed:

- no shell exists anywhere in Atlas, and Git runs only read-only subcommands
- repository text is encoded before it reaches a terminal, so an ANSI sequence in
  a commit subject cannot recolour output, an OSC payload cannot retitle a window,
  a carriage return cannot overwrite a printed line, and a bidirectional override
  cannot make output read differently from the bytes it describes
- the encoding is reversible: percent-decoding recovers the exact original bytes,
  and each JSON document names it in `text_encoding`

See [docs/provenance.md](docs/provenance.md) for the full rule and
[SECURITY.md](SECURITY.md) for the threat model.

## Repositories Atlas will refuse

Two cases fail closed with a clear, structured error rather than being read anyway.

**A repository owned by another user.** Atlas reads no global or system Git
configuration, so a `safe.directory` entry there does not apply, and Git cannot be
told to accept the repository another way: it deliberately ignores `safe.directory`
supplied via `-c` or the environment. Exit code 4, with a message naming the path.
Register a repository you own, or run Atlas as the owning user.

**A partial (promisor) clone.** Git may fetch a missing object on demand, and Git
2.39 has no way to forbid that. Since Atlas guarantees no network access, such a
repository is detected (a `*.promisor` pack, or promisor/partial-clone config) and
refused with exit code 7 before any object is read. Complete the clone and try
again.

## Submodules

`atlas diff`, `atlas status` and history all pass `--ignore-submodules=all`. A
submodule's own configuration is a separate untrusted surface with its own hooks and
helpers, so Atlas does not look inside one. A submodule appears as a `160000`
gitlink entry with a note; changes within it are not reported. Hardened submodule
handling is separate future work.
