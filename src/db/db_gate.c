/* Atlas - the typed storage operations A6 needs.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Three groups, and each of them exists because the alternative was a scan.
 *
 *   - **Ancestry.** Whether a decision's validation point is reachable from the
 *     indexed head, answered by walking `commits.parents` from the head. The
 *     walk is bounded, and reaching the bound is a different answer from
 *     reaching the end: "Atlas stopped looking" and "it is not there" are not
 *     the same fact, and only the second one is safe to act on.
 *
 *   - **The change range.** Which repository paths the commits between the
 *     validation point and the head touched, from `file_changes`. Bounded for
 *     the same reason and with a stronger consequence: a change set Atlas could
 *     not finish enumerating must not be tested for membership at all, because
 *     every miss would be indistinguishable from a path that was never in it.
 *
 *   - **The revalidation ledger.** Append-only, and this file contains no
 *     UPDATE and no DELETE that touches it. There is deliberately no
 *     `_clear`, no `_prune` and no `_forget`.
 *
 * Everything here reads through the caller's handle and therefore through the
 * caller's transaction, which is what lets an assessment be one coherent
 * snapshot rather than four reads that happened to be near each other.
 */
#include "db/db_internal.h"

#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/gate.h"

static atlas_status bind_i64(atlas_db *db, sqlite3_stmt *s, int idx, int64_t v, atlas_err *err) {
    if (sqlite3_bind_int64(s, idx, v) != SQLITE_OK) {
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind an integer");
    }
    return ATLAS_OK;
}

/* --- ancestry --------------------------------------------------------------
 *
 * `commits.parents` is a space-separated list of hex object ids, which is the
 * form the ingest wrote and the form this reads. A merge contributes both
 * parents, so this is a walk over a DAG rather than a chain.
 *
 * The visited set is a flat sorted array of object ids rather than a hash
 * table: it is bounded by ATLAS_GATE_MAX_ANCESTRY_COMMITS, a binary search over
 * it costs a handful of comparisons, and a bounded array has no failure mode a
 * reviewer has to reason about. */

typedef struct oid_set {
    char (*items)[ATLAS_OID_HEX_MAX_INCL];
    size_t count;
    size_t cap;
} oid_set;

static void oid_set_free(oid_set *s) {
    free(s->items);
    s->items = NULL;
    s->count = 0;
    s->cap = 0;
}

/* Index at which `oid` is, or would be inserted. */
static size_t oid_set_lower(const oid_set *s, const char *oid) {
    size_t lo = 0;
    size_t hi = s->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        if (strcmp(s->items[mid], oid) < 0) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static bool oid_set_has(const oid_set *s, const char *oid) {
    size_t at = oid_set_lower(s, oid);
    return at < s->count && strcmp(s->items[at], oid) == 0;
}

/* Adds `oid`, reporting whether it was new. */
static atlas_status oid_set_add(oid_set *s, const char *oid, bool *added_out, atlas_err *err) {
    *added_out = false;
    size_t n = strlen(oid);
    if (n == 0 || n >= ATLAS_OID_HEX_MAX_INCL) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "a stored commit id has an unusable length");
    }
    size_t at = oid_set_lower(s, oid);
    if (at < s->count && strcmp(s->items[at], oid) == 0) {
        return ATLAS_OK;
    }
    if (s->count == s->cap) {
        size_t cap = s->cap == 0 ? 64u : s->cap * 2u;
        void *grown = realloc(s->items, cap * sizeof(s->items[0]));
        if (grown == NULL) {
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
        }
        s->items = grown;
        s->cap = cap;
    }
    memmove(&s->items[at + 1u], &s->items[at], (s->count - at) * sizeof(s->items[0]));
    memcpy(s->items[at], oid, n + 1u);
    s->count++;
    *added_out = true;
    return ATLAS_OK;
}

/* A simple FIFO of ids still to expand. */
typedef struct oid_queue {
    char (*items)[ATLAS_OID_HEX_MAX_INCL];
    size_t head;
    size_t count;
    size_t cap;
} oid_queue;

static void oid_queue_free(oid_queue *q) {
    free(q->items);
    memset(q, 0, sizeof(*q));
}

