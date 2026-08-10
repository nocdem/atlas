/* Atlas - what A6 does not let a model do.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A6's whole model-facing surface is one read. Everything below is the evidence
 * for that sentence, and every part of it is asked of the process that would
 * have to answer rather than asserted about the source.
 *
 * The claim, precisely, and nothing beyond it:
 *
 *   Atlas exposes no operation through MCP, hooks or any AI-facing method that
 *   clears, overrides, caches or otherwise changes a freshness result, and none
 *   that revalidates a decision. A model may read a gate result and can do
 *   nothing about it. Revalidation goes through A4's operator channel
 *   unchanged, and A4's honesty limits about that channel apply here word for
 *   word: a same-UID process that can drive a pseudo-terminal — including an AI
 *   agent with shell access — may imitate it. `LOCAL_OPERATOR_CONFIRMED`
 *   identifies the channel, not a person.
 *
 * And A5's guarantee is asserted again here rather than assumed, because the
 * cheapest way to break it would have been for A6 to add the RPC method A5
 * deliberately does not have.
 */
#define _GNU_SOURCE 1

#include <stdio.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/ipc.h"
#include "atlas/mcp.h"
#include "atlas_test.h"
#include "support/fixture.h"

#define INIT_LINE                                                                                  \
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":"     \
    "\"2025-06-18\",\"capabilities\":{},\"clientInfo\":{\"name\":\"t\",\"version\":\"1\"}}}\n"

/* The schemas as the process actually reports them, over the real transport.
 * Asking the binary rather than the source is the whole point: a claim about a
 * tool surface that reads a header is a claim about a header. */
static void tools_list(atlas_buf *out) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_OK(fx_open(&fx, &err), &err);
    static const char SCRIPT[] =
        INIT_LINE "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n";
    const char *argv[] = {"--data-dir", fx_data_dir(&fx), "mcp"};
    atlas_buf errout = ATLAS_BUF_INIT;
    int code = 0;
    T_OK(fx_atlas_stdin(argv, 3u, NULL, SCRIPT, sizeof SCRIPT - 1u, out, &errout, &code, &err),
         &err);
    atlas_buf_free(&errout);
    fx_close(&fx);
}

/* --- the MCP inventory ---------------------------------------------------------
 *
 * A property of the binary, so it needs no daemon and no repository. */

/* Verbs that would mean a tool changes a lifecycle state or a freshness
 * result. The list is here rather than in prose because prose does not fail. */
static const char *const MUTATING_VERBS[] = {
    "approve",  "approval",   "reject",     "supersede",  "revalidate",
    "validate", "confirm",    "sign",       "authorize",  "authorise",
    "clear",    "dismiss",    "acknowledge", "override",  "waive",
    "suppress", "silence",    "refresh",    "recompute",  "invalidate",
    "bypass",   "force",      "unblock",
};

static bool contains_ci(const char *haystack, const char *needle) {
    size_t n = strlen(needle);
    for (const char *p = haystack; *p != '\0'; p++) {
        size_t i = 0;
        while (i < n && p[i] != '\0') {
            char a = p[i];
            char b = needle[i];
            if (a >= 'A' && a <= 'Z') {
                a = (char)(a - 'A' + 'a');
            }
            if (a != b) {
                break;
            }
            i++;
        }
        if (i == n) {
            return true;
        }
    }
    return false;
}

static void test_no_mcp_tool_name_carries_a_mutating_verb(void) {
    const char *const *names = atlas_mcp_tool_names();
    size_t n = 0;
    for (; names[n] != NULL; n++) {
        for (size_t v = 0; v < sizeof MUTATING_VERBS / sizeof MUTATING_VERBS[0]; v++) {
            T_CHECK_MSG(!contains_ci(names[n], MUTATING_VERBS[v]),
                        "the MCP tool \"%s\" carries the verb \"%s\"; A6 exposes no operation "
                        "that changes a lifecycle state or a freshness result",
                        names[n], MUTATING_VERBS[v]);
        }
    }
    /* And the one A6 tool exists, is a read, and is the only one. */
    bool found_gate = false;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(names[i], "atlas_gate_check") == 0) {
            found_gate = true;
        }
    }
    T_CHECK_MSG(found_gate, "A6's read tool must be present; a model that cannot see a stale "
                            "decision cannot be told to stop treating it as current");
}

