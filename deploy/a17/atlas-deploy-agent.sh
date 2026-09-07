#!/bin/bash
# Atlas A17 — the root deploy agent.
#
# Installed (by an operator, never by Atlas) to
# /usr/local/libexec/atlas/atlas-deploy-agent and run by a root systemd oneshot
# unit whenever the spool directory a confirmed deploy's `.req` file appears in
# is watched by a path unit. It applies the stored patch to the operator's own
# tree, builds, tests, installs and restarts the named units as the operator's
# own conf directs — and writes exactly one `results/<deploy_uid>.res` file for
# the daemon to ingest. It never opens the SQLite index; the spool files are its
# whole interface with Atlas.
#
# Every exit path through the per-request loop below writes a result and
# removes the request: a request the agent could not even identify a deploy uid
# for -- including one whose content names a *different* deploy uid than its
# own filename, which is refused the same way and for the same reason (see
# CRITICAL 1 below) -- is renamed "<name>.bad" instead, so a path unit watching
# "*.req" is never re-armed against something this agent already gave up on.
#
# A conf problem (unreadable, malformed, an unrecognised key, a missing
# required key) is refused *before* any request is even looked at: the agent
# exits 3 and touches no request file at all -- not renamed, not deleted, no
# `.res` written. That is deliberate, not an oversight: a conf the agent
# cannot trust is a conf it should not use to decide anything about a specific
# deploy, including which stage to blame it on.
#
# If a result cannot be published (the temp file cannot be created, or the
# publishing rename fails) the request is still removed -- leaving it would
# make the path unit re-run this agent against the same request forever, for
# a failure a retry cannot fix -- but the agent's own exit code is 2, so
# `systemctl status atlas-deploy` shows a failed run even though no `.res`
# exists for that deploy. A CONFIRMED deploy with no result and no visible
# service failure would be silent; this is not silent.
#
# The patch itself is copied into this agent's own tmp directory (0755, so the
# tree's owner can read a file inside it) before APPLY_CHECK/APPLY/reverse run
# as that owner: `requests/` is 0700, daemon-owned in a real deployment (D.3),
# and the owner account has no reason to be able to enter it at all.
#
# Three traps recorded operationally in /opt/atlas/deploy.local.sh and
# deliberately not repeated here:
#   1. `cmake --install` strips RPATH, so a digest comparison against the build
#      tree's own binary never matches even on a correct install. This agent
#      never compares binary digests — only exit codes and `--version` text.
#   2. Running the operator's own build/test AS ROOT re-runs it under a
#      different uid and leaves root-owned files in the operator's build
#      directory, breaking their next ordinary build. BUILD and TEST always run
#      as the conf's `owner` (via runuser, or directly when this agent is
#      already running as that user — which is how the test drives it); only
#      INSTALL and the unit restarts run as this agent's own principal.
#   3. `pipefail` plus `grep -q` silently turns a real command failure into a
#      false "ok" once the exit status is laundered through a pipeline. Nothing
#      here pipes a command's own exit status through anything: every exit code
#      is read directly from `$?` (or bash's own multi-command `&&`/`if`), and
#      commands configured in the conf are executed as an argv — split on
#      whitespace, never handed to a shell — so a configured value can never
#      inject a pipeline, a redirection or a second command.
#
# Usage: atlas-deploy-agent.sh --conf <path-to-deploy.conf>
set -u

# --- stage vocabulary, exactly (ABANDONED is written only by a human) -------
# PREFLIGHT APPLY_CHECK APPLY BUILD TEST INSTALL RESTART VERIFY DONE ABANDONED

RESULT_MAX_BYTES=32768

# Set to 1 the moment a `.res` this agent tried to publish could not be
# written or renamed into place -- checked once at the very end to decide the
# script's own exit code (0 vs 2). Every request is still removed either way
# (IMPORTANT 6 in the fix-round-1 review).
AGENT_HAD_ERROR=0

CURRENT_USER="$(id -un)"

# --- argument parsing --------------------------------------------------------

CONF_PATH=""
while [ $# -gt 0 ]; do
    case "$1" in
        --conf)
            [ $# -ge 2 ] || { printf 'usage: %s --conf <path>\n' "$0" >&2; exit 2; }
            CONF_PATH="$2"
            shift 2
            ;;
        *)
            printf 'usage: %s --conf <path>\n' "$0" >&2
            exit 2
            ;;
    esac
