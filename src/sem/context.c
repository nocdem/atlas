/* Atlas - impact, candidate tests, and the task context package.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Three answers, one rule: **every item says how it was found.**
 *
 * An impact report necessarily mixes things the compiler proved with things
 * Atlas guessed from a filename. A reader who cannot tell those apart is worse
 * off than one who was handed only the proven half, because they will act on
 * the guess with the confidence the proof earned. So nothing here returns a
 * bare list: each item carries an evidence class and a fixed reason saying
 * which question selected it, and the totals are reported split rather than
 * summed.
 *
 * The context builder adds one more constraint. It is **deterministic**: the
 * same repository, generation and request produce the same package. Ranking
 * therefore uses only counted, comparable facts — how many of the task's terms
 * a name contains, how far an item is from a seed, how strong its evidence —
 * and never a judgement, because a judgement is not reproducible and would make
 * two identical requests disagree.
 *
 * Task text is used **only to rank evidence Atlas already holds**. It selects
 * no repository, authorises nothing, and no imperative in it can cause a write:
 * everything here reads, and there is no code path from this file to a
 * mutation. That is not a check, it is an absence.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/pathrep.h"
#include "atlas/sem.h"
#include "atlas/sem_ops.h"
#include "atlas/service.h"
#include "core/service_internal.h"

/* --- the selection vocabulary ------------------------------------------------ */

static const char *const SELECTION_REASONS[] = {
    ATLAS_SEM_SEL_DIRECT_CALLER,      ATLAS_SEM_SEL_TRANSITIVE_CALLER,
    ATLAS_SEM_SEL_CALLEE,             ATLAS_SEM_SEL_DEFINED_HERE,
    ATLAS_SEM_SEL_INCLUDES,           ATLAS_SEM_SEL_INCLUDED_BY_SUBJECT,
    ATLAS_SEM_SEL_TYPE,               ATLAS_SEM_SEL_TEST_BY_REFERENCE,
    ATLAS_SEM_SEL_TEST_BY_NAME,       ATLAS_SEM_SEL_DECISION,
    ATLAS_SEM_SEL_SUBJECT,
};

const char *atlas_sem_selection_reason_intern(const char *reason) {
    if (reason == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(SELECTION_REASONS) / sizeof(SELECTION_REASONS[0]); i++) {
        if (strcmp(reason, SELECTION_REASONS[i]) == 0) {
            return SELECTION_REASONS[i];
        }
    }
    return NULL;
}

bool atlas_sem_selection_reason_is_known(const char *reason) {
    return atlas_sem_selection_reason_intern(reason) != NULL;
}

/* --- reports ------------------------------------------------------------------ */

void atlas_sem_impact_report_init(atlas_sem_impact_report *r) {
    memset(r, 0, sizeof(*r));
    atlas_repo_info_init(&r->repo);
    atlas_sem_generation_init(&r->generation);
}

void atlas_sem_impact_report_free(atlas_sem_impact_report *r) {
    if (r == NULL) {
        return;
    }
    atlas_repo_info_free(&r->repo);
    free(r->items);
    r->items = NULL;
}

void atlas_sem_context_req_init(atlas_sem_context_req *r) { memset(r, 0, sizeof(*r)); }

void atlas_sem_context_report_init(atlas_sem_context_report *r) {
    memset(r, 0, sizeof(*r));
    atlas_repo_info_init(&r->repo);
    atlas_sem_generation_init(&r->generation);
}

void atlas_sem_context_report_free(atlas_sem_context_report *r) {
    if (r == NULL) {
        return;
    }
    atlas_repo_info_free(&r->repo);
    free(r->items);
    r->items = NULL;
}

/* --- collecting items ---------------------------------------------------------- */

typedef struct item_list {
    atlas_sem_item **items;
    size_t *count;
    size_t *cap;
} item_list;

/* Appends unless the same (kind, name, file, line) is already present.
 *
 * Deduplication is by identity rather than by "have I seen this name", because
 * a symbol declared in a header and defined in a source file is two genuinely
 * different places a reader may need — but the *same* place selected twice by
 * two questions is one item, and the stronger reason wins. */
