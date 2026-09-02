/* Atlas - A12.1 T8: the pass -- observe outside the transaction, apply inside
 * it.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Decision 9 pins the shape: `atlas_memory_observe` reads every registered
 * source (T6), splits it (T7's pure half) and hashes it, with no transaction
 * open -- a file read and a git process are exactly what A1 forbids inside
 * one. `atlas_memory_apply_in_tx` runs second, inside the transaction the
 * caller already opened, and touches the database only: it resolves anchors
 * (T7's impure half, an index read), emits the intake ops through
 * `atlas_verify_intake_apply_in_tx` and nothing else, writes anchors and
 * unanchored rows, and appends a generation with its diff.
 *
 * Two things this file does that neither the brief nor the plan spelled out
 * in this much detail, both explained at their call sites below:
 *
 *   - a registered source's own bytes are not always indexed (a gitignored
 *     `*_DIR` child is the headline case, and T6's own fixtures are built
 *     around exactly it) -- and `EVIDENCE_ADD`'s `path_text` requires an
 *     indexed path. A candidate from an unindexed item is routed to
 *     `memory_unanchored` regardless of whether its own anchors would
 *     resolve, because the claim it would otherwise produce could never
 *     carry evidence for where its own bytes came from.
 *   - Decision 2's dependency edge is written as a *chain*, not a clique: a
 *     new duplicate-text evidence row links to the *first* prior match it
 *     finds, which is enough for the union-find to treat every copy as one
 *     connected component without an edge count that grows quadratically in
 *     the number of copies.
 */
#include "atlas/memory.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "atlas/db.h"
#include "atlas/mirror.h"
#include "atlas/pathrep.h"
#include "atlas/sha256.h"
#include "atlas/syspolicy.h"
#include "atlas/verify.h"
#include "atlas/verify_ops.h"

/* --- lifetimes --------------------------------------------------------------
 *
 * Every array in `atlas_memory_observation` is fixed-size at the compiled
 * ceilings, so `_init`/`_free` walk every slot regardless of how many are
 * actually used -- `atlas_memory_proposition_init`'s own shape for its eight
 * anchor slots, one layer out. */

static void observed_item_init(atlas_memory_observed_item *it) {
    memset(it, 0, sizeof *it);
    atlas_buf_init(&it->rel_path);
    atlas_buf_init(&it->blob_oid);
    atlas_buf_init(&it->commit_oid);
    atlas_buf_init(&it->bytes);
    atlas_buf_init(&it->content_sha256);
}

static void observed_item_free(atlas_memory_observed_item *it) {
    atlas_buf_free(&it->rel_path);
    atlas_buf_free(&it->blob_oid);
    atlas_buf_free(&it->commit_oid);
    atlas_buf_free(&it->bytes);
    atlas_buf_free(&it->content_sha256);
}

static void observed_source_init(atlas_memory_observed_source *s) {
    memset(s, 0, sizeof *s);
    atlas_buf_init(&s->path_raw);
    atlas_buf_init(&s->path_text);
    for (size_t i = 0; i < ATLAS_MEMORY_MAX_DIR_ENTRIES; i++) {
        observed_item_init(&s->items[i]);
    }
    for (size_t i = 0; i < ATLAS_MEMORY_MAX_PROPOSITIONS; i++) {
        atlas_memory_proposition_init(&s->candidates[i]);
    }
    atlas_memory_version_row_init(&s->external_latest);
}

static void observed_source_free(atlas_memory_observed_source *s) {
    atlas_buf_free(&s->path_raw);
    atlas_buf_free(&s->path_text);
    for (size_t i = 0; i < ATLAS_MEMORY_MAX_DIR_ENTRIES; i++) {
        observed_item_free(&s->items[i]);
    }
    for (size_t i = 0; i < ATLAS_MEMORY_MAX_PROPOSITIONS; i++) {
        atlas_memory_proposition_free(&s->candidates[i]);
    }
    atlas_memory_version_row_free(&s->external_latest);
}

void atlas_memory_touched_init(atlas_memory_touched *t) {
    if (t == NULL) {
        return;
    }
    memset(t, 0, sizeof *t);
    for (size_t i = 0; i < ATLAS_MEMORY_MAX_TOUCHED_PATHS; i++) {
        atlas_buf_init(&t->paths[i]);
    }
}

void atlas_memory_touched_free(atlas_memory_touched *t) {
    if (t == NULL) {
        return;
    }
    for (size_t i = 0; i < ATLAS_MEMORY_MAX_TOUCHED_PATHS; i++) {
        atlas_buf_free(&t->paths[i]);
    }
}

bool atlas_memory_touched_contains(const atlas_memory_touched *t, const char *path_text) {
    if (t == NULL) {
        return false;
    }
    if (t->bound_hit) {
        return true; /* cannot prove this path was not touched -- see the struct's own comment */
    }
    if (path_text == NULL) {
        return false;
    }
    size_t len = strlen(path_text);
    for (size_t i = 0; i < t->count; i++) {
        if (t->paths[i].len == len && memcmp(t->paths[i].data, path_text, len) == 0) {
            return true;
        }
    }
    return false;
}

void atlas_memory_observation_init(atlas_memory_observation *o) {
    if (o == NULL) {
        return;
    }
    memset(o, 0, sizeof *o);
    for (size_t i = 0; i < ATLAS_MEMORY_MAX_SOURCES; i++) {
        observed_source_init(&o->sources[i]);
    }
    atlas_memory_touched_init(&o->touched);
}

void atlas_memory_observation_free(atlas_memory_observation *o) {
    if (o == NULL) {
        return;
    }
    atlas_memory_touched_free(&o->touched);
    for (size_t i = 0; i < ATLAS_MEMORY_MAX_SOURCES; i++) {
        observed_source_free(&o->sources[i]);
    }
}

/* --- observe: one repo source, through T6 and T7's pure half ---------------- */

static atlas_status observe_repo_source(const atlas_repo_info *repo, const char *data_dir,
                                        atlas_memory_observed_source *src, atlas_err *err) {
    atlas_memory_read_item tmp[ATLAS_MEMORY_MAX_DIR_ENTRIES];
    size_t n = 0;
    bool from_mirror = false;
    atlas_status st =
        atlas_memory_read_source(repo, data_dir, src->cls, src->path_raw.data, src->path_raw.len,
                                 tmp, ATLAS_MEMORY_MAX_DIR_ENTRIES, &n, &from_mirror, err);
    if (st != ATLAS_OK) {
        return st;
    }
    src->from_mirror = from_mirror;
    src->item_count = n;

    size_t remaining_cap = ATLAS_MEMORY_MAX_PROPOSITIONS;
    for (size_t i = 0; i < n && st == ATLAS_OK; i++) {
        /* Ownership moves out of the temporary read result into the
         * observation this function is filling -- a plain member-wise move,
         * each source buffer reset to empty so the temporary's own
         * atlas_memory_read_item_free (never called; there is nothing left
         * for it to free) would be a no-op either way. */
        atlas_memory_observed_item *dst = &src->items[i];
        dst->rel_path = tmp[i].rel_path;
        dst->blob_oid = tmp[i].blob_oid;
        dst->commit_oid = tmp[i].commit_oid;
        dst->bytes = tmp[i].bytes;
        dst->outcome = tmp[i].outcome;
        dst->from_mirror = tmp[i].from_mirror;
        atlas_buf_init(&tmp[i].rel_path);
        atlas_buf_init(&tmp[i].blob_oid);
        atlas_buf_init(&tmp[i].commit_oid);
        atlas_buf_init(&tmp[i].bytes);
        dst->content_bytes = (int64_t)dst->bytes.len;

        if (dst->outcome != ATLAS_MEMORY_READ_OK) {
            continue;
        }

        char hex[ATLAS_SHA256_HEX_LEN + 1u];
        atlas_sha256_hex(dst->bytes.data != NULL ? dst->bytes.data : "", dst->bytes.len, hex);
        st = atlas_buf_set_str(&dst->content_sha256, hex, err);

        if (st == ATLAS_OK) {
            if (remaining_cap > 0) {
                size_t got = 0;
                bool bound_reached = false;
                st = atlas_memory_extract(&dst->bytes, &src->candidates[src->candidate_count],
                                          remaining_cap, &got, &bound_reached, err);
                if (st == ATLAS_OK) {
                    for (size_t k = 0; k < got; k++) {
                        src->candidate_item[src->candidate_count + k] = (uint8_t)i;
                    }
                    src->candidate_count += got;
                    remaining_cap -= got;
                    if (bound_reached) {
                        src->bound_hit = true;
                    }
                }
            } else {
                src->bound_hit = true;
            }
        }

        /* Exactly the versions with no blob carry their own bytes (migration
         * 29's CHECK) -- a git-tracked item's content is freed once
         * extraction has read it, because the version row that will
         * eventually describe it needs none. */
        if (st == ATLAS_OK && dst->blob_oid.len > 0) {
            atlas_buf_free(&dst->bytes);
            atlas_buf_init(&dst->bytes);
        }
    }
    return st;
}

/* --- observe: one EXTERNAL_* source, read from nothing but the index -------
 *
 * T8 never reads an EXTERNAL_* source itself: a different principal does, and
 * writes what it read through T11's `memory.put` (not yet built). This is a
 * plain read-only lookup -- no transaction is opened by it or held around
 * it -- of whatever that principal has already stored, which is why it may
 * run from the observe phase at all. */
static atlas_status observe_external_source(atlas_db *db, int64_t repo_id,
                                            atlas_memory_observed_source *src, atlas_err *err) {
    src->is_external = true;
    int64_t source_id = 0;
    bool found = false;
    atlas_status st = atlas_db_memory_source_find(db, repo_id, src->cls, src->path_raw.data,
                                                  src->path_raw.len, &source_id, NULL, &found, err);
    if (st != ATLAS_OK || !found) {
        return st;
    }
    bool vfound = false;
    st = atlas_db_memory_version_latest(db, source_id, &src->external_latest, &vfound, err);
    if (st != ATLAS_OK) {
        return st;
    }
    src->external_latest_found = vfound;
    if (!vfound) {
        return ATLAS_OK;
    }
    size_t got = 0;
    bool bound_reached = false;
    st = atlas_memory_extract(&src->external_latest.content, src->candidates,
                              ATLAS_MEMORY_MAX_PROPOSITIONS, &got, &bound_reached, err);
    if (st != ATLAS_OK) {
        return st;
    }
    for (size_t k = 0; k < got; k++) {
        src->candidate_item[k] = 0;
    }
    src->candidate_count = got;
    src->bound_hit = bound_reached;
    return ATLAS_OK;
}

/* --- observe: T9 fix-round-1 (C2), the touched-paths set --------------------
 *
 * `git diff --name-only <last generation's head> <head>`, bounded -- the
 * brief's own words for what a `COMMIT`-caused pass needs in order to know
 * *which* PATH-anchored claims a commit range actually invalidated, rather
 * than treating "verifier_input happened to move" as the only signal: a
 * bullet naming both a SYMBOL and a PATH gets a `symbol=` verifier input
 * (`src/memory/extract.c`), which does not move when the file changes but
 * the symbol does not, so without this set a commit rewriting that file
 * produced no diff row for a claim it had just invalidated.
 *
 * No new git process creation site: `atlas_repo_open_git` is A13's own
 * routing, already called for exactly this reason by `atlas_memory_read_source`
 * one call further in (T6); `atlas_git_log_since` is git.c's existing
 * incremental-history reader, the same one `src/core/reconcile.c` uses for
 * the tracked-file index itself. Runs here, in observe, because both are git
 * work and A1 forbids either inside the write transaction apply holds. */
typedef struct touched_ctx {
    atlas_memory_touched *out;
} touched_ctx;

