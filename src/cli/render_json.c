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

#include "atlas/orch.h"
#include "atlas/sem_discover.h"
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
    /* A7.1: "there is no index" and "there is an index I may not read" are
     * different facts, and only the second one has a next step. */
    TRY(atlas_json_key_bool(j, "index_unreadable", rep->index_unreadable, err));
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
    TRY(atlas_json_key_int(j, "repositories_without_scanner", rep->repos_without_scanner, err));
    /* A7. Closed vocabularies, so no encoding is needed or wanted. */
    TRY(atlas_json_key_str(j, "operator_authority",
                           rep->authority_state == ATLAS_AUTHORITY_GRANTED ? "granted" : "locked",
                           err));
    TRY(atlas_json_key_str(j, "operator_authority_reason",
                           atlas_authority_reason_name(rep->authority_reason), err));
    TRY(atlas_json_key_str(j, "operator_authority_detail",
                           atlas_authority_reason_explain(rep->authority_reason), err));
    TRY(atlas_json_key_str(j, "deployment",
                           rep->deployment_state == ATLAS_SYSPOLICY_SYSTEM ? "system" : "per-user",
                           err));
    TRY(atlas_json_key_str(j, "deployment_reason",
                           atlas_syspolicy_reason_name(rep->deployment_reason), err));
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
    TRY(atlas_json_key_int(j, "scanner_uid", ri->scanner_uid, err));
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
/* A13. `repo scanner`. The whole result is the assignment; the repository body
 * would bury it and `repo list` already carries the rest. */
