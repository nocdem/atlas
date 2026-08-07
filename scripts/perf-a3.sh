#!/bin/sh
# Atlas - A3 structural-indexing performance acceptance.
# Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
#
# Builds the deterministic synthetic C tree that approximates a DNA-scale
# project and measures what A3 claims: that the initial structural index is
# affordable, that an unchanged pass parses nothing, that one edit costs one
# file, that a header edit does not reparse the world, and that the bounded
# queries stay well under the interactive ceiling.
#
# No Python, no Node, no runtime. /bin/sh, git, and two compiled binaries.
#
# Usage: scripts/perf-a3.sh [BUILD_DIR]
#
# Every number here describes the machine it ran on and the fixture below. It is
# not a claim about any real repository, and it is not a claim about DNA: the
# fixture is deliberately synthetic, and the real repository is not indexed until
# a later phase.

set -eu

build=${1:-build}
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
atlas="$root/$build/atlas"
gen="$root/$build/tests/atlas-gen-ctree"

if [ ! -x "$atlas" ]; then
    printf 'no atlas binary at %s; run make first\n' "$atlas" >&2
    exit 1
fi
if [ ! -x "$gen" ]; then
    printf 'no fixture generator at %s; run make first\n' "$gen" >&2
    exit 1
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/atlas-perf-a3.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM

export ATLAS_DATA_DIR="$work/data"
repo="$work/tree"

# --- the fixture ------------------------------------------------------------
#
# 160 modules of 32 files, five entry functions each: about 5 100 sources plus
# 330 headers, half a million lines, and a graph with include cycles, repeated
# static names, ambiguous includes, unresolved calls and a test subtree. The
# shape matters more than the size: a uniform tree would measure one code path.

modules=${ATLAS_PERF_MODULES:-176}
per_module=${ATLAS_PERF_FILES:-32}

printf '=== fixture ===\n'
"$gen" "$repo" "$modules" "$per_module"

# The counting method, fixed before anything is measured and stated in the
# output so it cannot be reinterpreted afterwards: newline count over every
# tracked `.c` and `.h` file in the tree, whatever it contains — blank lines,
# comments and generated headers all included. `wc -l` counts newlines, and the
# generator terminates every line, so the two agree.
files=$(find "$repo" \( -name '*.c' -o -name '*.h' \) -type f | wc -l)
lines=$(find "$repo" \( -name '*.c' -o -name '*.h' \) -type f -exec cat {} + | wc -l)
bytes=$(du -sk "$repo" | cut -f1)
printf 'files              %s\n' "$files"
printf 'lines              %s   (newlines in every .c and .h)\n' "$lines"
printf 'tree size          %s KiB\n' "$bytes"

# --- the scale floors -------------------------------------------------------
#
# Asserted, not described. A fixture that quietly shrank below one of these
# would turn a failing gate into a passing one, and the difference would be
# invisible in the numbers underneath.

floor() {
    if [ "$2" -lt "$3" ]; then
        printf 'FIXTURE TOO SMALL: %s is %s, floor is %s\n' "$1" "$2" "$3" >&2
        exit 1
    fi
    printf 'floor ok           %-12s %8s >= %s\n' "$1" "$2" "$3"
}
floor files "$files" 5000
floor lines "$lines" 500000

git -C "$repo" init -q .
git -C "$repo" config user.email perf@example.invalid
git -C "$repo" config user.name Perf
git -C "$repo" config commit.gpgsign false
git -C "$repo" add -A
git -C "$repo" -c commit.gpgsign=false commit -qm seed

# --- helpers ----------------------------------------------------------------

now_ms() {
    # date +%s%3N is a GNU extension and is present wherever this runs; the
    # fallback keeps the script honest rather than silently reporting zeros on a
    # platform that lacks it.
    d=$(date +%s%3N 2>/dev/null || true)
    case "$d" in
        *N|'') date +%s | awk '{print $1 * 1000}' ;;
        *) printf '%s\n' "$d" ;;
    esac
}

