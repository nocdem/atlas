/* Atlas - the A4 decision-scale acceptance fixture generator.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * In C for the same reason the JSON checker and the C-tree generator are: a
 * measurement fixture is part of the build, and the build depends on no
 * language runtime. Deterministic from a fixed seed, because a performance
 * number is only comparable when the input was identical.
 *
 * It writes through `atlas_decision_apply` — the public entry point over the
 * same single write point, `atlas_decision_apply_in_tx`, that the daemon's
 * writer thread uses — rather than through hand-written SQL. A fixture
 * built by a second write path would measure a database shape the real one
 * never produces, and would not exercise the content hashing, the canonical
 * encoding or the ledger at all.
 *
 * Usage: atlas-gen-decisions DATA_DIR REPO_ROOT DOCUMENTS REVISIONS LINKS
 *
 * The lifecycle mix is deliberate and fixed:
 *   - every fourth document is approved (and its earlier revision superseded),
 *   - every seventh is rejected,
 *   - every eleventh approved one is superseded by the document after it,
 *   - the rest stay proposed.
 * So the measured queries run over a table with all four states in it, which is
 * the only version of the measurement worth having: a set of uniformly proposed
 * documents would never touch the partial index that makes the approved lookup
 * a seek.
 */
#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/decision_ops.h"

/* A tiny deterministic PRNG. Not `rand()`: its sequence is implementation
 * defined, so two machines would build two different fixtures and their numbers
 * would not be comparable. */
static uint64_t rng_state = 0x2545F4914F6CDD1Dull;

