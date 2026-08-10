/* Atlas - the deterministic freshness assessment.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * One approved revision, one exact repository state, one verdict.
 *
 * Everything in this file reads. It creates no process, opens no file, writes
 * no row, and takes no lock: an assessment is an observation, and an
 * observation that changed something would not be one. That is also what makes
 * the phase's central claim checkable — normal read-only indexing is never
 * blocked by the gate, because the gate has nothing to block it with.
 *
 * **The snapshot.** Every read here goes through the caller's handle inside the
 * caller's read transaction, so the decisions, the links, the commit graph, the
 * structural relations and the repository's own row are all read from one
 * SQLite snapshot. This is the difference between an assessment and four
 * queries that happened to run close together: without it, a pass that
 * committed between two reads would let a verdict combine a decision from
 * before it with a graph from after, and nothing in the answer would say so.
 *
 * **Two kinds of evidence, and they are not equally good.** A direct anchor
 * carries a content hash captured when the decision was written, so Atlas can
 * compare bytes and say precisely whether that file is what was approved.
 * Everything reached by traversal has no such snapshot, so the only thing Atlas
 * can ask about it is whether the path appears in the range of commits since
 * the validation point. The asymmetry is real and is reported rather than
 * smoothed over: a direct anchor that changed and changed back compares equal
 * and is FRESH, while a *dependency* that changed and changed back is still
 * IMPACTED, because path-level history is all there is to go on and IMPACTED is
 * a request to look rather than a finding.
 */
#include "gate/gate_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/code.h"
#include "atlas/decision.h"

/* --- the evidence digest ---------------------------------------------------
 *
 * Length-prefixed and domain-separated, for the reasons atlas/gate.h gives.
 * Every field is either Atlas-owned or a byte string whose length is written
 * before it, so no value can be mistaken for the start of the next one. */

static void put_u64(atlas_sha256 *h, uint64_t v) {
    unsigned char b[8];
    for (size_t i = 0; i < 8u; i++) {
        b[7u - i] = (unsigned char)(v & 0xffu);
        v >>= 8;
    }
    atlas_sha256_update(h, b, sizeof b);
}

static void put_bytes(atlas_sha256 *h, const void *p, size_t n) {
    put_u64(h, (uint64_t)n);
    if (n > 0) {
        atlas_sha256_update(h, p, n);
    }
}

static void put_str(atlas_sha256 *h, const char *s) {
    put_bytes(h, s == NULL ? "" : s, s == NULL ? 0u : strlen(s));
}

/* Canonical order over links: kind, then path bytes, then symbol bytes. The
 * same order `atlas_decision_content_hash` imposes, and for the same reason —
 * a set that was reordered is the same set. */
static int link_order(const void *a, const void *b) {
    const atlas_decision_link *const *pa = a;
    const atlas_decision_link *const *pb = b;
    const atlas_decision_link *x = *pa;
    const atlas_decision_link *y = *pb;
    if (x->kind != y->kind) {
        return x->kind < y->kind ? -1 : 1;
    }
    size_t n = x->path_raw.len < y->path_raw.len ? x->path_raw.len : y->path_raw.len;
    int c = n == 0 ? 0 : memcmp(x->path_raw.data, y->path_raw.data, n);
    if (c != 0) {
        return c;
    }
    if (x->path_raw.len != y->path_raw.len) {
        return x->path_raw.len < y->path_raw.len ? -1 : 1;
    }
    n = x->symbol_name.len < y->symbol_name.len ? x->symbol_name.len : y->symbol_name.len;
    c = n == 0 ? 0 : memcmp(x->symbol_name.data, y->symbol_name.data, n);
    if (c != 0) {
        return c;
    }
    if (x->symbol_name.len != y->symbol_name.len) {
        return x->symbol_name.len < y->symbol_name.len ? -1 : 1;
    }
    return 0;
}

