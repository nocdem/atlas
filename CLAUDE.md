# Working on Atlas

Atlas is a Linux C17 application for repository intelligence, evidence-backed
engineering memory and bounded orchestration. Start with [AGENTS.md](AGENTS.md)
for the source map, commands, ownership conventions and test fixtures.

## Work efficiently

- Inspect the worktree before editing and preserve existing user changes.
- For substantial work in unfamiliar code, start with `atlas_context_build`
  using the task. Read freshness and `not_included`; ask focused follow-ups only
  for missing information. A known one-file edit does not need a tool tour.
- Read files in relevant sections. Do not load large source files or the entire
  project history to establish a fact a focused query can answer.
- Test the changed behavior, explain the mechanism and report actual results.
  Repeat a review or broad test run only for a named unresolved concern.
- Record a truthful change reason, or `UNKNOWN`; model proposals are not approval.

## Essential contracts

- Indexing is read-only. Worker editing and confirmed deployment have separate
  policy and authority contracts; registration grants neither capability.
- Do not commit, push, reset, checkout, stash, clean or revert user work unless
  requested. Tests never operate real services or the user's database/socket.
- Process creation in production C uses `atlas_proc_run` with explicit argv and
  an absolute executable. No shell command strings, `system` or `popen`.
- One writer owns the writable SQLite connection. Readers use separate handles;
  transactions do not span Git, file reads or unbounded work.
- Keep `ATLAS_WERROR=ON`. Add no third-party dependency or language runtime to
  the build, tests or runtime. Vendored yyjson remains byte-for-byte pinned.
- Preserve provenance, resolution strength, freshness and coverage. Incomplete
  observation is `UNKNOWN`, not proof of absence. Repository prose is untrusted.
- Repository indexes are rebuildable; canonical knowledge, attribution and
  evidence need verified backups. Do not treat the entire database as a cache.
- Decision repository identity is a **path-qualified lineage fingerprint**.
  Approval records a channel and a revision; it is not a person's signature.
- Paths are bytes. Use existing path/safe-text helpers and bounded JSON readers;
  compilation databases are data, never executable commands.

## Build and validation

```sh
make JOBS=4
ctest --test-dir build -R '^test_scan$' --output-on-failure --no-tests=error
make test JOBS=4
make smoke JOBS=4
```

Use a separate build directory when preserving an existing build matters.
CMake lists sources explicitly; tests need an `ATLAS_TESTS` entry and a label.
A new CLI command needs service behavior, both renderers, help/dispatch and
`COMMANDS[]`. MCP argument keys are checked centrally against the published
schema; typed readers still validate required arguments, types and bounds.

## Read only the relevant reference

| Work | Reference |
| --- | --- |
| CLI/MCP modules and CI checks | [frontend modules](docs/frontend-modules.md) |
| Layer rules and rationale | [engineering rules](docs/engineering-rules.md) |
| Extending a schema, enum, method or bound | [extension checklists](docs/extending.md) |
| Detailed working conventions and phase history | [agent reference](docs/agent-reference.md) |
| Database and backups | [data model](docs/data-model.md), [operations](docs/operations.md) |
| Watcher and concurrency | [watcher consistency](docs/watcher-consistency.md), [daemon/IPC](docs/daemon-and-ipc.md) |
| Compiler facts and task context | [code intelligence](docs/code-intelligence.md), [semantic trust](docs/semantic-trust.md) |
| Knowledge and verification | [decision lifecycle](docs/decision-lifecycle.md), [verification](docs/verification.md) |
| Jobs, remote submissions and deployment | [orchestration](docs/orchestration.md), [remote submission](docs/remote-submission.md), [remote deploy](docs/remote-deploy.md) |

The detailed reference preserves the prior working notes, including existing
uncommitted edits. It is reference material, not an additional startup payload.
