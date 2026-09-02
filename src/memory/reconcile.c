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

void atlas_memory_observation_init(atlas_memory_observation *o) {
    if (o == NULL) {
        return;
    }
    memset(o, 0, sizeof *o);
    for (size_t i = 0; i < ATLAS_MEMORY_MAX_SOURCES; i++) {
        observed_source_init(&o->sources[i]);
    }
}

void atlas_memory_observation_free(atlas_memory_observation *o) {
    if (o == NULL) {
        return;
    }
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
 * own argument needs only connectivity. `added_claims` is every claim uid
 * this pass created for the first time, buffered because whether a
 * generation gets appended at all is decided only once the whole pass has
 * run. Both are sized to the pass's own compiled ceiling
 * (ATLAS_MEMORY_MAX_SOURCES * ATLAS_MEMORY_MAX_PROPOSITIONS) and heap
 * allocated once. */
#define DEP_UID_MAX 96
typedef struct dep_entry {
    char norm_hex[ATLAS_SHA256_HEX_LEN + 1];
    char evidence_uid[DEP_UID_MAX];
} dep_entry;

typedef struct apply_ctx {
    atlas_db *db;
    const atlas_repo_info *repo;
    const char *now;
    atlas_memory_pass_result *out;
    dep_entry *dep;
    size_t dep_count;
    size_t dep_cap;
    char (*added_claims)[DEP_UID_MAX];
    size_t added_count;
    size_t added_cap;
    bool any_change;
} apply_ctx;

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

static atlas_status ctx_add_claim(apply_ctx *ctx, const char *claim_uid, atlas_err *err) {
    if (ctx->added_count >= ctx->added_cap) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "more claims created this pass than the compiled ceiling allows");
    }
    (void)snprintf(ctx->added_claims[ctx->added_count++], DEP_UID_MAX, "%s", claim_uid);
    return ATLAS_OK;
}

/* One candidate, already known to be anchored and citable, through the one
 * write point, in the op order the write point requires (CLAIM_CREATE first:
 * `op_evidence_add` resolves its claim before anything else). */
static atlas_status emit_candidate(apply_ctx *ctx, const atlas_memory_observed_source *src,
                                   const char *source_uid, const char *item_path_text,
                                   const char *memory_version_uid, const char *observed_at,
                                   const atlas_memory_proposition *p, atlas_err *err) {
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

    if (st == ATLAS_OK && claim_new) {
        st = ctx_add_claim(ctx, claim_uid, err);
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
                st = atlas_db_memory_unanchored_add(
                    ctx->db, src->external_latest.id, (int64_t)p.ordinal,
                    atlas_buf_cstr(&p.text_sha256), p.text.data, p.text.len, err);
                if (st == ATLAS_OK) {
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

    for (size_t i = 0; st == ATLAS_OK && i < src->item_count; i++) {
        const atlas_memory_observed_item *it = &src->items[i];
        if (it->outcome != ATLAS_MEMORY_READ_OK) {
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
            const atlas_memory_proposition *cp = &src->candidates[k];
            st = atlas_db_memory_unanchored_add(ctx->db, item_version_id[item_idx],
                                                (int64_t)cp->ordinal,
                                                atlas_buf_cstr(&cp->text_sha256), cp->text.data,
                                                cp->text.len, err);
            if (st == ATLAS_OK) {
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
            st = atlas_db_memory_unanchored_add(ctx->db, item_version_id[item_idx],
                                                (int64_t)p.ordinal, atlas_buf_cstr(&p.text_sha256),
                                                p.text.data, p.text.len, err);
            if (st == ATLAS_OK) {
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
                ctx->out->intake_bound_hits++;
                st = atlas_db_memory_unanchored_add(ctx->db, item_version_id[item_idx],
                                                    (int64_t)p.ordinal,
                                                    atlas_buf_cstr(&p.text_sha256), p.text.data,
                                                    p.text.len, err);
                if (st == ATLAS_OK) {
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

atlas_status atlas_memory_apply_in_tx(atlas_db *db, const atlas_repo_info *repo,
                                      const atlas_memory_observation *obs,
                                      const atlas_syspolicy *pol, const char *now,
                                      atlas_memory_pass_result *out, atlas_err *err) {
    (void)pol; /* T9 reads the decision set and the commit range through it; T8 does not yet */
    if (db == NULL || repo == NULL || obs == NULL || now == NULL || out == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "a memory apply needs a repository, an observation and a place to "
                             "write its result");
    }
    memset(out, 0, sizeof *out);

    size_t cap = ATLAS_MEMORY_MAX_SOURCES * ATLAS_MEMORY_MAX_PROPOSITIONS;
    apply_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.db = db;
    ctx.repo = repo;
    ctx.now = now;
    ctx.out = out;
    ctx.dep = calloc(cap, sizeof *ctx.dep);
    ctx.dep_cap = cap;
    ctx.added_claims = calloc(cap, sizeof *ctx.added_claims);
    ctx.added_cap = cap;
    if (ctx.dep == NULL || ctx.added_claims == NULL) {
        free(ctx.dep);
        free(ctx.added_claims);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory starting a reconciliation pass");
    }

    atlas_status st = ATLAS_OK;
    for (size_t i = 0; st == ATLAS_OK && i < obs->source_count; i++) {
        st = apply_one_source(&ctx, &obs->sources[i], err);
    }

    if (st == ATLAS_OK && ctx.any_change) {
        int64_t generation = 0;
        st = atlas_db_memory_generation_next(db, repo->id, &generation, err);
        atlas_buf identity = ATLAS_BUF_INIT;
        if (st == ATLAS_OK) {
            st = atlas_db_repo_identity_hash(db, repo->id, &identity, err);
        }
        int64_t generation_id = 0;
        if (st == ATLAS_OK) {
            st = atlas_db_memory_generation_insert(db, repo->id, generation,
                                                   ATLAS_MEMORY_CAUSE_SOURCE_REVISION,
                                                   atlas_buf_cstr(&identity), repo->scanned_head,
                                                   "", "", 0, now, &generation_id, err);
        }
        atlas_buf_free(&identity);
        for (size_t i = 0; st == ATLAS_OK && i < ctx.added_count; i++) {
            st = atlas_db_memory_claim_diff_add(db, generation_id, ctx.added_claims[i],
                                                ATLAS_MEMORY_DIFF_ADDED, "", err);
        }
        if (st == ATLAS_OK) {
            out->generation = generation;
            out->diff_rows = ctx.added_count;
        }
    }

    free(ctx.dep);
    free(ctx.added_claims);
    return st;
}
