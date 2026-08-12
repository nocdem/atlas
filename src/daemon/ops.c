/* Atlas - long-running daemon operations, accepted then polled.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See atlas/ops.h for why this exists and, more importantly, for what it
 * deliberately does not promise.
 *
 * Everything in the table is guarded by one mutex, and nothing outside this
 * file ever holds a pointer into it. Records are copied out, for the reason
 * row callbacks hand out borrowed pointers: a caller that kept the pointer
 * would be reading a record the operation thread is still writing.
 */
#define _GNU_SOURCE 1

#include "atlas/ops.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>

#include "atlas/backup.h"
#include "atlas/limits.h"
#include "atlas/safetext.h"
#include "daemon/daemon_internal.h"

/* The same monotonic clock the watcher and the serve loop use. Wall-clock time
 * is evidence about when something happened; this is only used for durations
 * an operator reads, so it must not move when the system clock does. */
static int64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

/* A starting id no previous daemon can have reached.
 *
 * Wall-clock milliseconds, because the question is "has any earlier daemon
 * issued this?" and only real time separates two runs of the process. The
 * in-process floor covers the case real time cannot: two tables created inside
 * the same millisecond, which is what a test does and what a fast restart could
 * do. Together they make an id strictly increasing across every table this
 * machine will create.
 *
 * The value is not a timestamp and nothing reads it as one. It is a starting
 * point chosen so that an id from a previous daemon lands below this one's base
 * and is answered "unknown" — which is true, and which a caller can act on —
 * rather than resolving to whatever now occupies that slot and handing them a
 * different operation's verdict. */
static int64_t ops_id_base(void) {
    static pthread_mutex_t base_lock = PTHREAD_MUTEX_INITIALIZER;
    static int64_t last_base = 0;
    struct timespec ts;
    int64_t ms = 0;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        ms = (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
    }
    (void)pthread_mutex_lock(&base_lock);
    /* The floor is the previous base plus every id that base could have issued,
     * not the previous base itself.
     *
     * A table issues ids from its base upwards, so after `atlas_ops_start` at
     * base B the ids B, B+1, ... are already spent. Guarding only against
     * `ms <= last_base` left the one-millisecond case wrong: a second table
     * created 1 ms later takes base B+1, which the first table had already
     * handed out — and a client polling that id is given another operation's
     * verdict, which is the confident wrong answer this whole layer exists to
     * prevent.
     *
     * Sub-millisecond creation was already covered and is why this survived: it
     * only appears when the two starts land in adjacent milliseconds, which a
     * sanitizer build makes ordinary and a release build makes rare. */
    int64_t floor = last_base + (int64_t)ATLAS_OPS_MAX_RECORDS + 1;
    if (ms <= floor) {
        ms = floor;
    }
    last_base = ms;
    (void)pthread_mutex_unlock(&base_lock);
    return ms > 0 ? ms : 1;
}

const char *atlas_op_state_name(atlas_op_state s) {
    switch (s) {
    case ATLAS_OP_UNKNOWN: return "UNKNOWN";
    case ATLAS_OP_RUNNING: return "RUNNING";
    case ATLAS_OP_SUCCEEDED: return "SUCCEEDED";
    case ATLAS_OP_FAILED: return "FAILED";
    }
    return "UNKNOWN";
}

bool atlas_op_state_is_terminal(atlas_op_state s) {
    return s == ATLAS_OP_SUCCEEDED || s == ATLAS_OP_FAILED;
}

const char *atlas_op_kind_name(atlas_op_kind k) {
    switch (k) {
    case ATLAS_OP_KIND_UNKNOWN: return "UNKNOWN";
    case ATLAS_OP_KIND_BACKUP_CREATE: return "backup.create";
    case ATLAS_OP_KIND_BACKUP_VERIFY: return "backup.verify";
    case ATLAS_OP_KIND_SEM_INDEX: return "code.index";
    }
    return "UNKNOWN";
}

