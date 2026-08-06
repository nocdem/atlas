/* Atlas - standalone JSON checker for shell-driven verification.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * This exists so that verifying Atlas' JSON output needs no language runtime.
 * The build and test workflow depends on nothing beyond the C toolchain, SQLite
 * and Git, and that must hold for the verification scripts too.
 *
 * It reads a document from stdin and validates it with the same independent
 * parser the test suite uses, deliberately not with Atlas' writer.
 *
 *   atlas-jsoncheck                        validate stdin, exit 0 or 1
 *   atlas-jsoncheck --get KEY              print the string value of KEY
 *   atlas-jsoncheck --raw KEY              print the raw token after KEY
 *   atlas-jsoncheck --expect KEY=VALUE     require KEY to be the string VALUE
 *   atlas-jsoncheck --expect-raw KEY=TOKEN require KEY to be the token TOKEN
 *   atlas-jsoncheck --no-control           require no C0/C1 control bytes
 *
 * Checks combine; every one must pass. Diagnostics go to stderr.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/buf.h"
#include "atlas/safetext.h"
#include "support/jsoncheck.h"

#define MAX_INPUT (64u * 1024u * 1024u)

/* Reports a usage or lookup problem. Formats are literal at every call site so
 * the compiler can keep checking them. */
static int fail2(const char *what, const char *a, const char *b) {
    (void)fprintf(stderr, "atlas-jsoncheck: %s: %s%s%s\n", what, a != NULL ? a : "",
                  b != NULL ? " " : "", b != NULL ? b : "");
    return 1;
}

static int read_stdin(atlas_buf *out) {
    atlas_err err;
    atlas_err_init(&err);
    char chunk[65536];
    for (;;) {
        size_t n = fread(chunk, 1u, sizeof(chunk), stdin);
        if (n > 0) {
            if (out->len + n > MAX_INPUT) {
                (void)fprintf(stderr, "atlas-jsoncheck: input exceeds %u bytes\n", MAX_INPUT);
                return 1;
            }
            if (atlas_buf_append(out, chunk, n, &err) != ATLAS_OK) {
                (void)fprintf(stderr, "atlas-jsoncheck: %s\n", atlas_err_msg(&err));
                return 1;
            }
        }
        if (n < sizeof(chunk)) {
            if (ferror(stdin)) {
                (void)fprintf(stderr, "atlas-jsoncheck: read error\n");
                return 1;
            }
            break;
        }
    }
    return 0;
}

/* Rejects any byte a terminal would interpret, mirroring the test assertion. */
static int check_no_control(const atlas_buf *doc) {
    for (size_t i = 0; i < doc->len; i++) {
        unsigned char c = (unsigned char)doc->data[i];
        if (c == '\n' || c == '\t') {
            continue; /* document structure, not repository content */
        }
        if (c < 0x20u || c == 0x7fu) {
            (void)fprintf(stderr,
                          "atlas-jsoncheck: control byte 0x%02x at offset %zu\n", c, i);
            return 1;
        }
        if (c == 0xc2u && i + 1u < doc->len) {
            unsigned char next = (unsigned char)doc->data[i + 1u];
            if (next >= 0x80u && next <= 0x9fu) {
                (void)fprintf(stderr, "atlas-jsoncheck: C1 control U+00%02X at offset %zu\n", next,
                              i);
                return 1;
            }
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    atlas_buf doc = ATLAS_BUF_INIT;
    int rc = read_stdin(&doc);
    if (rc != 0) {
        atlas_buf_free(&doc);
        return rc;
    }

    size_t bad = 0;
    if (!tjson_valid(doc.data, doc.len, &bad)) {
        (void)fprintf(stderr, "atlas-jsoncheck: invalid JSON at byte offset %zu\n", bad);
        size_t from = bad > 40u ? bad - 40u : 0u;
        size_t len = doc.len - from;
        if (len > 80u) {
            len = 80u;
        }
        (void)fprintf(stderr, "  near: %.*s\n", (int)len, doc.data + from);
        atlas_buf_free(&doc);
        return 1;
    }

    int status = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--no-control") == 0) {
            if (check_no_control(&doc) != 0) {
                status = 1;
            }
            continue;
        }
        bool want_raw = (strcmp(a, "--raw") == 0 || strcmp(a, "--expect-raw") == 0);
        bool is_expect = (strcmp(a, "--expect") == 0 || strcmp(a, "--expect-raw") == 0);
        bool is_get = (strcmp(a, "--get") == 0 || strcmp(a, "--raw") == 0);
        if (!is_expect && !is_get) {
            status = fail2("unknown option", a, NULL);
            break;
        }
        if (i + 1 >= argc) {
            status = fail2("option needs an argument", a, NULL);
            break;
        }
        const char *operand = argv[++i];

        atlas_buf key = ATLAS_BUF_INIT;
        const char *expected = NULL;
        atlas_err err;
        atlas_err_init(&err);
        if (is_expect) {
            const char *eq = strchr(operand, '=');
            if (eq == NULL) {
                status = fail2("expects KEY=VALUE", a, operand);
                atlas_buf_free(&key);
                break;
            }
            if (atlas_buf_append(&key, operand, (size_t)(eq - operand), &err) != ATLAS_OK) {
                status = 1;
                atlas_buf_free(&key);
                break;
            }
            expected = eq + 1;
        } else if (atlas_buf_append_str(&key, operand, &err) != ATLAS_OK) {
            status = 1;
            atlas_buf_free(&key);
            break;
        }

        atlas_buf value = ATLAS_BUF_INIT;
        bool found = want_raw
                         ? tjson_get_raw(doc.data, doc.len, atlas_buf_cstr(&key), &value)
                         : tjson_get_string(doc.data, doc.len, atlas_buf_cstr(&key), &value);
        if (!found) {
            status = fail2("key is missing or not of the expected type",
                           atlas_buf_cstr(&key), NULL);
        } else if (is_expect) {
            if (strcmp(atlas_buf_cstr(&value), expected) != 0) {
                (void)fprintf(stderr, "atlas-jsoncheck: %s: expected \"%s\", got \"%s\"\n",
                              atlas_buf_cstr(&key), expected, atlas_buf_cstr(&value));
                status = 1;
            }
        } else {
            (void)printf("%s\n", atlas_buf_cstr(&value));
        }
        atlas_buf_free(&key);
        atlas_buf_free(&value);
    }

    atlas_buf_free(&doc);
    return status;
}
