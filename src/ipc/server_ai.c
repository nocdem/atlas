/* Atlas - the A2 IPC method group: repositories by path, AI sessions, records.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * These are the methods the MCP adapter and the hook adapter call. Both are
 * separate processes; neither opens the database, and neither has a writable
 * handle. Everything they do arrives here.
 *
 * Two shapes, and the split is the concurrency model:
 *
 *   - a **read** runs on this thread against the per-request read-only handle,
 *     and is bounded by a query rather than by a deadline;
 *   - a **write** is validated here, turned into one typed atlas_ai_op, and
 *     handed to the writer thread with a deadline. Nothing on this thread ever
 *     holds a writable handle, because there is exactly one and the writer owns
 *     it.
 *
 * Validation happens *before* anything is queued. A request with a session key
 * that is too long, a provenance an A2 adapter may not write, or a path that is
 * not repository-relative is refused here, so the writer never sees one.
 */
#define _GNU_SOURCE 1

#include <stdlib.h>
#include <string.h>

#include "atlas/ai.h"
#include "atlas/atlas.h"
#include "atlas/pathrep.h"
#include "ipc/server_internal.h"

/* How long an AI write may occupy the writer before the caller is told it did
 * not finish. Deliberately short: an adapter calling this is inside a hook or a
 * tool call that a person is waiting on, and a truthful timeout beats a stall. */
#define AI_WRITE_TIMEOUT_MS 4000
/* Registering a repository runs a handful of `git rev-parse` calls, so it gets
 * the same bound `repo.add` gets rather than a shorter one.
 *
 * A shorter server-side deadline would not make a session start faster: the
 * caller has its own, much tighter one and fails open when it expires. What a
 * short deadline here would do is abandon a registration that was about to
 * succeed, so the *next* session would have to start it again — and on a slow or
 * loaded machine, possibly never finish. The client gives up early; the daemon
 * finishes the work. */
#define AI_REGISTER_TIMEOUT_MS 30000

/* --- parameter validation -------------------------------------------------
 *
 * Every string that crosses this boundary is bounded and safe-encoded before it
 * is stored. Safe encoding is not a defence against prompt injection and is not
 * claimed to be one; it is what keeps a stored value from carrying a control
 * byte into a terminal or an invalid UTF-8 sequence into a JSON document.
 *
 * The defence against injection is elsewhere: model-authored text never enters
 * automatic context, and when it is returned by an explicit tool call it is
 * labelled with its provenance. See docs/ai-trust-boundary.md. */

static atlas_status take_text(const atlas_ipc_request *req, const char *key, size_t max,
                              atlas_buf *out, atlas_err *err) {
    const char *raw = NULL;
    if (!atlas_ipc_param_str(req, key, &raw)) {
        return ATLAS_OK; /* absent is not an error; the caller decides */
    }
    size_t n = strlen(raw);
    if (n > max) {
        /* Refused rather than truncated. A truncated session key would correlate
         * two different sessions, and a truncated reason would misquote a model. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "\"%s\" is %zu bytes, above the %zu byte limit; it was refused rather "
                             "than truncated",
                             key, n, max);
    }
    atlas_buf_reset(out);
    return atlas_text_encode_safe(raw, n, out, err);
}

/* An identifier Atlas will use as a name: [A-Za-z0-9._-] only.
 *
 * Not safe-encoded but *restricted*, which is stronger. A provider or client
 * name reaches log lines and the context envelope, so limiting the alphabet is
 * cheaper to reason about than escaping an unrestricted one. */
static atlas_status take_ident(const atlas_ipc_request *req, const char *key, size_t max,
                               atlas_buf *out, atlas_err *err) {
    const char *raw = NULL;
    if (!atlas_ipc_param_str(req, key, &raw)) {
        return ATLAS_OK;
    }
    size_t n = strlen(raw);
    if (n == 0 || n > max) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "\"%s\" must be 1..%zu bytes", key, max);
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)raw[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-';
        if (!ok) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "\"%s\" may only contain letters, digits, '.', '_' and '-'", key);
        }
    }
    return atlas_buf_set(out, raw, n, err);
}

/* An absolute filesystem path an adapter observed.
 *
 * Kept as raw bytes: a path is bytes everywhere else in Atlas and making an
 * exception here would be the one place a non-UTF-8 directory stopped
 * resolving. Only two properties are required — absolute, and no NUL — because
 * anything else about it is the filesystem's business. */
static atlas_status take_path(const atlas_ipc_request *req, const char *key, atlas_buf *out,
                              atlas_err *err) {
    const char *raw = NULL;
    if (!atlas_ipc_param_str(req, key, &raw)) {
        return ATLAS_OK;
    }
    size_t n = strlen(raw);
    if (n == 0) {
        return ATLAS_OK;
    }
    if (raw[0] != '/') {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "\"%s\" must be an absolute path; Atlas does not resolve a relative "
                             "one against its own working directory",
                             key);
    }
    if (n > 4096u) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "\"%s\" is longer than 4096 bytes", key);
    }
    return atlas_buf_set(out, raw, n, err);
}

/* Collects the `paths` array into the NUL-separated raw form the rest of Atlas
 * uses.
 *
 * Each entry must be repository-relative and must survive
 * `atlas_path_check_relative`, which refuses absolute paths, "." and ".."
 * components and embedded NULs. That is what stops a tool call from naming a
 * path outside the repository it claims to be talking about. Entries arrive
 * percent-encoded or plain, and both are accepted because Atlas prints the
 * encoded form and a caller pasting it back must work. */
static atlas_status take_paths(dispatch_state *ds, const atlas_ipc_request *req,
                               const atlas_buf *root_hint, const atlas_buf *repo_hint,
                               atlas_buf *out, int64_t max, atlas_err *err) {
    const atlas_ipc_array *arr = NULL;
    if (!atlas_ipc_param_array(req, "paths", &arr)) {
        return ATLAS_OK;
    }
    size_t n = atlas_ipc_array_len(arr);
    if ((int64_t)n > max) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "\"paths\" holds %zu entries, above the limit of %lld", n,
                             (long long)max);
    }
    if (n == 0) {
        return ATLAS_OK;
    }

    /* An adapter observes absolute paths — a hook is handed one by Claude, and a
     * model naming a file has no reason to know where Atlas thinks the root is.
     * The index has the authoritative root bytes, so the conversion happens here
     * rather than in the adapter, once, against the row rather than against a
     * guess. */
    atlas_repo_info repo;
    atlas_repo_info_init(&repo);
    bool have_repo = false;
    atlas_status st = ATLAS_OK;
    if (repo_hint != NULL && repo_hint->len > 0) {
        st = atlas_db_repo_get(ds->db, atlas_buf_cstr(repo_hint), &repo, &have_repo, err);
    } else if (root_hint != NULL && root_hint->len > 0) {
        st = atlas_db_repo_get_containing(ds->db, root_hint->data, root_hint->len, &repo,
                                          &have_repo, err);
    }
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&repo);
        return st;
    }

    for (size_t i = 0; i < n && st == ATLAS_OK; i++) {
        const char *entry = NULL;
        if (!atlas_ipc_array_str(arr, i, &entry)) {
            st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"paths\" entry %zu is not a string", i);
            break;
        }
        atlas_buf decoded = ATLAS_BUF_INIT;
        /* Accepted in either form. Atlas prints the percent-encoded form, so a
         * caller pasting one back has to work; a plain path decodes to itself. */
        st = atlas_path_text_decode(entry, strlen(entry), &decoded, err);
        if (st != ATLAS_OK) {
            atlas_buf_free(&decoded);
            break;
        }

        const char *rel = decoded.data;
        size_t rel_len = decoded.len;
        if (rel_len > 0 && rel[0] == '/') {
            const atlas_buf *root = have_repo ? &repo.root_path : NULL;
            if (root == NULL || rel_len <= root->len || memcmp(rel, root->data, root->len) != 0 ||
                rel[root->len] != '/') {
                /* Outside the repository, or no repository resolved. Dropped
                 * rather than refused: an agent editing a file outside the
                 * repository is ordinary, and it is simply not something Atlas
                 * has anything to say about. Refusing the whole request would
                 * lose the paths that *are* inside it. */
                atlas_buf_free(&decoded);
                continue;
            }
            rel += root->len + 1u;
            rel_len -= root->len + 1u;
        }
        st = atlas_path_check_relative(rel, rel_len, err);
        if (st == ATLAS_OK) {
            st = atlas_buf_append(out, rel, rel_len, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_append_ch(out, '\0', err);
        }
        atlas_buf_free(&decoded);
    }
    atlas_repo_info_free(&repo);
    return st;
}