void atlas_op_record_init(atlas_op_record *r) {
    memset(r, 0, sizeof(*r));
    atlas_buf_init(&r->message);
    atlas_buf_init(&r->detail);
    atlas_backup_verify_report_init(&r->verify);
}

void atlas_op_record_free(atlas_op_record *r) {
    if (r == NULL) {
        return;
    }
    atlas_buf_free(&r->message);
    atlas_buf_free(&r->detail);
    atlas_backup_verify_report_free(&r->verify);
}

/* --- the table ------------------------------------------------------------
 *
 * A fixed ring of records. Bounded like everything else in Atlas, and the
 * bound is reported rather than silently applied: once the ring wraps, the
 * oldest record's id becomes unknown, and `operation.get` says unknown. That is
 * the same answer a restart gives, so a client already has to handle it. */

typedef struct op_slot {
    int64_t id; /* 0 when the slot has never been used */
    atlas_op_kind kind;
    atlas_op_state state;
    int64_t repo_id;
    int64_t started_at_ms;
    int64_t finished_at_ms;
    atlas_status result;
    atlas_buf message;
    atlas_buf detail;
    /* See atlas_op_record: the operation's typed result. */
    int64_t size_bytes;
    int64_t page_size;
    int64_t page_count;
    int schema_version;
    bool source_online;
    char sha256[65];
    char atlas_version[32];
    atlas_backup_verify_report verify;
} op_slot;

/* What the operation thread has been handed. Only backups run here; a semantic
 * index runs on the writer thread, because it writes the index. */
typedef struct op_work {
    bool present;
    int64_t id;
    atlas_op_kind kind;
    atlas_buf name;
    bool force;
} op_work;

struct atlas_ops {
    pthread_t thread;
    bool thread_started;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t idle;

    /* --- guarded by lock --- */
    bool stopping;
    int64_t next_id;
    op_slot slots[ATLAS_OPS_MAX_RECORDS];
    size_t next_slot;
    op_work work;
    bool working;

    atlas_buf data_dir;
    FILE *log;
};

static op_slot *slot_find(atlas_ops *ops, int64_t id) {
    for (size_t i = 0; i < ATLAS_OPS_MAX_RECORDS; i++) {
        if (ops->slots[i].id == id && id != 0) {
            return &ops->slots[i];
        }
    }
    return NULL;
}

/* True when a record of this kind, for this repository, is not finished.
 * `repo_id` of -1 means "any". */
static const op_slot *slot_in_flight(atlas_ops *ops, atlas_op_kind kind, int64_t repo_id) {
    for (size_t i = 0; i < ATLAS_OPS_MAX_RECORDS; i++) {
        const op_slot *s = &ops->slots[i];
        if (s->id == 0 || s->kind != kind || atlas_op_state_is_terminal(s->state)) {
            continue;
        }
        if (repo_id < 0 || s->repo_id == repo_id) {
            return s;
        }
    }
    return NULL;
}

/* Claims the next slot. Must hold the lock. */
static op_slot *slot_claim(atlas_ops *ops, atlas_op_kind kind, int64_t repo_id) {
    op_slot *s = &ops->slots[ops->next_slot];
    ops->next_slot = (ops->next_slot + 1u) % ATLAS_OPS_MAX_RECORDS;
    atlas_buf_reset(&s->message);
    atlas_buf_reset(&s->detail);
    s->id = ops->next_id++;
    s->kind = kind;
    s->state = ATLAS_OP_RUNNING;
    s->repo_id = repo_id;
    s->started_at_ms = now_ms();
    s->finished_at_ms = 0;
    s->result = ATLAS_OK;
    s->size_bytes = 0;
    s->page_size = 0;
    s->page_count = 0;
    s->schema_version = 0;
    s->source_online = false;
    s->sha256[0] = '\0';
    s->atlas_version[0] = '\0';
    return s;
}

/* --- the operation thread ------------------------------------------------- */

