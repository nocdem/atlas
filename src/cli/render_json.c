/* Atlas - JSON renderer.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Output is streamed through atlas_json, so a result set of any size is written
 * incrementally rather than assembled in memory. Field names and their order are
 * a stable contract; see docs/provenance.md.
 *
 * Untrusted-text policy: every field carrying repository-originated text is
 * emitted in the safe encoding named by "text_encoding" in the envelope. That
 * makes the document terminal-safe when printed by a consumer and losslessly
 * reversible by percent-decoding. Values that were already stored encoded
 * (path_text and friends) are emitted as-is so '%' is not escaped twice.
 */
#include "cli/render.h"

#include <string.h>

#include "atlas/atlas.h"
#include "atlas/pathrep.h"

#define TRY(expr)                      \
    do {                               \
        atlas_status st_ = (expr);     \
        if (st_ != ATLAS_OK) {         \
            return st_;                \
        }                              \
    } while (0)

/* Emits a raw, untrusted string in the safe encoding. */
static atlas_status json_safe(atlas_json *j, atlas_safe_pool *p, const char *key, const char *s,
                              atlas_err *err) {
    if (s == NULL) {
        return atlas_json_key_null(j, key, err);
    }
    return atlas_json_key_str(j, key, atlas_safe(p, s), err);
}

static atlas_status json_safe_bytes(atlas_json *j, atlas_safe_pool *p, const char *key,
                                    const void *d, size_t n, atlas_err *err) {
    return atlas_json_key_str(j, key, atlas_safe_n(p, d, n), err);
}

/* Emits a path as its safe text form plus, when the raw bytes are not valid
 * UTF-8, the exact bytes in hex so nothing is lost. */
static atlas_status json_path(atlas_json *j, const char *key, const void *raw, size_t raw_len,
                              const char *text, bool is_utf8, atlas_err *err) {
    atlas_buf encoded = ATLAS_BUF_INIT;
    const char *value = text;
    if (value == NULL) {
        atlas_status st = atlas_path_text_encode(raw, raw_len, &encoded, err);
        if (st != ATLAS_OK) {
            atlas_buf_free(&encoded);
            return st;
        }
        value = atlas_buf_cstr(&encoded);
        is_utf8 = atlas_utf8_valid(raw, raw_len);
    }
    atlas_status st = atlas_json_key_str(j, key, value, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "path_encoding", is_utf8 ? "utf8" : "percent-escaped", err);
    }
    if (st == ATLAS_OK && !is_utf8 && raw != NULL) {
        st = atlas_json_key_hex(j, "path_bytes_hex", raw, raw_len, err);
    }
    atlas_buf_free(&encoded);
    return st;
}

/* --- lifecycle ----------------------------------------------------------- */

static atlas_status j_begin(atlas_renderer *r, const char *command, atlas_err *err) {
    TRY(atlas_json_obj_begin(r->j, err));
    TRY(atlas_json_key_str(r->j, "atlas", ATLAS_VERSION_STRING, err));
    TRY(atlas_json_key_str(r->j, "phase", ATLAS_PHASE, err));
    TRY(atlas_json_key_str(r->j, "command", command, err));
    TRY(atlas_json_key_bool(r->j, "ok", true, err));
    /* Names the encoding used for every repository-originated text field. */
    return atlas_json_key_str(r->j, "text_encoding", ATLAS_TEXT_ENCODING_NAME, err);
}

static atlas_status j_end(atlas_renderer *r, atlas_err *err) {
    TRY(atlas_json_obj_end(r->j, err));
    atlas_status st = atlas_json_finish(r->j, err);
    r->j = NULL; /* finish frees the writer */
    return st;
}

static atlas_status j_note_repo(atlas_renderer *r, const char *repo, atlas_err *err) {
    /* The repository name is validated to a safe charset at registration. */
    return atlas_json_key_str(r->j, "repo", repo, err);
}

static atlas_status j_note_query(atlas_renderer *r, const char *query, atlas_search_mode mode,
                                 atlas_err *err) {
    TRY(json_safe(r->j, &r->safe, "query", query, err));
    TRY(atlas_json_key_str(r->j, "search_mode", atlas_search_mode_name(mode), err));
    return atlas_json_key_bool(r->j, "degraded", mode == ATLAS_SEARCH_DEGRADED_LIKE, err);
}

static atlas_status j_list_begin(atlas_renderer *r, const char *key, atlas_err *err) {
    r->items = 0;
    r->in_list = true;
    TRY(atlas_json_key(r->j, key, err));
    return atlas_json_arr_begin(r->j, err);
}

static atlas_status j_list_end(atlas_renderer *r, const char *singular, const char *plural,
                               int64_t count, atlas_err *err) {
    (void)singular;
    (void)plural;
    r->in_list = false;
    TRY(atlas_json_arr_end(r->j, err));
    return atlas_json_key_int(r->j, "count", count, err);
}

/* --- doctor -------------------------------------------------------------- */

