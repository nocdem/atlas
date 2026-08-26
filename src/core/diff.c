/* Atlas - complete working-tree change reporting.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * `git status --porcelain=v2` is the single source of truth for *what* changed:
 * it reports the staged state against HEAD and the unstaged state against the
 * index in one pass, handles renames, unmerged paths and untracked paths, and
 * works with an unborn HEAD. Line counts come from `git diff --numstat`, which is
 * enrichment only: a missing count is reported as unknown rather than as zero.
 *
 * Entries are collected before rendering so they can be grouped by scope, and the
 * collection is bounded: past the ceiling the report is truncated and says so.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/pathrep.h"
#include "atlas/safetext.h"
#include "atlas/service.h"
#include "core/service_internal.h"

#define HASH_CHUNK (64u * 1024u)
/* git's own heuristic window for deciding a blob is binary. */
#define BINARY_SNIFF_BYTES 8000u

void atlas_diff_opts_init(atlas_diff_opts *o) {
    memset(o, 0, sizeof(*o));
    o->max_entries = ATLAS_DIFF_DEFAULT_MAX_ENTRIES;
    o->max_hash_bytes = ATLAS_DIFF_DEFAULT_MAX_HASH_BYTES;
}

void atlas_diff_report_init(atlas_diff_report *r) {
    memset(r, 0, sizeof(*r));
    atlas_buf_init(&r->truncated_reason);
}

void atlas_diff_report_free(atlas_diff_report *r) {
    if (r == NULL) {
        return;
    }
    atlas_buf_free(&r->truncated_reason);
}

/* --- collected entries --------------------------------------------------- */

/* One collected entry owns its strings, because the git callbacks that produced
 * them hand over borrowed pointers. */
typedef struct entry_node {
    atlas_change_scope scope;
    char status;
    int score;
    bool score_known;
    atlas_buf path_raw;
    atlas_buf path_text;
    atlas_buf old_path_text;
    bool has_old_path;
    bool path_is_utf8;
    char head_oid[ATLAS_OID_HEX_MAX_INCL];
    char index_oid[ATLAS_OID_HEX_MAX_INCL];
    char mode_head[8];
    char mode_index[8];
    char mode_worktree[8];
    int64_t added;
    int64_t deleted;
    bool counts_known;
    bool binary;
    bool is_directory;
    bool size_known;
    int64_t size_bytes;
    char content_hash[ATLAS_SHA256_HEX_LEN + 1u];
    bool has_hash;
    const char *note; /* static text */
} entry_node;

typedef struct collector {
    entry_node *items;
    size_t count;
    size_t cap;
    int64_t max_entries;
    bool truncated;
} collector;

static void collector_free(collector *c) {
    for (size_t i = 0; i < c->count; i++) {
        atlas_buf_free(&c->items[i].path_raw);
        atlas_buf_free(&c->items[i].path_text);
        atlas_buf_free(&c->items[i].old_path_text);
    }
    free(c->items);
    c->items = NULL;
    c->count = 0;
    c->cap = 0;
}

/* Returns NULL when the ceiling has been reached, having set `truncated`. */
static entry_node *collector_push(collector *c, atlas_err *err) {
    if ((int64_t)c->count >= c->max_entries) {
        c->truncated = true;
        return NULL;
    }
    if (c->count == c->cap) {
        size_t cap = c->cap != 0 ? c->cap * 2u : 64u;
        if ((int64_t)cap > c->max_entries) {
            cap = (size_t)c->max_entries;
        }
        entry_node *p = realloc(c->items, cap * sizeof(*p));
        if (p == NULL) {
            (void)atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory collecting diff entries");
            return NULL;
        }
        c->items = p;
        c->cap = cap;
    }
    entry_node *e = &c->items[c->count++];
    memset(e, 0, sizeof(*e));
    atlas_buf_init(&e->path_raw);
    atlas_buf_init(&e->path_text);
    atlas_buf_init(&e->old_path_text);
    e->added = -1;
    e->deleted = -1;
    return e;
}

/* --- hashing untracked files -------------------------------------------- */

/* Hashes an untracked file's content without holding it in memory, and reports
 * whether it looks binary. The contents themselves are never emitted: Atlas
 * records identity, not payload. */
