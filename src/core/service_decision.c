/* Atlas - the service layer for decision documents and operator approval.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * All A4 command behaviour lives here. The CLI parses arguments and picks a
 * renderer; the renderers format what these produce. Neither reaches past this
 * layer, which is what keeps human and JSON output structurally incapable of
 * disagreeing.
 *
 * Reads run against the local index on the calling thread, like every other
 * read command: they work whether or not a daemon is running, and a daemon that
 * is running holds the write lock rather than the read path.
 *
 * **Writes are routed to the daemon when one is answering**, and taken on this
 * thread only when this process holds the data-directory lock. That is the same
 * rule `atlas code sync --rebuild` follows, and it is what keeps "exactly one
 * process writes the index" true for the lifecycle as well.
 *
 * The operator channel is at the bottom of the file. Its one claim, stated
 * again there because it is the claim most likely to be overstated by somebody
 * reading only the function name: it establishes that a confirmation was typed
 * at a terminal, not that a particular person typed it.
 */
#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/decision_ops.h"
#include "atlas/ipc.h"
#include "atlas/jsonread.h"
#include "atlas/pathrep.h"
#include "atlas/safetext.h"
#include "atlas/terminal.h"
#include "core/service_internal.h"

/* --- structures --------------------------------------------------------------- */

void atlas_decision_summary_init(atlas_decision_summary *s) {
    memset(s, 0, sizeof(*s));
    atlas_buf_init(&s->uid);
    atlas_buf_init(&s->status);
    atlas_buf_init(&s->revision_state);
    atlas_buf_init(&s->title);
    atlas_buf_init(&s->content_hash);
    atlas_buf_init(&s->proposed_by);
    atlas_buf_init(&s->superseded_by);
    atlas_buf_init(&s->created_at);
    atlas_buf_init(&s->updated_at);
}

void atlas_decision_summary_free(atlas_decision_summary *s) {
    if (s == NULL) {
        return;
    }
    atlas_buf_free(&s->uid);
    atlas_buf_free(&s->status);
    atlas_buf_free(&s->revision_state);
    atlas_buf_free(&s->title);
    atlas_buf_free(&s->content_hash);
    atlas_buf_free(&s->proposed_by);
    atlas_buf_free(&s->superseded_by);
    atlas_buf_free(&s->created_at);
    atlas_buf_free(&s->updated_at);
}

void atlas_decision_link_view_init(atlas_decision_link_view *v) {
    memset(v, 0, sizeof(*v));
    atlas_buf_init(&v->kind);
    atlas_buf_init(&v->value);
    atlas_buf_init(&v->detail);
    atlas_buf_init(&v->currency);
    atlas_buf_init(&v->analyzer);
}

void atlas_decision_link_view_free(atlas_decision_link_view *v) {
    if (v == NULL) {
        return;
    }
    atlas_buf_free(&v->kind);
    atlas_buf_free(&v->value);
    atlas_buf_free(&v->detail);
    atlas_buf_free(&v->currency);
    atlas_buf_free(&v->analyzer);
}

void atlas_decision_document_init(atlas_decision_document *d) {
    memset(d, 0, sizeof(*d));
    atlas_decision_summary_init(&d->summary);
    atlas_buf_init(&d->repo);
    atlas_buf_init(&d->context_text);
    atlas_buf_init(&d->decision_text);
    atlas_buf_init(&d->rationale_text);
    atlas_buf_init(&d->consequences_text);
    atlas_buf_init(&d->scope);
    atlas_buf_init(&d->basis_head);
    atlas_buf_init(&d->basis_repo_identity);
    atlas_buf_init(&d->unbound_reason);
    for (size_t i = 0; i < ATLAS_DECISION_MAX_ALTERNATIVES; i++) {
        atlas_buf_init(&d->alternatives[i]);
    }
    d->ledger_agrees = true;
}

void atlas_decision_document_free(atlas_decision_document *d) {
    if (d == NULL) {
        return;
    }
    atlas_decision_summary_free(&d->summary);
    atlas_buf_free(&d->repo);
    atlas_buf_free(&d->context_text);
    atlas_buf_free(&d->decision_text);
    atlas_buf_free(&d->rationale_text);
    atlas_buf_free(&d->consequences_text);
    atlas_buf_free(&d->scope);
    atlas_buf_free(&d->basis_head);
    atlas_buf_free(&d->basis_repo_identity);
    atlas_buf_free(&d->unbound_reason);
    for (size_t i = 0; i < ATLAS_DECISION_MAX_ALTERNATIVES; i++) {
        atlas_buf_free(&d->alternatives[i]);
    }
    for (size_t i = 0; i < d->link_count && i < ATLAS_DECISION_MAX_LINKS; i++) {
        atlas_decision_link_view_free(&d->links[i]);
    }
    d->link_count = 0;
}

void atlas_decision_outcome_init(atlas_decision_outcome *o) {
    memset(o, 0, sizeof(*o));
    atlas_buf_init(&o->repo);
    atlas_buf_init(&o->uid);
    atlas_buf_init(&o->state);
    atlas_buf_init(&o->replaced_by);
    atlas_buf_init(&o->unbound_reason);
}

void atlas_decision_outcome_free(atlas_decision_outcome *o) {
    if (o == NULL) {
        return;
    }
    atlas_buf_free(&o->repo);
    atlas_buf_free(&o->uid);
    atlas_buf_free(&o->state);
    atlas_buf_free(&o->replaced_by);
    atlas_buf_free(&o->unbound_reason);
}

/* --- reads --------------------------------------------------------------------- */

typedef struct list_state {
    atlas_ctx *ctx;
    atlas_safe_pool safe;
    atlas_decision_summary_cb cb;
    void *ud;
    atlas_status st;
} list_state;

static atlas_status fill_summary(list_state *ls, const atlas_decision_doc_row *row,
                                 atlas_decision_summary *s, atlas_err *err) {
    struct {
        atlas_buf *to;
        const char *from;
        bool untrusted;
    } fields[] = {
        {&s->uid, row->uid, false},
        {&s->status, row->status, false},
        {&s->revision_state, row->head_state, false},
        {&s->content_hash, row->content_hash, false},
        {&s->proposed_by, row->proposed_by, false},
        {&s->superseded_by, row->superseded_by_uid, false},
        {&s->created_at, row->created_at, false},
        {&s->updated_at, row->updated_at, false},
        /* The only prose, and the only one encoded. Approval changes a record's
         * status, not the nature of its bytes. */
        {&s->title, row->title, true},
    };
    atlas_status st = ATLAS_OK;
    for (size_t i = 0; st == ATLAS_OK && i < sizeof(fields) / sizeof(fields[0]); i++) {
        const char *v = fields[i].from != NULL ? fields[i].from : "";
        st = atlas_buf_set_str(fields[i].to, fields[i].untrusted ? atlas_safe(&ls->safe, v) : v,
                               err);
    }
    s->revision_no = row->head_revision_no;
    s->latest_revision_no = row->latest_revision_no;
    s->link_count = row->link_count;
    return st;
}

static atlas_status on_doc(const atlas_decision_doc_row *row, void *ud, atlas_err *err) {
    list_state *ls = (list_state *)ud;
    atlas_decision_summary s;
    atlas_decision_summary_init(&s);
    atlas_status st = fill_summary(ls, row, &s, err);
    if (st == ATLAS_OK) {
        st = ls->cb(&s, ls->ud, err);
    }
    atlas_decision_summary_free(&s);
    return st;
}

atlas_status atlas_service_decision_list(atlas_ctx *ctx, const char *repo,
                                         const atlas_decision_list_opts *opts,
                                         atlas_decision_summary_cb cb, void *ud,
                                         atlas_decision_counts *counts, int64_t *count_out,
                                         bool *more_out, atlas_err *err) {
    *count_out = 0;
    *more_out = false;
    memset(counts, 0, sizeof(*counts));

    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_service_require_repo(ctx, repo, &info, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }
    int64_t limit = opts->limit > 0 ? opts->limit : ATLAS_DECISION_DEFAULT_ROWS;
    if (limit > ATLAS_DECISION_MAX_ROWS) {
        limit = ATLAS_DECISION_MAX_ROWS;
    }

    list_state ls;
    memset(&ls, 0, sizeof(ls));
    ls.ctx = ctx;
    atlas_safe_pool_init(&ls.safe);
    ls.cb = cb;
    ls.ud = ud;

    atlas_db *db = atlas_ctx_db(ctx);
    switch (opts->mode) {
    case ATLAS_DECISION_LIST_SEARCH:
        st = atlas_db_decision_search(db, info.id, opts->query, limit, on_doc, &ls, count_out,
                                      more_out, err);
        break;
    case ATLAS_DECISION_LIST_PATH: {
        /* A path arrives in the safe text encoding and is looked up by raw
         * bytes, like every path in Atlas. */
        atlas_buf raw = ATLAS_BUF_INIT;
        st = atlas_path_text_decode(opts->path, strlen(opts->path), &raw, err);
        if (st == ATLAS_OK) {
            st = atlas_db_decision_for_path(db, info.id, raw.data, raw.len, limit, on_doc, &ls,
                                            count_out, more_out, err);
        }
        atlas_buf_free(&raw);
        break;
    }
    case ATLAS_DECISION_LIST_STATUS:
        st = atlas_db_decision_documents_list(db, info.id, opts->status, limit, on_doc, &ls,
                                              count_out, more_out, err);
        break;
    case ATLAS_DECISION_LIST_ALL:
    default:
        st = atlas_db_decision_documents_list(db, info.id, NULL, limit, on_doc, &ls, count_out,
                                              more_out, err);
        break;
    }
    if (st == ATLAS_OK) {
        st = atlas_db_decision_repo_counts(db, info.id, &counts->proposed, &counts->approved,
                                           &counts->rejected, &counts->superseded, err);
    }
    atlas_safe_pool_free(&ls.safe);
    atlas_repo_info_free(&info);
    return st;
}

