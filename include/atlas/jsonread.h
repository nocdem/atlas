/* Atlas - reading JSON that Atlas did not write.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A0 and A1 had exactly one such document: an IPC request. A2 has three more —
 * a Claude Code hook payload, a JSON-RPC message from an MCP client, and a
 * daemon response an adapter reads back — and every one of them arrives from
 * outside Atlas.
 *
 * Rather than let three files each reach for the vendored parser, this is the
 * one facade over it. yyjson is called from `src/ipc` and nowhere else, which
 * keeps the answer to "where does Atlas parse untrusted JSON?" a directory
 * rather than a grep.
 *
 * Everything here is read-only and borrowed. A value is valid exactly as long
 * as the document that produced it, which is stated rather than implied because
 * the alternative — copying every string out — would make a bounded parse into
 * an unbounded allocation.
 *
 * There is deliberately no coercion. A caller that asks for a string and finds
 * a number is told the member is absent, not given a rendering of the number:
 * guessing what a client meant is how a protocol grows behaviour nobody wrote
 * down.
 */
#ifndef ATLAS_JSONREAD_H
#define ATLAS_JSONREAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "atlas/buf.h"
#include "atlas/error.h"
#include "atlas/json.h"

typedef struct atlas_jsondoc atlas_jsondoc;
/* One value inside a parsed document. Borrowed; never freed by the caller. */
typedef struct atlas_jsonv atlas_jsonv;

/* Parses `len` bytes.
 *
 * `max_bytes` is checked before the parser is entered, so a claimed size can
 * never become an allocation, and `max_depth` is enforced by an iterative walk —
 * measuring a hostile document's depth by recursing into it is the stack
 * exhaustion the limit exists to prevent.
 *
 * Strict JSON only: no comments, no trailing commas, no NaN or Infinity.
 * Accepting extensions would let Atlas and a client disagree about what a
 * document says. */
atlas_status atlas_jsondoc_parse(const void *data, size_t len, size_t max_bytes,
                                 unsigned max_depth, atlas_jsondoc **out, atlas_err *err);
void atlas_jsondoc_free(atlas_jsondoc *d);
const atlas_jsonv *atlas_jsondoc_root(const atlas_jsondoc *d);

bool atlas_jsonv_is_obj(const atlas_jsonv *v);
bool atlas_jsonv_is_arr(const atlas_jsonv *v);
bool atlas_jsonv_is_str(const atlas_jsonv *v);
bool atlas_jsonv_is_int(const atlas_jsonv *v);
bool atlas_jsonv_is_bool(const atlas_jsonv *v);
bool atlas_jsonv_is_null(const atlas_jsonv *v);

/* Member lookup. NULL when `obj` is not an object or the member is absent. */
const atlas_jsonv *atlas_jsonv_get(const atlas_jsonv *obj, const char *key);
size_t atlas_jsonv_arr_len(const atlas_jsonv *arr);
const atlas_jsonv *atlas_jsonv_at(const atlas_jsonv *arr, size_t index);

/* Typed reads. `*len_out` may be NULL. A string containing an embedded NUL is
 * refused, because the C view of it would be shorter than the JSON view and the
 * two halves of Atlas would then disagree about the value. */
bool atlas_jsonv_str(const atlas_jsonv *v, const char **out, size_t *len_out);
bool atlas_jsonv_int(const atlas_jsonv *v, int64_t *out);
bool atlas_jsonv_bool(const atlas_jsonv *v, bool *out);

/* Re-emits `v` through the first-party streaming writer, member by member.
 *
 * Not a byte copy. Every string goes through the writer's escaping and every
 * number through its integer formatter, so a component forwarding a document it
 * parsed produces text Atlas escaped rather than text it trusted. That is what
 * lets there still be no "write these bytes as JSON" primitive anywhere in
 * Atlas — the one hole through which an unescaped value eventually reaches a
 * consumer.
 *
 * The walk is iterative and depth-bounded, for the same reason the parse is: a
 * subtree deeper than `max_depth` becomes null rather than being partly
 * emitted. */
atlas_status atlas_jsonv_write(const atlas_jsonv *v, atlas_json *j, unsigned max_depth,
                               atlas_err *err);

/* Convenience: the string member `key` of `obj`, or NULL. */
const char *atlas_jsonv_str_member(const atlas_jsonv *obj, const char *key);
/* Convenience: follows a two-level path, e.g. tool_input -> file_path. */
const char *atlas_jsonv_str_member2(const atlas_jsonv *obj, const char *k1, const char *k2);

/* Checks that every key in `obj` appears in `allowed` (a NULL-terminated list).
 * Returns ATLAS_OK when all keys are recognised. Returns ATLAS_ERR_USAGE and
 * sets `err` when a key is absent from the list.
 *
 * Call this in a run function to enforce `additionalProperties: false` at
 * runtime: the MCP layer publishes that constraint in the schema JSON but does
 * not validate it before calling run(). */
atlas_status atlas_jsonv_check_only_keys(const atlas_jsonv *obj,
                                         const char *const *allowed,
                                         atlas_err *err);

#endif /* ATLAS_JSONREAD_H */
