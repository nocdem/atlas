/* Atlas - A12.1 T16: the `memory` command family.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Seven forms, one render struct (`atlas_memory_render`, `include/atlas/
 * service.h`) and one sink -- `job_item`'s philosophy carried across a whole
 * command family instead of two shapes of one command.
 *
 * `status`, `scan` and `reconcile` reach the daemon's existing T11 operator
 * methods (`memory.status`, `memory.put`, `memory.reconcile`). `scan` and
 * `reconcile` do so unconditionally: `atlas_writer_memory_put` and
 * `atlas_writer_submit_memory_reconcile` (`src/daemon/writer.c`) are bound to
 * the daemon's own job queue and have no local equivalent to fall back to --
 * the identical fact that gives A11.1's run driver no offline path. `status`
 * reads locally through `atlas_ctx_db(ctx)` when it can and falls back to the
 * same `memory.status` method otherwise, `sem_status`'s own shape.
 *
 * `pack`, `diff`, `patch` and `trailer` read already-materialised rows
 * through `atlas_ctx_db(ctx)` and have **no remote form in this build**.
 * `ctx == NULL` (an A7.1 deployment where the index is 0700 `atlasd`) is
 * refused with a stated reason rather than dereferencing a NULL handle. This
 * is a disclosed gap: `docs/backlog.md` and the T16 report carry the
 * reasoning for why it is not closed in this task.
 */
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/ipc.h"
#include "atlas/memory.h"
#include "atlas/pathrep.h"
#include "atlas/safetext.h"
#include "atlas/service.h"
#include "atlas/syspolicy.h"
#include "service_internal.h"

/* Every local-only form's refusal when `ctx` is NULL, worded once so the
 * daemon-owned-index sentence stays one sentence rather than four slightly
 * different ones. */
#define MEMORY_NO_LOCAL_HANDLE                                                                   \
    "this account has no local database handle for this repository (likely an A7.1 system "      \
    "deployment, where the index is owned by the Atlas service account). `memory pack`, "         \
    "`memory diff`, `memory patch` and `memory trailer` have no remote form in this build; "      \
    "run them as the account that owns the index, or see docs/backlog.md"

static const char *syspolicy_state_name(atlas_syspolicy_state s) {
    return s == ATLAS_SYSPOLICY_SYSTEM ? "SYSTEM" : "LEGACY";
}

/* One netstring decoder, private to this file -- the third copy of the shape
 * `src/db/db_memory.c`'s own `reliance_decode_into`/`reliance_ns_take` are the
 * second: a small, closed-form codec used by one layer, not a cross-layer
 * dependency worth exporting. `<count>:` then that many `<len>:bytes,`
 * records (M23's shape), decoded into a comma-joined display string. Every
 * element this format ever carries -- a claim uid, an anchor value, a
 * trailer field name -- is an Atlas-minted token or an already-`path_text`-
 * encoded value, so the joined result needs no further `atlas_safe()` pass. */
static atlas_status netstring_join(const char *text, atlas_buf *out, atlas_err *err) {
    atlas_buf_reset(out);
    size_t total = text != NULL ? strlen(text) : 0u;
    if (total == 0) {
        return ATLAS_OK;
    }
    size_t pos = 0, n = 0, digits = 0;
    while (pos < total && text[pos] >= '0' && text[pos] <= '9') {
        if (digits > 6) {
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "malformed netstring count");
        }
        n = n * 10u + (size_t)(text[pos] - '0');
        pos++;
        digits++;
    }
    if (digits == 0 || pos >= total || text[pos] != ':') {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "malformed netstring count");
    }
    pos++;
    for (size_t i = 0; i < n; i++) {
        size_t elen = 0, edigits = 0;
        while (pos < total && text[pos] >= '0' && text[pos] <= '9') {
            if (edigits > 9) {
                return atlas_err_set(err, ATLAS_ERR_INTERNAL, "malformed netstring element");
            }
            elen = elen * 10u + (size_t)(text[pos] - '0');
            pos++;
            edigits++;
        }
        if (edigits == 0 || pos >= total || text[pos] != ':') {
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "malformed netstring element");
        }
        pos++;
        if (elen > total - pos) {
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "malformed netstring element length");
        }
        atlas_status st = i > 0 ? atlas_buf_append_str(out, ", ", err) : ATLAS_OK;
        if (st == ATLAS_OK) {
            st = atlas_buf_append(out, text + pos, elen, err);
        }
        if (st != ATLAS_OK) {
            return st;
        }
        pos += elen;
        if (pos >= total || text[pos] != ',') {
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "malformed netstring terminator");
        }
        pos++;
    }
    return ATLAS_OK;
}

/* --- status: local read ------------------------------------------------- */

typedef struct src_row {
    atlas_buf uid, path, registered_at;
    const char *cls;
    bool has_version;
    atlas_memory_version_row ver;
} src_row;

