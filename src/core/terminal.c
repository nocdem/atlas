/* Atlas - the operator-only interactive channel.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See atlas/terminal.h for what this establishes and, more importantly, for
 * what it does not.
 */
#define _GNU_SOURCE 1

#include "atlas/terminal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "atlas/atlas.h"

struct atlas_terminal {
    int fd;
};

bool atlas_terminal_available(void) {
    /* Both ends. Requiring only the read side would let output redirection hide
     * the prompt while the answer was still accepted, and a confirmation nobody
     * saw is not one. */
    return isatty(STDIN_FILENO) == 1 && isatty(STDOUT_FILENO) == 1;
}

atlas_status atlas_terminal_open(atlas_terminal **out, atlas_err *err) {
    *out = NULL;
    if (!atlas_terminal_available()) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "this command needs an interactive terminal on both standard input "
                             "and standard output. Atlas will not accept an approval from a pipe, "
                             "a file, an environment variable or --yes.");
    }
    /* `/dev/tty` rather than standard input, and O_NOCTTY so that opening it
     * cannot make it this process's controlling terminal — the point is to
     * reach the terminal a person is already at, never to acquire one. */
    int fd = open("/dev/tty", O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "this process has no controlling terminal (/dev/tty: %s), so there is "
                             "nobody to confirm anything",
                             strerror(errno));
    }
    if (isatty(fd) != 1) {
        (void)close(fd);
        return atlas_err_set(err, ATLAS_ERR_USAGE, "/dev/tty is not a terminal on this system");
    }
    atlas_terminal *t = calloc(1u, sizeof(*t));
    if (t == NULL) {
        (void)close(fd);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory opening the terminal");
    }
    t->fd = fd;
    *out = t;
    return ATLAS_OK;
}

void atlas_terminal_close(atlas_terminal *t) {
    if (t == NULL) {
        return;
    }
    if (t->fd >= 0) {
        (void)close(t->fd);
    }
    free(t);
}

bool atlas_terminal_byte_ok(unsigned char c) {
    /* Printable ASCII, space and newline.
     *
     * Not a superset of what `atlas_safe()` produces by accident: this is the
     * second, independent check on the one output path where a terminal escape
     * would be actively harmful. `atlas_safe` already encodes controls, C1,
     * bidi overrides and invalid UTF-8 — and it encodes them *into* printable
     * ASCII, so correctly encoded text passes here unchanged. Text that somehow
     * reached this function unencoded does not. */
    return c == '\n' || (c >= 0x20u && c < 0x7Fu);
}

atlas_status atlas_terminal_write(atlas_terminal *t, const char *text, size_t len,
                                  atlas_err *err) {
    /* Scanned before anything is written, and a violating byte replaced rather
     * than escaped.
     *
     * Replaced, because this is a display of last resort: if a byte got this
     * far unencoded then the caller's encoding is broken, and the right
     * response is a visible `?` in the approval prompt — which somebody will
     * notice and report — rather than a plausible-looking prompt with an
     * invisible cursor movement in it. */
    for (size_t off = 0; off < len;) {
        char chunk[512];
        size_t n = 0;
        while (n < sizeof(chunk) && off < len) {
            unsigned char c = (unsigned char)text[off++];
            chunk[n++] = atlas_terminal_byte_ok(c) ? (char)c : '?';
        }
        size_t written = 0;
        while (written < n) {
            ssize_t w = write(t->fd, chunk + written, n - written);
            if (w < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return atlas_err_set(err, ATLAS_ERR_INTERNAL, "cannot write to the terminal: %s",
                                     strerror(errno));
            }
            written += (size_t)w;
        }
    }
    return ATLAS_OK;
}

atlas_status atlas_terminal_writef(atlas_terminal *t, atlas_err *err, const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "cannot format terminal output");
    }
    size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1u;
    return atlas_terminal_write(t, buf, len, err);
}

atlas_status atlas_terminal_read_line(atlas_terminal *t, atlas_buf *out, size_t max,
                                      atlas_err *err) {
    atlas_buf_reset(out);
    /* One byte at a time. A buffered read would consume bytes past the newline,
     * and this file descriptor is the operator's terminal — swallowing what
     * they type next would be a real bug rather than an inefficiency. The lines
     * involved are a handful of characters. */
    for (;;) {
        char c = 0;
        ssize_t n = read(t->fd, &c, 1u);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "cannot read from the terminal: %s",
                                 strerror(errno));
        }
        if (n == 0) {
            /* End of input on a terminal: the operator pressed Ctrl-D or the
             * terminal went away. An empty answer is not a confirmation. */
            break;
        }
        if (c == '\n' || c == '\r') {
            break;
        }
        if (out->len >= max) {
            /* An error, not a truncation. A truncated confirmation that
             * happened to match the expected prefix would be an approval the
             * operator did not give. */
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "that confirmation is longer than %zu bytes; nothing was changed",
                                 max);
        }
        atlas_status st = atlas_buf_append_ch(out, c, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    /* Trim surrounding whitespace, and nothing else. The result is compared
     * byte for byte against an Atlas-generated hex string; it never reaches a
     * parser, so there is nothing else to normalise. */
    size_t start = 0;
    while (start < out->len && (out->data[start] == ' ' || out->data[start] == '\t')) {
        start++;
    }
    size_t end = out->len;
    while (end > start && (out->data[end - 1u] == ' ' || out->data[end - 1u] == '\t')) {
        end--;
    }
    if (start > 0 || end < out->len) {
        atlas_buf trimmed = ATLAS_BUF_INIT;
        atlas_status st = atlas_buf_set(&trimmed, out->data + start, end - start, err);
        if (st != ATLAS_OK) {
            atlas_buf_free(&trimmed);
            return st;
        }
        atlas_buf_free(out);
        *out = trimmed;
    }
    return ATLAS_OK;
}
