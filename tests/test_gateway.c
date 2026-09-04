/* Atlas - A9: the gateway policy matrix and the HTTP reader's refusals.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Both halves here are almost entirely about saying no, and a refusal is what
 * is easiest to get wrong quietly: a parser that accepts one malformed request
 * still serves every well-formed one, so nothing observable goes wrong until
 * somebody uses the slack.
 *
 * The policy cases go through `atlas_gwpolicy_parse_buffer` rather than through
 * the loader. The loader's root-ownership walk can only succeed for a genuinely
 * root-owned file, so a test driven through it could assert exactly one thing —
 * that a fixture file is refused — and every interesting refusal below would be
 * unreachable. That one thing is asserted too, separately, because it is the
 * property the whole policy rests on.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/decision.h"
#include "atlas/gateway.h"
#include "atlas/gwpolicy.h"
#include "atlas/http.h"
#include "atlas_test.h"
#include "gw/gw_internal.h"
#include "ipc/server_internal.h"
#include "support/fixture.h"
#include "support/jsoncheck.h"

static void parse_policy(const char *text, atlas_gwpolicy *out) {
    atlas_gwpolicy_parse_buffer(text, strlen(text), out);
}

/* A policy that is complete and valid, used as the base every case mutates. */
static const char *const GOOD =
    "enabled = yes\n"
    "gateway_uid = 1001\n"
    "remote_mcp = yes\n"
    "web_gui = yes\n";

static void test_a_complete_policy_enables_the_gateway(void) {
    atlas_gwpolicy p;
    parse_policy(GOOD, &p);
    T_CHECK_MSG(p.state == ATLAS_GWPOLICY_ENABLED, "a complete policy did not enable: %s",
                atlas_gwpolicy_reason_name(p.reason));
    T_CHECK(p.reason == ATLAS_GWPOLICY_REASON_ACTIVE);
    /* The documented defaults, present because a key was absent rather than
     * because a zero happened to be safe. */
    T_CHECK_MSG(strcmp(p.listen_addr, "127.0.0.1") == 0,
                "the default bind is \"%s\" rather than loopback", p.listen_addr);
    T_EQ_INT(p.listen_port, 8787);
    T_CHECK(p.gateway_uid == 1001);
    T_CHECK(p.remote_mcp && p.web_gui);
    T_CHECK_MSG(!p.trust_forwarded_for, "forwarded-for is trusted by default");
    T_CHECK_MSG(p.origin_count == 0, "an origin is allowed by default");
}

static void test_a_zeroed_policy_authorises_nothing(void) {
    /* DISABLED is zero, for the reason A6 keeps UNKNOWN and BLOCKED there and
     * A8 keeps DISABLED there: a memset must not produce a running gateway. */
    atlas_gwpolicy p;
    memset(&p, 0, sizeof p);
    T_CHECK(p.state == ATLAS_GWPOLICY_DISABLED);
    T_CHECK(p.tls_mode == ATLAS_GWPOLICY_TLS_UNSET);
    T_CHECK(p.gateway_uid == 0);
    T_CHECK(!p.remote_mcp);
    T_CHECK(!p.web_gui);
    T_CHECK(p.origin_count == 0);
    T_CHECK_MSG(!atlas_http_origin_allowed(&p, "https://example.com"),
                "a zeroed policy allowed an origin");
}

