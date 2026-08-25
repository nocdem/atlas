# The watcher and Atlas' consistency model

The hard question A1 has to answer is not "did Atlas notice that file change".
It is: **when Atlas says the index is current, is that true?**

Filesystem watching cannot be made reliable. inotify drops events under load,
runs out of watches, and sees nothing at all while the daemon is not running.
Any of those can leave the index stale. What must never happen is Atlas
*claiming* to be current while one of them has happened.

So the model has one rule everything else serves:

> Facts come from git and from the filesystem, read by a reconciliation pass.
> Events are only ever a hint that a pass should run. When Atlas cannot prove it
> observed every change, it says so, and it does not describe the index as
> current until a pass has actually looked at everything.

## Generations

Each repository has two numbers in `repo_index_state`:

- `generation` — the pass currently in flight
- `last_complete_generation` — the newest pass that finished consistently

A pass claims the next generation before it writes anything, and publishes only
on success. **Readers are only ever shown `last_complete_generation`**, so a
crash half way through a pass is invisible rather than half-visible, and a
restart exposes the last complete consistent state rather than a mixture.

`last_complete_generation` is advanced with `max()`, never assignment: a slow pass
that finishes after a newer one must not move the published state backwards.

## The event gap

`event_gap` is the honesty bit. It is set whenever Atlas cannot prove it observed
every change:

| cause | detected by |
| --- | --- |
| inotify queue overflow | `IN_Q_OVERFLOW`, which is global to the instance — the kernel does not say what was lost, so every watched repository is marked |
| watch limit reached | `ENOSPC` from `inotify_add_watch`, i.e. `fs.inotify.max_user_watches` — reason `kernel_limit` |
| Atlas' own watch budget reached | reason `total_budget`, or `repo_budget` while other repositories are still being served |
| a new directory that could not be fully watched | the same, mid-flight |
| the watch set was rebuilt | P0: a rebuild drops every watch and reinstalls it, and events in that window are gone. Every repository is owed a full pass afterwards. |
| a subtree was watched late | P0: a directory that appeared while the daemon ran was not watched until git had been asked whether it is ignored, so events inside it during that interval were not observed |
| the ignore rules changed | P0: a `.gitignore`, `info/exclude` or branch change re-primes the repository, which is another such window |

Since P0 the cause is also recorded as a **reason code** on the repository —
`watch_reason`, a closed vocabulary — rather than only as prose. Three genuinely
different causes used to share two sentences between them, so an operator could
not tell "raise the sysctl" from "this daemon's budget is spent" from "this walk
hit its visit bound", and neither could Atlas: two of them set one boolean.
| a reconciliation pass that failed | any error from the pass |
| a pass repeatedly abandoned because HEAD kept moving | `ATLAS_RECONCILE_MAX_ATTEMPTS` exhausted |

While `event_gap` is set:

- `index_current` is **false**, in the CLI, in `repo.state` and in `repo.list`
- `atlas daemon status` reports the count of repositories in that state, and says
  in prose that their indexes are not current
- the next pass is upgraded to a full one, whatever was asked for

Both `event_gap` and `pending_full_reconcile` are persisted, so the obligation
survives a restart. A daemon that crashes with a gap open comes back knowing it
still owes a full pass.

**Only a completed content-verifying pass may clear it.** An incremental pass
cannot prove it saw what the gap hid — it skips files whose recorded identity
matches, and the whole point of the gap is that the recorded identity may be
stale. So an incremental pass leaves the flag exactly where it found it. There
is a test for this.

"Content-verifying" means the pass **read the bytes of every eligible file**, not
that it stat'ed every path. A thorough stat proves nothing here, because the
metadata is exactly what is in doubt.

The flag that gates clearing is `content_verified`, and it is computed from what
the pass *did*, not from what was requested: a pass qualifies only if it was a
full pass, nothing was truncated by a ceiling, **and** it recorded zero identity
hits. It is reported in `atlas sync --json` so the claim can be checked rather
than assumed.

The operations that force content verification are:

| operation | why |
| --- | --- |
| daemon startup | whatever happened while it was stopped was never observed |
| recovery after `IN_Q_OVERFLOW` or any event gap | events were lost; metadata may be stale |
| recovery after an unclean shutdown | detected from a `daemon_state` row with a start and no stop |
| `atlas sync --full` | the user explicitly asked to re-establish the truth |
| periodic reconciliation | the backstop for everything inotify cannot report |
| a dirty-path list that overflowed | the watcher cannot enumerate what changed |

