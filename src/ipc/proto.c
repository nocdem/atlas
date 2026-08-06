/* Atlas - IPC request parsing.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * This is the one place in Atlas that parses JSON it did not write. Everything
 * here treats the payload as hostile:
 *
 *   - the byte count was already bounded by the frame reader, so yyjson is never
 *     handed an unbounded buffer
 *   - nesting depth is bounded explicitly, because yyjson imposes no depth limit
 *     of its own and a deeply nested document would otherwise be a cheap way to
 *     make a recursive consumer misbehave
 *   - malformed input produces a structured error and closes one connection; it
 *     never terminates the daemon
 *   - strings are only accepted where a string is expected. There is no
 *     coercion: a caller that sends a number where a name belongs gets an error,
 *     not a guess.
 *
 * Responses are NOT built here. They go through the first-party streaming writer
 * in src/output/json.c, so the escaping contract established in A0 is the same
 * one the daemon speaks.
 */
#define _GNU_SOURCE 1

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/ipc.h"
#include "atlas/safetext.h"
#include "yyjson.h"

struct atlas_ipc_request {
    yyjson_doc *doc;
    yyjson_val *root;
    yyjson_val *params; /* NULL when absent */
    char id[ATLAS_IPC_MAX_REQUEST_ID + 1u];
    char method[ATLAS_IPC_MAX_METHOD + 1u];
};

/* Walks the document iteratively to establish its depth.
 *
 * Iterative rather than recursive on purpose: measuring the depth of a hostile
 * document by recursing into it is the same stack exhaustion the limit exists to
 * prevent. The walk carries an explicit stack of container iterators bounded by
 * the limit itself, so it stops as soon as the limit is exceeded and never
 * traverses the whole document to find out. */
static bool depth_within_limit(yyjson_val *root, unsigned limit) {
    if (root == NULL) {
        return true;
    }
    yyjson_arr_iter arr_stack[ATLAS_IPC_MAX_JSON_DEPTH];
    yyjson_obj_iter obj_stack[ATLAS_IPC_MAX_JSON_DEPTH];
    bool is_obj[ATLAS_IPC_MAX_JSON_DEPTH];
    unsigned depth = 0;

    yyjson_val *cur = root;
    for (;;) {
        if (yyjson_is_arr(cur) || yyjson_is_obj(cur)) {
            if (depth >= limit) {
                return false;
            }
            is_obj[depth] = yyjson_is_obj(cur);
            if (is_obj[depth]) {
                yyjson_obj_iter_init(cur, &obj_stack[depth]);
            } else {
                yyjson_arr_iter_init(cur, &arr_stack[depth]);
            }
            depth++;
        }
        /* Ascend until a container with a remaining child is found. */
        yyjson_val *next = NULL;
        while (depth > 0) {
            unsigned top = depth - 1u;
            if (is_obj[top]) {
                yyjson_val *key = yyjson_obj_iter_next(&obj_stack[top]);
                if (key != NULL) {
                    next = yyjson_obj_iter_get_val(key);
                    break;
                }
            } else {
                next = yyjson_arr_iter_next(&arr_stack[top]);
                if (next != NULL) {
                    break;
                }
            }
            depth--;
        }
        if (next == NULL) {
            return true; /* walked out of the root */
        }
        cur = next;
    }
}

/* Copies a bounded, printable identifier out of the document.
 *
 * Request ids and method names are echoed back to the caller and appear in log
 * lines, so they go through the same safe-text encoding every other untrusted
 * string does. A value that does not fit is refused rather than truncated: a
 * truncated id would correlate a response with the wrong request. */
static atlas_status copy_ident(yyjson_val *v, const char *what, char *dst, size_t dst_size,
                               atlas_err *err) {
    if (v == NULL || !yyjson_is_str(v)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "request %s must be a string", what);
    }
    const char *s = yyjson_get_str(v);
    size_t n = yyjson_get_len(v);
    if (s == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "request %s is not readable", what);
    }
    atlas_buf safe = ATLAS_BUF_INIT;
    atlas_status st = atlas_text_encode_safe(s, n, &safe, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&safe);
        return st;
    }
    if (safe.len + 1u > dst_size) {
        atlas_buf_free(&safe);
        return atlas_err_set(err, ATLAS_ERR_USAGE, "request %s is longer than %zu bytes", what,
                             dst_size - 1u);
    }
    memcpy(dst, safe.data, safe.len);
    dst[safe.len] = '\0';
    atlas_buf_free(&safe);
    return ATLAS_OK;
}

