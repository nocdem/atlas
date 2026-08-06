/* Atlas - a live daemon: lifecycle, IPC policy, and the watcher.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * These fork the built binary with an isolated data directory and an isolated
 * XDG_RUNTIME_DIR. No systemd unit is installed, enabled or started, and nothing
 * outside the fixture's temporary tree is touched.
 *
 * Watcher assertions wait for an observable outcome rather than sleeping a
 * guessed interval: a test that sleeps is either flaky on a loaded machine or
 * needlessly slow on an idle one.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/ipc.h"
#include "atlas/lock.h"
#include "atlas_test.h"
#include "support/fixture.h"

#define WAIT_MS 30000

typedef struct live {
    fixture fx;
    fx_daemon d;
} live;

/* A repository with a commit, registered, and a daemon watching it. */
static void live_start(live *L, atlas_err *err) {
    T_OK(fx_open(&L->fx, err), err);
    T_OK(fx_init_repo(&L->fx, fx_repo(&L->fx), NULL, err), err);
    T_OK(fx_write(fx_repo(&L->fx), "a.c", "int a;\n", err), err);
    T_OK(fx_mkdir(fx_repo(&L->fx), "sub", err), err);
    T_OK(fx_write(fx_repo(&L->fx), "sub/b.c", "int b;\n", err), err);
    T_OK(fx_add_all(&L->fx, fx_repo(&L->fx), err), err);
    T_OK(fx_commit(&L->fx, fx_repo(&L->fx), "first", err), err);

    const char *add[] = {"--data-dir", fx_data_dir(&L->fx), "repo", "add", fx_repo(&L->fx),
                         "--name", "fixture"};
    int code = 0;
    T_OK(fx_atlas(add, 7u, NULL, NULL, &code, err), err);
    T_EQ_INT(code, 0);

    fx_daemon_init(&L->d);
    T_OK(fx_daemon_start(&L->fx, &L->d, err), err);
    T_OK(fx_daemon_wait_ready(&L->d, WAIT_MS, err), err);
}

static void live_stop(live *L, bool hard) {
    fx_daemon_stop(&L->d, hard);
    fx_daemon_free(&L->d);
    fx_close(&L->fx);
}

static void cli(live *L, const char *const *args, size_t n, atlas_buf *out, int *code,
                atlas_err *err) {
    T_OK(fx_atlas_with_runtime(&L->fx, &L->d, args, n, out, NULL, code, err), err);
}

/* Waits until `atlas events fixture --json` contains `needle`. */
static bool wait_events(live *L, const char *needle, atlas_err *err) {
    const char *args[] = {"events", "fixture", "--json", "--limit", "200"};
    bool found = false;
    T_OK(fx_wait_for_substring(&L->fx, &L->d, args, 5u, needle, WAIT_MS, &found, err), err);
    return found;
}

/* --- lifecycle ----------------------------------------------------------- */

static void test_start_ping_status_stop(void) {
    live L;
    atlas_err err;
    atlas_err_init(&err);
    live_start(&L, &err);

    atlas_buf out = ATLAS_BUF_INIT;
    int code = -1;
    const char *ping[] = {"daemon", "ping", "--json"};
    cli(&L, ping, 3u, &out, &code, &err);
    T_EQ_INT(code, 0);
    T_CHECK(strstr(atlas_buf_cstr(&out), "\"reachable\":true") != NULL);

    atlas_buf_reset(&out);
    const char *status[] = {"daemon", "status", "--json"};
    cli(&L, status, 3u, &out, &code, &err);
    T_EQ_INT(code, 0);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"running\":true") != NULL,
                "the daemon owns the writer lock, so status must say running: %s",
                atlas_buf_cstr(&out));
    T_CHECK(strstr(atlas_buf_cstr(&out), "\"reachable\":true") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&out), "\"repositories\":1") != NULL);

    /* The socket exists with the right mode while it is serving. */
    struct stat sb;
    T_REQUIRE(lstat(atlas_buf_cstr(&L.d.socket), &sb) == 0);
    T_CHECK_MSG((sb.st_mode & (S_IRWXG | S_IRWXO)) == 0, "the socket must be 0600, got %o",
                (unsigned)(sb.st_mode & 07777));
    T_CHECK(S_ISSOCK(sb.st_mode));

    /* Graceful shutdown on SIGTERM: the socket is removed and the lock released,
     * so a restart finds a clean directory rather than debris. */
    fx_daemon_stop(&L.d, false);
    T_CHECK_MSG(lstat(atlas_buf_cstr(&L.d.socket), &sb) != 0,
                "a clean shutdown must remove its own socket");
    bool held = true;
    T_OK(atlas_lock_probe(fx_data_dir(&L.fx), &held, NULL, &err), &err);
    T_CHECK_MSG(!held, "a clean shutdown must release the writer lock");

    atlas_buf_free(&out);
    fx_daemon_free(&L.d);
    fx_close(&L.fx);
}

