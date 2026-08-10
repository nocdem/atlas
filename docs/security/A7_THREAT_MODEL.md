# Atlas A7 — threat model

This is the model the A7 review was conducted against. It states who Atlas
defends against, what it defends, and — at least as importantly — what it does
not defend and why.

A threat model that lists only the attacks a system survives is a marketing
document. Every actor below is followed by what Atlas actually stops, and
several are followed by an explicit statement that it stops nothing.

## What is being protected

In priority order, because they are not equally important:

1. **The integrity of the decision record.** Atlas' distinguishing claim is that
   an approved decision was approved through a channel a person controls. A
   record that says so falsely is worse than no record, because nothing about it
   says it is wrong.
2. **The read-only guarantee over indexed repositories.** Atlas must never
   modify a registered repository, and must never execute anything from one.
3. **The trust boundary around model-supplied text.** Repository prose, commit
   messages, filenames and decision text are data. They must not become
   instructions, terminal control sequences, JSON structure or SQL.
4. **The availability and honesty of the index.** Atlas must not claim the index
   is current when it cannot prove it, and must not answer FRESH/PASS on a state
   it has not measured.
5. **Confidentiality of the index** — which is bounded by the filesystem, not by
   Atlas. See actor 4.

## Actors

### 1. A malicious indexed repository

Controls: filenames (arbitrary bytes, including newlines, control characters and
invalid UTF-8), file contents, size and count; commit messages, author
identities, dates; refs and branch names; symlinks; `.git/config`; linked
worktrees; `compile_commands.json`.

**In scope, and defended.** This is the actor A0–A3 were built against and the
defences are structural rather than incidental: a constructed git environment
rather than an inherited one, `-c core.fsmonitor=false` and the rest of the
hardening prefix, a read-only subcommand allowlist, a positive allowlist of
complete argument vectors for `git config`, `O_NOFOLLOW` on every repository
read, `%XX` path encoding, bounded parsers, and `compile_commands.json` read as
data with its `command` string hashed and discarded. `tests/test_git_hardening.c`
and `scripts/adversarial.sh` (under `strace`) are the evidence.

**Not defended:** a repository whose *prose* is persuasive. `atlas_safe()` makes
text terminal-safe and JSON-safe; it does not make it model-safe. A commit
message reading `ignore all previous instructions` is entirely printable and
passes through unchanged. The mitigation is structural and lives elsewhere: no
repository-authored text enters automatic model context at all, and repository
prose reaches a model only through an explicit MCP result labelled
`UNTRUSTED_DATA`.

### 2. A model reaching Atlas only through MCP, hooks and the plugin

Controls: every tool argument, every hook payload field, the granted-roots list
its client answers, and the timing and repetition of all of them.

**In scope, and defended.** A7 changed this materially. The model-facing surface
is now reads, proposals and session bookkeeping. It contains no method that
approves, rejects, supersedes, revalidates, registers a repository, backs up,
restores, prunes, or alters authority. Those methods are **absent** from the
dispatch tables rather than present and refusing, which is the same guarantee A5
makes about backups; `tests/test_a7_authority.c` asks a live daemon for each name
and requires `unknown method`.

### 3. A model with arbitrary non-root shell execution and PTY creation, running as the same uid as the nominal operator

Controls: everything actor 2 controls, plus the ability to run any program,
speak the socket protocol directly, allocate a pseudo-terminal, read and write
every file the operator's uid can, and replace the `atlas` binary in
`~/.local/bin`.

**In scope. Defended only in one specific, stated way, and otherwise not
defended at all.**

This is the actor A7 exists for and the honest answer is uncomfortable. Nothing
observable from inside a process distinguishes this actor from a human at a
keyboard: not `isatty`, not `/dev/tty`, not pseudo-terminal ownership, not
environment variables, not parent-process names, not session ids, not typing
`YES`, and not response timing. `tests/test_decision_operator.c` has allocated a
pty and typed a confirmation since A4 — the demonstration is in the suite, not a
hypothesis about it.

What Atlas does: it stops *manufacturing a coherent approval record* for this
actor. Approval, rejection, supersession and revalidation require operator
authority, which requires an OS principal the actor is not, and in a profile
where that does not exist they are refused (see `atlas/authority.h`). The actor
can still write `state='APPROVED'` into SQLite by hand — but that forgery no
longer agrees with the append-only ledger, so `atlas_db_decision_verify` reports
it and `atlas doctor` surfaces it.

**Explicitly not defended:** the contents of the database, the backups, the
registry, and the index generally. This uid owns those files. `cp` exfiltrates,
`sqlite3` mutates, `rm` deletes, and no Atlas code is involved in any of it.
Atlas-level checks on backup, restore, prune and registration were considered and
deliberately not added, because a check this actor walks around with a shell
builtin reads as protection in a review and provides none.

The only thing that changes this is deployment: run the daemon and its data
directory as a uid the model does not have. Then the filesystem enforces what
Atlas cannot.

### 4. Another unprivileged local OS user

Controls: their own processes; anything world-readable or world-writable.

