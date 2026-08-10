/* Atlas - human-readable renderer.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Untrusted-text policy, applied value by value below:
 *
 *   RAW values come straight from the repository or from git and are passed
 *   through atlas_safe() before printing: commit subjects and bodies, author
 *   names and emails, branch names, git error text, and any path held as raw
 *   bytes (git_common_dir).
 *
 *   ALREADY-SAFE values were encoded when they were stored or built and are
 *   printed as-is: path_text, root_path_text, old_path_text, and the diff
 *   entries' path fields. Re-encoding them would double-escape '%'.
 *
 *   ATLAS-OWNED values are literals or validated fields and need no encoding:
 *   repository names (validated charset), object ids (validated hex), ISO
 *   timestamps, head_state, object_format, language, change_type, and notes.
 */
#include "cli/render.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "atlas/atlas.h"

#define LABEL "  %-17s "

void atlas_format_time(int64_t unix_time, char *out, size_t out_size) {
    if (unix_time <= 0) {
        (void)snprintf(out, out_size, "-");
        return;
    }
    time_t t = (time_t)unix_time;
    struct tm tm_utc;
    if (gmtime_r(&t, &tm_utc) == NULL) {
        (void)snprintf(out, out_size, "-");
        return;
    }
    (void)snprintf(out, out_size, "%04d-%02d-%02dT%02d:%02d:%02dZ", tm_utc.tm_year + 1900,
                   tm_utc.tm_mon + 1, tm_utc.tm_mday, tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
}

void atlas_short_oid(const char *oid, char *out, size_t out_size) {
    if (oid == NULL || oid[0] == '\0') {
        (void)snprintf(out, out_size, "-");
        return;
    }
    (void)snprintf(out, out_size, "%.12s", oid);
}

static const char *yes_no(bool v) {
    return v ? "yes" : "no";
}

static const char *dash_if_empty(const char *s) {
    return (s != NULL && s[0] != '\0') ? s : "-";
}

static atlas_status ok(void) {
    return ATLAS_OK;
}

/* --- lifecycle ----------------------------------------------------------- */

static atlas_status h_begin(atlas_renderer *r, const char *command, atlas_err *err) {
    (void)r;
    (void)command;
    (void)err;
    return ok();
}

static atlas_status h_end(atlas_renderer *r, atlas_err *err) {
    (void)err;
    (void)fflush(r->out);
    return ok();
}

static atlas_status h_note_repo(atlas_renderer *r, const char *repo, atlas_err *err) {
    (void)r;
    (void)repo;
    (void)err;
    return ok();
}

static atlas_status h_note_query(atlas_renderer *r, const char *query, atlas_search_mode mode,
                                 atlas_err *err) {
    (void)err;
    if (mode == ATLAS_SEARCH_DEGRADED_LIKE) {
        /* The query is user input and may contain anything. */
        (void)fprintf(r->out,
                      "note: FTS5 is unavailable, using unranked substring search for \"%s\"\n",
                      atlas_safe(&r->safe, query));
    }
    return ok();
}

static atlas_status h_list_begin(atlas_renderer *r, const char *key, atlas_err *err) {
    (void)key;
    (void)err;
    r->items = 0;
    r->in_list = true;
    return ok();
}

static atlas_status h_list_end(atlas_renderer *r, const char *singular, const char *plural,
                               int64_t count, atlas_err *err) {
    (void)err;
    r->in_list = false;
    if (count == 0) {
        (void)fprintf(r->out, "no %s\n", plural);
    } else {
        (void)fprintf(r->out, "%" PRId64 " %s\n", count, count == 1 ? singular : plural);
    }
    return ok();
}

/* --- doctor -------------------------------------------------------------- */

static atlas_status h_doctor(atlas_renderer *r, const atlas_doctor_report *rep, atlas_err *err) {
    (void)err;
    FILE *o = r->out;
    (void)fprintf(o, "Atlas %s (phase %s)\n", rep->atlas_version, ATLAS_PHASE);
    (void)fprintf(o, LABEL "%s\n", "build compiler", rep->build_compiler);
    if (rep->git_found) {
        /* The executable path and git's own version string are both external. */
        (void)fprintf(o, LABEL "%s (%s)\n", "git", atlas_safe(&r->safe, atlas_buf_cstr(&rep->git_exe)),
                      atlas_safe(&r->safe, atlas_buf_cstr(&rep->git_version)));
    } else {
        (void)fprintf(o, LABEL "NOT FOUND\n", "git");
    }
    (void)fprintf(o, LABEL "%s runtime, %s at build time\n", "sqlite", rep->sqlite_runtime,
                  rep->sqlite_compiled);
    (void)fprintf(o, LABEL "%s (from %s)\n", "data directory",
                  atlas_safe(&r->safe, atlas_buf_cstr(&rep->data_dir)),
                  atlas_datadir_source_name(rep->data_dir_source));
    (void)fprintf(o, LABEL "%s%s\n", "database",
                  atlas_safe(&r->safe, atlas_buf_cstr(&rep->db_path)),
                  rep->index_present ? "" : "  (absent)");
    /* A7. Printed above the index checks, because it is a property of the
     * machine rather than of the index and is exactly as answerable when there
     * is no index — which is when somebody most often runs this. Both strings
     * come from closed Atlas-owned vocabularies, so neither is encoded and
     * neither can carry a byte somebody else chose. */
    (void)fprintf(o, LABEL "%s (%s)\n", "operator authority",
                  rep->authority_state == ATLAS_AUTHORITY_GRANTED ? "granted" : "locked",
                  atlas_authority_reason_name(rep->authority_reason));
    (void)fprintf(o, LABEL "%s (%s)\n", "deployment",
                  rep->deployment_state == ATLAS_SYSPOLICY_SYSTEM ? "system" : "per-user",
                  atlas_syspolicy_reason_name(rep->deployment_reason));
    (void)fprintf(o, LABEL "%s\n", "",
                  atlas_authority_reason_explain(rep->authority_reason));
    if (!rep->index_present) {
        /* Reported, not created. On a machine where Atlas has never run this is
         * the whole answer, and it is a correct one. */
        (void)fprintf(o, LABEL "%s\n", "data directory",
                      rep->data_dir_present ? "present, no index yet" : "absent");
        (void)fprintf(o, LABEL "%s\n", "schema version", "- (nothing to inspect)");
        (void)fprintf(o, LABEL "%s\n", "note",
                      "no index exists yet; it is created by the first repository "
                      "registration or by the daemon");
        (void)fprintf(o, "status: %s\n", rep->ok ? "ok" : "attention needed");
        return ok();
    }
    (void)fprintf(o, LABEL "%d (expected %d)\n", "schema version", rep->schema_version,
                  rep->expected_schema_version);
    (void)fprintf(o, LABEL "%s\n", "journal mode", dash_if_empty(rep->journal_mode));
    (void)fprintf(o, LABEL "%s\n", "foreign keys", rep->foreign_keys ? "on" : "off");
    (void)fprintf(o, LABEL "%s\n", "fts5", rep->fts5 ? "available" : "unavailable");
    (void)fprintf(o, LABEL "%s\n", "search mode", atlas_search_mode_name(rep->search_mode));
    (void)fprintf(o, LABEL "%s\n", "integrity check",
                  atlas_safe(&r->safe, atlas_buf_cstr(&rep->integrity)));
    (void)fprintf(o, LABEL "%s\n", "foreign key check",
                  atlas_safe(&r->safe, atlas_buf_cstr(&rep->foreign_key_check)));
    (void)fprintf(o, LABEL "%s\n", "text encoding", ATLAS_TEXT_ENCODING_NAME);
    (void)fprintf(o, LABEL "%" PRId64 "\n", "repositories", rep->repo_count);

    if (rep->problems.len > 0) {
        (void)fprintf(o, "problems:\n");
        const char *p = atlas_buf_cstr(&rep->problems);
        while (*p != '\0') {
            const char *nl = strchr(p, '\n');
            size_t len = (nl != NULL) ? (size_t)(nl - p) : strlen(p);
            /* A problem may quote git's output. */
            (void)fprintf(o, "  - %s\n", atlas_safe_n(&r->safe, p, len));
            if (nl == NULL) {
                break;
            }
            p = nl + 1;
        }
    }
    (void)fprintf(o, "status: %s\n", rep->ok ? "ok" : "attention needed");
    return ok();
}

static atlas_status h_version(atlas_renderer *r, atlas_err *err) {
    (void)err;
    (void)fprintf(r->out, "atlas %s (phase %s)\n", ATLAS_VERSION_STRING, ATLAS_PHASE);
    return ok();
}

/* --- repositories -------------------------------------------------------- */

static atlas_status h_repo_item(atlas_renderer *r, const atlas_repo_info *ri, atlas_err *err) {
    (void)err;
    char short_head[16];
    atlas_short_oid(ri->scanned_head, short_head, sizeof(short_head));
    if (r->items == 0) {
        (void)fprintf(r->out, "%-20s %-40s %-21s %-13s %-16s %-5s %s\n", "NAME", "ROOT",
                      "LAST SCAN", "HEAD", "BRANCH", "DIRTY", "WORKTREE");
    }
    r->items++;
    /* root_path_text is already safe-encoded; the branch name is raw. */
    (void)fprintf(r->out, "%-20s %-40s %-21s %-13s %-16s %-5s %s\n", ri->name,
                  atlas_buf_cstr(&ri->root_path_text), dash_if_empty(ri->last_scan_at), short_head,
                  atlas_safe(&r->safe, dash_if_empty(ri->current_branch)), yes_no(ri->dirty),
                  ri->is_linked_worktree ? "linked" : "main");
    return ok();
}

static atlas_status h_repo_added(atlas_renderer *r, const atlas_repo_info *ri, atlas_err *err) {
    (void)err;
    (void)fprintf(r->out, "registered %s\n", ri->name);
    (void)fprintf(r->out, LABEL "%s\n", "root", atlas_buf_cstr(&ri->root_path_text));
    (void)fprintf(r->out, LABEL "%s\n", "git common dir",
                  atlas_safe_n(&r->safe, ri->git_common_dir.data, ri->git_common_dir.len));
    (void)fprintf(r->out, LABEL "%s\n", "git dir",
                  atlas_safe_n(&r->safe, ri->git_dir.data, ri->git_dir.len));
    (void)fprintf(r->out, LABEL "%s\n", "worktree",
                  ri->is_linked_worktree ? "linked worktree" : "main worktree");
    (void)fprintf(r->out, LABEL "%s\n", "object format", ri->object_format);
    (void)fprintf(r->out, LABEL "%s\n", "registered at", ri->registered_at);
    (void)fprintf(r->out, "next: atlas scan %s\n", ri->name);
    return ok();
}

static atlas_status h_repo_removed(atlas_renderer *r, const atlas_repo_info *ri, atlas_err *err) {
    (void)err;
    (void)fprintf(r->out, "removed %s from the Atlas index\n", ri->name);
    (void)fprintf(r->out, LABEL "%s\n", "root", atlas_buf_cstr(&ri->root_path_text));
    (void)fprintf(r->out, "the repository itself was not modified\n");
    return ok();
}

/* --- scan ---------------------------------------------------------------- */

static atlas_status h_scan(atlas_renderer *r, const char *repo, const atlas_scan_summary *s,
                           atlas_err *err) {
    (void)err;
    FILE *o = r->out;
    char short_head[16];
    atlas_short_oid(s->head_oid, short_head, sizeof(short_head));
    (void)fprintf(o, "scanned %s at %s (%s", repo, short_head, s->head_state);
    if (s->branch[0] != '\0') {
        (void)fprintf(o, ", branch %s", atlas_safe(&r->safe, s->branch));
    }
    (void)fprintf(o, ")\n");
    (void)fprintf(o,
                  LABEL "%" PRId64 " total, %" PRId64 " added, %" PRId64 " modified, %" PRId64
                        " deleted, %" PRId64 " unchanged\n",
                  "files", s->files_total, s->files_added, s->files_modified, s->files_deleted,
                  s->files_unchanged);
    if (s->files_unreadable > 0 || s->files_unsafe > 0) {
        (void)fprintf(o, LABEL "%" PRId64 " unreadable, %" PRId64 " refused (symlinked path)\n",
                      "skipped", s->files_unreadable, s->files_unsafe);
    }
    if (s->history_skipped) {
        (void)fprintf(o, LABEL "skipped (--no-history)\n", "history");
    } else {
        (void)fprintf(o,
                      LABEL "%" PRId64 " new of %" PRId64 " seen, %" PRId64 " file changes\n",
                      "commits", s->commits_ingested, s->commits_seen, s->changes_ingested);
    }
    (void)fprintf(o, LABEL "%" PRId64 " records\n", "evidence", s->evidence_created);
    (void)fprintf(o, LABEL "%s\n", "worktree", s->dirty ? "dirty" : "clean");
    (void)fprintf(o, LABEL "%s\n", "compile db",
                  s->compile_db_found
                      ? (s->compile_db_is_symlink ? "compile_commands.json (symlink, not parsed)"
                                                  : "compile_commands.json (not parsed in A0)")
                      : "not found");
    (void)fprintf(o, "scan id %" PRId64 "\n", s->scan_id);
    return ok();
}

/* --- status -------------------------------------------------------------- */

static atlas_status h_status(atlas_renderer *r, const atlas_status_report *s, atlas_err *err) {
    (void)err;
    FILE *o = r->out;
    const atlas_repo_info *ri = &s->repo;
    (void)fprintf(o, "repository %s\n", ri->name);
    (void)fprintf(o, LABEL "%s\n", "root", atlas_buf_cstr(&ri->root_path_text));
    (void)fprintf(o, LABEL "%s\n", "git common dir",
                  atlas_safe_n(&r->safe, ri->git_common_dir.data, ri->git_common_dir.len));
    (void)fprintf(o, LABEL "%s%s\n", "git dir",
                  atlas_safe_n(&r->safe, ri->git_dir.data, ri->git_dir.len),
                  ri->is_linked_worktree ? " (linked worktree)" : " (main worktree)");
    if (s->sibling_worktrees > 0) {
        (void)fprintf(o,
                      LABEL "%" PRId64
                            " other registered worktree(s) share this object store\n",
                      "siblings", s->sibling_worktrees);
    }
    (void)fprintf(o, LABEL "%s\n", "object format", ri->object_format);
    (void)fprintf(o, LABEL "%s\n", "registered at", ri->registered_at);

    if (s->never_scanned) {
        (void)fprintf(o, LABEL "never scanned (run: atlas scan %s)\n", "last scan", ri->name);
    } else {
        char short_head[16];
        atlas_short_oid(ri->scanned_head, short_head, sizeof(short_head));
        (void)fprintf(o, LABEL "%s (scan %" PRId64 ")\n", "last scan", ri->last_scan_at,
                      ri->last_scan_id);
        (void)fprintf(o, LABEL "%s (%s, branch %s)\n", "scanned head", short_head, ri->head_state,
                      atlas_safe(&r->safe, dash_if_empty(ri->current_branch)));
    }

    if (s->git_ok) {
        char short_live[16];
        atlas_short_oid(s->live_head.oid, short_live, sizeof(short_live));
        (void)fprintf(o, LABEL "%s (%s, branch %s)\n", "live head", short_live, s->live_head.state,
                      atlas_safe(&r->safe, dash_if_empty(s->live_head.branch)));
        (void)fprintf(o, LABEL "%s\n", "index drift",
                      s->never_scanned ? "unknown (never scanned)"
                                       : (s->head_drift ? "yes: rescan to refresh" : "none"));
        if (s->live_state.dirty) {
            (void)fprintf(o, LABEL "dirty: %d staged, %d unstaged, %d untracked, %d unmerged\n",
                          "worktree", s->live_state.staged, s->live_state.unstaged,
                          s->live_state.untracked, s->live_state.unmerged);
        } else {
            (void)fprintf(o, LABEL "clean\n", "worktree");
        }
    } else {
        /* git's error text is external input. */
        (void)fprintf(o, LABEL "unavailable: %s\n", "live git",
                      atlas_safe(&r->safe, atlas_buf_cstr(&s->git_error)));
    }

    (void)fprintf(o,
                  LABEL "%" PRId64 " files (%" PRId64 " deleted), %" PRId64 " commits, %" PRId64
                        " changes\n",
                  "indexed", s->counts.files_live, s->counts.files_deleted, s->counts.commits,
                  s->counts.changes);
    (void)fprintf(o, LABEL "%" PRId64 " records, %" PRId64 " scans, %" PRId64 " compile databases\n",
                  "evidence", s->counts.evidence, s->counts.scans, s->counts.compile_databases);
    return ok();
}

/* --- search / file / history --------------------------------------------- */

static atlas_status h_search_item(atlas_renderer *r, const atlas_search_hit *h, atlas_err *err) {
    (void)err;
    r->items++;
    if (strcmp(h->kind, "file") == 0) {
        /* path_text is already safe-encoded. */
        (void)fprintf(r->out, "file    %s%s  [%s]\n", h->path_text, h->deleted ? " (deleted)" : "",
                      h->evidence);
    } else {
        char short_oid[16];
        char when[ATLAS_TS_MAX];
        atlas_short_oid(h->commit_oid, short_oid, sizeof(short_oid));
        atlas_format_time(h->author_time, when, sizeof(when));
        /* The subject is written by whoever made the commit. */
        (void)fprintf(r->out, "commit  %s  %s  %s  [%s]\n", short_oid, when,
                      atlas_safe(&r->safe, h->subject), h->evidence);
    }
    return ok();
}

static atlas_status h_file(atlas_renderer *r, const atlas_file_report *f, atlas_err *err) {
    (void)err;
    FILE *o = r->out;
    const atlas_file_row *row = &f->row;
    (void)fprintf(o, "%s\n", row->path_text);
    if (!row->path_is_utf8) {
        (void)fprintf(o, LABEL "path is not valid UTF-8; shown percent-escaped\n", "encoding");
    }
    (void)fprintf(o, LABEL "%s%s\n", "type", row->file_type, row->deleted ? " (deleted)" : "");
    (void)fprintf(o, LABEL "%s\n", "language", dash_if_empty(row->language));
    (void)fprintf(o, LABEL "%s\n", "git mode", dash_if_empty(row->git_mode));
    (void)fprintf(o, LABEL "%s\n", "git index oid", dash_if_empty(row->git_index_oid));
    if (row->content_hash != NULL) {
        (void)fprintf(o, LABEL "%s:%s\n", "content hash", dash_if_empty(row->content_hash_algo),
                      row->content_hash);
    } else {
        (void)fprintf(o, LABEL "-\n", "content hash");
    }
    if (row->size_known) {
        (void)fprintf(o, LABEL "%" PRId64 " bytes\n", "size", row->size_bytes);
    } else {
        (void)fprintf(o, LABEL "-\n", "size");
    }
    (void)fprintf(o, LABEL "%s\n", "executable", yes_no(row->is_executable));
    (void)fprintf(o, LABEL "%s\n", "symlink", yes_no(row->is_symlink));
    if (row->unsafe_path) {
        (void)fprintf(o, LABEL "refused: a path component is a symlink\n", "unsafe path");
    }
    if (row->read_error != NULL) {
        (void)fprintf(o, LABEL "%s\n", "note", row->read_error);
    }
    (void)fprintf(o, LABEL "%s (scan %" PRId64 ")\n", "first seen", row->first_seen_at,
                  row->first_seen_scan_id);
    (void)fprintf(o, LABEL "%s (scan %" PRId64 ")\n", "last seen", row->last_seen_at,
                  row->last_seen_scan_id);
    (void)fprintf(o, LABEL "%" PRId64 "\n", "recorded changes", f->change_count);
    if (f->last_commit_oid != NULL) {
        char short_oid[16];
        char when[ATLAS_TS_MAX];
        atlas_short_oid(f->last_commit_oid, short_oid, sizeof(short_oid));
        atlas_format_time(f->last_commit_time, when, sizeof(when));
        (void)fprintf(o, LABEL "%s %s  %s\n", "last commit", short_oid, when,
                      atlas_safe(&r->safe, f->last_commit_subject));
    } else {
        (void)fprintf(o, LABEL "-\n", "last commit");
    }
    /* Provenance is part of the answer, not a footnote. */
    (void)fprintf(o, LABEL "SOURCE (git index and working tree), GIT (commit history)\n",
                  "evidence");
    (void)fprintf(o, LABEL "%s: Atlas records facts only and never infers why\n", "reason",
                  f->reason);
    return ok();
}

static atlas_status h_history_item(atlas_renderer *r, const atlas_history_row *h, atlas_err *err) {
    (void)err;
    char short_oid[16];
    char when[ATLAS_TS_MAX];
    atlas_short_oid(h->commit_oid, short_oid, sizeof(short_oid));
    atlas_format_time(h->commit_time, when, sizeof(when));
    r->items++;
    (void)fprintf(r->out, "%s  %s  %-10s %s", short_oid, when, h->change_type, h->path_text);
    if (h->old_path_text != NULL) {
        (void)fprintf(r->out, "  (from %s", h->old_path_text);
        if (h->score_known) {
            (void)fprintf(r->out, ", %d%% similar", h->score);
        }
        (void)fprintf(r->out, ")");
    }
    (void)fprintf(r->out, "  %s  [GIT]\n", atlas_safe(&r->safe, h->subject));
    return ok();
}

/* --- diff ---------------------------------------------------------------- */

static atlas_status h_diff_begin(atlas_renderer *r, const atlas_diff_report *rep, atlas_err *err) {
    (void)err;
    FILE *o = r->out;
    char short_head[16];
    atlas_short_oid(rep->base_head, short_head, sizeof(short_head));
    if (strcmp(rep->head_state, "unborn") == 0) {
        (void)fprintf(o, LABEL "unborn HEAD: nothing is committed yet, so every staged path is an "
                              "addition\n",
                      "base");
    } else {
        (void)fprintf(o, LABEL "%s (%s", "base", short_head, rep->head_state);
        if (rep->branch[0] != '\0') {
            (void)fprintf(o, ", branch %s", atlas_safe(&r->safe, rep->branch));
        }
        (void)fprintf(o, ")\n");
    }
    r->scope_open = false;
    r->open_scope = -1;
    r->items = 0;
    return ok();
}

static atlas_status h_diff_item(atlas_renderer *r, const atlas_diff_entry *e, atlas_err *err) {
    (void)err;
    FILE *o = r->out;
    if (!r->scope_open || r->open_scope != (int)e->scope) {
        (void)fprintf(o, "%s:\n", atlas_change_scope_name(e->scope));
        r->scope_open = true;
        r->open_scope = (int)e->scope;
    }
    r->items++;

    char counts[32];
    if (e->binary) {
        (void)snprintf(counts, sizeof(counts), "%s", "binary");
    } else if (e->counts_known) {
        (void)snprintf(counts, sizeof(counts), "+%" PRId64 " -%" PRId64, e->added, e->deleted);
    } else {
        (void)snprintf(counts, sizeof(counts), "%s", "-");
    }

    /* path_text and old_path_text are already safe-encoded. */
    (void)fprintf(o, "  %-10s %-12s %s", e->change_type, counts, e->path_text);
    if (e->old_path_text != NULL) {
        (void)fprintf(o, " (from %s", e->old_path_text);
        if (e->score_known) {
            (void)fprintf(o, ", %d%% similar", e->score);
        }
        (void)fprintf(o, ")");
    }
    if (e->scope == ATLAS_SCOPE_UNTRACKED) {
        if (e->is_directory) {
            (void)fprintf(o, "  [directory]");
        } else {
            if (e->size_known) {
                (void)fprintf(o, "  %" PRId64 " bytes", e->size_bytes);
            }
            if (e->content_hash != NULL) {
                (void)fprintf(o, "  %s:%.12s", e->content_hash_algo, e->content_hash);
            }
        }
    }
    if (e->note != NULL) {
        (void)fprintf(o, "  (%s)", e->note);
    }
    (void)fprintf(o, "\n");
    return ok();
}

static atlas_status h_diff_end(atlas_renderer *r, const atlas_diff_report *rep, atlas_err *err) {
    (void)err;
    FILE *o = r->out;
    if (rep->total_entries == 0) {
        (void)fprintf(o, "no changes\n");
    }
    (void)fprintf(o,
                  LABEL "%" PRId64 " staged, %" PRId64 " unstaged, %" PRId64 " untracked, %" PRId64
                        " unmerged\n",
                  "summary", rep->staged_count, rep->unstaged_count, rep->untracked_count,
                  rep->unmerged_count);
    if (rep->binary_changes > 0) {
        (void)fprintf(o, LABEL "%" PRId64 "\n", "binary changes", rep->binary_changes);
    }
    if (rep->truncated) {
        (void)fprintf(o, LABEL "%s\n", "truncated",
                      atlas_buf_cstr(&rep->truncated_reason));
    }
    (void)fprintf(o, LABEL "GIT (index and working tree), SOURCE (untracked file identity)\n",
                  "evidence");
    return ok();
}

/* --- A1: daemon, sync, events, service ---------------------------------- */

static atlas_status h_daemon_status(atlas_renderer *r, const atlas_daemon_status_report *rep,
                                    atlas_err *err) {
    (void)err;
    FILE *o = r->out;
    /* Running and reachable are separate facts: a daemon that owns the writer
     * lock but is not answering is a state worth seeing, and one boolean would
     * hide it. */
    (void)fprintf(o, LABEL "%s\n", "daemon",
                  rep->running ? (rep->reachable ? "running" : "running, not answering")
                               : (rep->reachable ? "answering, but not holding the writer lock"
                                                 : "not running"));
    (void)fprintf(o, LABEL "%s\n", "socket",
                  dash_if_empty(atlas_safe(&r->safe, atlas_buf_cstr(&rep->socket_path))));
    if (rep->lock_holder.len > 0) {
        (void)fprintf(o, LABEL "%s\n", "writer lock",
                      atlas_safe(&r->safe, atlas_buf_cstr(&rep->lock_holder)));
    }
    if (rep->record.present) {
        (void)fprintf(o, LABEL "%lld\n", "recorded pid", (long long)rep->record.pid);
        (void)fprintf(o, LABEL "%s\n", "started", dash_if_empty(rep->record.started_at));
        (void)fprintf(o, LABEL "%s\n", "heartbeat", dash_if_empty(rep->record.last_heartbeat_at));
    }
    (void)fprintf(o, LABEL "%d\n", "protocol", rep->protocol_version);
    (void)fprintf(o, LABEL "%" PRId64 "\n", "repositories", rep->repo_count);
    (void)fprintf(o, LABEL "%" PRId64 "\n", "watching", rep->watched_repos);
    (void)fprintf(o, LABEL "%" PRId64 "\n", "degraded", rep->degraded_repos);
    (void)fprintf(o, LABEL "%" PRId64 "\n", "event gaps", rep->repos_with_gap);
    if (rep->repos_with_gap > 0) {
        (void)fprintf(o,
                      "\n%" PRId64
                      " repository/repositories are known to have missed filesystem events.\n"
                      "Their indexes are NOT current until a full reconciliation completes.\n",
                      rep->repos_with_gap);
    }
    return ok();
}

static atlas_status h_daemon_ping(atlas_renderer *r, bool reachable, const char *socket_path,
                                  const char *detail, atlas_err *err) {
    (void)err;
    (void)fprintf(r->out, LABEL "%s\n", "socket",
                  dash_if_empty(atlas_safe(&r->safe, socket_path)));
    (void)fprintf(r->out, LABEL "%s\n", "daemon", reachable ? "responding" : "not responding");
    if (detail != NULL && detail[0] != '\0') {
        (void)fprintf(r->out, LABEL "%s\n", "detail", atlas_safe(&r->safe, detail));
    }
    return ok();
}

static atlas_status h_repo_state(atlas_renderer *r, const atlas_repo_state_report *rep,
                                 atlas_err *err) {
    (void)err;
    FILE *o = r->out;
    (void)fprintf(o, LABEL "%s\n", "repository", rep->repo.name);
    (void)fprintf(o, LABEL "%s\n", "root", atlas_buf_cstr(&rep->repo.root_path_text));
    (void)fprintf(o, LABEL "%s\n", "watch state", atlas_watch_state_name(rep->state.watch_state));
    (void)fprintf(o, LABEL "%" PRId64 "\n", "watched dirs", rep->state.watched_dirs);
    (void)fprintf(o, LABEL "%" PRId64 " (in flight %" PRId64 ")\n", "generation",
                  rep->state.last_complete_generation, rep->state.generation);
    (void)fprintf(o, LABEL "%s\n", "last complete", dash_if_empty(rep->state.last_complete_at));
    (void)fprintf(o, LABEL "%" PRId64 "\n", "event cursor", rep->event_cursor);
    /* The claim, and when it is false, why. Never "current" with a known gap. */
    (void)fprintf(o, LABEL "%s\n", "index current", yes_no(rep->index_current));
    if (!rep->index_current && rep->not_current_reason != NULL) {
        (void)fprintf(o, LABEL "%s\n", "reason", rep->not_current_reason);
    }
    if (rep->state.watch_detail.len > 0) {
        (void)fprintf(o, LABEL "%s\n", "watch detail",
                      atlas_safe(&r->safe, atlas_buf_cstr(&rep->state.watch_detail)));
    }
    if (rep->state.last_error.len > 0) {
        (void)fprintf(o, LABEL "%s\n", "last error",
                      atlas_safe(&r->safe, atlas_buf_cstr(&rep->state.last_error)));
    }
    return ok();
}

/* Appends the structural stage's counters to a sync report.
 *
 * Part of `sync` rather than a command of its own, because the structural stage
 * is part of the pass: a reader asking whether an unchanged pass parsed nothing
 * is asking about the same pass the file counters describe. */
static void h_code_summary(atlas_renderer *r, const atlas_reconcile_summary *s) {
    FILE *o = r->out;
    if (!s->code_ran) {
        (void)fprintf(o, LABEL "%s\n", "structural", "skipped");
        return;
    }
    (void)fprintf(o, LABEL "%lld selected, %lld parsed, %lld removed\n", "structural",
                  (long long)s->code.files_selected, (long long)s->code.files_parsed,
                  (long long)s->code.files_removed);
    (void)fprintf(o, LABEL "%lld symbols, %lld relations, %lld resolved\n", "structural facts",
                  (long long)s->code.symbols_written, (long long)s->code.relations_written,
                  (long long)s->code.relations_resolved);
    if (s->code.compile_db_present) {
        (void)fprintf(o, LABEL "%lld units\n", "compile database",
                      (long long)s->code.compile_units);
    }
    if (s->code.resolve_fallback) {
        (void)fprintf(o, LABEL "%s\n", "resolution",
                      "re-resolved the whole repository (too many names changed)");
    }
    if (s->code.degraded && s->code.degraded_reason != NULL) {
        (void)fprintf(o, LABEL "%s\n", "structural degraded", s->code.degraded_reason);
    }
    if (s->code.truncated && s->code.truncated_reason != NULL) {
        (void)fprintf(o, LABEL "%s\n", "structural truncated", s->code.truncated_reason);
    }
}

static atlas_status h_sync(atlas_renderer *r, const char *repo, const atlas_sync_report *rep,
                           atlas_err *err) {
    (void)err;
    FILE *o = r->out;
    (void)fprintf(o, LABEL "%s\n", "repository", repo);
    (void)fprintf(o, LABEL "%s\n", "performed by", rep->via_daemon ? "the daemon" : "this command");
    if (rep->via_daemon) {
        (void)fprintf(o, LABEL "%" PRId64 "\n", "sync sequence", rep->sync_seq);
        (void)fprintf(o, LABEL "%s\n", "state",
                      rep->completed ? "completed" : (rep->waited ? "still running" : "queued"));
        if (rep->completed) {
            (void)fprintf(o, LABEL "%" PRId64 "\n", "generation", rep->generation);
        }
        return ok();
    }
    const atlas_reconcile_summary *s = &rep->summary;
    if (!s->published) {
        (void)fprintf(o, LABEL "%s\n", "state",
                      "abandoned: HEAD moved during the pass, so nothing was committed");
        return ok();
    }
    (void)fprintf(o, LABEL "%" PRId64 "\n", "generation", s->generation);
    (void)fprintf(o, LABEL "%" PRId64 "\n", "files examined", s->files_examined);
    /* The number that shows the pass was incremental. */
    (void)fprintf(o, LABEL "%" PRId64 "\n", "content read", s->files_hashed);
    (void)fprintf(o, LABEL "%" PRId64 "\n", "unchanged by id", s->files_identity_hit);
    (void)fprintf(o, LABEL "%s\n", "content verified", yes_no(s->content_verified));
    if (s->files_dirty_forced > 0) {
        (void)fprintf(o, LABEL "%" PRId64 "\n", "read on event", s->files_dirty_forced);
    }
    if (s->files_racy > 0) {
        (void)fprintf(o, LABEL "%" PRId64 "\n", "read (racy stamp)", s->files_racy);
    }
    (void)fprintf(o, LABEL "+%" PRId64 " ~%" PRId64 " -%" PRId64 "\n", "changes", s->files_added,
                  s->files_modified, s->files_deleted);
    (void)fprintf(o, LABEL "%" PRId64 "\n", "untracked found", s->untracked_discovered);
    (void)fprintf(o, LABEL "%" PRId64 "\n", "ignored paths", s->ignored_paths);
    (void)fprintf(o, LABEL "%" PRId64 " new of %" PRId64 " walked\n", "commits",
                  s->commits_ingested, s->commits_seen);
    if (s->branch_rewrite) {
        (void)fprintf(o, LABEL "%s\n", "history",
                      "the previously ingested tip is unreachable; history was replayed in full");
    }
    (void)fprintf(o, LABEL "%" PRId64 "\n", "events recorded", s->events_appended);
    (void)fprintf(o, LABEL "%" PRId64 "\n", "write batches", s->batches_written);
    (void)fprintf(o, LABEL "%" PRId64 " ms\n", "duration", s->duration_ms);
    if (s->truncated) {
        (void)fprintf(o, LABEL "%s\n", "truncated", atlas_buf_cstr(&s->truncated_reason));
    }
    h_code_summary(r, s);
    return ok();
}

static atlas_status h_event_item(atlas_renderer *r, const atlas_event_row *row, atlas_err *err) {
    (void)err;
    r->items++;
    /* path_text is already in the safe encoding; detail comes from git and the
     * kernel, so it is encoded here. */
    (void)fprintf(r->out, "  %-10" PRId64 " %-16s %-19s %s%s%s\n", row->id, row->kind,
                  row->created_at, dash_if_empty(row->path_text),
                  row->detail != NULL ? "  " : "",
                  row->detail != NULL ? atlas_safe(&r->safe, row->detail) : "");
    return ok();
}

static atlas_status h_events_end(atlas_renderer *r, int64_t cursor, bool more, atlas_err *err) {
    (void)err;
    (void)fprintf(r->out, LABEL "%" PRId64 "\n", "next cursor", cursor);
    if (more) {
        /* Never a silent truncation: the caller is told to ask again. */
        (void)fprintf(r->out, LABEL "%s\n", "more",
                      "yes — re-run with --since <next cursor> for the rest");
    }
    return ok();
}

static atlas_status h_unit_text(atlas_renderer *r, const char *text, atlas_err *err) {
    (void)err;
    /* Verbatim: this is exactly the file `atlas service install` would write, so
     * a user can review it or redirect it without any transformation. */
    (void)fputs(text, r->out);
    return ok();
}

static atlas_status h_unit_install(atlas_renderer *r, const atlas_unit_install_report *rep,
                                   bool uninstall, atlas_err *err) {
    (void)err;
    FILE *o = r->out;
    (void)fprintf(o, LABEL "%s\n", "unit path", atlas_buf_cstr(&rep->path));
    if (uninstall) {
        (void)fprintf(o, LABEL "%s\n", "result",
                      rep->removed ? "removed" : (rep->was_absent ? "already absent" : "unchanged"));
        (void)fprintf(o, "\nRun: systemctl --user daemon-reload\n");
        return ok();
    }
    if (rep->created_dir) {
        (void)fprintf(o, LABEL "%s\n", "created", atlas_buf_cstr(&rep->dir));
    }
    (void)fprintf(o, LABEL "%s\n", "result",
                  rep->unchanged ? "already up to date"
                                 : (rep->replaced_existing ? "replaced the previous unit"
                                                           : "written"));
    (void)fprintf(o, LABEL "%s\n", "mode", "0600");
    (void)fprintf(o, LABEL "%s\n", "service", "not enabled and not started");
    (void)fprintf(o,
                  "\nAtlas wrote the unit and did nothing else. To start it:\n"
                  "  systemctl --user daemon-reload\n"
                  "  systemctl --user enable --now atlas\n"
                  "To keep it running after you log out of SSH:\n"
                  "  sudo loginctl enable-linger $USER\n");
    return ok();
}

static void h_check(FILE *o, const char *label, bool ok) {
    (void)fprintf(o, LABEL "%s\n", label, ok ? "yes" : "no");
}

static atlas_status h_integrate(atlas_renderer *r, const atlas_integrate_report *rep,
                                const char *action, const char *commands, atlas_err *err) {
    (void)err;
    FILE *o = r->out;
    if (commands != NULL) {
        (void)fputs(commands, o);
        return ok();
    }
    (void)fprintf(o, LABEL "%s\n", "action", action);
    /* Paths here come from the environment and from this process's own
     * executable, so they are encoded like every other value from outside. */
    (void)fprintf(o, LABEL "%s\n", "executable", atlas_safe(&r->safe, atlas_buf_cstr(&rep->exe)));
    (void)fprintf(o, LABEL "%s (%s)\n", "plugin",
                  rep->plugin_found ? atlas_safe(&r->safe, atlas_buf_cstr(&rep->plugin_dir))
                                    : "not found",
                  atlas_plugin_source_name(rep->plugin_source));
    (void)fprintf(o, LABEL "%s\n", "marketplace",
                  rep->marketplace_dir.len > 0
                      ? atlas_safe(&r->safe, atlas_buf_cstr(&rep->marketplace_dir))
                      : "not found");
    /* The state a user actually needs: installed and enabled, installed and
     * disabled, development-only, or absent. These are fixed by different
     * commands, so collapsing them would send somebody to the wrong one. */
    (void)fprintf(o, LABEL "%s\n", "claude plugin", atlas_claude_state_name(rep->claude_state));
    if (rep->installed_id.len > 0) {
        (void)fprintf(o, LABEL "%s (scope %s)\n", "installed as",
                      atlas_safe(&r->safe, atlas_buf_cstr(&rep->installed_id)),
                      rep->installed_scope.len > 0 ? atlas_buf_cstr(&rep->installed_scope)
                                                   : "unknown");
        (void)fprintf(o, LABEL "%s\n", "plugin cache",
                      atlas_safe(&r->safe, atlas_buf_cstr(&rep->installed_path)));
    }
    h_check(o, "marketplace known", rep->marketplace_registered);
    (void)fprintf(o, LABEL "%s\n", "config",
                  atlas_safe(&r->safe, atlas_buf_cstr(&rep->config_path)));
    h_check(o, "config present", rep->config_present);
    h_check(o, "manifest", rep->manifest_ok);
    h_check(o, "hooks.json", rep->hooks_ok);
    h_check(o, ".mcp.json", rep->mcp_ok);
    h_check(o, "skill", rep->skill_ok);
    h_check(o, "launchers", rep->launcher_ok);
    (void)fprintf(o, LABEL "%lld\n", "hook events", (long long)rep->hook_events);
    (void)fprintf(o, LABEL "%lld\n", "mcp tools", (long long)rep->mcp_tools);
    (void)fprintf(o, LABEL "%s\n", "mcp self-test",
                  atlas_safe(&r->safe, atlas_buf_cstr(&rep->mcp_selftest_detail)));
    (void)fprintf(o, LABEL "%s\n", "socket",
                  atlas_safe(&r->safe, atlas_buf_cstr(&rep->socket_path)));
    h_check(o, "daemon", rep->daemon_reachable);
    /* Reported, never created. */
    (void)fprintf(o, LABEL "%s\n", "index", rep->index_present ? "present" : "absent");
    if (rep->wrote_config) {
        (void)fprintf(o, LABEL "%s\n", "wrote", "the integration record");
    }
    if (rep->removed_config) {
        (void)fprintf(o, LABEL "%s\n", "removed",
                      "the integration record (the Atlas index was not touched)");
    }
    if (rep->problems.len > 0) {
        (void)fprintf(o, "\nproblems:\n");
        const char *p = atlas_buf_cstr(&rep->problems);
        while (*p != '\0') {
            const char *nl = strchr(p, '\n');
            size_t n = (nl != NULL) ? (size_t)(nl - p) : strlen(p);
            (void)fprintf(o, "  - %s\n", atlas_safe_n(&r->safe, p, n));
            p = (nl != NULL) ? nl + 1 : p + n;
        }
    } else {
        (void)fprintf(o, LABEL "%s\n", "result", "ready");
    }
    return ok();
}

/* --- A3: structural code intelligence ------------------------------------
 *
 * Paths and symbol names printed here are already in the safe encoding — they
 * were encoded when they were stored — so they are printed as-is. Everything
 * else is a fixed vocabulary or an integer. */

static atlas_status h_code_status(atlas_renderer *r, const atlas_code_status_report *rep,
                                  atlas_err *err) {
    (void)err;
    FILE *o = r->out;
    (void)fprintf(o, LABEL "%s\n", "repo", rep->repo.name);
    (void)fprintf(o, LABEL "%s\n", "index current", yes_no(rep->file_index_current));
    /* The claim a caller acts on, and the reason when it cannot be made. */
    (void)fprintf(o, LABEL "%s\n", "code index current", yes_no(rep->code_index_current));
    if (!rep->code_index_current && rep->not_current_reason != NULL) {
        (void)fprintf(o, LABEL "%s\n", "not current", rep->not_current_reason);
    }
    (void)fprintf(o, LABEL "%lld (files %lld)\n", "code generation",
                  (long long)rep->code_state.last_complete_generation,
                  (long long)rep->file_state.last_complete_generation);
    (void)fprintf(o, LABEL "%lld\n", "files indexed", (long long)rep->code_state.files_indexed);
    (void)fprintf(o, LABEL "%lld\n", "parsed last pass",
                  (long long)rep->code_state.files_parsed_last);
    (void)fprintf(o, LABEL "%lld\n", "symbols", (long long)rep->code_state.symbols);
    (void)fprintf(o, LABEL "%lld\n", "relations", (long long)rep->code_state.relations);
    /* Reported next to the totals rather than buried, because how much of the
     * graph is inferred is the first thing a reader should weigh. */
    (void)fprintf(o, LABEL "%lld\n", "ambiguous", (long long)rep->code_state.ambiguous);
    (void)fprintf(o, LABEL "%lld\n", "unresolved", (long long)rep->code_state.unresolved);
    (void)fprintf(o, LABEL "%s\n", "compile database",
                  rep->code_state.compile_db_present ? "present" : "absent");
    if (rep->code_state.compile_db_present) {
        (void)fprintf(o, LABEL "%lld\n", "compile units",
                      (long long)rep->code_state.compile_units);
        (void)fprintf(o, LABEL "%lld\n", "entries dropped",
                      (long long)rep->code_state.compile_entries_dropped);
    }
    if (rep->code_state.degraded) {
        (void)fprintf(o, LABEL "%s\n", "degraded",
                      atlas_safe(&r->safe, atlas_buf_cstr(&rep->code_state.degraded_reason)));
    }
    (void)fprintf(o, LABEL "%s\n", "last complete",
                  dash_if_empty(rep->code_state.last_complete_at));
    /* Printed as-is: both halves are constants from an Atlas binary, not values
     * a repository or a model could have chosen. */
    (void)fprintf(o, LABEL "%s v%lld", "analyzer",
                  dash_if_empty(atlas_buf_cstr(&rep->code_state.analyzer_name)),
                  (long long)rep->code_state.analyzer_version);
    if (!atlas_code_analyzer_matches(&rep->code_state)) {
        (void)fprintf(o, "  (this binary produces %s v%d)", ATLAS_CODE_ANALYZER_ID,
                      ATLAS_CODE_ANALYZER_VERSION);
    }
    (void)fprintf(o, "\n");
    return ok();
}

static atlas_status h_code_file(atlas_renderer *r, const atlas_code_file_report *rep,
                                atlas_err *err) {
    (void)err;
    FILE *o = r->out;
    (void)fprintf(o, LABEL "%s\n", "path", atlas_buf_cstr(&rep->path_text));
    if (!rep->indexed) {
        (void)fprintf(o, LABEL "%s\n", "structural", "not indexed");
        (void)fprintf(o, LABEL "%s\n", "reason",
                      "Atlas extracts structure from C sources, headers and included fragments "
                      "only");
        return ok();
    }
    (void)fprintf(o, LABEL "%s\n", "language", rep->language);
    (void)fprintf(o, LABEL "%s\n", "parse", rep->parse_status);
    if (rep->parse_detail.len > 0) {
        (void)fprintf(o, LABEL "%s\n", "parse detail",
                      atlas_safe(&r->safe, atlas_buf_cstr(&rep->parse_detail)));
    }
    if (rep->truncated) {
        (void)fprintf(o, LABEL "%s\n", "truncated",
                      atlas_safe(&r->safe, atlas_buf_cstr(&rep->truncated_reason)));
    }
    for (size_t i = 0; i < rep->role_count; i++) {
        /* The basis travels with the role. A file under `tests/` is *named* like
         * a test; that is a fact about the path, not proof about the file. */
        (void)fprintf(o, LABEL "%s (basis: %s)\n", i == 0 ? "role" : "", rep->roles[i].role,
                      rep->roles[i].basis);
    }
    (void)fprintf(o, LABEL "%s\n", "content hash", dash_if_empty(rep->content_hash));
    (void)fprintf(o, LABEL "%lld\n", "generation", (long long)rep->generation);
    (void)fprintf(o, LABEL "%lld\n", "symbols", (long long)rep->symbol_count);
    (void)fprintf(o, LABEL "%lld\n", "includes", (long long)rep->include_count);
    (void)fprintf(o, LABEL "%lld\n", "call candidates", (long long)rep->occurrence_count);
    (void)fprintf(o, LABEL "%lld\n", "ambiguous", (long long)rep->ambiguous);
    (void)fprintf(o, LABEL "%lld\n", "unresolved", (long long)rep->unresolved);
    (void)fprintf(o, LABEL "%lld\n", "lines", (long long)rep->lines);
    (void)fprintf(o, LABEL "%s\n", "code index current", yes_no(rep->code_index_current));
    if (!rep->code_index_current && rep->not_current_reason != NULL) {
        (void)fprintf(o, LABEL "%s\n", "not current", rep->not_current_reason);
    }
    /* A0's answer, unchanged by A3. Structure is not a reason. */
    (void)fprintf(o, LABEL "%s\n", "reason", "UNKNOWN: nobody recorded why");
    return ok();
}

static atlas_status h_code_symbol_item(atlas_renderer *r, const atlas_code_symbol_row *row,
                                       atlas_err *err) {
    (void)err;
    (void)fprintf(r->out, "  %-9s %-10s %s:%lld  %s%s\n", row->kind, row->linkage, row->path_text,
                  (long long)row->line, row->name_text,
                  row->is_definition ? "" : " (declaration)");
    return ok();
}

static atlas_status h_code_edge_item(atlas_renderer *r, const atlas_code_edge_row *row,
                                     atlas_err *err) {
    (void)err;
    const char *target = row->dst_path_text != NULL ? row->dst_path_text : row->dst_name_text;
    if (target == NULL) {
        target = "-";
    }
    const char *from = row->src_path_text != NULL ? row->src_path_text : "";
    (void)fprintf(r->out, "  %-14s %-16s %s%s%s", row->resolution, row->kind, from,
                  from[0] != '\0' ? " -> " : "", target);
    if (row->candidate_count > 1) {
        (void)fprintf(r->out, "  [%lld candidates]", (long long)row->candidate_count);
    }
    if (row->detail != NULL) {
        (void)fprintf(r->out, "  (%s)", row->detail);
    }
    (void)fprintf(r->out, "\n");
    return ok();
}

static atlas_status h_code_walk_item(atlas_renderer *r, const atlas_code_walk_row *row,
                                     atlas_err *err) {
    (void)err;
    /* The path is printed with the candidate, always. An impact result without
     * the chain that produced it is an assertion. */
    (void)fprintf(r->out, "  %-14s d%-2lld %-8s %s  <- %s via %s\n", row->resolution,
                  (long long)row->depth, row->node_kind, row->label,
                  row->via_label[0] != '\0' ? row->via_label : "(start)", row->edge_kind);
    return ok();
}

static atlas_status h_code_walk_end(atlas_renderer *r, const atlas_code_walk_summary *sum,
                                    atlas_err *err) {
    (void)err;
    FILE *o = r->out;
    (void)fprintf(o, LABEL "%lld exact, %lld unique-lexical, %lld ambiguous, %lld unresolved\n",
                  "candidates", (long long)sum->exact, (long long)sum->unique_lexical,
                  (long long)sum->ambiguous, (long long)sum->unresolved);
    if (sum->truncated) {
        (void)fprintf(o, LABEL "%s\n", "truncated",
                      sum->truncated_reason != NULL ? sum->truncated_reason : "a ceiling was reached");
    }
    (void)fprintf(o, LABEL "%s\n", "note",
                  "graph paths, not predictions: Atlas is not a compiler");
    return ok();
}

static atlas_status h_code_list_begin(atlas_renderer *r, const char *key, atlas_err *err) {
    (void)err;
    r->items = 0;
    r->in_list = true;
    (void)fprintf(r->out, "\n%s:\n", key);
    return ok();
}

static atlas_status h_code_list_end(atlas_renderer *r, const char *key, const char *singular,
                                    const char *plural, int64_t count, bool more, atlas_err *err) {
    (void)key;
    (void)err;
    r->in_list = false;
    if (count == 0) {
        (void)fprintf(r->out, "  no %s\n", plural);
    } else {
        (void)fprintf(r->out, "  %" PRId64 " %s%s\n", count, count == 1 ? singular : plural,
                      more ? " (more available; raise --limit)" : "");
    }
    return ok();
}


/* --- A4: decision documents ---------------------------------------------
 *
 * Every string here arrives already safe-encoded from the service layer.
 * Decision prose is untrusted whatever its status — approval changes a
 * record's state, not the nature of its bytes — so the encoding happens once,
 * in `service_decision.c`, and is not repeated here. Double-encoding would
 * turn a `%` in somebody's decision into `%25` on screen. */

static const char *dash(const atlas_buf *b) {
    return b->len > 0 ? atlas_buf_cstr(b) : "-";
}

static atlas_status h_decision_item(atlas_renderer *r, const atlas_decision_summary *s,
                                    atlas_err *err) {
    (void)err;
    r->items++;
    (void)fprintf(r->out, "  %-26s  %-10s  rev %" PRId64 "/%" PRId64 "  %s\n",
                  atlas_buf_cstr(&s->uid), atlas_buf_cstr(&s->status), s->revision_no,
                  s->latest_revision_no, atlas_buf_cstr(&s->title));
    if (s->superseded_by.len > 0) {
        (void)fprintf(r->out, "      superseded by %s\n", atlas_buf_cstr(&s->superseded_by));
    }
    return ok();
}

static void h_decision_body(atlas_renderer *r, const char *heading, const atlas_buf *body) {
    if (body->len == 0) {
        return;
    }
    (void)fprintf(r->out, "\n%s:\n", heading);
    /* Indented line by line rather than printed as one block, so a multi-line
     * body cannot be mistaken for the surrounding report's own output. */
    const char *p = body->data;
    size_t n = body->len;
    size_t start = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || p[i] == '\n') {
            (void)fprintf(r->out, "  %.*s\n", (int)(i - start), p + start);
            start = i + 1u;
        }
    }
}

