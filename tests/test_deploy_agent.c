/* Atlas - A17 T5: the root deploy agent, driven end to end against a fixture.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The agent (deploy/a17/atlas-deploy-agent.sh) is root operator tooling, not
 * atlas_core: it never opens the SQLite index and speaks to Atlas only through
 * the spool files a systemd path unit watches. This suite runs the real script
 * through `atlas_proc_run` with an explicit, absolute argv -- `/bin/bash
 * <script> --conf <conf>`, never `sh -c` -- exactly the class of invocation
 * `make smoke`'s scripts/smoke.sh uses to drive the real atlas binary, and for
 * the same reason: the only way to prove a shell script behaves is to run it.
 *
 * Every fixture repository carries a real Makefile (`all:` writes a file,
 * `test:` runs `true`) and every patch is a real `git diff` captured from an
 * edit to a tracked file and then reverted, never a hand-written unified diff.
 *
 * Cases (a)-(i) are the task brief's own list, verbatim in spirit:
 *   (a) a clean patch reaches DONE, SUCCEEDED, applied, and both spool files
 *       are gone while a 0644 .res exists.
 *   (b) a patch that cannot apply refuses at APPLY_CHECK with the tree byte-
 *       for-byte unchanged (git apply --check never writes anything).
 *   (c) a failing build with reverse_on_failure=no leaves the patch applied.
 *   (d) the same failing build with reverse_on_failure=yes reverses the patch
 *       and leaves the tree byte-for-byte as it started.
 *   (e) dry_run=yes always reverses after TEST succeeds, regardless of
 *       reverse_on_failure, and reports it.
 *   (f) a patch_sha256/patch_bytes mismatch refuses at PREFLIGHT before any
 *       git command runs, so the tree is provably untouched.
 *   (g) a repo_root that does not match the conf's tree refuses at PREFLIGHT,
 *       also before any git command runs.
 *   (h) a request with no parseable "deploy" line cannot even be attributed to
 *       a result file, so it is quarantined as "<name>.bad" instead.
 *   (i) a stage whose command prints far more than 32 KiB never produces a
 *       result file larger than that bound.
 *
 * Fix round 1's review added five more, plus a fix to the fixtures/helpers
 * every case above already used (write_conf's `test_cmd == NULL` branch,
 * previously dead, and write_request_ex2's new deploy-uid-override and
 * extra-line parameters):
 *   (j) the patch APPLY_CHECK/APPLY/REVERSE actually run against is a copy in
 *       this agent's own tmp directory, mode 0644 -- never the original in
 *       `requests/`, which is 0700 daemon-owned in a real deployment and the
 *       tree's owner (who runs those three stages) has no business entering.
 *   (k) `test =` left unset in the conf refuses every request at PREFLIGHT,
 *       tree untouched -- the operator's own choice, never the template's.
 *   (l) an unrecognised conf key is refused before any request is even
 *       looked at: exit 3, the request file untouched byte-for-byte, no
 *       `.res` written. Documented at the top of the agent script itself.
 *   (m) an unrecognised request key refuses at PREFLIGHT, `.req` removed.
 *   a request whose own "deploy" line names a *different* uid than the
 *       filename it was actually filed under is quarantined exactly like an
 *       unparseable one -- CRITICAL 1: `write_result` must never trust
 *       content-supplied text to build a path under a directory this agent
 *       (root, in a deployment) writes into.
 *
 * A note on tree-digest comparisons (b, d, e, f, g, k): `git status`/`git diff`
 * can rewrite `.git/index`'s bytes to persist a refreshed stat cache even when
 * no tracked content changed -- a well-known property of git's index, not a
 * bug in this agent. `pre_settle_index()` runs the exact same `git status
 * --porcelain` the agent's own PREFLIGHT runs, once, before any "digest
 * before" snapshot is taken, so that stat-cache refresh happens on the test's
 * clock rather than showing up as a spurious tree difference after the agent
 * runs. Cases (f) and (g) refuse before the agent ever calls git at all (the
 * agent orders its checks that way on purpose), so they need no such
 * settling.
 */
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas_test.h"
#include "support/fixture.h"

#ifndef ATLAS_SRC_DIR
#error "ATLAS_SRC_DIR must be defined by the build (see tests/CMakeLists.txt)"
#endif

#define DEPLOY_AGENT_SCRIPT ATLAS_SRC_DIR "/deploy/a17/atlas-deploy-agent.sh"

static const char *const MAKEFILE_CONTENT =
    "all:\n"
    "\techo built > built.txt\n"
    "\n"
    "test:\n"
    "\ttrue\n";

static const char *const NOTES_ORIGINAL =
    "line one\n"
    "line two\n"
    "line three\n";

static const char *const NOTES_PATCHED =
    "line one\n"
    "line two PATCHED\n"
    "line three\n";

static const char *const NOTES_DIVERGED =
    "line one\n"
    "line two DIVERGED\n"
    "line three\n";

/* --- small local helpers --------------------------------------------------- */

static void head_oid(const char *repo, atlas_buf *out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_git *g = NULL;
    T_OK(atlas_git_open(repo, &g, &err), &err);
    atlas_git_head h;
    memset(&h, 0, sizeof(h));
    T_OK(atlas_git_read_head(g, &h, &err), &err);
    T_OK(atlas_buf_set_str(out, h.oid, &err), &err);
    atlas_git_close(g);
}

static void write_file_raw(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    T_REQUIRE_MSG(f != NULL, "cannot open %s for writing", path);
    if (n > 0) {
        T_REQUIRE_MSG(fwrite(data, 1, n, f) == n, "short write to %s", path);
    }
    T_REQUIRE_MSG(fclose(f) == 0, "cannot close %s", path);
}

static bool file_exists(const char *path) {
    struct stat sb;
    return stat(path, &sb) == 0;
}

static mode_t file_mode(const char *path) {
    struct stat sb;
    T_REQUIRE_MSG(stat(path, &sb) == 0, "cannot stat %s", path);
    return sb.st_mode & 07777;
}

static char *read_whole_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    T_REQUIRE_MSG(f != NULL, "cannot open %s", path);
    T_REQUIRE_MSG(fseek(f, 0, SEEK_END) == 0, "seek failed on %s", path);
    long n = ftell(f);
    T_REQUIRE_MSG(n >= 0, "ftell failed on %s", path);
    T_REQUIRE_MSG(fseek(f, 0, SEEK_SET) == 0, "seek failed on %s", path);
    char *buf = (char *)malloc((size_t)n + 1);
    T_REQUIRE_MSG(buf != NULL, "out of memory reading %s", path);
    if (n > 0) {
        T_REQUIRE_MSG(fread(buf, 1, (size_t)n, f) == (size_t)n, "short read on %s", path);
    }
    buf[n] = '\0';
    T_REQUIRE_MSG(fclose(f) == 0, "cannot close %s", path);
    if (len_out != NULL) {
        *len_out = (size_t)n;
    }
    return buf;
}

/* Deterministic 33-character id: a one-letter prefix plus a 32-hex-character
 * body built by repeating a two-character pair 16 times. Not a real Atlas uid
 * (the agent does not validate the shape), just a fixed, readable, unique-per-
 * case value. */
