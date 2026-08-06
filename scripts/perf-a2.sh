#!/bin/sh
# Atlas - A2 performance acceptance measurements.
# Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
#
# Measures what a person actually waits for: how long a Claude Code hook takes,
# in the four cases that matter — a passive hook against a warm daemon, the same
# hook with no daemon at all, a session start, and a tool batch — plus one MCP
# round trip.
#
# The targets A2 set itself:
#
#   passive hooks                p95 below  50 ms
#   daemon unavailable           p95 below  50 ms
#   SessionStart, PostToolBatch  p95 below 250 ms
#
# The daemon-unavailable case has a target at all because it is the one a user
# hits when they have not started the service. A memory system that costs 50 ms
# per tool call when it is not even running is one they will remove.
#
# No Python, no Node, no runtime. /bin/sh, git, and the atlas binary.
#
# Usage: scripts/perf-a2.sh [BUILD_DIR]
#
# Numbers describe the machine this ran on. They are not a claim about anyone
# else's.

set -eu

build=${1:-build}
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
atlas="$root/$build/atlas"

if [ ! -x "$atlas" ]; then
    printf 'no atlas binary at %s; run make first\n' "$atlas" >&2
    exit 1
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/atlas-perf-a2.XXXXXX")
# A Unix-domain socket address is a fixed 108-byte field, so the runtime
# directory goes somewhere short regardless of where TMPDIR points.
runtime=$(mktemp -d /tmp/atla2.XXXXXX)
cleanup() {
    if [ -n "${daemon_pid:-}" ]; then
        kill "$daemon_pid" 2>/dev/null || true
        wait "$daemon_pid" 2>/dev/null || true
    fi
    rm -rf "$work" "$runtime"
}
trap cleanup EXIT INT TERM

export ATLAS_DATA_DIR="$work/data"
export XDG_RUNTIME_DIR="$runtime"
chmod 700 "$XDG_RUNTIME_DIR"

repo="$work/repo"
mkdir -p "$repo"
(
    cd "$repo"
    git init -q -b main .
    git config user.email atlas@example.invalid
    git config user.name Atlas
    i=0
    while [ "$i" -lt 200 ]; do
        printf 'int f%d(void) { return %d; }\n' "$i" "$i" > "src$i.c"
        i=$((i + 1))
    done
    git add -A
    git commit -qm "initial"
)

# --- measurement -----------------------------------------------------------

# Runs one hook `n` times and reports the median and the 95th percentile.
#
# Percentiles from sorted samples with `sort -n` and `sed -n`, because the whole
# point of the no-runtime rule is that a verification script needs nothing that
# is not already here.
measure() {
    label=$1
    event=$2
    payload=$3
    n=$4
    runtime_dir=$5

    samples="$work/samples"
    : > "$samples"
    i=0
    while [ "$i" -lt "$n" ]; do
        start=$(date +%s%N)
        printf '%s' "$payload" |
            XDG_RUNTIME_DIR="$runtime_dir" "$atlas" hook "$event" >/dev/null 2>&1 || true
        end=$(date +%s%N)
        echo $(( (end - start) / 1000000 )) >> "$samples"
        i=$((i + 1))
    done
    sort -n "$samples" > "$samples.sorted"
    median_line=$(( (n + 1) / 2 ))
    p95_line=$(( (n * 95 + 99) / 100 ))
    [ "$p95_line" -lt 1 ] && p95_line=1
    [ "$p95_line" -gt "$n" ] && p95_line="$n"
    median=$(sed -n "${median_line}p" "$samples.sorted")
    p95=$(sed -n "${p95_line}p" "$samples.sorted")
    max=$(sed -n "${n}p" "$samples.sorted")
    printf '  %-38s median %4s ms   p95 %4s ms   max %4s ms\n' "$label" "$median" "$p95" "$max"
}

S='{"session_id":"perf-1","prompt_id":"p1","cwd":"'"$repo"'"'
P_START="$S"',"hook_event_name":"SessionStart","source":"startup"}'
P_PROMPT="$S"',"hook_event_name":"UserPromptSubmit","user_message":"do the thing"}'
P_PRE="$S"',"hook_event_name":"PreToolUse","tool_name":"Edit","tool_use_id":"tu-1","tool_input":{"file_path":"'"$repo"'/src1.c"}}'
P_POST="$S"',"hook_event_name":"PostToolUse","tool_name":"Edit","tool_use_id":"tu-1","tool_input":{"file_path":"'"$repo"'/src1.c"},"tool_result":"ok"}'
P_BATCH="$S"',"hook_event_name":"PostToolBatch","tool_calls":[{"tool_name":"Edit","tool_use_id":"tu-1","tool_input":{"file_path":"'"$repo"'/src1.c"},"error":null}]}'
P_STOP="$S"',"hook_event_name":"Stop","stop_reason":"end_turn"}'