static void test_restart_after_crash(void) {
    live L;
    atlas_err err;
    atlas_err_init(&err);
    live_start(&L, &err);
    T_CHECK(wait_events(&L, "\"kind\":\"reconciled\"", &err));

    /* SIGKILL: no chance to clean up. The socket file and the daemon_state row
     * both survive, and neither may convince the next start that a daemon is
     * running. */
    fx_daemon_stop(&L.d, true);
    struct stat sb;
    T_CHECK_MSG(lstat(atlas_buf_cstr(&L.d.socket), &sb) == 0,
                "a killed daemon leaves its socket behind; that is the case under test");

    /* The lock is released by the kernel, which is why flock was chosen. */
    bool held = true;
    T_OK(atlas_lock_probe(fx_data_dir(&L.fx), &held, NULL, &err), &err);
    T_CHECK_MSG(!held, "the kernel must release the lock of a killed process");

    /* A change made while nothing was watching. */
    T_OK(fx_write(fx_repo(&L.fx), "made-while-down.c", "1\n", &err), &err);

    /* No second fx_daemon_init here: the struct is already initialised, and
     * re-initialising it would memset over the buffers it owns. fx_daemon_stop
     * has already reset the pid, and fx_daemon_start assigns into the existing
     * buffers rather than replacing them. */
    T_OK(fx_daemon_start(&L.fx, &L.d, &err), &err);
    T_OK(fx_daemon_wait_ready(&L.d, WAIT_MS, &err), &err);

    /* The restart reconciles from scratch rather than trusting an index built
     * from events it never saw, so the change made while it was down is found. */
    T_CHECK_MSG(wait_events(&L, "made-while-down.c", &err),
                "a restart must discover changes made while the daemon was not running");

    live_stop(&L, false);
}

static void test_second_daemon_is_refused(void) {
    live L;
    atlas_err err;
    atlas_err_init(&err);
    live_start(&L, &err);

    /* A second daemon on the same data directory must refuse: two writers is
     * exactly what the lock exists to prevent. */
    fx_daemon second;
    fx_daemon_init(&second);
    T_OK(fx_daemon_start(&L.fx, &second, &err), &err);
    /* It should exit quickly rather than sit there half-started. */
    bool exited = false;
    for (int i = 0; i < 200 && !exited; i++) {
        exited = fx_daemon_exited(&second);
        struct timespec nap = {0, 25L * 1000000L};
        (void)nanosleep(&nap, NULL);
    }
    T_CHECK_MSG(exited, "a second daemon must refuse to start, not linger");
    fx_daemon_stop(&second, true);
    fx_daemon_free(&second);

    live_stop(&L, false);
}

static void test_offline_writer_refused_while_daemon_runs(void) {
    live L;
    atlas_err err;
    atlas_err_init(&err);
    live_start(&L, &err);

    /* `atlas scan` is a mutation. With a daemon running it is routed to the
     * daemon rather than performed here, so it succeeds without a second
     * writer ever existing. */
    atlas_buf out = ATLAS_BUF_INIT;
    int code = -1;
    const char *scan[] = {"scan", "fixture", "--json"};
    cli(&L, scan, 3u, &out, &code, &err);
    T_EQ_INT(code, 0);

    /* And a read command still works against the read-only handle. */
    atlas_buf_reset(&out);
    const char *list[] = {"repo", "list", "--json"};
    cli(&L, list, 3u, &out, &code, &err);
    T_EQ_INT(code, 0);
    T_CHECK(strstr(atlas_buf_cstr(&out), "fixture") != NULL);

    /* Taking the writer lock directly, as a second offline writer would, is
     * refused while the daemon holds it. */
    atlas_lock *lk = NULL;
    atlas_err lerr;
    atlas_err_init(&lerr);
    T_FAILS_WITH(atlas_lock_acquire(fx_data_dir(&L.fx), ATLAS_LOCK_ROLE_ONESHOT, &lk, &lerr),
                 ATLAS_ERR_INTEGRITY, &lerr);

    atlas_buf_free(&out);
    live_stop(&L, false);
}

/* --- IPC policy against a live daemon ------------------------------------ */

