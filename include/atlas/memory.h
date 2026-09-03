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
#include "atlas/sha256.h" /* ATLAS_SHA256_HEX_LEN, for the two digest wrappers below */
/* For atlas_verify_claim_semantics and atlas_verify_verifier -- T7's
 * proposition carries both, set by resolve() per Decision 4. verify.h does not
 * include this header (checked), so the dependency still runs one way. */
#include "atlas/verify.h"

/* Forward-declared, `verify.h`'s precedent: this header needs only a pointer,
 * and declaring it here keeps the dependency one-way. */
typedef struct atlas_db atlas_db;

/* T9 fix-round-1: one lower-case hex encoder, shared rather than each of
 * `src/memory/reconcile.c` and `src/db/db_memory.c` keeping its own copy of
 * the same four-line table. `out` must hold at least `2*n + 1` bytes. */
static inline void atlas_hex_encode_lower(const unsigned char *bytes, size_t n, char *out) {
    static const char D[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2u] = D[bytes[i] >> 4];
        out[i * 2u + 1u] = D[bytes[i] & 0x0fu];
    }
    out[n * 2u] = '\0';
}

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

/* Forward-declared rather than included, and needed only by T8's two phase
 * entry points -- `verify.h`'s own precedent for `atlas_db`, for the same
 * reason. `syspolicy.h` already includes *this* header, because
 * `atlas_syspolicy_memory_source` carries a complete `atlas_memory_source_class`
 * (a struct member, so an incomplete type will not do); if this header
 * included `syspolicy.h` back, the two would cycle. `atlas_memory_observe`
 * and `atlas_memory_apply_in_tx` need only a pointer to the policy struct, so
 * a forward declaration is enough here and keeps the dependency one-way. */
typedef struct atlas_syspolicy atlas_syspolicy;

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
 * SUPERSEDED is the weakest member of that group, and a future author must
 * read this before adding its first producer. Unlike SUPPORTED, CONTRADICTED
 * and STALE it is not itself a `verify_results` verdict -- see
 * `atlas_memory_patch_build`'s own doc comment below for the two ways it is
 * actually derived today, neither of which any verifier establishes -- so
 * `src/memory/patch.c`'s reader deliberately does not gate this kind on
 * `basis`. What it DOES gate every kind on, this one included, is
 * `patch_may_delete`'s three absolutes: DESCRIPTIVE semantics, no
 * IMPLEMENTATION conflict, not stale. A producer added at
 * `reconcile.c:1889-1917` -- the if-chain every existing branch of which is
 * already derived from `evaluate_claim`'s verdict -- would make SUPERSEDED a
 * verdict-derived kind like its four world-group siblings, and would still
 * need exactly those three absolutes; nothing in the *type* enforces that on
 * a future writer, only the reader's own comment does, so read it before
 * writing one.
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
    /* T8. The stored bytes themselves, when the row carries them (NULL/empty
     * `blob_oid`; see the migration's own CHECK). Not read by T4's caller
     * (`op_evidence_add` only needs the hash), but T8's observe phase needs the
     * actual content of an EXTERNAL_* source's *already-stored* latest version
     * to extract propositions from -- a different principal (T11's
     * `memory.put`, not yet built) writes that row, and this is how the pass
     * reads it back. Left unset (empty) by `atlas_db_memory_version_by_uid`'s
     * and `atlas_db_memory_version_latest_meta`'s ordinary callers is fine: an
     * empty buffer for a row that has content is indistinguishable from a row
     * that has none only if a caller conflates "empty" with "absent", which
     * nothing here does -- `content_bytes` is the length that decides that
     * question, not this buffer's own `.len`. */
    atlas_buf content;
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

/* --- T11: memory.put and memory.status's own reads --------------------------
 *
 * `atlas_db_memory_version_by_uid`'s own shape, one row up: a registered
 * source named by its public uid rather than by the (repo, class, path)
 * tuple `atlas_db_memory_source_find` resolves. `memory.put` names a source
 * this way because the request never carries a repository id or a path --
 * §T11's rule that the uid is a reference, never a description a caller could
 * forge. Every `*_out` parameter is optional (NULL skips it), so a caller
 * wanting only the class need not receive the path too. */
atlas_status atlas_db_memory_source_by_uid(atlas_db *db, const char *uid, int64_t *id_out,
                                           int64_t *repo_id_out, atlas_memory_source_class *cls_out,
                                           atlas_buf *path_raw_out, atlas_buf *path_text_out,
                                           bool *found_out, atlas_err *err);

/* Every registered source for one repository, oldest first (`id ASC`) --
 * `memory.status`'s own read, and the first listing this layer has needed:
 * every prior reader already had a source id in hand. `cb` receives one row
 * per source; nothing here opens a transaction or forks a process. */
typedef atlas_status (*atlas_memory_source_cb)(int64_t id, const char *source_uid,
                                               atlas_memory_source_class cls, const char *path_text,
                                               const char *registered_at, void *ctx, atlas_err *err);
atlas_status atlas_db_memory_source_list(atlas_db *db, int64_t repo_id, atlas_memory_source_cb cb,
                                         void *ctx, atlas_err *err);

/* --- T8: the reconciliation pass's own typed operations --------------------
 *
 * All in `src/db/db_memory.c`, which the file header already states is the
 * only production writer of a `memory_*` table. None of these touch a
 * `verify_*` table -- that is `atlas_verify_intake_apply_in_tx`'s job and
 * nobody else's. */

/* Finds a registered source's row by its identity (repo, class, raw path),
 * without creating one. Used by the observe phase to look up an EXTERNAL_*
 * source's already-stored latest version -- T8 never writes during observe,
 * so it must not create the row it is only reading through. `*found_out` is
 * false and everything else untouched when there is no such row yet: nothing
 * has ever been `memory.put` for this policy entry. */
atlas_status atlas_db_memory_source_find(atlas_db *db, int64_t repo_id, atlas_memory_source_class cls,
                                         const void *path_raw, size_t path_raw_len, int64_t *id_out,
                                         atlas_buf *uid_out, bool *found_out, atlas_err *err);

/* Finds a registered source's row, or creates one. The apply phase's own
 * materialisation step: every registered policy entry gets exactly one
 * `memory_sources` row (`UNIQUE(repo_id, cls, path_raw)` is the idempotency),
 * created once and never rewritten -- registry churn deletes no memory
 * history, this migration's own rule. */
atlas_status atlas_db_memory_source_upsert(atlas_db *db, int64_t repo_id, atlas_memory_source_class cls,
                                           const void *path_raw, size_t path_raw_len,
                                           const char *path_text, const char *now, int64_t *id_out,
                                           atlas_buf *uid_out, atlas_err *err);

/* Whether this source has already recorded this exact content, and if so the
 * most recent such row's identity. The apply phase's "moved content" test:
 * finding a match means nothing changed and no new version row is written;
 * finding none means it is new content and `atlas_db_memory_version_insert`
 * is the next call. Compared by content, not by recency, because one
 * registered `*_DIR` source's several children share one `source_id` and each
 * is versioned independently by what it contains. */
atlas_status atlas_db_memory_version_exists(atlas_db *db, int64_t source_id, const char *content_sha256,
                                            bool *found_out, int64_t *id_out, atlas_buf *uid_out,
                                            atlas_buf *observed_at_out, atlas_err *err);

/* Appends a new version row. `content` is bound NULL when `blob_oid` is
 * non-empty -- migration 29's own `CHECK(blob_oid <> '' OR content IS NOT
 * NULL)`, and git is canonical for a blob-bound version so nothing here
 * duplicates it. */
atlas_status atlas_db_memory_version_insert(atlas_db *db, int64_t source_id, const char *commit_oid,
                                            const char *blob_oid, const char *content_sha256,
                                            int64_t content_bytes, const void *content,
                                            size_t content_len, const char *observed_at,
                                            const char *recorded_at, int64_t read_by_uid,
                                            int64_t *id_out, atlas_buf *uid_out, atlas_err *err);

/* The latest version row for a source, content included -- what the observe
 * phase reads back for an EXTERNAL_* source instead of reading the bytes
 * itself. `*found_out` is false when the source has no version yet. */
atlas_status atlas_db_memory_version_latest(atlas_db *db, int64_t source_id,
                                            atlas_memory_version_row *out, bool *found_out,
                                            atlas_err *err);

/* A12.1 T11 fix round (Important 1). `atlas_db_memory_version_latest`'s own
 * SELECT with `v.content` projected out -- a metadata-only read for a caller
 * that never uses the bytes. `memory.status` polls every registered source's
 * latest version on every call, and nothing ever deletes a `memory_sources`
 * row, so a caller reusing `atlas_db_memory_version_latest` and discarding
 * `.content` still pays for reading and copying up to
 * `ATLAS_MEMORY_MAX_SOURCE_BYTES` off disk per source, every poll, for the
 * life of the repository. `.content` is left empty on `*out`, exactly as
 * `atlas_db_memory_version_by_uid` already leaves it for its own callers.
 * `*found_out` is false when the source has no version yet, and a *read
 * failure* is neither -- it is `atlas_status != ATLAS_OK`, which is the
 * distinction `memory.status`'s own fix round exists for (Important 2). */
atlas_status atlas_db_memory_version_latest_meta(atlas_db *db, int64_t source_id,
                                                 atlas_memory_version_row *out, bool *found_out,
                                                 atlas_err *err);

/* Records one resolved anchor for one claim. `INSERT OR IGNORE`: the same
 * anchor recorded again by an unchanged re-run is not a second row --
 * `UNIQUE(claim_uid, kind, value)` is the idempotency, matching the intake
 * write point's own content-key duplicate discipline one layer up. */
atlas_status atlas_db_memory_anchor_add(atlas_db *db, int64_t repo_id, const char *claim_uid,
                                        atlas_memory_anchor_kind kind, const char *value,
                                        atlas_err *err);

/* T9 fix-round-1: removes exactly one `memory_claim_anchors` row -- the
 * `(claim_uid, kind, value)` tuple `classify_candidate` just used to find
 * `claim_uid` as a predecessor. Never the claim's *other* anchors: a claim
 * with more than one anchor (a bullet naming both a SYMBOL and a DECISION,
 * say) can be superseded on one tuple while its other tuples are exactly
 * what the vanished-anchor sweep still needs to see -- pruning the whole
 * claim broke `test_drift_conflict_leaves_the_decision_untouched` in this
 * fix round's own first attempt, by deleting a SYMBOL anchor the sweep had
 * not yet had the chance to find vanished, before it ever ran.
 *
 * This function's own pruning is still needed even though `verify_claims.
 * superseded_by_claim_id` now has a writer (the whole-branch review's C1 fix:
 * `ATLAS_VERIFY_OP_CLAIM_SUPERSEDE`, `src/verify/intake.c`, called from
 * `classify_candidate` immediately after this call returns) -- the two are
 * different tables answering different questions, and nothing propagates one
 * into the other. Without this call, this specific tuple's row would survive
 * every remint forever, and `atlas_db_memory_anchor_distinct`'s per-pass scan
 * would carry one stale row per `COMMIT`-caused pass a repository has ever
 * seen, on every anchor `classify_candidate` ever used to correlate a remint,
 * rather than exactly the live ones. Called only from `classify_candidate`,
 * never from the vanished-anchor sweep, which has no fresh candidate to
 * confirm succession against. */