done
[ -n "$CONF_PATH" ] || { printf 'usage: %s --conf <path>\n' "$0" >&2; exit 2; }
[ -r "$CONF_PATH" ] || { printf '%s: cannot read conf: %s\n' "$0" "$CONF_PATH" >&2; exit 3; }

# --- small string helpers ----------------------------------------------------

trim() {
    local s="$1"
    s="${s#"${s%%[![:space:]]*}"}"
    s="${s%"${s##*[![:space:]]}"}"
    printf '%s' "$s"
}

# --- conf parsing: "key = value", unknown key is a hard refusal -------------
#
# This is a different grammar from the request and result files below, which
# are Atlas' own "key value" (single space, no '=') wire format. The conf is
# operator-authored, so it gets the friendlier shell-config shape.

declare -A CONF
while IFS= read -r rawline || [ -n "$rawline" ]; do
    line="$(trim "$rawline")"
    [ -z "$line" ] && continue
    case "$line" in
        '#'*) continue ;;
    esac
    case "$line" in
        *'='*)
            key="$(trim "${line%%=*}")"
            val="$(trim "${line#*=}")"
            ;;
        *)
            printf '%s: malformed conf line (no "="): %s\n' "$0" "$line" >&2
            exit 3
            ;;
    esac
    case "$key" in
        tree|owner|spool|build|test|install|binary|units_system|units_user|ping|dry_run|reverse_on_failure)
            CONF["$key"]="$val"
            ;;
        *)
            printf '%s: unknown conf key: %s\n' "$0" "$key" >&2
            exit 3
            ;;
    esac
done < "$CONF_PATH"

for req_key in tree owner spool build install binary ping; do
    if [ -z "${CONF[$req_key]:-}" ]; then
        printf '%s: missing required conf key: %s\n' "$0" "$req_key" >&2
        exit 3
    fi
done

TREE="${CONF[tree]}"
OWNER="${CONF[owner]}"
SPOOL="${CONF[spool]}"
CONF_BUILD="${CONF[build]}"
CONF_TEST="${CONF[test]:-}"
CONF_INSTALL="${CONF[install]}"
CONF_BINARY="${CONF[binary]}"
CONF_UNITS_SYSTEM="${CONF[units_system]:-}"
CONF_UNITS_USER="${CONF[units_user]:-}"
CONF_PING="${CONF[ping]}"
CONF_DRY_RUN="${CONF[dry_run]:-no}"
CONF_REVERSE_ON_FAILURE="${CONF[reverse_on_failure]:-no}"

case "$CONF_DRY_RUN" in
    yes|no) ;;
    *) printf '%s: dry_run must be yes or no: %s\n' "$0" "$CONF_DRY_RUN" >&2; exit 3 ;;
esac
case "$CONF_REVERSE_ON_FAILURE" in
    yes|no) ;;
    *) printf '%s: reverse_on_failure must be yes or no: %s\n' "$0" "$CONF_REVERSE_ON_FAILURE" >&2; exit 3 ;;
esac

REQUESTS_DIR="$SPOOL/requests"
RESULTS_DIR="$SPOOL/results"
[ -d "$REQUESTS_DIR" ] || { printf '%s: no such directory: %s\n' "$0" "$REQUESTS_DIR" >&2; exit 0; }
# The daemon creates both spool directories at startup (0700, D.3); this is
# only a fallback for a directory that does not yet exist. The daemon (an
# unknown, possibly non-root uid to this agent) still has to be able to read
# what gets written under it, so an explicit 0755 -- not `mkdir -p`'s
# umask-dependent default -- is a deliberate, predictable choice here, not an
# oversight left at whatever mode the fallback produced.
mkdir -p "$RESULTS_DIR" 2>/dev/null || true
chmod 0755 "$RESULTS_DIR" 2>/dev/null || true

OWNER_HOME="$(getent passwd "$OWNER" 2>/dev/null | cut -d: -f6)"
if [ -z "$OWNER_HOME" ]; then
    printf '%s: cannot resolve home directory for owner: %s\n' "$0" "$OWNER" >&2
    exit 3
