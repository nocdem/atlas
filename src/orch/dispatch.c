/* Atlas - A8: the dispatcher, the one process that runs as `atlas-worker`.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * ## What it is
 *
 * A loop that asks the daemon for work, provisions an isolated workspace,
 * snapshots a pinned commit into it, runs a driver, validates, harvests, and
 * reports. It holds no database handle — not even read-only — and every fact it
 * acts on came over the socket from `atlasd`.
 *
 * ## What it is not allowed to decide
 *
 * Which repository, which commit, which driver, how many attempts, how long, or
 * whether the job succeeded in the end. All of those arrive in the lease grant
 * or are decided by the daemon when the completion is applied. The dispatcher
 * chooses nothing except *how* to carry out what it was handed.
 *
 * ## Fail-closed at startup
 *
 * A disabled or unreadable orchestration policy is a refusal to start, not a
 * loop that idles. A worker root that is not ours, or that another account can
 * write, is a refusal too. Both are checked before the first connection.
 */
#define _GNU_SOURCE 1

#include "atlas/dispatch.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/driver.h"
#include "atlas/ipc.h"
#include "atlas/orch.h"
#include "atlas/orchpolicy.h"
#include "atlas/safetext.h"
#include "atlas/sha256.h"
#include "atlas/snapshot.h"
#include "atlas/workspace.h"

/* Set by SIGTERM and SIGINT. The loop finishes the operation it is in and then
 * stops, so a shutdown never abandons an attempt mid-report — an abandoned
 * attempt is one the daemon has to recover, and a graceful stop should not
 * create work for the recovery path. */
static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static int64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

static void sleep_ms(int64_t ms) {
    if (ms <= 0) {
        return;
    }
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000L);
    (void)nanosleep(&ts, NULL);
}

static void say(const atlas_dispatch_opts *o, const char *fmt, ...) {
    if (o->log == NULL) {
        return;
    }
    char at[ATLAS_TS_MAX];
    atlas_now_iso8601(at, sizeof(at));
    va_list ap;
    va_start(ap, fmt);
    (void)fprintf(o->log, "%s dispatcher ", at);
    (void)vfprintf(o->log, fmt, ap);
    (void)fputc('\n', o->log);
    va_end(ap);
    (void)fflush(o->log);
}

/* --- one RPC round trip ---------------------------------------------------- */

typedef struct rpc {
    atlas_ipc_response *resp;
    atlas_buf raw;
} rpc;

static void rpc_free(rpc *r) {
    if (r->resp != NULL) {
        atlas_ipc_response_free(r->resp);
        r->resp = NULL;
    }
    atlas_buf_free(&r->raw);
}

/* Builds params with the typed writer and never by formatting JSON — the A2
 * rule, and the reason there is still no "write these bytes as JSON" primitive
 * anywhere in Atlas. */
typedef atlas_status (*rpc_build_fn)(atlas_json *j, void *ud, atlas_err *err);