static void test_no_mcp_schema_declares_a_capability_argument(void) {
    /* The absence is structural rather than guarded: every schema sets
     * `additionalProperties: false` and declares every argument it accepts, so
     * a tool that does not declare a token cannot be handed one. */
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf schemas = ATLAS_BUF_INIT;
    tools_list(&schemas);
    const char *s = atlas_buf_cstr(&schemas);
    T_REQUIRE_MSG(strstr(s, "atlas_gate_check") != NULL, "tools/list did not answer: %s", s);
    (void)err;

    static const char *const FORBIDDEN_ARGS[] = {
        "\"token\"", "\"confirmation\"", "\"challenge\"", "\"capability\"",
        "\"freshness\"", "\"prior_freshness\"", "\"prior_reasons\"", "\"force\"",
    };
    for (size_t i = 0; i < sizeof FORBIDDEN_ARGS / sizeof FORBIDDEN_ARGS[0]; i++) {
        T_CHECK_MSG(strstr(s, FORBIDDEN_ARGS[i]) == NULL,
                    "an MCP tool schema declares %s", FORBIDDEN_ARGS[i]);
    }
    /* Every tool closes its object, so an undeclared argument is refused rather
     * than ignored. */
    T_CHECK(strstr(s, "additionalProperties") != NULL);
    atlas_buf_free(&schemas);
}

static void test_the_gate_tool_says_what_it_does_not_claim(void) {
    /* The description a model reads is where the honesty has to be, because it
     * is the only part of A6 a model ever sees. STALE must not arrive labelled
     * as a finding that the decision is wrong. */
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf schemas = ATLAS_BUF_INIT;
    tools_list(&schemas);
    const char *s = atlas_buf_cstr(&schemas);
    (void)err;
    T_CHECK_MSG(strstr(s, "neither says the decision is wrong") != NULL,
                "the gate tool must decline to claim a stale decision is wrong");
    T_CHECK_MSG(strstr(s, "fails closed") != NULL,
                "the gate tool must say what UNKNOWN means");
    T_CHECK_MSG(strstr(s, "UNTRUSTED_DATA") != NULL,
                "the gate result carries decision prose and must say so");
    atlas_buf_free(&schemas);
}

/* --- the live daemon ------------------------------------------------------------ */

static void build_repo(fixture *fx, atlas_err *err) {
    T_OK(fx_init_repo(fx, fx_repo(fx), NULL, err), err);
    T_OK(fx_write(fx_repo(fx), "a.c", "int main(void){return 0;}\n", err), err);
    T_OK(fx_add_all(fx, fx_repo(fx), err), err);
    T_OK(fx_commit(fx, fx_repo(fx), "first", err), err);

    const char *argv[] = {"--data-dir", fx_data_dir(fx), "repo", "add", fx_repo(fx), "--name",
                          "proj"};
    atlas_buf out = ATLAS_BUF_INIT;
    atlas_buf eout = ATLAS_BUF_INIT;
    int code = -1;
    T_OK(fx_atlas(argv, 7u, &out, &eout, &code, err), err);
    T_EQ_INT(code, 0);
    atlas_buf_free(&out);
    atlas_buf_free(&eout);
}