static void test_malformed_frames(void) {
    live L;
    atlas_err err;
    atlas_err_init(&err);
    live_start(&L, &err);
    const char *sock = atlas_buf_cstr(&L.d.socket);

    atlas_buf resp = ATLAS_BUF_INIT;
    bool closed = false;

    /* Garbage where a header belongs. */
    T_OK(fx_ipc_raw(sock, "not-a-frame-at-all", 18u, &resp, &closed, &err), &err);
    T_CHECK_MSG(resp.len > 0, "a bad frame should still get a structured answer");
    T_CHECK(strstr(atlas_buf_cstr(&resp), "\"ok\":false") != NULL);

    /* A header claiming more than the request ceiling. The daemon must refuse
     * before reading a payload, so this costs it nothing. */
    unsigned char big[ATLAS_IPC_HEADER_BYTES];
    atlas_ipc_header_encode(big, ATLAS_IPC_MAX_REQUEST_BYTES + 1u);
    atlas_buf_reset(&resp);
    T_OK(fx_ipc_raw(sock, big, sizeof(big), &resp, &closed, &err), &err);
    T_CHECK(strstr(atlas_buf_cstr(&resp), "\"ok\":false") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&resp), "limit") != NULL);

    /* A header with a wrong protocol version. */
    unsigned char ver[ATLAS_IPC_HEADER_BYTES];
    atlas_ipc_header_encode(ver, 2u);
    ver[5] = 42;
    atlas_buf_reset(&resp);
    T_OK(fx_ipc_raw(sock, ver, sizeof(ver), &resp, &closed, &err), &err);
    T_CHECK(strstr(atlas_buf_cstr(&resp), "\"ok\":false") != NULL);

    /* A well-framed but malformed JSON payload. */
    unsigned char frame[ATLAS_IPC_HEADER_BYTES + 8u];
    atlas_ipc_header_encode(frame, 8u);
    memcpy(frame + ATLAS_IPC_HEADER_BYTES, "{not:jso", 8u);
    atlas_buf_reset(&resp);
    T_OK(fx_ipc_raw(sock, frame, sizeof(frame), &resp, &closed, &err), &err);
    T_CHECK(strstr(atlas_buf_cstr(&resp), "\"ok\":false") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&resp), "not valid JSON") != NULL);

    /* An unknown method. */
    static const char unknown[] = "{\"id\":\"1\",\"method\":\"daemon.shutdown\"}";
    unsigned char um[ATLAS_IPC_HEADER_BYTES + sizeof(unknown) - 1u];
    atlas_ipc_header_encode(um, (uint32_t)(sizeof(unknown) - 1u));
    memcpy(um + ATLAS_IPC_HEADER_BYTES, unknown, sizeof(unknown) - 1u);
    atlas_buf_reset(&resp);
    T_OK(fx_ipc_raw(sock, um, sizeof(um), &resp, &closed, &err), &err);
    T_CHECK(strstr(atlas_buf_cstr(&resp), "\"ok\":false") != NULL);
    /* There is deliberately no remotely callable shutdown. */
    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "unknown method") != NULL,
                "daemon.shutdown must not exist: %s", atlas_buf_cstr(&resp));

    /* After all of that the daemon is still serving. */
    atlas_buf out = ATLAS_BUF_INIT;
    int code = -1;
    const char *ping[] = {"daemon", "ping", "--json"};
    cli(&L, ping, 3u, &out, &code, &err);
    T_EQ_INT(code, 0);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"reachable\":true") != NULL,
                "malformed input must never take the daemon down");

    atlas_buf_free(&out);
    atlas_buf_free(&resp);
    live_stop(&L, false);
}

static void test_slow_client_and_many_clients(void) {
    live L;
    atlas_err err;
    atlas_err_init(&err);
    live_start(&L, &err);

    /* A client that connects, sends a partial header and stops. The daemon must
     * time it out rather than block every other client behind it. */
    int slow = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    T_REQUIRE(slow >= 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    (void)snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", atlas_buf_cstr(&L.d.socket));
    T_REQUIRE(connect(slow, (const struct sockaddr *)&addr, sizeof(addr)) == 0);
    T_REQUIRE(write(slow, "ATL", 3) == 3);

    /* Several well-behaved clients while the slow one is stuck. */
    for (int i = 0; i < 8; i++) {
        atlas_buf out = ATLAS_BUF_INIT;
        int code = -1;
        const char *ping[] = {"daemon", "ping", "--json"};
        cli(&L, ping, 3u, &out, &code, &err);
        T_CHECK_MSG(code == 0, "client %d should be served while a slow client is stuck", i);
        T_CHECK(strstr(atlas_buf_cstr(&out), "\"reachable\":true") != NULL);
        atlas_buf_free(&out);
    }
    (void)close(slow);

    live_stop(&L, false);
}

/* --- the watcher --------------------------------------------------------- */

static void test_watcher_create_modify_delete(void) {
    live L;
    atlas_err err;
    atlas_err_init(&err);
    live_start(&L, &err);
    T_CHECK(wait_events(&L, "\"kind\":\"reconciled\"", &err));

    T_OK(fx_write(fx_repo(&L.fx), "created.c", "1\n", &err), &err);
    T_CHECK_MSG(wait_events(&L, "created.c", &err), "a created file must be observed");

    T_OK(fx_write(fx_repo(&L.fx), "a.c", "modified\n", &err), &err);
    T_CHECK_MSG(wait_events(&L, "\"kind\":\"file_modified\"", &err),
                "a modified tracked file must be observed");

    T_OK(fx_remove(fx_repo(&L.fx), "created.c", &err), &err);
    /* A deletion shows up as the file no longer being live; the state stays
     * consistent rather than the file lingering. */
    const char *state[] = {"events", "fixture", "--json", "--limit", "1"};
    bool found = false;
    T_OK(fx_wait_for_substring(&L.fx, &L.d, state, 5u, "\"index_current\":true", WAIT_MS, &found,
                               &err),
         &err);
    T_CHECK(found);

    live_stop(&L, false);
}

static void test_watcher_atomic_save_and_rename(void) {
    live L;
    atlas_err err;
    atlas_err_init(&err);
    live_start(&L, &err);
    T_CHECK(wait_events(&L, "\"kind\":\"reconciled\"", &err));

    /* The editor pattern: write a temporary file, then rename it over the
     * target. inotify reports this as IN_MOVED_FROM/IN_MOVED_TO, not IN_MODIFY,
     * so a watcher that only handles writes would miss every save. */
    T_OK(fx_write(fx_repo(&L.fx), "a.c.tmp", "atomically saved\n", &err), &err);
    atlas_buf from = ATLAS_BUF_INIT;
    atlas_buf to = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&from, &err, "%s/a.c.tmp", fx_repo(&L.fx)), &err);
    T_OK(atlas_buf_appendf(&to, &err, "%s/a.c", fx_repo(&L.fx)), &err);
    T_REQUIRE(rename(atlas_buf_cstr(&from), atlas_buf_cstr(&to)) == 0);

    T_CHECK_MSG(wait_events(&L, "\"kind\":\"file_modified\"", &err),
                "an atomic save must be observed as a modification");

    /* A rename between two watched directories. */
    atlas_buf_reset(&from);
    atlas_buf_reset(&to);
    T_OK(atlas_buf_appendf(&from, &err, "%s/sub/b.c", fx_repo(&L.fx)), &err);
    T_OK(atlas_buf_appendf(&to, &err, "%s/moved-b.c", fx_repo(&L.fx)), &err);
    T_REQUIRE(rename(atlas_buf_cstr(&from), atlas_buf_cstr(&to)) == 0);
    T_CHECK_MSG(wait_events(&L, "moved-b.c", &err),
                "a rename across watched directories must be observed");

    atlas_buf_free(&from);
    atlas_buf_free(&to);
    live_stop(&L, false);
}

