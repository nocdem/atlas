/* Atlas - decision links survive the socket, and are validated the same on both
 * sides of it.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Two defects motivated this suite, and both were invisible to every test that
 * existed when they shipped:
 *
 *  1. **A client that does not own the index crashed on `--path` and
 *     `--symbol-link`.** Under a system deployment the CLI runs with
 *     `ctx == NULL` — that is the documented arrangement, not an error — and
 *     `build_op` resolved the repository and read the file row through that
 *     context anyway. `atlas_service_require_repo` dereferences `ctx->db`, so
 *     the client died of SIGSEGV before a request was built. `--commit` and
 *     `--alternative` survived because neither touches the database, which is
 *     why the crash looked like a link-kind problem rather than a mode problem.
 *
 *  2. **`--decision-link` was silently dropped over the socket.** `build_op`
 *     built the `relates_to` links, `op_to_params` serialised paths, commits
 *     and symbols and not those, and the daemon had no parameter to read them
 *     from. The command exited 0 and recorded nothing.
 *
 * The shape of the test follows from that: every assertion below is made
 * against a daemon over a socket, because the local path was never the one that
 * broke. What the local path owes is agreement, and the last case checks it
 * directly by recording the same links both ways and comparing the documents.
 *
 * This does not test the cross-uid deployment, and cannot: a test binary may
 * not create a second OS principal, and the system socket's path comes from a
 * root-owned policy this suite must not write. What it tests is the *mode* —
 * a client with no context talking to a daemon that owns the index — which is
 * the surface both defects lived on. The uid split itself is exercised against
 * the real system deployment, where it is not a fixture.
 *
 * Backup over the socket is absent here for the same reason and one more: a
 * fixture is a single-user install, so `backup create` correctly stays local
 * and the operator gate is never reached. Testing it here would assert the
 * fixture's arrangement rather than the deployment's. */
#include <stdlib.h>
#include <string.h>

#include "atlas/service.h"
#include "atlas_test.h"
#include "support/fixture.h"

typedef struct env {
    fixture fx;
    fx_daemon d;
} env;

/* One repository with two files and a symbol that is defined exactly once, plus
 * a second symbol defined twice — the ambiguous case a symbol link must refuse
 * to bind a file context to. */
static void env_open(env *e) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&e->fx, &err), &err);
    T_OK(fx_init_repo(&e->fx, fx_repo(&e->fx), NULL, &err), &err);
    T_OK(fx_write(fx_repo(&e->fx), "one.c", "int only_here(void){return 0;}\nint twice(void){return 1;}\n",
                  &err),
         &err);
    T_OK(fx_write(fx_repo(&e->fx), "two.c", "int twice(void){return 2;}\n", &err), &err);
    T_OK(fx_add_all(&e->fx, fx_repo(&e->fx), &err), &err);
    T_OK(fx_commit(&e->fx, fx_repo(&e->fx), "first", &err), &err);
    {
        const char *add[] = {"--data-dir", fx_data_dir(&e->fx), "repo", "add",
                             fx_repo(&e->fx), "--name", "proj"};
        int code = -1;
        T_OK(fx_atlas(add, 7u, NULL, NULL, &code, &err), &err);
        T_REQUIRE(code == 0);
    }
    {
        const char *scan[] = {"--data-dir", fx_data_dir(&e->fx), "scan", "proj"};
        int code = -1;
        T_OK(fx_atlas(scan, 4u, NULL, NULL, &code, &err), &err);
        T_REQUIRE(code == 0);
    }
    {
        const char *csync[] = {"--data-dir", fx_data_dir(&e->fx), "code", "sync", "proj"};
        int code = -1;
        T_OK(fx_atlas(csync, 5u, NULL, NULL, &code, &err), &err);
        T_REQUIRE(code == 0);
    }
    fx_daemon_init(&e->d);
    T_OK(fx_daemon_start(&e->fx, &e->d, &err), &err);
    T_OK(fx_daemon_wait_ready(&e->d, 15000, &err), &err);
}

/* env_open, plus a second registered repository. Split rather than folded into
 * env_open because every other case wants exactly one repository, and a second
 * one changes what "the only decision here" means. */