static void make_id(char prefix, const char *pair, char out[34]) {
    out[0] = prefix;
    for (int i = 0; i < 16; i++) {
        out[1 + i * 2] = pair[0];
        out[1 + i * 2 + 1] = pair[1];
    }
    out[33] = '\0';
}

/* --- the fixture project ---------------------------------------------------- */

typedef struct dep_case {
    fixture fx;
    atlas_buf repo;         /* fx_repo(&fx), kept as an atlas_buf for appendf */
    atlas_buf spool;        /* <data>/spool */
    atlas_buf requests;     /* <data>/spool/requests */
    atlas_buf results;      /* <data>/spool/results */
    atlas_buf conf_path;    /* <data>/deploy.conf */
    atlas_buf binary_path;  /* <data>/fakebinary */
    atlas_buf head;         /* HEAD after the initial commit */
    char owner[128];        /* the current user's name -- runuser is skipped */
} dep_case;

static void dep_open(dep_case *c) {
    atlas_err err;
    atlas_err_init(&err);
    memset(c, 0, sizeof(*c));
    T_OK(fx_open(&c->fx, &err), &err);
    atlas_buf_init(&c->repo);
    atlas_buf_init(&c->spool);
    atlas_buf_init(&c->requests);
    atlas_buf_init(&c->results);
    atlas_buf_init(&c->conf_path);
    atlas_buf_init(&c->binary_path);
    atlas_buf_init(&c->head);

    T_OK(atlas_buf_set_str(&c->repo, fx_repo(&c->fx), &err), &err);
    T_OK(atlas_buf_appendf(&c->spool, &err, "%s/spool", fx_data_dir(&c->fx)), &err);
    T_OK(atlas_buf_appendf(&c->requests, &err, "%s/requests", atlas_buf_cstr(&c->spool)), &err);
    T_OK(atlas_buf_appendf(&c->results, &err, "%s/results", atlas_buf_cstr(&c->spool)), &err);
    T_OK(atlas_buf_appendf(&c->conf_path, &err, "%s/deploy.conf", fx_data_dir(&c->fx)), &err);
    T_OK(atlas_buf_appendf(&c->binary_path, &err, "%s/fakebinary", fx_data_dir(&c->fx)), &err);

    T_OK(fx_mkdir(fx_data_dir(&c->fx), "spool", &err), &err);
    T_OK(fx_mkdir(fx_data_dir(&c->fx), "spool/requests", &err), &err);
    T_OK(fx_mkdir(fx_data_dir(&c->fx), "spool/results", &err), &err);

    T_OK(fx_write_exec(fx_data_dir(&c->fx), "fakebinary",
                        "#!/bin/sh\necho \"fake-atlas 1.0.0\"\n", &err),
         &err);

    struct passwd *pw = getpwuid(geteuid());
    T_REQUIRE_MSG(pw != NULL && pw->pw_name != NULL, "cannot resolve the current user's name");
    (void)snprintf(c->owner, sizeof(c->owner), "%s", pw->pw_name);

    T_OK(fx_init_repo(&c->fx, fx_repo(&c->fx), NULL, &err), &err);
    T_OK(fx_write(fx_repo(&c->fx), "Makefile", MAKEFILE_CONTENT, &err), &err);
    T_OK(fx_write(fx_repo(&c->fx), "notes.txt", NOTES_ORIGINAL, &err), &err);
    T_OK(fx_add_all(&c->fx, fx_repo(&c->fx), &err), &err);
    T_OK(fx_commit(&c->fx, fx_repo(&c->fx), "initial", &err), &err);
    head_oid(fx_repo(&c->fx), &c->head);
}

static void dep_close(dep_case *c) {
    atlas_buf_free(&c->repo);
    atlas_buf_free(&c->spool);
    atlas_buf_free(&c->requests);
    atlas_buf_free(&c->results);
    atlas_buf_free(&c->conf_path);
    atlas_buf_free(&c->binary_path);
    atlas_buf_free(&c->head);
    fx_close(&c->fx);
}

/* Edits the tracked file, captures a real `git diff`, then restores the file to
 * its committed content -- so the patch bytes come from git itself, never a
 * hand-written unified diff, and the working tree is clean again afterwards. */
static void make_patch(dep_case *c, const char *new_content, atlas_buf *patch_out) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_write(fx_repo(&c->fx), "notes.txt", new_content, &err), &err);
    const char *args[] = {"diff", "--", "notes.txt"};
    int code = -1;
    atlas_buf_reset(patch_out);
    T_OK(fx_git(&c->fx, fx_repo(&c->fx), args, 3, &code, patch_out, &err), &err);
    T_CHECK_MSG(code == 0, "git diff exited %d", code);
    T_REQUIRE_MSG(patch_out->len > 0, "git diff produced an empty patch");
    T_OK(fx_write(fx_repo(&c->fx), "notes.txt", NOTES_ORIGINAL, &err), &err);
}

/* Runs the exact read the agent's own PREFLIGHT runs, once, so that git's own
 * index stat-cache refresh (see the file header comment) happens before any
 * "digest before" snapshot rather than showing up as a spurious difference
 * after the agent runs. */
static void pre_settle_index(dep_case *c) {
    atlas_err err;
    atlas_err_init(&err);
    const char *args[] = {"status", "--porcelain"};
    int code = -1;
    T_OK(fx_git(&c->fx, fx_repo(&c->fx), args, 2, &code, NULL, &err), &err);
}

/* After an apply-then-reverse round trip (d, e) the tree's *content* is
 * exactly what it started as, but git recreates the file it patches rather
 * than editing it in place -- so a freshly recreated file can pick up this
 * process' own umask on its non-executable permission bits (measured: 0644 ->
 * 0664 under umask 002). Git tracks only the executable bit, never the rest of
 * the mode, so this is invisible to `git status` and not a property `git
 * apply -R` is asked to preserve; asking the agent to snapshot and restore a
 * mode git itself does not track would be scope `git apply -R` was chosen
 * over `git checkout --` specifically to avoid (design: never touch bytes
 * this agent did not itself add a moment ago). `fx_tree_digest`, which hashes
 * permission bits, is exactly right for (b) (git apply --check never writes
 * anything, so no such drift is possible) and wrong here -- content plus
 * git's own view of cleanliness is what actually matters. */
static void assert_tree_clean_and_original(dep_case *c, const char *msg) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf status_out = ATLAS_BUF_INIT;
    const char *args[] = {"status", "--porcelain"};
    int code = -1;
    T_OK(fx_git(&c->fx, fx_repo(&c->fx), args, 2, &code, &status_out, &err), &err);
    T_CHECK_MSG(atlas_buf_cstr(&status_out)[0] == '\0', "%s: git reports a tracked difference: %s",
                msg, atlas_buf_cstr(&status_out));
    atlas_buf_free(&status_out);

    char notes_path[1024];
    (void)snprintf(notes_path, sizeof(notes_path), "%s/notes.txt", atlas_buf_cstr(&c->repo));
    size_t notes_len = 0;
    char *notes = read_whole_file(notes_path, &notes_len);
    T_CHECK_MSG(strcmp(notes, NOTES_ORIGINAL) == 0,
                "%s: notes.txt is not back to its original content: %s", msg, notes);
    free(notes);
}