/* Fills the identity every AI request carries. */
static atlas_status take_identity(const atlas_ipc_request *req, atlas_ai_op *op, atlas_err *err) {
    atlas_status st = take_ident(req, "provider", ATLAS_AI_CLIENT_NAME_MAX, &op->provider, err);
    if (st == ATLAS_OK) {
        st = take_ident(req, "client", ATLAS_AI_CLIENT_NAME_MAX, &op->client, err);
    }
    if (st == ATLAS_OK) {
        st = take_text(req, "client_version", 64u, &op->client_version, err);
    }
    if (st == ATLAS_OK) {
        st = take_text(req, "session_key", ATLAS_AI_SESSION_KEY_MAX, &op->session_key, err);
    }
    if (st == ATLAS_OK) {
        st = take_text(req, "parent_session_key", ATLAS_AI_SESSION_KEY_MAX, &op->parent_session_key,
                       err);
    }
    if (st == ATLAS_OK) {
        st = take_text(req, "agent_id", ATLAS_AI_AGENT_ID_MAX, &op->agent_id, err);
    }
    if (st == ATLAS_OK) {
        st = take_text(req, "agent_type", ATLAS_AI_AGENT_ID_MAX, &op->agent_type, err);
    }
    if (st == ATLAS_OK) {
        st = take_path(req, "root", &op->root, err);
    }
    if (st == ATLAS_OK) {
        st = take_ident(req, "repo", ATLAS_NAME_MAX, &op->repo_name, err);
    }
    if (st == ATLAS_OK) {
        st = take_text(req, "dedup_key", 128u, &op->dedup_key, err);
    }
    if (st == ATLAS_OK) {
        (void)atlas_ipc_param_int(req, "turn_seq", &op->turn_seq);
    }
    return st;
}

/* Allocates an operation on the heap, because the writer takes ownership. */
static atlas_ai_op *op_new(atlas_ai_op_kind kind) {
    atlas_ai_op *op = calloc(1u, sizeof(*op));
    if (op != NULL) {
        atlas_ai_op_init(op, kind);
    }
    return op;
}

/* Builds, validates and submits one operation. Takes ownership of `op` on every
 * path, including the failure ones. */
static atlas_status submit(dispatch_state *ds, atlas_ai_op *op, int timeout_ms,
                           atlas_ai_result *result, atlas_err *err) {
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory building an AI request");
    }
    /* A reason or a decision may arrive without one: an MCP client that is not
     * Claude Code has no external session id to send, and refusing the record
     * would lose it entirely. It is stored sessionless and says so. Every other
     * method is session bookkeeping and is meaningless without the session it is
     * about, so the key is required rather than guessed at. */
    if (op->session_key.len == 0 && op->kind != ATLAS_AI_OP_REASON &&
        op->kind != ATLAS_AI_OP_DECISION) {
        atlas_ai_op_free(op);
        free(op);
        return atlas_err_set(err, ATLAS_ERR_USAGE, "this method needs a \"session_key\"");
    }
    return atlas_writer_ai(ds->ctx->writer, op, timeout_ms, result, err);
}

/* Writes the fields every AI mutation reports back. */
static atlas_status write_ai_result(dispatch_state *ds, const atlas_ai_result *r, atlas_err *err) {
    atlas_status st = atlas_json_key_int(ds->j, "session", r->session_id, err);
    if (st == ATLAS_OK) {
        /* Reported, never inferred from `session == 0`. A caller must be able to
         * tell "this record is attached to nothing" apart from "this operation
         * does not attach to a session", and the two look identical otherwise. */
        st = atlas_json_key_bool(ds->j, "session_unbound", r->session_unbound, err);
    }
    if (st == ATLAS_OK) {
        /* One of the fixed ATLAS_AI_UNBOUND_* constants, or "" — Atlas-authored
         * in every case, so it is emitted as-is. */
        st = atlas_json_key_str(ds->j, "unbound_reason",
                                r->unbound_reason != NULL ? r->unbound_reason : "", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "repo_id", r->repo_id, err);
    }
    if (st == ATLAS_OK) {
        /* Validated to [A-Za-z0-9._-] at registration, so not re-encoded. */
        st = atlas_json_key_str(ds->j, "repo", atlas_buf_cstr(&r->repo_name), err);
    }
    if (st == ATLAS_OK) {
        /* Already in the safe encoding; encoding it again would stop it
         * decoding back to the original bytes. */
        st = atlas_json_key_str(ds->j, "root", atlas_buf_cstr(&r->root_text), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "change_set", r->change_set_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "record", r->record_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "created", r->session_created, err);
    }
    if (st == ATLAS_OK) {
        /* The actual state, not a flag nothing sets.
         *
         * This used to report `repo_registered`, which only ever meant "this
         * operation performed the registration" and which nothing on the AI path
         * ever set — so it was always false even for a repository Atlas had
         * indexed for weeks. What a caller wants to know is whether the
         * repository is in the index at all, which is exactly `repo_id > 0`. */
        st = atlas_json_key_bool(ds->j, "registered", r->repo_id > 0, err);
    }
    if (st == ATLAS_OK) {
        /* Kept separately, because "it is registered" and "I registered it just
         * now" are different facts and the second one is occasionally useful. */
        st = atlas_json_key_bool(ds->j, "registered_now", r->repo_registered, err);
    }
    if (st == ATLAS_OK) {
        /* A duplicate is reported rather than hidden: a caller retrying a hook
         * needs to know its retry was recognised as one. */
        st = atlas_json_key_bool(ds->j, "duplicate", r->duplicate, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "changed_paths", r->changed_paths, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "direct_paths", r->direct_paths, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "ambiguous_paths", r->ambiguous_paths, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "unresolved_paths", r->unresolved_paths, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "concurrent_sessions", r->concurrent_sessions, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "sync_seq", r->sync_seq, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "degraded", r->degraded, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "degraded_reason",
                                atlas_safe(&ds->safe, atlas_buf_cstr(&r->degraded_reason)), err);
    }
    return st;
}

/* --- repository resolution ------------------------------------------------ */

/* Resolves a filesystem path to a registered repository, without git and
 * without writing anything.
 *
 * Pure index lookup on purpose. Running `git rev-parse` here would put a child
 * process inside the serve loop, where one slow invocation would stall every
 * other client for the git timeout. Registration, which does need git, goes to
 * the writer. */
static atlas_status method_repo_resolve(dispatch_state *ds, const atlas_ipc_request *req,
                                        atlas_err *err) {
    const char *raw = NULL;
    if (!atlas_ipc_param_str(req, "path", &raw)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "repo.resolve needs a \"path\" parameter");
    }
    if (raw[0] != '/') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "\"path\" must be absolute");
    }

    atlas_repo_info info;
    atlas_repo_info_init(&info);
    bool found = false;
    atlas_status st = atlas_db_repo_get_containing(ds->db, raw, strlen(raw), &info, &found, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "registered", found, err);
    }
    if (st == ATLAS_OK && found) {
        st = atlas_server_write_repo_state(ds, &info, err);
    }
    atlas_repo_info_free(&info);
    return st;
}

