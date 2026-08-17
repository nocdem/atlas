#!/bin/sh
# Atlas - A9.2.4 installed acceptance: generic discovery, activation, and
# daemon-owned convergence.
#
# Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
#
# This drives the **installed** binary against the **running** daemon over an
# isolated fixture repository, because that is the only configuration in which
# the season's claim is actually true or false. Every fixture in the suite proves
# things about functions; this proves the thing the season exists for:
#
#     an arbitrary registered repository discovers its build inputs, the daemon
#     maintains the semantic index without anybody asking, and a build input
#     appearing or disappearing converges on its own.
#
# **No manual rebuild.** The script never runs `code index`. If it passes, the
# daemon did all of it.
#
# It creates one fixture repository, registers it, and removes both at the end.
# It touches no other repository, and it never writes to /opt/dna or /opt/swapper.
#
# **Registration is the one privileged step, and that is A7's design rather than
# an inconvenience.** Changing the registry is a local operation under the
# data-directory write lock and Atlas exposes no RPC method for it, so that
# nothing reachable over the socket — including MCP and hooks — can decide what
# Atlas indexes. On a system deployment the index is `0700 atlasd`, so the
# ceremony is: stop the daemon, register as the service account, start it again.
# That is what `register` and `deregister` below do, and it is the only place
# this script uses `sudo`.
set -eu

ATLAS="${ATLAS:-/usr/local/bin/atlas}"
FIX="${FIX:-/opt/a924-acceptance}"
NAME="${NAME:-a924}"
# The daemon's discovery sweep interval, plus slack. Discovery is deliberately
# far slower than the freshness sweep: it walks a directory tree, which is the
# one expensive thing in this layer. Waiting is the test, not an inconvenience.
DISCOVER_WAIT="${DISCOVER_WAIT:-420}"
# Generous, because after a registration the daemon has to notice the repository,
# scan it, walk it and only then build — and it is sharing the machine.
BUILD_WAIT="${BUILD_WAIT:-420}"

pass=0
fail=0
ok()  { pass=$((pass+1)); printf '  ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf '  FAIL  %s: %s\n' "$1" "$2"; }

status_json() { "$ATLAS" code sem-status "$NAME" --json 2>&1; }

field() { # field <json> <key>  -> the raw value after "key":
  printf '%s' "$1" | sed -n 's/.*"'"$2"'":\([^,}]*\).*/\1/p' | head -1
}

# **Convergence is `freshness = CURRENT` with nothing left due — not
# `activity = CURRENT`.** They are different axes and conflating them is what
# A9.2.3 exists to prevent: a repository holding a source no compilation database
# names is legitimately coverage-INCOMPLETE for ever, and asserting `activity`
# would quietly demand full coverage before the daemon could be said to have
# finished. What "the daemon has finished" means is that the published generation
# describes the current source and there is nothing left it wants to do.
converged() { # converged <seconds> <label>
  deadline=$(( $(date +%s) + $1 )); label=$2; f=""; due=""; act=""
  while [ "$(date +%s)" -lt "$deadline" ]; do
    j=$(status_json)
    f=$(field "$j" freshness); due=$(field "$j" rebuild_due); act=$(field "$j" activity)
    case "$f" in
      *CURRENT*) if [ "$due" = "false" ]; then
                   ok "$label (freshness=$f activity=$act rebuild_due=$due)"; return 0
                 fi ;;
    esac
    sleep 5
  done
  bad "$label" "freshness stayed '$f' with rebuild_due='$due' (activity=$act)"
  return 1
}

# Waits until `code sem-status` reports <key> = <value>, or the deadline passes.
wait_for() { # wait_for <key> <value> <seconds> <label>
  key=$1; want=$2; deadline=$(( $(date +%s) + $3 )); label=$4
  while [ "$(date +%s)" -lt "$deadline" ]; do
    got=$(field "$(status_json)" "$key")
    case "$got" in
      *"$want"*) ok "$label ($key=$got)"; return 0 ;;
    esac
    sleep 5
  done
  bad "$label" "$key stayed $got, wanted $want"
  return 1
}

register() {
  sudo systemctl stop atlas.service
  sudo -u atlasd "$ATLAS" repo add "$FIX" --name "$NAME" >/dev/null
  sudo systemctl start atlas.service
  sleep 3
}

deregister() {
  sudo systemctl stop atlas.service 2>/dev/null || true
  sudo -u atlasd "$ATLAS" repo remove "$NAME" --yes >/dev/null 2>&1 || true
  sudo systemctl start atlas.service 2>/dev/null || true
}

