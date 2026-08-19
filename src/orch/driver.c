/* Atlas - A8: the drivers, and the environment they are given.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See atlas/driver.h for what a driver may and may not decide.
 *
 * Four drivers ship, in two pairs.
 *
 * A8's pair works inside an isolated job workspace: a deterministic `fake` that
 * runs entirely in process, and `claude`, which executes the installed Claude
 * Code CLI noninteractively there. The fake one exists so that every part of A8
 * above the driver — leasing, heartbeats, cancellation, retry, artifact
 * collection, completion — can be exercised without a model, a network or a
 * credential, deterministically and in milliseconds.
 *
 * A11.1's pair, `claude-repo` and `fake-repo`, works in the registered
 * repository's own tree. That is the milestone's one reversal and it is scoped
 * to these two entries: `atlas_orch_driver_is_repo_tree` names both, no lease
 * that does not ask for one by name is ever granted one, and the operator's
 * foreground run driver is the only thing in Atlas that asks. `fake-repo`
 * stands in the same relation to `claude-repo` that `fake` does to `claude`,
 * and for the same reason.
 */
#define _GNU_SOURCE 1

#include "atlas/driver.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
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
    atlas_buf_init(&r->log);
}

void atlas_driver_res_free(atlas_driver_res *r) {
    if (r == NULL) {
        return;
    }
    atlas_buf_free(&r->version);
    atlas_buf_free(&r->cost);
    atlas_buf_free(&r->log);
}

/* --- shared: capturing a driver's streams --------------------------------- */

typedef struct capture {
    atlas_buf out;
    int64_t max;
    bool truncated;

    /* A11.5a-R2. The progress reader, riding on the same chunks the sink sees.
     *
     * `--output-format stream-json` emits one JSON object per line, so what
     * arrives here is a byte stream that has to be reassembled into records
     * before any of it means anything. `line` holds the partial tail between
     * chunks and is bounded: a record longer than the cap is abandoned to the
     * next newline and counts as nothing, because an unbounded accumulator is
     * both a memory hole and a way to refresh the idle clock forever without
     * ever completing a record. */
    atlas_buf line;
    bool line_overflow;
    FILE *progress;
    int64_t events;
    int64_t event_bytes;
} capture;

/* The most a single streamed record may be before it is abandoned. Measured
 * against Claude Code 2.1.235: the largest lines are assistant messages
 * carrying a usage block, and those ran well under 4 KiB in the probe this was
 * written from. The cap is far above that and far below anything that could
 * matter, because its job is to bound a hostile or broken stream rather than to
 * fit a real one exactly. */
#define ATLAS_DRIVER_LINE_MAX 65536u
/* Bounds on what one attempt may record, so a chatty worker cannot fill a disk
 * to keep itself alive. Both refuse rather than trim: recording stops and the
 * events already written stay true. */
#define ATLAS_DRIVER_PROGRESS_MAX_EVENTS 5000
#define ATLAS_DRIVER_PROGRESS_MAX_BYTES (1024 * 1024)

/* The top-level record types Claude Code emits on a stream-json run, as
 * observed from the installed 2.1.235 and from nothing else.
 *
 * This is a checked vocabulary, in the A2 sense, not a parser: a record counts
 * as progress when it is a complete line that opens with `{"type":"` naming one
 * of these and closes with `}`. Nothing inside is interpreted, and nothing here
 * can produce authority — a progress record cannot accept a run, pass a gate or
 * move a status, which is why recognising one loosely is safe. A crafted line
 * buys a worker nothing except a refreshed idle clock, and the wall deadline is
 * the bound that actually stops it. */
static const char *const PROGRESS_TYPES[] = {"system", "assistant", "user", "result",
                                             "rate_limit_event", "stream_event"};

/* The tool names worth naming in the progress record. Anything else is recorded
 * as a tool use without a name rather than with one Atlas did not expect. */
static const char *const PROGRESS_TOOLS[] = {"Read", "Edit", "Write", "Bash", "Glob", "Grep",
                                             "Task", "NotebookEdit", "MultiEdit"};

