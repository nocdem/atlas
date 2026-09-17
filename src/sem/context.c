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
 * a name contains, how far an item is from a seed, whether it is repository code —
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
    ATLAS_SEM_SEL_SUBJECT, ATLAS_SEM_SEL_TASK_MATCH,
    ATLAS_SEM_SEL_TEST_TRANSITIVE, ATLAS_SEM_SEL_TEST_RECORD,
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
            if (strcmp(why, ATLAS_SEM_SEL_SUBJECT) == 0 ||
                (strcmp(why, ATLAS_SEM_SEL_TASK_MATCH) == 0 &&
                 strcmp(e->why, ATLAS_SEM_SEL_SUBJECT) != 0)) {
                e->why = atlas_sem_selection_reason_intern(why);
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
    /* A9.2.2, §24: needed to ask what Atlas has established about the record on
     * the truth axis. A read only; the context builder writes nothing. */
    atlas_db *db;
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
        /* A9.2.2, §24. The third axis, so a negative claim nobody confirmed
         * cannot be read as a settled negative. Conservative: an established
         * value only when every live claim on the record agrees, so what a
         * reader sees by default is that Atlas has not established this.
         *
         * A read failure leaves UNKNOWN rather than propagating: a context
         * package that could not answer this question should say it does not
         * know, not refuse to be built. */
        atlas_verify_truth truth = ATLAS_TRUTH_UNKNOWN;
        (void)atlas_db_verify_truth_for_document(s->db, row->id, &truth, s->err);
        (void)snprintf(it->knowledge_truth, sizeof it->knowledge_truth, "%s",
                       atlas_verify_truth_name(truth));
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

static const char *const CONTEXT_ADVICE[] = {
    "Read atlas_sem_status to inspect build inputs, discovery obstacles and incomplete units; a current index may still have coverage gaps.",
    "Retry atlas_context_build with explicit paths or symbols; preserve the original task and narrow one scope at a time.",
    "Use atlas_sem_impact for the specific symbol or file, or increase the context budget; omitted items are not evidence of absence.",
    "Resolve the symbol with atlas_sem_symbol, then inspect atlas_sem_callers and atlas_sem_callees; unresolved indirect calls remain unknown.",
    "Read atlas_decisions with the path filter for each relevant file; missing records in this package do not establish that no project rule applies.",
    "Inspect the listed file scopes: zero translation units does not prove a file is absent, and headers may be covered through includers.",
    "Review candidate tests and recorded suite/target mappings, then run the repository's trusted checks; historical results do not verify this change.",
};

const char *atlas_sem_context_advice_intern(const char *text) {
    if (text == NULL) return NULL;
    for (size_t i = 0; i < sizeof CONTEXT_ADVICE / sizeof CONTEXT_ADVICE[0]; i++) {
        if (strcmp(text, CONTEXT_ADVICE[i]) == 0) return CONTEXT_ADVICE[i];
    }
    return NULL;
}

static void context_advice(atlas_sem_context_report *out) {
    bool want[sizeof CONTEXT_ADVICE / sizeof CONTEXT_ADVICE[0]] = {false};
    for (size_t i = 0; i < out->missing_count; i++) {
        const char *m = out->missing[i];
        if (!strcmp(m, ATLAS_SEM_MISSING_INDEX) || !strcmp(m, ATLAS_SEM_MISSING_STALE) ||
            !strcmp(m, ATLAS_SEM_MISSING_COVERAGE) || !strcmp(m, ATLAS_SEM_MISSING_DISCOVERY)) want[0] = true;
        if (!strcmp(m, ATLAS_SEM_MISSING_SEEDS) || !strcmp(m, ATLAS_SEM_MISSING_SEARCH)) want[1] = true;
        if (!strcmp(m, ATLAS_SEM_MISSING_ITEMS) || !strcmp(m, ATLAS_SEM_MISSING_BUDGET)) want[2] = true;
        if (!strcmp(m, ATLAS_SEM_MISSING_GRAPH)) want[3] = true;
        if (!strcmp(m, ATLAS_SEM_MISSING_TESTS)) want[6] = true;
        if (!strcmp(m, ATLAS_SEM_MISSING_DECISIONS) || !strcmp(m, ATLAS_SEM_MISSING_KNOWLEDGE)) want[4] = true;
    }
    for (size_t i = 0; i < out->scope_count; i++) {
        const atlas_sem_context_scope *f = &out->scope[i];
        if (!f->in_file_index || f->units == 0 || f->units != f->complete_units) want[5] = true;
    }
    out->next_step_count = 0;
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        if (want[i]) out->next_steps[out->next_step_count++] = CONTEXT_ADVICE[i];
    }
}

