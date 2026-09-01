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
#include "atlas/limits.h"
/* For atlas_verify_claim_semantics and atlas_verify_verifier -- T7's
 * proposition carries both, set by resolve() per Decision 4. verify.h does not
 * include this header (checked), so the dependency still runs one way. */
#include "atlas/verify.h"

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
    ATLAS_MEMORY_READ_ABSENT,       /* the path is not there. Not an error. Produced only when
                                       this process looked at the real thing -- the tree itself,
                                       or a mirror's tracked content, both of which are complete
                                       for what they cover -- and found nothing. */
    ATLAS_MEMORY_READ_TOO_LARGE,    /* over ATLAS_MEMORY_MAX_SOURCE_BYTES; no bytes returned */
    ATLAS_MEMORY_READ_NOT_OURS,     /* EXTERNAL_*: another principal reads it; caller
                                       uses the latest stored version instead */
    ATLAS_MEMORY_READ_NO_MIRROR,    /* A13: a scanner is named and no complete mirror exists */
    ATLAS_MEMORY_READ_SYMLINK,      /* the registered path is a symlink; refused, never followed */
    /* A13. Read from a mirror, and the path is not there in it -- but a
     * mirror's untracked content is built from `git ls-files --others
     * --exclude-standard` (src/git/git.c), which never lists a gitignored
     * path, so the mirror can be missing a path the real tree still holds.
     * ABSENT is a claim this process looked and found nothing; a mirror
     * cannot make that claim about an untracked, ignored path, because it
     * never looked there at all -- A9.2.2's rule, one layer out: no evidence
     * of X is not evidence of no X.
     *
     * A caller MUST NOT treat this as evidence the path was removed, and must
     * not retract, supersede or diff away a claim anchored to it on this
     * outcome alone. T8's reconciliation pass should carry the prior
     * generation's claims for such a path forward undetermined rather than
     * concluding they no longer apply -- the same posture UNDETERMINED
     * already gives a claim this pass could not settle. Never produced for a
     * tree-direct read, and never for a tracked path (a mirror always carries
     * every tracked path, gitignore or not). */
    ATLAS_MEMORY_READ_NOT_MIRRORED
} atlas_memory_read_outcome;

/* One entry read from one registered source. A REPO_FILE/EXTERNAL_FILE source
 * yields exactly one of these, whatever its outcome — a missing, oversized or
 * symlinked path is still a fact about that one source and is reported as one
 * item rather than as a silent empty result. A REPO_DIR/EXTERNAL_DIR source
 * yields up to ATLAS_MEMORY_MAX_DIR_ENTRIES of these, sorted by name, when its
 * own path opens as a directory; when it does not (absent, a symlink, or
 * behind an A13 refusal), it too yields exactly one item describing that,
 * consistent with the FILE case rather than a second contract next to it.
 *
 * **A DIR listing where any item's `from_mirror` is true is not a claim of
 * completeness.** A mirror's untracked content is built from `git ls-files
 * --others --exclude-standard` (src/git/git.c), which never lists a
 * gitignored path, so a sibling the tree still holds can be entirely absent
 * from a listing that otherwise opened and read cleanly — with no item at all
 * describing it, unlike the source's own path going missing (which reports
 * ATLAS_MEMORY_READ_NOT_MIRRORED). Every item this reads through one call
 * shares one `from_mirror` value, because the root is one root for the whole
 * listing. A caller — T8 above all — MUST NOT conclude that a remembered
 * child no longer exists solely because it is absent from a mirror-backed
 * listing; the same UNDETERMINED posture ATLAS_MEMORY_READ_NOT_MIRRORED's
 * comment describes applies here; only a listing where `from_mirror` is false
 * (a tree-direct read) may be treated as the complete set. */
typedef struct atlas_memory_read_item {
    atlas_buf rel_path;      /* the child name for a *_DIR source; empty for *_FILE */
    /* The HEAD-blob contract: for a REPO_FILE that is tracked, `bytes` is
     * HEAD's blob content, read through `git cat-file` against the resolved
     * oid -- never the working tree's own copy of the file, even though that
     * copy is what was just opened and size-checked. An uncommitted edit to a
     * tracked memory source is therefore invisible to this read: the caller
     * sees the last committed version until the edit is committed. A REPO_DIR
     * entry, tracked or not, is always the plain filesystem bytes (never
     * checked against a tree at all -- see the class comment above), so this
     * contract binds REPO_FILE only. */
    atlas_buf bytes;
    atlas_buf blob_oid;      /* empty when untracked or external */
    atlas_buf commit_oid;    /* empty when untracked or external */
    atlas_memory_read_outcome outcome;
    /* A13. True when this item's determination -- whatever its outcome --
     * came from reading a scanner's mirror rather than the repository's own
     * tree. False for EXTERNAL_*, for NOT_OURS/NO_MIRROR (nothing was read),
     * and for every atlas_memory_read_external result (no mirror is ever
     * consulted there). See the struct comment above for what this means for
     * a DIR listing specifically. */
    bool from_mirror;
} atlas_memory_read_item;