cleanup() {
  deregister
  # The contents, not the directory: `/opt` is root-owned, so the fixture
  # directory is created once by an operator and reused. Nothing outside it is
  # touched, and no registered repository is.
  rm -rf "$FIX"/* "$FIX"/.git "$FIX"/.gitignore 2>/dev/null || true
}
trap cleanup EXIT

printf '== A9.2.4 installed acceptance\n'
printf 'binary   %s\n' "$ATLAS"
"$ATLAS" --version

# --- the fixture ------------------------------------------------------------
#
# `$FIX` must already exist and be writable by the operator: `/opt` is root-owned
# and this script does not use `sudo` for anything. Create it once with
#   sudo mkdir -p /opt/a924-acceptance && sudo chown $(id -un):$(id -gn) /opt/a924-acceptance
if [ ! -w "$FIX" ]; then
  printf 'the fixture directory %s does not exist or is not writable\n' "$FIX" >&2
  exit 2
fi
rm -rf "$FIX"/* "$FIX"/.git "$FIX"/.gitignore 2>/dev/null || true
mkdir -p "$FIX/src" "$FIX/build/a" "$FIX/build/b"
cat > "$FIX/src/alpha.c" <<'EOF'
int alpha(void) { return 1; }
EOF
cat > "$FIX/src/beta.c" <<'EOF'
int beta(void) { return 2; }
EOF
cat > "$FIX/src/gamma.c" <<'EOF'
int gamma_fn(void) { return 3; }
EOF
compdb() { # compdb <path> <source>
  cat > "$1" <<EOF
[{"directory":"$FIX","arguments":["cc","-std=gnu11","-c","$2"],"file":"$2"}]
EOF
}
compdb "$FIX/build/a/compile_commands.json" "src/alpha.c"
compdb "$FIX/build/b/compile_commands.json" "src/beta.c"

git -C "$FIX" init -q
git -C "$FIX" config user.email a924@example.invalid
git -C "$FIX" config user.name a924
printf 'build/\n' > "$FIX/.gitignore"
git -C "$FIX" add -A
git -C "$FIX" -c commit.gpgsign=false commit -q -m 'a924 fixture'

# --- §37: registration, discovery, automatic build --------------------------
printf '\n-- registration and automatic convergence (A + B)\n'
register
ok 'registered (daemon stopped, registered as the service account, started)'

wait_for discovery COMPLETE "$DISCOVER_WAIT" 'discovery reached COMPLETE without being asked' || true
J=$(status_json)
case "$(field "$J" inputs_accepted)" in
  2) ok 'two build inputs discovered' ;;
  *) bad 'discovered input count' "$(field "$J" inputs_accepted)" ;;
esac
case "$(field "$J" auto_intent)" in
  *UNSET*) ok 'no operator intent was invented' ;;
  *) bad 'auto_intent' "$(field "$J" auto_intent)" ;;
esac

converged "$BUILD_WAIT" 'the daemon built the index with no manual rebuild' || true

# --- §24: a third build description appears ---------------------------------
printf '\n-- a new build input appears (C), with no rebuild command\n'
mkdir -p "$FIX/build/c"
compdb "$FIX/build/c/compile_commands.json" "src/gamma.c"

wait_for inputs_accepted 3 "$DISCOVER_WAIT" 'the new build input was discovered' || true
converged "$BUILD_WAIT" 'the daemon converged again on its own' || true
J=$(status_json)
case "$J" in
  *'"input_count":3'*) ok 'the published generation covers all three' ;;
  *) bad 'generation input_count' "$(field "$J" input_count)" ;;
esac

# --- §24: one goes away ------------------------------------------------------
printf '\n-- a build input is removed (B)\n'
rm -rf "$FIX/build/b"
wait_for inputs_accepted 2 "$DISCOVER_WAIT" 'the removal was discovered' || true
converged "$BUILD_WAIT" 'the daemon converged after a removal' || true

# --- §38: explicit operator disable -----------------------------------------
printf '\n-- explicit operator disable is respected, and lifting it converges\n'
"$ATLAS" code sem-config "$NAME" --no-auto >/dev/null
J=$(status_json)
case "$(field "$J" activity)" in
  *EXPLICITLY_DISABLED*) ok 'an explicit disable is reported as its own state' ;;
  *) bad 'activity after --no-auto' "$(field "$J" activity)" ;;
esac
case "$(field "$J" auto_intent_by)" in
  *OPERATOR*) ok 'the refusal records who made it' ;;
  *) bad 'auto_intent_by' "$(field "$J" auto_intent_by)" ;;
esac

# An edit to a source the databases already name, not a new file: adding one
# would leave the fixture with a source no compilation database compiles, which
# is a legitimate state and a different one from the one under test here.
printf 'int alpha(void) { return 42; }\n' > "$FIX/src/alpha.c"
git -C "$FIX" add -A
git -C "$FIX" -c commit.gpgsign=false commit -q -m 'a924 change under an explicit disable'
sleep 45
J=$(status_json)
case "$(field "$J" activity)" in
  *EXPLICITLY_DISABLED*) ok 'a source change did not lift the operator refusal' ;;
  *) bad 'activity after a change under disable' "$(field "$J" activity)" ;;
esac

"$ATLAS" code sem-config "$NAME" --auto >/dev/null
converged "$BUILD_WAIT" 're-enabling converged with no manual rebuild' || true

printf '\n%d checks, %d failed\n' "$((pass+fail))" "$fail"
[ "$fail" -eq 0 ]