static void test_watcher_nested_new_directory(void) {
    live L;
    atlas_err err;
    atlas_err_init(&err);
    live_start(&L, &err);
    T_CHECK(wait_events(&L, "\"kind\":\"reconciled\"", &err));

    /* A directory created while the daemon is running, with files inside it and
     * a nested directory inside that. Each file must be discovered
     * individually — this is the A0 limitation the roadmap made an A1
     * acceptance criterion. */
    T_OK(fx_mkdir(fx_repo(&L.fx), "brandnew", &err), &err);
    T_OK(fx_write(fx_repo(&L.fx), "brandnew/one.c", "1\n", &err), &err);
    T_OK(fx_mkdir(fx_repo(&L.fx), "brandnew/deeper", &err), &err);
    T_OK(fx_write(fx_repo(&L.fx), "brandnew/deeper/two.c", "2\n", &err), &err);

    T_CHECK_MSG(wait_events(&L, "brandnew/one.c", &err),
                "a file in a new directory must be indexed by path");
    T_CHECK_MSG(wait_events(&L, "brandnew/deeper/two.c", &err),
                "a file in a nested new directory must be indexed by path");

    live_stop(&L, false);
}

static void test_watcher_gitignore_and_branch_switch(void) {
    live L;
    atlas_err err;
    atlas_err_init(&err);
    live_start(&L, &err);
    T_CHECK(wait_events(&L, "\"kind\":\"reconciled\"", &err));

    /* An ignored directory must not be indexed... */
    T_OK(fx_write(fx_repo(&L.fx), ".gitignore", "build/\n", &err), &err);
    T_OK(fx_add_all(&L.fx, fx_repo(&L.fx), &err), &err);
    T_OK(fx_commit(&L.fx, fx_repo(&L.fx), "ignore build", &err), &err);
    T_OK(fx_mkdir(fx_repo(&L.fx), "build", &err), &err);
    T_OK(fx_write(fx_repo(&L.fx), "build/artifact.o", "junk\n", &err), &err);
    /* ...and a visible file created at the same moment must be, which is how we
     * know the pass ran rather than that nothing happened. */
    T_OK(fx_write(fx_repo(&L.fx), "visible.c", "1\n", &err), &err);
    T_CHECK(wait_events(&L, "visible.c", &err));

    atlas_buf out = ATLAS_BUF_INIT;
    int code = -1;
    const char *events[] = {"events", "fixture", "--json", "--limit", "500"};
    cli(&L, events, 5u, &out, &code, &err);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "artifact.o") == NULL,
                "an ignored file must never be indexed");

    /* A branch switch changes HEAD and the working tree under the daemon. */
    const char *branch[] = {"checkout", "-b", "other"};
    T_OK(fx_git_ok(&L.fx, fx_repo(&L.fx), branch, 3u, &err), &err);
    T_OK(fx_write(fx_repo(&L.fx), "on-other-branch.c", "1\n", &err), &err);
    T_CHECK_MSG(wait_events(&L, "on-other-branch.c", &err),
                "a change after a branch switch must be observed");

    atlas_buf_free(&out);
    live_stop(&L, false);
}

static void test_watcher_index_change(void) {
    live L;
    atlas_err err;
    atlas_err_init(&err);
    live_start(&L, &err);
    T_CHECK(wait_events(&L, "\"kind\":\"reconciled\"", &err));

    /* Staging touches .git/index, not the working tree. The git directory is
     * watched separately for exactly this. */
    T_OK(fx_write(fx_repo(&L.fx), "staged.c", "1\n", &err), &err);
    T_OK(fx_add_all(&L.fx, fx_repo(&L.fx), &err), &err);
    T_CHECK_MSG(wait_events(&L, "staged.c", &err), "a staged file must be observed");

    live_stop(&L, false);
}

