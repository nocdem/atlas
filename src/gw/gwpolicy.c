/* Atlas - A9: the root-owned gateway policy.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See atlas/gwpolicy.h for what a policy establishes and what it does not.
 *
 * The whole file fails closed. There is one `out->state = ATLAS_GWPOLICY_ENABLED`
 * assignment, it is the last statement of the loader, and every path that does
 * not reach it leaves the struct as `memset` left it — which is disabled,
 * because disabled is zero. There is no direction in which a degraded policy
 * exposes more.
 */
#define _GNU_SOURCE 1

#include "atlas/gwpolicy.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "atlas/decision.h" /* atlas_decision_kind, ATLAS_DECISION_KIND_BIT, atlas_decision_kind_parse */
#include "atlas/limits.h"
#include "atlas/rootpath.h"

/* A policy is a handful of `key = value` lines. Bounded for the reason the
 * system policy is: a file Atlas parses is a file whose size is somebody else's
 * choice — root's here — and the habit costs nothing. */
#define GWPOLICY_MAX_BYTES 8192u

const char *atlas_gwpolicy_state_name(atlas_gwpolicy_state s) {
    return s == ATLAS_GWPOLICY_ENABLED ? "ENABLED" : "DISABLED";
}

const char *atlas_gwpolicy_reason_name(atlas_gwpolicy_reason r) {
    switch (r) {
    case ATLAS_GWPOLICY_REASON_UNKNOWN: return "UNKNOWN";
    case ATLAS_GWPOLICY_REASON_ABSENT: return "ABSENT";
    case ATLAS_GWPOLICY_REASON_PATH_UNSAFE: return "PATH_UNSAFE";
    case ATLAS_GWPOLICY_REASON_WRITABLE: return "WRITABLE";
    case ATLAS_GWPOLICY_REASON_MALFORMED: return "MALFORMED";
    case ATLAS_GWPOLICY_REASON_DISABLED: return "DISABLED";
    case ATLAS_GWPOLICY_REASON_ACTIVE: return "ACTIVE";
    }
    return "UNKNOWN";
}

const char *atlas_gwpolicy_reason_detail(atlas_gwpolicy_reason r) {
    switch (r) {
    case ATLAS_GWPOLICY_REASON_UNKNOWN:
        return "the policy was never loaded, so no gateway runs";
    case ATLAS_GWPOLICY_REASON_ABSENT:
        return "no gateway policy is installed at " ATLAS_GWPOLICY_PATH ", so Atlas is not "
               "reachable from off this machine";
    case ATLAS_GWPOLICY_REASON_PATH_UNSAFE:
        return "a component of the policy path is a symbolic link or is malformed, so whoever "
               "can create links there would choose the policy";
    case ATLAS_GWPOLICY_REASON_WRITABLE:
        return "the policy, or a directory leading to it, can be modified by somebody other than "
               "root, so it constrains nobody — least of all the gateway it is meant to bound";
    case ATLAS_GWPOLICY_REASON_MALFORMED:
        return "the policy exists but does not describe a complete, safe gateway";
    case ATLAS_GWPOLICY_REASON_DISABLED:
        return "the policy is installed and says `enabled = no`, so no gateway runs; change that "
               "line to `enabled = yes` and restart";
    case ATLAS_GWPOLICY_REASON_ACTIVE:
        return "a root-anchored policy defines the listener, the surfaces and the bounds";
    }
    return "no gateway runs";
}

const char *atlas_gwpolicy_tls_name(atlas_gwpolicy_tls t) {
    switch (t) {
    case ATLAS_GWPOLICY_TLS_REVERSE_PROXY: return "REVERSE_PROXY";
    case ATLAS_GWPOLICY_TLS_NONE: return "NONE";
    case ATLAS_GWPOLICY_TLS_UNSET: break;
    }
    return "UNSET";
}

