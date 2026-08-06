/* Atlas - streaming JSON writer.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 */
#include "atlas/json.h"

#include <stdlib.h>
#include <string.h>

#include "atlas/safetext.h"

typedef enum json_frame {
    JSON_FRAME_OBJ = 0,
    JSON_FRAME_ARR
} json_frame;

struct atlas_json {
    FILE *out;
    json_frame stack[ATLAS_JSON_MAX_DEPTH];
    size_t depth;
    bool need_comma;  /* a value has already been written in this frame */
    bool after_key;   /* the next value belongs to a pending key */
    atlas_status first_error;
    char err_msg[256];
};

/* Records the first error and keeps returning it; a broken document never
 * silently becomes a valid one. */
static atlas_status jfail(atlas_json *j, atlas_err *err, atlas_status st, const char *msg) {
    if (j->first_error == ATLAS_OK) {
        j->first_error = st;
        (void)snprintf(j->err_msg, sizeof(j->err_msg), "%s", msg);
    }
    return atlas_err_set(err, st, "json: %s", msg);
}

static atlas_status jcheck(atlas_json *j, atlas_err *err) {
    if (j->first_error != ATLAS_OK) {
        return atlas_err_set(err, j->first_error, "json: %s", j->err_msg);
    }
    return ATLAS_OK;
}

static atlas_status jputc(atlas_json *j, char c, atlas_err *err) {
    if (fputc((unsigned char)c, j->out) == EOF) {
        return jfail(j, err, ATLAS_ERR_INTERNAL, "write failed");
    }
    return ATLAS_OK;
}

static atlas_status jwrite(atlas_json *j, const char *s, size_t n, atlas_err *err) {
    if (n == 0) {
        return ATLAS_OK;
    }
    if (fwrite(s, 1u, n, j->out) != n) {
        return jfail(j, err, ATLAS_ERR_INTERNAL, "write failed");
    }
    return ATLAS_OK;
}