static atlas_status oid_queue_push(oid_queue *q, const char *oid, atlas_err *err) {
    if (q->head + q->count == q->cap) {
        if (q->head > 0) {
            memmove(&q->items[0], &q->items[q->head], q->count * sizeof(q->items[0]));
            q->head = 0;
        } else {
            size_t cap = q->cap == 0 ? 64u : q->cap * 2u;
            void *grown = realloc(q->items, cap * sizeof(q->items[0]));
            if (grown == NULL) {
                return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
            }
            q->items = grown;
            q->cap = cap;
        }
    }
    size_t n = strlen(oid);
    if (n >= ATLAS_OID_HEX_MAX_INCL) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "a stored commit id has an unusable length");
    }
    memcpy(q->items[q->head + q->count], oid, n + 1u);
    q->count++;
    return ATLAS_OK;
}

static const char *oid_queue_pop(oid_queue *q) {
    if (q->count == 0) {
        return NULL;
    }
    const char *v = q->items[q->head];
    q->head++;
    q->count--;
    return v;
}

atlas_status atlas_db_gate_ancestry(atlas_db *db, int64_t repo_id, const char *head_oid,
                                    const char *target_oid, atlas_db_gate_ancestry_result *out,
                                    atlas_err *err) {
    memset(out, 0, sizeof(*out));
    out->verdict = ATLAS_DB_GATE_ANCESTRY_UNKNOWN;
    if (head_oid == NULL || head_oid[0] == '\0' || target_oid == NULL || target_oid[0] == '\0') {
        return ATLAS_OK;
    }
    if (strcmp(head_oid, target_oid) == 0) {
        out->verdict = ATLAS_DB_GATE_ANCESTRY_REACHED;
        return ATLAS_OK;
    }

    sqlite3_stmt *s = NULL;
    atlas_status st =
        atlas_db_prepare(db, "SELECT parents FROM commits WHERE repo_id = ?1 AND oid = ?2;", &s,
                         err);
    if (st != ATLAS_OK) {
        return st;
    }

    oid_set seen = {0};
    oid_queue todo = {0};
    bool added = false;
    st = oid_set_add(&seen, head_oid, &added, err);
    if (st == ATLAS_OK) {
        st = oid_queue_push(&todo, head_oid, err);
    }

    /* The head itself must be a commit Atlas has ingested. If it is not, the
     * walk has no ground to stand on and nothing about the answer would be
     * reliable — which is UNREACHABLE_BASE rather than "not an ancestor". */
    bool head_known = false;

    while (st == ATLAS_OK && todo.count > 0) {
        if (out->visited >= ATLAS_GATE_MAX_ANCESTRY_COMMITS) {
            out->verdict = ATLAS_DB_GATE_ANCESTRY_LIMIT;
            break;
        }
        const char *cur = oid_queue_pop(&todo);
        char cur_copy[ATLAS_OID_HEX_MAX_INCL];
        (void)snprintf(cur_copy, sizeof cur_copy, "%s", cur);
        out->visited++;

        (void)sqlite3_reset(s);
        (void)sqlite3_clear_bindings(s);
        st = bind_i64(db, s, 1, repo_id, err);
        if (st == ATLAS_OK) {
            st = atlas_db_bind_text_opt(db, s, 2, cur_copy, err);
        }
        if (st != ATLAS_OK) {
            break;
        }
        int rc = sqlite3_step(s);
        if (rc == SQLITE_DONE) {
            /* A commit named as a parent that Atlas has never ingested. The
             * history it holds does not reach back far enough to answer, so it
             * must not answer. */
            out->missing_parent = true;
            continue;
        }
        if (rc != SQLITE_ROW) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read a commit's parents");
            break;
        }
        if (strcmp(cur_copy, head_oid) == 0) {
            head_known = true;
        }
        const char *parents = atlas_db_col_text_opt(s, 0);
        if (parents == NULL) {
            continue;
        }
        /* Split on spaces. The column is Atlas-written hex, but it is parsed
         * defensively anyway: this is stored data, and stored data is checked. */
        const char *p = parents;
        while (st == ATLAS_OK && *p != '\0') {
            while (*p == ' ') {
                p++;
            }
            if (*p == '\0') {
                break;
            }
            const char *start = p;
            while (*p != '\0' && *p != ' ') {
                p++;
            }
            size_t n = (size_t)(p - start);
            if (n == 0 || n >= ATLAS_OID_HEX_MAX_INCL) {
                st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                   "a stored commit lists a parent id Atlas cannot use");
                break;
            }
            char parent[ATLAS_OID_HEX_MAX_INCL];
            memcpy(parent, start, n);
            parent[n] = '\0';
            if (strcmp(parent, target_oid) == 0) {
                out->verdict = ATLAS_DB_GATE_ANCESTRY_REACHED;
                goto done;
            }
            if (!oid_set_has(&seen, parent)) {
                st = oid_set_add(&seen, parent, &added, err);
                if (st == ATLAS_OK && added) {
                    st = oid_queue_push(&todo, parent, err);
                }
            }
        }
    }

    if (st == ATLAS_OK && out->verdict == ATLAS_DB_GATE_ANCESTRY_UNKNOWN) {
        if (!head_known) {
            /* The indexed head is not in `commits`. Nothing was proved. */
            out->verdict = ATLAS_DB_GATE_ANCESTRY_UNKNOWN;
        } else if (out->missing_parent) {
            /* The walk ran out of ingested history before it ran out of
             * candidates. The target may be beyond the edge of what Atlas
             * holds, so absence here is not evidence of absence there. */
            out->verdict = ATLAS_DB_GATE_ANCESTRY_UNKNOWN;
        } else {
            /* Every reachable commit was expanded and none of them was the
             * target. This is the one case in which "not an ancestor" is a
             * finding rather than a shrug. */
            out->verdict = ATLAS_DB_GATE_ANCESTRY_NOT_ANCESTOR;
        }
    }