static atlas_status hash_untracked(int root_fd, const void *path, size_t path_len,
                                   uint64_t max_bytes, entry_node *e, atlas_err *err) {
    atlas_path_open_result res = ATLAS_PATH_OPEN_MISSING;
    int fd = -1;
    struct stat sb;
    memset(&sb, 0, sizeof(sb));
    atlas_status st = atlas_path_open_nofollow(root_fd, (const char *)path, path_len, &res, &fd,
                                               &sb, NULL, err);
    if (st != ATLAS_OK) {
        /* A path git listed but Atlas refuses to resolve is reported, not fatal. */
        atlas_err_init(err);
        e->note = "path could not be resolved safely";
        return ATLAS_OK;
    }

    switch (res) {
    case ATLAS_PATH_OPEN_OK:
        break;
    case ATLAS_PATH_OPEN_SYMLINK: {
        atlas_buf target = ATLAS_BUF_INIT;
        atlas_path_open_result lres = ATLAS_PATH_OPEN_MISSING;
        st = atlas_path_readlink_at(root_fd, (const char *)path, path_len, &target, &lres, err);
        if (st != ATLAS_OK) {
            atlas_buf_free(&target);
            atlas_err_init(err);
            e->note = "symlink target could not be read";
            return ATLAS_OK;
        }
        atlas_sha256_hex(target.data, target.len, e->content_hash);
        e->has_hash = true;
        e->size_known = true;
        e->size_bytes = (int64_t)target.len;
        e->note = "untracked symlink; the link text is what is hashed";
        atlas_buf_free(&target);
        return ATLAS_OK;
    }
    case ATLAS_PATH_OPEN_UNSAFE:
        e->note = "refused: a path component is a symlink";
        return ATLAS_OK;
    case ATLAS_PATH_OPEN_MISSING:
        e->note = "no longer present";
        return ATLAS_OK;
    case ATLAS_PATH_OPEN_NOT_REGULAR:
        e->note = "not a regular file";
        return ATLAS_OK;
    case ATLAS_PATH_OPEN_DENIED:
    default:
        e->note = "cannot be opened";
        return ATLAS_OK;
    }

    e->size_known = true;
    e->size_bytes = (int64_t)sb.st_size;
    if ((uint64_t)sb.st_size > max_bytes) {
        (void)close(fd);
        e->note = "exceeds the hash size limit; size recorded without a content hash";
        return ATLAS_OK;
    }

    atlas_sha256 ctx;
    atlas_sha256_init(&ctx);
    unsigned char buf[HASH_CHUNK];
    uint64_t total = 0;
    bool sniffed = false;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            (void)close(fd);
            e->note = "content could not be read";
            return ATLAS_OK;
        }
        if (n == 0) {
            break;
        }
        if (!sniffed) {
            size_t window = (size_t)n < BINARY_SNIFF_BYTES ? (size_t)n : BINARY_SNIFF_BYTES;
            if (memchr(buf, '\0', window) != NULL) {
                e->binary = true;
            }
            sniffed = true;
        }
        atlas_sha256_update(&ctx, buf, (size_t)n);
        total += (uint64_t)n;
    }
    (void)close(fd);
    unsigned char digest[ATLAS_SHA256_DIGEST_LEN];
    atlas_sha256_final(&ctx, digest);
    atlas_hex_encode(digest, sizeof(digest), e->content_hash);
    e->has_hash = true;
    e->size_bytes = (int64_t)total;
    return ATLAS_OK;
}

/* --- status collection --------------------------------------------------- */

typedef struct status_ctx {
    collector *coll;
    atlas_git *g;
    const atlas_diff_opts *opts;
    atlas_err *err;
} status_ctx;

static void copy_fixed(char *dst, size_t dst_size, const char *src) {
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    (void)snprintf(dst, dst_size, "%s", src);
}

static atlas_status on_status_entry(const atlas_git_status_entry *se, void *ud, atlas_err *err) {
    status_ctx *sc = (status_ctx *)ud;
    if (se->scope == ATLAS_SCOPE_UNTRACKED && sc->opts->skip_untracked) {
        return ATLAS_OK;
    }

    entry_node *e = collector_push(sc->coll, err);
    if (e == NULL) {
        if (sc->coll->truncated) {
            return ATLAS_OK; /* keep reading so the counts stay honest */
        }
        return err->status != ATLAS_OK ? err->status : ATLAS_ERR_INTERNAL;
    }

    e->scope = se->scope;
    e->status = se->status;
    e->score = se->score;
    e->score_known = se->score_known;
    e->is_directory = se->is_directory;

    atlas_status st = atlas_buf_set(&e->path_raw, se->path, se->path_len, err);
    if (st == ATLAS_OK) {
        st = atlas_text_encode_safe(se->path, se->path_len, &e->path_text, err);
    }
    if (st != ATLAS_OK) {
        return st;
    }
    e->path_is_utf8 = atlas_utf8_valid(se->path, se->path_len);
    if (se->old_path != NULL && se->old_path_len > 0) {
        st = atlas_text_encode_safe(se->old_path, se->old_path_len, &e->old_path_text, err);
        if (st != ATLAS_OK) {
            return st;
        }
        e->has_old_path = true;
    }
    copy_fixed(e->head_oid, sizeof(e->head_oid), se->head_oid);
    copy_fixed(e->index_oid, sizeof(e->index_oid), se->index_oid);
    copy_fixed(e->mode_head, sizeof(e->mode_head), se->mode_head);
    copy_fixed(e->mode_index, sizeof(e->mode_index), se->mode_index);
    copy_fixed(e->mode_worktree, sizeof(e->mode_worktree), se->mode_worktree);

    if (se->scope == ATLAS_SCOPE_UNTRACKED) {
        if (se->is_directory) {
            e->note = "untracked directory, collapsed by git; not descended into";
        } else {
            st = hash_untracked(atlas_git_root_fd(sc->g), se->path, se->path_len,
                                sc->opts->max_hash_bytes != 0 ? sc->opts->max_hash_bytes
                                                              : ATLAS_DIFF_DEFAULT_MAX_HASH_BYTES,
                                e, err);
            if (st != ATLAS_OK) {
                return st;
            }
        }
    }
    return ATLAS_OK;
}