/* --- session lifecycle ---------------------------------------------------- */

static atlas_status simple_op(dispatch_state *ds, const atlas_ipc_request *req,
                              atlas_ai_op_kind kind, atlas_err *err) {
    atlas_ai_op *op = op_new(kind);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    atlas_status st = take_identity(req, op, err);
    if (st == ATLAS_OK) {
        st = take_text(req, "source", 64u, &op->source, err);
    }
    if (st != ATLAS_OK) {
        atlas_ai_op_free(op);
        free(op);
        return st;
    }
    atlas_ai_result result;
    atlas_ai_result_init(&result);
    st = submit(ds, op, AI_WRITE_TIMEOUT_MS, &result, err);
    if (st == ATLAS_OK) {
        st = write_ai_result(ds, &result, err);
    }
    atlas_ai_result_free(&result);
    return st;
}

static atlas_status method_session_open(dispatch_state *ds, const atlas_ipc_request *req,
                                        atlas_err *err) {
    return simple_op(ds, req, ATLAS_AI_OP_SESSION_OPEN, err);
}

static atlas_status method_session_touch(dispatch_state *ds, const atlas_ipc_request *req,
                                         atlas_err *err) {
    return simple_op(ds, req, ATLAS_AI_OP_SESSION_TOUCH, err);
}

static atlas_status method_session_close(dispatch_state *ds, const atlas_ipc_request *req,
                                         atlas_err *err) {
    return simple_op(ds, req, ATLAS_AI_OP_SESSION_CLOSE, err);
}

static atlas_status method_session_attach(dispatch_state *ds, const atlas_ipc_request *req,
                                          atlas_err *err) {
    return simple_op(ds, req, ATLAS_AI_OP_ATTACH_ROOT, err);
}

static atlas_status method_turn_close(dispatch_state *ds, const atlas_ipc_request *req,
                                      atlas_err *err) {
    return simple_op(ds, req, ATLAS_AI_OP_TURN_CLOSE, err);
}

static atlas_status method_session_checkpoint(dispatch_state *ds, const atlas_ipc_request *req,
                                              atlas_err *err) {
    atlas_ai_op *op = op_new(ATLAS_AI_OP_SESSION_CHECKPOINT);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    atlas_status st = take_identity(req, op, err);
    const char *phase = NULL;
    if (st == ATLAS_OK && atlas_ipc_param_str(req, "phase", &phase)) {
        if (strcmp(phase, "pre_compact") == 0) {
            op->phase = ATLAS_AI_PHASE_PRE_COMPACT;
        } else if (strcmp(phase, "post_compact") == 0) {
            op->phase = ATLAS_AI_PHASE_POST_COMPACT;
        } else {
            st = atlas_err_set(err, ATLAS_ERR_USAGE,
                               "\"phase\" must be \"pre_compact\" or \"post_compact\"");
        }
    }
    if (st != ATLAS_OK) {
        atlas_ai_op_free(op);
        free(op);
        return st;
    }
    atlas_ai_result result;
    atlas_ai_result_init(&result);
    st = submit(ds, op, AI_WRITE_TIMEOUT_MS, &result, err);
    if (st == ATLAS_OK) {
        st = write_ai_result(ds, &result, err);
    }
    atlas_ai_result_free(&result);
    return st;
}

static atlas_status method_tool_record(dispatch_state *ds, const atlas_ipc_request *req,
                                       atlas_err *err) {
    atlas_ai_op *op = op_new(ATLAS_AI_OP_TOOL_RECORD);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    atlas_status st = take_identity(req, op, err);
    if (st == ATLAS_OK) {
        st = take_ident(req, "tool", 64u, &op->tool_name, err);
    }
    if (st == ATLAS_OK) {
        st = take_text(req, "tool_use_id", 128u, &op->tool_use_id, err);
    }
    if (st == ATLAS_OK) {
        st = take_paths(ds, req, &op->root, &op->repo_name, &op->paths, 1, err);
    }
    const char *phase = NULL;
    if (st == ATLAS_OK && atlas_ipc_param_str(req, "phase", &phase)) {
        if (strcmp(phase, "intent") == 0) {
            op->tool_phase = ATLAS_AI_TOOL_INTENT;
        } else if (strcmp(phase, "ok") == 0) {
            op->tool_phase = ATLAS_AI_TOOL_OK;
        } else if (strcmp(phase, "failed") == 0) {
            op->tool_phase = ATLAS_AI_TOOL_FAILED;
        } else {
            st = atlas_err_set(err, ATLAS_ERR_USAGE,
                               "\"phase\" must be \"intent\", \"ok\" or \"failed\"");
        }
    }
    if (st != ATLAS_OK) {
        atlas_ai_op_free(op);
        free(op);
        return st;
    }
    atlas_ai_result result;
    atlas_ai_result_init(&result);
    st = submit(ds, op, AI_WRITE_TIMEOUT_MS, &result, err);
    if (st == ATLAS_OK) {
        st = write_ai_result(ds, &result, err);
    }
    atlas_ai_result_free(&result);
    return st;
}

static atlas_status method_batch_correlate(dispatch_state *ds, const atlas_ipc_request *req,
                                           atlas_err *err) {
    atlas_ai_op *op = op_new(ATLAS_AI_OP_CORRELATE);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    atlas_status st = take_identity(req, op, err);
    if (st == ATLAS_OK) {
        st = take_paths(ds, req, &op->root, &op->repo_name, &op->paths, ATLAS_AI_MAX_BATCH_PATHS, err);
    }
    if (st == ATLAS_OK) {
        st = take_ident(req, "tool", 64u, &op->tool_name, err);
    }
    if (st == ATLAS_OK) {
        op->request_sync = true;
        (void)atlas_ipc_param_bool(req, "sync", &op->request_sync);
    }
    if (st != ATLAS_OK) {
        atlas_ai_op_free(op);
        free(op);
        return st;
    }
    atlas_ai_result result;
    atlas_ai_result_init(&result);
    st = submit(ds, op, AI_WRITE_TIMEOUT_MS, &result, err);
    if (st == ATLAS_OK) {
        st = write_ai_result(ds, &result, err);
    }
    atlas_ai_result_free(&result);
    return st;
}

/* --- durable records ------------------------------------------------------ */

