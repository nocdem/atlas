/* Atlas - A12.1 T11: the three operator methods over reconciled memory.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * `memory.put`, `memory.status` and `memory.reconcile`, in the **operator**
 * group beside `repo.scanner` -- `src/ipc/server_decision.c`'s
 * `OPERATOR_METHODS[]` names all three directly, reachable only from the peer
 * the root-owned authority policy names, exactly as `repo.scanner` is.
 * `atlas_server_peer_is_operator` is the whole gate; nothing here re-checks it,
 * and nothing here carries an authority verb -- these three read and append
 * rows a client without operator standing can already reach a version of
 * through other means (a scanner's mirror, `atlas memory scan`'s own read), and
 * confer no authority over the decision lifecycle, the registry or a backup.
 *
 * `memory.put` is the one write here, and it is answered synchronously: the
 * job it queues (`ATLAS_JOB_MEMORY`) is neither unbounded nor undrainable, so
 * a caller waiting the standard `ATLAS_IPC_WRITE_TIMEOUT_MS` is
 * `atlas_writer_apikey`'s shape, not `memory.reconcile`'s. `memory.reconcile`
 * enqueues `ATLAS_JOB_MEMORY_RECONCILE` and answers only that the write was
 * *accepted* -- see that method's own comment for why there is deliberately
 * no waiting form.
 */
#include "atlas/atlas.h"
#include "atlas/db.h"
#include "atlas/ipc.h"
#include "atlas/memory.h"
#include "atlas/safetext.h"
#include "atlas/syspolicy.h"
#include "daemon/daemon_internal.h"
#include "ipc/server_internal.h"

#include <string.h>

/* One hex digit, or -1. Uppercase is refused rather than accepted -- two
 * spellings of one byte would be two wire forms for one file, and a format
 * with a choice in it is a format two implementations will disagree about.
 * `src/ipc/server_scanner.c` carries the identical function for the identical
 * reason; a shared helper would touch a file outside this task's own list, so
 * this is deliberately a third copy of a well-tested twenty lines rather than
 * a refactor nobody asked for. */
static int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    return -1;
}

/* Decodes `hex` into `out`. A source file is arbitrary bytes and a JSON string
 * carries none of them unchanged -- A13's own measurement, a twelve-byte file
 * arriving as twenty-four -- so content travels as hex and this is the
 * daemon's one place to turn it back into what the caller actually read. An
 * odd length or a non-hex digit is refused rather than guessed at. */
static atlas_status hex_decode(const char *hex, atlas_buf *out, atlas_err *err) {
    size_t n = strlen(hex);
    if ((n % 2u) != 0u) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "content must be an even number of hex digits");
    }
    atlas_buf_reset(out);
    /* Fix round Minor 2: a zero-length blob and a NULL blob are different
     * facts to SQLite -- `sqlite3_bind_blob` with a NULL pointer binds NULL
     * regardless of length, and `atlas_buf_reset` alone leaves `out->data`
     * NULL when nothing has ever been appended to it, which is exactly what
     * `n == 0` leaves untouched below. Reserving zero bytes forces a real,
     * non-NULL, zero-length allocation -- what `atlas_buf_append`'s own
     * `n == 0` branch already guarantees for every other caller -- so an
     * emptied external file is stored as an empty blob and satisfies
     * migration 29's `CHECK(blob_oid <> '' OR content IS NOT NULL)`, rather
     * than being read as NULL and refused. */
    if (n == 0u) {
        return atlas_buf_reserve(out, 0, err);
    }
    for (size_t i = 0; i < n; i += 2u) {
        int hi = hex_digit(hex[i]);
        int lo = hex_digit(hex[i + 1u]);
        if (hi < 0 || lo < 0) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "content must be lowercase hex digits");
        }
        char b = (char)((hi << 4) | lo);
        atlas_status st = atlas_buf_append(out, &b, 1u, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    return ATLAS_OK;
}

/* `memory.put`. Params: `source` (a registered source's public uid), `rel_path`
 * (a `*_DIR` child's bare name; absent for a `*_FILE` source), `content`
 * (hex-encoded bytes), `observed_at` (when the bytes described, not when
 * Atlas recorded them).
 *
 * Every refusal that can be decided from the request alone -- a missing
 * field, malformed hex, content over `ATLAS_MEMORY_MAX_SOURCE_BYTES` -- is
 * decided before anything reaches the writer; `atlas_writer_memory_put` itself
 * re-checks the length bound as the authoritative gate, since that is what
 * lets a test drive it directly without going through this parser at all.
 * Everything that depends on the row this uid names -- does it exist, is it
 * `EXTERNAL_*`, does `rel_path` match its class -- can only be decided on the
 * writer's own handle, so it is decided inside `run_memory`
 * (`src/daemon/writer.c`) and surfaced back through `atlas_err`, never
 * duplicated here. */
