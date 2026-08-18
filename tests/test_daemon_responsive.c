/* Atlas - A9.2.6: the daemon stays answerable while it is busy.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Driven against a live daemon, because the defect this closes is not in any
 * function and no unit test could see it. Every piece behaved as written: the
 * serve loop dispatched one request at a time, as designed; a synchronous write
 * waited for the writer with a timeout, as designed; and a semantic pass ran on
 * the writer thread, as designed. The fault was in the join. A9.2.4 put an
 * automatic, minutes-long pass on the writer's queue, and from that moment a
 * hook's session write queued behind one sat out its whole four-second budget
 * and then failed — while `daemon.ping` and every other client sat behind *it*,
 * because the serve loop was inside that wait and not in `poll`.
 *
 * Measured on the Atlas repository before the fix: ping 26 ms idle, 3.9 s for
 * every write that arrived during a pass, and the write itself failing after
 * 4027 ms. A stack sample showed the whole story in two frames —
 * `atlas_server_serve -> atlas_server_dispatch -> method_session_open ->
 * atlas_writer_ai -> pthread_cond_timedwait` on the main thread, and
 * `atlas_sem_index_on -> atlas_sem_parse_unit` on the writer.
 *
 * So what is asserted here is the join, not the parts:
 *
 *   1. **A write that arrives during a semantic pass is answered quickly**, with
 *      a refusal that says nothing was queued, rather than being made to wait.
 *      On a single-threaded serve loop the duration of that answer *is* the
 *      stall every other client suffers, which is why it is timed.
 *   2. **Ping and an independent read stay fast while the pass runs**, including
 *      while a write is blocked concurrently — the head-of-line case, driven
 *      from a second process because that is the only way to have two clients.
 *   3. **The pass still completes**, the daemon is still serving afterwards, and
 *      the index it built is real. A daemon made responsive by breaking its own
 *      maintenance would pass 1 and 2.
 *   4. **A hook still fails open.** It is the caller that meets this refusal
 *      most often, and it must go on exiting 0 with an envelope.
 *
 * Every wait is for an observable outcome with a deadline, never a guessed
 * sleep. The one timing assertion is deliberately far looser than the figures
 * above: it must separate "answered" from "waited out a four-second budget" on a
 * loaded machine, and nothing finer than that is a property Atlas holds.
 */
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "atlas/hook.h"
#include "atlas/limits.h"
#include "atlas_test.h"
#include "support/fixture.h"

/* Generous: a semantic index runs a compiler over every unit in the fixture,
 * and the sweep that schedules it runs on its own interval. */
#define WAIT_MS 240000

/* The bound the season exists to hold, with room for a machine under load.
 *
 * It is not a performance figure and must not be read as one. It exists to
 * separate two outcomes that differ by an order of magnitude — an answer, or a
 * client made to wait out `AI_WRITE_TIMEOUT_MS` — and tightening it towards the
 * observed milliseconds would turn a real regression gate into a flaky one. */
#define RESPONSIVE_MS 2500

/* Enough translation units that the pass is still running when the assertions
 * are made, each pulling in a header heavy enough that parsing it is real work.
 * A fixture that finishes before anything can observe it would let this whole
 * file pass without testing anything, which is why the pass being observed at
 * all is itself asserted below. */
#define TU_COUNT 160

typedef struct live {
    fixture fx;
    fx_daemon d;
    atlas_buf runtime_env;
    atlas_buf data_env;
} live;

static int64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* A repository whose semantic index takes a while to build.
 *
 * The header is included by every unit and includes three standard headers of
 * its own, so each unit is a real parse rather than a token or two. Nothing here
 * is Atlas-specific and nothing is named after this repository. */