/* Records a backup's typed result. Separate from `atlas_ops_finish` so the
 * terminal transition stays the one place a state changes. */
static void ops_set_backup_result(atlas_ops *ops, int64_t id, const atlas_backup_report *rep) {
    (void)pthread_mutex_lock(&ops->lock);
    op_slot *s = slot_find(ops, id);
    if (s != NULL && !atlas_op_state_is_terminal(s->state)) {
        s->size_bytes = rep->size_bytes;
        s->page_size = rep->page_size;
        s->page_count = rep->page_count;
        s->schema_version = rep->schema_version;
        s->source_online = rep->source_online;
        (void)snprintf(s->sha256, sizeof s->sha256, "%s", rep->sha256);
        (void)snprintf(s->atlas_version, sizeof s->atlas_version, "%s", rep->atlas_version);
    }
    (void)pthread_mutex_unlock(&ops->lock);
}


static void run_backup(atlas_ops *ops, int64_t id, const char *name, bool force) {
    atlas_backup_report rep;
    atlas_backup_report_init(&rep);
    atlas_backup_create_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.output = name;
    opts.force = force;

    atlas_err err;
    atlas_err_init(&err);
    atlas_status st =
        atlas_service_backup_create(atlas_buf_cstr(&ops->data_dir), &opts, &rep, &err);

    /* Verified here, before the operation is allowed to reach SUCCEEDED.
     *
     * `atlas_service_backup_create` already verifies in full before it
     * publishes, so this is the second reading of the published file rather
     * than the first of anything. It stays because the promise the operation
     * makes is stronger than the promise create makes: a poll that answered
     * SUCCEEDED would be the last thing anybody checked, and an unverified
     * SUCCEEDED is exactly the artefact nobody discovers until they need it. */
    atlas_backup_verify_report vrep;
    atlas_backup_verify_report_init(&vrep);
    if (st == ATLAS_OK) {
        st = atlas_service_backup_verify(atlas_buf_cstr(&rep.path), &vrep, &err);
    }
    if (st == ATLAS_OK && !vrep.ok) {
        st = atlas_err_set(&err, ATLAS_ERR_INTEGRITY,
                           "the backup was written but did not verify (%s)",
                           atlas_backup_verdict_name(vrep.verdict));
    }

    /* The detail is Atlas' own summary of its own artefact — a digest, a byte
     * count and a schema number. Nothing here came from a repository. */
    atlas_buf detail = ATLAS_BUF_INIT;
    atlas_err derr;
    atlas_err_init(&derr);
    if (st == ATLAS_OK) {
        (void)atlas_buf_appendf(&detail, &derr, "sha256=%s bytes=%lld schema=%d verify=%s",
                                rep.sha256, (long long)rep.size_bytes, rep.schema_version,
                                atlas_backup_verdict_name(vrep.verdict));
    }
    if (st == ATLAS_OK) {
        ops_set_backup_result(ops, id, &rep);
    }
    atlas_ops_finish(ops, id, st,
                     st == ATLAS_OK ? "backup created and verified" : atlas_err_msg(&err),
                     atlas_buf_cstr(&detail));
    atlas_buf_free(&detail);
    atlas_backup_verify_report_free(&vrep);
    atlas_backup_report_free(&rep);
}

/* Verifies one existing backup and records the whole report.
 *
 * The report is stored typed rather than summarised into a string: a caller
 * asked whether the backup is usable, and "19 of 19 tables, 73 revisions
 * rehashed, 0 mismatches" is the answer. Flattening it to prose would be the
 * same mistake as sending only some of its fields over the socket, which
 * understated a check for a whole phase. */
