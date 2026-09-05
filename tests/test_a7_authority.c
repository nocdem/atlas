/* Atlas - A7: where authority comes from, and where it provably does not.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A4 said, correctly, that a same-UID process able to drive a pseudo-terminal
 * may imitate the operator channel. A7 asks the next question, which A4 never
 * did: does a caller need a terminal at all?
 *
 * It did not. `decision.challenge` was an ordinary RPC method, so any process
 * that could open the socket could mint the capability that `decision.approve`
 * spends, and the resulting record said `LOCAL_OPERATOR_CONFIRMED` about a
 * channel nothing had been through. The terminal check lived in
 * `atlas_service_decision_confirm`, which is the CLI's own helper — a check a
 * client performs on itself is not a boundary.
 *
 * The tests below are the evidence for A7's two answers:
 *
 *   1. Structural. Capability minting and spending, and every mutation of the
 *      repository registry, are not RPC methods at all. The daemon answers to
 *      none of them, which is the guarantee A5 already makes about backups and
 *      for the same reason: an absent method cannot be reached by a model that
 *      only speaks the socket.
 *
 *   2. Fail-closed. What remains is the local CLI path, and on a machine where
 *      one uid owns the daemon, the database, the binary and the shell, that
 *      path is protected by nothing. Atlas says so, in a probe that can only
 *      answer LOCKED here, and refuses the operation rather than performing it
 *      behind a confirmation prompt that proves nothing.
 *
 * What none of this establishes — and the suite asserts the honest wording
 * rather than leaving it to prose — is that the *database* is protected. A
 * process running as the owning uid can write `atlas.db` with SQLite and needs
 * no Atlas code path at all. The lock protects the Atlas-mediated route; only a
 * separate OS principal protects the record.
 */
#define _GNU_SOURCE 1

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/authority.h"
#include "atlas/decision.h"
#include "atlas/ipc.h"
#include "atlas_test.h"
#include "support/fixture.h"

/* --- the fixture ---------------------------------------------------------- */

typedef struct env {
    fixture fx;
    atlas_buf uid; /* one proposed decision to aim the capability at */
} env;

static void run_atlas(env *e, const char *const *extra, size_t n, atlas_buf *out, int *code) {
    atlas_err err;
    atlas_err_init(&err);
    const char *argv[24];
    size_t k = 0;
    argv[k++] = "--data-dir";
    argv[k++] = fx_data_dir(&e->fx);
    T_REQUIRE(n + k <= sizeof(argv) / sizeof(argv[0]));
    for (size_t i = 0; i < n; i++) {
        argv[k++] = extra[i];
    }
    atlas_buf errout = ATLAS_BUF_INIT;
    T_OK(fx_atlas(argv, k, out, &errout, code, &err), &err);
    /* A refusal explains itself on stderr, so a test that only kept stdout
     * would assert against an empty string and say nothing useful when it
     * failed. */
    if (errout.len > 0) {
        atlas_err ignore;
        atlas_err_init(&ignore);
        (void)atlas_buf_append(out, errout.data, errout.len, &ignore);
    }
    atlas_buf_free(&errout);
}

/* A repository, registered and scanned, with one proposed decision.
 *
 * Everything here happens before any daemon exists, so it exercises the local
 * path the operator has and not the socket the model has. */
static void env_open(env *e) {
    atlas_err err;
    atlas_err_init(&err);
    memset(e, 0, sizeof(*e));
    atlas_buf_init(&e->uid);
    T_OK(fx_open(&e->fx, &err), &err);
    T_OK(fx_init_repo(&e->fx, fx_repo(&e->fx), NULL, &err), &err);
    T_OK(fx_write(fx_repo(&e->fx), "main.c", "int main(void){return 0;}\n", &err), &err);
    T_OK(fx_add_all(&e->fx, fx_repo(&e->fx), &err), &err);
    T_OK(fx_commit(&e->fx, fx_repo(&e->fx), "init", &err), &err);

    int code = 0;
    atlas_buf out = ATLAS_BUF_INIT;
    const char *add[] = {"repo", "add", fx_repo(&e->fx), "--name", "proj"};
    run_atlas(e, add, 5u, &out, &code);
    T_EQ_INT(code, 0);
    atlas_buf_reset(&out);

    const char *scan[] = {"scan", "proj"};
    run_atlas(e, scan, 2u, &out, &code);
    T_EQ_INT(code, 0);
    atlas_buf_reset(&out);

    const char *propose[] = {
        "decision",   "propose",                          "proj",   "--title", "Use WAL journalling",
        "--decision", "Enable WAL on the index database.", "--path", "main.c",
    };
    run_atlas(e, propose, 9u, &out, &code);
    T_EQ_INT(code, 0);

    const char *p = strstr(atlas_buf_cstr(&out), ATLAS_DECISION_UID_PREFIX);
    T_REQUIRE_MSG(p != NULL, "propose did not print a decision id: %s", atlas_buf_cstr(&out));
    size_t len = strlen(ATLAS_DECISION_UID_PREFIX) + ATLAS_DECISION_UID_HEX;
    T_OK(atlas_buf_set(&e->uid, p, len, &err), &err);
    T_REQUIRE(atlas_decision_uid_is_valid(atlas_buf_cstr(&e->uid)));
    atlas_buf_free(&out);
}

