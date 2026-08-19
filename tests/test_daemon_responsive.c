/* Atlas - A9.2.6 and A9.2.7: the daemon stays answerable while it is busy, and
 * a short write now lands while it is.
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
 * A9.2.7 then closed the half A9.2.6 left open. Answering quickly is not the
 * same as writing: for the whole of a pass *nothing else was written at all*,
 * and the bill for that is in `docs/backlog.md` — a recovery sweep refused every
 * twenty seconds for a pilot window, a submission that needed sixteen attempts
 * across forty-seven seconds, and one finished worker's completion lost. The
 * pass now hands the writer thread back between translation units and the
 * waiting write is served before it resumes.
 *
 * So what is asserted here is the join, not the parts:
 *
 *   1. **A write that arrives during a semantic pass lands, during the pass.**
 *      That is the season's proof, and it is asserted with the pass observed in
 *      flight on both sides of the write rather than inferred from a timing.
 *   2. **Ping and an independent read stay fast while the pass runs**, including
 *      while a write is outstanding concurrently — the head-of-line case, driven
 *      from a second process because that is the only way to have two clients.
 *   3. **The pass still completes and the generation it publishes is intact.**
 *      A daemon made responsive by breaking its own maintenance would pass 1 and
 *      2, and so would one whose interleaved write corrupted a half-built
 *      generation — which is why the unit accounting is read back afterwards
 *      rather than trusted.
 *   4. **A hook still fails open**, whatever answer it gets. It is the caller
 *      that meets a busy daemon most often, and it must go on exiting 0 with an
 *      envelope.
 *   5. **A refusal still stores nothing, and a retry still makes one row.**
 *      Driven in process, because a back-out is precisely "the write point was
 *      never reached" and that is what a refusal must be simulated as.
 *   6. **Both job classifications agree over the whole enum.**
 *
 * Every wait is for an observable outcome with a deadline, never a guessed
 * sleep. The timing assertions are deliberately far looser than the figures
 * above: they must separate "answered" from "waited out a four-second budget" on
 * a loaded machine, and nothing finer than that is a property Atlas holds.
 */
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

#include "atlas/datadir.h"
#include "atlas/db.h"
#include "atlas/hook.h"
#include "atlas/ipc.h"
#include "atlas/limits.h"
#include "atlas/verify.h"
#include "atlas/verify_ops.h"
#include "atlas_test.h"
#include "daemon/daemon_internal.h"
#include "db/db_internal.h"
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

/* What a *refused* write may cost, which is a different figure from what a
 * served one may cost and must not be conflated with it.
 *
 * A waiter that meets a non-yielding stretch now spends
 * `ATLAS_WRITER_YIELD_GRACE_MS` before it backs out — deliberately, because that
 * is what buys every other write the chance to be served instead. So a refusal
 * is bounded by the grace plus the same room for a loaded machine that
 * `RESPONSIVE_MS` allows, and holding a refusal to `RESPONSIVE_MS` would be
 * asserting the absence of the grace. What both bounds still exclude is the
 * outcome this file exists to catch: a caller made to wait out its whole
 * four-second budget. */