static void test_burst_is_coalesced(void) {
    live L;
    atlas_err err;
    atlas_err_init(&err);
    live_start(&L, &err);
    T_CHECK(wait_events(&L, "\"kind\":\"reconciled\"", &err));

    atlas_buf before = ATLAS_BUF_INIT;
    int code = -1;
    const char *status[] = {"daemon", "status", "--json"};
    cli(&L, status, 3u, &before, &code, &err);

    /* Fifty writes in a burst. Debouncing must turn these into a small number of
     * passes, not fifty: an indexer that reconciles per keystroke is unusable. */
    for (int i = 0; i < 50; i++) {
        char name[32];
        (void)snprintf(name, sizeof(name), "burst%02d.c", i);
        T_OK(fx_write(fx_repo(&L.fx), name, "x\n", &err), &err);
    }
    T_CHECK(wait_events(&L, "burst49.c", &err));

    atlas_buf log = ATLAS_BUF_INIT;
    T_OK(fx_daemon_log(&L.d, &log, &err), &err);
    int passes = 0;
    for (const char *p = atlas_buf_cstr(&log); (p = strstr(p, "reconciled fixture")) != NULL;
         p += 18) {
        passes++;
    }
    T_CHECK_MSG(passes <= 8, "fifty writes should coalesce into a handful of passes, saw %d",
                passes);
    /* And the last write really is in the index, so coalescing did not lose it. */
    T_CHECK(wait_events(&L, "burst49.c", &err));

    atlas_buf_free(&before);
    atlas_buf_free(&log);
    live_stop(&L, false);
}

static void test_linked_worktrees_are_independent(void) {
    live L;
    atlas_err err;
    atlas_err_init(&err);
    live_start(&L, &err);
    T_CHECK(wait_events(&L, "\"kind\":\"reconciled\"", &err));

    /* A second worktree of the same repository: same object store, own HEAD, own
     * working tree. Changes in one must not be attributed to the other. */
    atlas_buf wt = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&wt, &err, "%s/wt2", atlas_buf_cstr(&L.fx.root)), &err);
    const char *addwt[] = {"worktree", "add", "-b", "second", atlas_buf_cstr(&wt)};
    atlas_err werr;
    atlas_err_init(&werr);
    if (fx_git_ok(&L.fx, fx_repo(&L.fx), addwt, 5u, &werr) != ATLAS_OK) {
        atlas_test_note("this git cannot create a linked worktree; skipping");
        atlas_buf_free(&wt);
        live_stop(&L, false);
        return;
    }

    const char *reg[] = {"repo", "add", atlas_buf_cstr(&wt), "--name", "second"};
    atlas_buf out = ATLAS_BUF_INIT;
    int code = -1;
    cli(&L, reg, 5u, &out, &code, &err);
    T_EQ_INT(code, 0);

    /* Registered while the daemon was running: it must be watched without a
     * restart. */
    T_OK(fx_write(atlas_buf_cstr(&wt), "only-in-second.c", "1\n", &err), &err);
    const char *ev2[] = {"events", "second", "--json", "--limit", "200"};
    bool found = false;
    T_OK(fx_wait_for_substring(&L.fx, &L.d, ev2, 5u, "only-in-second.c", WAIT_MS, &found, &err),
         &err);
    T_CHECK_MSG(found, "a worktree registered while the daemon runs must be watched");

    /* And the first worktree's journal must not claim that file. */
    atlas_buf_reset(&out);
    const char *ev1[] = {"events", "fixture", "--json", "--limit", "500"};
    cli(&L, ev1, 5u, &out, &code, &err);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "only-in-second.c") == NULL,
                "one worktree's change must not appear in another's journal");

    atlas_buf_free(&out);
    atlas_buf_free(&wt);
    live_stop(&L, false);
}

/* Regression: the recursive watch installer held a pointer into the buffer it
 * was appending child directories to, so the buffer's first reallocation left it
 * dangling and the walk stopped early. The visible symptom was a repository
 * whose deeper directories were silently not watched — which reads as "the
 * watcher is a bit slow" rather than as a bug, and which only shows up once a
 * repository is big enough to force a realloc.
 *
 * The assertion is on the watch count, not on a change being noticed, because a
 * change in an unwatched directory is still found by the periodic pass; the
 * count is what distinguishes "watched" from "eventually reconciled". */
