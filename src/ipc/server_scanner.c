/* Atlas - A13: the scanner channel.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A scanner runs as a repository's owner and reads a tree the daemon cannot:
 * `atlasd` is its own principal, and a file the owner created mode 600 is not
 * readable by it. What a scanner may report about is decided here, by one
 * comparison — the peer's uid from `SO_PEERCRED` against
 * `repositories.scanner_uid`.
 *
 * **The group is dispatchable by name rather than hidden**, and that is not a
 * weakening. Hiding a group needs a predicate answerable *before* the method
 * lookup; the dispatcher's is, because it reads a root-owned policy already in
 * memory. The scanner's identity is not in a policy — it is a column, and
 * `dispatch()` does not open the database until after the lookup has finished.
 *
 * Nothing is leaked by answering honestly. A caller learning that
 * `scanner.poll` exists learns what reading the binary would tell it, and the
 * refusal speaks only about the caller's own uid, which it already knows. It
 * names no repository, deliberately: a refusal that listed what the caller
 * could not have would be an inventory handed to whoever asked.
 *
 * `src/ipc/server.c` states the rule this follows: routing is not
 * authorisation, and reaching a name is never the same as being allowed to use
 * it.
 */
#include "atlas/atlas.h"
#include "atlas/db.h"
#include "atlas/sem_discover.h"
#include "atlas/ipc.h"
#include "atlas/safetext.h"
#include "daemon/daemon_internal.h"
#include "daemon/mirror.h"
#include "ipc/server_internal.h"

#include <string.h>
#include <unistd.h>

typedef struct scanner_scan {
    dispatch_state *ds;
    atlas_err *err;
    atlas_status status;
    int64_t matched;
} scanner_scan;

/* True when this peer may act as a scanner for `ri`.
 *
 * Uid 0 is never a match. Zero is how `repositories.scanner_uid` records "no
 * scanner assigned", so a peer arriving as uid 0 against an unassigned row
 * would be granted exactly what the absence means to withhold. Root is refused
 * as a scanner uid at assignment for the same reason; this is the other half of
 * it, and neither half is sufficient alone. */
static bool peer_owns(const dispatch_state *ds, const atlas_repo_info *ri) {
    return ds->peer_uid != 0 && ri->scanner_uid != 0 && ri->scanner_uid == ds->peer_uid;
}

static atlas_status count_cb(const atlas_repo_info *ri, void *ud, atlas_err *err) {
    scanner_scan *s = (scanner_scan *)ud;
    (void)err;
    if (peer_owns(s->ds, ri)) {
        s->matched++;
    }
    return ATLAS_OK;
}

/* The peer must be some repository's scanner. Checked here rather than at the
 * method lookup because the answer is in the database, which is open by now. */
static atlas_status require_scanner(dispatch_state *ds, atlas_err *err) {
    scanner_scan s;
    memset(&s, 0, sizeof(s));
    s.ds = ds;
    s.err = err;
    atlas_status st = atlas_db_repo_list(ds->db, count_cb, &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (s.matched == 0) {
        /* No repository is named. The caller learns about its own uid and
         * nothing else. */
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "no registered repository names uid %lld as its scanner",
                             (long long)ds->peer_uid);
    }
    return ATLAS_OK;
}