static void build_repo(live *L, atlas_err *err) {
    T_OK(fx_mkdir(fx_repo(&L->fx), "include", err), err);
    T_OK(fx_write(fx_repo(&L->fx), "include/wide.h",
                  "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
                  "#include <math.h>\n#include <time.h>\n#include <ctype.h>\n"
                  "#include <errno.h>\n#include <signal.h>\n#include <stdint.h>\n"
                  "int shared_entry(int);\n",
                  err),
         err);

    atlas_buf doc = ATLAS_BUF_INIT;
    T_OK(atlas_buf_append_str(&doc, "[", err), err);
    for (size_t i = 0; i < TU_COUNT; i++) {
        char name[64];
        (void)snprintf(name, sizeof name, "unit%02zu.c", i);

        atlas_buf body = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&body, err, "#include \"wide.h\"\n"), err);
        /* A handful of functions per unit, so the graph the pass publishes is
         * worth publishing and the parse is not trivially short. */
        for (size_t k = 0; k < 24u; k++) {
            T_OK(atlas_buf_appendf(&body, err,
                                   "static int u%02zu_h%02zu(int x){return x + %zu;}\n", i, k, k),
                 err);
        }
        T_OK(atlas_buf_appendf(&body, err, "int u%02zu_entry(int x){return u%02zu_h00(x);}\n", i, i),
             err);
        T_OK(fx_write(fx_repo(&L->fx), name, atlas_buf_cstr(&body), err), err);
        atlas_buf_free(&body);

        T_OK(atlas_buf_appendf(&doc, err,
                               "%s{\"directory\":\"%s\",\"arguments\":[\"cc\",\"-I\",\"include\","
                               "\"-std=gnu11\",\"-c\",\"%s\"],\"file\":\"%s\"}",
                               i == 0 ? "" : ",", fx_repo(&L->fx), name, name),
             err);
    }
    T_OK(atlas_buf_append_str(&doc, "]", err), err);
    T_OK(fx_write(fx_repo(&L->fx), "compile_commands.json", atlas_buf_cstr(&doc), err), err);
    atlas_buf_free(&doc);
}

/* Enables automatic rebuild before any daemon exists — the local path, because
 * `code.sem_config` is in the operator-uid group and a fixture cannot have a
 * root-owned policy naming it. See `tests/test_sem_auto.c`, which asserts that
 * refusal rather than working around it. */
static void enable_auto(live *L, atlas_err *err) {
    const char *args[] = {"--data-dir",            fx_data_dir(&L->fx),
                          "code",                  "sem-config",
                          "fixture",               "--compdb",
                          "compile_commands.json", "--auto"};
    int code = 0;
    atlas_buf out = ATLAS_BUF_INIT;
    T_OK(fx_atlas(args, 8u, &out, NULL, &code, err), err);
    T_CHECK_MSG(code == 0, "enabling automatic rebuild failed: %s", atlas_buf_cstr(&out));
    atlas_buf_free(&out);
}

static void live_start(live *L, atlas_err *err) {
    atlas_buf_init(&L->runtime_env);
    atlas_buf_init(&L->data_env);
    T_OK(fx_open(&L->fx, err), err);
    T_OK(fx_init_repo(&L->fx, fx_repo(&L->fx), NULL, err), err);
    build_repo(L, err);
    T_OK(fx_add_all(&L->fx, fx_repo(&L->fx), err), err);
    T_OK(fx_commit(&L->fx, fx_repo(&L->fx), "first", err), err);

    const char *add[] = {"--data-dir", fx_data_dir(&L->fx), "repo", "add", fx_repo(&L->fx),
                         "--name",     "fixture"};
    int code = 0;
    T_OK(fx_atlas(add, 7u, NULL, NULL, &code, err), err);
    T_EQ_INT(code, 0);

    enable_auto(L, err);

    fx_daemon_init(&L->d);
    T_OK(fx_daemon_start(&L->fx, &L->d, err), err);
    T_OK(fx_daemon_wait_ready(&L->d, WAIT_MS, err), err);

    T_OK(atlas_buf_appendf(&L->runtime_env, err, "XDG_RUNTIME_DIR=%s",
                           atlas_buf_cstr(&L->d.runtime_dir)),
         err);
    T_OK(atlas_buf_appendf(&L->data_env, err, "ATLAS_DATA_DIR=%s", fx_data_dir(&L->fx)), err);
}