/* Which vocabulary member this line announces itself as, or NULL. */
static const char *progress_type_of(const char *line, size_t len) {
    static const char PFX[] = "{\"type\":\"";
    const size_t plen = sizeof PFX - 1u;
    if (len < plen + 2u || memcmp(line, PFX, plen) != 0) {
        return NULL;
    }
    /* A record is only complete if it closes. A truncated line that happens to
     * start correctly is not evidence of anything. */
    if (line[len - 1u] != '}') {
        return NULL;
    }
    const char *name = line + plen;
    size_t room = len - plen;
    for (size_t i = 0; i < sizeof PROGRESS_TYPES / sizeof PROGRESS_TYPES[0]; i++) {
        size_t tl = strlen(PROGRESS_TYPES[i]);
        if (room > tl && memcmp(name, PROGRESS_TYPES[i], tl) == 0 && name[tl] == '"') {
            return PROGRESS_TYPES[i];
        }
    }
    return NULL;
}

bool atlas_driver_progress_line_is_event(const char *line, size_t len) {
    return line != NULL && progress_type_of(line, len) != NULL;
}

/* The tool a record names, if it is one Atlas expects. Bounded substring search
 * over the line, never an interpretation of the document. */
static const char *progress_tool_of(const char *line, size_t len) {
    static const char NEEDLE[] = "\"name\":\"";
    const size_t nlen = sizeof NEEDLE - 1u;
    if (len < nlen) {
        return NULL;
    }
    for (size_t i = 0; i + nlen < len; i++) {
        if (memcmp(line + i, NEEDLE, nlen) != 0) {
            continue;
        }
        const char *v = line + i + nlen;
        size_t room = len - (i + nlen);
        for (size_t k = 0; k < sizeof PROGRESS_TOOLS / sizeof PROGRESS_TOOLS[0]; k++) {
            size_t tl = strlen(PROGRESS_TOOLS[k]);
            if (room > tl && memcmp(v, PROGRESS_TOOLS[k], tl) == 0 && v[tl] == '"') {
                return PROGRESS_TOOLS[k];
            }
        }
    }
    return NULL;
}

/* One line of the progress log: when, what kind, which tool, how big.
 *
 * Metadata only, deliberately. A tool-use record carries the arguments the model
 * chose — file contents for an edit, a command line for a shell call — and none
 * of that belongs in a durable log Atlas keeps: it is repository content and
 * model output at once, and the rule is that neither is stored beyond what an
 * operator needs to see that work happened. */
static void progress_note(capture *c, const char *line, size_t len) {
    if (c->progress == NULL) {
        return;
    }
    if (c->events >= ATLAS_DRIVER_PROGRESS_MAX_EVENTS ||
        c->event_bytes >= ATLAS_DRIVER_PROGRESS_MAX_BYTES) {
        return;
    }
    const char *kind = progress_type_of(line, len);
    if (kind == NULL) {
        return;
    }
    const char *tool = progress_tool_of(line, len);
    bool tool_use = memmem(line, len, "\"tool_use\"", 10u) != NULL;
    bool tool_result = memmem(line, len, "\"tool_result\"", 13u) != NULL;
    struct timespec ts;
    (void)clock_gettime(CLOCK_REALTIME, &ts);
    int wrote = fprintf(c->progress, "%lld %s%s%s%s%s %zu\n", (long long)ts.tv_sec, kind,
                        tool_use ? " tool_use" : (tool_result ? " tool_result" : ""),
                        tool != NULL ? " " : "", tool != NULL ? tool : "", "", len);
    if (wrote > 0) {
        c->events++;
        c->event_bytes += wrote;
        (void)fflush(c->progress);
    }
}

/* A11.5a-R2. Reassembles lines out of raw chunks and answers the one question
 * `atlas_proc_run` asks: did this chunk contain evidence of work?
 *
 * Returns true as soon as one complete, recognised record lands. Everything
 * else — a partial line, an overlong one, ordinary prose, malformed JSON — is
 * not activity, which is the whole point: a worker that has stopped working
 * cannot keep itself alive by printing. */