static atlas_status rpc_call(const atlas_dispatch_opts *o, const char *method,
                             rpc_build_fn build, void *ud, rpc *out, atlas_err *err) {
    memset(out, 0, sizeof(*out));
    atlas_buf_init(&out->raw);
    atlas_ipc_params *p = NULL;
    atlas_json *j = NULL;
    atlas_status st = atlas_ipc_params_begin(&p, &j, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (build != NULL) {
        st = build(j, ud, err);
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_ipc_params_finish(p, &params, err);
    } else {
        atlas_ipc_params_abort(p);
    }
    if (st != ATLAS_OK) {
        atlas_buf_free(&params);
        return st;
    }
    st = atlas_ipc_call(o->socket_path, method, atlas_buf_cstr(&params), &out->raw, err);
    atlas_buf_free(&params);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_ipc_response_parse(out->raw.data, out->raw.len, &out->resp, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (!atlas_ipc_response_ok(out->resp)) {
        return atlas_err_set(err, atlas_ipc_response_status(out->resp), "%s: %s", method,
                             atlas_ipc_response_message(out->resp));
    }
    return ATLAS_OK;
}

/* --- the lease a dispatcher is currently working under --------------------- */

typedef struct attempt {
    atlas_buf job_uid;
    atlas_buf token;
    atlas_buf repo_root;
    atlas_buf commit;
    atlas_buf mode;
    atlas_buf driver;
    atlas_buf task;
    atlas_buf allowed_paths;
    atlas_buf validations;
    int64_t attempt_no;
    int64_t wall_timeout_ms;
    int64_t idle_timeout_ms;
    int64_t max_output_bytes;
    int64_t max_artifact_bytes;
    int64_t max_artifact_count;
    int64_t started_ms;
    int64_t event_seq;
    /* Set once the daemon has told us to stop. Read by the driver's cancel
     * callback, which is the only way a running child learns of it. */
    bool cancelled;
    const atlas_dispatch_opts *opts;
    int64_t last_heartbeat_ms;
} attempt;

static void attempt_init(attempt *a) {
    memset(a, 0, sizeof(*a));
    atlas_buf_init(&a->job_uid);
    atlas_buf_init(&a->token);
    atlas_buf_init(&a->repo_root);
    atlas_buf_init(&a->commit);
    atlas_buf_init(&a->mode);
    atlas_buf_init(&a->driver);
    atlas_buf_init(&a->task);
    atlas_buf_init(&a->allowed_paths);
    atlas_buf_init(&a->validations);
}

static void attempt_free(attempt *a) {
    atlas_buf_free(&a->job_uid);
    /* The token is zeroed before it is released. It is a bearer capability and
     * leaving it in freed memory is a needless second copy. */
    if (a->token.data != NULL && a->token.len > 0) {
        memset(a->token.data, 0, a->token.len);
    }
    atlas_buf_free(&a->token);
    atlas_buf_free(&a->repo_root);
    atlas_buf_free(&a->commit);
    atlas_buf_free(&a->mode);
    atlas_buf_free(&a->driver);
    atlas_buf_free(&a->task);
    atlas_buf_free(&a->allowed_paths);
    atlas_buf_free(&a->validations);
}

static atlas_status build_lease(atlas_json *j, void *ud, atlas_err *err) {
    const atlas_dispatch_opts *o = (const atlas_dispatch_opts *)ud;
    atlas_status st = atlas_json_key_str(j, "dispatcher", o->dispatcher_id, err);
    if (st == ATLAS_OK && o->drivers != NULL && o->drivers[0] != '\0') {
        /* Which drivers this dispatcher will run. The daemon matches it against
         * the job's *stored* driver, so this narrows what we are offered and can
         * never widen it. */
        st = atlas_json_key(j, "driver", err);
        if (st == ATLAS_OK) {
            st = atlas_json_arr_begin(j, err);
        }
        const char *p = o->drivers;
        while (st == ATLAS_OK && *p != '\0') {
            const char *comma = strchr(p, ',');
            size_t n = comma != NULL ? (size_t)(comma - p) : strlen(p);
            if (n > 0 && n < ATLAS_ORCH_NAME_MAX) {
                char one[ATLAS_ORCH_NAME_MAX + 1u];
                memcpy(one, p, n);
                one[n] = '\0';
                st = atlas_json_str(j, one, err);
            }
            p = comma != NULL ? comma + 1 : p + n;
        }
        if (st == ATLAS_OK) {
            st = atlas_json_arr_end(j, err);
        }
    }
    return st;
}

/* --- heartbeat -------------------------------------------------------------- */

typedef struct hb_args {
    const attempt *a;
    const char *phase; /* NULL to renew without advancing */
} hb_args;

static atlas_status build_heartbeat(atlas_json *j, void *ud, atlas_err *err) {
    hb_args *h = (hb_args *)ud;
    atlas_status st = atlas_json_key_str(j, "token", atlas_buf_cstr(&h->a->token), err);
    if (st == ATLAS_OK && h->phase != NULL) {
        st = atlas_json_key_str(j, "phase", h->phase, err);
    }
    if (st == ATLAS_OK) {
        /* The worker's own pid, sent as a claim. The daemon records it and
         * decides nothing on it: a worker describing itself is not evidence
         * about itself. */
        st = atlas_json_key_int(j, "pid", (int64_t)getpid(), err);
    }
    return st;
}

/* Sends a heartbeat and records whether cancellation was requested.
 *
 * Heartbeats are emitted on a timer by the caller, **independently of whether
 * the driver has produced any output**. A liveness signal derived from a
 * model's chattiness would go quiet exactly when a long tool call is running. */
static atlas_status heartbeat(attempt *a, const char *phase, atlas_err *err) {
    hb_args h = {a, phase};
    rpc r;
    atlas_status st = rpc_call(a->opts, "dispatch.heartbeat", build_heartbeat, &h, &r, err);
    if (st == ATLAS_OK) {
        bool cancel = false;
        if (atlas_ipc_result_bool(r.resp, "cancel_requested", &cancel) && cancel) {
            a->cancelled = true;
        }
        a->last_heartbeat_ms = now_ms();
    }
    rpc_free(&r);
    return st;
}

/* Heartbeats if the interval has elapsed, and otherwise does nothing.
 *
 * Every phase that can run longer than the lease TTL has to call this, not just
 * the one where a driver is running. A real repository proved why: transferring
 * a snapshot of 1 924 files took longer than the lease, and because the transfer
 * loop was the one long phase that never heartbeated, the attempt lost its lease
 * mid-copy and the job stalled in PREPARING. Keeping a lease alive is a property
 * of *making progress*, and copying a file is progress.
 *
 * A failed heartbeat is not a reason to abandon the work. The lease will expire
 * and the daemon will reconcile, which is the designed recovery path; the
 * timestamp is advanced anyway so a dead daemon is not asked once per poll. */
static void heartbeat_if_due(attempt *a) {
    int64_t now = now_ms();
    if (now - a->last_heartbeat_ms < a->opts->heartbeat_ms) {
        return;
    }
    atlas_err err;
    atlas_err_init(&err);
    if (heartbeat(a, NULL, &err) != ATLAS_OK) {
        say(a->opts, "heartbeat failed: %s", atlas_err_msg(&err));
        a->last_heartbeat_ms = now;
    }
}

/* The driver's cancel callback. Called on the runner's poll schedule; it
 * heartbeats no more often than the configured interval, so a long-running
 * driver keeps its lease alive and learns of a cancellation without the
 * dispatcher needing a second thread. */
static bool driver_should_stop(void *ud) {
    attempt *a = (attempt *)ud;
    if (g_stop != 0) {
        return true;
    }
    heartbeat_if_due(a);
    return a->cancelled;
}

/* --- structured events ------------------------------------------------------ */

typedef struct ev_args {
    attempt *a;
    const char *kind;
    const char *payload;
} ev_args;

static atlas_status build_event(atlas_json *j, void *ud, atlas_err *err) {
    ev_args *e = (ev_args *)ud;
    atlas_status st = atlas_json_key_str(j, "token", atlas_buf_cstr(&e->a->token), err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "seq", e->a->event_seq, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "kind", e->kind, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "payload", e->payload != NULL ? e->payload : "", err);
    }
    return st;
}

static void emit_event(attempt *a, const char *kind, const char *payload) {
    ev_args e = {a, kind, payload};
    rpc r;
    atlas_err err;
    atlas_err_init(&err);
    if (rpc_call(a->opts, "dispatch.event", build_event, &e, &r, &err) == ATLAS_OK) {
        a->event_seq++;
    } else {
        /* An event that could not be delivered is logged locally and does not
         * fail the attempt: the narrative is valuable, and losing a line of it
         * is not worth abandoning work that is otherwise fine. */
        say(a->opts, "event not delivered: %s", atlas_err_msg(&err));
    }
    rpc_free(&r);
}

/* --- validation commands ----------------------------------------------------
 *
 * Structured argv, an exact working directory, a clean environment, and bounded
 * output. There is no shell anywhere on this path: `atlas_proc_run` execve's an
 * argument vector, and a validation command is a vector of counted arguments all
 * the way from the job specification.
 *
 * argv[0] is resolved against a fixed allowlist rather than against PATH, so a
 * job cannot name an arbitrary program and a `PATH` a driver planted in the
 * workspace cannot select one. */
static const char *const VALIDATION_PROGRAMS[] = {"make", "sh-free-placeholder"};

static bool validation_program_allowed(const char *name) {
    /* Deliberately tiny, and deliberately not containing a shell. A deployment
     * that needs another program adds it here, in the binary, rather than in a
     * job — which is the difference between an operator's decision and a
     * submitter's. */
    (void)VALIDATION_PROGRAMS;
    static const char *const ALLOWED[] = {"make", "ctest", "cmake", "true", "false"};
    for (size_t i = 0; i < sizeof ALLOWED / sizeof ALLOWED[0]; i++) {
        if (strcmp(name, ALLOWED[i]) == 0) {
            return true;
        }
    }
    return false;
}

static atlas_status run_validations(attempt *a, const atlas_ws *ws, bool *passed, atlas_err *err) {
    *passed = true;
    atlas_orch_argv cmds[ATLAS_ORCH_MAX_VALIDATIONS];
    for (size_t i = 0; i < ATLAS_ORCH_MAX_VALIDATIONS; i++) {
        atlas_orch_argv_init(&cmds[i]);
    }
    size_t n = 0;
    atlas_status st = atlas_orch_validations_decode(atlas_buf_cstr(&a->validations), cmds,
                                                    ATLAS_ORCH_MAX_VALIDATIONS, &n, err);
    for (size_t i = 0; st == ATLAS_OK && i < n && *passed; i++) {
        const char *prog = atlas_buf_cstr(&cmds[i].args[0]);
        if (!validation_program_allowed(prog)) {
            *passed = false;
            emit_event(a, "validation", "refused: program is not on the allowlist");
            break;
        }
        atlas_buf exe = ATLAS_BUF_INIT;
        st = atlas_proc_which(prog, "/usr/local/bin:/usr/bin:/bin", &exe, err);
        if (st != ATLAS_OK) {
            *passed = false;
            atlas_buf_free(&exe);
            break;
        }
        const char *argv[ATLAS_ORCH_MAX_ARGV + 2u];
        size_t k = 0;
        argv[k++] = atlas_buf_cstr(&exe);
        for (size_t v = 1; v < cmds[i].count; v++) {
            argv[k++] = atlas_buf_cstr(&cmds[i].args[v]);
        }
        argv[k] = NULL;

        /* A clean, explicitly built environment. Nothing inherited: no SSH
         * agent, no sudo askpass, no credential, no operator configuration. */
        static const char *const ENV[] = {"PATH=/usr/local/bin:/usr/bin:/bin", "LC_ALL=C",
                                          "LANG=C", "TZ=UTC", NULL};
        atlas_buf out = ATLAS_BUF_INIT;
        atlas_buf errout = ATLAS_BUF_INIT;
        atlas_proc_opts opts;
        memset(&opts, 0, sizeof(opts));
        opts.argv = argv;
        opts.env = ENV;
        opts.cwd = atlas_buf_cstr(&ws->work);
        opts.timeout_ms = (int)a->wall_timeout_ms;
        opts.idle_timeout_ms = (int)a->idle_timeout_ms;
        opts.max_stdout = (size_t)a->max_output_bytes;
        opts.max_stderr = 256u * 1024u;
        opts.cancel = driver_should_stop;
        opts.cancel_ud = a;
        atlas_proc_result pr;
        memset(&pr, 0, sizeof(pr));
        atlas_status rs = atlas_proc_run(&opts, atlas_proc_sink_buf, &out, &errout, &pr, err);

        /* Evidence is stored whatever the outcome; a failed validation's output
         * is the only account of why it failed. */
        char rel[64];
        (void)snprintf(rel, sizeof rel, "tests/validation-%zu.log", i);
        atlas_err ignore;
        atlas_err_init(&ignore);
        /* Redacted like every other captured stream. A validation command's
         * output is as likely to echo an environment as a driver's is. */
        atlas_buf clean = ATLAS_BUF_INIT;
        if (atlas_ws_redact(out.data != NULL ? out.data : "", out.len, &clean, NULL, &ignore) ==
            ATLAS_OK) {
            (void)atlas_ws_write(ws, rel, clean.data, clean.len, &ignore);
        }
        atlas_buf_free(&clean);

        if (rs != ATLAS_OK || pr.exit_code != 0 || pr.timed_out || pr.idle_timed_out) {
            *passed = false;
            atlas_err_init(err);
        }
        atlas_buf_free(&out);
        atlas_buf_free(&errout);
        atlas_buf_free(&exe);
    }
    for (size_t i = 0; i < ATLAS_ORCH_MAX_VALIDATIONS; i++) {
        atlas_orch_argv_free(&cmds[i]);
    }
    return st;
}

/* --- receiving the snapshot ---------------------------------------------------
 *
 * The worker asks; the daemon reads. Nothing here names a repository, a commit
 * or a host path — the lease token is the whole request, and the daemon resolves
 * everything else from persisted state.
 *
 * What arrives is verified rather than trusted: every path is re-checked before
 * it is materialised, every entry's content digest is recomputed from the bytes
 * that actually landed, and the snapshot digest is recomputed from the manifest
 * the worker itself assembled. A stream that lost, duplicated or reordered an
 * entry cannot produce a match. */
static atlas_status snap_build_open(atlas_json *j, void *ud, atlas_err *err) {
    return atlas_json_key_str(j, "token", atlas_buf_cstr(&((attempt *)ud)->token), err);
}

typedef struct chunk_req {
    attempt *a;
    int64_t index;
    int64_t offset;
} chunk_req;

static atlas_status snap_build_chunk(atlas_json *j, void *ud, atlas_err *err) {
    chunk_req *c = (chunk_req *)ud;
    atlas_status st = atlas_json_key_str(j, "token", atlas_buf_cstr(&c->a->token), err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "index", c->index, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "offset", c->offset, err);
    }
    return st;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    return -1;
}

