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

## Status: phase A1

A1 adds a daemon. After registering a repository once and enabling the service,
filesystem and Git changes are detected and indexed **without running `atlas
scan` by hand**.

```sh
make
make test
sudo make install                      # optional; only this step needs root
atlas repo add /path/to/dna --name dna
atlas service install --user
systemctl --user daemon-reload
systemctl --user enable --now atlas
```

`sudo` there applies **only** to installing the binary system-wide. The daemon
and everything it writes run as your normal user. To keep it running after an SSH
logout you also need `sudo loginctl enable-linger $USER` — see
[docs/systemd-user-service.md](docs/systemd-user-service.md).

### What A1 adds

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

#### What A0 and A1 deliberately do not implement

None of the following exist yet, and Atlas does not pretend otherwise:

- decisions, ADRs, or recorded reasons for change. When a reason is requested,
  Atlas answers `UNKNOWN` rather than inferring one.
- `compile_commands.json` parsing. Atlas records that the file exists, whether it
  is a regular file or a symlink, and its content hash. It does not read its
  contents.
- clangd integration, symbol extraction, call graphs, dependency graphs
- impact analysis or stale-document gates
- **any AI integration.** A1 contains none. The model-context trust boundary A2
  will need is specified, not built, in
  [docs/ai-trust-boundary.md](docs/ai-trust-boundary.md) — and note that Atlas'
  safe-text encoding is *not* that boundary and cannot be extended into it.
- a Claude Code skill or an MCP adapter
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
atlas version
atlas help
```

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
- [docs/ai-trust-boundary.md](docs/ai-trust-boundary.md) — what safe text does
  and does not protect against, and what A2 must build
- [docs/backlog.md](docs/backlog.md) — known engineering and security backlog
- [docs/roadmap.md](docs/roadmap.md) — A2 through A6
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
