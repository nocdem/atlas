/* Atlas - A9.2.1: the one write point for verification intake.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See `atlas/verify_ops.h` for what this exists to enforce. In short: a
 * submitter does not describe itself, so the three facts that decide what an
 * attestation is *worth* — the actor's class, the actor's identity, and whether
 * a piece of evidence was produced by something Atlas ran — are derived from the
 * channel the transport established and never read from the request.
 *
 * Every rule lives behind `atlas_verify_intake_apply_in_tx` for the reason
 * `settle()`, `atlas_db_evidence_insert`, `atlas_decision_apply_in_tx` and
 * `atlas_orch_apply_in_tx` all have one write point: a second path to the tables
 * would bypass every one of them, and the checks here are exactly the checks a
 * forger would want to be somewhere else.
 *
 * ## This file creates no process and reads no file
 *
 * Intake runs on the daemon's writer thread, where A1 forbids both. So every
 * reference is validated against **the index** rather than by asking git: a
 * commit against `commits`, a path against `files`, a content hash through
 * `atlas_db_verify_file_hash`. That is A6's rule about ancestry — "computed from
 * the index, never from a new git call" — and it is what lets validation happen
 * inside the transaction that writes the row.
 *
 * The cost is stated rather than hidden: Atlas validates against what it has
 * indexed. A commit that exists in the repository but has not been ingested is
 * refused, which is a false refusal and a recoverable one. The alternative —
 * accepting an unvalidated reference — is a false acceptance, and A2's rule is
 * that a gap is repairable and a wrong row is not.
 */
#include "atlas/verify_ops.h"

#include <stdio.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/decision.h"
#include "atlas/limits.h"
#include "atlas/memory.h"
#include "atlas/sha256.h"

/* --- the channel ----------------------------------------------------------- */

static const char *const CHANNEL_NAMES[] = {"UNKNOWN", "MODEL", "OPERATOR", "ATLAS", "DOCUMENT"};

const char *atlas_verify_channel_name(atlas_verify_channel c) {
    if ((size_t)c < sizeof CHANNEL_NAMES / sizeof CHANNEL_NAMES[0]) {
        return CHANNEL_NAMES[c];
    }
    return "UNKNOWN";
}

bool atlas_verify_channel_is_transport_selectable(atlas_verify_channel c) {
    switch (c) {
    case ATLAS_VERIFY_CHANNEL_MODEL:
        /* The channel every MCP tool call and ordinary RPC peer speaks on. */
        return true;
    case ATLAS_VERIFY_CHANNEL_OPERATOR:
        /* Nameable, and honoured only downwards: the transport edge's strict
         * rank comparison refuses every raise. */
        return true;
    case ATLAS_VERIFY_CHANNEL_UNKNOWN:
        /* Zero means nothing established the caller, and a request cannot
         * establish that nothing established it. */
        return false;
    case ATLAS_VERIFY_CHANNEL_ATLAS:
        /* A request that could name this channel could make its own evidence
         * authentic. */
        return false;
    case ATLAS_VERIFY_CHANNEL_DOCUMENT:
        /* A request that could name this channel could mint one independent
         * speaker per pasted file — §12 inflation. */
        return false;
    }
    return false;
}

bool atlas_verify_channel_parse(const char *name, atlas_verify_channel *out) {
    if (name == NULL) {
        return false;
    }
    /* One walk over the vocabulary, gated on the one definition of
     * transport-selectability. ATLAS, DOCUMENT and UNKNOWN are refused by the
     * predicate rather than by absence from a hand-kept list: the predicate is
     * a switch with no `default:`, so a channel added to the vocabulary
     * without deciding whether a transport may name it does not compile — and
     * a request that could name its own channel could name the one that makes
     * its evidence authentic, or the one that mints a speaker per file. */
    for (size_t i = 0; i < sizeof CHANNEL_NAMES / sizeof CHANNEL_NAMES[0]; i++) {
        atlas_verify_channel c = (atlas_verify_channel)i;
        if (atlas_verify_channel_is_transport_selectable(c) &&
            strcmp(name, CHANNEL_NAMES[i]) == 0) {
            *out = c;
            return true;
        }
    }
    return false;
}

int atlas_verify_channel_authority(atlas_verify_channel c) {
    switch (c) {
    case ATLAS_VERIFY_CHANNEL_MODEL:
        return 1;
    case ATLAS_VERIFY_CHANNEL_OPERATOR:
        return 2;
    case ATLAS_VERIFY_CHANNEL_ATLAS:
        return 3;
    case ATLAS_VERIFY_CHANNEL_DOCUMENT:
        /* Beside MODEL, and inert: the parse never yields DOCUMENT, so no rank
         * comparison ever sees it. The rank exists so this switch stays total
         * and nobody reads an unranked member as outranking anything — it must
         * never become a guard, because for an operator peer it is none: 1 < 2,
         * so the strict weakening comparison would admit the name the moment a
         * parse accepted it. See the header on `atlas_verify_channel_authority`. */
        return 1;
    case ATLAS_VERIFY_CHANNEL_UNKNOWN:
        break;
    }
    /* The house rule: a `memset` must not produce a channel that outranks
     * anything, so the zero value sits below every real one. */
    return 0;
}

atlas_verify_actor_class atlas_verify_channel_actor_class(atlas_verify_channel c) {
    switch (c) {
    case ATLAS_VERIFY_CHANNEL_MODEL:
        return ATLAS_ACTOR_AI_AGENT;
    case ATLAS_VERIFY_CHANNEL_OPERATOR:
        return ATLAS_ACTOR_HUMAN;
    case ATLAS_VERIFY_CHANNEL_ATLAS:
        return ATLAS_ACTOR_ATLAS_VERIFIER;
    case ATLAS_VERIFY_CHANNEL_DOCUMENT:
        /* The document itself is the speaker — never the model that pasted it
         * and never Atlas, which only read it. */
        return ATLAS_ACTOR_DOCUMENT;
    case ATLAS_VERIFY_CHANNEL_UNKNOWN:
        break;
    }
    return ATLAS_ACTOR_UNKNOWN;
}

atlas_verify_actor_identity atlas_verify_channel_actor_identity(atlas_verify_channel c) {
    switch (c) {
    case ATLAS_VERIFY_CHANNEL_MODEL:
        /* Always. The transport carries no cryptographic statement about which
         * model is speaking, so §11's instruction is to mark the metadata
         * asserted rather than pretend to a certainty nothing established. */
        return ATLAS_ACTOR_IDENTITY_SELF_DECLARED;
    case ATLAS_VERIFY_CHANNEL_OPERATOR:
        return ATLAS_ACTOR_IDENTITY_PEER_AUTHENTICATED;
    case ATLAS_VERIFY_CHANNEL_ATLAS:
        return ATLAS_ACTOR_IDENTITY_ATLAS_ATTESTED;
    case ATLAS_VERIFY_CHANNEL_DOCUMENT:
        /* Prose in a file is asserted by whoever wrote the file, and nothing
         * authenticates the writer. The cap this imposes is the season's
         * sentence as an integer: min(DOCUMENT 400, SELF_DECLARED 350) = 350.
         * At ATLAS_ATTESTED a memory file would weigh 400 — above the
         * self-declared model that wrote it and second only to a human — so a
         * sentence anybody types into a memory file would outweigh the model
         * speaking directly, arriving as a number nobody chose. */
        return ATLAS_ACTOR_IDENTITY_SELF_DECLARED;
    case ATLAS_VERIFY_CHANNEL_UNKNOWN:
        break;
    }
    return ATLAS_ACTOR_IDENTITY_SELF_DECLARED;
}

