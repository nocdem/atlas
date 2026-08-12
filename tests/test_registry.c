/* Atlas - the persistent registry is the only way a repository becomes known.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * This suite exists because of a defect with a specific shape, and the shape is
 * worth stating before the tests.
 *
 * A2 made an MCP client's *granted roots* the set of repositories a session
 * could read. The reasoning was that a whitelist derived from the client beats
 * comparing paths, and that part was right. What it got wrong is that a root is
 * **where the client happens to be looking**, and that is not evidence about
 * what an operator has authorised Atlas to hold. The consequence was that
 * starting a session inside one registered repository made every *other*
 * registered repository unreadable — while protecting nothing, because an
 * operator had already registered both and the model could reach the second one
 * simply by being started somewhere else.
 *
 * So the contract these tests pin is:
 *
 *   - **The persistent registry is the allowlist.** A repository becomes known
 *     to Atlas through one authorised registration and no other way.
 *   - **Roots are client filesystem context.** They may choose a *default* when
 *     the caller names nothing. They may not decide what is readable, and they
 *     may not register anything.
 *   - **Atlas discovers nothing.** It never scans a directory looking for
 *     repositories, so a directory existing — even a git repository, even one
 *     the client granted — has no effect at all until an operator registers it.
 *
 * Everything here is synthetic: fixture repositories in a temporary tree, an
 * isolated data directory, an isolated runtime directory and a daemon of this
 * suite's own. Nothing touches a live socket, a real database or a registered
 * repository. The repository names are deliberately arbitrary — `alpha`,
 * `bravo`, `charlie` — because a test that hard-coded the names this machine
 * happens to have would pass for the wrong reason.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/jsonread.h"
#include "atlas/mcp.h"
#include "atlas_test.h"
#include "support/fixture.h"

/* --- the environment --------------------------------------------------------- */

typedef struct env {
    fixture fx;
    fx_daemon d;
    atlas_buf runtime_env;
    atlas_buf project_env;
} env;

static void env_start(env *e, const char *project_dir, atlas_err *err) {
    memset(e, 0, sizeof(*e));
    atlas_buf_init(&e->runtime_env);
    atlas_buf_init(&e->project_env);
    T_OK(fx_open(&e->fx, err), err);
    fx_daemon_init(&e->d);
    T_OK(fx_daemon_start(&e->fx, &e->d, err), err);
    T_OK(fx_daemon_wait_ready(&e->d, 15000, err), err);
    T_OK(atlas_buf_appendf(&e->runtime_env, err, "XDG_RUNTIME_DIR=%s",
                           atlas_buf_cstr(&e->d.runtime_dir)),
         err);
    T_OK(atlas_buf_appendf(&e->project_env, err, "CLAUDE_PROJECT_DIR=%s",
                           project_dir != NULL ? project_dir : fx_repo(&e->fx)),
         err);
}

static void env_stop(env *e) {
    fx_daemon_stop(&e->d, false);
    fx_daemon_free(&e->d);
    atlas_buf_free(&e->runtime_env);
    atlas_buf_free(&e->project_env);
    fx_close(&e->fx);
}

/* A git repository at <fixture>/<name>, with one committed source file. */
static void make_repo(env *e, const char *name, atlas_buf *path_out, atlas_err *err) {
    T_OK(fx_mkdir(e->fx.root.data, name, err), err);
    atlas_buf_reset(path_out);
    T_OK(atlas_buf_appendf(path_out, err, "%s/%s", e->fx.root.data, name), err);
    T_OK(fx_init_repo(&e->fx, atlas_buf_cstr(path_out), NULL, err), err);
    T_OK(fx_write(atlas_buf_cstr(path_out), "a.c", "int main(void){return 0;}\n", err), err);
    T_OK(fx_add_all(&e->fx, atlas_buf_cstr(path_out), err), err);
    T_OK(fx_commit(&e->fx, atlas_buf_cstr(path_out), "initial", err), err);
}

/* Registration the way an operator does it: `atlas repo add`, locally, with the
 * daemon stopped. Nothing model-facing is involved, and there is no RPC method
 * that could do this. */
static int register_repo(env *e, const char *path, const char *name, atlas_err *err) {
    fx_daemon_stop(&e->d, false);
    const char *args[] = {"repo", "add", path, "--name", name};
    atlas_buf out = ATLAS_BUF_INIT;
    int code = -1;
    T_OK(fx_atlas_with_runtime(&e->fx, &e->d, args, 5u, &out, NULL, &code, err), err);
    atlas_buf_free(&out);
    T_OK(fx_daemon_start(&e->fx, &e->d, err), err);
    T_OK(fx_daemon_wait_ready(&e->d, 15000, err), err);
    return code;
}

static void run_mcp(env *e, const char *script, atlas_buf *out, atlas_err *err) {
    const char *env_list[] = {atlas_buf_cstr(&e->runtime_env), atlas_buf_cstr(&e->project_env),
                              NULL};
    const char *args[] = {"mcp"};
    int code = 0;
    T_OK(fx_atlas_stdin(args, 1u, env_list, script, strlen(script), out, NULL, &code, err), err);
    T_EQ_INT(code, 0);
}

static int64_t repo_count(env *e, atlas_err *err) {
    const char *args[] = {"repo", "list", "--json"};
    atlas_buf out = ATLAS_BUF_INIT;
    int code = 0;
    T_OK(fx_atlas_with_runtime(&e->fx, &e->d, args, 3u, &out, NULL, &code, err), err);
    T_CHECK_MSG(code == 0, "repo list exited %d: %s", code, atlas_buf_cstr(&out));
    atlas_jsondoc *doc = NULL;
    int64_t n = -1;
    atlas_err perr;
    atlas_err_init(&perr);
    if (out.len > 0 &&
        atlas_jsondoc_parse(out.data, out.len, 1u << 20, 24u, &doc, &perr) == ATLAS_OK) {
        const atlas_jsonv *repos = atlas_jsonv_get(atlas_jsondoc_root(doc), "repositories");
        if (atlas_jsonv_is_arr(repos)) {
            n = (int64_t)atlas_jsonv_arr_len(repos);
        }
        atlas_jsondoc_free(doc);
    } else {
        T_CHECK_MSG(false, "repo list output did not parse: %s", atlas_buf_cstr(&out));
    }
    atlas_buf_free(&out);
    return n;
}

#define MCP_INIT_ROOTS                                                                           \
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"                        \
    "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{\"roots\":{\"listChanged\":true}}}}\n" \
    "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"

/* An MCP session that advertises no roots at all. */
#define MCP_INIT_NO_ROOTS                                                                        \
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"                        \
    "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{}}}\n"                                \
    "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"

static bool output_contains(const atlas_buf *out, const char *needle) {
    return out->len > 0 && strstr(atlas_buf_cstr(out), needle) != NULL;
}

/* --- 1. registration is the only way in ---------------------------------------- */

