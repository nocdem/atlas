/* Atlas - A15 T4: the review walker.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A review sheet (`include/atlas/review.h`, T3) is a plain-ASCII list an
 * operator copies out of a Mission Control browser session and saves as a
 * file on this machine. It stores no authority: its fifth field carries the
 * public prefix the operator will type, and has no field this file ever
 * reads in place of typing it -- every entry it names is still confirmed by
 * typing that hash prefix on `/dev/tty`, per entry, through
 * `atlas_service_decision_confirm` -- the whole of the operator channel --
 * and through nothing else. This file mints no capability and spends none
 * itself; it is a loop around the one function that does, plus the
 * pre-check that decides, for each entry, whether that function should be
 * called at all.
 *
 * The pre-check exists because a sheet is a snapshot: an operator read a
 * revision in a browser, and by the time the sheet reaches a terminal --
 * possibly minutes or days later, on a different machine -- the record may
 * have moved. Approving the wrong revision because nobody looked again would
 * be worse than the extra read, so `atlas_service_review_apply` reads the
 * live record again, immediately before it would mint anything, and refuses
 * an entry whose revision or status no longer matches what the sheet
 * describes. That refusal costs nothing: every step of the pre-check is a
 * read, so `--check` can run the whole thing with `check_only` true and
 * nothing is ever minted or spent.
 *
 * `--check`'s `READY` means only that this pre-check passed -- not that a
 * real run of the same entry would end `APPLIED`. The two ask different
 * questions: this file's DISPOSED check compares the intent against
 * `required_status_for`, which reads the *document's* current status
 * (`doc0.summary.status` below), while `op_challenge`
 * (`src/decision/lifecycle.c`), reached only once a capability is actually
 * being minted, checks a resolve intent against the *named revision's own*
 * stored state instead. An APPROVED document whose newest revision is a
 * still-PROPOSED candidate answers this file's question `APPROVED` and
 * `op_challenge`'s question `PROPOSED` for that same revision, so a resolve
 * entry naming it reads `READY` here and is refused there -- "only an
 * approved revision can be resolved; revision N is PROPOSED" -- before
 * anything is minted. See `docs/review-surface.md`'s costs section.
 *
 * One deviation from the contract as originally written, and the reason for
 * it: see the `atlas_service_review_apply` comment in `include/atlas/service.h`
 * for the chain, but in short, `atlas_authority_require` and
 * `atlas_terminal_open` are both skipped under `check_only`. A dry run mints
 * nothing and a locked profile's `atlas decision show` already answers every
 * question the pre-check asks, so gating the read on authority a `--check`
 * caller does not need would only make the dry run untestable in a profile
 * where the *running binary* -- never mind the operator -- lacks a root-owned
 * copy of itself, which is every development and test build on this
 * machine (`tests/test_a7_authority.c` asserts exactly that as the
 * precondition the rest of that suite runs in).
 */
#define _GNU_SOURCE 1

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/authority.h"
#include "atlas/decision.h"
#include "atlas/review.h"
#include "atlas/safetext.h"
#include "atlas/service.h"
#include "atlas/terminal.h"

void atlas_review_outcome_init(atlas_review_outcome *o) {
    memset(o, 0, sizeof(*o));
    atlas_buf_init(&o->status);
    atlas_buf_init(&o->detail);
}

void atlas_review_outcome_free(atlas_review_outcome *o) {
    if (o == NULL) {
        return;
    }
    atlas_buf_free(&o->status);
    atlas_buf_free(&o->detail);
}

/* --- small helpers ----------------------------------------------------------- */

/* `revision_no` of 0 means the effective revision; any other value is an
 * exact, already-existing one in every call site below (see the comment on
 * the MOVED branch in `walk_entry` for why this file never asks for a
 * revision number that might not exist). Dispatches to the remote form when
 * there is no local context, the same choice `atlas_service_decision_confirm`
 * itself makes for its own internal read. */
static atlas_status show_revision(atlas_ctx *ctx, const char *repo, const char *uid,
                                  int64_t revision_no, atlas_decision_document *out,
                                  atlas_err *err) {
    return ctx != NULL ? atlas_service_decision_show(ctx, repo, uid, revision_no, out, err)
                       : atlas_service_decision_show_remote(repo, uid, revision_no, out, err);
}