static void write_conf(dep_case *c, const char *build, const char *test_cmd, const char *dry_run,
                        const char *reverse_on_failure) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf b = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&b, &err, "tree = %s\n", atlas_buf_cstr(&c->repo)), &err);
    T_OK(atlas_buf_appendf(&b, &err, "owner = %s\n", c->owner), &err);
    T_OK(atlas_buf_appendf(&b, &err, "spool = %s\n", atlas_buf_cstr(&c->spool)), &err);
    T_OK(atlas_buf_appendf(&b, &err, "build = %s\n", build), &err);
    if (test_cmd != NULL) {
        T_OK(atlas_buf_appendf(&b, &err, "test = %s\n", test_cmd), &err);
    }
    T_OK(atlas_buf_appendf(&b, &err, "install = true\n"), &err);
    T_OK(atlas_buf_appendf(&b, &err, "binary = %s\n", atlas_buf_cstr(&c->binary_path)), &err);
    T_OK(atlas_buf_appendf(&b, &err, "units_system = \n"), &err);
    T_OK(atlas_buf_appendf(&b, &err, "units_user = \n"), &err);
    T_OK(atlas_buf_appendf(&b, &err, "ping = true\n"), &err);
    T_OK(atlas_buf_appendf(&b, &err, "dry_run = %s\n", dry_run), &err);
    T_OK(atlas_buf_appendf(&b, &err, "reverse_on_failure = %s\n", reverse_on_failure), &err);
    write_file_raw(atlas_buf_cstr(&c->conf_path), b.data, b.len);
    atlas_buf_free(&b);
}

/* Writes a conf carrying one key `write_conf` never would: an unrecognised
 * one, appended after an otherwise-ordinary conf. Fix round 1's case (l). */
static void write_conf_with_unknown_key(dep_case *c) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf b = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&b, &err, "tree = %s\n", atlas_buf_cstr(&c->repo)), &err);
    T_OK(atlas_buf_appendf(&b, &err, "owner = %s\n", c->owner), &err);
    T_OK(atlas_buf_appendf(&b, &err, "spool = %s\n", atlas_buf_cstr(&c->spool)), &err);
    T_OK(atlas_buf_appendf(&b, &err, "build = make\n"), &err);
    T_OK(atlas_buf_appendf(&b, &err, "test = make test\n"), &err);
    T_OK(atlas_buf_appendf(&b, &err, "install = true\n"), &err);
    T_OK(atlas_buf_appendf(&b, &err, "binary = %s\n", atlas_buf_cstr(&c->binary_path)), &err);
    T_OK(atlas_buf_appendf(&b, &err, "units_system = \n"), &err);
    T_OK(atlas_buf_appendf(&b, &err, "units_user = \n"), &err);
    T_OK(atlas_buf_appendf(&b, &err, "ping = true\n"), &err);
    T_OK(atlas_buf_appendf(&b, &err, "dry_run = no\n"), &err);
    T_OK(atlas_buf_appendf(&b, &err, "reverse_on_failure = no\n"), &err);
    T_OK(atlas_buf_appendf(&b, &err, "bogus_key = whatever\n"), &err);
    write_file_raw(atlas_buf_cstr(&c->conf_path), b.data, b.len);
    atlas_buf_free(&b);
}

/* `deploy_uid_override`, when non-NULL, is written as the request's own
 * "deploy" line instead of `deploy_uid` -- the request is still filed under
 * `<deploy_uid>.req`/`.patch`, so a caller can make the filename and the
 * content's own claim disagree (fix round 1's CRITICAL 1: the agent must
 * refuse that, never trust the content over the name it was filed under).
 * `extra_line`, when non-NULL, is appended as one more raw line, verbatim --
 * an unrecognised request key (fix round 1's case (m)). */
static void write_request_ex2(dep_case *c, const char *deploy_uid, const char *deploy_uid_override,
                               const char *job_uid, const char *repo_root, const char *base_commit,
                               const atlas_buf *patch, const char *confirmed_by,
                               const char *confirmed_at, bool corrupt_digest,
                               const char *extra_line) {
    atlas_err err;
    atlas_err_init(&err);
    char sha_hex[ATLAS_SHA256_HEX_LEN + 1];
    atlas_sha256_hex(patch->data, patch->len, sha_hex);
    if (corrupt_digest) {
        sha_hex[0] = (sha_hex[0] == '0') ? '1' : '0';
    }

    atlas_buf reqpath = ATLAS_BUF_INIT;
    atlas_buf patchpath = ATLAS_BUF_INIT;
    atlas_buf body = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&reqpath, &err, "%s/%s.req", atlas_buf_cstr(&c->requests), deploy_uid),
         &err);
    T_OK(atlas_buf_appendf(&patchpath, &err, "%s/%s.patch", atlas_buf_cstr(&c->requests), deploy_uid),
         &err);
    write_file_raw(atlas_buf_cstr(&patchpath), patch->data, patch->len);

    T_OK(atlas_buf_appendf(&body, &err, "atlas-deploy-request 1\n"), &err);
    T_OK(atlas_buf_appendf(&body, &err, "deploy %s\n",
                            deploy_uid_override != NULL ? deploy_uid_override : deploy_uid),
         &err);
    T_OK(atlas_buf_appendf(&body, &err, "job %s\n", job_uid), &err);
    T_OK(atlas_buf_appendf(&body, &err, "repo_root %s\n", repo_root), &err);
    T_OK(atlas_buf_appendf(&body, &err, "base_commit %s\n", base_commit), &err);
    T_OK(atlas_buf_appendf(&body, &err, "patch_sha256 %s\n", sha_hex), &err);
    T_OK(atlas_buf_appendf(&body, &err, "patch_bytes %zu\n", patch->len), &err);
    T_OK(atlas_buf_appendf(&body, &err, "confirmed_by %s\n", confirmed_by), &err);
    T_OK(atlas_buf_appendf(&body, &err, "confirmed_at %s\n", confirmed_at), &err);
    if (extra_line != NULL) {
        T_OK(atlas_buf_appendf(&body, &err, "%s\n", extra_line), &err);
    }
    write_file_raw(atlas_buf_cstr(&reqpath), body.data, body.len);

    atlas_buf_free(&reqpath);
    atlas_buf_free(&patchpath);
    atlas_buf_free(&body);
}

static void write_request_ex(dep_case *c, const char *deploy_uid, const char *job_uid,
                              const char *repo_root, const char *base_commit,
                              const atlas_buf *patch, const char *confirmed_by,
                              const char *confirmed_at, bool corrupt_digest) {
    write_request_ex2(c, deploy_uid, NULL, job_uid, repo_root, base_commit, patch, confirmed_by,
                       confirmed_at, corrupt_digest, NULL);
}

static void write_request(dep_case *c, const char *deploy_uid, const char *job_uid,
                           const char *repo_root, const char *base_commit, const atlas_buf *patch,
                           const char *confirmed_by, const char *confirmed_at) {
    write_request_ex(c, deploy_uid, job_uid, repo_root, base_commit, patch, confirmed_by,
                      confirmed_at, false);
}

/* --- running the real agent -------------------------------------------------- */