static atlas_status item_add(item_list *l, const char *kind, const char *name, const char *file,
                             int64_t line, atlas_sem_evidence ev, const char *why, int64_t depth,
                             atlas_err *err) {
    if (!atlas_sem_selection_reason_is_known(why)) {
        return ATLAS_OK; /* not one of Atlas' own reasons: not recorded */
    }
    for (size_t i = 0; i < *l->count; i++) {
        atlas_sem_item *e = &(*l->items)[i];
        if (strcmp(e->kind, kind) == 0 && strcmp(e->name, name) == 0 &&
            strcmp(e->file_text, file) == 0 && e->line == line) {
            /* Keep the strongest evidence and the shallowest depth: an item
             * reachable both directly and transitively is a direct one. */
            atlas_sem_evidence had = ATLAS_SEM_EV_UNKNOWN;
            (void)atlas_sem_evidence_parse(e->evidence, &had);
            if (atlas_sem_evidence_weaker(had, ev) == had && had != ev) {
                (void)snprintf(e->evidence, sizeof e->evidence, "%s",
                               atlas_sem_evidence_name(ev));
                e->why = atlas_sem_selection_reason_intern(why);
            }
            if (depth < e->depth) {
                e->depth = depth;
            }
            return ATLAS_OK;
        }
    }
    if (*l->count >= *l->cap) {
        size_t ncap = *l->cap == 0 ? 64 : *l->cap * 2;
        atlas_sem_item *ni = realloc(*l->items, ncap * sizeof(*ni));
        if (ni == NULL) {
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory collecting items");
        }
        *l->items = ni;
        *l->cap = ncap;
    }
    atlas_sem_item *it = &(*l->items)[(*l->count)++];
    memset(it, 0, sizeof(*it));
    (void)snprintf(it->kind, sizeof it->kind, "%s", kind);
    (void)snprintf(it->name, sizeof it->name, "%s", name);
    (void)snprintf(it->file_text, sizeof it->file_text, "%s", file);
    (void)snprintf(it->evidence, sizeof it->evidence, "%s", atlas_sem_evidence_name(ev));
    it->line = line;
    it->why = atlas_sem_selection_reason_intern(why);
    it->depth = depth;
    return ATLAS_OK;
}

/* --- A9.1: recorded knowledge as a context item ------------------------------
 *
 * A decision item's `name` is the record's public uid — Atlas-minted, carrying no
 * repository byte — and its `file_text` is the anchoring path. Its prose is
 * deliberately absent: a title is untrusted project text and the package is read
 * by a model, so what is offered is the identifier to fetch with
 * `atlas_decision`, where the prose arrives labelled. That is the A2 boundary,
 * unchanged. */
typedef struct decision_sink {
    item_list *list;
    /* The path this query anchored from, carried in rather than read from the
     * row: the row describes the document, and which of its links matched is the
     * caller's own question. */
    const char *anchor;
    bool include_history;
    atlas_err *err;
    atlas_status st;
} decision_sink;

static atlas_status take_decision(const atlas_decision_doc_row *row, void *ud, atlas_err *err) {
    (void)err;
    decision_sink *s = (decision_sink *)ud;
    if (s->st != ATLAS_OK) {
        return s->st;
    }
    const char *status = row->status != NULL ? row->status : "";
    /* What is *currently effective* by default. A proposal is not authority, and
     * a rejected, superseded or resolved record is history — including it beside
     * the effective ones without saying so would present withdrawn prose as
     * current. `--include-history` asks for the rest explicitly, and every item
     * still carries its own status. */
    if (!s->include_history && strcmp(status, "APPROVED") != 0) {
        return ATLAS_OK;
    }
    size_t before = *s->list->count;
    s->st = item_add(s->list, "decision", row->uid != NULL ? row->uid : "",
                     s->anchor != NULL ? s->anchor : "", 0, ATLAS_SEM_EV_LEXICAL,
                     ATLAS_SEM_SEL_DECISION, 0, s->err);
    if (s->st == ATLAS_OK && *s->list->count > before) {
        atlas_sem_item *it = &(*s->list->items)[*s->list->count - 1u];
        (void)snprintf(it->knowledge_kind, sizeof it->knowledge_kind, "%s",
                       row->kind != NULL ? row->kind : "DECISION");
        (void)snprintf(it->knowledge_status, sizeof it->knowledge_status, "%s", status);
    }
    return s->st;
}

static void note_missing(atlas_sem_context_report *out, const char *what) {
    if (out->missing_count >= sizeof(out->missing) / sizeof(out->missing[0])) {
        return;
    }
    for (size_t i = 0; i < out->missing_count; i++) {
        if (out->missing[i] == what) {
            return;
        }
    }
    out->missing[out->missing_count++] = what;
}

/* --- A9.1: the recorded knowledge anchored to what the task touches ----------
 *
 * `ATLAS_SEM_SEL_DECISION` and `ATLAS_SEM_MISSING_DECISIONS` were in the
 * vocabulary from A8-CI and nothing ever produced either, so a task-context
 * package described the code and silently omitted every rule, invariant and
 * outstanding obligation about it. That is the omission this season exists to
 * end: a package that lists twelve callers of a function and not the INVARIANT
 * saying what the function must preserve has left out the part a reader most
 * needs.
 *
 * Selection is by **path anchor**, which is the one relation between a knowledge
 * record and code that Atlas holds exactly: for each distinct file the request
 * named or the seeds reached, the records whose links name that path. Bounded on
 * both axes, deterministic, and it adds no new query shape — it is
 * `atlas_db_decision_for_path`, the same read `decision for-file` performs.
 *
 * **The caller's own paths anchor it too, and that is why this is a function.**
 * A repository with no semantic index returns early, and the comment at that
 * early return has always said the package is still useful because "the
 * repository, its decisions and its file index are all still there". Until A9.1
 * nothing produced a decision item, so the claim cost nothing and was never
 * tested; now it would be false. An explicitly named path needs no semantic
 * index to anchor a record, so the pass runs on both paths.
 *
 * The evidence class is **LEXICAL for every one of them, deliberately**. A path
 * link is a path somebody wrote down and matching it is a byte comparison; no
 * compiler established that this record governs this code, and A8-CI's rule is
 * that PROVEN means the compiler proved it. That the anchor is exact does not
 * make it compiler-derived.
 *
 * The kind is reported and **never ranked on**. Atlas has no basis for deciding
 * that an invariant matters more than an accepted risk, or a decision more than
 * an obligation; that is a judgement about the reader's task. What Atlas can do
 * is say which is which, so the reader can. */
