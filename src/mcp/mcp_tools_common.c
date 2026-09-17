/* Atlas - common MCP tools.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Shared transport and trust contracts: mcp_tools.c and mcp_tools_internal.h.
 */
#define _GNU_SOURCE 1

#include <stdlib.h>
#include <string.h>

#include "atlas/ai.h"
#include "atlas/atlas.h"
#include "atlas/pathrep.h"
#include "mcp/mcp_internal.h"


#include "mcp/mcp_tools_internal.h"

/* --- schema helpers ------------------------------------------------------- */

static atlas_status prop_begin(atlas_json *j, const char *name, const char *type,
                               const char *description, atlas_err *err) {
    atlas_status st = atlas_json_key(j, name, err);
    if (st == ATLAS_OK) {
        st = atlas_json_obj_begin(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "type", type, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "description", description, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_prop_str(atlas_json *j, const char *name, const char *description,
                             int64_t max_len, atlas_err *err) {
    atlas_status st = prop_begin(j, name, "string", description, err);
    if (st == ATLAS_OK && max_len > 0) {
        st = atlas_json_key_int(j, "maxLength", max_len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(j, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_prop_int(atlas_json *j, const char *name, const char *description,
                             int64_t min, int64_t max, atlas_err *err) {
    atlas_status st = prop_begin(j, name, "integer", description, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "minimum", min, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "maximum", max, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(j, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_prop_bool(atlas_json *j, const char *name, const char *description,
                              atlas_err *err) {
    atlas_status st = prop_begin(j, name, "boolean", description, err);
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(j, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_prop_enum(atlas_json *j, const char *name, const char *description,
                              const char *const *values, atlas_err *err) {
    atlas_status st = prop_begin(j, name, "string", description, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key(j, "enum", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(j, err);
    }
    for (size_t i = 0; st == ATLAS_OK && values[i] != NULL; i++) {
        st = atlas_json_str(j, values[i], err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(j, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_prop_paths(atlas_json *j, const char *description, atlas_err *err) {
    atlas_status st = prop_begin(j, "paths", "array", description, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "maxItems", ATLAS_AI_MAX_PATHS_PER_RECORD, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key(j, "items", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_begin(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "type", "string", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "description",
                                "a repository-relative path, never absolute", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(j, err);
    }
    return st;
}

/* A bounded array of plain strings, for the A4 argument lists that are not
 * paths. Separate from `prop_paths` because a path carries the
 * "repository-relative, never absolute" rule with it and a symbol name or an
 * alternative does not. */
atlas_status atlas_mcp_tools_prop_str_array(atlas_json *j, const char *name, const char *description,
                                   int64_t max_items, atlas_err *err) {
    atlas_status st = prop_begin(j, name, "array", description, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "maxItems", max_items, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key(j, "items", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_begin(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "type", "string", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(j, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_prop_repo(atlas_json *j, atlas_err *err) {
    return atlas_mcp_tools_prop_str(j, "repo",
                    "the Atlas repository name. Omit to use the first granted root. A name "
                    "outside the granted roots is refused.",
                    ATLAS_NAME_MAX, err);
}

/* Opens the schema object and its `properties`. */
atlas_status atlas_mcp_tools_schema_begin(atlas_json *j, atlas_err *err) {
    atlas_status st = atlas_json_obj_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "type", "object", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key(j, "properties", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_begin(j, err);
    }
    return st;
}

/* Closes `properties`, lists the required members, and forbids anything else.
 *
 * `additionalProperties: false` is deliberate. A tool that silently accepts an
 * argument it does not implement lets a caller believe it asked for something. */
atlas_status atlas_mcp_tools_schema_end(atlas_json *j, const char *const *required, atlas_err *err) {
    atlas_status st = atlas_json_obj_end(j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key(j, "required", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(j, err);
    }
    for (size_t i = 0; st == ATLAS_OK && required != NULL && required[i] != NULL; i++) {
        st = atlas_json_str(j, required[i], err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(j, "additionalProperties", false, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(j, err);
    }
    return st;
}

/* --- argument extraction --------------------------------------------------- */

/* A bounded string argument. Absent is not an error; over-length is. */
atlas_status atlas_mcp_tools_arg_str(const atlas_jsonv *args, const char *key, size_t max,
                            const char **out, atlas_err *err) {
    *out = NULL;
    const char *s = atlas_jsonv_str_member(args, key);
    if (s == NULL) {
        return ATLAS_OK;
    }
    if (strlen(s) > max) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "\"%s\" is longer than %zu bytes; it was refused rather than "
                             "truncated",
                             key, max);
    }
    *out = s;
    return ATLAS_OK;
}

/* A repository-relative path argument.
 *
 * Absolute paths are refused rather than resolved. MCP is not a filesystem
 * reader: every path it accepts names something inside a repository the client
 * granted, and accepting an absolute one would make the granted-roots check
 * decorative. */
atlas_status atlas_mcp_tools_arg_rel_path(const atlas_jsonv *args, const char *key, const char **out,
                                 atlas_err *err) {
    atlas_status st = atlas_mcp_tools_arg_str(args, key, 4096u, out, err);
    if (st != ATLAS_OK || *out == NULL) {
        return st;
    }
    if ((*out)[0] == '/') {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "\"%s\" must be repository-relative; Atlas does not read absolute "
                             "paths through MCP",
                             key);
    }
    atlas_buf decoded = ATLAS_BUF_INIT;
    st = atlas_path_text_decode(*out, strlen(*out), &decoded, err);
    if (st == ATLAS_OK) {
        st = atlas_path_check_relative(decoded.data, decoded.len, err);
    }
    atlas_buf_free(&decoded);
    return st;
}

int64_t atlas_mcp_tools_arg_int(const atlas_jsonv *args, const char *key, int64_t def) {
    int64_t v = 0;
    if (atlas_jsonv_int(atlas_jsonv_get(args, key), &v)) {
        return v;
    }
    return def;
}

/* --- forwarding a daemon result ------------------------------------------- */

typedef struct forward_ctx {
    const atlas_ipc_response *response;
    bool degraded;
    const char *provenance;
    bool untrusted;
    const char *message;      /* set when there is no response to forward */
    const char *unbound_note; /* set when the daemon stored the record sessionless */
} forward_ctx;

/* Explains an unbound session, from a fixed table keyed by the daemon's typed
 * reason. Atlas-owned text chosen by a switch, never assembled from the
 * response: this string ends up in a model's context.
 *
 * Every note describes the *connection's* binding state, not what happened to a
 * record — the same field is set on reads (`atlas_session_state`) and on writes,
 * and a note saying "recorded" would be false on a read. Whether a write was
 * stored is already in the result, as `record`. */
static const char *unbound_note(const char *reason) {
    if (reason == NULL) {
        return NULL;
    }
    if (strcmp(reason, ATLAS_AI_UNBOUND_NO_SESSION_ID) == 0) {
        return "This connection is not attached to any Atlas session: it was started without "
               "CLAUDE_CODE_SESSION_ID, so Atlas cannot tell which session it belongs to. "
               "Anything recorded through it is stored unattached rather than credited to a "
               "session Atlas guessed at.";
    }
    if (strcmp(reason, ATLAS_AI_UNBOUND_UNKNOWN_SESSION) == 0) {
        return "This connection is not attached to any Atlas session: Atlas has never seen this "
               "session id, which usually means the Claude Code hooks are not installed. Run "
               "`atlas integrate claude doctor` to check. Anything recorded through it is stored "
               "unattached.";
    }
    if (strcmp(reason, ATLAS_AI_UNBOUND_SESSION_CLOSED) == 0) {
        return "This connection is not attached to any Atlas session: the session it was started "
               "for has ended — after `/clear`, this server still holds the id of the "
               "conversation that was cleared. Atlas will not credit current work to a finished "
               "session, so anything recorded through it is stored unattached.";
    }
    return NULL;
}

/* Builds the tool's body document: the Atlas envelope plus the daemon's result.
 *
 * The result is re-emitted through the streaming writer rather than copied, so
 * every string in it is escaped by Atlas rather than trusted from the socket.
 * There is still no "write these bytes as JSON" primitive anywhere. */
static atlas_status build_body(atlas_json *j, void *ud, atlas_err *err) {
    forward_ctx *f = (forward_ctx *)ud;
    atlas_status st = atlas_json_obj_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "atlas", ATLAS_VERSION_STRING, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "phase", ATLAS_PHASE, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "context_version", ATLAS_AI_CONTEXT_VERSION, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(j, "ok", !f->degraded, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(j, "degraded", f->degraded, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "provenance", f->provenance, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(j, "untrusted_data", f->untrusted, err);
    }
    if (st == ATLAS_OK && f->untrusted) {
        st = atlas_json_key_str(j, "notice", UNTRUSTED_NOTICE, err);
    }
    if (st == ATLAS_OK && f->message != NULL) {
        st = atlas_json_key_str(j, "message", f->message, err);
    }
    if (st == ATLAS_OK && f->unbound_note != NULL) {
        /* Said in words as well as in the typed `session_unbound` field below.
         * A record stored without a session is a different thing from one stored
         * with it, and a caller that reads only prose should not have to infer
         * that from a boolean it did not look at. Atlas-owned text. */
        st = atlas_json_key_str(j, "attribution", f->unbound_note, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key(j, "result", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_ipc_result_write(f->response, j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(j, err);
    }
    return st;
}

/* Serialises a document into `out` using the streaming writer. */
atlas_status atlas_mcp_tools_render(atlas_buf *out, atlas_status (*build)(atlas_json *, void *, atlas_err *),
                           void *ud, atlas_err *err) {
    char *buffer = NULL;
    size_t size = 0;
    FILE *mem = open_memstream(&buffer, &size);
    if (mem == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "cannot open a result buffer");
    }
    atlas_json *j = atlas_json_new(mem, err);
    if (j == NULL) {
        (void)fclose(mem);
        free(buffer);
        return err->status;
    }
    atlas_status st = build(j, ud, err);
    if (st == ATLAS_OK) {
        st = atlas_json_finish(j, err);
    } else {
        atlas_json_free(j);
    }
    if (fclose(mem) != 0 && st == ATLAS_OK) {
        st = atlas_err_set(err, ATLAS_ERR_INTERNAL, "cannot finish a result buffer");
    }
    if (st == ATLAS_OK) {
        while (size > 0 && (buffer[size - 1u] == '\n' || buffer[size - 1u] == '\r')) {
            size--;
        }
        st = atlas_buf_set(out, buffer, size, err);
    }
    free(buffer);
    return st;
}

/* One daemon call, rendered into a tool body. */
atlas_status atlas_mcp_tools_forward(atlas_mcp_server *s, const char *method, const char *params,
                            const char *provenance, bool untrusted, atlas_buf *body,
                            bool *degraded, atlas_err *err) {
    atlas_ipc_response *r = atlas_mcp_call(s, method, params);
    forward_ctx f;
    memset(&f, 0, sizeof(f));
    /* The response itself, which build_body re-emits as `result`.
     *
     * This assignment was missing, so `result` was written from a NULL response
     * and every tool answered with an empty object — including the write tools,
     * whose whole answer is which session the record attached to. It went
     * unnoticed because the envelope around it (`ok`, `degraded`, `provenance`)
     * was built from the context and looked entirely healthy. */
    f.response = r;
    f.provenance = provenance;
    f.untrusted = untrusted;
    if (r == NULL) {
        /* Small, clear and machine-readable. A degraded result is a fact about
         * Atlas, so its provenance is ATLAS_OWNED and it carries no repository
         * data at all. */
        f.degraded = true;
        f.provenance = atlas_provenance_name(ATLAS_PROV_ATLAS_OWNED);
        f.untrusted = false;
        f.message = "the Atlas daemon is not reachable, so this answer is unavailable rather "
                    "than empty. Continue working; Atlas will index the change when it returns.";
        *degraded = true;
    } else if (!atlas_ipc_response_ok(r)) {
        f.degraded = true;
        f.message = atlas_ipc_response_message(r);
        *degraded = true;
    } else {
        bool unbound = false;
        const char *reason = NULL;
        if (atlas_ipc_result_bool(r, "session_unbound", &unbound) && unbound &&
            atlas_ipc_result_str(r, "unbound_reason", &reason)) {
            f.unbound_note = unbound_note(reason);
        }
    }
    atlas_status st = atlas_mcp_tools_render(body, build_body, &f, err);
    atlas_ipc_response_free(r);
    return st;
}

/* Builds an IPC params document. The callback receives a writer positioned
 * inside the object. */


atlas_status atlas_mcp_tools_make_params(params_fn build, void *ud, atlas_buf *out, atlas_err *err) {
    atlas_ipc_params *p = NULL;
    atlas_json *j = NULL;
    atlas_status st = atlas_ipc_params_begin(&p, &j, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = build(j, ud, err);
    if (st == ATLAS_OK) {
        st = atlas_ipc_params_finish(p, out, err);
    } else {
        atlas_ipc_params_abort(p);
    }
    return st;
}

/* --- shared parameter shapes ---------------------------------------------- */



/* The identity every AI method takes.
 *
 * The provider and client names are the same constants the hook adapter sends,
 * and they have to stay that way: a session is keyed on `(provider, client,
 * session_key)`, so if these two ever drift from HOOK_PROVIDER/HOOK_CLIENT the
 * lookup misses silently and every MCP write becomes unattributed. There is a
 * test that opens a session through the hooks and records through MCP, which is
 * what would catch it.
 *
 * The session key is this connection's own, from the environment, sent raw. It
 * is omitted when there is none — never replaced by anything derived from the
 * repository, which is not an identifier for a session. */
atlas_status atlas_mcp_tools_put_identity(atlas_json *j, const atlas_mcp_server *s, const char *repo,
                                 atlas_err *err) {
    atlas_status st = atlas_json_key_str(j, "provider", "anthropic", err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "client", "claude-code", err);
    }
    if (st == ATLAS_OK && s->session_key.len > 0) {
        st = atlas_json_key_str(j, "session_key", atlas_buf_cstr(&s->session_key), err);
    }
    if (st == ATLAS_OK && repo != NULL) {
        st = atlas_json_key_str(j, "repo", repo, err);
    }
    return st;
}

/* `ai.session.get` asks about *this* connection's session, so it sends the same
 * identity a write would. Without it the daemon has a repository and no session
 * key, and the only truthful answer is that there is no session. */


atlas_status atlas_mcp_tools_put_session_args(atlas_json *j, void *ud, atlas_err *err) {
    session_args *a = (session_args *)ud;
    return atlas_mcp_tools_put_identity(j, a->server, a->repo, err);
}

atlas_status atlas_mcp_tools_put_repo_args(atlas_json *j, void *ud, atlas_err *err) {
    repo_args *a = (repo_args *)ud;
    atlas_status st = atlas_json_key_str(j, "repo", a->repo, err);
    if (st == ATLAS_OK && a->path != NULL) {
        st = atlas_json_key_str(j, "path", a->path, err);
    }
    if (st == ATLAS_OK && a->query != NULL) {
        st = atlas_json_key_str(j, "query", a->query, err);
    }
    if (st == ATLAS_OK && a->scope != NULL) {
        st = atlas_json_key_str(j, "scope", a->scope, err);
    }
    if (st == ATLAS_OK && a->limit > 0) {
        st = atlas_json_key_int(j, "limit", a->limit, err);
    }
    if (st == ATLAS_OK && a->cursor > 0) {
        st = atlas_json_key_int(j, "cursor", a->cursor, err);
    }
    return st;
}

/* Resolves the repository and fills the shared argument block. */
atlas_status atlas_mcp_tools_begin_repo_call(atlas_mcp_server *s, const atlas_jsonv *args, repo_args *a,
                                    atlas_buf *repo, atlas_err *err) {
    memset(a, 0, sizeof(*a));
    const char *requested = NULL;
    atlas_status st = atlas_mcp_tools_arg_str(args, "repo", ATLAS_NAME_MAX, &requested, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_mcp_resolve_repo(s, requested, repo, err);
    if (st != ATLAS_OK) {
        return st;
    }
    a->repo = atlas_buf_cstr(repo);
    a->limit = atlas_mcp_tools_arg_int(args, "limit", 0);
    a->cursor = atlas_mcp_tools_arg_int(args, "cursor", 0);
    return ATLAS_OK;
}
