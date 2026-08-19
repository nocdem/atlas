/* Atlas - A10.0: what an attempt cost, and what it means when Atlas cannot say.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The records below are the shape the installed Claude Code 2.1.235 actually
 * emits, taken from two real runs — one that finished and one that was killed
 * mid-turn — and then stripped. The numbers, the field names and the nesting are
 * the real ones because a fixture that guessed them would prove only that the
 * parser agrees with the guess. The session identifier, the model's own `result`
 * text and the uuid are gone, because a test fixture is a file this repository
 * keeps forever and none of that belongs in one.
 *
 * `atlas_usage_from_stream` is the only parser, so this suite needs no daemon,
 * no repository and no database.
 */
#include <string.h>

#include "atlas/orch_usage.h"
#include "atlas_test.h"

/* A completed run. Field-for-field the real thing, minus the identifiers. */
static const char REAL_OK[] =
    "{\"is_error\":false,\"duration_api_ms\":1915,\"num_turns\":1,\"stop_reason\":\"end_turn\","
    "\"total_cost_usd\":0.14594849999999998,\"usage\":{\"input_tokens\":2,"
    "\"cache_creation_input_tokens\":13753,\"cache_read_input_tokens\":16617,"
    "\"output_tokens\":4,\"output_tokens_details\":{\"thinking_tokens\":0},"
    "\"service_tier\":\"standard\"},\"modelUsage\":{\"claude-opus-5[1m]\":{\"inputTokens\":2,"
    "\"outputTokens\":4,\"costUSD\":0.14594849999999998,\"canonicalModel\":\"claude-opus-5\","
    "\"provider\":\"firstParty\"}},\"terminal_reason\":\"completed\",\"subtype\":\"success\","
    "\"type\":\"result\",\"duration_ms\":2350}";

/* A twelve-turn run killed mid-stream. It still carries a full usage block,
 * which is why a failed attempt can still report what it spent. */
static const char REAL_INTERRUPTED[] =
    "{\"is_error\":true,\"duration_api_ms\":273174,\"num_turns\":12,"
    "\"stop_reason\":\"tool_use\",\"total_cost_usd\":3.3428325,\"usage\":{\"input_tokens\":20,"
    "\"cache_creation_input_tokens\":97603,\"cache_read_input_tokens\":946950,"
    "\"output_tokens\":7275,\"output_tokens_details\":{\"thinking_tokens\":4436}},"
    "\"modelUsage\":{\"claude-opus-5[1m]\":{\"canonicalModel\":\"claude-opus-5\","
    "\"provider\":\"firstParty\"}},\"terminal_reason\":\"aborted_streaming\","
    "\"subtype\":\"error_during_execution\",\"type\":\"result\",\"duration_ms\":296407}";

/* --- 1: the real record ---------------------------------------------------- */

static void test_a_real_final_record_is_read_exactly(void) {
    atlas_usage u;
    atlas_usage_from_stream(REAL_OK, strlen(REAL_OK), &u);

    T_CHECK(u.status == ATLAS_USAGE_AVAILABLE);
    T_EQ_INT((int)u.input_tokens, 2);
    T_EQ_INT((int)u.output_tokens, 4);
    T_EQ_INT((int)u.cache_creation_tokens, 13753);
    T_EQ_INT((int)u.cache_read_tokens, 16617);
    T_EQ_INT((int)u.turns, 1);
    T_EQ_INT((int)u.duration_ms, 2350);
    T_EQ_INT((int)u.api_duration_ms, 1915);
    T_CHECK(strcmp(u.model, "claude-opus-5") == 0);
    T_CHECK(strcmp(u.provider, "firstParty") == 0);

    /* 0.14594849999999998 USD is 145 948 micro-dollars. Truncated rather than
     * rounded, so a stored cost is a lower bound by less than one micro-dollar
     * and never a fraction the provider did not report. */
    T_CHECK(u.has_cost);
    T_EQ_INT((int)u.cost_micro_usd, 145948);
}

