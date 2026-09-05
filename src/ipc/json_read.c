/* Atlas - the one facade over the vendored JSON parser.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Every byte of JSON that Atlas did not write passes through this file: IPC
 * requests, IPC responses, Claude Code hook payloads, and MCP messages. yyjson
 * is called from src/ipc and nowhere else.
 *
 * The discipline is the same one src/ipc/proto.c established: the byte count is
 * bounded before the parser is entered, the nesting depth is bounded by an
 * iterative walk rather than a recursive one, malformed input produces a
 * structured error rather than a crash, and nothing is coerced.
 */
#define _GNU_SOURCE 1

#include "atlas/jsonread.h"

#include <stdlib.h>
#include <string.h>

#include "yyjson.h"

struct atlas_jsondoc {
    yyjson_doc *doc;
};

/* An atlas_jsonv is a yyjson_val. The opaque type is the seam that keeps
 * yyjson out of every other translation unit; these two casts are where it
 * lives, and they are spelled through unions because -Wcast-qual is on. */
static yyjson_val *as_val(const atlas_jsonv *v) {
    union {
        const atlas_jsonv *in;
        yyjson_val *out;
    } cast;
    cast.in = v;
    return cast.out;
}

static const atlas_jsonv *as_jsonv(yyjson_val *v) {
    union {
        yyjson_val *in;
        const atlas_jsonv *out;
    } cast;
    cast.in = v;
    return cast.out;
}

/* Establishes the document's depth without recursing into it.
 *
 * The stack is bounded by the limit itself, so the walk stops as soon as the
 * limit is exceeded rather than traversing the whole document to find out. */
static bool depth_ok(yyjson_val *root, unsigned limit) {
    if (root == NULL || limit == 0) {
        return root == NULL;
    }
    enum { MAX_STACK = 64 };
    if (limit > MAX_STACK) {
        limit = MAX_STACK;
    }
    yyjson_arr_iter arr_stack[MAX_STACK];
    yyjson_obj_iter obj_stack[MAX_STACK];
    bool is_obj[MAX_STACK];
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
            return true;
        }
        cur = next;
    }
}

atlas_status atlas_jsondoc_parse(const void *data, size_t len, size_t max_bytes,
                                 unsigned max_depth, atlas_jsondoc **out, atlas_err *err) {
    *out = NULL;
    if (data == NULL || len == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "the document is empty");
    }
    /* Checked before the parser is entered, so a large claimed size can never
     * become a large allocation. */
    if (max_bytes > 0 && len > max_bytes) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "the document is %zu bytes, above the %zu byte limit", len, max_bytes);
    }

    atlas_jsondoc *d = calloc(1u, sizeof(*d));
    if (d == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory parsing a document");
    }

    /* Without YYJSON_READ_INSITU, yyjson copies the input and never writes to
     * the buffer, so the const cast is sound. Spelled through a union rather
     * than a direct cast because -Wcast-qual is on, and silencing that warning
     * with a cast is exactly the habit it exists to prevent. */
    union {
        const void *in;
        char *out;
    } cast;
    cast.in = data;
    yyjson_read_err rerr;
    memset(&rerr, 0, sizeof(rerr));
    d->doc = yyjson_read_opts(cast.out, len, YYJSON_READ_NOFLAG, NULL, &rerr);
    if (d->doc == NULL) {
        atlas_status st =
            atlas_err_set(err, ATLAS_ERR_USAGE, "the document is not valid JSON: %s (at byte %zu)",
                          rerr.msg != NULL ? rerr.msg : "parse error", rerr.pos);
        atlas_jsondoc_free(d);
        return st;
    }
    if (max_depth > 0 && !depth_ok(yyjson_doc_get_root(d->doc), max_depth)) {
        atlas_jsondoc_free(d);
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                            "the document nests deeper than %u levels; refusing it", max_depth);
    }
    *out = d;
    return ATLAS_OK;
}

void atlas_jsondoc_free(atlas_jsondoc *d) {
    if (d == NULL) {
        return;
    }
    if (d->doc != NULL) {
        yyjson_doc_free(d->doc);
    }
    free(d);
}

const atlas_jsonv *atlas_jsondoc_root(const atlas_jsondoc *d) {
    if (d == NULL || d->doc == NULL) {
        return NULL;
    }
    return as_jsonv(yyjson_doc_get_root(d->doc));
}