atlas_status atlas_server_memory_put(dispatch_state *ds, const atlas_ipc_request *req,
                                      atlas_err *err) {
    const char *source_uid = NULL;
    if (!atlas_ipc_param_str(req, "source", &source_uid) || source_uid == NULL ||
        source_uid[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a put needs a source uid");
    }
    const char *rel_path = "";
    (void)atlas_ipc_param_str(req, "rel_path", &rel_path);
    if (rel_path == NULL) {
        rel_path = "";
    }
    const char *content_hex = NULL;
    if (!atlas_ipc_param_str(req, "content", &content_hex) || content_hex == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a put needs its content");
    }
    const char *observed_at = NULL;
    if (!atlas_ipc_param_str(req, "observed_at", &observed_at) || observed_at == NULL ||
        observed_at[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a put needs observed_at");
    }
    /* Fix round Minor 4: invariant 7 says every parser is bounded, and this was
     * the one caller-supplied string on this path with no ceiling at all --
     * `content` is bounded at the writer (`ATLAS_MEMORY_MAX_SOURCE_BYTES`) and
     * `source`/`rel_path` are register-row lengths the writer resolves
     * against. This one lands durably in `memory_source_versions.observed_at`
     * and is folded into the reconciliation pass's own evidence content key
     * (`src/memory/reconcile.c:1169`), so the transport frame
     * (`ATLAS_IPC_MAX_REQUEST_BYTES`) was its only ceiling before this. Decided
     * from the request alone, before anything reaches the writer, and refused
     * rather than truncated -- the MCP verify path's own `observed_at`
     * (`src/mcp/mcp_tools.c:3151`) is bounded at the same named constant for
     * the identical reason: an operator-supplied stamp, not file content. */
    if (strlen(observed_at) > ATLAS_VERIFY_NAME_MAX) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "observed_at is longer than %u bytes; it was refused rather than "
                             "truncated",
                             ATLAS_VERIFY_NAME_MAX);
    }

    if (ds->ctx == NULL || ds->ctx->writer == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no writer to run a memory put");
    }

    atlas_memory_put_op op;
    atlas_memory_put_op_init(&op);
    atlas_status st = atlas_buf_append_str(&op.source_uid, source_uid, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_append_str(&op.rel_path, rel_path, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_append_str(&op.observed_at, observed_at, err);
    }
    if (st == ATLAS_OK) {
        /* Refused here before anything is queued when the bound is exceeded --
         * `hex_decode` itself has no length ceiling, so the check that matters
         * is `atlas_writer_memory_put`'s, immediately below. */
        st = hex_decode(content_hex, &op.content, err);
    }
    /* SO_PEERCRED, not a request field -- A7.1's rule that a client describing
     * itself is not evidence about itself, applied to who read these bytes. */
    op.peer_uid = ds->peer_uid;
    if (st != ATLAS_OK) {
        atlas_memory_put_op_free(&op);
        return st;
    }

    atlas_memory_put_result res;
    atlas_memory_put_result_init(&res);
    st = atlas_writer_memory_put(ds->ctx->writer, &op, &res, err);
    atlas_memory_put_op_free(&op);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "version", atlas_buf_cstr(&res.version_uid), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "content_sha256", atlas_buf_cstr(&res.content_sha256), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "content_bytes", res.content_bytes, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "created", res.created, err);
    }
    atlas_memory_put_result_free(&res);
    return st;
}

typedef struct status_ctx {
    dispatch_state *ds;
    atlas_status status;
} status_ctx;

/* One `sources[]` entry: the row plus its own latest version, read fresh on
 * every call -- A9.2.3's "nothing is cached" rule, one layer over. A source
 * with no version yet (registered but never `memory.put`, or a `REPO_*`
 * source no pass has reconciled yet) reports `latest_version: null` rather
 * than omitting the key, so a caller need not tell "never read" apart from "a
 * field Atlas forgot to send". */