done:
    atlas_db_finish(db, s);
    oid_set_free(&seen);
    oid_queue_free(&todo);
    return st;
}

/* --- the change range ------------------------------------------------------
 *
 * The paths touched by every commit reachable from the head but not from the
 * validation point.
 *
 * Implemented as the same bounded walk, collecting `file_changes` rows for each
 * commit it expands and stopping at the target rather than walking through it.
 * A recursive CTE would be shorter and would also be unbounded, and an
 * unbounded query over a repository's whole history is exactly what this phase
 * must not do once per gate query. */

atlas_status atlas_db_gate_range_paths(atlas_db *db, int64_t repo_id, const char *head_oid,
                                       const char *stop_oid, atlas_db_gate_range *out,
                                       atlas_err *err) {
    memset(out, 0, sizeof(*out));
    if (head_oid == NULL || head_oid[0] == '\0') {
        out->limit_reached = true;
        return ATLAS_OK;
    }
    if (stop_oid != NULL && strcmp(head_oid, stop_oid) == 0) {
        /* An empty range, and an exactly empty one: the validation point is the
         * indexed head, so nothing has happened since. This is the common case
         * on a quiet repository, and it costs one string comparison. */
        return ATLAS_OK;
    }

    sqlite3_stmt *parents = NULL;
    atlas_status st = atlas_db_prepare(
        db, "SELECT parents FROM commits WHERE repo_id = ?1 AND oid = ?2;", &parents, err);
    if (st != ATLAS_OK) {
        return st;
    }
    sqlite3_stmt *paths = NULL;
    if (st == ATLAS_OK) {
        st = atlas_db_prepare(db,
                              "SELECT fc.path_raw FROM file_changes fc"
                              " JOIN commits c ON c.id = fc.commit_id"
                              " WHERE c.repo_id = ?1 AND c.oid = ?2;",
                              &paths, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, parents);
        return st;
    }

    oid_set seen = {0};
    oid_queue todo = {0};
    bool added = false;
    st = oid_set_add(&seen, head_oid, &added, err);
    if (st == ATLAS_OK) {
        st = oid_queue_push(&todo, head_oid, err);
    }

    while (st == ATLAS_OK && todo.count > 0) {
        if (out->commits >= ATLAS_GATE_MAX_ANCESTRY_COMMITS ||
            out->paths.count >= ATLAS_GATE_MAX_CHANGED_PATHS) {
            out->limit_reached = true;
            break;
        }
        const char *cur = oid_queue_pop(&todo);
        char cur_copy[ATLAS_OID_HEX_MAX_INCL];
        (void)snprintf(cur_copy, sizeof cur_copy, "%s", cur);
        out->commits++;

        /* Paths this commit touched. */
        (void)sqlite3_reset(paths);
        (void)sqlite3_clear_bindings(paths);
        st = bind_i64(db, paths, 1, repo_id, err);
        if (st == ATLAS_OK) {
            st = atlas_db_bind_text_opt(db, paths, 2, cur_copy, err);
        }
        while (st == ATLAS_OK) {
            int rc = sqlite3_step(paths);
            if (rc == SQLITE_DONE) {
                break;
            }
            if (rc != SQLITE_ROW) {
                st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read a commit's changed paths");
                break;
            }
            const void *blob = sqlite3_column_blob(paths, 0);
            int len = sqlite3_column_bytes(paths, 0);
            if (blob == NULL || len <= 0) {
                continue;
            }
            if (out->paths.count >= ATLAS_GATE_MAX_CHANGED_PATHS) {
                out->limit_reached = true;
                break;
            }
            st = atlas_db_gate_paths_add(&out->paths, blob, (size_t)len, err);
        }
        if (st != ATLAS_OK || out->limit_reached) {
            break;
        }

        /* Then its parents, unless this is where the range stops. */
        (void)sqlite3_reset(parents);
        (void)sqlite3_clear_bindings(parents);
        st = bind_i64(db, parents, 1, repo_id, err);
        if (st == ATLAS_OK) {
            st = atlas_db_bind_text_opt(db, parents, 2, cur_copy, err);
        }
        if (st != ATLAS_OK) {
            break;
        }
        int rc = sqlite3_step(parents);
        if (rc == SQLITE_DONE) {
            out->missing_commit = true;
            continue;
        }
        if (rc != SQLITE_ROW) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read a commit's parents");
            break;
        }
        const char *plist = atlas_db_col_text_opt(parents, 0);
        if (plist == NULL) {
            continue;
        }
        const char *p = plist;
        while (st == ATLAS_OK && *p != '\0') {
            while (*p == ' ') {
                p++;
            }
            if (*p == '\0') {
                break;
            }
            const char *start = p;
            while (*p != '\0' && *p != ' ') {
                p++;
            }
            size_t n = (size_t)(p - start);
            if (n == 0 || n >= ATLAS_OID_HEX_MAX_INCL) {
                st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                   "a stored commit lists a parent id Atlas cannot use");
                break;
            }
            char parent[ATLAS_OID_HEX_MAX_INCL];
            memcpy(parent, start, n);
            parent[n] = '\0';
            /* The range is half-open: the validation point's own commit is not
             * part of what changed since it. */
            if (stop_oid != NULL && strcmp(parent, stop_oid) == 0) {
                continue;
            }
            if (!oid_set_has(&seen, parent)) {
                st = oid_set_add(&seen, parent, &added, err);
                if (st == ATLAS_OK && added) {
                    st = oid_queue_push(&todo, parent, err);
                }
            }
        }
    }

    atlas_db_finish(db, parents);
    atlas_db_finish(db, paths);
    oid_set_free(&seen);
    oid_queue_free(&todo);
    return st;
}