printf '\n== hooks with no daemon (the fail-open path)\n'
# A runtime directory with nothing listening in it. This is what a user gets
# before they enable the service, and it has to be free.
nodaemon=$(mktemp -d /tmp/atlnd.XXXXXX)
chmod 700 "$nodaemon"
measure "UserPromptSubmit (no daemon)" UserPromptSubmit "$P_PROMPT" 40 "$nodaemon"
measure "PostToolUse (no daemon)" PostToolUse "$P_POST" 40 "$nodaemon"
measure "SessionStart (no daemon)" SessionStart "$P_START" 20 "$nodaemon"
rm -rf "$nodaemon"

# --- with a warm daemon ----------------------------------------------------

printf '\n== starting the daemon\n'
"$atlas" daemon run > "$work/daemon.log" 2>&1 &
daemon_pid=$!
tries=0
while [ "$tries" -lt 100 ]; do
    if "$atlas" daemon ping >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
    tries=$((tries + 1))
done
"$atlas" daemon ping >/dev/null 2>&1 || { printf '  daemon did not start\n'; exit 1; }
printf '  ready after %s polls\n' "$tries"

# Register and index once, so the measurements below are of a warm daemon rather
# than of a first run. The first SessionStart pays for registration and is
# measured separately.
printf '\n== first SessionStart in an unregistered repository\n'
measure "SessionStart (registers the repo)" SessionStart "$P_START" 1 "$runtime"
"$atlas" sync repo --wait --timeout-ms 60000 >/dev/null 2>&1 || true

printf '\n== hooks against a warm daemon\n'
measure "UserPromptSubmit" UserPromptSubmit "$P_PROMPT" 40 "$runtime"
measure "PreToolUse" PreToolUse "$P_PRE" 40 "$runtime"
measure "PostToolUse" PostToolUse "$P_POST" 40 "$runtime"
measure "Stop" Stop "$P_STOP" 40 "$runtime"
measure "SessionStart (already registered)" SessionStart "$P_START" 20 "$runtime"
measure "PostToolBatch" PostToolBatch "$P_BATCH" 20 "$runtime"

# --- MCP -------------------------------------------------------------------

printf '\n== MCP round trips\n'
script="$work/mcp.jsonl"
{
    printf '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{}}}\n'
    printf '{"jsonrpc":"2.0","method":"notifications/initialized"}\n'
    printf '{"jsonrpc":"2.0","id":2,"method":"tools/list"}\n'
} > "$script"

start=$(date +%s%N)
lines=$(CLAUDE_PROJECT_DIR="$repo" "$atlas" mcp < "$script" 2>/dev/null | wc -l)
end=$(date +%s%N)
printf '  %-38s %4s ms   (%s messages)\n' "initialize + tools/list" \
    "$(( (end - start) / 1000000 ))" "$lines"

{
    cat "$script"
    printf '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"atlas_repo_overview","arguments":{}}}\n'
    printf '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"atlas_changed_files","arguments":{}}}\n'
    printf '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"atlas_file_context","arguments":{"path":"src1.c"}}}\n'
} > "$work/mcp-tools.jsonl"

start=$(date +%s%N)
lines=$(CLAUDE_PROJECT_DIR="$repo" "$atlas" mcp < "$work/mcp-tools.jsonl" 2>/dev/null | wc -l)
end=$(date +%s%N)
printf '  %-38s %4s ms   (%s messages)\n' "handshake + three tool calls" \
    "$(( (end - start) / 1000000 ))" "$lines"

printf '\n== what the session recorded\n'
"$atlas" repo list --json 2>/dev/null | tr ',' '\n' | grep -E '"name"|"count"' | sed 's/^/  /' || true

printf '\n== integrity\n'
"$atlas" doctor --json 2>/dev/null | tr ',' '\n' |
    grep -E '"schema_version"|"integrity_check"|"foreign_key_check"' | sed 's/^/  /' || true

printf '\nNote: these numbers describe this machine. Each hook figure includes\n'
printf 'process start, so it is what Claude actually waits for, not just the\n'
printf 'time Atlas spends working.\n'
