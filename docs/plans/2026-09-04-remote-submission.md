# A14 — Remote submission: a job an operator submits from wherever they are, through a credential the daemon verifies itself, under bounds the policy sets and the request cannot — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: `superpowers:subagent-driven-development`
> (or `superpowers:executing-plans` for inline execution). Tasks are dispatched one at
> a time; the reviewing session reads every diff before the next task starts. Steps use
> checkbox (`- [ ]`) syntax. **T1 must not be dispatched until §Decisions the operator
> must be asked has been put to the operator and answered, all three rows.** That
> section exists because A15's plan left a choice in the document and the choice died
> there, and fourteen hours went into building the wrong half.
>
> **Deviation from the writing-plans skill, stated deliberately, exactly as the A12.0,
> A12.1, A15 and A16 plans stated it:** the operator has assigned roles — the planner
> (Fable) writes this plan and reviews; the executors (Opus) write all production code.
> This plan therefore pins *interfaces, vocabularies, refusal sentences, policy keys,
> route rows, tool schemas, UI wording, migration shape and test obligations* exactly,
> and leaves C and JavaScript function bodies to the executor. Where the skill says
> "show the code", this plan shows the contract the code must satisfy. Everything else
> in the skill applies: bite-sized steps, TDD, frequent commits, no placeholders, no
> "similar to Task N".

**Goal:** An operator hands Atlas a task from wherever they are — from the external
model they already talk to over the MCP tunnel, or from Mission Control on a phone —
and the task becomes an ordinary orchestration job: queued through the one write
point, pinned to a commit, bounded by the root-owned policy, gated by the operator's
own gate floor, executed by a dispatcher that already exists, and settled by nothing
that a model or a browser said. The gateway acquires no authority of its own in the
process: it carries a credential the daemon verifies, and without that credential in
flight it can submit exactly what it can submit today, which is nothing.

**Architecture:** One new ungrantable scope (`jobs:submit`), derived by the daemon for
exactly the credentials root-owned `remote_submit_key` lines name; one new daemon
method group of four names offered to the gateway's uid only under that policy, every
one of which carries the bearer token and verifies it in the daemon — the two that
write, inside the transaction that writes; one migration (32) adding `submit_key_id`
beside `submitter_uid` on the job row and on the ledger; seven policy keys in
`/etc/atlas/gateway.conf` that decide driver, mode, gate floor, attempts and two
per-credential budgets, so that a remote request may name the repository, the task
and an idempotency key and nothing else; four rows in the gateway's bearer-only route
table that A16 builds; four remote-only MCP tools; a Jobs view in Mission Control; a
`--remote` view for the operator's own terminal; and a separate written acceptance of
a cleartext submission channel that the disposal acceptance never implies. Every job
row this season writes still goes through `atlas_orch_apply_in_tx`, which keeps exactly
one caller.

**Sentence the season exists for** (the roadmap's, kept):
> **THE GATEWAY CANNOT SUBMIT WORK BECAUSE OF WHO IT RUNS AS, AND THE OBVIOUS FIX IS
> TO STOP THAT BEING TRUE.**

and the one this plan adds, which is what stops the obvious fix from being taken:

> **A CREDENTIAL IN FLIGHT IS THE ONLY AUTHORITY THE GATEWAY EVER HOLDS, AND THE
> POLICY — NOT THE REQUEST — DECIDES WHAT A SUBMISSION MAY COST.**

**The honest paragraph, in the same breath as the capability.** A remote submission is
worth exactly this: a credential a root-owned line names was presented over the
gateway's listener, the daemon verified it against the key table as it stood in the
transaction that queued the job, and the job it queued is one the policy's driver,
mode, gate floor, attempt bound and daily bound describe. It establishes nothing about
who or what presented the credential — a person on a phone and a model in an agentic
loop present it identically, and Atlas cannot tell them apart and does not claim to.
What it *does* is start a worker, or queue one for the operator to start; and on this
deployment a model worker runs as the operator's own account, in the operator's own
HOME, with the operator's own filesystem authority — A8.1's stated cost, unchanged by
this season and now reachable from outside the machine. The bound on what such a
worker may *write* is Claude Code's permission mode (`acceptEdits`, edits accepted in
the workspace only); the bound on what it may *read* is nothing Atlas sets; and every
byte it reads enters the model provider's transcript under the operator's account. A
task text is therefore a prompt that runs as the operator. This season bounds how many
such prompts a credential may queue and what verification they run under; it does not
and cannot bound what one prompt may ask a worker to read. Every document this season
writes says this in the paragraph that announces the capability, and the tripwire scans
two of them for it.

**Tech stack:** C17, the existing A8 orchestration write point and its op struct, the
existing A9 gateway and policy loader, the A16 bearer-only route table and channel test
tooling (dependencies named explicitly in §What A16 leaves), the shared MCP tool table,
hand-written HTML/CSS/JavaScript with no build step (embedded by `tools/atlas_embed.c`),
one additive SQLite migration, the first-party test harness, and two policy-injection
channels: the in-process dispatch edge `tests/test_plan_rpc.c` already builds, and the
`atlas_daemon_opts` test-hook shape P0 established and A16 extends.

**Spec:** the brief this plan was written from (the controller's message of
2026-09-04); `docs/roadmap.md` "Next: A14 — a job an operator submits from wherever
they are" (lines 1523–1620 at `c06e0a8`), which carries the sentence, the verified
constraint, the tempting fix that must not be taken, three shapes and three things that
must be true whichever shape wins; `docs/plans/2026-09-04-browser-disposal.md` in full,
with both execution amendments; `CLAUDE.md` A7/A7.1, A8/A8.1, A9, A11.0, A11.1–A11.4,
A11.6, A12.0; `docs/orchestration.md`; `docs/remote-access.md`; `SECURITY.md` "A9: the
remote gateway, and what it is worth" and "Approving a record from the browser: refused
today, and why".

**Season number: A14, and the numbering does not follow time here.** The roadmap
reserved A14 for remote submission in `752686e` when A12.1 closed; the operator then put
the review surface first, and A15 (shipped) and A16 (in flight) took the next two
numbers while A14 waited. The roadmap's own note, written at the operator's
instruction, records that A14 has said "Next" twice without a line of code, so a reader
must not mistake the section for shipped work. The number is kept for A16's stated
reason: renaming a committed section touches its sentence for no gain, and this
roadmap's numbers already do not follow time — A10.1 shipped after A11.6, A12.1 after
A13. **Nothing in this plan is true of the tree yet**: `git log --all --oneline | grep
-i a14` finds `752686e` (the reservation) and `c06e0a8` (the "never started" note), and
nothing else.

**The tree this plan was read at:** `c06e0a8`, 2026-09-04, `main`. The working tree
carried A16's T4 uncommitted (`include/atlas/gwpolicy.h`, `include/atlas/gateway.h`,
`src/gw/gwpolicy.c`) while this was read; every line number below is from the
committed tree unless marked *(A16 T4, uncommitted)*, and is cited together with the
function it is in, because A15 measured twelve of its own plan's sentences false by the
time execution reached them and five of A16's documentation defects were references
copied from a plan rather than read from the tree. **A16's T5, T6 and T7 will move
lines in `src/gw/gateway.c`, `src/ipc/server_gw.c`, `src/ipc/server.c` and
`src/gw/ui/mission-control.html` before A14's T1 is dispatched.** Where a function name
and a number disagree, the function name is the pin, and T1's first step is to re-read
every reference in this plan against A16's final commit and amend this document.

---

## Global constraints (repo-wide, every task inherits these)

- Warnings are errors (`ATLAS_WERROR=ON`). No new third-party dependency, no
  `FetchContent`, no network at build time. No shell. **No yyjson call site in
  `src/gw`** — A16's form-body decision removes the need, and this season forwards the
  same syntax. No Python, Node, Go or Rust anywhere in the build, tests or runtime; the
  page stays hand-written.