static atlas_status j_repo_scanner_set(atlas_renderer *r, const atlas_repo_info *ri,
                                       atlas_err *err) {
    TRY(atlas_json_key_str(r->j, "repo", ri->name, err));
    return atlas_json_key_int(r->j, "scanner_uid", ri->scanner_uid, err);
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

/* A8. Same note as the human renderer: these strings are already safe-encoded
 * by the daemon and are emitted through the JSON writer, which escapes for JSON
 * structure. Neither step is the other, and neither is applied twice. */
static atlas_status j_job_item(atlas_renderer *r, const atlas_job_render *jr, atlas_err *err) {
    atlas_json *j = r->j;
    /* Inside the `jobs` array this is an anonymous object; on its own it is a
     * set of members on the document. The writer refuses an unkeyed object at
     * the top level, which is the correct refusal and is why this branch
     * exists. */
    atlas_status st = jr->in_list ? atlas_json_obj_begin(j, err) : ATLAS_OK;
    struct {
        const char *k;
        const char *v;
    } strs[] = {
        {"job", jr->job},         {"state", jr->state},
        {"repo", jr->repo},       {"driver", jr->driver},
        {"created_at", jr->created_at},
    };
    for (size_t i = 0; st == ATLAS_OK && i < sizeof strs / sizeof strs[0]; i++) {
        if (strs[i].v != NULL) {
            st = atlas_json_key_str(j, strs[i].k, strs[i].v, err);
        }
    }
    if (st == ATLAS_OK && jr->detail) {
        struct {
            const char *k;
            const char *v;
        } more[] = {
            {"commit", jr->commit},           {"spec_digest", jr->spec_digest},
            {"terminal_at", jr->terminal_at}, {"task", jr->task},
        };
        for (size_t i = 0; st == ATLAS_OK && i < sizeof more / sizeof more[0]; i++) {
            if (more[i].v != NULL && more[i].v[0] != '\0') {
                st = atlas_json_key_str(j, more[i].k, more[i].v, err);
            }
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "task_encoding", "atlas-safe-1", err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(j, "max_attempts", jr->max_attempts, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_bool(j, "cancel_requested", jr->cancel_requested, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(j, "seq", jr->seq, err);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "attempts", jr->attempts, err);
    }
    /* A11.0/A11.1. Emitted only when present, and outside the detail block
     * because a list row carries a run too. An absent key means "this job
     * belongs to no run"; there is deliberately no empty-string spelling of
     * that, since an empty identifier reads like an identifier. */
    {
        struct {
            const char *k;
            const char *v;
        } runf[] = {
            {"run", jr->run}, {"run_status", jr->run_status}, {"follow_up", jr->follow_up},
        };
        for (size_t i = 0; st == ATLAS_OK && i < sizeof runf / sizeof runf[0]; i++) {
            if (runf[i].v != NULL && runf[i].v[0] != '\0') {
                st = atlas_json_key_str(j, runf[i].k, runf[i].v, err);
            }
        }
    }
    if (st == ATLAS_OK && (jr->tasks > 0 || jr->worker_starts > 0)) {
        st = atlas_json_key_int(j, "worker_starts", jr->worker_starts, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(j, "worker_start_limit", ATLAS_ORCH_RUN_MAX_WORKER_STARTS,
                                    err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(j, "tasks_carried", jr->tasks, err);
        }
    }
    /* A11.6. Both keys, or neither. They are emitted only when the bound is
     * present — a daemon that predates them reports nothing, and an emitted
     * `max_parallel` of 0 would be a claim about a run made from a key that
     * never arrived. `active_count` travels with it because the two answer one
     * question together and a count with no bound beside it is not an answer. */
    if (st == ATLAS_OK && jr->max_parallel > 0) {
        st = atlas_json_key_int(j, "active_count", jr->active_count, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(j, "max_parallel", jr->max_parallel, err);
        }
    }
    if (st == ATLAS_OK && jr->usage_present) {
        /* A10.0. One writer for the block, and the status is always present:
         * an absent status means a daemon that does not report usage, which is
         * a different fact from a run whose cost nobody observed. */
        const atlas_usage_run *u = &jr->usage;
        st = atlas_json_key_str(j, "usage_status", atlas_usage_status_name(u->status), err);
        const struct {
            const char *k;
            int64_t v;
        } A[] = {
            {"usage_attempts_started", u->attempts_started},
            {"usage_attempts_measured", u->attempts_with_usage},
            {"usage_attempts_missing", u->attempts_missing_usage},
        };
        for (size_t i = 0; st == ATLAS_OK && i < sizeof A / sizeof A[0]; i++) {
            st = atlas_json_key_int(j, A[i].k, A[i].v, err);
        }
        if (st == ATLAS_OK && u->status != ATLAS_USAGE_UNKNOWN) {
            const struct {
                const char *k;
                int64_t v;
            } T[] = {
                {"usage_input_tokens", u->input_tokens},
                {"usage_output_tokens", u->output_tokens},
                {"usage_cache_creation_tokens", u->cache_creation_tokens},
                {"usage_cache_read_tokens", u->cache_read_tokens},
                {"usage_worker_duration_ms", u->worker_duration_ms},
                {"usage_turns", u->turns},
            };
            for (size_t i = 0; st == ATLAS_OK && i < sizeof T / sizeof T[0]; i++) {
                st = atlas_json_key_int(j, T[i].k, T[i].v, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_key_bool(j, "usage_tokens_complete", u->tokens_complete, err);
            }
            if (st == ATLAS_OK && u->has_any_cost) {
                st = atlas_json_key_int(j, "usage_cost_known_micro_usd", u->cost_known_micro_usd,
                                        err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_key_bool(j, "usage_cost_complete", u->cost_complete, err);
            }
        }
    }
    if (st == ATLAS_OK && jr->memory_present) {
        /* A10.1. The same block the human renderer prints, from the same
         * fields: one service result, two presentations. Absent entirely for a
         * run with no frozen manifest, which is deliberately not the same
         * document as a run that ran with memory OFF. */
        st = atlas_json_key_str(j, "memory_mode",
                                jr->memory_mode != NULL ? jr->memory_mode : "UNKNOWN", err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(
                j, "memory_package_status",
                jr->memory_package_status != NULL ? jr->memory_package_status : "UNKNOWN", err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(
                j, "memory_package_digest",
                jr->memory_package_digest != NULL ? jr->memory_package_digest : "", err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(j, "memory_package_bytes", jr->memory_package_bytes, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(j, "memory_source_count", jr->memory_source_count, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_bool(j, "memory_candidates_truncated",
                                     jr->memory_candidates_truncated, err);
        }
        if (st == ATLAS_OK && jr->memory_source_listed > 0) {
            st = atlas_json_key(j, "memory_sources", err);
            if (st == ATLAS_OK) {
                st = atlas_json_arr_begin(j, err);
            }
            for (size_t i = 0; st == ATLAS_OK && i < jr->memory_source_listed; i++) {
                st = atlas_json_str(j, jr->memory_sources[i], err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_arr_end(j, err);
            }
        }
    }
    if (st == ATLAS_OK && jr->busy) {
        /* Present only when true, and named so it cannot be read as a verdict
         * about the run: nothing was started and nothing was written. */
        st = atlas_json_key_bool(j, "busy", true, err);
    }
    if (st == ATLAS_OK && jr->in_list) {
        st = atlas_json_obj_end(j, err);
    }
    return st;
}

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
    TRY(atlas_json_key_int(j, "priming", rep->priming_repos, err));
    TRY(atlas_json_key_int(j, "repositories_with_event_gap", rep->repos_with_gap, err));
    /* P0. `watches` is physical descriptors, `watch_subscriptions` is
     * (repository, descriptor) pairs, and the second is >= the first whenever
     * worktrees share a git directory. Both are emitted so nothing downstream
     * has to guess which one it is looking at.
     *
     * Emitted **only when the daemon reported them**. These come from the live
     * watcher, which exists in the daemon's process and nowhere else, so the
     * local path — a CLI that could open the data directory and answered from
     * the database — has nothing to say about them. Writing zeros there would be
     * a claim rather than a silence, and "the daemon holds no watches" is a very
     * different statement from "this process cannot see how many it holds". The
     * human renderer omits the same block for the same reason. */
    if (rep->watch_budget_total > 0) {
        TRY(atlas_json_key_int(j, "watches", rep->watches, err));
        TRY(atlas_json_key_int(j, "watch_subscriptions", rep->watch_subscriptions, err));
        TRY(atlas_json_key_int(j, "watch_budget_total", rep->watch_budget_total, err));
        TRY(atlas_json_key_int(j, "watch_budget_repo", rep->watch_budget_repo, err));
        TRY(atlas_json_key_int(j, "kernel_max_user_watches", rep->kernel_max_user_watches, err));
        TRY(atlas_json_key_str(j, "watch_budget_source",
                               rep->watch_budget_from_policy ? "policy" : "kernel", err));
        TRY(atlas_json_key_bool(j, "priming_complete", rep->priming_complete, err));
    }
    return ATLAS_OK;
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
    TRY(atlas_json_key_int(j, "watched_source", rep->state.watched_source, err));
    TRY(atlas_json_key_int(j, "watched_meta", rep->state.watched_meta, err));
    TRY(atlas_json_key_int(j, "watched_shared", rep->state.watched_shared, err));
    TRY(atlas_json_key_str(j, "watch_reason", atlas_watch_reason_name(rep->state.watch_reason),
                           err));
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
    TRY(atlas_json_key_str(j, "truncated_reason", atlas_buf_cstr(&s->truncated_reason), err));
    /* A3. Reported here rather than in a separate command, because the
     * structural stage is part of this pass: a caller checking "did an
     * unchanged pass parse nothing" is asking about the same pass whose file
     * counters are above. */
    TRY(atlas_json_key_bool(j, "code_ran", s->code_ran, err));
    TRY(atlas_json_key_int(j, "code_files_selected", s->code.files_selected, err));
    TRY(atlas_json_key_int(j, "code_files_parsed", s->code.files_parsed, err));
    TRY(atlas_json_key_int(j, "code_files_removed", s->code.files_removed, err));
    TRY(atlas_json_key_int(j, "code_files_failed", s->code.files_failed, err));
    TRY(atlas_json_key_int(j, "code_files_partial", s->code.files_partial, err));
    TRY(atlas_json_key_int(j, "code_symbols_written", s->code.symbols_written, err));
    TRY(atlas_json_key_int(j, "code_relations_written", s->code.relations_written, err));
    TRY(atlas_json_key_int(j, "code_relations_resolved", s->code.relations_resolved, err));
    TRY(atlas_json_key_int(j, "code_compile_units", s->code.compile_units, err));
    TRY(atlas_json_key_bool(j, "code_compile_db_present", s->code.compile_db_present, err));
    /* Reported, never silent: a whole-repository re-resolution is still
     * resolution and never a reparse, and a reader is entitled to know which
     * happened. */
    TRY(atlas_json_key_bool(j, "code_resolve_fallback", s->code.resolve_fallback, err));
    TRY(atlas_json_key_bool(j, "code_degraded", s->code.degraded, err));
    TRY(atlas_json_key_str_opt(j, "code_degraded_reason", s->code.degraded_reason, err));
    TRY(atlas_json_key_bool(j, "code_truncated", s->code.truncated, err));
    TRY(atlas_json_key_str_opt(j, "code_truncated_reason", s->code.truncated_reason, err));
    return atlas_json_key_int(j, "code_duration_ms", s->code.duration_ms, err);
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

/* --- A3: structural code intelligence ------------------------------------
 *
 * Paths and symbol names are already in the safe encoding when they are stored,
 * so they are emitted as-is; re-encoding would stop them decoding back to the
 * original bytes. Resolution classes, provenance, roles, bases and unresolved
 * reasons are fixed vocabularies. `untrusted_data` is set on every object that
 * carries a name or a path, because those are chosen by whoever can commit. */

static atlas_status j_code_status(atlas_renderer *r, const atlas_code_status_report *rep,
                                  atlas_err *err) {
    TRY(atlas_json_key_str(r->j, "repo", rep->repo.name, err));
    TRY(atlas_json_key_bool(r->j, "index_current", rep->file_index_current, err));
    TRY(atlas_json_key_bool(r->j, "code_index_current", rep->code_index_current, err));
    TRY(atlas_json_key_str_opt(r->j, "code_not_current_reason", rep->not_current_reason, err));
    TRY(atlas_json_key_int(r->j, "generation", rep->file_state.last_complete_generation, err));
    TRY(atlas_json_key_int(r->j, "code_generation", rep->code_state.last_complete_generation,
                           err));
    TRY(atlas_json_key_int(r->j, "files_indexed", rep->code_state.files_indexed, err));
    TRY(atlas_json_key_int(r->j, "files_parsed_last", rep->code_state.files_parsed_last, err));
    TRY(atlas_json_key_int(r->j, "symbols", rep->code_state.symbols, err));
    TRY(atlas_json_key_int(r->j, "relations", rep->code_state.relations, err));
    TRY(atlas_json_key_int(r->j, "ambiguous", rep->code_state.ambiguous, err));
    TRY(atlas_json_key_int(r->j, "unresolved", rep->code_state.unresolved, err));
    TRY(atlas_json_key_bool(r->j, "degraded", rep->code_state.degraded, err));
    TRY(json_safe(r->j, &r->safe, "degraded_reason",
                  rep->code_state.degraded ? atlas_buf_cstr(&rep->code_state.degraded_reason)
                                           : NULL,
                  err));
    TRY(atlas_json_key_bool(r->j, "compile_db_present", rep->code_state.compile_db_present, err));
    TRY(atlas_json_key_int(r->j, "compile_units", rep->code_state.compile_units, err));
    TRY(atlas_json_key_int(r->j, "compile_entries_dropped",
                           rep->code_state.compile_entries_dropped, err));
    TRY(atlas_json_key_str(r->j, "last_complete_at", rep->code_state.last_complete_at, err));
    /* Atlas-owned constants, emitted unencoded on purpose: `analyzer` is the
     * value of ATLAS_CODE_ANALYZER_ID compiled into this binary when the pass
     * ran, so it is a fixed vocabulary rather than a stored string a repository
     * could have chosen. `analyzer_expected` is what this binary would produce
     * now, and the two differ exactly when the graph is stale for that reason. */
    TRY(atlas_json_key_str(r->j, "analyzer",
                           atlas_buf_cstr(&rep->code_state.analyzer_name), err));
    TRY(atlas_json_key_int(r->j, "analyzer_version", rep->code_state.analyzer_version, err));
    TRY(atlas_json_key_str(r->j, "analyzer_expected", ATLAS_CODE_ANALYZER_ID, err));
    TRY(atlas_json_key_int(r->j, "analyzer_version_expected",
                           (int64_t)ATLAS_CODE_ANALYZER_VERSION, err));
    return ATLAS_OK;
}

static atlas_status j_code_file(atlas_renderer *r, const atlas_code_file_report *rep,
                                atlas_err *err) {
    /* Already encoded when stored. */
    TRY(atlas_json_key_str(r->j, "path", atlas_buf_cstr(&rep->path_text), err));
    TRY(atlas_json_key_bool(r->j, "indexed", rep->indexed, err));
    if (!rep->indexed) {
        TRY(atlas_json_key_str(r->j, "reason",
                               "Atlas extracts structure from C sources, headers and included "
                               "fragments only",
                               err));
        return ATLAS_OK;
    }
    TRY(atlas_json_key_str(r->j, "language", rep->language, err));
    TRY(atlas_json_key_str(r->j, "parse_status", rep->parse_status, err));
    TRY(json_safe(r->j, &r->safe, "parse_detail",
                  rep->parse_detail.len > 0 ? atlas_buf_cstr(&rep->parse_detail) : NULL, err));
    TRY(atlas_json_key_bool(r->j, "truncated", rep->truncated, err));
    TRY(json_safe(r->j, &r->safe, "truncated_reason",
                  rep->truncated_reason.len > 0 ? atlas_buf_cstr(&rep->truncated_reason) : NULL,
                  err));
    TRY(atlas_json_key_bool(r->j, "include_guard", rep->include_guard, err));
    TRY(atlas_json_key(r->j, "roles", err));
    TRY(atlas_json_arr_begin(r->j, err));
    for (size_t i = 0; i < rep->role_count; i++) {
        TRY(atlas_json_obj_begin(r->j, err));
        TRY(atlas_json_key_str(r->j, "role", rep->roles[i].role, err));
        /* How the role was arrived at, always. Path naming is evidence about a
         * path and not proof about a file. */
        TRY(atlas_json_key_str(r->j, "basis", rep->roles[i].basis, err));
        TRY(atlas_json_key_str(r->j, "resolution", rep->roles[i].resolution, err));
        TRY(atlas_json_obj_end(r->j, err));
    }
    TRY(atlas_json_arr_end(r->j, err));
    TRY(atlas_json_key_str_opt(r->j, "content_hash",
                               rep->content_hash[0] != '\0' ? rep->content_hash : NULL, err));
    TRY(atlas_json_key_int(r->j, "generation", rep->generation, err));
    /* Named so they cannot collide with the lists that follow them. Three
     * members called `symbols` in one object is a document whose meaning
     * depends on which one a parser keeps. */
    TRY(atlas_json_key_int(r->j, "symbol_count", rep->symbol_count, err));
    TRY(atlas_json_key_int(r->j, "include_count", rep->include_count, err));
    TRY(atlas_json_key_int(r->j, "call_candidate_count", rep->occurrence_count, err));
    TRY(atlas_json_key_int(r->j, "ambiguous", rep->ambiguous, err));
    TRY(atlas_json_key_int(r->j, "unresolved", rep->unresolved, err));
    TRY(atlas_json_key_int(r->j, "bytes", rep->bytes, err));
    TRY(atlas_json_key_int(r->j, "lines", rep->lines, err));
    TRY(atlas_json_key_bool(r->j, "code_index_current", rep->code_index_current, err));
    TRY(atlas_json_key_str_opt(r->j, "code_not_current_reason", rep->not_current_reason, err));
    /* A0's answer, unchanged: structure is not a reason. */
    TRY(atlas_json_key_str(r->j, "reason", ATLAS_REASON_UNKNOWN, err));
    return ATLAS_OK;
}

static atlas_status j_code_symbol_item(atlas_renderer *r, const atlas_code_symbol_row *row,
                                       atlas_err *err) {
    TRY(atlas_json_obj_begin(r->j, err));
    TRY(atlas_json_key_int(r->j, "id", row->id, err));
    TRY(atlas_json_key_str(r->j, "name", row->name_text, err));
    TRY(atlas_json_key_str(r->j, "kind", row->kind, err));
    TRY(atlas_json_key_str(r->j, "linkage", row->linkage, err));
    TRY(atlas_json_key_str(r->j, "path", row->path_text, err));
    TRY(atlas_json_key_int(r->j, "line", row->line, err));
    TRY(atlas_json_key_int(r->j, "col", row->col, err));
    TRY(atlas_json_key_bool(r->j, "definition", row->is_definition, err));
    TRY(atlas_json_key_bool(r->j, "declaration", row->is_declaration, err));
    TRY(atlas_json_key_str(r->j, "resolution", row->resolution, err));
    TRY(atlas_json_key_bool(r->j, "untrusted_data", true, err));
    TRY(atlas_json_obj_end(r->j, err));
    return ATLAS_OK;
}

static atlas_status j_code_edge_item(atlas_renderer *r, const atlas_code_edge_row *row,
                                     atlas_err *err) {
    TRY(atlas_json_obj_begin(r->j, err));
    TRY(atlas_json_key_str(r->j, "kind", row->kind, err));
    TRY(atlas_json_key_str(r->j, "from_kind", row->src_kind, err));
    TRY(atlas_json_key_str_opt(r->j, "from_path", row->src_path_text, err));
    TRY(atlas_json_key_str(r->j, "to_kind", row->dst_kind, err));
    TRY(atlas_json_key_str_opt(r->j, "to_path", row->dst_path_text, err));
    /* The spelling, kept whether or not anything resolved it. */
    TRY(atlas_json_key_str_opt(r->j, "spelling", row->dst_name_text, err));
    TRY(atlas_json_key_str(r->j, "resolution", row->resolution, err));
    TRY(atlas_json_key_str(r->j, "provenance", row->provenance, err));
    TRY(atlas_json_key_int(r->j, "candidates", row->candidate_count, err));
    TRY(atlas_json_key_str_opt(
        r->j, "reason",
        row->detail == NULL ? NULL : (atlas_code_why_is_known(row->detail) ? row->detail : "other"),
        err));
    TRY(atlas_json_key_int(r->j, "line", row->line, err));
    TRY(atlas_json_key_bool(r->j, "untrusted_data", true, err));
    TRY(atlas_json_obj_end(r->j, err));
    return ATLAS_OK;
}

static atlas_status j_code_walk_item(atlas_renderer *r, const atlas_code_walk_row *row,
                                     atlas_err *err) {
    TRY(atlas_json_obj_begin(r->j, err));
    TRY(atlas_json_key_int(r->j, "depth", row->depth, err));
    TRY(atlas_json_key_str(r->j, "node_kind", row->node_kind, err));
    TRY(atlas_json_key_str(r->j, "node", row->label, err));
    /* Why this candidate is here. Every impact result carries its path. */
    TRY(atlas_json_key_str(r->j, "via", row->via_label, err));
    TRY(atlas_json_key_str(r->j, "edge", row->edge_kind, err));
    TRY(atlas_json_key_str(r->j, "resolution", row->resolution, err));
    TRY(atlas_json_key_str_opt(
        r->j, "reason",
        row->detail == NULL ? NULL : (atlas_code_why_is_known(row->detail) ? row->detail : "other"),
        err));
    TRY(atlas_json_key_bool(r->j, "untrusted_data", true, err));
    TRY(atlas_json_obj_end(r->j, err));
    return ATLAS_OK;
}

static atlas_status j_code_walk_end(atlas_renderer *r, const atlas_code_walk_summary *sum,
                                    atlas_err *err) {
    TRY(atlas_json_key_int(r->j, "exact", sum->exact, err));
    TRY(atlas_json_key_int(r->j, "unique_lexical", sum->unique_lexical, err));
    TRY(atlas_json_key_int(r->j, "ambiguous", sum->ambiguous, err));
    TRY(atlas_json_key_int(r->j, "unresolved", sum->unresolved, err));
    TRY(atlas_json_key_int(r->j, "visited", sum->visited, err));
    TRY(atlas_json_key_bool(r->j, "truncated", sum->truncated, err));
    TRY(atlas_json_key_str_opt(r->j, "truncated_reason", sum->truncated_reason, err));
    /* In the document rather than only in the documentation: this is the
     * sentence that stops an impact list being read as a prediction. */
    TRY(atlas_json_key_str(r->j, "notice",
                           "These are graph paths, not predictions. Atlas is not a compiler: a "
                           "candidate here shares a recorded structural relation with what you "
                           "named, and may or may not be affected by changing it.",
                           err));
    return ATLAS_OK;
}

static atlas_status j_code_list_begin(atlas_renderer *r, const char *key, atlas_err *err) {
    r->items = 0;
    r->in_list = true;
    TRY(atlas_json_key(r->j, key, err));
    return atlas_json_arr_begin(r->j, err);
}

static atlas_status j_code_list_end(atlas_renderer *r, const char *key, const char *singular,
                                    const char *plural, int64_t count, bool more,
                                    atlas_err *err) {
    (void)singular;
    (void)plural;
    r->in_list = false;
    TRY(atlas_json_arr_end(r->j, err));
    /* `<key>_count` rather than `count`, because a document with three lists in
     * one object needs three counts that can be told apart. Pagination is
     * explicit for the same reason it is everywhere else: a caller is told there
     * is more rather than inferring it from a full page. */
    char buf[64];
    (void)snprintf(buf, sizeof(buf), "%s_count", key);
    TRY(atlas_json_key_int(r->j, buf, count, err));
    (void)snprintf(buf, sizeof(buf), "%s_more", key);
    return atlas_json_key_bool(r->j, buf, more, err);
}


/* --- A4: decision documents ---------------------------------------------
 *
 * Every string here arrives already safe-encoded from the service layer, so it
 * is written as-is — encoding it again would turn a `%` in somebody's decision
 * into `%25`. What the JSON writer still does is its own JSON escaping, which
 * is a different concern and is not optional.
 *
 * Every object that carries prose also carries `trust: UNTRUSTED_DATA`. On
 * every object rather than once per document, because a consumer that lifts one
 * element out of an array must carry the label with the text it took. */

static atlas_status j_decision_summary_members(atlas_renderer *r,
                                               const atlas_decision_summary *s, atlas_err *err) {
    TRY(atlas_json_key_str(r->j, "decision", atlas_buf_cstr(&s->uid), err));
    TRY(atlas_json_key_str(r->j, "status", atlas_buf_cstr(&s->status), err));
    /* A9.1: the same key the daemon and the web API emit, beside `status` and
     * never merged with it. Both surfaces name the two dimensions identically so
     * a client written against one reads the other. */
    TRY(atlas_json_key_str(r->j, "kind", atlas_buf_cstr(&s->kind), err));
    TRY(atlas_json_key_int(r->j, "revision", s->revision_no, err));
    TRY(atlas_json_key_int(r->j, "latest_revision", s->latest_revision_no, err));
    TRY(atlas_json_key_str(r->j, "revision_state", atlas_buf_cstr(&s->revision_state), err));
    TRY(atlas_json_key_str(r->j, "content_hash", atlas_buf_cstr(&s->content_hash), err));
    TRY(atlas_json_key_str(r->j, "proposed_by", atlas_buf_cstr(&s->proposed_by), err));
    TRY(atlas_json_key_str_opt(r->j, "superseded_by",
                               s->superseded_by.len > 0 ? atlas_buf_cstr(&s->superseded_by) : NULL,
                               err));
    TRY(atlas_json_key_int(r->j, "links", s->link_count, err));
    TRY(atlas_json_key_str(r->j, "created_at", atlas_buf_cstr(&s->created_at), err));
    TRY(atlas_json_key_str(r->j, "updated_at", atlas_buf_cstr(&s->updated_at), err));
    TRY(atlas_json_key_str(r->j, "title", atlas_buf_cstr(&s->title), err));
    return atlas_json_key_str(r->j, "trust", "UNTRUSTED_DATA", err);
}

static atlas_status j_decision_item(atlas_renderer *r, const atlas_decision_summary *s,
                                    atlas_err *err) {
    r->items++;
    TRY(atlas_json_obj_begin(r->j, err));
    TRY(j_decision_summary_members(r, s, err));
    return atlas_json_obj_end(r->j, err);
}

static atlas_status j_decision_show(atlas_renderer *r, const atlas_decision_document *d,
                                    atlas_err *err) {
    TRY(atlas_json_key_str(r->j, "repo", atlas_buf_cstr(&d->repo), err));
    TRY(j_decision_summary_members(r, &d->summary, err));
    TRY(atlas_json_key_str(r->j, "scope", atlas_buf_cstr(&d->scope), err));
    TRY(atlas_json_key_str_opt(r->j, "basis_head",
                               d->basis_head.len > 0 ? atlas_buf_cstr(&d->basis_head) : NULL, err));
    /* Null rather than an empty string when nothing was captured: "no identity
     * was knowable" and "the identity is the empty string" must not read the
     * same to a parser. */
    TRY(atlas_json_key_str_opt(r->j, "basis_repo_identity",
                               d->basis_repo_identity.len > 0
                                   ? atlas_buf_cstr(&d->basis_repo_identity)
                                   : NULL,
                               err));
    TRY(atlas_json_key_str(r->j, "context", atlas_buf_cstr(&d->context_text), err));
    TRY(atlas_json_key_str(r->j, "decision_text", atlas_buf_cstr(&d->decision_text), err));
    TRY(atlas_json_key_str(r->j, "rationale", atlas_buf_cstr(&d->rationale_text), err));
    TRY(atlas_json_key_str(r->j, "consequences", atlas_buf_cstr(&d->consequences_text), err));
    TRY(atlas_json_key(r->j, "alternatives", err));
    TRY(atlas_json_arr_begin(r->j, err));
    for (size_t i = 0; i < d->alternative_count; i++) {
        TRY(atlas_json_str(r->j, atlas_buf_cstr(&d->alternatives[i]), err));
    }
    TRY(atlas_json_arr_end(r->j, err));

    TRY(atlas_json_key(r->j, "links", err));
    TRY(atlas_json_arr_begin(r->j, err));
    for (size_t i = 0; i < d->link_count; i++) {
        const atlas_decision_link_view *l = &d->links[i];
        TRY(atlas_json_obj_begin(r->j, err));
        TRY(atlas_json_key_str(r->j, "kind", atlas_buf_cstr(&l->kind), err));
        TRY(atlas_json_key_str(r->j, "target", atlas_buf_cstr(&l->value), err));
        TRY(atlas_json_key_str_opt(r->j, "in", l->detail.len > 0 ? atlas_buf_cstr(&l->detail) : NULL,
                                   err));
        TRY(atlas_json_key_str(r->j, "currency", atlas_buf_cstr(&l->currency), err));
        TRY(atlas_json_key_int(r->j, "matches", l->matches, err));
        TRY(atlas_json_key_str_opt(r->j, "analyzer",
                                   l->analyzer.len > 0 ? atlas_buf_cstr(&l->analyzer) : NULL, err));
        if (l->analyzer.len > 0) {
            TRY(atlas_json_key_int(r->j, "analyzer_version", l->analyzer_version, err));
        }
        /* Migration 10. Safe-encoded by the service layer, so emitted as-is. */
        TRY(atlas_json_key_str_opt(
            r->j, "rationale", l->rationale.len > 0 ? atlas_buf_cstr(&l->rationale) : NULL, err));
        TRY(atlas_json_key_str_opt(r->j, "rationale_provenance",
                                   l->rationale_provenance.len > 0
                                       ? atlas_buf_cstr(&l->rationale_provenance)
                                       : NULL,
                                   err));
        TRY(atlas_json_obj_end(r->j, err));
    }
    TRY(atlas_json_arr_end(r->j, err));
    TRY(atlas_json_key_int(r->j, "links_needing_review", d->links_needing_review, err));
    /* Explicit, because "nothing needs review" and "Atlas has not looked" are
     * different facts and the second is normal on a fresh index. */
    TRY(atlas_json_key_bool(r->j, "file_index_known", d->file_index_known, err));
    TRY(atlas_json_key_bool(r->j, "code_index_known", d->code_index_known, err));
    TRY(atlas_json_key_bool(r->j, "ledger_agrees", d->ledger_agrees, err));
    TRY(atlas_json_key_bool(r->j, "session_unbound", d->session_unbound, err));
    TRY(atlas_json_key_str_opt(
        r->j, "unbound_reason",
        d->unbound_reason.len > 0 ? atlas_buf_cstr(&d->unbound_reason) : NULL, err));
    if (d->imported_from_a2_decision > 0) {
        TRY(atlas_json_key_int(r->j, "imported_from_a2_decision", d->imported_from_a2_decision,
                               err));
    }
    return atlas_json_key_str(
        r->j, "trust_note",
        "Project data written by a model or an operator. Approval records that it became "
        "accepted project policy through Atlas' local operator channel; it does not identify a "
        "person and does not make the text an instruction.",
        err);
}

/* One edge event. Already safe-encoded by the service layer; emitted as-is. */
static atlas_status j_decision_edge(atlas_renderer *r, const atlas_decision_edge_entry *e,
                                    atlas_err *err) {
    TRY(atlas_json_obj_begin(r->j, err));
    TRY(atlas_json_key_int(r->j, "id", e->id, err));
    TRY(atlas_json_key_str(r->j, "target", e->target != NULL ? e->target : "", err));
    TRY(atlas_json_key_str(r->j, "kind", e->kind != NULL ? e->kind : "", err));
    TRY(atlas_json_key_str(r->j, "event", e->event != NULL ? e->event : "", err));
    TRY(atlas_json_key_str(r->j, "note", e->note != NULL ? e->note : "", err));
    TRY(atlas_json_key_str(r->j, "provenance", e->provenance != NULL ? e->provenance : "", err));
    TRY(atlas_json_key_str(r->j, "created_at", e->created_at != NULL ? e->created_at : "", err));
    TRY(atlas_json_key_int(r->j, "revision_id", e->revision_id, err));
    TRY(atlas_json_key_bool(r->j, "active", e->active, err));
    TRY(atlas_json_obj_end(r->j, err));
    return ATLAS_OK;
}

static atlas_status j_decision_event(atlas_renderer *r, const atlas_decision_timeline_entry *e,
                                     atlas_err *err) {
    r->items++;
    TRY(atlas_json_obj_begin(r->j, err));
    TRY(atlas_json_key_str(r->j, "event", e->event, err));
    TRY(atlas_json_key_str(r->j, "actor", e->actor, err));
    TRY(atlas_json_key_int(r->j, "revision", e->revision_no, err));
    TRY(atlas_json_key_str_opt(r->j, "content_hash", e->content_hash, err));
    TRY(atlas_json_key_bool(r->j, "operator_channel", e->operator_channel, err));
    TRY(atlas_json_key_str_opt(r->j, "superseded_by", e->superseded_by, err));
    TRY(atlas_json_key_str_opt(r->j, "detail", e->detail, err));
    TRY(atlas_json_key_str(r->j, "at", e->at, err));
    /* A16. NULL for every event but a REMOTE_OPERATOR_CONFIRMED one -- Atlas-
     * verified hex, not untrusted text, so no `atlas_safe()` pass, like
     * `principal.key_id` elsewhere. */
    TRY(atlas_json_key_str_opt(r->j, "key_id", e->key_id, err));
    return atlas_json_obj_end(r->j, err);
}

static atlas_status j_decision_outcome(atlas_renderer *r, const atlas_decision_outcome *o,
                                       atlas_err *err) {
    TRY(atlas_json_key_str(r->j, "repo", atlas_buf_cstr(&o->repo), err));
    TRY(atlas_json_key_str(r->j, "decision", atlas_buf_cstr(&o->uid), err));
    TRY(atlas_json_key_int(r->j, "revision", o->revision_no, err));
    TRY(atlas_json_key_str(r->j, "state", atlas_buf_cstr(&o->state), err));
    /* A9.1: the same key every other decision object uses. */
    TRY(atlas_json_key_str(r->j, "kind", atlas_buf_cstr(&o->kind), err));
    TRY(atlas_json_key_str(r->j, "content_hash", o->content_hash, err));
    TRY(atlas_json_key_bool(r->j, "created", o->created, err));
    TRY(atlas_json_key_bool(r->j, "duplicate", o->duplicate, err));
    /* Only for a withdrawal. On any other command a `removed` key would read as
     * a statement that nothing was deleted, which is not an answer anyone asked
     * for. */
    if (o->is_removal) {
        TRY(atlas_json_key_bool(r->j, "removed", o->removed, err));
    }
    TRY(atlas_json_key_int(r->j, "superseded_revision", o->superseded_revision_no, err));
    TRY(atlas_json_key_str_opt(
        r->j, "replaced_by", o->replaced_by.len > 0 ? atlas_buf_cstr(&o->replaced_by) : NULL, err));
    TRY(atlas_json_key_bool(r->j, "session_unbound", o->session_unbound, err));
    TRY(atlas_json_key_str_opt(
        r->j, "unbound_reason",
        o->unbound_reason.len > 0 ? atlas_buf_cstr(&o->unbound_reason) : NULL, err));
    TRY(atlas_json_key_bool(r->j, "via_daemon", o->via_daemon, err));
    TRY(atlas_json_key_str(r->j, "actor",
                           o->operator_confirmed ? "LOCAL_OPERATOR_CONFIRMED" : "MODEL_PROPOSAL",
                           err));
    /* The same sentence the human renderer prints, so the two cannot come to
     * mean different things. */
    return atlas_json_key_str(
        r->j, "actor_means",
        o->operator_confirmed
            ? "an explicit action arrived through Atlas' local operator channel. This does not "
              "identify a person, does not prove a person was present, and is not a signature."
            : "a proposal, not an approval. Approval happens only through the interactive Atlas "
              "CLI on a terminal.",
        err);
}

static atlas_status j_decision_counts(atlas_renderer *r, const atlas_decision_counts *c,
                                      atlas_err *err) {
    TRY(atlas_json_key_int(r->j, "total_proposed", c->proposed, err));
    TRY(atlas_json_key_int(r->j, "total_approved", c->approved, err));
    TRY(atlas_json_key_int(r->j, "total_rejected", c->rejected, err));
    TRY(atlas_json_key_int(r->j, "total_superseded", c->superseded, err));
    TRY(atlas_json_key_int(r->j, "total_resolved", c->resolved, err));
    /* A9.1: the kind axis as one object, matching `decision.list` over IPC
     * exactly — same key, same member names, every kind present including the
     * zeroes, because a parser reading a fixed set of names is simpler than one
     * that has to cope with absent members. */
    TRY(atlas_json_key(r->j, "total_by_kind", err));
    TRY(atlas_json_obj_begin(r->j, err));
    for (size_t i = 0; i < atlas_decision_kind_count(); i++) {
        atlas_decision_kind k = atlas_decision_kind_at(i);
        TRY(atlas_json_key_int(r->j, atlas_decision_kind_name(k), c->by_kind[(size_t)k], err));
    }
    return atlas_json_obj_end(r->j, err);
}


static atlas_status j_decision_ledger(atlas_renderer *r, bool agrees, atlas_err *err) {
    return atlas_json_key_bool(r->j, "ledger_agrees", agrees, err);
}

/* --- A5 ------------------------------------------------------------------
 *
 * Paths were typed by the operator and are bytes, so they go through
 * `json_safe` like every other path. Digests, verdicts, class names and the
 * retention reasons are Atlas-owned fixed text. */

static atlas_status j_verify_body(atlas_json *j, atlas_safe_pool *p,
                                  const atlas_backup_verify_report *rep, atlas_err *err) {
    TRY(json_safe(j, p, "path", atlas_buf_cstr(&rep->path), err));
    TRY(atlas_json_key_str(j, "verdict", atlas_backup_verdict_name(rep->verdict), err));
    TRY(atlas_json_key_bool(j, "usable", rep->ok, err));
    TRY(atlas_json_key_int(j, "size_bytes", rep->size_bytes, err));
    TRY(atlas_json_key_str(j, "sha256", rep->sha256, err));
    TRY(atlas_json_key_int(j, "schema_version", rep->schema_version, err));
    TRY(atlas_json_key_int(j, "expected_schema_version", rep->expected_schema_version, err));
    TRY(json_safe(j, p, "integrity_check", atlas_buf_cstr(&rep->integrity), err));
    TRY(json_safe(j, p, "foreign_key_check", atlas_buf_cstr(&rep->foreign_key_check), err));
    TRY(atlas_json_key_int(j, "tables_required", rep->tables_required, err));
    TRY(atlas_json_key_int(j, "tables_present", rep->tables_present, err));
    TRY(json_safe(j, p, "missing_tables", atlas_buf_cstr(&rep->missing_tables), err));
    TRY(atlas_json_key_int(j, "repositories", rep->repo_count, err));
    TRY(atlas_json_key_int(j, "decision_documents_checked", rep->revisions_checked, err));
    TRY(atlas_json_key_int(j, "decision_revisions_rehashed", rep->revisions_rehashed, err));
    TRY(atlas_json_key_int(j, "decision_revisions_corrupt", rep->revisions_corrupt, err));
    TRY(atlas_json_key_int(j, "decision_ledger_mismatched", rep->ledger_mismatched, err));
    TRY(atlas_json_key(j, "problems", err));
    TRY(atlas_json_arr_begin(j, err));
    const char *text = atlas_buf_cstr(&rep->problems);
    while (*text != '\0') {
        const char *nl = strchr(text, '\n');
        size_t len = (nl != NULL) ? (size_t)(nl - text) : strlen(text);
        TRY(atlas_json_str(j, atlas_safe_n(p, text, len), err));
        if (nl == NULL) {
            break;
        }
        text = nl + 1;
    }
    return atlas_json_arr_end(j, err);
}

static atlas_status j_backup_created(atlas_renderer *r, const atlas_backup_report *rep,
                                     atlas_err *err) {
    atlas_json *j = r->j;
    atlas_safe_pool *p = &r->safe;
    TRY(atlas_json_key(j, "backup", err));
    TRY(atlas_json_obj_begin(j, err));
    TRY(json_safe(j, p, "path", atlas_buf_cstr(&rep->path), err));
    TRY(json_safe(j, p, "source_db_path", atlas_buf_cstr(&rep->source_db_path), err));
    TRY(atlas_json_key_bool(j, "source_online", rep->source_online, err));
    TRY(atlas_json_key_int(j, "size_bytes", rep->size_bytes, err));
    TRY(atlas_json_key_str(j, "sha256", rep->sha256, err));
    TRY(atlas_json_key_str(j, "atlas_version", rep->atlas_version, err));
    TRY(atlas_json_key_int(j, "schema_version", rep->schema_version, err));
    TRY(atlas_json_key_int(j, "page_size", rep->page_size, err));
    TRY(atlas_json_key_int(j, "page_count", rep->page_count, err));
    /* Stated as fields rather than left to a reader's assumption. */
    TRY(atlas_json_key_bool(j, "contains_configuration", false, err));
    TRY(atlas_json_key_bool(j, "encrypted", false, err));
    TRY(atlas_json_key_bool(j, "signed", false, err));
    return atlas_json_obj_end(j, err);
}

static atlas_status j_backup_verified(atlas_renderer *r, const atlas_backup_verify_report *rep,
                                      const char *key, atlas_err *err) {
    atlas_json *j = r->j;
    TRY(atlas_json_key(j, key != NULL ? key : "backup", err));
    TRY(atlas_json_obj_begin(j, err));
    TRY(j_verify_body(j, &r->safe, rep, err));
    return atlas_json_obj_end(j, err);
}

static atlas_status j_backup_restored(atlas_renderer *r, const atlas_backup_restore_report *rep,
                                      atlas_err *err) {
    atlas_json *j = r->j;
    atlas_safe_pool *p = &r->safe;
    TRY(atlas_json_key(j, "restore", err));
    TRY(atlas_json_obj_begin(j, err));
    TRY(json_safe(j, p, "data_dir", atlas_buf_cstr(&rep->data_dir), err));
    TRY(json_safe(j, p, "db_path", atlas_buf_cstr(&rep->db_path), err));
    TRY(atlas_json_key_bool(j, "published", rep->published, err));
    TRY(atlas_json_key_bool(j, "recovery_copy_made", rep->recovery_made, err));
    TRY(json_safe(j, p, "recovery_copy_path", atlas_buf_cstr(&rep->recovery_path), err));
    TRY(atlas_json_key_bool(j, "removed_stale_wal", rep->removed_wal, err));
    TRY(atlas_json_key_bool(j, "removed_stale_shm", rep->removed_shm, err));
    TRY(atlas_json_key_int(j, "schema_before", rep->schema_before, err));
    TRY(atlas_json_key_int(j, "schema_after", rep->schema_after, err));
    TRY(atlas_json_key_bool(j, "migrated", rep->migrated, err));
    TRY(atlas_json_key_bool(j, "restored_runtime_state", false, err));
    TRY(atlas_json_key(j, "source", err));
    TRY(atlas_json_obj_begin(j, err));
    TRY(j_verify_body(j, p, &rep->source, err));
    TRY(atlas_json_obj_end(j, err));
    TRY(atlas_json_key(j, "installed", err));
    TRY(atlas_json_obj_begin(j, err));
    TRY(j_verify_body(j, p, &rep->installed, err));
    TRY(atlas_json_obj_end(j, err));
    return atlas_json_obj_end(j, err);
}

static atlas_status j_maintenance(atlas_renderer *r, const atlas_maintenance_report *rep,
                                  atlas_err *err) {
    atlas_json *j = r->j;
    TRY(atlas_json_key(j, "maintenance", err));
    TRY(atlas_json_obj_begin(j, err));
    TRY(atlas_json_key_bool(j, "applied", rep->applied, err));
    TRY(atlas_json_key_int(j, "older_than_days", rep->older_than_days, err));
    TRY(atlas_json_key_int(j, "retain_per_repo", rep->retain_per_repo, err));
    TRY(atlas_json_key_str(j, "cutoff", rep->cutoff, err));
    TRY(atlas_json_key_int(j, "total_rows", rep->total_rows, err));
    TRY(atlas_json_key_int(j, "total_eligible", rep->total_eligible, err));
    TRY(atlas_json_key_int(j, "total_removed", rep->total_removed, err));
    TRY(atlas_json_key_int(j, "prunable_tables", (int64_t)rep->prunable_tables, err));
    TRY(atlas_json_key_int(j, "protected_tables", (int64_t)rep->protected_tables, err));
    TRY(atlas_json_key(j, "tables", err));
    TRY(atlas_json_arr_begin(j, err));
    for (size_t i = 0; i < rep->table_count; i++) {
        const atlas_maintenance_row *t = &rep->tables[i];
        TRY(atlas_json_obj_begin(j, err));
        TRY(atlas_json_key_str(j, "table", t->table, err));
        TRY(atlas_json_key_str(j, "retention_class", atlas_retention_class_name(t->cls), err));
        TRY(atlas_json_key_bool(j, "prunable", t->prunable, err));
        TRY(atlas_json_key_str(j, "reason", t->reason, err));
        TRY(atlas_json_key_bool(j, "present", t->counted, err));
        TRY(atlas_json_key_int(j, "rows_before", t->rows_before, err));
        TRY(atlas_json_key_int(j, "rows_eligible", t->rows_eligible, err));
        TRY(atlas_json_key_int(j, "rows_removed", t->rows_removed, err));
        TRY(atlas_json_key_int(j, "rows_after", t->rows_after, err));
        TRY(atlas_json_obj_end(j, err));
    }
    TRY(atlas_json_arr_end(j, err));
    return atlas_json_obj_end(j, err);
}


/* --- A9: remote credentials ------------------------------------------------ */

/* Carries the plaintext, once, for the same reason the human renderer does: an
 * operator scripting a bootstrap has to capture it somewhere, and a JSON form
 * that omitted it would push them to parse the human one. `shown_once` and
 * `recoverable` are emitted as fields rather than left to a reader's
 * assumption, exactly as the backup renderer states `encrypted` and `signed`. */
static atlas_status j_apikey_created(atlas_renderer *r, const atlas_apikey_created *c,
                                     atlas_err *err) {
    atlas_json *j = r->j;
    atlas_safe_pool *p = &r->safe;
    TRY(atlas_json_key(j, "api_key", err));
    TRY(atlas_json_obj_begin(j, err));
    TRY(atlas_json_key_str(j, "id", c->key_id, err));
    TRY(json_safe(j, p, "label", c->label, err));
    TRY(atlas_json_key(j, "scopes", err));
    TRY(atlas_json_arr_begin(j, err));
    {
        const char *s = c->scopes;
        while (*s != '\0') {
            const char *sp = strchr(s, ' ');
            size_t n = sp != NULL ? (size_t)(sp - s) : strlen(s);
            char one[64];
            if (n < sizeof one) {
                memcpy(one, s, n);
                one[n] = '\0';
                TRY(atlas_json_str(j, one, err));
            }
            if (sp == NULL) {
                break;
            }
            s = sp + 1;
        }
    }
    TRY(atlas_json_arr_end(j, err));
    TRY(atlas_json_key_str(j, "created_at", c->created_at, err));
    TRY(atlas_json_key_str(j, "rotated_from", c->rotated_from, err));
    TRY(atlas_json_key_bool(j, "previous_revoked", c->previous_revoked, err));
    TRY(atlas_json_key_str(j, "secret", c->token, err));
    TRY(atlas_json_key_bool(j, "shown_once", true, err));
    TRY(atlas_json_key_bool(j, "recoverable", false, err));
    /* A16, Decision 2: the same fact the human renderer states in prose,
     * always emitted rather than only on the `--no-scopes` path, so a reader
     * of the JSON form never has to infer it from an empty array. An empty
     * scope list is reachable only through `--no-scopes`. */
    const bool no_scopes = c->scopes[0] == '\0';
    TRY(atlas_json_key_bool(j, "no_scopes", no_scopes, err));
    if (no_scopes) {
        TRY(atlas_json_key_str(j, "no_scopes_note",
                               "this credential authorises nothing on its own; only a root-owned "
                               "line in /etc/atlas/gateway.conf can give it a scope: "
                               "remote_dispose_key gives decisions:dispose, a remote_submit_key "
                               "line gives jobs:submit, and the same key may never be named by both",
                               err));
        if (c->rotated_from[0] != '\0') {
            char reminder[192];
            (void)snprintf(reminder, sizeof reminder,
                          "the policy line remote_dispose_key must now name %s%s; until it "
                          "does, neither key can dispose",
                          ATLAS_APIKEY_ID_PREFIX, c->key_id);
            TRY(atlas_json_key_str(j, "rotate_dispose_reminder", reminder, err));
        }
    }
    return atlas_json_obj_end(j, err);
}

static atlas_status j_apikey_listed(atlas_renderer *r, const atlas_apikey_listing *l,
                                    atlas_err *err) {
    atlas_json *j = r->j;
    atlas_safe_pool *p = &r->safe;
    TRY(atlas_json_key(j, "api_keys", err));
    TRY(atlas_json_arr_begin(j, err));
    for (size_t i = 0; i < l->count; i++) {
        const atlas_apikey_record *k = &l->keys[i];
        TRY(atlas_json_obj_begin(j, err));
        TRY(atlas_json_key_str(j, "id", k->key_id, err));
        TRY(json_safe(j, p, "label", k->label, err));
        TRY(atlas_json_key_str(j, "status", atlas_apikey_status_name(k->status), err));
        TRY(atlas_json_key_str(j, "scopes", k->scopes, err));
        TRY(atlas_json_key_bool(j, "scopes_unreadable", k->scopes_unreadable, err));
        TRY(atlas_json_key_str(j, "created_at", k->created_at, err));
        TRY(atlas_json_key_str(j, "revoked_at", k->revoked_at, err));
        TRY(atlas_json_key_str(j, "last_used_at", k->last_used_at, err));
        TRY(atlas_json_key_str(j, "rotated_from", k->rotated_from, err));
        TRY(atlas_json_key_str(j, "rotated_to", k->rotated_to, err));
        TRY(atlas_json_obj_end(j, err));
    }
    TRY(atlas_json_arr_end(j, err));
    TRY(atlas_json_key_int(j, "count", (int64_t)l->count, err));
    /* There is no field here that could hold a secret, and saying so in the
     * document means a consumer never has to wonder whether one was omitted by
     * accident. */
    TRY(atlas_json_key_bool(j, "secrets_included", false, err));
    return ATLAS_OK;
}

static atlas_status j_apikey_revoked(atlas_renderer *r, const char *key_id, bool changed,
                                     atlas_err *err) {
    atlas_json *j = r->j;
    TRY(atlas_json_key(j, "revoked", err));
    TRY(atlas_json_obj_begin(j, err));
    TRY(atlas_json_key_str(j, "id", key_id, err));
    TRY(atlas_json_key_bool(j, "changed", changed, err));
    return atlas_json_obj_end(j, err);
}

/* --- A9.2: verification ------------------------------------------------------
 *
 * `confidence_score` is the key name, not `confidence`, so that no consumer can
 * read it as a probability by accident. `calibrated_probability` is emitted
 * **only** when calibration supports one — absent rather than null or zero,
 * because a null invites a default and a zero is a probability of nought. */
/* A9.2.1. What one intake operation recorded.
 *
 * `duplicate` is always present, never omitted when false: it is the field that
 * says a resubmission did not become a second corroboration, and a reader of an
 * audit trail needs it stated rather than inferred from its absence. */
static atlas_status j_verify_intake(atlas_renderer *r, const char *verb,
                                    const atlas_verify_intake_result *res, atlas_err *err) {
    atlas_json *j = r->j;
    TRY(atlas_json_key(j, "verification", err));
    TRY(atlas_json_obj_begin(j, err));
    TRY(atlas_json_key_str(j, "operation", verb, err));
    TRY(atlas_json_key_str(j, "uid", atlas_buf_cstr(&res->uid), err));
    if (res->actor_uid.len > 0) {
        TRY(atlas_json_key_str(j, "actor", atlas_buf_cstr(&res->actor_uid), err));
    }
    if (res->claim_id > 0) {
        TRY(atlas_json_key_int(j, "claim_id", res->claim_id, err));
    }
    if (res->evidence_id > 0) {
        TRY(atlas_json_key_int(j, "evidence_id", res->evidence_id, err));
    }
    if (res->attestation_id > 0) {
        TRY(atlas_json_key_int(j, "attestation_id", res->attestation_id, err));
    }
    TRY(atlas_json_key_bool(j, "duplicate", res->duplicate, err));
    if (strcmp(verb, "produce") == 0) {
        /* Atlas' own vocabulary and Atlas' own sentence; neither carries a
         * repository byte, so neither is encoded. */
        TRY(atlas_json_key_str(j, "check", atlas_verify_check_name(res->check), err));
        if (res->verified_scope[0] != '\0') {
            TRY(atlas_json_key_str(j, "verified_scope", res->verified_scope, err));
        }
        if (res->detail[0] != '\0') {
            TRY(atlas_json_key_str(j, "detail", res->detail, err));
        }
        /* A9.2.2. `check` says whether the verifier's truth condition was met;
         * `truth` says whether the subject is there. They are different
         * questions and PASS means opposite things for `atlas.symbol_present`
         * and `atlas.symbol_absent`, so a consumer given only the first would
         * have to invert by hand. */
        TRY(atlas_json_key_str(j, "truth", atlas_verify_truth_name(res->truth), err));
        TRY(atlas_json_key_str(j, "truth_reason",
                               atlas_verify_truth_reason_name(res->truth_reason), err));
        TRY(atlas_json_key_str(j, "truth_detail",
                               atlas_verify_truth_reason_description(res->truth_reason), err));
        TRY(atlas_json_key_str(
            j, "coverage",
            atlas_verify_coverage_name(
                atlas_verify_coverage_summary(&res->coverage, ATLAS_VERIFIER_NONE)),
            err));
        {
            char cov[512];
            (void)atlas_verify_coverage_render(&res->coverage, cov, sizeof cov);
            TRY(atlas_json_key_str(j, "coverage_detail", cov, err));
        }
    }
    if (strcmp(verb, "evaluate") == 0) {
        const atlas_verify_assessment *a = &res->assessment;
        /* The three axes, in three separate fields. Never one badge. */
        TRY(atlas_json_key_str(j, "kind", atlas_decision_kind_name(a->kind), err));
        TRY(atlas_json_key_str(j, "status", atlas_decision_state_name(a->from), err));
        TRY(atlas_json_key_str(j, "state", atlas_verify_state_name(a->aggregate.state), err));
        TRY(atlas_json_key_str(j, "basis", atlas_verify_basis_name(a->basis), err));
        TRY(atlas_json_key_int(j, "confidence_score", a->aggregate.confidence, err));
        TRY(atlas_json_key_str(j, "calibration",
                               atlas_verify_calibration_name(a->aggregate.calibration), err));
        /* Emitted only when calibration supports it. Absent, never null and
         * never zero: zero is a probability. */
        if (a->aggregate.calibrated_probability >= 0) {
            TRY(atlas_json_key_int(j, "calibrated_probability",
                                   a->aggregate.calibrated_probability, err));
        }
        TRY(atlas_json_key_int(j, "independent_groups", a->aggregate.independent_groups, err));
        TRY(atlas_json_key_str(j, "policy_verdict",
                               atlas_verify_policy_verdict_name(a->aggregate.verdict), err));
        TRY(atlas_json_key_bool(j, "source_drift", a->source_drift, err));
        TRY(atlas_json_key_bool(j, "actionable", a->actionable, err));
        TRY(atlas_json_key_bool(j, "transitioned", a->transitioned, err));
        if (a->transitioned) {
            TRY(atlas_json_key_str(j, "actor_of_record", "VERIFICATION_POLICY", err));
        }
    }
    return atlas_json_obj_end(j, err);
}

static atlas_status j_verify(atlas_renderer *r, const atlas_verify_report *rep, atlas_err *err) {
    atlas_json *j = r->j;
    const atlas_verify_assessment *a = &rep->assessment;
    TRY(atlas_json_key(j, "verification", err));
    TRY(atlas_json_obj_begin(j, err));
    TRY(atlas_json_key_str(j, "claim", atlas_buf_cstr(&rep->claim_uid), err));
    /* UNTRUSTED_DATA: written by whoever submitted the claim. Encoded here for
     * the same reason `atlas_service_verify_write_report` encodes it at
     * service_verify.c:751 for the daemon's socket reply — this function is
     * the CLI's own, separate JSON assembly for `atlas verify show --json` run
     * without a daemon, and it must not answer differently. */
    TRY(atlas_json_key_str(j, "claim_text", atlas_safe(&r->safe, atlas_buf_cstr(&rep->claim_text)),
                           err));
    /* Labelled, exactly as A4's decision documents are. Verification changes a
     * status; it does not make prose trustworthy. */
    TRY(atlas_json_key_str(j, "trust", "UNTRUSTED_DATA", err));
    TRY(atlas_json_key_str(j, "domain", atlas_buf_cstr(&rep->domain), err));
    TRY(atlas_json_key_str(j, "record", atlas_buf_cstr(&rep->record_uid), err));

    /* A9.2.2: the four axes, in four separate fields. Never one badge.
     *
     * These are the same key names `atlas_service_verify_write_assessment`
     * sends over the socket, so one consumer reads both surfaces — the CLI's
     * `--json` and the daemon's reply must not describe the same claim with
     * different keys. */
    TRY(atlas_json_key_str(j, "kind", atlas_decision_kind_name(a->kind), err));
    TRY(atlas_json_key_str(j, "status", atlas_decision_state_name(a->from), err));
    TRY(atlas_json_key_str(j, "state", atlas_verify_state_name(a->aggregate.state), err));
    TRY(atlas_json_key_str(j, "truth", atlas_verify_truth_name(a->truth), err));
    TRY(atlas_json_key_str(j, "truth_reason", atlas_verify_truth_reason_name(a->truth_reason),
                           err));
    TRY(atlas_json_key_str(j, "truth_detail",
                           atlas_verify_truth_reason_description(a->truth_reason), err));
    TRY(atlas_json_key_str(
        j, "coverage",
        atlas_verify_coverage_name(atlas_verify_coverage_summary(&a->coverage, a->verifier)),
        err));
    /* Every dimension by name, including the UNKNOWN ones: a reader asking why
     * an answer is UNKNOWN needs to see which part of the looking fell short,
     * and a map listing only what succeeded answers the opposite question. */
    TRY(atlas_json_key(j, "coverage_dimensions", err));
    TRY(atlas_json_obj_begin(j, err));
    for (size_t cd = 0; cd < ATLAS_VERIFY_COVERAGE_DIMS; cd++) {
        atlas_verify_coverage_dim d = (atlas_verify_coverage_dim)cd;
        TRY(atlas_json_key_str(j, atlas_verify_coverage_dim_name(d),
                               atlas_verify_coverage_name(a->coverage.dims[cd]), err));
    }
    TRY(atlas_json_obj_end(j, err));

    TRY(atlas_json_key_str(j, "basis", atlas_verify_basis_name(a->basis), err));
    TRY(atlas_json_key_str(j, "semantics", atlas_verify_claim_semantics_name(a->semantics), err));
    TRY(atlas_json_key_int(j, "confidence_score", a->aggregate.confidence, err));
    TRY(atlas_json_key_str(j, "calibration",
                           atlas_verify_calibration_name(a->aggregate.calibration), err));
    if (a->aggregate.calibration == ATLAS_CALIBRATION_CALIBRATED &&
        a->aggregate.calibrated_probability >= 0) {
        TRY(atlas_json_key_int(j, "calibrated_probability", a->aggregate.calibrated_probability,
                               err));
    }
    TRY(atlas_json_key_str(j, "algorithm", a->aggregate.algorithm, err));
    TRY(atlas_json_key_int(j, "prior_version", ATLAS_VERIFY_PRIOR_VERSION, err));
    TRY(atlas_json_key_int(j, "family_version", a->aggregate.family_version, err));

    TRY(atlas_json_key_str(j, "verifier", atlas_verify_verifier_name(a->verifier), err));
    TRY(atlas_json_key_str(j, "check", atlas_verify_check_name(a->check), err));
    TRY(atlas_json_key_str_opt(j, "verified_scope", a->verified_scope, err));
    TRY(atlas_json_key_str_opt(j, "detail", a->detail, err));

    /* A9.2.1: what the result is *of*. The daemon's `verify.show` has carried
     * these since migration 16 and this renderer did not, so `atlas verify show
     * --json` and the socket answered differently about whether a result
     * describes the tree the repository is on — which is the one question
     * SOURCE_DRIFT exists to answer, and the worst one to disagree about. */
    TRY(atlas_json_key_str(j, "claim_commit", a->claim_commit, err));
    TRY(atlas_json_key_str(j, "evaluated_commit", a->evaluated_commit, err));
    TRY(atlas_json_key_int(j, "sem_generation", a->sem_generation, err));
    TRY(atlas_json_key_bool(j, "source_drift", a->source_drift, err));

    TRY(atlas_json_key_int(j, "support_count", a->aggregate.support_count, err));
    TRY(atlas_json_key_int(j, "contradict_count", a->aggregate.contradict_count, err));
    TRY(atlas_json_key_int(j, "inconclusive_count", a->aggregate.inconclusive_count, err));
    TRY(atlas_json_key_int(j, "independent_groups", a->aggregate.independent_groups, err));
    TRY(atlas_json_key_int(j, "independent_families", a->aggregate.independent_families, err));
    TRY(atlas_json_key_int(j, "support_mass", a->aggregate.support_mass, err));
    TRY(atlas_json_key_int(j, "contradict_mass", a->aggregate.contradict_mass, err));
    TRY(atlas_json_key_int(j, "attestation_total", (int64_t)a->attestation_total, err));
    TRY(atlas_json_key_bool(j, "truncated", a->truncated, err));
    TRY(atlas_json_key_bool(j, "stale", a->aggregate.stale, err));
    TRY(atlas_json_key_str(j, "conflict", atlas_verify_conflict_name(a->aggregate.conflict), err));

    TRY(atlas_json_key_str(j, "policy_state", atlas_verifypolicy_state_name(rep->policy_state),
                           err));
    TRY(atlas_json_key_str(j, "policy_reason", atlas_verifypolicy_reason_name(rep->policy_reason),
                           err));
    TRY(atlas_json_key_str_opt(j, "policy_id", rep->policy_id, err));
    TRY(atlas_json_key_str_opt(j, "policy_hash", rep->policy_hash, err));
    TRY(atlas_json_key_str_opt(j, "policy_path", rep->policy_path, err));
    TRY(atlas_json_key_bool(j, "deterministic_enforce", rep->deterministic_enforce, err));
    TRY(atlas_json_key_bool(j, "empirical_enforce", rep->empirical_enforce, err));
    TRY(atlas_json_key_int(j, "policy_rules", (int64_t)rep->rule_count, err));

    TRY(atlas_json_key_str(j, "policy_verdict",
                           atlas_verify_policy_verdict_name(a->aggregate.verdict), err));
    TRY(atlas_json_key(j, "reasons", err));
    TRY(atlas_json_arr_begin(j, err));
    for (size_t i = 0; i < a->aggregate.reason_count; i++) {
        TRY(atlas_json_obj_begin(j, err));
        TRY(atlas_json_key_str(j, "reason", atlas_verify_reason_name(a->aggregate.reasons[i]),
                               err));
        TRY(atlas_json_key_str(j, "meaning",
                               atlas_verify_reason_description(a->aggregate.reasons[i]), err));
        TRY(atlas_json_obj_end(j, err));
    }
    TRY(atlas_json_arr_end(j, err));
    /* The true count, even when fewer were kept. */
    TRY(atlas_json_key_int(j, "reason_total", (int64_t)a->aggregate.reason_total, err));

    TRY(atlas_json_key_bool(j, "transitioned", a->transitioned, err));
    if (a->transitioned) {
        TRY(atlas_json_key_str(j, "transition_from", atlas_decision_state_name(a->from), err));
        TRY(atlas_json_key_str(j, "transition_to", atlas_decision_state_name(a->to), err));
        TRY(atlas_json_key_str(j, "actor", "VERIFICATION_POLICY", err));
    }
    TRY(atlas_json_key_int(j, "result_id", a->result_id, err));
    TRY(atlas_json_key_int(j, "audit_id", a->audit_id, err));
    /* The evidence behind the number, from the one writer every surface uses.
     * A score whose evidence a reader cannot see is a score taken on trust. */
    TRY(atlas_service_verify_write_detail(j, &r->safe, &rep->detail, err));
    return atlas_json_obj_end(j, err);
}

static atlas_status j_gate(atlas_renderer *r, const atlas_gate_report *rep, atlas_err *err) {
    atlas_json *j = r->j;
    TRY(atlas_json_key(j, "gate", err));
    TRY(atlas_json_obj_begin(j, err));
    TRY(atlas_json_key_str(j, "result", atlas_gate_result_name(rep->result), err));
    TRY(atlas_json_key_int(j, "exit_code", atlas_gate_exit_code(rep->result), err));
    TRY(atlas_json_key_str(j, "repository", atlas_buf_cstr(&rep->repo_name), err));
    TRY(atlas_json_key_str(j, "root", atlas_buf_cstr(&rep->root_text), err));
    TRY(atlas_json_key_str(j, "repo_identity_hash", atlas_buf_cstr(&rep->repo_identity_hash), err));
    TRY(atlas_json_key_str(j, "indexed_commit", rep->indexed_commit, err));
    TRY(atlas_json_key_str(j, "requested_commit", rep->requested_commit, err));
    TRY(atlas_json_key_int(j, "depth", rep->depth, err));
    TRY(atlas_json_key_int(j, "fresh", rep->fresh, err));
    TRY(atlas_json_key_int(j, "stale", rep->stale, err));
    TRY(atlas_json_key_int(j, "impacted", rep->impacted, err));
    TRY(atlas_json_key_int(j, "unknown", rep->unknown, err));
    TRY(atlas_json_key_int(j, "out_of_scope", rep->out_of_scope, err));
    TRY(atlas_json_key_bool(j, "limit_reached", rep->limit_reached, err));
    TRY(atlas_json_key_str_opt(j, "limit_detail", rep->limit_detail, err));
    TRY(atlas_json_key(j, "decisions", err));
    TRY(atlas_json_arr_begin(j, err));
    for (size_t i = 0; i < rep->item_count; i++) {
        const atlas_gate_assessment *a = &rep->items[i];
        TRY(atlas_json_obj_begin(j, err));
        TRY(atlas_json_key_str(j, "decision", atlas_buf_cstr(&a->uid), err));
        TRY(atlas_json_key_int(j, "revision", a->revision_no, err));
        /* Already safe-encoded by the service layer; not encoded again. */
        TRY(atlas_json_key_str(j, "title", atlas_buf_cstr(&a->title), err));
        TRY(atlas_json_key_str(j, "status", atlas_decision_state_name(a->state), err));
        TRY(atlas_json_key_str(j, "scope", atlas_decision_scope_name(a->scope), err));
        TRY(atlas_json_key_str(j, "content_hash", a->content_hash, err));
        TRY(atlas_json_key_str(j, "evidence_digest", a->evidence_digest, err));
        TRY(atlas_json_key_str(j, "freshness", atlas_gate_freshness_name(a->freshness), err));
        TRY(atlas_json_key_str(j, "validated_at_commit", a->validated_at_commit, err));
        TRY(atlas_json_key_bool(j, "validated_by_revalidation", a->validated_by_revalidation, err));
        TRY(atlas_json_key_int(j, "revalidations", a->revalidation_count, err));
        TRY(atlas_json_key_str(j, "indexed_commit", a->indexed_commit, err));
        TRY(atlas_json_key_str(j, "requested_commit", a->requested_commit, err));
        TRY(atlas_json_key(j, "reasons", err));
        TRY(atlas_json_arr_begin(j, err));
        for (size_t k = 0; k < a->reason_count; k++) {
            TRY(atlas_json_str(j, atlas_gate_reason_name(a->reasons[k]), err));
        }
        TRY(atlas_json_arr_end(j, err));
        TRY(atlas_json_key(j, "evidence", err));
        TRY(atlas_json_obj_begin(j, err));
        TRY(atlas_json_key_int(j, "links_total", a->links_total, err));
        TRY(atlas_json_key_int(j, "links_current", a->links_current, err));
        TRY(atlas_json_key_int(j, "links_changed", a->links_changed, err));
        TRY(atlas_json_key_int(j, "links_missing", a->links_missing, err));
        TRY(atlas_json_key_int(j, "links_ambiguous", a->links_ambiguous, err));
        TRY(atlas_json_key_int(j, "links_unknown", a->links_unknown, err));
        TRY(atlas_json_key_int(j, "range_commits", a->range_commits, err));
        TRY(atlas_json_key_int(j, "range_paths", a->range_paths, err));
        TRY(atlas_json_key_int(j, "walk_visited", a->walk_visited, err));
        TRY(atlas_json_key_int(j, "walk_matched", a->walk_matched, err));
        TRY(atlas_json_obj_end(j, err));
        TRY(atlas_json_key_bool(j, "limit_reached", a->limit_reached, err));
        TRY(atlas_json_key_str_opt(j, "limit_detail", a->limit_detail, err));
        TRY(atlas_json_obj_end(j, err));
    }
    TRY(atlas_json_arr_end(j, err));
    return atlas_json_obj_end(j, err);
}


/* --- A8-CI: the compiler-derived semantic index --------------------------
 *
 * Every field the human renderer prints appears here, under a stable key, and
 * every evidence class travels with its fact. A JSON consumer that could read
 * the rows without the class would be able to treat a candidate as a
 * resolution, which is the one thing this layer must never allow.
 *
 * Symbol names, file paths and type spellings are repository-derived and
 * therefore untrusted, so they go through the safe encoder at the point of
 * output. Vocabulary values (kinds, linkages, evidence classes, freshness,
 * truncation reasons) are Atlas' own literals and are emitted as-is. */

static atlas_status j_sem_generation(atlas_renderer *r, const atlas_sem_generation *g,
                                     atlas_err *err) {
    TRY(atlas_json_key(r->j, "generation", err));
    TRY(atlas_json_obj_begin(r->j, err));
    TRY(atlas_json_key_int(r->j, "id", g->id, err));
    TRY(atlas_json_key_str(r->j, "status", atlas_sem_gen_status_name(g->status), err));
    TRY(atlas_json_key_str_opt(r->j, "commit", g->commit_id, err));
    TRY(atlas_json_key_str_opt(r->j, "compdb_digest", g->compdb_digest, err));
    TRY(atlas_json_key_int(r->j, "compdb_count", g->compdb_count, err));
    TRY(atlas_json_key_str_opt(r->j, "compiler_id", g->compiler_id, err));
    TRY(atlas_json_key_str_opt(r->j, "compiler_version", g->compiler_version, err));
    TRY(atlas_json_key_str_opt(r->j, "analyzer_id", g->analyzer_id, err));
    TRY(atlas_json_key_int(r->j, "analyzer_version", g->analyzer_version, err));
    TRY(atlas_json_key_str_opt(r->j, "started_at", g->started_at, err));
    TRY(atlas_json_key_str_opt(r->j, "completed_at", g->completed_at, err));
    TRY(atlas_json_key_int(r->j, "tu_total", g->tu_total, err));
    TRY(atlas_json_key_int(r->j, "tu_complete", g->tu_complete, err));
    TRY(atlas_json_key_int(r->j, "tu_partial", g->tu_partial, err));
    TRY(atlas_json_key_int(r->j, "tu_failed", g->tu_failed, err));
    TRY(atlas_json_key_int(r->j, "tu_unsupported", g->tu_unsupported, err));
    TRY(atlas_json_key_int(r->j, "symbols", g->symbol_count, err));
    TRY(atlas_json_key_int(r->j, "edges", g->edge_count, err));
    TRY(atlas_json_key_int(r->j, "includes", g->include_count, err));
    TRY(atlas_json_key_int(r->j, "duration_ms", g->duration_ms, err));
    TRY(atlas_json_key_str_opt(r->j, "failure_reason", g->failure_reason, err));
    /* A9.2.3's coverage manifest, beside the unit counts and never summed with
     * them: the first pair says what the compilation database named, the second
     * says how much of the repository that was. */
    TRY(atlas_json_key_str(r->j, "scope_discovery",
                           atlas_sem_scope_discovery_name(g->scope_discovery), err));
    TRY(atlas_json_key_int(r->j, "scope_candidates", g->scope_candidates, err));
    TRY(atlas_json_key_int(r->j, "scope_covered", g->scope_covered, err));
    TRY(atlas_json_key_int(r->j, "scope_uncovered", g->scope_uncovered, err));
    /* A9.2.4. What the generation was sealed under, beside what it covered.
     * Exactly the keys `server_sem.c` writes, so a client cannot tell which
     * surface produced the document. */
    TRY(atlas_json_key_str(r->j, "discovery", atlas_sem_discovery_name(g->discovery), err));
    TRY(atlas_json_key_int(r->j, "input_count", g->input_count, err));
    TRY(atlas_json_key_int(r->j, "scope_excluded", g->scope_excluded, err));
    TRY(atlas_json_key_bool(r->j, "test_scope_known", g->test_scope_known, err));
    TRY(atlas_json_key_int(r->j, "tu_test", g->tu_test, err));
    TRY(atlas_json_key_int(r->j, "tu_production", g->tu_production, err));
    TRY(atlas_json_key_str_opt(r->j, "source_identity", g->source_identity, err));
    return atlas_json_obj_end(r->j, err);
}

/* A9.2.3. The derived state, in **exactly the shape the daemon sends it**.
 *
 * Flat keys and the same names as `write_sem_plan_fields` in `server_sem.c`,
 * deliberately: this renderer serves the local path and the daemon serves MCP,
 * the gateway and Mission Control, and a client that had to know which produced
 * a document would be a client that eventually gets it wrong. The first cut
 * nested these under `semantic_state` here and left them flat on the wire —
 * one answer in two shapes, which is the drift every one of these seasons has
 * had to fix once.
 *
 * `freshness` and `stale_reason` are not written here because `j_sem_status`
 * has already written them: freshness comes first on every semantic read, which
 * is A8-CI's rule, and a document must not carry a key twice. */
static atlas_status j_sem_plan(atlas_renderer *r, const atlas_sem_plan *p, atlas_err *err) {
    TRY(atlas_json_key_str(r->j, "activity", atlas_sem_activity_name(p->activity), err));
    TRY(atlas_json_key_str_opt(
        r->j, "hold_reason",
        atlas_sem_hold_reason_is_known(p->hold_reason) ? p->hold_reason : NULL, err));
    /* A9.2.5. Which dimension, and whether waiting will fix it. Checked against
     * Atlas' closed set before it is emitted. */
    TRY(atlas_json_key_str_opt(
        r->j, "coverage_gap",
        atlas_sem_unknown_reason_is_known(p->coverage_gap) ? p->coverage_gap : NULL, err));
    TRY(atlas_json_key_bool(r->j, "operator_action_required", p->operator_action_required, err));
    TRY(atlas_json_key_bool(r->j, "configured", p->configured, err));
    TRY(atlas_json_key_bool(r->j, "auto_rebuild", p->auto_rebuild, err));
    TRY(atlas_json_key_bool(r->j, "rebuild_due", p->should_build, err));
    TRY(atlas_json_key_int(r->j, "fail_count", p->fail_count, err));
    TRY(atlas_json_key_str_opt(r->j, "fail_reason",
                               atlas_sem_why_is_known(p->fail_reason) ? p->fail_reason : NULL,
                               err));
    TRY(atlas_json_key_str_opt(r->j, "fail_at", p->fail_at, err));
    TRY(atlas_json_key_str_opt(r->j, "source_identity", p->source_identity, err));

    /* --- A9.2.4 ---
     *
     * The activation intent and its provenance beside the effective boolean, and
     * discovery as its own axis. Exactly the keys `server_sem.c` writes, in the
     * same order: one shape on every surface is what stops a client having to
     * know whether the CLI or the daemon produced the document. */
    TRY(atlas_json_key_str(r->j, "auto_intent", atlas_sem_auto_intent_name(p->auto_intent), err));
    TRY(atlas_json_key_str(r->j, "auto_intent_by",
                           atlas_sem_intent_source_name(p->auto_intent_by), err));
    TRY(atlas_json_key_bool(r->j, "policy_default", p->policy_default, err));
    TRY(atlas_json_key_str(r->j, "discovery_mode",
                           atlas_sem_discovery_mode_name(p->discovery_mode), err));
    TRY(atlas_json_key_str_opt(r->j, "discovered_at", p->discovered_at, err));
    return atlas_json_key_str_opt(
        r->j, "discovery_limit",
        p->discovery_limit[0] != '\0' ? atlas_safe(&r->safe, p->discovery_limit) : NULL, err);
}

/* A9.2.4. Every candidate, accepted and rejected, with the reason for each.
 *
 * The rejected ones are the point: a candidate nobody is shown is
 * indistinguishable from one that does not exist, and that indistinguishability
 * is what let a third compilation database go unnoticed for a season. */
static atlas_status j_sem_inputs(atlas_renderer *r, const atlas_sem_status_report *rep,
                                 atlas_err *err) {
    TRY(atlas_json_key(r->j, "build_inputs", err));
    TRY(atlas_json_arr_begin(r->j, err));
    for (size_t i = 0; i < rep->input_count; i++) {
        const struct atlas_sem_input *in = &rep->inputs[i];
        TRY(atlas_json_obj_begin(r->j, err));
        TRY(json_safe(r->j, &r->safe, "path", in->path, err));
        TRY(atlas_json_key_str(r->j, "origin", atlas_sem_input_origin_name(in->origin), err));
        TRY(atlas_json_key_bool(r->j, "accepted", in->accepted, err));
        TRY(atlas_json_key_str_opt(
            r->j, "reject_reason",
            atlas_sem_reject_reason_is_known(in->reject_reason) ? in->reject_reason : NULL, err));
        TRY(atlas_json_key_str_opt(r->j, "digest", in->digest, err));
        TRY(atlas_json_key_int(r->j, "units", in->unit_count, err));
        TRY(atlas_json_obj_end(r->j, err));
    }
    TRY(atlas_json_arr_end(r->j, err));

    /* A9.2.5. Where the walk could not look — the same argument as the rejected
     * candidates above, applied to directories. Always emitted, so an empty
     * array is a positive statement rather than a missing key. */
    TRY(atlas_json_key(r->j, "discovery_obstacles", err));
    TRY(atlas_json_arr_begin(r->j, err));
    for (size_t i = 0; i < rep->obstacle_count; i++) {
        const struct atlas_sem_obstacle *ob = &rep->obstacles[i];
        TRY(atlas_json_obj_begin(r->j, err));
        TRY(json_safe(r->j, &r->safe, "path", ob->path, err));
        TRY(atlas_json_key_str_opt(
            r->j, "reason",
            atlas_sem_obstacle_reason_is_known(ob->reason) ? ob->reason : NULL, err));
        TRY(atlas_json_obj_end(r->j, err));
    }
    TRY(atlas_json_arr_end(r->j, err));
    return atlas_json_key_bool(r->j, "discovery_obstacles_truncated", rep->obstacles_truncated,
                               err);
}

/* The declared lists, each element its own string rather than the newline-joined
 * storage form: a client must not have to parse a separator to learn what an
 * operator configured. */
static atlas_status j_sem_path_list(atlas_renderer *r, const char *key, const atlas_buf *packed,
                                    atlas_err *err) {
    TRY(atlas_json_key(r->j, key, err));
    TRY(atlas_json_arr_begin(r->j, err));
    const char *data = packed->len > 0 ? (const char *)packed->data : "";
    size_t start = 0;
    for (size_t i = 0; i <= packed->len; i++) {
        if (i != packed->len && data[i] != '\n') {
            continue;
        }
        size_t n = i - start;
        char one[ATLAS_SEM_CONFIG_MAX_BYTES];
        if (n > 0 && n < sizeof one) {
            memcpy(one, data + start, n);
            one[n] = '\0';
            TRY(atlas_json_str(r->j, atlas_safe(&r->safe, one), err));
        }
        start = i + 1u;
    }
    return atlas_json_arr_end(r->j, err);
}

static atlas_status j_sem_config(atlas_renderer *r, const atlas_sem_status_report *rep,
                                 atlas_err *err) {
    TRY(atlas_json_key_str(r->j, "repo", rep->repo.name, err));
    /* Freshness first, as on every semantic read, and then the fields
     * `j_sem_plan` does not repeat — the same order `code.sem_config` uses on
     * the wire. */
    TRY(atlas_json_key_str(r->j, "freshness", atlas_sem_freshness_name(rep->plan.freshness), err));
    TRY(atlas_json_key_str_opt(
        r->j, "stale_reason",
        atlas_sem_stale_reason_is_known(rep->plan.stale_reason) ? rep->plan.stale_reason : NULL,
        err));
    TRY(j_sem_plan(r, &rep->plan, err));
    TRY(j_sem_inputs(r, rep, err));
    TRY(j_sem_path_list(r, "compdbs", &rep->compdbs, err));
    TRY(j_sem_path_list(r, "test_roots", &rep->test_roots, err));
    TRY(j_sem_path_list(r, "excludes", &rep->excludes, err));
    TRY(j_sem_path_list(r, "vendor_roots", &rep->vendor_roots, err));
    TRY(atlas_json_key_bool(r->j, "have_generation", rep->have_generation, err));
    if (rep->have_generation) {
        TRY(j_sem_generation(r, &rep->generation, err));
    }
    TRY(ATLAS_OK);
    /* A9.2.5. `sem-config` reports the same repository state `sem-status` does,
     * so it carries the same trust block — and must, since the block is where
     * the coverage and discovery fields now live. */
    return atlas_sem_trust_write_json(r->j, &rep->trust, err);
}

static atlas_status j_sem_status(atlas_renderer *r, const atlas_sem_status_report *rep,
                                 atlas_err *err) {
    TRY(atlas_json_key_str(r->j, "repo", rep->repo.name, err));
    /* A9.2.5. `libclang_available`, `freshness`, `stale_reason` and
     * `have_generation` are **not** written here: the trust block at the end of
     * this document carries them, and `j_sem_plan`'s own rule is that a document
     * must not carry a key twice. Atlas' own remote parser reads the *first*
     * occurrence (`yyjson_obj_get` is a linear scan) while `jq` and Python read
     * the last, so a duplicated key is not merely untidy — it is two consumers
     * disagreeing about one document. */
    TRY(atlas_json_key_str_opt(r->j, "compiler_id", rep->compiler_id, err));
    TRY(atlas_json_key_str_opt(r->j, "compiler_version", rep->compiler_version, err));
    if (rep->have_generation) {
        TRY(j_sem_generation(r, &rep->generation, err));
    }
    /* A9.2.3. The derived state and the build description, on the same document
     * as the generation, so one read answers "is this evidence trustworthy". */
    TRY(j_sem_plan(r, &rep->plan, err));
    TRY(j_sem_inputs(r, rep, err));
    TRY(j_sem_path_list(r, "compdbs", &rep->compdbs, err));
    TRY(j_sem_path_list(r, "test_roots", &rep->test_roots, err));
    TRY(j_sem_path_list(r, "excludes", &rep->excludes, err));
    TRY(j_sem_path_list(r, "vendor_roots", &rep->vendor_roots, err));
    if (rep->have_latest) {
        TRY(atlas_json_key(r->j, "latest_attempt", err));
        TRY(atlas_json_obj_begin(r->j, err));
        TRY(atlas_json_key_int(r->j, "id", rep->latest.id, err));
        TRY(atlas_json_key_str(r->j, "status", atlas_sem_gen_status_name(rep->latest.status),
                               err));
        TRY(atlas_json_key_str_opt(r->j, "failure_reason", rep->latest.failure_reason, err));
        TRY(atlas_json_obj_end(r->j, err));
    }

    TRY(atlas_json_key_int(r->j, "units_not_complete", rep->failed_total, err));
    TRY(atlas_json_key_bool(r->j, "units_truncated", rep->failed_truncated, err));
    TRY(atlas_json_key(r->j, "units", err));
    TRY(atlas_json_arr_begin(r->j, err));
    for (size_t i = 0; i < rep->failed_count; i++) {
        const atlas_sem_failed_unit *u = &rep->failed[i];
        TRY(atlas_json_obj_begin(r->j, err));
        TRY(json_safe(r->j, &r->safe, "source", u->source, err));
        TRY(atlas_json_key_str(r->j, "status", u->status, err));
        TRY(atlas_json_key_str_opt(r->j, "why", atlas_sem_why_is_known(u->why) ? u->why : NULL,
                                   err));
        TRY(atlas_json_key_int(r->j, "diagnostics_errors", u->diagnostics_errors, err));
        TRY(atlas_json_obj_end(r->j, err));
    }
    TRY(atlas_json_arr_end(r->j, err));
    /* A9.2.5. `sem-status` carries the same block as every query answer, so one
     * repository is described in one vocabulary wherever it is asked about. */
    return atlas_sem_trust_write_json(r->j, &rep->trust, err);
}

/* A9.2.5. The trust block, from the one writer `src/ipc/server_sem.c` also
 * calls. Every key this renderer used to write by hand — `freshness`,
 * `stale_reason`, `generation_id`, `indexed_commit` — is still emitted, by that
 * writer, so nothing public was removed; what changed is that there is one
 * implementation instead of two that had already drifted apart over
 * `have_generation`.
 *
 * It is written **after** the results rather than before them, and that is
 * forced rather than chosen: the verdict is a statement about the result set, so
 * it cannot precede it on a streaming writer. Key order is not a JSON contract
 * and the guarantee that mattered — that no answer can be read without its
 * currency — is unchanged, because the block is in the same document. */
static atlas_status j_sem_trust(atlas_renderer *r, const atlas_sem_trust *t, atlas_err *err) {
    return atlas_sem_trust_write_json(r->j, t, err);
}

static atlas_status j_sem_symbols(atlas_renderer *r, const atlas_sem_symbols_report *rep,
                                  atlas_err *err) {
    TRY(atlas_json_key_str(r->j, "repo", rep->repo.name, err));
    TRY(json_safe(r->j, &r->safe, "query", rep->query, err));
    TRY(atlas_json_key_bool(r->j, "truncated", rep->truncated, err));
    TRY(atlas_json_key(r->j, "symbols", err));
    TRY(atlas_json_arr_begin(r->j, err));
    for (size_t i = 0; i < rep->count; i++) {
        const atlas_sem_symbol_item *s = &rep->items[i];
        TRY(atlas_json_obj_begin(r->j, err));
        TRY(json_safe(r->j, &r->safe, "usr", s->usr, err));
        TRY(json_safe(r->j, &r->safe, "name", s->name, err));
        TRY(atlas_json_key_str(r->j, "kind", s->kind, err));
        TRY(atlas_json_key_str(r->j, "linkage", s->linkage, err));
        TRY(json_safe(r->j, &r->safe, "type", s->type_text, err));
        TRY(atlas_json_key_bool(r->j, "is_definition", s->is_definition, err));
        TRY(atlas_json_key_bool(r->j, "external", s->external, err));
        /* An external entity has no location Atlas indexed, so none is
         * emitted — an empty path would read as a file at the repository root. */
        if (!s->external) {
            TRY(json_safe(r->j, &r->safe, "file", s->file_text, err));
            TRY(atlas_json_key_int(r->j, "line", s->line, err));
            TRY(atlas_json_key_int(r->j, "col", s->col, err));
            TRY(atlas_json_key_int(r->j, "end_line", s->end_line, err));
        }
        TRY(atlas_json_key_str(r->j, "evidence", s->evidence, err));
        TRY(atlas_json_obj_end(r->j, err));
    }
    TRY(atlas_json_arr_end(r->j, err));
    return j_sem_trust(r, &rep->trust, err);
}

static atlas_status j_sem_graph(atlas_renderer *r, const atlas_sem_graph_report *rep,
                                atlas_err *err) {
    TRY(atlas_json_key_str(r->j, "repo", rep->repo.name, err));
    TRY(json_safe(r->j, &r->safe, "query", rep->query, err));
    TRY(atlas_json_key_str(r->j, "direction", rep->inbound ? "callers" : "callees", err));
    TRY(atlas_json_key(r->j, "nodes", err));
    TRY(atlas_json_arr_begin(r->j, err));
    for (size_t i = 0; i < rep->count; i++) {
        const atlas_sem_graph_item *g = &rep->items[i];
        TRY(atlas_json_obj_begin(r->j, err));
        TRY(atlas_json_key_int(r->j, "depth", g->depth, err));
        TRY(json_safe(r->j, &r->safe, "usr", g->usr, err));
        TRY(json_safe(r->j, &r->safe, "name", g->name, err));
        TRY(json_safe(r->j, &r->safe, "file", g->file_text, err));
        TRY(atlas_json_key_int(r->j, "line", g->line, err));
        TRY(atlas_json_key_str(r->j, "edge_kind", g->edge_kind, err));
        TRY(json_safe(r->j, &r->safe, "via", g->via_name, err));
        TRY(atlas_json_key_str(r->j, "evidence", g->evidence, err));
        TRY(json_safe(r->j, &r->safe, "site_file", g->site_file, err));
        TRY(atlas_json_key_int(r->j, "site_line", g->site_line, err));
        /* The true number of candidate targets, which may exceed how many were
         * recorded. Reported so an ambiguity never looks smaller than it is. */
        TRY(atlas_json_key_int(r->j, "candidate_total", g->candidate_total, err));
        TRY(atlas_json_obj_end(r->j, err));
    }
    TRY(atlas_json_arr_end(r->j, err));

    const atlas_sem_walk_summary *s = &rep->summary;
    TRY(atlas_json_key(r->j, "summary", err));
    TRY(atlas_json_obj_begin(r->j, err));
    TRY(atlas_json_key_int(r->j, "visited", s->visited, err));
    TRY(atlas_json_key_int(r->j, "emitted", s->emitted, err));
    TRY(atlas_json_key_int(r->j, "max_depth_reached", s->max_depth_reached, err));
    TRY(atlas_json_key_int(r->j, "proven", s->proven, err));
    TRY(atlas_json_key_int(r->j, "candidate", s->candidate, err));
    TRY(atlas_json_key_int(r->j, "lexical", s->lexical, err));
    TRY(atlas_json_key_int(r->j, "unknown", s->unknown, err));
    TRY(atlas_json_key_int(r->j, "unresolved_indirect", s->unresolved_indirect, err));
    TRY(atlas_json_key_bool(r->j, "truncated", s->truncated, err));
    TRY(atlas_json_key_str_opt(
        r->j, "truncated_reason",
        atlas_sem_trunc_reason_is_known(s->truncated_reason) ? s->truncated_reason : NULL, err));
    TRY(atlas_json_obj_end(r->j, err));
    return j_sem_trust(r, &rep->trust, err);
}

static atlas_status j_sem_indexed(atlas_renderer *r, const atlas_sem_index_summary *sum,
                                  atlas_err *err) {
    TRY(atlas_json_key_bool(r->j, "published", sum->published, err));
    TRY(atlas_json_key_bool(r->j, "no_change", sum->no_change, err));
    TRY(atlas_json_key_int(r->j, "generation_id", sum->generation_id, err));
    TRY(atlas_json_key_int(r->j, "units_total", sum->units_total, err));
    TRY(atlas_json_key_int(r->j, "units_parsed", sum->units_parsed, err));
    TRY(atlas_json_key_int(r->j, "units_reused", sum->units_reused, err));
    TRY(atlas_json_key_int(r->j, "units_retried", sum->units_retried, err));
    TRY(atlas_json_key_int(r->j, "units_complete", sum->units_complete, err));
    TRY(atlas_json_key_int(r->j, "units_partial", sum->units_partial, err));
    TRY(atlas_json_key_int(r->j, "units_failed", sum->units_failed, err));
    TRY(atlas_json_key_int(r->j, "units_unsupported", sum->units_unsupported, err));
    TRY(atlas_json_key_int(r->j, "symbols", sum->symbols, err));
    TRY(atlas_json_key_int(r->j, "edges", sum->edges, err));
    TRY(atlas_json_key_int(r->j, "includes", sum->includes, err));
    TRY(atlas_json_key_int(r->j, "candidates_attached", sum->candidates_attached, err));
    TRY(atlas_json_key_int(r->j, "duration_ms", sum->duration_ms, err));
    TRY(atlas_json_key_bool(r->j, "truncated", sum->truncated, err));
    TRY(atlas_json_key_str_opt(r->j, "truncated_reason", sum->truncated_reason, err));
    return json_safe(r->j, &r->safe, "failure_reason",
                     sum->failure_reason[0] != '\0' ? sum->failure_reason : NULL, err);
}

/* Item lists, shared by the impact report and the context package. Every item
 * carries its evidence class and the fixed reason that selected it, because a
 * consumer that could read the list without them could treat a filename guess
 * as a compiler proof. */
static atlas_status j_sem_items(atlas_renderer *r, const atlas_sem_item *items, size_t count,
                                atlas_err *err) {
    TRY(atlas_json_key(r->j, "items", err));
    TRY(atlas_json_arr_begin(r->j, err));
    for (size_t i = 0; i < count; i++) {
        const atlas_sem_item *it = &items[i];
        TRY(atlas_json_obj_begin(r->j, err));
        TRY(atlas_json_key_str(r->j, "kind", it->kind, err));
        TRY(json_safe(r->j, &r->safe, "name", it->name, err));
        TRY(json_safe(r->j, &r->safe, "file", it->file_text, err));
        TRY(atlas_json_key_int(r->j, "line", it->line, err));
        /* A9.1: the same keys `sem.context` emits over IPC, present only on a
         * decision item. Same names on both surfaces so one consumer reads both. */
        if (it->knowledge_kind[0] != '\0') {
            TRY(atlas_json_key_str(r->j, "knowledge_kind", it->knowledge_kind, err));
        }
        if (it->knowledge_status[0] != '\0') {
            TRY(atlas_json_key_str(r->j, "knowledge_status", it->knowledge_status, err));
        }
        /* A9.2.2, §24. Emitted whenever the item is a knowledge record — keyed
         * on `knowledge_kind` rather than on its own emptiness, so a record
         * Atlas knows nothing about still carries an explicit `UNKNOWN` rather
         * than an absent key a consumer would have to interpret. */
        if (it->knowledge_kind[0] != '\0') {
            TRY(atlas_json_key_str(r->j, "knowledge_truth",
                                   it->knowledge_truth[0] != '\0' ? it->knowledge_truth : "UNKNOWN",
                                   err));
        }
        TRY(atlas_json_key_str(r->j, "evidence", it->evidence, err));
        /* Checked against Atlas' own closed set before it is emitted. */
        TRY(atlas_json_key_str_opt(
            r->j, "why", atlas_sem_selection_reason_is_known(it->why) ? it->why : NULL, err));
        TRY(atlas_json_key_int(r->j, "depth", it->depth, err));
        TRY(atlas_json_obj_end(r->j, err));
    }
    return atlas_json_arr_end(r->j, err);
}

static atlas_status j_sem_impact(atlas_renderer *r, const atlas_sem_impact_report *rep,
                                 atlas_err *err) {
    TRY(atlas_json_key_str(r->j, "repo", rep->repo.name, err));
    TRY(json_safe(r->j, &r->safe, "subject", rep->query, err));
    TRY(atlas_json_key_str(r->j, "subject_kind", rep->subject_is_path ? "file" : "symbol", err));
    TRY(atlas_json_key_bool(r->j, "subject_found", rep->subject_found, err));
    TRY(j_sem_items(r, rep->items, rep->count, err));
    /* Split by evidence, never summed. */
    TRY(atlas_json_key_int(r->j, "proven", rep->proven, err));
    TRY(atlas_json_key_int(r->j, "candidate", rep->candidate, err));
    TRY(atlas_json_key_int(r->j, "lexical", rep->lexical, err));
    TRY(atlas_json_key_int(r->j, "unresolved_indirect", rep->unresolved_indirect, err));
    TRY(atlas_json_key_bool(r->j, "truncated", rep->truncated, err));
    TRY(atlas_json_key_str_opt(
        r->j, "truncated_reason",
        atlas_sem_trunc_reason_is_known(rep->truncated_reason) ? rep->truncated_reason : NULL,
        err));
    return j_sem_trust(r, &rep->trust, err);
}

static atlas_status j_sem_context(atlas_renderer *r, const atlas_sem_context_report *rep,
                                  atlas_err *err) {
    TRY(atlas_json_key_str(r->j, "repo", rep->repo.name, err));
    TRY(atlas_json_key_str_opt(r->j, "commit", rep->repo.scanned_head, err));
    /* The task is the caller's own text and is echoed safe-encoded. It is not
     * an instruction Atlas acted on: it ranked evidence and nothing else. */
    TRY(json_safe(r->j, &r->safe, "task", rep->task, err));
    TRY(atlas_json_key_int(r->j, "budget_bytes", rep->budget_bytes, err));
    TRY(atlas_json_key_int(r->j, "used_bytes", rep->used_bytes, err));
    TRY(atlas_json_key_bool(r->j, "budget_reached", rep->budget_reached, err));
    TRY(j_sem_items(r, rep->items, rep->count, err));
    /* The package's own gaps, always present so an empty array is a positive
     * statement rather than a missing key. */
    TRY(atlas_json_key(r->j, "not_included", err));
    TRY(atlas_json_arr_begin(r->j, err));
    for (size_t i = 0; i < rep->missing_count; i++) {
        TRY(atlas_json_str(r->j, rep->missing[i], err));
    }
    TRY(atlas_json_arr_end(r->j, err));
    return j_sem_trust(r, &rep->trust, err);
}

static atlas_status j_operation_status(atlas_renderer *r, const atlas_operation_report *rep,
                                       atlas_err *err) {
    atlas_json *j = r->j;
    atlas_status st = atlas_json_key_int(j, "operation_id", rep->id, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "kind", atlas_buf_cstr(&rep->kind), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "state", atlas_buf_cstr(&rep->state), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(j, "done", rep->done, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(j, "succeeded", rep->succeeded, err);
    }
    if (st == ATLAS_OK && rep->done) {
        st = atlas_json_key_int(j, "duration_ms", rep->duration_ms, err);
    }
    /* Already safe-encoded by the service layer. */
    if (st == ATLAS_OK && rep->message.len > 0) {
        st = atlas_json_key_str(j, "message", atlas_buf_cstr(&rep->message), err);
    }
    if (st == ATLAS_OK && rep->detail.len > 0) {
        st = atlas_json_key_str(j, "detail", atlas_buf_cstr(&rep->detail), err);
    }
    return st;
}

/* --- A12.0: plans ---------------------------------------------------------
 *
 * Same note as `j_job_item`: these strings are already safe-encoded by the
 * daemon and are emitted through the JSON writer, which escapes for JSON
 * structure. Neither step is the other, and neither is applied twice.
 *
 * The same fields the human renderer prints, from the same struct: one service
 * result, two presentations. Every run and job field is emitted only when
 * present — there is deliberately no empty-string spelling of "not submitted
 * yet", because an empty identifier reads like an identifier. */
static atlas_status j_plan_item(atlas_renderer *r, const atlas_plan_render *pr, atlas_err *err) {
    atlas_json *j = r->j;
    /* Inside the `plans` array this is an anonymous object; on its own it is a
     * set of members on the document. The writer refuses an unkeyed object at
     * the top level, which is the correct refusal and is why this branch
     * exists. */
    atlas_status st = pr->in_list ? atlas_json_obj_begin(j, err) : ATLAS_OK;
    const struct {
        const char *k;
        const char *v;
    } strs[] = {
        {"plan", pr->plan},
        {"repo", pr->repo},
        {"status", pr->status},
        {"created_at", pr->created_at},
    };
    for (size_t i = 0; st == ATLAS_OK && i < sizeof strs / sizeof strs[0]; i++) {
        if (strs[i].v != NULL && strs[i].v[0] != '\0') {
            st = atlas_json_key_str(j, strs[i].k, strs[i].v, err);
        }
    }
    if (st == ATLAS_OK && pr->in_list) {
        return atlas_json_obj_end(j, err);
    }
    if (st != ATLAS_OK || !pr->detail) {
        return st;
    }

    /* The counts, each beside its compiled-in ceiling: a count with no bound
     * next to it does not say whether the plan has budget left. */
    const struct {
        const char *k;
        int64_t v;
    } nums[] = {
        {"rev_no", pr->rev_no},
        {"revision_limit", ATLAS_PLAN_MAX_REVISIONS},
        {"planner_jobs_seen", pr->planner_jobs_seen},
        {"planner_job_limit", ATLAS_PLAN_MAX_PLANNER_JOBS},
        {"stages_accepted", pr->stages_accepted},
        {"revisions", pr->revision_count},
    };
    for (size_t i = 0; st == ATLAS_OK && i < sizeof nums / sizeof nums[0]; i++) {
        st = atlas_json_key_int(j, nums[i].k, nums[i].v, err);
    }
    if (st == ATLAS_OK && pr->max_parallel > 0) {
        st = atlas_json_key_int(j, "max_parallel", pr->max_parallel, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(j, "replan_wanted", pr->replan_wanted, err);
    }
    if (st == ATLAS_OK && pr->planner_job != NULL && pr->planner_job[0] != '\0') {
        st = atlas_json_key_str(j, "planner_job", pr->planner_job, err);
        if (st == ATLAS_OK && pr->planner_job_state != NULL) {
            st = atlas_json_key_str(j, "planner_job_state", pr->planner_job_state, err);
        }
    }
    /* The operator's own words and the operator's own commands, each with the
     * encoding it arrived in stated beside it so a reader can decode it back. */
    if (st == ATLAS_OK && pr->goal != NULL) {
        st = atlas_json_key_str(j, "goal_encoding", "atlas-safe-1", err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "goal", pr->goal, err);
        }
    }
    if (st == ATLAS_OK && pr->gate_floor_text != NULL) {
        st = atlas_json_key_int(j, "gate_floor_count", pr->gate_floor_count, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "gate_floor_encoding", "atlas-safe-1", err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "gate_floor_text", pr->gate_floor_text, err);
        }
    }

    /* One label for the array rather than one per row: every `title` below is a
     * planner's bytes, encoded the same way. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "task_title_encoding", "atlas-safe-1", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "task_title_provenance", "UNTRUSTED_DATA", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key(j, "tasks", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(j, err);
    }
    for (size_t i = 0; st == ATLAS_OK && i < pr->task_count; i++) {
        const atlas_plan_task_render *t = &pr->tasks[i];
        st = atlas_json_obj_begin(j, err);
        const struct {
            const char *k;
            const char *v;
        } tf[] = {
            {"key", t->key},   {"kind", t->kind}, {"title", t->title},
            {"job", t->job},   {"job_state", t->job_state},
            {"run", t->run},   {"run_status", t->run_status},
            {"usage_model", t->usage_model},
        };
        for (size_t k = 0; st == ATLAS_OK && k < sizeof tf / sizeof tf[0]; k++) {
            if (tf[k].v != NULL && tf[k].v[0] != '\0') {
                st = atlas_json_key_str(j, tf[k].k, tf[k].v, err);
            }
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(j, "stage", t->stage, err);
        }
        /* A10.0. Emitted only when observed. An absent measurement is not a
         * zero: a worker that never reported a price spent something Atlas
         * cannot name, and reporting that as 0 would be the lie. */
        if (st == ATLAS_OK && t->has_cost) {
            st = atlas_json_key_int(j, "usage_cost_micro_usd", t->usage_cost_micro_usd, err);
        }
        if (st == ATLAS_OK && t->has_turns) {
            st = atlas_json_key_int(j, "usage_turns", t->usage_turns, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_obj_end(j, err);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(j, err);
    }

    if (st == ATLAS_OK && pr->content != NULL) {
        /* A planner's bytes, labelled exactly as `job.artifact` labels the
         * artifact they came from. */
        st = atlas_json_key_int(j, "content_rev_no", pr->content_rev_no, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "content_encoding", "atlas-safe-1", err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "content_provenance", "UNTRUSTED_DATA", err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "content", pr->content, err);
        }
    }
    if (st == ATLAS_OK && pr->busy) {
        /* Present only when true, and named so it cannot be read as a verdict
         * about the plan: nothing was written and the plan is resumable. */
        st = atlas_json_key_bool(j, "busy", true, err);
    }
    return st;
}

/* --- A12.1 T16: the `memory` command family ---------------------------------
 *
 * One method, seven forms, `mr->form` the discriminator -- `j_plan_item`'s own
 * shape for `in_list`. Encoding follows `h_memory_item`'s own header comment
 * in `render_human.c` exactly, restated here because both renderers document
 * their own value-by-value contract at the top of the file: RAW values
 * (`registered_at`, `observed_at`) go through `json_safe()`; ALREADY-SAFE
 * values (`scan_rel_path`, `pack_body`, `patch_diff`, `patch_findings`) are
 * emitted as-is with an `_encoding: atlas-safe-1` label beside them
 * (`src/ipc/server_orch.c`'s spelling). Every source's `path` is ALREADY-SAFE
 * too but is deliberately the one exception left unlabelled: `path_encoding`
 * is already this file's own name, at `json_path()` above, for a different,
 * `docs/provenance.md`-documented contract (`"utf8"` | `"percent-escaped"`,
 * beside `path_bytes_hex`) -- reusing it here for the constant `"atlas-safe-1"`
 * would make one key name mean two incompatible things depending on which
 * command produced the document, which is worse than the asymmetry it would
 * fix (fix round, item 4: this was tried and reverted for exactly that
 * reason). Everything else is an Atlas-minted uid, hex digest, or
 * closed-vocabulary name and needs neither. */
static atlas_status j_memory_item(atlas_renderer *r, const atlas_memory_render *mr, atlas_err *err) {
    atlas_json *j = r->j;
    atlas_status st = mr->in_list ? atlas_json_obj_begin(j, err) : ATLAS_OK;
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "form", mr->form, err);
    }
    if (st == ATLAS_OK && mr->repo != NULL) {
        st = atlas_json_key_str(j, "repo", mr->repo, err);
    }

    if (st == ATLAS_OK && strcmp(mr->form, "status") == 0) {
        st = atlas_json_key_str(j, "policy_state", mr->policy_state, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "policy_reason", mr->policy_reason, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "policy_reason_detail", mr->policy_reason_detail, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "policy_path", mr->policy_path, err);
        }
        if (st == ATLAS_OK && mr->repo != NULL) {
            st = atlas_json_key_str(j, "plan_for", mr->plan_for, err);
            if (st == ATLAS_OK) {
                st = atlas_json_key_bool(j, "generation_found", mr->generation_found, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_key_int(j, "generation", mr->generation, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_key_str_opt(j, "head_commit", mr->head_commit, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_key(j, "sources", err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_arr_begin(j, err);
            }
            for (size_t i = 0; st == ATLAS_OK && i < mr->source_count; i++) {
                const atlas_memory_source_render *s = &mr->sources[i];
                st = atlas_json_obj_begin(j, err);
                if (st == ATLAS_OK) {
                    st = atlas_json_key_str(j, "uid", s->uid, err);
                }
                if (st == ATLAS_OK) {
                    st = atlas_json_key_str(j, "class", s->cls, err);
                }
                if (st == ATLAS_OK) {
                    st = atlas_json_key_str(j, "path", s->path, err);
                }
                if (st == ATLAS_OK) {
                    st = json_safe(j, &r->safe, "registered_at", s->registered_at, err);
                }
                if (st == ATLAS_OK) {
                    st = atlas_json_key_bool(j, "has_version", s->has_version, err);
                }
                if (st == ATLAS_OK && s->has_version) {
                    st = atlas_json_key_str(j, "version_uid", s->version_uid, err);
                    if (st == ATLAS_OK) {
                        st = atlas_json_key_str(j, "content_sha256", s->content_sha256, err);
                    }
                    if (st == ATLAS_OK) {
                        st = atlas_json_key_int(j, "content_bytes", s->content_bytes, err);
                    }
                    if (st == ATLAS_OK) {
                        st = json_safe(j, &r->safe, "observed_at", s->observed_at, err);
                    }
                    if (st == ATLAS_OK) {
                        st = atlas_json_key_str_opt(j, "commit_oid", s->commit_oid, err);
                    }
                }
                if (st == ATLAS_OK) {
                    st = atlas_json_obj_end(j, err);
                }
            }
            if (st == ATLAS_OK) {
                st = atlas_json_arr_end(j, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_key_bool(j, "sources_truncated", mr->sources_truncated, err);
            }
        }
    } else if (st == ATLAS_OK && strcmp(mr->form, "scan") == 0) {
        st = atlas_json_key_bool(j, "no_sources", mr->scan_no_sources, err);
        if (st == ATLAS_OK && !mr->scan_no_sources) {
            st = atlas_json_key_str(j, "source", mr->scan_source_uid, err);
            if (st == ATLAS_OK) {
                st = atlas_json_key_str(j, "rel_path_encoding", "atlas-safe-1", err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_key_str(j, "rel_path", mr->scan_rel_path, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_key_bool(j, "put", mr->scan_put, err);
            }
            if (st == ATLAS_OK && mr->scan_put) {
                st = atlas_json_key_str(j, "version", mr->scan_version_uid, err);
                if (st == ATLAS_OK) {
                    st = atlas_json_key_str(j, "content_sha256", mr->scan_content_sha256, err);
                }
                if (st == ATLAS_OK) {
                    st = atlas_json_key_int(j, "content_bytes", mr->scan_content_bytes, err);
                }
                if (st == ATLAS_OK) {
                    st = atlas_json_key_bool(j, "created", mr->scan_created, err);
                }
            } else if (st == ATLAS_OK) {
                st = atlas_json_key_str(j, "outcome", mr->scan_outcome, err);
            }
        }
    } else if (st == ATLAS_OK && strcmp(mr->form, "reconcile") == 0) {
        st = atlas_json_key_bool(j, "accepted", mr->accepted, err);
    } else if (st == ATLAS_OK && strcmp(mr->form, "pack") == 0) {
        st = atlas_json_key_bool(j, "preview", mr->pack_preview, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_bool(j, "found", mr->pack_found, err);
        }
        if (st == ATLAS_OK && !mr->pack_found) {
            /* Kept apart deliberately: a run that has a pack for a different
               repository is not the same fact as no run having a pack at
               all, and collapsing them into one `found: false` would tell a
               caller the wrong one. */
            st = atlas_json_key_bool(j, "other_repo", mr->pack_other_repo, err);
        }
        if (st == ATLAS_OK && mr->pack_found) {
            if (!mr->pack_preview) {
                st = atlas_json_key_str_opt(j, "status", mr->pack_status, err);
                if (st == ATLAS_OK) {
                    st = atlas_json_key_str_opt(j, "which_moved", mr->pack_which_moved, err);
                }
                if (st == ATLAS_OK) {
                    st = atlas_json_key_bool(j, "reliance_checked", mr->reliance_checked, err);
                }
                if (st == ATLAS_OK) {
                    st = atlas_json_key_bool(j, "reliance_complete", mr->reliance_complete, err);
                }
                if (st == ATLAS_OK) {
                    st = atlas_json_key_str(j, "reliance_claim_uids",
                                            mr->reliance_claim_uids != NULL ? mr->reliance_claim_uids
                                                                            : "",
                                            err);
                }
            }
            if (st == ATLAS_OK) {
                st = atlas_json_key_int(j, "claim_count", mr->pack_claim_count, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_key_int(j, "excluded_count", mr->pack_excluded_count, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_key_int(j, "unanchored_count", mr->pack_unanchored_count, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_key_str(j, "digest", mr->pack_digest, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_key_str(j, "body_encoding", "atlas-safe-1", err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_key_str(j, "body", mr->pack_body, err);
            }
        }
    } else if (st == ATLAS_OK && strcmp(mr->form, "diff") == 0) {
        st = atlas_json_key_bool(j, "generation_found", mr->diff_generation_found, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(j, "generation", mr->diff_generation, err);
        }
        if (st == ATLAS_OK && mr->diff_generation_found) {
            st = atlas_json_key_str(j, "claim", mr->diff_claim_uid, err);
            if (st == ATLAS_OK) {
                st = atlas_json_key_str(j, "kind", mr->diff_kind, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_key_str(j, "reason", mr->diff_reason != NULL ? mr->diff_reason : "",
                                        err);
            }
        }
    } else if (st == ATLAS_OK && strcmp(mr->form, "patch") == 0) {
        st = atlas_json_key_str(j, "source", mr->patch_source_uid, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "diff_encoding", "atlas-safe-1", err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "diff", mr->patch_diff, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "findings_encoding", "atlas-safe-1", err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "findings", mr->patch_findings, err);
        }
    } else if (st == ATLAS_OK && strcmp(mr->form, "trailer") == 0) {
        st = atlas_json_key_bool(j, "compose", mr->trailer_compose, err);
        if (st == ATLAS_OK && mr->trailer_compose) {
            st = atlas_json_key_str(j, "run", mr->trailer_run, err);
            if (st == ATLAS_OK) {
                st = atlas_json_key_str(j, "block", mr->trailer_block, err);
            }
        } else if (st == ATLAS_OK) {
            st = atlas_json_key_bool(j, "found", mr->trailer_found, err);
            if (st == ATLAS_OK && mr->trailer_found) {
                st = atlas_json_key_bool(j, "has_block", mr->trailer_has_block, err);
                if (st == ATLAS_OK) {
                    st = atlas_json_key_bool(j, "bound_hit", mr->trailer_bound_hit, err);
                }
                if (st == ATLAS_OK && mr->trailer_has_block) {
                    st = atlas_json_key_str_opt(j, "run", mr->trailer_run, err);
                    if (st == ATLAS_OK) {
                        st = atlas_json_key_int(j, "generation", mr->trailer_generation, err);
                    }
                    if (st == ATLAS_OK) {
                        st = atlas_json_key_bool(j, "context_digest_ok",
                                                 mr->trailer_context_digest_ok, err);
                    }
                    if (st == ATLAS_OK) {
                        st = atlas_json_key_bool(j, "decision_set_ok", mr->trailer_decision_set_ok,
                                                 err);
                    }
                    if (st == ATLAS_OK) {
                        st = atlas_json_key_str_opt(j, "change_reason", mr->trailer_change_reason_uid,
                                                    err);
                    }
                    if (st == ATLAS_OK) {
                        st = atlas_json_key_str(j, "unknown_fields",
                                                mr->trailer_unknown_fields != NULL
                                                    ? mr->trailer_unknown_fields
                                                    : "",
                                                err);
                    }
                }
            }
        }
    }

    if (st == ATLAS_OK && mr->in_list) {
        st = atlas_json_obj_end(j, err);
    }
    return st;
}

/* --- A15 T5: `atlas review apply` ----------------------------------------
 *
 * Deliberately not `list_begin`/`list_end`: `j_list_end` always writes a
 * `count` key, and the plan's frozen `--check` document has no such key
 * between `"entries":[...]` and `"ready":...` — verified against
 * `decision list --json`, which stacks `list_end`'s `"count"` with
 * `decision_counts`'s own fields, the same mechanism this command would
 * inherit if it reused `list_end`. `review_begin` opens the array itself and
 * `review_totals` closes it, so there is exactly one shape rather than one
 * the plan pinned and one the generic method would actually produce.
 *
 * `o->status` and `o->detail` arrive already safe-encoded or as fixed Atlas
 * text (see the fields' own comments in include/atlas/service.h); printed
 * as-is via `atlas_json_key_str_opt`, never re-encoded. `o->entry->repo` is
 * UNTRUSTED_DATA copied out of the operator's own sheet file and is
 * safe-encoded here like any other repository-originated string. */
static atlas_status j_review_begin(atlas_renderer *r, const char *sheet_path, bool check_only,
                                   atlas_err *err) {
    atlas_json *j = r->j;
    TRY(atlas_json_key_bool(j, "check", check_only, err));
    TRY(json_safe(j, &r->safe, "sheet", sheet_path, err));
    r->items = 0;
    r->in_list = true;
    TRY(atlas_json_key(j, "entries", err));
    return atlas_json_arr_begin(j, err);
}

static atlas_status j_review_entry(atlas_renderer *r, const atlas_review_outcome *o,
                                   atlas_err *err) {
    atlas_json *j = r->j;
    r->items++;
    TRY(atlas_json_obj_begin(j, err));
    TRY(atlas_json_key_int(j, "line", (int64_t)o->entry->line, err));
    TRY(atlas_json_key_str(j, "intent", atlas_decision_intent_name(o->entry->intent), err));
    TRY(json_safe(j, &r->safe, "repo", o->entry->repo, err));
    /* Already a checked shape (atlas_decision_uid_is_valid, applied by the
     * sheet parser) -- printed whole and unencoded, exactly as every other
     * decision id in this file is. */
    TRY(atlas_json_key_str(j, "decision", o->entry->decision, err));
    TRY(atlas_json_key_int(j, "revision", o->entry->revision_no, err));
    TRY(atlas_json_key_str(j, "prefix", o->entry->prefix, err));
    TRY(atlas_json_key_str(j, "verdict", atlas_review_verdict_name(o->verdict), err));
    TRY(atlas_json_key_str_opt(j, "status", o->status.len > 0 ? atlas_buf_cstr(&o->status) : NULL,
                               err));
    TRY(atlas_json_key_str_opt(j, "detail", o->detail.len > 0 ? atlas_buf_cstr(&o->detail) : NULL,
                               err));
    return atlas_json_obj_end(j, err);
}

static atlas_status j_review_totals(atlas_renderer *r, bool check_only,
                                    const atlas_review_totals *t, atlas_err *err) {
    atlas_json *j = r->j;
    r->in_list = false;
    TRY(atlas_json_arr_end(j, err));
    if (check_only) {
        TRY(atlas_json_key_int(j, "ready", t->ready, err));
        TRY(atlas_json_key_int(j, "moved", t->moved, err));
        TRY(atlas_json_key_int(j, "disposed", t->disposed, err));
        return atlas_json_key_int(j, "missing", t->missing, err);
    }
    /* Never reached through `--json` today -- it is refused without
     * `--check` -- but implemented for the same reason the human renderer's
     * other branch is: both renderers implement both totals shapes. */
    TRY(atlas_json_key_int(j, "applied", t->applied, err));
    TRY(atlas_json_key_int(j, "abandoned", t->abandoned, err));
    TRY(atlas_json_key_int(j, "moved", t->moved, err));
    TRY(atlas_json_key_int(j, "disposed", t->disposed, err));
    TRY(atlas_json_key_int(j, "missing", t->missing, err));
    return atlas_json_key_int(j, "refused", t->refused, err);
}

const atlas_renderer_vtbl ATLAS_RENDERER_JSON = {
    j_begin,      j_end,          j_note_repo,    j_note_query,   j_list_begin,
    j_list_end,   j_doctor,       j_version,      j_repo_item,    j_repo_added,
    j_repo_scanner_set,
    j_repo_removed, j_scan,       j_status,       j_search_item,  j_file,
    j_history_item, j_diff_begin, j_diff_item,    j_diff_end,
    j_job_item,
    j_daemon_status, j_daemon_ping, j_repo_state, j_sync,         j_event_item,
    j_events_end, j_unit_text,    j_unit_install, j_integrate,
    /* --- A3 --- */
    j_code_status, j_code_file,   j_code_symbol_item, j_code_edge_item,
    j_code_walk_item, j_code_walk_end,
    /* --- A8-CI --- */
    j_sem_status, j_sem_config, j_sem_symbols, j_sem_graph, j_sem_indexed, j_sem_impact, j_sem_context,
    j_code_list_begin, j_code_list_end,
    /* --- A4 --- */
    j_decision_item, j_decision_show, j_decision_event, j_decision_outcome, j_decision_edge,
    j_decision_counts, j_decision_ledger,
    /* --- A5 --- */
    j_backup_created, j_backup_verified, j_backup_restored,
    j_operation_status, j_maintenance,
    /* --- A6 --- */
    j_gate,
    /* --- A9.2 --- */
    j_verify,
    j_verify_intake,
    /* --- A9 --- */
    j_apikey_created, j_apikey_listed, j_apikey_revoked,
    /* --- A12.0 --- */
    j_plan_item,
    /* --- A12.1 T16 --- */
    j_memory_item,
    /* --- A15 T5 --- */
    j_review_begin, j_review_entry, j_review_totals,
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