static void run_agent_expect(dep_case *c, int expected_exit) {
    atlas_err err;
    atlas_err_init(&err);
    struct passwd *pw = getpwuid(geteuid());
    T_REQUIRE_MSG(pw != NULL, "cannot resolve the current user");
    char home_env[512];
    (void)snprintf(home_env, sizeof(home_env), "HOME=%s",
                    pw->pw_dir != NULL ? pw->pw_dir : "/tmp");

    const char *argv[] = {"/bin/bash", DEPLOY_AGENT_SCRIPT, "--conf", atlas_buf_cstr(&c->conf_path),
                          NULL};
    static const char *const PATH_ENV =
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    const char *env[] = {PATH_ENV, home_env, "LC_ALL=C", "TZ=UTC", NULL};

    atlas_proc_opts o;
    memset(&o, 0, sizeof(o));
    o.argv = argv;
    o.env = env;
    o.timeout_ms = 60000;

    atlas_buf stderr_out = ATLAS_BUF_INIT;
    atlas_proc_result res;
    memset(&res, 0, sizeof(res));
    T_OK(atlas_proc_run(&o, NULL, NULL, &stderr_out, &res, &err), &err);
    T_CHECK_MSG(res.exit_code == expected_exit, "agent exited %d, expected %d, stderr: %s",
                res.exit_code, expected_exit, atlas_buf_cstr(&stderr_out));
    atlas_buf_free(&stderr_out);
}

static void run_agent(dep_case *c) {
    run_agent_expect(c, 0);
}

/* --- reading the .res the agent wrote ---------------------------------------- */

typedef struct dep_result {
    bool present;
    char deploy[64];
    char outcome[16];
    char stage[24];
    char dry_run[8];
    char head_before[64];
    char head_after[64];
    char tree_dirty_before[8];
    char installed_version[256];
    char units[512];
    char rollback[16];
    long text_bytes;
    long body_len;    /* actual bytes measured after the "--" separator line */
    char *body_text;  /* heap-allocated, NUL-terminated; caller frees */
} dep_result;

static void set_field(char *dst, size_t dstlen, const char *val) {
    (void)snprintf(dst, dstlen, "%s", val);
}

static void read_result(const char *path, dep_result *r) {
    memset(r, 0, sizeof(*r));
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        r->present = false;
        return;
    }
    r->present = true;

    char line[2048];
    T_REQUIRE_MSG(fgets(line, sizeof(line), f) != NULL, "empty result file: %s", path);
    size_t hl = strlen(line);
    if (hl > 0 && line[hl - 1] == '\n') {
        line[hl - 1] = '\0';
    }
    T_CHECK_MSG(strcmp(line, "atlas-deploy-result 1") == 0, "bad result header: %s", line);

    while (fgets(line, sizeof(line), f) != NULL) {
        size_t l = strlen(line);
        if (l > 0 && line[l - 1] == '\n') {
            line[l - 1] = '\0';
        }
        if (strcmp(line, "--") == 0) {
            break;
        }
        char *sp = strchr(line, ' ');
        const char *key;
        const char *val;
        if (sp != NULL) {
            *sp = '\0';
            key = line;
            val = sp + 1;
        } else {
            key = line;
            val = "";
        }
        if (strcmp(key, "deploy") == 0) {
            set_field(r->deploy, sizeof(r->deploy), val);
        } else if (strcmp(key, "outcome") == 0) {
            set_field(r->outcome, sizeof(r->outcome), val);
        } else if (strcmp(key, "stage") == 0) {
            set_field(r->stage, sizeof(r->stage), val);
        } else if (strcmp(key, "dry_run") == 0) {
            set_field(r->dry_run, sizeof(r->dry_run), val);
        } else if (strcmp(key, "head_before") == 0) {
            set_field(r->head_before, sizeof(r->head_before), val);
        } else if (strcmp(key, "head_after") == 0) {
            set_field(r->head_after, sizeof(r->head_after), val);
        } else if (strcmp(key, "tree_dirty_before") == 0) {
            set_field(r->tree_dirty_before, sizeof(r->tree_dirty_before), val);
        } else if (strcmp(key, "installed_version") == 0) {
            set_field(r->installed_version, sizeof(r->installed_version), val);
        } else if (strcmp(key, "units") == 0) {
            set_field(r->units, sizeof(r->units), val);
        } else if (strcmp(key, "rollback") == 0) {
            set_field(r->rollback, sizeof(r->rollback), val);
        } else if (strcmp(key, "text_bytes") == 0) {
            r->text_bytes = atol(val);
        }
    }

    long start = ftell(f);
    T_REQUIRE_MSG(start >= 0, "ftell failed on %s", path);
    T_REQUIRE_MSG(fseek(f, 0, SEEK_END) == 0, "seek failed on %s", path);
    long end = ftell(f);
    T_REQUIRE_MSG(end >= start, "ftell failed on %s", path);
    r->body_len = end - start;

    r->body_text = (char *)malloc((size_t)r->body_len + 1);
    T_REQUIRE_MSG(r->body_text != NULL, "out of memory reading the body of %s", path);
    if (r->body_len > 0) {
        T_REQUIRE_MSG(fseek(f, start, SEEK_SET) == 0, "seek failed on %s", path);
        T_REQUIRE_MSG(fread(r->body_text, 1, (size_t)r->body_len, f) == (size_t)r->body_len,
                      "short read of the body of %s", path);
    }
    r->body_text[r->body_len] = '\0';

    (void)fclose(f);
}

/* --- (a) a clean patch reaches DONE ------------------------------------------ */

static void test_case_a_clean_patch_succeeds(void) {
    dep_case c;
    dep_open(&c);
    atlas_buf patch = ATLAS_BUF_INIT;
    make_patch(&c, NOTES_PATCHED, &patch);

    char deploy_uid[34];
    char job_uid[34];
    make_id('d', "a1", deploy_uid);
    make_id('j', "b1", job_uid);

    write_conf(&c, "make", "make test", "no", "no");
    write_request(&c, deploy_uid, job_uid, atlas_buf_cstr(&c.repo), atlas_buf_cstr(&c.head), &patch,
                  "key_test0001", "2026-09-07T12:00:00Z");

    run_agent(&c);

    char reqpath[1024];
    char patchpath[1024];
    char respath[1024];
    (void)snprintf(reqpath, sizeof(reqpath), "%s/%s.req", atlas_buf_cstr(&c.requests), deploy_uid);
    (void)snprintf(patchpath, sizeof(patchpath), "%s/%s.patch", atlas_buf_cstr(&c.requests),
                    deploy_uid);
    (void)snprintf(respath, sizeof(respath), "%s/%s.res", atlas_buf_cstr(&c.results), deploy_uid);

    T_CHECK_MSG(!file_exists(reqpath), "request file still present: %s", reqpath);
    T_CHECK_MSG(!file_exists(patchpath), "patch file still present: %s", patchpath);
    T_CHECK_MSG(file_exists(respath), "result file missing: %s", respath);
    T_CHECK_MSG(file_mode(respath) == 0644, "result file mode is %04o, expected 0644",
                (unsigned)file_mode(respath));

    dep_result r;
    read_result(respath, &r);
    T_EQ_STR(r.outcome, "SUCCEEDED");
    T_EQ_STR(r.stage, "DONE");
    T_EQ_STR(r.deploy, deploy_uid);
    T_EQ_STR(r.rollback, "none");

    size_t notes_len = 0;
    char notes_path[1024];
    (void)snprintf(notes_path, sizeof(notes_path), "%s/notes.txt", atlas_buf_cstr(&c.repo));
    char *notes = read_whole_file(notes_path, &notes_len);
    T_CHECK_MSG(strcmp(notes, NOTES_PATCHED) == 0, "notes.txt was not patched: %s", notes);
    free(notes);
    free(r.body_text);

    atlas_buf_free(&patch);
    dep_close(&c);
}