void atlas_memory_read_item_init(atlas_memory_read_item *it);
void atlas_memory_read_item_free(atlas_memory_read_item *it);

/* Reads a REPO_* source's current bytes through atlas_repo_open_git, so A13's
 * routing applies without this file restating it. EXTERNAL_* returns NOT_OURS
 * and reads nothing. Never called inside a transaction.
 *
 * `items` must hold at least `cap` slots and `cap` must be at least 1 — even a
 * FILE source's single result needs a slot. For a REPO_DIR/EXTERNAL_DIR read,
 * `cap` must not exceed ATLAS_MEMORY_MAX_DIR_ENTRIES either, or the call is
 * refused outright: that constant is this layer's own ceiling on a directory
 * listing, not merely a buffer size a caller happens to have chosen. On
 * ATLAS_OK, `*count_out` items were written (each already initialised) and
 * the caller frees each with atlas_memory_read_item_free; on any other status
 * nothing was left for the caller to free.
 *
 * `from_mirror_out`, when not NULL, is set to whether this call read a
 * scanner's mirror rather than the repository's own tree -- the same fact
 * every produced item's own `from_mirror` carries, but available even when
 * `*count_out` is zero. A DIR source can open a mirror-backed directory
 * cleanly and find no matching entries at all (a tracked non-`.md` file is
 * enough to make the directory exist there; every `.md` in it happened to be
 * gitignored, or there genuinely are none), and an empty result has no item
 * of its own to stamp -- exactly where atlas_memory_read_item's own
 * `from_mirror` runs out of room to carry the fact. Set as soon as
 * atlas_repo_open_git answers -- true for a mirror-backed read whatever it
 * finds, `false` for a tree-direct one and for every case where nothing was
 * read from anywhere at all (NOT_OURS, NO_MIRROR, a caller's own invalid
 * arguments). A caller must treat `*from_mirror_out == true` exactly as
 * atlas_memory_read_item's own comment describes: this source's
 * completeness, empty result included, is not established.
 *
 * **For a `REPO_DIR` read, `from_mirror_out` must not be NULL, and the call
 * is refused outright if it is.** A caller may decline to look at the flag's
 * value; it may not decline to be given it, because an empty or fully
 * filtered mirror-backed listing has no item of its own to fall back to --
 * the caller that skips this parameter on such a read reconstructs exactly
 * the defect this parameter exists to close, silently, on every empty
 * result. A `REPO_FILE` read imposes no such requirement: it always yields
 * exactly one item, and that item's own `from_mirror` field already carries
 * the fact with no result shape that can lose it, so the out-param is a
 * convenience there rather than the only carrier. `EXTERNAL_FILE`/
 * `EXTERNAL_DIR` impose no requirement either, through this function: both
 * return `NOT_OURS` without reading anything, so `from_mirror_out` is always
 * `false` and declining it costs nothing. */
atlas_status atlas_memory_read_source(const atlas_repo_info *repo, const char *data_dir,
                                      atlas_memory_source_class cls, const void *path_raw,
                                      size_t path_len, atlas_memory_read_item *items,
                                      size_t cap, size_t *count_out, bool *from_mirror_out,
                                      atlas_err *err);

/* Reads one absolute external path as the invoking principal (the CLI's scan).
 * O_NOFOLLOW at every step; a symlink is SYMLINK, never followed. Same output
 * contract as atlas_memory_read_source, `from_mirror_out` included -- always
 * `false` here and left optional for both FILE and DIR, since no mirror is
 * ever consulted for an external path: there is no result shape on this path
 * for which the answer could be anything but `false`, so nothing is lost by
 * declining it. */
atlas_status atlas_memory_read_external(const void *path_raw, size_t path_len, bool is_dir,
                                        atlas_memory_read_item *items, size_t cap,
                                        size_t *count_out, bool *from_mirror_out,
                                        atlas_err *err);

/* --- T7: the deterministic extractor and its anchors -----------------------
 *
 * A registered source's bytes are prose. Most of a memory file is not a
 * checkable assertion at all, so this layer's first job is deciding what even
 * *is* a candidate proposition, before anything asks whether it is true.
 *
 * The split (`atlas_memory_extract`) is pure -- no database handle, no
 * process, no file, no clock -- for `src/orch/memory.c`'s reason: a frozen
 * result a reader can re-derive from stored bytes is only checkable if the
 * derivation consulted nothing that moves. Anchor resolution
 * (`atlas_memory_anchor_resolve`) is the impure half and is a separate
 * function for exactly that reason: it reads the index, and it is what T8's
 * apply phase calls from inside the write transaction, where A1 already
 * forbids a git process or a file read. It must never ask git -- every
 * reference is validated against what Atlas has already indexed, the same
 * discipline `src/verify/intake.c` states for the same reason.
 *
 * Normalisation and the anchor syntax are frozen: a change to either bumps
 * `ATLAS_MEMORY_EXTRACTOR_VERSION`, exactly as a change to A3's lexical rules
 * bumps `ATLAS_CODE_ANALYZER_VERSION`. **A candidate that resolves no anchor
 * does not become a claim** -- it is stored as an unanchored proposition and
 * reported, never dropped. Did-not-extract is not proven-to-contain-nothing,
 * A9.2.2's rule one layer out. */

