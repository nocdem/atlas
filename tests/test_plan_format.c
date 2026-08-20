/* Atlas - A12.0: the `atlas-plan-1` parser, its refusals and the five prompts.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Everything under test here is a pure function — untrusted bytes in, a
 * document or a refusal out; fixed inputs in, an exact byte string out — so
 * this suite needs no repository, no database and no daemon.
 *
 * The composer goldens are written out in full in this file rather than built
 * from the strings `src/orch/plan.c` uses. A golden assembled from the
 * implementation's own constants passes by agreeing with itself, and both
 * copies then drift together — which is exactly the failure the season's
 * "a drifted spec is a defect" rule exists to catch. The cost is that changing
 * a sentence in a prompt means changing it twice, deliberately.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/plan.h"
#include "atlas_test.h"

/* --- helpers --------------------------------------------------------------- */

/* Parses `text` (as C bytes) and reports the outcome plus the refusal line. */
static atlas_status parse_str(const char *text, int max_parallel, atlas_plan_doc *out,
                              int *line_out, atlas_err *err) {
    return atlas_plan_parse(text, strlen(text), max_parallel, out, line_out, err);
}

/* Asserts a refusal with ATLAS_ERR_USAGE and, when `want_line >= 0`, the exact
 * 1-based line the refusal is about. */
static void expect_refusal(const char *what, const char *text, int max_parallel, int want_line) {
    atlas_plan_doc doc;
    memset(&doc, 0, sizeof doc);
    atlas_err err;
    atlas_err_init(&err);
    int line = -1;
    atlas_status st = parse_str(text, max_parallel, &doc, &line, &err);
    T_CHECK_MSG(st == ATLAS_ERR_USAGE, "%s: expected a usage refusal, got %s", what,
                atlas_status_name(st));
    if (st != ATLAS_OK) {
        T_CHECK_MSG(atlas_err_msg(&err) != NULL && atlas_err_msg(&err)[0] != '\0',
                    "%s: a refusal must carry a sentence", what);
    }
    /* Never partially filled: the caller's zeroed document is untouched. */
    T_CHECK_MSG(doc.task_count == 0 && doc.stage_count == 0,
                "%s: a refused parse left something behind", what);
    if (want_line >= 0) {
        T_CHECK_MSG(line == want_line, "%s: expected line %d, got %d (%s)", what, want_line, line,
                    atlas_err_msg(&err));
    }
    atlas_plan_doc_free(&doc);
}

/* --- the vocabulary -------------------------------------------------------- */

static void test_unknown_is_the_zero_status(void) {
    /* A zeroed state struct reads UNKNOWN. Nothing stores it and nothing parses
     * it; it is what "nobody derived this" looks like. */
    T_EQ_INT((int)ATLAS_PLAN_STATUS_UNKNOWN, 0);
    T_EQ_STR(atlas_plan_status_name(ATLAS_PLAN_STATUS_UNKNOWN), "UNKNOWN");
    T_EQ_STR(atlas_plan_status_name(ATLAS_PLAN_STATUS_PLANNING), "PLANNING");
    T_EQ_STR(atlas_plan_status_name(ATLAS_PLAN_STATUS_EXECUTING), "EXECUTING");
    T_EQ_STR(atlas_plan_status_name(ATLAS_PLAN_STATUS_NEEDS_REPLAN), "NEEDS_REPLAN");
    T_EQ_STR(atlas_plan_status_name(ATLAS_PLAN_STATUS_COMPLETED), "COMPLETED");
    T_EQ_STR(atlas_plan_status_name(ATLAS_PLAN_STATUS_BLOCKED), "BLOCKED");
    /* A value nobody defined is not silently something else. */
    T_EQ_STR(atlas_plan_status_name((atlas_plan_status)99), "UNKNOWN");
}

static void test_bounds_are_what_the_season_froze(void) {
    T_EQ_INT(ATLAS_PLAN_MAX_BYTES, 65536);
    T_EQ_INT(ATLAS_PLAN_MAX_TASKS, 8);
    T_EQ_INT(ATLAS_PLAN_MAX_STAGES, 4);
    T_EQ_INT(ATLAS_PLAN_MAX_SIDE_PER_STAGE, 3);
    T_EQ_INT(ATLAS_PLAN_TASK_PROMPT_MAX, 16384);
    T_EQ_INT(ATLAS_PLAN_TITLE_MAX, 200);
    T_EQ_INT(ATLAS_PLAN_MAX_REVISIONS, 3);
    T_EQ_INT(ATLAS_PLAN_MAX_PLANNER_JOBS, 5);
    T_EQ_INT(ATLAS_PLAN_GOAL_MAX, 16384);
    T_EQ_INT(ATLAS_PLAN_MAX_LINE, 4096);
    T_EQ_STR(ATLAS_PLAN_ARTIFACT_NAME, "plan.atlas-plan");
}

/* --- the happy path -------------------------------------------------------- */

static const char VALID_PLAN[] =
    "atlas-plan-1\n"
    "stage: 1\n"
    "task: one\n"
    "kind: tree\n"
    "title: The first task\n"
    "gate: make test\n"
    "prompt<<\n"
    "do the thing\n"
    ">>\n"
    "task: helper\n"
    "kind: side\n"
    "title: The sibling\n"
    "prompt<<\n"
    "gather facts\n"
    ">>\n"
    "stage: 2\n"
    "task: two\n"
    "kind: tree\n"
    "title: The second task\n"
    "prompt<<\n"
    "finish\n"
    ">>\n";

