/* Atlas - A8: producing and serving a daemon-owned source snapshot.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See atlas/snapshot.h for why the direction is inverted — the daemon reads, the
 * worker receives — and what that buys.
 *
 * Everything here runs as `atlasd`, inside the daemon, against a repository the
 * daemon resolved from its own registry. Nothing in this file takes a path, a
 * commit, a repository name or an entry index from a worker message without
 * validating it against persisted state first.
 */
#define _GNU_SOURCE 1

#include "atlas/snapshot.h"

#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/db.h"
#include "atlas/git.h"
#include "atlas/orch.h"
#include "atlas/orch_ops.h"
#include "atlas/sha256.h"

const char *atlas_snapshot_refusal_name(atlas_snapshot_refusal r) {
    switch (r) {
    case ATLAS_SNAPSHOT_REFUSE_NONE: return "NONE";
    case ATLAS_SNAPSHOT_REFUSE_SYMLINK: return "SYMLINK";
    case ATLAS_SNAPSHOT_REFUSE_GITLINK: return "GITLINK";
    case ATLAS_SNAPSHOT_REFUSE_MODE: return "MODE";
    case ATLAS_SNAPSHOT_REFUSE_PATH: return "PATH";
    case ATLAS_SNAPSHOT_REFUSE_SIZE: return "SIZE";
    }
    return "NONE";
}

bool atlas_snapshot_path_ok(const void *path, size_t len) {
    const char *p = (const char *)path;
    if (p == NULL || len == 0 || len > ATLAS_SNAPSHOT_PATH_MAX) {
        return false;
    }
    if (p[0] == '/') {
        return false; /* absolute: never a committed-tree path */
    }
    if (memchr(p, '\0', len) != NULL) {
        return false;
    }
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i != len && p[i] != '/') {
            continue;
        }
        size_t n = i - start;
        if (n == 0 || (n == 1 && p[start] == '.') ||
            (n == 2 && p[start] == '.' && p[start + 1] == '.')) {
            return false;
        }
        start = i + 1u;
    }
    return true;
}

/* --- the digest ------------------------------------------------------------
 *
 * Domain-separated and length-prefixed, for A4's reason: with any single-byte
 * delimiter a path of "a/b" with mode "c" would encode identically to a path of
 * "a" with mode "b/c". It covers the manifest — order, paths, modes, sizes and
 * content digests — plus the commit, the tree and the totals. It does **not**
 * cover the transfer, so a stream that lost, duplicated or reordered an entry
 * cannot produce a match when the worker recomputes it. */
typedef struct digest_state {
    atlas_sha256 h;
} digest_state;

static void feed(atlas_sha256 *h, const void *data, size_t len) {
    unsigned char hdr[8];
    uint64_t n = (uint64_t)len;
    for (int i = 0; i < 8; i++) {
        hdr[i] = (unsigned char)((n >> (8 * (7 - i))) & 0xffu);
    }
    atlas_sha256_update(h, hdr, sizeof(hdr));
    if (len > 0) {
        atlas_sha256_update(h, data, len);
    }
}

static void feed_i64(atlas_sha256 *h, int64_t v) {
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
    feed(h, tmp, n > 0 ? (size_t)n : 0u);
}

atlas_status atlas_snapshot_digest_begin(atlas_snapshot_digest *d, const char *commit,
                                         const char *tree, atlas_err *err) {
    digest_state *st = calloc(1, sizeof(*st));
    if (st == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    atlas_sha256_init(&st->h);
    feed(&st->h, ATLAS_SNAPSHOT_DOMAIN, strlen(ATLAS_SNAPSHOT_DOMAIN));
    feed_i64(&st->h, ATLAS_SNAPSHOT_PROTOCOL);
    feed(&st->h, commit, strlen(commit));
    feed(&st->h, tree, strlen(tree));
    d->opaque = st;
    return ATLAS_OK;
}

atlas_status atlas_snapshot_digest_entry(atlas_snapshot_digest *d, const void *path,
                                         size_t path_len, const char *mode, int64_t size,
                                         const char *sha256, atlas_err *err) {
    digest_state *st = (digest_state *)d->opaque;
    if (st == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "the snapshot digest was not begun");
    }
    feed(&st->h, path, path_len);
    feed(&st->h, mode, strlen(mode));
    feed_i64(&st->h, size);
    feed(&st->h, sha256, strlen(sha256));
    return ATLAS_OK;
}

