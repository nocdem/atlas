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
#define ATLAS_SEM_ANALYZER_VERSION 1

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
bool atlas_sem_why_is_known(const char *why);
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
#define ATLAS_SEM_STALE_INCOMPLETE "the_last_generation_did_not_complete"
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
} atlas_sem_generation;

void atlas_sem_generation_init(atlas_sem_generation *g);

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
 * Paths are repository-relative, validated inside the root by the indexer
 * exactly as `--compdb` is, and **never discovered**: Atlas does not search a
 * repository for a file that will tell it how to compile things. */
#define ATLAS_SEM_CONFIG_MAX_BYTES 8192u

typedef struct atlas_sem_config {
    bool present;
    int64_t repo_id;
    char repo_identity_hash[65];
    bool auto_rebuild;
    /* Newline-separated, repository-relative. Owned. */
    atlas_buf compdbs;
    /* Newline-separated, repository-relative prefixes an operator declares to be
     * test sources. Owned. Empty is not "there are no tests" — it is "Atlas does
     * not know which sources are tests", which leaves the tests coverage
     * dimension UNKNOWN. Atlas guesses at no point: a directory called `tests`
     * is a directory somebody named. */
    atlas_buf test_roots;
    char configured_at[ATLAS_TS_MAX];
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

/* A9.2.3: the compilation-database digest, computed from the files as they are
 * now rather than as the generation recorded them.
 *
 * `compdbs` is the NUL-separated repository-relative list, opened bounded and
 * without following a symlink from `root_fd`. This is the value
 * `atlas_sem_freshness_of` compares against a generation's stored
 * `compdb_digest`, and until A9.2.3 every caller passed NULL for it, which made
 * that comparison unreachable — see the implementation for why that was
 * defensible while rebuilding was manual and is not once the daemon owns
 * freshness.
 *
 * An unreadable database yields an empty digest and a non-OK status; an empty
 * live digest never makes a generation stale, because "Atlas could not look" is
 * not evidence that the description changed. */
atlas_status atlas_sem_live_compdb_digest(int root_fd, const char *compdbs, size_t compdbs_len,
                                          char out[65], atlas_err *err);

/* The same, for a registered repository, reading the databases its durable
 * build description names. A repository with no description yields an empty
 * digest and no error: there is nothing declared to compare against, so there is
 * no claim to make. */
atlas_status atlas_sem_repo_compdb_digest(atlas_db *db, atlas_repo_info *repo, char out[65],
                                          atlas_err *err);

/* A9.2.3: everything that determines what a semantic generation would contain,
 * in one comparable value.
 *
 * Domain-separated and length-prefixed, for A4's reason. It covers, in path
 * order: every live C source and header the file index holds, by path and
 * content hash; the live compilation-database digest; the compiler version; and
 * the analyzer id and version. A generation whose stored identity differs from
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

/* Freshness of the published generation, recomputed from live facts.
 *
 * Never cached — A6's rule. `reason_out` is one of the ATLAS_SEM_STALE_*
 * strings, or NULL when the generation is current. */
atlas_sem_freshness atlas_sem_freshness_of(const atlas_sem_generation *g, bool have_generation,
                                           bool running, const char *live_commit,
                                           const char *live_compdb_digest,
                                           const char *live_source_identity,
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

/* The libclang version string Atlas will record, e.g. "clang version 14.0.6".
 * Read from the library, never from a command's output. */
const char *atlas_sem_compiler_version(void);
const char *atlas_sem_compiler_id(void);
/* False when Atlas was built without libclang. Every entry point checks it and
 * reports a typed refusal rather than pretending an empty index is an answer. */
bool atlas_sem_available(void);

#endif /* ATLAS_SEM_H */