static void test_a_two_stage_plan_round_trips(void) {
    atlas_plan_doc doc;
    memset(&doc, 0, sizeof doc);
    atlas_err err;
    atlas_err_init(&err);
    int line = -1;
    T_OK(parse_str(VALID_PLAN, 2, &doc, &line, &err), &err);

    T_EQ_INT(doc.stage_count, 2);
    T_EQ_INT((int)doc.task_count, 3);

    T_EQ_STR(doc.tasks[0].key, "one");
    T_EQ_INT(doc.tasks[0].stage_no, 1);
    T_CHECK(doc.tasks[0].is_tree);
    T_EQ_STR(doc.tasks[0].title, "The first task");
    T_EQ_STR(atlas_buf_cstr(&doc.tasks[0].prompt), "do the thing\n");
    T_EQ_INT((int)doc.tasks[0].gate_count, 1);
    T_EQ_INT((int)doc.tasks[0].gates[0].count, 2);
    T_EQ_STR(atlas_buf_cstr(&doc.tasks[0].gates[0].args[0]), "make");
    T_EQ_STR(atlas_buf_cstr(&doc.tasks[0].gates[0].args[1]), "test");

    T_EQ_STR(doc.tasks[1].key, "helper");
    T_EQ_INT(doc.tasks[1].stage_no, 1);
    T_CHECK(!doc.tasks[1].is_tree);
    T_EQ_STR(doc.tasks[1].title, "The sibling");
    T_EQ_STR(atlas_buf_cstr(&doc.tasks[1].prompt), "gather facts\n");
    T_EQ_INT((int)doc.tasks[1].gate_count, 0);

    T_EQ_STR(doc.tasks[2].key, "two");
    T_EQ_INT(doc.tasks[2].stage_no, 2);
    T_CHECK(doc.tasks[2].is_tree);
    T_EQ_STR(atlas_buf_cstr(&doc.tasks[2].prompt), "finish\n");

    /* Free twice: idempotent, and the second call must not double-free. */
    atlas_plan_doc_free(&doc);
    atlas_plan_doc_free(&doc);
    T_EQ_INT((int)doc.task_count, 0);
    atlas_plan_doc_free(NULL);
}

static void test_a_document_with_no_trailing_newline_parses(void) {
    static const char TEXT[] =
        "atlas-plan-1\n"
        "stage: 1\n"
        "task: only\n"
        "kind: tree\n"
        "title: t\n"
        "prompt<<\n"
        "p\n"
        ">>";
    atlas_plan_doc doc;
    memset(&doc, 0, sizeof doc);
    atlas_err err;
    atlas_err_init(&err);
    T_OK(parse_str(TEXT, 1, &doc, NULL, &err), &err);
    T_EQ_INT((int)doc.task_count, 1);
    atlas_plan_doc_free(&doc);
}

static void test_crlf_is_tolerated(void) {
    static const char TEXT[] =
        "atlas-plan-1\r\n"
        "stage: 1\r\n"
        "task: only\r\n"
        "kind: tree\r\n"
        "title: t\r\n"
        "prompt<<\r\n"
        "p\r\n"
        ">>\r\n";
    atlas_plan_doc doc;
    memset(&doc, 0, sizeof doc);
    atlas_err err;
    atlas_err_init(&err);
    T_OK(parse_str(TEXT, 1, &doc, NULL, &err), &err);
    T_EQ_INT((int)doc.task_count, 1);
    T_EQ_STR(doc.tasks[0].title, "t");
    /* Exactly one carriage return is stripped per line, so the prompt body is
     * the line's own bytes plus one newline. */
    T_EQ_STR(atlas_buf_cstr(&doc.tasks[0].prompt), "p\n");
    atlas_plan_doc_free(&doc);
}

static void test_a_prompt_may_hold_bytes_that_are_not_utf8(void) {
    /* A prompt is never decoded as text by Atlas. Invalid UTF-8 is content. */
    static const char TEXT[] =
        "atlas-plan-1\n"
        "stage: 1\n"
        "task: only\n"
        "kind: tree\n"
        "title: t\n"
        "prompt<<\n"
        "\xff\xfe not utf-8 \x80\n"
        ">>\n";
    atlas_plan_doc doc;
    memset(&doc, 0, sizeof doc);
    atlas_err err;
    atlas_err_init(&err);
    T_OK(parse_str(TEXT, 1, &doc, NULL, &err), &err);
    T_EQ_MEM(doc.tasks[0].prompt.data, doc.tasks[0].prompt.len, "\xff\xfe not utf-8 \x80\n", 15u);
    atlas_plan_doc_free(&doc);
}

static void test_a_terminator_with_trailing_space_is_prompt_content(void) {
    static const char TEXT[] =
        "atlas-plan-1\n"
        "stage: 1\n"
        "task: only\n"
        "kind: tree\n"
        "title: t\n"
        "prompt<<\n"
        ">> \n"
        "real body\n"
        ">>\n";
    atlas_plan_doc doc;
    memset(&doc, 0, sizeof doc);
    atlas_err err;
    atlas_err_init(&err);
    T_OK(parse_str(TEXT, 1, &doc, NULL, &err), &err);
    T_EQ_STR(atlas_buf_cstr(&doc.tasks[0].prompt), ">> \nreal body\n");
    atlas_plan_doc_free(&doc);
}

static void test_the_maximum_shape_is_accepted(void) {
    /* Four stages, eight tasks, three siblings in the first stage: every bound
     * reached and none exceeded. A bound that refuses its own maximum is a
     * bound off by one, and nothing else would notice. */
    static const char TEXT[] =
        "atlas-plan-1\n"
        "stage: 1\n"
        "task: t1\n"
        "kind: tree\n"
        "title: a\n"
        "prompt<<\n"
        "x\n"
        ">>\n"
        "task: s1\n"
        "kind: side\n"
        "title: a\n"
        "prompt<<\n"
        "x\n"
        ">>\n"
        "task: s2\n"
        "kind: side\n"
        "title: a\n"
        "prompt<<\n"
        "x\n"
        ">>\n"
        "task: s3\n"
        "kind: side\n"
        "title: a\n"
        "prompt<<\n"
        "x\n"
        ">>\n"
        "stage: 2\n"
        "task: t2\n"
        "kind: tree\n"
        "title: a\n"
        "prompt<<\n"
        "x\n"
        ">>\n"
        "stage: 3\n"
        "task: t3\n"
        "kind: tree\n"
        "title: a\n"
        "prompt<<\n"
        "x\n"
        ">>\n"
        "stage: 4\n"
        "task: t4\n"
        "kind: tree\n"
        "title: a\n"
        "prompt<<\n"
        "x\n"
        ">>\n";
    atlas_plan_doc doc;
    memset(&doc, 0, sizeof doc);
    atlas_err err;
    atlas_err_init(&err);
    T_OK(parse_str(TEXT, 4, &doc, NULL, &err), &err);
    T_EQ_INT(doc.stage_count, 4);
    T_EQ_INT((int)doc.task_count, 7);
    atlas_plan_doc_free(&doc);
}

/* Builds a document with `n` `gate: true` additions on its single tree task. */
static void gates_doc(int n, atlas_buf *out, atlas_err *err) {
    T_OK(atlas_buf_set_str(out, "atlas-plan-1\nstage: 1\ntask: only\nkind: tree\ntitle: t\n", err),
         err);
    for (int i = 0; i < n; i++) {
        T_OK(atlas_buf_append_str(out, "gate: true\n", err), err);
    }
    T_OK(atlas_buf_append_str(out, "prompt<<\nx\n>>\n", err), err);
}

