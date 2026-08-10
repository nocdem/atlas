/* Atlas - IPC framing, request parsing and socket policy.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * These run without a daemon: the codec and the parser are driven directly with
 * bytes no well-behaved client would send, which is the only way to be sure the
 * daemon's first line of defence works.
 */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/ipc.h"
#include "atlas_test.h"
#include "support/fixture.h"

/* --- the frame header ---------------------------------------------------- */

static void test_header_roundtrip(void) {
    unsigned char buf[ATLAS_IPC_HEADER_BYTES];
    atlas_ipc_header_encode(buf, 0x01020304u);
    T_CHECK(memcmp(buf, ATLAS_IPC_MAGIC, 4u) == 0);
    /* Big-endian on the wire, so a capture is readable and two machines of
     * different endianness cannot disagree. */
    T_EQ_INT(buf[8], 0x01);
    T_EQ_INT(buf[9], 0x02);
    T_EQ_INT(buf[10], 0x03);
    T_EQ_INT(buf[11], 0x04);

    atlas_err err;
    atlas_err_init(&err);
    atlas_ipc_header h;
    T_OK(atlas_ipc_header_decode(buf, 0xffffffffu, &h, &err), &err);
    T_EQ_INT(h.version, ATLAS_IPC_PROTOCOL_VERSION);
    T_EQ_INT(h.flags, 0);
    T_EQ_INT(h.length, 0x01020304u);
}

static void test_header_rejections(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_ipc_header h;
    unsigned char buf[ATLAS_IPC_HEADER_BYTES];

    atlas_ipc_header_encode(buf, 4u);
    buf[0] = 'X';
    T_FAILS_WITH(atlas_ipc_header_decode(buf, 1024u, &h, &err), ATLAS_ERR_USAGE, &err);

    atlas_ipc_header_encode(buf, 4u);
    buf[5] = 99;
    T_FAILS_WITH(atlas_ipc_header_decode(buf, 1024u, &h, &err), ATLAS_ERR_USAGE, &err);
    T_CHECK(strstr(atlas_err_msg(&err), "protocol version") != NULL);

    /* Reserved flags are refused rather than ignored: an old daemon that
     * silently drops a future flag would answer a different question from the
     * one it was asked. */
    atlas_ipc_header_encode(buf, 4u);
    buf[7] = 1;
    T_FAILS_WITH(atlas_ipc_header_decode(buf, 1024u, &h, &err), ATLAS_ERR_USAGE, &err);
    T_CHECK(strstr(atlas_err_msg(&err), "reserved flag") != NULL);

    /* The length ceiling is checked before any payload byte is read, so a huge
     * claimed length can never become an allocation. */
    atlas_ipc_header_encode(buf, 4u * 1024u * 1024u);
    T_FAILS_WITH(atlas_ipc_header_decode(buf, 1024u, &h, &err), ATLAS_ERR_USAGE, &err);
    T_CHECK(strstr(atlas_err_msg(&err), "limit") != NULL);
}

/* --- frame I/O over a socketpair ----------------------------------------- */

static void test_frame_roundtrip_and_partial(void) {
    atlas_err err;
    atlas_err_init(&err);
    int sp[2];
    T_REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sp) == 0);

    static const char body[] = "{\"method\":\"daemon.ping\"}";
    T_OK(atlas_ipc_write_frame(sp[0], body, strlen(body), 2000, &err), &err);

    atlas_buf got = ATLAS_BUF_INIT;
    bool eof = false;
    T_OK(atlas_ipc_read_frame(sp[1], ATLAS_IPC_MAX_REQUEST_BYTES, 2000, &got, &eof, &err), &err);
    T_CHECK(!eof);
    T_EQ_STR(atlas_buf_cstr(&got), body);

    /* A header that promises more than arrives must time out rather than hand
     * back a short payload as if it were whole. */
    unsigned char head[ATLAS_IPC_HEADER_BYTES];
    atlas_ipc_header_encode(head, 64u);
    T_REQUIRE(write(sp[0], head, sizeof(head)) == (ssize_t)sizeof(head));
    T_REQUIRE(write(sp[0], "short", 5u) == 5);
    atlas_err perr;
    atlas_err_init(&perr);
    atlas_status st = atlas_ipc_read_frame(sp[1], ATLAS_IPC_MAX_REQUEST_BYTES, 250, &got, &eof,
                                           &perr);
    T_CHECK_MSG(st != ATLAS_OK, "a partial frame must not be accepted as complete");

    atlas_buf_free(&got);
    (void)close(sp[0]);
    (void)close(sp[1]);
}

