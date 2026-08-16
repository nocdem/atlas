/* Atlas - A9.2.3: the daemon maintaining semantic freshness on its own.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * This is the season's closure gate, driven against a live daemon rather than
 * against the functions it is built from. The distinction matters: the defect
 * that stopped the first implementation converging was not in any function —
 * every unit test would have passed — it was that the scheduler passed its own
 * in-flight flag into the plan it then used to decide whether to clear the flag.
 * A test that called `atlas_sem_plan_for` directly could not see it. Running a
 * daemon and editing a file could.
 *
 * What is asserted here:
 *
 *   1. **A repository an operator enabled reaches CURRENT with nobody running a
 *      command**, from nothing, and again after an uncommitted edit.
 *   2. **A build that fails preserves the last-known-good generation**, records
 *      a reason, and does not retry until the source moves — so a repository
 *      that cannot compile does not run a compiler every fifteen seconds for
 *      ever.
 *   3. **Fixing the source recovers automatically**, which is the half of the
 *      failure story that makes the other half safe to have.
 *   4. **Nothing rebuilds a repository nobody configured.** A8-CI's rule, after
 *      a repository change became a rebuild trigger.
 *   5. **The socket and the local context describe a generation identically**,
 *      which is where every previous season's parity defect lived.
 *
 * Waiting is always for an observable outcome with a deadline, never a guessed
 * sleep: the sweep interval is a constant, the machine is not.
 */
#include <stdio.h>
#include <string.h>

#include "atlas/db.h"
#include "atlas/limits.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

/* Generous: a semantic index runs a compiler over the fixture, and the sweep
 * that notices runs on its own interval. Every wait here is for an outcome, so
 * a slow machine takes longer rather than failing. */
#define WAIT_MS 120000

typedef struct live {
    fixture fx;
    fx_daemon d;
} live;

static void cli(live *L, const char *const *args, size_t n, atlas_buf *out, int *code,
                atlas_err *err) {
    T_OK(fx_atlas_with_runtime(&L->fx, &L->d, args, n, out, NULL, code, err), err);
}

/* A repository with one compiled source, one header and a compilation database.
 *
 * `extra` is written as `extra.c` and is deliberately *not* named by the
 * compilation database when `covered` is false, which is the ordinary shape of
 * a real repository and the shape that exposed the non-convergence defect. */
static void build_repo(live *L, const char *body, atlas_err *err) {
    T_OK(fx_mkdir(fx_repo(&L->fx), "include", err), err);
    T_OK(fx_write(fx_repo(&L->fx), "include/api.h", "int f(int);\n", err), err);
    T_OK(fx_write(fx_repo(&L->fx), "a.c", body, err), err);

    atlas_buf doc = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&doc, err,
                           "[{\"directory\":\"%s\",\"arguments\":[\"cc\",\"-I\",\"include\","
                           "\"-std=gnu11\",\"-c\",\"a.c\"],\"file\":\"a.c\"}]",
                           fx_repo(&L->fx)),
         err);
    T_OK(fx_write(fx_repo(&L->fx), "compile_commands.json", atlas_buf_cstr(&doc), err), err);
    atlas_buf_free(&doc);
}

static void enable_auto(live *L, atlas_err *err);

static void live_start(live *L, bool configured, atlas_err *err) {
    T_OK(fx_open(&L->fx, err), err);
    T_OK(fx_init_repo(&L->fx, fx_repo(&L->fx), NULL, err), err);
    build_repo(L, "#include \"api.h\"\nint f(int x){return x;}\n", err);
    T_OK(fx_add_all(&L->fx, fx_repo(&L->fx), err), err);
    T_OK(fx_commit(&L->fx, fx_repo(&L->fx), "first", err), err);

    const char *add[] = {"--data-dir", fx_data_dir(&L->fx), "repo", "add", fx_repo(&L->fx),
                         "--name", "fixture"};
    int code = 0;
    T_OK(fx_atlas(add, 7u, NULL, NULL, &code, err), err);
    T_EQ_INT(code, 0);

    if (configured) {
        enable_auto(L, err);
    }

    fx_daemon_init(&L->d);
    T_OK(fx_daemon_start(&L->fx, &L->d, err), err);
    T_OK(fx_daemon_wait_ready(&L->d, WAIT_MS, err), err);
}

static void live_stop(live *L) {
    fx_daemon_stop(&L->d, false);
    fx_daemon_free(&L->d);
    fx_close(&L->fx);
}