/* The status the intent needs before it may proceed: PROPOSED for approve and
 * reject, APPROVED for resolve. `atlas_review_intent_allowed` (src/core/review.c)
 * already refuses SUPERSEDE and REVALIDATE at parse time, so no sheet entry
 * ever reaches this switch carrying either -- the fallback after the switch is
 * unreachable in practice and exists only so the switch itself can list every
 * enumerator (`-Wswitch-enum`) rather than fall back on a `default:` that
 * would silently absorb a future intent added to the enum without a decision
 * here about what it needs. */
static atlas_decision_state required_status_for(atlas_decision_intent intent) {
    switch (intent) {
    case ATLAS_DECISION_INTENT_APPROVE: return ATLAS_DECISION_PROPOSED;
    case ATLAS_DECISION_INTENT_REJECT: return ATLAS_DECISION_PROPOSED;
    case ATLAS_DECISION_INTENT_RESOLVE: return ATLAS_DECISION_APPROVED;
    case ATLAS_DECISION_INTENT_SUPERSEDE: break;
    case ATLAS_DECISION_INTENT_REVALIDATE: break;
    }
    return ATLAS_DECISION_PROPOSED;
}

/* Copies at most ATLAS_DECISION_CONFIRM_HEX bytes of `hash` into `dst` (sized
 * ATLAS_DECISION_CONFIRM_HEX + 1), always NUL-terminated. A content hash read
 * back from a healthy record is always the full SHA-256 hex digest, so the
 * shorter branch is defensive rather than expected. */
static void set_prefix(char *dst, const char *hash) {
    size_t n = strlen(hash);
    if (n > ATLAS_DECISION_CONFIRM_HEX) {
        n = ATLAS_DECISION_CONFIRM_HEX;
    }
    memcpy(dst, hash, n);
    dst[n] = '\0';
}

/* The one place `o->status` is ever written -- always safe-encoded, with no
 * exception for the ordinary case, so there is exactly one rule to get right
 * rather than one per call site.
 *
 * A value from the closed status vocabulary (PROPOSED, APPROVED, REJECTED,
 * SUPERSEDED, RESOLVED) encodes to itself byte-for-byte -- atlas_safe() has
 * nothing to escape in it -- so this costs nothing on every legitimate
 * record and reads exactly as it did before encoding was added here. What it
 * closes is the one path a raw, unconditional atlas_buf_set() left open: a
 * status column holding bytes that are not a vocabulary member at all, which
 * only atlas_decision_state_parse() can detect and which this function no
 * longer needs a caller to have detected first. See the field's own comment
 * in include/atlas/service.h -- a caller of atlas_service_review_apply must
 * not encode this field again. */
static atlas_status set_status(atlas_review_outcome *o, const char *raw, size_t raw_len,
                               atlas_err *err) {
    atlas_buf_reset(&o->status);
    return atlas_text_encode_safe(raw, raw_len, &o->status, err);
}

/* Reads `path` with O_NOFOLLOW, never more than
 * ATLAS_REVIEW_SHEET_MAX_BYTES + 1 bytes -- one more than the parser's own
 * limit, never exactly that limit. A caller that stopped at exactly MAX_BYTES
 * would silently drop the tail of any larger sheet and hand the parser a
 * prefix that happens to parse, which would make the frozen refusal
 * "review sheet: larger than %u bytes" unreachable and would let a caller
 * violate the parser's own "never repairs, never truncates" guarantee on the
 * parser's behalf. One byte over the limit is instead handed straight to
 * `atlas_review_sheet_parse`, which refuses it with that exact sentence --
 * this function decides nothing about the size limit itself. */
static atlas_status read_sheet_bounded(const char *path, atlas_buf *out, atlas_err *err) {
    atlas_buf_reset(out);
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno, "cannot read %s", path);
    }
    const size_t cap = (size_t)ATLAS_REVIEW_SHEET_MAX_BYTES + 1u;
    char buf[8192];
    for (;;) {
        if (out->len >= cap) {
            break;
        }
        size_t remaining = cap - out->len;
        size_t want = sizeof(buf) < remaining ? sizeof(buf) : remaining;
        ssize_t r = read(fd, buf, want);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            atlas_status st =
                atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno, "cannot read %s", path);
            (void)close(fd);
            return st;
        }
        if (r == 0) {
            break;
        }
        atlas_status st = atlas_buf_append(out, buf, (size_t)r, err);
        if (st != ATLAS_OK) {
            (void)close(fd);
            return st;
        }
    }
    (void)close(fd);
    return ATLAS_OK;
}