fi
RUN_PATH="/usr/local/bin:/usr/bin:/bin"

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/atlas-deploy-agent.XXXXXX")" || exit 1
trap 'rm -rf "$TMP_DIR"' EXIT
# `mktemp -d` defaults to 0700, readable only by whoever created it -- which is
# this agent's own principal (root in a real deployment), never the tree's
# owner. The patch is staged here (IMPORTANT 4) precisely so the owner can
# read it even though `requests/` itself is 0700 daemon-owned, so this
# directory has to be traversable by that owner too.
chmod 0755 "$TMP_DIR"

# --- running a command as the tree's owner, or as this agent's own principal -
#
# `owner == $(id -un)` skips runuser entirely -- this is how the test drives
# the agent, and it is also correct on a box where the deploy agent is itself
# already running as the tree's owner. Otherwise every command that touches the
# operator's tree runs through `runuser -u <owner> --`, and HOME/PATH are set
# explicitly on the command rather than trusted from whatever runuser hands
# down: a root oneshot's runuser gives an almost-empty environment, and
# "almost" is not a guarantee.
#
# `cmd...` is executed as a literal argv, word-split from the conf's own
# strings by bash's ordinary unquoted expansion at the call site -- never
# handed to `sh -c`, so a semicolon, a pipe or a backtick in a configured
# command is just an inert argument character, not a second command.
run_owner() {
    local workdir="$1"
    shift
    if [ "$OWNER" = "$CURRENT_USER" ]; then
        ( cd "$workdir" && env HOME="$OWNER_HOME" PATH="$RUN_PATH" "$@" )
    else
        ( cd "$workdir" && runuser -u "$OWNER" -- env HOME="$OWNER_HOME" PATH="$RUN_PATH" "$@" )
    fi
}

run_root() {
    local workdir="$1"
    shift
    ( cd "$workdir" && "$@" )
}

# --- the result text: the stage lines plus the tail of the last command's ---
# --- output, never more than RESULT_MAX_BYTES ------------------------------

LOG=""
LAST_OUTPUT=""

log_line() {
    LOG="${LOG}${1}"$'\n'
}

# label workdir as_owner(yes|no) cmd...
# Captures combined stdout+stderr into LAST_OUTPUT (replacing whatever the
# previous stage left there -- the result carries only the *last* command's
# output, by design, so a successful run stays small and a failing one shows
# the failure rather than an early stage's noise) and logs one summary line.
run_stage() {
    local label="$1" workdir="$2" as_owner="$3"
    shift 3
    local outfile rc
    outfile="$(mktemp "$TMP_DIR/stage.XXXXXX")"
    if [ "$as_owner" = "yes" ]; then
        run_owner "$workdir" "$@" >"$outfile" 2>&1
        rc=$?
    else
        run_root "$workdir" "$@" >"$outfile" 2>&1
        rc=$?
    fi
    LAST_OUTPUT="$(cat "$outfile" 2>/dev/null)"
    rm -f "$outfile"
    if [ "$rc" -eq 0 ]; then
        log_line "$label: ok"
    else
        log_line "$label: exit=$rc"
    fi
    return "$rc"
}

# D.3: "the stage lines and the tail of the last command's output" -- both,
# not one or the other. The stage lines (one short line per stage, at most a
# couple of dozen even across INSTALL/RESTART/VERIFY) are kept *whole* and
# never sacrificed to make room; only LAST_OUTPUT is ever tailed, into
# whatever budget remains after LOG and the separator. Truncating the
# concatenation of the two (as this function used to) truncates from the
# front, which drops the stage lines first and keeps a fragment of whichever
# command happened to be chattiest -- exactly backwards from what an operator
# reading the result wants.
build_result_text() {
    local log_len sep_len budget out body blen
    log_len=$(printf '%s' "$LOG" | wc -c | tr -d '[:space:]')
    if [ -n "$LAST_OUTPUT" ]; then
        local sep=$'\n--- last command output ---\n'
        sep_len=$(printf '%s' "$sep" | wc -c | tr -d '[:space:]')
        budget=$((RESULT_MAX_BYTES - log_len - sep_len - 1)) # -1: the trailing newline added below
        [ "$budget" -lt 0 ] && budget=0
        out="$LAST_OUTPUT"
        local outlen
        outlen=$(printf '%s' "$out" | wc -c | tr -d '[:space:]')
        if [ "$outlen" -gt "$budget" ]; then
            out=$(printf '%s' "$out" | tail -c "$budget")
        fi
        body="${LOG}${sep}${out}"$'\n'
    else
        body="$LOG"
    fi
    blen=$(printf '%s' "$body" | wc -c | tr -d '[:space:]')
    # Safety net only: LOG itself is never expected to approach the bound (a
    # few dozen short lines), but a caller that broke that assumption must
    # still never publish more than RESULT_MAX_BYTES.
    if [ "$blen" -gt "$RESULT_MAX_BYTES" ]; then
        body=$(printf '%s' "$body" | tail -c "$RESULT_MAX_BYTES")
        blen=$(printf '%s' "$body" | wc -c | tr -d '[:space:]')
    fi
    RESULT_BODY="$body"
    RESULT_BODY_BYTES="$blen"
}

