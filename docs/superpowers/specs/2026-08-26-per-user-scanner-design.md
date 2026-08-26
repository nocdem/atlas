# A13 — the per-user scanner

**Status:** design, awaiting operator review. Nothing implemented.
**Date:** 2026-08-26
**Deploy baseline:** `main@2a80327`

> **A FACT ABOUT FILES IS A CLAIM BY WHOEVER READ THEM.**

Atlas' daemon runs as `atlasd` and must read repositories owned by other
people. It cannot, and no amount of repairing permissions keeps it able to.
This season stops trying: the process that reads a repository is the
principal that owns it, every observation names its observer, and `atlasd`
validates shape and never truth.

---

## 1. The defect, measured

`atlasd` (uid 994, groups `atlasd` + `atlas-clients`) is not in `nocdem` and
not in `cpunk-team`. On this machine, on 2026-08-25:

| Where | What `atlasd` cannot do | Count |
| --- | --- | --- |
| `/opt/atlas/.git` | read (modes 400/600), enter (700) | 112 files, 2 dirs |
| `/opt/atlas` worktree | read a tracked file (600) | 1 |
| `/opt/dna` | enter (700/770), read (600/640) | 59 dirs, 95 files |

The consequence was not a warning. `git -C /opt/atlas rev-parse HEAD` as
`atlasd` answered `fatal: not a git repository`, because `.git/HEAD` was
unreadable — so the daemon logged, every ten seconds for hours:

```
warn  repository atlas cannot be opened: /opt/atlas is not inside a git working tree
```

**The observer's permissions became the repository's facts.** Atlas recorded a
statement about `/opt/atlas` in which nothing was true of `/opt/atlas`.

`/opt/dna` reached a second, quieter failure: passes hashed all 22 009 files
and still could not clear the event gap, because 59 directories were never
entered and a pass that did not read every eligible file may not clear one.
`index_current` was false permanently, and the reason surfaced only as
*"build-input discovery is PARTIAL: a directory could not be entered"* — no
path, no count, no remedy.

### Why permissions cannot be repaired into a fix

Measured, not argued:

- **ACLs fix the snapshot, not the future.** A default ACL is inherited by new
  children of covered directories; a new *top-level* directory is not covered,
  and every session writing under a restrictive umask creates fresh
  unreadable objects. We applied the full ACL manifest during the P0
  deployment and it worked — 115 access targets, 320 directories — and it
  would need reapplying for the rest of the repository's life.
- **Group membership cannot work.** 117 of the blocking paths are mode 700 or
  600, where the group bits grant nothing at all; and `/opt/atlas` and
  `/opt/dna` are 2775/775 with group `cpunk-team`, so putting `atlasd` in that
  group would grant the daemon **write** on both repositories — destroying the
  guarantee that Atlas never modifies a registered repository.
- **`CAP_DAC_READ_SEARCH`** ends the problem class but lets a compromised
  daemon read every file on the machine. Rejected.
- **The `umask 0077` has no configuration source.** `/etc/profile`,
  `/etc/profile.d`, `pam_umask` and the ssh unit define no umask;
  `~/.profile`'s is commented out and `~/.bashrc:114` sets `002`. A fresh
  interactive login shell measures `0002`, a non-interactive one `0022`.
  Neither is `0077`. There is nothing to correct, so there is nothing that
  stays corrected.

---

## 2. The shape of the fix

A repository record gains `scanner_uid`. A **scanner** process running as that
uid watches the repository, reads and hashes files, runs git, and hands what
it read to `atlasd` over the existing socket.

Because the scanner runs as the owner, a file the owner created mode 600 is
readable by definition. The bug class does not get patched; it stops existing.

### The invariant is about the principal, not the process

The tempting sentence is *"`atlasd` never opens a repository file."* It is the
wrong one. It would force a **per-user install** — where the daemon already
runs as the operator and can read everything — to start a second process that
adds nothing. The rule this season actually establishes is:

> **File facts are produced by a principal that can read the files, and the
> record says which principal produced them.**

