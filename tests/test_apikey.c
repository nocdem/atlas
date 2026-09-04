/* Atlas - A9: the API-key lifecycle, and what must never come back out of it.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The lifecycle is easy to get right and easy to get *quietly* wrong. A key that
 * is created, listed and revoked correctly still fails A9 if the plaintext is
 * recoverable afterwards, and nothing about the happy path would say so. So the
 * assertions here are mostly about absence:
 *
 *   - the secret does not appear anywhere in the database file, as bytes;
 *   - no command returns it after the one that created it;
 *   - a revoked key stops verifying immediately, with no cache to invalidate;
 *   - a rotated-away key stops verifying while its replacement works;
 *   - a refusal never quotes the credential it refused.
 *
 * The database is searched as raw bytes rather than through SQL on purpose. A
 * query can only find a plaintext in a column somebody thought to look at; a
 * byte search finds one that leaked into a journal page, an index or a column
 * nobody expected.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/apikey.h"
#include "atlas/gw.h"
#include "atlas_test.h"
#include "support/fixture.h"

/* Runs the binary against the fixture data directory and captures stdout. */
static int run_key(fixture *fx, const char **args, size_t n, atlas_buf *out, atlas_err *err) {
    const char *argv[24];
    size_t k = 0;
    argv[k++] = "--data-dir";
    argv[k++] = fx_data_dir(fx);
    for (size_t i = 0; i < n; i++) {
        argv[k++] = args[i];
    }
    T_REQUIRE(k <= sizeof argv / sizeof argv[0]);
    int code = -1;
    atlas_buf_reset(out);
    T_OK(fx_atlas(argv, k, out, NULL, &code, err), err);
    return code;
}

/* Pulls `ATLAS_API_KEY=<token>` out of the human output. */
static void token_of(const atlas_buf *out, char *token, size_t token_size) {
    token[0] = '\0';
    const char *s = strstr(atlas_buf_cstr(out), "ATLAS_API_KEY=");
    T_REQUIRE(s != NULL);
    s += strlen("ATLAS_API_KEY=");
    size_t n = 0;
    while (s[n] != '\0' && s[n] != '\n' && n + 1 < token_size) {
        n++;
    }
    memcpy(token, s, n);
    token[n] = '\0';
    T_REQUIRE(n > 0);
}

static void id_of(const atlas_buf *out, char *id, size_t id_size) {
    id[0] = '\0';
    const char *s = strstr(atlas_buf_cstr(out), "id:     " ATLAS_APIKEY_ID_PREFIX);
    T_REQUIRE(s != NULL);
    s += strlen("id:     " ATLAS_APIKEY_ID_PREFIX);
    size_t n = 0;
    while (s[n] != '\0' && s[n] != '\n' && n + 1 < id_size) {
        n++;
    }
    memcpy(id, s, n);
    id[n] = '\0';
}

/* True when `needle` occurs anywhere in the file, as raw bytes. */
static bool file_contains(const char *path, const char *needle) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    /* Read the whole file: an index is small in these fixtures, and a chunked
     * search would need overlap handling that could itself miss a match. */
    (void)fseek(f, 0, SEEK_END);
    long len = ftell(f);
    (void)fseek(f, 0, SEEK_SET);
    if (len <= 0) {
        (void)fclose(f);
        return false;
    }
    char *buf = malloc((size_t)len);
    if (buf == NULL) {
        (void)fclose(f);
        return false;
    }
    size_t got = fread(buf, 1, (size_t)len, f);
    (void)fclose(f);
    bool found = false;
    size_t nlen = strlen(needle);
    if (nlen > 0 && got >= nlen) {
        for (size_t i = 0; i + nlen <= got; i++) {
            if (memcmp(buf + i, needle, nlen) == 0) {
                found = true;
                break;
            }
        }
    }
    free(buf);
    return found;
}

/* Verifies a presented token against the stored row, exactly as the gateway
 * will: parse, look up by selector, compare in constant time. */