An unclean shutdown additionally marks every repository as gapped **before** the
recovery pass runs, so the window between starting up and finishing that pass is
reported honestly rather than optimistically.

## What is watched

Per registered worktree:

- the working tree, recursively, **excluding** `.git` and excluding directories
  git's own ignore rules cover
- that worktree's own git directory, for `HEAD` and the index
- the shared common git directory and `refs/`, so a branch update in one worktree
  is seen by every worktree sharing the object store

Ignored directories are skipped because a `node_modules` or `build/` tree would
otherwise consume the entire watch budget for changes Atlas will not index. The
set comes from `git ls-files --others --ignored --exclude-standard --directory`,
so there is no second implementation of ignore semantics to drift from git's.

### What the ignore inventory is, and what it is not

**It is an inventory of ignored paths that existed when git was last asked. It is
never the authority on a path that did not exist then.**

`git ls-files` enumerates the filesystem. A `.gitignore` containing `build/` with
no `build/` on disk produces no entry at all — so an inventory built while
priming has nothing to say about a directory created afterwards, which is exactly
the directory the watcher needs an answer about. Consulting it would return "not
ignored", confidently, for the one case the mechanism exists to handle.

Before P0 the situation was worse still: the inventory was built on the stack
inside the priming function and freed when it returned, and the handler for "a
directory appeared" built its context with `memset`, leaving the pointer NULL. A
directory created while the daemon ran was watched **recursively, whatever git
thought of it** — every rebuilt `build/`, for as long as the daemon lived.

So a directory Atlas has not seen before is **not watched and not descended
into**. It waits in a bounded queue, and on the next debounce tick **one**
`git ls-files` invocation per repository answers for the whole queue at once.
Because the directory now exists on disk, that answer is correct — including for
an empty directory, and including a deep subtree, which `--directory` collapses
to its topmost ignored entry.

The cost of waiting is recorded rather than hidden. Nothing under the directory
is watched until the answer arrives, so events inside it in the meantime are
missed. While anything is queued the repository is `priming` and its index is
**not current**; when a queued directory turns out to be visible, the repository
is marked with an event gap and owes a content-verifying pass before it may be
described as current again.

### Ignore rules that change

Any `.gitignore` at any depth, `info/exclude`, and a HEAD move — because a branch
switch swaps one branch's ignore rules for another's without touching a file the
watcher would otherwise care about. Each marks the inventory stale, and the
repository is re-primed against a fresh one at repository scope: newly ignored
subtrees lose their watches and newly visible ones gain them, by construction
rather than by a diff. The re-prime is a window in which events are not observed,
so it too marks a gap and takes a full pass.

`info/exclude` needs its own subscription and does not come free with the git
directory. An inotify directory watch reports its **direct children only**:
watching `.git` produces events for `.git/config` and `.git/HEAD` and **nothing
at all** for `.git/info/exclude`. Verified by experiment, not assumed. It
resolves to the *common* directory even from a linked worktree, so one descriptor
serves every worktree sharing it.

**A stated gap:** `core.excludesFile` normally lives outside the repository root,
and Atlas never watches outside a repository root. A change to it is picked up by
the periodic pass rather than immediately.

### The inventory is a snapshot, and the walk re-checks itself

The priming walk asks git once, at the start, and judges every directory it
discovers against that answer. On a large repository the walk takes tens of
seconds, and a build running in that window can create an ignored tree the
snapshot has never heard of — which the walk would then watch, arriving at
exactly the outcome the inventory exists to prevent from the other direction.

So when the frontier empties, the inventory is read once more and any watch the
repository holds beneath a now-ignored path is released. One git invocation and
one pass over the watch map, once per priming run. It only ever *releases*
watches: a subtree that stopped being ignored is the `ignore_stale` path's
business.

The residual is stated rather than solved: between the snapshot and that
re-check, an ignored tree created mid-walk does hold watches. They are released
when priming ends, and nothing is ever indexed from them, because indexing asks
git separately.

### What a repository is told while it primes

`priming` sets the event gap, and not only because this Atlas would refuse to
call a priming repository current anyway. `atlas_watch_state_parse` maps any
state it does not recognise to `unwatched`, and the currency rule falls through
`unwatched` to *current* — so a client older than P0 reading `"priming"` over the
socket would re-derive `index_current: true` for a tree whose watches are still
going in. The gap is a field every such client already understands, and setting
it makes the honest answer the one they compute. A content-verifying pass clears
it, as it does every other gap.

