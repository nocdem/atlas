# Atlas A7.1 — operating the separated deployment

Companion to `A7_1_THREAT_MODEL.md`. This is the procedure: what to install, in
what order, how to verify it, how to perform a lifecycle transition, and how to
go back.

Everything here needs root except where marked. Nothing here is exposed through
MCP, the plugin, a hook or any model-callable helper, and nothing here may be.

## The layout

| path | owner | mode | what |
|---|---|---|---|
| `/usr/local/bin/atlas` | `root:root` | 0755 | the only executable the service runs |
| `/etc/atlas/system.conf` | `root:root` | 0644 | socket, index and client allowlist |
| `/etc/atlas/authority.conf` | `root:root` | 0644 | the lifecycle operator uid |
| `/etc/systemd/system/atlas.service` | `root:root` | 0644 | the system unit |
| `/var/lib/atlas` | `atlasd:atlasd` | 0700 | the authoritative index |
| `/var/backups/atlas` | `atlasd:atlasd` | 0700 | operational backups |
| `/run/atlas` | `atlasd:atlas-clients` | 0750 | runtime directory |
| `/run/atlas/atlas.sock` | `atlasd:atlas-clients` | 0660 | the only client entry point |

Accounts: `atlasd` (service, no login), `atlas-worker` (untrusted model, no
login), group `atlas-clients` (socket access only). `atlasd` and `atlas-worker`
are both members of `atlas-clients`; **neither `atlas-worker` nor the operator is
a member of `atlasd`**.

## Installing

```sh
sh scripts/a71-preflight.sh              # changes nothing; run first
sudo sh scripts/a71-deploy.sh --binary /path/to/atlas          # DRY RUN
sudo sh scripts/a71-deploy.sh --binary /path/to/atlas --apply
```

The binary must come from a clean extraction of the A7.1 commit, not from a
working tree. `a71-deploy.sh` installs accounts, directories, the executable and
the unit; it does **not** write the two policy files, enable the service, migrate
anything or stop the old daemon. Those are ordered steps below, deliberately
separate, so the deployment can be stopped between them and inspected.

Write the policies from `deploy/a71/*.template`, filling in this machine's uids:

```sh
sudo install -o root -g root -m 0644 /dev/stdin /etc/atlas/system.conf <<EOF
socket_path = /run/atlas/atlas.sock
data_dir = /var/lib/atlas
client_group = atlas-clients
client_uid = $(id -u atlas-worker)
client_uid = $(id -u "$OPERATOR")   # the operator account
EOF

sudo install -o root -g root -m 0644 /dev/stdin /etc/atlas/authority.conf <<EOF
operator_uid = $(id -u atlasd)
EOF
```

## Cutting over

The design is **copy, migrate, switch** — the original per-user database is never
migrated in place, so it stays an immediate rollback target that A7.1 has not
touched.

```sh
# 1. stop the writer, so the copy is of a quiet database
systemctl --user stop atlas.service

# 2. a verified backup of the schema-6 state, taken with the OLD binary
~/.local/bin/atlas backup create ~/.local/state/atlas/backups/pre-a71-$(date -u +%Y%m%dT%H%M%SZ).db
~/.local/bin/atlas backup verify  <that file>

# 3. copy — never move — the database into the separated state directory
sudo install -o atlasd -g atlasd -m 0600 \
     ~/.local/share/atlas/atlas.db /var/lib/atlas/atlas.db

# 4. start the system service; it migrates 6 -> 7 on first write
sudo systemctl start atlas.service

# 5. verify before enabling anything
sudo sh scripts/a71-verify.sh

# 6. only now: disable the old, enable the new
systemctl --user disable --now atlas.service
sudo systemctl enable atlas.service
```

The `-wal` and `-shm` sidecars are deliberately not copied. They are meaningful
only alongside the database at the instant they were written, and the daemon was
stopped in step 1, so the checkpointed database is complete on its own.

## Performing a lifecycle transition

Approving, rejecting, superseding or revalidating a decision needs the `atlasd`
uid, the root-owned binary and the root-owned policy — which means it happens
offline, with the daemon stopped, from a terminal the operator controls.

**This is a human workflow. It is not exposed to any model surface and must not
be.**

1. Open a terminal that is **not** driven by an AI process.
2. `sudo systemctl stop atlas.service`
3. `sudo -u atlasd /usr/local/bin/atlas decision approve <repo> <decision-id>`
   — read what it prints, then type the confirmation.
4. `sudo systemctl start atlas.service`
5. `sudo -u atlasd /usr/local/bin/atlas decision history <repo> <decision-id>`
   and `sudo -u atlasd /usr/local/bin/atlas doctor` — the ledger and the status
   cache must agree.
6. `sudo -k` to invalidate the sudo timestamp.

Step 3's confirmation prompt is a **user-experience check**, not a security
boundary: it protects against approving the wrong revision by accident. It
proves nothing about who typed it, and Atlas does not claim otherwise.

## Going back

```sh
sudo sh scripts/a71-rollback.sh            # DRY RUN
sudo sh scripts/a71-rollback.sh --apply
```

It stops and disables the system service, moves `/etc/atlas/system.conf` aside so
every client resolves per-user again, and re-enables the original user service
against the original schema-6 database.

It deliberately does not delete `/var/lib/atlas` (evidence), the accounts (a
disabled non-login account is safer than an improvised cleanup), or the installed
binary and policies (inert once stopped, and needed for a second attempt).

## The old per-user database is not authoritative

After cutover, `~/.local/share/atlas/atlas.db` is **retained rollback evidence
and nothing else**. It is not read by anything: with a system policy in force a
client resolves `/var/lib/atlas` and there is no fallback, so a stopped daemon
produces an error rather than a stale answer.

Its contents are never edited to mark it — a database modified to say it is not
authoritative is a database that has been modified, which is exactly what a
rollback target must not be.

## What to check if something looks wrong

```sh
sudo -u atlasd /usr/local/bin/atlas doctor     # deployment mode, schema, authority
systemctl status atlas.service
journalctl -u atlas.service -n 50
sudo sh scripts/a71-verify.sh
```

`doctor` reports the deployment mode and the authority profile and creates
nothing, so it is safe on a machine that is already misbehaving.

## Two things the first deployment taught, recorded so the next one does not

### `chown` is a privileged syscall under this sandbox

The daemon originally handed the socket to the client group with an
unconditional `chown(2)` at bind time. `chown` is in systemd's `@privileged`
syscall set, which `SystemCallFilter=~@privileged` removes, so the daemon was
killed with **SIGSYS before it ever bound a socket** — and the symptom was a
restart loop with `status=31/SYS` in the journal, which reads like a crash
rather than like a policy problem.

The fix is in two halves, and both matter. The unit gives the daemon
`atlas-clients` as its **primary** group, so systemd's `RuntimeDirectory` and the
socket are created with the right group and no `chown` is needed; and Atlas calls
`chown` only when the group is not already correct, so a hand-run daemon outside
the sandbox still works. The verification is unconditional either way.

`scripts/a71-verify.sh` checks the journal for `status=31/SYS` since the current
service start, so this cannot regress silently.

### Group membership needs a fresh session

The operator account and `atlas-worker` reach the socket through `atlas-clients`. A process
that was already running when the group was added keeps its old credential set
and gets `Permission denied` — including any Claude MCP or hook process started
before the cutover. Log out and back in, or restart Claude, after the deployment.

This is also the cleanest demonstration of the no-fallback rule: such a process
reports *"daemon not responding"* and stops. It does **not** quietly open the
pre-cutover database in `~/.local/share/atlas`, which is exactly what would have
made a stale answer look like a fresh one.