/* --- numstat enrichment -------------------------------------------------- */

typedef struct numstat_ctx {
    collector *coll;
    atlas_change_scope scope;
} numstat_ctx;

/* Attaches line counts to the already-collected entry with this path and scope.
 * A count that arrives for a path Atlas did not collect (for instance beyond the
 * truncation ceiling) is dropped rather than invented. */
static atlas_status on_numstat(const atlas_git_diff_entry *d, void *ud, atlas_err *err) {
    numstat_ctx *nc = (numstat_ctx *)ud;
    (void)err;
    for (size_t i = 0; i < nc->coll->count; i++) {
        entry_node *e = &nc->coll->items[i];
        if (e->scope != nc->scope) {
            continue;
        }
        if (e->path_raw.len != d->path_len ||
            memcmp(e->path_raw.data, d->path, d->path_len) != 0) {
            continue;
        }
        e->binary = d->binary;
        if (!d->binary) {
            e->added = d->added;
            e->deleted = d->deleted;
            e->counts_known = true;
        }
        return ATLAS_OK;
    }
    return ATLAS_OK;
}

/* --- driver -------------------------------------------------------------- */

static const char *change_type_for(atlas_change_scope scope, char status) {
    if (scope == ATLAS_SCOPE_UNTRACKED) {
        return "untracked";
    }
    if (scope == ATLAS_SCOPE_UNMERGED) {
        return "unmerged";
    }
    return atlas_git_change_type_name(status);
}

static atlas_status emit_scope(collector *c, atlas_change_scope scope, atlas_diff_entry_cb cb,
                               void *ud, atlas_err *err) {
    for (size_t i = 0; i < c->count; i++) {
        entry_node *n = &c->items[i];
        if (n->scope != scope) {
            continue;
        }
        atlas_diff_entry e;
        memset(&e, 0, sizeof(e));
        e.scope = n->scope;
        e.status = n->status;
        e.change_type = change_type_for(n->scope, n->status);
        e.score = n->score;
        e.score_known = n->score_known;
        e.path_text = atlas_buf_cstr(&n->path_text);
        e.path_raw = n->path_raw.data;
        e.path_raw_len = n->path_raw.len;
        e.path_is_utf8 = n->path_is_utf8;
        e.old_path_text = n->has_old_path ? atlas_buf_cstr(&n->old_path_text) : NULL;
        e.head_oid = n->head_oid;
        e.index_oid = n->index_oid;
        e.mode_head = n->mode_head;
        e.mode_index = n->mode_index;
        e.mode_worktree = n->mode_worktree;
        e.added = n->added;
        e.deleted = n->deleted;
        e.counts_known = n->counts_known;
        e.binary = n->binary;
        e.is_directory = n->is_directory;
        e.size_known = n->size_known;
        e.size_bytes = n->size_bytes;
        e.content_hash = n->has_hash ? n->content_hash : NULL;
        e.content_hash_algo = n->has_hash ? "sha256" : NULL;
        e.note = n->note;
        if (cb != NULL) {
            atlas_status st = cb(&e, ud, err);
            if (st != ATLAS_OK) {
                return st;
            }
        }
    }
    return ATLAS_OK;
}

