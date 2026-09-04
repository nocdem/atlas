/* Atlas - A14: verifying the credential a remote submission op carries.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * This is the third copy of the API-key credential check that Atlas holds,
 * and it is not shared with either of the other two.  The argument is
 * Decision 2 in `docs/plans/2026-09-04-remote-submission.md`, which is
 * restated here because this file will be read without the plan:
 *
 *   Three principals present API-key credentials to Atlas: a browser
 *   disposing of a record (A16, `src/decision/remote.c`), the gateway
 *   authenticating a bearer on its own listener (`src/gw/server_gw.c`), and
 *   a gateway forwarding a submission bearer to the daemon (this file).
 *
 *   Each copy sits in the layer it belongs to.  `server_gw.c` checks on
 *   behalf of the gateway's *own* listener — a gateway route, not a daemon
 *   write.  `src/decision/remote.c` checks on behalf of the decision write
 *   point — a disposal inside `atlas_decision_apply_in_tx`.  This file checks
 *   on behalf of the orchestration write point — a submission inside
 *   `atlas_orch_apply_in_tx`.  Sharing the logic would require threading the
 *   wrong context into the wrong layer, or placing both layers' logic in a
 *   common layer that would then have to know about both.  A person on a phone
 *   and a model present a bearer credential identically; what differs is what
 *   the verified result authorises, which is decided entirely by the layer
 *   that called the verifier — not by the verifier itself.
 *
 *   Decision 1 is a direct consequence: the submission verifier does NOT
 *   require an empty stored scope list.  Submission keys may hold `repo:read`
 *   or other scopes a gateway policy grants alongside submission rights; the
 *   disposal verifier requires empty scopes because a disposal credential
 *   holds *nothing else*, which is a property of disposal credentials, not of
 *   all API keys.
 *
 * This file must not call the decision write point or the decision operator
 * channel — `test_decision_mcp.c` scans src/ for those call sites and expects
 * exactly three of each.
 */
#include "atlas/orch_remote.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "atlas/apikey.h"
#include "atlas/gw.h"
#include "atlas/orch.h" /* ATLAS_ORCH_REMOTE_CLIENT_KEY_MAX, ATLAS_ORCH_NAME_MAX */

atlas_status atlas_orch_remote_verify(atlas_db *db, const atlas_buf *token,
                                      const char allowed[][ATLAS_APIKEY_SELECTOR_HEX + 1u],
                                      size_t allowed_count,
                                      char key_id_out[ATLAS_APIKEY_SELECTOR_HEX + 1u],
                                      atlas_err *err) {
    key_id_out[0] = '\0';

    /* Every failure up to and including "did not authenticate" produces the
     * identical outward sentence -- `gateway.auth`'s own rule, restated here
     * because this is the third and last place Atlas verifies this shape of
     * credential.  A caller that could distinguish a malformed token from an
     * unknown selector from a wrong secret from a revoked key would learn
     * which half of a guess was right. */
    static const char *const DID_NOT_AUTHENTICATE =
        "the credential presented for this submission did not authenticate; nothing was queued";

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
             * indexed equality test.
             *
             * Decision 1: `rec.mask != 0u` is NOT checked here.  Submission
             * keys may hold scopes; requiring an empty scope list is a property
             * of disposal credentials only. */
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

    /* Only past this point does the policy-mismatch check get its own named
     * sentence -- it is about a policy that does not list this credential,
     * which is a different and more actionable fact than "nothing you presented
     * checked out". */
    bool in_list = false;
    for (size_t i = 0; i < allowed_count; i++) {
        if (strcmp(rec.key_id, allowed[i]) == 0) {
            in_list = true;
            break;
        }
    }
    if (!in_list) {
        atlas_status st = atlas_err_set(
            err, ATLAS_ERR_INTEGRITY,
            "that credential is not one the remote submission policy names");
        memset(&rec, 0, sizeof(rec));
        return st;
    }

    (void)snprintf(key_id_out, ATLAS_APIKEY_SELECTOR_HEX + 1u, "%s", rec.key_id);
    memset(&rec, 0, sizeof(rec));
    return ATLAS_OK;
}

atlas_status atlas_orch_remote_idempotency_key(const char *key_id, const char *client,
                                               atlas_buf *out, atlas_err *err) {
    /* Validate the client part: 1..ATLAS_ORCH_REMOTE_CLIENT_KEY_MAX chars of
     * [a-z0-9._-].  Any other character, or a length of zero or above 40, is
     * refused with the frozen sentence. */
    if (client == NULL) {
        client = "";
    }
    size_t n = strlen(client);
    bool ok = (n >= 1u && n <= ATLAS_ORCH_REMOTE_CLIENT_KEY_MAX);
    for (size_t i = 0; ok && i < n; i++) {
        unsigned char c = (unsigned char)client[i];
        ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
             c == '-';
    }
    if (!ok) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a remote idempotency key is at most 40 characters of [a-z0-9._-]");
    }

    /* Build "remote.<key_id>.<client>".
     * 7 + 16 + 1 + 40 = 64 == ATLAS_ORCH_NAME_MAX. */
    atlas_buf_reset(out);
    return atlas_buf_appendf(out, err, "remote.%s.%s", key_id, client);
}
