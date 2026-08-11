/* Atlas - structural code intelligence: the truth model and the extractor.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A3 reads C-family source and records what it structurally contains. It is not
 * a compiler and this header is where that is made explicit rather than left as
 * a caveat: every fact Atlas stores carries an `atlas_code_resolution` saying
 * how it was arrived at, and the classes that mean "proven" are only ever used
 * for facts a byte-level reader can actually prove.
 *
 * The full contract, including the explicit non-claims, is in
 * docs/code-intelligence.md. The two rules that shape this header:
 *
 * 1. **The existence of a construct and the resolution of what it refers to are
 *    different facts with different classes.** That `#include "buf.h"` appears
 *    at line 12 is SOURCE_EXACT. That it means `src/core/buf.h` is a separate
 *    edge that may be BUILD_METADATA, UNIQUE_LEXICAL, AMBIGUOUS or UNRESOLVED.
 *
 * 2. **A symbol is a site, not a global entity.** Two files each defining
 *    `static void helper(void)` produce two rows and nothing merges them.
 *    Cross-file identity is an edge with a class, which is the only honest way a
 *    lexical indexer can express it.
 *
 * The extractor here is pure: bytes in, a bounded result object out. It opens no
 * file, touches no database handle and creates no process, which is what lets it
 * run on the daemon's worker threads beside the hash jobs.
 */
#ifndef ATLAS_CODE_H
#define ATLAS_CODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "atlas/buf.h"
#include "atlas/db.h"
#include "atlas/error.h"
#include "atlas/limits.h"
#include "atlas/workers.h"

/* --- resolution classes ---------------------------------------------------
 *
 * The vocabulary is complete across phases so the schema is stable, exactly as
 * `atlas_evidence_kind` is. A3 may not write MODEL_PROPOSAL; the restriction is
 * enforced in `atlas_code_resolution_writable_in_a3` and by the CHECK on the
 * insert path, not by convention. */
typedef enum atlas_code_resolution {
    /* Read directly from the bytes. No inference of any kind. */
    ATLAS_CODE_RES_SOURCE_EXACT = 0,
    /* Resolved using a validated compile-database record. */
    ATLAS_CODE_RES_BUILD_METADATA,
    /* Exactly one candidate matched lexically. Still not compiler-proven. */
    ATLAS_CODE_RES_UNIQUE_LEXICAL,
    /* More than one candidate matched. The set is recorded; nothing is chosen. */
    ATLAS_CODE_RES_AMBIGUOUS,
    /* No candidate matched, with a typed reason. */
    ATLAS_CODE_RES_UNRESOLVED,
    /* Found inside preprocessor conditionals Atlas did not evaluate. */
    ATLAS_CODE_RES_CONDITIONAL,
    /* A model asserted it. Never written in A3. */
    ATLAS_CODE_RES_MODEL_PROPOSAL,
    /* No basis at all. */
    ATLAS_CODE_RES_UNKNOWN
} atlas_code_resolution;

const char *atlas_code_resolution_name(atlas_code_resolution r);
/* Parses a stored name. False for anything unrecognised: defaulting an unknown
 * class to a known one is how a guess becomes a recorded fact. */
bool atlas_code_resolution_parse(const char *name, atlas_code_resolution *out);
/* True when the A3 indexer may write this class. Refuses MODEL_PROPOSAL. */
bool atlas_code_resolution_writable_in_a3(atlas_code_resolution r);
/* True when the class means "Atlas established which thing this refers to".
 * SOURCE_EXACT, BUILD_METADATA and UNIQUE_LEXICAL; nothing else. */
bool atlas_code_resolution_is_resolved(atlas_code_resolution r);

/* Where a graph row's information came from. Deliberately narrower than the
 * resolution class: this says which *source* was read, not how sure Atlas is. */