static void test_every_malformed_policy_disables_the_gateway(void) {
    struct {
        const char *name;
        const char *text;
    } cases[] = {
        {"an unrecognised key",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\nlisten_bind = 0.0.0.0\n"},
        {"no gateway_uid", "enabled = yes\nremote_mcp = yes\n"},
        {"gateway_uid zero", "enabled = yes\ngateway_uid = 0\nremote_mcp = yes\n"},
        {"no enabled key", "gateway_uid = 1001\nremote_mcp = yes\n"},
        {"no surface at all", "enabled = yes\ngateway_uid = 1001\n"},
        {"both surfaces off",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = no\nweb_gui = no\n"},
        {"a non-loopback bind with no TLS stance",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\nlisten_addr = 0.0.0.0\n"},
        {"a wildcard origin",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\nallowed_origin = *\n"},
        {"an origin that is not a URL",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\nallowed_origin = example.com\n"},
        {"a port out of range",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\nlisten_port = 70000\n"},
        {"a request ceiling above the compiled-in bound",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\nmax_request_bytes = 999999999\n"},
        {"a concurrency ceiling above the compiled-in bound",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\nmax_concurrent = 100000\n"},
        {"a rate ceiling above the compiled-in bound",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\nrate_limit_per_minute = 99999999\n"},
        {"a session ttl above the compiled-in bound",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\nsession_ttl_seconds = 99999999\n"},
        {"a boolean spelled another way",
         "enabled = true\ngateway_uid = 1001\nremote_mcp = yes\n"},
        {"an unknown tls_mode",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\ntls_mode = LETSENCRYPT\n"},
        {"a listen address with a control byte",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\nlisten_addr = 10.0.0.1\x01\n"},
        {"web_gui_anonymous_scopes naming an unknown scope",
         "enabled = yes\ngateway_uid = 1001\nweb_gui = yes\n"
         "web_gui_anonymous_scopes = repo:read not_a_real_scope\n"},
        {"web_gui_anonymous_scopes naming memory:write",
         "enabled = yes\ngateway_uid = 1001\nweb_gui = yes\n"
         "web_gui_anonymous_scopes = repo:read memory:write\n"},
        {"web_gui_anonymous_scopes with web_gui = no",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\nweb_gui = no\n"
         "web_gui_anonymous_scopes = repo:read\n"},
        {"web_gui_anonymous_scopes with web_gui absent",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\n"
         "web_gui_anonymous_scopes = repo:read\n"},

        /* A16: the two disposal keys and the acceptance key. Every base text
         * below already carries `web_gui = yes` and `tls_mode = REVERSE_PROXY`
         * so that the one condition each case means to break is the only one
         * that can be the reason -- a case that also happened to fail the
         * web_gui or tls_mode cross-check would not actually be testing what
         * its name says. */
        {"remote_dispose_key with 15 hex characters instead of 16",
         "enabled = yes\ngateway_uid = 1001\nweb_gui = yes\ntls_mode = REVERSE_PROXY\n"
         "remote_dispose_key = key_581e0a805cc1feb\nremote_dispose_kinds = PARKED\n"},
        {"remote_dispose_key in uppercase hex",
         "enabled = yes\ngateway_uid = 1001\nweb_gui = yes\ntls_mode = REVERSE_PROXY\n"
         "remote_dispose_key = key_581E0A805CC1FEBE\nremote_dispose_kinds = PARKED\n"},
        {"remote_dispose_key with no key_ prefix",
         "enabled = yes\ngateway_uid = 1001\nweb_gui = yes\ntls_mode = REVERSE_PROXY\n"
         "remote_dispose_key = 581e0a805cc1febe\nremote_dispose_kinds = PARKED\n"},
        {"remote_dispose_kinds naming an unknown kind",
         "enabled = yes\ngateway_uid = 1001\nweb_gui = yes\ntls_mode = REVERSE_PROXY\n"
         "remote_dispose_key = key_581e0a805cc1febe\n"
         "remote_dispose_kinds = PARKED NOT_A_REAL_KIND\n"},
        {"remote_dispose_kinds naming a duplicate kind",
         "enabled = yes\ngateway_uid = 1001\nweb_gui = yes\ntls_mode = REVERSE_PROXY\n"
         "remote_dispose_key = key_581e0a805cc1febe\n"
         "remote_dispose_kinds = PARKED PARKED\n"},
        /* This one is refused by `take_value` returning false on a value
         * that trims to nothing -- the line falls through to the generic
         * "unrecognised key" refusal below, never reaching the parser's own
         * `!any_kind` guard, which is unreachable under this file's grammar
         * for the reason `gwpolicy.c`'s comment on that guard explains. Kept
         * in this matrix anyway because the *outward* refusal -- DISABLED,
         * not ACTIVE -- is the property being asserted, not which internal
         * branch produced it. */
        {"remote_dispose_kinds given an empty value (refused by the generic "
         "blank-value fallthrough, not by the parser's own empty-list guard)",
         "enabled = yes\ngateway_uid = 1001\nweb_gui = yes\ntls_mode = REVERSE_PROXY\n"
         "remote_dispose_key = key_581e0a805cc1febe\nremote_dispose_kinds = \n"},
        {"remote_dispose_key without remote_dispose_kinds",
         "enabled = yes\ngateway_uid = 1001\nweb_gui = yes\ntls_mode = REVERSE_PROXY\n"
         "remote_dispose_key = key_581e0a805cc1febe\n"},
        {"remote_dispose_kinds without remote_dispose_key",
         "enabled = yes\ngateway_uid = 1001\nweb_gui = yes\ntls_mode = REVERSE_PROXY\n"
         "remote_dispose_kinds = PARKED\n"},
        {"both disposal keys with tls_mode = NONE and no acceptance",
         "enabled = yes\ngateway_uid = 1001\nweb_gui = yes\ntls_mode = NONE\n"
         "remote_dispose_key = key_581e0a805cc1febe\nremote_dispose_kinds = PARKED\n"},
        {"both disposal keys with web_gui = no",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\nweb_gui = no\n"
         "tls_mode = REVERSE_PROXY\n"
         "remote_dispose_key = key_581e0a805cc1febe\nremote_dispose_kinds = PARKED\n"},
        {"web_gui_anonymous_scopes naming decisions:dispose",
         "enabled = yes\ngateway_uid = 1001\nweb_gui = yes\n"
         "web_gui_anonymous_scopes = repo:read decisions:dispose\n"},
        {"operator_accepts_cleartext_disposal = no",
         "enabled = yes\ngateway_uid = 1001\nweb_gui = yes\ntls_mode = NONE\n"
         "remote_dispose_key = key_581e0a805cc1febe\nremote_dispose_kinds = PARKED\n"
         "operator_accepts_cleartext_disposal = no\n"},
        {"operator_accepts_cleartext_disposal = true",
         "enabled = yes\ngateway_uid = 1001\nweb_gui = yes\ntls_mode = NONE\n"
         "remote_dispose_key = key_581e0a805cc1febe\nremote_dispose_kinds = PARKED\n"
         "operator_accepts_cleartext_disposal = true\n"},
        {"operator_accepts_cleartext_disposal = 1",
         "enabled = yes\ngateway_uid = 1001\nweb_gui = yes\ntls_mode = NONE\n"
         "remote_dispose_key = key_581e0a805cc1febe\nremote_dispose_kinds = PARKED\n"
         "operator_accepts_cleartext_disposal = 1\n"},
        {"the acceptance with tls_mode = REVERSE_PROXY",
         "enabled = yes\ngateway_uid = 1001\nweb_gui = yes\ntls_mode = REVERSE_PROXY\n"
         "remote_dispose_key = key_581e0a805cc1febe\nremote_dispose_kinds = PARKED\n"
         "operator_accepts_cleartext_disposal = yes\n"},
        {"the acceptance with neither disposal key",
         "enabled = yes\ngateway_uid = 1001\nweb_gui = yes\n"
         "operator_accepts_cleartext_disposal = yes\n"},

        /* A14: the eight submission keys and the acceptance key.
         * Every base text below already carries `tls_mode = REVERSE_PROXY` so
         * that the one condition each case means to break is the only one that
         * can be the reason -- a case that also happened to fail a tls_mode
         * cross-check would not actually be testing what its name says.
         * The complete valid set (all seven required keys present under
         * REVERSE_PROXY) is in the ENABLED tests that follow this matrix. */
        {"remote_submit_key with 15 hex characters instead of 16",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\ntls_mode = REVERSE_PROXY\n"
         "remote_submit_key = key_b2578f48143c06\n"
         "remote_submit_driver = claude\nremote_submit_mode = patch\n"
         "remote_submit_gate = make\nremote_submit_max_attempts = 1\n"
         "remote_submit_max_active = 2\nremote_submit_max_per_day = 6\n"},
        {"remote_submit_key in uppercase hex",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\ntls_mode = REVERSE_PROXY\n"
         "remote_submit_key = key_B2578F48143C06D3\n"
         "remote_submit_driver = claude\nremote_submit_mode = patch\n"
         "remote_submit_gate = make\nremote_submit_max_attempts = 1\n"
         "remote_submit_max_active = 2\nremote_submit_max_per_day = 6\n"},
        {"remote_submit_key with no key_ prefix",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\ntls_mode = REVERSE_PROXY\n"
         "remote_submit_key = b2578f48143c06d3\n"
         "remote_submit_driver = claude\nremote_submit_mode = patch\n"
         "remote_submit_gate = make\nremote_submit_max_attempts = 1\n"
         "remote_submit_max_active = 2\nremote_submit_max_per_day = 6\n"},
        {"duplicate remote_submit_key",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\ntls_mode = REVERSE_PROXY\n"
         "remote_submit_key = key_b2578f48143c06d3\n"
         "remote_submit_key = key_b2578f48143c06d3\n"
         "remote_submit_driver = claude\nremote_submit_mode = patch\n"
         "remote_submit_gate = make\nremote_submit_max_attempts = 1\n"
         "remote_submit_max_active = 2\nremote_submit_max_per_day = 6\n"},
        {"more than four remote_submit_key lines",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\ntls_mode = REVERSE_PROXY\n"
         "remote_submit_key = key_b2578f48143c06d3\n"
         "remote_submit_key = key_1f0a2b3c4d5e6f70\n"
         "remote_submit_key = key_a1b2c3d4e5f60718\n"
         "remote_submit_key = key_0011223344556677\n"
         "remote_submit_key = key_9988776655443322\n"
         "remote_submit_driver = claude\nremote_submit_mode = patch\n"
         "remote_submit_gate = make\nremote_submit_max_attempts = 1\n"
         "remote_submit_max_active = 2\nremote_submit_max_per_day = 6\n"},
        {"remote_submit_key equal to remote_dispose_key",
         "enabled = yes\ngateway_uid = 1001\nweb_gui = yes\ntls_mode = REVERSE_PROXY\n"
         "remote_dispose_key = key_581e0a805cc1febe\nremote_dispose_kinds = PARKED\n"
         "remote_submit_key = key_581e0a805cc1febe\n"
         "remote_submit_driver = claude\nremote_submit_mode = patch\n"
         "remote_submit_gate = make\nremote_submit_max_attempts = 1\n"
         "remote_submit_max_active = 2\nremote_submit_max_per_day = 6\n"},
        {"remote_submit_key with all other submission lines absent",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\ntls_mode = REVERSE_PROXY\n"
         "remote_submit_key = key_b2578f48143c06d3\n"},
        {"remote_submit_driver without submit key",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\ntls_mode = REVERSE_PROXY\n"
         "remote_submit_driver = claude\nremote_submit_mode = patch\n"
         "remote_submit_gate = make\nremote_submit_max_attempts = 1\n"
         "remote_submit_max_active = 2\nremote_submit_max_per_day = 6\n"},
        {"duplicate remote_submit_driver line",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\ntls_mode = REVERSE_PROXY\n"
         "remote_submit_key = key_b2578f48143c06d3\n"
         "remote_submit_driver = claude\nremote_submit_driver = fake\n"
         "remote_submit_mode = patch\nremote_submit_gate = make\n"
         "remote_submit_max_attempts = 1\nremote_submit_max_active = 2\n"
         "remote_submit_max_per_day = 6\n"},
        {"remote_submit_driver with an invalid name (uppercase)",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\ntls_mode = REVERSE_PROXY\n"
         "remote_submit_key = key_b2578f48143c06d3\n"
         "remote_submit_driver = Claude\nremote_submit_mode = patch\n"
         "remote_submit_gate = make\nremote_submit_max_attempts = 1\n"
         "remote_submit_max_active = 2\nremote_submit_max_per_day = 6\n"},
        {"duplicate remote_submit_mode line",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\ntls_mode = REVERSE_PROXY\n"
         "remote_submit_key = key_b2578f48143c06d3\n"
         "remote_submit_driver = claude\nremote_submit_mode = patch\n"
         "remote_submit_mode = apply\nremote_submit_gate = make\n"
         "remote_submit_max_attempts = 1\nremote_submit_max_active = 2\n"
         "remote_submit_max_per_day = 6\n"},
        {"a gate line with a control byte",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\ntls_mode = REVERSE_PROXY\n"
         "remote_submit_key = key_b2578f48143c06d3\n"
         "remote_submit_driver = claude\nremote_submit_mode = patch\n"
         "remote_submit_gate = make\x01\nremote_submit_max_attempts = 1\n"
         "remote_submit_max_active = 2\nremote_submit_max_per_day = 6\n"},
        {"a gate line whose first token contains a slash",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\ntls_mode = REVERSE_PROXY\n"
         "remote_submit_key = key_b2578f48143c06d3\n"
         "remote_submit_driver = claude\nremote_submit_mode = patch\n"
         "remote_submit_gate = /usr/bin/make\nremote_submit_max_attempts = 1\n"
         "remote_submit_max_active = 2\nremote_submit_max_per_day = 6\n"},
        {"remote_submit_max_attempts of 0",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\ntls_mode = REVERSE_PROXY\n"
         "remote_submit_key = key_b2578f48143c06d3\n"
         "remote_submit_driver = claude\nremote_submit_mode = patch\n"
         "remote_submit_gate = make\nremote_submit_max_attempts = 0\n"
         "remote_submit_max_active = 2\nremote_submit_max_per_day = 6\n"},
        {"remote_submit_max_attempts above ATLAS_ORCH_MAX_ATTEMPTS (5)",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\ntls_mode = REVERSE_PROXY\n"
         "remote_submit_key = key_b2578f48143c06d3\n"
         "remote_submit_driver = claude\nremote_submit_mode = patch\n"
         "remote_submit_gate = make\nremote_submit_max_attempts = 6\n"
         "remote_submit_max_active = 2\nremote_submit_max_per_day = 6\n"},
        {"remote_submit_max_active of 0",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\ntls_mode = REVERSE_PROXY\n"
         "remote_submit_key = key_b2578f48143c06d3\n"
         "remote_submit_driver = claude\nremote_submit_mode = patch\n"
         "remote_submit_gate = make\nremote_submit_max_attempts = 1\n"
         "remote_submit_max_active = 0\nremote_submit_max_per_day = 6\n"},
        {"remote_submit_max_active above ceiling (8)",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\ntls_mode = REVERSE_PROXY\n"
         "remote_submit_key = key_b2578f48143c06d3\n"
         "remote_submit_driver = claude\nremote_submit_mode = patch\n"
         "remote_submit_gate = make\nremote_submit_max_attempts = 1\n"
         "remote_submit_max_active = 9\nremote_submit_max_per_day = 6\n"},
        {"remote_submit_max_per_day of 0",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\ntls_mode = REVERSE_PROXY\n"
         "remote_submit_key = key_b2578f48143c06d3\n"
         "remote_submit_driver = claude\nremote_submit_mode = patch\n"
         "remote_submit_gate = make\nremote_submit_max_attempts = 1\n"
         "remote_submit_max_active = 2\nremote_submit_max_per_day = 0\n"},
        {"remote_submit_max_per_day above ceiling (64)",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\ntls_mode = REVERSE_PROXY\n"
         "remote_submit_key = key_b2578f48143c06d3\n"
         "remote_submit_driver = claude\nremote_submit_mode = patch\n"
         "remote_submit_gate = make\nremote_submit_max_attempts = 1\n"
         "remote_submit_max_active = 2\nremote_submit_max_per_day = 65\n"},
        {"all submission lines with tls_mode = NONE and no cleartext acceptance",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\ntls_mode = NONE\n"
         "remote_submit_key = key_b2578f48143c06d3\n"
         "remote_submit_driver = claude\nremote_submit_mode = patch\n"
         "remote_submit_gate = make\nremote_submit_max_attempts = 1\n"
         "remote_submit_max_active = 2\nremote_submit_max_per_day = 6\n"},
        {"all submission lines with no tls_mode and no cleartext acceptance",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\n"
         "remote_submit_key = key_b2578f48143c06d3\n"
         "remote_submit_driver = claude\nremote_submit_mode = patch\n"
         "remote_submit_gate = make\nremote_submit_max_attempts = 1\n"
         "remote_submit_max_active = 2\nremote_submit_max_per_day = 6\n"},
        {"operator_accepts_cleartext_submission = no",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\ntls_mode = NONE\n"
         "remote_submit_key = key_b2578f48143c06d3\n"
         "remote_submit_driver = claude\nremote_submit_mode = patch\n"
         "remote_submit_gate = make\nremote_submit_max_attempts = 1\n"
         "remote_submit_max_active = 2\nremote_submit_max_per_day = 6\n"
         "operator_accepts_cleartext_submission = no\n"},
        {"operator_accepts_cleartext_submission = true",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\ntls_mode = NONE\n"
         "remote_submit_key = key_b2578f48143c06d3\n"
         "remote_submit_driver = claude\nremote_submit_mode = patch\n"
         "remote_submit_gate = make\nremote_submit_max_attempts = 1\n"
         "remote_submit_max_active = 2\nremote_submit_max_per_day = 6\n"
         "operator_accepts_cleartext_submission = true\n"},
        {"the submission acceptance with tls_mode = REVERSE_PROXY",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\ntls_mode = REVERSE_PROXY\n"
         "remote_submit_key = key_b2578f48143c06d3\n"
         "remote_submit_driver = claude\nremote_submit_mode = patch\n"
         "remote_submit_gate = make\nremote_submit_max_attempts = 1\n"
         "remote_submit_max_active = 2\nremote_submit_max_per_day = 6\n"
         "operator_accepts_cleartext_submission = yes\n"},
        {"operator_accepts_cleartext_submission without a submit key",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\n"
         "operator_accepts_cleartext_submission = yes\n"},
        {"jobs:submit in web_gui_anonymous_scopes (refused by ungrantable check)",
         "enabled = yes\ngateway_uid = 1001\nweb_gui = yes\n"
         "web_gui_anonymous_scopes = repo:read jobs:submit\n"},
        /* A14 cross-check: disposal cleartext accepted, submission lines present,
         * but no submission cleartext acceptance -- the two acceptances are
         * independent (NOT implied by each other). */
        {"disposal acceptance only with all submit lines under NONE (submission not accepted)",
         "enabled = yes\ngateway_uid = 1001\nweb_gui = yes\ntls_mode = NONE\n"
         "remote_dispose_key = key_581e0a805cc1febe\nremote_dispose_kinds = PARKED\n"
         "operator_accepts_cleartext_disposal = yes\n"
         "remote_submit_key = key_b2578f48143c06d3\n"
         "remote_submit_driver = claude\nremote_submit_mode = patch\n"
         "remote_submit_gate = make\nremote_submit_max_attempts = 1\n"
         "remote_submit_max_active = 2\nremote_submit_max_per_day = 6\n"},
        /* Nine remote_submit_gate lines -- max is ATLAS_ORCH_MAX_VALIDATIONS = 8. */
        {"nine remote_submit_gate lines (one over ATLAS_ORCH_MAX_VALIDATIONS)",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\ntls_mode = REVERSE_PROXY\n"
         "remote_submit_key = key_b2578f48143c06d3\n"
         "remote_submit_driver = claude\nremote_submit_mode = patch\n"
         "remote_submit_gate = make\nremote_submit_gate = test\n"
         "remote_submit_gate = lint\nremote_submit_gate = fmt\n"
         "remote_submit_gate = build\nremote_submit_gate = check\n"
         "remote_submit_gate = scan\nremote_submit_gate = verify\n"
         "remote_submit_gate = ninth\n"
         "remote_submit_max_attempts = 1\nremote_submit_max_active = 2\n"
         "remote_submit_max_per_day = 6\n"},
        /* remote_submit_mode must match is_submit_name: lower-alpha/digit/-/_/. only. */
        {"remote_submit_mode with an uppercase character",
         "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\ntls_mode = REVERSE_PROXY\n"
         "remote_submit_key = key_b2578f48143c06d3\n"
         "remote_submit_driver = claude\nremote_submit_mode = Patch\n"
         "remote_submit_gate = make\nremote_submit_max_attempts = 1\n"
         "remote_submit_max_active = 2\nremote_submit_max_per_day = 6\n"},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        atlas_gwpolicy p;
        parse_policy(cases[i].text, &p);
        T_CHECK_MSG(p.state == ATLAS_GWPOLICY_DISABLED, "%s did not disable the gateway",
                    cases[i].name);
        /* And nothing partial survives: a disabled policy must not be usable
         * for the fields it did manage to read. */
        T_CHECK_MSG(p.reason != ATLAS_GWPOLICY_REASON_ACTIVE, "%s reported ACTIVE", cases[i].name);
    }

    /* Over-long gate line (>= ATLAS_GWPOLICY_GATE_LINE_MAX = 256 bytes).
     * Cannot be expressed as a C string literal of known length, so built
     * programmatically and tested outside the table loop. */
    {
        char gate_text[1024];
        /* Build a policy with a gate line of exactly 256 printable characters
         * (no '/' in the first token) -- one byte over the 255-byte maximum. */
        char gate_val[300];
        memset(gate_val, 'x', sizeof gate_val - 1);
        gate_val[sizeof gate_val - 1] = '\0';
        /* Trim to exactly 256 printable chars (one too many). */
        gate_val[256] = '\0';
        snprintf(gate_text, sizeof gate_text,
                 "enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\n"
                 "tls_mode = REVERSE_PROXY\n"
                 "remote_submit_key = key_b2578f48143c06d3\n"
                 "remote_submit_driver = claude\nremote_submit_mode = patch\n"
                 "remote_submit_gate = %s\n"
                 "remote_submit_max_attempts = 1\nremote_submit_max_active = 2\n"
                 "remote_submit_max_per_day = 6\n",
                 gate_val);
        atlas_gwpolicy pg;
        parse_policy(gate_text, &pg);
        T_CHECK_MSG(pg.state == ATLAS_GWPOLICY_DISABLED,
                    "a 256-byte gate line (one over the 255-byte max) was accepted");
    }
}

static void test_web_gui_anonymous_scopes_is_absent_by_default(void) {
    /* No key at all means today's behaviour exactly: a zero mask, which
     * `anonymous_ok` in the gateway treats as "not configured". */
    atlas_gwpolicy p;
    parse_policy(GOOD, &p);
    T_REQUIRE(p.state == ATLAS_GWPOLICY_ENABLED);
    T_CHECK_MSG(p.web_gui_anonymous_scopes == 0u,
                "an absent key produced a nonzero anonymous scope mask");
}

static void test_web_gui_anonymous_scopes_parses_exactly_what_was_named(void) {
    atlas_gwpolicy p;
    parse_policy("enabled = yes\ngateway_uid = 1001\nweb_gui = yes\n"
                 "web_gui_anonymous_scopes = context:read repo:read decisions:read "
                 "graph:read impact:read\n",
                 &p);
    T_REQUIRE_MSG(p.state == ATLAS_GWPOLICY_ENABLED, "a valid anonymous scope list was refused: %s",
                  atlas_gwpolicy_reason_name(p.reason));
    atlas_scope_mask want = ATLAS_SCOPE_BIT(ATLAS_SCOPE_CONTEXT_READ) |
                            ATLAS_SCOPE_BIT(ATLAS_SCOPE_REPO_READ) |
                            ATLAS_SCOPE_BIT(ATLAS_SCOPE_DECISIONS_READ) |
                            ATLAS_SCOPE_BIT(ATLAS_SCOPE_GRAPH_READ) |
                            ATLAS_SCOPE_BIT(ATLAS_SCOPE_IMPACT_READ);
    T_CHECK_MSG(p.web_gui_anonymous_scopes == want,
                "the parsed anonymous scope mask (0x%x) is not exactly what was named (0x%x)",
                (unsigned)p.web_gui_anonymous_scopes, (unsigned)want);
    /* audit:read is grantable and not named here, so it must not appear —
     * this is not a default the code invents. */
    T_CHECK(!atlas_scope_has(p.web_gui_anonymous_scopes, ATLAS_SCOPE_AUDIT_READ));

    /* audit:read is grantable and MAY be named explicitly. */
    atlas_gwpolicy p2;
    parse_policy("enabled = yes\ngateway_uid = 1001\nweb_gui = yes\n"
                 "web_gui_anonymous_scopes = audit:read\n",
                 &p2);
    T_REQUIRE(p2.state == ATLAS_GWPOLICY_ENABLED);
    T_CHECK(atlas_scope_has(p2.web_gui_anonymous_scopes, ATLAS_SCOPE_AUDIT_READ));
}

/* A16. `remote_dispose_key` and `remote_dispose_kinds` parse into a policy
 * that starts, and every kind Atlas has -- not a compiled-in subset of
 * "browser-shaped" kinds -- is nameable, exactly as
 * `atlas_decision_kind_parse` already accepts everywhere else: the operator's
 * ruling for this deployment is that every kind may be disposed of from the
 * browser, and a loader that quietly narrowed that set would only be
 * discovered the day a record of the missing kind refused it. */
static void test_remote_dispose_key_and_kinds_parse_a_complete_policy(void) {
    atlas_gwpolicy p;
    parse_policy("enabled = yes\ngateway_uid = 1001\nweb_gui = yes\ntls_mode = REVERSE_PROXY\n"
                 "remote_dispose_key = key_581e0a805cc1febe\n"
                 "remote_dispose_kinds = OPERATIONAL_FACT PARKED\n",
                 &p);
    T_REQUIRE_MSG(p.state == ATLAS_GWPOLICY_ENABLED, "a complete disposal policy was refused: %s",
                  atlas_gwpolicy_reason_name(p.reason));
    T_CHECK_MSG(strcmp(p.remote_dispose_key, "581e0a805cc1febe") == 0,
                "remote_dispose_key was stored as \"%s\", not without its \"key_\" prefix",
                p.remote_dispose_key);
    uint32_t want = ATLAS_DECISION_KIND_BIT(ATLAS_DECISION_KIND_OPERATIONAL_FACT) |
                    ATLAS_DECISION_KIND_BIT(ATLAS_DECISION_KIND_PARKED);
    T_CHECK_MSG(p.remote_dispose_kinds == want,
                "the parsed dispose-kinds mask (0x%x) is not exactly the two bits named (0x%x)",
                (unsigned)p.remote_dispose_kinds, (unsigned)want);
    T_CHECK(!p.cleartext_disposal_accepted);

    /* Every one of the eight kinds is nameable, including DECISION and
     * POLICY -- the two the plan's own default left out, and the two whose
     * absence the operator's ruling exists to correct. */
    atlas_gwpolicy p2;
    parse_policy("enabled = yes\ngateway_uid = 1001\nweb_gui = yes\ntls_mode = REVERSE_PROXY\n"
                 "remote_dispose_key = key_581e0a805cc1febe\n"
                 "remote_dispose_kinds = DECISION POLICY INVARIANT OPERATIONAL_FACT "
                 "ACCEPTED_RISK OBLIGATION PARKED REJECTED_ALTERNATIVE\n",
                 &p2);
    T_REQUIRE_MSG(p2.state == ATLAS_GWPOLICY_ENABLED,
                 "naming every decision kind was refused: %s", atlas_gwpolicy_reason_name(p2.reason));
    uint32_t all = 0u;
    for (size_t i = 0; i < atlas_decision_kind_count(); i++) {
        all |= ATLAS_DECISION_KIND_BIT(atlas_decision_kind_at(i));
    }
    T_CHECK_MSG(p2.remote_dispose_kinds == all,
                "naming every kind produced mask 0x%x, not the full set 0x%x",
                (unsigned)p2.remote_dispose_kinds, (unsigned)all);
}

/* A policy naming neither key is ENABLED with remote disposal off, which is
 * today's every policy in the field -- "the binary must still load today's
 * policy as ENABLED, because nothing in it names a key." */
static void test_remote_dispose_is_absent_by_default(void) {
    atlas_gwpolicy p;
    parse_policy(GOOD, &p);
    T_REQUIRE(p.state == ATLAS_GWPOLICY_ENABLED);
    T_CHECK_MSG(p.remote_dispose_key[0] == '\0', "an absent policy named a dispose key");
    T_CHECK_MSG(p.remote_dispose_kinds == 0u, "an absent policy produced a nonzero dispose mask");
    T_CHECK(!p.cleartext_disposal_accepted);
}

/* A16, amended 2026-09-04. The operator's written acceptance of a cleartext
 * disposal channel: one legal value, required whenever the disposal
 * credential would otherwise be offered with nothing in front of it, and
 * refused everywhere else -- see `gwpolicy.c`'s cross-checks for the argument
 * behind each direction. */
static void test_the_cleartext_disposal_acceptance(void) {
    static const char *const BASE =
        "enabled = yes\ngateway_uid = 1001\nweb_gui = yes\n"
        "remote_dispose_key = key_581e0a805cc1febe\nremote_dispose_kinds = PARKED\n";

    atlas_gwpolicy p;
    char text[512];

    /* tls_mode = NONE, accepted. */
    (void)snprintf(text, sizeof text, "%stls_mode = NONE\noperator_accepts_cleartext_disposal = yes\n",
                  BASE);
    parse_policy(text, &p);
    T_REQUIRE_MSG(p.state == ATLAS_GWPOLICY_ENABLED, "tls_mode = NONE with acceptance was refused: %s",
                  atlas_gwpolicy_reason_name(p.reason));
    T_CHECK(p.cleartext_disposal_accepted);

    /* tls_mode absent -- a loopback bind -- accepted likewise. */
    (void)snprintf(text, sizeof text, "%soperator_accepts_cleartext_disposal = yes\n", BASE);
    parse_policy(text, &p);
    T_REQUIRE_MSG(p.state == ATLAS_GWPOLICY_ENABLED,
                 "an absent tls_mode with acceptance was refused: %s",
                 atlas_gwpolicy_reason_name(p.reason));
    T_CHECK(p.cleartext_disposal_accepted);

    /* No acceptance line at all, TLS in front: ENABLED, not accepted. */
    (void)snprintf(text, sizeof text, "%stls_mode = REVERSE_PROXY\n", BASE);
    parse_policy(text, &p);
    T_REQUIRE_MSG(p.state == ATLAS_GWPOLICY_ENABLED, "a reverse-proxy disposal policy was refused: %s",
                  atlas_gwpolicy_reason_name(p.reason));
    T_CHECK_MSG(!p.cleartext_disposal_accepted,
                "a policy naming no acceptance reported cleartext_disposal_accepted");
}

/* `atlas gateway status`'s `dispose:` and `clear:` lines, in both renderers.
 * Asserted by needle, never by whole line or line count: `clear:` carries a
 * sentence an auditor reads, not a width a test pins, and no test in the tree
 * asserted the human status output before this one existed. */
static void render_status(bool json, const atlas_gwpolicy *p, atlas_buf *out) {
    atlas_err err;
    atlas_err_init(&err);
    char *buf = NULL;
    size_t len = 0;
    FILE *fp = open_memstream(&buf, &len);
    T_REQUIRE(fp != NULL);
    T_OK(atlas_service_gateway_status_for(fp, json, p, &err), &err);
    (void)fclose(fp);
    T_OK(atlas_buf_set(out, buf, len, &err), &err);
    free(buf);
}

static void test_gateway_status_prints_dispose_and_clear(void) {
    atlas_gwpolicy accepted;
    parse_policy("enabled = yes\ngateway_uid = 1001\nweb_gui = yes\ntls_mode = NONE\n"
                 "remote_dispose_key = key_581e0a805cc1febe\n"
                 "remote_dispose_kinds = OPERATIONAL_FACT PARKED\n"
                 "operator_accepts_cleartext_disposal = yes\n",
                 &accepted);
    T_REQUIRE(accepted.state == ATLAS_GWPOLICY_ENABLED);

    atlas_buf human = ATLAS_BUF_INIT;
    render_status(false, &accepted, &human);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&human), "dispose: key_581e0a805cc1febe "
                                               "(OPERATIONAL_FACT PARKED)") != NULL,
                "the human form did not print the dispose line: %s", atlas_buf_cstr(&human));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&human), "clear:   ACCEPTED") != NULL,
                "the human form did not print the accepted clear line: %s",
                atlas_buf_cstr(&human));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&human), "operator_accepts_cleartext_disposal = yes") != NULL,
                "the accepted clear line did not quote the policy key: %s",
                atlas_buf_cstr(&human));
    atlas_buf_free(&human);

    atlas_buf j = ATLAS_BUF_INIT;
    render_status(true, &accepted, &j);
    size_t bad = 0;
    T_CHECK_MSG(tjson_valid(atlas_buf_cstr(&j), j.len, &bad), "status --json is not valid JSON at %zu",
                bad);
    atlas_buf key = ATLAS_BUF_INIT;
    T_CHECK(tjson_get_string(atlas_buf_cstr(&j), j.len, "remote_dispose_key", &key));
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&key), "581e0a805cc1febe") == 0,
                "remote_dispose_key in JSON was \"%s\"", atlas_buf_cstr(&key));
    atlas_buf_free(&key);
    atlas_buf kinds = ATLAS_BUF_INIT;
    T_CHECK(tjson_get_string(atlas_buf_cstr(&j), j.len, "remote_dispose_kinds", &kinds));
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&kinds), "OPERATIONAL_FACT PARKED") == 0,
                "remote_dispose_kinds in JSON was \"%s\"", atlas_buf_cstr(&kinds));
    atlas_buf_free(&kinds);
    atlas_buf accept_raw = ATLAS_BUF_INIT;
    T_CHECK(tjson_get_raw(atlas_buf_cstr(&j), j.len, "cleartext_disposal_accepted", &accept_raw));
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&accept_raw), "true") == 0,
                "cleartext_disposal_accepted in JSON was \"%s\"", atlas_buf_cstr(&accept_raw));
    atlas_buf_free(&accept_raw);
    atlas_buf_free(&j);

    /* Not accepted, and no dispose key at all: the "(none ...)" and
     * "(not accepted ...)" wording, in both forms. */
    atlas_gwpolicy off;
    parse_policy(GOOD, &off);
    T_REQUIRE(off.state == ATLAS_GWPOLICY_ENABLED);

    atlas_buf human_off = ATLAS_BUF_INIT;
    render_status(false, &off, &human_off);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&human_off), "dispose: (none") != NULL,
                "the human form did not print the disabled dispose line: %s",
                atlas_buf_cstr(&human_off));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&human_off), "clear:   (not accepted") != NULL,
                "the human form did not print the not-accepted clear line: %s",
                atlas_buf_cstr(&human_off));
    atlas_buf_free(&human_off);

    atlas_buf json_off = ATLAS_BUF_INIT;
    render_status(true, &off, &json_off);
    atlas_buf key_off = ATLAS_BUF_INIT;
    T_CHECK(tjson_get_string(atlas_buf_cstr(&json_off), json_off.len, "remote_dispose_key", &key_off));
    T_CHECK_MSG(atlas_buf_cstr(&key_off)[0] == '\0', "an off policy named a dispose key in JSON: \"%s\"",
                atlas_buf_cstr(&key_off));
    atlas_buf_free(&key_off);
    atlas_buf kinds_off = ATLAS_BUF_INIT;
    T_CHECK(tjson_get_string(atlas_buf_cstr(&json_off), json_off.len, "remote_dispose_kinds",
                             &kinds_off));
    T_CHECK_MSG(atlas_buf_cstr(&kinds_off)[0] == '\0',
                "an off policy named dispose kinds in JSON: \"%s\"", atlas_buf_cstr(&kinds_off));
    atlas_buf_free(&kinds_off);
    atlas_buf accept_off = ATLAS_BUF_INIT;
    T_CHECK(tjson_get_raw(atlas_buf_cstr(&json_off), json_off.len, "cleartext_disposal_accepted",
                         &accept_off));
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&accept_off), "false") == 0,
                "cleartext_disposal_accepted in JSON was \"%s\" for an off policy",
                atlas_buf_cstr(&accept_off));
    atlas_buf_free(&accept_off);
    atlas_buf_free(&json_off);
}