static void test_frame_eof_is_not_an_error(void) {
    atlas_err err;
    atlas_err_init(&err);
    int sp[2];
    T_REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sp) == 0);
    (void)close(sp[0]);

    atlas_buf got = ATLAS_BUF_INIT;
    bool eof = false;
    /* An idle peer that hangs up between frames is normal, not a fault. */
    T_OK(atlas_ipc_read_frame(sp[1], ATLAS_IPC_MAX_REQUEST_BYTES, 1000, &got, &eof, &err), &err);
    T_CHECK(eof);
    atlas_buf_free(&got);
    (void)close(sp[1]);
}

static void test_oversized_response_refused(void) {
    atlas_err err;
    atlas_err_init(&err);
    int sp[2];
    T_REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sp) == 0);
    /* Sending half a document would be worse than failing, so an oversized
     * response is an error rather than a truncation. */
    static char big[16];
    T_FAILS_WITH(atlas_ipc_write_frame(sp[0], big, (size_t)ATLAS_IPC_MAX_RESPONSE_BYTES + 1u, 500,
                                       &err),
                 ATLAS_ERR_INTERNAL, &err);
    (void)close(sp[0]);
    (void)close(sp[1]);
}

/* --- request parsing ----------------------------------------------------- */

static void test_request_parse(void) {
    atlas_err err;
    atlas_err_init(&err);
    static const char doc[] =
        "{\"id\":\"abc\",\"method\":\"repo.state\",\"params\":{\"repo\":\"x\",\"since\":7,"
        "\"full\":true}}";
    atlas_ipc_request *req = NULL;
    T_OK(atlas_ipc_request_parse(doc, strlen(doc), &req, &err), &err);
    T_EQ_STR(atlas_ipc_request_id(req), "abc");
    T_EQ_STR(atlas_ipc_request_method(req), "repo.state");

    const char *s = NULL;
    T_CHECK(atlas_ipc_param_str(req, "repo", &s));
    T_EQ_STR(s, "x");
    int64_t n = 0;
    T_CHECK(atlas_ipc_param_int(req, "since", &n));
    T_EQ_INT(n, 7);
    bool b = false;
    T_CHECK(atlas_ipc_param_bool(req, "full", &b));
    T_CHECK(b);

    /* No coercion. A caller that sends the wrong type gets a refusal, not a
     * guess: guessing is how a protocol grows behaviour nobody documented. */
    T_CHECK_MSG(!atlas_ipc_param_int(req, "repo", &n), "a string must not read as an integer");
    T_CHECK_MSG(!atlas_ipc_param_str(req, "since", &s), "an integer must not read as a string");
    T_CHECK_MSG(!atlas_ipc_param_bool(req, "since", &b), "an integer must not read as a bool");
    T_CHECK_MSG(!atlas_ipc_param_str(req, "absent", &s), "an absent member must not read");
    atlas_ipc_request_free(req);
}

static void test_request_malformed(void) {
    atlas_err err;
    atlas_ipc_request *req = NULL;
    static const char *const BAD[] = {
        "",                              /* empty */
        "{",                             /* truncated */
        "[]",                            /* not an object */
        "\"string\"",                    /* not an object */
        "{\"method\":42}",               /* method is not a string */
        "{\"method\":\"x\",\"params\":[]}", /* params is not an object */
        "{\"method\":\"x\",\"id\":{}}",  /* id is not a string */
        "{\"method\":\"x\"} trailing",   /* trailing garbage */
        "{'method':'x'}",                /* single quotes are not JSON */
        "{\"method\":\"x\",}",           /* trailing comma is not JSON */
    };
    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        atlas_err_init(&err);
        req = NULL;
        atlas_status st = atlas_ipc_request_parse(BAD[i], strlen(BAD[i]), &req, &err);
        T_CHECK_MSG(st != ATLAS_OK, "case %zu (\"%s\") should have been refused", i, BAD[i]);
        T_CHECK_MSG(req == NULL, "case %zu must not hand back a request", i);
    }
}

