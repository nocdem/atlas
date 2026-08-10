# Atlas A7 — dedicated security review

Scope: the whole of Atlas at commit `6cd054c98af00764a51c0daea24d1516c6d2af6e`
(A6), reviewed against `docs/security/A7_THREAT_MODEL.md`.

Method: source review of every trust boundary — CLI dispatch, daemon request
parsing, RPC and MCP registration, hooks and plugin, session and repository
discovery, decision approval and revalidation, capability lifecycle, SQLite and
migrations, backup/verify/restore/maintenance, git execution and parsing,
repository identity, indexing and graph traversal, the systemd unit, installation
and update, logging and error reporting — followed by writing an executable
demonstration for each candidate finding *before* fixing it.

Every finding below that is marked CONFIRMED was reproduced against a running
Atlas, not inferred from reading. Two of them were reproduced against a live
daemon in an isolated fixture and the transcripts are quoted.

## Summary

| ID | Severity | Component | Status |
|----|----------|-----------|--------|
| ATLAS-A7-001 | CRITICAL | `src/ipc/server_decision.c` | Fixed |
| ATLAS-A7-002 | HIGH | `src/ipc/server_ai.c`, `src/hook/hook.c`, `src/mcp/mcp.c` | Fixed |
| ATLAS-A7-003 | HIGH | `src/ipc/server.c` | Fixed |
| ATLAS-A7-004 | MEDIUM | `src/core/terminal.c`, `src/core/service_decision.c` | Fixed (design change) |
| ATLAS-A7-005 | LOW | `tests/test_gate_trust.c` | Fixed |
| ATLAS-A7-006 | INFO | `~/.local/share/atlas/claude-marketplace` | Documented, not fixed |
| ATLAS-A7-007 | INFO | `src/decision/lifecycle.c` | Documented, not fixed |
| ATLAS-A7-008 | INFO | A5 reporting vs A6 reporting | Resolved (no defect) |

No CRITICAL and no HIGH finding remains open. The MEDIUM finding is a trust
boundary and was not deferred: it was resolved by removing the claim it rested
on and replacing it with a fail-closed profile.

---

## ATLAS-A7-001 — any socket peer could mint an approval capability

**Severity:** CRITICAL
**Component:** `src/ipc/server_decision.c` (`method_challenge`, `DECISION_METHODS[]`)
**Actor:** threat-model actor 3 (shell under the operator uid). Also actor 2 if
any future MCP tool or hook had ever named the method.
**Prerequisites:** the ability to open the Atlas socket — i.e. to run one process
as the uid the daemon runs as. No terminal, no capability, no prior state.

### Security property violated

A4's entire operator channel rests on one sentence: an approval capability is
obtainable *only* through the local operator channel. `LOCAL_OPERATOR_CONFIRMED`
is defined as identifying that channel. If a capability can be obtained without
going through the channel, the value identifies nothing at all.

### Evidence

`decision.challenge` was an ordinary entry in `DECISION_METHODS[]`. Its handler
performed no terminal check, no actor check and consumed no capability — it *was*
the capability source. The terminal check lived in
`atlas_service_decision_confirm()`, which is the CLI's own helper: a check a
client performs on itself, which any other client simply does not perform.

Reproduced by `tests/test_a7_authority.c::test_a_socket_peer_cannot_mint_an_approval_capability`
against a live daemon in an isolated fixture. Before the fix, the daemon answered
a well-formed request from a process with no terminal at all:

```
{"id":"cli","ok":true,"result":{"repo":"proj","repo_id":1,
 "decision":"atlas-dec-4a0f452531e65aea3ec0b911151aa43e","revision":1,
 "content_hash":"9f8e9c7c…","state":"PROPOSED",
 "token":"60b9c828b65d86c1f3f8f32794253985","confirm":"9f8e9c7c",
 "expires_at":"2026-08-10T13:38:44Z", …}}
```

That is a complete, spendable capability: token, confirmation phrase and expiry.
Sending it back to `decision.approve` completes an approval whose stored actor is
`LOCAL_OPERATOR_CONFIRMED` and whose ledger, status cache and content hash all
agree — indistinguishable, afterwards, from an approval a person made.

### Impact

Any process running as the operator's uid could approve, reject or supersede any
decision, and (via A6) revalidate one, silently and completely. Every downstream
consumer of the decision record — `atlas decision list`, the gate, an exported
decision document, a human reading `atlas decision show` — would report that the
local operator channel was used. The record is Atlas' product; this made it
forgeable through a supported interface.

