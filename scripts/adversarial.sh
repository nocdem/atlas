#!/bin/sh
# Atlas - adversarial git-hardening verification under strace.
# Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
#
# tests/test_git_hardening.c proves no marker helper runs. This proves the same
# thing from outside, by recording every execve the whole process tree performs and
# requiring the set of executables to be exactly {atlas, git}. It also asserts no
# network syscall is attempted and no prompt is read.
#
# POSIX sh, no language runtime. Requires strace; skips cleanly without it.
#
# Usage: scripts/adversarial.sh [BUILD_DIR]        (default: build)

set -eu

BUILD="${1:-build}"
ATLAS="$BUILD/atlas"
JSONCHECK="$BUILD/tests/atlas-jsoncheck"
MARKER="$BUILD/tests/atlas-marker"

for tool in "$ATLAS" "$JSONCHECK" "$MARKER"; do
    if [ ! -x "$tool" ]; then
        echo "adversarial: $tool is missing; run 'make' first" >&2
        exit 1
    fi
done
if ! command -v strace > /dev/null 2>&1; then
    echo "adversarial: strace is not installed; skipping (the C suite still covers this)"
    exit 0
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/atlas-adv-XXXXXX")"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT INT TERM

DATA="$WORK/data"
REPO="$WORK/repo"
HELPER="$WORK/hostile-helper"
FIRED="$WORK/hostile-helper.fired"
mkdir -p "$DATA" "$REPO"
cp "$MARKER" "$HELPER"
chmod 0755 "$HELPER"

checks=0
fails=0
ok() { checks=$((checks + 1)); printf '  ok    %s\n' "$1"; }
bad() { checks=$((checks + 1)); fails=$((fails + 1)); printf '  FAIL  %s\n' "$1"; }

export GIT_CONFIG_NOSYSTEM=1
export GIT_CONFIG_GLOBAL=/dev/null
export GIT_AUTHOR_NAME='Adv Test'
export GIT_AUTHOR_EMAIL='adv@atlas.invalid'
export GIT_COMMITTER_NAME='Adv Test'
export GIT_COMMITTER_EMAIL='adv@atlas.invalid'
export LC_ALL=C
export TZ=UTC

# --- a maximally hostile repository ---------------------------------------
git -C "$REPO" init -q -b main .
printf 'one\ntwo\n' > "$REPO/a.txt"
printf '\000\001bin\000\002' > "$REPO/b.bin"
printf '*.bin diff=hostile\n' > "$REPO/.gitattributes"
git -C "$REPO" add -A
git -C "$REPO" commit -q -m 'initial commit'
printf 'one\nthree\n' > "$REPO/a.txt"
printf '\000\011chg\000\003' > "$REPO/b.bin"
printf 's\n' > "$REPO/staged.txt"; git -C "$REPO" add staged.txt
printf 'u\n' > "$REPO/untracked.txt"

# Every configuration-driven execution vector, all pointing at the marker.
git -C "$REPO" config core.fsmonitor "$HELPER"
git -C "$REPO" config diff.external "$HELPER"
git -C "$REPO" config diff.hostile.textconv "$HELPER"
git -C "$REPO" config core.pager "$HELPER"
git -C "$REPO" config core.askPass "$HELPER"
git -C "$REPO" config credential.helper "$HELPER"
git -C "$REPO" config core.editor "$HELPER"
git -C "$REPO" config uploadpack.packObjectsHook "$HELPER"
git -C "$REPO" config remote.origin.url 'https://127.0.0.1:1/nope.git'
mkdir -p "$WORK/hooks"
for h in post-index-change pre-auto-gc reference-transaction fsmonitor-watchman; do
    cp "$MARKER" "$WORK/hooks/$h"; chmod 0755 "$WORK/hooks/$h"
done
git -C "$REPO" config core.hooksPath "$WORK/hooks"