/* Strict: an odd length or any non-hex nibble is a malformed stream, not a
 * shorter one. */
static atlas_status hex_decode(const char *in, atlas_buf *out, atlas_err *err) {
    size_t n = strlen(in);
    atlas_buf_reset(out);
    if ((n % 2u) != 0) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "a snapshot chunk has an odd hex length");
    }
    for (size_t i = 0; i < n; i += 2u) {
        int hi = hexval(in[i]);
        int lo = hexval(in[i + 1u]);
        if (hi < 0 || lo < 0) {
            return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "a snapshot chunk is not hex");
        }
        char b = (char)((hi << 4) | lo);
        atlas_status st = atlas_buf_append(out, &b, 1u, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    return ATLAS_OK;
}

static atlas_status fetch_snapshot(attempt *a, const atlas_ws *ws, atlas_ws_snapshot_stats *stats,
                                   atlas_err *err) {
    memset(stats, 0, sizeof(*stats));
    rpc r;
    atlas_status st = rpc_call(a->opts, "dispatch.snapshot.open", snap_build_open, a, &r, err);
    if (st != ATLAS_OK) {
        rpc_free(&r);
        return st;
    }
    int64_t protocol = 0, entries = 0, total = 0;
    const char *commit = NULL, *tree = NULL, *digest = NULL;
    (void)atlas_ipc_result_int(r.resp, "protocol", &protocol);
    (void)atlas_ipc_result_int(r.resp, "entries", &entries);
    (void)atlas_ipc_result_int(r.resp, "total_bytes", &total);
    (void)atlas_ipc_result_str(r.resp, "commit", &commit);
    (void)atlas_ipc_result_str(r.resp, "tree", &tree);
    (void)atlas_ipc_result_str(r.resp, "digest", &digest);
    atlas_buf want_digest = ATLAS_BUF_INIT;
    atlas_buf commit_copy = ATLAS_BUF_INIT;
    atlas_buf tree_copy = ATLAS_BUF_INIT;
    if (digest != NULL) {
        (void)atlas_buf_set_str(&want_digest, digest, err);
    }
    if (commit != NULL) {
        (void)atlas_buf_set_str(&commit_copy, commit, err);
    }
    if (tree != NULL) {
        (void)atlas_buf_set_str(&tree_copy, tree, err);
    }
    (void)atlas_ipc_result_int(r.resp, "refused_symlinks", &stats->skipped_symlinks);
    (void)atlas_ipc_result_int(r.resp, "refused_gitlinks", &stats->skipped_submodules);
    (void)atlas_ipc_result_int(r.resp, "refused_other", &stats->skipped_other);
    (void)atlas_ipc_result_int(r.resp, "refused_sizes", &stats->skipped_oversize);
    rpc_free(&r);

    if (protocol != ATLAS_SNAPSHOT_PROTOCOL) {
        /* Refused rather than guessed at. A version this worker does not know is
         * a wire shape it cannot verify. */
        st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                           "the daemon offered snapshot protocol %lld; this worker speaks %d",
                           (long long)protocol, ATLAS_SNAPSHOT_PROTOCOL);
        goto done;
    }
    if (entries < 0 || entries > ATLAS_SNAPSHOT_MAX_ENTRIES ||
        total > ATLAS_SNAPSHOT_MAX_TOTAL_BYTES) {
        st = atlas_err_set(err, ATLAS_ERR_INTEGRITY, "the offered snapshot exceeds its bounds");
        goto done;
    }

    atlas_snapshot_digest dg;
    st = atlas_snapshot_digest_begin(&dg, atlas_buf_cstr(&commit_copy), atlas_buf_cstr(&tree_copy),
                                     err);
    if (st != ATLAS_OK) {
        goto done;
    }

    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_buf path = ATLAS_BUF_INIT;
    int64_t got_bytes = 0;
    for (int64_t i = 0; st == ATLAS_OK && i < entries; i++) {
        int64_t offset = 0;
        atlas_sha256 h;
        atlas_sha256_init(&h);
        char mode[8] = {0};
        char want_sha[65] = {0};
        int64_t size = -1;
        bool eof = false;
        atlas_buf_reset(&path);
        while (st == ATLAS_OK && !eof) {
            if (g_stop != 0 || a->cancelled) {
                /* Cancellation and shutdown stop a transfer in progress rather
                 * than letting it run to completion first. */
                st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                   "the snapshot transfer was cancelled");
                break;
            }
            /* Same throttle as the driver phase: a large tree is a long phase,
             * and a lease that expires mid-transfer loses the whole attempt. */
            heartbeat_if_due(a);
            chunk_req cr = {a, i, offset};
            rpc cr_resp;
            st = rpc_call(a->opts, "dispatch.snapshot.chunk", snap_build_chunk, &cr, &cr_resp,
                          err);
            if (st != ATLAS_OK) {
                rpc_free(&cr_resp);
                break;
            }
            const char *p = NULL, *m = NULL, *sha = NULL, *data = NULL;
            int64_t roff = -1, nbytes = 0, rsize = -1;
            bool reof = false;
            (void)atlas_ipc_result_str(cr_resp.resp, "path", &p);
            (void)atlas_ipc_result_str(cr_resp.resp, "mode", &m);
            (void)atlas_ipc_result_str(cr_resp.resp, "sha256", &sha);
            (void)atlas_ipc_result_str(cr_resp.resp, "data", &data);
            (void)atlas_ipc_result_int(cr_resp.resp, "offset", &roff);
            (void)atlas_ipc_result_int(cr_resp.resp, "bytes", &nbytes);
            (void)atlas_ipc_result_int(cr_resp.resp, "size", &rsize);
            (void)atlas_ipc_result_bool(cr_resp.resp, "eof", &reof);

            /* The offset the daemon answered must be the offset that was asked
             * for. A reordered or duplicated chunk is refused here rather than
             * being appended in the wrong place. */
            if (roff != offset) {
                st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                   "snapshot chunk arrived out of order (asked %lld, got %lld)",
                                   (long long)offset, (long long)roff);
            }
            if (st == ATLAS_OK && offset == 0) {
                size = rsize;
                (void)snprintf(mode, sizeof mode, "%s", m != NULL ? m : "");
                (void)snprintf(want_sha, sizeof want_sha, "%s", sha != NULL ? sha : "");
                /* The path is safe-encoded on the wire; decoded here and then
                 * re-checked, because a receiver that trusts the sender's
                 * validation has no boundary of its own. */
                st = atlas_text_decode_safe(p != NULL ? p : "", p != NULL ? strlen(p) : 0u, &path,
                                            err);
            }
            if (st == ATLAS_OK && data != NULL) {
                st = hex_decode(data, &raw, err);
            }
            if (st == ATLAS_OK && (int64_t)raw.len != nbytes) {
                st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                   "a snapshot chunk declared %lld bytes and carried %zu",
                                   (long long)nbytes, raw.len);
            }
            if (st == ATLAS_OK) {
                atlas_sha256_update(&h, raw.data != NULL ? raw.data : "", raw.len);
                st = atlas_ws_materialise(ws, path.data, path.len, mode, raw.data, raw.len,
                                          offset == 0, err);
            }
            if (st == ATLAS_OK) {
                offset += (int64_t)raw.len;
                got_bytes += (int64_t)raw.len;
                eof = reof;
                if (!reof && raw.len == 0) {
                    st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                       "the snapshot stream stalled before its end");
                }
            }
            rpc_free(&cr_resp);
        }
        if (st != ATLAS_OK) {
            break;
        }
        /* Per-entry content digest, recomputed from the bytes that landed. */
        unsigned char d[ATLAS_SHA256_DIGEST_LEN];
        atlas_sha256_final(&h, d);
        char hex[ATLAS_SHA256_HEX_LEN + 1u];
        atlas_hex_encode(d, sizeof d, hex);
        if (size != offset || strcmp(hex, want_sha) != 0) {
            st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                               "a materialised snapshot entry does not match its declared digest");
            break;
        }
        st = atlas_snapshot_digest_entry(&dg, path.data, path.len, mode, offset, hex, err);
        stats->files++;
        stats->bytes += offset;
    }
    atlas_buf_free(&raw);
    atlas_buf_free(&path);

    if (st == ATLAS_OK) {
        char got[65];
        st = atlas_snapshot_digest_finish(&dg, stats->files, got_bytes, got, err);
        if (st == ATLAS_OK && strcmp(got, atlas_buf_cstr(&want_digest)) != 0) {
            /* The whole point of the digest: a partial materialisation is never
             * accepted as complete. */
            st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                               "the materialised snapshot does not match the digest the daemon "
                               "declared");
        }
    } else {
        atlas_snapshot_digest_abort(&dg);
    }