Note the interaction with A6: `decision.revalidate` was covered by a test
asserting it "is equally useless without a capability only the terminal channel
can obtain". That was true of `revalidate` and false of the system, because the
capability was one method call away.

### Fix

The five operator-channel methods — `decision.challenge`, `decision.approve`,
`decision.reject`, `decision.supersede`, `decision.revalidate` — are **removed
from the dispatch table and their handlers deleted**, not left in place refusing.
An absent method is answered by the dispatcher's unknown-method case; a refusing
one is a refusal that a later edit can weaken or a second dispatch path can miss.
This is A5's argument about backups applied to the group that most needed it.

They remain available as local CLI operations under the data-directory write
lock, which the daemon holds while running, so the daemon must be stopped to use
them — a fact the kernel enforces. `apply_op()` in `src/core/service_decision.c`
no longer routes them, and the CLI says what to do.

### Regression test

`tests/test_a7_authority.c`:
- `a socket peer cannot mint an approval capability` — a live daemon, a
  well-formed request naming a real repository and decision, asserted to fail and
  asserted not to carry a `token` on the failure path either.
- `the daemon answers to no authority method` — 34 method names asked of a live
  daemon, each required to answer `unknown method`. The assertion is on absence
  rather than on failure, because "the call failed" would also pass against a
  method that exists and rejected empty parameters.

### Residual risk

A same-uid process can still run `atlas decision approve` on the local path when
the daemon is stopped. That is ATLAS-A7-004, and it is why the authority profile
exists.

---

## ATLAS-A7-002 — a model could register any directory as a trusted repository

**Severity:** HIGH
**Component:** `src/ipc/server_ai.c` (`repo.ensure`), `src/hook/hook.c`
(`ensure_repository`), `src/mcp/mcp.c` (root resolution)
**Actor:** threat-model actor 2 — a model reaching Atlas only through MCP and
hooks. No shell required.
**Prerequisites:** a Claude session started in a directory, or an MCP client
granting a root.

### Security property violated

Registration is what makes Atlas treat a directory as one it will open, read,
hash, index, run git against and answer questions about. It is a decision about
what is trusted, and it was being made by a model-triggered event.

### Evidence

`repo.ensure` took an absolute path and registered it if nothing matched. The
`SessionStart` and `DirectoryAdded` hooks called it with the session's working
directory; the MCP adapter called it for every granted root. All three inputs are
chosen by, or influenced by, the model: a session's cwd is wherever Claude was
launched, `DirectoryAdded` carries a slash-command argument, and `roots/list` is
answered by the client.

Reproduced by `tests/test_a7_authority.c::test_an_unknown_repository_is_a_candidate_and_not_a_registration`.
Before the fix, a single socket call created a registration:

```
{"id":"cli","ok":true,"result":{"registered":true,"created":true,"id":2,
 "repo":"unregistered","root":"/tmp/atlas-test-h7czyC/data/unregistered"}}
```

and `repo list` then reported `"count":2` where an operator had created one.

### Impact

An indexing pass over an unvetted tree, which is the entry point for every
actor-1 attack surface Atlas has: hostile filenames, hostile git configuration,
a hostile `compile_commands.json`, a partial-clone repository, symlink games. The
A0–A3 defences are strong, but they exist precisely because indexing an untrusted
tree is the exposure — and this let a model choose when Atlas took it. It also
grew the index, the watch set and the inotify budget on a model's initiative.

### Fix

`repo.ensure` is deleted. The hook calls `repo.resolve`, which reports and
creates nothing; the function is renamed `resolve_repository` so the call site
reads as what it does. The MCP adapter resolves granted roots against existing
registrations and refuses a tool call whose root is unregistered, with a message
saying so. `ATLAS_CLAUDE_NO_AUTO_REGISTER` is gone, because there is no automatic
registration left to turn off.

Already-registered repositories are still discovered and attached — that half
never carried authority. An unknown directory stays unknown until an operator
runs `atlas repo add`.

### Regression test

`tests/test_a7_authority.c::an unknown repository is a candidate and not a registration`
(both the `repo.ensure` call and the resulting registry count),
`tests/test_mcp_roots.c::MCP does not register a granted root`, and
`tests/test_hooks.c::DirectoryAdded does not register a repository Atlas has never seen`.
The last two are inverted versions of tests that previously asserted the
defective behaviour, which is recorded in their comments.

### Residual risk

An MCP client working in an unregistered repository now gets a refusal rather
than an automatic index. That is a deliberate loss of convenience; it is the
shape of the boundary, not a gap in it.