static bool token_authenticates(fixture *fx, const char *token) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf db_path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&db_path, &err, "%s/atlas.db", fx_data_dir(fx)), &err);
    atlas_db *db = NULL;
    T_OK(atlas_db_open_readonly(atlas_buf_cstr(&db_path), &db, &err), &err);

    char sel[ATLAS_APIKEY_SELECTOR_HEX + 1];
    unsigned char secret[ATLAS_APIKEY_SECRET_BYTES];
    bool ok = false;
    if (atlas_apikey_token_parse(token, sel, secret, &err) == ATLAS_OK) {
        atlas_apikey_record rec;
        bool found = false;
        if (atlas_db_apikey_lookup(db, sel, &rec, &found, &err) == ATLAS_OK && found) {
            ok = rec.status == ATLAS_APIKEY_STATUS_ACTIVE && !rec.scopes_unreadable &&
                 atlas_apikey_verify(secret, sizeof secret, rec.salt, sizeof rec.salt, rec.verifier,
                                     sizeof rec.verifier);
        }
    }
    memset(secret, 0, sizeof secret);
    atlas_db_close(db);
    atlas_buf_free(&db_path);
    return ok;
}

static void test_a_created_key_is_shown_once_and_never_again(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    atlas_buf out = ATLAS_BUF_INIT;

    const char *create[] = {"api-key", "create", "--label", "chatgpt",
                            "--scope",  "repo:read", "--scope", "decisions:read"};
    T_EQ_INT(run_key(&fx, create, 8, &out, &err), 0);

    char token[ATLAS_APIKEY_TOKEN_MAX];
    char id[ATLAS_APIKEY_SELECTOR_HEX + 1];
    token_of(&out, token, sizeof token);
    id_of(&out, id, sizeof id);

    /* The output says, in words, that it will not be shown again. That sentence
     * is part of the contract: an operator who does not know it discovers it
     * when the key is already lost. */
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "will not be shown again") != NULL,
                "the create output does not say the secret is shown once");
    T_CHECK(token_authenticates(&fx, token));

    /* The plaintext is nowhere in the database, searched as bytes. */
    atlas_buf db_path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&db_path, &err, "%s/atlas.db", fx_data_dir(&fx)), &err);
    T_CHECK_MSG(!file_contains(atlas_buf_cstr(&db_path), token),
                "the plaintext token is in the database file");
    {
        /* And neither is the secret half on its own, in case a prefix was
         * stripped somewhere on the way in.
         *
         * Split at the **known offset**, never at the last `_`. base64url's
         * alphabet ends `...789-_`, so about one secret in twenty contains an
         * underscore in its last two characters and `strrchr` then returns a
         * tail of one or two bytes — which is present in a file of any size, so
         * the assertion failed for a reason with nothing to do with what it was
         * asserting. Measured at 1 failure in 120 release runs before this
         * change; found by the P0 gate matrix, present since A9. */
        const char *body = token + strlen(ATLAS_APIKEY_PREFIX) + ATLAS_APIKEY_SELECTOR_HEX + 1u;
        T_CHECK_MSG(strlen(body) == ATLAS_APIKEY_SECRET_B64,
                    "the token's secret half is %zu characters, expected %u", strlen(body),
                    (unsigned)ATLAS_APIKEY_SECRET_B64);
        T_CHECK_MSG(!file_contains(atlas_buf_cstr(&db_path), body),
                    "the secret half of the token is in the database file");
    }

    /* No listing returns it, in either renderer. */
    const char *list[] = {"api-key", "list"};
    T_EQ_INT(run_key(&fx, list, 2, &out, &err), 0);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), token) == NULL, "list returned the secret");
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), id) != NULL, "list does not show the key id");
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "chatgpt") != NULL, "list does not show the label");

    const char *jlist[] = {"--json", "api-key", "list"};
    T_EQ_INT(run_key(&fx, jlist, 3, &out, &err), 0);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), token) == NULL, "the JSON listing returned the secret");
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"secrets_included\":false") != NULL,
                "the JSON listing does not state that it carries no secrets");

    atlas_buf_free(&db_path);
    atlas_buf_free(&out);
    fx_close(&fx);
}

