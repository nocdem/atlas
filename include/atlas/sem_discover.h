/* Atlas - A9.2.4: finding a repository's compilation databases, and knowing
 * whether that search was complete.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * ## The sentence this file exists for
 *
 *   **COMPLETE PROCESSING OF CONFIGURED INPUTS DOES NOT PROVE COMPLETE
 *   DISCOVERY OF RELEVANT INPUTS.**
 *
 * A9.2.3 gave a generation a coverage manifest, so `416/416 units complete` was
 * finally reported beside `369 of 761 sources covered` instead of being read as
 * a coverage claim. It could not ask the question underneath: *were those the
 * right two compilation databases, and were there only two?* Nothing in Atlas
 * could answer that, because A9.2.3's rule was that compilation databases are
 * **named, never discovered** — so the answer was always whatever an operator
 * had typed, and the season's own acceptance repository had typed two of three.
 *
 * ## This reverses "named, never discovered", and the bound replaces the refusal
 *
 * A9.2.3 declined to search on the grounds that Atlas must not go looking
 * through a repository for a file that will tell it how to compile things. The
 * reasoning was about *unbounded* search, and it was right about that: a walk
 * with no ceiling, that follows symlinks, that ingests anything named
 * `compile_commands.json` wherever it leads, is a repository telling Atlas what
 * to read. What replaces the refusal is a **bounded search universe**:
 *
 *   - the repository root subtree and nothing above or beside it;
 *   - never `.git`, and never a symlinked component — every descent is `openat`
 *     with `O_NOFOLLOW` from a descriptor validated once, which is A8's rule
 *     about workspace paths applied to a read;
 *   - minus the operator's declared exclusions, which are *shown*, because an
 *     exclusion nobody can see is a hole in the universe nobody can see;
 *   - within the ceilings in `limits.h`, every one of which is reported when it
 *     is reached.
 *
 * Atlas says COMPLETE about that universe and about nothing wider, and the
 * universe is reported beside the verdict. The epistemic rule is A9.2.2's,
 * unchanged and applied one layer down:
 *
 *   **DID NOT DISCOVER is not PROVEN NOT TO EXIST.**
 *
 * ## Why a pinned list is not a completeness claim
 *
 * MANUAL mode leaves discovery UNKNOWN even though the operator named an exact
 * set. That looks harsh and is the one lesson this season had to buy: the
 * repository that exposed the problem had a hand-written list of two databases,
 * the list was wrong, and no flag the operator could have ticked would have made
 * it right — because the operator did not know either. An assertion of
 * completeness by somebody who has not looked is not evidence of completeness.
 * If a later phase wants a declared-complete mode, it needs an argument that
 * survives that case, and "the operator promised" is not one.
 *
 * ## Where the walk runs, and where it must not
 *
 * `atlas_sem_plan_for` runs on every status read, every verification and every
 * sweep tick. A directory walk cannot live there. Membership of the accepted set
 * is *persisted* and refreshed on `ATLAS_SEM_DISCOVERY_INTERVAL_MS`, on a
 * configuration write, and at the start of an index attempt; the *content* of
 * each accepted database is digested live on every identity computation, which
 * is a handful of files. So an edited database moves the source identity at
 * once, a deleted one moves it at once, and a newly created one is noticed at
 * the next discovery pass. That is convergence, not correctness — the same
 * shape as the semantic sweep holding while the file index catches up.
 */
#ifndef ATLAS_SEM_DISCOVER_H
#define ATLAS_SEM_DISCOVER_H

#include <stdbool.h>
#include <stdint.h>

#include "atlas/buf.h"
#include "atlas/db.h"
#include "atlas/error.h"
#include "atlas/limits.h"
#include "atlas/sem.h"

/* How a candidate came to be considered. Reported, because "you asked for this
 * one" and "Atlas found this one" are different facts about the same path and an
 * operator debugging a build description needs to tell them apart. */
