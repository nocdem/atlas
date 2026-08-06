/* Atlas - building IPC requests and reading IPC responses, client side.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Two things live here, and they are two halves of the same decision.
 *
 * A1 built its two request documents with `atlas_buf_appendf` and refused any
 * repository path containing a quote, a backslash or a control byte rather than
 * escaping it — recorded as item 11 in docs/backlog.md. That was defensible when
 * the only things crossing the socket were a validated repository name and a
 * path the CLI could refuse. A2 sends filesystem paths it did not choose,
 * session identifiers a client chose, and prose a model wrote. A hand-built
 * document is not defensible for any of those.
 *
 * So requests are built with the same first-party streaming writer the daemon
 * answers with, and responses are read with the same bounded, hostile-input
 * yyjson discipline the daemon applies to requests. One escaping implementation,
 * one parser, both directions.
 *
 * yyjson stays inside src/ipc, as it does for the request parser.
 */
#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/ipc.h"
#include "atlas/jsonread.h"
#include "yyjson.h"

/* --- request construction ------------------------------------------------ */

struct atlas_ipc_params {
    FILE *stream;
    char *buffer;
    size_t size;
    atlas_json *j;
};

atlas_status atlas_ipc_params_begin(atlas_ipc_params **out, atlas_json **writer_out,
                                    atlas_err *err) {
    *out = NULL;
    *writer_out = NULL;
    atlas_ipc_params *p = calloc(1u, sizeof(*p));
    if (p == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory building a request");
    }
    p->stream = open_memstream(&p->buffer, &p->size);
    if (p->stream == NULL) {
        free(p);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "cannot open a request buffer");
    }
    p->j = atlas_json_new(p->stream, err);
    if (p->j == NULL) {
        (void)fclose(p->stream);
        free(p->buffer);
        free(p);
        return err->status;
    }
    atlas_status st = atlas_json_obj_begin(p->j, err);
    if (st != ATLAS_OK) {
        atlas_ipc_params_abort(p);
        return st;
    }
    *out = p;
    *writer_out = p->j;
    return ATLAS_OK;
}

atlas_status atlas_ipc_params_finish(atlas_ipc_params *p, atlas_buf *out, atlas_err *err) {
    atlas_status st = atlas_json_obj_end(p->j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_finish(p->j, err);
    } else {
        atlas_json_free(p->j);
    }
    p->j = NULL;
    if (fclose(p->stream) != 0 && st == ATLAS_OK) {
        st = atlas_err_set(err, ATLAS_ERR_INTERNAL, "cannot finish a request buffer");
    }
    p->stream = NULL;
    if (st == ATLAS_OK) {
        /* The writer appends a trailing newline when it finishes a document.
         * Harmless inside a length-framed payload, but trimmed so the params
         * text is exactly the object and nothing else. */
        size_t n = p->size;
        while (n > 0 && (p->buffer[n - 1u] == '\n' || p->buffer[n - 1u] == '\r')) {
            n--;
        }
        st = atlas_buf_set(out, p->buffer, n, err);
    }
    free(p->buffer);
    free(p);
    return st;
}

void atlas_ipc_params_abort(atlas_ipc_params *p) {
    if (p == NULL) {
        return;
    }
    if (p->j != NULL) {
        atlas_json_free(p->j);
    }
    if (p->stream != NULL) {
        (void)fclose(p->stream);
    }
    free(p->buffer);
    free(p);
}

/* --- response parsing ---------------------------------------------------- */

struct atlas_ipc_response {
    yyjson_doc *doc;
    yyjson_val *root;
    yyjson_val *result; /* NULL on failure or when absent */
    bool ok;
    atlas_status status;
    char message[ATLAS_ERR_MSG_MAX];
};