/* --- the changed-path set --------------------------------------------------
 *
 * Paths are bytes, so this is a set of byte strings and not of C strings. The
 * comparison is memcmp with the length compared first, which is the only
 * ordering that is correct for content that may contain anything at all. */

void atlas_db_gate_paths_init(atlas_db_gate_paths *p) {
    memset(p, 0, sizeof(*p));
}

void atlas_db_gate_paths_free(atlas_db_gate_paths *p) {
    for (size_t i = 0; i < p->count; i++) {
        free(p->items[i].bytes);
    }
    free(p->items);
    p->items = NULL;
    p->count = 0;
    p->cap = 0;
}

static int path_cmp(const void *a, size_t alen, const void *b, size_t blen) {
    size_t n = alen < blen ? alen : blen;
    int c = n == 0 ? 0 : memcmp(a, b, n);
    if (c != 0) {
        return c;
    }
    if (alen == blen) {
        return 0;
    }
    return alen < blen ? -1 : 1;
}

static size_t paths_lower(const atlas_db_gate_paths *p, const void *bytes, size_t len) {
    size_t lo = 0;
    size_t hi = p->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        if (path_cmp(p->items[mid].bytes, p->items[mid].len, bytes, len) < 0) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return lo;
}

bool atlas_db_gate_paths_has(const atlas_db_gate_paths *p, const void *bytes, size_t len) {
    size_t at = paths_lower(p, bytes, len);
    return at < p->count && path_cmp(p->items[at].bytes, p->items[at].len, bytes, len) == 0;
}

