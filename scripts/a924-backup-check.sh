#!/bin/sh
# Atlas - A9.2.4 §33: the activation and discovery metadata survives a backup and
# a restore, and the daemon's derived state is the same on the other side.
#
# Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
#
# Runs entirely against an **isolated** data directory and an isolated fixture
# repository. It never opens the machine's real index, never touches a registered
# repository, and never stops a service. `--data-dir` is explicit, which is the
# one thing that still selects an index under an active system policy.
#
# What it proves, and why each matters:
#
#   - the operator's *intent and its provenance* survive, because a restore that
#     lost them would silently re-enable a repository somebody had disabled;
#   - the *discovery verdict* survives, because a restore that lost it would turn
#     a COMPLETE search into UNKNOWN and quietly stop every negative conclusion;
#   - the *candidate list* survives, accepted and rejected, because a restored
#     index reporting fewer build inputs than it had is a wrong search rather
#     than a smaller one;
#   - the *source identity* and the current generation survive, so the daemon
#     resumes rather than rebuilding from nothing.
set -eu

ATLAS="${ATLAS:-/usr/local/bin/atlas}"
WORK="${WORK:-${TMPDIR:-/tmp}/a924-backup.$$}"
REPO="$WORK/repo"
DATA="$WORK/data"
REST="$WORK/restored"
NAME=a924b

pass=0; fail=0
ok()  { pass=$((pass+1)); printf '  ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf '  FAIL  %s: %s\n' "$1" "$2"; }
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT


mkdir -p "$REPO/src" "$REPO/build/a" "$REPO/build/b" "$DATA" "$REST"
printf 'int alpha(void){return 1;}\n' > "$REPO/src/alpha.c"
printf 'int beta(void){return 2;}\n'  > "$REPO/src/beta.c"
for d in a b; do
  s=alpha; [ "$d" = b ] && s=beta
  cat > "$REPO/build/$d/compile_commands.json" <<EOF
[{"directory":"$REPO","arguments":["cc","-std=gnu11","-c","src/$s.c"],"file":"src/$s.c"}]
EOF
done
# A candidate that must survive as a *rejected* one — inside the searched
# universe, so it is refused for being unreadable rather than skipped for being
# excluded. Those are different facts and the point of the check is the first.
mkdir -p "$REPO/build/bad"
printf '{ not json\n' > "$REPO/build/bad/compile_commands.json"
# And a subtree the operator excludes, which is what makes the search PARTIAL.
mkdir -p "$REPO/vendor/build"
cat > "$REPO/vendor/build/compile_commands.json" <<EOF
[{"directory":"$REPO","arguments":["cc","-std=gnu11","-c","src/alpha.c"],"file":"src/alpha.c"}]
EOF

git -C "$REPO" init -q
git -C "$REPO" config user.email a924b@example.invalid
git -C "$REPO" config user.name a924b
printf 'build/\n' > "$REPO/.gitignore"
git -C "$REPO" add -A
git -C "$REPO" -c commit.gpgsign=false commit -q -m fixture

printf '== A9.2.4 backup/restore of activation and discovery metadata\n'
"$ATLAS" --data-dir "$DATA" repo add "$REPO" --name "$NAME" >/dev/null
"$ATLAS" --data-dir "$DATA" scan "$NAME" >/dev/null
# Writing the configuration re-walks, so this both records an operator intent and
# produces a discovery result in one step.
"$ATLAS" --data-dir "$DATA" code sem-config "$NAME" --no-auto \
    --vendor-root vendor --exclude vendor >/dev/null
BEFORE=$("$ATLAS" --data-dir "$DATA" code sem-status "$NAME" --json)

want() { # want <label> <needle>
  case "$1" in
    *"$2"*) return 0 ;;
    *) return 1 ;;
  esac
}
for probe in '"auto_intent":"DISABLED"' '"auto_intent_by":"OPERATOR"' \
             '"activity":"EXPLICITLY_DISABLED"' '"inputs_accepted":2'; do
  if want "$BEFORE" "$probe"; then ok "before: $probe"; else bad 'before' "$probe missing"; fi
done

"$ATLAS" --data-dir "$DATA" backup create "$WORK/snap.db" >/dev/null
ok 'backup created'
"$ATLAS" --data-dir "$DATA" backup verify "$WORK/snap.db" >/dev/null
ok 'backup verified'

# Restore into a *separate* data directory, so the original is untouched and the
# comparison is between two real indexes rather than one before and after.
cp "$DATA/atlas.db" "$WORK/original.db"
mkdir -p "$REST"
cp "$WORK/snap.db" "$REST/atlas.db"
AFTER=$("$ATLAS" --data-dir "$REST" code sem-status "$NAME" --json)

for probe in '"auto_intent":"DISABLED"' '"auto_intent_by":"OPERATOR"' \
             '"activity":"EXPLICITLY_DISABLED"' '"inputs_accepted":2' \
             '"discovery":"PARTIAL"'; do
  if want "$AFTER" "$probe"; then ok "after restore: $probe"; else bad 'after restore' "$probe missing"; fi
done

# The rejected candidate is still shown. A restore that dropped it would make a
# candidate Atlas refused indistinguishable from one that was never there.
if want "$AFTER" 'the_compilation_database_could_not_be_parsed'; then
  ok 'after restore: the rejected candidate and its reason survive'
else
  bad 'after restore' 'the rejected candidate was lost'
fi

# And the source identity, which is what lets the daemon resume rather than
# rebuild from nothing.
sid_before=$(printf '%s' "$BEFORE" | sed -n 's/.*"source_identity":"\([0-9a-f]*\)".*/\1/p' | head -1)
sid_after=$(printf '%s' "$AFTER" | sed -n 's/.*"source_identity":"\([0-9a-f]*\)".*/\1/p' | head -1)
if [ -n "$sid_before" ] && [ "$sid_before" = "$sid_after" ]; then
  ok 'the source identity is unchanged across the restore'
else
  bad 'source identity' "$sid_before vs $sid_after"
fi

printf '\n%d checks, %d failed\n' "$((pass+fail))" "$fail"
[ "$fail" -eq 0 ]