bool atlas_jsonv_is_obj(const atlas_jsonv *v) {
    return v != NULL && yyjson_is_obj(as_val(v));
}

bool atlas_jsonv_is_arr(const atlas_jsonv *v) {
    return v != NULL && yyjson_is_arr(as_val(v));
}

bool atlas_jsonv_is_str(const atlas_jsonv *v) {
    return v != NULL && yyjson_is_str(as_val(v));
}

bool atlas_jsonv_is_int(const atlas_jsonv *v) {
    return v != NULL && (yyjson_is_sint(as_val(v)) || yyjson_is_uint(as_val(v)));
}

bool atlas_jsonv_is_bool(const atlas_jsonv *v) {
    return v != NULL && yyjson_is_bool(as_val(v));
}

bool atlas_jsonv_is_null(const atlas_jsonv *v) {
    return v != NULL && yyjson_is_null(as_val(v));
}

const atlas_jsonv *atlas_jsonv_get(const atlas_jsonv *obj, const char *key) {
    if (obj == NULL || key == NULL || !yyjson_is_obj(as_val(obj))) {
        return NULL;
    }
    yyjson_val *v = yyjson_obj_get(as_val(obj), key);
    return v != NULL ? as_jsonv(v) : NULL;
}

size_t atlas_jsonv_arr_len(const atlas_jsonv *arr) {
    if (arr == NULL || !yyjson_is_arr(as_val(arr))) {
        return 0;
    }
    return yyjson_arr_size(as_val(arr));
}

const atlas_jsonv *atlas_jsonv_at(const atlas_jsonv *arr, size_t index) {
    if (arr == NULL || !yyjson_is_arr(as_val(arr))) {
        return NULL;
    }
    yyjson_val *v = yyjson_arr_get(as_val(arr), index);
    return v != NULL ? as_jsonv(v) : NULL;
}

bool atlas_jsonv_str(const atlas_jsonv *v, const char **out, size_t *len_out) {
    *out = NULL;
    if (len_out != NULL) {
        *len_out = 0;
    }
    if (v == NULL || !yyjson_is_str(as_val(v))) {
        return false;
    }
    const char *s = yyjson_get_str(as_val(v));
    size_t n = yyjson_get_len(as_val(v));
    if (s == NULL || strlen(s) != n) {
        /* An embedded NUL would make the C view shorter than the JSON view. */
        return false;
    }
    *out = s;
    if (len_out != NULL) {
        *len_out = n;
    }
    return true;
}

bool atlas_jsonv_int(const atlas_jsonv *v, int64_t *out) {
    *out = 0;
    if (v == NULL) {
        return false;
    }
    /* Integers only. A float holding an integral value is still the wrong type
     * on the wire, and accepting it would make the contract depend on which
     * serialiser a client happens to use. */
    if (yyjson_is_sint(as_val(v))) {
        *out = yyjson_get_sint(as_val(v));
        return true;
    }
    if (yyjson_is_uint(as_val(v))) {
        uint64_t u = yyjson_get_uint(as_val(v));
        if (u > (uint64_t)INT64_MAX) {
            return false;
        }
        *out = (int64_t)u;
        return true;
    }
    return false;
}

bool atlas_jsonv_bool(const atlas_jsonv *v, bool *out) {
    *out = false;
    if (v == NULL || !yyjson_is_bool(as_val(v))) {
        return false;
    }
    *out = yyjson_get_bool(as_val(v));
    return true;
}

/* --- re-emitting through the first-party writer -------------------------- */

static atlas_status write_scalar(atlas_json *j, yyjson_val *v, atlas_err *err) {
    if (yyjson_is_str(v)) {
        const char *s = yyjson_get_str(v);
        return atlas_json_bytes(j, s != NULL ? s : "", s != NULL ? yyjson_get_len(v) : 0u, err);
    }
    if (yyjson_is_sint(v)) {
        return atlas_json_int(j, yyjson_get_sint(v), err);
    }
    if (yyjson_is_uint(v)) {
        return atlas_json_uint(j, yyjson_get_uint(v), err);
    }
    if (yyjson_is_bool(v)) {
        return atlas_json_bool(j, yyjson_get_bool(v), err);
    }
    /* A real is emitted as null rather than reformatted. Atlas' own documents
     * have no floating-point member, so one arriving here means two components
     * disagree about a field's type, and inventing a representation would hide
     * that rather than surface it. */
    return atlas_json_null(j, err);
}