static void run_verify(atlas_ops *ops, int64_t id, const char *path) {
    atlas_backup_verify_report rep;
    atlas_backup_verify_report_init(&rep);
    atlas_err err;
    atlas_err_init(&err);
    atlas_status st = atlas_service_backup_verify(path, &rep, &err);

    atlas_buf detail = ATLAS_BUF_INIT;
    atlas_err derr;
    atlas_err_init(&derr);
    if (st == ATLAS_OK) {
        (void)atlas_buf_appendf(&detail, &derr, "verdict=%s tables=%lld/%lld revisions=%lld",
                                atlas_backup_verdict_name(rep.verdict),
                                (long long)rep.tables_present, (long long)rep.tables_required,
                                (long long)rep.revisions_rehashed);
        (void)pthread_mutex_lock(&ops->lock);
        op_slot *sl = slot_find(ops, id);
        if (sl != NULL && !atlas_op_state_is_terminal(sl->state)) {
            atlas_backup_verify_report_free(&sl->verify);
            sl->verify = rep;
            atlas_backup_verify_report_init(&rep);
        }
        (void)pthread_mutex_unlock(&ops->lock);
    }
    /* A backup that verifies badly is a *successful* verification with a bad
     * verdict, not a failed operation: the question was answered. Only an
     * inability to answer is a failure. */
    atlas_ops_finish(ops, id, st, st == ATLAS_OK ? "backup verified" : atlas_err_msg(&err),
                     atlas_buf_cstr(&detail));
    atlas_buf_free(&detail);
    atlas_backup_verify_report_free(&rep);
}

static void *ops_main(void *ud) {
    atlas_ops *ops = (atlas_ops *)ud;
    for (;;) {
        (void)pthread_mutex_lock(&ops->lock);
        while (!ops->stopping && !ops->work.present) {
            (void)pthread_cond_wait(&ops->not_empty, &ops->lock);
        }
        if (ops->stopping && !ops->work.present) {
            (void)pthread_mutex_unlock(&ops->lock);
            break;
        }
        int64_t id = ops->work.id;
        bool force = ops->work.force;
        atlas_op_kind kind = ops->work.kind;
        atlas_buf name = ATLAS_BUF_INIT;
        atlas_err nerr;
        atlas_err_init(&nerr);
        (void)atlas_buf_append_str(&name, atlas_buf_cstr(&ops->work.name), &nerr);
        ops->work.present = false;
        ops->working = true;
        (void)pthread_mutex_unlock(&ops->lock);

        if (kind == ATLAS_OP_KIND_BACKUP_VERIFY) {
            run_verify(ops, id, atlas_buf_cstr(&name));
        } else {
            run_backup(ops, id, atlas_buf_cstr(&name), force);
        }
        atlas_buf_free(&name);

        (void)pthread_mutex_lock(&ops->lock);
        ops->working = false;
        (void)pthread_cond_broadcast(&ops->idle);
        (void)pthread_mutex_unlock(&ops->lock);
    }
    return NULL;
}

/* --- lifecycle ------------------------------------------------------------ */

atlas_status atlas_ops_start(const char *data_dir, FILE *log, atlas_ops **out, atlas_err *err) {
    *out = NULL;
    atlas_ops *ops = calloc(1u, sizeof(*ops));
    if (ops == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory starting the operations table");
    }
    atlas_buf_init(&ops->data_dir);
    atlas_buf_init(&ops->work.name);
    for (size_t i = 0; i < ATLAS_OPS_MAX_RECORDS; i++) {
        atlas_buf_init(&ops->slots[i].message);
        atlas_buf_init(&ops->slots[i].detail);
        atlas_backup_verify_report_init(&ops->slots[i].verify);
    }
    /* Ids are seeded from the wall clock at start, not from 1.
     *
     * The table is in memory and a restart forgets it, which is documented and
     * fine — but with the counter restarting at 1 the *ids* were reused, so an
     * operation id issued before a restart named a different operation
     * afterwards. A client polling it would be handed another operation's
     * verdict: not "unknown", which is what the documentation promises and what
     * a caller can act on, but a confident wrong answer about whether their
     * backup succeeded. That is the failure this whole layer exists to prevent,
     * so it must not be reintroduced by the id space.
     *
     * Seconds since the epoch is monotonic enough for the job: every id a new
     * daemon issues is larger than every id the previous one did, so an old id
     * is below the new base and is reported unknown, which is true. It is not a
     * timestamp and nothing reads it as one — it is a starting point chosen so
     * that two daemons cannot mint the same id. */
    ops->next_id = ops_id_base();
    ops->log = log;
    if (data_dir != NULL && atlas_buf_append_str(&ops->data_dir, data_dir, err) != ATLAS_OK) {
        atlas_ops_stop(ops);
        return ATLAS_ERR_INTERNAL;
    }
    if (pthread_mutex_init(&ops->lock, NULL) != 0 ||
        pthread_cond_init(&ops->not_empty, NULL) != 0 || pthread_cond_init(&ops->idle, NULL) != 0) {
        atlas_ops_stop(ops);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "could not initialise the operations table");
    }
    if (pthread_create(&ops->thread, NULL, ops_main, ops) != 0) {
        atlas_ops_stop(ops);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "could not start the operations thread");
    }
    ops->thread_started = true;
    *out = ops;
    return ATLAS_OK;
}

