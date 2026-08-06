/* Atlas - application/service layer.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * All command behaviour lives here so that the human renderer, the JSON
 * renderer and any future adapter observe exactly the same results.
 */
#include "atlas/service.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/lock.h"
#include "atlas/pathrep.h"
#include "core/service_internal.h"

struct atlas_ctx {
    atlas_buf data_dir;
    atlas_buf db_path;
    atlas_datadir_source data_dir_source;
    atlas_db *db;
    atlas_lock *lock; /* NULL when this context is not the writer */
};

atlas_status atlas_ctx_open(const atlas_ctx_opts *opts, atlas_ctx **out, atlas_err *err) {
    *out = NULL;
    atlas_ctx *ctx = calloc(1u, sizeof(*ctx));
    if (ctx == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory opening Atlas context");
    }
    atlas_buf_init(&ctx->data_dir);
    atlas_buf_init(&ctx->db_path);

    const char *override = (opts != NULL) ? opts->data_dir_override : NULL;
    atlas_ctx_mode mode = (opts != NULL) ? opts->mode : ATLAS_CTX_AUTO;
    atlas_status st =
        atlas_datadir_resolve(override, &ctx->data_dir, &ctx->data_dir_source, err);
    if (st == ATLAS_OK) {
        st = atlas_datadir_ensure(atlas_buf_cstr(&ctx->data_dir), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_datadir_db_path(atlas_buf_cstr(&ctx->data_dir), &ctx->db_path, err);
    }
    if (st != ATLAS_OK) {
        atlas_ctx_close(ctx);
        return st;
    }

    /* Exactly one process writes at a time.
     *
     * ATLAS_CTX_WRITE fails outright when the lock is held, because a mutation
     * that races the daemon would interleave generations and leave the index
     * describing a state that never existed. ATLAS_CTX_AUTO instead degrades to
     * a read-only handle, which is what lets every read command keep working
     * while the daemon owns the writer. */
    if (mode != ATLAS_CTX_READ) {
        atlas_err lock_err;
        atlas_err_init(&lock_err);
        atlas_status lst =
            atlas_lock_acquire(atlas_buf_cstr(&ctx->data_dir),
                               mode == ATLAS_CTX_WRITE ? ATLAS_LOCK_ROLE_ONESHOT
                                                       : ATLAS_LOCK_ROLE_ONESHOT,
                               &ctx->lock, &lock_err);
        if (lst != ATLAS_OK) {
            if (mode == ATLAS_CTX_WRITE) {
                *err = lock_err;
                atlas_ctx_close(ctx);
                return lst;
            }
            ctx->lock = NULL; /* fall through to a read-only handle */
        }
    }

    if (ctx->lock != NULL) {
        st = atlas_db_open(atlas_buf_cstr(&ctx->db_path), &ctx->db, err);
        if (st == ATLAS_OK) {
            st = atlas_db_migrate(ctx->db, err);
        }
    } else {
        st = atlas_db_open_readonly(atlas_buf_cstr(&ctx->db_path), &ctx->db, err);
        if (st == ATLAS_OK) {
            /* Cannot migrate without the lock; verify instead, so a version skew
             * is an explanation rather than a confusing missing-table error. */
            st = atlas_db_migrate(ctx->db, err);
        }
    }
    if (st != ATLAS_OK) {
        atlas_ctx_close(ctx);
        return st;
    }
    *out = ctx;
    return ATLAS_OK;
}

void atlas_ctx_close(atlas_ctx *ctx) {
    if (ctx == NULL) {
        return;
    }
    atlas_db_close(ctx->db);
    /* Released after the database handle, so the lock still covers the last
     * write's WAL checkpoint. */
    atlas_lock_release(ctx->lock);
    atlas_buf_free(&ctx->data_dir);
    atlas_buf_free(&ctx->db_path);
    free(ctx);
}

bool atlas_ctx_is_writer(const atlas_ctx *ctx) {
    return ctx != NULL && ctx->lock != NULL;
}

const char *atlas_ctx_data_dir(const atlas_ctx *ctx) {
    return atlas_buf_cstr(&ctx->data_dir);
}

const char *atlas_ctx_db_path(const atlas_ctx *ctx) {
    return atlas_buf_cstr(&ctx->db_path);
}

atlas_datadir_source atlas_ctx_data_dir_source(const atlas_ctx *ctx) {
    return ctx->data_dir_source;
}

atlas_db *atlas_ctx_db(atlas_ctx *ctx) {
    return ctx->db;
}

atlas_status atlas_service_require_repo(atlas_ctx *ctx, const char *name, atlas_repo_info *out,
                                        atlas_err *err) {
    bool found = false;
    atlas_status st = atlas_db_repo_get(ctx->db, name, out, &found, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (!found) {
        return atlas_err_set(err, ATLAS_ERR_REPO,
                             "no repository named \"%s\" is registered (try: atlas repo list)",
                             name);
    }
    return ATLAS_OK;
}

atlas_status atlas_service_open_repo_git(const atlas_repo_info *info, atlas_git **out,
                                         atlas_err *err) {
    atlas_status st = atlas_git_open(atlas_buf_cstr(&info->root_path), out, err);
    if (st != ATLAS_OK) {
        return st;
    }
    /* The registered root must still be the canonical root. If the path now
     * resolves elsewhere the registration is stale, and proceeding would report
     * one repository's facts under another's name. This also catches a linked
     * worktree that has been moved or pruned. */
    const char *root = atlas_git_root(*out);
    if (strlen(root) != info->root_path.len ||
        memcmp(root, info->root_path.data, info->root_path.len) != 0) {
        st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                           "registered root %s now resolves to %s; re-register the repository",
                           atlas_buf_cstr(&info->root_path_text), root);
        atlas_git_close(*out);
        *out = NULL;
        return st;
    }
    /* A worktree's own git dir is part of its identity: if it changed, this is no
     * longer the same worktree even though the path matches. */
    if (info->git_dir.len > 0) {
        const char *gdir = atlas_git_dir(*out);
        if (strlen(gdir) != info->git_dir.len ||
            memcmp(gdir, info->git_dir.data, info->git_dir.len) != 0) {
            st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                               "the git directory for %s changed since registration; "
                               "re-register the worktree",
                               atlas_buf_cstr(&info->root_path_text));
            atlas_git_close(*out);
            *out = NULL;
            return st;
        }
    }
    return ATLAS_OK;
}