static uint64_t next_rand(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* Fills a path link's snapshot from the file index: the content hash the index
 * currently records, and nothing when the path is not indexed.
 *
 * This is what the real propose path does, and doing it here is what makes the
 * corpus resolvable. A link with no captured hash is UNKNOWN for ever, which is
 * correct and makes every downstream assessment vacuous. */
static void anchor_link(atlas_db *db, int64_t repo_id, atlas_decision_link *link);

static void die(const char *what, const atlas_err *err) {
    (void)fprintf(stderr, "atlas-gen-decisions: %s: %s\n", what, atlas_err_msg(err));
    exit(1);
}

/* Prose long enough to be realistic and bounded well inside the limits, built
 * from a fixed vocabulary so the search fixture has terms that actually match
 * and terms that deliberately do not. */
static const char *const TOPICS[] = {
    "storage",   "protocol",  "concurrency", "indexing",   "provenance", "caching",
    "migration", "hashing",   "watching",    "resolution", "transport",  "approval",
};
#define TOPIC_COUNT (sizeof(TOPICS) / sizeof(TOPICS[0]))

static atlas_status take_hash(const atlas_file_row *row, void *ud, atlas_err *err) {
    atlas_buf *out = ud;
    if (row->content_hash != NULL && !row->deleted) {
        return atlas_buf_appendf(out, err, "%s", row->content_hash);
    }
    return ATLAS_OK;
}

static void anchor_link(atlas_db *db, int64_t repo_id, atlas_decision_link *link) {
    atlas_err err;
    atlas_err_init(&err);
    bool found = false;
    /* Through the public read, like everything else here: a fixture that
     * reached the tables would be building a shape the real write path cannot
     * produce, which is the one thing a fixture must not do. */
    (void)atlas_db_file_get(db, repo_id, link->path_raw.data, link->path_raw.len, take_hash,
                            &link->file_content_hash, &found, &err);
}

int main(int argc, char **argv) {
    if (argc != 6) {
        (void)fprintf(stderr,
                      "usage: atlas-gen-decisions DATA_DIR REPO_ROOT DOCUMENTS REVISIONS LINKS\n");
        return 2;
    }
    const char *data_dir = argv[1];
    const char *repo_root = argv[2];
    long documents = strtol(argv[3], NULL, 10);
    long revisions = strtol(argv[4], NULL, 10);
    long links = strtol(argv[5], NULL, 10);
    if (documents <= 0 || revisions < documents || links < 0) {
        (void)fprintf(stderr, "atlas-gen-decisions: REVISIONS must be at least DOCUMENTS\n");
        return 2;
    }

    atlas_err err;
    atlas_err_init(&err);

    atlas_buf db_path = ATLAS_BUF_INIT;
    if (atlas_buf_appendf(&db_path, &err, "%s/atlas.db", data_dir) != ATLAS_OK) {
        die("path", &err);
    }
    atlas_db *db = NULL;
    if (atlas_db_open(atlas_buf_cstr(&db_path), &db, &err) != ATLAS_OK) {
        die("open", &err);
    }
    if (atlas_db_migrate(db, &err) != ATLAS_OK) {
        die("migrate", &err);
    }

    /* The repository, registered the way a real registration leaves it. */
    int64_t repo_id = 0;
    atlas_repo_info existing;
    atlas_repo_info_init(&existing);
    bool found = false;
    if (atlas_db_repo_get(db, "perf", &existing, &found, &err) != ATLAS_OK) {
        die("repo lookup", &err);
    }
    if (found) {
        repo_id = existing.id;
    } else {
        atlas_repo_identity id;
        memset(&id, 0, sizeof(id));
        id.root = repo_root;
        id.root_len = strlen(repo_root);
        id.common_dir = repo_root;
        id.common_dir_len = strlen(repo_root);
        id.git_dir = repo_root;
        id.git_dir_len = strlen(repo_root);
        id.object_format = "sha1";
        if (atlas_db_repo_add(db, "perf", &id, &repo_id, &err) != ATLAS_OK) {
            die("repo add", &err);
        }
    }
    /* The indexed head, if this repository has been scanned. A decision
     * proposed before any scan legitimately has none, and an empty basis is a
     * real recorded value — but a fixture whose every revision has one measures
     * only that path. */
    char head[ATLAS_OID_HEX_MAX_INCL];
    (void)snprintf(head, sizeof head, "%s", found ? existing.scanned_head : "");
    atlas_repo_info_free(&existing);

    /* How many extra revisions each document gets, spread so the total lands on
     * the requested figure rather than being uniform — a uniform depth would
     * make the "newest revision" seek measure one shape. */
    long extra_total = revisions - documents;
    /* Per *revision*, not per document: every revision carries anchors, so
     * dividing by the document count would overshoot the requested link total
     * by the revision-to-document ratio — which is how a 100 000-link request
     * silently became a 250 000-link fixture and pushed A5's restore past its
     * budget. */
    long links_per_doc = links / (revisions > documents ? revisions : (documents > 0 ? documents : 1));
    if (links_per_doc < 1) {
        links_per_doc = 1;
    }
    if (links_per_doc > ATLAS_DECISION_MAX_LINKS) {
        links_per_doc = ATLAS_DECISION_MAX_LINKS;
    }

    long made_revisions = 0;
    long made_links = 0;
    long approved = 0, rejected = 0, superseded = 0;
    atlas_buf prev_uid = ATLAS_BUF_INIT;

    for (long d = 0; d < documents; d++) {
        const char *topic = TOPICS[(size_t)(next_rand() % TOPIC_COUNT)];
        atlas_decision_op op;
        atlas_decision_op_init(&op, ATLAS_DECISION_OP_PROPOSE);
        if (atlas_buf_set_str(&op.repo_name, "perf", &err) != ATLAS_OK) {
            die("repo name", &err);
        }
        if (atlas_buf_appendf(&op.revision.title, &err, "Decision %ld about %s", d, topic) !=
            ATLAS_OK) {
            die("title", &err);
        }
        if (atlas_buf_appendf(&op.revision.decision_text, &err,
                              "The %s subsystem uses approach %ld. This paragraph exists so the "
                              "searchable projection has realistic length and the query has "
                              "something to miss as well as something to match.",
                              topic, d % 7) != ATLAS_OK) {
            die("decision", &err);
        }
        if (atlas_buf_appendf(&op.revision.rationale_text, &err,
                              "Chosen over the alternatives because it bounds %s cost.",
                              topic) != ATLAS_OK) {
            die("rationale", &err);
        }
        op.revision.scope = ATLAS_DECISION_SCOPE_PATHS;
        if (head[0] != '\0' &&
            atlas_buf_appendf(&op.revision.basis_head, &err, "%s", head) != ATLAS_OK) {
            die("basis head", &err);
        }
        for (long l = 0; l < links_per_doc; l++) {
            atlas_decision_link link;
            atlas_decision_link_init(&link, ATLAS_DECISION_LINK_PATH);
            /* Paths drawn from a space smaller than the link count, so many
             * decisions share a file — which is what makes the
             * decisions-for-a-file query measure a real lookup rather than a
             * single row. */
            /* The layout `atlas-gen-ctree` actually produces:
             * `src/mod<M>/part_<I>.c`. It used to be `src/mod<M>/file<N>.c`,
             * which matched nothing — so every link in every fixture built by
             * this tool was dangling. That was invisible to A4 and A5, which
             * measure how fast decision rows can be read, and is not invisible
             * to A6, which asks what the links resolve to. */
            long file = (long)(next_rand() % 4000u);
            if (atlas_buf_appendf(&link.path_raw, &err, "src/mod%ld/part_%ld.c", file / 32,
                                  file % 32) != ATLAS_OK ||
                atlas_buf_appendf(&link.path_text, &err, "src/mod%ld/part_%ld.c", file / 32,
                                  file % 32) != ATLAS_OK) {
                die("link path", &err);
            }
            /* The snapshot the real propose path captures: the content hash
             * the file index currently records, and the commit it was taken
             * against.
             *
             * Without it every link resolves to UNKNOWN — "the file is there
             * and Atlas cannot say whether it is the same file" — which is
             * correct behaviour and useless as a fixture: every A6 assessment
             * over such a corpus is UNKNOWN before it has done any work, so a
             * gate measurement over it would be timing the early exit. */
            anchor_link(db, repo_id, &link);
            if (atlas_decision_revision_add_link(&op.revision, &link, &err) != ATLAS_OK) {
                die("link", &err);
            }
            atlas_decision_link_free(&link);
            made_links++;
        }

        atlas_decision_result res;
        atlas_decision_result_init(&res);
        if (atlas_decision_apply(db, &op, &res, &err) != ATLAS_OK) {
            die("propose", &err);
        }
        made_revisions++;
        atlas_buf uid = ATLAS_BUF_INIT;
        if (atlas_buf_set(&uid, res.uid.data, res.uid.len, &err) != ATLAS_OK) {
            die("uid", &err);
        }
        atlas_decision_result_free(&res);
        atlas_decision_op_free(&op);

        /* Extra revisions, so the history and the "newest revision" seek have
         * depth to work through. */
        /* Alternating 2 and 1 averages 1.5 extra revisions per document, which
         * is what turns DOCUMENTS into 2.5 x DOCUMENTS revisions. Not uniform,
         * so the "newest revision" seek walks depths of 1, 2 and 3. */
        long extra = extra_total > 0 ? (d % 2 == 0 ? 2 : 1) : 0;
        for (long r = 0; r < extra && made_revisions < revisions; r++) {
            atlas_decision_op rev;
            atlas_decision_op_init(&rev, ATLAS_DECISION_OP_REVISE);
            if (atlas_buf_set_str(&rev.repo_name, "perf", &err) != ATLAS_OK ||
                atlas_buf_set(&rev.uid, uid.data, uid.len, &err) != ATLAS_OK) {
                die("revise target", &err);
            }
            if (atlas_buf_appendf(&rev.revision.title, &err, "Decision %ld about %s", d, topic) !=
                    ATLAS_OK ||
                atlas_buf_appendf(&rev.revision.decision_text, &err,
                                  "Revision %ld: the %s subsystem now uses approach %ld instead.",
                                  r + 2, topic, (d + r + 1) % 7) != ATLAS_OK) {
                die("revision text", &err);
            }
            /* A later revision carries anchors too, and it is the one an
             * approval lands on. Without them the approved revision of every
             * multi-revision document has nothing to be about — which made the
             * A6 assessment SCOPE_NOT_ASSESSABLE for most of the corpus. */
            rev.revision.scope = ATLAS_DECISION_SCOPE_PATHS;
            if (head[0] != '\0' &&
                atlas_buf_appendf(&rev.revision.basis_head, &err, "%s", head) != ATLAS_OK) {
                die("basis head", &err);
            }
            for (long l = 0; l < links_per_doc; l++) {
                atlas_decision_link rlink;
                atlas_decision_link_init(&rlink, ATLAS_DECISION_LINK_PATH);
                long rfile = (long)(next_rand() % 4000u);
                if (atlas_buf_appendf(&rlink.path_raw, &err, "src/mod%ld/part_%ld.c", rfile / 32,
                                      rfile % 32) != ATLAS_OK ||
                    atlas_buf_appendf(&rlink.path_text, &err, "src/mod%ld/part_%ld.c", rfile / 32,
                                      rfile % 32) != ATLAS_OK) {
                    die("link path", &err);
                }
                anchor_link(db, repo_id, &rlink);
                if (atlas_decision_revision_add_link(&rev.revision, &rlink, &err) != ATLAS_OK) {
                    die("link", &err);
                }
                atlas_decision_link_free(&rlink);
                made_links++;
            }
            atlas_decision_result rres;
            atlas_decision_result_init(&rres);
            if (atlas_decision_apply(db, &rev, &rres, &err) != ATLAS_OK) {
                die("revise", &err);
            }
            atlas_decision_result_free(&rres);
            atlas_decision_op_free(&rev);
            made_revisions++;
        }

        /* The lifecycle mix. Each transition goes through the real operator
         * path — a challenge, then the spend — so the fixture contains genuine
         * ledger events and genuine consumed capabilities rather than
         * hand-written status columns. */
        if (d % 4 == 0 || d % 7 == 0) {
            atlas_decision_intent intent =
                (d % 7 == 0 && d % 4 != 0) ? ATLAS_DECISION_INTENT_REJECT
                                           : ATLAS_DECISION_INTENT_APPROVE;
            atlas_decision_op ch;
            atlas_decision_op_init(&ch, ATLAS_DECISION_OP_CHALLENGE);
            if (atlas_buf_set_str(&ch.repo_name, "perf", &err) != ATLAS_OK ||
                atlas_buf_set(&ch.uid, uid.data, uid.len, &err) != ATLAS_OK) {
                die("challenge target", &err);
            }
            ch.intent = intent;
            atlas_decision_result cres;
            atlas_decision_result_init(&cres);
            if (atlas_decision_apply(db, &ch, &cres, &err) != ATLAS_OK) {
                die("challenge", &err);
            }
            atlas_decision_op_free(&ch);

            atlas_decision_op sp;
            atlas_decision_op_init(&sp, intent == ATLAS_DECISION_INTENT_REJECT
                                             ? ATLAS_DECISION_OP_REJECT
                                             : ATLAS_DECISION_OP_APPROVE);
            if (atlas_buf_set_str(&sp.repo_name, "perf", &err) != ATLAS_OK ||
                atlas_buf_set(&sp.uid, uid.data, uid.len, &err) != ATLAS_OK ||
                atlas_buf_set(&sp.token, cres.token.data, cres.token.len, &err) != ATLAS_OK ||
                atlas_buf_set_str(&sp.confirmation, cres.confirm, &err) != ATLAS_OK) {
                die("spend", &err);
            }
            atlas_decision_result sres;
            atlas_decision_result_init(&sres);
            if (atlas_decision_apply(db, &sp, &sres, &err) != ATLAS_OK) {
                die("transition", &err);
            }
            if (intent == ATLAS_DECISION_INTENT_REJECT) {
                rejected++;
            } else {
                approved++;
            }
            atlas_decision_result_free(&sres);
            atlas_decision_result_free(&cres);
            atlas_decision_op_free(&sp);

            /* Every eleventh approved document is superseded by the previous
             * one, so the supersession chain and the document-level SUPERSEDED
             * status are both represented. */
            if (intent == ATLAS_DECISION_INTENT_APPROVE && prev_uid.len > 0 && approved % 11 == 0) {
                atlas_decision_op sch;
                atlas_decision_op_init(&sch, ATLAS_DECISION_OP_CHALLENGE);
                if (atlas_buf_set_str(&sch.repo_name, "perf", &err) != ATLAS_OK ||
                    atlas_buf_set(&sch.uid, prev_uid.data, prev_uid.len, &err) != ATLAS_OK ||
                    atlas_buf_set(&sch.replacement_uid, uid.data, uid.len, &err) != ATLAS_OK) {
                    die("supersede target", &err);
                }
                sch.intent = ATLAS_DECISION_INTENT_SUPERSEDE;
                atlas_decision_result scres;
                atlas_decision_result_init(&scres);
                /* A supersede whose target is not approved, or which would
                 * close a cycle, is legitimately refused; the fixture skips it
                 * rather than treating the refusal as a failure. */
                if (atlas_decision_apply(db, &sch, &scres, &err) == ATLAS_OK) {
                    atlas_decision_op ssp;
                    atlas_decision_op_init(&ssp, ATLAS_DECISION_OP_SUPERSEDE);
                    if (atlas_buf_set_str(&ssp.repo_name, "perf", &err) == ATLAS_OK &&
                        atlas_buf_set(&ssp.uid, prev_uid.data, prev_uid.len, &err) == ATLAS_OK &&
                        atlas_buf_set(&ssp.token, scres.token.data, scres.token.len, &err) ==
                            ATLAS_OK &&
                        atlas_buf_set_str(&ssp.confirmation, scres.confirm, &err) == ATLAS_OK) {
                        atlas_decision_result ssres;
                        atlas_decision_result_init(&ssres);
                        if (atlas_decision_apply(db, &ssp, &ssres, &err) == ATLAS_OK) {
                            superseded++;
                        } else {
                            atlas_err_init(&err);
                        }
                        atlas_decision_result_free(&ssres);
                    } else {
                        atlas_decision_op_free(&ssp);
                    }
                } else {
                    atlas_err_init(&err);
                    atlas_decision_op_free(&sch);
                }
                atlas_decision_result_free(&scres);
            }
            if (intent == ATLAS_DECISION_INTENT_APPROVE) {
                if (atlas_buf_set(&prev_uid, uid.data, uid.len, &err) != ATLAS_OK) {
                    die("prev uid", &err);
                }
            }
        }
        atlas_buf_free(&uid);

        if ((d % 500) == 0) {
            (void)fprintf(stderr, "  %ld/%ld documents\n", d, documents);
        }
    }

    (void)printf("documents %ld\nrevisions %ld\nlinks %ld\napproved %ld\nrejected %ld\n"
                 "superseded %ld\n",
                 documents, made_revisions, made_links, approved, rejected, superseded);

    atlas_buf_free(&prev_uid);
    atlas_db_close(db);
    atlas_buf_free(&db_path);
    return 0;
}
