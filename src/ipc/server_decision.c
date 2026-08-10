/* Atlas - the A4 daemon method group: decision documents and their lifecycle.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Reads run on the per-request read-only handle, like every other read in the
 * server. Writes are handed to the writer thread as one typed operation, like
 * every A2 write — including the ones the operator channel authorises, because
 * issuing a challenge is a write and there is no path to
 * `atlas_decision_apply` that does not run on the writer.
 *
 * **The parameter surface is the boundary.** Three properties of it are load
 * bearing and are checked here rather than downstream:
 *
 *   1. `token` and `confirmation` are read by `decision.approve`,
 *      `decision.reject` and `decision.supersede` and by nothing else. No
 *      proposal method reads them, so a proposal cannot carry an approval.
 *   2. `actor` is parsed against the closed vocabulary and then checked against
 *      `atlas_decision_actor_writable_by_adapter`, so a request that says
 *      `LOCAL_OPERATOR_CONFIRMED` is refused here as well as at the write
 *      point. Two refusals, because this one produces a better message and
 *      that one is the guarantee.
 *   3. Every text field is length-checked and validated as strict UTF-8 before
 *      anything is queued. `atlas_decision_revision_validate` runs again at the
 *      write point; a bound checked only at the edge is a bound the next edge
 *      will not have.
 *
 * Decision prose is repository- and model-authored, so every string that leaves
 * here is encoded with `atlas_safe()` — approval changes a record's status, not
 * the nature of its bytes.
 */
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/decision_ops.h"
#include "atlas/gate.h"
#include "atlas/pathrep.h"
#include "ipc/server_internal.h"

/* A lifecycle write is a short, bounded transaction. The timeout is the same
 * as A2's for the same reason: a caller that waits longer than this is waiting
 * on something that has gone wrong rather than on work. */
#define DECISION_WRITE_TIMEOUT_MS 5000

/* --- parameter helpers ------------------------------------------------------ */

static atlas_status take_text(const atlas_ipc_request *req, const char *key, size_t max,
                              bool allow_newlines, atlas_buf *out, atlas_err *err) {
    const char *v = NULL;
    if (!atlas_ipc_param_str(req, key, &v) || v == NULL) {
        return ATLAS_OK;
    }
    size_t n = strlen(v);
    atlas_status st = atlas_decision_check_text(key, v, n, max, allow_newlines, err);
    if (st != ATLAS_OK) {
        return st;
    }
    return atlas_buf_set(out, v, n, err);
}

/* A decision id, checked for shape before it is used as a lookup key. */
static atlas_status take_uid(const atlas_ipc_request *req, const char *key, bool required,
                             atlas_buf *out, atlas_err *err) {
    const char *v = NULL;
    if (!atlas_ipc_param_str(req, key, &v) || v == NULL || v[0] == '\0') {
        if (!required) {
            return ATLAS_OK;
        }
        return atlas_err_set(err, ATLAS_ERR_USAGE, "this method needs a \"%s\" decision id", key);
    }
    if (!atlas_decision_uid_is_valid(v)) {
        /* The value is not echoed. It failed a shape check, so it is arbitrary
         * caller bytes, and the message says what a valid one looks like
         * instead — which is more useful anyway. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "\"%s\" is not a decision id; they look like %s followed by %u "
                             "lowercase hex characters",
                             key, ATLAS_DECISION_UID_PREFIX, (unsigned)ATLAS_DECISION_UID_HEX);
    }
    return atlas_buf_set_str(out, v, err);
}

/* Where. `root` wins over `repo`, exactly as in the A2 group. */
static atlas_status take_where(const atlas_ipc_request *req, atlas_decision_op *op,
                               atlas_err *err) {
    const char *root = NULL;
    const char *repo = NULL;
    if (atlas_ipc_param_str(req, "root", &root) && root != NULL && root[0] != '\0') {
        return atlas_buf_set_str(&op->root, root, err);
    }
    if (atlas_ipc_param_str(req, "repo", &repo) && repo != NULL && repo[0] != '\0') {
        atlas_status st = atlas_db_check_repo_name(repo, err);
        if (st != ATLAS_OK) {
            return st;
        }
        return atlas_buf_set_str(&op->repo_name, repo, err);
    }
    return atlas_err_set(err, ATLAS_ERR_USAGE,
                         "this method needs a \"repo\" name or a \"root\" path");
}

static atlas_status take_identity(const atlas_ipc_request *req, atlas_decision_op *op,
                                  atlas_err *err) {
    const char *v = NULL;
    atlas_status st = ATLAS_OK;
    if (atlas_ipc_param_str(req, "provider", &v) && v != NULL) {
        st = atlas_buf_set_str(&op->provider, v, err);
    }
    if (st == ATLAS_OK && atlas_ipc_param_str(req, "client", &v) && v != NULL) {
        st = atlas_buf_set_str(&op->client, v, err);
    }
    if (st == ATLAS_OK && atlas_ipc_param_str(req, "session_key", &v) && v != NULL) {
        st = atlas_buf_set_str(&op->session_key, v, err);
    }
    if (st == ATLAS_OK && atlas_ipc_param_str(req, "dedup_key", &v) && v != NULL) {
        st = atlas_buf_set_str(&op->dedup_key, v, err);
    }
    return st;
}

/* The actor a proposal claims, checked twice on purpose.
 *
 * `atlas_decision_actor_writable_by_adapter` is the guarantee and runs at the
 * write point; this runs here so that a caller gets a message naming what it
 * asked for, and so that nothing operator-shaped is ever queued. */
static atlas_status take_actor(const atlas_ipc_request *req, atlas_decision_op *op,
                               atlas_err *err) {
    const char *v = NULL;
    op->revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
    if (!atlas_ipc_param_str(req, "actor", &v) || v == NULL) {
        return ATLAS_OK;
    }
    atlas_decision_actor a;
    if (!atlas_decision_actor_parse(v, &a)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "\"actor\" is not one Atlas recognises; it is MODEL_PROPOSAL or "
                             "MODEL_INFERENCE");
    }
    if (!atlas_decision_actor_writable_by_adapter(a)) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "a decision recorded over this interface is a proposal: actor %s is "
                             "refused, because an approval happens only through Atlas' local "
                             "operator channel and cannot be asserted in a request",
                             atlas_decision_actor_name(a));
    }
    op->revision.proposed_by = a;
    return ATLAS_OK;
}

static atlas_status take_scope(const atlas_ipc_request *req, atlas_decision_op *op,
                               atlas_err *err) {
    const char *v = NULL;
    op->revision.scope = ATLAS_DECISION_SCOPE_UNKNOWN;
    if (!atlas_ipc_param_str(req, "scope", &v) || v == NULL) {
        return ATLAS_OK;
    }
    if (!atlas_decision_scope_parse(v, &op->revision.scope)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "\"scope\" is UNKNOWN, REPOSITORY, SUBSYSTEM or PATHS");
    }
    return ATLAS_OK;
}