`.git` is watched for metadata and **never indexed as source**. Those are
different things and the code keeps them apart: metadata watches are flagged
`is_meta` and never contribute a path to the index.

Symlinks are never traversed when installing watches (`IN_DONT_FOLLOW`, plus an
`lstat` before descending), so a link pointing out of the repository cannot pull
the watcher out of it.

## Events the watcher handles

| event | why it matters |
| --- | --- |
| `IN_CREATE`, `IN_MODIFY`, `IN_CLOSE_WRITE`, `IN_DELETE` | ordinary edits |
| `IN_MOVED_FROM` / `IN_MOVED_TO` with a cookie | the editor atomic-save pattern is a rename, not a write; a watcher that only handles writes misses **every** save |
| `IN_CREATE` on a directory | a new directory is watched recursively at once, otherwise files created inside it are invisible until the next periodic pass |
| `IN_DELETE` / `IN_MOVED_FROM` on a directory | its watch subtree is removed |
| `IN_MOVE_SELF`, `IN_DELETE_SELF` | every path below the watched directory is invalidated |
| `IN_IGNORED` | the kernel dropped the watch; forget it so the map does not fill with dead descriptors |
| `IN_Q_OVERFLOW` | events were lost — see above |

Move cookies are paired so that a rename between two watched repositories dirties
**both**, not only the destination. Unpaired `IN_MOVED_FROM` entries expire after
`ATLAS_WATCH_MOVE_PAIR_MS` and are then treated as deletions, which is what a
move out of every watched directory is. The pending-move table is bounded; losing
a pairing costs a rename being reported as a delete plus an add, which the next
pass resolves anyway.

## Debouncing

A repository becomes dirty on its first event and is reconciled once it has been
quiet for `ATLAS_WATCH_DEBOUNCE_MS` (400 ms) — or immediately once it has been
dirty for `ATLAS_WATCH_MAX_DEBOUNCE_MS` (5 s), whichever comes first.

The cap matters: without it, a process writing continuously would defer indexing
for as long as it kept writing. A test asserts that fifty writes in a burst
produce a handful of passes rather than fifty, and that the last write is still
in the index afterwards.

Submissions also coalesce in the writer queue. A pass that has not started yet
will observe the filesystem as it is when it runs, so it already covers a second
identical request; a request for a *full* pass upgrades a pending incremental one
rather than being dropped.

## Periodic reconciliation

Every `ATLAS_WATCH_RECONCILE_INTERVAL_MS` (5 minutes) each repository gets a full
pass whether or not anything was observed. This is what covers what inotify
cannot: an unwatched subtree, a change made while the daemon was stopped, a
missed event, a filesystem that does not report reliably.

## Startup

A restart reconciles every repository **fully**, before trusting anything stored.
Whatever happened while the daemon was not running was not observed, and an index
built partly from events and partly from an assumption is not an index anyone can
reason about. There is a test that changes a file while the daemon is dead and
asserts the restart finds it.

## The cache-hit rule

A pass reads a file's content unless it can prove it does not need to. The rule,
in order of precedence, is:

1. a **gitlink** is never read — its content belongs to another repository
2. a **content-verifying pass** reads everything else, unconditionally, without
   consulting any stored identity
3. a path the **watcher named** is read, whatever its metadata says
4. a path whose **stat failed** is read, so the hash stage can classify it
5. otherwise, read **unless** the complete recorded identity matches *and* the
   new observation is not racy

The identity is all of: **device, inode, size, mode** (which carries the file
type), **mtime seconds and nanoseconds, and ctime seconds and nanoseconds**. All
eight must be present and all eight must be equal. **A missing or unsupported
field means unknown, not unchanged**, and an unknown identity is always read.

### Why ctime is not optional

mtime is writable. `utimensat` lets any process that can write a file restore the
mtime the file had before the write. So a same-length in-place edit —

```
open(O_WRONLY); write(same number of bytes); utimensat(old mtime)
```

— leaves device, inode, size, mode and mtime all identical. An identity built
from those five reports a cache hit, reads nothing, and keeps serving the old
content hash **indefinitely**, because every subsequent pass reaches the same
conclusion. The file is wrong in the index for as long as it is not touched
again in some other way.

Nothing in userspace can set ctime. Every write to an inode's data or metadata
updates it, including the `utimensat` call that restores mtime. Adding ctime
turns "the file looks unchanged" into "the inode has not been touched", which is
the claim the cache actually needs.

