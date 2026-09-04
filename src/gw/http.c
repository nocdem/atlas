/* Atlas - A9: a deliberately small HTTP/1.1 request reader.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See atlas/http.h for what this refuses and why each absence is deliberate.
 */
#include <stdio.h>
#include <string.h>

#include "atlas/http.h"

void atlas_http_request_init(atlas_http_request *r) {
    memset(r, 0, sizeof(*r));
    r->content_length = 0;
    /* HTTP/1.1 defaults to persistent connections. */
    r->keep_alive = true;
}

void atlas_http_request_free(atlas_http_request *r) {
    if (r == NULL) {
        return;
    }
    /* Through a volatile pointer: `authorization` holds credential material and
     * the compiler must not treat clearing it as a dead store. */
    volatile unsigned char *p = (volatile unsigned char *)r;
    for (size_t i = 0; i < sizeof(*r); i++) {
        p[i] = 0;
    }
}

size_t atlas_http_head_complete(const char *buf, size_t len) {
    /* Only CRLFCRLF. A bare-LF terminator is one of the two ways a request
     * smuggler gets two parsers to disagree about where a message ends, so
     * Atlas recognises exactly one spelling and treats the other as "not yet
     * complete" — which times out rather than being interpreted. */
    if (len < 4u) {
        return 0;
    }
    for (size_t i = 0; i + 4u <= len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return i + 4u;
        }
    }
    return 0;
}