# Peak resident set comes from the kernel's own high-water mark rather than from
# time(1), and that is deliberate. `busybox time -v` prints ru_maxrss multiplied
# by the page size, so on a 4 KiB-page machine every RSS it reports is four times
# the truth — and a measurement wrong by a constant factor is worse than none,
# because it still looks like a measurement. /proc/<pid>/VmHWM is what the kernel
# recorded, needs no tool, and cannot be off by a factor.
#
# Polling it costs one core in a shell loop that forks nothing. Measured against
# the same runs timed without it, the difference in wall clock is inside the
# run-to-run noise, because the pass being measured is the single writer thread.

# run_timed LABEL COMMAND...
# Prints the elapsed wall-clock milliseconds and the peak resident set.
run_timed() {
    label=$1
    shift
    hwm=0
    start=$(now_ms)
    "$@" >"$work/out" 2>"$work/err" &
    pid=$!
    # The redirections are ordered `2>/dev/null` *before* the input, and that is
    # load-bearing rather than stylistic: a shell applies them left to right, so
    # with the input first the open fails before stderr has been silenced and the
    # diagnostic reaches the log — which under `set -e` also ends the script. The
    # open failing is the normal way this loop ends: the process exited.
    set +e
    while :; do
        found=0
        while read -r key value rest; do
            if [ "$key" = "VmHWM:" ]; then
                hwm=$value
                found=1
            fi
        done 2>/dev/null <"/proc/$pid/status"
        [ "$found" = 1 ] || break
    done
    set -e
    if ! wait "$pid"; then
        printf '%s FAILED\n' "$label" >&2
        cat "$work/err" >&2
        exit 1
    fi
    end=$(now_ms)
    if [ "$hwm" = 0 ]; then
        rss='unmeasured'
    else
        rss="$hwm KiB"
    fi
    printf '%-34s %6s ms   peak RSS %s\n' "$label" "$((end - start))" "$rss"
}

# json_field FILE KEY — reads one integer or boolean from a flat JSON document.
json_field() {
    sed -n "s/.*\"$2\":\\([^,}]*\\).*/\\1/p" "$1" | head -1
}

# percentile_ms N COMMAND... — runs the command N times and reports p50/p95.
percentiles() {
    label=$1
    runs=$2
    shift 2
    : >"$work/times"
    i=0
    while [ "$i" -lt "$runs" ]; do
        s=$(now_ms)
        "$@" >/dev/null 2>&1
        e=$(now_ms)
        printf '%s\n' "$((e - s))" >>"$work/times"
        i=$((i + 1))
    done
    sort -n "$work/times" >"$work/sorted"
    p50=$(awk -v n="$runs" 'NR == int((n + 1) / 2) {print; exit}' "$work/sorted")
    p95=$(awk -v n="$runs" 'NR == int(n * 95 / 100) + (n * 95 % 100 ? 1 : 0) {print; exit}' \
        "$work/sorted")
    printf '%-34s p50 %4s ms   p95 %4s ms\n' "$label" "${p50:-0}" "${p95:-0}"
}

db_kib() {
    du -sk "$ATLAS_DATA_DIR" 2>/dev/null | cut -f1
}

# --- initial index ----------------------------------------------------------

# --- initial indexing, three independent runs -------------------------------
#
# Three, each against a data directory that has never existed before, because
# one number is an anecdote: the page cache is warm differently on a second run,
# and a gate that passes on the median while one run misses has not passed.
# Every run is printed, and the worst is what the gate is judged on.

printf '\n=== initial structural indexing ===\n'
runs=${ATLAS_PERF_RUNS:-3}
: >"$work/initial.ms"
i=1
while [ "$i" -le "$runs" ]; do
    rm -rf "$work/data"
    "$atlas" repo add "$repo" --name perf >/dev/null
    start=$(now_ms)
    run_timed "initial full pass ($i of $runs)" "$atlas" sync perf --full --json
    end=$(now_ms)
    printf '%s\n' "$((end - start))" >>"$work/initial.ms"
    if [ "$i" -eq "$runs" ]; then
        cp "$work/out" "$work/first.json"
    fi
    i=$((i + 1))
done
sort -n "$work/initial.ms" >"$work/initial.sorted"
printf 'initial: best %s ms   median %s ms   worst %s ms\n' \
    "$(head -1 "$work/initial.sorted")" \
    "$(awk -v n="$runs" 'NR == int((n + 1) / 2) {print; exit}' "$work/initial.sorted")" \
    "$(tail -1 "$work/initial.sorted")"