static atlas_status h_decision_show(atlas_renderer *r, const atlas_decision_document *d,
                                    atlas_err *err) {
    (void)err;
    const atlas_decision_summary *s = &d->summary;
    (void)fprintf(r->out, "decision:     %s\n", atlas_buf_cstr(&s->uid));
    (void)fprintf(r->out, "repository:   %s\n", atlas_buf_cstr(&d->repo));
    (void)fprintf(r->out, "status:       %s\n", atlas_buf_cstr(&s->status));
    (void)fprintf(r->out, "revision:     %" PRId64 " of %" PRId64 " (%s)\n", s->revision_no,
                  s->latest_revision_no, dash(&s->revision_state));
    (void)fprintf(r->out, "content hash: %s\n", dash(&s->content_hash));
    (void)fprintf(r->out, "proposed by:  %s\n", dash(&s->proposed_by));
    (void)fprintf(r->out, "scope:        %s\n", dash(&d->scope));
    if (d->basis_head.len > 0) {
        (void)fprintf(r->out, "basis commit: %s\n", atlas_buf_cstr(&d->basis_head));
    }
    /* The identity this revision *captured*, which is immutable and hashed —
     * not the document's current attachment identity, which is neither. */
    (void)fprintf(r->out, "repo identity: %s\n",
                  d->basis_repo_identity.len > 0 ? atlas_buf_cstr(&d->basis_repo_identity)
                                                 : "not captured (no ingested history at the time)");
    if (s->superseded_by.len > 0) {
        (void)fprintf(r->out, "superseded by: %s\n", atlas_buf_cstr(&s->superseded_by));
    }
    if (d->imported_from_a2_decision > 0) {
        (void)fprintf(r->out, "promoted from A2 proposal %" PRId64 "\n",
                      d->imported_from_a2_decision);
    }
    if (d->session_unbound) {
        /* A gap, stated. A2's rule applies unchanged: a record that could not be
         * attributed exactly is stored unattached rather than attached to a
         * neighbour, and saying so is what makes the gap repairable. */
        (void)fprintf(r->out, "session:      unattached (%s)\n", dash(&d->unbound_reason));
    }
    (void)fprintf(r->out, "created:      %s\n", dash(&s->created_at));

    (void)fprintf(r->out, "\ntitle:\n  %s\n", atlas_buf_cstr(&s->title));
    h_decision_body(r, "context", &d->context_text);
    h_decision_body(r, "decision", &d->decision_text);
    h_decision_body(r, "rationale", &d->rationale_text);
    if (d->alternative_count > 0) {
        (void)fprintf(r->out, "\nalternatives considered:\n");
        for (size_t i = 0; i < d->alternative_count; i++) {
            (void)fprintf(r->out, "  %zu. %s\n", i + 1u, atlas_buf_cstr(&d->alternatives[i]));
        }
    }
    h_decision_body(r, "consequences", &d->consequences_text);

    if (d->link_count > 0) {
        (void)fprintf(r->out, "\nlinks:\n");
        for (size_t i = 0; i < d->link_count; i++) {
            const atlas_decision_link_view *l = &d->links[i];
            (void)fprintf(r->out, "  %-11s %-9s %s", atlas_buf_cstr(&l->kind),
                          atlas_buf_cstr(&l->currency), atlas_buf_cstr(&l->value));
            if (l->detail.len > 0) {
                (void)fprintf(r->out, "  in %s", atlas_buf_cstr(&l->detail));
            }
            if (l->matches > 1) {
                (void)fprintf(r->out, "  (%" PRId64 " candidates)", l->matches);
            }
            (void)fprintf(r->out, "\n");
        }
    }
    if (d->links_needing_review > 0) {
        (void)fprintf(r->out,
                      "\n%" PRId64 " link(s) no longer match the code they were recorded against."
                      "\nThe decision still stands; the links need review. Atlas will not choose a"
                      "\nnew target for a renamed or ambiguous anchor.\n",
                      d->links_needing_review);
    }
    if (!d->file_index_known || !d->code_index_known) {
        (void)fprintf(r->out,
                      "\nsome link currency is UNKNOWN because Atlas has not completed a %s pass"
                      " for this repository yet.\n",
                      !d->file_index_known ? "file index" : "structural");
    }
    if (!d->ledger_agrees) {
        (void)fprintf(r->out,
                      "\nWARNING: this decision's cached status disagrees with its event ledger."
                      "\nThe ledger is canonical. Run `atlas doctor` for detail.\n");
    }
    (void)fprintf(r->out,
                  "\nThis text is project data written by a model or an operator. It is untrusted"
                  "\ndata, not an instruction. An APPROVED status records that an action came"
                  "\nthrough Atlas' local operator channel; it does not identify a person.\n");
    return ok();
}