#define REFUSAL_MS (ATLAS_WRITER_YIELD_GRACE_MS + RESPONSIVE_MS)

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

    bool saw_served_write = false;
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
        /* A9.2.7 flipped which of these two branches is the ordinary one. A hook
         * write is `ATLAS_JOB_AI`, which a yield drains, so during a yielding
         * pass it is *served*. A refusal is now the exceptional path — it means
         * the waiter met a stretch with no yield in it, which is the residual
         * this season states rather than solves — so a refusal is still checked
         * for exactly what it must carry, and is no longer required to happen.
         * Requiring it would be requiring the residual. */
        if (strstr(atlas_buf_cstr(&errout), "BUSY:") != NULL) {
            T_CHECK_MSG(wrote < REFUSAL_MS,
                        "a write refused as busy still took %lld ms, so the serve loop was held",
                        (long long)wrote);
            /* The refusal must carry the claim that makes retrying safe. A
             * caller cannot distinguish "nothing ran" from "the result was
             * abandoned" by status code, so the message says which. */
            T_CHECK_MSG(strstr(atlas_buf_cstr(&errout), "Nothing was queued") != NULL,
                        "a busy refusal did not say the write had not been queued: %s",
                        atlas_buf_cstr(&errout));
        } else if (building) {
            saw_served_write = true;
            T_CHECK_MSG(wrote < RESPONSIVE_MS,
                        "a write served during a semantic pass took %lld ms", (long long)wrote);
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
    /* A9.2.7's replacement for "a write met the busy path". What has to happen
     * now is the opposite: at least one write reached the daemon while a pass
     * was in flight and was *taken*. `saw_busy` is recorded and deliberately not
     * required — a run in which no refusal occurred is a run in which the pass
     * yielded every time, which is the outcome, not a gap in the test. */
    T_CHECK_MSG(saw_served_write,
                "no write was served during a semantic pass, so the yield was never exercised");

    T_CHECK_MSG(worst_ping < RESPONSIVE_MS, "the worst ping during a semantic pass was %lld ms",
                (long long)worst_ping);
    T_CHECK_MSG(worst_read < RESPONSIVE_MS,
                "the worst independent read during a semantic pass was %lld ms",
                (long long)worst_read);
    /* Against the refusal bound rather than the served one, because this is the
     * worst of both kinds of answer and a refusal is entitled to the grace. */
    T_CHECK_MSG(worst_write < REFUSAL_MS,
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
 * The child issues a write against a daemon whose writer is occupied, while the
 * parent pings. Before A9.2.6 the parent's pings queued behind the child's write
 * for as long as that write waited — measured at 3.9 s each — because the serve
 * loop was inside the wait rather than in `poll`. A9.2.7 changed what happens to
 * the child's write, which is not what this case is about: whether the write is
 * served at a yield, refused after the grace, or waits, the *second* client must
 * keep being answered throughout. That is why the assertion here is about the
 * parent's pings and the child's exit status and about nothing else. The child
 * asserts nothing itself: a failure recorded in a forked process is a failure
 * nobody sees. */
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

/* --- O10: a refused submission is refused, not swallowed ------------------ */

/* The proposition under test, submitted over and over until it lands. Fixed
 * rather than varied per attempt, because the question at the end is how many
 * rows one proposition produced. */
#define BUSY_CLAIM_TEXT "the shared entry point is declared in exactly one header"

#define MCP_HANDSHAKE                                                                              \
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"                          \
    "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{}}}\n"                                  \
    "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"

/* One MCP script through the stdio adapter, timed, exactly as a model's client
 * sends it: JSON on stdin, JSON on stdout. */
static int64_t run_mcp(live *L, const char *script, atlas_buf *out, atlas_err *err) {
    const char *env[] = {atlas_buf_cstr(&L->runtime_env), atlas_buf_cstr(&L->data_env), NULL};
    const char *args[] = {"mcp"};
    int code = 0;
    atlas_buf_reset(out);
    int64_t t0 = now_ms();
    T_OK(fx_atlas_stdin(args, 1u, env, script, strlen(script), out, NULL, &code, err), err);
    int64_t took = now_ms() - t0;
    /* The adapter reports a refusal inside a successful reply, so a non-zero
     * exit here is the adapter failing rather than Atlas refusing. */
    T_EQ_INT(code, 0);
    return took;
}

/* Whether the reply carrying `id` says the tool ran and succeeded. A refusal is
 * `ok:false` in the structured body inside an otherwise healthy envelope, so
 * reading the envelope would score every refusal as a success. */
static bool mcp_tool_ok(const atlas_buf *out, const char *id_marker) {
    const char *p = strstr(atlas_buf_cstr(out), id_marker);
    if (p == NULL) {
        return false;
    }
    const char *line_end = strchr(p, '\n');
    size_t len = line_end != NULL ? (size_t)(line_end - p) : strlen(p);
    static const char OK[] = "\\\"ok\\\":true";
    const size_t oklen = sizeof OK - 1u;
    for (size_t i = 0; i + oklen <= len; i++) {
        if (memcmp(p + i, OK, oklen) == 0) {
            return true;
        }
    }
    return false;
}