/* Case-insensitive comparison of a header name against a literal. */
static bool name_is(const char *line, size_t nlen, const char *want) {
    size_t wlen = strlen(want);
    if (nlen != wlen) {
        return false;
    }
    for (size_t i = 0; i < nlen; i++) {
        char a = line[i];
        char b = want[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

static bool copy_field(char *dst, size_t dst_size, const char *src, size_t len) {
    if (len + 1u > dst_size) {
        return false;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
    return true;
}

/* Every byte of a header value must be printable ASCII or a tab.
 *
 * This is what stops a header value carrying a control byte into an audit row,
 * a log line or a response header. A value that fails is a malformed request,
 * refused rather than sanitised: rewriting it would mean acting on something
 * different from what was sent. */
static bool value_printable(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c != '\t' && (c < 0x20u || c == 0x7fu)) {
            return false;
        }
    }
    return true;
}

/* Extracts one named cookie from a Cookie header, ignoring every other. */
static void take_cookie(const char *val, size_t len, const char *name, char *out,
                        size_t out_size) {
    out[0] = '\0';
    size_t nlen = strlen(name);
    size_t i = 0;
    while (i < len) {
        while (i < len && (val[i] == ' ' || val[i] == ';')) {
            i++;
        }
        size_t start = i;
        while (i < len && val[i] != ';') {
            i++;
        }
        size_t end = i;
        /* `name=value`, compared exactly. A cookie whose name merely ends with
         * ours is a different cookie. */
        if (end > start + nlen && (size_t)(end - start) > nlen &&
            strncmp(val + start, name, nlen) == 0 && val[start + nlen] == '=') {
            const char *v = val + start + nlen + 1u;
            size_t vlen = end - start - nlen - 1u;
            if (vlen + 1u <= out_size) {
                memcpy(out, v, vlen);
                out[vlen] = '\0';
            }
            return;
        }
    }
}

atlas_status atlas_http_parse_head(const char *buf, size_t len, int64_t max_body,
                                   atlas_http_request *out, atlas_err *err) {
    atlas_http_request_init(out);

    /* Every refusal below uses a message that describes the *shape* of the
     * problem and never reproduces what was sent. A parse error is answered
     * with 400 and a fixed string; the detail is for Atlas' own log. */
    size_t head_end = atlas_http_head_complete(buf, len);
    if (head_end == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "the request head is incomplete");
    }
    if (head_end > ATLAS_GW_MAX_HEADER_BYTES) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "the request head is too large");
    }
    out->body_offset = head_end;

    /* --- the request line --- */
    size_t i = 0;
    while (i < head_end && buf[i] != '\r') {
        i++;
    }
    size_t line_len = i;
    if (line_len == 0 || line_len > ATLAS_GW_MAX_REQUEST_LINE) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "the request line is malformed");
    }
    /* METHOD SP TARGET SP VERSION — exactly two spaces, and no leading one. A
     * request line with anything else in it is refused rather than salvaged. */
    const char *sp1 = memchr(buf, ' ', line_len);
    if (sp1 == NULL || sp1 == buf) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "the request line is malformed");
    }
    size_t mlen = (size_t)(sp1 - buf);
    const char *target = sp1 + 1;
    size_t rest = line_len - mlen - 1u;
    const char *sp2 = memchr(target, ' ', rest);
    if (sp2 == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "the request line is malformed");
    }
    size_t tlen = (size_t)(sp2 - target);
    const char *version = sp2 + 1;
    size_t vlen = line_len - mlen - 1u - tlen - 1u;
    if (memchr(version, ' ', vlen) != NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "the request line is malformed");
    }
    if (!copy_field(out->method, sizeof out->method, buf, mlen)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "the method is not one Atlas serves");
    }
    for (size_t k = 0; k < mlen; k++) {
        /* Methods are uppercase ASCII letters. Nothing else reaches the route
         * table. */
        if (out->method[k] < 'A' || out->method[k] > 'Z') {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "the method is not one Atlas serves");
        }
    }

    /* HTTP/1.1 or HTTP/1.0. `keep_alive` defaults from the version and is then
     * overridden by an explicit Connection header. */
    if (vlen == 8u && strncmp(version, "HTTP/1.0", 8) == 0) {
        out->keep_alive = false;
    } else if (vlen != 8u || strncmp(version, "HTTP/1.1", 8) != 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "unsupported HTTP version");
    }

    /* The target must be origin-form: a path beginning with `/`. An absolute
     * URI or an authority-form target is refused — Atlas is not a proxy, and
     * accepting a target that names a host is how one is turned into one. */
    if (tlen == 0 || target[0] != '/') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "the request target is not a path");
    }
    if (!value_printable(target, tlen)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "the request target is malformed");
    }
    const char *q = memchr(target, '?', tlen);
    size_t path_len = q != NULL ? (size_t)(q - target) : tlen;
    if (!copy_field(out->path, sizeof out->path, target, path_len)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "the request target is too long");
    }
    if (q != NULL) {
        size_t qlen = tlen - path_len - 1u;
        if (!copy_field(out->query, sizeof out->query, q + 1, qlen)) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "the query string is too long");
        }
    }

    /* --- the headers --- */
    i = line_len + 2u; /* past CRLF */
    size_t seen = 0;
    bool have_length = false;
    while (i + 2u <= head_end) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') {
            break; /* the blank line ending the block */
        }
        size_t start = i;
        while (i < head_end && buf[i] != '\r') {
            i++;
        }
        size_t hlen = i - start;
        i += 2u; /* past CRLF */
        if (hlen == 0 || hlen > ATLAS_GW_MAX_HEADER_LINE) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "a header line is malformed");
        }
        if (++seen > ATLAS_GW_MAX_HEADERS) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "too many headers");
        }
        /* A leading space is a continuation line, which Atlas does not accept:
         * folding is obsolete and is a second way to spell one value. */
        if (buf[start] == ' ' || buf[start] == '\t') {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "folded headers are not accepted");
        }
        const char *colon = memchr(buf + start, ':', hlen);
        if (colon == NULL) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "a header line is malformed");
        }
        size_t nlen = (size_t)(colon - (buf + start));
        const char *val = colon + 1;
        size_t vlen2 = hlen - nlen - 1u;
        while (vlen2 > 0 && (*val == ' ' || *val == '\t')) {
            val++;
            vlen2--;
        }
        while (vlen2 > 0 && (val[vlen2 - 1u] == ' ' || val[vlen2 - 1u] == '\t')) {
            vlen2--;
        }
        if (!value_printable(val, vlen2)) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "a header value is malformed");
        }

        const char *name = buf + start;
        if (name_is(name, nlen, "content-length")) {
            if (have_length) {
                /* Two of them is a request smuggling primitive whatever they
                 * say, including when they agree. */
                return atlas_err_set(err, ATLAS_ERR_USAGE, "duplicate Content-Length");
            }
            have_length = true;
            int64_t n = 0;
            if (vlen2 == 0 || vlen2 > 18u) {
                return atlas_err_set(err, ATLAS_ERR_USAGE, "Content-Length is malformed");
            }
            for (size_t k = 0; k < vlen2; k++) {
                if (val[k] < '0' || val[k] > '9') {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "Content-Length is malformed");
                }
                n = n * 10 + (val[k] - '0');
            }
            /* Checked here, before a single byte of body has been read, so a
             * peer cannot make Atlas allocate by claiming a large request. */
            if (n > max_body) {
                return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "the request body is too large");
            }
            out->content_length = n;
            out->has_content_length = true;
        } else if (name_is(name, nlen, "transfer-encoding")) {
            /* Refused outright rather than interpreted. Chunked decoding would
             * be a second length-bearing path, and two of those is how one
             * parser is made to disagree with another about where a message
             * ends. Every client A9 serves sends Content-Length. */
            return atlas_err_set(err, ATLAS_ERR_USAGE, "Transfer-Encoding is not accepted");
        } else if (name_is(name, nlen, "authorization")) {
            if (!copy_field(out->authorization, sizeof out->authorization, val, vlen2)) {
                /* Over-long: refused without being stored anywhere. */
                return atlas_err_set(err, ATLAS_ERR_USAGE, "the Authorization header is too long");
            }
        } else if (name_is(name, nlen, "content-type")) {
            (void)copy_field(out->content_type, sizeof out->content_type, val, vlen2);
        } else if (name_is(name, nlen, "accept")) {
            (void)copy_field(out->accept, sizeof out->accept, val, vlen2);
        } else if (name_is(name, nlen, "origin")) {
            (void)copy_field(out->origin, sizeof out->origin, val, vlen2);
        } else if (name_is(name, nlen, "host")) {
            /* Same discipline as `origin` immediately above: an over-long
             * value is not truncated — `copy_field` fails atomically and
             * `out->host` is left as it was: empty, from
             * `atlas_http_request_init`, if this is the first `Host` header
             * seen; the previously parsed value, if an earlier one already
             * fit. Neither case is a partial value, and a repeated header is
             * not specially detected either way, so the last one that fits
             * wins. All three match how `origin` already behaves; a `Host`
             * value is used only to gate the anonymous floor
             * (`host_matches_listener`, `src/gw/gateway.c`), never to
             * refuse or route the request itself, so failing that gate is
             * the whole consequence of either case. */
            (void)copy_field(out->host, sizeof out->host, val, vlen2);
        } else if (name_is(name, nlen, "cookie")) {
            take_cookie(val, vlen2, "atlas_session", out->session, sizeof out->session);
        } else if (name_is(name, nlen, "mcp-session-id")) {
            (void)copy_field(out->mcp_session, sizeof out->mcp_session, val, vlen2);
        } else if (name_is(name, nlen, "connection")) {
            if (vlen2 == 5u && (val[0] == 'c' || val[0] == 'C')) {
                out->keep_alive = false; /* close */
            }
        }
        /* Every other header is ignored — not stored, not forwarded, not
         * logged. There is nowhere for it to go. */
    }

    return ATLAS_OK;
}