/* A14. A complete submission policy with all seven required lines, under
 * REVERSE_PROXY (no cleartext acceptance needed). Two keys are stored, the
 * first token of the gate line has no '/', and a space in the gate line is
 * valid (gate lines allow printable ASCII including spaces). */
static void test_remote_submit_keys_parse_a_complete_policy(void) {
    atlas_gwpolicy p;
    parse_policy("enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\n"
                 "tls_mode = REVERSE_PROXY\n"
                 "remote_submit_key = key_b2578f48143c06d3\n"
                 "remote_submit_key = key_1f0a2b3c4d5e6f70\n"
                 "remote_submit_driver = claude\n"
                 "remote_submit_mode = patch\n"
                 "remote_submit_gate = make -j4\n"
                 "remote_submit_max_attempts = 1\n"
                 "remote_submit_max_active = 2\n"
                 "remote_submit_max_per_day = 6\n",
                 &p);
    T_REQUIRE_MSG(p.state == ATLAS_GWPOLICY_ENABLED,
                  "a complete submission policy was refused: %s",
                  atlas_gwpolicy_reason_name(p.reason));
    T_CHECK_MSG(p.remote_submit_count == 2u, "remote_submit_count was %zu, not 2",
                p.remote_submit_count);
    T_CHECK_MSG(strcmp(p.remote_submit_keys[0], "b2578f48143c06d3") == 0,
                "first key was stored as \"%s\"", p.remote_submit_keys[0]);
    T_CHECK_MSG(strcmp(p.remote_submit_keys[1], "1f0a2b3c4d5e6f70") == 0,
                "second key was stored as \"%s\"", p.remote_submit_keys[1]);
    T_CHECK_MSG(strcmp(p.remote_submit_driver, "claude") == 0,
                "driver was \"%s\"", p.remote_submit_driver);
    T_CHECK_MSG(strcmp(p.remote_submit_mode, "patch") == 0,
                "mode was \"%s\"", p.remote_submit_mode);
    T_CHECK_MSG(p.remote_submit_gate_count == 1u, "gate count was %zu", p.remote_submit_gate_count);
    T_CHECK_MSG(strcmp(p.remote_submit_gates[0], "make -j4") == 0,
                "gate was \"%s\"", p.remote_submit_gates[0]);
    T_CHECK_MSG(p.remote_submit_max_attempts == 1, "max_attempts was %lld",
                p.remote_submit_max_attempts);
    T_CHECK_MSG(p.remote_submit_max_active == 2, "max_active was %lld",
                p.remote_submit_max_active);
    T_CHECK_MSG(p.remote_submit_max_per_day == 6, "max_per_day was %lld",
                p.remote_submit_max_per_day);
    T_CHECK_MSG(!p.cleartext_submission_accepted,
                "a REVERSE_PROXY policy reported cleartext_submission_accepted");

    /* `web_gui = no` with submission lines is NOT MALFORMED: /mcp is a
     * submission surface independent of the browser. */
    atlas_gwpolicy p2;
    parse_policy("enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\nweb_gui = no\n"
                 "tls_mode = REVERSE_PROXY\n"
                 "remote_submit_key = key_b2578f48143c06d3\n"
                 "remote_submit_driver = claude\nremote_submit_mode = patch\n"
                 "remote_submit_gate = make\nremote_submit_max_attempts = 1\n"
                 "remote_submit_max_active = 2\nremote_submit_max_per_day = 6\n",
                 &p2);
    T_REQUIRE_MSG(p2.state == ATLAS_GWPOLICY_ENABLED,
                  "web_gui = no with submission lines was refused: %s",
                  atlas_gwpolicy_reason_name(p2.reason));

    /* The two acceptances are independent: disposal cleartext accepted does NOT
     * imply submission cleartext accepted, and the sibling that names both
     * must be ENABLED with both bools true.  This is the positive proof of the
     * MALFORMED case "disposal acceptance only with all submit lines under NONE". */
    atlas_gwpolicy p3;
    parse_policy("enabled = yes\ngateway_uid = 1001\nweb_gui = yes\ntls_mode = NONE\n"
                 "remote_dispose_key = key_581e0a805cc1febe\nremote_dispose_kinds = PARKED\n"
                 "operator_accepts_cleartext_disposal = yes\n"
                 "remote_submit_key = key_b2578f48143c06d3\n"
                 "remote_submit_driver = claude\nremote_submit_mode = patch\n"
                 "remote_submit_gate = make\nremote_submit_max_attempts = 1\n"
                 "remote_submit_max_active = 2\nremote_submit_max_per_day = 6\n"
                 "operator_accepts_cleartext_submission = yes\n",
                 &p3);
    T_REQUIRE_MSG(p3.state == ATLAS_GWPOLICY_ENABLED,
                  "a policy with both cleartext acceptances was refused: %s",
                  atlas_gwpolicy_reason_name(p3.reason));
    T_CHECK_MSG(p3.cleartext_disposal_accepted, "cleartext_disposal_accepted was false");
    T_CHECK_MSG(p3.cleartext_submission_accepted, "cleartext_submission_accepted was false");
}

