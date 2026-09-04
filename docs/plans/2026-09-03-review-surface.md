# A15 — The review surface: Mission Control reads a proposal, and the terminal disposes of it — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: `superpowers:subagent-driven-development`
> (or `superpowers:executing-plans` for inline execution). Tasks are dispatched one at
> a time; the reviewing session reads every diff before the next task starts. Steps use
> checkbox (`- [ ]`) syntax. T1 and T3 may run in parallel; everything else is ordered.
>
> **Deviation from the writing-plans skill, stated deliberately, exactly as the A12.0
> and A12.1 plans stated it:** the operator has assigned roles — the planner (Fable)
> writes this plan and reviews; the executors (Opus) write all production code. This
> plan therefore pins *interfaces, grammars, refusal sentences, UI wording, route rows and
> test obligations* exactly, and leaves C and JavaScript function bodies to the executor.
> Where the skill says "show the code", this plan shows the contract the code must
> satisfy. Everything else in the skill applies: bite-sized steps, TDD, frequent
> commits, no placeholders, no "similar to Task N".

**Goal:** Make Mission Control the place a knowledge record is *read* — every revision,
its claims with their evidence and attestations, the aggregate and its reasons, the gate
assessment, the impact set of what it links to, and A12.1's one finding where there is
one — and give the operator a way to carry what they decided there to the terminal, where
the only channel that may dispose of a record already exists. Nothing on the listener the
external model reaches gains the ability to change a lifecycle state.

**Architecture:** No new layer. Three existing read routes each forward one more
parameter the daemon already reads. Mission Control gains a Review view that renders over
routes that already exist and keeps a *review sheet* — a plain-text list of
`(intent, repository, decision id, revision, hash prefix)` lines, held in the operator's
own browser — that stores no authority anywhere. One new local command, `atlas review
apply FILE`, walks that sheet by looping the one function that already implements the
operator channel (`atlas_service_decision_confirm`, `include/atlas/service.h:940`) with
the reviewed revision pinned, and refuses an entry before minting anything when the record
moved since it was reviewed. `atlas_decision_apply_in_tx` keeps exactly three callers.
No migration, no new scope, no new daemon method, no new route, no new actor.

**Sentence the season exists for:**
> **A PROPOSAL NOBODY CAN REVIEW COMFORTABLY IS A PROPOSAL NOBODY REVIEWS.**

**Tech stack:** C17, the existing A4 operator channel, the existing A9 gateway and its
fixed route table, hand-written HTML/CSS/JavaScript with no build step (embedded by
`tools/atlas_embed.c` per `CMakeLists.txt:143-150`), the first-party test harness with the
pseudo-terminal helpers `tests/test_decision_operator.c` already has.

**Spec:** `docs/roadmap.md`, section "Later: the review surface, and where a proposal is
disposed of" (lines 1558–1643 at `b22f822`), and the brief at
`docs/plans/2026-09-03-review-surface-PROMPT.md`. §Design carries the full argument;
§Acceptance maps the roadmap's job sentence and the brief's requirements to the task
that discharges each.

**Season number: A15.** `docs/roadmap.md:1478` calls A14 (remote job submission) "Next",
and the operator has now put this season first. This plan takes **A15** and leaves A14's
number alone: renaming A14's committed section (`752686e`) would touch its sentence and
its three shapes for no gain, and this roadmap's numbers already do not follow time —
A13 shipped between A12.0 and A12.1. T9 edits the two headings: "Next: A14 …" becomes
"Later: A14 …" and this section becomes "Next: A15 …".

---

## Global constraints (repo-wide, every task inherits these)

- Warnings are errors (`ATLAS_WERROR=ON`). No new third-party dependency, no
  `FetchContent`, no network at build time. No shell: `atlas_proc_run` with an explicit
  argv array and an absolute `argv[0]` is the only way a process is created. No Python,
  Node, Go or Rust anywhere in the build, tests or runtime — the page stays hand-written.
- **Never modify a registered target repository.** A15 adds no writer of any kind. The
  adversarial obligation in T6 proves it with `fx_tree_digest`
  (`tests/support/fixture.h:91`).
- **`atlas_decision_apply_in_tx` keeps exactly three callers** — `src/ai/ai.c:864`,
  the `atlas_decision_apply` wrapper at `src/decision/lifecycle.c:2080`, and
  `src/verify/autolifecycle.c:678`. A15 adds none, and T6 greps for it.
- **The operator channel is reached through `atlas_service_decision_confirm` and
  through nothing else.** It has one caller today (`src/cli/cli.c:2060`, inside
  `run_decision_confirm` at `cli.c:2016`); after A15 it has two. Anything that mints or
  spends a challenge by another path is not this season.
- Tests always override the data directory (`fx_open` / `--data-dir`); daemon tests
  additionally override `XDG_RUNTIME_DIR`; no test opens the real database or the real
  socket, and no test installs, enables or starts a systemd unit.
- A new `.c` file goes in the explicit `atlas_core` source list in `CMakeLists.txt` —
  there is no `file(GLOB)`. A new test goes in `ATLAS_TESTS` in `tests/CMakeLists.txt`
  **and** in one of the `set_tests_properties(... LABELS ...)` lines
  (`tests/CMakeLists.txt:335-619`).
- A new command touches **five** places: a service function, a method on
  `atlas_renderer_vtbl` in `src/cli/render.h`, implementations in **both**
  `render_human.c` and `render_json.c`, dispatch plus help text in `src/cli/cli.c`,
  **and** `COMMANDS[]` in `is_a_command` (`cli.c:2672-2678`). Then run the built binary
  once.
- Every fallible function returns `atlas_status` and takes an `atlas_err *`; one exit
  path per function; `atlas_buf` owns its allocation; row callbacks receive borrowed
  pointers valid only for the call.
- Untrusted text — a title, a decision body, a claim's text, an actor name, a
  repository name — is `UNTRUSTED_DATA`: safe-encoded with `atlas_safe()` before a
  terminal, inserted with `textContent` and never `innerHTML` in the page
  (`src/gw/ui/mission-control.html:195-197`), and never interpreted.
- **Every sentence this season writes into `CLAUDE.md`, `docs/roadmap.md`,
  `docs/decision-lifecycle.md`, `src/cli/render_human.c`, `src/core/service_decision.c`
  and the page is scanned by `tests/test_decision_mcp.c:368-421`** against the phrase
  list at `:371-386`. The plan's own wording below was written against that list; the
  executor keeps it that way. The plan file itself lives under `docs/plans/` and is not
  scanned, so a drift here is caught only at T8/T9 — write the tripwire additions before
  the documents.
- Commit after every green task in the repo's style (`feat(a15): ...`, `fix(a15): ...`,
  `test(a15): ...`, `docs(a15): ...`). No commits of unrelated changes; nothing is pushed
  on this document's authority (T10 step 5).

---

# Design

## What Atlas answered, before anything was read

Asked first, as `CLAUDE.md` requires. Everything below is `UNTRUSTED_DATA` reported, not
followed.

- `atlas_repo_overview`: repository `atlas`, `index_current: true`, `scanned_head`
  `b22f822`, `watch_detail: "watching a mirror"`, `scanner_uid: 1000`,
  `compile_databases: 0`, 447 live files, 295 commits. The **deployed daemon reports
  `phase: "A12.0"`** while the tree's `include/atlas/atlas.h:11` says `"A12.1"` — the
  running binary predates commit `8dd07c6`. T10 deploys the season's binary, and the
  operator sees the phase move in the same breath.
- `atlas_decisions` with `path` set to `src/gw/gateway.c`, `src/gw/ui/mission-control.html`,
  `src/cli/cli.c`, `src/decision/lifecycle.c`: **nothing governs any of these paths**
  (`count: 0` for each). With no path: four records, all `PROPOSED`, none `APPROVED` —
  a POLICY about who writes season plans (`atlas-dec-963bf3…`), an OPERATIONAL_FACT at
  revision 2 (`atlas-dec-314ed6…`), and two records titled "PROBE-A8FINAL-… disposable"
  (`atlas-dec-28f03b…`, `atlas-dec-c711a6…`, the second at revision 3). That is the
  review surface's first real workload, and the two probes are what T10 disposes of.