static atlas_status take_alternatives(const atlas_ipc_request *req, atlas_decision_op *op,
                                      atlas_err *err) {
    const atlas_ipc_array *arr = NULL;
    if (!atlas_ipc_param_array(req, "alternatives", &arr)) {
        return ATLAS_OK;
    }
    size_t n = atlas_ipc_array_len(arr);
    if (n > ATLAS_DECISION_MAX_ALTERNATIVES) {
        /* Refused rather than truncated: a decision that silently records three
         * of five alternatives claims the other two were never considered. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a decision may list at most %d alternatives, and %zu were given",
                             ATLAS_DECISION_MAX_ALTERNATIVES, n);
    }
    for (size_t i = 0; i < n; i++) {
        const char *s = NULL;
        if (!atlas_ipc_array_str(arr, i, &s) || s == NULL) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "\"alternatives\" must be strings");
        }
        atlas_status st = atlas_decision_revision_add_alternative(&op->revision, s, strlen(s), err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    return ATLAS_OK;
}

/* Copies the indexed content hash out of the borrowed row. Borrowed pointers
 * are valid only for the call, so it is copied rather than kept. */
static atlas_status take_file_hash(const atlas_file_row *row, void *ud, atlas_err *err) {
    atlas_buf *out = (atlas_buf *)ud;
    if (row->deleted || row->content_hash == NULL || row->content_hash[0] == '\0') {
        return ATLAS_OK;
    }
    return atlas_buf_set_str(out, row->content_hash, err);
}

/* Path links, by raw bytes.
 *
 * `path_text` is the accepted input form everywhere in Atlas: a path is bytes,
 * a JSON string is text, and the `%XX` encoding is what carries the one through
 * the other losslessly. Decoding here rather than storing the text form means a
 * link keyed on bytes matches a `files` row keyed on the same bytes. */
static atlas_status take_path_links(dispatch_state *ds, int64_t repo_id, const char *head,
                                    const atlas_ipc_request *req, atlas_decision_op *op,
                                    atlas_err *err) {
    const atlas_ipc_array *arr = NULL;
    if (!atlas_ipc_param_array(req, "paths", &arr)) {
        return ATLAS_OK;
    }
    size_t n = atlas_ipc_array_len(arr);
    if (n > ATLAS_DECISION_MAX_LINKS) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a decision may carry at most %d links, and %zu paths were given",
                             ATLAS_DECISION_MAX_LINKS, n);
    }
    for (size_t i = 0; i < n; i++) {
        const char *s = NULL;
        if (!atlas_ipc_array_str(arr, i, &s) || s == NULL) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "\"paths\" must be strings");
        }
        atlas_decision_link l;
        atlas_decision_link_init(&l, ATLAS_DECISION_LINK_PATH);
        atlas_status st = atlas_path_text_decode(s, strlen(s), &l.path_raw, err);
        if (st == ATLAS_OK) {
            st = atlas_path_text_encode(l.path_raw.data, l.path_raw.len, &l.path_text, err);
        }
        /* The snapshot: the basis commit and the file's content hash as the
         * index holds it now.
         *
         * Taken from Atlas' own index rather than from the request, because a
         * caller-supplied hash would let a caller assert that a link is
         * current — which is exactly the claim the snapshot exists to make
         * checkable. A file Atlas has not indexed gets no hash, and its link
         * resolves UNKNOWN rather than a false CURRENT. */
        if (st == ATLAS_OK && head != NULL && head[0] != '\0') {
            st = atlas_buf_set_str(&l.basis_commit, head, err);
        }
        if (st == ATLAS_OK) {
            bool found = false;
            st = atlas_db_file_get(ds->db, repo_id, l.path_raw.data, l.path_raw.len,
                                   take_file_hash, &l.file_content_hash, &found, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_decision_revision_add_link(&op->revision, &l, err);
        }
        atlas_decision_link_free(&l);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    return ATLAS_OK;
}

/* Commit links, validated as hex before they are stored. */
static atlas_status take_commit_links(const atlas_ipc_request *req, atlas_decision_op *op,
                                      atlas_err *err) {
    const atlas_ipc_array *arr = NULL;
    if (!atlas_ipc_param_array(req, "commits", &arr)) {
        return ATLAS_OK;
    }
    size_t n = atlas_ipc_array_len(arr);
    for (size_t i = 0; i < n && i < ATLAS_DECISION_MAX_LINKS; i++) {
        const char *s = NULL;
        if (!atlas_ipc_array_str(arr, i, &s) || s == NULL) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "\"commits\" must be strings");
        }
        size_t len = strlen(s);
        if (len < 4u || len > ATLAS_OID_HEX_MAX) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "a commit link is 4 to %u hex characters",
                                 (unsigned)ATLAS_OID_HEX_MAX);
        }
        for (size_t k = 0; k < len; k++) {
            bool hex = (s[k] >= '0' && s[k] <= '9') || (s[k] >= 'a' && s[k] <= 'f') ||
                       (s[k] >= 'A' && s[k] <= 'F');
            if (!hex) {
                return atlas_err_set(err, ATLAS_ERR_USAGE, "a commit link must be hex");
            }
        }
        atlas_decision_link l;
        atlas_decision_link_init(&l, ATLAS_DECISION_LINK_COMMIT);
        atlas_status st = atlas_buf_set(&l.commit_oid, s, len, err);
        if (st == ATLAS_OK) {
            st = atlas_decision_revision_add_link(&op->revision, &l, err);
        }
        atlas_decision_link_free(&l);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    return ATLAS_OK;
}

/* Symbol links, as durable selector snapshots.
 *
 * The snapshot's provenance — the basis commit, the file's content hash and the
 * analyzer identity — is filled in **by Atlas from its own index**, never taken
 * from the request. A caller-supplied content hash would let a caller assert
 * that a link is current, which is exactly the claim the snapshot exists to
 * make checkable. */
