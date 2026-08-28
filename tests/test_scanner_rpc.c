/* Atlas - A13: the scanner channel, and the uid comparison that carries it.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A scanner runs as a repository's owner and reads a tree the daemon cannot.
 * What it may report about is decided by one comparison — the peer's uid from
 * `SO_PEERCRED` against `repositories.scanner_uid` — and these cases are that
 * comparison from both directions.
 *
 * The edge is driven through `atlas_server_dispatch`, which `daemon_internal.h`
 * exposes so the protocol can be tested without a socket. Everything above the
 * socket is the shipped code: the method table, the peer uid, the refusal, the
 * parser and the database. What is substituted is the carriage of bytes — and
 * the peer uid, which is the point: over a real socket every peer would be this
 * process's own uid, and a test that could only ever ask as itself could not
 * show that another uid is refused. `tests/test_plan_rpc.c` opens the same way
 * and for the same reason.
 */
#define _GNU_SOURCE 1

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/db.h"
#include "atlas/ipc.h"
#include "atlas_test.h"
#include "daemon/daemon_internal.h"
#include "support/fixture.h"

typedef struct env {
    fixture fx;
    atlas_buf db_path;
    atlas_db *db;
    atlas_server_ctx ctx;
    int64_t mine;    /* a repository this process's uid may scan */
    int64_t theirs;  /* a repository it may not */
    int64_t nobodys; /* a repository with no scanner assigned */
} env;

/* Registered through the CLI, because that is the only way a repository is ever
 * registered. Three repositories, each answering a different question about the
 * comparison. */
static void env_open(env *e) {
    atlas_err err;
    atlas_err_init(&err);
    memset(e, 0, sizeof(*e));
    T_OK(fx_open(&e->fx, &err), &err);
    atlas_buf_init(&e->db_path);

    T_OK(fx_init_repo(&e->fx, fx_repo(&e->fx), NULL, &err), &err);
    T_OK(fx_write(fx_repo(&e->fx), "a.c", "int main(void){return 0;}\n", &err), &err);
    T_OK(fx_add_all(&e->fx, fx_repo(&e->fx), &err), &err);
    T_OK(fx_commit(&e->fx, fx_repo(&e->fx), "first", &err), &err);
    {
        const char *add[] = {"--data-dir", fx_data_dir(&e->fx), "repo", "add",
                             fx_repo(&e->fx), "--name", "mine"};
        int code = -1;
        T_OK(fx_atlas(add, 7u, NULL, NULL, &code, &err), &err);
        T_REQUIRE(code == 0);
    }

    T_OK(atlas_buf_appendf(&e->db_path, &err, "%s/atlas.db", fx_data_dir(&e->fx)), &err);
    T_OK(atlas_db_open(atlas_buf_cstr(&e->db_path), &e->db, &err), &err);
    T_OK(atlas_db_migrate(e->db, &err), &err);

    atlas_repo_info ri;
    atlas_repo_info_init(&ri);
    bool found = false;
    T_OK(atlas_db_repo_get(e->db, "mine", &ri, &found, &err), &err);
    T_REQUIRE(found);
    e->mine = ri.id;
    /* Registration derived this from the root's owner, which is this process. */
    T_EQ_INT((int)ri.scanner_uid, (int)getuid());
    atlas_repo_info_free(&ri);

    /* Two more rows, written directly: registering them would need two more
     * real git repositories, and what these cases are about is the stored uid,
     * not how it got there. */
    {
        atlas_repo_identity id;
        memset(&id, 0, sizeof(id));
        id.root = "/tmp/atlas-a13-theirs";
        id.root_len = strlen(id.root);
        id.common_dir = "/tmp/atlas-a13-theirs/.git";
        id.common_dir_len = strlen(id.common_dir);
        id.git_dir = id.common_dir;
        id.git_dir_len = id.common_dir_len;
        id.object_format = "sha1";
        T_OK(atlas_db_repo_add(e->db, "theirs", &id, &e->theirs, &err), &err);
        T_OK(atlas_db_repo_set_scanner_uid(e->db, e->theirs, (int64_t)getuid() + 1, &err), &err);

        memset(&id, 0, sizeof(id));
        id.root = "/tmp/atlas-a13-nobodys";
        id.root_len = strlen(id.root);
        id.common_dir = "/tmp/atlas-a13-nobodys/.git";
        id.common_dir_len = strlen(id.common_dir);
        id.git_dir = id.common_dir;
        id.git_dir_len = id.common_dir_len;
        id.object_format = "sha1";
        T_OK(atlas_db_repo_add(e->db, "nobodys", &id, &e->nobodys, &err), &err);
        T_OK(atlas_db_repo_set_scanner_uid(e->db, e->nobodys, 0, &err), &err);
    }
    atlas_db_close(e->db);
    e->db = NULL;

    e->ctx.db_path = atlas_buf_cstr(&e->db_path);
    e->ctx.data_dir = fx_data_dir(&e->fx);
}

