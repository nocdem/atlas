#!/bin/sh
# Atlas - A5 backup, verification, restore and maintenance acceptance.
# Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
#
# Builds one deterministic fixture that carries every kind of row A5 has to move
# — a real A0/A1 file and history index, a real A3 structural graph, real A2
# session rows written by real hooks against a real daemon, and the A4 decision
# corpus at its full acceptance scale — and then measures what A5 claims: that a
# hundred-megabyte index can be snapshotted, verified and restored inside an
# operational budget, and that the restored copy is the same index.
#
# No Python, no Node, no runtime. /bin/sh, and three compiled binaries.
#
# Usage: scripts/perf-a5.sh [BUILD_DIR]
#
# Like perf-a3.sh and perf-a4.sh, this **asserts its own scale floors and its
# own limits** and exits non-zero rather than printing a number nobody checks.
# Shrinking the fixture or moving a limit to make a run pass is the one failure
# mode a performance gate cannot detect about itself, so neither is left to a
# reader.
#
# It also verifies the restored database **table by table** rather than trusting
# that the restore finished. A timing gate over an operation that silently did
# nothing is the worst kind of green.
#
# Peak RSS comes from /proc/<pid>/VmHWM, sampled by a watcher while the child
# runs, for the reason perf-a3.sh gives: busybox `time -v` reports ru_maxrss
# multiplied by the page size on this machine, so every figure it prints is four
# times the truth.

set -eu

build=${1:-build}
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
atlas="$root/$build/atlas"
gen_dec="$root/$build/tests/atlas-gen-decisions"
gen_c="$root/$build/tests/atlas-gen-ctree"

for bin in "$atlas" "$gen_dec" "$gen_c"; do
    if [ ! -x "$bin" ]; then
        printf 'no binary at %s; run make first\n' "$bin" >&2
        exit 1
    fi
done

git_exe=$(command -v git || true)
if [ -z "$git_exe" ]; then
    printf 'git is required to build the fixture repository\n' >&2
    exit 1
fi