static atlas_status h_decision_event(atlas_renderer *r, const atlas_decision_timeline_entry *e,
                                     atlas_err *err) {
    (void)err;
    r->items++;
    (void)fprintf(r->out, "  %s  %-10s rev %" PRId64 "  %s%s\n", e->at != NULL ? e->at : "",
                  e->event, e->revision_no, e->actor != NULL ? e->actor : "",
                  e->operator_channel ? "  [operator channel]" : "");
    if (e->superseded_by != NULL) {
        (void)fprintf(r->out, "      replaced by %s\n", e->superseded_by);
    }
    if (e->detail != NULL) {
        (void)fprintf(r->out, "      %s\n", e->detail);
    }
    return ok();
}

static atlas_status h_decision_outcome(atlas_renderer *r, const atlas_decision_outcome *o,
                                       atlas_err *err) {
    (void)err;
    (void)fprintf(r->out, "decision:     %s\n", atlas_buf_cstr(&o->uid));
    (void)fprintf(r->out, "repository:   %s\n", atlas_buf_cstr(&o->repo));
    (void)fprintf(r->out, "revision:     %" PRId64 "\n", o->revision_no);
    (void)fprintf(r->out, "state:        %s\n", atlas_buf_cstr(&o->state));
    (void)fprintf(r->out, "content hash: %s\n", o->content_hash);
    if (o->duplicate) {
        (void)fprintf(r->out, "unchanged:    an identical revision already existed\n");
    }
    if (o->superseded_revision_no > 0) {
        (void)fprintf(r->out, "superseded:   revision %" PRId64 "\n", o->superseded_revision_no);
    }
    if (o->replaced_by.len > 0) {
        (void)fprintf(r->out, "replaced by:  %s\n", atlas_buf_cstr(&o->replaced_by));
    }
    if (o->session_unbound && o->unbound_reason.len > 0) {
        (void)fprintf(r->out, "session:      unattached (%s)\n", atlas_buf_cstr(&o->unbound_reason));
    }
    if (o->operator_confirmed) {
        /* Printed every time, and worded so it cannot be read as an identity
         * claim. This is the moment somebody could believe Atlas knows who they
         * are. */
        (void)fprintf(r->out,
                      "\nRecorded as LOCAL_OPERATOR_CONFIRMED: the action came through Atlas'"
                      "\nlocal operator channel. This does not identify a person, does not prove"
                      "\na person was present, and is not a signature.\n");
    } else {
        (void)fprintf(r->out,
                      "\nThis is a proposal, not an approval. Approve it with"
                      "\n  atlas decision approve %s %s\n"
                      "on a terminal.\n",
                      atlas_buf_cstr(&o->repo), atlas_buf_cstr(&o->uid));
    }
    return ok();
}

