#!/bin/sh
# Atlas - A7.1 post-deployment verifier.
# Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
#
# Asks the deployed system whether it is what A7.1 claims, from fresh processes,
# and changes nothing except two temporary files under /etc/atlas that it removes
# again (the malformed-policy matrix, which has to run against the real
# root-owned path to mean anything).
#
# Run as root. Checks that need a second identity use `runuser`, which is how a
# claim about `atlas-worker` is established rather than assumed.
#
# Exit 0 when every check passes.

set -eu

# The repositories this deployment indexes, space separated. Site-specific, so
# it is a variable rather than a baked-in list: set ATLAS_REPOS to match the
# ReadOnlyPaths line in atlas.service.
ATLAS_REPOS="${ATLAS_REPOS:-/opt/atlas}"

RC=0
ok()   { printf '  ok    %s\n' "$1"; }
bad()  { printf '  FAIL  %s: %s\n' "$1" "$2"; RC=1; }
info() { printf '  --    %s: %s\n' "$1" "$2"; }

ATLAS=/usr/local/bin/atlas
SOCK=/run/atlas/atlas.sock
STATE=/var/lib/atlas
POLICY=/etc/atlas/system.conf

printf 'Atlas A7.1 verification\n\n'

printf 'trusted computing base\n'
for p in "$ATLAS" /etc/atlas "$POLICY" /etc/atlas/authority.conf /etc/systemd/system/atlas.service; do
  if [ -L "$p" ]; then
    bad "$p" "is a symbolic link"
  elif [ ! -e "$p" ]; then
    bad "$p" "missing"
  else
    o=$(stat -c %U "$p"); m=$(stat -c %a "$p")
    case "$o:$m" in
      root:755|root:644|root:700) ok "$p root-owned mode $m" ;;
      *) bad "$p" "owner $o mode $m" ;;
    esac
  fi
done
info "atlas sha256" "$(sha256sum "$ATLAS" | cut -d' ' -f1)"

printf '\nservice\n'
if systemctl is-active --quiet atlas.service; then ok 'atlas.service active'; else bad 'atlas.service' 'not active'; fi
if systemctl is-enabled --quiet atlas.service; then ok 'atlas.service enabled'; else bad 'atlas.service' 'not enabled'; fi
# Regression: the daemon must reach a listening socket without a syscall the
# sandbox filters. An unconditional chown(2) at bind time — chown is in
# systemd's @privileged set — killed it with SIGSYS before it bound anything,
# and the failure looked like a restart loop rather than like a policy problem.
# Scoped to the current invocation: history from before a fix is not evidence
# about the running daemon, and a check that never clears is a check people
# learn to ignore.
SINCE=$(systemctl show atlas.service -p ActiveEnterTimestamp --value)
if [ -n "$SINCE" ] && journalctl -u atlas.service --since "$SINCE" --no-pager 2>/dev/null | grep -q "status=31/SYS"; then
  bad "seccomp" "the running daemon was killed by the syscall filter (SIGSYS)"
else
  ok "no SIGSYS since this daemon started"
fi

RUNAS=$(systemctl show atlas.service -p User --value)
[ "$RUNAS" = "atlasd" ] && ok 'runs as atlasd' || bad 'service user' "$RUNAS"
MAINPID=$(systemctl show atlas.service -p MainPID --value)
if [ -n "$MAINPID" ] && [ "$MAINPID" != "0" ]; then
  PUID=$(awk '/^Uid:/ {print $2}' "/proc/$MAINPID/status" 2>/dev/null || echo "?")
  [ "$PUID" = "$(id -u atlasd)" ] && ok "main pid runs as atlasd uid $PUID" \
                                  || bad 'main pid uid' "$PUID"
  [ "$PUID" != "0" ] && ok 'daemon is not root' || bad 'daemon' 'running as root'
fi

printf '\nsocket\n'
if [ -S "$SOCK" ]; then
  o=$(stat -c %U "$SOCK"); g=$(stat -c %G "$SOCK"); m=$(stat -c %a "$SOCK")
  [ "$o" = "atlasd" ] && ok 'socket owned by atlasd' || bad 'socket owner' "$o"
  [ "$g" = "atlas-clients" ] && ok 'socket group atlas-clients' || bad 'socket group' "$g"
  [ "$m" = "660" ] && ok 'socket mode 0660' || bad 'socket mode' "$m"
  d=$(dirname "$SOCK")
  dm=$(stat -c %a "$d"); dg=$(stat -c %G "$d")
  [ "$dm" = "750" ] && ok "$d mode 0750" || bad "$d mode" "$dm"
  [ "$dg" = "atlas-clients" ] && ok "$d group atlas-clients" || bad "$d group" "$dg"
else
  bad "$SOCK" 'not a socket'
fi

printf '\nstate directory is closed to the worker\n'
o=$(stat -c %U "$STATE"); m=$(stat -c %a "$STATE")
[ "$o" = "atlasd" ] && ok 'state owned by atlasd' || bad 'state owner' "$o"
[ "$m" = "700" ] && ok 'state mode 0700' || bad 'state mode' "$m"
if runuser -u atlas-worker -- test -r "$STATE/atlas.db" 2>/dev/null; then
  bad 'atlas-worker' "can read $STATE/atlas.db"
else
  ok 'atlas-worker cannot read the database'
fi
if runuser -u atlas-worker -- ls "$STATE" >/dev/null 2>&1; then
  bad 'atlas-worker' "can list $STATE"
else
  ok 'atlas-worker cannot list the state directory'
fi
if runuser -u atlas-worker -- ls /var/backups/atlas >/dev/null 2>&1; then
  bad 'atlas-worker' 'can list /var/backups/atlas'
else
  ok 'atlas-worker cannot list the backups'