static atlas_status add_knowledge(atlas_db *db, const atlas_sem_context_req *req,
                                  atlas_sem_context_report *out, item_list *list, size_t *count,
                                  atlas_err *err) {
    const char *anchors[ATLAS_SEM_CONTEXT_MAX_DECISION_ANCHORS];
    size_t nanchors = 0;
    const size_t cap = sizeof anchors / sizeof anchors[0];

    /* The caller's own paths first: they are the most direct statement of what
     * the task is about, so they must not be crowded out by a ceiling that a
     * ranked seed expansion filled. */
    const char *p = req->paths;
    const char *pend = p != NULL ? p + req->paths_len : NULL;
    while (p != NULL && p < pend && *p != '\0' && nanchors < cap) {
        anchors[nanchors++] = p;
        p += strlen(p) + 1;
    }
    /* Then the distinct files the seeds reached. A `decision` item has no file to
     * anchor from, so only code items seed this — which also makes the pass
     * non-recursive by construction. */
    for (size_t i = 0; i < *count && nanchors < cap; i++) {
        const char *f = (*list->items)[i].file_text;
        if (f[0] == '\0' || strcmp((*list->items)[i].kind, "decision") == 0) {
            continue;
        }
        bool seen = false;
        for (size_t k = 0; k < nanchors; k++) {
            if (strcmp(anchors[k], f) == 0) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            anchors[nanchors++] = f;
        }
    }

    atlas_status st = ATLAS_OK;
    size_t before = *count;
    for (size_t i = 0; st == ATLAS_OK && i < nanchors; i++) {
        atlas_buf raw = ATLAS_BUF_INIT;
        st = atlas_path_text_decode(anchors[i], strlen(anchors[i]), &raw, err);
        if (st == ATLAS_OK) {
            decision_sink sink = {list, anchors[i], req->include_history, err, ATLAS_OK};
            int64_t n = 0;
            bool more = false;
            atlas_err ignored;
            atlas_err_init(&ignored);
            /* A failure to read one anchor's records must not empty the rest of
             * the package: the code half is still true. */
            (void)atlas_db_decision_for_path(db, out->repo.id, raw.data, raw.len, NULL,
                                             ATLAS_SEM_CONTEXT_MAX_DECISIONS_PER_ANCHOR,
                                             take_decision, &sink, &n, &more, &ignored);
            st = sink.st;
        }
        atlas_buf_free(&raw);
    }
    if (st == ATLAS_OK && *count == before) {
        note_missing(out, ATLAS_SEM_MISSING_DECISIONS);
    }
    return st;
}

/* --- test candidates ------------------------------------------------------------
 *
 * Two ways a file becomes a candidate test, and they are *not* the same
 * strength:
 *
 *   - It is a test file that **references the subject**. The reference is
 *     compiler-derived, so the link between the test and the subject is
 *     PROVEN; that the file is a test is still a judgement about its path.
 *     Recorded as PROVEN with the reference reason, because the useful claim —
 *     "this file uses the thing you are changing" — is the proven half.
 *   - Its **name resembles** the subject. That is text about text: LEXICAL,
 *     always, however obviously right it looks. `test_foo.c` is evidence about
 *     `foo` and calling it anything stronger would be a lie about how it was
 *     found.
 *
 * Atlas has no proven test-to-code relationship to offer, because none exists
 * in C: a test is a program that happens to call things. So the strongest
 * honest answer is "this test references it", and that is what is returned. */
static bool path_looks_like_a_test(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return false;
    }
    /* Substring checks on path components only. Deliberately conservative: a
     * false positive is a wasted suggestion, and the item says it was selected
     * by naming so a reader can discount it. */
    return strstr(path, "test") != NULL || strstr(path, "Test") != NULL ||
           strstr(path, "spec") != NULL;
}

typedef struct test_sink {
    item_list *list;
    const char *subject;
    atlas_status st;
} test_sink;

static atlas_status take_test_ref(const atlas_sem_edge_row *row, void *ud, atlas_err *err) {
    test_sink *t = (test_sink *)ud;
    if (!path_looks_like_a_test(row->file_text)) {
        return ATLAS_OK;
    }
    t->st = item_add(t->list, "file", row->file_text, row->file_text, row->line,
                     ATLAS_SEM_EV_PROVEN, ATLAS_SEM_SEL_TEST_BY_REFERENCE, 1, err);
    return t->st;
}