/* Whether the claim is on the read surface right now. Asked over MCP rather than
 * of the file, because "is it stored?" and "can the caller find it?" are two
 * questions and only the second one is the one a client has. Reads do not touch
 * the writer, so this is answerable while a pass is running — which is the whole
 * reason the check can be made at the moment of the refusal rather than after. */
static bool claim_is_listed(live *L, const char *text, atlas_buf *out, atlas_err *err) {
    (void)run_mcp(L,
                  MCP_HANDSHAKE
                  "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":"
                  "{\"name\":\"atlas_verify_claims\",\"arguments\":{\"repo\":\"fixture\"}}}\n",
                  out, err);
    T_CHECK_MSG(mcp_tool_ok(out, "\"id\":9"), "the claims could not be listed: %s",
                atlas_buf_cstr(out));
    return strstr(atlas_buf_cstr(out), text) != NULL;
}

/* How many rows one proposition produced, asked of the file after the daemon
 * has stopped. Read-only and out of process on purpose: "what is stored" is a
 * different question from "what a client can see", and this is the first one. */
static int64_t stored_claim_rows(const char *data_dir, const char *text) {
    atlas_buf db_path = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_buf_appendf(&db_path, &err, "%s/atlas.db", data_dir), &err);
    sqlite3 *db = NULL;
    T_REQUIRE(sqlite3_open_v2(atlas_buf_cstr(&db_path), &db, SQLITE_OPEN_READONLY, NULL) ==
              SQLITE_OK);
    sqlite3_stmt *st = NULL;
    T_REQUIRE(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM verify_claims WHERE text = ?1;", -1,
                                 &st, NULL) == SQLITE_OK);
    T_REQUIRE(sqlite3_bind_text(st, 1, text, -1, SQLITE_STATIC) == SQLITE_OK);
    int64_t rows = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : -1;
    sqlite3_finalize(st);
    sqlite3_close(db);
    atlas_buf_free(&db_path);
    return rows;
}

/* **A9.2.7: a write that arrives during a semantic pass lands, during the pass.**
 *
 * This is the season's proof and it is the inverse of what stood here before.
 * A9.2.6 asserted that such a write was *refused* quickly, which was the whole
 * answer then and only half of one: a refusal is an answer, not a write, and for
 * the length of a pass every client got answers and nobody got a write. The
 * measured bill is in `docs/backlog.md` — a recovery sweep refused every twenty
 * seconds for a whole pilot window, a submission that needed sixteen attempts
 * across forty-seven seconds, and one finished worker's completion lost outright
 * because its driver ran out of retries against a walk that would not end.
 *
 * A verification submission rather than a hook, for the reason O10 chose one: a
 * hook record is metadata that fails open and is allowed to be lost, while a
 * claim exists nowhere but here — git cannot be re-read to recover one and no
 * pass rebuilds it. So it is the write for which "served or refused" actually
 * matters.
 *
 * Three things are asserted, and the third is the one that stops this being a
 * responsiveness test wearing a correctness test's name:
 *
 *   - the submission is **accepted**, with no `BUSY:` token anywhere in it;
 *   - the pass was **in flight on both sides of it** — checked before and after
 *     rather than assumed, because a submission accepted after the pass ended
 *     would prove nothing at all;
 *   - the pass then **publishes an intact generation**: every unit the fixture
 *     describes is in it and none failed. A write interleaved into a half-built
 *     generation is exactly the risk this mechanism takes, and the argument that
 *     it is safe — a generation is invisible until `atlas_db_sem_publish` — is
 *     asserted here rather than trusted. */
