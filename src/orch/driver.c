/* Atlas - A8: the drivers, and the environment they are given.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See atlas/driver.h for what a driver may and may not decide.
 *
 * Two drivers ship: a deterministic `fake` that runs entirely in process, and
 * `claude`, which executes the installed Claude Code CLI noninteractively inside
 * a job workspace. The fake one exists so that every part of A8 above the driver
 * — leasing, heartbeats, cancellation, retry, artifact collection, completion —
 * can be exercised without a model, a network or a credential, deterministically
 * and in milliseconds.
 */
#define _GNU_SOURCE 1

#include "atlas/driver.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/proc.h"
#include "atlas/rootpath.h"

/* Where an operator may install a *service* credential for the worker.
 *
 * Root-owned and reached through `atlas_rootpath_open`, exactly like every other
 * A7/A7.1/A8 policy file: neither `atlasd` nor `atlas-worker` can create or edit
 * it, so a worker cannot provision its own credential. A compiled-in constant
 * with no environment override, for the reason `ATLAS_ORCHPOLICY_PATH` is one.
 *
 * Its absence is the ordinary state and is not an error until a driver that
 * needs a model is actually asked to run. Atlas never creates this file, never
 * copies a credential into it, and never prints its contents. */
#define ATLAS_CLAUDE_CREDENTIAL_PATH "/etc/atlas/claude.env"

void atlas_driver_res_init(atlas_driver_res *r) {
    memset(r, 0, sizeof(*r));
    r->exit_kind = ATLAS_ORCH_EXIT_UNKNOWN;
    r->exit_code = -1;
    atlas_buf_init(&r->version);
    atlas_buf_init(&r->cost);
}

void atlas_driver_res_free(atlas_driver_res *r) {
    if (r == NULL) {
        return;
    }
    atlas_buf_free(&r->version);
    atlas_buf_free(&r->cost);
}

/* --- shared: capturing a driver's streams --------------------------------- */

typedef struct capture {
    atlas_buf out;
    int64_t max;
    bool truncated;
} capture;

static atlas_status capture_sink(const char *chunk, size_t n, void *ud, atlas_err *err) {
    capture *c = (capture *)ud;
    if ((int64_t)(c->out.len + n) > c->max) {
        /* Refused rather than trimmed: the runner terminates the child, and the
         * attempt is reported as having exceeded its output bound rather than
         * as having produced a shorter answer than it did. */
        c->truncated = true;
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "the driver exceeded its output bound");
    }
    return atlas_buf_append(&c->out, chunk, n, err);
}

/* Redacts, then writes a captured stream into the workspace. Every log a driver
 * produces goes through here — there is no path that stores one unredacted. */
static atlas_status store_log(const atlas_ws *ws, const char *rel, const atlas_buf *raw,
                              int64_t *redactions, atlas_err *err) {
    atlas_buf clean = ATLAS_BUF_INIT;
    int64_t hits = 0;
    atlas_status st = atlas_ws_redact(raw->data != NULL ? raw->data : "", raw->len, &clean, &hits,
                                      err);
    if (st == ATLAS_OK) {
        st = atlas_ws_write(ws, rel, clean.data, clean.len, err);
    }
    if (redactions != NULL) {
        *redactions += hits;
    }
    atlas_buf_free(&clean);
    return st;
}

/* --- the fake driver -------------------------------------------------------
 *
 * Runs in process. No subprocess, no network, no clock dependence beyond the
 * bounds it is given, and identical output for identical input — which is what
 * "reproducible" has to mean for a fixture that the rest of the suite trusts.
 *
 * Behaviour is selected by a prefix of the task text. That is a deliberate
 * exception to "task text is never interpreted": this driver *is* the
 * interpretation, it exists only to be driven by tests and smoke jobs, and the
 * real driver reads no such thing. The prefixes are Atlas literals matched
 * exactly, so no other text can select a behaviour by accident.
 */
static bool task_is(const char *task, const char *verb) {
    size_t n = strlen(verb);
    return task != NULL && strncmp(task, verb, n) == 0 &&
           (task[n] == '\0' || task[n] == ' ' || task[n] == ':' || task[n] == '\n');
}

