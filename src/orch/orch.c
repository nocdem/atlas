/* Atlas - A8: the orchestration vocabularies, canonical identity and state machine.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See atlas/orch.h for what A8 claims and what it deliberately does not.
 *
 * Three things live here and nowhere else:
 *
 *   1. The closed vocabularies, each with its unknown value at zero.
 *   2. `atlas_orch_transition_allowed`, the single authority on what state may
 *      follow what. It is a function rather than a table a test can copy.
 *   3. The canonical specification encoding and its digest, domain-separated
 *      and length-prefixed for A4's reason exactly.
 */
#define _GNU_SOURCE 1

#include "atlas/orch.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

#include "atlas/sha256.h"

/* --- vocabularies --------------------------------------------------------- */

const char *atlas_orch_state_name(atlas_orch_state s) {
    switch (s) {
    case ATLAS_ORCH_STATE_UNKNOWN: return "UNKNOWN";
    case ATLAS_ORCH_STATE_QUEUED: return "QUEUED";
    case ATLAS_ORCH_STATE_LEASED: return "LEASED";
    case ATLAS_ORCH_STATE_PREPARING: return "PREPARING";
    case ATLAS_ORCH_STATE_RUNNING: return "RUNNING";
    case ATLAS_ORCH_STATE_VALIDATING: return "VALIDATING";
    case ATLAS_ORCH_STATE_SUCCEEDED: return "SUCCEEDED";
    case ATLAS_ORCH_STATE_FAILED: return "FAILED";
    case ATLAS_ORCH_STATE_CANCEL_REQUESTED: return "CANCEL_REQUESTED";
    case ATLAS_ORCH_STATE_CANCELLED: return "CANCELLED";
    case ATLAS_ORCH_STATE_TIMED_OUT: return "TIMED_OUT";
    case ATLAS_ORCH_STATE_RECOVERY_REQUIRED: return "RECOVERY_REQUIRED";
    }
    return "UNKNOWN";
}

bool atlas_orch_state_parse(const char *name, atlas_orch_state *out) {
    static const atlas_orch_state ALL[] = {
        ATLAS_ORCH_STATE_QUEUED,           ATLAS_ORCH_STATE_LEASED,
        ATLAS_ORCH_STATE_PREPARING,        ATLAS_ORCH_STATE_RUNNING,
        ATLAS_ORCH_STATE_VALIDATING,       ATLAS_ORCH_STATE_SUCCEEDED,
        ATLAS_ORCH_STATE_FAILED,           ATLAS_ORCH_STATE_CANCEL_REQUESTED,
        ATLAS_ORCH_STATE_CANCELLED,        ATLAS_ORCH_STATE_TIMED_OUT,
        ATLAS_ORCH_STATE_RECOVERY_REQUIRED};
    if (name == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof ALL / sizeof ALL[0]; i++) {
        if (strcmp(name, atlas_orch_state_name(ALL[i])) == 0) {
            *out = ALL[i];
            return true;
        }
    }
    /* "UNKNOWN" is deliberately not parseable. Nothing may ask for a job to be
     * put into the state that means "nobody wrote this row". */
    return false;
}

bool atlas_orch_state_is_terminal(atlas_orch_state s) {
    switch (s) {
    case ATLAS_ORCH_STATE_SUCCEEDED:
    case ATLAS_ORCH_STATE_FAILED:
    case ATLAS_ORCH_STATE_CANCELLED:
    case ATLAS_ORCH_STATE_TIMED_OUT:
    case ATLAS_ORCH_STATE_RECOVERY_REQUIRED: return true;
    case ATLAS_ORCH_STATE_UNKNOWN:
    case ATLAS_ORCH_STATE_QUEUED:
    case ATLAS_ORCH_STATE_LEASED:
    case ATLAS_ORCH_STATE_PREPARING:
    case ATLAS_ORCH_STATE_RUNNING:
    case ATLAS_ORCH_STATE_VALIDATING:
    case ATLAS_ORCH_STATE_CANCEL_REQUESTED: return false;
    }
    return false;
}

bool atlas_orch_state_is_active(atlas_orch_state s) {
    switch (s) {
    case ATLAS_ORCH_STATE_LEASED:
    case ATLAS_ORCH_STATE_PREPARING:
    case ATLAS_ORCH_STATE_RUNNING:
    case ATLAS_ORCH_STATE_VALIDATING:
    case ATLAS_ORCH_STATE_CANCEL_REQUESTED: return true;
    case ATLAS_ORCH_STATE_UNKNOWN:
    case ATLAS_ORCH_STATE_QUEUED:
    case ATLAS_ORCH_STATE_SUCCEEDED:
    case ATLAS_ORCH_STATE_FAILED:
    case ATLAS_ORCH_STATE_CANCELLED:
    case ATLAS_ORCH_STATE_TIMED_OUT:
    case ATLAS_ORCH_STATE_RECOVERY_REQUIRED: return false;
    }
    return false;
}