static void test_a_revoked_key_stops_working_immediately(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    atlas_buf out = ATLAS_BUF_INIT;

    const char *create[] = {"api-key", "create", "--label", "temp", "--scope", "repo:read"};
    T_EQ_INT(run_key(&fx, create, 6, &out, &err), 0);
    char token[ATLAS_APIKEY_TOKEN_MAX];
    char id[ATLAS_APIKEY_SELECTOR_HEX + 1];
    token_of(&out, token, sizeof token);
    id_of(&out, id, sizeof id);
    T_CHECK(token_authenticates(&fx, token));

    const char *revoke[] = {"api-key", "revoke", id};
    T_EQ_INT(run_key(&fx, revoke, 3, &out, &err), 0);
    T_CHECK(strstr(atlas_buf_cstr(&out), "revoked") != NULL);

    /* Immediately, with nothing to invalidate: authentication is a lookup
     * against the row, so there is no cached verdict anywhere that could
     * outlive the revocation. */
    T_CHECK_MSG(!token_authenticates(&fx, token), "a revoked key still authenticates");

    /* Revoking twice is not an error and is not a lie: the outcome asked for
     * already holds, and the second call says nothing changed. */
    T_EQ_INT(run_key(&fx, revoke, 3, &out, &err), 0);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "nothing changed") != NULL,
                "revoking twice claimed to have revoked something");

    /* The row survives revocation. A credential that vanished would leave an
     * operator unable to tell a revoked key from one that never existed. */
    const char *list[] = {"api-key", "list"};
    T_EQ_INT(run_key(&fx, list, 2, &out, &err), 0);
    T_CHECK(strstr(atlas_buf_cstr(&out), id) != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&out), "REVOKED") != NULL);

    atlas_buf_free(&out);
    fx_close(&fx);
}

