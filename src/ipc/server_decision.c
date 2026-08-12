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

/* The decision id of the document a write is about.
 *
 * **A8.2: `decision_uid` is the field, and `decision` is only a fallback.**
 *
 * The two meanings used to share one name. A propose put its prose in
 * `decision`; a revise put the document id there and its prose in
 * `decision_body`, which the server never read — so every revise stored the uid
 * as the decision text and reported success. Two meanings on one key is the
 * defect, so the fix is two keys, and the fallback exists only so a client
 * older than this daemon keeps working rather than silently mis-writing. */
static atlas_status take_doc_uid(const atlas_ipc_request *req, bool required, atlas_buf *out,
                                 atlas_err *err) {
    const char *v = NULL;
    if (atlas_ipc_param_str(req, "decision_uid", &v) && v != NULL && v[0] != '\0') {
        if (!atlas_decision_uid_is_valid(v)) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "\"decision_uid\" is not a decision id; they look like %s "
                                 "followed by %u lowercase hex characters",
                                 ATLAS_DECISION_UID_PREFIX, (unsigned)ATLAS_DECISION_UID_HEX);
        }
        return atlas_buf_set_str(out, v, err);
    }
    return take_uid(req, "decision", required, out, err);
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

/* A9.1. The knowledge kind on a propose, and the assertion checked on a revise.
 *
 * `knowledge_kind_given` records whether the request said anything at all, which
 * is a distinction the server has to keep: a client that has never heard of kinds
 * omits the key and must be able to revise a POLICY, while a client that sends
 * `"kind": "DECISION"` for a POLICY document is asserting something false and is
 * refused. Defaulting the absent case to an assertion would break every existing
 * client's revise. */
static atlas_status take_kind(const atlas_ipc_request *req, atlas_decision_op *op,
                              atlas_err *err) {
    const char *v = NULL;
    op->knowledge_kind = ATLAS_DECISION_KIND_DECISION;
    op->knowledge_kind_given = false;
    if (!atlas_ipc_param_str(req, "kind", &v) || v == NULL || v[0] == '\0') {
        return ATLAS_OK;
    }
    if (!atlas_decision_kind_parse(v, &op->knowledge_kind)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "\"kind\" is one of %s",
                             atlas_decision_kind_list());
    }
    op->knowledge_kind_given = true;
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
        /* No file was named, so resolve one: the symbol's definition site, from
         * this index, and only when there is exactly one of them.
         *
         * This used to return here instead, leaving the link with no file
         * context at all unless the request carried `symbol_files`. The CLI
         * never sends that — the local path resolves the site itself — so every
         * symbol link recorded over the socket lost the file and content hash
         * the local path captured, and the same decision written the two ways
         * produced two different content hashes. Resolving it here is also the
         * right layering: the index is the daemon's, and a caller-supplied file
         * would be a caller asserting what the snapshot exists to check.
         *
         * More than one definition site binds nothing, deliberately. A name
         * defined in several files identifies no single one, and picking the
         * first would record a file the decision may not be about; the link's
         * currency then reads AMBIGUOUS, which is the truth. */
        atlas_buf path_raw = ATLAS_BUF_INIT;
        atlas_buf hash = ATLAS_BUF_INIT;
        int64_t matches = 0;
        st = atlas_db_code_symbol_definition_site(ds->db, repo_id, l->symbol_name.data,
                                                  l->symbol_name.len, &path_raw, &hash, &matches,
                                                  err);
        if (st == ATLAS_OK && matches == 1 && path_raw.len > 0) {
            st = atlas_buf_set(&l->path_raw, path_raw.data, path_raw.len, err);
            if (st == ATLAS_OK) {
                st = atlas_path_text_encode(path_raw.data, path_raw.len, &l->path_text, err);
            }
            if (st == ATLAS_OK && hash.len > 0) {
                st = atlas_buf_set(&l->file_content_hash, hash.data, hash.len, err);
            }
        }
        atlas_buf_free(&path_raw);
        atlas_buf_free(&hash);
        return st;
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

/* Decision-to-decision references, as target uids.
 *
 * Carried as uids and nothing else. Existence and same-repository are checked at
 * the write point, inside the transaction that records the revision, so a
 * dangling or cross-repository target aborts before any row is written rather
 * than being repaired afterwards — see `lifecycle.c`, which resolves
 * `relates_to` through the same path as `supersedes`.
 *
 * Two checks belong here instead, because this is where the whole requested set
 * is visible at once:
 *
 *   - **self**, when the source document is known. A revise names its document,
 *     so a relation to itself is refusable; a propose has no uid yet, and the
 *     client refuses that case for the same reason.
 *   - **duplicate**, within one request. Recording the same relation twice says
 *     nothing the first says and doubles it in every walk of the graph. */