static atlas_status h_decision_counts(atlas_renderer *r, const atlas_decision_counts *c,
                                      atlas_err *err) {
    (void)err;
    (void)fprintf(r->out,
                  "\ntotals: %" PRId64 " proposed, %" PRId64 " approved, %" PRId64
                  " rejected, %" PRId64 " superseded\n",
                  c->proposed, c->approved, c->rejected, c->superseded);
    return ok();
}


static atlas_status h_decision_ledger(atlas_renderer *r, bool agrees, atlas_err *err) {
    (void)err;
    if (!agrees) {
        (void)fprintf(r->out,
                      "\nWARNING: this decision's cached status disagrees with its event ledger."
                      "\nThe ledger is canonical and Atlas does not repair the cache silently."
                      "\nRun `atlas doctor` for detail.\n");
    }
    return ok();
}

/* --- A5 ------------------------------------------------------------------
 *
 * Paths here were typed by the operator, so they are bytes and are encoded like
 * any other path. Digests, verdict words and class names come from fixed
 * vocabularies; the retention reasons are string literals in
 * src/core/service_maintenance.c. Nothing from a repository or a model reaches
 * these functions. */

static void h_verify_body(atlas_renderer *r, const atlas_backup_verify_report *rep,
                          const char *indent) {
    FILE *o = r->out;
    (void)fprintf(o, "%sverdict:      %s\n", indent, atlas_backup_verdict_name(rep->verdict));
    (void)fprintf(o, "%ssize:         %" PRId64 " bytes\n", indent, rep->size_bytes);
    (void)fprintf(o, "%ssha256:       %s\n", indent, rep->sha256);
    (void)fprintf(o, "%sschema:       %d (this build supports %d)\n", indent, rep->schema_version,
                  rep->expected_schema_version);
    (void)fprintf(o, "%sintegrity:    %s\n", indent,
                  atlas_safe(&r->safe, atlas_buf_cstr(&rep->integrity)));
    (void)fprintf(o, "%sforeign keys: %s\n", indent,
                  atlas_safe(&r->safe, atlas_buf_cstr(&rep->foreign_key_check)));
    (void)fprintf(o, "%stables:       %" PRId64 " of %" PRId64 " required\n", indent,
                  rep->tables_present, rep->tables_required);
    (void)fprintf(o, "%srepositories: %" PRId64 "\n", indent, rep->repo_count);
    (void)fprintf(o,
                  "%sdecisions:    %" PRId64 " revision(s) rehashed, %" PRId64
                  " content mismatch(es), %" PRId64 " ledger disagreement(s)\n",
                  indent, rep->revisions_rehashed, rep->revisions_corrupt, rep->ledger_mismatched);
    if (rep->problems.len > 0) {
        (void)fprintf(o, "%sproblems:\n", indent);
        const char *p = atlas_buf_cstr(&rep->problems);
        while (*p != '\0') {
            const char *nl = strchr(p, '\n');
            size_t len = (nl != NULL) ? (size_t)(nl - p) : strlen(p);
            (void)fprintf(o, "%s  - %s\n", indent, atlas_safe_n(&r->safe, p, len));
            if (nl == NULL) {
                break;
            }
            p = nl + 1;
        }
    }
}