static void src_row_init(src_row *r) {
    atlas_buf_init(&r->uid);
    atlas_buf_init(&r->path);
    atlas_buf_init(&r->registered_at);
    r->cls = "UNKNOWN";
    r->has_version = false;
    atlas_memory_version_row_init(&r->ver);
}

static void src_row_free(src_row *r) {
    atlas_buf_free(&r->uid);
    atlas_buf_free(&r->path);
    atlas_buf_free(&r->registered_at);
    atlas_memory_version_row_free(&r->ver);
}

typedef struct src_collect_ctx {
    atlas_db *db;
    src_row rows[ATLAS_MEMORY_MAX_SOURCES];
    size_t count;
    size_t total; /* every row the callback saw, whether or not it was kept --
                    * `ATLAS_MEMORY_MAX_SOURCES` bounds a policy's source list,
                    * not a repository's `memory_sources` rows, which are never
                    * deleted; `total > count` is a real truncation and must be
                    * reported rather than silently dropped. */
    atlas_status status;
} src_collect_ctx;

static atlas_status collect_source(int64_t id, const char *source_uid, atlas_memory_source_class cls,
                                   const char *path_text, const char *registered_at, void *ud,
                                   atlas_err *err) {
    src_collect_ctx *c = (src_collect_ctx *)ud;
    if (c->status != ATLAS_OK) {
        return ATLAS_OK;
    }
    c->total++;
    if (c->count >= ATLAS_MEMORY_MAX_SOURCES) {
        return ATLAS_OK;
    }
    src_row *row = &c->rows[c->count];
    src_row_init(row);
    atlas_status st = atlas_buf_set_str(&row->uid, source_uid != NULL ? source_uid : "", err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&row->path, path_text != NULL ? path_text : "", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&row->registered_at, registered_at != NULL ? registered_at : "", err);
    }
    row->cls = atlas_memory_source_class_name(cls);
    if (st == ATLAS_OK) {
        bool found = false;
        st = atlas_db_memory_version_latest_meta(c->db, id, &row->ver, &found, err);
        row->has_version = found;
    }
    if (st != ATLAS_OK) {
        src_row_free(row);
        c->status = st;
        return ATLAS_OK;
    }
    c->count++;
    return ATLAS_OK;
}

atlas_status atlas_service_memory_status(atlas_ctx *ctx, const char *repo, atlas_memory_sink sink,
                                         void *ud, atlas_err *err) {
    if (repo == NULL || repo[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "which repository?");
    }
    if (ctx == NULL) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG, "%s", MEMORY_NO_LOCAL_HANDLE);
    }

    /* Loaded first and unconditionally: a root-owned file read, never a
     * database read, so it is reported whether or not anything below it
     * succeeds -- `gateway status`'s own precedent (context §5). */
    atlas_syspolicy pol;
    atlas_syspolicy_load(&pol);

    atlas_repo_info ri;
    atlas_repo_info_init(&ri);
    atlas_status st = atlas_service_require_repo(ctx, repo, &ri, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&ri);
        return st;
    }

    atlas_memory_gen_cause cause = ATLAS_MEMORY_CAUSE_UNKNOWN;
    atlas_err perr;
    atlas_err_init(&perr);
    (void)atlas_memory_plan_for(atlas_ctx_db(ctx), &ri, &pol, &cause, &perr);

    int64_t generation = 0;
    atlas_buf head_commit = ATLAS_BUF_INIT, dset = ATLAS_BUF_INIT, sset = ATLAS_BUF_INIT;
    bool gen_found = false;
    st = atlas_db_memory_generation_latest(atlas_ctx_db(ctx), ri.id, &generation, &head_commit,
                                           &dset, &sset, &gen_found, err);

    src_collect_ctx sc;
    memset(&sc, 0, sizeof sc);
    sc.db = atlas_ctx_db(ctx);
    sc.status = ATLAS_OK;
    if (st == ATLAS_OK) {
        st = atlas_db_memory_source_list(atlas_ctx_db(ctx), ri.id, collect_source, &sc, err);
    }
    if (st == ATLAS_OK) {
        st = sc.status;
    }

    if (st == ATLAS_OK) {
        atlas_memory_render mr;
        memset(&mr, 0, sizeof mr);
        mr.form = "status";
        mr.detail = true;
        mr.repo = ri.name;
        mr.policy_state = syspolicy_state_name(pol.state);
        mr.policy_reason = atlas_syspolicy_reason_name(pol.reason);
        mr.policy_reason_detail = atlas_syspolicy_reason_explain(pol.reason);
        mr.policy_path = ATLAS_SYSPOLICY_PATH;
        mr.plan_for = atlas_memory_gen_cause_name(cause);
        mr.generation = gen_found ? generation : 0;
        mr.generation_found = gen_found;
        mr.head_commit = gen_found && head_commit.len > 0 ? atlas_buf_cstr(&head_commit) : NULL;
        mr.sources_truncated = sc.total > sc.count;
        for (size_t i = 0; i < sc.count; i++) {
            atlas_memory_source_render *s = &mr.sources[mr.source_count];
            s->uid = atlas_buf_cstr(&sc.rows[i].uid);
            s->cls = sc.rows[i].cls;
            s->path = atlas_buf_cstr(&sc.rows[i].path);
            s->registered_at = atlas_buf_cstr(&sc.rows[i].registered_at);
            s->has_version = sc.rows[i].has_version;
            if (sc.rows[i].has_version) {
                s->version_uid = atlas_buf_cstr(&sc.rows[i].ver.version_uid);
                s->content_sha256 = atlas_buf_cstr(&sc.rows[i].ver.content_sha256);
                s->content_bytes = sc.rows[i].ver.content_bytes;
                s->observed_at = atlas_buf_cstr(&sc.rows[i].ver.observed_at);
                s->commit_oid = sc.rows[i].ver.commit_oid.len > 0
                                    ? atlas_buf_cstr(&sc.rows[i].ver.commit_oid)
                                    : NULL;
            }
            mr.source_count++;
        }
        st = sink(&mr, ud, err);
    }

    for (size_t i = 0; i < sc.count; i++) {
        src_row_free(&sc.rows[i]);
    }
    atlas_buf_free(&head_commit);
    atlas_buf_free(&dset);
    atlas_buf_free(&sset);
    atlas_repo_info_free(&ri);
    return st;
}