# deploy outcome stage dry_run head_before head_after tree_dirty_before
# installed_version units rollback
#
# A failure here (the temp file cannot be created, or the publishing rename
# fails) sets AGENT_HAD_ERROR rather than being swallowed: every caller still
# removes the request either way (a retry cannot fix a result-directory
# problem), but the script's own exit code at the very end reflects it, so a
# CONFIRMED deploy with no `.res` is not also a silently-successful run.
write_result() {
    local d="$1" outcome="$2" stage="$3" dry="$4" hb="$5" ha="$6" dirty="$7" ver="$8" units="$9" roll="${10}"
    build_result_text
    local tmp
    tmp="$(mktemp "$RESULTS_DIR/.tmp.XXXXXX")" || {
        printf '%s: cannot create temp result file for deploy %s\n' "$0" "$d" >&2
        AGENT_HAD_ERROR=1
        return 1
    }
    {
        printf 'atlas-deploy-result 1\n'
        printf 'deploy %s\n' "$d"
        printf 'outcome %s\n' "$outcome"
        printf 'stage %s\n' "$stage"
        printf 'dry_run %s\n' "$dry"
        printf 'head_before %s\n' "$hb"
        printf 'head_after %s\n' "$ha"
        printf 'tree_dirty_before %s\n' "$dirty"
        printf 'installed_version %s\n' "$ver"
        printf 'units %s\n' "$units"
        printf 'rollback %s\n' "$roll"
        printf 'text_bytes %s\n' "$RESULT_BODY_BYTES"
        printf -- '--\n'
        printf '%s' "$RESULT_BODY"
    } > "$tmp"
    chmod 0644 "$tmp"
    if ! mv -f "$tmp" "$RESULTS_DIR/$d.res"; then
        printf '%s: cannot publish result for deploy %s\n' "$0" "$d" >&2
        rm -f "$tmp"
        AGENT_HAD_ERROR=1
        return 1
    fi
    return 0
}

# --- units: space-separated names (units_system) or "owner:unit" pairs -----
# --- (units_user); both are empty in every case this agent's own test drives -

restart_units() {
    local rc=0 u
    for u in $CONF_UNITS_SYSTEM; do
        run_stage "RESTART($u)" "/" "no" systemctl restart "$u" || rc=1
    done
    for pair in $CONF_UNITS_USER; do
        local u_owner="${pair%%:*}" u_unit="${pair#*:}"
        local u_home u_uid
        u_home="$(getent passwd "$u_owner" 2>/dev/null | cut -d: -f6)"
        u_uid="$(id -u "$u_owner" 2>/dev/null)"
        if [ -z "$u_home" ] || [ -z "$u_uid" ]; then
            log_line "RESTART($u_unit): cannot resolve user $u_owner"
            rc=1
            continue
        fi
        if [ "$u_owner" = "$CURRENT_USER" ]; then
            env HOME="$u_home" PATH="$RUN_PATH" XDG_RUNTIME_DIR="/run/user/$u_uid" \
                systemctl --user restart "$u_unit" >"$TMP_DIR/ru.$$" 2>&1
        else
            runuser -u "$u_owner" -- env HOME="$u_home" PATH="$RUN_PATH" \
                XDG_RUNTIME_DIR="/run/user/$u_uid" systemctl --user restart "$u_unit" >"$TMP_DIR/ru.$$" 2>&1
        fi
        local urc=$?
        LAST_OUTPUT="$(cat "$TMP_DIR/ru.$$" 2>/dev/null)"
        rm -f "$TMP_DIR/ru.$$"
        if [ "$urc" -eq 0 ]; then
            log_line "RESTART($u_unit): ok"
        else
            log_line "RESTART($u_unit): exit=$urc"
            rc=1
        fi
    done
    return "$rc"
}