static atlas_status touched_add_one(atlas_memory_touched *t, const void *bytes, size_t len,
                                    atlas_err *err) {
    atlas_buf enc = ATLAS_BUF_INIT;
    atlas_status st = atlas_path_text_encode(bytes, len, &enc, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&enc);
        return st;
    }
    for (size_t i = 0; i < t->count; i++) {
        if (t->paths[i].len == enc.len && memcmp(t->paths[i].data, enc.data, enc.len) == 0) {
            atlas_buf_free(&enc);
            return ATLAS_OK; /* already recorded this pass */
        }
    }
    if (t->count >= ATLAS_MEMORY_MAX_TOUCHED_PATHS) {
        t->bound_hit = true; /* reported, never silent -- A5's rule, one layer over */
        atlas_buf_free(&enc);
        return ATLAS_OK;
    }
    st = atlas_buf_set(&t->paths[t->count], enc.data, enc.len, err);
    if (st == ATLAS_OK) {
        t->count++;
    }
    atlas_buf_free(&enc);
    return st;
}

static atlas_status touched_change_cb(const atlas_git_commit *c, const atlas_git_change *ch, void *ud,
                                      atlas_err *err) {
    (void)c;
    touched_ctx *tc = ud;
    if (tc->out->bound_hit) {
        return ATLAS_OK; /* nothing more to learn; let the walk finish plainly */
    }
    atlas_status st = touched_add_one(tc->out, ch->path, ch->path_len, err);
    if (st == ATLAS_OK && !tc->out->bound_hit && ch->old_path != NULL && ch->old_path_len > 0) {
        st = touched_add_one(tc->out, ch->old_path, ch->old_path_len, err);
    }
    return st;
}

/* `have_last == false` (the first generation) or an unmoved HEAD both mean no
 * `COMMIT` cause is even possible this pass -- `out->available` stays false
 * and nothing here is worth reading. A repository that cannot be opened (no
 * mirror yet, A13) or whose recorded tip is stale or unknown (a rebase, a
 * force-push, garbage collection -- `src/core/reconcile.c`'s own reasons for
 * falling back to a full walk) has no bounded diff to offer either: the
 * conservative answer is `bound_hit`, not silence, because a caller reading
 * `available == false` here would wrongly read it as "nothing to worry
 * about" rather than "this could not be established". */
static atlas_status observe_touched_paths(atlas_db *db, const atlas_repo_info *repo,
                                          const char *data_dir, atlas_memory_touched *out,
                                          atlas_err *err) {
    int64_t last_generation = 0;
    bool have_last = false;
    atlas_buf last_head = ATLAS_BUF_INIT;
    atlas_status st = atlas_db_memory_generation_latest(db, repo->id, &last_generation, &last_head,
                                                        NULL, NULL, &have_last, err);
    (void)last_generation;
    if (st != ATLAS_OK) {
        atlas_buf_free(&last_head);
        return st;
    }
    if (!have_last || last_head.len == 0 ||
        strcmp(atlas_buf_cstr(&last_head), repo->scanned_head) == 0) {
        atlas_buf_free(&last_head);
        return ATLAS_OK;
    }

    atlas_git *g = NULL;
    bool from_mirror = false;
    st = atlas_repo_open_git(repo, data_dir, &g, &from_mirror, err);
    if (st != ATLAS_OK) {
        atlas_err_init(err); /* recorded as a bound, not surfaced as a pass failure */
        out->available = true;
        out->bound_hit = true;
        atlas_buf_free(&last_head);
        return ATLAS_OK;
    }

    bool stale = false, unknown = false;
    st = atlas_git_tip_is_stale(g, atlas_buf_cstr(&last_head), &stale, &unknown, err);
    if (st == ATLAS_OK && (stale || unknown)) {
        out->available = true;
        out->bound_hit = true;
        atlas_git_close(g);
        atlas_buf_free(&last_head);
        return ATLAS_OK;
    }
    if (st != ATLAS_OK) {
        atlas_git_close(g);
        atlas_buf_free(&last_head);
        return st;
    }

    touched_ctx tc;
    tc.out = out;
    out->available = true;
    st = atlas_git_log_since(g, atlas_buf_cstr(&last_head), 0, NULL, touched_change_cb, &tc, err);
    atlas_git_close(g);
    atlas_buf_free(&last_head);
    return st;
}

atlas_status atlas_memory_observe(atlas_db *db, const atlas_repo_info *repo, const char *data_dir,
                                  const atlas_syspolicy *pol, atlas_memory_observation *out,
                                  atlas_err *err) {
    if (db == NULL || repo == NULL || data_dir == NULL || pol == NULL || out == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "a memory observation needs a repository, a policy and a data "
                             "directory");
    }
    atlas_memory_observation_init(out);

    size_t total = atlas_syspolicy_memory_source_count_checked(pol);
    atlas_status st = ATLAS_OK;
    for (size_t i = 0; i < total && st == ATLAS_OK && out->source_count < ATLAS_MEMORY_MAX_SOURCES;
        i++) {
        const struct atlas_syspolicy_memory_source *ms =
            atlas_syspolicy_memory_source_at_checked(pol, i);
        if (ms == NULL) {
            continue;
        }
        if (ms->repo_name[0] != '\0' && strcmp(ms->repo_name, repo->name) != 0) {
            continue; /* this policy line is about a different repository */
        }

        atlas_memory_observed_source *src = &out->sources[out->source_count];
        src->cls = ms->cls;
        st = atlas_buf_set(&src->path_raw, ms->path, strlen(ms->path), err);
        if (st == ATLAS_OK) {
            atlas_buf encoded = ATLAS_BUF_INIT;
            st = atlas_path_text_encode(ms->path, strlen(ms->path), &encoded, err);
            if (st == ATLAS_OK) {
                st = atlas_buf_set(&src->path_text, encoded.data, encoded.len, err);
            }
            atlas_buf_free(&encoded);
        }
        if (st == ATLAS_OK) {
            if (atlas_memory_source_class_is_repo(ms->cls)) {
                st = observe_repo_source(repo, data_dir, src, err);
            } else {
                st = observe_external_source(db, repo->id, src, err);
            }
        }
        if (st == ATLAS_OK) {
            out->source_count++;
        }
    }
    if (st == ATLAS_OK) {
        st = observe_touched_paths(db, repo, data_dir, &out->touched, err);
    }
    return st;
}

/* --- apply: shared state across one pass's candidates ----------------------
 *
 * `dep` is Decision 2's bookkeeping: one entry per anchored candidate already
 * emitted this pass, keyed by its normalised text's own hash so a later
 * candidate with byte-identical normalised text can find it in O(pass size)
 * rather than re-hashing every prior candidate's normalised buffer. A new
 * match links to this one and is *not* itself added again for the same
 * normalised hash beyond the first -- the chain, not the clique, Decision 2's
 * own argument needs only connectivity.
 *
 * `diffs` is T9's: every `memory_claim_diffs` row this pass has decided to
 * write, buffered because whether a generation gets appended at all -- and
 * therefore which id its diff rows reference -- is decided only once the
 * whole pass has run. `confirmed` is T9's other set: the (kind, value) hash
 * of every anchor this pass's fresh extraction still resolved, whatever
 * claim it belonged to and whatever `emit_candidate` decided about it --
 * what the vanished-anchor sweep asks *not* to re-check, because still
 * resolving is itself the answer "nothing here needs it". All three are
 * sized to the pass's own compiled ceiling
 * (ATLAS_MEMORY_MAX_SOURCES * ATLAS_MEMORY_MAX_PROPOSITIONS, the third
 * further multiplied by ATLAS_MEMORY_MAX_ANCHORS_PER_PROPOSITION) and heap
 * allocated once. */
#define DEP_UID_MAX 96
typedef struct dep_entry {
    char norm_hex[ATLAS_SHA256_HEX_LEN + 1];
    char evidence_uid[DEP_UID_MAX];
} dep_entry;

typedef struct diff_entry {
    char claim_uid[DEP_UID_MAX];
    atlas_memory_diff_kind kind;
    char reason[128];
} diff_entry;

typedef struct confirmed_anchor {
    char hash[ATLAS_SHA256_HEX_LEN + 1];
} confirmed_anchor;

typedef struct apply_ctx {
    atlas_db *db;
    const atlas_repo_info *repo;
    const atlas_memory_touched *touched;
    const char *now;
    atlas_memory_pass_result *out;
    dep_entry *dep;
    size_t dep_count;
    size_t dep_cap;
    diff_entry *diffs;
    size_t diff_count;
    size_t diff_cap;
    confirmed_anchor *confirmed;
    size_t confirmed_count;
    size_t confirmed_cap;
    bool any_change;
} apply_ctx;

/* One (kind, value) anchor tuple's identity, independent of which claim uid
 * -- across however many remints -- currently carries it. */
static atlas_status anchor_tuple_hash(atlas_memory_anchor_kind kind, const char *value,
                                      char out[ATLAS_SHA256_HEX_LEN + 1], atlas_err *err) {
    atlas_buf b = ATLAS_BUF_INIT;
    atlas_status st = atlas_buf_set_str(&b, atlas_memory_anchor_kind_name(kind), err);
    if (st == ATLAS_OK) {
        st = atlas_buf_append_ch(&b, '\x1f', err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_append_str(&b, value, err);
    }
    if (st == ATLAS_OK) {
        atlas_sha256_hex(b.data, b.len, out);
    }
    atlas_buf_free(&b);
    return st;
}

/* Deep copy: `atlas_memory_anchor_resolve` fully resets every field it owns
 * before scanning `p->text` (see its own comment), so this leaves them at
 * `_init`'s zero defaults rather than copying values that are about to be
 * discarded anyway -- only `ordinal`, `truncated` and the three buffers
 * resolve does not touch are worth carrying over. Necessary because
 * `atlas_memory_apply_in_tx` receives `obs` as `const`, by the pinned
 * signature: resolving anchors on a candidate in place would mutate an
 * observation the caller may still hold. */
static atlas_status candidate_copy(atlas_memory_proposition *dst, const atlas_memory_proposition *src,
                                   atlas_err *err) {
    atlas_memory_proposition_init(dst);
    dst->ordinal = src->ordinal;
    dst->truncated = src->truncated;
    atlas_status st = atlas_buf_set(&dst->text, src->text.data, src->text.len, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&dst->normalized, src->normalized.data, src->normalized.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&dst->text_sha256, src->text_sha256.data, src->text_sha256.len, err);
    }
    if (st != ATLAS_OK) {
        atlas_memory_proposition_free(dst);
    }
    return st;
}

static atlas_status ctx_add_dep(apply_ctx *ctx, const char *norm_hex, const char *evidence_uid,
                                atlas_err *err) {
    if (ctx->dep_count >= ctx->dep_cap) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "more anchored candidates this pass than the compiled ceiling "
                             "allows");
    }
    dep_entry *e = &ctx->dep[ctx->dep_count++];
    (void)snprintf(e->norm_hex, sizeof e->norm_hex, "%s", norm_hex);
    (void)snprintf(e->evidence_uid, sizeof e->evidence_uid, "%s", evidence_uid);
    return ATLAS_OK;
}

/* T9. Buffers one `memory_claim_diffs` row; also what makes a diff row itself
 * -- independent of any claim, evidence or attestation row landing new --
 * enough reason to append a generation: the vanished-anchor sweep can find a
 * proposition whose only anchor disappeared without a single write from the
 * per-source loop above it, and that finding is exactly the kind of change
 * Decision 7 exists to report. */
static atlas_status ctx_add_diff(apply_ctx *ctx, const char *claim_uid, atlas_memory_diff_kind kind,
                                 const char *reason, atlas_err *err) {
    if (ctx->diff_count >= ctx->diff_cap) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "more diff rows this pass than the compiled ceiling allows");
    }
    diff_entry *d = &ctx->diffs[ctx->diff_count++];
    (void)snprintf(d->claim_uid, sizeof d->claim_uid, "%s", claim_uid);
    d->kind = kind;
    (void)snprintf(d->reason, sizeof d->reason, "%s", reason != NULL ? reason : "");
    ctx->any_change = true;
    return ATLAS_OK;
}