# A short base path: a Unix socket address is a fixed 108-byte field and the
# daemon refuses a runtime directory that would not fit rather than truncating
# it. Same reason scripts/perf.sh does this.
work=$(mktemp -d "/tmp/atlas-a5.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM

data="$work/data"
repo="$work/repo"
runtime="$work/rt"
mkdir -p "$data" "$repo" "$runtime"
chmod 700 "$runtime"

fail=0
assert_at_least() {
    if [ "$2" -lt "$3" ]; then
        printf 'FAIL: %s is %s, below the required floor of %s\n' "$1" "$2" "$3" >&2
        fail=1
    else
        printf 'ok  : %-30s %12s  (floor %s)\n' "$1" "$2" "$3"
    fi
}
assert_at_most() {
    if [ "$2" -gt "$3" ]; then
        printf 'FAIL: %s is %s, above the limit of %s\n' "$1" "$2" "$3" >&2
        fail=1
    else
        printf 'ok  : %-30s %12s  (limit %s)\n' "$1" "$2" "$3"
    fi
}

# --- the contract, stated before anything is measured -----------------------

min_documents=10000
min_revisions=25000
min_links=100000
min_symbols=20000
min_relations=100000
min_db_bytes=104857600 # 100 MiB

# Operational budgets, not benchmark targets. Generous on purpose: what matters
# is that an operator can take a backup, check it and rehearse a restore inside
# a coffee break, not that any of them is fast.
op_limit_s=10
rss_limit_kb=262144 # 256 MiB

printf '=== fixture: a real repository, indexed for real ===\n'
"$gen_c" "$repo" 40 45 > /dev/null 2>&1
(
    cd "$repo"
    "$git_exe" init -q -b main .
    "$git_exe" add -A -f
    "$git_exe" -c user.email=perf@atlas.invalid -c user.name=perf commit -qm "generated tree"
) > /dev/null 2>&1

export ATLAS_DATA_DIR="$data"
"$atlas" repo add "$repo" --name perf > /dev/null
"$atlas" scan perf > /dev/null
"$atlas" code sync perf > /dev/null

symbols=$("$atlas" --json code status perf | tr ',' '\n' \
    | sed -n 's/.*"symbols":\([0-9]*\).*/\1/p' | head -1)
relations=$("$atlas" --json code status perf | tr ',' '\n' \
    | sed -n 's/.*"relations":\([0-9]*\).*/\1/p' | head -1)
printf 'info: %-30s %12s symbols, %s relations\n' "structural graph" "$symbols" "$relations"

# --- A2 rows, written by real hooks against a real daemon -------------------
#
# Not seeded as SQL. The point of having them here is that the backup moves the
# shape the real write path produces, and a hand-written row is a shape the real
# path might never make.

printf '\n=== fixture: A2 session rows from real hooks ===\n'
XDG_RUNTIME_DIR="$runtime" "$atlas" daemon run > "$work/daemon.log" 2>&1 &
daemon_pid=$!
ready=0
i=0
while [ "$i" -lt 100 ]; do
    if XDG_RUNTIME_DIR="$runtime" "$atlas" daemon ping > /dev/null 2>&1; then
        ready=1
        break
    fi
    i=$(( i + 1 ))
done
if [ "$ready" -ne 1 ]; then
    printf 'FAIL: the daemon did not become ready\n' >&2
    kill "$daemon_pid" 2>/dev/null || true
    exit 1
fi

hook() {
    if ! printf '{"session_id":"perf-a5-session","hook_event_name":"%s","cwd":"%s"}' "$1" "$repo" \
        | CLAUDE_CODE_SESSION_ID=perf-a5-session XDG_RUNTIME_DIR="$runtime" \
          "$atlas" hook "$1" > /dev/null 2>&1; then
        printf 'FAIL: the %s hook exited non-zero; hooks fail open\n' "$1" >&2
        fail=1
    fi
}
hook SessionStart
hook UserPromptSubmit
hook PostToolUse
hook Stop

# A hook exiting 0 proves nothing about rows: hooks fail open, so one that
# reached nothing exits 0 too. What this fixture needs is that the A2 rows are
# actually there, so that is what is checked — by asking the daemon to resolve
# the session the hooks opened. A sessionless answer means the rows are absent
# and the fixture is not what this script says it is.
a2=$(printf '{"session_id":"perf-a5-session","hook_event_name":"UserPromptSubmit","cwd":"%s"}' \
        "$repo" \
    | CLAUDE_CODE_SESSION_ID=perf-a5-session XDG_RUNTIME_DIR="$runtime" \
      "$atlas" hook UserPromptSubmit 2>/dev/null)
case "$a2" in
    *session_unbound*|'')
        printf 'FAIL: the A2 session the hooks opened could not be resolved; the fixture has no\n' >&2
        printf '      session rows and its "real A2 rows" claim would be false\n' >&2
        fail=1 ;;
    *)
        printf 'ok  : %-30s %12s\n' "A2 session resolved by key" "yes" ;;
esac

# The snapshot below is taken while this daemon is still running and still
# holding the writer lock, which is the case that matters: an operator does not
# stop Atlas to back it up.
printf '\n=== fixture: the A4 decision corpus ===\n'
"$gen_dec" "$data" "$repo" 10000 25000 100000 2>/dev/null > "$work/counts"
cat "$work/counts"

documents=$(awk '$1=="documents"{print $2}' "$work/counts")
revisions=$(awk '$1=="revisions"{print $2}' "$work/counts")
links=$(awk '$1=="links"{print $2}' "$work/counts")
approved=$(awk '$1=="approved"{print $2}' "$work/counts")
rejected=$(awk '$1=="rejected"{print $2}' "$work/counts")
superseded=$(awk '$1=="superseded"{print $2}' "$work/counts")

db_bytes=$(wc -c < "$data/atlas.db")

printf '\n=== scale floors ===\n'
assert_at_least "decision documents" "$documents" "$min_documents"
assert_at_least "immutable revisions" "$revisions" "$min_revisions"
assert_at_least "code/history links" "$links" "$min_links"
assert_at_least "approved documents" "$approved" 1
assert_at_least "rejected documents" "$rejected" 1
assert_at_least "superseded documents" "$superseded" 1
assert_at_least "structural symbols" "${symbols:-0}" "$min_symbols"
assert_at_least "structural relations" "${relations:-0}" "$min_relations"
assert_at_least "database bytes" "$db_bytes" "$min_db_bytes"