# True (0) when a MainPID's own executable was unlinked while still running --
# `readlink /proc/<pid>/exe` then ends in the literal " (deleted)" suffix the
# kernel appends, which is D.4's own check. An empty or zero pid (a unit type
# with no single main process, or one this agent could not resolve) is never
# treated as stale -- absence of an answer is not evidence of a deleted exe,
# A9.2.2's rule one layer out from Atlas' own database.
mainpid_stale() {
    local pid="$1"
    if [ -z "$pid" ] || [ "$pid" = "0" ]; then
        return 1
    fi
    case "$(readlink "/proc/$pid/exe" 2>/dev/null)" in
        *' (deleted)') return 0 ;;
        *) return 1 ;;
    esac
}

verify_units() {
    local rc=0 u
    UNITS_LINE=""
    for u in $CONF_UNITS_SYSTEM; do
        local state pid
        state="$(systemctl is-active "$u" 2>/dev/null)"
        [ -z "$state" ] && state="unknown"
        pid="$(systemctl show -p MainPID --value "$u" 2>/dev/null)"
        if [ "$state" != "active" ]; then
            rc=1
        elif mainpid_stale "$pid"; then
            state="deleted-exe"
            rc=1
        fi
        UNITS_LINE="${UNITS_LINE}${UNITS_LINE:+ }${u}=${state}"
    done
    for pair in $CONF_UNITS_USER; do
        local u_owner="${pair%%:*}" u_unit="${pair#*:}"
        local u_home u_uid state pid
        u_home="$(getent passwd "$u_owner" 2>/dev/null | cut -d: -f6)"
        u_uid="$(id -u "$u_owner" 2>/dev/null)"
        if [ -z "$u_home" ] || [ -z "$u_uid" ]; then
            UNITS_LINE="${UNITS_LINE}${UNITS_LINE:+ }${u_unit}=unknown"
            rc=1
            continue
        fi
        if [ "$u_owner" = "$CURRENT_USER" ]; then
            state="$(env HOME="$u_home" PATH="$RUN_PATH" XDG_RUNTIME_DIR="/run/user/$u_uid" \
                systemctl --user is-active "$u_unit" 2>/dev/null)"
            pid="$(env HOME="$u_home" PATH="$RUN_PATH" XDG_RUNTIME_DIR="/run/user/$u_uid" \
                systemctl --user show -p MainPID --value "$u_unit" 2>/dev/null)"
        else
            state="$(runuser -u "$u_owner" -- env HOME="$u_home" PATH="$RUN_PATH" \
                XDG_RUNTIME_DIR="/run/user/$u_uid" systemctl --user is-active "$u_unit" 2>/dev/null)"
            pid="$(runuser -u "$u_owner" -- env HOME="$u_home" PATH="$RUN_PATH" \
                XDG_RUNTIME_DIR="/run/user/$u_uid" systemctl --user show -p MainPID --value "$u_unit" 2>/dev/null)"
        fi
        [ -z "$state" ] && state="unknown"
        if [ "$state" != "active" ]; then
            rc=1
        elif mainpid_stale "$pid"; then
            state="deleted-exe"
            rc=1
        fi
        UNITS_LINE="${UNITS_LINE}${UNITS_LINE:+ }${u_unit}=${state}"
    done
    return "$rc"
}

# --- one request, start to finish -------------------------------------------

