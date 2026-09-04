# A16 — Browser disposal: a knowledge record is approved, rejected or resolved from Mission Control, through a remote operator channel that says what it is worth — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: `superpowers:subagent-driven-development`
> (or `superpowers:executing-plans` for inline execution). Tasks are dispatched one at
> a time; the reviewing session reads every diff before the next task starts. Steps use
> checkbox (`- [ ]`) syntax. **T1 must not be dispatched until §Decisions the operator
> must be asked has been put to the operator and answered** — that section exists
> because A15's plan left a choice in the document and the choice died there.
> *(Amended 2026-09-04: row 1 of that section — TLS in front — was asked and answered
> no; rows 2 and 3 — which kinds may be disposed of from the browser, and whether the
> page may remember the disposal key for the tab's session — remain open and are both
> asked before T1. The gating question is no longer the TLS one.)*
>
> **Deviation from the writing-plans skill, stated deliberately, exactly as the A12.0,
> A12.1 and A15 plans stated it:** the operator has assigned roles — the planner (Fable)
> writes this plan and reviews; the executors (Opus) write all production code. This
> plan therefore pins *interfaces, vocabularies, refusal sentences, policy keys, route
> rows, UI wording, migration shape and test obligations* exactly, and leaves C and
> JavaScript function bodies to the executor. Where the skill says "show the code", this
> plan shows the contract the code must satisfy. Everything else in the skill applies:
> bite-sized steps, TDD, frequent commits, no placeholders, no "similar to Task N".

**Goal:** The operator disposes of a knowledge record — approves, rejects or resolves it
— from the browser they already review it in, on a phone with no shell, through a
channel that records its own name and its own weakness in the ledger. The terminal
channel A4 built and the sheet walker A15 built stay exactly as they are; nothing this
season adds is reachable from a session cookie, from the anonymous floor, from any MCP
tool, or from any credential a model has ever been handed.

**Architecture:** One new actor, one new ungrantable scope, one new stored fact on a
challenge (which channel minted it, and which credential), one new column in the
ledger (which credential), one migration (31), two root-owned policy keys in
`/etc/atlas/gateway.conf`, one new daemon method group offered to the gateway's uid
only when the policy declares TLS in front and names a disposal credential, a second
route table in the gateway for two `POST` routes whose bodies are query-string syntax
and whose credential is a bearer token presented on the request itself, one
root-owned key by which the operator accepts a cleartext disposal channel where the
policy declares no TLS in front (amended 2026-09-04; the honest paragraph below), and a
"Dispose from this browser" panel in Mission Control's Review view. Every write still
reaches the index through `atlas_decision_apply_in_tx`, which keeps exactly three
callers; `atlas_service_decision_confirm` keeps exactly two.

**Sentence the season exists for:**
> **THE OPERATOR BUILT THE BROWSER SO AS NOT TO USE THE TERMINAL, AND A CHANNEL THAT
> IS WEAKER BY CONSTRUCTION IS STILL A CHANNEL — PROVIDED THE LEDGER SAYS WHICH ONE.**

**The honest paragraph, in the same breath as the capability.** The remote operator
channel is *weaker than the local channel by construction*, and nothing in this plan
makes it stronger — only reachable from further away. The local channel's whole worth
was that the capability never touched a network: a local process, the operator's own
uid, `/dev/tty`, a single-use token that lived for 120 seconds inside one machine. On
this channel the operator's disposal credential passes through the gateway process —
a network-facing process A9 designed to hold no authority — and through whatever
terminates TLS in front of it, and Atlas verifies neither. A compromised gateway holds
that credential for as long as a request carrying it is in flight, and a holder of the
credential disposes exactly as the operator does. The ledger therefore records every
such act as `REMOTE_OPERATOR_CONFIRMED` with the credential's id beside it, never as
`LOCAL_OPERATOR_CONFIRMED`, so that a reader of any row ever written can still tell
the two apart. Every document this season writes says this in the paragraph that
announces the capability, and `tests/test_decision_mcp.c` scans two of them for it.

**Amended 2026-09-04, on the operator's authority, after the plan was committed at
`c305f40`.** The paragraph above says the credential passes "through whatever
terminates TLS in front of" the gateway. On this deployment nothing does. This plan
as written made TLS in front a hard requirement — the daemon refused to offer the
channel under `tls_mode = NONE`, the loader refused a disposal credential there, and
§The decision refused to add a cleartext opt-out, giving four reasons: that a read
floor leaks data while a cleartext disposal credential leaks authority; that an Atlas
API credential has no expiry, so one capture on the wire is durable disposal power
until it is revoked; that what it writes is the one record Atlas cannot rebuild,
under a channel identity the ledger keeps for ever; and that the operator had been
given "TLS in front" as a non-negotiable when choosing tier 3. The operator was shown
that chain on 2026-09-04 and answered, verbatim: *"https ye gerek yok. kendi aginda
sacmalamasin zaten"* — no need for HTTPS, it is on my own network. That is their
decision on their own network. The requirement does not disappear: it becomes a
root-owned policy key by which the operator states, in writing, that they accept a
cleartext disposal channel — `operator_accepts_cleartext_disposal = yes` — absent by
default, refused under TLS, printed by `atlas gateway status` on every run, and
reported by `/auth/me` to the page so the sentence appears at the moment of use. The
gate keeps its `REVERSE_PROXY` requirement *unless* that key is present: one
condition added, none removed, so a reader of the gate still sees TLS as the intended
shape and this deployment as a deliberate departure from it. The cost, unsoftened:
**on this deployment the disposal credential crosses the network in the clear;
anyone able to observe traffic on that segment can capture it; and a captured
credential disposes of records exactly as the operator does until `atlas api-key
revoke` is run.** The nginx terminator task is removed (§Tasks says where and why).
Every other requirement — the channel identity and migration 31, the ungrantable
scope, absence from MCP, the daemon verifying the credential inside the spending
transaction, replay bound to the content hash, the root-owned kinds policy — is
unchanged by this amendment.

**Tech stack:** C17, the existing A4 write point and challenge table, the existing A9
gateway and its policy loader, hand-written HTML/CSS/JavaScript with no build step
(embedded by `tools/atlas_embed.c`), SQLite migrations on migration 13's and 15's
rebuild pattern, the first-party test harness, and the P0 precedent for injecting a
policy into a real daemon through `atlas_daemon_opts` and a tool binary.

**Spec:** the brief this plan was written from (the controller's message of
2026-09-04); `docs/review-surface.md` §3 (A15's tier-3 cost list, lines 149–206);
`docs/plans/2026-09-03-review-surface.md` §The decision and §Authority argument with
all four execution amendments; `docs/roadmap.md` "Next: A15" (lines 1558–1661), whose
tier-3 bullet list is the roadmap's own statement of the six non-negotiables.

**Season number: A16.** `docs/roadmap.md:1478` holds A14 as "Later" (remote job
submission, reserved by `752686e`), and A15 shipped. A16 is the next unused number.
A14's section is left exactly as A15's plan left it, for A15's reason: renaming a
committed section touches its sentence for no gain, and this roadmap's numbers already
do not follow time. T9 retitles "Next: A15 …" to "A15 … (shipped)" and adds "Next:
A16 …" above the invariants.

**The tree this plan was read at:** `acbd7ad`, 2026-09-04, `main`, clean. Every line
number below was read at that commit and is cited together with the function it is
in, because A15 measured that twelve of its own plan's sentences were false by the time
execution reached them and four of its amendments were about line numbers alone. Where
a function name and a number disagree, the function name is the pin.

---

## Global constraints (repo-wide, every task inherits these)

- Warnings are errors (`ATLAS_WERROR=ON`). No new third-party dependency, no
  `FetchContent`, no network at build time. No shell. **No yyjson call site in
  `src/gw`** — Decision 7 removes the need. No Python, Node, Go or Rust anywhere in the
  build, tests or runtime; the page stays hand-written.
- **Never modify a registered target repository.** A16 adds no writer of any kind
  against a tree. The daemon-side tests prove it with `fx_tree_digest`
  (`tests/support/fixture.h:91`).
- **`atlas_decision_apply_in_tx` keeps exactly three callers** — `src/ai/ai.c:864`, the
  `atlas_decision_apply` wrapper at `src/decision/lifecycle.c:2080`, and
  `src/verify/autolifecycle.c:678`. **`atlas_service_decision_confirm` keeps exactly
  two** — `src/cli/cli.c:2069` and `src/core/service_review.c:407`. Both tripwires
  (`tests/test_decision_mcp.c:518-556`, `tests/test_review_apply.c:1439-1470`) count
  *files containing the substring with its opening parenthesis*, comments included
  (`scan_for_needle`). The two new source files this season creates —
  `src/ipc/server_remote.c` and `src/decision/remote.c` — must therefore never contain
  either function's name immediately followed by an opening parenthesis — the exact needle the scans use —
  even in prose; name either function without its parenthesis. Why the counts do not
  move is Decision 3's last paragraph.
- Tests always override the data directory (`fx_open` / `--data-dir`); daemon tests
  additionally override `XDG_RUNTIME_DIR`; no test opens the real database or the real
  socket, and no test installs, enables or starts a systemd unit. No test reads
  `/etc/atlas/gateway.conf`; the daemon under test receives its policy through
  Decision 13's channel and nothing else.
- A new `.c` file goes in the explicit `atlas_core` source list in `CMakeLists.txt`
  (`:158` onward; there is no `file(GLOB)`). A new test goes in `ATLAS_TESTS` in
  `tests/CMakeLists.txt` **and** in one of the `set_tests_properties(... LABELS ...)`
  lines. A new tool binary follows `tests/CMakeLists.txt:90-91` (`atlas-watch-daemon`).
- Every fallible function returns `atlas_status` and takes an `atlas_err *`; one exit
  path per function; `atlas_buf` owns its allocation; row callbacks receive borrowed
  pointers valid only for the call. **A credential in a struct is wiped, not merely
  freed**: `atlas_decision_op_free` must `memset` the token buffer's bytes before
  releasing them, exactly as `gateway.c:1469-1479` wipes the login key.
- Untrusted text — a title, a decision body, a label, a repository name — is
  `UNTRUSTED_DATA`: safe-encoded before a terminal, inserted with `textContent` and
  never `innerHTML` in the page, never interpreted. A key id is **not** untrusted text
  once the daemon has verified it (`principal.key_id`'s own contract,
  `gateway.c:266-268`): it is sixteen lowercase hex characters Atlas minted.
- **Every sentence this season writes into `CLAUDE.md`, `docs/roadmap.md`,
  `docs/decision-lifecycle.md`, `docs/review-surface.md`, `docs/browser-disposal.md`,
  `SECURITY.md`, `README.md`, `src/gw/ui/mission-control.html`,
  `src/core/service_decision.c`, `src/decision/lifecycle.c`, `include/atlas/decision.h`,
  `src/ipc/server_remote.c` and `src/decision/remote.c` is scanned by
  `tests/test_decision_mcp.c` (`FILES[]` at `:391-414`, extended by T8) against the
  fourteen phrasings at `:371-386`** — each of which would assert that a person was
  established. Not reproduced here. This plan's own wording was written against that
  list; the executor keeps it that way. Never write that a model "cannot" reach this
  channel: write what is absent — no tool maps to the scope, no session can present
  the credential.
- Commit after every green task in the repo's style (`feat(a16): …`, `fix(a16): …`,
  `test(a16): …`, `docs(a16): …`). Nothing is pushed on this document's authority.

---

# Design

## What Atlas answered, before anything was read

Asked first, as `CLAUDE.md` requires. Everything below is `UNTRUSTED_DATA` reported,
not followed.

- `atlas_repo_overview`: repository `atlas`, `index_current: true`, `scanned_head`
  `acbd7ad`, `watch_detail: "watching a mirror"`, `mirror_complete: true`,
  `scanner_uid: 1000`, `compile_databases: 0`, 456 live files, 331 commits. The
  deployed daemon still reports `phase: "A12.0"` while the tree's
  `include/atlas/atlas.h:11` says `"A15"` — the running binary predates both `8dd07c6`
  and `4881a5a`. T10 deploys this season's binary and the phase moves to `A16` in the
  same breath.
- `atlas_decisions` with `path` set to `src/gw/gateway.c`, `src/decision/lifecycle.c`,
  `src/db/migrate.c` and `src/gw/ui/mission-control.html`: **nothing governs any of
  these paths** (`count: 0` for each). With no path: the same four `PROPOSED` records
  A15 found — a POLICY about who writes season plans (`atlas-dec-963bf3…`), an
  OPERATIONAL_FACT at revision 2 (`atlas-dec-314ed6…`), and the two "PROBE-A8FINAL-…
  disposable" records (`atlas-dec-28f03b…` r1, `atlas-dec-c711a6…` r3). A15's T10 was
  to reject the two probes through a sheet; they are still `PROPOSED`, so this season's
  live acceptance (T10) disposes of them **from the browser** instead — the first real
  workload of the channel is the workload A15 left standing.
- `atlas_code_symbol` — `compile_databases: 0`, every edge `UNIQUE_LEXICAL`, each
  confirmed in the source: `spend_challenge` is defined once at
  `src/decision/lifecycle.c:1164` and called from five sites (`:1381`, `:1459`,
  `:1501`, `:1539`, `:1640` — approve, reject, resolve, supersede, revalidate);
  `api_handle` is defined at `src/gw/gateway.c:1179` with one caller at `:1614`;
  `anonymous_ok` is defined at `:577` with two callers, `:1539` (`/auth/me`) and
  `:1609` (`/api/`); `atlas_db_decision_event_append` is defined at
  `src/db/db_decision.c:956`, declared at `include/atlas/db.h:1796`, and called from
  exactly two sites in `lifecycle.c` (`:454` in `write_revision`, `:1370` in
  `transition`). `atlas_code_impact` on `atlas_decision_op` returned zero candidates —
  a struct, not a function; the callers that matter are listed by hand in T3.

## What exists, verified against the tree at `acbd7ad` (2026-09-04)

**A15's tier-3 cost list, line by line, checked.** `docs/review-surface.md` §3 was
written by this planner two days ago. Each of its nine costs is restated below with
what the tree says today; three of them turned out to need more than they said.

1. *"A new member of `atlas_decision_actor` … migration 31 … row-count verification …
   whether `decision_challenges` also needs a channel column … and whatever
   `atlas_db_decision_verify`'s replay would need to check about the new actor, are both
   unread."* **Read now.** The actor vocabulary is `include/atlas/decision.h:250-301`,
   five members, and its introductory comment at `:246-247` says "There are four actors
   and there will not casually be a fifth" — already false by one since A9.2; T1
   rewrites it honestly. `decision_events.actor` is a schema CHECK last widened by
   migration 15 (`src/db/migrate.c:3090-3139`, `M15_EVENTS`, `M15_VERIFY`,
   `M15_CONFIRM`), which rebuilt the table and verified its own row count.
   `decision_validations.actor` admits only `LOCAL_OPERATOR_CONFIRMED`
   (`migrate.c:1580`) and stays that way, because revalidation stays local (Decision
   11). **The replay asks nothing about actors**: `atlas_db_decision_verify`
   (`src/db/db_decision.c:2851-2960`) selects `revision_id, event` per revision's last
   event (`:2856-2865`) and compares the replayed status against the cached columns —
   an actor value is never read. The new actor needs no replay change. That is the
   answer to A15's unread obligation, and it is recorded in T2's migration comment.
   `decision_challenges` **does** need a channel column (Decision 3), and the reason is
   sharper than A15 put it: `op_approve` (`lifecycle.c:1433-1438`), `op_reject`
   (`:1464`), `op_resolve` (`:1506`) and `op_supersede` (`:1580`) each hard-code
   `ATLAS_DECISION_ACTOR_LOCAL_OPERATOR_CONFIRMED` in their `transition` call, so
   without a stored fact on the challenge the actor would have to come from the
   spending request — which is a boolean a request can assert (`decision_ops.h:173-177`
   says exactly why there must not be one).
