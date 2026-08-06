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

#include "atlas/json.h"
#include "atlas/safetext.h"
#include "atlas/service.h"

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