static void env_close(env *e) {
    atlas_buf_free(&e->uid);
    fx_close(&e->fx);
}

/* --- 1. the capability cannot be minted over the socket -------------------- */

static void test_a_socket_peer_cannot_mint_an_approval_capability(void) {
    /* The whole of A4's operator channel rests on the capability being
     * obtainable only through a terminal. This asks a live daemon for one,
     * over the socket, from a process with no terminal at all.
     *
     * The request is well formed and names a real repository and a real
     * decision, so a refusal here is a refusal of the *route*, not of the
     * arguments. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e);

    fx_daemon d;
    fx_daemon_init(&d);
    T_REQUIRE(fx_daemon_start(&e.fx, &d, &err) == ATLAS_OK);
    T_REQUIRE(fx_daemon_wait_ready(&d, 15000, &err) == ATLAS_OK);

    char params[256];
    (void)snprintf(params, sizeof(params), "{\"repo\":\"proj\",\"decision\":\"%s\"}",
                   atlas_buf_cstr(&e.uid));

    atlas_buf resp = ATLAS_BUF_INIT;
    atlas_err e2;
    atlas_err_init(&e2);
    (void)atlas_ipc_call(atlas_buf_cstr(&d.socket), "decision.challenge", params, &resp, &e2);

    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "\"ok\":true") == NULL,
                "the daemon minted an approval capability for a caller with no terminal: %s",
                atlas_buf_cstr(&resp));
    /* And it must not have leaked one in the failure path either. */
    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "\"token\"") == NULL,
                "the refusal carried a token: %s", atlas_buf_cstr(&resp));

    atlas_buf_free(&resp);
    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    env_close(&e);
}

/* --- 2. the daemon answers to no authority method ------------------------- */

