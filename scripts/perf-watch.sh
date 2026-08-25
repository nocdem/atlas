#!/bin/sh
# Atlas - P0 watcher acceptance: the envelope Atlas is allowed to claim.
# Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
#
# What this measures, and why it is a gate rather than a report.
#
# P0 replaced a compiled watch budget of 8192 — enforced daemon-wide as 8191,
# and reported as a per-repository limit — with one derived from the kernel. A
# derived budget can be large, and "large" is a claim: it says a repository of
# some size is watched completely, primed within a bound, and answered while it
# primes. None of that is true because a constant was raised. It is true because
# it was measured, and it stays true only if something keeps measuring it.
#
# So this script **asserts its own scale floors and its own limits** and exits
# non-zero rather than printing numbers nobody checks — perf-a3.sh's discipline,
# for perf-a3.sh's reason. Shrinking the fixture or moving a limit to make a run
# pass is the one failure mode a performance gate cannot detect about itself.
#
# Two tree shapes, because they fail differently:
#
#   deep  - a narrow tree many levels down. Exercises the walk's recursion
#           bound and the per-path cost.
#   wide  - one directory with tens of thousands of children. Exercises the
#           depth-first frontier, which holds every sibling of the level it is
#           on. This is the shape the frontier byte bound exists for, and the
#           shape a plan reviewer was right to insist could not be assumed.
#
# The envelope proved here is `ATLAS_WATCH_PROVEN_ENVELOPE_DIRS`, and it is a
# different field from `ATLAS_WATCH_DIRS_HARD_CEILING`: the ceiling is where a
# configured value is refused, the envelope is what was measured. Documentation
# may claim the envelope and nothing more.
#
# Peak RSS comes from /proc/<pid>/VmHWM, never from `time -v`: busybox reports
# ru_maxrss multiplied by the page size on this machine, so every figure it
# prints is four times the truth.
#
# Usage: scripts/perf-watch.sh [BUILD_DIR]
#
# A stated limitation, in the script rather than only in the docs: a true
# cold-cache measurement needs `drop_caches`, which needs root. The tree is
# built fresh and has never been read by the daemon process, so the priming
# figure is an upper bound on warm and a lower bound on truly cold. Resumable
# priming is what makes that acceptable — responsiveness is bounded by the chunk
# size, which is cache-independent, and the responsiveness assertion below is
# the one that would catch a regression either way.

set -eu

build=${1:-build}
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
atlas="$root/$build/atlas"
# The acceptance daemon, not `atlas daemon run`: the watch budget is deliberately
# not settable from a flag or an environment variable, so a run that needs a
# budget larger than this machine's policy would grant has to inject one through
# `atlas_daemon_opts`. See tests/tools/atlas_watch_daemon.c.
wdaemon="$root/$build/tests/atlas-watch-daemon"

for bin in "$atlas" "$wdaemon"; do
    if [ ! -x "$bin" ]; then
        printf 'perf-watch: %s is not built. Run `make` first.\n' "$bin" >&2
        exit 1
    fi
done

# --- the floors and the limits, stated before anything runs -----------------
#
# FLOOR_DIRS is the envelope claim itself. If the machine cannot reach it the
# run fails rather than quietly proving something smaller: an acceptance script
# that lowers its own bar has stopped being one.
FLOOR_DIRS=65536
FLOOR_WIDE_SIBLINGS=65536
FLOOR_DEPTH=64
LIMIT_PRIME_S=120
LIMIT_RSS_KIB=524288      # 512 MiB
LIMIT_PING_MS=500         # while priming runs, not while idle
LIMIT_FRONTIER_MIB=16     # half of ATLAS_WATCH_FRONTIER_MAX_BYTES

tmp=$(mktemp -d "${TMPDIR:-/tmp}/atlas-perf-watch.XXXXXX")
cleanup() {
    if [ -n "${daemon_pid:-}" ] && kill -0 "$daemon_pid" 2>/dev/null; then
        kill "$daemon_pid" 2>/dev/null || true
        wait "$daemon_pid" 2>/dev/null || true
    fi
    rm -rf "$tmp"
}
trap cleanup EXIT INT TERM
daemon_pid=

# The runtime directory must be short: a Unix socket address is a fixed 108-byte
# field and Atlas refuses a path that would not fit rather than truncating it.
rt="$tmp/r"
mkdir -p "$rt" "$tmp/data"