static atlas_status emit_source(int64_t id, const char *source_uid, atlas_memory_source_class cls,
                                const char *path_text, const char *registered_at, void *ud,
                                atlas_err *err) {
    status_ctx *c = (status_ctx *)ud;
    if (c->status != ATLAS_OK) {
        return ATLAS_OK;
    }
    dispatch_state *ds = c->ds;
    atlas_status st = atlas_json_obj_begin(ds->j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "uid", source_uid, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "class", atlas_memory_source_class_name(cls), err);
    }
    if (st == ATLAS_OK) {
        /* Already the lossless %XX form the row stores -- printed as-is,
         * never re-encoded. */
        st = atlas_json_key_str(ds->j, "path", path_text, err);
    }
    if (st == ATLAS_OK) {
        /* `registered_at` is Atlas' own stamp (`atlas_now_iso8601` at
         * materialisation), not request text, but it is still raw column
         * text rather than a pre-encoded display form, so it goes through
         * `atlas_safe()` like every raw value on this path. */
        st = atlas_json_key_str(ds->j, "registered_at", atlas_safe(&ds->safe, registered_at), err);
    }
    atlas_memory_version_row row;
    atlas_memory_version_row_init(&row);
    bool found = false;
    if (st == ATLAS_OK) {
        /* Fix round Important 1: the metadata-only projection, so a poll of
         * every registered source's latest version never reads and discards
         * up to `ATLAS_MEMORY_MAX_SOURCE_BYTES` of content per source.
         *
         * Fix round Important 2: `err` is the same `atlas_err` the caller
         * (`atlas_server_memory_status`) will report, threaded straight
         * through rather than caught in a local and swallowed -- a failed
         * read is `st != ATLAS_OK`, propagated below, and is never folded
         * into `found = false`. The comment at this function's own header
         * defines `latest_version: null` as "never put, or not yet
         * reconciled" -- a database read that failed is neither, and must
         * not answer the same as either. */
        st = atlas_db_memory_version_latest_meta(ds->db, id, &row, &found, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "latest_version", err);
    }
    if (st == ATLAS_OK && found) {
        st = atlas_json_obj_begin(ds->j, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "uid", atlas_buf_cstr(&row.version_uid), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "content_sha256", atlas_buf_cstr(&row.content_sha256),
                                    err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "content_bytes", row.content_bytes, err);
        }
        if (st == ATLAS_OK) {
            /* Caller-supplied text (`memory.put`'s own `observed_at`, or a
             * reconciliation pass' `atlas_now_iso8601` for a REPO_* source),
             * so it is safe-encoded like every other raw value here even
             * though today's producers only ever write plain timestamps. */
            st = atlas_json_key_str(ds->j, "observed_at",
                                    atlas_safe(&ds->safe, atlas_buf_cstr(&row.observed_at)), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str_opt(
                ds->j, "commit_oid",
                row.commit_oid.len > 0 ? atlas_buf_cstr(&row.commit_oid) : NULL, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_obj_end(ds->j, err);
        }
    } else if (st == ATLAS_OK) {
        st = atlas_json_null(ds->j, err);
    }
    atlas_memory_version_row_free(&row);
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(ds->j, err);
    }
    c->status = st;
    /* The list read keeps walking regardless: `c->status` is what the caller
     * checks once the whole array is closed, so one row's failure is reported
     * without leaving the JSON document half-written. */
    return ATLAS_OK;
}

/* `memory.status`. Params: `repo` (a registered repository's name).
 *
 * Reports per-source latest versions, the repository's latest generation (if
 * any) and `atlas_memory_plan_for`'s own answer -- everything T11 has to show;
 * the Canonical Context Pack's own presence (T12/T13) is not reported because
 * nothing freezes one yet, and adding a key that always reads empty would be
 * a promise this season has not kept.
 *
 * `plan_for` needs a policy, loaded here on this call's own stack via
 * `atlas_syspolicy_load` -- the same function `memory_sweep` calls, for the
 * same reason `memory.reconcile` loads its own below: a read that depended on
 * a policy a request could shape would not be describing the machine's own
 * configuration any more. */