static void test_watch_tree_is_not_truncated(void) {
    fixture fx;
    fx_daemon d;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);
    T_OK(fx_init_repo(&fx, fx_repo(&fx), NULL, &err), &err);

    /* Enough directories, with long enough names, to force the walk's buffer to
     * grow several times. */
    const int DIRS = 60;
    for (int i = 0; i < DIRS; i++) {
        char dir[128];
        char file[160];
        (void)snprintf(dir, sizeof(dir), "directory-with-a-long-name-%03d", i);
        T_OK(fx_mkdir(fx_repo(&fx), dir, &err), &err);
        (void)snprintf(file, sizeof(file), "%s/f.c", dir);
        T_OK(fx_write(fx_repo(&fx), file, "1\n", &err), &err);
    }
    T_OK(fx_add_all(&fx, fx_repo(&fx), &err), &err);
    T_OK(fx_commit(&fx, fx_repo(&fx), "many directories", &err), &err);

    const char *add[] = {"--data-dir", fx_data_dir(&fx), "repo", "add", fx_repo(&fx), "--name",
                         "wide"};
    int code = 0;
    T_OK(fx_atlas(add, 7u, NULL, NULL, &code, &err), &err);
    T_EQ_INT(code, 0);

    fx_daemon_init(&d);
    T_OK(fx_daemon_start(&fx, &d, &err), &err);
    T_OK(fx_daemon_wait_ready(&d, WAIT_MS, &err), &err);

    /* Wait for the watch set to be installed, then read the count. */
    const char *status[] = {"daemon", "status", "--json"};
    bool ready = false;
    T_OK(fx_wait_for_substring(&fx, &d, status, 3u, "\"watching\":1", WAIT_MS, &ready, &err), &err);
    T_CHECK(ready);

    atlas_buf out = ATLAS_BUF_INIT;
    const char *state[] = {"events", "wide", "--json", "--limit", "1"};
    T_OK(fx_atlas_with_runtime(&fx, &d, state, 5u, &out, NULL, &code, &err), &err);

    const char *p = strstr(atlas_buf_cstr(&out), "\"watched_directories\":");
    T_REQUIRE(p != NULL);
    long watched = strtol(p + strlen("\"watched_directories\":"), NULL, 10);
    /* Root plus every subdirectory, plus the git metadata watches. Asserting a
     * floor rather than an exact number keeps this robust against git laying out
     * .git differently, while still failing hard on a truncated walk — the bug
     * produced 6. */
    T_CHECK_MSG(watched >= DIRS + 1,
                "expected at least %d watches for %d directories, got %ld", DIRS + 1, DIRS,
                watched);

    atlas_buf_free(&out);
    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    fx_close(&fx);
}

/* The read-only guarantee, proven against the daemon rather than against a CLI
 * invocation.
 *
 * The smoke and adversarial suites prove that one-shot commands do not modify a
 * repository. A1 introduces something they do not cover: a process that watches
 * and re-reads a repository continuously, for as long as it is running. That is
 * a different exposure — more reads, more git invocations, and a worker pool
 * touching files concurrently — so it gets its own proof.
 *
 * The digest covers relative paths, entry types, permission bits, symlink
 * targets and file contents, across the working tree *and* .git. */
static void test_daemon_never_modifies_the_repository(void) {
    live L;
    atlas_err err;
    atlas_err_init(&err);
    live_start(&L, &err);

    /* Let the startup pass complete, so the digest is taken after the daemon has
     * actually read everything rather than before it started. */
    T_CHECK(wait_events(&L, "\"kind\":\"reconciled\"", &err));

    char before[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(fx_repo(&L.fx), before, &err), &err);

    /* Exercise the paths that read the most: a full reconciliation, a staging
     * change that makes the watcher re-read the git directory, a branch switch,
     * and a commit. */
    atlas_buf out = ATLAS_BUF_INIT;
    int code = -1;
    const char *sync[] = {"sync", "fixture", "--wait", "--full"};
    cli(&L, sync, 4u, &out, &code, &err);
    T_EQ_INT(code, 0);

    const char *state[] = {"events", "fixture", "--json", "--limit", "50"};
    cli(&L, state, 5u, &out, &code, &err);
    const char *status[] = {"daemon", "status", "--json"};
    cli(&L, status, 3u, &out, &code, &err);

    /* The digest is taken again immediately after a second full pass, with no
     * intervening fixture write, so any difference is Atlas' doing. */
    cli(&L, sync, 4u, &out, &code, &err);
    T_EQ_INT(code, 0);

    char after[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(fx_repo(&L.fx), after, &err), &err);
    T_CHECK_MSG(strcmp(before, after) == 0,
                "the daemon modified the repository it was watching:\n  before %s\n  after  %s",
                before, after);

    atlas_buf_free(&out);
    live_stop(&L, false);
}

/* --- the ctime defect, end to end ---------------------------------------
 *
 * A same-length in-place edit whose mtime is restored to the nanosecond leaves
 * device, inode, size, mode and mtime identical. Only ctime moves, and nothing
 * in userspace can put it back. These two cases cover the two ways such an edit
 * can reach a running system: while nothing was watching, and while it was. */

/* Overwrites `rel` with same-length different bytes and restores its mtime. */
static void rewrite_restoring_mtime(const char *dir, const char *rel, const char *bytes,
                                    atlas_err *err) {
    atlas_buf path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&path, err, "%s/%s", dir, rel), err);

    struct stat before;
    T_REQUIRE(lstat(atlas_buf_cstr(&path), &before) == 0);
    T_REQUIRE((size_t)before.st_size == strlen(bytes));

    int fd = open(atlas_buf_cstr(&path), O_WRONLY);
    T_REQUIRE(fd >= 0);
    T_REQUIRE(write(fd, bytes, strlen(bytes)) == (ssize_t)strlen(bytes));
    T_REQUIRE(close(fd) == 0);

    struct timespec times[2] = {before.st_atim, before.st_mtim};
    T_REQUIRE(utimensat(AT_FDCWD, atlas_buf_cstr(&path), times, AT_SYMLINK_NOFOLLOW) == 0);

    struct stat after;
    T_REQUIRE(lstat(atlas_buf_cstr(&path), &after) == 0);
    T_EQ_INT(after.st_size, before.st_size);
    T_EQ_INT(after.st_mtim.tv_sec, before.st_mtim.tv_sec);
    T_EQ_INT(after.st_mtim.tv_nsec, before.st_mtim.tv_nsec);
    T_EQ_INT(after.st_ino, before.st_ino);
    atlas_buf_free(&path);
}