typedef enum atlas_code_provenance {
    ATLAS_CODE_PROV_SOURCE = 0,   /* repository bytes */
    ATLAS_CODE_PROV_BUILD_METADATA, /* a validated compile-database record */
    ATLAS_CODE_PROV_INFERENCE,    /* derived from other graph rows; detail says how */
    ATLAS_CODE_PROV_UNKNOWN
} atlas_code_provenance;

const char *atlas_code_provenance_name(atlas_code_provenance p);

/* Why a relation is unresolved or ambiguous. A fixed Atlas vocabulary, so the
 * value can be reported to a model without being repository text. */
#define ATLAS_CODE_WHY_SYSTEM_HEADER "system_or_external_header"
#define ATLAS_CODE_WHY_NOT_IN_REPO "no_repository_file_matched"
#define ATLAS_CODE_WHY_MANY_FILES "several_repository_files_matched"
#define ATLAS_CODE_WHY_NO_DEFINITION "no_definition_found"
#define ATLAS_CODE_WHY_DECL_ONLY "only_declarations_found"
#define ATLAS_CODE_WHY_MANY_DEFINITIONS "several_definitions_found"
#define ATLAS_CODE_WHY_INDIRECT "indirect_or_unknown"
#define ATLAS_CODE_WHY_MACRO_AND_FUNCTION "macro_and_function_share_this_name"
#define ATLAS_CODE_WHY_CONDITIONAL "found_under_unevaluated_conditional"
#define ATLAS_CODE_WHY_DERIVED_INCLUDE "derived_from_a_resolved_include"
#define ATLAS_CODE_WHY_DERIVED_CALL "derived_from_a_resolved_call_candidate"
#define ATLAS_CODE_WHY_TRUNCATED "a_per_file_ceiling_was_reached"
/* True when `why` is one of the fixed strings above. Checked before a reason is
 * emitted, so a value that came from somewhere else becomes "other" rather than
 * being reproduced. */
bool atlas_code_why_is_known(const char *why);

/* --- the analyzer that produced the graph -----------------------------------
 *
 * Provenance says which *source* a fact was read from and resolution says how
 * firmly it was established. Neither says which *algorithm* produced it, and
 * that is a third question with its own failure mode: Atlas is upgraded with
 * corrected extraction or corrected resolution, the repository bytes and the
 * compile database are byte-identical, every generation still matches — and the
 * stored graph is now wrong in exactly the way the upgrade fixed, while
 * reporting itself current.
 *
 * These two values close that. They are Atlas-owned constants compiled into the
 * binary: no repository and no model can influence either, which is what lets
 * them be reported to a model at all.
 *
 * **Bump ATLAS_CODE_ANALYZER_VERSION whenever a pass would produce different
 * facts from identical bytes** — a lexer fix, a resolution rule change, a
 * different set of materialised edges. Not for a refactor that cannot change an
 * output. The next pass after a bump rebuilds the structural graph, and until it
 * does the graph is reported stale rather than current. */
#define ATLAS_CODE_ANALYZER_ID "atlas-c-lexical"
#define ATLAS_CODE_ANALYZER_VERSION 1

/* True when the stored graph was produced by this binary's analyzer. False when
 * nothing has indexed yet, and false after an upgrade that changed the
 * algorithm — which is what makes such a graph stale rather than current. */
bool atlas_code_analyzer_matches(const atlas_code_index_state *code_state);

/* --- language ------------------------------------------------------------- */

typedef enum atlas_code_language {
    ATLAS_CODE_LANG_NONE = 0, /* not a language A3 extracts from */
    ATLAS_CODE_LANG_C,        /* .c */
    ATLAS_CODE_LANG_C_HEADER, /* .h */
    ATLAS_CODE_LANG_C_FRAGMENT /* .inc / .def, parsed only when included */
} atlas_code_language;

const char *atlas_code_language_name(atlas_code_language l);
/* Classifies by extension from raw path bytes. A path is bytes, so this takes a
 * length rather than a C string. Returns ATLAS_CODE_LANG_NONE for everything
 * A3 does not extract from — including C++, deliberately. */
atlas_code_language atlas_code_language_of(const void *path_raw, size_t path_len);