static atlas_status j_doctor(atlas_renderer *r, const atlas_doctor_report *rep, atlas_err *err) {
    atlas_json *j = r->j;
    atlas_safe_pool *p = &r->safe;
    TRY(atlas_json_key(j, "doctor", err));
    TRY(atlas_json_obj_begin(j, err));
    TRY(atlas_json_key_str(j, "atlas_version", rep->atlas_version, err));
    TRY(atlas_json_key_str(j, "build_compiler", rep->build_compiler, err));
    TRY(atlas_json_key_bool(j, "git_found", rep->git_found, err));
    TRY(json_safe(j, p, "git_executable", atlas_buf_cstr(&rep->git_exe), err));
    TRY(json_safe(j, p, "git_version", atlas_buf_cstr(&rep->git_version), err));
    TRY(atlas_json_key_str(j, "sqlite_runtime", rep->sqlite_runtime, err));
    TRY(atlas_json_key_str(j, "sqlite_compiled", rep->sqlite_compiled, err));
    TRY(json_safe(j, p, "data_dir", atlas_buf_cstr(&rep->data_dir), err));
    TRY(atlas_json_key_str(j, "data_dir_source", atlas_datadir_source_name(rep->data_dir_source),
                           err));
    TRY(json_safe(j, p, "db_path", atlas_buf_cstr(&rep->db_path), err));
    /* Both stated as fields, so a caller can check that `doctor` found an
     * absent index rather than creating one to find. */
    TRY(atlas_json_key_bool(j, "data_dir_present", rep->data_dir_present, err));
    TRY(atlas_json_key_bool(j, "index_present", rep->index_present, err));
    TRY(atlas_json_key_int(j, "schema_version", rep->schema_version, err));
    TRY(atlas_json_key_int(j, "expected_schema_version", rep->expected_schema_version, err));
    TRY(atlas_json_key_bool(j, "schema_current", rep->db_ok, err));
    TRY(atlas_json_key_str(j, "journal_mode", rep->journal_mode, err));
    TRY(atlas_json_key_bool(j, "wal", rep->wal, err));
    TRY(atlas_json_key_bool(j, "foreign_keys", rep->foreign_keys, err));
    TRY(atlas_json_key_bool(j, "fts5", rep->fts5, err));
    TRY(atlas_json_key_str(j, "search_mode", atlas_search_mode_name(rep->search_mode), err));
    TRY(json_safe(j, p, "integrity_check", atlas_buf_cstr(&rep->integrity), err));
    TRY(json_safe(j, p, "foreign_key_check", atlas_buf_cstr(&rep->foreign_key_check), err));
    TRY(atlas_json_key_int(j, "repositories", rep->repo_count, err));
    TRY(atlas_json_key_bool(j, "healthy", rep->ok, err));

    TRY(atlas_json_key(j, "problems", err));
    TRY(atlas_json_arr_begin(j, err));
    const char *text = atlas_buf_cstr(&rep->problems);
    while (*text != '\0') {
        const char *nl = strchr(text, '\n');
        size_t len = (nl != NULL) ? (size_t)(nl - text) : strlen(text);
        /* A problem line may quote git's own output. */
        TRY(atlas_json_str(j, atlas_safe_n(p, text, len), err));
        if (nl == NULL) {
            break;
        }
        text = nl + 1;
    }
    TRY(atlas_json_arr_end(j, err));
    return atlas_json_obj_end(j, err);
}

static atlas_status j_version(atlas_renderer *r, atlas_err *err) {
    TRY(atlas_json_key_str(r->j, "version", ATLAS_VERSION_STRING, err));
    TRY(atlas_json_key_int(r->j, "version_major", ATLAS_VERSION_MAJOR, err));
    TRY(atlas_json_key_int(r->j, "version_minor", ATLAS_VERSION_MINOR, err));
    TRY(atlas_json_key_int(r->j, "version_patch", ATLAS_VERSION_PATCH, err));
    return atlas_json_key_int(r->j, "schema_version", ATLAS_SCHEMA_VERSION, err);
}

/* --- repositories -------------------------------------------------------- */

static atlas_status repo_body(atlas_renderer *r, const atlas_repo_info *ri, atlas_err *err) {
    atlas_json *j = r->j;
    atlas_safe_pool *p = &r->safe;
    TRY(atlas_json_key_int(j, "id", ri->id, err));
    TRY(atlas_json_key_str(j, "name", ri->name, err));
    TRY(json_path(j, "root", ri->root_path.data, ri->root_path.len,
                  atlas_buf_cstr(&ri->root_path_text),
                  atlas_utf8_valid(ri->root_path.data, ri->root_path.len), err));
    TRY(json_safe_bytes(j, p, "git_common_dir", ri->git_common_dir.data, ri->git_common_dir.len,
                        err));
    TRY(json_safe_bytes(j, p, "git_dir", ri->git_dir.data, ri->git_dir.len, err));
    TRY(atlas_json_key_bool(j, "is_linked_worktree", ri->is_linked_worktree, err));
    TRY(atlas_json_key_str(j, "object_format", ri->object_format, err));
    TRY(atlas_json_key_str(j, "registered_at", ri->registered_at, err));
    TRY(atlas_json_key_str_opt(j, "last_scan_at",
                               ri->last_scan_at[0] != '\0' ? ri->last_scan_at : NULL, err));
    TRY(atlas_json_key_int(j, "last_scan_id", ri->last_scan_id, err));
    TRY(atlas_json_key_str_opt(j, "scanned_head",
                               ri->scanned_head[0] != '\0' ? ri->scanned_head : NULL, err));
    TRY(json_safe(j, p, "branch", ri->current_branch[0] != '\0' ? ri->current_branch : NULL, err));
    TRY(atlas_json_key_str(j, "head_state", ri->head_state, err));
    TRY(atlas_json_key_bool(j, "dirty", ri->dirty, err));
    TRY(atlas_json_key_int(j, "dirty_staged", ri->dirty_staged, err));
    TRY(atlas_json_key_int(j, "dirty_unstaged", ri->dirty_unstaged, err));
    TRY(atlas_json_key_int(j, "dirty_untracked", ri->dirty_untracked, err));
    return atlas_json_key_int(j, "dirty_unmerged", ri->dirty_unmerged, err);
}

static atlas_status j_repo_item(atlas_renderer *r, const atlas_repo_info *ri, atlas_err *err) {
    r->items++;
    TRY(atlas_json_obj_begin(r->j, err));
    TRY(repo_body(r, ri, err));
    return atlas_json_obj_end(r->j, err);
}

static atlas_status j_repo_added(atlas_renderer *r, const atlas_repo_info *ri, atlas_err *err) {
    TRY(atlas_json_key(r->j, "repository", err));
    TRY(atlas_json_obj_begin(r->j, err));
    TRY(repo_body(r, ri, err));
    return atlas_json_obj_end(r->j, err);
}