static void env_close(env *e) {
    atlas_buf_free(&e->db_path);
    fx_close(&e->fx);
}

/* One request in, one response out, parsed with the client's own parser so what
 * the test reads is what a client would read. */
static atlas_ipc_response *call(env *e, long long uid, const char *method, const char *params,
                                atlas_buf *raw) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf payload = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&payload, &err, "{\"id\":\"t\",\"method\":\"%s\",\"params\":%s}", method,
                           params),
         &err);
    atlas_buf_reset(raw);
    T_OK(atlas_server_dispatch(&e->ctx, payload.data, payload.len, uid, (int64_t)getpid(), raw,
                               &err),
         &err);
    atlas_buf_free(&payload);
    atlas_ipc_response *resp = NULL;
    T_OK(atlas_ipc_response_parse(raw->data, raw->len, &resp, &err), &err);
    T_REQUIRE(resp != NULL);
    return resp;
}

static void test_a_scanner_is_told_its_own_repositories_and_no_others(void) {
    env e;
    env_open(&e);

    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = call(&e, (long long)getuid(), "scanner.poll", "{}", &raw);
    T_CHECK_MSG(atlas_ipc_response_ok(r), "scanner.poll failed: %s", atlas_buf_cstr(&raw));

    const char *body = atlas_buf_cstr(&raw);
    T_CHECK_MSG(strstr(body, "\"mine\"") != NULL, "own repository missing: %s", body);
    T_CHECK_MSG(strstr(body, "\"theirs\"") == NULL, "another uid's repository listed: %s", body);
    T_CHECK_MSG(strstr(body, "\"nobodys\"") == NULL, "an unassigned repository listed: %s", body);

    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    env_close(&e);
}

/* The refusal must not become an inventory: a uid that owns nothing learns that
 * it owns nothing, and not what else is registered. */
static void test_a_uid_that_owns_nothing_is_refused_and_names_no_repository(void) {
    env e;
    env_open(&e);

    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = call(&e, (long long)getuid() + 7, "scanner.poll", "{}", &raw);
    T_CHECK_MSG(!atlas_ipc_response_ok(r), "an unrelated uid was served: %s", atlas_buf_cstr(&raw));

    const char *body = atlas_buf_cstr(&raw);
    T_CHECK_MSG(strstr(body, "mine") == NULL, "the refusal named a repository: %s", body);
    T_CHECK_MSG(strstr(body, "theirs") == NULL, "the refusal named a repository: %s", body);
    T_CHECK_MSG(strstr(body, "nobodys") == NULL, "the refusal named a repository: %s", body);

    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    env_close(&e);
}

/* 0 is how Atlas records "no scanner assigned". A peer arriving as uid 0 must
 * not be handed the repositories that carry it, or an absence would grant
 * exactly what it means to withhold. */
static void test_uid_zero_is_never_a_scanner(void) {
    env e;
    env_open(&e);

    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = call(&e, 0, "scanner.poll", "{}", &raw);
    T_CHECK_MSG(!atlas_ipc_response_ok(r), "uid 0 was served: %s", atlas_buf_cstr(&raw));

    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    env_close(&e);
}

/* The poll answer carries the cadence Atlas will hold a scanner to.
 *
 * **The cadence is Atlas', not the scanner's.** `scanner.poll` doubles as the
 * heartbeat — the spec's design, and why there is no `hello` — so the number in
 * the answer and the number the freshness rule judges by have to be one number.
 * A scanner that declared its own would leave a promise standing after it died;
 * one that stops polling simply stops being heard.
 *
 * Asserted at the boundary a scanner reaches, because that is where a drift
 * between the two would show. */
static void test_the_poll_answer_carries_the_cadence(void) {
    env e;
    env_open(&e);

    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = call(&e, (long long)getuid(), "scanner.poll", "{}", &raw);
    T_CHECK_MSG(atlas_ipc_response_ok(r), "scanner.poll failed: %s", atlas_buf_cstr(&raw));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&raw), "\"poll_within_ms\"") != NULL,
                "the poll answer does not say when to ask again: %s", atlas_buf_cstr(&raw));
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);

    env_close(&e);
}