atlas_status atlas_ipc_request_parse(const void *payload, size_t len, atlas_ipc_request **out,
                                     atlas_err *err) {
    *out = NULL;
    if (payload == NULL || len == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "empty request payload");
    }
    if (len > ATLAS_IPC_MAX_REQUEST_BYTES) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "request payload of %zu bytes exceeds the limit",
                             len);
    }

    atlas_ipc_request *req = calloc(1u, sizeof(*req));
    if (req == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory parsing a request");
    }

    yyjson_read_err rerr;
    memset(&rerr, 0, sizeof(rerr));
    /* No YYJSON_READ_INSITU: without it yyjson copies the input and never writes
     * to the buffer, so the const cast below is sound. It is spelled out through
     * a union rather than with a direct cast because -Wcast-qual is on, and
     * silencing that warning with a cast is exactly the habit it exists to
     * prevent — the reason the write cannot happen belongs in a comment, not in
     * a suppressed diagnostic.
     *
     * No comments, no trailing commas, no NaN or Infinity: the wire format is
     * strict JSON. Accepting extensions would let the daemon and a client
     * disagree about what a document says. */
    union {
        const void *in;
        char *out;
    } payload_cast;
    payload_cast.in = payload;
    req->doc = yyjson_read_opts(payload_cast.out, len, YYJSON_READ_NOFLAG, NULL, &rerr);
    if (req->doc == NULL) {
        /* yyjson's message is a fixed English string from its own table, not
         * attacker data, but the offset is echoed so a caller can find the fault. */
        atlas_status st = atlas_err_set(err, ATLAS_ERR_USAGE,
                                        "request is not valid JSON: %s (at byte %zu)",
                                        rerr.msg != NULL ? rerr.msg : "parse error", rerr.pos);
        atlas_ipc_request_free(req);
        return st;
    }
    req->root = yyjson_doc_get_root(req->doc);
    if (req->root == NULL || !yyjson_is_obj(req->root)) {
        atlas_ipc_request_free(req);
        return atlas_err_set(err, ATLAS_ERR_USAGE, "request must be a JSON object");
    }
    if (!depth_within_limit(req->root, ATLAS_IPC_MAX_JSON_DEPTH)) {
        atlas_ipc_request_free(req);
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "request nests deeper than %u levels; refusing it",
                             (unsigned)ATLAS_IPC_MAX_JSON_DEPTH);
    }

    atlas_status st = copy_ident(yyjson_obj_get(req->root, "method"), "method", req->method,
                                 sizeof(req->method), err);
    if (st != ATLAS_OK) {
        atlas_ipc_request_free(req);
        return st;
    }
    yyjson_val *id = yyjson_obj_get(req->root, "id");
    if (id == NULL) {
        /* An absent id is allowed; a present but unusable one is not, because
         * silently replacing it would let a caller believe its correlation
         * worked. */
        (void)snprintf(req->id, sizeof(req->id), "0");
    } else {
        st = copy_ident(id, "id", req->id, sizeof(req->id), err);
        if (st != ATLAS_OK) {
            atlas_ipc_request_free(req);
            return st;
        }
    }

    yyjson_val *params = yyjson_obj_get(req->root, "params");
    if (params != NULL && !yyjson_is_obj(params)) {
        atlas_ipc_request_free(req);
        return atlas_err_set(err, ATLAS_ERR_USAGE, "request params must be an object when present");
    }
    req->params = params;

    *out = req;
    return ATLAS_OK;
}

void atlas_ipc_request_free(atlas_ipc_request *req) {
    if (req == NULL) {
        return;
    }
    if (req->doc != NULL) {
        yyjson_doc_free(req->doc);
    }
    free(req);
}

const char *atlas_ipc_request_id(const atlas_ipc_request *req) {
    return req->id;
}

const char *atlas_ipc_request_method(const atlas_ipc_request *req) {
    return req->method;
}

bool atlas_ipc_param_str(const atlas_ipc_request *req, const char *key, const char **out) {
    *out = NULL;
    if (req->params == NULL) {
        return false;
    }
    yyjson_val *v = yyjson_obj_get(req->params, key);
    if (v == NULL || !yyjson_is_str(v)) {
        return false;
    }
    const char *s = yyjson_get_str(v);
    if (s == NULL) {
        return false;
    }
    /* An embedded NUL would make the C string shorter than the JSON string, so
     * the two halves of Atlas would disagree about the value. Refuse it. */
    if (strlen(s) != yyjson_get_len(v)) {
        return false;
    }
    *out = s;
    return true;
}

bool atlas_ipc_param_int(const atlas_ipc_request *req, const char *key, int64_t *out) {
    *out = 0;
    if (req->params == NULL) {
        return false;
    }
    yyjson_val *v = yyjson_obj_get(req->params, key);
    if (v == NULL) {
        return false;
    }
    /* Integers only. A float that happens to hold an integral value is still the
     * wrong type on the wire, and accepting it would make the contract depend on
     * a client's serialiser. */
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

bool atlas_ipc_param_bool(const atlas_ipc_request *req, const char *key, bool *out) {
    *out = false;
    if (req->params == NULL) {
        return false;
    }
    yyjson_val *v = yyjson_obj_get(req->params, key);
    if (v == NULL || !yyjson_is_bool(v)) {
        return false;
    }
    *out = yyjson_get_bool(v);
    return true;
}