bool atlas_db_gate_paths_has_prefix(const atlas_db_gate_paths *p, const void *prefix,
                                    size_t prefix_len) {
    /* Every path under a directory sorts at or after the directory's own bytes,
     * so the scan starts at the lower bound and stops at the first entry that
     * does not share the prefix. No full pass. */
    for (size_t i = paths_lower(p, prefix, prefix_len); i < p->count; i++) {
        if (p->items[i].len < prefix_len ||
            memcmp(p->items[i].bytes, prefix, prefix_len) != 0) {
            return false;
        }
        /* An exact match, or a match at a path separator. `srcfoo` is not under
         * `src`. */
        if (p->items[i].len == prefix_len || p->items[i].bytes[prefix_len] == '/') {
            return true;
        }
    }
    return false;
}

atlas_status atlas_db_gate_paths_add(atlas_db_gate_paths *p, const void *bytes, size_t len,
                                     atlas_err *err) {
    size_t at = paths_lower(p, bytes, len);
    if (at < p->count && path_cmp(p->items[at].bytes, p->items[at].len, bytes, len) == 0) {
        return ATLAS_OK;
    }
    if (p->count == p->cap) {
        size_t cap = p->cap == 0 ? 64u : p->cap * 2u;
        void *grown = realloc(p->items, cap * sizeof(p->items[0]));
        if (grown == NULL) {
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
        }
        p->items = grown;
        p->cap = cap;
    }
    unsigned char *copy = malloc(len == 0 ? 1u : len);
    if (copy == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    if (len > 0) {
        memcpy(copy, bytes, len);
    }
    memmove(&p->items[at + 1u], &p->items[at], (p->count - at) * sizeof(p->items[0]));
    p->items[at].bytes = copy;
    p->items[at].len = len;
    p->count++;
    return ATLAS_OK;
}

void atlas_db_gate_range_free(atlas_db_gate_range *r) {
    atlas_db_gate_paths_free(&r->paths);
}

/* --- the revalidation ledger -----------------------------------------------
 *
 * One INSERT and two SELECTs. There is no UPDATE and no DELETE, and adding
 * either would be the change that makes every earlier validation record a claim
 * about something that may since have been edited. */

atlas_status atlas_db_gate_validation_insert(atlas_db *db,
                                             const atlas_db_gate_validation *v,
                                             int64_t *id_out, atlas_err *err) {
    *id_out = 0;
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "INSERT INTO decision_validations"
        "(document_id, revision_id, revision_no, content_hash, repo_id, repo_identity_hash,"
        " validated_at_commit, evidence_digest, intent, actor, challenge_id, prior_freshness,"
        " prior_reasons, created_at)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, 'revalidate', 'LOCAL_OPERATOR_CONFIRMED',"
        "        ?9, ?10, ?11, ?12);",
        &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, v->document_id, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, s, 2, v->revision_id, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, s, 3, v->revision_no, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 4, v->content_hash, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, s, 5, v->repo_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 6, v->repo_identity_hash, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 7, v->validated_at_commit, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 8, v->evidence_digest, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, s, 9, v->challenge_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 10, v->prior_freshness, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 11, v->prior_reasons, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 12, v->created_at, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    st = atlas_db_step_done(db, s, err);
    if (st == ATLAS_OK) {
        *id_out = sqlite3_last_insert_rowid(db->h);
    }
    return st;
}

atlas_status atlas_db_gate_validation_newest(atlas_db *db, int64_t revision_id,
                                             const char *repo_identity_hash,
                                             atlas_db_gate_validation *out, bool *found_out,
                                             atlas_err *err) {
    memset(out, 0, sizeof(*out));
    *found_out = false;
    /* Bound to the repository identity as well as the revision.
     *
     * Without that clause, one worktree's revalidation would establish a
     * validation point that another worktree's assessment measured from — and
     * the two are at different commits by definition. `repo_identity_hash` is
     * the durable identity, so this stays correct across a `repo remove` and a
     * re-registration of the same lineage at the same path. An empty stored
     * identity matches only an empty one, which is the honest behaviour for a
     * record written before the identity was knowable. */
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "SELECT id, document_id, revision_id, revision_no, content_hash, repo_id,"
        "       repo_identity_hash, validated_at_commit, evidence_digest, challenge_id,"
        "       prior_freshness, prior_reasons, created_at"
        "  FROM decision_validations"
        " WHERE revision_id = ?1 AND IFNULL(repo_identity_hash, '') = IFNULL(?2, '')"
        " ORDER BY id DESC LIMIT 1;",
        &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, revision_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 2, repo_identity_hash, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    int rc = sqlite3_step(s);
    if (rc == SQLITE_ROW) {
        out->id = sqlite3_column_int64(s, 0);
        out->document_id = sqlite3_column_int64(s, 1);
        out->revision_id = sqlite3_column_int64(s, 2);
        out->revision_no = sqlite3_column_int64(s, 3);
        st = atlas_db_col_copy(s, 4, out->content_hash, sizeof(out->content_hash),
                               "validation content hash", err);
        out->repo_id = sqlite3_column_int64(s, 5);
        if (st == ATLAS_OK) {
            st = atlas_db_col_copy(s, 6, out->repo_identity_hash,
                                   sizeof(out->repo_identity_hash), "repository identity", err);
        }
        if (st == ATLAS_OK) {
            st = atlas_db_col_copy(s, 7, out->validated_at_commit,
                                   sizeof(out->validated_at_commit), "validated commit", err);
        }
        if (st == ATLAS_OK) {
            st = atlas_db_col_copy(s, 8, out->evidence_digest, sizeof(out->evidence_digest),
                                   "evidence digest", err);
        }
        out->challenge_id = sqlite3_column_int64(s, 9);
        if (st == ATLAS_OK) {
            st = atlas_db_col_copy(s, 10, out->prior_freshness, sizeof(out->prior_freshness),
                                   "prior freshness", err);
        }
        if (st == ATLAS_OK) {
            st = atlas_db_col_copy(s, 11, out->prior_reasons, sizeof(out->prior_reasons),
                                   "prior reasons", err);
        }
        if (st == ATLAS_OK) {
            st = atlas_db_col_copy(s, 12, out->created_at, sizeof(out->created_at), "created_at",
                                   err);
        }
        *found_out = st == ATLAS_OK;
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read a revalidation record");
    }
    atlas_db_finish(db, s);
    return st;
}

atlas_status atlas_db_gate_validation_count(atlas_db *db, int64_t revision_id,
                                            const char *repo_identity_hash, int64_t *count_out,
                                            atlas_err *err) {
    *count_out = 0;
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "SELECT COUNT(*) FROM decision_validations"
        " WHERE revision_id = ?1 AND IFNULL(repo_identity_hash, '') = IFNULL(?2, '');",
        &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, revision_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 2, repo_identity_hash, err);
    }
    if (st == ATLAS_OK && sqlite3_step(s) == SQLITE_ROW) {
        *count_out = sqlite3_column_int64(s, 0);
    }
    atlas_db_finish(db, s);
    return st;
}