/* --- symbols --------------------------------------------------------------- */

typedef enum atlas_code_symbol_kind {
    ATLAS_CODE_SYM_FUNCTION = 0,
    ATLAS_CODE_SYM_MACRO,          /* #define NAME ... */
    ATLAS_CODE_SYM_MACRO_FUNCTION, /* #define NAME(...) ... */
    ATLAS_CODE_SYM_TYPEDEF,
    ATLAS_CODE_SYM_STRUCT,
    ATLAS_CODE_SYM_UNION,
    ATLAS_CODE_SYM_ENUM,
    ATLAS_CODE_SYM_ENUM_CONSTANT,
    ATLAS_CODE_SYM_VARIABLE,
    ATLAS_CODE_SYM_UNKNOWN
} atlas_code_symbol_kind;

const char *atlas_code_symbol_kind_name(atlas_code_symbol_kind k);
bool atlas_code_symbol_kind_parse(const char *name, atlas_code_symbol_kind *out);

/* C linkage, as far as it is lexically visible.
 *
 * This is load-bearing rather than decorative: an `internal` definition is a
 * candidate only for occurrences in the same file, which is what keeps two
 * files' `static helper` functions distinct. `none` is the preprocessor's
 * answer — a macro has no linkage — and `unknown` means the declarator run was
 * not classified, which is a real outcome the extractor reports rather than
 * guesses past. */
typedef enum atlas_code_linkage {
    ATLAS_CODE_LINK_EXTERNAL = 0,
    ATLAS_CODE_LINK_INTERNAL,
    ATLAS_CODE_LINK_NONE,
    ATLAS_CODE_LINK_UNKNOWN
} atlas_code_linkage;

const char *atlas_code_linkage_name(atlas_code_linkage l);

/* --- file roles ------------------------------------------------------------ */

typedef enum atlas_code_role {
    ATLAS_CODE_ROLE_IMPLEMENTATION = 0,
    ATLAS_CODE_ROLE_PUBLIC_HEADER,
    ATLAS_CODE_ROLE_PRIVATE_HEADER,
    ATLAS_CODE_ROLE_TEST,
    ATLAS_CODE_ROLE_BUILD_METADATA,
    ATLAS_CODE_ROLE_DOCUMENTATION,
    ATLAS_CODE_ROLE_VENDORED,
    ATLAS_CODE_ROLE_GENERATED,
    ATLAS_CODE_ROLE_UNKNOWN
} atlas_code_role;

const char *atlas_code_role_name(atlas_code_role r);

/* How a role was arrived at. Reported with every role, because path naming is
 * evidence about a path and not proof about a file, and a consumer that is shown
 * `role: test, basis: path_naming` knows exactly how much Atlas knows. */
typedef enum atlas_code_role_basis {
    ATLAS_CODE_BASIS_EXTENSION = 0,
    ATLAS_CODE_BASIS_PATH_NAMING,
    ATLAS_CODE_BASIS_CONTENT_MARKER,
    ATLAS_CODE_BASIS_BUILD_METADATA,
    ATLAS_CODE_BASIS_INCLUDE_GRAPH,
    ATLAS_CODE_BASIS_NONE
} atlas_code_role_basis;

const char *atlas_code_role_basis_name(atlas_code_role_basis b);

/* --- relations -------------------------------------------------------------- */

typedef enum atlas_code_rel_kind {
    ATLAS_CODE_REL_FILE_INCLUDES_FILE = 0,
    ATLAS_CODE_REL_FILE_DEFINES_SYMBOL,
    ATLAS_CODE_REL_FILE_DECLARES_SYMBOL,
    ATLAS_CODE_REL_UNIT_COMPILES_FILE,
    ATLAS_CODE_REL_UNIT_USES_HEADER,
    ATLAS_CODE_REL_SYMBOL_CONTAINS_OCCURRENCE,
    ATLAS_CODE_REL_SYMBOL_CALLS_SYMBOL,
    ATLAS_CODE_REL_SYMBOL_DECLARED_BY,
    ATLAS_CODE_REL_SYMBOL_DEFINED_BY,
    ATLAS_CODE_REL_FILE_DEPENDS_ON_FILE
} atlas_code_rel_kind;