/* True when `<data-dir>/mirror/<repo>/<rel>` exists. */
/* A13. Reports the run finished, which is what publishes the staged generation.
 *
 * A pass writes into `<id>.next` so a refresh cannot make a finished mirror
 * unreadable; nothing a scanner puts is visible to a reader until it says the
 * run is complete. Tests that check the mirror have to complete the cycle,
 * because that is the cycle a scanner performs. */
static void finish(env *e, int64_t repo) {
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    (void)atlas_buf_appendf(&params, &err, "{\"repo\":%lld,\"complete\":true}", (long long)repo);
    atlas_ipc_response *r =
        call(e, (long long)getuid(), "scanner.state", atlas_buf_cstr(&params), &raw);
    atlas_ipc_response_free(r);
    atlas_buf_free(&params);
    atlas_buf_free(&raw);
}

static bool mirrored(env *e, int64_t repo, const char *rel) {
    atlas_buf p = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    (void)atlas_buf_appendf(&p, &err, "%s/mirror/%lld/%s", fx_data_dir(&e->fx), (long long)repo,
                            rel);
    bool there = access(atlas_buf_cstr(&p), F_OK) == 0;
    atlas_buf_free(&p);
    return there;
}

static void test_a_scanner_writes_into_its_own_repositorys_mirror(void) {
    env e;
    env_open(&e);

    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    /* "int x;" as hex — the wire carries bytes, not text. */
    T_OK(atlas_buf_appendf(&params, &err,
                           "{\"repo\":%lld,\"path\":\"src/a.c\",\"first\":true,"
                           "\"data\":\"696e7420783b\"}",
                           (long long)e.mine),
         &err);
    atlas_ipc_response *r =
        call(&e, (long long)getuid(), "scanner.put", atlas_buf_cstr(&params), &raw);
    T_CHECK_MSG(atlas_ipc_response_ok(r), "scanner.put failed: %s", atlas_buf_cstr(&raw));
    finish(&e, e.mine);
    T_CHECK_MSG(mirrored(&e, e.mine, "src/a.c"), "the bytes did not reach the mirror");

    atlas_ipc_response_free(r);
    atlas_buf_free(&params);
    atlas_buf_free(&raw);
    env_close(&e);
}

/* The load-bearing refusal. `require_scanner` only asks whether this peer scans
 * *something*; without the per-repository check a scanner could write into a
 * mirror belonging to a repository it does not own. The absence on disk is
 * asserted as well as the refusal: a refusal that had already written would be
 * the whole design failing quietly. */
static void test_a_scanner_may_not_write_into_another_repositorys_mirror(void) {
    env e;
    env_open(&e);

    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_buf_appendf(&params, &err,
                           "{\"repo\":%lld,\"path\":\"stolen.c\",\"first\":true,\"data\":\"78\"}",
                           (long long)e.theirs),
         &err);
    atlas_ipc_response *r =
        call(&e, (long long)getuid(), "scanner.put", atlas_buf_cstr(&params), &raw);
    T_CHECK_MSG(!atlas_ipc_response_ok(r), "a scanner wrote into another repository's mirror: %s",
                atlas_buf_cstr(&raw));
    T_CHECK_MSG(!mirrored(&e, e.theirs, "stolen.c"), "the refused write still created a file");

    atlas_ipc_response_free(r);
    atlas_buf_free(&params);
    atlas_buf_free(&raw);
    env_close(&e);
}

/* A source file is arbitrary bytes. A quote, a backslash, a newline, a C0
 * control and a sequence that is not valid UTF-8 are all legal in one, and all
 * of them are what a JSON string will not carry unchanged. If the wire cannot
 * carry them the mirror is a copy of something else, so this is asserted before
 * anything is written on top of the format. */
