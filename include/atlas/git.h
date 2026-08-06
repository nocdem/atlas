/* Atlas - read-only git adapter.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The git executable is the source of truth for source and history facts. Every
 * invocation goes through atlas_proc (no shell, explicit argv), uses a hardened
 * environment, is bounded by a timeout and an output ceiling, and is checked
 * against an allowlist of read-only subcommands before it runs.
 *
 * Atlas never changes its own working directory: commands are addressed with
 * `git -C <canonical-root>`.
 */
#ifndef ATLAS_GIT_H
#define ATLAS_GIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "atlas/buf.h"
#include "atlas/error.h"
#include "atlas/limits.h"

#define ATLAS_GIT_DEFAULT_TIMEOUT_MS 60000
#define ATLAS_GIT_MAX_TOKEN (16u * 1024u * 1024u) /* per NUL-delimited record */

typedef struct atlas_git atlas_git;

/* Locate the git executable and read its version. Either output may be NULL. */
atlas_status atlas_git_probe(atlas_buf *exe_out, atlas_buf *version_out, atlas_err *err);

/* Validate `path` as a git working tree and bind an adapter to its canonical
 * root. Fails with ATLAS_ERR_REPO when the path is not a git repository. */
atlas_status atlas_git_open(const char *path, atlas_git **out, atlas_err *err);
void atlas_git_close(atlas_git *g);

const char *atlas_git_root(const atlas_git *g);
/* The common Git directory: shared by every worktree of one repository. */
const char *atlas_git_common_dir(const atlas_git *g);
/* This worktree's own Git directory: equal to the common dir for the main
 * worktree, and <common>/worktrees/<name> for a linked worktree. This is what
 * distinguishes two worktrees of the same repository. */
const char *atlas_git_dir(const atlas_git *g);
/* True when this is a linked worktree rather than the main one. */
bool atlas_git_is_linked_worktree(const atlas_git *g);

/* Partial (promisor) repositories are refused by atlas_git_open, because git may
 * lazily fetch a missing object and Atlas performs no network access. These
 * report what was detected, for diagnostics. */
bool atlas_git_is_partial_clone(const atlas_git *g);
const char *atlas_git_partial_reason(const atlas_git *g);
/* True when the object store has alternates, so objects may live outside this
 * repository. Recorded and reported, not refused: this is how git shares objects
 * between worktrees and shared clones. */
bool atlas_git_has_alternates(const atlas_git *g);
const char *atlas_git_object_format(const atlas_git *g); /* sha1|sha256|unknown */
const char *atlas_git_exe(const atlas_git *g);
/* Read-only, O_NOFOLLOW directory fd on the canonical root, owned by `g`. */
int atlas_git_root_fd(const atlas_git *g);

void atlas_git_set_timeout_ms(atlas_git *g, int ms);
void atlas_git_set_max_output(atlas_git *g, size_t bytes);

typedef struct atlas_git_head {
    char oid[ATLAS_OID_HEX_MAX_INCL];
    char branch[256]; /* "" when detached */
    char state[16];   /* born | unborn | detached */
} atlas_git_head;

atlas_status atlas_git_read_head(atlas_git *g, atlas_git_head *out, atlas_err *err);

typedef struct atlas_git_worktree_state {
    bool dirty;
    int staged;
    int unstaged;
    int untracked;
    int unmerged;
    char branch[256]; /* "" when detached */
    char oid[ATLAS_OID_HEX_MAX_INCL];
    bool unborn;
} atlas_git_worktree_state;

atlas_status atlas_git_read_worktree_state(atlas_git *g, atlas_git_worktree_state *out,
                                           atlas_err *err);

/* --- full working-tree change state ------------------------------------- */

/* Which comparison an entry describes. One porcelain-v2 record can produce both a
 * staged and an unstaged entry: a path staged and then modified again is two
 * distinct facts, and reporting only one of them would be misleading. */
typedef enum atlas_change_scope {
    ATLAS_SCOPE_STAGED = 0,  /* index vs HEAD */
    ATLAS_SCOPE_UNSTAGED,    /* working tree vs index */
    ATLAS_SCOPE_UNTRACKED,   /* present on disk, not in the index */
    ATLAS_SCOPE_UNMERGED     /* conflicted */
} atlas_change_scope;

const char *atlas_change_scope_name(atlas_change_scope s);

typedef struct atlas_git_status_entry {
    atlas_change_scope scope;
    char status;    /* A C D M R T U ? as reported by git */
    int score;      /* rename/copy similarity */
    bool score_known;
    const void *path;
    size_t path_len;
    const void *old_path; /* NULL unless rename/copy */
    size_t old_path_len;
    const char *head_oid;  /* object id in HEAD, "" when absent */
    const char *index_oid; /* object id in the index, "" when absent */
    const char *mode_head;
    const char *mode_index;
    const char *mode_worktree;
    bool is_directory; /* an untracked directory git collapsed */
} atlas_git_status_entry;

typedef atlas_status (*atlas_git_status_cb)(const atlas_git_status_entry *e, void *ud,
                                            atlas_err *err);

/* Reads the full change state in one `git status --porcelain=v2` invocation:
 * summary counts into `out` and every individual entry through `cb`. */
atlas_status atlas_git_read_status(atlas_git *g, atlas_git_worktree_state *out,
                                   atlas_git_status_cb cb, void *ud, atlas_err *err);

/* --- tracked file enumeration ------------------------------------------ */

typedef struct atlas_git_index_entry {
    const char *mode; /* e.g. "100644" */
    const char *oid;
    int stage;
    const void *path;
    size_t path_len;
} atlas_git_index_entry;

