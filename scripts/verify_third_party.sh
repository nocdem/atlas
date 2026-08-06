#!/bin/sh
# Atlas - verify vendored third-party source against its recorded digests.
# Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
#
# Vendored source is only trustworthy if it is still what was vendored. This
# re-computes the SHA-256 of every vendored file and compares it with
# third_party/yyjson/PROVENANCE.md. No network access, no language runtime.

set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fail=0

check() {
    path="$1"
    want="$2"
    if [ ! -f "$root/$path" ]; then
        printf '  FAIL  %s is missing\n' "$path"
        fail=$((fail + 1))
        return
    fi
    got=$(sha256sum "$root/$path" | cut -d' ' -f1)
    if [ "$got" = "$want" ]; then
        printf '  ok    %s\n' "$path"
    else
        printf '  FAIL  %s\n        want %s\n        got  %s\n' "$path" "$want" "$got"
        fail=$((fail + 1))
    fi
}

printf '== vendored third-party source\n'
check third_party/yyjson/yyjson.c \
    ac2e9bbb2e2d9149d90878d40506a1d624fa0b33c979a11b61075c54782c6d6a
check third_party/yyjson/yyjson.h \
    175867c5493a5df648cec566717fa1c29aa2f6096f5f0cf1efad0b65e1f6d7b3
check third_party/yyjson/LICENSE \
    45e384d3d52c73cba3a64d6e6c25d47cd738cd8a55c30629e3201046eda62947

if [ "$fail" -ne 0 ]; then
    printf '\n%d vendored file(s) do not match PROVENANCE.md.\n' "$fail" >&2
    printf 'Restore the upstream bytes; do not update the digest to match an edit.\n' >&2
    exit 1
fi
printf '\nvendored third-party source matches PROVENANCE.md\n'