static atlas_status emit_cb(const atlas_repo_info *ri, void *ud, atlas_err *err) {
    scanner_scan *s = (scanner_scan *)ud;
    (void)err;
    if (s->status != ATLAS_OK || !peer_owns(s->ds, ri)) {
        return ATLAS_OK;
    }
    atlas_json *j = s->ds->j;
    atlas_status st = atlas_json_obj_begin(j, s->err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "id", ri->id, s->err);
    }
    if (st == ATLAS_OK) {
        /* A repository name is a checked Atlas name, encoded on the way out
         * like every other value on this path. */
        st = atlas_json_key_str(j, "name", atlas_safe(&s->ds->safe, ri->name), s->err);
    }
    if (st == ATLAS_OK) {
        /* `root_path_text` is already the lossless %XX form held in the
         * database. It is emitted as-is: encoding it again would double-encode
         * a value that is already display-safe. */
        st = atlas_json_key_str(j, "root", atlas_buf_cstr(&ri->root_path_text), s->err);
    }
    if (st == ATLAS_OK) {
        /* The directive. `full` while no complete mirror exists, because a
         * partial one is refused by `atlas_repo_open_git` and the way out of
         * that is a whole pass; `incremental` once one does. The decision is
         * the daemon's because the daemon is what knows the index's state -- the
         * spec's shape, where the scanner asks what is owed rather than
         * deciding it. */
        st = atlas_json_key_str(j, "directive", ri->mirror_complete ? "incremental" : "full",
                                s->err);
    }
    if (st == ATLAS_OK) {
        /* A13. The build inputs this repository's semantic index needs.
         *
         * **They are ignored files, and the mirror carries them anyway.** A
         * compilation database is a build artefact — `.gitignore` names it, and
         * the walk that mirrors tracked and untracked paths deliberately skips
         * ignored ones, because otherwise every object file would be copied.
         * But it is also an *input* Atlas cannot index without: with it absent
         * from the mirror the semantic pass failed with "the named compilation
         * database is not a readable regular file inside the repository", the
         * retry governor held on "the last attempt failed and the source has not
         * changed since", and both repositories' semantic indexes stopped a day
         * ago and stayed stopped.
         *
         * The daemon names them rather than the scanner guessing: what belongs
         * here is what *discovery accepted*, which is a fact the daemon holds
         * and the scanner has no way to derive. */
        st = atlas_json_key(j, "build_inputs", s->err);
        if (st == ATLAS_OK) {
            st = atlas_json_arr_begin(j, s->err);
        }
        atlas_buf inputs = ATLAS_BUF_INIT;
        if (st == ATLAS_OK) {
            atlas_err ignore;
            atlas_err_init(&ignore);
            /* Absent or unreadable is an empty list, not a failure: a repository
             * with no accepted build inputs is an ordinary one. */
            if (atlas_sem_accepted_inputs(s->ds->db, ri->id, &inputs, NULL, &ignore) == ATLAS_OK) {
                /* NUL-separated, repository-relative. */
                const char *p = inputs.data;
                const char *end = p != NULL ? p + inputs.len : NULL;
                while (st == ATLAS_OK && p != NULL && p < end && *p != '\0') {
                    size_t n = strlen(p);
                    atlas_buf enc = ATLAS_BUF_INIT;
                    if (atlas_path_text_encode(p, n, &enc, s->err) == ATLAS_OK) {
                        st = atlas_json_str(j, atlas_buf_cstr(&enc), s->err);
                    }
                    atlas_buf_free(&enc);
                    p += n + 1u;
                }
            }
        }
        atlas_buf_free(&inputs);
        if (st == ATLAS_OK) {
            st = atlas_json_arr_end(j, s->err);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(j, s->err);
    }
    /* **Asking is the evidence.** `scanner.poll` doubles as the heartbeat, which
     * is why the spec has no `hello`: nothing is claimed by the caller, and what
     * is recorded is that a request arrived from the uid this row names. A
     * scanner that stops polling stops being heard, and
     * `atlas_server_overlay_mirror` stops calling the index current.
     *
     * In memory, not in the index. This method runs on a read-only handle --
     * writing here failed with "attempt to write a readonly database", which
     * `test_scanner_rpc` caught -- and a heartbeat is liveness rather than a
     * durable fact anyway: a restarted daemon has heard from nobody, which is
     * the answer that refuses rather than the one that trusts. */
    if (s->ds->ctx != NULL) {
        atlas_scanner_seen_touch(s->ds->ctx->scanner_seen, ri->id);
    }
    s->status = st;
    return ATLAS_OK;
}

/* Which repositories this peer may scan. Nothing else: no directive, no
 * budget, no generation. Those arrive with the work they govern. */
static atlas_status method_scanner_poll(dispatch_state *ds, const atlas_ipc_request *req,
                                        atlas_err *err) {
    (void)req;
    atlas_status st = require_scanner(ds, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_json_key(ds->j, "repositories", err);
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    if (st != ATLAS_OK) {
        return st;
    }
    scanner_scan s;
    memset(&s, 0, sizeof(s));
    s.ds = ds;
    s.err = err;
    st = atlas_db_repo_list(ds->db, emit_cb, &s, err);
    if (st == ATLAS_OK) {
        st = s.status;
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "scanner_uid", ds->peer_uid, err);
    }
    if (st == ATLAS_OK) {
        /* How soon Atlas expects to be asked again. The cadence is Atlas', not
         * the scanner's: it is the same number the freshness rule holds a
         * scanner to, so the promise and the judgement cannot drift apart. */
        st = atlas_json_key_int(ds->j, "poll_within_ms", ATLAS_SCANNER_POLL_INTERVAL_MS, err);
    }
    return st;
}