/* --- the subject ---------------------------------------------------------------
 *
 * A subject is a symbol name or a repository-relative path, and Atlas decides
 * which by asking the index rather than by looking at the string. A name that
 * is both — a file called `main.c` and a symbol called `main.c` cannot both
 * exist, but a symbol named `parse` and a directory named `parse` can — is
 * resolved as a symbol first, because that is the question `code impact` is
 * usually asked. */
typedef struct subject_sink {
    char usr[ATLAS_SEM_MAX_USR_BYTES];
    char name[ATLAS_SEM_MAX_NAME_BYTES];
    char file[512];
    int64_t line;
    size_t distinct;
    atlas_buf seen;
} subject_sink;

static atlas_status take_subject(const atlas_sem_symbol_row *row, void *ud, atlas_err *err) {
    subject_sink *s = (subject_sink *)ud;
    const char *p = (const char *)s->seen.data;
    const char *end = p + s->seen.len;
    while (p < end) {
        if (strcmp(p, row->usr) == 0) {
            return ATLAS_OK;
        }
        p += strlen(p) + 1;
    }
    s->distinct++;
    if (s->distinct == 1) {
        (void)snprintf(s->usr, sizeof s->usr, "%s", row->usr);
        (void)snprintf(s->name, sizeof s->name, "%s", row->name);
        (void)snprintf(s->file, sizeof s->file, "%s", row->file_text);
        s->line = row->line;
    }
    return atlas_buf_append(&s->seen, row->usr, strlen(row->usr) + 1, err);
}

/* --- impact --------------------------------------------------------------------- */

typedef struct walk_collect {
    item_list *list;
    const char *why_direct;
    const char *why_deep;
    atlas_status st;
} walk_collect;

static atlas_status take_reached(const atlas_sem_walk_row *row, void *ud, atlas_err *err) {
    walk_collect *w = (walk_collect *)ud;
    atlas_sem_evidence ev = ATLAS_SEM_EV_UNKNOWN;
    (void)atlas_sem_evidence_parse(row->evidence, &ev);
    w->st = item_add(w->list, "symbol", row->name[0] != '\0' ? row->name : row->usr,
                     row->file_text, row->line, ev,
                     row->depth <= 1 ? w->why_direct : w->why_deep, row->depth, err);
    return w->st;
}

typedef struct includer_collect {
    item_list *list;
    atlas_status st;
} includer_collect;

static atlas_status take_includer(const atlas_sem_edge_row *row, void *ud, atlas_err *err) {
    includer_collect *c = (includer_collect *)ud;
    /* The include graph is what the preprocessor actually did, so an inclusion
     * is PROVEN — but only about the directive, not about whether the including
     * file cares about the change. */
    c->st = item_add(c->list, "file", row->src_usr, row->src_usr, row->line,
                     ATLAS_SEM_EV_PROVEN, ATLAS_SEM_SEL_INCLUDES, 1, err);
    return c->st;
}

typedef struct file_symbol_collect {
    item_list *list;
    atlas_status st;
} file_symbol_collect;

static atlas_status take_file_symbol(const atlas_sem_symbol_row *row, void *ud, atlas_err *err) {
    file_symbol_collect *c = (file_symbol_collect *)ud;
    if (!row->is_definition) {
        return ATLAS_OK;
    }
    c->st = item_add(c->list, "symbol", row->name, row->file_text, row->line,
                     ATLAS_SEM_EV_PROVEN, ATLAS_SEM_SEL_DEFINED_HERE, 0, err);
    return c->st;
}

/* A shallow copy of the fields a report needs. `atlas_repo_info` owns buffers,
 * so this copies the owned ones rather than aliasing them — the memory rule. */
static atlas_status copy_repo(atlas_repo_info *dst, const atlas_repo_info *src, atlas_err *err) {
    dst->id = src->id;
    (void)snprintf(dst->name, sizeof dst->name, "%s", src->name);
    (void)snprintf(dst->scanned_head, sizeof dst->scanned_head, "%s", src->scanned_head);
    (void)snprintf(dst->current_branch, sizeof dst->current_branch, "%s", src->current_branch);
    atlas_status st = atlas_buf_set(&dst->root_path, src->root_path.data, src->root_path.len, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&dst->root_path_text, src->root_path_text.data,
                           src->root_path_text.len, err);
    }
    return st;
}

/* The core, taking a raw handle and an already-resolved repository.
 *
 * Split out so the CLI (which holds an `atlas_ctx`) and the daemon (which holds
 * a read-only `atlas_db` and has resolved the repository itself) run *the same*
 * implementation. Parity between the two surfaces is then structural rather
 * than a pair of functions somebody has to keep in step. */
