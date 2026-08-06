# Git safety

A registered repository is untrusted input. So is its Git configuration, and so is
any Git-related environment variable Atlas inherits. This document is the detail
behind the claim that Atlas cannot be made to execute something, read somewhere
else, or touch the network.

## Why the argv allowlist is not enough

Atlas checks every Git subcommand against a read-only allowlist. That stops Atlas
from *asking* Git to write. It does nothing about the more interesting problem:
**Git can be configured to execute programs while performing a read.**

A repository's own `.git/config` can point at a helper, and Git will run it:

| Setting | Runs during |
| --- | --- |
| `core.fsmonitor` | `git status`, `git ls-files` |
| `diff.external` | any patch-producing diff |
| `diff.<driver>.textconv` (via `.gitattributes`) | any patch-producing diff |
| `core.hooksPath` hooks (`post-index-change`, …) | index refresh during `status` |
| `core.pager` | any command with a pager |
| `core.askPass`, `credential.helper` | any command that wants credentials |

The first entry is not hypothetical. `core.fsmonitor` runs on `git status` **and**
`git ls-files`, which are the two commands every `atlas scan` performs. Before this
hardening, registering and scanning a repository whose `.git/config` set
`core.fsmonitor` would have executed that program. Neither
`--no-optional-locks` nor `GIT_OPTIONAL_LOCKS=0` prevents it; only
`-c core.fsmonitor=false` does. The test suite proves both halves: that plain Git
runs the helper, and that Atlas does not.

## The four layers

1. **A constructed environment.** Nothing is inherited.
2. **A `-c` prefix** closing every configuration-driven execution vector. `-c`
   outranks every configuration file, including the repository's own.
3. **Per-command flags** for diff-producing commands.
4. **The read-only argv allowlist**, checked before the fork.

All of it is built in one place, `src/git/git_harden.c`, so a new call site cannot
opt out. `src/git/git.c` has exactly one function that creates a Git process.

## Layer 1: the child environment

The environment handed to Git is **constructed from a fixed list**, not derived
from Atlas' own environment. Atlas' environment is never modified. Absence is
therefore the default, and the list below is what is present:

| Variable | Purpose |
| --- | --- |
| `PATH=/usr/bin:/bin` | fixed and minimal. Git finds its own subcommands through its compiled exec-path, not `PATH`; this exists only so anything Git legitimately needs resolves identically on every run. |
| `LC_ALL=C`, `LANG=C`, `LC_MESSAGES=C`, `TZ=UTC` | deterministic, locale- and timezone-independent output |
| `GIT_CONFIG_GLOBAL=/dev/null` | no user configuration |
| `GIT_CONFIG_SYSTEM=/dev/null`, `GIT_CONFIG_NOSYSTEM=1` | no system configuration |
| `GIT_CONFIG_COUNT=0` | no configuration through the environment |
| `GIT_ATTR_NOSYSTEM=1` | no system gitattributes |
| `GIT_OPTIONAL_LOCKS=0` | never write the index while reading it |
| `GIT_TERMINAL_PROMPT=0` | never block on a prompt |
| `GIT_PAGER=cat`, `PAGER=cat` | no pager process, alongside `--no-pager` |
| `GIT_NO_REPLACE_OBJECTS=1` | report true objects |
| `GIT_ALLOW_PROTOCOL=none` | no transport of any kind |
| `GIT_NO_LAZY_FETCH=1` | refuse an on-demand fetch (honoured by Git 2.41+, ignored by older) |
| `GIT_FLUSH=1` | predictable flushing |

Everything else is **absent**, which matters most for these, each of which would
otherwise redirect Git or make it execute something:

- repository and object-store selectors: `GIT_DIR`, `GIT_WORK_TREE`,
  `GIT_COMMON_DIR`, `GIT_INDEX_FILE`, `GIT_OBJECT_DIRECTORY`,
  `GIT_ALTERNATE_OBJECT_DIRECTORIES`, `GIT_NAMESPACE`, `GIT_CEILING_DIRECTORIES`
- configuration injection: `GIT_CONFIG`, `GIT_CONFIG_KEY_*`, `GIT_CONFIG_VALUE_*`
- programs Git would run: `GIT_EXTERNAL_DIFF`, `GIT_DIFF_OPTS`, `GIT_ASKPASS`,
  `SSH_ASKPASS`, `GIT_SSH`, `GIT_SSH_COMMAND`, `GIT_EXEC_PATH`, `GIT_TEMPLATE_DIR`,
  `GIT_EDITOR`, `GIT_ATTR_SOURCE`