static atlas_status snapshot_symbol(dispatch_state *ds, int64_t repo_id, atlas_decision_link *l,
                                    const char *head, atlas_err *err) {
    if (head != NULL && head[0] != '\0') {
        atlas_status st = atlas_buf_set_str(&l->basis_commit, head, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    /* Which analyzer's facts this snapshot was taken against. Compiled-in
     * constants, so nothing repository- or model-controlled reaches them. */
    atlas_status st = atlas_buf_set_str(&l->analyzer_name, ATLAS_CODE_ANALYZER_ID, err);
    if (st != ATLAS_OK) {
        return st;
    }
    l->analyzer_version = (int64_t)ATLAS_CODE_ANALYZER_VERSION;
    if (l->path_raw.len == 0) {
        return ATLAS_OK;
    }
    /* The content hash of the file the symbol was in, as the index holds it
     * now. Absent when the file is not indexed, which makes the link's currency
     * UNKNOWN rather than a false CURRENT. */
    bool found = false;
    return atlas_db_file_get(ds->db, repo_id, l->path_raw.data, l->path_raw.len, take_file_hash,
                             &l->file_content_hash, &found, err);
}

static atlas_status take_symbol_links(dispatch_state *ds, const atlas_ipc_request *req,
                                      atlas_decision_op *op, int64_t repo_id, const char *head,
                                      atlas_err *err) {
    const atlas_ipc_array *arr = NULL;
    if (!atlas_ipc_param_array(req, "symbols", &arr)) {
        return ATLAS_OK;
    }
    const atlas_ipc_array *files = NULL;
    (void)atlas_ipc_param_array(req, "symbol_files", &files);
    const atlas_ipc_array *kinds = NULL;
    (void)atlas_ipc_param_array(req, "symbol_kinds", &kinds);

    size_t n = atlas_ipc_array_len(arr);
    for (size_t i = 0; i < n && i < ATLAS_DECISION_MAX_LINKS; i++) {
        const char *name = NULL;
        if (!atlas_ipc_array_str(arr, i, &name) || name == NULL) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "\"symbols\" must be strings");
        }
        size_t nlen = strlen(name);
        if (nlen == 0 || nlen > ATLAS_CODE_MAX_NAME_BYTES) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "a symbol name is 1 to %u bytes",
                                 (unsigned)ATLAS_CODE_MAX_NAME_BYTES);
        }
        atlas_decision_link l;
        atlas_decision_link_init(&l, ATLAS_DECISION_LINK_SYMBOL);
        atlas_status st = atlas_buf_set(&l.symbol_name, name, nlen, err);
        if (st == ATLAS_OK) {
            st = atlas_path_text_encode(name, nlen, &l.symbol_name_text, err);
        }
        const char *file = NULL;
        if (st == ATLAS_OK && files != NULL && atlas_ipc_array_str(files, i, &file) &&
            file != NULL && file[0] != '\0') {
            st = atlas_path_text_decode(file, strlen(file), &l.path_raw, err);
            if (st == ATLAS_OK) {
                st = atlas_path_text_encode(l.path_raw.data, l.path_raw.len, &l.path_text, err);
            }
        }
        const char *kind = NULL;
        if (st == ATLAS_OK && kinds != NULL && atlas_ipc_array_str(kinds, i, &kind) &&
            kind != NULL && kind[0] != '\0') {
            atlas_code_symbol_kind sk;
            if (!atlas_code_symbol_kind_parse(kind, &sk)) {
                st = atlas_err_set(err, ATLAS_ERR_USAGE,
                                   "\"symbol_kinds\" must name an Atlas symbol kind");
            } else {
                st = atlas_buf_set_str(&l.symbol_kind, atlas_code_symbol_kind_name(sk), err);
            }
        }
        if (st == ATLAS_OK) {
            st = snapshot_symbol(ds, repo_id, &l, head, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_decision_revision_add_link(&op->revision, &l, err);
        }
        atlas_decision_link_free(&l);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    return ATLAS_OK;
}

/* --- submitting -------------------------------------------------------------- */

static atlas_decision_op *op_new(atlas_decision_op_kind kind) {
    atlas_decision_op *op = calloc(1u, sizeof(*op));
    if (op != NULL) {
        atlas_decision_op_init(op, kind);
    }
    return op;
}

static atlas_status submit(dispatch_state *ds, atlas_decision_op *op,
                           atlas_decision_result *result, atlas_err *err) {
    if (ds->ctx->writer == NULL) {
        atlas_decision_op_free(op);
        free(op);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "this Atlas daemon has no writer, so nothing can be recorded");
    }
    /* Takes ownership unconditionally, including on its own failure paths. */
    return atlas_writer_decision(ds->ctx->writer, op, DECISION_WRITE_TIMEOUT_MS, result, err);
}

/* The common members of a write result. Nothing here is repository prose except
 * `title`, which is encoded. */
static atlas_status write_result(dispatch_state *ds, const atlas_decision_result *r,
                                 atlas_err *err) {
    atlas_status st = atlas_json_key_str(ds->j, "repo", atlas_buf_cstr(&r->repo_name), err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "repo_id", r->repo_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "decision", atlas_buf_cstr(&r->uid), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "revision", r->revision_no, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "content_hash", r->content_hash, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "state", atlas_decision_state_name(r->state), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "created", r->document_created, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "duplicate", r->duplicate, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "session_unbound", r->session_unbound, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "unbound_reason", r->unbound_reason, err);
    }
    return st;
}

/* --- the write methods --------------------------------------------------------- */

static atlas_status build_revision_op(dispatch_state *ds, const atlas_ipc_request *req,
                                      atlas_decision_op *op, atlas_err *err) {
    /* The repository is resolved once, here, on the read handle. Both link
     * kinds need it for their snapshots, and resolving it twice could resolve
     * to two different repositories between the two calls. */
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_server_require_repo(ds, req, &info, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }
    st = take_where(req, op, err);
    if (st == ATLAS_OK) {
        st = take_identity(req, op, err);
    }
    if (st == ATLAS_OK) {
        st = take_actor(req, op, err);
    }
    if (st == ATLAS_OK) {
        st = take_scope(req, op, err);
    }
    if (st == ATLAS_OK) {
        st = take_text(req, "title", ATLAS_DECISION_TITLE_MAX, false, &op->revision.title, err);
    }
    if (st == ATLAS_OK) {
        st = take_text(req, "context", ATLAS_DECISION_TEXT_MAX, true, &op->revision.context_text,
                       err);
    }
    if (st == ATLAS_OK) {
        st = take_text(req, "decision", ATLAS_DECISION_TEXT_MAX, true, &op->revision.decision_text,
                       err);
    }
    if (st == ATLAS_OK) {
        st = take_text(req, "rationale", ATLAS_DECISION_TEXT_MAX, true,
                       &op->revision.rationale_text, err);
    }
    if (st == ATLAS_OK) {
        st = take_text(req, "consequences", ATLAS_DECISION_TEXT_MAX, true,
                       &op->revision.consequences_text, err);
    }
    if (st == ATLAS_OK) {
        st = take_alternatives(req, op, err);
    }
    if (st == ATLAS_OK) {
        st = take_path_links(ds, info.id, info.scanned_head, req, op, err);
    }
    if (st == ATLAS_OK) {
        st = take_commit_links(req, op, err);
    }
    if (st == ATLAS_OK) {
        st = take_symbol_links(ds, req, op, info.id, info.scanned_head, err);
    }
    /* Validated here as well as at the write point. Both, because this one
     * produces the better message and that one is the guarantee. */
    if (st == ATLAS_OK) {
        st = atlas_decision_revision_validate(&op->revision, err);
    }
    atlas_repo_info_free(&info);
    return st;
}