In a system deployment that means a separate scanner process, because the
daemon's principal cannot read the tree. In a per-user install the reader, the
invoker and the database's owner are the same principal, so one process
satisfies the rule — and the record still names it. That is not a second code
path kept for compatibility; it is the same rule with the same answer, reached
with one process instead of two.

What is removed without exception is `atlasd` reading a tree belonging to a
principal it is not. Every observation carries its observer either way.

```
  /opt/dna (owner nocdem)
        │  reads as nocdem
        ▼
  atlas-scanner  (systemd --user service, uid 1000)
        │  observations + requested bytes, over /run/atlas/atlas.sock
        ▼
  atlasd (uid 994)  ── writes ──▶  /var/lib/atlas/atlas.db
```

### What the scanner sends: content, not conclusions

The scanner reports observations — `path_raw`, the eight-field filesystem
identity, the content hash, git facts, live watch state — and, **when
`atlasd` asks for them, the file's bytes**, keyed by content hash.

This matters more than it looks. A scanner that sent only conclusions would
kill four subsystems that need file *contents* in the daemon:

| Subsystem | Where it reads the tree today |
| --- | --- |
| A8 workspace snapshots — and with them A8/A11.x/A12 | `src/orch/snapshot.c:7` — *"Everything here runs as `atlasd`, inside the daemon"* |
| Daemon-served gates (`gate.check`) | `src/core/service_gate.c:242` runs git for live HEAD |
| A3 lexical index | `src/code/index.c:190` opens files in the worker pool |
| A8-CI / A9.2.3 semantic index | `src/sem/index.c:750`, `src/sem/discover.c:758` |

Three of the four work directly from bytes. The fourth does not, and it is why
the bytes are kept rather than discarded. Verified by reading the code, not
assumed:

| Subsystem | Works from bytes? | Why |
| --- | --- | --- |
| A8 snapshots | **yes** | it writes files into a workspace; bytes are the input |
| Gates | **yes** | it needs git facts (live HEAD), which the scanner reports |
| A3 lexical | **yes** | `atlas_code_extract(const void *data, size_t len, …)` already takes a buffer; `open_nofollow` in `src/code/index.c:190` is only how it currently obtains one |
| A8-CI semantic | **no** | `src/sem/clangparse.c:837` calls `clang_parseTranslationUnit2(…, NULL, …)` — `unsaved_files` is NULL, so libclang opens the translation unit **and its whole include closure from the real filesystem, by path** |

Bytes keyed by hash are not a filesystem, and libclang needs one. **The
resolution is a mirror.**

### The mirror

`atlasd` keeps its own copy of each repository's tracked content under its
data directory, owned by `atlasd`, mode 0700. The scanner keeps it current:
every observation that reports changed content is followed by the bytes, and
`atlasd` writes them into the mirror. Deletions delete.

Everything the daemon does then reads from a real filesystem it owns:

- libclang parses translation units and their include closures from mirror
  paths, in the **same bounded child running as `atlasd`** it uses today. The
  system include closure lives in `/usr/include` and is world-readable, so it
  needs no mirroring.
- A8 workspace snapshots copy out of the mirror.
- A3 lexes mirror bytes.
- Nothing in `src/sem`, `src/code` or `src/orch` learns a new concept; the
  repository root they open becomes the mirror root.

**Measured cost — the mirror holds tracked content only:**

| | files | bytes |
| --- | --- | --- |
| `/opt/atlas` tracked | 394 | 12 631 527 (13 MB) |
| `/opt/dna` tracked | 2 009 | 119 215 (117 KB) |
| **mirror total** | **2 403** | **≈ 13 MB** |

Against a 3.0 GB `atlas.db` and 165 GB free, this is not a cost worth
optimising. It is incremental besides: only changed files are rewritten.

**It is tracked content, not everything Atlas currently indexes.** `dna`'s
index holds 22 009 files against 2 009 tracked; the difference is eight build
trees (`nodus/build-o15d-default`, `build-d2`, `build-d4`, `build-fault`, …,
≈ 2 500 files each) that are not in that repository's `.gitignore`. Mirroring
the full working tree would cost 6.7 GB for `dna` alone, almost all of it
compiler output. The mirror is defined by `git ls-files`, and a repository
whose ignore rules admit its own build output is a fact about that repository,
not a reason to hold 6.7 GB.

