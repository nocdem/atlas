#!/bin/sh
# Atlas - A7.1 deployment preflight.
# Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
#
# Answers one question: is this machine in the state the A7.1 cutover expects?
#
# It changes nothing. It creates no account, no directory and no file, it does
# not start or stop anything, and it does not need root for most of what it
# checks. Run it before `a71-deploy.sh`, and run it again afterwards if a
# deployment failed and you want to know where it stopped.
#
# POSIX sh, no bashisms, no eval, and no repository-controlled text is ever
# executed. Every path is a literal.

set -eu

# The repositories this deployment indexes, space separated. Site-specific, so
# it is a variable rather than a baked-in list: set ATLAS_REPOS to match the
# ReadOnlyPaths line in atlas.service.
ATLAS_REPOS="${ATLAS_REPOS:-/opt/atlas}"

RC=0
note() { printf '  %-46s %s\n' "$1" "$2"; }
fail() { printf '  %-46s FAIL: %s\n' "$1" "$2"; RC=1; }

printf 'Atlas A7.1 preflight\n\n'

printf 'accounts and groups\n'
for u in atlasd atlas-worker; do
  if id "$u" >/dev/null 2>&1; then
    note "$u" "exists (uid $(id -u "$u"))"
    shell=$(getent passwd "$u" | cut -d: -f7)
    case "$shell" in
      */nologin|*/false) note "  login shell" "$shell (non-login)" ;;
      *) fail "  login shell" "$shell is a login shell" ;;
    esac
  else
    note "$u" "absent (deploy will create)"
  fi
done
if getent group atlas-clients >/dev/null 2>&1; then
  note "group atlas-clients" "exists (gid $(getent group atlas-clients | cut -d: -f3))"
else
  note "group atlas-clients" "absent (deploy will create)"
fi

printf '\nprivileged target paths\n'
for p in /usr/local/bin /etc /etc/systemd/system /var/lib /var/backups /run; do
  if [ -L "$p" ]; then
    fail "$p" "is a symbolic link"
  elif [ ! -d "$p" ]; then
    fail "$p" "is not a directory"
  else
    owner=$(stat -c %u "$p")
    mode=$(stat -c %a "$p")
    if [ "$owner" != "0" ]; then
      fail "$p" "owned by uid $owner, not root"
    else
      note "$p" "root-owned, mode $mode"
    fi
  fi
done

printf '\nexisting Atlas deployment\n'
for p in /usr/local/bin/atlas /etc/atlas /etc/atlas/system.conf /etc/atlas/authority.conf \
         /var/lib/atlas /var/backups/atlas /etc/systemd/system/atlas.service; do
  if [ -e "$p" ] || [ -L "$p" ]; then
    note "$p" "PRESENT (deploy will preserve or refuse)"
  else
    note "$p" "absent"
  fi
done

printf '\nthe A5 per-user deployment (the rollback target)\n'
USER_BIN="$HOME/.local/bin/atlas"
USER_DB="$HOME/.local/share/atlas/atlas.db"
USER_UNIT="$HOME/.config/systemd/user/atlas.service"
for p in "$USER_BIN" "$USER_DB" "$USER_UNIT"; do
  if [ -f "$p" ]; then
    note "$(basename "$p")" "$(sha256sum "$p" | cut -c1-16)... $p"
  else
    fail "$(basename "$p")" "missing: $p"
  fi
done

printf '\nindexed repositories (must be readable, never written)\n'
for r in $ATLAS_REPOS; do
  if [ -d "$r/.git" ]; then
    note "$r" "git worktree, mode $(stat -c %a "$r")"
  else
    fail "$r" "not a git worktree"
  fi
done

printf '\ndisk space\n'
for p in /var/lib /var/backups; do
  avail=$(df -Pk "$p" | awk 'NR==2 {print $4}')
  if [ "$avail" -lt 1048576 ]; then
    fail "$p" "only ${avail}K available; want at least 1G"
  else
    note "$p" "${avail}K available"
  fi
done

printf '\nrunning daemons\n'
n=$(pgrep -c -f 'atlas daemon run' 2>/dev/null || true)
[ -n "$n" ] || n=0
if [ "$n" -gt 1 ]; then
  fail "atlas daemon run" "$n processes; expected at most one"
else
  note "atlas daemon run" "$n process(es)"
fi

printf '\n'
if [ "$RC" -eq 0 ]; then
  printf 'preflight: ok\n'
else
  printf 'preflight: PROBLEMS FOUND\n'
fi
exit "$RC"
