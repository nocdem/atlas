/* Atlas - A14R: the final record of a worker's stream.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See atlas/orch_jobresult.h for what this is and why it is not in `usage.c`.
 *
 * Deliberately not yyjson, for the reason `usage.c` gives: that dependency is
 * confined to the IPC boundary by `CLAUDE.md`, and a worker's stdout is not
 * that boundary. What is needed is narrower than parsing anyway — one named
 * string and one named value out of one line, refusing what it does not
 * recognise.
 */
#define _GNU_SOURCE 1

#include "atlas/orch_jobresult.h"

#include <string.h>

#include "atlas/orch.h"

static const char *const SUBTYPE_NAMES[] = {"UNKNOWN", "success", "error_max_budget_usd",
                                            "error_max_turns", "error_during_execution"};

const char *atlas_orch_result_subtype_name(atlas_orch_result_subtype s) {
    if ((size_t)s < sizeof SUBTYPE_NAMES / sizeof SUBTYPE_NAMES[0]) {
        return SUBTYPE_NAMES[s];
    }
    return "UNKNOWN";
}

void atlas_orch_final_init(atlas_orch_final *f) {
    memset(f, 0, sizeof(*f));
    atlas_buf_init(&f->text);
    f->subtype = ATLAS_ORCH_RESULT_SUBTYPE_UNKNOWN;
}

void atlas_orch_final_free(atlas_orch_final *f) {
    if (f == NULL) {
        return;
    }
    atlas_buf_free(&f->text);
}

/* --- bounded scanning ------------------------------------------------------ */

/* The first occurrence of `key` in `[p, end)`, or NULL. Same shape as
 * `usage.c`'s: `memmem` is not portable enough to rely on and the ranges here
 * are one line long. */
static const char *find_key(const char *p, const char *end, const char *key) {
    size_t klen = strlen(key);
    if (klen == 0 || (size_t)(end - p) < klen) {
        return NULL;
    }
    for (const char *q = p; q + klen <= end; q++) {
        if (memcmp(q, key, klen) == 0) {
            return q;
        }
    }
    return NULL;
}

/* --- the subtype ----------------------------------------------------------- */

/* Matched whole against the vocabulary, never prefix-matched: `success` must
 * not match a value that merely begins with it. */
static atlas_orch_result_subtype take_subtype(const char *p, const char *end) {
    static const char KEY[] = "\"subtype\":\"";
    const char *at = find_key(p, end, KEY);
    if (at == NULL) {
        return ATLAS_ORCH_RESULT_SUBTYPE_UNKNOWN;
    }
    at += sizeof KEY - 1u;
    const char *close = at;
    while (close < end && *close != '"') {
        close++;
    }
    if (close >= end) {
        return ATLAS_ORCH_RESULT_SUBTYPE_UNKNOWN;
    }
    size_t n = (size_t)(close - at);
    /* Index 0 is UNKNOWN's own display name and is not a value the CLI emits;
     * starting at 1 keeps it from ever being matched out of a stream. */
    for (size_t i = 1; i < sizeof SUBTYPE_NAMES / sizeof SUBTYPE_NAMES[0]; i++) {
        if (strlen(SUBTYPE_NAMES[i]) == n && memcmp(at, SUBTYPE_NAMES[i], n) == 0) {
            return (atlas_orch_result_subtype)i;
        }
    }
    return ATLAS_ORCH_RESULT_SUBTYPE_UNKNOWN;
}

/* --- the answer ------------------------------------------------------------ */

/* One UTF-8 encoding of one code point, appended to `out` when there is room
 * and counted either way. `stored` is false once the bound has been reached, so
 * the full length keeps being measured without the bytes being kept. */
