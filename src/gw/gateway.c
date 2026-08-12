/* Atlas - A9: the remote gateway.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See atlas/gateway.h for what this is and what it structurally cannot do.
 *
 * The shape of this file is deliberate: `atlas_gateway_serve_bytes` takes a
 * complete request and returns a complete response with no socket anywhere near
 * it, and the socket loop at the bottom contains no behaviour of its own. Every
 * route, every refusal and the whole authentication path is therefore testable
 * against a real fixture daemon without opening a listening port.
 */
#define _GNU_SOURCE 1

#include "atlas/gateway.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "atlas/atlas.h"
#include "atlas/hmac.h"
#include "atlas/ipc.h"
#include "atlas/json.h"
#include "atlas/limits.h"
#include "atlas/safetext.h"
#include "mcp/mcp_internal.h"

struct atlas_gateway {
    atlas_gwpolicy policy;
    atlas_buf socket;
    int timeout_ms;
    FILE *errout;
    atlas_safe_pool safe;

    /* A fixed-window counter. Crude on purpose: a leaky bucket per peer would
     * need per-peer state, and behind a reverse proxy there is only one peer
     * unless `trust_forwarded_for` is set — so the sophistication would buy
     * nothing while looking as though it did. What this bounds is the total
     * request rate the gateway will forward, which is the thing that protects
     * the daemon. Stated as such in `docs/remote-access.md`. */
    int64_t window_started_ms;
    long long window_count;
};

/* --- diagnostics ----------------------------------------------------------- */

static void gw_log(atlas_gateway *g, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void gw_log(atlas_gateway *g, const char *fmt, ...) {
    if (g == NULL || g->errout == NULL) {
        return;
    }
    char ts[ATLAS_TS_MAX];
    atlas_now_iso8601(ts, sizeof ts);
    (void)fprintf(g->errout, "%s atlas-gateway ", ts);
    va_list ap;
    va_start(ap, fmt);
    (void)vfprintf(g->errout, fmt, ap);
    va_end(ap);
    (void)fputc('\n', g->errout);
    (void)fflush(g->errout);
}

/* --- lifecycle ------------------------------------------------------------- */

atlas_status atlas_gateway_open(const atlas_gwpolicy *policy, const atlas_gateway_opts *opts,
                                atlas_gateway **out, atlas_err *err) {
    *out = NULL;
    if (policy == NULL || policy->state != ATLAS_GWPOLICY_ENABLED) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "the gateway policy does not enable a gateway");
    }
    atlas_gateway *g = calloc(1, sizeof(*g));
    if (g == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory opening the gateway");
    }
    /* Copied. A gateway whose policy could change under it would be a gateway
     * whose bounds are not the ones an operator read. */
    g->policy = *policy;
    atlas_buf_init(&g->socket);
    atlas_safe_pool_init(&g->safe);
    g->timeout_ms = (opts != NULL && opts->timeout_ms > 0) ? opts->timeout_ms
                                                           : ATLAS_GW_UPSTREAM_TIMEOUT_MS;
    g->errout = opts != NULL ? opts->errout : NULL;

    atlas_status st = ATLAS_OK;
    if (opts != NULL && opts->socket_path != NULL) {
        st = atlas_buf_set_str(&g->socket, opts->socket_path, err);
    } else {
        st = atlas_ipc_socket_path(&g->socket, err);
    }
    if (st != ATLAS_OK) {
        atlas_gateway_close(g);
        return st;
    }
    *out = g;
    return ATLAS_OK;
}

void atlas_gateway_close(atlas_gateway *g) {
    if (g == NULL) {
        return;
    }
    atlas_safe_pool_free(&g->safe);
    atlas_buf_free(&g->socket);
    free(g);
}

/* --- responses ------------------------------------------------------------- */

/* Builds one complete response. Every route ends here, so no route can invent a
 * header set or forget the security headers. */
static atlas_status respond(atlas_gateway *g, const atlas_http_request *req, int status,
                            const char *content_type, const void *body, size_t body_len,
                            const char *extra, atlas_buf *out, atlas_err *err) {
    atlas_http_response r;
    atlas_http_response_init(&r);
    r.status = status;
    r.content_type = content_type;
    r.body = body;
    r.body_len = body_len;
    r.extra = extra;
    /* Every response closes. Keep-alive would mean holding a slot for a peer
     * that may send nothing else, and the concurrency ceiling is what bounds
     * this process's exposure — so a connection is worth one request. */
    r.keep_alive = false;
    /* CORS only for an origin the policy allows, and never otherwise: a
     * response with no `Access-Control-Allow-Origin` is one a browser refuses
     * to hand to script, which is the whole mechanism. */
    if (req != NULL && atlas_http_origin_allowed(&g->policy, req->origin)) {
        r.allow_origin = req->origin;
    }

    char head[ATLAS_HTTP_RESPONSE_HEAD_MAX];
    size_t n = atlas_http_write_head(&r, head, sizeof head);
    if (n == 0) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "the response head does not fit");
    }
    atlas_buf_reset(out);
    atlas_status st = atlas_buf_append(out, head, n, err);
    if (st == ATLAS_OK && body_len > 0) {
        st = atlas_buf_append(out, body, body_len, err);
    }
    return st;
}

