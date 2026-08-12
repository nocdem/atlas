# Operations: backup, verification, restore and retention

Atlas is a local engineering-memory system. Its SQLite index is a *rebuildable*
index of a repository — that is architecture invariant 1 and it still holds —
but three kinds of thing in it cannot be rebuilt from any repository:

- **decision documents**, their immutable revisions and their append-only
  lifecycle ledger;
- **AI reasons, proposals and their attribution**, including the explicit
  `UNKNOWN` rows written for changed paths nobody explained;
- **evidence and provenance**.

Those are the record. Everything in this document exists because losing them is
not the same kind of event as losing a file index, and because "we have a copy
of `atlas.db`" is not the same thing as having a backup.

---

## What a backup is

One self-contained SQLite database file, produced through SQLite's online backup
API from a read-only connection to the live index.

```
atlas backup create ~/backups/atlas-2026-08-07.db
atlas backup verify  ~/backups/atlas-2026-08-07.db
atlas backup restore ~/backups/atlas-2026-08-07.db --yes
```

`--force` lets `create` replace an existing destination; without it an existing
file is refused. `restore` requires `--yes`.

**`restore` is a local CLI operation with no RPC method**, and no MCP tool or
hook can create, read or restore a backup. A model that can call every method in
the ordinary group still cannot replace the index. The absence is structural —
no such method exists — rather than a check that could be forgotten, and
`tests/test_backup_live.c` proves it by asking a live daemon for each of the
names such a method would plausibly have.

`create` and `verify` are additionally served over the socket to the operator
uid the root-owned policy names, because under a system deployment the index is
`0700 atlasd` and the operator account could otherwise not take a backup of its
own index at all.

### Long backups do not fail just because they take a while

A backup of a large index takes tens of seconds — 32 s to write and 15 s to
verify an 815 MiB index on the reference machine. Over the socket both `create`
and `verify` are **accepted, then polled**: the request returns an operation id
as soon as the work is queued, and the command waits.

Verification is in that list because it reads every page: `PRAGMA
integrity_check` walks the b-trees and every decision revision is rehashed.