/* Pulls the recorded content hash for a path out of `atlas file --json`. */
static void hash_of(live *L, const char *path, atlas_buf *out, atlas_err *err) {
    atlas_buf doc = ATLAS_BUF_INIT;
    int code = -1;
    const char *args[] = {"file", "fixture", path, "--json"};
    cli(L, args, 4u, &doc, &code, err);
    T_EQ_INT(code, 0);
    const char *p = strstr(atlas_buf_cstr(&doc), "\"content_hash\":\"");
    T_REQUIRE(p != NULL);
    p += strlen("\"content_hash\":\"");
    const char *end = strchr(p, '"');
    T_REQUIRE(end != NULL);
    T_OK(atlas_buf_set(out, p, (size_t)(end - p), err), err);
    atlas_buf_free(&doc);
}

/* The case in the finding: the edit happens while the daemon is stopped, so no
 * event is ever generated and the metadata tuple is the only evidence there is.
 * The startup pass verifies content, so it is caught anyway. */
static void test_offline_same_length_edit_is_caught_on_restart(void) {
    live L;
    atlas_err err;
    atlas_err_init(&err);
    live_start(&L, &err);
    T_CHECK(wait_events(&L, "\"kind\":\"reconciled\"", &err));

    /* A file long enough that a same-length rewrite is meaningfully different. */
    T_OK(fx_write(fx_repo(&L.fx), "payload.c", "const char k[] = \"AAAAAAAA\";\n", &err), &err);
    T_CHECK(wait_events(&L, "payload.c", &err));

    atlas_buf before = ATLAS_BUF_INIT;
    hash_of(&L, "payload.c", &before, &err);
    T_CHECK(before.len > 0);

    /* Stop the daemon, then edit behind its back. */
    fx_daemon_stop(&L.d, false);
    rewrite_restoring_mtime(fx_repo(&L.fx), "payload.c", "const char k[] = \"BBBBBBBB\";\n", &err);

    T_OK(fx_daemon_start(&L.fx, &L.d, &err), &err);
    T_OK(fx_daemon_wait_ready(&L.d, WAIT_MS, &err), &err);

    /* The startup pass verifies content, so the change is found even though every
     * field of the old identity except ctime is unchanged. */
    atlas_buf after = ATLAS_BUF_INIT;
    bool changed = false;
    for (int waited = 0; waited < WAIT_MS && !changed; waited += 200) {
        struct timespec nap = {0, 200L * 1000000L};
        (void)nanosleep(&nap, NULL);
        atlas_buf_reset(&after);
        hash_of(&L, "payload.c", &after, &err);
        changed = (strcmp(atlas_buf_cstr(&before), atlas_buf_cstr(&after)) != 0);
    }
    T_CHECK_MSG(changed,
                "a same-length offline edit with the mtime restored must be caught on restart:\n"
                "  before %s\n  after  %s",
                atlas_buf_cstr(&before), atlas_buf_cstr(&after));

    /* And the generation advanced, so a consumer watching the cursor sees it. */
    atlas_buf state = ATLAS_BUF_INIT;
    int code = -1;
    const char *ev[] = {"events", "fixture", "--json", "--limit", "1"};
    cli(&L, ev, 5u, &state, &code, &err);
    const char *g = strstr(atlas_buf_cstr(&state), "\"last_complete_generation\":");
    T_REQUIRE(g != NULL);
    T_CHECK(strtol(g + strlen("\"last_complete_generation\":"), NULL, 10) > 0);

    atlas_buf_free(&state);
    atlas_buf_free(&before);
    atlas_buf_free(&after);
    live_stop(&L, false);
}

/* The same edit while the daemon *is* watching. Here inotify reports it, and the
 * event must force a read even though the metadata argues nothing happened. */
static void test_live_same_length_edit_is_forced_by_the_event(void) {
    live L;
    atlas_err err;
    atlas_err_init(&err);
    live_start(&L, &err);
    T_CHECK(wait_events(&L, "\"kind\":\"reconciled\"", &err));

    T_OK(fx_write(fx_repo(&L.fx), "live.c", "const char k[] = \"CCCCCCCC\";\n", &err), &err);
    T_CHECK(wait_events(&L, "live.c", &err));

    atlas_buf before = ATLAS_BUF_INIT;
    hash_of(&L, "live.c", &before, &err);

    /* Let the debounce settle so this is a fresh event rather than part of the
     * previous burst. */
    struct timespec settle = {1, 0};
    (void)nanosleep(&settle, NULL);

    rewrite_restoring_mtime(fx_repo(&L.fx), "live.c", "const char k[] = \"DDDDDDDD\";\n", &err);

    atlas_buf after = ATLAS_BUF_INIT;
    bool changed = false;
    for (int waited = 0; waited < WAIT_MS && !changed; waited += 200) {
        struct timespec nap = {0, 200L * 1000000L};
        (void)nanosleep(&nap, NULL);
        atlas_buf_reset(&after);
        hash_of(&L, "live.c", &after, &err);
        changed = (strcmp(atlas_buf_cstr(&before), atlas_buf_cstr(&after)) != 0);
    }
    T_CHECK_MSG(changed,
                "an observed event must force a read even when the metadata is unchanged:\n"
                "  before %s\n  after  %s",
                atlas_buf_cstr(&before), atlas_buf_cstr(&after));

    atlas_buf_free(&before);
    atlas_buf_free(&after);
    live_stop(&L, false);
}