bool atlas_verify_evidence_class_requires_atlas_production(atlas_verify_evidence_class c) {
    switch (c) {
    /* The four whose whole evidentiary weight is that Atlas *did* the thing. */
    case ATLAS_EVIDENCE_COMPILER:
    case ATLAS_EVIDENCE_TEST:
    case ATLAS_EVIDENCE_RUNTIME:
    case ATLAS_EVIDENCE_DEPLOYED_CONFIG:
        return true;
    /* Enumerated rather than defaulted, so a class added to the vocabulary
     * without deciding this question does not compile. Which side a new class
     * falls on is a security decision and must not have a default. */
    case ATLAS_EVIDENCE_UNKNOWN:
    case ATLAS_EVIDENCE_SOURCE_CODE:
    case ATLAS_EVIDENCE_GIT_HISTORY:
    case ATLAS_EVIDENCE_SPECIFICATION:
    case ATLAS_EVIDENCE_DOCUMENT:
    case ATLAS_EVIDENCE_ATLAS_KNOWLEDGE:
    case ATLAS_EVIDENCE_HUMAN_STATEMENT:
    case ATLAS_EVIDENCE_AI_ANALYSIS:
        break;
    }
    return false;
}

static const char *const OP_NAMES[] = {"CLAIM_CREATE",     "EVIDENCE_ADD",    "EVIDENCE_PRODUCE",
                                       "ATTESTATION_ADD",  "DEPENDENCY_ADD",  "EVALUATE"};

const char *atlas_verify_op_kind_name(atlas_verify_op_kind k) {
    if ((size_t)k < sizeof OP_NAMES / sizeof OP_NAMES[0]) {
        return OP_NAMES[k];
    }
    return "CLAIM_CREATE";
}

bool atlas_verify_op_is_evaluation(atlas_verify_op_kind k) {
    return k == ATLAS_VERIFY_OP_EVALUATE;
}

/* --- lifetimes ------------------------------------------------------------- */

/* Every owned member in one table, so `_init` and `_free` cannot drift apart.
 * A9.2's own postmortem names this failure: a struct member added to one and not
 * the other is a leak or a use of uninitialised memory that only a sanitizer
 * finds, and only when the stack layout happens to change. */
#define VERIFY_OP_BUFS(X)                                                                          \
    X(repo_name) X(root) X(actor_name) X(actor_provider) X(actor_family) X(actor_version)          \
    X(actor_role) X(session_key) X(run_id) X(parent_actor_uid) X(document_uid) X(domain) X(text)   \
    X(scope_note) X(verifier) X(verifier_input) X(basis_commit) X(environment) X(claim_uid)        \
    X(commit_oid) X(path_text) X(symbol) X(target) X(probe) X(observed) X(observed_at)             \
    X(memory_version_uid) X(method) X(supersedes_uid) X(evidence_uids) X(derived_uid)              \
    X(source_uid)

void atlas_verify_op_init(atlas_verify_op *op) {
    if (op == NULL) {
        return;
    }
    memset(op, 0, sizeof *op);
#define X(f) atlas_buf_init(&op->f);
    VERIFY_OP_BUFS(X)
#undef X
    /* -1 rather than 0: an actor that stated no confidence is not an actor that
     * stated zero confidence. */
    op->self_confidence = -1;
}

void atlas_verify_op_free(atlas_verify_op *op) {
    if (op == NULL) {
        return;
    }
#define X(f) atlas_buf_free(&op->f);
    VERIFY_OP_BUFS(X)
#undef X
    memset(op, 0, sizeof *op);
}

void atlas_verify_intake_result_init(atlas_verify_intake_result *r) {
    if (r == NULL) {
        return;
    }
    memset(r, 0, sizeof *r);
    atlas_buf_init(&r->uid);
    atlas_buf_init(&r->actor_uid);
    atlas_buf_init(&r->repo_name);
    atlas_verify_assessment_init(&r->assessment);
}

void atlas_verify_intake_result_free(atlas_verify_intake_result *r) {
    if (r == NULL) {
        return;
    }
    atlas_buf_free(&r->uid);
    atlas_buf_free(&r->actor_uid);
    atlas_buf_free(&r->repo_name);
    memset(r, 0, sizeof *r);
}

/* --- content keys ----------------------------------------------------------
 *
 * Domain-separated and **length-prefixed**, never delimited, which is A4's rule
 * about the canonical content hash and it holds for exactly its reason: with any
 * single-byte delimiter a path of `a|b` at commit `c` encodes identically to a
 * path of `a` at commit `b|c`, so two different references would share a key and
 * the second would silently resolve to the first.
 *
 * What goes into a key is what makes the object *the same object*. What stays
 * out is anything that varies between two submissions of the same fact — the
 * submitting actor, the wall clock, the uid Atlas is about to mint. §27 states
 * both directions and the second is the sharp one: the commit is in the key
 * because the same text at a different revision is a different fact. */
#define KEY_DOMAIN_CLAIM "atlas-verify-claim-key-v1"
#define KEY_DOMAIN_EVIDENCE "atlas-verify-evidence-key-v1"
#define KEY_DOMAIN_ATTESTATION "atlas-verify-attestation-key-v1"

static void key_field(atlas_sha256 *h, const void *data, size_t len) {
    unsigned char prefix[8];
    size_t n = len;
    for (int i = 7; i >= 0; i--) {
        prefix[i] = (unsigned char)(n & 0xffu);
        n >>= 8;
    }
    atlas_sha256_update(h, prefix, sizeof prefix);
    if (len > 0) {
        atlas_sha256_update(h, data, len);
    }
}

static void key_buf(atlas_sha256 *h, const atlas_buf *b) {
    key_field(h, b->data, b->len);
}

static void key_str(atlas_sha256 *h, const char *s) {
    key_field(h, s, s == NULL ? 0 : strlen(s));
}

static void key_i64(atlas_sha256 *h, int64_t v) {
    char t[32];
    int n = snprintf(t, sizeof t, "%lld", (long long)v);
    key_field(h, t, n < 0 ? 0 : (size_t)n);
}

static atlas_status key_finish(atlas_sha256 *h, atlas_buf *out, atlas_err *err) {
    unsigned char digest[ATLAS_SHA256_DIGEST_LEN];
    atlas_sha256_final(h, digest);
    char hex[ATLAS_SHA256_DIGEST_LEN * 2 + 1];
    static const char HEXDIGITS[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof digest; i++) {
        hex[i * 2] = HEXDIGITS[digest[i] >> 4];
        hex[i * 2 + 1] = HEXDIGITS[digest[i] & 0x0fu];
    }
    hex[sizeof hex - 1] = '\0';
    return atlas_buf_set(out, hex, sizeof hex - 1, err);
}

/* --- bounded, printable input ----------------------------------------------
 *
 * Intake accepts text written by a model. It is UNTRUSTED_DATA and stays so
 * wherever it is reported; what is checked here is only that it is *bounded*,
 * because an unbounded field is a way to make the index grow without a claim
 * being made. Nothing here tries to decide whether prose is true — that is what
 * the rest of the phase is for.
 *
 * Refused rather than truncated, A5's rule about bounds: a shortened proposition
 * is a different proposition, and one whose scope has quietly widened. */