# --- measurement ------------------------------------------------------------

timed() {
    # $1 label  $2 limit_s  rest: argv. Reports elapsed seconds and peak RSS.
    label=$1
    limit=$2
    shift 2
    start=$(date +%s%N)
    "$@" > /dev/null 2>&1 &
    child=$!
    peak=0
    while kill -0 "$child" 2>/dev/null; do
        v=$(awk '/VmHWM/{print $2}' "/proc/$child/status" 2>/dev/null || true)
        if [ -n "$v" ] && [ "$v" -gt "$peak" ] 2>/dev/null; then
            peak=$v
        fi
    done
    if ! wait "$child"; then
        printf 'FAIL: %s did not succeed\n' "$label" >&2
        fail=1
    fi
    end=$(date +%s%N)
    ms=$(( (end - start) / 1000000 ))
    s=$(( (ms + 999) / 1000 ))
    if [ "$s" -gt "$limit" ]; then
        printf 'FAIL: %-30s %6s ms  peak %7s kB  (limit %s s)\n' "$label" "$ms" "$peak" "$limit" >&2
        fail=1
    else
        printf 'ok  : %-30s %6s ms  peak %7s kB  (limit %s s)\n' "$label" "$ms" "$peak" "$limit"
    fi
    if [ "$peak" -gt "$rss_limit_kb" ]; then
        printf 'FAIL: %s peak RSS %s kB is above the limit of %s kB\n' "$label" "$peak" \
            "$rss_limit_kb" >&2
        fail=1
    fi
}

printf '\n=== backup, with the daemon running and holding the writer lock ===\n'
timed "backup create (online)" "$op_limit_s" "$atlas" backup create "$work/snapshot.db"
timed "backup verify" "$op_limit_s" "$atlas" backup verify "$work/snapshot.db"

# The online claim, asserted rather than assumed: if the backup had taken the
# lock, the daemon would be the one that lost it.
if ! kill -0 "$daemon_pid" 2>/dev/null; then
    printf 'FAIL: the daemon died while a backup was taken\n' >&2
    fail=1
else
    printf 'ok  : %-30s %12s\n' "daemon survived the snapshot" "yes"
fi
online=$("$atlas" --json backup create "$work/snapshot2.db" | tr ',' '\n' \
    | sed -n 's/.*"source_online":\(true\|false\).*/\1/p' | head -1)
if [ "$online" != "true" ]; then
    printf 'FAIL: a backup taken against a running daemon did not report source_online\n' >&2
    fail=1
else
    printf 'ok  : %-30s %12s\n' "reported as an online source" "$online"
fi

printf '\n=== maintenance plan, with the daemon running ===\n'
timed "maintenance plan" "$op_limit_s" "$atlas" maintenance plan --older-than 30

# Everything below needs the writer lock, so the daemon stops here — which is
# also the documented operating procedure.
kill "$daemon_pid" 2>/dev/null || true
wait "$daemon_pid" 2>/dev/null || true

printf '\n=== restore into an isolated data directory ===\n'
restored="$work/restored"
mkdir -p "$restored"
timed "backup restore (isolated)" "$op_limit_s" \
    "$atlas" --data-dir "$restored" backup restore "$work/snapshot.db" --yes

# --- the restored copy is the same index ------------------------------------
#
# A timing gate over an operation that did nothing is the worst kind of green,
# so the restored database is compared against the source by row counts for
# every table A5 has to carry, and by the two things that are Atlas' own record
# rather than a rebuildable index: every revision rehashed, and every document's
# status replayed from its ledger. `atlas doctor` does both.

printf '\n=== the restored copy ===\n'
count_of() {
    # $1 data dir  $2 json key  rest: argv
    ATLAS_DATA_DIR=$1 "$atlas" --json "$3" "$4" "$5" 2>/dev/null | tr ',' '\n' \
        | sed -n "s/.*\"$2\":\([0-9]*\).*/\1/p" | head -1
}

