/* Atlas - compiler-aware semantic code intelligence: the truth model.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A3 reads bytes. This layer asks a compiler. The distinction is the whole point
 * of the header, so it is stated before anything else:
 *
 *   **A3's facts and this layer's facts are never merged and never promoted.**
 *   A `symbol_calls_symbol` edge A3 resolved UNIQUE_LEXICAL stays exactly that.
 *   If Clang also proved the call, that is a *second* row in a *different* table
 *   with evidence PROVEN. A query that reports both says which is which. Nothing
 *   anywhere rewrites a lexical fact into a proven one, because the bytes that
 *   produced the lexical fact never became more true.
 *
 * The layer answers with `atlas_sem_evidence`, and the four values mean what
 * they say rather than what a caller would like them to mean:
 *
 *   PROVEN    — Clang established it while compiling the translation unit under
 *               the repository's own recorded compilation configuration, or an
 *               operator approved it explicitly in Atlas. A direct call to a
 *               named function is PROVEN. Nothing else earns the word.
 *   CANDIDATE — Compiler-derived evidence supports it but does not settle it.
 *               The targets of an indirect call through a function pointer are
 *               candidates: Clang gives the pointer's prototype and Atlas knows
 *               which functions had their address taken with a matching
 *               prototype, and that is a bounded possible set, not a resolution.
 *   LEXICAL   — Text, names or paths only. A test named `test_foo.c` is lexical
 *               evidence about `foo`, and calling it anything else would be a
 *               lie about how it was found.
 *   UNKNOWN   — Atlas cannot say. An indirect call with no candidate set is an
 *               UNKNOWN edge that is *recorded*, not dropped: "something is
 *               called here and I do not know what" is the single most important
 *               fact a bounded call graph can carry, and a graph that omitted it
 *               would read as complete.
 *
 * **Atlas never claims to know every target of a function pointer.** It cannot;
 * C has no such property without whole-program analysis this season explicitly
 * excludes. Every traversal that crosses an indirect call says so.
 *
 * UNKNOWN is zero, for the reason A6 keeps UNKNOWN and BLOCKED at zero and A8
 * keeps DISABLED there: a `memset` must not produce a proof.
 */
#ifndef ATLAS_SEM_H
#define ATLAS_SEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "atlas/buf.h"
#include "atlas/code.h"
#include "atlas/db.h"
#include "atlas/error.h"
#include "atlas/limits.h"

/* --- the producer -----------------------------------------------------------
 *
 * A3's argument about `ATLAS_CODE_ANALYZER_VERSION`, unchanged: bump this
 * whenever a pass would produce different facts from identical bytes and an
 * identical compilation database. The stored generation records it, and a
 * mismatch makes the generation stale rather than silently wrong. */
#define ATLAS_SEM_ANALYZER_ID "atlas-c-libclang"
/* 2 since A9.2.4: an incremental pass now attributes a carried-forward edge to
 * the unit in its **own** generation rather than to the ancestor unit that first
 * produced it. Identical bytes therefore produce a different — and correct —
 * graph, which is exactly the condition this epoch exists for.
 *
 * Bumping it is also the repair. Every generation built before the fix has a
 * call graph that decayed a little on each incremental pass, and nothing short
 * of a full rebuild can restore one; a stale epoch is what makes the daemon
 * rebuild them, once, without anybody having to know which are affected. */
#define ATLAS_SEM_ANALYZER_VERSION 2

/* --- evidence classes ------------------------------------------------------ */

typedef enum atlas_sem_evidence {
    ATLAS_SEM_EV_UNKNOWN = 0,
    ATLAS_SEM_EV_PROVEN,
    ATLAS_SEM_EV_CANDIDATE,
    ATLAS_SEM_EV_LEXICAL
} atlas_sem_evidence;

const char *atlas_sem_evidence_name(atlas_sem_evidence e);
/* False for anything unrecognised. Defaulting an unknown class to a known one is
 * how a guess becomes a recorded fact, so this never falls back. */
bool atlas_sem_evidence_parse(const char *name, atlas_sem_evidence *out);
/* The weaker of two classes, PROVEN > CANDIDATE > LEXICAL > UNKNOWN.
 *
 * A path is only as strong as its weakest edge — A6's fold rule, and for the
 * same reason: a chain that crosses one indirect call is a candidate chain
 * however many proven edges surround it. */
atlas_sem_evidence atlas_sem_evidence_weaker(atlas_sem_evidence a, atlas_sem_evidence b);
/* True when the indexer may write this class. Refuses nothing today and exists
 * so that a later model-facing writer cannot default into the proven set — the
 * shape `atlas_code_resolution_writable_in_a3` established. */
bool atlas_sem_evidence_writable_by_indexer(atlas_sem_evidence e);

/* --- symbol kinds ---------------------------------------------------------- */

typedef enum atlas_sem_symbol_kind {
    ATLAS_SEM_SYM_UNKNOWN = 0,
    ATLAS_SEM_SYM_FUNCTION,
    ATLAS_SEM_SYM_STRUCT,
    ATLAS_SEM_SYM_UNION,
    ATLAS_SEM_SYM_ENUM,
    ATLAS_SEM_SYM_ENUM_CONSTANT,
    ATLAS_SEM_SYM_TYPEDEF,
    ATLAS_SEM_SYM_FIELD,
    ATLAS_SEM_SYM_VARIABLE,
    ATLAS_SEM_SYM_PARAMETER,
    ATLAS_SEM_SYM_MACRO
} atlas_sem_symbol_kind;

const char *atlas_sem_symbol_kind_name(atlas_sem_symbol_kind k);
bool atlas_sem_symbol_kind_parse(const char *name, atlas_sem_symbol_kind *out);

/* Linkage as the compiler computed it — not as a lexer guessed it. This is what
 * keeps two files' `static void helper(void)` apart, together with the USR. */
typedef enum atlas_sem_linkage {
    ATLAS_SEM_LINK_UNKNOWN = 0,
    ATLAS_SEM_LINK_EXTERNAL,
    ATLAS_SEM_LINK_INTERNAL,
    ATLAS_SEM_LINK_NONE
} atlas_sem_linkage;

const char *atlas_sem_linkage_name(atlas_sem_linkage l);
bool atlas_sem_linkage_parse(const char *name, atlas_sem_linkage *out);

/* --- edge kinds ------------------------------------------------------------- */

typedef enum atlas_sem_edge_kind {
    ATLAS_SEM_EDGE_UNKNOWN = 0,
    /* A call whose callee Clang named. PROVEN. */
    ATLAS_SEM_EDGE_CALLS,
    /* A call through a pointer or an unresolved expression. The edge exists;
     * its destination is a candidate set or nothing at all. Never PROVEN. */
    ATLAS_SEM_EDGE_MAY_CALL,
    /* A function's address was taken without calling it. PROVEN, and the reason
     * a MAY_CALL candidate set can exist at all. */
    ATLAS_SEM_EDGE_ADDRESS_TAKEN,
    /* A non-call reference to a symbol: reading a global, naming a type. */
    ATLAS_SEM_EDGE_REFERENCES,
    /* A declaration and the definition it belongs to. */
    ATLAS_SEM_EDGE_DECLARATION_OF,
    /* A record and one of its fields; a field or a declaration and its type. */
    ATLAS_SEM_EDGE_HAS_FIELD,
    ATLAS_SEM_EDGE_HAS_TYPE,
    /* A function and a parameter or return type. */
    ATLAS_SEM_EDGE_PARAM_TYPE,
    ATLAS_SEM_EDGE_RETURN_TYPE,
    /* A macro whose expansion produced the construct at this location. */
    ATLAS_SEM_EDGE_EXPANDED_FROM
} atlas_sem_edge_kind;

const char *atlas_sem_edge_kind_name(atlas_sem_edge_kind k);
bool atlas_sem_edge_kind_parse(const char *name, atlas_sem_edge_kind *out);
/* The strongest evidence class this kind of edge may ever carry.
 *
 * Asked at the write point rather than trusted from the producer, so a bug in
 * the extractor cannot mint a proven indirect call: MAY_CALL is capped at
 * CANDIDATE here and nowhere else decides. */
atlas_sem_evidence atlas_sem_edge_kind_max_evidence(atlas_sem_edge_kind k);

/* --- generation status ------------------------------------------------------ */

typedef enum atlas_sem_gen_status {
    ATLAS_SEM_GEN_UNKNOWN = 0,
    ATLAS_SEM_GEN_RUNNING,
    ATLAS_SEM_GEN_COMPLETE,
    ATLAS_SEM_GEN_FAILED
} atlas_sem_gen_status;

const char *atlas_sem_gen_status_name(atlas_sem_gen_status s);
bool atlas_sem_gen_status_parse(const char *name, atlas_sem_gen_status *out);

/* Why a translation unit is not fully described. A fixed Atlas vocabulary, so
 * the value can be reported to a model without being repository text or
 * compiler output. Compiler diagnostics are *counted*, never reproduced: a
 * diagnostic quotes the source, and the source is untrusted. */