static void test_the_daemon_answers_to_no_authority_method(void) {
    /* Every name the operator channel and the registry would have, asked of a
     * live daemon. This is the same negative enumeration A5 does for backups
     * and A6 for gate mutations, extended to the group A7 removed.
     *
     * Asked of the running process rather than of the source: a method table is
     * a claim about a file, and this is a claim about a daemon. */
    /* **A4's operator channel is no longer in this list, and that is a
     * deliberate reversal recorded here rather than a gap.**
     *
     * A7 deleted `decision.challenge/approve/reject/supersede/revalidate`
     * because the challenge minted a capability for anyone who could open the
     * socket. They are back, in a group offered only to the peer whose
     * `SO_PEERCRED` uid equals the `operator_uid` in the root-owned policy — so
     * the property this list asserted is still asserted, just about the right
     * set of peers. `test_operator_peer.c` checks the grant itself, and the
     * loop below checks these five names from a peer that is *not* the
     * operator. Every other name here is unchanged: nothing else came back. */
    static const char *const OPERATOR_METHODS[] = {
        "decision.challenge", "decision.approve", "decision.reject", "decision.supersede",
        "decision.revalidate",
        /* A9.1's one new lifecycle verb. Added here rather than left out, because
         * this list is the only place that asserts an operator method is not
         * offered to an ordinary peer, and a verb added to the group without a
         * row here would be unchecked. */
        "decision.resolve",
        /* A16 T5. A different group, gated by a different root-owned policy
         * and a different peer test -- `atlas_server_remote_disposal_offered`
         * checks the *gateway* policy's uid, not the authority policy's --
         * but this fixture daemon has neither policy installed, so both are
         * offered to nobody and belong in this same negative enumeration.
         * `test_gw_dispose.c` proves the positive case: a peer the gateway
         * policy *does* name still needs a disposal credential and either
         * TLS or the operator's written acceptance before either name stops
         * being "unknown method". */
        "decision.remote_challenge", "decision.remote_dispose",
    };
    static const char *const METHODS[] = {
        /* Plausible aliases and case variants of the operator channel. A
         * dispatcher
         * that matched loosely would answer one of these. */
        "decision.Approve", "DECISION.APPROVE", "decision.confirm", "decision.authorize",
        "decision.authorise", "decision.sign", "decision.validate", "decision.token",
        "decision.capability", "decision.grant", "decision.unlock", "decision.force",
        /* A9.1: plausible aliases and case variants of the resolve verb, and the
         * names a "close this out without a capability" shortcut would take. */
        "decision.Resolve", "DECISION.RESOLVE", "decision_resolve", "decision.close",
        "decision.discharge", "decision.complete", "decision.done", "decision.satisfy",
        /* And the classification must not be settable over the wire: a kind is
         * written by the INSERT that creates a document and by nothing else. */
        "decision.set_kind", "decision.reclassify", "decision.kind",
        /* The registry, removed in A7. */
        "repo.add", "repo.ensure", "repo.remove", "repo.register", "repo.create", "repo.delete",
        "repo.forget", "repo.trust",
        /* A5's, asserted again: the cheapest way to break its guarantee would
         * have been for A7 to add the method A5 does not have. */
        "backup.create", "backup.verify", "backup.restore", "maintenance.plan",
        "maintenance.prune",
        /* And the authority profile itself must not be settable over the wire. */
        "authority.unlock", "authority.set", "authority.grant", "authority.override",
        /* A14. A daemon with a zeroed gateway policy (no submit keys, no TLS)
         * must answer "unknown method" for the remote-submit group regardless of
         * the caller's uid. */
        "job.remote_submit", "job.remote_get", "job.remote_list", "job.remote_cancel",
    };

    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e);

    fx_daemon d;
    fx_daemon_init(&d);
    T_REQUIRE(fx_daemon_start(&e.fx, &d, &err) == ATLAS_OK);
    T_REQUIRE(fx_daemon_wait_ready(&d, 15000, &err) == ATLAS_OK);

    for (size_t i = 0; i < sizeof METHODS / sizeof METHODS[0]; i++) {
        atlas_buf resp = ATLAS_BUF_INIT;
        atlas_err e2;
        atlas_err_init(&e2);
        (void)atlas_ipc_call(atlas_buf_cstr(&d.socket), METHODS[i], "{}", &resp, &e2);
        const char *body = atlas_buf_cstr(&resp);
        T_CHECK_MSG(strstr(body, "\"ok\":true") == NULL, "the daemon accepted \"%s\": %s",
                    METHODS[i], body);
        /* Absent, not merely unhappy. Asserting only that the call failed would
         * pass against a method that exists and rejected empty parameters,
         * which is precisely the state this test has to be able to fail on. */
        T_CHECK_MSG(strstr(body, "unknown method") != NULL,
                    "\"%s\" exists and refused rather than being absent: %s", METHODS[i], body);
        atlas_buf_free(&resp);
    }

    /* The five that came back — and which this daemon must still refuse to
     * offer, whoever is asking.
     *
     * A fixture daemon runs from the build tree, so its own executable is
     * writable by the uid it constrains. That is the last of the four
     * conditions in `atlas/authority.h`, and it fails here on purpose: a check
     * running from a binary the constrained uid can replace reports whatever
     * that uid last compiled. So the operator channel is locked for *every*
     * peer of *this* daemon, including one the policy names, and the assertion
     * is unconditional.
     *
     * The consequence is worth stating rather than leaving implicit: the
     * positive half — the operator peer reaching these methods — is a property
     * only a deployed, root-owned daemon can have, so it is verified against
     * the deployment and not here. `test_operator_peer.c` pins the decision
     * procedure itself, which is the part that can be tested in isolation. */
    for (size_t i = 0; i < sizeof OPERATOR_METHODS / sizeof OPERATOR_METHODS[0]; i++) {
        atlas_buf resp = ATLAS_BUF_INIT;
        atlas_err e2;
        atlas_err_init(&e2);
        (void)atlas_ipc_call(atlas_buf_cstr(&d.socket), OPERATOR_METHODS[i], "{}", &resp, &e2);
        const char *body = atlas_buf_cstr(&resp);
        T_CHECK_MSG(strstr(body, "\"ok\":true") == NULL, "the daemon accepted \"%s\": %s",
                    OPERATOR_METHODS[i], body);
        T_CHECK_MSG(strstr(body, "unknown method") != NULL,
                    "\"%s\" was offered by a daemon running from a replaceable binary: %s",
                    OPERATOR_METHODS[i], body);
        atlas_buf_free(&resp);
    }

    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    env_close(&e);
}

/* --- 3. a session does not register the repository it was opened in -------- */