static bool progress_activity(const char *chunk, size_t n, void *ud) {
    capture *c = (capture *)ud;
    bool saw = false;
    atlas_err ignore;
    atlas_err_init(&ignore);
    for (size_t i = 0; i < n; i++) {
        if (chunk[i] != '\n') {
            if (c->line.len >= ATLAS_DRIVER_LINE_MAX) {
                c->line_overflow = true;
                atlas_buf_reset(&c->line);
            }
            if (!c->line_overflow) {
                (void)atlas_buf_append(&c->line, &chunk[i], 1u, &ignore);
            }
            continue;
        }
        if (!c->line_overflow && c->line.len > 0) {
            const char *line = c->line.data;
            size_t len = c->line.len;
            /* stream-json writes bare lines, but a stray carriage return must
             * not be what decides a record is incomplete. */
            while (len > 0 && (line[len - 1u] == '\r' || line[len - 1u] == ' ')) {
                len--;
            }
            if (progress_type_of(line, len) != NULL) {
                progress_note(c, line, len);
                saw = true;
            }
        }
        c->line_overflow = false;
        atlas_buf_reset(&c->line);
    }
    return saw;
}

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
 * produces goes through here — there is no path that stores one unredacted.
 *
 * A11.1 added the second destination. When the caller provisioned no workspace
 * — the run driver works in the registered repository's own tree and has none —
 * the redacted bytes are appended to `sink` instead, and the caller carries them
 * as an artifact. Redaction is not the branch: it happens before either
 * destination is chosen, so there is still no path that stores an unredacted
 * one. A driver's log is never written into the repository. */
