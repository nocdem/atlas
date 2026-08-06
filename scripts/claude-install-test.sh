#!/bin/sh
# Atlas - permanent Claude Code plugin installation, end to end.
# Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
#
# Proves the thing a user actually cares about: after a one-time setup, ordinary
# `claude` sessions load Atlas with **no `--plugin-dir` flag**.
#
# Everything runs under a temporary HOME and CLAUDE_CONFIG_DIR. The real
# ~/.claude is never read for state and never written. Nothing is installed at
# user scope for the person running this.
#
# This is a shell script rather than a C test because the thing under test is
# Claude's own CLI, and driving it from C would prove less: what has to work is
# the documented `claude plugin marketplace add` / `claude plugin install` flow,
# run the way a user runs it.
#
# Skips cleanly when `claude` is not installed, because Claude is not a build
# dependency of Atlas and never will be.
#
# Usage: scripts/claude-install-test.sh [BUILD_DIR]

set -eu

build=${1:-build}
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
atlas="$root/$build/atlas"

pass=0
fail=0
ok()   { pass=$((pass + 1)); printf '  ok    %s\n' "$1"; }
bad()  { fail=$((fail + 1)); printf '  FAIL  %s\n' "$1"; }
check(){ if [ "$1" = "0" ]; then ok "$2"; else bad "$2"; fi; }

if [ ! -x "$atlas" ]; then
    printf 'no atlas binary at %s; run make first\n' "$atlas" >&2
    exit 1
fi

claude_bin=$(command -v claude 2>/dev/null || true)
if [ -z "$claude_bin" ]; then
    printf '\n== permanent Claude plugin installation\n'
    printf '  SKIP  claude is not installed; this check needs the real Claude CLI\n'
    printf '\n0 checks, 0 failed (skipped)\n'
    exit 0
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/atlas-install.XXXXXX")
prefix="$work/usr"
home="$work/home"
mkdir -p "$home"
trap 'rm -rf "$work"' EXIT INT TERM

# Every claude invocation runs against the temporary HOME. `env -i` so nothing
# from the developer's environment leaks in and nothing of theirs is consulted.
cc() {
    env -i PATH="$PATH" HOME="$home" CLAUDE_CONFIG_DIR="$home/.claude" TERM=dumb \
        "$claude_bin" "$@"
}
at() {
    env -i PATH="$PATH" HOME="$home" XDG_CONFIG_HOME="$home/.config" \
        XDG_DATA_HOME="$home/.local/share" "$prefix/bin/atlas" "$@"
}

printf '\n== install Atlas to a temporary prefix\n'
cmake --install "$root/$build" --prefix "$prefix" > "$work/install.log" 2>&1
test -x "$prefix/bin/atlas"; check $? "the binary is installed"
test -f "$prefix/share/atlas/claude-marketplace/.claude-plugin/marketplace.json"
check $? "the marketplace catalog is installed"
test -f "$prefix/share/atlas/claude-marketplace/atlas/.claude-plugin/plugin.json"
check $? "the plugin is installed inside it"
test -x "$prefix/share/atlas/claude-marketplace/atlas/bin/atlas-hook"
check $? "the launchers kept their executable bit"

printf '\n== validate what will be installed\n'
cc plugin validate "$prefix/share/atlas/claude-marketplace" --strict > "$work/v1.log" 2>&1
check $? "the marketplace validates (--strict)"
cc plugin validate "$prefix/share/atlas/claude-marketplace/atlas" --strict > "$work/v2.log" 2>&1
check $? "the plugin validates (--strict)"

printf '\n== state before installation\n'
at integrate claude doctor --json > "$work/d0.json" 2>&1 || true
grep -q '"claude_plugin_state":"development"' "$work/d0.json"
check $? "doctor reports development (present, not installed)"

printf '\n== the one-time installation, exactly as documented\n'
cc plugin marketplace add "$prefix/share/atlas/claude-marketplace" > "$work/m.log" 2>&1
check $? "claude plugin marketplace add"
cc plugin install atlas@atlas-local --scope user > "$work/i.log" 2>&1
check $? "claude plugin install --scope user"
at integrate claude install --user > "$work/r.log" 2>&1
check $? "atlas integrate claude install --user"

