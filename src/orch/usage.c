/* Atlas - A10.0: reading what an attempt cost out of a worker's stream.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See atlas/orch_usage.h. This is the whole of it and there is no second parser:
 * the driver reads the stream it captured, and everything downstream — the
 * durable summary, the database row, the run total — consumes what this
 * produced. A model payload never reaches any of them.
 *
 * Deliberately not yyjson. That dependency is confined to the IPC boundary by
 * `CLAUDE.md`, and a worker's stdout is not that boundary. What is needed here
 * is narrower than parsing anyway: a bounded scan for a handful of named
 * integers and two names, refusing anything it does not recognise.
 */
#define _GNU_SOURCE 1

#include "atlas/orch_usage.h"

#include <string.h>

#include "atlas/sha256.h"

static const char *const STATUS_NAMES[] = {"UNKNOWN", "PARTIAL", "AVAILABLE"};

const char *atlas_usage_status_name(atlas_usage_status s) {
    if ((size_t)s < sizeof STATUS_NAMES / sizeof STATUS_NAMES[0]) {
        return STATUS_NAMES[s];
    }
    return "UNKNOWN";
}

bool atlas_usage_status_parse(const char *name, atlas_usage_status *out) {
    if (name == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof STATUS_NAMES / sizeof STATUS_NAMES[0]; i++) {
        if (strcmp(name, STATUS_NAMES[i]) == 0) {
            *out = (atlas_usage_status)i;
            return true;
        }
    }
    return false;
}

void atlas_usage_init(atlas_usage *u) {
    memset(u, 0, sizeof(*u));
    u->status = ATLAS_USAGE_UNKNOWN;
}

bool atlas_usage_add(int64_t a, int64_t b, int64_t *out) {
    if (a < 0 || b < 0 || a > INT64_MAX - b) {
        return false;
    }
    *out = a + b;
    return true;
}

/* --- bounded scanning ------------------------------------------------------ */

/* The first occurrence of `key` in `[p, end)`, or NULL. */
static const char *find_key(const char *p, const char *end, const char *key) {
    size_t kl = strlen(key);
    if ((size_t)(end - p) < kl) {
        return NULL;
    }
    return memmem(p, (size_t)(end - p), key, kl);
}

/* A non-negative integer immediately after `key`, refused if it is signed, over
 * the ceiling, or longer than any real count. Refusal is not zero: `*out` is
 * left alone and the caller records the field as absent. */
static bool take_int(const char *p, const char *end, const char *key, int64_t *out) {
    const char *at = find_key(p, end, key);
    if (at == NULL) {
        return false;
    }
    at += strlen(key);
    if (at >= end || *at < '0' || *at > '9') {
        /* A negative or non-numeric value is corrupt, and corrupt is absent. */
        return false;
    }
    int64_t v = 0;
    size_t digits = 0;
    while (at < end && *at >= '0' && *at <= '9') {
        if (digits > 18) {
            return false;
        }
        v = v * 10 + (*at - '0');
        if (v >= ATLAS_USAGE_COUNT_MAX) {
            return false;
        }
        at++;
        digits++;
    }
    if (digits == 0) {
        return false;
    }
    *out = v;
    return true;
}

/* `"total_cost_usd":0.14594849999999998` into integer micro-USD.
 *
 * Parsed digit by digit with integer arithmetic and never through a double:
 * a total that is summed in floating point is not reproducible, and a cost
 * nobody can reproduce is not evidence. Fractional digits beyond the scale are
 * discarded rather than rounded, so a stored cost is always a lower bound by
 * less than one micro-dollar and never an invented fraction. */
static bool take_cost(const char *p, const char *end, const char *key, int64_t *out) {
    const char *at = find_key(p, end, key);
    if (at == NULL) {
        return false;
    }
    at += strlen(key);
    if (at < end && *at == '-') {
        return false; /* a negative cost is corrupt */
    }
    int64_t whole = 0;
    size_t digits = 0;
    while (at < end && *at >= '0' && *at <= '9') {
        if (digits > 6) {
            return false; /* no single attempt costs a million dollars */
        }
        whole = whole * 10 + (*at - '0');
        at++;
        digits++;
    }
    if (digits == 0) {
        return false;
    }
    int64_t micro = whole * ATLAS_USAGE_COST_SCALE;
    if (micro >= ATLAS_USAGE_COST_MAX) {
        return false;
    }
    if (at < end && *at == '.') {
        at++;
        int64_t scale = ATLAS_USAGE_COST_SCALE / 10;
        while (at < end && *at >= '0' && *at <= '9') {
            if (scale >= 1) {
                micro += (int64_t)(*at - '0') * scale;
                scale /= 10;
            }
            at++;
        }
    }
    *out = micro;
    return true;
}

