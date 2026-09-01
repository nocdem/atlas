/* Atlas - A12.1: the reconciled-memory vocabularies.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * ## What problem this solves
 *
 * A model is handed memory files -- `CLAUDE.md`, a directory of notes -- and it
 * reads them as though they were project truth. They are not. They are what
 * somebody asserted at some point about a tree that has moved since, and Atlas
 * has no way to say which assertions still hold.
 *
 * A12.1's sentence is
 *
 *     MODEL MEMORY IS AN ATTESTATION, NOT PROJECT TRUTH.
 *
 * so every assertion Atlas extracts from a memory file becomes an A9.2 claim
 * carrying its own provenance, verified by the machinery that already exists,
 * rather than a second truth store beside the index. Nothing in this layer
 * invents an authority: the claim, the attestation and the evidence are A9.2's,
 * the read path is A13's, and the pass shape is A1's.
 *
 * ## What is in this header
 *
 * The five closed vocabularies the layer is built from, the policy value
 * grammar, and the typed reads over migration 29's tables that other layers
 * need — today exactly one, the version lookup the verification write point
 * resolves a snapshot reference through. The vocabularies are here rather than
 * beside their consumers because a vocabulary with two homes is a vocabulary
 * with two spellings, and every one of these is stored in a column with a
 * `CHECK` constraint that has to agree with it exactly.
 *
 * ## The house rules that govern all five
 *
 * **UNKNOWN is zero.** A `memset` must never produce a member that asserts
 * something -- A6 keeps UNKNOWN and BLOCKED at zero, A8 keeps DISABLED there,
 * A9.2 follows, and so does this.
 *
 * **UNKNOWN never parses.** Zero means "nobody filled this in". A parser that
 * accepted the spelling would let a caller *store* the absence of a statement as
 * though it were a statement, which is the one thing the zero exists to make
 * impossible. `..._name` still returns "UNKNOWN", because a zero that reached a
 * renderer has to say so rather than name a member that asserts something --
 * the two halves are different claims and both are enforced.
 *
 * **Every switch over one of these has no `default:`.** Deliberately: adding a
 * member later must fail the build at every site that has to decide about it,
 * rather than falling into a silent branch that happens to compile. The build
 * carries `-Wswitch-enum`, so a missing case is an error and not a hope.
 */
#ifndef ATLAS_MEMORY_H
#define ATLAS_MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "atlas/buf.h"
#include "atlas/error.h"

/* Forward-declared, `verify.h`'s precedent: this header needs only a pointer,
 * and declaring it here keeps the dependency one-way. */
typedef struct atlas_db atlas_db;

/* Forward-declared rather than included, and the direction is the whole point.
 *
 * `syspolicy.h` needs the **complete** `atlas_memory_source_class` for a struct
 * member, so it includes this header. This header needs only a pointer to the
 * struct that header defines, so an incomplete type is enough — and declaring it
 * here is what keeps the dependency one-way instead of a cycle. `verify.h` does
 * the same for `atlas_db`, for the same reason. The struct is defined once, in
 * `syspolicy.h`, because that is where a policy's parsed shape lives. */
typedef struct atlas_syspolicy_memory_source atlas_syspolicy_memory_source;

/* Forward-declared for the same reason and by the same rule: T6's reader needs
 * only a pointer to a repository row, so an incomplete type keeps this header's
 * dependency on `db.h` one-way. `src/memory/read.c` includes `atlas/db.h` for
 * the complete definition, exactly as it includes `atlas/mirror.h` for the A13
 * routing it never restates. */
typedef struct atlas_repo_info atlas_repo_info;

/* What a registered memory source is, and -- the load-bearing half -- **who can
 * read it**.
 *
 * A REPO_ class names a path inside a registered repository, so the bytes are
 * reached exactly the way Atlas reaches every other file in that tree: A13
 * decides whether that is the daemon directly or a scanner's mirror, one layer
 * out, and this layer asks rather than deciding again. An EXTERNAL_ class names
 * an absolute path outside every repository, which is a different question about
 * a different reader and is why the distinction is in the vocabulary instead of
 * being inferred from whether a path starts with a slash. */