/* T9. Marks one (kind, value) anchor as still resolving this pass -- whatever
 * `emit_candidate` went on to decide about the claim it belongs to. Silently
 * a no-op past the compiled ceiling: the cost of missing one is the
 * vanished-anchor sweep re-checking an anchor that in fact still resolves,
 * never a false claim that something vanished. */
static atlas_status ctx_mark_confirmed(apply_ctx *ctx, atlas_memory_anchor_kind kind,
                                       const char *value, atlas_err *err) {
    if (ctx->confirmed_count >= ctx->confirmed_cap) {
        return ATLAS_OK;
    }
    char hash[ATLAS_SHA256_HEX_LEN + 1];
    atlas_status st = anchor_tuple_hash(kind, value, hash, err);
    if (st != ATLAS_OK) {
        return st;
    }
    (void)snprintf(ctx->confirmed[ctx->confirmed_count++].hash, ATLAS_SHA256_HEX_LEN + 1, "%s", hash);
    return ATLAS_OK;
}

/* T9 fix-round-1: `strcmp` over two fixed `ATLAS_SHA256_HEX_LEN`-byte,
 * NUL-terminated hex strings -- `qsort`'s own comparator shape. */
static int confirmed_cmp(const void *a, const void *b) {
    return strcmp(((const confirmed_anchor *)a)->hash, ((const confirmed_anchor *)b)->hash);
}

/* Sorts once, in place, before the vanished-anchor sweep's own scan begins.
 * Every entry `ctx_mark_confirmed` will ever add is added during the
 * per-source loop, strictly before this runs (`atlas_memory_apply_in_tx`'s
 * own ordering), so one sort here is enough for every `ctx_is_confirmed`
 * lookup the sweep makes afterwards. */
static void ctx_sort_confirmed(apply_ctx *ctx) {
    if (ctx->confirmed_count > 1) {
        qsort(ctx->confirmed, ctx->confirmed_count, sizeof ctx->confirmed[0], confirmed_cmp);
    }
}

/* T9 fix-round-1: binary search, not a linear scan. At the compiled ceiling
 * (`ATLAS_MEMORY_MAX_SOURCES * ATLAS_MEMORY_MAX_PROPOSITIONS *
 * ATLAS_MEMORY_MAX_ANCHORS_PER_PROPOSITION` confirmed entries, checked once
 * per distinct anchor tuple the vanished-anchor sweep visits) the previous
 * linear form was O(n^2) -- measured as the reviewer's own arithmetic at
 * 16384 entries, which a `strcmp`-per-pair scan cannot absorb on the single
 * writer thread. `ctx_sort_confirmed` must have run first; nothing here
 * checks that it has; both of its production call sites are ordered so it
 * always has by the time this is reached. */
static bool ctx_is_confirmed(const apply_ctx *ctx, const char *hash) {
    confirmed_anchor key;
    (void)snprintf(key.hash, sizeof key.hash, "%s", hash);
    return bsearch(&key, ctx->confirmed, ctx->confirmed_count, sizeof ctx->confirmed[0],
                   confirmed_cmp) != NULL;
}

/* T9, fixed in fix-round-1 (C1). `atlas_db_memory_anchor_claim_uids` reports
 * every claim uid ever anchored to this (kind, value) tuple, oldest first --
 * and one anchor value can legitimately belong to more than one *live*
 * proposition (two memory bullets naming the same file is the ordinary case,
 * not an edge one). The first version of this callback kept overwriting on
 * every non-`skip` callback, so it answered "the last claim anyone anchored
 * here" rather than "this proposition's own predecessor" -- after any commit,
 * every claim re-mints (S27's basis_commit), both bullets' fresh claims would
 * probe the same anchor value, and whichever one is not actually the tuple's
 * most recent claim got a spurious ADDED row for an unchanged proposition,
 * which is exactly the absence acceptance item 2 stands on.
 *
 * `match_text`/`match_text_len` is the caller's own proposition text when one
 * is known (`classify_candidate`'s call): the callback fetches each
 * candidate's stored row and keeps only the *last* (most recent, since the
 * anchor listing is oldest-first) one whose text matches byte for byte, so
 * two propositions sharing one anchor resolve to their own history rather
 * than each other's. `match_text == NULL` (the vanished-anchor sweep's call,
 * which has no fresh candidate to match against at all) keeps the original
 * "most recent claim, whichever it is" behaviour -- there is no proposition
 * text to disambiguate against when the referent itself is what vanished. */
struct find_prior_ctx {
    atlas_db *db;
    char uid[DEP_UID_MAX];
    bool found;
    const char *skip;
    const char *match_text;
    size_t match_text_len;
};

static atlas_status find_prior_cb(const char *claim_uid, void *ud, atlas_err *err) {
    struct find_prior_ctx *fc = ud;
    if (fc->skip != NULL && strcmp(claim_uid, fc->skip) == 0) {
        return ATLAS_OK;
    }
    if (fc->match_text == NULL) {
        (void)snprintf(fc->uid, sizeof fc->uid, "%s", claim_uid);
        fc->found = true;
        return ATLAS_OK;
    }
    atlas_verify_claim cand;
    atlas_verify_claim_init(&cand);
    bool cfound = false;
    atlas_status st = atlas_db_verify_claim_find(fc->db, claim_uid, &cand, &cfound, err);
    if (st != ATLAS_OK) {
        atlas_verify_claim_free(&cand);
        return st;
    }
    bool matches = cfound && cand.text.len == fc->match_text_len &&
                  memcmp(cand.text.data != NULL ? cand.text.data : "", fc->match_text,
                        fc->match_text_len) == 0;
    atlas_verify_claim_free(&cand);
    if (matches) {
        (void)snprintf(fc->uid, sizeof fc->uid, "%s", claim_uid);
        fc->found = true;
    }
    return ATLAS_OK;
}

/* T9. `EVIDENCE_PRODUCE` then `EVALUATE`, through the write point and nothing
 * else -- the brief's own instruction, so drift arrives through T5's producer
 * with no new mechanics. Both on channel ATLAS: neither op derives an actor
 * from it (`atlas_verify_intake_apply_in_tx` skips `derive_actor` for both
 * kinds), so this is the channel refusal's only concern here and ATLAS is
 * what Atlas performing its own mechanical check has always meant. */
static atlas_status evaluate_claim(apply_ctx *ctx, const char *claim_uid, atlas_verify_check *check_out,
                                   atlas_verify_conflict *conflict_out, atlas_err *err) {
    if (check_out != NULL) {
        *check_out = ATLAS_CHECK_UNAVAILABLE;
    }
    if (conflict_out != NULL) {
        *conflict_out = ATLAS_CONFLICT_NONE;
    }
    atlas_verify_op op1;
    atlas_verify_op_init(&op1);
    op1.kind = ATLAS_VERIFY_OP_EVIDENCE_PRODUCE;
    op1.channel = ATLAS_VERIFY_CHANNEL_ATLAS;
    atlas_status st = atlas_buf_set_str(&op1.claim_uid, claim_uid, err);
    atlas_verify_intake_result res1;
    atlas_verify_intake_result_init(&res1);
    if (st == ATLAS_OK) {
        st = atlas_verify_intake_apply_in_tx(ctx->db, &op1, &res1, err);
    }
    atlas_verify_op_free(&op1);
    atlas_verify_intake_result_free(&res1);
    if (st != ATLAS_OK) {
        return st;
    }

    atlas_verify_op op2;
    atlas_verify_op_init(&op2);
    op2.kind = ATLAS_VERIFY_OP_EVALUATE;
    op2.channel = ATLAS_VERIFY_CHANNEL_ATLAS;
    st = atlas_buf_set_str(&op2.claim_uid, claim_uid, err);
    atlas_verify_intake_result res2;
    atlas_verify_intake_result_init(&res2);
    if (st == ATLAS_OK) {
        st = atlas_verify_intake_apply_in_tx(ctx->db, &op2, &res2, err);
    }
    atlas_verify_op_free(&op2);
    if (st == ATLAS_OK) {
        if (check_out != NULL) {
            *check_out = res2.assessment.check;
        }
        if (conflict_out != NULL) {
            *conflict_out = res2.assessment.aggregate.conflict;
        }
    }
    atlas_verify_intake_result_free(&res2);
    return st;
}

/* T9's semantic diff, settling the item-2 tension: `p`'s freshly-minted claim
 * (`claim_uid`) may be a genuinely new proposition, the very same proposition
 * carried forward at a new `basis_commit` because the repository's head
 * moved (S27's content key hashes it, `src/verify/intake.c:643`), or a
 * proposition whose checkable fact moved under it. Identity across a remint
 * is never the claim uid -- a fresh uid is exactly what a head move
 * produces regardless of which case this is -- so it is established from
 * `memory_claim_anchors` (a repository fact's own identity, independent of
 * which claim row currently cites it) confirmed by an exact text match
 * (fixed in fix-round-1, C1: `find_prior_cb` now fetches and compares text
 * for every candidate anchored here rather than trusting whichever one the
 * anchor listing happened to report last), never from anchor-sharing alone:
 * two different propositions can legitimately name the same path.
 * `claim_new == false` means the write point resolved to a row that already
 * existed byte-for-byte, so there is nothing to classify at all -- item 2's
 * stability for a `SOURCE_REVISION` or `DECISION_REVISION` pass holds by
 * exactly this construction, with no special case here. */