atlas_status atlas_gate_evidence_digest(const atlas_decision_revision *rev, char *hex_out,
                                        atlas_err *err) {
    const atlas_decision_link *sorted[ATLAS_DECISION_MAX_LINKS];
    size_t n = rev->link_count;
    if (n > ATLAS_DECISION_MAX_LINKS) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "a revision carries too many links");
    }
    for (size_t i = 0; i < n; i++) {
        sorted[i] = &rev->links[i];
    }
    qsort(sorted, n, sizeof sorted[0], link_order);

    atlas_sha256 h;
    atlas_sha256_init(&h);
    put_str(&h, ATLAS_GATE_EVIDENCE_DOMAIN);
    put_u64(&h, (uint64_t)n);
    for (size_t i = 0; i < n; i++) {
        const atlas_decision_link *l = sorted[i];
        put_str(&h, atlas_decision_link_kind_name(l->kind));
        /* The selector, exactly as stored. */
        put_bytes(&h, l->path_raw.data, l->path_raw.len);
        put_bytes(&h, l->symbol_name.data, l->symbol_name.len);
        put_bytes(&h, l->symbol_kind.data, l->symbol_kind.len);
        put_bytes(&h, l->commit_oid.data, l->commit_oid.len);
        put_bytes(&h, l->target_uid.data, l->target_uid.len);
        /* And what it resolves to right now. `currency` and `match_count` are
         * filled in by the resolver before this runs; a caller that digests an
         * unresolved revision gets a digest of "unresolved", which is a
         * different value from a digest of "resolves to nothing" and is meant
         * to be. */
        put_str(&h, atlas_decision_link_currency_name(l->currency));
        put_u64(&h, (uint64_t)l->match_count);
        put_u64(&h, (uint64_t)l->symbol_line);
    }
    unsigned char digest[ATLAS_SHA256_DIGEST_LEN];
    atlas_sha256_final(&h, digest);
    atlas_hex_encode(digest, sizeof digest, hex_out);
    (void)err;
    return ATLAS_OK;
}