static void env_open_two(env *e) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&e->fx, &err), &err);
    T_OK(fx_init_repo(&e->fx, fx_repo(&e->fx), NULL, &err), &err);
    T_OK(fx_write(fx_repo(&e->fx), "one.c", "int only_here(void){return 0;}\n", &err), &err);
    T_OK(fx_add_all(&e->fx, fx_repo(&e->fx), &err), &err);
    T_OK(fx_commit(&e->fx, fx_repo(&e->fx), "first", &err), &err);
    T_OK(fx_mkdir(e->fx.root.data, "other", &err), &err);
    atlas_buf other = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&other, &err, "%s/other", e->fx.root.data), &err);
    T_OK(fx_init_repo(&e->fx, atlas_buf_cstr(&other), NULL, &err), &err);
    T_OK(fx_write(atlas_buf_cstr(&other), "x.c", "int x(void){return 0;}\n", &err), &err);
    T_OK(fx_add_all(&e->fx, atlas_buf_cstr(&other), &err), &err);
    T_OK(fx_commit(&e->fx, atlas_buf_cstr(&other), "first", &err), &err);
    const char *names[2] = {"proj", "other"};
    const char *dirs[2] = {fx_repo(&e->fx), atlas_buf_cstr(&other)};
    for (size_t i = 0; i < 2u; i++) {
        const char *add[] = {"--data-dir", fx_data_dir(&e->fx), "repo", "add",
                             dirs[i],      "--name",            names[i]};
        int code = -1;
        T_OK(fx_atlas(add, 7u, NULL, NULL, &code, &err), &err);
        T_REQUIRE(code == 0);
        const char *scan[] = {"--data-dir", fx_data_dir(&e->fx), "scan", names[i]};
        T_OK(fx_atlas(scan, 4u, NULL, NULL, &code, &err), &err);
        T_REQUIRE(code == 0);
    }
    atlas_buf_free(&other);
    fx_daemon_init(&e->d);
    T_OK(fx_daemon_start(&e->fx, &e->d, &err), &err);
    T_OK(fx_daemon_wait_ready(&e->d, 15000, &err), &err);
}

static void env_close(env *e) {
    fx_daemon_stop(&e->d, false);
    fx_daemon_free(&e->d);
    fx_close(&e->fx);
}

/* Runs the CLI against the fixture daemon's runtime directory, i.e. as a client
 * whose request must be answered over the socket. */
static int run_remote(env *e, const char *const *args, size_t n, atlas_buf *out) {
    atlas_err err;
    atlas_err_init(&err);
    int code = -1;
    T_OK(fx_atlas_with_runtime(&e->fx, &e->d, args, n, out, NULL, &code, &err), &err);
    /* 139 is what a SIGSEGV becomes once a shell has seen it; the process API
     * reports the signal directly. Either way it is never an answer. */
    T_REQUIRE(code != 139);
    return code;
}

static void extract_uid(const atlas_buf *json, char *out, size_t n) {
    const char *s = atlas_buf_cstr(json);
    const char *k = strstr(s, "\"decision\":\"");
    T_REQUIRE(k != NULL);
    k += strlen("\"decision\":\"");
    const char *end = strchr(k, '"');
    T_REQUIRE(end != NULL);
    size_t len = (size_t)(end - k);
    T_REQUIRE(len + 1u < n);
    memcpy(out, k, len);
    out[len] = '\0';
}

/* A propose carrying every link kind at once, over the socket.
 *
 * This is the case that used to be a segfault. It asserts more than "did not
 * crash": the links must come back from `decision show`, because a client that
 * dropped them and a daemon that never received them would both exit 0. */
