/* Atlas - safe encoding of untrusted text.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Repository content and Git metadata are DATA, never instructions and never
 * terminal commands. Filenames, branch names, author identities, commit subjects
 * and bodies, and Git's own error text all originate outside Atlas and may
 * contain byte sequences that a terminal interprets: colour and cursor
 * manipulation (ESC [ ...), window-title and hyperlink control (OSC ... BEL),
 * carriage returns that overwrite an already-printed line, and bidirectional
 * overrides that make text read differently from how it is stored.
 *
 * Every such value is passed through atlas_text_encode_safe() before it reaches a
 * terminal or a JSON document. The encoding is:
 *
 *   - lossless and reversible: atlas_text_decode_safe() reproduces the exact bytes
 *   - always valid UTF-8, so it is safe to place in JSON
 *   - free of C0 and C1 control bytes, DEL, line/paragraph separators and
 *     bidirectional controls
 *   - identical to the input for ordinary printable text, so readable values stay
 *     readable
 *
 * Escaped items become %XX per byte (uppercase hex), and '%' itself is escaped so
 * the transform stays reversible. This is the same encoding used for repository
 * paths, so one rule covers every untrusted string Atlas prints.
 */
#ifndef ATLAS_SAFETEXT_H
#define ATLAS_SAFETEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "atlas/buf.h"
#include "atlas/error.h"

/* Name of the encoding, reported in JSON output as a contract marker. */
#define ATLAS_TEXT_ENCODING_NAME "atlas-safe-1"

/* Appends the safe encoding of `raw` to `out`. */
atlas_status atlas_text_encode_safe(const void *raw, size_t n, atlas_buf *out, atlas_err *err);
/* Reverses atlas_text_encode_safe(), appending the original bytes to `out`. */
atlas_status atlas_text_decode_safe(const char *text, size_t n, atlas_buf *out, atlas_err *err);
/* True when `raw` needs no escaping at all. */
bool atlas_text_is_safe(const void *raw, size_t n);

/* True when this code point is escaped by the encoding above. Exposed so tests
 * can assert the policy rather than a hard-coded list of examples. */
bool atlas_codepoint_is_unsafe(uint32_t cp);

/* Decodes one UTF-8 sequence, returning its length in bytes and writing the code
 * point, or 0 when the bytes are not well-formed UTF-8. Overlong encodings,
 * surrogates and values above U+10FFFF are rejected. This is the single UTF-8
 * decoder in Atlas; paths, safe text and the JSON writer all use it. */
size_t atlas_utf8_decode(const unsigned char *p, size_t avail, uint32_t *cp_out);

/* --- convenience for renderers ------------------------------------------ */

/* A small ring of scratch buffers, so several untrusted values can be encoded in
 * one printf-style call without the caller managing storage. Values remain valid
 * until the slot is reused, which happens after ATLAS_SAFE_POOL_SLOTS further
 * encodings. */
#define ATLAS_SAFE_POOL_SLOTS 8

typedef struct atlas_safe_pool {
    atlas_buf slots[ATLAS_SAFE_POOL_SLOTS];
    size_t next;
} atlas_safe_pool;

void atlas_safe_pool_init(atlas_safe_pool *p);
void atlas_safe_pool_free(atlas_safe_pool *p);

/* Encode into the next slot. Never returns NULL: on allocation failure it returns
 * a fixed placeholder, because failing to print is worse than printing a marker. */
const char *atlas_safe(atlas_safe_pool *p, const char *s);
const char *atlas_safe_n(atlas_safe_pool *p, const void *raw, size_t n);

#endif /* ATLAS_SAFETEXT_H */