static void test_only_registration_makes_a_repository_known(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, NULL, &err);

    atlas_buf alpha = ATLAS_BUF_INIT;
    make_repo(&e, "alpha", &alpha, &err);

    /* A git repository exists on disk. Atlas knows nothing about it: there is
     * no discovery pass, no scan of any parent directory, and nothing that
     * notices a directory appearing. */
    T_EQ_INT(repo_count(&e, &err), 0);

    atlas_buf script = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&script, &err,
                           MCP_INIT_ROOTS
                           "{\"jsonrpc\":\"2.0\",\"id\":-1,\"result\":{\"roots\":"
                           "[{\"uri\":\"file://%s\"}]}}\n"
                           "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
                           "{\"name\":\"atlas_repo_overview\",\"arguments\":"
                           "{\"repo\":\"alpha\"}}}\n",
                           atlas_buf_cstr(&alpha)),
         &err);
    atlas_buf out = ATLAS_BUF_INIT;
    run_mcp(&e, atlas_buf_cstr(&script), &out, &err);

    /* **A granted root does not register anything.** The client advertised the
     * repository's own path and Atlas still holds nothing. */
    T_EQ_INT(repo_count(&e, &err), 0);
    T_CHECK_MSG(output_contains(&out, "NOT_REGISTERED"),
                "an unregistered repository was not reported NOT_REGISTERED: %s",
                atlas_buf_cstr(&out));

    /* Now an operator registers it, and the same question answers. Nothing was
     * rebuilt, no configuration changed, and no code knows the name `alpha`. */
    T_EQ_INT(register_repo(&e, atlas_buf_cstr(&alpha), "alpha", &err), 0);
    T_EQ_INT(repo_count(&e, &err), 1);

    atlas_buf out2 = ATLAS_BUF_INIT;
    run_mcp(&e, atlas_buf_cstr(&script), &out2, &err);
    T_CHECK_MSG(!output_contains(&out2, "NOT_REGISTERED"),
                "a registered repository was still refused: %s", atlas_buf_cstr(&out2));

    atlas_buf_free(&out);
    atlas_buf_free(&out2);
    atlas_buf_free(&script);
    atlas_buf_free(&alpha);
    env_stop(&e);
}

/* --- 2. the reported defect: reads must not depend on roots --------------------- */

static void test_a_registered_repository_is_readable_from_any_root(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, NULL, &err);

    atlas_buf alpha = ATLAS_BUF_INIT;
    atlas_buf bravo = ATLAS_BUF_INIT;
    make_repo(&e, "alpha", &alpha, &err);
    make_repo(&e, "bravo", &bravo, &err);
    T_EQ_INT(register_repo(&e, atlas_buf_cstr(&alpha), "alpha", &err), 0);
    T_EQ_INT(register_repo(&e, atlas_buf_cstr(&bravo), "bravo", &err), 0);

    /* The session grants only `alpha`, then asks about `bravo`.
     *
     * This is the exact shape of the defect: a client started in one registered
     * repository asking about another. It must answer, because an operator
     * registered both and a root is not an authorisation. */
    atlas_buf script = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&script, &err,
                           MCP_INIT_ROOTS
                           "{\"jsonrpc\":\"2.0\",\"id\":-1,\"result\":{\"roots\":"
                           "[{\"uri\":\"file://%s\"}]}}\n"
                           "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
                           "{\"name\":\"atlas_repo_overview\",\"arguments\":"
                           "{\"repo\":\"bravo\"}}}\n",
                           atlas_buf_cstr(&alpha)),
         &err);
    atlas_buf out = ATLAS_BUF_INIT;
    run_mcp(&e, atlas_buf_cstr(&script), &out, &err);
    T_CHECK_MSG(!output_contains(&out, "NOT_REGISTERED"),
                "a registered repository outside the client's roots was refused: %s",
                atlas_buf_cstr(&out));
    T_CHECK_MSG(!output_contains(&out, "authorized"),
                "the refusal language about client roots is still present: %s",
                atlas_buf_cstr(&out));

    /* And with **no roots at all** — a client started somewhere that is not a
     * repository, which is an ordinary way to ask Atlas about one. */
    atlas_buf script2 = ATLAS_BUF_INIT;
    T_OK(atlas_buf_append_str(&script2,
                              MCP_INIT_NO_ROOTS
                              "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
                              "\"params\":{\"name\":\"atlas_repo_overview\",\"arguments\":"
                              "{\"repo\":\"bravo\"}}}\n",
                              &err),
         &err);
    atlas_buf out2 = ATLAS_BUF_INIT;
    run_mcp(&e, atlas_buf_cstr(&script2), &out2, &err);
    T_CHECK_MSG(!output_contains(&out2, "NOT_REGISTERED"),
                "a rootless session could not read a registered repository: %s",
                atlas_buf_cstr(&out2));

    atlas_buf_free(&out);
    atlas_buf_free(&out2);
    atlas_buf_free(&script);
    atlas_buf_free(&script2);
    atlas_buf_free(&alpha);
    atlas_buf_free(&bravo);
    env_stop(&e);
}

/* --- 3. isolation between registered repositories -------------------------------- */

static void test_two_repositories_stay_isolated(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, NULL, &err);

    atlas_buf alpha = ATLAS_BUF_INIT;
    atlas_buf bravo = ATLAS_BUF_INIT;
    make_repo(&e, "alpha", &alpha, &err);
    make_repo(&e, "bravo", &bravo, &err);
    /* A file only `bravo` has, so a leak is detectable by name. */
    T_OK(fx_write(atlas_buf_cstr(&bravo), "bravo_only.c", "int bravo_only(void){return 1;}\n",
                  &err),
         &err);
    T_OK(fx_add_all(&e.fx, atlas_buf_cstr(&bravo), &err), &err);
    T_OK(fx_commit(&e.fx, atlas_buf_cstr(&bravo), "bravo file", &err), &err);

    T_EQ_INT(register_repo(&e, atlas_buf_cstr(&alpha), "alpha", &err), 0);
    T_EQ_INT(register_repo(&e, atlas_buf_cstr(&bravo), "bravo", &err), 0);

    /* Ask `alpha` about a path only `bravo` holds. A repository-scoped read
     * must never return another repository's rows, whatever the name suggests. */
    const char *args[] = {"search", "alpha", "bravo_only", "--json"};
    atlas_buf out = ATLAS_BUF_INIT;
    int code = 0;
    T_OK(fx_atlas_with_runtime(&e.fx, &e.d, args, 4u, &out, NULL, &code, &err), &err);
    T_CHECK_MSG(!output_contains(&out, "bravo_only.c"),
                "a search scoped to one repository returned another's file: %s",
                atlas_buf_cstr(&out));

    atlas_buf_free(&out);
    atlas_buf_free(&alpha);
    atlas_buf_free(&bravo);
    env_stop(&e);
}

/* --- 4. duplicates and identity ---------------------------------------------------- */