static atlas_status emit_cp(atlas_buf *out, bool *stored, int64_t *full, uint32_t cp,
                            atlas_err *err) {
    unsigned char b[4];
    size_t n;
    if (cp < 0x80u) {
        b[0] = (unsigned char)cp;
        n = 1;
    } else if (cp < 0x800u) {
        b[0] = (unsigned char)(0xC0u | (cp >> 6));
        b[1] = (unsigned char)(0x80u | (cp & 0x3Fu));
        n = 2;
    } else if (cp < 0x10000u) {
        b[0] = (unsigned char)(0xE0u | (cp >> 12));
        b[1] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
        b[2] = (unsigned char)(0x80u | (cp & 0x3Fu));
        n = 3;
    } else {
        b[0] = (unsigned char)(0xF0u | (cp >> 18));
        b[1] = (unsigned char)(0x80u | ((cp >> 12) & 0x3Fu));
        b[2] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
        b[3] = (unsigned char)(0x80u | (cp & 0x3Fu));
        n = 4;
    }
    *full += (int64_t)n;
    if (!*stored) {
        return ATLAS_OK;
    }
    if (out->len + n > (size_t)ATLAS_ORCH_RESULT_TEXT_MAX) {
        /* The bound is on the stored prefix and it never splits a code point:
         * a truncated multi-byte sequence is invalid UTF-8, and producing
         * invalid UTF-8 on purpose is not a smaller answer, it is a worse one. */
        *stored = false;
        return ATLAS_OK;
    }
    return atlas_buf_append(out, b, n, err);
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Decodes the JSON string value of `"result":"…"` out of one line.
 *
 * Stops at the closing quote, at the end of the line, or at an escape it does
 * not recognise — and in the last two cases marks the answer truncated rather
 * than dropping it. A malformed tail is a reason to stop reading, never a
 * reason to discard what was already read: the prefix is what a reviewer would
 * be shown either way, and silently returning nothing is the failure mode this
 * season exists to end. */
static atlas_status take_text(const char *p, const char *end, const char *key, atlas_orch_final *out,
                              atlas_err *err) {
    const char *at = find_key(p, end, key);
    if (at == NULL) {
        return ATLAS_OK;
    }
    at += strlen(key);
    bool stored = true;
    atlas_status st = ATLAS_OK;
    while (st == ATLAS_OK && at < end) {
        unsigned char c = (unsigned char)*at;
        if (c == '"') {
            /* The whole value was read. */
            out->text_truncated = !stored;
            return ATLAS_OK;
        }
        if (c != '\\') {
            st = emit_cp(&out->text, &stored, &out->text_full_bytes, (uint32_t)c, err);
            at++;
            continue;
        }
        at++;
        if (at >= end) {
            break;
        }
        char e = *at++;
        uint32_t cp;
        switch (e) {
        case '"': cp = '"'; break;
        case '\\': cp = '\\'; break;
        case '/': cp = '/'; break;
        case 'b': cp = 0x08u; break;
        case 'f': cp = 0x0Cu; break;
        case 'n': cp = 0x0Au; break;
        case 'r': cp = 0x0Du; break;
        case 't': cp = 0x09u; break;
        case 'u': {
            if (end - at < 4) {
                out->text_truncated = true;
                return ATLAS_OK;
            }
            int h0 = hexval(at[0]), h1 = hexval(at[1]), h2 = hexval(at[2]), h3 = hexval(at[3]);
            if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) {
                out->text_truncated = true;
                return ATLAS_OK;
            }
            at += 4;
            cp = (uint32_t)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
            if (cp >= 0xD800u && cp <= 0xDBFFu && end - at >= 6 && at[0] == '\\' && at[1] == 'u') {
                int l0 = hexval(at[2]), l1 = hexval(at[3]), l2 = hexval(at[4]), l3 = hexval(at[5]);
                if (l0 >= 0 && l1 >= 0 && l2 >= 0 && l3 >= 0) {
                    uint32_t lo = (uint32_t)((l0 << 12) | (l1 << 8) | (l2 << 4) | l3);
                    if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                        at += 6;
                    }
                }
            }
            if (cp >= 0xD800u && cp <= 0xDFFFu) {
                /* An unpaired surrogate is not a code point. Substituted rather
                 * than refused: the replacement character is what a reader
                 * should see, and it keeps the byte count honest. */
                cp = 0xFFFDu;
            }
            break;
        }
        default:
            /* Not a JSON escape. Stop, keeping what was read. */
            out->text_truncated = true;
            return ATLAS_OK;
        }
        st = emit_cp(&out->text, &stored, &out->text_full_bytes, cp, err);
    }
    /* Ran off the end of the line without a closing quote. */
    if (st == ATLAS_OK) {
        out->text_truncated = true;
    }
    return st;
}