static atlas_status context_scope_add(atlas_db *db, const char *path,
                                      atlas_sem_context_report *out, atlas_err *err) {
    if (path == NULL || path[0] == '\0') return ATLAS_OK;
    atlas_buf raw = ATLAS_BUF_INIT, encoded = ATLAS_BUF_INIT;
    atlas_err ignored;
    atlas_err_init(&ignored);
    atlas_status st = atlas_path_text_decode(path, strlen(path), &raw, &ignored);
    if (st == ATLAS_OK) st = atlas_path_check_relative(raw.data, raw.len, &ignored);
    if (st != ATLAS_OK) { atlas_buf_free(&raw); return ATLAS_OK; }
    st = atlas_path_text_encode(raw.data, raw.len, &encoded, err);
    atlas_buf_free(&raw);
    if (st != ATLAS_OK) { atlas_buf_free(&encoded); return st; }
    char canonical[512];
    if (encoded.len >= sizeof canonical) {
        out->scope_truncated = true;
        atlas_buf_free(&encoded);
        return ATLAS_OK;
    }
    (void)snprintf(canonical, sizeof canonical, "%s", atlas_buf_cstr(&encoded));
    atlas_buf_free(&encoded);
    path = canonical;
    for (size_t i = 0; i < out->scope_count; i++) {
        if (strcmp(out->scope[i].path, path) == 0) return ATLAS_OK;
    }
    if (out->scope_count == ATLAS_SEM_CONTEXT_MAX_SCOPE || strlen(path) >= sizeof out->scope[0].path) {
        out->scope_truncated = true;
        return ATLAS_OK;
    }
    atlas_sem_context_scope *f = &out->scope[out->scope_count++];
    (void)snprintf(f->path, sizeof f->path, "%s", path);
    return atlas_db_sem_context_scope(db, out->repo.id, out->generation.id, path, f, err);
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
    char anchors[ATLAS_SEM_CONTEXT_MAX_DECISION_ANCHORS][512];
    size_t nanchors = 0;
    const size_t cap = sizeof anchors / sizeof anchors[0];

    /* The caller's own paths first: they are the most direct statement of what
     * the task is about, so they must not be crowded out by a ceiling that a
     * ranked seed expansion filled. */
    const char *p = req->paths;
    const char *pend = p != NULL ? p + req->paths_len : NULL;
    while (p != NULL && p < pend && *p != '\0' && nanchors < cap) {
        (void)snprintf(anchors[nanchors++], sizeof anchors[0], "%s", p);
        p += strlen(p) + 1;
    }
    if (p != NULL && p < pend && *p != '\0') {
        note_missing(out, ATLAS_SEM_MISSING_KNOWLEDGE);
    }
    /* Then the distinct files the seeds reached. A `decision` item has no file to
     * anchor from, so only code items seed this — which also makes the pass
     * non-recursive by construction. */
    for (size_t i = 0; i < *count; i++) {
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
            if (nanchors == cap) {
                note_missing(out, ATLAS_SEM_MISSING_KNOWLEDGE);
                continue;
            }
            (void)snprintf(anchors[nanchors++], sizeof anchors[0], "%s", f);
        }
    }

    atlas_status st = ATLAS_OK;
    size_t before = *count;
    for (size_t i = 0; st == ATLAS_OK && i < nanchors; i++) {
        atlas_buf raw = ATLAS_BUF_INIT;
        st = atlas_path_text_decode(anchors[i], strlen(anchors[i]), &raw, err);
        if (st == ATLAS_OK) {
            decision_sink sink = {list, db, anchors[i], req->include_history, err, ATLAS_OK};
            int64_t n = 0;
            bool more = false;
            atlas_err ignored;
            atlas_err_init(&ignored);
            /* A failure to read one anchor's records must not empty the rest of
             * the package: the code half is still true. */
            atlas_status read_st = atlas_db_decision_for_path(db, out->repo.id, raw.data, raw.len,
                                             req->include_history ? NULL : "APPROVED",
                                             ATLAS_SEM_CONTEXT_MAX_DECISIONS_PER_ANCHOR,
                                             take_decision, &sink, &n, &more, &ignored);
            if (read_st != ATLAS_OK || more) note_missing(out, ATLAS_SEM_MISSING_KNOWLEDGE);
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
    const char *p = path;
    while (*p != '\0') {
        const char *slash = strchr(p, '/');
        size_t n = slash != NULL ? (size_t)(slash - p) : strlen(p);
        if ((n == 4 && memcmp(p, "test", 4) == 0) ||
            (n == 5 && memcmp(p, "tests", 5) == 0) ||
            (n == 4 && memcmp(p, "spec", 4) == 0) ||
            (n >= 5 && memcmp(p, "test_", 5) == 0) ||
            (n >= 5 && memcmp(p, "spec_", 5) == 0)) return true;
        if (slash == NULL) break;
        p = slash + 1;
    }
    return false;
}

static bool context_test_path(const char *path, const char *roots) {
    return roots != NULL && roots[0] != '\0' ? atlas_sem_path_under_prefix(roots, path)
                                             : path_looks_like_a_test(path);
}

static atlas_status add_test_file(item_list *list, const char *file, int64_t line,
                                  atlas_sem_evidence ev, const char *why, int64_t depth,
                                  const char *roots, atlas_err *err) {
    size_t before = *list->count;
    atlas_status st = item_add(list, "file", file, file, line, ev, why, depth, err);
    if (st == ATLAS_OK && *list->count > before) {
        atlas_sem_item *it = &(*list->items)[before];
        (void)snprintf(it->test_classification, sizeof it->test_classification, "%s",
                       roots != NULL && roots[0] != '\0' ? "DECLARED_ROOT" : "NAME_HEURISTIC");
    }
    return st;
}

typedef struct test_sink {
    item_list *list;
    const char *roots;
    atlas_status st;
} test_sink;

static atlas_status take_test_ref(const atlas_sem_edge_row *row, void *ud, atlas_err *err) {
    test_sink *t = ud;
    if (!context_test_path(row->file_text, t->roots)) return ATLAS_OK;
    atlas_sem_evidence ev = ATLAS_SEM_EV_UNKNOWN;
    (void)atlas_sem_evidence_parse(row->evidence, &ev);
    t->st = add_test_file(t->list, row->file_text, row->line, ev,
                          ATLAS_SEM_SEL_TEST_BY_REFERENCE, 1, t->roots, err);
    return t->st;
}

typedef struct recorded_test_sink {
    item_list *list;
    const char *anchor;
    bool omitted;
} recorded_test_sink;

static atlas_status take_recorded_test(const atlas_verify_test_row *row, void *ud, atlas_err *err) {
    recorded_test_sink *s = ud;
    atlas_sem_item meta = {0};
    const char *values[] = {row->suite,row->name,row->result,row->commit,row->uid};
    char *fields[] = {meta.test_suite,meta.test_target,meta.test_result,meta.test_commit,meta.test_evidence_uid};
    size_t caps[] = {sizeof meta.test_suite,sizeof meta.test_target,sizeof meta.test_result,
                     sizeof meta.test_commit,sizeof meta.test_evidence_uid};
    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
        if (strlen(values[i]) >= caps[i]) { s->omitted = true; return ATLAS_OK; }
        memcpy(fields[i], values[i], strlen(values[i]) + 1);
    }
    const char *path = row->path[0] != '\0' ? row->path : s->anchor;
    if (strlen(path) >= sizeof meta.file_text) { s->omitted = true; return ATLAS_OK; }
    size_t before = *s->list->count;
    (void)snprintf(meta.name, sizeof meta.name, "%s:%s", row->suite, row->name);
    atlas_status st = item_add(s->list, "test", meta.name, path, 0, ATLAS_SEM_EV_LEXICAL,
                               ATLAS_SEM_SEL_TEST_RECORD, 1, err);
    if (st == ATLAS_OK && *s->list->count > before) {
        atlas_sem_item *it = &(*s->list->items)[before];
        memcpy(it->test_suite, meta.test_suite, sizeof it->test_suite);
        memcpy(it->test_target, meta.test_target, sizeof it->test_target);
        memcpy(it->test_result, meta.test_result, sizeof it->test_result);
        memcpy(it->test_commit, meta.test_commit, sizeof it->test_commit);
        memcpy(it->test_evidence_uid, meta.test_evidence_uid, sizeof it->test_evidence_uid);
        (void)snprintf(it->test_classification, sizeof it->test_classification, "%s", "RECORDED_TEST");
    }
    return st;
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
    const char *test_roots;
    bool tests;
} walk_collect;