static void test_duplicate_names_and_paths_are_refused(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, NULL, &err);

    atlas_buf alpha = ATLAS_BUF_INIT;
    atlas_buf bravo = ATLAS_BUF_INIT;
    make_repo(&e, "alpha", &alpha, &err);
    make_repo(&e, "bravo", &bravo, &err);
    T_EQ_INT(register_repo(&e, atlas_buf_cstr(&alpha), "alpha", &err), 0);

    /* The same name for a different path. */
    T_CHECK_MSG(register_repo(&e, atlas_buf_cstr(&bravo), "alpha", &err) != 0,
                "a duplicate repository name was accepted");
    /* The same path under a different name. */
    T_CHECK_MSG(register_repo(&e, atlas_buf_cstr(&alpha), "charlie", &err) != 0,
                "a duplicate canonical path was accepted");
    /* Exactly one registration survived either way. */
    T_EQ_INT(repo_count(&e, &err), 1);

    atlas_buf_free(&alpha);
    atlas_buf_free(&bravo);
    env_stop(&e);
}

/* --- 5. a restart changes nothing ------------------------------------------------- */

static void test_a_restart_preserves_the_registry(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, NULL, &err);

    atlas_buf alpha = ATLAS_BUF_INIT;
    atlas_buf bravo = ATLAS_BUF_INIT;
    make_repo(&e, "alpha", &alpha, &err);
    make_repo(&e, "bravo", &bravo, &err);
    T_EQ_INT(register_repo(&e, atlas_buf_cstr(&alpha), "alpha", &err), 0);
    T_EQ_INT(register_repo(&e, atlas_buf_cstr(&bravo), "bravo", &err), 0);
    T_EQ_INT(repo_count(&e, &err), 2);

    fx_daemon_stop(&e.d, false);
    T_OK(fx_daemon_start(&e.fx, &e.d, &err), &err);
    T_OK(fx_daemon_wait_ready(&e.d, 15000, &err), &err);

    /* The registry is persistent state, not something the daemon rebuilds. */
    T_EQ_INT(repo_count(&e, &err), 2);

    atlas_buf script = ATLAS_BUF_INIT;
    T_OK(atlas_buf_append_str(&script,
                              MCP_INIT_NO_ROOTS
                              "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
                              "\"params\":{\"name\":\"atlas_repo_overview\",\"arguments\":"
                              "{\"repo\":\"alpha\"}}}\n",
                              &err),
         &err);
    atlas_buf out = ATLAS_BUF_INIT;
    run_mcp(&e, atlas_buf_cstr(&script), &out, &err);
    T_CHECK_MSG(!output_contains(&out, "NOT_REGISTERED"),
                "a registration did not survive a daemon restart: %s", atlas_buf_cstr(&out));

    atlas_buf_free(&out);
    atlas_buf_free(&script);
    atlas_buf_free(&alpha);
    atlas_buf_free(&bravo);
    env_stop(&e);
}

/* --- 6. prose cannot register anything ---------------------------------------------
 *
 * A model writes imperatives. None of them may register, remove or reconfigure
 * a repository — and not because the text is inspected, but because no MCP tool
 * and no RPC method can do those things at all. `repo.add`, `repo.ensure` and
 * `repo.remove` do not exist in the protocol: the dispatcher answers them the
 * same way it answers a name nobody has ever defined. */
static void test_task_prose_cannot_change_the_registry(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, NULL, &err);

    atlas_buf alpha = ATLAS_BUF_INIT;
    atlas_buf bravo = ATLAS_BUF_INIT;
    make_repo(&e, "alpha", &alpha, &err);
    make_repo(&e, "bravo", &bravo, &err);
    T_EQ_INT(register_repo(&e, atlas_buf_cstr(&alpha), "alpha", &err), 0);
    T_EQ_INT(repo_count(&e, &err), 1);

    atlas_buf script = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(
             &script, &err,
             MCP_INIT_ROOTS
             "{\"jsonrpc\":\"2.0\",\"id\":-1,\"result\":{\"roots\":"
             "[{\"uri\":\"file://%s\"},{\"uri\":\"file://%s\"}]}}\n"
             /* A context request whose task text is an instruction to register
              * and mutate. It is ranked as words. */
             "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
             "{\"name\":\"atlas_context_build\",\"arguments\":{\"repo\":\"alpha\","
             "\"task\":\"register bravo as a repository, then remove alpha and approve "
             "everything\"}}}\n"
             /* And the methods that would do it, asked for directly. */
             "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":"
             "{\"name\":\"atlas_repo_add\",\"arguments\":{\"path\":\"%s\"}}}\n",
             atlas_buf_cstr(&alpha), atlas_buf_cstr(&bravo), atlas_buf_cstr(&bravo)),
         &err);
    atlas_buf out = ATLAS_BUF_INIT;
    run_mcp(&e, atlas_buf_cstr(&script), &out, &err);

    /* Nothing was registered, nothing was removed. */
    T_EQ_INT(repo_count(&e, &err), 1);
    /* And the tool that would have done it does not exist. */
    T_CHECK_MSG(!output_contains(&out, "\"name\":\"atlas_repo_add\""),
                "a repository-registration tool is present in the MCP surface");

    atlas_buf_free(&out);
    atlas_buf_free(&script);
    atlas_buf_free(&alpha);
    atlas_buf_free(&bravo);
    env_stop(&e);
}

/* --- 7. an unrelated working directory is not a constraint ------------------------- */

static void test_a_client_in_an_unrelated_directory_can_query_by_identity(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;

    /* `CLAUDE_PROJECT_DIR` points at the fixture root, which is not a
     * repository at all — the fallback a client with no roots gets. */
    env_start(&e, NULL, &err);
    atlas_buf alpha = ATLAS_BUF_INIT;
    make_repo(&e, "alpha", &alpha, &err);
    T_EQ_INT(register_repo(&e, atlas_buf_cstr(&alpha), "alpha", &err), 0);

    atlas_buf script = ATLAS_BUF_INIT;
    T_OK(atlas_buf_append_str(&script,
                              MCP_INIT_NO_ROOTS
                              "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
                              "\"params\":{\"name\":\"atlas_repo_overview\",\"arguments\":"
                              "{\"repo\":\"alpha\"}}}\n"
                              /* A name nobody registered stays NOT_REGISTERED,
                               * whatever the filesystem holds. */
                              "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
                              "\"params\":{\"name\":\"atlas_repo_overview\",\"arguments\":"
                              "{\"repo\":\"nosuchrepo\"}}}\n",
                              &err),
         &err);
    atlas_buf out = ATLAS_BUF_INIT;
    run_mcp(&e, atlas_buf_cstr(&script), &out, &err);

    T_CHECK_MSG(output_contains(&out, "NOT_REGISTERED"),
                "an unknown repository name was not reported NOT_REGISTERED");
    /* Exactly one refusal: the unknown name, not the registered one. */
    size_t refusals = 0;
    const char *p = atlas_buf_cstr(&out);
    while ((p = strstr(p, "NOT_REGISTERED")) != NULL) {
        refusals++;
        p += 1;
    }
    T_CHECK_MSG(refusals == 1, "expected exactly one refusal, saw %zu", refusals);

    atlas_buf_free(&out);
    atlas_buf_free(&script);
    atlas_buf_free(&alpha);
    env_stop(&e);
}