typedef enum atlas_memory_source_class {
    /* Never parses, never stored. */
    ATLAS_MEMORY_SOURCE_UNKNOWN = 0,
    ATLAS_MEMORY_SOURCE_REPO_FILE,
    ATLAS_MEMORY_SOURCE_REPO_DIR,
    ATLAS_MEMORY_SOURCE_EXTERNAL_FILE,
    ATLAS_MEMORY_SOURCE_EXTERNAL_DIR
} atlas_memory_source_class;

const char *atlas_memory_source_class_name(atlas_memory_source_class c);
bool atlas_memory_source_class_parse(const char *name, atlas_memory_source_class *out);

/* True for REPO_FILE and REPO_DIR -- the two classes the daemon may read
 * itself. The one implementation of "who reads this": a second copy of this
 * predicate somewhere else is how a repository-relative path ends up being
 * opened as an absolute one. */
bool atlas_memory_source_class_is_repo(atlas_memory_source_class c);

/* What a reconciliation pass found had happened to one remembered assertion
 * since the last generation.
 *
 * Two of these are about the *text*, five are about the *world*, and one is
 * neither -- the split is the point. ADDED and CHANGED say the memory file
 * itself moved. SUPPORTED, CONTRADICTED, STALE, IMPACTED and SUPERSEDED say
 * the tree moved underneath an assertion whose text did not -- which is
 * exactly the condition a model reading a memory file cannot detect for
 * itself, and the reason this season exists.
 *
 * UNDETERMINED is the one that is neither, and is not the zero either. It
 * sits outside both groups: it is not a claim about the text or a claim about
 * the world, it is Atlas having looked in this generation and not settled
 * which. The zero, ATLAS_MEMORY_DIFF_UNKNOWN, means "nobody filled this in"
 * and must never parse; UNDETERMINED means somebody looked and came back with
 * no verdict, and it parses like every other member. Placed after the seven
 * existing non-zero members so no stored ordinal moves. */
typedef enum atlas_memory_diff_kind {
    ATLAS_MEMORY_DIFF_UNKNOWN = 0,
    ATLAS_MEMORY_DIFF_ADDED,
    ATLAS_MEMORY_DIFF_CHANGED,
    ATLAS_MEMORY_DIFF_SUPPORTED,
    ATLAS_MEMORY_DIFF_CONTRADICTED,
    ATLAS_MEMORY_DIFF_STALE,
    ATLAS_MEMORY_DIFF_IMPACTED,
    ATLAS_MEMORY_DIFF_SUPERSEDED,
    ATLAS_MEMORY_DIFF_UNDETERMINED
} atlas_memory_diff_kind;

const char *atlas_memory_diff_kind_name(atlas_memory_diff_kind k);
bool atlas_memory_diff_kind_parse(const char *name, atlas_memory_diff_kind *out);

/* What in the repository a proposition is anchored to, and therefore what
 * moving would bear on it.
 *
 * An anchor is how an assertion stops being free-floating prose: it is the
 * reason Atlas can say a remembered sentence has been overtaken without
 * understanding the sentence. Four kinds, because those are the four things
 * Atlas already tracks the movement of -- a path, a symbol, a decision document
 * and a commit. */
typedef enum atlas_memory_anchor_kind {
    ATLAS_MEMORY_ANCHOR_UNKNOWN = 0,
    ATLAS_MEMORY_ANCHOR_PATH,
    ATLAS_MEMORY_ANCHOR_SYMBOL,
    ATLAS_MEMORY_ANCHOR_DECISION,
    ATLAS_MEMORY_ANCHOR_COMMIT
} atlas_memory_anchor_kind;

const char *atlas_memory_anchor_kind_name(atlas_memory_anchor_kind k);
bool atlas_memory_anchor_kind_parse(const char *name, atlas_memory_anchor_kind *out);

/* Whether a frozen Context Pack still describes the world it was frozen
 * against. STALE is a fact about the pack, never a judgement about the work
 * done with it -- A6's rule about a stale decision, one layer out. */
typedef enum atlas_memory_pack_status {
    ATLAS_MEMORY_PACK_UNKNOWN = 0,
    ATLAS_MEMORY_PACK_CURRENT,
    ATLAS_MEMORY_PACK_STALE
} atlas_memory_pack_status;

const char *atlas_memory_pack_status_name(atlas_memory_pack_status s);
bool atlas_memory_pack_status_parse(const char *name, atlas_memory_pack_status *out);