static void live_stop(live *L) {
    fx_daemon_stop(&L->d, false);
    fx_daemon_free(&L->d);
    atlas_buf_free(&L->runtime_env);
    atlas_buf_free(&L->data_env);
    fx_close(&L->fx);
}

/* One `SessionStart` hook, through the real transport, timed.
 *
 * The hook is the caller that meets a busy daemon most often — Claude Code fires
 * one on every event — and it is also the caller for which a four-second stall
 * is worst, because it stalls whatever fired it. */
static int64_t run_hook(live *L, atlas_buf *errout, int *code, atlas_err *err) {
    const char *env[] = {atlas_buf_cstr(&L->runtime_env), atlas_buf_cstr(&L->data_env), NULL};
    atlas_buf payload = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&payload, err,
                           "{\"session_id\":\"responsive-1\",\"cwd\":\"%s\","
                           "\"hook_event_name\":\"SessionStart\",\"source\":\"startup\"}",
                           fx_repo(&L->fx)),
         err);
    const char *args[] = {"hook", "SessionStart"};
    atlas_buf out = ATLAS_BUF_INIT;
    int64_t t0 = now_ms();
    T_OK(fx_atlas_stdin(args, 2u, env, payload.data, payload.len, &out, errout, code, err), err);
    int64_t took = now_ms() - t0;
    atlas_buf_free(&out);
    atlas_buf_free(&payload);
    return took;
}

/* `atlas daemon ping`, timed. Touches no database and takes no lock, so any time
 * it spends is time the serve loop was not in `poll`. */
static int64_t run_ping(live *L, int *code, atlas_err *err) {
    const char *args[] = {"daemon", "ping"};
    int64_t t0 = now_ms();
    T_OK(fx_atlas_with_runtime(&L->fx, &L->d, args, 2u, NULL, NULL, code, err), err);
    return now_ms() - t0;
}

/* An ordinary read, timed. Independent of the write path and of the pass. */
static int64_t run_read(live *L, atlas_buf *out, int *code, atlas_err *err) {
    const char *args[] = {"status", "fixture", "--json"};
    int64_t t0 = now_ms();
    T_OK(fx_atlas_with_runtime(&L->fx, &L->d, args, 3u, out, NULL, code, err), err);
    return now_ms() - t0;
}

/* One fetch of the semantic status, answering both questions the loops ask.
 *
 * A pass in flight is **not** `"activity":"BUILDING"` when there is no published
 * generation yet: with nothing to be current *about*, the activity is
 * UNAVAILABLE and the hold reason is what says a build is running. Watching only
 * the activity would have waited for a state this fixture never passes through —
 * it starts from no index at all, which is the case the season's defect lived
 * in. Both are accepted, so the fixture may start from either. */
static void sem_status(live *L, bool *building, bool *current, atlas_err *err) {
    const char *args[] = {"code", "sem-status", "fixture", "--json"};
    atlas_buf out = ATLAS_BUF_INIT;
    int code = 0;
    T_OK(fx_atlas_with_runtime(&L->fx, &L->d, args, 4u, &out, NULL, &code, err), err);
    const char *text = atlas_buf_cstr(&out);
    *current = (code == 0) && strstr(text, "\"activity\":\"CURRENT\"") != NULL;
    *building = (code == 0) && (strstr(text, "\"activity\":\"BUILDING\"") != NULL ||
                                strstr(text, "a_generation_is_already_being_built") != NULL);
    atlas_buf_free(&out);
}

/* --- the gate ------------------------------------------------------------- */