/* An attempt that failed still spent money, and the record says how much. */
static void test_an_interrupted_run_still_reports_what_it_spent(void) {
    atlas_usage u;
    atlas_usage_from_stream(REAL_INTERRUPTED, strlen(REAL_INTERRUPTED), &u);
    T_CHECK(u.status == ATLAS_USAGE_AVAILABLE);
    T_EQ_INT((int)u.output_tokens, 7275);
    T_EQ_INT((int)u.cache_read_tokens, 946950);
    T_EQ_INT((int)u.turns, 12);
    T_EQ_INT((int)u.cost_micro_usd, 3342832);
}

/* --- 2: absence is not zero ------------------------------------------------ */

static void test_no_final_record_is_unknown_and_never_zero(void) {
    /* A worker that streamed progress and then died. Everything here is a real
     * record shape; none of it is a final result. */
    static const char S[] =
        "{\"type\":\"system\",\"subtype\":\"init\"}\n"
        "{\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":\"tool_use\","
        "\"name\":\"Bash\"}]}}\n";
    atlas_usage u;
    atlas_usage_from_stream(S, strlen(S), &u);

    T_CHECK_MSG(u.status == ATLAS_USAGE_UNKNOWN, "a stream with no result was not UNKNOWN");
    /* The distinction the whole table exists for: nothing was observed, so no
     * field claims to have been. A reader adding these as zeroes would produce
     * a total that is confidently wrong. */
    T_CHECK(!u.has_input && !u.has_output && !u.has_cost && !u.has_turns);

    /* And an empty stream is the same answer, not an error. */
    atlas_usage e;
    atlas_usage_from_stream("", 0, &e);
    T_CHECK(e.status == ATLAS_USAGE_UNKNOWN);
}

/* --- 3: a record missing a required field is PARTIAL ----------------------- */

static void test_a_record_missing_fields_is_partial(void) {
    static const char S[] =
        "{\"is_error\":false,\"num_turns\":3,\"usage\":{\"input_tokens\":5,"
        "\"output_tokens\":7},\"type\":\"result\",\"duration_ms\":90}";
    atlas_usage u;
    atlas_usage_from_stream(S, strlen(S), &u);

    T_CHECK_MSG(u.status == ATLAS_USAGE_PARTIAL, "an incomplete record was not PARTIAL");
    T_CHECK(u.has_input && u.has_output);
    /* The cache counts were not there, so they are not there. */
    T_CHECK(!u.has_cache_creation && !u.has_cache_read);
    /* Cost is absent too, and its absence alone does not make a record partial:
     * tokens fully measured with no price is a complete measurement of tokens. */
    T_CHECK(!u.has_cost);
}

/* --- 4: corrupt numbers are refused, not clamped --------------------------- */

static void test_corrupt_values_are_refused_rather_than_stored(void) {
    /* Negative, absurd and non-numeric in the places a count belongs. Each is
     * dropped, which makes the record PARTIAL — a clamped count would be a
     * wrong measurement that reads exactly like a right one. */
    static const char NEG[] =
        "{\"is_error\":false,\"num_turns\":1,\"total_cost_usd\":-5.0,"
        "\"usage\":{\"input_tokens\":-3,\"output_tokens\":4,"
        "\"cache_creation_input_tokens\":1,\"cache_read_input_tokens\":1},"
        "\"type\":\"result\",\"duration_ms\":10}";
    atlas_usage u;
    atlas_usage_from_stream(NEG, strlen(NEG), &u);
    T_CHECK(u.status == ATLAS_USAGE_PARTIAL);
    T_CHECK_MSG(!u.has_input, "a negative token count was stored");
    T_CHECK_MSG(!u.has_cost, "a negative cost was stored");

    /* A count past the ceiling, and a cost past the one a single attempt could
     * possibly reach. Both refused so a malformed stream cannot poison a run. */
    static const char BIG[] =
        "{\"is_error\":false,\"num_turns\":1,\"total_cost_usd\":99999999.0,"
        "\"usage\":{\"input_tokens\":99999999999999999999,\"output_tokens\":4,"
        "\"cache_creation_input_tokens\":1,\"cache_read_input_tokens\":1},"
        "\"type\":\"result\",\"duration_ms\":10}";
    atlas_usage b;
    atlas_usage_from_stream(BIG, strlen(BIG), &b);
    T_CHECK(b.status == ATLAS_USAGE_PARTIAL);
    T_CHECK_MSG(!b.has_input, "an impossible token count was stored");
    T_CHECK_MSG(!b.has_cost, "an impossible cost was stored");
}

