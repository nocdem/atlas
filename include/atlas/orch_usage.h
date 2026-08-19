/* Atlas - A10.0: what one worker attempt actually cost.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A10.1 is an A/B experiment, and an experiment whose control arm cannot report
 * its own cost is not one. A11.5a's accepted run had to have its token figures
 * recovered from the worker's session transcript under the operator's home
 * directory, which worked only because A8.1's model dispatcher borrows the
 * operator's login; a worker running as `atlas-worker` leaves no such file.
 * That was a coincidence, not a mechanism, and this is the mechanism.
 *
 * ## Where the numbers come from, and where they do not
 *
 * The canonical source is the **final `result` record** of a `stream-json` run
 * and nothing else. Its `usage` block is the attempt total already. Measured on
 * a real twelve-turn run: the final record reported 7 275 output tokens while
 * the per-message records in the same stream summed to 274 — they are streaming
 * snapshots, not addends, and summing them produces a number that is wrong in
 * both directions depending on where you stop. So they are never summed.
 * `iterations[]` and `modelUsage` restate the same totals and are read for the
 * model and provider *names* only, for the same reason: two sources for one
 * number is how double counting comes back.
 *
 * The final record is present even when the attempt failed. An interrupted run
 * measured here still carried `num_turns`, `total_cost_usd` and a full `usage`
 * block alongside `is_error: true` and `terminal_reason: aborted_streaming`,
 * which is why a failed attempt can still report what it spent.
 *
 * ## What the three states mean
 *
 * `UNKNOWN` is not zero and zero is not `UNKNOWN`. A worker that never produced
 * a final record spent something Atlas cannot name, and writing 0 there would
 * turn "we do not know" into "it was free" — the same mistake A9.2.2 refuses to
 * make about absence. Every count is optional and absent means absent.
 */
#ifndef ATLAS_ORCH_USAGE_H
#define ATLAS_ORCH_USAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "atlas/buf.h"
#include "atlas/error.h"

/* Whether the numbers below may be relied on. Atlas' classification of what it
 * observed, never something the worker asserts about itself. */
typedef enum atlas_usage_status {
    /* No usable final record. Nothing here is a measurement. Zero. */
    ATLAS_USAGE_UNKNOWN = 0,
    /* A final record arrived and some required field was missing or refused. */
    ATLAS_USAGE_PARTIAL,
    /* Every required field was present and within bounds. */
    ATLAS_USAGE_AVAILABLE,
} atlas_usage_status;

const char *atlas_usage_status_name(atlas_usage_status s);
bool atlas_usage_status_parse(const char *name, atlas_usage_status *out);

/* Costs are held as integer micro-USD. Provider cost arrives as a decimal
 * string; parsing it into a double and adding doubles across attempts is how a
 * total stops being reproducible, so it never becomes one. */
#define ATLAS_USAGE_COST_SCALE 1000000

/* A count no real attempt reaches. Anything at or above it is refused as
 * corrupt rather than clamped, because a clamped count is a wrong measurement
 * that looks like a right one. */
#define ATLAS_USAGE_COUNT_MAX ((int64_t)1 << 53)
/* Costs above this are corrupt too: a single attempt cannot cost a million
 * dollars, and accepting one would let a malformed stream poison a run total. */
#define ATLAS_USAGE_COST_MAX ((int64_t)1000000 * ATLAS_USAGE_COST_SCALE)

/* One attempt's measurement. Every numeric field is paired with a `has_` flag
 * rather than using a sentinel, so "absent" cannot be confused with a value. */
typedef struct atlas_usage {
    atlas_usage_status status;

    char provider[64];
    char model[128];

    bool has_input;
    int64_t input_tokens;
    bool has_output;
    int64_t output_tokens;
    bool has_cache_creation;
    int64_t cache_creation_tokens;
    bool has_cache_read;
    int64_t cache_read_tokens;

    /* Provider-reported only. Atlas never estimates a cost from token counts:
     * an estimate that looks like a measurement is worse than no measurement. */
    bool has_cost;
    int64_t cost_micro_usd;

    bool has_duration;
    int64_t duration_ms;
    bool has_api_duration;
    int64_t api_duration_ms;
    bool has_turns;
    int64_t turns;
} atlas_usage;

void atlas_usage_init(atlas_usage *u);

/* Reads the one final result record out of a captured `stream-json` stream.
 *
 * `stream` is the whole of what the worker wrote. Only a *complete* line that
 * closes with `}` and carries both `"type":"result"` and a `"usage":{` block is
 * eligible, and when several match the **last** one wins — a worker can print
 * anything it likes mid-stream, including a record shaped exactly like this one,
 * and the only thing that costs it is being superseded by the real final line.
 *
 * Never fails: a stream with no eligible record yields `ATLAS_USAGE_UNKNOWN`,
 * which is the honest answer and not an error. */
void atlas_usage_from_stream(const char *stream, size_t len, atlas_usage *out);

/* The per-attempt summary as it is written to disk before a completion is
 * offered, and read back after a restart. Small, bounded and free of anything
 * the model wrote: numbers and checked names only, never the `result` string,
 * a prompt, a tool argument or a session identifier. */
atlas_status atlas_usage_encode(const atlas_usage *u, atlas_buf *out, atlas_err *err);
atlas_status atlas_usage_decode(const char *text, atlas_usage *out, atlas_err *err);

/* What a whole run cost, derived from its attempts and never stored as a
 * mutable total.
 *
 * The denominator is **attempts started**, taken from the orchestration ledger,
 * not the number of usage rows that happen to exist. A run whose worker never
 * spawned has three started attempts and no usage at all, and it aggregates to
 * `UNKNOWN` — not to a tidy zero, and not to `AVAILABLE` over an empty set.
 *
 * `cost_complete` is why there is no field called `total_cost`. When one attempt
 * of four reported no price, the sum of the other three is a real number that
 * answers no question anybody asked; it is offered as the cost that is *known*,
 * beside a flag saying it is not the whole. */
typedef struct atlas_usage_run {
    atlas_usage_status status;
    int64_t attempts_started;
    int64_t attempts_with_usage;
    int64_t attempts_missing_usage;
    int64_t worker_starts;

    int64_t input_tokens;
    int64_t output_tokens;
    int64_t cache_creation_tokens;
    int64_t cache_read_tokens;
    bool tokens_complete;

    int64_t cost_known_micro_usd;
    bool has_any_cost;
    bool cost_complete;

    int64_t worker_duration_ms;
    int64_t turns;
} atlas_usage_run;

void atlas_usage_run_init(atlas_usage_run *r);

/* Folds one attempt into a run total. Overflow marks the total incomplete
 * rather than wrapping, because a wrapped count is a wrong number that reads
 * like a right one. */
void atlas_usage_run_fold(atlas_usage_run *r, const atlas_usage *u);

/* Settles the run's status from what was folded against what the ledger says
 * was started. Call once, after every attempt has been folded. */
void atlas_usage_run_settle(atlas_usage_run *r, int64_t attempts_started, int64_t worker_starts);

/* Checked addition for aggregation. Returns false on overflow rather than
 * wrapping, and a caller that sees false reports PARTIAL rather than a total. */
bool atlas_usage_add(int64_t a, int64_t b, int64_t *out);

#endif /* ATLAS_ORCH_USAGE_H */