static atlas_status method_reason_record(dispatch_state *ds, const atlas_ipc_request *req,
                                         atlas_err *err) {
    atlas_ai_op *op = op_new(ATLAS_AI_OP_REASON);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    atlas_status st = take_identity(req, op, err);
    if (st == ATLAS_OK) {
        st = take_text(req, "summary", ATLAS_AI_SUMMARY_MAX, &op->summary, err);
    }
    if (st == ATLAS_OK) {
        st = take_text(req, "detail", ATLAS_AI_DETAIL_MAX, &op->detail, err);
    }
    if (st == ATLAS_OK) {
        st = take_text(req, "unknown_reason", ATLAS_AI_SUMMARY_MAX, &op->unknown_reason, err);
    }
    if (st == ATLAS_OK) {
        st = take_paths(ds, req, &op->root, &op->repo_name, &op->paths, ATLAS_AI_MAX_PATHS_PER_RECORD, err);
    }
    if (st == ATLAS_OK) {
        (void)atlas_ipc_param_bool(req, "unknown", &op->unknown);
    }

    const char *prov = NULL;
    if (st == ATLAS_OK) {
        if (atlas_ipc_param_str(req, "provenance", &prov)) {
            atlas_provenance p = ATLAS_PROV_UNKNOWN;
            if (!atlas_provenance_parse(prov, &p)) {
                st = atlas_err_set(err, ATLAS_ERR_USAGE,
                                   "\"provenance\" is not a provenance class Atlas recognises");
            } else if (!atlas_provenance_writable_in_a2(p)) {
                /* Refused here rather than in the writer, so the request never
                 * reaches the queue. A caller asserting an approval is told
                 * plainly that Atlas will not record one, instead of having it
                 * quietly downgraded to a proposal. */
                st = atlas_err_set(
                    err, ATLAS_ERR_INTEGRITY,
                    "provenance %s cannot be written by an A2 adapter: Atlas has no way to prove "
                    "a human approved anything, so a model record is MODEL_PROPOSAL, "
                    "MODEL_INFERENCE or UNKNOWN",
                    prov);
            } else {
                op->provenance = p;
            }
        } else {
            op->provenance = op->unknown ? ATLAS_PROV_UNKNOWN : ATLAS_PROV_MODEL_PROPOSAL;
        }
    }
    const char *conf = NULL;
    if (st == ATLAS_OK && atlas_ipc_param_str(req, "confidence", &conf)) {
        if (!atlas_ai_confidence_parse(conf, &op->confidence)) {
            st = atlas_err_set(err, ATLAS_ERR_USAGE,
                               "\"confidence\" must be none, low, medium or high");
        }
    }
    if (st == ATLAS_OK && !op->unknown && op->summary.len == 0) {
        /* The one place Atlas insists on something. A reason with no text is not
         * a reason; if there is nothing truthful to say, `unknown` is the field
         * for that, and it is deliberately as easy to send. */
        st = atlas_err_set(err, ATLAS_ERR_USAGE,
                           "a recorded reason needs a \"summary\"; if there is no truthful reason "
                           "to give, send \"unknown\": true instead of inventing one");
    }
    if (st != ATLAS_OK) {
        atlas_ai_op_free(op);
        free(op);
        return st;
    }

    /* Read before submitting. The writer takes ownership of the operation and
     * may have freed it by the time the answer comes back, so what is echoed
     * has to be captured here. */
    atlas_provenance recorded = op->unknown ? ATLAS_PROV_UNKNOWN : op->provenance;
    bool recorded_unknown = op->unknown;

    atlas_ai_result result;
    atlas_ai_result_init(&result);
    st = submit(ds, op, AI_WRITE_TIMEOUT_MS, &result, err);
    if (st == ATLAS_OK) {
        st = write_ai_result(ds, &result, err);
    }
    if (st == ATLAS_OK) {
        /* Echoed so a caller cannot believe it recorded something stronger than
         * it did. A proposal that came back looking like a fact would defeat the
         * whole point of having the class. */
        st = atlas_json_key_str(ds->j, "provenance", atlas_provenance_name(recorded), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "state", recorded_unknown ? "unknown" : "proposed", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "approved", false, err);
    }
    atlas_ai_result_free(&result);
    return st;
}

static atlas_status method_decision_record(dispatch_state *ds, const atlas_ipc_request *req,
                                           atlas_err *err) {
    atlas_ai_op *op = op_new(ATLAS_AI_OP_DECISION);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    atlas_status st = take_identity(req, op, err);
    if (st == ATLAS_OK) {
        st = take_text(req, "title", ATLAS_AI_SUMMARY_MAX, &op->title, err);
    }
    if (st == ATLAS_OK) {
        st = take_text(req, "statement", ATLAS_AI_DETAIL_MAX, &op->statement, err);
    }
    if (st == ATLAS_OK) {
        st = take_text(req, "rationale", ATLAS_AI_DETAIL_MAX, &op->rationale, err);
    }
    if (st == ATLAS_OK) {
        st = take_paths(ds, req, &op->root, &op->repo_name, &op->paths, ATLAS_AI_MAX_PATHS_PER_RECORD, err);
    }
    const char *prov = NULL;
    if (st == ATLAS_OK && atlas_ipc_param_str(req, "provenance", &prov)) {
        atlas_provenance p = ATLAS_PROV_UNKNOWN;
        if (!atlas_provenance_parse(prov, &p)) {
            st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"provenance\" is not recognised");
        } else if (p != ATLAS_PROV_MODEL_PROPOSAL && p != ATLAS_PROV_MODEL_INFERENCE) {
            st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                               "a decision recorded by an A2 adapter is a proposal: provenance %s "
                               "is refused because Atlas cannot prove a human approved it",
                               prov);
        } else {
            op->provenance = p;
        }
    }
    if (st != ATLAS_OK) {
        atlas_ai_op_free(op);
        free(op);
        return st;
    }
    atlas_ai_result result;
    atlas_ai_result_init(&result);
    st = submit(ds, op, AI_WRITE_TIMEOUT_MS, &result, err);
    if (st == ATLAS_OK) {
        st = write_ai_result(ds, &result, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "approved", false, err);
    }
    if (st == ATLAS_OK && result.decision_uid.len > 0) {
        /* A4. The decision document this call materialised. Additive: an A2-era
         * client that does not read it is unaffected, and one that does no
         * longer has to tell a user to run `atlas decision promote`. */
        st = atlas_json_key_str(ds->j, "decision", atlas_buf_cstr(&result.decision_uid), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(
            ds->j, "approval",
            "recorded as a proposal. Atlas exposes no approval capability through MCP, hooks or "
            "any AI-facing method; a proposal becomes policy only through the interactive Atlas "
            "CLI on a terminal.",
            err);
    }
    atlas_ai_result_free(&result);
    return st;
}

/* --- reads ---------------------------------------------------------------- */

/* Resolves the session a read is about: exact `(provider, client, session_key)`
 * and nothing else, matching what a write would bind to.
 *
 * A read runs on a read-only handle, so the client row is looked up rather than
 * upserted — asking Atlas a question must not create the thing being asked
 * about. An unknown client, an unknown key or a closed session all yield 0 with
 * a reason, which is exactly what the write path reports for the same input, so
 * a caller that reads before writing is never told a session it will not get.
 *
 * `*reason_out` is one of the fixed ATLAS_AI_UNBOUND_* constants or NULL. */
static atlas_status read_session(dispatch_state *ds, const atlas_ipc_request *req,
                                 int64_t *session_out, const char **reason_out, atlas_err *err) {
    *session_out = 0;
    *reason_out = NULL;

    atlas_buf provider = ATLAS_BUF_INIT;
    atlas_buf client = ATLAS_BUF_INIT;
    atlas_buf key = ATLAS_BUF_INIT;
    atlas_status st = take_ident(req, "provider", ATLAS_AI_CLIENT_NAME_MAX, &provider, err);
    if (st == ATLAS_OK) {
        st = take_ident(req, "client", ATLAS_AI_CLIENT_NAME_MAX, &client, err);
    }
    if (st == ATLAS_OK) {
        st = take_text(req, "session_key", ATLAS_AI_SESSION_KEY_MAX, &key, err);
    }
    if (st == ATLAS_OK && key.len == 0) {
        *reason_out = ATLAS_AI_UNBOUND_NO_SESSION_ID;
    }
    int64_t client_id = 0;
    if (st == ATLAS_OK && key.len > 0) {
        st = atlas_db_ai_client_find(ds->db, provider.len > 0 ? atlas_buf_cstr(&provider) : "unknown",
                                     client.len > 0 ? atlas_buf_cstr(&client) : "unknown",
                                     &client_id, err);
    }
    if (st == ATLAS_OK && key.len > 0) {
        bool session_open = false;
        if (client_id > 0) {
            st = atlas_db_ai_session_find_state(ds->db, client_id, atlas_buf_cstr(&key),
                                                session_out, &session_open, err);
        }
        if (st == ATLAS_OK && *session_out == 0) {
            *reason_out = ATLAS_AI_UNBOUND_UNKNOWN_SESSION;
        } else if (st == ATLAS_OK && !session_open) {
            *session_out = 0;
            *reason_out = ATLAS_AI_UNBOUND_SESSION_CLOSED;
        }
    }
    atlas_buf_free(&provider);
    atlas_buf_free(&client);
    atlas_buf_free(&key);
    return st;
}