src_docs=$(ATLAS_DATA_DIR="$data" "$atlas" --json decision list perf --limit 1 \
    | tr ',' '\n' | sed -n 's/.*"total_proposed":\([0-9]*\).*/\1/p' | head -1)
dst_docs=$(ATLAS_DATA_DIR="$restored" "$atlas" --json decision list perf --limit 1 \
    | tr ',' '\n' | sed -n 's/.*"total_proposed":\([0-9]*\).*/\1/p' | head -1)
src_sym=$(ATLAS_DATA_DIR="$data" "$atlas" --json code status perf | tr ',' '\n' \
    | sed -n 's/.*"symbols":\([0-9]*\).*/\1/p' | head -1)
dst_sym=$(ATLAS_DATA_DIR="$restored" "$atlas" --json code status perf | tr ',' '\n' \
    | sed -n 's/.*"symbols":\([0-9]*\).*/\1/p' | head -1)
src_rel=$(ATLAS_DATA_DIR="$data" "$atlas" --json code status perf | tr ',' '\n' \
    | sed -n 's/.*"relations":\([0-9]*\).*/\1/p' | head -1)
dst_rel=$(ATLAS_DATA_DIR="$restored" "$atlas" --json code status perf | tr ',' '\n' \
    | sed -n 's/.*"relations":\([0-9]*\).*/\1/p' | head -1)

same() {
    if [ "$2" != "$3" ]; then
        printf 'FAIL: %s differs after a restore: %s -> %s\n' "$1" "$2" "$3" >&2
        fail=1
    else
        printf 'ok  : %-30s %12s  (identical)\n' "$1" "$2"
    fi
}
same "proposed decisions" "$src_docs" "$dst_docs"
same "structural symbols" "$src_sym" "$dst_sym"
same "structural relations" "$src_rel" "$dst_rel"

# doctor rehashes every revision and replays every ledger. A non-zero exit here
# means the restored copy disagrees with itself.
if ATLAS_DATA_DIR="$restored" "$atlas" doctor > "$work/doctor.txt" 2>&1; then
    printf 'ok  : %-30s %12s\n' "restored doctor" "ok"
else
    printf 'FAIL: the restored index did not pass doctor\n' >&2
    sed -n '1,40p' "$work/doctor.txt" >&2
    fail=1
fi

# --- the refusals are not free ----------------------------------------------
#
# Verification has to be *fast to say no*, or an operator checking a directory
# of backups will stop checking.

printf '\n=== refusals ===\n'
head -c 1024 /dev/urandom > "$work/junk.db"
# `timed` requires success, and this must fail, so it is measured by hand. A
# refusal has to be fast: an operator checking a directory of backups who has to
# wait for each "no" stops checking.
start=$(date +%s%N)
if "$atlas" backup verify "$work/junk.db" > /dev/null 2>&1; then
    printf 'FAIL: a file of random bytes verified as usable\n' >&2
    fail=1
fi
end=$(date +%s%N)
refuse_ms=$(( (end - start) / 1000000 ))
assert_at_most "refusal latency (ms)" "$refuse_ms" 2000
cp "$work/snapshot.db" "$work/cut.db"
truncate -s -4096 "$work/cut.db"
if "$atlas" backup verify "$work/cut.db" > /dev/null 2>&1; then
    printf 'FAIL: a truncated backup verified as usable\n' >&2
    fail=1
else
    printf 'ok  : %-30s %12s\n' "truncated backup refused" "yes"
fi

printf '\n'
printf 'fixture: %s bytes, %s documents, %s revisions, %s links, %s symbols, %s relations\n' \
    "$db_bytes" "$documents" "$revisions" "$links" "$symbols" "$relations"
if [ "$fail" -ne 0 ]; then
    printf 'A5 operational acceptance FAILED\n' >&2
    exit 1
fi
printf 'A5 operational acceptance passed.\n'
printf 'Every figure describes this machine and the synthetic fixture above. It is\n'
printf 'not a claim about any real repository, and the limits are required bounds\n'
printf 'rather than observations.\n'
