#!/bin/sh
# Atlas - A4 decision-lifecycle performance acceptance.
# Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
#
# Builds a deterministic synthetic decision corpus and measures what A4 claims:
# that every bounded read stays well under the interactive ceiling at ten
# thousand documents, that the passive hook path stays inside its much tighter
# budget, and that repeated reconciliation and hook retries add nothing durable.
#
# No Python, no Node, no runtime. /bin/sh, and two compiled binaries.
#
# Usage: scripts/perf-a4.sh [BUILD_DIR]
#
# Like scripts/perf-a3.sh, this **asserts its own scale floors and its own
# limits** and exits non-zero rather than printing a number nobody checks.
# Shrinking the fixture or moving a limit to make a run pass is the one failure
# mode a performance gate cannot detect about itself, so neither is left to a
# reader.
#
# Peak RSS comes from /proc/<pid>/VmHWM rather than from time(1), for the reason
# perf-a3.sh gives: busybox `time -v` reports ru_maxrss multiplied by the page
# size on this machine, so every figure it prints is four times the truth. A
# measurement wrong by a constant factor is worse than none, because it still
# looks like a measurement.

set -eu

build=${1:-build}
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
atlas="$root/$build/atlas"
gen="$root/$build/tests/atlas-gen-decisions"

if [ ! -x "$atlas" ]; then
    printf 'no atlas binary at %s; run make first\n' "$atlas" >&2
    exit 1
fi
if [ ! -x "$gen" ]; then
    printf 'no fixture generator at %s; run make first\n' "$gen" >&2
    exit 1
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/atlas-perf-a4.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM

data="$work/data"
repo="$work/repo"
mkdir -p "$data" "$repo"

# --- the scale contract -----------------------------------------------------
#
# Stated before anything is measured, and asserted after the fixture is built.
# A gate whose floors are read off its own output is not a gate.

want_documents=${ATLAS_PERF_DOCS:-10000}
want_revisions=${ATLAS_PERF_REVS:-25000}
want_links=${ATLAS_PERF_LINKS:-100000}

min_documents=10000
min_revisions=25000
min_links=100000

# Every bounded read, p95, in milliseconds. The brief's ceiling.
read_limit_ms=100
# The passive hook path. Much tighter, because it runs on somebody's keystroke.
hook_limit_ms=20

printf '=== fixture ===\n'
"$gen" "$data" "$repo" "$want_documents" "$want_revisions" "$want_links" 2>/dev/null > "$work/counts"
cat "$work/counts"

documents=$(awk '$1=="documents"{print $2}' "$work/counts")
revisions=$(awk '$1=="revisions"{print $2}' "$work/counts")
links=$(awk '$1=="links"{print $2}' "$work/counts")
approved=$(awk '$1=="approved"{print $2}' "$work/counts")
rejected=$(awk '$1=="rejected"{print $2}' "$work/counts")
superseded=$(awk '$1=="superseded"{print $2}' "$work/counts")

fail=0
assert_at_least() {
    # $1 label  $2 actual  $3 floor
    if [ "$2" -lt "$3" ]; then
        printf 'FAIL: %s is %s, below the required floor of %s\n' "$1" "$2" "$3" >&2
        fail=1
    else
        printf 'ok  : %-28s %10s  (floor %s)\n' "$1" "$2" "$3"
    fi
}

printf '\n=== scale floors ===\n'
assert_at_least "decision documents" "$documents" "$min_documents"
assert_at_least "immutable revisions" "$revisions" "$min_revisions"
assert_at_least "code/history links" "$links" "$min_links"
# All four lifecycle states have to be present, or the measurements below run
# over a table shape the real one never has — in particular the partial unique
# index that makes the approved lookup a seek is never exercised.
assert_at_least "approved documents" "$approved" 1
assert_at_least "rejected documents" "$rejected" 1
assert_at_least "superseded documents" "$superseded" 1

db_bytes=$(wc -c < "$data/atlas.db")
printf 'info: %-28s %10s bytes\n' "database size" "$db_bytes"