atlas_status atlas_gate_evidence_digest_for(atlas_db *db, int64_t repo_id,
                                            atlas_decision_revision *rev, char *hex_out,
                                            atlas_err *err) {
    for (size_t i = 0; i < rev->link_count; i++) {
        atlas_status st = atlas_db_decision_link_resolve(db, repo_id, &rev->links[i], true, true, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    return atlas_gate_evidence_digest(rev, hex_out, err);
}

/* --- the structural walk ---------------------------------------------------
 *
 * Outbound, which is the direction that answers the question actually being
 * asked. A decision recorded about a file is a statement whose basis includes
 * what that file depends on; if a dependency moved, the basis may have moved
 * with it. The other direction — what depends on the file — describes code
 * whose own basis may have changed, and those are that code's decisions to
 * worry about, not this one's.
 *
 * The walk is bounded twice over: by A3's own ceilings, and by A6's shallower
 * default, because this runs once per assessed decision rather than once per
 * question a person asked. Truncation is never absorbed — a walk that stopped
 * early cannot say it found nothing. */

typedef struct walk_state {
    const atlas_db_gate_paths *changed;
    int64_t matched;
    atlas_buf tmp;
    atlas_status st;
} walk_state;

static atlas_status on_walk_node(const atlas_code_walk_row *row, void *ud, atlas_err *err) {
    walk_state *w = ud;
    if (row->node_kind == NULL || strcmp(row->node_kind, "file") != 0 || row->label == NULL) {
        return ATLAS_OK;
    }
    /* The walk hands back the safe text encoding; the change set holds raw
     * bytes. Decoding here rather than encoding there keeps the set in the one
     * form every membership test in Atlas uses. */
    w->tmp.len = 0;
    atlas_status st = atlas_text_decode_safe(row->label, strlen(row->label), &w->tmp, err);
    if (st != ATLAS_OK) {
        /* A label Atlas cannot decode is a label it will not test. It cannot be
         * counted as "not changed". */
        w->st = st;
        return ATLAS_OK;
    }
    if (atlas_db_gate_paths_has(w->changed, w->tmp.data, w->tmp.len)) {
        w->matched++;
    }
    return ATLAS_OK;
}

/* --- one assessment -------------------------------------------------------- */

/* Resolves every link, counts what it found, and folds the result into the
 * verdict.
 *
 * **Which baseline the direct-evidence question is asked against depends on
 * whether the decision has been revalidated**, and getting this wrong makes
 * revalidation useless.
 *
 * Without a revalidation, the baseline is each link's own snapshot — the
 * content hash captured when the revision was written and covered by the
 * approval. That is what "still what was approved" means.
 *
 * With one, it is the evidence digest the revalidation recorded. A revision is
 * immutable, so its link snapshots can never be updated; if they stayed the
 * baseline, a decision an operator had just checked against the current code
 * would report STALE for ever, and the only remedy would be to write a new
 * revision — which is a different act with a different meaning. The validation
 * record exists precisely to carry the newer baseline, and this is what it is
 * for.
 *
 * One reason is derived the same way under both baselines: an anchor Atlas
 * cannot resolve at all. FRESH claims that the required evidence still
 * resolves, and "Atlas could not look" never establishes that — not even when
 * it could not look last time either. */
static atlas_status assess_links(atlas_db *db, const atlas_gate_env *env,
                                 atlas_decision_revision *rev, atlas_gate_assessment *a,
                                 bool revalidated, bool *has_code_anchor, atlas_err *err) {
    *has_code_anchor = false;
    for (size_t i = 0; i < rev->link_count; i++) {
        atlas_decision_link *l = &rev->links[i];
        atlas_status st = atlas_db_decision_link_resolve(db, env->repo_id, l, env->file_index_known,
                                                       env->code_index_known, err);
        if (st != ATLAS_OK) {
            return st;
        }
        a->links_total++;

        bool is_code = l->kind == ATLAS_DECISION_LINK_PATH ||
                       l->kind == ATLAS_DECISION_LINK_SYMBOL ||
                       l->kind == ATLAS_DECISION_LINK_COMMIT;
        if (is_code) {
            *has_code_anchor = true;
        }

        switch (l->currency) {
            case ATLAS_DECISION_LINK_CURRENT:
                a->links_current++;
                break;
            case ATLAS_DECISION_LINK_CHANGED:
                a->links_changed++;
                if (is_code && !revalidated) {
                    atlas_gate_assessment_note(a, ATLAS_GATE_REASON_DIRECT_EVIDENCE_CHANGED);
                }
                break;
            case ATLAS_DECISION_LINK_MISSING:
                a->links_missing++;
                if (revalidated) {
                    break;
                }
                /* A missing anchor is a different fact for each kind, and the
                 * reason codes keep them apart because the repair is
                 * different: a deleted file, a renamed symbol and a document
                 * that is no longer there are three separate conversations. */
                if (l->kind == ATLAS_DECISION_LINK_PATH) {
                    atlas_gate_assessment_note(a, ATLAS_GATE_REASON_LINKED_PATH_MISSING);
                } else if (l->kind == ATLAS_DECISION_LINK_SYMBOL) {
                    atlas_gate_assessment_note(a, ATLAS_GATE_REASON_LINKED_SYMBOL_MISSING);
                } else if (l->kind == ATLAS_DECISION_LINK_COMMIT) {
                    atlas_gate_assessment_note(a, ATLAS_GATE_REASON_LINKED_COMMIT_MISSING);
                } else {
                    /* A link to another decision document. Not code evidence,
                     * so it does not make the code stale; it does mean Atlas
                     * cannot see the whole picture, which is UNKNOWN. */
                    atlas_gate_assessment_note(a, ATLAS_GATE_REASON_EVIDENCE_UNRESOLVED);
                }
                break;
            case ATLAS_DECISION_LINK_AMBIGUOUS:
                a->links_ambiguous++;
                if (revalidated) {
                    break;
                }
                /* Ambiguity is staleness, not impact. The snapshot named one
                 * thing and the name now names several; Atlas will not choose,
                 * and until somebody does, nothing here resolves to what was
                 * approved. That is exactly the state a human has to settle. */
                atlas_gate_assessment_note(a, ATLAS_GATE_REASON_LINKED_SYMBOL_AMBIGUOUS);
                break;
            case ATLAS_DECISION_LINK_UNKNOWN:
            default:
                a->links_unknown++;
                atlas_gate_assessment_note(a, ATLAS_GATE_REASON_EVIDENCE_UNRESOLVED);
                break;
        }
    }
    return ATLAS_OK;
}

/* Walks outbound from each of the revision's file anchors looking for a path in
 * the change set. */
static atlas_status assess_impact(atlas_db *db, const atlas_gate_env *env,
                                  const atlas_decision_revision *rev,
                                  const atlas_db_gate_paths *changed, int64_t depth,
                                  atlas_gate_assessment *a, atlas_err *err) {
    walk_state w;
    memset(&w, 0, sizeof w);
    atlas_buf_init(&w.tmp);
    w.changed = changed;
    w.st = ATLAS_OK;

    atlas_status st = ATLAS_OK;
    for (size_t i = 0; i < rev->link_count && st == ATLAS_OK; i++) {
        const atlas_decision_link *l = &rev->links[i];
        if (l->kind != ATLAS_DECISION_LINK_PATH && l->kind != ATLAS_DECISION_LINK_SYMBOL) {
            continue;
        }
        if (l->path_raw.len == 0) {
            continue;
        }
        bool found = false;
        int64_t code_file_id = 0;
        st = atlas_db_code_file_get(db, env->repo_id, l->path_raw.data, l->path_raw.len, NULL,
                                    NULL, &found, &code_file_id, err);
        if (st != ATLAS_OK) {
            break;
        }
        if (!found || code_file_id == 0) {
            /* The anchor is a path the structural index does not hold — a file
             * of a language A3 does not parse, or one that has gone. Not an
             * error, and not a reason on its own: the direct evidence check has
             * already had its say about whether the file is still what it was. */
            continue;
        }

        atlas_code_walk_opts opts;
        atlas_code_walk_opts_init(&opts);
        opts.start_kind = ATLAS_CODE_NODE_FILE;
        opts.start_id = code_file_id;
        opts.inbound = false; /* dependencies, not dependents. See the note above. */
        opts.depth = depth;
        opts.max_nodes = ATLAS_GATE_MAX_IMPACT_NODES;
        opts.follow_files = true;
        opts.follow_symbols = true;

        atlas_code_walk_summary sum;
        memset(&sum, 0, sizeof sum);
        st = atlas_code_walk(db, env->repo_id, &opts, on_walk_node, &w, &sum, err);
        if (st != ATLAS_OK) {
            break;
        }
        a->walk_visited += sum.visited;
        if (sum.truncated) {
            /* A truncated walk found a subset. It cannot report that it found
             * nothing, so it reports that it could not finish. */
            a->limit_reached = true;
            a->limit_detail = "structural traversal";
            atlas_gate_assessment_note(a, ATLAS_GATE_REASON_TRAVERSAL_LIMIT);
        }
        if (w.st != ATLAS_OK) {
            st = w.st;
            break;
        }
    }

    a->walk_matched = w.matched;
    if (st == ATLAS_OK && w.matched > 0) {
        atlas_gate_assessment_note(a, ATLAS_GATE_REASON_DEPENDENCY_CHANGED);
    }
    atlas_buf_free(&w.tmp);
    return st;
}

atlas_status atlas_gate_assess(atlas_db *db, const atlas_gate_env *env, int64_t document_id,
                               const atlas_decision_doc_row *doc, int64_t depth,
                               atlas_gate_assessment *a, atlas_err *err) {
    atlas_status st = ATLAS_OK;

    a->repo_id = env->repo_id;
    a->document_id = document_id;
    st = atlas_buf_set_str(&a->uid, doc->uid, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&a->title, doc->title == NULL ? "" : doc->title, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&a->repo_name, atlas_buf_cstr(&env->repo_name), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&a->root_text, atlas_buf_cstr(&env->root_text), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&a->repo_identity_hash, atlas_buf_cstr(&env->repo_identity_hash),
                               err);
    }
    if (st != ATLAS_OK) {
        return st;
    }
    (void)snprintf(a->indexed_commit, sizeof a->indexed_commit, "%s", env->indexed_commit);
    (void)snprintf(a->requested_commit, sizeof a->requested_commit, "%s", env->requested_commit);
    (void)atlas_decision_state_parse(doc->status, &a->state);

    /* Whatever the repository-level environment already knows is wrong applies
     * to every decision in it. Noted per assessment rather than only on the
     * report, so one assessment read on its own still says why it is UNKNOWN. */
    for (size_t i = 0; i < env->reason_count; i++) {
        atlas_gate_assessment_note(a, env->reasons[i]);
    }

    int64_t revision_id = doc->current_revision_id != 0 ? doc->current_revision_id
                                                        : doc->head_revision_id;
    atlas_decision_revision rev;
    atlas_decision_revision_init(&rev);
    bool found = false;
    st = atlas_db_decision_revision_load(db, revision_id, &rev, &found, err);
    if (st != ATLAS_OK) {
        atlas_decision_revision_free(&rev);
        return st;
    }
    if (!found) {
        atlas_gate_assessment_note(a, ATLAS_GATE_REASON_CONTENT_HASH_MISMATCH);
        atlas_decision_revision_free(&rev);
        return ATLAS_OK;
    }
    a->revision_id = rev.id;
    a->revision_no = rev.revision_no;
    a->scope = rev.scope;
    (void)snprintf(a->content_hash, sizeof a->content_hash, "%s", rev.content_hash);

    /* The revision must still hash to what it says it does. Atlas never updates
     * a content column, so a mismatch means something outside Atlas did — and
     * every approval bound to that digest now covers bytes that are not there.
     * That is not a stale decision; it is a record Atlas cannot reason about. */
    char recomputed[ATLAS_SHA256_HEX_LEN + 1u];
    st = atlas_decision_content_hash(&rev, recomputed, err);
    if (st != ATLAS_OK) {
        atlas_decision_revision_free(&rev);
        return st;
    }
    if (strcmp(recomputed, rev.content_hash) != 0) {
        atlas_gate_assessment_note(a, ATLAS_GATE_REASON_CONTENT_HASH_MISMATCH);
    }

    /* The decision's durable identity must be the repository being assessed.
     * An empty one is legal — a revision written before any history had been
     * ingested has none, permanently — and is not an ambiguity. A non-empty one
     * that names something else is. */
    if (rev.basis_repo_identity.len > 0 && env->repo_identity_hash.len > 0 &&
        strcmp(atlas_buf_cstr(&rev.basis_repo_identity),
               atlas_buf_cstr(&env->repo_identity_hash)) != 0) {
        atlas_gate_assessment_note(a, ATLAS_GATE_REASON_REPOSITORY_AMBIGUOUS);
    }

    /* --- the validation point ---------------------------------------------
     *
     * The newest revalidation of this revision *in this repository identity*,
     * or, when there is none, the basis the revision was proposed and approved
     * at. The fallback is the conservative one: an approval happened after the
     * proposal, so measuring from the proposal's basis can only ever widen the
     * change range, and a wider range produces more review rather than less. */
    atlas_db_gate_validation v;
    bool have_validation = false;
    st = atlas_db_gate_validation_newest(db, rev.id, atlas_buf_cstr(&env->repo_identity_hash), &v,
                                         &have_validation, err);
    if (st == ATLAS_OK) {
        st = atlas_db_gate_validation_count(db, rev.id, atlas_buf_cstr(&env->repo_identity_hash),
                                            &a->revalidation_count, err);
    }
    if (st != ATLAS_OK) {
        atlas_decision_revision_free(&rev);
        return st;
    }
    if (have_validation) {
        (void)snprintf(a->validated_at_commit, sizeof a->validated_at_commit, "%s",
                       v.validated_at_commit);
        a->validated_by_revalidation = true;
    } else if (rev.basis_head.len > 0) {
        (void)snprintf(a->validated_at_commit, sizeof a->validated_at_commit, "%s",
                       atlas_buf_cstr(&rev.basis_head));
    }

    /* --- direct evidence --------------------------------------------------- */
    bool has_code_anchor = false;
    st = assess_links(db, env, &rev, a, have_validation, &has_code_anchor, err);
    if (st == ATLAS_OK) {
        st = atlas_gate_evidence_digest(&rev, a->evidence_digest, err);
    }
    if (st != ATLAS_OK) {
        atlas_decision_revision_free(&rev);
        return st;
    }
    if (have_validation && strcmp(a->evidence_digest, v.evidence_digest) != 0) {
        /* The anchors resolve differently from how they resolved when a human
         * last checked them. That is the direct-evidence question, asked
         * against the newer baseline. */
        atlas_gate_assessment_note(a, ATLAS_GATE_REASON_DIRECT_EVIDENCE_CHANGED);
    }

    /* A decision with no anchor into the code, and no claim to the whole
     * repository, gives Atlas nothing to measure. That is not FRESH. */
    if (!has_code_anchor && rev.scope != ATLAS_DECISION_SCOPE_REPOSITORY) {
        atlas_gate_assessment_note(a, ATLAS_GATE_REASON_SCOPE_NOT_ASSESSABLE);
    }

    /* --- the change range -------------------------------------------------- */
    bool range_known = false;
    atlas_db_gate_range range;
    memset(&range, 0, sizeof range);

    if (a->validated_at_commit[0] == '\0') {
        atlas_gate_assessment_note(a, ATLAS_GATE_REASON_MISSING_VALIDATION_POINT);
    } else if (env->indexed_commit[0] == '\0') {
        atlas_gate_assessment_note(a, ATLAS_GATE_REASON_INDEX_LAG);
    } else {
        atlas_db_gate_ancestry_result anc;
        st = atlas_db_gate_ancestry(db, env->repo_id, env->indexed_commit, a->validated_at_commit,
                                    &anc, err);
        if (st != ATLAS_OK) {
            atlas_decision_revision_free(&rev);
            return st;
        }
        switch (anc.verdict) {
            case ATLAS_DB_GATE_ANCESTRY_REACHED:
                st = atlas_db_gate_range_paths(db, env->repo_id, env->indexed_commit,
                                               a->validated_at_commit, &range, err);
                if (st != ATLAS_OK) {
                    atlas_db_gate_range_free(&range);
                    atlas_decision_revision_free(&rev);
                    return st;
                }
                a->range_commits = range.commits;
                a->range_paths = (int64_t)range.paths.count;
                if (range.limit_reached) {
                    a->limit_reached = true;
                    a->limit_detail = "change range";
                    atlas_gate_assessment_note(a, ATLAS_GATE_REASON_TRAVERSAL_LIMIT);
                } else if (range.missing_commit) {
                    atlas_gate_assessment_note(a, ATLAS_GATE_REASON_UNREACHABLE_BASE);
                } else {
                    range_known = true;
                }
                break;
            case ATLAS_DB_GATE_ANCESTRY_NOT_ANCESTOR:
                /* Every reachable commit was expanded and the validation point
                 * was not among them. The history the decision was validated
                 * against is not the history that is there now. */
                atlas_gate_assessment_note(a, ATLAS_GATE_REASON_HISTORY_REWRITTEN);
                break;
            case ATLAS_DB_GATE_ANCESTRY_LIMIT:
                a->limit_reached = true;
                a->limit_detail = "ancestry walk";
                atlas_gate_assessment_note(a, ATLAS_GATE_REASON_TRAVERSAL_LIMIT);
                break;
            case ATLAS_DB_GATE_ANCESTRY_UNKNOWN:
            default:
                atlas_gate_assessment_note(a, ATLAS_GATE_REASON_UNREACHABLE_BASE);
                break;
        }
    }

    /* --- transitive impact -------------------------------------------------
     *
     * Only when the range is known. A membership test against a set that
     * stopped being collected answers "no" for two different reasons, and one
     * of them is "Atlas gave up" — which must never read as "nothing
     * changed". */
    if (st == ATLAS_OK && range_known) {
        if (range.paths.count == 0) {
            /* Nothing at all happened since the validation point. */
            (void)0;
        } else {
            st = assess_impact(db, env, &rev, &range.paths, depth, a, err);
            /* A decision that claims the whole repository is impacted by any
             * change in the range, because it has no narrower anchor to test
             * and claiming otherwise would be claiming a bound it never set. */
            if (st == ATLAS_OK && rev.scope == ATLAS_DECISION_SCOPE_REPOSITORY) {
                atlas_gate_assessment_note(a, ATLAS_GATE_REASON_DEPENDENCY_CHANGED);
            }
        }
    }

    /* Nothing found anything to say. That is the one path to FRESH, and it is
     * reachable only after every check above declined to weaken it. */
    if (st == ATLAS_OK && a->reason_count == 0) {
        atlas_gate_assessment_note(a, ATLAS_GATE_REASON_NO_RELEVANT_CHANGE);
    }

    atlas_db_gate_range_free(&range);
    atlas_decision_revision_free(&rev);
    return st;
}
