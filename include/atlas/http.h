/* Atlas - A9: a deliberately small HTTP/1.1 request reader.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * This is not a web server. It is the smallest parser that can accept the two
 * request shapes A9 serves — a JSON POST and a GET — from a peer that may be
 * hostile, and refuse everything else.
 *
 * ## What it does not do, and why each absence is deliberate
 *
 *   - **No generic header map.** Only the handful of headers Atlas acts on are
 *     extracted, into fixed fields. A header Atlas does not understand cannot
 *     influence anything, because there is nowhere for it to be stored. This is
 *     the same argument A2 makes for reading exactly one member of
 *     `tool_input`.
 *   - **No chunked transfer encoding.** A request must carry `Content-Length`.
 *     Chunked decoding is a second length-bearing path, and two of those is how
 *     a request smuggling bug happens; a request carrying `Transfer-Encoding` at
 *     all is refused rather than interpreted. Every client A9 serves sends a
 *     `Content-Length` for a JSON body.
 *   - **No pipelining, no continuation lines, no multipart, no compression, no
 *     ranges, no upgrades.** Each is a parser feature reachable by an
 *     unauthenticated peer.
 *   - **No URL decoding of the path.** Routes are compared as exact literal
 *     strings. A path is never joined to a filesystem path anywhere in the
 *     gateway, so `%2e%2e%2f` is not a traversal risk — it is simply a path that
 *     matches no route. Decoding it would create the risk that not decoding it
 *     avoids.
 *
 * ## The length is checked before a byte of body is read
 *
 * `Content-Length` is validated against the ceiling before anything is
 * allocated, so a peer cannot make Atlas reserve memory by claiming a large
 * request. That is the rule the Unix-socket framing already follows.
 *
 * ## The Authorization header holds a secret
 *
 * `authorization` is the one field here that carries credential material. It is
 * wiped by `atlas_http_request_free`, it is never logged, never audited and
 * never echoed into an error, and no route reads it except the one that
 * authenticates.
 */
#ifndef ATLAS_HTTP_H
#define ATLAS_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "atlas/apikey.h"
#include "atlas/error.h"
#include "atlas/gwpolicy.h"
#include "atlas/limits.h"

#define ATLAS_HTTP_METHOD_MAX 16u
#define ATLAS_HTTP_PATH_MAX 512u
#define ATLAS_HTTP_QUERY_MAX 512u
#define ATLAS_HTTP_ACCEPT_MAX 256u
#define ATLAS_HTTP_CTYPE_MAX 128u
/* "Bearer " plus a token. */
#define ATLAS_HTTP_AUTH_MAX (ATLAS_APIKEY_TOKEN_MAX + 16u)
#define ATLAS_HTTP_MCP_SESSION_MAX 64u

typedef struct atlas_http_request {
    char method[ATLAS_HTTP_METHOD_MAX];
    /* The path with the query string removed, compared as an exact literal
     * against a fixed route table. Never decoded and never joined to a
     * filesystem path. */
    char path[ATLAS_HTTP_PATH_MAX];
    char query[ATLAS_HTTP_QUERY_MAX];

    /* Credential material. Wiped by `atlas_http_request_free`. */
    char authorization[ATLAS_HTTP_AUTH_MAX];

    char content_type[ATLAS_HTTP_CTYPE_MAX];
    char accept[ATLAS_HTTP_ACCEPT_MAX];
    char origin[ATLAS_GWPOLICY_ORIGIN_MAX];
    /* The browser session cookie, when one was sent. Extracted by name from the
     * Cookie header; every other cookie is ignored rather than stored. */
    char session[ATLAS_GW_SESSION_TOKEN_HEX + 1u];
    /* `Mcp-Session-Id`, echoed back so a client that tracks one is satisfied.
     * Nothing is authorised by it. */
    char mcp_session[ATLAS_HTTP_MCP_SESSION_MAX];

    int64_t content_length;
    bool has_content_length;
    bool keep_alive;
    /* The body begins here, as an offset into the buffer the head was parsed
     * from. */
    size_t body_offset;
} atlas_http_request;

void atlas_http_request_init(atlas_http_request *r);
/* Wipes the whole structure, including the Authorization header. Call it on
 * every path. */
void atlas_http_request_free(atlas_http_request *r);

/* Parses a complete request head — everything up to and including the blank
 * line that ends the headers.
 *
 * `len` must cover at least that much; `atlas_http_head_complete` says whether
 * it does. Returns ATLAS_ERR_USAGE for a malformed request, and the caller
 * answers 400 without quoting anything the peer sent.
 *
 * `max_body` is the ceiling `Content-Length` is checked against, before any body
 * is read. A larger declared length is ATLAS_ERR_INTEGRITY and the caller
 * answers 413. */
atlas_status atlas_http_parse_head(const char *buf, size_t len, int64_t max_body,
                                   atlas_http_request *out, atlas_err *err);

/* The offset just past the end of the header block, or 0 when the block is not
 * yet complete. Only `\r\n\r\n` terminates a head: a bare-LF terminator is one
 * of the two ways a request smuggler gets two parsers to disagree, so Atlas
 * recognises exactly one. */
size_t atlas_http_head_complete(const char *buf, size_t len);

/* --- responses -------------------------------------------------------------
 *
 * One writer for every response, so no route can invent a header set. The
 * security headers below are attached to every response without exception —
 * including errors, which are the ones most likely to be rendered in a browser
 * by accident. */
typedef struct atlas_http_response {
    int status;
    const char *content_type;
    /* Echoed only when the request's Origin matched the policy allowlist. An
     * unmatched Origin gets no CORS header at all, which is what makes a
     * browser refuse the response. */
    const char *allow_origin;
    const char *extra;      /* additional fixed header lines, or NULL */
    /* The Content-Security-Policy value, without the header name.
     *
     * NULL means the default: `default-src 'none'`, which makes an error body
     * inert even if something one day puts reflected content in one.
     *
     * A caller that needs a different policy must set this rather than adding a
     * second header through `extra`. Two CSP headers are both enforced and a
     * browser applies their *intersection*, so a page that added
     * `connect-src 'self'` alongside a default of `default-src 'none'` would
     * have no connect permission at all — which is exactly the bug this field
     * exists to make impossible. */
    const char *csp;
    const void *body;
    size_t body_len;
    bool keep_alive;
} atlas_http_response;

void atlas_http_response_init(atlas_http_response *r);

/* Serialises the head into `out`, which must hold at least
 * ATLAS_HTTP_RESPONSE_HEAD_MAX bytes. Returns the number written, or 0 if it
 * would not fit — which the caller treats as an internal error rather than
 * truncating a response head. */
#define ATLAS_HTTP_RESPONSE_HEAD_MAX 2048u
size_t atlas_http_write_head(const atlas_http_response *r, char *out, size_t out_size);

/* The reason phrase for a status Atlas emits. Fixed strings; a status Atlas
 * does not emit returns "Unknown", which never reaches a client because no
 * route produces one. */
const char *atlas_http_status_text(int status);

/* True when `origin` exactly matches one the policy allows.
 *
 * Compared whole — scheme, host and port — and never by prefix or suffix. A
 * suffix match is how `https://atlas.example.com.attacker.net` is accepted by a
 * check that meant to allow `atlas.example.com`. */
bool atlas_http_origin_allowed(const atlas_gwpolicy *p, const char *origin);

#endif /* ATLAS_HTTP_H */