static atlas_status bounded(const atlas_buf *b, size_t max, const char *what, atlas_err *err) {
    if (b->len > max) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "%s is longer than %u bytes; it is refused rather than shortened, "
                             "because a truncated statement is a different statement",
                             what, (unsigned)max);
    }
    return ATLAS_OK;
}

/* A domain is a short lowercase key used to partition reliability. Checked
 * rather than escaped, the rule A2's context envelope follows: a value that is
 * not the shape it claims to be is refused, not reproduced. */
static atlas_status check_domain(const atlas_buf *d, atlas_err *err) {
    if (d->len > ATLAS_VERIFY_DOMAIN_MAX) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a claim domain is at most %u bytes",
                             (unsigned)ATLAS_VERIFY_DOMAIN_MAX);
    }
    for (size_t i = 0; i < d->len; i++) {
        char c = d->data[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
                  c == '_';
        if (!ok) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "a claim domain is lowercase letters, digits, '.', '-' and '_'; "
                                 "it keys reliability and is never rendered as control bytes");
        }
    }
    return ATLAS_OK;
}

/* A commit oid as Atlas stores them: lowercase hex, bounded. Validated for
 * shape here and for *existence* against the index below. */
static bool looks_like_oid(const atlas_buf *b) {
    if (b->len == 0 || b->len > ATLAS_OID_HEX_MAX) {
        return false;
    }
    for (size_t i = 0; i < b->len; i++) {
        char c = b->data[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

/* --- resolving the repository ---------------------------------------------- */

static atlas_status resolve_repo(atlas_db *db, const atlas_verify_op *op, atlas_repo_info *out,
                                 atlas_err *err) {
    bool found = false;
    atlas_status st;
    if (op->root.len > 0) {
        st = atlas_db_repo_get_by_root(db, op->root.data, op->root.len, out, &found, err);
    } else if (op->repo_name.len > 0) {
        st = atlas_db_repo_get(db, op->repo_name.data, out, &found, err);
    } else {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "name the repository this claim is about; a verification claim with "
                             "no repository is a claim about nothing in particular");
    }
    if (st != ATLAS_OK) {
        return st;
    }
    if (!found) {
        /* A2's rule: the same answer whether the directory exists, is a git
         * repository, or is nothing. Atlas does not look at the filesystem to
         * produce it, so the refusal reveals nothing about the machine. */
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "no repository by that name is registered; only an operator registers "
                             "one");
    }
    return ATLAS_OK;
}

/* --- the actor -------------------------------------------------------------
 *
 * §10 and §11. The class and the identity come from the channel. Everything
 * descriptive comes from the submitter and is stored as asserted metadata, which
 * is why `identity` sits beside it on the row: a reader can always tell which
 * half was established and which half was typed.
 *
 * The uid is derived from the descriptive fields so that the same speaker in the
 * same session resolves to one actor across calls. That is what makes "three
 * models attested" countable — and, just as importantly, what stops one model
 * retrying four times from looking like four sources. */
static atlas_status derive_actor(atlas_db *db, const atlas_verify_op *op, const char *now,
                                 int64_t *actor_id_out, atlas_buf *actor_uid_out, atlas_err *err) {
    atlas_verify_actor_class cls = atlas_verify_channel_actor_class(op->channel);
    atlas_verify_actor_identity ident = atlas_verify_channel_actor_identity(op->channel);

    atlas_sha256 h;
    atlas_sha256_init(&h);
    key_str(&h, "atlas-verify-actor-uid-v1");
    key_str(&h, atlas_verify_actor_class_name(cls));
    key_str(&h, atlas_verify_actor_identity_name(ident));
    key_buf(&h, &op->actor_name);
    key_buf(&h, &op->actor_provider);
    key_buf(&h, &op->actor_family);
    key_buf(&h, &op->actor_version);
    key_buf(&h, &op->actor_role);
    key_buf(&h, &op->session_key);
    key_buf(&h, &op->run_id);
    atlas_buf digest = ATLAS_BUF_INIT;
    atlas_status st = key_finish(&h, &digest, err);
    if (st != ATLAS_OK) {
        return st;
    }

    atlas_verify_actor a;
    atlas_verify_actor_init(&a);
    a.cls = cls;
    a.identity = ident;
    char uid[80];
    (void)snprintf(uid, sizeof uid, "atlas-actor-%.32s", digest.data);
    atlas_buf_free(&digest);

    struct {
        atlas_buf *to;
        const atlas_buf *from;
    } copies[] = {
        {&a.name, &op->actor_name},         {&a.provider, &op->actor_provider},
        {&a.family, &op->actor_family},     {&a.version, &op->actor_version},
        {&a.role, &op->actor_role},         {&a.session_key, &op->session_key},
        {&a.run_id, &op->run_id},
    };
    st = atlas_buf_set(&a.uid, uid, strlen(uid), err);
    for (size_t i = 0; st == ATLAS_OK && i < sizeof copies / sizeof copies[0]; i++) {
        st = atlas_buf_set(copies[i].to, copies[i].from->data, copies[i].from->len, err);
    }
    if (st == ATLAS_OK && op->parent_actor_uid.len > 0) {
        /* An orchestrator, by uid. Validated: a parent pointer to nothing would
         * make an orchestrated fleet look like unrelated speakers, which is the
         * §12 inflation this phase exists to prevent. */
        bool found = false;
        int64_t parent = 0;
        st = atlas_db_verify_actor_find(db, op->parent_actor_uid.data, &parent, &found, err);
        if (st == ATLAS_OK && !found) {
            st = atlas_err_set(err, ATLAS_ERR_USAGE,
                               "no actor by that uid has spoken here, so it cannot be named as an "
                               "orchestrator");
        }
        if (st == ATLAS_OK) {
            a.parent_actor_id = parent;
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_db_verify_actor_upsert(db, &a, now, err);
    }
    if (st == ATLAS_OK) {
        *actor_id_out = a.id;
        st = atlas_buf_set(actor_uid_out, a.uid.data, a.uid.len, err);
    }
    atlas_verify_actor_free(&a);
    return st;
}

/* --- source binding --------------------------------------------------------
 *
 * §4. Every repository-backed object binds to an explicit source state, and the
 * state Atlas can honestly bind to is **the one it has indexed**. `scanned_head`
 * is that: the commit the index describes, read from the repository row inside
 * the caller's transaction, so it is a consistent snapshot rather than a race
 * against the watcher.
 *
 * A caller may name a commit explicitly, and then it is checked for existence
 * against `commits`. A caller that names none is bound to the indexed head
 * rather than left unbound — §3's rule that a claim must not silently point at
 * whichever HEAD happens to exist later. */
static atlas_status bind_commit(atlas_db *db, const atlas_repo_info *repo, const atlas_buf *asked,
                                atlas_buf *out, atlas_err *err) {
    if (asked->len == 0) {
        if (repo->scanned_head[0] == '\0') {
            return atlas_err_set(err, ATLAS_ERR_CONFIG,
                                 "this repository has not been indexed, so Atlas cannot say what "
                                 "repository state a claim about it would be true of");
        }
        return atlas_buf_set(out, repo->scanned_head, strlen(repo->scanned_head), err);
    }
    if (!looks_like_oid(asked)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a commit is lowercase hex; this is not one");
    }
    bool found = false;
    atlas_status st = atlas_db_verify_commit_exists(db, repo->id, asked->data, &found, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (!found) {
        /* Fail closed. Atlas validates against what it has ingested, and says
         * so: this is a false refusal when the commit is real but unindexed,
         * which is recoverable, rather than a false acceptance, which is not. */
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "Atlas has not indexed that commit for this repository, so it cannot "
                             "bind evidence to it; index the repository first");
    }
    return atlas_buf_set(out, asked->data, asked->len, err);
}

/* --- claim ----------------------------------------------------------------- */

static atlas_status op_claim_create(atlas_db *db, const atlas_verify_op *op, const char *now,
                                    atlas_verify_intake_result *out, atlas_err *err) {
    atlas_repo_info repo;
    atlas_repo_info_init(&repo);
    atlas_status st = resolve_repo(db, op, &repo, err);

    if (st == ATLAS_OK) {
        st = bounded(&op->text, ATLAS_VERIFY_CLAIM_TEXT_MAX, "a claim's proposition", err);
    }
    if (st == ATLAS_OK) {
        st = bounded(&op->scope_note, ATLAS_VERIFY_SCOPE_MAX, "a claim's scope note", err);
    }
    if (st == ATLAS_OK) {
        st = check_domain(&op->domain, err);
    }
    if (st == ATLAS_OK && op->text.len == 0) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "a claim needs a proposition to state");
    }

    /* §14. A caller names a verifier; the verifier decides what it establishes,
     * and the root-owned policy decides whether that may move anything. Naming
     * one that does not exist is refused rather than ignored: a claim carrying a
     * verifier nobody runs would report as merely empirical while reading, to
     * anybody scanning a list, as though it were mechanically checkable. */
    atlas_verify_verifier v = ATLAS_VERIFIER_NONE;
    if (st == ATLAS_OK && op->verifier.len > 0) {
        if (!atlas_verify_verifier_parse(op->verifier.data, &v)) {
            st = atlas_err_set(err, ATLAS_ERR_USAGE,
                               "no deterministic verifier by that name exists; the allowlist is "
                               "fixed and a claim cannot name its own checker");
        }
    }

    atlas_verify_claim c;
    atlas_verify_claim_init(&c);

    /* The knowledge record, by uid, and the exact revision. A revision rather
     * than a document: an old revision of a rule and the current one are
     * different propositions and must not share a verdict. */
    if (st == ATLAS_OK && op->document_uid.len > 0) {
        bool found = false;
        int64_t doc = 0;
        int64_t doc_repo = 0;
        st = atlas_db_decision_find_uid(db, op->document_uid.data, &doc, &doc_repo, &found, err);
        if (st == ATLAS_OK && !found) {
            st = atlas_err_set(err, ATLAS_ERR_CONFIG,
                               "no knowledge record by that id exists; a claim may not be attached "
                               "to a record that is not there");
        }
        if (st == ATLAS_OK) {
            c.document_id = doc;
            /* The *approved* revision, derived from `decision_revisions` rather
             * than read from the status cache — A9.1's rule about
             * `recompute_status`. 0 when nothing is approved, which is correct
             * and useful: `INVARIANT + PROPOSED + VERIFIED` is a legal and
             * meaningful combination, so a claim about an unapproved record is
             * not an error. */
            st = atlas_db_decision_approved_revision(db, doc, &c.revision_id, err);
        }
    }

    if (st == ATLAS_OK) {
        st = bind_commit(db, &repo, &op->basis_commit, &c.basis_commit, err);
    }
    /* The durable identity, so a claim survives re-registration the way A4's
     * decision records do. A path-qualified lineage fingerprint, computed by the
     * one function that knows how. */
    if (st == ATLAS_OK) {
        st = atlas_db_repo_identity_hash(db, repo.id, &c.repo_identity_hash, err);
    }
    if (st == ATLAS_OK) {
        c.repo_id = repo.id;
        c.semantics = op->semantics_given ? op->semantics : ATLAS_CLAIM_DESCRIPTIVE;
        c.created_by_actor_id = out->actor_id;
        struct {
            atlas_buf *to;
            const atlas_buf *from;
        } copies[] = {
            {&c.domain, &op->domain},
            {&c.text, &op->text},
            {&c.scope_note, &op->scope_note},
            {&c.verifier_input, &op->verifier_input},
            {&c.environment, &op->environment},
        };
        for (size_t i = 0; st == ATLAS_OK && i < sizeof copies / sizeof copies[0]; i++) {
            st = atlas_buf_set(copies[i].to, copies[i].from->data, copies[i].from->len, err);
        }
        if (st == ATLAS_OK && v != ATLAS_VERIFIER_NONE) {
            const char *n = atlas_verify_verifier_name(v);
            st = atlas_buf_set(&c.verifier, n, strlen(n), err);
        }
    }

    /* §27. The key covers what makes it the same proposition: the repository,
     * the record revision, the text, the scope, the semantics, the verifier and
     * its input, and the commit it is bound to. Not the author and not the
     * clock — two people stating the same claim about the same tree are stating
     * one claim, and the second should attest to it rather than fork it. */
    if (st == ATLAS_OK) {
        atlas_sha256 h;
        atlas_sha256_init(&h);
        key_str(&h, KEY_DOMAIN_CLAIM);
        key_buf(&h, &c.repo_identity_hash);
        key_i64(&h, c.document_id);
        key_i64(&h, c.revision_id);
        key_buf(&h, &c.domain);
        key_buf(&h, &c.text);
        key_buf(&h, &c.scope_note);
        key_str(&h, atlas_verify_claim_semantics_name(c.semantics));
        key_buf(&h, &c.verifier);
        key_buf(&h, &c.verifier_input);
        key_buf(&h, &c.basis_commit);
        key_buf(&h, &c.environment);
        st = key_finish(&h, &c.content_key, err);
    }
    if (st == ATLAS_OK) {
        bool found = false;
        int64_t existing = 0;
        st = atlas_db_verify_claim_by_key(db, c.content_key.data, &existing, &out->uid, &found,
                                          err);
        if (st == ATLAS_OK && found) {
            out->claim_id = existing;
            out->duplicate = true;
            out->repo_id = repo.id;
            atlas_verify_claim_free(&c);
            atlas_repo_info_free(&repo);
            return ATLAS_OK;
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_db_verify_claim_insert(db, &c, now, err);
    }
    if (st == ATLAS_OK) {
        out->claim_id = c.id;
        out->repo_id = repo.id;
        st = atlas_buf_set(&out->uid, c.uid.data, c.uid.len, err);
    }
    atlas_verify_claim_free(&c);
    atlas_repo_info_free(&repo);
    return st;
}

/* --- evidence --------------------------------------------------------------
 *
 * §7 and §8. Two paths that must never be confused:
 *
 *   EVIDENCE_ADD      — a *reference*. The submitter says where to look; Atlas
 *                       checks that the place exists and records what is
 *                       actually there. The producer is the submitter.
 *   EVIDENCE_PRODUCE  — Atlas *does* the thing and records what it found. The
 *                       producer is Atlas, and only this path may say so.
 *
 * The forgery §33 describes — a model submitting `class=COMPILER, producer=clang,
 * result=PROVEN` — is refused on the first path by
 * `atlas_verify_evidence_class_requires_atlas_production`, and is unreachable on
 * the second because the second does not accept a verdict. */
static atlas_status resolve_claim(atlas_db *db, const atlas_buf *uid, atlas_verify_claim *out,
                                  atlas_err *err) {
    if (uid->len == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "name the claim this concerns");
    }
    bool found = false;
    atlas_status st = atlas_db_verify_claim_find(db, uid->data, out, &found, err);
    if (st == ATLAS_OK && !found) {
        st = atlas_err_set(err, ATLAS_ERR_CONFIG, "no claim by that id exists");
    }
    return st;
}