void atlas_ops_stop(atlas_ops *ops) {
    if (ops == NULL) {
        return;
    }
    if (ops->thread_started) {
        (void)pthread_mutex_lock(&ops->lock);
        ops->stopping = true;
        (void)pthread_cond_broadcast(&ops->not_empty);
        (void)pthread_mutex_unlock(&ops->lock);
        (void)pthread_join(ops->thread, NULL);
        ops->thread_started = false;
    }
    for (size_t i = 0; i < ATLAS_OPS_MAX_RECORDS; i++) {
        atlas_buf_free(&ops->slots[i].message);
        atlas_buf_free(&ops->slots[i].detail);
        atlas_backup_verify_report_free(&ops->slots[i].verify);
    }
    atlas_buf_free(&ops->work.name);
    atlas_buf_free(&ops->data_dir);
    free(ops);
}

/* --- submission ----------------------------------------------------------- */

atlas_status atlas_ops_submit_backup(atlas_ops *ops, const char *name, bool force, int64_t *id_out,
                                     atlas_err *err) {
    if (ops == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "the operations table is not running");
    }
    atlas_status st = ATLAS_OK;
    (void)pthread_mutex_lock(&ops->lock);
    if (ops->stopping) {
        st = atlas_err_set(err, ATLAS_ERR_CONFIG, "the daemon is stopping");
    } else {
        const op_slot *busy = slot_in_flight(ops, ATLAS_OP_KIND_BACKUP_CREATE, -1);
        if (busy != NULL) {
            /* Deterministic refusal, naming the operation that holds the slot,
             * so the caller's next move is to poll rather than to guess. */
            st = atlas_err_set(err, ATLAS_ERR_CONFIG,
                               "a backup is already running as operation %lld; poll it with "
                               "`atlas operation status %lld` rather than starting a second one",
                               (long long)busy->id, (long long)busy->id);
        }
    }
    if (st == ATLAS_OK) {
        op_slot *s = slot_claim(ops, ATLAS_OP_KIND_BACKUP_CREATE, 0);
        atlas_buf_reset(&ops->work.name);
        st = atlas_buf_append_str(&ops->work.name, name, err);
        if (st == ATLAS_OK) {
            ops->work.present = true;
            ops->work.kind = ATLAS_OP_KIND_BACKUP_CREATE;
            ops->work.id = s->id;
            ops->work.force = force;
            *id_out = s->id;
            (void)pthread_cond_signal(&ops->not_empty);
        } else {
            s->id = 0;
        }
    }
    (void)pthread_mutex_unlock(&ops->lock);
    return st;
}

