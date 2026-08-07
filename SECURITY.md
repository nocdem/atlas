# Security

## Threat model

Atlas reads repositories it did not create. **A registered repository is
untrusted input**: its filenames, file contents, Git metadata, commit messages,
and its own `.git/config` are all attacker-controlled in the worst case. Atlas is
designed so that indexing a hostile repository cannot lead to code execution,
reads outside the repository, unbounded resource use, or corruption of the index.

Atlas is a local, single-user tool. It opens no network sockets, listens on no
port, and transmits nothing anywhere.

**Since A1 it does have a daemon**, and that daemon listens — on a Unix-domain
socket at `$XDG_RUNTIME_DIR/atlas/atlas.sock`, with mode 0600 inside a 0700
directory, and refusing any peer whose `SO_PEERCRED` UID is not ours. There is
no TCP socket and no network transport of any kind; the systemd unit sets
`PrivateNetwork=yes` and `IPAddressDeny=any`, so the absence of network access is
enforced by the kernel rather than only asserted here.

The daemon's attack surface is one local socket reachable only by the same user.
Its protocol is length-framed with a hard size ceiling checked before any payload
is read, bounded nesting depth, deadlines on both directions, and no remotely
callable shutdown. See [docs/daemon-and-ipc.md](docs/daemon-and-ipc.md).

**Since A2 it talks to a language model**, through two adapters that are both
clients of that same socket. Neither holds a database handle, neither creates a
process, and neither has a write path to any filesystem. Atlas itself makes no
LLM API call and needs no network access: Claude is the client, Atlas is the
local service.

That adds one threat the earlier phases did not have — **prompt injection through
repository content** — and one control that is genuinely different in kind from
everything else here. Encoding does not help: "ignore all previous instructions"
is entirely printable and passes any escaping unchanged. The control is that
automatic model context contains **no repository prose at all**, enforced by a
fixed field set, a 4 KiB ceiling and a fixed ASCII character allowlist that the
renderer checks against its own output. Repository prose reaches a model only
through an explicit tool call, labelled with its provenance. See
[docs/ai-trust-boundary.md](docs/ai-trust-boundary.md).

**Since A3 it reads a build description**, and `compile_commands.json` is
written by whoever wrote the repository. That file's `command` field is a shell
command line, and the obvious mistake would be to interpret it. Atlas does not:
it hashes the string to notice when it changes, discards it, and reads the rest
through a positive allowlist of compiler arguments — include directories,
defines, the standard, the source and the output, and nothing else. Response
files (`@file`) and compiler plugins (`-fplugin=`) are recognised only well
enough to be ignored. An include directory outside the repository is stored with
`external = 1` and **never opened**: recording where a build looks is not being
allowed to look there.

The relevant test does not check that the parser is careful, which would be an
opinion. It plants an executable marker in the `command` string, in `argv[0]`,
in a `-fplugin=` argument and in a response file reference, runs a structural
pass, and asserts the marker never fired.

The structural indexer itself creates no process, reads only through
`atlas_path_open_nofollow`, and is bounded in file size, token length, nesting
depth, symbol count and relation count — every ceiling reported rather than
silently applied.

What Atlas does **not** defend against, and does not claim to:

- an attacker who can already run code as the user running Atlas
- an attacker who can write to the Atlas data directory or database
- a malicious `git` binary earlier in `PATH` than the real one
- deliberate misuse of the user's own credentials
- an attacker who can plant symlinks in the parents of the data directory
  (see [docs/backlog.md](docs/backlog.md) items 1 and 3 — such an attacker
  already has write access to the index)
- **a model that is persuaded by prose it explicitly asked Atlas for.** Atlas
  bounds it, labels it and states its provenance; it cannot make it safe. What
  limits the damage is capability rather than persuasion: an adapter that cannot
  write, cannot execute and cannot read outside the index has little to be
  persuaded into.
- **the truthfulness of what a model records.** A recorded reason is a
  `MODEL_PROPOSAL`, stored and reported as one. Atlas does not check whether it
  is true, and deliberately has no way for anything to claim it was approved.
- **a symbol name or an include spelling being persuasive prose.** They are
  repository content and they reach a model only through an explicit structural
  tool call, labelled `untrusted_data`. The automatic context envelope carries
  structural *counts* and nothing else, for exactly this reason.