/* Resolves a uid within a repository, refusing one that belongs elsewhere. */
static atlas_status resolve_uid(atlas_ctx *ctx, const atlas_repo_info *info, const char *uid,
                                int64_t *doc_id, atlas_err *err) {
    if (!atlas_decision_uid_is_valid(uid)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "that is not a decision id; they look like %s followed by %u "
                             "lowercase hex characters",
                             ATLAS_DECISION_UID_PREFIX, (unsigned)ATLAS_DECISION_UID_HEX);
    }
    int64_t repo_of = 0;
    bool found = false;
    atlas_status st =
        atlas_db_decision_find_uid(atlas_ctx_db(ctx), uid, doc_id, &repo_of, &found, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (!found) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "no decision has that id");
    }
    if (repo_of != info->id) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "that decision belongs to a different repository");
    }
    return ATLAS_OK;
}

/* Whether Atlas has ever looked, which is what turns "not indexed" into
 * UNKNOWN rather than MISSING.
 *
 * A completed `atlas scan` counts as well as a completed reconciliation pass.
 * Only the latter sets `last_complete_generation`, so checking that alone made
 * every link on a scanned-but-undaemonised repository report UNKNOWN forever —
 * which is not caution, it is discarding an answer Atlas has. */
static atlas_status index_known(atlas_ctx *ctx, const atlas_repo_info *info, bool *file_known,
                                bool *code_known, atlas_err *err) {
    int64_t repo_id = info->id;
    *file_known = info->last_scan_id > 0;
    *code_known = false;
    atlas_index_state is;
    atlas_index_state_init(&is);
    atlas_status st = atlas_db_index_state_get(atlas_ctx_db(ctx), repo_id, &is, err);
    if (st == ATLAS_OK && is.present && is.last_complete_generation > 0) {
        *file_known = true;
    }
    atlas_index_state_free(&is);
    if (st != ATLAS_OK) {
        return st;
    }
    atlas_code_index_state cs;
    atlas_code_index_state_init(&cs);
    st = atlas_db_code_state_get(atlas_ctx_db(ctx), repo_id, &cs, err);
    if (st == ATLAS_OK) {
        *code_known = cs.present && cs.last_complete_generation > 0;
    }
    atlas_code_index_state_free(&cs);
    return st;
}

typedef struct doc_state {
    atlas_decision_document *out;
    list_state ls;
} doc_state;

static atlas_status on_doc_header(const atlas_decision_summary *s, void *ud, atlas_err *err) {
    doc_state *dst = (doc_state *)ud;
    atlas_decision_summary *to = &dst->out->summary;
    struct {
        atlas_buf *to;
        const atlas_buf *from;
    } fields[] = {
        {&to->uid, &s->uid},           {&to->status, &s->status},
        {&to->revision_state, &s->revision_state}, {&to->title, &s->title},
        {&to->content_hash, &s->content_hash},     {&to->proposed_by, &s->proposed_by},
        {&to->superseded_by, &s->superseded_by},   {&to->created_at, &s->created_at},
        {&to->updated_at, &s->updated_at},
    };
    atlas_status st = ATLAS_OK;
    for (size_t i = 0; st == ATLAS_OK && i < sizeof(fields) / sizeof(fields[0]); i++) {
        st = atlas_buf_set(fields[i].to, fields[i].from->data, fields[i].from->len, err);
    }
    to->revision_no = s->revision_no;
    to->latest_revision_no = s->latest_revision_no;
    to->link_count = s->link_count;
    return st;
}

static atlas_status header_row(const atlas_decision_doc_row *row, void *ud, atlas_err *err) {
    doc_state *dst = (doc_state *)ud;
    atlas_decision_summary s;
    atlas_decision_summary_init(&s);
    atlas_status st = fill_summary(&dst->ls, row, &s, err);
    if (st == ATLAS_OK) {
        st = on_doc_header(&s, dst, err);
    }
    atlas_decision_summary_free(&s);
    return st;
}

atlas_status atlas_service_decision_show(atlas_ctx *ctx, const char *repo, const char *uid,
                                         int64_t revision_no, atlas_decision_document *out,
                                         atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_service_require_repo(ctx, repo, &info, err);
    int64_t doc_id = 0;
    if (st == ATLAS_OK) {
        st = resolve_uid(ctx, &info, uid, &doc_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&out->repo, info.name, err);
    }
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }
    atlas_db *db = atlas_ctx_db(ctx);

    /* Which revision. A named one reads what was actually approved; the default
     * reads what is effective, which is the approved one when there is one and
     * the newest otherwise. */
    int64_t rev_id = 0;
    if (revision_no > 0) {
        bool found = false;
        st = atlas_db_decision_revision_by_no(db, doc_id, revision_no, &rev_id, &found, err);
        if (st == ATLAS_OK && !found) {
            st = atlas_err_set(err, ATLAS_ERR_USAGE, "this decision has no revision %lld",
                               (long long)revision_no);
        }
    } else {
        st = atlas_db_decision_current_revision(db, doc_id, &rev_id, err);
        if (st == ATLAS_OK && rev_id == 0) {
            int64_t no = 0;
            char hash[ATLAS_SHA256_HEX_LEN + 1u];
            char state[16];
            st = atlas_db_decision_latest_revision(db, doc_id, &rev_id, &no, hash, sizeof(hash),
                                                   state, sizeof(state), err);
        }
    }
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }

    doc_state dst;
    memset(&dst, 0, sizeof(dst));
    dst.out = out;
    atlas_safe_pool_init(&dst.ls.safe);
    bool seen = false;
    st = atlas_db_decision_document_row(db, doc_id, header_row, &dst, &seen, err);

    atlas_decision_revision rev;
    atlas_decision_revision_init(&rev);
    bool found = false;
    if (st == ATLAS_OK) {
        st = atlas_db_decision_revision_load(db, rev_id, &rev, &found, err);
    }
    if (st == ATLAS_OK && !found) {
        st = atlas_err_set(err, ATLAS_ERR_INTEGRITY, "this decision has no revisions");
    }
    if (st == ATLAS_OK) {
        /* The header describes the *effective* revision; this describes the one
         * being shown. When they differ — reading revision 1 of a document
         * approved at revision 2 — the reader has to see which. */
        out->summary.revision_no = rev.revision_no;
        st = atlas_buf_set_str(&out->summary.revision_state,
                               atlas_decision_state_name(rev.state), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&out->summary.content_hash, rev.content_hash, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&out->scope, atlas_decision_scope_name(rev.scope), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&out->basis_head, rev.basis_head.data, rev.basis_head.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&out->basis_repo_identity, rev.basis_repo_identity.data,
                           rev.basis_repo_identity.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&out->unbound_reason, rev.unbound_reason.data, rev.unbound_reason.len,
                           err);
    }
    out->session_unbound = rev.session_unbound;
    out->imported_from_a2_decision = rev.imported_from_ai_decision_id;

    /* The prose, encoded. Every field, without exception: this is the one place
     * a whole decision reaches a terminal. */
    struct {
        atlas_buf *to;
        const atlas_buf *from;
    } prose[] = {
        {&out->summary.title, &rev.title},
        {&out->context_text, &rev.context_text},
        {&out->decision_text, &rev.decision_text},
        {&out->rationale_text, &rev.rationale_text},
        {&out->consequences_text, &rev.consequences_text},
    };
    for (size_t i = 0; st == ATLAS_OK && i < sizeof(prose) / sizeof(prose[0]); i++) {
        st = atlas_buf_set_str(prose[i].to,
                               atlas_safe(&dst.ls.safe, atlas_buf_cstr(prose[i].from)), err);
    }
    for (size_t i = 0; st == ATLAS_OK && i < rev.alternative_count; i++) {
        st = atlas_buf_set_str(&out->alternatives[i],
                               atlas_safe(&dst.ls.safe, atlas_buf_cstr(&rev.alternatives[i])), err);
        if (st == ATLAS_OK) {
            out->alternative_count++;
        }
    }

    if (st == ATLAS_OK) {
        st = index_known(ctx, &info, &out->file_index_known, &out->code_index_known, err);
    }
    for (size_t i = 0; st == ATLAS_OK && i < rev.link_count; i++) {
        atlas_decision_link *l = &rev.links[i];
        st = atlas_db_decision_link_resolve(db, info.id, l, out->file_index_known,
                                            out->code_index_known, err);
        if (st != ATLAS_OK) {
            break;
        }
        atlas_decision_link_view *v = &out->links[out->link_count];
        atlas_decision_link_view_init(v);
        v->matches = l->match_count;
        v->analyzer_version = l->analyzer_version;
        st = atlas_buf_set_str(&v->kind, atlas_decision_link_kind_name(l->kind), err);
        if (st == ATLAS_OK) {
            st = atlas_buf_set_str(&v->currency, atlas_decision_link_currency_name(l->currency),
                                   err);
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_set(&v->analyzer, l->analyzer_name.data, l->analyzer_name.len, err);
        }
        if (st == ATLAS_OK) {
            /* `path_text` and `symbol_name_text` are stored already encoded, so
             * they are used as-is; encoding them again would double-encode.
             * `commit_oid` and a target uid are validated shapes. */
            switch (l->kind) {
            case ATLAS_DECISION_LINK_PATH:
                st = atlas_buf_set(&v->value, l->path_text.data, l->path_text.len, err);
                break;
            case ATLAS_DECISION_LINK_COMMIT:
                st = atlas_buf_set(&v->value, l->commit_oid.data, l->commit_oid.len, err);
                break;
            case ATLAS_DECISION_LINK_SYMBOL:
                st = atlas_buf_set(&v->value, l->symbol_name_text.data, l->symbol_name_text.len,
                                   err);
                if (st == ATLAS_OK && l->path_text.len > 0) {
                    st = atlas_buf_set(&v->detail, l->path_text.data, l->path_text.len, err);
                }
                break;
            case ATLAS_DECISION_LINK_CHANGE_SET:
                st = atlas_buf_appendf(&v->value, err, "%lld", (long long)l->change_set_id);
                break;
            case ATLAS_DECISION_LINK_SUPERSEDES:
            case ATLAS_DECISION_LINK_REPLACED_BY:
            case ATLAS_DECISION_LINK_RELATES_TO:
                st = atlas_buf_set(&v->value, l->target_uid.data, l->target_uid.len, err);
                break;
            }
        }
        if (st == ATLAS_OK) {
            if (l->currency == ATLAS_DECISION_LINK_CHANGED ||
                l->currency == ATLAS_DECISION_LINK_MISSING ||
                l->currency == ATLAS_DECISION_LINK_AMBIGUOUS) {
                out->links_needing_review++;
            }
            out->link_count++;
        } else {
            atlas_decision_link_view_free(v);
        }
    }

    if (st == ATLAS_OK) {
        /* The ledger is canonical and the status columns cache it. Reported,
         * never repaired: a command that quietly fixed this would hide the
         * fact that it recurred. */
        st = atlas_db_decision_verify(db, doc_id, &out->ledger_agrees, NULL, err);
    }

    atlas_decision_revision_free(&rev);
    atlas_safe_pool_free(&dst.ls.safe);
    atlas_repo_info_free(&info);
    return st;
}