atlas_status atlas_db_gate_validations_for_document(atlas_db *db, int64_t document_id,
                                                    int64_t limit,
                                                    atlas_db_gate_validation_cb cb, void *ud,
                                                    atlas_err *err) {
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "SELECT id, document_id, revision_id, revision_no, content_hash, repo_id,"
        "       repo_identity_hash, validated_at_commit, evidence_digest, challenge_id,"
        "       prior_freshness, prior_reasons, created_at"
        "  FROM decision_validations WHERE document_id = ?1 ORDER BY id ASC LIMIT ?2;",
        &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, document_id, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, s, 2, limit, err);
    }
    while (st == ATLAS_OK) {
        int rc = sqlite3_step(s);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot list revalidation records");
            break;
        }
        atlas_db_gate_validation v;
        memset(&v, 0, sizeof v);
        v.id = sqlite3_column_int64(s, 0);
        v.document_id = sqlite3_column_int64(s, 1);
        v.revision_id = sqlite3_column_int64(s, 2);
        v.revision_no = sqlite3_column_int64(s, 3);
        st = atlas_db_col_copy(s, 4, v.content_hash, sizeof v.content_hash, "content hash", err);
        v.repo_id = sqlite3_column_int64(s, 5);
        if (st == ATLAS_OK) {
            st = atlas_db_col_copy(s, 6, v.repo_identity_hash, sizeof v.repo_identity_hash,
                                   "repository identity", err);
        }
        if (st == ATLAS_OK) {
            st = atlas_db_col_copy(s, 7, v.validated_at_commit, sizeof v.validated_at_commit,
                                   "validated commit", err);
        }
        if (st == ATLAS_OK) {
            st = atlas_db_col_copy(s, 8, v.evidence_digest, sizeof v.evidence_digest,
                                   "evidence digest", err);
        }
        v.challenge_id = sqlite3_column_int64(s, 9);
        if (st == ATLAS_OK) {
            st = atlas_db_col_copy(s, 10, v.prior_freshness, sizeof v.prior_freshness,
                                   "prior freshness", err);
        }
        if (st == ATLAS_OK) {
            st = atlas_db_col_copy(s, 11, v.prior_reasons, sizeof v.prior_reasons,
                                   "prior reasons", err);
        }
        if (st == ATLAS_OK) {
            st = atlas_db_col_copy(s, 12, v.created_at, sizeof v.created_at, "created_at", err);
        }
        if (st == ATLAS_OK) {
            st = cb(&v, ud, err);
        }
    }
    atlas_db_finish(db, s);
    return st;
}