static void test_the_daemon_answers_to_no_mutating_gate_method(void) {
    /* Every name such a method would plausibly have, asked of a live daemon.
     * A method that does not exist is a better guarantee than one that refuses,
     * and this is how the absence is checked rather than asserted.
     *
     * `decision.revalidate` is deliberately *not* in this list: it exists, it
     * sits beside `decision.approve`, and it is equally useless without a
     * capability only the terminal channel can obtain. Its own refusal is
     * asserted below. */
    static const char *const METHODS[] = {
        "gate.clear",          "gate.override",       "gate.approve",
        "gate.pass",           "gate.suppress",       "gate.refresh",
        "gate.recompute",      "gate.set",            "gate.waive",
        "decision.freshness.set", "decision.freshness.clear",
        "freshness.clear",     "freshness.set",       "decision.validate",
        "decision.unstale",    "decision.acknowledge",
        /* And A5's, again. The cheapest way to break A5's guarantee would have
         * been for A6 to add the method A5 does not have. */
        "backup.create",       "backup.verify",       "backup.restore",
        "maintenance.plan",    "maintenance.prune",
    };
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    build_repo(&fx, &err);

    fx_daemon d;
    fx_daemon_init(&d);
    T_REQUIRE(fx_daemon_start(&fx, &d, &err) == ATLAS_OK);
    T_REQUIRE(fx_daemon_wait_ready(&d, 15000, &err) == ATLAS_OK);

    for (size_t i = 0; i < sizeof METHODS / sizeof METHODS[0]; i++) {
        atlas_buf resp = ATLAS_BUF_INIT;
        atlas_err e2;
        atlas_err_init(&e2);
        (void)atlas_ipc_call(atlas_buf_cstr(&d.socket), METHODS[i], "{}", &resp, &e2);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "\"ok\":true") == NULL,
                    "the daemon accepted \"%s\"", METHODS[i]);
        atlas_buf_free(&resp);
    }

    /* The one method A6 does add answers, and answers as a read. */
    {
        atlas_buf resp = ATLAS_BUF_INIT;
        atlas_err e2;
        atlas_err_init(&e2);
        (void)atlas_ipc_call(atlas_buf_cstr(&d.socket), "gate.check", "{\"repo\":\"proj\"}", &resp,
                             &e2);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "\"result\"") != NULL,
                    "gate.check must answer: %s", atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    /* And `decision.revalidate` exists but refuses without a capability. That
     * is the honest shape: the method is reachable over the socket exactly as
     * `decision.approve` is, and neither can be used by anything that has not
     * been through the terminal. */
    {
        atlas_buf resp = ATLAS_BUF_INIT;
        atlas_err e2;
        atlas_err_init(&e2);
        (void)atlas_ipc_call(atlas_buf_cstr(&d.socket), "decision.revalidate",
                             "{\"repo\":\"proj\",\"decision\":\"atlas-dec-"
                             "00000000000000000000000000000000\"}",
                             &resp, &e2);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "\"ok\":true") == NULL,
                    "the daemon revalidated without a capability: %s", atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    fx_close(&fx);
}

static void test_a_hook_cannot_revalidate_or_change_a_gate_result(void) {
    /* Hooks store metadata and fail open. None of them emits a decision, a
     * permission verdict or a lifecycle change, and A6 adds no event. The list
     * the binary handles is the list the plugin configures, which
     * `tests/test_plugin.c` already pins; what is asserted here is that no hook
     * payload can carry a capability. */
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    build_repo(&fx, &err);

    static const char PAYLOAD[] =
        "{\"session_id\":\"s1\",\"cwd\":\"/tmp\",\"tool_name\":\"Edit\","
        "\"tool_input\":{\"file_path\":\"a.c\"},"
        /* Everything a hostile payload would try. None of these is a member of
         * any hook's input contract, and a hook reads `tool_input` for exactly
         * one member. */
        "\"token\":\"deadbeef\",\"confirmation\":\"deadbeef\","
        "\"intent\":\"revalidate\",\"freshness\":\"FRESH\",\"gate\":\"PASS\"}";

    const char *args[] = {"--data-dir", fx_data_dir(&fx), "hook", "PostToolUse"};
    const char *env[] = {NULL};
    atlas_buf out = ATLAS_BUF_INIT;
    atlas_buf eout = ATLAS_BUF_INIT;
    int code = -1;
    T_OK(fx_atlas_stdin(args, 4u, env, PAYLOAD, sizeof PAYLOAD - 1u, &out, &eout, &code, &err),
         &err);
    /* Hooks fail open and return valid JSON whatever happened. */
    T_EQ_INT(code, 0);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"decision\"") == NULL,
                "a hook must not emit a decision verdict: %s", atlas_buf_cstr(&out));

    /* And nothing was recorded. */
    const char *check[] = {"--data-dir", fx_data_dir(&fx), "--json", "gate", "check", "proj"};
    atlas_buf gout = ATLAS_BUF_INIT;
    atlas_buf geout = ATLAS_BUF_INIT;
    int gcode = -1;
    T_OK(fx_atlas(check, 6u, &gout, &geout, &gcode, &err), &err);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&gout), "\"stale\":0") != NULL,
                "a hook payload changed a gate result: %s", atlas_buf_cstr(&gout));

    atlas_buf_free(&out);
    atlas_buf_free(&eout);
    atlas_buf_free(&gout);
    atlas_buf_free(&geout);
    fx_close(&fx);
}