typedef struct history_state {
    list_state ls;
    atlas_decision_summary_cb rev_cb;
    atlas_decision_timeline_cb event_cb;
    void *ud;
} history_state;

static atlas_status on_rev(const atlas_decision_rev_row *row, void *ud, atlas_err *err) {
    history_state *hs = (history_state *)ud;
    atlas_decision_summary s;
    atlas_decision_summary_init(&s);
    s.revision_no = row->revision_no;
    atlas_status st = atlas_buf_set_str(&s.revision_state, row->state, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&s.content_hash, row->content_hash, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&s.proposed_by, row->proposed_by, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&s.created_at, row->created_at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&s.title,
                               atlas_safe(&hs->ls.safe, row->title != NULL ? row->title : ""), err);
    }
    if (st == ATLAS_OK) {
        st = hs->rev_cb(&s, hs->ud, err);
    }
    atlas_decision_summary_free(&s);
    return st;
}

static atlas_status on_event(const atlas_decision_event_row *row, void *ud, atlas_err *err) {
    history_state *hs = (history_state *)ud;
    atlas_decision_timeline_entry e;
    memset(&e, 0, sizeof(e));
    e.event = row->event;
    e.actor = row->actor;
    e.content_hash = row->content_hash;
    e.superseded_by = row->superseded_by_uid;
    /* A fixed Atlas vocabulary written by lifecycle.c as string literals. */
    e.detail = row->detail;
    e.at = row->created_at;
    e.revision_no = row->revision_no;
    e.operator_channel = row->challenge_id > 0;
    return hs->event_cb(&e, hs->ud, err);
}

atlas_status atlas_service_decision_history(atlas_ctx *ctx, const char *repo, const char *uid,
                                            atlas_decision_summary_cb rev_cb,
                                            atlas_decision_timeline_cb event_cb, void *ud,
                                            bool *ledger_agrees_out, atlas_err *err) {
    *ledger_agrees_out = true;
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_service_require_repo(ctx, repo, &info, err);
    int64_t doc_id = 0;
    if (st == ATLAS_OK) {
        st = resolve_uid(ctx, &info, uid, &doc_id, err);
    }
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }
    history_state hs;
    memset(&hs, 0, sizeof(hs));
    atlas_safe_pool_init(&hs.ls.safe);
    hs.rev_cb = rev_cb;
    hs.event_cb = event_cb;
    hs.ud = ud;

    atlas_db *db = atlas_ctx_db(ctx);
    int64_t count = 0;
    bool more = false;
    st = atlas_db_decision_revisions_list(db, doc_id, ATLAS_DECISION_MAX_REVISIONS, on_rev, &hs,
                                          &count, &more, err);
    if (st == ATLAS_OK) {
        st = atlas_db_decision_events_list(db, doc_id, ATLAS_DECISION_MAX_EVENTS, on_event, &hs,
                                           &count, &more, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_decision_verify(db, doc_id, ledger_agrees_out, NULL, err);
    }
    atlas_safe_pool_free(&hs.ls.safe);
    atlas_repo_info_free(&info);
    return st;
}

/* --- writes ---------------------------------------------------------------------
 *
 * Every one of them goes through here: build a typed operation, hand it to the
 * daemon when one is answering, and apply it on this thread only when this
 * process holds the writer lock. */

/* Copies the indexed content hash out of the borrowed row. Borrowed pointers
 * are valid only for the call, so it is copied rather than kept. */
static atlas_status take_file_hash(const atlas_file_row *row, void *ud, atlas_err *err) {
    atlas_buf *out = (atlas_buf *)ud;
    if (row->deleted || row->content_hash == NULL || row->content_hash[0] == '\0') {
        return ATLAS_OK;
    }
    return atlas_buf_set_str(out, row->content_hash, err);
}

/* Fills in the part of a symbol snapshot Atlas can establish: the file the
 * symbol is defined in and that file's current content hash, but only when the
 * name resolves to exactly one definition site.
 *
 * Only when it is unique, deliberately. Recording one of several same-named
 * definitions as "the" file would bake a choice into the durable record that
 * A3 refuses to make anywhere else, and the link would then read CURRENT
 * against a file the decision may never have been about. An ambiguous name
 * gets a snapshot with no file, which resolves AMBIGUOUS later — the honest
 * answer. */
static atlas_status snapshot_symbol(atlas_ctx *ctx, int64_t repo_id, atlas_decision_link *l,
                                    atlas_err *err) {
    atlas_buf path_raw = ATLAS_BUF_INIT;
    atlas_buf hash = ATLAS_BUF_INIT;
    int64_t matches = 0;
    atlas_status st = atlas_db_code_symbol_definition_site(
        atlas_ctx_db(ctx), repo_id, l->symbol_name.data, l->symbol_name.len, &path_raw, &hash,
        &matches, err);
    if (st == ATLAS_OK && matches == 1 && path_raw.len > 0) {
        st = atlas_buf_set(&l->path_raw, path_raw.data, path_raw.len, err);
        if (st == ATLAS_OK) {
            st = atlas_path_text_encode(path_raw.data, path_raw.len, &l->path_text, err);
        }
        if (st == ATLAS_OK && hash.len > 0) {
            st = atlas_buf_set(&l->file_content_hash, hash.data, hash.len, err);
        }
    }
    atlas_buf_free(&path_raw);
    atlas_buf_free(&hash);
    return st;
}

/* `source_uid` is the document the revision will belong to, or NULL when it does
 * not exist yet. It is used for one thing: refusing a relation to itself, which
 * can only be detected where both ends are known. */