static void test_the_wire_carries_arbitrary_bytes(void) {
    env e;
    env_open(&e);

    static const unsigned char HOSTILE[] = {'a', '"', 'b', '\\', 'c', '\n', 'd',
                                            '\t', 'e', 0x01, 0xff, 0xfe};
    atlas_err err;
    atlas_err_init(&err);

    /* Hex on the wire: two lowercase digits per byte. */
    atlas_buf hex = ATLAS_BUF_INIT;
    for (size_t i = 0; i < sizeof HOSTILE; i++) {
        T_OK(atlas_buf_appendf(&hex, &err, "%02x", HOSTILE[i]), &err);
    }

    atlas_buf params = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&params, &err,
                           "{\"repo\":%lld,\"path\":\"hostile.bin\",\"first\":true,\"data\":"
                           "\"%s\"}",
                           (long long)e.mine, atlas_buf_cstr(&hex)),
         &err);

    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r =
        call(&e, (long long)getuid(), "scanner.put", atlas_buf_cstr(&params), &raw);
    T_CHECK_MSG(atlas_ipc_response_ok(r), "scanner.put refused hostile bytes: %s",
                atlas_buf_cstr(&raw));

    /* Read it back from the filesystem and compare byte for byte. The run has to
     * say it finished first: a pass writes into a staging generation and
     * nothing is visible to a reader until it is published. */
    finish(&e, e.mine);
    atlas_buf path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&path, &err, "%s/mirror/%lld/hostile.bin", fx_data_dir(&e.fx),
                           (long long)e.mine),
         &err);
    FILE *f = fopen(atlas_buf_cstr(&path), "rb");
    T_REQUIRE(f != NULL);
    unsigned char got[64];
    size_t n = fread(got, 1u, sizeof got, f);
    (void)fclose(f);
    T_CHECK_MSG(n == sizeof HOSTILE, "mirrored %zu bytes, sent %zu", n, sizeof HOSTILE);
    T_CHECK_MSG(n == sizeof HOSTILE && memcmp(got, HOSTILE, n) == 0, "the bytes changed in transit");

    atlas_ipc_response_free(r);
    atlas_buf_free(&path);
    atlas_buf_free(&params);
    atlas_buf_free(&hex);
    atlas_buf_free(&raw);
    env_close(&e);
}

/* A13. `"exec":true` on the wire produces an executable file in the mirror.
 *
 * Git tracks exactly one mode bit, and the mirror carries the mirrored index
 * beside the files -- so a script written 0600 compares `100644` against an
 * index holding `100755`, and git calls that a modification. Measured on the
 * live tree: a clean repository read as dirty with 24 files changed, every one
 * a script, not one differing by a byte.
 *
 * Asserted at the boundary because the mode has to survive the whole hop: the
 * scanner reads it from its own `stat`, the wire carries it, and the mirror
 * applies it. A unit test of the writer alone would have passed while the
 * repository still read dirty. */
static void test_the_exec_bit_crosses_the_wire(void) {
    env e;
    env_open(&e);

    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_buf_appendf(&params, &err,
                           "{\"repo\":%lld,\"path\":\"run.sh\",\"first\":true,\"exec\":true,"
                           "\"data\":\"23212f62696e2f7368\"}",
                           (long long)e.mine),
         &err);
    atlas_ipc_response *r =
        call(&e, (long long)getuid(), "scanner.put", atlas_buf_cstr(&params), &raw);
    T_CHECK_MSG(atlas_ipc_response_ok(r), "scanner.put failed: %s", atlas_buf_cstr(&raw));

    finish(&e, e.mine);
    atlas_buf p = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&p, &err, "%s/mirror/%lld/run.sh", fx_data_dir(&e.fx),
                           (long long)e.mine),
         &err);
    struct stat sb;
    memset(&sb, 0, sizeof(sb));
    T_REQUIRE(lstat(atlas_buf_cstr(&p), &sb) == 0);
    T_CHECK_MSG((sb.st_mode & S_IXUSR) != 0,
                "the mirror wrote mode %o; git will read this as a modification",
                (unsigned)(sb.st_mode & 07777));

    atlas_buf_free(&p);
    atlas_ipc_response_free(r);
    atlas_buf_free(&params);
    atlas_buf_free(&raw);
    env_close(&e);
}


/* --- A13: a mirror state is answered when it is accepted -------------------- */

/* Polls until the directive for this uid's repositories carries `want`, which is
 * how a scanner learns what the daemon recorded: the poll is the confirmation
 * channel, so nothing goes silent when the response to `scanner.state` stops
 * carrying the outcome. */
static bool wait_for_directive(env *e, const char *want) {
    for (int i = 0; i < 250; i++) {
        atlas_buf raw = ATLAS_BUF_INIT;
        atlas_ipc_response *r = call(e, (long long)getuid(), "scanner.poll", "{}", &raw);
        const bool got = atlas_ipc_response_ok(r) && strstr(atlas_buf_cstr(&raw), want) != NULL;
        atlas_ipc_response_free(r);
        atlas_buf_free(&raw);
        if (got) {
            return true;
        }
        usleep(20000);
    }
    return false;
}