bool atlas_orch_transition_allowed(atlas_orch_state from, atlas_orch_state to) {
    /* Nothing leaves UNKNOWN and nothing enters it. A row in that state is a
     * row nobody wrote correctly, and the only safe handling is to refuse to
     * move it rather than to guess where it was going. */
    if (from == ATLAS_ORCH_STATE_UNKNOWN || to == ATLAS_ORCH_STATE_UNKNOWN) {
        return false;
    }
    /* Terminal is terminal. Checked once, here, rather than at each call site,
     * so "a completed job stays completed" is a property of the machine. */
    if (atlas_orch_state_is_terminal(from)) {
        return false;
    }
    if (from == to) {
        return false; /* a transition is a change; a no-op write is not one */
    }

    switch (from) {
    case ATLAS_ORCH_STATE_QUEUED:
        /* A queued job has no attempt running, so cancellation is immediate and
         * there is nothing to ask to stop. TIMED_OUT covers a job that sat in
         * the queue past its wall bound. FAILED covers a policy refusal noticed
         * at lease time — the policy may change between submission and lease. */
        return to == ATLAS_ORCH_STATE_LEASED || to == ATLAS_ORCH_STATE_CANCELLED ||
               to == ATLAS_ORCH_STATE_TIMED_OUT || to == ATLAS_ORCH_STATE_FAILED ||
               to == ATLAS_ORCH_STATE_RECOVERY_REQUIRED;

    case ATLAS_ORCH_STATE_LEASED:
    case ATLAS_ORCH_STATE_PREPARING:
    case ATLAS_ORCH_STATE_RUNNING:
    case ATLAS_ORCH_STATE_VALIDATING: {
        /* The forward edge of the pipeline, plus every way out of it. */
        if (from == ATLAS_ORCH_STATE_LEASED && to == ATLAS_ORCH_STATE_PREPARING) {
            return true;
        }
        if (from == ATLAS_ORCH_STATE_PREPARING && to == ATLAS_ORCH_STATE_RUNNING) {
            return true;
        }
        if (from == ATLAS_ORCH_STATE_RUNNING && to == ATLAS_ORCH_STATE_VALIDATING) {
            return true;
        }
        /* A job with no declared validation commands has nothing to validate,
         * so RUNNING may complete directly. VALIDATING may complete because
         * that is what it is for. Nothing earlier may: a job cannot succeed
         * before its driver has run. */
        if (to == ATLAS_ORCH_STATE_SUCCEEDED) {
            return from == ATLAS_ORCH_STATE_RUNNING || from == ATLAS_ORCH_STATE_VALIDATING;
        }
        /* Retry returns to the queue for a *new* attempt. The attempt count is
         * checked by the caller that has the row; this says only that the edge
         * exists. */
        if (to == ATLAS_ORCH_STATE_QUEUED) {
            return true;
        }
        return to == ATLAS_ORCH_STATE_FAILED || to == ATLAS_ORCH_STATE_CANCEL_REQUESTED ||
               to == ATLAS_ORCH_STATE_TIMED_OUT || to == ATLAS_ORCH_STATE_RECOVERY_REQUIRED;
    }

    case ATLAS_ORCH_STATE_CANCEL_REQUESTED:
        /* Cancellation wins, deterministically. Once an operator has asked for
         * it there is no edge back into the pipeline and no edge to SUCCEEDED:
         * a completion that arrives afterwards is refused with a typed reason
         * rather than racing the cancellation. "Completion and cancellation
         * cannot both win" is this line.
         *
         * TIMED_OUT and RECOVERY_REQUIRED remain reachable because a worker
         * that never acknowledges the request still has to end somewhere, and
         * neither of those is a success. */
        return to == ATLAS_ORCH_STATE_CANCELLED || to == ATLAS_ORCH_STATE_TIMED_OUT ||
               to == ATLAS_ORCH_STATE_RECOVERY_REQUIRED;

    case ATLAS_ORCH_STATE_UNKNOWN:
    case ATLAS_ORCH_STATE_SUCCEEDED:
    case ATLAS_ORCH_STATE_FAILED:
    case ATLAS_ORCH_STATE_CANCELLED:
    case ATLAS_ORCH_STATE_TIMED_OUT:
    case ATLAS_ORCH_STATE_RECOVERY_REQUIRED: return false;
    }
    return false;
}

const char *atlas_orch_reason_name(atlas_orch_reason r) {
    switch (r) {
    case ATLAS_ORCH_REASON_UNKNOWN: return "UNKNOWN";
    case ATLAS_ORCH_REASON_SUBMITTED: return "SUBMITTED";
    case ATLAS_ORCH_REASON_LEASE_GRANTED: return "LEASE_GRANTED";
    case ATLAS_ORCH_REASON_WORKER_PROGRESS: return "WORKER_PROGRESS";
    case ATLAS_ORCH_REASON_WORKER_SUCCESS: return "WORKER_SUCCESS";
    case ATLAS_ORCH_REASON_WORKER_FAILURE: return "WORKER_FAILURE";
    case ATLAS_ORCH_REASON_VALIDATION_FAILED: return "VALIDATION_FAILED";
    case ATLAS_ORCH_REASON_LEASE_EXPIRED: return "LEASE_EXPIRED";
    case ATLAS_ORCH_REASON_WALL_TIMEOUT: return "WALL_TIMEOUT";
    case ATLAS_ORCH_REASON_IDLE_TIMEOUT: return "IDLE_TIMEOUT";
    case ATLAS_ORCH_REASON_CANCEL_REQUESTED: return "CANCEL_REQUESTED";
    case ATLAS_ORCH_REASON_CANCEL_CONFIRMED: return "CANCEL_CONFIRMED";
    case ATLAS_ORCH_REASON_RETRY: return "RETRY";
    case ATLAS_ORCH_REASON_ATTEMPTS_EXHAUSTED: return "ATTEMPTS_EXHAUSTED";
    case ATLAS_ORCH_REASON_RECOVERY_AMBIGUOUS: return "RECOVERY_AMBIGUOUS";
    case ATLAS_ORCH_REASON_POLICY_REFUSED: return "POLICY_REFUSED";
    case ATLAS_ORCH_REASON_ENVELOPE_INVALID: return "ENVELOPE_INVALID";
    }
    return "UNKNOWN";
}