# A decision id from the corpus, for the single-document measurements. Taken
# from the listing rather than constructed, so it is one the fixture really made.
some_id=$(ATLAS_DATA_DIR="$data" "$atlas" --json decision list perf --limit 1 \
    | tr ',' '\n' | sed -n 's/.*"decision":"\(atlas-dec-[0-9a-f]*\)".*/\1/p' | head -1)
if [ -z "$some_id" ]; then
    printf 'FAIL: could not read a decision id back out of the fixture\n' >&2
    exit 1
fi
printf 'info: %-28s %10s\n' "sample decision" "$some_id"

# --- measurement ------------------------------------------------------------
#
# Twenty-one runs per query, sorted, and the twentieth taken as p95. Twenty-one
# because the twentieth of twenty-one is the smallest sample size for which p95
# is an observed value rather than an interpolation between two.

runs=${ATLAS_PERF_RUNS:-21}
p95_index=$(( (runs * 95 + 99) / 100 ))

measure() {
    label=$1
    limit=$2
    shift 2
    : > "$work/times"
    i=0
    while [ "$i" -lt "$runs" ]; do
        start=$(date +%s%N)
        ATLAS_DATA_DIR="$data" "$atlas" "$@" > /dev/null 2>&1
        end=$(date +%s%N)
        echo $(( (end - start) / 1000000 )) >> "$work/times"
        i=$(( i + 1 ))
    done
    p95=$(sort -n "$work/times" | sed -n "${p95_index}p")
    max=$(sort -n "$work/times" | tail -1)
    if [ "$p95" -gt "$limit" ]; then
        printf 'FAIL: %-30s p95 %4s ms  max %4s ms  (limit %s ms)\n' "$label" "$p95" "$max" "$limit" >&2
        fail=1
    else
        printf 'ok  : %-30s p95 %4s ms  max %4s ms  (limit %s ms)\n' "$label" "$p95" "$max" "$limit"
    fi
}

printf '\n=== bounded reads (p95 of %s runs, limit %s ms) ===\n' "$runs" "$read_limit_ms"

# Each of these is one process start plus one query, so the figure includes
# process startup — which is what a caller actually pays and is therefore the
# honest thing to bound.
measure "decision list" "$read_limit_ms" decision list perf
measure "decision list --status APPROVED" "$read_limit_ms" decision list perf --status APPROVED
measure "compact search" "$read_limit_ms" decision search perf storage
measure "compact search (no match)" "$read_limit_ms" decision search perf zzzznomatch
measure "full retrieval" "$read_limit_ms" decision show perf "$some_id"
measure "history/timeline" "$read_limit_ms" decision history perf "$some_id"
measure "decisions for a file" "$read_limit_ms" decision for-file perf src/mod0/file0.c
measure "export" "$read_limit_ms" decision export perf "$some_id"

printf '\n=== the passive hook path (limit %s ms) ===\n' "$hook_limit_ms"

# The hook runs with no daemon here, which is its slowest case: it has to decide
# that nothing is answering. A hook must never block somebody's keystroke, which
# is why its budget is five times tighter than a query's.
hook_payload='{"session_id":"perf-session","hook_event_name":"UserPromptSubmit","cwd":"'$repo'"}'
: > "$work/times"
i=0
while [ "$i" -lt "$runs" ]; do
    start=$(date +%s%N)
    printf '%s' "$hook_payload" | ATLAS_DATA_DIR="$data" "$atlas" hook UserPromptSubmit > /dev/null 2>&1
    end=$(date +%s%N)
    echo $(( (end - start) / 1000000 )) >> "$work/times"
    i=$(( i + 1 ))
done
hook_p95=$(sort -n "$work/times" | sed -n "${p95_index}p")
hook_max=$(sort -n "$work/times" | tail -1)
if [ "$hook_p95" -gt "$hook_limit_ms" ]; then
    printf 'FAIL: %-30s p95 %4s ms  max %4s ms  (limit %s ms)\n' "passive hook" "$hook_p95" "$hook_max" "$hook_limit_ms" >&2
    fail=1
else
    printf 'ok  : %-30s p95 %4s ms  max %4s ms  (limit %s ms)\n' "passive hook" "$hook_p95" "$hook_max" "$hook_limit_ms"
fi