typedef enum atlas_sem_input_origin {
    ATLAS_SEM_INPUT_UNKNOWN = 0,
    /* Named in the build description by an operator. */
    ATLAS_SEM_INPUT_PINNED,
    /* Found by the walk. */
    ATLAS_SEM_INPUT_DISCOVERED,
    /* Both: pinned by an operator and independently found. */
    ATLAS_SEM_INPUT_BOTH
} atlas_sem_input_origin;

const char *atlas_sem_input_origin_name(atlas_sem_input_origin o);
bool atlas_sem_input_origin_parse(const char *name, atlas_sem_input_origin *out);

/* Why a candidate was not accepted. Fixed Atlas strings, never assembled from
 * repository bytes or from a parser's message — the discipline every
 * `ATLAS_SEM_*` reason vocabulary follows, and it matters more here than
 * anywhere else in the layer because these strings describe files whose names a
 * repository chose. */
#define ATLAS_SEM_REJECT_EXCLUDED "an_operator_excluded_this_path_from_discovery"
#define ATLAS_SEM_REJECT_NOT_REGULAR "not_a_readable_regular_file_inside_the_repository"
#define ATLAS_SEM_REJECT_SYMLINK "a_symlinked_path_is_never_followed"
#define ATLAS_SEM_REJECT_TOO_LARGE "the_compilation_database_exceeds_the_size_bound"
#define ATLAS_SEM_REJECT_MALFORMED "the_compilation_database_could_not_be_parsed"
#define ATLAS_SEM_REJECT_DUPLICATE "the_same_file_was_already_accepted_under_another_path"
#define ATLAS_SEM_REJECT_TOO_MANY "more_compilation_databases_were_found_than_one_generation_may_hold"

bool atlas_sem_reject_reason_is_known(const char *reason);

/* --- A9.2.5: why the search fell short, and exactly where --------------------
 *
 * A9.2.4 recorded one reason, pathless, and only the first one. The header
 * defended withholding the path on the grounds that "a path is bytes a
 * repository chose" — but `atlas_sem_input.path` above already carries a
 * repository-chosen path, `%XX`-encoded like every other path Atlas reports, and
 * `encode_rel` is called a dozen lines from where the reason was recorded. The
 * layer had already solved the problem for candidate *files* and declined to
 * apply the solution to *directories*.
 *
 * The cost of that was measured rather than imagined. On the repository that
 * produced this season, `discovery_limit` read `an operator excluded a subtree
 * from the search` — true, and it was the *first* obstacle, so anything else the
 * walk could not enter was invisible for ever. "Something was missed" without
 * "what" is not something an operator can act on, and a hole nobody can see is
 * exactly what A9.2.4 exists to end.
 *
 * These are fixed Atlas strings and the classes are kept apart deliberately: an
 * operator's own exclusion, a directory the daemon's uid cannot enter, a
 * directory that could not be read to the end, and a ceiling are four different
 * situations with four different remedies, and folding them into "PARTIAL" is
 * the conflation this season removes. */
#define ATLAS_SEM_OBSTACLE_EXCLUDED "an_operator_excluded_this_subtree_from_the_search"
#define ATLAS_SEM_OBSTACLE_UNREADABLE_DIR "this_directory_could_not_be_entered"
#define ATLAS_SEM_OBSTACLE_UNREADABLE_ENTRIES "this_directory_could_not_be_read_to_the_end"
#define ATLAS_SEM_OBSTACLE_DEPTH "the_walk_reached_its_depth_ceiling_below_this_directory"
#define ATLAS_SEM_OBSTACLE_ENTRIES "the_walk_reached_its_entry_ceiling_at_this_directory"
#define ATLAS_SEM_OBSTACLE_PATH_TOO_LONG "a_path_below_this_directory_exceeded_the_path_ceiling"
#define ATLAS_SEM_OBSTACLE_UNREPRESENTABLE "a_path_below_this_directory_could_not_be_represented"
#define ATLAS_SEM_OBSTACLE_CANDIDATES                                                              \
    "more_compilation_databases_were_found_than_one_walk_may_hold"
#define ATLAS_SEM_OBSTACLE_MEMORY "there_was_not_enough_memory_to_walk_this_directory"