/* --- what `atlas doctor` checks about this ledger --------------------------
 *
 * Structure only, and deliberately so. Every row must reference a revision that
 * exists, a document that exists, and a challenge that was actually consumed —
 * facts that cannot legitimately change, so a disagreement is damage.
 *
 * It does **not** re-derive the evidence digests against the live index. Those
 * are expected to drift: that is what the whole phase is about, and a
 * diagnostic that reported ordinary code changes as corruption would train
 * everyone to ignore it. Whether the evidence has moved is the assessment's
 * question, asked by `atlas gate check`, and answered fresh every time. */
atlas_status atlas_db_gate_verify(atlas_db *db, atlas_buf *out, atlas_err *err) {
    static const struct {
        const char *sql;
        const char *problem;
    } CHECKS[] = {
        {"SELECT COUNT(*) FROM decision_validations v"
         " LEFT JOIN decision_revisions r ON r.id = v.revision_id WHERE r.id IS NULL;",
         "revalidation records naming a revision that is not there"},
        {"SELECT COUNT(*) FROM decision_validations v"
         " LEFT JOIN decision_documents d ON d.id = v.document_id WHERE d.id IS NULL;",
         "revalidation records naming a document that is not there"},
        {"SELECT COUNT(*) FROM decision_validations v"
         " LEFT JOIN decision_challenges c ON c.id = v.challenge_id"
         " WHERE c.id IS NULL OR c.consumed = 0 OR c.intent <> 'revalidate';",
         "revalidation records whose capability was not a consumed revalidation challenge"},
        {"SELECT COUNT(*) FROM decision_validations v"
         " JOIN decision_revisions r ON r.id = v.revision_id"
         " WHERE v.content_hash <> r.content_hash;",
         "revalidation records bound to a digest the revision does not carry"},
        {"SELECT COUNT(*) FROM decision_validations WHERE validated_at_commit = '';",
         "revalidation records with no repository state"},
    };
    /* An older database does not have this table, and that is not damage — it
     * is a database from before A6.
     *
     * `atlas doctor` opens in INSPECT mode precisely so it can be run against
     * whatever is there, including an index an older Atlas wrote, and a
     * diagnostic that failed outright on one would be useless exactly when
     * somebody most needs it. The schema version is already reported as its own
     * finding; this check simply has nothing to say. */
    atlas_status st = ATLAS_OK;
    {
        sqlite3_stmt *probe = NULL;
        st = atlas_db_prepare(
            db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name=?1;", &probe, err);
        if (st != ATLAS_OK) {
            return st;
        }
        bool present = false;
        if (atlas_db_bind_text_opt(db, probe, 1, "decision_validations", err) == ATLAS_OK &&
            sqlite3_step(probe) == SQLITE_ROW) {
            present = sqlite3_column_int64(probe, 0) > 0;
        }
        atlas_db_finish(db, probe);
        if (!present) {
            return ATLAS_OK;
        }
    }
    for (size_t i = 0; i < sizeof CHECKS / sizeof CHECKS[0] && st == ATLAS_OK; i++) {
        sqlite3_stmt *s = NULL;
        st = atlas_db_prepare(db, CHECKS[i].sql, &s, err);
        if (st != ATLAS_OK) {
            break;
        }
        if (sqlite3_step(s) == SQLITE_ROW) {
            int64_t n = sqlite3_column_int64(s, 0);
            if (n > 0) {
                st = atlas_buf_appendf(out, err, "%s%lld %s", out->len > 0 ? "; " : "",
                                       (long long)n, CHECKS[i].problem);
            }
        }
        atlas_db_finish(db, s);
    }
    return st;
}