static atlas_status h_backup_created(atlas_renderer *r, const atlas_backup_report *rep,
                                     atlas_err *err) {
    (void)err;
    FILE *o = r->out;
    (void)fprintf(o, "backup:  %s\n", atlas_safe(&r->safe, atlas_buf_cstr(&rep->path)));
    (void)fprintf(o, "source:  %s%s\n", atlas_safe(&r->safe, atlas_buf_cstr(&rep->source_db_path)),
                  rep->source_online ? "  (taken while a daemon held the writer lock)" : "");
    (void)fprintf(o, "size:    %" PRId64 " bytes (%" PRId64 " pages of %" PRId64 ")\n",
                  rep->size_bytes, rep->page_count, rep->page_size);
    (void)fprintf(o, "sha256:  %s\n", rep->sha256);
    (void)fprintf(o, "atlas:   %s, schema %d\n", rep->atlas_version, rep->schema_version);
    (void)fprintf(o,
                  "note:    the database only. Configuration, the runtime socket and the service "
                  "unit are not database content and are not in this file. It is not encrypted "
                  "and not signed.\n");
    return ok();
}

static atlas_status h_backup_verified(atlas_renderer *r, const atlas_backup_verify_report *rep,
                                      const char *key, atlas_err *err) {
    (void)err;
    (void)fprintf(r->out, "%s: %s\n", key != NULL ? key : "backup",
                  atlas_safe(&r->safe, atlas_buf_cstr(&rep->path)));
    h_verify_body(r, rep, "  ");
    (void)fprintf(r->out, "status: %s\n", rep->ok ? "ok" : "NOT USABLE");
    return ok();
}