bool atlas_sem_obstacle_reason_is_known(const char *reason);
/* Atlas' own copy of a known reason, or NULL. A value that arrived over a socket
 * is a matching string, not Atlas' string. */
const char *atlas_sem_obstacle_intern(const char *reason);

/* One place the search could not account for.
 *
 * `path` is repository-relative and `%XX`-encoded, exactly as
 * `atlas_sem_input.path` is, and it is the directory the obstacle is *about*:
 * for a subtree that could not be entered, that subtree; for a ceiling, the
 * directory the walk was in when it was reached. `reason` is one of the fixed
 * strings above and is never concatenated with the path — a reason an operator
 * reads must stay a value Atlas owns. */
typedef struct atlas_sem_obstacle {
    char path[ATLAS_SEM_MAX_PATH_BYTES];
    char reason[96];
} atlas_sem_obstacle;
/* Atlas' own copy of a known reason, or NULL. A value that arrived over a socket
 * is a *matching* string, not Atlas' string — `atlas_sem_hold_intern`'s rule. */
const char *atlas_sem_reject_intern(const char *reason);

/* One candidate compilation database. */
typedef struct atlas_sem_input {
    /* Repository-relative, canonical, `%XX`-encoded for display exactly as every
     * other path Atlas reports. */
    char path[ATLAS_SEM_MAX_PATH_BYTES];
    atlas_sem_input_origin origin;
    bool accepted;
    /* An ATLAS_SEM_REJECT_* string when `accepted` is false, "" otherwise. */
    char reject_reason[96];
    /* SHA-256 of the document bytes, or "" when it could not be read. */
    char digest[65];
    /* Translation units the document named. Zero for a rejected candidate, and
     * zero is *not* an error: a compilation database may legitimately be empty,
     * which is a different fact from being unreadable and is reported as one. */
    int64_t unit_count;
} atlas_sem_input;

/* The result of one discovery pass over one repository. */
typedef struct atlas_sem_discovery_result {
    atlas_sem_discovery state;
    atlas_sem_discovery_mode mode;

    atlas_sem_input inputs[ATLAS_SEM_DISCOVERY_MAX_CANDIDATES];
    size_t count;
    size_t accepted_count;

    /* Why the search fell short of its universe, if it did — the first reason,
     * because that is the one to act on.
     *
     * A ceiling is one such reason and not the only one: a directory Atlas could
     * not enter, a directory it could not finish reading, and a subtree an
     * operator excluded all leave exactly as big a hole. A8-CI's rule is that
     * every bound which is reached is reported; this is that rule widened to
     * every reason a walk is PARTIAL, because a hole nobody can see is what this
     * season exists to end. A fixed Atlas string, never a path. */
    bool limit_reached;
    char limit_detail[128];

    /* A9.2.5. Every obstacle, with its path — not merely the first, and not
     * merely a reason. `limit_detail` above is kept as the one-line summary an
     * existing reader already consumes; this is what an operator acts on.
     *
     * Deterministic order: the list is **sorted by path** when the walk finishes
     * (`cmp_obstacle` in `src/sem/discover.c`), because `readdir` order is not
     * guaranteed and a value an operator compares between runs must not depend
     * on the filesystem's iteration order. Entries are also deduplicated on
     * `(path, reason)`, so one directory cannot fill the list with copies of one
     * obstacle and hide every other. `obstacles_truncated` is reported
     * because a list that was trimmed without saying so would recreate exactly
     * the invisible hole this replaces. */
    atlas_sem_obstacle obstacles[ATLAS_SEM_DISCOVERY_MAX_OBSTACLES];
    size_t obstacle_count;
    bool obstacles_truncated;

    /* What the walk actually looked at, so the universe can be reported beside
     * the verdict rather than assumed by a reader. */
    int64_t dirs_visited;
    int64_t entries_seen;
    int64_t excluded_subtrees;

    char discovered_at[ATLAS_TS_MAX];
} atlas_sem_discovery_result;

void atlas_sem_discovery_result_init(atlas_sem_discovery_result *r);

