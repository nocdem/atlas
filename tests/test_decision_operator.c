/* Atlas - A4: the operator channel, and the exact shape of its limits.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * This suite is the honest demonstration of what `LOCAL_OPERATOR_CONFIRMED`
 * means, in both directions.
 *
 * It proves the channel **excludes** everything it claims to: an approval
 * cannot be produced by piped standard input, by a redirected terminal, by
 * `--yes`, by an environment variable, or by a JSON request. Those are real,
 * checkable properties.
 *
 * And it proves, by doing it, that the channel **is not an identity**: the
 * interactive test allocates a pseudo-terminal with `posix_openpt`, forks
 * `atlas` onto it, and types the confirmation from a program. If a test can do
 * that, so can anything else running as the same user — which is exactly why
 * Atlas records that its operator channel was used rather than claiming a
 * person acted. A suite that could not do this would be a suite whose subject
 * was making a stronger claim than the code supports.
 *
 * `posix_openpt` / `grantpt` / `unlockpt` / `ptsname_r` are POSIX and in libc.
 * No `-lutil`, no `forkpty`, no new dependency.
 */
#define _GNU_SOURCE 1

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/datadir.h"
#include "atlas/decision.h"
#include "atlas/decision_ops.h"
#include "atlas/proc.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"
#include "support/pty.h"

/* --- the fixture ------------------------------------------------------------ */

typedef struct env {
    fixture fx;
    atlas_buf uid;
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
    atlas_buf_free(&errout);
}

/* Registers a repository and proposes one decision, returning its id. */
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
        "decision",  "propose", "proj",  "--title", "Use WAL journalling",
        "--decision", "Enable WAL on the index database.", "--path", "main.c",
    };
    run_atlas(e, propose, 9u, &out, &code);
    T_EQ_INT(code, 0);

    /* The id, taken out of the human output rather than guessed. */
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

/* The document's status, read back through the CLI. */
static void expect_status(env *e, const char *want) {
    atlas_buf out = ATLAS_BUF_INIT;
    int code = 0;
    const char *show[] = {"decision", "show", "proj", atlas_buf_cstr(&e->uid)};
    run_atlas(e, show, 4u, &out, &code);
    T_EQ_INT(code, 0);
    char needle[64];
    (void)snprintf(needle, sizeof(needle), "status:       %s", want);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), needle) != NULL,
                "expected status %s, got:\n%s", want, atlas_buf_cstr(&out));
    atlas_buf_free(&out);
}

/* Approves `e->uid` through the write point, without the CLI.
 *
 * **A7.** The interactive path this suite was built around is refused in a
 * locked profile, and a locked profile is the only one an unprivileged test can
 * be in — a grant needs a root-owned policy at a root-owned path, which is
 * exactly what no test can manufacture. See `atlas/authority.h`.
 *
 * So the tests below that need an *approved* decision in order to check
 * something else — that the repository is not modified, that an approval does
 * not follow a path to an unrelated repository — seed it here instead, through
 * `atlas_decision_apply`. That is the same write point the CLI reaches and the
 * same one `tests/test_decision_lifecycle.c` uses; what it skips is the CLI
 * entry point, which is where A7 put the authority check and which those tests
 * are not about.
 *
 * The suite's own subject — that the operator channel excludes pipes, `--yes`,
 * JSON and a wrong answer — is unchanged and still driven through the CLI. */