Rules 2 and 3 exist because rule 5 is still, ultimately, an argument from
metadata. Rule 2 covers *we may have missed something*; rule 3 covers *we know
something happened to this file*. Neither depends on ctime at all.

### Why a named path outranks its metadata

The watcher hands the pass the repository-relative paths it saw events for. Each
is hashed regardless of what its metadata says.

An inotify event is positive evidence that something happened to that file. The
metadata tuple is an inference that nothing did — and it is an inference the
writer of the file can manipulate. When the two disagree, the observation wins.
Metadata equality must never suppress an explicit event.

The list is bounded (`ATLAS_WATCH_MAX_DIRTY_PATHS`, and a byte ceiling). Past
either bound the watcher stops naming paths and asks for a content-verifying
pass instead: it can no longer enumerate what changed, and saying so is better
than naming a subset that reads like a complete list. Coalescing two pending
requests **merges** their path lists rather than replacing one with the other.

## Racy timestamps

A recorded identity is only useful if a later write is guaranteed to change it.
That guarantee fails for a write landing in the same timestamp tick the
observation was taken in: the file changes, the timestamp does not, and the
identity keeps matching.

A pass records the instant it began looking, `observed_at`, before its first
observation — so it is earlier than every stat the pass performs, which is the
conservative direction. An observed timestamp is then **racy** when:

```
racy(ts)  iff  ts.sec >  observed_at.sec
          or  (ts.sec == observed_at.sec and (ts.nsec >= observed_at.nsec
                                              or ts.nsec == 0))
```

Both mtime and ctime are checked; either being racy makes the observation racy.

The `ts.nsec == 0` clause covers filesystems that truncate timestamps to whole
seconds. There a timestamp is always numerically below a sub-second
`observed_at`, so the first two clauses alone would call a same-second write
non-racy. A zero nanosecond field is the signal that truncation may have
happened, and the whole second is then treated as still open. On a
nanosecond-resolution filesystem a genuine zero costs one extra file read, which
is the right direction to be wrong in.

**A racy observation is stored as an unknown identity — all NULLs — rather than
as a value.** The next pass therefore reads that file exactly once, and by then
the tick has closed and a real identity is recorded. The condition self-heals and
does not accumulate: `atlas sync --json` reports the count as `files_racy`.

## Passes and staleness

A reconciliation pass runs in four stages, and the order is the design:

1. **observe** — git reports HEAD, the worktree state, tracked paths and
   untracked-but-not-ignored paths. No transaction is open.
2. **select** — every candidate is `lstat`ed and compared against its recorded
   identity (device, inode, size, mtime to the nanosecond, mode). A match means
   the content cannot have changed, so it is **not read**. No transaction is open.
3. **hash** — only the selected files are read, across the worker pool. Still no
   transaction.
4. **apply** — results are written in bounded batches, each its own transaction.
   No git process and no file read happens inside one.

Between stage 3 and stage 4, HEAD is read **again**. If it moved, the results
describe a repository that no longer exists, and the pass is abandoned rather
than committed. A branch switch during a pass must never leave the index
describing a mixture of two branches. The pass is retried, bounded by
`ATLAS_RECONCILE_MAX_ATTEMPTS`; a repository whose branch is being flipped in a
loop ends up in a reported degraded state rather than spinning.

## What `index_current` actually means

True only when **all** of:

- a completed generation exists (`last_complete_generation > 0`)
- `event_gap` is clear
- `pending_full_reconcile` is clear
- the watch state is not `error` and not `degraded`

Otherwise it is false and `not_current_reason` says which of those failed. There
is no state in which Atlas reports `index_current: true` alongside a known gap.

## Known limits of the model

Stated rather than implied:

- A change to a file inside a directory Atlas is not watching (ignored, or beyond
  the watch budget) is found by the periodic pass, not immediately.
- `mtime` granularity is the filesystem's. On a filesystem with coarse
  timestamps, a file rewritten within the granularity window **and** to exactly
  the same size, inode and mode would compare as unchanged. Every Linux
  filesystem Atlas targets reports nanoseconds; a periodic full pass is the
  backstop where one does not.
- Atlas does not watch for changes to the git configuration that affect ignore
  rules (`core.excludesFile`, for example). The ignore set is re-derived when the
  watch set is rebuilt and on every full pass.
- inotify does not report changes made through a bind mount or on a network
  filesystem that does not implement it. Those repositories are covered only by
  periodic reconciliation, and Atlas has no way to detect that in advance.