static atlas_status take_reached(const atlas_sem_walk_row *row, void *ud, atlas_err *err) {
    walk_collect *w = (walk_collect *)ud;
    atlas_sem_evidence ev = ATLAS_SEM_EV_UNKNOWN;
    (void)atlas_sem_evidence_parse(row->evidence, &ev);
    w->st = item_add(w->list, "symbol", row->name[0] != '\0' ? row->name : row->usr,
                     row->file_text, row->line, ev,
                     row->depth <= 1 ? w->why_direct : w->why_deep, row->depth, err);
    if (w->st == ATLAS_OK && w->tests && context_test_path(row->file_text, w->test_roots)) {
        w->st = add_test_file(w->list, row->file_text, row->line, ev,
                              row->depth <= 1 ? ATLAS_SEM_SEL_TEST_BY_REFERENCE : ATLAS_SEM_SEL_TEST_TRANSITIVE,
                              row->depth, w->test_roots, err);
    }
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
static atlas_status impact_at(atlas_db *db, const atlas_repo_info *repo, const char *subject,
                               const char *usr, const atlas_sem_generation *generation,
                               const atlas_sem_trust *trust, int64_t depth, int64_t limit,
                               atlas_sem_impact_report *out, atlas_err *err) {
    atlas_status st = copy_repo(&out->repo, repo, err);
    if (st != ATLAS_OK) {
        return st;
    }
    bool found = false;
    if (generation != NULL) {
        out->generation = *generation;
        found = true;
    } else {
        st = atlas_db_sem_current(db, out->repo.id, &out->generation, &found, err);
    }
    if (st != ATLAS_OK) {
        return st;
    }
    if (!found) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "no semantic index exists for this repository; an operator builds "
                             "one with `atlas code index`");
    }
    /* A9.2.5. One gatherer, replacing `atlas_sem_freshness_now`: both compute
     * freshness from the same single pass, and calling both would hash every
     * source twice per response. */
    if (trust != NULL) {
        out->trust = *trust;
    } else {
        atlas_sem_trust_now(db, &out->repo, &out->generation, true, false, &out->trust);
    }
    out->freshness = out->trust.freshness;
    out->stale_reason = out->trust.stale_reason;
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
    st = atlas_db_sem_symbols_by_name(db, gen, subject, usr, NULL,
                                      ATLAS_SEM_MAX_ROWS, take_subject, &sub, &total, &trunc, err);
    atlas_buf_free(&sub.seen);
    if (st != ATLAS_OK) {
        return st;
    }

    atlas_sem_config cfg;
    atlas_sem_config_init(&cfg);
    st = atlas_db_sem_config_get(db, out->repo.id, &cfg, err);
    if (st != ATLAS_OK) { atlas_sem_config_free(&cfg); return st; }
    const char *test_roots = atlas_buf_cstr(&cfg.test_roots);
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
                               ATLAS_SEM_SEL_TRANSITIVE_CALLER, ATLAS_OK, test_roots, true};
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
            walk_collect wc = {&list, ATLAS_SEM_SEL_CALLEE, ATLAS_SEM_SEL_CALLEE, ATLAS_OK, test_roots, false};
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
            out->truncated = out->truncated || t2;
        }

        /* Tests that reference it. */
        if (st == ATLAS_OK) {
            test_sink ts = {&list, test_roots, ATLAS_OK};
            int64_t n = 0;
            bool t2 = false;
            st = atlas_db_sem_edges_of(db, gen, sub.usr, true, NULL, false,
                                       ATLAS_SEM_MAX_ROWS, take_test_ref, &ts, &n, &t2, err);
            if (st == ATLAS_OK) {
                st = ts.st;
            }
            out->truncated = out->truncated || t2;
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
        out->truncated = out->truncated || t2;
        out->subject_found = n > 0;
        if (st == ATLAS_OK) {
            includer_collect ic = {&list, ATLAS_OK};
            st = atlas_db_sem_includers_of(db, gen, subject,
                                           limit > 0 ? limit : ATLAS_SEM_MAX_ROWS, take_includer,
                                           &ic, &n, &t2, err);
            if (st == ATLAS_OK) {
                st = ic.st;
            }
            out->truncated = out->truncated || t2;
        }
    }

    if (st == ATLAS_OK) {
        const char *anchor = sub.distinct > 0 ? sub.file : subject;
        recorded_test_sink sink = {&list, anchor, false};
        bool more = false;
        st = atlas_db_verify_tests_for_scope(db, repo->id, anchor,
                sub.distinct > 0 ? sub.name : "", ATLAS_SEM_CONTEXT_TEST_RECORDS,
                take_recorded_test, &sink, &more, err);
        out->truncated = out->truncated || more || sink.omitted;
    }
    atlas_sem_config_free(&cfg);

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
    /* A9.2.5. Settled once the item list is final. An impact report is a search
     * like any other: nothing found over a generation that read a third of the
     * tree is not evidence that a change reaches nothing. */
    if (st == ATLAS_OK) {
        int64_t semantic_count = 0;
        for (size_t i = 0; i < out->count; i++) {
            if (strcmp(out->items[i].kind, "test") != 0) semantic_count++;
        }
        atlas_sem_trust_settle(&out->trust, semantic_count, out->truncated);
    }
    return st;
}