/* --- the pre-check plus the one confirm call ---------------------------------
 *
 * One entry, start to finish. Every early return has already released every
 * `atlas_decision_document` and `atlas_decision_outcome` it opened -- there is
 * no shared cleanup label because each branch owns a disjoint set of locals.
 *
 * MISSING and MOVED and DISPOSED are decided from exactly one read of the
 * *effective* revision (revision_no = 0): that read alone carries
 * `summary.latest_revision_no` and `summary.status`, which is everything the
 * MOVED and DISPOSED checks need regardless of which specific revision the
 * sheet names. A second read, of one exact revision number, is needed only to
 * learn *that revision's own* content hash, and only two revision numbers are
 * ever asked for afterwards: `latest_revision_no` (real by construction -- it
 * is the number the document itself just reported) and `entry->revision_no`
 * when it equals `latest_revision_no` (real for the same reason). This file
 * never calls `atlas_service_decision_show` with a revision number that might
 * not exist, which is what lets it skip entirely the question of what
 * "this decision has no revision N" (service_decision.c) should map to in the
 * closed `atlas_review_verdict` vocabulary -- a sheet naming a revision higher
 * than anything ever minted still lands on MOVED, correctly, because
 * `entry->revision_no != latest` catches it the same way a stale sheet does. */
static atlas_status walk_entry(atlas_ctx *ctx, const atlas_review_entry *entry, bool check_only,
                               atlas_review_outcome *o, atlas_err *err) {
    o->entry = entry;

    atlas_decision_document doc0;
    atlas_decision_document_init(&doc0);
    atlas_status st = show_revision(ctx, entry->repo, entry->decision, 0, &doc0, err);
    if (st != ATLAS_OK) {
        atlas_decision_document_free(&doc0);
        if (atlas_err_is_transport(err)) {
            /* A lost answer, not a refusal (see atlas/error.h's A12.0 section):
             * the remote form (`ctx == NULL`, a daemon holding the writer lock)
             * can fail this way, and its status is whatever the transport
             * happened to carry -- ATLAS_ERR_REPO or ATLAS_ERR_USAGE included.
             * Reporting a lost answer as MISSING would tell a reviewer a
             * record does not exist when Atlas simply never heard back.
             * Nothing here retries; the walker's contract is "nothing is
             * retried" and a caller can run the sheet again. */
            return st;
        }
        if (st == ATLAS_ERR_REPO) {
            /* NOT_REGISTERED: atlas_service_require_repo's own refusal. */
            atlas_err_init(err);
            o->verdict = ATLAS_REVIEW_MISSING;
            return atlas_buf_set_str(&o->detail, "no such repository", err);
        }
        if (st == ATLAS_ERR_USAGE) {
            /* resolve_uid (service_decision.c) has three refusals, and only
             * two of them can ever reach this branch. "No decision has that
             * id" and "that decision belongs to a different repository" both
             * mean, from this sheet's point of view, that the named
             * repository has no such decision -- the distinction between
             * "does not exist at all" and "exists under a different
             * repository" is not one a reviewer disposing of a record in
             * *this* repository can act on differently. The third, "that is
             * not a decision id" (a malformed uid), cannot happen here: the
             * sheet parser already applied atlas_decision_uid_is_valid to
             * every entry's decision field before this file ever saw one, so
             * `entry->decision` is always a well-formed id by the time it
             * reaches show_revision. */
            atlas_err_init(err);
            o->verdict = ATLAS_REVIEW_MISSING;
            atlas_safe_pool pool;
            atlas_safe_pool_init(&pool);
            atlas_status dst = atlas_buf_appendf(&o->detail, err, "no such decision in %s",
                                                 atlas_safe(&pool, entry->repo));
            atlas_safe_pool_free(&pool);
            return dst;
        }
        /* Anything else is not a per-entry verdict -- a database or integrity
         * failure aborts the whole walk rather than being reported as one
         * entry's disposition. */
        return st;
    }

    int64_t latest = doc0.summary.latest_revision_no;
    atlas_status rst;

    if (entry->revision_no != latest) {
        char now_hash[ATLAS_SHA256_HEX_LEN + 1u];
        if (doc0.summary.revision_no == latest) {
            /* The effective revision already *is* the latest one -- its hash
             * is what we just read. */
            (void)snprintf(now_hash, sizeof(now_hash), "%s",
                           atlas_buf_cstr(&doc0.summary.content_hash));
        } else {
            /* Approved at an earlier revision than a newer, unapproved one:
             * the effective read gave us the approved hash, not the latest
             * one, so the latest's own hash needs its own read. `latest` is
             * real by construction, so this cannot land on "no revision N". */
            atlas_decision_document docL;
            atlas_decision_document_init(&docL);
            atlas_status stL = show_revision(ctx, entry->repo, entry->decision, latest, &docL, err);
            if (stL != ATLAS_OK) {
                atlas_decision_document_free(&docL);
                atlas_decision_document_free(&doc0);
                return stL;
            }
            (void)snprintf(now_hash, sizeof(now_hash), "%s",
                           atlas_buf_cstr(&docL.summary.content_hash));
            atlas_decision_document_free(&docL);
        }
        o->verdict = ATLAS_REVIEW_MOVED;
        o->current_revision_no = latest;
        set_prefix(o->current_prefix, now_hash);
        rst = set_status(o, doc0.summary.status.data, doc0.summary.status.len, err);
        if (rst == ATLAS_OK) {
            rst = atlas_buf_appendf(&o->detail, err, "reviewed r%lld (%.8s), now r%lld (%.8s)",
                                    (long long)entry->revision_no, entry->prefix,
                                    (long long)latest, now_hash);
        }
        atlas_decision_document_free(&doc0);
        return rst;
    }

    /* entry->revision_no == latest: the revision named has not been
     * superseded by a later one. Whether it is still what the sheet says
     * depends on its own hash, which needs revision_no's own content hash --
     * already in hand when the effective revision is that same one. */
    char n_hash[ATLAS_SHA256_HEX_LEN + 1u];
    if (doc0.summary.revision_no == entry->revision_no) {
        (void)snprintf(n_hash, sizeof(n_hash), "%s", atlas_buf_cstr(&doc0.summary.content_hash));
    } else {
        /* Approved at a later revision than the one still un-superseded and
         * equal to `latest` cannot happen -- revision numbers only increase --
         * so this is the "approved earlier, latest is what the sheet names"
         * case. entry->revision_no == latest here, so it is real. */
        atlas_decision_document docN;
        atlas_decision_document_init(&docN);
        atlas_status stN =
            show_revision(ctx, entry->repo, entry->decision, entry->revision_no, &docN, err);
        if (stN != ATLAS_OK) {
            atlas_decision_document_free(&docN);
            atlas_decision_document_free(&doc0);
            return stN;
        }
        (void)snprintf(n_hash, sizeof(n_hash), "%s", atlas_buf_cstr(&docN.summary.content_hash));
        atlas_decision_document_free(&docN);
    }

    if (strncmp(n_hash, entry->prefix, ATLAS_DECISION_CONFIRM_HEX) != 0) {
        /* The revision number the sheet names is still current, but its hash
         * is not what the sheet recorded -- reported the same way a revision
         * bump is, with the "reviewed" and "now" revision numbers equal and
         * their hashes different. */
        o->verdict = ATLAS_REVIEW_MOVED;
        o->current_revision_no = entry->revision_no;
        set_prefix(o->current_prefix, n_hash);
        rst = set_status(o, doc0.summary.status.data, doc0.summary.status.len, err);
        if (rst == ATLAS_OK) {
            rst = atlas_buf_appendf(&o->detail, err, "reviewed r%lld (%.8s), now r%lld (%.8s)",
                                    (long long)entry->revision_no, entry->prefix,
                                    (long long)entry->revision_no, n_hash);
        }
        atlas_decision_document_free(&doc0);
        return rst;
    }

    /* `!parsed` is schema-enforced unreachable today: `decision_documents.
     * current_status` (src/db/migrate.c, migration 13) carries a SQL CHECK
     * naming exactly the five vocabulary members, so no write through
     * SQLite -- not `atlas_service_decision_propose`, not a hand-written
     * `UPDATE`, nothing short of altering or dropping that constraint -- can
     * put any other value there. Checked and handled anyway, because a
     * defensive read should not assume a constraint it did not write and
     * cannot see broken elsewhere (a future migration, a restored backup from
     * an older schema) will always hold; set_status()/the branch below fail
     * safe rather than trust it silently. */
    atlas_decision_state have = ATLAS_DECISION_PROPOSED;
    bool parsed = atlas_decision_state_parse(atlas_buf_cstr(&doc0.summary.status), &have);
    atlas_decision_state need = required_status_for(entry->intent);
    if (!parsed || have != need) {
        o->verdict = ATLAS_REVIEW_DISPOSED;
        rst = set_status(o, doc0.summary.status.data, doc0.summary.status.len, err);
        if (rst == ATLAS_OK) {
            /* Built from `o->status`, not from `doc0.summary.status` a second
             * time: `o->status` was just set to the safe-encoded form above,
             * so re-reading it here rather than re-encoding the raw bytes is
             * what makes it structurally impossible for `detail` and
             * `status` to disagree about what this record's status was.
             * Always taken, `parsed` or not -- a vocabulary member encodes to
             * itself, so this reads exactly as the unencoded form did for
             * every ordinary DISPOSED record, and is also correct on the
             * `!parsed` path this branch exists to cover, without a second
             * branch that could fall out of step with the first. */
            rst = atlas_buf_appendf(&o->detail, err, "the record is %s; %s needs %s",
                                    atlas_buf_cstr(&o->status),
                                    atlas_decision_intent_name(entry->intent),
                                    atlas_decision_state_name(need));
        }
        atlas_decision_document_free(&doc0);
        return rst;
    }

    /* Ready: the revision is current, its hash matches, and its status is the
     * one the intent needs. Nothing has been minted yet. */
    rst = set_status(o, doc0.summary.status.data, doc0.summary.status.len, err);
    atlas_decision_document_free(&doc0);
    if (rst != ATLAS_OK) {
        return rst;
    }

    if (check_only) {
        o->verdict = ATLAS_REVIEW_READY;
        return ATLAS_OK;
    }

    atlas_decision_outcome outcome;
    atlas_decision_outcome_init(&outcome);
    atlas_status cst = atlas_service_decision_confirm(ctx, entry->repo, entry->decision,
                                                       entry->intent, NULL, entry->revision_no,
                                                       &outcome, err);
    if (cst == ATLAS_OK) {
        o->verdict = ATLAS_REVIEW_APPLIED;
        atlas_status ust = set_status(o, outcome.state.data, outcome.state.len, err);
        if (ust == ATLAS_OK) {
            ust = atlas_buf_appendf(&o->detail, err, "%s at r%lld", atlas_buf_cstr(&outcome.state),
                                    (long long)outcome.revision_no);
        }
        atlas_decision_outcome_free(&outcome);
        return ust;
    }
    atlas_decision_outcome_free(&outcome);

    if (atlas_err_is_transport(err)) {
        /* A lost answer during the confirm itself is not a refusal Atlas can
         * carry as REFUSED -- REFUSED means the confirm gave a final answer,
         * and a lost answer here leaves it genuinely unknown whether a
         * capability was minted or spent. Reporting a guess would be worse
         * than reporting nothing; this aborts the whole walk rather than
         * inventing a verdict for it, and this file retries nothing. */
        return cst;
    }

    /* Copied off `err` before either branch below might reuse it, rather than
     * comparing and encoding directly out of `err->msg` while also passing
     * `err` as the very error parameter that write could land in. */
    char msgbuf[ATLAS_ERR_MSG_MAX];
    (void)snprintf(msgbuf, sizeof(msgbuf), "%s", atlas_err_msg(err));

    if (strcmp(msgbuf, ATLAS_DECISION_CONFIRM_MISTYPED_MSG) == 0) {
        o->verdict = ATLAS_REVIEW_ABANDONED;
        atlas_err_init(err);
        return atlas_buf_set_str(&o->detail, "nothing was changed", err);
    }

    /* REFUSED: any other refusal from the confirm, carried whole. The
     * confirm's own message can quote a value read from the repository or the
     * database, so it is UNTRUSTED_DATA and is safe-encoded like any other
     * carried error text before it reaches `detail`. */
    atlas_buf enc = ATLAS_BUF_INIT;
    atlas_status est = atlas_text_encode_safe(msgbuf, strlen(msgbuf), &enc, err);
    atlas_status fst = est;
    if (est == ATLAS_OK) {
        fst = atlas_buf_set(&o->detail, enc.data, enc.len, err);
    }
    atlas_buf_free(&enc);
    if (fst == ATLAS_OK) {
        o->verdict = ATLAS_REVIEW_REFUSED;
        atlas_err_init(err);
    }
    return fst;
}