- tracing, which writes files to a caller-chosen path: `GIT_TRACE*`, `GIT_DEBUG*`
- `HOME` and `XDG_CONFIG_HOME`, which would reintroduce user configuration

`atlas_git_env_is_sanitized()` asserts this list on every invocation, not only at
startup, so a future edit that forwards a variable fails loudly instead of quietly
widening the surface. The forbidden set is a real list in the code, and the test
suite drives every entry through the checker.

### Why `HOME` is no longer forwarded

An earlier version of Atlas forwarded `HOME` so that the user's own
`safe.directory` configuration applied to repositories owned by another user. That
is no longer done: reading the user's global configuration means reading a file
Atlas does not control, and the whole point of layer 1 is that nothing outside the
fixed list influences Git.

The consequence is deliberate and documented: **Atlas cannot read a repository
owned by another user.** Git refuses with "dubious ownership", and Atlas cannot
work around it, because Git specifically ignores `safe.directory` when it comes
from `-c` or the environment rather than from a system or global config file. That
refusal is detected and reported as an actionable error (exit code 4) naming the
path and explaining why, rather than surfacing as an opaque Git failure.

## Layer 2: the `-c` prefix

Every invocation carries these, in this order, before the subcommand:

```
<abs-git> -C <canonical-root> --no-pager --no-optional-locks --no-replace-objects
  -c core.fsmonitor=false            no filesystem-monitor helper
  -c core.hooksPath=/dev/null        no repository hooks
  -c color.ui=false                  no colour escapes in output
  -c core.pager=cat                  no pager
  -c diff.external=                  no external diff program
  -c gc.auto=0                       no background repacking (which would write)
  -c maintenance.auto=false          likewise
  -c log.showSignature=false         no signature-verification helper
  -c core.quotePath=false            raw bytes with -z, never quoted
  -c advice.detachedHead=false       no advice text in output
  -c protocol.allow=never            no transport
  -c uploadpack.allowFilter=false    no filtered transfer
  -c core.askPass=                   no askpass helper
```

`--no-pager`, `--no-optional-locks` and `--no-replace-objects` are global flags and
are all present in every Git version Atlas supports; the test suite asserts the
prefix rather than trusting it.

The Git executable is an absolute path, resolved from `PATH` **once per process**
and cached. `PATH` is searched in exactly one function,
`atlas_proc_which`, which refuses relative and empty `PATH` elements so a
repository cannot shadow Git with a local file.

## Layer 3: per-command flags

These are subcommand options, so they follow the subcommand rather than preceding
it. Getting that wrong is a hard error from Git, not a silent omission, and the
suite covers it.

| Command family | Flags | Closes |
| --- | --- | --- |
| `diff`, `diff --cached`, `log` | `--no-ext-diff` | external diff driver |
| | `--no-textconv` | textconv filter |
| | `--ignore-submodules=all` | descent into a submodule with its own config and helpers |
| `status` | `--ignore-submodules=all` | the same, using status' own flag |
| `rev-parse`, `ls-files`, `symbolic-ref` | none needed | these produce no diff |

`--ignore-submodules=all` is a deliberate trade: a submodule's own configuration is
a separate untrusted surface with its own hooks and helpers, so Atlas does not look
inside one at all. A submodule appears as a `160000` gitlink entry with a note, and
changes inside it are not reported. Hardened submodule handling would be a separate
piece of work.

## Layer 4: the argv allowlist

The first token that is not an option or the value of one must be in:

```
rev-parse   ls-files   log   status   diff   symbolic-ref   cat-file
```

Anything else is refused with exit code 7 **before** the fork. `commit`, `push`,
`checkout`, `reset`, `clean`, `gc` and `config` are all refused, and a `-c` value
that happens to look like a subcommand is not mistaken for one. An argv with no
subcommand at all (`git --version`) is allowed, since it cannot modify anything.

## No network, and no lazy fetch

Three independent measures:

1. `GIT_ALLOW_PROTOCOL=none` and `-c protocol.allow=never` make any transport fail.
2. `GIT_NO_LAZY_FETCH=1` refuses an on-demand fetch on Git 2.41 and later.
3. Git 2.39, the version Atlas is developed against, has neither
   `--no-lazy-fetch` nor that environment variable. So **partial (promisor)
   repositories are detected and refused before any object is read.**

