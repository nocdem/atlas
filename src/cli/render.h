/* Atlas - output renderers.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Two renderers implement one interface, and both are driven by the same service
 * results in the same order. Nothing in a renderer queries the database or git,
 * so human and JSON output cannot drift apart.
 *
 * Renderers are streaming: list items are written as they arrive, so a large
 * result set is never assembled in memory.
 */
#ifndef ATLAS_CLI_RENDER_H
#define ATLAS_CLI_RENDER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "atlas/backup.h"
#include "atlas/gw.h"
#include "atlas/integrate.h"
#include "atlas/json.h"
#include "atlas/maintenance.h"
#include "atlas/safetext.h"
#include "atlas/service.h"
#include "atlas/unit.h"

typedef struct atlas_renderer atlas_renderer;

typedef struct atlas_renderer_vtbl {
    atlas_status (*begin)(atlas_renderer *r, const char *command, atlas_err *err);
    atlas_status (*end)(atlas_renderer *r, atlas_err *err);
    atlas_status (*note_repo)(atlas_renderer *r, const char *repo, atlas_err *err);
    atlas_status (*note_query)(atlas_renderer *r, const char *query, atlas_search_mode mode,
                               atlas_err *err);
    atlas_status (*list_begin)(atlas_renderer *r, const char *key, atlas_err *err);
    /* `singular` and `plural` are both supplied because English plurals are not
     * derivable by appending 's' ("repository" / "repositories"). */
    atlas_status (*list_end)(atlas_renderer *r, const char *singular, const char *plural,
                             int64_t count, atlas_err *err);
    atlas_status (*doctor)(atlas_renderer *r, const atlas_doctor_report *rep, atlas_err *err);
    atlas_status (*version)(atlas_renderer *r, atlas_err *err);
    atlas_status (*repo_item)(atlas_renderer *r, const atlas_repo_info *ri, atlas_err *err);
    atlas_status (*repo_added)(atlas_renderer *r, const atlas_repo_info *ri, atlas_err *err);
    atlas_status (*repo_removed)(atlas_renderer *r, const atlas_repo_info *ri, atlas_err *err);
    atlas_status (*scan)(atlas_renderer *r, const char *repo, const atlas_scan_summary *s,
                         atlas_err *err);
    atlas_status (*status)(atlas_renderer *r, const atlas_status_report *s, atlas_err *err);
    atlas_status (*search_item)(atlas_renderer *r, const atlas_search_hit *h, atlas_err *err);
    atlas_status (*file)(atlas_renderer *r, const atlas_file_report *f, atlas_err *err);
    atlas_status (*history_item)(atlas_renderer *r, const atlas_history_row *h, atlas_err *err);
    /* Diff is reported as a header, then entries grouped by scope, then a tail. */
    atlas_status (*diff_begin)(atlas_renderer *r, const atlas_diff_report *rep, atlas_err *err);
    atlas_status (*diff_item)(atlas_renderer *r, const atlas_diff_entry *e, atlas_err *err);
    atlas_status (*diff_end)(atlas_renderer *r, const atlas_diff_report *rep, atlas_err *err);
    /* --- A1 --- */
    /* A8. One method for both a list row and a detail view: the fields are the
     * same and only the depth differs, so two methods would be two places to
     * forget a field. */
    atlas_status (*job_item)(atlas_renderer *r, const atlas_job_render *jr, atlas_err *err);
    atlas_status (*daemon_status)(atlas_renderer *r, const atlas_daemon_status_report *rep,
                                  atlas_err *err);
    atlas_status (*daemon_ping)(atlas_renderer *r, bool reachable, const char *socket_path,
                                const char *detail, atlas_err *err);
    atlas_status (*repo_state)(atlas_renderer *r, const atlas_repo_state_report *rep,
                               atlas_err *err);
    atlas_status (*sync)(atlas_renderer *r, const char *repo, const atlas_sync_report *rep,
                         atlas_err *err);
    atlas_status (*event_item)(atlas_renderer *r, const atlas_event_row *row, atlas_err *err);
    atlas_status (*events_end)(atlas_renderer *r, int64_t cursor, bool more, atlas_err *err);
    atlas_status (*unit_text)(atlas_renderer *r, const char *text, atlas_err *err);
    atlas_status (*unit_install)(atlas_renderer *r, const atlas_unit_install_report *rep,
                                 bool uninstall, atlas_err *err);
    /* --- A2 ---
     *
     * One method rather than four. `integrate claude print|install|uninstall|
     * doctor` all report the same shape — what was found, what changed, what is
     * wrong — and giving each its own renderer method would have been four
     * places for the human and JSON forms to drift apart instead of one. */
    atlas_status (*integrate)(atlas_renderer *r, const atlas_integrate_report *rep,
                              const char *action, const char *commands, atlas_err *err);
    /* --- A3 ---
     *
     * Six methods rather than one per command. `code file` is several
     * independent lists sharing one header, and `code deps` and `code impact`
     * are the same traversal in opposite directions — so the shapes are the
     * report, the symbol, the edge and the traversal candidate, and the commands
     * compose them. Fewer shapes is fewer places for the two renderers to
     * drift. */
    atlas_status (*code_status)(atlas_renderer *r, const atlas_code_status_report *rep,
                                atlas_err *err);
    atlas_status (*code_file)(atlas_renderer *r, const atlas_code_file_report *rep,
                              atlas_err *err);
    atlas_status (*code_symbol_item)(atlas_renderer *r, const atlas_code_symbol_row *row,
                                     atlas_err *err);
    atlas_status (*code_edge_item)(atlas_renderer *r, const atlas_code_edge_row *row,
                                   atlas_err *err);
    atlas_status (*code_walk_item)(atlas_renderer *r, const atlas_code_walk_row *row,
                                   atlas_err *err);
    atlas_status (*code_walk_end)(atlas_renderer *r, const atlas_code_walk_summary *sum,
                                  atlas_err *err);
    /* A8-CI: the compiler-derived index.
     *
     * Deliberately separate methods from the A3 ones above, not an extension of
     * them. The two layers answer different questions with different evidence
     * and are never merged, so a renderer that could print one where the other
     * was meant would be exactly the conflation the season forbids. */
    atlas_status (*sem_status)(atlas_renderer *r, const atlas_sem_status_report *rep,
                               atlas_err *err);
    /* A9.2.3. The build description and the derived state, over the same report
     * `sem_status` renders — one report, two views: `sem-status` leads with the
     * generation, `sem-config` leads with what an operator configured and what
     * the daemon will do about it. */
    atlas_status (*sem_config)(atlas_renderer *r, const atlas_sem_status_report *rep,
                               atlas_err *err);
    atlas_status (*sem_symbols)(atlas_renderer *r, const atlas_sem_symbols_report *rep,
                                atlas_err *err);
    atlas_status (*sem_graph)(atlas_renderer *r, const atlas_sem_graph_report *rep,
                              atlas_err *err);
    atlas_status (*sem_indexed)(atlas_renderer *r, const atlas_sem_index_summary *sum,
                                atlas_err *err);
    atlas_status (*sem_impact)(atlas_renderer *r, const atlas_sem_impact_report *rep,
                               atlas_err *err);
    atlas_status (*sem_context)(atlas_renderer *r, const atlas_sem_context_report *rep,
                                atlas_err *err);
    /* A named list, for the commands that emit several.
     *
     * `list_begin`/`list_end` write the count under the fixed key `count`,
     * which is A0's contract and is right for a command with one list. A file
     * context has three, and three `count` members in one object is a document
     * whose meaning depends on which one a parser keeps. These write
     * `<key>` and `<key>_count`, so every list carries its own. */
    atlas_status (*code_list_begin)(atlas_renderer *r, const char *key, atlas_err *err);
    atlas_status (*code_list_end)(atlas_renderer *r, const char *key, const char *singular,
                                  const char *plural, int64_t count, bool more, atlas_err *err);
    /* --- A4 ---
     *
     * Four shapes, because there are four things to show: a document in a list,
     * a whole document, one entry in its timeline, and the outcome of a
     * lifecycle write. `decision export` reuses the whole-document shape rather
     * than adding a fifth, so the export and `decision show --json` cannot
     * describe one document differently.
     *
     * Every string these receive is already safe-encoded by the service layer
     * — decision prose is untrusted whatever its status — so the renderers do
     * not encode again, and both say so at the top of the file. */
    atlas_status (*decision_item)(atlas_renderer *r, const atlas_decision_summary *s,
                                  atlas_err *err);
    atlas_status (*decision_show)(atlas_renderer *r, const atlas_decision_document *d,
                                  atlas_err *err);
    atlas_status (*decision_event)(atlas_renderer *r, const atlas_decision_timeline_entry *e,
                                   atlas_err *err);
    atlas_status (*decision_outcome)(atlas_renderer *r, const atlas_decision_outcome *o,
                                     atlas_err *err);
    /* Migration 10: one event in the account of a document's relations. Its own
     * method rather than a variant of `decision_event`, because the two are
     * different ledgers — one records what happened to the document's lifecycle
     * and this one records what happened to its edges. */
    atlas_status (*decision_edge)(atlas_renderer *r, const atlas_decision_edge_entry *e,
                                  atlas_err *err);
    /* The lifecycle totals a listing reports beside its page. */
    atlas_status (*decision_counts)(atlas_renderer *r, const atlas_decision_counts *c,
                                    atlas_err *err);
    /* Whether the cached status agrees with the append-only ledger. Its own
     * method rather than a field on something else, because it is a statement
     * about the *record* rather than about any one decision, and `decision
     * history` is where a reader would look for it. */
    atlas_status (*decision_ledger)(atlas_renderer *r, bool agrees, atlas_err *err);
    /* --- A5 ---
     *
     * Three shapes for three operations, and `backup restore` reuses the verify
     * shape twice — once for the backup it was handed and once for the index it
     * installed — rather than growing a fourth. The two must be described
     * identically or an operator cannot compare them.
     *
     * Every string these receive is Atlas-owned: a path the operator typed
     * (encoded here, because a path is bytes), a hex digest, a fixed
     * vocabulary word, or a fixed reason string that is a literal in
     * src/core/service_maintenance.c. No repository or model text reaches
     * them. */
    atlas_status (*backup_created)(atlas_renderer *r, const atlas_backup_report *rep,
                                   atlas_err *err);
    atlas_status (*backup_verified)(atlas_renderer *r, const atlas_backup_verify_report *rep,
                                    const char *key, atlas_err *err);
    atlas_status (*backup_restored)(atlas_renderer *r, const atlas_backup_restore_report *rep,
                                    atlas_err *err);
    /* One long-running operation's state. `message` and `detail` arrive already
     * safe-encoded from the daemon and are printed as-is. */
    atlas_status (*operation_status)(atlas_renderer *r, const atlas_operation_report *rep,
                                     atlas_err *err);
    atlas_status (*maintenance)(atlas_renderer *r, const atlas_maintenance_report *rep,
                                atlas_err *err);

    /* --- A6: impact gates -------------------------------------------------
     *
     * One shape for both `gate check` and `gate show`, because they are one
     * query: `show` is `check` with a single item kept, and giving them
     * separate renderers would let the two descriptions of one assessment drift
     * apart.
     *
     * Almost everything here is Atlas-owned — two closed vocabularies, object
     * ids, digests and counts — which is what lets it be printed plainly and
     * lets a model be shown it. The one exception is each assessment's `title`,
     * which is project prose; it is already safe-encoded by the service layer
     * and is labelled as untrusted wherever it appears, exactly as A4's
     * decision renderers label theirs. */
    atlas_status (*gate)(atlas_renderer *r, const atlas_gate_report *rep, atlas_err *err);

    /* --- A9.2: verification -----------------------------------------------
     *
     * One renderer for all three `verify` subcommands, because they report the
     * same structure and a second would drift.
     *
     * **The score and the probability are printed differently and are never
     * both present.** `confidence_score` is an integer out of 100 and carries
     * no percent sign anywhere in either renderer; `calibrated_probability` is
     * emitted only when calibration supports it, and is absent — not zero —
     * otherwise. That separation is the point of the pair existing, and the
     * schema enforces it independently.
     *
     * `claim_text` is project prose and is safe-encoded by the service layer,
     * labelled untrusted exactly as A4's decision renderers label theirs.
     * Verification changes a status, never the nature of bytes. */
    atlas_status (*verify)(atlas_renderer *r, const atlas_verify_report *rep, atlas_err *err);
    /* A9.2.1. What one intake operation recorded: the uid Atlas minted, the
     * actor it resolved the speaker to, and whether the submission was a
     * duplicate of one already held.
     *
     * `duplicate` is reported rather than silent because a retry must not read
     * as a second corroboration — to the caller, and more importantly to
     * whoever later reads the trail. `verb` names which operation ran, so one
     * renderer serves all six rather than six near-identical ones drifting
     * apart. EVALUATE additionally carries an assessment. */
    atlas_status (*verify_intake)(atlas_renderer *r, const char *verb,
                                  const atlas_verify_intake_result *res, atlas_err *err);

    /* --- A9: remote credentials -------------------------------------------
     *
     * `apikey_created` is the one renderer in Atlas that prints a secret, and
     * it prints it exactly once because that is the only moment it exists. The
     * JSON form carries it too — an operator scripting a bootstrap needs it —
     * and both say so in the output, so nobody has to infer that it will not
     * come back.
     *
     * `apikey_listed` must never print one, and cannot: `atlas_apikey_record`
     * has no field that could hold a plaintext. The label is operator text,
     * validated at creation to printable ASCII, and is still safe-encoded on
     * the way out for the reason every other value is. */
    atlas_status (*apikey_created)(atlas_renderer *r, const atlas_apikey_created *c,
                                   atlas_err *err);
    atlas_status (*apikey_listed)(atlas_renderer *r, const atlas_apikey_listing *l,
                                  atlas_err *err);
    atlas_status (*apikey_revoked)(atlas_renderer *r, const char *key_id, bool changed,
                                   atlas_err *err);
} atlas_renderer_vtbl;

struct atlas_renderer {
    const atlas_renderer_vtbl *v;
    FILE *out;
    atlas_json *j; /* JSON renderer only */
    bool json;
    bool in_list;
    int64_t items;
    /* Untrusted values are encoded through this before they are printed. */
    atlas_safe_pool safe;
    /* Diff rendering state: which scope section is currently open. */
    int open_scope;
    bool scope_open;
};

extern const atlas_renderer_vtbl ATLAS_RENDERER_HUMAN;
extern const atlas_renderer_vtbl ATLAS_RENDERER_JSON;

/* Formats a unix timestamp as ISO-8601 UTC, or "-" for a zero timestamp. */
void atlas_format_time(int64_t unix_time, char *out, size_t out_size);
/* Shortens a hex object id for human output. */
void atlas_short_oid(const char *oid, char *out, size_t out_size);

/* Reports a failed command. In JSON mode this writes a complete error document
 * to `out` so a caller parsing stdout always receives valid JSON. */
void atlas_render_error(FILE *out, FILE *errout, bool json, const char *command,
                        const atlas_err *err);

#endif /* ATLAS_CLI_RENDER_H */