atlas_status atlas_ops_submit_verify(atlas_ops *ops, const char *path, int64_t *id_out,
                                     atlas_err *err) {
    if (ops == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "the operations table is not running");
    }
    atlas_status st = ATLAS_OK;
    (void)pthread_mutex_lock(&ops->lock);
    if (ops->stopping) {
        st = atlas_err_set(err, ATLAS_ERR_CONFIG, "the daemon is stopping");
    } else if (ops->work.present || ops->working) {
        /* One operation thread, so a verification queued behind a backup would
         * wait without saying so. Refusing names what is in the way. */
        st = atlas_err_set(err, ATLAS_ERR_CONFIG,
                           "another backup operation is already running; poll it with "
                           "`atlas operation status ID` and try again when it finishes");
    }
    if (st == ATLAS_OK) {
        op_slot *s = slot_claim(ops, ATLAS_OP_KIND_BACKUP_VERIFY, 0);
        atlas_buf_reset(&ops->work.name);
        st = atlas_buf_append_str(&ops->work.name, path, err);
        if (st == ATLAS_OK) {
            ops->work.present = true;
            ops->work.kind = ATLAS_OP_KIND_BACKUP_VERIFY;
            ops->work.id = s->id;
            ops->work.force = false;
            *id_out = s->id;
            (void)pthread_cond_signal(&ops->not_empty);
        } else {
            s->id = 0;
        }
    }
    (void)pthread_mutex_unlock(&ops->lock);
    return st;
}

atlas_status atlas_ops_begin_sem_index(atlas_ops *ops, int64_t repo_id, int64_t *id_out,
                                       atlas_err *err) {
    if (ops == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "the operations table is not running");
    }
    atlas_status st = ATLAS_OK;
    (void)pthread_mutex_lock(&ops->lock);
    if (ops->stopping) {
        st = atlas_err_set(err, ATLAS_ERR_CONFIG, "the daemon is stopping");
    } else {
        const op_slot *busy = slot_in_flight(ops, ATLAS_OP_KIND_SEM_INDEX, repo_id);
        if (busy != NULL) {
            st = atlas_err_set(err, ATLAS_ERR_CONFIG,
                               "a semantic index of this repository is already running as "
                               "operation %lld; poll it with `atlas operation status %lld`",
                               (long long)busy->id, (long long)busy->id);
        }
    }
    if (st == ATLAS_OK) {
        op_slot *s = slot_claim(ops, ATLAS_OP_KIND_SEM_INDEX, repo_id);
        *id_out = s->id;
    }
    (void)pthread_mutex_unlock(&ops->lock);
    return st;
}

void atlas_ops_finish(atlas_ops *ops, int64_t id, atlas_status result, const char *message,
                      const char *detail) {
    if (ops == NULL) {
        return;
    }
    (void)pthread_mutex_lock(&ops->lock);
    op_slot *s = slot_find(ops, id);
    /* Already terminal is not an error. This is reachable from an error path
     * that can itself run twice, and a second transition must not rewrite the
     * first answer — that is the whole idempotency claim. */
    if (s != NULL && !atlas_op_state_is_terminal(s->state)) {
        s->state = result == ATLAS_OK ? ATLAS_OP_SUCCEEDED : ATLAS_OP_FAILED;
        s->result = result;
        s->finished_at_ms = now_ms();
        atlas_err werr;
        atlas_err_init(&werr);
        if (message != NULL) {
            (void)atlas_buf_append_str(&s->message, message, &werr);
        }
        if (detail != NULL) {
            (void)atlas_buf_append_str(&s->detail, detail, &werr);
        }
        if (ops->log != NULL) {
            atlas_safe_pool safe;
            atlas_safe_pool_init(&safe);
            atlas_daemon_log(ops->log, result == ATLAS_OK ? "info" : "warn",
                             "operation %lld (%s) %s: %s", (long long)id,
                             atlas_op_kind_name(s->kind), atlas_op_state_name(s->state),
                             atlas_safe(&safe, message != NULL ? message : ""));
            atlas_safe_pool_free(&safe);
        }
    }
    (void)pthread_mutex_unlock(&ops->lock);
}