/* Resolves the repository a read is about, from either `repo` or `root`. */
static atlas_status read_repo(dispatch_state *ds, const atlas_ipc_request *req,
                              atlas_repo_info *out, bool *found, atlas_err *err) {
    *found = false;
    const char *name = NULL;
    if (atlas_ipc_param_str(req, "repo", &name)) {
        atlas_status st = atlas_db_check_repo_name(name, err);
        if (st != ATLAS_OK) {
            return st;
        }
        return atlas_db_repo_get(ds->db, name, out, found, err);
    }
    const char *path = NULL;
    if (atlas_ipc_param_str(req, "root", &path) && path[0] == '/') {
        return atlas_db_repo_get_containing(ds->db, path, strlen(path), out, found, err);
    }
    return ATLAS_OK;
}

/* The automatic context envelope, assembled server-side.
 *
 * Built here rather than in the adapter for one reason: the guarantee that
 * automatic context contains no repository prose has to be enforced where the
 * data is, not where it is rendered. `atlas_ai_context_render` checks its own
 * output against a fixed character allowlist before returning it, and this is
 * the only place the fields are chosen. */
static atlas_status method_context(dispatch_state *ds, const atlas_ipc_request *req,
                                   atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    bool found = false;
    atlas_status st = read_repo(ds, req, &info, &found, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }

    atlas_ai_context c;
    atlas_ai_context_init(&c);
    /* Reached over the socket, so by construction. */
    c.daemon_reachable = true;
    c.repo_known = found;

    if (found) {
        /* An opaque id and a hash of the root. Deliberately not the name and not
         * the path: both are chosen by whoever created the directory, and both
         * are entirely printable, so no encoding makes them safe to inject
         * automatically. See docs/ai-trust-boundary.md. */
        c.repo_id = info.id;
        atlas_ai_context_set_root_hash(&c, info.root_path.data, info.root_path.len);
        (void)snprintf(c.head_oid, sizeof(c.head_oid), "%s", info.scanned_head);
        (void)snprintf(c.head_state, sizeof(c.head_state), "%s", info.head_state);

        atlas_index_state state;
        atlas_index_state_init(&state);
        if (st == ATLAS_OK) {
            st = atlas_db_index_state_get(ds->db, info.id, &state, err);
        }
        if (st == ATLAS_OK) {
            const char *reason = NULL;
            c.index_current = atlas_server_index_current(&state, &reason);
            c.generation = state.last_complete_generation;
            if (reason != NULL) {
                st = atlas_buf_set_str(&c.not_current_reason, reason, err);
            }
        }
        /* A3: the structural index's own counters.
         *
         * Read here, next to everything else, and carried into the envelope as
         * integers and a boolean. Not one symbol name, path or include spelling
         * comes with them — see the note in `src/ai/context.c`. */
        if (st == ATLAS_OK) {
            atlas_code_index_state code_state;
            atlas_code_index_state_init(&code_state);
            st = atlas_db_code_state_get(ds->db, info.id, &code_state, err);
            if (st == ATLAS_OK) {
                const char *code_reason = NULL;
                c.code_index_current =
                    atlas_code_index_current(&state, &code_state, c.index_current, &code_reason);
                c.code_generation = code_state.last_complete_generation;
                c.code_symbols = code_state.symbols;
                c.code_relations = code_state.relations;
                c.code_ambiguous = code_state.ambiguous;
                c.code_unresolved = code_state.unresolved;
            }
            atlas_code_index_state_free(&code_state);
        }
        atlas_index_state_free(&state);
        if (st == ATLAS_OK) {
            st = atlas_db_events_head(ds->db, info.id, &c.event_cursor, err);
        }
        if (st == ATLAS_OK) {
            int64_t staged = 0;
            int64_t unstaged = 0;
            int64_t untracked = 0;
            int64_t unmerged = 0;
            st = atlas_db_worktree_changes_count(ds->db, info.id, &staged, &unstaged, &untracked,
                                                 &unmerged, err);
            c.changed_paths = staged + unstaged + untracked + unmerged;
        }
        if (st == ATLAS_OK) {
            /* A2's record counts are still read for the reason count. The
             * *decision* counts now come from the A4 lifecycle, where an
             * approval can actually exist — A2's `approved` was pinned to zero
             * for two phases because nothing could produce one. Reading both
             * keeps A2 proposals visible as reasons without letting them
             * masquerade as decision documents. */
            int64_t legacy_proposed = 0;
            int64_t legacy_approved = 0;
            st = atlas_db_ai_repo_record_counts(ds->db, info.id, &legacy_proposed,
                                                &legacy_approved, &c.unresolved_reasons, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_db_decision_repo_counts(ds->db, info.id, &c.proposed_decisions,
                                               &c.approved_decisions, &c.rejected_decisions,
                                               &c.superseded_decisions, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_db_decision_review_count(ds->db, info.id, &c.decisions_needing_review, err);
        }

        /* The caller's own session, by exact key.
         *
         * Never "the newest open session for this repository": with two clients
         * on one worktree that reports one of them its neighbour's change set,
         * and the envelope is injected into a model's context, so a wrong number
         * here becomes a wrong belief. No session yields 0, which the envelope
         * reports as session=0 rather than omitting. */
        if (st == ATLAS_OK) {
            const char *unbound = NULL;
            st = read_session(ds, req, &c.session_id, &unbound, err);
        }
        if (st == ATLAS_OK && c.session_id > 0) {
            st = atlas_db_ai_change_set_find(ds->db, c.session_id, info.id, &c.change_set_id, err);
        }
        if (st == ATLAS_OK && c.change_set_id > 0) {
            int64_t total = 0;
            int64_t direct = 0;
            int64_t ambiguous = 0;
            st = atlas_db_ai_changed_counts(ds->db, c.change_set_id, &total, &direct, &ambiguous,
                                            &c.unresolved_reasons, err);
        }
    }

    atlas_buf text = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_ai_context_render(&c, &text, err);
    }
    if (st == ATLAS_OK) {
        /* Emitted through the typed writer like everything else. The envelope's
         * own character allowlist is a second, independent bound. */
        st = atlas_json_key_str(ds->j, "context", atlas_buf_cstr(&text), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "context_version", ATLAS_AI_CONTEXT_VERSION, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "context_bytes", (int64_t)text.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "repo_known", found, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "session", c.session_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "change_set", c.change_set_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "changed_paths", c.changed_paths, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "unresolved_reasons", c.unresolved_reasons, err);
    }
    /* A4, as structured fields beside the rendered envelope. Integers only,
     * for the same reason the envelope carries only integers. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "decisions_proposed", c.proposed_decisions, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "decisions_approved", c.approved_decisions, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "decisions_needing_review", c.decisions_needing_review,
                                err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "code_index_current", c.code_index_current, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "code_symbols", c.code_symbols, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "code_relations", c.code_relations, err);
    }
    if (st == ATLAS_OK && found) {
        /* The identifiers the envelope itself carries, echoed as structured
         * fields. Deliberately the id and the hash rather than the name: this
         * result exists so an adapter can inject `context`, and an adapter that
         * found a name here might inject that too. There is nothing in this
         * response an adapter could put in automatic context by mistake. */
        st = atlas_json_key_int(ds->j, "repo_id", c.repo_id, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "root_hash", c.root_hash, err);
        }
    }
    atlas_buf_free(&text);
    atlas_ai_context_free(&c);
    atlas_repo_info_free(&info);
    return st;
}