---

## ATLAS-A7-003 — `repo.add` and `repo.remove` were RPC methods

**Severity:** HIGH
**Component:** `src/ipc/server.c` (`METHODS[]`)
**Actor:** threat-model actor 3.
**Prerequisites:** the ability to open the socket.

### Security property violated

The same property as ATLAS-A7-002, reached by a different door, plus its inverse:
`repo.remove` discards a registration. Anything that could open the socket could
decide both what Atlas trusts and what it forgets.

### Evidence

Both were entries in `METHODS[]` in `src/ipc/server.c`, routed to the writer
thread. `tests/test_backup_live.c` contained a test asserting that a registration
naming the daemon's own data directory "still routes to it" — the routing was
considered a feature, and the authority it conferred had not been examined.

### Impact

`repo remove` does not delete decision records (A4 deliberately has no cascade),
but it does discard the index, the registration and the repository's identity
binding, orphaning its decisions. Combined with ATLAS-A7-002 it allowed a full
remove-and-re-add cycle, and `atlas_db_repo_add` reuses rowids.

### Fix

Both removed from `METHODS[]` and their handlers deleted, along with
`write_repository_result` which became unused. `repo.resolve`, `repo.list`,
`repo.state` and `repo.sync` remain: three reads and an indexing request, none of
which changes what Atlas trusts. The CLI refuses with an actionable message
naming `systemctl --user stop atlas`.

### Regression test

The 34-name enumeration in `tests/test_a7_authority.c` includes `repo.add`,
`repo.ensure`, `repo.remove` and four plausible aliases, each required to be
absent rather than merely to fail. `tests/test_backup_live.c` now asserts the
refusal and that the daemon's index gained nothing.

---

## ATLAS-A7-004 — terminal presence was treated as operator authority

**Severity:** MEDIUM
**Component:** `src/core/terminal.c`, `src/core/service_decision.c`
**Actor:** threat-model actor 3.
**Prerequisites:** shell execution as the operator's uid.

### Security property violated

None that Atlas claimed — and that is why this is MEDIUM rather than HIGH. A4's
documented contract already said, correctly, that a same-uid process able to
drive a pseudo-terminal *including an AI agent with shell access* may imitate the
channel, and that `LOCAL_OPERATOR_CONFIRMED` identifies the channel rather than a
person. The overclaim tripwire in `tests/test_decision_mcp.c` enforced that
honesty.

What was wrong is the design that honesty was describing. Atlas performed an
operation whose entire purpose is to record that a human decided, gated on a
check it had already documented as meaningless against the actor it faces.

### Evidence

`tests/test_decision_operator.c` has, since A4, allocated a pseudo-terminal with
`posix_openpt`, forked `atlas` onto it with `setsid()` and `TIOCSCTTY`, and typed
the confirmation from a program — completing a real approval. The demonstration
was in the suite the whole time; it was labelled as an honesty exercise rather
than read as a finding.

### Impact

Every approval, rejection, supersession and revalidation was available to any
process with a shell as the operator's uid, and the resulting record asserted the
operator channel had been used.

### Fix

A design change rather than a patch, in `include/atlas/authority.h` and
`src/core/authority.c`.

Authority is no longer *detected*; it is **configured outside the reach of the
principal it constrains, or it does not exist.** A grant requires all of:

1. a policy at the compiled-in path `/etc/atlas/authority.conf`, reached without
   traversing any symbolic link;
2. that file and every directory leading to it owned by uid 0 and writable by
   nobody else;
3. `operator_uid = N` in it, with `getuid() == N`;
4. the running `atlas` executable, and every directory leading to it, owned by
   uid 0 and writable by nobody else.

Condition 4 is load-bearing: if the constrained uid can replace the binary, the
probe reports whatever that uid last compiled. The policy path is a compiled-in
constant with no environment override and no flag, because a caller that can
choose the policy is not constrained by it.

Zero is `LOCKED`, so a zeroed struct is refused; there is exactly one
`state = GRANTED` assignment and it is the last statement of the probe.

In a locked profile, `decision approve|reject|supersede|revalidate` are refused
**before** the terminal is opened, before a capability is minted and before a
prompt is printed. The refusal states what is locked, why, and exactly what OS
configuration would enable it.

### Scope of the guard, and what was deliberately left unguarded