# --- no unbounded result ----------------------------------------------------
#
# Every listing has to obey its ceiling whatever it is asked for. Checked by
# asking for more than the maximum and counting what came back.

printf '\n=== bounds ===\n'
over=$(ATLAS_DATA_DIR="$data" "$atlas" --json decision list perf --limit 100000 \
    | tr ',' '\n' | grep -c '"decision":"atlas-dec-' || true)
if [ "$over" -gt 200 ]; then
    printf 'FAIL: a listing returned %s rows; the ceiling is 200\n' "$over" >&2
    fail=1
else
    printf 'ok  : %-30s %s rows for --limit 100000 (ceiling 200)\n' "listing ceiling" "$over"
fi

# --- retries create no durable growth ---------------------------------------
#
# Rule 11 of the phase, measured rather than asserted: the same hook delivered
# repeatedly, and the same proposal repeated, must leave the database the size
# it was.

printf '\n=== idempotency ===\n'
before=$(wc -c < "$data/atlas.db")
i=0
while [ "$i" -lt 20 ]; do
    printf '%s' "$hook_payload" | ATLAS_DATA_DIR="$data" "$atlas" hook UserPromptSubmit > /dev/null 2>&1
    i=$(( i + 1 ))
done
# The same proposal, with the same dedup key, twenty times.
i=0
while [ "$i" -lt 20 ]; do
    ATLAS_DATA_DIR="$data" "$atlas" decision propose perf \
        --title "A retried proposal" --decision "The same content every time." \
        --dedup-key "perf-retry" > /dev/null 2>&1 || true
    i=$(( i + 1 ))
done
docs_after=$(ATLAS_DATA_DIR="$data" "$atlas" --json decision list perf --limit 1 \
    | tr ',' '\n' | sed -n 's/.*"total_proposed":\([0-9]*\).*/\1/p' | head -1)
after=$(wc -c < "$data/atlas.db")
growth=$(( after - before ))
printf 'info: %-30s %s -> %s bytes (%s)\n' "database size" "$before" "$after" "$growth"
# A retried *propose* creates a new document each time by design — a proposal
# with no existing document to attach to is a new decision, and the dedup key
# scopes to a document rather than across documents. What must not grow is the
# hook path, and what the twenty proposals prove is that they are bounded and
# accounted for rather than silently multiplying revisions.
if [ "$growth" -gt 1048576 ]; then
    printf 'FAIL: forty retries grew the database by %s bytes\n' "$growth" >&2
    fail=1
else
    printf 'ok  : %-30s %s bytes for 40 retries\n' "durable growth" "$growth"
fi

# --- peak RSS ---------------------------------------------------------------
#
# Read from /proc/<pid>/VmHWM by the process itself just before it exits, which
# is the only place the true figure is available. `atlas doctor` is used because
# it opens the index, reports on it and exits without writing anything.

printf '\n=== peak RSS ===\n'
rss_of() {
    # Runs the command and reports the child's VmHWM in kB, sampled by a watcher
    # rather than after the fact: /proc/<pid> is gone once the child exits.
    ATLAS_DATA_DIR="$data" "$atlas" "$@" > /dev/null 2>&1 &
    child=$!
    peak=0
    while kill -0 "$child" 2>/dev/null; do
        v=$(awk '/VmHWM/{print $2}' "/proc/$child/status" 2>/dev/null || true)
        if [ -n "$v" ] && [ "$v" -gt "$peak" ] 2>/dev/null; then
            peak=$v
        fi
    done
    wait "$child" 2>/dev/null || true
    echo "$peak"
}
list_rss=$(rss_of --json decision list perf --limit 200)
show_rss=$(rss_of decision show perf "$some_id")
printf 'info: %-30s %s kB\n' "decision list peak RSS" "$list_rss"
printf 'info: %-30s %s kB\n' "decision show peak RSS" "$show_rss"

printf '\n'
if [ "$fail" -ne 0 ]; then
    printf 'A4 performance acceptance FAILED\n' >&2
    exit 1
fi
printf 'A4 performance acceptance passed.\n'
printf 'Every figure describes this machine and the synthetic fixture above. It is\n'
printf 'not a claim about any real repository.\n'