### What the mirror costs, stated

- **A second copy of private content.** §5.2 already records that owner-private
  file contents enter the index; the mirror is where they physically live.
  0700, owned by `atlasd`, inside the data directory the backup subsystem
  already covers.
- **Drift is a correctness risk with a name.** If the mirror falls behind the
  real tree, Atlas describes the mirror and not the repository. The defence is
  that the mirror is written from the same observations that move the
  generation — the content and the facts about it arrive together, or the
  generation is abandoned. A mirror write that fails fails the pass.
- **Untracked working-tree state is not mirrored**, so anything the daemon
  does that needs untracked content sees only what the scanner reported about
  it, not its bytes. Dirty-state reporting is a fact, not a content read, so
  this is a bound rather than a regression — but it is a bound, and it is
  written here.

Rejected alternatives, for the record: `CXUnsavedFile` for the whole closure
is circular, because the closure is not known until after a parse. Running the
semantic pass in the scanner would parse hostile repository content as the
owner's account — which on this machine holds passwordless root — and the
existing bounded child as `atlasd` exists precisely to prevent that.

The added trust claim — *"these bytes are what is at path P"* — is byte for
byte the claim already accepted when a reported hash is trusted. Nothing new
is conceded by sending the content.

### Bytes are pushed with the observation that made them stale

A scanner that reports a file's content changed sends that file's bytes in the
same batch. `atlasd` writes them into the mirror (§ *The mirror*, below) as
part of applying the generation, so the content and the facts about it land
together or the generation is abandoned. There is no separate fetch round
trip, and no window in which the index describes content the mirror does not
hold.

`atlasd` may still *name* hashes it wants in a poll directive — after a
restore, or when a mirror file is missing — but that is repair, not the normal
path.

---

## 3. Identity, and the one rule that carries the design

### `scanner_uid` is the owner of the repository root

`atlas repo add` stats the root and records its owner. `--scanner-uid`
overrides; the chosen uid is always printed.

Measured on this machine: `/opt/atlas` → `nocdem`, `/opt/dna` → `nocdem`.
Both correct.