/* Waits until `code sem-status --json` contains `needle`. */
static bool wait_status(live *L, const char *needle, atlas_err *err) {
    const char *args[] = {"code", "sem-status", "fixture", "--json"};
    bool found = false;
    T_OK(fx_wait_for_substring(&L->fx, &L->d, args, 4u, needle, WAIT_MS, &found, err), err);
    return found;
}

/* Enables automatic rebuild, before any daemon exists.
 *
 * Deliberately the *local* path, and the reason is the security design rather
 * than convenience: `code.sem_config` is in the operator-uid group, so over the
 * socket it is offered only to the peer a **root-owned** policy names. A test
 * fixture cannot have one — it would need a root-owned policy file and a
 * root-owned, non-writable binary — so the daemon correctly refuses it here,
 * and `test_the_operator_method_is_hidden_from_an_ordinary_peer` below asserts
 * exactly that refusal instead of working around it.
 *
 * Configuring before the daemon starts is also what an operator actually does:
 * the description is written once and the daemon then maintains the repository
 * for as long as it runs. */
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

/* --- the closure gate -------------------------------------------------------- */

static void test_the_daemon_reaches_current_and_stays_there(void) {
    atlas_err err;
    atlas_err_init(&err);
    live L;
    live_start(&L, /*configured=*/true, &err);

    /* Nothing else happens. No command is run. The daemon notices that a
     * configured repository has no index and builds one — which is the state
     * transition UNAVAILABLE -> BUILDING -> CURRENT, unaided. */
    T_CHECK_MSG(wait_status(&L, "\"activity\":\"CURRENT\"", &err),
                "the daemon did not build a first semantic index unaided");

    /* And the coverage manifest is real, not a placeholder: `a.c` is compiled,
     * `include/api.h` is a header and not a candidate translation unit. */
    {
        const char *args[] = {"code", "sem-status", "fixture", "--json"};
        atlas_buf out = ATLAS_BUF_INIT;
        int code = 0;
        cli(&L, args, 4u, &out, &code, &err);
        T_EQ_INT(code, 0);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"scope_discovery\":\"DECLARED\"") != NULL,
                    "the published generation recorded no scope: %s", atlas_buf_cstr(&out));
        T_CHECK(strstr(atlas_buf_cstr(&out), "\"scope_uncovered\":0") != NULL);
        T_CHECK(strstr(atlas_buf_cstr(&out), "\"coverage_complete\":true") != NULL);
        atlas_buf_free(&out);
    }

    /* The decisive edit: uncommitted, so every A8-CI staleness check would miss
     * it. The new function must appear in the graph without anybody asking. */
    T_OK(fx_write(fx_repo(&L.fx), "a.c",
                  "#include \"api.h\"\nstatic int helper(int x){return x+1;}\n"
                  "int f(int x){return helper(x);}\n",
                  &err),
         &err);

    /* Asked of the *graph* rather than of the status line: the point is not
     * that a state machine advanced, it is that a query now answers about
     * source nobody has committed and nobody has asked Atlas to reread. */
    {
        const char *args[] = {"code", "semantic", "fixture", "helper", "--json"};
        bool found = false;
        T_OK(fx_wait_for_substring(&L.fx, &L.d, args, 5u, "\"name\":\"helper\"", WAIT_MS, &found,
                                   &err),
             &err);
        T_CHECK_MSG(found, "a symbol added by an uncommitted edit never reached the index");
    }
    T_CHECK_MSG(wait_status(&L, "\"activity\":\"CURRENT\"", &err),
                "the repository did not settle at CURRENT after rebuilding");

    live_stop(&L);
}

/* The refusal that `enable_auto` works around, asserted rather than assumed.
 *
 * `code.sem_config` decides whether the daemon runs a compiler every time a
 * repository changes, so it is in the operator-uid group: offered only to the
 * peer a root-owned policy names, and answered with `unknown method` — the same
 * answer a name that does not exist gets — for everybody else. A fixture daemon
 * has no such policy, so this is the ordinary machine's answer and the one a
 * model would get. */
