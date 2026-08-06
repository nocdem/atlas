# Known engineering and security backlog

Everything Atlas knows about itself that is not fixed. Recorded rather than
carried silently, so that a later phase inherits a list instead of a surprise.

Nothing here blocks A1 correctness. Items that did were fixed in A1 and are
listed at the bottom for the record.

## Deferred from the A0 security audit

These were identified during the A0 review and deliberately not investigated
further in A1: A1's scope was the daemon, and reopening each of them would have
turned it into a second security season. They are listed with what is known,
not with a guess at severity.

### 1. Symlink hardening beyond the working tree

`atlas_path_open_nofollow` refuses to traverse a symlink at any component when
reading repository content, and a tracked symlink is hashed by its link text.
That covers the working tree.

Not covered: the paths Atlas is *given* rather than the ones it walks. A data
directory reached through a symlink, a `--data-dir` pointing at one, or an
intermediate symlink in the runtime directory's parents are accepted. The
immediate parents are checked (`atlas_ipc_ensure_runtime_dir` refuses a symlinked
runtime directory; `lock.c` and `unit.c` refuse a symlinked target file), but the
chain above them is not.

*Why it is deferred:* an attacker able to plant symlinks in the parents of the
Atlas data directory already has write access to the directory containing the
index. The lock and the socket are not the interesting target at that point.

*What would close it:* resolve the data directory once with `openat` from a
verified root and hold a directory descriptor for the process's lifetime, using
`*at` calls throughout instead of absolute paths.

### 2. Defensive result initialisation

Several functions write their `out` parameter only on success. Callers all check
the status first, so no live path reads an uninitialised value, and the
sanitiser builds are clean. It is still a shape where a future caller that
forgets the check gets garbage instead of a defined value.

*What would close it:* every fallible function zeroes or initialises its outputs
before doing anything that can fail, uniformly, with the convention stated in
`CLAUDE.md`.

### 3. Database and data-directory symlink hardening

`atlas_db_open` creates the database file with `open(O_RDWR|O_CREAT)` and no
`O_NOFOLLOW`, so a pre-existing symlink at `atlas.db` would be followed. The WAL
and SHM sidecars are the same. Related to item 1 and with the same
already-has-write-access reasoning.

*What would close it:* `O_NOFOLLOW` on the database open, and `openat` relative
to a held data-directory descriptor.

### 4. Evidence retention and database growth

`evidence` rows accumulate and are never pruned. A1 bounds the *raw event*
journal (`ATLAS_EVENTS_RETAIN_PER_REPO`) but deliberately does not prune
evidence: evidence is the durable provenance record, and silently discarding it
would undermine the invariant that every result preserves provenance.

A repository churning for months therefore grows the evidence table without
limit. Measured behaviour today: a pass over an unchanged repository adds no
evidence at all (there is a test), so growth is proportional to real change, not
to time.

*What would close it:* a retention policy that is explicit about what is lost —
for instance, collapsing repeated `SOURCE` evidence for one path into a first-seen
and last-seen pair — plus a `doctor` report of index size and a documented
`atlas compact`.

### 5. Submodule visibility

A gitlink is recorded as `file_type = other` with a note saying its contents
belong to another repository, and `--ignore-submodules=all` keeps git from
descending into one. Atlas therefore knows a submodule exists and nothing about
what is in it.