worst=$(tail -1 "$work/initial.sorted")
if [ "$worst" -ge 60000 ]; then
    printf 'GATE FAILED: the slowest initial index was %s ms, limit is 60000 ms\n' "$worst" >&2
    exit 1
fi

"$atlas" code status perf --json >"$work/status.json"
printf 'code index current                %s\n' "$(json_field "$work/status.json" code_index_current)"
printf 'files indexed                     %s\n' "$(json_field "$work/status.json" files_indexed)"
printf 'symbols                           %s\n' "$(json_field "$work/status.json" symbols)"
printf 'relations                         %s\n' "$(json_field "$work/status.json" relations)"
printf 'ambiguous                         %s\n' "$(json_field "$work/status.json" ambiguous)"
printf 'unresolved                        %s\n' "$(json_field "$work/status.json" unresolved)"
printf 'compile units                     %s\n' "$(json_field "$work/status.json" compile_units)"
printf 'analyzer                          %s v%s\n' \
    "$(sed -n 's/.*"analyzer":"\([^"]*\)".*/\1/p' "$work/status.json" | head -1)" \
    "$(json_field "$work/status.json" analyzer_version)"
printf 'database                          %s KiB\n' "$(db_kib)"

floor symbols "$(json_field "$work/status.json" symbols)" 50000
floor relations "$(json_field "$work/status.json" relations)" 200000

# --- the unchanged pass -----------------------------------------------------
#
# The claim that matters most: a pass over an unchanged tree parses nothing,
# *including* a full content-verifying pass, because selection compares the hash
# the graph facts were built from rather than the pass's own activity.

printf '\n=== unchanged passes ===\n'
before_db=$(db_kib)
before_rel=$(json_field "$work/status.json" relations)
run_timed 'unchanged incremental pass' "$atlas" sync perf --json
printf 'files parsed                      %s\n' "$(json_field "$work/out" code_files_parsed)"
run_timed 'unchanged full content pass' "$atlas" sync perf --full --json
printf 'files parsed                      %s\n' "$(json_field "$work/out" code_files_parsed)"

i=0
while [ "$i" -lt 3 ]; do
    "$atlas" sync perf --json >/dev/null
    i=$((i + 1))
done
"$atlas" code status perf --json >"$work/status2.json"
after_db=$(db_kib)
after_rel=$(json_field "$work/status2.json" relations)
printf 'database before / after           %s / %s KiB\n' "$before_db" "$after_db"
printf 'relations before / after          %s / %s\n' "$before_rel" "$after_rel"

# --- one-file update --------------------------------------------------------

printf '\n=== incremental updates ===\n'
target="$repo/src/mod3/part_7.c"
printf '\nint perf_added_symbol(void) { return 0; }\n' >>"$target"
run_timed 'one implementation file changed' "$atlas" sync perf --json
printf 'files parsed                      %s\n' "$(json_field "$work/out" code_files_parsed)"

# A header every module includes: the reverse-dependency and re-resolution path
# at full width. The claim is that this does *not* reparse the repository.
printf '\nint fixture_added_api(void);\n' >>"$repo/include/common.h"
run_timed 'shared header changed' "$atlas" sync perf --json
printf 'files parsed                      %s\n' "$(json_field "$work/out" code_files_parsed)"

# --- bounded queries --------------------------------------------------------

printf '\n=== bounded queries ===\n'
percentiles 'symbol search' 20 "$atlas" code search perf entry_3 --json
percentiles 'symbol context' 20 "$atlas" code symbol perf fixture_common_helper --json
percentiles 'file context' 20 "$atlas" code file perf src/mod3/part_7.c --json
percentiles 'dependencies (depth 2)' 20 "$atlas" code deps perf src/mod3/part_7.c --json
percentiles 'impact of a shared header' 20 "$atlas" code impact perf include/common.h --json

printf '\n=== acceptance targets ===\n'
printf 'initial indexing            < 60 s\n'
printf 'peak RSS                    < 512 MiB (524288 KiB)\n'
printf 'unchanged pass              0 files parsed, < 1 s\n'
printf 'one implementation update   < 2 s\n'
printf 'bounded query p95           < 100 ms\n'
printf 'repeated unchanged passes   no durable growth\n'