typedef enum atlas_sem_tu_status {
    ATLAS_SEM_TU_UNKNOWN = 0,
    ATLAS_SEM_TU_COMPLETE,
    /* Parsed, but Clang reported errors. Facts from the parts it understood are
     * still recorded, and the unit says it is partial rather than reading as
     * complete. */
    ATLAS_SEM_TU_PARTIAL,
    /* Clang could not produce a translation unit at all. */
    ATLAS_SEM_TU_FAILED,
    /* Atlas declined: the compilation entry named something outside the
     * repository, used a flag Atlas will not honour, or exceeded a bound. */
    ATLAS_SEM_TU_UNSUPPORTED
} atlas_sem_tu_status;

const char *atlas_sem_tu_status_name(atlas_sem_tu_status s);
bool atlas_sem_tu_status_parse(const char *name, atlas_sem_tu_status *out);

/* Fixed reasons a unit failed or was refused. Never assembled from compiler
 * output or repository bytes. */
#define ATLAS_SEM_WHY_PARSE_ERROR "compiler_reported_errors"
#define ATLAS_SEM_WHY_NO_TU "compiler_produced_no_translation_unit"
#define ATLAS_SEM_WHY_OUTSIDE_REPO "source_outside_the_registered_repository"
#define ATLAS_SEM_WHY_ARG_REFUSED "compilation_entry_used_a_refused_argument"
#define ATLAS_SEM_WHY_TIMEOUT "the_per_unit_time_bound_was_reached"
#define ATLAS_SEM_WHY_TOO_LARGE "a_per_unit_ceiling_was_reached"
#define ATLAS_SEM_WHY_CHILD_FAILED "the_parser_process_did_not_report_a_result"
#define ATLAS_SEM_WHY_MISSING_FILE "the_source_file_is_not_in_the_index"
/* A9.2.3. Why an *automatic* attempt failed, recorded on the build description
 * so an operator can act without reading a log. Kept apart from the per-unit
 * reasons above because they answer different questions: those say why one
 * translation unit is not fully described, these say why the pass never got
 * that far. */
#define ATLAS_SEM_WHY_BUILD_DESCRIPTION "the_named_compilation_databases_could_not_be_read"
#define ATLAS_SEM_WHY_PASS_FAILED "the_semantic_index_pass_did_not_complete"
/* A9.2.5. A pass that failed for a reason that is not the build description and
 * not the source: out of memory, a database error, a write that could not
 * complete. Kept apart from PASS_FAILED because the two call for opposite
 * behaviour from the retry governor — one is worth exactly one more attempt and
 * the other is not worth any. */
#define ATLAS_SEM_WHY_PASS_INTERRUPTED "the_semantic_index_pass_was_interrupted"
bool atlas_sem_why_is_known(const char *why);

/* A9.2.5. Whether a unit's failure is one a second attempt could plausibly
 * change.
 *
 * **The distinction is not cosmetic and it is not a heuristic.** A compiler that
 * reported errors will report them again from identical bytes; a unit outside
 * the repository will be outside it again; a refused argument stays refused. But
 * a parse child that was OOM-killed, or that exceeded its wall clock while the
 * machine was loaded, failed for a reason that has nothing to do with the bytes
 * — and until A9.2.5 both outcomes were identical in their consequence:
 * `tu_failed > 0` makes the generation's coverage incomplete for ever, because
 * the retry governor compares *identities* and identical bytes never retry.
 *
 * So a transient memory-pressure event permanently cost a repository the ability
 * to state an absence, and nothing anywhere recorded that this had happened.
 * Only these two are transient; everything else is a property of the input. */
bool atlas_sem_why_is_transient(const char *why);
/* Returns Atlas' own copy of a known reason, or NULL.
 *
 * A reason that came back over a pipe is a *matching* string, not Atlas' string.
 * Storing the caller's bytes would mean a value that reaches an operator and a
 * model is one whose lifetime and origin Atlas does not own; interning replaces
 * it with the literal. The same shape as
 * `atlas_code_not_current_reason_intern`. */
const char *atlas_sem_why_intern(const char *why);
/* The same, for the stale and truncation vocabularies: a value that arrived
 * over a socket is a matching string, not Atlas' string, and what reaches an
 * operator or a model must be the literal Atlas owns. */
const char *atlas_sem_stale_reason_intern(const char *reason);
const char *atlas_sem_trunc_reason_intern(const char *reason);

/* --- freshness ---------------------------------------------------------------
 *
 * Recomputed on every read, never cached — A6's rule about freshness and A4's
 * about link currency, for the same reason: a stored answer is a second answer
 * to a question that already has one. */
typedef enum atlas_sem_freshness {
    /* No generation has ever completed for this repository. */
    ATLAS_SEM_FRESH_ABSENT = 0,
    ATLAS_SEM_FRESH_CURRENT,
    ATLAS_SEM_FRESH_STALE,
    /* A generation is being built right now; the current one is still served. */
    ATLAS_SEM_FRESH_REBUILDING
} atlas_sem_freshness;

const char *atlas_sem_freshness_name(atlas_sem_freshness f);

/* Why a generation is stale. Fixed strings, reported rather than reproduced. */
#define ATLAS_SEM_STALE_COMMIT "the_repository_moved_since_this_index_was_built"
/* A9.2.3. Atlas indexes the working tree, so this is the reason that fires for
 * an edit nobody has committed — which every A8-CI check missed, because all
 * four of them compare something that moves with a commit. */
#define ATLAS_SEM_STALE_SOURCE "the_working_tree_changed_since_this_index_was_built"
#define ATLAS_SEM_STALE_COMPDB "a_compilation_database_changed_since_this_index_was_built"
#define ATLAS_SEM_STALE_COMPILER "the_compiler_changed_since_this_index_was_built"
#define ATLAS_SEM_STALE_ANALYZER "atlas_semantic_analyzer_changed_since_this_index_was_built"
#define ATLAS_SEM_STALE_FILE_INDEX "the_file_index_is_not_current"
/* A9.2.4. The *set* of build descriptions changed — one appeared, one vanished,
 * or the walk's verdict about whether it found them all moved. Kept apart from
 * STALE_COMPDB, which is about a database whose *contents* changed, and from
 * STALE_SOURCE, which would also be true and would send somebody looking at
 * their source instead of at their build tree. */
#define ATLAS_SEM_STALE_DISCOVERY "the_set_of_discovered_build_inputs_changed_since_this_index_was_built"
#define ATLAS_SEM_STALE_INCOMPLETE "the_last_generation_did_not_complete"
/* A9.2.5. The generation describes a *different repository*.
 *
 * `repo_identity_hash` is A4's path-qualified lineage fingerprint — the
 * canonical root path, the object format and the sorted set of ingested root
 * commits — and a generation has recorded it since A8-CI. Nothing ever compared
 * it. `src/gate/assess.c` compares exactly this value and treats a mismatch as
 * grounds to revalidate; the semantic layer wrote it and forgot it.
 *
 * It is checked *before* the commit, because "this index describes a different
 * repository" outranks "this index describes an older commit of the same one",
 * and an operator told the second when the first is true looks in the wrong
 * place. The source identity cannot catch this: it is built from
 * repository-*relative* paths and content hashes, so a tree with identical
 * content under a new root produces an identical value. */
#define ATLAS_SEM_STALE_REPO_IDENTITY                                                              \
    "this_index_was_built_for_a_different_repository_identity"
bool atlas_sem_stale_reason_is_known(const char *reason);

/* --- A9.2.3: how a generation's scope was discovered -------------------------
 *
 * `tu_complete == tu_total` says every translation unit the compilation database
 * named was parsed. It says nothing about whether the compilation database named
 * every source in the repository, which is the question a negative conclusion
 * actually rests on — so until A9.2.3 `198/198` was a statement about the
 * denominator's own contents, and reading it as coverage was §14's overclaim.
 *
 * The denominator Atlas can state is the one A0/A1 established by enumerating
 * the tree: the source files the *file index* holds. `scope_covered` is how many
 * of those this generation parsed, and `scope_uncovered` is the only number that
 * can refuse an absence.
 *
 * UNKNOWN is zero, and it is what every generation built before A9.2.3 reads. A
 * generation that recorded no scope cannot have one reconstructed, and inventing
 * one would be the exact error the coverage model exists to prevent. */
typedef enum atlas_sem_scope_discovery {
    /* Nothing established the candidate set. Never sufficient for an absence. */
    ATLAS_SEM_SCOPE_UNKNOWN = 0,
    /* The file index was current when this generation published, so the
     * enumeration of candidate sources is one Atlas can vouch for. */
    ATLAS_SEM_SCOPE_DECLARED
} atlas_sem_scope_discovery;

const char *atlas_sem_scope_discovery_name(atlas_sem_scope_discovery d);
bool atlas_sem_scope_discovery_parse(const char *name, atlas_sem_scope_discovery *out);