const char *atlas_code_rel_kind_name(atlas_code_rel_kind k);
bool atlas_code_rel_kind_parse(const char *name, atlas_code_rel_kind *out);

/* What a relation endpoint is. `unresolved` is a first-class endpoint kind: an
 * include whose target Atlas could not find is still a recorded edge carrying
 * its spelling, because "this file includes something called config.h that I
 * cannot place" is a useful fact and dropping it would be a silence. */
typedef enum atlas_code_node_kind {
    ATLAS_CODE_NODE_FILE = 0,
    ATLAS_CODE_NODE_SYMBOL,
    ATLAS_CODE_NODE_UNIT,
    ATLAS_CODE_NODE_OCCURRENCE,
    ATLAS_CODE_NODE_UNRESOLVED
} atlas_code_node_kind;

const char *atlas_code_node_kind_name(atlas_code_node_kind k);

/* --- the extractor's result ------------------------------------------------
 *
 * A flat, bounded object. Names live in one arena and every item refers to them
 * by offset, so the item arrays stay arrays of fixed-size records: a worker pool
 * indexes them by job index, and an array of pointers into separately allocated
 * strings would be both larger and worse to iterate.
 *
 * Nothing here holds a pointer into the source buffer, so the caller may free
 * the source the moment the parse returns. */

typedef enum atlas_code_parse_status {
    ATLAS_CODE_PARSE_OK = 0,
    /* Something was not understood, or a ceiling was reached. The facts that
     * were extracted are still valid; the file is simply not fully described,
     * and it says so rather than reading as complete. */
    ATLAS_CODE_PARSE_PARTIAL,
    ATLAS_CODE_PARSE_FAILED,
    /* Deliberately not parsed: binary, or a language A3 does not extract from. */
    ATLAS_CODE_PARSE_SKIPPED
} atlas_code_parse_status;

const char *atlas_code_parse_status_name(atlas_code_parse_status s);

typedef enum atlas_code_include_form {
    ATLAS_CODE_INCLUDE_QUOTE = 0,
    ATLAS_CODE_INCLUDE_ANGLE
} atlas_code_include_form;

typedef struct atlas_code_symbol_item {
    uint32_t name_off; /* into atlas_code_parse::arena */
    uint32_t name_len;
    int32_t kind;      /* atlas_code_symbol_kind */
    int32_t linkage;   /* atlas_code_linkage */
    int32_t resolution; /* SOURCE_EXACT, or CONDITIONAL inside an #if */
    int32_t enclosing; /* index into symbols[]; -1 for file scope */
    int64_t line;
    int64_t col;
    int64_t byte_offset;
    int64_t end_line;
    bool is_definition;
    bool is_declaration;
} atlas_code_symbol_item;

typedef struct atlas_code_include_item {
    uint32_t spelling_off;
    uint32_t spelling_len;
    int32_t form;       /* atlas_code_include_form */
    int32_t resolution; /* SOURCE_EXACT, or CONDITIONAL inside an #if */
    int64_t line;
    int64_t col;
} atlas_code_include_item;

typedef struct atlas_code_occurrence_item {
    uint32_t name_off;
    uint32_t name_len;
    int32_t enclosing;  /* index into symbols[]; -1 when outside any function */
    int32_t resolution; /* SOURCE_EXACT, or CONDITIONAL inside an #if */
    int64_t line;
    int64_t col;
    int64_t byte_offset;
} atlas_code_occurrence_item;