static atlas_status take_decision_links(const atlas_ipc_request *req, const char *source_uid,
                                        atlas_decision_op *op, atlas_err *err) {
    const atlas_ipc_array *arr = NULL;
    if (!atlas_ipc_param_array(req, "decisions", &arr)) {
        return ATLAS_OK;
    }
    size_t n = atlas_ipc_array_len(arr);
    if (n > ATLAS_DECISION_MAX_LINKS) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a decision may carry at most %d links, and %zu decision links "
                             "were given",
                             ATLAS_DECISION_MAX_LINKS, n);
    }
    for (size_t i = 0; i < n; i++) {
        const char *s = NULL;
        if (!atlas_ipc_array_str(arr, i, &s) || s == NULL || s[0] == '\0') {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "\"decisions\" must be non-empty decision ids");
        }
        if (source_uid != NULL && source_uid[0] != '\0' && strcmp(source_uid, s) == 0) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "a decision cannot relate to itself (%s)",
                                 s);
        }
        for (size_t k = 0; k < op->revision.link_count; k++) {
            const atlas_decision_link *prev = &op->revision.links[k];
            if (prev->kind == ATLAS_DECISION_LINK_RELATES_TO &&
                prev->target_uid.len == strlen(s) &&
                memcmp(prev->target_uid.data, s, prev->target_uid.len) == 0) {
                return atlas_err_set(err, ATLAS_ERR_USAGE,
                                     "the same decision link was given twice (%s)", s);
            }
        }
        atlas_decision_link l;
        atlas_decision_link_init(&l, ATLAS_DECISION_LINK_RELATES_TO);
        atlas_status st = atlas_buf_set_str(&l.target_uid, s, err);
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
    /* A9.1: echoed on every write, so a client that proposed without naming a
     * kind is told what it created rather than having to assume. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "kind", atlas_decision_kind_name(r->knowledge_kind), err);
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
        st = take_kind(req, op, err);
    }
    if (st == ATLAS_OK) {
        st = take_text(req, "title", ATLAS_DECISION_TITLE_MAX, false, &op->revision.title, err);
    }
    if (st == ATLAS_OK) {
        st = take_text(req, "context", ATLAS_DECISION_TEXT_MAX, true, &op->revision.context_text,
                       err);
    }
    if (st == ATLAS_OK) {
        /* **A8.2: the prose comes from `decision_body`.**
         *
         * It used to come from `decision`, which on a revise is the document
         * id — so every revise, and every `link add` (which is a revise with
         * the same links re-sent), stored the uid as the decision text and
         * returned success. `decision` is still read for a propose from a
         * client older than this daemon, where it is unambiguously prose
         * because no document exists yet to name.
         *
         * The guard below is the one that would have caught this: prose that
         * equals the document's own id is never a decision, whatever key it
         * arrived under. */
        const char *body = NULL;
        if (atlas_ipc_param_str(req, "decision_body", &body) && body != NULL) {
            st = take_text(req, "decision_body", ATLAS_DECISION_TEXT_MAX, true,
                           &op->revision.decision_text, err);
        } else if (op->uid.len == 0) {
            st = take_text(req, "decision", ATLAS_DECISION_TEXT_MAX, true,
                           &op->revision.decision_text, err);
        }
        if (st == ATLAS_OK && op->uid.len > 0 &&
            op->revision.decision_text.len == op->uid.len &&
            memcmp(op->revision.decision_text.data, op->uid.data, op->uid.len) == 0) {
            st = atlas_err_set(err, ATLAS_ERR_USAGE,
                               "the decision text is this decision's own id; a document id is "
                               "not a decision. Send the prose in \"decision_body\".");
        }
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
    if (st == ATLAS_OK) {
        /* `op->uid` is set by `take_identity` above: present on a revise, empty
         * on a propose, which is exactly the difference that decides whether a
         * self-relation can be detected at all. */
        st = take_decision_links(req, atlas_buf_cstr(&op->uid), op, err);
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

/* Defined below, beside the link methods that are its other caller. */
static atlas_status take_edge_fields(const atlas_ipc_request *req, atlas_decision_op *op,
                                     const atlas_buf *target, const char *event, atlas_err *err);

static atlas_status method_revise(dispatch_state *ds, const atlas_ipc_request *req,
                                  atlas_err *err) {
    atlas_decision_op *op = op_new(ATLAS_DECISION_OP_REVISE);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    atlas_status st = take_doc_uid(req, true, &op->uid, err);
    if (st == ATLAS_OK) {
        st = build_revision_op(ds, req, op, err);
    }
    /* A revise may carry the account of an edge it draws or drops.
     *
     * This is not a second surface: it is the *same* operation arriving by the
     * other route. A client that holds a context but not the writer lock routes
     * its whole typed op through `decision.revise` rather than through
     * `decision.link_add`, and a reason that reached one path and not the other
     * would make the two ways of recording the same relation disagree. */
    if (st == ATLAS_OK) {
        st = take_edge_fields(req, op, NULL, ATLAS_DECISION_EDGE_EVENT_ADDED, err);
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

/* --- the operator channel is not served here ---------------------------------
 *
 * A7 removed `method_challenge` and the `spend_method` family — approve,
 * reject, supersede and revalidate — from this file, rather than leaving them
 * in place behind a refusal.
 *
 * Removed, because the two are not equivalent. A method that refuses is a
 * method whose refusal can be weakened by a later edit, mis-ordered against
 * another check, or reached through a second dispatch path; a method that does
 * not exist is answered by the dispatcher's unknown-method case, which is the
 * same code that answers every name nobody implemented. A5 made this argument
 * about backups and A6 repeated it about gate mutations. A7 applies it to the
 * one group where it was most needed and least obviously absent.
 *
 * The operations still exist. They run in `src/core/service_decision.c` on the
 * local path, under the data-directory write lock, so the daemon must be
 * stopped for one to happen at all — and that is a fact the kernel enforces.
 */

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
    /* A9.1. Its own key beside `status`, always present, from a closed Atlas
     * vocabulary. Never folded into `status`: an approved invariant and an
     * approved accepted risk share a status and differ here, and a client that
     * had to parse one field for both would be guessing. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "kind", row->kind != NULL ? row->kind : "DECISION", err);
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
                                 "\"status\" is PROPOSED, APPROVED, REJECTED, SUPERSEDED or "
                                 "RESOLVED");
        }
    } else {
        status = NULL;
    }
    /* A9.1. Validated against the vocabulary here rather than passed through: an
     * unrecognised kind must be a refusal, because a filter that matches nothing
     * and a filter that was misspelt look identical in an empty result. */
    const char *kind = NULL;
    if (atlas_ipc_param_str(req, "kind", &kind) && kind != NULL && kind[0] != '\0') {
        atlas_decision_kind k;
        if (!atlas_decision_kind_parse(kind, &k)) {
            atlas_repo_info_free(&info);
            return atlas_err_set(err, ATLAS_ERR_USAGE, "\"kind\" is one of %s",
                                 atlas_decision_kind_list());
        }
    } else {
        kind = NULL;
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
                st = atlas_db_decision_search(ds->db, info.id, query, kind, limit, on_doc, &lc,
                                              &count, &more, err);
            }
        } else if (atlas_ipc_param_str(req, "path", &path) && path != NULL && path[0] != '\0') {
            atlas_buf raw = ATLAS_BUF_INIT;
            st = atlas_path_text_decode(path, strlen(path), &raw, err);
            if (st == ATLAS_OK) {
                st = atlas_db_decision_for_path(ds->db, info.id, raw.data, raw.len, kind, limit,
                                                on_doc, &lc, &count, &more, err);
            }
            atlas_buf_free(&raw);
        } else {
            st = atlas_db_decision_documents_list(ds->db, info.id, status, kind, limit, on_doc, &lc,
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
        int64_t proposed = 0, approved = 0, rejected = 0, superseded = 0, resolved = 0;
        st = atlas_db_decision_repo_counts(ds->db, info.id, &proposed, &approved, &rejected,
                                           &superseded, &resolved, err);
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
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "total_resolved", resolved, err);
        }
        /* A9.1: one object keyed by the kind names, rather than eight
         * `total_*` keys. The status totals keep their flat shape because
         * clients already read them by name; a nested object for the new axis
         * means a kind added later changes no key a client depends on. */
        if (st == ATLAS_OK) {
            int64_t by_kind[ATLAS_DECISION_KIND_MAX];
            st = atlas_db_decision_kind_counts(ds->db, info.id, by_kind,
                                               sizeof(by_kind) / sizeof(by_kind[0]), err);
            if (st == ATLAS_OK) {
                st = atlas_json_key(ds->j, "total_by_kind", err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_obj_begin(ds->j, err);
            }
            for (size_t i = 0; st == ATLAS_OK && i < atlas_decision_kind_count(); i++) {
                atlas_decision_kind k = atlas_decision_kind_at(i);
                st = atlas_json_key_int(ds->j, atlas_decision_kind_name(k), by_kind[(size_t)k], err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_obj_end(ds->j, err);
            }
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
    st = take_doc_uid(req, true, &uid, err);
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
                                int64_t document_id, atlas_decision_revision *rev,
                                atlas_err *err) {
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
        /* Migration 10: the durable reason a relation exists. Looked up per
         * edge because it lives outside the immutable revision that carries the
         * link — see the migration comment. */
        if (st == ATLAS_OK && l->kind == ATLAS_DECISION_LINK_RELATES_TO && document_id > 0 &&
            l->target_uid.len > 0) {
            int64_t tid = 0, trepo = 0;
            bool tfound = false;
            if (atlas_db_decision_find_uid(ds->db, atlas_buf_cstr(&l->target_uid), &tid, &trepo,
                                           &tfound, err) == ATLAS_OK &&
                tfound) {
                atlas_buf note = ATLAS_BUF_INIT;
                atlas_buf prov = ATLAS_BUF_INIT;
                bool have = false;
                if (atlas_db_decision_edge_rationale(ds->db, document_id, tid,
                                                     atlas_decision_link_kind_name(l->kind), &note,
                                                     &prov, &have, err) == ATLAS_OK &&
                    have) {
                    /* Untrusted prose, encoded on the way out like every other
                     * stored text. Stored raw, encoded here, encoded once. */
                    st = atlas_json_key_str(ds->j, "rationale",
                                            atlas_safe(&ds->safe, atlas_buf_cstr(&note)), err);
                    if (st == ATLAS_OK) {
                        st = atlas_json_key_str(ds->j, "rationale_provenance",
                                                atlas_buf_cstr(&prov), err);
                    }
                }
                atlas_buf_free(&note);
                atlas_buf_free(&prov);
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
        st = write_links(ds, &info, doc_id, &rev, err);
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
        /* Additive, and the whole of what `atlas gate show NAME ID` needs: the
         * query already carries `only_uid`, so one decision costs one
         * assessment rather than a repository's worth. Absent, every approved
         * decision is assessed exactly as before. */
        const char *only = NULL;
        if (atlas_ipc_param_str(req, "decision", &only) && only != NULL && only[0] != '\0') {
            q.only_uid = only;
        }
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
    /* Additive, so `atlas gate check` can be answered over the socket by a
     * client that cannot open the index. Everything here was already in the
     * report and simply had no reader on the wire. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "out_of_scope", rep.out_of_scope, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "depth", rep.depth, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "root", atlas_buf_cstr(&rep.root_text), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "limit_detail", rep.limit_detail, err);
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
            st = atlas_json_key_str_opt(ds->j, "limit_detail", a->limit_detail, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "evidence_digest", a->evidence_digest, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_bool(ds->j, "validated_by_revalidation",
                                     a->validated_by_revalidation, err);
        }
        {
            const struct {
                const char *k;
                int64_t v;
            } counters[] = {
                {"links_total", a->links_total},
                {"links_current", a->links_current},
                {"links_changed", a->links_changed},
                {"links_missing", a->links_missing},
                {"links_ambiguous", a->links_ambiguous},
                {"links_unknown", a->links_unknown},
                {"range_commits", a->range_commits},
                {"range_paths", a->range_paths},
                {"walk_visited", a->walk_visited},
                {"walk_matched", a->walk_matched},
            };
            for (size_t c = 0; st == ATLAS_OK && c < sizeof counters / sizeof counters[0]; c++) {
                st = atlas_json_key_int(ds->j, counters[c].k, counters[c].v, err);
            }
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

/* decision.link_add — relate one decision to another, changing only the links.
 *
 * **A8.2: performed here, on the daemon, and not by the client.**
 *
 * It used to be a client-side read-modify-write: read the document, re-send
 * every field, add one relation. That was wrong twice over. The prose came back
 * from a remote read already in Atlas' safe-text encoding and was re-sent as if
 * it were raw, so each link add encoded it again — `%0A` became `%250A` — and
 * the body drifted a little further from what it said every time a relationship
 * was recorded. And because it went through revise, it inherited the defect
 * that stored the uid as the prose.
 *
 * Loading the revision here removes the round trip entirely: the content never
 * leaves the database, so there is nothing to re-encode and nothing to lose.
 * The client sends two ids and the name of a repository.
 *
 * Links are carried across, not rebuilt from rendered values: the whole
 * revision is loaded, the relation appended, and the result written as one new
 * immutable revision. */
/* Emits one edge-event row. `active` is computed from the current revision's
 * links rather than stored: the revision is canonical for what is live. */
typedef struct edge_emit_state {
    dispatch_state *ds;
    const atlas_decision_revision *current;
} edge_emit_state;

static atlas_status emit_edge_row(const atlas_decision_edge_event_row *row, void *ud,
                                  atlas_err *err) {
    edge_emit_state *es = (edge_emit_state *)ud;
    dispatch_state *ds = es->ds;
    bool active = false;
    if (es->current != NULL && row->target_uid != NULL) {
        for (size_t i = 0; i < es->current->link_count; i++) {
            const atlas_decision_link *l = &es->current->links[i];
            if (l->kind == ATLAS_DECISION_LINK_RELATES_TO &&
                strcmp(atlas_buf_cstr(&l->target_uid), row->target_uid) == 0) {
                active = true;
                break;
            }
        }
    }
    atlas_status st = atlas_json_obj_begin(ds->j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "id", row->id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "target", row->target_uid, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "kind", row->kind, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "event", row->event, err);
    }
    if (st == ATLAS_OK) {
        /* Prose, encoded here on the way out. */
        st = atlas_json_key_str(ds->j, "note",
                                atlas_safe(&ds->safe, row->note != NULL ? row->note : ""), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "provenance", row->provenance, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "created_at", row->created_at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "revision_id", row->revision_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "active", active, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(ds->j, err);
    }
    return st;
}

/* Migration 10: the account of an edge, read off a request.
 *
 * `edge_target` is a document id and `edge_note` is prose, under separate keys
 * and never interchangeable — the A8.2 rule. The values are only carried here;
 * every one of them is checked against its vocabulary at the write point, which
 * is where the guarantee has to be, because a request is not the authority on
 * what a provenance is. */
static atlas_status take_edge_fields(const atlas_ipc_request *req, atlas_decision_op *op,
                                     const atlas_buf *target, const char *event, atlas_err *err) {
    const char *note = NULL;
    if (!atlas_ipc_param_str(req, "edge_note", &note) || note == NULL || *note == '\0') {
        return ATLAS_OK; /* no account supplied */
    }
    /* `target == NULL` means the edge is named by the request rather than by
     * the caller. That is the routed-op path: a client that holds a context but
     * not the writer lock sends the whole operation as `decision.revise`, so
     * the edge it concerns arrives as a parameter like everything else. Both
     * paths land here, which is what keeps their validation identical. */
    atlas_status st;
    if (target != NULL) {
        st = atlas_buf_set(&op->edge_target_uid, target->data, target->len, err);
    } else {
        const char *t = NULL;
        if (!atlas_ipc_param_str(req, "edge_target", &t) || t == NULL || *t == '\0') {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "\"edge_note\" was given without \"edge_target\"; a reason "
                                 "explains one relation and must name it");
        }
        st = atlas_buf_set_str(&op->edge_target_uid, t, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op->edge_note, note, err);
    }
    /* The request may name the event; an unrecognised one is refused at the
     * write point against the closed vocabulary. */
    if (st == ATLAS_OK) {
        const char *e = NULL;
        if (atlas_ipc_param_str(req, "edge_event", &e) && e != NULL && *e != '\0') {
            event = e;
        }
        st = atlas_buf_set_str(&op->edge_event, event, err);
    }
    if (st == ATLAS_OK) {
        const char *prov = NULL;
        if (atlas_ipc_param_str(req, "edge_provenance", &prov) && prov != NULL && *prov != '\0') {
            st = atlas_buf_set_str(&op->edge_provenance, prov, err);
        }
    }
    return st;
}

static atlas_status method_link_add(dispatch_state *ds, const atlas_ipc_request *req,
                                    atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    int64_t doc_id = 0;
    atlas_status st = require_document(ds, req, &info, &doc_id, err);
    atlas_buf target = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = take_uid(req, "target", true, &target, err);
    }
    atlas_buf self = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = take_doc_uid(req, true, &self, err);
    }
    if (st == ATLAS_OK && strcmp(atlas_buf_cstr(&self), atlas_buf_cstr(&target)) == 0) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "a decision cannot relate to itself (%s)",
                           atlas_buf_cstr(&self));
    }
    /* The target must exist here. The write point checks this too; checking it
     * first is what makes the message name the problem rather than the row. */
    if (st == ATLAS_OK) {
        int64_t tid = 0, trepo = 0;
        bool found = false;
        st = atlas_db_decision_find_uid(ds->db, atlas_buf_cstr(&target), &tid, &trepo, &found, err);
        if (st == ATLAS_OK && !found) {
            st = atlas_err_set(err, ATLAS_ERR_USAGE,
                               "a decision link names a document Atlas does not hold");
        }
        if (st == ATLAS_OK && trepo != info.id) {
            st = atlas_err_set(err, ATLAS_ERR_USAGE,
                               "a decision may only link to another decision in the same "
                               "repository");
        }
    }

    int64_t rev_id = 0;
    if (st == ATLAS_OK) {
        st = atlas_db_decision_current_revision(ds->db, doc_id, &rev_id, err);
        if (st == ATLAS_OK && rev_id == 0) {
            int64_t no = 0;
            char hash[ATLAS_SHA256_HEX_LEN + 1u];
            char state[16];
            st = atlas_db_decision_latest_revision(ds->db, doc_id, &rev_id, &no, hash, sizeof(hash),
                                                   state, sizeof(state), err);
        }
        if (st == ATLAS_OK && rev_id == 0) {
            st = atlas_err_set(err, ATLAS_ERR_USAGE, "this decision has no revision to extend");
        }
    }

    atlas_decision_op *op = NULL;
    if (st == ATLAS_OK) {
        op = op_new(ATLAS_DECISION_OP_REVISE);
        if (op == NULL) {
            st = atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
        }
    }
    if (st == ATLAS_OK) {
        bool found = false;
        st = atlas_db_decision_revision_load(ds->db, rev_id, &op->revision, &found, err);
        if (st == ATLAS_OK && !found) {
            st = atlas_err_set(err, ATLAS_ERR_INTERNAL, "the current revision could not be read");
        }
    }
    /* Already related: report the revision that holds it and write nothing. */
    bool duplicate = false;
    if (st == ATLAS_OK) {
        for (size_t i = 0; i < op->revision.link_count; i++) {
            const atlas_decision_link *l = &op->revision.links[i];
            if (l->kind == ATLAS_DECISION_LINK_RELATES_TO && l->target_uid.len == target.len &&
                memcmp(l->target_uid.data, target.data, target.len) == 0) {
                duplicate = true;
                break;
            }
        }
    }
    if (st == ATLAS_OK && !duplicate) {
        atlas_decision_link l;
        atlas_decision_link_init(&l, ATLAS_DECISION_LINK_RELATES_TO);
        st = atlas_buf_set(&l.target_uid, target.data, target.len, err);
        if (st == ATLAS_OK) {
            st = atlas_decision_revision_add_link(&op->revision, &l, err);
        }
        atlas_decision_link_free(&l);
    }
    /* The reason the relation exists, if the caller gave one.
     *
     * On a new edge it is `ADDED` and rides the revision that introduces it. On
     * an edge that is already there it is `ANNOTATED`, and the operation stops
     * being a revise altogether: explaining an existing relation must not
     * produce a revision, because a rationale written now was not part of what
     * was approved then, and a new revision would move a content hash to record
     * something the approval never covered. */
    if (st == ATLAS_OK) {
        st = take_edge_fields(req, op, &target,
                              duplicate ? ATLAS_DECISION_EDGE_EVENT_ANNOTATED
                                        : ATLAS_DECISION_EDGE_EVENT_ADDED,
                              err);
    }
    bool annotate_only = false;
    if (st == ATLAS_OK && duplicate && op->edge_note.len > 0) {
        annotate_only = true;
        op->kind = ATLAS_DECISION_OP_EDGE_NOTE;
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&op->uid, self.data, self.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op->repo_name, info.name, err);
    }
    if (st == ATLAS_OK) {
        op->revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
        st = atlas_decision_revision_validate(&op->revision, err);
    }

    if (st != ATLAS_OK) {
        if (op != NULL) {
            atlas_decision_op_free(op);
            free(op);
        }
        atlas_buf_free(&self);
        atlas_buf_free(&target);
        atlas_repo_info_free(&info);
        return st;
    }
    if (duplicate && !annotate_only) {
        /* Nothing to write. Reported as the outcome the caller expects, with
         * the revision that already carries the relation. */
        int64_t no = 0;
        char hash[ATLAS_SHA256_HEX_LEN + 1u];
        char state[16];
        int64_t tmp = 0;
        (void)atlas_db_decision_latest_revision(ds->db, doc_id, &tmp, &no, hash, sizeof(hash),
                                                state, sizeof(state), err);
        st = atlas_json_key_str(ds->j, "repo", info.name, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "decision", atlas_buf_cstr(&self), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "revision", no, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "content_hash", hash, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_bool(ds->j, "created", false, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_bool(ds->j, "duplicate", true, err);
        }
        atlas_decision_op_free(op);
        free(op);
        atlas_buf_free(&self);
        atlas_buf_free(&target);
        atlas_repo_info_free(&info);
        return st;
    }

    atlas_buf_free(&self);
    atlas_buf_free(&target);
    atlas_repo_info_free(&info);
    atlas_decision_result result;
    atlas_decision_result_init(&result);
    st = submit(ds, op, &result, err);
    if (st == ATLAS_OK) {
        st = write_result(ds, &result, err);
    }
    atlas_decision_result_free(&result);
    return st;
}

/* The account of one document's relations: every event, oldest first.
 *
 * A read. It takes no capability, writes nothing and creates no process, and it
 * is in the ordinary method group rather than the operator one because
 * explaining why two decisions are related is not an authority. */
static atlas_status method_links(dispatch_state *ds, const atlas_ipc_request *req,
                                 atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    int64_t doc_id = 0;
    atlas_status st = require_document(ds, req, &info, &doc_id, err);

    /* The current revision, so `active` says what the document asserts now
     * rather than what the ledger last said about it. */
    atlas_decision_revision current;
    atlas_decision_revision_init(&current);
    bool have_current = false;
    if (st == ATLAS_OK) {
        int64_t rev_id = 0;
        if (atlas_db_decision_current_revision(ds->db, doc_id, &rev_id, err) == ATLAS_OK &&
            rev_id == 0) {
            int64_t no = 0;
            char hash[ATLAS_SHA256_HEX_LEN + 1u];
            char state[16];
            (void)atlas_db_decision_latest_revision(ds->db, doc_id, &rev_id, &no, hash,
                                                    sizeof(hash), state, sizeof(state), err);
        }
        if (rev_id > 0) {
            (void)atlas_db_decision_revision_load(ds->db, rev_id, &current, &have_current, err);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "repo", info.name, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "edges", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    edge_emit_state es;
    memset(&es, 0, sizeof(es));
    es.ds = ds;
    es.current = have_current ? &current : NULL;
    int64_t n = 0;
    bool more = false;
    if (st == ATLAS_OK) {
        st = atlas_db_decision_edge_events_list(ds->db, doc_id, ATLAS_DECISION_EDGE_EVENTS_MAX,
                                                emit_edge_row, &es, &n, &more, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "count", n, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "more", more, err);
    }
    atlas_decision_revision_free(&current);
    atlas_repo_info_free(&info);
    return st;
}

/* Attach an explanation to a relation that already exists.
 *
 * Writes one append-only row and nothing else. It is in the ordinary group, not
 * the operator one: explaining why two decisions are related asserts nothing
 * about either and changes no lifecycle state. */
static atlas_status method_edge_note(dispatch_state *ds, const atlas_ipc_request *req,
                                     atlas_err *err) {
    atlas_decision_op *op = op_new(ATLAS_DECISION_OP_EDGE_NOTE);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    atlas_status st = take_where(req, op, err);
    if (st == ATLAS_OK) {
        st = take_doc_uid(req, true, &op->uid, err);
    }
    /* The far end of the edge arrives under `target`, the same key
     * `decision.link_add` and `decision.link_remove` use — this is the same
     * relation named the same way, and a third spelling for it would be a third
     * chance to disagree. `edge_target` is still accepted, because a routed op
     * serialises every field under its own name. */
    atlas_buf target = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = take_uid(req, "target", false, &target, err);
    }
    if (st == ATLAS_OK) {
        st = take_edge_fields(req, op, target.len > 0 ? &target : NULL,
                              ATLAS_DECISION_EDGE_EVENT_ANNOTATED, err);
    }
    if (st == ATLAS_OK && op->edge_target_uid.len == 0) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "an edge note must name the relation it explains");
    }
    atlas_buf_free(&target);
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
    atlas_decision_result_free(&result);
    return st;
}

/* Withdraw a relation between two decisions.
 *
 * The exact mirror of `method_link_add`, and it deletes nothing. A revision is
 * immutable, so this loads the current one, drops the link from a working copy
 * and writes the result as a new PROPOSED revision. The revision that carried
 * the relation keeps it verbatim, along with its creation event and whatever
 * rationale was recorded for it, so the edge remains fully explicable after it
 * has stopped being live. What the new revision changes is only which relations
 * the *current* revision asserts.
 *
 * Withdrawing an edge that is not there is reported, not invented: `removed`
 * is false and no revision is written, which makes a repeated removal a no-op
 * rather than a stream of empty revisions.
 *
 * It is a proposal, not an operator action. It mints no capability, moves no
 * status and cannot reach a terminal decision state — the same standing
 * `link add` has. */
static atlas_status method_link_remove(dispatch_state *ds, const atlas_ipc_request *req,
                                       atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    int64_t doc_id = 0;
    atlas_status st = require_document(ds, req, &info, &doc_id, err);
    atlas_buf target = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = take_uid(req, "target", true, &target, err);
    }
    atlas_buf self = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = take_doc_uid(req, true, &self, err);
    }
    if (st == ATLAS_OK && strcmp(atlas_buf_cstr(&self), atlas_buf_cstr(&target)) == 0) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "a decision cannot relate to itself (%s)",
                           atlas_buf_cstr(&self));
    }
    /* The target must exist and must belong to this repository, checked for the
     * reason `link add` checks it: so the message names the problem, and so a
     * caller cannot use removal to probe another repository's document ids. */
    if (st == ATLAS_OK) {
        int64_t tid = 0, trepo = 0;
        bool found = false;
        st = atlas_db_decision_find_uid(ds->db, atlas_buf_cstr(&target), &tid, &trepo, &found, err);
        if (st == ATLAS_OK && !found) {
            st = atlas_err_set(err, ATLAS_ERR_USAGE,
                               "a decision link names a document Atlas does not hold");
        }
        if (st == ATLAS_OK && trepo != info.id) {
            st = atlas_err_set(err, ATLAS_ERR_USAGE,
                               "a decision may only link to another decision in the same "
                               "repository");
        }
    }

    int64_t rev_id = 0;
    if (st == ATLAS_OK) {
        st = atlas_db_decision_current_revision(ds->db, doc_id, &rev_id, err);
        if (st == ATLAS_OK && rev_id == 0) {
            int64_t no = 0;
            char hash[ATLAS_SHA256_HEX_LEN + 1u];
            char state[16];
            st = atlas_db_decision_latest_revision(ds->db, doc_id, &rev_id, &no, hash, sizeof(hash),
                                                   state, sizeof(state), err);
        }
        if (st == ATLAS_OK && rev_id == 0) {
            st = atlas_err_set(err, ATLAS_ERR_USAGE, "this decision has no revision to extend");
        }
    }

    atlas_decision_op *op = NULL;
    if (st == ATLAS_OK) {
        op = op_new(ATLAS_DECISION_OP_REVISE);
        if (op == NULL) {
            st = atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
        }
    }
    if (st == ATLAS_OK) {
        bool found = false;
        st = atlas_db_decision_revision_load(ds->db, rev_id, &op->revision, &found, err);
        if (st == ATLAS_OK && !found) {
            st = atlas_err_set(err, ATLAS_ERR_INTERNAL, "the current revision could not be read");
        }
    }
    bool removed = false;
    if (st == ATLAS_OK) {
        removed = atlas_decision_revision_remove_link(&op->revision, ATLAS_DECISION_LINK_RELATES_TO,
                                                      atlas_buf_cstr(&target));
    }
    if (st == ATLAS_OK && removed) {
        st = take_edge_fields(req, op, &target, ATLAS_DECISION_EDGE_EVENT_REMOVED, err);
        /* **A withdrawal without a reason is refused here, not only in the
         * CLI.** A removal is the last thing that happens to an edge: a reason
         * not recorded now is not recorded at all, and the edge would be gone
         * from the current revision with nothing saying why. The CLI checks
         * this too, for a better message — but a check a client runs on itself
         * is not a boundary, and this is the write path every client shares. */
        if (st == ATLAS_OK && op->edge_note.len == 0) {
            st = atlas_err_set(err, ATLAS_ERR_USAGE,
                               "withdrawing a relation needs \"edge_note\": the reason is the "
                               "only thing that will still explain it afterwards");
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&op->uid, self.data, self.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op->repo_name, info.name, err);
    }
    if (st == ATLAS_OK) {
        op->revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
        st = atlas_decision_revision_validate(&op->revision, err);
    }
    if (st != ATLAS_OK) {
        if (op != NULL) {
            atlas_decision_op_free(op);
            free(op);
        }
        atlas_buf_free(&self);
        atlas_buf_free(&target);
        atlas_repo_info_free(&info);
        return st;
    }
    if (!removed) {
        /* Nothing was related, so nothing is withdrawn and nothing is written.
         * Reported as an outcome rather than an error: a caller retrying a
         * removal it already completed asked for a state that now holds. */
        int64_t no = 0;
        char hash[ATLAS_SHA256_HEX_LEN + 1u];
        char state[16];
        int64_t tmp = 0;
        (void)atlas_db_decision_latest_revision(ds->db, doc_id, &tmp, &no, hash, sizeof(hash),
                                                state, sizeof(state), err);
        st = atlas_json_key_str(ds->j, "repo", info.name, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "decision", atlas_buf_cstr(&self), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "revision", no, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "content_hash", hash, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_bool(ds->j, "created", false, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_bool(ds->j, "removed", false, err);
        }
        atlas_decision_op_free(op);
        free(op);
        atlas_buf_free(&self);
        atlas_buf_free(&target);
        atlas_repo_info_free(&info);
        return st;
    }

    atlas_buf_free(&self);
    atlas_buf_free(&target);
    atlas_repo_info_free(&info);
    atlas_decision_result result;
    atlas_decision_result_init(&result);
    st = submit(ds, op, &result, err);
    if (st == ATLAS_OK) {
        st = write_result(ds, &result, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "removed", true, err);
    }
    atlas_decision_result_free(&result);
    return st;
}