static void test_an_unknown_repository_is_a_candidate_and_not_a_registration(void) {
    /* The session-start hook used to call `repo.ensure`, which registered
     * whatever absolute path it was handed. Opening a session in a directory is
     * a model-triggered event, so that made "a model looked at it" sufficient
     * to create a trusted Atlas registration.
     *
     * `repo.resolve` reports. It must report this second repository as
     * unregistered and must leave the registry with exactly the one entry that
     * an operator created. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e);

    /* A second, real git repository that no operator ever registered. */
    atlas_buf other = ATLAS_BUF_INIT;
    T_OK(atlas_buf_append_str(&other, fx_data_dir(&e.fx), &err), &err);
    T_OK(atlas_buf_append_str(&other, "/unregistered", &err), &err);
    T_OK(fx_mkdir(fx_data_dir(&e.fx), "unregistered", &err), &err);
    T_OK(fx_init_repo(&e.fx, atlas_buf_cstr(&other), NULL, &err), &err);
    T_OK(fx_write(atlas_buf_cstr(&other), "b.c", "int b;\n", &err), &err);
    T_OK(fx_add_all(&e.fx, atlas_buf_cstr(&other), &err), &err);
    T_OK(fx_commit(&e.fx, atlas_buf_cstr(&other), "init", &err), &err);

    fx_daemon d;
    fx_daemon_init(&d);
    T_REQUIRE(fx_daemon_start(&e.fx, &d, &err) == ATLAS_OK);
    T_REQUIRE(fx_daemon_wait_ready(&d, 15000, &err) == ATLAS_OK);

    char params[2048];
    (void)snprintf(params, sizeof(params), "{\"path\":\"%s\"}", atlas_buf_cstr(&other));

    /* Whatever a session-start path asks for, it must not end in a row. */
    atlas_buf resp = ATLAS_BUF_INIT;
    atlas_err e2;
    atlas_err_init(&e2);
    (void)atlas_ipc_call(atlas_buf_cstr(&d.socket), "repo.ensure", params, &resp, &e2);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "\"created\":true") == NULL,
                "repo.ensure registered an unknown repository: %s", atlas_buf_cstr(&resp));
    atlas_buf_free(&resp);

    /* And the read that replaces it says, honestly, that it is not registered. */
    atlas_err_init(&e2);
    T_OK(atlas_ipc_call(atlas_buf_cstr(&d.socket), "repo.resolve", params, &resp, &e2), &e2);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "\"registered\":false") != NULL,
                "repo.resolve must report an unknown repository as unregistered: %s",
                atlas_buf_cstr(&resp));
    atlas_buf_free(&resp);

    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);

    /* The registry, read afterwards, still holds exactly what the operator put
     * in it. Counted from the CLI rather than inferred from the replies. */
    atlas_buf out = ATLAS_BUF_INIT;
    int code = 0;
    const char *list[] = {"repo", "list", "--json"};
    run_atlas(&e, list, 3u, &out, &code);
    T_EQ_INT(code, 0);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"count\":1") != NULL,
                "the registry gained an entry no operator created: %s", atlas_buf_cstr(&out));
    atlas_buf_free(&out);
    atlas_buf_free(&other);
    env_close(&e);
}

/* --- 4. a pseudo-terminal does not open a locked profile ------------------ */

/* A real pseudo-terminal, allocated and typed at by this process.
 *
 * This is the same machinery `tests/test_decision_operator.c` has used since
 * A4, and it is here for the same reason it is there: the claim under test is
 * about what a terminal proves, and a stubbed terminal would be testing the
 * stub. If a test can drive a pty, so can anything else running as this uid. */
typedef struct pty {
    int master;
    pid_t child;
} pty;

static atlas_status pty_spawn(env *e, const char *const *args, size_t nargs, pty *out,
                              atlas_err *err) {
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "posix_openpt: %s", strerror(errno));
    }
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        (void)close(master);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "grantpt/unlockpt: %s", strerror(errno));
    }
    char slave_name[128];
    if (ptsname_r(master, slave_name, sizeof(slave_name)) != 0) {
        (void)close(master);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "ptsname_r: %s", strerror(errno));
    }

    const char *argv[24];
    size_t k = 0;
    argv[k++] = ATLAS_BIN;
    argv[k++] = "--data-dir";
    argv[k++] = fx_data_dir(&e->fx);
    for (size_t i = 0; i < nargs; i++) {
        argv[k++] = args[i];
    }
    argv[k] = NULL;
    char home[1024];
    (void)snprintf(home, sizeof(home), "HOME=%s", fx_data_dir(&e->fx));
    const char *envp[] = {"PATH=/usr/bin:/bin", home, "LC_ALL=C", NULL};

    pid_t pid = fork();
    if (pid < 0) {
        (void)close(master);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "fork: %s", strerror(errno));
    }
    if (pid == 0) {
        (void)close(master);
        if (setsid() < 0) {
            _exit(120);
        }
        int slave = open(slave_name, O_RDWR);
        if (slave < 0) {
            _exit(121);
        }
#ifdef TIOCSCTTY
        if (ioctl(slave, TIOCSCTTY, 0) < 0) {
            _exit(122);
        }
#endif
        if (dup2(slave, STDIN_FILENO) < 0 || dup2(slave, STDOUT_FILENO) < 0 ||
            dup2(slave, STDERR_FILENO) < 0) {
            _exit(123);
        }
        if (slave > STDERR_FILENO) {
            (void)close(slave);
        }
        execve(ATLAS_BIN, (char *const *)(uintptr_t)argv, (char *const *)(uintptr_t)envp);
        _exit(124);
    }
    out->master = master;
    out->child = pid;
    return ATLAS_OK;
}

/* Drains the terminal until the child exits, returning its status and the whole
 * transcript. Nothing is typed: the point is that it never gets far enough to
 * ask. A prompt appearing at all would be a failure, and is asserted on. */