/* What in the repository one proposition is anchored to. `value` is compared
 * exactly, in whichever form the kind's own store uses: `path_text` for PATH,
 * the symbol name for SYMBOL, the public decision uid for DECISION, and the
 * 40-lowercase-hex oid for COMMIT. */
typedef struct atlas_memory_anchor {
    atlas_memory_anchor_kind kind;
    atlas_buf value; /* path_text form, symbol, decision uid, or 40-hex oid */
} atlas_memory_anchor;

void atlas_memory_anchor_init(atlas_memory_anchor *a);
void atlas_memory_anchor_free(atlas_memory_anchor *a);

/* One candidate assertion, split from a source's bytes in document order and
 * -- once `atlas_memory_anchor_resolve` has run -- resolved against the index.
 * Owns its buffers; `_init`/`_free` pair, in the shape every owned-buffer
 * struct in this codebase has. */
typedef struct atlas_memory_proposition {
    size_t ordinal; /* position within the source item; stable across a pass */
    atlas_buf text; /* verbatim bytes, UNTRUSTED_DATA; never normalised */
    atlas_buf normalized; /* for the content key and for Decision 2's edge */
    atlas_buf text_sha256; /* lowercase hex, of `text` */
    atlas_memory_anchor anchors[ATLAS_MEMORY_MAX_ANCHORS_PER_PROPOSITION];
    size_t anchor_count;
    atlas_verify_claim_semantics semantics; /* set by resolve, per Decision 4 */
    atlas_verify_verifier verifier;         /* NONE when nothing mechanical applies */
    atlas_buf verifier_input;
    atlas_buf decision_uid; /* the DECISION anchor's document, when one resolved */
    bool truncated;         /* over ATLAS_MEMORY_MAX_PROPOSITION_BYTES; never trimmed */
} atlas_memory_proposition;

void atlas_memory_proposition_init(atlas_memory_proposition *p);
void atlas_memory_proposition_free(atlas_memory_proposition *p);

/* Pure split: no database, no process, no clock, no file (A10.1's memory.c
 * discipline). Candidates are list items and paragraphs, in document order --
 * a line starting with `-`, `*`, `+` or digits-then-`.`, each followed by
 * whitespace, is its own single-line candidate; a run of other non-blank
 * lines between blank lines (or a list item) is one paragraph candidate,
 * its internal line breaks normalised to `\n` regardless of whether the
 * source used LF or CRLF -- which is what makes CRLF input split identically
 * to LF rather than merely "equivalently".
 *
 * `out` must hold at least `cap` slots; on ATLAS_OK, `*count_out` of them were
 * written (each already initialised, per `atlas_memory_read_source`'s
 * contract) and the caller frees each with `atlas_memory_proposition_free`.
 * The effective limit is `min(cap, ATLAS_MEMORY_MAX_PROPOSITIONS)` -- the
 * policy ceiling is enforced here regardless of how large a buffer a caller
 * happens to pass, because A5's rule governs this bound too: refused, never
 * silently exceeded. A candidate beyond the limit sets `*bound_reached_out`
 * and is not written; a candidate over `ATLAS_MEMORY_MAX_PROPOSITION_BYTES`
 * is written in full (nothing is ever silently trimmed) with `truncated` set.
 *
 * `normalized`, `text_sha256` and `anchor_count` (zero) are set here;
 * `semantics`, `verifier`, `verifier_input` and `decision_uid` are left at
 * their zero defaults until `atlas_memory_anchor_resolve` runs. */
atlas_status atlas_memory_extract(const atlas_buf *bytes, atlas_memory_proposition *out,
                                  size_t cap, size_t *count_out, bool *bound_reached_out,
                                  atlas_err *err);

/* Resolves anchors against the index and assigns semantics and verifier.
 * Separate from the split precisely because the split is pure and this is
 * not: this reads `files` (via `atlas_db_verify_file_hash`, `path_text`
 * compared exactly), the compiler-derived semantic index (via
 * `atlas_db_verify_sem_symbol` -- the same read `atlas.symbol_present` itself
 * runs, which is what keeps extraction-time resolution and later
 * verification looking at the same fact), the decision store (via
 * `atlas_db_decision_find_uid`, scoped to `repo_id`) and `commits` (via
 * `atlas_db_verify_commit_exists`). Index reads only, never git.
 *
 * Idempotent: it resets `p`'s anchors, `semantics`, `verifier`,
 * `verifier_input` and `decision_uid` before scanning `p->text`, so calling it
 * twice on the same proposition against the same index produces the same
 * result. A ninth resolving anchor is dropped; `anchor_count` never exceeds
 * `ATLAS_MEMORY_MAX_ANCHORS_PER_PROPOSITION` and that is the report -- nothing
 * else names the drop because the field already carries it. */
atlas_status atlas_memory_anchor_resolve(atlas_db *db, int64_t repo_id, atlas_memory_proposition *p,
                                         atlas_err *err);

#endif /* ATLAS_MEMORY_H */