This is worth stating plainly because it used to be wrong. The work ran inside
the request, the client's frame read timed out at 10 s, and `atlas backup
create` reported `timed out while reading a frame header` and exit 1 — while the
daemon went on to write and verify a complete, correct backup. A success
reported as a failure is worse than a failure, because the next thing anybody
does about it is re-run it or work around it.

What follows from the split:

* **Interrupting the command does not cancel the backup.** The work holds no
  reference to your connection. The command prints the operation id to stderr
  before it starts waiting, so an interrupted run has already told you what to
  ask about: `atlas operation status ID`. You can also just verify the file.
* **Ids are not small counters.** Each daemon seeds them above every id any
  previous daemon issued, so an id from before a restart is reported unknown
  rather than resolving to a different operation.
* **Success is reported only after the backup is complete and verified.** The
  daemon verifies before the operation is allowed to reach SUCCEEDED.
* **Asking twice gives the same answer.** A record that has reached SUCCEEDED or
  FAILED never changes.
* **Other clients are not blocked.** Ordinary reads were measured at 25 ms
  during a backup that previously stalled every client for its whole duration.
* **A second backup while one is running is refused**, naming the one in flight.
* **A restart forgets operation records.** They live in the daemon's memory. An
  id that is no longer known is reported as unknown rather than guessed at, and
  the message points at the backup file — which is what actually survives, since
  nothing partial is ever published. Ask `atlas backup verify NAME`.

### Why not copy the files

An Atlas data directory in normal operation is three files: `atlas.db`,
`atlas.db-wal` and `atlas.db-shm`. They are only meaningful together, and only
at an instant no external reader can name. Copying them while the daemon writes
produces a file that opens cleanly and is wrong — usually missing the most
recent commits, occasionally internally inconsistent.

The online backup API copies the database through the source connection's pager,
inside one read transaction, in a single `sqlite3_backup_step(-1)`. The result is
the database as of exactly one commit boundary, including everything that was
still in the write-ahead log. Writers are not blocked while it runs, so a backup
of a live daemon needs no downtime and no coordination.

Stepping incrementally instead would let a writer commit between steps, and
SQLite would restart the copy from the beginning — against a busy daemon that is
a loop with no bound. One step is the whole reason it terminates.

### Why the backup file has no `-wal`

Before publication the copy is switched to rollback journalling, which makes it
one file with no sidecars. That is what lets `atlas backup verify` open it
read-only and create nothing at all: a WAL-mode database cannot be opened
without creating a `-shm` beside it, and a verification that writes is not a
verification.

### What is *not* in a backup

A backup contains the database. It does not contain, and restoring one does not
restore:

- `~/.config/atlas` or any Atlas configuration;
- the runtime socket or the writer lock;
- the systemd user unit;
- the Claude integration record, the local marketplace or the plugin;
- anything in any indexed repository.

A restore reports this in both output modes rather than leaving it to be
assumed. Start the daemon again yourself afterwards.

A backup is **not encrypted and not signed**. It is a plain SQLite file with
whatever protection its directory and mode give it; Atlas creates it `0600` and
makes no cryptographic claim about it. The reported SHA-256 detects damage and
accident. It is not a signature, and it establishes nothing about who wrote the
file. If the contents need to be secret in transit or at rest, that is a job for
something outside Atlas.

### How a backup is published

1. The destination's parent is resolved **component by component from `/` with
   `O_NOFOLLOW`**. A symbolic link anywhere in the path refuses the operation; it
   is never followed to see where it goes.
2. The destination itself is refused if it is a symlink, a directory, or any
   non-regular file — and, without `--force`, if it exists at all.
3. The copy is written to a mode-`0600` temporary file in that directory,
   created `O_EXCL`. The mode is set explicitly rather than left to the umask,
   because SQLite would otherwise create the file `0644` or worse.
4. The finished file is confirmed to be the same inode this process created,
   through the directory descriptor.
5. It is **verified in full** — the same verification `atlas backup verify` runs.
6. It is `fsync`ed, renamed into place, and the directory is `fsync`ed.

A failure at any point leaves no published file. A backup that would not restore
is never written, because a backup nobody can restore is worse than a failure:
it gets filed and forgotten.

---

## What verification checks

`atlas backup verify` creates nothing, writes nothing, repairs nothing and needs
no data directory. It can be run on a machine where Atlas has never stored
anything, and `tests/test_backup.c` runs it against a fresh `HOME` and digests
the whole tree before and after.

In order:

| check | catches |
|---|---|
| the file is a regular, non-empty, non-symlink file | the obvious cases, with their own verdict |
| the SQLite header magic | not a database; truncation at the front |
| header page-size × page-count equals the file length | **truncation and appending** |
| `PRAGMA integrity_check` | b-tree structural damage |
| required tables for the recorded schema version | a SQLite database Atlas did not write |
| schema version ≤ this build's | a backup from a newer Atlas |
| `PRAGMA foreign_key_check` | referential damage |
| every decision revision rehashed from its stored content | altered decision prose, scope, links or alternatives |
| every document's status recomputed from its ledger | a cache that disagrees with the record |

The length check is there because `integrity_check` does **not** reliably catch
truncation: it walks the pages the b-trees reach, so a file cut short in
unallocated space, or by less than a page, can pass it. A backup missing its tail
is exactly the failure an operator has, and "it verified" is exactly the wrong
answer.

Verdicts are a closed vocabulary: `ok`, `unreadable`, `not_sqlite`, `not_atlas`,
`schema_future`, `corrupt`, `inconsistent`. The distinctions are the ones an
operator needs in order to act — a truncated transfer and a database from a newer
Atlas are both unusable, but only one of them is fixed by fetching the file
again.

An unusable backup is an **answer**, not a failure to answer: `backup verify`
writes a complete document and *then* exits non-zero, and in `--json` mode it
writes exactly one document, so a script can test both.

### What verification cannot catch

**SQLite has no per-page checksum.** `PRAGMA integrity_check` validates b-tree
structure, not cell content. A single byte flipped inside an ordinary string
value leaves a structurally perfect database holding a different value, and
nothing Atlas can run will find it.

Two things narrow that, and neither is a general guarantee:

- Every decision revision is **rehashed from its stored content** and compared
  with its recorded `content_hash`. Silent alteration of decision prose, scope,
  links, alternatives or basis is detected, because that is where it would
  actually matter. This is also what `atlas doctor` does to the live index.
- The whole-file **SHA-256** is reported at creation and recomputed at
  verification. An operator who records the first can compare it later and know
  the bytes are the bytes.

`tests/test_backup.c` asserts all three cases explicitly, including the one that
is *not* detected, so the limitation cannot quietly disappear from the
documentation while remaining true of the code.

Atlas makes **no claim of durability against disk or kernel failure**. It
`fsync`s the file and the directory before publishing, which is what an
application can do; it does not survive a drive that acknowledges writes it did
not perform, and it is not a replacement for storing backups somewhere other
than the machine that made them.

---

## Restore

`atlas backup restore BACKUP --yes` replaces the index in the data directory.

It **acquires the data-directory writer lock exclusively** for the whole
operation, so the daemon must be stopped. That is the same lock the daemon holds
for its entire lifetime, so acquiring it *is* the proof that no daemon is
running — a running daemon makes a restore refuse rather than race it. Stop it
first:

```sh
systemctl --user stop atlas.service
atlas backup restore ~/backups/atlas-2026-08-07.db --yes
systemctl --user start atlas.service
```

The order of operations is:

1. **Verify the backup completely.** A future schema, a corrupt file or a
   database Atlas did not write is refused here, before the lock is even taken.
   Migrations only go forward, so a newer schema cannot be made into this one and
   opening it anyway would silently discard whatever this build cannot see.
2. **Refuse every symlinked component** of the data directory: `atlas.db`,
   `atlas.db-wal`, `atlas.db-shm` and `atlas.lock`. If `atlas.db` were a link,
   the rename would replace the link and leave the real index orphaned behind it.
3. **Snapshot what is about to be displaced**, using the same online backup path,
   into `atlas.db.replaced-<timestamp>`. It is a complete, consistent, verifiable
   database including anything still in the previous write-ahead log — a plain
   file copy would preserve the wrong thing. **Atlas never deletes it.**
4. **Stage** a byte-for-byte copy of the backup beside the index, mode `0600`,
   and confirm its SHA-256 and length match what was verified. That is also what
   closes the gap between verifying the file and reading it: swapping the file in
   between would require producing one with the same digest and length.
5. `fsync` the staged copy.
6. **Commit**: remove the previous `-shm` (a pure cache SQLite regenerates), move
   the previous `-wal` aside, rename the staged copy over `atlas.db`, `fsync` the
   directory.
7. **Reopen, migrate if needed, and re-verify in place** — integrity, foreign
   keys, revision hashes and ledger replay, again, against what was actually
   installed.

**Everything before step 6 fails with the original database byte-identical.**
`tests/test_backup.c` injects a failure at each of five fault points — the copy,
the write, the `fsync`, the directory `fsync`, the rename and the post-restore
verification — and compares the original file's digest and length after every
one.

Step 6 is the only part that is not a no-op on failure, and it is built to be
reversible: the previous write-ahead log is **renamed aside rather than deleted**,
so if the rename fails the name goes back and the original is again exactly what
it was, complete. It must not survive the rename, because SQLite would apply it
to the restored file. A crash between those two renames leaves the previous
database without its log; that window is two renames wide with no I/O between
them, and the snapshot from step 3 — which already contains the log's content —
is what recovers from it.

The restored database's SHA-256 will **not** equal the backup's. Opening it
writes the WAL-mode header back. That is expected; the two are compared by
content, which is what the table-by-table round-trip test does.

---

## Retention

Every table in the schema is classified, and the classification — not the
deletion — is the product. `atlas maintenance plan` prints all of it, including
the reason for each table:

```sh
atlas maintenance plan --older-than 90 --retain 1000
atlas maintenance prune --older-than 90 --retain 1000 --apply
```

`plan` opens the index read-only, takes no lock, writes no byte and runs happily
while the daemon is serving. `prune` without `--apply` is a **usage error**, not
a quiet plan, and `plan --apply` is refused too: the two words must not become
interchangeable.

### The classes

| class | meaning |
|---|---|
| `canonical` | the record Atlas exists to keep. Never removed by any automatic rule. |
| `memory` | durable engineering memory. Removable only by an explicit act aimed at the thing itself, never by age. |
| `derived` | rebuilt whole by a pass. Not pruned by age, because a half-aged derived table is not a smaller index — it is a wrong one, and nothing in it records that rows are missing. |
| `operational` | bounded operational history, or a cursor. |

### What is prunable

**One table: `repo_events`.** It is a bounded stream of observations that
already carried a documented per-repository ceiling before A5 existed, its `id`
is `AUTOINCREMENT` so no cursor can be re-pointed by a deletion, and the durable
`SOURCE` and `GIT` evidence lives in `evidence`, which is never pruned with it.

A row is eligible only when it is **both** older than `--older-than` **and**
outside the newest `--retain` for its repository. Both conditions, always: age
alone would empty a quiet repository's whole stream, and the floor alone would
delete fresh observations from a busy one.

Bounds are checked, not clamped. `--older-than` must be 1–36500 days;
`--retain` at least 100. A negative value is refused rather than silently
replaced by the default, because a discarded number that nobody is told about
deletes more than was asked for.

Deletion runs in **bounded batches, one transaction each** — never one
transaction across the loop, which is the same rule every A1 pass follows. A
failure rolls that batch back whole, and the operation is idempotent, so
re-running finishes it.

### What is not prunable, and why

The reasons are in `RETENTION[]` in `src/core/service_maintenance.c`, one per
table, and `atlas maintenance plan` prints them. Three are worth repeating here
because they are the ones that look prunable:

- **`scans`** — `files.first_seen_scan_id`, `last_seen_scan_id` and
  `deleted_scan_id` hold `scans.id`, which is a plain rowid SQLite reuses.
  Deleting an old pass would hand its id to a future one and make those columns
  describe a different pass. This is the same hazard A4 documented for
  `imported_from_ai_decision_id`.
- **`commits` and `file_changes`** — rebuildable only by rewalking history from
  scratch, and `repo_commit_tips` is the cursor that says it need not be.
  Removing them by age would make that cursor a lie.
- **`decision_challenges`** — a *consumed* challenge is part of an approval
  record and the event points at it. Expired unconsumed ones are already removed
  at the point of use, which is the only `DELETE` the decision tables have.
  A6 widened this table (a `revalidate` intent, and the state a revalidation
  capability is bound to) without changing its classification.
- **`decision_validations`** (A6) — the append-only record that a human checked
  an approved decision against an exact repository state, together with the
  assessment and the reasons that prompted them to. It is the evidence that a
  concern was addressed rather than ignored. An age-pruned validation history
  would silently move every surviving decision's validation point *backwards* —
  widening its change range — and, for decisions whose every record went, remove
  the only proof that anybody ever looked.

### There is no background deleter

Nothing prunes on a timer, at startup, on low disk, or as a side effect of any
other command. Rows go away when an operator runs `atlas maintenance prune
--apply` at a terminal, and at no other moment. (`repo_events` is separately
capped by the daemon's own long-standing per-repository ceiling; that is A1
behaviour A5 neither adds nor changes.)

No MCP tool and no hook can plan or apply a prune, and no method in the ordinary
group can either. A model holding every tool Atlas exposes cannot prune the
index.

`maintenance.plan` and `maintenance.prune` are served in the **operator-uid**
group — offered only to the peer whose `SO_PEERCRED` uid matches the root-owned
policy, and answered with `unknown method` for everybody else, including
`atlas-worker`. They were added by the A8-CI closeout, and the reason is worth
stating: A5 gave maintenance no RPC surface on the premise that whoever owns the
data directory can prune it anyway, and A7.1 broke that premise without anyone
noticing. Under a system deployment the index is `0700 atlasd`, so the operator
account could neither plan nor prune without becoming the service account —
manual impersonation standing in for a missing feature, which is not a
procedure.

**The daemon no longer has to be stopped for a prune.** Atlas still has exactly
one writer and a maintenance pass is still one: the prune runs *on* the writer
thread, so it is serialized with every other write rather than competing for the
lock. Everything else is unchanged — `--apply` is required, the delete is per
batch and not per loop, bounds are checked rather than clamped, and there is no
background deleter.

A local invocation, as the account that owns the data directory, still takes the
lock and still requires the daemon stopped. That path is unchanged and is what a
per-user install uses.

---

## Operating procedure

### Taking a backup while Atlas runs

No downtime, no coordination:

```sh
mkdir -p ~/.local/state/atlas/backups && chmod 700 ~/.local/state/atlas/backups
atlas backup create ~/.local/state/atlas/backups/atlas-$(date -u +%Y%m%dT%H%M%SZ).db
```

Use a timestamped name; `create` refuses to overwrite by default, and that
refusal is more useful than a `--force` habit. Record the reported SHA-256
somewhere other than beside the file.

### Rolling back to a backup

```sh
systemctl --user stop atlas.service
atlas backup verify  ~/.local/state/atlas/backups/atlas-20260807T160000Z.db
atlas backup restore ~/.local/state/atlas/backups/atlas-20260807T160000Z.db --yes
systemctl --user start atlas.service
atlas doctor
```

The database that was replaced is still there as `atlas.db.replaced-<timestamp>`
in the data directory. Nothing removes it; when you are satisfied, remove it
yourself.

### Rehearsing a restore without risking the live index

Restore into a throwaway data directory. This is what the A5 acceptance drill
does, and it is the only way to know a backup is restorable rather than merely
verifiable:

```sh
tmp=$(mktemp -d)
atlas --data-dir "$tmp" backup restore ~/.local/state/atlas/backups/atlas-20260807T160000Z.db --yes
atlas --data-dir "$tmp" doctor
atlas --data-dir "$tmp" repo list
rm -rf "$tmp"
```

### Uninstalling

```sh
systemctl --user disable --now atlas.service
atlas integrate claude uninstall --user     # never touches the index
atlas service uninstall --user
rm -f ~/.local/bin/atlas
```

The index, the backups and the decision record are left alone by all of that.
Removing `~/.local/share/atlas` is a separate, deliberate act, and it destroys
the decision record — take a backup first.

---

## Measured behaviour

`sh scripts/perf-a5.sh build` builds one deterministic fixture carrying every
kind of row A5 has to move — a real A0/A1 file and history index, a real A3
structural graph, real A2 session rows written by real hooks against a real
daemon, and the A4 decision corpus at full acceptance scale — and asserts its
own floors and its own limits, exiting non-zero rather than printing a number
nobody checks. It also compares the restored copy against the source rather than
trusting that the restore finished: a timing gate over an operation that
silently did nothing is the worst kind of green.

Fixture: **110 383 104 bytes**, 10 000 documents, 25 000 revisions, 100 000
links, all four lifecycle states, 25 336 symbols, 132 196 relations.

| operation | required limit | observed | peak RSS observed |
|---|---|---|---|
| `backup create`, daemon running | 10 s / 256 MiB | 4.3 s | 8.1 MB |
| `backup verify` | 10 s / 256 MiB | 3.9 s | 6.2 MB |
| `backup restore` into an isolated directory | 10 s / 256 MiB | 8.4 s | 6.5 MB |
| `maintenance plan`, daemon running | 10 s / 256 MiB | 17 ms | 5.9 MB |
| refusing a file of random bytes | 2 s | 1 ms | — |

The left column is the **required bound**; the right two are **observations on
this machine with this fixture**. They are not the same kind of statement, and
an observation reported as a bound ("under 4.3 s") would be a claim Atlas does
not hold. Every figure above comes from one run of the script.

Peak RSS is a property of the constants, not of the database: the copy goes
through SQLite's pager and the staging copy uses a 256 KiB chunk, so a larger
index costs time rather than memory.

`restore` is roughly twice `create` because it verifies the backup completely
before touching anything **and** re-verifies the installed database afterwards —
two full passes over the file, both including a rehash of every revision. That
is the intended trade.

None of this is a claim about any real repository.

## Testing facility

`ATLAS_BACKUP_FAULT` names one point at which `src/core/service_backup.c`
pretends the operating system failed: `copy`, `write`, `fsync`, `fsync_dir`,
`rename` or `post_verify`. It exists because the guarantees above are entirely
about failure paths, and a guarantee whose failure path is never executed is a
comment.

It is compiled into every build on purpose: an `#ifdef` would mean the binary
that ships is not the binary the failure tests ran against. It can only ever
cause an operation to **abort** — there is no fault point that skips a check,
weakens a guarantee or publishes something. The worst an operator who sets it can
do is fail to take a backup, loudly.