static atlas_status method_propose(dispatch_state *ds, const atlas_ipc_request *req,
                                   atlas_err *err) {
    atlas_decision_op *op = op_new(ATLAS_DECISION_OP_PROPOSE);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    atlas_status st = build_revision_op(ds, req, op, err);
    if (st != ATLAS_OK) {
        atlas_decision_op_free(op);
        free(op);
        return st;
    }
    atlas_decision_result result;
    atlas_decision_result_init(&result);
    st = submit(ds, op, &result, err);
    if (st == ATLAS_OK) {
        st = write_result(ds, &result, err);
    }
    if (st == ATLAS_OK) {
        /* Said in every proposal response, so a client cannot infer approval
         * from a successful write. */
        st = atlas_json_key_str(ds->j, "approval",
                                "a proposal is not approved; approval happens only through the "
                                "interactive Atlas CLI on a terminal",
                                err);
    }
    atlas_decision_result_free(&result);
    return st;
}

static atlas_status method_revise(dispatch_state *ds, const atlas_ipc_request *req,
                                  atlas_err *err) {
    atlas_decision_op *op = op_new(ATLAS_DECISION_OP_REVISE);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    atlas_status st = take_uid(req, "decision", true, &op->uid, err);
    if (st == ATLAS_OK) {
        st = build_revision_op(ds, req, op, err);
    }
    if (st != ATLAS_OK) {
        atlas_decision_op_free(op);
        free(op);
        return st;
    }
    atlas_decision_result result;
    atlas_decision_result_init(&result);
    st = submit(ds, op, &result, err);
    if (st == ATLAS_OK) {
        st = write_result(ds, &result, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "approval",
                                "a revision is proposed, never applied to what is approved; the "
                                "approved revision stays effective until a replacement is "
                                "approved through the interactive Atlas CLI",
                                err);
    }
    atlas_decision_result_free(&result);
    return st;
}

static atlas_status method_promote(dispatch_state *ds, const atlas_ipc_request *req,
                                   atlas_err *err) {
    atlas_decision_op *op = op_new(ATLAS_DECISION_OP_PROMOTE);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    atlas_status st = take_where(req, op, err);
    if (st == ATLAS_OK && !atlas_ipc_param_int(req, "legacy_id", &op->legacy_id)) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE,
                           "decision.promote needs the A2 proposal's \"legacy_id\"");
    }
    if (st != ATLAS_OK) {
        atlas_decision_op_free(op);
        free(op);
        return st;
    }
    atlas_decision_result result;
    atlas_decision_result_init(&result);
    st = submit(ds, op, &result, err);
    if (st == ATLAS_OK) {
        st = write_result(ds, &result, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "approval",
                                "a promoted A2 proposal is PROPOSED; the A2 row is unchanged and "
                                "was never approved",
                                err);
    }
    atlas_decision_result_free(&result);
    return st;
}

/* --- the operator channel ------------------------------------------------------- */

static atlas_status method_challenge(dispatch_state *ds, const atlas_ipc_request *req,
                                     atlas_err *err) {
    atlas_decision_op *op = op_new(ATLAS_DECISION_OP_CHALLENGE);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    atlas_status st = take_where(req, op, err);
    if (st == ATLAS_OK) {
        st = take_uid(req, "decision", true, &op->uid, err);
    }
    if (st == ATLAS_OK) {
        st = take_uid(req, "replacement", false, &op->replacement_uid, err);
    }
    if (st == ATLAS_OK) {
        /* A6. The assessment the caller displayed, carried into the capability
         * so the eventual record preserves what was shown rather than what a
         * later recomputation would produce. Both are checked against their
         * closed vocabularies at the write point: a request is not the
         * authority on what an A6 reason code is. */
        const char *fresh = NULL;
        if (atlas_ipc_param_str(req, "prior_freshness", &fresh) && fresh != NULL) {
            st = atlas_buf_set_str(&op->prior_freshness, fresh, err);
        }
        const char *reasons = NULL;
        if (st == ATLAS_OK && atlas_ipc_param_str(req, "prior_reasons", &reasons) &&
            reasons != NULL) {
            st = atlas_buf_set_str(&op->prior_reasons, reasons, err);
        }
    }
    if (st == ATLAS_OK) {
        (void)atlas_ipc_param_int(req, "revision", &op->expect_revision_no);
        const char *intent = NULL;
        op->intent = ATLAS_DECISION_INTENT_APPROVE;
        if (atlas_ipc_param_str(req, "intent", &intent) && intent != NULL) {
            /* Parsed against the closed vocabulary, with no default: an
             * unrecognised intent must not become "approve". */
            if (!atlas_decision_intent_parse(intent, &op->intent)) {
                st = atlas_err_set(err, ATLAS_ERR_USAGE,
                                   "\"intent\" is approve, reject, supersede or revalidate");
            }
        }
    }
    if (st != ATLAS_OK) {
        atlas_decision_op_free(op);
        free(op);
        return st;
    }
    atlas_decision_result result;
    atlas_decision_result_init(&result);
    st = submit(ds, op, &result, err);
    if (st == ATLAS_OK) {
        st = write_result(ds, &result, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "token", atlas_buf_cstr(&result.token), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "confirm", result.confirm, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "expires_at", result.expires_at, err);
    }
    if (st == ATLAS_OK) {
        /* The title is model- or operator-authored prose. It is displayed to
         * the operator so they can see what they are confirming, and it is
         * encoded on the way out like every other untrusted value. */
        st = atlas_json_key_str(ds->j, "title", atlas_safe(&ds->safe, atlas_buf_cstr(&result.title)),
                                err);
    }
    atlas_decision_result_free(&result);
    return st;
}

/* Approve, reject and supersede. One function, because they differ only in the
 * operation kind and the intent the capability must carry — and three copies of
 * a capability check is three places for one of them to be weaker. */