/* Why a generation of the reconciled memory was produced.
 *
 * Recorded rather than inferred, because "the memory file changed" and "the code
 * the memory file talks about changed" produce the same rows and mean entirely
 * different things to whoever reads the diff. */
typedef enum atlas_memory_gen_cause {
    ATLAS_MEMORY_CAUSE_UNKNOWN = 0,
    ATLAS_MEMORY_CAUSE_SOURCE_REVISION,
    ATLAS_MEMORY_CAUSE_DECISION_REVISION,
    ATLAS_MEMORY_CAUSE_COMMIT
} atlas_memory_gen_cause;

const char *atlas_memory_gen_cause_name(atlas_memory_gen_cause c);
bool atlas_memory_gen_cause_parse(const char *name, atlas_memory_gen_cause *out);

/* --- the policy value grammar ---------------------------------------------
 *
 * One `memory_source` line's value, which is
 *
 *     CLASS[@repository]:path
 *
 * Split at the first `:`; the head is split at the first `@`. `repository` is a
 * registered repository's name and its absence means *every* registered
 * repository, which is the ordinary case: an operator naming `CLAUDE.md` means
 * the one in whichever tree is being reconciled.
 *
 * **Refused rather than repaired, everywhere.** A repository path is relative
 * and an external path is absolute; a `..` component is refused; the
 * repository's own `.git` is not a memory source; a value that does not fit the
 * fields is refused rather than truncated, because a silently shortened path
 * names a different file and nothing downstream could tell. The `..` rule is
 * about a path **component**, so `a..b.md` is an ordinary filename and is
 * accepted — a parser reaching for `strstr` there is the obvious bug and the
 * suite has the case for it.
 *
 * Exposed rather than hidden inside the loader **so it can be tested at all**.
 * `atlas_syspolicy_load_at` reaches its file through `atlas_rootpath_open`,
 * which requires every path component from `/` to be root-owned; no test process
 * that is not root can construct such a file anywhere, so a grammar reachable
 * only through the loader would be a grammar no test could enumerate. This is
 * the shape P0 used for `atlas_syspolicy_watch_budget_in_range`.
 *
 * `val` is `len` raw bytes and is **not** required to be NUL-terminated. Paths
 * are bytes, not text: nothing here assumes UTF-8 and nothing splits on
 * whitespace. A value carrying a NUL byte is refused, because storing it would
 * silently shorten the path to the NUL.
 *
 * On refusal `*out` is zeroed, so a caller that ignored the return value holds
 * `ATLAS_MEMORY_SOURCE_UNKNOWN` rather than a half-filled source. */
bool atlas_memory_source_value_parse(const char *val, size_t len,
                                     atlas_syspolicy_memory_source *out);

/* --- the stored snapshot, resolved for the write point ---------------------
 *
 * One `memory_source_versions` row joined with the `memory_sources` row that
 * owns it — the shape `op_evidence_add` needs when an op names a snapshot
 * instead of an indexed path: everything the evidence will assert about the
 * bytes comes from this row, which Atlas wrote when it read them, and nothing
 * comes from the request. Owned buffers; `_init`/`_free` pair. */
typedef struct atlas_memory_version_row {
    int64_t id;
    int64_t source_id;
    int64_t repo_id;          /* the source's repository — a plain id on the
                               * row, because registry churn deletes no memory
                               * history and a join through `repositories`
                               * would */
    atlas_buf version_uid;    /* 'v' + 32 lowercase hex */
    atlas_buf commit_oid;     /* empty when the version was not git-bound;
                               * "this had no blob" and "nobody looked" are
                               * different facts and both are stored */
    atlas_buf content_sha256; /* the hash Atlas computed when it read the bytes */
    int64_t content_bytes;
    atlas_buf path_text;      /* the source's stored %XX-encoded path */
    atlas_buf observed_at;    /* when the bytes described, not when recorded */
} atlas_memory_version_row;

void atlas_memory_version_row_init(atlas_memory_version_row *r);
void atlas_memory_version_row_free(atlas_memory_version_row *r);

