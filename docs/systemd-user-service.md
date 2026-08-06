# Running Atlas as a systemd user service

Atlas runs as **you**, not as root. The index describes your repositories and
lives in your data directory; a system-wide daemon reading it would cross a
privilege boundary Atlas has no reason to cross.

There is exactly one step in this guide that needs root, and it is optional.

## The intended setup, end to end

```sh
make
make test
sudo make install                      # optional: a system-wide binary
atlas repo add /path/to/dna --name dna
atlas service install --user
systemctl --user daemon-reload
systemctl --user enable --now atlas
```

`sudo` there applies **only** to installing the binary into `/usr/local/bin`. If
you would rather not, skip it and run `atlas` from the build tree or from
`~/.local/bin`; the generated unit names whatever binary generated it, so it will
point at the right one either way.

The daemon and everything it writes run as your normal user.

## Reviewing the unit before installing it

```sh
atlas service print
```

changes nothing. It writes the unit to stdout, so you can read it, diff it, or
redirect it yourself. `atlas service install --user` writes exactly those bytes.

## What `atlas service install --user` does

- writes `$XDG_CONFIG_HOME/systemd/user/atlas.service` (or
  `~/.config/systemd/user/atlas.service` when `XDG_CONFIG_HOME` is unset)
- creates the directory chain if needed
- writes **atomically**, via a temporary file in the same directory and a rename,
  so systemd never sees a half-written unit
- mode 0600
- reports exactly what it changed

and, deliberately, **does not**:

- enable the service
- start the service
- run `daemon-reload`
- use `sudo`, or suggest it

A command that installs a unit and also starts it has made a decision you did not
ask for. The two steps stay separate, and the JSON output carries
`"enabled": false` and `"started": false` so a script can check.

### It will not overwrite a unit you wrote

The generated unit carries a marker line. If `atlas.service` exists and does not
have it, install refuses and tells you to review it against `atlas service
print`. `--force` overrides that.

`--force` does **not** override a symlink at the unit path. Following one would
let anything that can write the unit directory make Atlas overwrite a file
somewhere else; that is refused unconditionally.

`atlas service uninstall --user` removes the unit, and only if Atlas wrote it.

## Surviving logout

A systemd **user** manager normally stops when your last session ends. Over SSH
that means the daemon stops when you log out.

To keep it running:

```sh
sudo loginctl enable-linger $USER
```

This is the one root step, it is needed once, and it is a decision about your
account rather than about Atlas. Without it, the daemon runs while you are logged
in and stops when you leave — which is a perfectly reasonable way to use it, as
long as you know that is what is happening. `atlas daemon status` will tell you
it is not running.

## What the unit sets, and why

| directive | reason |
| --- | --- |
| `Type=simple`, foreground `ExecStart` | systemd owns supervision and log capture; a service that forks hides its own failures |
| `Restart=on-failure`, `RestartSec=5` | a crash is recovered from; a clean exit is respected |
| `UMask=0077` | files Atlas creates describe private repositories |
| `RuntimeDirectory=atlas`, `RuntimeDirectoryMode=0700` | systemd creates `$XDG_RUNTIME_DIR/atlas` at 0700 and removes it on stop |
| `ReadWritePaths=%h/.local/share/atlas` | `ProtectHome=read-only` would otherwise block the index; this is the one place the daemon writes |
| `PrivateNetwork=yes`, `IPAddressDeny=any` | Atlas performs no network access at all, so removing the ability to costs nothing and proves the claim |
| `NoNewPrivileges`, `ProtectSystem=strict`, `ProtectKernel*`, `MemoryDenyWriteExecute`, `SystemCallFilter=@system-service` | none of these is load-bearing for correctness — Atlas needs none of what they remove — so each one is free |

The executable path is written literally, and Atlas **refuses** to generate a unit
for a path containing a space, a quote, a `%`, a `$` or a `;`. Escaping those
correctly for systemd is possible; refusing is better, because a unit file is
executed and a subtly mis-escaped `ExecStart` is command injection with extra
steps. If Atlas lives at such a path, use `atlas service print` and write the
unit by hand.

## Using a non-default data directory

```sh
atlas service install --user --data-dir /srv/atlas-data
```

The override is written into `ExecStart`, and the same path restrictions apply to
it. Remember to adjust `ReadWritePaths` in the unit if the directory is not under
your home.

## Checking it

```sh
systemctl --user status atlas
journalctl --user -u atlas -f
atlas daemon status
atlas daemon ping        # exits 0 only if the daemon answered
```

`atlas daemon status` reports two independent facts: whether something holds the
writer lock, and whether the socket answers. A daemon that is running but not
answering is a state worth seeing, and a single boolean would hide it.

## Uninstalling

```sh
systemctl --user disable --now atlas
atlas service uninstall --user
systemctl --user daemon-reload
```

Removing the unit does not touch the index, and nothing Atlas does at any point
touches a registered repository.