static atlas_status h_backup_restored(atlas_renderer *r, const atlas_backup_restore_report *rep,
                                      atlas_err *err) {
    (void)err;
    FILE *o = r->out;
    (void)fprintf(o, "restored: %s\n", atlas_safe(&r->safe, atlas_buf_cstr(&rep->db_path)));
    (void)fprintf(o, "from:     %s\n", atlas_safe(&r->safe, atlas_buf_cstr(&rep->source.path)));
    if (rep->recovery_made) {
        (void)fprintf(o, "replaced: kept at %s\n",
                      atlas_safe(&r->safe, atlas_buf_cstr(&rep->recovery_path)));
    } else {
        (void)fprintf(o, "replaced: nothing (the data directory held no index)\n");
    }
    if (rep->removed_wal || rep->removed_shm) {
        (void)fprintf(o, "sidecars: removed the previous database's%s%s\n",
                      rep->removed_wal ? " write-ahead log" : "",
                      rep->removed_shm ? " shared-memory index" : "");
    }
    (void)fprintf(o, "schema:   %d%s\n", rep->schema_after,
                  rep->migrated ? " (migrated forward after restore)" : "");
    (void)fprintf(o, "backup as verified before anything moved:\n");
    h_verify_body(r, &rep->source, "  ");
    (void)fprintf(o, "index as verified in place:\n");
    h_verify_body(r, &rep->installed, "  ");
    (void)fprintf(o,
                  "note:     the runtime socket, the service unit and the Claude integration are "
                  "not database content and were not restored. Start the daemon again yourself.\n");
    (void)fprintf(o, "status: %s\n", rep->installed.ok ? "ok" : "attention needed");
    return ok();
}

