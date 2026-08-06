/* Atlas - minimal independent JSON validator for tests.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 */
#include "support/jsoncheck.h"

#include <stdint.h>
#include <string.h>

typedef struct scanner {
    const char *s;
    size_t n;
    size_t i;
    bool ok;
} scanner;

static void skip_ws(scanner *sc) {
    while (sc->i < sc->n) {
        char c = sc->s[sc->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            sc->i++;
        } else {
            break;
        }
    }
}

static bool at(scanner *sc, char c) {
    return sc->i < sc->n && sc->s[sc->i] == c;
}

static bool eat(scanner *sc, char c) {
    if (at(sc, c)) {
        sc->i++;
        return true;
    }
    return false;
}

static bool is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool scan_value(scanner *sc);

static bool scan_string(scanner *sc) {
    if (!eat(sc, '"')) {
        return false;
    }
    while (sc->i < sc->n) {
        unsigned char c = (unsigned char)sc->s[sc->i];
        if (c == '"') {
            sc->i++;
            return true;
        }
        if (c == '\\') {
            sc->i++;
            if (sc->i >= sc->n) {
                return false;
            }
            char e = sc->s[sc->i++];
            switch (e) {
            case '"':
            case '\\':
            case '/':
            case 'b':
            case 'f':
            case 'n':
            case 'r':
            case 't':
                break;
            case 'u':
                for (int k = 0; k < 4; k++) {
                    if (sc->i >= sc->n || !is_hex(sc->s[sc->i])) {
                        return false;
                    }
                    sc->i++;
                }
                break;
            default:
                return false;
            }
            continue;
        }
        /* RFC 8259 forbids unescaped control characters in strings. */
        if (c < 0x20u) {
            return false;
        }
        sc->i++;
    }
    return false;
}

static bool scan_number(scanner *sc) {
    size_t start = sc->i;
    (void)eat(sc, '-');
    if (eat(sc, '0')) {
        /* leading zero must not be followed by another digit */
        if (sc->i < sc->n && sc->s[sc->i] >= '0' && sc->s[sc->i] <= '9') {
            return false;
        }
    } else {
        size_t digits = 0;
        while (sc->i < sc->n && sc->s[sc->i] >= '0' && sc->s[sc->i] <= '9') {
            sc->i++;
            digits++;
        }
        if (digits == 0) {
            return false;
        }
    }
    if (eat(sc, '.')) {
        size_t digits = 0;
        while (sc->i < sc->n && sc->s[sc->i] >= '0' && sc->s[sc->i] <= '9') {
            sc->i++;
            digits++;
        }
        if (digits == 0) {
            return false;
        }
    }
    if (sc->i < sc->n && (sc->s[sc->i] == 'e' || sc->s[sc->i] == 'E')) {
        sc->i++;
        (void)(eat(sc, '+') || eat(sc, '-'));
        size_t digits = 0;
        while (sc->i < sc->n && sc->s[sc->i] >= '0' && sc->s[sc->i] <= '9') {
            sc->i++;
            digits++;
        }
        if (digits == 0) {
            return false;
        }
    }
    return sc->i > start;
}

static bool scan_literal(scanner *sc, const char *lit) {
    size_t len = strlen(lit);
    if (sc->n - sc->i < len || memcmp(sc->s + sc->i, lit, len) != 0) {
        return false;
    }
    sc->i += len;
    return true;
}

static bool scan_value(scanner *sc) {
    skip_ws(sc);
    if (sc->i >= sc->n) {
        return false;
    }
    char c = sc->s[sc->i];
    switch (c) {
    case '{': {
        sc->i++;
        skip_ws(sc);
        if (eat(sc, '}')) {
            return true;
        }
        for (;;) {
            skip_ws(sc);
            if (!scan_string(sc)) {
                return false;
            }
            skip_ws(sc);
            if (!eat(sc, ':')) {
                return false;
            }
            if (!scan_value(sc)) {
                return false;
            }
            skip_ws(sc);
            if (eat(sc, ',')) {
                continue;
            }
            return eat(sc, '}');
        }
    }
    case '[': {
        sc->i++;
        skip_ws(sc);
        if (eat(sc, ']')) {
            return true;
        }
        for (;;) {
            if (!scan_value(sc)) {
                return false;
            }
            skip_ws(sc);
            if (eat(sc, ',')) {
                continue;
            }
            return eat(sc, ']');
        }
    }
    case '"':
        return scan_string(sc);
    case 't':
        return scan_literal(sc, "true");
    case 'f':
        return scan_literal(sc, "false");
    case 'n':
        return scan_literal(sc, "null");
    default:
        return scan_number(sc);
    }
}

