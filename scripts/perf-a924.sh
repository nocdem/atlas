#!/bin/sh
# Atlas - A9.2.4 acceptance measurements: what build-input discovery costs.
#
# Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
#
# The discipline every `perf-*.sh` in this repository follows: **assert the
# floors and assert the limits, and exit non-zero rather than print a number
# nobody checks.** A measurement that is only printed is a measurement that stops
# being true without anybody noticing.
#
# What is measured, and why each one:
#
#   1. **One bounded walk of a real repository.** Discovery is the one expensive
#      thing this season adds, and it is what decides whether the interval
#      constant is chosen for convergence or for cost.
#   2. **`code sem-status`, which is the read every surface makes.** A9.2.3's
#      closure found this computing freshness twice and hashing every declared
#      compilation database twice; A9.2.4 gives those bytes a second consumer, so
#      the number is checked rather than assumed.
#   3. **A repository with no build description**, as the baseline the other two
#      are read against.
#
# The limits below are **required bounds, not observations**. Report an
# observation as an observation — "41 ms observed", never "under 41 ms", which
# reads as a bound Atlas does not hold.
set -eu

ATLAS="${ATLAS:-/usr/local/bin/atlas}"
# The repository to measure against. Named on the command line so this script
# contains no repository name of its own — the season's own rule.
REPO="${1:-}"
BASE="${2:-}"

if [ -z "$REPO" ]; then
  printf 'usage: %s REPO [BASELINE-REPO]\n' "$0" >&2
  printf '  REPO      a registered repository with discovered build inputs\n' >&2
  printf '  BASELINE  a registered repository with none, for the baseline\n' >&2
  exit 2
fi

# --- the limits, stated before anything is measured -------------------------
# A status read is on the path of every model tool call and every Mission
# Control refresh, so it is bounded at a tenth of a second: past that it stops
# being a thing a surface can do freely.
LIMIT_STATUS_MS=100
# One discovery walk runs on the writer thread. It is bounded well below the
# sweep interval, because a walk that outlasts its own interval would mean the
# daemon is always walking.
LIMIT_DISCOVER_MS=5000

fail=0
note() { printf '  %-38s %s\n' "$1" "$2"; }
check() { # check <label> <observed-ms> <limit-ms>
  if [ "$2" -gt "$3" ]; then
    printf '  FAIL  %s: %s ms observed, required under %s ms\n' "$1" "$2" "$3"
    fail=$((fail+1))
  else
    printf '  ok    %-30s %s ms observed (limit %s)\n' "$1" "$2" "$3"
  fi
}

ms_of() { # ms_of <command...>  -> elapsed milliseconds, best of three
  best=""
  for _ in 1 2 3; do
    s=$(date +%s%N)
    "$@" >/dev/null 2>&1 || true
    e=$(date +%s%N)
    d=$(( (e - s) / 1000000 ))
    if [ -z "$best" ] || [ "$d" -lt "$best" ]; then best=$d; fi
  done
  printf '%s' "$best"
}

printf '== A9.2.4 measurements\n'
"$ATLAS" --version
printf '\n-- the repository under measurement\n'
J=$("$ATLAS" code sem-status "$REPO" --json 2>&1)
for k in discovery inputs_accepted inputs_rejected tu_total scope_candidates; do
  v=$(printf '%s' "$J" | sed -n 's/.*"'"$k"'":\([^,}]*\).*/\1/p' | head -1)
  note "$k" "${v:-(absent)}"
done

# --- the floors: refuse to measure something too small to mean anything -----
ACCEPTED=$(printf '%s' "$J" | sed -n 's/.*"inputs_accepted":\([0-9]*\).*/\1/p' | head -1)
CANDIDATES=$(printf '%s' "$J" | sed -n 's/.*"scope_candidates":\([0-9]*\).*/\1/p' | head -1)
: "${ACCEPTED:=0}" "${CANDIDATES:=0}"
if [ "$ACCEPTED" -lt 1 ]; then
  printf '\nrefusing to measure: %s has no accepted build input, so a walk over it\n' "$REPO"
  printf 'measures nothing this season is about.\n'
  exit 1
fi
if [ "$CANDIDATES" -lt 100 ]; then
  printf '\nrefusing to measure: %s holds %s candidate sources, which is too few for\n' \
      "$REPO" "$CANDIDATES"
  printf 'the numbers to mean anything. Shrinking the fixture to make a run pass is the\n'
  printf 'one failure mode a performance gate cannot detect about itself.\n'
  exit 1
fi

printf '\n-- one status read (the read every surface makes)\n'
STATUS_MS=$(ms_of "$ATLAS" code sem-status "$REPO" --json)
check 'sem-status' "$STATUS_MS" "$LIMIT_STATUS_MS"

if [ -n "$BASE" ]; then
  BASE_MS=$(ms_of "$ATLAS" code sem-status "$BASE" --json)
  note 'baseline (no build description)' "${BASE_MS} ms observed"
fi

printf '\n-- one bounded discovery walk\n'
# Writing the configuration re-walks, because the write just changed what the
# walk would do — so a no-op write is the honest way to time one from outside.
DISCOVER_MS=$(ms_of "$ATLAS" code sem-config "$REPO" --discover)
check 'discovery walk' "$DISCOVER_MS" "$LIMIT_DISCOVER_MS"

printf '\n%s\n' "$( [ "$fail" -eq 0 ] && echo 'all measurements within their required limits' \
                                     || echo "$fail measurement(s) outside their limits" )"
[ "$fail" -eq 0 ]