/* **A7: the operator channel is not an RPC method group.**
 *
 * Until A7 these five sat in the table below, defended by the argument that no
 * MCP tool named them. That defends against a model which can only speak MCP
 * and against nothing else: `decision.challenge` took no capability — it *was*
 * the capability source — and it asked for no terminal, because the terminal
 * check lived in `atlas_service_decision_confirm`, which is the CLI's own
 * helper. A check a client runs on itself is not a boundary. Any process that
 * could open the socket could mint a token and spend it, and the record then
 * said `LOCAL_OPERATOR_CONFIRMED` about a channel nothing had been through.
 *
 * They are now local-only operations that take the writer lock, which is
 * exactly what A5 does with backup, restore and prune and for exactly that
 * reason: "the daemon must be stopped" is then a fact the kernel enforces
 * rather than a sentence in a manual. `apply_op` in `src/core/service_decision.c`
 * refuses to route them over the socket and says so.
 *
 * `tests/test_a7_authority.c` asks a live daemon for each of these names and
 * requires every one to fail. Adding one back deletes the guarantee. */
/* --- decision.orphaned / decision.legacy -------------------------------------
 *
 * Both reads, and both here for the reason `atlas decision orphaned` exists at
 * all: a canonical record that has become invisible looks exactly like one that
 * was deleted, and a client that cannot open the index could not see either.
 * `orphaned` takes no repository — that is the point of it. */
