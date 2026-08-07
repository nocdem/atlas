/* Atlas - output renderers.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Two renderers implement one interface, and both are driven by the same service
 * results in the same order. Nothing in a renderer queries the database or git,
 * so human and JSON output cannot drift apart.
 *
 * Renderers are streaming: list items are written as they arrive, so a large
 * result set is never assembled in memory.
 */
#ifndef ATLAS_CLI_RENDER_H
#define ATLAS_CLI_RENDER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "atlas/integrate.h"
#include "atlas/json.h"
#include "atlas/safetext.h"
#include "atlas/service.h"
#include "atlas/unit.h"

typedef struct atlas_renderer atlas_renderer;

typedef struct atlas_renderer_vtbl {
    atlas_status (*begin)(atlas_renderer *r, const char *command, atlas_err *err);
    atlas_status (*end)(atlas_renderer *r, atlas_err *err);
    atlas_status (*note_repo)(atlas_renderer *r, const char *repo, atlas_err *err);
    atlas_status (*note_query)(atlas_renderer *r, const char *query, atlas_search_mode mode,
                               atlas_err *err);
    atlas_status (*list_begin)(atlas_renderer *r, const char *key, atlas_err *err);
    /* `singular` and `plural` are both supplied because English plurals are not
     * derivable by appending 's' ("repository" / "repositories"). */
    atlas_status (*list_end)(atlas_renderer *r, const char *singular, const char *plural,
                             int64_t count, atlas_err *err);
    atlas_status (*doctor)(atlas_renderer *r, const atlas_doctor_report *rep, atlas_err *err);
    atlas_status (*version)(atlas_renderer *r, atlas_err *err);
    atlas_status (*repo_item)(atlas_renderer *r, const atlas_repo_info *ri, atlas_err *err);
    atlas_status (*repo_added)(atlas_renderer *r, const atlas_repo_info *ri, atlas_err *err);
    atlas_status (*repo_removed)(atlas_renderer *r, const atlas_repo_info *ri, atlas_err *err);
    atlas_status (*scan)(atlas_renderer *r, const char *repo, const atlas_scan_summary *s,
                         atlas_err *err);
    atlas_status (*status)(atlas_renderer *r, const atlas_status_report *s, atlas_err *err);
    atlas_status (*search_item)(atlas_renderer *r, const atlas_search_hit *h, atlas_err *err);
    atlas_status (*file)(atlas_renderer *r, const atlas_file_report *f, atlas_err *err);
    atlas_status (*history_item)(atlas_renderer *r, const atlas_history_row *h, atlas_err *err);
    /* Diff is reported as a header, then entries grouped by scope, then a tail. */
    atlas_status (*diff_begin)(atlas_renderer *r, const atlas_diff_report *rep, atlas_err *err);
    atlas_status (*diff_item)(atlas_renderer *r, const atlas_diff_entry *e, atlas_err *err);
    atlas_status (*diff_end)(atlas_renderer *r, const atlas_diff_report *rep, atlas_err *err);
    /* --- A1 --- */
    atlas_status (*daemon_status)(atlas_renderer *r, const atlas_daemon_status_report *rep,
                                  atlas_err *err);
    atlas_status (*daemon_ping)(atlas_renderer *r, bool reachable, const char *socket_path,
                                const char *detail, atlas_err *err);
    atlas_status (*repo_state)(atlas_renderer *r, const atlas_repo_state_report *rep,
                               atlas_err *err);
    atlas_status (*sync)(atlas_renderer *r, const char *repo, const atlas_sync_report *rep,
                         atlas_err *err);
    atlas_status (*event_item)(atlas_renderer *r, const atlas_event_row *row, atlas_err *err);
    atlas_status (*events_end)(atlas_renderer *r, int64_t cursor, bool more, atlas_err *err);
    atlas_status (*unit_text)(atlas_renderer *r, const char *text, atlas_err *err);
    atlas_status (*unit_install)(atlas_renderer *r, const atlas_unit_install_report *rep,
                                 bool uninstall, atlas_err *err);
    /* --- A2 ---
     *
     * One method rather than four. `integrate claude print|install|uninstall|
     * doctor` all report the same shape — what was found, what changed, what is
     * wrong — and giving each its own renderer method would have been four
     * places for the human and JSON forms to drift apart instead of one. */
    atlas_status (*integrate)(atlas_renderer *r, const atlas_integrate_report *rep,
                              const char *action, const char *commands, atlas_err *err);
    /* --- A3 ---
     *
     * Six methods rather than one per command. `code file` is several
     * independent lists sharing one header, and `code deps` and `code impact`
     * are the same traversal in opposite directions — so the shapes are the
     * report, the symbol, the edge and the traversal candidate, and the commands
     * compose them. Fewer shapes is fewer places for the two renderers to
     * drift. */
    atlas_status (*code_status)(atlas_renderer *r, const atlas_code_status_report *rep,
                                atlas_err *err);
    atlas_status (*code_file)(atlas_renderer *r, const atlas_code_file_report *rep,
                              atlas_err *err);
    atlas_status (*code_symbol_item)(atlas_renderer *r, const atlas_code_symbol_row *row,
                                     atlas_err *err);
    atlas_status (*code_edge_item)(atlas_renderer *r, const atlas_code_edge_row *row,
                                   atlas_err *err);
    atlas_status (*code_walk_item)(atlas_renderer *r, const atlas_code_walk_row *row,
                                   atlas_err *err);
    atlas_status (*code_walk_end)(atlas_renderer *r, const atlas_code_walk_summary *sum,
                                  atlas_err *err);
    /* A named list, for the commands that emit several.
     *
     * `list_begin`/`list_end` write the count under the fixed key `count`,
     * which is A0's contract and is right for a command with one list. A file
     * context has three, and three `count` members in one object is a document
     * whose meaning depends on which one a parser keeps. These write
     * `<key>` and `<key>_count`, so every list carries its own. */
    atlas_status (*code_list_begin)(atlas_renderer *r, const char *key, atlas_err *err);
    atlas_status (*code_list_end)(atlas_renderer *r, const char *key, const char *singular,
                                  const char *plural, int64_t count, bool more, atlas_err *err);
    /* --- A4 ---
     *
     * Four shapes, because there are four things to show: a document in a list,
     * a whole document, one entry in its timeline, and the outcome of a
     * lifecycle write. `decision export` reuses the whole-document shape rather
     * than adding a fifth, so the export and `decision show --json` cannot
     * describe one document differently.
     *
     * Every string these receive is already safe-encoded by the service layer
     * — decision prose is untrusted whatever its status — so the renderers do
     * not encode again, and both say so at the top of the file. */
    atlas_status (*decision_item)(atlas_renderer *r, const atlas_decision_summary *s,
                                  atlas_err *err);
    atlas_status (*decision_show)(atlas_renderer *r, const atlas_decision_document *d,
                                  atlas_err *err);
    atlas_status (*decision_event)(atlas_renderer *r, const atlas_decision_timeline_entry *e,
                                   atlas_err *err);
    atlas_status (*decision_outcome)(atlas_renderer *r, const atlas_decision_outcome *o,
                                     atlas_err *err);
    /* The lifecycle totals a listing reports beside its page. */
    atlas_status (*decision_counts)(atlas_renderer *r, const atlas_decision_counts *c,
                                    atlas_err *err);
    /* Whether the cached status agrees with the append-only ledger. Its own
     * method rather than a field on something else, because it is a statement
     * about the *record* rather than about any one decision, and `decision
     * history` is where a reader would look for it. */
    atlas_status (*decision_ledger)(atlas_renderer *r, bool agrees, atlas_err *err);
} atlas_renderer_vtbl;

struct atlas_renderer {
    const atlas_renderer_vtbl *v;
    FILE *out;
    atlas_json *j; /* JSON renderer only */
    bool json;
    bool in_list;
    int64_t items;
    /* Untrusted values are encoded through this before they are printed. */
    atlas_safe_pool safe;
    /* Diff rendering state: which scope section is currently open. */
    int open_scope;
    bool scope_open;
};

extern const atlas_renderer_vtbl ATLAS_RENDERER_HUMAN;
extern const atlas_renderer_vtbl ATLAS_RENDERER_JSON;

/* Formats a unix timestamp as ISO-8601 UTC, or "-" for a zero timestamp. */
void atlas_format_time(int64_t unix_time, char *out, size_t out_size);
/* Shortens a hex object id for human output. */
void atlas_short_oid(const char *oid, char *out, size_t out_size);

/* Reports a failed command. In JSON mode this writes a complete error document
 * to `out` so a caller parsing stdout always receives valid JSON. */
void atlas_render_error(FILE *out, FILE *errout, bool json, const char *command,
                        const atlas_err *err);

#endif /* ATLAS_CLI_RENDER_H */