- **Never modify a registered target repository from Atlas' own code.** A14 adds no
  writer against a tree. A remotely submitted job that runs in a *workspace* edits a
  snapshot; a remotely submitted job that names a *repo-tree* driver is queued and
  waits for the operator's own foreground driver, exactly as a locally submitted one
  does, and no background loop this season adds or enables can ever lease it
  (`atlas_service_orch_driver_filter`, `src/core/service_orch.c:354-393`, and the
  daemon's own refusal of a repo-tree grant to an unfiltered lease).
- **`atlas_orch_apply_in_tx` keeps exactly one caller** — `atlas_orch_apply`
  (`include/atlas/orch_ops.h`, the "single write point" comment). The remote methods
  build ordinary `atlas_orch_op`s and submit them through `atlas_writer_orch`
  (`src/daemon/writer.c:3019`), which is the path every orchestration write takes. The
  credential verification and the budget checks are calls *inside* `op_submit` and
  `op_cancel` (`src/db/db_orch.c:1097`, `:1421`), not callers of the write point.
- **`atlas_decision_apply_in_tx` keeps exactly three callers and
  `atlas_service_decision_confirm` exactly two.** This season touches neither the
  decision layer nor any lifecycle verb; the two tripwire scans
  (`tests/test_decision_mcp.c:518-556`, `tests/test_review_apply.c:1439-1470`) must
  pass unchanged. No file this season creates may contain either function's name
  immediately followed by an opening parenthesis, even in prose.
- **`submitter_uid = 992` never appears in `/etc/atlas/orchestration.conf`**, in a
  template, in a test fixture's policy text, or in a document as anything but the
  forbidden line. `tests/test_orch_model.c` gains a case asserting the orchestration
  policy loader still refuses nothing about it — it is a legal uid — and that the
  refusal is therefore *this plan's* and the deployment's, not the parser's; the
  acceptance table names the deployment check that proves the line is absent.
- Tests always override the data directory (`fx_open` / `--data-dir`); daemon tests
  additionally override `XDG_RUNTIME_DIR`; no test opens the real database or the real
  socket, and no test installs, enables or starts a systemd unit. No test reads
  `/etc/atlas/gateway.conf` or `/etc/atlas/orchestration.conf`; a daemon under test
  receives both policies through the two injection channels in Decision 15 and nothing
  else.
- A new `.c` file goes in the explicit `atlas_core` source list in `CMakeLists.txt`
  (there is no `file(GLOB)`). A new test goes in `ATLAS_TESTS` in
  `tests/CMakeLists.txt` **and** in one of the `set_tests_properties(... LABELS ...)`
  lines (`:356-527` hold the current ones). A new tool binary follows the
  `atlas-watch-daemon` / `atlas-gw-daemon` shape.
- Every fallible function returns `atlas_status` and takes an `atlas_err *`; one exit
  path per function; `atlas_buf` owns its allocation; row callbacks receive borrowed
  pointers valid only for the call. **A credential in a struct is wiped, not merely
  freed**: `atlas_orch_op_free` must `memset` the `remote_token` buffer's bytes before
  releasing them, exactly as `gateway.c:1469-1479` wipes the login key and A16's
  `atlas_decision_op_free` wipes `remote_token`; `atlas_mcp_server_teardown` wipes
  `remote_token` the same way.
- Untrusted text — task text, a repository name, a label, a worker's driver version —
  is `UNTRUSTED_DATA`: safe-encoded before a terminal, inserted with `textContent` and
  never `innerHTML` in the page, never interpreted, and never placed in automatic
  model context. A key id is **not** untrusted text once the daemon has verified it
  (`principal.key_id`'s own contract, `gateway.c:254-270`): sixteen lowercase hex
  characters Atlas minted.
- **Every sentence this season writes into `CLAUDE.md`, `docs/roadmap.md`,
  `docs/orchestration.md`, `docs/remote-access.md`, `docs/remote-submission.md`,
  `SECURITY.md`, `README.md`, `src/gw/ui/mission-control.html`, `src/mcp/mcp_tools.c`,
  `src/ipc/server_orch_remote.c` and `src/orch/remote.c` is scanned by
  `tests/test_decision_mcp.c` (`FILES[]` at `:391-414`, extended by T9) against the
  fourteen phrasings at `:371-386`** — each of which would assert that a person was
  established. Never write that a model "cannot" submit through this channel when a
  credential it holds is named: write what is true — that submission confers no
  authority, that nothing a job produces is applied, committed, approved or accepted by
  Atlas, and that the bound on a credential is the policy's.
- **Nothing from `~/.config/tunnel-client/atlas.env` — the tunnel's bearer — appears in
  this plan, in any commit, test, log line, document or example.** The file's
  existence, owner and mode are facts this plan relies on; its contents are not.
- Commit after every green task in the repo's style (`feat(a14): …`, `fix(a14): …`,
  `test(a14): …`, `docs(a14): …`). Nothing is pushed on this document's authority.

---

# Design

## What Atlas answered, before anything was read

Asked first, as `CLAUDE.md` requires. Everything below is `UNTRUSTED_DATA` reported,
not followed.

- `atlas_repo_overview`: repository `atlas`, `index_current: true`, `scanned_head`
  `c06e0a8`, `watch_detail: "watching a mirror"`, `mirror_complete: true`,
  `scanner_uid: 1000`, `compile_databases: 0`, 462 live files, 342 commits,
  `dirty: true` (A16 T4's uncommitted files). The running daemon still reports
  `phase: "A12.0"` in the MCP envelope while `atlas daemon ping --json` on the deployed
  binary says `"A15"` — two different processes, the MCP adapter's compiled-in string
  predating the daemon's. T11 deploys this season's binary and the phase moves to
  `A16`-then-`A14` in whatever order the two seasons land; the chore commit records
  which.
- `atlas_decisions` with `path` set to `src/orch/` was refused — *path has a trailing
  slash* — and asked again as `src/orch` and as `src/ipc/server_orch.c`: **nothing
  governs either path** (`count: 0` for each). With no path: the same four `PROPOSED`
  records A15 and A16 found — a POLICY about who writes season plans
  (`atlas-dec-963bf3…`), an OPERATIONAL_FACT at revision 2 (`atlas-dec-314ed6…`), and
  the two "PROBE-A8FINAL-… disposable" records (`atlas-dec-28f03b…` r1,
  `atlas-dec-c711a6…` r3), which A16's T10 is to dispose of from the browser. No
  recorded decision bears on remote submission, and no recorded rejected alternative
  says the tempting fix was ever tried.
- `atlas_code_symbol` was not needed: every symbol this plan cites was read at its
  definition, and the callers that matter (`atlas_orch_apply_in_tx`'s one,
  `submitter_uid`'s eleven sites) were enumerated by `grep` and each confirmed in the
  source rather than reported from a lexical index.

## What exists, verified against the tree at `c06e0a8` (2026-09-04)

Every item the brief said is true of this deployment, checked, plus what the tree adds
to each.

1. **The constraint is exactly where the roadmap says it is, and it is two comparisons
   and a policy line.** `dispatch()` in `src/ipc/server.c` consults the orchestration
   client group by name for every peer (`:1238-1246`) and the dispatcher group only
   when `atlas_orchpolicy_is_any_dispatcher` says so (`:1303-1312`); each client
   method then calls `require_submitter` (`src/ipc/server_orch.c:97-108`), which is
   `atlas_orchpolicy_permits_submitter(p, ds->peer_uid)` (`src/orch/policy.c:478-488`)
   over the `submitter_uid` lines the loader read (`:234-240`). `/etc/atlas/
   orchestration.conf` on this machine says `submitter_uid = 1000` and
   `/etc/atlas/gateway.conf` says `gateway_uid = 992`, so the gateway is refused with
   *this connection may not submit or read jobs* and hears `unknown method` for every
   `dispatch.` name. Nothing in `src/gw` is consulted. That is the sentence the brief
   requires to stay true, and Decision 1 says how it does.
2. **`submitter_uid` is established once, from the kernel, and used in eleven places.**
   Written from `ds->peer_uid` at `server_orch.c:231` (jobs) and `:1148`/`:1287`
   (plans); `NOT NULL` on `orch_jobs` (`src/db/migrate.c:1639`, with the comment "Never
   from the request body") and `orch_idempotency`'s primary key (`:1826-1831`); refused
   at zero by `atlas_orch_spec_validate` (`src/orch/orch.c:501-506`, "a job
   specification reached validation with no trusted submitter"); **fed into the spec
   digest** (`orch.c:861`, documented at `docs/orchestration.md:71` — "two principals
   sending identical text are not making the same request"); the scope of every
   client read — `job.get` (`server_orch.c:489`), `job.artifact` (`:714`),
   `job.run_status` (`:782`), `job.list` (`atlas_db_orch_job_list`,
   `src/db/db_orch.c:3869-3886`) — and of `op_cancel`'s ownership check
   (`db_orch.c:1435`); and inherited verbatim by a follow-up task
   (`spawn_follow_up`, `db_orch.c:2431-2439`: "the row's `submitter_uid` stays the
   parent's, because the task belongs to the same principal's work and the
   idempotency key is scoped to that principal"). Decision 3 keeps every one of those
   uses true.
3. **Who executes a queued job, on this machine, today.** Five units are loaded and
   enabled: `atlas.service` (uid 994, the daemon), `atlas-dispatcher.service` (uid
   993, `atlas dispatcher run`), `atlas-gateway.service` (992), `atlas-scanner.service`
   (1000) and `atlas-tunnel.service`. The orchestration policy names
   `model_dispatcher_uid = 1000`, `model_credential = operator_session`,
   `live_model = on`, drivers `fake claude claude-repo claude-plan`, `mode = patch`,
   `max_attempts = 3`, `max_wall_timeout_ms = 900000`. **No model dispatcher is
   running**: `deploy/a8/atlas-model-dispatcher.service` is a *user* unit ("Installed
   to `~/.config/systemd/user/`"), it is not among the loaded units, and no `atlas
   dispatcher run` process runs as 1000. So today a job whose driver needs a model is
   leased by nothing until the operator starts one by hand — which is how every
   `claude` job in `atlas job list` ran. The worker dispatcher (993) leases only
   non-model drivers (`atlas_service_orch_driver_filter`,
   `src/core/service_orch.c:354-393`), and *neither* dispatcher is ever handed a
   repo-tree driver (`:366-378`); only the foreground `atlas job run` carries
   `claude-repo`, by naming the task (`src/orch/rundriver.c:1-30`;
   `op_lease`'s `PICK_ONE`, `db_orch.c` "A lease may name the job it wants"). The
   dispatcher loop is sequential — no thread and no fork in `src/orch/dispatch.c` —
   so one model dispatcher runs one attempt at a time. Decision 4 and §Decisions the
   operator must be asked, row 1, follow from this.
4. **What a model worker may do, and who decides.** `atlas_driver_claude_build_argv`
   (`src/orch/driver.c:454-505`) runs `claude --print --output-format stream-json
   --permission-mode acceptEdits [--model M] TASK`; its own comment (`:486-490`) says
   "the isolation is left to the account and the service sandbox — never to the
   model's cooperation". Under `operator_session` the child's HOME is the dispatcher's
   real HOME (`claude_exec`, `:516` onward) and the process is uid 1000, so the
   operator's own `~/.claude/settings.json` applies to it — on this machine that file
   sets `defaultMode` and two `Bash(` allow patterns; Atlas never reads it and this
   plan does not reproduce it. The workspace is `model_worker_root =
   /home/nocdem/.local/state/atlas-model`. `docs/orchestration.md:506-511` states the
   cost — "those jobs hold the operator's filesystem authority, not `atlas-worker`'s,
   so A7.1's OS isolation does not cover them" — and `deploy/a8/
   atlas-model-dispatcher.service` says "It can read and write whatever the operator
   can." **Nothing this season can narrow that**, and Decision 7 is what follows.
5. **How the external model reaches the gateway, precisely.** `tunnel-client run
   --profile atlas` runs as uid 1000; `ss` shows its only network connection is
   outbound TLS to `172.66.0.243:443`; its profile posts MCP to
   `http://192.168.0.198:8799/mcp` with an `Authorization` header read from
   `~/.config/tunnel-client/atlas.env` (mode `0600`, owner `nocdem`). `192.168.0.198`
   is this host's own address on `enp1s0`, so that request is delivered by the kernel
   to the same host and **never crosses the LAN segment**; the credential's exposure
   on this path is the file in the operator's HOME and the far side of the tunnel,
   not the wire. A browser on the LAN posting to the same listener *does* cross the
   segment in the clear. Decision 11's chain is written from this, not from the
   listener alone.
6. **The credentials that exist.** `atlas api-key list`: `key_b2578f48143c06d3`
   `chatgpt-tunnel` ACTIVE with five read scopes (`context:read repo:read
   decisions:read graph:read impact:read`); `key_c0005df884842e55` `mission-control`
   ACTIVE with six (adds `audit:read`); three REVOKED. No key holds `memory:write` or
   `decisions:dispose`, and none can (`src/gw/apikey.c:22-37`, `grantable = false`;
   `src/core/service_apikey.c:119-131` refuses by name). A16 T1 has landed
   (`127b54d`, `cca9486`): `ATLAS_SCOPE_DECISIONS_DISPOSE` is at
   `include/atlas/apikey.h:115`, `atlas api-key create --no-scopes` exists, and its
   success block says "Only a root-owned `remote_dispose_key` line … can give it one
   scope, `decisions:dispose`, and nothing else" — **a sentence this season makes
   false and T1 amends** (§What A16 leaves).
7. **The gateway's route table is reads, and `api_handle` is GET-only for it.**
   `API_ROUTES[]` (`src/gw/gateway.c:963-1028`) holds twenty-six rows, every one a
   read, scope-checked at `:1212`; `api_handle` (`:1179`) refuses every non-GET
   method for the whole table; the `/api/` block resolves a principal from
   `session_get` first, `authenticate` second, the anonymous floor third. **There is
   no `job.` route and no `job.` MCP tool**: `TOOLS[]` (`src/mcp/mcp_tools.c:3548`)
   maps thirty-seven names (`tests/test_plugin.c:414` pins the count), none to any
   orchestration method, and `method_job_submit`'s own comment says so ("There is no
   MCP tool and no gateway route that reaches this method"). `/mcp`
   (`gateway.c:1378-1448`) authenticates the bearer, builds an in-process
   `atlas_mcp_server` with `remote = true` and `granted = pr->scopes` (`mcp_exchange`,
   `:698-736`), and the tool listing and every `tools/call` are gated on
   `atlas_scope_has(s->granted, TOOLS[i].scope)` (`mcp_tools.c:3839`, `:4027`). The
   MCP server never sees the token: `authenticate` wipes it (`:281-395`), and
   `mcp_exchange` receives the principal only. The audit row for every `/mcp` request
   is `interface = REMOTE_MCP, operation = "mcp"` (`:1440-1446`) — the tool name is
   not recorded.
8. **A16 as it stands in the tree, task by task.** T1 (vocabularies, `--no-scopes`),
   T2 (migration 31, `f77fc67`) and T3 (the write point, `13b45f3`, `bb37e0c`) are
   committed; `src/decision/remote.c` and `include/atlas/decision_remote.h` exist and
   are the in-transaction verifier this plan's Decision 2 mirrors. T4 is in the working
   tree uncommitted: `atlas_gwpolicy` gains `remote_dispose_key`,
   `remote_dispose_kinds`, `cleartext_disposal_accepted`; `gwpolicy.c` gains
   `parse_dispose_key` and the three keys' branches; `atlas_service_gateway_status_for`
   is declared. T5 (`src/ipc/server_remote.c`, `atlas_daemon_opts.gwpolicy_text`,
   `tests/tools/atlas_gw_daemon.c`), T6 (`API_WRITE_ROUTES[]`, `api_handle_write`,
   `ATLAS_GW_WRITE_BODY_MAX_BYTES`, `/auth/me` fields), T7 (the disposal panel), T8,
   T9 and T10 are **not in the tree**. §What A16 leaves says which of those this plan
   depends on and how.
9. **The bounds this season leans on.** `ATLAS_ORCH_TASK_MAX` 65536
   (`include/atlas/orch.h:256`); `ATLAS_ORCH_MAX_VALIDATIONS` 8 (`:266`);
   `ATLAS_ORCH_NAME_MAX` 64 (`:271`), which also bounds the idempotency key and the
   correlation (`orch.c:607-619`, `is_name`); `ATLAS_ORCH_MAX_ATTEMPTS` 5 (`:278`);
   `ATLAS_ORCH_MAX_WALL_TIMEOUT_MS` 3600000 (`:276`); `ATLAS_ORCH_SPEC_DOMAIN`
   `"atlas.orch.spec.v1"` (`:62`); `ATLAS_GW_MAX_BODY_BYTES` 1 MiB
   (`include/atlas/limits.h:973`); `ATLAS_GW_DEFAULT_RATE_PER_MINUTE` 600 (`:999`);
   `ATLAS_APIKEY_SELECTOR_HEX` 16 and `ATLAS_APIKEY_TOKEN_MAX` 80
   (`include/atlas/apikey.h:67-75`); `ATLAS_GWPOLICY_MAX_ORIGINS` 8 and
   `GWPOLICY_MAX_BYTES` 8192 (`gwpolicy.h:70`, `gwpolicy.c:28`);
   `ATLAS_ORCHPOLICY_MAX_SUBMITTERS` 16 (`orchpolicy.h:63`).
10. **What a job costs, and what Atlas records about it.** `orch_usage` (migration 22)
    stores the provider-reported `total_cost_usd` as integer micro-USD per attempt,
    never estimated (`include/atlas/orch_usage.h:1-40`, "Atlas never estimates a cost
    from token counts"), and `atlas_db_orch_job_usage` (`include/atlas/orch_ops.h`,
    A12.0) reads one job's total back with `has_cost` kept separate from zero.
    A10.1's experiment (`docs/roadmap.md:970-985`) measured **$10.253520 for two worker
    starts** totalling 870 301 ms — $5.13 and 435 s per start, observed, on a small
    real task, one model. That is the only cost figure this repository has measured
    and it is used in §Worst-case cost as an observation, never as a bound.
11. **The ledger.** `orch_transitions` (`migrate.c:1751-1769`) records every state
    change with `actor IN ('CLIENT','DISPATCHER','ATLAS')`, `actor_uid`, a closed
    `reason` vocabulary and an Atlas-written `detail`; it is append-only and its
    AUTOINCREMENT id is the ordering authority. A submission writes one row with
    `actor = CLIENT, actor_uid = <peer>`. Decision 3 puts the credential beside the
    uid rather than inventing a fourth actor.
12. **The test channels that exist.** `tests/test_plan_rpc.c:329-395` builds an
    in-process *edge*: a real writer thread, an `atlas_server_ctx` whose
    `orchpolicy` the test fills through `atlas_orchpolicy_parse_bytes` and then marks
    ENABLED itself (`edge_policy`, `:309-327`, with the argument for why that is
    honest), and `atlas_server_dispatch(&ctx, payload, len, uid, pid, …)` called with
    **a uid the test chooses** (`:386`). That is the shape every daemon-method test in
    this plan uses: `ctx.gwpolicy` is filled the same way, `gateway_uid` is set to a
    uid the test then dispatches as, and no socket, fork or root-owned file is
    involved. The HTTP half needs a forked daemon on a real socket with *both*
    policies injected, which is A16 T5's `atlas-gw-daemon` extended by one argument
    (Decision 15). `tests/test_a7_authority.c:250-268` states the limit both channels
    share: the operator group is locked for every peer of a daemon running from a
    replaceable binary, so **`atlas_server_peer_is_operator` is false in every
    fixture**, and Decision 13's operator-visibility half is provable only at the
    write point in process and on the deployment.

## What A16 leaves, and what this plan assumes — a dependency is not an assumption

| A14 needs | Built by | Mechanism, named | Status at `c06e0a8` |
| --- | --- | --- | --- |
| a bearer-only route table with its own handler, form bodies, and the 409 mapping | A16 T6 | `API_WRITE_ROUTES[]`, `api_handle_write`, `build_api_params` over the body, `ATLAS_ERR_INTEGRITY → 409` | **not in tree** — dependency |
| a daemon method group offered to the gateway uid under policy, with the pattern "each method asks the predicate again" | A16 T5 | `atlas_server_remote_disposal_offered`, `src/ipc/server_remote.c` | **not in tree** — dependency; A14 adds a sibling file, never a row in A16's table |
| the daemon verifying a bearer inside the transaction that spends it | A16 T3 | `atlas_decision_remote_verify` (`src/decision/remote.c`) | **in tree** (`13b45f3`) — precedent, mirrored not shared (Decision 2) |
| a gateway-policy key naming a credential, with `key_` grammar and MALFORMED rules | A16 T4 | `parse_dispose_key`, `remote_dispose_key` | **uncommitted in working tree** — A14 generalises the parser (T4) |
| a written cleartext acceptance key, printed by `gateway status`, reported by `/auth/me` | A16 T4/T6 | `operator_accepts_cleartext_disposal`, `cleartext_disposal_accepted` | T4 uncommitted, T6 not in tree — A14 adds a *second* key of the same grammar (Decision 11) |
| a way to hand a fixture daemon a gateway policy | A16 T5 | `atlas_daemon_opts.gwpolicy_text`, `tests/tools/atlas_gw_daemon.c` | **not in tree** — dependency, extended with `orchpolicy_text` (Decision 15) |
| a scope minted by no one (`--no-scopes`) and a refusal by name | A16 T1 | `atlas api-key create --no-scopes`; the `decisions:dispose` refusal | **in tree** — reused; one sentence amended (T1) |
| a panel that holds a key in tab memory and POSTs with `credentials: "omit"` | A16 T7 | `disposeKey`, `apiWrite` | **not in tree** — A14's Jobs view reuses `apiWrite` and adds `submitKey` |
| the write-table property test | A16 T6 | `test_every_write_route_is_a_disposal_on_the_reviewed_allowlist` | **not in tree** — A14 widens it (T9); its "every row is DECISIONS_DISPOSE" assertion becomes "every row's scope is ungrantable and on the positive list" |
| migration number | A16 T2 | 31 | **in tree** — A14 is 32, and holds only if A16 adds none after 31 (its plan adds none) |

**What A16 does not leave, and this plan builds:** the scope, the group, the column,
the budgets, the policy's execution keys, the MCP tools, the Jobs view, the operator's
terminal view, the cleartext acceptance for submission, and the argument for each.

**Sequencing.** T1 of this plan is dispatched after A16's last commit and after the
three operator questions are answered. Tasks T1–T4 depend on nothing A16 has not
landed; T5 needs A16 T5; T6 needs A16 T6; T8 needs A16 T7; T9 needs A16 T8.

## The decision on cleartext: submission does not inherit the disposal acceptance, and the operator is asked again with a different chain

The brief asked this plan to decide whether submission accepts the same cleartext
channel the operator accepted for disposal on 2026-09-04, and to argue it. Decided:
**no — not the same key, not the same acceptance, and not the same chain.** Submission
gets its own root-owned key, `operator_accepts_cleartext_submission = yes`, with the
same grammar as A16's (`yes` only; refused under `tls_mode = REVERSE_PROXY`; refused
without the keys it applies to) and a chain of its own that every document carries
verbatim. The reasons, each sufficient:

1. **The consequence is a different kind.** A captured disposal credential moves a
   lifecycle state in the one record Atlas cannot rebuild — a harm to *truth*, durable
   and auditable, reversible by a superseding revision even if the row is history for
   ever. A captured submission credential starts a process: on this deployment a
   Claude Code worker running as the operator's own account, in the operator's HOME,
   with a prompt the holder wrote, whose reads enter the model provider's transcript
   under the operator's login and whose edits land in a workspace the operator owns.
   That is a harm to the *machine and the account*, bounded by the policy's driver,
   gates, attempts and daily count, and by nothing in Atlas about what the prompt may
   ask a worker to read. An acceptance written with the first chain in front of a
   person does not cover the second, and reusing the key would let a `yes` given for
   one consequence silently authorise another — which is exactly the shape the brief
   forbids for the scope, one file over.
2. **The two credentials do not even travel the same way.** The disposal credential is
   presented by a browser on the LAN and crosses the segment in the clear (§What
   exists, 5). The submission credential the operator actually asked about — the
   tunnel's — is presented by a process on this host to this host's own address and
   never crosses the segment at all; its exposure is a `0600` file in the operator's
   HOME, which is readable by exactly the principal a remotely submitted worker runs
   as. A browser submission key crosses the segment like the disposal key does. One
   acceptance sentence cannot be true of both paths; the chain in §Frozen formats says
   both, and the acceptance key is what the operator writes having read both.
3. **The bound is different and it is money.** A disposal is free; a submission spends
   the operator's model budget and the daily bound caps it. The acceptance for
   submission is therefore read beside a number — `remote_submit_max_per_day` — and
   the two belong in the same policy block so an auditor sees the cost with the
   acceptance.
4. **The operator's "no" must buy something nameable.** If the operator declines to
   accept cleartext submission, the season does not stop: the terminator task A16
   removed is recoverable from `c305f40` in full (nginx on `192.168.0.198:8799`, the
   gateway on loopback under `tls_mode = REVERSE_PROXY`, the tunnel re-pointed), and
   it returns as this plan's T11a before the live acceptance. Under `REVERSE_PROXY`
   the submission group is offered with no acceptance key, exactly as A16's disposal
   group is.

**Stated as it will be true on this deployment if the operator answers yes:** a
browser submission key crosses `192.168.0.198`'s network segment in the clear on every
request; anyone able to observe traffic on that segment holds it after the first
request they see and holds it until `atlas api-key revoke`; and a holder submits work
that runs as the operator, within the policy's daily bound, until then. The tunnel
key's exposure is not the wire; it is the file, and the worker that can read the file.
Atlas states both and recommends neither.

## The decision on the executing shape: one mechanism, two drivers, and the operator picks the word

The roadmap wrote three shapes down and said the third "may not be what the operator
wants, which is the point of writing it down as a choice rather than picking it here".
This plan finds that shapes 1 and 3 are **the same mechanism with one different word in
the policy**, and that shape 2 buys nothing shape 1 does not already have once the
daemon verifies the credential itself:

- **Shape 1, done as A16 does it.** The gateway forwards the presented bearer; the
  daemon verifies it inside the transaction that queues the job; the row records the
  kernel's peer uid *and* the verified credential. The gateway asserts nothing — it
  carries. Without a request in flight that holds a named credential, the gateway's
  uid reaches `unknown method` for every remote name, which is what it reaches today.
  The roadmap's objection to shape 1 — "the gateway now asserts an identity instead
  of having one" — is answered by the verification being the daemon's and in the
  transaction, which is A16's Decision 1 restated for a job.
- **Shape 2 (a broker uid) is rejected**, with its chain: a third principal would
  hold the submitter right and apply the per-key policy; but the per-key policy is a
  root-owned file the daemon already holds in memory, the credential check is one
  the daemon already performs for `gateway.auth`, and the broker would still have to
  receive the bearer from the gateway — so it would verify exactly what the daemon
  verifies, one process and one socket later, and its only distinct property would
  be a uid the orchestration policy lists as a submitter, which is the tempting fix
  wearing a different account. It costs a process, a unit, a socket and a policy
  section and closes no chain the daemon's own verification leaves open.
- **Shape 3 (a proposal, not a job) is what `remote_submit_driver = claude-repo`
  produces.** A repo-tree driver is never on a background dispatcher's filter and
  never granted to an unfiltered lease (§What exists, 3), so a remotely submitted
  `claude-repo` task is a QUEUED row nobody but the operator's own foreground
  `atlas job run --resume RUN` can start. Nothing remote ever starts work; "an
  operator was in the loop" is literally true; a cancel is the rejection and the
  driver is the disposal; and it costs no new approval machinery at all. Its cost is
  A11.1's pin: the task is pinned to `scanned_head` at submission
  (`server_orch.c:210-243`), and a moved HEAD is refused by the driver before it
  starts anything (`rundriver.c` step 3), so a queued remote task is drivable only
  until the operator's next commit on that repository and must otherwise be
  resubmitted.
- **`remote_submit_driver = claude` is the unattended shape.** The model dispatcher —
  installed as the operator's user unit in T11 — leases it, runs a worker in a
  workspace as the operator, runs the gate floor there, and stores a patch artifact
  the operator reads on the machine. Money is spent on the credential's word within
  the policy's bounds.

Both drivers run through identical code in this plan. The operator chooses the word
(§Decisions the operator must be asked, row 1), and the choice decides only T11's
deployment steps and the number in §Worst-case cost. The plan's *default assumption*
is `claude`, because "give Atlas a task" was the ask and the deferred shape is the
answer to a question — "should a model be able to start work while I am away?" — that
only the operator can answer in plain words.

## The seventeen decisions this plan settles, each with its argument

### Decision 1 — A new ungrantable scope, `jobs:submit`, derived for the credentials root-owned lines name; A16's disposal scope is never widened to cover it

`jobs:submit` joins `SCOPES[]` (`src/gw/apikey.c:22-37`) with `grantable = false`,
beside `memory:write` (A9's precedent: in the vocabulary, not grantable) and
`decisions:dispose` (A16's: not grantable, derived). `atlas api-key create` refuses it
by name; no `api_keys` row ever holds it; `method_gateway_auth`
(`src/ipc/server_gw.c:80-135`) appends it to the returned scope list for exactly a key
whose id is on the policy's `remote_submit_key` list, and only when the group is
offered (Decision 8's three conditions); and the write point derives it again inside
the transaction for the same key (Decision 2). The name says what it grants — *submit*
— and nothing about running, applying, accepting or settling, none of which exist for
any credential.

**Why the disposal scope must never be widened to cover submission**, stated here
where the scope is introduced because the brief asked for it here. `decisions:dispose`
is derived only for a key holding *no* stored scope (A16 Decision 2), so a disposal
credential is structurally one nothing else can use; a submission credential must be
usable by a model that reads before it writes, so it *may* hold stored read scopes
(below). Widening the disposal scope to "dispose or submit" would make one credential
buy two unrelated powers — moving the canonical ledger, and starting a process as the
operator — so that one capture on the wire, or one prompt injection against a model
holding it, exercises both; the audit trail could not say which power a holder used
for which purpose, because the scope name would name both; and the root-owned line the
operator wrote on 2026-09-04 as `remote_dispose_key`, having accepted a *cleartext
disposal* channel, would silently have become a submission line under an acceptance
they never gave for that consequence. The two scopes, the two policy lines and the two
acceptance keys are therefore separate, and **a policy naming the same key id in both
`remote_dispose_key` and any `remote_submit_key` line is MALFORMED** (T4), which is
what makes "one credential, one power" a property of the parser rather than a
sentence.

**A submission credential may hold stored read scopes, and that is a deliberate
difference from disposal with its cost stated.** The operator's ask is that the model
they already talk to can hand Atlas a task; that model holds `key_b2578f48143c06d3`
with five read scopes and must keep them to describe work it has read. Requiring an
empty scope list would make the feature unusable for the one client it was asked for.
The cost: a captured or injected credential that reads *and* submits buys both
together. The bound on that is not the credential's shape but the policy's — driver,
mode, gate floor, attempts, active count, daily count — every one of which the request
cannot name (Decision 4), plus the absent verbs: a queued job applies, commits,
accepts and settles nothing whoever queued it.

### Decision 2 — The daemon verifies the credential itself on every remote job method; for the two that write, inside the transaction that writes; `src/orch/remote.c` is a third copy of one check, and the reason it is not shared is written down

A16 established that a key id the gateway merely claims is worthless
(`principal.key_id`, `gateway.c:254-270`), and that the only implementation possible
is the gateway forwarding the presented bearer and the daemon verifying it against the
key table as it stands at that moment. This season applies that rule to every remote
job method, not only the writes: `job.remote_submit` and `job.remote_cancel` carry the
token onto the `atlas_orch_op` (`remote_token`) and `op_submit` / `op_cancel` call
`atlas_orch_remote_verify` on the writer's own handle inside the transaction, so a key
revoked between the gateway's `gateway.auth` and the writer's turn queues nothing and
cancels nothing; `job.remote_get` and `job.remote_list` call the same function on the
request's read handle before scoping the read by the verified key id, so a claimed
key id never selects rows.

`atlas_orch_remote_verify(db, token, allowed_ids, allowed_count, key_id_out)` is the
same sequence `gateway.auth` and `atlas_decision_remote_verify` run — parse, indexed
lookup, ACTIVE, readable scopes, constant-time verify — with two differences that are
the whole reason it is a separate function: it does **not** require an empty stored
scope list (Decision 1), and it checks membership in a *list* of up to four ids
rather than equality with one. Every failure up to "did not authenticate" produces one
outward sentence; only "not one the policy names" is distinguished, because it is a
policy fact about a credential Atlas has just proven real. A16's header argued that
two callers with two concurrency contracts must not share one function; this is a
third caller with a third contract (a read handle outside any transaction, *and* the
writer's handle inside one), and the argument is the same. The cost is stated:
**three copies of one credential check now exist** (`server_gw.c`,
`src/decision/remote.c`, `src/orch/remote.c`), each with its own reason, and a quiet
season may fold the parse-lookup-verify core into one helper in `src/gw` that all three
call with their own policy checks around it — recorded in `docs/backlog.md` by T10,
not done here.

### Decision 3 — `submitter_uid` stays the kernel's answer; the credential is a new column beside it on the job and on the ledger; the digest does not move

`submitter_uid` on a remote job is `ds->peer_uid`, which is the gateway's uid (992
here). That is literally true — the peer that submitted *was* the gateway — and it
keeps every existing use of the column honest: the schema comment ("From
`SO_PEERCRED` at submission. Never from the request body"), the validator's
non-zero check, the digest's "two principals sending identical text are not making
the same request", and `spawn_follow_up`'s inheritance. Recording the operator's uid
instead would be a claimed value; recording the credential's *owner* would require a
column that does not exist and a claim nobody can verify.

**Which credential** is a new column, `orch_jobs.submit_key_id TEXT NOT NULL DEFAULT
''`, and the same on `orch_transitions` so the ledger row that records the submission
names it — A16 Decision 4's reason one layer over: a job row is not prunable but the
ledger is canonical, and "which credential queued this" must be readable from the row
that is the ordering authority. Migration 32 is two `ALTER TABLE … ADD COLUMN` and one
index; no CHECK moves, no table is rebuilt, and **the empty default is a true
statement about every existing row** — every job before this migration was submitted
through the only channel that existed — so it is not migration 19's mistake.
`orch_transitions.actor` stays `CLIENT` for a remote submission: the row already
carries `actor_uid = 992`, which distinguishes it from every operator submission
without widening a CHECK, and the `detail` sentence names the credential (§Frozen
formats) so a reader who ignores the new column still reads the row correctly.

The credential travels on `atlas_orch_op` (`remote_token`, `remote_key_id`,
`remote_allowed_ids[]`, `remote_max_active`, `remote_max_per_day`), **never on
`atlas_orch_spec`**, exactly as A10.1's memory mode and A11.6's `max_parallel` do, so
`ATLAS_ORCH_SPEC_DOMAIN` does not move and no stored `spec_digest` means anything
different than it did. Two credentials submitting identical text through one gateway
therefore produce one digest; what keeps them apart is the idempotency namespace
(Decision 6) and the column.

### Decision 4 — A remote request names the repository, the task and an idempotency key; the policy names everything else, and a request that names anything else is refused

`job.submit` accepts `mode`, `driver`, `validation`, `parallel`, `memory`, `parent`,
`allowed_path`, `correlation` and six bounds (`method_job_submit`,
`server_orch.c:157-433`). `job.remote_submit` accepts `repo`, `task`, `key` and
`token`, and **refuses** a request carrying any other name with the sentence in
§Frozen formats — not ignores, because a caller who believes they configured a driver
and silently got the policy's is worse off than one who was told (A7.1's rule about
policy keys, applied to a request). The reason is A12.0's: *the operator brings the
gate floor; a model choosing its own verification would be acceptance on the model's
word.* Extended to the whole submission: a driver chosen by a request is a request
choosing which model runs and where; a mode chosen by a request is a request choosing
what a worker may touch; an attempt count is a request choosing what it costs; a
parent is a request joining a run it did not create. So `remote_submit_driver`,
`remote_submit_mode`, `remote_submit_gate` (one or more, the floor), and
`remote_submit_max_attempts` are root-owned lines, copied onto the op by the method
from `ctx->gwpolicy`, and cross-checked at submit against the orchestration policy the
daemon holds in the same process: the driver and the mode must be ones
`orchestration.conf` lists, and a driver that needs a live model requires
`live_model = on` — each refused with a sentence naming the file that would change
it; the attempts bound is placed on the spec and refused above the ceiling by
`atlas_orchpolicy_apply_limits` (`src/orch/policy.c:522-558`) with the sentence it
already has, because a second spelling of one refusal would be two rules. The daemon logs one line at start if
the two policies disagree, so an operator who edits one and not the other finds out
before the first submission does.

**A remote submission never adds a gate.** A12.0 let a planner add gates to the floor
because a plan's stages are compiled from a document the operator can read before any
run; a remote request is a single call with no such reading. Additions are a later
season's question with the allowlist argument already made at
`take_gate_floor` (`server_orch.c:1015`); this season keeps the smaller surface.

**`memory` is OFF, `parallel` is 1 and `parent` is empty for every remote
submission**, and there is no key to change any of them: bounded memory is a
per-run experiment an operator turns on (A10.1), parallelism is a resource question
the operator answers per run (A11.6), and a remote task that could join an existing run
could join one whose gates it did not see.

### Decision 5 — Two budgets per credential, checked in the submit transaction, refused never clamped; a follow-up inherits the credential and counts toward one budget and not the other

The roadmap's first "must be true": *a budget per key*. Two, because they bound
different things:

- **`remote_submit_max_active`** (1..8): how many non-terminal jobs may carry this
  credential at once. Counted in `op_submit` from `orch_jobs` where `submit_key_id =
  ?1 AND state NOT IN (<the M24 terminal set>)`, with the literal the two A11.6
  indexes use, so `tests/test_orch_run.c:589-627`'s agreement test gains this third
  SQL spelling. It bounds queue depth — what a burst can commit before anything runs —
  and it is time-free.
- **`remote_submit_max_per_day`** (1..64): how many *root submissions* may carry this
  credential in one UTC calendar day. Counted from `orch_jobs` where `submit_key_id =
  ?1 AND parent_job_uid = '' AND created_at >= ?2`, `?2` being the current UTC date
  followed by `T00:00:00Z` — `created_at` is `atlas_now_iso8601` text, so the
  comparison is lexicographic and needs no parsing. It bounds money. It is keyed on a
  timestamp, which this project's ordering rule forbids for *ordering*; this is a
  *count*, and the cost is stated: a clock stepped backwards admits more jobs into
  "today", and a clock stepped forward resets the count. A bound on a well-behaved
  clock is the bound Atlas has; the ledger id cannot say what day it is.

Both are refused with the count and the bound named, in the transaction, **after** the
idempotency lookup — a duplicate of an existing job is not a new submission and must
not spend budget, so the budget checks run only when a row would be created. The
credential is verified **before** the lookup, not after it: the idempotency key is
namespaced by the verified id (Decision 6) and the digest covers the key
(`db_orch.c:1147`, `orch.c` feeds `idempotency_key`), so nothing about the row can be
computed until the credential has been proven; and a wrong credential presenting a
colliding key must never be handed the existing job. The frozen order in `op_submit`
for a remote op is therefore: verify → build the namespaced key into the spec →
the existing shape checks → digest → idempotency lookup → budgets (new rows only) →
insert. (A11.6 orders its shape checks before idempotency because a malformed
submission is malformed whether or not a key resolves it; a budget is not a shape,
and a credential is what decides whose key it is.)

**A follow-up task** (`spawn_follow_up`, `db_orch.c:2419-2445`; only a repo-tree chain
produces one, so only the deferred shape reaches this) inherits `submit_key_id` with
`submitter_uid`, so it stays visible to the credential and to the operator and counts
toward the active bound (it is a non-terminal row). It does **not** count toward the
daily bound (`parent_job_uid` is not empty), because the chain's worker starts are
already bounded by `ATLAS_ORCH_RUN_MAX_WORKER_STARTS` and a bound on a caller must not
double-count Atlas' own retries as the caller's submissions.

### Decision 6 — The idempotency key is namespaced by the credential, by one builder

`orch_idempotency` is keyed on `(submitter_uid, key)` (`migrate.c:1826-1831`), and every
remote submission's `submitter_uid` is the gateway's, so two credentials choosing the
same key would collide. Rather than widen the primary key, **the write point** derives
the stored key: `remote.<key_id>.<client key>`, built by exactly one function
(`atlas_orch_remote_idempotency_key`, A12.0's "one string from one builder" rule),
called from `op_submit` with the id `atlas_orch_remote_verify` has just established
and never from the method — a method building a namespace from the principal the
gateway forwarded would be building it from a claimed id. The method carries the
client's part on the op (`remote_client_key`) and leaves `spec.idempotency_key`
empty; the write point fills it, then digests. The client's part is bounded at **40**
bytes so the whole fits `ATLAS_ORCH_NAME_MAX` (64) under `is_name`: `remote.` is 7,
the id is 16, the dot is 1, and 64 − 24 = 40; the builder's own check on
`[a-z0-9._-]{1,40}` is the guard, and the assembled key satisfies `is_name` by
construction. A client key outside that shape is refused with the sentence in
§Frozen formats. A remote submission with no `key` carries no idempotency at all,
which is the local default too; the MCP tool's description tells a model to pass one
so a retried call resolves to the job it already made.

### Decision 7 — No remote read of a worker's output: no artifact route, no log, no `job.remote_artifact`; and the chain that is not closed by that is written down

`job.artifact` returns an artifact's bytes inline when the worker sent them
(`server_orch.c:692-743`). A remote artifact read would turn submission into
something else. The chain, written out: a task text is a prompt; the worker runs as the
operator with the operator's read authority and `Read` is a tool `acceptEdits` does
not gate; a prompt can say *write the contents of any file this account can read into
a file in the workspace*; the patch carries it; a remote artifact route returns it to
the credential holder. That is "read any file uid 1000 can read" — a capability no
read scope grants and this season must not grant under the name *submit*. So the group
has no artifact method and no log method, and `job.remote_get` returns state, reason,
attempts, timestamps, the driver, the pinned commit, the credential and what the
attempts *cost*, never what they *produced*. A finished job's patch is read on the
Atlas machine with `atlas job artifact` — by the operator, whose uid the daemon
already trusts.

**What this does not close, said here so nobody reads the sentence above as a
guarantee.** The same prompt's `Read` output enters the model provider's transcript
under the operator's account before any artifact exists, and a worker that could reach
the network (a `Bash` pattern the operator allowed, a tool the permission mode admits)
could send it there itself. Atlas neither reads the operator's Claude settings nor
bounds the worker's tools; the bound on this chain is the operator's own configuration
and the model's own conduct, neither of which is an Atlas guarantee. The honest
paragraph carries this, and so does the Jobs view.

### Decision 8 — The daemon group lives in its own file, is offered only under three conditions, and answers `unknown method` otherwise; the gateway refuses with a sentence first

`REMOTE_SUBMIT_METHODS[]` — `job.remote_submit`, `job.remote_get`, `job.remote_list`,
`job.remote_cancel` — lives in `src/ipc/server_orch_remote.c`, beside A16's
`server_remote.c` rather than inside `server_orch.c`, so the A8 comment at
`server_orch.c:1-34` ("What neither group can do", "Why a dispatcher tier is not a
privileged tier") stays readable as written and gains one paragraph pointing at the
new file. `dispatch()` consults it additively after the gateway group, when
`atlas_server_remote_submit_offered(ctx, peer_uid)`: the gateway peer predicate
(`atlas_server_peer_is_gateway`, `server_gw.c:348-382`), **and**
`ctx->gwpolicy.remote_submit_count > 0`, **and** (`ctx->gwpolicy.tls_mode ==
ATLAS_GWPOLICY_TLS_REVERSE_PROXY` **or** `ctx->gwpolicy.cleartext_submission_accepted`).
Every other peer, and this peer under any other policy, gets `unknown method`. Each
method asks the predicate again for itself (A8's rule that routing is not
authorisation), then `require_remote_submitter`, which is: the orchestration policy is
ENABLED (else `orch_disabled`'s sentence — the group is *offered* by the gateway policy
and *refuses* by the orchestration one, so an operator can tell the two files apart),
and the token verifies against the list (Decision 2). **`require_submitter` is never
called on this path**, and the orchestration policy's `submitter_uid` lines are
never consulted for it — which is the sentence the whole season turns on, and T5's
test asserts it by dispatching as the gateway uid against a policy that lists only
another uid as a submitter.

The gateway, holding its own copy of the same policy, answers `404 not_found` *this
gateway does not serve remote submission* before forwarding when the list is empty —
A16's shape from `/mcp`'s `remote_mcp` refusal — so an operator debugging the panel
gets a sentence and the daemon's silence stays the guard.

### Decision 9 — Four rows in A16's bearer-only table, every one a POST with a form body, with a per-row body bound the disposal rows did not need

The remote job routes resolve a principal from the bearer and from nothing else — a
session cookie or the anonymous floor must not submit — and forward the bearer to the
daemon. The read table (`API_ROUTES[]`) does neither: it accepts three principal
sources and forwards no token. So all four routes, **including the two reads**, are
rows in A16's `API_WRITE_ROUTES[]` under its handler, which already does exactly this;
that table's name is a misnomer for a read row and its comment is amended by T6 to
say what the table really is — *routes whose only principal is the bearer on the
request, forwarded to a daemon method that verifies it* — with the two reads named as
the reason. A `GET` with a bearer would work in principle; keeping every
bearer-forwarding route under one handler with one property test is worth more than
the verb.

A16 froze `ATLAS_GW_WRITE_BODY_MAX_BYTES` at 4096, which a task text cannot fit:
`ATLAS_ORCH_TASK_MAX` is 65536 and form-encoding triples a byte in the worst case. So
`api_route` gains a trailing `size_t body_max`; the two A16 rows carry
`ATLAS_GW_WRITE_BODY_MAX_BYTES`; the submit row carries
`ATLAS_GW_SUBMIT_BODY_MAX_BYTES` (262144 — three times the task bound plus the other
fields, well under `ATLAS_GW_MAX_BODY_BYTES`); the other three A14 rows carry 4096; a
read row leaves it zero and `api_handle` never reads it. The 413 check reads the row's
bound. The 404 step ("does this gateway serve this route family?") becomes
`route_offered(g, route)`, a switch on the row's scope with no `default:`:
`DECISIONS_DISPOSE` asks A16's condition, `JOBS_SUBMIT` asks
`remote_submit_count > 0`. The property test (T9) asserts every row's scope is one of
exactly those two, ungrantable, its method on a positive list of six names, and its
body bound non-zero.

### Decision 10 — Four MCP tools, remote-only, whose run functions forward the request's own bearer; absent from the stdio adapter; the count moves from 37 to 41

The operator's ask is that the external model can hand Atlas a task; that model speaks
MCP and nothing else. `TOOLS[]` (`mcp_tools.c:3548`) is shared between the stdio
adapter and `/mcp`, gated per tool by scope on the remote path only. Four tools —
`atlas_job_submit`, `atlas_job_status`, `atlas_job_list`, `atlas_job_cancel` — carry
scope `ATLAS_SCOPE_JOBS_SUBMIT` and a new `remote_only = true` member of `tool_def`,
whose meaning is: not listed by `atlas_mcp_write_tool_list` when `s == NULL || !s->remote`,
and `unknown tool` from `atlas_mcp_call_tool` on the stdio adapter — *absent* there,
not refused, because a local Claude session has a shell and `atlas job submit`, and a
tool that reached `job.submit` as the operator's uid from inside a model session would
be a second submit surface with no gate floor. The tool schemas declare `repo`, `task`,
`key` (submit), `job` (status, cancel), `after`/`limit` (list) and **no `token`**
— `"token":` is a forbidden schema property (`test_decision_mcp.c:132-134`) and stays
one. The bearer reaches the daemon because `mcp_exchange` (`gateway.c:698-736`) gains
a parameter: the `/mcp` handler parses the request's `Authorization` once more into a
stack buffer after `authenticate` has wiped its own, hands it to `mcp_exchange`, which
sets `s->remote_token` for the exchange and wipes it at teardown; the four run
functions append it as the daemon's `token` parameter, and nothing else reads the
field. `writes = true` on the submit and cancel tools, and the `tool_def` comment at
`mcp_tools.c:63-73` — "every tool that `writes` maps to `ATLAS_SCOPE_MEMORY_WRITE`" —
is amended: every tool that writes maps to a scope no credential can be granted, and
since A14 there are two such scopes, one absent from every mask and one derived by the
daemon for named keys.

The tool count pinned at `tests/test_plugin.c:414` becomes 41 with a paragraph in the
comment above it; `tests/test_mcp.c:250`'s comparison of `atlas_mcp_tool_names()`
against the stdio listing learns `atlas_mcp_tool_remote_only(name)` and requires each
remote-only name *absent* from that listing; `tests/test_gw_remote.c:279`'s
`test_no_credential_can_reach_a_write_tool` gains the four names for a credential
holding every grantable scope, and a new case shows a credential the policy names
reaches them. Every remote `tools/call` still audits as `operation = "mcp"`
(`gateway.c:1440-1446`); the submission is named by the daemon's own log line and by
the job row, and this is stated as a cost in T10 rather than fixed by threading a tool
name back through `mcp_exchange`.

### Decision 11 — A second written acceptance, `operator_accepts_cleartext_submission = yes`, with its own chain; the disposal acceptance implies nothing about it

§The decision on cleartext carries the argument. The key's grammar is A16's: `yes` is
the only value; MALFORMED under `tls_mode = REVERSE_PROXY`; MALFORMED without any
`remote_submit_key`; never a default. The gate is Decision 8's third condition. `atlas
gateway status` prints it on every run beside the `submit:` line; `/auth/me` reports
it so the Jobs view shows the chain at the moment of use; every document carries the
chain verbatim (§Frozen formats, "The cleartext chain for submission"), including the
sentence that the tunnel's credential does not cross the segment and the file it lives
in is readable by the worker.

### Decision 12 — Seven keys in `/etc/atlas/gateway.conf`, refused rather than clamped, cross-checked against the orchestration policy in the daemon

The keys are `remote_submit_key` (repeatable, at most `ATLAS_GWPOLICY_MAX_SUBMIT_KEYS`
= 4), `remote_submit_driver`, `remote_submit_mode`, `remote_submit_gate` (repeatable,
at most `ATLAS_ORCH_MAX_VALIDATIONS` = 8, at least 1), `remote_submit_max_attempts`,
`remote_submit_max_active`, `remote_submit_max_per_day`, and the acceptance key. They
go in the gateway policy and not in `orchestration.conf` for A16 Decision 6's three
reasons, each sufficient here too: the file's stated purpose is to constrain the
gateway's principal and a credential *is* that principal; the daemon already loads it
(`src/daemon/daemon.c:224`) beside the orchestration policy (`:212`), so both are in
one process and one submit can consult both; and a malformed `orchestration.conf`
disables **all** orchestration — the operator's own local jobs included — while a
malformed gateway policy disables the gateway and nothing else, which is the right
blast radius for a typo in a remote line. The cost: the gateway policy now names a
driver, a mode and gate lines the *orchestration* policy is the authority on, and the
two can disagree. The daemon cross-checks at every submit (Decision 4) and logs at
start; `atlas gateway status` prints what the gateway policy says and cannot know what
the orchestration policy says, and its `submit:` line says so in four words
(`(checked at submit)`).

The gate lines are stored as bounded text (`ATLAS_GWPOLICY_GATE_LINE_MAX` = 256
printable ASCII bytes, first token a bare name without `/`) and split at submit by
`atlas_orch_gate_split` with every argument pushed through `atlas_orch_argv_push`,
exactly as `take_gate_floor` does for a plan's floor — one splitter, one strictness —
so `gwpolicy.c` gains no dependency on `atlas/orch.h`. The program allowlist is applied
by the executor as for every gate. A gate line the splitter refuses at submit is a
policy the loader accepted, which is the one place this season's parse and its use can
disagree; the daemon's start-up log line splits every gate once and names the first
one it cannot, so the disagreement is found at restart rather than at the first
submission.

### Decision 13 — The operator's own terminal sees, cancels and drives remote jobs; the write point learns that the peer is the operator from the method, never from the request

Remote jobs spend the operator's money and run as the operator's account; `atlas job
list` not showing them would be a trap, and cancelling a runaway one from the terminal
is the path that must work when the gateway is down. The operator account is trusted
by design (A7.1), and `CLAUDE.md` says never to write a test asserting it cannot do
something. So `job.get`, `job.list`, `job.artifact`, `job.cancel` and `job.run_status`
widen their scope by one disjunct: a job whose `submit_key_id` is non-empty is visible
to a peer for which `atlas_server_peer_is_operator(peer_uid)` (`server_internal.h:178`)
is true. `job.list` gains a boolean `remote` parameter (`atlas job list --remote`)
that lists exactly those rows, so the operator's own list is unchanged by default.
`op_cancel`'s ownership check (`db_orch.c:1435`) gains `|| (op->peer_is_operator &&
j.submit_key_id[0] != '\0')`, where `peer_is_operator` is a new `atlas_orch_op` member
the method sets from the same kernel-derived predicate — a flag set from
`SO_PEERCRED`, on the model of `peer_uid`, never from a parameter. **This half is
provable only at the write point in process and on the deployment**: every fixture
daemon locks the operator group (`tests/test_a7_authority.c:250-268`), so
`test_orch_remote.c` drives `atlas_orch_apply` directly with the flag set, and T11
observes `atlas job list --remote` on the machine.

The deferred shape depends on this decision twice: `atlas job run --resume RUN` reads
the run through `job.run_status` (`xport_run_get`, `service_orch.c:759-800`, scope at
`server_orch.c:782`) and the task through `job.get` (`xport_job_get`, `:855`), both as
the operator's uid against a root whose `submitter_uid` is the gateway's. Without the
widening, the deferred shape answers "no such run" and cannot be driven at all.

### Decision 14 — Every remote submission creates a run, and what that run does is the driver's existing behaviour, stated

A root submission always creates a run (`submit_resolve_run`, `db_orch.c:880-910`).
Under `claude` the run is workspace-rooted and never settles — A12.0's stated cost
for planner jobs, now produced on purpose for every unattended remote job; the job's
own state (SUCCEEDED when every gate passed in the workspace, FAILED, TIMED_OUT,
CANCELLED) is the answer a caller reads, and `run: ACTIVE` beside it is not a claim.
Under `claude-repo` the run is a real A11.1 run the operator drives, with A11.1's
budget of three worker starts and one follow-up per gate failure. Neither is changed
by this season and both are written into `docs/remote-submission.md` so a reader of a
remote job's `run_status` does not go looking for a settlement that will never come.

### Decision 15 — Two injection channels, one existing and one extended; never a flag, an environment variable or a policy path override

Daemon-method tests use the in-process edge from `tests/test_plan_rpc.c:329-395`: a
real writer, `ctx.orchpolicy` and `ctx.gwpolicy` both filled through the parse seams
and marked ENABLED by the test, and `atlas_server_dispatch` called as the uid the
policy names as `gateway_uid`. No socket, no fork, no root-owned file. HTTP tests need
a real socket and a real daemon holding both policies: A16 T5's
`atlas_daemon_opts.gwpolicy_text` and `tests/tools/atlas_gw_daemon.c` gain a sibling,
`atlas_daemon_opts.orchpolicy_text` and a third argument
(`atlas-gw-daemon DATA_DIR GW_POLICY_FILE [ORCH_POLICY_FILE]`), with `daemon.c:212`
becoming the same three-way branch A16 makes of `:224` — inject if given, else load,
else zero. The orchestration policy injected names the test's own uid as
`model_dispatcher_uid` and `fake` as the only driver, so a remotely submitted job can
be leased and completed by the test itself through the real dispatcher group and the
whole path is exercised with no model, no network and no money. Two facts this rests
on, verified: **the daemon enforces no model/non-model partition on a lease** —
`op_lease` (`db_orch.c:1504`) matches the lease's own filter against
`orch_jobs.driver` and nothing in `src/db/db_orch.c` or `src/ipc/server_orch.c`
consults `needs_live_model` or `atlas_orchpolicy_is_model_dispatcher`; the partition
is built client-side by `atlas_service_orch_driver_filter`, so a test naming its uid
as `model_dispatcher_uid` and filtering `fake` is granted `fake`. And **`make` and
`true` are both on the gate program allowlist** (`src/orch/validate.c:28`: `make`,
`ctest`, `cmake`, `true`, `false`), so T5's fixture gate `true` and T11's deployment
floor `make` both run. P0's rule: the hook is on `atlas_daemon_opts` and on a tool
binary, and nowhere a person who can merely start a daemon can reach.

### Decision 16 — Mission Control's Jobs view holds the submission credential in tab memory, submits through `apiWrite`, reads through the same routes, and shows the two chains at the moment of use

A new `jobs` entry in `VIEWS` (`mission-control.html:224-229`); a password field for
the submission key (memory only, or `sessionStorage` per A16's answered row 3 — the
same rule for the same reason, one variable `submitKey` beside `disposeKey`); a
repository picker from `loadRepos()`; a task textarea bounded at 65536; an optional
key field bounded at 40; a `Submit` button that POSTs through A16's `apiWrite` with
`credentials: "omit"`; a list of this credential's jobs with state, driver, created
time, attempts and reported cost, each with a `Cancel` button while non-terminal; and
the fixed sentences in §Frozen formats — the channel sentence, the worker-authority
sentence, the cleartext sentence when `/auth/me` reports it, and the "read the patch
on the machine" sentence. Everything `textContent`; no new external resource; the CSP
untouched. **No test executes the page's JavaScript**; the suite greps the served
bytes and drives the routes with a real bearer against the tool daemon.

### Decision 17 — Audit, log and status say what happened without saying more

Every bearer-table request appends its `gw_audit` row through `audit()`
(`gateway.c:606-655`), `interface = WEB_API`, `operation` = the route path, `key_id`
= the verified selector — never the token, never the task text. A remote `tools/call`
audits as today (Decision 10's stated cost). The daemon logs one safe-encoded line
per remote submit, remote cancel and remote refusal, naming the key id, the repository,
the job uid and the driver — never the task text. `atlas gateway status` prints a
`submit:` line and a `clear-submit:` line (human and JSON) on every run, on
`acbd7ad`'s reasoning for `anon:` and A16's for `clear:`: a credential that can start
work as the operator, and whether it crosses the network in the clear, are facts an
auditor must get from the command Atlas offers for that question. `/auth/me` gains
`remote_submission`, `remote_submission_driver` and `cleartext_submission`.

---

## Frozen formats

Every new vocabulary member, policy key, route row, method name, tool name and schema
property, request and response shape, refusal sentence, ledger sentence, log line,
status line and UI sentence this season introduces. The executor implements these
verbatim; a change here is a plan amendment, dated.

### Vocabulary members

```c
/* include/atlas/apikey.h — appended after ATLAS_SCOPE_DECISIONS_DISPOSE, before
 * ATLAS_SCOPE__COUNT. Never stored on a key row: derived by the daemon for exactly the
 * credentials the root-owned `remote_submit_key` lines name. Unlike DECISIONS_DISPOSE
 * it is derived for a key that may hold stored read scopes, and Decision 1 says why. */
ATLAS_SCOPE_JOBS_SUBMIT                                  /* name: "jobs:submit", grantable = false */

/* include/atlas/limits.h */
#define ATLAS_GW_SUBMIT_BODY_MAX_BYTES 262144u  /* 3 × ATLAS_ORCH_TASK_MAX for percent-encoding, plus the other fields */

/* include/atlas/gwpolicy.h */
#define ATLAS_GWPOLICY_MAX_SUBMIT_KEYS 4
#define ATLAS_GWPOLICY_GATE_LINE_MAX 256u
#define ATLAS_GWPOLICY_SUBMIT_MAX_ACTIVE_CEILING 8
#define ATLAS_GWPOLICY_SUBMIT_MAX_PER_DAY_CEILING 64

/* include/atlas/orch.h */
#define ATLAS_ORCH_REMOTE_CLIENT_KEY_MAX 40u   /* "remote." + 16 hex + "." + this == ATLAS_ORCH_NAME_MAX */
```

```c
/* include/atlas/gwpolicy.h — new members of atlas_gwpolicy */
char remote_submit_keys[ATLAS_GWPOLICY_MAX_SUBMIT_KEYS][ATLAS_APIKEY_SELECTOR_HEX + 1u];
size_t remote_submit_count;                              /* 0 = remote submission off */
char remote_submit_driver[ATLAS_ORCH_NAME_MAX + 1u];
char remote_submit_mode[ATLAS_ORCH_NAME_MAX + 1u];
char remote_submit_gates[ATLAS_ORCH_MAX_VALIDATIONS][ATLAS_GWPOLICY_GATE_LINE_MAX];
size_t remote_submit_gate_count;
long long remote_submit_max_attempts;
long long remote_submit_max_active;
long long remote_submit_max_per_day;
/* True only when the policy carries `operator_accepts_cleartext_submission = yes`.
 * Never a default; refused under REVERSE_PROXY; refused without a submit key; and
 * never implied by `cleartext_disposal_accepted`, for the reason in §The decision
 * on cleartext. */
bool cleartext_submission_accepted;

/* include/atlas/orch_ops.h — new members of atlas_orch_op */
atlas_buf remote_token;                                  /* REMOTE only: the presented bearer; wiped in _free */
char remote_key_id[ATLAS_APIKEY_SELECTOR_HEX + 1u];      /* filled by the write point after verification; empty on entry */
char remote_client_key[ATLAS_ORCH_REMOTE_CLIENT_KEY_MAX + 1u]; /* the request's `key`; the write point namespaces it */
char remote_allowed_ids[ATLAS_GWPOLICY_MAX_SUBMIT_KEYS][ATLAS_APIKEY_SELECTOR_HEX + 1u];
size_t remote_allowed_count;                             /* 0 on a local op */
int64_t remote_max_active;                               /* REMOTE SUBMIT only */
int64_t remote_max_per_day;                              /* REMOTE SUBMIT only */
bool peer_is_operator;                                   /* set by the method from atlas_server_peer_is_operator; never from a parameter */

/* include/atlas/orch_ops.h — new members of atlas_orch_result (SUBMIT) */
char key_id[ATLAS_APIKEY_SELECTOR_HEX + 1u];             /* REMOTE only; empty otherwise */
int64_t remote_active;                                   /* after this submission */
int64_t remote_today;                                    /* after this submission */

/* include/atlas/orch_ops.h — new members of atlas_orch_job_view and atlas_orch_list_row */
char submit_key_id[ATLAS_APIKEY_SELECTOR_HEX + 1u];      /* '' for a local job */

/* include/atlas/daemon.h — new member of atlas_daemon_opts (test hook, Decision 15) */
const char *orchpolicy_text;

/* src/mcp/mcp_tools.c — new member of tool_def, after `scope` */
bool remote_only;                                        /* listed and callable only when s->remote */
/* include/atlas/mcp.h */
bool atlas_mcp_tool_remote_only(const char *name);

/* src/mcp/mcp_internal.h — new member of atlas_mcp_server */
atlas_buf remote_token;                                  /* the request's bearer, for the four job tools; wiped at teardown */
```

### The eight policy keys (`/etc/atlas/gateway.conf`)

```ini
# --- remote submission (A14) --------------------------------------------------
#
# A credential named here may queue a job through the gateway, from Mission
# Control or from a model over /mcp. Everything a job IS -- which driver runs it,
# in what mode, under which gates, how many attempts, how many at once and how
# many per day -- is decided by these lines and never by the request. All of
# them, or none. Every one REQUIRES tls_mode = REVERSE_PROXY unless the
# acceptance key below is present. A key named here may not also be the
# remote_dispose_key: one credential, one power.
#
# remote_submit_key          = key_b2578f48143c06d3
# remote_submit_key          = key_<the browser's own --no-scopes key>
# remote_submit_driver       = claude
# remote_submit_mode         = patch
# remote_submit_gate         = make
# remote_submit_max_attempts = 1
# remote_submit_max_active   = 2
# remote_submit_max_per_day  = 6

# THE OPERATOR'S WRITTEN ACCEPTANCE OF A CLEARTEXT SUBMISSION CHANNEL. Not a
# feature toggle, and NOT implied by operator_accepts_cleartext_disposal: a
# submission starts a worker that runs as you, which is a different consequence
# from moving a record. With this line present and tls_mode = NONE, a browser's
# submission credential crosses the network unencrypted on every request and a
# captured one queues work that runs as you, within remote_submit_max_per_day,
# until `atlas api-key revoke`. A credential presented from this host itself --
# the MCP tunnel's -- never crosses the segment; its exposure is the file it lives
# in, which a worker running as you can read. `yes` is the only accepted value;
# leave the line out rather than writing `no`. Refused under tls_mode =
# REVERSE_PROXY and without a remote_submit_key. `atlas gateway status` prints
# this acceptance on every run.
# operator_accepts_cleartext_submission = yes
```

- `remote_submit_key`: exactly `key_` + 16 lowercase hex, the form `atlas api-key
  list` prints; stored without the prefix; repeatable up to 4; a duplicate is
  MALFORMED; the same id as `remote_dispose_key` is MALFORMED.
- `remote_submit_driver`, `remote_submit_mode`: one name each in the shape
  `atlas_orchpolicy_parse_bytes` accepts (`[a-z0-9._-]`, ≤ 64); singletons. Whether
  the orchestration policy lists them is checked **at submit** and logged at daemon
  start, not here — this loader does not read the other file.
- `remote_submit_gate`: repeatable, 1..8, each ≤ 255 printable ASCII bytes, non-empty
  after trimming, first token containing no `/`; split at submit.
- `remote_submit_max_attempts`: 1..`ATLAS_ORCH_MAX_ATTEMPTS` (5); above the
  orchestration policy's own ceiling is refused at submit by
  `atlas_orchpolicy_apply_limits`'s existing sentence, not here.
- `remote_submit_max_active`: 1..8. `remote_submit_max_per_day`: 1..64.
- MALFORMED (gateway DISABLED, reason MALFORMED): a bad key shape; a duplicate key;
  a key equal to `remote_dispose_key`; more than four keys; any key with any other
  submission line absent (all eight-minus-acceptance or none); a gate line empty,
  over-long, non-printable or with `/` in its first token; more than eight gates; a
  bound of 0, negative, or above its ceiling; a driver or mode not of name shape;
  a duplicated driver or mode line; any submission line with `tls_mode` not
  `REVERSE_PROXY` **unless** `operator_accepts_cleartext_submission = yes`;
  `jobs:submit` inside `web_gui_anonymous_scopes` (already refused by the
  ungrantable check, tested explicitly); the acceptance key with any value but
  `yes`; the acceptance key under `REVERSE_PROXY`; the acceptance key without a
  submit key. `web_gui = no` with submission lines is **not** MALFORMED: `/mcp` is
  a submission surface independent of the browser.
- Loaded by the daemon at start and by the gateway at start. **Editing it means
  restarting both.**

### The minting sentence A16 wrote, amended

`atlas api-key create --label L --no-scopes` succeeds with A16's block, whose second
and third lines change to:

```
scopes: (none) -- this credential authorises nothing on its own. Only a root-owned
        line in /etc/atlas/gateway.conf can give it a scope: remote_dispose_key
        gives decisions:dispose, a remote_submit_key line gives jobs:submit, and
        the same key may never be named by both. Name it there, or revoke it.
```

Refusal (exit 2), beside A16's for `decisions:dispose`:

```
jobs:submit cannot be granted to a credential; it is derived for the keys a remote_submit_key line in /etc/atlas/gateway.conf names
```

### The daemon method group (`src/ipc/server_orch_remote.c`)

```c
static const atlas_method_entry REMOTE_SUBMIT_METHODS[] = {
    {"job.remote_submit", method_remote_submit},
    {"job.remote_get", method_remote_get},
    {"job.remote_list", method_remote_list},
    {"job.remote_cancel", method_remote_cancel},
};
const atlas_method_entry *atlas_server_remote_submit_methods(size_t *count_out);
/* Offered iff all three: atlas_server_peer_is_gateway(ctx, peer_uid);
 * ctx->gwpolicy.remote_submit_count > 0; and (tls_mode == REVERSE_PROXY or
 * ctx->gwpolicy.cleartext_submission_accepted). Otherwise `unknown method`. Never
 * consults atlas_orchpolicy_permits_submitter. */
bool atlas_server_remote_submit_offered(const atlas_server_ctx *ctx, long long peer_uid);
```

```c
/* include/atlas/orch_remote.h, src/orch/remote.c */
atlas_status atlas_orch_remote_verify(atlas_db *db, const atlas_buf *token,
                                      const char allowed[][ATLAS_APIKEY_SELECTOR_HEX + 1u],
                                      size_t allowed_count,
                                      char key_id_out[ATLAS_APIKEY_SELECTOR_HEX + 1u],
                                      atlas_err *err);
/* `remote.<key_id>.<client>`; refuses a client part outside [a-z0-9._-]{1,40}. */
atlas_status atlas_orch_remote_idempotency_key(const char *key_id, const char *client,
                                               atlas_buf *out, atlas_err *err);
```

`job.remote_submit` params: `repo` (string), `task` (string, ≤ 65536, no NUL), `key`
(optional, ≤ 40), `token` (the bearer, added by the gateway or by the MCP tool). Any
other parameter present is refused. Response: `job`, `state`, `seq`, `run`,
`duplicate`, `spec_digest`, `key_id`, `driver`, `mode`, `attempts_max`, and a `budget`
object `{active, active_max, today, today_max}`.

`job.remote_get` params: `job`, `token`. Response: `job.get`'s fields (`job`, `state`,
`repo`, `commit`, `mode`, `driver`, `spec_digest`, `created_at`, `terminal_at`,
`attempts`, `max_attempts`, `cancel_requested`, `seq`, `task_encoding`, `task`) plus
`key_id`, `run`, `reason` (the newest transition's `atlas_orch_reason_name`), and
`usage` `{present, model, has_cost, cost_micro_usd, has_turns, turns}` from
`atlas_db_orch_job_usage`. **No artifact, no log, no gate output.**

`job.remote_list` params: `token`, `after` (int), `limit` (int). Response: `jobs[]`
rows (`job`, `state`, `repo`, `driver`, `created_at`, `attempts`), `count`, `cursor`,
`more` — scoped to the verified key id.

`job.remote_cancel` params: `job`, `token`. Response: `job`, `state`, `seq`, `run`.

`job.list` (existing, operator group) gains `remote` (bool): when true and the peer is
the operator, lists rows whose `submit_key_id` is non-empty; each row gains `key_id`.
`job.get` gains `key_id` when non-empty.

### The gateway's bearer table rows (`src/gw/gateway.c`)

```c
/* Appended to API_WRITE_ROUTES[]. Every row's scope is ungrantable and on the
 * positive list in tests/test_gateway.c; the two reads are here because their
 * only principal is the bearer, which the read table does not forward. */
{"/api/v1/job/submit", "job.remote_submit", ATLAS_SCOPE_JOBS_SUBMIT,
 {"repo", "task", "key", NULL}, {NULL}, ATLAS_GW_SUBMIT_BODY_MAX_BYTES},
{"/api/v1/job/get", "job.remote_get", ATLAS_SCOPE_JOBS_SUBMIT,
 {"job", NULL}, {NULL}, ATLAS_GW_WRITE_BODY_MAX_BYTES},
{"/api/v1/job/list", "job.remote_list", ATLAS_SCOPE_JOBS_SUBMIT,
 {"after", "limit", NULL}, {"after", "limit", NULL}, ATLAS_GW_WRITE_BODY_MAX_BYTES},
{"/api/v1/job/cancel", "job.remote_cancel", ATLAS_SCOPE_JOBS_SUBMIT,
 {"job", NULL}, {NULL}, ATLAS_GW_WRITE_BODY_MAX_BYTES},
```

`api_handle_write`'s frozen order is A16's with two substitutions: the 413 step reads
`route->body_max`; the 404 step asks `route_offered(g, route)`. The gateway appends
`token` = the presented bearer; no row may declare `token`, `driver`, `mode`,
`validation`, `parallel`, `memory` or `parent` as client-supplied, and the property
test says so.

Requests, as the page sends them:

```
POST /api/v1/job/submit HTTP/1.1
Authorization: Bearer atlas_<selector>_<secret>
Content-Type: application/x-www-form-urlencoded

repo=atlas&task=Add+a+unit+test+for+atlas_orch_gate_split+covering+a+tab-separated+line&key=gate-split-tab-1
```

`/auth/me` success body gains, after `cleartext_disposal`: `"remote_submission":
true|false` (the policy names at least one key), `"remote_submission_driver": "<name>"`
(or `""`), `"cleartext_submission": true|false`.

### The gateway's refusal sentences

```
404 not_found        this gateway does not serve remote submission
401 unauthenticated  a submission needs a credential the policy names, presented as a bearer token; a session cookie or the anonymous floor cannot submit
403 forbidden        this credential does not hold the "jobs:submit" scope     (existing shape)
413 request_too_large the request body exceeds the gateway limit              (existing; bound per row)
405, 415, 400, 409   A16's sentences, unchanged
```

### The write-point and method refusal sentences (`src/orch/remote.c`, `src/db/db_orch.c`, `src/ipc/server_orch_remote.c`)

```
the credential presented for this submission did not authenticate; nothing was queued
that credential is not one the remote submission policy names
credential %s already has %lld active remote job(s), which is its bound of %lld; it takes no further one until one of them ends
credential %s has submitted %lld job(s) today (UTC), which is its bound of %lld; it takes no further one until tomorrow
a remote submission names the repository, the task and an idempotency key; %s is decided by the root-owned policy
a remote idempotency key is at most 40 characters of [a-z0-9._-]
the remote driver %s is not one /etc/atlas/orchestration.conf configures
the remote mode %s is not one /etc/atlas/orchestration.conf configures
the remote driver %s needs a live model and /etc/atlas/orchestration.conf has live_model = off
remote gate %zu could not be split: %s
```

`no such job` stays the answer to a job another credential submitted, at every remote
read and at cancel, for `job.get`'s reason. `orch_disabled`'s sentence and the existing
`the orchestration policy does not permit jobs against that repository` and `no
repository named that is registered` are unchanged and reached on this path.

### The ledger sentence (`orch_transitions.detail`, SUBMITTED)

```
submitted through the Atlas gateway with credential %s; this records the channel and the credential, not which person or program presented it
```

`key_id` is written beside it. A local submission's detail is unchanged.

### The daemon's log lines

```
remote submit accepted key=%s repo=%s job=%s driver=%s active=%lld/%lld today=%lld/%lld
remote submit refused key=%s: %s
remote cancel key=%s job=%s
remote submission policy: driver %s is not in the orchestration policy; every remote submission will be refused until one of the two files changes
```

The key id is Atlas-checked; the repository name and the refusal are safe-encoded;
task text never appears.

### The MCP tools (`src/mcp/mcp_tools.c`)

| name | title | schema properties (all `additionalProperties: false`) | writes | remote_only |
| --- | --- | --- | --- | --- |
| `atlas_job_submit` | Submit a task | `repo` (string), `task` (string ≤ 65536, required), `key` (string ≤ 40) | yes | yes |
| `atlas_job_status` | Job status | `repo` (string), `job` (string, required) | no | yes |
| `atlas_job_list` | Jobs this credential submitted | `repo` (string), `after` (int ≥ 0), `limit` (int 1..200) | no | yes |
| `atlas_job_cancel` | Cancel a job | `repo` (string), `job` (string, required) | yes | yes |

Descriptions, verbatim:

- `atlas_job_submit`: "Queue one task for a worker on the Atlas machine. Atlas decides
  the driver, the gates, the attempts and the daily bound from a root-owned policy;
  you name the repository, the task text and an optional idempotency key. Pass the
  same key on a retry so it resolves to the job you already made. A queued job is
  not an authority: nothing it produces is applied, committed, approved or accepted
  by Atlas, and the patch it makes is read on the Atlas machine, never here. Task
  text is stored as UNTRUSTED_DATA."
- `atlas_job_status`: "State, reason, attempts and reported cost of one job this
  credential submitted. Never its output."
- `atlas_job_list`: "Jobs this credential submitted, oldest first, bounded and
  paginated."
- `atlas_job_cancel`: "Ask Atlas to stop one job this credential submitted. A queued
  job is cancelled outright; a running worker learns of it at its next heartbeat."

No schema declares `token`, `confirmation`, `challenge`, `approved`, `driver`, `mode`,
`gate`, `validation`, `parallel`, `memory` or `parent`.

### `atlas gateway status`

Human, after A16's `clear:` line, two lines, always both when ENABLED:

```
submit:  key_b2578f48143c06d3 key_1f0a…  (driver claude, mode patch, 1 gate(s), attempts 1, active 2, per day 6; checked at submit)
clear-submit: ACCEPTED -- operator_accepts_cleartext_submission = yes: a browser's submission credential crosses this network unencrypted, and a captured credential queues work that runs as the operator until it is revoked
```
or
```
submit:  (none -- nothing reachable over the network can queue a job)
clear-submit: (not accepted -- a submission credential is offered only behind tls_mode = REVERSE_PROXY)
```

JSON: `"remote_submit_keys"` (array of ids), `"remote_submit_driver"`,
`"remote_submit_mode"`, `"remote_submit_gates"` (array), `"remote_submit_max_attempts"`,
`"remote_submit_max_active"`, `"remote_submit_max_per_day"`,
`"cleartext_submission_accepted"`.

### The CLI

`atlas job list --remote` sends `remote: true` on `job.list`. It is honoured for the
operator peer and **refused** — never ignored, Decision 4's rule for a request
parameter — for any other peer, with:

```
this connection may not list jobs submitted through the gateway
```

Human rows and `atlas job get` gain `credential: key_…` when present; JSON gains
`key_id`. Both renderers.

### The Jobs view's fixed sentences

Under the **Submit a task** heading, verbatim:

> Submitting from this browser queues a task under the credential you paste here.
> Atlas records the channel and the credential, not a person. What the task may do is
> decided by the root-owned policy on the Atlas machine, never by this page: the
> driver, the gates, the attempts and the daily bound are its.

Directly beneath it, verbatim:

> A task is a prompt. On this deployment it runs as the operator's own account, with
> that account's files readable to it and its edits accepted in a workspace; Atlas
> bounds how many tasks a credential may queue and what checks they run under, not
> what one task may ask a worker to read. Nothing here applies a patch, changes a
> lifecycle state or accepts a run; a finished job's patch is read on the Atlas
> machine.

When `/auth/me` reports `cleartext_submission: true`, beneath that, verbatim:

> On this deployment the submission key crosses the network in the clear from a
> browser. Anyone able to observe this network segment can capture it, and a captured
> key queues work that runs as the operator until it is revoked.

Beside the key field: A16's memory-only sentence, or its `sessionStorage` sentence,
per A16's answered row 3 — the same one A16's panel shows, never both.

When `/auth/me` reports `remote_submission: false`, in place of the panel, verbatim:

> This gateway does not serve remote submission. Jobs are submitted with atlas job
> submit on a terminal on the Atlas machine.

### The cleartext chain for submission

Verbatim in `SECURITY.md`, `docs/remote-access.md`, `docs/remote-submission.md` and
`docs/orchestration.md`, in the paragraph that announces the capability, and in short
form in `CLAUDE.md`'s season paragraph:

> **On this deployment a submission credential presented by a browser travels in the
> clear.** The gateway listens on `192.168.0.198:8799` with `tls_mode = NONE`, and the
> four submission routes carry the credential as a bearer header on every request, so
> anyone able to observe traffic on that network segment can read it. An Atlas API
> credential has no expiry, so a credential captured once queues work — a worker that
> runs as the operator's own account, within the policy's daily bound — until the
> operator notices and runs `atlas api-key revoke`. The credential the MCP tunnel
> presents does not cross that segment: the tunnel client runs on this host and posts
> to this host's own address, so its exposure is the file it is read from in the
> operator's home directory — readable by exactly the account a remotely submitted
> worker runs as — and the far side of the tunnel. The operator was shown this chain
> on 2026-09-04 and accepted it for this network by writing
> `operator_accepts_cleartext_submission = yes` into the root-owned gateway policy;
> `atlas gateway status` prints that acceptance on every run. Atlas states this cost
> and does not judge the trade; the same key on a listener reachable from a network
> the operator does not control is a different decision using the same mechanism.

*(The sentence beginning "The operator was shown" is written into the documents only
if row 2 of §Decisions the operator must be asked is answered yes; if no, the
paragraph ends at "revoke" and a sentence naming the terminator replaces it.)*

### Migration 32

```c
{32, "which credential queued a job, beside which uid: the remote submission channel", M32_STATEMENTS, false},
```

`M32_STATEMENTS` = `{M32_JOBS, M32_TRANSITIONS, NULL}`:

```sql
ALTER TABLE orch_jobs ADD COLUMN submit_key_id TEXT NOT NULL DEFAULT '';
CREATE INDEX idx_orch_jobs_submit_key ON orch_jobs(submit_key_id, id);
ALTER TABLE orch_transitions ADD COLUMN key_id TEXT NOT NULL DEFAULT '';
```

No table is rebuilt, no CHECK moves, `foreign_keys_off` stays false. The migration
comment states that `''` is a true statement about every existing row (one channel
existed), that `orch_idempotency` is deliberately untouched (Decision 6), and that
`atlas_db_orch_job_list`'s existing index still serves the operator's default list.

---

## Authority argument — the season's non-negotiables

These go into `docs/engineering-rules.md` in full and into `CLAUDE.md` as one line each
(T10). The roadmap's three "must be true whichever shape wins" are first.

- **A budget per key, in the policy, checked in the transaction.** Two bounds per
  credential — active jobs and root submissions per UTC day — read from root-owned
  lines, counted from stored rows inside `op_submit`, refused with the count and the
  bound named, never clamped. A11.1's three starts bound a chain; these bound a caller.
- **A9's absences stay absences.** No remote credential administration, no MCP tool
  name with an authority verb (`submit`, `status`, `list`, `cancel` are none), no
  route or method that applies, commits, pushes, accepts or settles; `job.remote_*`
  is four names and none of them is `dispatch.`, so the gateway still cannot lease,
  heartbeat or complete a job. Submission is bounded by the gates the operator's floor
  names and settled by Atlas' own verdict; a model payload still cannot accept a run.
- **The audit row names the key, never a claimed value.** `gw_audit.key_id` is the
  verified selector on every bearer-table request; `orch_jobs.submit_key_id` and
  `orch_transitions.key_id` are written by the write point from its own verification;
  `submitter_uid` stays `SO_PEERCRED`'s answer.
- **What the gateway cannot do is still true because of who it runs as.** The
  gateway's uid reaches four new names only when a root-owned policy names a
  credential, and each of those names does nothing without that credential presented
  on the request and verified by the daemon. `submitter_uid` in
  `/etc/atlas/orchestration.conf` is never the gateway's; `require_submitter` is
  never on the remote path; T5 dispatches as the gateway uid against a policy listing
  another submitter and gets refused for the credential, never admitted for the uid.
  No check in `src/gw` is the boundary and none is described as one.
- **Its own scope, ungrantable, derived for named keys; the disposal scope never
  widened; one credential, one power.** `jobs:submit` is in `SCOPES[]` with
  `grantable = false`; `remote_submit_key` and `remote_dispose_key` may never name the
  same id, and the parser refuses it.
- **The credential is verified by the daemon on every remote job method — inside
  the transaction for the two that write.** A key revoked between `gateway.auth` and
  the writer's turn queues nothing. Three copies of one check exist and the reason for
  each is written at its head.
- **The request names the repository, the task and an idempotency key, and nothing
  else.** Driver, mode, gate floor, attempts and both budgets are the policy's; a
  request naming any of them is refused, not ignored. A remote submission adds no
  gate, runs no memory, holds one slot and joins no run.
- **No remote read of a worker's output.** No artifact, no log, no gate output
  travels a remote route or tool; state, reason, attempts and cost do. The chain that
  this does not close — the transcript — is written where the capability is.
- **`submitter_uid` is the kernel's, the credential is a column beside it, and the
  spec digest does not move.** `ATLAS_ORCH_SPEC_DOMAIN` is unchanged; the credential
  travels on `atlas_orch_op`.
- **The idempotency namespace is `remote.<key_id>.<client>` from one builder**, and
  the client part is at most 40 bytes so the whole fits the name bound.
- **A repo-tree driver named as the remote driver is a queue, not an executor.** No
  background dispatcher ever leases it; only the operator's foreground driver does,
  and only while the pin holds.
- **Two written acceptances, never one implying the other.**
  `operator_accepts_cleartext_submission` has A16's grammar and its own chain, and the
  chain says which credential crosses the wire and which does not.
- **The operator's terminal sees, cancels and drives remote jobs**, by a flag the
  method derives from `SO_PEERCRED`, never from a parameter; provable at the write
  point in process and on the deployment, and stated as such.
- **The MCP tools are remote-only and absent from the stdio adapter**; the bearer
  reaches them through the server struct for the life of one exchange and is wiped;
  no schema declares a token.
- **The test channels are `atlas_daemon_opts` members and a tool binary**, and the
  in-process edge; never a flag, an environment variable or a policy path override.
- **`atlas_orch_apply_in_tx` keeps exactly one caller**, and the decision layer's two
  counts are untouched.
- **No new thread, process, timer or background loop is added by code.** T11
  installs the operator's model dispatcher unit *by hand, as the operator*, and only
  if row 1 says `claude`; nothing in `src/` starts it.

## Worst-case cost, stated so nobody discovers it in a bill

**Per remote submission:** one `POST` (or one `tools/call`), one `gateway.auth`, one
HMAC verification in the write transaction, two counting queries, one job row, one run
row, one ledger row, one `gw_audit` row, one daemon log line. Zero processes at
submission.

**Per remote job, once leased (unattended shape, `claude`):** up to
`remote_submit_max_attempts` worker starts, each a Claude Code process as the
operator's account bounded by the orchestration policy's `max_wall_timeout_ms`
(900 000 ms on this machine) and `max_idle_timeout_ms` (300 000 ms), each followed by
the gate floor in the workspace. **Money:** A10.1 measured **$10.253520 for two worker
starts of 435 s each — $5.13 per start, observed** (`docs/roadmap.md:979`), on a small
real task, one model. Extrapolated linearly to the 900 s wall bound — an extrapolation,
not a measurement, and one Atlas never makes itself — a single start could cost about
**$10.6**. With the values this plan proposes for this deployment (row 3:
`max_attempts = 1`, `max_per_day = 6`, two keys):

| | observed rate ($5.13/start) | at the wall bound (~$10.6/start, extrapolated) |
| --- | ---: | ---: |
| one job | $5.13 | $10.6 |
| one credential, one UTC day, `max_attempts = 1`, `max_per_day = 6` | $30.8 | $63.6 |
| both credentials, one day | $61.6 | $127 |
| the same with `max_attempts = 3` (the orchestration ceiling here) | $185 | $382 |

The formula an operator can recompute with their own lines: **keys × per_day ×
attempts × cost-per-start**. Every stored attempt's actual cost is provider-reported
into `orch_usage` and read back by `job.remote_get`; **absent is not zero**, and a
job whose worker produced no final record spent something the table cannot name.

**Per remote job (deferred shape, `claude-repo`):** zero until the operator runs
`atlas job run --resume RUN`, then A11.1's bound of three worker starts per run at
the same per-start figures. A queued task's cost is one row and the operator's
attention.

**Per credential holder:** whoever holds a named credential can queue up to
`remote_submit_max_active` jobs at once and `remote_submit_max_per_day` per UTC day,
each of which runs as the operator's account on the unattended shape, **and can do so
until `atlas api-key revoke`**, which takes effect on the next request
(`tests/test_gw_remote.c`, "a revoked credential stops working immediately") and on
the next write-transaction verification. A credential in a model's hands is subject to
whatever that model can be talked into; the bound is the policy's, and the transcript
chain in Decision 7 is the cost this season states and cannot close.

**Migration 32:** two `ALTER TABLE` statements and one index over `orch_jobs`,
bounded by the job table's size; on this machine, dozens of rows.

**The live acceptance (T11):** one `fake` job at zero cost, then — on the unattended
shape — one real `claude` job: **one start, $5–11 by the figures above**, the
operator's contemporaneous go-ahead recorded before it runs (the operator's own rule
about paid runs). On the deferred shape, one real start only when the operator drives
it.

---

# Decisions the operator must be asked, and when — read this before dispatching anything

> **ANSWERED 2026-09-04, before any task was dispatched.** Rows 1 and 2 were put to the
> operator with the cost in front of them and are settled below; row 3 stands. Their
> answers also added a requirement this plan did not carry — see *What the answers added*
> immediately after the table.
>
> - **Row 1 — start while away.** `remote_submit_driver = claude`. Their words: *"başlasın"*.
> - **Row 2 — cleartext accepted, for now.** `operator_accepts_cleartext_submission = yes`.
>   Their words: *"evet şu anda olabilir ileride bunu daha güvenli hale getiririz"* — yes for
>   now, we make it more secure later. Recorded as a decision with an intent attached, not
>   as a permanent shape: the chain in §The decision on cleartext is unchanged and still
>   true, and the key stays a written acceptance that `atlas gateway status` prints, so the
>   day they change their mind the line comes out and the season's own gate refuses again.
> - **Row 3 — which credentials and which numbers.** Still to ask, still before T1.


A15's plan left a genuine choice in its §The decision and nobody put it to the
operator; fourteen hours followed. A16 asked its rows before T1. Every choice this plan
leaves is here, in plain words about the thing itself, with the default the plan
assumes and **the task before which it must be asked**. All three are asked before T1:
two of them change only deployment steps, but carrying an open question into a season
costs more than asking it now, and row 2 changes whether the season has a twelfth task.

| # | Question, in full | Default this plan assumes | Ask before |
| --- | --- | --- | --- |
| 1 | **When a task arrives from the model or from your phone, should a worker start on it while you are away, or should it wait until you start it from a terminal?** *Start while away* means `remote_submit_driver = claude`: the model dispatcher runs as your account in a workspace, spends model budget on the credential's word within the daily bound, and leaves a patch you read on the machine with `atlas job artifact`. *Wait for me* means `remote_submit_driver = claude-repo`: the task sits queued, pinned to the commit the repository was at when it arrived, until you run `atlas job run --resume RUN`; nothing is spent until then; and if you commit anything to that repository first, the task is refused as pinned to a moved tree and has to be resubmitted. Either is one word in one root-owned line; the code is the same. | `claude` (start while away) | **T1** |
| 2 | **Do you accept that a submission credential presented from a browser crosses your network in the clear — with a captured credential able to queue work that runs as your account, within the daily bound, until you revoke it?** This is a different consequence from the disposal acceptance you gave on 2026-09-04 (moving a record) and the plan does not reuse that answer. The credential the ChatGPT tunnel presents does not cross the segment at all; its exposure is the `0600` file in your home directory, which a worker running as you can read. *Yes* is one root-owned line, `operator_accepts_cleartext_submission = yes`, printed by `atlas gateway status` on every run. *No* means the terminator task A16 removed (`c305f40`: nginx on `192.168.0.198:8799`, the gateway on loopback, the tunnel re-pointed) returns as T11a before the live acceptance, and the group is offered under `tls_mode = REVERSE_PROXY` with no acceptance key. | yes | **T1** |
| 3 | **Which credentials, and what numbers?** The plan proposes naming `key_b2578f48143c06d3` (`chatgpt-tunnel`, the model) and one new `--no-scopes` key minted for the browser — never the `mission-control` login key, so a session cookie and a submission key are different secrets — with `remote_submit_mode = patch`, gate floor `make`, `remote_submit_max_attempts = 1`, `remote_submit_max_active = 2`, `remote_submit_max_per_day = 6`. Each number multiplies the bill in §Worst-case cost; each is one root-owned line and needs no code change. Do you want the model's key named at all, or only the browser's? | as proposed, both keys | **T1** |

## What the answers added: a place to watch the workers

Answering row 1 the operator added a requirement this plan did not carry, and it is theirs
rather than an inference — *"bunlar için UI'de bir tab olması lazım. workerlar ne
yapıyorlar vs vs"*: **there must be a tab in Mission Control showing what the workers are
doing.**

The reason follows from row 1 rather than being a preference. Choosing *start while away*
means a worker runs as their account, spends model budget, and edits a workspace at a time
when they are not at the machine — so the surface that tells them what happened is not a
convenience, it is the only thing that makes the choice reviewable. Without it, "start
while away" means "start where I cannot see it", and this season would repeat A15's shape:
a mechanism that works and a person who cannot use it.

**A twelfth task, T12, owns it**, and it comes before the live acceptance rather than after,
because the acceptance is the first time a real worker runs and that is exactly when the tab
has to already exist. Its shape follows the rules the read surface already has: it reads
routes that exist or routes added under the same fixed-table discipline, it renders every
byte a worker produced as `UNTRUSTED_DATA` with `textContent` and never as markup, and it
grants nothing — watching is a read, and no button in it may start, cancel or approve
anything that the write path does not already gate. What it shows: which runs exist and
their state, which task within a run is active, how many worker starts each has spent
against its budget, what the gates said, and — for a finished attempt — that an artifact
exists and how to fetch it on the machine. What it does **not** show is a worker's prose or
its log, for the reason §Rejected already gives: with the worker holding the operator's read
authority, a remote route to its output is a remote route to any file that account can read.
That limit is stated on the tab itself, not only here.


---

# File structure

**Create:**

| Path | Responsibility |
| --- | --- |
| `src/orch/remote.c`, `include/atlas/orch_remote.h` | `atlas_orch_remote_verify` (the in-transaction and read-handle credential check against a list of ids) and `atlas_orch_remote_idempotency_key` (the one builder) |
| `src/ipc/server_orch_remote.c` | `REMOTE_SUBMIT_METHODS[]`, `atlas_server_remote_submit_methods`, `atlas_server_remote_submit_offered`, `require_remote_submitter`, the four methods, the start-up cross-check log line |
| `tests/test_migrate32.c` | unit: the two columns, the index, the true default, row preservation, `pragma_foreign_key_check` |
| `tests/test_orch_remote.c` | integration: the write point under a remote op without a daemon — verification, budgets, namespace, cancel ownership, follow-up inheritance, the operator flag |
| `tests/test_orch_remote_rpc.c` | daemon: the four methods on the in-process edge, dispatched as the gateway uid, with both policies injected |
| `tests/test_gw_submit.c` | daemon: the four routes and the four tools end to end through `atlas_gateway_serve_bytes` against `atlas-gw-daemon` with an injected orchestration policy, leased and completed by the test through the real dispatcher group |
| `docs/remote-submission.md` | the season's document |

**Modify:** `include/atlas/apikey.h`, `src/gw/apikey.c`, `src/core/service_apikey.c`,
`src/cli/cli.c`, `src/cli/render_human.c`, `src/cli/render_json.c`, `src/db/migrate.c`,
`src/db/db_orch.c`, `include/atlas/orch.h`, `include/atlas/orch_ops.h`,
`include/atlas/gwpolicy.h`, `src/gw/gwpolicy.c`, `src/gw/gateway.c`,
`include/atlas/gateway.h`, `include/atlas/limits.h`, `src/ipc/server_gw.c`,
`src/ipc/server.c`, `src/ipc/server_internal.h`, `src/ipc/server_orch.c`,
`src/daemon/daemon.c`, `include/atlas/daemon.h`, `src/daemon/writer.c`,
`src/mcp/mcp_tools.c`, `src/mcp/mcp_internal.h`, `include/atlas/mcp.h`,
`src/mcp/mcp.c` (teardown wipes the token), `src/core/service_orch.c`,
`src/gw/ui/mission-control.html`, `tests/tools/atlas_gw_daemon.c` (A16's, one
argument added), `CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/test_apikey.c`,
`tests/test_gateway.c`, `tests/test_gw_remote.c`, `tests/test_orch_rpc.c`,
`tests/test_orch_run.c`, `tests/test_orch_model.c`, `tests/test_a7_authority.c`,
`tests/test_decision_mcp.c`, `tests/test_mcp.c`, `tests/test_plugin.c`,
`deploy/a9/gateway.conf.template`, `docs/roadmap.md`, `docs/orchestration.md`,
`docs/remote-access.md`, `docs/engineering-rules.md`, `docs/extending.md`,
`docs/backlog.md`, `SECURITY.md`, `README.md`, `CLAUDE.md`,
`/opt/atlas/deploy.local.sh` (local, gitignored), and last `include/atlas/atlas.h`
(`ATLAS_PHASE`).

**Deliberately not modified:** `src/orch/policy.c` and `include/atlas/orchpolicy.h`
(no key is added to the orchestration policy, and `submitter_uid` never gains a
gateway line); `src/orch/dispatch.c` and `src/orch/rundriver.c` (a remote job is an
ordinary job to both); `src/orch/orch.c`'s digest (Decision 3);
`src/decision/*` (no lifecycle verb is touched); `src/hook/hook.c`;
`integrations/claude/atlas/skills/atlas-memory/SKILL.md` (the four tools are absent
from the local adapter, so the skill needs no sentence and the plan says so rather
than spending its 21-byte margin); `src/ipc/server_remote.c` (A16's group gains no
row; A14's is a sibling); `deploy/a8/atlas-model-dispatcher.service` (installed by hand
in T11, unchanged).

---

# Tasks

Dependency order: T1 → T2 → T3 → T4 → T5 → T6 → T7 → T8 → T9 → T10 → T11. T4 depends
only on T1 and may run in parallel with T2 and T3; T7 depends on T5 and may run in
parallel with T6; everything else is ordered. **T1 is not dispatched until the three
rows of §Decisions the operator must be asked are answered and A16's final commit is
in.** T11a exists only if row 2 was answered no.

---

### Task T1: the scope, the amended minting sentence, and the re-read

**Files:**
- Modify: `include/atlas/apikey.h`, `src/gw/apikey.c`, `src/core/service_apikey.c`,
  `include/atlas/orch.h`, `include/atlas/limits.h`, `tests/test_apikey.c`,
  `tests/test_orch_model.c`, this plan

**Interfaces produced:** `ATLAS_SCOPE_JOBS_SUBMIT`; `ATLAS_ORCH_REMOTE_CLIENT_KEY_MAX`;
`ATLAS_GW_SUBMIT_BODY_MAX_BYTES`; the amended `--no-scopes` block and the
`jobs:submit` refusal (§Frozen formats).

- [ ] **Step 0: Re-read.** A16 has landed since this plan was written. Read every line
      reference in §What exists and §Frozen formats against A16's final commit;
      amend this document where a number moved, dated, before writing a test.
- [ ] **Step 1: Write the failing tests.** In `tests/test_apikey.c` beside
      `test_creation_refuses_what_it_cannot_grant`: `jobs:submit` parses, renders after
      `decisions:dispose`, `atlas_apikey_scope_grantable` is false; `api-key create
      --scope jobs:submit` is refused with the frozen sentence; the `--no-scopes`
      success block carries the amended second and third lines and A16's test of the
      old wording is updated, not duplicated. In `tests/test_orch_model.c`: a policy
      text with `submitter_uid = 992` parses ACTIVE — the loader has no opinion — with a
      comment saying the refusal of that line is this plan's and the deployment's
      (§Global constraints), and that this test exists so nobody adds a parser check
      there and calls it the boundary.
- [ ] **Step 2: Run and watch them fail.**
- [ ] **Step 3: Implement.** Append the scope with the header comment in §Frozen
      formats; the `SCOPES[]` row `{ATLAS_SCOPE_JOBS_SUBMIT, "jobs:submit", false}` with a
      comment naming Decision 1; the refusal by name in `atlas_apikey_create_on`; the
      two bounds; the amended block.
- [ ] **Step 4: Run `test_apikey`, `test_orch_model`, `test_gw_remote`**; `make` with
      zero warnings.
- [ ] **Step 5: Commit** — `feat(a14): a scope nothing can be granted, derived for the credentials a root-owned line names`

---

### Task T2: migration 32, and the database layer that reads, writes and counts what it adds

**Files:**
- Create: `tests/test_migrate32.c`
- Modify: `src/db/migrate.c`, `src/db/db_orch.c`, `include/atlas/orch_ops.h`,
  `tests/CMakeLists.txt`, `tests/test_orch_run.c`

**Interfaces produced:** migration 32 (§Frozen formats); `atlas_orch_job_view` and
`atlas_orch_list_row` gain `submit_key_id`; `atlas_db_orch_job_get` and
`atlas_db_orch_job_list` fill it; `atlas_db_orch_job_list_by_key(db, key_id, after,
limit, cb, ud, …)` and `atlas_db_orch_job_list_remote(db, after, limit, …)` (every row
with a non-empty key, for the operator); `atlas_db_orch_remote_active_count(db, key_id,
out)` and `atlas_db_orch_remote_today_count(db, key_id, utc_day_start, out)`; the
transition writer takes `key_id`.

- [ ] **Step 1: Write the failing tests.** `tests/test_migrate32.c`, `unit` label, on
      `tests/test_migrate31.c`'s shape: a fresh database reaches 32; a database stopped
      at 31 with three jobs and their transitions reaches 32 with every pre-existing
      column byte-identical, every job reading `submit_key_id = ''`, every transition
      `key_id = ''`; the index exists by name; `pragma_foreign_key_check` is empty. In
      `tests/test_orch_run.c`, extend `test_the_sql_terminal_set_matches_the_c_one`
      (`:589-627`) so the active-count query's literal is the third spelling it
      compares. Counting tests in `tests/test_orch_remote.c` (T3) drive the two count
      functions through real rows.
- [ ] **Step 2: Run and watch them fail.**
- [ ] **Step 3: Write `M32_*`** exactly as §Frozen formats states, with the migration
      comment it specifies; append the `MIGRATIONS[]` row; the db layer.
- [ ] **Step 4: Run `test_migrate32`, `test_migrate31`, `test_orch_run` and the unit
      label**; `make`. Every existing transition writer passes `""` for now.
- [ ] **Step 5: Commit** — `feat(a14): migration 32 -- the job and the ledger say which credential queued it, beside which uid`

---

### Task T3: the write point — verification in the transaction, the two budgets, the namespace, cancel ownership, follow-up inheritance, the operator flag

**Files:**
- Create: `src/orch/remote.c`, `include/atlas/orch_remote.h`, `tests/test_orch_remote.c`
- Modify: `include/atlas/orch_ops.h`, `src/db/db_orch.c`, `src/daemon/writer.c` (the
  result block copies `key_id`, `remote_active`, `remote_today` field by field —
  `docs/extending.md` "Extending A8 safely" says every new writer payload crosses that
  block), `CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces produced:** the op and result members (§Frozen formats);
`atlas_orch_remote_verify`; `atlas_orch_remote_idempotency_key`.

Inside `op_submit`, for an op with `remote_allowed_count > 0`, in this order and
nowhere else: `atlas_orch_remote_verify` **first** (refuse with the two sentences;
store `remote_key_id`); then `atlas_orch_remote_idempotency_key` over
`remote_key_id` and `remote_client_key` into `spec.idempotency_key` when the client
part is non-empty (refuse with the frozen sentence otherwise); then the existing shape
checks, the digest and the idempotency lookup exactly as today; then, **only when a
row would be created**, the active count against `remote_max_active`, the today count
against `remote_max_per_day`, and the insert with `submit_key_id` and the ledger row
with `key_id` and the frozen detail. A local op (`remote_allowed_count == 0`) takes
the path it takes today, unchanged. `spawn_follow_up` copies `submit_key_id` into the child.
`op_cancel`: when `op->remote_allowed_count > 0`, verify and refuse `no such job` unless
`j.submit_key_id == remote_key_id`; when `op->peer_is_operator` and the job has a key,
allow; otherwise the existing check. `atlas_orch_op_free` wipes `remote_token`.

- [ ] **Step 1: Write the failing tests** in `tests/test_orch_remote.c`, `integration`
      label, driving `atlas_orch_apply` directly on a fixture with one registered
      repository, an orchestration policy parsed through the seam, and three real keys
      minted by the CLI — `model` (`--scope repo:read`), `browser` (`--no-scopes`) and
      `other` (`--scope repo:read`, not named):
      (a) a SUBMIT with `model`'s token, allowed = {model, browser}, `max_active = 2`,
      `max_per_day = 3`: succeeds; the row reads `submit_key_id = <model>`,
      `submitter_uid = <the op's peer_uid>`; the ledger row reads `key_id`, actor
      CLIENT, the frozen detail; `result.key_id`, `remote_active = 1`, `remote_today =
      1`.
      (b) `other`'s token: refused with the "not one the policy names" sentence; a
      wrong secret, an unknown selector, and `model` revoked between two submissions:
      each the "did not authenticate" sentence; **no row written in any case**.
      (c) a third submission under `max_active = 2` with two non-terminal rows: refused
      with the active sentence naming 2 and 2; complete one (through the real
      dispatcher ops as `tests/test_plan_db.c` does), submit again: succeeds.
      (d) `max_per_day = 3`: the fourth root submission in one UTC day is refused with
      the today sentence; a row with `created_at` of yesterday (inserted by hand) does
      not count.
      (e) the same key and task submitted twice with `key = "k1"`: `duplicate = true`,
      one row, the second spends no budget (counts unchanged); `browser` with `key =
      "k1"` and the same text: a *different* job (the namespace); a client key of 41
      bytes or with a space: refused with the frozen sentence; the stored idempotency
      key is `remote.<id>.k1`.
      (f) CANCEL with `browser`'s token on `model`'s job: `no such job`, row unchanged;
      with `model`'s: CANCELLED; with no token and `peer_is_operator = true` on a job
      with a key: CANCELLED; with no token and `peer_is_operator = false` and a
      different `peer_uid`: `no such job`.
      (g) a repo-tree remote submission (driver `fake-repo`, one gate) whose worker
      fails its gate, completed through the write point as `test_a11_run.c` does:
      the follow-up row carries the parent's `submit_key_id`; the active count is 1
      (the follow-up) and the today count is still 1.
      (h) a local SUBMIT (no token, `remote_allowed_count = 0`) is unchanged: empty key,
      the existing detail, byte for byte.
      (i) `atlas_orch_op_free` leaves the token bytes zero (the A16 T3 shape, or
      inspection stated in the comment).
- [ ] **Step 2: Run and watch them fail** for the right reasons (link failure first).
- [ ] **Step 3: Implement.** `src/orch/remote.c` says at its head why it is a third copy
      (Decision 2). Neither new file may contain the two decision-layer needles.
- [ ] **Step 4: Run `test_orch_remote`, `test_orch_lifecycle`, `test_a11_run`,
      `test_orch_run`, `test_decision_mcp`, `test_review_apply`**; `make`.
- [ ] **Step 5: Commit** — `feat(a14): the write point verifies the credential in the transaction, counts its budget, and records which credential queued the job`

---

### Task T4: the eight policy keys, the template, the status lines

**Files:**
- Modify: `include/atlas/gwpolicy.h`, `src/gw/gwpolicy.c`, `src/gw/gateway.c` (status,
  human and JSON, through A16's `atlas_service_gateway_status_for`),
  `deploy/a9/gateway.conf.template`, `tests/test_gateway.c`

**Interfaces produced:** the `atlas_gwpolicy` members; the keys with their grammar and
every MALFORMED condition; the `submit:` and `clear-submit:` lines.

- [ ] **Step 1: Write the failing tests** in `tests/test_gateway.c` inside
      `test_every_malformed_policy_disables_the_gateway`'s matrix (`:71`) and beside the
      A16 cases: a complete policy with `tls_mode = REVERSE_PROXY` and all seven lines →
      ENABLED with every field as written, two keys stored without `key_`, one gate
      line verbatim; each MALFORMED case from §Frozen formats, one text each — bad key
      shape, duplicate key, key equal to `remote_dispose_key`, five keys, key without
      driver, driver without key, empty gate, gate with `/` in its first token, nine
      gates, `max_attempts = 0`, `= 6`, `max_active = 9`, `max_per_day = 65`, `= 0`,
      two driver lines, keys with `tls_mode = NONE` and no acceptance,
      `web_gui_anonymous_scopes = jobs:submit`, `operator_accepts_cleartext_submission
      = no` / `= true` / `= 1`, the acceptance under `REVERSE_PROXY`, the acceptance
      with no submit key, and **`operator_accepts_cleartext_disposal = yes` with
      submission lines and no submission acceptance → MALFORMED** (one acceptance does
      not imply the other); a policy with `web_gui = no` and submission lines →
      ENABLED. Status: `submit:` and `clear-submit:` in both wordings, human and JSON,
      asserted by needle (`submit:  key_`, `checked at submit`, `clear-submit: ACCEPTED`,
      `(not accepted`), never by whole line.
- [ ] **Step 2: Run and watch them fail.**
- [ ] **Step 3: Implement.** Rename A16's `parse_dispose_key` to `parse_key_selector`
      (two callers now; one grammar); the branches; the end-of-parse cross-checks
      beside A16's; the template block; the status lines after `clear:`.
- [ ] **Step 4: Run `test_gateway`**; `make`; run `./build/atlas gateway status` on this
      machine and confirm `submit:  (none -- …)` — today's policy must still load
      ENABLED.
- [ ] **Step 5: Commit** — `feat(a14): eight root-owned keys decide who may queue a job and what a job is, and a request decides none of it`

---

### Task T5: the daemon — the group, the predicate, the derivation, the four methods, the operator's view, the second injection channel

**Files:**
- Create: `src/ipc/server_orch_remote.c`, `tests/test_orch_remote_rpc.c`
- Modify: `src/ipc/server_gw.c` (the derivation in `method_gateway_auth`),
  `src/ipc/server.c` (the additive consult after A16's remote group),
  `src/ipc/server_internal.h`, `src/ipc/server_orch.c` (`job.get/list/artifact/cancel/
  run_status` widened for the operator peer; `job.list`'s `remote`; the head comment's
  new paragraph), `src/daemon/daemon.c`, `include/atlas/daemon.h`,
  `tests/tools/atlas_gw_daemon.c` (A16's; the third argument), `src/core/service_orch.c`
  and `src/cli/cli.c` + both renderers (`atlas job list --remote`, `credential:`),
  `tests/test_a7_authority.c`, `tests/test_orch_rpc.c`, `CMakeLists.txt`,
  `tests/CMakeLists.txt`

**Interfaces produced:** §Frozen formats "The daemon method group";
`atlas_daemon_opts.orchpolicy_text`; the CLI additions.

`method_remote_submit`: asks the predicate again; `require_remote_submitter` (the
orchestration policy ENABLED, else `orch_disabled`); reads `repo`, `task`, `key`,
`token` and refuses any other parameter name present with the frozen sentence; resolves
the repository exactly as `method_job_submit` does (policy first, then the registry,
then `scanned_head`); cross-checks `remote_submit_driver` / `_mode` / `_max_attempts` /
`live_model` against `ctx->orchpolicy` with the frozen sentences; splits every gate
line with `atlas_orch_gate_split` and pushes each argument through
`atlas_orch_argv_push`, refusing with `remote gate %zu could not be split: %s`; builds
the op with `peer_uid = ds->peer_uid`, `actor = CLIENT`, `spec.submitter_uid =
ds->peer_uid`, `memory_mode = OFF`, `run_max_parallel = 0`, `spec.max_attempts =
ctx->gwpolicy.remote_submit_max_attempts` (so `atlas_orchpolicy_apply_limits` refuses
one above the orchestration ceiling with its existing sentence, `max_attempts of %lld
exceeds the orchestration policy ceiling of %lld`, and no second sentence exists),
`remote_client_key` from `key` with `spec.idempotency_key` left empty (the write point
namespaces it, Decision 6), `remote_token`, the allowed list and the two bounds copied
from `ctx->gwpolicy`;
submits through `orch_write` with the syspolicy exactly as `method_job_submit` does;
writes the frozen response. `method_remote_get/list`: verify on `ds->db`, scope by key
id. `method_remote_cancel`: build a CANCEL op with the token and the list.

- [ ] **Step 1: Write the failing tests.** `tests/test_orch_remote_rpc.c`, `daemon`
      label, on `tests/test_plan_rpc.c`'s edge: `ctx.orchpolicy` names `submitter_uid =
      <owner>`, `model_dispatcher_uid = <owner>`, drivers `fake fake-repo`, `mode =
      patch`; `ctx.gwpolicy` names `gateway_uid = <gw>` (a uid the test chooses,
      distinct from `owner`), two submit keys minted by the CLI, `driver = fake`,
      `mode = patch`, one gate `true`, attempts 1, active 2, per day 3, `tls_mode =
      REVERSE_PROXY`; dispatch as `gw`:
      (a) `job.submit` as `gw` → "may not submit" (the orchestration policy does not
      list it — asserted first, because it is the sentence the season keeps true);
      `job.remote_submit` as `gw` with the model key's token → a QUEUED job; the
      response carries `key_id`, `budget`; `gateway.auth` with that token → scopes
      include `jobs:submit`; with the unnamed key → its stored list only.
      (b) the four names as `owner` → `unknown method`; as `gw` under a gwpolicy with
      no submit key → `unknown method`; under keys but `tls_mode = NONE` and no
      acceptance → `unknown method`; with the acceptance → offered; under a zeroed
      `ctx.orchpolicy` → `orch_disabled`'s sentence.
      (c) a request carrying `driver`, `mode`, `validation`, `parallel`, `memory` or
      `parent` → the frozen refusal naming the parameter, nothing queued.
      (d) `gwpolicy.remote_submit_driver = claude` with an orchestration policy that
      lists only `fake` → the driver sentence; `live_model = off` with a model driver →
      its sentence; `remote_submit_max_attempts = 4` against an orchestration ceiling
      of 3 → `atlas_orchpolicy_apply_limits`'s existing sentence, byte for byte.
      (e) the full path with `fake`: submit as `gw`, then as `owner` `dispatch.lease`
      with filter `fake`, heartbeat, complete → `job.remote_get` as `gw` reads
      SUCCEEDED with `reason`, `usage.present = false`; `job.remote_list` shows it;
      the other key's `job.remote_get` on it → `no such job`; `job.remote_cancel` from
      the other key → `no such job`; from the submitting key on a queued second job →
      CANCELLED.
      (f) `job.list` as `owner` with `remote = true` → both rows (the edge marks the
      operator group locked, so assert this through the write-point flag in
      `test_orch_remote.c` (g) and note here that the RPC form is verified on the
      deployment in T11).
      In `tests/test_a7_authority.c:204-230`, add the four names to `METHODS[]` (a
      fixture daemon with a zeroed gateway policy must answer `unknown method`). In
      `tests/test_orch_rpc.c`, add `job.remote_apply`, `job.remote_artifact`,
      `job.remote_log`, `job.remote_run` to `FORBIDDEN_METHODS[]`; extend the namespace
      assertion to the third table; extend `VERBS[]` with `artifact` and `log` for the
      remote table only.
- [ ] **Step 2: Run and watch them fail.**
- [ ] **Step 3: Implement.** The file's head comment places it beside
      `server_orch.c:1-34`'s argument and says why the group is a third table and why
      `require_submitter` is never called. The consult in `dispatch()` goes after A16's
      remote disposal consult. The `orchpolicy_text` branch at `daemon.c:212` on A16's
      shape. **Then amend the sentences this task makes false**: `server_orch.c:274-277`
      ("There is no MCP tool and no gateway route that reaches this method" — true of
      `job.submit`, and the paragraph now points at the remote group and says what it
      cannot name), `server_internal.h:213-222` ("It cannot … run a job" stays; add
      "and may queue one only while a request carrying a named credential is in
      flight"), and the group table in `docs/orchestration.md` (T10).
- [ ] **Step 4: Run `test_orch_remote_rpc`, `test_orch_rpc`, `test_a7_authority`,
      `test_plan_rpc`, `test_gw_remote`**; `make`.
- [ ] **Step 5: Commit** — `feat(a14): the daemon offers four job methods to the gateway's uid under policy, each carrying a credential it verifies, and none of them a dispatch`

---

### Task T6: the gateway — four bearer-table rows, the per-row body bound, the token into the MCP server, `/auth/me`

**Files:**
- Create: `tests/test_gw_submit.c` (this task writes the HTTP half; T7 adds the tool half)
- Modify: `src/gw/gateway.c`, `include/atlas/gateway.h`, `include/atlas/limits.h`,
  `src/mcp/mcp_internal.h`, `src/mcp/mcp.c`, `tests/test_gateway.c`,
  `tests/test_gw_remote.c`, `tests/CMakeLists.txt`

**Interfaces produced:** the four rows; `api_route.body_max`; `route_offered`; the
`/auth/me` fields; `mcp_exchange`'s token parameter and `atlas_mcp_server.remote_token`.

- [ ] **Step 1: Write the failing tests.** In `tests/test_gateway.c`, widen A16's
      write-table property test: every row's scope is `DECISIONS_DISPOSE` or
      `JOBS_SUBMIT`, ungrantable, its method on a positive list of six names, its
      `body_max` non-zero, and the read table's rows all have `body_max == 0`. In
      `tests/test_gw_submit.c`, `daemon` label, serialised: start `atlas-gw-daemon` with
      a gateway policy naming two submit keys (`REVERSE_PROXY`) **and** an
      orchestration policy naming the test's uid as model dispatcher and `fake` as the
      driver; open a gateway on the same policy text:
      (a) `POST /api/v1/job/submit` with the model key as bearer and a form body → 200
      with `job`, `key_id`, `budget`; a 70 000-byte task → 200 (it fits the submit
      bound); a 262 145-byte body → 413; `/api/v1/job/get` → the job; `/list` → one
      row; `/cancel` on a queued second job → CANCELLED; two `gw_audit` rows per call
      with `operation` = the path, `key_id` = the selector, no task text and no token
      bytes anywhere in the audit table.
      (b) refusals: `GET` → 405; JSON content type → 415; no `Authorization` → 401
      with and without a valid session cookie from a login with the browser key (**the
      cookie must not help**); the anonymous floor named plus a matching `Host` → 401;
      an unnamed key as bearer → 403 with the scope sentence; a body naming `driver=`
      → dropped by the row, and the daemon accepts (assert the stored driver is the
      policy's); a policy with no submit key → 404; a malformed percent-escape → 400;
      a job the other key submitted → `no such job`, with whatever status
      `api_handle`'s mapping gives `ATLAS_ERR_USAGE` (read the table after A16's T6
      and pin that number in the test; this plan does not guess it — the sentence is
      USAGE everywhere it is produced, and A16's 409 is for INTEGRITY).
      (c) `/auth/me` reports `remote_submission: true`, `remote_submission_driver:
      "fake"`, `cleartext_submission: false` under `REVERSE_PROXY`, and `true` under a
      policy carrying the acceptance; `false`/`""`/`false` under `gui_env`'s.
      (d) the model key over `/mcp`: `tools/list` includes the four names and
      `atlas_search`; the browser key (`--no-scopes`) lists exactly the four; an
      unnamed key lists none of the four and `tools/call` on each answers the scope
      sentence.
- [ ] **Step 2: Run and watch them fail.**
- [ ] **Step 3: Implement.** The rows; `body_max` on the struct with A16's rows carrying
      4096; `route_offered` with no `default:`; the `/auth/me` fields; the `/mcp`
      handler parsing the bearer into a stack buffer after `authenticate`, handing it
      to `mcp_exchange`, wiping it; `mcp_exchange` setting `s.remote_token` and
      `atlas_mcp_server_teardown` wiping it. Amend the `API_WRITE_ROUTES[]` comment
      (Decision 9).
- [ ] **Step 4: Run `test_gateway`, `test_gw_remote`, `test_gw_submit`, `test_gw_dispose`**;
      `make`.
- [ ] **Step 5: Commit** — `feat(a14): four routes whose only credential is the bearer on the request, and a body bound a task can fit`

---

### Task T7: four remote-only MCP tools

**Files:**
- Modify: `src/mcp/mcp_tools.c`, `include/atlas/mcp.h`, `tests/test_gw_submit.c`,
  `tests/test_mcp.c`, `tests/test_plugin.c`, `tests/test_gw_remote.c`,
  `tests/test_decision_mcp.c`

**Interfaces produced:** the four tools (§Frozen formats); `tool_def.remote_only`;
`atlas_mcp_tool_remote_only`.

- [ ] **Step 1: Write the failing tests.** In `tests/test_gw_submit.c`: `tools/call
      atlas_job_submit` with `{repo, task, key}` as the model key → a job; the same call
      again → `duplicate: true`; `atlas_job_status` → SUCCEEDED after the test completes
      it; `atlas_job_list` → one row; `atlas_job_cancel` on a queued one → CANCELLED; a
      call with an undeclared argument (`driver`) → the schema refusal; the emitted
      `tools/list` document declares none of the forbidden properties. In
      `tests/test_mcp.c:250`: every name `atlas_mcp_tool_names()` returns that is
      `remote_only` is **absent** from the stdio listing, and `tools/call` on it locally
      answers `unknown tool`. In `tests/test_plugin.c:414`: 41, with the paragraph. In
      `tests/test_gw_remote.c:279`: the four names for a credential holding every
      grantable scope → the scope sentence. `tests/test_decision_mcp.c:79-81` must
      still pass over the four names.
- [ ] **Step 2: Run and watch them fail.**
- [ ] **Step 3: Implement.** The four schema and run functions on `run_search`'s shape;
      `make_params` gains `token` from `s->remote_token`; the `tool_def` comment
      amended (Decision 10).
- [ ] **Step 4: Run `test_mcp`, `test_plugin`, `test_gw_remote`, `test_gw_submit`,
      `test_decision_mcp`**; `make`.
- [ ] **Step 5: Commit** — `feat(a14): four tools a named credential may call over the network and no local session can see`

---

### Task T8: Mission Control's Jobs view

**Files:**
- Modify: `src/gw/ui/mission-control.html`, `tests/test_gw_remote.c`

- [ ] **Step 1: Write the failing test** (`test_mission_control_carries_the_jobs_view`):
      fetch `/` and require `["jobs","Jobs"]`, `submitKey`, `job/submit`, `job/get`,
      `job/list`, `job/cancel`, `remote_submission`, `remote_submission_driver`,
      `cleartext_submission`, `maxlength="65536"`, `maxlength="40"`, and the needles
      `records the channel and the credential, not a person`, `a task is a prompt`,
      `read on the atlas machine`, `does not serve remote submission`, `crosses the
      network in the clear from a browser`; require the absence of `innerHTML` and of
      the bare phrase `proves`. State that no JavaScript is executed.
- [ ] **Step 2: Run and watch it fail.**
- [ ] **Step 3: Write the view** per Decision 16, reusing `el`, `kv`, `tag`, `statusTag`,
      `panel`, `say`, `apiWrite`.
- [ ] **Step 4: Run the test**; `make`.
- [ ] **Step 5: Commit** — `feat(a14): Mission Control queues a task from the browser, with what a task is on screen`

---

### Task T9: the tripwires learn the new surfaces

**Files:**
- Modify: `tests/test_decision_mcp.c`, `tests/test_gateway.c` (if T6 did not),
  `tests/test_orch_rpc.c` (if T5 did not)

- [ ] **Step 1: Extend `FILES[]`** (`test_decision_mcp.c:391-414`) with
      `src/ipc/server_orch_remote.c`, `src/orch/remote.c`, `include/atlas/orch_remote.h`,
      `docs/remote-submission.md`, `docs/orchestration.md`, `docs/remote-access.md`.
      A forbidden phrase found is removed from the source, never from the list.
- [ ] **Step 2: Extend the required-wording table** (`:433-455`) with
      `{mission-control.html, "records the channel and the credential, not a person"}`,
      `{mission-control.html, "a task is a prompt"}`,
      `{docs/remote-submission.md, "a task is a prompt"}`,
      `{docs/remote-submission.md, "runs as the operator"}`,
      `{SECURITY.md, "runs as the operator"}`,
      `{docs/orchestration.md, "not what one task may ask a worker to read"}`,
      `{src/ipc/server_orch_remote.c, "never consults"}` (the `require_submitter`
      sentence in its head comment), and — the cleartext chain —
      `{SECURITY.md, "does not cross that segment"}`,
      `{docs/remote-access.md, "does not cross that segment"}`,
      `{docs/remote-submission.md, "does not cross that segment"}`.
- [ ] **Step 3: The caller-count scans** stay at three and two; add to each comment
      that A14 came here and touched nothing in the decision layer.
- [ ] **Step 4: Run `test_decision_mcp`, `test_review_apply`, `test_gateway`,
      `test_orch_rpc`, `test_a7_authority`, `test_plugin`.**
- [ ] **Step 5: Commit** — `test(a14): the new files and documents join the tripwire, and the two decision counts did not move`

---

### Task T10: documentation, the season rules, the roadmap's ordering

**Files:**
- Create: `docs/remote-submission.md`
- Modify: `docs/roadmap.md`, `docs/orchestration.md`, `docs/remote-access.md`,
  `docs/engineering-rules.md`, `docs/extending.md`, `docs/backlog.md`, `SECURITY.md`,
  `README.md`, `CLAUDE.md`

- [ ] **Step 1: `docs/remote-submission.md`** — the season's document in
      `docs/browser-disposal.md`'s shape: the two sentences; **the honest paragraph
      from the top of this plan, in the paragraph that announces the capability**;
      what is true today in the precise form (the four rows, the four methods and their
      three conditions, the eight keys, the two columns, the four tools); the seventeen
      decisions with their chains; the two shapes and which one this deployment runs;
      the frozen formats by reference; the stated costs (Decision 2's three copies,
      Decision 7's transcript chain, Decision 10's `operation = "mcp"`, Decision 12's
      two files that can disagree, Decision 13's fixture limit, Decision 14's
      never-settling run, the pin under the deferred shape); the cleartext chain
      verbatim with the operator's answers to all three rows quoted and dated; and, at
      the end, what execution established that the plan did not claim.
- [ ] **Step 2: `docs/roadmap.md`** — retitle "Next: A14 …" to "A14 — … (shipped)",
      keeping the "never started until 2026-09-…" history paragraph and adding the date
      it started; replace "**no surface outside the local socket can submit a job**"
      (`:1550-1552`) with what is now true; fold the three shapes into one paragraph
      saying which two were built as one mechanism and why the broker was rejected;
      add the A14 row to `CLAUDE.md`'s table via Step 9. Every sentence against the
      tripwire list.
- [ ] **Step 3: `docs/orchestration.md`** — the RPC-surface table (`:218-240`) gains a
      third group with its three conditions and the sentence "never `require_submitter`";
      the policy section (`:257-284`) gains one paragraph saying the gateway policy
      names a driver and a mode this file is the authority on, and where the two are
      cross-checked; the digest table (`:68-89`) gains a row `submit_key_id | no |
      travels on the op; the credential is not part of what was asked for`; "Where the
      two drivers run" (`:826-846`) gains the two remote shapes; "Status" (`:1158-1196`)
      amends "the only submitter was the operator". **Amend `:1170-1177`** ("That absence is the deferral" and "Their
      absence is the deferral") to add that remote submission is present and remote
      execution of a repo-tree task is still absent.
- [ ] **Step 4: `docs/remote-access.md`** — amend "What the gateway cannot do"
      (`:48-66`): "every `job.` and every `dispatch.` method" becomes "every `dispatch.`
      method, and every `job.` method but the four `job.remote_*` names offered under
      a root-owned policy, each of which does nothing without a named credential in
      flight"; keep "run a job"; a new section "A14: remote submission" after the A16
      section with the routes, the tools, the keys, the derived scope, the two
      acceptance keys and why they are two, the `submit:` lines, the `/auth/me`
      fields, the honest paragraph and the cleartext chain verbatim; amend "Connecting
      ChatGPT" (`:257-270`): "The credential must be read-only … structurally cannot"
      becomes true only of a credential no `remote_submit_key` names, and says so.
- [ ] **Step 5: `docs/engineering-rules.md`** — "A14 layers — additions" and "A14 rules
      — these are not negotiable" in full, from §Authority argument, after the A16
      section. **`docs/extending.md`** — "A14 — remote submission": adding a bearer-table
      row (the positive list, an ungrantable scope, a body bound, `route_offered`'s
      switch); adding a remote job method (the predicate, the token, never
      `require_submitter`); adding a submission policy key (grammar, MALFORMED, the
      cross-check, the restart); adding a remote-only tool (the flag, the two test
      lists, the count); adding a per-credential bound; and "Bounds this season added".
- [ ] **Step 6: `docs/backlog.md`** — entries for: three copies of one credential
      check; a remote `tools/call` audits as `mcp`; gate additions from a remote
      request; per-key drivers (one credential deferred, another unattended); a
      remote artifact read, with Decision 7's chain as the bill; a daily bound keyed on
      a clock; the two policy files that can disagree; and the never-settling
      workspace run one layer over.
- [ ] **Step 7: `SECURITY.md`** — amend `:477-484` as Step 4 amends remote-access; a new
      section "A14: remote submission, and what it is worth" after A16's: what it
      establishes (a named credential was presented and verified in the transaction; a
      job of the policy's shape was queued), what it does not establish (who or what
      presented it; that a person meant it; anything about the prompt), what it
      excludes (a session, the floor, any unnamed key, any driver/mode/gate/attempt
      the request chose, any `dispatch.` name, any artifact read), the honest
      paragraph, and the cleartext chain verbatim. The chain's two hedges — "anyone
      able to observe traffic on that network segment" and "does not cross that
      segment" — are deliberate and are not to be sharpened. `README.md` — the eight
      keys in the gateway section, the four tools in the command-line section's MCP
      note, one sentence under Mission Control, one under limitations.
- [ ] **Step 8: `CLAUDE.md`** — the season paragraph at the top in the register the
      others use ("The current work is **A14** … **A14 added migration 32.**" and both
      sentences), the table row, the one-line rules under "### A14 — remote submission",
      the honest paragraph in the season paragraph itself, and `docs/remote-submission.md`
      in "Where things are documented". Every line against `test_decision_mcp.c:371-386`.
- [ ] **Step 9: Run `test_decision_mcp`** and the full suite.
- [ ] **Step 10: Commit** — `docs(a14): remote submission -- what was built, what a credential in flight is worth, and what a task is`

---

### Task T11a (only if row 2 was answered no): the terminator, recovered from `c305f40`

Recover A16's removed T10 verbatim from `c305f40`, with two changes: the tunnel's
`server_urls` entry is re-pointed to the loopback listener through the proxy, and the
acceptance keys are absent. Its own steps, costs and observations are in that commit
and are not restated here.

---

### Task T11: live acceptance — the policy, the dispatcher, one free job, one real one

Run by the reviewing session with the operator acting. **It spends money only at
Step 5, once, on the operator's contemporaneous go-ahead**, and changes this machine's
policy.

- [ ] **Step 1: Full suite, then deploy** with `/opt/atlas/deploy.local.sh` after
      changing its `MARK` (`:17`) to a string only A14's binary contains
      (`jobs:submit`). The daemon restarts; `atlas daemon ping --json` reports
      `phase: "A14"` once `include/atlas/atlas.h` says so.
- [ ] **Step 2: The credentials and the policy, with `fake` first.** `atlas api-key
      create --label browser-submit --no-scopes` as the operator; the secret goes into
      the operator's password manager. Edit `/etc/atlas/gateway.conf` — listener and
      `tls_mode` unchanged — adding the eight lines from row 3 **with
      `remote_submit_driver = fake`**, and the acceptance line per row 2. Restart
      `atlas.service` and `atlas-gateway.service`. `atlas gateway status` reads
      `submit:  key_… key_… (driver fake, …)` and the `clear-submit:` line; record the
      lines. **Confirm `grep submitter_uid /etc/atlas/orchestration.conf` prints
      exactly one line and it is `1000`** — the acceptance table's check that the
      forbidden line is absent.
- [ ] **Step 3: The free job, from the browser.** Jobs view on the phone; paste the
      browser key; submit a task against `atlas`; observe the job appear; **it will
      not run** — `fake` is leased by `atlas-dispatcher.service` (993), which is
      running, so it completes at zero cost; observe SUCCEEDED in the view, in `atlas
      job list --remote` on the terminal with `credential: key_…`, in `atlas job get`,
      and in `journalctl -u atlas` (the accepted line, no task text). Then the free job
      **from the model**: ask the external model to call `atlas_job_submit` with a
      small task and a key; observe the job; ask it for `atlas_job_status`; observe
      that the Audit view shows `REMOTE_MCP / mcp` rows and nothing more specific
      (Decision 10's stated cost), and that the daemon log names the submission.
      Cancel one queued job from each surface. Record every observation as an
      observation.
- [ ] **Step 4: The driver the operator chose.** Edit `remote_submit_driver` to row 1's
      answer; restart both services; `atlas gateway status` reads it back. If `claude`:
      install `deploy/a8/atlas-model-dispatcher.service` to
      `~/.config/systemd/user/`, `systemctl --user enable --now
      atlas-model-dispatcher`, and `loginctl enable-linger` if the unit must outlive a
      login — **by the operator's hand, never by a test or by Atlas' code** — and
      record that a background process now runs as the operator's account and will
      lease every model job the queue holds, remote or local. If `claude-repo`:
      nothing is installed.
- [ ] **Step 5: One real job.** Ask the operator, in the moment, whether to spend one
      start (§Worst-case cost: $5–11 by the figures there); record the answer. If yes:
      from the model, a small real task against `atlas` (one from `docs/backlog.md`
      that a patch can answer); on `claude`, watch it lease, run, gate and end; read
      the patch on the machine with `atlas job artifact`; read its cost with `atlas
      job get` and in the Jobs view; on `claude-repo`, `atlas job list --remote`, then
      `atlas job run --resume RUN` at the terminal, and observe the pin check pass or
      refuse. **Nothing about one live pass is a general result; say so in those
      words.**
- [ ] **Step 6: Record the observations** in `docs/remote-submission.md` with the date,
      the device, the client, the cost the provider reported, and anything the tests
      could not have seen.
- [ ] **Step 7: Final commits. Nothing is pushed on this document's authority.** Present
      the season's commit list, ask, and push only on the operator's contemporaneous
      go-ahead — recording the answer either way.

---

# Acceptance — the brief's requirements, mapped

| # | Requirement | Discharged by | The assertion that proves it |
| --- | --- | --- | --- |
| 1 | the sentence stays true: what the gateway cannot do is because of who it runs as; no check in `src/gw` is the boundary | Decisions 1, 2, 8; T5 | `job.submit` as the gateway uid refused by the orchestration policy; `job.remote_submit` refused for an unnamed or wrong credential *by the daemon*; `submitter_uid` in `orchestration.conf` is one line, `1000`, checked in T11 step 2; `require_submitter` has no caller on the remote path |
| 2 | its own ungrantable scope and its own root-owned line; the disposal scope never widened | Decision 1; T1, T4 | `grantable == false`; `api-key create --scope jobs:submit` refused; a policy naming one key in both lines is MALFORMED; `decisions:dispose` is derived for no key a submit line names |
| 3 | the credential verified by the daemon itself, inside the transaction for writes | Decision 2; T3, T5 | a wrong secret, an unknown selector, an unnamed key and a key revoked between two submissions are each refused in `op_submit` with no row; `job.remote_get` for another key's job is `no such job` |
| 4 | a budget per key | Decision 5; T3 | the active and daily refusals with counts named; a duplicate spends none; a follow-up counts toward active and not daily |
| 5 | A9's absences stay absences | Decisions 7, 8, 10; T5, T7, T9 | `job.remote_apply/artifact/log/run` are `unknown method`; no tool name carries a lifecycle verb; no schema declares `token`; the remote group holds no `dispatch.` name |
| 6 | the audit row names the key, never a claimed value | Decisions 3, 17; T3, T6 | `gw_audit.key_id` is the verified selector on every route; `submit_key_id` and the ledger's `key_id` are written by the write point; no audit row holds task text or a token |
| 7 | a decision on cleartext, argued | §The decision on cleartext; T4, T6, T10 | a submission line with `tls_mode = NONE` is MALFORMED without its own acceptance and MALFORMED with only the disposal acceptance; `gateway status` prints `clear-submit:`; `/auth/me` reports it; the chain is verbatim in four documents with both hedges |
| 8 | the model-submits case reasoned about | Decisions 1, 4, 7; the honest paragraph; T7, T8, T10 | a request naming driver/mode/gates/attempts is refused; no artifact or log travels remotely; the transcript chain is in the documents and the page; the tools are absent locally |
| 9 | `submitter_uid` found and its uses kept true | §What exists 2; Decision 3; T2, T3 | every one of the eleven uses reads the kernel's uid unchanged; the digest domain is unchanged; the credential is a column |
| 10 | A16 dependencies named, not assumed | §What A16 leaves | every A16 mechanism A14 uses is named with its status at `c06e0a8`; T1 step 0 re-reads |
| 11 | worst-case cost including money | §Worst-case cost | the formula, the measured $5.13/start, the extrapolation labelled as one, the table at the proposed lines, and T11's single paid start on a recorded go-ahead |
| 12 | the operator asked before, not after | §Decisions the operator must be asked | three rows, each in plain words about the thing itself, each before T1; the answers recorded in `docs/remote-submission.md` |
| 13 | the tempting fix not taken | §Global constraints; T1, T11 | `tests/test_orch_model.c`'s case says whose refusal it is; T11 step 2 checks the file |
| 14 | one real submission from the external model on this deployment | T11 | a job queued by `atlas_job_submit` through the tunnel, observed in the Jobs view and on the terminal, with its cost reported |

---

# Self-review (planner)

**1. Spec coverage.** The brief's fact (uid, `SO_PEERCRED`, `unknown method`) is
§What exists 1 and acceptance row 1; the scope paragraph with its why is Decision 1;
the cleartext decision is its own section with the tunnel finding the brief did not
have; the model-submits case is Decisions 1, 4 and 7 and the honest paragraph;
`submitter_uid` is §What exists 2 and Decision 3; A16's two mechanisms are the
dependency table; the roadmap's three shapes are decided with the broker rejected by
chain; the three "must be true" are the first three non-negotiables; the money is
tabulated; the number is kept with the reason; the operator's decisions are in a
section of their own with an "ask before" column.

**2. Sentences of the recurring defect class, hunted.** Every line reference was read
at `c06e0a8` this session and paired with a function; the ones A16's T5–T7 will move
are named and T1 step 0 re-reads them. Three things this plan found that the brief and
the roadmap did not state: the tunnel credential never crosses the segment; no model
dispatcher is installed; and a remote artifact read would be a read of any file the
operator's account can read. Two things this plan deliberately does not claim: that
the deferred shape's pin will hold long enough to be useful on a repository the
operator commits to daily (row 1 says the cost; T11 observes it), and that a worker
cannot reach the network (Atlas does not bound it; Decision 7 says so).

**3. Placeholder scan.** No TBDs. Every vocabulary member, policy key, route row, tool
name and schema property, method name, request and response shape, refusal sentence,
ledger sentence, log line, status line, UI sentence, migration statement and bound is
written out. Left to the executor and named as such: C and JavaScript bodies; whether
`route_offered` is a switch or a two-entry table (a location, not a design); how the
tool daemon reads its second policy file (any bounded read).

**4. Type consistency.** `remote_allowed_ids`/`_count` are copied by the method from
`gwpolicy.remote_submit_keys`/`_count` and read by `atlas_orch_remote_verify` in
`op_submit` and `op_cancel`; `remote_key_id` is written there and copied to
`submit_key_id`, the ledger's `key_id` and `result.key_id`; the two budget members are
copied from the same policy fields the predicate and the status line read;
`ATLAS_ORCH_REMOTE_CLIENT_KEY_MAX` is the 40 in the builder, the refusal sentence, the
MCP schema and the page's `maxlength`; `ATLAS_GW_SUBMIT_BODY_MAX_BYTES` is the row's
`body_max`, the 413 case and the 262 145-byte test; the four route rows are what T6
writes, T7's tools reach and T8's page calls.

**5. What this plan does not settle, and says so (T10 step 6 records each).** Gate
additions from a remote request; per-key drivers; a remote artifact read and the
worker-authority narrowing it would first need (a service credential as
`atlas-worker`, which is A8's default and this deployment's stated departure); a
per-tool audit row over `/mcp`; folding three credential checks into one; a bound keyed
on the ledger rather than the clock; a single file holding both policies; a settling
verdict for a workspace-rooted run; and the operator-visibility RPC form, which no
fixture can prove.