static atlas_status h_maintenance(atlas_renderer *r, const atlas_maintenance_report *rep,
                                  atlas_err *err) {
    (void)err;
    FILE *o = r->out;
    (void)fprintf(o, "mode:    %s\n", rep->applied ? "prune (applied)" : "plan (nothing written)");
    (void)fprintf(o, "cutoff:  rows created before %s (%" PRId64 " days)\n", rep->cutoff,
                  rep->older_than_days);
    (void)fprintf(o, "floor:   the newest %" PRId64 " events per repository are always kept\n",
                  rep->retain_per_repo);
    (void)fprintf(o, "\n%-24s %-12s %-9s %10s %10s %10s\n", "table", "class", "prunable", "rows",
                  "eligible", "removed");
    for (size_t i = 0; i < rep->table_count; i++) {
        const atlas_maintenance_row *t = &rep->tables[i];
        if (!t->counted) {
            (void)fprintf(o, "%-24s %-12s %-9s %10s %10s %10s\n", t->table,
                          atlas_retention_class_name(t->cls), t->prunable ? "yes" : "no", "-", "-",
                          "-");
            continue;
        }
        (void)fprintf(o, "%-24s %-12s %-9s %10" PRId64 " %10" PRId64 " %10" PRId64 "\n", t->table,
                      atlas_retention_class_name(t->cls), t->prunable ? "yes" : "no",
                      t->rows_before, t->rows_eligible, t->rows_removed);
    }
    (void)fprintf(o, "\ntotals:  %" PRId64 " rows, %" PRId64 " eligible, %" PRId64 " removed\n",
                  rep->total_rows, rep->total_eligible, rep->total_removed);
    (void)fprintf(o, "         %zu table(s) prunable, %zu protected from every automatic rule\n",
                  rep->prunable_tables, rep->protected_tables);
    (void)fprintf(o, "\nwhy each table is classified as it is:\n");
    for (size_t i = 0; i < rep->table_count; i++) {
        (void)fprintf(o, "  %-24s %s\n", rep->tables[i].table, rep->tables[i].reason);
    }
    if (!rep->applied) {
        (void)fprintf(o, "\nnothing was written. Re-run with --apply to remove the eligible rows.\n");
    }
    return ok();
}