static void test_eight_gate_additions_are_accepted_and_nine_are_not(void) {
    atlas_buf text = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    gates_doc(8, &text, &err);
    atlas_plan_doc doc;
    memset(&doc, 0, sizeof doc);
    T_OK(parse_str(atlas_buf_cstr(&text), 1, &doc, NULL, &err), &err);
    T_EQ_INT((int)doc.tasks[0].gate_count, 8);
    atlas_plan_doc_free(&doc);

    atlas_buf_reset(&text);
    gates_doc(9, &text, &err);
    /* The ninth gate line is line 14: five head lines plus nine gates. */
    expect_refusal("a ninth gate addition", atlas_buf_cstr(&text), 1, 14);
    atlas_buf_free(&text);
}

/* --- the refusals ---------------------------------------------------------- */

static void test_header_refusals(void) {
    expect_refusal("an empty document", "", 1, 0);
    expect_refusal("a wrong header", "atlas-plan-2\nstage: 1\n", 1, 1);
    expect_refusal("a header with trailing text", "atlas-plan-1 please\n", 1, 1);
    expect_refusal("a header and nothing else", "atlas-plan-1\n", 1, 0);
    expect_refusal("a document that is only a header line", "atlas-plan-1", 1, 0);
}

static void test_stage_refusals(void) {
    expect_refusal("a plan that does not open with stage 1",
                   "atlas-plan-1\n"
                   "task: a\n",
                   1, 2);
    expect_refusal("a plan starting at stage 2",
                   "atlas-plan-1\n"
                   "stage: 2\n",
                   1, 2);
    expect_refusal("a gap in the stage numbering",
                   "atlas-plan-1\n"
                   "stage: 1\n"
                   "task: a\n"
                   "kind: tree\n"
                   "title: t\n"
                   "prompt<<\n"
                   "x\n"
                   ">>\n"
                   "stage: 3\n",
                   1, 9);
    expect_refusal("a repeated stage number",
                   "atlas-plan-1\n"
                   "stage: 1\n"
                   "task: a\n"
                   "kind: tree\n"
                   "title: t\n"
                   "prompt<<\n"
                   "x\n"
                   ">>\n"
                   "stage: 1\n",
                   1, 9);
    expect_refusal("a fifth stage",
                   "atlas-plan-1\n"
                   "stage: 1\n"
                   "task: a\n"
                   "kind: tree\n"
                   "title: t\n"
                   "prompt<<\n"
                   "x\n"
                   ">>\n"
                   "stage: 2\n"
                   "task: b\n"
                   "kind: tree\n"
                   "title: t\n"
                   "prompt<<\n"
                   "x\n"
                   ">>\n"
                   "stage: 3\n"
                   "task: c\n"
                   "kind: tree\n"
                   "title: t\n"
                   "prompt<<\n"
                   "x\n"
                   ">>\n"
                   "stage: 4\n"
                   "task: d\n"
                   "kind: tree\n"
                   "title: t\n"
                   "prompt<<\n"
                   "x\n"
                   ">>\n"
                   "stage: 5\n",
                   1, 30);
    expect_refusal("a stage number that is not a number", "atlas-plan-1\nstage: one\n", 1, 2);
}

static void test_tree_task_count_refusals(void) {
    expect_refusal("two tree tasks in one stage",
                   "atlas-plan-1\n"
                   "stage: 1\n"
                   "task: a\n"
                   "kind: tree\n"
                   "title: t\n"
                   "prompt<<\n"
                   "x\n"
                   ">>\n"
                   "task: b\n"
                   "kind: tree\n",
                   2, 10);
    expect_refusal("a stage with no tree task",
                   "atlas-plan-1\n"
                   "stage: 1\n"
                   "task: a\n"
                   "kind: side\n"
                   "title: t\n"
                   "prompt<<\n"
                   "x\n"
                   ">>\n",
                   2, 2);
}

static void test_side_task_refusals(void) {
    static const char TWO_SIDES[] =
        "atlas-plan-1\n"
        "stage: 1\n"
        "task: a\n"
        "kind: tree\n"
        "title: t\n"
        "prompt<<\n"
        "x\n"
        ">>\n"
        "task: b\n"
        "kind: side\n"
        "title: t\n"
        "prompt<<\n"
        "x\n"
        ">>\n"
        "task: c\n"
        "kind: side\n"
        "title: t\n"
        "prompt<<\n"
        "x\n"
        ">>\n";
    /* max_parallel 3 admits two siblings; max_parallel 2 admits one, and the
     * refusal names the `kind: side` line that asked for the second. */
    atlas_plan_doc doc;
    memset(&doc, 0, sizeof doc);
    atlas_err err;
    atlas_err_init(&err);
    T_OK(parse_str(TWO_SIDES, 3, &doc, NULL, &err), &err);
    T_EQ_INT((int)doc.task_count, 3);
    atlas_plan_doc_free(&doc);

    expect_refusal("a second sibling under max_parallel 2", TWO_SIDES, 2, 16);
    expect_refusal("any sibling under max_parallel 1", TWO_SIDES, 1, 10);

    expect_refusal("a gate on a side task",
                   "atlas-plan-1\n"
                   "stage: 1\n"
                   "task: a\n"
                   "kind: tree\n"
                   "title: t\n"
                   "prompt<<\n"
                   "x\n"
                   ">>\n"
                   "task: b\n"
                   "kind: side\n"
                   "gate: make\n",
                   2, 11);
}