typedef struct atlas_code_parse {
    atlas_buf arena;

    atlas_code_symbol_item *symbols;
    size_t symbol_count;
    size_t symbol_cap;

    atlas_code_include_item *includes;
    size_t include_count;
    size_t include_cap;

    atlas_code_occurrence_item *occurrences;
    size_t occurrence_count;
    size_t occurrence_cap;

    atlas_code_parse_status status;
    /* True when a ceiling was reached or a construct was not understood.
     * `truncated_reason` is one of the fixed ATLAS_CODE_WHY_* strings or a fixed
     * literal in the extractor; it is never assembled from source bytes. */
    bool truncated;
    const char *truncated_reason;
    /* Set when the extractor recognised a leading include guard and therefore
     * did not count it as a conditional level. Reported so the choice is
     * visible rather than silent. */
    bool include_guard;
    int64_t lines;
    int64_t bytes;
    /* Counters for what was dropped, so "we saw nothing" and "we stopped
     * looking" are different answers. */
    int64_t dropped_symbols;
    int64_t dropped_includes;
    int64_t dropped_occurrences;
} atlas_code_parse;

void atlas_code_parse_init(atlas_code_parse *p);
void atlas_code_parse_free(atlas_code_parse *p);

/* Borrowed for the duration of the call only; the item indexes into the arena,
 * which moves as the parse grows. */
const char *atlas_code_parse_name(const atlas_code_parse *p, uint32_t off);

/* Extracts structure from `len` bytes of C-family source.
 *
 * Pure: no file is opened, no process is created, no database handle is touched.
 * That is what lets this run on the daemon's worker threads.
 *
 * Never fails on malformed input. A file it cannot make sense of comes back
 * PARTIAL or FAILED with whatever was established, because a hostile or
 * generated file must degrade rather than take the pass down. The only error
 * status this returns is an allocation failure. */
atlas_status atlas_code_extract(const void *data, size_t len, atlas_code_language lang,
                                atlas_code_parse *out, atlas_err *err);

/* --- compile databases ------------------------------------------------------
 *
 * `compile_commands.json` is data. It is read, bounded, and parsed through the
 * one yyjson facade; nothing in it is ever executed, passed to a shell, or used
 * to construct a process. The `command` string is not even stored: its presence
 * and a SHA-256 of it are, which is enough to notice that it changed and
 * incapable of being run.
 *
 * `arguments` is walked with a positive allowlist. Everything not on it —
 * including `-include`, `-fplugin=` and any `@response-file` — is counted in
 * `dropped_args` and otherwise ignored. Response files are never opened. */

typedef enum atlas_code_incdir_kind {
    ATLAS_CODE_INCDIR_SEARCH = 0, /* -I */
    ATLAS_CODE_INCDIR_QUOTE,      /* -iquote */
    ATLAS_CODE_INCDIR_SYSTEM,     /* -isystem */
    ATLAS_CODE_INCDIR_AFTER       /* -idirafter */
} atlas_code_incdir_kind;

const char *atlas_code_incdir_kind_name(atlas_code_incdir_kind k);

typedef struct atlas_code_cu_incdir {
    uint32_t path_off; /* repository-relative when internal, absolute when not */
    uint32_t path_len;
    int32_t kind;
    bool external; /* outside the repository: metadata only, never opened */
} atlas_code_cu_incdir;

typedef struct atlas_code_cu_define {
    uint32_t name_off;
    uint32_t name_len;
    uint32_t value_off; /* 0 when the define carried no value */
    uint32_t value_len;
    bool undef;
} atlas_code_cu_define;

typedef struct atlas_code_cu {
    uint32_t source_off; /* repository-relative source path, raw bytes */
    uint32_t source_len;
    uint32_t output_off; /* "" when the entry had none */
    uint32_t output_len;
    uint32_t dir_off;
    uint32_t dir_len;
    uint32_t std_off; /* "" when no -std was given */
    uint32_t std_len;
    uint32_t lang_off; /* "" when no -x was given */
    uint32_t lang_len;
    size_t incdir_first;
    size_t incdir_count;
    size_t define_first;
    size_t define_count;
    int64_t arg_count;
    int64_t dropped_args;
    /* Where this entry sat in the document, so two configurations of one file
     * keep a stable, reproducible order. */
    int64_t entry_index;
    bool command_present;
    /* SHA-256 of the `command` string, lowercase hex, or "" when absent. The
     * string itself is deliberately not kept. */
    char command_hash[65];
} atlas_code_cu;

