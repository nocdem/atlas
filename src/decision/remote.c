/* Atlas - A16: verifying the credential a remote disposal op carries.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See `atlas/decision_remote.h` for why the decision layer depends on
 * `atlas/apikey.h` and `atlas/gw.h`: the credential a browser disposal spends
 * is an ordinary Atlas API key, the daemon holds the only verifiers for one,
 * and the single write point is the only place that may spend a capability --
 * so it is also where that credential must be authenticated, inside the
 * transaction that is about to act on its strength.
 */
#include "atlas/decision_remote.h"

#include <stdio.h>
#include <string.h>

#include "atlas/apikey.h"
#include "atlas/gw.h"

atlas_status atlas_decision_remote_verify(atlas_db *db, const atlas_buf *token,
                                          const char *expected_key_id,
                                          char key_id_out[ATLAS_APIKEY_SELECTOR_HEX + 1u],
                                          atlas_err *err) {
    key_id_out[0] = '\0';

    /* Every failure up to and including "did not authenticate" produces the
     * identical outward sentence -- `gateway.auth`'s own rule, restated here
     * because this is the second and last place Atlas verifies this shape of
     * credential. A caller that could distinguish a malformed token from an
     * unknown selector from a wrong secret would learn which half of a guess
     * was right. */
    static const char *const DID_NOT_AUTHENTICATE =
        "the credential presented for this disposal did not authenticate; nothing was changed";

    char selector[ATLAS_APIKEY_SELECTOR_HEX + 1u];
    unsigned char secret[ATLAS_APIKEY_SECRET_BYTES];
    memset(selector, 0, sizeof(selector));
    memset(secret, 0, sizeof(secret));

    atlas_apikey_record rec;
    memset(&rec, 0, sizeof(rec));
    bool authenticated = false;

    atlas_err perr;
    atlas_err_init(&perr);
    if (token != NULL &&
        atlas_apikey_token_parse(atlas_buf_cstr(token), selector, secret, &perr) == ATLAS_OK) {
        bool found = false;
        atlas_err lerr;
        atlas_err_init(&lerr);
        if (atlas_db_apikey_lookup(db, selector, &rec, &found, &lerr) == ATLAS_OK && found) {
            /* Every condition in one expression, exactly as `gateway.auth`
             * evaluates them, so the work done for a wrong secret does not
             * differ measurably from the work done for a right one:
             * `atlas_apikey_verify` is constant-time and the lookup is an
             * indexed equality test. */
            authenticated = rec.status == ATLAS_APIKEY_STATUS_ACTIVE && !rec.scopes_unreadable &&
                            atlas_apikey_verify(secret, sizeof(secret), rec.salt, sizeof(rec.salt),
                                                rec.verifier, sizeof(rec.verifier));
        }
    }
    /* The secret is wiped the moment it has been used, whatever the outcome. */
    memset(secret, 0, sizeof(secret));
    memset(selector, 0, sizeof(selector));

    if (!authenticated) {
        memset(&rec, 0, sizeof(rec));
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "%s", DID_NOT_AUTHENTICATE);
    }

    /* Only past this point do the last two conditions get their own named
     * sentences -- both are about a policy mismatch on a credential Atlas has
     * just proven is real, which is a different and more actionable fact than
     * "nothing you presented checked out". */
    if (rec.mask != 0u) {
        atlas_status st = atlas_err_set(
            err, ATLAS_ERR_INTEGRITY,
            "the remote disposal credential must hold no stored scope, and %s holds %s",
            rec.key_id, rec.scopes);
        memset(&rec, 0, sizeof(rec));
        return st;
    }
    if (expected_key_id == NULL || strcmp(rec.key_id, expected_key_id) != 0) {
        atlas_status st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                        "that credential is not the one the remote disposal "
                                        "policy names");
        memset(&rec, 0, sizeof(rec));
        return st;
    }

    (void)snprintf(key_id_out, ATLAS_APIKEY_SELECTOR_HEX + 1u, "%s", rec.key_id);
    memset(&rec, 0, sizeof(rec));
    return ATLAS_OK;
}