static atlas_status build_op(atlas_ctx *ctx, const char *repo, const char *source_uid,
                             const atlas_decision_input *in, atlas_decision_op *op,
                             atlas_err *err) {
    atlas_status st = atlas_buf_set_str(&op->repo_name, repo, err);
    struct {
        atlas_buf *to;
        const char *from;
        const char *name;
        size_t max;
        bool multiline;
    } fields[] = {
        {&op->revision.title, in->title, "title", ATLAS_DECISION_TITLE_MAX, false},
        {&op->revision.context_text, in->context_text, "context", ATLAS_DECISION_TEXT_MAX, true},
        {&op->revision.decision_text, in->decision_text, "decision", ATLAS_DECISION_TEXT_MAX, true},
        {&op->revision.rationale_text, in->rationale_text, "rationale", ATLAS_DECISION_TEXT_MAX,
         true},
        {&op->revision.consequences_text, in->consequences_text, "consequences",
         ATLAS_DECISION_TEXT_MAX, true},
    };
    for (size_t i = 0; st == ATLAS_OK && i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (fields[i].from == NULL) {
            continue;
        }
        size_t n = strlen(fields[i].from);
        st = atlas_decision_check_text(fields[i].name, fields[i].from, n, fields[i].max,
                                       fields[i].multiline, err);
        if (st == ATLAS_OK) {
            st = atlas_buf_set(fields[i].to, fields[i].from, n, err);
        }
    }
    if (st == ATLAS_OK && in->scope != NULL &&
        !atlas_decision_scope_parse(in->scope, &op->revision.scope)) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE,
                           "--scope is UNKNOWN, REPOSITORY, SUBSYSTEM or PATHS");
    }
    if (st == ATLAS_OK && in->dedup_key != NULL) {
        st = atlas_buf_set_str(&op->dedup_key, in->dedup_key, err);
    }
    for (size_t i = 0; st == ATLAS_OK && i < in->alternative_count; i++) {
        st = atlas_decision_revision_add_alternative(&op->revision, in->alternatives[i],
                                                     strlen(in->alternatives[i]), err);
    }
    /* Path and symbol links carry a snapshot, so the repository is resolved
     * once here rather than per link.
     *
     * **Only when this process is the one that will apply the write.** Under a
     * system deployment the index is 0700 `atlasd` and a client uid never opens
     * it, so `atlas_ctx_open` is never called and `ctx` is NULL here — see
     * `remote_serves` in src/cli/cli.c, which routes `decision propose/revise`
     * over the socket for exactly that reason. Resolving the repository or
     * reading a file row through a NULL context dereferenced it: any `--path`
     * or `--symbol-link` crashed the client with SIGSEGV before a request was
     * ever built, while `--commit` and `--alternative` survived because neither
     * touches the database.
     *
     * The snapshot is not lost by skipping it. `op_to_params` sends the link
     * intents — path text, commit hex, symbol name — and the daemon re-takes
     * every snapshot from its own index in `take_path_links` /
     * `take_symbol_links`, which is the behaviour those functions already
     * document: a caller-supplied content hash would let a caller assert that a
     * link is current, so the server never trusts one. Local mode keeps taking
     * it here so both paths write identical rows. */
    const bool applies_locally = (ctx != NULL && atlas_ctx_is_writer(ctx));
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    if (st == ATLAS_OK && applies_locally && (in->path_count > 0 || in->symbol_count > 0)) {
        st = atlas_service_require_repo(ctx, repo, &info, err);
    }
    for (size_t i = 0; st == ATLAS_OK && i < in->path_count; i++) {
        atlas_decision_link l;
        atlas_decision_link_init(&l, ATLAS_DECISION_LINK_PATH);
        st = atlas_path_text_decode(in->paths[i], strlen(in->paths[i]), &l.path_raw, err);
        if (st == ATLAS_OK) {
            st = atlas_path_text_encode(l.path_raw.data, l.path_raw.len, &l.path_text, err);
        }
        /* The snapshot, taken from Atlas' own index. A caller-supplied content
         * hash would let a caller assert that a link is current, which is the
         * claim the snapshot exists to make checkable. Skipped when the daemon
         * will apply this write: it takes the same snapshot from the same index
         * and this process has no handle to take it with. */
        if (st == ATLAS_OK && applies_locally && info.scanned_head[0] != '\0') {
            st = atlas_buf_set_str(&l.basis_commit, info.scanned_head, err);
        }
        if (st == ATLAS_OK && applies_locally) {
            bool found = false;
            st = atlas_db_file_get(atlas_ctx_db(ctx), info.id, l.path_raw.data, l.path_raw.len,
                                   take_file_hash, &l.file_content_hash, &found, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_decision_revision_add_link(&op->revision, &l, err);
        }
        atlas_decision_link_free(&l);
    }
    for (size_t i = 0; st == ATLAS_OK && i < in->commit_count; i++) {
        atlas_decision_link l;
        atlas_decision_link_init(&l, ATLAS_DECISION_LINK_COMMIT);
        st = atlas_buf_set_str(&l.commit_oid, in->commits[i], err);
        if (st == ATLAS_OK) {
            st = atlas_decision_revision_add_link(&op->revision, &l, err);
        }
        atlas_decision_link_free(&l);
    }
    /* Symbol links from the CLI carry the snapshot Atlas can take here: the
     * analyzer identity, the basis head, and — when the symbol resolves to
     * exactly one definition site — that file and its content hash.
     *
     * Taken from the index rather than from the argument, deliberately: a
     * caller-supplied content hash would let a caller assert that a link is
     * current, which is the claim the snapshot exists to make checkable. */
    {
        for (size_t i = 0; st == ATLAS_OK && i < in->symbol_count; i++) {
            atlas_decision_link l;
            atlas_decision_link_init(&l, ATLAS_DECISION_LINK_SYMBOL);
            st = atlas_buf_set_str(&l.symbol_name, in->symbols[i], err);
            if (st == ATLAS_OK) {
                st = atlas_path_text_encode(in->symbols[i], strlen(in->symbols[i]),
                                            &l.symbol_name_text, err);
            }
            if (st == ATLAS_OK && applies_locally) {
                st = atlas_buf_set_str(&l.analyzer_name, ATLAS_CODE_ANALYZER_ID, err);
            }
            if (applies_locally) {
                l.analyzer_version = (int64_t)ATLAS_CODE_ANALYZER_VERSION;
            }
            if (st == ATLAS_OK && applies_locally && info.scanned_head[0] != '\0') {
                st = atlas_buf_set_str(&l.basis_commit, info.scanned_head, err);
            }
            if (st == ATLAS_OK && applies_locally) {
                st = snapshot_symbol(ctx, info.id, &l, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_decision_revision_add_link(&op->revision, &l, err);
            }
            atlas_decision_link_free(&l);
        }
    }
    /* Decision-to-decision references. Carried as uids and resolved at the
     * write point, where the target's existence and its repository are checked
     * together — the same place `supersedes` is resolved, so there is one
     * answer to "does that document exist here" rather than two.
     *
     * Self-links are refused here because this is where the source uid is
     * known: at the write point a proposal has no uid yet. A revision that
     * pointed at its own document would be a relation that says nothing and
     * reads as a cycle to anything that walks links. */
    for (size_t i = 0; st == ATLAS_OK && i < in->decision_link_count; i++) {
        const char *target = in->decision_links[i];
        if (source_uid != NULL && target != NULL && strcmp(source_uid, target) == 0) {
            st = atlas_err_set(err, ATLAS_ERR_USAGE,
                               "a decision cannot relate to itself (%s)", target);
            break;
        }
        atlas_decision_link l;
        atlas_decision_link_init(&l, ATLAS_DECISION_LINK_RELATES_TO);
        st = atlas_buf_set_str(&l.target_uid, target != NULL ? target : "", err);
        if (st == ATLAS_OK) {
            st = atlas_decision_revision_add_link(&op->revision, &l, err);
        }
        atlas_decision_link_free(&l);
    }
    atlas_repo_info_free(&info);
    op->revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
    if (st == ATLAS_OK) {
        st = atlas_decision_revision_validate(&op->revision, err);
    }
    return st;
}

/* Copies the outcome out of the typed result. */
static atlas_status take_outcome(const atlas_decision_result *r, atlas_decision_outcome *out,
                                 bool via_daemon, bool operator_confirmed, atlas_err *err) {
    out->revision_no = r->revision_no;
    out->superseded_revision_no = r->superseded_revision_no;
    out->created = r->document_created;
    out->duplicate = r->duplicate;
    out->session_unbound = r->session_unbound;
    out->via_daemon = via_daemon;
    out->operator_confirmed = operator_confirmed;
    (void)snprintf(out->content_hash, sizeof(out->content_hash), "%s", r->content_hash);
    atlas_status st = atlas_buf_set(&out->repo, r->repo_name.data, r->repo_name.len, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&out->uid, r->uid.data, r->uid.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&out->state, atlas_decision_state_name(r->state), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&out->replaced_by, r->replaced_by_uid.data, r->replaced_by_uid.len, err);
    }
    if (st == ATLAS_OK && r->unbound_reason != NULL) {
        st = atlas_buf_set_str(&out->unbound_reason, r->unbound_reason, err);
    }
    return st;
}

/* --- routing a write ---------------------------------------------------------
 *
 * Exactly one process writes the index, and the data-directory lock is what
 * decides which. So there are two paths and the lock chooses between them:
 *
 *   - this process holds the lock: apply on this thread, through the same
 *     `atlas_decision_apply` the daemon's writer thread calls;
 *   - something else holds it: that something is the daemon, and the operation
 *     goes over the socket to its writer.
 *
 * Both end at one function. There is no third path, and neither branch has a
 * copy of the lifecycle rules. */

static const char *method_for(atlas_decision_op_kind kind) {
    switch (kind) {
    case ATLAS_DECISION_OP_PROPOSE: return "decision.propose";
    case ATLAS_DECISION_OP_REVISE: return "decision.revise";
    case ATLAS_DECISION_OP_CHALLENGE: return "decision.challenge";
    case ATLAS_DECISION_OP_APPROVE: return "decision.approve";
    case ATLAS_DECISION_OP_REJECT: return "decision.reject";
    case ATLAS_DECISION_OP_SUPERSEDE: return "decision.supersede";
    case ATLAS_DECISION_OP_PROMOTE: return "decision.promote";
    case ATLAS_DECISION_OP_REVALIDATE: return "decision.revalidate";
    }
    return "decision.propose";
}

static atlas_status put_buf(atlas_json *j, const char *key, const atlas_buf *b, atlas_err *err) {
    if (b->len == 0) {
        return ATLAS_OK;
    }
    return atlas_json_key_str(j, key, atlas_buf_cstr(b), err);
}

/* Serialises the typed operation into request parameters, through the typed
 * writer. There is still no "write these bytes as JSON" primitive anywhere in
 * Atlas, and this is not the place to introduce one. */