static void approve_through_the_write_point(env *e) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf db_path = ATLAS_BUF_INIT;
    T_OK(atlas_datadir_db_path(fx_data_dir(&e->fx), &db_path, &err), &err);
    atlas_db *db = NULL;
    T_OK(atlas_db_open(atlas_buf_cstr(&db_path), &db, &err), &err);

    atlas_decision_op ch;
    atlas_decision_op_init(&ch, ATLAS_DECISION_OP_CHALLENGE);
    T_OK(atlas_buf_set_str(&ch.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set_str(&ch.uid, atlas_buf_cstr(&e->uid), &err), &err);
    ch.intent = ATLAS_DECISION_INTENT_APPROVE;
    atlas_decision_result cr;
    atlas_decision_result_init(&cr);
    T_OK(atlas_decision_apply(db, &ch, &cr, &err), &err);

    atlas_decision_op ap;
    atlas_decision_op_init(&ap, ATLAS_DECISION_OP_APPROVE);
    T_OK(atlas_buf_set_str(&ap.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set_str(&ap.uid, atlas_buf_cstr(&e->uid), &err), &err);
    T_OK(atlas_buf_set(&ap.token, cr.token.data, cr.token.len, &err), &err);
    T_OK(atlas_buf_set_str(&ap.confirmation, cr.confirm, &err), &err);
    atlas_decision_result ar;
    atlas_decision_result_init(&ar);
    T_OK(atlas_decision_apply(db, &ap, &ar, &err), &err);
    T_CHECK(ar.state == ATLAS_DECISION_APPROVED);

    atlas_decision_result_free(&ar);
    atlas_decision_op_free(&ap);
    atlas_decision_result_free(&cr);
    atlas_decision_op_free(&ch);
    atlas_db_close(db);
    atlas_buf_free(&db_path);
}

/* --- the interactive path ------------------------------------------------------- */

/* A7 replaced two tests here, and what replaced them is the finding.
 *
 * They were `interactive approval of the exact revision` and `a wrong answer at
 * the prompt changes nothing`: both drove a real pseudo-terminal, typed a
 * confirmation, and asserted on what the approval prompt displayed. Both
 * described a channel that A7 established cannot mean what it was taken to
 * mean, because `pty_spawn` (tests/support/pty.h) is the demonstration — a
 * program allocating a terminal and typing at it is indistinguishable, from
 * inside Atlas, from a person at a keyboard.
 *
 * So the prompt is no longer reached in a profile Atlas cannot separate, and
 * what is asserted is that it is not reached: not that the refusal happens
 * eventually, but that it happens *before* a capability exists and before a
 * question is asked whose answer this process could supply. */
static void test_the_prompt_is_never_reached_in_a_locked_profile(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e);

    const char *args[] = {"decision", "approve", "proj", atlas_buf_cstr(&e.uid)};
    pty p = {-1, -1};
    T_REQUIRE(pty_spawn(fx_data_dir(&e.fx), ATLAS_BIN, args, 4u, &p, &err) == ATLAS_OK);

    /* Typed blind, before anything is read.
     *
     * An adversary does not wait to be asked. The confirmation is the first
     * eight characters of the revision's content hash, which is printed by
     * `atlas decision show` and so is known to anything that can run Atlas at
     * all — it is a check against approving the wrong revision by accident, and
     * never was a secret. Sending it up front is the strongest version of this
     * test: nothing is waiting to receive it, and nothing must act on it. */
    pty_type(&p, "0123456789abcdef");

    /* `pty_expect` returns false on child exit, so a run that never prompts
     * ends here rather than blocking. The transcript is kept either way. */
    atlas_buf transcript = ATLAS_BUF_INIT;
    bool prompted = pty_expect(&p, "Type ", &transcript);
    int code = pty_wait(&p, &transcript);
    const char *text = atlas_buf_cstr(&transcript);

    T_CHECK_MSG(!prompted, "a locked profile still displayed the approval prompt:\n%s", text);
    T_CHECK_MSG(code != 0, "approval from a pseudo-terminal succeeded (exit %d):\n%s", code, text);
    T_CHECK_MSG(strstr(text, "locked in this Atlas profile") != NULL,
                "the refusal did not explain itself:\n%s", text);
    /* The refusal has to say what would change it, or it will be worked around
     * rather than acted on. */
    T_CHECK_MSG(strstr(text, "operator_uid") != NULL,
                "the refusal did not name the configuration that would enable it:\n%s", text);

    expect_status(&e, "PROPOSED");

    /* No capability was minted on the way to the refusal. One left behind in
     * the database would be spendable by anything that can read it. */
    atlas_buf db_path = ATLAS_BUF_INIT;
    T_OK(atlas_datadir_db_path(fx_data_dir(&e.fx), &db_path, &err), &err);
    atlas_db *db = NULL;
    T_OK(atlas_db_open_readonly(atlas_buf_cstr(&db_path), &db, &err), &err);
    int64_t challenges = -1;
    T_OK(atlas_db_query_int64(db, "SELECT COUNT(*) FROM decision_challenges;", &challenges, &err),
         &err);
    T_CHECK_MSG(challenges == 0, "a locked profile minted %lld capabilities",
                (long long)challenges);
    atlas_db_close(db);
    atlas_buf_free(&db_path);

    atlas_buf_free(&transcript);
    env_close(&e);
}