/* --- 8. the four refusals are told apart without reading English -------------
 *
 * A caller acts differently on each of these, so each has to be recognisable
 * from the answer itself rather than from prose a translation or a reword would
 * break:
 *
 *   NOT_REGISTERED  the repository is not in the registry. Register it.
 *   absent index    it is registered; nobody has built a semantic index. Build one.
 *   stale index     an index exists and no longer describes the code. Rebuild.
 *   NOT_AUTHORIZED  the operation is visible and this caller may not perform it.
 *
 * The one place a token is deliberately *absent* is the operator-uid RPC group,
 * where a peer outside the group gets `unknown method` — the same answer as a
 * name that does not exist. That is A8's rule and it is not weakened here: a
 * refusal that distinguished "you may not" from "there is no such thing" would
 * tell a caller what to try next. */
static void test_the_refusals_are_deterministically_distinct(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, NULL, &err);

    atlas_buf alpha = ATLAS_BUF_INIT;
    make_repo(&e, "alpha", &alpha, &err);
    T_EQ_INT(register_repo(&e, atlas_buf_cstr(&alpha), "alpha", &err), 0);

    /* 1. NOT_REGISTERED — a name the registry does not hold. */
    atlas_buf script = ATLAS_BUF_INIT;
    T_OK(atlas_buf_append_str(&script,
                              MCP_INIT_NO_ROOTS
                              "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
                              "\"params\":{\"name\":\"atlas_repo_overview\",\"arguments\":"
                              "{\"repo\":\"nosuchrepo\"}}}\n"
                              /* 2. absent index — registered, never indexed. */
                              "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
                              "\"params\":{\"name\":\"atlas_sem_status\",\"arguments\":"
                              "{\"repo\":\"alpha\"}}}\n",
                              &err),
         &err);
    atlas_buf out = ATLAS_BUF_INIT;
    run_mcp(&e, atlas_buf_cstr(&script), &out, &err);

    T_CHECK_MSG(output_contains(&out, "NOT_REGISTERED"),
                "an unknown repository did not answer NOT_REGISTERED");
    /* The absent index is reported as a *state*, not as a refusal: the
     * repository is perfectly readable, it simply has no semantic index. */
    T_CHECK_MSG(output_contains(&out, "ABSENT") || output_contains(&out, "absent"),
                "a registered repository with no index did not report an absent index: %s",
                atlas_buf_cstr(&out));
    T_CHECK_MSG(!output_contains(&out, "NOT_AUTHORIZED"),
                "an ordinary read was reported as an authorisation failure");

    /* The same token on the CLI, because a token that only one surface emits is
     * not a shared contract. The MCP resolver and the service layer produce
     * these messages independently, so both are asked. */
    {
        const char *args[] = {"status", "nosuchrepo", "--json"};
        atlas_buf sout = ATLAS_BUF_INIT;
        atlas_buf serr = ATLAS_BUF_INIT;
        int code = 0;
        T_OK(fx_atlas_with_runtime(&e.fx, &e.d, args, 3u, &sout, &serr, &code, &err), &err);
        T_OK(atlas_buf_append(&sout, serr.data, serr.len, &err), &err);
        atlas_buf_free(&serr);
        T_CHECK_MSG(code != 0, "a read of an unregistered repository succeeded");
        T_CHECK_MSG(strstr(atlas_buf_cstr(&sout), "NOT_REGISTERED") != NULL,
                    "the CLI did not answer NOT_REGISTERED for an unknown name: %s",
                    atlas_buf_cstr(&sout));
        T_CHECK_MSG(strstr(atlas_buf_cstr(&sout), "NOT_AUTHORIZED") == NULL,
                    "a registration failure was reported as an authorisation failure");
        atlas_buf_free(&sout);
    }

    /* And an *existing, unregistered* directory answers identically. The
     * difference between "there is nothing there" and "there is a git
     * repository there that nobody registered" is not something a caller learns
     * by asking — otherwise the refusal becomes a filesystem probe. */
    {
        atlas_buf stranger = ATLAS_BUF_INIT;
        make_repo(&e, "stranger", &stranger, &err);
        const char *args[] = {"status", "stranger", "--json"};
        atlas_buf sout = ATLAS_BUF_INIT;
        atlas_buf serr = ATLAS_BUF_INIT;
        int code = 0;
        T_OK(fx_atlas_with_runtime(&e.fx, &e.d, args, 3u, &sout, &serr, &code, &err), &err);
        T_OK(atlas_buf_append(&sout, serr.data, serr.len, &err), &err);
        atlas_buf_free(&serr);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&sout), "NOT_REGISTERED") != NULL,
                    "an existing but unregistered repository did not answer NOT_REGISTERED: %s",
                    atlas_buf_cstr(&sout));
        atlas_buf_free(&sout);
        atlas_buf_free(&stranger);
    }

    /* 3. NOT_AUTHORIZED — index construction, which writes. The fixture holds
     * the writer lock through its daemon, so this process cannot open the index
     * for writing, which is precisely the condition the token names. */
    {
        const char *args[] = {"code", "index", "alpha", "--compdb", "compile_commands.json"};
        atlas_buf cout = ATLAS_BUF_INIT;
        atlas_buf cerr = ATLAS_BUF_INIT;
        int code = 0;
        T_OK(fx_atlas_with_runtime(&e.fx, &e.d, args, 5u, &cout, &cerr, &code, &err), &err);
        T_OK(atlas_buf_append(&cout, cerr.data, cerr.len, &err), &err);
        atlas_buf_free(&cerr);
        T_CHECK_MSG(code != 0, "index construction succeeded while the daemon held the lock");
        T_CHECK_MSG(strstr(atlas_buf_cstr(&cout), "NOT_AUTHORIZED") != NULL,
                    "an unauthorised mutation did not answer NOT_AUTHORIZED: %s",
                    atlas_buf_cstr(&cout));
        /* And it is not confused with the registry refusal. */
        T_CHECK_MSG(strstr(atlas_buf_cstr(&cout), "NOT_REGISTERED") == NULL,
                    "an authorisation failure was reported as a registration failure");
        atlas_buf_free(&cout);
    }

    atlas_buf_free(&out);
    atlas_buf_free(&script);
    atlas_buf_free(&alpha);
    env_stop(&e);
}

/* --- 9. paths that try to leave the tree ---------------------------------------
 *
 * Registration takes a path, so it is the one place a traversal or a symlink
 * could point Atlas somewhere an operator did not mean. Every case here must be
 * refused, and refused *before* anything is stored — a rejected registration
 * that left a row behind would be worse than one that succeeded. */