static void test_the_operator_method_is_hidden_from_an_ordinary_peer(void) {
    atlas_err err;
    atlas_err_init(&err);
    live L;
    live_start(&L, /*configured=*/false, &err);

    const char *args[] = {"code", "sem-config", "fixture", "--auto"};
    atlas_buf out = ATLAS_BUF_INIT;
    atlas_buf errout = ATLAS_BUF_INIT;
    int code = 0;
    /* Stderr as well as stdout: the refusal is a diagnostic, and a test that
     * read only stdout would pass on an empty answer. */
    T_OK(fx_atlas_with_runtime(&L.fx, &L.d, args, 4u, &out, &errout, &code, &err), &err);
    T_CHECK_MSG(code != 0, "an ordinary peer enabled automatic rebuild over the socket: %s",
                atlas_buf_cstr(&out));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&errout), "unknown method") != NULL ||
                    strstr(atlas_buf_cstr(&out), "unknown method") != NULL,
                "the refusal named the method rather than hiding it: %s / %s",
                atlas_buf_cstr(&out), atlas_buf_cstr(&errout));
    atlas_buf_free(&errout);
    atlas_buf_free(&out);

    /* And the repository is still unconfigured, so nothing will rebuild it. */
    const char *read[] = {"code", "sem-config", "fixture", "--json"};
    atlas_buf state = ATLAS_BUF_INIT;
    cli(&L, read, 4u, &state, &code, &err);
    T_EQ_INT(code, 0);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&state), "\"auto_rebuild\":false") != NULL,
                "a refused request still enabled automatic rebuild: %s", atlas_buf_cstr(&state));
    atlas_buf_free(&state);

    live_stop(&L);
}

static void test_a_failed_build_preserves_the_last_good_generation(void) {
    atlas_err err;
    atlas_err_init(&err);
    live L;
    live_start(&L, /*configured=*/true, &err);
    T_CHECK_MSG(wait_status(&L, "\"activity\":\"CURRENT\"", &err),
                "the daemon did not build a first semantic index");

    /* Break the *build description* rather than the source: a compilation
     * database Atlas cannot read fails the pass outright, which is the
     * deterministic failure this test needs. Breaking the C would produce a
     * PARTIAL unit, which is a different and equally real state — but not a
     * failed pass. */
    T_OK(fx_write(fx_repo(&L.fx), "compile_commands.json", "{ not a compilation database", &err),
         &err);

    /* The last-known-good generation is still served, and still says which
     * source it describes. That is the whole of "preserve the last valid
     * generation until the replacement succeeds". */
    T_CHECK_MSG(wait_status(&L, "\"have_generation\":true", &err),
                "a failed rebuild discarded the generation that was working");

    /* And the answer is no longer CURRENT: a repository whose build description
     * cannot be read is not one whose index describes the current source. */
    {
        const char *args[] = {"code", "sem-status", "fixture", "--json"};
        atlas_buf out = ATLAS_BUF_INIT;
        int code = 0;
        cli(&L, args, 4u, &out, &code, &err);
        T_EQ_INT(code, 0);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"activity\":\"CURRENT\"") == NULL,
                    "an unreadable build description still reported CURRENT: %s",
                    atlas_buf_cstr(&out));
        atlas_buf_free(&out);
    }

    /* Fix it. The daemon recovers with nobody running a command — which is what
     * makes the conservative retry rule safe to have: it holds until the inputs
     * change, and changing them is exactly what fixing the fault does. */
    build_repo(&L, "#include \"api.h\"\nint f(int x){return x;}\n", &err);
    T_CHECK_MSG(wait_status(&L, "\"activity\":\"CURRENT\"", &err),
                "the daemon did not recover after the build description was fixed");

    live_stop(&L);
}

static void test_an_unconfigured_repository_is_never_rebuilt(void) {
    /* A8-CI's rule, after A9.2.3 made a repository change a rebuild trigger.
     * The repository is registered, watched, edited and reconciled — and no
     * compiler runs, because no operator said one may. */
    atlas_err err;
    atlas_err_init(&err);
    live L;
    live_start(&L, /*configured=*/false, &err);

    T_OK(fx_write(fx_repo(&L.fx), "a.c", "#include \"api.h\"\nint f(int x){return x+1;}\n", &err),
         &err);

    /* Wait for the *file* index to notice, which proves the daemon is awake and
     * looking at this repository — so a subsequent absence of semantic activity
     * is a decision rather than a daemon that never ran. */
    {
        const char *args[] = {"code", "status", "fixture", "--json"};
        bool found = false;
        T_OK(fx_wait_for_substring(&L.fx, &L.d, args, 4u, "\"index_current\":true", WAIT_MS,
                                   &found, &err),
             &err);
        T_CHECK(found);
    }

    const char *args[] = {"code", "sem-config", "fixture", "--json"};
    atlas_buf out = ATLAS_BUF_INIT;
    int code = 0;
    cli(&L, args, 4u, &out, &code, &err);
    T_EQ_INT(code, 0);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"activity\":\"DISABLED\"") != NULL,
                "an unconfigured repository left DISABLED: %s", atlas_buf_cstr(&out));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"have_generation\":false") != NULL,
                "a compiler ran over a repository nobody authorised: %s", atlas_buf_cstr(&out));
    atlas_buf_free(&out);

    live_stop(&L);
}