const char *atlas_orch_actor_name(atlas_orch_actor a) {
    switch (a) {
    case ATLAS_ORCH_ACTOR_UNKNOWN: return "UNKNOWN";
    case ATLAS_ORCH_ACTOR_CLIENT: return "CLIENT";
    case ATLAS_ORCH_ACTOR_DISPATCHER: return "DISPATCHER";
    case ATLAS_ORCH_ACTOR_ATLAS: return "ATLAS";
    }
    return "UNKNOWN";
}

const char *atlas_orch_exit_kind_name(atlas_orch_exit_kind k) {
    switch (k) {
    case ATLAS_ORCH_EXIT_UNKNOWN: return "UNKNOWN";
    case ATLAS_ORCH_EXIT_OK: return "OK";
    case ATLAS_ORCH_EXIT_NONZERO: return "NONZERO";
    case ATLAS_ORCH_EXIT_SIGNALLED: return "SIGNALLED";
    case ATLAS_ORCH_EXIT_TIMEOUT: return "TIMEOUT";
    case ATLAS_ORCH_EXIT_CANCELLED: return "CANCELLED";
    case ATLAS_ORCH_EXIT_SPAWN_FAILED: return "SPAWN_FAILED";
    case ATLAS_ORCH_EXIT_MALFORMED_RESULT: return "MALFORMED_RESULT";
    }
    return "UNKNOWN";
}

/* --- argv vectors --------------------------------------------------------- */

void atlas_orch_argv_init(atlas_orch_argv *a) {
    memset(a, 0, sizeof(*a));
    for (size_t i = 0; i < ATLAS_ORCH_MAX_ARGV; i++) {
        atlas_buf_init(&a->args[i]);
    }
}

void atlas_orch_argv_free(atlas_orch_argv *a) {
    if (a == NULL) {
        return;
    }
    for (size_t i = 0; i < ATLAS_ORCH_MAX_ARGV; i++) {
        atlas_buf_free(&a->args[i]);
    }
    a->count = 0;
}

atlas_status atlas_orch_argv_push(atlas_orch_argv *a, const char *arg, size_t len,
                                  atlas_err *err) {
    if (a->count >= ATLAS_ORCH_MAX_ARGV) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a validation command may have at most %u arguments",
                             (unsigned)ATLAS_ORCH_MAX_ARGV);
    }
    if (len > ATLAS_ORCH_ARG_MAX) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a validation argument may be at most %u bytes",
                             (unsigned)ATLAS_ORCH_ARG_MAX);
    }
    if (len == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a validation argument may not be empty");
    }
    /* NUL is refused rather than truncated at. An argv element is delimited by
     * NUL when it reaches execve, so a value containing one is a value that
     * would arrive at the child as something shorter than what was validated. */
    if (memchr(arg, '\0', len) != NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a validation argument may not contain a NUL byte");
    }
    /* Printable ASCII only. This is not an escaping decision — nothing is
     * escaped, because nothing is ever handed to a shell. It keeps the stored
     * canonical form readable and keeps a control byte out of a log line. */
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)arg[i];
        if (c < 0x20u || c >= 0x7fu) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "a validation argument must be printable ASCII");
        }
    }
    atlas_status st = atlas_buf_set(&a->args[a->count], arg, len, err);
    if (st != ATLAS_OK) {
        return st;
    }
    a->count++;
    return ATLAS_OK;
}

/* See atlas/orch.h. A12.0 lifted this verbatim out of `split_words` in
 * `src/core/service_orch.c`, which is now a caller: the `atlas-plan-1` parser
 * needs the same split for its `gate:` lines, and a planner's gate must be
 * split exactly as an operator's `--gate` is or the allowlist is applied to a
 * different argv[0] than the one that runs. */
atlas_status atlas_orch_gate_split(const char *line, atlas_orch_argv *out, atlas_err *err) {
    const char *p = line;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        const char *start = p;
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            p++;
        }
        if (p > start) {
            atlas_status st = atlas_orch_argv_push(out, start, (size_t)(p - start), err);
            if (st != ATLAS_OK) {
                return st;
            }
        }
    }
    if (out->count == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a gate needs a command");
    }
    return ATLAS_OK;
}

/* --- the specification ---------------------------------------------------- */

void atlas_orch_spec_init(atlas_orch_spec *s) {
    memset(s, 0, sizeof(*s));
    s->spec_version = ATLAS_ORCH_SPEC_VERSION;
    atlas_buf_init(&s->repo_name);
    atlas_buf_init(&s->repo_identity_hash);
    atlas_buf_init(&s->source_commit);
    atlas_buf_init(&s->mode);
    atlas_buf_init(&s->driver);
    atlas_buf_init(&s->task_text);
    atlas_buf_init(&s->correlation);
    atlas_buf_init(&s->parent_job_uid);
    atlas_buf_init(&s->idempotency_key);
    for (size_t i = 0; i < ATLAS_ORCH_MAX_ALLOWED_PATHS; i++) {
        atlas_buf_init(&s->allowed_paths[i]);
    }
    for (size_t i = 0; i < ATLAS_ORCH_MAX_VALIDATIONS; i++) {
        atlas_orch_argv_init(&s->validations[i]);
    }
}