atlas_status atlas_sem_impact_on(atlas_db *db, const atlas_repo_info *repo, const char *subject,
                                 int64_t depth, int64_t limit, atlas_sem_impact_report *out,
                                 atlas_err *err) {
    atlas_status st = copy_repo(&out->repo, repo, err);
    if (st != ATLAS_OK) {
        return st;
    }
    bool found = false;
    st = atlas_db_sem_current(db, out->repo.id, &out->generation, &found, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (!found) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "no semantic index exists for this repository; an operator builds "
                             "one with `atlas code index`");
    }
    out->freshness = atlas_sem_freshness_of(&out->generation, true, false,
                                            out->repo.scanned_head, NULL, true, &out->stale_reason);
    (void)snprintf(out->query, sizeof out->query, "%s", subject);

    item_list list = {&out->items, &out->count, &out->cap};
    int64_t gen = out->generation.id;
    if (depth <= 0) {
        depth = ATLAS_SEM_DEFAULT_DEPTH;
    }

    /* Is the subject a symbol? */
    subject_sink sub;
    memset(&sub, 0, sizeof(sub));
    atlas_buf_init(&sub.seen);
    int64_t total = 0;
    bool trunc = false;
    st = atlas_db_sem_symbols_by_name(db, gen, subject, NULL, NULL,
                                      ATLAS_SEM_MAX_ROWS, take_subject, &sub, &total, &trunc, err);
    atlas_buf_free(&sub.seen);
    if (st != ATLAS_OK) {
        return st;
    }

    if (sub.distinct > 0) {
        out->subject_found = true;
        st = item_add(&list, "symbol", sub.name, sub.file, sub.line, ATLAS_SEM_EV_PROVEN,
                      ATLAS_SEM_SEL_SUBJECT, 0, err);

        /* Who reaches it. Transitive and bounded; the walk reports its own
         * truncation and how many indirect call sites it could not resolve, and
         * both are carried into the report rather than dropped. */
        if (st == ATLAS_OK) {
            atlas_sem_walk_opts o;
            atlas_sem_walk_opts_init(&o);
            o.usr = sub.usr;
            o.inbound = true;
            o.depth = depth;
            o.max_rows = limit > 0 ? limit : ATLAS_SEM_MAX_ROWS;
            walk_collect wc = {&list, ATLAS_SEM_SEL_DIRECT_CALLER,
                               ATLAS_SEM_SEL_TRANSITIVE_CALLER, ATLAS_OK};
            atlas_sem_walk_summary sum;
            st = atlas_sem_walk(db, gen, &o, take_reached, &wc, &sum, err);
            if (st == ATLAS_OK) {
                st = wc.st;
            }
            out->unresolved_indirect += sum.unresolved_indirect;
            if (sum.truncated) {
                out->truncated = true;
                out->truncated_reason = sum.truncated_reason;
            }
        }

        /* What it calls, one level: a change to a function is usually also a
         * change to what it depends on. */
        if (st == ATLAS_OK) {
            atlas_sem_walk_opts o;
            atlas_sem_walk_opts_init(&o);
            o.usr = sub.usr;
            o.inbound = false;
            o.depth = 1;
            o.max_rows = limit > 0 ? limit : ATLAS_SEM_MAX_ROWS;
            walk_collect wc = {&list, ATLAS_SEM_SEL_CALLEE, ATLAS_SEM_SEL_CALLEE, ATLAS_OK};
            atlas_sem_walk_summary sum;
            st = atlas_sem_walk(db, gen, &o, take_reached, &wc, &sum, err);
            if (st == ATLAS_OK) {
                st = wc.st;
            }
            out->unresolved_indirect += sum.unresolved_indirect;
        }

        /* Files that include the file the subject lives in. */
        if (st == ATLAS_OK && sub.file[0] != '\0') {
            includer_collect ic = {&list, ATLAS_OK};
            int64_t n = 0;
            bool t2 = false;
            st = atlas_db_sem_includers_of(db, gen, sub.file,
                                           limit > 0 ? limit : ATLAS_SEM_MAX_ROWS, take_includer,
                                           &ic, &n, &t2, err);
            if (st == ATLAS_OK) {
                st = ic.st;
            }
        }

        /* Tests that reference it. */
        if (st == ATLAS_OK) {
            test_sink ts = {&list, sub.usr, ATLAS_OK};
            int64_t n = 0;
            bool t2 = false;
            st = atlas_db_sem_edges_of(db, gen, sub.usr, true, NULL, false,
                                       ATLAS_SEM_MAX_ROWS, take_test_ref, &ts, &n, &t2, err);
            if (st == ATLAS_OK) {
                st = ts.st;
            }
        }
    } else {
        /* Not a symbol: treat it as a file. */
        out->subject_is_path = true;
        file_symbol_collect fc = {&list, ATLAS_OK};
        int64_t n = 0;
        bool t2 = false;
        st = atlas_db_sem_symbols_in_file(db, gen, subject,
                                          limit > 0 ? limit : ATLAS_SEM_MAX_ROWS,
                                          take_file_symbol, &fc, &n, &t2, err);
        if (st == ATLAS_OK) {
            st = fc.st;
        }
        out->subject_found = n > 0;
        if (st == ATLAS_OK) {
            includer_collect ic = {&list, ATLAS_OK};
            st = atlas_db_sem_includers_of(db, gen, subject,
                                           limit > 0 ? limit : ATLAS_SEM_MAX_ROWS, take_includer,
                                           &ic, &n, &t2, err);
            if (st == ATLAS_OK) {
                st = ic.st;
            }
        }
    }

    /* Tally, split. A total would hide exactly the distinction this layer
     * exists to keep. */
    for (size_t i = 0; i < out->count; i++) {
        atlas_sem_evidence ev = ATLAS_SEM_EV_UNKNOWN;
        (void)atlas_sem_evidence_parse(out->items[i].evidence, &ev);
        switch (ev) {
            case ATLAS_SEM_EV_PROVEN:
                out->proven++;
                break;
            case ATLAS_SEM_EV_CANDIDATE:
                out->candidate++;
                break;
            case ATLAS_SEM_EV_LEXICAL:
                out->lexical++;
                break;
            case ATLAS_SEM_EV_UNKNOWN:
            default:
                break;
        }
    }
    return st;
}