static atlas_status op_evidence_add(atlas_db *db, const atlas_verify_op *op, const char *now,
                                    atlas_verify_intake_result *out, atlas_err *err) {
    if (op->evidence_class == ATLAS_EVIDENCE_UNKNOWN) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "evidence must say what sort of thing it is; there is no "
                             "unclassified evidence");
    }
    /* §33. Refused rather than stored-and-discounted: a discounted forgery still
     * appears in the evidence list, still reads as tool output to somebody
     * skimming a UI, and still has to be argued away by whoever finds it. */
    if (op->channel != ATLAS_VERIFY_CHANNEL_ATLAS &&
        atlas_verify_evidence_class_requires_atlas_production(op->evidence_class)) {
        return atlas_err_set(
            err, ATLAS_ERR_USAGE,
            "%s evidence records something Atlas ran, so it cannot be submitted; ask Atlas to "
            "produce it, or record what you read as AI_ANALYSIS declaring its source",
            atlas_verify_evidence_class_name(op->evidence_class));
    }
    /* A12.1. A memory snapshot reference is internal: Atlas' own pass read the
     * bytes and stored the row this uid names, so on a transport-selectable
     * channel the field is refused and no transport gains a byte of new
     * surface. Asked of the predicate rather than of two named channels, so a
     * future selectable channel cannot inherit the field by omission. */
    if (op->memory_version_uid.len > 0 &&
        atlas_verify_channel_is_transport_selectable(op->channel)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a memory snapshot is bound by the pass that read it; name a path "
                             "Atlas has indexed instead");
    }
    /* One reference per row. Only Atlas' own code can construct an op carrying
     * this field, so both set at once is a defect in that code — refused
     * rather than resolved by a silent precedence, which would store evidence
     * about which bytes nobody could say. */
    if (op->memory_version_uid.len > 0 && op->path_text.len > 0) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "evidence refers to an indexed path or to a stored memory snapshot, "
                             "never both at once");
    }

    atlas_verify_claim claim;
    atlas_verify_claim_init(&claim);
    atlas_status st = resolve_claim(db, &op->claim_uid, &claim, err);

    atlas_repo_info repo;
    atlas_repo_info_init(&repo);
    if (st == ATLAS_OK && claim.repo_id != 0) {
        bool found = false;
        st = atlas_db_repo_get_by_id(db, claim.repo_id, &repo, &found, err);
        if (st == ATLAS_OK && !found) {
            st = atlas_err_set(err, ATLAS_ERR_CONFIG,
                               "the repository this claim was about is no longer registered");
        }
    }

    atlas_verify_evidence e;
    atlas_verify_evidence_init(&e);
    if (st == ATLAS_OK) {
        e.cls = op->evidence_class;
        e.repo_id = claim.repo_id;
        e.actor_id = out->actor_id;
        e.line_start = op->line_start;
        e.line_end = op->line_end;
    }
    if (st == ATLAS_OK) {
        st = bounded(&op->observed, ATLAS_VERIFY_SCOPE_MAX, "an observation", err);
    }

    /* The commit this evidence is *of*. Bound explicitly, validated against the
     * index, never left to whatever HEAD is when somebody reads the row. */
    if (st == ATLAS_OK && claim.repo_id != 0) {
        const atlas_buf *asked = op->commit_oid.len > 0 ? &op->commit_oid : &claim.basis_commit;
        st = bind_commit(db, &repo, asked, &e.commit_oid, err);
    }

    /* §8: Atlas binds the actual repository object rather than trusting prose.
     * The submitter says which path; Atlas looks the path up in its own index
     * and records the content hash *it* found. A path that is not there is
     * refused — a reference to a file that does not exist is not weak evidence,
     * it is a reference to nothing. */
    if (st == ATLAS_OK && op->path_text.len > 0) {
        st = atlas_buf_set(&e.path_text, op->path_text.data, op->path_text.len, err);
        if (st == ATLAS_OK) {
            bool found = false;
            st = atlas_db_verify_file_hash(db, claim.repo_id, e.path_text.data, &e.content_hash,
                                           &found, err);
            if (st == ATLAS_OK && !found) {
                st = atlas_err_set(err, ATLAS_ERR_CONFIG,
                                   "Atlas has no such path indexed in that repository, so there is "
                                   "nothing for this evidence to refer to");
            }
        }
    }

    /* A12.1 — §8 for a file the index cannot hold. An external memory source's
     * absolute path can never pass the lookup above, so the internal caller
     * names a stored `memory_source_versions` row instead, and every fact the
     * evidence takes from it is Atlas' own: the content hash the pass computed
     * when it read the bytes, the source's stored path, and — when the version
     * was git-bound — the commit *it* recorded, overriding the claim's default
     * binding above. Nothing here is taken from the request; the op has no
     * content-hash field to take it from, deliberately. */
    if (st == ATLAS_OK && op->memory_version_uid.len > 0) {
        atlas_memory_version_row mv;
        atlas_memory_version_row_init(&mv);
        bool found = false;
        st = atlas_db_memory_version_by_uid(db, atlas_buf_cstr(&op->memory_version_uid), &mv,
                                            &found, err);
        if (st == ATLAS_OK && !found) {
            st = atlas_err_set(err, ATLAS_ERR_CONFIG,
                               "no memory source version by that id exists, so there is nothing "
                               "for this evidence to refer to");
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_set(&e.content_hash, mv.content_sha256.data, mv.content_sha256.len,
                               err);
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_set(&e.path_text, mv.path_text.data, mv.path_text.len, err);
        }
        if (st == ATLAS_OK && mv.commit_oid.len > 0) {
            st = atlas_buf_set(&e.commit_oid, mv.commit_oid.data, mv.commit_oid.len, err);
        }
        atlas_memory_version_row_free(&mv);
    }
    if (st == ATLAS_OK) {
        struct {
            atlas_buf *to;
            const atlas_buf *from;
        } copies[] = {
            {&e.symbol, &op->symbol},     {&e.target, &op->target},
            {&e.probe, &op->probe},       {&e.observed, &op->observed},
            {&e.observed_at, &op->observed_at},
        };
        for (size_t i = 0; st == ATLAS_OK && i < sizeof copies / sizeof copies[0]; i++) {
            st = atlas_buf_set(copies[i].to, copies[i].from->data, copies[i].from->len, err);
        }
    }
    /* §30: which generation, when the evidence came from the semantic index. */
    if (st == ATLAS_OK && claim.repo_id != 0) {
        st = atlas_db_verify_sem_generation(db, claim.repo_id, &e.sem_generation, err);
    }

    /* §27. The key is the reference, and the reference includes the commit and,
     * for a dynamic observation, the instant. `observed_at` is in the key
     * precisely so that "the same runtime fact at a different time" stays two
     * facts, which §27 names as the case that must not merge. */
    if (st == ATLAS_OK) {
        atlas_sha256 h;
        atlas_sha256_init(&h);
        key_str(&h, KEY_DOMAIN_EVIDENCE);
        key_str(&h, atlas_verify_evidence_class_name(e.cls));
        key_i64(&h, e.repo_id);
        key_buf(&h, &e.commit_oid);
        key_buf(&h, &e.path_text);
        key_buf(&h, &e.symbol);
        key_i64(&h, e.line_start);
        key_i64(&h, e.line_end);
        key_buf(&h, &e.target);
        key_buf(&h, &e.probe);
        key_buf(&h, &e.observed);
        key_buf(&h, &e.observed_at);
        key_i64(&h, e.actor_id);
        /* A12.1. In the key only when it is set: the same snapshot reference
         * is one row and a different snapshot is another, which is the whole
         * job. Unconditional inclusion would append the empty field's length
         * prefix and silently re-key every evidence row A9.2.1 has already
         * stored — after which a retry of a pre-A12.1 submission would miss
         * its own row and land as a second one, §27's forgery arriving as an
         * upgrade. Fields are length-prefixed, so a stream carrying the field
         * and a stream without it cannot encode the same reference. */
        if (op->memory_version_uid.len > 0) {
            key_buf(&h, &op->memory_version_uid);
        }
        st = key_finish(&h, &e.content_key, err);
    }
    if (st == ATLAS_OK) {
        bool found = false;
        int64_t existing = 0;
        st = atlas_db_verify_evidence_by_key(db, e.content_key.data, &existing, &out->uid, &found,
                                             err);
        if (st == ATLAS_OK && found) {
            out->evidence_id = existing;
            out->duplicate = true;
            out->repo_id = claim.repo_id;
            atlas_verify_evidence_free(&e);
            atlas_verify_claim_free(&claim);
            atlas_repo_info_free(&repo);
            return ATLAS_OK;
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_db_verify_evidence_insert(db, &e, now, err);
    }
    if (st == ATLAS_OK) {
        out->evidence_id = e.id;
        out->claim_id = claim.id;
        out->repo_id = claim.repo_id;
        st = atlas_buf_set(&out->uid, e.uid.data, e.uid.len, err);
    }
    atlas_verify_evidence_free(&e);
    atlas_verify_claim_free(&claim);
    atlas_repo_info_free(&repo);
    return st;
}

