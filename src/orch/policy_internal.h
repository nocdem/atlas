/* Atlas - A8/A12.0: the orchestration policy's parser, separated from its
 * authority check.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * `atlas_orchpolicy_load_at` does two things: it establishes that the file is
 * one only root could have written, and it reads the keys. The first is what
 * makes a policy a policy, and it is deliberately not a parameter anywhere.
 *
 * This declares the second half on its own, and it exists for one reason: the
 * suite runs as an ordinary uid, so every file it can create is refused by the
 * path walk before a byte is read. Without this seam the key rules — one
 * occurrence, a checked token, an unrecognised key is an error — are reachable
 * only on a machine where the test can write into a root-owned directory, which
 * is to say nowhere the suite runs.
 *
 * **It is not a second loader.** It never touches `out->state`, so it cannot
 * produce an enabled policy: the one `ATLAS_ORCHPOLICY_ENABLED` assignment in
 * Atlas is still the last statement of the loader, reached only after the path
 * walk. What it returns is the reason the loader would carry, and the caller
 * zeroes the struct first — the loader through its own `memset`, a test through
 * its own.
 *
 * Private to `src/`, like `db_internal.h`. Nothing in `include/atlas` exposes
 * it, and the only production caller is the loader in the same file.
 */
#ifndef ATLAS_ORCH_POLICY_INTERNAL_H
#define ATLAS_ORCH_POLICY_INTERNAL_H

#include <stddef.h>

#include "atlas/orchpolicy.h"

/* Reads `len` bytes of policy text into an already-zeroed `out`.
 *
 * Returns `ATLAS_ORCHPOLICY_REASON_ACTIVE` when the text describes a complete
 * deployment, and `ATLAS_ORCHPOLICY_REASON_MALFORMED` otherwise — a bad value, a
 * repeated singleton key, a key Atlas does not recognise, a missing required
 * key, a ceiling above the compiled-in absolute, or a combination the policy
 * refuses. On any refusal the struct is left however far parsing got, which is
 * safe because `state` is untouched and stays disabled. */
atlas_orchpolicy_reason atlas_orchpolicy_parse_bytes(const char *bytes, size_t len,
                                                     atlas_orchpolicy *out);

#endif /* ATLAS_ORCH_POLICY_INTERNAL_H */