/* --- responses ------------------------------------------------------------- */

void atlas_http_response_init(atlas_http_response *r) {
    memset(r, 0, sizeof(*r));
    r->status = 500;
    r->content_type = "application/json";
    r->keep_alive = false;
}

const char *atlas_http_status_text(int status) {
    switch (status) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 302: return "Found";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    default: break;
    }
    return "Unknown";
}

size_t atlas_http_write_head(const atlas_http_response *r, char *out, size_t out_size) {
    /* One writer for every response, so no route can invent a header set.
     *
     * The security headers are attached without exception, including to errors
     * — which are the responses most likely to be rendered in a browser by
     * accident. `default-src 'none'` is what makes an error body inert even if
     * something one day puts reflected content in one; the GUI's own page
     * overrides it with a policy that permits only its inline assets.
     *
     * `Cache-Control: no-store` is on everything because every response here is
     * either a credentialed API answer or an error about one, and neither
     * belongs in a shared cache. */
    int n = snprintf(
        out, out_size,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: %s\r\n"
        "Cache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "X-Frame-Options: DENY\r\n"
        "Referrer-Policy: no-referrer\r\n"
        "Content-Security-Policy: %s\r\n"
        "%s%s%s"
        "%s"
        "\r\n",
        r->status, atlas_http_status_text(r->status), r->content_type, r->body_len,
        r->keep_alive ? "keep-alive" : "close",
        r->csp != NULL ? r->csp
                       : "default-src 'none'; frame-ancestors 'none'; base-uri 'none'",
        r->allow_origin != NULL ? "Access-Control-Allow-Origin: " : "",
        r->allow_origin != NULL ? r->allow_origin : "",
        r->allow_origin != NULL ? "\r\nAccess-Control-Allow-Credentials: true\r\nVary: Origin\r\n"
                                : "",
        r->extra != NULL ? r->extra : "");
    if (n <= 0 || (size_t)n >= out_size) {
        return 0;
    }
    return (size_t)n;
}

bool atlas_http_origin_allowed(const atlas_gwpolicy *p, const char *origin) {
    if (p == NULL || origin == NULL || origin[0] == '\0') {
        return false;
    }
    for (size_t i = 0; i < p->origin_count; i++) {
        /* Whole-string equality. Never a prefix or a suffix match: a suffix
         * match is how `https://atlas.example.com.attacker.net` is accepted by
         * a check that meant to allow `atlas.example.com`. */
        if (strcmp(p->origins[i], origin) == 0) {
            return true;
        }
    }
    return false;
}