/* A14. Remote submission absent by default. */
static void test_remote_submit_is_absent_by_default(void) {
    atlas_gwpolicy p;
    parse_policy(GOOD, &p);
    T_REQUIRE(p.state == ATLAS_GWPOLICY_ENABLED);
    T_CHECK_MSG(p.remote_submit_count == 0u, "an absent policy named a submit key");
    T_CHECK_MSG(!p.cleartext_submission_accepted,
                "an absent policy reported cleartext_submission_accepted");
}

/* A14. `atlas gateway status`'s `submit:` and `clear-submit:` lines, in both
 * renderers. Asserted by needle, never by whole line or line count. */
static void test_gateway_status_prints_submit_and_clear_submit(void) {
    atlas_gwpolicy accepted;
    parse_policy("enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\ntls_mode = NONE\n"
                 "remote_submit_key = key_b2578f48143c06d3\n"
                 "remote_submit_driver = claude\nremote_submit_mode = patch\n"
                 "remote_submit_gate = make\nremote_submit_max_attempts = 1\n"
                 "remote_submit_max_active = 2\nremote_submit_max_per_day = 6\n"
                 "operator_accepts_cleartext_submission = yes\n",
                 &accepted);
    T_REQUIRE(accepted.state == ATLAS_GWPOLICY_ENABLED);

    atlas_buf human = ATLAS_BUF_INIT;
    render_status(false, &accepted, &human);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&human), "submit:  key_") != NULL,
                "the human form did not print the submit key: %s", atlas_buf_cstr(&human));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&human), "checked at submit") != NULL,
                "the human form did not print 'checked at submit': %s", atlas_buf_cstr(&human));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&human), "clear-submit: ACCEPTED") != NULL,
                "the human form did not print the accepted clear-submit line: %s",
                atlas_buf_cstr(&human));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&human), "operator_accepts_cleartext_submission = yes") != NULL,
                "the accepted clear-submit line did not quote the policy key: %s",
                atlas_buf_cstr(&human));
    atlas_buf_free(&human);

    atlas_buf j = ATLAS_BUF_INIT;
    render_status(true, &accepted, &j);
    size_t bad = 0;
    T_CHECK_MSG(tjson_valid(atlas_buf_cstr(&j), j.len, &bad), "status --json is not valid JSON at %zu",
                bad);
    atlas_buf submit_raw = ATLAS_BUF_INIT;
    T_CHECK(tjson_get_raw(atlas_buf_cstr(&j), j.len, "cleartext_submission_accepted", &submit_raw));
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&submit_raw), "true") == 0,
                "cleartext_submission_accepted was \"%s\"", atlas_buf_cstr(&submit_raw));
    atlas_buf_free(&submit_raw);

    /* Verify the frozen JSON key names and that they carry the right content. */
    atlas_buf keys_raw = ATLAS_BUF_INIT;
    T_CHECK(tjson_get_raw(atlas_buf_cstr(&j), j.len, "remote_submit_keys", &keys_raw));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&keys_raw), "b2578f48143c06d3") != NULL,
                "remote_submit_keys JSON did not contain the key id: %s",
                atlas_buf_cstr(&keys_raw));
    atlas_buf_free(&keys_raw);

    atlas_buf driver_str = ATLAS_BUF_INIT;
    T_CHECK(tjson_get_string(atlas_buf_cstr(&j), j.len, "remote_submit_driver", &driver_str));
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&driver_str), "claude") == 0,
                "remote_submit_driver was \"%s\"", atlas_buf_cstr(&driver_str));
    atlas_buf_free(&driver_str);

    atlas_buf mode_str = ATLAS_BUF_INIT;
    T_CHECK(tjson_get_string(atlas_buf_cstr(&j), j.len, "remote_submit_mode", &mode_str));
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&mode_str), "patch") == 0,
                "remote_submit_mode was \"%s\"", atlas_buf_cstr(&mode_str));
    atlas_buf_free(&mode_str);
    atlas_buf_free(&j);

    /* For the off (no submit keys) policy, remote_submit_keys must be an
     * empty JSON array. */

    /* Not accepted, and no submit keys: the "(none ...)" and "(not accepted)"
     * wording. Note: "(not accepted" also appears in the dispose clear: line,
     * so assert the full "clear-submit: (not accepted" prefix. */
    atlas_gwpolicy off;
    parse_policy(GOOD, &off);
    T_REQUIRE(off.state == ATLAS_GWPOLICY_ENABLED);

    atlas_buf human_off = ATLAS_BUF_INIT;
    render_status(false, &off, &human_off);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&human_off), "submit:  (none") != NULL,
                "the human form did not print the disabled submit line: %s",
                atlas_buf_cstr(&human_off));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&human_off), "clear-submit: (not accepted") != NULL,
                "the human form did not print the not-accepted clear-submit line: %s",
                atlas_buf_cstr(&human_off));
    atlas_buf_free(&human_off);

    atlas_buf json_off = ATLAS_BUF_INIT;
    render_status(true, &off, &json_off);
    atlas_buf accept_off = ATLAS_BUF_INIT;
    T_CHECK(tjson_get_raw(atlas_buf_cstr(&json_off), json_off.len, "cleartext_submission_accepted",
                         &accept_off));
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&accept_off), "false") == 0,
                "cleartext_submission_accepted was \"%s\" for an off policy",
                atlas_buf_cstr(&accept_off));
    atlas_buf_free(&accept_off);

    /* remote_submit_keys must be an empty JSON array when no keys are set. */
    atlas_buf keys_off = ATLAS_BUF_INIT;
    T_CHECK(tjson_get_raw(atlas_buf_cstr(&json_off), json_off.len, "remote_submit_keys",
                          &keys_off));
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&keys_off), "[]") == 0,
                "remote_submit_keys was \"%s\" for a policy with no submit keys",
                atlas_buf_cstr(&keys_off));
    atlas_buf_free(&keys_off);
    atlas_buf_free(&json_off);
}