Backup create, backup restore, maintenance prune and repository registration were
considered for the same guard and **deliberately excluded**. Against the actor
this defends against, a guard on them protects nothing: the index is readable and
writable by that uid, so `cp`, `mv`, `rm` and `sqlite3` reach the same bytes with
no Atlas code involved. Verified on the deployment machine — the data directory is
`0700` and the database `0600`, both owned by the calling uid. A check an
adversary walks around with a shell builtin reads as protection in a review and
provides none, and it would additionally stop the owner of an ordinary
single-user install from taking a backup. In a *separated* deployment the
filesystem already refuses those four, so an Atlas-level check is redundant there
too.

The lifecycle is different, and the difference is what Atlas *produces* rather
than what it moves. Approving through Atlas mints a coherent record: a consumed
challenge, an append-only ledger event, a status cache written in the same
transaction, and a revision marked `LOCAL_OPERATOR_CONFIRMED`. Refusing that does
not stop a same-uid process writing `state='APPROVED'` into SQLite by hand — but
that forgery no longer agrees with the ledger, so `atlas_db_decision_verify`
reports it and `atlas doctor` surfaces it. **The guard converts an undetectable
forgery into a detectable one.** That is the whole claim and nothing more is
asserted.

### Regression test

- `tests/test_a7_authority.c::a pseudo-terminal does not open a locked profile` —
  a real pty with a controlling terminal, asserting refusal, asserting that no
  prompt was printed, and asserting the decision stayed `PROPOSED`.
- `…::every lifecycle verb is refused in a locked profile` — all four verbs, each
  required to fail with the locked-profile message, plus assertions that no
  approval was recorded and that reads still answer.
- `…::no unprivileged shape grants authority` — a policy owned by the caller, one
  naming the caller's own uid, a symlinked policy pointing at a root-owned file,
  a relative path, an empty path, `..` traversal, a doubled slash, and a directory
  in place of the file. Plus an assertion that this machine's real profile is
  LOCKED, so the rest of the suite's assertions mean what they say.
- `tests/test_decision_operator.c::the prompt is never reached in a locked profile` —
  types a confirmation *blind*, before reading anything, and asserts no
  capability row was created.
- `tests/test_gate_operator.c::a pseudo-terminal does not revalidate` — asserts
  the stale assessment stays stale, which is the consequence that matters.

### Residual risk

**Stated plainly: on this machine, and on any machine where one uid owns the
daemon, the data directory, the binary and the shell, Atlas cannot protect the
decision record from a same-uid process.** It can only refuse to help. The lock
is not a substitute for separation; it is an accurate report that separation is
absent. See "Enabling operator authority" below.

---

## ATLAS-A7-005 — a test asserted a property the system did not have

**Severity:** LOW
**Component:** `tests/test_gate_trust.c`
**Status:** Fixed

The A6 trust suite carried the comment that `decision.approve` and
`decision.revalidate` "can [not] be used by anything that has not been through
the terminal", and tested only that they refuse *without a capability*. It never
tested whether a capability could be obtained, which was ATLAS-A7-001. A test
whose comment asserts more than its assertions is worse than no test, because it
is read as coverage.

Fixed by the removal in ATLAS-A7-001 (the methods no longer exist, so the suite's
negative list now covers them truthfully) and by the 34-name enumeration in
`tests/test_a7_authority.c`, which asserts absence rather than refusal.

---

## ATLAS-A7-006 — the daemon's writable path contains executable plugin launchers

**Severity:** INFO
**Component:** deployment layout; `~/.local/share/atlas/claude-marketplace`
**Status:** Documented, not fixed

The systemd unit grants `ReadWritePaths=%h/.local/share/atlas`, and the Claude
plugin — including the shell launchers `bin/atlas-hook`, `bin/atlas-mcp` and
`bin/atlas-resolve.sh`, which Claude executes — is installed inside that
directory. A compromised daemon could therefore rewrite scripts that the user's
Claude sessions run.

Not fixed because it does not cross a boundary that is currently closed: the
daemon runs as the same uid that owns `~/.claude` and the plugin anyway, so a
compromised daemon can rewrite them with or without `ReadWritePaths`. It becomes
a real finding the moment the daemon is moved to its own uid — which is exactly
the deployment change recommended below — and at that point the plugin must be
installed outside the daemon's writable path. Recorded here so that the
separation work does not reintroduce it.

---

## ATLAS-A7-007 — capability lookup is not constant-time

**Severity:** INFO
**Component:** `src/decision/lifecycle.c` (`spend_challenge`), `src/db/db_decision.c`
**Status:** Documented, not fixed

A capability is found with a SQLite index lookup on the token, which is not a
constant-time comparison. In principle this leaks timing information about a
128-bit secret.