static void test_rotation_replaces_one_credential_with_another(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    atlas_buf out = ATLAS_BUF_INIT;

    const char *create[] = {"api-key", "create", "--label", "chatgpt", "--scope", "repo:read"};
    T_EQ_INT(run_key(&fx, create, 6, &out, &err), 0);
    char old_token[ATLAS_APIKEY_TOKEN_MAX];
    char old_id[ATLAS_APIKEY_SELECTOR_HEX + 1];
    token_of(&out, old_token, sizeof old_token);
    id_of(&out, old_id, sizeof old_id);

    const char *rotate[] = {"api-key", "rotate", old_id, "--label",
                            "chatgpt", "--scope", "repo:read"};
    T_EQ_INT(run_key(&fx, rotate, 7, &out, &err), 0);
    char new_token[ATLAS_APIKEY_TOKEN_MAX];
    char new_id[ATLAS_APIKEY_SELECTOR_HEX + 1];
    token_of(&out, new_token, sizeof new_token);
    id_of(&out, new_id, sizeof new_id);

    T_CHECK_MSG(strcmp(old_id, new_id) != 0, "rotation reused the key id");
    T_CHECK_MSG(strcmp(old_token, new_token) != 0, "rotation reused the token");
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "replaces") != NULL,
                "the rotation output does not say what it replaced");

    T_CHECK_MSG(token_authenticates(&fx, new_token), "the rotated-in key does not authenticate");
    T_CHECK_MSG(!token_authenticates(&fx, old_token), "the rotated-out key still authenticates");

    /* The link is recorded in both directions, so a revoked key can explain
     * itself. */
    const char *list[] = {"api-key", "list"};
    T_EQ_INT(run_key(&fx, list, 2, &out, &err), 0);
    T_CHECK(strstr(atlas_buf_cstr(&out), "replaced by") != NULL);

    /* Extended for A16: `--no-scopes` rotation, the deliberate form for
     * replacing a disposal credential. It is refused and accepted under the
     * same three sentences as create, and its success output carries one more
     * line an ordinary rotation does not: the operator must repoint the
     * policy's `remote_dispose_key` line, because rotation mints a new key id
     * and the old one no longer authenticates. */
    const char *disposal_create[] = {"api-key", "create", "--label", "disposal", "--no-scopes"};
    T_EQ_INT(run_key(&fx, disposal_create, 5, &out, &err), 0);
    char disposal_old_id[ATLAS_APIKEY_SELECTOR_HEX + 1];
    id_of(&out, disposal_old_id, sizeof disposal_old_id);

    const char *disposal_rotate[] = {"api-key", "rotate", disposal_old_id, "--label", "disposal",
                                     "--no-scopes"};
    T_EQ_INT(run_key(&fx, disposal_rotate, 6, &out, &err), 0);
    char disposal_new_id[ATLAS_APIKEY_SELECTOR_HEX + 1];
    id_of(&out, disposal_new_id, sizeof disposal_new_id);
    T_CHECK_MSG(strcmp(disposal_old_id, disposal_new_id) != 0,
                "a --no-scopes rotation reused the key id");

    /* A14, amended. The rotation reminder now names both policy lines a
     * --no-scopes key could be on. Assert the key-specific needle (the new
     * id) and the role-neutral tail, both of which survive the A14 wording. */
    char reminder[256];
    (void)snprintf(reminder, sizeof reminder,
                  "the policy line that names the previous key");
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), reminder) != NULL,
                "a --no-scopes rotation did not print the policy-line reminder: %s",
                atlas_buf_cstr(&out));
    char reminder_id[64];
    (void)snprintf(reminder_id, sizeof reminder_id, "key_%s", disposal_new_id);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), reminder_id) != NULL,
                "a --no-scopes rotation reminder did not name the new key id: %s",
                atlas_buf_cstr(&out));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "until it does, neither key can use the role") != NULL,
                "a --no-scopes rotation is missing the second half of the reminder: %s",
                atlas_buf_cstr(&out));

    /* An ordinary rotation (`repo:read`, not `--no-scopes`) must never print
     * this reminder: rotating the `chatgpt` key above did not, which is
     * re-checked here so a later change that made the reminder unconditional
     * would fail this test rather than only look untested. */
    const char *ordinary_rotate[] = {"api-key", "rotate", new_id, "--label", "chatgpt", "--scope",
                                     "repo:read"};
    T_EQ_INT(run_key(&fx, ordinary_rotate, 7, &out, &err), 0);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "remote_dispose_key") == NULL,
                "an ordinary rotation printed the disposal-only reminder: %s",
                atlas_buf_cstr(&out));

    atlas_buf_free(&out);
    fx_close(&fx);
}