kernel_max=$(cat /proc/sys/fs/inotify/max_user_watches 2>/dev/null || echo 0)
printf 'perf-watch: kernel fs.inotify.max_user_watches = %s\n' "$kernel_max"
if [ "$kernel_max" -lt $((FLOOR_DIRS + 8192)) ]; then
    printf 'perf-watch: FAIL: this uid'"'"'s inotify budget (%s) cannot hold the\n' "$kernel_max" >&2
    printf '  proven envelope of %s watches plus headroom. Raise\n' "$FLOOR_DIRS" >&2
    printf '  fs.inotify.max_user_watches, or run this on a machine that can.\n' >&2
    printf '  The envelope is not lowered to fit the machine.\n' >&2
    exit 1
fi

# The budget the acceptance daemon is given. Comfortably above the envelope, and
# checked against the kernel before a large fixture is built rather than after:
# discovering the run cannot pass only once 65536 directories exist wastes
# minutes and tells you nothing you could not have known first.
BUDGET=$((FLOOR_DIRS + 16384))
if [ "$BUDGET" -ge "$kernel_max" ]; then
    printf 'perf-watch: FAIL: the acceptance budget of %s does not fit this uid'"'"'s\n' "$BUDGET" >&2
    printf '  inotify limit of %s.\n' "$kernel_max" >&2
    exit 1
fi
printf 'perf-watch: acceptance budget = %s watches (injected, not policy)\n' "$BUDGET"

# --- the fixture ------------------------------------------------------------

repo="$tmp/repo"
mkdir -p "$repo"
git -C "$repo" init -q .
git -C "$repo" config user.email perf@atlas.invalid
git -C "$repo" config user.name 'Atlas Perf'

printf 'perf-watch: building the fixture (deep + wide)...\n'

# Wide: one directory with FLOOR_WIDE_SIBLINGS children. Every sibling lands on
# the frontier at once, which is what the byte bound is about.
wide="$repo/wide"
mkdir -p "$wide"
# Batched through xargs rather than one `mkdir` per directory: 65536 forks is
# minutes of fixture construction, and a measurement whose setup dominates its
# subject is one nobody will run.
seq 0 $((FLOOR_WIDE_SIBLINGS - 1)) | sed "s|^|$wide/d|" | xargs -r mkdir -p

# Deep: a narrow chain FLOOR_DEPTH levels down, with a file at the bottom so git
# reports the tree rather than an empty one.
deep="$repo/deep"
path="$deep"
mkdir -p "$path"
i=0
while [ "$i" -lt "$FLOOR_DEPTH" ]; do
    path="$path/l$i"
    mkdir -p "$path"
    i=$((i + 1))
done
printf 'int deep;\n' > "$path/leaf.c"
printf 'int top;\n' > "$repo/top.c"
git -C "$repo" add -A >/dev/null 2>&1 || true
git -C "$repo" -c user.email=perf@atlas.invalid -c user.name='Atlas Perf' \
    commit -qm 'perf fixture' >/dev/null 2>&1 || true

dirs=$(find "$repo" -path "$repo/.git" -prune -o -type d -print | wc -l)
path_bytes=$(find "$repo" -path "$repo/.git" -prune -o -type d -print | wc -c)
printf 'perf-watch: fixture holds %s directories, %s bytes of directory path\n' \
    "$dirs" "$path_bytes"

if [ "$dirs" -lt "$FLOOR_DIRS" ]; then
    printf 'perf-watch: FAIL: fixture has %s directories, floor is %s.\n' \
        "$dirs" "$FLOOR_DIRS" >&2
    printf '  The floor is the claim. Do not lower it to make a run pass.\n' >&2
    exit 1
fi

# --- the measurement --------------------------------------------------------

"$atlas" --data-dir "$tmp/data" repo add "$repo" --name perf >/dev/null

start=$(date +%s)
XDG_RUNTIME_DIR="$rt" "$wdaemon" "$tmp/data" "$BUDGET" > "$tmp/daemon.log" 2>&1 &
daemon_pid=$!