static atlas_status j_repo_removed(atlas_renderer *r, const atlas_repo_info *ri, atlas_err *err) {
    TRY(atlas_json_key(r->j, "removed", err));
    TRY(atlas_json_obj_begin(r->j, err));
    TRY(atlas_json_key_int(r->j, "id", ri->id, err));
    TRY(atlas_json_key_str(r->j, "name", ri->name, err));
    TRY(json_path(r->j, "root", ri->root_path.data, ri->root_path.len,
                  atlas_buf_cstr(&ri->root_path_text),
                  atlas_utf8_valid(ri->root_path.data, ri->root_path.len), err));
    TRY(atlas_json_key_bool(r->j, "target_repository_modified", false, err));
    return atlas_json_obj_end(r->j, err);
}

/* --- scan ---------------------------------------------------------------- */

static atlas_status j_scan(atlas_renderer *r, const char *repo, const atlas_scan_summary *s,
                           atlas_err *err) {
    atlas_json *j = r->j;
    (void)repo;
    TRY(atlas_json_key(j, "scan", err));
    TRY(atlas_json_obj_begin(j, err));
    TRY(atlas_json_key_int(j, "scan_id", s->scan_id, err));
    TRY(atlas_json_key_str_opt(j, "head", s->head_oid[0] != '\0' ? s->head_oid : NULL, err));
    TRY(atlas_json_key_str(j, "head_state", s->head_state, err));
    TRY(json_safe(j, &r->safe, "branch", s->branch[0] != '\0' ? s->branch : NULL, err));
    TRY(atlas_json_key_bool(j, "dirty", s->dirty, err));
    TRY(atlas_json_key_int(j, "files_total", s->files_total, err));
    TRY(atlas_json_key_int(j, "files_added", s->files_added, err));
    TRY(atlas_json_key_int(j, "files_modified", s->files_modified, err));
    TRY(atlas_json_key_int(j, "files_deleted", s->files_deleted, err));
    TRY(atlas_json_key_int(j, "files_unchanged", s->files_unchanged, err));
    TRY(atlas_json_key_int(j, "files_unreadable", s->files_unreadable, err));
    TRY(atlas_json_key_int(j, "files_refused_unsafe_path", s->files_unsafe, err));
    TRY(atlas_json_key_bool(j, "history_skipped", s->history_skipped, err));
    TRY(atlas_json_key_int(j, "commits_seen", s->commits_seen, err));
    TRY(atlas_json_key_int(j, "commits_ingested", s->commits_ingested, err));
    TRY(atlas_json_key_int(j, "changes_ingested", s->changes_ingested, err));
    TRY(atlas_json_key_int(j, "evidence_created", s->evidence_created, err));
    TRY(atlas_json_key_bool(j, "compile_database_found", s->compile_db_found, err));
    TRY(atlas_json_key_bool(j, "compile_database_is_symlink", s->compile_db_is_symlink, err));
    TRY(atlas_json_key_bool(j, "compile_database_parsed", false, err));
    return atlas_json_obj_end(j, err);
}

/* --- status -------------------------------------------------------------- */

static atlas_status j_status(atlas_renderer *r, const atlas_status_report *s, atlas_err *err) {
    atlas_json *j = r->j;
    atlas_safe_pool *p = &r->safe;
    TRY(atlas_json_key(j, "repository", err));
    TRY(atlas_json_obj_begin(j, err));
    TRY(repo_body(r, &s->repo, err));
    TRY(atlas_json_obj_end(j, err));

    TRY(atlas_json_key_int(j, "sibling_worktrees", s->sibling_worktrees, err));

    TRY(atlas_json_key(j, "indexed", err));
    TRY(atlas_json_obj_begin(j, err));
    TRY(atlas_json_key_int(j, "files", s->counts.files_live, err));
    TRY(atlas_json_key_int(j, "files_deleted", s->counts.files_deleted, err));
    TRY(atlas_json_key_int(j, "commits", s->counts.commits, err));
    TRY(atlas_json_key_int(j, "changes", s->counts.changes, err));
    TRY(atlas_json_key_int(j, "scans", s->counts.scans, err));
    TRY(atlas_json_key_int(j, "evidence", s->counts.evidence, err));
    TRY(atlas_json_key_int(j, "compile_databases", s->counts.compile_databases, err));
    TRY(atlas_json_obj_end(j, err));

    TRY(atlas_json_key_bool(j, "scanned", s->scanned, err));
    TRY(atlas_json_key(j, "live", err));
    TRY(atlas_json_obj_begin(j, err));
    TRY(atlas_json_key_bool(j, "available", s->git_ok, err));
    if (s->git_ok) {
        TRY(atlas_json_key_str_opt(j, "head",
                                   s->live_head.oid[0] != '\0' ? s->live_head.oid : NULL, err));
        TRY(atlas_json_key_str(j, "head_state", s->live_head.state, err));
        TRY(json_safe(j, p, "branch", s->live_head.branch[0] != '\0' ? s->live_head.branch : NULL,
                      err));
        TRY(atlas_json_key_bool(j, "dirty", s->live_state.dirty, err));
        TRY(atlas_json_key_int(j, "staged", s->live_state.staged, err));
        TRY(atlas_json_key_int(j, "unstaged", s->live_state.unstaged, err));
        TRY(atlas_json_key_int(j, "untracked", s->live_state.untracked, err));
        TRY(atlas_json_key_int(j, "unmerged", s->live_state.unmerged, err));
    } else {
        TRY(json_safe(j, p, "error", atlas_buf_cstr(&s->git_error), err));
    }
    TRY(atlas_json_obj_end(j, err));
    return atlas_json_key_bool(j, "head_drift", s->head_drift, err);
}

/* --- search / file / history --------------------------------------------- */

