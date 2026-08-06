/* Atlas - minimal independent JSON validator for tests.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Written independently of src/output/json.c on purpose: a writer that validated
 * its own output against itself would prove nothing.
 */
#ifndef ATLAS_TEST_JSONCHECK_H
#define ATLAS_TEST_JSONCHECK_H

#include <stdbool.h>
#include <stddef.h>

#include "atlas/buf.h"

/* True when the whole input is exactly one well-formed JSON value (trailing
 * whitespace allowed). On failure `*err_pos` receives the byte offset. */
bool tjson_valid(const char *s, size_t n, size_t *err_pos);

/* Finds the first `"key":` at any depth and decodes the string value that
 * follows, resolving \\uXXXX escapes to UTF-8. Returns false when the key is
 * absent or its value is not a string. */
bool tjson_get_string(const char *s, size_t n, const char *key, atlas_buf *out);

/* Finds the first `"key":` and reports the raw token that follows (number,
 * true, false, null, or the source span of a string/array/object). */
bool tjson_get_raw(const char *s, size_t n, const char *key, atlas_buf *out);

#endif /* ATLAS_TEST_JSONCHECK_H */