void atlas_orch_spec_free(atlas_orch_spec *s) {
    if (s == NULL) {
        return;
    }
    atlas_buf_free(&s->repo_name);
    atlas_buf_free(&s->repo_identity_hash);
    atlas_buf_free(&s->source_commit);
    atlas_buf_free(&s->mode);
    atlas_buf_free(&s->driver);
    atlas_buf_free(&s->task_text);
    atlas_buf_free(&s->correlation);
    atlas_buf_free(&s->parent_job_uid);
    atlas_buf_free(&s->idempotency_key);
    for (size_t i = 0; i < ATLAS_ORCH_MAX_ALLOWED_PATHS; i++) {
        atlas_buf_free(&s->allowed_paths[i]);
    }
    for (size_t i = 0; i < ATLAS_ORCH_MAX_VALIDATIONS; i++) {
        atlas_orch_argv_free(&s->validations[i]);
    }
    s->allowed_path_count = 0;
    s->validation_count = 0;
}

bool atlas_orch_relpath_is_safe(const char *path, size_t len) {
    if (path == NULL || len == 0 || len > ATLAS_ORCH_PATH_MAX) {
        return false;
    }
    if (path[0] == '/') {
        return false; /* absolute: never a repository-relative prefix */
    }
    if (memchr(path, '\0', len) != NULL) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)path[i];
        /* Printable ASCII, and no backslash. Repository paths are bytes in
         * general (A0 says so and means it), but a *declared* path in a job
         * specification is a thing a submitter typed, and keeping it to a
         * narrow set is what lets it be compared, sorted and reported without
         * a decoder. A repository file whose name is not in this set simply
         * cannot be named in a declaration; it is still indexed. */
        if (c < 0x20u || c >= 0x7fu || c == '\\') {
            return false;
        }
    }
    /* No empty, `.` or `..` component, and no trailing slash. Checked
     * component by component rather than with strstr, because "/../" misses a
     * leading "../" and a trailing "/..". */
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || path[i] == '/') {
            size_t n = i - start;
            if (n == 0) {
                return false; /* empty component: leading, doubled or trailing / */
            }
            if (n == 1 && path[start] == '.') {
                return false;
            }
            if (n == 2 && path[start] == '.' && path[start + 1] == '.') {
                return false;
            }
            start = i + 1;
        }
    }
    return true;
}

static int path_cmp(const void *a, const void *b) {
    const atlas_buf *x = (const atlas_buf *)a;
    const atlas_buf *y = (const atlas_buf *)b;
    size_t n = x->len < y->len ? x->len : y->len;
    int c = n == 0 ? 0 : memcmp(x->data, y->data, n);
    if (c != 0) {
        return c;
    }
    if (x->len == y->len) {
        return 0;
    }
    return x->len < y->len ? -1 : 1;
}

atlas_status atlas_orch_spec_canonicalise(atlas_orch_spec *s, atlas_err *err) {
    for (size_t i = 0; i < s->allowed_path_count; i++) {
        if (!atlas_orch_relpath_is_safe(atlas_buf_cstr(&s->allowed_paths[i]),
                                        s->allowed_paths[i].len)) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "declared path %zu is not a safe repository-relative prefix", i);
        }
    }
    /* A set, so it is sorted and deduplicated: the same declaration submitted
     * in a different order is the same specification and must digest
     * identically. The validation list is deliberately *not* sorted — its order
     * is part of what was asked for. */
    if (s->allowed_path_count > 1) {
        qsort(s->allowed_paths, s->allowed_path_count, sizeof(s->allowed_paths[0]), path_cmp);
        size_t w = 1;
        for (size_t r = 1; r < s->allowed_path_count; r++) {
            if (path_cmp(&s->allowed_paths[w - 1], &s->allowed_paths[r]) == 0) {
                atlas_buf_free(&s->allowed_paths[r]);
                atlas_buf_init(&s->allowed_paths[r]);
                continue;
            }
            if (w != r) {
                s->allowed_paths[w] = s->allowed_paths[r];
                atlas_buf_init(&s->allowed_paths[r]);
            }
            w++;
        }
        s->allowed_path_count = w;
    }
    return ATLAS_OK;
}

