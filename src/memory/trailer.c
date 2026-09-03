/* Atlas - A12.1 T14: commit trailers -- composed for a person, ingested from
 * the index.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See the "T14: commit trailers" section comment in `include/atlas/memory.h`
 * for the full argument: what the six fields mean, which survive an index
 * rebuild and which do not, and why the composer has no parameter that could
 * ever carry prose. This file is the two functions that comment describes,
 * plus the binding struct's init/free pair and the bounded, file-local
 * parser -- no SQL of its own beyond what `src/db/db_memory.c`,
 * `src/db/db_index.c`, `src/db/db_ai.c` and `atlas_db_orch_run_get`
 * (`include/atlas/orch_ops.h`, `src/db/db_orch.c`) already expose, matching
 * `pack.c`'s own discipline one section over.
 *
 * `atlas_memory_trailer_ingest` parses `commits.body` -- a stored column, no
 * process and no git invocation, A1's "no git process ... inside a write
 * transaction" satisfied by construction rather than by care at each call
 * site. Repository content is untrusted input (the untrusted-text contract in
 * `CLAUDE.md`): every parse below is bounded, and a value this file never
 * manages to verify is named in `unknown_fields` and never stored -- it binds
 * nothing, exactly as a value that verifies binds nothing more than a
 * pointer.
 */
#include "atlas/memory.h"

#include <stdint.h>
#include <string.h>

#include "atlas/db.h"
#include "atlas/orch_ops.h"

/* --- the binding struct ------------------------------------------------- */

void atlas_memory_trailer_binding_init(atlas_memory_trailer_binding *b) {
    if (b == NULL) {
        return;
    }
    memset(b, 0, sizeof *b);
    atlas_buf_init(&b->run_uid);
    atlas_buf_init(&b->change_reason_uid);
    atlas_buf_init(&b->unknown_fields);
}

void atlas_memory_trailer_binding_free(atlas_memory_trailer_binding *b) {
    if (b == NULL) {
        return;
    }
    atlas_buf_free(&b->run_uid);
    atlas_buf_free(&b->change_reason_uid);
    atlas_buf_free(&b->unknown_fields);
}

/* --- netstrings -----------------------------------------------------------
 *
 * `<decimal length>:<bytes>,` -- `src/memory/pack.c`'s own comment gives the
 * full argument for why this is a small file-local copy rather than a shared
 * helper; unchanged here. */