static void test_task_field_refusals(void) {
    expect_refusal("a title before its task",
                   "atlas-plan-1\n"
                   "stage: 1\n"
                   "title: t\n",
                   1, 3);
    expect_refusal("a gate before its task",
                   "atlas-plan-1\n"
                   "stage: 1\n"
                   "gate: make\n",
                   1, 3);
    expect_refusal("a prompt before its task",
                   "atlas-plan-1\n"
                   "stage: 1\n"
                   "prompt<<\n",
                   1, 3);
    expect_refusal("a gate before the kind is known",
                   "atlas-plan-1\n"
                   "stage: 1\n"
                   "task: a\n"
                   "gate: make\n",
                   1, 4);
    expect_refusal("a task outside any stage",
                   "atlas-plan-1\n"
                   "task: a\n",
                   1, 2);
    expect_refusal("a repeated kind",
                   "atlas-plan-1\n"
                   "stage: 1\n"
                   "task: a\n"
                   "kind: tree\n"
                   "kind: side\n",
                   2, 5);
    expect_refusal("a repeated title",
                   "atlas-plan-1\n"
                   "stage: 1\n"
                   "task: a\n"
                   "kind: tree\n"
                   "title: t\n"
                   "title: u\n",
                   1, 6);
    expect_refusal("a repeated prompt",
                   "atlas-plan-1\n"
                   "stage: 1\n"
                   "task: a\n"
                   "kind: tree\n"
                   "title: t\n"
                   "prompt<<\n"
                   "x\n"
                   ">>\n"
                   "prompt<<\n",
                   1, 9);
    expect_refusal("a task with no kind",
                   "atlas-plan-1\n"
                   "stage: 1\n"
                   "task: a\n"
                   "title: t\n"
                   "prompt<<\n"
                   "x\n"
                   ">>\n",
                   1, 3);
    expect_refusal("a task with no title",
                   "atlas-plan-1\n"
                   "stage: 1\n"
                   "task: a\n"
                   "kind: tree\n"
                   "prompt<<\n"
                   "x\n"
                   ">>\n",
                   1, 3);
    expect_refusal("a task with no prompt",
                   "atlas-plan-1\n"
                   "stage: 1\n"
                   "task: a\n"
                   "kind: tree\n"
                   "title: t\n",
                   1, 3);
    expect_refusal("an unknown kind",
                   "atlas-plan-1\n"
                   "stage: 1\n"
                   "task: a\n"
                   "kind: root\n",
                   1, 4);
    expect_refusal("an empty title",
                   "atlas-plan-1\n"
                   "stage: 1\n"
                   "task: a\n"
                   "kind: tree\n"
                   "title: \n",
                   1, 5);
}

static void test_key_refusals(void) {
    expect_refusal("a duplicate key",
                   "atlas-plan-1\n"
                   "stage: 1\n"
                   "task: a\n"
                   "kind: tree\n"
                   "title: t\n"
                   "prompt<<\n"
                   "x\n"
                   ">>\n"
                   "stage: 2\n"
                   "task: a\n",
                   1, 10);
    expect_refusal("an upper-case key",
                   "atlas-plan-1\nstage: 1\ntask: Alpha\n", 1, 3);
    expect_refusal("a key with an underscore",
                   "atlas-plan-1\nstage: 1\ntask: a_b\n", 1, 3);
    expect_refusal("a key with a colon",
                   "atlas-plan-1\nstage: 1\ntask: a:b\n", 1, 3);
    expect_refusal("an empty key", "atlas-plan-1\nstage: 1\ntask: \n", 1, 3);
    expect_refusal("a key of 33 characters",
                   "atlas-plan-1\nstage: 1\ntask: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n", 1, 3);
    /* Thirty-two is the bound, and the bound is accepted. */
    atlas_plan_doc doc;
    memset(&doc, 0, sizeof doc);
    atlas_err err;
    atlas_err_init(&err);
    T_OK(parse_str("atlas-plan-1\n"
                   "stage: 1\n"
                   "task: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
                   "kind: tree\n"
                   "title: t\n"
                   "prompt<<\n"
                   "x\n"
                   ">>\n",
                   1, &doc, NULL, &err),
         &err);
    T_EQ_INT((int)strlen(doc.tasks[0].key), 32);
    atlas_plan_doc_free(&doc);
}

static void test_task_count_refusal(void) {
    /* Four stages, two tasks each, is eight — the bound. A ninth is refused at
     * the `task:` line that asked for it. */
    atlas_buf text = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_buf_set_str(&text, "atlas-plan-1\n", &err), &err);
    for (int s = 1; s <= 4; s++) {
        T_OK(atlas_buf_appendf(&text, &err,
                               "stage: %d\n"
                               "task: t%d\nkind: tree\ntitle: t\nprompt<<\nx\n>>\n"
                               "task: s%d\nkind: side\ntitle: t\nprompt<<\nx\n>>\n",
                               s, s, s),
             &err);
    }
    atlas_plan_doc doc;
    memset(&doc, 0, sizeof doc);
    T_OK(parse_str(atlas_buf_cstr(&text), 2, &doc, NULL, &err), &err);
    T_EQ_INT((int)doc.task_count, 8);
    atlas_plan_doc_free(&doc);

    /* 1 header + 4 stages of 13 lines each = 53; the ninth task is line 54. */
    T_OK(atlas_buf_append_str(&text, "task: extra\n", &err), &err);
    expect_refusal("a ninth task", atlas_buf_cstr(&text), 2, 54);
    atlas_buf_free(&text);
}

static void test_size_and_byte_refusals(void) {
    /* A document one byte over the ceiling is refused about the document, not
     * about a line. */
    size_t n = (size_t)ATLAS_PLAN_MAX_BYTES + 1u;
    char *big = malloc(n + 1u);
    T_REQUIRE(big != NULL);
    memset(big, 'a', n);
    big[n] = '\0';
    atlas_plan_doc doc;
    memset(&doc, 0, sizeof doc);
    atlas_err err;
    atlas_err_init(&err);
    int line = -1;
    T_FAILS_WITH(atlas_plan_parse(big, n, 1, &doc, &line, &err), ATLAS_ERR_USAGE, &err);
    T_EQ_INT(line, 0);
    free(big);
    atlas_plan_doc_free(&doc);

    /* An over-long line is refused naming the line. */
    atlas_buf longline = ATLAS_BUF_INIT;
    T_OK(atlas_buf_set_str(&longline, "atlas-plan-1\nstage: 1\ntitle: ", &err), &err);
    for (size_t i = 0; i <= (size_t)ATLAS_PLAN_MAX_LINE; i++) {
        T_OK(atlas_buf_append_ch(&longline, 'a', &err), &err);
    }
    T_OK(atlas_buf_append_ch(&longline, '\n', &err), &err);
    expect_refusal("a line over the line bound", atlas_buf_cstr(&longline), 1, 3);
    atlas_buf_free(&longline);

    /* An embedded NUL is refused, naming the line it is on. */
    static const char WITH_NUL[] = "atlas-plan-1\nstage: 1\ntask: a\0b\nkind: tree\n";
    memset(&doc, 0, sizeof doc);
    atlas_err_init(&err);
    line = -1;
    T_FAILS_WITH(atlas_plan_parse(WITH_NUL, sizeof WITH_NUL - 1u, 1, &doc, &line, &err),
                 ATLAS_ERR_USAGE, &err);
    T_EQ_INT(line, 3);
    atlas_plan_doc_free(&doc);
}

