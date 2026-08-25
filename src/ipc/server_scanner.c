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
#include "atlas/db.h"
#include "atlas/ipc.h"
#include "atlas/safetext.h"
#include "ipc/server_internal.h"

#include <string.h>

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
        st = atlas_json_obj_end(j, s->err);
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
    return st;
}

static const atlas_method_entry SCANNER_METHODS[] = {
    {"scanner.poll", method_scanner_poll},
};

const atlas_method_entry *atlas_server_scanner_methods(size_t *count_out) {
    if (count_out != NULL) {
        *count_out = sizeof SCANNER_METHODS / sizeof SCANNER_METHODS[0];
    }
    return SCANNER_METHODS;
}