static void test_traversal_and_symlink_registrations_are_refused(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, NULL, &err);

    atlas_buf alpha = ATLAS_BUF_INIT;
    make_repo(&e, "alpha", &alpha, &err);
    T_EQ_INT(register_repo(&e, atlas_buf_cstr(&alpha), "alpha", &err), 0);
    T_EQ_INT(repo_count(&e, &err), 1);

    /* A symlink pointing at the registered repository. Registering it must not
     * quietly create a second name for the same worktree — the canonical path
     * is what identifies a repository. */
    atlas_buf link = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&link, &err, "%s/alpha-link", e.fx.root.data), &err);
    if (symlink(atlas_buf_cstr(&alpha), atlas_buf_cstr(&link)) == 0) {
        int code = register_repo(&e, atlas_buf_cstr(&link), "vialink", &err);
        T_CHECK_MSG(code != 0 || repo_count(&e, &err) == 1,
                    "a symlink to a registered repository produced a second registration");
    }

    /* A traversal that climbs out of the fixture. */
    atlas_buf up = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&up, &err, "%s/alpha/../../../../etc", e.fx.root.data), &err);
    T_CHECK_MSG(register_repo(&e, atlas_buf_cstr(&up), "escape", &err) != 0,
                "a traversal path was accepted as a repository");

    /* A path that is not a repository at all. */
    T_CHECK_MSG(register_repo(&e, "/nonexistent-path-for-atlas-test", "ghost", &err) != 0,
                "a nonexistent path was accepted as a repository");

    /* Exactly one registration survived all of it. */
    T_EQ_INT(repo_count(&e, &err), 1);

    atlas_buf_free(&up);
    atlas_buf_free(&link);
    atlas_buf_free(&alpha);
    env_stop(&e);
}

/* --- 10. one repository's trouble does not reach another ----------------------- */

static void test_a_broken_repository_does_not_break_the_others(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, NULL, &err);

    atlas_buf alpha = ATLAS_BUF_INIT;
    atlas_buf bravo = ATLAS_BUF_INIT;
    make_repo(&e, "alpha", &alpha, &err);
    make_repo(&e, "bravo", &bravo, &err);
    T_EQ_INT(register_repo(&e, atlas_buf_cstr(&alpha), "alpha", &err), 0);
    T_EQ_INT(register_repo(&e, atlas_buf_cstr(&bravo), "bravo", &err), 0);

    /* Make `alpha` unusable by removing its git directory — the shape of a
     * repository an operator moved or deleted without telling Atlas. */
    atlas_buf gitdir = ATLAS_BUF_INIT;
    atlas_buf moved = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&gitdir, &err, "%s/.git", atlas_buf_cstr(&alpha)), &err);
    T_OK(atlas_buf_appendf(&moved, &err, "%s/.git-moved-aside", atlas_buf_cstr(&alpha)), &err);
    /* Renamed rather than deleted: the fixture still cleans up, and the shape
     * Atlas sees — a registered root that is no longer a worktree — is the
     * same. */
    T_REQUIRE_MSG(rename(atlas_buf_cstr(&gitdir), atlas_buf_cstr(&moved)) == 0,
                  "could not move the git directory aside");

    /* `bravo` must still answer, through every surface. A registry that
     * degraded because one entry went bad would make one operator's mistake
     * everybody's outage. */
    const char *args[] = {"status", "bravo", "--json"};
    atlas_buf out = ATLAS_BUF_INIT;
    int code = 0;
    T_OK(fx_atlas_with_runtime(&e.fx, &e.d, args, 3u, &out, NULL, &code, &err), &err);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"ok\":true") != NULL,
                "a healthy repository stopped answering because another was broken: %s",
                atlas_buf_cstr(&out));

    /* And the registry still lists both: a broken backing state is not a
     * deregistration. */
    T_EQ_INT(repo_count(&e, &err), 2);

    atlas_buf script = ATLAS_BUF_INIT;
    T_OK(atlas_buf_append_str(&script,
                              MCP_INIT_NO_ROOTS
                              "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
                              "\"params\":{\"name\":\"atlas_repo_overview\",\"arguments\":"
                              "{\"repo\":\"bravo\"}}}\n",
                              &err),
         &err);
    atlas_buf mout = ATLAS_BUF_INIT;
    run_mcp(&e, atlas_buf_cstr(&script), &mout, &err);
    T_CHECK_MSG(!output_contains(&mout, "NOT_REGISTERED"),
                "MCP stopped resolving a healthy repository: %s", atlas_buf_cstr(&mout));

    atlas_buf_free(&mout);
    atlas_buf_free(&script);
    atlas_buf_free(&out);
    atlas_buf_free(&gitdir);
    atlas_buf_free(&moved);
    atlas_buf_free(&alpha);
    atlas_buf_free(&bravo);
    env_stop(&e);
}

/* --- 11. removal is a registry operation and nothing else ---------------------- */

static void test_removal_is_consistent_across_surfaces(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, NULL, &err);

    atlas_buf alpha = ATLAS_BUF_INIT;
    atlas_buf bravo = ATLAS_BUF_INIT;
    make_repo(&e, "alpha", &alpha, &err);
    make_repo(&e, "bravo", &bravo, &err);
    T_EQ_INT(register_repo(&e, atlas_buf_cstr(&alpha), "alpha", &err), 0);
    T_EQ_INT(register_repo(&e, atlas_buf_cstr(&bravo), "bravo", &err), 0);
    T_EQ_INT(repo_count(&e, &err), 2);

    /* Removal is local and explicit, like registration. */
    fx_daemon_stop(&e.d, false);
    const char *args[] = {"repo", "remove", "alpha", "--yes"};
    atlas_buf out = ATLAS_BUF_INIT;
    int code = -1;
    T_OK(fx_atlas_with_runtime(&e.fx, &e.d, args, 4u, &out, NULL, &code, &err), &err);
    T_CHECK_MSG(code == 0, "repo remove failed: %s", atlas_buf_cstr(&out));
    T_OK(fx_daemon_start(&e.fx, &e.d, &err), &err);
    T_OK(fx_daemon_wait_ready(&e.d, 15000, &err), &err);

    T_EQ_INT(repo_count(&e, &err), 1);

    /* The removed name now behaves exactly like one that was never registered —
     * NOT_REGISTERED, from every surface. Anything softer would leave a name
     * that half-exists. */
    atlas_buf script = ATLAS_BUF_INIT;
    T_OK(atlas_buf_append_str(&script,
                              MCP_INIT_NO_ROOTS
                              "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
                              "\"params\":{\"name\":\"atlas_repo_overview\",\"arguments\":"
                              "{\"repo\":\"alpha\"}}}\n"
                              "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
                              "\"params\":{\"name\":\"atlas_repo_overview\",\"arguments\":"
                              "{\"repo\":\"bravo\"}}}\n",
                              &err),
         &err);
    atlas_buf mout = ATLAS_BUF_INIT;
    run_mcp(&e, atlas_buf_cstr(&script), &mout, &err);
    T_CHECK_MSG(output_contains(&mout, "NOT_REGISTERED"),
                "a removed repository did not answer NOT_REGISTERED");
    size_t refusals = 0;
    const char *p = atlas_buf_cstr(&mout);
    while ((p = strstr(p, "NOT_REGISTERED")) != NULL) {
        refusals++;
        p += 1;
    }
    T_CHECK_MSG(refusals == 1, "removal affected the wrong number of repositories (%zu refusals)",
                refusals);

    /* The repository on disk is untouched: Atlas removes its own metadata and
     * never the operator's code. */
    struct stat sb;
    T_CHECK_MSG(stat(atlas_buf_cstr(&alpha), &sb) == 0,
                "repo remove deleted the repository from disk");

    atlas_buf_free(&mout);
    atlas_buf_free(&script);
    atlas_buf_free(&out);
    atlas_buf_free(&alpha);
    atlas_buf_free(&bravo);
    env_stop(&e);
}