*Why it is deferred:* indexing a submodule means registering it as its own
repository, which is a modelling decision (does a change inside a submodule
belong to the parent's history?) rather than a bug.

*What would close it:* an explicit `atlas repo add --with-submodules` that
registers each one separately and records the parent/child relation.

### 6. Lazy-fetch assumptions

Atlas refuses partial (promisor) repositories outright because git 2.39 cannot be
told to refuse a lazy fetch. `GIT_NO_LAZY_FETCH=1` is set and is honoured by git
2.41 and later, but Atlas does not currently *detect* the git version and relax
the refusal accordingly.

The A1 detection is now exact and fail-closed (see below), so the refusal is
correct. It is also stricter than it needs to be on a modern git.

*What would close it:* parse the git version at `atlas_git_probe` time and, on
2.41 or later, accept a promisor repository while relying on `GIT_NO_LAZY_FETCH`
— with a test that proves the variable is actually honoured rather than assumed.

### 7. Exact scope of the read-only digest proofs

`fx_tree_digest` hashes relative paths, entry types, permission bits, symlink
targets and file contents, and the smoke and adversarial suites use it to prove
that Atlas does not modify a repository. It does not cover extended attributes,
ACLs, ownership, or timestamps.

Timestamps in particular are *expected* to change: reading a file updates its
`atime` unless the filesystem is mounted `noatime`. Including them would make the
proof fail for a reason that is not a modification.

*What would close it:* state the scope in `SECURITY.md` (done), and add xattr and
ownership to the digest where the platform supports it.

## New in A1, not fixed

### 8. The watcher cannot see everything, by construction

Documented in full in `docs/watcher-consistency.md`. inotify does not report
changes made through some bind mounts or on network filesystems that do not
implement it, and Atlas has no way to detect that in advance. Such repositories
are covered only by periodic reconciliation.

This is not closable — it is a property of the mechanism. What Atlas does about
it is refuse to *claim* currency it cannot prove.

### 9. `mtime` granularity

A file rewritten within the filesystem's timestamp granularity, to exactly the
same size, inode and mode, compares as unchanged and is not rehashed. Every Linux
filesystem Atlas targets reports nanoseconds. The periodic full pass is the
backstop where one does not.

*What would close it:* an optional "always hash" mode for filesystems known to
have coarse timestamps, selected by measuring the granularity at registration.

### 10. Reconciliation is per repository, not per path

An event on one file triggers a pass over the whole repository's file list. The
pass is cheap — one `lstat` per file, no content read — but it is O(files), not
O(changed). On the 5000-file fixture that is about 480 ms.

*Why it is not fixed:* a path-targeted pass has to decide what else the change
could have invalidated (a `.gitignore` edit changes the classification of
everything below it), and getting that wrong means a stale index that looks
current. The whole-repository pass is the version that is obviously correct.

*What would close it:* a fast path for the common case — a modification to an
already-tracked file with no `.gitignore` involvement — falling back to a full
pass otherwise.

### 11. `repo.add` over IPC refuses awkward paths

A repository path containing a control byte, a quote or a backslash is refused
when routed through the daemon, because the CLI builds that request's JSON by
hand. The offline path takes the same value as an argv operand and accepts it.

*What would close it:* build client requests through the streaming JSON writer
instead of `atlas_buf_appendf`, which removes the hand-built document entirely.

## Fixed in A1

For the record, so the list above is not read as the complete set of what was
known.

- **Partial-clone detection was evadable.** The A0 64 KiB prefix scan of
  `.git/config` missed a marker beyond 64 KiB, a marker straddling the boundary,
  `config.worktree`, and included config files — and over-refused a repository
  that merely mentioned the word. Replaced by exact, fail-closed queries through
  the hardened git runner, with a regression test for each case.
- **The git executable cache was an unsynchronised global.** Correct while Atlas
  was single-threaded; a data race the moment it was not. Now immutable after
  publication with every access serialised, frozen before any thread is created,
  and verified under ThreadSanitizer.
- **The documentation implied safe text prevented prompt injection.** Corrected
  in `SECURITY.md` and expanded in `docs/ai-trust-boundary.md`.
- **The IPC serve loop blocked on a slow client.** A client that sent three bytes
  of a header and stopped held the loop for the read timeout, stalling every
  other client. Found by a test written for it; the loop is now non-blocking with
  per-connection state.
- **The recursive watch installer walked a truncated tree.** It held a pointer
  into the buffer it was appending to, so the first reallocation left it
  dangling and the walk stopped early — 6 directories watched out of 51. Found
  by the performance script; there is now a regression test.