static atlas_status ns_put(atlas_buf *out, const char *s, atlas_err *err) {
    size_t n = s != NULL ? strlen(s) : 0u;
    atlas_status st = atlas_buf_appendf(out, err, "%zu:", n);
    if (st == ATLAS_OK && n > 0) {
        st = atlas_buf_append(out, s, n, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_append_ch(out, ',', err);
    }
    return st;
}

/* --- bounded parsing --------------------------------------------------------
 *
 * `commits.body` is untrusted, unbounded-in-principle repository content
 * (architecture invariant 6/7 -- untrusted input, bounded parsers). Every
 * function below examines at most `ATLAS_MEMORY_TRAILER_TAIL_BYTES_MAX` raw
 * bytes of it (`limits.h`), and within that already-bounded tail considers at
 * most `ATLAS_MEMORY_TRAILER_SCAN_MAX` lines -- two different dimensions,
 * both named at their own constant, per that constant's own comment. */

/* The longest value any of the five known fields could legitimately carry: a
 * run uid is `ATLAS_ORCH_RUN_UID_MAX` (40) bytes at most, a digest field is
 * "sha256:" plus 64 hex (71), an integer field a handful of digits. A value
 * this long can only be a hostile or corrupted line, never a real one, so it
 * is rejected before comparison rather than copied. */
#define TRAILER_FIELD_VALUE_MAX 128u

static const char PROVENANCE_MARKER[] = "Atlas-Provenance: v1";
static const char PREFIX_RUN[] = "Atlas-Run: ";
static const char PREFIX_GENERATION[] = "Atlas-Memory-Generation: ";
static const char PREFIX_CONTEXT_DIGEST[] = "Atlas-Context-Digest: ";
static const char PREFIX_DECISION_DIGEST[] = "Atlas-Decision-Set-Digest: ";
static const char PREFIX_CHANGE_REASON[] = "Atlas-Change-Reason: ";

/* Returns the content length of the line starting at `body[pos]` (up to the
 * next '\n' or the end of `body`), with one trailing '\r' trimmed, and sets
 * `*next_pos` to the offset immediately past this line's own '\n' (or past
 * the line itself, at end of body) -- a caller always advances, whatever this
 * line turns out to contain. `pos` must be `<= body_len`; called with
 * `pos == body_len` this reports a zero-length line and leaves `*next_pos`
 * unchanged, which is what lets every field check below run unconditionally
 * once the block's parse position has run off the end of the body. */
static size_t line_extent(const char *body, size_t body_len, size_t pos, size_t *next_pos) {
    const char *nl = (pos < body_len) ? memchr(body + pos, '\n', body_len - pos) : NULL;
    size_t line_len = (nl != NULL) ? (size_t)(nl - (body + pos)) : (body_len - pos);
    *next_pos = pos + line_len + ((nl != NULL) ? 1u : 0u);
    size_t content_len = line_len;
    if (content_len > 0 && body[pos + content_len - 1u] == '\r') {
        content_len--;
    }
    return content_len;
}

/* The backward twin of `line_extent`: given `end` (the exclusive offset one
 * past this line's own content, i.e. the offset of its terminating '\n', or
 * `body_len` for a final line with none), finds where that line's content
 * starts by searching backward for the previous '\n', never before `low`.
 * Returns the content length (trailing '\r' trimmed, matching `line_extent`)
 * and sets `*start` to the first content byte; `*prev_end` is the `end` to
 * pass on the next call to keep walking backward -- the position of this
 * line's own leading '\n' (skipped, so the previous line's content does not
 * include it), or `low` once the walk has reached the start of the region.
 * `end` must be `> low`; every call strictly decreases `end` (by at least
 * one, whether or not a '\n' is found), so a caller that stops once
 * `end <= low` cannot loop. */
static size_t line_extent_rev(const char *body, size_t low, size_t end, size_t *start,
                              size_t *prev_end) {
    size_t nl = end;
    while (nl > low && body[nl - 1u] != '\n') {
        nl--;
    }
    size_t content_start = nl;
    size_t content_len = end - nl;
    if (content_len > 0 && body[content_start + content_len - 1u] == '\r') {
        content_len--;
    }
    *start = content_start;
    *prev_end = (nl > low) ? (nl - 1u) : low;
    return content_len;
}

/* Finds "Atlas-Provenance: v1" as an exact whole line within the last
 * `ATLAS_MEMORY_TRAILER_TAIL_BYTES_MAX` bytes of `body`, considering at most
 * `ATLAS_MEMORY_TRAILER_SCAN_MAX` of that tail's lines -- **from the end of
 * the tail backward**, because a trailer block lives at the end of a message
 * by construction (`limits.h`'s own comment on `ATLAS_MEMORY_TRAILER_SCAN_MAX`)
 * and the earlier forward walk considered the tail's *first* `SCAN_MAX`
 * lines, which is the wrong end for a message whose bounded window holds
 * more lines than that (fix round I1). Walking backward also makes "found"
 * and "the search's own bound was reached" the outcomes of one walk rather
 * than two: the *first* match encountered walking backward is the one
 * nearest the end of the message, so a `git merge --squash` concatenation
 * that quotes an older block earlier in the same tail binds the real,
 * trailing one rather than the first one on the page.
 *
 * Returns the offset of the byte immediately after the marker line (the
 * block's first data line, if any), or `SIZE_MAX` when no such line exists
 * in the bounded window -- conservative in the one direction that matters: a
 * block pushed out of the window by trailing padding reads as no block at
 * all, never as a forged one. `*bound_hit_out` (never NULL) is set to
 * whether the window's own bound -- the byte backstop, the line cap, or both
 * -- kept this search from examining the *whole* message; both are "this
 * parse did not look far enough", A9.2.2's asymmetry over a per-commit
 * search rather than only over the pass-wide commit scan. It is left false
 * whenever a marker is found: once one is, the search stops, so neither
 * bound can have cut off the line that actually matched. */
static size_t find_provenance_line(const char *body, size_t body_len, bool *bound_hit_out) {
    *bound_hit_out = false;
    size_t tail_len =
        (body_len > ATLAS_MEMORY_TRAILER_TAIL_BYTES_MAX) ? ATLAS_MEMORY_TRAILER_TAIL_BYTES_MAX
                                                          : body_len;
    size_t tail_start = body_len - tail_len;
    const size_t marker_len = sizeof(PROVENANCE_MARKER) - 1u;

    size_t end = body_len;
    size_t lines = 0;
    while (end > tail_start && lines < ATLAS_MEMORY_TRAILER_SCAN_MAX) {
        lines++;
        size_t start = 0, prev_end = 0;
        size_t content_len = line_extent_rev(body, tail_start, end, &start, &prev_end);
        if (content_len == marker_len && memcmp(body + start, PROVENANCE_MARKER, marker_len) == 0) {
            /* The block's data begins right after this marker line's own
             * newline (forward semantics, matching `line_extent`'s `next`):
             * `end` is that newline's offset unless this line runs to
             * `body_len` with no trailing newline of its own, in which case
             * there is no data to follow it either. */
            return (end < body_len) ? end + 1u : body_len;
        }
        if (prev_end >= end) {
            break; /* unreachable: line_extent_rev always decreases `end` */
        }
        end = prev_end;
    }
    /* Not found. Either bound left more of the message unexamined: the line
     * cap stopped this walk before it reached `tail_start` (`end >
     * tail_start`), or the byte backstop had already discarded everything
     * before `tail_start` (`tail_start > 0`). */
    if (end > tail_start || tail_start > 0) {
        *bound_hit_out = true;
    }
    return SIZE_MAX;
}

/* Matches the line at `body[pos]` against `<prefix><value>` exactly -- the
 * prefix carries its own trailing ": " -- with a nonempty, bounded value.
 * `*next_pos` is always set to this line's own successor (see `line_extent`);
 * the field is absent (line missing, wrong key, empty or oversized value)
 * whenever this returns false, and the caller advances regardless. */
static bool take_field_line(const char *body, size_t body_len, size_t pos, const char *prefix,
                            size_t prefix_len, const char **value, size_t *value_len,
                            size_t *next_pos) {
    size_t content_len = line_extent(body, body_len, pos, next_pos);
    if (content_len <= prefix_len || memcmp(body + pos, prefix, prefix_len) != 0) {
        return false;
    }
    size_t vlen = content_len - prefix_len;
    if (vlen == 0 || vlen > TRAILER_FIELD_VALUE_MAX) {
        return false;
    }
    *value = body + pos + prefix_len;
    *value_len = vlen;
    return true;
}

/* A bounded, non-negative decimal integer -- at most 18 digits, comfortably
 * inside int64_t regardless of leading digit, `src/git/git_parse.c`'s own
 * `atlas_parse_i64` bound of 19 minus the sign this field never carries. Not
 * reused from there: a git-output parser is not this file's dependency, the
 * same discipline `pack.c`'s tokenizer comment gives for reimplementing
 * rather than sharing a small, closed-form routine across layers. */
static bool parse_nonneg_i64(const char *s, size_t n, int64_t *out) {
    if (n == 0 || n > 18u) {
        return false;
    }
    int64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        v = v * 10 + (int64_t)(s[i] - '0');
    }
    *out = v;
    return true;
}