atlas_status atlas_sem_impact_on(atlas_db *db, const atlas_repo_info *repo, const char *subject,
                                 int64_t depth, int64_t limit, atlas_sem_impact_report *out,
                                 atlas_err *err) {
    return impact_at(db, repo, subject, NULL, NULL, NULL, depth, limit, out, err);
}

/* --- the context package --------------------------------------------------------
 *
 * Ranking is a small integer score, and every term of it is a counted fact:
 *
 *   +32 an exact seed, +24 a lexical seed candidate
 *   +4  per task term the item's name or path contains (capped)
 *   +8  a repository location; external callees receive no location bonus
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
        score += 32;
    } else if (it->why != NULL && strcmp(it->why, ATLAS_SEM_SEL_TASK_MATCH) == 0) {
        score += 24;
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
    /* Evidence says what was established, not whether it helps this task.
     * A compiler-proven call to an external allocator must not automatically
     * outrank a related repository test or knowledge record. */
    if (it->file_text[0] != '\0') {
        score += 8;
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
    c = strcmp(x->it->name, y->it->name);
    return c != 0 ? c : strcmp(x->it->kind, y->it->kind);
}

static bool is_test_item(const atlas_sem_item *it) {
    return strcmp(it->why, ATLAS_SEM_SEL_TEST_BY_REFERENCE) == 0 ||
           strcmp(it->why, ATLAS_SEM_SEL_TEST_BY_NAME) == 0 ||
           strcmp(it->why, ATLAS_SEM_SEL_TEST_TRANSITIVE) == 0 ||
           strcmp(it->why, ATLAS_SEM_SEL_TEST_RECORD) == 0;
}