static void test_creation_refuses_what_it_cannot_grant(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    atlas_buf out = ATLAS_BUF_INIT;

    struct {
        const char *name;
        const char *argv[8];
        size_t n;
    } cases[] = {
        {"an unknown scope",
         {"api-key", "create", "--label", "x", "--scope", "wat:read"}, 6},
        {"a scope no credential may hold",
         {"api-key", "create", "--label", "x", "--scope", "memory:write"}, 6},
        {"the disposal scope, which nothing can be granted",
         {"api-key", "create", "--label", "x", "--scope", "decisions:dispose"}, 6},
        {"the submission scope, which nothing can be granted",
         {"api-key", "create", "--label", "x", "--scope", "jobs:submit"}, 6},
        {"no scope at all", {"api-key", "create", "--label", "x"}, 4},
        {"no label", {"api-key", "create", "--scope", "repo:read"}, 4},
        {"a label with a newline in it",
         {"api-key", "create", "--label", "a\nb", "--scope", "repo:read"}, 6},
        {"a key id that is not one", {"api-key", "revoke", "not-an-id"}, 3},
        {"--scope and --no-scopes together",
         {"api-key", "create", "--label", "x", "--scope", "repo:read", "--no-scopes"}, 7},
    };

    /* One real credential first, so "nothing was created" is checked against a
     * count that would visibly grow rather than against an empty listing. Some
     * of these are refused at the command line before any index exists, and an
     * empty listing would then be indistinguishable from a listing that failed
     * to open anything. */
    const char *baseline[] = {"api-key", "create", "--label", "baseline", "--scope", "repo:read"};
    T_EQ_INT(run_key(&fx, baseline, 6, &out, &err), 0);

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int code = run_key(&fx, cases[i].argv, cases[i].n, &out, &err);
        T_CHECK_MSG(code == 2, "%s produced exit %d rather than a usage error", cases[i].name,
                    code);
        /* Nothing was created: the count is still the one baseline key. */
        atlas_buf list_out = ATLAS_BUF_INIT;
        const char *list[] = {"--json", "api-key", "list"};
        int lc = run_key(&fx, list, 3, &list_out, &err);
        T_CHECK_MSG(lc == 0, "listing failed after %s", cases[i].name);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&list_out), "\"count\":1") != NULL,
                    "%s changed the credential count", cases[i].name);
        atlas_buf_free(&list_out);
    }

    /* `memory:write` is refused by name, so the message tells an operator that
     * the scope exists and is not for them — rather than that it is unknown,
     * which would invite a bug report. */
    const char *w[] = {"api-key", "create", "--label", "x", "--scope", "memory:write"};
    (void)run_key(&fx, w, 6, &out, &err);

    /* `decisions:dispose` is refused with its own frozen sentence, naming how
     * the credential it belongs to is actually made — not the generic
     * "cannot be granted" text `memory:write` gets. */
    atlas_buf dout = ATLAS_BUF_INIT;
    atlas_buf derr = ATLAS_BUF_INIT;
    {
        const char *argv[8];
        size_t k = 0;
        argv[k++] = "--data-dir";
        argv[k++] = fx_data_dir(&fx);
        argv[k++] = "api-key";
        argv[k++] = "create";
        argv[k++] = "--label";
        argv[k++] = "x";
        argv[k++] = "--scope";
        argv[k++] = "decisions:dispose";
        int code = -1;
        T_OK(fx_atlas(argv, k, &dout, &derr, &code, &err), &err);
        T_CHECK_MSG(code == 2, "decisions:dispose produced exit %d rather than a usage error",
                    code);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&derr),
                           "decisions:dispose cannot be granted to a credential; it is derived "
                           "for the key /etc/atlas/gateway.conf names, and only for one that "
                           "holds no stored scope") != NULL,
                    "the refusal did not carry the frozen sentence: %s", atlas_buf_cstr(&derr));
    }
    atlas_buf_free(&dout);
    atlas_buf_free(&derr);

    /* `jobs:submit` is refused with its own frozen sentence, naming how
     * a credential gets this scope derived for it — through a
     * remote_submit_key policy line, not through api-key create. */
    atlas_buf jsout = ATLAS_BUF_INIT;
    atlas_buf jserr = ATLAS_BUF_INIT;
    {
        const char *argv[8];
        size_t k = 0;
        argv[k++] = "--data-dir";
        argv[k++] = fx_data_dir(&fx);
        argv[k++] = "api-key";
        argv[k++] = "create";
        argv[k++] = "--label";
        argv[k++] = "x";
        argv[k++] = "--scope";
        argv[k++] = "jobs:submit";
        int code = -1;
        T_OK(fx_atlas(argv, k, &jsout, &jserr, &code, &err), &err);
        T_CHECK_MSG(code == 2, "jobs:submit produced exit %d rather than a usage error", code);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&jserr),
                           "jobs:submit cannot be granted to a credential; it is derived "
                           "for the keys a remote_submit_key line in /etc/atlas/gateway.conf "
                           "names") != NULL,
                    "the refusal did not carry the frozen sentence: %s", atlas_buf_cstr(&jserr));
    }
    atlas_buf_free(&jsout);
    atlas_buf_free(&jserr);

    /* Neither `--scope` nor `--no-scopes`: the amended sentence, which says
     * `--no-scopes` exists rather than only that a scope is missing. */
    {
        atlas_buf nout = ATLAS_BUF_INIT, nerr = ATLAS_BUF_INIT;
        const char *argv[8];
        size_t k = 0;
        argv[k++] = "--data-dir";
        argv[k++] = fx_data_dir(&fx);
        argv[k++] = "api-key";
        argv[k++] = "create";
        argv[k++] = "--label";
        argv[k++] = "x";
        int code = -1;
        T_OK(fx_atlas(argv, k, &nout, &nerr, &code, &err), &err);
        T_CHECK_MSG(code == 2, "neither --scope nor --no-scopes produced exit %d", code);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&nerr),
                           "at least one --scope is required, or --no-scopes for a "
                           "remote-disposal credential; a credential with no scopes could "
                           "not read anything") != NULL,
                    "the refusal did not carry the amended sentence: %s", atlas_buf_cstr(&nerr));
        atlas_buf_free(&nout);
        atlas_buf_free(&nerr);
    }

    /* `--scope repo:read --no-scopes` together: the frozen sentence, checked
     * by content rather than only by exit code (the `cases[]` loop above only
     * checked the exit code and that nothing was created). */
    {
        atlas_buf bout = ATLAS_BUF_INIT, berr = ATLAS_BUF_INIT;
        const char *argv[10];
        size_t k = 0;
        argv[k++] = "--data-dir";
        argv[k++] = fx_data_dir(&fx);
        argv[k++] = "api-key";
        argv[k++] = "create";
        argv[k++] = "--label";
        argv[k++] = "x";
        argv[k++] = "--scope";
        argv[k++] = "repo:read";
        argv[k++] = "--no-scopes";
        int code = -1;
        T_OK(fx_atlas(argv, k, &bout, &berr, &code, &err), &err);
        T_CHECK_MSG(code == 2, "--scope with --no-scopes produced exit %d", code);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&berr), "--scope and --no-scopes cannot both be given") !=
                        NULL,
                    "the refusal did not carry the frozen sentence: %s", atlas_buf_cstr(&berr));
        atlas_buf_free(&bout);
        atlas_buf_free(&berr);
    }

    atlas_buf_free(&out);
    fx_close(&fx);
}

