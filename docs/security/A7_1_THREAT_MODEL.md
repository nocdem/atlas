# Atlas A7.1 — threat model

This supersedes `A7_THREAT_MODEL.md` for the deployed system. A7's actor list
still describes the software; what changes here is **who runs as what**, and
therefore which of those actors the OS can stop before Atlas is involved at all.

Read the first section before anything else. It is the difference between A7 and
A7.1, and getting it wrong in either direction makes the rest meaningless.

## The principals, and which are trusted

| principal | trusted? | what it is |
|---|---|---|
| `root` | **yes, by design** | owns the binary, the policies and the unit |
| the operator account | **yes, by design** | the human operator and deployment principal; holds `NOPASSWD: ALL` |
| `atlasd` | **partly** — trusted with the index, nothing else | the service account the daemon runs as; no login, no password, no sudo, no writable code |
| `atlas-worker` | **no. This is the adversary.** | the account every persistent or autonomous model process runs as |
| other local users | no | anyone else with a shell on the machine |

### The operator account is trusted and can bypass everything. This is deliberate.

The operator account holds passwordless root. It can read `/var/lib/atlas/atlas.db`, replace
`/usr/local/bin/atlas`, rewrite `/etc/atlas/system.conf`, become `atlasd`, and
approve any decision it likes. **Atlas does not defend against this and must
never be described as doing so.** Any process intentionally launched as the operator account
— including an AI session started by the operator — is outside Atlas' OS
isolation guarantee, by the operator's explicit decision.

What follows from that, and is binding:

- No test asserts that the operator account is unable to do something. Such a test would
  pass only until the operator used their own machine.
- No document claims the operator account is constrained.
- The separation is meaningful because **A8's worker dispatcher and every
  persistent model process run as `atlas-worker`, never as the operator account**, except
  where a root-owned policy names an exception. That is the architectural
  commitment A7.1 exists to make possible; if it is broken outside such a
  policy, the guarantee below is void and no code change will restore it.
- **A8.1 declares one such exception, and the honest reading is that it opts
  those jobs out of this document.** When `/etc/atlas/orchestration.conf` sets
  `model_dispatcher_uid`, jobs whose driver needs a live model run as that uid —
  in this deployment the operator account — with that account's filesystem authority. For
  those jobs `atlas-worker` is not the principal and nothing below applies to
  them; the reason is that the only Claude Code credential on this machine is a
  personal session, and Atlas will not copy, read or relocate one. Jobs on every
  other driver still run as `atlas-worker` under the whole of this model.

### `atlas-worker` is the adversary A7.1 defends against

Assume it has: a shell, arbitrary code execution, the ability to allocate
pseudo-terminals, membership in `atlas-clients`, and unlimited time. Assume it is
actively hostile and that anything it can reach, it will.

## What the OS stops before Atlas is involved

These are kernel-enforced, not Atlas-enforced, and that is what makes them worth
more than the A7 checks they replace:

| `atlas-worker` attempts | stopped by |
|---|---|
| read or write `/var/lib/atlas/atlas.db` | directory mode 0700 owned by `atlasd` |
| read `/var/backups/atlas` | directory mode 0700 owned by `atlasd` |
| replace `/usr/local/bin/atlas` | root-owned, mode 0755, root-owned parents |
| edit `/etc/atlas/*.conf` | root-owned, mode 0644, root-owned parents |
| edit or mask the unit | root-owned `/etc/systemd/system` |
| stop or restart the service | not in `sudo`; no polkit rule grants it |
| become `atlasd` or root | no sudo entry, no setuid helper, no capabilities |

A7 could only *refuse* these in Atlas code, which a process able to write the
database did not have to call. A7.1 makes them fail in the kernel, in a process
that never reaches Atlas at all.

## What Atlas still has to enforce itself

The socket is the one place `atlas-worker` and `atlasd` meet, so everything on
it is Atlas' problem:

1. **Who may connect.** `SO_PEERCRED`, checked against a root-owned allowlist,
   before a byte is read. Never a uid, gid, pid or role from the request body,
   the environment or `/proc` — `tests/test_a71_syspolicy.c` sends ten such
   claims and requires the answer to be byte-identical without them.
2. **What a connected client may do.** The A7 model-safe surface: reads,
   proposals, session bookkeeping. The lifecycle, registry, backup, restore and
   maintenance methods **do not exist in the protocol**, so there is no per-uid
   method table to get wrong and no privileged tier to escalate into.
3. **Which index a client resolves.** With a system policy active, `/var/lib/atlas`
   and nothing else, unless `--data-dir` says otherwise explicitly. There is no
   silent fallback to the stale per-user database A7.1 leaves in place — a
   client that cannot reach the daemon fails rather than reading history from
   before the cutover.
4. **That the socket is not more open than intended.** Owner, group and mode are
   set explicitly at bind and then read back; a mismatch unlinks the socket and
   the daemon does not start.

## The authority model

`/etc/atlas/authority.conf` names **the `atlasd` uid**, not the operator's login
account, because `atlasd` is the only principal that can open the index at all.
A grant additionally requires the running binary to be root-owned and unwritable
by anyone else, which `/usr/local/bin/atlas` satisfies and `~/.local/bin/atlas`
never could.

Consequences, stated plainly:

- `atlas-worker` probes `LOCKED`. It is not the operator uid, and it could not
  open the database even if it were.
- The operator account probes `LOCKED` in its own right — and, being root-equivalent, can
  trivially become `atlasd` and probe `GRANTED`. That is not a defect; it is the
  trusted-operator path, and it is how the documented ceremony works.
- Terminal presence, PTY ownership, `isatty`, environment and typed
  confirmations remain **user-experience confirmations only**. None is described
  or tested as a security boundary anywhere in Atlas, and none may become one.

## Out of scope, explicitly

- **Root and the operator account.** Trusted by design, per the operator's decision.
- A malicious trusted compiler, and supply-chain attacks on the toolchain.
- Physical access, memory-bus attacks, side channels.
- Denial of service by any principal that can exhaust disk or CPU.
- Cryptographic identity of any kind: no signatures, no attestation, no
  non-repudiation. `LOCAL_OPERATOR_CONFIRMED` names a channel, never a person.
- Anything reachable only after `atlas-worker` has already obtained root by a
  route outside Atlas (a kernel bug, an unrelated setuid binary). A7.1's
  privilege-escalation audit enumerates the routes that exist on this machine
  and finds none available to `atlas-worker`; it cannot promise none will ever
  exist.

## The one-sentence version

Atlas A7.1 makes the index, the backups, the binary and the policies unreachable
by `atlas-worker` through the kernel rather than through Atlas' own checks, gives
it a socket whose entire vocabulary is reads and proposals, and is honest that
The operator account and root stand outside all of it by the operator's choice.