atlas_status atlas_ipc_response_parse(const void *payload, size_t len, atlas_ipc_response **out,
                                      atlas_err *err) {
    *out = NULL;
    if (payload == NULL || len == 0) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "the Atlas daemon returned nothing");
    }
    if (len > ATLAS_IPC_MAX_RESPONSE_BYTES) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "the Atlas daemon returned %zu bytes, above the response limit", len);
    }
    atlas_ipc_response *r = calloc(1u, sizeof(*r));
    if (r == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory reading a response");
    }
    r->status = ATLAS_ERR_INTERNAL;

    /* The const cast is sound for the same reason it is in the request parser:
     * without YYJSON_READ_INSITU, yyjson copies the input and never writes to
     * it. Spelled through a union rather than a cast because -Wcast-qual is on
     * and suppressing that warning is the habit it exists to prevent. */
    union {
        const void *in;
        char *out;
    } cast;
    cast.in = payload;
    yyjson_read_err rerr;
    memset(&rerr, 0, sizeof(rerr));
    r->doc = yyjson_read_opts(cast.out, len, YYJSON_READ_NOFLAG, NULL, &rerr);
    if (r->doc == NULL) {
        atlas_ipc_response_free(r);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "the Atlas daemon's response is not valid JSON: %s (at byte %zu)",
                             rerr.msg != NULL ? rerr.msg : "parse error", rerr.pos);
    }
    r->root = yyjson_doc_get_root(r->doc);
    if (r->root == NULL || !yyjson_is_obj(r->root)) {
        atlas_ipc_response_free(r);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "the Atlas daemon's response is not a JSON object");
    }

    yyjson_val *ok = yyjson_obj_get(r->root, "ok");
    r->ok = (ok != NULL && yyjson_is_true(ok));
    if (r->ok) {
        r->status = ATLAS_OK;
        yyjson_val *result = yyjson_obj_get(r->root, "result");
        r->result = (result != NULL && yyjson_is_obj(result)) ? result : NULL;
    } else {
        yyjson_val *e = yyjson_obj_get(r->root, "error");
        if (e != NULL && yyjson_is_obj(e)) {
            yyjson_val *st = yyjson_obj_get(e, "status");
            if (st != NULL && yyjson_is_int(st)) {
                int64_t v = yyjson_get_sint(st);
                /* Only the documented range is accepted. A daemon reporting a
                 * status outside the contract is reported as an internal error
                 * rather than having its number believed. */
                if (v >= (int64_t)ATLAS_OK && v <= (int64_t)ATLAS_ERR_INTEGRITY) {
                    r->status = (atlas_status)v;
                }
            }
            yyjson_val *msg = yyjson_obj_get(e, "message");
            if (msg != NULL && yyjson_is_str(msg)) {
                const char *s = yyjson_get_str(msg);
                if (s != NULL) {
                    /* Already safe-encoded by the daemon, so it is copied rather
                     * than re-encoded: encoding it twice would stop it decoding
                     * back to the original bytes. */
                    (void)snprintf(r->message, sizeof(r->message), "%s", s);
                }
            }
        }
    }
    *out = r;
    return ATLAS_OK;
}

void atlas_ipc_response_free(atlas_ipc_response *r) {
    if (r == NULL) {
        return;
    }
    if (r->doc != NULL) {
        yyjson_doc_free(r->doc);
    }
    free(r);
}

bool atlas_ipc_response_ok(const atlas_ipc_response *r) {
    return r != NULL && r->ok;
}

atlas_status atlas_ipc_response_status(const atlas_ipc_response *r) {
    return r != NULL ? r->status : ATLAS_ERR_INTERNAL;
}

const char *atlas_ipc_response_message(const atlas_ipc_response *r) {
    return r != NULL ? r->message : "";
}

bool atlas_ipc_result_str(const atlas_ipc_response *r, const char *key, const char **out) {
    *out = NULL;
    if (r == NULL || r->result == NULL) {
        return false;
    }
    yyjson_val *v = yyjson_obj_get(r->result, key);
    if (v == NULL || !yyjson_is_str(v)) {
        return false;
    }
    const char *s = yyjson_get_str(v);
    if (s == NULL || strlen(s) != yyjson_get_len(v)) {
        return false; /* an embedded NUL would make the two halves disagree */
    }
    *out = s;
    return true;
}

bool atlas_ipc_result_int(const atlas_ipc_response *r, const char *key, int64_t *out) {
    *out = 0;
    if (r == NULL || r->result == NULL) {
        return false;
    }
    yyjson_val *v = yyjson_obj_get(r->result, key);
    if (v == NULL) {
        return false;
    }
    if (yyjson_is_sint(v)) {
        *out = yyjson_get_sint(v);
        return true;
    }
    if (yyjson_is_uint(v)) {
        uint64_t u = yyjson_get_uint(v);
        if (u > (uint64_t)INT64_MAX) {
            return false;
        }
        *out = (int64_t)u;
        return true;
    }
    return false;
}

bool atlas_ipc_result_bool(const atlas_ipc_response *r, const char *key, bool *out) {
    *out = false;
    if (r == NULL || r->result == NULL) {
        return false;
    }
    yyjson_val *v = yyjson_obj_get(r->result, key);
    if (v == NULL || !yyjson_is_bool(v)) {
        return false;
    }
    *out = yyjson_get_bool(v);
    return true;
}

/* --- re-emitting a result through the first-party writer ------------------
 *
 * The walk itself lives in src/ipc/json_read.c, because the MCP adapter needs
 * exactly the same transcoding and two copies of an iterative depth-bounded
 * walk is two places for a bound to be wrong. */

atlas_status atlas_ipc_result_write(const atlas_ipc_response *r, atlas_json *j, atlas_err *err) {
    if (r == NULL || r->result == NULL) {
        /* An empty object rather than null: a caller merging this into its own
         * document is expecting a container, and null would change its shape. */
        atlas_status st = atlas_json_obj_begin(j, err);
        if (st == ATLAS_OK) {
            st = atlas_json_obj_end(j, err);
        }
        return st;
    }
    union {
        yyjson_val *in;
        const atlas_jsonv *out;
    } cast;
    cast.in = r->result;
    return atlas_jsonv_write(cast.out, j, ATLAS_IPC_MAX_JSON_DEPTH, err);
}