static int pty_drain(pty *p, atlas_buf *transcript) {
    atlas_err err;
    atlas_err_init(&err);
    for (int i = 0; i < 400; i++) {
        struct pollfd pfd = {p->master, POLLIN, 0};
        int rc = poll(&pfd, 1u, 50);
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        if (rc <= 0) {
            int status = 0;
            if (waitpid(p->child, &status, WNOHANG) == p->child) {
                (void)close(p->master);
                return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            }
            continue;
        }
        char buf[1024];
        ssize_t n = read(p->master, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        (void)atlas_buf_append(transcript, buf, (size_t)n, &err);
    }
    int status = 0;
    (void)waitpid(p->child, &status, 0);
    (void)close(p->master);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static void test_a_pseudo_terminal_does_not_open_a_locked_profile(void) {
    /* The adversary here is the one A7 exists for: a model with shell access
     * and the ability to allocate a pty, running as the same uid as the person
     * it is imitating. It gets a real terminal on both descriptors, a
     * controlling terminal, and a controlling session — everything `isatty`,
     * `/dev/tty` and a `TIOCSCTTY` ioctl can establish.
     *
     * It must still be refused, and refused *before* a confirmation prompt is
     * printed. A prompt would mean the design had fallen back to asking a
     * question whose answer this process can supply. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e);

    const char *args[] = {"decision", "approve", "proj", atlas_buf_cstr(&e.uid)};
    pty p = {-1, -1};
    T_REQUIRE(pty_spawn(&e, args, 4u, &p, &err) == ATLAS_OK);
    atlas_buf transcript = ATLAS_BUF_INIT;
    int code = pty_drain(&p, &transcript);
    const char *text = atlas_buf_cstr(&transcript);

    T_CHECK_MSG(code != 0, "an approval from a pseudo-terminal succeeded (exit %d):\n%s", code,
                text);
    T_CHECK_MSG(strstr(text, "locked in this Atlas profile") != NULL,
                "the refusal did not name the locked profile:\n%s", text);
    /* Refused before anything was asked. The confirmation prompt quotes the
     * digest for the operator to retype; if it appeared, Atlas got as far as
     * minting a capability. */
    T_CHECK_MSG(strstr(text, "type the confirmation") == NULL &&
                    strstr(text, "digest     :") == NULL,
                "Atlas prompted for a confirmation in a locked profile:\n%s", text);

    /* And the decision is still exactly where it was. */
    atlas_buf out = ATLAS_BUF_INIT;
    int lcode = 0;
    const char *show[] = {"decision", "show", "proj", atlas_buf_cstr(&e.uid)};
    run_atlas(&e, show, 4u, &out, &lcode);
    T_EQ_INT(lcode, 0);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "status:       PROPOSED") != NULL,
                "the decision did not stay PROPOSED:\n%s", atlas_buf_cstr(&out));

    atlas_buf_free(&out);
    atlas_buf_free(&transcript);
    env_close(&e);
}

/* --- 5. the locked profile covers every operation that asserts authority -- */

static void test_every_lifecycle_verb_is_refused_in_a_locked_profile(void) {
    /* One list, driven through the real CLI. A guard that covered approval and
     * forgot backups would be a guard that moved the problem rather than
     * solving it. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e);

    atlas_buf backup_path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&backup_path, &err, "%s/copy.db", fx_data_dir(&e.fx)), &err);

    /* Every verb that spends or mints a lifecycle capability, through the real
     * CLI, with no terminal. A guard that covered `approve` and forgot
     * `revalidate` would move the problem rather than solve it. */
    struct {
        const char *what;
        const char *args[8];
        size_t n;
    } CASES[] = {
        {"approve", {"decision", "approve", "proj", atlas_buf_cstr(&e.uid)}, 4u},
        {"reject", {"decision", "reject", "proj", atlas_buf_cstr(&e.uid)}, 4u},
        {"revalidate", {"decision", "revalidate", "proj", atlas_buf_cstr(&e.uid)}, 4u},
        {"supersede", {"decision", "supersede", "proj", atlas_buf_cstr(&e.uid), "--by",
                       atlas_buf_cstr(&e.uid)},
         6u},
    };

    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        atlas_buf out = ATLAS_BUF_INIT;
        int code = 0;
        run_atlas(&e, CASES[i].args, CASES[i].n, &out, &code);
        T_CHECK_MSG(code != 0, "%s succeeded in a locked profile: %s", CASES[i].what,
                    atlas_buf_cstr(&out));
        T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "locked in this Atlas profile") != NULL,
                    "%s was refused for some other reason: %s", CASES[i].what,
                    atlas_buf_cstr(&out));
        atlas_buf_free(&out);
    }

    /* No capability was minted along the way. A locked profile that still
     * created challenge rows would be leaving spendable capabilities behind
     * for anything that can read the database. */
    {
        atlas_buf out = ATLAS_BUF_INIT;
        int code = 0;
        const char *hist[] = {"decision", "history", "proj", atlas_buf_cstr(&e.uid)};
        run_atlas(&e, hist, 4u, &out, &code);
        T_EQ_INT(code, 0);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "APPROVED") == NULL,
                    "a locked profile recorded an approval: %s", atlas_buf_cstr(&out));
        atlas_buf_free(&out);
    }

    /* And the operations a lock would only pretend to protect still work,
     * because they are reachable with `cp` and `sqlite3` by the same uid: a
     * refusal would cost the owner function and cost an adversary nothing.
     * This is asserted rather than left to prose so that a later change that
     * quietly guards them has to argue for it here. */
    {
        atlas_buf out = ATLAS_BUF_INIT;
        int code = -1;
        const char *mk[] = {"backup", "create", atlas_buf_cstr(&backup_path)};
        run_atlas(&e, mk, 3u, &out, &code);
        T_CHECK_MSG(code == 0, "backup create was refused in a locked profile: %s",
                    atlas_buf_cstr(&out));
        struct stat sb;
        T_CHECK_MSG(stat(atlas_buf_cstr(&backup_path), &sb) == 0, "no backup was written");
        atlas_buf_free(&out);
    }

    /* And the reads a locked profile must keep answering still answer, because
     * a lock that stopped Atlas being useful would simply be turned off. */
    const char *reads[][4] = {
        {"repo", "list", NULL, NULL},
        {"decision", "list", "proj", NULL},
        {"gate", "check", "proj", NULL},
        {"maintenance", "plan", NULL, NULL},
    };
    const size_t read_n[] = {2u, 3u, 3u, 2u};
    for (size_t i = 0; i < sizeof read_n / sizeof read_n[0]; i++) {
        atlas_buf out = ATLAS_BUF_INIT;
        int code = -1;
        run_atlas(&e, reads[i], read_n[i], &out, &code);
        T_CHECK_MSG(code == 0, "a read was refused in a locked profile: %s",
                    atlas_buf_cstr(&out));
        atlas_buf_free(&out);
    }

    atlas_buf_free(&backup_path);
    env_close(&e);
}