/* --- the walk ----------------------------------------------------------------- */

atlas_status atlas_service_review_apply(atlas_ctx *ctx, const char *sheet_path, bool check_only,
                                        atlas_review_outcome_cb cb, void *ud,
                                        atlas_review_totals *totals, atlas_err *err) {
    memset(totals, 0, sizeof(*totals));

    if (!check_only) {
        /* First, so a locked profile reads no file and reaches no terminal.
         * Skipped under `check_only`: see this file's own header comment and
         * the contract comment on this function in include/atlas/service.h
         * for why a dry run needs neither. */
        atlas_status auth = atlas_authority_require(ATLAS_AUTHORITY_OP_DECISION_LIFECYCLE, err);
        if (auth != ATLAS_OK) {
            return auth;
        }
    }

    if (!check_only) {
        atlas_terminal *t = NULL;
        atlas_status tst = atlas_terminal_open(&t, err);
        if (tst != ATLAS_OK) {
            return tst;
        }
        atlas_terminal_close(t);
    }

    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_status st = read_sheet_bounded(sheet_path, &raw, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&raw);
        return st;
    }

    atlas_review_sheet sheet;
    st = atlas_review_sheet_parse(raw.data, raw.len, &sheet, err);
    atlas_buf_free(&raw);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sheet.count == 0) {
        /* A header-only sheet is grammatically valid -- T3's parser accepts it
         * on purpose, since refusing an empty *grammar* is not its question to
         * answer. It is this file's question: under the frozen exit-code rule
         * ("0 when every entry ended APPLIED"), zero entries makes that
         * vacuously true, so a sheet truncated to its header alone would exit
         * 0 having disposed of nothing and said nothing -- indistinguishable
         * from a real, empty success unless something here refuses it. Refused
         * here, in the "review sheet:" refusal family the parser's own two
         * bound refusals already use, rather than left as a special case a
         * later, presentation-layer task would have to remember to add. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "review sheet: no entries; there is nothing to review");
    }

    for (size_t i = 0; i < sheet.count; i++) {
        const atlas_review_entry *entry = &sheet.entries[i];
        atlas_review_outcome o;
        atlas_review_outcome_init(&o);

        atlas_status wst = walk_entry(ctx, entry, check_only, &o, err);
        if (wst != ATLAS_OK) {
            atlas_review_outcome_free(&o);
            return wst;
        }

        switch (o.verdict) {
        case ATLAS_REVIEW_READY: totals->ready++; break;
        case ATLAS_REVIEW_APPLIED: totals->applied++; break;
        case ATLAS_REVIEW_ABANDONED: totals->abandoned++; break;
        case ATLAS_REVIEW_MOVED: totals->moved++; break;
        case ATLAS_REVIEW_DISPOSED: totals->disposed++; break;
        case ATLAS_REVIEW_MISSING: totals->missing++; break;
        case ATLAS_REVIEW_REFUSED: totals->refused++; break;
        case ATLAS_REVIEW_UNKNOWN: break; /* never produced; see review.h */
        }

        atlas_status cbst = cb(&o, ud, err);
        atlas_review_outcome_free(&o);
        if (cbst != ATLAS_OK) {
            return cbst;
        }
    }

    return ATLAS_OK;
}