typedef struct atlas_code_compdb {
    atlas_buf arena;
    atlas_code_cu *units;
    size_t unit_count;
    size_t unit_cap;
    atlas_code_cu_incdir *incdirs;
    size_t incdir_count;
    size_t incdir_cap;
    atlas_code_cu_define *defines;
    size_t define_count;
    size_t define_cap;

    int64_t entries_seen;
    int64_t entries_dropped; /* outside the repository, or malformed */
    bool truncated;
    const char *truncated_reason; /* a fixed Atlas string */
} atlas_code_compdb;

void atlas_code_compdb_init(atlas_code_compdb *c);
void atlas_code_compdb_free(atlas_code_compdb *c);
const char *atlas_code_compdb_str(const atlas_code_compdb *c, uint32_t off);

/* Parses a compile database.
 *
 * `root_raw`/`root_len` are the repository's canonical root bytes: every path is
 * normalised lexically (no symlink is followed, nothing is stat'ed) and then
 * checked against the root. A `file` outside it is dropped with a reason; an
 * include directory outside it is kept with `external` set and is never opened.
 *
 * A malformed or oversized document is an ordinary outcome: it produces zero
 * units and a reason, not a failure that takes a pass down. */
atlas_status atlas_code_compdb_parse(const void *data, size_t len, const void *root_raw,
                                     size_t root_len, atlas_code_compdb *out, atlas_err *err);

/* --- file roles ------------------------------------------------------------
 *
 * Classification from the path bytes and a bounded content prefix. Deliberately
 * separate from the extractor: a role can be established for a file A3 does not
 * parse at all, which is most of a repository. */

typedef struct atlas_code_role_item {
    int32_t role;  /* atlas_code_role */
    int32_t basis; /* atlas_code_role_basis */
    int32_t resolution;
} atlas_code_role_item;

/* At most this many roles are recorded for one file. */
#define ATLAS_CODE_MAX_ROLES_PER_FILE 6

typedef struct atlas_code_roles {
    atlas_code_role_item items[ATLAS_CODE_MAX_ROLES_PER_FILE];
    size_t count;
} atlas_code_roles;

/* `prefix` may be NULL: a role that needs content simply is not claimed. */
void atlas_code_classify_roles(const void *path_raw, size_t path_len, const void *prefix,
                               size_t prefix_len, atlas_code_roles *out);

/* --- the structural pass -----------------------------------------------------
 *
 * A stage of the A1 reconciliation pass, not a second pipeline. It runs on the
 * writer thread, after the file index has been applied, and it borrows the same
 * worker pool the hash stage used.
 *
 * The stages mirror A1's for the same reasons:
 *
 *   1. **select** — one query comparing `files.content_hash` against the hash
 *      the stored graph facts were extracted from. Nothing is stat'ed and
 *      nothing is read. This is what makes an unchanged pass parse zero files
 *      *even when it was a full content-verifying pass*.
 *   2. **parse** — read and extract only the selected files, across the worker
 *      pool. No transaction is open, no database handle is touched by a job,
 *      and no process is created.
 *   3. **apply** — replace each file's graph rows in bounded transactions. No
 *      file read and no parse happens inside one.
 *   4. **resolve** — deterministically, over a bounded set, after everything is
 *      applied. Ordered by stable keys, so the result does not depend on the
 *      order the workers finished in.
 *
 * The generation is published only at the end, so a crash mid-pass leaves the
 * previous complete generation in place and the hash comparison in stage 1
 * redoes exactly what was missing.
 */

