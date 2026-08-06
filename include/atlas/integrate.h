/* Atlas - one-time integration with an AI client.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * `atlas integrate claude ...` is the *Atlas* half of the one-time setup. It
 * writes exactly one file — a small configuration record naming this Atlas
 * executable — and it prints the commands the user runs for the other half.
 *
 * What it deliberately does not do:
 *
 *   - it does not edit `~/.claude` or any Claude-owned file. Claude owns its
 *     configuration and has documented commands for changing it; a tool that
 *     hand-edits another tool's state is a tool that breaks when that state's
 *     format changes, silently and in somebody else's product;
 *   - it does not enable or start a systemd unit. `atlas service install`
 *     writes a unit and stops, and this is no different;
 *   - it does not run `claude`. Validation of the plugin is a thing the user
 *     runs, with output they can see;
 *   - `uninstall` never touches the index. The database is the thing that took
 *     time to build and it survives an uninstall unless somebody deliberately
 *     removes the data directory.
 *
 * The one real problem this solves is finding the Atlas executable from inside
 * an installed plugin. Claude copies a plugin into a cache directory whose path
 * changes on every update, so a relative path from the plugin to the binary is
 * not stable and a symlink planted next to it does not survive. The
 * configuration file this writes is what the plugin's launcher reads, and it
 * lives in the user's own config directory where nothing else rewrites it.
 */
#ifndef ATLAS_INTEGRATE_H
#define ATLAS_INTEGRATE_H

#include <stdbool.h>
#include <stdint.h>

#include "atlas/buf.h"
#include "atlas/error.h"

/* Where the plugin directory was found, so `doctor` can say. */
typedef enum atlas_plugin_source {
    ATLAS_PLUGIN_SRC_NONE = 0,
    ATLAS_PLUGIN_SRC_ENV,       /* ATLAS_CLAUDE_PLUGIN_DIR */
    ATLAS_PLUGIN_SRC_CONFIG,    /* recorded by a previous install */
    ATLAS_PLUGIN_SRC_INSTALLED, /* <prefix>/share/atlas/claude-plugin */
    ATLAS_PLUGIN_SRC_SOURCE     /* a build tree, for development */
} atlas_plugin_source;

const char *atlas_plugin_source_name(atlas_plugin_source s);

/* How the plugin is loaded, as far as Claude's own configuration says.
 *
 * These are distinct because they fail differently and are fixed differently. A
 * plugin that is installed but disabled looks exactly like one that is missing
 * from inside a session, and telling somebody to reinstall it when they only
 * need to enable it wastes their afternoon. */
typedef enum atlas_claude_state {
    /* Claude has never been told about this plugin. Only `--plugin-dir` would
     * load it, and only for that one session. */
    ATLAS_CLAUDE_NOT_INSTALLED = 0,
    /* Installed from a marketplace at some scope, and enabled. Ordinary
     * sessions load it with no flags. This is the target state. */
    ATLAS_CLAUDE_INSTALLED_ENABLED,
    /* Installed, and explicitly turned off. `claude plugin enable atlas` fixes
     * it; reinstalling does not. */
    ATLAS_CLAUDE_INSTALLED_DISABLED,
    /* A development plugin: the source tree is present and usable with
     * `--plugin-dir`, but nothing is permanently installed. */
    ATLAS_CLAUDE_DEVELOPMENT,
    /* Claude's configuration directory could not be read at all, so nothing
     * here is known. Reported as unknown rather than guessed as absent. */
    ATLAS_CLAUDE_UNKNOWN
} atlas_claude_state;

const char *atlas_claude_state_name(atlas_claude_state s);