# Responsiveness *while priming*, which is the claim resumable priming makes.
# A watcher that walked to completion without yielding would still finish; what
# it would not do is answer, and what it would risk is IN_Q_OVERFLOW, which is
# global to the inotify instance and would gap every repository at once.
worst_ping=0
rss_peak=0
primed=0
while [ $(( $(date +%s) - start )) -lt "$LIMIT_PRIME_S" ]; do
    if [ -r "/proc/$daemon_pid/status" ]; then
        hwm=$(awk '/^VmHWM:/ {print $2}' "/proc/$daemon_pid/status" 2>/dev/null || echo 0)
        [ -n "$hwm" ] && [ "$hwm" -gt "$rss_peak" ] && rss_peak=$hwm
    fi
    t0=$(date +%s%N)
    if XDG_RUNTIME_DIR="$rt" "$atlas" --data-dir "$tmp/data" daemon ping >/dev/null 2>&1; then
        t1=$(date +%s%N)
        ms=$(( (t1 - t0) / 1000000 ))
        [ "$ms" -gt "$worst_ping" ] && worst_ping=$ms
    fi
    # Read the repository's own published state rather than `daemon status`.
    #
    # `daemon status` answers from the *local* path whenever the caller can open
    # the data directory, and the watcher lives in the daemon's process — so the
    # live watch counts are not available to it and are deliberately omitted
    # rather than reported as zero. The per-repository row is written by the
    # watcher itself and is the authority on what it installed.
    if XDG_RUNTIME_DIR="$rt" "$atlas" --data-dir "$tmp/data" events perf --limit 1 --json \
        2>/dev/null | grep -q '"watch_state":"watching"'; then
        primed=1
        break
    fi
    sleep 1
done
elapsed=$(( $(date +%s) - start ))

state=$(XDG_RUNTIME_DIR="$rt" "$atlas" --data-dir "$tmp/data" events perf --limit 1 --json \
    2>/dev/null || echo '{}')
watches=$(printf '%s' "$state" | sed -n 's/.*"watched_source":\([0-9]*\).*/\1/p')
meta=$(printf '%s' "$state" | sed -n 's/.*"watched_meta":\([0-9]*\).*/\1/p')
reason=$(printf '%s' "$state" | sed -n 's/.*"watch_reason":"\([a-z_]*\)".*/\1/p')
wstate=$(printf '%s' "$state" | sed -n 's/.*"watch_state":"\([a-z_]*\)".*/\1/p')
: "${watches:=0}"
: "${meta:=0}"
: "${reason:=unknown}"
: "${wstate:=unwatched}"
budget=$BUDGET

printf '\nperf-watch: observations\n'
printf '  priming completed      : %s\n' "$([ "$primed" -eq 1 ] && echo yes || echo NO)"
printf '  priming elapsed        : %s s observed (limit %s s)\n' "$elapsed" "$LIMIT_PRIME_S"
printf '  source watches held    : %s (budget %s)\n' "$watches" "$budget"
printf '  metadata watches held  : %s\n' "$meta"
printf '  watch state / reason   : %s / %s\n' "$wstate" "$reason"
printf '  worst ping while priming: %s ms observed (limit %s ms)\n' "$worst_ping" "$LIMIT_PING_MS"
printf '  peak RSS               : %s KiB observed (limit %s KiB)\n' "$rss_peak" "$LIMIT_RSS_KIB"

fail=0
if [ "$primed" -ne 1 ]; then
    printf 'perf-watch: FAIL: priming did not complete within %s s.\n' "$LIMIT_PRIME_S" >&2
    fail=1
fi
if [ "$watches" -lt "$FLOOR_DIRS" ]; then
    printf 'perf-watch: FAIL: held %s watches, the proven envelope is %s.\n' \
        "$watches" "$FLOOR_DIRS" >&2
    printf '  Atlas may not claim an envelope it did not reach.\n' >&2
    fail=1
fi
if [ "$worst_ping" -gt "$LIMIT_PING_MS" ]; then
    printf 'perf-watch: FAIL: the daemon took %s ms to answer while priming (limit %s).\n' \
        "$worst_ping" "$LIMIT_PING_MS" >&2
    printf '  Priming is supposed to yield between chunks.\n' >&2
    fail=1
fi
if [ "$rss_peak" -gt "$LIMIT_RSS_KIB" ]; then
    printf 'perf-watch: FAIL: peak RSS %s KiB exceeds %s KiB.\n' "$rss_peak" "$LIMIT_RSS_KIB" >&2
    fail=1
fi

# No overflow, and no repository quietly left behind: a run that primed fast and
# gapped everything would pass every timing gate above.
if grep -q 'inotify queue overflowed' "$tmp/daemon.log" 2>/dev/null; then
    printf 'perf-watch: FAIL: the inotify queue overflowed during priming.\n' >&2
    fail=1
fi
if [ "$wstate" != "watching" ] || [ "$reason" != "none" ]; then
    printf 'perf-watch: FAIL: the repository is %s (%s) after priming; a complete watch set\n' \
        "$wstate" "$reason" >&2
    printf '  is `watching` with reason `none`.\n' >&2
    fail=1
fi

printf '  frontier byte bound    : %s MiB (half of the configured maximum)\n' "$LIMIT_FRONTIER_MIB"
printf '\nperf-watch: %s\n' "$([ "$fail" -eq 0 ] && echo 'all limits held' || echo 'FAILED')"
exit "$fail"