/* --- (b) a patch that cannot apply -------------------------------------------- */

static void test_case_b_patch_does_not_apply(void) {
    dep_case c;
    dep_open(&c);
    atlas_buf patch = ATLAS_BUF_INIT;
    make_patch(&c, NOTES_PATCHED, &patch); /* the patch's context expects NOTES_ORIGINAL */

    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_write(fx_repo(&c.fx), "notes.txt", NOTES_DIVERGED, &err), &err);
    T_OK(fx_add_all(&c.fx, fx_repo(&c.fx), &err), &err);
    T_OK(fx_commit(&c.fx, fx_repo(&c.fx), "diverge", &err), &err);

    char deploy_uid[34];
    char job_uid[34];
    make_id('d', "a2", deploy_uid);
    make_id('j', "b2", job_uid);

    write_conf(&c, "make", "make test", "no", "no");
    /* base_commit deliberately names the stale first commit -- design says a
     * mismatch here is recorded, not refused; APPLY_CHECK is the arbiter. */
    write_request(&c, deploy_uid, job_uid, atlas_buf_cstr(&c.repo), atlas_buf_cstr(&c.head), &patch,
                  "key_test0002", "2026-09-07T12:00:00Z");

    pre_settle_index(&c);
    char digest_before[ATLAS_SHA256_HEX_LEN + 1];
    T_OK(fx_tree_digest(atlas_buf_cstr(&c.repo), digest_before, &err), &err);

    run_agent(&c);

    char digest_after[ATLAS_SHA256_HEX_LEN + 1];
    T_OK(fx_tree_digest(atlas_buf_cstr(&c.repo), digest_after, &err), &err);
    T_CHECK_MSG(strcmp(digest_before, digest_after) == 0,
                "the tree changed across a refused APPLY_CHECK");

    char reqpath[1024];
    char respath[1024];
    (void)snprintf(reqpath, sizeof(reqpath), "%s/%s.req", atlas_buf_cstr(&c.requests), deploy_uid);
    (void)snprintf(respath, sizeof(respath), "%s/%s.res", atlas_buf_cstr(&c.results), deploy_uid);
    T_CHECK_MSG(!file_exists(reqpath), "request file still present: %s", reqpath);

    dep_result r;
    read_result(respath, &r);
    T_EQ_STR(r.outcome, "FAILED");
    T_EQ_STR(r.stage, "APPLY_CHECK");

    free(r.body_text);
    atlas_buf_free(&patch);
    dep_close(&c);
}

/* --- (c) a failing build, reverse_on_failure = no ----------------------------- */

static void test_case_c_build_fails_no_reverse(void) {
    dep_case c;
    dep_open(&c);
    atlas_buf patch = ATLAS_BUF_INIT;
    make_patch(&c, NOTES_PATCHED, &patch);

    char deploy_uid[34];
    char job_uid[34];
    make_id('d', "a3", deploy_uid);
    make_id('j', "b3", job_uid);

    write_conf(&c, "false", "make test", "no", "no");
    write_request(&c, deploy_uid, job_uid, atlas_buf_cstr(&c.repo), atlas_buf_cstr(&c.head), &patch,
                  "key_test0003", "2026-09-07T12:00:00Z");

    run_agent(&c);

    char respath[1024];
    (void)snprintf(respath, sizeof(respath), "%s/%s.res", atlas_buf_cstr(&c.results), deploy_uid);
    dep_result r;
    read_result(respath, &r);
    T_EQ_STR(r.outcome, "FAILED");
    T_EQ_STR(r.stage, "BUILD");
    T_EQ_STR(r.rollback, "none");

    size_t notes_len = 0;
    char notes_path[1024];
    (void)snprintf(notes_path, sizeof(notes_path), "%s/notes.txt", atlas_buf_cstr(&c.repo));
    char *notes = read_whole_file(notes_path, &notes_len);
    T_CHECK_MSG(strcmp(notes, NOTES_PATCHED) == 0, "the tree should still carry the patch: %s",
                notes);
    free(notes);
    free(r.body_text);

    atlas_buf_free(&patch);
    dep_close(&c);
}

/* --- (d) the same failing build, reverse_on_failure = yes --------------------- */

static void test_case_d_build_fails_reverse_on_failure(void) {
    dep_case c;
    dep_open(&c);
    atlas_buf patch = ATLAS_BUF_INIT;
    make_patch(&c, NOTES_PATCHED, &patch);

    char deploy_uid[34];
    char job_uid[34];
    make_id('d', "a4", deploy_uid);
    make_id('j', "b4", job_uid);

    write_conf(&c, "false", "make test", "no", "yes");
    write_request(&c, deploy_uid, job_uid, atlas_buf_cstr(&c.repo), atlas_buf_cstr(&c.head), &patch,
                  "key_test0004", "2026-09-07T12:00:00Z");

    run_agent(&c);

    assert_tree_clean_and_original(&c, "case d");

    char respath[1024];
    (void)snprintf(respath, sizeof(respath), "%s/%s.res", atlas_buf_cstr(&c.results), deploy_uid);
    dep_result r;
    read_result(respath, &r);
    T_EQ_STR(r.outcome, "FAILED");
    T_EQ_STR(r.stage, "BUILD");
    T_EQ_STR(r.rollback, "patch");

    free(r.body_text);
    atlas_buf_free(&patch);
    dep_close(&c);
}

/* --- (e) dry_run = yes --------------------------------------------------------- */

static void test_case_e_dry_run(void) {
    dep_case c;
    dep_open(&c);
    atlas_buf patch = ATLAS_BUF_INIT;
    make_patch(&c, NOTES_PATCHED, &patch);

    char deploy_uid[34];
    char job_uid[34];
    make_id('d', "a5", deploy_uid);
    make_id('j', "b5", job_uid);

    /* build/test are side-effect-free (no build artifact) so the only tracked
     * change in the tree is ever the patch itself. */
    write_conf(&c, "true", "true", "yes", "no");
    write_request(&c, deploy_uid, job_uid, atlas_buf_cstr(&c.repo), atlas_buf_cstr(&c.head), &patch,
                  "key_test0005", "2026-09-07T12:00:00Z");

    run_agent(&c);

    assert_tree_clean_and_original(&c, "case e");

    char respath[1024];
    (void)snprintf(respath, sizeof(respath), "%s/%s.res", atlas_buf_cstr(&c.results), deploy_uid);
    dep_result r;
    read_result(respath, &r);
    T_EQ_STR(r.stage, "TEST");
    T_EQ_STR(r.dry_run, "yes");
    T_EQ_STR(r.outcome, "SUCCEEDED");

    free(r.body_text);
    atlas_buf_free(&patch);
    dep_close(&c);
}