/* --- 6. the probe itself, against real filesystem shapes ------------------ */

static void probe_at(const fixture *fx, const char *rel, atlas_authority *out) {
    char path[1024];
    (void)snprintf(path, sizeof(path), "%s/%s", fx_data_dir(fx), rel);
    /* The executable argument is the real one, so these cases isolate the
     * policy half. On a developer machine it is user-writable and would lock on
     * its own, which is exactly why the policy checks below all expect a
     * policy-shaped reason rather than simply "locked". */
    atlas_authority_probe_at(path, ATLAS_BIN, out);
}

static void test_no_unprivileged_shape_grants_authority(void) {
    /* Every way an unprivileged uid might try to manufacture a policy. None of
     * them can, because all of them fail the root-ownership check that the uid
     * cannot satisfy for any path on the filesystem. */
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_OK(fx_open(&fx, &err), &err);

    atlas_authority a;

    /* Absent.
     *
     * The reason is not asserted for the fixture cases. A fixture lives under a
     * temporary directory this uid owns, so the walk refuses at the first
     * component it reaches and never gets far enough to distinguish "missing"
     * from "writable" — which is correct, and is itself the property under
     * test: no path an unprivileged uid can write to is ever inspected further
     * than its first non-root component. */
    probe_at(&fx, "no-such-policy.conf", &a);
    T_EQ_INT((int)a.state, (int)ATLAS_AUTHORITY_LOCKED);

    /* Present, well formed, and owned by this uid — the file an attacker would
     * write. Root ownership is what refuses it. */
    T_OK(fx_write(fx_data_dir(&fx), "mine.conf", "operator_uid = 1000\n", &err), &err);
    probe_at(&fx, "mine.conf", &a);
    T_CHECK_MSG(a.state == ATLAS_AUTHORITY_LOCKED,
                "a policy written by the constrained uid granted authority");

    /* Naming this very uid, which is the point: the uid is not the problem,
     * the ability to write the file that names it is. */
    char self[64];
    (void)snprintf(self, sizeof(self), "operator_uid = %lld\n", (long long)getuid());
    T_OK(fx_write(fx_data_dir(&fx), "self.conf", self, &err), &err);
    probe_at(&fx, "self.conf", &a);
    T_CHECK_MSG(a.state == ATLAS_AUTHORITY_LOCKED, "a self-naming policy granted authority");

    /* A symlink pointing at something root-owned. The walk refuses to traverse
     * it rather than resolving it, so pointing at a real root-owned file gains
     * nothing. */
    T_OK(fx_symlink(fx_data_dir(&fx), "/etc/hostname", "link.conf", &err), &err);
    probe_at(&fx, "link.conf", &a);
    T_CHECK_MSG(a.state == ATLAS_AUTHORITY_LOCKED, "a symlinked policy granted authority");

    /* A relative path, an empty path, and one with traversal in it. */
    const char *BAD[] = {"etc/atlas/authority.conf", "", "/etc/../etc/atlas/authority.conf",
                         "/etc//atlas/authority.conf"};
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        atlas_authority b;
        atlas_authority_probe_at(BAD[i], ATLAS_BIN, &b);
        T_CHECK_MSG(b.state == ATLAS_AUTHORITY_LOCKED, "\"%s\" granted authority", BAD[i]);
    }

    /* A directory where the policy should be. */
    T_OK(fx_mkdir(fx_data_dir(&fx), "dir.conf", &err), &err);
    probe_at(&fx, "dir.conf", &a);
    T_CHECK_MSG(a.state == ATLAS_AUTHORITY_LOCKED, "a directory granted authority");

    /* And the real, compiled-in path on this machine, which is the profile the
     * rest of the suite runs in. */
    atlas_authority live;
    atlas_authority_probe(&live);
    T_CHECK_MSG(live.state == ATLAS_AUTHORITY_LOCKED,
                "this machine's profile is not locked (reason %s); the suite's other "
                "assertions would not mean what they say",
                atlas_authority_reason_name(live.reason));

    fx_close(&fx);
}

