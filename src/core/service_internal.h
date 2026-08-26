/* Atlas - shared helpers inside the service layer.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Not a public header. Split out so that one command per translation unit can
 * still share repository lookup without duplicating its error messages.
 */
#ifndef ATLAS_SERVICE_INTERNAL_H
#define ATLAS_SERVICE_INTERNAL_H

#include "atlas/ipc.h"
#include "atlas/orchpolicy.h"
#include "atlas/plandriver.h"
#include "atlas/rundriver.h"
#include "atlas/service.h"

/* Loads a registered repository by name, or fails with a message naming it. */
atlas_status atlas_service_require_repo(atlas_ctx *ctx, const char *name, atlas_repo_info *out,
                                        atlas_err *err);

/* Opens the git adapter for a registered repository and verifies that its
 * canonical root has not moved since registration.
 *
 * A13. `data_dir` is where the scanner's mirror lives, and passing NULL means
 * the tree itself is the only acceptable source -- see `atlas_repo_open_git`.
 * When the mirror answered, the two identity checks are skipped, because they
 * are claims about the real tree and the real tree was not opened. */
atlas_status atlas_service_open_repo_git(const atlas_repo_info *info, const char *data_dir,
                                         atlas_git **out, atlas_err *err);

/* The fresh git observation behind `atlas status NAME`, shared by the local
 * read and the daemon-served one. Fills `live_head`, `live_state`, `git_ok`,
 * `git_error` and `head_drift` from `out->repo.root_path` and `out->scanned`. */
atlas_status atlas_service_status_observe_live(atlas_status_report *out, const char *data_dir,
                                              atlas_err *err);

/* --- talking to the daemon --------------------------------------------------
 *
 * One request out, one parsed response back, for the two domains that have no
 * offline path: orchestration and, since A12.0, plans. Both live in the index,
 * `atlasd` is the only writer of either, and a CLI that fell back to opening the
 * database itself would be a second writer.
 *
 * Shared rather than copied. A12.0 added a second translation unit that speaks
 * this protocol, and a second copy of "build the params, call, parse, turn a
 * refusal into an `atlas_err`" would be a second place for the refusal handling
 * to drift — which is the only interesting part of it.
 *
 * `build` writes the request's parameter members and may be NULL for a method
 * that takes none. On success `*out` is a parsed response the caller frees with
 * `atlas_ipc_response_free`, and `raw` holds the bytes it borrows from and must
 * outlive it. A refusal returns the daemon's own status and message and leaves
 * `*out` non-NULL when the response parsed, so a caller can still read whatever
 * typed detail the refusal carried. */
typedef atlas_status (*atlas_service_build_fn)(atlas_json *j, void *ud, atlas_err *err);

atlas_status atlas_service_orch_call(atlas_ctx *ctx, const char *method,
                                     atlas_service_build_fn build, void *ud,
                                     atlas_ipc_response **out, atlas_buf *raw, atlas_err *err);

/* --- A12.0: what `plan run` shares with `job run` and `dispatcher run` -------
 *
 * Two seams, both factored rather than copied, and for the same reason in both
 * cases: a second implementation would be a second answer to a question the
 * whole deployment has to agree on.
 *
 * `atlas_service_orch_driver_filter` writes this uid's driver partition as the
 * comma-separated list a lease request carries — the model drivers when
 * `model_partition`, the rest otherwise, and a repo-tree driver on neither. It
 * refuses with a sentence when the policy configures no driver in that
 * partition.
 *
 * `atlas_service_run_drive` runs A11.1's run driver on one run over this
 * process's socket transport, with the run driver's options assembled from the
 * caller's already-loaded, already-ENABLED root-owned policy. A run that ended
 * BLOCKED is an answer: `ATLAS_OK`, with the status in `rep`. */
atlas_status atlas_service_orch_driver_filter(const atlas_orchpolicy *pol, bool model_partition,
                                              char *out, size_t out_size, atlas_err *err);