static void test_a_semantic_pass_does_not_stall_the_serve_loop(void) {
    atlas_err err;
    atlas_err_init(&err);
    live L;
    live_start(&L, &err);

    bool saw_busy = false;
    bool observed_building = false;
    int64_t worst_ping = 0;
    int64_t worst_read = 0;
    int64_t worst_write = 0;

    /* The daemon schedules the build itself; nobody runs a command to start it.
     * The loop drives a write, a ping and a read until the pass has finished,
     * so whatever window the machine gives is used rather than guessed at.
     *
     * The timings are kept only for iterations in which a pass was actually in
     * flight, and that is the claim rather than a convenience. The daemon's
     * first act on a fresh fixture is a reconciliation, which is deliberately
     * *not* treated as unbounded — an incremental pass finishes well inside
     * every timeout here, and refusing writes during one would drop hook records
     * that would otherwise have succeeded, because hooks fail open. So a write
     * can still wait behind a first full reconciliation of a large tree, most
     * visibly under a sanitiser build, and asserting otherwise would be
     * asserting something Atlas does not claim. What it claims is this: **a
     * semantic pass does not stall the serve loop.** */
    int64_t deadline = now_ms() + WAIT_MS;
    while (now_ms() < deadline) {
        bool building = false;
        bool current = false;
        sem_status(&L, &building, &current, &err);
        if (current) {
            break;
        }
        if (building) {
            observed_building = true;
        }

        /* The write. On a serve loop that dispatches one request at a time, the
         * time this takes is the time no other client is served — which is why
         * it is the measurement that matters, and why it is taken before the
         * ping rather than inferred from it. */
        atlas_buf errout = ATLAS_BUF_INIT;
        int hook_code = -1;
        int64_t wrote = run_hook(&L, &errout, &hook_code, &err);
        if (building && wrote > worst_write) {
            worst_write = wrote;
        }
        /* **A hook fails open, busy daemon or not.** */
        T_CHECK_MSG(hook_code == 0, "a hook did not fail open while the daemon was busy (exit %d)",
                    hook_code);
        if (strstr(atlas_buf_cstr(&errout), "BUSY:") != NULL) {
            saw_busy = true;
            T_CHECK_MSG(wrote < RESPONSIVE_MS,
                        "a write refused as busy still took %lld ms, so the serve loop was held",
                        (long long)wrote);
            /* The refusal must carry the claim that makes retrying safe. A
             * caller cannot distinguish "nothing ran" from "the result was
             * abandoned" by status code, so the message says which. */
            T_CHECK_MSG(strstr(atlas_buf_cstr(&errout), "Nothing was queued") != NULL,
                        "a busy refusal did not say the write had not been queued: %s",
                        atlas_buf_cstr(&errout));
        }
        atlas_buf_free(&errout);

        int ping_code = -1;
        int64_t pinged = run_ping(&L, &ping_code, &err);
        if (building && pinged > worst_ping) {
            worst_ping = pinged;
        }
        T_CHECK_MSG(ping_code == 0, "the daemon stopped answering ping while it was busy");

        atlas_buf state = ATLAS_BUF_INIT;
        int read_code = -1;
        int64_t red = run_read(&L, &state, &read_code, &err);
        if (building && red > worst_read) {
            worst_read = red;
        }
        T_CHECK_MSG(read_code == 0, "an independent read failed while the daemon was busy: %s",
                    atlas_buf_cstr(&state));
        atlas_buf_free(&state);
    }

    /* A fixture that finished before anything could observe it would let every
     * assertion above pass without testing anything. Saying so is the point:
     * a gate that cannot fail is worse than no gate. */
    T_CHECK_MSG(observed_building,
                "the semantic pass was never observed in flight, so this test proved nothing");
    T_CHECK_MSG(saw_busy,
                "no write ever met the busy path, so this test did not exercise the defect");

    T_CHECK_MSG(worst_ping < RESPONSIVE_MS, "the worst ping during a semantic pass was %lld ms",
                (long long)worst_ping);
    T_CHECK_MSG(worst_read < RESPONSIVE_MS,
                "the worst independent read during a semantic pass was %lld ms",
                (long long)worst_read);
    T_CHECK_MSG(worst_write < RESPONSIVE_MS,
                "the worst write during a semantic pass was %lld ms", (long long)worst_write);

    /* **The pass still finishes.** A daemon kept responsive by abandoning its
     * own maintenance would have satisfied everything above. */
    const char *args[] = {"code", "sem-status", "fixture", "--json"};
    bool found = false;
    T_OK(fx_wait_for_substring(&L.fx, &L.d, args, 4u, "\"activity\":\"CURRENT\"", WAIT_MS, &found,
                               &err),
         &err);
    T_CHECK_MSG(found, "the semantic pass never reached CURRENT");

    /* And the index it built is real, asked of the graph rather than of the
     * status line. */
    {
        const char *q[] = {"code", "semantic", "fixture", "u00_entry", "--json"};
        atlas_buf out = ATLAS_BUF_INIT;
        int code = 0;
        T_OK(fx_atlas_with_runtime(&L.fx, &L.d, q, 5u, &out, NULL, &code, &err), &err);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"name\":\"u00_entry\"") != NULL,
                    "the published index does not answer about a symbol it compiled: %s",
                    atlas_buf_cstr(&out));
        atlas_buf_free(&out);
    }

    /* Still serving, and now fast for the ordinary reason as well. */
    int code = 0;
    (void)run_ping(&L, &code, &err);
    T_EQ_INT(code, 0);

    live_stop(&L);
}

