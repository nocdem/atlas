/* Atlas - A14R: the final record of a worker's stream.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A10.0 reads *what an attempt cost* out of the stream Atlas captured
 * (`atlas/orch_usage.h`). This reads the two other things that record carries:
 * **why the worker stopped**, from a closed vocabulary, and **what it answered**,
 * as bytes.
 *
 * ## Why this is not in `usage.c`
 *
 * That file's opening paragraph says what it is for — "Numbers and checked names
 * only. ... Anything else is dropped rather than stored" — and the final text is
 * neither a number nor a checked name. It is a model's prose: UNTRUSTED_DATA,
 * bounded, safe-encoded at every point it is displayed, and read by no branch
 * anywhere. Putting a prose extractor behind that sentence would make the
 * sentence false, and the sentence is the reason a reader can trust `usage.c`
 * without reading it. Two files, two contracts.
 *
 * ## What is trusted here, and what is not
 *
 * `subtype` is matched against a closed vocabulary and is UNKNOWN when it does
 * not match, exactly as `dispatch.complete` treats an `exit_kind` a dispatcher
 * sends. It is a *measurement*: it says what the CLI reported about its own
 * stopping, and Atlas' own classification of the process exit is what decides
 * the attempt's outcome. Nothing routes on the text — there is no `strstr` over
 * it anywhere, and adding one would end this argument.
 *
 * ## The last eligible line wins
 *
 * A worker's stdout can contain a line shaped exactly like a final record —
 * echoed by a tool, quoted in prose, or written on purpose. `usage.c` answers
 * this by taking the last one, and so does this: a forgery is only ever
 * superseded by the record the CLI actually ends with, and a worker that dies
 * right after emitting one has its outcome classified by Atlas from the process
 * exit regardless.
 */
#ifndef ATLAS_ORCH_JOBRESULT_H
#define ATLAS_ORCH_JOBRESULT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "atlas/atlas.h"
#include "atlas/buf.h"

/* Why the worker stopped, as the worker's own final record named it.
 *
 * A closed vocabulary. Every value the installed Claude Code emits in the
 * `subtype` field of a `"type":"result"` record and that Atlas has a use for is
 * here; anything else reads UNKNOWN rather than being reproduced, because a
 * worker must not write its own words into a field a reader treats as Atlas'.
 *
 * UNKNOWN is zero and means "the record did not say", never "it went fine". */
typedef enum atlas_orch_result_subtype {
    ATLAS_ORCH_RESULT_SUBTYPE_UNKNOWN = 0,
    ATLAS_ORCH_RESULT_SUBTYPE_SUCCESS,
    ATLAS_ORCH_RESULT_SUBTYPE_ERROR_MAX_BUDGET_USD,
    ATLAS_ORCH_RESULT_SUBTYPE_ERROR_MAX_TURNS,
    ATLAS_ORCH_RESULT_SUBTYPE_ERROR_DURING_EXECUTION
} atlas_orch_result_subtype;

const char *atlas_orch_result_subtype_name(atlas_orch_result_subtype s);

typedef struct atlas_orch_final {
    /* A final record was found at all. False for a worker killed at a bound
     * before it could write one — which is not the same as one that finished
     * with nothing to say, and the two must not be collapsed. */
    bool present;
    atlas_orch_result_subtype subtype;
    /* The decoded answer, at most `ATLAS_ORCH_RESULT_TEXT_MAX` bytes.
     * UNTRUSTED_DATA. Raw here; every reader safe-encodes at its own output. */
    atlas_buf text;
    /* True when `text` holds a prefix. A reader shown a prefix is told so. */
    bool text_truncated;
    /* The decoded length of the whole answer, counted past the bound without
     * storing it, so "how much was there" is an answer and not an estimate. */
    int64_t text_full_bytes;
} atlas_orch_final;

void atlas_orch_final_init(atlas_orch_final *f);
void atlas_orch_final_free(atlas_orch_final *f);

/* Scans a captured worker stream for its last `"type":"result"` line and fills
 * `out`. Returns non-OK only for a failure of Atlas itself (an allocation): a
 * stream with no final record leaves `present` false with ATLAS_OK, because an
 * absent measurement is an answer and not an error. */
atlas_status atlas_orch_final_from_stream(const char *stream, size_t len, atlas_orch_final *out,
                                          atlas_err *err);

/* Codex JSONL: last agent message and terminal turn envelope. Cost remains
 * unknown; no Claude result or usage vocabulary is applied to Codex output. */
atlas_status atlas_orch_final_from_codex_stream(const char *stream, size_t len,
                                                atlas_orch_final *out, atlas_err *err);

#endif /* ATLAS_ORCH_JOBRESULT_H */