static atlas_status fake_run(const atlas_driver_req *req, atlas_driver_res *res, atlas_err *err) {
    atlas_status st = atlas_buf_set_str(&res->version, "fake/1", err);
    if (st != ATLAS_OK) {
        return st;
    }

    /* Cancellation is honoured the way a real driver honours it: by being asked
     * while it works, not by being signalled. */
    if (req->cancel != NULL && req->cancel(req->cancel_ud)) {
        res->exit_kind = ATLAS_ORCH_EXIT_CANCELLED;
        res->exit_code = -1;
        return ATLAS_OK;
    }

    if (task_is(req->task, "fake:timeout")) {
        res->exit_kind = ATLAS_ORCH_EXIT_TIMEOUT;
        res->exit_code = -1;
        /* A timed-out run still leaves a log, empty though it is: "the driver
         * said nothing" is itself the evidence. */
        atlas_buf empty = ATLAS_BUF_INIT;
        atlas_status ls = store_log(req->ws, "logs/stdout.log", &empty, &res->redactions, err);
        atlas_buf_free(&empty);
        return ls;
    }
    if (task_is(req->task, "fake:cancel")) {
        res->exit_kind = ATLAS_ORCH_EXIT_CANCELLED;
        res->exit_code = -1;
        return ATLAS_OK;
    }
    if (task_is(req->task, "fake:malformed")) {
        /* Exits zero and produces metadata that is not a result document. The
         * whole point: a zero exit is not a success claim Atlas accepts. */
        atlas_buf junk = ATLAS_BUF_INIT;
        st = atlas_buf_set_str(&junk, "this is not a result document\n", err);
        if (st == ATLAS_OK) {
            st = atlas_ws_write(req->ws, "driver/result.json", junk.data, junk.len, err);
        }
        atlas_buf_free(&junk);
        res->exit_kind = ATLAS_ORCH_EXIT_MALFORMED_RESULT;
        res->exit_code = 0;
        return st;
    }

    bool fail = task_is(req->task, "fake:fail");

    /* The ordinary path: touch the work tree and leave an artifact, so the
     * layers above have something real to diff, collect and record. */
    atlas_buf note = ATLAS_BUF_INIT;
    st = atlas_buf_appendf(&note, err, "atlas fake driver\njob %s\nattempt %lld\nmode %s\n",
                           req->job_uid != NULL ? req->job_uid : "", (long long)req->attempt_no,
                           req->mode != NULL ? req->mode : "");
    if (st == ATLAS_OK) {
        st = atlas_ws_write(req->ws, "work/ATLAS_FAKE_DRIVER.txt", note.data, note.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_ws_write(req->ws, "artifacts/report.txt", note.data, note.len, err);
    }
    if (st == ATLAS_OK) {
        atlas_buf log = ATLAS_BUF_INIT;
        atlas_status ls = atlas_buf_appendf(&log, err, "fake driver ran for job %s\n",
                                            req->job_uid != NULL ? req->job_uid : "");
        if (ls == ATLAS_OK) {
            ls = store_log(req->ws, "logs/stdout.log", &log, &res->redactions, err);
        }
        atlas_buf_free(&log);
        st = ls;
    }
    int64_t note_len = (int64_t)note.len;
    atlas_buf_free(&note);
    if (st != ATLAS_OK) {
        return st;
    }
    res->stdout_bytes = note_len;
    res->exit_kind = fail ? ATLAS_ORCH_EXIT_NONZERO : ATLAS_ORCH_EXIT_OK;
    res->exit_code = fail ? 1 : 0;
    return ATLAS_OK;
}

/* --- the Claude Code driver ------------------------------------------------ */

/* Reads a root-installed service credential, if one exists.
 *
 * The value never appears in a log, an error, an artifact, a job specification
 * or the database — it is copied into the child's environment and nowhere else.
 * `*present_out` says whether one was found; the caller reports *that*, never
 * the value. */
static atlas_status read_service_credential(atlas_buf *out, bool *present_out, atlas_err *err) {
    *present_out = false;
    atlas_buf_reset(out);
    atlas_rootpath_result rr = ATLAS_ROOTPATH_UNKNOWN;
    char detail[256];
    int fd = atlas_rootpath_open(ATLAS_CLAUDE_CREDENTIAL_PATH, false, &rr, detail, sizeof(detail));
    if (fd < 0) {
        /* Absent is the ordinary state and is not an error here; the caller
         * turns it into a refusal only when a model driver is actually asked to
         * run. A credential file that exists but is not root-owned is refused
         * for the reason every other Atlas policy file is: one the constrained
         * account can write is one it can choose. */
        return ATLAS_OK;
    }
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1u);
    (void)close(fd);
    if (n <= 0) {
        return ATLAS_OK;
    }
    buf[n] = '\0';
    /* One `KEY=value` line, and only keys Atlas knows how to hand to Claude
     * Code. An unrecognised key is ignored rather than forwarded: the child's
     * environment is an allowlist, not a passthrough. */
    static const char *const KEYS[] = {"ANTHROPIC_API_KEY=", "CLAUDE_CODE_OAUTH_TOKEN="};
    for (char *line = buf, *save = NULL; line != NULL; line = save) {
        save = strchr(line, '\n');
        if (save != NULL) {
            *save++ = '\0';
        }
        while (*line == ' ' || *line == '\t') {
            line++;
        }
        if (*line == '#' || *line == '\0') {
            continue;
        }
        for (size_t k = 0; k < sizeof KEYS / sizeof KEYS[0]; k++) {
            if (strncmp(line, KEYS[k], strlen(KEYS[k])) == 0 && line[strlen(KEYS[k])] != '\0') {
                atlas_status st = atlas_buf_set_str(out, line, err);
                if (st != ATLAS_OK) {
                    /* Cleared rather than left half-set: a partial credential
                     * is still a secret in memory. */
                    atlas_buf_reset(out);
                    return st;
                }
                *present_out = true;
                return ATLAS_OK;
            }
        }
    }
    return ATLAS_OK;
}