atlas_status atlas_jsonv_write(const atlas_jsonv *v, atlas_json *j, unsigned max_depth,
                               atlas_err *err) {
    if (v == NULL) {
        return atlas_json_null(j, err);
    }
    enum { MAX_STACK = 64 };
    if (max_depth == 0 || max_depth > MAX_STACK) {
        max_depth = MAX_STACK;
    }
    struct frame {
        yyjson_obj_iter obj;
        yyjson_arr_iter arr;
        bool is_obj;
    } stack[MAX_STACK];
    unsigned depth = 0;

    yyjson_val *root = as_val(v);
    if (!yyjson_is_obj(root) && !yyjson_is_arr(root)) {
        return write_scalar(j, root, err);
    }

    atlas_status st;
    stack[0].is_obj = yyjson_is_obj(root);
    if (stack[0].is_obj) {
        yyjson_obj_iter_init(root, &stack[0].obj);
        st = atlas_json_obj_begin(j, err);
    } else {
        yyjson_arr_iter_init(root, &stack[0].arr);
        st = atlas_json_arr_begin(j, err);
    }
    if (st != ATLAS_OK) {
        return st;
    }
    depth = 1;

    while (depth > 0) {
        struct frame *f = &stack[depth - 1u];
        yyjson_val *value = NULL;
        if (f->is_obj) {
            yyjson_val *key = yyjson_obj_iter_next(&f->obj);
            if (key == NULL) {
                st = atlas_json_obj_end(j, err);
                if (st != ATLAS_OK) {
                    return st;
                }
                depth--;
                continue;
            }
            const char *name = yyjson_get_str(key);
            if (name == NULL) {
                continue; /* a non-string key is skipped, never guessed at */
            }
            st = atlas_json_key(j, name, err);
            if (st != ATLAS_OK) {
                return st;
            }
            value = yyjson_obj_iter_get_val(key);
        } else {
            value = yyjson_arr_iter_next(&f->arr);
            if (value == NULL) {
                st = atlas_json_arr_end(j, err);
                if (st != ATLAS_OK) {
                    return st;
                }
                depth--;
                continue;
            }
        }

        if (yyjson_is_obj(value) || yyjson_is_arr(value)) {
            if (depth >= max_depth) {
                st = atlas_json_null(j, err);
                if (st != ATLAS_OK) {
                    return st;
                }
                continue;
            }
            struct frame *nf = &stack[depth];
            nf->is_obj = yyjson_is_obj(value);
            if (nf->is_obj) {
                yyjson_obj_iter_init(value, &nf->obj);
                st = atlas_json_obj_begin(j, err);
            } else {
                yyjson_arr_iter_init(value, &nf->arr);
                st = atlas_json_arr_begin(j, err);
            }
            if (st != ATLAS_OK) {
                return st;
            }
            depth++;
            continue;
        }
        st = write_scalar(j, value, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    return ATLAS_OK;
}

const char *atlas_jsonv_str_member(const atlas_jsonv *obj, const char *key) {
    const char *s = NULL;
    if (atlas_jsonv_str(atlas_jsonv_get(obj, key), &s, NULL)) {
        return s;
    }
    return NULL;
}

const char *atlas_jsonv_str_member2(const atlas_jsonv *obj, const char *k1, const char *k2) {
    return atlas_jsonv_str_member(atlas_jsonv_get(obj, k1), k2);
}

atlas_status atlas_jsonv_check_only_keys(const atlas_jsonv *obj,
                                         const char *const *allowed,
                                         atlas_err *err) {
    if (obj == NULL || !yyjson_is_obj(as_val(obj))) {
        return ATLAS_OK;
    }
    yyjson_obj_iter it;
    yyjson_obj_iter_init(as_val(obj), &it);
    yyjson_val *key;
    while ((key = yyjson_obj_iter_next(&it)) != NULL) {
        const char *k = yyjson_get_str(key);
        if (k == NULL) {
            continue;
        }
        bool found = false;
        for (size_t i = 0; allowed[i] != NULL; i++) {
            if (strcmp(k, allowed[i]) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "\"%s\" is not a recognised argument", k);
        }
    }
    return ATLAS_OK;
}