/* §9. The legitimate way to obtain authenticated tool evidence: Atlas runs a
 * named allowlisted verifier and records what it concluded.
 *
 * The caller chooses *which* verifier applies. It does not choose the verdict,
 * cannot supply one, and has no field in which to put one — which is why this is
 * a separate operation rather than a flag on EVIDENCE_ADD. Everything the row
 * asserts about a compiler having proved something is asserted by Atlas because
 * Atlas asked the compiler's index. */
static atlas_status op_evidence_produce(atlas_db *db, const atlas_verify_op *op, const char *now,
                                        atlas_verify_intake_result *out, atlas_err *err) {
    atlas_verify_claim claim;
    atlas_verify_claim_init(&claim);
    atlas_status st = resolve_claim(db, &op->claim_uid, &claim, err);

    atlas_verify_verifier v = ATLAS_VERIFIER_NONE;
    if (st == ATLAS_OK) {
        /* The claim's own verifier, or one the caller named. The claim's wins:
         * what a claim is mechanically checkable *by* is a property of the
         * claim, decided when it was written. */
        const char *name = claim.verifier.len > 0 ? claim.verifier.data
                                                  : (op->verifier.len > 0 ? op->verifier.data
                                                                          : NULL);
        if (name == NULL || !atlas_verify_verifier_parse(name, &v)) {
            st = atlas_err_set(err, ATLAS_ERR_USAGE,
                               "this claim names no deterministic verifier, and none was given; "
                               "Atlas will not invent a mechanical test for a proposition");
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_verify_run_verifier(db, v, claim.repo_id,
                                       claim.verifier_input.len > 0 ? claim.verifier_input.data
                                                                    : "",
                                       &out->check, &out->coverage, out->verified_scope,
                                       sizeof out->verified_scope, out->detail, sizeof out->detail,
                                       err);
        if (st == ATLAS_OK) {
            /* A9.2.2. The same single producer the assessment path uses. A
             * second implementation here — even an obviously equivalent one —
             * would be a second place ABSENT could come from, and the whole
             * guarantee is that there is one.
             *
             * The basis is DETERMINISTIC because this operation *is* Atlas
             * running a mechanical verifier; that is what distinguishes
             * `verify produce` from `verify evidence`, which merely references
             * something. */
            out->truth = atlas_verify_truth_of(v, ATLAS_VERIFY_BASIS_DETERMINISTIC,
                                               claim.semantics, out->check, &out->coverage,
                                               &out->truth_reason);
        }
    }

    atlas_repo_info repo;
    atlas_repo_info_init(&repo);
    if (st == ATLAS_OK && claim.repo_id != 0) {
        bool found = false;
        st = atlas_db_repo_get_by_id(db, claim.repo_id, &repo, &found, err);
        if (st == ATLAS_OK && !found) {
            st = atlas_err_set(err, ATLAS_ERR_CONFIG,
                               "the repository this claim was about is no longer registered");
        }
    }

    /* The producing actor is Atlas, on the ATLAS channel, which is the only way
     * an ATLAS_VERIFIER actor can exist — enforced in C for the message and by a
     * schema CHECK for the guarantee. */
    int64_t atlas_actor = 0;
    atlas_buf atlas_actor_uid = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        atlas_verify_op self;
        atlas_verify_op_init(&self);
        self.channel = ATLAS_VERIFY_CHANNEL_ATLAS;
        st = atlas_buf_set(&self.actor_name, atlas_verify_verifier_name(v),
                           strlen(atlas_verify_verifier_name(v)), err);
        if (st == ATLAS_OK) {
            st = atlas_buf_set(&self.actor_provider, "atlas", 5, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_set(&self.actor_version, ATLAS_VERIFY_ALGORITHM,
                               strlen(ATLAS_VERIFY_ALGORITHM), err);
        }
        if (st == ATLAS_OK) {
            st = derive_actor(db, &self, now, &atlas_actor, &atlas_actor_uid, err);
        }
        atlas_verify_op_free(&self);
    }

    atlas_verify_evidence e;
    atlas_verify_evidence_init(&e);
    if (st == ATLAS_OK) {
        e.cls = ATLAS_EVIDENCE_COMPILER;
        e.repo_id = claim.repo_id;
        e.actor_id = atlas_actor;
        st = atlas_buf_set(&e.commit_oid, claim.basis_commit.data, claim.basis_commit.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&e.tool, atlas_verify_verifier_name(v),
                           strlen(atlas_verify_verifier_name(v)), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&e.tool_version, ATLAS_VERIFY_ALGORITHM,
                           strlen(ATLAS_VERIFY_ALGORITHM), err);
    }
    if (st == ATLAS_OK) {
        /* What the verifier concluded, in Atlas' own closed vocabulary. Never a
         * repository byte and never a caller's word. */
        const char *r = atlas_verify_check_name(out->check);
        st = atlas_buf_set(&e.result, r, strlen(r), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&e.observed, out->verified_scope, strlen(out->verified_scope), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&e.proof_class, out->check == ATLAS_CHECK_PASS ? "PROVEN" : "UNKNOWN",
                           out->check == ATLAS_CHECK_PASS ? 6 : 7, err);
    }
    if (st == ATLAS_OK && claim.repo_id != 0) {
        st = atlas_db_verify_sem_generation(db, claim.repo_id, &e.sem_generation, err);
    }
    /* §30: the generation is in the key. Re-running the same verifier over a
     * *rebuilt* index is a fresh observation and must land as one; re-running it
     * over the same generation is the same observation and must not. */
    if (st == ATLAS_OK) {
        atlas_sha256 h;
        atlas_sha256_init(&h);
        key_str(&h, KEY_DOMAIN_EVIDENCE);
        key_str(&h, "produced");
        key_i64(&h, e.repo_id);
        key_buf(&h, &e.commit_oid);
        key_buf(&h, &e.tool);
        key_buf(&h, &claim.verifier_input);
        key_buf(&h, &e.result);
        key_i64(&h, e.sem_generation);
        st = key_finish(&h, &e.content_key, err);
    }
    if (st == ATLAS_OK) {
        bool found = false;
        int64_t existing = 0;
        st = atlas_db_verify_evidence_by_key(db, e.content_key.data, &existing, &out->uid, &found,
                                             err);
        if (st == ATLAS_OK && found) {
            out->evidence_id = existing;
            out->duplicate = true;
            out->claim_id = claim.id;
            out->repo_id = claim.repo_id;
            out->actor_id = atlas_actor;
            goto done;
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_db_verify_evidence_insert(db, &e, now, err);
    }
    if (st == ATLAS_OK) {
        out->evidence_id = e.id;
        out->claim_id = claim.id;
        out->repo_id = claim.repo_id;
        out->actor_id = atlas_actor;
        st = atlas_buf_set(&out->uid, e.uid.data, e.uid.len, err);
    }
done:
    if (st == ATLAS_OK && atlas_actor_uid.len > 0) {
        st = atlas_buf_set(&out->actor_uid, atlas_actor_uid.data, atlas_actor_uid.len, err);
    }
    atlas_buf_free(&atlas_actor_uid);
    atlas_verify_evidence_free(&e);
    atlas_verify_claim_free(&claim);
    atlas_repo_info_free(&repo);
    return st;
}

/* --- attestation ------------------------------------------------------------ */

/* Splits a comma-separated list of evidence uids and resolves every one.
 *
 * §12. Declaring what an attestation rests on is what lets the union-find see
 * that three actors read one document. An undeclared interpretation is folded
 * into one shared group rather than becoming a root, so omitting the list is
 * conservative rather than advantageous — which is the property that makes this
 * safe to accept from a model at all. */
static atlas_status resolve_evidence_list(atlas_db *db, const atlas_buf *list, int64_t *ids,
                                          size_t max, size_t *count_out, atlas_err *err) {
    *count_out = 0;
    if (list->len == 0) {
        return ATLAS_OK;
    }
    const char *p = list->data;
    const char *end = list->data + list->len;
    while (p < end) {
        while (p < end && (*p == ',' || *p == ' ')) {
            p++;
        }
        const char *start = p;
        while (p < end && *p != ',' && *p != ' ') {
            p++;
        }
        if (p == start) {
            continue;
        }
        if (*count_out >= max) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "an attestation may rest on at most %u pieces of evidence",
                                 (unsigned)max);
        }
        char uid[ATLAS_VERIFY_UID_MAX + 1];
        size_t n = (size_t)(p - start);
        if (n > ATLAS_VERIFY_UID_MAX) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "that is not an evidence id");
        }
        memcpy(uid, start, n);
        uid[n] = '\0';
        bool found = false;
        int64_t id = 0;
        atlas_status st = atlas_db_verify_evidence_find(db, uid, &id, NULL, &found, err);
        if (st != ATLAS_OK) {
            return st;
        }
        if (!found) {
            /* Refused, not skipped. An attestation that silently dropped an
             * unresolvable reference would be recorded as resting on less than
             * it claimed — and an attestation resting on nothing is grouped as
             * an undeclared interpretation, which changes what it is worth. */
            return atlas_err_set(err, ATLAS_ERR_CONFIG,
                                 "no evidence by that id exists, so an attestation cannot rest on "
                                 "it");
        }
        ids[(*count_out)++] = id;
    }
    return ATLAS_OK;
}