static bool is_lower_hex(const char *s, size_t len, size_t want) {
    if (len != want) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

/* The one implementation of "is this a job identifier?". See `atlas/orch.h`:
 * `atlas_orch_spec_validate` asks it of a submitted parent and A12.0's
 * `plan.revision_add` asks it at the IPC edge, and a second spelling would be a
 * second answer. */
bool atlas_orch_is_job_uid(const char *s, size_t len) {
    return s != NULL && len == ATLAS_ORCH_UID_HEX + 1u && s[0] == 'j' &&
           is_lower_hex(s + 1, len - 1u, ATLAS_ORCH_UID_HEX);
}

/* A vocabulary name the policy supplies: short, lowercase, and from a set the
 * operator wrote. Checked here so a name can never carry a byte that would need
 * escaping anywhere it is later reported. */
static bool is_name(const char *s, size_t len) {
    if (len == 0 || len > ATLAS_ORCH_NAME_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                  c == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

atlas_status atlas_orch_spec_validate(const atlas_orch_spec *s, atlas_err *err) {
    if (s->spec_version != ATLAS_ORCH_SPEC_VERSION) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "job specification version %d is not supported (this Atlas speaks %d)",
                             s->spec_version, ATLAS_ORCH_SPEC_VERSION);
    }
    if (s->submitter_uid <= 0) {
        /* Never read from the request body; this catches a caller that forgot
         * to fill it in from SO_PEERCRED rather than a claim by a client. */
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "a job specification reached validation with no trusted submitter");
    }
    if (!is_name(atlas_buf_cstr(&s->repo_name), s->repo_name.len)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "the repository name is not a valid name");
    }
    if (!is_lower_hex(atlas_buf_cstr(&s->repo_identity_hash), s->repo_identity_hash.len, 64u)) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "the repository identity hash is not a 64-character digest");
    }
    /* An exact commit, resolved by the daemon before the job was persisted. A
     * branch name is refused here rather than resolved: a moving reference in a
     * stored specification is a job whose source depends on when it happens to
     * run, and the digest would then describe nothing durable. */
    if (!is_lower_hex(atlas_buf_cstr(&s->source_commit), s->source_commit.len, 40u)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "the source commit must be an exact resolved 40-character object id");
    }
    if (!is_name(atlas_buf_cstr(&s->mode), s->mode.len)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "the job mode is not a valid name");
    }
    if (!is_name(atlas_buf_cstr(&s->driver), s->driver.len)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "the driver is not a valid name");
    }
    if (s->task_text.len == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a job needs task text");
    }
    if (s->task_text.len > ATLAS_ORCH_TASK_MAX) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "task text may be at most %u bytes",
                             (unsigned)ATLAS_ORCH_TASK_MAX);
    }
    /* Task text is UNTRUSTED_DATA and stays that way. The only shape rule is
     * that it contains no NUL, because it is stored as TEXT and a NUL would
     * make the stored value shorter than the hashed one. Shell metacharacters
     * are explicitly *allowed*: nothing in A8 passes task text to a shell, and
     * refusing a dollar sign would imply the opposite. */
    if (memchr(s->task_text.data, '\0', s->task_text.len) != NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "task text may not contain a NUL byte");
    }
    if (s->allowed_path_count > ATLAS_ORCH_MAX_ALLOWED_PATHS) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a job may declare at most %u paths",
                             (unsigned)ATLAS_ORCH_MAX_ALLOWED_PATHS);
    }
    for (size_t i = 0; i < s->allowed_path_count; i++) {
        if (!atlas_orch_relpath_is_safe(atlas_buf_cstr(&s->allowed_paths[i]),
                                        s->allowed_paths[i].len)) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "declared path %zu is not a safe repository-relative prefix", i);
        }
    }
    if (s->validation_count > ATLAS_ORCH_MAX_VALIDATIONS) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a job may declare at most %u validations",
                             (unsigned)ATLAS_ORCH_MAX_VALIDATIONS);
    }
    for (size_t i = 0; i < s->validation_count; i++) {
        if (s->validations[i].count == 0) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "validation %zu has no argv", i);
        }
        /* argv[0] is resolved against a fixed allowlist by the executor, not
         * here — this only requires that it is a bare program name rather than
         * a path, so nothing in a specification can name an absolute
         * executable. */
        const atlas_buf *a0 = &s->validations[i].args[0];
        if (memchr(a0->data, '/', a0->len) != NULL) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "validation %zu names a program by path; A8 accepts a bare "
                                 "program name resolved against its own allowlist",
                                 i);
        }
    }
    /* Bounds are checked, never clamped — A5's rule. Zero means "not given" and
     * the caller has already applied the policy default by this point, so a
     * zero here is a caller that forgot. */
    if (s->wall_timeout_ms <= 0 || s->wall_timeout_ms > ATLAS_ORCH_MAX_WALL_TIMEOUT_MS) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "the wall-clock timeout must be between 1 and %d ms",
                             ATLAS_ORCH_MAX_WALL_TIMEOUT_MS);
    }
    if (s->idle_timeout_ms <= 0 || s->idle_timeout_ms > ATLAS_ORCH_MAX_IDLE_TIMEOUT_MS) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "the idle timeout must be between 1 and %d ms",
                             ATLAS_ORCH_MAX_IDLE_TIMEOUT_MS);
    }
    if (s->idle_timeout_ms > s->wall_timeout_ms) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "the idle timeout may not exceed the wall-clock timeout");
    }
    if (s->max_attempts <= 0 || s->max_attempts > ATLAS_ORCH_MAX_ATTEMPTS) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "attempts must be between 1 and %d",
                             ATLAS_ORCH_MAX_ATTEMPTS);
    }
    if (s->max_output_bytes <= 0 || s->max_output_bytes > (int64_t)ATLAS_ORCH_MAX_OUTPUT_BYTES) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "the output bound must be between 1 and %u",
                             (unsigned)ATLAS_ORCH_MAX_OUTPUT_BYTES);
    }
    if (s->max_artifact_bytes <= 0 ||
        s->max_artifact_bytes > (int64_t)ATLAS_ORCH_MAX_ARTIFACT_BYTES) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "the artifact bound must be between 1 and %u",
                             (unsigned)ATLAS_ORCH_MAX_ARTIFACT_BYTES);
    }
    if (s->max_artifact_count <= 0 || s->max_artifact_count > ATLAS_ORCH_MAX_ARTIFACT_COUNT) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "the artifact count must be between 1 and %d",
                             ATLAS_ORCH_MAX_ARTIFACT_COUNT);
    }
    if (s->correlation.len > ATLAS_ORCH_NAME_MAX ||
        (s->correlation.len > 0 && !is_name(atlas_buf_cstr(&s->correlation), s->correlation.len))) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "the correlation id is not a valid name");
    }
    if (s->parent_job_uid.len > 0 &&
        !atlas_orch_is_job_uid(atlas_buf_cstr(&s->parent_job_uid), s->parent_job_uid.len)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "the parent job id is not a job identifier");
    }
    if (s->idempotency_key.len > ATLAS_ORCH_NAME_MAX ||
        (s->idempotency_key.len > 0 &&
         !is_name(atlas_buf_cstr(&s->idempotency_key), s->idempotency_key.len))) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "the idempotency key is not a valid name");
    }
    return ATLAS_OK;
}