static void test_a_verification_write_lands_during_a_semantic_pass(void) {
    atlas_err err;
    atlas_err_init(&err);
    live L;
    live_start(&L, &err);

    bool observed_building = false;
    bool landed_during_pass = false;
    int64_t worst_submit = 0;
    atlas_buf last_other = ATLAS_BUF_INIT;
    /* The proposition that was accepted with the pass in flight on both sides of
     * it. Read back afterwards, so what is verified at the end is the row this
     * test actually caused rather than any row. */
    atlas_buf landed_text = ATLAS_BUF_INIT;

    atlas_buf out = ATLAS_BUF_INIT;
    atlas_buf script = ATLAS_BUF_INIT;
    int attempt = 0;
    int64_t deadline = now_ms() + WAIT_MS;
    while (now_ms() < deadline && !landed_during_pass) {
        bool building_before = false;
        bool current = false;
        sem_status(&L, &building_before, &current, &err);
        if (!building_before) {
            /* Nothing to submit *during* yet. A submission to an idle writer
             * would be accepted for reasons that have nothing to do with the
             * yield, so it is not made: the only interesting submission is one a
             * pass is holding the thread across. */
            if (current) {
                break; /* the pass ended; `observed_building` decides below */
            }
            continue;
        }
        observed_building = true;

        /* A distinct proposition every attempt, and that is load-bearing rather
         * than tidy. Intake is idempotent by content key, so resubmitting one
         * text would be accepted from then on without writing anything — and
         * "accepted" would stop being evidence that a write was served. */
        attempt++;
        char text[192];
        (void)snprintf(text, sizeof text, "%s (attempt %d)", BUSY_CLAIM_TEXT, attempt);
        atlas_buf_reset(&script);
        T_OK(atlas_buf_appendf(&script, &err,
                               MCP_HANDSHAKE
                               "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\",\"params\":"
                               "{\"name\":\"atlas_verify_claim_create\",\"arguments\":"
                               "{\"repo\":\"fixture\",\"text\":\"%s\","
                               "\"domain\":\"code\",\"actor\":\"a-model\","
                               "\"run\":\"a927-yield\"}}}\n",
                               text),
             &err);

        int64_t took = run_mcp(&L, atlas_buf_cstr(&script), &out, &err);
        if (took > worst_submit) {
            worst_submit = took;
        }
        bool accepted = mcp_tool_ok(&out, "\"id\":8");

        bool building_after = false;
        bool current_after = false;
        sem_status(&L, &building_after, &current_after, &err);

        if (accepted && building_after) {
            /* The claim the season makes: taken, not refused, with the pass
             * holding the writer on both sides of the submission. */
            T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "BUSY:") == NULL,
                        "an accepted submission still carried a busy token: %s",
                        atlas_buf_cstr(&out));
            T_CHECK_MSG(took < RESPONSIVE_MS,
                        "a submission served during a semantic pass took %lld ms",
                        (long long)took);
            T_OK(atlas_buf_set_str(&landed_text, text, &err), &err);
            landed_during_pass = true;
        } else if (strstr(atlas_buf_cstr(&out), "BUSY:") != NULL) {
            /* Still possible and still correct: a stretch with no yield in it —
             * one large translation unit — is the residual this season states
             * rather than solves. It is recorded, checked for the sentence that
             * makes a retry safe, and retried. */
            T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "Nothing was queued") != NULL,
                        "a busy refusal did not say the write had not been queued: %s",
                        atlas_buf_cstr(&out));
            T_CHECK_MSG(took < REFUSAL_MS, "a submission refused as busy took %lld ms",
                        (long long)took);
        } else if (!accepted) {
            /* Neither accepted nor refused as busy — a repository the daemon has
             * not finished indexing yet, most often. Documented behaviour rather
             * than a defect, so it is recorded and retried, and carried so that a
             * genuine refusal is reported instead of a bare "never accepted". */
            T_OK(atlas_buf_set(&last_other, out.data, out.len, &err), &err);
        }
    }
    atlas_buf_free(&script);

    /* A fixture that never made the daemon busy would let all of the above pass
     * without testing anything. */
    T_CHECK_MSG(observed_building,
                "the semantic pass was never observed in flight, so this test proved nothing");
    T_CHECK_MSG(landed_during_pass,
                "no submission was accepted while a semantic pass held the writer; last other "
                "answer: %s",
                last_other.len > 0 ? atlas_buf_cstr(&last_other) : "(none)");
    T_CHECK_MSG(worst_submit < REFUSAL_MS,
                "the worst submission during a semantic pass was %lld ms",
                (long long)worst_submit);

    /* **The pass still finishes.** A daemon kept responsive by abandoning its own
     * maintenance would have satisfied everything above. */
    const char *args[] = {"code", "sem-status", "fixture", "--json"};
    bool found = false;
    T_OK(fx_wait_for_substring(&L.fx, &L.d, args, 4u, "\"activity\":\"CURRENT\"", WAIT_MS, &found,
                               &err),
         &err);
    T_CHECK_MSG(found, "the semantic pass never reached CURRENT");

    /* **And the generation it published is whole.** The unit accounting is read
     * back rather than trusted: this is where an interleaved write corrupting a
     * half-built generation would show, and it is the only assertion here that
     * would survive somebody deleting the yield and leaving the refusal. */
    {
        atlas_buf status = ATLAS_BUF_INIT;
        int code = 0;
        T_OK(fx_atlas_with_runtime(&L.fx, &L.d, args, 4u, &status, NULL, &code, &err), &err);
        char want_total[64];
        (void)snprintf(want_total, sizeof want_total, "\"tu_total\":%d", (int)TU_COUNT);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&status), want_total) != NULL,
                    "the published generation does not hold every unit the fixture describes "
                    "(wanted %s): %s",
                    want_total, atlas_buf_cstr(&status));
        T_CHECK_MSG(strstr(atlas_buf_cstr(&status), "\"tu_failed\":0") != NULL,
                    "the published generation holds failed units: %s", atlas_buf_cstr(&status));
        atlas_buf_free(&status);
    }

    /* And the claim written mid-pass is where a client can find it, asked over
     * the surface a client actually has rather than of the file. "It was
     * accepted" and "a caller can read it back" are two claims and the second is
     * the one that would fail if a mid-pass write were applied to something the
     * publication then discarded. */
    {
        bool listed = claim_is_listed(&L, atlas_buf_cstr(&landed_text), &out, &err);
        T_CHECK_MSG(listed, "the claim accepted during the pass is not on the read surface: %s",
                    atlas_buf_cstr(&out));
    }

    /* And it is one row, asked of the file after the daemon has stopped. */
    fx_daemon_stop(&L.d, false);
    {
        int64_t rows = stored_claim_rows(fx_data_dir(&L.fx), atlas_buf_cstr(&landed_text));
        T_CHECK_MSG(rows == 1, "the proposition accepted during the pass produced %lld rows",
                    (long long)rows);
    }

    atlas_buf_free(&landed_text);
    atlas_buf_free(&out);
    atlas_buf_free(&last_other);
    live_stop(&L);
}