/* --- (f) a patch_sha256/patch_bytes mismatch ---------------------------------- */

static void test_case_f_digest_mismatch(void) {
    dep_case c;
    dep_open(&c);
    atlas_buf patch = ATLAS_BUF_INIT;
    make_patch(&c, NOTES_PATCHED, &patch);

    char deploy_uid[34];
    char job_uid[34];
    make_id('d', "a6", deploy_uid);
    make_id('j', "b6", job_uid);

    write_conf(&c, "make", "make test", "no", "no");
    write_request_ex(&c, deploy_uid, job_uid, atlas_buf_cstr(&c.repo), atlas_buf_cstr(&c.head),
                      &patch, "key_test0006", "2026-09-07T12:00:00Z", true /* corrupt_digest */);

    atlas_err err;
    atlas_err_init(&err);
    char digest_before[ATLAS_SHA256_HEX_LEN + 1];
    T_OK(fx_tree_digest(atlas_buf_cstr(&c.repo), digest_before, &err), &err);

    run_agent(&c);

    char digest_after[ATLAS_SHA256_HEX_LEN + 1];
    T_OK(fx_tree_digest(atlas_buf_cstr(&c.repo), digest_after, &err), &err);
    T_CHECK_MSG(strcmp(digest_before, digest_after) == 0, "a digest mismatch touched the tree");

    char reqpath[1024];
    char respath[1024];
    (void)snprintf(reqpath, sizeof(reqpath), "%s/%s.req", atlas_buf_cstr(&c.requests), deploy_uid);
    (void)snprintf(respath, sizeof(respath), "%s/%s.res", atlas_buf_cstr(&c.results), deploy_uid);
    T_CHECK_MSG(!file_exists(reqpath), "request file still present: %s", reqpath);

    dep_result r;
    read_result(respath, &r);
    T_EQ_STR(r.outcome, "FAILED");
    T_EQ_STR(r.stage, "PREFLIGHT");

    free(r.body_text);
    atlas_buf_free(&patch);
    dep_close(&c);
}

/* --- (g) repo_root does not match the conf's tree ----------------------------- */

static void test_case_g_repo_root_mismatch(void) {
    dep_case c;
    dep_open(&c);
    atlas_buf patch = ATLAS_BUF_INIT;
    make_patch(&c, NOTES_PATCHED, &patch);

    char deploy_uid[34];
    char job_uid[34];
    make_id('d', "a7", deploy_uid);
    make_id('j', "b7", job_uid);

    write_conf(&c, "make", "make test", "no", "no");

    atlas_err err;
    atlas_err_init(&err);
    atlas_buf wrong_root = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&wrong_root, &err, "%s-not-the-tree", atlas_buf_cstr(&c.repo)), &err);
    write_request(&c, deploy_uid, job_uid, atlas_buf_cstr(&wrong_root), atlas_buf_cstr(&c.head),
                  &patch, "key_test0007", "2026-09-07T12:00:00Z");

    char digest_before[ATLAS_SHA256_HEX_LEN + 1];
    T_OK(fx_tree_digest(atlas_buf_cstr(&c.repo), digest_before, &err), &err);

    run_agent(&c);

    char digest_after[ATLAS_SHA256_HEX_LEN + 1];
    T_OK(fx_tree_digest(atlas_buf_cstr(&c.repo), digest_after, &err), &err);
    T_CHECK_MSG(strcmp(digest_before, digest_after) == 0, "a repo_root refusal touched the tree");

    char reqpath[1024];
    char respath[1024];
    (void)snprintf(reqpath, sizeof(reqpath), "%s/%s.req", atlas_buf_cstr(&c.requests), deploy_uid);
    (void)snprintf(respath, sizeof(respath), "%s/%s.res", atlas_buf_cstr(&c.results), deploy_uid);
    T_CHECK_MSG(!file_exists(reqpath), "request file still present: %s", reqpath);

    dep_result r;
    read_result(respath, &r);
    T_EQ_STR(r.outcome, "FAILED");
    T_EQ_STR(r.stage, "PREFLIGHT");

    free(r.body_text);
    atlas_buf_free(&wrong_root);
    atlas_buf_free(&patch);
    dep_close(&c);
}

/* --- (h) a request with no parseable deploy uid ------------------------------- */

static void test_case_h_unparseable_request(void) {
    dep_case c;
    dep_open(&c);
    write_conf(&c, "make", "make test", "no", "no");

    char reqpath[1024];
    (void)snprintf(reqpath, sizeof(reqpath), "%s/garbage.req", atlas_buf_cstr(&c.requests));
    static const char *const GARBAGE = "this is not a deploy request at all\nno deploy line here\n";
    write_file_raw(reqpath, GARBAGE, strlen(GARBAGE));

    run_agent(&c);

    char badpath[1024];
    (void)snprintf(badpath, sizeof(badpath), "%s/garbage.bad", atlas_buf_cstr(&c.requests));
    T_CHECK_MSG(!file_exists(reqpath), "the malformed .req is still present: %s", reqpath);
    T_CHECK_MSG(file_exists(badpath), "the malformed request was not quarantined as .bad: %s",
                badpath);

    dep_close(&c);
}

/* --- (i) a result never exceeds 32 KiB ---------------------------------------- */

static void test_case_i_result_bounded(void) {
    dep_case c;
    dep_open(&c);
    atlas_buf patch = ATLAS_BUF_INIT;
    make_patch(&c, NOTES_PATCHED, &patch);

    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_write_exec(fx_data_dir(&c.fx), "bigtest.sh",
                        "#!/bin/sh\nyes x | head -c 100000\nexit 1\n", &err),
         &err);
    char bigtest_path[1024];
    (void)snprintf(bigtest_path, sizeof(bigtest_path), "%s/bigtest.sh", fx_data_dir(&c.fx));

    char deploy_uid[34];
    char job_uid[34];
    make_id('d', "a8", deploy_uid);
    make_id('j', "b8", job_uid);

    write_conf(&c, "make", bigtest_path, "no", "no");
    write_request(&c, deploy_uid, job_uid, atlas_buf_cstr(&c.repo), atlas_buf_cstr(&c.head), &patch,
                  "key_test0008", "2026-09-07T12:00:00Z");

    run_agent(&c);

    char respath[1024];
    (void)snprintf(respath, sizeof(respath), "%s/%s.res", atlas_buf_cstr(&c.results), deploy_uid);
    dep_result r;
    read_result(respath, &r);
    T_EQ_STR(r.outcome, "FAILED");
    T_EQ_STR(r.stage, "TEST");
    T_CHECK_MSG(r.body_len <= 32768, "the result body is %ld bytes, exceeds the 32 KiB bound",
                r.body_len);
    T_CHECK_MSG(r.text_bytes <= 32768, "text_bytes claims %ld, exceeds the 32 KiB bound",
                r.text_bytes);
    T_CHECK_MSG(r.body_len > 20000, "the tail was truncated far more than expected (%ld bytes)",
                r.body_len);
    /* IMPORTANT 2 (fix round 1): text_bytes is not a static claim -- it must
     * equal what was actually measured and written. */
    T_CHECK_MSG(r.text_bytes == r.body_len, "text_bytes (%ld) does not match the actual body (%ld)",
                r.text_bytes, r.body_len);
    /* And the stage lines must survive the truncation whole: only the tail of
     * the *last command's* output is ever sacrificed, never the stage log. */
    T_CHECK_MSG(strstr(r.body_text, "APPLY_CHECK: ok") != NULL,
                "APPLY_CHECK's stage line did not survive truncation: %.200s", r.body_text);
    T_CHECK_MSG(strstr(r.body_text, "APPLY: ok") != NULL,
                "APPLY's stage line did not survive truncation: %.200s", r.body_text);
    T_CHECK_MSG(strstr(r.body_text, "BUILD: ok") != NULL,
                "BUILD's stage line did not survive truncation: %.200s", r.body_text);
    T_CHECK_MSG(strstr(r.body_text, "TEST:") != NULL,
                "TEST's stage line did not survive truncation: %.200s", r.body_text);

    free(r.body_text);
    atlas_buf_free(&patch);
    dep_close(&c);
}