static atlas_status spend_method(dispatch_state *ds, const atlas_ipc_request *req,
                                 atlas_decision_op_kind kind, atlas_err *err) {
    atlas_decision_op *op = op_new(kind);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    atlas_status st = take_where(req, op, err);
    if (st == ATLAS_OK) {
        st = take_uid(req, "decision", true, &op->uid, err);
    }
    /* `token` and `confirmation` are read here and in no other method, which is
     * what makes "a proposal cannot carry an approval" a property of the
     * parameter surface rather than of a check. */
    const char *v = NULL;
    if (st == ATLAS_OK) {
        if (!atlas_ipc_param_str(req, "token", &v) || v == NULL || v[0] == '\0') {
            st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                               "this operation needs an approval challenge issued by "
                               "decision.challenge");
        } else if (strlen(v) != ATLAS_DECISION_CHALLENGE_HEX) {
            st = atlas_err_set(err, ATLAS_ERR_INTEGRITY, "that is not an Atlas approval challenge");
        } else {
            st = atlas_buf_set_str(&op->token, v, err);
        }
    }
    if (st == ATLAS_OK) {
        if (!atlas_ipc_param_str(req, "confirmation", &v) || v == NULL) {
            st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                               "this operation needs the confirmation shown at the prompt");
        } else if (strlen(v) > ATLAS_DECISION_CONFIRM_MAX - 1u) {
            st = atlas_err_set(err, ATLAS_ERR_INTEGRITY, "that confirmation is too long");
        } else {
            st = atlas_buf_set_str(&op->confirmation, v, err);
        }
    }
    if (st != ATLAS_OK) {
        atlas_decision_op_free(op);
        free(op);
        return st;
    }
    atlas_decision_result result;
    atlas_decision_result_init(&result);
    st = submit(ds, op, &result, err);
    if (st == ATLAS_OK) {
        st = write_result(ds, &result, err);
    }
    if (st == ATLAS_OK && result.superseded_revision_no > 0) {
        st = atlas_json_key_int(ds->j, "superseded_revision", result.superseded_revision_no, err);
    }
    if (st == ATLAS_OK && result.replaced_by_uid.len > 0) {
        st = atlas_json_key_str(ds->j, "replaced_by", atlas_buf_cstr(&result.replaced_by_uid), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "actor", "LOCAL_OPERATOR_CONFIRMED", err);
    }
    if (st == ATLAS_OK) {
        /* Said in the response, not only in the documentation. A client that
         * reads this cannot honestly report that Atlas identified a person. */
        st = atlas_json_key_str(
            ds->j, "actor_means",
            "an explicit action arrived through Atlas' local operator channel. This does not "
            "identify a person, does not prove a person was present, and is not a signature.",
            err);
    }
    atlas_decision_result_free(&result);
    return st;
}

static atlas_status method_approve(dispatch_state *ds, const atlas_ipc_request *req,
                                   atlas_err *err) {
    return spend_method(ds, req, ATLAS_DECISION_OP_APPROVE, err);
}

static atlas_status method_reject(dispatch_state *ds, const atlas_ipc_request *req,
                                  atlas_err *err) {
    return spend_method(ds, req, ATLAS_DECISION_OP_REJECT, err);
}

static atlas_status method_supersede(dispatch_state *ds, const atlas_ipc_request *req,
                                     atlas_err *err) {
    return spend_method(ds, req, ATLAS_DECISION_OP_SUPERSEDE, err);
}

/* A6. Spends a revalidation capability.
 *
 * It is here, beside approve and reject, because it is the same kind of thing:
 * an operator action that a capability authorises, reachable over IPC and
 * useless without one. Like them, it is **not** an AI-facing method — there is
 * no MCP tool for it, no hook emits it, and a caller that has not been through
 * the terminal has no token to send. Like them, the whole of what makes it safe
 * is that `spend_challenge` refuses every request that does not carry a
 * capability Atlas issued, to this revision, for this intent, unspent and
 * unexpired.
 *
 * A6 adds two refusals on top of A4's, and both are in the write point rather
 * than here: the indexed head must be the one the capability was issued
 * against, and the evidence must still resolve to the digest it was issued
 * against. */
static atlas_status method_revalidate(dispatch_state *ds, const atlas_ipc_request *req,
                                      atlas_err *err) {
    return spend_method(ds, req, ATLAS_DECISION_OP_REVALIDATE, err);
}

/* --- reads ------------------------------------------------------------------------ */

typedef struct list_ctx {
    dispatch_state *ds;
    atlas_status st;
} list_ctx;

/* One document, as the compact listing shows it: identity, status, provenance
 * and bounded metadata. The title is the only prose, and it is encoded. */
static atlas_status write_doc(dispatch_state *ds, const atlas_decision_doc_row *row,
                              atlas_err *err) {
    atlas_status st = atlas_json_obj_begin(ds->j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "decision", row->uid, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "status", row->status, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "revision", row->head_revision_no, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "latest_revision", row->latest_revision_no, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "revision_state", row->head_state, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "content_hash", row->content_hash, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "proposed_by", row->proposed_by, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "links", row->link_count, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "created_at", row->created_at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "updated_at", row->updated_at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "superseded_by", row->superseded_by_uid, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "title",
                                atlas_safe(&ds->safe, row->title != NULL ? row->title : ""), err);
    }
    if (st == ATLAS_OK) {
        /* Repeated on every document rather than once per response.
         *
         * A caller that reads one element out of an array and drops the
         * envelope — which is what a model summarising a search result does —
         * must still carry the label with the prose it took. */
        st = atlas_json_key_str(ds->j, "trust", "UNTRUSTED_DATA", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(ds->j, err);
    }
    return st;
}

static atlas_status on_doc(const atlas_decision_doc_row *row, void *ud, atlas_err *err) {
    list_ctx *lc = (list_ctx *)ud;
    return write_doc(lc->ds, row, err);
}

static atlas_status doc_list(dispatch_state *ds, const atlas_ipc_request *req,
                             const char *key, atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_server_require_repo(ds, req, &info, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }
    int64_t limit = ATLAS_DECISION_DEFAULT_ROWS;
    (void)atlas_ipc_param_int(req, "limit", &limit);
    if (limit <= 0 || limit > ATLAS_DECISION_MAX_ROWS) {
        limit = ATLAS_DECISION_MAX_ROWS;
    }
    const char *status = NULL;
    if (atlas_ipc_param_str(req, "status", &status) && status != NULL && status[0] != '\0') {
        atlas_decision_state s;
        if (!atlas_decision_state_parse(status, &s)) {
            atlas_repo_info_free(&info);
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "\"status\" is PROPOSED, APPROVED, REJECTED or SUPERSEDED");
        }
    } else {
        status = NULL;
    }

    st = atlas_json_key_str(ds->j, "repo", info.name, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, key, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    int64_t count = 0;
    bool more = false;
    list_ctx lc = {ds, ATLAS_OK};
    if (st == ATLAS_OK) {
        const char *query = NULL;
        const char *path = NULL;
        if (atlas_ipc_param_str(req, "query", &query) && query != NULL && query[0] != '\0') {
            if (strlen(query) > ATLAS_DECISION_QUERY_MAX) {
                st = atlas_err_set(err, ATLAS_ERR_USAGE, "that query is too long");
            } else {
                st = atlas_db_decision_search(ds->db, info.id, query, limit, on_doc, &lc, &count,
                                              &more, err);
            }
        } else if (atlas_ipc_param_str(req, "path", &path) && path != NULL && path[0] != '\0') {
            atlas_buf raw = ATLAS_BUF_INIT;
            st = atlas_path_text_decode(path, strlen(path), &raw, err);
            if (st == ATLAS_OK) {
                st = atlas_db_decision_for_path(ds->db, info.id, raw.data, raw.len, limit, on_doc,
                                                &lc, &count, &more, err);
            }
            atlas_buf_free(&raw);
        } else {
            st = atlas_db_decision_documents_list(ds->db, info.id, status, limit, on_doc, &lc,
                                                  &count, &more, err);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "count", count, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "more", more, err);
    }
    if (st == ATLAS_OK) {
        int64_t proposed = 0, approved = 0, rejected = 0, superseded = 0;
        st = atlas_db_decision_repo_counts(ds->db, info.id, &proposed, &approved, &rejected,
                                           &superseded, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "total_proposed", proposed, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "total_approved", approved, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "total_rejected", rejected, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "total_superseded", superseded, err);
        }
    }
    atlas_repo_info_free(&info);
    return st;
}