/* --- A6: impact gates -------------------------------------------------------
 *
 * Every value below is Atlas-owned except each item's title, which is project
 * prose and is labelled where it is printed. The freshness and reason words are
 * closed vocabularies, the commits are object ids out of Atlas' own index, and
 * the rest are counts. */
static atlas_status h_gate(atlas_renderer *r, const atlas_gate_report *rep, atlas_err *err) {
    (void)err;
    FILE *o = r->out;
    (void)fprintf(o, "gate:       %s\n", atlas_gate_result_name(rep->result));
    (void)fprintf(o, "repository: %s\n", atlas_buf_cstr(&rep->root_text));
    (void)fprintf(o, "indexed:    %s\n",
                  rep->indexed_commit[0] != '\0' ? rep->indexed_commit : "(never scanned)");
    if (strcmp(rep->requested_commit, rep->indexed_commit) != 0) {
        (void)fprintf(o, "requested:  %s\n",
                      rep->requested_commit[0] != '\0' ? rep->requested_commit : "(none)");
    }
    (void)fprintf(o, "depth:      %" PRId64 "\n", rep->depth);
    (void)fprintf(o,
                  "decisions:  %zu assessed (%" PRId64 " fresh, %" PRId64 " stale, %" PRId64
                  " impacted, %" PRId64 " unknown)\n",
                  rep->item_count, rep->fresh, rep->stale, rep->impacted, rep->unknown);
    if (rep->out_of_scope > 0) {
        (void)fprintf(o, "            %" PRId64 " excluded by the requested scope\n",
                      rep->out_of_scope);
    }
    if (rep->limit_reached) {
        (void)fprintf(o, "limit:      reached (%s); this answer is a subset of an answer\n",
                      rep->limit_detail != NULL ? rep->limit_detail : "unspecified");
    }

    for (size_t i = 0; i < rep->item_count; i++) {
        const atlas_gate_assessment *a = &rep->items[i];
        (void)fprintf(o, "\n%s  revision %" PRId64 "  %s\n", atlas_buf_cstr(&a->uid),
                      a->revision_no, atlas_gate_freshness_name(a->freshness));
        (void)fprintf(o, "  title (untrusted project text): %s\n", atlas_buf_cstr(&a->title));
        (void)fprintf(o, "  digest:    %s\n", a->content_hash);
        (void)fprintf(o, "  evidence:  %s\n", a->evidence_digest);
        (void)fprintf(o, "  validated: %s%s\n",
                      a->validated_at_commit[0] != '\0' ? a->validated_at_commit
                                                        : "(no validation point)",
                      a->validated_by_revalidation ? " (revalidated)" : " (at proposal)");
        if (a->revalidation_count > 0) {
            (void)fprintf(o, "  revalidations: %" PRId64 "\n", a->revalidation_count);
        }
        (void)fprintf(o, "  because:  ");
        for (size_t k = 0; k < a->reason_count; k++) {
            (void)fprintf(o, " %s", atlas_gate_reason_name(a->reasons[k]));
        }
        (void)fprintf(o, "\n");
        (void)fprintf(o,
                      "  links:     %" PRId64 " total, %" PRId64 " current, %" PRId64
                      " changed, %" PRId64 " missing, %" PRId64 " ambiguous, %" PRId64 " unknown\n",
                      a->links_total, a->links_current, a->links_changed, a->links_missing,
                      a->links_ambiguous, a->links_unknown);
        (void)fprintf(o,
                      "  range:     %" PRId64 " commit(s), %" PRId64 " path(s); walk reached %"
                      PRId64 " node(s), %" PRId64 " changed\n",
                      a->range_commits, a->range_paths, a->walk_visited, a->walk_matched);
        if (a->limit_reached) {
            (void)fprintf(o, "  limit:     reached (%s)\n",
                          a->limit_detail != NULL ? a->limit_detail : "unspecified");
        }
    }
    if (rep->stale > 0 || rep->impacted > 0) {
        (void)fprintf(o,
                      "\nnote: STALE and IMPACTED mean the code a decision is bound to has moved, "
                      "so it needs\n      a human to look again. Neither says the decision is "
                      "wrong; Atlas has not judged that\n      and cannot.\n");
    }
    return ATLAS_OK;
}

const atlas_renderer_vtbl ATLAS_RENDERER_HUMAN = {
    h_begin,      h_end,          h_note_repo,    h_note_query,   h_list_begin,
    h_list_end,   h_doctor,       h_version,      h_repo_item,    h_repo_added,
    h_repo_removed, h_scan,       h_status,       h_search_item,  h_file,
    h_history_item, h_diff_begin, h_diff_item,    h_diff_end,
    h_daemon_status, h_daemon_ping, h_repo_state, h_sync,         h_event_item,
    h_events_end, h_unit_text,    h_unit_install, h_integrate,
    /* --- A3 --- */
    h_code_status, h_code_file,   h_code_symbol_item, h_code_edge_item,
    h_code_walk_item, h_code_walk_end, h_code_list_begin, h_code_list_end,
    /* --- A4 --- */
    h_decision_item, h_decision_show, h_decision_event, h_decision_outcome,
    h_decision_counts, h_decision_ledger,
    /* --- A5 --- */
    h_backup_created, h_backup_verified, h_backup_restored, h_maintenance,
    /* --- A6 --- */
    h_gate,
};