/* --- canonical serialisation ----------------------------------------------
 *
 * Netstring-style: `<decimal length>:<bytes>,`. Length-prefixed, so no element
 * can be confused with a delimiter no matter what it contains, and readable
 * enough that a stored row can be understood without a decoder. */

static atlas_status ns_put(atlas_buf *out, const char *data, size_t len, atlas_err *err) {
    atlas_status st = atlas_buf_appendf(out, err, "%zu:", len);
    if (st == ATLAS_OK && len > 0) {
        st = atlas_buf_append(out, data, len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_append_ch(out, ',', err);
    }
    return st;
}

/* Reads one netstring. `*pos` advances past it. */
static bool ns_take(const char *text, size_t total, size_t *pos, const char **out, size_t *len) {
    size_t i = *pos;
    size_t n = 0;
    size_t digits = 0;
    while (i < total && text[i] >= '0' && text[i] <= '9') {
        if (digits > 9) {
            return false; /* a length nobody could mean */
        }
        n = n * 10u + (size_t)(text[i] - '0');
        i++;
        digits++;
    }
    if (digits == 0 || i >= total || text[i] != ':') {
        return false;
    }
    i++;
    if (n > total - i) {
        return false;
    }
    *out = text + i;
    *len = n;
    i += n;
    if (i >= total || text[i] != ',') {
        return false;
    }
    *pos = i + 1u;
    return true;
}

atlas_status atlas_orch_paths_encode(const atlas_buf *paths, size_t count, atlas_buf *out,
                                     atlas_err *err) {
    atlas_buf_reset(out);
    atlas_status st = atlas_buf_appendf(out, err, "%zu:", count);
    for (size_t i = 0; st == ATLAS_OK && i < count; i++) {
        st = ns_put(out, paths[i].data, paths[i].len, err);
    }
    return st;
}

atlas_status atlas_orch_paths_decode(const char *text, atlas_buf *paths, size_t cap,
                                     size_t *count_out, atlas_err *err) {
    *count_out = 0;
    size_t total = strlen(text);
    size_t pos = 0;
    size_t n = 0;
    size_t digits = 0;
    while (pos < total && text[pos] >= '0' && text[pos] <= '9') {
        n = n * 10u + (size_t)(text[pos] - '0');
        pos++;
        digits++;
        if (digits > 6) {
            return atlas_err_set(err, ATLAS_ERR_DB, "malformed stored path list");
        }
    }
    if (digits == 0 || pos >= total || text[pos] != ':') {
        return atlas_err_set(err, ATLAS_ERR_DB, "malformed stored path list");
    }
    pos++;
    if (n > cap) {
        return atlas_err_set(err, ATLAS_ERR_DB, "a stored path list holds %zu entries, cap is %zu",
                             n, cap);
    }
    for (size_t i = 0; i < n; i++) {
        const char *p = NULL;
        size_t len = 0;
        if (!ns_take(text, total, &pos, &p, &len)) {
            return atlas_err_set(err, ATLAS_ERR_DB, "malformed stored path list");
        }
        atlas_status st = atlas_buf_set(&paths[i], p, len, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    *count_out = n;
    return ATLAS_OK;
}

atlas_status atlas_orch_validations_encode(const atlas_orch_argv *v, size_t count, atlas_buf *out,
                                           atlas_err *err) {
    atlas_buf_reset(out);
    atlas_status st = atlas_buf_appendf(out, err, "%zu:", count);
    for (size_t i = 0; st == ATLAS_OK && i < count; i++) {
        st = atlas_buf_appendf(out, err, "%zu:", v[i].count);
        for (size_t k = 0; st == ATLAS_OK && k < v[i].count; k++) {
            st = ns_put(out, v[i].args[k].data, v[i].args[k].len, err);
        }
    }
    return st;
}

static bool take_count(const char *text, size_t total, size_t *pos, size_t *out) {
    size_t n = 0;
    size_t digits = 0;
    while (*pos < total && text[*pos] >= '0' && text[*pos] <= '9') {
        n = n * 10u + (size_t)(text[*pos] - '0');
        (*pos)++;
        digits++;
        if (digits > 6) {
            return false;
        }
    }
    if (digits == 0 || *pos >= total || text[*pos] != ':') {
        return false;
    }
    (*pos)++;
    *out = n;
    return true;
}

atlas_status atlas_orch_validations_decode(const char *text, atlas_orch_argv *v, size_t cap,
                                           size_t *count_out, atlas_err *err) {
    *count_out = 0;
    size_t total = strlen(text);
    size_t pos = 0;
    size_t n = 0;
    if (!take_count(text, total, &pos, &n)) {
        return atlas_err_set(err, ATLAS_ERR_DB, "malformed stored validation list");
    }
    if (n > cap) {
        return atlas_err_set(err, ATLAS_ERR_DB, "a stored validation list holds %zu entries", n);
    }
    for (size_t i = 0; i < n; i++) {
        size_t argc = 0;
        if (!take_count(text, total, &pos, &argc) || argc > ATLAS_ORCH_MAX_ARGV) {
            return atlas_err_set(err, ATLAS_ERR_DB, "malformed stored validation list");
        }
        for (size_t k = 0; k < argc; k++) {
            const char *p = NULL;
            size_t len = 0;
            if (!ns_take(text, total, &pos, &p, &len)) {
                return atlas_err_set(err, ATLAS_ERR_DB, "malformed stored validation list");
            }
            atlas_status st = atlas_buf_set(&v[i].args[k], p, len, err);
            if (st != ATLAS_OK) {
                return st;
            }
        }
        v[i].count = argc;
    }
    *count_out = n;
    return ATLAS_OK;
}

/* A11.5a-R2. The daemon's record of its own unavailability. See atlas/orch.h. */
static _Atomic int64_t g_contended_until_ms = 0;

void atlas_orch_contention_note(int64_t at_ms) {
    if (at_ms > 0) {
        atomic_store_explicit(&g_contended_until_ms, at_ms, memory_order_relaxed);
    }
}

int64_t atlas_orch_contention_seen(void) {
    return atomic_load_explicit(&g_contended_until_ms, memory_order_relaxed);
}

bool atlas_orch_lease_in_grace(int64_t deadline_ms, int64_t at_ms, int64_t contended_until_ms) {
    if (contended_until_ms <= 0 || deadline_ms <= 0) {
        return false;
    }
    /* The wall deadline is the submitter's bound and outranks everything here. */
    if (at_ms >= deadline_ms) {
        return false;
    }
    int64_t since = at_ms - contended_until_ms;
    /* A stamp from the future is a clock that moved, not evidence of anything. */
    if (since < 0) {
        return false;
    }
    return since < ATLAS_ORCH_CONTENTION_GRACE_MS;
}

atlas_status atlas_orch_validation_wire_decode(const char *enc, atlas_orch_argv *out,
                                               atlas_err *err) {
    size_t got = 0;
    atlas_status st = atlas_orch_validations_decode(enc, out, 1u, &got, err);
    if (st == ATLAS_OK && got != 1u) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a gate must encode exactly one command");
    }
    return st;
}

/* --- the canonical digest --------------------------------------------------
 *
 * Domain-separated and length-prefixed, never delimited. Every field is fed as
 * its byte length followed by its bytes, so no combination of two fields can
 * encode the same byte stream as a different combination.
 *
 * What is covered and what is not is the contract; the table in
 * `docs/orchestration.md` is the authority and adding a field means adding a row
 * to it with a reason. Excluded on purpose: the job id (assigned after the
 * digest), the creation timestamp, the state, the attempt count, every lease,
 * and every derived display encoding. Those change; the request does not.
 */
static void feed(atlas_sha256 *h, const void *data, size_t len) {
    unsigned char hdr[8];
    uint64_t n = (uint64_t)len;
    for (int i = 0; i < 8; i++) {
        hdr[i] = (unsigned char)((n >> (8 * (7 - i))) & 0xffu);
    }
    atlas_sha256_update(h, hdr, sizeof(hdr));
    if (len > 0) {
        atlas_sha256_update(h, data, len);
    }
}

static void feed_buf(atlas_sha256 *h, const atlas_buf *b) { feed(h, b->data, b->len); }

static void feed_i64(atlas_sha256 *h, int64_t v) {
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
    feed(h, tmp, n > 0 ? (size_t)n : 0u);
}

atlas_status atlas_orch_spec_digest(const atlas_orch_spec *s, char out[65], atlas_err *err) {
    (void)err;
    atlas_sha256 h;
    atlas_sha256_init(&h);
    feed(&h, ATLAS_ORCH_SPEC_DOMAIN, strlen(ATLAS_ORCH_SPEC_DOMAIN));
    feed_i64(&h, s->spec_version);
    feed_i64(&h, s->submitter_uid);
    feed_buf(&h, &s->repo_name);
    feed_buf(&h, &s->repo_identity_hash);
    feed_buf(&h, &s->source_commit);
    feed_buf(&h, &s->mode);
    feed_buf(&h, &s->driver);
    feed_buf(&h, &s->task_text);
    feed_i64(&h, (int64_t)s->allowed_path_count);
    for (size_t i = 0; i < s->allowed_path_count; i++) {
        feed_buf(&h, &s->allowed_paths[i]);
    }
    feed_i64(&h, (int64_t)s->validation_count);
    for (size_t i = 0; i < s->validation_count; i++) {
        feed_i64(&h, (int64_t)s->validations[i].count);
        for (size_t k = 0; k < s->validations[i].count; k++) {
            feed_buf(&h, &s->validations[i].args[k]);
        }
    }
    feed_i64(&h, s->wall_timeout_ms);
    feed_i64(&h, s->idle_timeout_ms);
    feed_i64(&h, s->max_attempts);
    feed_i64(&h, s->max_output_bytes);
    feed_i64(&h, s->max_artifact_bytes);
    feed_i64(&h, s->max_artifact_count);
    feed_buf(&h, &s->correlation);
    feed_buf(&h, &s->parent_job_uid);
    feed_buf(&h, &s->idempotency_key);

    unsigned char digest[ATLAS_SHA256_DIGEST_LEN];
    atlas_sha256_final(&h, digest);
    atlas_hex_encode(digest, sizeof(digest), out);
    return ATLAS_OK;
}

/* --- identifiers -----------------------------------------------------------
 *
 * From the kernel's random source, and nothing weaker. There is no fallback to
 * a PID, a clock or a counter: a predictable job id is one another local process
 * can name before it exists, and a predictable lease token is one it can
 * present. A machine that cannot produce randomness refuses to create jobs,
 * which is the correct direction to fail in. */
static atlas_status random_hex(size_t nbytes, atlas_buf *out, atlas_err *err) {
    unsigned char raw[64];
    if (nbytes > sizeof(raw)) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "randomness request is too large");
    }
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "cannot open /dev/urandom");
    }
    size_t got = 0;
    while (got < nbytes) {
        ssize_t r = read(fd, raw + got, nbytes - got);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            int saved = errno;
            (void)close(fd);
            return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, saved,
                                       "cannot read /dev/urandom");
        }
        if (r == 0) {
            (void)close(fd);
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "/dev/urandom returned no bytes");
        }
        got += (size_t)r;
    }
    (void)close(fd);
    char hex[129];
    atlas_hex_encode(raw, nbytes, hex);
    return atlas_buf_set_str(out, hex, err);
}