/* --- 7. the suite cannot reach anything real ------------------------------ */

static void test_the_suite_cannot_reach_the_live_installation(void) {
    /* Every assertion in this file is about an isolated Atlas. If a test could
     * reach the developer's own daemon, database, socket or backups, then a
     * passing run would prove something about the wrong machine — and a
     * *failing* one could damage it.
     *
     * The fixture's isolation is asserted here rather than assumed, because it
     * is the precondition for every other test in the suite. */
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_OK(fx_open(&fx, &err), &err);

    fx_daemon d;
    fx_daemon_init(&d);
    T_REQUIRE(fx_daemon_start(&fx, &d, &err) == ATLAS_OK);
    T_REQUIRE(fx_daemon_wait_ready(&d, 15000, &err) == ATLAS_OK);

    /* The socket is inside the fixture, not in the user's runtime directory. */
    const char *sock = atlas_buf_cstr(&d.socket);
    T_CHECK_MSG(strncmp(sock, fx.root.data, fx.root.len) == 0,
                "the fixture daemon's socket is outside the fixture: %s", sock);
    T_CHECK_MSG(strstr(sock, "/run/user/") == NULL,
                "the fixture daemon bound a socket in the real runtime directory: %s", sock);

    /* The data directory is inside the fixture, and is not the one the real
     * installation uses. */
    const char *dd = fx_data_dir(&fx);
    T_CHECK_MSG(strncmp(dd, fx.root.data, fx.root.len) == 0,
                "the fixture data directory is outside the fixture: %s", dd);
    T_CHECK_MSG(strstr(dd, "/.local/share/atlas") == NULL,
                "the fixture points at the real data directory: %s", dd);

    /* And the daemon this process talks to is the one it forked: its reported
     * pid is the child, and the data directory it says it owns is the
     * fixture's. Asked of the daemon rather than inferred from the path. */
    atlas_buf resp = ATLAS_BUF_INIT;
    T_OK(atlas_ipc_call(sock, "daemon.ping", "{}", &resp, &err), &err);
    char want_pid[64];
    (void)snprintf(want_pid, sizeof(want_pid), "\"pid\":%lld", (long long)d.pid);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), want_pid) != NULL,
                "answered by a daemon this test did not start (wanted %s): %s", want_pid,
                atlas_buf_cstr(&resp));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), dd) != NULL,
                "the daemon owns a different data directory: %s", atlas_buf_cstr(&resp));
    atlas_buf_free(&resp);

    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    fx_close(&fx);
}

/* --- 8. bounded mutation of the two untrusted parser boundaries ----------- */

/* A deterministic byte scrambler.
 *
 * Not `rand()`: a fuzz case that cannot be reproduced from the source alone is
 * a fuzz case nobody can act on when it fails on a build machine at 3am. This
 * is a fixed 64-bit LCG seeded from the iteration index, so run N is byte-for-
 * byte the same run N on any machine, and a failing index is a complete bug
 * report. */
static unsigned long long lcg(unsigned long long *s) {
    *s = *s * 6364136223846793005ULL + 1442695040888963407ULL;
    return *s >> 33;
}