atlas_status atlas_snapshot_digest_finish(atlas_snapshot_digest *d, int64_t entries,
                                          int64_t total_bytes, char out[65], atlas_err *err) {
    digest_state *st = (digest_state *)d->opaque;
    if (st == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "the snapshot digest was not begun");
    }
    /* The totals are covered too, so a truncated stream that happened to end on
     * an entry boundary still fails: the count it recomputes will not match. */
    feed_i64(&st->h, entries);
    feed_i64(&st->h, total_bytes);
    unsigned char digest[ATLAS_SHA256_DIGEST_LEN];
    atlas_sha256_final(&st->h, digest);
    atlas_hex_encode(digest, sizeof(digest), out);
    free(st);
    d->opaque = NULL;
    return ATLAS_OK;
}

void atlas_snapshot_digest_abort(atlas_snapshot_digest *d) {
    free(d->opaque);
    d->opaque = NULL;
}

void atlas_snapshot_chunk_init(atlas_snapshot_chunk *c) {
    memset(c, 0, sizeof(*c));
    atlas_buf_init(&c->path);
    atlas_buf_init(&c->data);
}

void atlas_snapshot_chunk_free(atlas_snapshot_chunk *c) {
    if (c == NULL) {
        return;
    }
    atlas_buf_free(&c->path);
    atlas_buf_free(&c->data);
}

/* --- enumeration ------------------------------------------------------------ */

typedef struct enum_ctx {
    atlas_db *db;
    atlas_git *git;
    int64_t snapshot_id;
    int64_t index;
    int64_t total;
    atlas_snapshot_meta *meta;
    atlas_snapshot_digest digest;
    atlas_buf blob;
    atlas_status st;
    atlas_err *err;
} enum_ctx;

static atlas_status blob_sink(const char *chunk, size_t n, void *ud, atlas_err *err) {
    enum_ctx *c = (enum_ctx *)ud;
    if ((int64_t)(c->blob.len + n) > ATLAS_SNAPSHOT_MAX_FILE_BYTES) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "a committed file exceeds the %lld byte snapshot bound",
                             (long long)ATLAS_SNAPSHOT_MAX_FILE_BYTES);
    }
    return atlas_buf_append(&c->blob, chunk, n, err);
}

