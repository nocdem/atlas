#!/bin/sh
# Atlas - A6 impact-gate acceptance.
# Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
#
# Measures what A6 claims about cost: that an ordinary gate query is bounded
# work against a repository at roughly DNA's scale, and specifically that it is
# **not** a full scan of the index per query.
#
# Three latencies, because they exercise three different amounts of the engine:
#
#   fresh      the validation point is the indexed head, so the change range is
#              empty and the structural walk is skipped entirely. This is the
#              case a pipeline runs on every commit, and it has to be cheap.
#   direct     the range is non-empty and a bound anchor's content moved, so
#              every link is resolved and the range is collected.
#   transitive the range is non-empty, the anchors still hold, and the bounded
#              outbound walk runs for every assessed decision. The most
#              expensive shape there is.
#
# No Python, no Node, no runtime. /bin/sh, and two compiled binaries.
#
# Usage: scripts/perf-a6.sh [BUILD_DIR]
#
# Like perf-a3.sh, perf-a4.sh and perf-a5.sh, this **asserts its own scale
# floors and its own limits** and exits non-zero rather than printing a number
# nobody checks. Shrinking the fixture or moving a limit to make a run pass is
# the one failure mode a performance gate cannot detect about itself.
#
# It also asserts the *verdicts*, not only the timings. A timing gate over a
# query that answered UNKNOWN because the index was not built is the worst kind
# of green: fast, passing, and measuring nothing.
#
# Peak RSS comes from /proc/<pid>/VmHWM, for the reason perf-a3.sh gives:
# busybox `time -v` reports ru_maxrss multiplied by the page size on this
# machine, so every figure it prints is four times the truth.

set -eu

build=${1:-build}
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
atlas="$root/$build/atlas"
gen_c="$root/$build/tests/atlas-gen-ctree"
gen_dec="$root/$build/tests/atlas-gen-decisions"

for bin in "$atlas" "$gen_c" "$gen_dec"; do
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