static void append_run(atlas_buf *b, char c, size_t n, atlas_err *err) {
    for (size_t i = 0; i < n; i++) {
        T_OK(atlas_buf_append_ch(b, c, err), err);
    }
}

static void test_prompt_and_title_length_refusals(void) {
    atlas_buf text = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);

    T_OK(atlas_buf_set_str(&text, "atlas-plan-1\nstage: 1\ntask: a\nkind: tree\ntitle: ", &err),
         &err);
    append_run(&text, 'x', (size_t)ATLAS_PLAN_TITLE_MAX + 1u, &err);
    T_OK(atlas_buf_append_ch(&text, '\n', &err), &err);
    expect_refusal("a title one byte over the bound", atlas_buf_cstr(&text), 1, 5);

    /* Two hundred bytes exactly is the bound, and the bound is accepted. */
    atlas_buf_reset(&text);
    T_OK(atlas_buf_set_str(&text, "atlas-plan-1\nstage: 1\ntask: a\nkind: tree\ntitle: ", &err),
         &err);
    append_run(&text, 'x', (size_t)ATLAS_PLAN_TITLE_MAX, &err);
    T_OK(atlas_buf_append_str(&text, "\nprompt<<\nx\n>>\n", &err), &err);
    atlas_plan_doc doc;
    memset(&doc, 0, sizeof doc);
    T_OK(parse_str(atlas_buf_cstr(&text), 1, &doc, NULL, &err), &err);
    T_EQ_INT((int)strlen(doc.tasks[0].title), ATLAS_PLAN_TITLE_MAX);
    atlas_plan_doc_free(&doc);

    /* A prompt over the bound is refused at the body line that crossed it:
     * seventeen 1001-byte lines is 17 017, and the bound is 16 384. */
    atlas_buf_reset(&text);
    T_OK(atlas_buf_set_str(&text,
                           "atlas-plan-1\nstage: 1\ntask: a\nkind: tree\ntitle: t\nprompt<<\n",
                           &err),
         &err);
    for (int i = 0; i < 20; i++) {
        append_run(&text, 'y', 1000u, &err);
        T_OK(atlas_buf_append_ch(&text, '\n', &err), &err);
    }
    T_OK(atlas_buf_append_str(&text, ">>\n", &err), &err);
    expect_refusal("a prompt over the bound", atlas_buf_cstr(&text), 1, 6 + 17);

    /* An unterminated heredoc names the line that opened it. */
    atlas_buf_reset(&text);
    T_OK(atlas_buf_set_str(&text,
                           "atlas-plan-1\nstage: 1\ntask: a\nkind: tree\ntitle: t\nprompt<<\n"
                           "body\n",
                           &err),
         &err);
    expect_refusal("an unterminated heredoc", atlas_buf_cstr(&text), 1, 6);
    atlas_buf_free(&text);
}

static void test_unrecognised_line_refusals(void) {
    expect_refusal("a blank line outside a heredoc",
                   "atlas-plan-1\n"
                   "\n"
                   "stage: 1\n",
                   1, 2);
    expect_refusal("an unknown field",
                   "atlas-plan-1\n"
                   "stage: 1\n"
                   "notes: hello\n",
                   1, 3);
    expect_refusal("a field with no space after the colon",
                   "atlas-plan-1\n"
                   "stage:1\n",
                   1, 2);
    expect_refusal("a heredoc opener with trailing text",
                   "atlas-plan-1\n"
                   "stage: 1\n"
                   "task: a\n"
                   "kind: tree\n"
                   "title: t\n"
                   "prompt<< now\n",
                   1, 6);
}

static void test_gate_allowlist_refusals(void) {
    expect_refusal("a gate program that is not on the allowlist",
                   "atlas-plan-1\n"
                   "stage: 1\n"
                   "task: a\n"
                   "kind: tree\n"
                   "gate: rm -rf /\n",
                   1, 5);
    expect_refusal("a gate that is a shell",
                   "atlas-plan-1\n"
                   "stage: 1\n"
                   "task: a\n"
                   "kind: tree\n"
                   "gate: sh -c make\n",
                   1, 5);
    expect_refusal("an empty gate",
                   "atlas-plan-1\n"
                   "stage: 1\n"
                   "task: a\n"
                   "kind: tree\n"
                   "gate: \n",
                   1, 5);
    /* The split is on ASCII spaces and tabs, with no shell anywhere: a gate
     * that reads like a pipeline is a program named `make` with the rest as
     * arguments, and it is the allowlist that decides, not the punctuation. */
    atlas_plan_doc doc;
    memset(&doc, 0, sizeof doc);
    atlas_err err;
    atlas_err_init(&err);
    T_OK(parse_str("atlas-plan-1\n"
                   "stage: 1\n"
                   "task: a\n"
                   "kind: tree\n"
                   "title: t\n"
                   "gate: make\ttest ARGS=-j2\n"
                   "prompt<<\n"
                   "x\n"
                   ">>\n",
                   1, &doc, NULL, &err),
         &err);
    T_EQ_INT((int)doc.tasks[0].gates[0].count, 3);
    T_EQ_STR(atlas_buf_cstr(&doc.tasks[0].gates[0].args[0]), "make");
    T_EQ_STR(atlas_buf_cstr(&doc.tasks[0].gates[0].args[1]), "test");
    T_EQ_STR(atlas_buf_cstr(&doc.tasks[0].gates[0].args[2]), "ARGS=-j2");
    atlas_plan_doc_free(&doc);
}

static void test_max_parallel_is_refused_not_resolved(void) {
    expect_refusal("max_parallel below one", VALID_PLAN, 0, 0);
    expect_refusal("a negative max_parallel", VALID_PLAN, -1, 0);
}

static void test_a_null_input_is_refused(void) {
    atlas_plan_doc doc;
    memset(&doc, 0, sizeof doc);
    atlas_err err;
    atlas_err_init(&err);
    int line = -1;
    T_FAILS_WITH(atlas_plan_parse(NULL, 0, 1, &doc, &line, &err), ATLAS_ERR_USAGE, &err);
    T_EQ_INT(line, 0);
    atlas_plan_doc_free(&doc);
}

/* --- the composers --------------------------------------------------------
 *
 * Independent copies of every fixed sentence. See the file header for why.
 */