done:
    atlas_buf_free(&want_digest);
    atlas_buf_free(&commit_copy);
    atlas_buf_free(&tree_copy);
    return st;
}

/* --- completion -------------------------------------------------------------- */

typedef struct done_args {
    attempt *a;
    const atlas_driver_res *dr;
    const atlas_ws_artifact *arts;
    size_t nart;
    bool success;
} done_args;

static atlas_status build_complete(atlas_json *j, void *ud, atlas_err *err) {
    done_args *d = (done_args *)ud;
    atlas_status st = atlas_json_key_str(j, "token", atlas_buf_cstr(&d->a->token), err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(j, "success", d->success, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "exit_kind", atlas_orch_exit_kind_name(d->dr->exit_kind), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "exit_code", d->dr->exit_code, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "driver_version", atlas_buf_cstr(&d->dr->version), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "pid", (int64_t)getpid(), err);
    }
    if (st == ATLAS_OK && d->nart > 0) {
        st = atlas_json_key(j, "artifact", err);
        if (st == ATLAS_OK) {
            st = atlas_json_arr_begin(j, err);
        }
        for (size_t i = 0; st == ATLAS_OK && i < d->nart; i++) {
            /* One manifest entry per artifact, unit-separated. The separator
             * cannot occur in any field: a name is a safe relative path, a kind
             * and a digest are names, and a size is decimal. */
            atlas_buf ent = ATLAS_BUF_INIT;
            st = atlas_buf_appendf(&ent, err, "%s\x1f%s\x1f%s\x1f%lld",
                                   atlas_buf_cstr(&d->arts[i].name), "artifact",
                                   atlas_buf_cstr(&d->arts[i].sha256),
                                   (long long)d->arts[i].size_bytes);
            if (st == ATLAS_OK) {
                st = atlas_json_str(j, atlas_buf_cstr(&ent), err);
            }
            atlas_buf_free(&ent);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_arr_end(j, err);
        }
    }
    return st;
}