atlas_status atlas_server_memory_status(dispatch_state *ds, const atlas_ipc_request *req,
                                         atlas_err *err) {
    const char *name = NULL;
    if (!atlas_ipc_param_str(req, "repo", &name) || name == NULL || name[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a memory status needs a repository");
    }
    atlas_repo_info ri;
    atlas_repo_info_init(&ri);
    bool found = false;
    atlas_status st = atlas_db_repo_get(ds->db, name, &ri, &found, err);
    if (st == ATLAS_OK && !found) {
        st = atlas_err_set(err, ATLAS_ERR_REPO, "no repository has that name");
    }
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&ri);
        return st;
    }

    atlas_syspolicy pol;
    atlas_syspolicy_load(&pol);
    atlas_memory_gen_cause cause = ATLAS_MEMORY_CAUSE_UNKNOWN;
    atlas_err perr;
    atlas_err_init(&perr);
    /* A read the answer of which cannot be worse than UNKNOWN: a policy that
     * failed to load answers LEGACY, and `atlas_memory_plan_for` (through
     * `atlas_syspolicy_memory_source_count_checked`) reads that as no source
     * registered, never as a reason to fail this whole status read. */
    (void)atlas_memory_plan_for(ds->db, &ri, &pol, &cause, &perr);

    st = atlas_json_key_str(ds->j, "repo", atlas_safe(&ds->safe, ri.name), err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "plan_for", atlas_memory_gen_cause_name(cause), err);
    }

    int64_t generation = 0;
    atlas_buf head_commit = ATLAS_BUF_INIT;
    atlas_buf decision_digest = ATLAS_BUF_INIT;
    atlas_buf source_digest = ATLAS_BUF_INIT;
    bool gen_found = false;
    if (st == ATLAS_OK) {
        st = atlas_db_memory_generation_latest(ds->db, ri.id, &generation, &head_commit,
                                               &decision_digest, &source_digest, &gen_found, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "generation", gen_found ? generation : 0, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(
            ds->j, "head_commit",
            gen_found && head_commit.len > 0 ? atlas_buf_cstr(&head_commit) : NULL, err);
    }
    atlas_buf_free(&head_commit);
    atlas_buf_free(&decision_digest);
    atlas_buf_free(&source_digest);

    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "sources", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    if (st == ATLAS_OK) {
        status_ctx c;
        c.ds = ds;
        c.status = ATLAS_OK;
        st = atlas_db_memory_source_list(ds->db, ri.id, emit_source, &c, err);
        if (st == ATLAS_OK) {
            st = c.status;
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    atlas_repo_info_free(&ri);
    return st;
}

/* `memory.reconcile`. Params: `repo` (a registered repository's name).
 *
 * Enqueues one `ATLAS_JOB_MEMORY_RECONCILE` and answers that the write was
 * *accepted*, never that it completed -- A8-CI's rule, restated at A13's own
 * write point (`scanner.state`) and true here for the identical measured
 * reason: T8's own worst-case pass is 2429.9 ms at the compiled ceiling, which
 * outlasts a caller with an ordinary deadline, and answering only on
 * completion would let a caller time out reading a response for a write that
 * had, in fact, already landed. `memory.status` is the confirmation channel a
 * caller polls afterward, which is what it is built to answer in the first
 * place. **There is deliberately no waiting form** -- adding one would need a
 * bound on how long the writer may be busy, and Atlas has none.
 *
 * The policy is loaded here, on this call's own stack, via
 * `atlas_syspolicy_load` -- exactly what `memory_sweep` does at
 * `src/daemon/watch.c`, and for the same reason:
 * `atlas_writer_submit_memory_reconcile`'s own contract is that `pol` is a
 * policy *this process loaded*, and provenance cannot be established at the
 * callee. No request field -- no path, no override, no "use this profile"
 * argument -- may reach it: A7's rule that authority is configured outside the
 * reach of the principal it constrains, and a policy arriving in a request
 * body is inside it.
 *
 * The `memory_reconcile` policy key -- whether Atlas starts a pass nobody
 * asked for -- is deliberately **not** consulted here. It governs initiative;
 * an operator asking explicitly is a different question, and letting that key
 * veto a request made on purpose would answer a question nobody asked it. */
atlas_status atlas_server_memory_reconcile(dispatch_state *ds, const atlas_ipc_request *req,
                                            atlas_err *err) {
    const char *name = NULL;
    if (!atlas_ipc_param_str(req, "repo", &name) || name == NULL || name[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a memory reconciliation needs a repository");
    }
    atlas_repo_info ri;
    atlas_repo_info_init(&ri);
    bool found = false;
    atlas_status st = atlas_db_repo_get(ds->db, name, &ri, &found, err);
    if (st == ATLAS_OK && !found) {
        st = atlas_err_set(err, ATLAS_ERR_REPO, "no repository has that name");
    }
    int64_t repo_id = ri.id;
    atlas_repo_info_free(&ri);
    if (st != ATLAS_OK) {
        return st;
    }
    if (ds->ctx == NULL || ds->ctx->writer == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no writer to run a memory reconciliation");
    }

    atlas_syspolicy pol;
    atlas_syspolicy_load(&pol);

    st = atlas_writer_submit_memory_reconcile(ds->ctx->writer, repo_id, &pol, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "accepted", true, err);
    }
    return st;
}