- **the structural index being right about what a compiler would do.** It is
  lexical. Every fact carries a resolution class saying how firmly it is
  established, and `UNIQUE_LEXICAL` means one lexical match rather than one
  truth. Acting on an impact result without reading it is a mistake Atlas labels
  but cannot prevent.

## Controls

### No shell, ever

There is no code path in Atlas that can reach a shell. Subprocesses are created
with `fork` and `execve` using an explicit argument vector. There is no
`system()`, no `popen()`, and no `/bin/sh -c`. Shell metacharacters in a filename
or commit message are therefore inert bytes, and the test suite passes a payload
of shell syntax through as an argument and asserts it arrives literally.

`argv[0]` must be an absolute path. `PATH` resolution happens in exactly one
audited function, which refuses relative and empty `PATH` elements so a
repository cannot shadow `git` with a local file.

### Read-only Git access, and why the allowlist is not the main control

Git can be *configured to execute programs while performing a read*, so an argv
allowlist is necessary but far from sufficient. `core.fsmonitor` runs on
`git status` and `git ls-files`, the two commands every scan performs; a repository
setting it in its own `.git/config` would have had that program executed. Neither
`--no-optional-locks` nor `GIT_OPTIONAL_LOCKS=0` prevents it. Only
`-c core.fsmonitor=false` does.

Four layers, in order of how much they cover:

1. **A constructed child environment.** Built from a fixed list, never inherited, so
   no `GIT_DIR`, `GIT_CONFIG_*`, `GIT_EXTERNAL_DIFF`, `GIT_ASKPASS`, `GIT_SSH*`,
   `GIT_EXEC_PATH`, `GIT_TRACE*`, `HOME` or similar can reach Git. Asserted on every
   invocation, not just at startup. Atlas' own environment is never modified.
2. **A `-c` prefix** disabling fsmonitor, hooks, external diff, pager, askpass,
   signature verification, automatic gc and every transport. `-c` outranks the
   repository's own config.
3. **Per-command flags**: `--no-ext-diff --no-textconv --ignore-submodules=all` on
   every diff-producing command, `--ignore-submodules=all` on status.
4. **The read-only subcommand allowlist**, checked before the fork (exit code 7).

All of it is built in one function, so a new call site cannot opt out, and the Git
executable is an absolute path resolved once per process.

Details and the full policy: [docs/git-safety.md](docs/git-safety.md).

### Symlinks are never traversed

Atlas resolves every repository-relative path one component at a time with
`openat(..., O_NOFOLLOW)`, refusing to descend through any symlink. A tracked
symlink has its **link text** hashed and is never opened, so a symlink pointing
at `/etc/passwd` yields the hash of the string `/etc/passwd` and nothing else. A
tracked file whose parent directory has been replaced by a symlink is refused and
reported, not read.

Both cases are covered by tests that assert the content outside the repository was
not read.

### Bounded work and bounded memory

A hostile repository must not be able to exhaust the machine:

- every Git invocation has a timeout (60 s by default); on expiry the whole
  process group is terminated, so grandchildren cannot survive
- captured stdout has a hard ceiling (256 MiB by default); exceeding it kills the
  child and fails the command rather than growing memory
- captured stderr is bounded separately (64 KiB)
- Git output is parsed incrementally, with a ceiling of 16 MiB on any single
  NUL-delimited record
- file content is hashed in a streaming fashion and never held in memory; files
  above 256 MiB are recorded without a content hash
- JSON is streamed directly to the output stream, so a large result set is never
  assembled in memory
- the JSON writer has a fixed maximum nesting depth

### Parsers fail loudly

Every Git output parser validates its input and reports a clear error on
malformed data instead of guessing: object ids must be hex of a supported length,
timestamps must be numeric, modes must be octal, and records that do not match a
known shape are rejected. Filenames are taken positionally, never re-split on
whitespace, so a filename containing a tab, a newline, or a valid-looking status
code cannot shift the parse. Truncated output is an error, not a short read.

Paths are handled as bytes. They are stored as BLOBs and looked up by their exact
bytes; the printable `%XX` form is a lossless companion representation, never the
key.

### Injection into SQL and into search