static void test_mutated_frames_are_answered_or_refused_but_never_fatal(void) {
    /* The IPC boundary takes bytes from an untrusted peer, and the JSON parser
     * behind it is the one piece of third-party code Atlas vendors. This walks
     * a corpus of valid requests, mutates each one many ways, and requires the
     * daemon to still be alive and still answering afterwards.
     *
     * The assertion is deliberately weak on *what* comes back — a mutated
     * request may legitimately succeed, fail, or close the connection — and
     * strong on what must not happen: the daemon must not die, and it must
     * still answer a well-formed request when the round is over. A crash, a
     * hang or a sanitiser abort all fail this. Run under `make asan` and
     * `make ubsan`, which is where the memory-safety half of the claim comes
     * from; this test on its own only establishes liveness.
     *
     * Bounded and exhaustive rather than open-ended: 8 seeds over 12 templates,
     * 96 mutated frames, a fixed corpus, and no timing-dependent stopping
     * condition. */
    static const char *const CORPUS[] = {
        "{\"id\":\"1\",\"method\":\"daemon.ping\",\"params\":{}}",
        "{\"id\":\"1\",\"method\":\"repo.list\",\"params\":{}}",
        "{\"id\":\"1\",\"method\":\"repo.resolve\",\"params\":{\"path\":\"/opt\"}}",
        "{\"id\":\"1\",\"method\":\"decision.list\",\"params\":{\"repo\":\"proj\"}}",
        "{\"id\":\"1\",\"method\":\"gate.check\",\"params\":{\"repo\":\"proj\"}}",
        "{\"id\":\"1\",\"method\":\"code.symbol\",\"params\":{\"repo\":\"proj\",\"name\":\"main\"}}",
        "{\"id\":\"1\",\"method\":\"ai.session.open\",\"params\":{\"provider\":\"p\","
        "\"client\":\"c\",\"session_key\":\"k\",\"root\":\"/opt\"}}",
        "{\"id\":\"1\",\"method\":\"events.since\",\"params\":{\"repo\":\"proj\",\"after\":0}}",
        "{\"id\":\"1\",\"method\":\"daemon.status\",\"params\":{}}",
        "{\"id\":\"1\",\"method\":\"decision.get\",\"params\":{\"repo\":\"proj\","
        "\"decision\":\"atlas-dec-00000000000000000000000000000000\"}}",
        "{\"id\":\"1\",\"method\":\"ai.context\",\"params\":{\"root\":\"/opt\"}}",
        "{\"id\":\"1\",\"method\":\"code.status\",\"params\":{\"repo\":\"proj\"}}",
    };

    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e);

    fx_daemon d;
    fx_daemon_init(&d);
    T_REQUIRE(fx_daemon_start(&e.fx, &d, &err) == ATLAS_OK);
    T_REQUIRE(fx_daemon_wait_ready(&d, 15000, &err) == ATLAS_OK);

    size_t sent = 0;
    for (size_t t = 0; t < sizeof CORPUS / sizeof CORPUS[0]; t++) {
        for (unsigned seed = 0; seed < 8u; seed++) {
            char body[1024];
            size_t n = strlen(CORPUS[t]);
            T_REQUIRE(n < sizeof(body));
            memcpy(body, CORPUS[t], n);

            unsigned long long s = 0x9e3779b97f4a7c15ULL ^ (t * 1000u + seed);
            /* Between one and four edits, each of a kind that has historically
             * broken a parser: a flipped byte, a truncation, a doubled
             * structural character, and a NUL in the middle. */
            unsigned edits = (unsigned)(lcg(&s) % 4u) + 1u;
            for (unsigned k = 0; k < edits && n > 0; k++) {
                size_t at = (size_t)(lcg(&s) % n);
                switch (lcg(&s) % 4u) {
                case 0: body[at] = (char)(lcg(&s) & 0xffu); break;
                case 1: n = at; break;
                case 2:
                    if (n + 1u < sizeof(body)) {
                        memmove(body + at + 1u, body + at, n - at);
                        n++;
                    }
                    break;
                default: body[at] = '\0'; break;
                }
            }

            atlas_buf resp = ATLAS_BUF_INIT;
            bool closed = false;
            atlas_err e2;
            atlas_err_init(&e2);
            /* Rawframing, so the length prefix and the payload can disagree —
             * which the ordinary client would never let happen. */
            (void)fx_ipc_raw(atlas_buf_cstr(&d.socket), body, n, &resp, &closed, &e2);
            atlas_buf_free(&resp);
            sent++;

            T_REQUIRE_MSG(!fx_daemon_exited(&d),
                          "the daemon died on corpus %zu seed %u (%zu bytes)", t, seed, n);
        }
    }

    /* Still serving, and still serving *correctly* — a daemon that survived by
     * wedging its serve loop would pass a liveness check on the process alone. */
    atlas_buf resp = ATLAS_BUF_INIT;
    T_OK(atlas_ipc_call(atlas_buf_cstr(&d.socket), "daemon.ping", "{}", &resp, &err), &err);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "\"pong\":true") != NULL,
                "after %zu mutated frames the daemon no longer answers: %s", sent,
                atlas_buf_cstr(&resp));
    atlas_buf_free(&resp);

    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    env_close(&e);
}

static const atlas_test TESTS[] = {
    {"a socket peer cannot mint an approval capability",
     test_a_socket_peer_cannot_mint_an_approval_capability},
    {"a pseudo-terminal does not open a locked profile",
     test_a_pseudo_terminal_does_not_open_a_locked_profile},
    {"every lifecycle verb is refused in a locked profile",
     test_every_lifecycle_verb_is_refused_in_a_locked_profile},
    {"no unprivileged shape grants authority", test_no_unprivileged_shape_grants_authority},
    {"the suite cannot reach the live installation",
     test_the_suite_cannot_reach_the_live_installation},
    {"mutated frames are answered or refused but never fatal",
     test_mutated_frames_are_answered_or_refused_but_never_fatal},
    {"the daemon answers to no authority method",
     test_the_daemon_answers_to_no_authority_method},
    {"an unknown repository is a candidate and not a registration",
     test_an_unknown_repository_is_a_candidate_and_not_a_registration},
};

ATLAS_TEST_MAIN("a7_authority", TESTS)
