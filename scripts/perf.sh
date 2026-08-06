#!/bin/sh
# Atlas - A1 performance acceptance measurements.
# Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
#
# Builds synthetic repositories of named sizes and measures the properties A1
# claims: that a repeated pass reads nothing, that one changed file costs one
# file, that a burst coalesces, and that readers stay responsive.
#
# No Python, no Node, no runtime. /bin/sh, git, and the atlas binary.
#
# Usage: scripts/perf.sh [BUILD_DIR]
#
# Numbers from this script describe the machine it ran on and the fixture sizes
# named below. They are not a claim about any real repository.

set -eu

build=${1:-build}
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
atlas="$root/$build/atlas"

if [ ! -x "$atlas" ]; then
    printf 'no atlas binary at %s; run make first\n' "$atlas" >&2
    exit 1
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/atlas-perf.XXXXXX")
# A Unix-domain socket address is a fixed 108-byte field, so the runtime
# directory goes somewhere short regardless of where TMPDIR points. Atlas
# refuses a path that would not fit rather than truncating it into a different
# path, so without this the daemon would correctly decline to start.
runtime=$(mktemp -d /tmp/atls.XXXXXX)
trap 'rm -rf "$work" "$runtime"' EXIT INT TERM

export ATLAS_DATA_DIR="$work/data"
export XDG_RUNTIME_DIR="$runtime"
chmod 700 "$XDG_RUNTIME_DIR"

# --- fixture ---------------------------------------------------------------

# make_repo NAME FILES DIRS BYTES_PER_FILE
make_repo() {
    name=$1
    files=$2
    dirs=$3
    bytes=$4
    repo="$work/$name"
    mkdir -p "$repo"
    git -C "$repo" init -q .
    git -C "$repo" config user.email perf@example.invalid
    git -C "$repo" config user.name Perf
    git -C "$repo" config commit.gpgsign false

    # One line repeated to the requested size, so content is deterministic and
    # the numbers do not depend on a random generator.
    line='static const char pad[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";'
    reps=$((bytes / 96 + 1))

    d=0
    while [ "$d" -lt "$dirs" ]; do
        mkdir -p "$repo/d$d"
        d=$((d + 1))
    done

    i=0
    while [ "$i" -lt "$files" ]; do
        target="$repo/d$((i % dirs))/f$i.c"
        {
            printf '/* file %d */\n' "$i"
            r=0
            while [ "$r" -lt "$reps" ]; do
                printf '%s\n' "$line"
                r=$((r + 1))
            done
        } > "$target"
        i=$((i + 1))
    done

    git -C "$repo" add -A
    git -C "$repo" commit -q -m "seed $name"
    printf '%s' "$repo"
}

# Wall-clock milliseconds around a command.
timed() {
    start=$(date +%s%N)
    "$@" > /dev/null 2>&1 || true
    end=$(date +%s%N)
    printf '%s' $(( (end - start) / 1000000 ))
}

field() {
    # field JSON KEY -> the integer value of "KEY":N
    printf '%s' "$1" | tr ',' '\n' | grep -m1 "\"$2\":" | sed 's/.*: *//' | tr -d '}' || true
}

report() {
    printf '  %-38s %s\n' "$1" "$2"
}

# --- measurements ----------------------------------------------------------

printf '== fixtures\n'
small=$(make_repo small 200 8 2048)
report "small: 200 files, 8 dirs, ~2 KiB each" "$(du -sh "$small" | cut -f1)"
large=$(make_repo large 5000 50 4096)
report "large: 5000 files, 50 dirs, ~4 KiB each" "$(du -sh "$large" | cut -f1)"

"$atlas" repo add "$small" --name small > /dev/null
"$atlas" repo add "$large" --name large > /dev/null

printf '\n== initial versus incremental pass (large fixture)\n'
initial_ms=$(timed "$atlas" sync large --full)
out=$("$atlas" sync large --json)
read_after=$(field "$out" files_content_read)
examined=$(field "$out" files_examined)
identity=$(field "$out" files_unchanged_by_identity)
incr_ms=$(timed "$atlas" sync large)

report "initial full pass" "${initial_ms} ms"
report "incremental pass, nothing changed" "${incr_ms} ms"
report "files examined" "$examined"
report "files whose content was read" "$read_after"
report "files skipped by stored identity" "$identity"

printf '\n== one changed file (large fixture)\n'
printf '/* touched */\n' >> "$large/d0/f0.c"
out=$("$atlas" sync large --json)
one_read=$(field "$out" files_content_read)
one_ms=$(field "$out" duration_ms)
one_mod=$(field "$out" files_modified)
report "files whose content was read" "$one_read"
report "files reported modified" "$one_mod"
report "pass duration" "${one_ms} ms"

printf '\n== full pass reads every eligible file (large fixture)\n'
# A warm cache followed by --full: the whole point is that "full" ignores every
# stored identity and reads the bytes, because that is the only thing that can
# honestly clear an event gap.
out=$("$atlas" sync large --full --json)
full_read=$(field "$out" files_content_read)
full_hit=$(field "$out" files_unchanged_by_identity)
full_ms=$(field "$out" duration_ms)
full_verified=$(printf '%s' "$out" | tr ',' '\n' | grep -m1 '"content_verified"' | sed 's/.*: *//')
report "files examined" "$(field "$out" files_examined)"
report "files whose content was read" "$full_read"
report "files skipped by stored identity" "$full_hit"
report "content_verified" "$full_verified"
report "pass duration" "${full_ms} ms"