bool atlas_orch_driver_is_repo_tree(const char *name) {
    /* Written out, in one place, rather than derived from a flag somewhere
     * else. Adding a member is a deliberate act with an entry in
     * `docs/extending.md`, because what it adds is a driver that may edit a
     * registered repository. */
    static const char *const REPO_TREE[] = {"claude-repo", "fake-repo"};
    if (name == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof REPO_TREE / sizeof REPO_TREE[0]; i++) {
        if (strcmp(name, REPO_TREE[i]) == 0) {
            return true;
        }
    }
    return false;
}

atlas_status atlas_orch_new_uid(atlas_buf *out, atlas_err *err) {
    atlas_buf hex = ATLAS_BUF_INIT;
    atlas_status st = random_hex(ATLAS_ORCH_UID_HEX / 2u, &hex, err);
    if (st == ATLAS_OK) {
        /* A one-character prefix so a job id is recognisable on sight and can
         * never be confused with a commit id, a content hash or a token. */
        st = atlas_buf_set_str(out, "j", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_append(out, hex.data, hex.len, err);
    }
    atlas_buf_free(&hex);
    return st;
}


/* --- the run's own axis (A11.0) ------------------------------------------
 *
 * No `default:` in either switch, so adding a member to the vocabulary is a
 * compile error here rather than a value that silently reads as neither
 * terminal nor active. */
const char *atlas_orch_run_status_name(atlas_orch_run_status s) {
    switch (s) {
    case ATLAS_ORCH_RUN_ACTIVE: return "ACTIVE";
    case ATLAS_ORCH_RUN_ACCEPTED: return "ACCEPTED";
    case ATLAS_ORCH_RUN_BLOCKED: return "BLOCKED";
    case ATLAS_ORCH_RUN_UNKNOWN: break;
    }
    return "UNKNOWN";
}

bool atlas_orch_run_status_parse(const char *name, atlas_orch_run_status *out) {
    if (name == NULL || out == NULL) {
        return false;
    }
    static const struct {
        const char *name;
        atlas_orch_run_status value;
    } TABLE[] = {
        {"ACTIVE", ATLAS_ORCH_RUN_ACTIVE},
        {"ACCEPTED", ATLAS_ORCH_RUN_ACCEPTED},
        {"BLOCKED", ATLAS_ORCH_RUN_BLOCKED},
    };
    for (size_t i = 0; i < sizeof TABLE / sizeof TABLE[0]; i++) {
        if (strcmp(name, TABLE[i].name) == 0) {
            *out = TABLE[i].value;
            return true;
        }
    }
    /* "UNKNOWN" is deliberately absent from the table. It is the zero of the
     * vocabulary, never a value a stored row may hold, so a database that
     * presents it is reporting corruption and must not parse cleanly. */
    return false;
}

bool atlas_orch_run_status_is_terminal(atlas_orch_run_status s) {
    switch (s) {
    case ATLAS_ORCH_RUN_ACCEPTED:
    case ATLAS_ORCH_RUN_BLOCKED: return true;
    case ATLAS_ORCH_RUN_UNKNOWN:
    case ATLAS_ORCH_RUN_ACTIVE: break;
    }
    return false;
}

atlas_status atlas_orch_new_run_uid(atlas_buf *out, atlas_err *err) {
    atlas_buf hex = ATLAS_BUF_INIT;
    atlas_status st = random_hex(ATLAS_ORCH_RUN_UID_HEX / 2u, &hex, err);
    if (st == ATLAS_OK) {
        /* "r", so a run id is recognisable on sight and can never be mistaken
         * for a job id, a commit id, a content hash or a token. */
        st = atlas_buf_set_str(out, "r", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_append(out, hex.data, hex.len, err);
    }
    atlas_buf_free(&hex);
    return st;
}
atlas_status atlas_orch_new_token(atlas_buf *out, atlas_err *err) {
    return random_hex(ATLAS_ORCH_TOKEN_BYTES, out, err);
}

atlas_status atlas_orch_token_digest(const char *token, char out[65], atlas_err *err) {
    if (token == NULL || !is_lower_hex(token, strlen(token), ATLAS_ORCH_TOKEN_HEX)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "the lease token is not well formed");
    }
    atlas_sha256 h;
    atlas_sha256_init(&h);
    feed(&h, ATLAS_ORCH_LEASE_DOMAIN, strlen(ATLAS_ORCH_LEASE_DOMAIN));
    feed(&h, token, strlen(token));
    unsigned char digest[ATLAS_SHA256_DIGEST_LEN];
    atlas_sha256_final(&h, digest);
    atlas_hex_encode(digest, sizeof(digest), out);
    return ATLAS_OK;
}