static atlas_status classify_candidate(apply_ctx *ctx, const atlas_memory_proposition *p,
                                       const char *claim_uid, bool claim_new, atlas_err *err) {
    if (!claim_new) {
        return ATLAS_OK;
    }
    if (p->anchor_count == 0) {
        return ATLAS_OK; /* unreachable in practice: emit_candidate requires an anchor */
    }

    int64_t doc_id = 0, rev_id = 0;
    atlas_status st = ATLAS_OK;
    if (p->decision_uid.len > 0) {
        int64_t doc_repo = 0;
        bool doc_found = false;
        st = atlas_db_decision_find_uid(ctx->db, atlas_buf_cstr(&p->decision_uid), &doc_id, &doc_repo,
                                        &doc_found, err);
        if (st == ATLAS_OK && doc_found) {
            st = atlas_db_decision_approved_revision(ctx->db, doc_id, &rev_id, err);
        } else if (st == ATLAS_OK) {
            doc_id = 0;
        }
    }
    if (st != ATLAS_OK) {
        return st;
    }

    struct find_prior_ctx fc;
    fc.db = ctx->db;
    fc.found = false;
    fc.uid[0] = '\0';
    fc.skip = claim_uid;
    fc.match_text = p->text.data != NULL ? p->text.data : "";
    fc.match_text_len = p->text.len;
    st = atlas_db_memory_anchor_claim_uids(ctx->db, ctx->repo->id, p->anchors[0].kind,
                                           atlas_buf_cstr(&p->anchors[0].value), find_prior_cb, &fc, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (!fc.found) {
        /* No live claim anchored here carries this proposition's own text --
         * genuinely new, whether because nothing was ever anchored here or
         * because every claim that was is a different proposition. */
        return ctx_add_diff(ctx, claim_uid, ATLAS_MEMORY_DIFF_ADDED, "", err);
    }

    /* `claim_uid` now carries this one (anchors[0].kind, anchors[0].value)
     * tuple forward from `fc.uid` -- true regardless of which diff kind this
     * function goes on to choose below, so pruned once, here, rather than
     * once per branch. Only this one tuple: `fc.uid` may carry other anchors
     * this candidate does not (a DECISION anchor beside a SYMBOL one, say),
     * and those are exactly what the vanished-anchor sweep still needs to
     * see if nothing else confirms them this pass. */
    st = atlas_db_memory_anchor_prune_one(ctx->db, ctx->repo->id, fc.uid, p->anchors[0].kind,
                                          atlas_buf_cstr(&p->anchors[0].value), err);
    if (st != ATLAS_OK) {
        return st;
    }

    atlas_verify_claim prior;
    atlas_verify_claim_init(&prior);
    bool prior_found = false;
    st = atlas_db_verify_claim_find(ctx->db, fc.uid, &prior, &prior_found, err);
    if (st != ATLAS_OK) {
        atlas_verify_claim_free(&prior);
        return st;
    }
    if (!prior_found) {
        /* find_prior_cb already confirmed this uid's text under the
         * single-writer rule (A1); a row gone by the time it is re-fetched
         * here cannot happen in production, but failing open into "new"
         * would silently drop the predecessor's identity rather than fail
         * closed on a state this function did not expect. */
        atlas_verify_claim_free(&prior);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "a claim confirmed live by its own text vanished before it could be "
                             "re-read");
    }

    bool verifier_input_equal =
        prior.verifier_input.len == p->verifier_input.len &&
        memcmp(prior.verifier_input.data != NULL ? prior.verifier_input.data : "",
               p->verifier_input.data != NULL ? p->verifier_input.data : "", p->verifier_input.len) ==
            0;
    bool decision_equal = prior.document_id == doc_id && prior.revision_id == rev_id;
    bool semantics_equal = prior.semantics == p->semantics;

    if (!decision_equal) {
        /* The revision bound to this claim moved (a `DECISION_REVISION`
         * pass's own effect on the claims it touches) with nothing mechanical
         * to re-check either way. */
        atlas_verify_claim_free(&prior);
        return ctx_add_diff(ctx, claim_uid, ATLAS_MEMORY_DIFF_CHANGED, "", err);
    }

    /* T9 fix-round-1 (C2): a bullet naming both a SYMBOL and a PATH gets a
     * `symbol=` verifier input (`src/memory/extract.c`, Decision 4's own
     * precedence), which does not move when the file changes and the symbol
     * does not -- so `verifier_input_equal` alone would call this a pure
     * remint even though a commit just rewrote the very file this
     * proposition also names. `ctx->touched` is this pass's own bounded
     * `git diff --name-only <last generation's head> <head>` set (built in
     * observe, never here); a PATH anchor found in it means the commit range
     * that produced this remint touched this proposition's own referent,
     * whatever the SYMBOL-derived verifier input says. `available == false`
     * (no `COMMIT` cause is even possible this pass) leaves `path_touched`
     * false, so a `SOURCE_REVISION`/`DECISION_REVISION` pass is unaffected. */
    bool path_touched = false;
    for (size_t i = 0; i < p->anchor_count && !path_touched; i++) {
        if (p->anchors[i].kind == ATLAS_MEMORY_ANCHOR_PATH) {
            path_touched = atlas_memory_touched_contains(ctx->touched, atlas_buf_cstr(&p->anchors[i].value));
        }
    }
    bool fact_touched = !verifier_input_equal || path_touched;

    if (!fact_touched && semantics_equal) {
        /* The item-2 settlement: same proposition, same checkable fact, same
         * classification -- only `basis_commit` moved. A new claim row
         * exists (a head move re-mints unconditionally, T8's own measured
         * fact), and it is deliberately given no diff row -- "byte-for-byte
         * stable" is asserted of the diff surface here, not of the
         * `verify_claims` row, which is new by construction. */
        atlas_verify_claim_free(&prior);
        return ATLAS_OK;
    }

    if (!fact_touched) {
        /* Only Decision 4's own classification moved (semantics), with no
         * mechanical fact to re-check. */
        atlas_verify_claim_free(&prior);
        return ctx_add_diff(ctx, claim_uid, ATLAS_MEMORY_DIFF_CHANGED, "", err);
    }
    atlas_verify_claim_free(&prior);

    if (p->verifier == ATLAS_VERIFIER_NONE) {
        return ctx_add_diff(ctx, claim_uid, ATLAS_MEMORY_DIFF_CHANGED, "", err);
    }

    /* The fresh claim already carries the current input (anchor_resolve just
     * built it from the live index), so evaluating *it* is evaluating the
     * current state of the world. */
    atlas_verify_check check = ATLAS_CHECK_UNAVAILABLE;
    atlas_verify_conflict conflict = ATLAS_CONFLICT_NONE;
    st = evaluate_claim(ctx, claim_uid, &check, &conflict, err);
    if (st != ATLAS_OK) {
        return st;
    }

    if (conflict == ATLAS_CONFLICT_IMPLEMENTATION) {
        char reason[128];
        (void)snprintf(reason, sizeof reason, "DRIFT %s", atlas_buf_cstr(&p->decision_uid));
        return ctx_add_diff(ctx, claim_uid, ATLAS_MEMORY_DIFF_CONTRADICTED, reason, err);
    }
    if (check == ATLAS_CHECK_FAIL || conflict == ATLAS_CONFLICT_CONTRADICTION) {
        return ctx_add_diff(ctx, claim_uid, ATLAS_MEMORY_DIFF_CONTRADICTED, "", err);
    }
    if (check == ATLAS_CHECK_PASS) {
        return ctx_add_diff(ctx, claim_uid, ATLAS_MEMORY_DIFF_IMPACTED, "", err);
    }
    return ctx_add_diff(ctx, claim_uid, ATLAS_MEMORY_DIFF_UNDETERMINED, "", err);
}

/* One candidate, already known to be anchored and citable, through the one
 * write point, in the op order the write point requires (CLAIM_CREATE first:
 * `op_evidence_add` resolves its claim before anything else). */
static atlas_status emit_candidate(apply_ctx *ctx, const atlas_memory_observed_source *src,
                                   const char *source_uid, const char *item_path_text,
                                   const char *memory_version_uid, const char *observed_at,
                                   const atlas_memory_proposition *p, atlas_err *err) {
    /* T9's vanished-anchor sweep asks "did this pass's fresh extraction still
     * resolve this repository fact", so every anchor a candidate resolved is
     * marked confirmed here, before anything about the claim is decided --
     * the world validated the anchor whether the claim that follows turns out
     * to be new, unchanged or reclassified. */
    atlas_status confirm_st = ATLAS_OK;
    for (size_t i = 0; confirm_st == ATLAS_OK && i < p->anchor_count; i++) {
        confirm_st =
            ctx_mark_confirmed(ctx, p->anchors[i].kind, atlas_buf_cstr(&p->anchors[i].value), err);
    }
    if (confirm_st != ATLAS_OK) {
        return confirm_st;
    }

    atlas_verify_op op1;
    atlas_verify_op_init(&op1);
    op1.kind = ATLAS_VERIFY_OP_CLAIM_CREATE;
    op1.channel = ATLAS_VERIFY_CHANNEL_DOCUMENT;
    atlas_status st = atlas_buf_set(&op1.root, ctx->repo->root_path.data, ctx->repo->root_path.len, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op1.actor_name, source_uid, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op1.actor_provider, "memory", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op1.actor_family, atlas_memory_source_class_name(src->cls), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op1.domain, "memory", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&op1.text, p->text.data, p->text.len, err);
    }
    if (st == ATLAS_OK) {
        op1.semantics = p->semantics;
        op1.semantics_given = true;
        if (p->verifier != ATLAS_VERIFIER_NONE) {
            st = atlas_buf_set_str(&op1.verifier, atlas_verify_verifier_name(p->verifier), err);
            if (st == ATLAS_OK) {
                st = atlas_buf_set(&op1.verifier_input, p->verifier_input.data,
                                   p->verifier_input.len, err);
            }
        }
    }
    if (st == ATLAS_OK && p->decision_uid.len > 0) {
        st = atlas_buf_set(&op1.document_uid, p->decision_uid.data, p->decision_uid.len, err);
    }
    atlas_verify_intake_result res1;
    atlas_verify_intake_result_init(&res1);
    if (st == ATLAS_OK) {
        st = atlas_verify_intake_apply_in_tx(ctx->db, &op1, &res1, err);
    }
    atlas_verify_op_free(&op1);
    if (st != ATLAS_OK) {
        atlas_verify_intake_result_free(&res1);
        return st;
    }
    if (res1.duplicate) {
        ctx->out->claims_resolved++;
    } else {
        ctx->out->claims_created++;
    }
    bool claim_new = !res1.duplicate;
    char claim_uid[DEP_UID_MAX];
    (void)snprintf(claim_uid, sizeof claim_uid, "%s", atlas_buf_cstr(&res1.uid));
    atlas_verify_intake_result_free(&res1);

    /* EVIDENCE_ADD, channel ATLAS: Atlas performed the reading. Debt 1 lands
     * here -- `actor_version` is the extractor epoch, the `intake.c:873`
     * synthetic-op pattern, and it is what makes a later bump to
     * ATLAS_MEMORY_EXTRACTOR_VERSION mint a new speaker rather than being a
     * silent no-op the way ATLAS_SEM_ANALYZER_VERSION was for years. */
    atlas_verify_op op2;
    atlas_verify_op_init(&op2);
    op2.kind = ATLAS_VERIFY_OP_EVIDENCE_ADD;
    op2.channel = ATLAS_VERIFY_CHANNEL_ATLAS;
    st = atlas_buf_set_str(&op2.actor_name, "memory-reconciler", err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op2.actor_provider, "atlas", err);
    }
    if (st == ATLAS_OK) {
        char ver[16];
        (void)snprintf(ver, sizeof ver, "%d", ATLAS_MEMORY_EXTRACTOR_VERSION);
        st = atlas_buf_set_str(&op2.actor_version, ver, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op2.claim_uid, claim_uid, err);
    }
    if (st == ATLAS_OK) {
        op2.evidence_class = ATLAS_EVIDENCE_DOCUMENT;
        if (memory_version_uid != NULL) {
            st = atlas_buf_set_str(&op2.memory_version_uid, memory_version_uid, err);
        } else {
            st = atlas_buf_set_str(&op2.path_text, item_path_text, err);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op2.observed_at, observed_at, err);
    }
    atlas_verify_intake_result res2;
    atlas_verify_intake_result_init(&res2);
    if (st == ATLAS_OK) {
        st = atlas_verify_intake_apply_in_tx(ctx->db, &op2, &res2, err);
    }
    atlas_verify_op_free(&op2);
    if (st != ATLAS_OK) {
        atlas_verify_intake_result_free(&res2);
        return st;
    }
    char evidence_uid[DEP_UID_MAX];
    (void)snprintf(evidence_uid, sizeof evidence_uid, "%s", atlas_buf_cstr(&res2.uid));
    bool evidence_new = !res2.duplicate;
    atlas_verify_intake_result_free(&res2);

    /* ATTESTATION_ADD, channel DOCUMENT: the same speaker as CLAIM_CREATE. */
    atlas_verify_op op3;
    atlas_verify_op_init(&op3);
    op3.kind = ATLAS_VERIFY_OP_ATTESTATION_ADD;
    op3.channel = ATLAS_VERIFY_CHANNEL_DOCUMENT;
    st = atlas_buf_set_str(&op3.actor_name, source_uid, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op3.actor_provider, "memory", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op3.actor_family, atlas_memory_source_class_name(src->cls), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op3.claim_uid, claim_uid, err);
    }
    if (st == ATLAS_OK) {
        op3.verdict = ATLAS_ATTEST_SUPPORT;
        op3.self_confidence = -1;
        st = atlas_buf_set_str(&op3.method, "memory-extraction", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op3.evidence_uids, evidence_uid, err);
    }
    atlas_verify_intake_result res3;
    atlas_verify_intake_result_init(&res3);
    if (st == ATLAS_OK) {
        st = atlas_verify_intake_apply_in_tx(ctx->db, &op3, &res3, err);
    }
    atlas_verify_op_free(&op3);
    bool attestation_new = st == ATLAS_OK && !res3.duplicate;
    atlas_verify_intake_result_free(&res3);
    if (st != ATLAS_OK) {
        return st;
    }

    /* DEPENDENCY_ADD, channel ATLAS -- Decision 2. Linked to the first prior
     * candidate this pass whose normalised text is byte-identical, if any;
     * the self-edge two propositions sharing this *same* evidence row would
     * produce (identical path, commit and actor -- the same memory file read
     * twice in one pass, cited by two byte-identical candidates) is refused
     * by `atlas_db_verify_evidence_dep_add` and is skipped here rather than
     * asked for, since it is not evidence deriving from anything. */
    char norm_hex[ATLAS_SHA256_HEX_LEN + 1];
    atlas_sha256_hex(p->normalized.data != NULL ? p->normalized.data : "", p->normalized.len,
                     norm_hex);
    for (size_t i = 0; st == ATLAS_OK && i < ctx->dep_count; i++) {
        if (strcmp(ctx->dep[i].norm_hex, norm_hex) == 0) {
            if (strcmp(ctx->dep[i].evidence_uid, evidence_uid) != 0) {
                atlas_verify_op op4;
                atlas_verify_op_init(&op4);
                op4.kind = ATLAS_VERIFY_OP_DEPENDENCY_ADD;
                op4.channel = ATLAS_VERIFY_CHANNEL_ATLAS;
                st = atlas_buf_set_str(&op4.derived_uid, evidence_uid, err);
                if (st == ATLAS_OK) {
                    st = atlas_buf_set_str(&op4.source_uid, ctx->dep[i].evidence_uid, err);
                }
                atlas_verify_intake_result res4;
                atlas_verify_intake_result_init(&res4);
                if (st == ATLAS_OK) {
                    st = atlas_verify_intake_apply_in_tx(ctx->db, &op4, &res4, err);
                }
                atlas_verify_op_free(&op4);
                atlas_verify_intake_result_free(&res4);
            }
            break;
        }
    }
    if (st == ATLAS_OK) {
        st = ctx_add_dep(ctx, norm_hex, evidence_uid, err);
    }

    /* Anchors, written through db_memory.c -- the only production writer of
     * `memory_claim_anchors`. */
    for (size_t i = 0; st == ATLAS_OK && i < p->anchor_count; i++) {
        st = atlas_db_memory_anchor_add(ctx->db, ctx->repo->id, claim_uid, p->anchors[i].kind,
                                        atlas_buf_cstr(&p->anchors[i].value), err);
    }

    if (st == ATLAS_OK) {
        st = classify_candidate(ctx, p, claim_uid, claim_new, err);
    }
    /* A re-run over unchanged bytes must not itself count as a change: every
     * one of the three ops above resolved to an existing row via its own
     * content key, §27's duplicate discipline, so nothing about this
     * candidate is new. Decision 7's own rule -- "a pass that changed
     * nothing appends no generation" -- is checkable only if this bit means
     * exactly that. */
    if (claim_new || evidence_new || attestation_new) {
        ctx->any_change = true;
    }
    return st;
}