/* One hex digit, or -1.
 *
 * Uppercase is refused rather than accepted. Two spellings of one byte would
 * mean two wire forms for one file, and a format with a choice in it is a
 * format two implementations will disagree about. */
static int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    return -1;
}

/* Decodes `hex` into `out`.
 *
 * **A source file is arbitrary bytes** — a quote, a backslash, a newline, a C0
 * control and a sequence that is not valid UTF-8 are all legal in one, and a
 * JSON string carries none of them unchanged. Measured before this existed: a
 * twelve-byte file arrived as twenty-four, because the wire value was stored
 * verbatim. So the content travels as hex and the mirror holds what the
 * repository holds.
 *
 * An odd length or a digit that is not hex is refused rather than guessed at:
 * a truncated file that reported success would be the worst of the three
 * possible failures. */
static atlas_status hex_decode(const char *hex, atlas_buf *out, atlas_err *err) {
    size_t n = strlen(hex);
    if ((n % 2u) != 0u) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "file content must be an even number of hex digits");
    }
    atlas_buf_reset(out);
    for (size_t i = 0; i < n; i += 2u) {
        int hi = hex_digit(hex[i]);
        int lo = hex_digit(hex[i + 1u]);
        if (hi < 0 || lo < 0) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "file content must be lowercase hex digits");
        }
        char b = (char)((hi << 4) | lo);
        atlas_status st = atlas_buf_append(out, &b, 1u, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    return ATLAS_OK;
}

/* Hands one file's bytes to the mirror.
 *
 * The order of the checks is the design. Everything that can refuse happens
 * before any descriptor is opened, so a refused call leaves nothing behind —
 * and the third check is the load-bearing one: `require_scanner` only asks
 * whether this peer scans *something*, and without `peer_owns` on the named
 * repository a scanner could write into a mirror belonging to a repository it
 * does not own. */
static atlas_status method_scanner_put(dispatch_state *ds, const atlas_ipc_request *req,
                                       atlas_err *err) {
    atlas_status st = require_scanner(ds, err);
    if (st != ATLAS_OK) {
        return st;
    }

    int64_t repo_id = 0;
    if (!atlas_ipc_param_int(req, "repo", &repo_id)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a put needs a repository id");
    }
    const char *path = NULL;
    if (!atlas_ipc_param_str(req, "path", &path) || path == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a put needs a path");
    }
    const char *data = NULL;
    if (!atlas_ipc_param_str(req, "data", &data) || data == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a put needs its content");
    }
    bool first = true;
    (void)atlas_ipc_param_bool(req, "first", &first);
    /* A13. A symlink's text is its content: Atlas hashes the text and never
     * opens the target, so the mirror holds a symlink rather than a file whose
     * bytes happen to be a path. Nothing follows it -- every descent here and in
     * `reconcile.c` is `O_NOFOLLOW` -- so the text is data and never a route. */
    bool is_symlink = false;
    (void)atlas_ipc_param_bool(req, "symlink", &is_symlink);
    /* The one mode bit git tracks. Absent is false, which is the ordinary file
     * and the conservative answer: a file wrongly marked executable would show
     * as modified, and so would one wrongly left plain, but only the second is
     * what an absent key can produce. */
    bool is_exec = false;
    (void)atlas_ipc_param_bool(req, "exec", &is_exec);

    atlas_repo_info ri;
    atlas_repo_info_init(&ri);
    bool found = false;
    st = atlas_db_repo_get_by_id(ds->db, repo_id, &ri, &found, err);
    if (st == ATLAS_OK && !found) {
        /* The id is one the caller supplied, so the refusal says only that it
         * resolved to nothing — it does not describe the registry. */
        st = atlas_err_set(err, ATLAS_ERR_REPO, "no repository has that id");
    }
    if (st == ATLAS_OK && !peer_owns(ds, &ri)) {
        st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                           "uid %lld is not this repository's scanner", (long long)ds->peer_uid);
    }
    atlas_repo_info_free(&ri);
    if (st != ATLAS_OK) {
        return st;
    }

    atlas_buf content = ATLAS_BUF_INIT;
    st = hex_decode(data, &content, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&content);
        return st;
    }

    int root = -1;
    /* Into the staging generation, never the one readers use: a refresh must not
     * make a finished mirror unreadable. `scanner.state` with `complete` true
     * publishes it. */
    st = atlas_mirror_open_staging(ds->ctx->data_dir, repo_id, &root, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&content);
        return st;
    }
    st = is_symlink ? atlas_mirror_put_symlink(root, path, strlen(path), content.data, content.len,
                                               err)
                    : atlas_mirror_put(root, path, strlen(path), first, is_exec, content.data,
                                       content.len, err);
    (void)close(root);
    size_t written = content.len;
    atlas_buf_free(&content);
    if (st != ATLAS_OK) {
        return st;
    }
    /* The decoded byte count, so a caller can tell a short write from a
     * successful one rather than inferring it from the hex length. */
    return atlas_json_key_int(ds->j, "written", (int64_t)written, err);
}