static void test_a_refusal_never_quotes_the_credential(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    atlas_buf out = ATLAS_BUF_INIT;

    /* A token-shaped value handed to a command that takes a key id. The refusal
     * must not echo it: an error message is a place a secret ends up in a
     * terminal, a shell history and a bug report. */
    static const char *const SECRETISH =
        "atlas_0123456789abcdef_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    const char *revoke[] = {"api-key", "revoke", SECRETISH};
    atlas_buf errout = ATLAS_BUF_INIT;
    const char *argv[8];
    size_t k = 0;
    argv[k++] = "--data-dir";
    argv[k++] = fx_data_dir(&fx);
    for (size_t i = 0; i < 3; i++) {
        argv[k++] = revoke[i];
    }
    int code = -1;
    T_OK(fx_atlas(argv, k, &out, &errout, &code, &err), &err);
    T_CHECK(code != 0);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), SECRETISH) == NULL,
                "the refusal echoed a token-shaped argument to stdout");
    T_CHECK_MSG(strstr(atlas_buf_cstr(&errout), SECRETISH) == NULL,
                "the refusal echoed a token-shaped argument to stderr");

    atlas_buf_free(&errout);
    atlas_buf_free(&out);
    fx_close(&fx);
}

static void test_the_two_renderers_agree(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    atlas_buf out = ATLAS_BUF_INIT;

    const char *create[] = {"api-key", "create", "--label", "both",
                            "--scope",  "repo:read", "--scope", "audit:read"};
    T_EQ_INT(run_key(&fx, create, 8, &out, &err), 0);
    char id[ATLAS_APIKEY_SELECTOR_HEX + 1];
    id_of(&out, id, sizeof id);

    const char *human[] = {"api-key", "list"};
    T_EQ_INT(run_key(&fx, human, 2, &out, &err), 0);
    T_CHECK(strstr(atlas_buf_cstr(&out), id) != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&out), "ACTIVE") != NULL);

    atlas_buf jout = ATLAS_BUF_INIT;
    const char *json[] = {"--json", "api-key", "list"};
    const char *argv[8];
    size_t k = 0;
    argv[k++] = "--data-dir";
    argv[k++] = fx_data_dir(&fx);
    for (size_t i = 0; i < 3; i++) {
        argv[k++] = json[i];
    }
    int code = -1;
    T_OK(fx_atlas(argv, k, &jout, NULL, &code, &err), &err);
    T_EQ_INT(code, 0);
    /* The same three facts, in the other renderer. A field present in one and
     * missing from the other is how the two descriptions of one credential
     * drift apart. */
    T_CHECK(strstr(atlas_buf_cstr(&jout), id) != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&jout), "\"status\":\"ACTIVE\"") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&jout), "\"label\":\"both\"") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&jout), "repo:read audit:read") != NULL);

    /* Extended for A16: a `--no-scopes` credential agrees between renderers
     * too. Human prints the frozen "(none)" block; JSON's `scopes` array is
     * empty and its `no_scopes` field says why, rather than a reader having to
     * infer the reason from an absence. */
    const char *nocreate[] = {"api-key", "create", "--label", "disposal", "--no-scopes"};
    T_EQ_INT(run_key(&fx, nocreate, 5, &out, &err), 0);
    char nid[ATLAS_APIKEY_SELECTOR_HEX + 1];
    id_of(&out, nid, sizeof nid);
    /* The frozen block carries the A14-amended second and third lines, which
     * name both root-owned lines and say the same key may never be named by
     * both. The old A16 wording said "one scope, decisions:dispose, and
     * nothing else" — that sentence is no longer in the output. */
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out),
                       "gives decisions:dispose, a remote_submit_key line gives jobs:submit, and\n"
                       "        the same key may never be named by both. Name it there, or revoke "
                       "it.") != NULL,
                "the --no-scopes create output is missing the A14-amended frozen block: %s",
                atlas_buf_cstr(&out));

    /* The JSON renderer's own creation output carries the same fact as a
     * field rather than only an empty array, so a caller parsing JSON is not
     * forced to fall back to the human prose to learn why. */
    atlas_buf jcreate = ATLAS_BUF_INIT;
    {
        const char *argv3[10];
        size_t k3 = 0;
        argv3[k3++] = "--data-dir";
        argv3[k3++] = fx_data_dir(&fx);
        argv3[k3++] = "--json";
        argv3[k3++] = "api-key";
        argv3[k3++] = "create";
        argv3[k3++] = "--label";
        argv3[k3++] = "disposal2";
        argv3[k3++] = "--no-scopes";
        int code3 = -1;
        T_OK(fx_atlas(argv3, k3, &jcreate, NULL, &code3, &err), &err);
        T_EQ_INT(code3, 0);
    }
    T_CHECK_MSG(strstr(atlas_buf_cstr(&jcreate), "\"no_scopes\":true") != NULL,
                "the JSON create output does not agree that the credential holds no scopes: %s",
                atlas_buf_cstr(&jcreate));
    atlas_buf_free(&jcreate);

    const char *nohuman[] = {"api-key", "list"};
    T_EQ_INT(run_key(&fx, nohuman, 2, &out, &err), 0);
    T_CHECK(strstr(atlas_buf_cstr(&out), nid) != NULL);
    /* `api-key list` shows the key with no scopes: the existing "(none)"
     * fallback for an empty scope string, exercised for the first time by a
     * key that can actually have one. */
    {
        const char *row = strstr(atlas_buf_cstr(&out), nid);
        T_REQUIRE(row != NULL);
        T_CHECK_MSG(strstr(row, "(none)") != NULL,
                    "the listing does not show the disposal credential as scopeless: %s", row);
    }

    atlas_buf njout = ATLAS_BUF_INIT;
    const char *nojson[] = {"--json", "api-key", "list"};
    {
        const char *argv2[8];
        size_t k2 = 0;
        argv2[k2++] = "--data-dir";
        argv2[k2++] = fx_data_dir(&fx);
        for (size_t i = 0; i < 3; i++) {
            argv2[k2++] = nojson[i];
        }
        int code2 = -1;
        T_OK(fx_atlas(argv2, k2, &njout, NULL, &code2, &err), &err);
        T_EQ_INT(code2, 0);
    }
    T_CHECK(strstr(atlas_buf_cstr(&njout), nid) != NULL);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&njout), "\"scopes\":\"\"") != NULL,
                "the JSON listing did not render an empty scope string: %s",
                atlas_buf_cstr(&njout));
    atlas_buf_free(&njout);

    atlas_buf_free(&jout);
    atlas_buf_free(&out);
    fx_close(&fx);
}