/* A short, stable label for a read outcome, for `last_read_obstacle` --
 * informational only, never stored, so this is not one of the five closed
 * vocabularies and carries no `_parse` twin. Still no `default:`: a new
 * outcome added to `atlas_memory_read_outcome` must fail this build too,
 * the same discipline every switch over a closed enum in this codebase
 * follows. `OK` and `ABSENT` never reach here (see the two call sites). */
static const char *read_outcome_label(atlas_memory_read_outcome o) {
    switch (o) {
    case ATLAS_MEMORY_READ_UNKNOWN: return "UNKNOWN";
    case ATLAS_MEMORY_READ_OK: return "OK";
    case ATLAS_MEMORY_READ_ABSENT: return "ABSENT";
    case ATLAS_MEMORY_READ_TOO_LARGE: return "TOO_LARGE";
    case ATLAS_MEMORY_READ_NOT_OURS: return "NOT_OURS";
    case ATLAS_MEMORY_READ_NO_MIRROR: return "NO_MIRROR";
    case ATLAS_MEMORY_READ_SYMLINK: return "SYMLINK";
    case ATLAS_MEMORY_READ_NOT_MIRRORED: return "NOT_MIRRORED";
    }
    return "UNKNOWN";
}

/* One source: materialise its row, version its items, and emit or unanchor
 * every candidate T7 split out of it. */
static atlas_status apply_one_source(apply_ctx *ctx, const atlas_memory_observed_source *src,
                                     atlas_err *err) {
    ctx->out->sources_seen++;
    if (src->bound_hit) {
        ctx->out->sources_bound_hit = true;
    }

    int64_t source_id = 0;
    atlas_buf source_uid = ATLAS_BUF_INIT;
    atlas_status st = atlas_db_memory_source_upsert(ctx->db, ctx->repo->id, src->cls,
                                                    src->path_raw.data, src->path_raw.len,
                                                    atlas_buf_cstr(&src->path_text), ctx->now,
                                                    &source_id, &source_uid, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&source_uid);
        return st;
    }

    if (src->is_external) {
        if (!src->external_latest_found) {
            atlas_buf_free(&source_uid);
            return ATLAS_OK; /* nothing has ever been put for this policy entry */
        }
        for (size_t k = 0; st == ATLAS_OK && k < src->candidate_count; k++) {
            atlas_memory_proposition p;
            st = candidate_copy(&p, &src->candidates[k], err);
            if (st != ATLAS_OK) {
                break;
            }
            st = atlas_memory_anchor_resolve(ctx->db, ctx->repo->id, &p, err);
            if (st == ATLAS_OK && p.anchor_count == 0) {
                bool landed = false;
                st = atlas_db_memory_unanchored_add(
                    ctx->db, src->external_latest.id, (int64_t)p.ordinal,
                    atlas_buf_cstr(&p.text_sha256), p.text.data, p.text.len, &landed, err);
                if (st == ATLAS_OK && landed) {
                    ctx->out->unanchored++;
                }
            } else if (st == ATLAS_OK) {
                st = emit_candidate(ctx, src, atlas_buf_cstr(&source_uid), NULL,
                                    atlas_buf_cstr(&src->external_latest.version_uid),
                                    atlas_buf_cstr(&src->external_latest.observed_at), &p, err);
            }
            atlas_memory_proposition_free(&p);
        }
        atlas_buf_free(&source_uid);
        return st;
    }

    /* REPO_*: version every item that read OK, and note which items' own
     * path is indexed -- EVIDENCE_ADD's `path_text` lookup needs that, and it
     * is a different question from whether any anchor *inside* the text
     * resolves. An item whose own path is not indexed (a gitignored `*_DIR`
     * child is the case T6's fixtures are built around) still gets a version
     * row -- Atlas' own record of what it read does not depend on whether
     * the bytes can be cited as evidence -- but every candidate split from it
     * is routed to `memory_unanchored` rather than into the claim pipeline,
     * because `op_evidence_add`'s index lookup would refuse it regardless of
     * what the candidate's own anchors resolve to, and a claim with no
     * evidence for its own source is worse than an honestly unanchored
     * record. */
    int64_t item_version_id[ATLAS_MEMORY_MAX_DIR_ENTRIES];
    atlas_buf item_version_uid[ATLAS_MEMORY_MAX_DIR_ENTRIES];
    atlas_buf item_observed_at[ATLAS_MEMORY_MAX_DIR_ENTRIES];
    atlas_buf item_path_text[ATLAS_MEMORY_MAX_DIR_ENTRIES];
    bool item_indexed[ATLAS_MEMORY_MAX_DIR_ENTRIES];
    for (size_t i = 0; i < ATLAS_MEMORY_MAX_DIR_ENTRIES; i++) {
        item_version_id[i] = 0;
        atlas_buf_init(&item_version_uid[i]);
        atlas_buf_init(&item_observed_at[i]);
        atlas_buf_init(&item_path_text[i]);
        item_indexed[i] = false;
    }

    /* I1: an empty mirror-backed `*_DIR` listing has no item of its own
     * (T6's NF3) -- the one case a read obstacle can exist with nothing in
     * `items[]` to carry it, exactly the gap `from_mirror_out` on the call
     * closes for a reader. A tree-direct empty directory (`from_mirror ==
     * false`) is a real look that found nothing and is not one of these. */
    if (src->item_count == 0 && src->from_mirror) {
        ctx->out->read_obstacles++;
        /* Outcome first, deliberately -- see the struct's own comment in
         * memory.h: a long %XX-encoded path can consume the whole buffer,
         * and the outcome is what must survive that. There is no single
         * ATLAS_MEMORY_READ_* member for "empty mirror-backed listing" (no
         * item exists to carry one), so this is the one obstacle
         * description not built from read_outcome_label. */
        (void)snprintf(ctx->out->last_read_obstacle, sizeof ctx->out->last_read_obstacle,
                      "EMPTY_MIRROR_LISTING: %s", atlas_buf_cstr(&src->path_text));
    }

    for (size_t i = 0; st == ATLAS_OK && i < src->item_count; i++) {
        const atlas_memory_observed_item *it = &src->items[i];
        if (it->outcome != ATLAS_MEMORY_READ_OK) {
            /* ABSENT is a real look that found nothing -- not an obstacle.
             * Every other non-OK outcome (NO_MIRROR, NOT_MIRRORED,
             * TOO_LARGE, SYMLINK) is this process failing to see what is
             * actually there, and must not read the same as "unchanged". */
            if (it->outcome != ATLAS_MEMORY_READ_ABSENT) {
                ctx->out->read_obstacles++;
                /* Outcome first -- see the struct's own comment in memory.h:
                 * a long %XX-encoded path can consume the whole buffer, and
                 * the outcome is what must survive that. */
                (void)snprintf(ctx->out->last_read_obstacle, sizeof ctx->out->last_read_obstacle,
                              "%s: %s", read_outcome_label(it->outcome),
                              atlas_buf_cstr(&src->path_text));
            }
            continue;
        }

        /* This item's own repository-relative path: the source's own path
         * for a *_FILE class, or the source's path plus this DIR child's
         * name -- encoded as one raw concatenation, per-byte %XX encoding
         * carries no cross-byte state so encoding the two pieces separately
         * and joining with a literal '/' is the same bytes as encoding the
         * join directly. */
        if (it->rel_path.len > 0) {
            atlas_buf enc = ATLAS_BUF_INIT;
            st = atlas_path_text_encode(it->rel_path.data, it->rel_path.len, &enc, err);
            if (st == ATLAS_OK) {
                st = atlas_buf_set(&item_path_text[i], src->path_text.data, src->path_text.len, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_buf_append_ch(&item_path_text[i], '/', err);
            }
            if (st == ATLAS_OK) {
                st = atlas_buf_append(&item_path_text[i], enc.data, enc.len, err);
            }
            atlas_buf_free(&enc);
        } else {
            st = atlas_buf_set(&item_path_text[i], src->path_text.data, src->path_text.len, err);
        }
        if (st != ATLAS_OK) {
            break;
        }

        atlas_buf ignored_hash = ATLAS_BUF_INIT;
        bool found = false;
        st = atlas_db_verify_file_hash(ctx->db, ctx->repo->id, atlas_buf_cstr(&item_path_text[i]),
                                       &ignored_hash, &found, err);
        atlas_buf_free(&ignored_hash);
        item_indexed[i] = found;
        if (st != ATLAS_OK) {
            break;
        }

        bool exists = false;
        int64_t existing_id = 0;
        atlas_buf existing_uid = ATLAS_BUF_INIT;
        atlas_buf existing_observed = ATLAS_BUF_INIT;
        st = atlas_db_memory_version_exists(ctx->db, source_id, atlas_buf_cstr(&it->content_sha256),
                                            &exists, &existing_id, &existing_uid,
                                            &existing_observed, err);
        if (st == ATLAS_OK && exists) {
            item_version_id[i] = existing_id;
            st = atlas_buf_set(&item_version_uid[i], existing_uid.data, existing_uid.len, err);
            if (st == ATLAS_OK) {
                st = atlas_buf_set(&item_observed_at[i], existing_observed.data,
                                   existing_observed.len, err);
            }
        } else if (st == ATLAS_OK) {
            int64_t new_id = 0;
            atlas_buf new_uid = ATLAS_BUF_INIT;
            st = atlas_db_memory_version_insert(
                ctx->db, source_id, atlas_buf_cstr(&it->commit_oid), atlas_buf_cstr(&it->blob_oid),
                atlas_buf_cstr(&it->content_sha256), it->content_bytes,
                it->bytes.data, it->bytes.len, ctx->now, ctx->now, (int64_t)geteuid(), &new_id,
                &new_uid, err);
            if (st == ATLAS_OK) {
                item_version_id[i] = new_id;
                st = atlas_buf_set(&item_version_uid[i], new_uid.data, new_uid.len, err);
                if (st == ATLAS_OK) {
                    st = atlas_buf_set(&item_observed_at[i], ctx->now, strlen(ctx->now), err);
                }
                if (st == ATLAS_OK) {
                    ctx->out->versions_added++;
                    ctx->any_change = true;
                }
            }
            atlas_buf_free(&new_uid);
        }
        atlas_buf_free(&existing_uid);
        atlas_buf_free(&existing_observed);
    }

    for (size_t k = 0; st == ATLAS_OK && k < src->candidate_count; k++) {
        size_t item_idx = src->candidate_item[k];
        const atlas_memory_observed_item *it = &src->items[item_idx];
        if (it->outcome != ATLAS_MEMORY_READ_OK || item_version_id[item_idx] == 0) {
            continue; /* this item never got a version row: nothing to attach to */
        }

        if (!item_indexed[item_idx]) {
            /* I2, the trade-off written down rather than only chosen: T8
             * could instead cite this item by `memory_version_uid` -- Atlas'
             * own record, already written above, and immune to whether
             * `files` happens to index this path -- exactly as an EXTERNAL_*
             * source's evidence does. Not done: the brief's pinned op
             * description reads "for a repository source, path_text ... for
             * an external source, memory_version_uid", and blurring that
             * line would make a REPO_*-sourced evidence row structurally
             * indistinguishable from an EXTERNAL_* one, which a later
             * consumer (T9's SUPERSEDED detection reads a source's *indexed*
             * path history) would then have to special-case rather than
             * being able to assume from the evidence's own shape. Routing to
             * `memory_unanchored` instead costs sharing one table -- and one
             * counter -- with the genuinely-prose-only case below; the two
             * are distinguishable only by re-deriving `item_indexed` from
             * the source's own read outcome, which is unresolved as its own
             * finding rather than silently absorbed. */
            const atlas_memory_proposition *cp = &src->candidates[k];
            bool landed = false;
            st = atlas_db_memory_unanchored_add(ctx->db, item_version_id[item_idx],
                                                (int64_t)cp->ordinal,
                                                atlas_buf_cstr(&cp->text_sha256), cp->text.data,
                                                cp->text.len, &landed, err);
            if (st == ATLAS_OK && landed) {
                ctx->out->unanchored++;
            }
            continue;
        }

        atlas_memory_proposition p;
        st = candidate_copy(&p, &src->candidates[k], err);
        if (st != ATLAS_OK) {
            break;
        }
        st = atlas_memory_anchor_resolve(ctx->db, ctx->repo->id, &p, err);
        if (st == ATLAS_OK && p.anchor_count == 0) {
            bool landed = false;
            st = atlas_db_memory_unanchored_add(ctx->db, item_version_id[item_idx],
                                                (int64_t)p.ordinal, atlas_buf_cstr(&p.text_sha256),
                                                p.text.data, p.text.len, &landed, err);
            if (st == ATLAS_OK && landed) {
                ctx->out->unanchored++;
            }
        } else if (st == ATLAS_OK) {
            /* One more defensive bound, checked here rather than discovered
             * at the write point: every candidate this pass can produce is
             * at most ATLAS_MEMORY_MAX_PROPOSITION_BYTES (2048), which is
             * below ATLAS_VERIFY_CLAIM_TEXT_MAX (4096) today, so this should
             * never fire -- but "refused, never silently exceeded" is A5's
             * rule regardless of whether the compiled bounds happen to make
             * a check unreachable right now. */
            if (p.text.len > ATLAS_VERIFY_CLAIM_TEXT_MAX) {
                bool landed = false;
                ctx->out->intake_bound_hits++;
                st = atlas_db_memory_unanchored_add(ctx->db, item_version_id[item_idx],
                                                    (int64_t)p.ordinal,
                                                    atlas_buf_cstr(&p.text_sha256), p.text.data,
                                                    p.text.len, &landed, err);
                if (st == ATLAS_OK && landed) {
                    ctx->out->unanchored++;
                }
            } else {
                st = emit_candidate(ctx, src, atlas_buf_cstr(&source_uid),
                                    atlas_buf_cstr(&item_path_text[item_idx]), NULL,
                                    atlas_buf_cstr(&item_observed_at[item_idx]), &p, err);
            }
        }
        atlas_memory_proposition_free(&p);
    }

    for (size_t i = 0; i < ATLAS_MEMORY_MAX_DIR_ENTRIES; i++) {
        atlas_buf_free(&item_version_uid[i]);
        atlas_buf_free(&item_observed_at[i]);
        atlas_buf_free(&item_path_text[i]);
    }
    atlas_buf_free(&source_uid);
    return st;
}