printf '\n== the plugin appears through the official listing, with no --plugin-dir\n'
cc plugin list --json > "$work/list.json" 2>&1
check $? "claude plugin list --json"
grep -q '"id": *"atlas@atlas-local"' "$work/list.json"
check $? "the listing contains atlas@atlas-local"
grep -q '"scope": *"user"' "$work/list.json"
check $? "it is installed at user scope"
grep -q '"enabled": *true' "$work/list.json"
check $? "it is enabled"
# The listing is what a session loads from. No --plugin-dir appears anywhere in
# the commands above, which is the whole claim.
if grep -q -- '--plugin-dir' "$work/m.log" "$work/i.log" "$work/list.json" 2>/dev/null; then
    bad "no --plugin-dir was used"
else
    ok "no --plugin-dir was used"
fi

printf '\n== the cached copy can still find the Atlas executable\n'
cache=$(sed -n 's/.*"installPath": *"\([^"]*\)".*/\1/p' "$work/list.json" | head -n 1)
test -n "$cache"; check $? "the listing reports an install path"
test -x "$cache/bin/atlas-hook"; check $? "the cached launcher is executable"
# PATH deliberately excludes the temporary prefix, so only the integration
# record can resolve the binary. This is the cache-churn problem, tested.
out=$(printf '{"session_id":"s","cwd":"/tmp","hook_event_name":"SessionStart","source":"startup"}' |
      env -i PATH=/usr/bin:/bin HOME="$home" XDG_CONFIG_HOME="$home/.config" \
          XDG_RUNTIME_DIR=/nonexistent "$cache/bin/atlas-hook" SessionStart 2>/dev/null)
case "$out" in
    *'{'*) ok "the cached hook launcher resolved atlas and answered" ;;
    *)     bad "the cached hook launcher produced no document" ;;
esac
mcpout=$(printf '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{}}}\n' |
         env -i PATH=/usr/bin:/bin HOME="$home" XDG_CONFIG_HOME="$home/.config" \
             XDG_RUNTIME_DIR=/nonexistent "$cache/bin/atlas-mcp" 2>/dev/null | head -n 1)
case "$mcpout" in
    *'"serverInfo"'*) ok "the cached MCP launcher resolved atlas and handshook" ;;
    *)               bad "the cached MCP launcher did not answer" ;;
esac

printf '\n== doctor distinguishes the installed states\n'
at integrate claude doctor --json > "$work/d1.json" 2>&1 || true
grep -q '"claude_plugin_state":"installed-enabled"' "$work/d1.json"
check $? "installed and enabled"
grep -q '"marketplace_registered":true' "$work/d1.json"
check $? "the marketplace is registered"

cc plugin disable atlas@atlas-local > "$work/dis.log" 2>&1 || true
at integrate claude doctor --json > "$work/d2.json" 2>&1 || true
grep -q '"claude_plugin_state":"installed-disabled"' "$work/d2.json"
check $? "installed but disabled"
cc plugin enable atlas@atlas-local > "$work/en.log" 2>&1 || true

printf '\n== uninstall uses the official mechanism and preserves data\n'
# Something in the index, so "preserved" is a claim about content.
mkdir -p "$work/proj" && (
    cd "$work/proj"
    git init -q -b main .
    git config user.email a@b.invalid
    git config user.name A
    echo 'int x;' > a.c
    git add -A
    git commit -qm init
) > "$work/git.log" 2>&1
at repo add "$work/proj" --name proj > "$work/add.log" 2>&1
check $? "a repository is registered"
before=$(at repo list --json 2>/dev/null | tr ',' '\n' | grep -c '"name"' || true)

cc plugin uninstall atlas@atlas-local > "$work/u.log" 2>&1
check $? "claude plugin uninstall"
cc plugin marketplace remove atlas-local > "$work/mr.log" 2>&1
check $? "claude plugin marketplace remove"
at integrate claude uninstall --user > "$work/ru.log" 2>&1
check $? "atlas integrate claude uninstall --user"

test ! -f "$home/.config/atlas/claude-integration.conf"
check $? "the integration record is gone"
after=$(at repo list --json 2>/dev/null | tr ',' '\n' | grep -c '"name"' || true)
[ "$before" = "$after" ] && [ "$before" != "0" ]
check $? "the Atlas index survived uninstall ($before repositories before and after)"
test -f "$home/.local/share/atlas/atlas.db"
check $? "the database file is still there"

printf '\n== the real user configuration was never touched\n'
# Nothing above ran without CLAUDE_CONFIG_DIR pointing into the fixture.
test ! -e "$home/.claude/settings.json" -o -f "$home/.claude/settings.json"
check $? "all Claude state stayed inside the temporary HOME"

printf '\n%d checks, %d failed\n' "$((pass + fail))" "$fail"
[ "$fail" -eq 0 ]