static atlas_status enum_entry(const atlas_git_tree_entry *e, void *ud, atlas_err *err) {
    enum_ctx *c = (enum_ctx *)ud;

    /* Refused by kind, counted, and never materialised. A symlink recreated in
     * the workspace is a path that leaves it the moment it is followed; a
     * gitlink is a submodule, and there is no machinery here to act on one. */
    if (strcmp(e->mode, "120000") == 0) {
        c->meta->refused_symlinks++;
        return ATLAS_OK;
    }
    if (strcmp(e->mode, "160000") == 0) {
        c->meta->refused_gitlinks++;
        return ATLAS_OK;
    }
    if (strcmp(e->type, "blob") != 0 ||
        (strcmp(e->mode, "100644") != 0 && strcmp(e->mode, "100755") != 0)) {
        c->meta->refused_other++;
        return ATLAS_OK;
    }
    if (!atlas_snapshot_path_ok(e->path, e->path_len)) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "the committed tree holds a path that cannot be materialised safely");
    }
    if (c->index >= ATLAS_SNAPSHOT_MAX_ENTRIES) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "the committed tree holds more than %d "
                                                       "entries",
                             ATLAS_SNAPSHOT_MAX_ENTRIES);
    }

    /* Read once, here, to learn the size and the content digest. The bytes are
     * not stored; a chunk request re-reads them by object id. */
    atlas_buf_reset(&c->blob);
    atlas_status st = atlas_git_cat_blob(c->git, e->oid, blob_sink, c,
                                         (size_t)ATLAS_SNAPSHOT_MAX_FILE_BYTES, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (c->total + (int64_t)c->blob.len > ATLAS_SNAPSHOT_MAX_TOTAL_BYTES) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "the committed tree exceeds the %lld byte snapshot bound",
                             (long long)ATLAS_SNAPSHOT_MAX_TOTAL_BYTES);
    }
    char hex[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_sha256_hex(c->blob.data != NULL ? c->blob.data : "", c->blob.len, hex);

    st = atlas_db_orch_snapshot_add_entry(c->db, c->snapshot_id, c->index, e->path, e->path_len,
                                          e->mode, e->oid, (int64_t)c->blob.len, hex, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_snapshot_digest_entry(&c->digest, e->path, e->path_len, e->mode,
                                     (int64_t)c->blob.len, hex, err);
    if (st != ATLAS_OK) {
        return st;
    }
    c->index++;
    c->total += (int64_t)c->blob.len;
    return ATLAS_OK;
}

atlas_status atlas_snapshot_open(atlas_db *db, int64_t attempt_id, atlas_snapshot_meta *out,
                                 atlas_err *err) {
    memset(out, 0, sizeof(*out));
    out->protocol = ATLAS_SNAPSHOT_PROTOCOL;

    /* Idempotent: a dispatcher that restarts mid-transfer resumes against the
     * same snapshot identity rather than a freshly enumerated one that might
     * differ, because the repository is a live directory. */
    bool existing = false;
    atlas_status st = atlas_db_orch_snapshot_get(db, attempt_id, out, &existing, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (existing) {
        return ATLAS_OK;
    }

    /* Everything below comes from persisted state. The attempt names the job,
     * the job names the repository and the exact pinned commit, and the registry
     * names the canonical path. A worker supplies none of it. */
    atlas_orch_snapshot_source src;
    memset(&src, 0, sizeof(src));
    atlas_buf_init(&src.repo_root);
    atlas_buf_init(&src.commit);
    atlas_buf_init(&src.identity);
    st = atlas_db_orch_snapshot_source(db, attempt_id, &src, err);
    if (st != ATLAS_OK) {
        goto done_src;
    }

    atlas_git *g = NULL;
    st = atlas_git_open(atlas_buf_cstr(&src.repo_root), &g, err);
    if (st != ATLAS_OK) {
        goto done_src;
    }

    /* The pinned commit must belong to *this* repository. Resolving it here,
     * against the repository the registry named, is what stops a commit that
     * exists somewhere else from being snapshotted as though it were this
     * project's. */
    atlas_buf tree = ATLAS_BUF_INIT;
    st = atlas_git_commit_tree(g, atlas_buf_cstr(&src.commit), &tree, err);
    if (st != ATLAS_OK) {
        atlas_git_close(g);
        goto done_src;
    }

    (void)snprintf(out->commit, sizeof(out->commit), "%s", atlas_buf_cstr(&src.commit));
    (void)snprintf(out->tree, sizeof(out->tree), "%s", atlas_buf_cstr(&tree));

    enum_ctx c;
    memset(&c, 0, sizeof(c));
    c.db = db;
    c.git = g;
    c.meta = out;
    c.err = err;
    atlas_buf_init(&c.blob);

    st = atlas_db_begin(db, err);
    if (st == ATLAS_OK) {
        st = atlas_db_orch_snapshot_create(db, attempt_id, out->commit, out->tree,
                                           &c.snapshot_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_snapshot_digest_begin(&c.digest, out->commit, out->tree, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_git_ls_tree(g, out->commit, enum_entry, &c, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_snapshot_digest_finish(&c.digest, c.index, c.total, out->digest, err);
    } else {
        atlas_snapshot_digest_abort(&c.digest);
    }
    if (st == ATLAS_OK) {
        out->snapshot_id = c.snapshot_id;
        out->entries = c.index;
        out->total_bytes = c.total;
        st = atlas_db_orch_snapshot_finish(db, c.snapshot_id, out, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_commit(db, err);
        if (st != ATLAS_OK) {
            atlas_db_rollback(db);
        }
    } else {
        /* Whole or nothing: a half-enumerated manifest would be a snapshot
         * identity that describes a tree nobody has. */
        atlas_db_rollback(db);
    }
    atlas_buf_free(&c.blob);
    atlas_buf_free(&tree);
    atlas_git_close(g);

done_src:
    atlas_buf_free(&src.repo_root);
    atlas_buf_free(&src.commit);
    atlas_buf_free(&src.identity);
    return st;
}

/* --- serving one chunk ------------------------------------------------------- */

typedef struct slice_ctx {
    int64_t want_from;
    int64_t want_to;
    int64_t seen;
    atlas_buf *out;
} slice_ctx;

/* Captures only [want_from, want_to) and stops the child once it is past the
 * range. Reading a whole blob to serve its tail would be O(n) per chunk; this
 * is O(offset + chunk), which is the best a streaming `cat-file` allows. */
static atlas_status slice_sink(const char *chunk, size_t n, void *ud, atlas_err *err) {
    slice_ctx *s = (slice_ctx *)ud;
    int64_t start = s->seen;
    int64_t end = s->seen + (int64_t)n;
    s->seen = end;
    if (end <= s->want_from) {
        return ATLAS_OK; /* still before the range */
    }
    if (start >= s->want_to) {
        /* Past the range. Returning non-OK aborts the run and terminates the
         * child, which is exactly what should happen. */
        /* A deliberate abort once the range is filled, not a fault. The caller
         * distinguishes it from a real failure by whether the range was
         * filled. */
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "snapshot slice complete");
    }
    int64_t from = start > s->want_from ? start : s->want_from;
    int64_t to = end < s->want_to ? end : s->want_to;
    return atlas_buf_append(s->out, chunk + (from - start), (size_t)(to - from), err);
}

atlas_status atlas_snapshot_read(atlas_db *db, int64_t attempt_id, int64_t index, int64_t offset,
                                 atlas_snapshot_chunk *out, atlas_err *err) {
    atlas_snapshot_chunk_init(out);
    if (index < 0 || offset < 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a chunk request needs a non-negative index "
                                                   "and offset");
    }
    /* Validated against the persisted manifest: a worker cannot ask for an entry
     * that does not exist, or a range past the end of one that does. */
    atlas_orch_snapshot_entry ent;
    memset(&ent, 0, sizeof(ent));
    atlas_buf_init(&ent.path);
    atlas_buf repo_root = ATLAS_BUF_INIT;
    atlas_status st = atlas_db_orch_snapshot_entry(db, attempt_id, index, &ent, &repo_root, err);
    if (st != ATLAS_OK) {
        goto done;
    }
    if (offset > ent.size_bytes) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE,
                           "offset %lld is past the end of a %lld byte entry", (long long)offset,
                           (long long)ent.size_bytes);
        goto done;
    }
    st = atlas_buf_set(&out->path, ent.path.data, ent.path.len, err);
    if (st != ATLAS_OK) {
        goto done;
    }
    (void)snprintf(out->mode, sizeof(out->mode), "%s", ent.mode);
    (void)snprintf(out->sha256, sizeof(out->sha256), "%s", ent.sha256);
    out->size_bytes = ent.size_bytes;
    out->offset = offset;

    int64_t want = ent.size_bytes - offset;
    if (want > (int64_t)ATLAS_SNAPSHOT_CHUNK_BYTES) {
        want = (int64_t)ATLAS_SNAPSHOT_CHUNK_BYTES;
    }
    if (want > 0) {
        atlas_git *g = NULL;
        st = atlas_git_open(atlas_buf_cstr(&repo_root), &g, err);
        if (st != ATLAS_OK) {
            goto done;
        }
        slice_ctx s = {offset, offset + want, 0, &out->data};
        atlas_err serr;
        atlas_err_init(&serr);
        atlas_status rs = atlas_git_cat_blob(g, ent.oid, slice_sink, &s,
                                             (size_t)ATLAS_SNAPSHOT_MAX_FILE_BYTES, &serr);
        atlas_git_close(g);
        /* The sink aborts deliberately once it has the range; that is a success
         * here, and it is distinguished from a real failure by whether the range
         * was filled. */
        if (rs != ATLAS_OK && (int64_t)out->data.len != want) {
            st = atlas_err_set(err, ATLAS_ERR_GIT, "cannot read a snapshot entry: %s",
                               atlas_err_msg(&serr));
            goto done;
        }
    }
    out->eof = (offset + (int64_t)out->data.len) >= ent.size_bytes;

done:
    atlas_buf_free(&ent.path);
    atlas_buf_free(&repo_root);
    return st;
}