/* --- one attempt --------------------------------------------------------------- */

static atlas_status run_attempt(attempt *a, atlas_err *err) {
    const atlas_dispatch_opts *o = a->opts;
    atlas_ws ws;
    atlas_ws_init(&ws);
    atlas_driver_res dr;
    atlas_driver_res_init(&dr);
    atlas_ws_artifact *arts = NULL;
    size_t nart = 0;
    bool success = false;

    /* Disk first. A job that fills the disk takes the machine with it, and the
     * dispatcher is the only thing positioned to notice before it starts. */
    int64_t freeb = 0;
    atlas_status st = atlas_ws_free_space(o->worker_root, &freeb, err);
    if (st == ATLAS_OK && freeb < ATLAS_WS_MIN_FREE_BYTES) {
        st = atlas_err_set(err, ATLAS_ERR_CONFIG,
                           "only %lld bytes free under the worker root; %lld are required",
                           (long long)freeb, (long long)ATLAS_WS_MIN_FREE_BYTES);
    }

    if (st == ATLAS_OK) {
        st = heartbeat(a, "PREPARING", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_ws_open(o->worker_root, atlas_buf_cstr(&a->job_uid), a->attempt_no, &ws, err);
    }
    if (st == ATLAS_OK) {
        /* The immutable specification, written into the attempt so the workspace
         * is self-describing after the fact. */
        atlas_buf spec = ATLAS_BUF_INIT;
        st = atlas_buf_appendf(&spec, err,
                               "job %s\nattempt %lld\ncommit %s\nmode %s\ndriver %s\n",
                               atlas_buf_cstr(&a->job_uid), (long long)a->attempt_no,
                               atlas_buf_cstr(&a->commit), atlas_buf_cstr(&a->mode),
                               atlas_buf_cstr(&a->driver));
        if (st == ATLAS_OK) {
            st = atlas_ws_write(&ws, "spec.json", spec.data, spec.len, err);
        }
        atlas_buf_free(&spec);
    }

    atlas_ws_snapshot_stats snap;
    memset(&snap, 0, sizeof(snap));
    if (st == ATLAS_OK) {
        st = fetch_snapshot(a, &ws, &snap, err);
    }
    if (st == ATLAS_OK) {
        char note[256];
        (void)snprintf(note, sizeof note,
                       "snapshot: %lld files, %lld bytes, %lld oversize, %lld symlinks and %lld "
                       "submodules "
                       "refused",
                       (long long)snap.files, (long long)snap.bytes,
                       (long long)snap.skipped_oversize, (long long)snap.skipped_symlinks,
                       (long long)snap.skipped_submodules);
        emit_event(a, "snapshot", note);
        st = heartbeat(a, "RUNNING", err);
    }

    const atlas_driver *drv = NULL;
    if (st == ATLAS_OK) {
        drv = atlas_driver_find(atlas_buf_cstr(&a->driver));
        if (drv == NULL) {
            /* An unknown driver is a refusal, never a default. */
            st = atlas_err_set(err, ATLAS_ERR_USAGE, "no driver named \"%s\" is built in",
                               atlas_buf_cstr(&a->driver));
        }
    }
    if (st == ATLAS_OK) {
        atlas_driver_req req;
        memset(&req, 0, sizeof(req));
        req.ws = &ws;
        req.job_uid = atlas_buf_cstr(&a->job_uid);
        req.attempt_no = a->attempt_no;
        req.task = atlas_buf_cstr(&a->task);
        req.mode = atlas_buf_cstr(&a->mode);
        req.wall_timeout_ms = a->wall_timeout_ms;
        req.idle_timeout_ms = a->idle_timeout_ms;
        req.max_output_bytes = a->max_output_bytes;
        req.cancel = driver_should_stop;
        req.cancel_ud = a;
        req.live_model = o->live_model;
        req.operator_session = o->operator_session;
        st = drv->run(&req, &dr, err);
    }

    /* Whatever the driver did, the patch and the artifacts are produced from
     * what is on disk rather than from what the driver said it did. */
    if (st == ATLAS_OK) {
        int64_t changed = 0;
        bool differed = false;
        atlas_err ignore;
        atlas_err_init(&ignore);
        (void)atlas_ws_make_patch(&ws, a->max_artifact_bytes, &changed, &differed, &ignore);
        char note[128];
        (void)snprintf(note, sizeof note, "%lld files changed", (long long)changed);
        emit_event(a, "patch", note);
    }

    bool validated = true;
    if (st == ATLAS_OK && dr.exit_kind == ATLAS_ORCH_EXIT_OK && a->validations.len > 0) {
        atlas_err verr;
        atlas_err_init(&verr);
        if (heartbeat(a, "VALIDATING", &verr) == ATLAS_OK) {
            (void)run_validations(a, &ws, &validated, &verr);
        }
    }

    if (st == ATLAS_OK) {
        atlas_err ignore;
        atlas_err_init(&ignore);
        int64_t refused = 0;
        (void)atlas_ws_collect(&ws, a->max_artifact_count, a->max_artifact_bytes,
                               ATLAS_ORCH_ARTIFACT_INLINE_MAX, &arts, &nart, &refused, &ignore);
        if (refused > 0) {
            emit_event(a, "artifacts", "entries were refused: not regular files");
        }
    }

    success = (st == ATLAS_OK) && dr.exit_kind == ATLAS_ORCH_EXIT_OK && validated && !a->cancelled;

    /* Reported even when something above failed: an attempt the daemon never
     * hears about is one it has to expire, and an expiry says less than a
     * failure does. */
    {
        done_args d = {a, &dr, arts, nart, success};
        rpc r;
        atlas_err cerr;
        atlas_err_init(&cerr);
        if (rpc_call(o, "dispatch.complete", build_complete, &d, &r, &cerr) != ATLAS_OK) {
            say(o, "completion not delivered for %s: %s", atlas_buf_cstr(&a->job_uid),
                atlas_err_msg(&cerr));
        } else {
            const char *state = NULL;
            (void)atlas_ipc_result_str(r.resp, "state", &state);
            say(o, "job %s attempt %lld -> %s", atlas_buf_cstr(&a->job_uid),
                (long long)a->attempt_no, state != NULL ? state : "?");
        }
        rpc_free(&r);
    }

    /* Retention: a successful attempt's workspace is removed, a failed one is
     * kept as evidence. Bounded either way — the number of attempts is bounded,
     * so the number of retained workspaces is too. */
    atlas_ws_free(&ws);
    if (success && o->keep_workspaces == false) {
        atlas_err ignore;
        atlas_err_init(&ignore);
        (void)atlas_ws_remove(o->worker_root, atlas_buf_cstr(&a->job_uid), a->attempt_no,
                              &ignore);
    }
    atlas_ws_artifacts_free(arts, nart);
    atlas_driver_res_free(&dr);
    return st;
}

/* --- the loop ------------------------------------------------------------------ */

atlas_status atlas_dispatch_run(const atlas_dispatch_opts *o, atlas_err *err) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    (void)sigaction(SIGTERM, &sa, NULL);
    (void)sigaction(SIGINT, &sa, NULL);
    /* A driver's child closing a pipe must not kill the dispatcher. */
    signal(SIGPIPE, SIG_IGN);

    say(o, "starting as uid %lld, worker root %s", (long long)getuid(), o->worker_root);

    int64_t backoff = o->poll_ms;
    int64_t iterations = 0;
    for (;;) {
        if (g_stop != 0) {
            say(o, "stopping on signal");
            break;
        }
        if (o->max_iterations > 0 && iterations >= o->max_iterations) {
            break;
        }
        iterations++;

        rpc r;
        atlas_err lerr;
        atlas_err_init(&lerr);
        /* A non-const alias rather than casting away const: the builder only
         * reads it, and a cast that discards a qualifier is the kind of thing
         * that is right today and wrong after one edit. */
        atlas_dispatch_opts lease_ud = *o;
        atlas_status st = rpc_call(o, "dispatch.lease", build_lease, &lease_ud, &r, &lerr);
        if (st != ATLAS_OK) {
            /* A daemon that is not answering is an ordinary condition — it may
             * be restarting — so the loop backs off and keeps trying rather
             * than exiting. Bounded exponential backoff, because a tight retry
             * against a down socket is a busy loop. */
            rpc_free(&r);
            say(o, "no daemon: %s (retrying in %lld ms)", atlas_err_msg(&lerr),
                (long long)backoff);
            sleep_ms(backoff);
            backoff = backoff * 2 > o->max_backoff_ms ? o->max_backoff_ms : backoff * 2;
            continue;
        }
        backoff = o->poll_ms;

        bool granted = false;
        (void)atlas_ipc_result_bool(r.resp, "granted", &granted);
        if (!granted) {
            rpc_free(&r);
            if (o->max_iterations > 0) {
                continue;
            }
            sleep_ms(o->poll_ms);
            continue;
        }

        attempt a;
        attempt_init(&a);
        a.opts = o;
        a.started_ms = now_ms();
        a.last_heartbeat_ms = a.started_ms;
        struct {
            atlas_buf *to;
            const char *key;
        } strs[] = {
            {&a.job_uid, "job"},        {&a.token, "token"},
            {&a.repo_root, "repo_root"}, {&a.commit, "commit"},
            {&a.mode, "mode"},          {&a.driver, "driver"},
            {&a.task, "task"},          {&a.allowed_paths, "allowed_paths"},
            {&a.validations, "validations"},
        };
        atlas_status gs = ATLAS_OK;
        for (size_t i = 0; gs == ATLAS_OK && i < sizeof strs / sizeof strs[0]; i++) {
            const char *v = NULL;
            if (atlas_ipc_result_str(r.resp, strs[i].key, &v) && v != NULL) {
                gs = atlas_buf_set_str(strs[i].to, v, &lerr);
            }
        }
        (void)atlas_ipc_result_int(r.resp, "attempt", &a.attempt_no);
        (void)atlas_ipc_result_int(r.resp, "wall_timeout_ms", &a.wall_timeout_ms);
        (void)atlas_ipc_result_int(r.resp, "idle_timeout_ms", &a.idle_timeout_ms);
        (void)atlas_ipc_result_int(r.resp, "max_output_bytes", &a.max_output_bytes);
        (void)atlas_ipc_result_int(r.resp, "max_artifact_bytes", &a.max_artifact_bytes);
        (void)atlas_ipc_result_int(r.resp, "max_artifact_count", &a.max_artifact_count);
        rpc_free(&r);

        if (gs == ATLAS_OK) {
            say(o, "leased %s attempt %lld (%s)", atlas_buf_cstr(&a.job_uid),
                (long long)a.attempt_no, atlas_buf_cstr(&a.driver));
            atlas_err aerr;
            atlas_err_init(&aerr);
            if (run_attempt(&a, &aerr) != ATLAS_OK) {
                say(o, "attempt failed: %s", atlas_err_msg(&aerr));
            }
        }
        attempt_free(&a);
    }
    (void)err;
    return ATLAS_OK;
}