static void test_a_mirror_state_is_answered_when_it_is_accepted(void) {
    /* A8-CI's rule, one layer down: an operation that can outlast a client's
     * patience does not run in the serve loop, and the client is answered when
     * the work is *accepted*.
     *
     * Measured on a live daemon: a scanner's `scanner.state` arrived while the
     * writer was 19,864 ms into a full reconciliation of the same repository —
     * every one of 21,996 files re-hashed, because publishing a mirror
     * generation replaces every inode and no stored filesystem identity
     * survives it — and the call timed out reading its response frame. The
     * write had been queued and did land, so the scanner's warning that the
     * daemon "did not record this run" was false. A9.2.6 already says why that
     * is the wrong claim: backing out and timing out are different, and only
     * one of them means nothing was queued.
     *
     * Raising the scanner's deadline instead would have needed a bound on how
     * long a reconciliation may take, and there is no such bound in Atlas to
     * derive one from. */
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    FILE *log = fopen("/dev/null", "we");
    T_REQUIRE_MSG(log != NULL, "cannot open a log sink");
    atlas_writer *w = NULL;
    T_OK(atlas_writer_start(atlas_buf_cstr(&e.db_path), fx_data_dir(&e.fx), "", NULL, log, &w,
                            &err),
         &err);
    e.ctx.writer = w;

    /* One file, so the run has something to publish. */
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_buf params = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&params, &err,
                           "{\"repo\":%lld,\"path\":\"src/a.c\",\"first\":true,"
                           "\"data\":\"696e7420783b\"}",
                           (long long)e.mine),
         &err);
    atlas_ipc_response *pr =
        call(&e, (long long)getuid(), "scanner.put", atlas_buf_cstr(&params), &raw);
    T_CHECK_MSG(atlas_ipc_response_ok(pr), "scanner.put failed: %s", atlas_buf_cstr(&raw));
    atlas_ipc_response_free(pr);
    atlas_buf_reset(&params);
    atlas_buf_reset(&raw);

    /* Hold the writer inside a reconciliation, which is exactly what it was
     * doing when this failed in production. */
    atlas_writer_test_stall(w, ATLAS_JOB_RECONCILE);
    (void)atlas_writer_submit_reconcile(w, e.mine, false, false, NULL, 0, NULL, &err);
    bool stalled = false;
    for (int i = 0; i < 250 && !stalled; i++) {
        stalled = atlas_writer_test_stalled(w);
        if (!stalled) {
            usleep(20000);
        }
    }
    T_REQUIRE_MSG(stalled, "the writer never entered the stall");

    struct timespec t0;
    struct timespec t1;
    (void)clock_gettime(CLOCK_MONOTONIC, &t0);
    T_OK(atlas_buf_appendf(&params, &err, "{\"repo\":%lld,\"complete\":true}", (long long)e.mine),
         &err);
    atlas_ipc_response *r =
        call(&e, (long long)getuid(), "scanner.state", atlas_buf_cstr(&params), &raw);
    (void)clock_gettime(CLOCK_MONOTONIC, &t1);
    const long long took_ms = (long long)((t1.tv_sec - t0.tv_sec) * 1000) +
                              (long long)((t1.tv_nsec - t0.tv_nsec) / 1000000);

    T_CHECK_MSG(atlas_ipc_response_ok(r),
                "a mirror state was refused while the writer was busy: %s", atlas_buf_cstr(&raw));
    T_CHECK_MSG(took_ms < 1000, "a mirror state waited %lld ms for a busy writer", took_ms);
    atlas_ipc_response_free(r);
    atlas_buf_free(&params);
    atlas_buf_free(&raw);

    /* Accepted is not lost. Once the writer is free the record lands, and the
     * poll stops asking for a full mirror — which is what makes answering early
     * safe rather than merely quick. */
    atlas_writer_test_release(w);
    T_CHECK_MSG(wait_for_directive(&e, "\"incremental\""),
                "an accepted mirror state never reached the row");

    atlas_writer_stop(w);
    (void)fclose(log);
    env_close(&e);
}
static const atlas_test TESTS[] = {
    {"a scanner is told its own repositories and no others",
     test_a_scanner_is_told_its_own_repositories_and_no_others},
    {"the wire carries arbitrary bytes", test_the_wire_carries_arbitrary_bytes},
    {"a scanner writes into its own repository's mirror",
     test_a_scanner_writes_into_its_own_repositorys_mirror},
    {"a scanner may not write into another repository's mirror",
     test_a_scanner_may_not_write_into_another_repositorys_mirror},
    {"the poll answer carries the cadence", test_the_poll_answer_carries_the_cadence},
    {"the exec bit crosses the wire", test_the_exec_bit_crosses_the_wire},
    {"a uid that owns nothing is refused and names no repository",
     test_a_uid_that_owns_nothing_is_refused_and_names_no_repository},
    {"uid zero is never a scanner", test_uid_zero_is_never_a_scanner},
    {"a mirror state is answered when it is accepted",
     test_a_mirror_state_is_answered_when_it_is_accepted},
};

ATLAS_TEST_MAIN("scanner_rpc", TESTS)
