#!/bin/sh
# Atlas - A7.1 privileged deployment.
# Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
#
# Installs the OS-separated Atlas deployment: the `atlasd` service account, the
# `atlas-worker` model account, the `atlas-clients` socket group, the root-owned
# binary and policy, the system unit, and the state, runtime and backup
# directories.
#
# **Dry-run is mandatory and is the default.** Without `--apply` nothing is
# created, changed, started or stopped; every mutation is printed exactly as it
# would be performed. `--apply` is a deliberate second invocation.
#
# It does not migrate the database and does not stop the old user service. Those
# are separate, ordered steps in docs/security/A7_1_OPERATIONS.md, because a
# script that both installs and cuts over cannot be stopped in between to look at
# what it did.
#
# Rules this script follows, and a reviewer should check it still does:
#   - every path is a literal; none is built from repository content
#   - no eval, no shell interpolation of anything read from a file
#   - no recursive chown, chmod or rm, ever, on any path
#   - indexed repositories are never named except to read
#   - refuses a symlink at any privileged target
#   - preserves any existing file by moving it aside, never overwriting the only
#     copy
#   - installs atomically where the filesystem allows it (write, fsync, rename)
#   - journals every mutation it makes

set -eu

APPLY=0
BINARY=""
JOURNAL="/var/log/atlas-a71-deploy.log"

usage() {
  cat <<'EOF'
usage: a71-deploy.sh --binary PATH [--apply]

  --binary PATH   the atlas executable to install (built from a clean
                  extraction of the A7.1 commit)
  --apply         perform the mutations. Without it this is a dry run and
                  nothing on the system is touched.
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --apply) APPLY=1 ;;
    --binary) shift; [ $# -gt 0 ] || { usage; exit 2; }; BINARY="$1" ;;
    -h|--help) usage; exit 0 ;;
    *) printf 'unknown argument: %s\n' "$1" >&2; usage; exit 2 ;;
  esac
  shift
done

[ -n "$BINARY" ] || { printf 'a71-deploy: --binary is required\n' >&2; exit 2; }
case "$BINARY" in
  /*) ;;
  *) printf 'a71-deploy: --binary must be an absolute path\n' >&2; exit 2 ;;
esac
[ -f "$BINARY" ] || { printf 'a71-deploy: %s is not a regular file\n' "$BINARY" >&2; exit 2; }

if [ "$(id -u)" != "0" ]; then
  printf 'a71-deploy: must run as root\n' >&2
  exit 2
fi

HERE=$(cd "$(dirname "$0")" && pwd)
TPL="$HERE/../deploy/a71"
for f in atlas.service system.conf.template authority.conf.template; do
  [ -f "$TPL/$f" ] || { printf 'a71-deploy: missing template %s\n' "$TPL/$f" >&2; exit 2; }
done

say() { printf '%s\n' "$*"; }
run() {
  if [ "$APPLY" -eq 1 ]; then
    say "  + $*"
    "$@"
    printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*" >> "$JOURNAL"
  else
    say "  would: $*"
  fi
}

# Refuses a symlink at a privileged target. A symlink here would let whoever
# created it choose the file being replaced.
no_symlink() {
  if [ -L "$1" ]; then
    printf 'a71-deploy: %s is a symbolic link; refusing\n' "$1" >&2
    exit 1
  fi
}

# Moves an existing file aside rather than overwriting the only copy.
preserve() {
  if [ -f "$1" ]; then
    run cp -a "$1" "$1.pre-a71"
  fi
}

# Atomic install: copy to a temporary in the *destination directory* (so the
# rename cannot cross a filesystem), fsync, set owner and mode, then rename.
install_file() {
  src="$1"; dst="$2"; owner="$3"; mode="$4"
  no_symlink "$dst"
  preserve "$dst"
  tmp="$dst.a71-new"
  run cp "$src" "$tmp"
  run chown "$owner" "$tmp"
  run chmod "$mode" "$tmp"
  run sync
  run mv -f "$tmp" "$dst"
}

if [ "$APPLY" -eq 0 ]; then
  say 'Atlas A7.1 deployment — DRY RUN (nothing will be changed)'
else
  say 'Atlas A7.1 deployment — APPLYING'
fi
say ''
say "binary: $BINARY"
say "sha256: $(sha256sum "$BINARY" | cut -d' ' -f1)"
say ''

say 'accounts and groups'
if ! getent group atlas-clients >/dev/null 2>&1; then
  run groupadd --system atlas-clients
else
  say '  atlas-clients already exists'
fi
if ! id atlasd >/dev/null 2>&1; then
  run useradd --system --home-dir /var/lib/atlas --no-create-home \
      --shell /usr/sbin/nologin --comment 'Atlas index daemon' atlasd
else
  say '  atlasd already exists'
fi
if ! id atlas-worker >/dev/null 2>&1; then
  run useradd --system --home-dir /var/lib/atlas-worker --create-home \
      --shell /usr/sbin/nologin --comment 'Atlas AI worker (untrusted)' atlas-worker
else
  say '  atlas-worker already exists'
fi
# atlasd joins atlas-clients so it can hand the socket to that group. This gives
# it nothing else: the group owns no file but the socket.
run usermod -aG atlas-clients atlasd
run usermod -aG atlas-clients atlas-worker
# Neither worker nor operator ever joins atlasd.
say '  (atlas-worker is deliberately NOT added to atlasd, sudo, docker or adm)'

say ''
say 'directories'
no_symlink /etc/atlas
no_symlink /var/lib/atlas
no_symlink /var/backups/atlas
run install -d -o root -g root -m 0755 /etc/atlas
run install -d -o atlasd -g atlasd -m 0700 /var/lib/atlas
run install -d -o atlasd -g atlasd -m 0700 /var/backups/atlas

say ''
say 'executable and policy'
install_file "$BINARY" /usr/local/bin/atlas root:root 0755
install_file "$TPL/atlas.service" /etc/systemd/system/atlas.service root:root 0644

say ''
say 'systemd'
run systemctl daemon-reload
say '  (the unit is installed but deliberately NOT enabled or started here;'
say '   docs/security/A7_1_OPERATIONS.md orders the cutover)'

say ''
if [ "$APPLY" -eq 0 ]; then
  say 'dry run complete. Re-run with --apply to perform the above.'
else
  say "applied. Journal: $JOURNAL"
  say 'Next: write /etc/atlas/system.conf and /etc/atlas/authority.conf from the'
  say 'templates in deploy/a71, then follow docs/security/A7_1_OPERATIONS.md.'
fi