static atlas_status j_search_item(atlas_renderer *r, const atlas_search_hit *h, atlas_err *err) {
    atlas_json *j = r->j;
    r->items++;
    TRY(atlas_json_obj_begin(j, err));
    TRY(atlas_json_key_str(j, "kind", h->kind, err));
    if (strcmp(h->kind, "file") == 0) {
        TRY(json_path(j, "path", h->path_raw, h->path_raw_len, h->path_text, h->path_is_utf8, err));
        TRY(atlas_json_key_str_opt(j, "git_index_oid", h->git_index_oid, err));
        TRY(atlas_json_key_bool(j, "deleted", h->deleted, err));
    } else {
        TRY(atlas_json_key_str(j, "commit", h->commit_oid, err));
        TRY(json_safe(j, &r->safe, "subject", h->subject, err));
        TRY(json_safe(j, &r->safe, "author_name", h->author_name, err));
        TRY(atlas_json_key_int(j, "author_time", h->author_time, err));
    }
    TRY(atlas_json_key_str(j, "evidence", h->evidence, err));
    return atlas_json_obj_end(j, err);
}

static atlas_status j_file(atlas_renderer *r, const atlas_file_report *f, atlas_err *err) {
    atlas_json *j = r->j;
    const atlas_file_row *row = &f->row;
    TRY(atlas_json_key(j, "file", err));
    TRY(atlas_json_obj_begin(j, err));
    TRY(json_path(j, "path", row->path_raw, row->path_raw_len, row->path_text, row->path_is_utf8,
                  err));
    TRY(atlas_json_key_str(j, "file_type", row->file_type, err));
    TRY(atlas_json_key_str_opt(j, "language", row->language, err));
    TRY(atlas_json_key_str_opt(j, "git_mode", row->git_mode, err));
    TRY(atlas_json_key_str_opt(j, "git_index_oid", row->git_index_oid, err));
    TRY(atlas_json_key_str_opt(j, "content_hash", row->content_hash, err));
    TRY(atlas_json_key_str_opt(j, "content_hash_algo", row->content_hash_algo, err));
    if (row->size_known) {
        TRY(atlas_json_key_int(j, "size_bytes", row->size_bytes, err));
    } else {
        TRY(atlas_json_key_null(j, "size_bytes", err));
    }
    TRY(atlas_json_key_bool(j, "is_executable", row->is_executable, err));
    TRY(atlas_json_key_bool(j, "is_symlink", row->is_symlink, err));
    TRY(atlas_json_key_bool(j, "unsafe_path", row->unsafe_path, err));
    TRY(atlas_json_key_bool(j, "deleted", row->deleted, err));
    TRY(atlas_json_key_str_opt(j, "note", row->read_error, err));
    TRY(atlas_json_key_str(j, "first_seen_at", row->first_seen_at, err));
    TRY(atlas_json_key_int(j, "first_seen_scan_id", row->first_seen_scan_id, err));
    TRY(atlas_json_key_str(j, "last_seen_at", row->last_seen_at, err));
    TRY(atlas_json_key_int(j, "last_seen_scan_id", row->last_seen_scan_id, err));
    TRY(atlas_json_key_str_opt(j, "deleted_at", row->deleted_at, err));
    TRY(atlas_json_key_int(j, "recorded_changes", f->change_count, err));
    TRY(atlas_json_key_str_opt(j, "last_commit", f->last_commit_oid, err));
    TRY(json_safe(j, &r->safe, "last_commit_subject", f->last_commit_subject, err));
    TRY(atlas_json_key_int(j, "last_commit_time", f->last_commit_time, err));

    /* Provenance travels with the answer. */
    TRY(atlas_json_key(j, "evidence", err));
    TRY(atlas_json_arr_begin(j, err));
    TRY(atlas_json_str(j, "SOURCE", err));
    TRY(atlas_json_str(j, "GIT", err));
    TRY(atlas_json_arr_end(j, err));
    TRY(atlas_json_key_str(j, "reason", f->reason, err));
    TRY(atlas_json_key_str(j, "reason_evidence", f->reason_evidence, err));
    return atlas_json_obj_end(j, err);
}

static atlas_status j_history_item(atlas_renderer *r, const atlas_history_row *h, atlas_err *err) {
    atlas_json *j = r->j;
    atlas_safe_pool *p = &r->safe;
    r->items++;
    TRY(atlas_json_obj_begin(j, err));
    TRY(atlas_json_key_str(j, "commit", h->commit_oid, err));
    TRY(atlas_json_key_str(j, "change_type", h->change_type, err));
    /* Already safe-encoded when stored. */
    TRY(atlas_json_key_str(j, "path", h->path_text, err));
    TRY(atlas_json_key_str_opt(j, "old_path", h->old_path_text, err));
    if (h->score_known) {
        TRY(atlas_json_key_int(j, "similarity", h->score, err));
    } else {
        TRY(atlas_json_key_null(j, "similarity", err));
    }
    TRY(json_safe(j, p, "author_name", h->author_name, err));
    TRY(json_safe(j, p, "author_email", h->author_email, err));
    TRY(atlas_json_key_int(j, "author_time", h->author_time, err));
    TRY(atlas_json_key_int(j, "commit_time", h->commit_time, err));
    TRY(json_safe(j, p, "subject", h->subject, err));
    TRY(atlas_json_key_str(j, "evidence", "GIT", err));
    TRY(atlas_json_key_str(j, "reason", ATLAS_REASON_UNKNOWN, err));
    return atlas_json_obj_end(j, err);
}

/* --- diff ---------------------------------------------------------------- */

/* The diff document reports each scope as its own array, so a consumer never has
 * to infer whether a change is staged from a status letter. */
static atlas_status j_diff_begin(atlas_renderer *r, const atlas_diff_report *rep, atlas_err *err) {
    atlas_json *j = r->j;
    TRY(atlas_json_key_str_opt(j, "base_head", rep->base_head[0] != '\0' ? rep->base_head : NULL,
                               err));
    TRY(atlas_json_key_str(j, "head_state", rep->head_state, err));
    TRY(json_safe(j, &r->safe, "branch", rep->branch[0] != '\0' ? rep->branch : NULL, err));
    TRY(atlas_json_key_bool(j, "dirty", rep->dirty, err));
    r->scope_open = false;
    r->open_scope = -1;
    r->items = 0;
    /* Sections are opened lazily as entries arrive, and every section is present
     * even when empty, so a consumer can index them unconditionally. */
    TRY(atlas_json_key(j, "staged", err));
    return atlas_json_arr_begin(j, err);
}

