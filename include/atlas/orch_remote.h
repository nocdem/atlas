/* Atlas - A14: verifying the credential a remote submission op carries.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See `src/orch/remote.c` for why this is a third copy of the credential-
 * verification logic and why the three copies are not shared.
 *
 * Both functions here are called only from `op_submit` and `op_cancel` in
 * `src/db/db_orch.c`, inside `atlas_orch_apply_in_tx`'s transaction. Neither
 * touches the decision tables, the verification tables, or anything outside the
 * `orch_*` family. */
#pragma once

#include <stddef.h>

#include "atlas/apikey.h" /* ATLAS_APIKEY_SELECTOR_HEX */
#include "atlas/atlas.h"  /* atlas_buf, atlas_status, atlas_err */
#include "atlas/db.h"     /* atlas_db */

/* Verifies that `token` resolves to an active Atlas API key whose id is a
 * member of `allowed[0..allowed_count-1]`.
 *
 * This is the write point's own verifier.  It differs from
 * `atlas_decision_remote_verify` in two ways (Decision 1 and Decision 2,
 * `src/orch/remote.c`): it does not require the stored scope list to be empty,
 * and it checks membership in a list rather than equality with one id.
 *
 * Every failure up to and including "did not authenticate" produces the same
 * outward sentence. A caller that could distinguish a malformed token from an
 * unknown selector from a wrong secret from a revoked key would learn which
 * half of a guess was right.
 *
 * On success, `key_id_out` holds the bare 16-hex selector.  The secret is
 * wiped immediately after use, whatever the outcome. */
atlas_status atlas_orch_remote_verify(atlas_db *db, const atlas_buf *token,
                                      const char allowed[][ATLAS_APIKEY_SELECTOR_HEX + 1u],
                                      size_t allowed_count,
                                      char key_id_out[ATLAS_APIKEY_SELECTOR_HEX + 1u],
                                      atlas_err *err);

/* Builds the namespaced idempotency key `remote.<key_id>.<client>` into `out`.
 *
 * `client` must be between 1 and `ATLAS_ORCH_REMOTE_CLIENT_KEY_MAX` characters
 * and must consist only of `[a-z0-9._-]`.  Any other value is refused with the
 * frozen sentence.  `key_id` must be the 16-hex selector returned by
 * `atlas_orch_remote_verify`.
 *
 * The assembled key fits in `ATLAS_ORCH_NAME_MAX` bytes by construction:
 * `"remote." (7) + 16 hex + "." (1) + 40 = 64 == ATLAS_ORCH_NAME_MAX`. */
atlas_status atlas_orch_remote_idempotency_key(const char *key_id, const char *client,
                                               atlas_buf *out, atlas_err *err);