atlas_status atlas_db_memory_anchor_prune_one(atlas_db *db, int64_t repo_id, const char *claim_uid,
                                              atlas_memory_anchor_kind kind, const char *value,
                                              atlas_err *err);

/* Records one candidate that resolved no anchor. `INSERT OR IGNORE`:
 * `UNIQUE(source_version_id, ordinal)` makes a re-run over an unchanged
 * version idempotent, exactly as the anchored path is through the intake
 * write point's content keys -- and, since migration 29's own
 * `UNIQUE(source_id, content_sha256, observed_at)` on `memory_source_versions`
 * merges two byte-identical `*_DIR` children into one version row, two
 * children can legitimately share one `source_version_id`, at which point a
 * second candidate landing on an ordinal the first one already used is not
 * new information -- it is the same fact, already recorded.
 *
 * `*landed_out`, when not NULL, says whether this call actually inserted a
 * row (`sqlite3_changes() > 0`) as opposed to being silently ignored by the
 * `UNIQUE` constraint above. **A caller that counts "one candidate
 * unanchored" per call without checking this overstates memory_unanchored's
 * true row count** -- exactly the failure this parameter exists to close, a
 * caller's own C1. */
atlas_status atlas_db_memory_unanchored_add(atlas_db *db, int64_t source_version_id, int64_t ordinal,
                                            const char *text_sha256, const void *text,
                                            size_t text_len, bool *landed_out, atlas_err *err);

/* `max(generation) + 1` for this repository -- Decision 7's monotonic
 * sequence. 1 when this repository has none yet. */
atlas_status atlas_db_memory_generation_next(atlas_db *db, int64_t repo_id, int64_t *next_out,
                                             atlas_err *err);

/* Appends one generation row. Fix round: `trailer_scan_high` is no longer
 * the trailer scan's authoritative cursor -- `repositories.trailer_scan_high`
 * (migration 30, `atlas_db_repo_set_trailer_scan_high`) is, decoupled from
 * this insert so it can advance on a pass that mints no generation at all.
 * This parameter and column are kept purely as an informational record of
 * what the cursor's value was at the moment this generation was minted;
 * nothing reads it back. */
atlas_status atlas_db_memory_generation_insert(atlas_db *db, int64_t repo_id, int64_t generation,
                                               atlas_memory_gen_cause cause,
                                               const char *repo_identity_hash,
                                               const char *head_commit,
                                               const char *decision_set_digest,
                                               const char *source_set_digest,
                                               int64_t trailer_scan_high, const char *created_at,
                                               int64_t *id_out, atlas_err *err);

/* Appends one per-claim diff row for a generation. `INSERT OR IGNORE`:
 * `UNIQUE(generation_id, claim_uid)` -- a freshly appended generation can
 * never already hold one, so this can only ever insert, but the same
 * discipline as the two functions above costs nothing to keep. */
atlas_status atlas_db_memory_claim_diff_add(atlas_db *db, int64_t generation_id, const char *claim_uid,
                                            atlas_memory_diff_kind kind, const char *reason,
                                            atlas_err *err);

/* --- T9: cross-generation reads --------------------------------------------
 *
 * A commit re-mints a claim's row (its content key hashes `basis_commit`,
 * `src/verify/intake.c:643`), so "the same proposition" is not "the same
 * `verify_claims` row" once a repository's head has moved. These five reads
 * are what T9's diff computation uses to answer "is this the same proposition
 * at a new basis, and if not, what changed" from stored facts only -- no git,
 * no file, callable from inside the apply transaction. */

/* The most recently appended generation for a repository, or none. What
 * `atlas_memory_apply_in_tx` compares its freshly computed digests and head
 * against to derive this pass's cause, and what `atlas_memory_plan_for` reads
 * to answer the same question before any pass runs. */
atlas_status atlas_db_memory_generation_latest(atlas_db *db, int64_t repo_id, int64_t *generation_out,
                                               atlas_buf *head_commit_out,
                                               atlas_buf *decision_set_digest_out,
                                               atlas_buf *source_set_digest_out, bool *found_out,
                                               atlas_err *err);

/* A12.1 T16. `memory diff --repo R --generation N`'s one read: every claim-diff
 * row this repository's generation `N` recorded, in insertion order. A pure
 * SELECT -- T17's grep rule is about `INSERT INTO memory_`, and this adds none
 * -- so it is safe beside the writers above despite living in the same file.
 *
 * `*found_out` is whether generation `N` exists for this repository at all,
 * checked independently of whether it produced any diff row: a generation
 * that recorded a source revision with nothing to say about any individual
 * claim is a real, empty answer, and "no such generation" is a different one.
 * `cb` receives one row per diff entry; nothing here opens a transaction or
 * forks a process. */
typedef atlas_status (*atlas_memory_diff_row_cb)(const char *claim_uid, atlas_memory_diff_kind kind,
                                                 const char *reason, void *ctx, atlas_err *err);
atlas_status atlas_db_memory_generation_diffs_list(atlas_db *db, int64_t repo_id, int64_t generation,
                                                   atlas_memory_diff_row_cb cb, void *ctx,
                                                   bool *found_out, atlas_err *err);

/* A12.1 T16. `memory pack --run UID`'s missing half: T13's reliance check
 * (`atlas_db_memory_pack_reliance_set`) writes `reliance_checked`,
 * `reliance_complete` and `reliance_claim_uids`, and nothing outside
 * `db_memory.c` read any of the three before this -- context §7's finding.
 * A pure SELECT, added beside `atlas_db_memory_pack_get` for the identical
 * row it does not itself fetch these three columns of.
 *
 * `*found_out` is false and every out-param left at its zero when no pack row
 * exists for this run at all -- the same "never gathered because never
 * frozen" case `atlas_db_memory_pack_get` reports as not-found. A row that
 * *does* exist but has never been reliance-checked ("--task preview" packs
 * are never checked at all, and a frozen "--run" pack's chain may not have
 * completed yet) reads `*found_out = true`, `*checked_out = false` -- the
 * "never gathered" state, distinguished from "gathered zero"
 * (`*checked_out = true`, `*complete_out = true`, an empty `*claim_uids_out`)
 * and from "truncated" (`*checked_out = true`, `*complete_out = false`).
 * `*claim_uids_out` is reset and left empty rather than NULL when there is
 * nothing to report, `atlas_buf`'s own convention. */
atlas_status atlas_db_memory_pack_reliance_get(atlas_db *db, const char *run_uid, bool *checked_out,
                                               bool *complete_out, atlas_buf *claim_uids_out,
                                               bool *found_out, atlas_err *err);

/* Every distinct (kind, value) anchor ever recorded for this repository --
 * one row per repository fact a memory claim has anchored to, regardless of
 * how many claim uids (across how many remints) share it. The vanished-anchor
 * sweep in `atlas_memory_apply_in_tx` asks this once per pass, and it is what
 * lets a proposition whose only anchor disappeared from the tree (a symbol
 * deleted, a path removed) be re-checked even though this pass's fresh
 * extraction no longer resolves it into anything at all. */
typedef atlas_status (*atlas_memory_anchor_tuple_cb)(atlas_memory_anchor_kind kind, const char *value,
                                                     void *ctx, atlas_err *err);
atlas_status atlas_db_memory_anchor_distinct(atlas_db *db, int64_t repo_id,
                                             atlas_memory_anchor_tuple_cb cb, void *ctx,
                                             atlas_err *err);

/* Every claim uid ever anchored to one exact (repo, kind, value) tuple --
 * across every remint, oldest first (`id ASC`), so a caller wanting "the most
 * recently created one" takes the last callback invocation. */
typedef atlas_status (*atlas_memory_claim_uid_cb)(const char *claim_uid, void *ctx, atlas_err *err);
atlas_status atlas_db_memory_anchor_claim_uids(atlas_db *db, int64_t repo_id,
                                               atlas_memory_anchor_kind kind, const char *value,
                                               atlas_memory_claim_uid_cb cb, void *ctx,
                                               atlas_err *err);

/* T12. Every anchor recorded for one claim uid, in `(kind, value)` order --
 * the reverse of `atlas_db_memory_anchor_claim_uids` above, and what
 * `atlas_memory_pack_build` needs to fold a claim's anchor values into its
 * lexical-overlap text and to find its PATH anchors for `flagged_anchors`.
 * The order is a total order (there is no third column to break a tie on
 * otherwise), which is what keeps two builds over an unchanged set of anchor
 * rows byte-identical. */
atlas_status atlas_db_memory_anchors_for_claim(atlas_db *db, int64_t repo_id, const char *claim_uid,
                                               atlas_memory_anchor_tuple_cb cb, void *ctx,
                                               atlas_err *err);

/* T12. This repository's current total of unanchored candidates
 * (`memory_unanchored`), joined through the source version each belongs to.
 * `atlas_memory_pack_build`'s own `unanchored_count`, and not otherwise
 * exposed: every prior reader of this table had a `source_version_id`
 * already in hand, and this is the first that wants a repository-wide
 * total. */
atlas_status atlas_db_memory_unanchored_count(atlas_db *db, int64_t repo_id, int64_t *count_out,
                                              atlas_err *err);

/* `atlas_db_memory_pack_insert` and `atlas_db_memory_pack_get` are declared
 * beside the `atlas_memory_pack` struct they operate on, further down this
 * file, rather than here -- both need the complete type. */

/* The most recent diff kind ever recorded for one claim uid, across every
 * generation -- never only the last one, because "a claim no event touched
 * gets no row" means the last time this uid changed state can be several
 * generations back. `*found_out` is false when no diff row has ever named
 * this claim uid (it has never transitioned, or was only ever ADDED and
 * ADDED itself is what a caller already knows not to repeat). */
atlas_status atlas_db_memory_claim_diff_last_kind(atlas_db *db, int64_t repo_id, const char *claim_uid,
                                                  atlas_memory_diff_kind *kind_out, bool *found_out,
                                                  atlas_err *err);

/* Whether any file indexed under one registered `REPO_DIR` source's own path
 * (its own path exactly, or one path level below it) carries a content hash
 * this source has no recorded version for -- `atlas_memory_plan_for`'s own
 * "a child changed" signal, index-only, bounded at
 * `ATLAS_MEMORY_MAX_DIR_ENTRIES` files examined. A `REPO_FILE` source needs no
 * equivalent: `atlas_db_verify_file_hash` plus the existing
 * `atlas_db_memory_version_exists` already answer the same question for one
 * path. */