2. *"`decisions:dispose` in `SCOPES[]` with `grantable = false` … derived at
   verification from a root-owned line."* **As stated, plus one cost A15 did not
   itemise.** `SCOPES[]` is `src/gw/apikey.c:22-33`; `atlas_apikey_scope_grantable` is
   `:58`; `atlas_apikey_create_on` (`src/core/service_apikey.c:85-110`) already refuses
   every non-grantable scope by name. But it also refuses a key with **no** scope at all
   (`:96-101`: "at least one --scope is required; a credential with no scopes could not
   read anything"), and Decision 2 needs exactly such a key. A deliberate form is
   needed, never a silent relaxation.
3. *"Absent from the MCP surface … `tests/test_gw_remote.c` asks `tools/list` and
   `tools/call` for every plausible name."* **The shape exists**:
   `test_the_gateway_holds_no_credential_administration_verb` (`test_gw_remote.c:400-438`)
   is the template. `TOOLS[]` in `src/mcp/mcp_tools.c` maps twelve names, none to a
   write scope; nothing this season adds a tool.
4. *"The daemon refuses to offer the remote-operator method group unless the gateway
   policy declares `tls_mode = REVERSE_PROXY`. On this machine, as configured today,
   that check fails."* **Still true, and the mechanism is where A15 assumed it was**:
   the daemon loads the gateway policy at `src/daemon/daemon.c:222-226` into
   `sctx.gwpolicy` (`:271`), and `atlas_server_peer_is_gateway`
   (`src/ipc/server_gw.c:348-382`) is the uid predicate the dispatcher consults at
   `src/ipc/server.c:1293`. `build/atlas gateway status` today: `ENABLED`,
   `192.168.0.198 port 8799`, `tls: NONE`, `remote_mcp=yes web_gui=yes`, `uid: 992`,
   `origins: 0 allowed`, `anon: (none …)`. §The decision says what this plan does about
   it. **One cost A15 did not itemise**: the daemon loads that policy only when
   `serving_system_index` (`daemon.c:223`); a fixture daemon zeroes it, so the daemon's
   own TLS gate is unreachable from any forked fixture daemon and needs Decision 13's
   test channel.
5. *"Replay protection bound to the content hash — the existing challenge shape."* **As
   stated.** `spend_challenge` (`lifecycle.c:1164-1260`) checks the token (`:1167-1170`),
   the intent (`:1184-1188`), the repository (`:1190`), consumption (`:1195`), expiry
   (`:1200`), the typed confirmation against the *stored* hash (`:1210-1216`), a rehash
   of the stored content (`:1222-1245`) and then consumes (`:1250-1260`). Decision 3 adds
   the channel and the credential to that list; nothing is removed.
6. *"A root-owned policy naming which kinds it may act on, refused rather than
   clamped."* **As stated.** `atlas_gwpolicy_parse_buffer` (`src/gw/gwpolicy.c`)
   refuses an unrecognised key (`:435-442`), refuses a ceiling above its bound rather
   than clamping (`:405-413`), and already refuses a scope list naming a non-grantable
   scope (`:386-395`) — which is exactly the loop that makes Decision 10's "a disposal
   scope can never be part of an anonymous grant" a property of the parser rather than
   a sentence.
7. *"The daemon must authenticate the bearer token itself in the transaction that spends
   the capability."* **As stated, and harder than stated.** `method_gateway_auth`
   (`server_gw.c:80-135`) is the verifier — `atlas_apikey_token_parse`,
   `atlas_db_apikey_lookup`, `ATLAS_APIKEY_STATUS_ACTIVE`, `!scopes_unreadable`,
   constant-time `atlas_apikey_verify` (`:102-111`). But **the gateway holds no bearer
   token for a browser session**: `/auth/login` presents the key once
   (`gateway.c:1462-1470`), `session_put` (`:775-815`) stores key id, label, scopes and
   expiry only, and the key bytes are wiped (`:1469-1479`). So for a cookie principal
   there is nothing the gateway *could* forward for the daemon to verify. Decision 1
   follows from this fact and is not a preference.
8. *"`api_handle` refuses every non-GET method for the whole table."* **As stated**:
   `gateway.c:1192-1197`. A second table (Decision 7).
9. *"The gateway parses no JSON body today except the login key, by hand … a yyjson call
   site in `src/gw` would extend the vendored library's stated contract."* **As stated,
   and avoidable.** `take_login_key` (`gateway.c:870-920`) is the only body reader.
   `build_api_params` (`:1103` region) already parses `k=v&k=v` with per-row typed
   integers and `percent_decode` (`:1057` region, "the *only* place the gateway decodes
   anything"). A POST body in `application/x-www-form-urlencoded` is byte-for-byte that
   syntax. Decision 7 uses it and the yyjson argument never has to be made.

**The listener, the principal and the floor, as they are today.** `atlas_gateway_serve_bytes`
(`gateway.c:1314`) dispatches `OPTIONS` (`:1345`), `GET /healthz`, `POST /mcp` (`:1370`),
`POST /auth/login` (`:1453`), `POST /auth/logout` (`:1522`), `GET /auth/me` (`:1533`),
`GET /` (`:1575`) and then `/api/` (`:1594-1621`). The `/api/` block's comment at
`:1597-1600` — "Either mechanism, one principal … the authorization engine does not
know which was used" — is true of every request that reaches `api_handle`, because the
block resolves a principal from `session_get` first (`:1601`), `authenticate` second
(`:1602`), and the anonymous floor third (`:1609-1611`). **After T6 that comment is true
of reads only**, and T6 amends it: the write table (Decision 7) resolves a principal
from `authenticate` and from nothing else. The anonymous floor is `anonymous_ok`
(`:577-580`): `web_gui` on, a non-zero `web_gui_anonymous_scopes` mask, no
`Authorization` header at all, and `host_matches_listener` (`:480-525`). The mask
itself can never hold a non-grantable bit (`gwpolicy.c:386-395`). The floor is
**not installed** on this machine today (`anon: (none …)`); the mechanism is in the
binary since `e9cfa85` and the `Host` clause since `acbd7ad`. `host_matches_listener`
compares `Host` against `listen_addr:listen_port` whole (`:513-521`) and its comment at
`:527-543` says outright that no key names a reverse proxy's hostname — which matters
to any later deployment that puts a terminator in front — none does after the 2026-09-04 amendment — because a proxy changes what `Host` the gateway sees.

**The operator group and why the gateway reaches none of it.** `OPERATOR_METHODS[]`
(`src/ipc/server_decision.c:2708-2723`) holds `decision.challenge`, `decision.approve`,
`decision.reject`, `decision.supersede`, `decision.revalidate`, `decision.resolve`,
`repo.scanner` and the three `memory.*` methods; `dispatch()` offers it only when
`atlas_server_peer_is_operator(peer_uid)` (`server.c:1250`; the probe at
`server_decision.c:2738-2742`). The gateway is uid 992 and is not that peer, so every
one of those names answers `unknown method` to it. `tests/test_a7_authority.c:195-197`
lists the five decision names and asserts that answer against a live daemon
(`:271-281`). This season adds two names to that list (T8): the remote methods must
answer `unknown method` to a daemon whose policy does not offer them.

**The gateway group, and legacy mode.** `GATEWAY_METHODS[]` (`server_gw.c:335-339`)
holds `gateway.auth`, `gateway.audit`, `gateway.audit_list`, offered at `server.c:1293`
when `atlas_server_peer_is_gateway`. That predicate is `gateway_uid == peer_uid` when a
policy names one (`server_gw.c:354-356`), `false` under a system deployment with no
policy (`:360-362`), and `peer_uid == getuid()` in legacy per-user mode (`:381`) — the
mode every fixture daemon runs in. So a fixture daemon *does* offer the gateway group
to the test's own uid, which is how `tests/test_gw_remote.c` authenticates real keys
today; what it does not have is a non-zero `tls_mode` or any disposal key, because
its `gwpolicy` is zeroed. Decision 13's channel supplies both without touching a
root-owned file.

**The write point and the operator ops.** `atlas_decision_apply_in_tx`
(`lifecycle.c:1988-2072`) resolves the repository, binds and then unconditionally
discards the session for any op that needs a challenge (`:2033-2052`, "sessionless,
explicitly and unconditionally"), and switches on `op->kind` (`:2055-2069`).
`op_challenge` (`:891-1138`) refuses a pinned revision only when it does not exist
(`:911-919`) and otherwise takes the newest (`:934-945`); it binds
`(repo_id, document_id, revision_id, revision_no, content_hash, intent)` (`:956-975`).
`atlas_decision_op` (`include/atlas/decision_ops.h:132-230`) carries `token`,
`confirmation` (`:173-179`), `expect_revision_no` (`:181-184`) and `intent`
(`:185-192`); `atlas_decision_result` (`:242-285`) carries `token`, `title`, `confirm`
and `expires_at` for CHALLENGE (`:259-265`). `method_challenge`
(`server_decision.c:2468-2535`) returns `confirm` to the CLI at `:2528` because the
terminal prompt prints it; `spend_method` (`:2542-2610`) is the one reader of `token`
and `confirmation` (`:2551-2553`) and hard-codes the actor string in its response at
`:2597`. `submit` (`:545-555`) hands the op to `atlas_writer_decision`, which owns the
transaction; that is the path every op this season builds takes.

**The challenge table and its retention.** `decision_challenges` was last rebuilt by
migration 13 (`migrate.c:2578-2614`, `M13_CHALLENGES`); `atlas_db_decision_challenge_insert`
(`db_decision.c:1027-1070`) and `_find` (`:1102-1140`) name its columns; it is
**prunable** to `ATLAS_DECISION_CHALLENGES_RETAIN` (200, `include/atlas/limits.h:612`)
by `atlas_db_decision_challenges_prune` (`:1210-1230`). A fact that must outlive a
challenge cannot live only on the challenge row — Decision 4.

**Bounds this season leans on.** `ATLAS_DECISION_CHALLENGE_TTL_MS` 120000
(`limits.h:604`), `ATLAS_DECISION_CONFIRM_HEX` 8 (`:615`), `ATLAS_DECISION_CHALLENGE_HEX`
32 (`:601`), `ATLAS_GW_MAX_BODY_BYTES` 1 MiB (`:973`), `ATLAS_GW_MAX_SESSIONS` 32
(`:1008`), `ATLAS_APIKEY_SELECTOR_HEX` 16 and `ATLAS_APIKEY_TOKEN_MAX` 80
(`include/atlas/apikey.h:67-76`). `atlas_apikey_record` (`include/atlas/gw.h:56-77`)
carries `scopes` as stored text and `mask`, `scopes_unreadable`, `status`.

**Mission Control today** is `src/gw/ui/mission-control.html`, 1592 lines: `VIEWS`
(`:224-229`) with `review` present; `REVIEW_SHEET_KEY` and the two regexes
(`:236-241`); `REVIEW_UI_MAX_SHEET_ENTRIES` (`:247`); `api()` (`:271-296`), which sends
`credentials: "same-origin"` and no `Authorization` header; `reviewIntentsAllowed`
(`:661-668`); the queue functions (`:680-782`, with the no-`await` invariant
`docs/review-surface.md` §6 records); `renderReviewSheet` (`:805-902`) ending in the
first frozen sentence (`:896-901`); `showReviewDetail` (`:907`); `viewReview` (`:1110`);
`start()` reading `/auth/me` and its `anonymous` field (`:1500-1535`). The page's CSP
(`gateway.c:1585-1587`) is `connect-src 'self'` and inline script only; a same-origin
`fetch` with an `Authorization` header needs no preflight and no CSP change.

**The tripwires and the required wording.** `tests/test_decision_mcp.c`: forbidden
tool-name verbs (`:79-81`); forbidden schema properties (`:132-134`); the fourteen
phrasings (`:371-386`) over `FILES[]` (`:391-414`, twenty-two files, including
`src/gw/ui/mission-control.html`, `src/core/service_review.c`, `docs/review-surface.md`);
the required-wording table (`:433-455`); the three-caller scan (`:518-556`).
`tests/test_review_apply.c:1439-1470` pins the two-caller count of the confirm helper.

**The deployment script.** `/opt/atlas/deploy.local.sh` (gitignored) verifies an
install by grepping the binary for `MARK='a sheet disposes of a record once'` — a
string only A15's code contains. T10 changes it to a string only A16's code contains.

## The decision: tier 3, this season, with the TLS requirement intact

The decision is the operator's and is made; this section does not re-open it and
offers no tiers. What it settles is the one thing the brief said this plan must decide
in writing: what to do about TLS on a machine whose listener is cleartext today.

**The requirement stays hard.** The daemon offers the remote disposal method group only
when the root-owned gateway policy declares `tls_mode = REVERSE_PROXY` **and** names a
disposal credential; the gateway refuses the two write routes with a sentence before
that; and the policy loader refuses — MALFORMED, the gateway disabled with a reason —
a policy that names a disposal credential under `tls_mode = NONE` or without
`web_gui = yes`. On this machine, as configured today, all three refuse. **This plan
deliberately does not add a cleartext opt-out key**, and the argument is the one the
brief asked for — why a read floor and an approval channel are not the same risk,
even to an operator who accepted the first on this same listener today:

1. **What leaks.** The anonymous floor leaks *data*: every read the named scopes cover,
   to anyone on the segment. A cleartext disposal credential leaks *authority*: an
   Atlas API key has no TTL (`api_keys` has `created_at`, `revoked_at`, `last_used_at`
   and nothing that expires it — `migrate.c:2276-2316`), so one capture on the segment
   is durable disposal power until the operator notices and revokes. The 120-second
   challenge is not the credential; the key that mints challenges is.
2. **What it writes.** A read writes a `gw_audit` row. A disposal writes to the one
   record in Atlas that is **not rebuildable** — invariant 1's stated exception
   (`docs/decision-lifecycle.md`, "Two invariants this phase bends") — and the ledger is
   canonical and append-only: a forged approval can be superseded, but its event row
   is history for ever, under a channel identity that says the operator's channel was
   used.
3. **What the channel's name would mean.** `REMOTE_OPERATOR_CONFIRMED` is only worth
   recording if it means "the credential the policy names was presented over the
   transport the policy declares". Over cleartext on a LAN it would mean "whoever was on
   the segment", and a name that can mean that is the retrospective ambiguity the
   requirement list exists to prevent.
4. **The operator was given this list.** `docs/review-surface.md` §3 item 4 and the
   roadmap's tier-3 bullet both say "TLS in front"; the operator chose tier 3 with that
   bill in view. A plan that quietly priced it out would be answering a question they
   did not ask, which is the A15 failure in a new place.

**What the season did instead, as first written: the terminator was in scope.** The
plan as committed at `c305f40` carried a T10 that installed nginx on
`192.168.0.198:8799` with a self-signed certificate, moved the gateway to loopback,
and re-pointed the remote MCP tunnel, with each cost stated; and it said that whether
the operator would run that terminator and trust its certificate was to be asked
**before T1 was dispatched**, that a no meant the season did not start, and that if
the operator wanted cleartext disposal anyway after reading the four points above,
that was a written amendment they author, not a key the executor adds — and that this
plan advised against it.

**Amended 2026-09-04, on the operator's authority.** The question was put to the
operator with the four points above, and the answer was no: *"https ye gerek yok.
kendi aginda sacmalamasin zaten"* — no need for HTTPS, it is on my own network. They
have authored the amendment this paragraph anticipated, for their own network, and it
stands. What changes and what does not:

- **The requirement becomes an explicit, root-owned acceptance, not an absence.** On
  this project's own pattern from the same morning — the anonymous read floor is a
  policy key, absent by default, that `atlas gateway status` prints — the gateway
  policy gains `operator_accepts_cleartext_disposal = yes`. Absent, the loader still
  refuses a disposal credential under any `tls_mode` but `REVERSE_PROXY`, exactly as
  designed. Present, it is allowed. The name reads as what it is: a person's written
  acceptance of a stated risk. Its full grammar and every MALFORMED condition are in
  §Frozen formats.
- **The gate keeps `REVERSE_PROXY` unless that key is present** — one condition added
  to `atlas_server_remote_disposal_offered`, none removed (Decision 9), so the code
  still says TLS is the intended shape and this deployment departed from it on
  purpose.
- **`atlas gateway status` prints the acceptance**, human and JSON, beside the
  `dispose:` line (Decision 16). The review that caught this on the anonymous floor
  said a ceiling may be omitted from that command but an authentication bypass may
  not; this is stronger than a bypass, so it is printed on every run.
- **`/auth/me` reports it to the page**, and the disposal panel shows the chain at
  the moment of use (Decision 15, the fifth frozen sentence).
- **The nginx terminator task is removed** from §Tasks, with a note at its place
  saying so, and the season's task count is ten.
- **Every document the season writes states the chain, verbatim** (§Frozen formats,
  "The cleartext chain"): the credential travels in the clear on this deployment;
  anyone able to observe traffic on the segment can capture it; an A9 credential has
  no expiry, so a capture is durable until revoked; the operator chose this on
  2026-09-04 after being shown that chain. No sentence anywhere implies Atlas
  recommends it.
- **Nothing else moves.** The channel identity and migration 31, the ungrantable
  scope, absence from MCP, the daemon verifying the credential inside the spending
  transaction, replay bound to the content hash, and the root-owned kinds policy are
  exactly as the seventeen decisions below state them.

**The lifecycle gap, and how this channel avoids it.** A15 established that approving a
pinned, non-newest revision succeeds and leaves the newer `PROPOSED` revision stranded
(`docs/decision-lifecycle.md:837-869`), and that `required_status_for` asks the
document's status where the lifecycle keys on the revision's (`docs/backlog.md:2384`).
The remote channel never enters either gap, structurally: a remote challenge is minted
only for the newest revision and refused otherwise, and is refused again at spend if a
revision landed in the window (Decision 5). The local `--revision N` semantics A15
measured are untouched — the plan does not fix a terminal behaviour under cover of a
browser season — and the walker's `required_status_for` stays a backlog item.

**The two defects A15 recorded, decided.** `op_approve`'s "replaced by a **later**
revision" detail (`lifecycle.c:1420-1426`; `docs/backlog.md:2197`) **is this season's to
fix**: T3 rewrites the actor selection in that exact function, the function's diff is
open anyway, the fix is one comparison of `prev_rev_no` against `c.revision_no` and one
of two sentences, and `test_a_pinned_revision_that_is_not_the_newest`
(`tests/test_decision_operator.c`) already constructs the state that exposes it — one
assertion added. Leaving a known-false ledger sentence inside a function whose actor
logic is being rewritten would draw the same review question on every diff. The three
dead read routes (`docs/backlog.md:2232`) **are not**: they are graph reads in the read
table, this season never edits a read row, and A15's argument for leaving that table's
shape alone still holds.

## The seventeen decisions this plan settles, each with its argument

### Decision 1 — A session cookie can never dispose; only a bearer token presented on the request itself, verified by the daemon inside the transaction that spends the capability

The requirement is that the daemon authenticates the credential itself, because a key
id the gateway merely *claims* is worthless (`principal.key_id`, `gateway.c:266-268`,
already states that contract for reads). The tree makes only one implementation of
that requirement possible: the gateway holds no token for a cookie principal
(`session_put`, `gateway.c:775-815`, stores key id, label, scopes, expiry; the login key
is wiped at `:1469-1479`). So the two write routes resolve a principal from
`authenticate(g, req, &pr)` — the `Authorization: Bearer` header on the request — and
never call `session_get` or `anonymous_ok`, and the gateway forwards the token itself
to the daemon as the `token` parameter, exactly as `/auth/login` forwards a presented
key to `gateway.auth`. The daemon verifies it once in `gateway.auth` (so the gateway's
own scope check has a scope mask to check) and **once more at the write point, inside
the transaction that mints or spends** (Decision 3), against the key table as it is at
that moment — so a key revoked between the gateway's check and the writer's turn spends
nothing.

Three things follow and are stated rather than discovered:

- **CSRF is structural.** No cross-site request carries a bearer header; a cross-origin
  `fetch` that adds one triggers the `OPTIONS` preflight, which `gateway.c:1351-1366`
  answers only for a listed origin, and this deployment lists none. `SameSite=Strict` on
  the cookie is not what protects a disposal; the absence of any cookie path is.
- **DNS rebinding is moot for disposal.** A rebound page presents nothing, and nothing
  is exactly what the write routes refuse. `host_matches_listener` is not consulted on
  the write path; it exists for the floor and stays there.
- **The page holds the credential.** Mission Control keeps the disposal key in a
  JavaScript variable for the tab's life, never in `localStorage` or `sessionStorage`
  by default, and sends it only on the two write routes. Whether it may be remembered
  for the tab's session is §Decisions the operator must be asked, row 3.

### Decision 2 — The disposal credential holds no stored scope; the policy derives exactly one; and a key with any stored scope is refused as the disposal credential

"Ungrantable to any model credential" was A15's cost line. Making it structural rather
than a discipline: `decisions:dispose` is never stored on an `api_keys` row — `atlas
api-key create` refuses it by name exactly as it refuses `memory:write`
(`service_apikey.c:102-110`) — and is *derived* by the daemon, in `gateway.auth` and
again at the write point, for exactly the key whose id the root-owned
`remote_dispose_key` line names, **and only if that key's stored scope list is empty**.
A key that can read anything is a key that could have been handed to a model over
`/mcp`; a key that authorises no read lists zero tools there (`tools/list` hides what a
credential may not call; a tool needs a read scope) and is useless to anything but the
two write routes. The operator therefore mints the disposal credential with the
deliberate form `atlas api-key create --label L --no-scopes` (T1), whose success
message says what the key is for and that only the policy line can give it a scope.
This is the one place the season relaxes `service_apikey.c:96-101`, and it does so with
a flag whose name says so, never by accepting silence.

### Decision 3 — The channel is a stored fact on the challenge; UNKNOWN is zero and is refused; the actor comes from the stored challenge, never from the request; and a mismatch is refused in both directions

`atlas_decision_channel` gains three members: `UNKNOWN = 0`, `LOCAL`, `REMOTE`. The
zero is refused at the write point for every op that mints or spends a capability
(`atlas_decision_op_needs_challenge` is the classifier, `lifecycle.c:2033-2052`), and
every producer sets its channel explicitly — `op_new` in `src/core/service_decision.c`
and in `src/ipc/server_decision.c` set `LOCAL`; `src/ipc/server_remote.c` sets
`REMOTE`. The alternative — `LOCAL = 0`, on `knowledge_kind`'s precedent that "a caller
that says nothing means what every caller before meant" — fails the season's first
requirement in the worst way: a forgotten field would record a remote act as
`LOCAL_OPERATOR_CONFIRMED`, which is the one cost that cannot be paid back. A refusal
is the safe failure, so the vocabulary keeps the house rule that zero is unknown.

`op_challenge` stores `channel` and, for `REMOTE`, the verified `key_id` on the
challenge row. `spend_challenge` refuses a challenge whose stored channel differs from
the spending op's (`"…minted through the remote channel and cannot be spent locally"`
and the reverse), and for `REMOTE` refuses one whose stored `key_id` differs from the
credential verified in this transaction. `op_approve`, `op_reject` and `op_resolve`
then choose the actor **from `c.channel`** — the row `spend_challenge` loaded — never
from the op, so the actor is evidence of the path a capability actually took.
`op_supersede` and `op_revalidate` refuse a `REMOTE` challenge outright (Decision 11).

**Why the caller counts do not move.** `src/ipc/server_remote.c` builds ordinary
`atlas_decision_op`s of the existing kinds — `CHALLENGE`, `APPROVE`, `REJECT`,
`RESOLVE` — with `channel = REMOTE`, the forwarded token, and the policy's expected key
id and kinds mask copied onto the op, and submits them through the `submit` shape at
`server_decision.c:545-555` (`atlas_writer_decision`), which runs `atlas_decision_apply`
(`lifecycle.c:2074-2092`) on the writer thread. The credential verification lives in a
new file, `src/decision/remote.c`, called from inside `atlas_decision_apply_in_tx`
after `resolve_repo` and before the op switch, for `channel == REMOTE` only. So
`atlas_decision_apply_in_tx` gains one internal call and no external caller (three
files still name it), and `atlas_service_decision_confirm` — the CLI's terminal helper
— is never on this path at all (two callers, unchanged). The decision layer gains a
dependency on `atlas/apikey.h` and `atlas/gw.h` (`atlas_apikey_token_parse`,
`atlas_apikey_verify`, `atlas_db_apikey_lookup`); that is a stated cost, and the reason
it is placed there rather than in the RPC method is the requirement itself — the check
must be in the transaction, and the transaction belongs to the writer.

### Decision 4 — The credential travels into the ledger, because the challenge table is prunable

`decision_challenges` is pruned to 200 rows (`db_decision.c:1210-1230`). If "which
credential disposed of this" lived only there, it would be gone after two hundred
later challenges — and the ledger is canonical. Migration 31 rebuilds `decision_events`
anyway to widen the actor CHECK, so it adds `key_id TEXT` (NULL for every existing row
and every local row); `atlas_db_decision_event_append` (`db.h:1796-1803`) grows one
parameter; `transition` passes `c.key_id` for a `REMOTE` challenge and NULL otherwise;
`write_revision` (`lifecycle.c:454`) and `op_auto` (`:1963`) pass NULL. `decision.history`'s
timeline (`server_decision.c:1426-1443`) emits `key_id` beside `actor` when present, and
both CLI renderers print it. The detail string on a remote event names the credential
too (§Frozen formats), so the row reads correctly even to a tool that ignores the new
column.

### Decision 5 — A remote challenge is minted only for the newest revision, and is refused at spend if a newer one landed

The browser always pins the revision it displayed (`revision` is a required parameter of
the challenge route; `0` is refused). `op_challenge` under `REMOTE` additionally refuses
when `expect_revision_no != latest_no`, with a sentence naming both; `spend_challenge`
under `REMOTE` re-reads the latest revision and refuses when it is no longer
`c.revision_no`. This is A15's walker pre-check moved to where it belongs for a channel
with no terminal — the write point — and it closes, for this channel only, the
stranded-newer-`PROPOSED` gap A15 measured. The local channel's `--revision N` is
deliberately untouched: it is documented, measured, and a terminal operator naming an
older revision is naming it on purpose.

### Decision 6 — Two root-owned policy keys in `/etc/atlas/gateway.conf`, both refused rather than clamped, and refused outright without TLS

`remote_dispose_key = key_<16 lowercase hex>` and `remote_dispose_kinds = KIND …` go in
the gateway policy and not in `authority.conf`, for three reasons that each suffice:
the file's own stated purpose is to constrain the gateway's principal
(`include/atlas/gwpolicy.h:11-22`), and a credential is that principal; the daemon
already loads it (`daemon.c:222-226`) and the TLS stance the gate needs is in the same
file, so one read answers "is remote disposal on"; and a malformed `authority.conf`
locks the *local* channel (`docs/decision-lifecycle.md:39-48`), so a typo in a remote
line placed there would lock the operator out of their own terminal. A malformed
gateway policy disables the gateway and nothing else, which is the right blast radius.

The parser refuses (MALFORMED, gateway DISABLED with a reason, P0's rule): a key not of
the `key_` + 16-hex shape; a kinds list naming a kind `atlas_decision_kind_parse` does
not know; either key without the other; either key with `tls_mode` other than
`REVERSE_PROXY` **unless `operator_accepts_cleartext_disposal = yes` is present**;
either key with `web_gui = no`; `decisions:dispose` named in
`web_gui_anonymous_scopes` (already refused by `gwpolicy.c:386-395`, tested
explicitly now); the acceptance key with any value but `yes`; the acceptance key under
`tls_mode = REVERSE_PROXY`; the acceptance key without both disposal keys. The
defensible starting line is A15's — `OPERATIONAL_FACT PARKED` — and the operator
chooses (§Decisions the operator must be asked, row 2). **The daemon reads the policy
at start** (`daemon.c:222`), so a policy edit needs a daemon restart as well as a
gateway restart; T10 says so and `atlas gateway status` prints the `dispose:` and
`clear:` lines so an auditor can see what is installed (`acbd7ad`'s reason for the
`anon:` line applies verbatim).

*(Amended 2026-09-04: this decision as committed had no third key and refused a
disposal credential under `tls_mode = NONE` unconditionally. The operator declined
TLS on their own network after being shown the chain in §The decision;
`operator_accepts_cleartext_disposal` is the written form of that acceptance, and its
three MALFORMED conditions above are what keep it from ever reading as a feature
toggle — it cannot be `no`, cannot coexist with TLS, and cannot stand alone.)*

### Decision 7 — A second route table for two `POST` routes whose bodies are query-string syntax, so no JSON is parsed in `src/gw`

`api_handle` refuses every non-GET method for the whole table (`gateway.c:1192-1197`),
and that refusal is right for the table it guards. Rather than a method column that
would put a write and twenty-six reads in one array, `API_WRITE_ROUTES[]` is a second
table with its own handler, `api_handle_write`, consulted after `api_handle` returns
false and before the 404. Each row names its path, its daemon method, its scope
(`ATLAS_SCOPE_DECISIONS_DISPOSE`, and the property test requires that of every row),
and the body parameters it forwards with their integer subset — the same
`params`/`ints` shape as `api_route`. The body **must** be
`Content-Type: application/x-www-form-urlencoded` (415 otherwise), at most
`ATLAS_GW_WRITE_BODY_MAX_BYTES` (4096; 413 above it), and is parsed by
`build_api_params` — the function that already parses `req->query` — run over the body
instead. Tokens therefore never appear in a URL, so never in a proxy's access log, and
the gateway acquires no JSON reader. The gateway appends `token` (the bearer it
authenticated) to the forwarded params itself; no row may declare a `token`,
`confirmation` or `key_id` parameter as *client-supplied* except `confirmation` on the
dispose row, and the property test says so.

### Decision 8 — The challenge response carries the digest and never the confirmation phrase; the operator types eight characters

`method_challenge` returns `confirm` to the CLI (`server_decision.c:2528`) because the
terminal prompt prints it for the operator to read and type. A browser would auto-fill
it, and requirement 5 would be decoration. `decision.remote_challenge` returns the
revision's full `content_hash` — already on screen in the Review view — and `token`,
`expires_at`, `revision`, `kind`, `state`, `title` (encoded), and **no `confirm` key**;
T5's test asserts its absence. The page compares the typed eight characters against
the displayed digest before sending, as courtesy (`service_decision.c:1803-1810`'s
pre-check has the same status locally: a typo spends nothing), and `spend_challenge`
compares them against the *stored* hash as the guard, exactly as today.

### Decision 9 — The daemon-side group lives in its own file, is offered only under three conditions, and answers `unknown method` otherwise; the gateway refuses with a sentence first

`REMOTE_DISPOSAL_METHODS[]` — `decision.remote_challenge` and `decision.remote_dispose`
— lives in `src/ipc/server_remote.c`, beside the A7 comment at
`server_decision.c:2295-2330` rather than inside it, so the argument that the operator
channel is not an ordinary RPC group stays readable as written. `dispatch()` consults
the group additively, after the gateway group, when
`atlas_server_remote_disposal_offered(ctx, peer_uid)` — `ctx->gwpolicy.state ==
ATLAS_GWPOLICY_ENABLED` **and** the gateway peer predicate,
**and** (`ctx->gwpolicy.tls_mode == ATLAS_GWPOLICY_TLS_REVERSE_PROXY` **or**
`ctx->gwpolicy.cleartext_disposal_accepted` — amended 2026-09-04: one condition
added, none removed, and the predicate's comment says TLS is the intended shape and
the acceptance is the operator's written departure from it), **and**
`ctx->gwpolicy.remote_dispose_key[0] != '\0'`; every other peer, and this peer under
any other policy, gets `unknown method`, the answer a name that does not exist gets.
Each method asks the same predicate again for itself, A8's rule that routing is not
authorisation. The gateway, holding its own copy of the same policy, answers
`404 not_found` `"this gateway does not serve remote disposal"` before forwarding when
the key is absent — the shape `/mcp` uses when `remote_mcp` is off
(`gateway.c:1378-1383`) — so an operator debugging the panel gets a sentence and the
daemon's silence stays the guard.

  **Amended again 2026-09-04, and this is the `state == ENABLED` condition above.**
  T4's review measured that a MALFORMED policy still carries a usable disposal key:
  `atlas_gwpolicy_parse_buffer` writes each field as it parses and returns at the
  refusal without clearing what it already wrote — the file's convention for every
  key — and `src/daemon/daemon.c` copies the whole struct into the server context for
  any system deployment, consulting `state` nowhere. A policy Atlas *rejected* would
  therefore have satisfied all three original conditions. The fix belongs in the
  predicate rather than in the parser: the predicate is the one function that decides
  whether the group is offered, and clearing fields at each malformed exit would
  depart from the parser's convention for every other key. The same shape is already
  latent at `src/ipc/server_gw.c:354`, which trusts `gateway_uid` from a rejected
  policy; that one is recorded rather than fixed here.

### Decision 10 — `gateway.auth` derives the scope; `decisions:dispose` is in the vocabulary with `grantable = false`; and that single bit is what keeps the anonymous floor from ever holding it

`method_gateway_auth` (`server_gw.c:80-135`) returns the stored `rec.scopes`; after T5
it appends ` decisions:dispose` when the verified key's id equals
`ctx->gwpolicy.remote_dispose_key`, the stored list is empty, and the group is offered
(Decision 9's three conditions). The gateway's principal mask then carries the bit,
`api_handle_write`'s scope check is the ordinary `atlas_scope_has`, and `/auth/me`
would show it — though the page never logs in with this key (Decision 1). The floor
cannot hold the bit because `web_gui_anonymous_scopes` refuses every non-grantable
scope at parse (`gwpolicy.c:386-395`) and `anonymous_principal` copies only that mask
(`gateway.c:584-590`); and the write path never consults `anonymous_ok` anyway.
Two independent fences, one of them a parser refusal; both tested.

### Decision 11 — Approve, reject and resolve are the remote intents; supersede and revalidate are not offered, at the write point and not only at the method

A15's Decision 7 argument holds one channel over: supersede names a second document and
revalidate binds a pinned repository state and an evidence digest that a browser review
does not establish. `decision.remote_challenge` refuses an intent outside the three;
`op_challenge` and `spend_challenge` refuse a `REMOTE` challenge for `SUPERSEDE` or
`REVALIDATE` intent independently, so the method's check is courtesy and the write
point's is the guard. `decision_validations.actor` stays `LOCAL_OPERATOR_CONFIRMED`-only.

### Decision 12 — `op_approve`'s false supersession sentence is fixed here; the three dead read routes are not

Stated in §The decision; repeated as a decision so the acceptance table can name it.

### Decision 13 — The test channel is an `atlas_daemon_opts` field and a tool binary, never a flag, an environment variable or a policy path override

A fixture daemon zeroes its gateway policy (`daemon.c:222-226`), so the TLS gate and the
key derivation are unreachable from `fx_daemon_start`. P0's precedent
(`include/atlas/daemon.h:41-50`, `tests/tools/atlas_watch_daemon.c`,
`tests/test_watch_budget.c:25-37`) is exactly the right shape: `atlas_daemon_opts`
gains `const char *gwpolicy_text` ("Test hook: gateway policy text parsed with
`atlas_gwpolicy_parse_buffer` in place of the compiled-in path; never set by the CLI"),
`daemon.c:222-226` becomes "inject if given, else load if serving the system index,
else zero", and `tests/tools/atlas_gw_daemon.c` (`atlas-gw-daemon DATA_DIR POLICY_FILE`)
calls `atlas_daemon_run` with it. Everything else the daemon under test runs is
production's — the dispatcher, the predicate, the writer, the write point. The
write-point half is also tested without a daemon, the way
`approve_through_the_write_point` (`tests/test_gw_remote.c`) and
`test_a_pinned_revision_that_is_not_the_newest` already do.

### Decision 14 — Migration 31 rebuilds two tables on their own precedents, and its one default is a true statement about every existing row

`decision_events` is rebuilt exactly as migration 15 rebuilt it (`migrate.c:3090-3139`):
the actor CHECK gains `'REMOTE_OPERATOR_CONFIRMED'`, one column `key_id TEXT` is added,
`id` is copied explicitly because the ledger's order *is* that id, and a named CHECK
(`no_decision_event_may_be_lost_in_migration_31`) verifies the count before commit.
`decision_challenges` is rebuilt exactly as migration 13 rebuilt it
(`migrate.c:2578-2614`): two columns, `channel TEXT NOT NULL DEFAULT 'LOCAL'
CHECK(channel IN ('LOCAL','REMOTE'))` and `key_id TEXT`, with its own named count
check. **`DEFAULT 'LOCAL'` is not migration 19's mistake**: every challenge row that
exists before this migration was minted through the only channel that existed, so the
default records a fact that is true of each of them rather than inventing an intent
nobody expressed. Nothing references either table (migration 15's and 13's own
comments), so `foreign_keys_off` stays `false` and migration 13 remains the only one
that runs with foreign keys disabled. `decision_validations` is untouched.
`atlas_db_decision_verify` needs no change (§What exists, item 1).

### Decision 15 — The page's disposal panel holds the credential in memory, shows the same facts the terminal prompt shows, and requires the typed digest prefix

The Review view's detail pane gains, below the queue buttons, a **Dispose from this
browser** block: a password-type field for the disposal key (memory only by default;
row 3 of the operator's decisions), a `Dispose: approve|reject|resolve` button per
intent the record's revision state and kind allow (the page's mirror of
`atlas_review_intent_allowed` and `atlas_decision_kind_resolvable`, courtesy as
before), which POSTs the challenge and then renders a confirmation block: intent,
repository, decision id, kind, **the revision's own state**, revision number, the full
digest, the expiry, the title and body already on screen labelled as untrusted project
text, the frozen sentences (§Frozen formats), and a text field for the first eight hex
characters. `Confirm` compares client-side, POSTs the disposal, and renders the
daemon's own answer — `state`, `actor`, `key_id`, `actor_means` — as a result line.
On `APPLIED` the matching sheet entry, if queued, is removed through the existing
`reviewQueueRemove` (which keeps its no-`await` invariant; the removal is called after
the fetch resolves, not between a load and a save). The sheet, the copy button and the
first frozen sentence stay exactly as A15 left them: tier 1 is not removed by tier 3.
**No test executes the page's JavaScript**; the suite greps the served bytes and
drives the two routes with a real bearer credential against a tool daemon (Decision 9
of A15, restated once).

### Decision 16 — Audit, log and status say what happened without saying more

Every write-route request appends its `gw_audit` row through the existing `audit()`
(`gateway.c:606`), `interface = WEB_API`, `operation` = the route path, `key_id` = the
verified selector — never the token, never the confirmation, never the body. The daemon
logs one line per remote mint and per remote spend naming the key id, the decision id,
the revision and the outcome, safe-encoded. `atlas gateway status` prints a `dispose:`
line and, beside it, a `clear:` line stating whether `operator_accepts_cleartext_disposal`
is present (human and JSON; amended 2026-09-04 — an auditor asking "does this gateway
carry a disposal credential in the clear?" must get the answer from the command Atlas
offers for that question, on `acbd7ad`'s reasoning for the `anon:` line). `/auth/me`'s
success body gains `"remote_disposal": true|false` — a policy fact, so the page can say
whether the panel can work here before the operator pastes anything — and
`"cleartext_disposal": true|false`, so the page shows the cleartext chain at the
moment of use; its shape changed once today already (`acbd7ad`) and is documented as
changing again.

### Decision 17 — Withdrawn by amendment 2026-09-04: there is no terminator on this machine, and the policy says so in writing

As committed at `c305f40` this decision placed nginx on `192.168.0.198:8799` with the
gateway on loopback. The operator declined TLS on their own network (§The decision,
"Amended 2026-09-04"). After this season, `atlas gateway status` on this machine reads
`listen: 192.168.0.198 port 8799`, `tls: NONE`, `dispose: key_… (…)` and
`clear: ACCEPTED -- operator_accepts_cleartext_disposal = yes …`, and the browser
reaches `http://192.168.0.198:8799/` exactly as it does today. The cost is the
cleartext chain in §Frozen formats, printed by that command on every run.

---

## Frozen formats

Every new vocabulary member, policy key, route row, method name, request and response
shape, refusal sentence, ledger sentence and UI sentence this season introduces. The
executor implements these verbatim; a change here is a plan amendment, dated.

### Vocabulary members

```c
/* include/atlas/decision.h — appended after VERIFICATION_POLICY; UNKNOWN stays absent
 * from this vocabulary because its zero is MODEL_PROPOSAL and always was. */
ATLAS_DECISION_ACTOR_REMOTE_OPERATOR_CONFIRMED          /* name: "REMOTE_OPERATOR_CONFIRMED" */
/* atlas_decision_actor_writable_by_adapter: false. atlas_decision_actor_name/_parse: both. */

/* include/atlas/decision.h — new vocabulary */
typedef enum atlas_decision_channel {
    ATLAS_DECISION_CHANNEL_UNKNOWN = 0,   /* never stored; refused by every op that mints or spends */
    ATLAS_DECISION_CHANNEL_LOCAL,         /* "LOCAL"  — a terminal on the Atlas machine */
    ATLAS_DECISION_CHANNEL_REMOTE         /* "REMOTE" — the gateway, a bearer credential, TLS in front */
} atlas_decision_channel;
const char *atlas_decision_channel_name(atlas_decision_channel c);
bool atlas_decision_channel_parse(const char *name, atlas_decision_channel *out); /* refuses "UNKNOWN" */

/* include/atlas/apikey.h — appended after ATLAS_SCOPE_MEMORY_WRITE, before ATLAS_SCOPE__COUNT.
 * Never stored on a key row: derived by the daemon for exactly the key the root-owned
 * policy names, and only if that key's stored scope list is empty. */
ATLAS_SCOPE_DECISIONS_DISPOSE                           /* name: "decisions:dispose", grantable = false */

/* include/atlas/limits.h */
#define ATLAS_GW_WRITE_BODY_MAX_BYTES 4096u   /* a disposal body is five short fields; take_login_key's bound */
```

```c
/* include/atlas/decision_ops.h — new members of atlas_decision_op */
atlas_decision_channel channel;                          /* set by every producer; UNKNOWN refused */
atlas_buf remote_token;                                  /* REMOTE only: the presented bearer; wiped in _free */
char remote_expected_key_id[ATLAS_APIKEY_SELECTOR_HEX + 1u]; /* REMOTE only: the policy's key id */
uint32_t remote_kinds;                                   /* REMOTE only: ATLAS_DECISION_KIND_BIT(k) mask */

/* include/atlas/decision_ops.h — new members of atlas_decision_result */
atlas_decision_actor actor;                              /* which actor the write point recorded */
char key_id[ATLAS_APIKEY_SELECTOR_HEX + 1u];             /* REMOTE only; empty otherwise */

/* include/atlas/decision.h — new members of atlas_decision_challenge */
atlas_decision_channel channel;
char key_id[ATLAS_APIKEY_SELECTOR_HEX + 1u];             /* REMOTE only; empty otherwise */

/* include/atlas/db.h — atlas_decision_event_row gains `const char *key_id;` (NULL when absent);
 * atlas_db_decision_event_append gains `const char *key_id` after `detail`. */

/* include/atlas/gwpolicy.h — new members of atlas_gwpolicy */
char remote_dispose_key[ATLAS_APIKEY_SELECTOR_HEX + 1u]; /* empty = remote disposal off */
uint32_t remote_dispose_kinds;                           /* ATLAS_DECISION_KIND_BIT(k) mask; 0 with key empty */
/* Amended 2026-09-04. True only when the policy carries
 * `operator_accepts_cleartext_disposal = yes`: the operator's written acceptance
 * that the disposal credential crosses the network unencrypted. Never a default;
 * refused under REVERSE_PROXY; the one thing that lets the group be offered
 * without TLS in front. */
bool cleartext_disposal_accepted;

/* include/atlas/daemon.h — new member of atlas_daemon_opts (test hook, Decision 13) */
const char *gwpolicy_text;

/* include/atlas/gateway.h — a read-only view of API_WRITE_ROUTES[], for the property test */
const atlas_gateway_route_view *atlas_gateway_api_write_routes(size_t *count_out);
```

### The three policy keys (`/etc/atlas/gateway.conf`)

```ini
# Remote disposal from Mission Control. Both keys or neither. Both REQUIRE
# web_gui = yes, and REQUIRE tls_mode = REVERSE_PROXY unless the third key below
# is present; naming either with web_gui = no is a malformed policy and the
# gateway does not start.
# remote_dispose_key   = key_581e0a805cc1febe
# remote_dispose_kinds = OPERATIONAL_FACT PARKED

# THE OPERATOR'S WRITTEN ACCEPTANCE OF A CLEARTEXT DISPOSAL CHANNEL. Not a feature
# toggle. With this line present and tls_mode = NONE (or absent), the disposal credential crosses
# the network unencrypted on every disposal; anyone able to observe traffic on the
# segment can capture it; an Atlas credential has no expiry, so a credential
# captured once disposes of records exactly as you do until `atlas api-key revoke`. `yes` is the
# only accepted value; leave the line out rather than writing `no`. Refused under
# tls_mode = REVERSE_PROXY (nothing to accept) and without the two keys above.
# `atlas gateway status` prints this acceptance on every run.
# operator_accepts_cleartext_disposal = yes
```

**Amended during execution, 2026-09-04.** Two strings in this frozen block were corrected
in the shipped template before the plan caught up, and the corrections are kept rather
than reverted because both are true and the frozen text was not.

`tls_mode = NONE` became `tls_mode = NONE (or absent)`: the loader requires this
acceptance when `tls_mode` is absent too, not only when it is written as `NONE`, and
T4's own new test proves it. The frozen text would have told an operator their policy
was safe in a case where the loader refuses it.

`so a captured one disposes` became `so a credential captured once disposes`: "a captured
one" reads as the *record* in the clause before it, which is the opposite of what the
sentence warns about.

Recorded here rather than fixed silently, because this block's own rule is that a change
to it is a dated amendment.


**Amended 2026-09-04.** The third key, `operator_accepts_cleartext_disposal`, exists
because the operator declined TLS on their own network after being shown the chain
(§The decision). Its name is frozen: it reads as a person's acceptance of a stated
risk, not as a switch. Its grammar: the value is exactly `yes`; any other value, its
presence under `tls_mode = REVERSE_PROXY`, and its presence without both disposal keys
are each MALFORMED.

- `remote_dispose_key`: exactly `key_` followed by 16 lowercase hex characters (the
  form `atlas api-key list` prints). Stored without the `key_` prefix, as
  `api_keys.key_id` is.
- `remote_dispose_kinds`: one or more names `atlas_decision_kind_parse` accepts,
  space-separated, no duplicates. An empty list is refused — disposal of nothing is
  not a smaller grant, it is a key that cannot take effect.
- MALFORMED (gateway DISABLED, reason MALFORMED): a malformed key shape; an unknown or
  duplicated kind; one key without the other; either key with `tls_mode` not
  `REVERSE_PROXY` **unless `operator_accepts_cleartext_disposal = yes` is present**
  (amended 2026-09-04); either key with `web_gui` not `yes`; `decisions:dispose` inside
  `web_gui_anonymous_scopes`; `operator_accepts_cleartext_disposal` with any value but
  `yes`; `operator_accepts_cleartext_disposal` with `tls_mode = REVERSE_PROXY`;
  `operator_accepts_cleartext_disposal` without both disposal keys.
- Loaded by the daemon at start and by the gateway at start. **Editing it means
  restarting both.**

### The disposal credential's minting

```
atlas api-key create --label L --no-scopes
```

Success output adds one line after the existing `ATLAS_API_KEY=` block:

```
scopes: (none) -- this credential authorises nothing on its own. Only a root-owned
        remote_dispose_key line in /etc/atlas/gateway.conf can give it one scope,
        decisions:dispose, and nothing else. Name it there, or revoke it.
```

Refusals (exit 2):

```
--scope and --no-scopes cannot both be given
at least one --scope is required, or --no-scopes for a remote-disposal credential; a credential with no scopes could not read anything
decisions:dispose cannot be granted to a credential; it is derived for the key /etc/atlas/gateway.conf names, and only for one that holds no stored scope
```

### The daemon method group (`src/ipc/server_remote.c`)

```c
static const atlas_method_entry REMOTE_DISPOSAL_METHODS[] = {
    {"decision.remote_challenge", method_remote_challenge},
    {"decision.remote_dispose", method_remote_dispose},
};
const atlas_method_entry *atlas_server_remote_disposal_methods(size_t *count_out);
/* Offered iff all four (amended 2026-09-04 -- the fourth is the state check, and
 * the amendment that added it is beside the frozen predicate above; this
 * declaration still said "three" after the code said four, which is exactly the
 * drift a frozen block exists to prevent):
 * ctx->gwpolicy.state == ATLAS_GWPOLICY_ENABLED, because a refused policy still
 * carries every field the parser wrote before it refused;
 * atlas_server_peer_is_gateway(ctx, peer_uid);
 * ctx->gwpolicy.tls_mode == ATLAS_GWPOLICY_TLS_REVERSE_PROXY **or**
 * ctx->gwpolicy.cleartext_disposal_accepted (amended 2026-09-04 — TLS in front is
 * the intended shape, and the acceptance is the operator's written departure from
 * it; one condition added, none removed); and
 * ctx->gwpolicy.remote_dispose_key[0] != '\0'. Otherwise `unknown method`. */
bool atlas_server_remote_disposal_offered(const atlas_server_ctx *ctx, long long peer_uid);
```

`decision.remote_challenge` params (all forwarded by the gateway from the body, plus
`token` the gateway adds): `repo` (string), `decision` (uid), `revision` (int, > 0
required), `intent` (`approve | reject | resolve`), `token` (the bearer). Response, the
daemon's own document: `repo`, `repo_id`, `decision`, `revision`, `content_hash`,
`state` (the revision's own state), `kind`, `title` (encoded), `token` (32 hex),
`expires_at`, `key_id`. **No `confirm` key.**

`decision.remote_dispose` params: `repo`, `decision`, `intent`, `token` (the bearer,
added by the gateway), `challenge` (the 32-hex challenge token), `confirmation` (what
the operator typed, ≤ `ATLAS_DECISION_CONFIRM_MAX - 1`). Response: the `write_result`
block (`server_decision.c:559`) plus `superseded_revision` when set, `actor`
(`"REMOTE_OPERATOR_CONFIRMED"`, read from `result.actor`), `key_id`, and
`actor_means` (below).

The `spend_method` response at `server_decision.c:2597` changes from the literal
`"LOCAL_OPERATOR_CONFIRMED"` to `atlas_decision_actor_name(result.actor)` — two
spellings of one fact would otherwise drift; the local value it prints is unchanged.

### The gateway's write table (`src/gw/gateway.c`)

```c
/* Every route is a disposal. There is no read in this table, and adding a row means
 * adding its method to WRITE_METHODS[] in tests/test_gateway.c by hand. */
static const api_route API_WRITE_ROUTES[] = {
    {"/api/v1/decision/challenge", "decision.remote_challenge", ATLAS_SCOPE_DECISIONS_DISPOSE,
     {"repo", "decision", "revision", "intent", NULL}, {"revision", NULL}},
    {"/api/v1/decision/dispose", "decision.remote_dispose", ATLAS_SCOPE_DECISIONS_DISPOSE,
     {"repo", "decision", "intent", "challenge", "confirmation", NULL}, {NULL}},
};
```

`api_handle_write` order of operations, none of which may move: path match → method
must be `POST` (405) → `Content-Type` must be exactly
`application/x-www-form-urlencoded` (415) → body length ≤ `ATLAS_GW_WRITE_BODY_MAX_BYTES`
(413) → policy has `remote_dispose_key` (404) → `authenticate()` from the bearer
header only (401) → `atlas_scope_has(pr.scopes, DECISIONS_DISPOSE)` (403) →
`build_api_params` over the body (400) → append `token` = the presented bearer →
`atlas_ipc_call_timeout` → the daemon's document unchanged, status mapped as
`api_handle` maps it → `audit()`.

Requests, as the page sends them:

```
POST /api/v1/decision/challenge HTTP/1.1
Authorization: Bearer atlas_<selector>_<secret>
Content-Type: application/x-www-form-urlencoded

repo=atlas&decision=atlas-dec-28f03b0a44a53db88f0deace6e79721b&revision=1&intent=reject
```

```
POST /api/v1/decision/dispose HTTP/1.1
Authorization: Bearer atlas_<selector>_<secret>
Content-Type: application/x-www-form-urlencoded

repo=atlas&decision=atlas-dec-28f03b0a44a53db88f0deace6e79721b&intent=reject&challenge=<32 hex>&confirmation=6fb2be08
```

`/auth/me` success body gains `"remote_disposal": true|false` (the policy names a
key) and `"cleartext_disposal": true|false` (the policy carries the acceptance key;
amended 2026-09-04), in that order after `"anonymous"`.

### The gateway's refusal sentences

```
405 method_not_allowed    this endpoint takes a POST
415 unsupported_media_type a disposal request is application/x-www-form-urlencoded
413 request_too_large     the request body exceeds the gateway limit           (existing)
404 not_found             this gateway does not serve remote disposal
401 unauthenticated       a disposal needs the disposal credential presented as a bearer token; a session cookie or the anonymous floor cannot dispose
403 forbidden             this credential does not hold the "decisions:dispose" scope   (existing shape)
400 bad_request           a body parameter is malformed
409 conflict              the record moved, the capability does not match, or the policy refuses this record now
```

**Amended during execution, 2026-09-04.** The 409 row is new, and so is a rule about
what maps to it. T3's review established that every spend-side and credential refusal
carries `ATLAS_ERR_INTEGRITY`, which `src/gw/gateway.c:1278` maps to **500**. That is
wrong in the case that matters most and is not exotic: an operator reading a record while
a colleague revises it gets *this decision gained revision N after the challenge was
minted; nothing was changed -- read it again*, and it would arrive in the browser as
**500 Internal Server Error** — a sentence that says Atlas broke, sending them to hunt a
bug that does not exist.

**Ruling.** The gateway maps `ATLAS_ERR_INTEGRITY` to **409 Conflict**, and it does so
globally rather than per route. `ATLAS_ERR_INTEGRITY` in Atlas means *the state you acted
on is not the state that is there* — a challenge already spent, a revision that moved, a
credential that is not the one the row names, a policy that has narrowed. That is what
409 means and it is not what 500 means. A per-route exception was considered and rejected:
one status table is the shape this gateway already has, and a second one is a place for
the two to disagree.

**And one class per sentence.** The same review found two frozen sentences carrying
`ATLAS_ERR_USAGE` at mint and `ATLAS_ERR_INTEGRITY` at spend — through the gateway that is
400 against 409 for byte-identical text, and through the CLI exit 2 against exit 7. Both
sentences are refusals *about state*: a record's kind against the policy, and an intent the
browser does not offer. Both take `ATLAS_ERR_INTEGRITY` at both sites. The rule is that a
frozen sentence names one condition, so it carries one class.

### The write point's refusal sentences (`src/decision/lifecycle.c`, `src/decision/remote.c`)

```
this operation names no channel; a capability is minted and spent through exactly one of LOCAL or REMOTE
that approval challenge was minted through the remote channel and cannot be spent locally
that approval challenge was minted through the local channel and cannot be spent from the browser
that approval challenge was minted for a different credential
the credential presented for this disposal did not authenticate; nothing was changed
that credential is not the one the remote disposal policy names
the remote disposal credential must hold no stored scope, and %s holds %s
a remote challenge is minted only for the newest revision; r%lld was reviewed but r%lld is newest -- read it again

**Amended during execution, 2026-09-04.** This sentence carries `ATLAS_ERR_INTEGRITY`,
not `ATLAS_ERR_USAGE` — so 409 through the gateway and exit 7 at the terminal, matching
its spend-time twin rather than differing from it.

T6's review found the two halves of the amendment's *own named scenario* landing on
different statuses: an operator reading a record while a colleague revises it gets **400**
if the revision moved before the challenge was minted and **409** if it moved after. The
"one frozen sentence, one class" rule is not violated — these are two sentences — but the
rule exists to stop one real-world event answering in two voices, and here it did.

The class is INTEGRITY because the refusal is **about state, not about the request**. A
caller who sends a perfectly well-formed request gets this refusal; nothing about what
they sent is wrong. `400` tells them to fix their request, which is advice they cannot
act on. `409` tells them the record moved, which is what happened and what "read it
again" already says in the sentence itself.
this decision gained revision %lld after the challenge was minted; nothing was changed -- read it again
a remote challenge names the revision it is for; 0 is not a revision
a record of kind %s is not one the remote disposal policy names; dispose of it on a terminal
supersede and revalidate are not offered from the browser; use a terminal on the Atlas machine
```

The `%s` in the credential sentence is a verified 16-hex key id and the stored scope
list, both Atlas-checked shapes. Every sentence `spend_challenge` produces today is
unchanged, including `the confirmation does not match this revision; nothing was
changed`.

### The ledger sentences (event `detail`)

```
confirmed through the Atlas remote operator channel with credential %s; this records that the channel and the credential were used, not which person used them
replaced by a later revision of the same decision, which was approved in the same transaction        (existing; now only when prev_rev_no < c.revision_no)
replaced by an earlier revision of the same decision, approved after it in the same transaction      (new; when prev_rev_no > c.revision_no)
```

The existing local sentence — `confirmed through the Atlas local operator channel; this
records that the channel was used, not which person used it` — is unchanged.

`actor_means` for a remote disposal:

```
an explicit action arrived through Atlas' remote operator channel: the credential named in key_id was presented over the gateway's listener, under whatever transport security that listener has, which Atlas does not verify. This does not identify a person, does not prove a person was present, and is not a signature. It is weaker than the local channel by construction: the credential passed through a network-facing process.
```

### `atlas gateway status`

Human, after the `anon:` line, two lines, always both when the gateway is ENABLED:

```
dispose: key_581e0a805cc1febe (OPERATIONAL_FACT PARKED)
clear:   ACCEPTED -- operator_accepts_cleartext_disposal = yes: the disposal credential crosses this network unencrypted, and a captured credential disposes until it is revoked
```
or
```
dispose: (none -- the browser can read and queue, never dispose)
clear:   (not accepted -- a disposal credential is offered only behind tls_mode = REVERSE_PROXY)
```

JSON: `"remote_dispose_key"` (the id or `""`), `"remote_dispose_kinds"` (the rendered
list or `""`) and `"cleartext_disposal_accepted"` (`true` or `false`). The `clear:`
line and the JSON key are amended in on 2026-09-04: a ceiling may be omitted from this
command; a credential crossing the network in the clear may not.

### `decision.history` and the CLI

`timeline[]` rows gain `"key_id"` (present only when the event has one). `atlas
decision history` prints it as `credential: key_…` on the event's line in the human
form and as `key_id` in the JSON form; both renderers.

### The Review view's fixed sentences

Under the **Dispose from this browser** heading, verbatim:

> Disposing from this browser records REMOTE_OPERATOR_CONFIRMED with the credential's
> id. That names the channel and the credential, not a person. This channel is weaker
> than a terminal on the Atlas machine: your disposal key passes through the gateway
> process and across the network, and Atlas verifies neither the process nor the
> transport.

When `/auth/me` reports `cleartext_disposal: true`, directly beneath the sentence
above, verbatim (amended 2026-09-04):

> On this deployment the disposal key crosses the network in the clear. Anyone able
> to observe this network segment can capture it, and a captured key disposes of
> records until it is revoked.

Beside the key field, verbatim:

> The disposal key is held in this tab's memory only and is never stored; reloading
> the page asks for it again.

**Conditional on row 3 of §Decisions the operator must be asked.** If the operator
chose `sessionStorage`, the sentence above is replaced — not joined — by this one,
verbatim, and T8's needle changes with it:

> The disposal key is remembered for this tab only and forgotten when the tab closes;
> it is never written to this browser's persistent storage.

Above the confirmation field, verbatim:

> Type the first 8 characters of the digest shown above to confirm. A mismatch spends
> nothing.

When `/auth/me` reports `remote_disposal: false`, in place of the panel, verbatim:

> This gateway does not serve remote disposal. Records queued here are disposed of
> with atlas review apply FILE on a terminal on the Atlas machine.

The client-side mismatch line reuses the CLI's sentence: `that is not the confirmation
for this revision; nothing was changed`.

### The cleartext chain (amended 2026-09-04)

Verbatim in `SECURITY.md`, `docs/remote-access.md` and `docs/browser-disposal.md`, in
the paragraph that announces the capability, and in short form in `CLAUDE.md`'s
season paragraph. A chain, not a conclusion; no sentence implies Atlas recommends it.

> **On this deployment the disposal credential travels in the clear.** The gateway
> listens on `192.168.0.198:8799` with `tls_mode = NONE`, and the two disposal routes
> carry the credential as a bearer header on every request, so anyone able to observe
> traffic on that network segment can read it. An Atlas API credential has no expiry,
> so a credential captured once disposes of records exactly as the operator does until
> the operator notices and runs `atlas api-key revoke`. The operator was shown this
> chain on 2026-09-04 and accepted it for this network by writing
> `operator_accepts_cleartext_disposal = yes` into the root-owned gateway policy;
> `atlas gateway status` prints that acceptance on every run. Atlas states this cost
> and does not judge the trade; the same key on a listener reachable from a network the
> operator does not control is a different decision using the same mechanism.

### Migration 31

```c
{31, "the remote operator channel: which channel and credential minted a challenge, and which credential the ledger records", M31_STATEMENTS, false},
```

`M31_STATEMENTS` = `{M31_VERIFY, M31_EVENTS, M31_CHALLENGES, M31_CONFIRM, NULL}`:
`M31_VERIFY` captures both counts into temp tables; `M31_EVENTS` rebuilds
`decision_events` as `M15_EVENTS` does with the actor CHECK widened and `key_id TEXT`
appended, copying `id` explicitly and recreating the three indexes; `M31_CHALLENGES`
rebuilds `decision_challenges` as `M13_CHALLENGES` does with `channel TEXT NOT NULL
DEFAULT 'LOCAL' CHECK(channel IN ('LOCAL','REMOTE'))` and `key_id TEXT` appended,
recreating `idx_decision_challenges_repo`; `M31_CONFIRM` checks both counts and
`pragma_foreign_key_check` under two named CHECKs,
`no_decision_event_may_be_lost_in_migration_31` and
`no_decision_challenge_may_be_lost_in_migration_31`.

---

## Authority argument — the season's non-negotiables

These go into `docs/engineering-rules.md` in full and into `CLAUDE.md` as one line each
(T9). The six the brief restates and the three A15 found are first, each with its
reason and what this plan changed about it; the rest are this season's own.

- **Its own channel identity, never `LOCAL_OPERATOR_CONFIRMED` reused.**
  `REMOTE_OPERATOR_CONFIRMED` is a sixth actor; migration 31 widens the ledger's
  CHECK on migration 15's pattern; `decision_challenges` gains a `channel` so a
  remotely minted capability cannot be spent locally or the reverse. Reusing the local
  name would make every ledger row ever written retrospectively ambiguous, which is
  the one cost that cannot be paid back. **Changed from A15's costing:** the ledger
  row also carries the credential (`key_id`), because the challenge table is prunable
  and a fact the ledger needs cannot live only on a row that will be pruned.
- **Its own scope, ungrantable to any model credential.** `decisions:dispose` is in
  `SCOPES[]` with `grantable = false`, never stored on a key row, derived by the daemon
  for exactly the key a root-owned line names. **Changed from A15's costing:** derived
  only for a key with *no* stored scope, so the credential that disposes is structurally
  not a credential that reads, and `atlas api-key create --no-scopes` is the deliberate
  way to mint one.
- **Absent from the MCP surface.** No tool maps to the scope; a credential holding it
  lists zero tools; `tests/test_gw_remote.c` asks `tools/list` and `tools/call` for every
  plausible spelling, in the shape it already uses for credential administration.
- **TLS in front is the intended shape, and a departure from it is a written
  acceptance, never an absence.** The policy loader refuses a disposal key under
  `tls_mode = NONE`; the daemon offers the group only under `REVERSE_PROXY`; the
  gateway refuses the routes with a sentence first. *(Amended 2026-09-04: the plan as
  committed said "there is no cleartext opt-out key" and put the terminator on this
  machine in T10. The operator, shown the four reasons in §The decision, declined TLS
  on their own network. The requirement is now `operator_accepts_cleartext_disposal =
  yes` — root-owned, absent by default, refused under `REVERSE_PROXY`, printed by
  `atlas gateway status` and reported by `/auth/me` — and the gate's `REVERSE_PROXY`
  condition stays in the code with that key as its one exception. Every document
  states the cleartext chain verbatim.)* Atlas must never be described as providing
  TLS.
- **Replay protection bound to the content hash.** The existing challenge shape, plus
  the channel, the credential and — for the remote channel — the newest-revision check
  at mint and at spend. The operator types the first eight hex of the digest on screen;
  the challenge response never carries the confirmation phrase.
- **A root-owned policy naming which kinds may be disposed of remotely, refused rather
  than clamped.** `remote_dispose_kinds`, both keys or neither, MALFORMED under any
  condition that would leave the key unable to take effect — P0's rule, which the
  anonymous-scopes key already follows.
- **The daemon authenticates the credential itself, inside the transaction that mints
  or spends.** `src/decision/remote.c`, called from the write point for `channel ==
  REMOTE`, on the writer's handle, against the key table as it is at that moment. A
  session cookie can never dispose, because the gateway holds no token for one.
- **A second route table, not a method column.** `api_handle`'s GET-only refusal is
  right for the table it guards; the write table has its own handler, its own positive
  allowlist in the test, and exactly one scope.
- **No JSON is parsed in `src/gw`.** The body is `application/x-www-form-urlencoded`,
  which is the query-string syntax `build_api_params` already parses; a token never
  appears in a URL.
- **UNKNOWN is zero on the channel vocabulary and is refused, not defaulted.** A
  forgotten field must fail, because the alternative records a remote act under the
  local name.
- **The actor comes from the stored challenge, never from the request.** A boolean in a
  request is a boolean a request can assert; the capability is the evidence, and the
  capability now says which channel minted it.
- **A remote challenge is minted only for the newest revision, and spends only if it
  still is.** The local `--revision N` semantics A15 measured are untouched.
- **Nothing may claim a channel establishes that a natural person acted.**
  `REMOTE_OPERATOR_CONFIRMED` names a channel and a credential. The page, the two new
  source files, the season's document and the lifecycle document join the tripwire.
- **It is weaker than the local channel by construction**, and every document that
  announces it says so in the same paragraph. The required-wording test pins the
  phrase in the page and in `docs/browser-disposal.md`.
- **`atlas_decision_apply_in_tx` keeps exactly three callers and
  `atlas_service_decision_confirm` exactly two.** The remote methods build ordinary ops
  and submit them through the writer; the verification is a call *inside* the write
  point, not a caller of it.
- **The test channel is not a public surface.** `atlas_daemon_opts.gwpolicy_text` and
  `tests/tools/atlas_gw_daemon.c`, never a flag, an environment variable or a policy
  path override — P0's rule, for P0's reason.
- **The anonymous floor can never hold the disposal scope**, by the parser's existing
  refusal of any non-grantable scope and by the write path never consulting it; both
  are tested rather than asserted.
- **No new thread, process, timer or background loop; no MCP tool; no new read route;
  no authority verb in a tool name; one migration.** The two daemon methods carry
  `remote_challenge` and `remote_dispose`, which name the act the season is for and
  are not in the scanner's verb list; they are offered only to the gateway's uid and
  only under the policy.

## Worst-case cost, stated so nobody discovers it in a bill

Per remote disposal: two `POST`s, two `gateway.auth` round trips, two HMAC verifications
at the write point (one per op), one challenge row, one or two ledger rows (two when an
approval supersedes an effective revision), two `gw_audit` rows, three writer jobs at
most (the challenge, the spend, and the audit rows are fire-and-forget). Zero processes,
zero model calls, zero money.

Per credential holder: whoever holds the disposal credential can mint challenges at the
gateway's rate limit (600 per minute by default, `ATLAS_GW_DEFAULT_RATE_PER_MINUTE`),
pruned to `ATLAS_DECISION_CHALLENGES_RETAIN` (200) unconsumed rows, and **can dispose of
any record whose kind the policy names and whose newest revision is in the state the
intent needs — exactly as the operator can.** That is the whole risk of this season and
it is bounded by one thing: `atlas api-key revoke`, which takes effect on the next
request (`tests/test_gw_remote.c`, "a revoked credential stops working immediately").

Migration 31 rebuilds two tables: every ledger row ever written (`decision_events`) and
at most a few hundred challenge rows. Bounded by the ledger's size; on this machine the
ledger holds a handful of documents' events. Run under the writer lock, like every
migration.

On the wire (amended 2026-09-04): with `operator_accepts_cleartext_disposal = yes`
and `tls_mode = NONE`, every disposal sends the credential as a bearer header in the
clear across `192.168.0.198`'s network segment — twice per disposal. A passive
observer on that segment holds the credential after the first disposal they see, and
holds it until `atlas api-key revoke`; there is no TTL to wait out. That is the cost
the operator accepted, and it is the whole of it.

---

# Decisions the operator must be asked, and when — read this before dispatching anything

A15's plan left a genuine choice in its §The decision and nobody put it to the operator;
fourteen hours followed. Every choice this plan leaves is here, with the default the
plan assumes and **the task before which it must be asked**. None may be asked "at the
end".

**Amended 2026-09-04.** Row 1 was asked and answered no (§The decision); its row is
kept as the record of that answer. Rows 4 and 5 existed only because of the reverse
proxy row 1 would have installed, and are struck with it. Rows 2 and 3 remain, and
**both are now asked before T1** — the coordinator's instruction, and cheaper than
carrying two open questions into a season that no longer has a gating one.

| # | Question, in full | Default this plan assumes | Ask before |
| --- | --- | --- | --- |
| 1 | ~~Will you run a TLS terminator on this machine and trust its certificate on every device you will dispose from?~~ **Asked and answered 2026-09-04: no** — *"https ye gerek yok. kendi aginda sacmalamasin zaten"*. The season proceeds without TLS under `operator_accepts_cleartext_disposal = yes`; the chain the operator was shown and accepted is in §The decision and §Frozen formats. | — | answered |
| 2 | **Which kinds may be disposed of from the browser?** The starting line is `OPERATIONAL_FACT PARKED`. Widening to `POLICY`, `INVARIANT` or `ACCEPTED_RISK` means a captured credential can accept a risk or set an invariant; widening to `DECISION` means every A4-era record — and on this deployment "captured" means observed on the network segment (§Frozen formats, the cleartext chain). Any set is one root-owned line and needs no code change. | `OPERATIONAL_FACT PARKED` | **T1** |
| 3 | **May the page remember the disposal key for the tab's session?** Memory only means re-pasting it after every reload — the shape of annoyance that produced today's anonymous floor. `sessionStorage` survives a reload, dies with the tab, and is readable by any script the page runs — under this CSP that is the page's own inline script and nothing else. Either way it is never `localStorage`. | memory only | **T1** |
| ~~4~~ | ~~Behind the proxy, should the gateway believe `X-Forwarded-For`?~~ Struck 2026-09-04: there is no proxy. | — | — |
| ~~5~~ | ~~Should the anonymous read floor be installed behind the proxy?~~ Struck 2026-09-04: there is no proxy; the floor's `Host` check is unaffected by this season. | — | — |

---

# File structure

**Create:**

| Path | Responsibility |
| --- | --- |
| `src/decision/remote.c`, `include/atlas/decision_remote.h` | `atlas_decision_remote_verify`: the in-transaction credential check for a `REMOTE` op — token parse, key lookup on the writer's handle, ACTIVE, readable scopes, empty stored scopes, constant-time verify, id equals the op's expected id; no I/O beyond the database, no process |
| `src/ipc/server_remote.c` | `REMOTE_DISPOSAL_METHODS[]`, `atlas_server_remote_disposal_methods`, `atlas_server_remote_disposal_offered`, the two methods |
| `tests/tools/atlas_gw_daemon.c` | `atlas-gw-daemon DATA_DIR POLICY_FILE`: the real daemon with an injected gateway policy (Decision 13) |
| `tests/test_migrate31.c` | unit: the rebuild, the widened CHECK, the two new columns, the true default, row preservation, the vocabulary CHECKs |
| `tests/test_decision_remote.c` | integration: the write point under a `REMOTE` op, without a daemon |
| `tests/test_gw_dispose.c` | daemon: the two routes end to end through `atlas_gateway_serve_bytes` against `atlas-gw-daemon` |
| `docs/browser-disposal.md` | the season's document |

**Modify:** `include/atlas/decision.h`, `include/atlas/decision_ops.h`,
`include/atlas/apikey.h`, `src/gw/apikey.c`, `src/decision/decision.c`,
`src/decision/lifecycle.c`, `src/core/service_decision.c`, `src/core/service_apikey.c`,
`src/cli/cli.c`, `src/cli/render_human.c`, `src/cli/render_json.c`, `src/db/migrate.c`,
`src/db/db_decision.c`, `include/atlas/db.h`, `include/atlas/gwpolicy.h`,
`src/gw/gwpolicy.c`, `src/gw/gateway.c`, `include/atlas/gateway.h`,
`src/ipc/server_gw.c`, `src/ipc/server_decision.c`, `src/ipc/server.c`,
`src/ipc/server_internal.h`, `src/daemon/daemon.c`, `src/daemon/writer.c`,
`include/atlas/daemon.h`,
`include/atlas/limits.h`, `src/gw/ui/mission-control.html`, `CMakeLists.txt`,
`tests/CMakeLists.txt`, `tests/test_apikey.c`, `tests/test_gateway.c`,
`tests/test_gw_remote.c`, `tests/test_decision_operator.c`, `tests/test_a7_authority.c`,
`tests/test_decision_mcp.c`, `deploy/a9/gateway.conf.template`, `docs/roadmap.md`,
`docs/remote-access.md`, `docs/decision-lifecycle.md`, `docs/review-surface.md`,
`docs/engineering-rules.md`, `docs/extending.md`, `docs/backlog.md`, `SECURITY.md`,
`README.md`, `CLAUDE.md`, `/opt/atlas/deploy.local.sh` (local, gitignored), and last
`include/atlas/atlas.h` (`ATLAS_PHASE`).

**Deliberately not modified:** `src/mcp/mcp_tools.c` (no tool), `src/hook/hook.c`,
`integrations/claude/atlas/skills/atlas-memory/SKILL.md` (its 8192-byte bound has 21
bytes of margin, `docs/backlog.md:2332`; the two commands it already names cover the
terminal, and the disposal credential is never on disk for a model to find — the
skill needs no new sentence, and the plan says so rather than spending the margin),
`tests/support/fixture.c` (the tool daemon is forked by its own test on
`fx_daemon_start`'s shape), and the read route table.

---

# Tasks

Dependency order: T1 → T2 → T3 → T4 → T5 → T6 → T7 → T8 → T9 → T10. T4 depends only
on T1 and may run in parallel with T2 and T3; everything else is ordered. **T1 is not
dispatched until rows 2 and 3 of §Decisions the operator must be asked are answered.**
*(Amended 2026-09-04: the order was eleven tasks ending T10 → T11, and T1 was gated on
row 1; row 1 was answered no, the nginx task that was T10 is removed — see the note in
its place below — and the live acceptance that was T11 is now T10.)*

---

### Task T1: the vocabularies — actor, channel, scope — and the credential that holds nothing

**Files:**
- Modify: `include/atlas/decision.h`, `src/decision/decision.c`,
  `include/atlas/apikey.h`, `src/gw/apikey.c`, `src/core/service_apikey.c`,
  `src/cli/cli.c`, `src/cli/render_human.c`, `src/cli/render_json.c`,
  `tests/test_apikey.c`, `tests/test_decision_model.c`

**Interfaces produced:** §Frozen formats, "Vocabulary members" — the actor member, the
channel enum with its name/parse pair, the scope member, and `--no-scopes` with its
three sentences.

- [ ] **Step 1: Write the failing tests.** In `tests/test_decision_model.c` (where the
      actor table is enumerated today): `atlas_decision_actor_name` round-trips
      `REMOTE_OPERATOR_CONFIRMED`; `atlas_decision_actor_writable_by_adapter` is false
      for it (and still false for `LOCAL_OPERATOR_CONFIRMED`, `ATLAS_AUTOMATIC`,
      `VERIFICATION_POLICY`); the channel vocabulary's zero names `"UNKNOWN"` and
      refuses to parse, `LOCAL` and `REMOTE` round-trip. In `tests/test_apikey.c` beside
      `test_creation_refuses_what_it_cannot_grant` (`:283`): `decisions:dispose` parses,
      renders after `memory:write`, `atlas_apikey_scope_grantable` is false;
      `api-key create --scope decisions:dispose` is refused with the frozen sentence;
      `api-key create --label x` with neither `--scope` nor `--no-scopes` is refused with
      the amended sentence; `--scope repo:read --no-scopes` is refused with the frozen
      sentence; `--no-scopes` succeeds, stores an empty scope list, and prints the frozen
      `scopes: (none)` block; `api-key list` shows the key with no scopes; the two
      renderers agree (`test_the_two_renderers_agree`, `:372`, extended).
- [ ] **Step 2: Run and watch them fail** — `ctest -R "test_apikey|test_decision_model"`.
- [ ] **Step 3: Implement.** Append the actor; rewrite the comment at
      `decision.h:246-247` ("four actors … not casually a fifth") to say there are six
      and why each of the last three was argued for. Add the channel enum after the
      actor block with its name/parse pair in `decision.c` (no `default:` anywhere).
      Append the scope; `SCOPES[]` row `{ATLAS_SCOPE_DECISIONS_DISPOSE,
      "decisions:dispose", false}` with a comment naming Decision 2. `--no-scopes` in
      `cli.c`'s api-key parsing and `atlas_apikey_create_opts` (a `bool no_scopes`);
      `atlas_apikey_create_on` accepts `scopes == 0` only when it is set. **`api-key
      rotate KEY-ID --label L --no-scopes` accepts the same flag under the same three
      sentences** (`cli.c:183` is the rotate form), because rotation mints a new key id
      and the operator must then edit `remote_dispose_key` — the success output of a
      `--no-scopes` rotation says so in one added line: `the policy line
      remote_dispose_key must now name key_<new id>; until it does, neither key can
      dispose`. Extend `test_rotation_replaces_one_credential_with_another`
      (`tests/test_apikey.c:243`) with the `--no-scopes` case.
- [ ] **Step 4: Run the tests and watch them pass**; `make` with zero warnings; run
      `./build/atlas api-key create --label probe --no-scopes --data-dir /tmp/x` once
      against a scratch directory and read the output.
- [ ] **Step 5: Commit** — `feat(a16): a sixth actor, a channel vocabulary whose zero is refused, and a scope nothing can be granted`

---

### Task T2: migration 31, and the database layer that reads and writes what it adds

**Files:**
- Create: `tests/test_migrate31.c`
- Modify: `src/db/migrate.c`, `src/db/db_decision.c`, `include/atlas/db.h`,
  `include/atlas/decision.h` (the challenge struct's two members), `tests/CMakeLists.txt`

**Interfaces produced:** §Frozen formats, "Migration 31"; `atlas_decision_challenge`
gains `channel` and `key_id`; `atlas_db_decision_challenge_insert` writes both and
`_find` reads both; `atlas_db_decision_event_append` gains `const char *key_id` after
`detail`; `atlas_decision_event_row` gains `key_id`; the history reader fills it.

- [ ] **Step 1: Write the failing test** in `tests/test_migrate31.c`, `unit` label, on
      `tests/test_migrate29.c`'s shape: a fresh database reaches 31; a database stopped
      at 30 with three events (one per actor value the old CHECK admitted, including
      `VERIFICATION_POLICY`) and two challenges reaches 31 with every column of every
      pre-existing row byte-identical, every challenge reading `channel = 'LOCAL'`
      and `key_id IS NULL`, every event reading `key_id IS NULL`; the events CHECK
      accepts `REMOTE_OPERATOR_CONFIRMED` and refuses `REMOTE_OPERATOR` and `UNKNOWN`;
      the challenges CHECK accepts `LOCAL` and `REMOTE` and refuses `UNKNOWN` and `''`;
      `pragma_foreign_key_check` is empty; the three event indexes and the challenge
      index exist by name; `atlas_db_decision_verify` on the migrated document agrees
      (the replay reads events only — assert it by inserting a `REMOTE_OPERATOR_CONFIRMED`
      APPROVED event by hand and watching the replay still agree).
- [ ] **Step 2: Run it and watch it fail** — schema 30 reached, no migration 31.
- [ ] **Step 3: Write `M31_*`** exactly as §Frozen formats describes, with a migration
      comment stating: why both tables are rebuilt (the CHECKs); why `foreign_keys_off`
      stays false (nothing references either table — cite migration 13's and 15's own
      comments); **why `DEFAULT 'LOCAL'` is a true statement about every existing row
      and not migration 19's mistake**; and that the ledger replay needs no change and
      why. Append the `MIGRATIONS[]` row. Then the db layer: insert/find/event-append
      and the history row.
- [ ] **Step 4: Run `test_migrate31`, `test_decision_model`, `test_decision_operator`,
      `test_migrate29`, `test_migrate10` and the full unit label**; `make` with zero warnings. Every
      existing caller of `atlas_db_decision_event_append` passes NULL for now.
- [ ] **Step 5: Commit** — `feat(a16): migration 31 -- the challenge says which channel and credential minted it, and the ledger keeps the credential`

---

### Task T3: the write point — channel, credential, newest revision, kinds, and the actor from the stored challenge

**Files:**
- Create: `src/decision/remote.c`, `include/atlas/decision_remote.h`,
  `tests/test_decision_remote.c`
- Modify: `include/atlas/decision_ops.h`, `src/decision/lifecycle.c`,
  `src/core/service_decision.c` (`op_new` sets `LOCAL`; `op_to_params` refuses a
  `REMOTE` op), `src/ipc/server_decision.c` (`op_new` sets `LOCAL`; `spend_method`'s
  actor from `result.actor`), `src/daemon/writer.c` (`atlas_writer_decision`'s result
  block copies `actor` and `key_id` field by field — `docs/extending.md` "Extending A4
  safely" says every new writer payload crosses that block, and a result member that
  does not is read as zero by every RPC method), `tests/test_decision_operator.c`,
  `CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces produced:**

```c
/* include/atlas/decision_remote.h */
/* Verifies the credential a REMOTE op carries, on `db` (the writer's handle, inside
 * the caller's transaction), and writes the verified key id to `key_id_out`. Refuses,
 * with the §Frozen sentences, when: the token does not parse; the selector is unknown;
 * the secret does not verify (constant-time); the key is not ACTIVE; its scopes are
 * unreadable; its stored scope list is not empty; its id is not `expected_key_id`.
 * Every failure but the last two produces the same outward sentence. Reads the
 * database and nothing else: no process, no file, no clock beyond `now`. */
atlas_status atlas_decision_remote_verify(atlas_db *db, const atlas_buf *token,
                                          const char *expected_key_id,
                                          char key_id_out[ATLAS_APIKEY_SELECTOR_HEX + 1u],
                                          atlas_err *err);
```

The op members and result members are in §Frozen formats. Inside the write point, in
this order and nowhere else: after `resolve_repo` and `bind_session`, for any op with
`atlas_decision_op_needs_challenge(op->kind)` **or** `op->kind == CHALLENGE`, refuse
`channel == UNKNOWN`; for `channel == REMOTE`, call `atlas_decision_remote_verify` and
keep the key id in `apply_ctx`. `op_challenge`: for `REMOTE`, refuse `expect_revision_no
<= 0`, refuse `expect_revision_no != latest`, refuse a kind outside `op->remote_kinds`,
refuse `SUPERSEDE`/`REVALIDATE` intent; store `channel` and `key_id` on the row.
`spend_challenge`: refuse a channel mismatch either way; for `REMOTE`, refuse a
`key_id` mismatch, refuse when the newest revision is no longer `c.revision_no`,
refuse a kind outside `op->remote_kinds` (the policy on the op is the same policy that
minted; checked twice for the reason the repository is). `op_approve`, `op_reject`,
`op_resolve`: actor from `c.channel`; detail sentence per channel; `key_id` passed to
`transition` for `REMOTE`; `result.actor` and `result.key_id` set. `op_supersede`,
`op_revalidate`: refuse a `REMOTE` challenge with the frozen sentence. `op_approve`:
the supersession detail chosen by comparing `prev_rev_no` against `c.revision_no`
(Decision 12).

- [ ] **Step 1: Write the failing tests** in `tests/test_decision_remote.c`,
      `integration` label, driving `atlas_decision_apply` directly at the write point
      as `approve_through_the_write_point` does, against a fixture with one repository,
      one `OPERATIONAL_FACT` at r1 and one `POLICY` at r1, and two real keys minted by
      the CLI — `dispose` with `--no-scopes` and `reader` with `decisions:read`:
      (a) a `CHALLENGE` op with `channel == UNKNOWN` is refused with the frozen
      sentence and inserts no row; the same for `APPROVE`.
      (b) a `REMOTE` challenge with `dispose`'s token, expected id = its id, kinds =
      `{OPERATIONAL_FACT}`, `expect_revision_no = 1`, intent approve: succeeds; the row
      reads `channel = 'REMOTE'`, `key_id = <dispose>`; a `LOCAL` spend of that token is
      refused with the frozen sentence and the row stays unconsumed; a `REMOTE` spend
      with the typed prefix succeeds; `decision_events` holds an `APPROVED` row with
      `actor = 'REMOTE_OPERATOR_CONFIRMED'`, `key_id = <dispose>`, the frozen detail;
      `atlas_db_decision_verify` agrees; `result.actor` is the new actor.
      (c) a `LOCAL` challenge spent by a `REMOTE` op is refused with the reverse
      sentence.
      (d) `reader`'s token as the credential, expected id = `reader`'s id: refused with
      the "must hold no stored scope" sentence, naming `decisions:read`; `dispose`'s
      token with expected id = `reader`'s id: refused with the "not the one the policy
      names" sentence; a wrong secret, an unknown selector, and `dispose` revoked
      between mint and spend: each refused with the "did not authenticate" sentence,
      and the challenge stays unconsumed.
      (e) `expect_revision_no = 0`: refused; revise the record to r2, then `REMOTE`
      challenge for r1: refused naming r1 and r2; mint for r2, revise to r3, spend:
      refused with the "gained revision 3" sentence, unconsumed.
      (f) the `POLICY` record under kinds `{OPERATIONAL_FACT}`: refused at mint with
      the kinds sentence; under kinds `{OPERATIONAL_FACT, POLICY}`: minted.
      (g) `REMOTE` challenge with `SUPERSEDE` intent and with `REVALIDATE` intent: each
      refused with the frozen sentence.
      (h) `REMOTE` challenge for the `OPERATIONAL_FACT`, then `REJECT` via the same
      channel: `REJECTED` row with the new actor and key id; and, on an approved
      `OBLIGATION` added to the fixture, `RESOLVE`: `RESOLVED` row likewise.
      (i) the local path is unchanged: a `LOCAL` challenge and spend through the same
      helper records `LOCAL_OPERATOR_CONFIRMED` with `key_id IS NULL` and the existing
      detail sentence, byte for byte.
      (j) `op_to_params` on a `REMOTE` op returns an error naming the rule (a remote op
      never travels over the socket), and `atlas_decision_op_free` leaves the token
      buffer's bytes zero (read them back before the free through a copy of the
      pointer, as `test_apikey.c`'s wipe test does if one exists; otherwise assert the
      function memsets by inspection and say so in the test's comment).
      In `tests/test_decision_operator.c`, extend
      `test_a_pinned_revision_that_is_not_the_newest`: the SUPERSEDED event's detail
      now reads the "earlier revision, approved after it" sentence, and a fresh case
      approving r2 over an approved r1 reads the "later revision" sentence.
- [ ] **Step 2: Run and watch them fail** for the right reasons (link failure first).
- [ ] **Step 3: Implement** per the contract. `src/decision/remote.c` includes
      `atlas/apikey.h` and `atlas/gw.h` and says at its head why the decision layer now
      depends on them (Decision 3's last paragraph). Neither new file may contain the
      two forbidden substrings from §Global constraints.
- [ ] **Step 4: Run `test_decision_remote`, `test_decision_operator`,
      `test_decision_mcp` (the three-caller scan) and `test_review_apply` (the
      two-caller scan)**; `make` with zero warnings.
- [ ] **Step 5: Commit** — `feat(a16): the write point knows which channel minted a capability, verifies the credential in the transaction, and records the actor from the row`

---

### Task T4: the three policy keys, the template, the status lines

**Files:**
- Modify: `include/atlas/gwpolicy.h`, `src/gw/gwpolicy.c`, `src/gw/gateway.c` (status,
  human and JSON), `deploy/a9/gateway.conf.template`, `tests/test_gateway.c`

**Interfaces produced:** the two `atlas_gwpolicy` members (§Frozen formats); the two
keys with their grammar and every MALFORMED condition; the `dispose:` line.

- [ ] **Step 1: Write the failing tests** in `tests/test_gateway.c` beside
      `test_web_gui_anonymous_scopes_parses_exactly_what_was_named` (`:139`) and inside
      `test_every_malformed_policy_disables_the_gateway` (`:69`)'s matrix: a complete
      policy with `tls_mode = REVERSE_PROXY`, `web_gui = yes`, both keys → ENABLED,
      `remote_dispose_key` holds the 16 hex without `key_`, `remote_dispose_kinds` holds
      exactly the two bits; each MALFORMED case from §Frozen formats, one policy text
      each: bad key shape (15 hex; uppercase; no `key_`), unknown kind, duplicate kind,
      empty kinds, key without kinds, kinds without key, both keys with `tls_mode =
      NONE` and no acceptance, both keys with `web_gui = no`, and
      `web_gui_anonymous_scopes = decisions:dispose`; a policy with neither key →
      ENABLED with the field empty and the mask zero. **The acceptance key (amended
      2026-09-04):** both keys with `tls_mode = NONE` and
      `operator_accepts_cleartext_disposal = yes` → ENABLED with
      `cleartext_disposal_accepted == true`; the same with `tls_mode` absent (loopback
      bind) → ENABLED likewise; `operator_accepts_cleartext_disposal = no`, `= true`,
      `= 1` → MALFORMED each; the acceptance with `tls_mode = REVERSE_PROXY` →
      MALFORMED; the acceptance with neither disposal key → MALFORMED; a policy
      without the acceptance → `cleartext_disposal_accepted == false`. `atlas gateway
      status --json` and human against a parsed buffer (the status renderer takes a
      policy) print the `dispose:` and `clear:` lines in both forms, in both the
      accepted and the not-accepted wording. **No test in the tree asserts the human
      status output today** — `tests/test_gateway.c` covers the parser matrix
      (`:104-115`) and nothing greps the `anon:` line — so these assertions are the
      first of their kind; assert by needle (`clear:   ACCEPTED`,
      `operator_accepts_cleartext_disposal = yes`, `(not accepted`), never by whole
      line or line count, so the long `clear:` line is a sentence an auditor reads
      and not a width a test pins.
- [ ] **Step 2: Run and watch them fail.**
- [ ] **Step 3: Implement** the two branches in `atlas_gwpolicy_parse_buffer` and the
      end-of-parse cross-checks beside the existing wider-bind check
      (`gwpolicy.c:459-467`); the template's commented block; the status line after
      `anon:` with a comment citing `acbd7ad`'s reason.
- [ ] **Step 4: Run `test_gateway`**; `make`; run `./build/atlas gateway status` on
      this machine and confirm it prints `dispose: (none -- …)` — the binary must
      still load today's policy as ENABLED, because nothing in it names a key.
- [ ] **Step 5: Commit** — `feat(a16): two root-owned keys name the disposal credential and the kinds it may dispose of, refused without TLS in front`

---

### Task T5: the daemon — derivation in `gateway.auth`, the remote group, the test channel

**Files:**
- Create: `src/ipc/server_remote.c`, `tests/tools/atlas_gw_daemon.c`,
  `tests/test_gw_dispose.c` (this task writes its daemon-level half; T6 adds the HTTP half)
- Modify: `src/ipc/server_gw.c`, `src/ipc/server.c`, `src/ipc/server_internal.h`,
  `src/daemon/daemon.c`, `include/atlas/daemon.h`, `tests/test_a7_authority.c`,
  `CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces produced:** §Frozen formats, "The daemon method group";
`atlas_daemon_opts.gwpolicy_text`; the tool binary `atlas-gw-daemon DATA_DIR
POLICY_FILE` (reads the file — a fixture-written, user-owned file, deliberately not
through `atlas_gwpolicy_load_at`'s root-ownership walk — and hands its text to the
daemon, which parses it with `atlas_gwpolicy_parse_buffer`).

`method_remote_challenge`: asks `atlas_server_remote_disposal_offered` again for itself;
reads `repo`, `decision`, `revision` (required, > 0), `intent` (three values), `token`;
builds a `CHALLENGE` op with `channel = REMOTE`, `remote_token`,
`remote_expected_key_id = ctx->gwpolicy.remote_dispose_key`, `remote_kinds =
ctx->gwpolicy.remote_dispose_kinds`; submits; writes the §Frozen response with no
`confirm`. `method_remote_dispose`: reads `repo`, `decision`, `intent`, `token`,
`challenge`, `confirmation`; builds the op kind the intent names; submits; writes the
§Frozen response including `actor_means`. Both log one safe-encoded line.

- [ ] **Step 1: Write the failing tests.** `tests/test_gw_dispose.c`, `daemon` label,
      serialised like the other daemon suites, starting `atlas-gw-daemon` with a policy
      file the test writes (`enabled = yes`, `gateway_uid = <getuid()>`, `remote_mcp =
      yes`, `web_gui = yes`, `listen_addr = 127.0.0.1`, `tls_mode = REVERSE_PROXY`, the
      two keys naming a `--no-scopes` key the test minted) and driving the daemon over
      its socket with `atlas_ipc_call`:
      (a) `gateway.auth` with the disposal key's token answers `authenticated: true`
      and `scopes` equal to exactly `decisions:dispose`; with the reader key's token the
      scopes are its stored list and nothing more; with the disposal key under a policy
      whose `tls_mode` is `NONE`-with-no-keys (a second daemon instance, or a policy
      file without the two keys) the scopes are empty.
      (b) `decision.remote_challenge` and `decision.remote_dispose` as the test's own
      uid: the full happy path for a `reject` on an `OPERATIONAL_FACT` (mint → the
      response has `token`, `content_hash`, `expires_at`, `key_id` and **no `confirm`
      key** → spend with the first eight hex of `content_hash` → `state: REJECTED`,
      `actor: REMOTE_OPERATOR_CONFIRMED`, `key_id`, `actor_means` containing "weaker
      than the local channel"); `decision.history` shows the event with `key_id`;
      `atlas doctor` reports the ledger agreeing; `fx_tree_digest` unchanged.
      (c) the same two names against a plain `fx_daemon_start` daemon: `unknown method`
      for both — and `tests/test_a7_authority.c:195-197` gains both names so its own
      assertion covers them too.
      (d) a second `atlas-gw-daemon` with a policy naming the keys but `tls_mode`
      absent and no acceptance is refused by the loader (the tool exits non-zero naming
      MALFORMED); with keys absent and `tls_mode = REVERSE_PROXY`, both names answer
      `unknown method`. **Amended 2026-09-04:** a third instance with the keys,
      `tls_mode = NONE` and `operator_accepts_cleartext_disposal = yes` offers both
      names and completes the (b) happy path unchanged, and `gateway.auth` derives the
      scope under it exactly as under `REVERSE_PROXY` — the one condition the
      acceptance adds is the gate's, and the write point is indifferent to transport.
      (e) `revision = 0`, a non-newest revision, a kind outside the list, `supersede`
      intent, a wrong confirmation, a replayed challenge: each refused with its frozen
      sentence, and the challenge table's consumed count moves only on the one success.
- [ ] **Step 2: Run and watch them fail** — no tool binary, no methods.
- [ ] **Step 3: Implement.** The opts field and the three-way branch at
      `daemon.c:222-226` (inject → system load → zero) with P0's comment shape; the tool
      on `atlas_watch_daemon.c`'s shape; `server_remote.c` with a head comment placing
      it beside `server_decision.c:2295-2330`'s A7 argument and saying why it is a
      separate file; the dispatcher's additive consult after the gateway group at
      `server.c:1293-1302`; the derivation in `method_gateway_auth` after `:122`.
      **Then amend the two comments this task makes false**, because they are exactly
      the class of sentence this plan hunts: `server_decision.c:13-14` ("`token` and
      `confirmation` are read by `decision.approve`, `decision.reject` and
      `decision.supersede` and by nothing else") and `:2551-2553` ("read here and in
      no other method"). The new property, stated in both places: `token` and
      `confirmation` are read in `spend_method` and in `src/ipc/server_remote.c`'s
      `method_remote_dispose`, and nowhere a model reaches — the first is offered only
      to the operator's uid, the second only to the gateway's uid behind TLS and a
      named credential.
- [ ] **Step 4: Run `test_gw_dispose`, `test_a7_authority`, `test_gw_remote`,
      `test_decision_mcp`**; `make` with zero warnings.
- [ ] **Step 5: Commit** — `feat(a16): the daemon offers two remote methods to the gateway's uid only behind TLS and a named credential, and derives the one scope nothing can be granted`

---

### Task T6: the gateway — the write table, the form body, the bearer-only principal

**Files:**
- Modify: `src/gw/gateway.c`, `include/atlas/gateway.h`, `include/atlas/limits.h`,
  `tests/test_gateway.c`, `tests/test_gw_remote.c`, `tests/test_gw_dispose.c`

**Interfaces produced:** §Frozen formats, "The gateway's write table", every gateway
refusal sentence, `/auth/me`'s `remote_disposal`, `atlas_gateway_api_write_routes`,
`ATLAS_GW_WRITE_BODY_MAX_BYTES`.

- [ ] **Step 1: Write the failing tests.**
      In `tests/test_gateway.c`, `test_every_write_route_is_a_disposal_on_the_reviewed_allowlist`:
      for every view row of the write table, `method` is on a positive `WRITE_METHODS[]`
      of exactly the two names; `scope == ATLAS_SCOPE_DECISIONS_DISPOSE`;
      `atlas_apikey_scope_grantable(scope)` is **false**; `method` is not on
      `READ_METHODS[]`, not in the operator group, not `gateway.auth`/`gateway.audit`;
      and the existing read-table test still passes unchanged (no read row gained a
      write scope). The count is the loop bound only.
      In `tests/test_gw_dispose.c` (HTTP half, through `atlas_gateway_serve_bytes`
      against the tool daemon, with a gateway opened on the same policy text):
      (a) the happy path from T5 (b) over HTTP: `POST /api/v1/decision/challenge` with
      the disposal key as bearer and the form body → 200, no `"confirm"` in the body;
      `POST /api/v1/decision/dispose` → 200, `actor` remote, `key_id`; two `gw_audit`
      rows with `operation` = the two paths, `key_id` = the selector, `interface =
      WEB_API`, and no token or confirmation bytes anywhere in the audit table.
      (b) each refusal with its sentence and status: `GET` → 405; `Content-Type:
      application/json` → 415; a 4097-byte body → 413; a gateway whose policy has no
      keys → 404; no `Authorization` header → 401 (with and without a *valid* session
      cookie from a real login with the reader key — **the cookie must not help**);
      the anonymous floor named in the policy plus a matching `Host` and no header →
      401; the reader key as bearer → 403 with the scope sentence; a body naming a
      parameter the row does not declare (`token=…`, `key_id=…`) → the parameter is
      dropped, never forwarded (assert through the daemon's refusal of a missing field
      or by a route that would otherwise have succeeded); a malformed percent-escape →
      400.
      (c) a cross-origin `OPTIONS` preflight for the write path from an unlisted
      origin → 403 (`gateway.c:1351-1366` unchanged); the write path with a listed
      origin still requires the bearer.
      (d) `/auth/me` reports `"remote_disposal": true` under the keyed policy and
      `false` under `gui_env`'s; `"cleartext_disposal": true` under a keyed policy
      carrying the acceptance and `false` under one with `tls_mode = REVERSE_PROXY`
      (amended 2026-09-04).
      (e) the disposal key over `/mcp`: `tools/list` returns an empty tool array, and
      `tools/call` for each of `atlas_decision_approve`, `atlas_approve_decision`,
      `atlas_decision_dispose`, `atlas_dispose`, `atlas_review_apply`,
      `atlas_decision_reject`, `atlas_decision_resolve`, `atlas_remote_dispose`,
      `decision.remote_dispose`, `decision.remote_challenge` answers `unknown tool` —
      `test_the_gateway_holds_no_credential_administration_verb`'s shape. In
      `tests/test_gw_remote.c`, extend `test_no_credential_can_reach_a_write_tool`
      (`:279`) so a principal whose mask carries `DECISIONS_DISPOSE` still reaches no
      write tool.
- [ ] **Step 2: Run and watch them fail.**
- [ ] **Step 3: Implement** `API_WRITE_ROUTES[]`, `api_handle_write` in the frozen
      order, the view accessor on `atlas_gateway_api_routes`'s shape, the `/auth/me`
      field, the 404 shape from `/mcp`'s. **Amend the comment at `gateway.c:1597-1600`**
      to say "for reads" and point at the write block, which says in its own words that
      it resolves a principal from the bearer header and from nothing else, and why.
      **Do not add an `Origin` check to the write handler.** A browser sends `Origin`
      on a same-origin `POST`, and this deployment lists no origin; a check against the
      list would refuse the page's own request. `atlas_http_origin_allowed` has exactly
      two consumers today — `respond` (`gateway.c:164`), which *adds* the CORS headers
      for a listed origin, and the `OPTIONS` preflight (`:1353`) — and neither refuses a
      request; `/auth/login` works from the page under `origins: 0 allowed` for that
      reason. The write routes' CSRF defence is the bearer header (Decision 1), not
      `Origin`; say so in the handler's comment.
- [ ] **Step 4: Run `test_gateway`, `test_gw_remote`, `test_gw_dispose`**; `make`.
- [ ] **Step 5: Commit** — `feat(a16): two POST routes whose body is query-string syntax and whose only credential is the bearer on the request`

---

### Task T7: Mission Control's disposal panel

**Files:**
- Modify: `src/gw/ui/mission-control.html`, `tests/test_gw_remote.c`

**What the panel is** (Decision 15, restated as the executor's checklist):

- A module-level `let disposeKey = "";` beside `scopes` (`:248`). Never written to
  `localStorage` or `sessionStorage` unless row 3 of the operator's decisions said
  `sessionStorage`, in which case the key `atlas.dispose.key.v1` and a `try/catch` on
  every access, exactly as the sheet's storage is wrapped. Cleared on `Sign out`.
- A second fetch helper, `apiWrite(path, fields)`, beside `api()` (`:271-296`): `POST`,
  `Content-Type: application/x-www-form-urlencoded`, body from `new
  URLSearchParams(fields)`, `Authorization: "Bearer " + disposeKey`, **`credentials:
  "omit"`** so no cookie travels with a disposal, and the same error mapping as `api()`
  except that a 401 renders "the disposal key was not accepted" in the panel rather
  than calling `showLogin()` — a disposal has no session to end.
- In `showReviewDetail` (`:907`), after the queue buttons: the **Dispose from this
  browser** block. When `me.remote_disposal` is false, the fourth frozen sentence and
  nothing else. Otherwise: the key field (`type="password"`, `autocomplete="off"`),
  the second frozen sentence beside it, and one `Dispose: <intent>` button per intent
  `reviewIntentsAllowed(row.revision_state, row.kind)` returns — note **the revision's
  own state** (`revision_state` from `decision.list`), not the document's status,
  because the write point keys on the revision (Decision 5; `docs/backlog.md:2384`).
- On click: `apiWrite("decision/challenge", {repo, decision, revision:
  row.latest_revision, intent})`; render the confirmation block — intent, repository,
  decision id, kind, revision, state, the full `content_hash` in a monospace element,
  `expires_at`, the first frozen sentence, the third frozen sentence, a text field of
  `maxlength="8"` and a `Confirm` button. `Confirm`: if the typed value is not the first
  eight characters of the displayed hash, render the CLI's mismatch sentence and stop
  (nothing is sent); else `apiWrite("decision/dispose", {repo, decision, intent,
  challenge: token, confirmation})`; render the daemon's answer as one result line —
  `state`, `actor`, `key_id`, and `actor_means` in full beneath it — and, if a sheet
  entry for this `(repo, decision)` exists, remove it through `reviewQueueRemove`
  **after** the fetch has resolved, never between a queue load and save.
- Everything `textContent`; no `innerHTML`; no new external resource; the CSP
  untouched. The first A15 sentence under the sheet stays as it is.

- [ ] **Step 1: Write the failing test** in `tests/test_gw_remote.c`
      (`test_mission_control_carries_the_disposal_panel`): fetch `/` and require the
      bindings `disposeKey`, `apiWrite`, `decision/challenge`, `decision/dispose`,
      `remote_disposal`, `cleartext_disposal`, `crosses the network in the clear`,
      `credentials: "omit"`, `application/x-www-form-urlencoded`,
      `maxlength="8"`, and the needles `names the channel and the credential, not a
      person`, `weaker than a terminal on the Atlas machine`, `A mismatch spends
      nothing`, `does not serve remote disposal`, and — per row 3's answer — exactly
      one of `held in this tab's memory only` or `remembered for this tab only`, with
      the other required absent;
      require the absence of `innerHTML`, of `localStorage.setItem(REVIEW_SHEET_KEY`
      inside any function that also contains `apiWrite(` (a coarse grep that the queue
      save and the disposal fetch are not in one function — state its coarseness in the
      comment), and of the bare phrase `proves`. Then drive the two routes through
      `atlas_gateway_serve_bytes` with a real bearer, as T6 (a) does, from this test's
      own fixture. **State in the test's header that no JavaScript is executed.**
- [ ] **Step 2: Run and watch it fail** on the first missing binding.
- [ ] **Step 3: Write the panel.** Reuse `el`, `kv`, `tag`, `statusTag`, `kindTag`,
      `panel`, `say`; add nothing that duplicates them.
- [ ] **Step 4: Run the test**; `make` (the page is re-embedded by the custom command).
- [ ] **Step 5: Commit** — `feat(a16): Mission Control disposes of a record from the browser, with the digest typed and the channel's weakness on screen`

---

### Task T8: the tripwires learn the new surfaces

**Files:**
- Modify: `tests/test_decision_mcp.c`, `tests/test_a7_authority.c` (if T5 did not
  already), `tests/test_review_apply.c` (comment only)

- [ ] **Step 1: Extend `FILES[]`** (`test_decision_mcp.c:391-414`) with
      `src/ipc/server_remote.c`, `src/decision/remote.c`,
      `include/atlas/decision_remote.h`, `docs/browser-disposal.md`. Run the test: a
      forbidden phrase found in any of them is a finding, and the phrase is removed
      from the source, never from the list.
- [ ] **Step 2: Extend the required-wording table** (`:433-455`) with
      `{mission-control.html, "names the channel and the credential, not a person"}`,
      `{mission-control.html, "weaker than a terminal on the atlas machine"}`,
      `{mission-control.html, <row 3's sentence: "held in this tab's memory only" or
      "remembered for this tab only">}` — one needle, chosen by the operator's answer,
      never both,
      `{docs/browser-disposal.md, "weaker than the local channel by construction"}`,
      `{docs/decision-lifecycle.md, "weaker than the local channel by construction"}`,
      `{SECURITY.md, "weaker than the local channel by construction"}`,
      `{src/ipc/server_remote.c, "weaker than the local channel by construction"}` (the
      `actor_means` string), `{include/atlas/decision.h, "remote_operator_confirmed"}`
      (lower-cased by the reader), and — amended 2026-09-04, the cleartext chain —
      `{SECURITY.md, "travels in the clear"}`, `{docs/remote-access.md, "travels in the
      clear"}`, `{docs/browser-disposal.md, "travels in the clear"}` and
      `{mission-control.html, "crosses the network in the clear"}`. The scan lower-cases every file
      (`read_lowercased`), so needles are lower-case.
- [ ] **Step 3: The caller-count scans.** `test_the_single_write_point_has_exactly_three_callers`
      (`:518-556`) must still count three files; add to its comment that A16 came here
      and argued for **zero** new callers — the remote verification is a call inside the
      write point, and the remote methods submit through the writer — so the count did
      not move. `tests/test_review_apply.c:1439-1470`'s two-caller assertion is
      unchanged; add one sentence to its comment saying A16 added no caller and why.
- [ ] **Step 4: Run `test_decision_mcp`, `test_review_apply`, `test_a7_authority`,
      `test_plugin`** and watch them pass.
- [ ] **Step 5: Commit** — `test(a16): the page, the two new files and the season's document join the tripwire, and the write point still has three callers`

---

### Task T9: documentation, the season rules, the roadmap's ordering

**Files:**
- Create: `docs/browser-disposal.md`
- Modify: `docs/roadmap.md`, `docs/remote-access.md`, `docs/decision-lifecycle.md`,
  `docs/review-surface.md`, `docs/engineering-rules.md`, `docs/extending.md`,
  `docs/backlog.md`, `SECURITY.md`, `README.md`, `CLAUDE.md`,
  `deploy/a9/gateway.conf.template` (if T4 did not already)

- [ ] **Step 1: `docs/browser-disposal.md`** — the season's document in
      `docs/review-surface.md`'s shape: the sentence; **the honest paragraph from the
      top of this plan, in the paragraph that announces the capability**; what is true
      today in the precise form (the write table's two rows, the method group's three
      conditions, the policy's two keys, the ledger's new column); the seventeen
      decisions with their chains; the frozen formats by reference to this plan and to
      the headers; the stated costs (the credential passes through two processes Atlas
      does not verify; a holder of it disposes as the operator does; revocation is the
      bound; the page executes no JavaScript under test; a page reload asks for the
      key again unless row 3 said otherwise; and — amended 2026-09-04 — **the
      cleartext chain verbatim**, in the paragraph that announces the capability, with
      the operator's answer quoted and dated); and, at the end, what execution
      established that the plan did not claim.
- [ ] **Step 2: `docs/roadmap.md`** — retitle `:1558` "Next: A15 …" to "A15 — … (shipped)"
      and fold its closing paragraph into one sentence naming `docs/review-surface.md`;
      add "Next: A16 — browser disposal, and what the remote channel is worth" above
      "Invariants that outlive every phase", carrying the sentence, the honest
      paragraph, the seventeen decisions in one line each, and the pointer. Leave
      `:1478` "Later: A14" untouched. Every sentence against the tripwire list.
- [ ] **Step 3: `docs/remote-access.md`** — a new section "A16: the remote operator
      channel" after the A15 section (`:317-356`): the two routes, the form body and
      why not JSON, the bearer-only principal and why a cookie cannot dispose, the
      derived scope, the two policy keys with their MALFORMED conditions, the `dispose:`
      status line, `/auth/me`'s new field, and the honest paragraph. **Amend the A15
      section's last paragraph** (`:340-356`), which says disposal "stayed on the one
      channel that never runs through this listener": it is now true of the *session*
      and the floor, and false of a bearer the policy names — say so rather than
      leaving a sentence that was true on 2026-09-04 and is not after this season.
      Amend "TLS" (`:77-91`) with `operator_accepts_cleartext_disposal`: what it is,
      its three MALFORMED conditions, that `REVERSE_PROXY` remains the shape the gate
      prefers, and **the cleartext chain verbatim** (§Frozen formats) — amended
      2026-09-04. Amend the first paragraph's "authenticates a bearer credential"
      caveat (`:4-9`) to add that the write routes authenticate nothing else.
- [ ] **Step 4: `docs/decision-lifecycle.md`** — beside "The operator channel,
      mechanically" (`:624-662`), a section "The remote operator channel, mechanically":
      the seven steps in the same diagram shape (bearer → gateway.auth → remote
      challenge for the newest revision → the digest on screen → the typed eight → the
      spend that re-verifies the credential, the channel, the newest revision and the
      hash → the ledger row with actor and key id); the honest paragraph; the sixth
      actor in "The claim, and the non-claim" (`:16-90`) with its own bullet list of
      what it does not establish; the two channel-mismatch refusals under "What the
      channel does exclude" (`:68-90`); in the `--revision N` section (`:837-869`), one
      paragraph saying the remote channel mints only for the newest revision and why the
      local semantics are unchanged; and the corrected supersession sentences beside
      the `op_approve` paragraph.
- [ ] **Step 5: `docs/review-surface.md`** — one paragraph at the head of §3 ("Tier 3,
      costed and not built"): built in A16, see `docs/browser-disposal.md`; and one line
      in §7's "ssh, or nothing" cost saying the cost was widened by A16 and how. Do not
      rewrite the rest: it is the record of what A15 chose and why.
- [ ] **Step 6: `docs/engineering-rules.md`** — "A16 layers — additions" and "A16 rules
      — these are not negotiable" in full, from §Authority argument, in the A15
      section's register (`:3643-3760`). **`docs/extending.md`** — "A16 — the remote
      operator channel": adding a write route (the positive allowlist, exactly one
      scope, the bearer-only principal, the form body — and why a read row must never
      move into this table); adding a channel (both name/parse mappings, no
      `default:`, a migration for two CHECKs, the actor selection in three ops, the
      mismatch sentences); adding a remotely-disposable intent (why supersede and
      revalidate are not, one channel over from A15's entry); adding a gateway policy
      key that names a credential (the `key_` shape, MALFORMED conditions, the daemon
      restart); changing `ATLAS_GW_WRITE_BODY_MAX_BYTES`; and "Bounds this season
      added".
- [ ] **Step 7: `docs/backlog.md`** — close the `op_approve` entry (`:2197`) naming the
      commit; amend "Two costs tier 1 leaves" (`:2260`): cost 2 is closed by A16 for the
      kinds the policy names, cost 1 (no cross-device queue) stands and now matters
      less; amend "Tier 3 … costed" (`:2280`): built, with the three A15 costs and the
      four this plan found beyond them (a key with no scopes needs a deliberate form;
      the ledger must carry the credential because challenges are pruned; the fixture
      daemon zeroes its policy; a reverse proxy, if one is ever installed, changes
      what `Host` the anonymous floor sees); a new entry for the residuals this season
      leaves (§Self-review, 5); and — amended 2026-09-04 — an entry recording that the
      TLS requirement became the acceptance key on the operator's decision, with the
      chain, so a later reader of the backlog finds the departure where they look for
      open costs.
- [ ] **Step 8: `SECURITY.md`** — "A16: the remote operator channel, and what it is
      worth" after the A15 section (`:453-470`): what it establishes (the credential the
      policy names was presented over the transport the policy declares, verified by
      the daemon in the transaction; the newest revision; the typed digest), what it
      does not establish (which person; that a person; non-repudiation; that the proxy
      terminates TLS at all — Atlas cannot verify it), what it excludes (a session, the
      floor, any MCP tool, any key with a stored scope, any kind the policy does not
      name), the honest paragraph, and — amended 2026-09-04 — **the cleartext chain
      verbatim** in the paragraph that announces the capability, followed by what the
      acceptance key is and that `atlas gateway status` prints it. **The chain's hedge
      — "anyone able to observe traffic on that network segment" — is deliberate and
      must not be sharpened to "anyone on the network" in any copy**: this plan
      verified the listener and the transport, not whether the operator's LAN is
      switched or shared, and a sentence claiming more than was verified is the class
      of sentence this project scans for. `README.md` — the
      three policy keys in the gateway section, `api-key create --no-scopes` in the
      usage list, and one sentence under Mission Control.
- [ ] **Step 9: `CLAUDE.md`** — the season paragraph at the top in the register the
      others use ("The current work is **A16** … **A16 added migration 31.**" and the
      sentence), the table row, the one-line rules under "### A16 — browser disposal"
      — including one line for the acceptance key: TLS in front is the intended shape;
      `operator_accepts_cleartext_disposal = yes` is the operator's written departure
      from it, printed by `gateway status`, and on this deployment the credential
      travels in the clear — the honest paragraph in the season paragraph itself, and
      `docs/browser-disposal.md` in "Where things are documented". Every line against
      `test_decision_mcp.c:371-386`.
- [ ] **Step 10: Run `test_decision_mcp`** and the full suite.
- [ ] **Step 11: Commit** — `docs(a16): browser disposal -- what was built, what the remote channel is worth, and what it cost`

---

### Task T10 as committed at `c305f40` — the nginx terminator — was removed on 2026-09-04

It installed nginx on `192.168.0.198:8799` with a self-signed SAN-IP certificate,
moved the gateway to `127.0.0.1:8787` under `tls_mode = REVERSE_PROXY`, re-pointed the
remote MCP tunnel, and verified each step. It is gone because the operator declined
TLS on their own network (§The decision, "Amended 2026-09-04"), not because it was
forgotten; its deployment steps are recoverable from `c305f40` if a later deployment
wants a terminator, and nothing in the code refuses one — `REVERSE_PROXY` is still the
shape the gate prefers. What it carried that still has to happen — deploying the
binary, minting the credential, editing the policy, restarting both services — is now
the first two steps of the task below.

---

### Task T10: live acceptance — the credential, the policy, and two probes disposed of from the browser

Run by the reviewing session with the operator acting. **It spends no money**; it
changes this machine's policy and disposes of two records.

- [ ] **Step 1: Full suite, then deploy the binary** with `/opt/atlas/deploy.local.sh`
      after changing its `MARK` to a string only A16's binary contains
      (`REMOTE_OPERATOR_CONFIRMED`). The daemon restarts; `atlas daemon ping --json`
      reports `phase: "A16"` once `include/atlas/atlas.h:11` says so (the chore commit,
      as `4881a5a` did for A15).
- [ ] **Step 2: The credential and the policy.** `atlas api-key create --label
      browser-dispose --no-scopes` as the operator; the secret is shown once and goes
      into the operator's password manager, never into a file on this machine. Edit
      `/etc/atlas/gateway.conf` — `listen_addr`, `listen_port` and `tls_mode = NONE`
      unchanged — adding `remote_dispose_key = key_…`, `remote_dispose_kinds` from row
      2's answer, and `operator_accepts_cleartext_disposal = yes`. Restart **both**
      `atlas.service` and `atlas-gateway.service` (the daemon reads this file at
      start, `daemon.c:222`). `atlas gateway status` reads `tls: NONE`, `dispose:
      key_… (…)` and `clear: ACCEPTED -- operator_accepts_cleartext_disposal = yes …`;
      record the three lines as the first observation. The browser still reaches
      `http://192.168.0.198:8799/`; nothing about the listener moved.
- [ ] **Step 3: Read.** The operator opens Mission Control on the phone, Review view,
      selects the `atlas` repository, `PROPOSED`. Both "PROBE-A8FINAL-… disposable"
      records are listed. **Their kind is `DECISION`** — if row 2's answer did not
      include `DECISION`, the panel's `reject` is refused with the kinds sentence, and
      that refusal is the first observation to record; the operator then either widens
      the policy for the acceptance or disposes of the probes with `atlas review apply`
      and the acceptance is run on a fresh `OPERATIONAL_FACT` proposed for the purpose.
      The panel shows the cleartext sentence (the fifth frozen sentence) because
      `/auth/me` reports `cleartext_disposal: true`; record that it did.
- [ ] **Step 4: Dispose.** Paste the disposal key; `Dispose: reject` on
      `atlas-dec-28f03b…` r1; read the confirmation block; type `6fb2be08`; `Confirm`;
      expect `REJECTED`, `REMOTE_OPERATOR_CONFIRMED`, the key id. Repeat for
      `atlas-dec-c711a6…` r3 with `5146bbb3`. Then a deliberate mistype on a third
      record (propose one for the purpose): expect the mismatch sentence and an
      unconsumed challenge.
- [ ] **Step 5: Verify on the machine.** `atlas decision history atlas atlas-dec-28f03b…`
      shows `REJECTED` by `REMOTE_OPERATOR_CONFIRMED`, `credential: key_…`;
      `atlas doctor` reports the ledger agreeing; the Audit view shows two `WEB_API`
      rows per disposal with the key id and no token; `journalctl -u atlas` shows the
      two daemon lines per disposal.
- [ ] **Step 6: Record the observations** in `docs/browser-disposal.md` — as
      observations, with the date, the device, and anything the page did that the grep
      test could not have seen. **Nothing about a live pass is a general result; say so
      in those words.**
- [ ] **Step 7: Final commits. Nothing is pushed on this document's authority.** Present
      the season's commit list, ask, and push only on the operator's contemporaneous
      go-ahead — recording the answer either way.

---

# Acceptance — the brief's requirements, mapped

| # | Requirement | Discharged by | The assertion that proves it |
| --- | --- | --- | --- |
| 1 | its own channel identity; `LOCAL_OPERATOR_CONFIRMED` never reused; migration 31 on migration 15's pattern; a channel column on challenges | T1, T2, T3 | the actor round-trips and is adapter-unwritable; migration 31 preserves every row and widens both CHECKs; a `REMOTE` challenge spent locally and a `LOCAL` one spent remotely are both refused; the ledger row reads `REMOTE_OPERATOR_CONFIRMED` |
| 2 | its own scope, ungrantable, derived from a root-owned line, never stored | T1, T4, T5 | `grantable == false`; `api-key create --scope decisions:dispose` refused; `gateway.auth` derives it for the named key only, and only when its stored list is empty; no `api_keys` row ever holds it |
| 3 | absent from the MCP surface | T6 | the disposal key lists zero tools and every plausible name is `unknown tool`; a mask carrying the bit reaches no write tool |
| 4 | TLS in front is the intended shape; this machine's departure from it is written, printed and stated — never quiet (amended 2026-09-04) | §The decision, T4, T5, T6, T7, T9, T10 | keys under `tls_mode = NONE` are MALFORMED without `operator_accepts_cleartext_disposal = yes`, and MALFORMED with it under `REVERSE_PROXY`; the group answers `unknown method` without `REVERSE_PROXY` and without the acceptance, and is offered with either; `gateway status` prints `clear:` in both forms; `/auth/me` reports `cleartext_disposal`; the page shows the fifth sentence; the cleartext chain is verbatim in three documents; the operator's answer is recorded in this plan |
| 5 | replay protection bound to the content hash; something typed | T3, T5, T7 | the challenge response has no `confirm`; the spend compares the typed eight against the stored hash; a mismatch consumes nothing; `maxlength="8"` and the third frozen sentence are in the page |
| 6 | a root-owned policy naming kinds, refused rather than clamped | T3, T4 | every MALFORMED case in the matrix; a kind outside the list refused at mint and at spend |
| 7 | the daemon authenticates the bearer itself in the transaction | T3, T5 | a wrong secret, a revoked key, a key with a stored scope and a key not named by the policy are each refused inside the write point; a session cookie on the write route is 401 |
| 8 | `api_handle` refuses non-GET; a second table | T6 | the write-table property test; GET on a write route is 405; the read table's test unchanged |
| 9 | no yyjson in `src/gw`, argued | Decision 7, T6 | the body is form syntax parsed by `build_api_params`; `grep -c yyjson src/gw/*.c` is 0 |
| 10 | the anonymous floor can never hold the disposal scope, enforced | T4, T6 | `web_gui_anonymous_scopes = decisions:dispose` is MALFORMED; the floor with a matching `Host` and no header is 401 on the write route |
| 11 | weaker by construction, said where the capability is announced | T7, T8, T9 | the required-wording needles in the page, `docs/browser-disposal.md`, `docs/decision-lifecycle.md`, `SECURITY.md` and the `actor_means` string |
| 12 | nothing claims a person acted; the scan passes | T8 | `FILES[]` extended; `test_decision_mcp` passes over every new file and document |
| 13 | the lifecycle gap planned around | T3 | a remote challenge for a non-newest revision is refused; a revision landing in the window refuses the spend; the local `--revision N` test is unchanged |
| 14 | the two A15 defects decided | T3, §The decision | the supersession sentence is correct in both orders (one assertion each); the three dead routes are recorded as not this season's |
| 15 | caller counts stated and held | T3, T8 | three files name the write point; three name the confirm helper (one definition, two callers); both scans pass with the two new files present |
| 16 | the operator asked before, not after | §Decisions the operator must be asked | row 1 asked and answered no on 2026-09-04, recorded in the plan; rows 2 and 3 answered before T1 is dispatched; every answer recorded in `docs/browser-disposal.md` |
| 17 | one real disposal from the browser, on this deployment as it is | T10 | the policy carries the three keys and `gateway status` prints `clear: ACCEPTED`; two probe records `REJECTED` through the panel with the cleartext sentence on screen; observations recorded as observations |

---

# Self-review (planner)

**1. Spec coverage.** The six non-negotiables are Decisions 3, 2, 11/T6, §The decision,
8 and 6; the three A15 found are Decision 1 (the bearer verified in the transaction) and Decision 7 twice (the second table, and the form body in place of JSON); the two honesty requirements are
the honest paragraph and T8; the anonymous-floor interaction is Decision 10 and row 10
of the acceptance table; the lifecycle gap is Decision 5; the two defects are Decision
12; the caller counts are Decision 3's last paragraph; the numbering is stated; the
operator's decisions are in a section of their own with an "ask before" column.

**2. Sentences of the recurring defect class, hunted.** Every line reference was read
at `acbd7ad` this session and is paired with a function name. Three things A15's cost
list said were unread are now read and answered (the replay reads no actor; the
challenge table needs a channel *and* the ledger needs the credential; the fixture
daemon zeroes its policy). Four things this plan found that A15's list did not
itemise are named in T9 step 7. Two things this plan deliberately does not claim,
because nothing read establishes them: whether the remote MCP tunnel on this machine
can be re-pointed without a change on its far side (row 1 asks), and whether the
operator's phone accepts a self-signed SAN-IP certificate as trusted (T10 step 6
observes it).

**3. Placeholder scan.** No TBDs. Every vocabulary member, policy key, route row,
request shape, refusal sentence, ledger sentence, UI sentence, status line, migration
statement name and bound is written out. Deliberately left to the executor and named
as such: C and JavaScript bodies; whether `atlas_gateway_api_write_routes` is a static
copy or a one-time fill (a location, not a design); how `atlas-gw-daemon` reads its
policy file (any bounded read; it is a test tool).

**4. Type consistency.** `atlas_decision_channel` is produced by three `op_new` sites
and consumed by `op_challenge`, `spend_challenge` and three actor selections; the
challenge struct's `channel`/`key_id` are written by T2's insert and read by T2's find
and T3's spend; `atlas_decision_result.actor` is set in three ops and read by
`spend_method` and `method_remote_dispose`; `atlas_gwpolicy.remote_dispose_key` is
written by T4's parser and read by T5's derivation, T5's predicate, T6's 404 and T4's
status line; `remote_kinds` on the op is copied from the same field the predicate
checks; `ATLAS_GW_WRITE_BODY_MAX_BYTES` is the 413 bound in T6 and the 4097-byte case
in its test; the two frozen route rows are what T6 writes and T7 drives.

**5. What this plan does not settle, and says so (T9 step 7 records each).** A
cross-device queue (the sheet still lives in one browser; it matters less now that the
browser can dispose); a disposal key that expires (A9 keys have no TTL, and adding one
is A9's change, not this season's); `supersede` and `revalidate` from the browser; a
per-credential rate bound below the gateway's global one; a policy key naming a
reverse proxy's hostname for the anonymous floor (`gateway.c:527-543` deliberately
left it out, and this plan agrees); a browser test that executes the page; and whether
the local walker's `required_status_for` should move onto the revision's state
(`docs/backlog.md:2384`, unchanged by this season).

**6. Amended 2026-09-04, after commit `c305f40`.** The plan's gating question was
asked and answered no. The amendment is recorded in five places so it cannot be read
as an omission: the honest paragraph at the top, §The decision, Decision 6, Decision
17, the authority argument's TLS bullet, and the note standing where the nginx task
was. What the amendment changed: one policy key, one condition on the gate, one
`clear:` line in `gateway status`, one `/auth/me` field, one sentence on the page,
one verbatim chain in three documents, the removal of one task (eleven became ten),
and rows 2 and 3 of the operator's decisions moved to before T1. What it did not
change: every other decision, every frozen format not named above, every caller
count, and every test obligation except the additions marked "amended".