/* --- status: remote (memory.status RPC) --------------------------------- */

/* `ud` is the address of a local `const char *`, `build_job`'s own idiom
 * (`src/core/service_orch.c`) -- passing the address avoids casting away the
 * repository name's constness to satisfy `atlas_service_build_fn`'s plain
 * `void *`. */
static atlas_status build_repo_req(atlas_json *j, void *ud, atlas_err *err) {
    return atlas_json_key_str(j, "repo", *(const char *const *)ud, err);
}

atlas_status atlas_service_memory_status_remote(const char *repo, atlas_memory_sink sink, void *ud,
                                                atlas_err *err) {
    if (repo == NULL || repo[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "which repository?");
    }
    atlas_syspolicy pol;
    atlas_syspolicy_load(&pol);

    const char *repo_copy = repo;
    atlas_ipc_response *resp = NULL;
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_status st = atlas_service_orch_call(NULL, "memory.status", build_repo_req, &repo_copy,
                                              &resp, &raw, err);
    if (st != ATLAS_OK) {
        atlas_ipc_response_free(resp);
        atlas_buf_free(&raw);
        return st;
    }

    atlas_memory_render mr;
    memset(&mr, 0, sizeof mr);
    mr.form = "status";
    mr.detail = true;
    mr.policy_state = syspolicy_state_name(pol.state);
    mr.policy_reason = atlas_syspolicy_reason_name(pol.reason);
    mr.policy_reason_detail = atlas_syspolicy_reason_explain(pol.reason);
    mr.policy_path = ATLAS_SYSPOLICY_PATH;
    const char *v = NULL;
    if (atlas_ipc_result_str(resp, "repo", &v)) {
        mr.repo = v;
    }
    if (atlas_ipc_result_str(resp, "plan_for", &v)) {
        mr.plan_for = v;
    }
    int64_t gen = 0;
    if (atlas_ipc_result_int(resp, "generation", &gen) && gen > 0) {
        mr.generation = gen;
        mr.generation_found = true;
    }
    if (atlas_ipc_result_str(resp, "head_commit", &v)) {
        mr.head_commit = v;
    }
    size_t n = 0;
    (void)atlas_ipc_result_arr_len(resp, "sources", &n);
    if (n > ATLAS_MEMORY_MAX_SOURCES) {
        mr.sources_truncated = true;
        n = ATLAS_MEMORY_MAX_SOURCES; /* a rendering ceiling this client applies
                                        * to how many rows it displays -- the
                                        * daemon sent every row in
                                        * `memory_sources`, which are never
                                        * deleted and can outnumber a policy's
                                        * declared source list over time. */
    }
    /* The nested `latest_version` object per source is not read here: the
     * response accessor API reads one level into a result object or one
     * member of an array element, and this is an object nested a level
     * deeper than either reaches. A disclosed asymmetry with the local read
     * above, not a silent omission -- see the T16 report.
     *
     * Whoever closes this: `observed_at` is `atlas_safe()`-encoded at the
     * same call site as `registered_at` below (`emit_source`,
     * `src/ipc/server_memory.c:268`), so reading `latest_version.observed_at`
     * here will reproduce the identical double-encode this fix round closed
     * for `registered_at` unless it gets the same
     * `atlas_text_decode_safe()` treatment.
     *
     * **The decode is total only because there is one producer.**
     * `atlas_text_decode_safe` refuses a `%` not followed by two hex
     * digits, and silently turns a never-encoded `%41` into `A` -- so a
     * value that reached this field *unencoded* would be corrupted or
     * rejected rather than passed through. Neither happens today because
     * `emit_source` is the sole producer and always encodes, and even its
     * out-of-memory placeholder (`%3F`) is a valid escape. That is a
     * property of the current wire, not of this code: **a second producer
     * of this field must encode it too**, or must arrive here without the
     * decode. Stated because the fix above is correct only for as long as
     * that stays true, and nothing else says so. */
    /* `registered_at` crosses the socket already `atlas_safe()`-encoded
     * (`emit_source`, `src/ipc/server_memory.c`) -- caller-supplied text, not
     * an Atlas-minted token. `atlas_memory_render` documents the field as RAW
     * (`service.h`) and both renderers apply `atlas_safe()`/`json_safe()` to
     * it at print (`render_human.c`, `render_json.c`), which is right for the
     * local read and would double-encode this one. Decoding it back to raw
     * here, once, keeps the field in the single category the struct and both
     * renderers already agree on, on both transports -- rather than
     * inventing a second, per-source "already safe" category that only the
     * remote path would ever set. */
    atlas_buf registered_at_buf[ATLAS_MEMORY_MAX_SOURCES];
    for (size_t i = 0; i < ATLAS_MEMORY_MAX_SOURCES; i++) {
        atlas_buf_init(&registered_at_buf[i]);
    }
    for (size_t i = 0; i < n && st == ATLAS_OK; i++) {
        atlas_memory_source_render *s = &mr.sources[mr.source_count];
        const char *sv = NULL;
        if (atlas_ipc_result_arr_obj_str(resp, "sources", i, "uid", &sv)) {
            s->uid = sv;
        }
        if (atlas_ipc_result_arr_obj_str(resp, "sources", i, "class", &sv)) {
            s->cls = sv;
        }
        if (atlas_ipc_result_arr_obj_str(resp, "sources", i, "path", &sv)) {
            s->path = sv;
        }
        if (atlas_ipc_result_arr_obj_str(resp, "sources", i, "registered_at", &sv)) {
            st = atlas_text_decode_safe(sv, strlen(sv), &registered_at_buf[i], err);
            if (st == ATLAS_OK) {
                s->registered_at = atlas_buf_cstr(&registered_at_buf[i]);
            }
        }
        mr.source_count++;
    }
    if (st == ATLAS_OK) {
        st = sink(&mr, ud, err);
    }
    for (size_t i = 0; i < ATLAS_MEMORY_MAX_SOURCES; i++) {
        atlas_buf_free(&registered_at_buf[i]);
    }
    atlas_ipc_response_free(resp);
    atlas_buf_free(&raw);
    return st;
}