/* A short checked string value after `key`, refused if it is unterminated,
 * longer than the destination, or carries anything but the printable ASCII a
 * model or provider name is made of. Anything else is dropped rather than
 * stored: this field is displayed, and A2's rule is that a displayed value comes
 * from a checked vocabulary or is validated, never escaped after the fact. */
static void take_name(const char *p, const char *end, const char *key, char *dst, size_t cap) {
    const char *at = find_key(p, end, key);
    if (at == NULL) {
        return;
    }
    at += strlen(key);
    size_t n = 0;
    while (at < end && *at != '"') {
        unsigned char c = (unsigned char)*at;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_' || c == '.' || c == '[' || c == ']';
        if (!ok || n + 1u >= cap) {
            return; /* leave it empty rather than store something unexpected */
        }
        dst[n++] = (char)c;
        at++;
    }
    if (at >= end) {
        return;
    }
    dst[n] = '\0';
}

/* --- the final record ------------------------------------------------------ */

void atlas_usage_from_stream(const char *stream, size_t len, atlas_usage *out) {
    atlas_usage_init(out);
    if (stream == NULL || len == 0) {
        return;
    }

    /* The last eligible line wins.
     *
     * A worker's stdout can contain anything, including a line shaped exactly
     * like a final result — echoed by a tool, quoted in prose, or written on
     * purpose. Taking the last one means a forgery is only ever superseded by
     * the record the CLI actually ends with, and a worker that dies immediately
     * after emitting one has its outcome classified by Atlas from the process
     * exit regardless. Nothing here is authority: it is a measurement that
     * accompanies a verdict reached elsewhere. */
    const char *best = NULL;
    size_t best_len = 0;
    const char *p = stream;
    const char *end = stream + len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *line_end = nl != NULL ? nl : end;
        size_t line_len = (size_t)(line_end - p);
        while (line_len > 0 && (p[line_len - 1u] == '\r' || p[line_len - 1u] == ' ')) {
            line_len--;
        }
        if (line_len > 2u && p[0] == '{' && p[line_len - 1u] == '}' &&
            find_key(p, p + line_len, "\"type\":\"result\"") != NULL &&
            find_key(p, p + line_len, "\"usage\":{") != NULL) {
            best = p;
            best_len = line_len;
        }
        if (nl == NULL) {
            break;
        }
        p = nl + 1;
    }
    if (best == NULL) {
        return; /* UNKNOWN, and not zero */
    }

    const char *b = best;
    const char *be = best + best_len;
    out->has_input = take_int(b, be, "\"input_tokens\":", &out->input_tokens);
    out->has_output = take_int(b, be, "\"output_tokens\":", &out->output_tokens);
    out->has_cache_creation =
        take_int(b, be, "\"cache_creation_input_tokens\":", &out->cache_creation_tokens);
    out->has_cache_read = take_int(b, be, "\"cache_read_input_tokens\":", &out->cache_read_tokens);
    out->has_turns = take_int(b, be, "\"num_turns\":", &out->turns);
    out->has_duration = take_int(b, be, "\"duration_ms\":", &out->duration_ms);
    out->has_api_duration = take_int(b, be, "\"duration_api_ms\":", &out->api_duration_ms);
    out->has_cost = take_cost(b, be, "\"total_cost_usd\":", &out->cost_micro_usd);
    take_name(b, be, "\"canonicalModel\":\"", out->model, sizeof out->model);
    take_name(b, be, "\"provider\":\"", out->provider, sizeof out->provider);

    /* What must be present for the record to be relied on. Cost is deliberately
     * not in this list: a provider that reports none leaves it absent, and that
     * is a complete measurement of tokens with an unknown price, not a partial
     * one. */
    bool required = out->has_input && out->has_output && out->has_cache_creation &&
                    out->has_cache_read && out->has_turns && out->has_duration;
    out->status = required ? ATLAS_USAGE_AVAILABLE : ATLAS_USAGE_PARTIAL;
}

