/* Atlas - git output parsers, isolated from process spawning.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * These parsers consume byte streams and are driven directly by the unit tests
 * with adversarial input; nothing here executes a program. Every parser is
 * incremental and bounded: no record may exceed `max_token` bytes, so a hostile
 * repository cannot make Atlas allocate without limit.
 */
#ifndef ATLAS_GIT_PARSE_H
#define ATLAS_GIT_PARSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "atlas/buf.h"
#include "atlas/error.h"
#include "atlas/git.h"

/* --- NUL-delimited record splitter -------------------------------------- */

typedef atlas_status (*atlas_tok_cb)(const char *tok, size_t len, void *ud, atlas_err *err);

typedef struct atlas_nulsplit {
    atlas_buf acc;     /* partial record carried across chunk boundaries */
    size_t max_token;
    atlas_tok_cb cb;
    void *ud;
} atlas_nulsplit;

void atlas_nulsplit_init(atlas_nulsplit *s, size_t max_token, atlas_tok_cb cb, void *ud);
void atlas_nulsplit_free(atlas_nulsplit *s);
atlas_status atlas_nulsplit_feed(atlas_nulsplit *s, const char *data, size_t n, atlas_err *err);
/* Fails when trailing bytes are not NUL-terminated: git always terminates its
 * -z records, so a partial tail means truncated or corrupt output. */
atlas_status atlas_nulsplit_finish(atlas_nulsplit *s, atlas_err *err);

/* --- git ls-files --stage -z -------------------------------------------- */
/* Record layout: "<mode> <oid> <stage>\t<path>" */

typedef struct atlas_lsfiles_parser {
    atlas_git_index_cb cb;
    void *ud;
    int64_t entries;
} atlas_lsfiles_parser;

atlas_status atlas_lsfiles_token(const char *tok, size_t len, void *ud, atlas_err *err);

/* --- git log -z --name-status ------------------------------------------- */
/* Commit records are introduced by ATLAS_LOG_SENTINEL and hold fields separated
 * by ATLAS_LOG_FS; name-status entries follow as their own records. */

#define ATLAS_LOG_SENTINEL '\x01'
#define ATLAS_LOG_FS '\x1f'

typedef struct atlas_log_parser {
    atlas_buf oid;
    atlas_buf parents;
    atlas_buf author_name;
    atlas_buf author_email;
    atlas_buf body;
    atlas_buf subject;
    int64_t author_time;
    int64_t commit_time;
    int parent_count;
    bool have_commit;

    /* pending name-status entry */
    char status[8];
    int paths_needed;
    atlas_buf path1;

    atlas_git_commit_cb commit_cb;
    atlas_git_change_cb change_cb;
    void *ud;
    int64_t commits_seen;
    int64_t changes_seen;
} atlas_log_parser;

void atlas_log_parser_init(atlas_log_parser *p, atlas_git_commit_cb commit_cb,
                           atlas_git_change_cb change_cb, void *ud);
void atlas_log_parser_free(atlas_log_parser *p);
atlas_status atlas_log_token(const char *tok, size_t len, void *ud, atlas_err *err);
/* Fails when a name-status entry is still waiting for its path operands. */
atlas_status atlas_log_parser_finish(atlas_log_parser *p, atlas_err *err);

/* Maps a raw name-status letter to an Atlas change type. */
const char *atlas_git_change_type_name(char kind);

/* --- git status --porcelain=v2 -z --branch ------------------------------ */

typedef struct atlas_status_parser {
    atlas_git_worktree_state *out;
    /* Renamed entries carry their original path as a following record, so the
     * entry is held back until that record arrives. */
    bool pending_rename;
    atlas_buf pending_path;
    char pending_xy[3];
    char pending_score[8];
    atlas_buf pending_head_oid;
    atlas_buf pending_index_oid;
    atlas_buf pending_modes[3];

    atlas_git_status_cb cb; /* may be NULL when only counts are wanted */
    void *ud;
    int64_t entries;
} atlas_status_parser;

void atlas_status_parser_init(atlas_status_parser *p, atlas_git_worktree_state *out,
                              atlas_git_status_cb cb, void *ud);
void atlas_status_parser_free(atlas_status_parser *p);
atlas_status atlas_status_token(const char *tok, size_t len, void *ud, atlas_err *err);
/* Fails when a renamed entry never received its original path. */
atlas_status atlas_status_parser_finish(atlas_status_parser *p, atlas_err *err);

/* --- git diff --numstat -z --------------------------------------------- */

typedef struct atlas_numstat_parser {
    atlas_git_diff_cb cb;
    void *ud;
    int64_t entries;
    /* a rename record leaves the path field empty and sends two more records */
    bool pending_pair;
    int64_t pending_added;
    int64_t pending_deleted;
    bool pending_binary;
    atlas_buf pending_old;
    int pending_paths;
} atlas_numstat_parser;

void atlas_numstat_parser_init(atlas_numstat_parser *p, atlas_git_diff_cb cb, void *ud);
void atlas_numstat_parser_free(atlas_numstat_parser *p);
atlas_status atlas_numstat_token(const char *tok, size_t len, void *ud, atlas_err *err);
atlas_status atlas_numstat_parser_finish(atlas_numstat_parser *p, atlas_err *err);

/* --- shared validators -------------------------------------------------- */

bool atlas_git_is_hex_oid(const char *s, size_t n);
/* Parses a decimal integer that must consume the whole span. */
bool atlas_parse_i64(const char *s, size_t n, int64_t *out);

#endif /* ATLAS_GIT_PARSE_H */