/* A13. Tells the daemon what a run claims about the mirror it just wrote.
 *
 * Two calls per repository per run. `complete=false` at the start is what makes
 * a crash leave the mirror refused rather than trusted: the value that survives
 * a failure is the one that costs a refusal, never the one that costs a delete
 * sweep against a half-written tree.
 *
 * The peer is checked exactly as `scanner.put` checks it — being some
 * repository's scanner is not being *this* repository's. */
static atlas_status method_scanner_state(dispatch_state *ds, const atlas_ipc_request *req,
                                         atlas_err *err) {
    atlas_status st = require_scanner(ds, err);
    if (st != ATLAS_OK) {
        return st;
    }
    int64_t repo_id = 0;
    if (!atlas_ipc_param_int(req, "repo", &repo_id)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a mirror state needs a repository id");
    }
    bool complete = false;
    (void)atlas_ipc_param_bool(req, "complete", &complete);

    atlas_repo_info ri;
    atlas_repo_info_init(&ri);
    bool found = false;
    st = atlas_db_repo_get_by_id(ds->db, repo_id, &ri, &found, err);
    if (st == ATLAS_OK && !found) {
        st = atlas_err_set(err, ATLAS_ERR_REPO, "no repository has that id");
    }
    if (st == ATLAS_OK && !peer_owns(ds, &ri)) {
        st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                           "uid %lld is not this repository's scanner", (long long)ds->peer_uid);
    }
    atlas_repo_info_free(&ri);
    if (st != ATLAS_OK) {
        return st;
    }

    /* Through the writer, because this is a write and every dispatch handle is
     * read-only. Doing it here failed with "attempt to write a readonly
     * database" on every call, and silently, because the scanner ignored the
     * result -- so a mirror was never marked complete and no repository could
     * ever be read. The time is Atlas', set by the writer: a scanner reporting
     * when it thinks it finished would let a client's clock decide whether an
     * index is current. */
    if (ds->ctx == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no context to record a mirror state");
    }
    /* Publish before recording, so a crash between the two leaves a readable
     * mirror the row does not yet vouch for -- which costs a refusal -- rather
     * than a row vouching for a generation that was never published. */
    /* Publishing is a filesystem operation and the recording is a database
     * write, so they are ordered rather than combined: publish first, so a
     * crash between them leaves a readable mirror the row does not yet vouch
     * for -- which costs a refusal -- rather than a row vouching for a generation
     * that was never published. */
    if (complete) {
        st = atlas_mirror_publish(ds->ctx->data_dir, repo_id, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    if (ds->ctx->writer == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no writer to record a mirror state");
    }
    /* Accepted, not completed, and that is A8-CI's rule rather than a shortcut:
     * an operation that can outlast a client's patience does not run in the
     * serve loop, and the client is answered when the work is accepted.
     *
     * The writer may legitimately be minutes inside an unbounded job — and on a
     * mirror-backed repository it is routinely twenty seconds inside a bounded
     * one, because publishing a generation replaces every inode and the whole
     * repository is re-hashed. A scanner that waited timed out reading its
     * response while the write it asked for was still on its way, and then
     * reported a run as unrecorded that had in fact been recorded.
     *
     * The next `scanner.poll` carries the outcome: its directive is `full`
     * while no complete mirror exists, so a job that fails is answered by being
     * asked for again. */
    st = atlas_writer_submit_mirror_state(ds->ctx->writer, repo_id, complete, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "complete", complete, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "accepted", true, err);
    }
    return st;
}

