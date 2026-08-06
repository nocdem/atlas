/* Atlas - growable byte buffer.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Ownership: an atlas_buf owns its heap allocation. Initialise with
 * ATLAS_BUF_INIT or atlas_buf_init(); release with atlas_buf_free(). All
 * append operations keep a NUL terminator one byte past `len` so `data` can be
 * passed to C string APIs when the content is known to be NUL-free, but the
 * buffer is fully binary-safe otherwise.
 */
#ifndef ATLAS_BUF_H
#define ATLAS_BUF_H

#include <stdarg.h>
#include <stddef.h>

#include "atlas/error.h"

typedef struct atlas_buf {
    char *data;   /* NULL when cap == 0 */
    size_t len;   /* bytes in use, excluding the implicit NUL */
    size_t cap;   /* allocated bytes, including room for the NUL */
} atlas_buf;

#define ATLAS_BUF_INIT { NULL, 0, 0 }

void atlas_buf_init(atlas_buf *b);
void atlas_buf_free(atlas_buf *b);
void atlas_buf_reset(atlas_buf *b); /* keeps capacity, len = 0 */

/* Ensure room for `extra` more bytes. */
atlas_status atlas_buf_reserve(atlas_buf *b, size_t extra, atlas_err *err);

atlas_status atlas_buf_append(atlas_buf *b, const void *data, size_t n, atlas_err *err);
atlas_status atlas_buf_append_str(atlas_buf *b, const char *s, atlas_err *err);
atlas_status atlas_buf_append_ch(atlas_buf *b, char c, atlas_err *err);
atlas_status atlas_buf_appendf(atlas_buf *b, atlas_err *err, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
atlas_status atlas_buf_set(atlas_buf *b, const void *data, size_t n, atlas_err *err);
atlas_status atlas_buf_set_str(atlas_buf *b, const char *s, atlas_err *err);

/* Content as a C string; returns "" for an empty buffer (never NULL). */
const char *atlas_buf_cstr(const atlas_buf *b);

/* Detach the allocation. Caller must free() the result. Buffer becomes empty. */
char *atlas_buf_detach(atlas_buf *b, size_t *len_out);

#endif /* ATLAS_BUF_H */