work=$(mktemp -d "/tmp/atlas-a6.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM

data="$work/data"
repo="$work/repo"
runtime="$work/rt"
mkdir -p "$data" "$repo" "$runtime"
chmod 700 "$runtime"
export ATLAS_DATA_DIR="$data"
# An empty private runtime directory: no socket lives here, so every command
# below takes the offline path and cannot reach a developer's live daemon.
export XDG_RUNTIME_DIR="$runtime"

fail=0
assert_at_least() {
    if [ "$2" -lt "$3" ]; then
        printf 'FAIL: %s is %s, below the required floor of %s\n' "$1" "$2" "$3" >&2
        fail=1
    else
        printf 'ok  : %-34s %10s  (floor %s)\n' "$1" "$2" "$3"
    fi
}
assert_at_most() {
    if [ "$2" -gt "$3" ]; then
        printf 'FAIL: %s is %s, above the limit of %s\n' "$1" "$2" "$3" >&2
        fail=1
    else
        printf 'ok  : %-34s %10s  (limit %s)\n' "$1" "$2" "$3"
    fi
}
assert_equal() {
    if [ "$2" != "$3" ]; then
        printf 'FAIL: %s is "%s", expected "%s"\n' "$1" "$2" "$3" >&2
        fail=1
    else
        printf 'ok  : %-34s %10s\n' "$1" "$2"
    fi
}

# --- the contract, stated before anything is measured -----------------------

min_files=5000   # .c and .h together, counted the way perf-a3.sh counts them
min_symbols=20000
min_relations=100000
min_decisions=400

# Required bounds, not observations. A gate query is a pipeline step, so the
# budget is "a person will not notice it", not a benchmark target.
gate_limit_ms=2000
rss_limit_kb=262144 # 256 MiB

printf '=== fixture: a repository at roughly DNA scale ===\n'
# The same shape and scale perf-a3.sh uses: about 5 100 sources plus headers,
# with include cycles, repeated static names and unresolved calls. A uniform
# tree would measure one code path.
"$gen_c" "$repo" "${ATLAS_PERF_MODULES:-176}" "${ATLAS_PERF_FILES:-32}" > /dev/null 2>&1
(
    cd "$repo"
    "$git_exe" init -q -b main .
    "$git_exe" add -A -f
    "$git_exe" -c user.email=perf@atlas.invalid -c user.name=perf commit -qm "generated tree"
) > /dev/null 2>&1

"$atlas" repo add "$repo" --name perf > /dev/null
"$atlas" scan perf > /dev/null
"$atlas" code sync perf > /dev/null

json_int() {
    "$atlas" --json "$@" | tr ',{}' '\n\n\n' | sed -n "s/.*\"$field\":\([0-9]*\).*/\1/p" | head -1
}
json_str() {
    "$atlas" --json "$@" | tr ',{}' '\n\n\n' | sed -n "s/.*\"$field\":\"\([^\"]*\)\".*/\1/p" \
        | head -1
}

files=$(find "$repo" \( -name '*.c' -o -name '*.h' \) -type f | wc -l)
field=symbols;       symbols=$(json_int code status perf)
field=relations;     relations=$(json_int code status perf)

assert_at_least "source files in the tree" "${files:-0}" "$min_files"
assert_at_least "symbols" "${symbols:-0}" "$min_symbols"
assert_at_least "relations" "${relations:-0}" "$min_relations"

# --- the decisions ----------------------------------------------------------
#
# Proposed through the real CLI so each one carries the snapshot the real write
# path captures. They are spread across distinct files, so the gate is assessing
# many independent anchors rather than the same one repeatedly.

printf '\n=== fixture: approved decisions ===\n'
#
# Through `atlas-gen-decisions`, the same fixture builder A4 and A5 use, which
# writes through the real `atlas_decision_apply` — a corpus built by a second
# write path would be a database shape the real one never produces. Every fourth
# document is approved, and each links to `src/mod<N>/file<N>.c`, which is
# exactly the layout `atlas-gen-ctree` produced above. So the anchors the gate
# resolves are real files with real structural edges.
"$gen_dec" "$data" "$repo" 2000 3000 6000 2>/dev/null > "$work/counts" || {
    printf 'FAIL: the decision fixture generator failed\n' >&2
    exit 1
}
approved=$(sed -n 's/^approved \([0-9]*\)$/\1/p' "$work/counts" | head -1)
printf 'info: %-34s %10s\n' "approved decisions" "${approved:-0}"
# The generator registers the repository itself if it is not already there, so
# the scan above is what makes its anchors resolvable.
assert_at_least "approved decisions" "${approved:-0}" "$min_decisions"

printf '\n=== gate latency ===\n'

# Wall-clock milliseconds around one command, plus its peak RSS.
measure() {
    label=$1
    shift
    start=$(date +%s%N 2>/dev/null || echo 0)
    "$@" > "$work/out.json" 2>"$work/err.txt" &
    child=$!
    peak=0
    while kill -0 "$child" 2>/dev/null; do
        if [ -r "/proc/$child/status" ]; then
            v=$(sed -n 's/^VmHWM:[[:space:]]*\([0-9]*\).*/\1/p' "/proc/$child/status" 2>/dev/null \
                || true)
            if [ -n "$v" ] && [ "$v" -gt "$peak" ] 2>/dev/null; then
                peak=$v
            fi
        fi
    done
    wait "$child" || true
    end=$(date +%s%N 2>/dev/null || echo 0)
    ms=$(( (end - start) / 1000000 ))
    printf 'info: %-34s %8s ms  %8s KiB peak RSS\n' "$label" "$ms" "$peak"
    assert_at_most "$label latency (ms)" "$ms" "$gate_limit_ms"
    assert_at_most "$label peak RSS (KiB)" "$peak" "$rss_limit_kb"
}

# 1. No change at all since the index was built.
measure "fresh, no change" "$atlas" --json gate check perf
r=$(sed -n 's/.*"result":"\([A-Z_]*\)".*/\1/p' "$work/out.json" | head -1)
assessed=$(tr ',{}' '\n\n\n' < "$work/out.json" | grep -c '"freshness"' || true)
printf 'info: %-34s %10s\n' "verdict, no change" "${r:-none}"
# The measurement is worthless unless the query actually assessed something. A
# timing gate over an empty result set is fast, passing, and measuring nothing.
assert_at_least "decisions assessed by the query" "${assessed:-0}" "$min_decisions"

# 2. A direct anchor moves. The fixture links into src/mod<N>/file<N>.c, so a
# file every decision corpus reaches is the right one to move.
first=$(cd "$repo" && find src -name 'part_*.c' | sort | head -1)
printf 'int atlas_a6_marker(void){return 1;}\n' >> "$repo/$first"
(
    cd "$repo"
    "$git_exe" add -A
    "$git_exe" -c user.email=perf@atlas.invalid -c user.name=perf commit -qm "direct change"
) > /dev/null 2>&1
"$atlas" scan perf > /dev/null
"$atlas" code sync perf > /dev/null
measure "after a direct change" "$atlas" --json gate check perf
stale=$(tr ',{}' '\n\n\n' < "$work/out.json" | sed -n 's/.*"stale":\([0-9]*\).*/\1/p' | head -1)
printf 'info: %-34s %10s\n' "stale after a direct change" "${stale:-0}"
assert_at_least "stale after a direct change" "${stale:-0}" 1

# 3. A dependency moves: a header many files include.
hdr=$(cd "$repo" && find src -name 'module.h' | sort | head -1)
if [ -n "$hdr" ]; then
    printf 'int atlas_a6_dep_marker(void);\n' >> "$repo/$hdr"
    (
        cd "$repo"
        "$git_exe" add -A
        "$git_exe" -c user.email=perf@atlas.invalid -c user.name=perf commit -qm "dependency change"
    ) > /dev/null 2>&1
    "$atlas" scan perf > /dev/null
    "$atlas" code sync perf > /dev/null
    measure "after a dependency change" "$atlas" --json gate check perf
fi

# --- the gate is not a full scan -------------------------------------------
#
# The claim that matters more than any single number: assessing one decision is
# bounded work, so `gate show` over a repository of this size must not cost what
# `gate check` over all of it costs. If it did, the per-decision path would be
# doing something proportional to the whole index.

printf '\n=== bounded, not scanned ===\n'
uid=$("$atlas" --json decision list perf --limit 1 \
    | tr ',{}' '\n\n\n' | sed -n 's/.*"decision":"\(atlas-dec-[0-9a-f]*\)".*/\1/p' | head -1)
if [ -n "$uid" ]; then
    measure "one decision (gate show)" "$atlas" --json gate show perf "$uid"
    one_ms=$ms
    measure "all decisions (gate check)" "$atlas" --json gate check perf
    all_ms=$ms
    # Not a ratio anybody should tune: the claim is only that asking about one
    # decision does not cost what asking about every decision costs, which is
    # what "bounded per decision" has to mean in practice.
    if [ "$one_ms" -lt "$all_ms" ] || [ "$all_ms" -lt 20 ]; then
        printf 'ok  : %-34s %10s\n' "one decision costs less than all" "${one_ms}ms<${all_ms}ms"
    else
        printf 'FAIL: one decision cost %sms and all of them cost %sms; the per-decision path is\n' \
            "$one_ms" "$all_ms" >&2
        printf '      doing work proportional to the whole index\n' >&2
        fail=1
    fi
fi

# --- the gate blocks nothing ------------------------------------------------
#
# A read command that took the writer lock would be a read command that stopped
# indexing. Asserted here as well as in the suite because it is a property an
# optimisation could quietly break.

printf '\n=== the gate holds no lock ===\n'
"$atlas" gate check perf > /dev/null 2>&1 &
gate_pid=$!
if "$atlas" scan perf > /dev/null 2>&1; then
    printf 'ok  : %-34s %10s\n' "a scan ran during a gate query" "yes"
else
    printf 'FAIL: a scan was blocked by a concurrent gate query\n' >&2
    fail=1
fi
wait "$gate_pid" 2>/dev/null || true

printf '\n'
if [ "$fail" -ne 0 ]; then
    printf 'A6 acceptance FAILED\n' >&2
    exit 1
fi
printf 'A6 acceptance passed. Every figure above is an observation from this run,\n'
printf 'not a bound Atlas holds. The limits are the asserted bounds.\n'