static atlas_status j_diff_entry_body(atlas_renderer *r, const atlas_diff_entry *e,
                                      atlas_err *err) {
    atlas_json *j = r->j;
    TRY(atlas_json_obj_begin(j, err));
    TRY(atlas_json_key_str(j, "change_type", e->change_type, err));
    char status[2] = {e->status, '\0'};
    TRY(atlas_json_key_str(j, "status", status, err));
    /* Already safe-encoded by the diff service. */
    TRY(atlas_json_key_str(j, "path", e->path_text, err));
    TRY(atlas_json_key_str(j, "path_encoding", e->path_is_utf8 ? "utf8" : "percent-escaped", err));
    if (!e->path_is_utf8 && e->path_raw != NULL) {
        TRY(atlas_json_key_hex(j, "path_bytes_hex", e->path_raw, e->path_raw_len, err));
    }
    TRY(atlas_json_key_str_opt(j, "old_path", e->old_path_text, err));
    if (e->score_known) {
        TRY(atlas_json_key_int(j, "similarity", e->score, err));
    } else {
        TRY(atlas_json_key_null(j, "similarity", err));
    }
    TRY(atlas_json_key_bool(j, "binary", e->binary, err));
    if (e->counts_known) {
        TRY(atlas_json_key_int(j, "added", e->added, err));
        TRY(atlas_json_key_int(j, "deleted", e->deleted, err));
    } else {
        TRY(atlas_json_key_null(j, "added", err));
        TRY(atlas_json_key_null(j, "deleted", err));
    }
    if (e->scope == ATLAS_SCOPE_UNTRACKED) {
        TRY(atlas_json_key_bool(j, "is_directory", e->is_directory, err));
        if (e->size_known) {
            TRY(atlas_json_key_int(j, "size_bytes", e->size_bytes, err));
        } else {
            TRY(atlas_json_key_null(j, "size_bytes", err));
        }
        TRY(atlas_json_key_str_opt(j, "content_hash", e->content_hash, err));
        TRY(atlas_json_key_str_opt(j, "content_hash_algo", e->content_hash_algo, err));
    } else {
        TRY(atlas_json_key_str_opt(j, "head_oid", e->head_oid[0] != '\0' ? e->head_oid : NULL, err));
        TRY(atlas_json_key_str_opt(j, "index_oid", e->index_oid[0] != '\0' ? e->index_oid : NULL,
                                   err));
        TRY(atlas_json_key_str_opt(j, "mode_head", e->mode_head[0] != '\0' ? e->mode_head : NULL,
                                   err));
        TRY(atlas_json_key_str_opt(j, "mode_index", e->mode_index[0] != '\0' ? e->mode_index : NULL,
                                   err));
        TRY(atlas_json_key_str_opt(
            j, "mode_worktree", e->mode_worktree[0] != '\0' ? e->mode_worktree : NULL, err));
    }
    TRY(atlas_json_key_str_opt(j, "note", e->note, err));
    /* Untracked identity is read from the working tree, so it is SOURCE evidence;
     * everything else comes from git's own comparison. */
    TRY(atlas_json_key_str(j, "evidence",
                           e->scope == ATLAS_SCOPE_UNTRACKED ? "SOURCE" : "GIT", err));
    return atlas_json_obj_end(j, err);
}

/* Closes the current array and opens the arrays up to `scope`, so the document
 * always contains staged, unstaged, unmerged and untracked in that order. */
static atlas_status j_diff_open_scope(atlas_renderer *r, int scope, atlas_err *err) {
    static const char *const KEYS[] = {"staged", "unstaged", "unmerged", "untracked"};
    /* Service order is staged, unstaged, unmerged, untracked. */
    static const int ORDER[] = {ATLAS_SCOPE_STAGED, ATLAS_SCOPE_UNSTAGED, ATLAS_SCOPE_UNMERGED,
                                ATLAS_SCOPE_UNTRACKED};
    int current = r->scope_open ? r->open_scope : ATLAS_SCOPE_STAGED;
    size_t cur_idx = 0;
    size_t want_idx = 0;
    for (size_t i = 0; i < 4u; i++) {
        if (ORDER[i] == current) {
            cur_idx = i;
        }
        if (ORDER[i] == scope) {
            want_idx = i;
        }
    }
    for (size_t i = cur_idx; i < want_idx; i++) {
        TRY(atlas_json_arr_end(r->j, err));
        TRY(atlas_json_key(r->j, KEYS[i + 1u], err));
        TRY(atlas_json_arr_begin(r->j, err));
    }
    r->scope_open = true;
    r->open_scope = scope;
    return ATLAS_OK;
}

static atlas_status j_diff_item(atlas_renderer *r, const atlas_diff_entry *e, atlas_err *err) {
    TRY(j_diff_open_scope(r, (int)e->scope, err));
    r->items++;
    return j_diff_entry_body(r, e, err);
}