atlas_status atlas_ops_get(atlas_ops *ops, int64_t id, atlas_op_record *out, atlas_err *err) {
    if (ops == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "the operations table is not running");
    }
    atlas_status st = ATLAS_OK;
    (void)pthread_mutex_lock(&ops->lock);
    const op_slot *s = slot_find(ops, id);
    if (s == NULL) {
        /* Unknown covers three situations deliberately: it was never issued,
         * the ring wrapped past it, or the daemon restarted. All three mean
         * "Atlas cannot tell you about that operation", and distinguishing them
         * would be inventing a history the table does not keep. The artefact —
         * the backup file, the semantic generation — is what to ask about
         * instead, and it is durable. */
        st = atlas_err_set(err, ATLAS_ERR_REPO,
                           "no operation %lld is known to this daemon. Operation records live in "
                           "memory and do not survive a restart; ask about the artefact instead "
                           "(`atlas backup verify NAME`, `atlas code sem-status REPO`)",
                           (long long)id);
    } else {
        out->id = s->id;
        out->kind = s->kind;
        out->state = s->state;
        out->repo_id = s->repo_id;
        out->started_at_ms = s->started_at_ms;
        out->finished_at_ms = s->finished_at_ms;
        out->result = s->result;
        out->size_bytes = s->size_bytes;
        out->page_size = s->page_size;
        out->page_count = s->page_count;
        out->schema_version = s->schema_version;
        out->source_online = s->source_online;
        (void)snprintf(out->sha256, sizeof out->sha256, "%s", s->sha256);
        (void)snprintf(out->atlas_version, sizeof out->atlas_version, "%s", s->atlas_version);
        if (s->kind == ATLAS_OP_KIND_BACKUP_VERIFY) {
            /* Copied field by field: the report owns buffers and a struct
             * assignment would hand the caller pointers into the table. */
            out->verify.verdict = s->verify.verdict;
            out->verify.ok = s->verify.ok;
            out->verify.size_bytes = s->verify.size_bytes;
            (void)snprintf(out->verify.sha256, sizeof out->verify.sha256, "%s", s->verify.sha256);
            out->verify.schema_version = s->verify.schema_version;
            out->verify.expected_schema_version = s->verify.expected_schema_version;
            out->verify.tables_required = s->verify.tables_required;
            out->verify.tables_present = s->verify.tables_present;
            out->verify.repo_count = s->verify.repo_count;
            out->verify.revisions_checked = s->verify.revisions_checked;
            out->verify.revisions_rehashed = s->verify.revisions_rehashed;
            out->verify.revisions_corrupt = s->verify.revisions_corrupt;
            out->verify.ledger_mismatched = s->verify.ledger_mismatched;
            (void)atlas_buf_set_str(&out->verify.integrity,
                                    atlas_buf_cstr(&s->verify.integrity), err);
            (void)atlas_buf_set_str(&out->verify.foreign_key_check,
                                    atlas_buf_cstr(&s->verify.foreign_key_check), err);
            (void)atlas_buf_set_str(&out->verify.missing_tables,
                                    atlas_buf_cstr(&s->verify.missing_tables), err);
            (void)atlas_buf_set_str(&out->verify.problems,
                                    atlas_buf_cstr(&s->verify.problems), err);
        }
        atlas_buf_reset(&out->message);
        atlas_buf_reset(&out->detail);
        st = atlas_buf_append_str(&out->message, atlas_buf_cstr(&s->message), err);
        if (st == ATLAS_OK) {
            st = atlas_buf_append_str(&out->detail, atlas_buf_cstr(&s->detail), err);
        }
    }
    (void)pthread_mutex_unlock(&ops->lock);
    return st;
}

bool atlas_ops_is_running(atlas_ops *ops, int64_t id) {
    if (ops == NULL) {
        return false;
    }
    (void)pthread_mutex_lock(&ops->lock);
    const op_slot *s = slot_find(ops, id);
    bool running = s != NULL && s->state == ATLAS_OP_RUNNING;
    (void)pthread_mutex_unlock(&ops->lock);
    return running;
}
