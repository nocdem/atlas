/* Atlas - IPC frame codec and bounded frame I/O.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Everything here is written so that a hostile or merely broken peer costs a
 * bounded amount of memory and a bounded amount of time, and never causes a
 * partial frame to be interpreted as a whole one.
 */
#define _GNU_SOURCE 1

#include "atlas/ipc.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* --- header -------------------------------------------------------------- */

void atlas_ipc_header_encode(unsigned char *out, uint32_t payload_len) {
    memcpy(out, ATLAS_IPC_MAGIC, 4u);
    out[4] = (unsigned char)((ATLAS_IPC_PROTOCOL_VERSION >> 8) & 0xffu);
    out[5] = (unsigned char)(ATLAS_IPC_PROTOCOL_VERSION & 0xffu);
    out[6] = 0u; /* flags: reserved */
    out[7] = 0u;
    out[8] = (unsigned char)((payload_len >> 24) & 0xffu);
    out[9] = (unsigned char)((payload_len >> 16) & 0xffu);
    out[10] = (unsigned char)((payload_len >> 8) & 0xffu);
    out[11] = (unsigned char)(payload_len & 0xffu);
}

atlas_status atlas_ipc_header_decode(const unsigned char *in, uint32_t max, atlas_ipc_header *out,
                                     atlas_err *err) {
    memset(out, 0, sizeof(*out));
    if (memcmp(in, ATLAS_IPC_MAGIC, 4u) != 0) {
        /* Deliberately does not echo the bytes: they are attacker-controlled and
         * this message can reach a terminal. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "not an Atlas frame: the four-byte magic does not match");
    }
    out->version = (uint16_t)((in[4] << 8) | in[5]);
    out->flags = (uint16_t)((in[6] << 8) | in[7]);
    out->length = ((uint32_t)in[8] << 24) | ((uint32_t)in[9] << 16) | ((uint32_t)in[10] << 8) |
                  (uint32_t)in[11];

    if (out->version != ATLAS_IPC_PROTOCOL_VERSION) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "unsupported Atlas IPC protocol version %u (this build speaks %u)",
                             (unsigned)out->version, (unsigned)ATLAS_IPC_PROTOCOL_VERSION);
    }
    if (out->flags != 0u) {
        /* Reserved bits are refused rather than ignored, so that a future flag
         * cannot be silently dropped by an old daemon that would then answer the
         * wrong question. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "Atlas frame sets reserved flag bits (0x%04x); refusing it",
                             (unsigned)out->flags);
    }
    if (out->length > max) {
        /* Checked before a single payload byte is read, so a claimed length can
         * never turn into an allocation. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "Atlas frame claims %u payload bytes, above the %u byte limit",
                             (unsigned)out->length, (unsigned)max);
    }
    return ATLAS_OK;
}

/* --- deadlines ----------------------------------------------------------- */

/* Monotonic milliseconds. CLOCK_MONOTONIC so a wall-clock adjustment cannot
 * extend or collapse a deadline. */
static int64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Waits until `fd` is ready or the deadline passes. The remaining time is
 * recomputed on every call, so a peer that dribbles one byte at a time cannot
 * hold a slot indefinitely by resetting a per-read timeout. */
static atlas_status wait_ready(int fd, short events, int64_t deadline, const char *what,
                               atlas_err *err) {
    for (;;) {
        int64_t remaining = deadline - now_ms();
        if (remaining <= 0) {
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "timed out while %s", what);
        }
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = events;
        pfd.revents = 0;
        int rc = poll(&pfd, 1u, remaining > 1000 ? 1000 : (int)remaining);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "poll failed while %s", what);
        }
        if (rc == 0) {
            continue; /* re-check the deadline */
        }
        if ((pfd.revents & (POLLERR | POLLNVAL)) != 0) {
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "socket error while %s", what);
        }
        return ATLAS_OK;
    }
}

/* --- frame I/O ----------------------------------------------------------- */

/* Reads exactly `n` bytes, or reports EOF when the peer closed before any of
 * them arrived. A close *part way* through is an error, not an EOF: a truncated
 * frame must never be handed on as if it were complete. */