bool tjson_valid(const char *s, size_t n, size_t *err_pos) {
    scanner sc = {s, n, 0, true};
    bool good = scan_value(&sc);
    if (good) {
        skip_ws(&sc);
        good = (sc.i == sc.n);
    }
    if (!good && err_pos != NULL) {
        *err_pos = sc.i;
    }
    return good;
}

/* --- key lookup ---------------------------------------------------------- */

/* Locates `"key":` and returns the offset just past the colon. The search is
 * string-aware so a key name appearing inside a value is not matched. */
static bool find_key(const char *s, size_t n, const char *key, size_t *out) {
    size_t klen = strlen(key);
    size_t i = 0;
    while (i < n) {
        if (s[i] == '"') {
            size_t start = i + 1u;
            scanner sc = {s, n, i, true};
            if (!scan_string(&sc)) {
                return false;
            }
            size_t end = sc.i - 1u; /* closing quote */
            size_t len = end - start;
            size_t after = sc.i;
            while (after < n && (s[after] == ' ' || s[after] == '\t')) {
                after++;
            }
            if (len == klen && memcmp(s + start, key, klen) == 0 && after < n && s[after] == ':') {
                *out = after + 1u;
                return true;
            }
            i = sc.i;
            continue;
        }
        i++;
    }
    return false;
}

static bool append_utf8(atlas_buf *out, uint32_t cp, atlas_err *err) {
    char tmp[4];
    size_t n;
    if (cp < 0x80u) {
        tmp[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800u) {
        tmp[0] = (char)(0xc0u | (cp >> 6));
        tmp[1] = (char)(0x80u | (cp & 0x3fu));
        n = 2;
    } else if (cp < 0x10000u) {
        tmp[0] = (char)(0xe0u | (cp >> 12));
        tmp[1] = (char)(0x80u | ((cp >> 6) & 0x3fu));
        tmp[2] = (char)(0x80u | (cp & 0x3fu));
        n = 3;
    } else {
        tmp[0] = (char)(0xf0u | (cp >> 18));
        tmp[1] = (char)(0x80u | ((cp >> 12) & 0x3fu));
        tmp[2] = (char)(0x80u | ((cp >> 6) & 0x3fu));
        tmp[3] = (char)(0x80u | (cp & 0x3fu));
        n = 4;
    }
    return atlas_buf_append(out, tmp, n, err) == ATLAS_OK;
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

bool tjson_get_string(const char *s, size_t n, const char *key, atlas_buf *out) {
    size_t pos = 0;
    if (!find_key(s, n, key, &pos)) {
        return false;
    }
    while (pos < n && (s[pos] == ' ' || s[pos] == '\t')) {
        pos++;
    }
    if (pos >= n || s[pos] != '"') {
        return false;
    }
    pos++;
    atlas_buf_reset(out);
    atlas_err err;
    atlas_err_init(&err);
    while (pos < n) {
        char c = s[pos];
        if (c == '"') {
            return true;
        }
        if (c == '\\') {
            pos++;
            if (pos >= n) {
                return false;
            }
            char e = s[pos++];
            char lit = 0;
            switch (e) {
            case '"': lit = '"'; break;
            case '\\': lit = '\\'; break;
            case '/': lit = '/'; break;
            case 'b': lit = '\b'; break;
            case 'f': lit = '\f'; break;
            case 'n': lit = '\n'; break;
            case 'r': lit = '\r'; break;
            case 't': lit = '\t'; break;
            case 'u': {
                if (pos + 4u > n) {
                    return false;
                }
                uint32_t cp = 0;
                for (int k = 0; k < 4; k++) {
                    int h = hexval(s[pos + (size_t)k]);
                    if (h < 0) {
                        return false;
                    }
                    cp = cp * 16u + (uint32_t)h;
                }
                pos += 4u;
                if (!append_utf8(out, cp, &err)) {
                    return false;
                }
                continue;
            }
            default:
                return false;
            }
            if (atlas_buf_append(out, &lit, 1u, &err) != ATLAS_OK) {
                return false;
            }
            continue;
        }
        if (atlas_buf_append(out, &c, 1u, &err) != ATLAS_OK) {
            return false;
        }
        pos++;
    }
    return false;
}

bool tjson_get_raw(const char *s, size_t n, const char *key, atlas_buf *out) {
    size_t pos = 0;
    if (!find_key(s, n, key, &pos)) {
        return false;
    }
    scanner sc = {s, n, pos, true};
    skip_ws(&sc);
    size_t start = sc.i;
    if (!scan_value(&sc)) {
        return false;
    }
    atlas_buf_reset(out);
    atlas_err err;
    atlas_err_init(&err);
    return atlas_buf_append(out, s + start, sc.i - start, &err) == ATLAS_OK;
}