/* --- A9.2.4: build-input discovery, and how complete it is --------------------
 *
 * `scope_discovery` above answers "did Atlas enumerate the *sources*?".  This
 * answers the question underneath it, which A9.2.3 could not ask at all:
 *
 *   **COMPLETE PROCESSING OF CONFIGURED INPUTS DOES NOT PROVE COMPLETE
 *   DISCOVERY OF RELEVANT INPUTS.**
 *
 * `tu_complete == tu_total` says every unit the named databases contained was
 * parsed. Two databases reporting 200/200 and 216/216 establish nothing whatever
 * about whether a third database exists — and on the repository that produced
 * this season, one did. So a second, separate axis: not "were the inputs
 * processed" but "were the inputs *found*".
 *
 * UNKNOWN is zero, and it is what a repository nobody has walked reads. It is
 * also what a repository whose build description was pinned by hand reads,
 * deliberately: an operator naming two databases is not evidence that there are
 * two, which is the precise mistake this season exists to make unrepeatable. */
typedef enum atlas_sem_discovery {
    /* Atlas did not look, or looking failed. Never sufficient for an absence. */
    ATLAS_SEM_DISC_UNKNOWN = 0,
    /* Atlas looked and stopped early: a ceiling was reached, a subtree was
     * excluded, or a directory could not be read. What was not discovered is not
     * thereby proven absent — DID NOT DISCOVER is not PROVEN NOT TO EXIST. */
    ATLAS_SEM_DISC_PARTIAL,
    /* Atlas walked the whole bounded search universe without reaching a ceiling
     * and without a subtree it could not account for. The claim is bounded by
     * that universe and by nothing wider, and the universe is reported beside
     * the verdict rather than left for a reader to assume. */
    ATLAS_SEM_DISC_COMPLETE
} atlas_sem_discovery;

const char *atlas_sem_discovery_name(atlas_sem_discovery d);
bool atlas_sem_discovery_parse(const char *name, atlas_sem_discovery *out);

/* Whether a repository's compilation databases are searched for or only taken
 * from the operator's pinned list. AUTOMATIC is zero because it is the default,
 * and because the zero must not be the state that *suppresses* looking: a
 * `memset` that turned discovery off would produce a repository reporting
 * UNKNOWN discovery for ever with nothing saying why. */
typedef enum atlas_sem_discovery_mode {
    ATLAS_SEM_DISCMODE_AUTOMATIC = 0,
    ATLAS_SEM_DISCMODE_MANUAL
} atlas_sem_discovery_mode;

const char *atlas_sem_discovery_mode_name(atlas_sem_discovery_mode m);
bool atlas_sem_discovery_mode_parse(const char *name, atlas_sem_discovery_mode *out);

/* --- A9.2.4: whether automatic maintenance runs, and who said so -------------
 *
 * A9.2.3 stored one boolean, `auto_rebuild`, written unconditionally as 0 or 1.
 * A stored 0 therefore could not distinguish "an operator disabled this" from
 * "nobody has ever said anything", and the two call for opposite behaviour: the
 * first must be honoured for ever, the second carries no information at all.
 *
 * The intent and its provenance are separate fields so that the ambiguity cannot
 * recur. A migrated row is UNSET/MIGRATION and reads *migrated, intent unknown*
 * — never *operator disabled*, which would be inventing an intent nobody
 * expressed. */
typedef enum atlas_sem_auto_intent {
    /* Nobody has expressed an intent. The default policy decides. */
    ATLAS_SEM_INTENT_UNSET = 0,
    ATLAS_SEM_INTENT_ENABLED,
    /* Honoured for ever, and never lifted by anything but an operator. */
    ATLAS_SEM_INTENT_DISABLED
} atlas_sem_auto_intent;

const char *atlas_sem_auto_intent_name(atlas_sem_auto_intent i);
bool atlas_sem_auto_intent_parse(const char *name, atlas_sem_auto_intent *out);

typedef enum atlas_sem_intent_source {
    /* No row, or a row that predates anybody saying anything. */
    ATLAS_SEM_INTENT_BY_DEFAULT = 0,
    ATLAS_SEM_INTENT_BY_OPERATOR,
    /* Written by migration 19 from a value that carried no information. */
    ATLAS_SEM_INTENT_BY_MIGRATION
} atlas_sem_intent_source;

const char *atlas_sem_intent_source_name(atlas_sem_intent_source s);
bool atlas_sem_intent_source_parse(const char *name, atlas_sem_intent_source *out);

/* --- a generation ------------------------------------------------------------ */

typedef struct atlas_sem_generation {
    int64_t id;
    int64_t repo_id;
    char repo_identity_hash[65];
    char commit_id[65];
    char compdb_digest[65];  /* over every compilation database, in path order */
    int64_t compdb_count;
    char compiler_id[64];      /* "clang" */
    char compiler_version[96]; /* the libclang version string, Atlas-read */
    char analyzer_id[64];
    int64_t analyzer_version;
    atlas_sem_gen_status status;
    char started_at[ATLAS_TS_MAX];
    char completed_at[ATLAS_TS_MAX];
    int64_t tu_total;
    int64_t tu_complete;
    int64_t tu_partial;
    int64_t tu_failed;
    int64_t tu_unsupported;
    int64_t symbol_count;
    int64_t edge_count;
    int64_t include_count;
    int64_t duration_ms;
    /* A fixed Atlas string, or "" — never compiler output. */
    char failure_reason[96];
    bool is_current;

    /* --- A9.2.3: the coverage manifest, sealed at publication ---------------
     *
     * Reported beside the unit counts and never summed with them: they answer
     * different questions, and a surface that added them would report a
     * generation that read four fifths of a repository the way it reports one
     * that read all of it. */
    atlas_sem_scope_discovery scope_discovery;
    int64_t scope_candidates; /* source files the file index holds */
    int64_t scope_covered;    /* of those, parsed as a translation unit */
    int64_t scope_uncovered;  /* the difference; non-zero refuses an absence */
    /* The test/production split, from the operator's declared test roots and
     * from nothing else. Both zero with `test_scope_known` false means
     * unclassified, which is a different statement from "no test units" — and
     * it is the statement that makes "no production caller" unanswerable. */
    int64_t tu_test;
    int64_t tu_production;
    bool test_scope_known;
    /* A9.2.3. What this generation was built from, in one value: the content of
     * every source and header the file index holds, plus the build description
     * and the toolchain. Empty on a pre-A9.2.3 generation, and an empty stored
     * identity never makes one stale. */
    char source_identity[65];

    /* --- A9.2.4: what the input universe looked like when this was built -----
     *
     * Sealed with the rest of the manifest, and reported beside the unit counts
     * rather than folded into them. `input_count` is how many compilation
     * databases were accepted; `discovery` is whether Atlas can say that was all
     * of them. A generation whose discovery is UNKNOWN or PARTIAL may be
     * perfectly current and perfectly complete over what it read, and still
     * cannot support "there is no X anywhere in this repository". */
    atlas_sem_discovery discovery;
    int64_t input_count;
    /* Candidate sources under an operator-declared vendor prefix. Reported
     * separately and *not* counted as uncovered: an operator saying "this
     * subtree is somebody else's code" is a classification, and treating it as a
     * coverage failure would make every repository with a vendored dependency
     * permanently unable to state an absence about its own code. */
    int64_t scope_excluded;
} atlas_sem_generation;

void atlas_sem_generation_init(atlas_sem_generation *g);

/* --- A9.2.5: the verdict every semantic read carries --------------------------
 *
 * ## The sentence this section exists for
 *
 *   **A SEMANTIC READ THAT FOUND NOTHING HAS NOT ESTABLISHED THAT THERE IS
 *   NOTHING.**
 *
 * A9.2.2 established that for *claims*: `atlas_verify_truth_of` refuses
 * `ABSENT` unless the coverage dimensions an absence rests on were shown
 * sufficient. A9.2.3 gave a generation a coverage manifest and A9.2.4 gave it a
 * discovery verdict. What none of them did was put any of it on the answer to
 * `callers of X`.
 *
 * So until this season every semantic query replied with the rows it found plus
 * `{freshness, stale_reason, generation_id, indexed_commit}` and stopped. A
 * caller receiving
 *
 *     {"freshness":"CURRENT","nodes":[],"summary":{"emitted":0,...}}
 *
 * had nothing in the document telling it the generation had read one source file
 * out of three — and the repository that produced this season answered exactly
 * that, with a PROVEN caller sitting in the file the compilation database never
 * named. The information needed to refuse the conclusion existed, in the same
 * process, one function away, and was not on the answer.
 *
 * ## The asymmetry is A9.2.2's, unchanged, applied one layer out
 *
 *   - One row is enough to establish PRESENT. Coverage does not enter into it:
 *     a caller Atlas *found* exists whatever it failed to look at, and a stale
 *     generation that found one is still evidence that one existed. Positive
 *     rows are therefore emitted whatever the trust facts say, and the
 *     generation they came from is reported beside them so nothing reads as a
 *     statement about the current tree.
 *   - Zero rows establish ABSENT only when the universe searched can be shown
 *     to have been the whole of the relevant one. Anything less is UNKNOWN.
 *
 * UNKNOWN is zero, for the reason every Atlas vocabulary keeps its zero there: a
 * `memset` must not produce an absence proof. */