/* --- T9: the generation's cause, derived from the same three signals
 * `atlas_memory_plan_for` reads before any pass runs ------------------------- */

/* Every DECISION anchor this repository's memory claims have ever resolved,
 * folded into one digest over (document_id, its *current* effective approved
 * revision) -- so approving a revision moves this digest for every
 * repository whose memory claims cite that document, and nothing else does.
 * Distinct anchor tuples only (`atlas_db_memory_anchor_distinct`), so a
 * document cited by three claims folds in once. The scan is ordered
 * (`ORDER BY kind, value`) so an unrelated PATH or COMMIT anchor recorded
 * between two passes cannot reorder the DECISION tuples folded in here and
 * flip this digest without an actual approval having moved. */
typedef struct decision_digest_ctx {
    atlas_db *db;
    atlas_sha256 h;
} decision_digest_ctx;

static atlas_status decision_digest_tuple_cb(atlas_memory_anchor_kind kind, const char *value,
                                             void *ud, atlas_err *err) {
    if (kind != ATLAS_MEMORY_ANCHOR_DECISION) {
        return ATLAS_OK;
    }
    decision_digest_ctx *dc = ud;
    int64_t doc_id = 0, doc_repo = 0, rev_id = 0;
    bool found = false;
    atlas_status st = atlas_db_decision_find_uid(dc->db, value, &doc_id, &doc_repo, &found, err);
    if (st == ATLAS_OK && found) {
        st = atlas_db_decision_approved_revision(dc->db, doc_id, &rev_id, err);
    }
    if (st != ATLAS_OK) {
        return st;
    }
    char buf[64];
    int n = snprintf(buf, sizeof buf, "%lld:%lld;", (long long)doc_id, (long long)rev_id);
    atlas_sha256_update(&dc->h, buf, n > 0 ? (size_t)n : 0u);
    return ATLAS_OK;
}

static atlas_status compute_decision_set_digest(atlas_db *db, int64_t repo_id,
                                                char out[ATLAS_SHA256_HEX_LEN + 1], atlas_err *err) {
    decision_digest_ctx dc;
    dc.db = db;
    atlas_sha256_init(&dc.h);
    atlas_status st = atlas_db_memory_anchor_distinct(db, repo_id, decision_digest_tuple_cb, &dc, err);
    if (st != ATLAS_OK) {
        return st;
    }
    unsigned char digest[ATLAS_SHA256_DIGEST_LEN];
    atlas_sha256_final(&dc.h, digest);
    atlas_hex_encode_lower(digest, sizeof digest, out);
    return ATLAS_OK;
}

/* Every registered source's own identity plus its latest recorded content
 * hash, folded together in policy order -- moves whenever a source's latest
 * version changes, including an `EXTERNAL_*` one this pass never reads
 * itself (T11's `memory.put`, one layer over). Policy order rather than a
 * sort: the same policy read twice in one pass produces the same order, and
 * that is the only stability this digest needs. */
static atlas_status compute_source_set_digest(atlas_db *db, const atlas_repo_info *repo,
                                              const atlas_syspolicy *pol,
                                              char out[ATLAS_SHA256_HEX_LEN + 1], atlas_err *err) {
    atlas_sha256 h;
    atlas_sha256_init(&h);
    size_t total = atlas_syspolicy_memory_source_count_checked(pol);
    atlas_status st = ATLAS_OK;
    for (size_t i = 0; st == ATLAS_OK && i < total; i++) {
        const struct atlas_syspolicy_memory_source *ms = atlas_syspolicy_memory_source_at_checked(pol, i);
        if (ms == NULL) {
            continue;
        }
        if (ms->repo_name[0] != '\0' && strcmp(ms->repo_name, repo->name) != 0) {
            continue;
        }
        int64_t source_id = 0;
        bool found = false;
        st = atlas_db_memory_source_find(db, repo->id, ms->cls, ms->path, strlen(ms->path), &source_id,
                                         NULL, &found, err);
        if (st != ATLAS_OK) {
            break;
        }
        atlas_buf tag = ATLAS_BUF_INIT;
        if (!found) {
            st = atlas_buf_appendf(&tag, err, "%s:%s:none;", atlas_memory_source_class_name(ms->cls),
                                   ms->path);
        } else {
            atlas_memory_version_row row;
            atlas_memory_version_row_init(&row);
            bool vfound = false;
            st = atlas_db_memory_version_latest(db, source_id, &row, &vfound, err);
            if (st == ATLAS_OK) {
                st = atlas_buf_appendf(&tag, err, "%s:%s:%s;", atlas_memory_source_class_name(ms->cls),
                                       ms->path, vfound ? atlas_buf_cstr(&row.content_sha256) : "none");
            }
            atlas_memory_version_row_free(&row);
        }
        if (st == ATLAS_OK) {
            atlas_sha256_update(&h, tag.data, tag.len);
        }
        atlas_buf_free(&tag);
    }
    if (st != ATLAS_OK) {
        return st;
    }
    unsigned char digest[ATLAS_SHA256_DIGEST_LEN];
    atlas_sha256_final(&h, digest);
    atlas_hex_encode_lower(digest, sizeof digest, out);
    return ATLAS_OK;
}

/* Decision 7's own precedence, applied to what actually moved rather than
 * asserted: the first of SOURCE_REVISION, DECISION_REVISION, COMMIT whose own
 * signal differs from the last stored generation. `!have_last` -- this
 * repository has never completed a pass -- reads as SOURCE_REVISION rather
 * than as nothing owed, because every registered source is then unread,
 * which is indistinguishable from having just changed.
 *
 * T9 fix-round-1: the final fallthrough was documented as unreachable and is
 * not -- the vanished-anchor sweep's own drift finding (a decision-bound
 * claim whose SYMBOL anchor's semantic generation changed) can make
 * `ctx.any_change == true` while moving none of the three signals this
 * function compares: no source's own content changed, no decision's approved
 * revision changed, and HEAD did not move. Decision 7's vocabulary -- and
 * `memory_generations.cause`'s own CHECK constraint, migration 29 -- has
 * exactly three values and no fourth for "the semantic index changed under
 * an approved decision"; adding one needs a new migration, out of a fix
 * round's scope. `SOURCE_REVISION` is the closest available label by the
 * same reasoning `!have_last` already uses above -- a repository fact this
 * process cannot otherwise account for -- and this is now an asserted,
 * documented imprecision rather than a silent one: see
 * `test_drift_conflict_leaves_the_decision_untouched`, which reaches exactly
 * this path and checks it. */
static atlas_memory_gen_cause determine_cause(bool have_last, const char *last_head,
                                              const char *cur_head, const char *last_decision_digest,
                                              const char *cur_decision_digest,
                                              const char *last_source_digest,
                                              const char *cur_source_digest) {
    if (!have_last) {
        return ATLAS_MEMORY_CAUSE_SOURCE_REVISION;
    }
    if (strcmp(last_source_digest, cur_source_digest) != 0) {
        return ATLAS_MEMORY_CAUSE_SOURCE_REVISION;
    }
    if (strcmp(last_decision_digest, cur_decision_digest) != 0) {
        return ATLAS_MEMORY_CAUSE_DECISION_REVISION;
    }
    if (strcmp(last_head, cur_head) != 0) {
        return ATLAS_MEMORY_CAUSE_COMMIT;
    }
    return ATLAS_MEMORY_CAUSE_SOURCE_REVISION;
}