/* --- the scan -------------------------------------------------------------- */

atlas_status atlas_orch_final_from_stream(const char *stream, size_t len, atlas_orch_final *out,
                                          atlas_err *err) {
    atlas_orch_final_init(out);
    if (stream == NULL || len == 0) {
        return ATLAS_OK;
    }

    const char *best = NULL;
    size_t best_len = 0;
    const char *p = stream;
    const char *end = stream + len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *line_end = nl != NULL ? nl : end;
        size_t line_len = (size_t)(line_end - p);
        while (line_len > 0 && (p[line_len - 1u] == '\r' || p[line_len - 1u] == ' ')) {
            line_len--;
        }
        if (line_len > 2u && p[0] == '{' && p[line_len - 1u] == '}' &&
            find_key(p, p + line_len, "\"type\":\"result\"") != NULL) {
            best = p;
            best_len = line_len;
        }
        if (nl == NULL) {
            break;
        }
        p = nl + 1;
    }
    if (best == NULL) {
        return ATLAS_OK; /* not present, which is not "it said nothing" */
    }
    out->present = true;
    out->subtype = take_subtype(best, best + best_len);
    return take_text(best, best + best_len, "\"result\":\"", out, err);
}

/* Codex's documented compact JSONL envelope. Only top-level event prefixes
 * are recognised; text inside a message cannot announce a completed turn.
 * Like the Claude scanner this is bounded extraction, not a JSON validator.
 * The subprocess outcome and Atlas gates remain authoritative. */
atlas_status atlas_orch_final_from_codex_stream(const char *stream, size_t len,
                                                atlas_orch_final *out, atlas_err *err) {
    atlas_orch_final_init(out);
    if (stream == NULL) return ATLAS_OK;
    const char *p = stream, *end = stream + len;
    const char *answer = NULL;
    size_t answer_len = 0;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *stop = nl != NULL ? nl : end;
        while (stop > p && (stop[-1] == '\r' || stop[-1] == ' ')) stop--;
        size_t n = (size_t)(stop - p);
        if (n > 2u && p[0] == '{' && stop[-1] == '}') {
            static const char ITEM[] = "{\"type\":\"item.completed\",\"item\":{";
            static const char DONE[] = "{\"type\":\"turn.completed\",";
            static const char FAIL[] = "{\"type\":\"turn.failed\",";
            if (n >= sizeof ITEM - 1u && memcmp(p, ITEM, sizeof ITEM - 1u) == 0 &&
                find_key(p, stop, "\"type\":\"agent_message\"") != NULL) {
                answer = p;
                answer_len = n;
            } else if (n >= sizeof DONE - 1u && memcmp(p, DONE, sizeof DONE - 1u) == 0) {
                out->present = true;
                out->subtype = ATLAS_ORCH_RESULT_SUBTYPE_SUCCESS;
            } else if (n >= sizeof FAIL - 1u && memcmp(p, FAIL, sizeof FAIL - 1u) == 0) {
                out->present = true;
                out->subtype = ATLAS_ORCH_RESULT_SUBTYPE_ERROR_DURING_EXECUTION;
            } else if (n == sizeof "{\"type\":\"turn.started\"}" - 1u &&
                       memcmp(p, "{\"type\":\"turn.started\"}", n) == 0) {
                out->present = false;
                out->subtype = ATLAS_ORCH_RESULT_SUBTYPE_UNKNOWN;
                answer = NULL;
            }
        }
        p = nl != NULL ? nl + 1 : end;
    }
    if (answer != NULL) {
        return take_text(answer, answer + answer_len, "\"text\":\"", out, err);
    }
    return ATLAS_OK;
}