typedef struct atlas_code_pass_opts {
    atlas_workers *workers; /* NULL parses serially on the calling thread */
    int root_fd;            /* the repository root, for no-follow reads */
    /* Canonical repository root bytes. Used to check compile-database paths
     * against the repository; never used to construct one to open. */
    const void *root_raw;
    size_t root_len;
    /* Drop every structural row first, so the pass rebuilds from nothing. This
     * is what `atlas code sync --rebuild` asks for; a normal pass never does it,
     * because the hash comparison already covers every real change. */
    bool rebuild;
    int64_t max_files; /* 0 means ATLAS_CODE_MAX_PARSE_FILES_PER_PASS */
} atlas_code_pass_opts;

void atlas_code_pass_opts_init(atlas_code_pass_opts *o);

typedef struct atlas_code_pass_summary {
    int64_t files_selected;
    int64_t files_parsed;
    int64_t files_removed;
    int64_t files_failed;
    int64_t files_partial;
    int64_t symbols_written;
    int64_t relations_written;
    int64_t relations_resolved;
    int64_t relations_ambiguous;
    int64_t relations_unresolved;
    int64_t compile_units;
    bool compile_db_present;
    bool compile_db_changed;
    /* Resolution had to sweep the whole repository rather than the names this
     * pass touched. Reported rather than silent: it is still resolution and
     * never a reparse, and a reader is entitled to know which happened. */
    bool resolve_fallback;
    /* The stored graph was built by a different analyzer version, so this pass
     * rebuilt it whether or not a rebuild was asked for. */
    bool analyzer_changed;
    bool degraded;
    const char *degraded_reason; /* a fixed Atlas string */
    bool truncated;
    const char *truncated_reason;
    int64_t duration_ms;
} atlas_code_pass_summary;

void atlas_code_pass_summary_init(atlas_code_pass_summary *s);

/* Runs the structural pass for one repository.
 *
 * `db` must be a writable handle owned by the calling thread — in the daemon,
 * the writer thread and only the writer thread. `generation` is the
 * reconciliation pass's own generation, so "the graph describes the file index"
 * stays an integer comparison. */
atlas_status atlas_code_pass_run(atlas_db *db, int64_t repo_id, int64_t generation,
                                 const atlas_code_pass_opts *opts, atlas_code_pass_summary *sum,
                                 atlas_err *err);

/* What a resolution sweep is allowed to look at.
 *
 * Resolution is the expensive half of a structural pass, and the reason is that
 * "re-attempt everything unresolved" is repository-sized however small the
 * change was. This describes the smallest set that can differ from last time,
 * and each field is here because leaving it out would produce a wrong answer,
 * not merely a slower one:
 *
 *   `names`       — the definitions that appeared or vanished. A call resolves
 *                   by name and by nothing else, so an edge naming one of these
 *                   may have a different answer now, wherever it lives. Found by
 *                   an indexed seek on `dst_name`, never by reparsing.
 *   `files`       — the files this pass parsed. Their edges were rewritten
 *                   unresolved and must be settled; their destinations need not
 *                   be names anybody changed.
 *   `file_set_changed` — a path was added or removed. Include resolution reads
 *                   the set of paths, so this is the only thing that can make a
 *                   *previously* unresolvable include resolvable. An edit to an
 *                   existing file cannot, which is what keeps the common case
 *                   from sweeping every `<stdio.h>` in the repository.
 *   `full`        — re-resolve everything: a rebuild, or an incremental scope
 *                   that overflowed its own bound and honestly says so.
 *
 * Exposed separately from the pass because the reasoning above belongs next to
 * the type rather than buried in the caller. A zeroed scope is legal and means
 * "settle the edges of the files named here, and nothing else"; a zeroed scope
 * with no files settles nothing, which is exactly what an unchanged pass wants
 * and is why the pass does not call it at all in that case. */
typedef struct atlas_code_resolve_scope {
    const char *names; /* NUL-separated, no trailing separator required */
    size_t names_len;
    const int64_t *files; /* code_files ids */
    size_t file_count;
    bool file_set_changed;
    bool full;
} atlas_code_resolve_scope;

atlas_status atlas_code_resolve(atlas_db *db, int64_t repo_id, int64_t generation,
                                const atlas_code_resolve_scope *scope,
                                atlas_code_pass_summary *sum, atlas_err *err);