All SQL uses bound parameters; no user or repository value is ever concatenated
into a statement. A search query is never interpreted as FTS5 query syntax: each
term is wrapped as a quoted phrase, so a query of `"unbalanced` or `NEAR(` is a
literal search rather than a syntax error or an unintended query. In the degraded
`LIKE` fallback, `%`, `_` and `\` in the query are escaped so they match
literally.

### Data at rest

The data directory is created with mode `0700` and the database file with mode
`0600`; the write-ahead log and shared-memory sidecars are tightened too. Atlas
refuses an empty or relative configured data directory rather than falling back to
somewhere surprising.

### What an AI session leaves behind

A2 stores session *metadata* and nothing else. This is a privacy control as much
as a storage one: an engineering-memory tool that quietly accumulated a
transcript would be a liability sitting in a user's home directory.

Stored: which client and session, which repositories it worked in, which tool
ran and whether it reported success, at most one normalized repository-relative
path per tool call, which paths the index observed changing and how they were
attributed, and the reasons and decisions somebody explicitly asked Atlas to
record.

**Not stored, and not read:** prompts, assistant messages, transcripts, the
contents of `transcript_path`, tool inputs, tool results, error text, shell
commands, `compact_summary`, source snippets, environment variables and
credentials. The hook adapter reads a fixed list of fields and reaches into
`tool_input` for exactly one member — a file path — and only for the tools whose
purpose is to write a named file.

This is verified rather than asserted: `tests/test_hooks.c` drives every
configured event with payloads containing a fake AWS key, a bearer token, a
destructive shell command, a private file path and a compaction summary, then
searches the resulting database **as raw bytes**, so a value written into a
column nobody thought to check is still caught.

Retention: ephemeral session events are pruned to
`ATLAS_AI_EVENTS_RETAIN_PER_SESSION` (2000 per session). Durable reasons and
decisions are never pruned — the prune statement addresses one table and cannot
reach them. An idle session is expired after 24 hours, and `expired` is kept
distinct from `closed` because "the client crashed" is a different fact from "the
user quit".

### Privileges

Atlas needs no elevated privileges and should not be run with any. Only
`make install` needs write access to the installation prefix.

`atlas integrate claude install --user` writes exactly one file, mode 0600, in
the user's own configuration directory, opened `O_NOFOLLOW` because it names an
executable a launcher will run. It never edits a Claude-owned file, never enables
or starts a systemd unit, and never runs `claude`. `uninstall` removes that one
file and never touches the index.

## Verification

The safety properties above are tested, not merely documented:

- shell metacharacters arrive as literal arguments
- the child environment is exactly what was passed, with nothing inherited
- a command that will not finish is killed and reported as a timeout
- an unbounded output stream is cut off and reported
- a symlink aimed outside the repository yields the hash of its link text
- a tracked path traversing a symlinked directory is refused
- filenames containing spaces, tabs, newlines and invalid UTF-8 round-trip
  exactly
- malformed Git output of every shape is rejected with a clear error
- write subcommands (`commit`, `push`, `checkout`, `reset`, `clean`, `gc`,
  `config`) are refused by the allowlist
- the entire repository tree, including `.git`, is byte-identical after every
  command, including `scan` and `repo remove`

Run them with `make test`, `make asan` and `make ubsan`.

## Terminal and untrusted-text safety

Repository content is **data, never terminal commands**. Filenames, commit
subjects and bodies, author identities, branch names and Git's own error text are
all attacker-controlled in the worst case, and all of them reach a human's
terminal or a machine consumer.

> **This is a defence against terminals and parsers, not against language
> models.** The A0 text here said "data, never instructions" beside the
> description of the encoding, which could be read as a claim that the encoding
> makes repository text safe to hand to a model. It does not, and no encoding
> could. A commit message reading *"ignore all previous instructions"* is
> entirely printable: it contains nothing to escape and passes through unchanged.
> Printable repository prose remains **semantically untrusted** however
> well-formed it is. A2 needs a separate model-context trust boundary, specified
> in [docs/ai-trust-boundary.md](docs/ai-trust-boundary.md).
>
> What the encoding does guarantee: terminal-safe, JSON-structure-safe, and
> reversible. Those three, and nothing about meaning.

Before any of it is printed, Atlas encodes it: C0 controls and DEL, C1 controls
(U+0080–U+009F, including the single-byte CSI), line and paragraph separators,
bidirectional controls (the Trojan Source family), bytes that are not valid UTF-8,
and `%` itself become `%XX`. The result is valid UTF-8, contains nothing a terminal
interprets, and percent-decodes back to the exact original bytes. Each JSON
document names the encoding in `text_encoding`.

Concretely, this stops a repository from:

- recolouring or rewriting Atlas' output with ANSI sequences
- retitling a terminal window or planting a hyperlink with an OSC payload
- overwriting an already-printed line with a carriage return, so that a refusal
  could be made to look like a success
- reordering displayed text with a bidirectional override so it reads differently
  from the bytes Atlas is reporting
- smuggling a control byte through JSON into a downstream consumer

The tests build a repository whose filenames, branch name, author identity and
commit subject all carry these payloads, run every command, and assert the precise
claim below, in both output modes, while the JSON document stays valid and the
original bytes remain recoverable.

**Exactly which control characters are allowed.** LF (0x0A) is itself a C0 control
character and legitimate output contains it, so "no control bytes" would be both
false and unhelpful. The claim Atlas makes and tests is:

- ALLOWED: LF (0x0A) only, and only as a line separator Atlas emits itself. Output
  is line-structured and a non-empty result ends with one.
- FORBIDDEN: every other C0 byte (0x00-0x09, 0x0B-0x1F) including TAB, CR, ESC and
  BEL; DEL (0x7F); every C1 control (U+0080-U+009F); and every bidirectional
  override.

LF is not a loophole for untrusted data, because an LF originating in repository
content is escaped as `%0A`. Every LF in the output is therefore structural by
construction. The tests close the loop independently of that reasoning by requiring
that each untrusted payload's raw byte sequence appears **nowhere** in the output,
so the set of permitted structural characters cannot be used to smuggle anything.

This boundary is not only about terminals. A5 puts Atlas output into a Claude Code
session and A6 may expose it over MCP; the same rule applies, for the same reason.
Atlas encodes at the point of output rather than trusting each consumer to defend
itself.

## Verification uses no language runtime

The build, the tests and the verification scripts depend on nothing beyond a C
compiler, CMake, Make, pkg-config, SQLite and Git. JSON produced by the CLI is
validated by `atlas-jsoncheck`, a compiled first-party tool built from the same
independent parser the test suite uses — deliberately not Atlas' own writer, since
a writer checked against itself proves nothing. `make smoke` runs it. No Python,
Node, Go or Rust is invoked at any point.
## Fail-closed refusals

Two situations make a read-only, network-free guarantee impossible, and Atlas
refuses rather than proceeding:

- **A partial (promisor) repository.** Git may fetch a missing object on demand, and
  Git 2.39 offers no way to forbid it (`--no-lazy-fetch` arrived in 2.41). Atlas
  detects the markers (a `*.promisor` pack, or promisor/partial-clone config) with a
  filesystem-only check that cannot itself trigger a fetch, and refuses with exit
  code 7 before any object is read. `GIT_ALLOW_PROTOCOL=none`,
  `-c protocol.allow=never` and `GIT_NO_LAZY_FETCH=1` are also set, so even a missed
  detection cannot reach the network.
- **A repository owned by another user.** Atlas reads no global or system Git
  configuration, so a `safe.directory` entry there does not apply, and Git ignores
  `safe.directory` supplied through `-c` or the environment. Reported as exit code 4
  with an actionable message rather than an opaque Git error.

## Adversarial testing

`tests/test_git_hardening.c` plants each execution vector in a real repository and
runs every Atlas command against it. Each vector is tested in two halves: a
**control** proving plain Git really does run the helper, so the assertion is not
vacuous, and then the requirement that Atlas never does. The helper is a compiled
program placed so that Git execs it directly with no shell; if it runs, it leaves a
file behind.

`scripts/adversarial.sh` (`make adversarial`) repeats this from outside under
`strace -f` and additionally requires that the only executables in the whole process
tree are `atlas` and `git`, that no `AF_INET` socket or `connect` is attempted, that
no child opens `/dev/tty`, that a decoy repository named by an inherited `GIT_DIR`
is never read, that the working tree and `.git` digests are unchanged, and that a
promisor repository yields a valid structured JSON error.
## Known findings

The controls above describe what Atlas is designed to do. For what an audit actually
found — including one verified bypass of the partial-clone refusal, the concurrency
hazards that will block the next phase, and the verified false positives — see
[docs/security-audit-a0.md](docs/security-audit-a0.md). Start there for any security
sweep; do not treat this file's claims as proof on their own.

## Reporting a vulnerability

Atlas has no public distribution or security contact yet. Until it does, report
suspected vulnerabilities through the same private channel you use for the rest of
this project, and please include:

- the Atlas version (`atlas version`) and `atlas doctor` output
- a repository or input that reproduces the problem, or a description of how to
  construct one
- what you expected Atlas to do instead

Please do not open a public issue for a vulnerability before it is fixed.