process_request() {
    local reqfile="$1"
    local base="${reqfile##*/}"
    base="${base%.req}"
    local patchfile="$REQUESTS_DIR/$base.patch"

    # Parse the request: Atlas' own "key value" wire format (D.3), not the
    # conf's "key = value" shape above. The first line found that starts with
    # "deploy " names the deploy uid; if none exists at all, this agent cannot
    # even name the result it would write, so the request is quarantined
    # instead -- renamed so the path unit is never re-armed against it.
    declare -A REQ
    local unknown_key="" line lineno=0 header=""
    while IFS= read -r line || [ -n "$line" ]; do
        lineno=$((lineno + 1))
        if [ "$lineno" -eq 1 ]; then
            header="$line"
            continue
        fi
        [ -z "$line" ] && continue
        case "$line" in
            *' '*)
                local key="${line%% *}" val="${line#* }"
                ;;
            *)
                local key="$line" val=""
                ;;
        esac
        case "$key" in
            deploy|job|repo_root|base_commit|patch_sha256|patch_bytes|confirmed_by|confirmed_at)
                REQ["$key"]="$val"
                ;;
            *)
                unknown_key="$key"
                ;;
        esac
    done < "$reqfile"

    # CRITICAL 1 (fix round 1): the request's *filename* -- a real path this
    # agent globbed off disk, which can never itself contain "/" -- is the
    # only thing $d in write_result's "$RESULTS_DIR/$d.res" may safely come
    # from. Requiring the parsed "deploy" value to equal that filename's own
    # base closes a path traversal (a content-supplied "deploy ../../x" value
    # can no longer reach write_result at all) without a regex, and as a
    # second effect refuses a request whose content simply names a different
    # deploy than the one the daemon filed it under. Either way the request
    # is quarantined exactly as the "no deploy uid at all" case already was:
    # this agent cannot trust what result filename to write, so it writes
    # none and renames the request instead.
    local deploy_uid="${REQ[deploy]:-}"
    if [ -z "$deploy_uid" ] || [ "$deploy_uid" != "$base" ]; then
        mv -f "$reqfile" "$REQUESTS_DIR/$base.bad"
        rm -f "$patchfile"
        return 0
    fi

    LOG=""
    LAST_OUTPUT=""
    UNITS_LINE=""

    # --- PREFLIGHT: cheap structural checks first, so a refusal here never
    # touches git at all and the tree stays provably untouched. ---------------

    preflight_fail() {
        log_line "PREFLIGHT: $1"
        write_result "$deploy_uid" FAILED PREFLIGHT "$CONF_DRY_RUN" "" "" "" "" "" none
        rm -f "$reqfile" "$patchfile"
    }

    if [ "$header" != "atlas-deploy-request 1" ]; then
        preflight_fail "unrecognised request header"
        return 0
    fi
    if [ -n "$unknown_key" ]; then
        preflight_fail "unknown request key: $unknown_key"
        return 0
    fi
    for k in deploy job repo_root base_commit patch_sha256 patch_bytes confirmed_by confirmed_at; do
        if [ -z "${REQ[$k]:-}" ]; then
            preflight_fail "missing request key: $k"
            return 0
        fi
    done
    if [ -z "$CONF_TEST" ]; then
        preflight_fail "test is not configured; the operator must set exactly one of the two documented test commands in deploy.conf"
        return 0
    fi
    if [ "${REQ[repo_root]}" != "$TREE" ]; then
        preflight_fail "repo_root does not match the configured tree"
        return 0
    fi
    if [ ! -f "$patchfile" ]; then
        preflight_fail "missing patch file: $base.patch"
        return 0
    fi
    local actual_bytes actual_sha256
    actual_bytes="$(wc -c < "$patchfile" | tr -d '[:space:]')"
    actual_sha256="$(sha256sum "$patchfile" | cut -d' ' -f1)"
    case "${REQ[patch_bytes]}" in
        ''|*[!0-9]*)
            preflight_fail "malformed patch_bytes"
            return 0
            ;;
    esac
    if [ "${REQ[patch_bytes]}" != "$actual_bytes" ]; then
        preflight_fail "patch_bytes does not match the stored patch (digest mismatch)"
        return 0
    fi
    if [ "${REQ[patch_sha256]}" != "$actual_sha256" ]; then
        preflight_fail "patch_sha256 does not match the stored patch (digest mismatch)"
        return 0
    fi
    if [ ! -e "$TREE/.git" ]; then
        preflight_fail "tree is not a git repository: $TREE"
        return 0
    fi

    # IMPORTANT 4 (fix round 1): `requests/` is 0700 daemon-owned in a real
    # deployment (D.3); the patch's *integrity* was just proved above against
    # the digest the confirmed deploy row carries, but the tree's *owner*
    # account -- which is who runs APPLY_CHECK/APPLY/REVERSE below -- has no
    # reason to be able to traverse into `requests/` at all and generally
    # cannot. The verified bytes are copied into this agent's own 0755 tmp
    # directory, at 0644, and every stage from here on reads that copy, never
    # the original in `requests/`.
    local work_patch="$TMP_DIR/$base.patch"
    if ! cp "$patchfile" "$work_patch" 2>/dev/null; then
        preflight_fail "cannot stage the patch for application"
        return 0
    fi
    chmod 0644 "$work_patch"
    log_line "PREFLIGHT: staged patch at $work_patch mode $(stat -c %a "$work_patch" 2>/dev/null)"

    # IMPORTANT 3 (fix round 1): both reads below used to run directly as this
    # agent's own principal (root in a real deployment) rather than through
    # run_owner like every other git call against the tree. `git status`
    # reads (and, depending on config, can execute) `core.fsmonitor` from the
    # repository's own config -- a value the tree's owner controls, not this
    # agent -- so running it as root would let that owner's config run an
    # arbitrary command as root. `-c core.fsmonitor=false` disables the
    # feature outright, on every git call this agent makes, and `run_owner`
    # additionally drops these reads to the tree's own owner rather than root,
    # which is the same reason APPLY_CHECK below has always run as owner
    # (docs/git-safety.md; CLAUDE.md's git-safety hard rules).
    local head_before head_after tree_dirty_before
    head_before="$(run_owner "$TREE" git -C "$TREE" -c safe.directory="$TREE" \
        -c core.fsmonitor=false rev-parse HEAD 2>/dev/null)"
    if [ -z "$head_before" ]; then
        preflight_fail "cannot resolve HEAD in $TREE"
        return 0
    fi
    if [ -n "$(run_owner "$TREE" git -C "$TREE" -c safe.directory="$TREE" \
            -c core.fsmonitor=false status --porcelain 2>/dev/null)" ]; then
        tree_dirty_before="yes"
    else
        tree_dirty_before="no"
    fi
    log_line "PREFLIGHT: ok (base_commit=${REQ[base_commit]}, head=$head_before)"
    head_after="$head_before" # this agent never commits; HEAD never moves

    # --- APPLY_CHECK -----------------------------------------------------------

    if ! run_stage "APPLY_CHECK" "$TREE" "yes" git -C "$TREE" -c safe.directory="$TREE" \
            -c core.fsmonitor=false apply --check "$work_patch"; then
        write_result "$deploy_uid" FAILED APPLY_CHECK "$CONF_DRY_RUN" "$head_before" "$head_after" \
            "$tree_dirty_before" "" "" none
        rm -f "$reqfile" "$patchfile"
        return 0
    fi

    # --- APPLY -------------------------------------------------------------

    if ! run_stage "APPLY" "$TREE" "yes" git -C "$TREE" -c safe.directory="$TREE" \
            -c core.fsmonitor=false apply "$work_patch"; then
        write_result "$deploy_uid" FAILED APPLY "$CONF_DRY_RUN" "$head_before" "$head_after" \
            "$tree_dirty_before" "" "" none
        rm -f "$reqfile" "$patchfile"
        return 0
    fi

    # A patch applied and then a later stage failing is answered by reversing
    # exactly the bytes this agent added a moment ago -- never a reset, a
    # checkout or a clean, which could discard the operator's own uncommitted
    # work alongside it.
    reverse_patch() {
        if run_stage "REVERSE" "$TREE" "yes" git -C "$TREE" -c safe.directory="$TREE" \
                -c core.fsmonitor=false apply -R "$work_patch"; then
            ROLLBACK="patch"
        else
            log_line "REVERSE: failed to reverse the patch; the tree is left patched"
            ROLLBACK="none"
        fi
    }

    # --- BUILD ---------------------------------------------------------------

    if ! run_stage "BUILD" "$TREE" "yes" $CONF_BUILD; then
        local roll="none"
        if [ "$CONF_DRY_RUN" = "yes" ] || [ "$CONF_REVERSE_ON_FAILURE" = "yes" ]; then
            reverse_patch
            roll="$ROLLBACK"
        fi
        write_result "$deploy_uid" FAILED BUILD "$CONF_DRY_RUN" "$head_before" "$head_after" \
            "$tree_dirty_before" "" "" "$roll"
        rm -f "$reqfile" "$patchfile"
        return 0
    fi

    # --- TEST ------------------------------------------------------------------

    if ! run_stage "TEST" "$TREE" "yes" $CONF_TEST; then
        local roll="none"
        if [ "$CONF_DRY_RUN" = "yes" ] || [ "$CONF_REVERSE_ON_FAILURE" = "yes" ]; then
            reverse_patch
            roll="$ROLLBACK"
        fi
        write_result "$deploy_uid" FAILED TEST "$CONF_DRY_RUN" "$head_before" "$head_after" \
            "$tree_dirty_before" "" "" "$roll"
        rm -f "$reqfile" "$patchfile"
        return 0
    fi

    if [ "$CONF_DRY_RUN" = "yes" ]; then
        # A dry run proves apply+build+test succeed and never leaves the tree
        # patched -- that is the whole point of asking for one.
        reverse_patch
        write_result "$deploy_uid" SUCCEEDED TEST "yes" "$head_before" "$head_after" \
            "$tree_dirty_before" "" "" "$ROLLBACK"
        rm -f "$reqfile" "$patchfile"
        return 0
    fi

    # --- INSTALL -----------------------------------------------------------
    #
    # INSTALL, RESTART and VERIFY run as this agent's own principal (root in a
    # real deployment), never as the tree's owner: they touch the installed
    # binary and system units, not the operator's tree.

    local installed_version=""
    local prevfile="${CONF_BINARY}.prev"
    if [ -e "$CONF_BINARY" ]; then
        cp -p "$CONF_BINARY" "$prevfile" 2>/dev/null
    fi

    # IMPORTANT 5 (fix round 1): `rollback binary` used to be written
    # unconditionally, whether or not `.prev` existed or the restoring `cp`
    # actually succeeded, and `installed_version` carried the *new* binary's
    # string even when RESTART/VERIFY failed after the swap -- a rollback the
    # result claimed happened even when it did not, and a version that no
    # longer matched what was actually on disk. Now: `rollback binary` is
    # written only when the restore is confirmed to have happened; otherwise
    # `rollback none` plus a stage line naming why (no `.prev`, or the `cp`
    # itself failed); and `installed_version` is re-read from whatever is
    # actually installed *after* the restore attempt, never carried over from
    # before it -- empty when nothing was restored.
    restore_binary_and_fail() {
        local failed_stage="$1"
        # The recovery below (restore .prev, restart again) runs its own
        # commands through the same LAST_OUTPUT slot run_stage/restart_units
        # write to -- save the *failing* stage's own output first, so the
        # operator's result carries why INSTALL/RESTART/VERIFY failed, not the
        # recovery attempt's own (usually silent) output.
        local original_output="$LAST_OUTPUT"
        local roll="none"
        local restored_version=""
        if [ ! -e "$prevfile" ]; then
            log_line "$failed_stage: no previous binary at $prevfile to restore"
        elif ! cp -p "$prevfile" "$CONF_BINARY" 2>/dev/null; then
            log_line "$failed_stage: restoring $prevfile over $CONF_BINARY failed"
        else
            roll="binary"
            if [ -x "$CONF_BINARY" ]; then
                restored_version="$(timeout 20 "$CONF_BINARY" --version 2>&1 | head -n1)"
            fi
        fi
        restart_units
        LAST_OUTPUT="$original_output"
        write_result "$deploy_uid" FAILED "$failed_stage" "$CONF_DRY_RUN" "$head_before" "$head_after" \
            "$tree_dirty_before" "$restored_version" "${UNITS_LINE:-}" "$roll"
        rm -f "$reqfile" "$patchfile"
    }

    if ! run_stage "INSTALL" "$TREE" "no" $CONF_INSTALL; then
        restore_binary_and_fail INSTALL
        return 0
    fi
    if [ -x "$CONF_BINARY" ]; then
        installed_version="$(timeout 20 "$CONF_BINARY" --version 2>&1 | head -n1)"
    fi

    # --- RESTART -------------------------------------------------------------

    if ! restart_units; then
        restore_binary_and_fail RESTART
        return 0
    fi

    # --- VERIFY --------------------------------------------------------------

    if ! verify_units; then
        restore_binary_and_fail VERIFY
        return 0
    fi
    if ! run_stage "VERIFY(ping)" "$TREE" "yes" timeout 40 $CONF_PING; then
        restore_binary_and_fail VERIFY
        return 0
    fi

    # --- DONE ------------------------------------------------------------------

    log_line "DONE: ok"
    write_result "$deploy_uid" SUCCEEDED DONE "no" "$head_before" "$head_after" \
        "$tree_dirty_before" "$installed_version" "${UNITS_LINE:-}" none
    rm -f "$reqfile" "$patchfile"
    return 0
}

# --- the loop: every *.req present when this agent starts is one deploy ----

shopt -s nullglob
for reqfile in "$REQUESTS_DIR"/*.req; do
    process_request "$reqfile"
done

if [ "$AGENT_HAD_ERROR" -ne 0 ]; then
    exit 2
fi
exit 0