static void test_propose_all_link_kinds(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    atlas_buf head = ATLAS_BUF_INIT;
    {
        const char *args[] = {"--json", "status", "proj"};
        T_REQUIRE(run_remote(&e, args, 3u, &head) == 0);
    }
    /* The scanned head, taken from the daemon's own answer rather than composed
     * here, so the commit link is one the index actually holds. */
    char oid[64];
    {
        const char *s = atlas_buf_cstr(&head);
        const char *k = strstr(s, "\"head\":\"");
        T_REQUIRE(k != NULL);
        k += strlen("\"head\":\"");
        const char *end = strchr(k, '"');
        T_REQUIRE(end != NULL);
        size_t len = (size_t)(end - k);
        T_REQUIRE(len + 1u < sizeof oid);
        memcpy(oid, k, len);
        oid[len] = '\0';
    }
    atlas_buf_free(&head);

    /* A base decision first, so the second can relate to it. */
    atlas_buf out = ATLAS_BUF_INIT;
    char base_uid[64];
    {
        const char *args[] = {"--json", "decision", "propose", "proj",
                              "--title", "base", "--decision", "the base"};
        T_REQUIRE(run_remote(&e, args, 8u, &out) == 0);
        extract_uid(&out, base_uid, sizeof base_uid);
    }
    atlas_buf_reset(&out);

    char uid[64];
    {
        const char *args[] = {"--json",      "decision",    "propose",  "proj",
                              "--title",     "everything",  "--decision", "carries every link kind",
                              "--path",      "one.c",       "--commit", oid,
                              "--symbol-link", "only_here", "--decision-link", base_uid};
        T_REQUIRE(run_remote(&e, args, 16u, &out) == 0);
        extract_uid(&out, uid, sizeof uid);
    }
    atlas_buf_reset(&out);

    {
        const char *args[] = {"--json", "decision", "show", "proj", uid};
        T_REQUIRE(run_remote(&e, args, 5u, &out) == 0);
        const char *s = atlas_buf_cstr(&out);
        /* Each kind present, and the relates_to target the one that was asked
         * for — the defect recorded zero of these while exiting 0. */
        T_REQUIRE(strstr(s, "\"path\"") != NULL);
        T_REQUIRE(strstr(s, "one.c") != NULL);
        T_REQUIRE(strstr(s, "\"commit\"") != NULL);
        T_REQUIRE(strstr(s, "\"symbol\"") != NULL);
        T_REQUIRE(strstr(s, "only_here") != NULL);
        T_REQUIRE(strstr(s, "\"relates_to\"") != NULL);
        T_REQUIRE(strstr(s, base_uid) != NULL);
        /* The snapshot the daemon took from its own index, not one the client
         * asserted: a path link that resolved is CURRENT. */
        T_REQUIRE(strstr(s, "CURRENT") != NULL);
    }
    atlas_buf_free(&out);
    env_close(&e);
}

/* revise keeps what it is given and adds to it; link add is the same write with
 * the previous links re-sent. Neither may crash, and both must persist. */
static void test_revise_and_link_add(void) {
    env e;
    env_open(&e);
    atlas_buf out = ATLAS_BUF_INIT;

    char a_uid[64];
    {
        const char *args[] = {"--json", "decision", "propose", "proj",
                              "--title", "a", "--decision", "first"};
        T_REQUIRE(run_remote(&e, args, 8u, &out) == 0);
        extract_uid(&out, a_uid, sizeof a_uid);
    }
    atlas_buf_reset(&out);
    char b_uid[64];
    {
        const char *args[] = {"--json", "decision", "propose", "proj",
                              "--title", "b", "--decision", "second", "--path", "two.c"};
        T_REQUIRE(run_remote(&e, args, 10u, &out) == 0);
        extract_uid(&out, b_uid, sizeof b_uid);
    }
    atlas_buf_reset(&out);

    /* A revise that re-sends the path and adds a relation. */
    {
        const char *args[] = {"--json", "decision", "revise", "proj", b_uid,
                              "--title", "b", "--decision", "second, revised",
                              "--path", "two.c", "--decision-link", a_uid};
        T_REQUIRE(run_remote(&e, args, 13u, &out) == 0);
    }
    atlas_buf_reset(&out);
    {
        const char *args[] = {"--json", "decision", "show", "proj", b_uid};
        T_REQUIRE(run_remote(&e, args, 5u, &out) == 0);
        const char *s = atlas_buf_cstr(&out);
        T_REQUIRE(strstr(s, "two.c") != NULL);
        T_REQUIRE(strstr(s, a_uid) != NULL);
        T_REQUIRE(strstr(s, "\"revision\":2") != NULL);
    }
    atlas_buf_reset(&out);

    /* `decision link add` reads the document and re-proposes it with the whole
     * link set, so it exercises the re-send path that a naive fix breaks. */
    char c_uid[64];
    {
        const char *args[] = {"--json", "decision", "propose", "proj",
                              "--title", "c", "--decision", "third"};
        T_REQUIRE(run_remote(&e, args, 8u, &out) == 0);
        extract_uid(&out, c_uid, sizeof c_uid);
    }
    atlas_buf_reset(&out);
    {
        const char *args[] = {"--json", "decision", "link", "add", "proj", b_uid, c_uid};
        T_REQUIRE(run_remote(&e, args, 7u, &out) == 0);
    }
    atlas_buf_reset(&out);
    {
        const char *args[] = {"--json", "decision", "show", "proj", b_uid};
        T_REQUIRE(run_remote(&e, args, 5u, &out) == 0);
        const char *s = atlas_buf_cstr(&out);
        /* Both relations, and the path that was there before the link add. */
        T_REQUIRE(strstr(s, a_uid) != NULL);
        T_REQUIRE(strstr(s, c_uid) != NULL);
        T_REQUIRE(strstr(s, "two.c") != NULL);
    }
    atlas_buf_free(&out);
    env_close(&e);
}