static void test_ansi_in_a_title_cannot_reach_the_prompt(void) {
    /* Two layers, and this checks that at least one of them holds.
     *
     * `atlas_decision_check_text` refuses an escape sequence at the point of
     * writing, so the hostile title never reaches storage. If that were ever
     * relaxed, `atlas_terminal_write` would still replace the byte. The test
     * asserts the outcome — no raw ESC on the terminal — rather than which
     * layer produced it, because that is the property that matters. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e);

    atlas_buf out = ATLAS_BUF_INIT;
    int code = 0;
    const char *hostile[] = {
        "decision", "propose", "proj",
        "--title",  "\x1b[2J\x1b[HApproved by the security team",
        "--decision", "Nothing.",
    };
    run_atlas(&e, hostile, 7u, &out, &code);
    T_CHECK_MSG(code != 0, "a title containing an ANSI escape must be refused outright");
    atlas_buf_free(&out);

    /* And the same for the body, plus a bidi override and a C1 control — each
     * of which is a way to make a terminal display something other than what is
     * stored. */
    static const char *const payloads[] = {
        "\x1b]0;pwned\x07",       /* an OSC title-setting sequence */
        "\xe2\x80\xae" "reversed", /* a bidi override */
        "\xc2\x9b" "31m",          /* C1 CSI */
        "line\rline",              /* a carriage return, which overwrites */
        NULL,
    };
    for (size_t i = 0; payloads[i] != NULL; i++) {
        const char *args[] = {
            "decision", "propose", "proj", "--title", "Fine", "--decision", payloads[i],
        };
        atlas_buf o = ATLAS_BUF_INIT;
        int c = 0;
        run_atlas(&e, args, 7u, &o, &c);
        T_CHECK_MSG(c != 0, "payload %zu must be refused", i);
        atlas_buf_free(&o);
    }

    /* What reaches the terminal on the *legitimate* decision is still checked
     * byte by byte — it is now a refusal rather than a prompt, and a refusal is
     * printed to a terminal by the same code and carries the same risk. Atlas
     * quotes no repository-authored text in it, so anything outside printable
     * ASCII arriving here would be a regression in the message itself. */
    const char *args[] = {"decision", "approve", "proj", atlas_buf_cstr(&e.uid)};
    pty p = {-1, -1};
    T_REQUIRE(pty_spawn(fx_data_dir(&e.fx), ATLAS_BIN, args, 4u, &p, &err) == ATLAS_OK);
    atlas_buf transcript = ATLAS_BUF_INIT;
    (void)pty_expect(&p, "locked in this Atlas profile", &transcript);
    (void)pty_wait(&p, &transcript);
    for (size_t i = 0; i < transcript.len; i++) {
        unsigned char ch = (unsigned char)transcript.data[i];
        bool ok = ch == '\n' || ch == '\r' || (ch >= 0x20u && ch < 0x7Fu);
        T_CHECK_MSG(ok, "byte 0x%02x reached the terminal at offset %zu", ch, i);
    }

    atlas_buf_free(&transcript);
    env_close(&e);
}

/* --- the exclusions ---------------------------------------------------------------- */

static void test_non_interactive_approval_is_refused(void) {
    /* Every non-interactive shape, each with the neighbouring record checked
     * afterwards. `fx_atlas` gives the child `/dev/null` on standard input,
     * which is the pipe case. */
    atlas_err err;
    atlas_err_init(&err);
    (void)err;
    env e;
    env_open(&e);

    struct {
        const char *what;
        const char *args[8];
        size_t n;
    } cases[] = {
        {"no terminal", {"decision", "approve", "proj", NULL}, 4u},
        {"--yes", {"decision", "approve", "proj", NULL, "--yes"}, 5u},
        {"--json", {"decision", "approve", "proj", NULL, "--json"}, 5u},
        {"reject with --yes", {"decision", "reject", "proj", NULL, "--yes"}, 5u},
        {"supersede with --yes",
         {"decision", "supersede", "proj", NULL, "--by", "atlas-dec-0000000000000000", "--yes"},
         7u},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        cases[i].args[3] = atlas_buf_cstr(&e.uid);
        atlas_buf out = ATLAS_BUF_INIT;
        int code = 0;
        run_atlas(&e, cases[i].args, cases[i].n, &out, &code);
        T_CHECK_MSG(code != 0, "%s must be refused, and exited %d", cases[i].what, code);
        atlas_buf_free(&out);
        /* After every refusal, not only after the last. */
        expect_status(&e, "PROPOSED");
    }

    /* An environment variable cannot stand in for a terminal either. There is
     * no variable that does this, and the test exists to keep it that way:
     * anything added later that looks like one has to break it. */
    {
        const char *args[] = {"decision", "approve", "proj", atlas_buf_cstr(&e.uid)};
        const char *env_attempts[] = {
            "ATLAS_YES=1", "ATLAS_APPROVE=1", "ATLAS_ASSUME_YES=1", "ATLAS_CONFIRM=yes", NULL,
        };
        atlas_buf out = ATLAS_BUF_INIT;
        atlas_buf errout = ATLAS_BUF_INIT;
        int code = 0;
        const char *argv[8];
        size_t k = 0;
        argv[k++] = "--data-dir";
        argv[k++] = fx_data_dir(&e.fx);
        for (size_t i = 0; i < 4u; i++) {
            argv[k++] = args[i];
        }
        atlas_err ferr;
        atlas_err_init(&ferr);
        T_OK(fx_atlas_stdin(argv, k, env_attempts, "", 0u, &out, &errout, &code, &ferr), &ferr);
        T_CHECK_MSG(code != 0, "no environment variable may authorise an approval");
        atlas_buf_free(&out);
        atlas_buf_free(&errout);
        expect_status(&e, "PROPOSED");
    }

    env_close(&e);
}