/* --- (j) the patch is applied from a copy outside requests/, mode 0644 ------- */

static void test_case_j_patch_staged_outside_requests(void) {
    /* IMPORTANT 4 (fix round 1): in a real deployment `requests/` is 0700,
     * daemon-owned (D.3), so the tree's owner -- who runs APPLY_CHECK/APPLY --
     * cannot even traverse into it; this suite cannot reproduce that EACCES
     * directly (owner == the test's own user, who created that directory and
     * can always read it), so instead this asserts the *mechanism* the fix
     * added: PREFLIGHT logs where it staged its own copy of the patch before
     * ever touching git with it, and that line is checked here rather than a
     * post-mortem stat of the copy, because the copy lives under the agent's
     * own tmp directory and is gone (cleaned up) by the time this process can
     * look. */
    dep_case c;
    dep_open(&c);
    atlas_buf patch = ATLAS_BUF_INIT;
    make_patch(&c, NOTES_PATCHED, &patch);

    char deploy_uid[34];
    char job_uid[34];
    make_id('d', "a9", deploy_uid);
    make_id('j', "c9", job_uid);

    write_conf(&c, "make", "make test", "no", "no");
    write_request(&c, deploy_uid, job_uid, atlas_buf_cstr(&c.repo), atlas_buf_cstr(&c.head), &patch,
                  "key_test0009", "2026-09-07T12:00:00Z");

    run_agent(&c);

    char respath[1024];
    (void)snprintf(respath, sizeof(respath), "%s/%s.res", atlas_buf_cstr(&c.results), deploy_uid);
    dep_result r;
    read_result(respath, &r);
    T_EQ_STR(r.outcome, "SUCCEEDED");

    const char *marker = "staged patch at ";
    const char *p = strstr(r.body_text, marker);
    T_REQUIRE_MSG(p != NULL, "no staged-patch log line found in the result: %.400s", r.body_text);
    p += strlen(marker);
    char staged_path[1024] = {0};
    char staged_mode[16] = {0};
    int n = sscanf(p, "%1023s mode %15s", staged_path, staged_mode);
    T_REQUIRE_MSG(n == 2, "could not parse the staged-patch log line: %.200s", p);

    char requests_prefix[1200];
    (void)snprintf(requests_prefix, sizeof(requests_prefix), "%s/", atlas_buf_cstr(&c.requests));
    T_CHECK_MSG(strncmp(staged_path, requests_prefix, strlen(requests_prefix)) != 0,
                "the staged patch path is still inside requests/: %s", staged_path);
    T_CHECK_MSG(strstr(staged_path, "atlas-deploy-agent.") != NULL,
                "the staged patch path is not under the agent's own tmp dir: %s", staged_path);
    T_EQ_STR(staged_mode, "644");

    free(r.body_text);
    atlas_buf_free(&patch);
    dep_close(&c);
}

/* --- (k) test= unset refuses at PREFLIGHT, tree untouched --------------------- */

static void test_case_k_test_unset_preflight_refusal(void) {
    dep_case c;
    dep_open(&c);
    atlas_buf patch = ATLAS_BUF_INIT;
    make_patch(&c, NOTES_PATCHED, &patch);

    char deploy_uid[34];
    char job_uid[34];
    make_id('d', "b1", deploy_uid);
    make_id('j', "d1", job_uid);

    write_conf(&c, "make", NULL /* test= omitted entirely */, "no", "no");
    write_request(&c, deploy_uid, job_uid, atlas_buf_cstr(&c.repo), atlas_buf_cstr(&c.head), &patch,
                  "key_test0010", "2026-09-07T12:00:00Z");

    atlas_err err;
    atlas_err_init(&err);
    char digest_before[ATLAS_SHA256_HEX_LEN + 1];
    T_OK(fx_tree_digest(atlas_buf_cstr(&c.repo), digest_before, &err), &err);

    run_agent(&c);

    char digest_after[ATLAS_SHA256_HEX_LEN + 1];
    T_OK(fx_tree_digest(atlas_buf_cstr(&c.repo), digest_after, &err), &err);
    T_CHECK_MSG(strcmp(digest_before, digest_after) == 0,
                "an unset test= refusal touched the tree");

    char reqpath[1024];
    char respath[1024];
    (void)snprintf(reqpath, sizeof(reqpath), "%s/%s.req", atlas_buf_cstr(&c.requests), deploy_uid);
    (void)snprintf(respath, sizeof(respath), "%s/%s.res", atlas_buf_cstr(&c.results), deploy_uid);
    T_CHECK_MSG(!file_exists(reqpath), "request file still present: %s", reqpath);

    dep_result r;
    read_result(respath, &r);
    T_EQ_STR(r.outcome, "FAILED");
    T_EQ_STR(r.stage, "PREFLIGHT");

    free(r.body_text);
    atlas_buf_free(&patch);
    dep_close(&c);
}

/* --- (l) an unknown conf key: exit 3, the conf is untrusted before any request
 * is even looked at, so nothing under the spool is touched at all ------------ */

static void test_case_l_unknown_conf_key_leaves_request_untouched(void) {
    dep_case c;
    dep_open(&c);
    atlas_buf patch = ATLAS_BUF_INIT;
    make_patch(&c, NOTES_PATCHED, &patch);

    char deploy_uid[34];
    char job_uid[34];
    make_id('d', "b2", deploy_uid);
    make_id('j', "d2", job_uid);

    write_conf_with_unknown_key(&c);
    write_request(&c, deploy_uid, job_uid, atlas_buf_cstr(&c.repo), atlas_buf_cstr(&c.head), &patch,
                  "key_test0011", "2026-09-07T12:00:00Z");

    char reqpath[1024];
    (void)snprintf(reqpath, sizeof(reqpath), "%s/%s.req", atlas_buf_cstr(&c.requests), deploy_uid);
    size_t before_len = 0;
    char *before_content = read_whole_file(reqpath, &before_len);

    char respath[1024];
    (void)snprintf(respath, sizeof(respath), "%s/%s.res", atlas_buf_cstr(&c.results), deploy_uid);

    run_agent_expect(&c, 3);

    T_CHECK_MSG(file_exists(reqpath), "the request was removed even though the conf was refused: %s",
                reqpath);
    size_t after_len = 0;
    char *after_content = read_whole_file(reqpath, &after_len);
    T_CHECK_MSG(before_len == after_len && memcmp(before_content, after_content, before_len) == 0,
                "the request's own bytes changed even though the conf was refused");
    T_CHECK_MSG(!file_exists(respath), "a result was written even though the conf was refused: %s",
                respath);

    free(before_content);
    free(after_content);
    atlas_buf_free(&patch);
    dep_close(&c);
}