---

## Known limitations

- Not encrypted, not signed. See above.
- A single flipped byte inside an ordinary value is not detectable by any check
  Atlas can run. Decision revisions are the exception, by rehash.
- No incremental or differential backup. Each backup is a whole copy of the
  index; on a large repository index that is the dominant cost and there is
  nothing clever about it.
- No recovery to an arbitrary instant between backups, and no log shipping.
  What you have is the backups you took.
- No durability claim against disk or kernel failure, and no claim about media
  that lies about `fsync`.
- Restore is same-machine, same-architecture SQLite only. Nothing here has been
  tested beyond Linux on x86-64, and it is not claimed to work elsewhere.
- `prune` and `restore` require the daemon to be stopped, because Atlas has
  exactly one writer and both are writers.
- Only `repo_events` is prunable. If your index is large, it is large because of
  `files`, `commits` and the structural graph, and the way to make it smaller is
  to register fewer repositories or to rebuild the structural index — not to
  prune.

## A6 and this contract

A6 took the schema to 7 and changed none of the guarantees on this page.

- **Verification stays read-only and still migrates nothing.** `backup verify`
  opens the file through `atlas_db_open_readonly`, which cannot migrate, so a
  backup taken at schema 6 verifies as a schema-6 file and is still a schema-6
  file afterwards. `tests/test_migrate7.c` asserts exactly that, including that
  nothing under the data directory changed while verifying.
- **A restored older database migrates forward through the ordinary path.**
  Restore installs the bytes it was given; the next `atlas_db_open` migrates
  them. A schema-6 backup restored over a schema-7 index comes back at 6 and
  goes forward to 7, with every decision record and every event-to-challenge
  reference intact.
- **A backup from a newer Atlas is still refused**, unchanged: the recorded
  schema version is checked against this build's expectation before anything is
  installed.
- **Neither new A6 table is prunable**, and A6 adds no background deleter, no
  timer and no cleanup side effect.
- **A6 adds no RPC method for backup, restore or maintenance**, and
  `tests/test_gate_trust.c` re-asserts their absence against a live daemon. The
  cheapest way to have broken this page's central guarantee would have been for
  a later phase to add the method it says does not exist.

## Where else this is written down

`docs/data-model.md` (the tables) · `docs/daemon-and-ipc.md` (the writer lock) ·
`docs/decision-lifecycle.md` (what the revision hash covers) ·
`docs/systemd-user-service.md` (stopping and starting the daemon) ·
`SECURITY.md` (the threat model).