/* Keep the leading subject, then offer one knowledge record and one test when
 * present. The rest retains score order. This reserves representation, never
 * ranks knowledge kinds or promotes their evidence. Limits still apply. */
static void diversify(ranked *r, size_t n) {
    size_t slot = 1;
    for (unsigned category = 0; category < 2 && slot < n; category++) {
        bool already = category == 0 ? strcmp(r[0].it->kind, "decision") == 0
                                      : is_test_item(r[0].it);
        if (already) continue;
        for (size_t i = slot; i < n; i++) {
            bool match = category == 0 ? strcmp(r[i].it->kind, "decision") == 0
                                      : is_test_item(r[i].it);
            if (!match) continue;
            ranked chosen = r[i];
            memmove(r + slot + 1, r + slot, (i - slot) * sizeof(*r));
            r[slot++] = chosen;
            break;
        }
    }
}

/* Identifiers keep their case; paths keep separators and percent encoding.
 * UTF-8 bytes stay together, so non-English words do not become unrelated
 * ASCII fragments. This is lexical retrieval, not translation. */
static bool term_byte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c >= 128 || c == '_' || c == '/' ||
           c == '.' || c == '-' || c == '%';
}

static size_t split_terms(const char *task, char *buf, size_t bufsz, const char **terms,
                          size_t max_terms, bool *truncated) {
    size_t n = 0, used = 0;
    const char *p = task;
    *truncated = false;
    while (*p != '\0') {
        while (*p != '\0' && !term_byte((unsigned char)*p)) p++;
        const char *start = p;
        while (*p != '\0' && term_byte((unsigned char)*p)) p++;
        size_t len = (size_t)(p - start);
        while (len > 0 && start[len - 1] == '.') len--;
        if (len < 3) continue;
        if (n == max_terms || used + len + 1 > bufsz) {
            *truncated = true;
            break;
        }
        memcpy(buf + used, start, len);
        buf[used + len] = '\0';
        bool duplicate = false;
        for (size_t i = 0; i < n; i++) {
            if (strcmp(terms[i], buf + used) == 0) duplicate = true;
        }
        if (!duplicate) {
            terms[n++] = buf + used;
            used += len + 1;
        }
    }
    return n;
}

static bool generic_term(const char *term) {
    static const char *const words[] = {
        "the", "and", "for", "with", "from", "this", "that", "into", "how",
        "find", "inspect", "improve", "change", "review", "explain", "task",
        "function", "functions", "code", "please", "fix", "add", "update"
    };
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++) {
        size_t n = strlen(term);
        if (n != strlen(words[i])) continue;
        bool equal = true;
        for (size_t k = 0; k < n; k++) {
            char c = term[k];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != words[i][k]) equal = false;
        }
        if (equal) return true;
    }
    return false;
}

typedef struct context_seed {
    char usr[ATLAS_SEM_MAX_USR_BYTES];
    char name[ATLAS_SEM_MAX_NAME_BYTES];
    char file[512];
    bool lexical;
} context_seed;

typedef struct context_seeds {
    context_seed *items;
    size_t count;
    bool lexical, truncated;
} context_seeds;