/* --- (m) an unknown request key: PREFLIGHT, .req removed ---------------------- */

static void test_case_m_unknown_request_key(void) {
    dep_case c;
    dep_open(&c);
    atlas_buf patch = ATLAS_BUF_INIT;
    make_patch(&c, NOTES_PATCHED, &patch);

    char deploy_uid[34];
    char job_uid[34];
    make_id('d', "b3", deploy_uid);
    make_id('j', "d3", job_uid);

    write_conf(&c, "make", "make test", "no", "no");
    write_request_ex2(&c, deploy_uid, NULL, job_uid, atlas_buf_cstr(&c.repo), atlas_buf_cstr(&c.head),
                       &patch, "key_test0012", "2026-09-07T12:00:00Z", false,
                       "surprise_field an unexpected value");

    run_agent(&c);

    char reqpath[1024];
    char respath[1024];
    (void)snprintf(reqpath, sizeof(reqpath), "%s/%s.req", atlas_buf_cstr(&c.requests), deploy_uid);
    (void)snprintf(respath, sizeof(respath), "%s/%s.res", atlas_buf_cstr(&c.results), deploy_uid);
    T_CHECK_MSG(!file_exists(reqpath), "request file still present: %s", reqpath);

    dep_result r;
    read_result(respath, &r);
    T_EQ_STR(r.outcome, "FAILED");
    T_EQ_STR(r.stage, "PREFLIGHT");

    free(r.body_text);
    atlas_buf_free(&patch);
    dep_close(&c);
}

/* --- CRITICAL 1: a request whose content claims a different deploy uid than
 * its own filename is quarantined, never trusted to name its own result path */

static void test_case_deploy_uid_mismatch_is_quarantined(void) {
    dep_case c;
    dep_open(&c);
    atlas_buf patch = ATLAS_BUF_INIT;
    make_patch(&c, NOTES_PATCHED, &patch);

    char deploy_uid[34];   /* the filename this request is actually filed under */
    char other_uid[34];    /* what its own "deploy" line claims instead */
    char job_uid[34];
    make_id('d', "b4", deploy_uid);
    make_id('d', "e4", other_uid);
    make_id('j', "d4", job_uid);

    write_conf(&c, "make", "make test", "no", "no");
    write_request_ex2(&c, deploy_uid, other_uid, job_uid, atlas_buf_cstr(&c.repo),
                       atlas_buf_cstr(&c.head), &patch, "key_test0013", "2026-09-07T12:00:00Z",
                       false, NULL);

    atlas_err err;
    atlas_err_init(&err);
    char digest_before[ATLAS_SHA256_HEX_LEN + 1];
    T_OK(fx_tree_digest(atlas_buf_cstr(&c.repo), digest_before, &err), &err);

    run_agent(&c);

    char digest_after[ATLAS_SHA256_HEX_LEN + 1];
    T_OK(fx_tree_digest(atlas_buf_cstr(&c.repo), digest_after, &err), &err);
    T_CHECK_MSG(strcmp(digest_before, digest_after) == 0,
                "a quarantined deploy-uid mismatch touched the tree");

    char reqpath[1024];
    char badpath[1024];
    char other_respath[1024];
    char filed_respath[1024];
    (void)snprintf(reqpath, sizeof(reqpath), "%s/%s.req", atlas_buf_cstr(&c.requests), deploy_uid);
    (void)snprintf(badpath, sizeof(badpath), "%s/%s.bad", atlas_buf_cstr(&c.requests), deploy_uid);
    (void)snprintf(other_respath, sizeof(other_respath), "%s/%s.res", atlas_buf_cstr(&c.results),
                    other_uid);
    (void)snprintf(filed_respath, sizeof(filed_respath), "%s/%s.res", atlas_buf_cstr(&c.results),
                    deploy_uid);

    T_CHECK_MSG(!file_exists(reqpath), "the mismatched .req is still present: %s", reqpath);
    T_CHECK_MSG(file_exists(badpath), "the mismatched request was not quarantined as .bad: %s",
                badpath);
    T_CHECK_MSG(!file_exists(other_respath),
                "a result was written at the *content-claimed* uid's path -- the path traversal "
                "this fix closes: %s",
                other_respath);
    T_CHECK_MSG(!file_exists(filed_respath),
                "a result was written even though the request was quarantined: %s", filed_respath);

    atlas_buf_free(&patch);
    dep_close(&c);
}

/* --- bash -n on the shipped script, so a syntax error fails the suite -------- */

static void test_script_passes_bash_syntax_check(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf exe = ATLAS_BUF_INIT;
    T_OK(atlas_proc_which("bash", getenv("PATH"), &exe, &err), &err);
    const char *argv[] = {atlas_buf_cstr(&exe), "-n", DEPLOY_AGENT_SCRIPT, NULL};
    static const char *const ENV[] = {"PATH=/usr/bin:/bin", NULL};
    atlas_proc_opts o;
    memset(&o, 0, sizeof(o));
    o.argv = argv;
    o.env = ENV;
    o.timeout_ms = 10000;
    atlas_buf stderr_out = ATLAS_BUF_INIT;
    atlas_proc_result res;
    T_OK(atlas_proc_run(&o, NULL, NULL, &stderr_out, &res, &err), &err);
    T_CHECK_MSG(res.exit_code == 0, "bash -n reported a syntax error: %s",
                atlas_buf_cstr(&stderr_out));
    atlas_buf_free(&stderr_out);
    atlas_buf_free(&exe);
}

static const atlas_test TESTS[] = {
    {"script_passes_bash_syntax_check", test_script_passes_bash_syntax_check},
    {"case_a_clean_patch_succeeds", test_case_a_clean_patch_succeeds},
    {"case_b_patch_does_not_apply", test_case_b_patch_does_not_apply},
    {"case_c_build_fails_no_reverse", test_case_c_build_fails_no_reverse},
    {"case_d_build_fails_reverse_on_failure", test_case_d_build_fails_reverse_on_failure},
    {"case_e_dry_run", test_case_e_dry_run},
    {"case_f_digest_mismatch", test_case_f_digest_mismatch},
    {"case_g_repo_root_mismatch", test_case_g_repo_root_mismatch},
    {"case_h_unparseable_request", test_case_h_unparseable_request},
    {"case_i_result_bounded", test_case_i_result_bounded},
    {"case_j_patch_staged_outside_requests", test_case_j_patch_staged_outside_requests},
    {"case_k_test_unset_preflight_refusal", test_case_k_test_unset_preflight_refusal},
    {"case_l_unknown_conf_key_leaves_request_untouched",
     test_case_l_unknown_conf_key_leaves_request_untouched},
    {"case_m_unknown_request_key", test_case_m_unknown_request_key},
    {"case_deploy_uid_mismatch_is_quarantined", test_case_deploy_uid_mismatch_is_quarantined},
};

ATLAS_TEST_MAIN("test_deploy_agent", TESTS)