static atlas_status read_exact(int fd, void *dst, size_t n, int64_t deadline, bool *eof_out,
                               const char *what, atlas_err *err) {
    unsigned char *p = (unsigned char *)dst;
    size_t got = 0;
    while (got < n) {
        atlas_status st = wait_ready(fd, POLLIN, deadline, what, err);
        if (st != ATLAS_OK) {
            return st;
        }
        ssize_t r = recv(fd, p + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "read failed while %s", what);
        }
        if (r == 0) {
            if (got == 0 && eof_out != NULL) {
                *eof_out = true;
                return ATLAS_OK;
            }
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "peer closed after %zu of %zu bytes while %s", got, n, what);
        }
        got += (size_t)r;
    }
    return ATLAS_OK;
}

atlas_status atlas_ipc_read_frame(int fd, uint32_t max_payload, int timeout_ms, atlas_buf *out,
                                  bool *eof_out, atlas_err *err) {
    if (eof_out != NULL) {
        *eof_out = false;
    }
    atlas_buf_reset(out);
    if (max_payload > ATLAS_IPC_MAX_RESPONSE_BYTES) {
        max_payload = ATLAS_IPC_MAX_RESPONSE_BYTES;
    }
    int64_t deadline = now_ms() + (timeout_ms > 0 ? timeout_ms : ATLAS_IPC_READ_TIMEOUT_MS);

    unsigned char head[ATLAS_IPC_HEADER_BYTES];
    bool eof = false;
    atlas_status st = read_exact(fd, head, sizeof(head), deadline, &eof, "reading a frame header",
                                 err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (eof) {
        if (eof_out != NULL) {
            *eof_out = true;
        }
        return ATLAS_OK;
    }

    atlas_ipc_header h;
    st = atlas_ipc_header_decode(head, max_payload, &h, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (h.length == 0u) {
        return ATLAS_OK; /* a well-formed empty payload; the caller rejects it */
    }
    /* Only now, with the length proven to be within bounds, is memory reserved. */
    st = atlas_buf_reserve(out, (size_t)h.length + 1u, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = read_exact(fd, out->data, h.length, deadline, NULL, "reading a frame payload", err);
    if (st != ATLAS_OK) {
        return st;
    }
    out->len = h.length;
    out->data[out->len] = '\0';
    return ATLAS_OK;
}

atlas_status atlas_ipc_write_frame(int fd, const void *payload, size_t len, int timeout_ms,
                                   atlas_err *err) {
    if (len > ATLAS_IPC_MAX_RESPONSE_BYTES) {
        /* An oversized response is an internal fault, never a truncated
         * document: sending half of a JSON object would be worse than failing. */
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "refusing to send a %zu byte response, above the %u byte limit", len,
                             (unsigned)ATLAS_IPC_MAX_RESPONSE_BYTES);
    }
    int64_t deadline = now_ms() + (timeout_ms > 0 ? timeout_ms : ATLAS_IPC_WRITE_TIMEOUT_MS);

    unsigned char head[ATLAS_IPC_HEADER_BYTES];
    atlas_ipc_header_encode(head, (uint32_t)len);

    struct iovec_like {
        const unsigned char *p;
        size_t n;
    } parts[2] = {{head, sizeof(head)}, {(const unsigned char *)payload, len}};

    for (int i = 0; i < 2; i++) {
        size_t sent = 0;
        while (sent < parts[i].n) {
            atlas_status st = wait_ready(fd, POLLOUT, deadline, "writing a frame", err);
            if (st != ATLAS_OK) {
                return st;
            }
            /* MSG_NOSIGNAL: a peer that vanished must produce EPIPE here, not
             * SIGPIPE in a daemon that has other clients to serve. */
            ssize_t w = send(fd, parts[i].p + sent, parts[i].n - sent, MSG_NOSIGNAL);
            if (w < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno,
                                           "write failed while sending a frame");
            }
            sent += (size_t)w;
        }
    }
    return ATLAS_OK;
}