static atlas_status method_list(dispatch_state *ds, const atlas_ipc_request *req, atlas_err *err) {
    return doc_list(ds, req, "decisions", err);
}

/* Resolves the `decision` parameter against this repository. */
static atlas_status require_document(dispatch_state *ds, const atlas_ipc_request *req,
                                     atlas_repo_info *info, int64_t *doc_id, atlas_err *err) {
    atlas_status st = atlas_server_require_repo(ds, req, info, err);
    if (st != ATLAS_OK) {
        return st;
    }
    atlas_buf uid = ATLAS_BUF_INIT;
    st = take_uid(req, "decision", true, &uid, err);
    int64_t repo_of = 0;
    bool found = false;
    if (st == ATLAS_OK) {
        st = atlas_db_decision_find_uid(ds->db, atlas_buf_cstr(&uid), doc_id, &repo_of, &found,
                                        err);
    }
    if (st == ATLAS_OK && !found) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "no decision has that id");
    }
    if (st == ATLAS_OK && repo_of != info->id) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE,
                           "that decision belongs to a different repository");
    }
    atlas_buf_free(&uid);
    return st;
}

/* Whether each index has ever completed a pass, which is what turns "not
 * indexed" into UNKNOWN rather than MISSING. */
static atlas_status index_known(dispatch_state *ds, const atlas_repo_info *info, bool *file_known,
                                bool *code_known, atlas_err *err) {
    int64_t repo_id = info->id;
    /* A completed `atlas scan` counts as well as a completed reconciliation
     * pass: only the latter sets `last_complete_generation`, and checking that
     * alone made every link on a scanned-but-undaemonised repository report
     * UNKNOWN forever. */
    *file_known = info->last_scan_id > 0;
    *code_known = false;
    atlas_index_state is;
    atlas_index_state_init(&is);
    atlas_status st = atlas_db_index_state_get(ds->db, repo_id, &is, err);
    if (st == ATLAS_OK && is.present && is.last_complete_generation > 0) {
        *file_known = true;
    }
    atlas_index_state_free(&is);
    if (st != ATLAS_OK) {
        return st;
    }
    atlas_code_index_state cs;
    atlas_code_index_state_init(&cs);
    st = atlas_db_code_state_get(ds->db, repo_id, &cs, err);
    if (st == ATLAS_OK) {
        *code_known = cs.present && cs.last_complete_generation > 0;
    }
    atlas_code_index_state_free(&cs);
    return st;
}

static atlas_status write_links(dispatch_state *ds, const atlas_repo_info *info,
                                atlas_decision_revision *rev, atlas_err *err) {
    int64_t repo_id = info->id;
    bool file_known = false, code_known = false;
    atlas_status st = index_known(ds, info, &file_known, &code_known, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "links", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    int64_t needs_review = 0;
    for (size_t i = 0; st == ATLAS_OK && i < rev->link_count; i++) {
        atlas_decision_link *l = &rev->links[i];
        st = atlas_db_decision_link_resolve(ds->db, repo_id, l, file_known, code_known, err);
        if (st != ATLAS_OK) {
            break;
        }
        if (l->currency == ATLAS_DECISION_LINK_CHANGED ||
            l->currency == ATLAS_DECISION_LINK_MISSING ||
            l->currency == ATLAS_DECISION_LINK_AMBIGUOUS) {
            needs_review++;
        }
        st = atlas_json_obj_begin(ds->j, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "kind", atlas_decision_link_kind_name(l->kind), err);
        }
        if (st == ATLAS_OK && l->path_text.len > 0) {
            /* Already in the safe encoding: `path_text` is stored encoded, so
             * encoding it again would double-encode. */
            st = atlas_json_key_str(ds->j, "path", atlas_buf_cstr(&l->path_text), err);
        }
        if (st == ATLAS_OK && l->commit_oid.len > 0) {
            st = atlas_json_key_str(ds->j, "commit", atlas_buf_cstr(&l->commit_oid), err);
        }
        if (st == ATLAS_OK && l->symbol_name_text.len > 0) {
            st = atlas_json_key_str(ds->j, "symbol", atlas_buf_cstr(&l->symbol_name_text), err);
        }
        if (st == ATLAS_OK && l->symbol_kind.len > 0) {
            st = atlas_json_key_str(ds->j, "symbol_kind", atlas_buf_cstr(&l->symbol_kind), err);
        }
        if (st == ATLAS_OK && l->target_uid.len > 0) {
            st = atlas_json_key_str(ds->j, "target", atlas_buf_cstr(&l->target_uid), err);
        }
        if (st == ATLAS_OK && l->change_set_id > 0) {
            st = atlas_json_key_int(ds->j, "change_set", l->change_set_id, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "currency",
                                    atlas_decision_link_currency_name(l->currency), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "matches", l->match_count, err);
        }
        if (st == ATLAS_OK && l->basis_commit.len > 0) {
            st = atlas_json_key_str(ds->j, "basis_commit", atlas_buf_cstr(&l->basis_commit), err);
        }
        if (st == ATLAS_OK && l->analyzer_name.len > 0) {
            /* A fixed Atlas vocabulary — the only writer of the column is Atlas
             * with a string literal — which is why it is not encoded, and why
             * it can be reported at all. */
            st = atlas_json_key_str(ds->j, "analyzer", atlas_buf_cstr(&l->analyzer_name), err);
            if (st == ATLAS_OK) {
                st = atlas_json_key_int(ds->j, "analyzer_version", l->analyzer_version, err);
            }
        }
        if (st == ATLAS_OK) {
            st = atlas_json_obj_end(ds->j, err);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "links_count", (int64_t)rev->link_count, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "links_needing_review", needs_review, err);
    }
    if (st == ATLAS_OK) {
        /* Explicit, because "no link needs review" and "Atlas has not looked"
         * are different facts and the second is common on a fresh index. */
        st = atlas_json_key_bool(ds->j, "file_index_known", file_known, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "code_index_known", code_known, err);
    }
    return st;
}