/* --- the context package --------------------------------------------------------
 *
 * Ranking is a small integer score, and every term of it is a counted fact:
 *
 *   +8  the item is a seed, or the subject the task named
 *   +4  per task term the item's name or path contains (capped)
 *   +3  evidence PROVEN, +1 CANDIDATE, 0 LEXICAL
 *   -1  per level of depth from a seed
 *
 * Nothing here consults a model, and nothing depends on the order rows came
 * back in: ties break on (file, line, name), so the package is byte-identical
 * across runs over one generation. That determinism is the property the tests
 * assert, and it is why the weights are integers rather than anything cleverer.
 */
static int score_of(const atlas_sem_item *it, const char *const *terms, size_t nterms) {
    int score = 0;
    if (it->why != NULL && strcmp(it->why, ATLAS_SEM_SEL_SUBJECT) == 0) {
        score += 8;
    }
    int matched = 0;
    for (size_t i = 0; i < nterms && matched < 3; i++) {
        if (terms[i][0] == '\0') {
            continue;
        }
        if (strstr(it->name, terms[i]) != NULL || strstr(it->file_text, terms[i]) != NULL) {
            score += 4;
            matched++;
        }
    }
    atlas_sem_evidence ev = ATLAS_SEM_EV_UNKNOWN;
    (void)atlas_sem_evidence_parse(it->evidence, &ev);
    if (ev == ATLAS_SEM_EV_PROVEN) {
        score += 3;
    } else if (ev == ATLAS_SEM_EV_CANDIDATE) {
        score += 1;
    }
    score -= (int)it->depth;
    return score;
}

typedef struct ranked {
    const atlas_sem_item *it;
    int score;
} ranked;

static int rank_cmp(const void *a, const void *b) {
    const ranked *x = (const ranked *)a;
    const ranked *y = (const ranked *)b;
    if (x->score != y->score) {
        return x->score > y->score ? -1 : 1;
    }
    /* Deterministic tie-break. Without this the package would depend on the
     * order SQLite happened to return rows in, and two identical requests could
     * disagree — which is the one thing a context builder must never do. */
    int c = strcmp(x->it->file_text, y->it->file_text);
    if (c != 0) {
        return c;
    }
    if (x->it->line != y->it->line) {
        return x->it->line < y->it->line ? -1 : 1;
    }
    return strcmp(x->it->name, y->it->name);
}

/* Splits the task into lowercase terms of three characters or more.
 *
 * Deliberately crude: this is a ranking aid, not comprehension. A term that
 * happens to appear in a symbol name lifts that symbol; a term that appears
 * nowhere costs nothing. Nothing about the task is interpreted as an
 * instruction, because nothing here can act on one. */
static size_t split_terms(const char *task, char *buf, size_t bufsz, const char **terms,
                          size_t max_terms) {
    size_t n = 0;
    size_t used = 0;
    const char *p = task;
    while (*p != '\0' && n < max_terms && used + 1 < bufsz) {
        while (*p != '\0' && !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                               (*p >= '0' && *p <= '9') || *p == '_')) {
            p++;
        }
        size_t start = used;
        while (*p != '\0' && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                              (*p >= '0' && *p <= '9') || *p == '_') &&
               used + 1 < bufsz) {
            char c = *p;
            if (c >= 'A' && c <= 'Z') {
                c = (char)(c - 'A' + 'a');
            }
            buf[used++] = c;
            p++;
        }
        buf[used++] = '\0';
        if (used - start >= 4u) { /* three characters plus the NUL */
            terms[n++] = buf + start;
        } else {
            used = start;
        }
    }
    return n;
}

