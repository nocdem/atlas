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

#include <pthread.h>

#include "atlas/atlas.h"
#include "atlas/decision.h"
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

    /* There is deliberately **no `atlas_safe_pool` here.**
     *
     * A pool is a ring of scratch buffers with a mutable cursor, so one shared
     * between connection threads is a data race and, worse, hands two threads
     * the same slot — a value encoded by one appearing in the other's log line.
     * Every other pool in Atlas is request-scoped (dispatch_state, the
     * renderers, the MCP server) and this one must be too: each call site
     * declares its own on the stack. Making it absent rather than locked is
     * what stops a later edit from reintroducing the sharing.
     *
     * A fixed-window counter. Crude on purpose: a leaky bucket per peer would
     * need per-peer state, and behind a reverse proxy there is only one peer
     * unless `trust_forwarded_for` is set — so the sophistication would buy
     * nothing while looking as though it did. What this bounds is the total
     * request rate the gateway will forward, which is the thing that protects
     * the daemon. Stated as such in `docs/remote-access.md`. */
    int64_t window_started_ms;
    long long window_count;
    /* The counters above are read-modify-written by every connection thread, so
     * they need this. Without it the limit is not merely approximate — it is a
     * data race on a 64-bit counter, and the bound it appears to enforce is not
     * enforced at all. */
    pthread_mutex_t rate_lock;
};

/* --- diagnostics ----------------------------------------------------------- */