/* --- doctor -------------------------------------------------------------- */

void atlas_doctor_report_init(atlas_doctor_report *r) {
    memset(r, 0, sizeof(*r));
    atlas_buf_init(&r->git_exe);
    atlas_buf_init(&r->git_version);
    atlas_buf_init(&r->data_dir);
    atlas_buf_init(&r->db_path);
    atlas_buf_init(&r->integrity);
    atlas_buf_init(&r->foreign_key_check);
    atlas_buf_init(&r->problems);
}

void atlas_doctor_report_free(atlas_doctor_report *r) {
    if (r == NULL) {
        return;
    }
    atlas_buf_free(&r->git_exe);
    atlas_buf_free(&r->git_version);
    atlas_buf_free(&r->data_dir);
    atlas_buf_free(&r->db_path);
    atlas_buf_free(&r->integrity);
    atlas_buf_free(&r->foreign_key_check);
    atlas_buf_free(&r->problems);
}

static atlas_status add_problem(atlas_doctor_report *r, atlas_err *err, const char *text) {
    r->ok = false;
    if (r->problems.len > 0) {
        atlas_status st = atlas_buf_append_ch(&r->problems, '\n', err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    return atlas_buf_append_str(&r->problems, text, err);
}

static atlas_status count_repos_cb(const atlas_repo_info *ri, void *ud, atlas_err *err) {
    (void)ri;
    (void)err;
    (*(int64_t *)ud)++;
    return ATLAS_OK;
}

atlas_status atlas_service_doctor(atlas_ctx *ctx, atlas_doctor_report *out, atlas_err *err) {
    out->ok = true;
    (void)snprintf(out->atlas_version, sizeof(out->atlas_version), "%s", ATLAS_VERSION_STRING);
    (void)snprintf(out->build_compiler, sizeof(out->build_compiler), "%s", atlas_build_compiler());
    (void)snprintf(out->sqlite_runtime, sizeof(out->sqlite_runtime), "%s", sqlite3_libversion());
    (void)snprintf(out->sqlite_compiled, sizeof(out->sqlite_compiled), "%s", SQLITE_VERSION);

    atlas_err git_err;
    atlas_err_init(&git_err);
    atlas_status st = atlas_git_probe(&out->git_exe, &out->git_version, &git_err);
    out->git_found = (st == ATLAS_OK);
    if (!out->git_found) {
        atlas_status ast = add_problem(out, err, atlas_err_msg(&git_err));
        if (ast != ATLAS_OK) {
            return ast;
        }
    }

    st = atlas_buf_set_str(&out->data_dir, atlas_ctx_data_dir(ctx), err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&out->db_path, atlas_ctx_db_path(ctx), err);
    }
    if (st != ATLAS_OK) {
        return st;
    }
    out->data_dir_source = ctx->data_dir_source;

    const atlas_db_caps *caps = atlas_db_caps_of(ctx->db);
    out->fts5 = caps->fts5;
    out->wal = caps->wal;
    out->foreign_keys = caps->foreign_keys;
    (void)snprintf(out->journal_mode, sizeof(out->journal_mode), "%s", caps->journal_mode);
    out->search_mode = caps->fts5 ? ATLAS_SEARCH_FTS5 : ATLAS_SEARCH_DEGRADED_LIKE;

    out->expected_schema_version = ATLAS_SCHEMA_VERSION;
    out->schema_version = atlas_db_schema_version(ctx->db, err);
    out->db_ok = (out->schema_version == ATLAS_SCHEMA_VERSION);
    if (out->schema_version < 0) {
        return err->status;
    }
    if (!out->db_ok) {
        char msg[160];
        (void)snprintf(msg, sizeof(msg), "database schema is at version %d, expected %d",
                       out->schema_version, ATLAS_SCHEMA_VERSION);
        st = add_problem(out, err, msg);
        if (st != ATLAS_OK) {
            return st;
        }
    }

    st = atlas_db_integrity_check(ctx->db, &out->integrity, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (strcmp(atlas_buf_cstr(&out->integrity), "ok") != 0) {
        st = add_problem(out, err, "sqlite integrity_check did not report ok");
        if (st != ATLAS_OK) {
            return st;
        }
    }
    st = atlas_db_foreign_key_check(ctx->db, &out->foreign_key_check, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (strcmp(atlas_buf_cstr(&out->foreign_key_check), "ok") != 0) {
        st = add_problem(out, err, "sqlite foreign_key_check reported violations");
        if (st != ATLAS_OK) {
            return st;
        }
    }
    if (!caps->foreign_keys) {
        st = add_problem(out, err, "foreign key enforcement is off");
        if (st != ATLAS_OK) {
            return st;
        }
    }
    if (!caps->fts5) {
        /* Not a failure: search degrades and says so. */
        st = add_problem(out, err,
                         "FTS5 is unavailable in this SQLite build; search falls back to "
                         "unranked substring matching");
        if (st != ATLAS_OK) {
            return st;
        }
    }

    out->repo_count = 0;
    return atlas_db_repo_list(ctx->db, count_repos_cb, &out->repo_count, err);
}

/* --- repositories -------------------------------------------------------- */

/* Derives a usable repository name from the canonical root's basename. */
static atlas_status derive_name(const char *root, char *out, size_t out_size, atlas_err *err) {
    const char *base = strrchr(root, '/');
    base = (base != NULL) ? base + 1 : root;
    size_t n = 0;
    for (const char *p = base; *p != '\0' && n + 1u < out_size; p++) {
        unsigned char c = (unsigned char)*p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-';
        if (ok) {
            out[n++] = (char)c;
        }
    }
    out[n] = '\0';
    if (n == 0 || out[0] == '-' || out[0] == '.') {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "cannot derive a repository name from \"%s\"; pass --name NAME", root);
    }
    return atlas_db_check_repo_name(out, err);
}

atlas_status atlas_service_repo_add(atlas_ctx *ctx, const char *path, const char *name,
                                    atlas_repo_info *out, atlas_err *err) {
    return atlas_service_repo_add_db(ctx->db, path, name, out, err);
}

atlas_status atlas_service_repo_add_db(atlas_db *db, const char *path, const char *name,
                                       atlas_repo_info *out, atlas_err *err) {
    atlas_git *g = NULL;
    atlas_status st = atlas_git_open(path, &g, err);
    if (st != ATLAS_OK) {
        return st;
    }

    char derived[ATLAS_NAME_MAX + 1u];
    const char *effective = name;
    if (effective == NULL) {
        st = derive_name(atlas_git_root(g), derived, sizeof(derived), err);
        if (st != ATLAS_OK) {
            atlas_git_close(g);
            return st;
        }
        effective = derived;
    } else {
        st = atlas_db_check_repo_name(effective, err);
        if (st != ATLAS_OK) {
            atlas_git_close(g);
            return st;
        }
    }

    /* Identity of one worktree: its canonical root, the Git directory shared by
     * every worktree of the repository, and its own Git directory, which is what
     * distinguishes two worktrees of the same repository from each other. */
    const char *root = atlas_git_root(g);
    const char *cdir = atlas_git_common_dir(g);
    const char *gdir = atlas_git_dir(g);
    atlas_repo_identity ident;
    memset(&ident, 0, sizeof(ident));
    ident.root = root;
    ident.root_len = strlen(root);
    ident.common_dir = cdir;
    ident.common_dir_len = strlen(cdir);
    ident.git_dir = gdir;
    ident.git_dir_len = strlen(gdir);
    ident.is_linked_worktree = atlas_git_is_linked_worktree(g);
    ident.object_format = atlas_git_object_format(g);

    int64_t id = 0;
    st = atlas_db_repo_add(db, effective, &ident, &id, err);
    atlas_git_close(g);
    if (st != ATLAS_OK) {
        return st;
    }
    /* A repository registered under A1 gets its index-state row immediately, so
     * `atlas daemon status` can describe it as unwatched rather than as absent. */
    atlas_err ignore;
    atlas_err_init(&ignore);
    (void)atlas_db_index_state_ensure(db, id, &ignore);
    if (out != NULL) {
        bool found = false;
        return atlas_db_repo_get(db, effective, out, &found, err);
    }
    return ATLAS_OK;
}

typedef struct repo_list_ctx {
    atlas_repo_cb cb;
    void *ud;
    int64_t count;
} repo_list_ctx;

static atlas_status repo_list_tap(const atlas_repo_info *ri, void *ud, atlas_err *err) {
    repo_list_ctx *rc = (repo_list_ctx *)ud;
    rc->count++;
    if (rc->cb == NULL) {
        return ATLAS_OK;
    }
    return rc->cb(ri, rc->ud, err);
}

atlas_status atlas_service_repo_list(atlas_ctx *ctx, atlas_repo_cb cb, void *ud, int64_t *count_out,
                                     atlas_err *err) {
    repo_list_ctx rc = {cb, ud, 0};
    atlas_status st = atlas_db_repo_list(ctx->db, repo_list_tap, &rc, err);
    if (st == ATLAS_OK && count_out != NULL) {
        *count_out = rc.count;
    }
    return st;
}

atlas_status atlas_service_repo_remove(atlas_ctx *ctx, const char *name, atlas_repo_info *removed,
                                       atlas_err *err) {
    return atlas_service_repo_remove_db(ctx->db, name, removed, err);
}

atlas_status atlas_service_repo_remove_db(atlas_db *db, const char *name, atlas_repo_info *removed,
                                          atlas_err *err) {
    /* Read the record first so the caller can report exactly what was forgotten.
     * Only Atlas metadata is deleted; the repository itself is never touched. */
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    bool found = false;
    atlas_status st = atlas_db_repo_get(db, name, &info, &found, err);
    if (st == ATLAS_OK && !found) {
        st = atlas_err_set(err, ATLAS_ERR_REPO,
                           "no repository named \"%s\" is registered (try: atlas repo list)", name);
    }
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }
    bool gone = false;
    st = atlas_db_repo_remove(db, name, &gone, err);
    if (st == ATLAS_OK && !gone) {
        st = atlas_err_set(err, ATLAS_ERR_DB, "repository \"%s\" could not be removed", name);
    }
    if (st == ATLAS_OK && removed != NULL) {
        removed->id = info.id;
        (void)snprintf(removed->name, sizeof(removed->name), "%s", info.name);
        st = atlas_buf_set(&removed->root_path, info.root_path.data, info.root_path.len, err);
        if (st == ATLAS_OK) {
            st = atlas_buf_set(&removed->root_path_text, info.root_path_text.data,
                               info.root_path_text.len, err);
        }
    }
    atlas_repo_info_free(&info);
    return st;
}

atlas_status atlas_service_scan(atlas_ctx *ctx, const char *name, const atlas_scan_opts *opts,
                                atlas_scan_summary *summary, atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_service_require_repo(ctx, name, &info, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }

    atlas_git *g = NULL;
    st = atlas_service_open_repo_git(&info, &g, err);
    if (st == ATLAS_OK) {
        st = atlas_scan_run(ctx->db, g, info.id, opts, summary, err);
    }
    atlas_git_close(g);
    atlas_repo_info_free(&info);
    return st;
}

/* --- status -------------------------------------------------------------- */

void atlas_status_report_init(atlas_status_report *r) {
    memset(r, 0, sizeof(*r));
    atlas_repo_info_init(&r->repo);
    atlas_buf_init(&r->git_error);
}

void atlas_status_report_free(atlas_status_report *r) {
    if (r == NULL) {
        return;
    }
    atlas_repo_info_free(&r->repo);
    atlas_buf_free(&r->git_error);
}

atlas_status atlas_service_status(atlas_ctx *ctx, const char *name, atlas_status_report *out,
                                  atlas_err *err) {
    atlas_status st = atlas_service_require_repo(ctx, name, &out->repo, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_repo_counts(ctx->db, out->repo.id, &out->counts, err);
    if (st != ATLAS_OK) {
        return st;
    }
    out->never_scanned = (out->repo.last_scan_id == 0);
    out->scanned = !out->never_scanned;

    /* Sibling worktrees are reported so it is obvious that other registrations
     * share this repository's object store while having their own HEAD and dirt. */
    st = atlas_db_repo_siblings(ctx->db, out->repo.id, out->repo.git_common_dir.data,
                                out->repo.git_common_dir.len, NULL, NULL,
                                &out->sibling_worktrees, err);
    if (st != ATLAS_OK) {
        return st;
    }

    /* The live observation is taken fresh: a stale index must be visible as
     * drift rather than presented as the current state. */
    atlas_git *g = NULL;
    atlas_err git_err;
    atlas_err_init(&git_err);
    atlas_status gst = atlas_git_open(atlas_buf_cstr(&out->repo.root_path), &g, &git_err);
    if (gst == ATLAS_OK) {
        gst = atlas_git_read_head(g, &out->live_head, &git_err);
    }
    if (gst == ATLAS_OK) {
        gst = atlas_git_read_worktree_state(g, &out->live_state, &git_err);
    }
    atlas_git_close(g);

    out->git_ok = (gst == ATLAS_OK);
    if (!out->git_ok) {
        st = atlas_buf_set_str(&out->git_error, atlas_err_msg(&git_err), err);
        if (st != ATLAS_OK) {
            return st;
        }
    } else if (out->scanned) {
        out->head_drift = (strcmp(out->live_head.oid, out->repo.scanned_head) != 0);
    }
    return ATLAS_OK;
}

/* --- search -------------------------------------------------------------- */

atlas_status atlas_service_search(atlas_ctx *ctx, const char *name, const char *query,
                                  int64_t limit, atlas_search_mode *mode_out, atlas_search_cb cb,
                                  void *ud, int64_t *count_out, atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_service_require_repo(ctx, name, &info, err);
    if (st == ATLAS_OK) {
        st = atlas_db_search(ctx->db, info.id, query, limit, mode_out, cb, ud, count_out, err);
    }
    atlas_repo_info_free(&info);
    return st;
}

/* --- file ---------------------------------------------------------------- */

typedef struct last_commit {
    atlas_buf oid;
    atlas_buf subject;
    int64_t time;
    bool have;
} last_commit;

static atlas_status capture_last_commit(const atlas_history_row *row, void *ud, atlas_err *err) {
    last_commit *lc = (last_commit *)ud;
    if (lc->have) {
        return ATLAS_OK;
    }
    atlas_status st = atlas_buf_set_str(&lc->oid, row->commit_oid, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&lc->subject, row->subject, err);
    }
    if (st != ATLAS_OK) {
        return st;
    }
    lc->time = row->commit_time;
    lc->have = true;
    return ATLAS_OK;
}

typedef struct file_ctx {
    atlas_ctx *ctx;
    int64_t repo_id;
    atlas_file_report_cb cb;
    void *ud;
} file_ctx;

static atlas_status on_file_row(const atlas_file_row *row, void *ud, atlas_err *err) {
    file_ctx *fc = (file_ctx *)ud;

    last_commit lc;
    memset(&lc, 0, sizeof(lc));
    atlas_buf_init(&lc.oid);
    atlas_buf_init(&lc.subject);

    int64_t change_count = 0;
    atlas_status st = atlas_db_file_history(fc->ctx->db, fc->repo_id, row->path_raw,
                                            row->path_raw_len, -1, NULL, NULL, &change_count, err);
    if (st == ATLAS_OK) {
        st = atlas_db_file_history(fc->ctx->db, fc->repo_id, row->path_raw, row->path_raw_len, 1,
                                   capture_last_commit, &lc, NULL, err);
    }
    if (st == ATLAS_OK) {
        atlas_file_report rep;
        memset(&rep, 0, sizeof(rep));
        rep.row = *row;
        /* A0 never infers why a file exists or changed. */
        rep.reason = ATLAS_REASON_UNKNOWN;
        rep.reason_evidence = "UNKNOWN";
        rep.change_count = change_count;
        rep.last_commit_oid = lc.have ? atlas_buf_cstr(&lc.oid) : NULL;
        rep.last_commit_subject = lc.have ? atlas_buf_cstr(&lc.subject) : NULL;
        rep.last_commit_time = lc.have ? lc.time : 0;
        if (fc->cb != NULL) {
            st = fc->cb(&rep, fc->ud, err);
        }
    }
    atlas_buf_free(&lc.oid);
    atlas_buf_free(&lc.subject);
    return st;
}

/* Accepts either the exact path bytes or the %XX-escaped text form Atlas prints,
 * so a path copied out of Atlas output can be pasted straight back in. */
static atlas_status resolve_path_variants(const char *path, atlas_buf *decoded, bool *have_decoded,
                                          atlas_err *err) {
    *have_decoded = false;
    if (strchr(path, '%') == NULL) {
        return ATLAS_OK;
    }
    atlas_err ignore;
    atlas_err_init(&ignore);
    atlas_buf_reset(decoded);
    if (atlas_path_text_decode(path, strlen(path), decoded, &ignore) != ATLAS_OK) {
        return ATLAS_OK; /* not a valid escaped form; the raw bytes stand alone */
    }
    (void)err;
    *have_decoded = true;
    return ATLAS_OK;
}

atlas_status atlas_service_file(atlas_ctx *ctx, const char *name, const char *path,
                                atlas_file_report_cb cb, void *ud, atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_service_require_repo(ctx, name, &info, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }
    if (info.last_scan_id == 0) {
        atlas_repo_info_free(&info);
        return atlas_err_set(err, ATLAS_ERR_REPO,
                             "repository \"%s\" has not been scanned yet (run: atlas scan %s)",
                             name, name);
    }

    file_ctx fc = {ctx, info.id, cb, ud};
    bool found = false;
    st = atlas_db_file_get(ctx->db, info.id, path, strlen(path), on_file_row, &fc, &found, err);

    if (st == ATLAS_OK && !found) {
        atlas_buf decoded = ATLAS_BUF_INIT;
        bool have_decoded = false;
        st = resolve_path_variants(path, &decoded, &have_decoded, err);
        if (st == ATLAS_OK && have_decoded) {
            st = atlas_db_file_get(ctx->db, info.id, decoded.data, decoded.len, on_file_row, &fc,
                                   &found, err);
        }
        atlas_buf_free(&decoded);
    }
    if (st == ATLAS_OK && !found) {
        st = atlas_err_set(err, ATLAS_ERR_REPO, "path \"%s\" is not indexed in \"%s\"", path, name);
    }
    atlas_repo_info_free(&info);
    return st;
}

atlas_status atlas_service_history(atlas_ctx *ctx, const char *name, const char *path,
                                   int64_t limit, atlas_history_cb cb, void *ud,
                                   int64_t *count_out, atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_service_require_repo(ctx, name, &info, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }
    if (info.last_scan_id == 0) {
        atlas_repo_info_free(&info);
        return atlas_err_set(err, ATLAS_ERR_REPO,
                             "repository \"%s\" has not been scanned yet (run: atlas scan %s)",
                             name, name);
    }

    int64_t count = 0;
    st = atlas_db_file_history(ctx->db, info.id, path, strlen(path), limit, cb, ud, &count, err);
    if (st == ATLAS_OK && count == 0) {
        atlas_buf decoded = ATLAS_BUF_INIT;
        bool have_decoded = false;
        st = resolve_path_variants(path, &decoded, &have_decoded, err);
        if (st == ATLAS_OK && have_decoded) {
            st = atlas_db_file_history(ctx->db, info.id, decoded.data, decoded.len, limit, cb, ud,
                                       &count, err);
        }
        atlas_buf_free(&decoded);
    }
    if (st == ATLAS_OK && count_out != NULL) {
        *count_out = count;
    }
    atlas_repo_info_free(&info);
    return st;
}