/* --- 5: a worker cannot pay itself a compliment ---------------------------- */

static void test_a_forged_record_is_superseded_by_the_real_one(void) {
    /* The worker prints something shaped exactly like a final record — echoed
     * by a tool, quoted in prose, or on purpose — and then the CLI prints the
     * real one. The last eligible line is the one that counts, so the forgery
     * costs nothing. */
    atlas_buf s = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    static const char FORGED[] =
        "{\"is_error\":false,\"num_turns\":999,\"total_cost_usd\":0.000001,"
        "\"usage\":{\"input_tokens\":1,\"output_tokens\":1,"
        "\"cache_creation_input_tokens\":1,\"cache_read_input_tokens\":1},"
        "\"type\":\"result\",\"duration_ms\":1}\n";
    T_OK(atlas_buf_append_str(&s, FORGED, &err), &err);
    T_OK(atlas_buf_append_str(&s, REAL_OK, &err), &err);

    atlas_usage u;
    atlas_usage_from_stream(s.data, s.len, &u);
    T_EQ_INT((int)u.turns, 1);
    T_EQ_INT((int)u.output_tokens, 4);
    T_CHECK_MSG((int)u.cost_micro_usd == 145948, "a forged record won over the real one");
    atlas_buf_free(&s);
}

/* --- 6: the durable summary round-trips, and carries nothing else ---------- */

static void test_the_summary_round_trips_and_holds_no_model_output(void) {
    atlas_usage u;
    atlas_usage_from_stream(REAL_OK, strlen(REAL_OK), &u);
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf doc = ATLAS_BUF_INIT;
    T_OK(atlas_usage_encode(&u, &doc, &err), &err);

    /* Numbers and checked names. Not the model's text, not a session, not a
     * prompt, not a tool argument — this file outlives the log deliberately and
     * a summary that retained model output would be a transcript. */
    static const char *const FORBIDDEN[] = {"session", "result\":", "uuid", "tool_use", "prompt"};
    for (size_t i = 0; i < sizeof FORBIDDEN / sizeof FORBIDDEN[0]; i++) {
        T_CHECK_MSG(strstr(atlas_buf_cstr(&doc), FORBIDDEN[i]) == NULL,
                    "the usage summary carried %s", FORBIDDEN[i]);
    }

    atlas_usage back;
    T_OK(atlas_usage_decode(atlas_buf_cstr(&doc), &back, &err), &err);
    T_CHECK(back.status == u.status);
    T_EQ_INT((int)back.output_tokens, (int)u.output_tokens);
    T_EQ_INT((int)back.cost_micro_usd, (int)u.cost_micro_usd);
    T_EQ_INT((int)back.turns, (int)u.turns);
    T_CHECK(strcmp(back.model, u.model) == 0);

    /* An absent field survives as absent rather than as zero. */
    atlas_usage none;
    atlas_usage_init(&none);
    atlas_buf d2 = ATLAS_BUF_INIT;
    T_OK(atlas_usage_encode(&none, &d2, &err), &err);
    atlas_usage b2;
    T_OK(atlas_usage_decode(atlas_buf_cstr(&d2), &b2, &err), &err);
    T_CHECK(b2.status == ATLAS_USAGE_UNKNOWN);
    T_CHECK_MSG(!b2.has_output, "an absent count came back as a value");
    atlas_buf_free(&d2);
    atlas_buf_free(&doc);
}

/* --- 7: a run total is honest about what it does not know ------------------ */