static atlas_status claude_run(const atlas_driver_req *req, atlas_driver_res *res,
                               atlas_err *err) {
    atlas_status st = atlas_buf_set_str(&res->version, "claude/1", err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (!req->live_model) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "the orchestration policy does not enable live model execution "
                             "(live_model = off)");
    }

    atlas_buf cred = ATLAS_BUF_INIT;
    bool have_cred = false;
    st = read_service_credential(&cred, &have_cred, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&cred);
        return st;
    }
    if (!have_cred) {
        atlas_buf_free(&cred);
        /* Named precisely, and without hinting that any other credential on the
         * machine could be used instead. An operator's personal session is not
         * a service credential and Atlas must never reach for one. */
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "no worker service credential is installed at %s, so the claude "
                             "driver cannot run",
                             ATLAS_CLAUDE_CREDENTIAL_PATH);
    }

    atlas_buf exe = ATLAS_BUF_INIT;
    st = atlas_proc_which("claude", "/usr/local/bin:/usr/bin:/bin", &exe, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&cred);
        atlas_buf_free(&exe);
        return st;
    }

    /* A private HOME inside the attempt, so the CLI's own state directory is
     * created in the workspace and never in the worker's home — and so nothing
     * a previous job left behind can influence this one. */
    atlas_buf home = ATLAS_BUF_INIT;
    st = atlas_buf_appendf(&home, err, "%s/home", atlas_buf_cstr(&req->ws->driver));
    if (st == ATLAS_OK && mkdir(atlas_buf_cstr(&home), 0700) != 0 && errno != EEXIST) {
        st = atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno,
                                 "cannot create the driver home directory");
    }

    /* A constructed environment, never inherited — the rule `src/git` follows,
     * for the same reason. Nothing here can carry an SSH agent, a sudo askpass,
     * an operator's Claude session, a proxy or a locale that changes parsing. */
    atlas_buf home_env = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_buf_appendf(&home_env, err, "HOME=%s", atlas_buf_cstr(&home));
    }
    const char *env[8];
    size_t nenv = 0;
    env[nenv++] = "PATH=/usr/local/bin:/usr/bin:/bin";
    env[nenv++] = "LC_ALL=C";
    env[nenv++] = "LANG=C";
    env[nenv++] = "TZ=UTC";
    env[nenv++] = atlas_buf_cstr(&home_env);
    env[nenv++] = atlas_buf_cstr(&cred);
    env[nenv] = NULL;

    capture out;
    memset(&out, 0, sizeof(out));
    atlas_buf_init(&out.out);
    out.max = req->max_output_bytes > 0 ? req->max_output_bytes : (1024 * 1024);
    atlas_buf errbuf = ATLAS_BUF_INIT;
    atlas_proc_result pr;
    memset(&pr, 0, sizeof(pr));

    if (st == ATLAS_OK) {
        /* Noninteractive, with the working directory set to the job's writable
         * tree. `--print` is Claude Code's documented noninteractive mode.
         *
         * The task text is passed as a single argv element. It is never
         * concatenated into a command line and never reaches a shell, so shell
         * syntax inside it is inert — which is why A8 accepts it. */
        const char *argv[] = {
            atlas_buf_cstr(&exe),
            "--print",
            "--output-format", "json",
            /* The workspace is the boundary, and it is enforced by the OS: the
             * dispatcher runs as `atlas-worker`, whose only writable path is its
             * own runtime root. Permission prompts cannot be answered in a
             * noninteractive run, so they are skipped here and the isolation is
             * left to the account and the service sandbox — never to the
             * model's cooperation. */
            "--permission-mode", "acceptEdits",
            req->task,
            NULL,
        };
        atlas_proc_opts opts;
        memset(&opts, 0, sizeof(opts));
        opts.argv = argv;
        opts.env = env;
        opts.cwd = atlas_buf_cstr(&req->ws->work);
        opts.timeout_ms = (int)(req->wall_timeout_ms > 0 ? req->wall_timeout_ms : 600000);
        opts.idle_timeout_ms = (int)req->idle_timeout_ms;
        opts.max_stdout = (size_t)out.max;
        opts.max_stderr = 256u * 1024u;
        opts.cancel = req->cancel;
        opts.cancel_ud = req->cancel_ud;
        st = atlas_proc_run(&opts, capture_sink, &out, &errbuf, &pr, err);
    }

    /* The credential is dropped as soon as the child has it. */
    atlas_buf_free(&cred);

    /* Streams are stored redacted whatever happened, because the log of a failed
     * run is the only account of it. */
    if (out.out.len > 0 || pr.exit_code != 0) {
        atlas_err ignore;
        atlas_err_init(&ignore);
        (void)store_log(req->ws, "logs/stdout.log", &out.out, &res->redactions, &ignore);
        (void)store_log(req->ws, "logs/stderr.log", &errbuf, &res->redactions, &ignore);
    }
    res->stdout_bytes = (int64_t)out.out.len;
    res->stderr_bytes = (int64_t)errbuf.len;
    res->exit_code = pr.exit_code;

    /* Classification, in order of what actually happened. A zero exit is the
     * *last* thing considered, not the first. */
    if (pr.cancelled) {
        res->exit_kind = ATLAS_ORCH_EXIT_CANCELLED;
    } else if (pr.timed_out || pr.idle_timed_out) {
        res->exit_kind = ATLAS_ORCH_EXIT_TIMEOUT;
    } else if (pr.term_signal != 0) {
        res->exit_kind = ATLAS_ORCH_EXIT_SIGNALLED;
    } else if (st != ATLAS_OK && pr.exit_code < 0) {
        res->exit_kind = ATLAS_ORCH_EXIT_SPAWN_FAILED;
    } else if (pr.exit_code != 0) {
        res->exit_kind = ATLAS_ORCH_EXIT_NONZERO;
    } else {
        /* Exit zero. `--output-format json` was requested, so the run is only a
         * success if what came back is a JSON document. A driver that exits
         * zero and prints prose has produced malformed result metadata, and
         * reading that as success is exactly the mistake this branch exists to
         * refuse.
         *
         * The check is structural — is this a JSON object? — and deliberately
         * shallow: nothing inside the document is read, because a model's
         * output is never parsed as authority. */
        const char *s = atlas_buf_cstr(&out.out);
        while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') {
            s++;
        }
        res->exit_kind = (*s == '{') ? ATLAS_ORCH_EXIT_OK : ATLAS_ORCH_EXIT_MALFORMED_RESULT;
        atlas_err ignore;
        atlas_err_init(&ignore);
        (void)atlas_ws_write(req->ws, "driver/result.json", out.out.data, out.out.len, &ignore);
    }
    /* A cancelled or bounded run is not a failure of Atlas, so the status is
     * cleared and the outcome is carried in `exit_kind` where the caller reads
     * it. A genuine spawn failure keeps its status. */
    if (st != ATLAS_OK && res->exit_kind != ATLAS_ORCH_EXIT_SPAWN_FAILED) {
        atlas_err_init(err);
        st = ATLAS_OK;
    }

    atlas_buf_free(&out.out);
    atlas_buf_free(&errbuf);
    atlas_buf_free(&home_env);
    atlas_buf_free(&home);
    atlas_buf_free(&exe);
    return st;
}

/* --- the registry ----------------------------------------------------------- */

static const atlas_driver DRIVER_FAKE = {"fake", "1", false, fake_run};
static const atlas_driver DRIVER_CLAUDE = {"claude", "1", true, claude_run};

static const atlas_driver *const DRIVERS[] = {&DRIVER_FAKE, &DRIVER_CLAUDE};

const atlas_driver *const *atlas_drivers(size_t *count_out) {
    if (count_out != NULL) {
        *count_out = sizeof DRIVERS / sizeof DRIVERS[0];
    }
    return DRIVERS;
}

const atlas_driver *atlas_driver_find(const char *name) {
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof DRIVERS / sizeof DRIVERS[0]; i++) {
        if (strcmp(DRIVERS[i]->name, name) == 0) {
            return DRIVERS[i];
        }
    }
    /* Unknown is a refusal, never a default. Substituting a driver would run
     * something other than what the job specified. */
    return NULL;
}