/* --- T9: the vanished-anchor sweep ------------------------------------------
 *
 * A candidate whose only anchor no longer resolves is routed to
 * `memory_unanchored` by the per-source loop above (`p->anchor_count == 0`
 * this pass), which means no CLAIM_CREATE runs for it at all and the fresh-
 * candidate correlation in `classify_candidate` never sees it. That is
 * `atlas_memory_anchor_resolve`'s own contract: an anchor is recorded only
 * when this pass's index confirms the thing it names still exists. The claim
 * a *prior* pass created while the referent still existed is untouched by
 * any of that -- it sits in `verify_claims`, live, still carrying the
 * `verifier`/`verifier_input` that was true when it was written -- and this
 * is the pass's only other way to notice a decision-scoped assertion whose
 * symbol was deleted, item 3's own scenario. */
struct vanish_ctx {
    apply_ctx *ctx;
};

static atlas_status vanish_tuple_cb(atlas_memory_anchor_kind kind, const char *value, void *ud,
                                    atlas_err *err) {
    struct vanish_ctx *vc = ud;
    apply_ctx *ctx = vc->ctx;

    char hash[ATLAS_SHA256_HEX_LEN + 1];
    atlas_status st = anchor_tuple_hash(kind, value, hash, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (ctx_is_confirmed(ctx, hash)) {
        return ATLAS_OK; /* this pass's own extraction still resolved it */
    }

    bool still_valid = true;
    switch (kind) {
    case ATLAS_MEMORY_ANCHOR_PATH: {
        atlas_buf ignored = ATLAS_BUF_INIT;
        bool found = false;
        st = atlas_db_verify_file_hash(ctx->db, ctx->repo->id, value, &ignored, &found, err);
        atlas_buf_free(&ignored);
        if (st != ATLAS_OK) {
            return st;
        }
        still_valid = found;
        break;
    }
    case ATLAS_MEMORY_ANCHOR_SYMBOL: {
        int64_t count = 0;
        st = atlas_db_verify_sem_symbol(ctx->db, ctx->repo->id, value, &count, NULL, NULL, err);
        if (st != ATLAS_OK) {
            return st;
        }
        still_valid = count > 0;
        break;
    }
    case ATLAS_MEMORY_ANCHOR_DECISION:
    case ATLAS_MEMORY_ANCHOR_COMMIT:
        still_valid = true; /* neither is ever deleted -- A4, and commits are immutable history */
        break;
    case ATLAS_MEMORY_ANCHOR_UNKNOWN:
        still_valid = true; /* never stored; unreachable, kept for -Wswitch-enum */
        break;
    }
    if (still_valid) {
        return ATLAS_OK;
    }

    struct find_prior_ctx fc;
    fc.db = ctx->db;
    fc.found = false;
    fc.uid[0] = '\0';
    fc.skip = NULL;
    fc.match_text = NULL;
    fc.match_text_len = 0;
    st = atlas_db_memory_anchor_claim_uids(ctx->db, ctx->repo->id, kind, value, find_prior_cb, &fc,
                                           err);
    if (st != ATLAS_OK || !fc.found) {
        return st;
    }

    atlas_verify_claim claim;
    atlas_verify_claim_init(&claim);
    bool found = false;
    st = atlas_db_verify_claim_find(ctx->db, fc.uid, &claim, &found, err);
    if (st != ATLAS_OK || !found) {
        atlas_verify_claim_free(&claim);
        return st;
    }

    /* T9 fix-round-1: read the last reported kind once, up front, rather than
     * only at the end -- both uses it now serves need the *same* value.
     * `claim` is immutable once written (A9.1's rule) and `still_valid` is
     * already established false for this exact anchor this pass, so once
     * this branch has produced one terminal verdict for `fc.uid` at all, a
     * repeated `evaluate_claim` on every later pass can only re-derive the
     * same verdict: nothing about the claim's own text, verifier or
     * verifier_input can change without a remint, and a remint would have
     * routed through `classify_candidate` instead of here. Bounded, and
     * measured: before this, `verify_results` grew by one row per pass, per
     * permanently-vanished referent, for ever -- a table `RETENTION[]`
     * cannot prune. */
    atlas_memory_diff_kind last_kind = ATLAS_MEMORY_DIFF_UNKNOWN;
    bool last_found = false;
    st = atlas_db_memory_claim_diff_last_kind(ctx->db, ctx->repo->id, fc.uid, &last_kind, &last_found,
                                              err);
    if (st != ATLAS_OK) {
        atlas_verify_claim_free(&claim);
        return st;
    }
    if (last_found && (last_kind == ATLAS_MEMORY_DIFF_CONTRADICTED ||
                       last_kind == ATLAS_MEMORY_DIFF_SUPPORTED ||
                       last_kind == ATLAS_MEMORY_DIFF_UNDETERMINED ||
                       last_kind == ATLAS_MEMORY_DIFF_STALE)) {
        /* Already reported once for this exact vanished condition; nothing
         * has changed that could change the verdict. */
        atlas_verify_claim_free(&claim);
        return ATLAS_OK;
    }

    atlas_memory_diff_kind new_kind;
    char reason[128];
    reason[0] = '\0';
    if (claim.verifier.len == 0) {
        new_kind = ATLAS_MEMORY_DIFF_STALE;
    } else {
        atlas_verify_check check = ATLAS_CHECK_UNAVAILABLE;
        atlas_verify_conflict conflict = ATLAS_CONFLICT_NONE;
        st = evaluate_claim(ctx, fc.uid, &check, &conflict, err);
        if (st != ATLAS_OK) {
            atlas_verify_claim_free(&claim);
            return st;
        }
        if (conflict == ATLAS_CONFLICT_IMPLEMENTATION) {
            atlas_buf duid = ATLAS_BUF_INIT;
            st = atlas_db_decision_uid_of(ctx->db, claim.document_id, &duid, err);
            if (st == ATLAS_OK) {
                (void)snprintf(reason, sizeof reason, "DRIFT %s", atlas_buf_cstr(&duid));
            }
            atlas_buf_free(&duid);
            new_kind = ATLAS_MEMORY_DIFF_CONTRADICTED;
        } else if (check == ATLAS_CHECK_FAIL || conflict == ATLAS_CONFLICT_CONTRADICTION) {
            new_kind = ATLAS_MEMORY_DIFF_CONTRADICTED;
        } else if (check == ATLAS_CHECK_PASS) {
            new_kind = ATLAS_MEMORY_DIFF_SUPPORTED;
        } else {
            new_kind = ATLAS_MEMORY_DIFF_UNDETERMINED;
        }
    }
    atlas_verify_claim_free(&claim);
    if (st != ATLAS_OK) {
        return st;
    }

    /* Absence is the evidence: once this exact claim uid has already been
     * reported at this kind, re-reporting it every subsequent pass would be
     * the "re-flagged forever" failure the diff surface exists to avoid. */
    if (last_found && last_kind == new_kind) {
        return ATLAS_OK;
    }
    return ctx_add_diff(ctx, fc.uid, new_kind, reason, err);
}

atlas_status atlas_memory_apply_in_tx(atlas_db *db, const atlas_repo_info *repo,
                                      const atlas_memory_observation *obs,
                                      const atlas_syspolicy *pol, const char *now,
                                      atlas_memory_pass_result *out, atlas_err *err) {
    if (db == NULL || repo == NULL || obs == NULL || pol == NULL || now == NULL || out == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "a memory apply needs a repository, a policy, an observation and a "
                             "place to write its result");
    }
    memset(out, 0, sizeof *out);

    size_t cap = ATLAS_MEMORY_MAX_SOURCES * ATLAS_MEMORY_MAX_PROPOSITIONS;
    size_t confirmed_cap = cap * ATLAS_MEMORY_MAX_ANCHORS_PER_PROPOSITION;
    apply_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.db = db;
    ctx.repo = repo;
    ctx.touched = &obs->touched;
    ctx.now = now;
    ctx.out = out;
    ctx.dep = calloc(cap, sizeof *ctx.dep);
    ctx.dep_cap = cap;
    ctx.diffs = calloc(cap, sizeof *ctx.diffs);
    ctx.diff_cap = cap;
    ctx.confirmed = calloc(confirmed_cap, sizeof *ctx.confirmed);
    ctx.confirmed_cap = confirmed_cap;
    if (ctx.dep == NULL || ctx.diffs == NULL || ctx.confirmed == NULL) {
        free(ctx.dep);
        free(ctx.diffs);
        free(ctx.confirmed);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory starting a reconciliation pass");
    }

    /* I4. One source's own obstacle must not discard the other fifteen
     * sources' work this pass: each gets its own SAVEPOINT, taken before
     * `apply_one_source` runs and released on success. On failure, this
     * source's *SQL* writes are undone by rolling back to the savepoint,
     * and this source's share of the pass-wide *write-side* bookkeeping is
     * undone right alongside it -- see `atlas_memory_pass_result`'s own
     * comment in `memory.h` for exactly which fields that is and why; this
     * loop is what enforces the split, not what defines it.
     *
     * Round 2, New-C1 / round 3 correction / round 4 correction: certain
     * SQLite errors (SQLITE_FULL, IOERR, NOMEM, BUSY, INTERRUPT) can end the
     * *outer* transaction, not merely the savepoint -- documented SQLite
     * behaviour, not a defect in the primitives above. The savepoint this
     * loop opened is released or rolled back by this same loop and nothing
     * else, so a failed `ROLLBACK TO`/`RELEASE` most likely means the
     * transaction it named is gone. Not *certainly*: `sqlite3_exec` can
     * fail to prepare `ROLLBACK TO` itself with SQLITE_NOMEM while the
     * transaction is still perfectly alive (`db.c`'s `atlas_db_exec_sql`),
     * so this is one likely explanation, not the only possible one. What
     * makes the inference safe without being certain is that the response
     * is fail-closed either way: `atlas_db_rollback` is called
     * unconditionally and the whole pass is abandoned, whether the
     * transaction was actually gone already or merely could not be trusted
     * to isolate anything further. Decided from the failure itself, not
     * from asking `atlas_db_in_transaction` first -- inside this function
     * `tx_depth` is always >= 1 (the caller opened the transaction before
     * calling in), so that question would answer `true` from its own
     * short-circuit without ever reaching `sqlite3_get_autocommit`, which
     * an earlier draft of this comment wrongly credited with an answer it
     * was never asked for. */
    atlas_status st = ATLAS_OK;
    for (size_t i = 0; st == ATLAS_OK && i < obs->source_count; i++) {
        size_t versions_added_snap = out->versions_added;
        size_t claims_created_snap = out->claims_created;
        size_t claims_resolved_snap = out->claims_resolved;
        size_t unanchored_snap = out->unanchored;
        size_t intake_bound_hits_snap = out->intake_bound_hits;
        size_t dep_snapshot = ctx.dep_count;
        size_t diff_snapshot = ctx.diff_count;
        size_t confirmed_snapshot = ctx.confirmed_count;
        bool any_change_snapshot = ctx.any_change;

        char sp_name[32];
        (void)snprintf(sp_name, sizeof sp_name, "memsrc_%zu", i);
        st = atlas_db_savepoint(db, sp_name, err);
        if (st != ATLAS_OK) {
            break;
        }

        atlas_status one_st = apply_one_source(&ctx, &obs->sources[i], err);
        bool source_failed = one_st != ATLAS_OK;

        if (!source_failed) {
            st = atlas_db_savepoint_release(db, sp_name, err);
            if (st == ATLAS_OK) {
                continue;
            }
            /* The release itself failed even though apply_one_source
             * succeeded -- fall through and treat this source as failed
             * rather than guessing whether its writes are actually part of
             * the transaction. */
            source_failed = true;
            st = ATLAS_OK;
        }

        /* The original failure's own message, preserved before anything
         * below overwrites `err` with the rollback attempt's own -- "the
         * pass failed" and "why" are two different things a caller reading
         * `last_obstacle` needs, and losing the first to the second was
         * round 2's own residual (`reconcile.c:977`, named in review). */
        /* Sized to the largest value that still proves safe to
         * -Wformat-truncation, not rounded down further than that: the
         * final `snprintf` into `last_obstacle` (256 bytes) prefixes this
         * with "source %zu: ", whose worst case is 7 + 20 (a 64-bit size_t's
         * longest decimal form) + 2 = 29 bytes, so 256 - 29 = 227 is the
         * largest size the compiler can verify never overflows; one byte of
         * margin below that avoids an off-by-one against its own count of
         * the trailing NUL. An earlier round capped this at 200 to silence
         * the same warning, discarding up to 26 bytes of a real failure
         * message for no reason tied to actual safety. */
        char original_msg[226];
        (void)snprintf(original_msg, sizeof original_msg, "%s", atlas_err_msg(err));

        atlas_err rollback_err;
        atlas_err_init(&rollback_err);
        atlas_status rb_st = atlas_db_savepoint_rollback(db, sp_name, &rollback_err);
        atlas_status rel_st = ATLAS_OK;
        if (rb_st == ATLAS_OK) {
            rel_st = atlas_db_savepoint_release(db, sp_name, &rollback_err);
        }
        if (rb_st != ATLAS_OK || rel_st != ATLAS_OK) {
            /* `ROLLBACK TO`/`RELEASE` named a savepoint this same loop
             * just opened and has not yet released -- the most likely
             * explanation for either failing is the transaction it lived in
             * ending from under it, though a resource failure in
             * `sqlite3_exec` itself (SQLITE_NOMEM at prepare) could produce
             * the same failure with the transaction still alive. Forced
             * closed unconditionally rather than asked about again either
             * way: the response is fail-closed regardless of which
             * explanation is true, and a rollback that itself fails is what
             * turns a bad source into a bad database if it is not. */
            atlas_db_rollback(db);
            st = atlas_err_set(err, ATLAS_ERR_DB,
                               "the write transaction ended while processing source %zu (%s); "
                               "the pass is abandoned rather than risk a database in a state "
                               "nothing checked -- original failure: %s",
                               i, atlas_err_msg(&rollback_err), original_msg);
            break;
        }

        out->versions_added = versions_added_snap;
        out->claims_created = claims_created_snap;
        out->claims_resolved = claims_resolved_snap;
        out->unanchored = unanchored_snap;
        out->intake_bound_hits = intake_bound_hits_snap;
        ctx.dep_count = dep_snapshot;
        ctx.diff_count = diff_snapshot;
        ctx.confirmed_count = confirmed_snapshot;
        ctx.any_change = any_change_snapshot;

        out->intake_bound_hits++;
        (void)snprintf(out->last_obstacle, sizeof out->last_obstacle, "source %zu: %s", i,
                      original_msg);
        atlas_err_init(err);
        /* st stays ATLAS_OK: recorded, not fatal to the rest of the pass. */
    }

    /* T9's vanished-anchor sweep: no savepoint of its own, deliberately --
     * every write it can make (an `EVIDENCE_PRODUCE`/`EVALUATE` pair, one
     * diff row) is through the same idempotent write points the per-source
     * loop above already uses, so a failure here abandons the whole pass
     * exactly as a failure anywhere else in this function does, through the
     * caller's own outer-transaction rollback. */
    if (st == ATLAS_OK) {
        ctx_sort_confirmed(&ctx);
        struct vanish_ctx vc;
        vc.ctx = &ctx;
        st = atlas_db_memory_anchor_distinct(db, repo->id, vanish_tuple_cb, &vc, err);
    }

    if (st == ATLAS_OK && ctx.any_change) {
        int64_t last_generation = 0;
        bool have_last = false;
        atlas_buf last_head = ATLAS_BUF_INIT;
        atlas_buf last_decision_digest = ATLAS_BUF_INIT;
        atlas_buf last_source_digest = ATLAS_BUF_INIT;
        st = atlas_db_memory_generation_latest(db, repo->id, &last_generation, &last_head,
                                               &last_decision_digest, &last_source_digest, &have_last,
                                               err);
        (void)last_generation;

        char cur_decision_digest[ATLAS_SHA256_HEX_LEN + 1];
        char cur_source_digest[ATLAS_SHA256_HEX_LEN + 1];
        cur_decision_digest[0] = '\0';
        cur_source_digest[0] = '\0';
        if (st == ATLAS_OK) {
            st = compute_decision_set_digest(db, repo->id, cur_decision_digest, err);
        }
        if (st == ATLAS_OK) {
            st = compute_source_set_digest(db, repo, pol, cur_source_digest, err);
        }
        atlas_memory_gen_cause cause = ATLAS_MEMORY_CAUSE_SOURCE_REVISION;
        if (st == ATLAS_OK) {
            cause = determine_cause(have_last, have_last ? atlas_buf_cstr(&last_head) : "",
                                    repo->scanned_head,
                                    have_last ? atlas_buf_cstr(&last_decision_digest) : "",
                                    cur_decision_digest,
                                    have_last ? atlas_buf_cstr(&last_source_digest) : "",
                                    cur_source_digest);
        }
        atlas_buf_free(&last_head);
        atlas_buf_free(&last_decision_digest);
        atlas_buf_free(&last_source_digest);

        int64_t generation = 0;
        if (st == ATLAS_OK) {
            st = atlas_db_memory_generation_next(db, repo->id, &generation, err);
        }
        atlas_buf identity = ATLAS_BUF_INIT;
        if (st == ATLAS_OK) {
            st = atlas_db_repo_identity_hash(db, repo->id, &identity, err);
        }
        int64_t generation_id = 0;
        if (st == ATLAS_OK) {
            st = atlas_db_memory_generation_insert(db, repo->id, generation, cause,
                                                   atlas_buf_cstr(&identity), repo->scanned_head,
                                                   cur_decision_digest, cur_source_digest, 0, now,
                                                   &generation_id, err);
        }
        atlas_buf_free(&identity);
        for (size_t i = 0; st == ATLAS_OK && i < ctx.diff_count; i++) {
            st = atlas_db_memory_claim_diff_add(db, generation_id, ctx.diffs[i].claim_uid,
                                                ctx.diffs[i].kind, ctx.diffs[i].reason, err);
        }
        if (st == ATLAS_OK) {
            out->generation = generation;
            out->diff_rows = ctx.diff_count;
        }
    }

    free(ctx.dep);
    free(ctx.diffs);
    free(ctx.confirmed);
    return st;
}