static atlas_status store_log(const atlas_ws *ws, const char *rel, const atlas_buf *raw,
                              atlas_buf *sink, int64_t *redactions, atlas_err *err) {
    atlas_buf clean = ATLAS_BUF_INIT;
    int64_t hits = 0;
    atlas_status st = atlas_ws_redact(raw->data != NULL ? raw->data : "", raw->len, &clean, &hits,
                                      err);
    if (st == ATLAS_OK) {
        st = ws != NULL ? atlas_ws_write(ws, rel, clean.data, clean.len, err)
                        : atlas_buf_append(sink, clean.data, clean.len, err);
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
        atlas_status ls = store_log(req->ws, "logs/stdout.log", &empty, &res->log, &res->redactions, err);
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
            ls = store_log(req->ws, "logs/stdout.log", &log, &res->log, &res->redactions, err);
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

/* The Claude Code CLI, executed once, noninteractively.
 *
 * One implementation for both drivers that use it. What differs between them is
 * *where the child runs* and *where its log goes*, and both are already
 * parameters: `req->work_dir` and `req->ws`. Two copies of this function would
 * be two places for the environment construction, the credential handling and
 * the exit classification to drift apart, and the exit classification is the
 * part that decides whether a zero exit is read as success. */
static atlas_status claude_exec(const atlas_driver_req *req, atlas_driver_res *res,
                                atlas_err *err) {
    atlas_status st = ATLAS_OK;
    if (!req->live_model) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "the orchestration policy does not enable live model execution "
                             "(live_model = off)");
    }

    atlas_buf cred = ATLAS_BUF_INIT;
    bool have_cred = false;
    if (!req->operator_session) {
        st = read_service_credential(&cred, &have_cred, err);
        if (st != ATLAS_OK) {
            atlas_buf_free(&cred);
            return st;
        }
        if (!have_cred) {
            atlas_buf_free(&cred);
            /* Named precisely, and without hinting that any other credential on
             * the machine could be used instead. An operator's personal session
             * is not a service credential, and Atlas reaches for one only when
             * the root-owned policy has said so explicitly. */
            return atlas_err_set(err, ATLAS_ERR_CONFIG,
                                 "no worker service credential is installed at %s, so the claude "
                                 "driver cannot run",
                                 ATLAS_CLAUDE_CREDENTIAL_PATH);
        }
    }

    atlas_buf exe = ATLAS_BUF_INIT;
    st = atlas_proc_which("claude", "/usr/local/bin:/usr/bin:/bin", &exe, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&cred);
        atlas_buf_free(&exe);
        return st;
    }

    /* Where the CLI keeps its own state.
     *
     * In service mode this is a private directory inside the attempt, so nothing
     * a previous job left behind can influence this one and the worker's home is
     * never touched.
     *
     * In operator-session mode it is the dispatcher's *real* HOME, because that
     * is where the operator's existing login lives and using it is the whole
     * point. Atlas reads nothing there: it sets the variable and executes. */
    atlas_buf home = ATLAS_BUF_INIT;
    if (req->operator_session) {
        const char *h = getenv("HOME");
        if (h == NULL || h[0] != '/') {
            st = atlas_err_set(err, ATLAS_ERR_CONFIG,
                               "the model dispatcher has no HOME, so there is no operator "
                               "session to use");
        } else {
            st = atlas_buf_set_str(&home, h, err);
        }
    } else if (req->ws == NULL) {
        /* A11.1. Service mode keeps the CLI's state in a private directory
         * *inside the attempt's workspace*, and a caller with no workspace has
         * nowhere private to put it. Refused rather than substituted: the
         * substitutes are the real HOME, which is the operator session this
         * caller did not ask for, and the repository, which Atlas will not put
         * a credential store in. */
        st = atlas_err_set(err, ATLAS_ERR_CONFIG,
                           "this driver has no workspace, so it can only run under an operator "
                           "session; the orchestration policy asked for service mode");
    } else {
        st = atlas_buf_appendf(&home, err, "%s/home", atlas_buf_cstr(&req->ws->driver));
        if (st == ATLAS_OK && mkdir(atlas_buf_cstr(&home), 0700) != 0 && errno != EEXIST) {
            st = atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno,
                                     "cannot create the driver home directory");
        }
    }

    /* A constructed environment, never inherited — the rule `src/git` follows,
     * for the same reason. Nothing here can carry an SSH agent, a sudo askpass,
     * an operator's Claude session, a proxy or a locale that changes parsing. */
    atlas_buf home_env = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_buf_appendf(&home_env, err, "HOME=%s", atlas_buf_cstr(&home));
    }
    const char *env[10];
    size_t nenv = 0;
    env[nenv++] = "PATH=/usr/local/bin:/usr/bin:/bin";
    env[nenv++] = "LC_ALL=C";
    env[nenv++] = "LANG=C";
    env[nenv++] = "TZ=UTC";
    env[nenv++] = atlas_buf_cstr(&home_env);
    if (!req->operator_session) {
        env[nenv++] = atlas_buf_cstr(&cred);
    }
    /* `CLAUDECODE` is deliberately absent, and its absence is load-bearing:
     * Claude Code refuses to start when it sees that variable, because a nested
     * session would share runtime state with its parent. The environment is
     * constructed rather than inherited, so it is absent by default — this
     * comment exists so nobody "helpfully" forwards it later. */
    env[nenv] = NULL;

    capture out;
    memset(&out, 0, sizeof(out));
    atlas_buf_init(&out.out);
    atlas_buf_init(&out.line);
    out.max = req->max_output_bytes > 0 ? req->max_output_bytes : (1024 * 1024);

    /* A11.5a-R2. The progress log, opened before the child so that a worker that
     * dies in its first second still leaves a file saying it started.
     *
     * Appended to rather than written and renamed: this is a log that grows
     * while the child runs, and the atomic-rename discipline the *result* spool
     * uses answers a different question — there a reader must never see half a
     * document, here a reader wants whatever has happened so far. O_APPEND plus
     * a flush per record is the durability that fits. */
    atlas_buf ppath = ATLAS_BUF_INIT;
    if (st == ATLAS_OK && req->progress_dir != NULL && req->progress_dir[0] == '/' &&
        req->job_uid != NULL) {
        if (atlas_buf_appendf(&ppath, err, "%s/%s.%lld.progress", req->progress_dir, req->job_uid,
                              (long long)req->attempt_no) == ATLAS_OK) {
            out.progress = fopen(atlas_buf_cstr(&ppath), "ae");
        }
        /* A progress log that cannot be opened is not a reason to refuse to run
         * the work; it costs visibility, not correctness. */
        atlas_err_init(err);
    }
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
            /* A11.5a-R2. Streamed, one JSON record per line, so Atlas can see
             * the worker doing its work instead of inferring it from a silence
             * that only ever ends. With the single-document format the CLI says
             * nothing on stdout until it is finished, which made every long task
             * look identical to a wedged one and killed two real workers at the
             * idle bound while they were mid-turn. Verified against the
             * installed 2.1.235: `stream-json` needs no other flag, and line one
             * is still an object, so the malformed-result check below is
             * unaffected. */
            "--output-format", "stream-json",
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
        /* A11.1. The explicit working directory when the caller gave one, and
         * the workspace's writable tree otherwise. Both come from Atlas: the
         * first is the registry's canonical repository root, the second is a
         * path the dispatcher provisioned. Neither is ever taken from the task
         * text, the environment or anything the model produced. */
        opts.cwd = req->work_dir != NULL ? req->work_dir : atlas_buf_cstr(&req->ws->work);
        opts.timeout_ms = (int)(req->wall_timeout_ms > 0 ? req->wall_timeout_ms : 600000);
        opts.idle_timeout_ms = (int)req->idle_timeout_ms;
        opts.max_stdout = (size_t)out.max;
        opts.max_stderr = 256u * 1024u;
        opts.cancel = req->cancel;
        opts.cancel_ud = req->cancel_ud;
        /* Idleness now means "no recognised progress record", not "no bytes". */
        opts.activity = progress_activity;
        opts.activity_ud = &out;
        st = atlas_proc_run(&opts, capture_sink, &out, &errbuf, &pr, err);
    }

    /* The credential is dropped as soon as the child has it. */
    atlas_buf_free(&cred);

    /* Streams are stored redacted whatever happened, because the log of a failed
     * run is the only account of it. */
    if (out.out.len > 0 || pr.exit_code != 0) {
        atlas_err ignore;
        atlas_err_init(&ignore);
        (void)store_log(req->ws, "logs/stdout.log", &out.out, &res->log, &res->redactions, &ignore);
        (void)store_log(req->ws, "logs/stderr.log", &errbuf, &res->log, &res->redactions, &ignore);
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
        /* `--output-format json` emits a JSON *array* of message objects, not a
         * single object — measured against Claude Code 2.1.226, after a first
         * real run that did the work correctly and was then classified
         * MALFORMED_RESULT for starting with `[`. Both shapes are accepted; a
         * driver that printed prose still is not.
         *
         * The check stays structural and deliberately shallow. Nothing inside
         * the document is read, because a model's output is never parsed as
         * authority — the token and cost fields on the result stay zero, which
         * the header documents as "not reported" rather than "free". */
        res->exit_kind = (*s == '{' || *s == '[') ? ATLAS_ORCH_EXIT_OK
                                                  : ATLAS_ORCH_EXIT_MALFORMED_RESULT;
        if (req->ws != NULL) {
            atlas_err ignore;
            atlas_err_init(&ignore);
            (void)atlas_ws_write(req->ws, "driver/result.json", out.out.data, out.out.len,
                                 &ignore);
        }
    }
    /* A cancelled or bounded run is not a failure of Atlas, so the status is
     * cleared and the outcome is carried in `exit_kind` where the caller reads
     * it. A genuine spawn failure keeps its status. */
    if (st != ATLAS_OK && res->exit_kind != ATLAS_ORCH_EXIT_SPAWN_FAILED) {
        atlas_err_init(err);
        st = ATLAS_OK;
    }

    if (out.progress != NULL) {
        (void)fclose(out.progress);
        out.progress = NULL;
    }
    res->events = out.events;
    atlas_buf_free(&ppath);
    atlas_buf_free(&out.line);
    atlas_buf_free(&out.out);
    atlas_buf_free(&errbuf);
    atlas_buf_free(&home_env);
    atlas_buf_free(&home);
    atlas_buf_free(&exe);
    return st;
}