/* --- scan: always daemon-served ------------------------------------------ */

typedef struct put_req_ctx {
    const char *source_uid;
    const char *rel_path;     /* raw bytes, as read from the directory */
    const char *content_hex;
    const char *observed_at;
} put_req_ctx;

static atlas_status build_put_req(atlas_json *j, void *ud, atlas_err *err) {
    const put_req_ctx *p = (const put_req_ctx *)ud;
    atlas_status st = atlas_json_key_str(j, "source", p->source_uid, err);
    if (st == ATLAS_OK && p->rel_path != NULL && p->rel_path[0] != '\0') {
        st = atlas_json_key_str(j, "rel_path", p->rel_path, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "content", p->content_hex, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "observed_at", p->observed_at, err);
    }
    return st;
}

atlas_status atlas_service_memory_scan(const char *repo, atlas_memory_sink sink, void *ud,
                                       int64_t *count_out, atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    if (repo == NULL || repo[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "which repository?");
    }

    const char *repo_copy = repo;
    atlas_ipc_response *resp = NULL;
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_status st = atlas_service_orch_call(NULL, "memory.status", build_repo_req, &repo_copy,
                                              &resp, &raw, err);
    if (st != ATLAS_OK) {
        atlas_ipc_response_free(resp);
        atlas_buf_free(&raw);
        return st;
    }

    size_t n = 0;
    (void)atlas_ipc_result_arr_len(resp, "sources", &n);
    bool any_external = false;
    for (size_t i = 0; i < n && st == ATLAS_OK; i++) {
        const char *cls_name = NULL, *uid = NULL, *path_text = NULL;
        if (!atlas_ipc_result_arr_obj_str(resp, "sources", i, "class", &cls_name) ||
            !atlas_ipc_result_arr_obj_str(resp, "sources", i, "uid", &uid) ||
            !atlas_ipc_result_arr_obj_str(resp, "sources", i, "path", &path_text)) {
            continue;
        }
        atlas_memory_source_class cls = ATLAS_MEMORY_SOURCE_UNKNOWN;
        if (!atlas_memory_source_class_parse(cls_name, &cls) || atlas_memory_source_class_is_repo(cls)) {
            continue; /* REPO_*: the daemon reads it itself; not this command's job */
        }
        any_external = true;
        bool is_dir = cls == ATLAS_MEMORY_SOURCE_EXTERNAL_DIR;

        atlas_buf path_raw = ATLAS_BUF_INIT;
        st = atlas_path_text_decode(path_text, strlen(path_text), &path_raw, err);
        if (st != ATLAS_OK) {
            atlas_buf_free(&path_raw);
            break;
        }

        size_t cap = is_dir ? ATLAS_MEMORY_MAX_DIR_ENTRIES : 1u;
        atlas_memory_read_item *items = calloc(cap, sizeof(*items));
        if (items == NULL) {
            atlas_buf_free(&path_raw);
            st = atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory reading a memory source");
            break;
        }
        for (size_t k = 0; k < cap; k++) {
            atlas_memory_read_item_init(&items[k]);
        }
        size_t count = 0;
        bool from_mirror = false;
        /* T6's read, as the invoking principal -- A13's rule that reading
         * bytes this process can already read is a capability question, not
         * an authority one; nothing here treats it as granting anything. */
        st = atlas_memory_read_external(path_raw.data, path_raw.len, is_dir, items, cap, &count,
                                        &from_mirror, err);
        atlas_buf_free(&path_raw);
        if (st != ATLAS_OK) {
            for (size_t k = 0; k < cap; k++) {
                atlas_memory_read_item_free(&items[k]);
            }
            free(items);
            break;
        }

        for (size_t k = 0; k < count && st == ATLAS_OK; k++) {
            atlas_memory_render mr;
            memset(&mr, 0, sizeof mr);
            mr.form = "scan";
            mr.in_list = true;
            mr.repo = repo;
            mr.scan_source_uid = uid;

            atlas_buf rel_enc = ATLAS_BUF_INIT;
            if (items[k].rel_path.len > 0) {
                atlas_err rerr;
                atlas_err_init(&rerr);
                (void)atlas_path_text_encode(items[k].rel_path.data, items[k].rel_path.len, &rel_enc,
                                             &rerr);
            }
            mr.scan_rel_path = atlas_buf_cstr(&rel_enc);

            atlas_buf hex = ATLAS_BUF_INIT;
            if (items[k].outcome == ATLAS_MEMORY_READ_OK) {
                char now[ATLAS_TS_MAX];
                atlas_now_iso8601(now, sizeof now);
                size_t need = items[k].bytes.len * 2u;
                st = atlas_buf_reserve(&hex, need + 1u, err);
                if (st == ATLAS_OK) {
                    atlas_hex_encode_lower(
                        (const unsigned char *)(items[k].bytes.data != NULL ? items[k].bytes.data
                                                                            : ""),
                        items[k].bytes.len, hex.data);
                    hex.len = need;

                    put_req_ctx preq;
                    preq.source_uid = uid;
                    preq.rel_path = atlas_buf_cstr(&items[k].rel_path);
                    preq.content_hex = atlas_buf_cstr(&hex);
                    preq.observed_at = now;
                    atlas_ipc_response *presp = NULL;
                    atlas_buf praw = ATLAS_BUF_INIT;
                    atlas_status pst = atlas_service_orch_call(NULL, "memory.put", build_put_req,
                                                               &preq, &presp, &praw, err);
                    if (pst == ATLAS_OK) {
                        mr.scan_put = true;
                        const char *pv = NULL;
                        if (atlas_ipc_result_str(presp, "version", &pv)) {
                            mr.scan_version_uid = pv;
                        }
                        if (atlas_ipc_result_str(presp, "content_sha256", &pv)) {
                            mr.scan_content_sha256 = pv;
                        }
                        (void)atlas_ipc_result_int(presp, "content_bytes", &mr.scan_content_bytes);
                        (void)atlas_ipc_result_bool(presp, "created", &mr.scan_created);
                    } else {
                        st = pst;
                    }
                    if (st == ATLAS_OK) {
                        st = sink(&mr, ud, err);
                        if (st == ATLAS_OK && count_out != NULL) {
                            (*count_out)++;
                        }
                    }
                    atlas_ipc_response_free(presp);
                    atlas_buf_free(&praw);
                }
            } else {
                /* Not read: reported as a finding, never as a silent skip.
                 * The outcome name is one of a fixed set of Atlas literals. */
                switch (items[k].outcome) {
                case ATLAS_MEMORY_READ_OK:
                    /* Unreachable in this branch (the `if` above tests
                     * exactly this case) but the switch is over the whole
                     * vocabulary, `-Wswitch-enum`'s own requirement -- a
                     * member added later must fail this build rather than
                     * fall through silently. */
                    mr.scan_outcome = "OK";
                    break;
                case ATLAS_MEMORY_READ_UNKNOWN:
                    mr.scan_outcome = "UNKNOWN";
                    break;
                case ATLAS_MEMORY_READ_ABSENT:
                    mr.scan_outcome = "ABSENT";
                    break;
                case ATLAS_MEMORY_READ_TOO_LARGE:
                    mr.scan_outcome = "TOO_LARGE";
                    break;
                case ATLAS_MEMORY_READ_NOT_OURS:
                    mr.scan_outcome = "NOT_OURS";
                    break;
                case ATLAS_MEMORY_READ_NO_MIRROR:
                    mr.scan_outcome = "NO_MIRROR";
                    break;
                case ATLAS_MEMORY_READ_SYMLINK:
                    mr.scan_outcome = "SYMLINK";
                    break;
                case ATLAS_MEMORY_READ_NOT_MIRRORED:
                    mr.scan_outcome = "NOT_MIRRORED";
                    break;
                default:
                    mr.scan_outcome = "UNKNOWN";
                    break;
                }
                st = sink(&mr, ud, err);
            }
            atlas_buf_free(&hex);
            atlas_buf_free(&rel_enc);
        }
        for (size_t k = 0; k < cap; k++) {
            atlas_memory_read_item_free(&items[k]);
        }
        free(items);
    }

    if (st == ATLAS_OK && !any_external) {
        atlas_memory_render mr;
        memset(&mr, 0, sizeof mr);
        mr.form = "scan";
        mr.repo = repo;
        mr.scan_no_sources = true;
        st = sink(&mr, ud, err);
    }

    atlas_ipc_response_free(resp);
    atlas_buf_free(&raw);
    return st;
}

