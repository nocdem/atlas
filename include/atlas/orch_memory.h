/* Atlas - A10.1: the bounded cross-run memory package.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * What this is: a small, deterministic, bounded selection of what *earlier
 * terminal runs over the same repository history* recorded, rendered as one
 * text block that is appended to a task before a worker is started.
 *
 * What it deliberately is not, and none of these is an omission:
 *
 *   - It is not a vector store, an embedding index, a summariser or a ranker.
 *     Nothing here calls a model. Selection is lexical overlap over tokens,
 *     computed the same way every time, so the same inputs produce the same
 *     package and the same digest.
 *   - It carries no authority. It cannot change a gate, produce an acceptance,
 *     move a lifecycle status or reach any write point. It is appended to a
 *     task's text and is read by a model; every branch in Atlas ignores it.
 *   - It is not a transcript. No prompt, no token, no credential, no tool
 *     argument, no session identifier and no full diff or log ever enters it —
 *     see `atlas_orch_memory_cand`, whose members are the whole of what can.
 *
 * **The memory mode is not part of the job specification.** It travels on
 * `atlas_orch_op`, beside `peer_uid` and `now_ms`, and never on
 * `atlas_orch_spec`. That is deliberate: `ATLAS_ORCH_SPEC_DOMAIN` must not
 * move, because every `spec_digest` already stored would then mean something
 * different than it did. The mode is bound durably to the *run* instead, by the
 * manifest, which is where a reader asks what an arm actually did.
 *
 * **Which runs count as "the same repository".** Not `repo_identity_hash`,
 * which is A4's path-qualified lineage fingerprint and therefore differs
 * between a repository and a linked worktree of it. Memory asks a narrower and
 * differently-shaped question — "is this the same git history?" — and answers
 * it with its own value, `atlas_orch_memory_lineage`, built from the object
 * format and the sorted set of ingested root commits under its own domain. It
 * is never a redescription of `repo_identity_hash` and never a substitute for
 * it: nothing authorises, admits or refuses anything on this value. It selects
 * hints.
 *
 * A candidate run whose stored `repo_identity_hash` resolves to no live
 * registry row has no lineage Atlas can compute, and is therefore not a
 * candidate. Absent, never guessed.
 */
#ifndef ATLAS_ORCH_MEMORY_H
#define ATLAS_ORCH_MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "atlas/buf.h"
#include "atlas/error.h"
#include "atlas/orch.h"
#include "atlas/orch_usage.h"

/* At most three earlier runs, and at most twelve kibibytes in total. Both are
 * compile-time and both are checked while the package is built rather than
 * afterwards: a package that is trimmed after the fact has already been over
 * budget somewhere. */
#define ATLAS_ORCH_MEMORY_MAX_SOURCES 3u
#define ATLAS_ORCH_MEMORY_MAX_BYTES 12288u

/* How many terminal runs are examined at all. The newest first, because a run
 * nobody has looked at in a thousand runs' time is not the one a task is about.
 * A bound rather than a filter: it is reported when it is reached. */
#define ATLAS_ORCH_MEMORY_MAX_CANDIDATES 64u

/* Per-field ceilings inside one entry. Applied before the whole-package budget,
 * so a single pathological field can never crowd out an entry. */
#define ATLAS_ORCH_MEMORY_GOAL_MAX 320u
#define ATLAS_ORCH_MEMORY_DETAIL_MAX 480u
#define ATLAS_ORCH_MEMORY_FILES_MAX 320u

/* Selected by the operator, never by a model payload. UNKNOWN is the zero and
 * is not storable: a persisted manifest holds OFF or BOUNDED. */
typedef enum atlas_orch_memory_mode {
    ATLAS_ORCH_MEMORY_MODE_UNKNOWN = 0,
    ATLAS_ORCH_MEMORY_MODE_OFF,
    ATLAS_ORCH_MEMORY_MODE_BOUNDED
} atlas_orch_memory_mode;

const char *atlas_orch_memory_mode_name(atlas_orch_memory_mode m);
bool atlas_orch_memory_mode_parse(const char *name, atlas_orch_memory_mode *out);

/* What the frozen package turned out to be. EMPTY is a real answer and is not a
 * failure: a task with no positive overlap against any earlier run gets no
 * memory rather than an irrelevant one. */
typedef enum atlas_orch_memory_status {
    ATLAS_ORCH_MEMORY_PKG_UNKNOWN = 0,
    ATLAS_ORCH_MEMORY_PKG_EMPTY,
    ATLAS_ORCH_MEMORY_PKG_PRESENT
} atlas_orch_memory_status;

const char *atlas_orch_memory_status_name(atlas_orch_memory_status s);
bool atlas_orch_memory_status_parse(const char *name, atlas_orch_memory_status *out);