**In scope, and defended.** The data directory is `0700`, the database `0600`,
the runtime directory `0700` and verified with `lstat` before use, and the socket
`0600` with its mode re-checked after `bind` because `bind` applies the umask.
`SO_PEERCRED` is checked on every accepted connection and a peer uid that is not
the daemon's is refused — belt and braces against a permission mistake, a
bind-mount, or a descriptor handed over by a more privileged process. There is no
`/tmp` fallback for the socket, ever.

### 5. Malformed, truncated, oversized or replayed daemon requests

**In scope, and defended.** Frames are length-prefixed and bounded
(`ATLAS_IPC_MAX_REQUEST_BYTES`, 1 MiB); JSON depth is bounded at 24; method names
and request ids are bounded; the serve loop is non-blocking with per-connection
state so a partial header stalls only its own connection; responses are bounded
at 8 MiB and rows at 1000; concurrent clients at 64. Malformed JSON is answered
with an error document, not a crash. Capabilities are single-use, consumed inside
the transaction that spends them with `WHERE ... AND consumed = 0`, so a replay
loses deterministically rather than last-write-wins, and consumption survives a
daemon restart because it is a committed row rather than in-memory state.

### 6. A corrupted, partially written or maliciously substituted database or backup

**In scope, partially defended, and the limits are stated.** Backups are written
to a mode-0600 `O_EXCL` temporary file in the destination directory, verified in
full by the same code `backup verify` runs, `fsync`ed, and only then renamed —
so nothing partial is ever published. Verification checks the declared page
count against the actual file length *before* anything else, because a file
truncated in unallocated space passes `PRAGMA integrity_check`. Every decision
revision is rehashed from its stored content, so tampering there is detected.

**Not detected:** a byte flipped inside an ordinary value in a non-decision
table. SQLite has no per-page checksum and Atlas adds none, so such a change
leaves a structurally valid database and nothing Atlas runs will find it.
`tests/test_backup.c` asserts this case *as undetected*, so the limitation cannot
quietly disappear from the documentation while remaining true of the code.

### 7. Process crashes, daemon restarts and concurrent operations

**In scope, and defended.** One writer thread owns the only writable handle; the
data-directory lock makes "exactly one writer" a kernel-enforced fact; no git
process or file read happens inside a write transaction; a pass that observes
HEAD moving is abandoned rather than committed; an event gap makes
`index_current` false until a full content-verifying pass clears it;
`code_index_state.resolve_settled` is cleared before resolution and set after, so
a pass that died mid-resolution leaves the next one to sweep.

### 8. Cross-repository identity confusion

**In scope, and defended.** A decision attaches by `repo_identity_hash`, a
path-qualified lineage fingerprint: the canonical root path, the object format
*and* the sorted set of ingested root commits. Both halves matter — without the
lineage, `git init` at the same path inherits the previous project's approved
decisions; without the path, the same lineage elsewhere reattaches silently.
Detach happens unconditionally at registration and attach only after ingestion on
an exact non-empty match. A session is found by its key and never by recency or
by repository.

### 9. Environment, executable-path and Git configuration poisoning

**In scope, and defended for git.** Nothing outside the fixed `ATLAS_GIT_ENV`
list reaches a git child, asserted on every invocation; `HOME`, `GIT_DIR`,
`GIT_CONFIG_*`, `GIT_EXTERNAL_DIFF`, `GIT_ASKPASS` and `GIT_TRACE*` are never
forwarded; the executable is resolved once per process; repositories are
addressed with `git -C <canonical-root>` and never by `chdir`.

**Bounded for Atlas itself:** `XDG_RUNTIME_DIR` and `ATLAS_DATA_DIR` are read
from the environment, so a caller chooses its own socket and index. That is not a
vulnerability — a process that can set them can also just run its own daemon —
but it is why the authority policy path is a compiled-in constant with no
environment override and no flag. A caller that could choose the policy would not
be constrained by it.

### 10. Resource exhaustion through indexing, graph traversal or response generation

**In scope, and defended.** Every bound is named in `include/atlas/limits.h` with
a written reason, reported when reached rather than silently applied, and — for
A6 — reaching any bound produces `TRAVERSAL_LIMIT`, which is `UNKNOWN`, which is
`BLOCKED`. A truncated walk can never report that it found nothing. Ambiguity
counts report the true number even when more candidates existed than the ceiling
keeps, because a bound that makes an ambiguity look smaller than it is is a bound
that lies.

## Explicitly out of scope

- **Root or kernel compromise.** Root can replace the binary, read every file and
  edit the policy. Nothing in userspace defends against this and Atlas does not
  pretend to.
- **A malicious trusted compiler**, and supply-chain attacks on the toolchain or
  on libc. Atlas vendors exactly one dependency (yyjson, pinned by tag and
  digest, verified in the suite) and downloads nothing at build time, which
  bounds this but does not eliminate it.
- **Physical access, memory-bus attacks, and side channels** including timing,
  cache and power analysis.
- **Denial of service by a same-uid process.** It can `kill` the daemon.
- **Cryptographic identity of any kind.** Atlas has no signatures, no
  certificates, no non-repudiation and no encryption at rest.
  `LOCAL_OPERATOR_CONFIRMED` names a channel, never a person.

## The one-sentence version

Atlas defends the record against everything that reaches it through Atlas, and
defends nothing against a process that already runs as the uid owning the files —
for which the answer is a separate OS principal, which is a deployment decision
Atlas reports on and will not make for you.