- `atlas_code_symbol` — `compile_databases: 0`, so every edge is a `UNIQUE_LEXICAL`
  candidate and was confirmed in the source:
  - `atlas_service_decision_confirm`: defined `src/core/service_decision.c:1685`,
    declared `include/atlas/service.h:940`, **one caller** at `src/cli/cli.c:2060`. It
    calls `apply_op` twice — the mint at `service_decision.c:1763` and the spend at
    `:1846` (confirmed by grep; Atlas' lines were exact).
  - `run_decision_confirm`: `cli.c:2016`, five callers at `cli.c:2485-2500`, one per
    intent.
  - `atlas_terminal_available`: `src/core/terminal.c:25`, body at `:29`, one caller at
    `terminal.c:34` inside `atlas_terminal_open`.
  - `api_handle`: `src/gw/gateway.c:1002`, one caller at `gateway.c:1392`.

## What exists, verified against the tree at `b22f822` (2026-09-03)

Every line reference below was read during planning. Where the brief and the tree
disagree, the tree is reported and the disagreement is named.

**The gateway's routes, counted here rather than taken from anywhere.**
`API_ROUTES[]` is `src/gw/gateway.c:787-851` and holds **26 rows**. Outside the table the
dispatcher in `atlas_gateway_serve_bytes` (`gateway.c:1115`) matches six literal paths:
`OPTIONS` on any path (`:1149`), `GET /healthz` (`:1168`), `POST /mcp` (`:1176`),
`POST /auth/login` (`:1251`), `POST /auth/logout` (`:1321`), `GET /auth/me` (`:1332`), and
`GET /` with `/index.html` as an alias (`:1360-1361`). The brief's "27 routes" is not a
count this plan could reproduce from the tree; the roadmap section's "26" matches the
table. Neither number is pinned anywhere in this season — a count kept by hand is the
defect commit `a169393` records, so T1 tests a *property* of the table instead.

**"None of them mutates", in its precise form.** Every table row forwards one daemon
method through `api_handle`, which refuses any HTTP method but GET at `gateway.c:1015`;
no row names a member of `OPERATOR_METHODS[]` (`src/ipc/server_decision.c:2708-2723`).
What *does* write: every request appends its own row to `gw_audit` through
`gateway.audit` (`audit()` at `gateway.c:430`, queued to the writer, its fate never
learned), and `/auth/login` and `/auth/logout` mutate the gateway's in-memory session
table (`gw_sessions[]`, `gateway.c:597`). The stronger fact beneath the table: a row
naming `decision.approve` would still be answered `unknown method`, because the operator
group is offered on `SO_PEERCRED` at `src/ipc/server.c:1244-1250`, not on anything a route
says.

**The listener and the credentials, as deployed.** `build/atlas gateway status` (binds
nothing) reports `ENABLED`, listen `192.168.0.198:8799`, `tls: NONE`,
`remote_mcp=yes web_gui=yes`, uid `992`, `origins: 0 allowed`. `build/atlas api-key list`
shows two `ACTIVE` keys: `key_b2578f48143c06d3 chatgpt-tunnel` with
`context:read repo:read decisions:read graph:read impact:read`, last used
`2026-09-03T17:58:51Z`; and `key_581e0a805cc1febe mission-control` with those five plus
`audit:read`, last used `2026-08-12T18:57:56Z`. So the model and the browser are
**distinct keys**, and the operator reaches Atlas by a third identity — uid 1000 on the
local socket. That is the fact the threat argument rests on, and its limit is stated at
`gateway.c:1384-1387`: a session cookie and a bearer token resolve to one `principal`, and
"the authorization engine does not know which was used". Under `tls_mode = NONE` the
session cookie is set without `Secure` (`gateway.c:1303-1307`), which is correct for a
cleartext listener and is why nothing that disposes of a record may live on it.

**The operator channel, mechanically.** `atlas_terminal_available` is
`isatty(STDIN_FILENO) == 1 && isatty(STDOUT_FILENO) == 1` (`terminal.c:29`);
`atlas_terminal_open` refuses without it (`terminal.c:32-36`) and then opens `/dev/tty`
(`:43`). `atlas_service_decision_confirm` opens the terminal, mints a challenge
(`service_decision.c:1763`), shows the prompt (`show_prompt`, `:1532`), reads one line
(`:1787`), compares it to the eight-hex confirmation before the round trip so a typo does
not spend the capability (`:1803-1810`, sentence "that is not the confirmation for this
revision; nothing was changed"), and spends it (`:1846`). `run_decision_confirm`
(`cli.c:2016`) checks authority **first** (`atlas_authority_require`, `:2024`), refuses
`--yes` (`:2033-2040`) and `--json` (`:2041-2048`), and passes `st->opts.decision.revision`
as `revision_no` (`:2060`) — `--revision N` exists today (`cli.c:270`, parsed at
`:839-847`). In the lifecycle, `expect_revision_no > 0` refuses only a revision that does
not exist (`src/decision/lifecycle.c:911-919`, "this decision has no revision %lld"). **What
happens when the pinned revision exists but is not the newest is not established by
anything this plan read**, and no sentence here claims it — T6 step 7 establishes it.

**The ledger's actor vocabulary is a schema CHECK.** `decision_events.actor` was widened
once, by migration 15 (`src/db/migrate.c:3049-3139`), which rebuilt the table because
SQLite cannot alter a CHECK, verified its own row count, and documented why
`decision_revisions.proposed_by` was deliberately left narrower. `decision_validations.actor`
admits only `LOCAL_OPERATOR_CONFIRMED` (`migrate.c:1580`). `MIGRATIONS[]` ends at **30**
(`migrate.c:4680`). A15 adds no migration; what tier 3 would need is in §The decision.

**Mission Control today** is one file, `src/gw/ui/mission-control.html`, 1009 lines,
eight views (`VIEWS`, `:199-203`), one fetch path (`api()`, `:227-250`, which sends
`credentials: "same-origin"` and hides a 403 behind "this credential does not permit that
read"), a Knowledge view with kind and status as separate selects (`:114-133`), a
`showDecision` detail that reads `decision` only (`:553-590`), and a `showClaim` that
already renders `conflict` beside the confidence figures (`:736`). The page's CSP is set
at `gateway.c:1366-1373`: `default-src 'none'; style-src 'unsafe-inline'; script-src
'unsafe-inline'; connect-src 'self'; form-action 'none'; frame-ancestors 'none'; base-uri
'none'` — inline script is already the only script, `localStorage` is not governed by
CSP, and nothing in this season needs the header to change. There is one header writer,
`atlas_http_write_head` (`src/gw/http.c:349`), and nothing here adds a header.

**One live defect the review view would have inherited.** `viewImpact`
(`mission-control.html:859-883`) sends `symbol` to `/api/v1/code/impact`; the row at
`gateway.c:824-825` forwards only `repo`, `path`, `depth`; `build_api_params` drops any
name the row does not declare (`gateway.c:930`, `route_wants` at `:853`); `code.impact` (`run_walk`,
`src/ipc/server_code.c:783-787`) then sees neither and refuses with `a "path" or a
"symbol" is required`. Chain: an operator naming a symbol in the Impact view is told a
symbol is required. T2 adds `symbol` to the row; T9 records the finding.

**Two more parameters the daemon reads and the table does not forward.** `gate.check`
reads `decision` (`server_decision.c:1564`, and `only_uid` at `:1578-1580` so one record
costs one assessment); the row at `gateway.c:808` forwards `repo` only. `decision.get`
reads `revision` (`server_decision.c:1221`); the row at `gateway.c:804-805` forwards
`repo` and `decision` only. Adding a parameter to a row is the sanctioned way a query
string reaches a daemon call (`gateway.c:788-795`, and A9.1's `kind` precedent at
`:798-803`).

**What the read routes already return, and what the review view composes from.**
`decision.list` (`doc_list`, `server_decision.c:826-880`) emits `decision`, `status`,
`kind`, `revision`, `latest_revision`, `revision_state`, `content_hash`, `proposed_by`,
`links`, `title` (`UNTRUSTED_DATA`). `decision.history` (`:1454-1530`) emits `document`,
`revisions[]` (`number`, `state`, `content_hash`, `proposed_by`, `created_at`,
`basis_head`, `title`), `timeline[]` (`event`, `actor`, `revision`, `content_hash`,
`operator_channel` — `challenge_id > 0` at `:1435` — `superseded_by`), and
`ledger_agrees` from `atlas_db_decision_verify` (`:1522`; "reported, never repaired").
`gate.check` emits the repository result plus `decisions[]` with `decision`, `revision`,
`title`, `freshness`, `content_hash`, `validated_at_commit` (`:1631-1660` region). The
default gate path assesses *approved* records (`:1577-1581` comment); what
`atlas_gate_run_one` returns for a PROPOSED one is established by T2's test, not claimed
here. `verify.claims` takes `decision`; `verify.show` returns the aggregate and, in it,
`conflict` (`src/core/service_verify.c:423`).

**A12.1's finding reaches the browser through exactly one field.** The reconciler's
diff and pack rows are read by `memory.status`, which is in `OPERATOR_METHODS[]`
(`server_decision.c:2721`) and therefore unreachable from the gateway uid. What the
browser can see is a claim whose aggregate `conflict` is `IMPLEMENTATION`, and
`docs/context-reconciliation.md:950-1004` establishes that the reconciler produces that
for **one shape only**: a claim carrying both a decision anchor and a SYMBOL anchor,
extracted while the symbol resolved, later removed from a semantic generation the vanish
sweep can show is coverage-complete. A new claim is never evaluated; an edited
PATH-anchored file is `IMPACTED`; a deleted one is `UNDETERMINED`. The page may say
exactly that and nothing wider.

**The tripwire.** `tests/test_decision_mcp.c`: tool names against `approve, approval,
reject, supersede, confirm, sign, resolve, revalidate` (`:79-81`); schema properties
against `"token":`, `"confirmation":`, `"challenge":`, `"approved":` (`:132-134`); the
documents and sources in `FILES[]` (`:388-409`) against fourteen phrases (`:371-386`),
each of which would assert that a person was established — not reproduced here, so that
nothing copied out of this plan can trip the scanner; required
wording (`:427-455`), including `integrations/claude/atlas/skills/atlas-memory/SKILL.md`
containing "do not run it yourself" — the skill's paragraph at `SKILL.md:54-59` names
`atlas decision approve` and nothing else. Mission Control is not in `FILES[]` today.

**The A9 scope precedent.** `SCOPES[]` (`src/gw/apikey.c:22-33`) lists `memory:write`
with `grantable = false`; `atlas_apikey_scope_grantable` (`:58`) is the one question.
`tests/test_gw_remote.c:275-307` proves the most powerful A9 credential reaches no write
tool.

**How the page is tested.** `test_mission_control_reaches_the_verification_routes`
(`tests/test_gw_remote.c:1394-1478`) fetches `/` through `gui_request` (`:723`), greps the
served bytes for bindings, then drives the routes with a session cookie. **No test
executes the page's JavaScript.** A15's UI tasks inherit exactly that and say so.

## The decision: tier 1 this season, tier 2 rejected, tier 3 costed and not authorised

The roadmap offers three tiers and does not choose. This plan chooses **tier 1** — the
UI reviews, the terminal disposes — and the case is a chain, not a preference.

**The expensive half is reading, and reading is rendering over routes that exist.** A
proposal is read by opening every revision, the claims about it, each claim's evidence
and attestations with their identities and groups, the aggregate's reasons, the gate's
freshness where the record is approved, and what its links reach. Every one of those is
a GET the gateway already serves (§What exists), and the browser already renders four of
them. Tier 1 is that rendering plus a way to carry a decision to the terminal.

**Disposal cannot move to the listener without moving the threat model, and the
listener is where the model is.** The model's key and the browser's key are different
keys and the same kind of principal; `gateway.c:1384-1387` says in its own words that the
authorization engine cannot tell them apart. Any capability placed on `/api/` is
therefore one scope bit away from the key that proposed the record. `tls_mode = NONE` on
this machine means that bit, and the session cookie carrying it, travel in the clear on
the LAN. So tier 1's claim is exact: **the threat model moves by nothing** — no route, no
scope, no method, no actor, no migration — and the route-table property T1 adds is what
keeps that true after this season.

**What the local channel actually buys, and the only comparison this plan makes.**
`LOCAL_OPERATOR_CONFIRMED` names a channel: a local process running as the operator's
uid, a controlling terminal on both standard streams, a single-use capability bound to
one revision's content hash and valid 120 s (`ATLAS_DECISION_CHALLENGE_TTL_MS`,
`include/atlas/limits.h:604`), and eight hex characters of that hash typed on `/dev/tty`.
A same-uid process driving a pseudo-terminal reaches it exactly as a person does
(`docs/decision-lifecycle.md:32-36`); `tests/test_decision_operator.c` does so on purpose.
Tier 1 keeps that channel byte-for-byte and adds a *list* in front of it. Every
comparison below is against that sentence and not a stronger one.

**What tier 1 leaves undone, so the choice is the operator's.** The operator's stated
purpose is to dispose of a record from wherever they are. After A15, the reading happens
wherever a browser reaches the gateway, and the disposal still needs a terminal on the
Atlas machine as uid 1000 — which `ssh` from anywhere provides, at the cost of an ssh
session and a copied file. Tier 1 does not give a phone with no shell a way to dispose.
It also gives no cross-device queue: the sheet lives in one browser's `localStorage` and
moves by the operator's hand. Both are stated costs, not oversights.

**Tier 2 is rejected, with the chain.** Tier 2 shows the single-use capability in the
browser. A challenge is minted by `decision.challenge`, which is in `OPERATOR_METHODS[]`
and answered `unknown method` to the gateway uid (`server.c:1244-1250`); minting one for
the browser needs a new daemon method offered to uid 992, which is a new door on the
listener — tier 3's cost with none of its protections. And the local command mints its
own challenge inside `atlas_service_decision_confirm` anyway, so a browser-minted one
buys nothing but a second minting path and a capability displayed over cleartext HTTP.

**Tier 3 is absent, not refused; here is what it would cost.** Written so the operator
chooses with the bill in view. If it is ever chosen it is a season of its own, with its
own authorisation, and it is *weaker than the local channel by construction*, because the
operator's authority would pass through a network-facing process that A9 designed to
hold none. The six roadmap non-negotiables, each with the concrete mechanism and the
cost this planning found:

1. **Its own channel identity** — `ATLAS_DECISION_ACTOR_REMOTE_OPERATOR_CONFIRMED`, a
   new member of `atlas_decision_actor` (`include/atlas/decision.h:255-302`), and
   **migration 31** rebuilding `decision_events` to widen the actor CHECK exactly as
   migration 15 did (`migrate.c:3049-3139`), with the same row-count verification.
   `decision_validations` stays untouched if revalidation stays local. Whatever
   `atlas_db_decision_verify`'s replay (`src/db/db_decision.c:2851`) checks about actors
   is **unread** and is an obligation of that season, not a claim of this one. A channel
   column on `decision_challenges`, so a remotely minted capability cannot be spent
   locally or the reverse.
2. **Its own scope, ungrantable to any model credential** — `decisions:dispose` in
   `SCOPES[]` with `grantable = false` on the `memory:write` precedent; the bit is never
   stored on a key row and is instead *derived at verification* from a root-owned
   `remote_operator_key = key_…` line in `/etc/atlas/gateway.conf`, which is A7's rule
   that authority is configured outside the reach of the principal it constrains.
3. **Absent from the MCP surface** — no tool maps to the scope; `tests/test_gw_remote.c`
   asks `tools/list` and `tools/call` for every plausible name, as `:396` already does
   for credential administration.
4. **TLS in front** — Atlas terminates none and must never be described as providing
   it; the daemon refuses to offer
   the remote-operator method group unless the gateway policy declares
   `tls_mode = REVERSE_PROXY`. **On this machine as configured today that check fails**
   (`tls: NONE`), so tier 3 would be refused by its own first gate until a terminator is
   put in front and the policy says so.
5. **Replay protection bound to the content hash** — the existing challenge shape: the
   daemon mints against `(repo, document, revision, content_hash, intent)`, the browser
   sends back the eight-hex confirmation the operator typed, and the write point compares
   it as it does today (`service_decision.c:1803` is the pre-check; the write point's own
   comparison is the guard).
6. **A root-owned policy naming which kinds it may act on** — a
   `remote_dispose_kinds = OPERATIONAL_FACT PARKED` line, refused rather than clamped.

   And three costs the roadmap did not list: the daemon must authenticate the bearer
   token itself in the transaction that spends the capability (it already holds the
   verifiers — `method_gateway_auth`, `src/ipc/server_gw.c:80-109`), because a key id the
   gateway *claims* is A14's shape-1 problem; `api_handle` refuses non-GET for the whole
   table (`gateway.c:1015`), so a second table or a method column is needed; and the
   gateway parses no JSON body today except the login key by hand (`take_login_key`,
   `gateway.c:700-745`) — a yyjson call site in `src/gw` would extend the vendored
   library's stated contract and must be argued for in writing. The honest weakening,
   stated in the same paragraph that would announce it: a compromised gateway holds the
   operator's token for as long as the operator uses the channel.

## The nine decisions this plan settles, each with its argument

### Decision 1 — The queue is a review sheet in the operator's browser, and it stores no authority

The roadmap's tier 1 says "a queue that stores no authority at all" and "one local
command walks the queue". Where the queue lives decides whether the first half is true.

A queue in the daemon would need a write route: the first on `/api/`, under a scope; the
model's key is the same principal type as the browser's (§What exists), so a model
holding that scope could place "I would approve this" on its own proposal, and it would
appear in the operator's walk looking like the operator's own intent. The terminal step
would still demand the typed hash, but the queue would have become a way to put words
in the operator's mouth. So the queue lives in the operator's browser — `localStorage`
under the gateway's origin — and reaches the terminal as text the operator copies. The
**review sheet** is that text: a plain ASCII list of what the operator said in the
browser, one line per record, in a grammar (§Frozen formats) that has **no field for a
confirmation**, so a sheet is not a capability and cannot be turned into one by editing
it. It is the browser-side mirror of `tests/test_decision_mcp.c:115-140`'s rule that no
schema declares a confirmation, and T3's grammar test says so in the same shape.

Cost, stated: one browser, one origin; a cleared site store empties the queue; nothing
about a sheet is durable evidence of anything.

### Decision 2 — The sheet reaches the terminal as a file argument, never on standard input

`atlas_terminal_available` requires *both* standard input and standard output to be
terminals (`terminal.c:29`), and `docs/decision-lifecycle.md:629` documents that as
step 1 of the channel, before `/dev/tty` is opened. A sheet piped on stdin makes stdin a
pipe, and the confirm refuses before minting. So `atlas review apply` takes the sheet as
a **file path argument** and nothing else, and "sheet on stdin → refused, nothing minted"
is a test obligation of the same shape as `test_non_interactive_approval_is_refused`
(`tests/test_decision_operator.c:475`).

### Decision 3 — The walker loops the one confirm function with the revision pinned, and refuses before minting when the record moved

`atlas_service_decision_confirm` is "the whole of" the operator channel
(`service.h:933-939`): terminal, mint, display, `/dev/tty`, spend, in one function with
no way to reach the spending half without the displaying half. The walker calls it once
per entry with `revision_no` set to the revision the operator reviewed, exactly as
`--revision N` does today. It creates no second minting or spending path, so
`atlas_decision_apply_in_tx` keeps its three callers and the four sentences
`docs/decision-lifecycle.md:25-30` lists as what `APPROVED` does *not* mean stay true
unchanged.

What the walker adds is the check the browser made possible and the terminal alone could
not: **before minting**, it reads the record through the existing service read
(`atlas_service_decision_show`, `service.h:792`) **with `revision_no = N`, the reviewed
revision** — not `0`, which `service.h:790-791` defines as the *effective* revision, the
approved one when there is one, and which for a `resolve` entry can differ from the
newest — and refuses the entry when `atlas_decision_summary.latest_revision_no`
(`service.h:606`) is not `N`, when revision N's `content_hash` (`service.h:600`) does not
begin with the reviewed prefix, or when the record's status is no longer the one the
intent needs. So the walker and the page compare the same bytes: the page queues the
revision it displayed, and the walker checks that exact revision and that nothing newer
exists, for every intent. A reviewer who read revision 2 in a browser must never approve
revision 3 because a model revised the record between the browser and the terminal, and
must re-review a record that gained a revision after it was queued. The refusal costs no
challenge row.

The window between that read and the mint is real and is not closed here: a revision
landing in it produces a challenge bound to the reviewed revision N with `expect_revision_no
= N`. What the lifecycle then does with an approval of a revision that exists but is not
newest is **not established by anything read for this plan**; T6 step 7 establishes it
with a test and records the answer in `docs/decision-lifecycle.md` beside `--revision`.
The prompt itself still shows the revision number and the digest, and the operator types
that digest's prefix, so the record approved is the one displayed whatever the answer.

### Decision 4 — No route is added; three rows gain a parameter; and a property of the table is tested instead of its count

`gate?decision`, `decision?revision` and `code/impact?symbol` are each one row edit in
`API_ROUTES[]`, each forwarding a name the daemon already reads to a method that is
already a read (§What exists). Nothing else about the table changes. T1 adds the accessor
the table has never had and a unit test that asserts what the documents claim: no row
names a member of `atlas_server_operator_methods()`, no row names `gateway.auth` or
`gateway.audit`, no row names any of the methods `docs/remote-access.md:51-56` says the
gateway cannot reach, and every row's scope answers true to `atlas_apikey_scope_grantable`
— which is the exact form of "no route needs a write scope". The row count is not
asserted anywhere.

### Decision 5 — Drift in the browser is exactly one field, labelled with the one shape it can mean

The only A12.1 output the gateway uid can read is `verify.show`'s `conflict`. The page
therefore shows one thing: when a claim's aggregate `conflict` is `IMPLEMENTATION`, the
frozen label in §Frozen formats, which names the one producible shape and says it is a
finding against the code and not a rewrite of the decision. When it is anything else the
page says nothing about drift at all. A view that implied broader detection would be
advertising something the code does not do, which is the defect A12.1 spent itself
finding.

### Decision 6 — Review-surface prose names the channel and never a person, and the page joins the tripwire

Every sentence the Review view, the walker and the documents print is written against
`test_decision_mcp.c:371-386`. T8 adds `src/gw/ui/mission-control.html` and
`src/core/service_review.c` to `FILES[]` and adds a required-wording pair for the page's
frozen sentence, so the guarantee is a failing build rather than a review habit. The
skill file gains `atlas review apply` beside `atlas decision approve` in its "do not run
it yourself" paragraph, because a model that will not run one and would run the other
has learned the name and not the rule.

### Decision 7 — Intents on a sheet are approve, reject and resolve; supersede and revalidate stay terminal-only

Approve and reject act on a PROPOSED record; resolve acts on an APPROVED record of a
kind that makes a demand (`atlas_decision_kind_resolvable`, `decision.h:245`). All three
are `(repository, decision, revision)`-shaped, which is what one sheet line carries.
Supersede needs a second record (`--by`), and revalidate binds two more things — the
indexed commit and an evidence digest (`decision.h:708-733`) — that a browser review does
not establish. Both keep their existing single-verb commands, unchanged.

### Decision 8 — The sheet's bound is written down, and the worst case follows from it

`ATLAS_REVIEW_SHEET_MAX_ENTRIES` is **64**. Each entry that reaches the prompt mints one
challenge; an abandoned entry leaves one unconsumed row until its 120 s TTL. Sixty-four is
below `ATLAS_DECISION_CHALLENGES_RETAIN` (200, `limits.h:612`), so a walk in which every
entry is abandoned cannot by itself reach the retention ceiling; and sixty-four typed
confirmations, each after reading a prompt, is already more than one sitting holds. Above
the bound the sheet is refused, never truncated.

### Decision 9 — No test executes the page's JavaScript, and the plan says so once

The suite has no browser and adds none. The page is tested as the A9.2.1 closure tested
it: the served bytes are grepped for the bindings each panel depends on and for the
frozen sentences, and the routes those bindings name are driven through a real session
cookie. Surface parity beyond that is not claimed. T10's live acceptance is the one time a
person loads the page, and it is recorded as an observation.

---

## Frozen formats

### The review sheet

```
atlas-review-sheet/1
# lines beginning with # are ignored; blank lines are ignored
approve atlas atlas-dec-28f03b0a44a53db88f0deace6e79721b r1 6fb2be08
reject  atlas atlas-dec-c711a6d9c4954961a5e9d18240591d8e r3 5146bbb3
resolve atlas atlas-dec-314ed60fe9bd11400646934658843bf3 r2 89d53ae3
```

- The first non-blank, non-comment line is exactly `atlas-review-sheet/1`.
- An entry line has **exactly five fields** separated by one or more ASCII spaces or
  tabs: `intent`, `repository`, `decision`, `revision`, `confirm-prefix`.
- `intent` ∈ `approve | reject | resolve`, lowercase. The parse produces an
  `atlas_decision_intent`; `atlas_review_intent_allowed` is the one predicate.
- `repository`: whatever `atlas_db_check_repo_name` accepts (`src/db/db_repo.c:43-69`):
  1..`ATLAS_NAME_MAX` (128, `limits.h:11-12`, "user-facing repository name") bytes of
  ASCII letters, digits, `.`, `_` and `-`, not starting with `-` or `.`. The parser calls
  that function rather than restating it, so a name the registry would refuse is a name
  the sheet refuses, and a name the registry holds always fits the grammar — which is
  what makes whitespace-splitting sufficient. It is still untrusted text and is
  safe-encoded before it is printed.
- `decision`: `atlas_decision_uid_is_valid` (`decision.h:612`) — `atlas-dec-` plus 32
  lowercase hex.
- `revision`: `r` followed by a decimal in 1..2147483647 with no leading zero.
- `confirm-prefix`: exactly `ATLAS_DECISION_CONFIRM_HEX` (8) lowercase hex characters —
  the first eight of the reviewed revision's content hash. It is **not** the
  confirmation; the confirmation is typed on `/dev/tty` per entry, as today.
- Bytes: printable ASCII, `\n`, `\t`, and `\r` immediately before `\n` (stripped).
  Anything else refuses the sheet.
- At most `ATLAS_REVIEW_SHEET_MAX_ENTRIES` (64) entries, `ATLAS_REVIEW_SHEET_MAX_LINE`
  (256) bytes per line, `ATLAS_REVIEW_SHEET_MAX_BYTES` (64 KiB) per file. Refused above.
- A `(repository, decision)` pair appears at most once; a second occurrence refuses the
  sheet. A sheet with any refused line is refused whole, before any read or mint — a
  sheet with one bad line may be a sheet mangled in transit.

### The refusal sentences (grammar)

```
this is not an Atlas review sheet: the first line must be atlas-review-sheet/1
review sheet line %zu: expected 5 fields (intent repository decision rN prefix), found %zu
review sheet line %zu: "%s" is not an intent a sheet may carry (approve, reject or resolve)
review sheet line %zu: the decision id is malformed
review sheet line %zu: the revision must be r followed by a positive number
review sheet line %zu: the confirmation prefix must be exactly 8 lowercase hex characters
review sheet line %zu: %s was already named on line %zu; a sheet disposes of a record once
review sheet line %zu: a byte outside printable ASCII; a sheet carries identifiers, never prose
review sheet: more than %u entries; split it
review sheet: larger than %u bytes
```

The `%s` in the intent sentence is the offending field safe-encoded and truncated to 32
bytes; the `%s` in the duplicate sentence is the decision id, which is already a checked
shape.

**Amended during execution, 2026-09-04.** Two sentences the grammar's bullets require
and this block did not carry, both written by T3 and adopted here rather than respelled:

```
review sheet line %zu: longer than %u bytes
review sheet line %zu: %s
```

The first is the per-line bound; the second wraps `atlas_db_check_repo_name`'s own
message for a repository field it refuses, so one grammar answers for the registry's.
A third, produced by the walker rather than the parser, is in the next block.

**Amended during execution, 2026-09-04 (whole-branch review fix wave).** The line above
this paragraph originally read "the offending field safe-encoded and truncated to 32
bytes" — a description of a defect, not of the grammar. `refuse_bad_intent`
(`src/core/review.c`) truncates the field to `ATLAS_REVIEW_INTENT_MSG_MAX` (32) bytes and
carries it into the message *raw*; `atlas_render_error` (`src/cli/render_json.c`)
safe-encodes the whole rendered message exactly once, at the point it reaches a terminal
or a JSON document — the tree's ordinary rule, that an error message carries raw text and
is encoded once at render, and not a rule this file gets to have its own version of.
Encoding here as well as there double-encoded a literal `%` byte into `%2525`, measured
against the installed binary; `tests/test_review_sheet.c` pinned the pre-render form
(`%25bad`) and therefore pinned the deviation. The corrected line: the `%s` in the intent
sentence is the offending field truncated to 32 bytes, carried raw and encoded once, at
render, like every other value `atlas_err_set` carries in this tree.

### The refusal sentences (`atlas review apply`)

```
--yes cannot apply a review sheet. Each entry needs its confirmation typed on an interactive terminal, and Atlas will not accept one from a flag, a pipe, the sheet itself or an environment variable.
--json is not available for review apply: it is an interactive command. Use --check for a machine-readable dry run.
review sheet: no entries; there is nothing to review
```

Without a terminal on both standard streams the walker refuses with **the sentence
`atlas_terminal_open` already produces** (`terminal.c:34-36`), obtained by calling it and
closing the handle before any entry is read.

### The per-entry verdicts

```c
typedef enum atlas_review_verdict {
    ATLAS_REVIEW_UNKNOWN = 0,   /* never emitted, never parses */
    ATLAS_REVIEW_READY,         /* --check only: the pre-check passed, nothing was minted */
    ATLAS_REVIEW_APPLIED,       /* the capability was spent; the ledger has the event */
    ATLAS_REVIEW_ABANDONED,     /* the operator typed something other than the prefix */
    ATLAS_REVIEW_MOVED,         /* newest revision or hash prefix differs from the sheet */
    ATLAS_REVIEW_DISPOSED,      /* the record's status is no longer the one the intent needs */
    ATLAS_REVIEW_MISSING,       /* no such repository, or no such decision in it */
    ATLAS_REVIEW_REFUSED        /* any other refusal from the confirm; its message is carried */
} atlas_review_verdict;
```

Detail lines, fixed Atlas text with checked values only:

```
reviewed r%lld (%.8s), now r%lld (%.8s)             MOVED
the record is %s; %s needs %s                         DISPOSED (status, intent, required status)
no such decision in %s                                MISSING
no such repository                                    MISSING
```

**Amended during execution, 2026-09-04.** Three lines this block did not carry, written
by T4 against the human example above and adopted here rather than respelled. Each `%s`
is a checked value, not untrusted text: the state name comes from
`atlas_decision_state_name`, and the REFUSED message is the confirm's own.

```
%s at r%lld                                           APPLIED (state, revision)
nothing was changed                                   ABANDONED
<the confirm's own refusal message>                   REFUSED
```

### The command's output

Human, one line per entry then a totals line:

```
atlas review apply /home/op/review.txt
  1  approve  atlas  atlas-dec-28f03b0a…  r1  APPLIED    APPROVED at r1
  2  reject   atlas  atlas-dec-c711a6d9…  r3  MOVED      reviewed r3 (5146bbb3), now r4 (0a91c2ee)
  3  resolve  atlas  atlas-dec-314ed60f…  r2  ABANDONED  nothing was changed
applied 1, abandoned 1, moved 1, disposed 0, missing 0, refused 0
```

JSON (`--check` only):

```json
{"command":"review apply","ok":true,"check":true,"sheet":"/home/op/review.txt",
 "entries":[{"line":3,"intent":"approve","repo":"atlas","decision":"atlas-dec-…",
             "revision":1,"prefix":"6fb2be08","verdict":"READY","status":"PROPOSED",
             "detail":null}],
 "ready":1,"moved":0,"disposed":0,"missing":0}
```

Exit codes: `0` when every entry ended `APPLIED` (or `READY` under `--check`); **`8`**
when at least one entry ended otherwise — a command-specific code above the seven-value
contract, following `gate`'s `8`/`9` (`cli.c:203`); `2` for a malformed sheet, a sheet
with no entries, `--yes`, `--json` without `--check`, or no terminal. A locked
authority profile exits with whatever `atlas decision approve` exits with, because it is the same
`atlas_authority_require` call made first.

### The Review view's fixed sentences

Under the sheet panel, verbatim:

> Queuing a record here stores no authority anywhere. To dispose of these records, save
> this sheet as a file on the Atlas machine and run `atlas review apply FILE` in a
> terminal there. Atlas records each disposal as LOCAL_OPERATOR_CONFIRMED, which names
> the channel, not a person.

Beside a claim whose `conflict` is `IMPLEMENTATION`, verbatim:

> IMPLEMENTATION conflict: a decision-bound symbol anchor no longer resolves in a
> coverage-complete semantic generation. This is the only shape A12.1's reconciler can
> produce. It is a finding against the code, not a rewrite of the decision.

What T7's test asserts about the page is exactly two things: the frozen label above is
present, and the bare phrase `implementation drift` is absent. The A9.2.2 truth-axis
field at `mission-control.html:704` (`c.source_drift`, rendered as `SOURCE_DRIFT`) is a
different axis and stays as it is.

### The three route rows, after T2

```c
{"/api/v1/decision", "decision.get", ATLAS_SCOPE_DECISIONS_READ,
 {"repo", "decision", "revision", NULL}, {"revision", NULL}},
{"/api/v1/gate", "gate.check", ATLAS_SCOPE_DECISIONS_READ,
 {"repo", "decision", NULL}, {NULL}},
{"/api/v1/code/impact", "code.impact", ATLAS_SCOPE_IMPACT_READ,
 {"repo", "path", "symbol", "depth", NULL}, {"depth", NULL}},
```

`GW_API_MAX_PARAMS` is 8 (`gateway.c:769`); the widest row after this edit
(`/api/v1/decisions`) still holds five names plus its terminator.

---

## Authority argument — the season's non-negotiables

These go into `docs/engineering-rules.md` in full and into `CLAUDE.md` as one line each
(T9). The five the brief requires are restated first, with their reasons; the rest are
this season's own.

- **No approval verb in an MCP tool name, ever.** `tests/test_decision_mcp.c:79-81`
  scans for `approve`, `approval`, `reject`, `supersede`, `confirm`, `sign`, `resolve`,
  `revalidate`, and the scanner is right: a lifecycle transition reachable from a model
  by name is the thing the whole A4 channel exists to make absent. A15 adds no MCP tool.
- **Nothing may claim a channel establishes that a natural person acted.**
  `LOCAL_OPERATOR_CONFIRMED` identifies a channel, not a person; a same-uid process
  driving a pseudo-terminal reaches it exactly as a person does. Every comparison in
  this plan is against that sentence. The page and the walker's text are scanned by the
  same test that scans `CLAUDE.md`.
- **A mutating route needs its own channel identity in the audit row, never
  `LOCAL_OPERATOR_CONFIRMED` reused.** Reusing the name would make every ledger row ever
  written retrospectively ambiguous, which is the one cost that cannot be paid back.
  A15 adds no mutating route; the requirement is restated because tier 3 is written
  down here and this is its first line.
- **`memory:write` is the precedent: in the vocabulary and not grantable.** Any scope a
  later season adds for disposal follows it — present in `SCOPES[]`, `grantable =
  false`, derived only from a root-owned line. A15 adds no scope.
- **The gateway has no filesystem read path, and Atlas terminates no TLS.** The page is
  served from the binary; the sheet is a file the *operator* saves on the Atlas machine
  and hands to a local command, never a file the gateway reads. A15 does not describe
  its listener as secure; on this machine it is cleartext, and that is why disposal is
  not on it.
- **A PROPOSAL NOBODY CAN REVIEW COMFORTABLY IS A PROPOSAL NOBODY REVIEWS.** The
  reading moves to the surface that is good at it; the disposing stays where the only
  channel is.
- **The review sheet stores no authority.** It is a list of what the operator said in a
  browser; every entry is still confirmed on `/dev/tty` against the revision's content
  hash, and the grammar test refuses a line with a sixth field for the reason the MCP
  schema test refuses a `"confirmation":` property.

  **Amended during execution, 2026-09-04.** This bullet said "and has no field for a
  confirmation", and the whole-branch review established that half is false as written:
  the confirmation an operator types *is* the first eight hex of the revision's content
  hash (`atlas_decision_confirm_phrase`, `src/decision/lifecycle.c`), and the sheet's
  fifth field is exactly that string — a test types the sheet's own prefix and gets
  APPLIED. The security claim is unharmed and is what the corrected wording keeps: the
  prefix is public, `decision show` prints it, and under A7 nothing typed by a same-uid
  process is evidence that a person acted. What is true is that the sheet carries the
  public prefix the operator will type and has **no field the walker reads in place of
  that typing on `/dev/tty`**. The four source comments and the three documents carrying
  the old sentence were corrected; the one at line 937 below is left alone deliberately,
  because it is the subject line of a commit that exists in git history and the plan
  should not disagree with the ledger of what was done.
- **The sheet is a file argument, never standard input.** Both standard streams must be
  terminals (`terminal.c:29`); a piped sheet is refused before anything is minted.
- **The walker loops `atlas_service_decision_confirm` with the reviewed revision pinned
  and refuses before minting when the record moved.** `atlas_decision_apply_in_tx` still
  has exactly three callers; a fourth is not this season.
- **No route was added.** Three rows forward one more name each, which is the sanctioned
  way; the table's property — no operator method, no gateway write, every scope
  grantable — is tested, and its count is not.
- **A12.1's finding reaches the browser through one field and is labelled with the one
  shape it can mean.** The reconciler's own rows are operator-group reads.
- **No test executes the page's JavaScript.** The served bytes are grepped and the routes
  are driven; that is the whole of what the suite establishes about the page.
- **Tier 3 is absent, not refused.** Its cost is written in this plan and in
  `docs/review-surface.md`; it is a season of its own with its own authorisation, and it
  is weaker than the local channel by construction.
- **No new thread, process, timer or background loop; no migration; no MCP tool; no
  gateway route; no scope; no daemon method; no actor; no authority verb in any new
  name.** The new command is `review apply`, and `review` is not on the scanner's list
  because it is not an authority verb.

## Worst-case cost, stated so nobody discovers it in a bill

Per `atlas review apply`: ≤ 64 entries; per entry one service read, then at most one
challenge mint, one prompt, one `/dev/tty` line and one spend; ≤ 64 unconsumed
challenges if every entry is abandoned, each expiring in 120 s and all below the
retention bound of 200. Zero processes created, zero model calls, zero migrations.

Per record reviewed in Mission Control: one `decision`, one `decision/history`, one
`verify/claims`, one `gate` when the record is APPROVED, one `verify/claim` per claim
opened, one `decision?revision=N` per earlier revision opened, and at most
`REVIEW_UI_MAX_IMPACT_LINKS` (8, a page constant) impact walks — one per PATH or SYMBOL
link, each bounded by the daemon's own walk limit. Every one is a read and every one
appends a `gw_audit` row, so reviewing one record writes roughly a dozen audit rows; that
is A9's design, and it is stated. On load the Review view re-reads each queued entry
once, ≤ 64 reads. **Zero money.** T10 spends none either: it disposes of the two
disposable probes already in the index.

---

# File structure

**Create:**

| Path | Responsibility |
| --- | --- |
| `include/atlas/review.h` | the sheet model, the verdict vocabulary, the bounds, `atlas_review_sheet_parse`, `atlas_review_intent_allowed` |
| `src/core/review.c` | the parser and the name/parse pairs; no I/O, no database |
| `src/core/service_review.c` | `atlas_service_review_apply`: authority, terminal, file read, pre-check, the loop over `atlas_service_decision_confirm` |
| `tests/support/pty.h`, `tests/support/pty.c` | `pty_spawn`, `pty_expect`, `pty_type`, `pty_wait`, moved out of `tests/test_decision_operator.c:195-350` unchanged |
| `tests/test_review_sheet.c` | unit: the grammar, the bounds, the sixth-field refusal |
| `tests/test_review_apply.c` | integration: the walker under a pseudo-terminal |
| `docs/review-surface.md` | the season's document |

**Modify:** `CMakeLists.txt` (two `.c` files), `tests/CMakeLists.txt` (two tests, the
support file, labels), `include/atlas/gateway.h` and `src/gw/gateway.c` (the route-view
accessor, three rows), `tests/test_gateway.c` (the table property), `tests/test_gw_remote.c`
(the three parameters, the Review view's bindings and sentences), `src/gw/ui/mission-control.html`
(the Review view), `include/atlas/limits.h` (three bounds), `include/atlas/service.h`,
`src/cli/cli.c`, `src/cli/render.h`, `src/cli/render_human.c`, `src/cli/render_json.c`,
`tests/test_decision_mcp.c` (`FILES[]`, required wording), `tests/test_decision_operator.c`
(use the moved helpers), `integrations/claude/atlas/skills/atlas-memory/SKILL.md`,
`docs/roadmap.md`, `docs/remote-access.md`, `docs/decision-lifecycle.md`,
`docs/engineering-rules.md`, `docs/extending.md`, `docs/backlog.md`, `SECURITY.md`,
`README.md`, `CLAUDE.md`, and last `include/atlas/atlas.h` (`ATLAS_PHASE`).

---

# Tasks

Dependency order: {T1, T3} → T2 → T4 → T5 → T6 → T7 → T8 → T9 → T10. T1 and T3 are
independent and may run in parallel.

---

### Task T1: the route table gets an accessor, and its property gets a test

**Files:**
- Modify: `include/atlas/gateway.h`, `src/gw/gateway.c`, `tests/test_gateway.c`

**Interfaces produced:**

```c
/* include/atlas/gateway.h */
typedef struct atlas_gateway_route_view {
    const char *path;      /* the exact literal matched at gateway.c:1007 */
    const char *method;    /* the daemon method it forwards to */
    atlas_apikey_scope scope;
} atlas_gateway_route_view;

/* A read-only view of API_ROUTES[]. Exists so a test can assert a property of the
 * table; nothing in the gateway calls it. `count_out` receives the row count, which no
 * test may pin. */
const atlas_gateway_route_view *atlas_gateway_api_routes(size_t *count_out);
```

- [ ] **Step 1: Write the failing test** in `tests/test_gateway.c`
      (named `test_every_api_route_is_a_read_the_gateway_uid_may_make` when this plan was
      written; **renamed during execution** to
      `test_every_api_route_forwards_to_a_read_on_the_reviewed_allowlist`, because
      T1's fix rounds replaced the four negative checks with a positive allowlist and
      the old name asserted more than the body established): for every view row,
      (a) `method` is not the name of any entry returned by
      `atlas_server_operator_methods()`; (b) `method` is neither `gateway.auth` nor
      `gateway.audit`; (c) `method` is none of `backup.create`, `backup.verify`,
      `code.index`, `maintenance.plan`, `maintenance.prune`, `apikey.create`,
      `apikey.list`, `apikey.revoke`, and has neither the prefix `job.` nor `dispatch.`
      — the list at `docs/remote-access.md:51-56`; (d) `atlas_apikey_scope_grantable(scope)`
      is true and `scope != ATLAS_SCOPE_UNKNOWN`. The count is used only as the loop
      bound.
- [ ] **Step 2: Run it and watch it fail** — `cd build && ctest -R test_gateway
      --output-on-failure`. Expected: link failure, `atlas_gateway_api_routes` undefined.
- [ ] **Step 3: Implement the accessor** as a static array of views built beside
      `API_ROUTES[]` — or a function that copies the three fields into a static view
      table once. It reads `API_ROUTES[]` and nothing else.
- [ ] **Step 4: Run the test and watch it pass**; `make` with zero warnings.
- [ ] **Step 5: Commit** — `test(a15): every API route is a read the gateway uid may make, as a property and not a count`

---

### Task T2: three rows forward one more name each

**Files:**
- Modify: `src/gw/gateway.c` (rows at `:804-805`, `:808`, `:824-825`),
  `tests/test_gw_remote.c`

- [ ] **Step 1: Write the failing tests** in `tests/test_gw_remote.c`
      (`test_the_review_parameters_reach_the_daemon`), through a browser session as
      `test_mission_control_reaches_the_verification_routes` does:
      (a) propose, then `decision revise` the fixture record so revision 2 exists;
      `GET /api/v1/decision?repo=proj&decision=<uid>&revision=1` returns
      `"revision":{"number":1,…}` with revision 1's title, and `revision=2` returns
      revision 2's; `revision=abc` is `400`.
      (b) `GET /api/v1/gate?repo=proj&decision=<uid>` returns `decisions` with exactly
      one row naming `<uid>` when the record is APPROVED (approve it through the write
      point as `tests/test_decision_operator.c:145` does, with the fixture's authority
      unlocked as `env_open` there arranges). **Then establish, and write into the test
      as a comment and into `docs/decision-lifecycle.md` in T9, what the same request
      returns for a PROPOSED record** — this plan does not claim it.
      (c) `GET /api/v1/code/impact?repo=proj&symbol=main` is `200` and names the symbol;
      the same request today is refused, which is the defect this fixes.
- [ ] **Step 2: Run and watch (a), (b) and (c) fail** — `ctest -R test_gw_remote`.
- [ ] **Step 3: Edit the three rows** to the exact shape in §Frozen formats.
- [ ] **Step 4: Run the test and watch it pass**; run `test_gateway` (T1's property
      still holds); `make` with zero warnings.
- [ ] **Step 5: Commit** — `feat(a15): three read routes forward the parameter the daemon already reads`

---

### Task T3: the review sheet grammar, refused rather than repaired

**Files:**
- Create: `include/atlas/review.h`, `src/core/review.c`, `tests/test_review_sheet.c`
- Modify: `include/atlas/limits.h`, `CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces produced:**

```c
/* include/atlas/limits.h — each with its one-line reason in the house style */
#define ATLAS_REVIEW_SHEET_MAX_ENTRIES 64u   /* below ATLAS_DECISION_CHALLENGES_RETAIN */
#define ATLAS_REVIEW_SHEET_MAX_LINE 256u
#define ATLAS_REVIEW_SHEET_MAX_BYTES (64u * 1024u)

/* include/atlas/review.h */
#define ATLAS_REVIEW_SHEET_HEADER "atlas-review-sheet/1"

typedef enum atlas_review_verdict { /* §Frozen formats, verbatim */ } atlas_review_verdict;
const char *atlas_review_verdict_name(atlas_review_verdict v);
bool atlas_review_verdict_parse(const char *name, atlas_review_verdict *out); /* refuses "UNKNOWN" */

/* True for APPROVE, REJECT and RESOLVE; false for SUPERSEDE and REVALIDATE. The one
 * predicate; the parser asks it and so does the page's mirror in T7. */
bool atlas_review_intent_allowed(atlas_decision_intent i);

typedef struct atlas_review_entry {
    size_t line;                                   /* 1-based, for every message */
    atlas_decision_intent intent;
    char repo[ATLAS_NAME_MAX + 1u];
    char decision[ATLAS_DECISION_UID_MAX];
    int64_t revision_no;
    char prefix[ATLAS_DECISION_CONFIRM_HEX + 1u];
} atlas_review_entry;

typedef struct atlas_review_sheet {
    atlas_review_entry entries[ATLAS_REVIEW_SHEET_MAX_ENTRIES];
    size_t count;
} atlas_review_sheet;

/* Parses `len` bytes. Refuses the whole sheet on the first defect with one of the
 * §Frozen sentences; never repairs, never truncates, never skips a line. */
atlas_status atlas_review_sheet_parse(const char *bytes, size_t len, atlas_review_sheet *out,
                                      atlas_err *err);
```

- [ ] **Step 1: Write the failing test** in `tests/test_review_sheet.c`: the example
      sheet parses to three entries with the expected fields; each refusal sentence is
      produced by its input (missing header; four fields; **six fields — a line carrying
      a would-be confirmation**; `supersede`; a uid with 31 hex; `r0`, `r01`, `1`;
      a 7-hex and a 9-hex prefix; an uppercase prefix; a duplicate `(repo, decision)`;
      65 entries; a 257-byte line; a byte `0x80`; a `\r` not before `\n`); `\r\n` line
      ends parse; `#` and blank lines are ignored; the verdict vocabulary's zero member
      names `"UNKNOWN"` and refuses to parse; every other member round-trips;
      `atlas_review_intent_allowed` is true for exactly three intents.
- [ ] **Step 2: Run it and watch it fail** — compile failure, `atlas/review.h` not found.
      Register the test in `ATLAS_TESTS` and the `unit` label line.
- [ ] **Step 3: Write the header and `src/core/review.c`**; add the file to
      `atlas_core`. Every switch over the verdict vocabulary has **no** `default:`.
- [ ] **Step 4: Run the test and watch it pass**; `make` with zero warnings.
- [ ] **Step 5: Commit** — `feat(a15): the review sheet grammar, with no field for a confirmation`

---

### Task T4: the walker — authority, terminal, pre-check, then the one confirm function

**Files:**
- Create: `src/core/service_review.c`
- Modify: `include/atlas/service.h`, `CMakeLists.txt`

**Interfaces produced:**

```c
/* include/atlas/service.h */
typedef struct atlas_review_outcome {
    const atlas_review_entry *entry;          /* borrowed for the callback */
    atlas_review_verdict verdict;
    atlas_buf status;                         /* the record's status as read, or empty */
    atlas_buf detail;                         /* a §Frozen detail line, or the refusal message; fixed Atlas text with checked values, or safe-encoded */
    int64_t current_revision_no;              /* MOVED: what the newest is */
    char current_prefix[ATLAS_DECISION_CONFIRM_HEX + 1u];
} atlas_review_outcome;
void atlas_review_outcome_init(atlas_review_outcome *o);
void atlas_review_outcome_free(atlas_review_outcome *o);

typedef atlas_status (*atlas_review_outcome_cb)(const atlas_review_outcome *o, void *ud,
                                                atlas_err *err);

typedef struct atlas_review_totals {
    int64_t ready, applied, abandoned, moved, disposed, missing, refused;
} atlas_review_totals;

/* Walks one sheet. Order of operations, and none may move:
 *   1. atlas_authority_require(ATLAS_AUTHORITY_OP_DECISION_LIFECYCLE) — first, so a
 *      locked profile reads no file and reaches no terminal;
 *   2. unless `check_only`, atlas_terminal_open + close — the refusal is that function's
 *      own sentence, and it happens before the sheet is read;
 *   3. read `sheet_path` with O_NOFOLLOW, bounded by ATLAS_REVIEW_SHEET_MAX_BYTES, and
 *      atlas_review_sheet_parse;
 *   4. per entry, in sheet order: atlas_service_decision_show with revision_no = the
 *      entry's revision (or its remote form, chosen as the rest of the service layer
 *      chooses) → MISSING when the repository or the decision is unknown; MOVED when
 *      summary.latest_revision_no != revision_no or that revision's content_hash does
 *      not begin with the entry's prefix; DISPOSED when summary.status is not the
 *      status the intent needs (PROPOSED for approve and reject, APPROVED for
 *      resolve) — all three without minting; else, under `check_only`, READY; else
 *      atlas_service_decision_confirm(ctx, repo, decision, intent, NULL, revision_no, …)
 *      → APPLIED, or ABANDONED when its refusal is the mistyped-confirmation sentence,
 *      or REFUSED carrying its message.
 * The callback is invoked once per entry, in order, with borrowed pointers. A refused
 * or abandoned entry does not stop the walk. Nothing is retried. */
atlas_status atlas_service_review_apply(atlas_ctx *ctx, const char *sheet_path, bool check_only,
                                        atlas_review_outcome_cb cb, void *ud,
                                        atlas_review_totals *totals, atlas_err *err);
```

- [ ] **Step 1: Write the failing test** — T6 owns the pseudo-terminal suite; this
      task's own test is the `--check` half in `tests/test_review_apply.c`, which needs
      no terminal: against a fixture with one PROPOSED record at revision 1 and one
      revised to revision 2, a sheet naming `r1` for both yields `READY` and `MOVED`
      with the frozen detail; naming a uid that does not exist yields `MISSING`; naming
      `resolve` for a PROPOSED record yields `DISPOSED` ("the record is PROPOSED; resolve
      needs APPROVED"); the `decision_challenges` row count is unchanged across the whole
      call (read it through the database in the fixture before and after).
- [ ] **Step 2: Run it and watch it fail** — link failure.
- [ ] **Step 3: Implement `service_review.c`** per the contract; add to `atlas_core`.
      The ABANDONED mapping keys on the confirm's own error message being exactly the
      `service_decision.c:1809` sentence — pin that sentence as one `static const char[]`
      declared in `include/atlas/service.h` and used by both sites, compared whole with
      `strcmp`, so the two spellings cannot drift apart.
- [ ] **Step 4: Run the test and watch it pass**; `make` with zero warnings.
- [ ] **Step 5: Commit** — `feat(a15): the review walker pre-checks before it mints and loops the one confirm function`

---

### Task T5: the CLI command family, in all five places

**Files:**
- Modify: `src/cli/cli.c`, `src/cli/render.h`, `src/cli/render_human.c`,
  `src/cli/render_json.c`

**Interfaces produced:**

```
atlas review apply FILE [--check] [--json]     # --json only with --check
```

```c
/* src/cli/render.h — one method on the vtbl, implemented in both renderers */
atlas_status (*review_entry)(atlas_renderer *r, const atlas_review_outcome *o, atlas_err *err);
/* plus the totals through list_end's singular/plural ("entry", "entries") and a
 * review_totals method; both renderers implement both. */
```

- [ ] **Step 1: Add `"review"` to `COMMANDS[]`** (`cli.c:2672-2678`) first — the place
      that gets forgotten.
- [ ] **Step 2: Dispatch and help text**: `run_review` refuses `--yes` and `--json`
      without `--check` with the §Frozen sentences; checks operand shape
      (`usage: atlas review apply FILE [--check]`); calls `atlas_service_review_apply`;
      renders through the vtbl; maps totals to the exit code (`0` / `8`).
- [ ] **Step 3: Both renderers.** Human: the §Frozen table; the decision id shortened to
      its first 8 hex plus `…` only in the human form; repository names safe-encoded at
      output. JSON: the §Frozen document, `check: true`, `verdict` as the vocabulary
      name, `detail` null when empty.
- [ ] **Step 4: Run the built binary once**: `./build/atlas review apply /nonexistent`
      must answer with anything but `unknown command` — on a machine whose authority
      profile is locked that is the authority refusal, because authority is checked
      before the path is opened; on an unlocked one it is the file error. Assert only
      the absence of `unknown command`. `./build/atlas review` must print the usage
      line.
- [ ] **Step 5: Commit** — `feat(a15): atlas review apply — a sheet walks the operator channel one entry at a time`

---

### Task T6: the pseudo-terminal suite, and what it establishes

**Files:**
- Create: `tests/support/pty.h`, `tests/support/pty.c`
- Modify: `tests/test_decision_operator.c` (include the support header; delete the local
  copies at `:195-350`), `tests/test_review_apply.c`, `tests/CMakeLists.txt`

- [ ] **Step 1: Move `pty_spawn`, `pty_expect`, `pty_type`, `pty_wait`** into
      `tests/support/pty.{h,c}` unchanged in behaviour (the one signature change: the
      `env` parameter becomes the data directory and the binary path they actually
      use). `test_decision_operator` must
      pass unchanged before anything else is written. Commit —
      `test(a15): the pseudo-terminal helpers move to tests/support, unchanged`.
- [ ] **Step 2: Write the failing tests** in `tests/test_review_apply.c`, `integration`
      label, each against a fresh fixture with the authority profile unlocked as
      `test_decision_operator.c:70` arranges:
      (a) **a sheet on stdin is refused before anything is minted**: run
      `atlas review apply /dev/stdin` with a pipe on stdin and no pty; the message
      contains "interactive terminal"; `decision_challenges` count unchanged.
      (b) **`--yes` is refused** with the frozen sentence; **`--json` without `--check`**
      is refused with the frozen sentence; both mint nothing.
      (c) **a two-entry sheet under a pty**: entry 1 current, entry 2 revised after the
      sheet was written. Expect the prompt for entry 1 to show `revision   : 1` and the
      digest; type the eight hex; expect `APPLIED`; expect **no prompt** for entry 2 and
      `MOVED` with `reviewed r1 (…), now r2 (…)`; `decision.history` shows one `APPROVED`
      event with `actor: LOCAL_OPERATOR_CONFIRMED` and `operator_channel: true`;
      `decision_challenges` grew by exactly one.
      (d) **a mistyped confirmation** on entry 1 of a two-entry sheet: `ABANDONED`,
      the challenge unconsumed, and entry 2 still prompted and applied; exit code `8`.
      (e) **an already-approved record** on an `approve` line: `DISPOSED` with
      "the record is APPROVED; approve needs PROPOSED"; nothing minted.
      (f) **a locked profile**: with the authority policy absent, the walk refuses before
      the sheet file is opened — prove it by naming a path that does not exist and
      asserting the refusal is the authority sentence, not the file error — following
      `test_the_prompt_is_never_reached_in_a_locked_profile` (`:351`).
      (g) **the repository is never modified**: `fx_tree_digest` before and after (c).
      (h) **`grep -c atlas_decision_apply_in_tx src/core/service_review.c` is 0** and the
      three-caller assertion the tree already carries still holds — encode as the
      existing source-walk shape in `test_decision_mcp.c:456` region, or in this file.
- [ ] **Step 3: Run and watch them fail** for the right reasons.
- [ ] **Step 4: Make them pass** — this is the executor's first real read of the walker
      under a terminal; fix the walker, never the tests' expectations.
- [ ] **Step 5: Establish what the lifecycle does with a pinned, non-newest revision.**
      Write `test_a_pinned_revision_that_is_not_the_newest` — `atlas decision approve
      proj <uid> --revision 1` under a pty while revision 2 is PROPOSED — and assert
      whatever the tree does (refusal, or approval of revision 1 with revision 2 left
      PROPOSED, or something else), in words, in the test's comment. **This plan asserts
      nothing about it.** The answer goes into `docs/decision-lifecycle.md` in T9 beside
      the `--revision` flag.
- [ ] **Step 6: Commit** — `test(a15): the walker under a pseudo-terminal — refused on a pipe, moved before minting, abandoned without spending`

---

### Task T7: Mission Control's Review view

**Files:**
- Modify: `src/gw/ui/mission-control.html`, `tests/test_gw_remote.c`

**What the view is:**

- `VIEWS` gains `["review","Review"]` after `"decisions"`. A section `v-review` with a
  repository select `w-pick`, a status select fixed to `PROPOSED` (default) and
  `APPROVED`, a three-column grid: **Records** (the list, `decision.list` with
  `limit: 200`, columns id, kind chip, status tag, title as `untrusted`, `rev` as
  `revision of latest_revision`), **Detail**, and **Review sheet**.
- **Detail** for a selected record, composed in this order, each in its own panel or
  heading, each failure a readable line as `panel()` already guarantees:
  1. `decision/history`: the `revisions[]` table (number, state tag, created, proposed
     by, first 12 of content hash, title `untrusted`); clicking a revision loads
     `decision?revision=N` and renders its prose and links exactly as `showDecision`
     does today (reuse that function with a revision argument); the `timeline[]` table
     (event, actor, revision, `operator channel` as a tag, superseded by); a
     `ledger agrees` row showing `ledger_agrees` as `ok`/`bad`, with `ledger_detail`
     when false — reported, never repaired.
  2. `verify/claims?decision=<uid>`: the claims table (claim, state, truth, basis, text
     `untrusted`, and a `conflict` column); clicking a claim renders the existing
     `showClaim` into the detail. Where a claim's `conflict` is `IMPLEMENTATION`, the
     frozen label from §Frozen formats appears beside it; otherwise nothing about drift.
  3. `gate?repo&decision=<uid>` when the record is `APPROVED`: the `decisions[0]` row's
     `freshness` tag, `validated_at_commit`, `content_hash` first 12, and the
     repository-level `result`/`indexed_commit`; for a PROPOSED record, whatever T2's
     test established, rendered without invention.
  4. **Impact**: for the first `REVIEW_UI_MAX_IMPACT_LINKS` (8) links of the newest
     revision whose kind is `PATH` or `SYMBOL`, one `code/impact` walk each (`depth: 2`),
     rendered as the Impact view renders one, with the same "graph paths, not
     predictions" line; when more links exist, a line saying how many were not walked.
  5. **Queue buttons**, enabled by status: `Queue: approve` and `Queue: reject` when
     `PROPOSED`; `Queue: resolve` when `APPROVED` and the kind is `OBLIGATION` or
     `ACCEPTED_RISK` — the page's mirror of `atlas_review_intent_allowed` and
     `atlas_decision_kind_resolvable`; the C predicate is the authority and the page is
     courtesy, exactly as `/auth/me`'s scope list is (`gateway.c:1339-1341`).
- **Review sheet** panel: the queue from `localStorage` key `atlas.review.sheet.v1`
  (every read and write in `try/catch`; a throwing store renders an empty queue and a
  line saying the browser kept nothing). A queue button records the revision the detail
  is *displaying* — `latest_revision` from the list row, and the first eight hex of the
  `content_hash` returned by `decision?revision=<that number>` — never a hash from a
  different revision, so the sheet names the bytes the operator read. Each entry is
  re-read on view load through `decision?revision=<queued revision>` and marked
  `current`, `moved` (the record's `latest_revision` is no longer the queued one, or that
  revision's hash prefix differs) or `disposed` (status changed), with a remove button;
  a read-only `<textarea>` holding the
  sheet text in the §Frozen grammar, built only from values the page validated
  (`/^atlas-dec-[0-9a-f]{32}$/`, `/^[0-9a-f]{8}$/`, a positive integer, a repository
  name taken from `repos[]`) — the page **validates rather than escapes**, A2's renderer
  rule; a `Copy` button (`navigator.clipboard.writeText`, falling back to selecting the
  textarea) and a `Clear` button; and the frozen sentence under it, verbatim.
- The Overview's "Awaiting approval" panel (`:99`, `:371-388`) gains a link to the Review
  view; its heading is unchanged.
- Everything is `textContent`; nothing is `innerHTML`; no new external resource; the
  CSP at `gateway.c:1366-1373` is untouched.

- [ ] **Step 1: Write the failing test** in `tests/test_gw_remote.c`
      (`test_mission_control_carries_the_review_view`): fetch `/` and require the
      bindings `v-review`, `atlas-review-sheet/1`, `atlas.review.sheet.v1`,
      `decision/history`, `ledger_agrees`, `operator_channel`, `verify/claims`,
      `IMPLEMENTATION conflict`, `review apply`, and the frozen sentence's needle
      `names the channel, not a person`; require the absence of `innerHTML`; require the
      absence of the bare phrase `implementation drift`. Then, through a session cookie,
      drive `decision/history`, `decision?revision=1`, `verify/claims?decision=`,
      `gate?decision=` and `code/impact?symbol=` — the routes the view names — and
      require `200` on each. **State in the test's header comment that no JavaScript is
      executed.**
- [ ] **Step 2: Run and watch it fail** on the first missing binding.
- [ ] **Step 3: Write the view.** Reuse `table`, `kv`, `tag`, `statusTag`, `kindTag`,
      `verifyTag`, `truthTag`, `panel`, `api`; add nothing that duplicates them.
- [ ] **Step 4: Run the test and watch it pass**; `make` (the page is re-embedded by the
      custom command at `CMakeLists.txt:146-150`).
- [ ] **Step 5: Commit** — `feat(a15): Mission Control reads every revision, every claim, the gate and the impact set, and keeps a review sheet that stores no authority`

---

### Task T8: the tripwire learns the new surfaces

**Files:**
- Modify: `tests/test_decision_mcp.c`, `integrations/claude/atlas/skills/atlas-memory/SKILL.md`

- [ ] **Step 1: Extend `FILES[]`** (`test_decision_mcp.c:388-409`) with
      `src/gw/ui/mission-control.html` and `src/core/service_review.c`. Run the test:
      if either already carries a forbidden phrase, that is a finding, and the phrase
      is removed from the source, never from the list.
- [ ] **Step 2: Extend the required-wording table** (`:427-455`) with
      `{mission-control.html, "names the channel, not a person"}`,
      `{mission-control.html, "stores no authority"}`, and
      `{SKILL.md, "atlas review apply"}`.
- [ ] **Step 3: Edit `SKILL.md:54-59`** so the paragraph names both commands: a proposal
      becomes policy only when somebody runs `atlas decision approve` or walks a review
      sheet with `atlas review apply` on a terminal; **do not run either yourself**, and
      do not write a review sheet on a user's behalf — a sheet is what a person decided
      in a browser, not a thing a model prepares.
- [ ] **Step 4: Run `test_decision_mcp` and `test_plugin`** and watch both pass.
- [ ] **Step 5: Commit** — `test(a15): the page and the walker join the tripwire, and the skill names the second command it must not run`

---

### Task T9: documentation, the season rules, and the roadmap's ordering

**Files:**
- Create: `docs/review-surface.md`
- Modify: `docs/roadmap.md`, `docs/remote-access.md`, `docs/decision-lifecycle.md`,
  `docs/engineering-rules.md`, `docs/extending.md`, `docs/backlog.md`, `SECURITY.md`,
  `README.md`, `CLAUDE.md`

- [ ] **Step 1: `docs/review-surface.md`** — the season's document in the shape of
      `docs/context-reconciliation.md:1-27`: the sentence, what is true today in the
      precise form (26 rows, six literal paths, the audit row, the session table, the
      `SO_PEERCRED` fact beneath), the tier decision with its chain, what tier 1 leaves
      undone, tier 3's full cost list from §The decision, the nine decisions, the frozen
      formats, the stated costs (one browser; ssh; no JavaScript executed; a dozen audit
      rows per review), and what T2 and T6 established.
- [ ] **Step 2: `docs/roadmap.md`** — retitle `:1478` to "Later: A14 — …" and `:1558`
      to "Next: A15 — the review surface, and where a proposal is disposed of"; replace
      the "26 routes and not one of them mutates" sentence (`:1573-1577`) with the
      precise form; temper "A12.1's drift finding where there is one" (`:1570`) to the
      one producible shape; record that tier 1 was chosen and why, and that tier 3 is
      costed in `docs/review-surface.md`. Every sentence against the tripwire list.
- [ ] **Step 3: `docs/remote-access.md`** — the Review view under Mission Control; the
      three forwarded parameters; the sentence that the browser session and the bearer
      token are one principal type and what follows for disposal.
- [ ] **Step 4: `docs/decision-lifecycle.md`** — `atlas review apply` under CLI beside
      `decision approve`; the sheet grammar by reference; the honest contract restated
      once ("names the channel, not a person" — the same words the page uses); T6 step
      5's answer beside `--revision`; T2 (b)'s answer beside the gate.
- [ ] **Step 5: `docs/engineering-rules.md`** — "A15 layers — additions" and "A15 rules —
      these are not negotiable" in full, from §Authority argument. **`docs/extending.md`**
      — "A15 — the review surface": adding a sheet intent (the predicate, the grammar
      test, the page's mirror), adding a verdict (UNKNOWN stays zero; no `default:`),
      adding a parameter to a route row (T2's shape; the property test), changing a
      sheet bound (the retention chain), and "Bounds this season added".
- [ ] **Step 6: `docs/backlog.md`** — the Impact view defect and its fix; the two costs
      tier 1 leaves (no cross-device queue; disposal needs a shell on the machine); the
      tier 3 cost list as the entry a future season starts from; the `code.impact`
      `symbol` finding's date.
- [ ] **Step 7a: the exit-code contract.** `8` from `atlas review apply` is a contract
      change: add a row to `README.md`'s exit-code table beside the gate's `8`/`9`
      (`README.md:270-276`, which says those are outcomes rather than errors — this one
      is too), and the same line under `CLAUDE.md`'s "Exit codes (stable contract)"
      (`CLAUDE.md:1617`).
- [ ] **Step 7: `SECURITY.md`** — one paragraph under the A4 section: a review sheet is a
      list, not a capability; it has no confirmation field; the channel is unchanged.
      **`README.md`** — the command in the usage list.
- [ ] **Step 8: `CLAUDE.md`** — the season paragraph at the top in the register the
      others use, the table row, the one-line rules under "### A15 — the review
      surface", and `docs/review-surface.md` in "Where things are documented". Every
      line against `test_decision_mcp.c:371-386`.
- [ ] **Step 9: Run `test_decision_mcp`** (it scans `CLAUDE.md`, `docs/roadmap.md` and
      `docs/decision-lifecycle.md`) and the full suite.
- [ ] **Step 10: Commit** — `docs(a15): the review surface — what was chosen, what it costs, and what tier 3 would cost`

---

### Task T10: live acceptance on this machine — deploy, read, dispose of two probes

Run by the reviewing session with the operator acting. **It spends no money**; it does
touch the live machine.

- [ ] **Step 1: Full suite, then deploy** with `/opt/atlas/deploy.local.sh` (install
      and daemon restart; the three traps that fake a failed deploy are in the working
      notes). `atlas daemon ping --json` reports the new phase once
      `include/atlas/atlas.h:11` says `"A15"` (the final chore commit, as `8dd07c6` did
      for A12.1).
- [ ] **Step 2: Read.** The operator opens Mission Control with the `mission-control` key
      and reviews the two records titled "PROBE-A8FINAL-… disposable" in the Review
      view: every revision of `atlas-dec-c711a6…` (three), the timeline, the claims
      panel (expected empty — say so, never "no evidence"), the gate panel's answer for a
      PROPOSED record as T2 established it, the impact panel (both have zero links —
      expect the "nothing to walk" line). Queue `reject` for both. Copy the sheet.
- [ ] **Step 3: Dispose.** Save the sheet on the machine; `atlas review apply FILE
      --check --json` first (both `READY`); then `atlas review apply FILE` under the
      operator's terminal; type each prefix; expect two `APPLIED`, exit `0`;
      `atlas decision history atlas atlas-dec-…` shows `REJECTED` by
      `LOCAL_OPERATOR_CONFIRMED` with `operator_channel: true` for each; `atlas doctor`
      reports the ledger agreeing.
- [ ] **Step 4: Record the observations** in `docs/review-surface.md` — as observations,
      with the date, the request count the audit view shows for the review, and anything
      the page did that the grep test could not have seen. **Nothing about a live pass
      is a general result; say so in those words.**
- [ ] **Step 5: Final commits. Nothing is pushed on this document's authority.** Present
      the season's commit list, ask, and push only on the operator's contemporaneous
      go-ahead — recording the answer either way.

---

# Acceptance — the roadmap's job sentence and the brief's requirements, mapped

| # | Requirement | Discharged by | The assertion that proves it |
| --- | --- | --- | --- |
| 1 | every revision of a record is readable in the browser | T2, T7 | `decision?revision=N` returns revision N after a revise; the page binds `decision/history` and renders `revisions[]` |
| 2 | its evidence, counter-evidence, aggregate and reasons | T7 | the review view drives `verify/claims?decision=` and `verify/claim` (existing routes) and binds their fields |
| 3 | the gate results | T2, T7 | `gate?decision=` returns one row for an APPROVED record; the PROPOSED answer is established by test, not asserted |
| 4 | the impact set | T2, T7 | `code/impact?symbol=` is `200` (today refused); the page walks ≤ 8 links and says when more were not walked |
| 5 | A12.1's finding where there is one, and no wider | T7, T8 | the frozen `IMPLEMENTATION conflict` label is present; the bare phrase `implementation drift` is absent |
| 6 | a queue that stores no authority | T3, T7 | the grammar refuses a sixth field; the sheet is `localStorage` text built from validated values; no route, scope or method was added (T1's property) |
| 7 | one local command walks the queue through the existing capability | T4, T5, T6 | (c): one challenge per applied entry, ledger event `LOCAL_OPERATOR_CONFIRMED` with `operator_channel: true`; `service_review.c` contains no `atlas_decision_apply_in_tx` |
| 8 | the threat model moves by nothing | T1, T2 | every route's method is outside the operator, gateway-write and remote-forbidden sets and every scope is grantable; three rows edited, zero added |
| 9 | a piped queue cannot dispose | T6 (a) | `/dev/stdin` on a pipe is refused with the terminal sentence and `decision_challenges` is unchanged |
| 10 | a moved record is refused before anything is minted | T4, T6 (c) | `MOVED` with the frozen detail; challenge count grew by exactly one for the two-entry sheet |
| 11 | nothing claims a person acted; the page is scanned | T8 | `FILES[]` includes the page and the walker; the required needle is present; `test_decision_mcp` passes |
| 12 | the roadmap's ordering and the season's rules | T9 | A14 "Later", A15 "Next"; `CLAUDE.md` and `engineering-rules.md` carry the rules; the tripwire passes over them |
| 13 | tier 3 is costed, not built | §The decision, T9 | `docs/review-surface.md` carries the six non-negotiables with mechanisms and the three further costs; no migration 31 exists in the tree |
| 14 | one real read-and-dispose on this machine | T10 | two probe records `REJECTED` through a sheet, observations recorded as observations |

---

# Self-review (planner)

**1. Spec coverage.** The roadmap's job sentence is items 1–5; the tier choice is made
openly with its chain and the roadmap's non-goals hold (no approval from a model surface;
no shared credential that reads and disposes — the sheet is not a credential; tier 3 is
described as weaker in the same paragraph that costs it). The brief's five
non-negotiables are restated with reasons in §Authority argument. The brief's tempering
of "drift" is Decision 5. Numbering is stated and the roadmap edit is T9 step 2.

**2. Sentences of the recurring defect class, hunted.** Every line reference was read
this session; the brief's "27 routes" is reported as not reproducible rather than
adopted; the advisor's row numbers for `gate` and `decision` were checked and the tree's
(`:808`, `:804-805`) are used. Two things this plan deliberately does not claim, because
nothing read establishes them: what the lifecycle does with a pinned non-newest
revision (T6 step 5) and what `gate.check` returns for a PROPOSED record (T2 (b)). Both
are test obligations whose answers are written into the documents afterwards.

**3. Placeholder scan.** No TBDs. Every grammar rule, refusal sentence, verdict, route
row, UI sentence, exit code and bound is written out. Deliberately left to the executor
and named as such: C and JavaScript bodies; the exact member list of the daemon-read
path inside `service_review.c` (local or remote, chosen as the service layer already
chooses); and how the route-view accessor is populated (a parallel static table or a
one-time copy — a location, not a design).

**4. Type consistency.** `atlas_review_entry` is produced by T3's parser and consumed by
T4's walker and T5's renderers; `atlas_review_verdict`'s eight members match the
§Frozen enum and the JSON `verdict` names; `atlas_review_intent_allowed` is defined in
T3 and mirrored by name in T7's button logic; the three route rows in §Frozen formats
are the rows T2 writes and T7 drives; `ATLAS_REVIEW_SHEET_MAX_ENTRIES` is the loop bound
in T3's 65-entry test and the array bound in `atlas_review_sheet`.

**5. What this plan does not settle, and says so.** Whether tier 3 is ever built, and
which key the policy would name; a cross-device queue; disposing with no shell on the
machine; `supersede` and `revalidate` on a sheet; a browser test that executes the page —
all in the backlog by T9 step 6, none silently dropped.