static atlas_status method_get(dispatch_state *ds, const atlas_ipc_request *req, atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    int64_t doc_id = 0;
    atlas_status st = require_document(ds, req, &info, &doc_id, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }

    /* Which revision: a named one, or the effective one. Naming a revision is
     * how a caller reads what was actually approved rather than what is newest. */
    int64_t want_no = 0;
    (void)atlas_ipc_param_int(req, "revision", &want_no);
    int64_t rev_id = 0;
    if (want_no > 0) {
        bool found = false;
        st = atlas_db_decision_revision_by_no(ds->db, doc_id, want_no, &rev_id, &found, err);
        if (st == ATLAS_OK && !found) {
            st = atlas_err_set(err, ATLAS_ERR_USAGE, "this decision has no revision %lld",
                               (long long)want_no);
        }
    } else {
        st = atlas_db_decision_current_revision(ds->db, doc_id, &rev_id, err);
        if (st == ATLAS_OK && rev_id == 0) {
            int64_t no = 0;
            char hash[ATLAS_SHA256_HEX_LEN + 1u];
            char state[16];
            st = atlas_db_decision_latest_revision(ds->db, doc_id, &rev_id, &no, hash,
                                                   sizeof(hash), state, sizeof(state), err);
        }
    }
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }

    atlas_decision_revision rev;
    atlas_decision_revision_init(&rev);
    bool found = false;
    st = atlas_db_decision_revision_load(ds->db, rev_id, &rev, &found, err);
    if (st == ATLAS_OK && !found) {
        st = atlas_err_set(err, ATLAS_ERR_INTEGRITY, "this decision has no revisions");
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "repo", info.name, err);
    }
    list_ctx lc = {ds, ATLAS_OK};
    if (st == ATLAS_OK) {
        bool seen = false;
        /* The document header comes from the same projection the listing uses,
         * so `decision show` and `decision list` cannot describe one document
         * differently. */
        st = atlas_json_key(ds->j, "document", err);
        if (st == ATLAS_OK) {
            st = atlas_db_decision_document_row(ds->db, doc_id, on_doc, &lc, &seen, err);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "revision", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_begin(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "number", rev.revision_no, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "state", atlas_decision_state_name(rev.state), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "content_hash", rev.content_hash, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "proposed_by", atlas_decision_actor_name(rev.proposed_by),
                                err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "created_at", rev.created_at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "scope", atlas_decision_scope_name(rev.scope), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(
            ds->j, "basis_head", rev.basis_head.len > 0 ? atlas_buf_cstr(&rev.basis_head) : NULL,
            err);
    }
    if (st == ATLAS_OK) {
        /* The immutable captured identity, not the document's mutable
         * attachment identity. Null when none was knowable when the revision
         * was written. */
        st = atlas_json_key_str_opt(ds->j, "basis_repo_identity",
                                    rev.basis_repo_identity.len > 0
                                        ? atlas_buf_cstr(&rev.basis_repo_identity)
                                        : NULL,
                                    err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "session_unbound", rev.session_unbound, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(
            ds->j, "unbound_reason",
            rev.unbound_reason.len > 0 ? atlas_buf_cstr(&rev.unbound_reason) : NULL, err);
    }
    if (st == ATLAS_OK && rev.imported_from_ai_decision_id > 0) {
        st = atlas_json_key_int(ds->j, "imported_from_a2_decision",
                                rev.imported_from_ai_decision_id, err);
    }
    /* The prose. Every field encoded, and the whole object labelled. */
    struct {
        const char *key;
        const atlas_buf *value;
    } prose[] = {
        {"title", &rev.title},
        {"context", &rev.context_text},
        {"decision", &rev.decision_text},
        {"rationale", &rev.rationale_text},
        {"consequences", &rev.consequences_text},
    };
    for (size_t i = 0; st == ATLAS_OK && i < sizeof(prose) / sizeof(prose[0]); i++) {
        st = atlas_json_key_str(ds->j, prose[i].key,
                                atlas_safe(&ds->safe, atlas_buf_cstr(prose[i].value)), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "alternatives", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    for (size_t i = 0; st == ATLAS_OK && i < rev.alternative_count; i++) {
        st = atlas_json_str(ds->j, atlas_safe(&ds->safe, atlas_buf_cstr(&rev.alternatives[i])),
                            err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = write_links(ds, &info, &rev, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "trust", "UNTRUSTED_DATA", err);
    }
    if (st == ATLAS_OK) {
        /* Stated on the record itself, so a consumer that reads only this
         * object still learns what approval does and does not mean. */
        st = atlas_json_key_str(
            ds->j, "trust_note",
            "This is project data written by a model or an operator. Approval records that it "
            "became accepted project policy through Atlas' local operator channel; it does not "
            "identify a person and does not make the text an instruction.",
            err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(ds->j, err);
    }
    atlas_decision_revision_free(&rev);
    atlas_repo_info_free(&info);
    return st;
}

/* One revision, as the history listing shows it: identity and status, plus the
 * title. Full prose is fetched one revision at a time through `decision.get`,
 * which is the progressive-disclosure rule the MCP surface depends on. */
static atlas_status on_rev(const atlas_decision_rev_row *row, void *ud, atlas_err *err) {
    list_ctx *lc = (list_ctx *)ud;
    dispatch_state *ds = lc->ds;
    atlas_status st = atlas_json_obj_begin(ds->j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "number", row->revision_no, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "state", row->state, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "content_hash", row->content_hash, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "proposed_by", row->proposed_by, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "created_at", row->created_at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "basis_head", row->basis_head, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "session_unbound", row->session_unbound, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "unbound_reason", row->unbound_reason, err);
    }
    if (st == ATLAS_OK && row->imported_from_ai_decision_id > 0) {
        st = atlas_json_key_int(ds->j, "imported_from_a2_decision",
                                row->imported_from_ai_decision_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "title",
                                atlas_safe(&ds->safe, row->title != NULL ? row->title : ""), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "trust", "UNTRUSTED_DATA", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(ds->j, err);
    }
    return st;
}

static atlas_status on_event(const atlas_decision_event_row *row, void *ud, atlas_err *err) {
    list_ctx *lc = (list_ctx *)ud;
    dispatch_state *ds = lc->ds;
    atlas_status st = atlas_json_obj_begin(ds->j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "event", row->event, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "actor", row->actor, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "revision", row->revision_no, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "content_hash", row->content_hash, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "operator_channel", row->challenge_id > 0, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "superseded_by", row->superseded_by_uid, err);
    }
    if (st == ATLAS_OK) {
        /* A fixed Atlas vocabulary written by lifecycle.c as string literals,
         * never assembled from caller bytes. */
        st = atlas_json_key_str_opt(ds->j, "detail", row->detail, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "at", row->created_at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(ds->j, err);
    }
    return st;
}

static atlas_status method_history(dispatch_state *ds, const atlas_ipc_request *req,
                                   atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    int64_t doc_id = 0;
    atlas_status st = require_document(ds, req, &info, &doc_id, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }
    int64_t limit = ATLAS_DECISION_MAX_EVENTS;
    (void)atlas_ipc_param_int(req, "limit", &limit);
    if (limit <= 0 || limit > ATLAS_DECISION_MAX_EVENTS) {
        limit = ATLAS_DECISION_MAX_EVENTS;
    }

    list_ctx lc = {ds, ATLAS_OK};
    st = atlas_json_key_str(ds->j, "repo", info.name, err);
    if (st == ATLAS_OK) {
        bool seen = false;
        st = atlas_json_key(ds->j, "document", err);
        if (st == ATLAS_OK) {
            st = atlas_db_decision_document_row(ds->db, doc_id, on_doc, &lc, &seen, err);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "revisions", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    int64_t rev_count = 0;
    bool rev_more = false;
    if (st == ATLAS_OK) {
        st = atlas_db_decision_revisions_list(ds->db, doc_id, limit, on_rev, &lc, &rev_count,
                                              &rev_more, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "timeline", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    int64_t count = 0;
    bool more = false;
    if (st == ATLAS_OK) {
        st = atlas_db_decision_events_list(ds->db, doc_id, limit, on_event, &lc, &count, &more,
                                           err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "count", count, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "more", more, err);
    }
    if (st == ATLAS_OK) {
        /* The ledger is canonical and the status columns are a cache of it, so
         * a timeline says whether the two agree. Reported, never repaired. */
        bool ok = true;
        atlas_buf detail = ATLAS_BUF_INIT;
        st = atlas_db_decision_verify(ds->db, doc_id, &ok, &detail, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_bool(ds->j, "ledger_agrees", ok, err);
        }
        if (st == ATLAS_OK && !ok) {
            st = atlas_json_key_str(ds->j, "ledger_detail",
                                    atlas_safe(&ds->safe, atlas_buf_cstr(&detail)), err);
        }
        atlas_buf_free(&detail);
    }
    atlas_repo_info_free(&info);
    return st;
}

/* --- the group ------------------------------------------------------------------- */


/* --- A6: reading a gate result ---------------------------------------------
 *
 * A **read**, and the only A6 method there is. It computes nothing that is
 * stored, changes nothing, and takes no lock; a caller that can reach it can
 * see what Atlas thinks and can do nothing about it.
 *
 * There is deliberately no method that clears, overrides, caches or recomputes
 * a freshness result, because there is no such operation anywhere in Atlas. The
 * one thing that changes what an assessment says next time is the code, or a
 * revalidation — and a revalidation needs a capability that only the terminal
 * channel can obtain.
 *
 * `decision.revalidate` sits beside `decision.approve` in this table and is
 * equally unreachable without that capability. Neither has an MCP tool, and no
 * hook emits either. */
static atlas_status method_gate_check(dispatch_state *ds, const atlas_ipc_request *req,
                                      atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_server_require_repo(ds, req, &info, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }
    const char *at = NULL;
    (void)atlas_ipc_param_str(req, "at", &at);
    const char *uid = NULL;
    (void)atlas_ipc_param_str(req, "decision", &uid);
    int64_t depth = 0;
    (void)atlas_ipc_param_int(req, "depth", &depth);

    atlas_gate_report rep;
    atlas_gate_report_init(&rep);
    if (uid != NULL && uid[0] != '\0') {
        st = atlas_gate_run_one(ds->db, info.name, uid, at, &rep, err);
    } else {
        atlas_gate_query q;
        atlas_gate_query_init(&q);
        q.repo_name = info.name;
        q.at_commit = at;
        q.depth = depth;
        st = atlas_gate_run(ds->db, &q, &rep, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "repo", info.name, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "result", atlas_gate_result_name(rep.result), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "indexed_commit", rep.indexed_commit, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "requested_commit", rep.requested_commit, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "fresh", rep.fresh, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "stale", rep.stale, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "impacted", rep.impacted, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "unknown", rep.unknown, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "limit_reached", rep.limit_reached, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "decisions", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    for (size_t i = 0; st == ATLAS_OK && i < rep.item_count; i++) {
        const atlas_gate_assessment *a = &rep.items[i];
        st = atlas_json_obj_begin(ds->j, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "decision", atlas_buf_cstr(&a->uid), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "revision", a->revision_no, err);
        }
        if (st == ATLAS_OK) {
            /* Project prose. Encoded on the way out like every untrusted value,
             * and labelled where it reaches a model by the MCP layer. */
            st = atlas_json_key_str(ds->j, "title",
                                    atlas_safe(&ds->safe, atlas_buf_cstr(&a->title)), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "freshness",
                                    atlas_gate_freshness_name(a->freshness), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "content_hash", a->content_hash, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "validated_at_commit", a->validated_at_commit, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "revalidations", a->revalidation_count, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key(ds->j, "reasons", err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_arr_begin(ds->j, err);
        }
        for (size_t k = 0; st == ATLAS_OK && k < a->reason_count; k++) {
            st = atlas_json_str(ds->j, atlas_gate_reason_name(a->reasons[k]), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_arr_end(ds->j, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_bool(ds->j, "limit_reached", a->limit_reached, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_obj_end(ds->j, err);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    atlas_gate_report_free(&rep);
    atlas_repo_info_free(&info);
    return st;
}

static const atlas_method_entry DECISION_METHODS[] = {
    /* Reads. */
    {"decision.list", method_list},
    {"decision.get", method_get},
    {"decision.history", method_history},
    /* Writes a model may reach. */
    {"decision.propose", method_propose},
    {"decision.revise", method_revise},
    {"decision.promote", method_promote},
    /* The operator channel. These are reachable over the socket — the CLI is a
     * socket client like everything else — but the MCP tool surface exposes no
     * tool that calls them, and no MCP tool accepts a `token` or a
     * `confirmation` at all. See `atlas_mcp_tool_names()` and
     * `tests/test_decision_mcp.c`. */
    {"decision.challenge", method_challenge},
    {"decision.approve", method_approve},
    {"decision.reject", method_reject},
    {"decision.supersede", method_supersede},
    {"decision.revalidate", method_revalidate},
    /* A6, and a read. Nothing here can change an assessment. */
    {"gate.check", method_gate_check},
};

const atlas_method_entry *atlas_server_decision_methods(size_t *count_out) {
    if (count_out != NULL) {
        *count_out = sizeof(DECISION_METHODS) / sizeof(DECISION_METHODS[0]);
    }
    return DECISION_METHODS;
}
