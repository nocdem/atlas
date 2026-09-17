/* Atlas - command line front end.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The CLI parses arguments, calls the service layer, and hands results to a
 * renderer. It contains no SQL, no git invocation and no output formatting.
 */
#include "atlas/cli.h"

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/authority.h"
#include "atlas/backup.h"
#include "atlas/gateway.h"
#include "atlas/daemon.h"
#include "atlas/hook.h"
#include "atlas/integrate.h"
#include "atlas/ipc.h"
#include "atlas/maintenance.h"
#include "atlas/mcp.h"
#include "atlas/sem.h"
#include "atlas/unit.h"
#include "cli/render.h"


#include "cli/cli_internal.h"

atlas_status atlas_cli_take_name(cli_state *st, const char **name_out, atlas_err *err) {
    *name_out = NULL;
    size_t w = 0;
    for (size_t i = 0; i < st->operand_count; i++) {
        if (strcmp(st->operands[i], "--name") == 0) {
            if (i + 1u >= st->operand_count) {
                return atlas_err_set(err, ATLAS_ERR_USAGE, "--name needs a value");
            }
            *name_out = st->operands[i + 1u];
            i++;
            continue;
        }
        st->operands[w++] = st->operands[i];
    }
    st->operand_count = w;
    return ATLAS_OK;
}

/* A13. `--scanner-uid N`, for `repo add` and `repo scanner`.
 *
 * `*given_out` stays false when the flag is absent, which is what selects
 * "derive from the repository root's owner". The flag is separate from the
 * value because 0 already means "no scanner assigned" and cannot also mean
 * "derive one". A value that is not a plain non-negative integer is a usage
 * error rather than a silent 0. */
atlas_status atlas_cli_take_scanner_uid(cli_state *st, bool *given_out, int64_t *uid_out,
                                     atlas_err *err) {
    *given_out = false;
    *uid_out = 0;
    size_t w = 0;
    for (size_t i = 0; i < st->operand_count; i++) {
        if (strcmp(st->operands[i], "--scanner-uid") == 0) {
            if (i + 1u >= st->operand_count) {
                return atlas_err_set(err, ATLAS_ERR_USAGE, "--scanner-uid needs a value");
            }
            const char *v = st->operands[i + 1u];
            char *end = NULL;
            errno = 0;
            long long parsed = strtoll(v, &end, 10);
            if (errno != 0 || end == v || *end != '\0' || parsed < 0) {
                return atlas_err_set(err, ATLAS_ERR_USAGE,
                                     "--scanner-uid takes a non-negative integer uid");
            }
            *given_out = true;
            *uid_out = (int64_t)parsed;
            i++;
            continue;
        }
        st->operands[w++] = st->operands[i];
    }
    st->operand_count = w;
    return ATLAS_OK;
}

/* --- renderer construction ---------------------------------------------- */

atlas_status atlas_cli_renderer_open(atlas_renderer *r, bool json, FILE *out, const char *command,
                                  atlas_err *err) {
    memset(r, 0, sizeof(*r));
    r->out = out;
    r->json = json;
    atlas_safe_pool_init(&r->safe);
    r->open_scope = -1;
    r->v = json ? &ATLAS_RENDERER_JSON : &ATLAS_RENDERER_HUMAN;
    if (json) {
        r->j = atlas_json_new(out, err);
        if (r->j == NULL) {
            return err->status;
        }
    }
    return r->v->begin(r, command, err);
}

atlas_status atlas_cli_renderer_close(atlas_renderer *r, atlas_err *err) {
    atlas_status st = r->v->end(r, err);
    atlas_safe_pool_free(&r->safe);
    return st;
}

void atlas_cli_renderer_abort(atlas_renderer *r) {
    /* On failure the partial document is discarded: atlas_render_error writes a
     * complete error document instead. */
    if (r->j != NULL) {
        atlas_json_free(r->j);
        r->j = NULL;
    }
    atlas_safe_pool_free(&r->safe);
}

/* --- command implementations -------------------------------------------- */



atlas_status atlas_cli_repo_item_sink(const atlas_repo_info *ri, void *ud, atlas_err *err) {
    list_sink *ls = (list_sink *)ud;
    return ls->r->v->repo_item(ls->r, ri, err);
}

atlas_status atlas_cli_search_item_sink(const atlas_search_hit *h, void *ud, atlas_err *err) {
    list_sink *ls = (list_sink *)ud;
    return ls->r->v->search_item(ls->r, h, err);
}

atlas_status atlas_cli_history_item_sink(const atlas_history_row *h, void *ud, atlas_err *err) {
    list_sink *ls = (list_sink *)ud;
    return ls->r->v->history_item(ls->r, h, err);
}

atlas_status atlas_cli_diff_item_sink(const atlas_diff_entry *e, void *ud, atlas_err *err) {
    list_sink *ls = (list_sink *)ud;
    return ls->r->v->diff_item(ls->r, e, err);
}

/* `file` is the one streaming read whose service call can fail having produced
 * no row at all — asking a repository for a path it does not index is an error
 * rather than an empty result. Every other streaming command answers that with
 * zero rows.
 *
 * Opened eagerly like the others, the document header and the repository line
 * were already on stdout when the call failed, and the error document went out
 * after them: two documents, the first one unterminated, from a `--json`
 * invocation that promises exactly one. A streaming writer cannot recall bytes,
 * so the fix is not to write them until there is something to write.
 *
 * So the sink opens the renderer on its first call and the command closes it
 * only if the sink ever fired. A failure before the first row leaves stdout
 * untouched and the error document is the whole output. */


atlas_status atlas_cli_file_report_sink(const atlas_file_report *rep, void *ud, atlas_err *err) {
    file_sink *fs = (file_sink *)ud;
    if (!fs->opened) {
        atlas_status s = atlas_cli_renderer_open(fs->r, fs->st->opts.json, fs->st->out, "file", err);
        if (s != ATLAS_OK) {
            return s;
        }
        fs->opened = true;
        s = fs->r->v->note_repo(fs->r, fs->repo, err);
        if (s != ATLAS_OK) {
            return s;
        }
    }
    return fs->r->v->file(fs->r, rep, err);
}

atlas_status atlas_cli_need_operands(const cli_state *st, size_t want, const char *usage,
                                  atlas_err *err) {
    if (st->operand_count < want) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas %s", usage);
    }
    if (st->operand_count > want) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "unexpected extra argument \"%s\" (usage: atlas %s)",
                             st->operands[want], usage);
    }
    return ATLAS_OK;
}

void atlas_cli_scan_opts_from_cli(const cli_state *st, atlas_scan_opts *so) {
    atlas_scan_opts_init(so);
    so->skip_history = st->opts.no_history;
    so->max_commits = st->opts.max_commits;
    so->timeout_ms = st->opts.timeout_ms;
}

atlas_status atlas_cli_event_item_sink(const atlas_event_row *row, void *ud, atlas_err *err) {
    list_sink *ls = (list_sink *)ud;
    return ls->r->v->event_item(ls->r, row, err);
}