/* A13. "This path has not changed since I last sent it; carry it forward."
 *
 * A separate method from `scanner.put` and never a flag on it, for the reason
 * `cancel` is separate from `yield`: they are different assertions. A put says
 * "here are the bytes"; this says "you already have them", and only one of the
 * two can be refused by the daemon simply not having the file.
 *
 * **It carries no claim the daemon acts on.** The scanner's belief that a file
 * is unchanged decides nothing here: the daemon links whatever its own published
 * generation holds at that path, or answers `kept: false` and waits to be sent
 * the bytes. So a scanner whose memory has outlived the mirror — a daemon
 * restarted, a mirror removed, a generation this scanner never built — corrects
 * itself on the next call rather than producing a wrong mirror.
 *
 * What the scanner's belief *does* decide is whether the source file changed,
 * and that is A1's eight-field identity with ctime in it, which is the same
 * evidence a reconciliation already uses to skip hashing when no event named the
 * path. This does not weaken that rule; it applies it one process further out. */
static atlas_status method_scanner_keep(dispatch_state *ds, const atlas_ipc_request *req,
                                        atlas_err *err) {
    atlas_status st = require_scanner(ds, err);
    if (st != ATLAS_OK) {
        return st;
    }

    int64_t repo_id = 0;
    if (!atlas_ipc_param_int(req, "repo", &repo_id)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a keep needs a repository id");
    }
    const atlas_ipc_array *arr = NULL;
    if (!atlas_ipc_param_array(req, "paths", &arr)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a keep needs a \"paths\" array");
    }
    const size_t n = atlas_ipc_array_len(arr);
    if (n == 0 || n > ATLAS_SCANNER_KEEP_MAX_PATHS) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "\"paths\" holds %zu entries; the bound is 1..%d", n,
                             ATLAS_SCANNER_KEEP_MAX_PATHS);
    }

    atlas_repo_info ri;
    atlas_repo_info_init(&ri);
    bool found = false;
    st = atlas_db_repo_get_by_id(ds->db, repo_id, &ri, &found, err);
    if (st == ATLAS_OK && !found) {
        st = atlas_err_set(err, ATLAS_ERR_REPO, "no repository has that id");
    }
    if (st == ATLAS_OK && !peer_owns(ds, &ri)) {
        st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                           "uid %lld is not this repository's scanner", (long long)ds->peer_uid);
    }
    atlas_repo_info_free(&ri);
    if (st != ATLAS_OK) {
        return st;
    }

    /* Bounded by the parameter check above, so this is a fixed frame rather than
     * an allocation sized by a caller. */
    atlas_mirror_path paths[ATLAS_SCANNER_KEEP_MAX_PATHS];
    bool kept[ATLAS_SCANNER_KEEP_MAX_PATHS];
    for (size_t i = 0; i < n; i++) {
        const char *p = NULL;
        if (!atlas_ipc_array_str(arr, i, &p) || p == NULL) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "every entry of \"paths\" is a string");
        }
        paths[i].rel = p;
        paths[i].rel_len = strlen(p);
    }

    st = atlas_mirror_keep_many(ds->ctx->data_dir, repo_id, paths, n, kept, err);
    if (st != ATLAS_OK) {
        return st;
    }

    /* The paths that were **not** carried, so the caller knows exactly what to
     * send rather than inferring it from a count. An empty array is the ordinary
     * answer and means everything was carried. */
    int64_t carried = 0;
    st = atlas_json_key(ds->j, "resend", err);
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    for (size_t i = 0; i < n && st == ATLAS_OK; i++) {
        if (kept[i]) {
            carried++;
        } else {
            st = atlas_json_str(ds->j, (const char *)paths[i].rel, err);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "carried", carried, err);
    }
    return st;
}

static const atlas_method_entry SCANNER_METHODS[] = {
    {"scanner.poll", method_scanner_poll},
    {"scanner.put", method_scanner_put},
    {"scanner.keep", method_scanner_keep},
    {"scanner.state", method_scanner_state},
};

const atlas_method_entry *atlas_server_scanner_methods(size_t *count_out) {
    if (count_out != NULL) {
        *count_out = sizeof SCANNER_METHODS / sizeof SCANNER_METHODS[0];
    }
    return SCANNER_METHODS;
}