/* --- O10, carried forward: a refusal stores nothing ----------------------- */

/* A minimal database with one repository and one indexed head, which is all the
 * intake write point needs to bind a claim to a source state. The same shape
 * `tests/test_verify_intake.c` uses, kept small here because what is under test
 * is not intake. */
typedef struct intake_env {
    fixture fx;
    atlas_buf db_path;
    atlas_db *db;
    int64_t repo_id;
} intake_env;

#define INTAKE_COMMIT "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

static void intake_env_open(intake_env *e, atlas_err *err) {
    memset(e, 0, sizeof *e);
    atlas_buf_init(&e->db_path);
    T_OK(fx_open(&e->fx, err), err);
    T_OK(atlas_buf_appendf(&e->db_path, err, "%s/atlas.db", fx_data_dir(&e->fx)), err);
    T_OK(atlas_datadir_ensure(fx_data_dir(&e->fx), err), err);
    T_OK(atlas_db_open(atlas_buf_cstr(&e->db_path), &e->db, err), err);
    T_OK(atlas_db_migrate(e->db, err), err);

    atlas_repo_identity id;
    memset(&id, 0, sizeof id);
    id.root = "/tmp/atlas-yield-repo";
    id.root_len = strlen(id.root);
    id.common_dir = "/tmp/atlas-yield-repo/.git";
    id.common_dir_len = strlen(id.common_dir);
    id.git_dir = id.common_dir;
    id.git_dir_len = id.common_dir_len;
    id.object_format = "sha1";
    T_OK(atlas_db_repo_add(e->db, "proj", &id, &e->repo_id, err), err);
    T_OK(atlas_db_exec_sql(e->db,
                           "INSERT INTO commits(repo_id, oid, parent_count, subject)"
                           "  VALUES(1, '" INTAKE_COMMIT "', 0, 'a');"
                           "UPDATE repositories SET scanned_head = '" INTAKE_COMMIT "';",
                           err),
         err);
}

static void intake_env_close(intake_env *e) {
    atlas_db_close(e->db);
    atlas_buf_free(&e->db_path);
    fx_close(&e->fx);
}