static atlas_status j_diff_end(atlas_renderer *r, const atlas_diff_report *rep, atlas_err *err) {
    atlas_json *j = r->j;
    /* Close whichever section is open, and emit the remaining empty ones. */
    TRY(j_diff_open_scope(r, ATLAS_SCOPE_UNTRACKED, err));
    TRY(atlas_json_arr_end(j, err));
    r->scope_open = false;

    TRY(atlas_json_key(j, "counts", err));
    TRY(atlas_json_obj_begin(j, err));
    TRY(atlas_json_key_int(j, "staged", rep->staged_count, err));
    TRY(atlas_json_key_int(j, "unstaged", rep->unstaged_count, err));
    TRY(atlas_json_key_int(j, "untracked", rep->untracked_count, err));
    TRY(atlas_json_key_int(j, "unmerged", rep->unmerged_count, err));
    TRY(atlas_json_key_int(j, "reported_entries", rep->total_entries, err));
    TRY(atlas_json_obj_end(j, err));

    TRY(atlas_json_key_int(j, "binary_changes", rep->binary_changes, err));
    TRY(atlas_json_key_bool(j, "truncated", rep->truncated, err));
    TRY(atlas_json_key_str_opt(
        j, "truncated_reason",
        rep->truncated ? atlas_buf_cstr(&rep->truncated_reason) : NULL, err));

    TRY(atlas_json_key(j, "evidence", err));
    TRY(atlas_json_arr_begin(j, err));
    TRY(atlas_json_str(j, "GIT", err));
    TRY(atlas_json_str(j, "SOURCE", err));
    TRY(atlas_json_arr_end(j, err));
    return atlas_json_key_str(j, "reason", ATLAS_REASON_UNKNOWN, err);
}

/* --- A1: daemon, sync, events, service ---------------------------------- */

static atlas_status j_daemon_status(atlas_renderer *r, const atlas_daemon_status_report *rep,
                                    atlas_err *err) {
    atlas_json *j = r->j;
    atlas_safe_pool *p = &r->safe;
    /* Two independent facts, reported as two fields. A consumer that only wants
     * "is it working" reads both; collapsing them here would take that choice
     * away and hide a wedged daemon. */
    TRY(atlas_json_key_bool(j, "running", rep->running, err));
    TRY(atlas_json_key_bool(j, "reachable", rep->reachable, err));
    TRY(json_safe(j, p, "socket", atlas_buf_cstr(&rep->socket_path), err));
    TRY(json_safe(j, p, "writer_lock_holder", atlas_buf_cstr(&rep->lock_holder), err));
    TRY(atlas_json_key_int(j, "protocol_version", (int64_t)rep->protocol_version, err));
    TRY(atlas_json_key_str(j, "atlas_version", atlas_buf_cstr(&rep->atlas_version), err));
    TRY(atlas_json_key_bool(j, "record_present", rep->record.present, err));
    if (rep->record.present) {
        TRY(atlas_json_key_int(j, "recorded_pid", rep->record.pid, err));
        TRY(atlas_json_key_str(j, "started_at", rep->record.started_at, err));
        TRY(atlas_json_key_str(j, "last_heartbeat_at", rep->record.last_heartbeat_at, err));
        TRY(atlas_json_key_str(j, "stopped_at", rep->record.stopped_at, err));
    }
    TRY(atlas_json_key_int(j, "repositories", rep->repo_count, err));
    TRY(atlas_json_key_int(j, "watching", rep->watched_repos, err));
    TRY(atlas_json_key_int(j, "degraded", rep->degraded_repos, err));
    return atlas_json_key_int(j, "repositories_with_event_gap", rep->repos_with_gap, err);
}

static atlas_status j_daemon_ping(atlas_renderer *r, bool reachable, const char *socket_path,
                                  const char *detail, atlas_err *err) {
    TRY(atlas_json_key_bool(r->j, "reachable", reachable, err));
    TRY(json_safe(r->j, &r->safe, "socket", socket_path, err));
    return json_safe(r->j, &r->safe, "detail", detail, err);
}

static atlas_status j_repo_state(atlas_renderer *r, const atlas_repo_state_report *rep,
                                 atlas_err *err) {
    atlas_json *j = r->j;
    atlas_safe_pool *p = &r->safe;
    /* name is a validated charset and root_path_text is already encoded; both
     * are emitted as-is. watch_detail and last_error come from git and the
     * kernel, so both are encoded. */
    TRY(atlas_json_key_str(j, "repository", rep->repo.name, err));
    TRY(atlas_json_key_str(j, "root", atlas_buf_cstr(&rep->repo.root_path_text), err));
    TRY(atlas_json_key_str(j, "watch_state", atlas_watch_state_name(rep->state.watch_state), err));
    TRY(atlas_json_key_int(j, "watched_directories", rep->state.watched_dirs, err));
    TRY(atlas_json_key_int(j, "generation", rep->state.generation, err));
    TRY(atlas_json_key_int(j, "last_complete_generation", rep->state.last_complete_generation,
                           err));
    TRY(atlas_json_key_int(j, "last_sync_seq", rep->state.last_sync_seq, err));
    TRY(atlas_json_key_str(j, "last_reconcile_at", rep->state.last_reconcile_at, err));
    TRY(atlas_json_key_str(j, "last_complete_at", rep->state.last_complete_at, err));
    TRY(atlas_json_key_bool(j, "event_gap", rep->state.event_gap, err));
    TRY(atlas_json_key_bool(j, "pending_full_reconcile", rep->state.pending_full_reconcile, err));
    TRY(atlas_json_key_int(j, "event_cursor", rep->event_cursor, err));
    TRY(atlas_json_key_bool(j, "index_current", rep->index_current, err));
    TRY(atlas_json_key_str_opt(j, "not_current_reason", rep->not_current_reason, err));
    TRY(json_safe(j, p, "watch_detail", atlas_buf_cstr(&rep->state.watch_detail), err));
    return json_safe(j, p, "last_error", atlas_buf_cstr(&rep->state.last_error), err);
}