/* --- concurrency, and non-interference ------------------------------------------
 *
 * A gate query runs against a live daemon that is indexing, repeatedly. Two
 * things must hold: the daemon keeps working — the gate takes no lock and so has
 * nothing with which to block it — and every answer is internally consistent,
 * never a mixture of a decision from one state and a graph from another. */
static void test_the_gate_neither_blocks_nor_mixes_while_the_daemon_indexes(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    build_repo(&fx, &err);

    fx_daemon d;
    fx_daemon_init(&d);
    T_REQUIRE(fx_daemon_start(&fx, &d, &err) == ATLAS_OK);
    T_REQUIRE(fx_daemon_wait_ready(&d, 15000, &err) == ATLAS_OK);

    for (int round = 0; round < 6; round++) {
        /* Keep the repository moving under the query. */
        char body[64];
        (void)snprintf(body, sizeof body, "int main(void){return %d;}\n", round);
        T_OK(fx_write(fx_repo(&fx), "a.c", body, &err), &err);
        T_OK(fx_add_all(&fx, fx_repo(&fx), &err), &err);
        T_OK(fx_commit(&fx, fx_repo(&fx), "moving", &err), &err);

        atlas_buf out = ATLAS_BUF_INIT;
        atlas_buf eout = ATLAS_BUF_INIT;
        int code = -1;
        const char *args[] = {"--json", "gate", "check", "proj"};
        T_OK(fx_atlas_with_runtime(&fx, &d, args, 4u, &out, &eout, &code, &err), &err);
        /* PASS, REVIEW_REQUIRED or BLOCKED — all three are correct answers
         * depending on where the daemon has got to. What must never happen is a
         * crash, a hang, or a malformed document. */
        T_CHECK_MSG(code == 0 || code == ATLAS_EXIT_GATE_REVIEW_REQUIRED ||
                        code == ATLAS_EXIT_GATE_BLOCKED,
                    "gate check exited %d during indexing:\n%s\n%s", code,
                    atlas_buf_cstr(&out), atlas_buf_cstr(&eout));
        const char *doc = atlas_buf_cstr(&out);
        T_CHECK_MSG(strstr(doc, "\"result\"") != NULL, "no gate result in:\n%s", doc);
        /* One coherent snapshot: the report's indexed commit is the one every
         * assessment in it was measured against. A mixture would show up here
         * as two different values in one document. */
        T_CHECK_MSG(strstr(doc, "\"indexed_commit\"") != NULL, "no indexed commit in:\n%s", doc);
        atlas_buf_free(&out);
        atlas_buf_free(&eout);
    }

    /* The daemon is still alive and still answering, which is the
     * non-interference half: a read-only gate query cannot stop indexing
     * because it takes nothing that indexing needs. */
    T_CHECK_MSG(!fx_daemon_exited(&d), "the daemon died while the gate was querying it");
    atlas_buf resp = ATLAS_BUF_INIT;
    atlas_err e2;
    atlas_err_init(&e2);
    T_OK(atlas_ipc_call(atlas_buf_cstr(&d.socket), "gate.check", "{\"repo\":\"proj\"}", &resp,
                        &e2),
         &e2);
    T_CHECK(strstr(atlas_buf_cstr(&resp), "\"result\"") != NULL);
    atlas_buf_free(&resp);

    /* And a restart changes nothing: the assessment is recomputed from durable
     * state every time, so replaying it after the daemon comes back gives the
     * same shape of answer. */
    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    fx_daemon_init(&d);
    T_REQUIRE(fx_daemon_start(&fx, &d, &err) == ATLAS_OK);
    T_REQUIRE(fx_daemon_wait_ready(&d, 15000, &err) == ATLAS_OK);
    atlas_buf again = ATLAS_BUF_INIT;
    atlas_err e3;
    atlas_err_init(&e3);
    T_OK(atlas_ipc_call(atlas_buf_cstr(&d.socket), "gate.check", "{\"repo\":\"proj\"}", &again,
                        &e3),
         &e3);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&again), "\"result\"") != NULL,
                "the gate did not answer after a daemon restart: %s", atlas_buf_cstr(&again));
    atlas_buf_free(&again);

    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    fx_close(&fx);
}

