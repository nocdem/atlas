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
 * The five closed vocabularies the layer is built from, and nothing else. They
 * are here rather than beside their consumers because a vocabulary with two
 * homes is a vocabulary with two spellings, and every one of these is stored in
 * a column with a `CHECK` constraint that has to agree with it exactly.
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
 * Two of these are about the *text* and five are about the *world*, and the
 * split is the point. ADDED and CHANGED say the memory file itself moved.
 * SUPPORTED, CONTRADICTED, STALE, IMPACTED and SUPERSEDED say the tree moved
 * underneath an assertion whose text did not -- which is exactly the condition a
 * model reading a memory file cannot detect for itself, and the reason this
 * season exists. */
typedef enum atlas_memory_diff_kind {
    ATLAS_MEMORY_DIFF_UNKNOWN = 0,
    ATLAS_MEMORY_DIFF_ADDED,
    ATLAS_MEMORY_DIFF_CHANGED,
    ATLAS_MEMORY_DIFF_SUPPORTED,
    ATLAS_MEMORY_DIFF_CONTRADICTED,
    ATLAS_MEMORY_DIFF_STALE,
    ATLAS_MEMORY_DIFF_IMPACTED,
    ATLAS_MEMORY_DIFF_SUPERSEDED
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

#endif /* ATLAS_MEMORY_H */
