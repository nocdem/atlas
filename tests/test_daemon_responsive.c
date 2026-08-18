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

#include <sqlite3.h>

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
static bool claim_is_listed(live *L, atlas_buf *out, atlas_err *err) {
    (void)run_mcp(L,
                  MCP_HANDSHAKE
                  "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":"
                  "{\"name\":\"atlas_verify_claims\",\"arguments\":{\"repo\":\"fixture\"}}}\n",
                  out, err);
    T_CHECK_MSG(mcp_tool_ok(out, "\"id\":9"), "the claims could not be listed: %s",
                atlas_buf_cstr(out));
    return strstr(atlas_buf_cstr(out), BUSY_CLAIM_TEXT) != NULL;
}

/* **A write refused because the daemon is busy wrote nothing.**
 *
 * A9.2.6 made a caller stop waiting; it did not say what happens to the record
 * the caller was trying to make. For a hook that question has a written answer —
 * hooks fail open and the metadata is lost on purpose — but a verification
 * submission is not metadata. A claim, its evidence and its attestations exist
 * nowhere but here: git cannot be re-read to recover one, and no pass rebuilds
 * them. A refusal that half-wrote, or that wrote and reported failure, would be
 * a record whose existence nobody could determine from the answer they got.
 *
 * So the two halves are asserted at the moment they are true rather than
 * inferred at the end:
 *
 *   - the refusal is explicit, says nothing was queued, and arrives quickly;
 *   - **at that instant the claim is not on the read surface**, which is what
 *     makes the advertised retry safe rather than a way to submit twice;
 *   - the retry that eventually lands produces exactly one row.
 *
 * The last one alone would not discriminate: a refusal that silently stored the
 * row would still total one, because the retry would resolve to it by content
 * key. Checking at the refusal is what separates "nothing was written" from
 * "something was written and the idempotency machinery covered for it". */