typedef enum atlas_sem_verdict {
    /* Atlas cannot say. Never evidence in either direction. */
    ATLAS_SEM_VERDICT_UNKNOWN = 0,
    /* At least one row was found. Establishes existence, never completeness. */
    ATLAS_SEM_VERDICT_PRESENT,
    /* Nothing was found, over a universe Atlas can vouch for. */
    ATLAS_SEM_VERDICT_ABSENT
} atlas_sem_verdict;

const char *atlas_sem_verdict_name(atlas_sem_verdict v);
/* False for anything unrecognised — never falls back to a known value, because
 * defaulting an unparsed verdict to ABSENT is the one error that matters. */
bool atlas_sem_verdict_parse(const char *name, atlas_sem_verdict *out);

/* Why a read could not settle. Fixed Atlas strings, checked before they are
 * emitted, never assembled from repository bytes or compiler output — the
 * discipline every `ATLAS_SEM_*` vocabulary follows.
 *
 * Ordered here as `atlas_sem_trust_settle` tests them: the most actionable
 * answer first, so an operator is told the thing they would fix. */
#define ATLAS_SEM_UNK_NO_LIBCLANG "this_atlas_was_built_without_libclang"
#define ATLAS_SEM_UNK_NO_GENERATION                                                                \
    "no_semantic_generation_has_been_published_for_this_repository"
#define ATLAS_SEM_UNK_BUILDING "a_semantic_generation_is_being_built_right_now"
#define ATLAS_SEM_UNK_STALE "the_semantic_generation_does_not_describe_the_current_source"
#define ATLAS_SEM_UNK_MAINTENANCE                                                                  \
    "automatic_semantic_maintenance_is_disabled_for_this_repository"
#define ATLAS_SEM_UNK_SCOPE_UNKNOWN "the_generation_recorded_no_coverage_manifest"
#define ATLAS_SEM_UNK_DISCOVERY                                                                    \
    "build_input_discovery_cannot_say_it_found_every_compilation_database"
#define ATLAS_SEM_UNK_UNITS "a_translation_unit_was_not_fully_described"
#define ATLAS_SEM_UNK_COVERAGE "the_generation_did_not_cover_every_candidate_source"
#define ATLAS_SEM_UNK_TRUNCATED "the_query_reached_a_bound_before_it_finished"

bool atlas_sem_unknown_reason_is_known(const char *reason);

/* Which coverage dimension, if any, stops a generation supporting an absence.
 *
 * **The one implementation of "is this generation's coverage complete?"**, and
 * it returns *which* dimension failed rather than a boolean, because the four
 * are four different problems with four different remedies and a caller that got
 * `false` could only say "something". `NULL` means complete.
 *
 * A9.2.3 put this rule in `coverage_is_complete` in `src/sem/schedule.c` and
 * A9.2.5 needed it twice more — in the verdict and in the scheduler's hold
 * reason. Three copies of a rule that decides whether Atlas may state an absence
 * is exactly the shape this codebase keeps removing, so there is one, and the
 * others ask it. The order is the order `atlas_sem_trust_settle` reports. */
const char *atlas_sem_coverage_gap(atlas_sem_scope_discovery scope_discovery,
                                   atlas_sem_discovery generation_discovery, bool units_complete,
                                   int64_t scope_uncovered);
/* Atlas' own copy of a known reason, or NULL. A value that arrived over a socket
 * is a *matching* string, not Atlas' string — `atlas_sem_why_intern`'s rule. */
const char *atlas_sem_unknown_reason_intern(const char *reason);

/* Everything a consumer needs in order to decide what a semantic answer is worth,
 * gathered once and carried on every load-bearing read.
 *
 * It is one struct rather than a handful of loose fields because it must appear
 * **identically** on `symbol`, `callers`, `callees`, `trace`, `impact`,
 * `context` and `status`, across the CLI's two renderers, the RPC document and
 * the MCP passthrough. Six surfaces keeping seven fields in step by hand is how
 * `have_generation` came to be on the RPC document and not the CLI's; one struct
 * with one writer is how that stops being possible. */
typedef struct atlas_sem_trust {
    atlas_sem_verdict verdict;
    /* An ATLAS_SEM_UNK_* string when `verdict` is UNKNOWN, NULL otherwise. */
    const char *unknown_reason;

    /* Which generation answered, and what it was built from. Reported even when
     * the verdict is PRESENT, because positive rows from a stale generation are
     * evidence about the tree that generation described and about no other. */
    bool have_generation;
    int64_t generation_id;
    char indexed_commit[65];
    /* The generation's sealed `source_identity`, and the tree's identity now.
     * Both, because the divergence is the fact — a surface that showed only the
     * verdict could not say how far behind the index is. Either may be "". */
    char generation_identity[65];
    char live_identity[65];

    atlas_sem_freshness freshness;
    const char *stale_reason; /* an ATLAS_SEM_STALE_* string, or NULL */

    /* A9.2.3's axis, kept separate from freshness for A9.2.3's reason: a
     * generation can be perfectly current and describe half a tree. */
    bool coverage_complete;
    bool units_complete; /* no partial, failed or unsupported translation unit */
    atlas_sem_scope_discovery scope_discovery;
    int64_t scope_candidates;
    int64_t scope_covered;
    int64_t scope_uncovered;

    /* A9.2.4's axis. Two values, never one: `generation_discovery` is what the
     * index being served was built under and is what the verdict rests on;
     * `discovery` is what Atlas can account for now. They differ exactly when a
     * rebuild is due. */
    atlas_sem_discovery generation_discovery;
    atlas_sem_discovery discovery;
    int64_t inputs_accepted;
    int64_t inputs_rejected;

    /* The effective activation answer from `atlas_sem_auto_effective`. A
     * repository nobody maintains drifts, and a consumer is told so rather than
     * left to infer it from a freshness value that was true a moment ago. */
    bool auto_maintenance;
    bool libclang_available;
} atlas_sem_trust;

void atlas_sem_trust_init(atlas_sem_trust *t);

/* Settles the verdict from the trust facts and what the read actually emitted.
 *
 * Pure: it reads `*t`, consults nothing else, and writes only `verdict` and
 * `unknown_reason`. That is what lets the CLI, the RPC server and the daemon
 * reach the same verdict because they call one function rather than because
 * three copies of a rule are kept in step — the property `atlas_sem_freshness_now`
 * established for the freshness question and this season extends to the verdict.
 *
 * `rows_emitted > 0` settles PRESENT and stops. `truncated` matters only when
 * nothing was emitted: a walk that hit a bound and found nothing has not
 * searched its universe. */
void atlas_sem_trust_settle(atlas_sem_trust *t, int64_t rows_emitted, bool truncated);

/* Writes the trust block into an open JSON object, as one fixed set of keys.
 *
 * **This is the whole of the parity fix, and it is a placement decision rather
 * than a helper.** Before A9.2.5 the CLI's renderer and the IPC server were two
 * independently maintained serializers of the same answers, and they had already
 * drifted: `have_generation` was on the RPC document and not the CLI's. Adding
 * fourteen more fields to two writers by hand would have made that certain
 * rather than likely. Both call this, so the block is identical by construction
 * and `tests/test_sem_trust.c` can compare them field for field.
 *
 * Every value it emits is an Atlas integer, an Atlas boolean, a string from a
 * checked Atlas vocabulary or a checked hex digest — A2's five kinds. No
 * repository-controlled byte reaches it, so nothing here needs `atlas_safe`,
 * and nothing here may be extended with a value that would.
 *
 * `atlas_json` is forward-declared rather than included: this header is on the
 * include path of most of Atlas and does not otherwise need the writer. */
typedef struct atlas_json atlas_json;
atlas_status atlas_sem_trust_write_json(atlas_json *j, const atlas_sem_trust *t, atlas_err *err);

/* Gathers every trust fact for one repository's published generation.
 *
 * **Replaces `atlas_sem_freshness_now` on the query paths rather than joining
 * it.** Both compute freshness from the same one pass over the build
 * description; this one keeps the rest of what that pass already had in hand.
 * Calling both would read every source hash twice per response, which is the
 * defect A9.2.3's closure measured and removed.
 *
 * It does **not** settle the verdict: only the caller knows how many rows it
 * emitted, and `atlas_sem_trust_settle` is the one function that decides.
 *
 * `running` is the caller's own observation of whether a generation is being
 * built, for `atlas_sem_freshness_now`'s reason. The `_with_default` form takes
 * the machine-wide activation answer rather than reading the root-owned policy,
 * so a test can drive it both ways and a sweep can read the policy once — the
 * shape `atlas_sem_plan_for_with_default` established, and not a bypass. */