static void test_a_policy_that_says_no_is_not_malformed(void) {
    /* Present and switched off is a complete, valid policy that says no.
     * Reporting it as malformed would send an operator looking for a syntax
     * error they did not make. */
    atlas_gwpolicy p;
    parse_policy("enabled = no\ngateway_uid = 1001\nremote_mcp = yes\n", &p);
    T_CHECK(p.state == ATLAS_GWPOLICY_DISABLED);
    T_CHECK_MSG(p.reason == ATLAS_GWPOLICY_REASON_DISABLED,
                "a policy that says no reported %s", atlas_gwpolicy_reason_name(p.reason));
    /* And it must not be reported as absent: an operator who installed a policy
     * and switched it off should not be told there is no policy. */
    T_CHECK(p.reason != ATLAS_GWPOLICY_REASON_ABSENT);
    T_CHECK(strstr(atlas_gwpolicy_reason_detail(p.reason), "enabled = no") != NULL);
}

static void test_a_wider_bind_needs_a_written_tls_stance(void) {
    atlas_gwpolicy p;
    /* Refused without one... */
    parse_policy("enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\nlisten_addr = 10.1.2.3\n",
                 &p);
    T_CHECK_MSG(p.state == ATLAS_GWPOLICY_DISABLED,
                "a non-loopback bind was accepted with no TLS stance");

    /* ...and accepted with one, including the explicit NONE, which is then a
     * decision an auditor can find rather than the silent consequence of
     * leaving a key out. */
    parse_policy("enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\n"
                 "listen_addr = 10.1.2.3\ntls_mode = REVERSE_PROXY\n",
                 &p);
    T_CHECK_MSG(p.state == ATLAS_GWPOLICY_ENABLED, "a reverse-proxy deployment was refused");
    T_CHECK(p.tls_mode == ATLAS_GWPOLICY_TLS_REVERSE_PROXY);

    parse_policy("enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\n"
                 "listen_addr = 10.1.2.3\ntls_mode = NONE\n",
                 &p);
    T_CHECK(p.state == ATLAS_GWPOLICY_ENABLED);

    /* A loopback bind never needs one. */
    parse_policy("enabled = yes\ngateway_uid = 1001\nremote_mcp = yes\nlisten_addr = 127.0.0.5\n",
                 &p);
    T_CHECK_MSG(p.state == ATLAS_GWPOLICY_ENABLED, "a loopback bind was refused");

    T_CHECK(atlas_gwpolicy_is_loopback("127.0.0.1"));
    T_CHECK(atlas_gwpolicy_is_loopback("127.1.2.3"));
    T_CHECK(atlas_gwpolicy_is_loopback("::1"));
    T_CHECK(!atlas_gwpolicy_is_loopback("0.0.0.0"));
    T_CHECK(!atlas_gwpolicy_is_loopback("10.0.0.1"));
    /* A hostname that merely begins with the loopback digits is not loopback. */
    T_CHECK_MSG(!atlas_gwpolicy_is_loopback("127.evil.example.com"),
                "a hostname beginning \"127.\" was treated as loopback");
}