/* --- reconcile: always daemon-served ------------------------------------- */

atlas_status atlas_service_memory_reconcile(const char *repo, atlas_memory_sink sink, void *ud,
                                            atlas_err *err) {
    if (repo == NULL || repo[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "which repository?");
    }
    const char *repo_copy = repo;
    atlas_ipc_response *resp = NULL;
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_status st = atlas_service_orch_call(NULL, "memory.reconcile", build_repo_req, &repo_copy,
                                              &resp, &raw, err);
    if (st == ATLAS_OK) {
        atlas_memory_render mr;
        memset(&mr, 0, sizeof mr);
        mr.form = "reconcile";
        mr.repo = repo;
        (void)atlas_ipc_result_bool(resp, "accepted", &mr.accepted);
        st = sink(&mr, ud, err);
    }
    atlas_ipc_response_free(resp);
    atlas_buf_free(&raw);
    return st;
}

/* --- pack: local only ----------------------------------------------------- */

atlas_status atlas_service_memory_pack(atlas_ctx *ctx, const char *repo, const char *task,
                                       const char *run, atlas_memory_sink sink, void *ud,
                                       atlas_err *err) {
    if (repo == NULL || repo[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "which repository?");
    }
    bool has_task = task != NULL && task[0] != '\0';
    bool has_run = run != NULL && run[0] != '\0';
    if (has_task == has_run) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "memory pack needs exactly one of --task or --run");
    }
    if (ctx == NULL) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG, "%s", MEMORY_NO_LOCAL_HANDLE);
    }

    atlas_repo_info ri;
    atlas_repo_info_init(&ri);
    atlas_status st = atlas_service_require_repo(ctx, repo, &ri, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&ri);
        return st;
    }

    atlas_memory_pack p;
    atlas_memory_pack_init(&p);
    atlas_buf which = ATLAS_BUF_INIT;
    atlas_buf claim_uids = ATLAS_BUF_INIT;
    atlas_buf joined = ATLAS_BUF_INIT;

    atlas_memory_render mr;
    memset(&mr, 0, sizeof mr);
    mr.form = "pack";
    mr.detail = true;
    mr.repo = ri.name;
    mr.pack_preview = has_task;

    if (has_run) {
        bool found = false;
        st = atlas_db_memory_pack_get(atlas_ctx_db(ctx), run, &p, &found, err);
        if (st == ATLAS_OK && (!found || p.repo_id != ri.id)) {
            mr.pack_found = false;
            mr.pack_other_repo = found && p.repo_id != ri.id;
            st = sink(&mr, ud, err);
            atlas_memory_pack_free(&p);
            atlas_buf_free(&which);
            atlas_buf_free(&claim_uids);
            atlas_buf_free(&joined);
            atlas_repo_info_free(&ri);
            return st;
        }
        if (st == ATLAS_OK) {
            mr.pack_found = true;
            atlas_syspolicy pol;
            atlas_syspolicy_load(&pol);
            atlas_memory_pack_status pstatus = ATLAS_MEMORY_PACK_UNKNOWN;
            atlas_status fst =
                atlas_memory_pack_freshness(atlas_ctx_db(ctx), &pol, &p, &pstatus, &which, err);
            if (fst == ATLAS_OK) {
                mr.pack_status = atlas_memory_pack_status_name(pstatus);
                mr.pack_which_moved = which.len > 0 ? atlas_buf_cstr(&which) : NULL;
            }
            bool checked = false, complete = false, rfound = false;
            atlas_err rerr;
            atlas_err_init(&rerr);
            (void)atlas_db_memory_pack_reliance_get(atlas_ctx_db(ctx), run, &checked, &complete,
                                                    &claim_uids, &rfound, &rerr);
            mr.reliance_checked = checked;
            mr.reliance_complete = complete;
            (void)netstring_join(atlas_buf_cstr(&claim_uids), &joined, err);
            mr.reliance_claim_uids = atlas_buf_cstr(&joined);
        }
    } else {
        atlas_syspolicy pol;
        atlas_syspolicy_load(&pol);
        st = atlas_memory_pack_build(atlas_ctx_db(ctx), ri.id, &pol, task, &p, err);
        mr.pack_found = st == ATLAS_OK;
    }

    if (st == ATLAS_OK) {
        mr.pack_claim_count = p.claim_count;
        mr.pack_excluded_count = p.excluded_count;
        mr.pack_unanchored_count = p.unanchored_count;
        mr.pack_digest = atlas_buf_cstr(&p.pack_digest);
        mr.pack_body = atlas_buf_cstr(&p.rendered);
        st = sink(&mr, ud, err);
    }
    atlas_memory_pack_free(&p);
    atlas_buf_free(&which);
    atlas_buf_free(&claim_uids);
    atlas_buf_free(&joined);
    atlas_repo_info_free(&ri);
    return st;
}