static atlas_status method_session_get(dispatch_state *ds, const atlas_ipc_request *req,
                                       atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    bool found = false;
    atlas_status st = read_repo(ds, req, &info, &found, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }

    /* The caller's session, resolved the same way a write resolves it, so this
     * read cannot promise an attachment the write would refuse. */
    int64_t session_id = 0;
    const char *unbound = NULL;
    st = read_session(ds, req, &session_id, &unbound, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "present", session_id > 0, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "session", session_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "session_unbound", session_id == 0, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "unbound_reason", unbound != NULL ? unbound : "", err);
    }
    if (st == ATLAS_OK && found) {
        /* A repository-level fact, which is all that can be said truthfully
         * without a session key: how many sessions currently have this worktree
         * open. A caller with no session of its own learns that changes here may
         * not be its own, without being handed somebody else's session. */
        int64_t open_sessions = 0;
        st = atlas_db_ai_concurrent_sessions(ds->db, info.id, 0, &open_sessions, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "open_sessions", open_sessions, err);
        }
    }
    if (st == ATLAS_OK && found) {
        st = atlas_json_key_str(ds->j, "repo", info.name, err);
    }
    int64_t change_set = 0;
    if (st == ATLAS_OK && session_id > 0 && found) {
        st = atlas_db_ai_change_set_find(ds->db, session_id, info.id, &change_set, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "change_set", change_set, err);
    }
    if (st == ATLAS_OK && change_set > 0) {
        int64_t total = 0;
        int64_t direct = 0;
        int64_t ambiguous = 0;
        int64_t unresolved = 0;
        st = atlas_db_ai_changed_counts(ds->db, change_set, &total, &direct, &ambiguous,
                                        &unresolved, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "changed_paths", total, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "direct_paths", direct, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "ambiguous_paths", ambiguous, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "unresolved_paths", unresolved, err);
        }
    }
    if (st == ATLAS_OK && found) {
        int64_t concurrent = 0;
        st = atlas_db_ai_concurrent_sessions(ds->db, info.id, session_id, &concurrent, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "concurrent_sessions", concurrent, err);
        }
    }
    atlas_repo_info_free(&info);
    return st;
}

/* Emits one changed path from the index's working-tree snapshot. */
static atlas_status emit_wt_change(const atlas_worktree_change_row *row, void *ud,
                                   atlas_err *err) {
    dispatch_state *ds = (dispatch_state *)ud;
    atlas_status st = atlas_json_obj_begin(ds->j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "scope", row->scope, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "change_type", row->change_type, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "status", row->status, err);
    }
    if (st == ATLAS_OK) {
        /* Stored in the safe path encoding; re-encoding would stop it decoding
         * back to the original bytes. */
        st = atlas_json_key_str(ds->j, "path", row->path_text, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "old_path", row->old_path_text, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "is_directory", row->is_directory, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "evidence", "SOURCE", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(ds->j, err);
    }
    return st;
}

