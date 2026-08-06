#!/bin/sh
# Atlas - find the atlas executable from inside an installed plugin.
# Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
#
# Sourced by atlas-hook and atlas-mcp. Sets ATLAS_EXE, or leaves it empty.
#
# This exists because a plugin cannot hold a stable path to a binary outside
# itself. Claude copies an installed plugin into a cache directory whose path
# changes on every update, so a relative path from here to the Atlas executable
# is not stable, and a symlink planted next to the plugin does not survive being
# re-copied. Nor may a compiled binary be committed into the plugin: it would be
# a build artefact in a source tree and would be wrong for every platform but
# one.
#
# So the search is, in order:
#
#   1. $ATLAS_BIN            an explicit override, for tests and odd layouts
#   2. the integration record written by `atlas integrate claude install`,
#      which lives in the user's own config directory and is not rewritten by
#      anything else
#   3. PATH                  the ordinary case after `make install`
#   4. two well-known prefixes, for a PATH that a desktop session did not
#      inherit — which is common when Claude is launched from a GUI
#
# POSIX sh, no bashisms, no subprocess beyond `command -v`. This runs on every
# hook invocation, so it has to be cheap.

atlas_resolve() {
    ATLAS_EXE=""

    if [ -n "${ATLAS_BIN:-}" ] && [ -x "${ATLAS_BIN}" ]; then
        ATLAS_EXE="${ATLAS_BIN}"
        return 0
    fi

    _conf="${XDG_CONFIG_HOME:-${HOME:-}/.config}/atlas/claude-integration.conf"
    if [ -r "${_conf}" ]; then
        # One `key=value` line, read with parameter expansion only. The file is
        # written by Atlas, but a config file the shell is asked to interpret is
        # a config file that runs whatever anyone can write into it, so this
        # never hands its contents back to the shell as code.
        _line=$(grep '^atlas_executable=' "${_conf}" 2>/dev/null | head -n 1)
        _candidate="${_line#atlas_executable=}"
        if [ -n "${_candidate}" ] && [ "${_candidate}" != "${_line}" ] && [ -x "${_candidate}" ]; then
            ATLAS_EXE="${_candidate}"
            return 0
        fi
    fi

    _candidate=$(command -v atlas 2>/dev/null)
    if [ -n "${_candidate}" ] && [ -x "${_candidate}" ]; then
        ATLAS_EXE="${_candidate}"
        return 0
    fi

    for _candidate in "${HOME:-}/.local/bin/atlas" /usr/local/bin/atlas /usr/bin/atlas; do
        if [ -x "${_candidate}" ]; then
            ATLAS_EXE="${_candidate}"
            return 0
        fi
    done

    return 1
}
