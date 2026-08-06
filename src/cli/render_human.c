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
    (void)fprintf(o, LABEL "%s\n", "database",
                  atlas_safe(&r->safe, atlas_buf_cstr(&rep->db_path)));
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

const atlas_renderer_vtbl ATLAS_RENDERER_HUMAN = {
    h_begin,      h_end,          h_note_repo,    h_note_query,   h_list_begin,
    h_list_end,   h_doctor,       h_version,      h_repo_item,    h_repo_added,
    h_repo_removed, h_scan,       h_status,       h_search_item,  h_file,
    h_history_item, h_diff_begin, h_diff_item,    h_diff_end,
    h_daemon_status, h_daemon_ping, h_repo_state, h_sync,         h_event_item,
    h_events_end, h_unit_text,    h_unit_install,
};