static atlas_status take_context_seed(const atlas_sem_symbol_row *row, void *ud,
                                      atlas_err *err) {
    (void)err;
    context_seeds *s = ud;
    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->items[i].usr, row->usr) == 0) {
            if (!s->lexical) s->items[i].lexical = false;
            return ATLAS_OK;
        }
    }
    if (s->count == ATLAS_SEM_CONTEXT_MAX_SEEDS) {
        s->truncated = true;
        return ATLAS_OK;
    }
    context_seed *it = &s->items[s->count++];
    (void)snprintf(it->usr, sizeof it->usr, "%s", row->usr);
    (void)snprintf(it->name, sizeof it->name, "%s", row->name);
    (void)snprintf(it->file, sizeof it->file, "%s", row->file_text);
    it->lexical = s->lexical;
    return ATLAS_OK;
}

/* Core validation also covers direct service callers, not only JSON adapters. */
static atlas_status validate_seed_list(const char *p, size_t len, bool paths, atlas_err *err) {
    if (len == 0) return ATLAS_OK;
    if (p == NULL || len > ATLAS_SEM_CONTEXT_MAX_SEEDS * (size_t)ATLAS_SEM_MAX_NAME_BYTES) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "invalid context seed list");
    }
    size_t offset = 0, count = 0;
    while (offset < len) {
        const char *end = memchr(p + offset, '\0', len - offset);
        size_t n = end != NULL ? (size_t)(end - (p + offset)) : 0;
        if (end == NULL || n == 0 || n >= (paths ? 512u : ATLAS_SEM_MAX_NAME_BYTES) ||
            ++count > ATLAS_SEM_CONTEXT_MAX_SEEDS) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "context seeds exceed their count or length bound");
        }
        if (paths) {
            atlas_buf raw = ATLAS_BUF_INIT;
            atlas_status st = atlas_path_text_decode(p + offset, n, &raw, err);
            if (st == ATLAS_OK) st = atlas_path_check_relative(raw.data, raw.len, err);
            atlas_buf_free(&raw);
            if (st != ATLAS_OK) return st;
        }
        offset += n + 1;
    }
    return ATLAS_OK;
}