static atlas_status claude_run(const atlas_driver_req *req, atlas_driver_res *res,
                               atlas_err *err) {
    atlas_status st = atlas_buf_set_str(&res->version, "claude/1", err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (req->ws == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "the claude driver runs in an isolated workspace and was given none");
    }
    return claude_exec(req, res, err);
}

/* --- A11.1: the two drivers that work in the repository's own tree -----------
 *
 * The reversal this milestone makes, and the whole of it. A8's drivers work on
 * a snapshot in a worker-owned workspace, and Atlas applies nothing they
 * produce; A11.1's work in the registered repository's own tree, because the
 * season the operator asked for is a chain of tasks that build on each other's
 * changes and a follow-up task that could not see the first one's work would be
 * a follow-up to nothing.
 *
 * That does not weaken "never modify a registered target repository" anywhere
 * else. Atlas' own reads — scan, the index passes, every `src/git` invocation —
 * are unchanged and still read-only. What changes is that an **operator running
 * a foreground command** may now start a child process whose purpose is to
 * edit the tree, in a directory Atlas resolved from its own registry. The full
 * argument is in `docs/engineering-rules.md` under A11.1.
 *
 * Both are `exclusive`: they may only be handed to a lease that names them, so
 * the background dispatcher — which polls with an empty filter meaning "any" —
 * can never pick one up and run it somewhere it was not meant to run. */