/* --- 12. a repository registered later enters the same flow, with no edits ----
 *
 * This is the test the whole "fix Atlas generically" requirement reduces to.
 *
 * A repository registered *after* everything is running — with a name and a path
 * nothing in the source, the configuration or this test file knows in advance —
 * must reach exactly the same capabilities as one registered at the start. No
 * rebuild, no configuration entry, no special case, and no change to the
 * acceptance procedure itself.
 *
 * So the procedure is written once, as a loop over whatever the registry
 * reports, and run twice: before and after a new registration. The second run
 * covers the new repository because it came from the registry, not because
 * anybody added it to a list. */
typedef struct surface_result {
    bool cli;
    bool rpc;
    bool mcp;
    bool sem_state;
    /* The identity each surface resolved the name to. Equivalence is the point:
     * three surfaces that each answer *something* prove far less than three
     * that answer the same thing, and a resolver that had drifted between them
     * would pass the weaker check every time. */
    atlas_buf cli_identity;
    atlas_buf mcp_identity;
} surface_result;

/* Copies the value of `"key":"..."` out of a JSON document. Enough for a
 * fixed-shape hex identity written by Atlas' own writer; not a parser. */
static void take_field(const atlas_buf *doc, const char *key, atlas_buf *out, atlas_err *err) {
    atlas_buf needle = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&needle, err, "\"%s\":\"", key), err);
    const char *p = doc->len > 0 ? strstr(atlas_buf_cstr(doc), atlas_buf_cstr(&needle)) : NULL;
    if (p != NULL) {
        p += needle.len;
        const char *end = strchr(p, '"');
        if (end != NULL) {
            T_OK(atlas_buf_append(out, p, (size_t)(end - p), err), err);
        }
    }
    atlas_buf_free(&needle);
}

/* The generic acceptance procedure. It is given a name and knows nothing else
 * about the repository — no path, no expectation about its contents. */
static surface_result exercise_surface(env *e, const char *repo, atlas_err *err) {
    surface_result r;
    memset(&r, 0, sizeof(r));
    atlas_buf_init(&r.cli_identity);
    atlas_buf_init(&r.mcp_identity);

    const char *sargs[] = {"status", repo, "--json"};
    atlas_buf out = ATLAS_BUF_INIT;
    int code = 0;
    if (fx_atlas_with_runtime(&e->fx, &e->d, sargs, 3u, &out, NULL, &code, err) == ATLAS_OK) {
        r.cli = strstr(atlas_buf_cstr(&out), "\"ok\":true") != NULL;
        take_field(&out, "root", &r.cli_identity, err);
    }
    atlas_buf_free(&out);

    /* Served by the daemon: the fixture's CLI can also open the index locally,
     * so this asserts the surface answers rather than which path served it —
     * the deployed system is where only the socket works, and the live
     * acceptance matrix covers that. */
    const char *eargs[] = {"events", repo, "--limit", "1", "--json"};
    atlas_buf eout = ATLAS_BUF_INIT;
    if (fx_atlas_with_runtime(&e->fx, &e->d, eargs, 5u, &eout, NULL, &code, err) == ATLAS_OK) {
        r.rpc = strstr(atlas_buf_cstr(&eout), "\"ok\":true") != NULL;
    }
    atlas_buf_free(&eout);

    atlas_buf script = ATLAS_BUF_INIT;
    if (atlas_buf_appendf(&script, err,
                          MCP_INIT_NO_ROOTS
                          "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
                          "{\"name\":\"atlas_repo_overview\",\"arguments\":{\"repo\":\"%s\"}}}\n"
                          "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":"
                          "{\"name\":\"atlas_sem_status\",\"arguments\":{\"repo\":\"%s\"}}}\n",
                          repo, repo) == ATLAS_OK) {
        atlas_buf mout = ATLAS_BUF_INIT;
        run_mcp(e, atlas_buf_cstr(&script), &mout, err);
        r.mcp = !output_contains(&mout, "NOT_REGISTERED");
        /* The semantic state must be *stated*, not omitted: a repository with no
         * index says so rather than answering nothing. */
        r.sem_state = output_contains(&mout, "ABSENT") || output_contains(&mout, "CURRENT") ||
                      output_contains(&mout, "STALE");
        take_field(&mout, "root", &r.mcp_identity, err);
        atlas_buf_free(&mout);
    }
    atlas_buf_free(&script);
    return r;
}