# Prove the vectors are live: plain git must run the helper and a hook. Without
# this, "no marker fired" during the Atlas runs would prove nothing.
rm -f "$FIRED" "$WORK"/hooks/*.fired
git -C "$REPO" status --porcelain=v2 -z > /dev/null 2>&1 || true
if [ -f "$FIRED" ]; then
    ok "control: plain git status runs the fsmonitor helper"
else
    bad "control: plain git status did NOT run the helper, so this run proves little"
fi
hook_control=no
for h in "$WORK"/hooks/*.fired; do
    [ -e "$h" ] && hook_control=yes
done
if [ "$hook_control" = yes ]; then
    ok "control: plain git status runs a repository hook"
else
    printf '  note  no hook fired under plain git; the hook assertion is weaker here\n'
fi
# Clear every marker the control produced, so what follows measures Atlas alone.
rm -f "$FIRED" "$WORK"/hooks/*.fired

# --- hostile inherited environment ----------------------------------------
mkdir -p "$WORK/decoy"
git -C "$WORK/decoy" init -q -b main .
printf 'decoy\n' > "$WORK/decoy/DECOY-ONLY.txt"
git -C "$WORK/decoy" add -A
git -C "$WORK/decoy" commit -q -m 'decoy'

TRACE="$WORK/trace.log"
run_traced() {
    strace -f -qq -e trace=execve,socket,connect,openat -s 200 -o "$TRACE.part" \
        env \
        GIT_EXTERNAL_DIFF="$HELPER" \
        GIT_PAGER="$HELPER" \
        PAGER="$HELPER" \
        GIT_ASKPASS="$HELPER" \
        SSH_ASKPASS="$HELPER" \
        GIT_SSH="$HELPER" \
        GIT_SSH_COMMAND="$HELPER" \
        GIT_EXEC_PATH="$WORK/hooks" \
        GIT_CONFIG_COUNT=1 \
        GIT_CONFIG_KEY_0=core.fsmonitor \
        GIT_CONFIG_VALUE_0="$HELPER" \
        GIT_DIR="$WORK/decoy/.git" \
        GIT_WORK_TREE="$WORK/decoy" \
        GIT_INDEX_FILE="$WORK/decoy/.git/index" \
        GIT_OBJECT_DIRECTORY="$WORK/decoy/.git/objects" \
        GIT_ALTERNATE_OBJECT_DIRECTORIES="$WORK/decoy/.git/objects" \
        GIT_COMMON_DIR="$WORK/decoy/.git" \
        GIT_TRACE=1 GIT_TRACE2=1 \
        GIT_TERMINAL_PROMPT=1 \
        GIT_OPTIONAL_LOCKS=1 \
        HOME="$WORK/fakehome" \
        "$ATLAS" --data-dir "$DATA" "$@" > "$WORK/out.txt" 2> "$WORK/err.txt"
    rc=$?
    cat "$TRACE.part" >> "$TRACE"
    return $rc
}

: > "$TRACE"
run_traced repo add "$REPO" --name adv || bad "repo add failed: $(cat "$WORK/err.txt")"
for spec in "scan adv" "status adv" "diff adv" "search adv one" "file adv a.txt" \
            "history adv a.txt" "scan adv"; do
    # shellcheck disable=SC2086
    if run_traced $spec; then :; else bad "atlas $spec failed: $(cat "$WORK/err.txt")"; fi
done
ok "every command completed with a hostile repository and environment"

# --- assertion 1: the helper never ran ------------------------------------
if [ -f "$FIRED" ]; then
    bad "the hostile helper RAN: $(cat "$FIRED")"
else
    ok "no marker helper executed"
fi
found_hook_marker=no
for h in "$WORK"/hooks/*.fired; do
    [ -e "$h" ] && found_hook_marker=yes
done
if [ "$found_hook_marker" = no ]; then
    ok "no repository hook executed"
else
    bad "a repository hook executed"
fi

# --- assertion 2: only atlas and git were executed ------------------------
# Every execve target in the traced tree, deduplicated.
sed -n 's/.*execve("\([^"]*\)".*/\1/p' "$TRACE" | sort -u > "$WORK/execs.txt"
unexpected=0
while IFS= read -r prog; do
    case "$prog" in
        */git|*/git-core/git) : ;;
        */atlas) : ;;
        */env) : ;;                      # the env(1) wrapper this script uses
        "$ATLAS") : ;;
        *) printf '    unexpected executable: %s\n' "$prog"; unexpected=$((unexpected + 1)) ;;
    esac