/* A JSON error document, in the shape the rest of Atlas uses so a caller has one
 * vocabulary rather than two.
 *
 * `message` is always fixed Atlas text or an already-safe-encoded value. No
 * credential material, no header, no request body and no part of a token ever
 * reaches here — a refusal is a place a secret gets written down. */
static atlas_status respond_error(atlas_gateway *g, const atlas_http_request *req, int status,
                                  const char *code, const char *message, atlas_buf *out,
                                  atlas_err *err) {
    /* Built with the streaming writer over a memory stream, never by
     * formatting JSON by hand. There is no "write these bytes as JSON"
     * primitive anywhere in Atlas, and an error document is exactly the place a
     * hand-quoted string would eventually meet a value containing a quote. */
    char *raw = NULL;
    size_t raw_len = 0;
    FILE *mem = open_memstream(&raw, &raw_len);
    if (mem == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "cannot open an error buffer");
    }
    atlas_err berr;
    atlas_err_init(&berr);
    atlas_json *j = atlas_json_new(mem, &berr);
    atlas_status st = ATLAS_OK;
    if (j == NULL) {
        st = berr.status != ATLAS_OK ? berr.status : ATLAS_ERR_INTERNAL;
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_begin(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(j, "ok", false, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key(j, "error", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_begin(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "code", code, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "message", message, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_finish(j, err);
    } else if (j != NULL) {
        atlas_json_free(j);
    }
    (void)fflush(mem);
    (void)fclose(mem);
    if (st == ATLAS_OK) {
        st = respond(g, req, status, "application/json", raw, raw_len, NULL, out, err);
    }
    free(raw);
    return st;
}

/* --- the principal --------------------------------------------------------- */

typedef struct principal {
    bool authenticated;
    char key_id[ATLAS_APIKEY_SELECTOR_HEX + 1];
    char label[ATLAS_APIKEY_LABEL_MAX + 1];
    atlas_scope_mask scopes;
} principal;

/* Authenticates the request's bearer credential by asking the daemon.
 *
 * The gateway cannot verify a token itself: the verifier lives in the index and
 * the index is 0700 `atlasd`. That is the separation A9 wanted, and this round
 * trip is what it costs.
 *
 * Every failure — no header, a malformed one, a token of the wrong shape, an
 * unknown selector, a wrong secret, a revoked credential — produces the same
 * outward answer. A caller that could tell them apart would learn which half of
 * a guess was right. */
static void authenticate(atlas_gateway *g, const atlas_http_request *req, principal *out) {
    memset(out, 0, sizeof(*out));

    char token[ATLAS_APIKEY_TOKEN_MAX];
    atlas_err perr;
    atlas_err_init(&perr);
    if (atlas_apikey_bearer_parse(req->authorization, token, sizeof token, &perr) != ATLAS_OK) {
        return;
    }

    atlas_buf params = ATLAS_BUF_INIT;
    atlas_ipc_params *p = NULL;
    atlas_json *j = NULL;
    atlas_err berr;
    atlas_err_init(&berr);
    if (atlas_ipc_params_begin(&p, &j, &berr) != ATLAS_OK) {
        memset(token, 0, sizeof token);
        return;
    }
    /* Built with the typed writer, never by formatting a string: there is no
     * "write these bytes as JSON" primitive anywhere in Atlas, and a token
     * spliced into a hand-built document would be a token that could close it. */
    if (atlas_json_key_str(j, "token", token, &berr) != ATLAS_OK ||
        atlas_ipc_params_finish(p, &params, &berr) != ATLAS_OK) {
        atlas_ipc_params_abort(p);
        memset(token, 0, sizeof token);
        atlas_buf_free(&params);
        return;
    }
    /* The plaintext leaves this function's stack now. It exists in `params`
     * until that is freed, and nowhere else. */
    memset(token, 0, sizeof token);

    atlas_buf resp = ATLAS_BUF_INIT;
    atlas_err cerr;
    atlas_err_init(&cerr);
    atlas_status cst = atlas_ipc_call_timeout(atlas_buf_cstr(&g->socket), "gateway.auth",
                                              atlas_buf_cstr(&params), g->timeout_ms, &resp, &cerr);
    /* Wiped rather than merely freed: it held the presented secret. */
    if (params.data != NULL) {
        volatile unsigned char *q = (volatile unsigned char *)params.data;
        for (size_t i = 0; i < params.len; i++) {
            q[i] = 0;
        }
    }
    atlas_buf_free(&params);

    if (cst != ATLAS_OK) {
        gw_log(g, "the daemon did not answer gateway.auth: %s",
               atlas_safe(&g->safe, atlas_err_msg(&cerr)));
        atlas_buf_free(&resp);
        return;
    }

    atlas_ipc_response *r = NULL;
    atlas_err rerr;
    atlas_err_init(&rerr);
    if (atlas_ipc_response_parse(resp.data, resp.len, &r, &rerr) == ATLAS_OK) {
        bool yes = false;
        if (!atlas_ipc_response_ok(r)) {
            /* The daemon refused the *method*, which is a deployment fault
             * rather than a credential one: this gateway's uid is not the one
             * the daemon recognises. Logged, because otherwise every request
             * fails as "unauthenticated" and an operator has nothing to read.
             * The client is still told only that authentication failed. */
            gw_log(g, "the daemon refused gateway.auth: %s",
                   atlas_safe(&g->safe, atlas_ipc_response_message(r)));
        }
        if (atlas_ipc_response_ok(r) && atlas_ipc_result_bool(r, "authenticated", &yes) && yes) {
            const char *v = NULL;
            if (atlas_ipc_result_str(r, "key_id", &v) && v != NULL) {
                (void)snprintf(out->key_id, sizeof out->key_id, "%s", v);
            }
            if (atlas_ipc_result_str(r, "label", &v) && v != NULL) {
                (void)snprintf(out->label, sizeof out->label, "%s", v);
            }
            if (atlas_ipc_result_str(r, "scopes", &v) && v != NULL) {
                atlas_err serr;
                atlas_err_init(&serr);
                /* A scope list this binary cannot parse leaves the mask zero,
                 * which grants nothing. Fail closed. */
                if (atlas_apikey_scopes_parse(v, &out->scopes, &serr) != ATLAS_OK) {
                    out->scopes = 0u;
                }
            }
            out->authenticated = true;
        }
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&resp);
}

/* --- audit ----------------------------------------------------------------- */

/* Records one request. Fire and forget in every sense: the daemon queues it
 * without waiting for the write, and this ignores whatever comes back.
 *
 * A9.6 requires that audit failure must not break request handling. Here that is
 * structural — there is no path by which the outcome of this call reaches the
 * response. Atlas prefers answering with a gap in the trail to refusing a
 * request because it could not write one; the trade is stated in
 * `docs/remote-access.md`.
 *
 * Nothing here carries a secret. `operation` is a fixed route name or a tool
 * name matched against the compiled-in table, `detail` is fixed Atlas text, and
 * the Authorization header is not reachable from this function. */
static void audit(atlas_gateway *g, const char *iface, const principal *pr, const char *operation,
                  bool allowed, bool ok, int status, int64_t duration_ms, const char *detail) {
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_ipc_params *p = NULL;
    atlas_json *j = NULL;
    atlas_err err;
    atlas_err_init(&err);
    if (atlas_ipc_params_begin(&p, &j, &err) != ATLAS_OK) {
        return;
    }
    atlas_status st = atlas_json_key_str(j, "interface", iface, &err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "key_id", pr != NULL ? pr->key_id : "", &err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "label", pr != NULL ? pr->label : "", &err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "operation", operation, &err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(j, "allowed", allowed, &err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "outcome", ok ? "OK" : "FAILED", &err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "status", status, &err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "duration_ms", duration_ms, &err);
    }
    if (st == ATLAS_OK && detail != NULL) {
        st = atlas_json_key_str(j, "detail", detail, &err);
    }
    if (st != ATLAS_OK) {
        atlas_ipc_params_abort(p);
        atlas_buf_free(&params);
        return;
    }
    if (atlas_ipc_params_finish(p, &params, &err) != ATLAS_OK) {
        atlas_buf_free(&params);
        return;
    }
    atlas_buf resp = ATLAS_BUF_INIT;
    atlas_err cerr;
    atlas_err_init(&cerr);
    (void)atlas_ipc_call_timeout(atlas_buf_cstr(&g->socket), "gateway.audit",
                                 atlas_buf_cstr(&params), g->timeout_ms, &resp, &cerr);
    atlas_buf_free(&resp);
    atlas_buf_free(&params);
}

/* --- rate limiting --------------------------------------------------------- */

static int64_t now_ms(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* True when this request is within the window's budget.
 *
 * Counted before authentication, so a flood of unauthenticated requests is
 * bounded too — that is the case worth bounding, since an authenticated one at
 * least costs an attacker a credential. */
static bool rate_ok(atlas_gateway *g) {
    int64_t t = now_ms();
    if (g->window_started_ms == 0 || t - g->window_started_ms >= 60000) {
        g->window_started_ms = t;
        g->window_count = 0;
    }
    g->window_count++;
    return g->window_count <= g->policy.rate_limit_per_minute;
}

/* --- the MCP route --------------------------------------------------------- */

/* Runs one MCP message through the *same* tool implementations the stdio adapter
 * uses.
 *
 * The mechanism is an `open_memstream`: the MCP server writes its response to a
 * FILE* exactly as it does on stdout, and that FILE* is memory. Nothing about
 * the tool layer is duplicated, re-implemented or special-cased for HTTP — the
 * one difference is `remote`, which turns off the three things that only make
 * sense with a long-lived stdio peer, and `granted`, which is the scope check.
 *
 * A9's "no duplicated tool semantics" requirement is satisfied by construction:
 * there is one `TOOLS[]` and one `run` function per tool. */
static atlas_status mcp_exchange(atlas_gateway *g, const principal *pr, const char *body,
                                 size_t body_len, atlas_buf *out_json, atlas_err *err) {
    char *buf = NULL;
    size_t buf_len = 0;
    FILE *mem = open_memstream(&buf, &buf_len);
    if (mem == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "cannot open a response buffer");
    }

    atlas_mcp_server s;
    atlas_mcp_opts o;
    atlas_mcp_opts_init(&o);
    o.socket_path = atlas_buf_cstr(&g->socket);
    o.timeout_ms = g->timeout_ms;
    /* `remote` is set before init, because init consults it: a remote server
     * binds no session from the environment. */
    memset(&s, 0, sizeof(s));
    s.remote = true;
    atlas_mcp_server_init(&s, NULL, mem, g->errout, &o);
    s.remote = true;
    s.granted = pr != NULL ? pr->scopes : 0u;
    /* A stateless transport answers each POST on its own, so the handshake flag
     * is set rather than demanded. `dispatch` still refuses anything that is not
     * a valid JSON-RPC message. */
    s.got_initialize = true;
    s.initialized = true;

    atlas_status st = atlas_mcp_handle_document(&s, body, body_len, err);
    atlas_mcp_server_teardown(&s);
    (void)fflush(mem);
    (void)fclose(mem);

    if (st == ATLAS_OK) {
        atlas_buf_reset(out_json);
        if (buf != NULL && buf_len > 0) {
            st = atlas_buf_append(out_json, buf, buf_len, err);
        }
    }
    free(buf);
    return st;
}

/* --- routing --------------------------------------------------------------- */

atlas_status atlas_gateway_serve_bytes(atlas_gateway *g, const char *request, size_t len,
                                       atlas_buf *response, atlas_err *err) {
    int64_t started = now_ms();
    atlas_http_request req;
    atlas_http_request_init(&req);

    atlas_err perr;
    atlas_err_init(&perr);
    atlas_status pst = atlas_http_parse_head(request, len, g->policy.max_request_bytes, &req, &perr);
    if (pst != ATLAS_OK) {
        int status = pst == ATLAS_ERR_INTEGRITY ? 413 : 400;
        /* The refusal never reproduces what was sent. */
        atlas_status st = respond_error(g, NULL, status,
                                        status == 413 ? "request_too_large" : "bad_request",
                                        status == 413 ? "the request body exceeds the gateway limit"
                                                      : "the request could not be parsed",
                                        response, err);
        atlas_http_request_free(&req);
        return st;
    }

    if (!rate_ok(g)) {
        atlas_status st = respond_error(g, &req, 429, "rate_limited",
                                        "too many requests; slow down", response, err);
        audit(g, "REMOTE_MCP", NULL, "rate_limit", false, false, 0, now_ms() - started,
              "the gateway rate limit was reached");
        atlas_http_request_free(&req);
        return st;
    }

    /* A CORS preflight is answered without authenticating: it carries no
     * credential and asks only whether a real request would be permitted. It is
     * answered *only* for an origin the policy allows, so it reveals nothing
     * about what an unlisted origin could do. */
    if (strcmp(req.method, "OPTIONS") == 0) {
        atlas_status st;
        if (atlas_http_origin_allowed(&g->policy, req.origin)) {
            st = respond(g, &req, 204, "application/json", NULL, 0,
                         "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                         "Access-Control-Allow-Headers: Authorization, Content-Type\r\n"
                         "Access-Control-Max-Age: 600\r\n",
                         response, err);
        } else {
            st = respond_error(g, NULL, 403, "origin_not_allowed",
                               "this origin is not allowed by the gateway policy", response, err);
        }
        atlas_http_request_free(&req);
        return st;
    }

    /* An unauthenticated liveness probe. It says whether the process is up and
     * nothing else — no version, no policy, no repository, no counts — because a
     * probe that leaks configuration is a reconnaissance endpoint. */
    if (strcmp(req.path, "/healthz") == 0 && strcmp(req.method, "GET") == 0) {
        static const char OK_BODY[] = "{\"ok\":true}";
        atlas_status st = respond(g, &req, 200, "application/json", OK_BODY, sizeof OK_BODY - 1u,
                                  NULL, response, err);
        atlas_http_request_free(&req);
        return st;
    }

    if (strcmp(req.path, "/mcp") == 0) {
        if (!g->policy.remote_mcp) {
            atlas_status st = respond_error(g, &req, 404, "not_found",
                                            "this gateway does not serve remote MCP", response,
                                            err);
            atlas_http_request_free(&req);
            return st;
        }
        if (strcmp(req.method, "POST") != 0) {
            /* The Streamable HTTP specification says a server that offers no
             * SSE stream must answer 405 to GET. Atlas offers none: every
             * response here is a single JSON document, which is a complete and
             * conformant implementation of the transport. */
            atlas_status st = respond_error(g, &req, 405, "method_not_allowed",
                                            "POST a single JSON-RPC message to this endpoint",
                                            response, err);
            atlas_http_request_free(&req);
            return st;
        }

        principal pr;
        authenticate(g, &req, &pr);
        if (!pr.authenticated) {
            /* One answer for every authentication failure. `WWW-Authenticate`
             * names the scheme, which is what a conforming client needs, and
             * says nothing about why. */
            static const char UNAUTH[] =
                "{\"ok\":false,\"error\":{\"code\":\"unauthenticated\","
                "\"message\":\"a valid Atlas API key is required\"}}";
            atlas_status st = respond(g, &req, 401, "application/json", UNAUTH,
                                      sizeof UNAUTH - 1u, "WWW-Authenticate: Bearer\r\n",
                                      response, err);
            audit(g, "REMOTE_MCP", NULL, "mcp", false, false, ATLAS_ERR_INTEGRITY,
                  now_ms() - started, "authentication failed");
            atlas_http_request_free(&req);
            return st;
        }

        const char *body = request + req.body_offset;
        size_t blen = len > req.body_offset ? len - req.body_offset : 0;
        if (req.has_content_length && (size_t)req.content_length < blen) {
            blen = (size_t)req.content_length;
        }

        atlas_buf doc = ATLAS_BUF_INIT;
        atlas_err merr;
        atlas_err_init(&merr);
        atlas_status mst = mcp_exchange(g, &pr, body, blen, &doc, &merr);
        atlas_status st;
        if (mst != ATLAS_OK) {
            st = respond_error(g, &req, 500, "internal", "the request could not be handled",
                               response, err);
            audit(g, "REMOTE_MCP", &pr, "mcp", true, false, (int)mst, now_ms() - started,
                  "the MCP exchange failed");
        } else {
            /* The MCP layer writes newline-delimited documents; over HTTP the
             * body is the document, so a single trailing newline is trimmed
             * rather than sent as part of the JSON. */
            size_t n = doc.len;
            while (n > 0 && (doc.data[n - 1] == '\n' || doc.data[n - 1] == '\r')) {
                n--;
            }
            st = respond(g, &req, 200, "application/json", doc.data, n, NULL, response, err);
            audit(g, "REMOTE_MCP", &pr, "mcp", true, true, 0, now_ms() - started, NULL);
        }
        atlas_buf_free(&doc);
        atlas_http_request_free(&req);
        return st;
    }

    /* Everything else. A request that matches no route never becomes a socket
     * message: there is no path by which a client's path chooses what Atlas is
     * asked. */
    atlas_status st = respond_error(g, &req, 404, "not_found", "no such endpoint", response, err);
    atlas_http_request_free(&req);
    return st;
}