/* --- the durable summary ---------------------------------------------------
 *
 * One small document per attempt, written before a completion is offered and
 * read back after a restart. It exists because the *result* spool is cleared
 * once the daemon accepts a completion, and the worker log it contains is
 * dropped above the inline artifact ceiling — so on the path where a run
 * succeeded, which is the path an experiment cares about, every number was
 * being thrown away.
 *
 * Numbers and checked names only. The final record also carries the model's
 * `result` text, a session identifier and whatever the run produced; none of
 * that is here, because this file outlives the log deliberately and a summary
 * that quietly retained model output would be a transcript with a shorter name.
 *
 * A field that is absent is written as `-`, which decodes back to absent. Zero
 * is written as `0` and means zero. */
static atlas_status put_opt(atlas_buf *out, const char *key, bool has, int64_t v,
                            atlas_err *err) {
    if (has) {
        return atlas_buf_appendf(out, err, "%s=%lld\n", key, (long long)v);
    }
    return atlas_buf_appendf(out, err, "%s=-\n", key);
}

atlas_status atlas_usage_encode(const atlas_usage *u, atlas_buf *out, atlas_err *err) {
    atlas_buf_reset(out);
    atlas_status st = atlas_buf_appendf(out, err, "atlas-usage-1\nstatus=%s\n",
                                        atlas_usage_status_name(u->status));
    if (st == ATLAS_OK) {
        st = atlas_buf_appendf(out, err, "provider=%s\nmodel=%s\n", u->provider, u->model);
    }
    const struct {
        const char *k;
        bool has;
        int64_t v;
    } F[] = {
        {"input_tokens", u->has_input, u->input_tokens},
        {"output_tokens", u->has_output, u->output_tokens},
        {"cache_creation_tokens", u->has_cache_creation, u->cache_creation_tokens},
        {"cache_read_tokens", u->has_cache_read, u->cache_read_tokens},
        {"cost_micro_usd", u->has_cost, u->cost_micro_usd},
        {"duration_ms", u->has_duration, u->duration_ms},
        {"api_duration_ms", u->has_api_duration, u->api_duration_ms},
        {"turns", u->has_turns, u->turns},
    };
    for (size_t i = 0; st == ATLAS_OK && i < sizeof F / sizeof F[0]; i++) {
        st = put_opt(out, F[i].k, F[i].has, F[i].v, err);
    }
    return st;
}

/* Reads one `key=` line. Absent key or `-` both leave the field absent. */
static bool get_opt(const char *text, const char *key, bool *has, int64_t *v) {
    *has = false;
    size_t kl = strlen(key);
    const char *p = text;
    while (p != NULL && *p != '\0') {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            const char *at = p + kl + 1u;
            if (*at == '-') {
                return true;
            }
            int64_t n = 0;
            size_t digits = 0;
            while (*at >= '0' && *at <= '9') {
                if (digits > 18) {
                    return true;
                }
                n = n * 10 + (*at - '0');
                if (n >= ATLAS_USAGE_COUNT_MAX) {
                    return true;
                }
                at++;
                digits++;
            }
            if (digits == 0) {
                return true;
            }
            *has = true;
            *v = n;
            return true;
        }
        p = strchr(p, '\n');
        if (p != NULL) {
            p++;
        }
    }
    return false;
}

static void get_name(const char *text, const char *key, char *dst, size_t cap) {
    size_t kl = strlen(key);
    const char *p = text;
    while (p != NULL && *p != '\0') {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            const char *at = p + kl + 1u;
            size_t n = 0;
            while (*at != '\n' && *at != '\0' && n + 1u < cap) {
                dst[n++] = *at++;
            }
            dst[n] = '\0';
            return;
        }
        p = strchr(p, '\n');
        if (p != NULL) {
            p++;
        }
    }
}