static atlas_status op_to_params(const atlas_decision_op *op, atlas_buf *out, atlas_err *err) {
    atlas_ipc_params *p = NULL;
    atlas_json *j = NULL;
    atlas_status st = atlas_ipc_params_begin(&p, &j, err);
    if (st != ATLAS_OK) {
        return st;
    }
    struct {
        const char *key;
        const atlas_buf *value;
    } scalars[] = {
        {"repo", &op->repo_name},         {"root", &op->root},
        {"decision", &op->uid},           {"replacement", &op->replacement_uid},
        {"token", &op->token},            {"confirmation", &op->confirmation},
        {"provider", &op->provider},      {"client", &op->client},
        {"session_key", &op->session_key}, {"dedup_key", &op->dedup_key},
        {"prior_freshness", &op->prior_freshness},
        {"prior_reasons", &op->prior_reasons},
        {"title", &op->revision.title},   {"context", &op->revision.context_text},
        {"decision_text", &op->revision.decision_text},
        {"rationale", &op->revision.rationale_text},
        {"consequences", &op->revision.consequences_text},
    };
    for (size_t i = 0; st == ATLAS_OK && i < sizeof(scalars) / sizeof(scalars[0]); i++) {
        /* `decision` is the document id and `decision_text` is the prose. The
         * server reads the prose under the key `decision`, so the one that
         * carries prose is renamed on the way out rather than colliding. */
        const char *key = scalars[i].key;
        if (strcmp(key, "decision_text") == 0) {
            if (op->kind == ATLAS_DECISION_OP_PROPOSE) {
                key = "decision";
            } else {
                continue; /* handled below, where the id is not also present */
            }
        }
        st = put_buf(j, key, scalars[i].value, err);
    }
    if (st == ATLAS_OK && op->kind == ATLAS_DECISION_OP_REVISE) {
        /* A revise carries both, so the prose goes under its own key and the
         * server accepts either spelling. */
        st = put_buf(j, "decision_body", &op->revision.decision_text, err);
    }
    if (st == ATLAS_OK && op->revision.scope != ATLAS_DECISION_SCOPE_UNKNOWN) {
        st = atlas_json_key_str(j, "scope", atlas_decision_scope_name(op->revision.scope), err);
    }
    if (st == ATLAS_OK && op->kind == ATLAS_DECISION_OP_CHALLENGE) {
        st = atlas_json_key_str(j, "intent", atlas_decision_intent_name(op->intent), err);
        if (st == ATLAS_OK && op->expect_revision_no > 0) {
            st = atlas_json_key_int(j, "revision", op->expect_revision_no, err);
        }
    }
    if (st == ATLAS_OK && op->kind == ATLAS_DECISION_OP_PROMOTE) {
        st = atlas_json_key_int(j, "legacy_id", op->legacy_id, err);
    }
    if (st == ATLAS_OK && op->revision.alternative_count > 0) {
        st = atlas_json_key(j, "alternatives", err);
        if (st == ATLAS_OK) {
            st = atlas_json_arr_begin(j, err);
        }
        for (size_t i = 0; st == ATLAS_OK && i < op->revision.alternative_count; i++) {
            st = atlas_json_str(j, atlas_buf_cstr(&op->revision.alternatives[i]), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_arr_end(j, err);
        }
    }
    /* Links are re-sent as the caller gave them: paths in the safe text
     * encoding, commits as hex, symbol names as text. The server re-takes the
     * snapshot from its own index, which is what keeps a caller from asserting
     * that a link is current. */
    struct {
        const char *key;
        atlas_decision_link_kind kind;
    } lists[] = {
        {"paths", ATLAS_DECISION_LINK_PATH},
        {"commits", ATLAS_DECISION_LINK_COMMIT},
        {"symbols", ATLAS_DECISION_LINK_SYMBOL},
        /* Decision-to-decision references, as target uids.
         *
         * Absent until now, and silently so: `build_op` created the
         * `relates_to` links, this serialiser dropped them, and the daemon had
         * no parameter to read them from — so under a system deployment every
         * `--decision-link` vanished with a success exit and no diagnostic. The
         * uid is all that travels; the daemon resolves it at the write point,
         * where existence and same-repository are already checked. */
        {"decisions", ATLAS_DECISION_LINK_RELATES_TO},
    };
    for (size_t k = 0; st == ATLAS_OK && k < sizeof(lists) / sizeof(lists[0]); k++) {
        size_t n = 0;
        for (size_t i = 0; i < op->revision.link_count; i++) {
            if (op->revision.links[i].kind == lists[k].kind) {
                n++;
            }
        }
        if (n == 0) {
            continue;
        }
        st = atlas_json_key(j, lists[k].key, err);
        if (st == ATLAS_OK) {
            st = atlas_json_arr_begin(j, err);
        }
        for (size_t i = 0; st == ATLAS_OK && i < op->revision.link_count; i++) {
            const atlas_decision_link *l = &op->revision.links[i];
            if (l->kind != lists[k].kind) {
                continue;
            }
            const atlas_buf *v = l->kind == ATLAS_DECISION_LINK_PATH ? &l->path_text
                                 : l->kind == ATLAS_DECISION_LINK_COMMIT ? &l->commit_oid
                                 : l->kind == ATLAS_DECISION_LINK_RELATES_TO ? &l->target_uid
                                                                             : &l->symbol_name_text;
            st = atlas_json_str(j, atlas_buf_cstr(v), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_arr_end(j, err);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_ipc_params_finish(p, out, err);
    } else {
        atlas_ipc_params_abort(p);
    }
    return st;
}

static atlas_status result_from_response(const atlas_ipc_response *r, atlas_decision_result *res,
                                         atlas_err *err) {
    const char *s = NULL;
    atlas_status st = ATLAS_OK;
    struct {
        const char *key;
        atlas_buf *to;
    } strings[] = {
        {"repo", &res->repo_name},   {"decision", &res->uid},
        {"token", &res->token},      {"title", &res->title},
        {"replaced_by", &res->replaced_by_uid},
    };
    for (size_t i = 0; st == ATLAS_OK && i < sizeof(strings) / sizeof(strings[0]); i++) {
        if (atlas_ipc_result_str(r, strings[i].key, &s) && s != NULL) {
            st = atlas_buf_set_str(strings[i].to, s, err);
        }
    }
    if (st != ATLAS_OK) {
        return st;
    }
    struct {
        const char *key;
        char *to;
        size_t size;
    } fixed[] = {
        {"content_hash", res->content_hash, sizeof(res->content_hash)},
        {"confirm", res->confirm, sizeof(res->confirm)},
        {"expires_at", res->expires_at, sizeof(res->expires_at)},
    };
    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
        if (atlas_ipc_result_str(r, fixed[i].key, &s) && s != NULL) {
            (void)snprintf(fixed[i].to, fixed[i].size, "%s", s);
        }
    }
    if (atlas_ipc_result_str(r, "state", &s) && s != NULL) {
        /* Parsed against the closed vocabulary, with no default: a state the
         * daemon reported that this binary does not know is a version mismatch
         * and must not silently become PROPOSED. */
        if (!atlas_decision_state_parse(s, &res->state)) {
            return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                 "the Atlas daemon reported a decision state this binary does not "
                                 "recognise; the two are different versions");
        }
    }
    (void)atlas_ipc_result_int(r, "revision", &res->revision_no);
    (void)atlas_ipc_result_int(r, "superseded_revision", &res->superseded_revision_no);
    (void)atlas_ipc_result_bool(r, "created", &res->document_created);
    (void)atlas_ipc_result_bool(r, "duplicate", &res->duplicate);
    (void)atlas_ipc_result_bool(r, "session_unbound", &res->session_unbound);
    return ATLAS_OK;
}

/* Takes ownership of `op` unconditionally, matching `atlas_writer_decision`. */
static atlas_status apply_op(atlas_ctx *ctx, atlas_decision_op *op, atlas_decision_result *res,
                             bool *via_daemon_out, atlas_err *err) {
    *via_daemon_out = false;
    /* A7.1: no context at all means the index belongs to the daemon's account
     * and this process cannot open it. There is nothing to be the writer of and
     * no data directory to compare, so the socket is the only path — and the
     * check below about *which* index a daemon owns is answered by there being
     * exactly one the policy names. */
    if (ctx != NULL && atlas_ctx_is_writer(ctx)) {
        atlas_status st = atlas_decision_apply(atlas_ctx_db(ctx), op, res, err);
        atlas_decision_op_free(op);
        free(op);
        return st;
    }

    atlas_buf sock = ATLAS_BUF_INIT;
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_buf resp = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    atlas_status st = atlas_ipc_socket_path(&sock, err);
    /* Owning this data directory, not merely answering: a daemon on another
     * index cannot apply this write, and asking it to would put the record in
     * the wrong place. */
    if (st == ATLAS_OK && ctx != NULL && !atlas_ipc_daemon_owns(atlas_ctx_data_dir(ctx))) {
        st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                           "another Atlas writer owns this index and no daemon for it is answering "
                           "on the IPC socket. Start the daemon (systemctl --user start atlas) or "
                           "wait for the other command to finish.");
    }
    if (st == ATLAS_OK) {
        st = op_to_params(op, &params, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_ipc_call(atlas_buf_cstr(&sock), method_for(op->kind), atlas_buf_cstr(&params),
                            &resp, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_ipc_response_parse(resp.data, resp.len, &r, err);
    }
    if (st == ATLAS_OK && !atlas_ipc_response_ok(r)) {
        /* The daemon's status code is carried through rather than flattened, so
         * a refused approval exits 7 from the CLI just as it would have from
         * the daemon. */
        st = atlas_err_set(err, atlas_ipc_response_status(r), "%s", atlas_ipc_response_message(r));
    }
    if (st == ATLAS_OK) {
        *via_daemon_out = true;
        st = result_from_response(r, res, err);
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&resp);
    atlas_buf_free(&params);
    atlas_buf_free(&sock);
    atlas_decision_op_free(op);
    free(op);
    return st;
}

static atlas_decision_op *op_new(atlas_decision_op_kind kind) {
    atlas_decision_op *op = calloc(1u, sizeof(*op));
    if (op != NULL) {
        atlas_decision_op_init(op, kind);
    }
    return op;
}

atlas_status atlas_service_decision_propose(atlas_ctx *ctx, const char *repo,
                                            const atlas_decision_input *in,
                                            atlas_decision_outcome *out, atlas_err *err) {
    atlas_decision_op *op = op_new(ATLAS_DECISION_OP_PROPOSE);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    atlas_status st = build_op(ctx, repo, NULL, in, op, err);
    if (st != ATLAS_OK) {
        atlas_decision_op_free(op);
        free(op);
        return st;
    }
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    bool via = false;
    st = apply_op(ctx, op, &res, &via, err);
    if (st == ATLAS_OK) {
        st = take_outcome(&res, out, via, false, err);
    }
    atlas_decision_result_free(&res);
    return st;
}

atlas_status atlas_service_decision_revise(atlas_ctx *ctx, const char *repo, const char *uid,
                                           const atlas_decision_input *in,
                                           atlas_decision_outcome *out, atlas_err *err) {
    if (!atlas_decision_uid_is_valid(uid)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "that is not a decision id");
    }
    atlas_decision_op *op = op_new(ATLAS_DECISION_OP_REVISE);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    atlas_status st = atlas_buf_set_str(&op->uid, uid, err);
    if (st == ATLAS_OK) {
        st = build_op(ctx, repo, uid, in, op, err);
    }
    if (st != ATLAS_OK) {
        atlas_decision_op_free(op);
        free(op);
        return st;
    }
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    bool via = false;
    st = apply_op(ctx, op, &res, &via, err);
    if (st == ATLAS_OK) {
        st = take_outcome(&res, out, via, false, err);
    }
    atlas_decision_result_free(&res);
    return st;
}

atlas_status atlas_service_decision_orphans(atlas_ctx *ctx, int64_t limit,
                                            atlas_decision_summary_cb cb, void *ud,
                                            int64_t *count_out, bool *more_out, atlas_err *err) {
    *count_out = 0;
    *more_out = false;
    if (limit <= 0 || limit > ATLAS_DECISION_MAX_ROWS) {
        limit = ATLAS_DECISION_MAX_ROWS;
    }
    list_state ls;
    memset(&ls, 0, sizeof(ls));
    ls.ctx = ctx;
    atlas_safe_pool_init(&ls.safe);
    ls.cb = cb;
    ls.ud = ud;
    /* No repository to resolve: that is the point of the query. */
    atlas_status st = atlas_db_decision_orphans_list(atlas_ctx_db(ctx), limit, on_doc, &ls,
                                                     count_out, more_out, err);
    atlas_safe_pool_free(&ls.safe);
    return st;
}

void atlas_decision_legacy_view_init(atlas_decision_legacy_view *v) {
    memset(v, 0, sizeof(*v));
    atlas_buf_init(&v->title);
    atlas_buf_init(&v->statement);
    atlas_buf_init(&v->provenance);
    atlas_buf_init(&v->created_at);
    atlas_buf_init(&v->imported_uid);
}

void atlas_decision_legacy_view_free(atlas_decision_legacy_view *v) {
    if (v == NULL) {
        return;
    }
    atlas_buf_free(&v->title);
    atlas_buf_free(&v->statement);
    atlas_buf_free(&v->provenance);
    atlas_buf_free(&v->created_at);
    atlas_buf_free(&v->imported_uid);
}

atlas_status atlas_service_decision_promote(atlas_ctx *ctx, const char *repo, int64_t legacy_id,
                                            atlas_decision_outcome *out, atlas_err *err) {
    if (legacy_id <= 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "promote needs the A2 proposal's numeric id, as shown by "
                             "`atlas decision legacy`");
    }
    atlas_decision_op *op = op_new(ATLAS_DECISION_OP_PROMOTE);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    atlas_status st = atlas_buf_set_str(&op->repo_name, repo, err);
    op->legacy_id = legacy_id;
    if (st != ATLAS_OK) {
        atlas_decision_op_free(op);
        free(op);
        return st;
    }
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    bool via = false;
    st = apply_op(ctx, op, &res, &via, err);
    if (st == ATLAS_OK) {
        st = take_outcome(&res, out, via, false, err);
    }
    atlas_decision_result_free(&res);
    return st;
}