/* The head-of-line case, driven from two processes because one client cannot
 * demonstrate it.
 *
 * The child holds a write against the daemon while the parent pings. Before this
 * season the parent's pings queued behind the child's write for as long as that
 * write waited — measured at 3.9 s each — because the serve loop was inside the
 * wait rather than in `poll`. The child asserts nothing: a failure recorded in a
 * forked process is a failure nobody sees, so it reports only its exit status
 * and the parent decides. */
static void test_one_blocked_write_does_not_hold_the_other_clients(void) {
    atlas_err err;
    atlas_err_init(&err);
    live L;
    live_start(&L, &err);

    /* Only meaningful while the writer is occupied, so it waits for that rather
     * than racing it. */
    bool building = false;
    int64_t deadline = now_ms() + WAIT_MS;
    while (now_ms() < deadline) {
        bool current = false;
        sem_status(&L, &building, &current, &err);
        if (building || current) {
            break;
        }
    }
    T_CHECK_MSG(building, "the semantic pass was never observed in flight, so this proved nothing");

    pid_t child = fork();
    T_REQUIRE(child >= 0);
    if (child == 0) {
        atlas_err cerr;
        atlas_err_init(&cerr);
        atlas_buf errout = ATLAS_BUF_INIT;
        int code = -1;
        const char *env[] = {atlas_buf_cstr(&L.runtime_env), atlas_buf_cstr(&L.data_env), NULL};
        atlas_buf payload = ATLAS_BUF_INIT;
        (void)atlas_buf_appendf(&payload, &cerr,
                                "{\"session_id\":\"responsive-2\",\"cwd\":\"%s\","
                                "\"hook_event_name\":\"SessionStart\",\"source\":\"startup\"}",
                                fx_repo(&L.fx));
        const char *args[] = {"hook", "SessionStart"};
        atlas_buf out = ATLAS_BUF_INIT;
        (void)fx_atlas_stdin(args, 2u, env, payload.data, payload.len, &out, &errout, &code, &cerr);
        _exit(code == 0 ? 0 : 1);
    }

    /* Ping throughout the child's write. Every one of these is a second client
     * asking the daemon a question that touches nothing. */
    int64_t worst = 0;
    int status = 0;
    for (;;) {
        int code = -1;
        int64_t took = run_ping(&L, &code, &err);
        if (took > worst) {
            worst = took;
        }
        T_CHECK_MSG(code == 0, "the daemon stopped answering ping beside a blocked write");
        pid_t r = waitpid(child, &status, WNOHANG);
        if (r == child) {
            break;
        }
        T_REQUIRE(r == 0);
    }
    T_CHECK_MSG(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "the hook holding the write did not fail open");
    T_CHECK_MSG(worst < RESPONSIVE_MS,
                "a second client's ping took %lld ms beside a blocked write", (long long)worst);

    live_stop(&L);
}

static const atlas_test TESTS[] = {
    {"a semantic pass does not stall the serve loop",
     test_a_semantic_pass_does_not_stall_the_serve_loop},
    {"one blocked write does not hold the other clients",
     test_one_blocked_write_does_not_hold_the_other_clients},
};

ATLAS_TEST_MAIN("daemon_responsive", TESTS)