static void test_the_repository_is_never_modified(void) {
    /* The A0 guarantee, still true for a phase whose whole subject is writing
     * things down. A decision is Atlas' record; the project's files are not
     * touched by proposing, approving or exporting one. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e);

    char before[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(fx_repo(&e.fx), before, &err), &err);

    approve_through_the_write_point(&e);

    /* And an export writes to stdout, never into the tree. */
    atlas_buf out = ATLAS_BUF_INIT;
    int code = 0;
    const char *ex[] = {"decision", "export", "proj", atlas_buf_cstr(&e.uid)};
    run_atlas(&e, ex, 4u, &out, &code);
    T_EQ_INT(code, 0);
    T_CHECK(strstr(atlas_buf_cstr(&out), "# Use WAL journalling") != NULL);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "is not a signature") != NULL,
                "an exported decision must carry the non-claim with it");
    atlas_buf_free(&out);

    char after[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(fx_repo(&e.fx), after, &err), &err);
    T_CHECK_MSG(strcmp(before, after) == 0,
                "the registered repository must be byte-identical after an approval and an export");

    env_close(&e);
}

static void test_doctor_reports_a_ledger_disagreement_and_repairs_nothing(void) {
    /* The ledger is canonical and the status columns are a cache of it. Doctor
     * replays the ledger and compares — and **reports rather than repairs**,
     * because a diagnostic that silently fixes what it finds cannot tell you
     * whether the fault recurs.
     *
     * Checked by running doctor twice after corrupting the cache: a repairing
     * doctor would report the problem once and then claim health. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e);

    atlas_buf out = ATLAS_BUF_INIT;
    int code = 0;
    const char *doctor[] = {"doctor"};
    run_atlas(&e, doctor, 1u, &out, &code);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "cached status disagrees") == NULL,
                "a healthy index must report no ledger disagreement");
    atlas_buf_reset(&out);

    /* Claim, in the cache only, that the proposal is approved. */
    atlas_buf db_path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&db_path, &err, "%s/atlas.db", fx_data_dir(&e.fx)), &err);
    atlas_db *db = NULL;
    T_OK(atlas_db_open(atlas_buf_cstr(&db_path), &db, &err), &err);
    T_OK(atlas_db_exec_sql(db,
                           "UPDATE decision_documents"
                           " SET current_status = 'APPROVED', current_revision_id = 1;",
                           &err),
         &err);
    atlas_db_close(db);
    atlas_buf_free(&db_path);

    for (int pass = 0; pass < 2; pass++) {
        run_atlas(&e, doctor, 1u, &out, &code);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "cached status disagrees") != NULL,
                    "pass %d: doctor must report the disagreement, and must not have repaired it "
                    "on the previous pass. Output:\n%s",
                    pass, atlas_buf_cstr(&out));
        atlas_buf_reset(&out);
    }

    /* And the decision itself is untouched by the diagnostic: doctor observes. */
    const char *history[] = {"decision", "history", "proj", atlas_buf_cstr(&e.uid)};
    run_atlas(&e, history, 4u, &out, &code);
    T_EQ_INT(code, 0);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "APPROVED") == NULL,
                "no approval event may exist; only the cache was corrupted");
    atlas_buf_free(&out);
    env_close(&e);
}

