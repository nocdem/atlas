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
#include <string.h>

#include "atlas/gwpolicy.h"
#include "atlas/http.h"
#include "atlas_test.h"
#include "support/fixture.h"

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
}

static void test_a_policy_that_says_no_is_not_malformed(void) {
    /* Present and switched off is a complete, valid policy that says no.
     * Reporting it as malformed would send an operator looking for a syntax
     * error they did not make. */
    atlas_gwpolicy p;
    parse_policy("enabled = no\ngateway_uid = 1001\nremote_mcp = yes\n", &p);
    T_CHECK(p.state == ATLAS_GWPOLICY_DISABLED);
    T_CHECK_MSG(p.reason == ATLAS_GWPOLICY_REASON_ABSENT,
                "a policy that says no reported %s", atlas_gwpolicy_reason_name(p.reason));
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

static const atlas_test TESTS[] = {
    {"a complete policy enables the gateway", test_a_complete_policy_enables_the_gateway},
    {"a zeroed policy authorises nothing", test_a_zeroed_policy_authorises_nothing},
    {"every malformed policy disables the gateway",
     test_every_malformed_policy_disables_the_gateway},
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
};

ATLAS_TEST_MAIN("gateway", TESTS)