void atlas_sem_trust_now(atlas_db *db, atlas_repo_info *repo, const atlas_sem_generation *g,
                         bool have_generation, bool running, atlas_sem_trust *out);
void atlas_sem_trust_now_with_default(atlas_db *db, atlas_repo_info *repo,
                                      const atlas_sem_generation *g, bool have_generation,
                                      bool running, bool policy_default, atlas_sem_trust *out);

/* --- A9.2.3: the durable semantic build description --------------------------
 *
 * Until A9.2.3 a compilation database reached the indexer only as an argument to
 * the command that ran it, so nothing durable said which build description a
 * repository has — and the daemon therefore could not rebuild anything unaided:
 * it could see that a generation was stale and had no way to know what to read.
 *
 * The row is also the **authority opt-in**, and that is not a secondary use.
 * A8-CI's rule is that indexing runs a compiler over repository source, so it is
 * an authorised operator action and no model may cause one. Making a repository
 * change a rebuild trigger would quietly delete that rule for every registered
 * repository at once. It does not, because `auto_rebuild` is false by default
 * and only an operator writes the row: absent configuration means this daemon
 * never runs a compiler for this repository.
 *
 * Paths are repository-relative and validated inside the root by the indexer
 * exactly as `--compdb` is.
 *
 * **A9.2.4 reversed the second half of this rule.** It said compilation
 * databases are *never discovered* — Atlas does not search a repository for a
 * file that will tell it how to compile things — and the consequence was that
 * the answer to "what are this repository's build inputs?" was whatever somebody
 * had typed, which on the repository that produced that season was two of three.
 * Atlas now searches, inside a bounded universe with every ceiling reported; see
 * `atlas/sem_discover.h`. What survives is that a *pinned* path is still exactly
 * what an operator named, and that the search never follows a symlink or leaves
 * the root. */
#define ATLAS_SEM_CONFIG_MAX_BYTES 8192u

typedef struct atlas_sem_config {
    bool present;
    int64_t repo_id;
    char repo_identity_hash[65];
    /* A9.2.3's boolean, kept as the *effective* answer so that every existing
     * reader keeps working. It is derived from `auto_intent` and the root-owned
     * default by `atlas_sem_auto_effective`, and is never the thing an operator
     * writes. */
    bool auto_rebuild;
    /* A9.2.4. What an operator actually said, and who said it. See
     * `atlas_sem_auto_intent`: these two exist because one boolean could not
     * tell a deliberate refusal from silence. */
    atlas_sem_auto_intent auto_intent;
    atlas_sem_intent_source auto_intent_by;
    /* A9.2.4. Whether Atlas searches this repository for compilation databases.
     * MANUAL leaves discovery UNKNOWN, which is honest rather than harsh: a
     * pinned list is a list somebody wrote, and this season exists because one
     * such list was incomplete and nothing could say so. */
    atlas_sem_discovery_mode discovery_mode;
    /* Newline-separated, repository-relative prefixes the walk does not enter.
     * Owned. Visible in every status surface, because an exclusion that is not
     * shown is a hole in the search universe nobody can see. */
    atlas_buf excludes;
    /* Newline-separated, repository-relative prefixes the operator declares to
     * be somebody else's code. Owned. Candidates under one are counted as
     * `scope_excluded` rather than as uncovered. Atlas guesses at no point: a
     * directory called `vendor` is a directory somebody named. */
    atlas_buf vendor_roots;
    /* Newline-separated, repository-relative. Owned. Under AUTOMATIC discovery
     * these are *pinned* paths, accepted in addition to whatever the walk finds;
     * under MANUAL they are the whole accepted set. */
    atlas_buf compdbs;
    /* Newline-separated, repository-relative prefixes an operator declares to be
     * test sources. Owned. Empty is not "there are no tests" — it is "Atlas does
     * not know which sources are tests", which leaves the tests coverage
     * dimension UNKNOWN. Atlas guesses at no point: a directory called `tests`
     * is a directory somebody named. */
    atlas_buf test_roots;
    char configured_at[ATLAS_TS_MAX];
    /* A9.2.4. The last discovery pass's verdict, when it ran, and which ceiling
     * stopped it if one did.
     *
     * Derived rather than declared — the retry-governor fields below are the
     * same shape, and for the same reason: it is a fact about the repository
     * rather than about any one candidate, and the alternative is a table with
     * one row in it. Written only by a discovery pass, never by `sem-config`. */
    atlas_sem_discovery discovery_state;
    char discovered_at[ATLAS_TS_MAX];
    char discovery_limit[128];
    /* The retry governor's durable half. A further automatic attempt is allowed
     * only once the source identity has moved past `fail_identity` — never after
     * an interval, which would retry an unbuildable tree for ever. */
    int64_t fail_count;
    char fail_identity[65];
    char fail_reason[96]; /* a fixed Atlas string, never compiler output */
    char fail_at[ATLAS_TS_MAX];
} atlas_sem_config;

void atlas_sem_config_init(atlas_sem_config *c);
void atlas_sem_config_free(atlas_sem_config *c);

/* Splits a NUL-separated list into the newline-separated storage form, refusing
 * any element that contains a newline or exceeds the bound.
 *
 * A newline in a path is legal on this filesystem and Atlas' rule is that paths
 * are bytes — so the refusal is deliberate and is the honest trade for a storage
 * form an operator can read back: a path containing a newline cannot be named as
 * a compilation database, and it is refused rather than silently truncated at
 * the newline, which would name a different file. */
atlas_status atlas_sem_config_pack(const char *const *items, size_t count, atlas_buf *out,
                                   atlas_err *err);
/* The inverse: the newline-separated storage form becomes the NUL-separated form
 * every bounded path list in Atlas is carried in — which is the form
 * `atlas_sem_index_opts.compdbs` already takes, so the durable configuration
 * feeds the indexer without a third representation in between. */
atlas_status atlas_sem_config_unpack(const char *packed, atlas_buf *out, size_t *count_out,
                                     atlas_err *err);

/* True when `rel` lies under one of the declared test-root prefixes.
 *
 * A prefix match on a path *component boundary*, never a substring: `tests` must
 * not match `tests_helper.c` sitting beside it, which would classify a
 * production source as a test and make a production-scope absence wrong in the
 * one direction that matters. */
bool atlas_sem_path_is_test(const char *packed_roots, const char *rel);

/* True when `rel` lies under one of the packed prefixes, on a path component
 * boundary. The general form `atlas_sem_path_is_test` is one use of; exclusions
 * and vendor roots are the others, and they must all match identically or an
 * operator would have to learn three different prefix rules. */
bool atlas_sem_path_under_prefix(const char *packed_prefixes, const char *rel);

/* --- A9.2.4: what "automatic maintenance is on" resolves to -------------------
 *
 * The whole activation policy, in one pure function, so that the daemon, the CLI
 * and every status surface answer identically because they call it rather than
 * because somebody keeps three copies in step.
 *
 *   DISABLED -> off, for ever, until an operator says otherwise.
 *   ENABLED  -> on.
 *   UNSET    -> `policy_default`, which is the root-owned machine-wide answer.
 *
 * A9.2.3's rule was that `auto_rebuild` defaults to 0 so that no compiler runs
 * over a repository nobody has spoken about, and **A9.2.4 reverses that
 * default**. The reversal is stated rather than slipped in:
 *
 *   - What the opt-in protected was never code execution. libclang *parses*
 *     repository text; the `command` string is word-split and never executed,
 *     arguments pass a positive allowlist, include directories outside the
 *     repository are recorded and never opened, and the parse runs in a bounded
 *     child with an empty environment and an address-space ceiling. The opt-in
 *     was authority and resource policy, and it is replaced by authority and
 *     resource policy rather than removed.
 *   - Registering a repository is already an operator act that no model can
 *     perform, and it is the act that now carries the consent.
 *   - Whether this daemon may run a compiler on its own initiative for a
 *     repository nobody has spoken about stays a *root-owned* decision — the
 *     `semantic_auto_default` key — rather than becoming a compiled-in fact.
 *   - Nothing model-facing changed: no MCP tool, no gateway route and no
 *     ordinary RPC method enables, disables or triggers semantic maintenance,
 *     and `code.sem_config` stays in the operator-uid table.
 *
 * What it buys is the property the season exists for: **semantic maintenance
 * must not depend on an operator remembering to repair freshness, except where
 * the operator has explicitly disabled it.** */
bool atlas_sem_auto_effective(atlas_sem_auto_intent intent, bool policy_default);

/* --- the configuration digest -------------------------------------------------
 *
 * Two compilations of one file under different `-D`s are different translation
 * units and must not share an identity — the same argument A4 makes about
 * length prefixing, so this is domain-separated and length-prefixed rather than
 * delimited: with any single-byte separator, `-DA=B -DC` and `-DA -DB=C` could
 * encode identically.
 *
 * It covers the include directories, the defines and undefines, the standard and
 * the explicit language, in the order the compilation entry gave them, and
 * nothing else. It deliberately excludes the output path (a build-tree detail
 * that changes nothing about the semantics) and the `command` string (which
 * Atlas hashes separately and never executes). */