static void test_an_origin_must_match_whole(void) {
    atlas_gwpolicy p;
    parse_policy("enabled = yes\ngateway_uid = 1001\nweb_gui = yes\n"
                 "allowed_origin = https://atlas.example.com\n",
                 &p);
    T_REQUIRE(p.state == ATLAS_GWPOLICY_ENABLED);
    T_EQ_INT((long long)p.origin_count, 1);

    T_CHECK(atlas_http_origin_allowed(&p, "https://atlas.example.com"));
    /* The suffix attack: a check that matched by suffix would accept this. */
    T_CHECK_MSG(!atlas_http_origin_allowed(&p, "https://atlas.example.com.attacker.net"),
                "an origin was accepted by suffix");
    T_CHECK_MSG(!atlas_http_origin_allowed(&p, "https://evil.https://atlas.example.com"),
                "an origin was accepted by containment");
    /* The scheme and the port are part of the origin. */
    T_CHECK(!atlas_http_origin_allowed(&p, "http://atlas.example.com"));
    T_CHECK(!atlas_http_origin_allowed(&p, "https://atlas.example.com:8443"));
    T_CHECK(!atlas_http_origin_allowed(&p, ""));
    T_CHECK(!atlas_http_origin_allowed(&p, "null"));
}

static void test_the_loader_refuses_a_policy_anyone_could_write(void) {
    /* The property the whole policy rests on: a file this account can write is
     * not a policy, whatever it says. The fixture directory is owned by the
     * test user, so the root-anchored walk must refuse it — and the reason must
     * say so rather than reporting it as absent. */
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);

    T_OK(fx_write(fx_data_dir(&fx), "gateway.conf", GOOD, &err), &err);
    atlas_buf path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&path, &err, "%s/gateway.conf", fx_data_dir(&fx)), &err);

    atlas_gwpolicy p;
    atlas_gwpolicy_load_at(atlas_buf_cstr(&path), &p);
    T_CHECK_MSG(p.state == ATLAS_GWPOLICY_DISABLED,
                "a user-writable policy enabled the gateway");
    T_CHECK_MSG(p.reason == ATLAS_GWPOLICY_REASON_WRITABLE ||
                    p.reason == ATLAS_GWPOLICY_REASON_PATH_UNSAFE,
                "a user-writable policy reported %s rather than saying it is writable",
                atlas_gwpolicy_reason_name(p.reason));
    /* And the refusal explains what would change it. */
    T_CHECK(atlas_gwpolicy_reason_detail(p.reason) != NULL);
    T_CHECK(strlen(atlas_gwpolicy_reason_detail(p.reason)) > 0);

    atlas_buf_free(&path);
    fx_close(&fx);
}

/* --- the HTTP reader ------------------------------------------------------- */

static atlas_status parse_req(const char *text, atlas_http_request *out, atlas_err *err) {
    atlas_err_init(err);
    return atlas_http_parse_head(text, strlen(text), (int64_t)ATLAS_GW_MAX_BODY_BYTES, out, err);
}

static void test_a_well_formed_request_is_read(void) {
    atlas_http_request r;
    atlas_err err;
    atlas_status st = parse_req("POST /mcp?x=1 HTTP/1.1\r\n"
                                "Host: atlas.example.com\r\n"
                                "Authorization: Bearer atlas_token\r\n"
                                "Content-Type: application/json\r\n"
                                "Accept: application/json, text/event-stream\r\n"
                                "Origin: https://atlas.example.com\r\n"
                                "Cookie: other=1; atlas_session=abcdef; more=2\r\n"
                                "Mcp-Session-Id: s-123\r\n"
                                "Content-Length: 2\r\n"
                                "\r\n"
                                "{}",
                                &r, &err);
    T_OK(st, &err);
    T_CHECK(strcmp(r.method, "POST") == 0);
    T_CHECK_MSG(strcmp(r.path, "/mcp") == 0, "the path was \"%s\"", r.path);
    T_CHECK_MSG(strcmp(r.query, "x=1") == 0, "the query was \"%s\"", r.query);
    T_CHECK(strcmp(r.authorization, "Bearer atlas_token") == 0);
    T_CHECK(strcmp(r.content_type, "application/json") == 0);
    T_CHECK(strcmp(r.origin, "https://atlas.example.com") == 0);
    T_CHECK_MSG(strcmp(r.session, "abcdef") == 0, "the session cookie was \"%s\"", r.session);
    T_CHECK(strcmp(r.mcp_session, "s-123") == 0);
    T_CHECK(r.has_content_length && r.content_length == 2);
    T_CHECK(r.keep_alive);
    /* The body begins right after the blank line that ends the head. */
    T_CHECK(r.body_offset > 0);

    /* Freeing wipes the Authorization header: it is the one field here that
     * carries credential material. */
    atlas_http_request_free(&r);
    bool any = false;
    for (size_t i = 0; i < sizeof r.authorization; i++) {
        if (r.authorization[i] != 0) {
            any = true;
            break;
        }
    }
    T_CHECK_MSG(!any, "the Authorization header survived atlas_http_request_free");
}

static void test_only_crlfcrlf_ends_a_head(void) {
    /* A bare-LF terminator is one of the two ways a request smuggler gets two
     * parsers to disagree about where a message ends. Atlas recognises exactly
     * one spelling; the other is "not complete yet", which times out rather
     * than being interpreted. */
    T_CHECK(atlas_http_head_complete("GET / HTTP/1.1\r\n\r\n", 18) == 18);
    T_CHECK_MSG(atlas_http_head_complete("GET / HTTP/1.1\n\n", 16) == 0,
                "a bare-LF terminator ended a head");
    T_CHECK(atlas_http_head_complete("GET / HTTP/1.1\r\n", 16) == 0);
    T_CHECK(atlas_http_head_complete("", 0) == 0);
}