/* Every refusal, and the thing they have in common: a controlled non-zero exit
 * and no revision written. */
static void test_refusals(void) {
    env e;
    env_open(&e);
    atlas_buf out = ATLAS_BUF_INIT;

    char uid[64];
    {
        const char *args[] = {"--json", "decision", "propose", "proj",
                              "--title", "target", "--decision", "a target"};
        T_REQUIRE(run_remote(&e, args, 8u, &out) == 0);
        extract_uid(&out, uid, sizeof uid);
    }
    atlas_buf_reset(&out);

    /* A symbol defined in two files binds no file context; the link is still
     * recorded, and its currency is AMBIGUOUS rather than a guess at one of
     * them. That is the behaviour the manifest depends on when it refuses to
     * use a symbol with more than one definition site. */
    {
        const char *args[] = {"--json", "decision", "propose", "proj", "--title", "amb",
                              "--decision", "ambiguous symbol", "--symbol-link", "twice"};
        T_REQUIRE(run_remote(&e, args, 10u, &out) == 0);
        const char *s = atlas_buf_cstr(&out);
        char amb[64];
        extract_uid(&out, amb, sizeof amb);
        (void)s;
        atlas_buf_reset(&out);
        const char *show[] = {"--json", "decision", "show", "proj", amb};
        T_REQUIRE(run_remote(&e, show, 5u, &out) == 0);
        T_REQUIRE(strstr(atlas_buf_cstr(&out), "AMBIGUOUS") != NULL);
    }
    atlas_buf_reset(&out);

    /* A path that is not in the index is recorded and reported MISSING rather
     * than refused: the link says what the decision is about, and Atlas saying
     * "not indexed" is more useful than refusing to record the claim. */
    {
        const char *args[] = {"--json", "decision", "propose", "proj", "--title", "gone",
                              "--decision", "untracked path", "--path", "not-here.c"};
        T_REQUIRE(run_remote(&e, args, 10u, &out) == 0);
        char g[64];
        extract_uid(&out, g, sizeof g);
        atlas_buf_reset(&out);
        const char *show[] = {"--json", "decision", "show", "proj", g};
        T_REQUIRE(run_remote(&e, show, 5u, &out) == 0);
        T_REQUIRE(strstr(atlas_buf_cstr(&out), "MISSING") != NULL);
    }
    atlas_buf_reset(&out);

    /* Self-relation, on a revise where the source uid is known. */
    {
        const char *args[] = {"decision", "revise", "proj", uid, "--title", "target",
                              "--decision", "self", "--decision-link", uid};
        int code = run_remote(&e, args, 10u, &out);
        T_REQUIRE(code != 0);
    }
    atlas_buf_reset(&out);

    /* A relation to a document that does not exist. */
    {
        const char *args[] = {"decision", "propose", "proj", "--title", "dangling",
                              "--decision", "dangling", "--decision-link",
                              "atlas-dec-00000000000000000000000000000000"};
        int code = run_remote(&e, args, 9u, &out);
        T_REQUIRE(code != 0);
    }
    atlas_buf_reset(&out);

    /* The same relation twice in one request. */
    {
        const char *args[] = {"decision", "propose", "proj", "--title", "dupe",
                              "--decision", "dupe", "--decision-link", uid,
                              "--decision-link", uid};
        int code = run_remote(&e, args, 11u, &out);
        T_REQUIRE(code != 0);
    }
    atlas_buf_reset(&out);

    /* Nothing above may have left a revision behind. The two decisions this
     * test created deliberately (`target`, plus the ambiguous and missing-path
     * cases) are the only documents, and `target` is still at revision 1. */
    {
        const char *args[] = {"--json", "decision", "show", "proj", uid};
        T_REQUIRE(run_remote(&e, args, 5u, &out) == 0);
        T_REQUIRE(strstr(atlas_buf_cstr(&out), "\"revision\":1") != NULL);
    }
    atlas_buf_free(&out);
    env_close(&e);
}