typedef struct legacy_state {
    atlas_safe_pool safe;
    atlas_decision_legacy_view_cb cb;
    void *ud;
} legacy_state;

static atlas_status on_legacy(const atlas_decision_legacy_row *row, void *ud, atlas_err *err) {
    legacy_state *ls = (legacy_state *)ud;
    atlas_decision_legacy_view v;
    atlas_decision_legacy_view_init(&v);
    v.id = row->id;
    v.path_count = row->path_count;
    v.imported = row->imported;
    /* `title` and `statement` are model-authored prose from the A2 tables and
     * are encoded here, exactly as A4's own prose is. Being older does not make
     * them trusted. */
    atlas_status st = atlas_buf_set_str(&v.title, atlas_safe(&ls->safe, row->title), err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&v.statement, atlas_safe(&ls->safe, row->statement), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&v.provenance, row->provenance, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&v.created_at, row->created_at, err);
    }
    if (st == ATLAS_OK && row->imported_uid != NULL) {
        st = atlas_buf_set_str(&v.imported_uid, row->imported_uid, err);
    }
    if (st == ATLAS_OK) {
        st = ls->cb(&v, ls->ud, err);
    }
    atlas_decision_legacy_view_free(&v);
    return st;
}

atlas_status atlas_service_decision_legacy(atlas_ctx *ctx, const char *repo, int64_t limit,
                                           atlas_decision_legacy_view_cb cb, void *ud,
                                           int64_t *count_out, bool *more_out, atlas_err *err) {
    *count_out = 0;
    *more_out = false;
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_service_require_repo(ctx, repo, &info, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }
    if (limit <= 0 || limit > ATLAS_DECISION_MAX_ROWS) {
        limit = ATLAS_DECISION_MAX_ROWS;
    }
    legacy_state ls;
    memset(&ls, 0, sizeof(ls));
    atlas_safe_pool_init(&ls.safe);
    ls.cb = cb;
    ls.ud = ud;
    st = atlas_db_decision_legacy_list(atlas_ctx_db(ctx), info.id, false, limit, on_legacy, &ls,
                                       count_out, more_out, err);
    atlas_safe_pool_free(&ls.safe);
    atlas_repo_info_free(&info);
    return st;
}

/* --- the operator channel ---------------------------------------------------------
 *
 * **What this establishes.** That a confirmation matching one revision's
 * content hash was typed at a controlling terminal, and that a capability bound
 * to that exact (repository, document, revision, content hash) tuple was spent
 * exactly once to record the transition.
 *
 * **What it does not.** Which person typed it, or that a person typed it at
 * all. Any process running as the same user can allocate a pseudo-terminal and
 * drive this — Atlas' own tests do — so the actor recorded is
 * `LOCAL_OPERATOR_CONFIRMED`, which names a channel rather than a person. There
 * are no keys and no signatures in A4, and there is no non-repudiation.
 *
 * What it excludes is real and is the whole claim: an approval cannot be
 * produced by a model's text, a hook payload, an MCP tool call, a repository
 * file, an environment variable, `--yes`, piped standard input, or a replayed
 * request. */

/* Writes the confirmation prompt. Every untrusted value is encoded before it
 * gets here, and `atlas_terminal_write` refuses a byte a terminal would act on
 * regardless — belt and braces, on the one display where an escape sequence
 * could rewrite what the operator thinks they are agreeing to. */