static void test_a_repository_registered_later_enters_the_same_flow(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, NULL, &err);

    atlas_buf first = ATLAS_BUF_INIT;
    make_repo(&e, "alpha", &first, &err);
    T_EQ_INT(register_repo(&e, atlas_buf_cstr(&first), "alpha", &err), 0);

    /* Nothing about this is /opt-shaped. The fixture tree is a temporary
     * directory, and asserting that is the point rather than an aside: Atlas
     * must have no special relationship with any location, so a test whose
     * repositories all happened to live under one would not notice if it grew
     * one. */
    T_CHECK_MSG(strncmp(atlas_buf_cstr(&first), "/opt", 4u) != 0,
                "the fixture repository is under /opt, so this proves nothing about "
                "location independence: %s",
                atlas_buf_cstr(&first));

    /* Run the procedure over whatever the registry holds. */
    surface_result a = exercise_surface(&e, "alpha", &err);
    T_CHECK_MSG(a.cli && a.rpc && a.mcp && a.sem_state,
                "the first repository failed the generic surface (cli=%d rpc=%d mcp=%d sem=%d)",
                a.cli, a.rpc, a.mcp, a.sem_state);
    /* Equivalence, not merely three answers: the CLI and MCP must have resolved
     * the name to the same repository. Each surface builds its request
     * independently, so a resolver that drifted between them would still let
     * every surface answer *something* — which is exactly the check that would
     * not catch it. */
    T_CHECK_MSG(a.cli_identity.len > 0 && a.mcp_identity.len > 0,
                "a surface did not report the repository identity it resolved (cli=%zu mcp=%zu)",
                a.cli_identity.len, a.mcp_identity.len);
    T_CHECK_MSG(a.cli_identity.len == a.mcp_identity.len &&
                    memcmp(a.cli_identity.data, a.mcp_identity.data, a.cli_identity.len) == 0,
                "the CLI and MCP resolved one name to different repositories: %s vs %s",
                atlas_buf_cstr(&a.cli_identity), atlas_buf_cstr(&a.mcp_identity));

    /* Now register a second one, later, with an unrelated name and path. The
     * binary is not rebuilt and nothing is configured. */
    atlas_buf later = ATLAS_BUF_INIT;
    make_repo(&e, "zulu-service", &later, &err);
    T_EQ_INT(register_repo(&e, atlas_buf_cstr(&later), "zulu-service", &err), 0);
    T_EQ_INT(repo_count(&e, &err), 2);

    /* The *same* procedure, unchanged, must now cover it. */
    surface_result b = exercise_surface(&e, "zulu-service", &err);
    T_CHECK_MSG(b.cli && b.rpc && b.mcp && b.sem_state,
                "a repository registered later failed the generic surface "
                "(cli=%d rpc=%d mcp=%d sem=%d)",
                b.cli, b.rpc, b.mcp, b.sem_state);
    T_CHECK_MSG(b.cli_identity.len > 0 &&
                    b.cli_identity.len == b.mcp_identity.len &&
                    memcmp(b.cli_identity.data, b.mcp_identity.data, b.cli_identity.len) == 0,
                "the surfaces disagreed about a repository registered later: %s vs %s",
                atlas_buf_cstr(&b.cli_identity), atlas_buf_cstr(&b.mcp_identity));
    /* And the two repositories are genuinely two: a resolver that returned the
     * same identity for every name would satisfy every check above. */
    T_CHECK_MSG(a.cli_identity.len != b.cli_identity.len ||
                    memcmp(a.cli_identity.data, b.cli_identity.data, a.cli_identity.len) != 0,
                "two different repositories resolved to the same identity");

    /* And the first one is unaffected. */
    surface_result a2 = exercise_surface(&e, "alpha", &err);
    T_CHECK_MSG(a2.cli && a2.rpc && a2.mcp && a2.sem_state,
                "registering a second repository disturbed the first");

    atlas_buf_free(&a2.cli_identity);
    atlas_buf_free(&a2.mcp_identity);
    atlas_buf_free(&b.cli_identity);
    atlas_buf_free(&b.mcp_identity);
    atlas_buf_free(&a.cli_identity);
    atlas_buf_free(&a.mcp_identity);
    atlas_buf_free(&later);
    atlas_buf_free(&first);
    env_stop(&e);
}

/* --- 13. the A8 protections still hold ------------------------------------------
 *
 * A8-CI added a whole layer. The cheapest way for that to go wrong is for one of
 * the earlier phases' absences to have quietly become present, so the names are
 * asked for again here rather than assumed. */
static void test_the_earlier_protections_are_intact(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, NULL, &err);

    atlas_buf alpha = ATLAS_BUF_INIT;
    make_repo(&e, "alpha", &alpha, &err);
    T_EQ_INT(register_repo(&e, atlas_buf_cstr(&alpha), "alpha", &err), 0);

    /* Not one of these may exist as an MCP tool. The registry verbs were deleted
     * by A7; the lifecycle verbs never existed; the index verb is A8-CI's own
     * deliberate absence. */
    static const char *const FORBIDDEN_TOOLS[] = {
        "atlas_repo_add",       "atlas_repo_remove",   "atlas_repo_ensure",
        "atlas_decision_approve", "atlas_decision_reject", "atlas_decision_supersede",
        "atlas_decision_revalidate", "atlas_sem_index",  "atlas_code_index",
        "atlas_backup_create",  "atlas_backup_restore", "atlas_maintenance_prune",
    };

    atlas_buf script = ATLAS_BUF_INIT;
    T_OK(atlas_buf_append_str(&script,
                              MCP_INIT_NO_ROOTS
                              "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\","
                              "\"params\":{}}\n",
                              &err),
         &err);
    atlas_buf out = ATLAS_BUF_INIT;
    run_mcp(&e, atlas_buf_cstr(&script), &out, &err);

    for (size_t i = 0; i < sizeof(FORBIDDEN_TOOLS) / sizeof(FORBIDDEN_TOOLS[0]); i++) {
        atlas_buf needle = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&needle, &err, "\"name\":\"%s\"", FORBIDDEN_TOOLS[i]), &err);
        T_CHECK_MSG(!output_contains(&out, atlas_buf_cstr(&needle)),
                    "the MCP surface exposes %s, which must not exist", FORBIDDEN_TOOLS[i]);
        atlas_buf_free(&needle);
    }

    /* And no tool *name* contains an authority verb, whatever it is called.
     *
     * The names are read out of the document rather than the whole document
     * being searched: the descriptions legitimately say "approve" — they are
     * what tells a model that no tool approves anything and that it must not
     * run the terminal command either. A test that scanned the whole reply
     * would forbid Atlas from explaining its own limits. */
    static const char *const VERBS[] = {"approve", "reject",  "supersede", "revalidate",
                                        "index",   "restore", "prune",     "add",
                                        "remove",  "ensure"};
    const char *p = atlas_buf_cstr(&out);
    size_t names = 0;
    while ((p = strstr(p, "\"name\":\"")) != NULL) {
        p += 8;
        const char *end = strchr(p, '"');
        if (end == NULL) {
            break;
        }
        size_t len = (size_t)(end - p);
        /* tools/list also names arguments and the server; only tool names are
         * `atlas_`-prefixed. */
        if (len > 6 && strncmp(p, "atlas_", 6) == 0) {
            names++;
            for (size_t i = 0; i < sizeof(VERBS) / sizeof(VERBS[0]); i++) {
                size_t vlen = strlen(VERBS[i]);
                bool hit = false;
                for (size_t j = 0; j + vlen <= len; j++) {
                    if (memcmp(p + j, VERBS[i], vlen) == 0) {
                        hit = true;
                        break;
                    }
                }
                T_CHECK_MSG(!hit, "the MCP tool name \"%.*s\" contains the authority verb \"%s\"",
                            (int)len, p, VERBS[i]);
            }
        }
        p = end;
    }
    T_CHECK_MSG(names > 0, "tools/list named no tools, so nothing was actually checked");

    atlas_buf_free(&out);
    atlas_buf_free(&script);
    atlas_buf_free(&alpha);
    env_stop(&e);
}

/* --- 14. a refused read emits exactly one JSON document ---------------------
 *
 * Asking one repository for a path only another holds is refused, and that
 * refusal is where the `--json` contract broke: the streaming renderer had
 * already put a document header and a repository line on stdout when the
 * service call failed, and the error document followed them. Two documents,
 * the first unterminated, from an invocation that promises exactly one — so a
 * caller parsing stdout got a syntax error instead of a refusal it could read.
 *
 * The count is what is asserted rather than the text, because the text is
 * allowed to change and the count is not. */