atlas_status atlas_service_run_drive(const atlas_orchpolicy *pol, const char *run_uid, FILE *log,
                                     atlas_rundriver_report *rep, atlas_err *err);

/* --- A12.0: the plan transport's response readers ---------------------------
 *
 * Separated from the calls that fetch them so they can be proved without a
 * daemon, exactly as `daemon_internal.h` exposes `atlas_server_dispatch` so the
 * protocol can be tested without a socket. A plan read is gated by
 * `require_submitter`, which reads the **root-owned** orchestration policy, and
 * an unprivileged uid cannot create one anywhere — so the socket half of these
 * is unreachable from the suite and the interesting half is not.
 *
 * Each is a pure function of one parsed `plan.get` response. Between them they
 * carry the three obligations the plan transport has and nothing below it does:
 *
 *   - **the single `atlas-safe-1` decode** — the goal, the gate floor block, a
 *     title and a prompt are decoded here and nowhere else, so the driver never
 *     decodes and never re-encodes;
 *   - **the merged gate list carried verbatim** — Atlas' own canonical
 *     netstring, never decoded, never re-merged, byte-identical from the task
 *     row to the submission;
 *   - **the conservative value for every absent key** — an absent status stays
 *     UNKNOWN, an absent job is an empty uid, and nothing is an error.
 *
 * `atlas_plan_read_task` additionally refuses a response describing a revision
 * other than the one that was asked for: `plan.get` serves the *latest*
 * revision's tasks, and a driver that submitted a superseded revision's stage
 * would be running work the plan no longer holds.
 *
 * Every `out` is caller-initialised and is set, not appended to. */
atlas_status atlas_plan_read_plan(const atlas_ipc_response *r, atlas_plandriver_plan *out,
                                  atlas_err *err);
atlas_status atlas_plan_read_state(const atlas_ipc_response *r, atlas_plan_state *out,
                                   atlas_err *err);
atlas_status atlas_plan_read_task(const atlas_ipc_response *r, const char *plan_uid, int rev_no,
                                  const char *task_key, atlas_plandriver_task *out,
                                  atlas_err *err);

/* --- A12.0: constructing the production plan transport ----------------------
 *
 * `atlas_service_plan_run` builds one of these and hands it to
 * `atlas_plandriver_run`. It is exposed here so that the *shipped* transport —
 * every request builder, every response reader, the single `atlas-safe-1`
 * decode, the opaque carriage of a merged gate list, both drives and the `BUSY:`
 * mapping — is the one an end-to-end test hosts, rather than a second one
 * written beside it that could agree with the driver and disagree with the
 * daemon.
 *
 * The one thing that is *not* compiled in is the policy, and that is the whole
 * reason for the seam: `atlas_orchpolicy_load` walks a root-owned path with no
 * override, so a suite running as an ordinary uid cannot install one, and
 * `tests/test_plan_rpc.c` already establishes the substitution — a policy parsed
 * through `atlas_orchpolicy_parse_bytes`, whose provenance is the test's and
 * whose *content* is checked by the same parser a real deployment's is.
 *
 * `*out` is owned by the caller and released with the matching `_free`, which
 * accepts NULL. `_wire` fills a transport whose `ctx` is the handle, so the
 * handle must outlive every call made through it. `_saw_busy` reports whether
 * the daemon answered `BUSY:` to anything — a report about the invocation, never
 * a verdict about the plan. */
typedef struct plan_xport plan_xport;

atlas_status atlas_service_plan_xport_new(const atlas_orchpolicy *pol, FILE *log, plan_xport **out,
                                          atlas_err *err);
void atlas_service_plan_xport_free(plan_xport *x);
void atlas_service_plan_xport_wire(plan_xport *x, atlas_plandriver_transport *t);
bool atlas_service_plan_xport_saw_busy(const plan_xport *x);

#endif /* ATLAS_SERVICE_INTERNAL_H */
