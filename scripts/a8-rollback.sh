#!/bin/sh
# Atlas - A8 rollback to the accepted A7.1 deployment.
# Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
#
# Restores the exact A7.1 binary and schema-7 database. Dry run by default.
#
# It preserves failed A8 state rather than deleting it: evidence of what went
# wrong is worth more than a tidy directory, and a rollback that destroys the
# thing you would debug is one you can only perform once.
set -eu

APPLY=0
BINARY=""
BACKUP=""
for arg in "$@"; do
  case "$arg" in
    --apply) APPLY=1 ;;
    --binary=*) BINARY="${arg#--binary=}" ;;
    --backup=*) BACKUP="${arg#--backup=}" ;;
    *) printf 'usage: a8-rollback.sh --binary=PATH --backup=PATH [--apply]\n' >&2; exit 2 ;;
  esac
done
[ -n "$BINARY" ] && [ -n "$BACKUP" ] || {
  printf 'a8-rollback.sh: --binary= and --backup= are required\n' >&2; exit 2; }

say() { printf '%s\n' "$*"; }
run() {
  if [ "$APPLY" -eq 1 ]; then say "+ $*"; "$@"; else say "  would run: $*"; fi
}

[ "$APPLY" -eq 0 ] || [ "$(id -u)" -eq 0 ] || {
  printf 'a8-rollback.sh: --apply needs root\n' >&2; exit 3; }

say "A7.1 binary : $BINARY ($(sha256sum "$BINARY" | cut -d' ' -f1))"
say "schema-7 db : $BACKUP ($(sha256sum "$BACKUP" | cut -d' ' -f1))"

# 1. Stop and disable the dispatcher first: the untrusted principal stops before
#    anything else changes underneath it.
run systemctl disable --now atlas-dispatcher.service
# 2. Stop the daemon, so nothing holds the index while it is replaced.
run systemctl stop atlas.service

# 3. Preserve the failed schema-8 state beside the original rather than deleting
#    it. Renamed, never removed.
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
run mv /var/lib/atlas/atlas.db "/var/lib/atlas/atlas.db.a8-failed-$STAMP"

# 4. Restore the verified schema-7 backup and the A7.1 binary.
run install -o atlasd -g atlasd -m 0600 "$BACKUP" /var/lib/atlas/atlas.db
run install -o root -g root -m 0755 "$BINARY" /usr/local/bin/atlas

# 5. Bring A7.1 back.
run systemctl start atlas.service

say ''
say 'Rolled back. Verify: schema 7, three repositories, two PROPOSED decisions,'
say 'the socket answering, and the authority profile unchanged.'