/* No fixture: these are pure functions over the closed vocabulary, so a
 * failure here means the table itself is wrong rather than anything about a
 * running command. */
static void test_the_disposal_scope_is_in_the_vocabulary_and_ungrantable(void) {
    T_CHECK(atlas_apikey_scope_parse("decisions:dispose") == ATLAS_SCOPE_DECISIONS_DISPOSE);
    T_CHECK_MSG(strcmp(atlas_apikey_scope_name(ATLAS_SCOPE_DECISIONS_DISPOSE),
                       "decisions:dispose") == 0,
                "the canonical name does not round-trip");
    T_CHECK_MSG(!atlas_apikey_scope_grantable(ATLAS_SCOPE_DECISIONS_DISPOSE),
                "decisions:dispose must never be grantable");

    /* Renders after memory:write, in enum/table order: a mask holding both
     * bits stores as one canonical byte string, "memory:write
     * decisions:dispose", never the reverse. */
    atlas_scope_mask both =
        ATLAS_SCOPE_BIT(ATLAS_SCOPE_MEMORY_WRITE) | ATLAS_SCOPE_BIT(ATLAS_SCOPE_DECISIONS_DISPOSE);
    atlas_buf rendered = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_apikey_scopes_render(both, &rendered, &err), &err);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&rendered), "memory:write decisions:dispose") == 0,
                "unexpected render order: %s", atlas_buf_cstr(&rendered));
    atlas_buf_free(&rendered);
}