typedef atlas_status (*atlas_git_index_cb)(const atlas_git_index_entry *e, void *ud,
                                           atlas_err *err);

atlas_status atlas_git_ls_files(atlas_git *g, atlas_git_index_cb cb, void *ud, atlas_err *err);

/* --- untracked file discovery (A1) -------------------------------------- */

/* One path with no accompanying index metadata. */
typedef atlas_status (*atlas_git_path_cb)(const void *path, size_t path_len, void *ud,
                                          atlas_err *err);

/* Every untracked path git's own ignore rules do not cover, one per file rather
 * than collapsed to a directory.
 *
 * `git status --untracked-files=normal` collapses a wholly untracked directory
 * into a single entry, which keeps `atlas diff` bounded but means a newly
 * created directory — exactly where new work appears — is indexed as a name and
 * nothing else. This enumerates the files instead. Using git rather than walking
 * the tree ourselves means .gitignore, .git/info/exclude, the global excludes
 * file and nested ignore files are all honoured exactly as git honours them,
 * with no second implementation to drift. */
atlas_status atlas_git_ls_untracked(atlas_git *g, atlas_git_path_cb cb, void *ud, atlas_err *err);

/* Untracked paths that git's ignore rules DO cover. Reported separately so a
 * caller can distinguish "skipped because ignored" from "skipped because a
 * ceiling was reached". Directories are collapsed here, because an ignored build
 * tree is exactly the thing that should not be enumerated file by file. */
atlas_status atlas_git_ls_ignored(atlas_git *g, atlas_git_path_cb cb, void *ud, atlas_err *err);


/* --- history ----------------------------------------------------------- */

typedef struct atlas_git_commit {
    const char *oid;
    const char *parents; /* space-separated */
    int parent_count;
    const char *author_name;
    const char *author_email;
    int64_t author_time;
    int64_t commit_time;
    const char *subject; /* first line of the raw message */
    const char *body;    /* full raw message */
    size_t body_len;
} atlas_git_commit;

typedef struct atlas_git_change {
    const char *raw_status; /* e.g. "M", "R100" */
    char kind;              /* A C D M R T U X */
    int score;
    bool score_known;
    const void *path;
    size_t path_len;
    const void *old_path; /* NULL unless rename/copy */
    size_t old_path_len;
} atlas_git_change;

typedef atlas_status (*atlas_git_commit_cb)(const atlas_git_commit *c, void *ud, atlas_err *err);
typedef atlas_status (*atlas_git_change_cb)(const atlas_git_commit *c,
                                            const atlas_git_change *ch, void *ud, atlas_err *err);

/* Stream `git log` with rename and copy detection. Commits arrive newest first;
 * `commit_cb` fires before that commit's changes. `limit_path` may be NULL to
 * walk the whole history, or restrict the walk to one path. `max_commits <= 0`
 * means unlimited. Memory use is bounded by ATLAS_GIT_MAX_TOKEN. */
atlas_status atlas_git_log(atlas_git *g, const void *limit_path, size_t limit_path_len,
                           int64_t max_commits, atlas_git_commit_cb commit_cb,
                           atlas_git_change_cb change_cb, void *ud, atlas_err *err);

/* --- incremental history (A1) ------------------------------------------- */

/* Like atlas_git_log, but walks only what is reachable from HEAD and not from
 * `exclude_oid` — the commit whose history Atlas already ingested. That turns a
 * repeated pass over a large repository from "replay everything" into "read the
 * new commits". `exclude_oid` may be NULL or "" for a full walk. */
atlas_status atlas_git_log_since(atlas_git *g, const char *exclude_oid, int64_t max_commits,
                                 atlas_git_commit_cb commit_cb, atlas_git_change_cb change_cb,
                                 void *ud, atlas_err *err);

/* True when `oid` holds commits that HEAD cannot reach — the signature of a
 * force-push, a rebase or a reset, as opposed to an ordinary fast-forward.
 *
 * `*unknown_out` is set when the commit is not in the object store at all,
 * which is the other way history gets rewritten out from under an index. */
atlas_status atlas_git_tip_is_stale(atlas_git *g, const char *oid, bool *stale_out,
                                    bool *unknown_out, atlas_err *err);

/* --- working tree diff ------------------------------------------------- */

typedef struct atlas_git_diff_entry {
    int64_t added;   /* -1 for binary */
    int64_t deleted; /* -1 for binary */
    bool binary;
    const void *path;
    size_t path_len;
    const void *old_path; /* NULL unless renamed */
    size_t old_path_len;
} atlas_git_diff_entry;

typedef atlas_status (*atlas_git_diff_cb)(const atlas_git_diff_entry *e, void *ud, atlas_err *err);

/* Unstaged working-tree diff against the index. */
atlas_status atlas_git_diff_worktree(atlas_git *g, atlas_git_diff_cb cb, void *ud, atlas_err *err);
/* Staged diff: the index against HEAD. Requires a born HEAD, since there is no
 * base to compare against otherwise. */
atlas_status atlas_git_diff_staged(atlas_git *g, atlas_git_diff_cb cb, void *ud, atlas_err *err);

/* Exposed for tests: true when argv describes a read-only git invocation. */
bool atlas_git_argv_is_readonly(const char *const *argv, const char **reason_out);

/* Maps a raw git name-status letter to the Atlas change type recorded in the
 * index: add, modify, delete, rename, copy, typechange, unmerged or unknown. */
const char *atlas_git_change_type_name(char kind);

#endif /* ATLAS_GIT_H */