/* An unclean shutdown means changes may have happened unobserved, so the index
 * must not describe itself as current until a content-verifying pass completes. */
static void test_unclean_shutdown_marks_the_index_not_current(void) {
    live L;
    atlas_err err;
    atlas_err_init(&err);
    live_start(&L, &err);
    T_CHECK(wait_events(&L, "\"kind\":\"reconciled\"", &err));

    fx_daemon_stop(&L.d, true); /* SIGKILL: no chance to record a clean stop */
    T_OK(fx_daemon_start(&L.fx, &L.d, &err), &err);
    T_OK(fx_daemon_wait_ready(&L.d, WAIT_MS, &err), &err);

    atlas_buf log = ATLAS_BUF_INIT;
    bool noted = false;
    for (int waited = 0; waited < WAIT_MS && !noted; waited += 200) {
        struct timespec nap = {0, 200L * 1000000L};
        (void)nanosleep(&nap, NULL);
        atlas_buf_reset(&log);
        T_OK(fx_daemon_log(&L.d, &log, &err), &err);
        noted = strstr(atlas_buf_cstr(&log), "did not shut down cleanly") != NULL;
    }
    T_CHECK_MSG(noted, "an unclean previous shutdown must be detected and reported");

    /* The recovery pass verifies content and then clears the gap, so the index
     * becomes current again rather than staying degraded forever. */
    const char *ev[] = {"events", "fixture", "--json", "--limit", "1"};
    bool current = false;
    T_OK(fx_wait_for_substring(&L.fx, &L.d, ev, 5u, "\"index_current\":true", WAIT_MS, &current,
                               &err),
         &err);
    T_CHECK_MSG(current, "a completed content-verifying recovery pass must clear the gap");

    atlas_buf_free(&log);
    live_stop(&L, false);
}

static void test_repo_remove_through_the_daemon(void) {
    live L;
    atlas_err err;
    atlas_err_init(&err);
    live_start(&L, &err);

    atlas_buf out = ATLAS_BUF_INIT;
    int code = -1;
    const char *rm[] = {"repo", "remove", "fixture", "--yes", "--json"};
    cli(&L, rm, 5u, &out, &code, &err);
    T_EQ_INT(code, 0);

    atlas_buf_reset(&out);
    const char *list[] = {"repo", "list", "--json"};
    cli(&L, list, 3u, &out, &code, &err);
    T_CHECK(strstr(atlas_buf_cstr(&out), "\"count\":0") != NULL);

    /* The target repository is untouched: only Atlas metadata was removed. */
    struct stat sb;
    atlas_buf f = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&f, &err, "%s/a.c", fx_repo(&L.fx)), &err);
    T_CHECK(stat(atlas_buf_cstr(&f), &sb) == 0);

    atlas_buf_free(&f);
    atlas_buf_free(&out);
    live_stop(&L, false);
}

static const atlas_test TESTS[] = {
    {"start, ping, status, graceful stop", test_start_ping_status_stop},
    {"restart after a crash reconciles what it missed", test_restart_after_crash},
    {"a second daemon is refused", test_second_daemon_is_refused},
    {"mutations route through the daemon; a second writer is refused",
     test_offline_writer_refused_while_daemon_runs},
    {"malformed frames are answered, not fatal", test_malformed_frames},
    {"a slow client does not block the others", test_slow_client_and_many_clients},
    {"the watcher sees create, modify and delete", test_watcher_create_modify_delete},
    {"the watcher sees atomic saves and renames", test_watcher_atomic_save_and_rename},
    {"a new nested directory is indexed per file", test_watcher_nested_new_directory},
    {"ignore rules are honoured and a branch switch is followed",
     test_watcher_gitignore_and_branch_switch},
    {"staging the index is observed", test_watcher_index_change},
    {"a burst of writes is coalesced", test_burst_is_coalesced},
    {"linked worktrees are watched independently", test_linked_worktrees_are_independent},
    {"the watch tree is not truncated by a buffer growth", test_watch_tree_is_not_truncated},
    {"the daemon never modifies the repository it watches",
     test_daemon_never_modifies_the_repository},
    /* The ctime defect, end to end. */
    {"a same-length offline edit with a restored mtime is caught on restart",
     test_offline_same_length_edit_is_caught_on_restart},
    {"a watched same-length edit is forced by its event",
     test_live_same_length_edit_is_forced_by_the_event},
    {"an unclean shutdown is detected and recovered from",
     test_unclean_shutdown_marks_the_index_not_current},
    {"repo remove routes through the daemon", test_repo_remove_through_the_daemon},
};

ATLAS_TEST_MAIN("daemon", TESTS)