static void test_an_unrelated_repository_at_the_same_path_inherits_nothing(void) {
    /* The scenario the correction pass exists for.
     *
     * `repo_root_hash` answers "same directory", and a directory is a location
     * rather than an identity. Remove a project, `git init` an unrelated one in
     * its place, register that — and a path hash says they are the same
     * repository, so one team's approved decisions attach to another team's
     * code. The durable identity commits to the ingested root commits as well,
     * which is what tells them apart.
     *
     * Driven through the real CLI against real Git repositories, because the
     * thing under test is what happens to a *worktree*. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e);

    /* Approve it, so what must not move is an approved record rather than a
     * draft. Seeded through the write point: the CLI route is locked here, and
     * lineage is what this test is about. */
    approve_through_the_write_point(&e);
    expect_status(&e, "APPROVED");

    /* Remove the repository. The decision is detached, not deleted. */
    atlas_buf out = ATLAS_BUF_INIT;
    int code = 0;
    const char *rm[] = {"repo", "remove", "proj", "--yes"};
    run_atlas(&e, rm, 4u, &out, &code);
    T_EQ_INT(code, 0);
    atlas_buf_reset(&out);

    const char *orphaned[] = {"decision", "orphaned"};
    run_atlas(&e, orphaned, 2u, &out, &code);
    T_EQ_INT(code, 0);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), atlas_buf_cstr(&e.uid)) != NULL,
                "a removed repository's decision must be visible as orphaned, not vanish: %s",
                atlas_buf_cstr(&out));
    atlas_buf_reset(&out);

    /* Replace the working tree with a genuinely unrelated repository at the
     * same path: a different history, a different root commit. */
    /* Removed with `rm -rf`, through the same explicit-argv process API the
     * rest of the suite uses — no shell. `fx_remove` unlinks one file, and the
     * point here is to destroy a whole worktree. */
    {
        const char *rm_argv[] = {"/bin/rm", "-rf", fx_repo(&e.fx), NULL};
        atlas_proc_opts opts;
        memset(&opts, 0, sizeof(opts));
        opts.argv = rm_argv;
        atlas_proc_result res;
        memset(&res, 0, sizeof(res));
        atlas_buf rm_err = ATLAS_BUF_INIT;
        T_OK(atlas_proc_run(&opts, NULL, NULL, &rm_err, &res, &err), &err);
        T_EQ_INT(res.exit_code, 0);
        atlas_buf_free(&rm_err);
    }
    T_OK(fx_mkdir(e.fx.root.data, "repo", &err), &err);
    T_OK(fx_init_repo(&e.fx, fx_repo(&e.fx), NULL, &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "unrelated.c", "int other(void){return 1;}\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "a different project entirely", &err), &err);

    const char *add[] = {"repo", "add", fx_repo(&e.fx), "--name", "other"};
    run_atlas(&e, add, 5u, &out, &code);
    T_EQ_INT(code, 0);
    atlas_buf_reset(&out);
    const char *scan[] = {"scan", "other"};
    run_atlas(&e, scan, 2u, &out, &code);
    T_EQ_INT(code, 0);
    atlas_buf_reset(&out);

    /* **The unrelated repository must have inherited nothing.** */
    const char *list[] = {"--json", "decision", "list", "other"};
    run_atlas(&e, list, 4u, &out, &code);
    T_EQ_INT(code, 0);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), atlas_buf_cstr(&e.uid)) == NULL,
                "an unrelated repository at the same path inherited a decision: %s",
                atlas_buf_cstr(&out));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"total_approved\":0") != NULL,
                "and must certainly not inherit an approved one: %s", atlas_buf_cstr(&out));
    atlas_buf_reset(&out);

    /* And the decision is still there, still approved, still visible as an
     * orphan. Nothing was deleted. */
    run_atlas(&e, orphaned, 2u, &out, &code);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), atlas_buf_cstr(&e.uid)) != NULL,
                "the original decision must survive as an orphan: %s", atlas_buf_cstr(&out));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "APPROVED") != NULL,
                "and keep its approved status");
    atlas_buf_free(&out);
    env_close(&e);
}

static const atlas_test TESTS[] = {
    {"an unrelated repository at the same path inherits nothing",
     test_an_unrelated_repository_at_the_same_path_inherits_nothing},
    {"the prompt is never reached in a locked profile",
     test_the_prompt_is_never_reached_in_a_locked_profile},
    {"ANSI in a title cannot reach the prompt", test_ansi_in_a_title_cannot_reach_the_prompt},
    {"non-interactive approval is refused", test_non_interactive_approval_is_refused},
    {"the repository is never modified", test_the_repository_is_never_modified},
    {"doctor reports a ledger disagreement and repairs nothing",
     test_doctor_reports_a_ledger_disagreement_and_repairs_nothing},
};

ATLAS_TEST_MAIN("decision_operator", TESTS)
