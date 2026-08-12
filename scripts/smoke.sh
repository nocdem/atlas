#!/bin/sh
# Atlas - CLI smoke test.
# Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
#
# Exercises the built binary against a throwaway repository and validates every
# JSON document with the compiled first-party checker. Deliberately POSIX sh with
# no language runtime: the verification workflow must depend on nothing beyond the
# C toolchain, SQLite and Git.
#
# Usage: scripts/smoke.sh [BUILD_DIR]        (default: build)

set -eu

BUILD="${1:-build}"
ATLAS="$BUILD/atlas"
JSONCHECK="$BUILD/tests/atlas-jsoncheck"

for tool in "$ATLAS" "$JSONCHECK"; do
    if [ ! -x "$tool" ]; then
        echo "smoke: $tool is missing; run 'make' first" >&2
        exit 1
    fi
done

WORK="$(mktemp -d "${TMPDIR:-/tmp}/atlas-smoke-XXXXXX")"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT INT TERM

DATA="$WORK/data"
REPO="$WORK/repo"
mkdir -p "$DATA" "$REPO"

A="$ATLAS --data-dir $DATA"
checks=0
fails=0

ok() { checks=$((checks + 1)); printf '  ok    %s\n' "$1"; }
bad() { checks=$((checks + 1)); fails=$((fails + 1)); printf '  FAIL  %s\n' "$1"; }

# Runs an atlas command, capturing stdout to a file. Piping into a function would
# run it in a subshell and lose the tallies, so the document goes to a file first.
DOC="$WORK/doc.json"
capture() {
    if $A "$@" > "$DOC" 2> "$WORK/stderr.txt"; then
        return 0
    fi
    printf '  FAIL  atlas %s exited %d: %s\n' "$*" "$?" "$(cat "$WORK/stderr.txt")"
    checks=$((checks + 1))
    fails=$((fails + 1))
    return 1
}

# Validates the captured document with the compiled checker.
check_doc() {
    label="$1"
    shift
    if "$JSONCHECK" "$@" < "$DOC" > /dev/null; then
        ok "$label"
    else
        bad "$label"
    fi
}

# --- fixture ---------------------------------------------------------------
# git is driven with an explicit environment so the run is reproducible.
export GIT_CONFIG_NOSYSTEM=1
export GIT_CONFIG_GLOBAL=/dev/null
export GIT_AUTHOR_NAME='Smoke Test'
export GIT_AUTHOR_EMAIL='smoke@atlas.invalid'
export GIT_COMMITTER_NAME='Smoke Test'
export GIT_COMMITTER_EMAIL='smoke@atlas.invalid'
export LC_ALL=C
export TZ=UTC

git -C "$REPO" init -q -b main .
printf 'int main(void){return 0;}\n' > "$REPO/main.c"
mkdir -p "$REPO/src"
printf 'void util(void){}\n' > "$REPO/src/util.c"
printf 'spaces\n' > "$REPO/with space.txt"
printf 'tabs\n' > "$REPO/with	tab.txt"
git -C "$REPO" add -A
git -C "$REPO" commit -q -m 'initial commit' -m 'body of the initial commit'
git -C "$REPO" mv src/util.c src/helper.c
git -C "$REPO" commit -q -m 'rename util to helper'
# Leave the tree dirty in all three ways so diff has something to report.
printf 'staged\n' > "$REPO/staged.txt"
git -C "$REPO" add staged.txt
printf 'int main(void){return 1;}\n' > "$REPO/main.c"
printf 'untracked\n' > "$REPO/untracked.txt"

echo "== human output"
for spec in "doctor" "version" "help"; do
    if $A $spec > /dev/null; then ok "atlas $spec"; else bad "atlas $spec"; fi
done
if $A repo add "$REPO" --name smoke > /dev/null; then ok "repo add"; else bad "repo add"; fi
for spec in "scan smoke" "status smoke" "repo list" "search smoke main" \
            "file smoke main.c" "history smoke src/helper.c" "diff smoke"; do
    if $A $spec > /dev/null; then ok "atlas $spec"; else bad "atlas $spec"; fi