Not fixed, and it should not be: the token is stored in a database the attacker
of concern can simply read, so a timing side channel is not the cheapest attack
against it by a very wide margin — and every other actor cannot reach the lookup
at all. Adding a constant-time comparison would be a change that looks like
security work and addresses nothing. Recorded so that a future reviewer finds the
reasoning rather than the absence.

Capability generation itself was reviewed and is sound: 16 bytes from
`/dev/urandom` with **no fallback** — a failure to read randomness refuses to
issue a challenge rather than substituting a predictable token — hex-encoded,
bound to repository, document, revision, revision number, content hash and
intent, short-lived, and consumed exactly once inside the spending transaction.

---

## ATLAS-A7-008 — the backup-path discrepancy

**Severity:** INFO
**Status:** Resolved; no defect

A5 reported backups under `~/.local/state/atlas/backups` and the A6 report
inspected `~/.local/share/atlas`. Both were correct about the directory each
named, and Atlas has **no configured or default backup directory at all**:
`atlas backup create` takes the destination as an operand and nothing in `src/`
names a backups directory.

- `~/.local/share/atlas` is the **data directory** — the live database. It
  contains no `backups/`.
- `~/.local/state/atlas/backups/` is the **operator-chosen** directory
  recommended by `docs/operations.md`, and on this machine it holds the one
  retained backup.

No code change. The two reports were describing two different things.

---

## Areas reviewed with no finding

These were examined against the threat model and no defect was found. Recorded
because "we looked and found nothing" is a different statement from silence.

- **Socket and runtime directory** (`src/ipc/sock.c`). No `/tmp` fallback ever;
  `/run/user/<uid>` used only on proof — `lstat` (never `stat`), must be a
  directory, not a symlink, owned by this uid, with no group or other bits. The
  socket's mode is set explicitly after `bind` (which applies the umask) and then
  re-verified, and the daemon refuses to serve if it is still accessible.
  `clear_stale_socket` removes only a socket, only one this uid owns, and only
  one nothing answers on — refusing symlinks, regular files and live daemons.
  `SO_PEERCRED` is read at accept and a foreign uid is refused. `sun_path`
  overflow is refused rather than truncated.
- **Frame codec and JSON parsing.** Bounded request and response sizes, bounded
  depth, bounded method and id lengths, non-blocking per-connection state so one
  partial header cannot stall other clients, integer-overflow-safe length
  handling, malformed input answered rather than fatal. yyjson is confined to
  `src/ipc` behind one facade.
- **Git execution** (`src/git/git_harden.c`, `src/git/git.c`). Four layers, of
  which the argv allowlist is explicitly the weakest; constructed environment
  asserted sanitized on every invocation; `-c core.fsmonitor=false` and the rest;
  per-subcommand flags placed after the subcommand; `config` covered by a
  positive allowlist of complete argument vectors; partial clones refused at
  open; exact fail-closed promisor detection. No shell anywhere in the codebase —
  no `system()`, no `popen()`, no `/bin/sh -c`.
- **Filesystem safety in backup and restore** (`src/core/service_backup.c`).
  Every component opened from `/` with `O_NOFOLLOW`; `realpath(3)` deliberately
  absent; mode-0600 `O_EXCL` temporary in the destination directory; verified in
  full before publication; `fsync` then rename; the previous write-ahead log
  renamed aside rather than deleted so a failed rename can put it back.
  `ATLAS_BACKUP_FAULT` is compiled into every build so the shipped binary is the
  one the failure tests ran against, and can only ever abort.
- **Migrations.** Numbered and transactional; schema 6→7 exercised by
  `tests/test_migrate7.c`, including that a schema-6 backup remains verifiable by
  the A7 build and that `atlas_db_backup_inspect` never opens the artefact as a
  migrating handle — a diagnostic that upgrades what it was asked about has
  destroyed the evidence.
- **Untrusted text.** `atlas_safe()` at every terminal and JSON boundary, with
  the double-encoding rule documented per field at the top of both renderers; the
  A2 envelope's closed five-kind vocabulary, which validates rather than escapes
  and replaces a value that is not the shape it claims to be.