static atlas_status op_attestation_add(atlas_db *db, const atlas_verify_op *op, const char *now,
                                       atlas_verify_intake_result *out, atlas_err *err) {
    atlas_verify_claim claim;
    atlas_verify_claim_init(&claim);
    atlas_status st = resolve_claim(db, &op->claim_uid, &claim, err);

    if (st == ATLAS_OK) {
        st = bounded(&op->method, ATLAS_VERIFY_METHOD_MAX, "an attestation's method", err);
    }
    if (st == ATLAS_OK) {
        st = bounded(&op->scope_note, ATLAS_VERIFY_SCOPE_MAX, "an attestation's scope note", err);
    }
    if (st == ATLAS_OK && (op->self_confidence < -1 || op->self_confidence > 100)) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE,
                           "self-reported confidence is 0..100, or absent; it is data about the "
                           "source and is never used as Atlas' confidence");
    }

    int64_t ids[ATLAS_VERIFY_MAX_EVIDENCE_PER_ATTESTATION];
    size_t id_count = 0;
    if (st == ATLAS_OK) {
        st = resolve_evidence_list(db, &op->evidence_uids, ids, sizeof ids / sizeof ids[0],
                                   &id_count, err);
    }

    atlas_verify_attestation a;
    atlas_verify_attestation_init(&a);
    if (st == ATLAS_OK) {
        a.claim_id = claim.id;
        a.actor_id = out->actor_id;
        a.verdict = op->verdict;
        a.self_confidence = op->self_confidence;
        struct {
            atlas_buf *to;
            const atlas_buf *from;
        } copies[] = {
            {&a.method, &op->method},
            {&a.scope_note, &op->scope_note},
        };
        for (size_t i = 0; st == ATLAS_OK && i < sizeof copies / sizeof copies[0]; i++) {
            st = atlas_buf_set(copies[i].to, copies[i].from->data, copies[i].from->len, err);
        }
    }
    /* What the actor says it examined. Defaults to the claim's binding, so an
     * attestation is never unbound; supplied explicitly it is validated, which
     * is what makes "this actor examined an older tree" visible. */
    atlas_repo_info repo;
    atlas_repo_info_init(&repo);
    if (st == ATLAS_OK && claim.repo_id != 0) {
        bool found = false;
        st = atlas_db_repo_get_by_id(db, claim.repo_id, &repo, &found, err);
        if (st == ATLAS_OK && found) {
            const atlas_buf *asked =
                op->basis_commit.len > 0 ? &op->basis_commit : &claim.basis_commit;
            st = bind_commit(db, &repo, asked, &a.basis_commit, err);
        }
    }
    if (st == ATLAS_OK && op->supersedes_uid.len > 0) {
        bool found = false;
        int64_t prior = 0;
        st = atlas_db_verify_attestation_find(db, op->supersedes_uid.data, &prior, &found, err);
        if (st == ATLAS_OK && !found) {
            st = atlas_err_set(err, ATLAS_ERR_CONFIG,
                               "no attestation by that id exists, so nothing can supersede it");
        }
        if (st == ATLAS_OK) {
            a.supersedes_id = prior;
        }
    }

    /* §27/§28. The key covers the claim, the actor, the verdict, the method, the
     * scope and the examined commit. A repeat is one attestation; a genuine
     * change of mind is a *different verdict*, so a different key, so it lands
     * as the new row `supersedes_id` exists to carry. Replay protection that
     * suppressed a reversal would hide exactly the fact a reliability system
     * most needs to see. */
    if (st == ATLAS_OK) {
        atlas_sha256 h;
        atlas_sha256_init(&h);
        key_str(&h, KEY_DOMAIN_ATTESTATION);
        key_i64(&h, a.claim_id);
        key_i64(&h, a.actor_id);
        key_str(&h, atlas_verify_verdict_name(a.verdict));
        key_buf(&h, &a.method);
        key_buf(&h, &a.scope_note);
        key_buf(&h, &a.basis_commit);
        for (size_t i = 0; i < id_count; i++) {
            key_i64(&h, ids[i]);
        }
        st = key_finish(&h, &a.content_key, err);
    }
    if (st == ATLAS_OK) {
        bool found = false;
        int64_t existing = 0;
        st = atlas_db_verify_attestation_by_key(db, a.content_key.data, &existing, &out->uid,
                                                &found, err);
        if (st == ATLAS_OK && found) {
            out->attestation_id = existing;
            out->duplicate = true;
            out->claim_id = claim.id;
            out->repo_id = claim.repo_id;
            atlas_verify_attestation_free(&a);
            atlas_verify_claim_free(&claim);
            atlas_repo_info_free(&repo);
            return ATLAS_OK;
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_db_verify_attestation_insert(db, &a, ids, id_count, now, err);
    }
    if (st == ATLAS_OK) {
        out->attestation_id = a.id;
        out->claim_id = claim.id;
        out->repo_id = claim.repo_id;
        st = atlas_buf_set(&out->uid, a.uid.data, a.uid.len, err);
    }
    atlas_verify_attestation_free(&a);
    atlas_verify_claim_free(&claim);
    atlas_repo_info_free(&repo);
    return st;
}