static void test_request_depth_bounded(void) {
    /* A deeply nested document is cheap to send and expensive to consume
     * recursively. The limit is enforced explicitly, because yyjson has none. */
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf doc = ATLAS_BUF_INIT;
    T_OK(atlas_buf_append_str(&doc, "{\"method\":\"x\",\"params\":{\"a\":", &err), &err);
    const size_t depth = ATLAS_IPC_MAX_JSON_DEPTH + 8u;
    for (size_t i = 0; i < depth; i++) {
        T_OK(atlas_buf_append_ch(&doc, '[', &err), &err);
    }
    T_OK(atlas_buf_append_ch(&doc, '1', &err), &err);
    for (size_t i = 0; i < depth; i++) {
        T_OK(atlas_buf_append_ch(&doc, ']', &err), &err);
    }
    T_OK(atlas_buf_append_str(&doc, "}}", &err), &err);

    atlas_ipc_request *req = NULL;
    atlas_err derr;
    atlas_err_init(&derr);
    T_FAILS_WITH(atlas_ipc_request_parse(doc.data, doc.len, &req, &derr), ATLAS_ERR_USAGE, &derr);
    T_CHECK(strstr(atlas_err_msg(&derr), "nests deeper") != NULL);
    T_CHECK(req == NULL);

    /* A document just inside the limit is still accepted, so the bound is a
     * bound and not an accidental rejection of ordinary input. */
    atlas_buf ok_doc = ATLAS_BUF_INIT;
    T_OK(atlas_buf_append_str(&ok_doc, "{\"method\":\"x\",\"params\":{\"a\":", &err), &err);
    for (size_t i = 0; i < 4u; i++) {
        T_OK(atlas_buf_append_ch(&ok_doc, '[', &err), &err);
    }
    T_OK(atlas_buf_append_ch(&ok_doc, '1', &err), &err);
    for (size_t i = 0; i < 4u; i++) {
        T_OK(atlas_buf_append_ch(&ok_doc, ']', &err), &err);
    }
    T_OK(atlas_buf_append_str(&ok_doc, "}}", &err), &err);
    req = NULL;
    T_OK(atlas_ipc_request_parse(ok_doc.data, ok_doc.len, &req, &err), &err);
    atlas_ipc_request_free(req);

    atlas_buf_free(&doc);
    atlas_buf_free(&ok_doc);
}

static void test_request_hostile_strings(void) {
    atlas_err err;
    atlas_err_init(&err);
    /* An id carrying terminal control bytes is echoed in the response and in
     * logs, so it must come back safe-encoded rather than verbatim. */
    static const char doc[] = "{\"id\":\"\\u001b[31mred\",\"method\":\"daemon.ping\"}";
    atlas_ipc_request *req = NULL;
    T_OK(atlas_ipc_request_parse(doc, strlen(doc), &req, &err), &err);
    const char *id = atlas_ipc_request_id(req);
    T_CHECK_MSG(strchr(id, 0x1b) == NULL, "the id must not carry an ESC byte, got \"%s\"", id);
    T_CHECK(strstr(id, "%1B") != NULL);
    atlas_ipc_request_free(req);

    /* An over-long id is refused rather than truncated: a truncated id would
     * correlate a response with the wrong request. */
    atlas_buf big = ATLAS_BUF_INIT;
    T_OK(atlas_buf_append_str(&big, "{\"id\":\"", &err), &err);
    for (size_t i = 0; i < ATLAS_IPC_MAX_REQUEST_ID + 16u; i++) {
        T_OK(atlas_buf_append_ch(&big, 'a', &err), &err);
    }
    T_OK(atlas_buf_append_str(&big, "\",\"method\":\"daemon.ping\"}", &err), &err);
    req = NULL;
    atlas_err berr;
    atlas_err_init(&berr);
    T_FAILS_WITH(atlas_ipc_request_parse(big.data, big.len, &req, &berr), ATLAS_ERR_USAGE, &berr);
    atlas_buf_free(&big);
}

/* --- socket policy ------------------------------------------------------- */