static const char G_SPEC[] =
    "plan-format: atlas-plan-1\n"
    "\n"
    "The document is line-based. Lines are at most 4096 bytes. One trailing\n"
    "carriage return per line is stripped. UTF-8 is not assumed. Any line the\n"
    "parser does not recognise is a refusal naming the line number.\n"
    "\n"
    "atlas-plan-1\n"
    "stage: 1\n"
    "task: <key>            # [a-z0-9-]{1,32}, unique across the whole plan\n"
    "kind: tree             # exactly one tree task per stage\n"
    "title: <one line, at most 200 bytes>\n"
    "gate: <cmd>            # tree only, 0..n; appended AFTER the operator floor;\n"
    "                       # same parsing as --gate (space-split argv, allowlist\n"
    "                       # make/ctest/cmake/true/false)\n"
    "prompt<<\n"
    "<free text for the executor, at most 16384 bytes>\n"
    ">>\n"
    "task: <key2>\n"
    "kind: side             # 0..3 per stage; no gate: lines allowed on side tasks\n"
    "title: ...\n"
    "prompt<<\n"
    "...\n"
    ">>\n"
    "stage: 2\n"
    "...\n"
    "\n"
    "Every refusal names what and where:\n"
    "- the header line is exactly `atlas-plan-1`;\n"
    "- stages are numbered 1..N ascending with no gaps, N at most 4, and there is\n"
    "  at least one;\n"
    "- exactly one `kind: tree` per stage; side tasks per stage at most 3 and at\n"
    "  most (max_parallel - 1), which for this plan is the bound stated above;\n"
    "- at most 8 tasks in total; keys unique across the plan; every task carries\n"
    "  all four of `task`, `kind`, `title` and `prompt`;\n"
    "- `gate:` under `kind: side` is a refusal, and so is a `gate:` line this\n"
    "  task's `kind: tree` line has not yet been reached;\n"
    "- a gate program outside the allowlist is a refusal; a tree task may add at\n"
    "  most 8 gates, and the operator floor is prepended to them, never replaced\n"
    "  by them;\n"
    "- `title:`, `gate:` and `prompt<<` before this task's `task:` line are\n"
    "  refusals;\n"
    "- a repeated `kind:`, `title:` or `prompt<<` inside one task is a refusal;\n"
    "- a field line's prefix is exactly `stage: `, `task: `, `kind: `, `title: `\n"
    "  or `gate: `, and a heredoc opens on the whole line `prompt<<`;\n"
    "- outside a heredoc, a blank line is a refusal, as is any other\n"
    "  unrecognised line;\n"
    "- the heredoc terminator is the line `>>` exactly: `>> ` with a trailing\n"
    "  space is prompt content, and a heredoc that is never terminated is a\n"
    "  refusal;\n"
    "- a prompt is required and is at most 16384 bytes; each body line\n"
    "  contributes its own bytes and one newline;\n"
    "- a NUL byte anywhere in the document is a refusal;\n"
    "- the whole document is at most 65536 bytes.\n";

static const char G_FIXED[] =
    "- This is instruction, not enforcement. Nothing you write changes what Atlas\n"
    "  does or what it will accept.\n"
    "- Atlas runs the verification gates itself and settles the run itself. Your\n"
    "  output grants nothing: it is not an approval, an acceptance or a verdict.\n"
    "- Report honestly. Saying that the work is done is not a decision that it is.\n";

static const char G_HEAD[] =
    "atlas-plan-request: produce a plan for the goal below.\n"
    "\n"
    "goal:\n"
    "make the widget green\n"
    "\n"
    "gate-floor (immutable, operator-supplied):\n"
    "make test\n"
    "ctest -L unit\n"
    "\n"
    "bounds:\n"
    "- stages: at most 4\n"
    "- tasks in total: at most 8\n"
    "- side tasks per stage: at most 1\n"
    "\n";

static const char G_REQUIRED[] =
    "required-output:\n"
    "Write exactly one file, artifacts/plan.atlas-plan, in the format\n"
    "atlas-plan-1 specified below. Write no other plan, in no other place, in no\n"
    "other format.\n"
    "\n";

static const char GOAL[] = "make the widget green";
static const char FLOOR[] = "make test\nctest -L unit\n";

/* Concatenates into a freshly allocated string the caller frees. */
static char *cat(const char *first, ...) {
    va_list ap;
    size_t total = 0;
    va_start(ap, first);
    for (const char *s = first; s != NULL; s = va_arg(ap, const char *)) {
        total += strlen(s);
    }
    va_end(ap);
    char *out = malloc(total + 1u);
    if (out == NULL) {
        return NULL;
    }
    out[0] = '\0';
    va_start(ap, first);
    for (const char *s = first; s != NULL; s = va_arg(ap, const char *)) {
        strcat(out, s);
    }
    va_end(ap);
    return out;
}

static void expect_bytes(const char *what, const atlas_buf *got, const char *want) {
    size_t wl = strlen(want);
    if (got->len != wl || memcmp(atlas_buf_cstr(got), want, wl) != 0) {
        /* Report the first differing offset: a 3 KiB diff is unreadable
         * otherwise, and a prompt that drifts by one space is exactly the
         * failure this test exists for. */
        const char *g = atlas_buf_cstr(got);
        size_t i = 0;
        size_t n = got->len < wl ? got->len : wl;
        while (i < n && g[i] == want[i]) {
            i++;
        }
        atlas_test_fail(__FILE__, __LINE__,
                        "%s: differs at byte %zu (got %zu bytes, want %zu)\n"
                        "  got : %.80s\n  want: %.80s",
                        what, i, got->len, wl, g + i, want + i);
    }
}

static void test_planner_prompt_is_byte_exact(void) {
    atlas_buf out = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_plan_compose_planner(GOAL, FLOOR, 2, &out, &err), &err);
    char *want = cat(G_HEAD, G_REQUIRED, G_SPEC, "\nconstraints:\n", G_FIXED, NULL);
    T_REQUIRE(want != NULL);
    expect_bytes("the planner prompt", &out, want);
    free(want);
    atlas_buf_free(&out);
}

static void test_planner_retry_prompt_is_byte_exact(void) {
    atlas_buf out = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    static const char REFUSED[] = "atlas-plan-2\nstage: 1\n";
    T_OK(atlas_plan_compose_planner_retry(GOAL, FLOOR, 2,
                                          "line 1: the first line must be exactly atlas-plan-1",
                                          REFUSED, sizeof REFUSED - 1u, &out, &err),
         &err);
    char *want = cat(G_HEAD,
                     "previous-plan-refused: line 1: the first line must be exactly atlas-plan-1\n"
                     "refused-excerpt (bounded, untrusted):\n"
                     "atlas-plan-2\n"
                     "stage: 1\n"
                     "\n",
                     G_REQUIRED, G_SPEC, "\nconstraints:\n", G_FIXED, NULL);
    T_REQUIRE(want != NULL);
    expect_bytes("the planner retry prompt", &out, want);
    free(want);
    atlas_buf_free(&out);
}