/* --- bounded traversal --------------------------------------------------------
 *
 * Dependency and impact questions are the same breadth-first walk over the same
 * edges, in opposite directions. One implementation, because two would answer
 * differently the first time somebody fixed a bug in only one of them.
 *
 * Everything about it is bounded and stated: a caller-selected depth clamped to
 * a hard maximum, a node ceiling, cycle detection by a visited set, and a
 * deterministic order — depth, then the edge's resolution, then path bytes — so
 * the same graph produces the same answer whatever order it was built in.
 *
 * **An impact result is a graph path, not a prediction.** It says there is a
 * chain of recorded relations from the thing you named to this node, and it
 * shows the chain. It does not say the node will fail to build, will be
 * rebuilt, or will execute. */

typedef struct atlas_code_walk_opts {
    atlas_code_node_kind start_kind; /* file or symbol */
    int64_t start_id;
    /* Inbound answers "what depends on this" and "what may call this"; outbound
     * answers "what does this depend on". */
    bool inbound;
    int64_t depth;     /* clamped to ATLAS_CODE_MAX_TRAVERSAL_DEPTH */
    int64_t max_nodes; /* clamped to ATLAS_CODE_MAX_TRAVERSAL_NODES */
    bool follow_files;   /* file_depends_on_file */
    bool follow_symbols; /* symbol_calls_symbol */
} atlas_code_walk_opts;

void atlas_code_walk_opts_init(atlas_code_walk_opts *o);

/* One reached node. Borrowed for the callback only, like every row callback. */
typedef struct atlas_code_walk_row {
    int64_t depth;
    const char *node_kind;
    int64_t node_id;
    /* The repository-relative path for a file node, or the symbol's name for a
     * symbol node. Already in the safe encoding. */
    const char *label;
    /* Why this node is here: the edge kind that reached it, the node it was
     * reached from, and the weakest resolution class on the whole path. A
     * candidate reached through one ambiguous edge is an ambiguous candidate
     * however exact the rest of the chain was. */
    const char *edge_kind;
    const char *via_label;
    const char *resolution;
    const char *detail;
} atlas_code_walk_row;

typedef atlas_status (*atlas_code_walk_cb)(const atlas_code_walk_row *row, void *ud,
                                           atlas_err *err);

typedef struct atlas_code_walk_summary {
    int64_t visited;
    int64_t emitted;
    /* The reached set split by the weakest resolution on the path, because
     * merging them would be exactly the conflation A3 exists to prevent. */
    int64_t exact;
    int64_t unique_lexical;
    int64_t ambiguous;
    int64_t unresolved;
    bool truncated;
    const char *truncated_reason;
} atlas_code_walk_summary;

atlas_status atlas_code_walk(atlas_db *db, int64_t repo_id, const atlas_code_walk_opts *opts,
                             atlas_code_walk_cb cb, void *ud, atlas_code_walk_summary *sum,
                             atlas_err *err);

/* --- currency -----------------------------------------------------------------
 *
 * The one claim a caller acts on, computed in one place rather than
 * reconstructed by every consumer from the flags. Exactly the shape
 * `atlas_server_index_current` has, and for the same reason.
 *
 * The structural index is current only when the file index is — a graph built
 * from a file index nobody can vouch for is not a graph anybody should act on —
 * *and* the structural generation equals the file generation, *and* nothing
 * degraded it. `*reason_out` is one of a fixed set of Atlas-authored strings, or
 * NULL when it is current. */
bool atlas_code_index_current(const atlas_index_state *file_state,
                              const atlas_code_index_state *code_state, bool file_current,
                              const char **reason_out);
/* True when `reason` is one of the strings above. Checked before a reason is
 * reported, so a value from anywhere else becomes "other" rather than being
 * reproduced. */
bool atlas_code_not_current_reason_is_known(const char *reason);
const char *atlas_code_not_current_reason_intern(const char *reason);

#endif /* ATLAS_CODE_H */