/* "sha256:" plus exactly ATLAS_SHA256_HEX_LEN lowercase hex digits -- the
 * shape every digest this codebase emits already has. A value the wrong
 * length (truncated, padded) or the wrong case never reaches comparison: it
 * is malformed, not merely a mismatch. */
static bool parse_digest_hex(const char *value, size_t value_len,
                             char out_hex[ATLAS_SHA256_HEX_LEN + 1u]) {
    static const char PFX[] = "sha256:";
    const size_t pfx_len = sizeof(PFX) - 1u;
    if (value_len != pfx_len + ATLAS_SHA256_HEX_LEN || memcmp(value, PFX, pfx_len) != 0) {
        return false;
    }
    for (size_t i = 0; i < ATLAS_SHA256_HEX_LEN; i++) {
        char c = value[pfx_len + i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
        out_hex[i] = c;
    }
    out_hex[ATLAS_SHA256_HEX_LEN] = '\0';
    return true;
}

/* --- ingest ---------------------------------------------------------------- */

atlas_status atlas_memory_trailer_ingest(atlas_db *db, int64_t repo_id, const char *commit_oid,
                                         atlas_memory_trailer_binding *out, atlas_err *err) {
    if (db == NULL || commit_oid == NULL || commit_oid[0] == '\0' || out == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "no database, commit or destination to ingest a trailer into");
    }
    atlas_memory_trailer_binding_free(out);
    atlas_memory_trailer_binding_init(out);

    atlas_buf body = ATLAS_BUF_INIT;
    bool commit_found = false;
    atlas_status st = atlas_db_commit_body_get(db, repo_id, commit_oid, &body, &commit_found, err);
    if (st == ATLAS_OK && !commit_found) {
        atlas_buf_free(&body);
        return atlas_err_set(err, ATLAS_ERR_REPO, "commit %s is not an indexed commit of repository",
                             commit_oid);
    }
    if (st != ATLAS_OK) {
        atlas_buf_free(&body);
        return st;
    }

    const char *b = atlas_buf_cstr(&body);
    size_t blen = body.len;
    size_t pos = find_provenance_line(b, blen, &out->bound_hit);
    if (pos == SIZE_MAX) {
        atlas_buf_free(&body);
        /* has_block stays false; bound_hit was set by find_provenance_line
         * above (fix round I1/I2) -- true when this parse could not fully
         * examine the message, false when it genuinely found no marker
         * anywhere within the bound; every other member at its zero. */
        return ATLAS_OK;
    }
    out->has_block = true;

    const char *run_val = NULL, *gen_val = NULL, *ctx_val = NULL, *dec_val = NULL,
              *reason_val = NULL;
    size_t run_vlen = 0, gen_vlen = 0, ctx_vlen = 0, dec_vlen = 0, reason_vlen = 0;
    bool run_line = take_field_line(b, blen, pos, PREFIX_RUN, sizeof(PREFIX_RUN) - 1u, &run_val,
                                    &run_vlen, &pos);
    bool gen_line = take_field_line(b, blen, pos, PREFIX_GENERATION, sizeof(PREFIX_GENERATION) - 1u,
                                    &gen_val, &gen_vlen, &pos);
    bool ctx_line = take_field_line(b, blen, pos, PREFIX_CONTEXT_DIGEST,
                                    sizeof(PREFIX_CONTEXT_DIGEST) - 1u, &ctx_val, &ctx_vlen, &pos);
    bool dec_line = take_field_line(b, blen, pos, PREFIX_DECISION_DIGEST,
                                    sizeof(PREFIX_DECISION_DIGEST) - 1u, &dec_val, &dec_vlen, &pos);
    bool reason_line = take_field_line(b, blen, pos, PREFIX_CHANGE_REASON,
                                       sizeof(PREFIX_CHANGE_REASON) - 1u, &reason_val, &reason_vlen,
                                       &pos);

    /* Every byte this function still needs is copied out of `body` (or
     * parsed into a fixed-size local) before it is freed -- `run_val` etc.
     * point into it and must not outlive this block. */
    atlas_buf run_candidate = ATLAS_BUF_INIT;
    if (run_line) {
        st = atlas_buf_set(&run_candidate, run_val, run_vlen, err);
    }
    int64_t gen_candidate = 0;
    bool gen_parsed = gen_line && parse_nonneg_i64(gen_val, gen_vlen, &gen_candidate);
    char ctx_hex[ATLAS_SHA256_HEX_LEN + 1u];
    bool ctx_parsed = ctx_line && parse_digest_hex(ctx_val, ctx_vlen, ctx_hex);
    char dec_hex[ATLAS_SHA256_HEX_LEN + 1u];
    bool dec_parsed = dec_line && parse_digest_hex(dec_val, dec_vlen, dec_hex);
    int64_t reason_candidate = 0;
    bool reason_parsed = reason_line && parse_nonneg_i64(reason_val, reason_vlen, &reason_candidate);

    atlas_buf_free(&body);

    if (st != ATLAS_OK) {
        atlas_buf_free(&run_candidate);
        atlas_memory_trailer_binding_free(out);
        return st;
    }

    const char *unknown[5];
    size_t unknown_n = 0;

    /* Atlas-Run: resolved against orch_runs, this repository. */
    atlas_buf repo_hash = ATLAS_BUF_INIT;
    st = atlas_db_repo_identity_hash(db, repo_id, &repo_hash, err);
    bool run_ok = false;
    if (st == ATLAS_OK && run_line) {
        atlas_orch_run_view rv;
        bool run_found = false;
        st = atlas_db_orch_run_get(db, atlas_buf_cstr(&run_candidate), &rv, &run_found, err);
        if (st == ATLAS_OK && run_found &&
           strcmp(rv.repo_identity_hash, atlas_buf_cstr(&repo_hash)) == 0) {
            run_ok = true;
        }
    }
    if (st == ATLAS_OK) {
        if (run_ok) {
            st = atlas_buf_set(&out->run_uid, run_candidate.data, run_candidate.len, err);
        } else {
            unknown[unknown_n++] = "run";
        }
    }

    /* Atlas-Memory-Generation, Atlas-Context-Digest and
     * Atlas-Decision-Set-Digest are all three checked against ONE frozen
     * pack, looked up by the raw run value the trailer named -- independent
     * of whether that same value also passed the orch_runs check just above.
     * A pack row carries its own `repo_id`, its own authority, so tampering
     * only the Atlas-Run line cannot also forge agreement here: an unknown
     * run uid finds no pack (since a pack is only ever frozen under a real
     * run_uid -- `atlas_memory_pack_freeze_in_tx`'s own precondition), and
     * these three fields go unknown right alongside "run", never silently
     * verifying against some other run's pack. */
    atlas_memory_pack pack;
    atlas_memory_pack_init(&pack);
    bool pack_ok = false;
    if (st == ATLAS_OK && run_line) {
        bool pack_found = false;
        st = atlas_db_memory_pack_get(db, atlas_buf_cstr(&run_candidate), &pack, &pack_found, err);
        if (st == ATLAS_OK && pack_found && pack.repo_id == repo_id) {
            pack_ok = true;
        }
    }
    if (st == ATLAS_OK) {
        if (pack_ok && gen_parsed && gen_candidate == pack.memory_generation) {
            out->memory_generation = gen_candidate;
        } else {
            unknown[unknown_n++] = "generation";
        }
    }
    if (st == ATLAS_OK) {
        if (pack_ok && ctx_parsed && strcmp(ctx_hex, atlas_buf_cstr(&pack.pack_digest)) == 0) {
            out->context_digest_ok = true;
        } else {
            unknown[unknown_n++] = "context_digest";
        }
    }
    if (st == ATLAS_OK) {
        if (pack_ok && dec_parsed &&
           strcmp(dec_hex, atlas_buf_cstr(&pack.decision_set_digest)) == 0) {
            out->decision_set_ok = true;
        } else {
            unknown[unknown_n++] = "decision_set_digest";
        }
    }
    atlas_memory_pack_free(&pack);

    /* Atlas-Change-Reason: resolved against ai_reasons, this repository --
     * independently of the run field, exactly as "generation" and the two
     * digests are independent of each other above. A tampered run uid must
     * not also take this field down with it. */
    if (st == ATLAS_OK) {
        bool reason_ok = false;
        if (reason_parsed) {
            bool exists = false;
            st = atlas_db_ai_reason_exists(db, repo_id, reason_candidate, &exists, err);
            reason_ok = (st == ATLAS_OK) && exists;
        }
        if (st == ATLAS_OK) {
            if (reason_ok) {
                st = atlas_buf_appendf(&out->change_reason_uid, err, "%lld",
                                      (long long)reason_candidate);
            } else {
                unknown[unknown_n++] = "change_reason";
            }
        }
    }

    atlas_buf_free(&repo_hash);
    atlas_buf_free(&run_candidate);

    if (st == ATLAS_OK) {
        st = atlas_buf_appendf(&out->unknown_fields, err, "%zu:", unknown_n);
        for (size_t i = 0; st == ATLAS_OK && i < unknown_n; i++) {
            st = ns_put(&out->unknown_fields, unknown[i], err);
        }
    }
    if (st != ATLAS_OK) {
        atlas_memory_trailer_binding_free(out);
    }
    return st;
}