static void test_a_retry_excerpt_announces_its_own_truncation(void) {
    atlas_buf out = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    size_t n = 5000u;
    char *refused = malloc(n);
    T_REQUIRE(refused != NULL);
    memset(refused, 'z', n);
    T_OK(atlas_plan_compose_planner_retry(GOAL, FLOOR, 2, "refused", refused, n, &out, &err), &err);
    T_CHECK(strstr(atlas_buf_cstr(&out), "[... 904 further bytes not shown ...]") != NULL);
    free(refused);
    atlas_buf_free(&out);
}

static void test_replan_prompt_is_byte_exact(void) {
    atlas_plan_state st;
    memset(&st, 0, sizeof st);
    st.rev_no = 1;
    st.task_count = 3;
    snprintf(st.tasks[0].task_key, sizeof st.tasks[0].task_key, "%s", "groundwork");
    st.tasks[0].stage_no = 1;
    st.tasks[0].is_tree = true;
    st.tasks[0].job_state = ATLAS_ORCH_STATE_SUCCEEDED;
    snprintf(st.tasks[0].title, sizeof st.tasks[0].title, "%s", "Lay the groundwork");
    snprintf(st.tasks[1].task_key, sizeof st.tasks[1].task_key, "%s", "notes");
    st.tasks[1].stage_no = 1;
    st.tasks[1].is_tree = false;
    st.tasks[1].job_state = ATLAS_ORCH_STATE_SUCCEEDED;
    snprintf(st.tasks[1].title, sizeof st.tasks[1].title, "%s", "Collect notes");
    snprintf(st.tasks[2].task_key, sizeof st.tasks[2].task_key, "%s", "finish");
    st.tasks[2].stage_no = 2;
    st.tasks[2].is_tree = true;
    st.tasks[2].job_state = ATLAS_ORCH_STATE_FAILED;
    snprintf(st.tasks[2].title, sizeof st.tasks[2].title, "%s", "Finish it");

    atlas_buf out = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    static const char GATE_OUT[] = "make: *** [test] Error 1\n";
    T_OK(atlas_plan_compose_replan(GOAL, FLOOR, 2, &st, "finish", "make test", GATE_OUT,
                                   sizeof GATE_OUT - 1u, &out, &err),
         &err);
    char *want = cat(G_HEAD,
                     "completed-work (Atlas facts):\n"
                     "- stage 1 task groundwork: SUCCEEDED  title (untrusted): "
                     "Lay the groundwork\n"
                     "- stage 1 task notes: SUCCEEDED  title (untrusted): Collect notes\n"
                     "\n"
                     "blocked-task: finish\n"
                     "failed-gate: make test\n"
                     "gate-output (bounded excerpt, untrusted):\n"
                     "make: *** [test] Error 1\n"
                     "\n"
                     "instruction:\n"
                     "Produce a complete new plan, in the format below, for the REMAINING work\n"
                     "only. The completed work above stands: do not redo it, do not undo it and\n"
                     "do not plan it again. You may add gates; you may not remove, replace or\n"
                     "weaken one the operator set.\n"
                     "\n",
                     G_REQUIRED, G_SPEC, "\nconstraints:\n", G_FIXED, NULL);
    T_REQUIRE(want != NULL);
    expect_bytes("the replan prompt", &out, want);
    free(want);
    atlas_buf_free(&out);
}

static void test_a_replan_with_nothing_completed_says_so(void) {
    atlas_plan_state st;
    memset(&st, 0, sizeof st);
    atlas_buf out = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_plan_compose_replan(GOAL, FLOOR, 2, &st, "first", NULL, NULL, 0, &out, &err), &err);
    T_CHECK(strstr(atlas_buf_cstr(&out), "completed-work (Atlas facts):\n- (none)\n") != NULL);
    /* A gate nobody named is stated as unrecorded rather than left out, and an
     * absent output produces no section at all rather than an empty one. */
    T_CHECK(strstr(atlas_buf_cstr(&out), "failed-gate: (none recorded)\n") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&out), "gate-output") == NULL);
    atlas_buf_free(&out);
}

static void test_executor_tree_prompt_is_byte_exact(void) {
    atlas_plan_doc_task t;
    memset(&t, 0, sizeof t);
    snprintf(t.key, sizeof t.key, "%s", "groundwork");
    t.stage_no = 2;
    t.is_tree = true;
    snprintf(t.title, sizeof t.title, "%s", "Lay the groundwork");
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_buf_set_str(&t.prompt, "Add the header and wire it in.\n", &err), &err);

    atlas_buf out = ATLAS_BUF_INIT;
    T_OK(atlas_plan_compose_executor("p0123456789abcdef0123456789abcdef", 1, &t, &out, &err), &err);
    char *want = cat("atlas-plan-task:\n"
                     "plan: p0123456789abcdef0123456789abcdef\n"
                     "revision: 1\n"
                     "stage: 2\n"
                     "task: groundwork\n"
                     "title (untrusted): Lay the groundwork\n"
                     "\n"
                     "instructions (untrusted, from the plan):\n"
                     "Add the header and wire it in.\n"
                     "\n"
                     "constraints:\n"
                     "- Work only inside the registered repository's own root.\n"
                     "- Apply exactly this task. Do not change its scope, the plan, or any\n"
                     "  verification gate.\n"
                     "- Do not commit, push, deploy, restart a daemon, or run any destructive\n"
                     "  git operation. Leave the working tree as you found it plus your\n"
                     "  changes.\n",
                     G_FIXED, NULL);
    T_REQUIRE(want != NULL);
    expect_bytes("the executor tree prompt", &out, want);
    free(want);
    atlas_buf_free(&out);
    atlas_buf_free(&t.prompt);
}