#define ATLAS_SEM_CONFIG_DOMAIN "atlas.sem.config.v1"

atlas_status atlas_sem_config_digest(const atlas_code_compdb *db, size_t unit_index,
                                     char out[65], atlas_err *err);

/* --- the parse request and its result ----------------------------------------
 *
 * Parsing happens in a **bounded child process**, never on the daemon's writer
 * thread, and the reasons are not stylistic:
 *
 *   - libclang parses untrusted repository source. A malformed input that
 *     crashes it must cost one translation unit, not the daemon that owns the
 *     index.
 *   - The task requires bounded memory and time per unit. Those are rlimits and
 *     a wall clock on a process, which is a thing a thread does not have.
 *   - A1 forbids creating a process from a worker job, and forbids unbounded
 *     work inside a write transaction. The children are spawned one at a time
 *     from the pass, outside any transaction.
 *
 * The child is this same Atlas binary, re-executed through `atlas_proc_run` —
 * still the one process-creation path in Atlas. It receives an explicitly
 * constructed argument vector built from the *validated* compile-database
 * record, never from the entry's `command` string, and it emits newline
 * delimited JSON facts on stdout which the parent reads through the one yyjson
 * facade.
 *
 * Nothing repository-controlled ever becomes an executable name: argv[0] is
 * Atlas' own resolved path and the subcommand is a compiled-in literal. */

typedef struct atlas_sem_parse_req {
    /* Absolute path of the translation unit's source, already checked to be
     * inside the repository root. */
    const char *source;
    /* The canonical repository root. Facts about files outside it are recorded
     * as external references and their contents are never described. */
    const char *root;
    /* The compilation entry's working directory, used only to resolve relative
     * include directories lexically. Never chdir'ed into. */
    const char *directory;
    /* The allowlisted compiler arguments, already extracted by
     * `atlas_code_compdb_parse`. Each is a counted argument; there is no field
     * here that could hold a shell fragment, which is the A8 rule about
     * validation commands applied to compilation. */
    const char *const *args;
    size_t arg_count;
    int timeout_ms;
    int64_t max_facts;
} atlas_sem_parse_req;

/* One fact the child emitted. Borrowed for the callback only. */
typedef struct atlas_sem_fact {
    /* "symbol", "edge" or "include". */
    const char *record;
    /* Symbol fields. `usr` is Clang's Unified Symbol Resolution string: it is
     * the identity, and it already distinguishes same-named statics in
     * different files, same-named symbols in different scopes, and declarations
     * from the definitions they belong to. Atlas does not invent a mangling. */
    const char *usr;
    const char *name;
    const char *kind;
    const char *linkage;
    const char *type_text;
    /* Repository-relative when inside the root; "" when the symbol belongs to a
     * file outside it, in which case `external` is true and no location is
     * claimed. */
    const char *file;
    int64_t line;
    int64_t col;
    int64_t end_line;
    bool is_definition;
    bool external;

    /* Edge fields. */
    const char *src_usr;
    const char *dst_usr;
    const char *dst_name;
    const char *evidence;
    const char *detail;

    /* Include fields. */
    const char *include_from;
    const char *include_to;
    bool include_angled;
} atlas_sem_fact;

typedef atlas_status (*atlas_sem_fact_cb)(const atlas_sem_fact *fact, void *ud, atlas_err *err);

typedef struct atlas_sem_parse_result {
    atlas_sem_tu_status status;
    const char *why; /* one of the ATLAS_SEM_WHY_* strings, or NULL */
    int64_t symbols;
    int64_t edges;
    int64_t includes;
    int64_t diagnostics_errors; /* counted, never reproduced */
    int64_t duration_ms;
    bool truncated;
} atlas_sem_parse_result;

/* Runs one translation unit through the child parser.
 *
 * `atlas_exe` is Atlas' own absolute executable path. Never fails because the
 * unit failed: a unit that could not be parsed is an ordinary outcome reported
 * through `res`, exactly as A3's extractor degrades rather than taking a pass
 * down. The only error status returned is an allocation failure or an inability
 * to create the process at all. */
atlas_status atlas_sem_parse_unit(const char *atlas_exe, const atlas_sem_parse_req *req,
                                  atlas_sem_fact_cb cb, void *ud, atlas_sem_parse_result *res,
                                  atlas_err *err);

/* The child side: parses one unit and writes NDJSON facts to `out`.
 *
 * Separated from the process plumbing so it is directly testable without
 * spawning anything, which is how the identity tests run. */
atlas_status atlas_sem_parse_here(const atlas_sem_parse_req *req, atlas_buf *out,
                                  atlas_sem_parse_result *res, atlas_err *err);

/* --- building a generation ---------------------------------------------------
 *
 * The pass runs on the writer thread, like A3's structural pass and for the same
 * reason: it is the only thread that owns a writable handle. It differs from A3
 * in one way that matters — it creates processes. So it does that *between*
 * transactions, never inside one, and never from a worker job. A1's rules are
 * unchanged; the pass simply alternates between spawning a bounded child and
 * applying what the child returned.
 *
 * Compilation databases are named explicitly and never discovered. Atlas does
 * not go looking through a repository for a file that will tell it how to
 * compile things: an operator says which databases to read, the paths are
 * validated to be inside the registered root, and a repository that presents
 * several (DNA presents two, in subdirectories) is described by all of them. */

typedef struct atlas_sem_index_opts {
    /* Repository-relative paths of the compilation databases to read,
     * NUL-separated. Empty means the pass has nothing to do and says so — it
     * never falls back to a search. */
    const char *compdbs;
    size_t compdbs_len;
    /* Rebuild from nothing rather than carrying unchanged units forward. */
    bool rebuild;
    /* Absolute path of Atlas' own executable, for the child parser. */
    const char *atlas_exe;
    int root_fd;
    const char *root; /* canonical repository root, absolute */
    const char *commit_id;
    const char *repo_identity_hash;
    /* A9.2.3. Newline-separated, repository-relative prefixes an operator
     * declared to be test sources, or NULL. Used only to classify this
     * generation's units at publication; it changes nothing about what is
     * parsed, because excluding tests from the *index* would make a caller in a
     * test invisible rather than merely labelled. */
    const char *test_roots;
    int64_t max_units;
    /* Polled between units so a long index can be asked to stop. Returning true
     * fails the generation rather than publishing a partial one. */
    bool (*cancel)(void *ud);
    void *cancel_ud;
} atlas_sem_index_opts;

void atlas_sem_index_opts_init(atlas_sem_index_opts *o);

typedef struct atlas_sem_index_summary {
    int64_t generation_id;
    int64_t units_total;
    int64_t units_parsed;
    int64_t units_reused;
    /* A9.2.5. Second attempts spent on transiently failed units. Travels on the
     * operation's detail line with `units_parsed` and `units_reused`, because
     * like them it describes *the pass* rather than the rows the generation
     * holds — A8-CI's closure rule, and the one A9.2.4 had to relearn when the
     * remote form printed `parsed 0` after parsing a whole repository. */
    int64_t units_retried;
    int64_t units_complete;
    int64_t units_partial;
    int64_t units_failed;
    int64_t units_unsupported;
    int64_t symbols;
    int64_t edges;
    int64_t includes;
    int64_t candidates_attached;
    int64_t duration_ms;
    bool published;
    /* Nothing changed at all: same commit, same compilation databases, same
     * compiler, same analyzer. The generation was not rebuilt and did not need
     * to be — the documented reason a no-change run does no work. */
    bool no_change;
    bool truncated;
    const char *truncated_reason;
    char failure_reason[96];
} atlas_sem_index_summary;

void atlas_sem_index_summary_init(atlas_sem_index_summary *s);

/* Builds and publishes one generation for one repository.
 *
 * `db` must be a writable handle owned by the calling thread. On success a new
 * generation is current; on any failure the previous generation is untouched and
 * still current, which is what "preserve the last valid generation until the
 * replacement succeeds" means concretely. */
atlas_status atlas_sem_index_run(atlas_db *db, int64_t repo_id,
                                 const atlas_sem_index_opts *opts,
                                 atlas_sem_index_summary *sum, atlas_err *err);

/* The digest over the compilation databases this repository's semantic index is
 * actually built from — since A9.2.4, the set **discovery accepted** rather than
 * the set an operator pinned.
 *
 * It is what gives the specific `a compilation database changed` staleness
 * reason, which `atlas_sem_source_identity` cannot: that value also moves for an
 * ordinary source edit, and an operator told "the working tree changed" would go
 * looking at their source rather than at their build tree.
 *
 * A repository with nothing accepted yields an empty digest and no error, and an
 * empty live digest never makes a generation stale. A **partly** readable set
 * does report as changed: an accepted database that can no longer be read
 * contributes a fixed marker rather than being skipped, because skipping it
 * would make a repository that has just lost one compare equal to one that never
 * had it.
 *
 * A9.2.3's `atlas_sem_live_compdb_digest` — the same digest over a caller's
 * explicit list — is gone. Every live value now comes from one pass over the
 * accepted set, so it had no callers left, and a second implementation of a
 * digest that decides whether a repository is rebuilt is exactly what must not
 * survive. */