It is **not** derived from who ran the command. There is no peer to ask:
`repo.add` is not an RPC method — A7 deleted it (`docs/daemon-and-ipc.md:346`)
— so registration is a local write under the data-directory lock with the
daemon stopped, and which uid performs it depends on how Atlas is deployed
(the operator's own uid in a per-user install, `atlasd` in a system one).
An answer that changes with the deployment is not an identity. The root's
owner is the same answer either way.

`repo_identity_hash` needs root commits, which needs git, which the
registering process may not be able to run on that tree — the measured
failure *is* this. Identity therefore moves to first ingestion, which A4
already licenses: *"Detach at registration, attach after ingestion, and never
guess."*

### The load-bearing rule

> On every `scanner.` call, `atlasd` compares the peer's uid from
> `SO_PEERCRED` against the target repository's `scanner_uid` and refuses on
> mismatch.

Without it, one user's scanner could fabricate facts about another user's
repository. Nothing else in this design substitutes for it.

**Stated honestly:** a database trigger can check that a stored observer uid
matches `repositories.scanner_uid`, but SQLite CHECK constraints cannot
reference another table, and the uid's *origin* is C code reading
`SO_PEERCRED`. So this is **not** A11.0's claim, where the schema refuses the
write on its own with the C check disabled. The schema enforces consistency of
what C wrote; only C establishes the peer. Say the weaker thing.

### Refusals

`scanner_uid` may never be the orchestration worker uid, the gateway uid, or
`atlasd` — read from the root-owned `orchestration.conf` and `gateway.conf`.

The reason is precise, and it is the same one the whole design rests on. This
season is safe because the reporting principal **owns the files it reports**:
whatever it could misreport, it could equally write. `atlas-worker` owns none
of these trees. Letting it scan would break that equivalence — it would be
reporting on files it cannot write, which is the one case where a report is
worth more than the reporter already has. The refusal keeps the reporter and
the owner the same principal.

**Scoped to system deployments only.** In a per-user install the daemon's uid
*is* the user's uid, and refusing it would forbid the entire deployment mode.

### Uid reuse is self-consistent

A recreated user holding uid N also owns the old files, because ownership is
numeric. Scanner authority follows file ownership by construction. Store the
number, never the name — a username is a mutable mapping pretending to be an
identity. A `scanner_uid` that no longer exists is a visible reason on
`repo.state` and an `atlas doctor` finding, never silence.

---

## 4. The protocol — three methods

A new `scanner.` group on the existing socket. The dispatch in
`src/ipc/server.c:1176-1230` is already additive by uid — operator, gateway and
dispatcher groups coexist — so this follows the established pattern and is
hidden the way the dispatcher group is: a peer not in it gets `unknown method`.

**Decisions stay in `atlasd`. The scanner asks what is owed and does it.** The
"what is owed" computation is a pure read, modelled on A9.2.3's
`atlas_sem_plan_for`.

| Method | Carries |
| --- | --- |
| `scanner.poll` | Assignments (which repositories are this uid's), per-repository directives (full pass / incremental / idle), the **generation claim token**, watch budgets, `max_file_bytes`, declared exclusions, and **the content hashes `atlasd` wants**. Doubles as heartbeat. |
| `scanner.observe` | A batch of observations for one claimed generation, plus requested blobs. Carries a batch sequence number and, on the last batch, a **final summary**: the basis for `content_verified` and the end-of-pass HEAD re-read. |
| `scanner.watch` | Live watch state: counts, degraded reason, owed gaps, and obstacle paths. Not generation-scoped. |

There is no `hello`. A repository inventory is stale the moment the registry
changes — P0's own sentence, *"the inventory is not an authority on a path
that did not exist when it was read"* — and `poll` must re-answer assignments
anyway.

### Ordering, idempotency and the clock

- **Generation claim tokens.** A directive carries a per-repository token;
  `observe` must present it. A second claimant forces the first to abandon and
  records a gap. Two writers never share one generation — the case is real: a
  systemd user unit plus a manually started scanner.
- **Batch sequence numbers are mandatory, not optional.** O10's rule is that a
  retry makes one row, proved at the boundary. `scanner.observe` meets
  A9.2.6/A9.2.7's two distinct failures: `BUSY:` means nothing was queued and
  a resend is safe; a timeout means the write is still on its way and a resend
  duplicates. The scanner cannot always tell a timeout from a lost answer.
- **`observed_at` travels with every observation, from the scanner's clock.**
  `identity_is_stable` (`src/core/reconcile.c:100-109`) compares stat
  timestamps against the observation instant. Evaluated against `atlasd`'s
  clock over transport-delayed observations, every fresh file reads racy — or,
  under skew, a genuinely racy one reads stable and *"a racy observation is
  stored as unknown, not as a value"* is silently lost. The rule stays in
  `atlasd`; its input comes from the scanner.
- **The end-of-pass HEAD re-read is ordered after the last batch**, so A1's
  *"a pass re-reads HEAD before committing; if it moved, the pass is
  abandoned"* keeps its input.

### Path validation is lexical, and says so

`atlasd` cannot canonicalise a root it cannot traverse. "Belongs to this
repository" is therefore a byte-shape check over reported relative paths: no
leading `/`, no NUL, no `.` or `..` component, bounded length and count.
`atlas_snapshot_path_ok` (`src/orch/snapshot.c:38-60`) is exactly this check —
reuse it rather than writing a second one. Paths are bytes; `%XX` only at
display.

---

## 5. What this costs — written down, not buried

### 5.1 SOURCE evidence records who observed it

A scanner reports what it read, and `atlasd` cannot check the report against
the file — that is the design, not a weakness in it. **Nothing is conceded by
this.** A principal that can write a file already decides what that file says,
so an index describing that file was never a record the file's owner could not
control. Atlas gains no new exposure to the owner; it stops pretending the
reader was someone else.

The one residual, stated because Atlas states residuals: a scanner's report
and the disk can disagree without the disagreement being visible — git shows
nothing, and a later incremental pass compares metadata rather than content
(`src/core/reconcile.c:615`), so the divergence can persist until a
content-verifying pass. Editing the file instead leaves a trace a human or a
diff would catch. This is a detectability difference against the owner, not a
capability the owner did not already have, and it is not worth a mechanism —
A7 forbids the only one that would help: *"Nothing observable inside a process
distinguishes a human from a program running as the same uid. Do not add a
check of that shape."*

What follows is **provenance, not defence**. Invariant 3 requires every result
to preserve provenance, and a fact Atlas read is not the same fact as one a
scanner reported. So evidence carries its observer, and pre-cutover rows are
never relabelled — inventing a provenance nobody recorded would be migration
19's mistake: *"a default carries no information, and inventing an intent
nobody expressed is the one thing a migration must not do."*

### 5.2 Private file contents enter the index

`atlasd` cannot read a mode-600 file today, so its content is absent from the
index. After this season it is present, and anything that can read the index
or reach the socket can see it. This is the price of indexing what the owner
owns.

### 5.3 `index_current` becomes an attestation

Selection stays in `atlasd`, but the *execution* of *"a path the watcher named
is always hashed"* and *"'full' means content verification"* is performed by a
process `atlasd` cannot audit. `content_verified` is computed from what the
pass *did* (`src/core/reconcile.c:1278`); it will be computed from what the
scanner *said* it did. `docs/watcher-consistency.md` must say so.

### 5.4 Absence of a scanner is UNKNOWN coverage, never sufficiency

A repository whose scanner is absent, disconnected or heartbeat-expired has
**UNKNOWN** coverage — never "stale but sufficient" — because A9.2.2's rule is
that UNKNOWN coverage is never sufficient for an ABSENT verdict. Scanner death
is an event gap, persisted like every other P0 waiting window. P0's overlay
(*"it only ever subtracts"*) generalises cleanly: an absent scanner subtracts
everything.

### 5.5 The symlink risk inverts

The scanner has **more** authority over the owner's account than `atlasd` ever
had. A hostile repository symlinking `~/.ssh/id_rsa` gets `EACCES` from
`atlasd` today and a successful read from a careless scanner tomorrow, with
size and hash then visible through query surfaces. Every A0/A1 hardening —
`atlas_path_open_nofollow`, hashing a symlink's *link text*, the constructed
git environment, `-c core.fsmonitor=false` — becomes more load-bearing, not
less. `make adversarial` and `fx_install_marker` must run against the scanner
path.

---

## 6. The watcher moves, and P0's budget is re-derived

`src/daemon/watch.c` is 3 486 lines and all of it moves process: subscriber
sets, the priming frontier, the ignore inventory, the pending-ignore queue.
`inotify_add_watch` must be called by a uid that can traverse the tree.

**P0's arithmetic does not survive the move unchanged.** Its rule reads:

> half of `max_user_watches` under a root-owned system policy, a fifth
> otherwise, because a dedicated `atlasd` has no other consumer of that uid's
> budget

Scanners run as user uids that have exactly the other consumers the smaller
share exists for — editors, IDEs, language servers. The 50 % claim is
invalidated at the process boundary, and the daemon-wide pool becomes one pool
per uid.

**Resolution:** `atlasd` computes every budget and ships it as a poll
directive; the scanner installs what it is told and reports `ENOSPC` as
`kernel_limit`. This preserves *"nothing outside `watch.c` decides how many
watches exist"* — the decision simply travels — and it repairs P0's test
channel at the same time: the budget rides the protocol, so tests drive it
without a flag, an environment variable or a policy key.

Two genuine improvements to claim:

- `IN_Q_OVERFLOW` is per inotify instance, so one uid's overflow no longer
  gaps every repository on the machine.
- `fs.inotify.max_user_watches` is per uid, so a 33 128-directory repository
  no longer competes with anything else Atlas watches.

And one defect this season retires: the P0 deployment measured
`kernel_max_user_watches` as **1024** against a real value of **122 910**,
because `ProcSubset=pid` hides `/proc/sys` inside the daemon's namespace. A
scanner is an ordinary user process and reads the real number.

---

## 7. Deployment and lifecycle

Atlas already established this pattern for its own daemon.
`docs/systemd-user-service.md` opens: *"Atlas runs as **you**, not as root."*

```sh
atlas scanner install --user
systemctl --user daemon-reload
systemctl --user enable --now atlas-scanner
```

**No root, no sudo.** The owning user installs and starts their own scanner;
`loginctl enable-linger` (already on for `nocdem`) keeps it alive across
logout. One scanner per uid, watching every repository with that
`scanner_uid`. `atlas scanner install` **writes** a unit and does nothing else
— CLAUDE.md's rule that nothing installs, enables or starts a real systemd
service from code or from a test is unchanged.

Bootstrap order, printed by `repo add` and reflected in `repo.state` so the
missing step is named rather than guessed:

```
1. atlas repo add /opt/dna          (daemon stopped)
2. start the daemon
3. atlas scanner install --user; systemctl --user enable --now atlas-scanner
4. first pass
```

`atlas doctor` reports scanner connectivity per repository as a finding. It
observes and creates nothing, and it starts no scanner.

`sync_seq` flows through poll directives so `atlas sync --wait` keeps meaning.
Startup's forced content-verifying pass becomes a directive the scanner
fulfils; the obligation stays persisted daemon-side, which is where it
belongs.

---

## 8. Schema

One migration, number 27.

- `repositories.scanner_uid INTEGER` — nullable only for the migration
  window; a repository without one is registered and never current.
- An observer column on evidence, per §5.1, distinguishing pre-cutover rows
  from scanner-attested ones. Pre-cutover rows keep their existing meaning and
  are not rewritten.
- Generation claim tokens and last-seen batch sequence, per repository.
- Scanner liveness: last poll, last observation, connection state.
- A trigger asserting a stored observer uid equals `repositories.scanner_uid`
  — with §3's honest caveat about what a trigger can and cannot establish.

Backfill for the two existing repositories: `scanner_uid` from the root
directory's owner, printed at migration time, with the registered value shown
in `repo list` so an operator can correct it before the first pass.

---

## 9. Testing

- `fx_scanner_start` beside `fx_daemon_start`; the daemon suite's
  serialisation rationale (inotify budget contention) now applies to scanners
  too.
- A fixture that proves the load-bearing rule: a scanner for uid A is refused
  when it speaks about a repository whose `scanner_uid` is B.
- A fixture that proves two claimants on one generation force an abandon and a
  gap, not two writers.
- Retry proof at the boundary, O10's discipline: one submission, one row,
  across `BUSY:` and across a timeout.
- The adversarial suite run against the scanner path (§5.5), including a
  repository that symlinks outside itself.
- A test that a repository with no scanner never reports `index_current=true`
  and never settles an ABSENT verdict.
- The existing A1 cache-hit tests re-pointed at observations carrying
  `observed_at`, including the racy-observation case.

---

## 10. Explicitly out of scope

- Multiple scanners per repository. Mixed ownership within one tree — real
  today: 21 files in `/opt/dna` owned by `bios`, the rest by `nocdem` — is
  **not solved**. The scanner reports what it cannot read as an obstacle with
  its exact path and the repository goes degraded. Honest, and visibly
  unfinished, rather than hidden.
- Any change to `atlas.service`'s sandbox, to `ProcSubset`, or to any sysctl.
- Any repair of the `umask 0077` that produced the measured damage. There is
  no configuration source to repair (§1).

---

## What implementation changed, and why

This section is written *after* the season shipped, from what the first live
runs proved. It is not a revision of the argument above; it records where the
built thing differs from the design and what forced each difference. Where the
two disagree, this section is what the code does.

### The scanner ships bytes, not observations

The design had `scanner.observe` carry file identities, hashes and status
entries, and had `atlasd` rebuild the index from them. The operator chose bytes
instead, and counting settled it: `src/core/reconcile.c` calls **twenty**
distinct git operations. Reproducing them over a socket does not move data — it
moves the *execution* of A1's cache-hit rules, the ones `CLAUDE.md` marks "do
not weaken these", into a process the daemon cannot audit.

So the scanner writes a **mirror**: the tracked tree, the untracked tree, and
`.git`, at `<data_dir>/mirror/<repo_id>`. The daemon opens it with
`atlas_git_open` and asks it every question it asks a real repository. Measured:
git works unchanged on a copied `.git` — `rev-parse HEAD` returns the same
commit, `ls-files` the same paths, `status` reports zero changes. A3, the
semantic layer, snapshots and gates were not touched.

`scanner.observe` therefore does not exist. `scanner.put` carries one chunk of
one file, hex-encoded because a JSON string carries neither invalid UTF-8 nor a
C0 control unchanged — measured, a twelve-byte file arrived as twenty-four.

**Stated cost:** the daemon holds a copy of the whole history, not only current
content. §5.2 already recorded that owner-private content enters the index;
this widens it from the working tree to the object store.

### The row decides the source, and there is no fallback

The design left "which tree does the daemon read" implicit. The first
implementation tried the real root and used the mirror when the root refused,
and that was wrong: both real failures on the machine this season was built for
were **partial**. A hundred loose objects at mode 0400, so `atlas_git_open`
succeeded and `git log` failed three calls later; and fifty private directories
that could not be entered, so every pass completed and covered less than the
tree. A rule keyed on "could not open" answers neither.

So: a repository naming a scanner is read from its mirror and from **nothing
else**. No mirror is a refusal, an incomplete mirror is a refusal, and the tree
is never the way out of either. A process running *as* the scanner uid still
reads the tree — that is a capability question, not an authority one.

### `scanner.state`, `mirror_complete` and `mirror_at`

Not in the design, added because the daemon reads the mirror *as* the
repository: every file the mirror does not hold is a file that no longer
exists. Measured on the first live run — a mirror carrying 2007 of a tree's
22012 files made the daemon record **20000 deletions**.

`mirror_complete` is the run's own claim: every file it enumerated was written
and none was skipped. Cleared when a run starts, set only when one finishes, so
a crash leaves it false. The asymmetry is the design: false costs a refusal,
true would cost a delete sweep against a half-written tree.

`mirror_at` is when that run finished. The write goes through the writer thread,
because every dispatch handle is read-only — doing it on the dispatch handle
failed on every call with "attempt to write a readonly database", silently,
because the scanner ignored the result.

### `repo.scanner` is an RPC

The design assumed A7's rule that the registry is local-only, because "the
socket carries no authority: every peer on it is the same uid as the daemon".
**A7.1 ended that premise** by splitting the principals. `repo.scanner` is in
the operator-uid group — selected by `SO_PEERCRED` against the root-owned
policy — where `backup.create`, `code.index` and every `dispatch.` method
already sit. `repo add` and `repo remove` are deliberately unchanged: they
decide which directories Atlas will read at all.

### `poll` is the heartbeat, and the cadence is Atlas'

The design says `scanner.poll` "doubles as heartbeat", and an implementation
that had the scanner declare its own cadence was written and reverted. The
reason it is wrong: a scanner that declares a cadence and then dies leaves its
promise standing, and the daemon goes on trusting the mirror because a dead
process said it would be back. A scanner that stops polling simply stops being
heard.

The heartbeat is held in memory rather than in the index. A daemon that has
just started has heard from nobody, which is the conservative answer; a
persisted heartbeat would let it trust a scanner that died before it.

Both numbers are **derived from `ATLAS_WATCH_RECONCILE_INTERVAL_MS`**, which is
already Atlas' answer to "how long may a repository go without being
re-examined". The staleness bound is that period; the poll cadence is half of
it, so a scanner keeping the cadence has one whole missed poll of margin.
Neither is a guess about how often anybody's tree changes.

### There is no file size bound

There was one, twice — 8 MiB, then 64 MiB — and both were invented rather than
derived. Both sat below Atlas' own `ATLAS_HASH_MAX_FILE_BYTES` of 256 MiB, so
the scanner refused to mirror files Atlas would have indexed. On the first live
run one of the two files it refused was a **91 MiB pack**, which left the
mirror's `.git` incomplete and therefore not a repository at all.

Atlas' bound is on *hashing*, not on *existing*: above it reconcile records
`ENTRY_TOO_LARGE` and the file is still there. The mirror has to be there too.
A bound here protects nothing and converts a large file into a repository that
cannot be indexed. With it removed, both repositories mirror with zero skips.