static void test_the_socket_and_the_local_path_describe_one_generation(void) {
    /* Where every previous season's parity defect lived. `sem.status` shipped
     * without `compiler_id` and `started_at` for three seasons, so on a system
     * deployment — where the socket is the only path to the index — both were
     * silently empty. The fields are asserted through the transport rather than
     * through the struct, because the struct was never the thing that was
     * wrong. */
    atlas_err err;
    atlas_err_init(&err);
    live L;
    live_start(&L, /*configured=*/true, &err);
    T_CHECK_MSG(wait_status(&L, "\"activity\":\"CURRENT\"", &err),
                "the daemon did not build a first semantic index");

    const char *args[] = {"code", "sem-status", "fixture", "--json"};
    atlas_buf out = ATLAS_BUF_INIT;
    int code = 0;
    cli(&L, args, 4u, &out, &code, &err);
    T_EQ_INT(code, 0);
    const char *doc = atlas_buf_cstr(&out);

    /* The two fields the daemon never sent. */
    T_CHECK_MSG(strstr(doc, "\"compiler_id\":\"clang\"") != NULL,
                "the daemon did not send the generation's compiler id: %s", doc);
    T_CHECK_MSG(strstr(doc, "\"started_at\":\"20") != NULL,
                "the daemon did not send the generation's start time: %s", doc);
    /* And A9.2.3's own fields, which must not repeat the mistake. */
    T_CHECK_MSG(strstr(doc, "\"scope_candidates\":") != NULL, "no scope manifest crossed the "
                                                              "socket");
    T_CHECK_MSG(strstr(doc, "\"activity\":") != NULL, "no derived state crossed the socket");
    T_CHECK_MSG(strstr(doc, "\"auto_rebuild\":true") != NULL,
                "the build description did not cross the socket: %s", doc);
    atlas_buf_free(&out);

    live_stop(&L);
}


/* §27: a model must be able to learn whether semantic evidence is trustworthy,
 * and must not be able to do anything about it.
 *
 * Asserted through the real MCP adapter — JSON on stdin, JSON on stdout — for
 * the reason A9.2.1 gives about security refusals: a guarantee that exists only
 * below the transport is one an attacker never meets, and a field that exists
 * only in a struct is one a model never sees.
 *
 * No new tool was added for this. `atlas_sem_status` already relays `sem.status`
 * verbatim, so the derived state and the coverage manifest reach a model because
 * the daemon sends them, which is why the pinned tool count in
 * `tests/test_plugin.c` does not move. What a model still cannot do is enable a
 * rebuild or cause one: there is no tool for it and the method it would need is
 * in the operator-uid group. */
static void test_a_model_can_read_freshness_and_cannot_change_it(void) {
    atlas_err err;
    atlas_err_init(&err);
    live L;
    live_start(&L, /*configured=*/true, &err);
    T_CHECK_MSG(wait_status(&L, "\"activity\":\"CURRENT\"", &err),
                "the daemon did not build a first semantic index");

    atlas_buf runtime_env = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&runtime_env, &err, "XDG_RUNTIME_DIR=%s",
                           atlas_buf_cstr(&L.d.runtime_dir)),
         &err);
    const char *env_list[] = {atlas_buf_cstr(&runtime_env), NULL};
    static const char SCRIPT[] =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
        "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{}}}\n"
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
        "{\"name\":\"atlas_sem_status\",\"arguments\":{\"repo\":\"fixture\"}}}\n"
        /* Every name a tool would plausibly have if one existed to change any of
         * this. None does, and their absence is the guarantee. */
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":"
        "{\"name\":\"atlas_sem_config\",\"arguments\":{\"repo\":\"fixture\"}}}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":"
        "{\"name\":\"atlas_sem_rebuild\",\"arguments\":{\"repo\":\"fixture\"}}}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":"
        "{\"name\":\"atlas_code_index\",\"arguments\":{\"repo\":\"fixture\"}}}\n";

    const char *args[] = {"mcp"};
    atlas_buf out = ATLAS_BUF_INIT;
    int code = 0;
    T_OK(fx_atlas_stdin(args, 1u, env_list, SCRIPT, strlen(SCRIPT), &out, NULL, &code, &err),
         &err);
    T_EQ_INT(code, 0);
    const char *doc = atlas_buf_cstr(&out);

    /* The state reaches the model, in the shape every other surface uses. */
    T_CHECK_MSG(strstr(doc, "\\\"activity\\\":\\\"CURRENT\\\"") != NULL,
                "a model cannot see whether the semantic index is trustworthy: %s", doc);
    T_CHECK_MSG(strstr(doc, "coverage_complete") != NULL,
                "a model cannot see whether the semantic coverage is complete: %s", doc);
    T_CHECK_MSG(strstr(doc, "scope_uncovered") != NULL,
                "a model cannot see how much of the repository was read: %s", doc);

    /* And nothing that could change it exists. */
    T_CHECK_MSG(strstr(doc, "unknown tool") != NULL,
                "a tool that changes semantic state answered: %s", doc);
    atlas_buf_free(&out);
    atlas_buf_free(&runtime_env);

    live_stop(&L);
}