atlas_status atlas_sem_repo_compdb_digest(atlas_db *db, atlas_repo_info *repo, char out[65],
                                          atlas_err *err);

/* A9.2.4: the digest over one repository's *input universe*, as it stands now.
 *
 * Membership from the persisted candidate list a discovery pass wrote; content
 * read live, so an edited or removed compilation database moves this value at
 * once and a newly created one moves it at the next discovery pass. The
 * discovery state is folded in as well, so a PARTIAL walk that later completes
 * with an identical accepted set still moves the identity — which is the only
 * way a generation's sealed manifest can be upgraded, and it costs almost
 * nothing because every unit is reused.
 *
 * This replaces `atlas_sem_repo_compdb_digest`'s role inside
 * `atlas_sem_source_identity`. That function stays, because it answers a
 * narrower question the freshness reasons still want: *did the databases this
 * generation was built from change?* */
atlas_status atlas_sem_repo_discovery_identity(atlas_db *db, atlas_repo_info *repo, char out[65],
                                               atlas_err *err);

/* A9.2.3: everything that determines what a semantic generation would contain,
 * in one comparable value.
 *
 * Domain-separated and length-prefixed, for A4's reason. It covers, in path
 * order: every live C source and header the file index holds, by path and
 * content hash; the **input universe** — A9.2.4, replacing the configured-list
 * digest, so that a compilation database appearing or disappearing moves it; the
 * compiler version; and the analyzer id and version. A generation whose stored identity differs from
 * this one would be built differently, and one whose identity matches would not.
 *
 * The **content hashes** are what make this the working-tree answer rather than
 * a commit answer: Atlas indexes the tree it can see, and an uncommitted edit
 * changes a hash while every commit-derived value stands still.
 *
 * A file the index cannot vouch for — no content hash — contributes a fixed
 * marker rather than being skipped. Skipping it would make a file whose hash
 * Atlas lost compare equal to one that was never there. */
atlas_status atlas_sem_source_identity(atlas_db *db, atlas_repo_info *repo, char out[65],
                                       atlas_err *err);

/* A9.2.4. The live input universe, as a comparable pair.
 *
 * `known` rather than a sentinel count, because **zero accepted inputs is a
 * real and meaningful state**: a repository whose build description has just
 * vanished has fewer inputs than its generation recorded, and that is exactly
 * the case freshness must catch rather than mistake for "nothing was measured".
 * A caller that has not looked passes `known = false` and the check does not
 * fire — "Atlas did not look" is never evidence of change. */
typedef struct atlas_sem_live_inputs {
    bool known;
    int64_t accepted_count;
    atlas_sem_discovery discovery;
} atlas_sem_live_inputs;

/* Freshness of the published generation, recomputed from live facts.
 *
 * Never cached — A6's rule. `reason_out` is one of the ATLAS_SEM_STALE_*
 * strings, or NULL when the generation is current. `live_inputs` may be NULL,
 * which means the same as `known = false`. */
atlas_sem_freshness atlas_sem_freshness_of(const atlas_sem_generation *g, bool have_generation,
                                           bool running, const char *live_commit,
                                           const char *live_repo_identity,
                                           const char *live_compdb_digest,
                                           const char *live_source_identity,
                                           const atlas_sem_live_inputs *live_inputs,
                                           bool file_index_current, const char **reason_out);

/* The same question, with every live fact gathered from the database rather than
 * supplied by the caller.
 *
 * **One implementation per answer**, which is the rule `atlas_sem_impact_on` and
 * `atlas_sem_context_on` follow. Before A9.2.3 four call sites each assembled
 * their own arguments to `atlas_sem_freshness_of`, and they disagreed: the CLI
 * and the daemon passed NULL for the compilation-database digest, and the
 * context builder passed NULL for it *and* hard-coded `file_index_current` to
 * true — so a context package could report a stale index as current, which is
 * exactly the statement a model must never be handed. Parity between the
 * surfaces is structural here rather than four call sites somebody keeps in
 * step.
 *
 * `running` is passed in because it is the caller's own observation of whether a
 * generation is being built, which a read of the current generation cannot see. */
atlas_sem_freshness atlas_sem_freshness_now(atlas_db *db, atlas_repo_info *repo,
                                            const atlas_sem_generation *g, bool have_generation,
                                            bool running, const char **reason_out);

/* --- bounded queries ---------------------------------------------------------
 *
 * Callers, callees, transitive reach and a path between two symbols are one
 * breadth-first walk over the same edges in one of two directions. A single
 * implementation, because two would answer differently the first time somebody
 * fixed a bug in only one of them — A3's argument, unchanged.
 *
 * Everything is bounded and every bound that is reached is *reported*. A
 * truncated walk cannot say it found nothing, and a result that hit a ceiling
 * must never read as a complete answer: that is A6's rule about
 * TRAVERSAL_LIMIT, and it is why `truncated` is not a detail a caller may
 * ignore.
 *
 * **The evidence of a path is the weakest edge on it.** A chain that crosses one
 * indirect call is a candidate chain however many proven edges surround it, and
 * `atlas_sem_evidence_weaker` is the only thing that decides. A reachability
 * answer that silently upgraded to PROVEN because most of the path was proven
 * would be the exact overclaim this layer exists to prevent. */

typedef struct atlas_sem_walk_opts {
    /* The USR the walk starts from. */
    const char *usr;
    /* Inbound answers "who reaches this" (callers); outbound answers "what does
     * this reach" (callees). */
    bool inbound;
    int64_t depth;     /* clamped to ATLAS_SEM_MAX_DEPTH */
    int64_t max_nodes; /* clamped to ATLAS_SEM_MAX_NODES */
    int64_t max_rows;  /* clamped to ATLAS_SEM_MAX_ROWS */
    /* Follow only compiler-proven edges. A caller that wants certainty asks for
     * it explicitly rather than being given it silently. */
    bool proven_only;
} atlas_sem_walk_opts;

void atlas_sem_walk_opts_init(atlas_sem_walk_opts *o);

typedef struct atlas_sem_walk_row {
    int64_t depth;
    const char *usr;
    const char *name;
    const char *kind;
    const char *file_text;
    int64_t line;
    /* How this node was reached: the edge kind, the node it came from, and the
     * weakest evidence class on the whole path to it. */
    const char *edge_kind;
    const char *via_usr;
    const char *via_name;
    const char *evidence;
    /* The call site that produced the edge into this node. */
    const char *site_file;
    int64_t site_line;
    /* For a node reached through an indirect call: how many candidate targets
     * that site had in total, which may exceed how many were recorded. */
    int64_t candidate_total;
} atlas_sem_walk_row;

typedef atlas_status (*atlas_sem_walk_cb)(const atlas_sem_walk_row *row, void *ud,
                                          atlas_err *err);

typedef struct atlas_sem_walk_summary {
    int64_t visited;
    int64_t emitted;
    int64_t max_depth_reached;
    /* The reached set split by the weakest evidence on the path. Merging them
     * would be exactly the conflation this layer exists to prevent. */
    int64_t proven;
    int64_t candidate;
    int64_t lexical;
    int64_t unknown;
    /* An indirect call site whose targets Atlas could not name at all was
     * crossed. The walk past it is not merely uncertain, it is incomplete, and
     * a caller has to be told. */
    int64_t unresolved_indirect;
    bool truncated;
    const char *truncated_reason; /* a fixed Atlas string */
} atlas_sem_walk_summary;

#define ATLAS_SEM_TRUNC_DEPTH "the depth bound was reached"
#define ATLAS_SEM_TRUNC_NODES "the node bound was reached"
#define ATLAS_SEM_TRUNC_ROWS "the result-row bound was reached"
#define ATLAS_SEM_TRUNC_TIME "the query time bound was reached"
bool atlas_sem_trunc_reason_is_known(const char *reason);

atlas_status atlas_sem_walk(atlas_db *db, int64_t generation_id,
                            const atlas_sem_walk_opts *opts, atlas_sem_walk_cb cb, void *ud,
                            atlas_sem_walk_summary *sum, atlas_err *err);

/* One or more bounded paths from `from_usr` to `to_usr`.
 *
 * Rows arrive grouped by path, in order, each carrying its `path_index` in
 * `depth`. `atlas_sem_trace` finds shortest paths first and stops at
 * `max_paths`; there is no claim that the paths returned are the only ones. */
atlas_status atlas_sem_trace(atlas_db *db, int64_t generation_id, const char *from_usr,
                             const char *to_usr, int64_t depth, int64_t max_paths,
                             atlas_sem_walk_cb cb, void *ud, atlas_sem_walk_summary *sum,
                             atlas_err *err);