Detection is filesystem-only, so it cannot itself trigger a fetch, and it needs no
subcommand outside the allowlist. Either marker is sufficient:

- a `*.promisor` file beside a pack in `<common-dir>/objects/pack`
- the strings `promisor` or `partialclone` in `<common-dir>/config`

The config scan is a deliberately blunt, bounded substring match: over-refusing a
repository is the safe direction. On detection `atlas_git_open` fails with exit code
7 and a message naming which marker was found and what to do about it (complete the
clone). A repository that becomes partial after registration fails on the next
command, not silently.

The cost is that Atlas cannot index a partial clone at all. That is the honest
outcome of "no network access, guaranteed": a repository whose objects are not all
local cannot be read offline, and pretending otherwise would mean a scan that
sometimes reaches the network.

Object-store **alternates** are a different thing and are not refused: they are how
Git shares objects between worktrees and shared clones. Their presence is recorded,
because it means objects may be read from outside the repository directory.

## Addressing and traversal

Atlas never changes its own working directory. Commands are addressed with
`git -C <canonical-root>`, so there is no process-wide state to get wrong.

The canonical root is discovered once and stored, along with the common Git
directory and this worktree's own Git directory. A scan re-checks **both**: if the
registered root no longer resolves to itself, or the Git directory changed, the scan
refuses with exit code 7 and asks the user to re-register, rather than silently
indexing a different repository or a different worktree.

### Symlinks are never traversed

Inside a repository, Atlas resolves every relative path component by component with
`openat(..., O_DIRECTORY | O_NOFOLLOW)`, `lstat`ing each component first:

- an intermediate component that is a symlink → the file is **refused**,
  `unsafe_path` is recorded, and the reason is reported
- a final component that is a symlink → it is **never opened**; `readlinkat` reads
  the link text and Atlas hashes that text
- anything that is neither a regular file nor a symlink → recorded as `other`

So a tracked symlink `escape.txt -> /etc/passwd` yields the SHA-256 of the string
`/etc/passwd`, and `/etc/passwd` is never opened. Only three places in Atlas open
anything inside a repository: the root directory fd itself, and the two `O_NOFOLLOW`
`openat` calls in `src/core/pathrep.c`.

## No shell, anywhere

Subprocesses are created with `fork` and `execve` and an explicit argument vector,
in exactly one function (`src/core/proc.c`). There is no `system()`, no `popen()`,
and no `/bin/sh -c`. `argv[0]` must be an absolute path. Shell metacharacters in a
filename, a commit message or a configuration value are inert bytes.

## Bounded execution

| Bound | Default | On breach |
| --- | --- | --- |
| wall-clock timeout | 60 s per invocation | `SIGTERM` to the process group, `SIGKILL` after 200 ms, reported as a timeout (exit 6) |
| captured stdout | 256 MiB | `SIGKILL`, command fails naming the limit |
| captured stderr | 64 KiB | further output discarded |
| single Git record | 16 MiB | parse fails (exit 6) |
| hashed file size | 256 MiB | recorded without a content hash |
| diff entries | 2000 | report marked `truncated`, counts stay exact |

The child is put in its own process group, so a timeout kills the whole group and
no grandchild survives. `stdin` is always `/dev/null`, so nothing can block on a
prompt.

## What is proven, not asserted

`tests/test_git_hardening.c` (13 cases) plants each vector in a real repository and
runs every Atlas command against it. Each vector test has two halves:

- **control**: plain Git, driven by the fixture, really does run the helper. A
  vector that cannot fire would make the assertion vacuous, so the control is
  asserted, not assumed.
- **Atlas**: the helper never runs, and the repository tree digest is unchanged.

The helper is a small compiled program, copied into the fixture so the config value
is a bare absolute path with no arguments: Git execs it directly, with no shell
involved. If it ever runs, it leaves a file behind.

Covered: repo-local `core.fsmonitor`; `diff.external`; `.gitattributes` plus a
`textconv` driver; a `core.hooksPath` directory of hooks; hostile inherited
`GIT_EXTERNAL_DIFF`, `GIT_PAGER`, `PAGER`, `GIT_ASKPASS`, `SSH_ASKPASS`,
`GIT_SSH_COMMAND`, `GIT_CONFIG_COUNT`/`KEY_0`/`VALUE_0`, `GIT_TRACE*`; hostile
inherited `GIT_DIR`, `GIT_WORK_TREE`, `GIT_INDEX_FILE`, `GIT_OBJECT_DIRECTORY`,
`GIT_ALTERNATE_OBJECT_DIRECTORIES` pointing at a decoy repository; askpass and
credential helpers with an unreachable remote; promisor config; a promisor pack; and
a repository that becomes partial after registration.