/* --- diff: local only ------------------------------------------------------ */

typedef struct diff_collect_ctx {
    atlas_memory_sink sink;
    void *ud;
    const char *repo;
    int64_t generation;
    atlas_status status;
    int64_t *count_out;
} diff_collect_ctx;

static atlas_status diff_row_cb(const char *claim_uid, atlas_memory_diff_kind kind,
                                const char *reason, void *ud, atlas_err *err) {
    diff_collect_ctx *c = (diff_collect_ctx *)ud;
    if (c->status != ATLAS_OK) {
        return ATLAS_OK;
    }
    atlas_memory_render mr;
    memset(&mr, 0, sizeof mr);
    mr.form = "diff";
    mr.in_list = true;
    mr.repo = c->repo;
    mr.diff_generation = c->generation;
    mr.diff_generation_found = true;
    mr.diff_claim_uid = claim_uid;
    mr.diff_kind = atlas_memory_diff_kind_name(kind);
    mr.diff_reason = reason;
    c->status = c->sink(&mr, c->ud, err);
    if (c->status == ATLAS_OK && c->count_out != NULL) {
        (*c->count_out)++;
    }
    return ATLAS_OK; /* keep walking; c->status is checked once the list closes */
}

atlas_status atlas_service_memory_diff(atlas_ctx *ctx, const char *repo, int64_t generation,
                                       atlas_memory_sink sink, void *ud, int64_t *count_out,
                                       atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    if (repo == NULL || repo[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "which repository?");
    }
    if (generation <= 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "--generation must be a positive integer");
    }
    if (ctx == NULL) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG, "%s", MEMORY_NO_LOCAL_HANDLE);
    }

    atlas_repo_info ri;
    atlas_repo_info_init(&ri);
    atlas_status st = atlas_service_require_repo(ctx, repo, &ri, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&ri);
        return st;
    }

    diff_collect_ctx c;
    c.sink = sink;
    c.ud = ud;
    c.repo = ri.name;
    c.generation = generation;
    c.status = ATLAS_OK;
    c.count_out = count_out;
    bool found = false;
    st = atlas_db_memory_generation_diffs_list(atlas_ctx_db(ctx), ri.id, generation, diff_row_cb, &c,
                                               &found, err);
    if (st == ATLAS_OK) {
        st = c.status;
    }
    if (st == ATLAS_OK && !found) {
        atlas_memory_render mr;
        memset(&mr, 0, sizeof mr);
        mr.form = "diff";
        mr.repo = ri.name;
        mr.diff_generation = generation;
        mr.diff_generation_found = false;
        st = sink(&mr, ud, err);
    }
    atlas_repo_info_free(&ri);
    return st;
}