static atlas_status diff_from_git(atlas_git *g, const atlas_diff_opts *opts,
                                  atlas_diff_entry_cb cb, void *ud, atlas_diff_report *report,
                                  atlas_err *err) {
    atlas_diff_opts defaults;
    atlas_diff_opts_init(&defaults);
    if (opts == NULL) {
        opts = &defaults;
    }

    atlas_git_head head;
    atlas_status st = atlas_git_read_head(g, &head, err);
    if (st != ATLAS_OK) {
        return st;
    }
    (void)snprintf(report->base_head, sizeof(report->base_head), "%s", head.oid);
    (void)snprintf(report->head_state, sizeof(report->head_state), "%s", head.state);
    (void)snprintf(report->branch, sizeof(report->branch), "%s", head.branch);

    collector coll;
    memset(&coll, 0, sizeof(coll));
    coll.max_entries =
        opts->max_entries > 0 ? opts->max_entries : ATLAS_DIFF_DEFAULT_MAX_ENTRIES;

    status_ctx sc;
    memset(&sc, 0, sizeof(sc));
    sc.coll = &coll;
    sc.g = g;
    sc.opts = opts;

    atlas_git_worktree_state wt;
    st = atlas_git_read_status(g, &wt, on_status_entry, &sc, err);
    if (st != ATLAS_OK) {
        collector_free(&coll);
        return st;
    }
    report->dirty = wt.dirty;
    report->staged_count = wt.staged;
    report->unstaged_count = wt.unstaged;
    report->untracked_count = opts->skip_untracked ? 0 : wt.untracked;
    report->unmerged_count = wt.unmerged;

    /* Line counts. The staged comparison needs a base, so it is skipped for an
     * unborn HEAD: the staged entries are still reported, without counts. */
    numstat_ctx nc;
    nc.coll = &coll;
    nc.scope = ATLAS_SCOPE_UNSTAGED;
    st = atlas_git_diff_worktree(g, on_numstat, &nc, err);
    if (st == ATLAS_OK && strcmp(head.state, "unborn") != 0) {
        nc.scope = ATLAS_SCOPE_STAGED;
        st = atlas_git_diff_staged(g, on_numstat, &nc, err);
    }
    if (st != ATLAS_OK) {
        collector_free(&coll);
        return st;
    }

    report->total_entries = (int64_t)coll.count;
    report->binary_changes = 0;
    for (size_t i = 0; i < coll.count; i++) {
        if (coll.items[i].binary) {
            report->binary_changes++;
        }
    }
    report->truncated = coll.truncated;
    if (coll.truncated) {
        st = atlas_buf_appendf(&report->truncated_reason, err,
                               "more than %lld changed paths; only the first %lld are reported",
                               (long long)coll.max_entries, (long long)coll.max_entries);
        if (st != ATLAS_OK) {
            collector_free(&coll);
            return st;
        }
    }

    /* Grouped so a reader sees one coherent section at a time. */
    st = emit_scope(&coll, ATLAS_SCOPE_STAGED, cb, ud, err);
    if (st == ATLAS_OK) {
        st = emit_scope(&coll, ATLAS_SCOPE_UNSTAGED, cb, ud, err);
    }
    if (st == ATLAS_OK) {
        st = emit_scope(&coll, ATLAS_SCOPE_UNMERGED, cb, ud, err);
    }
    if (st == ATLAS_OK) {
        st = emit_scope(&coll, ATLAS_SCOPE_UNTRACKED, cb, ud, err);
    }
    collector_free(&coll);
    return st;
}

/* The observation itself, given a repository row.
 *
 * `diff` reads no index: it resolves the repository and then asks git. Split so
 * the daemon-served form can supply the row over the socket and run exactly
 * this, rather than a second implementation of the same observation — the same
 * reason `atlas_service_status_observe_live` is split, and the reason A8's read
 * surface needs no `repo.diff` method. */
atlas_status atlas_service_diff_repo(const atlas_repo_info *info, const atlas_diff_opts *opts,
                                     atlas_diff_entry_cb cb, void *ud, atlas_diff_report *report,
                                     atlas_err *err) {
    atlas_git *g = NULL;
    /* A13: no `ctx` anywhere in this chain, and this is a read an operator
     * runs against their own tree. NULL means the tree itself. */
    atlas_status st = atlas_service_open_repo_git(info, NULL, &g, err);
    if (st == ATLAS_OK) {
        st = diff_from_git(g, opts, cb, ud, report, err);
    }
    atlas_git_close(g);
    return st;
}

atlas_status atlas_service_diff(atlas_ctx *ctx, const char *name, const atlas_diff_opts *opts,
                                atlas_diff_entry_cb cb, void *ud, atlas_diff_report *report,
                                atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_service_require_repo(ctx, name, &info, err);
    if (st == ATLAS_OK) {
        st = atlas_service_diff_repo(&info, opts, cb, ud, report, err);
    }
    atlas_repo_info_free(&info);
    return st;
}