/* The `atlas_ctx` wrapper: resolve the repository, then run the same core. */
atlas_status atlas_service_sem_impact(atlas_ctx *ctx, const char *name, const char *subject,
                                      int64_t depth, int64_t limit,
                                      atlas_sem_impact_report *out, atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_service_require_repo(ctx, name, &info, err);
    if (st == ATLAS_OK) {
        st = atlas_sem_impact_on(atlas_ctx_db(ctx), &info, subject, depth, limit, out, err);
    }
    atlas_repo_info_free(&info);
    return st;
}

/* The context core, over a raw handle and an already-resolved repository — the
 * same split `atlas_sem_impact_on` makes, and for the same reason: the CLI and
 * the daemon must run one implementation, not two that agree today. */
atlas_status atlas_sem_context_on(atlas_db *db, const atlas_repo_info *repo,
                                  const atlas_sem_context_req *req,
                                  atlas_sem_context_report *out, atlas_err *err) {
    if (req == NULL || repo == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "context build needs a repository");
    }
    if (req->task == NULL || req->task[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "context build needs a --task description");
    }
    if (strlen(req->task) > ATLAS_SEM_CONTEXT_MAX_TASK_BYTES) {
        /* Refused, not truncated: a ranked answer to half a question is worse
         * than a refusal, and A5's rule is that bounds refuse rather than
         * clamp. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "the task description exceeds %u bytes",
                             ATLAS_SEM_CONTEXT_MAX_TASK_BYTES);
    }

    atlas_status st = copy_repo(&out->repo, repo, err);
    if (st != ATLAS_OK) {
        return st;
    }
    (void)snprintf(out->task, sizeof out->task, "%s", req->task);

    out->budget_bytes = req->max_bytes > 0 ? req->max_bytes
                        : req->max_tokens > 0
                            ? req->max_tokens * ATLAS_SEM_BYTES_PER_TOKEN
                            : ATLAS_SEM_CONTEXT_DEFAULT_BYTES;
    if (out->budget_bytes > ATLAS_SEM_CONTEXT_MAX_BYTES) {
        out->budget_bytes = ATLAS_SEM_CONTEXT_MAX_BYTES;
    }
    int64_t max_items = req->max_items > 0 && req->max_items < ATLAS_SEM_CONTEXT_MAX_ITEMS
                            ? req->max_items
                            : ATLAS_SEM_CONTEXT_MAX_ITEMS;

    bool found = false;
    st = atlas_db_sem_current(db, out->repo.id, &out->generation, &found, err);
    if (st != ATLAS_OK) {
        return st;
    }
    /* A missing or stale index does not fail the request. The package is still
     * useful — the repository, its decisions and its file index are all still
     * there — and it must say what it could not supply rather than read as
     * complete.
     *
     * **A9.1 made that sentence true rather than merely written.** It used to
     * return here, so a repository with no semantic index got an empty package
     * whose only content was the note saying why — including none of the
     * decisions the comment claimed were still there, because until A9.1 nothing
     * produced a decision item at all. Now the knowledge pass runs on this path
     * too, anchored on the paths the caller named, and the package is ranked and
     * filled exactly as on the ordinary path. What is absent is the code half,
     * and the note is what says so. */
    if (!found) {
        note_missing(out, ATLAS_SEM_MISSING_INDEX);
        out->freshness = ATLAS_SEM_FRESH_ABSENT;
    } else {
        out->freshness = atlas_sem_freshness_of(&out->generation, true, false,
                                                out->repo.scanned_head, NULL, true,
                                                &out->stale_reason);
        if (out->freshness == ATLAS_SEM_FRESH_STALE) {
            note_missing(out, ATLAS_SEM_MISSING_STALE);
        }
    }

    char termbuf[ATLAS_SEM_CONTEXT_MAX_TASK_BYTES];
    const char *terms[ATLAS_SEM_CONTEXT_MAX_TERMS];
    size_t nterms = split_terms(req->task, termbuf, sizeof termbuf, terms,
                                sizeof terms / sizeof terms[0]);

    /* Seeds: what the caller named, then what the task's terms match. */
    atlas_sem_item *items = NULL;
    size_t count = 0;
    size_t cap = 0;
    item_list list = {&items, &count, &cap};
    int64_t gen = out->generation.id;
    int64_t depth = req->depth > 0 ? req->depth : 2;
    bool any_seed = false;

    const char *p = req->symbols;
    const char *end = found && p != NULL ? p + req->symbols_len : NULL;
    while (st == ATLAS_OK && found && p != NULL && p < end && *p != '\0') {
        atlas_sem_impact_report imp;
        atlas_sem_impact_report_init(&imp);
        atlas_err ignored;
        atlas_err_init(&ignored);
        if (atlas_sem_impact_on(db, &out->repo, p, depth, ATLAS_SEM_MAX_ROWS, &imp,
                                &ignored) == ATLAS_OK &&
            imp.subject_found) {
            any_seed = true;
            for (size_t i = 0; i < imp.count && st == ATLAS_OK; i++) {
                atlas_sem_evidence ev = ATLAS_SEM_EV_UNKNOWN;
                (void)atlas_sem_evidence_parse(imp.items[i].evidence, &ev);
                st = item_add(&list, imp.items[i].kind, imp.items[i].name,
                              imp.items[i].file_text, imp.items[i].line, ev, imp.items[i].why,
                              imp.items[i].depth, err);
            }
        }
        atlas_sem_impact_report_free(&imp);
        p += strlen(p) + 1;
    }

    /* No explicit seed: rank the whole symbol table by the task's terms.
     * Bounded by the row ceiling, and the package says so when it finds
     * nothing. */
    if (st == ATLAS_OK && found && !any_seed) {
        for (size_t t = 0; t < nterms && st == ATLAS_OK; t++) {
            subject_sink probe;
            memset(&probe, 0, sizeof(probe));
            atlas_buf_init(&probe.seen);
            int64_t n = 0;
            bool tr = false;
            atlas_err ignored;
            atlas_err_init(&ignored);
            bool hit = atlas_db_sem_symbols_by_name(db, gen, terms[t], NULL, NULL,
                                                    4, take_subject, &probe, &n, &tr,
                                                    &ignored) == ATLAS_OK &&
                       probe.distinct > 0;
            atlas_buf_free(&probe.seen);
            if (!hit) {
                continue;
            }
            any_seed = true;
            /* A term that matched is expanded exactly as an explicitly named
             * seed is. Adding the bare symbol and stopping there would produce
             * a package listing names with no neighbourhood — which is the
             * least useful thing this command could return, because a caller
             * asking for context already knows the name they typed. */
            atlas_sem_impact_report imp;
            atlas_sem_impact_report_init(&imp);
            atlas_err ignored2;
            atlas_err_init(&ignored2);
            if (atlas_sem_impact_on(db, &out->repo, terms[t], depth, ATLAS_SEM_MAX_ROWS,
                                    &imp, &ignored2) == ATLAS_OK &&
                imp.subject_found) {
                for (size_t i = 0; i < imp.count && st == ATLAS_OK; i++) {
                    atlas_sem_evidence ev = ATLAS_SEM_EV_UNKNOWN;
                    (void)atlas_sem_evidence_parse(imp.items[i].evidence, &ev);
                    st = item_add(&list, imp.items[i].kind, imp.items[i].name,
                                  imp.items[i].file_text, imp.items[i].line, ev,
                                  imp.items[i].why, imp.items[i].depth, err);
                }
            }
            atlas_sem_impact_report_free(&imp);
        }
    }
    if (found && !any_seed) {
        /* Only when there was an index to search. With none, the missing index is
         * the reason there are no seeds, and saying both would report one fact
         * twice. */
        note_missing(out, ATLAS_SEM_MISSING_SEEDS);
    }

    if (st == ATLAS_OK) {
        st = add_knowledge(db, req, out, &list, &count, err);
    }

    /* Rank, then fill to the budget. */
    if (st == ATLAS_OK && count > 0) {
        ranked *r = calloc(count, sizeof(*r));
        if (r == NULL) {
            free(items);
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory ranking context");
        }
        for (size_t i = 0; i < count; i++) {
            r[i].it = &items[i];
            r[i].score = score_of(&items[i], terms, nterms);
        }
        qsort(r, count, sizeof(*r), rank_cmp);

        for (size_t i = 0; i < count && (int64_t)out->count < max_items; i++) {
            /* Each item costs what it will occupy: its name, its path and the
             * fixed labels around it. Counted rather than estimated, because a
             * budget that is not enforced is not a budget. */
            int64_t cost = (int64_t)(strlen(r[i].it->name) + strlen(r[i].it->file_text) +
                                     strlen(r[i].it->evidence) +
                                     (r[i].it->why != NULL ? strlen(r[i].it->why) : 0) + 24);
            if (out->used_bytes + cost > out->budget_bytes) {
                out->budget_reached = true;
                note_missing(out, ATLAS_SEM_MISSING_BUDGET);
                break;
            }
            if (out->count >= out->cap) {
                size_t ncap = out->cap == 0 ? 64 : out->cap * 2;
                atlas_sem_item *ni = realloc(out->items, ncap * sizeof(*ni));
                if (ni == NULL) {
                    st = atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory building context");
                    break;
                }
                out->items = ni;
                out->cap = ncap;
            }
            out->items[out->count++] = *r[i].it;
            out->used_bytes += cost;
        }
        free(r);
    }

    free(items);
    return st;
}

/* The `atlas_ctx` wrapper: resolve the repository from the registry, then run
 * the same core the daemon runs. */
atlas_status atlas_service_sem_context(atlas_ctx *ctx, const atlas_sem_context_req *req,
                                       atlas_sem_context_report *out, atlas_err *err) {
    if (req == NULL || req->repo == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "context build needs a repository");
    }
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_service_require_repo(ctx, req->repo, &info, err);
    if (st == ATLAS_OK) {
        st = atlas_sem_context_on(atlas_ctx_db(ctx), &info, req, out, err);
    }
    atlas_repo_info_free(&info);
    return st;
}