static atlas_status show_prompt(atlas_terminal *t, const char *repo, const char *uid,
                                const atlas_decision_document *doc, atlas_decision_intent intent,
                                const char *replacement_uid, const char *confirm,
                                const atlas_gate_assessment *assessment,
                                const atlas_decision_result *issued, atlas_err *err) {
    atlas_status st = atlas_terminal_writef(
        t, err, "\nAtlas decision %s\n  repository : %s\n  decision   : %s\n",
        atlas_decision_intent_name(intent), repo, uid);
    if (st == ATLAS_OK) {
        st = atlas_terminal_writef(t, err, "  revision   : %lld\n  status     : %s\n",
                                   (long long)doc->summary.revision_no,
                                   atlas_buf_cstr(&doc->summary.status));
    }
    if (st == ATLAS_OK) {
        st = atlas_terminal_writef(t, err, "  digest     : %s\n",
                                   atlas_buf_cstr(&doc->summary.content_hash));
    }
    if (st == ATLAS_OK) {
        /* **The approval policy for a revision with no captured repository
         * identity: allow it, and say so.**
         *
         * Refusing would make a decision unapprovable until a scan completed,
         * and the approval still binds to the content hash, which covers the
         * captured identity *including* its absence. What the absence costs is
         * one specific thing: the approval does not bind the content to a
         * non-empty repository identity, so the document is not auto-relinked
         * after a `repo remove`.
         *
         * The wording below states exactly that and stops. It makes no claim
         * about whether the decision should be approved, and no reassurance of
         * any kind: the content is untrusted project prose Atlas is about to
         * display and has not judged, so a general endorsement printed beside
         * it would be Atlas vouching for something outside what it can know.
         * The narrow factual consequence is the whole of what Atlas is in a
         * position to say. `tests/test_decision_mcp.c` scans for the wider
         * phrasings. */
        if (doc->basis_repo_identity.len > 0) {
            st = atlas_terminal_writef(t, err, "  repository : %s\n",
                                       atlas_buf_cstr(&doc->basis_repo_identity));
        } else {
            static const char UNKNOWN[] =
                "  repository : identity not captured\n"
                "               This revision has no captured repository identity. Approval\n"
                "               covers the displayed content, but does not bind it to a\n"
                "               non-empty repository identity and cannot provide automatic\n"
                "               reattachment after repository removal.\n";
            st = atlas_terminal_write(t, UNKNOWN, sizeof(UNKNOWN) - 1u, err);
        }
    }
    if (st == ATLAS_OK && replacement_uid != NULL) {
        st = atlas_terminal_writef(t, err, "  replaced by: %s\n", replacement_uid);
    }
    if (st == ATLAS_OK) {
        /* The title is prose somebody else wrote, so it is labelled as such
         * rather than presented as part of Atlas' own prompt. */
        st = atlas_terminal_writef(t, err, "\n  title (untrusted project text):\n    %s\n",
                                   atlas_buf_cstr(&doc->summary.title));
    }
    if (st == ATLAS_OK && doc->decision_text.len > 0) {
        st = atlas_terminal_writef(t, err, "\n  decision (untrusted project text):\n    %s\n",
                                   atlas_buf_cstr(&doc->decision_text));
    }
    if (st == ATLAS_OK && doc->links_needing_review > 0) {
        st = atlas_terminal_writef(t, err,
                                   "\n  note: %lld of this revision's links no longer match the "
                                   "code they were recorded against.\n",
                                   (long long)doc->links_needing_review);
    }
    if (st == ATLAS_OK && !doc->ledger_agrees) {
        static const char WARN[] =
            "\n  note: this decision's cached status disagrees with its event ledger. Run "
            "`atlas doctor` before proceeding.\n";
        st = atlas_terminal_write(t, WARN, sizeof(WARN) - 1u, err);
    }
    if (st == ATLAS_OK && assessment != NULL) {
        /* --- A6: what is being revalidated, and against what -----------------
         *
         * Every value below is Atlas-owned: two closed vocabularies, two object
         * ids Atlas minted or read from its own index, and integers it counted.
         * No repository prose reaches this block, which is why it can be
         * printed as Atlas' own statement rather than labelled untrusted the
         * way the title and the decision text above are.
         *
         * The wording is careful in the direction the phase requires. A stale
         * assessment is a statement about anchors that moved, not a finding
         * that the decision was wrong — so the prompt says what changed and
         * declines to say what it means. */
        st = atlas_terminal_writef(
            t, err,
            "\n  freshness  : %s\n  because    : %s\n  validated  : %s\n  against    : %s\n",
            atlas_gate_freshness_name(assessment->freshness),
            assessment->reason_count > 0 ? atlas_gate_reason_name(assessment->reasons[0])
                                         : "NO_RELEVANT_CHANGE",
            assessment->validated_at_commit[0] != '\0' ? assessment->validated_at_commit
                                                       : "no recorded validation point",
            issued != NULL && issued->indexed_commit[0] != '\0' ? issued->indexed_commit
                                                                : "an unindexed repository");
        for (size_t i = 1; st == ATLAS_OK && i < assessment->reason_count; i++) {
            st = atlas_terminal_writef(t, err, "               %s\n",
                                       atlas_gate_reason_name(assessment->reasons[i]));
        }
        if (st == ATLAS_OK) {
            static const char WHAT[] =
                "\nRevalidating records that this decision was checked against the repository\n"
                "state above. It does not edit the approved revision, does not change its\n"
                "status, and does not withdraw the assessment — the assessment and its reasons\n"
                "are kept alongside the new record. Atlas has not judged whether the decision\n"
                "is still correct and cannot: it observed that what the decision is bound to\n"
                "has moved, and that is the whole of what it is telling you.\n";
            st = atlas_terminal_write(t, WHAT, sizeof(WHAT) - 1u, err);
        }
    }
    if (st == ATLAS_OK) {
        /* Atlas' own statement about what the operator is about to record.
         * It is here rather than only in the documentation because this is the
         * moment somebody could believe Atlas is identifying them. */
        static const char NOTE[] =
            "\nAtlas will record this as LOCAL_OPERATOR_CONFIRMED. That means the action came\n"
            "through this interactive channel. It does not identify you, does not prove a\n"
            "person was present, and is not a signature.\n";
        st = atlas_terminal_write(t, NOTE, sizeof(NOTE) - 1u, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_terminal_writef(t, err,
                                   "\nType %s to %s this exact revision, or anything else to "
                                   "abandon: ",
                                   confirm, atlas_decision_intent_name(intent));
    }
    return st;
}

atlas_status atlas_service_decision_confirm(atlas_ctx *ctx, const char *repo, const char *uid,
                                            atlas_decision_intent intent,
                                            const char *replacement_uid, int64_t revision_no,
                                            atlas_decision_outcome *out, atlas_err *err) {
    /* The terminal first, before a capability is issued.
     *
     * A challenge issued to a non-interactive caller would be a capability
     * lying around with nobody to spend it, and the refusal is clearer here
     * anyway: the caller learns it needs a terminal rather than that its
     * confirmation was wrong. */
    atlas_terminal *t = NULL;
    atlas_status st = atlas_terminal_open(&t, err);
    if (st != ATLAS_OK) {
        return st;
    }

    /* --- A6: the assessment being revalidated ------------------------------
     *
     * Computed before the capability is issued, and carried into it, so that
     * what the validation record preserves is what the operator was actually
     * shown. Recomputing it at the write point would record whatever was true a
     * moment later, which is not what anybody confirmed.
     *
     * Nothing here can produce an assessment: this reads the same engine
     * `atlas gate check` reads, through the same snapshot discipline, and its
     * result is displayed rather than acted on. */
    atlas_gate_report assessment;
    atlas_gate_report_init(&assessment);
    if (intent == ATLAS_DECISION_INTENT_REVALIDATE) {
        st = ctx != NULL ? atlas_service_gate_show(ctx, repo, uid, NULL, &assessment, err)
                         : atlas_service_gate_show_remote(repo, uid, &assessment, err);
        if (st != ATLAS_OK) {
            atlas_gate_report_free(&assessment);
            atlas_terminal_close(t);
            return st;
        }
    }

    /* Issue the capability. */
    atlas_decision_op *op = op_new(ATLAS_DECISION_OP_CHALLENGE);
    if (op == NULL) {
        atlas_gate_report_free(&assessment);
        atlas_terminal_close(t);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    if (intent == ATLAS_DECISION_INTENT_REVALIDATE && assessment.item_count == 1) {
        st = atlas_buf_set_str(&op->prior_freshness,
                               atlas_gate_freshness_name(assessment.items[0].freshness), err);
        if (st == ATLAS_OK) {
            st = atlas_gate_reasons_pack(&assessment.items[0], &op->prior_reasons, err);
        }
        if (st != ATLAS_OK) {
            atlas_decision_op_free(op);
            free(op);
            atlas_gate_report_free(&assessment);
            atlas_terminal_close(t);
            return st;
        }
    }
    st = atlas_buf_set_str(&op->repo_name, repo, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op->uid, uid, err);
    }
    if (st == ATLAS_OK && replacement_uid != NULL) {
        st = atlas_buf_set_str(&op->replacement_uid, replacement_uid, err);
    }
    op->intent = intent;
    op->expect_revision_no = revision_no;
    if (st != ATLAS_OK) {
        atlas_decision_op_free(op);
        free(op);
        atlas_gate_report_free(&assessment);
        atlas_terminal_close(t);
        return st;
    }
    atlas_decision_result issued;
    atlas_decision_result_init(&issued);
    bool via = false;
    st = apply_op(ctx, op, &issued, &via, err);
    if (st != ATLAS_OK) {
        atlas_decision_result_free(&issued);
        atlas_gate_report_free(&assessment);
        atlas_terminal_close(t);
        return st;
    }

    /* Read the exact revision the capability names, so the prompt shows what
     * the capability is bound to rather than what is newest. */
    atlas_decision_document doc;
    atlas_decision_document_init(&doc);
    st = ctx != NULL ? atlas_service_decision_show(ctx, repo, uid, issued.revision_no, &doc, err)
                     : atlas_service_decision_show_remote(repo, uid, issued.revision_no, &doc, err);
    if (st == ATLAS_OK) {
        st = show_prompt(t, repo, uid, &doc, intent, replacement_uid, issued.confirm,
                         intent == ATLAS_DECISION_INTENT_REVALIDATE && assessment.item_count == 1
                             ? &assessment.items[0]
                             : NULL,
                         &issued, err);
    }

    atlas_buf answer = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_terminal_read_line(t, &answer, ATLAS_DECISION_CONFIRM_MAX - 1u, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_terminal_write(t, "\n", 1u, err);
    }
    atlas_terminal_close(t);
    t = NULL;
    atlas_decision_document_free(&doc);
    atlas_gate_report_free(&assessment);
    if (st != ATLAS_OK) {
        atlas_buf_free(&answer);
        atlas_decision_result_free(&issued);
        return st;
    }

    /* The comparison happens at the write point too, against the stored hash.
     * This one is here so that a mistyped confirmation costs a message rather
     * than a round trip, and so the capability is not spent by a typo. */
    if (answer.len != strlen(issued.confirm) ||
        strncmp(atlas_buf_cstr(&answer), issued.confirm, strlen(issued.confirm)) != 0) {
        atlas_buf_free(&answer);
        atlas_decision_result_free(&issued);
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "that is not the confirmation for this revision; nothing was changed");
    }

    /* Spend it. */
    static const atlas_decision_op_kind KIND[] = {
        [ATLAS_DECISION_INTENT_APPROVE] = ATLAS_DECISION_OP_APPROVE,
        [ATLAS_DECISION_INTENT_REJECT] = ATLAS_DECISION_OP_REJECT,
        [ATLAS_DECISION_INTENT_SUPERSEDE] = ATLAS_DECISION_OP_SUPERSEDE,
        [ATLAS_DECISION_INTENT_REVALIDATE] = ATLAS_DECISION_OP_REVALIDATE,
    };
    atlas_decision_op *spend = op_new(KIND[intent]);
    if (spend == NULL) {
        atlas_buf_free(&answer);
        atlas_decision_result_free(&issued);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    st = atlas_buf_set_str(&spend->repo_name, repo, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&spend->uid, uid, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&spend->token, issued.token.data, issued.token.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&spend->confirmation, answer.data, answer.len, err);
    }
    atlas_buf_free(&answer);
    if (st != ATLAS_OK) {
        atlas_decision_op_free(spend);
        free(spend);
        atlas_decision_result_free(&issued);
        return st;
    }
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    st = apply_op(ctx, spend, &res, &via, err);
    if (st == ATLAS_OK) {
        st = take_outcome(&res, out, via, true, err);
    }
    atlas_decision_result_free(&res);
    atlas_decision_result_free(&issued);
    return st;
}

/* --- export ------------------------------------------------------------------------
 *
 * To a stream, never into the target repository. Atlas is read-only with
 * respect to a registered worktree, and a decision document is Atlas' record
 * rather than the project's file — writing one would make Atlas the author of
 * something the project then has to maintain. */

static void write_section(FILE *out, const char *heading, const atlas_buf *body) {
    if (body->len == 0) {
        return;
    }
    (void)fprintf(out, "\n## %s\n\n%s\n", heading, atlas_buf_cstr(body));
}