/* --- patch: local only ------------------------------------------------------ */

atlas_status atlas_service_memory_patch(atlas_ctx *ctx, const char *repo, const char *source_uid,
                                        atlas_memory_sink sink, void *ud, atlas_err *err) {
    if (repo == NULL || repo[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "which repository?");
    }
    if (source_uid == NULL || source_uid[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "which source?");
    }
    if (ctx == NULL) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG, "%s", MEMORY_NO_LOCAL_HANDLE);
    }

    atlas_repo_info ri;
    atlas_repo_info_init(&ri);
    atlas_status st = atlas_service_require_repo(ctx, repo, &ri, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&ri);
        return st;
    }

    atlas_buf diff_out = ATLAS_BUF_INIT, findings_out = ATLAS_BUF_INIT;
    st = atlas_memory_patch_build(atlas_ctx_db(ctx), &ri, atlas_ctx_data_dir(ctx), source_uid,
                                  &diff_out, &findings_out, err);
    if (st == ATLAS_OK) {
        atlas_memory_render mr;
        memset(&mr, 0, sizeof mr);
        mr.form = "patch";
        mr.repo = ri.name;
        mr.patch_source_uid = source_uid;
        mr.patch_diff = atlas_buf_cstr(&diff_out);
        mr.patch_findings = atlas_buf_cstr(&findings_out);
        st = sink(&mr, ud, err);
    }
    atlas_buf_free(&diff_out);
    atlas_buf_free(&findings_out);
    atlas_repo_info_free(&ri);
    return st;
}