/* Emit the separator required before the next value in the current frame. */
static atlas_status jpre_value(atlas_json *j, atlas_err *err) {
    atlas_status st = jcheck(j, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (j->after_key) {
        j->after_key = false;
        return ATLAS_OK;
    }
    if (j->depth > 0 && j->stack[j->depth - 1u] == JSON_FRAME_OBJ) {
        return jfail(j, err, ATLAS_ERR_INTERNAL, "value written without a key");
    }
    if (j->need_comma) {
        st = jputc(j, ',', err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    return ATLAS_OK;
}

static void jpost_value(atlas_json *j) {
    j->need_comma = true;
}

static atlas_status jpush(atlas_json *j, json_frame f, char open, atlas_err *err) {
    atlas_status st = jpre_value(j, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (j->depth >= ATLAS_JSON_MAX_DEPTH) {
        return jfail(j, err, ATLAS_ERR_INTERNAL, "maximum nesting depth exceeded");
    }
    st = jputc(j, open, err);
    if (st != ATLAS_OK) {
        return st;
    }
    j->stack[j->depth++] = f;
    j->need_comma = false;
    return ATLAS_OK;
}

static atlas_status jpop(atlas_json *j, json_frame f, char close, atlas_err *err) {
    atlas_status st = jcheck(j, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (j->depth == 0 || j->stack[j->depth - 1u] != f) {
        return jfail(j, err, ATLAS_ERR_INTERNAL, "mismatched container close");
    }
    if (j->after_key) {
        return jfail(j, err, ATLAS_ERR_INTERNAL, "container closed after a dangling key");
    }
    st = jputc(j, close, err);
    if (st != ATLAS_OK) {
        return st;
    }
    j->depth--;
    jpost_value(j);
    return ATLAS_OK;
}

atlas_json *atlas_json_new(FILE *out, atlas_err *err) {
    atlas_json *j = calloc(1u, sizeof(*j));
    if (j == NULL) {
        (void)atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory creating json writer");
        return NULL;
    }
    j->out = out;
    j->first_error = ATLAS_OK;
    return j;
}

atlas_status atlas_json_finish(atlas_json *j, atlas_err *err) {
    if (j == NULL) {
        return ATLAS_OK;
    }
    atlas_status st = jcheck(j, err);
    if (st == ATLAS_OK && j->depth != 0) {
        st = jfail(j, err, ATLAS_ERR_INTERNAL, "document ended with an open container");
    }
    if (st == ATLAS_OK) {
        st = jputc(j, '\n', err);
    }
    if (st == ATLAS_OK && fflush(j->out) != 0) {
        st = jfail(j, err, ATLAS_ERR_INTERNAL, "flush failed");
    }
    atlas_json_free(j);
    return st;
}

void atlas_json_free(atlas_json *j) {
    free(j);
}

atlas_status atlas_json_obj_begin(atlas_json *j, atlas_err *err) {
    return jpush(j, JSON_FRAME_OBJ, '{', err);
}

atlas_status atlas_json_obj_end(atlas_json *j, atlas_err *err) {
    return jpop(j, JSON_FRAME_OBJ, '}', err);
}

atlas_status atlas_json_arr_begin(atlas_json *j, atlas_err *err) {
    return jpush(j, JSON_FRAME_ARR, '[', err);
}

atlas_status atlas_json_arr_end(atlas_json *j, atlas_err *err) {
    return jpop(j, JSON_FRAME_ARR, ']', err);
}

/* --- string escaping ---------------------------------------------------- */

/* One UTF-8 decoder serves the whole codebase; see atlas/safetext.h. */
static size_t utf8_seq_len_json(const unsigned char *p, size_t avail) {
    uint32_t cp = 0;
    return atlas_utf8_decode(p, avail, &cp);
}

bool atlas_utf8_valid(const void *data, size_t n) {
    const unsigned char *p = (const unsigned char *)data;
    size_t i = 0;
    while (i < n) {
        size_t seq = utf8_seq_len_json(p + i, n - i);
        if (seq == 0) {
            return false;
        }
        i += seq;
    }
    return true;
}

static atlas_status jstring_body(atlas_json *j, const void *data, size_t n, atlas_err *err) {
    static const char hexd[] = "0123456789abcdef";
    const unsigned char *p = (const unsigned char *)data;
    size_t i = 0;
    size_t run_start = 0;
    atlas_status st;

#define FLUSH_RUN()                                                     \
    do {                                                                \
        if (i > run_start) {                                            \
            st = jwrite(j, (const char *)p + run_start, i - run_start, err); \
            if (st != ATLAS_OK) {                                       \
                return st;                                              \
            }                                                           \
        }                                                               \
    } while (0)

    while (i < n) {
        unsigned char c = p[i];
        if (c == '"' || c == '\\') {
            FLUSH_RUN();
            char esc[2] = {'\\', (char)c};
            st = jwrite(j, esc, 2u, err);
            if (st != ATLAS_OK) {
                return st;
            }
            i++;
            run_start = i;
            continue;
        }
        if (c < 0x20u) {
            FLUSH_RUN();
            const char *shortform = NULL;
            switch (c) {
            case '\b': shortform = "\\b"; break;
            case '\f': shortform = "\\f"; break;
            case '\n': shortform = "\\n"; break;
            case '\r': shortform = "\\r"; break;
            case '\t': shortform = "\\t"; break;
            default: break;
            }
            if (shortform != NULL) {
                st = jwrite(j, shortform, 2u, err);
            } else {
                char u[6] = {'\\', 'u', '0', '0', hexd[(c >> 4) & 0x0fu], hexd[c & 0x0fu]};
                st = jwrite(j, u, 6u, err);
            }
            if (st != ATLAS_OK) {
                return st;
            }
            i++;
            run_start = i;
            continue;
        }
        size_t seq = utf8_seq_len_json(p + i, n - i);
        if (seq == 0) {
            /* Invalid byte: emit U+FFFD and advance one byte. Callers that need
             * the exact bytes pair this with a hex field. */
            FLUSH_RUN();
            st = jwrite(j, "\xef\xbf\xbd", 3u, err);
            if (st != ATLAS_OK) {
                return st;
            }
            i++;
            run_start = i;
            continue;
        }
        i += seq;
    }
    FLUSH_RUN();
#undef FLUSH_RUN
    return ATLAS_OK;
}

atlas_status atlas_json_bytes(atlas_json *j, const void *data, size_t n, atlas_err *err) {
    atlas_status st = jpre_value(j, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = jputc(j, '"', err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = jstring_body(j, data, n, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = jputc(j, '"', err);
    if (st != ATLAS_OK) {
        return st;
    }
    jpost_value(j);
    return ATLAS_OK;
}

atlas_status atlas_json_str(atlas_json *j, const char *s, atlas_err *err) {
    if (s == NULL) {
        return atlas_json_null(j, err);
    }
    return atlas_json_bytes(j, s, strlen(s), err);
}

atlas_status atlas_json_key(atlas_json *j, const char *key, atlas_err *err) {
    atlas_status st = jcheck(j, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (j->depth == 0 || j->stack[j->depth - 1u] != JSON_FRAME_OBJ) {
        return jfail(j, err, ATLAS_ERR_INTERNAL, "key written outside an object");
    }
    if (j->after_key) {
        return jfail(j, err, ATLAS_ERR_INTERNAL, "two consecutive keys");
    }
    if (j->need_comma) {
        st = jputc(j, ',', err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    st = jputc(j, '"', err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = jstring_body(j, key, strlen(key), err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = jwrite(j, "\":", 2u, err);
    if (st != ATLAS_OK) {
        return st;
    }
    j->after_key = true;
    j->need_comma = true;
    return ATLAS_OK;
}

atlas_status atlas_json_int(atlas_json *j, int64_t v, atlas_err *err) {
    atlas_status st = jpre_value(j, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
    if (n < 0) {
        return jfail(j, err, ATLAS_ERR_INTERNAL, "number formatting failed");
    }
    st = jwrite(j, tmp, (size_t)n, err);
    if (st != ATLAS_OK) {
        return st;
    }
    jpost_value(j);
    return ATLAS_OK;
}

atlas_status atlas_json_uint(atlas_json *j, uint64_t v, atlas_err *err) {
    atlas_status st = jpre_value(j, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)v);
    if (n < 0) {
        return jfail(j, err, ATLAS_ERR_INTERNAL, "number formatting failed");
    }
    st = jwrite(j, tmp, (size_t)n, err);
    if (st != ATLAS_OK) {
        return st;
    }
    jpost_value(j);
    return ATLAS_OK;
}

atlas_status atlas_json_bool(atlas_json *j, bool v, atlas_err *err) {
    atlas_status st = jpre_value(j, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = v ? jwrite(j, "true", 4u, err) : jwrite(j, "false", 5u, err);
    if (st != ATLAS_OK) {
        return st;
    }
    jpost_value(j);
    return ATLAS_OK;
}

atlas_status atlas_json_null(atlas_json *j, atlas_err *err) {
    atlas_status st = jpre_value(j, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = jwrite(j, "null", 4u, err);
    if (st != ATLAS_OK) {
        return st;
    }
    jpost_value(j);
    return ATLAS_OK;
}

/* --- key + value helpers ------------------------------------------------ */

#define KEY_THEN(expr)                    \
    do {                                  \
        atlas_status st_ = (expr);        \
        if (st_ != ATLAS_OK) {            \
            return st_;                   \
        }                                 \
    } while (0)

atlas_status atlas_json_key_str(atlas_json *j, const char *key, const char *s, atlas_err *err) {
    KEY_THEN(atlas_json_key(j, key, err));
    return atlas_json_str(j, s != NULL ? s : "", err);
}

atlas_status atlas_json_key_str_opt(atlas_json *j, const char *key, const char *s, atlas_err *err) {
    KEY_THEN(atlas_json_key(j, key, err));
    if (s == NULL) {
        return atlas_json_null(j, err);
    }
    return atlas_json_str(j, s, err);
}

atlas_status atlas_json_key_bytes(atlas_json *j, const char *key, const void *d, size_t n,
                                  atlas_err *err) {
    KEY_THEN(atlas_json_key(j, key, err));
    return atlas_json_bytes(j, d, n, err);
}

atlas_status atlas_json_key_int(atlas_json *j, const char *key, int64_t v, atlas_err *err) {
    KEY_THEN(atlas_json_key(j, key, err));
    return atlas_json_int(j, v, err);
}

atlas_status atlas_json_key_int_opt(atlas_json *j, const char *key, const int64_t *v,
                                    atlas_err *err) {
    KEY_THEN(atlas_json_key(j, key, err));
    if (v == NULL) {
        return atlas_json_null(j, err);
    }
    return atlas_json_int(j, *v, err);
}

atlas_status atlas_json_key_bool(atlas_json *j, const char *key, bool v, atlas_err *err) {
    KEY_THEN(atlas_json_key(j, key, err));
    return atlas_json_bool(j, v, err);
}

atlas_status atlas_json_key_null(atlas_json *j, const char *key, atlas_err *err) {
    KEY_THEN(atlas_json_key(j, key, err));
    return atlas_json_null(j, err);
}

atlas_status atlas_json_key_hex(atlas_json *j, const char *key, const void *d, size_t n,
                                atlas_err *err) {
    static const char hexd[] = "0123456789abcdef";
    KEY_THEN(atlas_json_key(j, key, err));
    atlas_status st = jpre_value(j, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = jputc(j, '"', err);
    if (st != ATLAS_OK) {
        return st;
    }
    const unsigned char *p = (const unsigned char *)d;
    for (size_t i = 0; i < n; i++) {
        char pair[2] = {hexd[(p[i] >> 4) & 0x0fu], hexd[p[i] & 0x0fu]};
        st = jwrite(j, pair, 2u, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    st = jputc(j, '"', err);
    if (st != ATLAS_OK) {
        return st;
    }
    jpost_value(j);
    return ATLAS_OK;
}

#undef KEY_THEN