static atlas_status method_changed(dispatch_state *ds, const atlas_ipc_request *req,
                                   atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    bool found = false;
    atlas_status st = read_repo(ds, req, &info, &found, err);
    if (st == ATLAS_OK && !found) {
        st = atlas_err_set(err, ATLAS_ERR_REPO, "no registered repository matches this request");
    }
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }

    const char *scope = NULL;
    if (atlas_ipc_param_str(req, "scope", &scope)) {
        if (strcmp(scope, "all") == 0) {
            scope = NULL;
        } else if (strcmp(scope, "staged") != 0 && strcmp(scope, "unstaged") != 0 &&
                   strcmp(scope, "untracked") != 0 && strcmp(scope, "unmerged") != 0) {
            atlas_repo_info_free(&info);
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "\"scope\" must be staged, unstaged, untracked, unmerged or all");
        }
    }
    int64_t limit = 0;
    (void)atlas_ipc_param_int(req, "limit", &limit);
    int64_t cursor = 0;
    (void)atlas_ipc_param_int(req, "cursor", &cursor);
    if (cursor < 0) {
        cursor = 0;
    }

    int64_t staged = 0;
    int64_t unstaged = 0;
    int64_t untracked = 0;
    int64_t unmerged = 0;
    st = atlas_db_worktree_changes_count(ds->db, info.id, &staged, &unstaged, &untracked, &unmerged,
                                         err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "repo", info.name, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "counts", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_begin(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "staged", staged, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "unstaged", unstaged, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "untracked", untracked, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "unmerged", unmerged, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "changes", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    int64_t count = 0;
    int64_t next = cursor;
    bool more = false;
    if (st == ATLAS_OK) {
        st = atlas_db_worktree_changes_list(ds->db, info.id, scope, cursor, limit, emit_wt_change,
                                            ds, &count, &next, &more, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "count", count, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "cursor", next, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "more", more, err);
    }
    if (st == ATLAS_OK) {
        /* The snapshot describes the last completed pass, which may be older
         * than the working tree. Saying which pass is what lets a caller tell a
         * stale answer from a current one. */
        atlas_index_state state;
        atlas_index_state_init(&state);
        st = atlas_db_index_state_get(ds->db, info.id, &state, err);
        if (st == ATLAS_OK) {
            const char *reason = NULL;
            bool current = atlas_server_index_current(&state, &reason);
            st = atlas_json_key_bool(ds->j, "index_current", current, err);
            if (st == ATLAS_OK) {
                st = atlas_json_key_str_opt(ds->j, "not_current_reason", reason, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_key_str(ds->j, "observed_at", state.last_complete_at, err);
            }
        }
        atlas_index_state_free(&state);
    }
    atlas_repo_info_free(&info);
    return st;
}

/* Emits a recorded reason. Everything here is model-authored, so the provenance
 * travels with it and the caller is told plainly that it is not approved. */
static atlas_status emit_reason(const atlas_ai_reason_row *row, void *ud, atlas_err *err) {
    dispatch_state *ds = (dispatch_state *)ud;
    atlas_status st = atlas_json_obj_begin(ds->j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "id", row->id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "session", row->session_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "at", row->created_at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "provenance", row->provenance, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "state", row->state, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "confidence", row->confidence, err);
    }
    if (st == ATLAS_OK) {
        /* Stored safe-encoded on the way in, so emitted as-is. */
        st = atlas_json_key_str_opt(ds->j, "summary", row->summary, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "detail", row->detail, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "unknown_reason", row->unknown_reason, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "paths", row->path_count, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "approved", row->approved, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(ds->j, err);
    }
    return st;
}

static atlas_status emit_decision(const atlas_ai_decision_row *row, void *ud, atlas_err *err) {
    dispatch_state *ds = (dispatch_state *)ud;
    atlas_status st = atlas_json_obj_begin(ds->j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "id", row->id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "session", row->session_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "at", row->created_at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "provenance", row->provenance, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "state", row->state, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "title", row->title, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "statement", row->statement, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "rationale", row->rationale, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "paths", row->path_count, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "approved", row->approved, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(ds->j, err);
    }
    return st;
}

/* One typed file role, with the basis it was arrived at on. Path naming is
 * evidence about a path and not proof about a file, and the basis is what keeps
 * that visible to a reader. */
static atlas_status emit_code_role(const atlas_code_role_row *row, void *ud, atlas_err *err) {
    dispatch_state *ds = (dispatch_state *)ud;
    atlas_status st = atlas_json_obj_begin(ds->j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "role", row->role, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "basis", row->basis, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "resolution", row->resolution, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(ds->j, err);
    }
    return st;
}

/* Indexed facts about one path, plus its recorded history and any reasons.
 *
 * The history rows carry commit subjects, which are repository prose. They are
 * returned because a caller asked for this path specifically, they are bounded,
 * and every one of them is labelled GIT. They never reach automatic context. */
static atlas_status emit_file_row(const atlas_file_row *row, void *ud, atlas_err *err) {
    dispatch_state *ds = (dispatch_state *)ud;
    atlas_status st = atlas_json_key_bool(ds->j, "indexed", true, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "path", row->path_text, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "file_type", row->file_type, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "language", row->language, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "git_mode", row->git_mode, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "git_index_oid", row->git_index_oid, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "content_hash", row->content_hash, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "size_bytes", row->size_known ? row->size_bytes : 0, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "tracked", row->tracked, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "deleted", row->deleted, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "evidence", "SOURCE", err);
    }
    return st;
}

static atlas_status emit_history(const atlas_history_row *row, void *ud, atlas_err *err) {
    dispatch_state *ds = (dispatch_state *)ud;
    atlas_status st = atlas_json_obj_begin(ds->j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "commit", row->commit_oid, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "change_type", row->change_type, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "commit_time", row->commit_time, err);
    }
    if (st == ATLAS_OK) {
        /* Read raw from git, so encoded here at the point of output. Bounded,
         * because a commit subject is evidence and a commit subject that fills a
         * screen is a payload. */
        char subject[ATLAS_MCP_MAX_PROSE_BYTES + 1u];
        (void)snprintf(subject, sizeof(subject), "%s", row->subject != NULL ? row->subject : "");
        st = atlas_json_key_str(ds->j, "subject", atlas_safe(&ds->safe, subject), err);
    }
    if (st == ATLAS_OK) {
        char author[128];
        (void)snprintf(author, sizeof(author), "%s",
                       row->author_name != NULL ? row->author_name : "");
        st = atlas_json_key_str(ds->j, "author", atlas_safe(&ds->safe, author), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "evidence", "GIT", err);
    }
    if (st == ATLAS_OK) {
        /* A commit subject says what was written, not why it was done. A0
         * established that and A2 does not weaken it: the reason stays UNKNOWN
         * unless somebody recorded one, which is what `reasons` below carries. */
        st = atlas_json_key_str(ds->j, "reason", "UNKNOWN", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "untrusted_data", true, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(ds->j, err);
    }
    return st;
}

static atlas_status method_file_context(dispatch_state *ds, const atlas_ipc_request *req,
                                        atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    bool found = false;
    atlas_status st = read_repo(ds, req, &info, &found, err);
    if (st == ATLAS_OK && !found) {
        st = atlas_err_set(err, ATLAS_ERR_REPO, "no registered repository matches this request");
    }
    const char *path = NULL;
    if (st == ATLAS_OK && !atlas_ipc_param_str(req, "path", &path)) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "ai.file.context needs a \"path\" parameter");
    }
    atlas_buf raw = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_path_text_decode(path, strlen(path), &raw, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_path_check_relative(raw.data, raw.len, err);
    }
    if (st != ATLAS_OK) {
        atlas_buf_free(&raw);
        atlas_repo_info_free(&info);
        return st;
    }

    int64_t limit = 0;
    (void)atlas_ipc_param_int(req, "limit", &limit);
    if (limit <= 0 || limit > ATLAS_MCP_MAX_ROWS) {
        limit = 20;
    }

    st = atlas_json_key_str(ds->j, "repo", info.name, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "path", path, err);
    }
    bool file_found = false;
    if (st == ATLAS_OK) {
        st = atlas_db_file_get(ds->db, info.id, raw.data, raw.len, emit_file_row, ds, &file_found,
                               err);
    }
    if (st == ATLAS_OK && !file_found) {
        st = atlas_json_key_bool(ds->j, "indexed", false, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "history", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    int64_t hcount = 0;
    if (st == ATLAS_OK) {
        st = atlas_db_file_history(ds->db, info.id, raw.data, raw.len, limit, emit_history, ds,
                                   &hcount, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "history_count", hcount, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "reasons", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    int64_t rcount = 0;
    bool rmore = false;
    if (st == ATLAS_OK) {
        st = atlas_db_ai_reasons_list(ds->db, info.id, raw.data, raw.len, limit, emit_reason, ds,
                                      &rcount, &rmore, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "reason_count", rcount, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "decisions", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    int64_t dcount = 0;
    bool dmore = false;
    if (st == ATLAS_OK) {
        st = atlas_db_ai_decisions_list(ds->db, info.id, raw.data, raw.len, limit, emit_decision,
                                        ds, &dcount, &dmore, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "decision_count", dcount, err);
    }
    /* A3: the structural synopsis, folded into the tool that already answers
     * "what does Atlas know about this path" rather than given a tool of its
     * own. A model asking about a file wants one answer, and two tools returning
     * overlapping halves of it is how they come to disagree.
     *
     * Deliberately counts and states rather than lists: the lists are what
     * `atlas_code_file` is for, and a file context that carried every symbol
     * would be the thing that fills a context window. */
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "structure", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_begin(ds->j, err);
    }
    if (st == ATLAS_OK) {
        atlas_index_state fs;
        atlas_index_state_init(&fs);
        atlas_code_index_state cs;
        atlas_code_index_state_init(&cs);
        st = atlas_db_index_state_get(ds->db, info.id, &fs, err);
        if (st == ATLAS_OK) {
            st = atlas_db_code_state_get(ds->db, info.id, &cs, err);
        }
        if (st == ATLAS_OK) {
            const char *fr = NULL;
            bool file_current = atlas_server_index_current(&fs, &fr);
            const char *reason = NULL;
            bool current = atlas_code_index_current(&fs, &cs, file_current, &reason);
            st = atlas_json_key_bool(ds->j, "code_index_current", current, err);
            if (st == ATLAS_OK) {
                st = atlas_json_key_str_opt(
                    ds->j, "code_not_current_reason",
                    reason == NULL
                        ? NULL
                        : (atlas_code_not_current_reason_is_known(reason) ? reason : "other"),
                    err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_key_int(ds->j, "code_generation", cs.last_complete_generation, err);
            }
        }
        atlas_index_state_free(&fs);
        atlas_code_index_state_free(&cs);
    }
    if (st == ATLAS_OK) {
        int64_t code_file_id = 0;
        bool indexed = false;
        st = atlas_db_code_file_get(ds->db, info.id, raw.data, raw.len, NULL, NULL, &indexed,
                                    &code_file_id, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_bool(ds->j, "indexed", indexed, err);
        }
        if (st == ATLAS_OK && indexed) {
            st = atlas_json_key(ds->j, "roles", err);
            if (st == ATLAS_OK) {
                st = atlas_json_arr_begin(ds->j, err);
            }
            int64_t roles = 0;
            if (st == ATLAS_OK) {
                st = atlas_db_code_roles_of(ds->db, code_file_id, emit_code_role, ds, &roles, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_arr_end(ds->j, err);
            }
            int64_t ambiguous = 0;
            int64_t unresolved = 0;
            if (st == ATLAS_OK) {
                st = atlas_db_code_file_unsettled(ds->db, code_file_id, &ambiguous, &unresolved,
                                                  err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_key_int(ds->j, "ambiguous", ambiguous, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_key_int(ds->j, "unresolved", unresolved, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_key_str(ds->j, "detail",
                                        "call atlas_code_file for the symbols, includes and "
                                        "dependents, and atlas_code_impact before changing a "
                                        "shared header",
                                        err);
            }
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(ds->j, err);
    }

    if (st == ATLAS_OK && rcount == 0 && dcount == 0) {
        /* The A0 answer, unchanged, and A3 does not weaken it: structure is not
         * a reason. Nobody recorded one, so there is not one, and Atlas says so
         * rather than offering the commit subject or an include graph as a
         * substitute. */
        st = atlas_json_key_str(ds->j, "reason", "UNKNOWN", err);
    }
    atlas_buf_free(&raw);
    atlas_repo_info_free(&info);
    return st;
}

/* A bounded search over what a model previously recorded. */
static atlas_status method_memory_search(dispatch_state *ds, const atlas_ipc_request *req,
                                         atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    bool found = false;
    atlas_status st = read_repo(ds, req, &info, &found, err);
    if (st == ATLAS_OK && !found) {
        st = atlas_err_set(err, ATLAS_ERR_REPO, "no registered repository matches this request");
    }
    const char *query = NULL;
    if (st == ATLAS_OK && !atlas_ipc_param_str(req, "query", &query)) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "ai.memory.search needs a \"query\" parameter");
    }
    if (st == ATLAS_OK && (query[0] == '\0' || strlen(query) > 256u)) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"query\" must be 1..256 bytes");
    }
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }
    int64_t limit = 0;
    (void)atlas_ipc_param_int(req, "limit", &limit);

    /* The pattern is built here rather than by the caller, so a query cannot
     * become a wildcard the caller did not ask for. `\` escapes the LIKE
     * metacharacters and itself; the statement declares ESCAPE '\'. */
    atlas_buf pattern = ATLAS_BUF_INIT;
    st = atlas_buf_append_ch(&pattern, '%', err);
    for (const char *p = query; st == ATLAS_OK && *p != '\0'; p++) {
        if (*p == '%' || *p == '_' || *p == '\\') {
            st = atlas_buf_append_ch(&pattern, '\\', err);
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_append_ch(&pattern, *p, err);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_append_ch(&pattern, '%', err);
    }

    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "repo", info.name, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "reasons", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    int64_t rcount = 0;
    bool rmore = false;
    if (st == ATLAS_OK) {
        st = atlas_db_ai_reasons_search(ds->db, info.id, atlas_buf_cstr(&pattern), limit,
                                        emit_reason, ds, &rcount, &rmore, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "reason_count", rcount, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "reasons_more", rmore, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "decisions", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    int64_t dcount = 0;
    bool dmore = false;
    if (st == ATLAS_OK) {
        st = atlas_db_ai_decisions_search(ds->db, info.id, atlas_buf_cstr(&pattern), limit,
                                          emit_decision, ds, &dcount, &dmore, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "decision_count", dcount, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "decisions_more", dmore, err);
    }
    atlas_buf_free(&pattern);
    atlas_repo_info_free(&info);
    return st;
}

/* Bounded search over the index, reusing the same query the CLI uses.
 *
 * Not a second implementation: `atlas_db_search` is the one A0 wrote, including
 * its FTS5 / degraded-LIKE split, so an MCP caller and a shell user searching
 * the same repository get the same rows and the same honesty about how they
 * were ranked. */
typedef struct search_ctx {
    dispatch_state *ds;
    int64_t count;
} search_ctx;

static atlas_status emit_search_hit(const atlas_search_hit *hit, void *ud, atlas_err *err) {
    search_ctx *sc = (search_ctx *)ud;
    dispatch_state *ds = sc->ds;
    atlas_status st = atlas_json_obj_begin(ds->j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "kind", hit->kind, err);
    }
    if (st == ATLAS_OK && hit->path_text != NULL) {
        /* Stored in the safe path encoding, emitted as-is. */
        st = atlas_json_key_str(ds->j, "path", hit->path_text, err);
    }
    if (st == ATLAS_OK && hit->commit_oid != NULL) {
        st = atlas_json_key_str(ds->j, "commit", hit->commit_oid, err);
    }
    if (st == ATLAS_OK && hit->subject != NULL) {
        /* Read raw from git, so bounded and encoded here at the point of
         * output. A commit subject is evidence; one that fills a screen is a
         * payload. */
        char subject[ATLAS_MCP_MAX_PROSE_BYTES + 1u];
        (void)snprintf(subject, sizeof(subject), "%s", hit->subject);
        st = atlas_json_key_str(ds->j, "subject", atlas_safe(&ds->safe, subject), err);
    }
    if (st == ATLAS_OK && hit->author_name != NULL) {
        char author[128];
        (void)snprintf(author, sizeof(author), "%s", hit->author_name);
        st = atlas_json_key_str(ds->j, "author", atlas_safe(&ds->safe, author), err);
    }
    if (st == ATLAS_OK && hit->author_time > 0) {
        st = atlas_json_key_int(ds->j, "time", hit->author_time, err);
    }
    /* Additive, for `atlas search` over the socket: both are display facts the
     * listing already showed and neither had a reader on the wire. */
    if (st == ATLAS_OK && strcmp(hit->kind, "file") == 0) {
        st = atlas_json_key_bool(ds->j, "deleted", hit->deleted, err);
    }
    if (st == ATLAS_OK && hit->git_index_oid != NULL) {
        st = atlas_json_key_str(ds->j, "git_index_oid", hit->git_index_oid, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "evidence", hit->evidence, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "untrusted_data", true, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        sc->count++;
    }
    return st;
}

static atlas_status method_repo_search(dispatch_state *ds, const atlas_ipc_request *req,
                                       atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    bool found = false;
    atlas_status st = read_repo(ds, req, &info, &found, err);
    if (st == ATLAS_OK && !found) {
        st = atlas_err_set(err, ATLAS_ERR_REPO, "no registered repository matches this request");
    }
    const char *query = NULL;
    if (st == ATLAS_OK && !atlas_ipc_param_str(req, "query", &query)) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "repo.search needs a \"query\" parameter");
    }
    if (st == ATLAS_OK && (query[0] == '\0' || strlen(query) > 256u)) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"query\" must be 1..256 bytes");
    }
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }
    int64_t limit = 0;
    (void)atlas_ipc_param_int(req, "limit", &limit);
    if (limit <= 0 || limit > ATLAS_MCP_MAX_ROWS) {
        limit = ATLAS_MCP_DEFAULT_ROWS;
    }

    atlas_search_mode mode = ATLAS_SEARCH_FTS5;
    st = atlas_json_key_str(ds->j, "repo", info.name, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "results", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    search_ctx sc = {ds, 0};
    int64_t count = 0;
    if (st == ATLAS_OK) {
        st = atlas_db_search(ds->db, info.id, query, limit, &mode, emit_search_hit, &sc, &count,
                             err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "count", count, err);
    }
    if (st == ATLAS_OK) {
        /* Whether results were ranked or fell back to a bounded substring scan.
         * A caller must never have to guess which it got. */
        st = atlas_json_key_str(ds->j, "search_mode", atlas_search_mode_name(mode), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "degraded", mode != ATLAS_SEARCH_FTS5, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "more", count >= limit, err);
    }
    atlas_repo_info_free(&info);
    return st;
}

/* --- the group ------------------------------------------------------------ */

static const atlas_method_entry AI_METHODS[] = {
    {"repo.resolve", method_repo_resolve},

    {"repo.search", method_repo_search},
    {"ai.session.open", method_session_open},
    {"ai.session.touch", method_session_touch},
    {"ai.session.close", method_session_close},
    {"ai.session.attach", method_session_attach},
    {"ai.session.checkpoint", method_session_checkpoint},
    {"ai.session.get", method_session_get},
    {"ai.tool.record", method_tool_record},
    {"ai.batch.correlate", method_batch_correlate},
    {"ai.turn.close", method_turn_close},
    {"ai.reason.record", method_reason_record},
    {"ai.decision.record", method_decision_record},
    {"ai.context", method_context},
    {"ai.changed", method_changed},
    {"ai.file.context", method_file_context},
    {"ai.memory.search", method_memory_search},
};

const atlas_method_entry *atlas_server_ai_methods(size_t *count_out) {
    if (count_out != NULL) {
        *count_out = sizeof(AI_METHODS) / sizeof(AI_METHODS[0]);
    }
    return AI_METHODS;
}