static void test_malformed_requests_are_refused(void) {
    struct {
        const char *name;
        const char *text;
        atlas_status expect;
    } cases[] = {
        {"chunked transfer encoding",
         "POST /mcp HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", ATLAS_ERR_USAGE},
        {"two Content-Length headers",
         "POST /mcp HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n", ATLAS_ERR_USAGE},
        {"a non-numeric Content-Length",
         "POST /mcp HTTP/1.1\r\nContent-Length: 12a\r\n\r\n", ATLAS_ERR_USAGE},
        {"a body larger than the ceiling",
         "POST /mcp HTTP/1.1\r\nContent-Length: 99999999\r\n\r\n", ATLAS_ERR_INTEGRITY},
        {"a folded header",
         "POST /mcp HTTP/1.1\r\nAccept: a\r\n b\r\n\r\n", ATLAS_ERR_USAGE},
        {"an absolute-URI target",
         "GET http://elsewhere/x HTTP/1.1\r\n\r\n", ATLAS_ERR_USAGE},
        {"an authority-form target", "CONNECT host:443 HTTP/1.1\r\n\r\n", ATLAS_ERR_USAGE},
        {"a lowercase method", "get / HTTP/1.1\r\n\r\n", ATLAS_ERR_USAGE},
        {"an unsupported version", "GET / HTTP/2.0\r\n\r\n", ATLAS_ERR_USAGE},
        {"no version at all", "GET /\r\n\r\n", ATLAS_ERR_USAGE},
        {"three fields in the request line", "GET / x HTTP/1.1\r\n\r\n", ATLAS_ERR_USAGE},
        {"a leading space", " GET / HTTP/1.1\r\n\r\n", ATLAS_ERR_USAGE},
        {"a header with no colon", "GET / HTTP/1.1\r\nNotAHeader\r\n\r\n", ATLAS_ERR_USAGE},
        {"an empty request line", "\r\n\r\n", ATLAS_ERR_USAGE},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        atlas_http_request r;
        atlas_err err;
        atlas_status st = parse_req(cases[i].text, &r, &err);
        T_CHECK_MSG(st == cases[i].expect, "%s produced %s rather than %s", cases[i].name,
                    atlas_status_name(st), atlas_status_name(cases[i].expect));
        /* No refusal reproduces what was sent. */
        T_CHECK_MSG(strstr(atlas_err_msg(&err), "elsewhere") == NULL,
                    "%s echoed the request target", cases[i].name);
        atlas_http_request_free(&r);
    }
}

static void test_a_control_byte_never_survives_a_header(void) {
    /* This is the audit-log injection defence at its source: a header value
     * carrying a control byte is a malformed request, refused rather than
     * sanitised. Nothing downstream has to remember to encode it because
     * nothing downstream ever receives it. */
    atlas_http_request r;
    atlas_err err;
    static const char REQ[] = "GET / HTTP/1.1\r\nOrigin: https://a.example\x01\x0c\r\n\r\n";
    atlas_status st = atlas_http_parse_head(REQ, sizeof REQ - 1u,
                                            (int64_t)ATLAS_GW_MAX_BODY_BYTES, &r, &err);
    T_CHECK_MSG(st != ATLAS_OK, "a header carrying a control byte was accepted");
    atlas_http_request_free(&r);
}

static void test_a_path_is_never_decoded_and_never_a_filesystem_path(void) {
    /* Routes are exact literals, so an encoded traversal is simply a path that
     * matches nothing. Decoding it would create the risk that not decoding it
     * avoids. */
    atlas_http_request r;
    atlas_err err;
    T_OK(parse_req("GET /api/v1/%2e%2e%2f%2e%2e%2fetc%2fpasswd HTTP/1.1\r\n\r\n", &r, &err), &err);
    T_CHECK_MSG(strcmp(r.path, "/api/v1/%2e%2e%2f%2e%2e%2fetc%2fpasswd") == 0,
                "the path was decoded to \"%s\"", r.path);
    T_CHECK_MSG(strstr(r.path, "..") == NULL, "a decoded traversal appeared in the path");
    atlas_http_request_free(&r);
}

static void test_every_response_carries_the_security_headers(void) {
    atlas_http_response resp;
    atlas_http_response_init(&resp);
    resp.status = 401;
    resp.body = "{}";
    resp.body_len = 2;
    char head[ATLAS_HTTP_RESPONSE_HEAD_MAX];
    size_t n = atlas_http_write_head(&resp, head, sizeof head);
    T_REQUIRE(n > 0);
    head[n] = '\0';

    /* On an error response, which is the one most likely to be rendered in a
     * browser by accident. */
    T_CHECK(strstr(head, "HTTP/1.1 401 Unauthorized") != NULL);
    T_CHECK(strstr(head, "X-Content-Type-Options: nosniff") != NULL);
    T_CHECK(strstr(head, "X-Frame-Options: DENY") != NULL);
    T_CHECK(strstr(head, "Referrer-Policy: no-referrer") != NULL);
    T_CHECK(strstr(head, "Content-Security-Policy: default-src 'none'") != NULL);
    T_CHECK(strstr(head, "Cache-Control: no-store") != NULL);
    T_CHECK(strstr(head, "Content-Length: 2") != NULL);
    /* No CORS header at all when no origin was allowed, which is what makes a
     * browser refuse the response. */
    T_CHECK_MSG(strstr(head, "Access-Control-Allow-Origin") == NULL,
                "an unallowed request got a CORS header");

    resp.allow_origin = "https://atlas.example.com";
    n = atlas_http_write_head(&resp, head, sizeof head);
    T_REQUIRE(n > 0);
    head[n] = '\0';
    T_CHECK(strstr(head, "Access-Control-Allow-Origin: https://atlas.example.com") != NULL);
    T_CHECK(strstr(head, "Access-Control-Allow-Credentials: true") != NULL);
    T_CHECK_MSG(strstr(head, "Vary: Origin") != NULL,
                "a credentialed CORS response did not vary on Origin");
}

/* A15 T1, fix round 1. The route table adds no behaviour by itself; what has
 * to hold is a property of every row, not a count of them.
 * `atlas_gateway_api_routes()` hands the test the same three fields
 * `api_handle` matches against.
 *
 * The first version of this test asserted only absence from a few known-write
 * groups and a hand-kept name list, which a route naming `verify.evaluate`,
 * `decision.propose`, `decision.revise`, `decision.link_add`,
 * `verify.claim_create` or `verify.attestation_add` would have passed: none of
 * those names is in `OPERATOR_METHODS[]`, none matches the old exact list, and
 * none carries a `job.` or `dispatch.` prefix, yet every one of them is a
 * write in the ordinary dispatch group (`src/ipc/server.c`'s `dispatch()`
 * consults `VERIFY_METHODS[]` and `DECISION_METHODS[]` with no peer
 * predicate), reachable by the gateway uid the moment a route named it. A
 * negative list can only ever list what somebody already thought of.
 *
 * `READ_METHODS[]` below is the actual guarantee: a positive allowlist of
 * every method `API_ROUTES[]` names as what it forwards to, today. A name is
 * added to it only on purpose, never because it merely failed to match one of
 * the negative checks that follow. A row naming anything else fails loudly,
 * on this line, the moment it is added; a count would have drifted silently
 * instead. This is the same shape `HOOK_EVENTS[]` (`src/hook/hook.c`) and the
 * `git config` argument vectors (`src/git/git.c`) already use in this
 * repository: a positive, hand-reviewed list that can only be widened on
 * purpose. `route_count` is still used only as the loop bound and is never
 * asserted -- a table that grows or shrinks by a row must not change whether
 * this test passes, only the property of each row must.
 *
 * Fix round 2: not every name on that list names a method that exists. Three
 * of the twenty-six do not reach any dispatch table in `src/ipc/` at all:
 * `code.search`, `sem.callers` and `sem.callees`. `api_handle` forwards
 * `route->method` over IPC verbatim; `dispatch()` (`src/ipc/server.c`) walks
 * its tables for that exact name, finds none of these three anywhere, and
 * answers `unknown method`. The real methods are `code.symbol.search`
 * (`src/ipc/server_code.c:924`) for a code search, and one method for both
 * call-graph directions -- `sem.graph` (`src/ipc/server_sem.c:1033`), which
 * picks `callers` versus `callees` from an `inbound` boolean parameter that
 * `/api/v1/sem/callers` and `/api/v1/sem/callees` do not currently forward.
 * So `/api/v1/code/search`, `/api/v1/sem/callers` and `/api/v1/sem/callees`
 * are three dead routes: every request to them fails at the daemon, not at
 * the gateway, and always has, since long before A15. This test does not fix
 * them -- `code/search` is a one-word row edit, but the two `sem` routes need
 * either a new fixed-parameter field in `api_route` or a client-forwarded
 * `inbound` name, and both change the shape of `API_ROUTES[]` that this
 * season's threat argument rests on not changing, so A15 leaves them as they
 * are and `docs/backlog.md` records the defect. They stay on
 * `READ_METHODS[]` rather than being removed: removing them would fail this
 * test against a table nobody is fixing this season, and a name absent from
 * the table it forwards to is still, truthfully, a name the table forwards
 * to. So for twenty-three of the twenty-six names, membership means what the
 * paragraph above says -- established as a read the gateway uid may make. For
 * these three it means only that `API_ROUTES[]` names them; nothing reaches
 * them to be a read or a write at all.
 *
 * The negative checks kept below are the stated reasons, not the guarantee:
 * they document *why* several specific names must never appear, each citing
 * the table it mirrors. Fix round 1 found the first version's own copy of the
 * backup group's names had already drifted from `BACKUP_METHODS[]` -- missing
 * `operation.get` and `code.sem_config` -- and that its `job.`/`dispatch.`
 * prefix pair did not cover `ORCH_CLIENT_METHODS[]`'s four `plan.` methods.
 * Both groups are read here directly from their own accessor functions
 * (`atlas_server_backup_methods`, `atlas_server_apikey_methods`,
 * `atlas_server_orch_client_methods`, `atlas_server_orch_dispatch_methods`)
 * rather than restated as a second literal, which is the cheap derivation the
 * fix round asked for: none of these four can drift from the table it
 * documents, because each loop reads that table itself instead of a copy of
 * it. `gateway.auth` and `gateway.audit` stay as two exact names -- the third
 * member of the same `GATEWAY_METHODS[]` table, `gateway.audit_list`, is
 * itself the method behind the `/api/v1/audit` route, so deriving this one
 * from its table would need an exclusion rather than a plain membership test;
 * two names is not worth that. */

/* Every method API_ROUTES[] forwards to, as of this fix round. Not generated
 * from the table -- a name missing here is a build-time-visible test failure,
 * never a silent gap, which is the point. File scope, not local to the test
 * function below: A16's write-route test reuses it, as the negative half of
 * its own membership check -- a write route naming one of these would be a
 * read forwarded through the write table, which the write table's own frozen
 * shape (every row `decisions:dispose`, nothing else) must never permit. */
static const char *const READ_METHODS[] = {
    "daemon.status",    "repo.list",          "repo.state",     "events.since",
    "repo.search",      "repo.file",          "repo.history",   "decision.list",
    "decision.get",     "decision.history",   "gate.check",     "code.status",
    "code.file",        "code.symbol",        "code.search",    "sem.status",
    "sem.symbol",       "sem.callers",        "sem.callees",    "sem.impact",
    "code.impact",      "sem.context",        "verify.claims",  "verify.show",
    "verify.policy",    "gateway.audit_list",
};