/* Resolves one stored memory source version by its public uid. A uid that
 * resolves nothing is `*found_out = false` and ATLAS_OK — the caller decides
 * what an absent row means, exactly as `atlas_db_verify_file_hash` leaves that
 * decision to its caller. Lives in `src/db/db_memory.c`, whose functions are
 * the only production writers of the `memory_*` tables. */
atlas_status atlas_db_memory_version_by_uid(atlas_db *db, const char *uid,
                                            atlas_memory_version_row *out, bool *found_out,
                                            atlas_err *err);

/* --- T6: reading a source, by the principal that can read it ---------------
 *
 * A registered source names bytes; it does not hand them over. `src/memory/
 * read.c` is the one place that turns a source into the bytes it names, and
 * it does so by asking A13's own routing (`atlas_repo_open_git`) rather than
 * restating it: every principal but a named scanner reads a `REPO_*` source
 * from the mirror A13 already decided on, and this layer never opens the tree
 * itself or falls back to it.
 *
 * `ATLAS_MEMORY_READ_UNKNOWN` is the zero, asserts nothing, and is never the
 * outcome of a completed read. */
typedef enum atlas_memory_read_outcome {
    ATLAS_MEMORY_READ_UNKNOWN = 0,
    ATLAS_MEMORY_READ_OK,
    ATLAS_MEMORY_READ_ABSENT,       /* the path is not there. Not an error. */
    ATLAS_MEMORY_READ_TOO_LARGE,    /* over ATLAS_MEMORY_MAX_SOURCE_BYTES; no bytes returned */
    ATLAS_MEMORY_READ_NOT_OURS,     /* EXTERNAL_*: another principal reads it; caller
                                       uses the latest stored version instead */
    ATLAS_MEMORY_READ_NO_MIRROR,    /* A13: a scanner is named and no complete mirror exists */
    ATLAS_MEMORY_READ_SYMLINK       /* the registered path is a symlink; refused, never followed */
} atlas_memory_read_outcome;

/* One entry read from one registered source. A REPO_FILE/EXTERNAL_FILE source
 * yields exactly one of these, whatever its outcome — a missing, oversized or
 * symlinked path is still a fact about that one source and is reported as one
 * item rather than as a silent empty result. A REPO_DIR/EXTERNAL_DIR source
 * yields up to ATLAS_MEMORY_MAX_DIR_ENTRIES of these, sorted by name, when its
 * own path opens as a directory; when it does not (absent, a symlink, or
 * behind an A13 refusal), it too yields exactly one item describing that,
 * consistent with the FILE case rather than a second contract next to it. */
typedef struct atlas_memory_read_item {
    atlas_buf rel_path;      /* the child name for a *_DIR source; empty for *_FILE */
    atlas_buf bytes;
    atlas_buf blob_oid;      /* empty when untracked or external */
    atlas_buf commit_oid;    /* empty when untracked or external */
    atlas_memory_read_outcome outcome;
} atlas_memory_read_item;

void atlas_memory_read_item_init(atlas_memory_read_item *it);
void atlas_memory_read_item_free(atlas_memory_read_item *it);

/* Reads a REPO_* source's current bytes through atlas_repo_open_git, so A13's
 * routing applies without this file restating it. EXTERNAL_* returns NOT_OURS
 * and reads nothing. Never called inside a transaction.
 *
 * `items` must hold at least `cap` slots and `cap` must be at least 1 — even a
 * FILE source's single result needs a slot. On ATLAS_OK, `*count_out` items
 * were written (each already initialised) and the caller frees each with
 * atlas_memory_read_item_free; on any other status nothing was left for the
 * caller to free. */
atlas_status atlas_memory_read_source(const atlas_repo_info *repo, const char *data_dir,
                                      atlas_memory_source_class cls, const void *path_raw,
                                      size_t path_len, atlas_memory_read_item *items,
                                      size_t cap, size_t *count_out, atlas_err *err);

/* Reads one absolute external path as the invoking principal (the CLI's scan).
 * O_NOFOLLOW at every step; a symlink is SYMLINK, never followed. Same output
 * contract as atlas_memory_read_source. */
atlas_status atlas_memory_read_external(const void *path_raw, size_t path_len, bool is_dir,
                                        atlas_memory_read_item *items, size_t cap,
                                        size_t *count_out, atlas_err *err);

#endif /* ATLAS_MEMORY_H */