/* --- dependency ------------------------------------------------------------- */

static atlas_status op_dependency_add(atlas_db *db, const atlas_verify_op *op, const char *now,
                                      atlas_verify_intake_result *out, atlas_err *err) {
    bool found = false;
    int64_t derived = 0;
    int64_t source = 0;
    atlas_status st =
        atlas_db_verify_evidence_find(db, op->derived_uid.data, &derived, NULL, &found, err);
    if (st == ATLAS_OK && !found) {
        st = atlas_err_set(err, ATLAS_ERR_CONFIG, "no evidence by that id exists to derive");
    }
    if (st == ATLAS_OK) {
        st = atlas_db_verify_evidence_find(db, op->source_uid.data, &source, NULL, &found, err);
    }
    if (st == ATLAS_OK && !found) {
        st = atlas_err_set(err, ATLAS_ERR_CONFIG, "no evidence by that id exists to derive from");
    }
    if (st == ATLAS_OK) {
        /* The self-edge check lives in the DB layer, which is the write point
         * for the edge; asking twice would be a second copy of the rule. */
        st = atlas_db_verify_evidence_dep_add(db, derived, source, now, err);
    }
    if (st == ATLAS_OK) {
        out->evidence_id = derived;
        st = atlas_buf_set(&out->uid, op->derived_uid.data, op->derived_uid.len, err);
    }
    return st;
}