static int64_t intake_claim_rows(intake_env *e) {
    int64_t n = -1;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_db_query_int64(e->db,
                              "SELECT count(*) FROM verify_claims WHERE text = '" BUSY_CLAIM_TEXT
                              "';",
                              &n, &err),
         &err);
    return n;
}

/* One submission, through a seam that either refuses it the way the writer does
 * or lets it reach the write point.
 *
 * **A back-out is not an error the write point returns; it is the write point
 * never being called.** `writer_wait_locked` takes the job out of the queue
 * before anything looks at it, and that is precisely what makes the advertised
 * retry safe. Simulating it any other way — a failure inside intake, a rolled
 * back transaction — would be testing a different mechanism and would let a
 * refusal that half-wrote pass. This is the `tests/test_a11_run.c` pattern
 * applied to the intake surface, and it is in process because a *live* refusal
 * is no longer deterministically reachable: the pass now yields, so a real
 * daemon serves this write instead of refusing it, which is the season and is
 * also why the refusal needs a home that does not depend on losing a race. */
static atlas_status submit_claim(intake_env *e, int *refusals_left, int *refusals,
                                 atlas_err *err) {
    if (*refusals_left > 0) {
        (*refusals_left)--;
        (*refusals)++;
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "%s",
                             ATLAS_IPC_BUSY_TOKEN
                             " the Atlas daemon is performing semantic maintenance and cannot "
                             "take this write yet. Nothing was queued and nothing will run, so "
                             "the request may be sent again.");
    }
    atlas_verify_op op;
    atlas_verify_op_init(&op);
    op.kind = ATLAS_VERIFY_OP_CLAIM_CREATE;
    op.channel = ATLAS_VERIFY_CHANNEL_MODEL;
    atlas_status st = atlas_buf_set_str(&op.repo_name, "proj", err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op.domain, "code", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op.text, BUSY_CLAIM_TEXT, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op.actor_name, "a-model", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op.actor_provider, "anthropic", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op.session_key, "a927", err);
    }
    atlas_verify_intake_result res;
    atlas_verify_intake_result_init(&res);
    if (st == ATLAS_OK) {
        st = atlas_verify_intake_apply(e->db, &op, &res, err);
    }
    atlas_verify_intake_result_free(&res);
    atlas_verify_op_free(&op);
    return st;
}

/* **A submission refused because the daemon was busy wrote nothing, and the
 * retry that lands makes one row.**
 *
 * The two halves are separate claims and only one of them survives being checked
 * at the end. A refusal that silently stored the row would still total one row,
 * because the retry resolves to it by content key — so the count *after* each
 * refusal is the assertion that discriminates, and the total afterwards is the
 * one that proves the retry was not a second submission. */
static void test_a_refused_submission_stores_nothing_and_a_retry_makes_one_row(void) {
    atlas_err err;
    atlas_err_init(&err);
    intake_env e;
    intake_env_open(&e, &err);

    int refusals_left = 3;
    int refusals = 0;
    for (int i = 0; i < 3; i++) {
        atlas_err rerr;
        atlas_err_init(&rerr);
        atlas_status st = submit_claim(&e, &refusals_left, &refusals, &rerr);
        T_CHECK_MSG(st != ATLAS_OK, "a simulated busy refusal reported success");
        T_CHECK_MSG(strstr(atlas_err_msg(&rerr), ATLAS_IPC_BUSY_TOKEN) != NULL,
                    "a busy refusal did not carry the token a client keys on: %s",
                    atlas_err_msg(&rerr));
        T_CHECK_MSG(strstr(atlas_err_msg(&rerr), "Nothing was queued") != NULL,
                    "a busy refusal did not say the write had not been queued: %s",
                    atlas_err_msg(&rerr));
        /* Asserted here, at the instant it is true, rather than inferred from a
         * total later. This is the half that cannot be reconstructed. */
        T_CHECK_MSG(intake_claim_rows(&e) == 0,
                    "a submission refused as busy left a row, so the advertised retry would "
                    "submit it twice");
    }
    T_EQ_INT(refusals, 3);

    /* The retry that reaches the write point. */
    T_OK(submit_claim(&e, &refusals_left, &refusals, &err), &err);
    T_CHECK_MSG(intake_claim_rows(&e) == 1, "the accepted submission produced %lld rows",
                (long long)intake_claim_rows(&e));

    /* And a retry after it lands is still not a second proposition — a retry is
     * not a corroboration, which is the rule intake enforces by content key. */
    T_OK(submit_claim(&e, &refusals_left, &refusals, &err), &err);
    T_CHECK_MSG(intake_claim_rows(&e) == 1,
                "one proposition, refused three times and submitted twice, produced %lld rows",
                (long long)intake_claim_rows(&e));

    intake_env_close(&e);
}