/* §31: a generation left RUNNING by a daemon that is gone must not wedge the
 * scheduler.
 *
 * A8-CI said the next pass would report and reap one, and nothing ever did —
 * harmless while nothing scheduled off the record. A9.2.3 holds the scheduler
 * while a generation is being built, and a RUNNING row from a dead daemon is
 * indistinguishable from a live build, so one crash left the repository
 * reporting BUILDING for ever and never rebuilding again.
 *
 * The row is planted directly rather than by killing a daemon mid-build,
 * because a compiler over a one-file fixture finishes in milliseconds and the
 * window cannot be hit reliably. What is under test is what the *next daemon*
 * makes of the record, and the record is the same either way. */
static void test_a_generation_left_running_by_a_dead_daemon_is_reaped(void) {
    atlas_err err;
    atlas_err_init(&err);
    live L;
    live_start(&L, /*configured=*/true, &err);
    T_CHECK_MSG(wait_status(&L, "\"activity\":\"CURRENT\"", &err),
                "the daemon did not build a first semantic index");

    /* Stop it, then plant the wreckage a crash would leave. */
    fx_daemon_stop(&L.d, false);

    atlas_buf db_path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&db_path, &err, "%s/atlas.db", fx_data_dir(&L.fx)), &err);
    atlas_db *db = NULL;
    T_OK(atlas_db_open(atlas_buf_cstr(&db_path), &db, &err), &err);
    T_OK(atlas_db_exec_sql(db,
                           "INSERT INTO sem_generations(repo_id, commit_id, status, started_at,"
                           "  analyzer_id, analyzer_version)"
                           "  SELECT id, 'deadbeef', 'RUNNING', '2026-01-01T00:00:00Z',"
                           "         'atlas-c-libclang', 1 FROM repositories WHERE name='fixture';",
                           &err),
         &err);
    atlas_db_close(db);
    atlas_buf_free(&db_path);

    /* The same struct, restarted — no second fx_daemon_init, which would memset
     * over the buffers it owns. */
    T_OK(fx_daemon_start(&L.fx, &L.d, &err), &err);
    T_OK(fx_daemon_wait_ready(&L.d, WAIT_MS, &err), &err);

    /* The published generation is untouched and the repository is answerable
     * again. Without the reap this reports BUILDING and never leaves it. */
    T_CHECK_MSG(wait_status(&L, "\"activity\":\"CURRENT\"", &err),
                "an interrupted generation left the repository reporting a build for ever");

    const char *args[] = {"code", "sem-status", "fixture", "--json"};
    atlas_buf out = ATLAS_BUF_INIT;
    int code = 0;
    cli(&L, args, 4u, &out, &code, &err);
    T_EQ_INT(code, 0);
    /* And the interrupted attempt is recorded rather than erased: "indexing
     * died" is an operational fact and the table that could not state it would
     * be the one that only recorded outcomes it reached. */
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"have_generation\":true") != NULL,
                "the reap discarded the generation that was working: %s", atlas_buf_cstr(&out));
    atlas_buf_free(&out);

    live_stop(&L);
}

static const atlas_test TESTS[] = {
    {"the daemon reaches current and stays there without being asked",
     test_the_daemon_reaches_current_and_stays_there},
    {"the operator method is hidden from an ordinary peer",
     test_the_operator_method_is_hidden_from_an_ordinary_peer},
    {"a failed build preserves the last-known-good generation and recovers",
     test_a_failed_build_preserves_the_last_good_generation},
    {"an unconfigured repository is never rebuilt",
     test_an_unconfigured_repository_is_never_rebuilt},
    {"the socket and the local path describe one generation",
     test_the_socket_and_the_local_path_describe_one_generation},
    {"a model can read freshness and cannot change it",
     test_a_model_can_read_freshness_and_cannot_change_it},
    {"a generation left running by a dead daemon is reaped",
     test_a_generation_left_running_by_a_dead_daemon_is_reaped},
};

ATLAS_TEST_MAIN("sem_auto", TESTS)