atlas_status atlas_usage_decode(const char *text, atlas_usage *out, atlas_err *err) {
    atlas_usage_init(out);
    if (text == NULL || strncmp(text, "atlas-usage-1\n", 14u) != 0) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "not an Atlas usage summary");
    }
    char st[32] = {0};
    get_name(text, "status", st, sizeof st);
    if (!atlas_usage_status_parse(st, &out->status)) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "the usage summary names no known status");
    }
    get_name(text, "provider", out->provider, sizeof out->provider);
    get_name(text, "model", out->model, sizeof out->model);
    (void)get_opt(text, "input_tokens", &out->has_input, &out->input_tokens);
    (void)get_opt(text, "output_tokens", &out->has_output, &out->output_tokens);
    (void)get_opt(text, "cache_creation_tokens", &out->has_cache_creation,
                  &out->cache_creation_tokens);
    (void)get_opt(text, "cache_read_tokens", &out->has_cache_read, &out->cache_read_tokens);
    (void)get_opt(text, "cost_micro_usd", &out->has_cost, &out->cost_micro_usd);
    (void)get_opt(text, "duration_ms", &out->has_duration, &out->duration_ms);
    (void)get_opt(text, "api_duration_ms", &out->has_api_duration, &out->api_duration_ms);
    (void)get_opt(text, "turns", &out->has_turns, &out->turns);
    return ATLAS_OK;
}

/* --- run aggregation ------------------------------------------------------- */

void atlas_usage_run_init(atlas_usage_run *r) {
    memset(r, 0, sizeof(*r));
    r->status = ATLAS_USAGE_UNKNOWN;
    r->tokens_complete = true;
    r->cost_complete = true;
}

/* Adds one field, and marks the whole total incomplete if it cannot. */
static void fold_one(int64_t *acc, bool has, int64_t v, bool *complete) {
    if (!has) {
        *complete = false;
        return;
    }
    int64_t sum = 0;
    if (!atlas_usage_add(*acc, v, &sum)) {
        *complete = false;
        return;
    }
    *acc = sum;
}

void atlas_usage_run_fold(atlas_usage_run *r, const atlas_usage *u) {
    if (u->status == ATLAS_USAGE_UNKNOWN) {
        /* Nothing was observed about this attempt. Counting it as zero is the
         * one thing that would make a run look cheaper than it was. */
        r->attempts_missing_usage++;
        r->tokens_complete = false;
        r->cost_complete = false;
        return;
    }
    r->attempts_with_usage++;
    fold_one(&r->input_tokens, u->has_input, u->input_tokens, &r->tokens_complete);
    fold_one(&r->output_tokens, u->has_output, u->output_tokens, &r->tokens_complete);
    fold_one(&r->cache_creation_tokens, u->has_cache_creation, u->cache_creation_tokens,
             &r->tokens_complete);
    fold_one(&r->cache_read_tokens, u->has_cache_read, u->cache_read_tokens, &r->tokens_complete);
    fold_one(&r->worker_duration_ms, u->has_duration, u->duration_ms, &r->tokens_complete);
    fold_one(&r->turns, u->has_turns, u->turns, &r->tokens_complete);
    if (u->has_cost) {
        int64_t sum = 0;
        if (atlas_usage_add(r->cost_known_micro_usd, u->cost_micro_usd, &sum)) {
            r->cost_known_micro_usd = sum;
            r->has_any_cost = true;
        } else {
            r->cost_complete = false;
        }
    } else {
        r->cost_complete = false;
    }
    if (u->status == ATLAS_USAGE_PARTIAL) {
        r->tokens_complete = false;
    }
}

void atlas_usage_run_settle(atlas_usage_run *r, int64_t attempts_started, int64_t worker_starts) {
    r->attempts_started = attempts_started;
    r->worker_starts = worker_starts;
    /* Attempts the ledger knows about but that produced no usage row at all —
     * a worker that never spawned, or one whose completion never landed. They
     * are missing whether or not anything was folded for them. */
    int64_t unaccounted = attempts_started - r->attempts_with_usage - r->attempts_missing_usage;
    if (unaccounted > 0) {
        r->attempts_missing_usage += unaccounted;
        r->tokens_complete = false;
        r->cost_complete = false;
    }
    if (r->attempts_with_usage == 0) {
        r->status = ATLAS_USAGE_UNKNOWN;
    } else if (r->attempts_missing_usage > 0 || !r->tokens_complete) {
        r->status = ATLAS_USAGE_PARTIAL;
    } else {
        r->status = ATLAS_USAGE_AVAILABLE;
    }
}
