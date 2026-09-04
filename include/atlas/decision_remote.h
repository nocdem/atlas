/* Atlas - A16: verifying the credential a remote disposal op carries.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Why the decision layer depends on `atlas/apikey.h` and `atlas/gw.h`
 * ---------------------------------------------------------------------
 * Every lifecycle transition passes through `atlas_decision_apply_in_tx`, and
 * this season adds a channel that mints and spends a capability from the
 * browser rather than from a terminal. The gateway *claims* a key id on every
 * request it forwards, but a claim is not evidence about itself -- this
 * project's own rule about peer identity, one layer up. The daemon holds the
 * verifiers (`api_keys.salt`, `api_keys.verifier`) and is the only principal
 * that may use them, so the one function that writes a lifecycle transition
 * is also the one that must authenticate a remote disposal credential, inside
 * the same transaction that spends it -- never before, and never by asking
 * anyone else to have already done so. That is what makes `src/decision`
 * depend on `atlas/apikey.h` (the token format and the constant-time verifier)
 * and `atlas/gw.h` (`atlas_apikey_record`, `atlas_db_apikey_lookup`): the
 * credential this season's channel spends is an ordinary Atlas API key, and
 * there is exactly one place Atlas knows how to check one.
 *
 * This mirrors `gateway.auth` (`src/ipc/server_gw.c`) almost exactly -- same
 * parse, same lookup, same constant-time compare -- and deliberately does not
 * share code with it: `gateway.auth` runs on a request's own read-only handle,
 * outside any transaction, for a caller (the gateway) that only needs to know
 * what a credential may read. This runs on the writer's own handle, inside
 * the transaction that is about to spend the credential's one derived scope,
 * for a caller that is about to change project state on its strength. Two
 * different callers with two different concurrency contracts sharing one
 * function would be one more surface where a future change to either quietly
 * became a change to both.
 */
#ifndef ATLAS_DECISION_REMOTE_H
#define ATLAS_DECISION_REMOTE_H

#include "atlas/apikey.h"
#include "atlas/buf.h"
#include "atlas/db.h"
#include "atlas/error.h"

/* Verifies the credential a REMOTE op carries, on `db` (the writer's own
 * writable handle, inside the transaction the caller already holds), and
 * writes the verified key id to `key_id_out`.
 *
 * Refuses, with the season's frozen sentences, when: the token does not
 * parse; the selector names no credential; the secret does not verify
 * (`atlas_apikey_verify`, constant-time); the key is not ACTIVE; its stored
 * scopes could not be parsed; its stored scope list is not empty; or its id is
 * not `expected_key_id`. Every failure but the last two produces the identical
 * outward sentence, on `gateway.auth`'s own precedent: a caller that could
 * tell a wrong secret from an unknown selector would learn which half of a
 * guess was right.
 *
 * Reads the database and nothing else -- no process is created, no file is
 * opened, and the only clock this function touches is the one the caller
 * already has (it consults none). `key_id_out` is left empty on every
 * refusal. */
atlas_status atlas_decision_remote_verify(atlas_db *db, const atlas_buf *token,
                                          const char *expected_key_id,
                                          char key_id_out[ATLAS_APIKEY_SELECTOR_HEX + 1u],
                                          atlas_err *err);

#endif /* ATLAS_DECISION_REMOTE_H */