atlas_status atlas_service_decision_export_markdown(const atlas_decision_document *doc, FILE *out,
                                                    atlas_err *err) {
    (void)err;
    const atlas_decision_summary *s = &doc->summary;
    /* Every value here is either Atlas-owned or already safe-encoded by the
     * service layer. The document is Markdown, so a body containing Markdown is
     * rendered as Markdown — which is what an export is for, and is not a
     * terminal-safety question. */
    (void)fprintf(out, "# %s\n\n", atlas_buf_cstr(&s->title));
    (void)fprintf(out, "- id: `%s`\n", atlas_buf_cstr(&s->uid));
    (void)fprintf(out, "- repository: `%s`\n", atlas_buf_cstr(&doc->repo));
    (void)fprintf(out, "- status: **%s**\n", atlas_buf_cstr(&s->status));
    (void)fprintf(out, "- revision: %lld of %lld (%s)\n", (long long)s->revision_no,
                  (long long)s->latest_revision_no, atlas_buf_cstr(&s->revision_state));
    (void)fprintf(out, "- content hash: `%s`\n", atlas_buf_cstr(&s->content_hash));
    (void)fprintf(out, "- proposed by: %s\n", atlas_buf_cstr(&s->proposed_by));
    (void)fprintf(out, "- scope: %s\n", atlas_buf_cstr(&doc->scope));
    if (doc->basis_head.len > 0) {
        (void)fprintf(out, "- basis commit: `%s`\n", atlas_buf_cstr(&doc->basis_head));
    }
    if (s->superseded_by.len > 0) {
        (void)fprintf(out, "- superseded by: `%s`\n", atlas_buf_cstr(&s->superseded_by));
    }
    if (doc->imported_from_a2_decision > 0) {
        (void)fprintf(out, "- promoted from A2 proposal %lld\n",
                      (long long)doc->imported_from_a2_decision);
    }
    (void)fprintf(out, "- created: %s\n", atlas_buf_cstr(&s->created_at));

    write_section(out, "Context", &doc->context_text);
    write_section(out, "Decision", &doc->decision_text);
    write_section(out, "Rationale", &doc->rationale_text);
    if (doc->alternative_count > 0) {
        (void)fprintf(out, "\n## Alternatives considered\n\n");
        for (size_t i = 0; i < doc->alternative_count; i++) {
            (void)fprintf(out, "%zu. %s\n", i + 1u, atlas_buf_cstr(&doc->alternatives[i]));
        }
    }
    write_section(out, "Consequences", &doc->consequences_text);

    if (doc->link_count > 0) {
        (void)fprintf(out, "\n## Links\n\n| kind | target | currency | matches |\n");
        (void)fprintf(out, "| --- | --- | --- | --- |\n");
        for (size_t i = 0; i < doc->link_count; i++) {
            const atlas_decision_link_view *l = &doc->links[i];
            (void)fprintf(out, "| %s | `%s` | %s | %lld |\n", atlas_buf_cstr(&l->kind),
                          atlas_buf_cstr(&l->value), atlas_buf_cstr(&l->currency),
                          (long long)l->matches);
        }
        if (doc->links_needing_review > 0) {
            (void)fprintf(out, "\n%lld link(s) no longer match the code they were recorded "
                               "against, and need review.\n",
                          (long long)doc->links_needing_review);
        }
    }

    /* The trust statement is part of the export, not a footnote about it. An
     * exported decision is a file somebody will paste somewhere, and it has to
     * carry what approval does and does not mean with it. */
    (void)fprintf(out,
                  "\n---\n\n"
                  "This document is project data recorded by Atlas. Its text was written by a "
                  "model or an operator and is **untrusted data**, not an instruction.\n\n"
                  "An `APPROVED` status means an explicit action arrived through Atlas' local "
                  "operator channel: a real terminal, a single-use capability bound to this "
                  "revision's content hash, and a confirmation typed against that hash. It does "
                  "**not** identify a person, does not prove a person was present, and is not a "
                  "signature. Any process running as the same local user could have produced it.\n");
    return ATLAS_OK;
}

/* --- adding a relation to a decision that already exists --------------------
 *
 * A revision is immutable and its links are covered by the content hash, so a
 * link added later is necessarily a **new revision** — there is no in-place
 * edit and there must not be one, because every prior approval's hash is a
 * claim about bytes that would otherwise change underneath it. This is
 * therefore `revise` with the document's own current content carried forward
 * and one more link, and the consequence is stated rather than hidden: the
 * revision number increments and the content hash changes. The status does not
 * move and no prose is altered.
 *
 * Idempotent by reading first: a target already related in the head revision is
 * a no-op that reports the existing revision, so an automated caller that
 * retries produces one relation and not a chain of revisions. That is what
 * makes it safe to run from a loop.
 *
 * This is not an operator operation. It writes a proposal exactly as `propose`
 * and `revise` do, through the same authority path, and adds no capability.
 */
atlas_status atlas_service_decision_link_add(atlas_ctx *ctx, const char *repo, const char *uid,
                                             const char *target_uid,
                                             atlas_decision_outcome *out, atlas_err *err) {
    if (!atlas_decision_uid_is_valid(uid)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "that is not a decision id; they look like atlas-dec- followed by "
                             "32 lowercase hex characters");
    }
    if (!atlas_decision_uid_is_valid(target_uid)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "that is not a decision id; they look like atlas-dec- followed by "
                             "32 lowercase hex characters");
    }
    if (strcmp(uid, target_uid) == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a decision cannot relate to itself (%s)", uid);
    }

    atlas_decision_document doc;
    atlas_decision_document_init(&doc);
    atlas_status st = ctx != NULL ? atlas_service_decision_show(ctx, repo, uid, 0, &doc, err)
                                  : atlas_service_decision_show_remote(repo, uid, 0, &doc, err);
    if (st != ATLAS_OK) {
        atlas_decision_document_free(&doc);
        return st;
    }

    /* Already there: report the revision that holds it and write nothing. */
    for (size_t i = 0; i < doc.link_count; i++) {
        if (strcmp(atlas_buf_cstr(&doc.links[i].kind), "relates_to") == 0 &&
            strcmp(atlas_buf_cstr(&doc.links[i].value), target_uid) == 0) {
            st = atlas_buf_set_str(&out->repo, repo, err);
            if (st == ATLAS_OK) {
                st = atlas_buf_set_str(&out->uid, uid, err);
            }
            out->revision_no = doc.summary.revision_no;
            out->duplicate = true;
            atlas_decision_document_free(&doc);
            return st;
        }
    }

    /* Carry every link the head revision holds, so a new revision loses none of
     * them, and append the new relation. The other kinds are rebuilt from their
     * stored values; a symbol link's snapshot is retaken by `build_op`, which
     * is the same thing `revise` does for any other reason. */
    const char *paths[ATLAS_DECISION_MAX_LINKS];
    const char *commits[ATLAS_DECISION_MAX_LINKS];
    const char *symbols[ATLAS_DECISION_MAX_LINKS];
    const char *relations[ATLAS_DECISION_MAX_LINKS];
    size_t np = 0, nc = 0, ns = 0, nr = 0;
    for (size_t i = 0; i < doc.link_count; i++) {
        const char *k = atlas_buf_cstr(&doc.links[i].kind);
        const char *v = atlas_buf_cstr(&doc.links[i].value);
        if (strcmp(k, "path") == 0 && np < ATLAS_DECISION_MAX_LINKS) {
            paths[np++] = v;
        } else if (strcmp(k, "commit") == 0 && nc < ATLAS_DECISION_MAX_LINKS) {
            commits[nc++] = v;
        } else if (strcmp(k, "symbol") == 0 && ns < ATLAS_DECISION_MAX_LINKS) {
            symbols[ns++] = v;
        } else if (strcmp(k, "relates_to") == 0 && nr < ATLAS_DECISION_MAX_LINKS) {
            relations[nr++] = v;
        }
        /* `supersedes` and `replaced_by` are written by the lifecycle, never
         * carried forward by a proposal: reproducing one here would assert a
         * transition nobody performed. */
    }
    if (nr >= ATLAS_DECISION_MAX_LINKS) {
        atlas_decision_document_free(&doc);
        return atlas_err_set(err, ATLAS_ERR_USAGE, "at most %d decision relations",
                             ATLAS_DECISION_MAX_LINKS);
    }
    relations[nr++] = target_uid;

    const char *alts[ATLAS_DECISION_MAX_ALTERNATIVES];
    for (size_t i = 0; i < doc.alternative_count; i++) {
        alts[i] = atlas_buf_cstr(&doc.alternatives[i]);
    }

    atlas_decision_input in;
    memset(&in, 0, sizeof in);
    in.title = atlas_buf_cstr(&doc.summary.title);
    in.context_text = doc.context_text.len > 0 ? atlas_buf_cstr(&doc.context_text) : NULL;
    in.decision_text = atlas_buf_cstr(&doc.decision_text);
    in.rationale_text = doc.rationale_text.len > 0 ? atlas_buf_cstr(&doc.rationale_text) : NULL;
    in.consequences_text =
        doc.consequences_text.len > 0 ? atlas_buf_cstr(&doc.consequences_text) : NULL;
    in.scope = doc.scope.len > 0 ? atlas_buf_cstr(&doc.scope) : NULL;
    in.alternatives = alts;
    in.alternative_count = doc.alternative_count;
    in.paths = paths;
    in.path_count = np;
    in.commits = commits;
    in.commit_count = nc;
    in.symbols = symbols;
    in.symbol_count = ns;
    in.decision_links = relations;
    in.decision_link_count = nr;

    st = atlas_service_decision_revise(ctx, repo, uid, &in, out, err);
    atlas_decision_document_free(&doc);
    return st;
}