static atlas_status method_orphaned(dispatch_state *ds, const atlas_ipc_request *req,
                                    atlas_err *err) {
    int64_t limit = ATLAS_DECISION_DEFAULT_ROWS;
    (void)atlas_ipc_param_int(req, "limit", &limit);
    if (limit <= 0 || limit > ATLAS_DECISION_MAX_ROWS) {
        limit = ATLAS_DECISION_MAX_ROWS;
    }
    atlas_status st = atlas_json_key(ds->j, "decisions", err);
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    int64_t count = 0;
    bool more = false;
    list_ctx lc = {ds, ATLAS_OK};
    if (st == ATLAS_OK) {
        st = atlas_db_decision_orphans_list(ds->db, limit, on_doc, &lc, &count, &more, err);
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
    return st;
}

static atlas_status on_legacy(const atlas_decision_legacy_row *v, void *ud, atlas_err *err) {
    list_ctx *lc = (list_ctx *)ud;
    dispatch_state *ds = lc->ds;
    atlas_status st = atlas_json_obj_begin(ds->j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "id", v->id, err);
    }
    /* Project prose, encoded on the way out and labelled with every element for
     * the reason `write_doc` gives: a caller that reads one element and drops
     * the envelope must still carry the label with the prose it took. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(
            ds->j, "title", v->title != NULL ? atlas_safe(&ds->safe, v->title) : NULL, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(
            ds->j, "statement", v->statement != NULL ? atlas_safe(&ds->safe, v->statement) : NULL,
            err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "provenance", v->provenance, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "created_at", v->created_at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "imported_uid", v->imported_uid, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "paths", v->path_count, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "imported", v->imported, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "trust", "UNTRUSTED_DATA", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(ds->j, err);
    }
    return st;
}

static atlas_status method_legacy(dispatch_state *ds, const atlas_ipc_request *req,
                                  atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_server_require_repo(ds, req, &info, err);
    int64_t limit = ATLAS_DECISION_DEFAULT_ROWS;
    (void)atlas_ipc_param_int(req, "limit", &limit);
    if (limit <= 0 || limit > ATLAS_DECISION_MAX_ROWS) {
        limit = ATLAS_DECISION_MAX_ROWS;
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "repo", info.name, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "legacy", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    int64_t count = 0;
    list_ctx lc = {ds, ATLAS_OK};
    if (st == ATLAS_OK) {
        bool lmore = false;
        st = atlas_db_decision_legacy_list(ds->db, info.id, false, limit, on_legacy, &lc, &count,
                                           &lmore, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "count", count, err);
    }
    atlas_repo_info_free(&info);
    return st;
}

/* --- the operator channel --------------------------------------------------------
 *
 * **These five were deleted by A7, and are restored here in a disjoint group
 * that only the operator peer can reach.** The A7 defect was not that they
 * existed: it was that `decision.challenge` took no capability and asked for no
 * terminal — it *was* the capability source — so any process able to open the
 * socket could mint a token, spend it, and produce a record whose stored actor
 * was `LOCAL_OPERATOR_CONFIRMED` about a channel nothing had been through. The
 * terminal check lived in the CLI's own helper, and a check a client performs
 * on itself is not a boundary.
 *
 * What makes this different is where the identity comes from. The group is
 * offered only to a peer whose `SO_PEERCRED` uid equals the `operator_uid` in
 * the root-owned policy, checked by the same probe the local path uses —
 * root-anchored path, root ownership, root-owned executable, and the uid.
 * Every other peer gets `unknown method`, which is what a name that does not
 * exist gets. A uid written into a request never reaches this decision.
 *
 * **What this does not do, stated plainly:** it does not distinguish a person
 * from a program running as the operator's account. Nothing can. An AI agent
 * with a shell as that uid reaches these methods exactly as a human does; the
 * terminal prompt is a UX confirmation that the right revision is being acted
 * on, and it is not a security boundary. The rule that a model must not approve
 * anything is an orchestration rule, not something the kernel enforces here.
 * `LOCAL_OPERATOR_CONFIRMED` continues to identify the channel and not a
 * person, and every honesty limit A4 states about it still holds word for word.
 *
 * Why restore them at all: the alternative was to leave the lifecycle reachable
 * only by the account that owns the index, and that account is the daemon's.
 * The deployment's human operator is a different uid and must not be given the
 * index — so the choice was between an operator who cannot approve and an
 * operator identified by the kernel over the socket. This is the second.
 */
static atlas_status method_challenge(dispatch_state *ds, const atlas_ipc_request *req,
                                     atlas_err *err) {
    atlas_decision_op *op = op_new(ATLAS_DECISION_OP_CHALLENGE);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    atlas_status st = take_where(req, op, err);
    if (st == ATLAS_OK) {
        st = take_doc_uid(req, true, &op->uid, err);
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
        st = take_doc_uid(req, true, &op->uid, err);
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
 * It is here, beside approve and reject, because it is the same kind of thing:
 * an operator action that a capability authorises, reachable over IPC and
 * useless without one. Like them, it is **not** an AI-facing method — there is
 * no MCP tool for it, no hook emits it, and a caller that has not been through
 * the terminal has no token to send. Like them, the whole of what makes it safe
 * is that `spend_challenge` refuses every request that does not carry a
 * capability Atlas issued, to this revision, for this intent, unspent and
 * unexpired.
 * A6 adds two refusals on top of A4's, and both are in the write point rather
 * than here: the indexed head must be the one the capability was issued
 * against, and the evidence must still resolve to the digest it was issued
 * against. */
static atlas_status method_revalidate(dispatch_state *ds, const atlas_ipc_request *req,
                                      atlas_err *err) {
    return spend_method(ds, req, ATLAS_DECISION_OP_REVALIDATE, err);
}

/* A9.1. Spends a resolution capability.
 *
 * In the operator group beside approve, reject and supersede, because it is the
 * same kind of thing: a lifecycle transition that a single-use capability
 * authorises. It is *not* in the ordinary group and there is no MCP tool for it,
 * so a model holding every Atlas tool cannot close out an obligation — which is
 * the same boundary A4 drew around approval and for the same reason. Closing an
 * obligation is a claim that work was done. */
static atlas_status method_resolve(dispatch_state *ds, const atlas_ipc_request *req,
                                   atlas_err *err) {
    return spend_method(ds, req, ATLAS_DECISION_OP_RESOLVE, err);
}

/* The operator group. Disjoint from `DECISION_METHODS`, and reachable only from
 * the peer the root-owned policy names — A8's two-group pattern, for A8's
 * reason: a name in a group a peer is not in answers `unknown method`, the same
 * as a name that does not exist, because a refusal that distinguished "you may
 * not" from "there is no such thing" would tell a caller what to try next. */
static const atlas_method_entry OPERATOR_METHODS[] = {
    {"decision.challenge", method_challenge},
    {"decision.approve", method_approve},
    {"decision.reject", method_reject},
    {"decision.supersede", method_supersede},
    {"decision.revalidate", method_revalidate},
    {"decision.resolve", method_resolve},
};

const atlas_method_entry *atlas_server_operator_methods(size_t *count_out) {
    if (count_out != NULL) {
        *count_out = sizeof(OPERATOR_METHODS) / sizeof(OPERATOR_METHODS[0]);
    }
    return OPERATOR_METHODS;
}

/* Whether this peer is the operator the root-owned policy names.
 *
 * `SO_PEERCRED` and nothing else. The probe is the one the local path runs, so
 * a policy that is missing, symlinked, group-writable or not root-owned, or an
 * executable a non-root uid could replace, locks this group exactly as it locks
 * the local channel. */
bool atlas_server_peer_is_operator(long long peer_uid) {
    atlas_authority a;
    atlas_authority_probe_peer(peer_uid, &a);
    return a.state == ATLAS_AUTHORITY_GRANTED;
}

static const atlas_method_entry DECISION_METHODS[] = {
    {"decision.orphaned", method_orphaned},
    {"decision.legacy", method_legacy},
    /* Reads. */
    {"decision.list", method_list},
    {"decision.get", method_get},
    {"decision.history", method_history},
    /* Writes a model may reach. */
    {"decision.propose", method_propose},
    {"decision.revise", method_revise},
    /* A8.2: link add is a daemon-side operation now, so the content it must
     * preserve never travels and cannot be re-encoded on the way back. */
    {"decision.link_add", method_link_add},
    /* Migration 10. A proposal like `link_add`, not an operator verb: it writes
     * a proposed revision that asserts one relation fewer, mints no capability
     * and changes no status. */
    {"decision.link_remove", method_link_remove},
    /* A read: the account of one document's relations. */
    {"decision.links", method_links},
    /* The routed form of an annotation: a client holding a context but not the
     * writer lock sends its typed op here. It writes one append-only row — no
     * revision, no status change, no capability. */
    {"decision.edge.note", method_edge_note},
    {"decision.promote", method_promote},
    /* The operator channel is deliberately absent — see A7 below. */
    /* A6, and a read. Nothing here can change an assessment. */
    {"gate.check", method_gate_check},
};

const atlas_method_entry *atlas_server_decision_methods(size_t *count_out) {
    if (count_out != NULL) {
        *count_out = sizeof(DECISION_METHODS) / sizeof(DECISION_METHODS[0]);
    }
    return DECISION_METHODS;
}