printf '\n== same-length edit with the mtime restored (large fixture)\n'
# The defect this pass corrects: identical device, inode, size, mode and mtime,
# with only ctime moved. Without ctime in the identity this reads zero files and
# keeps the stale hash forever.
victim="$large/d1/f1.c"   # file i lives in d(i mod dirs), so f1 is in d1
orig_size=$(wc -c < "$victim")
touch -r "$victim" "$work/stamp"
dd if=/dev/zero bs=1 count="$orig_size" 2>/dev/null | tr '\0' 'Z' > "$victim"
touch -r "$work/stamp" "$victim"
out=$("$atlas" sync large --json)
report "files whose content was read" "$(field "$out" files_content_read)"
report "files reported modified" "$(field "$out" files_modified)"

printf '\n== FTS is maintained, not rebuilt\n'
# A rebuild would be O(index); the incremental path touches one row. The
# observable proof is that a one-file pass costs about what an unchanged pass
# costs, not what an initial pass costs.
report "initial pass" "${initial_ms} ms"
report "one-file pass" "${one_ms} ms"

printf '\n== burst coalescing (small fixture, daemon)\n'
"$atlas" daemon run > "$work/daemon.log" 2>&1 &
dpid=$!
i=0
while [ "$i" -lt 100 ] && ! "$atlas" daemon ping > /dev/null 2>&1; do
    sleep 0.1
    i=$((i + 1))
done
# A daemon that failed to start would make every measurement below silently read
# zero, which is worse than no measurement at all.
if ! "$atlas" daemon ping > /dev/null 2>&1; then
    printf '\nthe daemon did not start; the remaining measurements are meaningless\n' >&2
    cat "$work/daemon.log" >&2
    exit 1
fi
# Let the startup full pass of both fixtures finish, so the burst below is
# measured against a quiet daemon rather than against startup work.
sleep 3

before=$(grep -c 'reconciled small' "$work/daemon.log" || printf '0')
i=0
while [ "$i" -lt 50 ]; do
    printf '/* burst %d */\n' "$i" >> "$small/d0/f$i.c"
    i=$((i + 1))
done
sleep 3
after=$(grep -c 'reconciled small' "$work/daemon.log" || printf '0')
report "writes issued" "50"
report "reconciliation passes triggered" "$((after - before))"

printf '\n== reader responsiveness while the writer works\n'
# Start a full pass of the large repository through the daemon, then time reads
# against it. WAL plus per-request read-only handles mean a reader never waits
# for the writer.
"$atlas" sync large --full > /dev/null 2>&1 &
syncpid=$!
slow=0
n=0
while [ "$n" -lt 10 ]; do
    ms=$(timed "$atlas" daemon status)
    [ "$ms" -gt "$slow" ] && slow=$ms
    n=$((n + 1))
done
wait $syncpid 2>/dev/null || true
report "slowest of 10 reads during a full pass" "${slow} ms"

printf '\n== event-to-index latency (small fixture)\n'
# The journal already holds hundreds of events, so the poll has to start from a
# cursor rather than from the beginning: reading the oldest page forever would
# measure nothing but the timeout.
cursor=$(field "$("$atlas" events small --json --limit 1)" event_cursor)
start=$(date +%s%N)
printf '/* latency probe */\n' > "$small/d1/latency-probe.c"
found=no
n=0
while [ "$n" -lt 300 ]; do
    if "$atlas" events small --json --since "$cursor" --limit 200 2>/dev/null \
        | grep -q 'latency-probe.c'; then
        found=yes
        break
    fi
    sleep 0.1
    n=$((n + 1))
done
end=$(date +%s%N)
if [ "$found" = yes ]; then
    report "file created to visible in the journal" "$(( (end - start) / 1000000 )) ms"
else
    report "file created to visible in the journal" "NOT OBSERVED within 30 s"
fi

printf '\n== database growth on repeated idle passes (large fixture)\n'
size_before=$(wc -c < "$ATLAS_DATA_DIR/atlas.db")
n=0
while [ "$n" -lt 5 ]; do
    "$atlas" sync large > /dev/null 2>&1
    n=$((n + 1))
done
size_after=$(wc -c < "$ATLAS_DATA_DIR/atlas.db")
report "database bytes before 5 idle passes" "$size_before"
report "database bytes after" "$size_after"
report "growth" "$((size_after - size_before)) bytes"

printf "\n== daemon log\n"
sed "s/^/  /" "$work/daemon.log"

kill -TERM $dpid 2>/dev/null || true
wait $dpid 2>/dev/null || true

printf '\n== integrity after everything\n'
"$atlas" doctor --json | tr ',' '\n' \
    | grep -E '"integrity_check"|"foreign_key_check"|"schema_version"' | sed 's/^/  /'

printf '\nNote: these numbers describe this machine and the fixture sizes above.\n'
printf 'They are not a claim about the performance of any real repository.\n'