static void test_every_api_route_forwards_to_a_read_on_the_reviewed_allowlist(void) {
    size_t operator_count = 0;
    const atlas_method_entry *operator_methods = atlas_server_operator_methods(&operator_count);
    T_REQUIRE(operator_methods != NULL || operator_count == 0);

    size_t backup_count = 0;
    const atlas_method_entry *backup_methods = atlas_server_backup_methods(&backup_count);
    T_REQUIRE(backup_methods != NULL || backup_count == 0);

    size_t apikey_count = 0;
    const atlas_method_entry *apikey_methods = atlas_server_apikey_methods(&apikey_count);
    T_REQUIRE(apikey_methods != NULL || apikey_count == 0);

    size_t orch_client_count = 0;
    const atlas_method_entry *orch_client_methods =
        atlas_server_orch_client_methods(&orch_client_count);
    T_REQUIRE(orch_client_methods != NULL || orch_client_count == 0);

    size_t orch_dispatch_count = 0;
    const atlas_method_entry *orch_dispatch_methods =
        atlas_server_orch_dispatch_methods(&orch_dispatch_count);
    T_REQUIRE(orch_dispatch_methods != NULL || orch_dispatch_count == 0);

    static const char *const GATEWAY_ONLY = "gateway.auth";
    static const char *const GATEWAY_AUDIT = "gateway.audit";

    size_t route_count = 0;
    const atlas_gateway_route_view *routes = atlas_gateway_api_routes(&route_count);
    T_REQUIRE(routes != NULL);

    for (size_t i = 0; i < route_count; i++) {
        const atlas_gateway_route_view *r = &routes[i];

        bool allowed = false;
        for (size_t j = 0; j < sizeof READ_METHODS / sizeof READ_METHODS[0]; j++) {
            if (strcmp(r->method, READ_METHODS[j]) == 0) {
                allowed = true;
                break;
            }
        }
        T_CHECK_MSG(allowed, "route %s forwards to %s, which is not on the reviewed read allowlist",
                    r->path, r->method);

        for (size_t j = 0; j < operator_count; j++) {
            T_CHECK_MSG(strcmp(r->method, operator_methods[j].name) != 0,
                        "route %s forwards to operator method %s", r->path, r->method);
        }
        for (size_t j = 0; j < backup_count; j++) {
            T_CHECK_MSG(strcmp(r->method, backup_methods[j].name) != 0,
                        "route %s forwards to backup-group method %s", r->path, r->method);
        }
        for (size_t j = 0; j < apikey_count; j++) {
            T_CHECK_MSG(strcmp(r->method, apikey_methods[j].name) != 0,
                        "route %s forwards to apikey-group method %s", r->path, r->method);
        }
        for (size_t j = 0; j < orch_client_count; j++) {
            T_CHECK_MSG(strcmp(r->method, orch_client_methods[j].name) != 0,
                        "route %s forwards to orchestration client method %s", r->path,
                        r->method);
        }
        for (size_t j = 0; j < orch_dispatch_count; j++) {
            T_CHECK_MSG(strcmp(r->method, orch_dispatch_methods[j].name) != 0,
                        "route %s forwards to orchestration dispatch method %s", r->path,
                        r->method);
        }

        T_CHECK_MSG(strcmp(r->method, GATEWAY_ONLY) != 0, "route %s forwards to %s", r->path,
                    GATEWAY_ONLY);
        T_CHECK_MSG(strcmp(r->method, GATEWAY_AUDIT) != 0, "route %s forwards to %s", r->path,
                    GATEWAY_AUDIT);

        T_CHECK_MSG(atlas_apikey_scope_grantable(r->scope), "route %s scope is not grantable",
                    r->path);
        T_CHECK_MSG(r->scope != ATLAS_SCOPE_UNKNOWN, "route %s scope is ATLAS_SCOPE_UNKNOWN",
                    r->path);
    }
}

/* A16. The write table's own version of the property test above: every row
 * forwards to exactly one of the two remote-disposal methods, needs exactly
 * `ATLAS_SCOPE_DECISIONS_DISPOSE`, and that scope is not grantable to an
 * ordinary credential -- the whole reason a browser can dispose of a record
 * at all without a write-capable credential ever existing for anyone to
 * steal. The count is the loop bound only, on the read test's own precedent:
 * a table that grows or shrinks by a row must not change whether this test
 * passes, only the property of each row must. */
static void test_every_write_route_is_a_disposal_on_the_reviewed_allowlist(void) {
    size_t operator_count = 0;
    const atlas_method_entry *operator_methods = atlas_server_operator_methods(&operator_count);
    T_REQUIRE(operator_methods != NULL || operator_count == 0);

    static const char *const GATEWAY_ONLY = "gateway.auth";
    static const char *const GATEWAY_AUDIT = "gateway.audit";

    /* The whole write table, by name -- exactly the two methods A16 adds. Not
     * generated from the table, on `READ_METHODS[]`'s own precedent above: a
     * third row naming anything else fails loudly here, the moment it is
     * added. */
    static const char *const WRITE_METHODS[] = {
        "decision.remote_challenge",
        "decision.remote_dispose",
    };

    size_t route_count = 0;
    const atlas_gateway_route_view *routes = atlas_gateway_api_write_routes(&route_count);
    T_REQUIRE(routes != NULL);

    for (size_t i = 0; i < route_count; i++) {
        const atlas_gateway_route_view *r = &routes[i];

        bool allowed = false;
        for (size_t j = 0; j < sizeof WRITE_METHODS / sizeof WRITE_METHODS[0]; j++) {
            if (strcmp(r->method, WRITE_METHODS[j]) == 0) {
                allowed = true;
                break;
            }
        }
        T_CHECK_MSG(allowed,
                    "write route %s forwards to %s, which is not one of the two disposal methods",
                    r->path, r->method);

        T_CHECK_MSG(r->scope == ATLAS_SCOPE_DECISIONS_DISPOSE,
                    "write route %s does not need decisions:dispose", r->path);
        T_CHECK_MSG(!atlas_apikey_scope_grantable(r->scope),
                    "write route %s's scope is grantable to an ordinary credential", r->path);

        for (size_t j = 0; j < sizeof READ_METHODS / sizeof READ_METHODS[0]; j++) {
            T_CHECK_MSG(strcmp(r->method, READ_METHODS[j]) != 0,
                        "write route %s forwards to the read method %s", r->path, r->method);
        }
        for (size_t j = 0; j < operator_count; j++) {
            T_CHECK_MSG(strcmp(r->method, operator_methods[j].name) != 0,
                        "write route %s forwards to operator method %s", r->path, r->method);
        }
        T_CHECK_MSG(strcmp(r->method, GATEWAY_ONLY) != 0, "write route %s forwards to %s", r->path,
                    GATEWAY_ONLY);
        T_CHECK_MSG(strcmp(r->method, GATEWAY_AUDIT) != 0, "write route %s forwards to %s", r->path,
                    GATEWAY_AUDIT);
    }

    /* The existing read-table test still passes unchanged (it is not touched
     * by this test at all), and no read row gained the write scope while
     * this table was added beside it. */
    size_t read_count = 0;
    const atlas_gateway_route_view *read_routes = atlas_gateway_api_routes(&read_count);
    T_REQUIRE(read_routes != NULL);
    for (size_t i = 0; i < read_count; i++) {
        T_CHECK_MSG(read_routes[i].scope != ATLAS_SCOPE_DECISIONS_DISPOSE,
                    "read route %s carries the write scope", read_routes[i].path);
    }
}

static const atlas_test TESTS[] = {
    {"a complete policy enables the gateway", test_a_complete_policy_enables_the_gateway},
    {"a zeroed policy authorises nothing", test_a_zeroed_policy_authorises_nothing},
    {"every malformed policy disables the gateway",
     test_every_malformed_policy_disables_the_gateway},
    {"web_gui_anonymous_scopes is absent by default",
     test_web_gui_anonymous_scopes_is_absent_by_default},
    {"web_gui_anonymous_scopes parses exactly what was named",
     test_web_gui_anonymous_scopes_parses_exactly_what_was_named},
    {"remote_dispose_key and remote_dispose_kinds parse a complete policy",
     test_remote_dispose_key_and_kinds_parse_a_complete_policy},
    {"remote disposal is absent by default", test_remote_dispose_is_absent_by_default},
    {"the cleartext disposal acceptance", test_the_cleartext_disposal_acceptance},
    {"remote submit keys parse a complete policy",
     test_remote_submit_keys_parse_a_complete_policy},
    {"remote submission is absent by default", test_remote_submit_is_absent_by_default},
    {"gateway status prints dispose and clear", test_gateway_status_prints_dispose_and_clear},
    {"gateway status prints submit and clear-submit",
     test_gateway_status_prints_submit_and_clear_submit},
    {"a policy that says no is not malformed", test_a_policy_that_says_no_is_not_malformed},
    {"a wider bind needs a written TLS stance", test_a_wider_bind_needs_a_written_tls_stance},
    {"an origin must match whole", test_an_origin_must_match_whole},
    {"the loader refuses a policy anyone could write",
     test_the_loader_refuses_a_policy_anyone_could_write},
    {"a well-formed request is read", test_a_well_formed_request_is_read},
    {"only CRLFCRLF ends a head", test_only_crlfcrlf_ends_a_head},
    {"malformed requests are refused", test_malformed_requests_are_refused},
    {"a control byte never survives a header", test_a_control_byte_never_survives_a_header},
    {"a path is never decoded and never a filesystem path",
     test_a_path_is_never_decoded_and_never_a_filesystem_path},
    {"every response carries the security headers",
     test_every_response_carries_the_security_headers},
    {"every API route forwards to a read on the reviewed allowlist",
     test_every_api_route_forwards_to_a_read_on_the_reviewed_allowlist},
    {"every write route is a disposal on the reviewed allowlist",
     test_every_write_route_is_a_disposal_on_the_reviewed_allowlist},
};

ATLAS_TEST_MAIN("gateway", TESTS)