static atlas_status j_sync(atlas_renderer *r, const char *repo, const atlas_sync_report *rep,
                           atlas_err *err) {
    atlas_json *j = r->j;
    TRY(atlas_json_key_str(j, "repository", repo, err));
    TRY(atlas_json_key_bool(j, "via_daemon", rep->via_daemon, err));
    TRY(atlas_json_key_bool(j, "waited", rep->waited, err));
    TRY(atlas_json_key_bool(j, "completed", rep->completed, err));
    TRY(atlas_json_key_int(j, "sync_seq", rep->sync_seq, err));
    TRY(atlas_json_key_int(j, "generation", rep->generation, err));
    if (rep->via_daemon) {
        return ATLAS_OK;
    }
    const atlas_reconcile_summary *s = &rep->summary;
    TRY(atlas_json_key_bool(j, "published", s->published, err));
    TRY(atlas_json_key_int(j, "files_examined", s->files_examined, err));
    TRY(atlas_json_key_int(j, "files_content_read", s->files_hashed, err));
    TRY(atlas_json_key_int(j, "files_unchanged_by_identity", s->files_identity_hit, err));
    /* Whether this pass read every eligible file. Only a pass with this set may
     * clear an event gap, so a caller can check the claim rather than infer it
     * from the flags it passed in. */
    TRY(atlas_json_key_bool(j, "content_verified", s->content_verified, err));
    TRY(atlas_json_key_int(j, "files_dirty_forced", s->files_dirty_forced, err));
    TRY(atlas_json_key_int(j, "files_racy", s->files_racy, err));
    TRY(atlas_json_key_int(j, "files_added", s->files_added, err));
    TRY(atlas_json_key_int(j, "files_modified", s->files_modified, err));
    TRY(atlas_json_key_int(j, "files_deleted", s->files_deleted, err));
    TRY(atlas_json_key_int(j, "files_unchanged", s->files_unchanged, err));
    TRY(atlas_json_key_int(j, "files_unreadable", s->files_unreadable, err));
    TRY(atlas_json_key_int(j, "files_unsafe", s->files_unsafe, err));
    TRY(atlas_json_key_int(j, "files_truncated", s->files_truncated, err));
    TRY(atlas_json_key_int(j, "untracked_discovered", s->untracked_discovered, err));
    TRY(atlas_json_key_int(j, "ignored_paths", s->ignored_paths, err));
    TRY(atlas_json_key_int(j, "commits_ingested", s->commits_ingested, err));
    TRY(atlas_json_key_int(j, "commits_seen", s->commits_seen, err));
    TRY(atlas_json_key_int(j, "changes_ingested", s->changes_ingested, err));
    TRY(atlas_json_key_bool(j, "history_full_replay", s->history_full_replay, err));
    TRY(atlas_json_key_bool(j, "branch_rewrite", s->branch_rewrite, err));
    TRY(atlas_json_key_int(j, "events_appended", s->events_appended, err));
    TRY(atlas_json_key_int(j, "write_batches", s->batches_written, err));
    TRY(atlas_json_key_int(j, "duration_ms", s->duration_ms, err));
    TRY(atlas_json_key_bool(j, "truncated", s->truncated, err));
    return atlas_json_key_str(j, "truncated_reason", atlas_buf_cstr(&s->truncated_reason), err);
}

static atlas_status j_event_item(atlas_renderer *r, const atlas_event_row *row, atlas_err *err) {
    r->items++;
    TRY(atlas_json_obj_begin(r->j, err));
    TRY(atlas_json_key_int(r->j, "cursor", row->id, err));
    TRY(atlas_json_key_int(r->j, "generation", row->generation, err));
    TRY(atlas_json_key_str(r->j, "kind", row->kind, err));
    /* Stored already encoded; re-encoding would stop it decoding back. */
    TRY(atlas_json_key_str_opt(r->j, "path", row->path_text, err));
    TRY(json_safe(r->j, &r->safe, "detail", row->detail, err));
    TRY(atlas_json_key_str(r->j, "at", row->created_at, err));
    return atlas_json_obj_end(r->j, err);
}

static atlas_status j_events_end(atlas_renderer *r, int64_t cursor, bool more, atlas_err *err) {
    TRY(atlas_json_key_int(r->j, "cursor", cursor, err));
    /* Explicit, so a consumer never has to infer completeness from a page size. */
    return atlas_json_key_bool(r->j, "more", more, err);
}

static atlas_status j_unit_text(atlas_renderer *r, const char *text, atlas_err *err) {
    /* The unit is Atlas-generated and contains only paths that passed
     * check_unit_safe_path, so it holds no control bytes; it still goes through
     * the JSON writer's escaping like any other string. */
    return atlas_json_key_str(r->j, "unit", text, err);
}

static atlas_status j_unit_install(atlas_renderer *r, const atlas_unit_install_report *rep,
                                   bool uninstall, atlas_err *err) {
    atlas_json *j = r->j;
    TRY(json_safe(j, &r->safe, "path", atlas_buf_cstr(&rep->path), err));
    TRY(json_safe(j, &r->safe, "directory", atlas_buf_cstr(&rep->dir), err));
    if (uninstall) {
        TRY(atlas_json_key_bool(j, "removed", rep->removed, err));
        return atlas_json_key_bool(j, "was_absent", rep->was_absent, err);
    }
    TRY(atlas_json_key_bool(j, "created_directory", rep->created_dir, err));
    TRY(atlas_json_key_bool(j, "wrote_file", rep->wrote_file, err));
    TRY(atlas_json_key_bool(j, "replaced_existing", rep->replaced_existing, err));
    TRY(atlas_json_key_bool(j, "unchanged", rep->unchanged, err));
    TRY(atlas_json_key_str(j, "mode", "0600", err));
    /* Stated as a field, not only in prose: a caller must be able to check that
     * installing did not also start anything. */
    TRY(atlas_json_key_bool(j, "enabled", false, err));
    return atlas_json_key_bool(j, "started", false, err);
}