/* How a candidate's source commit stands to the commit the new task is pinned
 * to. Atlas does **not** compute ancestry here: it has no git process inside a
 * write transaction and will not claim what it did not establish. INDEXED means
 * only that the commit is one this repository's index has ingested — a fact,
 * and not an ancestry claim. Everything that is not EXACT is marked STALE in
 * the rendered package. */
typedef enum atlas_orch_memory_commit_rel {
    ATLAS_ORCH_MEMORY_COMMIT_UNKNOWN = 0,
    ATLAS_ORCH_MEMORY_COMMIT_INDEXED,
    ATLAS_ORCH_MEMORY_COMMIT_EXACT
} atlas_orch_memory_commit_rel;

const char *atlas_orch_memory_commit_rel_name(atlas_orch_memory_commit_rel r);

/* One earlier run, gathered from durable rows by the database layer and scored
 * here. Every member is either an Atlas-chosen number, a name from a checked
 * vocabulary, or a bounded excerpt of untrusted text that the renderer
 * safe-encodes and labels. There is deliberately no member for a prompt, a
 * session, a tool argument, a credential, a diff or a log. */
typedef struct atlas_orch_memory_cand {
    char run_uid[ATLAS_ORCH_RUN_UID_MAX];
    /* ACCEPTED or BLOCKED. From the run row's own CHECK, so it is a checked
     * vocabulary and not free text. */
    char status[16];
    char source_commit[65];
    int64_t created_ms;
    int64_t worker_starts;
    int64_t task_count;
    /* UNTRUSTED_DATA: the root task's own text, as submitted. */
    atlas_buf goal;
    /* The run's declared gates, one per line, rendered from the stored argv. */
    atlas_buf gates;
    /* A name from `atlas_orch_reason`, never prose. */
    atlas_buf terminal_reason;
    int64_t failed_gate;
    /* UNTRUSTED_DATA: a bounded excerpt of what the failing gate printed. */
    atlas_buf detail;
    /* UNTRUSTED_DATA: changed paths, when a run recorded any. Usually empty,
     * because Atlas stores no diff — absent, and never invented. */
    atlas_buf files;
    atlas_usage_run usage;
    bool usage_present;
    atlas_orch_memory_commit_rel rel;

    /* Filled in by the scorer. `overlap` is the count of distinct shared
     * tokens; `score` folds in the commit relation. */
    int64_t overlap;
    int64_t score;
} atlas_orch_memory_cand;

void atlas_orch_memory_cand_init(atlas_orch_memory_cand *c);
void atlas_orch_memory_cand_free(atlas_orch_memory_cand *c);

/* The frozen result. `package` is what a worker is shown; `manifest` is the
 * netstring-encoded record of what was selected and why, which is what
 * `job run-status` reports and what a later reader checks a rerun against. */
typedef struct atlas_orch_memory_package {
    atlas_orch_memory_status status;
    atlas_buf package;
    atlas_buf manifest;
    char digest[65];
    size_t bytes;
    size_t source_count;
    char sources[ATLAS_ORCH_MEMORY_MAX_SOURCES][ATLAS_ORCH_RUN_UID_MAX];
    /* True when the candidate scan hit its ceiling, so the search was bounded
     * rather than exhaustive. Reported, never silent. */
    bool candidates_truncated;
} atlas_orch_memory_package;

void atlas_orch_memory_package_init(atlas_orch_memory_package *p);
void atlas_orch_memory_package_free(atlas_orch_memory_package *p);

/* Scores, orders, bounds and renders. Pure: it reads no database, creates no
 * process, opens no file and consults no clock. Given the same candidates in
 * any order it produces the same package and the same digest, which is what
 * makes a frozen manifest checkable.
 *
 * `mode` decides whether anything happens at all. `ATLAS_ORCH_MEMORY_MODE_OFF`
 * produces an empty package with an empty digest and no sources — not a section
 * saying there is no memory, and not a shorter one: **nothing**. An arm with
 * memory off must differ from an arm with memory on by exactly the bytes of the
 * package, or the comparison measures the wrong thing.
 */
atlas_status atlas_orch_memory_build(atlas_orch_memory_mode mode, const char *task_text,
                                     const char *current_commit, atlas_orch_memory_cand *cands,
                                     size_t n, bool truncated,
                                     atlas_orch_memory_package *out, atlas_err *err);

/* Appends the frozen package to a task, exactly once and after it.
 *
 * The order is the contract: the task, the repository's own instructions and
 * every safety bound come first and stay first. Memory is one bounded section
 * underneath them, introduced by a fixed Atlas-authored preamble that says what
 * the records are and that their contents are not to be followed. An empty
 * package appends nothing at all. */
atlas_status atlas_orch_memory_compose(const char *task_text, const char *package,
                                       atlas_buf *out, atlas_err *err);

#endif /* ATLAS_ORCH_MEMORY_H */