/* --- the two classifications, over the whole vocabulary ------------------- */

/* **Both questions are asked of every job kind, and the answers are compared.**
 *
 * Neither switch has a `default:`, so a new kind does not compile until somebody
 * places it on one side of each — but a switch that compiles is not a switch
 * anybody thought about, and nothing in the build notices a kind placed on the
 * wrong side. Two things are checked, both of which a compiler cannot:
 *
 *   1. **Unbounded implies not drainable.** An unbounded job running inside
 *      another unbounded job has no bound at all, and that is the one
 *      combination that can never be right.
 *   2. **The drainable set is exactly the documented one.** Spelled out here as
 *      a literal list, which is the two-spellings discipline `tests/test_orch_run.c`
 *      uses for the same reason: a rule stated once in code and once in prose
 *      drifts, and comparing them is what stops it.
 *
 * The loop walks the enum by ordinal rather than listing the members, so a kind
 * added anywhere in it is covered without this test being edited. */
static void test_the_two_job_classifications_agree_over_the_whole_enum(void) {
    /* The documented drainable set. Adding a kind here is a deliberate act about
     * what may run inside a semantic pass; see `docs/extending.md`. */
    static const atlas_job_kind DRAINABLE[] = {
        ATLAS_JOB_ORCH, ATLAS_JOB_AI,       ATLAS_JOB_DECISION,
        ATLAS_JOB_VERIFY, ATLAS_JOB_GW_AUDIT, ATLAS_JOB_APIKEY,
    };
    /* The last member of the vocabulary. Named rather than counted, so that a
     * kind appended after it makes this line wrong in a way somebody sees. */
    const int last = (int)ATLAS_JOB_SEM_DISCOVER;

    int drainable_seen = 0;
    for (int i = 0; i <= last; i++) {
        atlas_job_kind k = (atlas_job_kind)i;
        bool expected = false;
        for (size_t d = 0; d < sizeof DRAINABLE / sizeof DRAINABLE[0]; d++) {
            if (DRAINABLE[d] == k) {
                expected = true;
            }
        }
        T_CHECK_MSG(job_kind_is_drainable(k) == expected,
                    "job kind %d is %s drainable and the documented set says %s", i,
                    job_kind_is_drainable(k) ? "" : "not", expected ? "it is" : "it is not");
        if (job_kind_is_unbounded(k)) {
            T_CHECK_MSG(!job_kind_is_drainable(k),
                        "job kind %d is unbounded and drainable, so one unbounded job could run "
                        "inside another",
                        i);
        }
        if (expected) {
            drainable_seen++;
        }
    }
    /* Every documented member was actually reached by the walk: a set member
     * outside the enum's range would otherwise be silently unchecked. */
    T_EQ_INT(drainable_seen, (int)(sizeof DRAINABLE / sizeof DRAINABLE[0]));
}

static const atlas_test TESTS[] = {
    {"a semantic pass does not stall the serve loop",
     test_a_semantic_pass_does_not_stall_the_serve_loop},
    {"one blocked write does not hold the other clients",
     test_one_blocked_write_does_not_hold_the_other_clients},
    {"a verification write lands during a semantic pass",
     test_a_verification_write_lands_during_a_semantic_pass},
    {"a refused submission stores nothing and a retry makes one row",
     test_a_refused_submission_stores_nothing_and_a_retry_makes_one_row},
    {"the two job classifications agree over the whole enum",
     test_the_two_job_classifications_agree_over_the_whole_enum},
};

ATLAS_TEST_MAIN("daemon_responsive", TESTS)