`scripts/adversarial.sh` (`make adversarial`) repeats the exercise from outside
under `strace -f`, and additionally requires that the set of executables in the
whole process tree is exactly {`atlas`, `git`}, that no `AF_INET` socket or
`connect` is attempted, that no child opens `/dev/tty`, that the decoy repository is
never read, and that a promisor repository produces a valid structured JSON error
with exit code 7.

## A1: `git config` on the allowlist, and why the allowlist alone is not enough for it

`config` was added to `READONLY_SUBCOMMANDS` so that partial-clone detection can
ask git what its own configuration is. The subcommand name says nothing about
whether an invocation reads or writes — `git config a.b c` writes — so the
allowlist does not cover it.

Instead, every `config` invocation is matched against a **positive allowlist of
complete argument vectors**:

```
config --includes --null --get-regexp ^remote\..*\.promisor$
config --includes --null --get-regexp ^remote\..*\.partialclonefilter$
config --includes --null --get-regexp ^extensions\.partialclone$
```

Nothing else is permitted. A denylist of writing options would have to enumerate
`--add`, `--replace-all`, `--unset`, `--unset-all`, `--edit`,
`--rename-section`, `--remove-section`, and whatever a future git adds — and the
first one missed is a write to a repository Atlas promised never to modify.
Matching the whole vector means a new query has to be added deliberately, in one
place, and cannot be smuggled in by a new call site.

## A1: exact, fail-closed partial-clone detection

A0 detected a partial (promisor) clone by reading the first 64 KiB of
`<common-dir>/config` and searching for the substrings `promisor` and
`partialclone`. That was wrong in five separate ways, each of which is a bypass
rather than a false negative in the safe direction:

1. a config larger than 64 KiB hid the marker behind padding, and a config can be
   padded to any size with comment lines
2. a marker straddling the 64 KiB boundary was split and matched neither half
3. `$GIT_DIR/config.worktree`, which git reads when `extensions.worktreeConfig`
   is set, was never opened
4. `include.path` and `includeIf` files, which git honours as if their contents
   were inline, were never opened either
5. it was a substring match, so it also *over*-refused any repository whose
   config merely mentioned the word — a branch named `promisor-experiment` was
   enough

It is replaced by asking git, which is the only component that knows exactly
which files make up a repository's configuration and how they compose.
`--includes` makes git resolve `include.path` and `includeIf`; the hardened
environment already pins `GIT_CONFIG_GLOBAL` and `GIT_CONFIG_SYSTEM` to
`/dev/null`, so the answer describes this repository and nothing else.
`--get-regexp` matches git's own canonicalised key names — lowercased for section
and variable — so key case variants and arbitrary remote names are covered by the
pattern rather than by string matching.

Plus a bounded scan for a `*.promisor` file in `objects/pack`, which is proof
independent of configuration: it is what git writes after a filtered fetch and it
survives the configuration being edited away.

**Fail-closed throughout.** `git config --get-regexp` exits 0 when it matched and
1 when it did not. Every other outcome — a signal, a timeout, truncated stdout,
an undocumented exit code, success with no output, an unreadable `objects/pack`,
more pack entries than the scan will read — is treated as *cannot prove this
repository is complete*, and refuses.

`tests/test_git_hardening.c` has a regression case for each of the five bypasses,
including the verified ~150 KiB padding one, plus one asserting that an innocent
mention is **not** over-refused.

## A1: the git executable cache is no longer a data race

A0 cached the resolved git path in two plain globals and read them without
synchronisation. That was correct exactly as long as Atlas was single-threaded,
and A1 is not: the daemon runs a writer thread, a watcher thread and a worker
pool, any of which may open a repository.

The cache is now immutable after publication and every access is serialised by a
mutex. `atlas_git_runtime_init()` forces the one-time resolution before the
daemon creates any thread, so the PATH search happens once, deterministically,
and a missing git is reported at startup rather than by whichever worker first
needed it. Verified under ThreadSanitizer.