static void test_a_verification_write_refused_while_busy_stores_nothing(void) {
    atlas_err err;
    atlas_err_init(&err);
    live L;
    live_start(&L, &err);

    static const char SUBMIT[] =
        MCP_HANDSHAKE "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\",\"params\":"
                      "{\"name\":\"atlas_verify_claim_create\",\"arguments\":"
                      "{\"repo\":\"fixture\",\"text\":\"" BUSY_CLAIM_TEXT "\","
                      "\"domain\":\"code\",\"actor\":\"a-model\",\"run\":\"o10-busy\"}}}\n";

    bool observed_building = false;
    bool saw_busy = false;
    bool accepted = false;
    /* True once a submission has been answered in a way that leaves a write
     * possibly still on its way. Two outcomes do that and both are documented
     * rather than defects: a repository the daemon has not finished indexing
     * yet, and — the one A9.2.6 wrote down as a residual — a submission queued
     * behind a *bounded* job, which `job_kind_is_unbounded` deliberately refuses
     * to back out of, so the caller waits out its own timeout and the job still
     * runs. After either, "the claim is not on the read surface" stops being a
     * statement about the busy refusal and becomes a race with that write, so
     * the check is not made again. */
    bool write_may_be_in_flight = false;
    int64_t worst_submit = 0;
    atlas_buf last_other = ATLAS_BUF_INIT;

    atlas_buf out = ATLAS_BUF_INIT;
    int64_t deadline = now_ms() + WAIT_MS;
    while (now_ms() < deadline) {
        bool building = false;
        bool current = false;
        sem_status(&L, &building, &current, &err);
        if (building) {
            observed_building = true;
        }

        int64_t took = run_mcp(&L, SUBMIT, &out, &err);
        if (building && took > worst_submit) {
            worst_submit = took;
        }

        if (mcp_tool_ok(&out, "\"id\":8")) {
            accepted = true;
        } else if (strstr(atlas_buf_cstr(&out), "BUSY:") != NULL) {
            saw_busy = true;
            /* The refusal must carry the claim that makes retrying safe: a
             * caller cannot tell "nothing ran" from "the result was abandoned"
             * by status, so the message is where the difference is stated. */
            T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "Nothing was queued") != NULL,
                        "a busy refusal did not say the write had not been queued: %s",
                        atlas_buf_cstr(&out));
            T_CHECK_MSG(took < RESPONSIVE_MS,
                        "a submission refused as busy still took %lld ms", (long long)took);
            /* The half that cannot be reconstructed afterwards: at this instant
             * nothing was written, which is what makes the advertised retry safe
             * rather than a way to submit twice. */
            if (!accepted && !write_may_be_in_flight) {
                atlas_buf listing = ATLAS_BUF_INIT;
                bool listed = claim_is_listed(&L, &listing, &err);
                T_CHECK_MSG(!listed,
                            "a submission refused as busy is on the read surface, so a refusal "
                            "wrote a row and the advertised retry would submit it twice: %s",
                            atlas_buf_cstr(&listing));
                atlas_buf_free(&listing);
            }
        } else {
            /* Neither accepted nor refused as busy. This is **not** a failure
             * here, and treating it as one was the first version's mistake: the
             * two outcomes that land here are both documented behaviour, not
             * defects, and both are transient. Failing on them would convert
             * A9.2.6's written residual into a broken test, most often under a
             * sanitiser where the window is widest.
             *
             * So it is recorded and retried. What decides the test is the pair
             * of end conditions — a busy refusal was seen, and a retry was
             * eventually accepted — and this text is carried so that a genuine
             * refusal is reported instead of a bare "never accepted". */
            write_may_be_in_flight = true;
            T_OK(atlas_buf_set(&last_other, out.data, out.len, &err), &err);
        }

        if (accepted && current) {
            break;
        }
    }

    /* A fixture that never made the daemon busy would let all of the above pass
     * without testing anything. */
    T_CHECK_MSG(observed_building,
                "the semantic pass was never observed in flight, so this test proved nothing");
    T_CHECK_MSG(saw_busy,
                "no submission ever met the busy path, so this test did not exercise the refusal");
    /* **The refusal was transient and the retry was safe.** A daemon that
     * refused for ever would satisfy every assertion above. */
    T_CHECK_MSG(accepted, "a submission was never accepted on a retry; last other answer: %s",
                last_other.len > 0 ? atlas_buf_cstr(&last_other) : "(none)");
    T_CHECK_MSG(worst_submit < RESPONSIVE_MS,
                "the worst submission during a semantic pass was %lld ms",
                (long long)worst_submit);

    /* One proposition, however many times it was refused and resubmitted. */
    {
        bool listed = claim_is_listed(&L, &out, &err);
        T_CHECK_MSG(listed, "the accepted claim is not on the read surface: %s",
                    atlas_buf_cstr(&out));
    }
    fx_daemon_stop(&L.d, false);
    {
        atlas_buf db_path = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&db_path, &err, "%s/atlas.db", fx_data_dir(&L.fx)), &err);
        sqlite3 *db = NULL;
        T_REQUIRE(sqlite3_open_v2(atlas_buf_cstr(&db_path), &db, SQLITE_OPEN_READONLY, NULL) ==
                  SQLITE_OK);
        sqlite3_stmt *st = NULL;
        T_REQUIRE(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM verify_claims WHERE text = ?1;", -1,
                                     &st, NULL) == SQLITE_OK);
        T_REQUIRE(sqlite3_bind_text(st, 1, BUSY_CLAIM_TEXT, -1, SQLITE_STATIC) == SQLITE_OK);
        int64_t rows = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : -1;
        sqlite3_finalize(st);
        sqlite3_close(db);
        atlas_buf_free(&db_path);
        T_CHECK_MSG(rows == 1,
                    "one proposition, refused and resubmitted, produced %lld rows",
                    (long long)rows);
    }

    atlas_buf_free(&out);
    atlas_buf_free(&last_other);
    live_stop(&L);
}

static const atlas_test TESTS[] = {
    {"a semantic pass does not stall the serve loop",
     test_a_semantic_pass_does_not_stall_the_serve_loop},
    {"one blocked write does not hold the other clients",
     test_one_blocked_write_does_not_hold_the_other_clients},
    {"a verification write refused while busy stores nothing",
     test_a_verification_write_refused_while_busy_stores_nothing},
};

ATLAS_TEST_MAIN("daemon_responsive", TESTS)