/* --- impact, candidate tests, and the task context package -------------------
 *
 * These four answers share one rule, and it is the rule the whole season is
 * about: **every item says how it was found.** An impact report mixes things
 * the compiler proved with things Atlas guessed from a filename, and a reader
 * who cannot tell them apart is worse off than one who was given only the
 * proven half. So each item carries an `atlas_sem_evidence` and a fixed reason
 * string saying which question it answers.
 *
 * `ATLAS_SEM_WHY_*_SELECTED` below is that vocabulary. It is closed, it is
 * Atlas-authored, and it is checked before it is emitted — the same treatment
 * every other model-facing string in Atlas gets. */

#define ATLAS_SEM_SEL_DIRECT_CALLER "calls the subject directly"
#define ATLAS_SEM_SEL_TRANSITIVE_CALLER "reaches the subject through a chain of calls"
#define ATLAS_SEM_SEL_CALLEE "is called by the subject"
#define ATLAS_SEM_SEL_DEFINED_HERE "is defined in the subject file"
#define ATLAS_SEM_SEL_INCLUDES "includes the subject file"
#define ATLAS_SEM_SEL_INCLUDED_BY_SUBJECT "is included by the subject file"
#define ATLAS_SEM_SEL_TYPE "is a type the subject uses"
#define ATLAS_SEM_SEL_TEST_BY_REFERENCE "a test file that references the subject"
#define ATLAS_SEM_SEL_TEST_BY_NAME "a test file whose name resembles the subject"
/* A9.1 corrected this literal. It said "an approved Atlas decision anchored near
 * the subject", which was accurate for nothing: A8-CI never produced an item
 * carrying it, and A9.1's items may be any knowledge kind and — with
 * `include_history` — any status, so "approved" and "decision" were both claims
 * the item itself contradicts. A selection reason says *how the item was found*;
 * what it is, is in `knowledge_kind` and `knowledge_status`. */
#define ATLAS_SEM_SEL_DECISION "a recorded knowledge record whose links name a file in scope"
#define ATLAS_SEM_SEL_SUBJECT "the subject itself"
bool atlas_sem_selection_reason_is_known(const char *reason);
const char *atlas_sem_selection_reason_intern(const char *reason);

/* One selected item, whatever selected it. */
typedef struct atlas_sem_item {
    /* "symbol", "file", "decision". */
    char kind[16];
    char name[ATLAS_SEM_MAX_NAME_BYTES];
    char file_text[512];
    /* A9.1, and set only when `kind` is "decision": what sort of knowledge the
     * record is, from `atlas_decision_kind_name`, and where the approval workflow
     * left it.
     *
     * Two fields because they are two dimensions. A package that said only
     * "decision" would tell a reader that a record exists and not whether it is
     * an invariant they must preserve, a risk somebody accepted, or an obligation
     * still outstanding — which is the difference that decides what they do next.
     * Empty on every other kind of item. */
    char knowledge_kind[24];
    char knowledge_status[16];
    /* A9.2.2, §24. What Atlas has established about whether the record's
     * subject is *there* — PRESENT, ABSENT, UNKNOWN or NOT_VERIFIABLE — as a
     * third field beside the other two, because it is a third dimension.
     *
     * This exists so that a model reading a context package cannot turn
     *
     *     [OPERATIONAL_FACT · PROPOSED] "no runtime override for X exists"
     *     truth: UNKNOWN (deployed config unavailable)
     *
     * into "runtime override X does not exist". Without the field the package
     * offers a record whose text is a negative claim and no indication that
     * Atlas never confirmed it, and the summary a reader writes from that is
     * the wrong one.
     *
     * Conservative by construction: `atlas_db_verify_truth_for_document`
     * reports an established value only when every live claim on the record
     * agrees, so the default a reader sees is "Atlas has not established this".
     * Empty on every item that is not a knowledge record. */
    char knowledge_truth[16];
    int64_t line;
    /* How strong the evidence for including this is. A test found by reading a
     * filename is LEXICAL however useful it turns out to be. */
    char evidence[16];
    /* One of the fixed selection reasons above. */
    const char *why;
    int64_t depth;
} atlas_sem_item;

typedef struct atlas_sem_impact_report {
    atlas_repo_info repo;
    atlas_sem_generation generation;
    atlas_sem_freshness freshness;
    const char *stale_reason;
    atlas_sem_trust trust; /* A9.2.5 */
    char query[ATLAS_SEM_MAX_NAME_BYTES];
    /* True when the subject named a file rather than a symbol. The two are
     * different questions and the report says which was asked. */
    bool subject_is_path;
    bool subject_found;

    atlas_sem_item *items;
    size_t count;
    size_t cap;

    /* Split by evidence, because a total would hide the distinction. */
    int64_t proven;
    int64_t candidate;
    int64_t lexical;
    int64_t unresolved_indirect;
    bool truncated;
    const char *truncated_reason;
} atlas_sem_impact_report;

void atlas_sem_impact_report_init(atlas_sem_impact_report *r);
void atlas_sem_impact_report_free(atlas_sem_impact_report *r);

/* --- the task context package ------------------------------------------------
 *
 * Deterministic: the same repository, generation and request produce the same
 * package, byte for byte. Ranking uses only counted, comparable facts — how
 * many of the task's terms a name contains, how close an item is to a seed, and
 * the strength of the evidence — never a model's opinion, because a model's
 * opinion is not reproducible and would make two identical requests disagree.
 *
 * Natural-language task text is used *only* to rank existing evidence. It never
 * selects a repository, never authorises anything, and no imperative in it can
 * cause a write: this builder holds a read-only handle and calls nothing that
 * mutates. */
typedef struct atlas_sem_context_req {
    const char *repo;
    /* Free text. Bounded at ATLAS_SEM_CONTEXT_MAX_TASK_BYTES and refused rather
     * than truncated: a ranked answer to half a question is worse than a
     * refusal. */
    const char *task;
    /* Optional starting points, NUL-separated. */
    const char *paths;
    size_t paths_len;
    const char *symbols;
    size_t symbols_len;
    int64_t depth;
    /* A token budget is converted at ATLAS_SEM_BYTES_PER_TOKEN and then treated
     * as bytes, because bytes are what Atlas can count. */
    int64_t max_tokens;
    int64_t max_bytes;
    int64_t max_items;
    /* Include superseded and rejected decision history. Off by default:
     * rejected prose is not current authority and must not read as though it
     * were. */
    bool include_history;
} atlas_sem_context_req;

void atlas_sem_context_req_init(atlas_sem_context_req *r);

typedef struct atlas_sem_context_report {
    atlas_repo_info repo;
    atlas_sem_generation generation;
    atlas_sem_freshness freshness;
    const char *stale_reason;
    atlas_sem_trust trust; /* A9.2.5 */
    char task[ATLAS_SEM_CONTEXT_MAX_TASK_BYTES];

    atlas_sem_item *items;
    size_t count;
    size_t cap;

    int64_t budget_bytes;
    int64_t used_bytes;
    bool budget_reached;
    /* What Atlas could not supply, so the package states its own gaps rather
     * than reading as complete. Fixed strings. */
    const char *missing[8];
    size_t missing_count;
} atlas_sem_context_report;

void atlas_sem_context_report_init(atlas_sem_context_report *r);
void atlas_sem_context_report_free(atlas_sem_context_report *r);

#define ATLAS_SEM_MISSING_INDEX "no semantic index exists for this repository"
#define ATLAS_SEM_MISSING_STALE "the semantic index does not describe the current commit"
#define ATLAS_SEM_MISSING_SEEDS "no starting path or symbol matched the task"
#define ATLAS_SEM_MISSING_BUDGET "the byte budget was reached before every item was included"
#define ATLAS_SEM_MISSING_DECISIONS "no recorded knowledge was found for the files in scope"
/* A9.2.5. The package could not state its own coverage gaps.
 *
 * A context package built from an INCOMPLETE generation stated its budget gaps
 * and its staleness and said nothing whatever about having read a third of the
 * tree — and this is the package that goes to a model. Two constants rather than
 * one, because they are two different holes with two different remedies: the
 * compilation database names a strict subset of the sources, and Atlas cannot
 * say it found every compilation database. */
#define ATLAS_SEM_MISSING_COVERAGE                                                                 \
    "the semantic index did not cover every candidate source in this repository"
#define ATLAS_SEM_MISSING_DISCOVERY                                                                \
    "Atlas cannot say it found every compilation database in this repository"

/* The libclang version string Atlas will record, e.g. "clang version 14.0.6".
 * Read from the library, never from a command's output. */
const char *atlas_sem_compiler_version(void);
const char *atlas_sem_compiler_id(void);
/* False when Atlas was built without libclang. Every entry point checks it and
 * reports a typed refusal rather than pretending an empty index is an answer. */
bool atlas_sem_available(void);

#endif /* ATLAS_SEM_H */
