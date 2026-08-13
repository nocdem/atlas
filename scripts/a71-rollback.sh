#!/bin/sh
# Atlas - A7.1 rollback to the A5 per-user deployment.
# Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
#
# Returns operational state to the pre-A7.1 baseline: the system service stopped
# and disabled, and the original per-user daemon running again against the
# original schema-6 database, which A7.1 never modified.
#
# What it deliberately does NOT do:
#
#   - delete the failed schema-7 state under /var/lib/atlas. That is evidence,
#     and a rollback that destroys the evidence makes the failure unfixable.
#   - delete the `atlasd` or `atlas-worker` accounts. A disabled non-login
#     account is safer than an improvised destructive cleanup, and removing one
#     that still owns files would orphan them.
#   - delete /usr/local/bin/atlas, /etc/atlas or the unit. They are inert once
#     the service is stopped and disabled, and they are what a second attempt
#     needs.
#   - touch the retained A5 backup or the original database.
#
# Dry-run by default, like the deployment. `--apply` performs it.

set -eu

APPLY=0
while [ $# -gt 0 ]; do
  case "$1" in
    --apply) APPLY=1 ;;
    -h|--help) printf 'usage: a71-rollback.sh [--apply]\n'; exit 0 ;;
    *) printf 'unknown argument: %s\n' "$1" >&2; exit 2 ;;
  esac
  shift
done

if [ "$(id -u)" != "0" ]; then
  printf 'a71-rollback: must run as root\n' >&2
  exit 2
fi

# The operator account. Derived from the invocation rather than baked in, so
# this script carries no site-specific identity: under sudo the invoking user
# is the operator, and ATLAS_A71_OPERATOR overrides it explicitly.
OPERATOR="${ATLAS_A71_OPERATOR:-${SUDO_USER:-$(id -un)}}"
OPUID=$(id -u "$OPERATOR")

run() {
  if [ "$APPLY" -eq 1 ]; then printf '  + %s\n' "$*"; "$@"; else printf '  would: %s\n' "$*"; fi
}

if [ "$APPLY" -eq 0 ]; then
  printf 'Atlas A7.1 rollback — DRY RUN\n\n'
else
  printf 'Atlas A7.1 rollback — APPLYING\n\n'
fi

printf 'stop and disable the system service\n'
run systemctl stop atlas.service
run systemctl disable atlas.service

printf '\npreserve the failed state as evidence\n'
printf '  (left in place: /var/lib/atlas, journalctl -u atlas)\n'

printf '\nneutralise the system policy so clients stop resolving the system index\n'
printf '  the policy is what makes a client prefer /var/lib/atlas; moving it aside\n'
printf '  returns every client to per-user resolution in one step, and keeps the\n'
printf '  file for diagnosis.\n'
if [ -f /etc/atlas/system.conf ]; then
  run mv /etc/atlas/system.conf /etc/atlas/system.conf.rolled-back
fi

printf '\nrestart the original per-user daemon\n'
run runuser -u "$OPERATOR" -- env XDG_RUNTIME_DIR=/run/user/"$OPUID" \
    systemctl --user enable --now atlas.service

printf '\nverify the rollback target\n'
if [ "$APPLY" -eq 1 ]; then
  runuser -u "$OPERATOR" -- env XDG_RUNTIME_DIR=/run/user/"$OPUID" \
      systemctl --user is-active atlas.service || true
  printf '  original database: %s\n' \
      "$(sha256sum "/home/$OPERATOR/.local/share/atlas/atlas.db" | cut -d' ' -f1)"
fi

printf '\n'
printf 'rollback %s.\n' "$([ "$APPLY" -eq 1 ] && printf 'applied' || printf 'dry run complete')"