static void test_runtime_dir_requires_xdg(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf out = ATLAS_BUF_INIT;
    const char *saved = getenv("XDG_RUNTIME_DIR");
    char *copy = (saved != NULL) ? strdup(saved) : NULL;
    const char *saved_dd = getenv("ATLAS_DATA_DIR");
    char *copy_dd = (saved_dd != NULL) ? strdup(saved_dd) : NULL;

    /* **A7.1: name an index that is not the system one, so this exercises the
     * per-user resolution it is about.**
     *
     * A socket belongs to an index. On a machine with a system deployment, a
     * process that names no index resolves the authoritative one and therefore
     * the system socket — correctly, and with `$XDG_RUNTIME_DIR` ignored, which
     * is the whole point of that rule. This test is about the *other* branch,
     * so it says which index it means rather than depending on whether the
     * machine happens to be deployed. */
    (void)setenv("ATLAS_DATA_DIR", "/tmp/atlas-ipc-runtime-test", 1);

    (void)unsetenv("XDG_RUNTIME_DIR");
    /* With the variable absent, Atlas falls back to the one directory a login
     * session would have named — and only after proving it is private. Which
     * branch this machine takes is decided here rather than skipped, so the
     * assertion is exact either way.
     *
     * The properties checked here are the properties the fallback checks: a
     * directory, not a link, owned by this uid, nothing for group or other.
     * There is still no /tmp fallback, and the refusal has to say so. */
    char probe[64];
    (void)snprintf(probe, sizeof probe, "/run/user/%lld", (long long)getuid());
    struct stat rsb;
    bool qualifies = lstat(probe, &rsb) == 0 && S_ISDIR(rsb.st_mode) && !S_ISLNK(rsb.st_mode) &&
                     rsb.st_uid == getuid() && (rsb.st_mode & (S_IRWXG | S_IRWXO)) == 0;
    if (qualifies) {
        char want[80];
        (void)snprintf(want, sizeof want, "%s/atlas", probe);
        T_OK(atlas_ipc_runtime_dir(&out, &err), &err);
        T_EQ_STR(atlas_buf_cstr(&out), want);
    } else {
        T_FAILS_WITH(atlas_ipc_runtime_dir(&out, &err), ATLAS_ERR_CONFIG, &err);
        T_CHECK(strstr(atlas_err_msg(&err), "/tmp") != NULL);
        T_CHECK(strstr(atlas_err_msg(&err), "XDG_RUNTIME_DIR") != NULL);
    }

    /* A directory that is not private is never used, whatever its name. The
     * fallback only ever considers /run/user/<uid>, so this is checked by
     * pointing the variable at a world-writable directory and confirming that
     * the *explicit* path is still taken verbatim — the fallback is not a
     * second opinion about a path the caller gave. */
    (void)setenv("XDG_RUNTIME_DIR", "/tmp", 1);
    T_OK(atlas_ipc_runtime_dir(&out, &err), &err);
    T_EQ_STR(atlas_buf_cstr(&out), "/tmp/atlas");

    (void)setenv("XDG_RUNTIME_DIR", "relative/path", 1);
    T_FAILS_WITH(atlas_ipc_runtime_dir(&out, &err), ATLAS_ERR_CONFIG, &err);

    (void)setenv("XDG_RUNTIME_DIR", "/run/user/1234/", 1);
    T_OK(atlas_ipc_runtime_dir(&out, &err), &err);
    T_EQ_STR(atlas_buf_cstr(&out), "/run/user/1234/atlas");

    if (copy_dd != NULL) {
        (void)setenv("ATLAS_DATA_DIR", copy_dd, 1);
        free(copy_dd);
    } else {
        (void)unsetenv("ATLAS_DATA_DIR");
    }
    if (copy != NULL) {
        (void)setenv("XDG_RUNTIME_DIR", copy, 1);
        free(copy);
    } else {
        (void)unsetenv("XDG_RUNTIME_DIR");
    }
    atlas_buf_free(&out);
}