atlas_status atlas_db_memory_dir_hash_mismatch(atlas_db *db, int64_t repo_id, int64_t source_id,
                                               const char *path_text, bool *changed_out,
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

/* Why `atlas_memory_anchor_resolve` left `verifier` at `ATLAS_VERIFIER_NONE`
 * even though a PATH or SYMBOL anchor resolved -- the anchor's own value
 * exists, but could not be turned into a verifier input Atlas will run.
 * That is a different fact from "no mechanical check applies to this
 * proposition" (no PATH or SYMBOL resolved at all, or only DECISION did), and
 * before a value could be refused this way the two were indistinguishable
 * once `verifier` was `NONE` -- A9.2.4's rule one layer down: a candidate
 * that cannot be used is recorded with a reason, never skipped, because a
 * rejected candidate nobody is shown is indistinguishable from one that does
 * not exist. UNKNOWN means "nothing was withheld", not "a reason exists but
 * nobody recorded it" -- it is what every DECISION-only, unanchored or
 * ordinarily-verified proposition carries. */
typedef enum atlas_memory_verifier_withhold_reason {
    ATLAS_MEMORY_WITHHOLD_UNKNOWN = 0,
    /* The anchor's value contains a byte the verifier's `key=value;`
     * grammar cannot represent without ambiguity -- today, `;`, the
     * grammar's only segment delimiter (`src/verify/detverify.c`'s
     * `input_field`). Filtering the byte out would only relocate the
     * ambiguity; refusing the verifier is what keeps the anchor honestly
     * recorded instead. */
    ATLAS_MEMORY_WITHHOLD_GRAMMAR,
    /* The built input (`path=...;sha256=...` or `symbol=...`) would exceed
     * `ATLAS_VERIFY_VERIFIER_INPUT_MAX`. The anchor's own value has no
     * length bound tied to the verifier's grammar. */
    ATLAS_MEMORY_WITHHOLD_TOO_LONG
} atlas_memory_verifier_withhold_reason;

const char *atlas_memory_verifier_withhold_reason_name(atlas_memory_verifier_withhold_reason r);
bool atlas_memory_verifier_withhold_reason_parse(const char *name,
                                                 atlas_memory_verifier_withhold_reason *out);

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
    /* NONE means either that no mechanical check applies to this proposition or
     * that one applied and was withheld. `verifier_withheld_reason` below is
     * what tells the two apart, and they are different facts. */
    atlas_verify_verifier verifier;
    atlas_buf verifier_input;
    atlas_buf decision_uid; /* the DECISION anchor's document, when one resolved */
    bool truncated;         /* over ATLAS_MEMORY_MAX_PROPOSITION_BYTES; never trimmed */
    /* UNKNOWN unless `verifier == ATLAS_VERIFIER_NONE` because a resolved
     * PATH or SYMBOL anchor's value was refused rather than used -- see the
     * type's own comment. Never set when no such anchor resolved: that case
     * is "no mechanical check applies" and carries UNKNOWN like every other
     * unwithheld proposition. */
    atlas_memory_verifier_withhold_reason verifier_withheld_reason;
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
 * `semantics`, `verifier`, `verifier_input`, `decision_uid` and
 * `verifier_withheld_reason` are left at their zero defaults until
 * `atlas_memory_anchor_resolve` runs. */
atlas_status atlas_memory_extract(const atlas_buf *bytes, atlas_memory_proposition *out,
                                  size_t cap, size_t *count_out, bool *bound_reached_out,
                                  atlas_err *err);

/* Resolves anchors against the index and assigns semantics, verifier and --
 * when a resolved PATH or SYMBOL anchor's value could not be turned into one
 * -- the reason the verifier was withheld. Separate from the split precisely
 * because the split is pure and this is not: this reads `files` (via
 * `atlas_db_verify_file_hash`, `path_text` compared exactly), the compiler-
 * derived semantic index (via `atlas_db_verify_sem_symbol` -- the same read
 * `atlas.symbol_present` itself runs, which is what keeps extraction-time
 * resolution and later verification looking at the same fact), the decision
 * store (via `atlas_db_decision_find_uid`, scoped to `repo_id`) and `commits`
 * (via `atlas_db_verify_commit_exists`). Index reads only, never git.
 *
 * A proposition already over `ATLAS_MEMORY_MAX_PROPOSITION_BYTES`
 * (`p->truncated`) is returned unscanned, resolved into nothing: it is left
 * exactly as an unanchored proposition, because scanning up to 256 KiB of
 * text for anchor-shaped byte runs would cost roughly one indexed read per
 * such run inside the write transaction this is called from.
 *
 * Idempotent: it resets `p`'s anchors, `semantics`, `verifier`,
 * `verifier_input`, `decision_uid` and `verifier_withheld_reason` before
 * scanning `p->text`, so calling it twice on the same proposition against the
 * same index produces the same result. A ninth resolving anchor is dropped;
 * `anchor_count` never exceeds `ATLAS_MEMORY_MAX_ANCHORS_PER_PROPOSITION` and
 * that is the report -- nothing else names the drop because the field already
 * carries it. */
atlas_status atlas_memory_anchor_resolve(atlas_db *db, int64_t repo_id, atlas_memory_proposition *p,
                                         atlas_err *err);

/* --- T8: the pass ------------------------------------------------------------
 *
 * Observe outside a transaction, apply inside one -- Decision 9, and the exact
 * reason an earlier draft that read, extracted and submitted "inside the
 * caller's transaction" was rejected in review: reading a source is a file
 * open and a `git cat-file` process, and A1 forbids both inside a write
 * transaction.
 */

/* One item read from one registered source, plus what the observe phase
 * derived from it before any transaction opened: T7's split (anchors not yet
 * resolved -- that is the apply phase's own read of the index) and, for a
 * `REPO_*` item whose bytes were actually read, the whole item's own content
 * hash -- distinct from any one proposition's `text_sha256`, and what decides
 * whether the apply phase writes a new `memory_source_versions` row at all.
 *
 * `bytes` is kept, not freed after extraction, when `blob_oid` is empty:
 * exactly the versions with no blob carry their own bytes (migration 29's
 * `CHECK(blob_oid <> '' OR content IS NOT NULL)`), and only the apply phase
 * -- database work, inside the transaction -- may write them. A git-tracked
 * item's `bytes` is freed once extraction has read them, because git is
 * canonical for it and the version row will carry no `content` at all. */
typedef struct atlas_memory_observed_item {
    atlas_buf rel_path;  /* the DIR child's own name; empty for a FILE source */
    atlas_buf blob_oid;  /* empty for untracked/DIR-child/EXTERNAL content */
    atlas_buf commit_oid;
    atlas_buf bytes;       /* freed once read when blob_oid is non-empty; see above */
    atlas_buf content_sha256; /* of `bytes` as actually read, computed in observe */
    int64_t content_bytes; /* the length observed, kept even after `bytes` is freed */
    atlas_memory_read_outcome outcome;
    bool from_mirror;
} atlas_memory_observed_item;

/* Everything the observe phase learned about one registered source, ready for
 * the apply phase to act on. `path_raw`/`path_text` are the source's own
 * registered path (a directory's own path for a `*_DIR` class, not any
 * child's); `items`/`item_count` are that source's `atlas_memory_read_source`
 * result (a `*_FILE` source's is always exactly one, at `items[0]`).
 *
 * `candidates`/`candidate_count` are T7's split, **pooled across every item of
 * this one source** and bounded together at `ATLAS_MEMORY_MAX_PROPOSITIONS` --
 * that constant's own comment reads "per source, per pass", so a `*_DIR`
 * source's children share one budget rather than each getting their own.
 * `candidate_item[k]` is the index into `items[]` the `k`th candidate was
 * split from, which is what lets the apply phase find the right item's own
 * path and version identity for a candidate pooled this way. */
typedef struct atlas_memory_observed_source {
    atlas_memory_source_class cls;
    atlas_buf path_raw;
    atlas_buf path_text;
    /* The call-level flag from `atlas_memory_read_source`'s own `from_mirror_out`
     * -- kept beside each item's own copy because an empty or fully-filtered
     * `*_DIR` listing has no item of its own to carry it (memory.h's own
     * comment on that parameter). Always false for an EXTERNAL_* source: T8
     * never reads one itself. */
    bool from_mirror;

    size_t item_count;
    atlas_memory_observed_item items[ATLAS_MEMORY_MAX_DIR_ENTRIES];

    size_t candidate_count;
    atlas_memory_proposition candidates[ATLAS_MEMORY_MAX_PROPOSITIONS];
    uint8_t candidate_item[ATLAS_MEMORY_MAX_PROPOSITIONS];
    /* This source's own share of ATLAS_MEMORY_MAX_PROPOSITIONS was reached --
     * reported, never silent, A5's rule. */
    bool bound_hit;

    /* EXTERNAL_*: T8 never reads one itself (a different principal does, and
     * writes it through T11's `memory.put`, not yet built). `external_latest`
     * is the already-stored `memory_source_versions` row this pass treats as
     * its observation for this source -- read, not read *from* -- and its own
     * `.content` is what `candidates` above was split from. Unset
     * (`external_latest_found == false`) when no `memory_sources` row for this
     * policy entry exists yet: nothing has ever been put, so there is nothing
     * to extract this pass. */
    bool is_external;
    bool external_latest_found;
    atlas_memory_version_row external_latest;
} atlas_memory_observed_source;

/* T9 fix-round-1 (C2). The bounded set of paths a commit range touched,
 * between the last generation this repository recorded and the HEAD this
 * pass observed -- read via `atlas_git_log_since` during observe (never
 * inside the write transaction, A1's rule against a git process in one),
 * over exactly the range `determine_cause`'s own COMMIT branch is naming.
 *
 * Every path is `path_text`-encoded, the representation a PATH anchor's own
 * value and `files.path_text` already use, so a caller compares like for
 * like with no re-encoding. `available` is false whenever there is nothing
 * to compare against (the first generation, or HEAD did not move since the
 * last one) -- a `COMMIT`-caused pass is the only one that ever needs this
 * set at all. `bound_hit` covers two cases the same way, deliberately not
 * distinguished: the walk found more than `ATLAS_MEMORY_MAX_TOUCHED_PATHS`
 * distinct paths, or it could not be read at all (an A13 mirror that is not
 * yet complete, in particular). Both mean this process cannot prove which
 * paths were *not* touched, and "every anchored claim is conservatively
 * touched" is the only sound reading of that -- A9.2.2's asymmetry, one
 * layer over: no evidence of absence is not evidence of absence. */
typedef struct atlas_memory_touched {
    atlas_buf paths[ATLAS_MEMORY_MAX_TOUCHED_PATHS];
    size_t count;
    bool bound_hit;
    bool available;
} atlas_memory_touched;

void atlas_memory_touched_init(atlas_memory_touched *t);
void atlas_memory_touched_free(atlas_memory_touched *t);

/* True when `path_text` is in the touched set, or the set could not prove it
 * was not -- see `bound_hit`'s own comment above for why the second case must
 * answer true rather than false. `t == NULL` (no observation reached the
 * caller at all) answers false: a caller with no set to consult has no COMMIT
 * cause to be conservative about either. */
bool atlas_memory_touched_contains(const atlas_memory_touched *t, const char *path_text);

/* Everything the observe phase learned that a transaction can now act on.
 * Owns its buffers; `_init`/`_free` pair. Nothing in it references a live
 * statement, a git handle or an open fd -- Decision 9's boundary.
 *
 * **This struct is large by construction** (bounded at
 * `ATLAS_MEMORY_MAX_SOURCES` times `ATLAS_MEMORY_MAX_DIR_ENTRIES` items and
 * `ATLAS_MEMORY_MAX_PROPOSITIONS` candidates each -- on the order of a
 * megabyte at the compiled ceilings) precisely because those ceilings are
 * Decision 10's whole argument for why this job's duration is *statable*
 * rather than unbounded. A caller allocates one on the heap, never the
 * stack. */
typedef struct atlas_memory_observation {
    size_t source_count;
    atlas_memory_observed_source sources[ATLAS_MEMORY_MAX_SOURCES];
    atlas_memory_touched touched;
} atlas_memory_observation;

void atlas_memory_observation_init(atlas_memory_observation *o);
void atlas_memory_observation_free(atlas_memory_observation *o);

/* Phase 1. Reads every registered source for one repository through T6, splits
 * and normalises through T7's pure half, and hashes. Creates processes and
 * opens files; the caller must hold NO transaction. For an EXTERNAL_* source it
 * reads nothing and marks the latest stored version as the observation.
 *
 * `pol` supplies the registered sources (`atlas_syspolicy_memory_source_at_checked`);
 * an entry naming a repository other than `repo->name` is skipped -- one
 * observation is always about one repository. `db` is used for read-only
 * lookups only (an EXTERNAL_* source's already-stored latest version; never a
 * write, never inside a transaction this function opens -- it opens none). */
atlas_status atlas_memory_observe(atlas_db *db, const atlas_repo_info *repo,
                                  const char *data_dir, const atlas_syspolicy *pol,
                                  atlas_memory_observation *out, atlas_err *err);

/* Round 3's own rule, corrected in round 4 to name all twelve members
 * rather than generalise over them -- the two a general description left
 * out were the two it got wrong. Three categories, not two, and every
 * member below is tagged with exactly one:
 *
 * WRITE-SIDE, snapshotted before each source in the per-source loop
 * (`atlas_memory_apply_in_tx`, `reconcile.c`) and restored on that source's
 * own failure, because a SQL rollback undoing a source's rows must undo the
 * count alongside them: `versions_added`, `claims_created`,
 * `claims_resolved`, `unanchored`, `intake_bound_hits` (both its sites --
 * the compiled, still-unreachable text-length bound inside one source's own
 * processing, and the per-source obstacle count in the outer loop -- either
 * describe a row this pass wrote or a row it attempted and rolled back,
 * never an observation).
 *
 * WRITE-SIDE, but set exactly once after every source has resolved rather
 * than snapshotted per source, because nothing before that point has
 * committed the rows they describe: `generation` and `diff_rows`.
 *
 * OBSERVATION, never snapshotted or restored: `sources_seen`,
 * `sources_bound_hit`, `read_obstacles`, `last_read_obstacle`. These
 * describe what the *observe* phase saw before any transaction existed;
 * rolling one back on a write failure would erase the fact that this pass
 * looked at the source at all -- the distinction round 1's I1 was raised to
 * create, and round 2's I2 had to correct once already because an earlier
 * loop restored the whole struct wholesale rather than by field.
 *
 * DIAGNOSTIC, never snapshotted or restored for a third, different reason:
 * `last_obstacle`. It is not a running count with a "before" value worth
 * restoring -- it holds whichever obstacle happened most recently, by
 * design, so a source's own failure is exactly the case that must
 * *overwrite* it rather than have an earlier snapshot survive in its
 * place. */
typedef struct atlas_memory_pass_result {
    int64_t generation;          /* write-side, set once; 0 = nothing changed, no generation appended */
    size_t sources_seen;         /* observation */
    size_t versions_added, claims_created, claims_resolved; /* write-side, per source */
    size_t unanchored;           /* write-side, per source */
    size_t diff_rows;            /* write-side, set once, alongside generation */
    size_t intake_bound_hits;    /* write-side, per source -- see the struct comment above */
    bool sources_bound_hit;      /* observation; a bound was reached, reported never silent */
    /* Observation. Review round 1, I1. Every item this pass could not read
     * as a positive fact -- ABSENT is not one of these (that is a real look
     * that found nothing); NO_MIRROR, NOT_MIRRORED, TOO_LARGE, SYMLINK, and
     * an empty mirror-backed `*_DIR` listing are, because each says this
     * process did not see what is actually there. `sources_seen 1,
     * generation 0, read_obstacles 0` and `sources_seen 1, generation 0,
     * read_obstacles 1` are two different repositories -- a healthy one
     * with nothing new, and one this pass could not fully look at -- and
     * before this field they read identically. */
    size_t read_obstacles;
    /* Observation, alongside `read_obstacles` above. Review round 2, I1's
     * residual. `read_obstacles` on its own is a count with no path and no
     * reason -- A9.2.5's own rule is that an obstacle is recorded with its
     * exact cause, not the first reason and no path. This is not the full
     * per-obstacle table that rule ultimately wants (a stated cost, not
     * solved this round); it carries the most recent obstacle's own outcome
     * and source path, so a caller reading a nonzero `read_obstacles` has at
     * least one concrete example rather than only a number.
     *
     * Round 3: the **outcome comes first**, deliberately, and this field can
     * silently truncate -- `snprintf` never overflows the buffer, but a
     * registered path can be up to 512 raw bytes and `%XX`-encoding can
     * triple that (`atlas_syspolicy_memory_source.path`, `syspolicy.h`), so
     * a long path can consume the whole 256 bytes on its own. Ordering the
     * outcome first is what guarantees *it* always survives even when the
     * path does not, since a truncated path is still informative but a
     * truncated-away outcome would leave a reader with nothing that
     * classifies the obstacle at all. Read off the literals rather than
     * measured, which is the weaker claim and the true one: the longest
     * outcome label this field ever carries is
     * "EMPTY_MIRROR_LISTING: " at 22 bytes into 256, so the outcome always
     * survives regardless of what the path segment costs.
     *
     * Both halves are already safe to print or store as-is: the outcome is
     * one of a fixed set of literal C strings (`read_outcome_label`,
     * `reconcile.c`), never repository content, and the path segment is
     * `path_text` -- already `%XX`-encoded by `atlas_path_text_encode`, this
     * codebase's own "safe to print" form for a path. Neither half needs a
     * further `atlas_safe()` pass, and this field's own encoding is
     * satisfied by construction rather than left for a renderer to
     * discover. */
    char last_read_obstacle[256];
    /* Diagnostic -- see the struct's own comment above for why this is a
     * third category rather than write-side or observation. Review round 1,
     * I4. Carries the most recent obstacle's own message, from whichever
     * source this pass most recently had to roll back: a bare count
     * (`intake_bound_hits`) without a cause is "something happened", which
     * A9.2.5 does not accept as a report.
     *
     * Unlike `last_read_obstacle` above, this field carries raw
     * `sqlite3_errmsg`/`atlas_err` text (`reconcile.c`), which is Atlas' own
     * diagnostic prose, not repository content -- but it is not
     * `atlas_safe()`-encoded either, and a caller that renders it to a
     * terminal or a JSON document must not assume it already is. Left as a
     * stated gap rather than solved this round. */
    char last_obstacle[256];
    /* Observation, `sources_bound_hit`'s own shape. T9 fix-round-2 (New
     * Important 3): `atlas_memory_touched.bound_hit` was set at three call
     * sites in `observe_touched_paths` and read only by
     * `atlas_memory_touched_contains`, with a comment claiming it is
     * "reported, never silent" while nothing outside `reconcile.c` could
     * ever see it -- A8-CI's rule ("every bound that is reached is
     * reported") stated but not done. True when the pass could not prove
     * which paths a commit range did *not* touch (the walk's own
     * `ATLAS_MEMORY_MAX_TOUCHED_PATHS` bound, a stale or unknown recorded
     * tip, or the repository could not be opened at all) and therefore
     * treated every `PATH`-anchored claim as conservatively `IMPACTED` this
     * pass rather than checking each one against the set. */
    bool touched_bound_hit;
    /* T14. Write-side, unconditional: this pass's trailer scan runs whatever
     * `ctx.any_change` turns out to be from the claim/anchor loop above,
     * because a commit's trailer block is a fact about `commits`, not about a
     * registered source. `trailer_scanned` is how many commit rows above the
     * cursor this pass examined, bounded at `ATLAS_MEMORY_TRAILER_PASS_MAX`;
     * `trailer_bindings_written` is how many of those *landed* a
     * `memory_trailer_bindings` row (fix round M2: `sqlite3_changes`-counted,
     * never a re-presentation of an already-recorded commit) -- one of
     * `has_block` (a recognised `Atlas-Provenance: v1` block, verified or
     * not) or `bound_hit` (the block search could not fully examine this
     * commit's message -- see `atlas_memory_trailer_binding`'s own comment
     * for the four states this and the cursor together let a reader tell
     * apart). `trailer_scan_bound_hit` is true when more commits remain above
     * what this pass reached.
     *
     * Fix round (Critical 1): the cursor for *this* field is
     * `repositories.trailer_scan_high` (migration 30,
     * `atlas_db_repo_trailer_scan_high`/`_set_trailer_scan_high`,
     * `src/db/db_repo.c`), written after every pass that scanned at all,
     * whether or not `ctx.any_change` ends up true -- decoupled from
     * `memory_generations`, which is a ledger of what a pass *found*, not of
     * how far it looked. A pass that scans 512 blockless commits and changes
     * nothing else still leaves the next one starting at commit 513: finding
     * nothing is no longer the same event as making no progress. See
     * `atlas_memory_apply_in_tx`'s own comment for the exact write. */
    size_t trailer_scanned;
    size_t trailer_bindings_written;
    bool trailer_scan_bound_hit;
} atlas_memory_pass_result;

/* Phase 2. Inside the caller's transaction: database work only. Materialises
 * source rows from the policy, appends version rows for moved content, resolves
 * anchors (T7's impure half), emits the intake ops, writes anchors and
 * unanchored rows, appends the generation and its diff (T9), and advances the
 * trailer cursor (T14's ingest). */
atlas_status atlas_memory_apply_in_tx(atlas_db *db, const atlas_repo_info *repo,
                                      const atlas_memory_observation *obs,
                                      const atlas_syspolicy *pol, const char *now,
                                      atlas_memory_pass_result *out, atlas_err *err);

/* --- T9: does this repository owe a pass, and why --------------------------
 *
 * Pure derivation, asked by the sweep (T10) and by `memory status` (T16) --
 * one function, two askers, A9.2.3's shape. Reads the index only: it compares
 * the same three signals `atlas_memory_apply_in_tx` derives its cause from --
 * each registered source's current content against its latest recorded
 * version, the decision-set digest over every DECISION anchor's effective
 * approved revision, and `repo->scanned_head` -- against the last stored
 * generation row. `*cause_out` is `ATLAS_MEMORY_CAUSE_UNKNOWN` (the zero) when
 * nothing owed, exactly the pass's own precedence otherwise: the first of
 * SOURCE_REVISION, DECISION_REVISION, COMMIT that holds.
 *
 * No repository with no generation yet and no registered source answers
 * UNKNOWN by omission -- it answers UNKNOWN honestly, because there is
 * nothing to compare against and nothing registered to owe a pass over. A
 * registered source with no generation yet and no recorded version answers
 * SOURCE_REVISION: it has never been read, which is indistinguishable from
 * having just changed. */
atlas_status atlas_memory_plan_for(atlas_db *db, const atlas_repo_info *repo,
                                   const atlas_syspolicy *pol, atlas_memory_gen_cause *cause_out,
                                   atlas_err *err);

/* --- T12: two live digests, promoted out of reconcile.c's own file --------
 *
 * `atlas_memory_plan_for` already needed a live decision-set digest and a live
 * source-set digest to compare against the last stored generation, and
 * `src/memory/reconcile.c` computes both as file-local static helpers
 * (`compute_decision_set_digest`, `compute_source_set_digest`). T12's pack
 * needs the *same* two values for the same reason -- a pinned digest is only
 * comparable against a later reader if both sides derive it the same way, and
 * a second implementation of a digest that decides staleness is exactly what
 * A9.2.4 says must not survive. Rather than re-derive either inside pack.c,
 * these two thin wrappers expose the existing static bodies: the digest logic
 * has exactly one implementation, in reconcile.c, unchanged; only its
 * reachability moved. Both are pure DB reads -- `atlas_db_memory_anchor_
 * distinct` plus `atlas_db_decision_approved_revision` for the first, `atlas_
 * db_memory_source_find` plus a stored version lookup for the second -- so
 * both may run inside or outside a transaction; T12's build() calls them
 * outside one only because the same call also needs `atlas_sem_source_
 * identity`, which does not share that freedom. */
atlas_status atlas_memory_decision_set_digest(atlas_db *db, int64_t repo_id,
                                              char out[ATLAS_SHA256_HEX_LEN + 1], atlas_err *err);
atlas_status atlas_memory_source_set_digest(atlas_db *db, const atlas_repo_info *repo,
                                            const atlas_syspolicy *pol,
                                            char out[ATLAS_SHA256_HEX_LEN + 1], atlas_err *err);

/* --- T12: the Canonical Context Pack ---------------------------------------
 *
 * A worker is handed a bounded, frozen excerpt of what Atlas has recorded
 * about a repository's memory claims -- built once per run, checkable for
 * staleness on every later read, and rendered the same way every time the same
 * pinned inputs are asked to produce it.
 *
 * **The corrected split.** An earlier draft of this comment described build
 * and freeze as one claim -- "stored rows only, no process, no file read --
 * which is what lets the freeze run inside the run-creating submit
 * transaction" -- and that sentence is true of `atlas_memory_pack_freeze_in_tx`
 * and false of `atlas_memory_pack_build`. Build fills `source_identity` from
 * `atlas_sem_source_identity` whenever the repository row says the tree is
 * dirty, and that function opens the repository root to read every accepted
 * compilation database (`live_facts`, `src/sem/index.c:92`) -- file I/O, which
 * A1 forbids inside a write transaction ("no git process and no file read
 * happens inside a write transaction"). So:
 *
 *   - `atlas_memory_pack_build` -- stored rows plus, when the repository row
 *     says dirty, one `atlas_sem_source_identity` call that opens the tree.
 *     Must run with **no transaction open**, `atlas_memory_observe`'s own
 *     rule and for the same reason.
 *   - `atlas_memory_pack_freeze_in_tx` -- stored rows only, no read, no
 *     process. It inserts the already-built struct verbatim and belongs
 *     inside the transaction, beside `atlas_db_orch_memory_freeze`
 *     (`src/db/db_orch.c`, in `submit_resolve_run`'s root-task branch).
 *
 * **Where the seam actually is.** The daemon's writer thread dispatches a
 * SUBMIT operation through `run_orch()` (`src/daemon/writer.c`, around line
 * 1192), which calls `atlas_orch_apply(w->db, j->orch, ...)` directly --  and
 * `atlas_orch_apply` opens its transaction on the first line and closes it on
 * the last. There is no point *inside* that dispatch, once the operation has
 * reached the writer thread, at which a file-reading build could run without
 * moving it inside the transaction it opens. The seam this layer needs is one
 * step earlier: `run_orch()` itself, before it calls `atlas_orch_apply`, is a
 * point on the writer thread with no transaction open and `w->db` already in
 * hand -- exactly the shape `atlas_memory_observe` already uses one layer
 * over. Wiring a root-task SUBMIT to call `atlas_memory_pack_build` there and
 * carry the result into `op_submit`'s transaction for `atlas_memory_pack_
 * freeze_in_tx` to consume is T13's task, not this one; this header records
 * the finding so T13 does not have to re-derive it.
 *
 * **Both bounds are refused, never trimmed.** `ATLAS_MEMORY_PACK_MAX_CLAIMS`
 * and `ATLAS_MEMORY_PACK_MAX_BYTES` (`include/atlas/limits.h`) govern the
 * relevant set and the rendered body. A relevant-candidate count or a rendered
 * size over either bound is a refusal from `atlas_memory_pack_build`, not a
 * silent truncation -- `limits.h`'s own comment: a worker must never be shown
 * a pack that silently omits the claim its task turned on.
 *
 * **Relevance is never recency.** Deterministic lexical overlap between the
 * task's tokens and a claim's text plus its anchor values, A10.1's own
 * discipline (`src/orch/memory.c`) applied to claims instead of run goals: a
 * candidate with no shared token is never selected, whatever else is true of
 * it, and two arms of a comparison must differ by exactly the package's bytes,
 * never by which claim happened to be newest.
 *
 * **`compose` appends nothing for an absent piece.** Not a shorter section,
 * not a sentence saying there is none -- A10.1's `OFF` rule verbatim, and the
 * reason is the same: two arms of a comparison must differ by exactly the
 * appended bytes. */
typedef struct atlas_memory_pack {
    int64_t repo_id;
    /* The six pinned inputs freshness compares against. `pinned_commit` and
     * `source_identity` may legitimately be empty (an unborn HEAD; a clean
     * tree) -- an empty stored value never makes a pack stale, A9.2.3's rule
     * carried over unchanged. `decision_set_digest` and `source_set_digest`
     * are always populated, even over zero registered sources or zero
     * DECISION anchors: a digest over an empty set is still a value, and
     * comparing it is still meaningful. */
    atlas_buf repo_identity_hash;
    atlas_buf pinned_commit;
    atlas_buf source_identity;
    int64_t memory_generation;
    atlas_buf decision_set_digest;
    atlas_buf source_set_digest;

    /* The frozen body and its digest. `rendered` carries the fixed Atlas
     * preamble, the selected claims and the fixed postamble -- never the
     * delivery-time status line, which `atlas_memory_pack_compose` adds
     * separately so the stored bytes stay independent of when they are read.
     * `pack_digest` is sha256(rendered), hex. */
    atlas_buf rendered;
    atlas_buf pack_digest;

    /* `claim_count` is the size of the relevant set actually rendered.
     * `excluded_count` counts claims that were *not* selected (zero lexical
     * overlap with the task) whose stored verification state or conflict
     * needed attention regardless -- reported and dropped, never gated on,
     * A10.1's "unrelated stale material" rule. `unanchored_count` is this
     * repository's current total of candidates that resolved no anchor at
     * all (`memory_unanchored`), carried through for the same reason a
     * generation reports it. */
    int64_t claim_count;
    int64_t excluded_count;
    int64_t unanchored_count;

    /* Netstring-encoded (M23's manifest shape: `<n>:` then `n` `<len>:
     * bytes,` records). `claims_manifest` is `claim_count` triples of
     * (claim uid, stored verification state name, "1"/"0" flagged), in the
     * pack's own rendered order. `flagged_anchors` is one triple (claim uid,
     * "PATH", anchor value) per PATH anchor belonging to a flagged claim in
     * the relevant set -- the reliance check's own input, and the reason a
     * SYMBOL- or DECISION-only flagged claim contributes nothing here.
     *
     * "Flagged" (`pack.c`'s `troubled`) is a stored conflict, a stale or
     * contradicted verification state, **or no result at all** -- and a
     * claim's deterministic result is only ever produced when a `COMMIT`
     * pass's own touched-path check reaches its anchor (season-review I2),
     * so "no result yet" is the ordinary state of a claim nothing has
     * separately re-verified, not an edge case. `flagged_anchors` is
     * therefore, in practice, close to every PATH anchor of every relevant
     * claim, and the reliance check built from it reads as "the worker
     * touched a file a relevant memory bullet mentions" more often than as
     * a specific finding about stale or contradicted material
     * (season-review I6, a disclosed consequence rather than a mechanism
     * defect: the check still decides nothing). */
    atlas_buf claims_manifest;
    atlas_buf flagged_anchors;
} atlas_memory_pack;

void atlas_memory_pack_init(atlas_memory_pack *p);
void atlas_memory_pack_free(atlas_memory_pack *p);

/* T12. The row insert for `memory_context_packs`, called by `atlas_
 * memory_pack_freeze_in_tx` and by nothing else -- `db_memory.c`'s own file
 * header states it is the only production writer of a `memory_*` table, and
 * T17's grep is what proves it. `UNIQUE(run_uid)` is the freeze; a second
 * call for the same run_uid fails on the constraint rather than replacing
 * the row. `reliance_checked`, `reliance_complete` and `reliance_claim_uids`
 * take their column defaults (0, 1, '') -- T13's reliance check writes them
 * later, in the completion transaction, and this call has nothing to say
 * about a run that has not started. */
atlas_status atlas_db_memory_pack_insert(atlas_db *db, const char *run_uid,
                                         const atlas_memory_pack *p, const char *now,
                                         atlas_err *err);

/* T12. Reads one frozen pack back by its run uid -- what a later reader
 * (a delivery-time freshness check, `atlas memory pack --run UID`) needs
 * instead of re-running `atlas_memory_pack_build`, which would ask a
 * possibly-moved database a question about the present rather than reporting
 * what was frozen. `*found_out` is false and `*out` is left freshly
 * initialised when no such run has a pack.
 *
 * `*out` must already be an initialised pack, the same precondition
 * `atlas_memory_pack_build` carries and for the same reason: this function's
 * first action is `atlas_memory_pack_free(out)`. */
atlas_status atlas_db_memory_pack_get(atlas_db *db, const char *run_uid, atlas_memory_pack *out,
                                      bool *found_out, atlas_err *err);

/* T13. Records one completion's reliance finding on an already-frozen pack
 * row -- refused, not created, when no such row exists. Merges rather than
 * replaces, because a run's repo-tree chain may complete more than once
 * (A11.6): `reliance_checked` becomes sticky-true, `reliance_complete` is
 * ANDed with whatever was already stored, and `matched_claim_uids` (the
 * netstring-encoded set `atlas_memory_pack_reliance_match` produces) is
 * unioned with whatever was already stored, deduplicated. See the in-code
 * comment at the call site (`src/db/db_orch.c`'s `reliance_check`, inside
 * `op_complete`) for why this write settles nothing: it touches this table
 * and no column any settlement scan reads. */
atlas_status atlas_db_memory_pack_reliance_set(atlas_db *db, const char *run_uid, bool complete,
                                               const char *matched_claim_uids, atlas_err *err);

/* Builds a pack for the given repository's *current* state, deterministically:
 * the same stored rows and the same live reads (an unmoved dirty flag, an
 * unmoved `files` table) produce byte-identical `rendered` and an equal
 * `pack_digest`, whatever order the underlying claim rows happen to be
 * returned in -- the sort is a total order (overlap descending, claim id
 * ascending) with no tie left to chance.
 *
 * `pol` supplies the registered memory sources `atlas_memory_source_set_
 * digest` needs -- the same requirement `atlas_memory_observe` and `atlas_
 * memory_plan_for` already carry, and not present in an earlier draft of this
 * signature; disclosed in the T12 report as the one deviation from the plan's
 * literal interface.
 *
 * **`pol` must be a policy this process loaded through `atlas_syspolicy_load`,
 * and nothing at this function can check that** -- it is a struct, so
 * `state == SYSTEM` is an integer any caller can set, and this sentence is the
 * whole of the defence. Never from a request body, a parameter a client
 * influences, or a struct built by hand outside a test.
 * `atlas_writer_submit_memory_reconcile` (`src/daemon/daemon_internal.h`)
 * carries the same sentence at higher stakes: there a forged policy
 * authorises a whole reconciliation pass. Here `pol` authorises nothing --
 * lower stakes, not none: it decides a verdict a worker is shown. This
 * function pins `source_set_digest` from it, and a wrong policy pins that
 * digest over a source set the operator never registered; handed the same
 * wrong policy, `atlas_memory_pack_freshness` then agrees with itself and
 * reports `CURRENT` over a source set that has in fact moved.
 *
 * Refuses (does not truncate) when more than `ATLAS_MEMORY_PACK_MAX_CLAIMS`
 * claims have positive lexical overlap with `task_text`, when the rendered
 * body would exceed `ATLAS_MEMORY_PACK_MAX_BYTES`, or when one claim has
 * accumulated more than `ATLAS_MEMORY_PACK_MAX_ANCHORS_PER_CLAIM` anchors
 * across the passes that resolved it. That third bound is **picked, not
 * derived** -- what it would be derived from is unbounded in time, because
 * nothing prunes a kept claim's anchors -- and its refusal therefore **has no
 * exit**: it is repository-wide and task-independent. `limits.h`'s comment
 * there states the cost in full and `docs/backlog.md` carries the argument.
 * Must be called with **no transaction open** -- see the section comment
 * above.
 *
 * `*out` must already be an initialised pack (`atlas_memory_pack_init`, or a
 * struct this function or `atlas_db_memory_pack_get` has already filled) --
 * the first thing this function does is `atlas_memory_pack_free(out)`, which
 * dereferences whatever `out` already owns. Passing a merely-zeroed or
 * uninitialised struct is not the same thing; zeroed happens to be safe only
 * because `atlas_buf_free` on a zeroed `atlas_buf` is a no-op, which is not
 * true of an arbitrary uninitialised one. */
atlas_status atlas_memory_pack_build(atlas_db *db, int64_t repo_id, const atlas_syspolicy *pol,
                                     const char *task_text, atlas_memory_pack *out, atlas_err *err);

/* Inserts an already-built pack under `UNIQUE(run_uid)` -- the freeze, and the
 * only thing that makes it one: a second call for the same run_uid is refused
 * by the constraint rather than replacing what an already-leased task may have
 * been shown. Stored rows only in the sense that matters for A1: this function
 * itself reads nothing and asks nothing of git or the filesystem, so it may run
 * inside the transaction that creates the run, beside `atlas_db_orch_memory_
 * freeze`. The row insert itself lives in `atlas_db_
 * memory_pack_insert` (`src/db/db_memory.c`), which this function calls --
 * `db_memory.c`'s own file header states it is the only production writer of
 * a `memory_*` table, and T17's grep is what proves it. */
atlas_status atlas_memory_pack_freeze_in_tx(atlas_db *db, const char *run_uid,
                                            const atlas_memory_pack *p, atlas_err *err);

/* Recomputed on every read; nothing cached, no dirty bit -- A6's rule. Compares
 * each of the six pinned inputs against a freshly read or freshly derived live
 * value, in this order: repository identity, indexed commit, memory
 * generation, decision-set digest, source-set digest, then -- broadest and
 * last, `atlas_sem_freshness_of`'s own ordering argument -- the live source
 * identity, compared only when both the pinned and the live values are
 * non-empty (an empty value on either side pins nothing, A9.2.3's rule
 * unchanged). `*out` is `ATLAS_MEMORY_PACK_CURRENT` when none of the six has
 * moved.
 *
 * On `ATLAS_MEMORY_PACK_STALE`, `which_moved` is reset and filled with
 * `STALE:<NAME>` for the *first* of the six (in the order above) found to have
 * moved -- one of `STALE:REPO_IDENTITY`, `STALE:COMMIT`, `STALE:GENERATION`,
 * `STALE:DECISION_SET`, `STALE:SOURCE_SET`, `STALE:SOURCE_IDENTITY`. On
 * `ATLAS_MEMORY_PACK_CURRENT`, `which_moved` is reset and left empty.
 *
 * `pol` is needed for the same reason `atlas_memory_pack_build` needs it: the
 * live source-set digest is not derivable without the registered source list.
 * This function reads the repository row and, whenever the pack pinned a
 * non-empty `source_identity`, opens the tree (`atlas_sem_source_identity`) --
 * gated on the *pinned* value, never on the repository's current `dirty` flag
 * (`atlas_sem_freshness_of`'s own precedent has no such gate either). A pin
 * taken while dirty and a tree that has since gone clean again -- an
 * uncommitted edit reverted without a commit -- must still be compared, so a
 * pack pinned over content that no longer exists must not read as CURRENT
 * merely because `dirty` now reads false; see the in-code comment at the call
 * site for the reasoning in full. Never called from inside a transaction
 * anywhere in this codebase, and it must not be.
 *
 * **The same `pol` provenance sentence as `atlas_memory_pack_build` applies
 * here, at the same lower-than-T10 stakes.** A wrong policy handed to this
 * function recomputes the live `source_set_digest` over the wrong source set
 * and compares it against whatever `build` pinned; if both calls were handed
 * the same wrong policy, the comparison agrees with itself and this function
 * reports `CURRENT` over a source set that has in fact moved. `pol` must
 * always be one this process loaded through `atlas_syspolicy_load`, never one
 * from a request body, a parameter a client influences, or a struct built by
 * hand outside a test -- nothing at this function can check that either. */
atlas_status atlas_memory_pack_freshness(atlas_db *db, const atlas_syspolicy *pol,
                                         const atlas_memory_pack *p,
                                         atlas_memory_pack_status *out, atlas_buf *which_moved,
                                         atlas_err *err);

/* Renders the body (the stored form, with no status line) -- a copy of
 * `p->rendered`. Its own function, rather than a caller reaching into the
 * struct directly, so a pack materialised from a freshly stored row (`atlas_
 * db_memory_pack_get`) and one still in hand from `atlas_memory_pack_build`
 * are read through one implementation. */
atlas_status atlas_memory_pack_render(const atlas_memory_pack *p, atlas_buf *out, atlas_err *err);

/* The one composer: task, then an optional A10.1 cross-run memory package
 * (blank-line separated, `atlas_orch_memory_compose`'s own shape), then an
 * optional pack section carrying its delivery-time `status_line` ahead of
 * `pack_body`. Appends nothing for an absent piece -- not a shorter section,
 * not a sentence saying there is none: `memory_package == NULL` and `pack_body
 * == NULL` (or either empty) each contribute zero bytes, independently, so
 * `atlas_memory_pack_compose(task, NULL, NULL, NULL)` returns `task` byte for
 * byte. `status_line` is never shown when `pack_body` is absent -- a status
 * line with nothing to describe is not a piece of its own. */
atlas_status atlas_memory_pack_compose(const char *task, const char *memory_package,
                                       const char *status_line, const char *pack_body,
                                       atlas_buf *out, atlas_err *err);

/* --- T13: the reliance check ------------------------------------------------
 *
 * Decision 8's rule: the reliance check reads anchors, never prose. This is the
 * one function that ever compares a pack's `flagged_anchors` against a driver's
 * observed touched-paths list, so it is the one place that rule can be checked
 * once rather than re-argued at every call site.
 *
 * `touched_paths` is already decoded -- an array of `path_text`-encoded buffers,
 * the caller's own `atlas_orch_paths_decode` of `atlas_orch_op.touched_paths` --
 * because this file issues no SQL and depends on no `src/orch` header; decoding
 * that wire format is `src/db/db_orch.c`'s job, exactly as it already decodes
 * `lease_drivers`. A `flagged_anchors` PATH value and a touched path are both
 * `path_text`, so the comparison is an exact byte match, never a normalisation.
 *
 * `*matched_out` is reset and filled with the netstring-encoded (M23's shape:
 * `<count>:` then that many elements) set of distinct claim uids that had at
 * least one PATH anchor equal to some entry of `touched_paths` -- empty, never
 * NULL, when nothing matched. `*any_out` is `*matched_out` non-empty, spelled
 * out separately so a caller never has to re-parse the netstring just to ask
 * "did anything match". Deterministic: the matched set is a function of the two
 * inputs alone, in `flagged_anchors`' own stored order with duplicates
 * collapsed, never of iteration order or of anything read live. */
atlas_status atlas_memory_pack_reliance_match(const atlas_memory_pack *p,
                                              const atlas_buf *touched_paths, size_t touched_count,
                                              atlas_buf *matched_out, bool *any_out, atlas_err *err);

/* --- T14: commit trailers ----------------------------------------------------
 *
 * A trailer is composed for a person (or a driver's final report) to paste
 * into a commit message; Atlas itself commits nothing. It is a *pointer*,
 * never proof and never authority -- every field is verified against Atlas'
 * own rows on ingestion, and a field that does not verify is named in
 * `unknown_fields` and binds nothing: it can never manufacture an approval, a
 * gate result or a verified claim. The frozen six-line format is in
 * `docs/plans/2026-09-01-a12.1-reconciled-memory.md` under "The commit
 * trailer block".
 *
 * **Three of the six lines survive an index rebuild; two do not; the sixth
 * is the fixed marker, never a value.** (Fix round M1: the previous wording
 * here -- "two ... survive; two do not; two are content-derived and always
 * do" -- named the two content-derived fields as if they were a third group
 * disjoint from "survive", which doubles-counts them and reads as four of
 * six surviving; they are two of the three that do.) SQLite is a rebuildable
 * index and git is authoritative (architecture invariant 1) -- a trailer
 * lives in git's permanent record, so a reader needs to know which of its
 * fields still mean something after the index that produced them is gone:
 *
 *   - `Atlas-Context-Digest` and `Atlas-Decision-Set-Digest` are
 *     content-derived (`sha256` of a rendered pack and of a decision-set
 *     tuple, respectively) and verify again against any correctly-rebuilt
 *     index that reproduces the same content, whatever rowids it assigns.
 *   - `Atlas-Memory-Generation` is `memory_generations.generation`, a
 *     per-repository sequence number tied to how many reconciliation passes
 *     have run, not to what they found. A rebuild that replays the same
 *     history will not in general re-run the same passes at the same
 *     moments, so a rebuilt index can legitimately assign a stored commit's
 *     claimed generation to a different pass than the one that produced it,
 *     or to none.
 *   - `Atlas-Change-Reason` is the bare `ai_reasons.id` -- `ai_reasons` has
 *     no uid column (`id INTEGER PRIMARY KEY`, `src/db/migrate.c:492-511`,
 *     and no later migration adds one) -- so this field carries the same
 *     rowid-reassignment exposure `Atlas-Memory-Generation` does. Worse: a
 *     reason row is not rederivable from git at all (nothing in a
 *     repository's history says why an agent made a change), so a database
 *     wipe loses it outright rather than merely renumbering it. Adding a uid
 *     column would fix this and is a migration -- migration 30 shows a fix
 *     round can add one when it must, so the reason this stays undone is not
 *     "no migration was available": it is that `ai_reasons` is A2's table,
 *     not T14's, and widening another season's table for a rebuild-survival
 *     nicety this field can live without is a call for whoever owns it, not
 *     a fix round closing a liveness defect. Recorded as a finding rather
 *     than done.
 *   - `Atlas-Run` (`orch_runs.run_uid`) is original data, not a derived
 *     index fact -- assigned once at submission and never recomputed by
 *     anything -- so it does not share this exposure; a database restored
 *     from a backup still has the same value.
 *
 * The design already degrades correctly for the two exposed fields: ingesting
 * a trailer against a database where the referenced generation or reason no
 * longer resolves leaves that field `UNKNOWN`, exactly as a wrong value would.
 * **A reader who sees `UNKNOWN` must not read it as tampering** -- it may
 * only mean the index under it moved -- which is why this rebuild exposure is
 * written down here rather than left for a reader to discover by surprise.
 *
 * **The guarantee that no prompt, memory body, credential, model name or cost
 * ever appears in a trailer is structural, not a filter**: `atlas_memory_
 * trailer_compose` has no parameter that could carry one. Do not add a
 * `note`, `summary`, `detail` or `model` parameter to it, however convenient
 * -- A10.1's memory-candidate struct carries the identical absence and for
 * the identical reason. */

/* What one commit's trailer block resolved to -- `memory_trailer_bindings`'
 * own shape (migration 29, `src/db/migrate.c:4450-4467`, widened by
 * migration 30's `bound_hit`). A value is present only when it verified; a
 * field that did not is named in `unknown_fields` and carries no stored
 * value, so a caller can never confuse "verified true" with "not even
 * attempted" -- A9.2.2's shape, one layer over commit trailers. `has_block`
 * is the one field that is never itself in `unknown_fields`: it says whether
 * an `Atlas-Provenance: v1` line was found at all, and when it is false
 * every other member is at its zero and `unknown_fields` is empty.
 *
 * `bound_hit` is the fix-round addition that keeps a third state from
 * collapsing into `has_block` false: the block search
 * (`find_provenance_line`, `src/memory/trailer.c`) can also stop because it
 * ran out of message to look at, at either of its two bounds
 * (`ATLAS_MEMORY_TRAILER_SCAN_MAX` lines or the
 * `ATLAS_MEMORY_TRAILER_TAIL_BYTES_MAX` byte backstop), before ever finding a
 * marker. `bound_hit` is true exactly then: `has_block` false and `bound_hit`
 * true together say "this parse could not fully examine this commit's
 * message, so a block may exist beyond what it considered."
 *
 * Fix round (I2): this struct is what `atlas_memory_trailer_ingest` returns
 * for every examined commit, whether or not a row is ever stored for it --
 * `atlas_db_memory_trailer_binding_insert`'s own comment below says the
 * writer stores a row only when `has_block || bound_hit`. A commit whose
 * message was fully examined and genuinely carries no block -- `has_block`
 * false, `bound_hit` false -- gets **no row at all**, which reads identically
 * to a commit above the persisted cursor that has not been examined yet; a
 * reader tells the two apart by comparing the commit's id against
 * `repositories.trailer_scan_high` (`atlas_db_repo_trailer_scan_high`), never
 * by any field on a row, because in both cases there is no row to hold one.
 * `atlas_memory_pass_result`'s own comment names the four states this and the
 * cursor together let a reader tell apart in full. `bound_hit` is never true
 * alongside `has_block`: once a marker is found the search stops, so a found
 * block was never truncated by either bound (see `find_provenance_line`'s own
 * comment for why the search runs from the end of the tail backward, which is
 * what makes "found" and "bound reached" mutually exclusive outcomes of the
 * same walk). */
typedef struct atlas_memory_trailer_binding {
    bool has_block;
    /* Fix round (I1/I2). True only when `has_block` is false: the block
     * search could not fully examine this commit's message before stopping,
     * so this parse's absence finding is not a proof of absence -- see the
     * struct comment above and A9.2.2's asymmetry, one layer over commit
     * trailers. */
    bool bound_hit;
    atlas_buf run_uid;           /* set only when orch_runs names this run and this repository */
    int64_t memory_generation;   /* set only when it equals the resolved run's frozen pack's own
                                     memory_generation -- see the section comment above for why an
                                     independent memory_generations existence check is not this */
    bool context_digest_ok;
    bool decision_set_ok;
    atlas_buf change_reason_uid; /* set only when ai_reasons names this id and this repository;
                                     the bare decimal id, per the section comment's rebuild caveat */
    /* Netstring-encoded (`src/memory/pack.c`'s own shape: `<count>:` then that
     * many `ns_put` elements), field names among "run", "generation",
     * "context_digest", "decision_set_digest", "change_reason" that did not
     * verify. Empty when `has_block` is false. */
    atlas_buf unknown_fields;
} atlas_memory_trailer_binding;

void atlas_memory_trailer_binding_init(atlas_memory_trailer_binding *b);
void atlas_memory_trailer_binding_free(atlas_memory_trailer_binding *b);

/* Composes the block from stored identifiers for the given run. Refuses
 * (`ATLAS_ERR_INTEGRITY`) rather than emit a partial block when the run has no
 * frozen Canonical Context Pack yet (`atlas_db_memory_pack_get` finds
 * nothing) or when `change_reason_uid` does not name an `ai_reasons` row for
 * the pack's own repository -- every value this function emits is one it
 * first read back out of a row it trusts, never one it merely formats. Has no
 * parameter that could carry prose, a prompt, a credential, a model name or a
 * cost -- see the section comment above. */
atlas_status atlas_memory_trailer_compose(atlas_db *db, const char *run_uid,
                                          const char *change_reason_uid, atlas_buf *out,
                                          atlas_err *err);

/* Parses one commit's stored body (`commits.body` -- no process, no git,
 * A1's rule), resolves every field against Atlas' own rows, verifies both
 * digests, and returns the binding with unverified fields named in
 * `unknown_fields`. Writes nothing itself -- the reconciliation pass
 * (`atlas_memory_apply_in_tx`) stores the row. Refuses with `ATLAS_ERR_REPO`
 * when `commit_oid` is not an indexed commit of `repo_id`; a commit that is
 * indexed but carries no `Atlas-Provenance: v1` line within the bounded
 * search returns `ATLAS_OK` with `out->has_block` false, `out->bound_hit`
 * set according to whether that search could look at the whole message (see
 * the struct comment), and every other member at its zero. */
atlas_status atlas_memory_trailer_ingest(atlas_db *db, int64_t repo_id, const char *commit_oid,
                                         atlas_memory_trailer_binding *out, atlas_err *err);

/* T14, widened by the fix round. The one write for `memory_trailer_bindings`
 * -- `db_memory.c`'s own file header states it is the only production writer
 * of a `memory_*` table, and T17's grep is what proves it. `INSERT OR
 * IGNORE`: `UNIQUE(repo_id, commit_oid)` makes a re-scan of an
 * already-recorded commit idempotent rather than a constraint violation, and
 * `*landed_out` (when non-NULL) is set from `sqlite3_changes` -- true only
 * when this call actually inserted a new row, false on the IGNORE path --
 * `atlas_db_memory_unanchored_add`'s own precedent, so a caller counting
 * genuine writes (`trailer_bindings_written`, fix round M2) does not
 * overstate a re-walk of an already-recorded commit as a fresh one. */
atlas_status atlas_db_memory_trailer_binding_insert(atlas_db *db, int64_t repo_id,
                                                    const char *commit_oid,
                                                    const atlas_memory_trailer_binding *b,
                                                    const char *recorded_at, bool *landed_out,
                                                    atlas_err *err);

/* T14. Reads one commit's stored binding back, for a caller that wants what
 * the pass recorded rather than what re-ingesting would compute now (a later
 * `memory trailer --commit OID --repo R` show path, and this task's own
 * tests). `*found_out` is false and `*out` is left freshly initialised when
 * this repository has no binding row for this commit -- which reads
 * identically whether the commit was fully examined and genuinely carries no
 * block, or has not been scanned yet (the struct comment above says how a
 * caller tells the two apart, and it is not through this call). Fix round
 * (I2): this is distinct from a *found* row with `has_block` false, because
 * the writer stores a row only when `has_block || bound_hit` -- so a found
 * row with `has_block` false is always a `bound_hit` row, "this parse could
 * not fully examine the message," never a plain absence. */
atlas_status atlas_db_memory_trailer_binding_get(atlas_db *db, int64_t repo_id,
                                                 const char *commit_oid,
                                                 atlas_memory_trailer_binding *out, bool *found_out,
                                                 atlas_err *err);

/* --- T15: the proposed patch ------------------------------------------------
 *
 * Acceptance item 5's second half. T12's pack and T9's diff rows are an
 * Atlas-owned projection: they regenerate from moved inputs with no proposal
 * step, and nothing here changes that. A hand-authored memory file is not --
 * nobody but its own author may rewrite it, so the only thing Atlas may ever
 * hand back about it is a proposal a person reviews and applies (or does not)
 * themselves.
 *
 * `atlas_memory_patch_build` renders a unified diff against one registered
 * source's content at HEAD (a tracked `REPO_*` source's *working-tree* edits
 * are not read -- M1, fix round; see the paragraph below), proposing ONLY
 * deletions -- never a rewrite, never an addition, never a line changed in
 * place -- plus a findings list for everything the diff does not carry an
 * opinion about. A line (one T7 proposition) is proposed for removal iff:
 *
 *   - it resolved an anchor, correlates to a claim currently live under that
 *     anchor (matched by exact text, `atlas_db_memory_anchor_claim_uids`' own
 *     shape -- `src/memory/reconcile.c`'s `find_prior_cb`, read-only here),
 *     that claim's semantics are DESCRIPTIVE, and its most recently stored
 *     `verify_results` row reads `state = CONTRADICTED` and, load-bearing,
 *     `basis = DETERMINISTIC` -- "deterministically CONTRADICTED" is exactly
 *     that pair and nothing else, never approximated from `algorithm`, the
 *     conflict kind or the confidence score (A9.2: a model cannot become a
 *     tool) -- with a conflict that is anything other than IMPLEMENTATION,
 *     and with that same row's `stale` column false (fix round, I2: `stale`
 *     is true only when *every* piece of evidence behind the verdict has
 *     since gone stale -- the verdict was true of bytes that have since
 *     moved, and A9.2.2's rule that an ABSENT result never survives the
 *     source moving applies to a CONTRADICTED one exactly the same way. A6
 *     says it again one layer over: STALE requires human revalidation and is
 *     never grounds to conclude the claim is wrong. This was omitted from
 *     the shipped predicate, not deviated from -- the original context's own
 *     definition of "deterministically CONTRADICTED" simply did not mention
 *     it, and the omission is the season owner's to correct, which this
 *     round does);
 *   - or that claim's most recently recorded diff kind
 *     (`atlas_db_memory_claim_diff_last_kind`) is SUPERSEDED, its semantics
 *     are DESCRIPTIVE, and its most recently stored `verify_results` row (if
 *     any) does not carry conflict IMPLEMENTATION and is not stale (fix
 *     round, I1: the shipped predicate treated SUPERSEDED as an
 *     unconditional disjunct with neither guard, which let a NORMATIVE claim
 *     or an IMPLEMENTATION conflict reach a hunk through this arm alone --
 *     unreachable in production, since nothing in `src/` writes a SUPERSEDED
 *     diff row yet, which is exactly why it would have gone unnoticed once
 *     something did. Fix round, R1: I1's own re-review found the identical
 *     shape once more, one guard narrower -- the CONTRADICTED arm above
 *     carried `stale` and this one did not, so a stale verdict reached a hunk
 *     through this arm alone. `stale` is restored here for the same reason
 *     I2 stated it for the arm above: a verdict true of bytes that have since
 *     moved is not grounds to delete a person's line). `basis` is
 *     deliberately NOT required on this arm, and this is a conclusion, not an
 *     omission: the project's own documents describe SUPERSEDED two ways --
 *     the T9 brief calls it "the source's new version no longer contains the
 *     proposition" (a fact about the *text*), while the T9 review's residual
 *     note scopes its real, never-implemented derivation to
 *     `verify_claims.superseded_by_claim_id`, a claim replaced by a
 *     successor claim (a fact about *lineage*). Either way it is not a
 *     `verify_results` verdict: it is either the extractor's own text
 *     comparison or a claim-identity fact, neither of which any verifier
 *     establishes, so a `verify_results`-shaped gate (`basis`) does not
 *     describe it under either reading. `conflict` and `stale`, by contrast,
 *     are not a description of supersession itself -- they are two of the
 *     three absolutes below, which this arm holds exactly because every arm
 *     does, not because SUPERSEDED is itself a conflict-and-staleness verdict.
 *
 * **NORMATIVE semantics, an IMPLEMENTATION conflict, and a stale verdict never
 * reach a hunk, on either arm above.** `src/memory/patch.c`'s
 * `patch_may_delete` is the one place these three absolutes are stated, and
 * it has exactly ONE call site: both arms' kind-specific tests are ORed
 * *inside* that one call's argument, never computed as their own named
 * `bool`s and ORed into an `if` from outside it. That is what makes the three
 * absolutes structural rather than a convention an arm's author has to
 * remember -- a fourth arm's natural edit is to widen the disjunction inside
 * that one call, which inherits the three absolutes by construction; a
 * deletion reached any other way needs a visibly separate `if` outside this
 * function's one call, which is what a reviewer is already looking for.
 * IMPLEMENTATION means the code diverged from what was
 * approved -- the approved thing is not the thing that is wrong, and
 * proposing deletion there would be automatically adopting a design because
 * current code happens to implement it, a named non-goal of this season.
 * Stale means the verdict was true of bytes that have since moved, and A6
 * says the same thing one layer over: STALE requires human revalidation and
 * is never grounds to conclude the claim is wrong. A line excluded for any of
 * the three is a *finding*, never a hunk.
 *
 * A line that resolved no anchor, or whose anchor resolves but no live claim
 * under it carries this exact text (a proposition this pass cannot correlate
 * to any stored assessment at all), produces neither a hunk nor a finding --
 * A9.2.2's asymmetry, one layer over: no evidence against a line is not
 * evidence for keeping or removing it, so this function says nothing about
 * it rather than guessing.
 *
 * **Fix round (M4): only a candidate's *first* anchor is ever consulted**
 * (`atlas_db_memory_anchor_claim_uids` is asked about `anchors[0]` alone --
 * `src/memory/reconcile.c:833-834`'s own correlation lookup does the same,
 * though that file's separate *pruning* pass was widened to every one of a
 * claim's anchors in T9's fix round, `reconcile.c:845-855`, so the precedent
 * is for the lookup shape, not a claim that the file has this limitation
 * throughout). A line whose live claim was anchored to its *second*
 * backticked path correlates to nothing here and produces neither a hunk
 * nor a finding, by the paragraph above -- conservative in the deletion
 * direction, but it means neither `diff_out` nor `findings_out` is a census
 * of every assessed line in the source; a reader must not infer "not
 * mentioned in findings" as "known healthy" for a line with more than one
 * anchor.
 *
 * **This function reads files and must not run inside a transaction** --
 * `atlas_memory_observe`'s own rule, for the same reason: it asks
 * `atlas_memory_read_source`, which is A13-routed (a repository naming a
 * scanner is read from its mirror and from nothing else; one with none reads
 * its own tree, exactly as every other T6 caller does -- this function
 * restates none of that routing, it only asks). `data_dir` exists so the
 * diff's context lines are read through the same path this function's own
 * read uses.
 *
 * **Fix round (M1): "the same path" is HEAD's blob, not the working tree.**
 * For a tracked `REPO_FILE`, `atlas_memory_read_source` resolves the path
 * against HEAD's tree and reads the git object (`read.c`'s `atlas_git_
 * blob_oid_at` / `atlas_git_cat_blob`); the filesystem handle it opens first
 * proves the path is not a symlink and is never read from again once the
 * blob is fetched. So an uncommitted edit to a tracked memory file is
 * invisible here, and the diff's context lines are HEAD's bytes, not the
 * file on disk. This fails closed rather than corrupting anything: `git
 * apply` rejects a hunk whose context does not match the working tree
 * exactly, it does not apply it against the wrong lines. An EXTERNAL_*
 * source instead reads its already-stored latest version
 * (`atlas_db_memory_version_latest`), `atlas_memory_observe`'s own
 * EXTERNAL_* shape, since no principal but the one that called `memory.put`
 * reads one directly -- that half was always "stored", never "working
 * tree", and is unaffected by this correction.
 *
 * **It writes nothing anywhere.** `diff_out` and `findings_out` are the only
 * output; no `memory_*` row, no `verify_*` row, no file. A caller may verify
 * this with `fx_tree_digest` bracketing the whole call, not only the write it
 * does not make.
 *
 * **Nothing Atlas authored appears inside a hunk.** Every context and removed
 * line is the source's own bytes, safe-encoded (`atlas_text_encode_safe`) at
 * the point they are written -- repository content is untrusted input,
 * CLAUDE.md's rule, applied to a diff line exactly as it is to a rendered
 * claim. A finding is Atlas talking about a line, and is listed *beside* the
 * diff, never inside it: wanting to put an explanatory comment inside a hunk
 * is a sign the comment belongs in `findings_out` instead.
 *
 * **Fix round (M2): that makes the output a rendering, and not always an
 * applicable patch.** `atlas_text_encode_safe` escapes `%` as `%25` along
 * with the C0/C1 controls -- required, since the byte it is escaping is
 * untrusted, but the consequence is that a source line containing a literal
 * `%` produces a diff line that is no longer equal to the file's own bytes.
 * `git apply` then rejects that hunk rather than mis-applying it (the
 * context or removed line no longer matches), so this fails closed exactly
 * like M1 -- but it means this output is a safe *rendering* of the proposal,
 * not a guarantee that the bytes are `git apply`-ready. A caller wanting an
 * applicable patch must know the source is free of `%` and of every other
 * byte `atlas_text_encode_safe` escapes.
 *
 * **Fix round (M3): no `\ No newline at end of file` marker.** Every emitted
 * line, including the last, is terminated with `'\n'`; a source whose own
 * last byte is not `'\n'` is rendered as if it were. Fails closed the same
 * way: if the hunk's own window -- the deleted run plus its context lines --
 * reaches the file's true last line, `git apply` rejects the hunk (its
 * trailing context or removal no longer matches the file exactly) rather
 * than truncating or duplicating a newline.
 *
 * `diff_out` is empty and `findings_out` says so -- one entry naming the
 * source and that nothing was proposed -- when nothing in the source
 * qualified, whether because there was nothing to read, nothing anchored, or
 * everything anchored was healthy: an empty diff on its own does not say
 * *why*, and a reader must not read silence as "this pass looked and found
 * every line clean" without the finding confirming it. */
atlas_status atlas_memory_patch_build(atlas_db *db, const atlas_repo_info *repo,
                                      const char *data_dir, const char *source_uid,
                                      atlas_buf *diff_out, atlas_buf *findings_out, atlas_err *err);

#endif /* ATLAS_MEMORY_H */