bool atlas_gwpolicy_is_loopback(const char *addr) {
    if (addr == NULL || addr[0] == '\0') {
        return true; /* the default, which is 127.0.0.1 */
    }
    if (strcmp(addr, "127.0.0.1") == 0 || strcmp(addr, "::1") == 0 ||
        strcmp(addr, "localhost") == 0) {
        return true;
    }
    /* The whole 127.0.0.0/8 block. Checked by prefix rather than by parsing,
     * because anything that is not exactly a loopback literal must fall through
     * to "not loopback" — the safe direction. */
    if (strncmp(addr, "127.", 4) == 0) {
        for (const unsigned char *p = (const unsigned char *)addr + 4; *p != '\0'; p++) {
            if ((*p < '0' || *p > '9') && *p != '.') {
                return false;
            }
        }
        return true;
    }
    return false;
}

/* --- parsing ----------------------------------------------------------------
 *
 * Deliberately dull, and identical in shape to the system and orchestration
 * policies: one `key = value` per line, `#` comments, no quoting, no escapes,
 * no includes and no continuations. Every one of those would be a parser
 * feature whose bugs are reachable from a security-relevant file. */

static bool take_value(const char *line, size_t len, const char *key, const char **val_out,
                       size_t *val_len_out) {
    size_t klen = strlen(key);
    if (len <= klen || strncmp(line, key, klen) != 0) {
        return false;
    }
    size_t i = klen;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) {
        i++;
    }
    if (i >= len || line[i] != '=') {
        return false;
    }
    i++;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) {
        i++;
    }
    size_t end = len;
    while (end > i && (line[end - 1u] == ' ' || line[end - 1u] == '\t' || line[end - 1u] == '\r')) {
        end--;
    }
    if (end <= i) {
        return false;
    }
    *val_out = line + i;
    *val_len_out = end - i;
    return true;
}

static bool copy_value(char *dst, size_t dst_size, const char *val, size_t len) {
    if (len + 1u > dst_size) {
        return false;
    }
    memcpy(dst, val, len);
    dst[len] = '\0';
    return true;
}

static bool parse_number(const char *val, size_t len, long long *out) {
    long long v = 0;
    if (len == 0 || len > 18u) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (val[i] < '0' || val[i] > '9') {
            return false;
        }
        v = v * 10 + (val[i] - '0');
    }
    *out = v;
    return true;
}

static bool parse_bool(const char *val, size_t len, bool *out) {
    if (len == 3u && strncmp(val, "yes", 3) == 0) {
        *out = true;
        return true;
    }
    if (len == 2u && strncmp(val, "no", 2) == 0) {
        *out = false;
        return true;
    }
    /* `true`/`false`/`1`/`0` are deliberately not accepted. One spelling, so a
     * policy an operator reads back says what they wrote. */
    return false;
}

/* Printable ASCII with no whitespace. Used for the listen address, the public
 * URL and every origin: none of them may carry a control byte into a log, an
 * audit row or an HTTP header. */
static bool printable_token(const char *s) {
    if (s[0] == '\0') {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        if (*p < 0x21u || *p > 0x7eu) {
            return false;
        }
    }
    return true;
}

/* `remote_dispose_key`'s grammar: exactly the display form `atlas api-key
 * list` prints -- "key_" followed by sixteen lowercase hex characters, the
 * same alphabet and length as `api_keys.key_id`. `ATLAS_APIKEY_ID_PREFIX`
 * (`atlas/gw.h`) names the same four bytes, but that header pulls in
 * `atlas/db.h`, which this loader must not -- `ATLAS_DECISION_KIND_BIT`'s own
 * header comment (`atlas/decision.h`) states the identical constraint for
 * this same file, which is why it lives in `decision.h` rather than the
 * heavier `decision_ops.h`. The literal is repeated here rather than shared
 * for the same reason. Stored *without* the prefix, because the prefix is a
 * display convention and `api_keys.key_id` never carries it. */
#define GWPOLICY_DISPOSE_KEY_PREFIX "key_"
#define GWPOLICY_DISPOSE_KEY_PREFIX_LEN 4u