static atlas_status j_integrate(atlas_renderer *r, const atlas_integrate_report *rep,
                                const char *action, const char *commands, atlas_err *err) {
    atlas_json *j = r->j;
    TRY(atlas_json_key_str(j, "action", action, err));
    if (commands != NULL) {
        /* The commands are Atlas-owned text with one interpolated path, which
         * `json_safe` encodes like any other value that came from outside. */
        return json_safe(j, &r->safe, "commands", commands, err);
    }
    TRY(json_safe(j, &r->safe, "executable", atlas_buf_cstr(&rep->exe), err));
    TRY(json_safe(j, &r->safe, "plugin_dir", atlas_buf_cstr(&rep->plugin_dir), err));
    TRY(json_safe(j, &r->safe, "marketplace_dir", atlas_buf_cstr(&rep->marketplace_dir), err));
    TRY(atlas_json_key_str(j, "plugin_source", atlas_plugin_source_name(rep->plugin_source), err));
    TRY(atlas_json_key_bool(j, "plugin_found", rep->plugin_found, err));
    TRY(atlas_json_key_bool(j, "marketplace_ok", rep->marketplace_ok, err));
    TRY(atlas_json_key_bool(j, "marketplace_registered", rep->marketplace_registered, err));
    /* The four load states, distinguished because they are fixed by four
     * different commands. */
    TRY(atlas_json_key_str(j, "claude_plugin_state", atlas_claude_state_name(rep->claude_state),
                           err));
    TRY(json_safe(j, &r->safe, "claude_config_dir", atlas_buf_cstr(&rep->claude_config_dir), err));
    TRY(json_safe(j, &r->safe, "installed_id", atlas_buf_cstr(&rep->installed_id), err));
    TRY(json_safe(j, &r->safe, "installed_scope", atlas_buf_cstr(&rep->installed_scope), err));
    TRY(json_safe(j, &r->safe, "installed_path", atlas_buf_cstr(&rep->installed_path), err));
    TRY(atlas_json_key_bool(j, "index_present", rep->index_present, err));
    TRY(json_safe(j, &r->safe, "config_path", atlas_buf_cstr(&rep->config_path), err));
    TRY(atlas_json_key_bool(j, "config_present", rep->config_present, err));
    TRY(atlas_json_key_bool(j, "manifest_ok", rep->manifest_ok, err));
    TRY(atlas_json_key_bool(j, "hooks_ok", rep->hooks_ok, err));
    TRY(atlas_json_key_bool(j, "mcp_ok", rep->mcp_ok, err));
    TRY(atlas_json_key_bool(j, "skill_ok", rep->skill_ok, err));
    TRY(atlas_json_key_bool(j, "launcher_ok", rep->launcher_ok, err));
    TRY(atlas_json_key_int(j, "hook_events", rep->hook_events, err));
    TRY(atlas_json_key_int(j, "mcp_tools", rep->mcp_tools, err));
    TRY(atlas_json_key_bool(j, "mcp_selftest_ok", rep->mcp_selftest_ok, err));
    TRY(json_safe(j, &r->safe, "mcp_selftest_detail", atlas_buf_cstr(&rep->mcp_selftest_detail),
                  err));
    TRY(json_safe(j, &r->safe, "socket", atlas_buf_cstr(&rep->socket_path), err));
    TRY(atlas_json_key_bool(j, "daemon_reachable", rep->daemon_reachable, err));
    TRY(atlas_json_key_bool(j, "wrote_config", rep->wrote_config, err));
    /* Stated as fields, not only in prose: a caller must be able to check that
     * an uninstall removed the record and nothing else. */
    TRY(atlas_json_key_bool(j, "removed_config", rep->removed_config, err));
    TRY(atlas_json_key_bool(j, "removed_index", false, err));
    TRY(atlas_json_key_bool(j, "claude_configured", false, err));
    TRY(atlas_json_key_bool(j, "service_enabled", false, err));
    TRY(atlas_json_key_bool(j, "ok", rep->ok, err));
    return json_safe(j, &r->safe, "problems", atlas_buf_cstr(&rep->problems), err);
}

const atlas_renderer_vtbl ATLAS_RENDERER_JSON = {
    j_begin,      j_end,          j_note_repo,    j_note_query,   j_list_begin,
    j_list_end,   j_doctor,       j_version,      j_repo_item,    j_repo_added,
    j_repo_removed, j_scan,       j_status,       j_search_item,  j_file,
    j_history_item, j_diff_begin, j_diff_item,    j_diff_end,
    j_daemon_status, j_daemon_ping, j_repo_state, j_sync,         j_event_item,
    j_events_end, j_unit_text,    j_unit_install, j_integrate,
};

void atlas_render_error(FILE *out, FILE *errout, bool json, const char *command,
                        const atlas_err *err) {
    /* An error message can quote a repository path or git's own output, so it is
     * encoded before it reaches a terminal in either mode. */
    atlas_safe_pool pool;
    atlas_safe_pool_init(&pool);
    const char *safe_msg = atlas_safe(&pool, atlas_err_msg(err));

    if (!json) {
        (void)fprintf(errout, "atlas: %s\n", safe_msg);
        atlas_safe_pool_free(&pool);
        return;
    }
    /* A caller parsing stdout must still receive a valid document on failure. */
    atlas_err local;
    atlas_err_init(&local);
    atlas_json *j = atlas_json_new(out, &local);
    if (j == NULL) {
        (void)fprintf(errout, "atlas: %s\n", safe_msg);
        atlas_safe_pool_free(&pool);
        return;
    }
    if (atlas_json_obj_begin(j, &local) == ATLAS_OK) {
        (void)atlas_json_key_str(j, "atlas", ATLAS_VERSION_STRING, &local);
        (void)atlas_json_key_str(j, "phase", ATLAS_PHASE, &local);
        (void)atlas_json_key_str(j, "command", command != NULL ? command : "", &local);
        (void)atlas_json_key_bool(j, "ok", false, &local);
        (void)atlas_json_key_str(j, "text_encoding", ATLAS_TEXT_ENCODING_NAME, &local);
        (void)atlas_json_key(j, "error", &local);
        (void)atlas_json_obj_begin(j, &local);
        (void)atlas_json_key_str(j, "status", atlas_status_name(err->status), &local);
        (void)atlas_json_key_int(j, "exit_code", (int64_t)err->status, &local);
        (void)atlas_json_key_str(j, "message", safe_msg, &local);
        (void)atlas_json_obj_end(j, &local);
        (void)atlas_json_obj_end(j, &local);
    }
    (void)atlas_json_finish(j, &local);
    (void)fprintf(errout, "atlas: %s\n", safe_msg);
    atlas_safe_pool_free(&pool);
}