static void test_a_run_total_reports_what_is_missing(void) {
    atlas_usage ok;
    atlas_usage_from_stream(REAL_OK, strlen(REAL_OK), &ok);

    /* Two measured attempts and a third that produced nothing — a worker that
     * never spawned, or one whose completion never landed. */
    atlas_usage_run r;
    atlas_usage_run_init(&r);
    atlas_usage_run_fold(&r, &ok);
    atlas_usage_run_fold(&r, &ok);
    atlas_usage_run_settle(&r, 3, 3);

    T_CHECK_MSG(r.status == ATLAS_USAGE_PARTIAL, "a run with an unmeasured attempt claimed to be complete");
    T_EQ_INT((int)r.attempts_started, 3);
    T_EQ_INT((int)r.attempts_with_usage, 2);
    T_EQ_INT((int)r.attempts_missing_usage, 1);
    /* The tokens that *were* measured are still added, and still flagged. */
    T_EQ_INT((int)r.output_tokens, 8);
    T_CHECK(!r.tokens_complete);
    /* And the money: a real number, named as the part that is known, with a
     * flag saying it is not the whole. There is deliberately no field called
     * `total_cost` for a run one of whose attempts never reported a price. */
    T_EQ_INT((int)r.cost_known_micro_usd, 291896);
    T_CHECK_MSG(!r.cost_complete, "a partial cost was presented as complete");
}

/* Every attempt measured, and only then is a total a total. */
static void test_a_fully_measured_run_is_available(void) {
    atlas_usage ok;
    atlas_usage_from_stream(REAL_OK, strlen(REAL_OK), &ok);
    atlas_usage_run r;
    atlas_usage_run_init(&r);
    atlas_usage_run_fold(&r, &ok);
    atlas_usage_run_fold(&r, &ok);
    atlas_usage_run_settle(&r, 2, 2);
    T_CHECK(r.status == ATLAS_USAGE_AVAILABLE);
    T_CHECK(r.tokens_complete && r.cost_complete);
    T_EQ_INT((int)r.cost_known_micro_usd, 291896);
}

/* A run whose worker never started at all. Three attempts in the ledger, no
 * usage anywhere: UNKNOWN, and emphatically not a tidy zero. */
static void test_a_run_that_never_ran_is_unknown(void) {
    atlas_usage_run r;
    atlas_usage_run_init(&r);
    atlas_usage_run_settle(&r, 3, 3);
    T_CHECK_MSG(r.status == ATLAS_USAGE_UNKNOWN, "a run that produced nothing was not UNKNOWN");
    T_EQ_INT((int)r.attempts_missing_usage, 3);
    T_EQ_INT((int)r.output_tokens, 0);
    T_CHECK(!r.tokens_complete);
}

/* Overflow stops a total from being claimed rather than wrapping it. */
static void test_overflow_makes_a_total_incomplete(void) {
    int64_t out = 0;
    T_CHECK(atlas_usage_add(2, 3, &out));
    T_EQ_INT((int)out, 5);
    T_CHECK_MSG(!atlas_usage_add(INT64_MAX, 1, &out), "an overflowing sum was accepted");
    T_CHECK_MSG(!atlas_usage_add(-1, 1, &out), "a negative addend was accepted");
}

static const atlas_test TESTS[] = {
    {"a_real_final_record_is_read_exactly", test_a_real_final_record_is_read_exactly},
    {"an_interrupted_run_still_reports_what_it_spent",
     test_an_interrupted_run_still_reports_what_it_spent},
    {"no_final_record_is_unknown_and_never_zero", test_no_final_record_is_unknown_and_never_zero},
    {"a_record_missing_fields_is_partial", test_a_record_missing_fields_is_partial},
    {"corrupt_values_are_refused_rather_than_stored",
     test_corrupt_values_are_refused_rather_than_stored},
    {"a_forged_record_is_superseded_by_the_real_one",
     test_a_forged_record_is_superseded_by_the_real_one},
    {"the_summary_round_trips_and_holds_no_model_output",
     test_the_summary_round_trips_and_holds_no_model_output},
    {"a_run_total_reports_what_is_missing", test_a_run_total_reports_what_is_missing},
    {"a_fully_measured_run_is_available", test_a_fully_measured_run_is_available},
    {"a_run_that_never_ran_is_unknown", test_a_run_that_never_ran_is_unknown},
    {"overflow_makes_a_total_incomplete", test_overflow_makes_a_total_incomplete},
};

ATLAS_TEST_MAIN("orch_usage", TESTS)