static void gw_log(atlas_gateway *g, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Serialises diagnostics.
 *
 * `stderr` is line-buffered at best and several connection threads writing at
 * once interleave within a line, which turns a log an operator reads under
 * pressure into one they cannot. */
static pthread_mutex_t gw_log_lock = PTHREAD_MUTEX_INITIALIZER;

static void gw_log(atlas_gateway *g, const char *fmt, ...) {
    if (g == NULL || g->errout == NULL) {
        return;
    }
    (void)pthread_mutex_lock(&gw_log_lock);
    char ts[ATLAS_TS_MAX];
    atlas_now_iso8601(ts, sizeof ts);
    (void)fprintf(g->errout, "%s atlas-gateway ", ts);
    va_list ap;
    va_start(ap, fmt);
    (void)vfprintf(g->errout, fmt, ap);
    va_end(ap);
    (void)fputc('\n', g->errout);
    (void)fflush(g->errout);
    (void)pthread_mutex_unlock(&gw_log_lock);
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
    (void)pthread_mutex_init(&g->rate_lock, NULL);
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
    (void)pthread_mutex_destroy(&g->rate_lock);
    atlas_buf_free(&g->socket);
    free(g);
}

/* --- responses ------------------------------------------------------------- */

/* Builds one complete response. Every route ends here, so no route can invent a
 * header set or forget the security headers. */
static atlas_status respond_csp(atlas_gateway *g, const atlas_http_request *req, int status,
                                const char *content_type, const void *body, size_t body_len,
                                const char *extra, const char *csp, atlas_buf *out,
                                atlas_err *err) {
    atlas_http_response r;
    atlas_http_response_init(&r);
    r.status = status;
    r.content_type = content_type;
    r.body = body;
    r.body_len = body_len;
    r.extra = extra;
    /* One CSP header, never two. A second one added through `extra` would be
     * enforced alongside this one and a browser applies the intersection, so a
     * page needing `connect-src 'self'` beside a default of `default-src 'none'`
     * would end up with no connect permission at all. */
    r.csp = csp;
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

static atlas_status respond(atlas_gateway *g, const atlas_http_request *req, int status,
                            const char *content_type, const void *body, size_t body_len,
                            const char *extra, atlas_buf *out, atlas_err *err) {
    return respond_csp(g, req, status, content_type, body, body_len, extra, NULL, out, err);
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
    /* The selector the client presented, whether or not it authenticated.
     *
     * Not a secret — see `atlas/apikey.h`: it exists so a token can be looked up
     * by an indexed test, and it is half of what the client sent in the clear.
     * Recorded so a DENIED audit row can say *which* credential was tried, which
     * is what an operator reads the trail for: "this key was rejected four
     * hundred times" is actionable and "something was rejected" is not.
     *
     * It is deliberately kept out of `key_id`, which means "the principal Atlas
     * authenticated" and must never hold a value somebody merely claimed. */
    char presented[ATLAS_APIKEY_SELECTOR_HEX + 1];
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

    /* The selector, for the audit trail only.
     *
     * The gateway does not verify anything with it — it cannot, because the
     * verifier lives in the index it may not read. It records *what was
     * claimed*, so a DENIED row can name the credential somebody tried rather
     * than saying only that something was rejected. The secret half is decoded
     * as a side effect of parsing and is wiped immediately; it is forwarded to
     * the daemon in `token`, not from here. */
    {
        char selector[ATLAS_APIKEY_SELECTOR_HEX + 1];
        unsigned char secret[ATLAS_APIKEY_SECRET_BYTES];
        atlas_err serr;
        atlas_err_init(&serr);
        if (atlas_apikey_token_parse(token, selector, secret, &serr) == ATLAS_OK) {
            (void)snprintf(out->presented, sizeof out->presented, "%s", selector);
        }
        volatile unsigned char *z = secret;
        for (size_t i = 0; i < sizeof secret; i++) {
            z[i] = 0;
        }
        memset(selector, 0, sizeof selector);
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
        atlas_safe_pool safe;
        atlas_safe_pool_init(&safe);
        gw_log(g, "the daemon did not answer gateway.auth: %s",
               atlas_safe(&safe, atlas_err_msg(&cerr)));
        atlas_safe_pool_free(&safe);
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
            atlas_safe_pool safe;
            atlas_safe_pool_init(&safe);
            gw_log(g, "the daemon refused gateway.auth: %s",
                   atlas_safe(&safe, atlas_ipc_response_message(r)));
            atlas_safe_pool_free(&safe);
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

/* Fixed Atlas text naming the selector that was presented, when one was.
 *
 * Never the secret half, never the header, and never a value that is not 16
 * lowercase hex characters — the parser refused anything else before this could
 * be reached. */
static const char *auth_detail(const principal *pr, char *buf, size_t n) {
    if (pr == NULL || pr->presented[0] == '\0') {
        return "authentication failed; no usable credential was presented";
    }
    (void)snprintf(buf, n, "authentication failed for the credential claiming id %s",
                   pr->presented);
    return buf;
}

/* --- the anonymous principal -------------------------------------------------
 *
 * This is not part of A9 as it shipped. An operator asked for it, and was told
 * the cost in the same conversation: anyone who can reach the listener reads
 * every scope named here with no credential at all, and on a cleartext LAN
 * listener that means anyone on the network segment. They reaffirmed the
 * decision on 2026-09-04. See `docs/remote-access.md` and `SECURITY.md` for the
 * full statement; this is the mechanism.
 *
 * `web_gui_anonymous_scopes` (`atlas/gwpolicy.h`) is the only source of the
 * scopes granted here — never a default this code chooses, and never `audit:read`
 * in particular unless the operator wrote it down.
 */

/* A fixed, unmistakable audit identity for the policy-granted anonymous
 * principal. Neither value is something somebody merely claimed — see
 * `principal.key_id`'s own contract above — which is exactly why both must be
 * fixed Atlas text rather than anything derived from the request.
 *
 * `key_id` can never collide with a real one: a real `key_id` is always
 * exactly `ATLAS_APIKEY_SELECTOR_HEX` lowercase hex characters, and this
 * string is a different length and contains characters ('n', 'o', 'u', 's')
 * that are not hex digits.
 *
 * `label` can never collide with a real one either, and the leading space is
 * the reason, not decoration: `atlas_apikey_label_valid` refuses a label that
 * begins or ends with one, so no real key can ever be labelled this. A `%` was
 * the first draft and was wrong — it is also excluded from a real label, but
 * `gateway.audit`'s intake runs every label through `atlas_safe()` before
 * storing it (`take_audit_text`, `src/ipc/server_gw.c`), which encodes `%`
 * reversibly and would have made every anonymous row's `label` column read
 * "%25anonymous%25" instead of naming what happened. A leading space needs no
 * such encoding and reaches the audit trail exactly as written here — which is
 * what "say plainly" requires or the mechanism is invisible in its
 * own log. */
#define GW_ANON_KEY_ID "anonymous"
#define GW_ANON_LABEL " (anonymous)"

/* Case-insensitive ASCII comparison, for a `Host` value: RFC 3986 makes host
 * comparison case-insensitive, and a policy's `listen_addr` is written by an
 * operator who may not have matched a browser's canonicalisation. Manual
 * rather than `strcasecmp` because nothing else in this codebase reaches for
 * a libc case-fold — `name_is` in `src/gw/http.c` does the same fold by hand
 * for header names, for the same reason. */
static bool host_eq_ci(const char *a, const char *b) {
    size_t i = 0;
    for (;; i++) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
        if (ca == '\0') {
            return true;
        }
    }
}

/* True when `host` — the request's `Host` header value, already bounded and
 * validated as printable ASCII by `atlas_http_parse_head` — names exactly the
 * address and port this gateway is bound to, compared whole and never by
 * prefix or suffix: the rule `atlas_http_origin_allowed` already follows for
 * an Origin, for the same reason a suffix match on a hostname is how
 * `atlas.example.com.attacker.net` gets treated as `atlas.example.com`.
 *
 * **This is not an authorisation check, and must never be read as one.** A7's
 * rule is that what the gateway cannot do is true because of who it runs as,
 * never because of a check in `src/gw` — and the operator who set
 * `web_gui_anonymous_scopes` deliberately removed the authorisation boundary
 * for the scopes it names. What this restores is a *narrower* equivalence
 * than "authenticated": that "can reach this listener" and "can read this
 * data" mean the same set of requests for browser-mediated access, which is
 * the sentence the operator actually authorised. Without it, a hostile page
 * served from any name whose DNS is briefly rebound to this gateway's address
 * is same-origin with itself in the browser's eyes — it gets no `Origin`
 * header applied to it, no CORS check, and no session cookie (none was ever
 * set for the attacker's name) — so it presents nothing, which is exactly
 * what an anonymous floor with no Host check would have accepted. A `Host`
 * mismatch is refused for the *floor* only: a request carrying a real
 * credential is judged on that credential exactly as before, on any Host.
 *
 * A client may omit the port when it equals the scheme's default. Atlas
 * terminates no TLS (`atlas/gwpolicy.h`), so a client reaching this listener
 * directly is always plain HTTP, and the only default that could apply is
 * port 80 — honoured only when the policy is in fact bound to it. On a
 * typical deployment (e.g. port 8799) that clause never fires and a browser
 * always sends the port. */
static bool host_matches_listener(const atlas_gateway *g, const char *host) {
    if (host == NULL || host[0] == '\0') {
        /* No Host at all — an HTTP/1.0 client, or one Atlas' own parser
         * refused to store because the header did not fit. Either way there
         * is nothing to compare, and the floor is refused rather than
         * guessed: an absent input must never read as a match. */
        return false;
    }
    char want[ATLAS_HTTP_HOST_MAX];
    (void)snprintf(want, sizeof want, "%s:%d", g->policy.listen_addr, g->policy.listen_port);
    if (host_eq_ci(host, want)) {
        return true;
    }
    if (g->policy.listen_port == 80 && host_eq_ci(host, g->policy.listen_addr)) {
        return true;
    }
    return false;
}

/* No policy key names additional accepted hostnames — e.g. for a reverse
 * proxy or a DNS name in front of this gateway — and that is a deliberate
 * omission, not an oversight. The deployment this mechanism was built for
 * reaches the gateway by its raw address today, which `listen_addr` and
 * `listen_port` already state and already suffice for. Adding a second,
 * broader-matching source of truth during a security fix round is exactly
 * the kind of surface a hasty remedy leaves behind for someone else to
 * misconfigure; an operator who later stands up a reverse proxy or a named
 * deployment can add such a key then, following the same discipline this
 * file already uses elsewhere — root-owned, absent by default, and every
 * value compared whole, never by prefix or suffix. Its absence here is not a
 * silent limitation: a `Host` that fails to match anything is a plain,
 * ordinary refusal of the anonymous floor (401, exactly as a wrong Host
 * always produces), never a crash, a bypass, or a confusing error — a
 * request with a real session or bearer credential is entirely unaffected,
 * on any Host, because this check exists only inside `anonymous_ok`. */

/* True when this request may be resolved to the policy's anonymous principal:
 * the web GUI is enabled, the policy names at least one such scope, the
 * request's `Host` names this listener (see `host_matches_listener`), and
 * `session_get` already failed to find a *live* session for whatever cookie
 * (if any) accompanied this request.
 *
 * The two credential kinds are treated differently on purpose, and the
 * difference is the audit trail, not the wire format. **Any** presented
 * `Authorization` header disqualifies the floor outright
 * (`req->authorization[0] != '\0'`) — the common and motivating case is a
 * bearer token, where `authenticate` extracts a selector into `presented`
 * before rejecting it so a DENIED row can say *which* credential was tried,
 * and falling through to the anonymous floor would spend that signal on a
 * request that already failed once; a header that is not even shaped like a
 * bearer token (`Authorization: Basic ...`) is refused the same way, on the
 * same principle, even though it carries no selector to lose — a presented
 * credential of any kind is a caller who tried and was refused, not a caller
 * who presented nothing. A **session cookie** that does not resolve —
 * expired, forged, or simply stale because a gateway restart forgot every
 * in-memory session (`gateway.c:573-577`, deliberately) — carries no selector
 * `session_get` could log; it authenticates to nothing and there is no
 * rejected credential's identity to lose by treating it as no cookie at all.
 * That is also the case this key exists to help: an operator's browser
 * holding a cookie from before the daemon's last restart must land on the
 * anonymous floor, not on a hard 401 that only a manual logout clears.
 *
 * A *live* session is never affected by any of this, `Host` included: it is
 * never checked against the session, because it cannot have been forged into
 * existing — a session cookie is only ever set by this gateway's own
 * `/auth/login` response, scoped by the browser to the origin that received
 * it, so a live match in `session_get` already proves the request reached the
 * right listener. */
static bool anonymous_ok(const atlas_gateway *g, const atlas_http_request *req) {
    return g->policy.web_gui && g->policy.web_gui_anonymous_scopes != 0u &&
           req->authorization[0] == '\0' && host_matches_listener(g, req->host);
}

/* Fills `out` with the anonymous principal: authenticated, carrying exactly
 * the scopes the policy named and no others. */
static void anonymous_principal(const atlas_gateway *g, principal *out) {
    memset(out, 0, sizeof(*out));
    out->authenticated = true;
    out->scopes = g->policy.web_gui_anonymous_scopes;
    (void)snprintf(out->key_id, sizeof out->key_id, "%s", GW_ANON_KEY_ID);
    (void)snprintf(out->label, sizeof out->label, "%s", GW_ANON_LABEL);
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
    (void)pthread_mutex_lock(&g->rate_lock);
    if (g->window_started_ms == 0 || t - g->window_started_ms >= 60000) {
        g->window_started_ms = t;
        g->window_count = 0;
    }
    g->window_count++;
    bool ok = g->window_count <= g->policy.rate_limit_per_minute;
    (void)pthread_mutex_unlock(&g->rate_lock);
    return ok;
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



/* --- browser sessions ------------------------------------------------------
 *
 * A9.9 says not to hand a long-lived API key to browser JavaScript. So the key
 * is posted once to `/auth/login`, exchanged for an opaque session, and the
 * browser keeps only an `HttpOnly` cookie that script cannot read. The page
 * clears the input immediately and stores the key nowhere.
 *
 * **Sessions live in gateway memory and a restart forgets them**, deliberately —
 * the reason A8-CI's operations table is in memory. A durable session would need
 * a durable secret and a table to hold it, and re-authenticating after a gateway
 * restart is the correct experience for an operator tool. There is no
 * `gw_sessions` migration and there must not be one.
 *
 * A session maps to exactly the same server-side principal a bearer token does:
 * one key id, one label, one scope mask, one audit identity. The two
 * authentication mechanisms differ; the authorization engine does not.
 *
 * The token is 32 bytes of kernel randomness. CSRF is handled by `SameSite=Strict`
 * plus the origin check, and by every API route being a GET — the only POSTs are
 * login and logout, and a forged login is an attacker authenticating as
 * themselves. */

typedef struct gw_session {
    char token[ATLAS_GW_SESSION_TOKEN_HEX + 1u];
    char key_id[ATLAS_APIKEY_SELECTOR_HEX + 1];
    char label[ATLAS_APIKEY_LABEL_MAX + 1];
    atlas_scope_mask scopes;
    int64_t expires_ms;
} gw_session;

static pthread_mutex_t gw_sess_lock = PTHREAD_MUTEX_INITIALIZER;
static gw_session gw_sessions[ATLAS_GW_MAX_SESSIONS];

static void session_put(atlas_gateway *g, const principal *pr, char *token_out) {
    unsigned char raw[ATLAS_GW_SESSION_TOKEN_BYTES];
    atlas_err err;
    atlas_err_init(&err);
    token_out[0] = '\0';
    if (atlas_random_bytes(raw, sizeof raw, &err) != ATLAS_OK) {
        /* No randomness, no session. Atlas does not issue a credential — of any
         * kind — that it could not make unpredictable. */
        return;
    }
    char hex[ATLAS_GW_SESSION_TOKEN_HEX + 1u];
    atlas_hex_encode(raw, sizeof raw, hex);
    memset(raw, 0, sizeof raw);

    int64_t now = now_ms();
    (void)pthread_mutex_lock(&gw_sess_lock);
    size_t slot = 0;
    int64_t oldest = 0;
    bool found = false;
    for (size_t i = 0; i < ATLAS_GW_MAX_SESSIONS; i++) {
        if (gw_sessions[i].token[0] == '\0' || gw_sessions[i].expires_ms <= now) {
            slot = i;
            found = true;
            break;
        }
        if (!found && (oldest == 0 || gw_sessions[i].expires_ms < oldest)) {
            oldest = gw_sessions[i].expires_ms;
            slot = i;
        }
    }
    memset(&gw_sessions[slot], 0, sizeof gw_sessions[slot]);
    (void)snprintf(gw_sessions[slot].token, sizeof gw_sessions[slot].token, "%s", hex);
    (void)snprintf(gw_sessions[slot].key_id, sizeof gw_sessions[slot].key_id, "%s", pr->key_id);
    (void)snprintf(gw_sessions[slot].label, sizeof gw_sessions[slot].label, "%s", pr->label);
    gw_sessions[slot].scopes = pr->scopes;
    gw_sessions[slot].expires_ms = now + g->policy.session_ttl_seconds * 1000;
    (void)pthread_mutex_unlock(&gw_sess_lock);

    (void)snprintf(token_out, ATLAS_GW_SESSION_TOKEN_HEX + 1u, "%s", hex);
    memset(hex, 0, sizeof hex);
}

/* Resolves a cookie to a principal. An expired session is cleared rather than
 * merely refused, so a slot is not held by something nobody can use. */
static bool session_get(const char *token, principal *out) {
    memset(out, 0, sizeof(*out));
    if (token == NULL || token[0] == '\0') {
        return false;
    }
    size_t tlen = strlen(token);
    if (tlen != ATLAS_GW_SESSION_TOKEN_HEX) {
        return false;
    }
    bool ok = false;
    int64_t now = now_ms();
    (void)pthread_mutex_lock(&gw_sess_lock);
    for (size_t i = 0; i < ATLAS_GW_MAX_SESSIONS; i++) {
        if (gw_sessions[i].token[0] == '\0') {
            continue;
        }
        if (gw_sessions[i].expires_ms <= now) {
            memset(&gw_sessions[i], 0, sizeof gw_sessions[i]);
            continue;
        }
        /* Constant-time, for the reason the credential comparison is: this is a
         * secret an attacker supplies and can vary. */
        if (atlas_ct_equal(gw_sessions[i].token, token, ATLAS_GW_SESSION_TOKEN_HEX)) {
            (void)snprintf(out->key_id, sizeof out->key_id, "%s", gw_sessions[i].key_id);
            (void)snprintf(out->label, sizeof out->label, "%s", gw_sessions[i].label);
            out->scopes = gw_sessions[i].scopes;
            out->authenticated = true;
            ok = true;
        }
    }
    (void)pthread_mutex_unlock(&gw_sess_lock);
    return ok;
}

static void session_drop(const char *token) {
    if (token == NULL || token[0] == '\0') {
        return;
    }
    (void)pthread_mutex_lock(&gw_sess_lock);
    for (size_t i = 0; i < ATLAS_GW_MAX_SESSIONS; i++) {
        if (gw_sessions[i].token[0] != '\0' &&
            atlas_ct_equal(gw_sessions[i].token, token, ATLAS_GW_SESSION_TOKEN_HEX)) {
            memset(&gw_sessions[i], 0, sizeof gw_sessions[i]);
        }
    }
    (void)pthread_mutex_unlock(&gw_sess_lock);
}

extern const unsigned char atlas_ui_page[];
extern const size_t atlas_ui_page_len;

/* Reads the `key` member out of a login body.
 *
 * Deliberately not a JSON parse: the body is one member whose value is a
 * credential, and running an untrusted document through a parser to reach it
 * would be more machinery than the job needs. The scan is bounded, refuses
 * anything that is not the documented shape, and never copies more than a token
 * can be. */
static bool take_login_key(const char *body, size_t len, char *out, size_t out_size) {
    out[0] = '\0';
    static const char NEEDLE[] = "\"key\"";
    if (body == NULL || len == 0 || len > 4096u) {
        return false;
    }
    size_t nlen = sizeof NEEDLE - 1u;
    size_t i = 0;
    for (; i + nlen <= len; i++) {
        if (memcmp(body + i, NEEDLE, nlen) == 0) {
            break;
        }
    }
    if (i + nlen > len) {
        return false;
    }
    i += nlen;
    while (i < len && (body[i] == ' ' || body[i] == ':' || body[i] == '\t')) {
        i++;
    }
    if (i >= len || body[i] != '"') {
        return false;
    }
    i++;
    size_t start = i;
    while (i < len && body[i] != '"') {
        /* No escapes: a token is base64url and hex, so a backslash in one is a
         * token that is not ours. */
        if (body[i] == '\\') {
            return false;
        }
        i++;
    }
    if (i >= len) {
        return false;
    }
    size_t vlen = i - start;
    if (vlen == 0 || vlen + 1u > out_size) {
        return false;
    }
    memcpy(out, body + start, vlen);
    out[vlen] = '\0';
    return true;
}

/* --- the web API -----------------------------------------------------------
 *
 * A fixed table of routes. Each names the daemon method it forwards to, the
 * scope it needs, and the query parameters it will pass on — and nothing else
 * from the request reaches the socket. A client cannot name an Atlas method, add
 * a parameter, or reach a repository it did not name through a parameter the
 * route declares.
 *
 * The response is the daemon's own document, returned unchanged. Every value in
 * it was already safe-encoded by the daemon, so re-serialising here would either
 * double-encode it — the A8.2 defect — or require a second renderer that could
 * drift from the first. What the browser receives is what Atlas said.
 *
 * Paths are exact literals and parameters live in the query string, so no route
 * has a path component to parse and nothing is ever joined to a filesystem
 * path.
 */

/* Room for the parameters *and* the NULL terminator every row relies on. A9.1's
 * `kind` filter took `/api/v1/decisions` to five parameters and exactly filled a
 * six-element array, which still worked and left the next route with nowhere to
 * put its terminator — a bound that is exactly full is a bound about to be
 * exceeded silently. */
#define GW_API_MAX_PARAMS 8

typedef struct api_route {
    const char *path;
    const char *method;     /* the daemon method it forwards to */
    atlas_apikey_scope scope;
    /* Query parameters this route forwards, by name. Anything else in the query
     * string is ignored — not an error, because a browser adds cache-busting
     * parameters, but never forwarded. */
    const char *params[GW_API_MAX_PARAMS];
    /* Which of those are integers rather than strings. Typed at the edge so a
     * value that is not a number is refused rather than reaching the daemon as
     * a string it will reject less clearly. */
    const char *ints[GW_API_MAX_PARAMS];
} api_route;

/* Every route is a read. There is no write anywhere in this table, and adding
 * one would need a write scope no A9 credential can hold. */
static const api_route API_ROUTES[] = {
    {"/api/v1/status", "daemon.status", ATLAS_SCOPE_REPO_READ, {NULL}, {NULL}},
    {"/api/v1/repos", "repo.list", ATLAS_SCOPE_REPO_READ, {NULL}, {NULL}},
    {"/api/v1/repo", "repo.state", ATLAS_SCOPE_REPO_READ, {"repo", NULL}, {NULL}},
    {"/api/v1/events", "events.since", ATLAS_SCOPE_REPO_READ,
     {"repo", "since", "limit", NULL}, {"since", "limit", NULL}},
    {"/api/v1/search", "repo.search", ATLAS_SCOPE_REPO_READ,
     {"repo", "query", "limit", "cursor", NULL}, {"limit", "cursor", NULL}},
    {"/api/v1/file", "repo.file", ATLAS_SCOPE_REPO_READ, {"repo", "path", NULL}, {NULL}},
    {"/api/v1/history", "repo.history", ATLAS_SCOPE_REPO_READ,
     {"repo", "path", "limit", NULL}, {"limit", NULL}},
    /* A9.1 adds `kind` beside `status`: two independent filters, both forwarded,
     * both validated by the daemon against their vocabularies. Adding a parameter
     * to the row is the only way a query string reaches a daemon call, which is
     * what keeps the surface a fixed table rather than a proxy. */
    {"/api/v1/decisions", "decision.list", ATLAS_SCOPE_DECISIONS_READ,
     {"repo", "status", "kind", "limit", "cursor", NULL}, {"limit", "cursor", NULL}},
    {"/api/v1/decision", "decision.get", ATLAS_SCOPE_DECISIONS_READ,
     {"repo", "decision", "revision", NULL}, {"revision", NULL}},
    {"/api/v1/decision/history", "decision.history", ATLAS_SCOPE_DECISIONS_READ,
     {"repo", "decision", NULL}, {NULL}},
    {"/api/v1/gate", "gate.check", ATLAS_SCOPE_DECISIONS_READ,
     {"repo", "decision", NULL}, {NULL}},
    {"/api/v1/code/status", "code.status", ATLAS_SCOPE_GRAPH_READ, {"repo", NULL}, {NULL}},
    {"/api/v1/code/file", "code.file", ATLAS_SCOPE_GRAPH_READ, {"repo", "path", NULL}, {NULL}},
    {"/api/v1/code/symbol", "code.symbol", ATLAS_SCOPE_GRAPH_READ,
     {"repo", "symbol", NULL}, {NULL}},
    {"/api/v1/code/search", "code.search", ATLAS_SCOPE_GRAPH_READ,
     {"repo", "query", "limit", NULL}, {"limit", NULL}},
    {"/api/v1/sem/status", "sem.status", ATLAS_SCOPE_GRAPH_READ, {"repo", NULL}, {NULL}},
    {"/api/v1/sem/symbol", "sem.symbol", ATLAS_SCOPE_GRAPH_READ,
     {"repo", "symbol", "subject", NULL}, {NULL}},
    {"/api/v1/sem/callers", "sem.callers", ATLAS_SCOPE_GRAPH_READ,
     {"repo", "symbol", "depth", NULL}, {"depth", NULL}},
    {"/api/v1/sem/callees", "sem.callees", ATLAS_SCOPE_GRAPH_READ,
     {"repo", "symbol", "depth", NULL}, {"depth", NULL}},
    {"/api/v1/impact", "sem.impact", ATLAS_SCOPE_IMPACT_READ,
     {"repo", "subject", "depth", NULL}, {"depth", NULL}},
    {"/api/v1/code/impact", "code.impact", ATLAS_SCOPE_IMPACT_READ,
     {"repo", "path", "symbol", "depth", NULL}, {"depth", NULL}},
    {"/api/v1/context", "sem.context", ATLAS_SCOPE_CONTEXT_READ,
     {"repo", "task", "max_tokens", NULL}, {"max_tokens", NULL}},
    /* A9.2.1. The verification workflow, read-only and no more.
     *
     * Three routes, all reads, and the absence of a fourth is the point: there
     * is no remote route that creates a claim, cites evidence, attests, or asks
     * for an evaluation. A9's rule says a mutating route "needs a write scope no
     * A9 credential can hold, which is the argument it has to survive" — and
     * intake has not survived it. `verify.evaluate` can cause Atlas to move a
     * lifecycle state, and putting that behind an Internet-facing credential
     * would mean a leaked bearer token could drive the one path in Atlas that
     * transitions a record without a person. A local model reaches intake
     * through MCP over a Unix socket, where the peer uid is a kernel fact; that
     * is a different trust position and it is the one intake requires.
     *
     * DECISIONS_READ rather than a scope of its own: a claim is evidence about
     * a knowledge record, and a credential trusted to read the records is
     * trusted to read what bears on them. */
    {"/api/v1/verify/claims", "verify.claims", ATLAS_SCOPE_DECISIONS_READ,
     {"repo", "decision", "limit", NULL}, {"limit", NULL}},
    {"/api/v1/verify/claim", "verify.show", ATLAS_SCOPE_DECISIONS_READ,
     {"claim", "claim_id", NULL}, {"claim_id", NULL}},
    {"/api/v1/verify/policy", "verify.policy", ATLAS_SCOPE_DECISIONS_READ, {NULL}, {NULL}},
    {"/api/v1/audit", "gateway.audit_list", ATLAS_SCOPE_AUDIT_READ,
     {"limit", "cursor", "key_id", NULL}, {"limit", "cursor", NULL}},
};

static bool route_wants(const api_route *r, const char *name, bool *is_int) {
    *is_int = false;
    bool found = false;
    for (size_t i = 0; i < GW_API_MAX_PARAMS && r->params[i] != NULL; i++) {
        if (strcmp(r->params[i], name) == 0) {
            found = true;
            break;
        }
    }
    if (!found) {
        return false;
    }
    for (size_t i = 0; i < GW_API_MAX_PARAMS && r->ints[i] != NULL; i++) {
        if (strcmp(r->ints[i], name) == 0) {
            *is_int = true;
            break;
        }
    }
    return true;
}

/* Percent-decodes one query value in place.
 *
 * This is the *only* place the gateway decodes anything, and it decodes a value
 * that is about to be passed as a typed JSON string — never a path, never a
 * method name, never anything that selects behaviour. A malformed escape is a
 * refusal rather than a guess, and a decoded NUL is refused outright: a value
 * that ends early is a different value from the one that was sent. */
static bool percent_decode(const char *in, size_t len, char *out, size_t out_size) {
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        char c = in[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%') {
            if (i + 2 >= len) {
                return false;
            }
            int hi = -1, lo = -1;
            for (int k = 0; k < 2; k++) {
                char h = in[i + 1 + (size_t)k];
                int v = -1;
                if (h >= '0' && h <= '9') {
                    v = h - '0';
                } else if (h >= 'a' && h <= 'f') {
                    v = h - 'a' + 10;
                } else if (h >= 'A' && h <= 'F') {
                    v = h - 'A' + 10;
                } else {
                    return false;
                }
                if (k == 0) {
                    hi = v;
                } else {
                    lo = v;
                }
            }
            c = (char)((hi << 4) | lo);
            i += 2;
        }
        if (c == '\0') {
            return false;
        }
        if (o + 1 >= out_size) {
            return false;
        }
        out[o++] = c;
    }
    out[o] = '\0';
    return true;
}

/* Builds the daemon params for one route from the request's query string.
 *
 * Only names the route declares are read. Everything else in the query string
 * is ignored rather than forwarded, so a client cannot add a parameter to a
 * daemon call by adding one to a URL. */
static atlas_status build_api_params(const api_route *r, const char *query, atlas_buf *out,
                                     atlas_err *err) {
    atlas_ipc_params *p = NULL;
    atlas_json *j = NULL;
    atlas_status st = atlas_ipc_params_begin(&p, &j, err);
    if (st != ATLAS_OK) {
        return st;
    }
    size_t i = 0;
    size_t qlen = query != NULL ? strlen(query) : 0;
    while (st == ATLAS_OK && i < qlen) {
        size_t start = i;
        while (i < qlen && query[i] != '&') {
            i++;
        }
        size_t end = i;
        if (i < qlen) {
            i++;
        }
        const char *eq = memchr(query + start, '=', end - start);
        if (eq == NULL) {
            continue;
        }
        size_t nlen = (size_t)(eq - (query + start));
        char name[32];
        if (nlen == 0 || nlen >= sizeof name) {
            continue;
        }
        memcpy(name, query + start, nlen);
        name[nlen] = '\0';
        bool is_int = false;
        if (!route_wants(r, name, &is_int)) {
            continue;
        }
        char value[1024];
        if (!percent_decode(eq + 1, (size_t)(query + end - (eq + 1)), value, sizeof value)) {
            atlas_ipc_params_abort(p);
            atlas_buf_free(out);
            return atlas_err_set(err, ATLAS_ERR_USAGE, "a query parameter is malformed");
        }
        if (is_int) {
            int64_t n = 0;
            if (value[0] == '\0') {
                continue;
            }
            for (const char *c = value; *c != '\0'; c++) {
                if (*c < '0' || *c > '9') {
                    atlas_ipc_params_abort(p);
                    return atlas_err_set(err, ATLAS_ERR_USAGE,
                                         "a numeric query parameter is not a number");
                }
                n = n * 10 + (*c - '0');
                if (n > 1000000) {
                    atlas_ipc_params_abort(p);
                    return atlas_err_set(err, ATLAS_ERR_USAGE,
                                         "a numeric query parameter is out of range");
                }
            }
            st = atlas_json_key_int(j, name, n, err);
        } else {
            st = atlas_json_key_str(j, name, value, err);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_ipc_params_finish(p, out, err);
    } else {
        atlas_ipc_params_abort(p);
    }
    return st;
}

/* Handles one API request. Returns false when the path names no route. */
static bool api_handle(atlas_gateway *g, const atlas_http_request *req, const principal *pr,
                       int64_t started, atlas_buf *response, atlas_status *st_out,
                       atlas_err *err) {
    const api_route *route = NULL;
    for (size_t i = 0; i < sizeof API_ROUTES / sizeof API_ROUTES[0]; i++) {
        if (strcmp(API_ROUTES[i].path, req->path) == 0) {
            route = &API_ROUTES[i];
            break;
        }
    }
    if (route == NULL) {
        return false;
    }
    if (strcmp(req->method, "GET") != 0) {
        /* Every route is a read. A POST to one is not a different operation, it
         * is a request for something that does not exist. */
        *st_out = respond_error(g, req, 405, "method_not_allowed", "this endpoint reads only",
                                response, err);
        return true;
    }
    if (!pr->authenticated) {
        static const char UNAUTH[] =
            "{\"ok\":false,\"error\":{\"code\":\"unauthenticated\","
            "\"message\":\"a valid Atlas API key is required\"}}";
        *st_out = respond(g, req, 401, "application/json", UNAUTH, sizeof UNAUTH - 1u,
                          "WWW-Authenticate: Bearer\r\n", response, err);
        {
            char detail[128];
            audit(g, "WEB_API", NULL, req->path, false, false, ATLAS_ERR_INTEGRITY,
                  now_ms() - started, auth_detail(pr, detail, sizeof detail));
        }
        return true;
    }
    if (!atlas_scope_has(pr->scopes, route->scope)) {
        const char *needed = atlas_apikey_scope_name(route->scope);
        atlas_buf msg = ATLAS_BUF_INIT;
        atlas_err merr;
        atlas_err_init(&merr);
        (void)atlas_buf_appendf(&msg, &merr, "this credential does not hold the \"%s\" scope",
                                needed != NULL ? needed : "required");
        *st_out = respond_error(g, req, 403, "forbidden", atlas_buf_cstr(&msg), response, err);
        atlas_buf_free(&msg);
        audit(g, "WEB_API", pr, req->path, false, false, ATLAS_ERR_INTEGRITY, now_ms() - started,
              "the credential lacks the required scope");
        return true;
    }

    atlas_buf params = ATLAS_BUF_INIT;
    atlas_err perr;
    atlas_err_init(&perr);
    if (build_api_params(route, req->query, &params, &perr) != ATLAS_OK) {
        *st_out = respond_error(g, req, 400, "bad_request", "a query parameter is malformed",
                                response, err);
        atlas_buf_free(&params);
        audit(g, "WEB_API", pr, req->path, true, false, ATLAS_ERR_USAGE, now_ms() - started,
              "a query parameter was malformed");
        return true;
    }

    atlas_buf resp = ATLAS_BUF_INIT;
    atlas_err cerr;
    atlas_err_init(&cerr);
    atlas_status cst = atlas_ipc_call_timeout(atlas_buf_cstr(&g->socket), route->method,
                                              atlas_buf_cstr(&params), g->timeout_ms, &resp, &cerr);
    atlas_buf_free(&params);

    if (cst != ATLAS_OK) {
        atlas_safe_pool safe;
        atlas_safe_pool_init(&safe);
        gw_log(g, "the daemon did not answer %s: %s", route->method,
               atlas_safe(&safe, atlas_err_msg(&cerr)));
        atlas_safe_pool_free(&safe);
        *st_out = respond_error(g, req, 502, "upstream", "the Atlas daemon did not answer",
                                response, err);
        audit(g, "WEB_API", pr, req->path, true, false, cst, now_ms() - started,
              "the daemon did not answer");
        atlas_buf_free(&resp);
        return true;
    }

    /* The daemon's own document, unchanged. Every value in it was safe-encoded
     * by the daemon; re-serialising would either double-encode — the A8.2
     * defect — or need a second renderer that could drift from the first. */
    atlas_ipc_response *r = NULL;
    atlas_err rerr;
    atlas_err_init(&rerr);
    int status = 200;
    if (atlas_ipc_response_parse(resp.data, resp.len, &r, &rerr) == ATLAS_OK &&
        !atlas_ipc_response_ok(r)) {
        /* Atlas' status vocabulary mapped onto HTTP, so a caller does not have
         * to read the body to know what happened. */
        switch (atlas_ipc_response_status(r)) {
        case ATLAS_ERR_USAGE: status = 400; break;
        case ATLAS_ERR_REPO: status = 404; break;
        case ATLAS_ERR_CONFIG: status = 503; break;
        case ATLAS_OK:
        case ATLAS_ERR_INTERNAL:
        case ATLAS_ERR_DB:
        case ATLAS_ERR_GIT:
        case ATLAS_ERR_INTEGRITY: status = 500; break;
        }
    }
    atlas_ipc_response_free(r);

    *st_out = respond(g, req, status, "application/json", resp.data, resp.len, NULL, response, err);
    audit(g, "WEB_API", pr, req->path, true, status == 200, status == 200 ? 0 : ATLAS_ERR_INTERNAL,
          now_ms() - started, NULL);
    atlas_buf_free(&resp);
    return true;
}

/* A15 T1. `atlas_gateway_api_routes()` copies the three fields
 * `atlas_gateway_route_view` exposes out of `API_ROUTES[]`, once, into a static
 * table of the same length. It reads `API_ROUTES[]` and nothing else, so a row
 * added or changed here is reflected without a second literal to keep in step
 * -- the count a hand-kept parallel table would need is exactly the defect
 * commit a169393 records. Nothing in the gateway calls it; it exists for a
 * test. */
const atlas_gateway_route_view *atlas_gateway_api_routes(size_t *count_out) {
    static const size_t n = sizeof API_ROUTES / sizeof API_ROUTES[0];
    static atlas_gateway_route_view views[sizeof API_ROUTES / sizeof API_ROUTES[0]];
    static bool populated = false;
    if (!populated) {
        for (size_t i = 0; i < n; i++) {
            views[i].path = API_ROUTES[i].path;
            views[i].method = API_ROUTES[i].method;
            views[i].scope = API_ROUTES[i].scope;
        }
        populated = true;
    }
    if (count_out != NULL) {
        *count_out = n;
    }
    return views;
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
        char detail[128];
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
                  now_ms() - started, auth_detail(&pr, detail, sizeof detail));
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


    /* --- the browser surface --------------------------------------------- */

    if (g->policy.web_gui) {
        if (strcmp(req.path, "/auth/login") == 0 && strcmp(req.method, "POST") == 0) {
            const char *body = request + req.body_offset;
            size_t blen = len > req.body_offset ? len - req.body_offset : 0;
            if (req.has_content_length && (size_t)req.content_length < blen) {
                blen = (size_t)req.content_length;
            }
            char key[ATLAS_APIKEY_TOKEN_MAX];
            principal pr;
            memset(&pr, 0, sizeof pr);
            if (take_login_key(body, blen, key, sizeof key)) {
                /* Reuse the one authentication path by presenting the key the
                 * way a bearer client would. There is no second verifier. */
                atlas_http_request as_bearer = req;
                (void)snprintf(as_bearer.authorization, sizeof as_bearer.authorization,
                               "Bearer %s", key);
                authenticate(g, &as_bearer, &pr);
                volatile unsigned char *z = (volatile unsigned char *)as_bearer.authorization;
                for (size_t i = 0; i < sizeof as_bearer.authorization; i++) {
                    z[i] = 0;
                }
            }
            {
                volatile unsigned char *z = (volatile unsigned char *)key;
                for (size_t i = 0; i < sizeof key; i++) {
                    z[i] = 0;
                }
            }
            atlas_status st;
            if (!pr.authenticated) {
                st = respond_error(g, &req, 401, "unauthenticated",
                                   "that key was not accepted", response, err);
                {
                    char detail[128];
                    audit(g, "WEB_GUI", NULL, "auth.login", false, false, ATLAS_ERR_INTEGRITY,
                          now_ms() - started, auth_detail(&pr, detail, sizeof detail));
                }
            } else {
                char token[ATLAS_GW_SESSION_TOKEN_HEX + 1u];
                session_put(g, &pr, token);
                if (token[0] == '\0') {
                    st = respond_error(g, &req, 500, "internal",
                                       "no session could be created", response, err);
                } else {
                    char cookie[256];
                    /* HttpOnly: script cannot read it. SameSite=Strict: it is
                     * not sent on a cross-site request, which is the CSRF
                     * defence. Secure is set only when TLS is declared in
                     * front, because a Secure cookie over plain HTTP is one the
                     * browser silently drops — and an operator debugging that
                     * has no way to see why. */
                    (void)snprintf(cookie, sizeof cookie,
                                   "Set-Cookie: atlas_session=%s; Path=/; HttpOnly; "
                                   "SameSite=Strict; Max-Age=%lld%s\r\n",
                                   token, g->policy.session_ttl_seconds,
                                   g->policy.tls_mode == ATLAS_GWPOLICY_TLS_REVERSE_PROXY
                                       ? "; Secure"
                                       : "");
                    static const char OKB[] = "{\"ok\":true}";
                    st = respond(g, &req, 200, "application/json", OKB, sizeof OKB - 1u, cookie,
                                 response, err);
                    audit(g, "WEB_GUI", &pr, "auth.login", true, true, 0, now_ms() - started,
                          NULL);
                    memset(cookie, 0, sizeof cookie);
                }
                memset(token, 0, sizeof token);
            }
            atlas_http_request_free(&req);
            return st;
        }

        if (strcmp(req.path, "/auth/logout") == 0 && strcmp(req.method, "POST") == 0) {
            session_drop(req.session);
            static const char OKB[] = "{\"ok\":true}";
            atlas_status st = respond(g, &req, 200, "application/json", OKB, sizeof OKB - 1u,
                                      "Set-Cookie: atlas_session=; Path=/; HttpOnly; "
                                      "SameSite=Strict; Max-Age=0\r\n",
                                      response, err);
            atlas_http_request_free(&req);
            return st;
        }

        if (strcmp(req.path, "/auth/me") == 0 && strcmp(req.method, "GET") == 0) {
            principal pr;
            atlas_status st;
            bool have_principal = session_get(req.session, &pr);
            bool anon = false;
            if (!have_principal && anonymous_ok(g, &req)) {
                anonymous_principal(g, &pr);
                have_principal = true;
                anon = true;
            }
            if (!have_principal) {
                st = respond_error(g, &req, 401, "unauthenticated", "no session", response, err);
            } else {
                /* The scope list, so the page can hide what this principal
                 * cannot read. Hiding is courtesy; every route checks for
                 * itself. `anonymous` says plainly which kind of principal
                 * this is: the page must not report a session that does not
                 * exist, and must not report nobody when the policy has in
                 * fact granted an anonymous floor. */
                atlas_buf scopes = ATLAS_BUF_INIT;
                atlas_err serr;
                atlas_err_init(&serr);
                (void)atlas_apikey_scopes_render(pr.scopes, &scopes, &serr);
                atlas_buf body = ATLAS_BUF_INIT;
                atlas_err berr;
                atlas_err_init(&berr);
                (void)atlas_buf_appendf(
                    &body, &berr,
                    "{\"ok\":true,\"anonymous\":%s,\"label\":\"%s\",\"scopes\":\"%s\"}",
                    anon ? "true" : "false", pr.label, atlas_buf_cstr(&scopes));
                st = respond(g, &req, 200, "application/json", body.data, body.len, NULL, response,
                             err);
                atlas_buf_free(&body);
                atlas_buf_free(&scopes);
            }
            atlas_http_request_free(&req);
            return st;
        }

        if ((strcmp(req.path, "/") == 0 || strcmp(req.path, "/index.html") == 0) &&
            strcmp(req.method, "GET") == 0) {
            /* One page, from the binary. The gateway has no filesystem read
             * path, so there is no directory an attacker could aim a request
             * at. The page's own CSP permits its inline style and script and
             * nothing else — no external origin, no eval, no frame. */
            atlas_status st = respond_csp(
                g, &req, 200, "text/html; charset=utf-8", atlas_ui_page, atlas_ui_page_len, NULL,
                /* The page's own policy, replacing the default rather than
                 * joining it: it needs its inline style and script and it needs
                 * to call its own origin, and nothing else. No external origin,
                 * no eval, no frame, no form target. */
                "default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; "
                "connect-src 'self'; form-action 'none'; frame-ancestors 'none'; base-uri 'none'",
                response, err);
            atlas_http_request_free(&req);
            return st;
        }
    }

    /* The web API. Authenticated the same way and audited the same way; the
     * route table is what stops a request naming a daemon method. */
    if (strncmp(req.path, "/api/", 5u) == 0) {
        principal pr;
        /* Either mechanism, one principal. A bearer token is the remote-MCP
         * shape; a session cookie is the browser's. They map to the same
         * key id, label, scope mask and audit identity — the authorization
         * engine does not know which was used. */
        if (!session_get(req.session, &pr)) {
            authenticate(g, &req, &pr);
            /* Neither mechanism produced a live principal. A root-owned
             * policy may name a third: a fixed, floor-only scope set. A
             * bearer token that was presented and failed stays failed —
             * `anonymous_ok` refuses that case, for the audit reason at its
             * own definition. An unresolving session cookie (absent,
             * expired, or forged) does not: see the same comment. */
            if (!pr.authenticated && anonymous_ok(g, &req)) {
                anonymous_principal(g, &pr);
            }
        }
        atlas_status ast = ATLAS_OK;
        bool handled = api_handle(g, &req, &pr, started, response, &ast, err);
        memset(&pr, 0, sizeof pr);
        if (handled) {
            atlas_http_request_free(&req);
            return ast;
        }
    }

    /* Everything else. A request that matches no route never becomes a socket
     * message: there is no path by which a client's path chooses what Atlas is
     * asked. */
    atlas_status st = respond_error(g, &req, 404, "not_found", "no such endpoint", response, err);
    atlas_http_request_free(&req);
    return st;
}

/* --- the listener ----------------------------------------------------------
 *
 * One thread per connection, capped by the policy. A connection beyond the
 * ceiling is answered 503 and closed rather than queued: A8-CI's rule about
 * deterministic refusal, because a queued connection that eventually times out
 * is a slow failure nobody can tell from a hang.
 *
 * Every read carries a deadline, so a peer that stops mid-request costs one
 * slot for a bounded time rather than forever — which is the whole of the
 * slow-loris defence, together with the ceiling.
 */

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* Binds the policy's address and port.
 *
 * `AI_NUMERICHOST` is not optional: it means the address is parsed and never
 * resolved. A gateway that performed a DNS lookup to decide what to bind would
 * take its listening address from whatever answered, which is not what an
 * operator wrote down. */
static atlas_status gw_listen(atlas_gateway *g, int *fd_out, atlas_err *err) {
    *fd_out = -1;
    char port[16];
    (void)snprintf(port, sizeof port, "%d", g->policy.listen_port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV | AI_PASSIVE;

    struct addrinfo *res = NULL;
    int rc = getaddrinfo(g->policy.listen_addr, port, &hints, &res);
    if (rc != 0 || res == NULL) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "the gateway cannot bind \"%s\": it is not a numeric address",
                             g->policy.listen_addr);
    }

    int fd = socket(res->ai_family, res->ai_socktype | SOCK_CLOEXEC, res->ai_protocol);
    if (fd < 0) {
        int e = errno;
        freeaddrinfo(res);
        return atlas_err_set_errno(err, ATLAS_ERR_CONFIG, e, "cannot create the gateway socket");
    }
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (res->ai_family == AF_INET6) {
        /* v6-only, so binding `::1` cannot silently also accept IPv4 traffic on
         * a dual-stack kernel. An operator who wrote a loopback address must not
         * get a wider listener than they asked for. */
        (void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof one);
    }
    if (bind(fd, res->ai_addr, res->ai_addrlen) != 0) {
        int e = errno;
        freeaddrinfo(res);
        (void)close(fd);
        return atlas_err_set_errno(err, ATLAS_ERR_CONFIG, e, "cannot bind %s port %d",
                                   g->policy.listen_addr, g->policy.listen_port);
    }
    freeaddrinfo(res);
    if (listen(fd, 64) != 0) {
        int e = errno;
        (void)close(fd);
        return atlas_err_set_errno(err, ATLAS_ERR_CONFIG, e, "cannot listen on port %d",
                                   g->policy.listen_port);
    }
    *fd_out = fd;
    return ATLAS_OK;
}

/* Reads with a deadline. Returns false on close, error or timeout. */
static bool read_deadline(int fd, void *buf, size_t cap, int timeout_ms, ssize_t *got) {
    struct pollfd p = {fd, POLLIN, 0};
    int rc = poll(&p, 1, timeout_ms);
    if (rc <= 0) {
        return false;
    }
    *got = recv(fd, buf, cap, 0);
    return *got > 0;
}

static bool write_all(int fd, const char *data, size_t len, int timeout_ms) {
    size_t sent = 0;
    while (sent < len) {
        struct pollfd p = {fd, POLLOUT, 0};
        if (poll(&p, 1, timeout_ms) <= 0) {
            return false;
        }
        ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

typedef struct conn_job {
    atlas_gateway *g;
    int fd;
} conn_job;

/* Live connection count, so the ceiling is enforced across threads. */
static pthread_mutex_t gw_conn_lock = PTHREAD_MUTEX_INITIALIZER;
static long long gw_conn_live = 0;

static void serve_one(atlas_gateway *g, int fd) {
    atlas_buf req = ATLAS_BUF_INIT;
    atlas_buf resp = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);

    /* Read the head. Bounded twice: by the header ceiling and by the deadline,
     * so neither a peer that sends a byte a second nor one that sends megabytes
     * of headers can hold this slot. */
    size_t head_end = 0;
    bool ok = true;
    for (;;) {
        head_end = atlas_http_head_complete(req.data != NULL ? req.data : "", req.len);
        if (head_end > 0) {
            break;
        }
        if (req.len >= ATLAS_GW_MAX_HEADER_BYTES) {
            ok = false;
            break;
        }
        char chunk[4096];
        ssize_t got = 0;
        if (!read_deadline(fd, chunk, sizeof chunk, ATLAS_GW_HEADER_TIMEOUT_MS, &got)) {
            ok = false;
            break;
        }
        if (atlas_buf_append(&req, chunk, (size_t)got, &err) != ATLAS_OK) {
            ok = false;
            break;
        }
    }

    if (ok) {
        /* Parse just enough to learn the declared body length. The full parse
         * happens inside `atlas_gateway_serve_bytes`, which is the one place
         * routing decisions are made. */
        atlas_http_request peek;
        atlas_err perr;
        atlas_err_init(&perr);
        int64_t want = 0;
        if (atlas_http_parse_head(req.data, req.len, g->policy.max_request_bytes, &peek, &perr) ==
            ATLAS_OK) {
            want = peek.content_length;
        }
        atlas_http_request_free(&peek);

        while (ok && (int64_t)(req.len - head_end) < want) {
            char chunk[8192];
            ssize_t got = 0;
            if (!read_deadline(fd, chunk, sizeof chunk, ATLAS_GW_BODY_TIMEOUT_MS, &got)) {
                ok = false;
                break;
            }
            if (atlas_buf_append(&req, chunk, (size_t)got, &err) != ATLAS_OK) {
                ok = false;
            }
            if (req.len > ATLAS_GW_MAX_HEADER_BYTES + (size_t)g->policy.max_request_bytes) {
                ok = false;
            }
        }
    }

    if (ok) {
        if (atlas_gateway_serve_bytes(g, req.data, req.len, &resp, &err) == ATLAS_OK) {
            (void)write_all(fd, resp.data, resp.len, ATLAS_GW_IDLE_TIMEOUT_MS);
        }
    } else {
        /* A truncated, over-long or timed-out request still gets an answer
         * rather than a silent close: a client that is merely slow deserves to
         * be told, and a scanner learns nothing from 400 that it did not know
         * from connecting. */
        static const char BAD[] =
            "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 52\r\n"
            "Connection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
            "\r\n{\"ok\":false,\"error\":{\"code\":\"bad_request\"}}\n";
        (void)write_all(fd, BAD, sizeof BAD - 1u, 2000);
    }

    atlas_buf_free(&resp);
    atlas_buf_free(&req);
    (void)close(fd);
}

static void *conn_thread(void *ud) {
    conn_job *j = (conn_job *)ud;
    serve_one(j->g, j->fd);
    (void)pthread_mutex_lock(&gw_conn_lock);
    gw_conn_live--;
    (void)pthread_mutex_unlock(&gw_conn_lock);
    free(j);
    return NULL;
}

atlas_status atlas_gateway_serve(atlas_gateway *g, atomic_bool *stop, atlas_err *err) {
    int lfd = -1;
    atlas_status st = gw_listen(g, &lfd, err);
    if (st != ATLAS_OK) {
        return st;
    }
    /* A peer that disconnects mid-write must not kill the process. */
    (void)signal(SIGPIPE, SIG_IGN);

    gw_log(g, "listening on %s port %d (remote_mcp=%s web_gui=%s tls=%s)", g->policy.listen_addr,
           g->policy.listen_port, g->policy.remote_mcp ? "yes" : "no",
           g->policy.web_gui ? "yes" : "no", atlas_gwpolicy_tls_name(g->policy.tls_mode));

    while (!atomic_load(stop)) {
        struct pollfd p = {lfd, POLLIN, 0};
        int rc = poll(&p, 1, 500);
        if (rc <= 0) {
            continue;
        }
        int cfd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
        if (cfd < 0) {
            continue;
        }

        bool over = false;
        (void)pthread_mutex_lock(&gw_conn_lock);
        if (gw_conn_live >= g->policy.max_concurrent) {
            over = true;
        } else {
            gw_conn_live++;
        }
        (void)pthread_mutex_unlock(&gw_conn_lock);

        if (over) {
            /* Refused deterministically and told so, rather than queued. */
            static const char BUSY[] =
                "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\n"
                "Content-Length: 55\r\nConnection: close\r\nRetry-After: 1\r\n"
                "Cache-Control: no-store\r\n\r\n"
                "{\"ok\":false,\"error\":{\"code\":\"too_many_connections\"}}";
            (void)write_all(cfd, BUSY, sizeof BUSY - 1u, 1000);
            (void)close(cfd);
            continue;
        }

        conn_job *j = calloc(1, sizeof(*j));
        if (j == NULL) {
            (void)pthread_mutex_lock(&gw_conn_lock);
            gw_conn_live--;
            (void)pthread_mutex_unlock(&gw_conn_lock);
            (void)close(cfd);
            continue;
        }
        j->g = g;
        j->fd = cfd;
        pthread_t t;
        if (pthread_create(&t, NULL, conn_thread, j) != 0) {
            (void)pthread_mutex_lock(&gw_conn_lock);
            gw_conn_live--;
            (void)pthread_mutex_unlock(&gw_conn_lock);
            (void)close(cfd);
            free(j);
            continue;
        }
        /* Detached: nothing joins these, and a connection that finishes must
         * not wait for the accept loop to notice. */
        (void)pthread_detach(t);
    }

    (void)close(lfd);
    gw_log(g, "%s", "stopped");
    return ATLAS_OK;
}

/* --- the commands ---------------------------------------------------------- */

/* Written from a signal handler and read by the accept loop. An atomic, not a
 * `volatile bool`: a handler may run on any thread. */
static atomic_bool gw_stop_flag = false;

static void gw_on_signal(int sig) {
    (void)sig;
    atomic_store(&gw_stop_flag, true);
}

atlas_status atlas_service_gateway_run(atlas_err *err) {
    atlas_gwpolicy p;
    atlas_gwpolicy_load(&p);
    if (p.state != ATLAS_GWPOLICY_ENABLED) {
        /* Refused with the reason and with what would change it. A gateway that
         * declined to start and said only "disabled" would send an operator
         * looking in the wrong place. */
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "the Atlas gateway is not enabled: %s (%s). The policy is %s",
                             atlas_gwpolicy_reason_name(p.reason),
                             atlas_gwpolicy_reason_detail(p.reason), ATLAS_GWPOLICY_PATH);
    }

    atlas_gateway_opts o;
    memset(&o, 0, sizeof o);
    o.errout = stderr;
    atlas_gateway *g = NULL;
    atlas_status st = atlas_gateway_open(&p, &o, &g, err);
    if (st != ATLAS_OK) {
        return st;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = gw_on_signal;
    (void)sigaction(SIGTERM, &sa, NULL);
    (void)sigaction(SIGINT, &sa, NULL);

    atomic_store(&gw_stop_flag, false);
    st = atlas_gateway_serve(g, &gw_stop_flag, err);
    atlas_gateway_close(g);
    return st;
}

/* A16. Renders `remote_dispose_kinds` as a space-separated list of
 * `atlas_decision_kind_name`, always in the kind table's own order rather
 * than the order an operator happened to write them in the policy -- the
 * same "one canonical rendering for one set" discipline
 * `atlas_apikey_scopes_render` already follows for a scope mask, so the
 * same grant always prints as the same bytes. An empty mask renders as an
 * empty string, matched by the caller against the "(none ...)" wording
 * rather than printed as such itself, so this function has exactly one
 * job. */
static atlas_status render_dispose_kinds(uint32_t mask, atlas_buf *out, atlas_err *err) {
    for (size_t i = 0; i < atlas_decision_kind_count(); i++) {
        atlas_decision_kind k = atlas_decision_kind_at(i);
        if ((mask & ATLAS_DECISION_KIND_BIT(k)) == 0u) {
            continue;
        }
        if (out->len > 0) {
            atlas_status st = atlas_buf_append(out, " ", 1, err);
            if (st != ATLAS_OK) {
                return st;
            }
        }
        atlas_status st = atlas_buf_appendf(out, err, "%s", atlas_decision_kind_name(k));
        if (st != ATLAS_OK) {
            return st;
        }
    }
    return ATLAS_OK;
}

atlas_status atlas_service_gateway_status_for(FILE *out, bool json, const atlas_gwpolicy *p,
                                              atlas_err *err) {
    if (json) {
        atlas_json *j = atlas_json_new(out, err);
        if (j == NULL) {
            return err->status;
        }
        atlas_status st = atlas_json_obj_begin(j, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "state", atlas_gwpolicy_state_name(p->state), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "reason", atlas_gwpolicy_reason_name(p->reason), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "detail", atlas_gwpolicy_reason_detail(p->reason), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "policy_path", ATLAS_GWPOLICY_PATH, err);
        }
        if (st == ATLAS_OK && p->state == ATLAS_GWPOLICY_ENABLED) {
            if (st == ATLAS_OK) {
                st = atlas_json_key_str(j, "listen_addr", p->listen_addr, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_key_int(j, "listen_port", p->listen_port, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_key_str(j, "tls_mode", atlas_gwpolicy_tls_name(p->tls_mode), err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_key_bool(j, "remote_mcp", p->remote_mcp, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_key_bool(j, "web_gui", p->web_gui, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_key_int(j, "gateway_uid", p->gateway_uid, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_key_int(j, "allowed_origins", (int64_t)p->origin_count, err);
            }
            if (st == ATLAS_OK) {
                /* The one policy line that most moves this deployment's own
                 * threat model, so it is named here rather than left for an
                 * auditor to find only by reading a root-owned file they may
                 * not be able to open. Empty (the default) is itself the
                 * answer "no unauthenticated read is granted" and is printed
                 * as such, never omitted the way a ceiling like
                 * `max_request_bytes` is. */
                atlas_buf anon = ATLAS_BUF_INIT;
                atlas_err aerr;
                atlas_err_init(&aerr);
                (void)atlas_apikey_scopes_render(p->web_gui_anonymous_scopes, &anon, &aerr);
                st = atlas_json_key_str(j, "web_gui_anonymous_scopes", atlas_buf_cstr(&anon), err);
                atlas_buf_free(&anon);
            }
            if (st == ATLAS_OK) {
                /* A16. `acbd7ad`'s review put the argument for `anon:` above
                 * as "an authentication bypass an auditor must see here, not
                 * a ceiling safe to leave to the policy file" -- and a
                 * disposal credential *changes* a lifecycle state rather
                 * than only reading one, so the same argument applies with
                 * more force, not less. Empty is itself the answer "remote
                 * disposal is off" and is printed as such, never omitted. */
                st = atlas_json_key_str(j, "remote_dispose_key", p->remote_dispose_key, err);
            }
            if (st == ATLAS_OK) {
                atlas_buf kinds = ATLAS_BUF_INIT;
                atlas_err kerr;
                atlas_err_init(&kerr);
                (void)render_dispose_kinds(p->remote_dispose_kinds, &kinds, &kerr);
                st = atlas_json_key_str(j, "remote_dispose_kinds", atlas_buf_cstr(&kinds), err);
                atlas_buf_free(&kinds);
            }
            if (st == ATLAS_OK) {
                /* A16, amended 2026-09-04. The operator's written acceptance
                 * that this credential crosses the network unencrypted. A
                 * ceiling like `max_request_bytes` may be left to the policy
                 * file; a fact this close to authentication may not --
                 * yesterday's review made exactly that distinction for a
                 * weaker mechanism, and disposal is stronger than a bypass. */
                st = atlas_json_key_bool(j, "cleartext_disposal_accepted",
                                         p->cleartext_disposal_accepted, err);
            }
        }
        /* Stated as a field rather than left to a reader's assumption, exactly
         * as the backup report states `encrypted` and `signed`. */
        if (st == ATLAS_OK) {
            st = atlas_json_key_bool(j, "terminates_tls", false, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_obj_end(j, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_finish(j, err);
        } else {
            atlas_json_free(j);
        }
        return st;
    }

    (void)fprintf(out, "gateway: %s\n", atlas_gwpolicy_state_name(p->state));
    (void)fprintf(out, "reason:  %s\n", atlas_gwpolicy_reason_name(p->reason));
    (void)fprintf(out, "         %s\n", atlas_gwpolicy_reason_detail(p->reason));
    (void)fprintf(out, "policy:  %s\n", ATLAS_GWPOLICY_PATH);
    if (p->detail[0] != '\0') {
        (void)fprintf(out, "at:      %s\n", p->detail);
    }
    if (p->state == ATLAS_GWPOLICY_ENABLED) {
        (void)fprintf(out, "listen:  %s port %d\n", p->listen_addr, p->listen_port);
        (void)fprintf(out, "tls:     %s (Atlas terminates no TLS)\n",
                      atlas_gwpolicy_tls_name(p->tls_mode));
        (void)fprintf(out, "surface: remote_mcp=%s web_gui=%s\n", p->remote_mcp ? "yes" : "no",
                      p->web_gui ? "yes" : "no");
        (void)fprintf(out, "uid:     %lld\n", p->gateway_uid);
        (void)fprintf(out, "origins: %zu allowed\n", p->origin_count);
        {
            /* Same reasoning as the JSON form above: this is an
             * authentication bypass an auditor must see here, not a ceiling
             * safe to leave to the policy file. */
            atlas_buf anon = ATLAS_BUF_INIT;
            atlas_err aerr;
            atlas_err_init(&aerr);
            (void)atlas_apikey_scopes_render(p->web_gui_anonymous_scopes, &anon, &aerr);
            (void)fprintf(out, "anon:    %s\n",
                          atlas_buf_cstr(&anon)[0] != '\0'
                              ? atlas_buf_cstr(&anon)
                              : "(none -- /api/ still requires a session or bearer credential)");
            atlas_buf_free(&anon);
        }
        {
            /* A16. `acbd7ad`'s reason for `anon:` above, one step stronger:
             * this is a credential that can change a lifecycle state, not
             * only read one, so it belongs here even more than a read-only
             * bypass does. Both lines print unconditionally, exactly as
             * `anon:` does, so their absence from a policy is itself the
             * printed answer rather than a line an auditor has to notice is
             * missing. */
            if (p->remote_dispose_key[0] != '\0') {
                atlas_buf kinds = ATLAS_BUF_INIT;
                atlas_err kerr;
                atlas_err_init(&kerr);
                (void)render_dispose_kinds(p->remote_dispose_kinds, &kinds, &kerr);
                (void)fprintf(out, "dispose: key_%s (%s)\n", p->remote_dispose_key,
                              atlas_buf_cstr(&kinds));
                atlas_buf_free(&kinds);
            } else {
                (void)fprintf(out,
                              "dispose: (none -- the browser can read and queue, never dispose)\n");
            }
            if (p->cleartext_disposal_accepted) {
                (void)fprintf(
                    out,
                    "clear:   ACCEPTED -- operator_accepts_cleartext_disposal = yes: the disposal "
                    "credential crosses this network unencrypted, and a captured credential "
                    "disposes until it is revoked\n");
            } else {
                (void)fprintf(out,
                              "clear:   (not accepted -- a disposal credential is offered only "
                              "behind tls_mode = REVERSE_PROXY)\n");
            }
        }
        if (p->public_url[0] != '\0') {
            (void)fprintf(out, "url:     %s/mcp\n", p->public_url);
        }
    }
    return ATLAS_OK;
}

atlas_status atlas_service_gateway_status(FILE *out, bool json, atlas_err *err) {
    atlas_gwpolicy p;
    atlas_gwpolicy_load(&p);
    return atlas_service_gateway_status_for(out, json, &p, err);
}