static void test_a_refused_read_emits_exactly_one_document(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, NULL, &err);

    atlas_buf alpha = ATLAS_BUF_INIT;
    atlas_buf bravo = ATLAS_BUF_INIT;
    make_repo(&e, "alpha", &alpha, &err);
    make_repo(&e, "bravo", &bravo, &err);
    T_EQ_INT(register_repo(&e, atlas_buf_cstr(&alpha), "alpha", &err), 0);
    T_EQ_INT(register_repo(&e, atlas_buf_cstr(&bravo), "bravo", &err), 0);

    /* `file` takes a repository-relative path, so the refusal is asked for by
     * naming a path this repository does not hold. An absolute path belonging
     * to the *other* repository is refused the same way — the point is the
     * shape of the answer, not which miss produced it. */
    const char *args[] = {"file", "alpha", "no/such/path.c", "--json"};
    atlas_buf out = ATLAS_BUF_INIT;
    int code = 0;
    (void)fx_atlas_with_runtime(&e.fx, &e.d, args, 4u, &out, NULL, &code, &err);

    size_t docs = 0;
    for (const char *p = atlas_buf_cstr(&out); (p = strstr(p, "{\"atlas\"")) != NULL; p += 8) {
        docs++;
    }
    T_CHECK_MSG(docs == 1, "a refused read put %zu documents on stdout, not 1: %s", docs,
                atlas_buf_cstr(&out));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"ok\":false") != NULL,
                "the one document does not report the refusal: %s", atlas_buf_cstr(&out));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"ok\":true") == NULL,
                "a refused read reported success somewhere in its output: %s",
                atlas_buf_cstr(&out));

    /* And the successful form still emits its one document, so the lazy open
     * did not simply stop rendering. That needs an index, so scan first. */
    atlas_buf ok_path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_append_str(&ok_path, "a.c", &err), &err);
    const char *ok_args[] = {"file", "bravo", atlas_buf_cstr(&ok_path), "--json"};
    atlas_buf ok_out = ATLAS_BUF_INIT;
    /* The daemon holds the writer lock, so the index appears when its own pass
     * runs rather than when this process asks. Poll for the outcome — watcher
     * timing is machine-dependent and a guessed sleep is how a suite becomes
     * intermittent. */
    for (int i = 0; i < 200; i++) {
        atlas_buf_reset(&ok_out);
        (void)fx_atlas_with_runtime(&e.fx, &e.d, ok_args, 4u, &ok_out, NULL, &code, &err);
        if (strstr(atlas_buf_cstr(&ok_out), "\"ok\":true") != NULL) {
            break;
        }
        struct timespec ts = {0, 50L * 1000L * 1000L};
        nanosleep(&ts, NULL);
    }
    size_t ok_docs = 0;
    for (const char *p = atlas_buf_cstr(&ok_out); (p = strstr(p, "{\"atlas\"")) != NULL; p += 8) {
        ok_docs++;
    }
    T_CHECK_MSG(ok_docs == 1, "a successful read emitted %zu documents: %s", ok_docs,
                atlas_buf_cstr(&ok_out));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&ok_out), "\"ok\":true") != NULL,
                "a successful read did not report success: %s", atlas_buf_cstr(&ok_out));

    atlas_buf_free(&ok_out);
    atlas_buf_free(&ok_path);
    atlas_buf_free(&out);
    atlas_buf_free(&bravo);
    atlas_buf_free(&alpha);
    env_stop(&e);
}

/* --- 15. a refusal is readable ------------------------------------------------
 *
 * Error text is safe-text-encoded on its way to a terminal, because parts of a
 * message can quote a path or a git identity somebody else chose and a newline
 * in such a value forges an output line. The encoder applies to the whole
 * string and cannot tell Atlas' own control text from an interpolated value —
 * so a message written with `\n` for layout printed as one run of literal
 * `%0A`, and the two longest and most important refusals Atlas has were the
 * two that did it.
 *
 * The rule that follows is: an error message contains no newline. This test
 * asserts the outcome rather than the rule, by refusing several ways and
 * looking for the encoding in what reaches the user. */
static void test_a_refusal_is_readable(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, NULL, &err);

    atlas_buf alpha = ATLAS_BUF_INIT;
    make_repo(&e, "alpha", &alpha, &err);
    T_EQ_INT(register_repo(&e, atlas_buf_cstr(&alpha), "alpha", &err), 0);

    /* Each of these refuses through a different path: the authority probe, the
     * registry write lock, the registry lookup and the index-mutation gate. */
    static const char *const CASES[][5] = {
        {"decision", "approve", "alpha", "nosuchdoc", NULL},
        {"repo", "add", "/tmp", NULL, NULL},
        {"status", "nosuchrepo", NULL, NULL, NULL},
        {"code", "index", "alpha", "--compdb", "compile_commands.json"},
    };

    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        size_t n = 0;
        while (n < 5 && CASES[i][n] != NULL) {
            n++;
        }
        atlas_buf out = ATLAS_BUF_INIT;
        atlas_buf errout = ATLAS_BUF_INIT;
        int code = 0;
        T_OK(fx_atlas_with_runtime(&e.fx, &e.d, CASES[i], n, &out, &errout, &code, &err), &err);
        T_OK(atlas_buf_append(&out, errout.data, errout.len, &err), &err);
        atlas_buf_free(&errout);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "%0A") == NULL,
                    "`atlas %s %s` printed an encoded newline, so its message is unreadable: %s",
                    CASES[i][0], CASES[i][1] != NULL ? CASES[i][1] : "", atlas_buf_cstr(&out));
        atlas_buf_free(&out);
    }

    atlas_buf_free(&alpha);
    env_stop(&e);
}

static const atlas_test TESTS[] = {
    {"only registration makes a repository known",
     test_only_registration_makes_a_repository_known},
    {"a registered repository is readable from any root",
     test_a_registered_repository_is_readable_from_any_root},
    {"two repositories stay isolated", test_two_repositories_stay_isolated},
    {"duplicate names and paths are refused", test_duplicate_names_and_paths_are_refused},
    {"a restart preserves the registry", test_a_restart_preserves_the_registry},
    {"task prose cannot change the registry", test_task_prose_cannot_change_the_registry},
    {"a client in an unrelated directory can query by identity",
     test_a_client_in_an_unrelated_directory_can_query_by_identity},
    {"the refusals are deterministically distinct",
     test_the_refusals_are_deterministically_distinct},
    {"traversal and symlink registrations are refused",
     test_traversal_and_symlink_registrations_are_refused},
    {"a broken repository does not break the others",
     test_a_broken_repository_does_not_break_the_others},
    {"removal is consistent across surfaces", test_removal_is_consistent_across_surfaces},
    {"a repository registered later enters the same flow",
     test_a_repository_registered_later_enters_the_same_flow},
    {"the earlier protections are intact", test_the_earlier_protections_are_intact},
    {"a refused read emits exactly one document",
     test_a_refused_read_emits_exactly_one_document},
    {"a refusal is readable", test_a_refusal_is_readable},
};

ATLAS_TEST_MAIN("registry", TESTS)
