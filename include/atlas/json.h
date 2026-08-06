/* Atlas - streaming JSON writer.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The writer emits directly to a FILE* as values are produced: it never
 * assembles a complete response in memory, so response size is bounded by the
 * output stream rather than by RAM. State is a fixed-depth nesting stack.
 *
 * Escaping rules (stable contract, see docs/provenance.md):
 *   '"' becomes \" and '\' becomes \\; control bytes below 0x20 use the short
 *   forms \b \f \n \r \t where they exist and \u00XX otherwise; 0x7f is passed
 *   through verbatim, as permitted by RFC 8259.
 *   Byte sequences that are not valid UTF-8 are replaced, one invalid byte at a
 *   time, with U+FFFD. Callers that must preserve exact bytes emit a companion
 *   hex field via atlas_json_key_hex().
 */
#ifndef ATLAS_JSON_H
#define ATLAS_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "atlas/error.h"

#define ATLAS_JSON_MAX_DEPTH 32u

typedef struct atlas_json atlas_json;

/* Creates a writer over `out`. Does not take ownership of the stream. */
atlas_json *atlas_json_new(FILE *out, atlas_err *err);
/* Flushes a trailing newline and frees the writer. Returns the first error
 * encountered during the whole document, if any. */
atlas_status atlas_json_finish(atlas_json *j, atlas_err *err);
void atlas_json_free(atlas_json *j);

atlas_status atlas_json_obj_begin(atlas_json *j, atlas_err *err);
atlas_status atlas_json_obj_end(atlas_json *j, atlas_err *err);
atlas_status atlas_json_arr_begin(atlas_json *j, atlas_err *err);
atlas_status atlas_json_arr_end(atlas_json *j, atlas_err *err);

/* Object member name. Must be a valid UTF-8 C string. */
atlas_status atlas_json_key(atlas_json *j, const char *key, atlas_err *err);

atlas_status atlas_json_str(atlas_json *j, const char *s, atlas_err *err);
atlas_status atlas_json_bytes(atlas_json *j, const void *data, size_t n, atlas_err *err);
atlas_status atlas_json_int(atlas_json *j, int64_t v, atlas_err *err);
atlas_status atlas_json_uint(atlas_json *j, uint64_t v, atlas_err *err);
atlas_status atlas_json_bool(atlas_json *j, bool v, atlas_err *err);
atlas_status atlas_json_null(atlas_json *j, atlas_err *err);

/* Convenience: key + value in one call. `*_opt` variants emit null for NULL. */
atlas_status atlas_json_key_str(atlas_json *j, const char *key, const char *s, atlas_err *err);
atlas_status atlas_json_key_str_opt(atlas_json *j, const char *key, const char *s, atlas_err *err);
atlas_status atlas_json_key_bytes(atlas_json *j, const char *key, const void *d, size_t n,
                                  atlas_err *err);
atlas_status atlas_json_key_int(atlas_json *j, const char *key, int64_t v, atlas_err *err);
atlas_status atlas_json_key_int_opt(atlas_json *j, const char *key, const int64_t *v,
                                    atlas_err *err);
atlas_status atlas_json_key_bool(atlas_json *j, const char *key, bool v, atlas_err *err);
atlas_status atlas_json_key_null(atlas_json *j, const char *key, atlas_err *err);
/* Emits the bytes as a lowercase hex string value. */
atlas_status atlas_json_key_hex(atlas_json *j, const char *key, const void *d, size_t n,
                                atlas_err *err);

/* True when `n` bytes at `data` are well-formed UTF-8. */
bool atlas_utf8_valid(const void *data, size_t n);

#endif /* ATLAS_JSON_H */