/* Walks one repository for compilation databases.
 *
 * Creates no process, opens no database handle and writes no row — it is a
 * filesystem read and nothing else, so it may be called from anywhere including
 * a status command. A1's rule that no file read happens inside a write
 * transaction is why the caller runs this *before* it opens one.
 *
 * `cfg` supplies the mode, the pinned list and the exclusions. In MANUAL mode no
 * directory is opened at all and the result is the pinned list with state
 * UNKNOWN.
 *
 * A candidate that cannot be read, is a symlink, exceeds the bound or does not
 * parse is *recorded as rejected with a reason* rather than failing the pass:
 * one unreadable file must not make a repository's whole build description
 * disappear, which is precisely how a coverage claim would silently become
 * wrong.
 *
 * `yield`, when supplied, is called every `ATLAS_SEM_DISCOVER_YIELD_EVERY`
 * directory entries so the thread running the walk can be lent to something
 * short. It changes nothing this function produces — the candidate list, the
 * obstacle list and the verdict are identical whether it is supplied or not —
 * and it is safe here for the reason the walk itself is: no transaction is open
 * anywhere in this file, because none is ever opened in it. NULL is the ordinary
 * case; a status command has nothing to lend its thread to. */
atlas_status atlas_sem_discover(const char *root, const atlas_sem_config *cfg,
                                void (*yield)(void *ud), void *yield_ud,
                                atlas_sem_discovery_result *out, atlas_err *err);

/* The digest over the *input universe*, and where it is computed.
 *
 * There is exactly **one** implementation of it, in `src/sem/index.c`, over the
 * persisted candidate list — see `atlas_sem_repo_discovery_identity` below. An
 * earlier cut had a second one here, over a fresh walk result, and two
 * implementations of a value that decides whether a repository is rebuilt is how
 * a stored identity becomes incomparable with a live one.
 *
 * What it covers, domain-separated and length-prefixed for A4's reason: the
 * discovery state, the exclusion list, and every accepted input's path and
 * content digest in path order.
 *
 * The state is included deliberately. A PARTIAL walk that later completes with
 * an identical accepted set still moves the identity and still rebuilds — which
 * is the only way a *sealed* manifest can be upgraded, and it is cheap because
 * every unit's input digest is unchanged and every unit is reused. */
#define ATLAS_SEM_DISCOVERY_DOMAIN "atlas.sem.discovery.v1"

/* --- the durable half, implemented in `src/sem/index.c` ----------------------
 *
 * Declared here so a caller has one header for the subject, and implemented
 * beside the indexer because that is where the database and the writer already
 * are. `atlas_sem_discover` above stays free of both. */

/* Walks a repository and records what it found, in one transaction.
 *
 * The delete and the inserts are one fact — a candidate list half replaced is a
 * search nobody performed — so the transaction is owned here rather than left to
 * a caller who might reasonably forget. The walk runs to completion *before* the
 * transaction opens, because A1 forbids a file read inside one.
 *
 * A root Atlas cannot open leaves the stored verdict alone rather than blanking
 * it: "I could not look this time" is not evidence that what was found last time
 * is wrong.
 *
 * `yield` is carried straight through to the walk and is never called once the
 * transaction is open — which is the same statement as "the walk runs to
 * completion before the transaction opens", said from the other side. */
atlas_status atlas_sem_discovery_run(atlas_db *db, atlas_repo_info *repo,
                                     void (*yield)(void *ud), void *yield_ud,
                                     atlas_sem_discovery_result *out, atlas_err *err);

/* The accepted set as the indexer takes it: NUL-separated, repository-relative,
 * in path order.
 *
 * Read from the persisted candidate list rather than from the operator's pinned
 * list, which is the whole difference between A9.2.3 and this season: what gets
 * indexed is what discovery accepted, and a pinned path is one input to that
 * rather than the entirety of it. */
atlas_status atlas_sem_accepted_inputs(atlas_db *db, int64_t repo_id, atlas_buf *out,
                                       size_t *count_out, atlas_err *err);

#endif /* ATLAS_SEM_DISCOVER_H */