static bool parse_dispose_key(const char *val, size_t vlen,
                              char out[ATLAS_APIKEY_SELECTOR_HEX + 1u]) {
    if (vlen != GWPOLICY_DISPOSE_KEY_PREFIX_LEN + ATLAS_APIKEY_SELECTOR_HEX ||
        strncmp(val, GWPOLICY_DISPOSE_KEY_PREFIX, GWPOLICY_DISPOSE_KEY_PREFIX_LEN) != 0) {
        return false;
    }
    const char *hex = val + GWPOLICY_DISPOSE_KEY_PREFIX_LEN;
    for (size_t i = 0; i < ATLAS_APIKEY_SELECTOR_HEX; i++) {
        char c = hex[i];
        bool is_lower_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!is_lower_hex) {
            /* Uppercase or any other byte. One spelling, exactly as a scope
             * name or a `tls_mode` value has exactly one spelling in this
             * file: a policy an operator reads back must say what it means. */
            return false;
        }
    }
    memcpy(out, hex, ATLAS_APIKEY_SELECTOR_HEX);
    out[ATLAS_APIKEY_SELECTOR_HEX] = '\0';
    return true;
}

void atlas_gwpolicy_load_at(const char *path, atlas_gwpolicy *out) {
    memset(out, 0, sizeof(*out));
    out->state = ATLAS_GWPOLICY_DISABLED;
    out->reason = ATLAS_GWPOLICY_REASON_ABSENT;

    atlas_rootpath_result rr = ATLAS_ROOTPATH_UNKNOWN;
    int fd = atlas_rootpath_open(path, false, &rr, out->detail, sizeof(out->detail));
    if (fd < 0) {
        switch (rr) {
        case ATLAS_ROOTPATH_MISSING: out->reason = ATLAS_GWPOLICY_REASON_ABSENT; break;
        case ATLAS_ROOTPATH_SYMLINK:
        case ATLAS_ROOTPATH_BAD_PATH: out->reason = ATLAS_GWPOLICY_REASON_PATH_UNSAFE; break;
        case ATLAS_ROOTPATH_NOT_REGULAR:
        case ATLAS_ROOTPATH_NOT_DIRECTORY: out->reason = ATLAS_GWPOLICY_REASON_MALFORMED; break;
        case ATLAS_ROOTPATH_UNKNOWN:
        case ATLAS_ROOTPATH_OK:
        case ATLAS_ROOTPATH_WRITABLE: out->reason = ATLAS_GWPOLICY_REASON_WRITABLE; break;
        }
        return;
    }

    char buf[GWPOLICY_MAX_BYTES];
    size_t total = 0;
    while (total < sizeof(buf)) {
        ssize_t got = read(fd, buf + total, sizeof(buf) - total);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            (void)close(fd);
            out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
            return;
        }
        if (got == 0) {
            break;
        }
        total += (size_t)got;
    }
    (void)close(fd);
    if (total == sizeof(buf)) {
        /* Larger than a policy can be. Refused rather than truncated: a
         * truncated policy is a different policy, and the part that would be
         * lost is whichever line happened to be last. */
        out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
        return;
    }

    atlas_gwpolicy_parse_buffer(buf, total, out);
}

/* The parser, over bytes somebody else established the provenance of.
 *
 * Split from the loader so the whole key matrix is testable. The root-ownership
 * walk is what makes a policy trustworthy and it can only pass for a real
 * root-owned file, so a test that had to go through it could exercise exactly
 * one outcome: refusal. Every malformed case below — an unknown key, a ceiling
 * above the compiled-in bound, a non-loopback bind with no TLS stance, a
 * wildcard origin — would then be unreachable from the suite, which is the
 * opposite of what those refusals deserve.
 *
 * This function establishes nothing about where the bytes came from. Production
 * reaches it only through `atlas_gwpolicy_load_at`, which opens the file from
 * `/` with no symlink traversed and every component root-owned. */