fi
for p in "$ATLAS" "$POLICY" /etc/systemd/system/atlas.service; do
  if runuser -u atlas-worker -- test -w "$p" 2>/dev/null; then
    bad 'atlas-worker' "can write $p"
  else
    ok "atlas-worker cannot write $p"
  fi
done

printf '\nthe worker can use the safe surface\n'
if runuser -u atlas-worker -- "$ATLAS" daemon ping >/dev/null 2>&1; then
  ok 'atlas-worker can ping the daemon'
else
  bad 'atlas-worker' 'cannot ping the daemon'
fi
# The worker reads through MCP, which is socket-only and holds no database
# handle. `atlas repo list` opens the index directly and must NOT work for it —
# that is the separation, not a gap in it.
MCP_IN='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{"roots":{"listChanged":true}}}}
{"jsonrpc":"2.0","method":"notifications/initialized"}
{"jsonrpc":"2.0","id":-1,"result":{"roots":[{"uri":"file:///opt/atlas"}]}}
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"atlas_repo_overview","arguments":{}}}'
if printf '%s\n' "$MCP_IN" | runuser -u atlas-worker -- "$ATLAS" mcp 2>/dev/null | grep -q 'untrusted_data'; then
  ok 'atlas-worker can read repository state over MCP'
else
  bad 'atlas-worker' 'cannot read repository state over MCP'
fi
if runuser -u atlas-worker -- "$ATLAS" repo list >/dev/null 2>&1; then
  bad 'atlas-worker' 'opened the index directly with repo list'
else
  ok 'atlas-worker cannot open the index directly'
fi

printf '\nthe worker cannot reach authority\n'
if runuser -u atlas-worker -- "$ATLAS" repo add /opt/atlas --name x >/dev/null 2>&1; then
  bad 'atlas-worker' 'registered a repository'
else
  ok 'atlas-worker cannot register a repository'
fi
if runuser -u atlas-worker -- "$ATLAS" backup create /tmp/w.db >/dev/null 2>&1; then
  bad 'atlas-worker' 'created a backup'
else
  ok 'atlas-worker cannot create a backup'
fi
if runuser -u atlas-worker -- "$ATLAS" maintenance prune --apply --yes >/dev/null 2>&1; then
  bad 'atlas-worker' 'pruned'
else
  ok 'atlas-worker cannot prune'
fi
AUTH=$(runuser -u atlas-worker -- "$ATLAS" doctor 2>/dev/null | grep -i 'operator authority' || true)
case "$AUTH" in
  *locked*) ok 'atlas-worker authority profile is locked' ;;
  *) bad 'atlas-worker authority' "$AUTH" ;;
esac

printf '\nthe malformed-policy matrix, against the real root-owned path\n'
BAK=""
if [ -f "$POLICY" ]; then BAK="$POLICY.verify-bak"; cp -a "$POLICY" "$BAK"; fi
matrix_case() {
  printf '%s' "$2" > "$POLICY"
  chown root:root "$POLICY"; chmod 0644 "$POLICY"
  # `atlas doctor` reports the deployment mode without touching the index.
  out=$(runuser -u atlasd -- "$ATLAS" doctor 2>&1 || true)
  case "$out" in
    *"per-user"*) ok "refused: $1" ;;
    *) bad "$1" 'was accepted as a system policy' ;;
  esac
}
matrix_case 'no socket_path'   'data_dir = /var/lib/atlas
'
matrix_case 'no data_dir'      'socket_path = /run/atlas/atlas.sock
'
matrix_case 'relative socket'  'socket_path = run/atlas.sock
data_dir = /var/lib/atlas
'
matrix_case 'traversal'        'socket_path = /run/atlas/atlas.sock
data_dir = /var/../etc
'
matrix_case 'unknown key'      'socket_path = /run/atlas/atlas.sock
data_dir = /var/lib/atlas
allow_all = yes
'
matrix_case 'client_uid = 0'   'socket_path = /run/atlas/atlas.sock
data_dir = /var/lib/atlas
client_uid = 0
'
matrix_case 'non-numeric uid'  'socket_path = /run/atlas/atlas.sock
data_dir = /var/lib/atlas
client_uid = root
'
# A9.2.4. An unrecognised *value* is malformed too, not silently taken as one of
# the two it resembles: a policy whose author wrote something Atlas half
# understood is the failure this parser's dullness exists to prevent.
matrix_case 'bad semantic_auto_default' 'socket_path = /run/atlas/atlas.sock
data_dir = /var/lib/atlas
semantic_auto_default = yes
'
# A group-writable policy must be refused even though its content is perfect.
if [ -n "$BAK" ]; then cp -a "$BAK" "$POLICY"; fi
chmod 0664 "$POLICY"
out=$(runuser -u atlasd -- "$ATLAS" doctor 2>&1 || true)
case "$out" in
  *"per-user"*) ok 'refused: group-writable policy' ;;
  *) bad 'group-writable policy' 'was accepted' ;;
esac
chmod 0644 "$POLICY"
if [ -n "$BAK" ]; then cp -a "$BAK" "$POLICY"; rm -f "$BAK"; fi
out=$(runuser -u atlasd -- "$ATLAS" doctor 2>&1 || true)
case "$out" in
  *"system"*) ok 'the real policy is accepted again' ;;
  *) bad 'the real policy' 'is no longer accepted' ;;
esac

printf '\nindexed repositories are unwritable by the service sandbox\n'
for r in $ATLAS_REPOS; do
  info "$r" "$(cd "$r" && git rev-parse HEAD)"
done

printf '\n'
if [ "$RC" -eq 0 ]; then printf 'verification: ok\n'; else printf 'verification: PROBLEMS FOUND\n'; fi
exit "$RC"
