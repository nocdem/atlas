#!/bin/sh
# Atlas - A8 privileged deployment.
# Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
#
# Dry run is the default. `--apply` is a second, deliberate invocation — the
# same discipline scripts/a71-deploy.sh follows and for the same reason.
#
# This script never uses eval, never interpolates repository text as shell,
# never recursively deletes, never chowns a tree, and names an indexed
# repository only inside a read-only unit directive.
set -eu

APPLY=0
CANDIDATE=""
for arg in "$@"; do
  case "$arg" in
    --apply) APPLY=1 ;;
    --candidate=*) CANDIDATE="${arg#--candidate=}" ;;
    *) printf 'usage: a8-deploy.sh --candidate=PATH [--apply]\n' >&2; exit 2 ;;
  esac
done
[ -n "$CANDIDATE" ] || { printf 'a8-deploy.sh: --candidate=PATH is required\n' >&2; exit 2; }
[ -x "$CANDIDATE" ] || { printf 'a8-deploy.sh: %s is not executable\n' "$CANDIDATE" >&2; exit 2; }

say() { printf '%s\n' "$*"; }
run() {
  if [ "$APPLY" -eq 1 ]; then
    say "+ $*"
    "$@"
  else
    say "  would run: $*"
  fi
}

if [ "$APPLY" -eq 1 ]; then
  say 'Atlas A8 deployment — APPLYING'
  [ "$(id -u)" -eq 0 ] || { printf 'a8-deploy.sh: --apply needs root\n' >&2; exit 3; }
else
  say 'Atlas A8 deployment — DRY RUN (nothing will be changed)'
fi

SRC="$(cd "$(dirname "$0")/.." && pwd)"
say "candidate: $CANDIDATE"
say "candidate digest: $(sha256sum "$CANDIDATE" | cut -d' ' -f1)"

# 1. The binary, installed atomically: written beside the target and renamed, so
#    no reader ever sees a partial file and a failure leaves the old one.
run install -o root -g root -m 0755 "$CANDIDATE" /usr/local/bin/.atlas.a8.new
run mv -f /usr/local/bin/.atlas.a8.new /usr/local/bin/atlas

# 2. The orchestration policy. Root-owned and not group-writable, or Atlas
#    refuses to read it — which is the whole point of it being root-owned.
if [ -f /etc/atlas/orchestration.conf ]; then
  say '  /etc/atlas/orchestration.conf already exists; leaving it alone'
else
  run install -o root -g root -m 0644 "$SRC/deploy/a8/orchestration.conf.template" \
      /etc/atlas/orchestration.conf
fi

# 3. The worker root. Created, never chowned recursively.
run install -d -o atlas-worker -g atlas-worker -m 0700 /var/lib/atlas-worker

# 4. The dispatcher unit — installed, and deliberately NOT enabled here.
#    Enabling is a separate operator act, after the gates have passed.
run install -o root -g root -m 0644 "$SRC/deploy/a8/atlas-dispatcher.service" \
    /etc/systemd/system/atlas-dispatcher.service
run systemctl daemon-reload

say ''
say 'Installed. Not started, and the dispatcher is not enabled.'
say 'Next: migrate the index (atlas doctor with the daemon stopped), start atlasd,'
say 'verify, then `systemctl enable --now atlas-dispatcher`.'