done

echo "== JSON output validated by the compiled C checker"
capture --json doctor && check_doc "doctor" --expect command=doctor --expect-raw ok=true \
    --expect-raw schema_current=true --expect text_encoding=atlas-safe-1 --no-control
capture --json version && check_doc "version" --expect command=version --no-control
capture --json repo list && check_doc "repo list" --expect command="repo list" \
    --expect-raw count=1 --expect-raw is_linked_worktree=false --no-control
capture --json status smoke && check_doc "status" --expect command=status \
    --expect-raw head_drift=false --expect-raw sibling_worktrees=0 --no-control
capture --json scan smoke && check_doc "scan" --expect command=scan --no-control
capture --json search smoke main && check_doc "search" --expect query=main --no-control
capture --json file smoke main.c && check_doc "file" --expect path=main.c \
    --expect reason=UNKNOWN --expect content_hash_algo=sha256 --no-control
capture --json history smoke src/helper.c && check_doc "history" --expect command=history \
    --expect old_path=src/util.c --no-control

# The diff document must report its base and distinguish all four scopes.
if capture --json diff smoke; then
    check_doc "diff envelope" --expect command=diff --expect head_state=born \
        --expect-raw truncated=false --no-control
    check_doc "diff reports base_head" --raw base_head
    check_doc "diff has a staged section" --raw staged
    check_doc "diff has an unstaged section" --raw unstaged
    check_doc "diff has an untracked section" --raw untracked
    check_doc "diff has an unmerged section" --raw unmerged
    check_doc "diff counts binary changes" --raw binary_changes
    # The fixture staged one file, modified another, and left one untracked, so
    # three entries are reported in total. (The per-scope tallies live in the
    # nested "counts" object; "reported_entries" is the unique key there.)
    check_doc "diff reports three entries" --expect-raw reported_entries=3
    # Sections appear in the documented order, so the first entry is the staged one
    # and the untracked entry carries identity rather than contents.
    check_doc "first diff entry is the staged add" --expect path=staged.txt \
        --expect change_type=add
    check_doc "untracked entry carries a hash, not contents" \
        --expect content_hash_algo=sha256
fi

echo "== paths that are not plain text"
capture --json file smoke 'with%09tab.txt' && check_doc "tab in a filename" \
    --expect path=with%09tab.txt --no-control

echo "== error documents"
if $A --json status no-such-repo > "$DOC" 2> /dev/null; then
    bad "unknown repo should fail"
else
    code=$?
    if [ "$code" -eq 4 ]; then ok "unknown repo exits 4"; else bad "unknown repo exit $code"; fi
fi
check_doc "error document" --expect-raw ok=false --expect status=repo \
    --expect-raw exit_code=4 --no-control

# A repository name carrying a terminal escape must not reach the terminal raw.
if $A --json status "$(printf 'esc\033[31mname')" > "$DOC" 2> "$WORK/stderr.txt"; then
    bad "a bogus repo name should fail"
else
    ok "hostile repo name exits non-zero"
fi
check_doc "hostile name in an error document is encoded" --no-control
if grep -q "$(printf '\033')" "$WORK/stderr.txt"; then
    bad "an ESC byte reached stderr"
else
    ok "no ESC byte on stderr"
fi

echo "== the target repository is unchanged"
before="$(git -C "$REPO" status --porcelain=v2 --branch -z | od -An -tx1 | tr -d ' \n')"
$A status smoke > /dev/null
$A scan smoke > /dev/null
$A diff smoke > /dev/null
after="$(git -C "$REPO" status --porcelain=v2 --branch -z | od -An -tx1 | tr -d ' \n')"
if [ "$before" = "$after" ]; then
    ok "repository state unchanged by read commands"
else
    bad "repository state changed by read commands"
fi

echo "== A5: backup, verification, restore and maintenance"