/* A relation may not cross repositories, and the refusal must come from the
 * write point rather than from whichever caller happened to check. */
static void test_cross_repo_refused(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    /* Registration takes the writer lock, which the daemon holds for its whole
     * life — so the second repository is built and registered before the daemon
     * starts, exactly as an operator would have to. */
    env_open_two(&e);
    atlas_buf out = ATLAS_BUF_INIT;


    char foreign[64];
    {
        const char *args[] = {"--json", "decision", "propose", "other",
                              "--title", "elsewhere", "--decision", "in another repo"};
        T_REQUIRE(run_remote(&e, args, 8u, &out) == 0);
        extract_uid(&out, foreign, sizeof foreign);
    }
    atlas_buf_reset(&out);
    {
        const char *args[] = {"decision", "propose", "proj", "--title", "crosser",
                              "--decision", "crosses", "--decision-link", foreign};
        T_REQUIRE(run_remote(&e, args, 9u, &out) != 0);
    }
    atlas_buf_free(&out);
    env_close(&e);
}

/* The local path and the socket path must record the same document.
 *
 * Same links, two routes, and the content hash is what decides: it is computed
 * from the revision's content, so two documents with the same hash are the same
 * document however they were written. */
static void test_local_socket_equivalence(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf remote_out = ATLAS_BUF_INIT;
    atlas_buf local_out = ATLAS_BUF_INIT;

    const char *body[] = {"--title", "same", "--decision", "identical both ways",
                          "--path",  "one.c", "--symbol-link", "only_here"};

    {
        const char *args[] = {"--json", "decision", "propose", "proj",
                              body[0], body[1], body[2], body[3],
                              body[4], body[5], body[6], body[7]};
        T_REQUIRE(run_remote(&e, args, 12u, &remote_out) == 0);
    }
    /* The daemon must be stopped before the local path can take the writer
     * lock; that is the same rule a real operator follows. */
    fx_daemon_stop(&e.d, false);
    {
        const char *args[] = {"--json", "--data-dir", fx_data_dir(&e.fx), "decision", "propose",
                              "proj",   body[0],      body[1],            body[2],    body[3],
                              body[4],  body[5],      body[6],            body[7]};
        int code = -1;
        T_OK(fx_atlas(args, 14u, &local_out, NULL, &code, &err), &err);
        T_REQUIRE(code == 0);
    }

    /* Different documents — a propose always creates one — but identical
     * content, which is what "the two paths agree" means. */
    const char *r = strstr(atlas_buf_cstr(&remote_out), "\"content_hash\":\"");
    const char *l = strstr(atlas_buf_cstr(&local_out), "\"content_hash\":\"");
    T_REQUIRE(r != NULL && l != NULL);
    T_REQUIRE_MSG(strncmp(r, l, strlen("\"content_hash\":\"") + 64u) == 0,
                  "remote=%.80s local=%.80s", r, l);

    atlas_buf_free(&remote_out);
    atlas_buf_free(&local_out);
    fx_daemon_free(&e.d);
    fx_close(&e.fx);
}

static const atlas_test TESTS[] = {
    {"propose carrying path, commit, symbol and decision links", test_propose_all_link_kinds},
    {"revise and link add keep and extend the link set", test_revise_and_link_add},
    {"ambiguous, missing, self, dangling and duplicate", test_refusals},
    {"a relation may not cross repositories", test_cross_repo_refused},
    {"the local path and the socket record the same document", test_local_socket_equivalence},
};

ATLAS_TEST_MAIN("decision_link_rpc", TESTS)
