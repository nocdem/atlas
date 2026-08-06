/* Atlas - safe encoding of untrusted text.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 */
#include "atlas/safetext.h"

#include <string.h>

static const char HEX[] = "0123456789ABCDEF";
static const char SAFE_PLACEHOLDER[] = "%3F"; /* an escaped '?' when encoding fails */

size_t atlas_utf8_decode(const unsigned char *p, size_t avail, uint32_t *cp_out) {
    if (avail == 0) {
        return 0;
    }
    unsigned char c = p[0];
    size_t need;
    uint32_t cp;
    if (c < 0x80u) {
        *cp_out = c;
        return 1;
    }
    if ((c & 0xe0u) == 0xc0u) {
        need = 2;
        cp = (uint32_t)(c & 0x1fu);
    } else if ((c & 0xf0u) == 0xe0u) {
        need = 3;
        cp = (uint32_t)(c & 0x0fu);
    } else if ((c & 0xf8u) == 0xf0u) {
        need = 4;
        cp = (uint32_t)(c & 0x07u);
    } else {
        return 0;
    }
    if (avail < need) {
        return 0;
    }
    for (size_t i = 1; i < need; i++) {
        if ((p[i] & 0xc0u) != 0x80u) {
            return 0;
        }
        cp = (cp << 6) | (uint32_t)(p[i] & 0x3fu);
    }
    if ((need == 2 && cp < 0x80u) || (need == 3 && cp < 0x800u) ||
        (need == 4 && cp < 0x10000u)) {
        return 0; /* overlong */
    }
    if (cp > 0x10ffffu || (cp >= 0xd800u && cp <= 0xdfffu)) {
        return 0;
    }
    *cp_out = cp;
    return need;
}

bool atlas_codepoint_is_unsafe(uint32_t cp) {
    /* C0 controls and DEL: ESC, BEL, CR, LF, TAB and friends. */
    if (cp < 0x20u || cp == 0x7fu) {
        return true;
    }
    /* C1 controls. U+009B is a single-byte CSI on some terminals. */
    if (cp >= 0x80u && cp <= 0x9fu) {
        return true;
    }
    /* Line and paragraph separators can break line-oriented output. */
    if (cp == 0x2028u || cp == 0x2029u) {
        return true;
    }
    /* Bidirectional controls: these make displayed text read differently from
     * the bytes stored, which is exactly the trick behind Trojan Source. */
    if (cp == 0x200eu || cp == 0x200fu) {
        return true;
    }
    if (cp >= 0x202au && cp <= 0x202eu) {
        return true;
    }
    if (cp >= 0x2066u && cp <= 0x2069u) {
        return true;
    }
    return false;
}

static atlas_status escape_bytes(const unsigned char *p, size_t n, atlas_buf *out, atlas_err *err) {
    for (size_t i = 0; i < n; i++) {
        char esc[3] = {'%', HEX[(p[i] >> 4) & 0x0fu], HEX[p[i] & 0x0fu]};
        atlas_status st = atlas_buf_append(out, esc, sizeof(esc), err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    return ATLAS_OK;
}

atlas_status atlas_text_encode_safe(const void *raw, size_t n, atlas_buf *out, atlas_err *err) {
    const unsigned char *p = (const unsigned char *)raw;
    atlas_status st = atlas_buf_reserve(out, n, err);
    if (st != ATLAS_OK) {
        return st;
    }
    size_t i = 0;
    while (i < n) {
        if (p[i] == '%') {
            /* Escaped so the transform is reversible. */
            st = escape_bytes(p + i, 1u, out, err);
            i++;
        } else {
            uint32_t cp = 0;
            size_t seq = atlas_utf8_decode(p + i, n - i, &cp);
            if (seq == 0) {
                /* Not valid UTF-8: escape exactly one byte and resynchronise, so
                 * the position of the bad byte is preserved. */
                st = escape_bytes(p + i, 1u, out, err);
                i++;
            } else if (atlas_codepoint_is_unsafe(cp)) {
                st = escape_bytes(p + i, seq, out, err);
                i += seq;
            } else {
                st = atlas_buf_append(out, p + i, seq, err);
                i += seq;
            }
        }
        if (st != ATLAS_OK) {
            return st;
        }
    }
    /* Guarantee a non-NULL, NUL-terminated result even for empty input. */
    return atlas_buf_append(out, "", 0, err);
}

static int hex_val(unsigned char c) {
    if (c >= '0' && c <= '9') {
        return (int)(c - '0');
    }
    if (c >= 'A' && c <= 'F') {
        return (int)(c - 'A') + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return (int)(c - 'a') + 10;
    }
    return -1;
}

atlas_status atlas_text_decode_safe(const char *text, size_t n, atlas_buf *out, atlas_err *err) {
    const unsigned char *p = (const unsigned char *)text;
    atlas_status st = atlas_buf_reserve(out, n, err);
    if (st != ATLAS_OK) {
        return st;
    }
    size_t i = 0;
    while (i < n) {
        if (p[i] == '%') {
            if (i + 2u >= n) {
                return atlas_err_set(err, ATLAS_ERR_USAGE,
                                     "truncated %% escape at offset %zu", i);
            }
            int hi = hex_val(p[i + 1u]);
            int lo = hex_val(p[i + 2u]);
            if (hi < 0 || lo < 0) {
                return atlas_err_set(err, ATLAS_ERR_USAGE, "invalid %% escape at offset %zu", i);
            }
            char b = (char)((unsigned)hi * 16u + (unsigned)lo);
            st = atlas_buf_append(out, &b, 1u, err);
            i += 3u;
        } else {
            st = atlas_buf_append(out, p + i, 1u, err);
            i++;
        }
        if (st != ATLAS_OK) {
            return st;
        }
    }
    return atlas_buf_append(out, "", 0, err);
}

bool atlas_text_is_safe(const void *raw, size_t n) {
    const unsigned char *p = (const unsigned char *)raw;
    size_t i = 0;
    while (i < n) {
        if (p[i] == '%') {
            return false;
        }
        uint32_t cp = 0;
        size_t seq = atlas_utf8_decode(p + i, n - i, &cp);
        if (seq == 0 || atlas_codepoint_is_unsafe(cp)) {
            return false;
        }
        i += seq;
    }
    return true;
}

/* --- scratch pool -------------------------------------------------------- */

void atlas_safe_pool_init(atlas_safe_pool *p) {
    for (size_t i = 0; i < ATLAS_SAFE_POOL_SLOTS; i++) {
        atlas_buf_init(&p->slots[i]);
    }
    p->next = 0;
}

void atlas_safe_pool_free(atlas_safe_pool *p) {
    for (size_t i = 0; i < ATLAS_SAFE_POOL_SLOTS; i++) {
        atlas_buf_free(&p->slots[i]);
    }
    p->next = 0;
}

const char *atlas_safe_n(atlas_safe_pool *p, const void *raw, size_t n) {
    if (raw == NULL) {
        return "";
    }
    atlas_buf *slot = &p->slots[p->next];
    p->next = (p->next + 1u) % ATLAS_SAFE_POOL_SLOTS;
    atlas_buf_reset(slot);
    atlas_err err;
    atlas_err_init(&err);
    if (atlas_text_encode_safe(raw, n, slot, &err) != ATLAS_OK) {
        /* Only allocation can fail here; printing a marker beats printing raw
         * bytes that were never checked. */
        return SAFE_PLACEHOLDER;
    }
    return atlas_buf_cstr(slot);
}

const char *atlas_safe(atlas_safe_pool *p, const char *s) {
    if (s == NULL) {
        return "";
    }
    return atlas_safe_n(p, s, strlen(s));
}