static atlas_status find_context_seed(atlas_db *db, int64_t gen, const char *term,
                                      bool path, bool partial, context_seeds *seeds,
                                      atlas_err *err) {
    int64_t n = 0;
    bool more = false;
    seeds->lexical = partial;
    atlas_status st;
    if (path) {
        st = atlas_db_sem_symbols_in_file(db, gen, term, ATLAS_SEM_MAX_ROWS,
                                          take_context_seed, seeds, &n, &more, err);
    } else if (partial) {
        st = atlas_db_sem_symbols_matching(db, gen, term, ATLAS_SEM_MAX_ROWS,
                                           take_context_seed, seeds, &n, &more, err);
    } else {
        st = atlas_db_sem_symbols_by_name(db, gen, term, NULL, NULL, ATLAS_SEM_MAX_ROWS,
                                          take_context_seed, seeds, &n, &more, err);
    }
    seeds->truncated = seeds->truncated || more;
    return st;
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

    out->budget_bytes = ATLAS_SEM_CONTEXT_DEFAULT_BYTES;
    if (req->max_bytes > 0) {
        out->budget_bytes = req->max_bytes;
    } else if (req->max_tokens > 0) {
        /* Saturate before multiplying: a direct caller can supply INT64_MAX. */
        out->budget_bytes = req->max_tokens >
                                ATLAS_SEM_CONTEXT_MAX_BYTES / ATLAS_SEM_BYTES_PER_TOKEN
                                ? ATLAS_SEM_CONTEXT_MAX_BYTES
                                : req->max_tokens * ATLAS_SEM_BYTES_PER_TOKEN;
    }
    if (out->budget_bytes > ATLAS_SEM_CONTEXT_MAX_BYTES) {
        out->budget_bytes = ATLAS_SEM_CONTEXT_MAX_BYTES;
    }
    int64_t max_items = req->max_items > 0 ? req->max_items : ATLAS_SEM_CONTEXT_DEFAULT_ITEMS;
    if (max_items > ATLAS_SEM_CONTEXT_MAX_ITEMS) {
        max_items = ATLAS_SEM_CONTEXT_MAX_ITEMS;
    }

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
        atlas_sem_trust_init(&out->trust);
        out->trust.libclang_available = atlas_sem_available();
    } else {
        atlas_sem_trust_now(db, &out->repo, &out->generation, true, false, &out->trust);
        out->freshness = out->trust.freshness;
        out->stale_reason = out->trust.stale_reason;
        if (out->freshness == ATLAS_SEM_FRESH_STALE) {
            note_missing(out, ATLAS_SEM_MISSING_STALE);
        }
        /* A9.2.5. The package states its coverage gaps in the same place it
         * states its budget gaps, because a reader who cannot see them will read
         * the space where an item would have been as evidence that no such code
         * exists. Two notes, not one: an uncovered source and an unfindable
         * compilation database are different holes with different remedies. */
        if (out->trust.scope_uncovered > 0 ||
            out->trust.scope_discovery != ATLAS_SEM_SCOPE_DECLARED ||
            !out->trust.units_complete) {
            note_missing(out, ATLAS_SEM_MISSING_COVERAGE);
        }
        if (out->trust.generation_discovery != ATLAS_SEM_DISC_COMPLETE) {
            note_missing(out, ATLAS_SEM_MISSING_DISCOVERY);
        }
    }

    st = validate_seed_list(req->paths, req->paths_len, true, err);
    if (st == ATLAS_OK) st = validate_seed_list(req->symbols, req->symbols_len, false, err);
    if (st != ATLAS_OK) return st;
    char termbuf[ATLAS_SEM_CONTEXT_MAX_TASK_BYTES + 1];
    const char *terms[ATLAS_SEM_CONTEXT_MAX_TERMS];
    bool term_limit = false;
    size_t nterms = split_terms(req->task, termbuf, sizeof termbuf, terms,
                                sizeof terms / sizeof terms[0], &term_limit);
    if (term_limit) note_missing(out, ATLAS_SEM_MISSING_SEARCH);

    atlas_sem_item *items = NULL;
    size_t count = 0, cap = 0;
    item_list list = {&items, &count, &cap};
    int64_t gen = out->generation.id;
    int64_t depth = req->depth > 0 ? req->depth : 2;
    context_seeds seeds = {0};
    seeds.items = calloc(ATLAS_SEM_CONTEXT_MAX_SEEDS, sizeof(*seeds.items));
    if (seeds.items == NULL) return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory selecting seeds");

    const char *packed[] = {req->paths, req->symbols};
    size_t lengths[] = {req->paths_len, req->symbols_len};
    for (size_t k = 0; found && st == ATLAS_OK && k < 2; k++) {
        for (size_t off = 0; off < lengths[k] && st == ATLAS_OK; off += strlen(packed[k] + off) + 1) {
            st = find_context_seed(db, gen, packed[k] + off, k == 0, false, &seeds, err);
        }
    }
    /* Explicit scope wins. Within prose, paths and exact identifiers precede
     * lexical candidates; a partial match can never crowd out an exact one. */
    if (found && req->paths_len == 0 && req->symbols_len == 0) {
        for (size_t t = 0; t < nterms && st == ATLAS_OK; t++) {
            bool path = strchr(terms[t], '/') != NULL || strchr(terms[t], '.') != NULL;
            if (nterms > 1 && !path && generic_term(terms[t])) continue;
            st = find_context_seed(db, gen, terms[t], path, false, &seeds, err);
        }
        if (seeds.count == 0) {
            for (size_t t = 0; t < nterms && st == ATLAS_OK; t++) {
                if (strchr(terms[t], '/') || strchr(terms[t], '.') || generic_term(terms[t])) continue;
                st = find_context_seed(db, gen, terms[t], false, true, &seeds, err);
            }
        }
    }
    if (seeds.truncated) note_missing(out, ATLAS_SEM_MISSING_SEARCH);
    if (found && seeds.count == 0) note_missing(out, ATLAS_SEM_MISSING_SEEDS);
    for (size_t k = 0; k < seeds.count && st == ATLAS_OK; k++) {
        atlas_sem_impact_report imp;
        atlas_sem_impact_report_init(&imp);
        st = impact_at(db, &out->repo, seeds.items[k].name, seeds.items[k].usr,
                        &out->generation, &out->trust, depth, ATLAS_SEM_MAX_ROWS, &imp, err);
        if (st == ATLAS_OK && (imp.truncated || imp.unresolved_indirect > 0)) {
            note_missing(out, ATLAS_SEM_MISSING_GRAPH);
        }
        for (size_t i = 0; i < imp.count && st == ATLAS_OK; i++) {
            atlas_sem_evidence ev = ATLAS_SEM_EV_UNKNOWN;
            (void)atlas_sem_evidence_parse(imp.items[i].evidence, &ev);
            const char *why = imp.items[i].why;
            if (seeds.items[k].lexical && strcmp(why, ATLAS_SEM_SEL_SUBJECT) == 0) {
                why = ATLAS_SEM_SEL_TASK_MATCH;
            }
            size_t before = count;
            st = item_add(&list, imp.items[i].kind, imp.items[i].name, imp.items[i].file_text,
                          imp.items[i].line, ev, why, imp.items[i].depth, err);
            if (st == ATLAS_OK && count > before && imp.items[i].test_classification[0] != '\0') {
                items[before] = imp.items[i];
                items[before].why = why;
            }
        }
        atlas_sem_impact_report_free(&imp);
    }
    for (size_t off = 0; st == ATLAS_OK && off < req->paths_len; off += strlen(req->paths + off) + 1) {
        st = context_scope_add(db, req->paths + off, out, err);
        if (st == ATLAS_OK) {
            recorded_test_sink sink = {&list, req->paths + off, false};
            bool more = false;
            st = atlas_db_verify_tests_for_scope(db, out->repo.id, req->paths + off, "",
                    ATLAS_SEM_CONTEXT_TEST_RECORDS, take_recorded_test, &sink, &more, err);
            if (more || sink.omitted) note_missing(out, ATLAS_SEM_MISSING_TESTS);
        }
    }
    if (!found || seeds.count == 0) {
        for (size_t off = 0; st == ATLAS_OK && off < req->symbols_len; off += strlen(req->symbols + off) + 1) {
            recorded_test_sink sink = {&list, "", false};
            bool more = false;
            st = atlas_db_verify_tests_for_scope(db, out->repo.id, "", req->symbols + off,
                    ATLAS_SEM_CONTEXT_TEST_RECORDS, take_recorded_test, &sink, &more, err);
            if (more || sink.omitted) note_missing(out, ATLAS_SEM_MISSING_TESTS);
        }
    }
    for (size_t t = 0; st == ATLAS_OK && t < nterms; t++) {
        if (strchr(terms[t], '/') || strchr(terms[t], '.')) st = context_scope_add(db, terms[t], out, err);
    }
    for (size_t k = 0; st == ATLAS_OK && k < seeds.count; k++) {
        st = context_scope_add(db, seeds.items[k].file, out, err);
    }
    free(seeds.items);

    if (st == ATLAS_OK) {
        st = add_knowledge(db, req, out, &list, &count, err);
    }

    note_missing(out, ATLAS_SEM_MISSING_TESTS);

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
        diversify(r, count);

        for (size_t i = 0; i < count; i++) {
            if ((int64_t)out->count >= max_items) {
                out->budget_reached = true;
                note_missing(out, ATLAS_SEM_MISSING_ITEMS);
                break;
            }
            /* Each item costs what it will occupy: its name, its path and the
             * fixed labels around it. Counted rather than estimated, because a
             * budget that is not enforced is not a budget. */
            int64_t cost = (int64_t)(strlen(r[i].it->name) + strlen(r[i].it->file_text) +
                                     strlen(r[i].it->evidence) +
                                     (r[i].it->why != NULL ? strlen(r[i].it->why) : 0) +
                                     strlen(r[i].it->test_classification) + strlen(r[i].it->test_suite) +
                                     strlen(r[i].it->test_target) + strlen(r[i].it->test_result) +
                                     strlen(r[i].it->test_commit) + strlen(r[i].it->test_evidence_uid) + 24);
            if (out->used_bytes + cost > out->budget_bytes) {
                out->budget_reached = true;
                note_missing(out, ATLAS_SEM_MISSING_BUDGET);
                continue;
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
    /* Settled last, once the item list and the budget flag are final.
     *
     * A package is a read rather than a search for one thing, so what its
     * verdict says is what a *negative* reading of it would be worth: a gap in a
     * package built from an incomplete generation is not evidence that no such
     * code exists. A budget that was reached truncates it exactly as a walk
     * bound truncates a graph query, and settles the same way. */
    if (st == ATLAS_OK) {
        /* **The row count that settles this is the *semantic* one.**
         *
         * A context package mixes compiler-derived items with recorded knowledge,
         * and a repository that has never been indexed can still contribute a
         * decision document. Counting those as "rows found" made a package with
         * no semantic generation at all settle PRESENT — a verdict about a
         * semantic read, asserted from rows that are not semantic. `PRESENT`
         * means "Atlas found a compiler-derived item"; anything else settles on
         * the trust facts, which for an unindexed repository say NO_GENERATION. */
        int64_t sem_items = 0;
        for (size_t i = 0; i < out->count; i++) {
            if (strcmp(out->items[i].kind, "decision") != 0 && strcmp(out->items[i].kind, "test") != 0) {
                sem_items++;
            }
        }
        bool incomplete = out->budget_reached;
        for (size_t i = 0; i < out->missing_count; i++) {
            if (strcmp(out->missing[i], ATLAS_SEM_MISSING_SEARCH) == 0 ||
                strcmp(out->missing[i], ATLAS_SEM_MISSING_GRAPH) == 0) incomplete = true;
        }
        atlas_sem_trust_settle(&out->trust, sem_items, incomplete);
        context_advice(out);
    }
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