static atlas_status claude_repo_run(const atlas_driver_req *req, atlas_driver_res *res,
                                    atlas_err *err) {
    atlas_status st = atlas_buf_set_str(&res->version, "claude-repo/1", err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (req->work_dir == NULL || req->work_dir[0] != '/') {
        /* Refused rather than defaulted. The one safe value for this is an
         * absolute path Atlas resolved from the registry, and every other value
         * — a relative path, the caller's cwd, a workspace — is a different
         * repository than the one the job was authorised over. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "the claude-repo driver needs an absolute working directory "
                             "resolved by Atlas and was given none");
    }
    return claude_exec(req, res, err);
}

/* The repository-tree counterpart of `fake`, and the reason the ten A11.1
 * acceptance contracts need no model, no network and no credential.
 *
 * It appends one line per start to a single file in the work tree, so a gate
 * can distinguish "the first worker ran" from "a second one did" without the
 * fixture having to reach inside Atlas. That is what lets one test prove a
 * failing gate produces exactly one follow-up whose worker then passes. The
 * `fake:` prefixes are the same Atlas literals `fake` matches, so a real task
 * text cannot select a behaviour by accident. */
static atlas_status fake_repo_run(const atlas_driver_req *req, atlas_driver_res *res,
                                  atlas_err *err) {
    atlas_status st = atlas_buf_set_str(&res->version, "fake-repo/1", err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (req->work_dir == NULL || req->work_dir[0] != '/') {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "the fake-repo driver needs an absolute working directory resolved "
                             "by Atlas and was given none");
    }
    if (req->cancel != NULL && req->cancel(req->cancel_ud)) {
        res->exit_kind = ATLAS_ORCH_EXIT_CANCELLED;
        res->exit_code = -1;
        return ATLAS_OK;
    }
    if (task_is(req->task, "fake:timeout")) {
        res->exit_kind = ATLAS_ORCH_EXIT_TIMEOUT;
        res->exit_code = -1;
        return atlas_buf_set_str(&res->log, "fake-repo: the worker did not finish\n", err);
    }
    if (task_is(req->task, "fake:cancel")) {
        res->exit_kind = ATLAS_ORCH_EXIT_CANCELLED;
        res->exit_code = -1;
        return ATLAS_OK;
    }
    if (task_is(req->task, "fake:malformed")) {
        res->exit_kind = ATLAS_ORCH_EXIT_MALFORMED_RESULT;
        res->exit_code = 0;
        return atlas_buf_set_str(&res->log, "fake-repo: this is not a result document\n", err);
    }

    bool fail = task_is(req->task, "fake:fail");
    /* A worker that committed, reset or checked out — which is a thing a real
     * `claude-repo` worker can do and the one thing the run driver re-checks
     * after the child exits. Reproducing it needs no git process here: the
     * caller has placed a ref at a second real commit, and pointing HEAD at that
     * ref is exactly the state `git checkout` would leave. The name is an Atlas
     * literal, so nothing else can select it.
     *
     * This assumes an ordinary repository whose git directory is `<root>/.git`,
     * which is what the fixture that drives it builds. A linked worktree would
     * need the git file read first, and this behaviour exists to be driven by
     * tests rather than to be general. */
    bool move_head = task_is(req->task, "fake:movehead");

    atlas_buf path = ATLAS_BUF_INIT;
    st = atlas_buf_appendf(&path, err, "%s/ATLAS_FAKE_DRIVER.txt", req->work_dir);
    if (st == ATLAS_OK) {
        FILE *f = fopen(atlas_buf_cstr(&path), "ae");
        if (f == NULL) {
            st = atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno,
                                     "cannot write into the repository work tree");
        } else {
            (void)fprintf(f, "atlas fake-repo driver job %s attempt %lld\n",
                          req->job_uid != NULL ? req->job_uid : "", (long long)req->attempt_no);
            if (fclose(f) != 0) {
                st = atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno,
                                         "cannot flush the fake driver's work");
            }
        }
    }
    atlas_buf_free(&path);
    if (st != ATLAS_OK) {
        return st;
    }

    if (move_head) {
        atlas_buf head = ATLAS_BUF_INIT;
        st = atlas_buf_appendf(&head, err, "%s/.git/HEAD", req->work_dir);
        if (st == ATLAS_OK) {
            FILE *f = fopen(atlas_buf_cstr(&head), "we");
            if (f == NULL) {
                st = atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno,
                                         "cannot move the fake worker's HEAD");
            } else {
                static const char REF[] = "ref: refs/heads/" ATLAS_FAKE_MOVED_BRANCH "\n";
                if (fwrite(REF, 1, sizeof REF - 1u, f) != sizeof REF - 1u) {
                    st = atlas_err_set(err, ATLAS_ERR_INTERNAL,
                                       "cannot move the fake worker's HEAD");
                }
                if (fclose(f) != 0 && st == ATLAS_OK) {
                    st = atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno,
                                             "cannot move the fake worker's HEAD");
                }
            }
        }
        atlas_buf_free(&head);
        if (st != ATLAS_OK) {
            return st;
        }
    }

    st = atlas_buf_appendf(&res->log, err, "fake-repo ran for job %s\n",
                           req->job_uid != NULL ? req->job_uid : "");
    res->stdout_bytes = (int64_t)res->log.len;
    res->exit_kind = fail ? ATLAS_ORCH_EXIT_NONZERO : ATLAS_ORCH_EXIT_OK;
    res->exit_code = fail ? 1 : 0;
    return st;
}

/* --- the registry ----------------------------------------------------------- */

static const atlas_driver DRIVER_FAKE = {
    .name = "fake", .version = "1", .needs_live_model = false,
    .run = fake_run};
static const atlas_driver DRIVER_CLAUDE = {
    .name = "claude", .version = "1", .needs_live_model = true,
    .run = claude_run};
static const atlas_driver DRIVER_CLAUDE_REPO = {
    .name = "claude-repo", .version = "1", .needs_live_model = true,
    .run = claude_repo_run};
static const atlas_driver DRIVER_FAKE_REPO = {
    .name = "fake-repo", .version = "1", .needs_live_model = false,
    .run = fake_repo_run};

static const atlas_driver *const DRIVERS[] = {&DRIVER_FAKE, &DRIVER_CLAUDE, &DRIVER_CLAUDE_REPO,
                                              &DRIVER_FAKE_REPO};

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