capture --json backup create "$WORK/smoke.db" \
    && check_doc "backup create" --expect command="backup create" --expect-raw ok=true \
                 --expect-raw encrypted=false --expect-raw contains_configuration=false \
                 --no-control
capture --json backup verify "$WORK/smoke.db" \
    && check_doc "backup verify" --expect command="backup verify" --expect verdict=ok \
                 --expect-raw usable=true --no-control

# Refusing to overwrite is the default, and a refusal must still be one valid
# document with a usage exit code.
if $A --json backup create "$WORK/smoke.db" > "$DOC" 2> /dev/null; then
    bad "backup create overwrote an existing file without --force"
else
    rc=$?
    if [ "$rc" -eq 2 ] && "$JSONCHECK" --expect-raw ok=false < "$DOC" > /dev/null; then
        ok "backup create refuses to overwrite (exit 2, one document)"
    else
        bad "backup create refusal: exit $rc"
    fi
fi
$A backup create "$WORK/smoke.db" --force > /dev/null && ok "--force replaces it"

# An unusable backup is an answer: a complete document, then a non-zero exit.
printf 'not a database' > "$WORK/junk.db"
if $A --json backup verify "$WORK/junk.db" > "$DOC" 2> /dev/null; then
    bad "a junk file verified as usable"
else
    rc=$?
    if [ "$rc" -eq 7 ] && "$JSONCHECK" --expect verdict=not_sqlite < "$DOC" > /dev/null; then
        ok "backup verify refuses a non-database (exit 7, one document)"
    else
        bad "backup verify refusal: exit $rc"
    fi
fi

# Restore only ever into an isolated directory here; the smoke fixture's own
# index is never replaced.
mkdir -p "$WORK/restored"
if $ATLAS --data-dir "$WORK/restored" --json backup restore "$WORK/smoke.db" --yes > "$DOC" \
       2> /dev/null; then
    check_doc "backup restore (isolated)" --expect command="backup restore" \
              --expect-raw ok=true --expect-raw published=true \
              --expect-raw restored_runtime_state=false --no-control
else
    bad "backup restore into an isolated data directory failed"
fi
# And the restored index answers the same question the source does.
src_repos="$($A --json repo list | tr ',' '\n' | grep -c '"name":' || true)"
dst_repos="$($ATLAS --data-dir "$WORK/restored" --json repo list | tr ',' '\n' \
    | grep -c '"name":' || true)"
if [ "$src_repos" = "$dst_repos" ]; then
    ok "the restored index lists the same repositories"
else
    bad "restored index lists $dst_repos repositories, source lists $src_repos"
fi

# Restore is refused without --yes, and refused for a backup that does not
# verify. Neither may create a database.
if $ATLAS --data-dir "$WORK/never" backup restore "$WORK/smoke.db" > /dev/null 2>&1; then
    bad "backup restore ran without --yes"
elif [ -e "$WORK/never/atlas.db" ]; then
    bad "a refused restore created a database"
else
    ok "backup restore refuses without --yes and creates nothing"
fi

capture --json maintenance plan --older-than 30 \
    && check_doc "maintenance plan" --expect command="maintenance plan" \
                 --expect-raw applied=false --expect-raw prunable_tables=2 --no-control
if $A maintenance prune --older-than 30 > /dev/null 2>&1; then
    bad "maintenance prune ran without --apply"
else
    ok "maintenance prune refuses without --apply"
fi

echo "== sqlite integrity"
if command -v sqlite3 > /dev/null 2>&1; then
    integrity="$(sqlite3 "$DATA/atlas.db" 'PRAGMA integrity_check; PRAGMA foreign_key_check;')"
    if [ "$integrity" = "ok" ]; then
        ok "integrity_check and foreign_key_check"
    else
        bad "integrity: $integrity"
    fi
else
    echo "  skip  sqlite3 CLI not installed; atlas doctor already checked integrity"
fi

printf '\n%d checks, %d failed\n' "$checks" "$fails"
[ "$fails" -eq 0 ]