- **Systemd unit** (`src/core/unit.c`). Absolute `ExecStart`, `UMask=0077`,
  `RuntimeDirectoryMode=0700`, `NoNewPrivileges`, `ProtectSystem=strict`,
  `ProtectHome=read-only` with a single `ReadWritePaths`, `MemoryDenyWriteExecute`,
  `SystemCallFilter=@system-service` minus `@privileged @resources @obsolete`.
  Three directives a system manager could apply are *deliberately absent with
  written reasons* rather than present and ineffective — including
  `ProtectKernelModules=yes`, which fails a user unit outright with
  `218/CAPABILITIES`. That fix is retained. Verified by starting a real unit in an
  isolated `XDG_*` environment, not by reading the text.

---

## Enabling operator authority

Atlas will not do any of this to a machine by itself; each step needs root and
each is a deployment decision.

1. **Give the daemon its own principal.** Create a service account, move the data
   directory to it, and run the user unit under that account (or a system unit
   with `User=`). The model's uid then cannot open `atlas.db` at all, which is
   what makes the four operations excluded from the guard genuinely protected —
   by the kernel, rather than by an Atlas check.
2. **Install the policy.** `/etc/atlas/authority.conf`, owned by root, mode
   `0644`, on a path whose every component is root-owned and not group- or
   other-writable, containing `operator_uid = <the human's uid>`.
3. **Install the binary immutably.** `atlas` owned by root and not writable by
   any other uid, on a root-owned path — `/usr/local/bin/atlas`, not
   `~/.local/bin/atlas`. Without this, condition 4 of the probe fails and the
   profile stays locked, which is correct: a replaceable binary makes every check
   in it replaceable.
4. **Move the Claude plugin out of the daemon's writable path** (ATLAS-A7-006).

Until step 1 is done, the human operator and the AI worker are the same OS
principal and no software can tell them apart. `atlas doctor` reports the profile
state and the reason on every run.

## What A7 does not claim

- That a locked profile prevents a same-uid process from corrupting the index,
  the registry or the backups. It does not, and the guard was deliberately
  scoped to avoid pretending otherwise.
- That `LOCAL_OPERATOR_CONFIRMED` identifies a person, establishes presence, is
  a signature, or provides non-repudiation. It names a channel.
- That a granted profile has been exercised end to end here. It has not: a grant
  requires a root-owned policy, which no unprivileged test can create, so the
  suite exhaustively tests every shape that must *fail* and cannot test the shape
  that must succeed. This is stated rather than papered over, and it is the one
  piece of A7 that a machine with real separation must verify on first use.
- That the review found everything. It is a review, not a proof.

---

# A7.1 addendum — what OS separation changed

A7.1 deployed the separation this review recommended. Two findings above change
status; the rest are unaffected.

## ATLAS-A7-006 — resolved

The plugin was inside the daemon's `ReadWritePaths`. In the separated deployment
the daemon runs as `atlasd` with `ProtectHome=yes` and
`ReadWritePaths=/var/lib/atlas`, so it has no path to the Claude plugin, either
home directory, the installed binary, the policies, the backups, or any indexed
repository. The plugin may still be written by the untrusted model principal —
it is not part of the trusted computing base, and modifying it cannot expand
daemon or database authority, because the only thing it can reach is a socket
whose whole vocabulary is reads and proposals.

## ATLAS-A7-004 — residual risk narrowed, and re-scoped

A7 recorded: "on a machine where one uid owns the daemon, the data directory,
the binary and the shell, Atlas cannot protect the decision record from a
same-uid process."

That machine no longer exists for the principal that matters. `atlas-worker`
cannot open `/var/lib/atlas/atlas.db` at all — mode 0700, owner `atlasd` — so
the forgery A7 could only make *detectable* is now prevented by the kernel
before Atlas is involved.

The residual risk is re-scoped rather than removed, and the operator has
explicitly accepted its shape:

- **`nocdem` holds `NOPASSWD: ALL` and is trusted by design.** It can read and
  write the index, become `atlasd`, replace the binary and approve anything.
  A7.1 does not defend against this and no test asserts otherwise. The
  separation is meaningful because A8's dispatcher and every persistent model
  process run as `atlas-worker`.
- **`atlasd` is trusted with the index.** A compromise of the daemon itself is a
  compromise of the index. Nothing in A7.1 changes that, and the sandbox exists
  to bound what such a compromise could reach beyond it.

## The scope argument in A7-004 is unchanged, and now has a second half

A7 declined to guard backup, restore, prune and registration because `cp`, `mv`,
`rm` and `sqlite3` reached the same bytes, making a guard cosmetic. In the
separated deployment those verbs are prevented for `atlas-worker` by the
filesystem — which is precisely the argument A7 made for *not* adding the Atlas
check: the right layer was the OS, and A7.1 is where it was applied.