/* --- trailer: local only ----------------------------------------------------- */

atlas_status atlas_service_memory_trailer(atlas_ctx *ctx, const char *run, const char *reason,
                                          const char *commit, const char *repo,
                                          atlas_memory_sink sink, void *ud, atlas_err *err) {
    bool has_run = run != NULL && run[0] != '\0';
    bool has_commit = commit != NULL && commit[0] != '\0';
    if (has_run == has_commit) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "memory trailer needs --run and --reason (compose) or --commit and "
                             "--repo (show)");
    }
    /* Every usage-shape check (this one and the two below) runs before the
     * `ctx == NULL` refusal, so an argument mistake is always reported as
     * ATLAS_ERR_USAGE regardless of whether a local handle happens to be
     * available -- the same ordering `atlas_service_memory_pack`/`_diff`/
     * `_patch` already have by construction (their single required-argument
     * checks simply come first). */
    if (has_run && (reason == NULL || reason[0] == '\0')) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "--run needs --reason");
    }
    if (has_commit && (repo == NULL || repo[0] == '\0')) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "--commit needs --repo");
    }
    if (ctx == NULL) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG, "%s", MEMORY_NO_LOCAL_HANDLE);
    }

    if (has_run) {
        atlas_buf block = ATLAS_BUF_INIT;
        atlas_status st = atlas_memory_trailer_compose(atlas_ctx_db(ctx), run, reason, &block, err);
        if (st == ATLAS_OK) {
            atlas_memory_render mr;
            memset(&mr, 0, sizeof mr);
            mr.form = "trailer";
            mr.trailer_compose = true;
            mr.trailer_run = run;
            mr.trailer_block = atlas_buf_cstr(&block);
            st = sink(&mr, ud, err);
        }
        atlas_buf_free(&block);
        return st;
    }

    atlas_repo_info ri;
    atlas_repo_info_init(&ri);
    atlas_status st = atlas_service_require_repo(ctx, repo, &ri, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&ri);
        return st;
    }
    atlas_memory_trailer_binding b;
    atlas_memory_trailer_binding_init(&b);
    bool found = false;
    st = atlas_db_memory_trailer_binding_get(atlas_ctx_db(ctx), ri.id, commit, &b, &found, err);
    atlas_buf unknown_joined = ATLAS_BUF_INIT;
    if (st == ATLAS_OK && found) {
        (void)netstring_join(atlas_buf_cstr(&b.unknown_fields), &unknown_joined, err);
    }
    if (st == ATLAS_OK) {
        atlas_memory_render mr;
        memset(&mr, 0, sizeof mr);
        mr.form = "trailer";
        mr.trailer_compose = false;
        mr.repo = ri.name;
        mr.trailer_found = found;
        mr.trailer_has_block = b.has_block;
        mr.trailer_bound_hit = b.bound_hit;
        mr.trailer_run = atlas_buf_cstr(&b.run_uid);
        mr.trailer_generation = b.memory_generation;
        mr.trailer_context_digest_ok = b.context_digest_ok;
        mr.trailer_decision_set_ok = b.decision_set_ok;
        mr.trailer_change_reason_uid = atlas_buf_cstr(&b.change_reason_uid);
        mr.trailer_unknown_fields = atlas_buf_cstr(&unknown_joined);
        st = sink(&mr, ud, err);
    }
    atlas_memory_trailer_binding_free(&b);
    atlas_buf_free(&unknown_joined);
    atlas_repo_info_free(&ri);
    return st;
}