static void test_executor_side_prompt_is_byte_exact(void) {
    atlas_plan_doc_task t;
    memset(&t, 0, sizeof t);
    snprintf(t.key, sizeof t.key, "%s", "notes");
    t.stage_no = 2;
    t.is_tree = false;
    snprintf(t.title, sizeof t.title, "%s", "Collect notes");
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_buf_set_str(&t.prompt, "Summarise the failures.\n", &err), &err);

    atlas_buf out = ATLAS_BUF_INIT;
    T_OK(atlas_plan_compose_executor("p0123456789abcdef0123456789abcdef", 3, &t, &out, &err), &err);
    char *want = cat("atlas-plan-task:\n"
                     "plan: p0123456789abcdef0123456789abcdef\n"
                     "revision: 3\n"
                     "stage: 2\n"
                     "task: notes\n"
                     "title (untrusted): Collect notes\n"
                     "\n"
                     "instructions (untrusted, from the plan):\n"
                     "Summarise the failures.\n"
                     "\n"
                     "constraints:\n"
                     "- Work only inside the workspace you were given. Produce your results as\n"
                     "  files under artifacts/.\n"
                     "- Apply exactly this task. Do not change its scope, the plan, or any\n"
                     "  verification gate.\n"
                     "- You cannot modify the repository itself and must not attempt to.\n",
                     G_FIXED, NULL);
    T_REQUIRE(want != NULL);
    expect_bytes("the executor side prompt", &out, want);
    free(want);
    atlas_buf_free(&out);
    atlas_buf_free(&t.prompt);
}

static void test_composers_refuse_rather_than_invent(void) {
    atlas_buf out = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    T_FAILS_WITH(atlas_plan_compose_planner(NULL, FLOOR, 2, &out, &err), ATLAS_ERR_USAGE, &err);
    T_FAILS_WITH(atlas_plan_compose_planner("", FLOOR, 2, &out, &err), ATLAS_ERR_USAGE, &err);
    /* A plan with no operator gate could only ever be accepted on a model's
     * word, so the floor is required here as it is at `plan run`. */
    T_FAILS_WITH(atlas_plan_compose_planner(GOAL, NULL, 2, &out, &err), ATLAS_ERR_USAGE, &err);
    T_FAILS_WITH(atlas_plan_compose_planner(GOAL, "", 2, &out, &err), ATLAS_ERR_USAGE, &err);
    T_FAILS_WITH(atlas_plan_compose_planner(GOAL, FLOOR, 0, &out, &err), ATLAS_ERR_USAGE, &err);

    size_t n = (size_t)ATLAS_PLAN_GOAL_MAX + 1u;
    char *big = malloc(n + 1u);
    T_REQUIRE(big != NULL);
    memset(big, 'g', n);
    big[n] = '\0';
    T_FAILS_WITH(atlas_plan_compose_planner(big, FLOOR, 2, &out, &err), ATLAS_ERR_USAGE, &err);
    free(big);

    T_FAILS_WITH(atlas_plan_compose_planner_retry(GOAL, FLOOR, 2, NULL, "x", 1u, &out, &err),
                 ATLAS_ERR_USAGE, &err);
    atlas_plan_state st;
    memset(&st, 0, sizeof st);
    T_FAILS_WITH(atlas_plan_compose_replan(GOAL, FLOOR, 2, NULL, "k", NULL, NULL, 0, &out, &err),
                 ATLAS_ERR_USAGE, &err);
    T_FAILS_WITH(atlas_plan_compose_replan(GOAL, FLOOR, 2, &st, "", NULL, NULL, 0, &out, &err),
                 ATLAS_ERR_USAGE, &err);
    T_FAILS_WITH(atlas_plan_compose_executor("p1", 1, NULL, &out, &err), ATLAS_ERR_USAGE, &err);
    atlas_plan_doc_task t;
    memset(&t, 0, sizeof t);
    T_FAILS_WITH(atlas_plan_compose_executor(NULL, 1, &t, &out, &err), ATLAS_ERR_USAGE, &err);
    atlas_buf_free(&t.prompt);
    atlas_buf_free(&out);
}

static void test_a_composed_prompt_replaces_what_the_buffer_held(void) {
    /* `out` is set, not appended to: a driver reusing one buffer across two
     * attempts must not send the first attempt's prompt again with the second
     * stapled to it. */
    atlas_buf out = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_buf_set_str(&out, "leftovers", &err), &err);
    T_OK(atlas_plan_compose_planner(GOAL, FLOOR, 2, &out, &err), &err);
    T_CHECK(strncmp(atlas_buf_cstr(&out), "atlas-plan-request:", 19) == 0);
    atlas_buf_free(&out);
}

static const atlas_test TESTS[] = {
    {"unknown is the zero status", test_unknown_is_the_zero_status},
    {"the bounds are what the season froze", test_bounds_are_what_the_season_froze},
    {"a two-stage plan round-trips", test_a_two_stage_plan_round_trips},
    {"a document with no trailing newline parses", test_a_document_with_no_trailing_newline_parses},
    {"CRLF is tolerated", test_crlf_is_tolerated},
    {"a prompt may hold bytes that are not UTF-8", test_a_prompt_may_hold_bytes_that_are_not_utf8},
    {"a terminator with a trailing space is content",
     test_a_terminator_with_trailing_space_is_prompt_content},
    {"the maximum shape is accepted", test_the_maximum_shape_is_accepted},
    {"eight gate additions yes, nine no", test_eight_gate_additions_are_accepted_and_nine_are_not},
    {"header refusals", test_header_refusals},
    {"stage refusals", test_stage_refusals},
    {"tree-task count refusals", test_tree_task_count_refusals},
    {"side-task refusals", test_side_task_refusals},
    {"task field refusals", test_task_field_refusals},
    {"key refusals", test_key_refusals},
    {"the task-count refusal", test_task_count_refusal},
    {"size and byte refusals", test_size_and_byte_refusals},
    {"prompt and title length refusals", test_prompt_and_title_length_refusals},
    {"unrecognised line refusals", test_unrecognised_line_refusals},
    {"gate allowlist refusals", test_gate_allowlist_refusals},
    {"max_parallel is refused, not resolved", test_max_parallel_is_refused_not_resolved},
    {"a null input is refused", test_a_null_input_is_refused},
    {"the planner prompt is byte-exact", test_planner_prompt_is_byte_exact},
    {"the planner retry prompt is byte-exact", test_planner_retry_prompt_is_byte_exact},
    {"a retry excerpt announces its truncation",
     test_a_retry_excerpt_announces_its_own_truncation},
    {"the replan prompt is byte-exact", test_replan_prompt_is_byte_exact},
    {"a replan with nothing completed says so", test_a_replan_with_nothing_completed_says_so},
    {"the executor tree prompt is byte-exact", test_executor_tree_prompt_is_byte_exact},
    {"the executor side prompt is byte-exact", test_executor_side_prompt_is_byte_exact},
    {"composers refuse rather than invent", test_composers_refuse_rather_than_invent},
    {"a composed prompt replaces the buffer", test_a_composed_prompt_replaces_what_the_buffer_held},
};

ATLAS_TEST_MAIN("plan_format", TESTS)