static void test_listen_permissions_and_collisions(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    atlas_buf dir = ATLAS_BUF_INIT;
    T_OK(atlas_buf_set(&dir, fx.root.data, fx.root.len, &err), &err);
    T_OK(atlas_buf_append_str(&dir, "/rt", &err), &err);
    T_OK(atlas_ipc_ensure_runtime_dir(atlas_buf_cstr(&dir), NULL, &err), &err);

    struct stat sb;
    T_REQUIRE(lstat(atlas_buf_cstr(&dir), &sb) == 0);
    T_CHECK_MSG((sb.st_mode & (S_IRWXG | S_IRWXO)) == 0,
                "the runtime directory must be 0700, got %o", (unsigned)(sb.st_mode & 07777));

    atlas_buf sock = ATLAS_BUF_INIT;
    T_OK(atlas_buf_set(&sock, dir.data, dir.len, &err), &err);
    T_OK(atlas_buf_append_str(&sock, "/atlas.sock", &err), &err);

    int fd = -1;
    T_OK(atlas_ipc_listen(atlas_buf_cstr(&sock), NULL, &fd, &err), &err);
    T_REQUIRE(fd >= 0);
    T_REQUIRE(lstat(atlas_buf_cstr(&sock), &sb) == 0);
    T_CHECK_MSG((sb.st_mode & (S_IRWXG | S_IRWXO)) == 0,
                "the socket must be 0600, got %o", (unsigned)(sb.st_mode & 07777));
    T_CHECK(S_ISSOCK(sb.st_mode));

    /* A live socket is never unlinked out from under its owner. */
    int second = -1;
    atlas_err serr;
    atlas_err_init(&serr);
    T_FAILS_WITH(atlas_ipc_listen(atlas_buf_cstr(&sock), NULL, &second, &serr), ATLAS_ERR_INTEGRITY,
                 &serr);
    T_CHECK(strstr(atlas_err_msg(&serr), "already listening") != NULL);
    (void)close(fd);

    /* A dead socket left behind by a crash *is* removable, because nothing
     * answers on it and it is ours. */
    T_OK(atlas_ipc_listen(atlas_buf_cstr(&sock), NULL, &fd, &err), &err);
    (void)close(fd);
    (void)unlink(atlas_buf_cstr(&sock));

    /* A regular file in the way is refused, not deleted: "clean up whatever is
     * there" is how somebody's data disappears during a service start. */
    int rf = open(atlas_buf_cstr(&sock), O_WRONLY | O_CREAT | O_EXCL, 0600);
    T_REQUIRE(rf >= 0);
    (void)close(rf);
    atlas_err ferr;
    atlas_err_init(&ferr);
    T_FAILS_WITH(atlas_ipc_listen(atlas_buf_cstr(&sock), NULL, &fd, &ferr), ATLAS_ERR_INTEGRITY, &ferr);
    T_CHECK(strstr(atlas_err_msg(&ferr), "not a socket") != NULL);
    T_CHECK_MSG(access(atlas_buf_cstr(&sock), F_OK) == 0,
                "the refused file must still be there, not deleted");
    (void)unlink(atlas_buf_cstr(&sock));

    /* A symlink in the way is refused too. */
    T_REQUIRE(symlink("/dev/null", atlas_buf_cstr(&sock)) == 0);
    atlas_err lerr;
    atlas_err_init(&lerr);
    T_FAILS_WITH(atlas_ipc_listen(atlas_buf_cstr(&sock), NULL, &fd, &lerr), ATLAS_ERR_INTEGRITY, &lerr);
    T_CHECK(strstr(atlas_err_msg(&lerr), "symbolic link") != NULL);
    (void)unlink(atlas_buf_cstr(&sock));

    atlas_buf_free(&dir);
    atlas_buf_free(&sock);
    fx_close(&fx);
}

static void test_runtime_dir_rejects_symlink(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    atlas_buf dir = ATLAS_BUF_INIT;
    T_OK(atlas_buf_set(&dir, fx.root.data, fx.root.len, &err), &err);
    T_OK(atlas_buf_append_str(&dir, "/linked", &err), &err);
    T_REQUIRE(symlink("/tmp", atlas_buf_cstr(&dir)) == 0);

    /* Following this would put the socket in a world-writable directory, which
     * is exactly what refusing a /tmp fallback was supposed to prevent. */
    atlas_err derr;
    atlas_err_init(&derr);
    T_FAILS_WITH(atlas_ipc_ensure_runtime_dir(atlas_buf_cstr(&dir), NULL, &derr), ATLAS_ERR_INTEGRITY,
                 &derr);
    T_CHECK(strstr(atlas_err_msg(&derr), "symbolic link") != NULL);

    atlas_buf_free(&dir);
    fx_close(&fx);
}

static const atlas_test TESTS[] = {
    {"frame header round trip", test_header_roundtrip},
    {"frame header rejections", test_header_rejections},
    {"frame round trip and partial frames", test_frame_roundtrip_and_partial},
    {"a clean close between frames is not an error", test_frame_eof_is_not_an_error},
    {"an oversized response is refused, never truncated", test_oversized_response_refused},
    {"request parsing and typed params", test_request_parse},
    {"malformed requests are refused", test_request_malformed},
    {"request nesting depth is bounded", test_request_depth_bounded},
    {"hostile ids are encoded and bounded", test_request_hostile_strings},
    {"the runtime directory is private, from the environment or from /run/user",
     test_runtime_dir_requires_xdg},
    {"socket permissions and path collisions", test_listen_permissions_and_collisions},
    {"a symlinked runtime directory is refused", test_runtime_dir_rejects_symlink},
};

ATLAS_TEST_MAIN("ipc", TESTS)