/* --- T9: does this repository owe a pass, and why --------------------------
 *
 * Asks the same three questions `atlas_memory_apply_in_tx` derives its own
 * cause from, over the same stored facts, and in the same order (Decision 7:
 * SOURCE_REVISION, then DECISION_REVISION, then COMMIT). Index reads only --
 * no git, no file, callable from the watcher tick or from `memory status`
 * with no transaction and no side effect. */
atlas_status atlas_memory_plan_for(atlas_db *db, const atlas_repo_info *repo,
                                   const atlas_syspolicy *pol, atlas_memory_gen_cause *cause_out,
                                   atlas_err *err) {
    if (cause_out != NULL) {
        *cause_out = ATLAS_MEMORY_CAUSE_UNKNOWN;
    }
    if (db == NULL || repo == NULL || pol == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no repository or policy to plan a pass for");
    }

    int64_t last_generation = 0;
    bool have_last = false;
    atlas_buf last_head = ATLAS_BUF_INIT;
    atlas_buf last_decision_digest = ATLAS_BUF_INIT;
    atlas_buf last_source_digest = ATLAS_BUF_INIT;
    atlas_status st = atlas_db_memory_generation_latest(db, repo->id, &last_generation, &last_head,
                                                        &last_decision_digest, &last_source_digest,
                                                        &have_last, err);
    (void)last_generation;
    if (st != ATLAS_OK) {
        goto done;
    }

    /* Decision-set signal, checked first per Decision 7's own ordering
     * inverted for cost: cheaper to compute once than to re-derive per
     * source below, and the order among the three does not depend on which
     * is computed first, only on which is *returned* first when several
     * hold -- so SOURCE_REVISION is still checked, and preferred, below. */
    char cur_decision_digest[ATLAS_SHA256_HEX_LEN + 1];
    st = compute_decision_set_digest(db, repo->id, cur_decision_digest, err);
    if (st != ATLAS_OK) {
        goto done;
    }
    bool decision_owed =
        have_last && strcmp(atlas_buf_cstr(&last_decision_digest), cur_decision_digest) != 0;

    /* Source-revision signal: for each registered `REPO_*` source, does the
     * index currently disagree with the latest version this pass ever
     * recorded for it. `EXTERNAL_*` is skipped -- T8 never reads one itself,
     * so the index carries nothing to compare it against; T11's `memory.put`
     * is its own writer and its own freshness question. */
    size_t total = atlas_syspolicy_memory_source_count_checked(pol);
    bool source_owed = false;
    for (size_t i = 0; st == ATLAS_OK && !source_owed && i < total; i++) {
        const struct atlas_syspolicy_memory_source *ms = atlas_syspolicy_memory_source_at_checked(pol, i);
        if (ms == NULL) {
            continue;
        }
        if (ms->repo_name[0] != '\0' && strcmp(ms->repo_name, repo->name) != 0) {
            continue;
        }
        if (!atlas_memory_source_class_is_repo(ms->cls)) {
            continue;
        }

        int64_t source_id = 0;
        bool found = false;
        st = atlas_db_memory_source_find(db, repo->id, ms->cls, ms->path, strlen(ms->path), &source_id,
                                         NULL, &found, err);
        if (st != ATLAS_OK) {
            break;
        }
        if (!found) {
            /* Never read at all: indistinguishable from having just changed. */
            source_owed = true;
            break;
        }

        atlas_buf path_text = ATLAS_BUF_INIT;
        st = atlas_path_text_encode(ms->path, strlen(ms->path), &path_text, err);
        if (st != ATLAS_OK) {
            atlas_buf_free(&path_text);
            break;
        }

        if (ms->cls == ATLAS_MEMORY_SOURCE_REPO_FILE) {
            atlas_buf hash = ATLAS_BUF_INIT;
            bool hfound = false;
            st = atlas_db_verify_file_hash(db, repo->id, atlas_buf_cstr(&path_text), &hash, &hfound,
                                           err);
            if (st == ATLAS_OK && hfound) {
                bool vfound = false;
                st = atlas_db_memory_version_exists(db, source_id, atlas_buf_cstr(&hash), &vfound,
                                                    NULL, NULL, NULL, err);
                if (st == ATLAS_OK && !vfound) {
                    source_owed = true;
                }
            }
            atlas_buf_free(&hash);
        } else {
            bool changed = false;
            st = atlas_db_memory_dir_hash_mismatch(db, repo->id, source_id, atlas_buf_cstr(&path_text),
                                                   &changed, err);
            if (st == ATLAS_OK && changed) {
                source_owed = true;
            }
        }
        atlas_buf_free(&path_text);
    }
    if (st != ATLAS_OK) {
        goto done;
    }

    if (source_owed) {
        if (cause_out != NULL) {
            *cause_out = ATLAS_MEMORY_CAUSE_SOURCE_REVISION;
        }
        goto done;
    }
    if (decision_owed) {
        if (cause_out != NULL) {
            *cause_out = ATLAS_MEMORY_CAUSE_DECISION_REVISION;
        }
        goto done;
    }
    if (have_last && strcmp(atlas_buf_cstr(&last_head), repo->scanned_head) != 0) {
        if (cause_out != NULL) {
            *cause_out = ATLAS_MEMORY_CAUSE_COMMIT;
        }
        goto done;
    }
    /* Nothing owed: `*cause_out` stays ATLAS_MEMORY_CAUSE_UNKNOWN, the zero,
     * set at entry. */

done:
    atlas_buf_free(&last_head);
    atlas_buf_free(&last_decision_digest);
    atlas_buf_free(&last_source_digest);
    return st;
}