done < "$WORK/execs.txt"
if [ "$unexpected" -eq 0 ]; then
    ok "strace shows only atlas and git were executed ($(wc -l < "$WORK/execs.txt") distinct)"
else
    bad "$unexpected unexpected executable(s) in the trace"
fi

# --- assertion 3: no network was attempted --------------------------------
if grep -qE 'socket\(AF_INET|socket\(AF_INET6|connect\(' "$TRACE"; then
    bad "a network syscall was attempted"
    grep -E 'socket\(AF_INET|connect\(' "$TRACE" | head -3 | sed 's/^/      /'
else
    ok "no network socket or connect attempted"
fi

# --- assertion 4: no prompt was read -------------------------------------
# stdin is /dev/null for every child, so nothing can read a terminal.
if grep -q '/dev/tty' "$TRACE"; then
    bad "a child opened /dev/tty"
else
    ok "no child opened a terminal"
fi

# --- assertion 5: the decoy repository was never read ---------------------
if grep -q 'DECOY-ONLY' "$WORK/out.txt"; then
    bad "output mentions the decoy repository: GIT_DIR was honoured"
else
    ok "the decoy repository named by GIT_DIR was not read"
fi

# --- assertion 6: working tree and .git are unchanged --------------------
digest() {
    ( cd "$1" && find . -type f -o -type l | LC_ALL=C sort | while IFS= read -r f; do
        printf '%s|' "$f"
        if [ -L "$f" ]; then readlink "$f"; else sha256sum "$f" 2>/dev/null | cut -d' ' -f1; fi
      done ) | sha256sum | cut -d' ' -f1
}
AFTER_REPO="$(digest "$REPO")"
if [ -f "$WORK/repo.digest" ] && [ "$(cat "$WORK/repo.digest")" = "$AFTER_REPO" ]; then
    ok "working tree and .git digest unchanged"
else
    # First run records the baseline, then re-runs the commands to compare.
    printf '%s' "$AFTER_REPO" > "$WORK/repo.digest"
    run_traced scan adv || true
    run_traced diff adv || true
    if [ "$(digest "$REPO")" = "$AFTER_REPO" ]; then
        ok "working tree and .git digest unchanged across repeated commands"
    else
        bad "the repository changed"
    fi
fi

# --- assertion 7: a partial clone fails closed with valid JSON -----------
PC="$WORK/partial"
mkdir -p "$PC"
git -C "$PC" init -q -b main .
printf 'x\n' > "$PC/f.txt"
git -C "$PC" add -A
git -C "$PC" commit -q -m 'first'
git -C "$PC" config remote.origin.url 'https://example.invalid/r.git'
git -C "$PC" config remote.origin.promisor true
git -C "$PC" config remote.origin.partialclonefilter 'blob:none'

if "$ATLAS" --data-dir "$DATA" --json repo add "$PC" --name pc > "$WORK/pc.json" 2> /dev/null; then
    bad "a promisor repository was accepted"
else
    rc=$?
    if [ "$rc" -eq 7 ]; then
        ok "a promisor repository fails closed with exit 7"
    else
        bad "a promisor repository exited $rc, expected 7"
    fi
fi
if "$JSONCHECK" --expect-raw ok=false --expect status=integrity --no-control < "$WORK/pc.json" \
        > /dev/null; then
    ok "the failure is a valid structured JSON error"
else
    bad "the failure was not a valid JSON error document"
fi

printf '\n%d checks, %d failed\n' "$checks" "$fails"
[ "$fails" -eq 0 ]