void atlas_gwpolicy_parse_buffer(const char *buf, size_t total, atlas_gwpolicy *out) {
    memset(out, 0, sizeof(*out));
    out->state = ATLAS_GWPOLICY_DISABLED;
    out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;

    /* Defaults before parsing, so an absent key means the documented default
     * rather than a zero that happens to be safe by accident. */
    (void)snprintf(out->listen_addr, sizeof out->listen_addr, "127.0.0.1");
    out->listen_port = 8787;
    out->max_request_bytes = (long long)ATLAS_GW_MAX_BODY_BYTES;
    out->max_concurrent = ATLAS_GW_MAX_CONNECTIONS;
    out->rate_limit_per_minute = ATLAS_GW_DEFAULT_RATE_PER_MINUTE;
    out->session_ttl_seconds = ATLAS_GW_DEFAULT_SESSION_TTL_SECONDS;

    bool have_enabled = false;
    bool enabled = false;
    bool addr_given = false;
    bool anon_scopes_given = false;
    bool dispose_key_given = false;
    bool dispose_kinds_given = false;
    bool accept_given = false;

    size_t i = 0;
    while (i < total) {
        size_t start = i;
        while (i < total && buf[i] != '\n') {
            i++;
        }
        size_t end = i;
        if (i < total) {
            i++;
        }
        while (start < end && (buf[start] == ' ' || buf[start] == '\t')) {
            start++;
        }
        if (start >= end || buf[start] == '#') {
            continue;
        }
        const char *line = buf + start;
        size_t len = end - start;
        const char *val = NULL;
        size_t vlen = 0;
        long long num = 0;

        if (take_value(line, len, "enabled", &val, &vlen)) {
            if (!parse_bool(val, vlen, &enabled)) {
                out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
                return;
            }
            have_enabled = true;
        } else if (take_value(line, len, "listen_addr", &val, &vlen)) {
            if (!copy_value(out->listen_addr, sizeof out->listen_addr, val, vlen) ||
                !printable_token(out->listen_addr)) {
                out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
                return;
            }
            addr_given = true;
        } else if (take_value(line, len, "listen_port", &val, &vlen)) {
            if (!parse_number(val, vlen, &num) || num < 1 || num > 65535) {
                out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
                return;
            }
            out->listen_port = (int)num;
        } else if (take_value(line, len, "public_url", &val, &vlen)) {
            if (!copy_value(out->public_url, sizeof out->public_url, val, vlen) ||
                !printable_token(out->public_url)) {
                out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
                return;
            }
        } else if (take_value(line, len, "tls_mode", &val, &vlen)) {
            if (vlen == 13u && strncmp(val, "REVERSE_PROXY", 13) == 0) {
                out->tls_mode = ATLAS_GWPOLICY_TLS_REVERSE_PROXY;
            } else if (vlen == 4u && strncmp(val, "NONE", 4) == 0) {
                out->tls_mode = ATLAS_GWPOLICY_TLS_NONE;
            } else {
                out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
                return;
            }
        } else if (take_value(line, len, "gateway_uid", &val, &vlen)) {
            if (!parse_number(val, vlen, &num) || num <= 0) {
                /* uid 0 is never named. A gateway terminating Internet
                 * connections as root is not a deployment Atlas will describe
                 * as configured. */
                out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
                return;
            }
            out->gateway_uid = num;
        } else if (take_value(line, len, "allowed_origin", &val, &vlen)) {
            if (out->origin_count >= ATLAS_GWPOLICY_MAX_ORIGINS) {
                out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
                return;
            }
            char *slot = out->origins[out->origin_count];
            if (!copy_value(slot, ATLAS_GWPOLICY_ORIGIN_MAX, val, vlen) || !printable_token(slot)) {
                out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
                return;
            }
            /* An origin is a scheme, a host and optionally a port — never a
             * path, and never `*`. A wildcard origin with credentials is the
             * one CORS configuration that is always wrong, so it is refused
             * here rather than being something an operator can reach. */
            if (strcmp(slot, "*") == 0 || strchr(slot, '/') == NULL) {
                out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
                return;
            }
            out->origin_count++;
        } else if (take_value(line, len, "remote_mcp", &val, &vlen)) {
            if (!parse_bool(val, vlen, &out->remote_mcp)) {
                out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
                return;
            }
        } else if (take_value(line, len, "web_gui", &val, &vlen)) {
            if (!parse_bool(val, vlen, &out->web_gui)) {
                out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
                return;
            }
        } else if (take_value(line, len, "web_gui_anonymous_scopes", &val, &vlen)) {
            /* A space-separated scope list, the same grammar an API key's
             * `--scope` list renders as. Copied to a NUL-terminated buffer
             * because `atlas_apikey_scopes_parse` takes a C string and this
             * value never approaches the buffer's size in practice — the
             * whole vocabulary rendered at once is under 90 bytes. */
            char scopes_buf[256];
            if (!copy_value(scopes_buf, sizeof scopes_buf, val, vlen)) {
                out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
                return;
            }
            atlas_scope_mask mask = 0u;
            atlas_err serr;
            atlas_err_init(&serr);
            if (atlas_apikey_scopes_parse(scopes_buf, &mask, &serr) != ATLAS_OK) {
                /* An unknown scope name. Fails closed the same way an unknown
                 * top-level key does. */
                out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
                return;
            }
            for (size_t si = 0; si < ATLAS_SCOPE__COUNT; si++) {
                if (atlas_scope_has(mask, (atlas_apikey_scope)si) &&
                    !atlas_apikey_scope_grantable((atlas_apikey_scope)si)) {
                    /* `memory:write` is the standing example: in the
                     * vocabulary, and not grantable to any A9 credential.
                     * Naming it here does not make it grantable — it makes
                     * the policy malformed, exactly as it would at
                     * `atlas api-key create`. */
                    out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
                    return;
                }
            }
            out->web_gui_anonymous_scopes = mask;
            anon_scopes_given = true;
        } else if (take_value(line, len, "remote_dispose_key", &val, &vlen)) {
            /* A16: the disposal credential's selector, in its display form.
             * `parse_dispose_key` both validates the shape and strips the
             * prefix, so `out->remote_dispose_key` is stored exactly as
             * `api_keys.key_id` is. */
            if (!parse_dispose_key(val, vlen, out->remote_dispose_key)) {
                out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
                return;
            }
            dispose_key_given = true;
        } else if (take_value(line, len, "remote_dispose_kinds", &val, &vlen)) {
            /* A16: a space-separated list of `atlas_decision_kind_parse`'s own
             * names, one bit per kind. This loader places no vocabulary in
             * front of the operator's choice narrower than
             * `atlas_decision_kind_parse` itself already accepts everywhere
             * else in Atlas — every kind, including DECISION and POLICY, is
             * nameable here, on purpose: a compiled-in subset would be a
             * silent narrowing an operator could only discover the day a
             * record of the missing kind refused them.
             *
             * Copied to a bounded buffer for the same reason the anonymous
             * scope list above is: `ATLAS_DECISION_KIND_MAX` is 8, and even
             * every name at once, longest first, comes nowhere near this
             * buffer. */
            char kinds_buf[256];
            if (!copy_value(kinds_buf, sizeof kinds_buf, val, vlen)) {
                out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
                return;
            }
            uint32_t kmask = 0u;
            bool any_kind = false;
            size_t ki = 0;
            while (kinds_buf[ki] != '\0') {
                while (kinds_buf[ki] == ' ') {
                    ki++;
                }
                if (kinds_buf[ki] == '\0') {
                    break;
                }
                size_t kstart = ki;
                while (kinds_buf[ki] != '\0' && kinds_buf[ki] != ' ') {
                    ki++;
                }
                size_t ktoklen = ki - kstart;
                char token[32];
                if (ktoklen == 0 || ktoklen >= sizeof(token)) {
                    out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
                    return;
                }
                memcpy(token, kinds_buf + kstart, ktoklen);
                token[ktoklen] = '\0';
                atlas_decision_kind kind;
                if (!atlas_decision_kind_parse(token, &kind)) {
                    /* An unknown kind name. Fails closed the same way an
                     * unknown scope name or an unknown top-level key does. */
                    out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
                    return;
                }
                uint32_t bit = ATLAS_DECISION_KIND_BIT(kind);
                if ((kmask & bit) != 0u) {
                    /* Named twice. Not a wider grant than naming it once and
                     * not a narrower one either — refused, because a policy
                     * an operator reads back should never carry a name twice
                     * for no reason nothing here can explain. */
                    out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
                    return;
                }
                kmask |= bit;
                any_kind = true;
            }
            if (!any_kind) {
                /* "An empty list is refused -- disposal of nothing is not a
                 * smaller grant, it is a key that cannot take effect." In
                 * practice `take_value` already refuses a value that trims to
                 * nothing before this branch is ever reached, by falling
                 * through to the "unrecognised key" refusal below — but that
                 * is a property of how whitespace happens to be trimmed
                 * elsewhere in this file, not a property of this key, so the
                 * refusal is written here explicitly rather than left to be
                 * true by accident. */
                out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
                return;
            }
            out->remote_dispose_kinds = kmask;
            dispose_kinds_given = true;
        } else if (take_value(line, len, "operator_accepts_cleartext_disposal", &val, &vlen)) {
            /* A16, amended 2026-09-04. One legal value, deliberately narrower
             * than `parse_bool`: this key records a person's written
             * acceptance of a stated risk, not an on/off switch, and the
             * frozen instruction is "leave the line out rather than writing
             * `no`" — so `no`, `true`, `1` and every other spelling are
             * refused exactly like an unrecognised key, rather than read as
             * "not accepted". */
            if (vlen != 3u || strncmp(val, "yes", 3) != 0) {
                out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
                return;
            }
            out->cleartext_disposal_accepted = true;
            accept_given = true;
        } else if (take_value(line, len, "trust_forwarded_for", &val, &vlen)) {
            if (!parse_bool(val, vlen, &out->trust_forwarded_for)) {
                out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
                return;
            }
        } else if (take_value(line, len, "max_request_bytes", &val, &vlen)) {
            if (!parse_number(val, vlen, &num) || num <= 0 ||
                num > (long long)ATLAS_GW_MAX_BODY_BYTES) {
                /* A policy may lower a compiled-in ceiling and may never raise
                 * one. Refused rather than clamped: a discarded number nobody
                 * is told about produces a gateway unlike the one configured. */
                out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
                return;
            }
            out->max_request_bytes = num;
        } else if (take_value(line, len, "max_concurrent", &val, &vlen)) {
            if (!parse_number(val, vlen, &num) || num <= 0 || num > ATLAS_GW_MAX_CONNECTIONS) {
                out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
                return;
            }
            out->max_concurrent = num;
        } else if (take_value(line, len, "rate_limit_per_minute", &val, &vlen)) {
            if (!parse_number(val, vlen, &num) || num <= 0 || num > ATLAS_GW_MAX_RATE_PER_MINUTE) {
                out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
                return;
            }
            out->rate_limit_per_minute = num;
        } else if (take_value(line, len, "session_ttl_seconds", &val, &vlen)) {
            if (!parse_number(val, vlen, &num) || num <= 0 ||
                num > ATLAS_GW_MAX_SESSION_TTL_SECONDS) {
                out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
                return;
            }
            out->session_ttl_seconds = num;
        } else {
            /* An unrecognised key is an error, not something skipped. A policy
             * Atlas half-understands is one whose author believes they
             * configured something Atlas never read — and here that something
             * is very likely to have been a restriction. */
            out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
            return;
        }
    }

    if (!have_enabled || !enabled) {
        /* Present and switched off is a complete, valid policy that says no.
         * It is not malformed, and reporting it as such would send an operator
         * looking for a syntax error. */
        out->reason = have_enabled ? ATLAS_GWPOLICY_REASON_DISABLED
                                   : ATLAS_GWPOLICY_REASON_MALFORMED;
        return;
    }
    if (out->gateway_uid <= 0) {
        /* Without it the daemon cannot recognise the gateway, so the gateway
         * could authenticate nobody. Refused rather than started in a state
         * where every request fails for a reason nothing explains. */
        out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
        return;
    }
    if (addr_given && !atlas_gwpolicy_is_loopback(out->listen_addr) &&
        out->tls_mode == ATLAS_GWPOLICY_TLS_UNSET) {
        /* The whole of A9's "secure by default" claim. Binding beyond loopback
         * requires the operator to have written down how transport security is
         * provided — even if the answer is `NONE`, which is then a decision
         * somebody made and an auditor can find, rather than the silent
         * consequence of leaving a key out. */
        out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
        return;
    }
    if (dispose_key_given != dispose_kinds_given) {
        /* A16. "Both keys or neither." Disposal of nothing is not a smaller
         * grant than naming no key at all -- it is a key that can never take
         * effect -- and a credential named with no kinds to spend it on is
         * exactly as inert. Neither half is a documented behaviour Atlas
         * would actually implement, so both are refused rather than one of
         * them silently doing nothing. */
        out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
        return;
    }
    if (accept_given && !dispose_key_given) {
        /* A16, amended 2026-09-04. This key records a person's acceptance of
         * a risk carried by a specific credential; with no disposal
         * credential named there is nothing here for the operator to have
         * accepted. */
        out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
        return;
    }
    if (dispose_key_given) {
        if (!out->web_gui) {
            /* A16. The panel this credential serves lives in the browser
             * surface; naming a disposal credential with that surface off is
             * P0's rule again -- a documented bound that is not the
             * implemented one is worse than no bound, so it is refused
             * rather than left silently inert, exactly as
             * `web_gui_anonymous_scopes` already is above. */
            out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
            return;
        }
        if (out->tls_mode == ATLAS_GWPOLICY_TLS_REVERSE_PROXY) {
            if (accept_given) {
                /* A16, amended 2026-09-04. TLS is already in front, so there
                 * is no cleartext risk for this line to accept; present
                 * anyway, it is refused rather than silently ignored -- an
                 * operator-written line this loader quietly did nothing with
                 * is exactly the shape "an unrecognised key is an error, not
                 * something skipped" exists to catch, one layer in. */
                out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
                return;
            }
        } else if (!accept_given) {
            /* A16, amended 2026-09-04. `tls_mode` is `NONE`, or was never
             * written at all (a loopback bind) -- either way nothing in
             * front of this listener terminates TLS, and the disposal
             * credential is a bearer token presented on every request this
             * group answers. Offering the daemon method group here without
             * the operator's written acceptance would be the authentication
             * bypass `acbd7ad`'s review found in a weaker mechanism, except
             * this one disposes of a knowledge record rather than only
             * reading one -- so it is refused here, at the policy that would
             * otherwise offer the group, rather than left to be caught later. */
            out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
            return;
        }
    }
    if (!out->remote_mcp && !out->web_gui) {
        /* A listener with no surface accepts connections and answers 404 to
         * everything. Refused: enabling the gateway is one decision and
         * exposing a surface is another, and a policy that made neither has
         * asked for a port to be open for no reason. */
        out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
        return;
    }
    if (anon_scopes_given && !out->web_gui) {
        /* `/api/` is reachable whether or not `web_gui` is on — a bearer
         * token reaches it either way — so this key would silently never take
         * effect with the browser surface off. A documented bound that is not
         * the implemented bound is worse than no bound: refused, not merely
         * inert. */
        out->reason = ATLAS_GWPOLICY_REASON_MALFORMED;
        return;
    }

    out->reason = ATLAS_GWPOLICY_REASON_ACTIVE;
    out->state = ATLAS_GWPOLICY_ENABLED;
}

void atlas_gwpolicy_load(atlas_gwpolicy *out) {
    atlas_gwpolicy_load_at(ATLAS_GWPOLICY_PATH, out);
}