/* --- the write point -------------------------------------------------------- */

atlas_status atlas_verify_intake_apply_in_tx(atlas_db *db, const atlas_verify_op *op,
                                             atlas_verify_intake_result *out, atlas_err *err) {
    if (db == NULL || op == NULL || out == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no verification operation to apply");
    }
    /* UNKNOWN is zero and is refused. A `memset` must not produce a channel that
     * can write, and an intake path that forgot to set the channel must fail
     * rather than default to the weakest caller that can still speak. */
    if (op->channel == ATLAS_VERIFY_CHANNEL_UNKNOWN) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "nothing established who is submitting this, and Atlas does not "
                             "record evidence from an unidentified channel");
    }

    char now[64];
    atlas_now_iso8601(now, sizeof now);

    /* Every operation but a dependency edge is attributable, so the actor is
     * derived first and the claim records it. A dependency is a statement about
     * two rows that already carry their own provenance. */
    atlas_status st = ATLAS_OK;
    if (op->kind != ATLAS_VERIFY_OP_DEPENDENCY_ADD &&
        op->kind != ATLAS_VERIFY_OP_EVIDENCE_PRODUCE && op->kind != ATLAS_VERIFY_OP_EVALUATE) {
        st = derive_actor(db, op, now, &out->actor_id, &out->actor_uid, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }

    switch (op->kind) {
    case ATLAS_VERIFY_OP_CLAIM_CREATE:
        return op_claim_create(db, op, now, out, err);
    case ATLAS_VERIFY_OP_EVIDENCE_ADD:
        return op_evidence_add(db, op, now, out, err);
    case ATLAS_VERIFY_OP_EVIDENCE_PRODUCE:
        return op_evidence_produce(db, op, now, out, err);
    case ATLAS_VERIFY_OP_ATTESTATION_ADD:
        return op_attestation_add(db, op, now, out, err);
    case ATLAS_VERIFY_OP_DEPENDENCY_ADD:
        return op_dependency_add(db, op, now, out, err);
    case ATLAS_VERIFY_OP_EVALUATE:
        break;
    }

    /* §16. Evaluation does not reimplement one line of lifecycle logic: it hands
     * the claim to the engine A9.2 already has, which aggregates, applies the
     * root-owned policy's gates and — if and only if they are met — spends a
     * warrant through `atlas_verify_autolifecycle_run`. The caller's involvement
     * ends at having asked. */
    atlas_verify_claim claim;
    atlas_verify_claim_init(&claim);
    st = resolve_claim(db, &op->claim_uid, &claim, err);
    if (st == ATLAS_OK) {
        out->claim_id = claim.id;
        out->repo_id = claim.repo_id;
        st = atlas_buf_set(&out->uid, claim.uid.data, claim.uid.len, err);
    }
    atlas_verifypolicy p;
    if (st == ATLAS_OK) {
        atlas_verifypolicy_load(&p);
        st = atlas_verify_autolifecycle_run(db, &p, claim.id,
                                            op->repo_name.len > 0 ? op->repo_name.data : NULL,
                                            &out->assessment, err);
    }
    atlas_verify_claim_free(&claim);
    return st;
}

atlas_status atlas_verify_intake_apply(atlas_db *db, const atlas_verify_op *op,
                                       atlas_verify_intake_result *out, atlas_err *err) {
    atlas_status st = atlas_db_begin(db, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_verify_intake_apply_in_tx(db, op, out, err);
    if (st == ATLAS_OK) {
        st = atlas_db_commit(db, err);
    } else {
        atlas_db_rollback(db);
    }
    return st;
}
