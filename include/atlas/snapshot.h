/* Atlas - A8: the daemon-owned source snapshot protocol.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * ## Why this exists
 *
 * A8's first cut had the worker read the registered repository itself. That was
 * wrong twice over. It failed in practice — git refuses a repository owned by
 * another uid, and A7.1 deliberately splits the principals — and it was wrong in
 * principle: it required `atlas-worker`, the untrusted account, to hold a
 * read-only path to `/opt/dna`, `/opt/atlas` and `/opt/swapper`.
 *
 * So the direction is inverted. **`atlasd` reads; the worker receives.** The
 * daemon resolves the repository from its own registry, validates the pinned
 * commit belongs to it, enumerates the committed tree, and streams a canonical
 * bounded snapshot to whichever worker holds the current valid lease. The
 * worker never opens a registered repository, is never told a daemon-side
 * filesystem path, and cannot name a repository or a commit.
 *
 * The dispatcher service no longer needs `ReadOnlyPaths` for the repositories at
 * all — that absence is the point, and it is stronger than the read-only mount
 * it replaces.
 *
 * ## The transfer
 *
 * Two dispatcher-only methods, both bound to an attempt by its lease token:
 *
 *   `dispatch.snapshot.open`  enumerate once, persist the manifest, return the
 *                             identity and the bounds.
 *   `dispatch.snapshot.chunk` one bounded slice of one entry's content.
 *
 * Content is hex-encoded. A JSON string cannot carry arbitrary bytes, and hex is
 * the encoding whose decoder cannot be argued with — no escapes, no ambiguity,
 * a fixed 2:1 expansion, and a malformed body is malformed at the first bad
 * nibble rather than silently shorter.
 *
 * The snapshot digest is domain-separated and length-prefixed, over the manifest
 * rather than over the transfer. The worker recomputes it from what it actually
 * received and materialised, so a stream that lost, duplicated or reordered an
 * entry cannot produce a matching digest.
 */
#ifndef ATLAS_SNAPSHOT_H
#define ATLAS_SNAPSHOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "atlas/buf.h"
#include "atlas/error.h"

typedef struct atlas_db atlas_db;

/* Bumped when the wire shape or the digest encoding changes. A worker that does
 * not recognise the version refuses rather than guessing. */
#define ATLAS_SNAPSHOT_PROTOCOL 1

/* Domain separation for the snapshot digest, for A4's reason exactly. */
#define ATLAS_SNAPSHOT_DOMAIN "atlas.orch.snapshot.v1"

/* Bounds. Each refuses rather than trims: a snapshot that silently stopped
 * copying would hand a driver a repository missing files, and every result
 * computed from it would be about something that never existed.
 *
 * The chunk bound is what keeps a repository off a single RPC frame. The
 * response ceiling is 8 MiB and hex doubles the payload, so 256 KiB of content
 * leaves an order of magnitude of headroom for the envelope. */
#define ATLAS_SNAPSHOT_CHUNK_BYTES (256u * 1024u)
#define ATLAS_SNAPSHOT_MAX_FILE_BYTES (8ll * 1024ll * 1024ll)
#define ATLAS_SNAPSHOT_MAX_ENTRIES 20000
#define ATLAS_SNAPSHOT_MAX_TOTAL_BYTES (256ll * 1024ll * 1024ll)
#define ATLAS_SNAPSHOT_PATH_MAX 4096u

/* Why an entry was refused. Reported and counted rather than silently skipped:
 * a caller that does not know a symlink was dropped believes it has a faithful
 * snapshot. */
typedef enum atlas_snapshot_refusal {
    ATLAS_SNAPSHOT_REFUSE_NONE = 0,
    ATLAS_SNAPSHOT_REFUSE_SYMLINK,
    ATLAS_SNAPSHOT_REFUSE_GITLINK,
    ATLAS_SNAPSHOT_REFUSE_MODE,
    ATLAS_SNAPSHOT_REFUSE_PATH,
    ATLAS_SNAPSHOT_REFUSE_SIZE
} atlas_snapshot_refusal;

const char *atlas_snapshot_refusal_name(atlas_snapshot_refusal r);

typedef struct atlas_snapshot_meta {
    int protocol;
    int64_t snapshot_id;
    char commit[65];
    char tree[65];
    int64_t entries;
    int64_t total_bytes;
    char digest[65];
    int64_t refused_symlinks;
    int64_t refused_gitlinks;
    int64_t refused_other;
} atlas_snapshot_meta;

/* Produces the manifest for one attempt and persists it.
 *
 * Everything it acts on comes from authoritative Atlas state: the attempt names
 * the job, the job names the repository by durable identity and the exact pinned
 * commit, and the registry names the canonical path. Nothing here is taken from
 * a worker message. The repository is opened read-only through the one trusted
 * access layer, and the commit is verified to belong to it before a single blob
 * is read.
 *
 * Idempotent per attempt: a second call returns the existing manifest rather
 * than enumerating again, so a dispatcher that restarts mid-transfer resumes
 * against the same snapshot identity instead of a newly enumerated one that
 * might differ. */
atlas_status atlas_snapshot_open(atlas_db *db, int64_t attempt_id, atlas_snapshot_meta *out,
                                 atlas_err *err);

/* One bounded slice of one entry.
 *
 * `index` is the entry's position in the canonical order and `offset` is a byte
 * offset within it. Both are validated against the persisted manifest, so a
 * worker cannot ask for an entry that does not exist or a range past the end. */
typedef struct atlas_snapshot_chunk {
    atlas_buf path; /* repository-relative, raw bytes */
    char mode[8];
    char sha256[65];
    int64_t size_bytes;
    int64_t offset;
    atlas_buf data; /* raw bytes; the caller hex-encodes for the wire */
    bool eof;
} atlas_snapshot_chunk;

void atlas_snapshot_chunk_init(atlas_snapshot_chunk *c);
void atlas_snapshot_chunk_free(atlas_snapshot_chunk *c);

atlas_status atlas_snapshot_read(atlas_db *db, int64_t attempt_id, int64_t index, int64_t offset,
                                 atlas_snapshot_chunk *out, atlas_err *err);

/* The canonical digest over a manifest, domain-separated and length-prefixed.
 * Computed by the daemon over what it enumerated and by the worker over what it
 * materialised; a stream that lost, duplicated or reordered anything cannot
 * produce a match. */
typedef struct atlas_snapshot_digest {
    void *opaque;
} atlas_snapshot_digest;

atlas_status atlas_snapshot_digest_begin(atlas_snapshot_digest *d, const char *commit,
                                         const char *tree, atlas_err *err);
atlas_status atlas_snapshot_digest_entry(atlas_snapshot_digest *d, const void *path,
                                         size_t path_len, const char *mode, int64_t size,
                                         const char *sha256, atlas_err *err);
atlas_status atlas_snapshot_digest_finish(atlas_snapshot_digest *d, int64_t entries,
                                          int64_t total_bytes, char out[65], atlas_err *err);
void atlas_snapshot_digest_abort(atlas_snapshot_digest *d);

/* True when `path` may be materialised: relative, no empty, `.` or `..`
 * component, no NUL, and within the length bound. Repository paths are bytes and
 * stay bytes — this is a structural check, not an encoding one. */
bool atlas_snapshot_path_ok(const void *path, size_t len);

#endif /* ATLAS_SNAPSHOT_H */