/* --- compose ----------------------------------------------------------------
 *
 * See the section comment in `memory.h`: no parameter here could ever carry
 * prose, a prompt, a credential, a model name or a cost. Every value emitted
 * is one this function first read back out of a row it trusts -- the frozen
 * pack, whose presence is itself proof `run_uid` names a real run
 * (`atlas_memory_pack_freeze_in_tx` is only ever called from a root-task
 * SUBMIT that has already resolved one) -- never one it merely formats. */
atlas_status atlas_memory_trailer_compose(atlas_db *db, const char *run_uid,
                                          const char *change_reason_uid, atlas_buf *out,
                                          atlas_err *err) {
    if (db == NULL || run_uid == NULL || run_uid[0] == '\0' || change_reason_uid == NULL ||
       change_reason_uid[0] == '\0' || out == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "a trailer needs a run, a change reason and a destination");
    }
    atlas_buf_reset(out);

    atlas_memory_pack pack;
    atlas_memory_pack_init(&pack);
    bool pack_found = false;
    atlas_status st = atlas_db_memory_pack_get(db, run_uid, &pack, &pack_found, err);
    if (st == ATLAS_OK && !pack_found) {
        st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                           "run %s has no frozen context pack to compose a trailer from", run_uid);
    }

    int64_t reason_id = 0;
    if (st == ATLAS_OK &&
       !parse_nonneg_i64(change_reason_uid, strlen(change_reason_uid), &reason_id)) {
        st = atlas_err_set(err, ATLAS_ERR_INTERNAL,
                           "\"%s\" is not a recorded change reason id", change_reason_uid);
    }
    if (st == ATLAS_OK) {
        bool reason_exists = false;
        st = atlas_db_ai_reason_exists(db, pack.repo_id, reason_id, &reason_exists, err);
        if (st == ATLAS_OK && !reason_exists) {
            st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                               "change reason %lld names no recorded reason for this repository",
                               (long long)reason_id);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_appendf(out, err,
                               "Atlas-Provenance: v1\n"
                               "Atlas-Run: %s\n"
                               "Atlas-Memory-Generation: %lld\n"
                               "Atlas-Context-Digest: sha256:%s\n"
                               "Atlas-Decision-Set-Digest: sha256:%s\n"
                               "Atlas-Change-Reason: %lld\n",
                               run_uid, (long long)pack.memory_generation,
                               atlas_buf_cstr(&pack.pack_digest),
                               atlas_buf_cstr(&pack.decision_set_digest), (long long)reason_id);
    }
    atlas_memory_pack_free(&pack);
    if (st != ATLAS_OK) {
        atlas_buf_reset(out);
    }
    return st;
}