typedef struct atlas_integrate_report {
    /* Identity of the pieces. */
    atlas_buf exe;         /* this Atlas executable, absolute */
    atlas_buf plugin_dir;  /* the plugin directory, absolute */
    atlas_buf marketplace_dir; /* the directory holding marketplace.json */
    atlas_plugin_source plugin_source;
    atlas_buf config_path; /* the launcher's configuration record */

    /* What Claude's own configuration says, read-only. */
    atlas_claude_state claude_state;
    atlas_buf claude_config_dir;  /* $CLAUDE_CONFIG_DIR or ~/.claude */
    atlas_buf installed_id;       /* e.g. "atlas@atlas-local", when installed */
    atlas_buf installed_scope;    /* user | project | local */
    atlas_buf installed_path;     /* the cache directory Claude copied it into */
    bool marketplace_registered;  /* the Atlas marketplace is known to Claude */
    bool marketplace_ok;          /* marketplace.json is present and parses */

    /* What was found. Each is checked by reading the file, not by stat alone:
     * a manifest that exists and does not parse is worse than a missing one,
     * because it looks installed. */
    bool plugin_found;
    bool manifest_ok;
    bool hooks_ok;
    bool mcp_ok;
    bool skill_ok;
    bool launcher_ok;
    int64_t hook_events;    /* events the plugin configures */
    int64_t mcp_tools;      /* tools this binary exposes */

    /* Runtime readiness. */
    bool config_present;
    bool daemon_reachable;
    atlas_buf socket_path;
    atlas_buf data_dir;
    /* Whether an index exists. Reported, never created: see `atlas doctor`. */
    bool index_present;

    /* An in-process MCP handshake, so `doctor` proves the server answers rather
     * than proving the file exists. Needs no daemon and no Claude. */
    bool mcp_selftest_ok;
    atlas_buf mcp_selftest_detail;

    /* What install/uninstall did. */
    bool wrote_config;
    bool removed_config;

    bool ok;
    atlas_buf problems; /* newline-separated; empty when ok */
} atlas_integrate_report;

void atlas_integrate_report_init(atlas_integrate_report *r);
void atlas_integrate_report_free(atlas_integrate_report *r);

/* Resolves the plugin directory. Search order: ATLAS_CLAUDE_PLUGIN_DIR, then a
 * previously recorded one, then the installed location beside this executable,
 * then a source tree. Reports which was used, and reports "none" rather than
 * guessing when none of them holds a manifest. */
atlas_status atlas_integrate_find_plugin(const char *exe, atlas_buf *out,
                                         atlas_plugin_source *source_out, atlas_err *err);

/* The directory holding `.claude-plugin/marketplace.json`, which is the plugin
 * directory's parent in every shipped layout. This is what a user passes to
 * `claude plugin marketplace add`. */
atlas_status atlas_integrate_find_marketplace(const char *plugin_dir, atlas_buf *out,
                                              atlas_err *err);

/* The path of the launcher's configuration record:
 * $XDG_CONFIG_HOME/atlas/claude-integration.conf, or ~/.config/... */
atlas_status atlas_integrate_config_path(atlas_buf *out, atlas_err *err);

/* Inspects everything and fills the report. Changes nothing. */
atlas_status atlas_integrate_claude_doctor(atlas_integrate_report *out, atlas_err *err);

/* Writes the configuration record, and nothing else. Idempotent: an identical
 * record is left alone, so a repeated install is a no-op rather than a rewrite. */
atlas_status atlas_integrate_claude_install(atlas_integrate_report *out, atlas_err *err);

/* Removes the configuration record, and only that. The Atlas index is never
 * touched: it is the thing that took time to build. */
atlas_status atlas_integrate_claude_uninstall(atlas_integrate_report *out, atlas_err *err);

/* The one-time commands a user runs for the Claude half, as text.
 *
 * Printed rather than executed. A tool that silently reconfigures another tool
 * is a tool whose effects nobody can review. */
atlas_status atlas_integrate_claude_commands(const atlas_integrate_report *r, atlas_buf *out,
                                             atlas_err *err);

#endif /* ATLAS_INTEGRATE_H */
