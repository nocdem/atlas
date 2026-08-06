/* Atlas - growable byte buffer.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 */
#include "atlas/buf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Refuse absurd growth early so an overflow can never be reached. */
#define ATLAS_BUF_MAX ((size_t)1 << 40)

void atlas_buf_init(atlas_buf *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void atlas_buf_free(atlas_buf *b) {
    if (b == NULL) {
        return;
    }
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void atlas_buf_reset(atlas_buf *b) {
    b->len = 0;
    if (b->data != NULL && b->cap > 0) {
        b->data[0] = '\0';
    }
}

atlas_status atlas_buf_reserve(atlas_buf *b, size_t extra, atlas_err *err) {
    if (extra > ATLAS_BUF_MAX - b->len - 1u) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "buffer growth request too large");
    }
    size_t need = b->len + extra + 1u; /* room for the implicit NUL */
    if (need <= b->cap) {
        return ATLAS_OK;
    }
    size_t cap = b->cap != 0 ? b->cap : 64u;
    while (cap < need) {
        if (cap > ATLAS_BUF_MAX / 2u) {
            cap = need;
            break;
        }
        cap *= 2u;
    }
    char *p = realloc(b->data, cap);
    if (p == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory growing buffer to %zu bytes",
                             cap);
    }
    b->data = p;
    b->cap = cap;
    /* Reserving alone must leave a valid NUL-terminated buffer: a caller that
     * reserves and then appends nothing (encoding an empty string, for instance)
     * would otherwise read uninitialised bytes through atlas_buf_cstr(). */
    b->data[b->len] = '\0';
    return ATLAS_OK;
}

atlas_status atlas_buf_append(atlas_buf *b, const void *data, size_t n, atlas_err *err) {
    if (n == 0) {
        /* Still ensure a valid NUL-terminated empty buffer. */
        if (b->data == NULL) {
            atlas_status st = atlas_buf_reserve(b, 0, err);
            if (st != ATLAS_OK) {
                return st;
            }
            b->data[0] = '\0';
        }
        return ATLAS_OK;
    }
    atlas_status st = atlas_buf_reserve(b, n, err);
    if (st != ATLAS_OK) {
        return st;
    }
    memcpy(b->data + b->len, data, n);
    b->len += n;
    b->data[b->len] = '\0';
    return ATLAS_OK;
}

atlas_status atlas_buf_append_str(atlas_buf *b, const char *s, atlas_err *err) {
    if (s == NULL) {
        return ATLAS_OK;
    }
    return atlas_buf_append(b, s, strlen(s), err);
}

atlas_status atlas_buf_append_ch(atlas_buf *b, char c, atlas_err *err) {
    return atlas_buf_append(b, &c, 1u, err);
}

atlas_status atlas_buf_appendf(atlas_buf *b, atlas_err *err, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "formatting failed");
    }
    atlas_status st = atlas_buf_reserve(b, (size_t)need, err);
    if (st != ATLAS_OK) {
        return st;
    }
    va_start(ap, fmt);
    int wrote = vsnprintf(b->data + b->len, (size_t)need + 1u, fmt, ap);
    va_end(ap);
    if (wrote < 0 || wrote > need) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "formatting failed");
    }
    b->len += (size_t)wrote;
    return ATLAS_OK;
}

atlas_status atlas_buf_set(atlas_buf *b, const void *data, size_t n, atlas_err *err) {
    atlas_buf_reset(b);
    return atlas_buf_append(b, data, n, err);
}

atlas_status atlas_buf_set_str(atlas_buf *b, const char *s, atlas_err *err) {
    atlas_buf_reset(b);
    return atlas_buf_append_str(b, s, err);
}

const char *atlas_buf_cstr(const atlas_buf *b) {
    if (b == NULL || b->data == NULL) {
        return "";
    }
    return b->data;
}

char *atlas_buf_detach(atlas_buf *b, size_t *len_out) {
    char *p = b->data;
    if (len_out != NULL) {
        *len_out = b->len;
    }
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    return p;
}