/* No fixture: pure function over the closed vocabulary. */
static void test_the_submit_scope_is_in_the_vocabulary_and_ungrantable(void) {
    T_CHECK(atlas_apikey_scope_parse("jobs:submit") == ATLAS_SCOPE_JOBS_SUBMIT);
    T_CHECK_MSG(strcmp(atlas_apikey_scope_name(ATLAS_SCOPE_JOBS_SUBMIT),
                       "jobs:submit") == 0,
                "the canonical name does not round-trip");
    T_CHECK_MSG(!atlas_apikey_scope_grantable(ATLAS_SCOPE_JOBS_SUBMIT),
                "jobs:submit must never be grantable");

    /* Renders after decisions:dispose, in enum/table order: a mask holding
     * both bits stores as "decisions:dispose jobs:submit", never the reverse. */
    atlas_scope_mask both =
        ATLAS_SCOPE_BIT(ATLAS_SCOPE_DECISIONS_DISPOSE) |
        ATLAS_SCOPE_BIT(ATLAS_SCOPE_JOBS_SUBMIT);
    atlas_buf rendered = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_apikey_scopes_render(both, &rendered, &err), &err);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&rendered), "decisions:dispose jobs:submit") == 0,
                "unexpected render order: %s", atlas_buf_cstr(&rendered));
    atlas_buf_free(&rendered);
}

static const atlas_test TESTS[] = {
    {"a created key is shown once and never again",
     test_a_created_key_is_shown_once_and_never_again},
    {"a revoked key stops working immediately", test_a_revoked_key_stops_working_immediately},
    {"rotation replaces one credential with another",
     test_rotation_replaces_one_credential_with_another},
    {"creation refuses what it cannot grant", test_creation_refuses_what_it_cannot_grant},
    {"a refusal never quotes the credential", test_a_refusal_never_quotes_the_credential},
    {"the two renderers agree", test_the_two_renderers_agree},
    {"the disposal scope is in the vocabulary and ungrantable",
     test_the_disposal_scope_is_in_the_vocabulary_and_ungrantable},
    {"the submit scope is in the vocabulary and ungrantable",
     test_the_submit_scope_is_in_the_vocabulary_and_ungrantable},
};

ATLAS_TEST_MAIN("apikey", TESTS)