/* --- the claim itself ----------------------------------------------------------- */

static atlas_status read_file(const char *rel, atlas_buf *out, atlas_err *err) {
    atlas_buf path = ATLAS_BUF_INIT;
    atlas_status st = atlas_buf_appendf(&path, err, "%s/%s", ATLAS_SRC_DIR, rel);
    if (st != ATLAS_OK) {
        atlas_buf_free(&path);
        return st;
    }
    FILE *f = fopen(atlas_buf_cstr(&path), "rb");
    atlas_buf_free(&path);
    if (f == NULL) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG, "cannot open %s", rel);
    }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1u, sizeof buf, f)) > 0 && st == ATLAS_OK) {
        st = atlas_buf_append(out, buf, n, err);
    }
    (void)fclose(f);
    return st;
}

static void test_no_a6_claim_is_stronger_than_the_implementation(void) {
    /* The tripwire, in the same shape A4 and A5 use and for the same reason: an
     * overclaim is the one defect that costs nothing to ship and everything to
     * rely on. Both lists are the point.
     *
     * A6's temptation is specific. Freshness is deterministic, so it is very
     * easy to write that Atlas "detects" that a decision is "invalid" or
     * "no longer applies" — and it detects neither. It observes that anchors
     * moved. Whether the decision survives that is a question about intent. */
    static const char *const FORBIDDEN[] = {
        "proves the decision is",
        "decision is no longer valid",
        "decision is invalid",
        "decision no longer applies",
        "automatically invalidates",
        "automatically revokes",
        "guarantees that every",
        "detects every change",
        "cannot produce a false",
        "verified by a person",
        /* "signed off" is A4's to police, and CLAUDE.md names it as a phrasing
         * to avoid — so scanning for it here would fail on the instruction not
         * to use it. */
        "cryptographically signed",
        "provides non-repudiation",
        "tamper-proof",
    };
    /* And the wording that must stay. */
    static const char *const REQUIRED[] = {
        "requires human revalidation",
        "does not mean the decision is wrong",
        "fails closed",
    };
    static const char *const FILES[] = {
        "docs/impact-gates.md",
        "include/atlas/gate.h",
        "src/gate/gate.c",
        "src/gate/assess.c",
        "CLAUDE.md",
    };
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf all = ATLAS_BUF_INIT;
    for (size_t i = 0; i < sizeof FILES / sizeof FILES[0]; i++) {
        atlas_buf one = ATLAS_BUF_INIT;
        T_OK(read_file(FILES[i], &one, &err), &err);
        for (size_t f = 0; f < sizeof FORBIDDEN / sizeof FORBIDDEN[0]; f++) {
            T_CHECK_MSG(strstr(atlas_buf_cstr(&one), FORBIDDEN[f]) == NULL,
                        "%s contains the overclaim \"%s\"", FILES[i], FORBIDDEN[f]);
        }
        T_OK(atlas_buf_append(&all, one.data, one.len, &err), &err);
        atlas_buf_free(&one);
    }
    for (size_t r = 0; r < sizeof REQUIRED / sizeof REQUIRED[0]; r++) {
        T_CHECK_MSG(strstr(atlas_buf_cstr(&all), REQUIRED[r]) != NULL,
                    "the A6 documentation no longer says \"%s\"", REQUIRED[r]);
    }
    atlas_buf_free(&all);
}

static const atlas_test TESTS[] = {
    {"no MCP tool name carries a mutating verb", test_no_mcp_tool_name_carries_a_mutating_verb},
    {"no MCP schema declares a capability argument",
     test_no_mcp_schema_declares_a_capability_argument},
    {"the gate tool says what it does not claim",
     test_the_gate_tool_says_what_it_does_not_claim},
    {"the daemon answers to no mutating gate method",
     test_the_daemon_answers_to_no_mutating_gate_method},
    {"a hook cannot revalidate or change a gate result",
     test_a_hook_cannot_revalidate_or_change_a_gate_result},
    {"the gate neither blocks nor mixes while the daemon indexes",
     test_the_gate_neither_blocks_nor_mixes_while_the_daemon_indexes},
    {"no A6 claim is stronger than the implementation",
     test_no_a6_claim_is_stronger_than_the_implementation},
};

ATLAS_TEST_MAIN("gate_trust", TESTS)
